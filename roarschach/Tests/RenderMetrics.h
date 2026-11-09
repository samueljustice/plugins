#pragma once

// Offline measurements for the render harness (RoarschachTests --render ... --metrics / --sweep).
// Test code only: nothing here ships in the plug-in. Every measure is a plain, documented proxy,
// meant for comparing renders of the same input against each other (a parameter sweep), not
// for absolute claims.
//
//   loudness        BS.1770-4 integrated loudness (K weighting at any rate, 400 ms blocks,
//                   -70 LUFS absolute and -10 LU relative gates) and EBU Tech 3342 loudness
//                   range (3 s short-term, 100 ms hop, -20 LU relative gate, 10th..95th pct)
//   pumping         std dev (LU) of the momentary-loudness difference out - in over the input's
//                   gated blocks: level movement the input did not have
//   spectrum        Welch long-term power spectrum (Hann, ~3 Hz bins, 50 % overlap), averaged
//                   over L and R, as 1/3-octave bands 20 Hz..20 kHz in dB re full-scale mean
//                   square (a full-scale sine reads -3 dB)
//   harmonicity     per 75 ms frame (20 ms hop) normalised autocorrelation peak r for 40..600 Hz
//                   (window-corrected), HNR = 10 log10 (r / (1 - r)) averaged over the frames
//                   where the INPUT is voiced (input r > 0.6, within 40 dB of its loudest frame)
//   above 8 kHz     share of the energy above 8 kHz in those voiced frames (dB): a voice has
//                   little there, so a jump is fizz, aliasing or inharmonic distortion
//   modulation      envelope (power, 250 Hz low-pass, 1 kHz rate) of 1 s segments, normalised
//                   to its mean: the depth m of a sinusoidal amplitude modulation reads as m.
//                   Peaks 1..150 Hz and RMS depth per octave band. Peaks near 2 f0 of a very
//                   low voice are the pitch itself, not a tremolo.
//   intelligibility Pearson correlation of the 1..4 kHz envelopes (dB, 40 dB floor, 200 Hz
//                   rate) of input and output, best lag within +-30 ms
//   THD             only for a pure-sine input: harmonics 2..10 of the output's strongest line
//                   (PITCH may move it) against that line; plusNoiseDb is everything else

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <numeric>
#include <vector>

namespace RenderMetrics
{
    using Sig = std::vector<float>;
    constexpr double pi = 3.14159265358979323846;

    inline double db10(double p) { return 10.0 * std::log10(std::max(p, 1.0e-20)); }
    inline double db20(double a) { return 20.0 * std::log10(std::max(a, 1.0e-10)); }

    //==========================================================================
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process(double x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    inline Biquad lowPass(double fc, double sr, double q = 0.70710678)
    {
        const double w = 2.0 * pi * std::min(fc, 0.49 * sr) / sr, cw = std::cos(w), alpha = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        Biquad b;
        b.b0 = (1.0 - cw) / 2.0 / a0; b.b1 = (1.0 - cw) / a0; b.b2 = b.b0;
        b.a1 = -2.0 * cw / a0; b.a2 = (1.0 - alpha) / a0;
        return b;
    }

    inline Biquad highPass(double fc, double sr, double q = 0.70710678)
    {
        const double w = 2.0 * pi * std::min(fc, 0.49 * sr) / sr, cw = std::cos(w), alpha = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        Biquad b;
        b.b0 = (1.0 + cw) / 2.0 / a0; b.b1 = -(1.0 + cw) / a0; b.b2 = b.b0;
        b.a1 = -2.0 * cw / a0; b.a2 = (1.0 - alpha) / a0;
        return b;
    }

    //==========================================================================
    inline juce::dsp::FFT& fftFor(int order)
    {
        static std::map<int, std::unique_ptr<juce::dsp::FFT>> cache;
        auto& f = cache[order];
        if (f == nullptr)
            f = std::make_unique<juce::dsp::FFT>(order);
        return *f;
    }

    inline std::vector<float> hann(int len)
    {
        std::vector<float> w(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i)
            w[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * (i + 0.5) / len));
        return w;
    }

    /** |X|^2 of the windowed frame x[0..len) zero-padded to 2^order: bins 0..N/2 into `power`. */
    inline void powerSpectrum(const float* x, int len, const std::vector<float>& window, int order,
                              std::vector<float>& work, std::vector<double>& power)
    {
        const int n = 1 << order;
        work.assign(static_cast<size_t>(2 * n), 0.0f);
        for (int i = 0; i < len && i < n; ++i)
            work[static_cast<size_t>(i)] = x[i] * window[static_cast<size_t>(i)];
        fftFor(order).performRealOnlyForwardTransform(work.data(), true);
        power.resize(static_cast<size_t>(n / 2 + 1));
        for (int k = 0; k <= n / 2; ++k)
        {
            const double re = work[static_cast<size_t>(2 * k)], im = work[static_cast<size_t>(2 * k + 1)];
            power[static_cast<size_t>(k)] = re * re + im * im;
        }
    }

    //==========================================================================
    // Loudness.

    inline Sig kWeightedSquares(const Sig& l, const Sig& r, double sr)
    {
        // BS.1770 pre-filter and RLB high-pass, derived for any rate (the libebur128 form).
        Biquad shelf, hp;
        {
            const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
            const double k = std::tan(pi * f0 / sr), vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416);
            const double a0 = 1.0 + k / q + k * k;
            shelf.b0 = (vh + vb * k / q + k * k) / a0; shelf.b1 = 2.0 * (k * k - vh) / a0; shelf.b2 = (vh - vb * k / q + k * k) / a0;
            shelf.a1 = 2.0 * (k * k - 1.0) / a0; shelf.a2 = (1.0 - k / q + k * k) / a0;
        }
        {
            const double f0 = 38.13547087602444, q = 0.5003270373238773;
            const double k = std::tan(pi * f0 / sr), a0 = 1.0 + k / q + k * k;
            hp.b0 = 1.0; hp.b1 = -2.0; hp.b2 = 1.0;
            hp.a1 = 2.0 * (k * k - 1.0) / a0; hp.a2 = (1.0 - k / q + k * k) / a0;
        }
        Sig z(l.size(), 0.0f);
        for (const auto* ch : { &l, &r })
        {
            Biquad s = shelf, h = hp;
            for (size_t i = 0; i < ch->size(); ++i)
            {
                const double y = h.process(s.process((*ch)[i]));
                z[i] += static_cast<float>(y * y);
            }
        }
        return z;
    }

    struct Loudness
    {
        double integrated = -200.0, range = 0.0, momentaryStd = 0.0, shortTermStd = 0.0;
        std::vector<double> momentary;   // LUFS per 400 ms block, 100 ms hop
    };

    inline double lufs(double meanSquare) { return -0.691 + db10(meanSquare); }

    inline double powerMean(const std::vector<double>& l)
    {
        double s = 0.0;
        for (double x : l)
            s += std::pow(10.0, (x + 0.691) / 10.0);
        return l.empty() ? -200.0 : lufs(s / static_cast<double>(l.size()));
    }

    inline double stdDev(const std::vector<double>& v)
    {
        if (v.size() < 2)
            return 0.0;
        const double m = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
        double s = 0.0;
        for (double x : v)
            s += (x - m) * (x - m);
        return std::sqrt(s / static_cast<double>(v.size() - 1));
    }

    inline Loudness loudness(const Sig& l, const Sig& r, double sr)
    {
        Loudness out;
        const auto z = kWeightedSquares(l, r, sr);
        const int hop = static_cast<int>(std::lround(0.1 * sr));
        const int segments = static_cast<int>(z.size()) / hop;
        std::vector<double> seg(static_cast<size_t>(segments));
        for (int j = 0; j < segments; ++j)
        {
            double s = 0.0;
            for (int i = 0; i < hop; ++i)
                s += z[static_cast<size_t>(j * hop + i)];
            seg[static_cast<size_t>(j)] = s / hop;
        }
        const auto blocks = [&](int len)
        {
            std::vector<double> b;
            for (int j = 0; j + len <= segments; ++j)
            {
                double s = 0.0;
                for (int k = 0; k < len; ++k)
                    s += seg[static_cast<size_t>(j + k)];
                b.push_back(lufs(s / len));
            }
            return b;
        };
        out.momentary = blocks(4);
        const auto shortTerm = blocks(30);

        const auto gated = [](const std::vector<double>& b, double rel)
        {
            std::vector<double> abs;
            for (double x : b)
                if (x > -70.0)
                    abs.push_back(x);
            const double gate = powerMean(abs) + rel;
            std::vector<double> kept;
            for (double x : abs)
                if (x > gate)
                    kept.push_back(x);
            return kept;
        };
        const auto m = gated(out.momentary, -10.0);
        out.integrated = powerMean(m);
        out.momentaryStd = stdDev(m);
        auto st = gated(shortTerm, -20.0);
        out.shortTermStd = stdDev(st);
        if (st.size() >= 2)
        {
            std::sort(st.begin(), st.end());
            const auto pct = [&](double p) { return st[static_cast<size_t>(std::lround(p * static_cast<double>(st.size() - 1)))]; };
            out.range = pct(0.95) - pct(0.10);
        }
        return out;
    }

    //==========================================================================
    // Long-term spectrum.

    struct Spectrum
    {
        double binHz = 0.0;
        std::vector<double> ms;   // mean square per one-sided bin (a sine of amplitude A sums to A^2 / 2)
    };

    inline Spectrum welch(const Sig& l, const Sig& r, double sr)
    {
        int order = 10;
        while ((1 << order) < sr / 3.0)
            ++order;
        const int n = 1 << order, hop = n / 2;
        const auto w = hann(n);
        double sumW2 = 0.0;
        for (float x : w)
            sumW2 += static_cast<double>(x) * x;

        Spectrum s;
        s.binHz = sr / n;
        s.ms.assign(static_cast<size_t>(n / 2 + 1), 0.0);
        std::vector<float> work;
        std::vector<double> p;
        int frames = 0;
        const int len = static_cast<int>(l.size());
        for (const auto* ch : { &l, &r })
            for (int start = 0; start == 0 || start + n <= len; start += hop)
            {
                Sig frame(static_cast<size_t>(n), 0.0f);
                std::copy(ch->begin() + start, ch->begin() + std::min(len, start + n), frame.begin());
                powerSpectrum(frame.data(), n, w, order, work, p);
                for (size_t k = 0; k < p.size(); ++k)
                    s.ms[k] += p[k];
                ++frames;
                if (start + n >= len)
                    break;
            }
        const double scale = 2.0 / (static_cast<double>(n) * sumW2 * std::max(1, frames));
        for (auto& x : s.ms)
            x *= scale;
        return s;
    }

    inline double band(const Spectrum& s, double lo, double hi)
    {
        double sum = 0.0;
        for (size_t k = 1; k < s.ms.size(); ++k)
        {
            const double f = static_cast<double>(k) * s.binHz;
            if (f >= lo && f < hi)
                sum += s.ms[k];
        }
        return sum;
    }

    inline std::vector<double> thirdOctaveCentres()
    {
        std::vector<double> c;
        for (int k = 13; k <= 43; ++k)   // 20 Hz .. 20 kHz (base-10 centres)
            c.push_back(std::pow(10.0, k / 10.0));
        return c;
    }

    //==========================================================================
    // Frame analysis: harmonicity and energy above 8 kHz.

    struct Frames
    {
        std::vector<double> r, energy, above8k;
    };

    inline Frames analyseFrames(const Sig& x, double sr)
    {
        const int len = static_cast<int>(std::lround(0.075 * sr)), hop = static_cast<int>(std::lround(0.02 * sr));
        int order = 8;
        while ((1 << order) < 2 * len)
            ++order;
        const int n = 1 << order;
        const auto w = hann(len);
        const int minLag = static_cast<int>(sr / 600.0), maxLag = std::min(len / 2, static_cast<int>(sr / 40.0));
        const int bin8k = static_cast<int>(std::ceil(8000.0 * n / sr));

        // The window's own autocorrelation (Boersma's correction).
        std::vector<float> work;
        std::vector<double> p;
        const auto autocorr = [&](const std::vector<double>& power)
        {
            work.assign(static_cast<size_t>(2 * n), 0.0f);
            for (int k = 0; k < n; ++k)
                work[static_cast<size_t>(k)] = static_cast<float>(power[static_cast<size_t>(k <= n / 2 ? k : n - k)]);
            fftFor(order).performRealOnlyForwardTransform(work.data(), false);
            std::vector<double> ac(static_cast<size_t>(maxLag + 1));
            for (int t = 0; t <= maxLag; ++t)
                ac[static_cast<size_t>(t)] = work[static_cast<size_t>(2 * t)];
            return ac;
        };
        Sig ones(static_cast<size_t>(len), 1.0f);
        powerSpectrum(ones.data(), len, w, order, work, p);
        const auto acw = autocorr(p);

        Frames f;
        for (int start = 0; start + len <= static_cast<int>(x.size()); start += hop)
        {
            powerSpectrum(x.data() + start, len, w, order, work, p);
            double total = 0.0, high = 0.0;
            for (int k = 0; k <= n / 2; ++k)
            {
                total += p[static_cast<size_t>(k)];
                if (k >= bin8k)
                    high += p[static_cast<size_t>(k)];
            }
            double e = 0.0;
            for (int i = 0; i < len; ++i)
                e += static_cast<double>(x[static_cast<size_t>(start + i)]) * x[static_cast<size_t>(start + i)];
            double best = 0.0;
            if (total > 1.0e-18)
            {
                const auto ac = autocorr(p);
                // Search only past the zero-lag lobe (the first zero crossing), so plain low-pass
                // content does not read as periodic.
                int from = 1;
                while (from <= maxLag && ac[static_cast<size_t>(from)] > 0.0)
                    ++from;
                for (int t = std::max(minLag, from); t <= maxLag; ++t)
                {
                    const double v = (ac[static_cast<size_t>(t)] / ac[0]) / (acw[static_cast<size_t>(t)] / acw[0]);
                    if (v > best)
                        best = v;
                }
            }
            f.r.push_back(std::clamp(best, 0.0, 1.0));
            f.energy.push_back(e);
            f.above8k.push_back(total > 0.0 ? high / total : 0.0);
        }
        return f;
    }

    //==========================================================================
    // Envelopes.

    /** sqrt (low-passed power of the L/R mean square), decimated to ~`rate` Hz; returns the rate. */
    inline double envelope(const Sig& l, const Sig& r, double sr, double lowPassHz, double rate,
                           std::vector<double>& env, double bandLo = 0.0, double bandHi = 0.0)
    {
        const int step = std::max(1, static_cast<int>(std::lround(sr / rate)));
        auto lp1 = lowPass(lowPassHz, sr), lp2 = lp1;
        std::array<Biquad, 4> bl {}, br {};
        const bool banded = bandHi > bandLo;
        if (banded)
        {
            bl = { highPass(bandLo, sr), highPass(bandLo, sr), lowPass(bandHi, sr), lowPass(bandHi, sr) };
            br = bl;
        }
        env.clear();
        for (size_t i = 0; i < l.size(); ++i)
        {
            double a = l[i], b = r[i];
            if (banded)
                for (size_t k = 0; k < 4; ++k)
                {
                    a = bl[k].process(a);
                    b = br[k].process(b);
                }
            const double y = lp2.process(lp1.process(0.5 * (a * a + b * b)));
            if (i % static_cast<size_t>(step) == 0)
                env.push_back(std::sqrt(std::max(0.0, y)));
        }
        return sr / step;
    }

    struct Modulation
    {
        std::vector<std::pair<double, double>> peaks;   // (Hz, depth), deepest first, 1..150 Hz
        std::vector<double> bandDepth;                  // RMS depth per band of modBandEdges
    };

    inline const std::vector<double>& modBandEdges()
    {
        static const std::vector<double> e { 1, 2, 4, 8, 16, 32, 64, 128, 150 };
        return e;
    }

    inline Modulation modulation(const Sig& l, const Sig& r, double sr)
    {
        std::vector<double> env;
        const double rate = envelope(l, r, sr, 250.0, 1000.0, env);
        constexpr int order = 10, n = 1 << order, hop = n / 2;
        const auto w = hann(n);
        const double sumW = std::accumulate(w.begin(), w.end(), 0.0);

        // Only segments within 30 dB of the loudest take part.
        std::vector<int> starts;
        std::vector<double> means;
        for (int s = 0; s + n <= static_cast<int>(env.size()); s += hop)
        {
            starts.push_back(s);
            means.push_back(std::accumulate(env.begin() + s, env.begin() + s + n, 0.0) / n);
        }
        Modulation m;
        m.bandDepth.assign(modBandEdges().size() - 1, 0.0);
        if (starts.empty())
            return m;
        const double loudest = *std::max_element(means.begin(), means.end());
        std::vector<double> power(static_cast<size_t>(n / 2 + 1), 0.0);
        std::vector<float> work;
        std::vector<double> p;
        int used = 0;
        for (size_t i = 0; i < starts.size(); ++i)
        {
            if (means[i] < loudest * 0.0316 || means[i] <= 0.0)
                continue;
            Sig x(static_cast<size_t>(n));
            for (int k = 0; k < n; ++k)
                x[static_cast<size_t>(k)] = static_cast<float>(env[static_cast<size_t>(starts[i] + k)] / means[i] - 1.0);
            powerSpectrum(x.data(), n, w, order, work, p);
            for (size_t k = 0; k < p.size(); ++k)
                power[k] += p[k];
            ++used;
        }
        const double binHz = rate / n;
        std::vector<double> depth(power.size());
        for (size_t k = 0; k < power.size(); ++k)
            depth[k] = 2.0 * std::sqrt(power[k] / std::max(1, used)) / sumW;   // a sinusoidal AM of index m reads m

        for (size_t b = 0; b + 1 < modBandEdges().size(); ++b)
        {
            double s = 0.0;
            for (size_t k = 1; k < depth.size(); ++k)
            {
                const double f = static_cast<double>(k) * binHz;
                if (f >= modBandEdges()[b] && f < modBandEdges()[b + 1])
                    s += depth[k] * depth[k] / 1.5;   // Hann's noise bandwidth: a pure line sums to m^2
            }
            m.bandDepth[b] = std::sqrt(s);
        }
        for (size_t k = 2; k + 1 < depth.size(); ++k)
        {
            const double f = static_cast<double>(k) * binHz;
            if (f < 1.0 || f > 150.0)
                continue;
            if (depth[k] > depth[k - 1] && depth[k] >= depth[k + 1] && depth[k] > 0.01)
            {
                // parabolic interpolation of the peak
                const double a = depth[k - 1], b = depth[k], c = depth[k + 1];
                const double d = 0.5 * (a - c) / std::min(-1.0e-12, a - 2.0 * b + c);
                m.peaks.emplace_back((static_cast<double>(k) + d) * binHz, b - 0.25 * (a - c) * d);
            }
        }
        std::sort(m.peaks.begin(), m.peaks.end(), [](auto& x, auto& y) { return x.second > y.second; });
        if (m.peaks.size() > 5)
            m.peaks.resize(5);
        return m;
    }

    /** Pearson correlation of the 1..4 kHz dB envelopes, best lag within +-30 ms. */
    inline double envelopeCorrelation(const Sig& inL, const Sig& inR, const Sig& outL, const Sig& outR, double sr)
    {
        std::vector<double> a, b;
        const double rate = envelope(inL, inR, sr, 30.0, 200.0, a, 1000.0, 4000.0);
        envelope(outL, outR, sr, 30.0, 200.0, b, 1000.0, 4000.0);
        const auto toDb = [](std::vector<double>& e)
        {
            const double peak = e.empty() ? 0.0 : *std::max_element(e.begin(), e.end());
            for (auto& x : e)
                x = std::max(db20(x), db20(peak) - 40.0);
        };
        toDb(a);
        toDb(b);
        const int n = static_cast<int>(std::min(a.size(), b.size()));
        const int maxLag = static_cast<int>(std::lround(0.03 * rate));
        double best = -1.0;
        for (int lag = -maxLag; lag <= maxLag; ++lag)
        {
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
            int c = 0;
            for (int i = std::max(0, -lag); i < n && i + lag < n; ++i)
            {
                const double x = a[static_cast<size_t>(i)], y = b[static_cast<size_t>(i + lag)];
                sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y;
                ++c;
            }
            if (c < 10)
                continue;
            const double cov = sab - sa * sb / c, va = saa - sa * sa / c, vb = sbb - sb * sb / c;
            if (va > 1.0e-12 && vb > 1.0e-12)
                best = std::max(best, cov / std::sqrt(va * vb));
        }
        return best;
    }

    //==========================================================================
    // Sine detection and THD.

    /** The sine's frequency if the spectrum is one (>= 98 % of the power within +-4 bins of its peak), else 0. */
    inline double sineFrequency(const Spectrum& s)
    {
        if (s.ms.size() < 8)
            return 0.0;
        size_t peak = 1;
        double total = 0.0;
        for (size_t k = 1; k < s.ms.size(); ++k)
        {
            total += s.ms[k];
            if (s.ms[k] > s.ms[peak])
                peak = k;
        }
        double around = 0.0;
        for (size_t k = peak > 4 ? peak - 4 : 1; k <= std::min(s.ms.size() - 1, peak + 4); ++k)
            around += s.ms[k];
        if (total <= 0.0 || around < 0.98 * total || peak < 2 || peak + 1 >= s.ms.size())
            return 0.0;
        const double a = std::log(s.ms[peak - 1] + 1e-30), b = std::log(s.ms[peak] + 1e-30), c = std::log(s.ms[peak + 1] + 1e-30);
        const double d = 0.5 * (a - c) / std::min(-1.0e-12, a - 2.0 * b + c);
        return (static_cast<double>(peak) + d) * s.binHz;
    }

    /** The strongest line above 20 Hz (parabolic interpolation on log power). */
    inline double strongestLine(const Spectrum& s)
    {
        size_t peak = 2;
        for (size_t k = 2; k + 1 < s.ms.size(); ++k)
            if (static_cast<double>(k) * s.binHz >= 20.0 && s.ms[k] > s.ms[peak])
                peak = k;
        const double a = std::log(s.ms[peak - 1] + 1e-30), b = std::log(s.ms[peak] + 1e-30), c = std::log(s.ms[peak + 1] + 1e-30);
        const double d = 0.5 * (a - c) / std::min(-1.0e-12, a - 2.0 * b + c);
        return (static_cast<double>(peak) + d) * s.binHz;
    }

    struct Thd
    {
        double pct = 0.0, db = 0.0, plusNoiseDb = 0.0;
    };

    inline Thd thd(const Spectrum& s, double f0, double sr)
    {
        const auto around = [&](double f)
        {
            const double c = f / s.binHz;
            double sum = 0.0;
            for (long k = std::max(1L, std::lround(c) - 4); k <= std::lround(c) + 4 && k < static_cast<long>(s.ms.size()); ++k)
                sum += s.ms[static_cast<size_t>(k)];
            return sum;
        };
        const double fund = around(f0);
        double harm = 0.0;
        for (int h = 2; h <= 10 && h * f0 < 0.5 * sr - 4 * s.binHz; ++h)
            harm += around(h * f0);
        const double total = band(s, 10.0, 0.5 * sr);
        Thd t;
        t.pct = 100.0 * std::sqrt(harm / std::max(fund, 1.0e-30));
        t.db = db10(harm / std::max(fund, 1.0e-30));
        t.plusNoiseDb = db10(std::max(total - fund, 1.0e-30) / std::max(fund, 1.0e-30));
        return t;
    }

    //==========================================================================
    // The record.

    inline juce::var num(double x)
    {
        return std::isfinite(x) ? juce::var(std::round(x * 1.0e4) / 1.0e4) : juce::var();
    }

    inline juce::var numArray(const std::vector<double>& v)
    {
        juce::Array<juce::var> a;
        for (double x : v)
            a.add(num(x));
        return a;
    }

    struct Reference
    {
        Sig l, r;
        Loudness loud;
        Frames frames;
        Spectrum spec;
        std::vector<bool> voiced;
        double sineHz = 0.0;
    };

    inline Sig mid(const Sig& l, const Sig& r)
    {
        Sig m(l.size());
        for (size_t i = 0; i < l.size(); ++i)
            m[i] = 0.5f * (l[i] + r[i]);
        return m;
    }

    inline Reference makeReference(const Sig& l, const Sig& r, double sr)
    {
        Reference ref { l, r, loudness(l, r, sr), analyseFrames(mid(l, r), sr), welch(l, r, sr), {}, 0.0 };
        ref.sineHz = sineFrequency(ref.spec);
        const double loudest = ref.frames.energy.empty() ? 0.0 : *std::max_element(ref.frames.energy.begin(), ref.frames.energy.end());
        for (size_t i = 0; i < ref.frames.r.size(); ++i)
            ref.voiced.push_back(ref.frames.energy[i] > loudest * 1.0e-4 && (ref.sineHz > 0.0 || ref.frames.r[i] > 0.6));
        return ref;
    }

    /**
     * Metrics of out (L, R; any non-finite sample counted and zeroed) against the input reference
     * (same length, latency-aligned). truePeakDb comes from the caller (its meter), clamps too.
     */
    inline juce::var measure(Sig l, Sig r, const Reference& ref, double sr, double truePeakDb, std::uint64_t clamps)
    {
        int nonFinite = 0;
        double peak = 0.0, sumSq = 0.0, sideSq = 0.0, midSq = 0.0;
        for (size_t i = 0; i < l.size(); ++i)
        {
            for (auto* x : { &l[i], &r[i] })
                if (!std::isfinite(*x))
                {
                    *x = 0.0f;
                    ++nonFinite;
                }
            peak = std::max({ peak, static_cast<double>(std::abs(l[i])), static_cast<double>(std::abs(r[i])) });
            sumSq += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
            const double m = 0.5 * (l[i] + r[i]), s = 0.5 * (l[i] - r[i]);
            midSq += m * m;
            sideSq += s * s;
        }
        const double rms = std::sqrt(sumSq / std::max<double>(1.0, 2.0 * static_cast<double>(l.size())));

        auto* o = new juce::DynamicObject();
        juce::var record(o);

        const auto loud = loudness(l, r, sr);
        o->setProperty("lufs", num(loud.integrated));
        o->setProperty("lra", num(loud.range));
        o->setProperty("momentaryStdLU", num(loud.momentaryStd));
        o->setProperty("shortTermStdLU", num(loud.shortTermStd));
        {
            // Pumping: how much the out-in loudness difference moves over the input's gated blocks.
            std::vector<double> d;
            const auto& inM = ref.loud.momentary;
            for (size_t i = 0; i < std::min(inM.size(), loud.momentary.size()); ++i)
                if (inM[i] > -70.0 && inM[i] > ref.loud.integrated - 20.0)
                    d.push_back(loud.momentary[i] - inM[i]);
            o->setProperty("pumpingLU", num(stdDev(d)));
        }
        o->setProperty("samplePeakDb", num(db20(peak)));
        o->setProperty("truePeakDb", num(truePeakDb));
        o->setProperty("rmsDb", num(db20(rms)));
        o->setProperty("crestDb", num(db20(peak) - db20(rms)));
        o->setProperty("sideMidDb", num(db10(sideSq / std::max(midSq, 1.0e-20))));

        const auto spec = welch(l, r, sr);
        {
            std::vector<double> bands;
            for (double c : thirdOctaveCentres())
                bands.push_back(c * std::pow(2.0, -1.0 / 6.0) < 0.5 * sr ? db10(band(spec, c * std::pow(2.0, -1.0 / 6.0), std::min(0.5 * sr, c * std::pow(2.0, 1.0 / 6.0)))) : -200.0);
            o->setProperty("thirdOctaveDb", numArray(bands));
        }
        const double total = band(spec, 20.0, 20000.0);
        double weighted = 0.0;
        for (size_t k = 1; k < spec.ms.size(); ++k)
        {
            const double f = static_cast<double>(k) * spec.binHz;
            if (f >= 20.0 && f < 20000.0)
                weighted += f * spec.ms[k];
        }
        o->setProperty("centroidHz", num(weighted / std::max(total, 1.0e-30)));
        o->setProperty("share160to500", num(band(spec, 160.0, 500.0) / std::max(total, 1.0e-30)));
        o->setProperty("share40to80", num(band(spec, 40.0, 80.0) / std::max(total, 1.0e-30)));
        const double hf = band(spec, 10000.0, 16000.0) / std::max(total, 1.0e-30);
        const double refTotal = band(ref.spec, 20.0, 20000.0);
        const double refHf = band(ref.spec, 10000.0, 16000.0) / std::max(refTotal, 1.0e-30);
        o->setProperty("share10kto16k", num(hf));
        o->setProperty("hf10kto16kRelDb", refHf > 1.0e-9 ? num(db10(hf) - db10(refHf)) : juce::var());

        // Harmonicity and fizz over the input's voiced frames.
        const auto frames = analyseFrames(mid(l, r), sr);
        double rSum = 0.0, eSum = 0.0, hfSum = 0.0;
        int voicedFrames = 0;
        for (size_t i = 0; i < std::min(frames.r.size(), ref.voiced.size()); ++i)
            if (ref.voiced[i])
            {
                rSum += frames.r[i];
                eSum += frames.energy[i];
                hfSum += frames.energy[i] * frames.above8k[i];
                ++voicedFrames;
            }
        const double rMean = std::clamp(rSum / std::max(1, voicedFrames), 1.0e-4, 0.9999);
        o->setProperty("voicedFrames", voicedFrames);
        o->setProperty("hnrDb", num(10.0 * std::log10(rMean / (1.0 - rMean))));
        o->setProperty("voicedAbove8kDb", num(db10(hfSum / std::max(eSum, 1.0e-30))));

        const auto mod = modulation(l, r, sr);
        {
            juce::Array<juce::var> peaks;
            for (const auto& [hz, depth] : mod.peaks)
            {
                auto* pk = new juce::DynamicObject();
                pk->setProperty("hz", num(hz));
                pk->setProperty("depth", num(depth));
                peaks.add(juce::var(pk));
            }
            o->setProperty("amPeaks", peaks);
            o->setProperty("amBandDepth", numArray(mod.bandDepth));
        }

        o->setProperty("envCorr1to4k", num(envelopeCorrelation(ref.l, ref.r, l, r, sr)));

        if (ref.sineHz > 0.0)
        {
            // Against the output's own strongest line: PITCH may have moved the tone.
            const double f0 = strongestLine(spec);
            const auto t = thd(spec, f0, sr);
            auto* th = new juce::DynamicObject();
            th->setProperty("f0", num(f0));
            th->setProperty("pct", num(t.pct));
            th->setProperty("db", num(t.db));
            th->setProperty("plusNoiseDb", num(t.plusNoiseDb));
            o->setProperty("thd", juce::var(th));
        }
        else
        {
            o->setProperty("thd", juce::var());
        }

        o->setProperty("clamps", static_cast<juce::int64>(clamps));
        o->setProperty("nonFinite", nonFinite);
        return record;
    }
}

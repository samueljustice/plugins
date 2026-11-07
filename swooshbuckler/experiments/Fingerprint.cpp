#include "Fingerprint.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace fp
{
namespace
{
    constexpr float c_floorDb = -100.0f;
    constexpr float c_activeRangeDb = 45.0f;   // frames within this of the peak are "active"
    constexpr int c_numMel = 40;
    constexpr float c_minF0 = 50.0f, c_maxF0 = 2000.0f;
    constexpr float c_yinThreshold = 0.15f;
    constexpr float c_voicedClarity = 0.70f;   // clarity = 1 - CMNDF minimum
    constexpr float c_voicedMinDb = -65.0f;

    float median(std::vector<float> v)
    {
        if (v.empty())
            return 0.0f;
        const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
        std::nth_element(v.begin(), mid, v.end());
        return *mid;
    }

    float toDb(double power) { return std::max(c_floorDb, static_cast<float>(10.0 * std::log10(power + 1.0e-12))); }

    //==========================================================================
    /** Low-pass + decimate by D (windowed sinc, only the kept outputs are computed). */
    std::vector<float> decimate(const float* x, int n, int D)
    {
        if (D <= 1)
            return std::vector<float>(x, x + n);

        const int half = 8 * D;
        const int taps = 2 * half + 1;
        std::vector<float> h(static_cast<size_t>(taps));
        const double fc = 0.42 / D; // cycles per input sample
        double sum = 0.0;
        for (int k = 0; k < taps; ++k)
        {
            const double m = k - half;
            const double sinc = m == 0 ? 2.0 * fc : std::sin(2.0 * juce::MathConstants<double>::pi * fc * m) / (juce::MathConstants<double>::pi * m);
            const double w = 0.42 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * k / (taps - 1))
                             + 0.08 * std::cos(4.0 * juce::MathConstants<double>::pi * k / (taps - 1)); // Blackman
            h[static_cast<size_t>(k)] = static_cast<float>(sinc * w);
            sum += sinc * w;
        }
        for (auto& c : h)
            c = static_cast<float>(c / sum);

        const int m = n / D;
        std::vector<float> y(static_cast<size_t>(m));
        for (int i = 0; i < m; ++i)
        {
            const int c = i * D;
            const int k0 = std::max(0, half - c);
            const int k1 = std::min(taps, n - c + half);
            float acc = 0.0f;
            for (int k = k0; k < k1; ++k)
                acc += h[static_cast<size_t>(k)] * x[c - half + k];
            y[static_cast<size_t>(i)] = acc;
        }
        return y;
    }

    //==========================================================================
    /** FFT-based YIN on the decimated signal. */
    struct Yin
    {
        double fs = 12000.0;
        int W = 0, tauMin = 2, tauMax = 0, order = 10, N = 1024;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> a, b, seg, prefix, d;

        void prepare(double rate)
        {
            fs = rate;
            tauMax = static_cast<int>(std::ceil(fs / c_minF0));
            tauMin = std::max(2, static_cast<int>(std::floor(fs / c_maxF0)));
            W = static_cast<int>(std::ceil(0.025 * fs));
            const int need = 2 * W + tauMax; // a (W) correlated against b (W + tauMax), no wrap for positive lags
            order = 1;
            while ((1 << order) < need)
                ++order;
            N = 1 << order;
            fft = std::make_unique<juce::dsp::FFT>(order);
            a.assign(static_cast<size_t>(2 * N), 0.0f);
            b.assign(static_cast<size_t>(2 * N), 0.0f);
            seg.assign(static_cast<size_t>(W + tauMax), 0.0f);
            prefix.assign(static_cast<size_t>(W + tauMax + 1), 0.0f);
            d.assign(static_cast<size_t>(tauMax + 1), 0.0f);
        }

        /** Analyses the segment starting at x[start]; returns {f0 (0 = none), clarity}. */
        std::pair<float, float> analyse(const std::vector<float>& x, int start)
        {
            const int L = W + tauMax;
            const int n = static_cast<int>(x.size());
            for (int i = 0; i < L; ++i)
            {
                const int k = start + i;
                seg[static_cast<size_t>(i)] = (k >= 0 && k < n) ? x[static_cast<size_t>(k)] : 0.0f;
            }
            prefix[0] = 0.0f;
            double acc = 0.0;
            for (int i = 0; i < L; ++i)
            {
                acc += static_cast<double>(seg[static_cast<size_t>(i)]) * seg[static_cast<size_t>(i)];
                prefix[static_cast<size_t>(i + 1)] = static_cast<float>(acc);
            }
            const float e0 = prefix[static_cast<size_t>(W)];
            if (e0 < 1.0e-9f * static_cast<float>(W))
                return { 0.0f, 0.0f };

            std::fill(a.begin(), a.end(), 0.0f);
            std::fill(b.begin(), b.end(), 0.0f);
            std::copy(seg.begin(), seg.begin() + W, a.begin());
            std::copy(seg.begin(), seg.begin() + L, b.begin());
            fft->performRealOnlyForwardTransform(a.data());
            fft->performRealOnlyForwardTransform(b.data());
            // conj(A) * B for all N bins (interleaved complex)
            for (int k = 0; k < N; ++k)
            {
                const float ar = a[static_cast<size_t>(2 * k)], ai = a[static_cast<size_t>(2 * k + 1)];
                const float br = b[static_cast<size_t>(2 * k)], bi = b[static_cast<size_t>(2 * k + 1)];
                b[static_cast<size_t>(2 * k)] = ar * br + ai * bi;
                b[static_cast<size_t>(2 * k + 1)] = ar * bi - ai * br;
            }
            fft->performRealOnlyInverseTransform(b.data());
            // b[tau] = r(tau) up to a scale; calibrate with r(0) = e0.
            const float scale = std::abs(b[0]) > 1.0e-20f ? e0 / b[0] : 1.0f;

            // Difference function and cumulative-mean-normalised difference (CMNDF).
            d[0] = 1.0f;
            double running = 0.0;
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                const float et = prefix[static_cast<size_t>(tau + W)] - prefix[static_cast<size_t>(tau)];
                const float diff = std::max(0.0f, e0 + et - 2.0f * scale * b[static_cast<size_t>(tau)]);
                running += diff;
                d[static_cast<size_t>(tau)] = running > 0.0 ? static_cast<float>(diff * tau / running) : 1.0f;
            }

            int best = -1;
            for (int tau = tauMin; tau < tauMax; ++tau)
            {
                if (d[static_cast<size_t>(tau)] < c_yinThreshold)
                {
                    while (tau + 1 < tauMax && d[static_cast<size_t>(tau + 1)] < d[static_cast<size_t>(tau)])
                        ++tau;
                    best = tau;
                    break;
                }
            }
            if (best < 0)
            {
                best = tauMin;
                for (int tau = tauMin + 1; tau < tauMax; ++tau)
                    if (d[static_cast<size_t>(tau)] < d[static_cast<size_t>(best)])
                        best = tau;
            }

            // Parabolic interpolation.
            float t = static_cast<float>(best);
            float dmin = d[static_cast<size_t>(best)];
            if (best > 1 && best < tauMax)
            {
                const float y0 = d[static_cast<size_t>(best - 1)], y1 = d[static_cast<size_t>(best)], y2 = d[static_cast<size_t>(best + 1)];
                const float den = y0 - 2.0f * y1 + y2;
                if (den > 1.0e-9f)
                {
                    const float off = juce::jlimit(-0.5f, 0.5f, 0.5f * (y0 - y2) / den);
                    t += off;
                    dmin = y1 - 0.25f * (y0 - y2) * off;
                }
            }
            const float clarity = juce::jlimit(0.0f, 1.0f, 1.0f - dmin);
            return { static_cast<float>(fs / t), clarity };
        }
    };

    //==========================================================================
    void computeSummary(Fingerprint& f)
    {
        auto& s = f.summary;
        s = {};
        const int n = f.numFrames();
        s.durationSeconds = static_cast<float>(f.numSamples / f.sampleRate);
        if (n == 0)
            return;

        // Smoothed loudness (5-frame power average) for the peak.
        std::vector<float> smooth(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            double p = 0.0;
            int c = 0;
            for (int k = std::max(0, i - 2); k <= std::min(n - 1, i + 2); ++k, ++c)
                p += std::pow(10.0, f.rmsDb[static_cast<size_t>(k)] / 10.0);
            smooth[static_cast<size_t>(i)] = toDb(p / c);
        }
        s.peakFrame = static_cast<int>(std::max_element(smooth.begin(), smooth.end()) - smooth.begin());
        s.peakDb = smooth[static_cast<size_t>(s.peakFrame)];

        const float gate = s.peakDb - c_activeRangeDb;
        s.activeStart = 0;
        while (s.activeStart < n - 1 && f.rmsDb[static_cast<size_t>(s.activeStart)] < gate)
            ++s.activeStart;
        s.activeEnd = n;
        while (s.activeEnd > s.activeStart + 1 && f.rmsDb[static_cast<size_t>(s.activeEnd - 1)] < gate)
            --s.activeEnd;

        std::vector<float> voicedPitch;
        double power = 0, clar = 0, flat = 0, cent = 0;
        int counted = 0;
        for (int i = s.activeStart; i < s.activeEnd; ++i)
        {
            const auto u = static_cast<size_t>(i);
            power += std::pow(10.0, f.rmsDb[u] / 10.0);
            if (f.rmsDb[u] < gate)
                continue;
            ++counted;
            clar += f.pitchClarity[u];
            flat += f.flatness[u];
            cent += f.centroidHz[u];
            if (f.pitchHz[u] > 0.0f)
                voicedPitch.push_back(f.pitchHz[u]);
            for (int k = 0; k < c_numMfcc; ++k)
                s.mfccMean[static_cast<size_t>(k)] += f.mfcc[u][static_cast<size_t>(k)];
        }
        const int active = s.activeEnd - s.activeStart;
        s.meanDb = toDb(power / std::max(1, active));
        if (counted > 0)
        {
            s.meanClarity = static_cast<float>(clar / counted);
            s.meanFlatness = static_cast<float>(flat / counted);
            s.meanCentroidHz = static_cast<float>(cent / counted);
            for (auto& m : s.mfccMean)
                m /= static_cast<float>(counted);
            s.voicedRatio = static_cast<float>(voicedPitch.size()) / static_cast<float>(counted);
        }
        s.medianPitchHz = median(voicedPitch);

        const double t0 = s.activeStart * f.hopSeconds, t1 = s.activeEnd * f.hopSeconds;
        int onsets = 0;
        for (auto t : f.onsetsSeconds)
            if (t >= t0 && t < t1)
                ++onsets;
        s.onsetRate = static_cast<float>(onsets / std::max(0.01, t1 - t0));
    }
}

//==============================================================================
Fingerprint analyse(const float* mono, int numSamples, double sampleRate)
{
    Fingerprint f;
    f.sampleRate = sampleRate;
    f.numSamples = numSamples;
    if (mono == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return f;

    const int hop = std::max(1, static_cast<int>(std::lround(sampleRate * 0.01)));
    f.hopSeconds = hop / sampleRate;
    const int numFrames = numSamples / hop + 1;

    // ---- Spectral analysis (Hann, ~21 ms) ----
    int order = 1;
    while ((1 << order) < static_cast<int>(sampleRate * 0.021))
        ++order;
    const int N = 1 << order;
    const int bins = N / 2 + 1;
    juce::dsp::FFT fft(order);
    std::vector<float> window(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i)
        window[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * static_cast<float>(i) / static_cast<float>(N));
    const float winGain = 0.5f * static_cast<float>(N);

    const float binHz = static_cast<float>(sampleRate / N);
    const float topHz = std::min(16000.0f, static_cast<float>(sampleRate * 0.5));
    const int kTop = std::min(bins - 1, static_cast<int>(topHz / binHz));

    // Mel filterbank (triangular, power), 40 Hz .. topHz.
    auto hzToMel = [](float hz) { return 2595.0f * std::log10(1.0f + hz / 700.0f); };
    auto melToHz = [](float m) { return 700.0f * (std::pow(10.0f, m / 2595.0f) - 1.0f); };
    struct MelBand { int k0, k1; std::vector<float> w; };
    std::vector<MelBand> mel(c_numMel);
    {
        const float m0 = hzToMel(40.0f), m1 = hzToMel(topHz);
        std::array<float, c_numMel + 2> edges {};
        for (int i = 0; i < c_numMel + 2; ++i)
            edges[static_cast<size_t>(i)] = melToHz(m0 + (m1 - m0) * static_cast<float>(i) / static_cast<float>(c_numMel + 1));
        for (int b = 0; b < c_numMel; ++b)
        {
            const float lo = edges[static_cast<size_t>(b)], mid = edges[static_cast<size_t>(b + 1)], hi = edges[static_cast<size_t>(b + 2)];
            auto& band = mel[static_cast<size_t>(b)];
            band.k0 = std::max(1, static_cast<int>(std::floor(lo / binHz)));
            band.k1 = std::min(kTop, static_cast<int>(std::ceil(hi / binHz)));
            for (int k = band.k0; k <= band.k1; ++k)
            {
                const float hz = static_cast<float>(k) * binHz;
                float w = hz < mid ? (hz - lo) / std::max(1.0f, mid - lo) : (hi - hz) / std::max(1.0f, hi - mid);
                band.w.push_back(std::max(0.0f, w));
            }
            // Narrow low bands may fall between bins: give them their nearest bin.
            if (std::all_of(band.w.begin(), band.w.end(), [](float w) { return w <= 0.0f; }))
            {
                band.k0 = band.k1 = juce::jlimit(1, kTop, static_cast<int>(std::lround(mid / binHz)));
                band.w.assign(1, 1.0f);
            }
        }
    }
    std::array<std::array<float, c_numMel>, c_numMfcc> dct {};
    for (int k = 0; k < c_numMfcc; ++k)
        for (int b = 0; b < c_numMel; ++b)
            dct[static_cast<size_t>(k)][static_cast<size_t>(b)] = std::cos(juce::MathConstants<float>::pi * static_cast<float>(k) * (static_cast<float>(b) + 0.5f) / c_numMel)
                                                                  * std::sqrt((k == 0 ? 1.0f : 2.0f) / c_numMel);

    f.rmsDb.resize(static_cast<size_t>(numFrames));
    f.pitchHz.assign(static_cast<size_t>(numFrames), 0.0f);
    f.pitchClarity.assign(static_cast<size_t>(numFrames), 0.0f);
    f.centroidHz.resize(static_cast<size_t>(numFrames));
    f.rolloffHz.resize(static_cast<size_t>(numFrames));
    f.flatness.resize(static_cast<size_t>(numFrames));
    f.flux.assign(static_cast<size_t>(numFrames), 0.0f);
    f.mfcc.resize(static_cast<size_t>(numFrames));
    std::vector<std::array<float, c_numMel>> melDb(static_cast<size_t>(numFrames));

    std::vector<float> buf(static_cast<size_t>(2 * N));
    std::vector<float> pw(static_cast<size_t>(bins));
    for (int i = 0; i < numFrames; ++i)
    {
        const auto u = static_cast<size_t>(i);
        const int start = i * hop - N / 2;
        double sq = 0.0;
        for (int j = 0; j < N; ++j)
        {
            const int k = start + j;
            const float v = (k >= 0 && k < numSamples) ? mono[k] : 0.0f;
            sq += static_cast<double>(v) * v;
            buf[static_cast<size_t>(j)] = v * window[static_cast<size_t>(j)];
        }
        std::fill(buf.begin() + N, buf.end(), 0.0f);
        f.rmsDb[u] = toDb(sq / N);

        fft.performFrequencyOnlyForwardTransform(buf.data(), true);
        double sumMag = 0, sumMagHz = 0, sumPow = 0, sumLog = 0;
        for (int k = 1; k <= kTop; ++k)
        {
            const float m = buf[static_cast<size_t>(k)] / winGain;
            const float p = m * m;
            pw[static_cast<size_t>(k)] = p;
            sumMag += m;
            sumMagHz += static_cast<double>(m) * k * binHz;
            sumPow += p;
            sumLog += std::log(p + 1.0e-14);
        }
        const int K = kTop;
        if (sumPow > 1.0e-12)
        {
            f.centroidHz[u] = static_cast<float>(sumMagHz / sumMag);
            f.flatness[u] = juce::jlimit(0.0f, 1.0f, static_cast<float>(std::exp(sumLog / K) / (sumPow / K + 1.0e-14)));
            const double target = 0.85 * sumPow;
            double run = 0.0;
            int k = 1;
            for (; k <= kTop; ++k)
            {
                run += pw[static_cast<size_t>(k)];
                if (run >= target)
                    break;
            }
            f.rolloffHz[u] = static_cast<float>(std::min(k, kTop)) * binHz;
        }
        else
        {
            f.centroidHz[u] = 0.0f;
            f.flatness[u] = 1.0f;
            f.rolloffHz[u] = 0.0f;
        }

        auto& md = melDb[u];
        std::array<float, c_numMel> lnMel {};
        for (int b = 0; b < c_numMel; ++b)
        {
            const auto& band = mel[static_cast<size_t>(b)];
            double e = 0.0;
            for (int k = band.k0; k <= band.k1; ++k)
                e += band.w[static_cast<size_t>(k - band.k0)] * pw[static_cast<size_t>(k)];
            md[static_cast<size_t>(b)] = toDb(e);
            lnMel[static_cast<size_t>(b)] = static_cast<float>(std::log(e + 1.0e-10));
        }
        for (int k = 0; k < c_numMfcc; ++k)
        {
            float c = 0.0f;
            for (int b = 0; b < c_numMel; ++b)
                c += dct[static_cast<size_t>(k)][static_cast<size_t>(b)] * lnMel[static_cast<size_t>(b)];
            f.mfcc[u][static_cast<size_t>(k)] = c;
        }
    }

    // ---- Flux (positive log-mel difference, floored 70 dB under the loudest band) and onsets ----
    {
        float top = c_floorDb;
        for (const auto& md : melDb)
            top = std::max(top, *std::max_element(md.begin(), md.end()));
        const float floorDb = top - 70.0f;
        for (int i = 1; i < numFrames; ++i)
        {
            float s = 0.0f;
            for (int b = 0; b < c_numMel; ++b)
            {
                const float now = std::max(floorDb, melDb[static_cast<size_t>(i)][static_cast<size_t>(b)]);
                const float was = std::max(floorDb, melDb[static_cast<size_t>(i - 1)][static_cast<size_t>(b)]);
                s += std::max(0.0f, now - was);
            }
            f.flux[static_cast<size_t>(i)] = s / c_numMel;
        }

        const int minGap = std::max(1, static_cast<int>(std::lround(0.03 / f.hopSeconds)));
        int last = -minGap;
        std::vector<float> win;
        for (int i = 1; i < numFrames - 1; ++i)
        {
            const float v = f.flux[static_cast<size_t>(i)];
            if (v < 3.0f) // dB per band
                continue;
            bool peak = true;
            for (int k = std::max(0, i - 3); k <= std::min(numFrames - 1, i + 3) && peak; ++k)
                if (k != i && (f.flux[static_cast<size_t>(k)] > v || (k < i && f.flux[static_cast<size_t>(k)] == v)))
                    peak = false;
            if (! peak)
                continue;
            win.clear();
            for (int k = std::max(0, i - 10); k <= std::min(numFrames - 1, i + 10); ++k)
                win.push_back(f.flux[static_cast<size_t>(k)]);
            if (v < median(win) + 2.0f)
                continue;
            if (i - last < minGap)
                continue;
            last = i;
            f.onsetsSeconds.push_back(static_cast<float>(i * f.hopSeconds));
        }
    }

    // ---- Pitch (FFT YIN at ~12 kHz) ----
    {
        const int D = std::max(1, static_cast<int>(std::floor(sampleRate / 11000.0)));
        const auto low = decimate(mono, numSamples, D);
        Yin yin;
        yin.prepare(sampleRate / D);
        const int L = yin.W + yin.tauMax;
        for (int i = 0; i < numFrames; ++i)
        {
            if (f.rmsDb[static_cast<size_t>(i)] < c_voicedMinDb)
                continue;
            const int centre = (i * hop) / D;
            const auto [hz, clarity] = yin.analyse(low, centre - L / 2);
            f.pitchClarity[static_cast<size_t>(i)] = clarity;
            if (clarity >= c_voicedClarity && hz >= c_minF0 && hz <= c_maxF0)
                f.pitchHz[static_cast<size_t>(i)] = hz;
        }

        // 5-frame median over voiced neighbours (kills isolated octave errors), then drop voiced islands < 3 frames.
        std::vector<float> fixed = f.pitchHz;
        std::vector<float> nb;
        for (int i = 0; i < numFrames; ++i)
        {
            if (f.pitchHz[static_cast<size_t>(i)] <= 0.0f)
                continue;
            nb.clear();
            for (int k = std::max(0, i - 2); k <= std::min(numFrames - 1, i + 2); ++k)
                if (f.pitchHz[static_cast<size_t>(k)] > 0.0f)
                    nb.push_back(f.pitchHz[static_cast<size_t>(k)]);
            fixed[static_cast<size_t>(i)] = median(nb);
        }
        for (int i = 0; i < numFrames;)
        {
            if (fixed[static_cast<size_t>(i)] <= 0.0f) { ++i; continue; }
            int j = i;
            while (j < numFrames && fixed[static_cast<size_t>(j)] > 0.0f)
                ++j;
            if (j - i < 3)
                for (int k = i; k < j; ++k)
                    fixed[static_cast<size_t>(k)] = 0.0f;
            i = j;
        }
        f.pitchHz = std::move(fixed);
    }

    computeSummary(f);
    return f;
}

Fingerprint slice(const Fingerprint& f, int frameStart, int frameEnd)
{
    Fingerprint s;
    s.hopSeconds = f.hopSeconds;
    s.sampleRate = f.sampleRate;
    frameStart = juce::jlimit(0, f.numFrames(), frameStart);
    frameEnd = juce::jlimit(frameStart, f.numFrames(), frameEnd);
    s.numSamples = static_cast<int>(std::lround((frameEnd - frameStart) * f.hopSeconds * f.sampleRate));
    auto cut = [&](const auto& v) { return std::decay_t<decltype(v)>(v.begin() + frameStart, v.begin() + frameEnd); };
    s.rmsDb = cut(f.rmsDb);
    s.pitchHz = cut(f.pitchHz);
    s.pitchClarity = cut(f.pitchClarity);
    s.centroidHz = cut(f.centroidHz);
    s.rolloffHz = cut(f.rolloffHz);
    s.flatness = cut(f.flatness);
    s.flux = cut(f.flux);
    s.mfcc = cut(f.mfcc);
    const double t0 = frameStart * f.hopSeconds, t1 = frameEnd * f.hopSeconds;
    for (auto t : f.onsetsSeconds)
        if (t >= t0 && t < t1)
            s.onsetsSeconds.push_back(static_cast<float>(t - t0));
    computeSummary(s);
    return s;
}

//==============================================================================
// Comparison: per-frame "shape" features, normalised per sound so a gain change, a transposition
// or a different speed doesn't count as a difference.
namespace
{
    constexpr int c_dims = 4 + 6; // loudness, clarity, log flatness, relative brightness, MFCC 1..6

    struct Seq
    {
        std::vector<std::array<float, c_dims>> x;
        std::vector<float> semis, clarity; // semis relative to the sound's median pitch (valid where clarity > 0)
        std::vector<float> frame;          // source frame index of each element
        int size() const { return static_cast<int>(x.size()); }
    };

    /** Features of frames [s, e) of f, resampled to L elements (L <= 0: one per frame). */
    Seq makeSeq(const Fingerprint& f, int s, int e, int L)
    {
        Seq q;
        if (e <= s)
            return q;
        if (L <= 0)
            L = e - s;

        float peak = c_floorDb;
        std::vector<float> voiced, cents;
        for (int i = s; i < e; ++i)
            peak = std::max(peak, f.rmsDb[static_cast<size_t>(i)]);
        for (int i = s; i < e; ++i)
        {
            if (f.pitchHz[static_cast<size_t>(i)] > 0.0f)
                voiced.push_back(f.pitchHz[static_cast<size_t>(i)]);
            if (f.rmsDb[static_cast<size_t>(i)] > peak - c_activeRangeDb && f.centroidHz[static_cast<size_t>(i)] > 0.0f)
                cents.push_back(f.centroidHz[static_cast<size_t>(i)]);
        }
        const float medPitch = median(voiced);
        const float medCent = std::max(50.0f, median(cents));

        auto frameFeatures = [&](int i, std::array<float, c_dims>& out, float& semi, float& clar)
        {
            const auto u = static_cast<size_t>(i);
            const float rel = std::max(-60.0f, f.rmsDb[u] - peak);
            const float gate = rel > -c_activeRangeDb ? 1.0f : 0.0f;
            out[0] = rel / 20.0f;
            out[1] = f.pitchClarity[u] * gate;
            out[2] = gate > 0 ? juce::jlimit(-1.0f, 0.0f, std::log10(std::max(1.0e-6f, f.flatness[u])) / 3.0f) : 0.0f;
            out[3] = gate > 0 && f.centroidHz[u] > 0 ? juce::jlimit(-1.5f, 1.5f, std::log2(f.centroidHz[u] / medCent) / 2.0f) : 0.0f;
            for (int k = 0; k < 6; ++k)
                out[static_cast<size_t>(4 + k)] = gate * f.mfcc[u][static_cast<size_t>(1 + k)] / 25.0f;
            if (f.pitchHz[u] > 0.0f && medPitch > 0.0f)
            {
                semi = 12.0f * std::log2(f.pitchHz[u] / medPitch);
                clar = f.pitchClarity[u];
            }
            else
            {
                semi = 0.0f;
                clar = 0.0f;
            }
        };

        q.x.resize(static_cast<size_t>(L));
        q.semis.resize(static_cast<size_t>(L));
        q.clarity.resize(static_cast<size_t>(L));
        q.frame.resize(static_cast<size_t>(L));
        const int count = e - s;
        for (int j = 0; j < L; ++j)
        {
            // Average the source frames that fall into this element (or interpolate when upsampling).
            const double a = s + static_cast<double>(j) * count / L;
            const double b = s + static_cast<double>(j + 1) * count / L;
            int i0 = static_cast<int>(std::floor(a)), i1 = std::max(i0 + 1, static_cast<int>(std::ceil(b)));
            i1 = std::min(i1, e);
            std::array<float, c_dims> acc {};
            float semiAcc = 0, clarAcc = 0, wAcc = 0;
            for (int i = i0; i < i1; ++i)
            {
                std::array<float, c_dims> v {};
                float semi = 0, clar = 0;
                frameFeatures(i, v, semi, clar);
                for (int d = 0; d < c_dims; ++d)
                    acc[static_cast<size_t>(d)] += v[static_cast<size_t>(d)];
                semiAcc += semi * clar;
                clarAcc += clar;
                wAcc += 1.0f;
            }
            for (auto& v : acc)
                v /= wAcc;
            q.x[static_cast<size_t>(j)] = acc;
            q.semis[static_cast<size_t>(j)] = clarAcc > 0 ? semiAcc / clarAcc : 0.0f;
            q.clarity[static_cast<size_t>(j)] = clarAcc / wAcc;
            q.frame[static_cast<size_t>(j)] = static_cast<float>(0.5 * (a + b) - 0.5);
        }
        return q;
    }

    constexpr std::array<float, c_dims> c_weights { 1.0f, 1.0f, 1.0f, 0.5f, 0.25f, 0.25f, 0.2f, 0.2f, 0.15f, 0.15f };

    float frameDistance(const Seq& A, int i, const Seq& B, int j)
    {
        const auto& a = A.x[static_cast<size_t>(i)];
        const auto& b = B.x[static_cast<size_t>(j)];
        float d = 0.0f;
        for (int k = 0; k < c_dims; ++k)
        {
            const float t = a[static_cast<size_t>(k)] - b[static_cast<size_t>(k)];
            d += c_weights[static_cast<size_t>(k)] * t * t;
        }
        // Pitch only counts where both are pitched.
        const float conf = A.clarity[static_cast<size_t>(i)] * B.clarity[static_cast<size_t>(j)];
        const float dp = (A.semis[static_cast<size_t>(i)] - B.semis[static_cast<size_t>(j)]) / 12.0f;
        d += 1.5f * conf * dp * dp;
        return std::sqrt(d);
    }

    struct DtwResult
    {
        float cost = std::numeric_limits<float>::max();
        std::vector<std::pair<int, int>> path; // (i, j) from start to end
    };

    /**
     * Symmetric DTW (diagonal steps weigh 2, so the normalised cost is a per-frame average).
     * band: Sakoe-Chiba radius as a fraction of the longer sequence (<= 0 = none).
     * subsequence: A may start and end anywhere in B (open begin / open end).
     */
    DtwResult dtw(const Seq& A, const Seq& B, float band, bool subsequence)
    {
        DtwResult r;
        const int n = A.size(), m = B.size();
        if (n == 0 || m == 0)
            return r;
        const float inf = std::numeric_limits<float>::max() * 0.25f;
        std::vector<float> D(static_cast<size_t>(n * m), inf);
        std::vector<unsigned char> step(static_cast<size_t>(n * m), 0); // 0 diag, 1 from i-1, 2 from j-1, 3 start
        const int radius = band > 0.0f && ! subsequence ? std::max(2, static_cast<int>(band * std::max(n, m))) : std::max(n, m);
        auto at = [m](int i, int j) { return static_cast<size_t>(i * m + j); };

        for (int i = 0; i < n; ++i)
        {
            int jc = subsequence ? 0 : static_cast<int>(std::lround(static_cast<double>(i) * (m - 1) / std::max(1, n - 1)));
            const int j0 = subsequence ? 0 : std::max(0, jc - radius);
            const int j1 = subsequence ? m - 1 : std::min(m - 1, jc + radius);
            for (int j = j0; j <= j1; ++j)
            {
                const float d = frameDistance(A, i, B, j);
                if (i == 0 && (j == 0 || subsequence))
                {
                    D[at(i, j)] = d;
                    step[at(i, j)] = 3;
                    continue;
                }
                float best = inf;
                unsigned char s = 0;
                if (i > 0 && j > 0 && D[at(i - 1, j - 1)] + 2.0f * d < best) { best = D[at(i - 1, j - 1)] + 2.0f * d; s = 0; }
                if (i > 0 && D[at(i - 1, j)] + d < best) { best = D[at(i - 1, j)] + d; s = 1; }
                if (j > 0 && D[at(i, j - 1)] + d < best) { best = D[at(i, j - 1)] + d; s = 2; }
                D[at(i, j)] = best;
                step[at(i, j)] = s;
            }
        }

        int bj = m - 1;
        if (subsequence)
        {
            // Open end: best normalised cost on the last row. Normalise by n + (matched length) like the path weights.
            float best = inf;
            for (int j = 0; j < m; ++j)
                if (D[at(n - 1, j)] < best)
                {
                    best = D[at(n - 1, j)];
                    bj = j;
                }
        }
        if (D[at(n - 1, bj)] >= inf)
            return r;

        int i = n - 1, j = bj;
        while (true)
        {
            r.path.emplace_back(i, j);
            const auto s = step[at(i, j)];
            if (s == 3)
                break;
            if (s == 0) { --i; --j; }
            else if (s == 1) --i;
            else --j;
            if (i < 0 || j < 0)
                break;
        }
        std::reverse(r.path.begin(), r.path.end());
        const int jStart = r.path.front().second;
        r.cost = D[at(n - 1, bj)] / static_cast<float>(n + (bj - jStart + 1));
        return r;
    }

    float summaryDistance(const Fingerprint::Summary& a, const Fingerprint::Summary& b)
    {
        auto lf = [](float flat) { return std::log10(std::max(1.0e-5f, flat)); };
        const float dv = a.voicedRatio - b.voicedRatio;
        const float dc = a.meanClarity - b.meanClarity;
        const float df = (lf(a.meanFlatness) - lf(b.meanFlatness)) / 2.0f;
        const float dbr = a.meanCentroidHz > 0 && b.meanCentroidHz > 0 ? std::log2(a.meanCentroidHz / b.meanCentroidHz) / 3.0f : 0.5f;
        const float dor = (std::log2(1.0f + a.onsetRate) - std::log2(1.0f + b.onsetRate)) / 2.0f;
        return std::sqrt(dv * dv + dc * dc + df * df + 0.5f * dbr * dbr + dor * dor);
    }

    constexpr int c_simLength = 100;

    float scoreFromCosts(float dtwCost, float summaryCost)
    {
        return std::exp(-(dtwCost + 0.5f * summaryCost) / 0.45f);
    }
}

float similarity(const Fingerprint& a, const Fingerprint& b)
{
    if (a.empty() || b.empty())
        return 0.0f;
    const auto A = makeSeq(a, a.summary.activeStart, a.summary.activeEnd, c_simLength);
    const auto B = makeSeq(b, b.summary.activeStart, b.summary.activeEnd, c_simLength);
    const auto r = dtw(A, B, 0.25f, false);
    if (r.path.empty())
        return 0.0f;
    return juce::jlimit(0.0f, 1.0f, scoreFromCosts(r.cost, summaryDistance(a.summary, b.summary)));
}

Alignment align(const Fingerprint& a, const Fingerprint& b)
{
    Alignment out;
    out.bFrameForA.assign(static_cast<size_t>(a.numFrames()), 0.0f);
    if (a.empty() || b.empty())
        return out;

    const int as = a.summary.activeStart, ae = a.summary.activeEnd;
    const int bs = b.summary.activeStart, be = b.summary.activeEnd;
    // Keep the grid under ~1M cells: decimate both by the same factor (keeps the time scale).
    int step = 1;
    while (static_cast<double>(ae - as) * (be - bs) / (step * step) > 1.0e6)
        ++step;
    const auto A = makeSeq(a, as, ae, (ae - as + step - 1) / step);
    const auto B = makeSeq(b, bs, be, (be - bs + step - 1) / step);
    const auto r = dtw(A, B, 0.0f, false);
    if (r.path.empty())
        return out;
    out.cost = r.cost;

    // Mean matched B frame per A element.
    std::vector<double> sum(static_cast<size_t>(A.size()), 0.0);
    std::vector<int> cnt(static_cast<size_t>(A.size()), 0);
    for (auto [i, j] : r.path)
    {
        sum[static_cast<size_t>(i)] += B.frame[static_cast<size_t>(j)];
        ++cnt[static_cast<size_t>(i)];
    }
    std::vector<float> mapped(static_cast<size_t>(A.size()));
    for (int i = 0; i < A.size(); ++i)
        mapped[static_cast<size_t>(i)] = static_cast<float>(sum[static_cast<size_t>(i)] / std::max(1, cnt[static_cast<size_t>(i)]));
    // Light smoothing (3-tap) of the warp, then interpolate onto every frame of a.
    if (mapped.size() > 2)
    {
        auto copy = mapped;
        for (size_t i = 1; i + 1 < mapped.size(); ++i)
            mapped[i] = 0.25f * copy[i - 1] + 0.5f * copy[i] + 0.25f * copy[i + 1];
    }
    out.stretch = static_cast<float>(be - bs) / static_cast<float>(std::max(1, ae - as));
    for (int i = 0; i < a.numFrames(); ++i)
    {
        float v;
        const float fi = static_cast<float>(i);
        if (fi <= A.frame.front())
            v = mapped.front() + (fi - A.frame.front()) * out.stretch;
        else if (fi >= A.frame.back())
            v = mapped.back() + (fi - A.frame.back()) * out.stretch;
        else
        {
            const auto it = std::upper_bound(A.frame.begin(), A.frame.end(), fi);
            const auto k = static_cast<size_t>(it - A.frame.begin());
            const float t = (fi - A.frame[k - 1]) / std::max(1.0e-6f, A.frame[k] - A.frame[k - 1]);
            v = mapped[k - 1] + (mapped[k] - mapped[k - 1]) * t;
        }
        out.bFrameForA[static_cast<size_t>(i)] = v;
    }
    return out;
}

//==============================================================================
ControlCurves makeControlCurves(const Fingerprint& master, float lengthSeconds, float smoothingSeconds)
{
    ControlCurves c;
    c.lengthSeconds = lengthSeconds;
    c.envelope.fill(0.0f);
    c.brightness.fill(0.5f);
    if (master.empty())
        return c;

    const auto& s = master.summary;
    const int a = s.activeStart, e = s.activeEnd, n = e - a;
    if (n < 2)
        return c;

    const int half = std::max(0, static_cast<int>(std::lround(smoothingSeconds / master.hopSeconds / 2.0)));
    auto smoothed = [&](const std::vector<float>& v)
    {
        std::vector<float> out(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            double acc = 0;
            int cnt = 0;
            for (int k = std::max(0, i - half); k <= std::min(n - 1, i + half); ++k, ++cnt)
                acc += v[static_cast<size_t>(k)];
            out[static_cast<size_t>(i)] = static_cast<float>(acc / cnt);
        }
        return out;
    };

    // Envelope: linear amplitude (smoothed in the power domain), peak 1, -60 dB -> 0.
    std::vector<float> power(static_cast<size_t>(n)), semis(static_cast<size_t>(n)), conf(static_cast<size_t>(n)), bright(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        power[static_cast<size_t>(i)] = std::pow(10.0f, master.rmsDb[static_cast<size_t>(a + i)] / 10.0f);
    power = smoothed(power);
    const float peakPow = *std::max_element(power.begin(), power.end());
    const float g60 = 0.001f;
    std::vector<float> env(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        const float g = std::sqrt(power[static_cast<size_t>(i)] / std::max(1.0e-20f, peakPow));
        env[static_cast<size_t>(i)] = std::max(0.0f, (g - g60) / (1.0f - g60));
    }

    // Pitch: semitones from the median, gaps filled by interpolation (held at the ends).
    const float med = s.medianPitchHz;
    std::vector<int> voicedIdx;
    for (int i = 0; i < n; ++i)
    {
        const float hz = master.pitchHz[static_cast<size_t>(a + i)];
        if (hz > 0.0f && med > 0.0f)
        {
            semis[static_cast<size_t>(i)] = 12.0f * std::log2(hz / med);
            conf[static_cast<size_t>(i)] = master.pitchClarity[static_cast<size_t>(a + i)];
            voicedIdx.push_back(i);
        }
    }
    if (! voicedIdx.empty())
    {
        size_t v = 0;
        for (int i = 0; i < n; ++i)
        {
            while (v + 1 < voicedIdx.size() && voicedIdx[v + 1] <= i)
                ++v;
            const int i0 = voicedIdx[v];
            if (i <= voicedIdx.front())
                semis[static_cast<size_t>(i)] = semis[static_cast<size_t>(voicedIdx.front())];
            else if (i >= voicedIdx.back())
                semis[static_cast<size_t>(i)] = semis[static_cast<size_t>(voicedIdx.back())];
            else if (i != i0)
            {
                const int i1 = voicedIdx[v + 1];
                const float t = static_cast<float>(i - i0) / static_cast<float>(i1 - i0);
                semis[static_cast<size_t>(i)] = semis[static_cast<size_t>(i0)] + (semis[static_cast<size_t>(i1)] - semis[static_cast<size_t>(i0)]) * t;
            }
        }
    }
    semis = smoothed(semis);
    conf = smoothed(conf);

    // Brightness: log centroid, 100 Hz..10 kHz -> 0..1, quiet frames interpolated from loud neighbours.
    const float gate = s.peakDb - 40.0f;
    float lastGood = -1.0f;
    for (int i = 0; i < n; ++i)
    {
        const auto u = static_cast<size_t>(a + i);
        if (master.rmsDb[u] >= gate && master.centroidHz[u] > 0.0f)
            lastGood = juce::jlimit(0.0f, 1.0f, std::log2(master.centroidHz[u] / 100.0f) / std::log2(100.0f));
        bright[static_cast<size_t>(i)] = lastGood;
    }
    {
        float first = 0.5f;
        for (auto b : bright)
            if (b >= 0.0f) { first = b; break; }
        for (auto& b : bright)
            if (b < 0.0f) b = first;
    }
    bright = smoothed(bright);

    for (int p = 0; p < ControlCurves::c_points; ++p)
    {
        const float x = static_cast<float>(p) / static_cast<float>(ControlCurves::c_points - 1) * static_cast<float>(n - 1);
        const int i = std::min(n - 2, static_cast<int>(x));
        const float t = x - static_cast<float>(i);
        auto lerp = [&](const std::vector<float>& v) { return v[static_cast<size_t>(i)] + (v[static_cast<size_t>(i + 1)] - v[static_cast<size_t>(i)]) * t; };
        const auto u = static_cast<size_t>(p);
        c.envelope[u] = lerp(env);
        c.pitchSemis[u] = lerp(semis);
        c.pitchConf[u] = lerp(conf);
        c.brightness[u] = lerp(bright);
    }
    // The envelope must start and end at zero so a flight that follows it fades in and out.
    c.envelope.front() = 0.0f;
    c.envelope.back() = 0.0f;

    const auto peakIt = std::max_element(c.envelope.begin(), c.envelope.end());
    c.peakPhase = static_cast<float>(peakIt - c.envelope.begin()) / static_cast<float>(ControlCurves::c_points - 1);
    c.suggestedShape = juce::jlimit(-1.0f, 1.0f, (c.peakPhase - 0.5f) / 0.43f);
    c.masterSeconds = static_cast<float>(n * master.hopSeconds);
    c.pitchAmount = juce::jlimit(0.0f, 1.0f, s.voicedRatio * s.meanClarity);
    c.valid = true;
    return c;
}

//==============================================================================
std::vector<RegionMatch> matchRegions(const Fingerprint& master, const Fingerprint& candidate, const std::vector<Span>& regions)
{
    std::vector<RegionMatch> out;
    if (master.empty() || candidate.empty())
        return out;

    const auto& ms = master.summary;
    const int mLen = std::max(1, ms.activeEnd - ms.activeStart);
    const double hop = candidate.hopSeconds;
    const int hopSamples = static_cast<int>(std::lround(hop * candidate.sampleRate));
    // Subsequence search at a common resolution: master at <= 80 elements, candidate at the same frames per element.
    const int step = std::max(1, (mLen + 79) / 80);
    const auto M = makeSeq(master, ms.activeStart, ms.activeEnd, (mLen + step - 1) / step);
    const float masterPeakElem = static_cast<float>(ms.peakFrame - ms.activeStart) / static_cast<float>(step);

    for (size_t r = 0; r < regions.size(); ++r)
    {
        const int f0 = juce::jlimit(0, candidate.numFrames(), regions[r].start / std::max(1, hopSamples));
        const int f1 = juce::jlimit(f0, candidate.numFrames(), (regions[r].end + hopSamples - 1) / std::max(1, hopSamples));
        if (f1 - f0 < 3)
            continue;

        RegionMatch m;
        m.region = static_cast<int>(r);
        int w0 = f0, w1 = f1;
        const auto region = slice(candidate, f0, f1);

        if (f1 - f0 > (mLen * 16) / 10)
        {
            // Long region: find the best-matching window (open begin / open end DTW).
            const auto C = makeSeq(candidate, f0, f1, (f1 - f0 + step - 1) / step);
            const auto d = dtw(M, C, 0.0f, true);
            if (! d.path.empty())
            {
                w0 = static_cast<int>(C.frame[static_cast<size_t>(d.path.front().second)]);
                w1 = static_cast<int>(C.frame[static_cast<size_t>(d.path.back().second)]) + step;
                w0 = juce::jlimit(f0, f1 - 1, w0);
                w1 = juce::jlimit(w0 + 1, f1, w1);
                // Peak: where the master's loudest element landed.
                float sum = 0;
                int cnt = 0;
                for (auto [i, j] : d.path)
                    if (std::abs(static_cast<float>(i) - masterPeakElem) < 0.5f)
                    {
                        sum += C.frame[static_cast<size_t>(j)];
                        ++cnt;
                    }
                m.peakSample = static_cast<int>(std::lround((cnt > 0 ? sum / cnt : 0.5f * (w0 + w1)) * hopSamples));
            }
        }

        const auto window = (w0 == f0 && w1 == f1) ? region : slice(candidate, w0, w1);
        m.score = similarity(master, window);
        if (w0 == f0 && w1 == f1)
        {
            const auto al = align(master, window);
            m.peakSample = static_cast<int>(std::lround((w0 + al.bFrameForA[static_cast<size_t>(ms.peakFrame)]) * hopSamples));
        }
        m.startSample = std::max(regions[r].start, w0 * hopSamples);
        m.endSample = std::min(regions[r].end, w1 * hopSamples);
        m.peakSample = juce::jlimit(m.startSample, std::max(m.startSample, m.endSample - 1), m.peakSample);
        const int wa = window.summary.activeEnd - window.summary.activeStart;
        m.stretch = static_cast<float>(wa) / static_cast<float>(mLen);
        out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [](const RegionMatch& x, const RegionMatch& y) { return x.score > y.score; });
    return out;
}
}

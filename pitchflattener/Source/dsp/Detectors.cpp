#include "Detectors.h"
#include <numeric>

namespace pf
{
    namespace
    {
        constexpr float twoPi = 6.283185307179586f;

        int nextPow2 (int v)
        {
            int p = 1;
            while (p < v)
                p <<= 1;
            return p;
        }

        int log2Int (int pow2)
        {
            int o = 0;
            while ((1 << o) < pow2)
                ++o;
            return o;
        }

        /** Offset of the vertex of a parabola through (-1, a), (0, b), (1, c). */
        float parabolicOffset (float a, float b, float c)
        {
            const float den = a - 2.0f * b + c;
            if (std::abs (den) < 1.0e-12f)
                return 0.0f;
            return std::clamp (0.5f * (a - c) / den, -0.5f, 0.5f);
        }
    }

    //==========================================================================
    RealFFT::RealFFT (int order) : fft (order), n (1 << order)
    {
        // JUCE's fallback FFT (Windows/Linux) heap-allocates above 2^14 points; stay below.
        jassert (order <= 14);
        // Measure what inverse(forward(x)) does to x so callers can normalise
        // regardless of which JUCE FFT backend is in use.
        std::vector<float> probe ((size_t) n * 2, 0.0f);
        probe[0] = 1.0f;
        fft.performRealOnlyForwardTransform (probe.data());
        fft.performRealOnlyInverseTransform (probe.data());
        roundTripScale = std::abs (probe[0]) > 1.0e-9f ? probe[0] : 1.0f;
    }

    //==========================================================================
    // pYIN
    //==========================================================================
    PYinDetector::PYinDetector (const DetectorConfig& cfg)
    {
        config = cfg;
        const double fs = cfg.analysisRate;
        tauMax = (int) std::ceil (fs / cfg.minHz) + 2;
        tauMin = std::max (2, (int) std::floor (fs / cfg.maxHz));
        window = std::max ((int) std::ceil (2.2 * fs / cfg.minHz), 256);
        window = std::min (window, cfg.maxFrameLength - tauMax - 1);
        frameLen = window + tauMax + 1;
        // YIN compares x[0..W) with x[tau..tau+W): its centre is (W + tau)/2, not frameLen/2.
        // Compensate for a typical (geometric-mean) period.
        bias = (int) std::lround (0.5 * ((double) tauMax + 1.0 - std::sqrt ((double) tauMin * (double) tauMax)));

        const int nfft = nextPow2 (frameLen);
        fft = std::make_unique<RealFFT> (log2Int (nfft));
        a.assign ((size_t) nfft * 2, 0.0f);
        b.assign ((size_t) nfft * 2, 0.0f);
        prefix.assign ((size_t) frameLen + 1, 0.0);
        diff.assign ((size_t) tauMax + 2, 0.0f);
        cmnd.assign ((size_t) tauMax + 2, 1.0f);

        thresholds.resize (100);
        thresholdPrior.resize (100);
        for (int i = 0; i < 100; ++i)
            thresholds[(size_t) i] = 0.01f * (float) (i + 1);
        updatePrior (0.5f);
    }

    void PYinDetector::updatePrior (float sensitivity)
    {
        // Beta(2, b) prior over YIN thresholds (Mauch & Dixon 2014). Mean 0.05..0.20.
        // CDF of Beta(2, b) has a closed form: 1 - (1 - x)^b (1 + b x).
        const float mean = 0.05f + 0.15f * std::clamp (sensitivity, 0.0f, 1.0f);
        const double beta = 2.0 / mean - 2.0;
        double prev = 0.0;
        for (size_t i = 0; i < thresholds.size(); ++i)
        {
            const double x = thresholds[i];
            const double cdf = 1.0 - std::pow (1.0 - x, beta) * (1.0 + beta * x);
            thresholdPrior[i] = (float) std::max (0.0, cdf - prev);
            prev = cdf;
        }
        priorForSensitivity = sensitivity;
    }

    void PYinDetector::analyse (const float* x, float sensitivity, FrameObservation& out)
    {
        if (std::abs (sensitivity - priorForSensitivity) > 1.0e-3f)
            updatePrior (sensitivity);

        const int nfft = fft->size();

        // r(t) = sum_{j<W} x[j] x[j+t] via FFT cross-correlation (no circular wrap: nfft >= W + tauMax)
        std::fill (a.begin(), a.end(), 0.0f);
        std::fill (b.begin(), b.end(), 0.0f);
        std::copy (x, x + window, a.begin());
        std::copy (x, x + frameLen, b.begin());
        fft->forward (a.data());
        fft->forward (b.data());

        for (int k = 0; k < nfft; ++k)
        {
            const float ar = a[(size_t) (2 * k)], ai = a[(size_t) (2 * k + 1)];
            const float br = b[(size_t) (2 * k)], bi = b[(size_t) (2 * k + 1)];
            a[(size_t) (2 * k)]     = ar * br + ai * bi;   // conj(A) * B
            a[(size_t) (2 * k + 1)] = ar * bi - ai * br;
        }
        fft->inverse (a.data());
        const float invScale = 1.0f / fft->roundTripScale;

        prefix[0] = 0.0;
        for (int i = 0; i < frameLen; ++i)
            prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) x[i] * (double) x[i];

        const double e0 = prefix[(size_t) window];
        if (e0 < 1.0e-12)
        {
            out.voicedProb = 0.0f;
            return;
        }

        for (int t = 0; t <= tauMax; ++t)
        {
            const double et = prefix[(size_t) (t + window)] - prefix[(size_t) t];
            diff[(size_t) t] = (float) std::max (0.0, e0 + et - 2.0 * (double) (a[(size_t) t] * invScale));
        }

        // Cumulative mean normalised difference
        cmnd[0] = 1.0f;
        double run = 0.0;
        for (int t = 1; t <= tauMax; ++t)
        {
            run += diff[(size_t) t];
            cmnd[(size_t) t] = run > 0.0 ? (float) ((double) diff[(size_t) t] * (double) t / run) : 1.0f;
        }

        // Troughs (local minima) in lag order
        struct Trough { float tau; float value; };
        std::array<Trough, 16> troughs {};
        int numTroughs = 0;
        int globalMin = tauMin;

        for (int t = std::max (tauMin, 1); t < tauMax; ++t)
        {
            if (cmnd[(size_t) t] < cmnd[(size_t) globalMin])
                globalMin = t;

            if (cmnd[(size_t) t] < cmnd[(size_t) t - 1] && cmnd[(size_t) t] <= cmnd[(size_t) t + 1])
            {
                const float off = parabolicOffset (diff[(size_t) t - 1], diff[(size_t) t], diff[(size_t) t + 1]);
                const Trough tr { (float) t + off, cmnd[(size_t) t] };

                if (numTroughs < (int) troughs.size())
                {
                    troughs[(size_t) numTroughs++] = tr;
                }
                else
                {
                    // keep the deepest troughs
                    int worst = 0;
                    for (int i = 1; i < numTroughs; ++i)
                        if (troughs[(size_t) i].value > troughs[(size_t) worst].value)
                            worst = i;
                    if (tr.value < troughs[(size_t) worst].value)
                    {
                        for (int i = worst; i < numTroughs - 1; ++i)
                            troughs[(size_t) i] = troughs[(size_t) i + 1];
                        troughs[(size_t) numTroughs - 1] = tr;
                    }
                }
            }
        }

        if (numTroughs == 0)
        {
            out.voicedProb = 0.0f;
            return;
        }

        int globalTrough = 0;
        for (int i = 1; i < numTroughs; ++i)
            if (troughs[(size_t) i].value < troughs[(size_t) globalTrough].value)
                globalTrough = i;
        juce::ignoreUnused (globalMin);

        // pYIN: marginalise the threshold over its Beta prior. Within each
        // threshold the troughs below it compete with a Boltzmann prior on rank.
        std::array<float, 16> prob {};
        for (size_t i = 0; i < thresholds.size(); ++i)
        {
            const float s = thresholds[i];
            float z = 0.0f;
            int rank = 0;
            for (int k = 0; k < numTroughs; ++k)
                if (troughs[(size_t) k].value < s)
                    z += std::exp (-2.0f * (float) rank++);

            if (rank == 0)
            {
                prob[(size_t) globalTrough] += thresholdPrior[i] * 0.01f;
                continue;
            }

            rank = 0;
            for (int k = 0; k < numTroughs; ++k)
                if (troughs[(size_t) k].value < s)
                    prob[(size_t) k] += thresholdPrior[i] * std::exp (-2.0f * (float) rank++) / z;
        }

        float voiced = 0.0f;
        for (int k = 0; k < numTroughs; ++k)
        {
            voiced += prob[(size_t) k];
            if (prob[(size_t) k] > 1.0e-3f)
                out.add ((float) (config.analysisRate / troughs[(size_t) k].tau), prob[(size_t) k]);
        }

        out.voicedProb = std::clamp (voiced, 0.0f, 1.0f);
        out.sortBySalience();
    }

    //==========================================================================
    // MPM
    //==========================================================================
    MpmDetector::MpmDetector (const DetectorConfig& cfg)
    {
        config = cfg;
        const double fs = cfg.analysisRate;
        tauMax = (int) std::ceil (fs / cfg.minHz) + 2;
        tauMin = std::max (2, (int) std::floor (fs / cfg.maxHz));
        window = std::clamp ((int) std::ceil (3.0 * fs / cfg.minHz), 512, std::min (cfg.maxFrameLength, 8192));
        tauMax = std::min (tauMax, window / 2);

        const int nfft = nextPow2 (2 * window);
        fft = std::make_unique<RealFFT> (log2Int (nfft));
        buf.assign ((size_t) nfft * 2, 0.0f);
        nsdf.assign ((size_t) window, 0.0f);
    }

    void MpmDetector::analyse (const float* x, float sensitivity, FrameObservation& out)
    {
        const int nfft = fft->size();
        std::fill (buf.begin(), buf.end(), 0.0f);
        std::copy (x, x + window, buf.begin());
        fft->forward (buf.data());
        for (int k = 0; k < nfft; ++k)
        {
            const float re = buf[(size_t) (2 * k)], im = buf[(size_t) (2 * k + 1)];
            buf[(size_t) (2 * k)] = re * re + im * im;
            buf[(size_t) (2 * k + 1)] = 0.0f;
        }
        fft->inverse (buf.data());
        const float invScale = 1.0f / fft->roundTripScale;

        double m = 0.0;
        for (int j = 0; j < window; ++j)
            m += 2.0 * (double) x[j] * (double) x[j];

        if (m < 1.0e-12)
        {
            out.voicedProb = 0.0f;
            return;
        }

        const int last = std::min (tauMax + 1, window - 1);
        nsdf[0] = 1.0f;
        for (int t = 1; t <= last; ++t)
        {
            m -= (double) x[t - 1] * x[t - 1] + (double) x[window - t] * x[window - t];
            nsdf[(size_t) t] = m > 1.0e-12 ? (float) (2.0 * buf[(size_t) t] * invScale / m) : 0.0f;
        }

        // Key maxima: the highest point of each positive lobe after the first negative zero crossing.
        struct KeyMax { float tau; float value; };
        std::array<KeyMax, 32> keys {};
        int numKeys = 0;

        int t = 1;
        while (t < last && nsdf[(size_t) t] > 0.0f)
            ++t;

        while (t < last && numKeys < (int) keys.size())
        {
            while (t < last && nsdf[(size_t) t] <= 0.0f)
                ++t;
            int best = -1;
            while (t < last && nsdf[(size_t) t] > 0.0f)
            {
                if (best < 0 || nsdf[(size_t) t] > nsdf[(size_t) best])
                    best = t;
                ++t;
            }
            if (best > 0 && best >= tauMin && best <= tauMax && best + 1 <= last)
            {
                const float off = parabolicOffset (nsdf[(size_t) best - 1], nsdf[(size_t) best], nsdf[(size_t) best + 1]);
                const float v = nsdf[(size_t) best] - 0.25f * (nsdf[(size_t) best - 1] - nsdf[(size_t) best + 1]) * off;
                keys[(size_t) numKeys++] = { (float) best + off, std::min (v, 1.0f) };
            }
        }

        if (numKeys == 0)
        {
            out.voicedProb = 0.0f;
            return;
        }

        float nmax = 0.0f;
        for (int i = 0; i < numKeys; ++i)
            nmax = std::max (nmax, keys[(size_t) i].value);

        const float k = 0.97f - 0.12f * std::clamp (sensitivity, 0.0f, 1.0f);
        int chosen = 0;
        for (int i = 0; i < numKeys; ++i)
            if (keys[(size_t) i].value >= k * nmax) { chosen = i; break; }

        const float clarity = keys[(size_t) chosen].value;
        out.add ((float) (config.analysisRate / keys[(size_t) chosen].tau), std::max (clarity, 0.01f));

        for (int i = 0; i < numKeys; ++i)
            if (i != chosen && keys[(size_t) i].value > 0.3f)
                out.add ((float) (config.analysisRate / keys[(size_t) i].tau), 0.5f * keys[(size_t) i].value);

        const float floorClarity = 0.75f - 0.35f * std::clamp (sensitivity, 0.0f, 1.0f);
        out.voicedProb = std::clamp ((clarity - floorClarity) / 0.25f, 0.0f, 1.0f);
        out.sortBySalience();
    }

    //==========================================================================
    // Spectral peaks (sirens, whistles, tones)
    //==========================================================================
    SpectralPeakDetector::SpectralPeakDetector (const DetectorConfig& cfg)
    {
        config = cfg;
        const double fs = cfg.analysisRate;
        const double scale = fs / 24000.0;

        std::vector<int> lengths;
        for (int base : { 512, 1024, 2048, 4096, 8192 })
        {
            const int len = nextPow2 ((int) std::round (base * scale));
            if (len > cfg.maxFrameLength || len * 2 > 16384)
                continue;
            if (base == 512 && cfg.minHz < 250.0f)
                continue;      // too coarse for low tones
            if (base == 8192 && cfg.minHz > 200.0f)
                continue;      // only needed for very low or very steady tones
            // a window must hold at least ~3 periods of the lowest frequency for the longest size
            lengths.push_back (len);
        }
        if (lengths.empty())
            lengths.push_back (std::min (cfg.maxFrameLength, 2048));

        frameLen = *std::max_element (lengths.begin(), lengths.end());

        for (int len : lengths)
        {
            Resolution r;
            r.length = len;
            const int nfft = len * 2;
            r.fft = std::make_unique<RealFFT> (log2Int (nfft));
            r.win.resize ((size_t) len);
            r.dwin.resize ((size_t) len);
            const float centre = 0.5f * (float) (len - 1);
            const float sigma = (float) len / 7.0f;
            for (int n = 0; n < len; ++n)
            {
                const float u = ((float) n - centre) / sigma;
                r.win[(size_t) n] = std::exp (-0.5f * u * u);
                r.dwin[(size_t) n] = -r.win[(size_t) n] * ((float) n - centre) / (sigma * sigma);
            }
            r.bufH.assign ((size_t) nfft * 2, 0.0f);
            r.bufD.assign ((size_t) nfft * 2, 0.0f);
            r.logMag.assign ((size_t) nfft / 2 + 1, -200.0f);
            r.floorDb.assign ((size_t) nfft / 2 + 1, -200.0f);
            res.push_back (std::move (r));
        }

        prefix.assign ((size_t) (frameLen + 2), 0.0);
        peaks.reserve (64);
    }

    float SpectralPeakDetector::salienceAt (float hz, int ri) const
    {
        const auto& r = res[(size_t) ri];
        const int nfft = r.length * 2;
        const float kf = hz * (float) nfft / (float) config.analysisRate;
        const int k0 = (int) std::round (kf);
        const int half = nfft / 2;
        if (k0 < 2 || k0 >= half - 2)
            return 0.0f;

        float best = 0.0f;
        for (int k = k0 - 2; k <= k0 + 2; ++k)
            best = std::max (best, r.logMag[(size_t) k] - r.floorDb[(size_t) k]);
        return best;
    }

    float SpectralPeakDetector::harmonicSupport (float hz, int ri) const
    {
        float s = 0.0f, w = 1.0f;
        const float nyq = 0.48f * (float) config.analysisRate;
        for (int h = 1; h <= 6; ++h, w *= 0.85f)
        {
            if (hz * (float) h > nyq)
                break;
            s += w * std::max (0.0f, salienceAt (hz * (float) h, ri) - 3.0f);
        }
        return s;
    }

    void SpectralPeakDetector::analyse (const float* frame, float sensitivity, FrameObservation& out)
    {
        const float thrDb = 15.0f - 9.0f * std::clamp (sensitivity, 0.0f, 1.0f);   // 15 .. 6 dB
        const double fs = config.analysisRate;
        peaks.clear();

        for (int ri = 0; ri < (int) res.size(); ++ri)
        {
            auto& r = res[(size_t) ri];
            const int len = r.length;
            const int nfft = len * 2;
            const int half = nfft / 2;
            const float* x = frame + (frameLen - len) / 2;   // all resolutions share the frame centre

            std::fill (r.bufH.begin(), r.bufH.end(), 0.0f);
            std::fill (r.bufD.begin(), r.bufD.end(), 0.0f);
            double norm = 0.0;
            for (int n = 0; n < len; ++n)
            {
                r.bufH[(size_t) n] = x[n] * r.win[(size_t) n];
                r.bufD[(size_t) n] = x[n] * r.dwin[(size_t) n];
                norm += (double) r.win[(size_t) n] * r.win[(size_t) n];
            }
            r.fft->forward (r.bufH.data());
            r.fft->forward (r.bufD.data());
            const double invNorm = 1.0 / std::max (norm, 1.0e-12);

            for (int k = 0; k <= half; ++k)
            {
                const double re = r.bufH[(size_t) (2 * k)], im = r.bufH[(size_t) (2 * k + 1)];
                r.logMag[(size_t) k] = (float) (10.0 * std::log10 ((re * re + im * im) * invNorm + 1.0e-20));
            }

            // Local spectral floor: mean dB over +-32 bins (prefix sums)
            const int span = 32;
            prefix.resize ((size_t) half + 2);
            prefix[0] = 0.0;
            for (int k = 0; k <= half; ++k)
                prefix[(size_t) k + 1] = prefix[(size_t) k] + r.logMag[(size_t) k];
            for (int k = 0; k <= half; ++k)
            {
                const int lo = std::max (0, k - span), hi = std::min (half, k + span);
                r.floorDb[(size_t) k] = (float) ((prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (double) (hi - lo + 1));
            }

            const int kMin = std::max (3, (int) std::floor (config.minHz * 0.97 * (double) nfft / fs));
            const int kMax = std::min (half - 3, (int) std::ceil (std::min (config.maxHz * 1.03, 0.48 * fs) * (double) nfft / fs));
            const float binHz = (float) (fs / nfft);

            int found = 0;
            for (int k = kMin; k <= kMax; ++k)
            {
                const float v = r.logMag[(size_t) k];
                if (v < r.logMag[(size_t) k - 1] || v < r.logMag[(size_t) k + 1]
                    || v < r.logMag[(size_t) k - 2] || v < r.logMag[(size_t) k + 2])
                    continue;
                const float sal = v - r.floorDb[(size_t) k];
                if (sal < thrDb * 0.5f)
                    continue;

                const float p = parabolicOffset (r.logMag[(size_t) k - 1], v, r.logMag[(size_t) k + 1]);
                float hz = ((float) k + p) * binHz;

                // Reassigned (instantaneous) frequency: w_hat = w_k - Im(Xdh / Xh)
                const float hr = r.bufH[(size_t) (2 * k)], hi = r.bufH[(size_t) (2 * k + 1)];
                const float dr = r.bufD[(size_t) (2 * k)], di = r.bufD[(size_t) (2 * k + 1)];
                const float mag2 = hr * hr + hi * hi;
                if (mag2 > 1.0e-20f)
                {
                    const float imRatio = (di * hr - dr * hi) / mag2;   // Im(Xd * conj(Xh)) / |Xh|^2
                    const float wk = twoPi * (float) k / (float) nfft;
                    const float hzRe = (wk - imRatio) * (float) fs / twoPi;
                    if (std::abs (hzRe - hz) < 0.75f * binHz)
                        hz = hzRe;
                }

                peaks.push_back ({ hz, sal - 0.25f * (r.logMag[(size_t) k - 1] - r.logMag[(size_t) k + 1]) * p, ri });
                if (++found >= 12)
                    break;
            }
        }

        if (peaks.empty())
        {
            out.voicedProb = 0.0f;
            return;
        }

        // Strongest peak per ~30 cent cluster across resolutions (the best-matched window wins)
        std::sort (peaks.begin(), peaks.end(), [] (const Peak& p1, const Peak& p2) { return p1.salDb > p2.salDb; });
        const float bestSal = peaks.front().salDb;

        int kept = 0;
        for (size_t i = 0; i < peaks.size() && kept < 6; ++i)
        {
            const auto& pk = peaks[i];
            bool dup = false;
            for (size_t j = 0; j < i; ++j)
                if (std::abs (hzToCents (peaks[j].hz) - hzToCents (pk.hz)) < 30.0f) { dup = true; break; }
            if (dup)
                continue;
            ++kept;

            // the peak may be a harmonic: consider it and its sub-harmonics as the fundamental
            for (int sub = 1; sub <= 3; ++sub)
            {
                const float f0 = pk.hz / (float) sub;
                if (f0 < config.minHz * 0.97f || f0 > config.maxHz * 1.03f)
                    continue;
                if (sub > 1 && salienceAt (f0, pk.res) < thrDb)
                    continue;    // the fundamental itself must be visible
                const float s = harmonicSupport (f0, pk.res);
                out.add (f0, 1.0f - std::exp (-s / 25.0f));
            }
        }

        out.voicedProb = std::clamp ((bestSal - thrDb) / 10.0f + 0.5f, 0.0f, 1.0f);
        out.sortBySalience();
    }

    //==========================================================================
    // Harmonic sum (engines, drones, field)
    //==========================================================================
    HarmonicSumDetector::HarmonicSumDetector (const DetectorConfig& cfg)
    {
        config = cfg;
        const double fs = cfg.analysisRate;
        window = std::clamp (nextPow2 ((int) std::ceil (4.0 * fs / cfg.minHz)), 1024, cfg.maxFrameLength);
        fftLen = std::min (window * 4, 16384);   // keep JUCE's fallback FFT allocation-free
        fft = std::make_unique<RealFFT> (log2Int (fftLen));

        win.resize ((size_t) window);
        for (int n = 0; n < window; ++n)
            win[(size_t) n] = 0.5f - 0.5f * std::cos (twoPi * (float) n / (float) (window - 1));

        buf.assign ((size_t) fftLen * 2, 0.0f);
        mag.assign ((size_t) fftLen / 2 + 1, 0.0f);

        logMinHz = std::max (12.0f, cfg.minHz * 0.8f);
        const float topHz = std::min (0.45f * (float) fs, std::max (cfg.maxHz * 8.0f, 2000.0f));
        numLogBins = std::max (8, (int) std::floor (binsPerOct * std::log2 (topHz / logMinHz)));

        logSpec.assign ((size_t) numLogBins, 0.0f);
        floorSpec.assign ((size_t) numLogBins, 0.0f);
        peaky.assign ((size_t) numLogBins, 0.0f);
        shs.assign ((size_t) numLogBins, 0.0f);
        prefix.assign ((size_t) numLogBins + 1, 0.0);

        for (int h = 1; h <= numHarmonics; ++h)
            harmonicOffset[(size_t) h - 1] = (int) std::lround (binsPerOct * std::log2 ((double) h));

        searchLo = std::max (0, (int) std::floor (binsPerOct * std::log2 (cfg.minHz / logMinHz)));
        searchHi = std::min (numLogBins - 2, (int) std::ceil (binsPerOct * std::log2 (cfg.maxHz / logMinHz)));
    }

    float HarmonicSumDetector::energyAt (float hz) const
    {
        const float b = binsPerOct * std::log2 (hz / logMinHz);
        const int bi = (int) std::round (b);
        if (bi < 1 || bi >= numLogBins - 1)
            return 0.0f;
        return std::max ({ peaky[(size_t) bi - 1], peaky[(size_t) bi], peaky[(size_t) bi + 1] });
    }

    void HarmonicSumDetector::analyse (const float* x, float sensitivity, FrameObservation& out)
    {
        const double fs = config.analysisRate;
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int n = 0; n < window; ++n)
            buf[(size_t) n] = x[n] * win[(size_t) n];
        fft->forward (buf.data());

        const int half = fftLen / 2;
        for (int k = 0; k <= half; ++k)
        {
            const float re = buf[(size_t) (2 * k)], im = buf[(size_t) (2 * k + 1)];
            mag[(size_t) k] = std::sqrt (re * re + im * im);
        }

        // Resample onto a log-frequency grid (interpolate where log bins are narrower than FFT bins)
        const double binsPerHz = fftLen / fs;
        const double step = std::exp2 (1.0 / binsPerOct);
        for (int b = 0; b < numLogBins; ++b)
        {
            const double f = logMinHz * std::exp2 ((double) b / binsPerOct);
            const double lo = f / std::sqrt (step) * binsPerHz;
            const double hi = f * std::sqrt (step) * binsPerHz;
            float m;
            if (hi - lo < 1.0)
            {
                const double kf = f * binsPerHz;
                const int k0 = std::min ((int) kf, half - 1);
                const float fr = (float) (kf - k0);
                m = mag[(size_t) k0] * (1.0f - fr) + mag[(size_t) k0 + 1] * fr;
            }
            else
            {
                m = 0.0f;
                for (int k = (int) std::ceil (lo); k <= std::min ((int) hi, half); ++k)
                    m = std::max (m, mag[(size_t) k]);
            }
            logSpec[(size_t) b] = 20.0f * std::log10 (m + 1.0e-9f);
        }

        // Whitening: dB above a +-1/3 octave moving average
        const int span = binsPerOct / 3;
        prefix[0] = 0.0;
        for (int b = 0; b < numLogBins; ++b)
            prefix[(size_t) b + 1] = prefix[(size_t) b] + logSpec[(size_t) b];
        float peakyMax = 0.0f;
        for (int b = 0; b < numLogBins; ++b)
        {
            const int lo = std::max (0, b - span), hi = std::min (numLogBins - 1, b + span);
            floorSpec[(size_t) b] = (float) ((prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (hi - lo + 1));
            peaky[(size_t) b] = std::max (0.0f, logSpec[(size_t) b] - floorSpec[(size_t) b] - 3.0f);
            peakyMax = std::max (peakyMax, peaky[(size_t) b]);
        }

        if (peakyMax <= 0.0f)
        {
            out.voicedProb = 0.0f;
            return;
        }

        // Subharmonic summation
        float weightSum = 0.0f;
        {
            float w = 1.0f;
            for (int h = 0; h < numHarmonics; ++h, w *= 0.84f)
                weightSum += w;
        }

        float sMax = 0.0f;
        int bMax = searchLo;
        for (int b = searchLo; b <= searchHi; ++b)
        {
            float s = 0.0f, w = 1.0f;
            for (int h = 0; h < numHarmonics; ++h, w *= 0.84f)
            {
                const int bb = b + harmonicOffset[(size_t) h];
                if (bb >= numLogBins)
                    break;
                // allow +-1 bin of inharmonicity / smearing
                const float e = std::max ({ peaky[(size_t) bb], bb > 0 ? peaky[(size_t) bb - 1] : 0.0f,
                                            bb + 1 < numLogBins ? peaky[(size_t) bb + 1] : 0.0f });
                s += w * e;
            }
            shs[(size_t) b] = s;
            if (s > sMax) { sMax = s; bMax = b; }
        }

        if (sMax <= 0.0f)
        {
            out.voicedProb = 0.0f;
            return;
        }

        const float conf = std::clamp (sMax / (weightSum * 10.0f), 0.0f, 1.0f);

        // Top local maxima as candidates
        for (int pass = 0; pass < maxCandidates; ++pass)
        {
            float best = 0.0f;
            int bi = -1;
            for (int b = std::max (searchLo, 1); b < searchHi; ++b)
            {
                const float s = shs[(size_t) b];
                if (s > best && s >= shs[(size_t) b - 1] && s >= shs[(size_t) b + 1])
                {
                    bool taken = false;
                    for (int c = 0; c < out.numCands; ++c)
                        if (std::abs (hzToCents (out.cands[(size_t) c].hz) - hzToCents (binToHz ((float) b))) < 40.0f)
                            taken = true;
                    if (! taken) { best = s; bi = b; }
                }
            }
            if (bi < 0 || best < 0.25f * sMax)
                break;
            const float off = parabolicOffset (shs[(size_t) bi - 1], shs[(size_t) bi], shs[(size_t) bi + 1]);
            out.add (binToHz ((float) bi + off), (best / sMax) * conf, 1.0f);
        }

        // Octave check (SHR-style): if the odd harmonics of f/2 carry real energy, f/2 is the fundamental.
        const float f0 = binToHz ((float) bMax);
        const float half0 = 0.5f * f0;
        if (half0 >= config.minHz)
        {
            float odd = 0.0f, even = 0.0f;
            for (int k = 1; k <= 11; k += 2) odd += energyAt (half0 * (float) k);
            for (int k = 2; k <= 12; k += 2) even += energyAt (half0 * (float) k);
            if (even > 0.0f && odd >= 0.5f * even)
            {
                // strong evidence for the sub-octave (e.g. a missing fundamental): promote it
                for (int c = 0; c < out.numCands; ++c)
                    if (std::abs (hzToCents (out.cands[(size_t) c].hz) - hzToCents (f0)) < 40.0f)
                        out.cands[(size_t) c].salience *= 0.6f;
                out.add (half0, conf, 1.0f);
            }
            else if (even > 0.0f && odd >= 0.3f * even)
            {
                out.add (half0, conf * (odd / even) * 1.4f, 1.0f);
            }
        }

        const float thr = 0.30f - 0.20f * std::clamp (sensitivity, 0.0f, 1.0f);   // 0.30 .. 0.10
        out.voicedProb = std::clamp ((conf - thr) / 0.25f + 0.5f, 0.0f, 1.0f);
        out.sortBySalience();
    }

    //==========================================================================
    // Spectral Peak + MPM
    //==========================================================================
    SpectralMpmDetector::SpectralMpmDetector (const DetectorConfig& cfg)
        : spectral (cfg), mpm (cfg)
    {
        config = cfg;
        frameLen = std::max (spectral.frameLength(), mpm.frameLength());
    }

    void SpectralMpmDetector::analyse (const float* frame, float sensitivity, FrameObservation& out)
    {
        // Both see the same moment: each gets the centre of the (longer) frame.
        FrameObservation s, m;
        s.clear();
        m.clear();
        spectral.analyse (frame + (frameLen - spectral.frameLength()) / 2, sensitivity, s);
        mpm.analyse (frame + (frameLen - mpm.frameLength()) / 2, sensitivity, m);

        for (int i = 0; i < s.numCands; ++i)
            out.add (s.cands[(size_t) i].hz, s.cands[(size_t) i].salience * s.voicedProb);
        // MPM's pitch is the sharper one on fast sweeps, so where both are confident it should win. Weighted by
        // its own confidence, it fades out in noise, where Spectral Peak's candidates carry the track.
        constexpr float mpmWeight = 4.0f;
        for (int i = 0; i < m.numCands; ++i)
            out.add (m.cands[(size_t) i].hz, m.cands[(size_t) i].salience * m.voicedProb * mpmWeight);

        out.voicedProb = std::max (s.voicedProb, m.voicedProb);
        out.sortBySalience();
    }

    //==========================================================================
    std::unique_ptr<IPitchDetector> createDetector (const DetectorConfig& cfgIn)
    {
        DetectorConfig cfg = cfgIn;
        cfg.minHz = std::clamp (cfg.minHz, 15.0f, 0.4f * (float) cfg.analysisRate);
        cfg.maxHz = std::clamp (cfg.maxHz, cfg.minHz * 1.5f, 0.45f * (float) cfg.analysisRate);

        switch (cfg.type)
        {
            case DetectorType::PYin:         return std::make_unique<PYinDetector> (cfg);
            case DetectorType::Mpm:          return std::make_unique<MpmDetector> (cfg);
            case DetectorType::HarmonicSum:  return std::make_unique<HarmonicSumDetector> (cfg);
            case DetectorType::SpectralMpm:  return std::make_unique<SpectralMpmDetector> (cfg);
            case DetectorType::SpectralPeak:
            case DetectorType::NumDetectors:
            default:                         return std::make_unique<SpectralPeakDetector> (cfg);
        }
    }
}

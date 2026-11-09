#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace pf
{
    /**
     * A plain pitch meter for checking results, independent of the detectors: the normalised square difference of
     * one window, searched from 1.5x below to 2.2x above the pitch it is expected near. Like MPM it takes the first
     * strong peak (within 10% of the best), so a signal an octave above the expected pitch reads as that octave
     * rather than as its own period doubled. Returns 0 when there is no clear periodicity there.
     *
     * scratch is resized on first use; reuse it to keep later calls allocation-free.
     */
    inline float measurePitchNear (const float* x, int len, double fs, float expectedHz, std::vector<float>& scratch)
    {
        if (! (expectedHz > 0.0f) || len < 16)
            return 0.0f;

        const int tauLo = std::max (2, (int) (fs / (expectedHz * 2.2)));
        const int tauHi = std::min (len / 2, (int) (fs / (expectedHz / 1.5)) + 2);
        if (tauHi <= tauLo + 2)
            return 0.0f;

        scratch.assign ((size_t) tauHi + 2, 0.0f);
        for (int t = tauLo - 1; t <= tauHi + 1; ++t)
        {
            double r = 0, m = 0;
            for (int j = 0; j + t < len; ++j)
            {
                r += (double) x[j] * x[j + t];
                m += (double) x[j] * x[j] + (double) x[j + t] * x[j + t];
            }
            scratch[(size_t) t] = m > 0 ? (float) (2 * r / m) : 0.0f;
        }

        const auto isPeak = [&scratch] (int t) { return scratch[(size_t) t] >= scratch[(size_t) t - 1] && scratch[(size_t) t] >= scratch[(size_t) t + 1]; };
        float highest = 0.0f;
        for (int t = tauLo; t <= tauHi; ++t)
            if (isPeak (t))
                highest = std::max (highest, scratch[(size_t) t]);
        if (highest < 0.45f)
            return 0.0f;

        int best = -1;
        for (int t = tauLo; t <= tauHi && best < 0; ++t)
            if (isPeak (t) && scratch[(size_t) t] >= 0.9f * highest)
                best = t;

        const float a = scratch[(size_t) best - 1], b = scratch[(size_t) best], c = scratch[(size_t) best + 1];
        const float den = a - 2 * b + c;
        const float off = std::abs (den) > 1e-9f ? 0.5f * (a - c) / den : 0.0f;
        return (float) (fs / (best + off));
    }

    /** Two cascaded RBJ low-passes, to take broadband noise off a copy of the signal before metering it. */
    struct MeterLowPass
    {
        void set (double fs, double hz)
        {
            const double w = 2.0 * 3.14159265358979323846 * std::min (hz, 0.45 * fs) / fs;
            const double alpha = std::sin (w) / (2.0 * 0.70710678);
            const double a0 = 1.0 + alpha;
            b0 = (1.0 - std::cos (w)) * 0.5 / a0;
            b1 = (1.0 - std::cos (w)) / a0;
            b2 = b0;
            a1 = -2.0 * std::cos (w) / a0;
            a2 = (1.0 - alpha) / a0;
            for (auto& s : z) s = 0.0;
        }

        float process (float x)
        {
            double y = x;
            for (int st = 0; st < 2; ++st)
            {
                const double in = y;
                y = b0 * in + z[st * 2];
                z[st * 2] = b1 * in - a1 * y + z[st * 2 + 1];
                z[st * 2 + 1] = b2 * in - a2 * y;
            }
            return (float) y;
        }

        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z[4] {};
    };
}

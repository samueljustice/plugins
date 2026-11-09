#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace roar
{
    /**
     * Small helpers shared by every block in dsp/common and by the modules. Plain C++, no JUCE.
     */
    namespace dsp
    {
        inline constexpr double c_pi = 3.141592653589793238462643383279502884;
        inline constexpr double c_twoPi = 6.283185307179586476925286766559005768;

        /** A usable sample rate: anything below 1 kHz (or NaN) falls back to 48 kHz. */
        inline double safeSampleRate (double fs)
        {
            return (std::isfinite (fs) && fs > 1000.0) ? fs : 48000.0;
        }

        /** One-pole smoothing coefficient a = 1 - exp(-1 / (tau fs)), for y += a (x - y). tau <= 0 gives 1 (instant). */
        inline double onePoleCoeff (double tauSeconds, double fs)
        {
            if (! (tauSeconds > 0.0))
                return 1.0;
            return 1.0 - std::exp (-1.0 / (tauSeconds * fs));
        }

        /** Pole radius alpha = exp(-1 / (tau fs)), for y = alpha y + (1 - alpha) x. tau <= 0 gives 0 (instant). */
        inline double poleFromTau (double tauSeconds, double fs)
        {
            if (! (tauSeconds > 0.0))
                return 0.0;
            return std::exp (-1.0 / (tauSeconds * fs));
        }

        inline float smoothstep01 (float t)
        {
            t = std::clamp (t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        inline double smoothstep01 (double t)
        {
            t = std::clamp (t, 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }

        /** smoothstep between edges e0 < e1. */
        inline float smoothstep (float x, float e0, float e1)
        {
            return smoothstep01 ((x - e0) / (e1 - e0));
        }

        inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }
        inline double dbToGain (double db) { return std::pow (10.0, db * 0.05); }

        /** 20 log10 |x|, floored at -200 dB (so silence never gives -inf). */
        inline float gainToDb (float g)
        {
            return 20.0f * std::log10 (std::max (std::abs (g), 1.0e-10f));
        }

        inline double gainToDb (double g)
        {
            return 20.0 * std::log10 (std::max (std::abs (g), 1.0e-10));
        }

        /** Non-finite -> 0. */
        inline float finiteOrZero (float x) { return std::isfinite (x) ? x : 0.0f; }
        inline double finiteOrZero (double x) { return std::isfinite (x) ? x : 0.0; }

        /** Flush values that would decay into denormals (recursive states). */
        inline float flushTiny (float x) { return std::abs (x) < 1.0e-20f ? 0.0f : x; }
        inline double flushTiny (double x) { return std::abs (x) < 1.0e-30 ? 0.0 : x; }

        inline int nextPowerOfTwo (int n)
        {
            int p = 1;
            while (p < n)
                p <<= 1;
            return p;
        }

        /** Samples for a duration, rounded, at least minimum. */
        inline int samplesFor (double seconds, double fs, int minimum = 0)
        {
            return std::max (minimum, static_cast<int> (std::lround (seconds * fs)));
        }

        /** Cubic Hermite (Catmull-Rom) through 4 equally spaced points, t in [0, 1) between y1 and y2. */
        inline float hermite4 (float y0, float y1, float y2, float y3, float t)
        {
            const float c1 = 0.5f * (y2 - y0);
            const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            return ((c3 * t + c2) * t + c1) * t + y1;
        }
    }
}

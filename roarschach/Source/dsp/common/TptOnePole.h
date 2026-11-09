#pragma once

#include "DspMath.h"

namespace roar
{
    /**
     * Topology-preserving-transform one-pole (trapezoidal integrator). Cutoff can change every
     * sample without zipper or instability; low-pass, high-pass and the first-order allpass come
     * out of the same state:
     *
     *   G = g / (1 + g),  g = tan (pi fc / fs)
     *   v = (x - s) G;  lp = v + s;  s = lp + v;  hp = x - lp;  ap = lp - hp
     *
     * Plain copyable value type, no allocation. Denormal-safe.
     */
    class TptOnePole
    {
    public:
        void prepare (double sampleRate) { m_fs = dsp::safeSampleRate (sampleRate); setCutoff (m_cutoff); }

        void setCutoff (double hz)
        {
            m_cutoff = hz;
            setG (gFor (hz, m_fs));
        }

        /** g = tan (pi fc / fs) directly (for per-sample modulation with a fast tan). */
        void setG (double g)
        {
            g = std::max (0.0, g);
            m_G = static_cast<float> (g / (1.0 + g));
        }

        static double gFor (double hz, double fs)
        {
            const double f = std::clamp (hz, 0.0, 0.49 * fs);
            return std::tan (dsp::c_pi * f / fs);
        }

        void reset (float state = 0.0f) { m_s = state; }

        float processLowpass (float x)
        {
            const float v = (x - m_s) * m_G;
            const float lp = v + m_s;
            m_s = dsp::flushTiny (lp + v);
            return lp;
        }

        float processHighpass (float x) { return x - processLowpass (x); }

        /** First-order allpass: 0 degrees at DC, -180 at Nyquist, -90 at the cutoff. */
        float processAllpass (float x) { return 2.0f * processLowpass (x) - x; }

        float getState() const { return m_s; }

        void copyStateFrom (const TptOnePole& other) { m_s = other.m_s; }

    private:
        double m_fs = 48000.0;
        double m_cutoff = 1000.0;
        float m_G = 0.0f;
        float m_s = 0.0f;
    };
}

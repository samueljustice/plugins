#pragma once

#include "DspMath.h"

namespace roar
{
    /**
     * Topology-preserving-transform state-variable filter (trapezoidal, Zavalishin / Simper form).
     * Safe to modulate every sample; all responses from one state:
     *
     *   g = tan (pi fc / fs), k = 1 / Q
     *   a1 = 1 / (1 + g (g + k)),  a2 = g a1,  a3 = g a2
     *   v3 = x - ic2;  v1 = a1 ic1 + a2 v3;  v2 = ic2 + a2 ic1 + a3 v3
     *   ic1 = 2 v1 - ic1;  ic2 = 2 v2 - ic2
     *   lp = v2,  bp = v1,  hp = x - k v1 - v2
     *
     * bp has a peak gain of Q at fc; bandpassNormalised() (k bp) peaks at 0 dB.
     * Plain copyable value type, no allocation, denormal-safe.
     */
    class TptSvf
    {
    public:
        struct Outputs
        {
            float lowpass = 0.0f, bandpass = 0.0f, highpass = 0.0f;
        };

        void prepare (double sampleRate) { m_fs = dsp::safeSampleRate (sampleRate); setParams (m_cutoff, m_q); }

        void setParams (double cutoffHz, double q)
        {
            m_cutoff = cutoffHz;
            m_q = std::max (q, 0.01);
            const double f = std::clamp (cutoffHz, 1.0, 0.49 * m_fs);
            setGK (std::tan (dsp::c_pi * f / m_fs), 1.0 / m_q);
        }

        /** g = tan (pi fc / fs), k = 1 / Q directly. */
        void setGK (double g, double k)
        {
            m_k = static_cast<float> (k);
            const double a1 = 1.0 / (1.0 + g * (g + k));
            m_a1 = static_cast<float> (a1);
            m_a2 = static_cast<float> (g * a1);
            m_a3 = static_cast<float> (g * g * a1);
        }

        void reset() { m_ic1 = m_ic2 = 0.0f; }

        Outputs process (float x)
        {
            const float v3 = x - m_ic2;
            const float v1 = m_a1 * m_ic1 + m_a2 * v3;
            const float v2 = m_ic2 + m_a2 * m_ic1 + m_a3 * v3;
            m_ic1 = dsp::flushTiny (2.0f * v1 - m_ic1);
            m_ic2 = dsp::flushTiny (2.0f * v2 - m_ic2);
            return { v2, v1, x - m_k * v1 - v2 };
        }

        float lowpass (float x) { return process (x).lowpass; }
        float bandpass (float x) { return process (x).bandpass; }
        float bandpassNormalised (float x) { return m_k * process (x).bandpass; }
        float highpass (float x) { return process (x).highpass; }
        float notch (float x) { const auto o = process (x); return x - m_k * o.bandpass; }
        float allpass (float x) { const auto o = process (x); return x - 2.0f * m_k * o.bandpass; }

        float getK() const { return m_k; }

        void copyStateFrom (const TptSvf& other) { m_ic1 = other.m_ic1; m_ic2 = other.m_ic2; }

    private:
        double m_fs = 48000.0;
        double m_cutoff = 1000.0, m_q = 0.7071;
        float m_k = 1.4142f, m_a1 = 0.0f, m_a2 = 0.0f, m_a3 = 0.0f;
        float m_ic1 = 0.0f, m_ic2 = 0.0f;
    };
}

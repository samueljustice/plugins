#pragma once

#include "Biquad.h"

#include <array>

namespace roar
{
    /**
     * 4th-order allpass with the poles of a Butterworth-4 at fc: two 2nd-order allpass sections
     * with Q 0.5412 and 1.3066. It is exactly the sum of an LR8 low-pass and high-pass at the same
     * fc (LP8 + HP8 = AP4), so it is the phase a band below a crossover must get to stay coherent
     * with the bands it was split from (TAME's low band, DryPhaseMatch).
     */
    class Allpass4
    {
    public:
        void prepare (double sampleRate, double crossoverHz)
        {
            m_sections[0].setCoeffs (Biquad::allpass (sampleRate, crossoverHz, Biquad::c_bw4Q1));
            m_sections[1].setCoeffs (Biquad::allpass (sampleRate, crossoverHz, Biquad::c_bw4Q2));
            reset();
        }

        void reset()
        {
            for (auto& s : m_sections)
                s.reset();
        }

        double process (double x)
        {
            return m_sections[1].process (m_sections[0].process (x));
        }

        float process (float x) { return static_cast<float> (process (static_cast<double> (x))); }

        std::complex<double> response (double hz, double fs) const
        {
            return m_sections[0].getCoeffs().response (hz, fs) * m_sections[1].getCoeffs().response (hz, fs);
        }

        void copyStateFrom (const Allpass4& other)
        {
            for (size_t i = 0; i < m_sections.size(); ++i)
                m_sections[i].copyStateFrom (other.m_sections[i]);
        }

    private:
        std::array<Biquad, 2> m_sections;
    };

    /**
     * Linkwitz-Riley 8th-order (48 dB/oct) two-way split at fc. LP8 = (Butterworth-4 low-pass)^2,
     * HP8 = (Butterworth-4 high-pass)^2, 4 sections each. low + high = Allpass4 at fc (flat
     * magnitude, zero latency); both bands are -6.02 dB and in phase at fc.
     *
     *   LR8Split split;  split.prepare (fs, 2500.0);
     *   split.process (x, low, high);
     *
     * Plain copyable value type, no allocation, state in double.
     */
    class LR8Split
    {
    public:
        void prepare (double sampleRate, double crossoverHz)
        {
            const auto lp1 = Biquad::lowpass (sampleRate, crossoverHz, Biquad::c_bw4Q1);
            const auto lp2 = Biquad::lowpass (sampleRate, crossoverHz, Biquad::c_bw4Q2);
            const auto hp1 = Biquad::highpass (sampleRate, crossoverHz, Biquad::c_bw4Q1);
            const auto hp2 = Biquad::highpass (sampleRate, crossoverHz, Biquad::c_bw4Q2);
            m_low[0].setCoeffs (lp1);
            m_low[1].setCoeffs (lp2);
            m_low[2].setCoeffs (lp1);
            m_low[3].setCoeffs (lp2);
            m_high[0].setCoeffs (hp1);
            m_high[1].setCoeffs (hp2);
            m_high[2].setCoeffs (hp1);
            m_high[3].setCoeffs (hp2);
            reset();
        }

        void reset()
        {
            for (auto& s : m_low)
                s.reset();
            for (auto& s : m_high)
                s.reset();
        }

        void process (double x, double& low, double& high)
        {
            double l = x, h = x;
            for (auto& s : m_low)
                l = s.process (l);
            for (auto& s : m_high)
                h = s.process (h);
            low = l;
            high = h;
        }

        void process (float x, float& low, float& high)
        {
            double l, h;
            process (static_cast<double> (x), l, h);
            low = static_cast<float> (l);
            high = static_cast<float> (h);
        }

        std::complex<double> lowResponse (double hz, double fs) const
        {
            std::complex<double> r (1.0, 0.0);
            for (const auto& s : m_low)
                r *= s.getCoeffs().response (hz, fs);
            return r;
        }

        std::complex<double> highResponse (double hz, double fs) const
        {
            std::complex<double> r (1.0, 0.0);
            for (const auto& s : m_high)
                r *= s.getCoeffs().response (hz, fs);
            return r;
        }

        void copyStateFrom (const LR8Split& other)
        {
            for (size_t i = 0; i < m_low.size(); ++i)
            {
                m_low[i].copyStateFrom (other.m_low[i]);
                m_high[i].copyStateFrom (other.m_high[i]);
            }
        }

    private:
        std::array<Biquad, 4> m_low, m_high;
    };
}

#pragma once

#include "DspUtils.h"

/**
 * The filters of Stowaway's Sub mode: what makes sure only the low end of whatever is sent there comes through.
 *
 *   band():   a 20 Hz high-pass (12 dB per octave), then the crossover: an eighth-order Linkwitz-Riley low-pass
 *             (two fourth-order Butterworths, 48 dB per octave: 6 dB down at the crossover, 48 dB one octave
 *             above it, 96 dB two octaves above).
 *   safety(): the same low-pass at 1.6 times the crossover, for after Low End and Drive: it is 17 dB down at twice
 *             the crossover and 44 dB down at three times, so what they add stays within about an octave of it
 *             whatever the knobs do.
 *
 * Double precision throughout (see sj::Svf64). No allocation; set() may be called every chunk as the crossover
 * glides.
 */
struct SubFilter
{
    static constexpr double c_highpassHz = 20.0;
    static constexpr double c_safetyRatio = 1.6; // the safety low-pass, as a multiple of the crossover

    void set(double crossoverHz, double sampleRate)
    {
        // A fourth-order Butterworth is two second-order sections with these Qs; two of them in a row is the
        // Linkwitz-Riley.
        static constexpr double q[4] = { 0.54119610, 1.30656296, 0.54119610, 1.30656296 };
        m_highpass.set(c_highpassHz, 0.70710678, sampleRate);
        for (size_t i = 0; i < 4; ++i)
        {
            m_crossover[i].set(crossoverHz, q[i], sampleRate);
            m_safety[i].set(c_safetyRatio * crossoverHz, q[i], sampleRate);
        }
    }

    void reset()
    {
        m_highpass.reset();
        for (auto& f : m_crossover) f.reset();
        for (auto& f : m_safety) f.reset();
    }

    double band(double x)
    {
        x = m_highpass.highpass(x);
        for (auto& f : m_crossover)
            x = f.lowpass(x);
        return x;
    }

    double safety(double x)
    {
        for (auto& f : m_safety)
            x = f.lowpass(x);
        return x;
    }

private:
    sj::Svf64 m_highpass;
    std::array<sj::Svf64, 4> m_crossover, m_safety;
};

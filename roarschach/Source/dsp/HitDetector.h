#pragma once

#include "common/DspMath.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    /**
     * Hits in the processed output, for the picture's ripples: a sudden rise of the low end over
     * what it has been doing for the last half second. A stomp, a thump, a plosive burst, the onset
     * of a roar; never the syllables of steady talking, and never a sustained tone.
     *
     * The mono sum is weighted towards the low end (two one-poles at 150 Hz, the rest 20 dB down)
     * and squared. Three followers of that power: fast (0.5 ms up, 40 ms down), mid (8 ms up, 40 ms
     * down) and slow (0.5 s both ways). A hit is armed where the fast one stands 9 dB over the slow
     * one (it is salient) and 5 dB over the mid one (it rose within a few milliseconds), above
     * -45 dB; the next 30 ms keep its peaks, and then it is counted, with a strength (strengthOf)
     * from how far it rose, how loud it got and how suddenly. A voice's syllables and the start of
     * a sung phrase rise over tens of milliseconds and are counted weak (no ring is started for
     * them); a stomp or a thump rises in a few and is counted strong, and so is the onset of a loud
     * roar however it rises. After one, 150 ms pass before the next can arm (after a weak one, none). Also published: the rumble, the low end's own level (under 120 Hz, 0.3 s), for
     * a fine tremble while it is sustained.
     *
     * Every time is in seconds and every filter is set from the sample rate, and the state is per
     * sample, so the same audio gives the same hits at any rate and in any size of block. Audio
     * thread: no allocation, no locks.
     */
    class HitDetector
    {
    public:
        void prepare (double fs)
        {
            m_fs = fs > 0.0 ? fs : 48000.0;
            const auto pole = [this] (double tau) { return static_cast<float> (dsp::poleFromTau (tau, m_fs)); };
            m_lowPole = static_cast<float> (std::exp (-2.0 * 3.14159265358979 * c_lowHz / m_fs));
            m_rumblePole = static_cast<float> (std::exp (-2.0 * 3.14159265358979 * c_rumbleHz / m_fs));
            m_fastUp = pole (0.0005);
            m_fastDown = pole (0.040);
            m_midUp = pole (0.008);
            m_midDown = pole (0.040);
            m_slowPole = pole (0.5);
            m_rumbleEnvPole = pole (0.3);
            m_windowSamples = static_cast<int> (std::lround (c_windowSeconds * m_fs));
            m_refractorySamples = static_cast<int> (std::lround (c_refractorySeconds * m_fs));
            reset();
        }

        void reset()
        {
            m_lo1 = m_lo2 = m_ru1 = m_ru2 = 0.0f;
            m_fast = m_mid = m_slow = m_rumbleEnv = 0.0f;
            m_armedLeft = -1;
            m_sinceHit = m_refractorySamples;
            m_peakJump = m_peakFast = m_peakSteep = 0.0f;
            // (the count goes on: a reader that saw it change would take a reset for a hit)
        }

        /** One run of samples, any length (r may be nullptr: mono). */
        void process (const float* l, const float* r, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                const float xl = l[i], xr = r != nullptr ? r[i] : xl;
                float x = 0.5f * (xl + xr);
                if (! std::isfinite (x))
                    x = 0.0f;
                // the low end, and the rest 20 dB down
                m_lo1 += (1.0f - m_lowPole) * (x - m_lo1);
                m_lo2 += (1.0f - m_lowPole) * (m_lo1 - m_lo2);
                const float w = m_lo2 + 0.1f * (x - m_lo2);
                const float e = w * w;
                m_fast += (1.0f - (e > m_fast ? m_fastUp : m_fastDown)) * (e - m_fast);
                m_mid += (1.0f - (e > m_mid ? m_midUp : m_midDown)) * (e - m_mid);
                m_slow += (1.0f - m_slowPole) * (e - m_slow);
                // the rumble: under 120 Hz only
                m_ru1 += (1.0f - m_rumblePole) * (x - m_ru1);
                m_ru2 += (1.0f - m_rumblePole) * (m_ru1 - m_ru2);
                m_rumbleEnv += (1.0f - m_rumbleEnvPole) * (m_ru2 * m_ru2 - m_rumbleEnv);

                if (m_sinceHit < m_refractorySamples)
                    ++m_sinceHit;
                // (ratios as powers: 9 dB is 7.9 times, 5 dB is 3.2 times; -45 dB is 3.2e-5)
                if (m_armedLeft < 0)
                {
                    if (m_sinceHit >= m_refractorySamples && m_fast > c_floorPower && m_fast > c_jumpRatio * (m_slow + 1.0e-9f)
                        && m_fast > c_steepRatio * (m_mid + 1.0e-9f))
                    {
                        m_armedLeft = m_windowSamples;
                        m_sinceHit = 0;
                        m_peakJump = m_peakFast = m_peakSteep = 0.0f;
                    }
                }
                if (m_armedLeft >= 0)
                {
                    m_peakJump = std::max (m_peakJump, m_fast / (m_slow + 1.0e-9f));
                    m_peakFast = std::max (m_peakFast, m_fast);
                    m_peakSteep = std::max (m_peakSteep, m_fast / (m_mid + 1.0e-9f));
                    if (m_armedLeft-- == 0)
                    {
                        m_strength = strengthOf (m_peakJump, m_peakFast, m_peakSteep);
                        // (a weak one holds nothing back: a syllable just before a stomp must not hide it)
                        if (m_strength < c_weak)
                            m_sinceHit = m_refractorySamples;
                        ++m_hits;
                        m_hits &= 0x3fffffff;
                    }
                }
            }
            m_lo1 = dsp::flushTiny (m_lo1);
            m_lo2 = dsp::flushTiny (m_lo2);
            m_ru1 = dsp::flushTiny (m_ru1);
            m_ru2 = dsp::flushTiny (m_ru2);
            m_fast = dsp::flushTiny (m_fast);
            m_mid = dsp::flushTiny (m_mid);
            m_slow = dsp::flushTiny (m_slow);
            m_rumbleEnv = dsp::flushTiny (m_rumbleEnv);
        }

        /** How many hits so far (wraps at 2^30). */
        int hits() const noexcept { return m_hits; }
        /** The latest hit's strength, 0..1. */
        float strength() const noexcept { return m_strength; }
        /** The low end's level, 0 (-32 dB or less) .. 1 (-12 dB). */
        float rumble() const noexcept
        {
            const float db = 10.0f * std::log10 (m_rumbleEnv + 1.0e-12f);
            return std::clamp ((db + 32.0f) / 20.0f, 0.0f, 1.0f);
        }

        /** A hit's strength, 0..1, from its peaks (powers of the weighted signal): how far it rose
            over the slow level, how loud it got, and how suddenly (the fast follower over the mid
            one). A rise of 27 dB, -12 dB loud and 10 dB sudden is a full hit; a rise over 30 ms or
            more counts only when it is loud (-18 dB and up: a roar). */
        static float strengthOf (float jump, float peak, float steep)
        {
            const auto db = [] (float x) { return 10.0f * std::log10 (std::max (x, 1.0e-12f)); };
            const float rise = std::clamp ((db (jump) - 9.0f) / 18.0f, 0.0f, 1.0f);
            const float loud = std::clamp ((db (peak) + 42.0f) / 30.0f, 0.0f, 1.0f);
            const float sudden = std::clamp ((db (steep) - 6.5f) / 3.5f, 0.0f, 1.0f);
            const float roar = std::clamp ((db (peak) + 30.0f) / 12.0f, 0.0f, 1.0f);
            return std::clamp (std::sqrt (rise) * loud * std::max (sudden, roar), 0.0f, 1.0f);
        }

        static constexpr double c_lowHz = 150.0, c_rumbleHz = 120.0;
        static constexpr double c_windowSeconds = 0.030, c_refractorySeconds = 0.150;
        static constexpr float c_weak = 0.1f;   // a hit this weak has no refractory time after it
        static constexpr float c_jumpRatio = 7.94f, c_steepRatio = 3.16f, c_floorPower = 3.16e-5f;

    private:
        double m_fs = 48000.0;
        float m_lowPole = 0.0f, m_rumblePole = 0.0f;
        float m_fastUp = 0.0f, m_fastDown = 0.0f, m_midUp = 0.0f, m_midDown = 0.0f, m_slowPole = 0.0f, m_rumbleEnvPole = 0.0f;
        float m_lo1 = 0.0f, m_lo2 = 0.0f, m_ru1 = 0.0f, m_ru2 = 0.0f;
        float m_fast = 0.0f, m_mid = 0.0f, m_slow = 0.0f, m_rumbleEnv = 0.0f;
        int m_windowSamples = 1440, m_refractorySamples = 7200;
        int m_armedLeft = -1, m_sinceHit = 0;
        float m_peakJump = 0.0f, m_peakFast = 0.0f, m_peakSteep = 0.0f;
        int m_hits = 0;
        float m_strength = 0.0f;
    };
}

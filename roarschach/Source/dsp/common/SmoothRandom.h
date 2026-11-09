#pragma once

#include "DspMath.h"

#include <cstdint>

namespace roar
{
    /**
     * Smooth random modulation: xorshift32 points at a given rate, joined by Catmull-Rom cubic
     * Hermite segments, so both the value and its slope are continuous (no stop-and-go at the
     * knots, unlike smoothstep segments).
     *
     *   SmoothRandom r;  r.prepare (fs);  r.setRate (0.2);  r.seed (1234);
     *   float v = r.process();     // -1..1: knots are uniform in +-0.8 and a Catmull-Rom segment
     *                              // overshoots its knots by at most 1.25x, so it never clips
     *
     * Deterministic for a seed. Plain copyable value type, no allocation.
     */
    class SmoothRandom
    {
    public:
        void prepare (double sampleRate)
        {
            m_fs = dsp::safeSampleRate (sampleRate);
            setRate (m_rate);
        }

        /** New random points per second (> 0). Safe to change every sample. */
        void setRate (double hz)
        {
            m_rate = std::max (hz, 1.0e-4);
            m_increment = m_rate / m_fs;
        }

        void seed (std::uint32_t s)
        {
            m_state = s != 0 ? s : 0x9e3779b9u;
            reset();
        }

        /** Fresh points from the current generator state, at t = 0. */
        void reset()
        {
            for (auto& p : m_points)
                p = c_knotScale * next();
            m_t = 0.0;
        }

        float process()
        {
            const float v = dsp::hermite4 (m_points[0], m_points[1], m_points[2], m_points[3], static_cast<float> (m_t));
            m_t += m_increment;
            while (m_t >= 1.0)
            {
                m_t -= 1.0;
                m_points[0] = m_points[1];
                m_points[1] = m_points[2];
                m_points[2] = m_points[3];
                m_points[3] = c_knotScale * next();
            }
            return std::clamp (v, -1.0f, 1.0f);
        }

        /** Uniform -1..1 from the generator (also advances it). */
        float next()
        {
            m_state ^= m_state << 13;
            m_state ^= m_state >> 17;
            m_state ^= m_state << 5;
            return static_cast<float> (static_cast<double> (m_state) * (2.0 / 4294967295.0) - 1.0);
        }

        void copyStateFrom (const SmoothRandom& other)
        {
            m_points[0] = other.m_points[0];
            m_points[1] = other.m_points[1];
            m_points[2] = other.m_points[2];
            m_points[3] = other.m_points[3];
            m_t = other.m_t;
            m_state = other.m_state;
        }

    private:
        static constexpr float c_knotScale = 0.8f;

        double m_fs = 48000.0;
        double m_rate = 1.0;
        double m_increment = 1.0 / 48000.0;
        double m_t = 0.0;
        float m_points[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        std::uint32_t m_state = 0x9e3779b9u;
    };
}

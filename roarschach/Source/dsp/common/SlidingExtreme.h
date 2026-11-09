#pragma once

#include "DspMath.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace roar
{
    /**
     * Running maximum (or minimum) over the last W samples, the newest included: a monotonic
     * deque held in a fixed power-of-two ring. Amortised O(1) per sample; the deque never holds
     * more than W entries, so the ring never overflows.
     *
     *   SlidingExtreme peak (SlidingExtreme::Kind::max);
     *   peak.prepare (maxWindow);         // allocates
     *   peak.setWindow (w);               // 1..maxWindow, no allocation
     *   y = peak.process (x);             // max of x[n-w+1 .. n]
     *
     * Until W samples have been seen the result covers the samples so far. Non-finite input is
     * taken as 0. copyStateFrom() copies a deque prepared with the same maximum window.
     */
    class SlidingExtreme
    {
    public:
        enum class Kind { max, min };

        explicit SlidingExtreme (Kind kind = Kind::max) : m_isMax (kind == Kind::max) {}

        void prepare (int maxWindow)
        {
            m_maxWindow = std::max (1, maxWindow);
            const int capacity = dsp::nextPowerOfTwo (m_maxWindow + 1);
            m_values.assign (static_cast<size_t> (capacity), 0.0f);
            m_stamps.assign (static_cast<size_t> (capacity), 0u);
            m_mask = static_cast<std::uint32_t> (capacity - 1);
            m_window = std::clamp (m_window, 1, m_maxWindow);
            reset();
        }

        void setWindow (int windowSamples) { m_window = std::clamp (windowSamples, 1, m_maxWindow); }
        int getWindow() const { return m_window; }

        void reset()
        {
            m_head = m_tail = 0;
            m_clock = 0;
        }

        float process (float x)
        {
            x = dsp::finiteOrZero (x);

            // Drop the entries the new sample dominates.
            while (m_tail != m_head)
            {
                const float back = m_values[static_cast<size_t> ((m_tail - 1u) & m_mask)];
                if (m_isMax ? (back > x) : (back < x))
                    break;
                --m_tail;
            }

            m_values[static_cast<size_t> (m_tail & m_mask)] = x;
            m_stamps[static_cast<size_t> (m_tail & m_mask)] = m_clock;
            ++m_tail;

            // Expire the front (older than the window).
            const auto window = static_cast<std::uint32_t> (m_window);
            while (m_clock - m_stamps[static_cast<size_t> (m_head & m_mask)] >= window)
                ++m_head;

            ++m_clock;
            return m_values[static_cast<size_t> (m_head & m_mask)];
        }

        /** The current extreme (last process() result); 0 before any input. */
        float getValue() const
        {
            return m_tail != m_head ? m_values[static_cast<size_t> (m_head & m_mask)] : 0.0f;
        }

        void copyStateFrom (const SlidingExtreme& other)
        {
            assert (other.m_values.size() == m_values.size());
            const size_t n = std::min (m_values.size(), other.m_values.size());
            std::copy (other.m_values.begin(), other.m_values.begin() + static_cast<std::ptrdiff_t> (n), m_values.begin());
            std::copy (other.m_stamps.begin(), other.m_stamps.begin() + static_cast<std::ptrdiff_t> (n), m_stamps.begin());
            m_head = other.m_head;
            m_tail = other.m_tail;
            m_clock = other.m_clock;
            m_window = std::clamp (other.m_window, 1, m_maxWindow);
            m_isMax = other.m_isMax;
        }

    private:
        bool m_isMax = true;
        std::vector<float> m_values;
        std::vector<std::uint32_t> m_stamps;
        std::uint32_t m_mask = 0;
        std::uint32_t m_head = 0, m_tail = 0;   // free-running; index = value & mask
        std::uint32_t m_clock = 0;
        int m_window = 1;
        int m_maxWindow = 1;
    };
}

#pragma once

#include "DspMath.h"

#include <algorithm>
#include <cassert>
#include <vector>

namespace roar
{
    /**
     * Mono delay line on a power-of-two ring. push() one sample, then read back by delay: read(0)
     * is the sample just pushed, read(d) the one pushed d samples earlier.
     *
     *   - Integer read: read(d), 0 <= d <= getMaxDelay().
     *   - Fractional reads: readLinear(d) and readHermite(d) (4-point cubic Hermite). Hermite needs
     *     1 <= d <= getMaxDelay() - 2, so keep 1 sample clear of the write position for a modulated
     *     tap and size the line with prepare(maxDelay + 2).
     *   - processFixed(x): push x and return the sample setFixedDelay() samples old. A fixed
     *     delay of 0 returns x.
     *
     * prepare() allocates; nothing else does. copyStateFrom() copies the ring of a line prepared
     * with the same maximum delay (std::copy, no allocation). Non-finite input is stored as 0.
     */
    class DelayLine
    {
    public:
        void prepare (int maxDelaySamples)
        {
            m_maxDelay = std::max (0, maxDelaySamples);
            const int length = dsp::nextPowerOfTwo (m_maxDelay + 1);
            m_buffer.assign (static_cast<size_t> (length), 0.0f);
            m_mask = length - 1;
            m_fixedDelay = std::min (m_fixedDelay, m_maxDelay);
            reset();
        }

        void reset()
        {
            std::fill (m_buffer.begin(), m_buffer.end(), 0.0f);
            m_write = 0;
        }

        int getMaxDelay() const { return m_maxDelay; }

        void setFixedDelay (int delaySamples) { m_fixedDelay = std::clamp (delaySamples, 0, m_maxDelay); }
        int getFixedDelay() const { return m_fixedDelay; }

        void push (float x)
        {
            m_write = (m_write + 1) & m_mask;
            m_buffer[static_cast<size_t> (m_write)] = dsp::finiteOrZero (x);
        }

        float read (int delaySamples) const
        {
            assert (delaySamples >= 0 && delaySamples <= m_maxDelay);
            return m_buffer[static_cast<size_t> ((m_write - delaySamples) & m_mask)];
        }

        float readLinear (float delaySamples) const
        {
            const float d = std::clamp (delaySamples, 0.0f, static_cast<float> (m_maxDelay));
            const int i = static_cast<int> (d);
            const float t = d - static_cast<float> (i);
            const float a = read (i);
            const float b = read (std::min (i + 1, m_maxDelay));
            return a + t * (b - a);
        }

        float readHermite (float delaySamples) const
        {
            const float d = std::clamp (delaySamples, 1.0f, static_cast<float> (std::max (1, m_maxDelay - 2)));
            const int i = static_cast<int> (d);
            const float t = d - static_cast<float> (i);
            // Newer to older: y0 = d-1, y1 = d, y2 = d+1, y3 = d+2.
            const auto at = [this] (int k) { return m_buffer[static_cast<size_t> ((m_write - k) & m_mask)]; };
            return dsp::hermite4 (at (i - 1), at (i), at (i + 1), at (i + 2), t);
        }

        float processFixed (float x)
        {
            push (x);
            return read (m_fixedDelay);
        }

        void processFixed (float* io, int numSamples)
        {
            for (int i = 0; i < numSamples; ++i)
                io[i] = processFixed (io[i]);
        }

        void copyStateFrom (const DelayLine& other)
        {
            assert (other.m_buffer.size() == m_buffer.size());
            const size_t n = std::min (m_buffer.size(), other.m_buffer.size());
            std::copy (other.m_buffer.begin(), other.m_buffer.begin() + static_cast<std::ptrdiff_t> (n), m_buffer.begin());
            m_write = other.m_write & m_mask;
            m_fixedDelay = std::min (other.m_fixedDelay, m_maxDelay);
        }

    private:
        std::vector<float> m_buffer;
        int m_mask = 0;
        int m_write = 0;
        int m_maxDelay = 0;
        int m_fixedDelay = 0;
    };
}

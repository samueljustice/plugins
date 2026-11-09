#pragma once

#include "DspMath.h"

#include <algorithm>
#include <cassert>
#include <vector>

namespace roar
{
    /**
     * Two cascaded moving averages (box filters) with a total span S: the impulse response is a
     * trapezoid (a triangle for odd S) S samples long, unity DC gain, delay (S - 1) / 2. Used to
     * smooth the look-ahead maximizer's gain after its sliding minimum (spec §5.14): with S = L + 1
     * the smoothed gain at the look-ahead-delayed sample is never above the gain that sample needs.
     *
     *   lengths: L1 = ceil ((S + 1) / 2), L2 = S + 1 - L1
     *
     * Running sums are held in double and recomputed exactly each time a ring wraps, so there is
     * no drift however long it runs. prepare() allocates; setSpan() and process() do not.
     * reset (v) fills both rings with v (a gain of 1 starts at unity, not at silence).
     */
    class BoxAverage2
    {
    public:
        void prepare (int maxSpan)
        {
            m_maxSpan = std::max (1, maxSpan);
            const int maxLength = (m_maxSpan + 2) / 2 + 1;
            m_a.buffer.assign (static_cast<size_t> (maxLength), 0.0);
            m_b.buffer.assign (static_cast<size_t> (maxLength), 0.0);
            setSpan (m_span > 0 ? std::min (m_span, m_maxSpan) : m_maxSpan);
        }

        /** Total span S (1..maxSpan). Resets the rings to the last output value. */
        void setSpan (int span)
        {
            m_span = std::clamp (span, 1, m_maxSpan);
            m_a.length = (m_span + 2) / 2;
            m_b.length = m_span + 1 - m_a.length;
            reset (m_last);
        }

        int getSpan() const { return m_span; }

        /** Group delay in samples, (S - 1) / 2. */
        double getDelay() const { return 0.5 * static_cast<double> (m_span - 1); }

        void reset (double value = 0.0)
        {
            m_last = value;
            m_a.fill (value);
            m_b.fill (value);
        }

        double process (double x)
        {
            x = dsp::finiteOrZero (x);
            m_last = m_b.process (m_a.process (x));
            return m_last;
        }

        void copyStateFrom (const BoxAverage2& other)
        {
            assert (other.m_a.buffer.size() == m_a.buffer.size());
            m_a.copyFrom (other.m_a);
            m_b.copyFrom (other.m_b);
            m_span = other.m_span;
            m_last = other.m_last;
        }

    private:
        struct Box
        {
            std::vector<double> buffer;
            int length = 1;
            int pos = 0;
            double sum = 0.0;

            void fill (double value)
            {
                std::fill (buffer.begin(), buffer.end(), value);
                pos = 0;
                sum = value * static_cast<double> (length);
            }

            double process (double x)
            {
                auto& slot = buffer[static_cast<size_t> (pos)];
                sum += x - slot;
                slot = x;
                if (++pos >= length)
                {
                    pos = 0;
                    double exact = 0.0;
                    for (int i = 0; i < length; ++i)
                        exact += buffer[static_cast<size_t> (i)];
                    sum = exact;
                }
                return sum / static_cast<double> (length);
            }

            void copyFrom (const Box& other)
            {
                const size_t n = std::min (buffer.size(), other.buffer.size());
                std::copy (other.buffer.begin(), other.buffer.begin() + static_cast<std::ptrdiff_t> (n), buffer.begin());
                length = other.length;
                pos = other.pos;
                sum = other.sum;
            }
        };

        Box m_a, m_b;
        int m_span = 0;
        int m_maxSpan = 1;
        double m_last = 0.0;
    };
}

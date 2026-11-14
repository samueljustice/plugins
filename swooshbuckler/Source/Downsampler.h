#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

/**
 * Band-limited downsampler for the monitor path: when the rig renders above the
 * device rate (a high Record Rate), this is what turns the render into what you
 * hear and what the host gets.
 *
 * Each output sample is read from the input with a Kaiser-windowed sinc whose
 * cutoff sits just under the output's Nyquist frequency, so what the input holds
 * above that is removed instead of folding back into the audible band. Any ratio
 * of 1 or more works (2:1 and 4:1 as well as 96 kHz into 44.1 kHz).
 *
 * Output sample k is centred on input sample k * ratio, so the filter itself
 * adds no delay to the stream; what it needs is input ahead of the output, and
 * getLatencySamples() says how far ahead in output samples.
 *
 * No JUCE dependency, no allocation after prepare().
 */
class Downsampler
{
public:
    /** ratio = input rate / output rate (1 or more). */
    void prepare(double ratio, int numChannels)
    {
        m_ratio = std::max(1.0, ratio);
        m_numChannels = std::max(1, numChannels);

        // The sinc's zero crossings are 1 / cutoff input samples apart (cutoff as a fraction of the input's
        // Nyquist frequency), and the kernel reaches c_zeros of them either side.
        m_cutoff = c_guard / m_ratio;
        m_half = static_cast<int>(std::ceil(c_zeros / m_cutoff));

        // For a whole-number ratio, make the input the filter needs ahead a whole number of output samples.
        const int whole = static_cast<int>(std::lround(m_ratio));
        if (std::abs(m_ratio - whole) < 1.0e-9)
            while ((m_half + 1) % whole != 0)
                ++m_half;

        m_taps = 2 * m_half;

        const int tableSize = m_half * c_resolution + 2;
        m_table.assign(static_cast<size_t>(tableSize), 0.0f);
        const double norm = besselI0(c_beta);
        for (int i = 0; i <= m_half * c_resolution; ++i)
        {
            const double x = static_cast<double>(i) / c_resolution; // distance in input samples
            const double px = 3.141592653589793 * m_cutoff * x;
            const double sinc = i == 0 ? 1.0 : std::sin(px) / px;
            const double r = x / m_half;
            m_table[static_cast<size_t>(i)] = static_cast<float>(sinc * besselI0(c_beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / norm);
        }

        m_weights.assign(static_cast<size_t>(m_taps), 0.0f);
        m_history.assign(static_cast<size_t>(m_numChannels), std::vector<float>(static_cast<size_t>(2 * (m_taps + 1)), 0.0f));
        reset();
    }

    void reset()
    {
        for (auto& h : m_history)
            std::fill(h.begin(), h.end(), 0.0f);
        m_position = 0;
        m_until = -static_cast<double>(m_half + 1); // output 0 is centred on input 0
        m_weightsFor = -1.0;
    }

    /** How many input samples the next numOut output samples will use. */
    int inputsNeeded(int numOut) const
    {
        double until = m_until;
        int count = 0;
        for (int k = 0; k < numOut; ++k)
        {
            while (until < 0.0)
            {
                until += 1.0;
                ++count;
            }
            until -= m_ratio;
        }
        return count;
    }

    /** The most input samples numOut output samples can use. */
    int maxInputsFor(int numOut) const { return static_cast<int>(std::ceil(numOut * m_ratio)) + m_half + 2; }

    /** Writes numOut output samples per channel from exactly inputsNeeded(numOut) input samples. */
    void process(const float* const* in, float* const* out, int numOut)
    {
        const int length = m_taps + 1; // the history holds one sample more than the kernel reads
        int used = 0;

        for (int k = 0; k < numOut; ++k)
        {
            while (m_until < 0.0)
            {
                for (int ch = 0; ch < m_numChannels; ++ch)
                {
                    float* h = m_history[static_cast<size_t>(ch)].data();
                    h[m_position] = h[m_position + length] = in[ch][used];
                }
                if (++m_position == length)
                    m_position = 0;
                ++used;
                m_until += 1.0;
            }

            // The output sits m_until (0 or more, under 1) input samples before the middle of the history.
            if (std::not_equal_to<double>()(m_until, m_weightsFor)) // the same every time for a whole-number ratio
            {
                m_weightsFor = m_until;
                float total = 0.0f;
                for (int j = 0; j < m_taps; ++j)
                {
                    const double a = std::abs(static_cast<double>(j - m_half) + m_until) * c_resolution;
                    const int index = static_cast<int>(a);
                    const float frac = static_cast<float>(a - index);
                    const float* t = m_table.data() + index;
                    const float w = t[0] + frac * (t[1] - t[0]);
                    m_weights[static_cast<size_t>(j)] = w;
                    total += w;
                }
                const float scale = 1.0f / total; // a steady level comes out as itself
                for (auto& w : m_weights)
                    w *= scale;
            }

            for (int ch = 0; ch < m_numChannels; ++ch)
            {
                const float* window = m_history[static_cast<size_t>(ch)].data() + m_position; // oldest first
                float sums[4] {};
                int j = 0;
                for (; j + 4 <= m_taps; j += 4)
                    for (int q = 0; q < 4; ++q)
                        sums[q] += window[j + q] * m_weights[static_cast<size_t>(j + q)];
                float sum = (sums[0] + sums[1]) + (sums[2] + sums[3]);
                for (; j < m_taps; ++j)
                    sum += window[j] * m_weights[static_cast<size_t>(j)];
                out[ch][k] = sum;
            }

            m_until -= m_ratio;
        }
    }

    /** How far ahead of the output the input has to be, in output samples: what is fed in at the start of a
        process() call is heard this much later. A whole number for a whole-number ratio. */
    double getLatencySamples() const { return (m_half + 1) / m_ratio - 1.0; }

    /** The cutoff as a fraction of the output's Nyquist frequency (the -6 dB point). */
    static constexpr double c_guard = 0.92;

private:
    static constexpr int c_zeros = 24;        // zero crossings of the sinc either side
    static constexpr int c_resolution = 128;  // table points per input sample (read with linear interpolation)
    static constexpr double c_beta = 8.6;     // Kaiser window: about -85 dB outside the transition band

    static double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 60; ++k)
        {
            term *= (0.5 * x / k) * (0.5 * x / k);
            sum += term;
        }
        return sum;
    }

    double m_ratio = 1.0, m_cutoff = 1.0;
    int m_numChannels = 1, m_half = 1, m_taps = 2;
    std::vector<float> m_table, m_weights;
    std::vector<std::vector<float>> m_history; // per channel, written twice so a window is contiguous
    int m_position = 0;
    double m_until = 0.0;      // (newest input index - m_half) - the next output's centre; an output is due at >= 0
    double m_weightsFor = -1.0;
};

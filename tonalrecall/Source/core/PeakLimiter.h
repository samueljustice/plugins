#pragma once

// The safety at the end of Stamp: a trim (one fixed gain) followed by a look-ahead peak limiter
// that never lets a sample past the ceiling.
//
// The trim is the main safety and is set from outside (a measured figure: see
// core::measureStamp). The limiter is the backstop for what could not be measured beforehand,
// and with a good trim it does nothing: below the ceiling the audio passes through unchanged,
// only delayed.
//
// How the limiter keeps its promise. For each sample it works out the gain that sample needs
// (1 below the ceiling). That is eased back up over the release time, then held at its lowest
// over the look-ahead (a sliding minimum), then averaged over the same length. The audio is
// delayed by the look-ahead. Every value in the average that is applied to a sample is a minimum
// over a stretch that contains that sample's own need, so the gain applied is never above what
// the sample needs, and the average makes the gain arrive as a ramp instead of a step.
//
// The channels are limited together (one gain), so the stereo picture does not move.
//
// No JUCE. Construction allocates; process() does not. setTrim() and setEnabled() may be called
// from another thread than process().

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace tonalrecall::core
{

class PeakLimiter
{
public:
    static constexpr float kCeiling = 0.98855309f;   // -0.1 dBFS

    PeakLimiter(double sampleRate, int channels, float ceiling = kCeiling)
        : m_ceiling(ceiling),
          m_lookahead(std::max(8, static_cast<int>(std::lround(sampleRate * 0.002)))),
          m_span(static_cast<std::size_t>(m_lookahead) + 1)
    {
        const auto rate = std::max(1.0, sampleRate);
        m_release = static_cast<float>(1.0 - std::exp(-1.0 / (0.08 * rate)));
        m_trimStep = static_cast<float>(1.0 / (0.02 * rate));   // a full-scale change of trim takes 20 ms
        m_delay.assign(static_cast<std::size_t>(std::max(1, channels)), std::vector<float>(static_cast<std::size_t>(m_lookahead), 0.0f));
        m_minima.assign(m_span, 1.0f);
        m_sum = static_cast<double>(m_span);   // the average starts at 1
        m_dequeValue.assign(m_span + 1, 1.0f);
        m_dequeAt.assign(m_span + 1, 0);
    }

    /** The audio leaves this many samples late, always (enabled or not). */
    int delaySamples() const noexcept { return m_lookahead; }
    float ceiling() const noexcept { return m_ceiling; }

    /** The fixed gain ahead of the limiter (1 = none). Changes are eased in. */
    void setTrim(float gain) noexcept { m_trimTarget.store(std::clamp(gain, 0.0f, 1.0f), std::memory_order_relaxed); }
    /** Off: a plain delay (no trim, no limiting), for measuring what the limiter would be given. */
    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }

    /** The most the limiter has turned the audio down so far, in dB (0 = it has done nothing). */
    float maxReductionDb() const noexcept
    {
        const float g = m_lowestGain.load(std::memory_order_relaxed);
        return g >= 1.0f ? 0.0f : -20.0f * std::log10(std::max(g, 1.0e-6f));
    }

    /** In place; any block size. */
    void process(float* const* channels, int numSamples) noexcept
    {
        const auto numChannels = m_delay.size();
        const bool enabled = m_enabled.load(std::memory_order_relaxed);
        const float trimTarget = enabled ? m_trimTarget.load(std::memory_order_relaxed) : 1.0f;
        if (! m_trimSet)
        {
            m_trim = trimTarget;   // the first block starts at the trim, it does not fade to it
            m_trimSet = true;
        }
        float lowest = m_lowestGain.load(std::memory_order_relaxed);
        const float lowestBefore = lowest;

        for (int i = 0; i < numSamples; ++i)
        {
            if (m_trim < trimTarget)
                m_trim = std::min(trimTarget, m_trim + m_trimStep);
            else if (m_trim > trimTarget)
                m_trim = std::max(trimTarget, m_trim - m_trimStep);

            // What this sample needs.
            float peak = 0.0f;
            for (std::size_t c = 0; c < numChannels; ++c)
                peak = std::max(peak, std::abs(channels[c][i] * m_trim));
            float need = 1.0f;
            if (enabled && peak > m_ceiling)
                need = m_ceiling / peak;
            if (! (need >= 0.0f))   // NaN in the input: silence it
                need = 0.0f;

            // Eased back up after a peak, never above what the newest sample needs.
            m_eased = std::min(need, m_eased + (1.0f - m_eased) * m_release);

            // Its lowest over the look-ahead: a queue of candidates in rising order, the front
            // being the minimum of the last m_span values.
            while (m_dequeCount > 0 && m_dequeValue[dequeIndex(m_dequeCount - 1)] >= m_eased)
                --m_dequeCount;
            m_dequeValue[dequeIndex(m_dequeCount)] = m_eased;
            m_dequeAt[dequeIndex(m_dequeCount)] = m_count;
            ++m_dequeCount;
            if (m_dequeAt[m_dequeHead] + static_cast<long long>(m_span) <= m_count)
            {
                m_dequeHead = (m_dequeHead + 1) % m_dequeValue.size();
                --m_dequeCount;
            }
            const float minimum = m_dequeValue[m_dequeHead];

            // The average of those minima over the same length.
            m_sum += static_cast<double>(minimum) - static_cast<double>(m_minima[m_at]);
            m_minima[m_at] = minimum;

            m_at = (m_at + 1) % m_span;
            if (m_at == 0)
            {
                // Once round the ring: add the minima up afresh, so rounding never builds up.
                double sum = 0.0;
                for (const float v : m_minima)
                    sum += static_cast<double>(v);
                m_sum = sum;
            }
            float gain = static_cast<float>(m_sum / static_cast<double>(m_span));
            gain = std::clamp(gain, 0.0f, 1.0f);
            lowest = std::min(lowest, gain);

            for (std::size_t c = 0; c < numChannels; ++c)
            {
                // The slot about to be overwritten holds the sample from m_lookahead ago.
                float& slot = m_delay[c][m_delayAt];
                const float out = slot * gain;
                slot = channels[c][i] * m_trim;
                // The gain already holds it; the clamp only covers rounding in the last bit.
                channels[c][i] = enabled ? std::clamp(out, -m_ceiling, m_ceiling) : out;
            }

            m_delayAt = (m_delayAt + 1) % static_cast<std::size_t>(m_lookahead);
            ++m_count;
        }

        if (lowest < lowestBefore)
            m_lowestGain.store(lowest, std::memory_order_relaxed);
    }

private:
    std::size_t dequeIndex(std::size_t offset) const noexcept { return (m_dequeHead + offset) % m_dequeValue.size(); }

    const float m_ceiling;
    const int m_lookahead;
    const std::size_t m_span;            // look-ahead + 1: the length of the minimum and of the average
    float m_release = 0.0f;
    float m_trimStep = 0.0f;

    std::atomic<float> m_trimTarget { 1.0f };
    std::atomic<bool> m_enabled { true };
    std::atomic<float> m_lowestGain { 1.0f };

    float m_trim = 1.0f;
    bool m_trimSet = false;
    float m_eased = 1.0f;

    std::vector<std::vector<float>> m_delay;   // the audio (after the trim), m_lookahead samples of it
    std::size_t m_delayAt = 0;
    std::vector<float> m_minima;         // the last m_span sliding minima
    double m_sum = 0.0;
    std::size_t m_at = 0;
    long long m_count = 0;

    std::vector<float> m_dequeValue;
    std::vector<long long> m_dequeAt;
    std::size_t m_dequeHead = 0, m_dequeCount = 0;
};

} // namespace tonalrecall::core

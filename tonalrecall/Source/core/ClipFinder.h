#pragma once

// Tells whether the audio streaming past is a clip that is already known, sample for sample.
//
// Tonal Recall uses it to recognise the learned music when it is played or rendered with Stamp
// on: the music is the thing being matched to, so it is never shifted itself.
//
// How: every so often a short window of the stream is taken, starting at a sample that has some
// level, and looked for in the clip at every position. The same clip played again gives the same
// samples, so nearly every position is ruled out by its first sample and a look takes a
// millisecond or two even for a minute of audio. It works wherever in the clip the stream starts
// (a different selection of the same music), and when other audio comes first (a render's handle).
// After a few seconds without a match it stops looking.
//
// No JUCE, no allocation after construction. The clip is not owned: the caller keeps it alive
// from begin() until the next begin() or end().

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tonalrecall::core
{

class ClipFinder
{
public:
    using Channels = std::vector<std::vector<float>>;

    /** Starts looking for clip (may be null: then nothing is ever found) in a new stream. */
    void begin(const Channels* clip, double sampleRate) noexcept
    {
        m_clip = clip;
        m_found = false;
        m_fill = -1;
        m_seen = 0;
        m_nextProbe = 0;
        m_giveUpAt = static_cast<std::int64_t>(kSecondsLooking * std::max(1.0, sampleRate));
        m_length = 0;
        m_peak = 0.0f;
        if (clip == nullptr || clip->empty() || clip->size() > 2)
        {
            m_clip = nullptr;
            return;
        }
        m_length = (*clip)[0].size();
        for (const auto& channel : *clip)
        {
            m_length = std::min(m_length, channel.size());
            for (const float v : channel)
                m_peak = std::max(m_peak, std::abs(v));
        }
        if (m_length < kWindow || m_peak <= 0.0f)
            m_clip = nullptr;
    }

    void end() noexcept { m_clip = nullptr; }

    /** True once the stream has been recognised as the clip (and from then on). */
    bool found() const noexcept { return m_found; }
    /** Nothing more will be looked for (found, given up, or there is no clip). */
    bool finished() const noexcept { return m_found || m_clip == nullptr || m_seen >= m_giveUpAt; }
    /** Stops looking (the stream has turned out to be something else). */
    void stop() noexcept { if (! m_found) m_clip = nullptr; }

    /** The next block of the stream; numChannels must match the clip's for anything to be found. */
    void feed(const float* const* channels, int numChannels, int numSamples) noexcept
    {
        if (finished())
            return;
        if (numChannels != static_cast<int>(m_clip->size()))
        {
            m_clip = nullptr;   // another width: not the same audio
            return;
        }

        const float gate = m_peak * 0.02f * static_cast<float>(numChannels);
        for (int i = 0; i < numSamples && ! m_found; ++i, ++m_seen)
        {
            float sum = 0.0f;
            for (int c = 0; c < numChannels; ++c)
                sum += channels[c][i];

            if (m_fill < 0)
            {
                // Waiting for a place to start a window: far enough on from the last one, and
                // at a sample with some level (silence would match silence anywhere).
                if (m_seen < m_nextProbe || std::abs(sum) < gate)
                    continue;
                m_fill = 0;
            }
            m_window[static_cast<std::size_t>(m_fill++)] = sum;
            if (m_fill == static_cast<int>(kWindow))
            {
                m_found = search();
                m_fill = -1;
                m_nextProbe = m_seen + kSpacing;
            }
        }
    }

private:
    static constexpr std::size_t kWindow = 1024;
    static constexpr std::int64_t kSpacing = 4096;
    static constexpr double kSecondsLooking = 6.0;

    float clipSum(std::size_t at) const noexcept
    {
        float sum = 0.0f;
        for (const auto& channel : *m_clip)
            sum += channel[at];
        return sum;
    }

    bool search() const noexcept
    {
        // The same samples, give or take what a host's own gain stage rounds off.
        const float tolerance = m_peak * 1.0e-4f * static_cast<float>(m_clip->size());
        const float first = m_window[0];
        for (std::size_t at = 0; at + kWindow <= m_length; ++at)
        {
            if (std::abs(clipSum(at) - first) > tolerance)
                continue;
            bool same = true;
            for (std::size_t i = 1; i < kWindow && same; ++i)
                same = std::abs(clipSum(at + i) - m_window[i]) <= tolerance;
            if (same)
                return true;
        }
        return false;
    }

    const Channels* m_clip = nullptr;
    std::size_t m_length = 0;
    float m_peak = 0.0f;
    std::array<float, kWindow> m_window {};
    int m_fill = -1;                    // -1: waiting to start a window
    std::int64_t m_seen = 0, m_nextProbe = 0, m_giveUpAt = 0;
    bool m_found = false;
};

} // namespace tonalrecall::core

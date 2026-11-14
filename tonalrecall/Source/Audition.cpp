#include "Audition.h"
#include "core/RubberBandDriver.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace tonalrecall
{

namespace
{
    std::int64_t nowNs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    constexpr std::size_t kFifoSize = 32768;
}

AuditionBus::AuditionBus() = default;
AuditionBus::~AuditionBus() = default;

AuditionBus& AuditionBus::instance()
{
    static AuditionBus bus;
    return bus;
}

void AuditionBus::setSource(std::shared_ptr<const Channels> channels, double sampleRate, bool preserveFormants)
{
    const std::lock_guard<std::mutex> lock(m_lock);
    const int count = channels != nullptr ? static_cast<int>(std::min<std::size_t>(2, channels->size())) : 0;
    const bool sameSound = channels == m_source && std::abs(sampleRate - m_rate) < 0.5;

    if (! sameSound)
    {
        m_active = false;
        m_gain = 0.0f;
        m_source = std::move(channels);
        m_sourceChannels = count;
        m_length = count > 0 ? static_cast<std::int64_t>((*m_source)[0].size()) : 0;
        m_rate = sampleRate;
        m_formants = preserveFormants;
        m_driver.reset();
        if (count > 0 && m_length > 0 && sampleRate > 0.0)
        {
            // The same engine and settings as Stamp. Built here, off the audio thread.
            m_driver = std::make_unique<RubberBandDriver>(sampleRate, count, kChunk, 0, preserveFormants);
            m_feed.assign(static_cast<std::size_t>(count), std::vector<float>(static_cast<std::size_t>(kChunk), 0.0f));
            m_feedPtrs.resize(static_cast<std::size_t>(count));
            for (int c = 0; c < count; ++c)
                m_feedPtrs[static_cast<std::size_t>(c)] = m_feed[static_cast<std::size_t>(c)].data();
            m_fifo.assign(static_cast<std::size_t>(count), std::vector<float>(kFifoSize, 0.0f));
            m_fifoRead = m_fifoCount = 0;
        }
    }
    else if (preserveFormants != m_formants && m_driver != nullptr)
    {
        m_formants = preserveFormants;
        m_driver->setFormantPreserve(preserveFormants);
    }
}

void AuditionBus::setCurve(const std::vector<float>& semitones, double hopSeconds)
{
    const std::lock_guard<std::mutex> lock(m_lock);
    m_curve = semitones;
    m_hop = hopSeconds;
}

// No clicks, by construction. Sound only ever starts from silence, with the stretcher reset and
// the level at zero, and rises over a few milliseconds once the stretcher delivers; it ends by
// falling the same way. A change while sounding never cuts anything off: straight playback
// fades out before it moves to a new place and back in after, and a held moment is made of
// pieces that each fade themselves.
void AuditionBus::startIfSilent()
{
    if (m_active || m_driver == nullptr)
        return;
    m_driver->reset();
    m_fifoRead = m_fifoCount = 0;
    m_grains[0] = {};
    m_grains[1] = {};
    m_grains[1].phase = -1;   // half a grain behind the first: set up in fillChunk()
    m_gain = 0.0f;
    m_playGain = 0.0f;
    m_playTarget = 0.0f;
    m_pendingPosition = -1.0;
    m_started = false;
    m_priming = true;
    m_active = true;
}

void AuditionBus::hold(double seconds, double semitones)
{
    const std::lock_guard<std::mutex> lock(m_lock);
    if (m_driver == nullptr)
        return;
    startIfSilent();
    m_holdSemitones = semitones;
    m_holding = true;
    m_playTarget = 0.0f;          // straight playback gives way to the held moment
    m_pendingPosition = -1.0;
    if (m_playGain < 0.01f)
        m_position = seconds * m_rate;
    else
        m_pendingPosition = seconds * m_rate;   // once playback has faded
    m_fadeTarget = 1.0f;
}

void AuditionBus::playFrom(double seconds)
{
    const std::lock_guard<std::mutex> lock(m_lock);
    if (m_driver == nullptr)
        return;
    startIfSilent();
    m_holding = false;            // the held moment's pieces finish and no more start
    if (m_playGain < 0.01f)
    {
        m_position = seconds * m_rate;
        m_playTarget = 1.0f;
    }
    else
    {
        m_pendingPosition = seconds * m_rate;   // duck, move, come back
        m_playTarget = 0.0f;
    }
    m_fadeTarget = 1.0f;
}

void AuditionBus::stop()
{
    const std::lock_guard<std::mutex> lock(m_lock);
    m_fadeTarget = 0.0f;
}

void AuditionBus::notePreviewRunning() noexcept
{
    m_lastPulledNs.store(nowNs(), std::memory_order_relaxed);
}

bool AuditionBus::previewRunning() const noexcept
{
    return nowNs() - m_lastPulledNs.load(std::memory_order_relaxed) < 250'000'000;
}

void AuditionBus::resetForTesting()
{
    const std::lock_guard<std::mutex> lock(m_lock);
    m_source.reset();
    m_driver.reset();
    m_sourceChannels = 0;
    m_length = 0;
    m_curve.clear();
    m_active = m_holding = false;
    m_gain = m_fadeTarget = 0.0f;
    m_shownSeconds.store(-1.0, std::memory_order_relaxed);
    m_lastPulledNs.store(0, std::memory_order_relaxed);
}

double AuditionBus::semitonesNow() const noexcept
{
    if (m_holding || m_curve.empty() || m_hop <= 0.0)
        return m_holdSemitones;
    // The ratio set with a chunk is heard a little behind the newest sample fed.
    const double heard = (m_position - static_cast<double>(m_driver->getRatioAlignment())) / m_rate;
    const auto frame = static_cast<std::size_t>(std::clamp(heard / m_hop, 0.0, static_cast<double>(m_curve.size() - 1)));
    return static_cast<double>(m_curve[frame]);
}

// One chunk of source for the stretcher: the effect played straight from the play position, and
// / or a held moment built from overlapping pieces of the effect around it.
void AuditionBus::fillChunk() noexcept
{
    const auto& source = *m_source;
    const int grain = std::max(256, static_cast<int>(0.09 * m_rate));
    if (m_grains[1].phase < 0)
        m_grains[1].phase = grain / 2;
    const float glide = 1.0f - std::exp(-1.0f / static_cast<float>(0.005 * m_rate));   // 5 ms
    constexpr float twoPi = 6.28318530717958647692f;

    for (int i = 0; i < kChunk; ++i)
    {
        m_playGain += (m_playTarget - m_playGain) * glide;
        if (m_pendingPosition >= 0.0 && m_playGain < 0.01f)
        {
            m_position = m_pendingPosition;
            m_pendingPosition = -1.0;
            if (! m_holding)
                m_playTarget = 1.0f;
        }

        float out[2] { 0.0f, 0.0f };

        if (m_playGain > 1.0e-4f)
        {
            const auto at = static_cast<std::int64_t>(m_position);
            if (at >= 0 && at < m_length)
                for (int c = 0; c < m_sourceChannels; ++c)
                    out[c] += m_playGain * source[static_cast<std::size_t>(c)][static_cast<std::size_t>(at)];
        }
        if (! m_holding && m_playTarget > 0.0f)
            m_position += 1.0;

        for (auto& g : m_grains)
        {
            if (g.phase >= grain || (! g.live && g.phase == 0))
            {
                g.phase = 0;
                g.live = m_holding;
                if (g.live)
                {
                    // Each piece starts a little either side of the held moment, so it does not buzz.
                    m_random = m_random * 1664525u + 1013904223u;
                    const double jitter = (static_cast<double>(m_random >> 8) / 16777216.0 - 0.5) * 0.03 * m_rate;
                    g.read = m_position - 0.5 * grain + jitter;
                }
            }
            if (g.live)
            {
                const auto at = static_cast<std::int64_t>(g.read);
                if (at >= 0 && at < m_length)
                {
                    // Two pieces overlap at any time and they are not in step with each other, so
                    // their sum swings above the effect's own level: 0.8 keeps it about there.
                    const float window = 0.8f * (0.5f - 0.5f * std::cos(twoPi * static_cast<float>(g.phase) / static_cast<float>(grain)));
                    for (int c = 0; c < m_sourceChannels; ++c)
                        out[c] += window * source[static_cast<std::size_t>(c)][static_cast<std::size_t>(at)];
                }
                g.read += 1.0;
            }
            ++g.phase;
        }

        for (int c = 0; c < m_sourceChannels; ++c)
            m_feed[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = out[c];
    }
}

void AuditionBus::render(float* const* channels, int numChannels, int numSamples, double streamRate) noexcept
{
    m_lastPulledNs.store(nowNs(), std::memory_order_relaxed);

    // The editor holds the lock for microseconds at a time (a few numbers, or a copy of the curve),
    // on every movement of a drag. Giving the block up to it would let the dry Preview through
    // for one block in the middle of an audition, which is a click: a short bounded spin gets the
    // lock instead, and only a lock held far longer than that is walked away from.
    std::unique_lock<std::mutex> lock(m_lock, std::try_to_lock);
    for (int tries = 0; ! lock.owns_lock() && tries < 4000; ++tries)
        lock.try_lock();
    if (! lock.owns_lock())
        return;
    // The stretcher was built for the rate the effect was learned at.
    if (! m_active || m_driver == nullptr || numChannels <= 0 || std::abs(streamRate - m_rate) > 0.5)
    {
        m_shownSeconds.store(-1.0, std::memory_order_relaxed);
        return;
    }

    // Keep the stretcher fed: about as much in as goes out, and a little extra at the start so
    // the first sound arrives sooner.
    int budget = numSamples / kChunk + 2;
    if (m_priming)
    {
        budget += 6;
        m_priming = false;
    }
    while (static_cast<int>(m_fifoCount) < numSamples && budget-- > 0)
    {
        fillChunk();
        const double scale = std::exp2(std::clamp(semitonesNow(), -24.0, 24.0) / 12.0);
        m_driver->processChunk(m_feedPtrs.data(), scale, [&](const float* const* data, int got)
        {
            for (int i = 0; i < got && m_fifoCount < kFifoSize; ++i)
            {
                const auto at = (m_fifoRead + m_fifoCount) % kFifoSize;
                for (int c = 0; c < m_sourceChannels; ++c)
                    m_fifo[static_cast<std::size_t>(c)][at] = data[c][i];
                ++m_fifoCount;
            }
        });
    }

    // Playing on has reached the end of the effect.
    if (! m_holding && m_playTarget > 0.0f && m_position >= static_cast<double>(m_length))
        m_fadeTarget = 0.0f;

    // The level moves towards its target with an 8 ms time constant, and only starts to rise
    // once there is sound to rise into (the Preview stream is not ducked for nothing).
    const float glide = 1.0f - std::exp(-1.0f / static_cast<float>(0.008 * streamRate));
    for (int i = 0; i < numSamples; ++i)
    {
        float in[2] { 0.0f, 0.0f };
        if (m_fifoCount > 0)
        {
            for (int c = 0; c < m_sourceChannels; ++c)
                in[c] = m_fifo[static_cast<std::size_t>(c)][m_fifoRead];
            m_fifoRead = (m_fifoRead + 1) % kFifoSize;
            --m_fifoCount;
            m_started = true;
        }

        const float target = m_started ? m_fadeTarget : 0.0f;
        m_gain += (target - m_gain) * glide;
        if (m_fadeTarget <= 0.0f && m_gain < 1.0e-4f)
        {
            m_active = false;
            m_gain = 0.0f;
            break;
        }

        if (m_sourceChannels == 1)
            in[1] = in[0];
        for (int c = 0; c < numChannels; ++c)
        {
            // Mono out of a stereo effect is the average; anything else maps straight across.
            const float audition = numChannels == 1 && m_sourceChannels == 2 ? 0.5f * (in[0] + in[1]) : in[std::min(c, 1)];
            channels[c][i] = channels[c][i] * (1.0f - m_gain) + std::clamp(audition, -1.0f, 1.0f) * m_gain;
        }
    }
    m_shownSeconds.store(m_active ? std::clamp(m_position, 0.0, static_cast<double>(m_length)) / m_rate : -1.0, std::memory_order_relaxed);
}

} // namespace tonalrecall

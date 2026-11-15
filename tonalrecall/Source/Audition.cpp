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
    // Preview is taken to be running for this long after it last asked for audio. A longer gap
    // than this between two blocks means it stopped and started again.
    constexpr std::int64_t kRunningNs = 250'000'000;
    // The fade at either end of straight playback, and between two things played one after the
    // other: short enough to keep the start of a sound, long enough not to click.
    constexpr double kFadeSeconds = 0.005;
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
        // Another effect: whatever was playing the old one stops (it may be freed with it).
        killVoices(Playback::Input);
        const auto role = playing();
        if (role == Playback::Input || role == Playback::Output)
            setRole(Playback::None, false);
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

void AuditionBus::setMusic(std::shared_ptr<const Channels> channels, double sampleRate)
{
    const std::lock_guard<std::mutex> lock(m_lock);
    if (channels == m_music && std::abs(sampleRate - m_musicRate) < 0.5)
        return;
    // The old music may be freed here: nothing may still be reading it.
    killVoices(Playback::Music);
    if (playing() == Playback::Music)
        setRole(Playback::None, false);
    m_music = std::move(channels);
    m_musicRate = sampleRate;
}

void AuditionBus::setCurve(const std::vector<float>& semitones, double hopSeconds)
{
    const std::lock_guard<std::mutex> lock(m_lock);
    m_curve = semitones;
    m_hop = hopSeconds;
}

void AuditionBus::setRole(Playback what, bool byPlay) noexcept
{
    m_byPlay.store(byPlay, std::memory_order_release);
    m_role.store(static_cast<int>(what), std::memory_order_release);
}

void AuditionBus::killVoices(Playback what) noexcept
{
    for (auto& v : m_voices)
        if (what == Playback::None || v.what == what)
            v = {};
}

void AuditionBus::fadeOutVoices() noexcept
{
    for (auto& v : m_voices)
    {
        if (! v.live)
            continue;
        if (v.gain <= 0.0f)
            v = {};
        else
            v.step = -1.0f / static_cast<float>(v.fade);
    }
}

void AuditionBus::startVoice(Playback what)
{
    const auto& source = what == Playback::Music ? m_music : m_source;
    const double rate = what == Playback::Music ? m_musicRate : m_rate;
    // A free voice, or else the quieter of the two (it is on its way out).
    auto* voice = &m_voices[0];
    if (m_voices[0].live)
        voice = ! m_voices[1].live || m_voices[1].gain < m_voices[0].gain ? &m_voices[1] : &m_voices[0];

    Voice v;
    v.what = what;
    v.source = source.get();
    v.channels = static_cast<int>(std::min<std::size_t>(2, source->size()));
    v.length = static_cast<std::int64_t>((*source)[0].size());
    for (int c = 1; c < v.channels; ++c)
        v.length = std::min(v.length, static_cast<std::int64_t>((*source)[static_cast<std::size_t>(c)].size()));
    v.rate = rate;
    v.fade = std::max<std::int64_t>(32, std::llround(kFadeSeconds * rate));
    v.gain = 0.0f;
    v.step = 1.0f / static_cast<float>(v.fade);
    v.live = true;
    *voice = v;
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
    m_fedCount = m_playedCount = 0;
    m_lastScale = 1.0;
    m_trimSet = false;
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

void AuditionBus::play(Playback what)
{
    const std::lock_guard<std::mutex> lock(m_lock);
    if (what == Playback::None)
    {
        m_fadeTarget = 0.0f;
        fadeOutVoices();
        setRole(Playback::None, false);
        return;
    }

    const auto& source = what == Playback::Music ? m_music : m_source;
    const double rate = what == Playback::Music ? m_musicRate : m_rate;
    if (source == nullptr || source->empty() || (*source)[0].empty() || rate <= 0.0)
        return;
    if (what == Playback::Output && m_driver == nullptr)
        return;

    // With Preview stopped nothing is sounding: start clean, and wait for it.
    if (! previewRunning())
    {
        killVoices(Playback::None);
        m_active = false;
        m_gain = 0.0f;
    }

    if (what == Playback::Output)
    {
        startIfSilent();
        m_holding = false;
        if (m_playGain < 0.01f)
        {
            m_position = 0.0;
            m_playTarget = 1.0f;
        }
        else
        {
            m_pendingPosition = 0.0;   // duck, move, come back
            m_playTarget = 0.0f;
        }
        m_fadeTarget = 1.0f;
        // Straight playback keeps sounding until the stretcher has something to take over with.
        m_yieldToStretch = m_voices[0].live || m_voices[1].live;
    }
    else
    {
        m_fadeTarget = 0.0f;   // the stretcher fades out as this fades in
        m_yieldToStretch = false;
        fadeOutVoices();
        startVoice(what);
    }
    setRole(what, true);
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
    m_yieldToStretch = m_voices[0].live || m_voices[1].live;
    setRole(Playback::Output, m_byPlay.load(std::memory_order_relaxed) && playing() == Playback::Output);
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
    m_yieldToStretch = m_voices[0].live || m_voices[1].live;
    setRole(Playback::Output, m_byPlay.load(std::memory_order_relaxed) && playing() == Playback::Output);
}

void AuditionBus::stop()
{
    const std::lock_guard<std::mutex> lock(m_lock);
    m_fadeTarget = 0.0f;
    m_yieldToStretch = false;
    fadeOutVoices();
    setRole(Playback::None, false);
}

void AuditionBus::stopNow()
{
    const std::lock_guard<std::mutex> lock(m_lock);
    killVoices(Playback::None);
    m_yieldToStretch = false;
    m_active = m_holding = false;
    m_gain = m_fadeTarget = 0.0f;
    setRole(Playback::None, false);
    m_shownWhat.store(0, std::memory_order_relaxed);
    m_shownSeconds.store(-1.0, std::memory_order_relaxed);
}

void AuditionBus::notePreviewRunning() noexcept
{
    m_lastPulledNs.store(nowNs(), std::memory_order_relaxed);
    // Learn is listening: whatever was playing is over, and must not come back when it finishes.
    m_abandon.store(true, std::memory_order_release);
    setRole(Playback::None, false);
}

bool AuditionBus::previewRunning() const noexcept
{
    return nowNs() - m_lastPulledNs.load(std::memory_order_relaxed) < kRunningNs;
}

AuditionBus::Playhead AuditionBus::playhead() const noexcept
{
    Playhead p;
    p.what = static_cast<Playback>(m_shownWhat.load(std::memory_order_acquire));
    p.seconds = m_shownSeconds.load(std::memory_order_relaxed);
    p.lengthSeconds = m_shownLength.load(std::memory_order_relaxed);
    p.moving = m_shownMoving.load(std::memory_order_relaxed);
    p.atNs = m_shownNs.load(std::memory_order_relaxed);
    if (p.what == Playback::None || p.seconds < 0.0)
    {
        p.what = Playback::None;
        p.seconds = -1.0;
    }
    return p;
}

void AuditionBus::publishStampPosition(double seconds, double lengthSeconds) noexcept
{
    m_stampSeconds.store(seconds, std::memory_order_relaxed);
    m_stampLength.store(lengthSeconds, std::memory_order_relaxed);
    m_stampNs.store(nowNs(), std::memory_order_release);
}

AuditionBus::Playhead AuditionBus::stampPlayhead() const noexcept
{
    Playhead p;
    const auto at = m_stampNs.load(std::memory_order_acquire);
    const double seconds = m_stampSeconds.load(std::memory_order_relaxed);
    if (seconds < 0.0 || nowNs() - at >= kRunningNs)
        return p;
    p.what = Playback::Output;
    p.seconds = seconds;
    p.lengthSeconds = m_stampLength.load(std::memory_order_relaxed);
    p.moving = true;
    p.atNs = at;
    return p;
}

void AuditionBus::resetForTesting()
{
    const std::lock_guard<std::mutex> lock(m_lock);
    killVoices(Playback::None);
    m_source.reset();
    m_music.reset();
    m_musicRate = 0.0;
    m_driver.reset();
    m_sourceChannels = 0;
    m_length = 0;
    m_curve.clear();
    m_active = m_holding = false;
    m_yieldToStretch = false;
    m_gain = m_fadeTarget = 0.0f;
    setRole(Playback::None, false);
    m_shownWhat.store(0, std::memory_order_relaxed);
    m_shownSeconds.store(-1.0, std::memory_order_relaxed);
    m_stampSeconds.store(-1.0, std::memory_order_relaxed);
    m_stampNs.store(0, std::memory_order_relaxed);
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

// Where in the effect the sample just played came from. The stretcher's output lines up sample
// for sample with what it was fed (see RubberBandDriver) at a pitch scale of 1; at another scale
// it comes out getBaseDelay() * (1/r - 1) samples later. Each chunk fed left a mark of where in
// the effect it was read from.
double AuditionBus::heardSourcePosition() const noexcept
{
    if (m_fedCount <= 0)
        return m_position;
    const double late = m_driver != nullptr ? static_cast<double>(m_driver->getBaseDelay()) * (1.0 / m_lastScale - 1.0) : 0.0;
    const double fed = static_cast<double>(m_playedCount) - 1.0 - late;
    if (m_playedCount <= 0 || fed < 0.0)
        return m_marks[0].from;
    const auto oldest = std::max<std::int64_t>(0, m_fedCount - static_cast<std::int64_t>(kMarks) * kChunk);
    const auto index = std::clamp(static_cast<std::int64_t>(fed), oldest, m_fedCount - 1);
    const auto& mark = m_marks[static_cast<std::size_t>((index / kChunk) % kMarks)];
    // A move to another place inside the chunk: the chunk is placed where it ended up.
    if (std::abs(mark.to - mark.from) > 2.0 * kChunk)
        return mark.to;
    const double frac = static_cast<double>(index % kChunk) / static_cast<double>(kChunk);
    return mark.from + (mark.to - mark.from) * frac;
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
    double from = -1.0, last = m_position;

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

        if (from < 0.0)
            from = m_position;
        last = m_position;
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

    m_marks[static_cast<std::size_t>((m_fedCount / kChunk) % kMarks)] = { from, last };
    m_fedCount += kChunk;
}

void AuditionBus::render(float* const* channels, int numChannels, int numSamples, double streamRate, float trim) noexcept
{
    const auto now = nowNs();
    const auto lastPulled = m_lastPulledNs.exchange(now, std::memory_order_relaxed);

    // The editor holds the lock for microseconds at a time (a few numbers, or a copy of the curve),
    // on every movement of a drag. Giving the block up to it would let the dry Preview through
    // for one block in the middle of an audition, which is a click: a short bounded spin gets the
    // lock instead, and only a lock held far longer than that is walked away from.
    std::unique_lock<std::mutex> lock(m_lock, std::try_to_lock);
    for (int tries = 0; ! lock.owns_lock() && tries < 4000; ++tries)
        lock.try_lock();
    if (! lock.owns_lock())
        return;

    if (m_abandon.exchange(false, std::memory_order_acq_rel))
    {
        killVoices(Playback::None);
        m_yieldToStretch = false;
        m_active = m_holding = false;
        m_gain = m_fadeTarget = 0.0f;
    }

    // Preview stopped and has started again: what was sounding comes back in from silence (it
    // was cut off when the stream stopped, and must not come back at full level).
    if (now - lastPulled > kRunningNs)
    {
        m_gain = 0.0f;
        for (auto& v : m_voices)
        {
            if (! v.live)
                continue;
            if (v.step < 0.0f)
                v = {};
            else
            {
                v.gain = 0.0f;
                v.step = 1.0f / static_cast<float>(v.fade);
            }
        }
    }

    // Each source is played only at the rate it was learned at: anything else would be the
    // wrong speed and pitch (the stretcher too was built for the effect's rate).
    bool stretching = m_active && m_driver != nullptr && numChannels > 0 && std::abs(streamRate - m_rate) <= 0.5;
    for (auto& v : m_voices)
        if (v.live && std::abs(streamRate - v.rate) > 0.5)
            v = {};
    const auto role = playing();
    Playback ended = Playback::None;   // a voice that played to the end of its clip in this block
    double endedSeconds = 0.0;

    if (numChannels > 0 && (stretching || m_voices[0].live || m_voices[1].live))
    {
        if (stretching)
        {
            // Keep the stretcher fed: about as much in as goes out, and a little extra at the
            // start so the first sound arrives sooner.
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
                m_lastScale = std::clamp(scale, 0.25, 4.0);
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

            // Playing on has reached the end of the effect, as heard.
            if (! m_holding && m_playTarget > 0.0f && m_pendingPosition < 0.0 && heardSourcePosition() >= static_cast<double>(m_length))
                m_fadeTarget = 0.0f;
            if (! m_trimSet)
            {
                m_trimNow = trim;
                m_trimSet = true;
            }
        }

        // Levels move towards their targets: the stretcher's with an 8 ms time constant, and only
        // once there is sound to rise into (the Preview stream is not ducked for nothing);
        // straight playback in a straight line over a few milliseconds, so that once it is up it
        // is exactly the learned audio. Together they take over the stream: where they add up to
        // more than one, both are scaled back, so a change from one to the other never bulges.
        const float glide = 1.0f - std::exp(-1.0f / static_cast<float>(0.008 * streamRate));
        for (int i = 0; i < numSamples; ++i)
        {
            float s[2] { 0.0f, 0.0f };
            float stretchGain = 0.0f;
            if (stretching)
            {
                if (m_fifoCount > 0)
                {
                    for (int c = 0; c < m_sourceChannels; ++c)
                        s[c] = m_fifo[static_cast<std::size_t>(c)][m_fifoRead];
                    m_fifoRead = (m_fifoRead + 1) % kFifoSize;
                    --m_fifoCount;
                    ++m_playedCount;
                    m_started = true;
                }
                const float target = m_started ? m_fadeTarget : 0.0f;
                m_gain += (target - m_gain) * glide;
                if (m_fadeTarget <= 0.0f && m_gain < 1.0e-4f)
                {
                    m_active = false;
                    m_gain = 0.0f;
                    stretching = false;
                }
                if (m_sourceChannels == 1)
                    s[1] = s[0];
                stretchGain = m_gain;
                m_trimNow += (trim - m_trimNow) * glide;   // a new trim eases in
                if (m_yieldToStretch && m_started)
                {
                    fadeOutVoices();
                    m_yieldToStretch = false;
                }
            }

            float p[2] { 0.0f, 0.0f };
            float plainGain = 0.0f;
            for (auto& v : m_voices)
            {
                if (! v.live)
                    continue;
                // The end of the clip: out to nothing over its last few milliseconds.
                const auto left = v.length - v.position;
                if (v.step >= 0.0f && left <= v.fade)
                    v.step = -v.gain / static_cast<float>(std::max<std::int64_t>(1, left));
                const auto at = static_cast<std::size_t>(v.position);
                const float x0 = (*v.source)[0][at];
                const float x1 = v.channels > 1 ? (*v.source)[1][at] : x0;
                p[0] += v.gain * x0;
                p[1] += v.gain * x1;
                plainGain += v.gain;
                ++v.position;
                v.gain += v.step;
                // (Within half a step of the top is the top: the sum of the steps in floating
                // point stops a hair short of it, and once up the level must be exactly one.)
                if (v.gain >= 1.0f - 0.5f * std::max(0.0f, v.step))
                {
                    v.gain = 1.0f;
                    if (v.step > 0.0f)
                        v.step = 0.0f;
                }
                if (v.position >= v.length)
                {
                    ended = v.what;
                    endedSeconds = static_cast<double>(v.length) / v.rate;
                }
                if ((v.step < 0.0f && v.gain <= 0.0f) || v.position >= v.length)
                    v = {};
            }

            const float total = stretchGain + plainGain;
            if (total <= 0.0f)
                continue;
            const float scale = total > 1.0f ? 1.0f / total : 1.0f;
            const float take = std::min(1.0f, total);
            const float stretchLevel = m_trimNow * stretchGain;
            for (int c = 0; c < numChannels; ++c)
            {
                // Mono out of a stereo source is the average; anything else maps straight across.
                const int k = std::min(c, 1);
                const float a = numChannels == 1 ? 0.5f * (s[0] + s[1]) : s[k];
                const float b = numChannels == 1 ? 0.5f * (p[0] + p[1]) : p[k];
                const float mix = std::clamp((stretchLevel * a + b) * scale, -1.0f, 1.0f);
                channels[c][i] = channels[c][i] * (1.0f - take) + mix;
            }
        }
    }

    // Where what is playing has got to, as heard; and whether it has finished.
    double seconds = -1.0, length = 0.0;
    bool moving = false, finished = false;
    if (role == Playback::Output && m_active && m_rate > 0.0 && std::abs(streamRate - m_rate) <= 0.5)
    {
        seconds = std::clamp(heardSourcePosition(), 0.0, static_cast<double>(m_length)) / m_rate;
        length = static_cast<double>(m_length) / m_rate;
        moving = ! m_holding && m_playTarget > 0.0f;
    }
    else if (role == Playback::Music || role == Playback::Input)
    {
        const Voice* shown = nullptr;
        for (const auto& v : m_voices)
            if (v.live && v.what == role && (shown == nullptr || v.step >= 0.0f))
                shown = &v;
        if (shown != nullptr && shown->rate > 0.0)
        {
            seconds = static_cast<double>(shown->position) / shown->rate;
            length = static_cast<double>(shown->length) / shown->rate;
            moving = true;
        }
        else if (ended == role)
        {
            seconds = length = endedSeconds;   // the last report of a clip played through is its end
            finished = true;
        }
    }
    if ((seconds < 0.0 || finished) && role != Playback::None)
        setRole(Playback::None, false);   // it has played to its end (or cannot be played at this rate)

    m_shownSeconds.store(seconds, std::memory_order_relaxed);
    m_shownLength.store(length, std::memory_order_relaxed);
    m_shownMoving.store(moving, std::memory_order_relaxed);
    m_shownNs.store(now, std::memory_order_relaxed);
    m_shownWhat.store(seconds >= 0.0 ? static_cast<int>(role) : 0, std::memory_order_release);
}

} // namespace tonalrecall

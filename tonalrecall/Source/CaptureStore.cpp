#include "CaptureStore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

namespace tonalrecall
{

const char* slotName(Slot slot) noexcept
{
    return slot == Slot::Music ? "Music" : "Sfx";
}

const char* passEndName(PassEnd end) noexcept
{
    switch (end)
    {
        case PassEnd::None:              return "none";
        case PassEnd::TimelineJumpBack:  return "timeline jumped backwards (loop)";
        case PassEnd::TimelineJump:      return "timeline jumped";
        case PassEnd::Stall:             return "audio stopped (stall)";
        case PassEnd::BufferFull:        return "buffer full (truncated)";
        case PassEnd::Disarmed:          return "disarmed by user";
        case PassEnd::Cleared:           return "cleared";
        case PassEnd::SampleRateChanged: return "sample rate changed";
        case PassEnd::OwnerDestroyed:    return "capturing instance was deleted";
        case PassEnd::SelectionEnd:      return "heard the whole selection";
    }
    return "?";
}

namespace
{
    std::int64_t steadyNow() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::atomic<CaptureStore::ClockFn> g_clock { nullptr };

    // Host sample rates are whole numbers (or 1/1.001 pull-downs of them); a tolerance avoids
    // comparing doubles exactly.
    bool sameRate(double a, double b) noexcept { return std::abs(a - b) < 1.0e-3; }
    std::mutex g_allocMutex;   // only ever taken off the audio thread
}

std::int64_t CaptureStore::now() noexcept
{
    if (auto fn = g_clock.load(std::memory_order_acquire))
        return fn();
    return steadyNow();
}

void CaptureStore::setClockForTesting(ClockFn fn) noexcept
{
    g_clock.store(fn, std::memory_order_release);
}

void CaptureStore::SpinLock::lock() noexcept
{
    // Off the audio thread only; the audio thread holds it for one block's copy at most.
    while (! tryLock())
        std::this_thread::yield();
}

CaptureStore& CaptureStore::instance()
{
    static CaptureStore store;
    return store;
}

CaptureStore::CaptureStore() = default;

void CaptureStore::ensureAllocated()
{
    if (isAllocated())
        return;

    const std::lock_guard<std::mutex> guard(g_allocMutex);
    if (isAllocated())
        return;

    // Uninitialised on purpose: untouched pages cost no memory until a capture writes them.
    const auto bins = static_cast<std::size_t>(kCapacity / kPeakHop + 1);
    m_storage.reset(new float[static_cast<std::size_t>(kCapacity) * kNumSlots]);
    m_peakStorage.reset(new float[bins * kNumSlots]);
    m_channelStorage.reset(new float[static_cast<std::size_t>(kCapacity) * 2]);

    for (int i = 0; i < kNumSlots; ++i)
    {
        auto& s = m_slots[static_cast<std::size_t>(i)];
        s.lock.lock();
        s.buffer = m_storage.get() + static_cast<std::size_t>(kCapacity) * static_cast<std::size_t>(i);
        s.peaks = m_peakStorage.get() + bins * static_cast<std::size_t>(i);
        if (static_cast<Slot>(i) == Slot::Sfx)
        {
            s.channel[0] = m_channelStorage.get();
            s.channel[1] = m_channelStorage.get() + static_cast<std::size_t>(kCapacity);
        }
        s.lock.unlock();
    }
    m_allocated.store(true, std::memory_order_release);
}

std::int64_t CaptureStore::limitFor(double sampleRate) noexcept
{
    if (sampleRate <= 0.0)
        return kCapacity;
    return std::min(kCapacity, static_cast<std::int64_t>(kMaxSeconds * sampleRate));
}

void CaptureStore::resetData(SlotData& s) noexcept
{
    s.sampleRate = 0.0;
    s.channels = 0;
    s.timelineKnown = false;
    s.origin = 0;
    s.extent = 0;
    s.numSegments = 0;
    s.passes = 0;
    s.peak = 0.0f;
    s.truncated = false;
    s.capturedBy = 0;
    s.frozen = false;
    s.passActive = false;
    s.owner = 0;
    s.passSamples = 0;
    s.passLimit = 0;
}

void CaptureStore::arm(Slot slot)
{
    ensureAllocated();
    auto& s = slotData(slot);
    s.lock.lock();
    s.armed.store(true, std::memory_order_release);
    s.samplesSinceArm = 0;
    s.pendingSkip.store(0, std::memory_order_relaxed);
    s.generation.fetch_add(1, std::memory_order_release);
    s.lock.unlock();
}

void CaptureStore::disarm(Slot slot)
{
    auto& s = slotData(slot);
    s.lock.lock();
    if (s.passActive)
        endPass(s, PassEnd::Disarmed);
    s.armed.store(false, std::memory_order_release);
    s.generation.fetch_add(1, std::memory_order_release);
    s.lock.unlock();
}

void CaptureStore::clear(Slot slot)
{
    auto& s = slotData(slot);
    s.lock.lock();
    resetData(s);
    s.samplesSinceArm = 0;
    s.skippedOtherOwner = 0;
    s.lastEnd = PassEnd::Cleared;
    s.armed.store(false, std::memory_order_release);
    s.missedBlocks.store(0, std::memory_order_relaxed);
    s.generation.fetch_add(1, std::memory_order_release);
    s.lock.unlock();
}

bool CaptureStore::isArmed(Slot slot) const noexcept
{
    return slotData(slot).armed.load(std::memory_order_acquire);
}

std::uint32_t CaptureStore::generation(Slot slot) const noexcept
{
    return slotData(slot).generation.load(std::memory_order_acquire);
}

void CaptureStore::setExpectedRange(Slot slot, std::optional<TimelineRange> range)
{
    auto& s = slotData(slot);
    s.lock.lock();
    s.expected = range;
    s.generation.fetch_add(1, std::memory_order_release);
    s.lock.unlock();
}

void CaptureStore::addSegment(SlotData& s, TimelineRange r) noexcept
{
    if (r.end <= r.start)
        return;

    // Insert in order, then coalesce overlapping or touching ranges.
    if (s.numSegments == kMaxSegments)
    {
        // Out of room: widen the last range rather than lose the coverage (overstates it slightly).
        auto& last = s.segments[static_cast<std::size_t>(kMaxSegments - 1)];
        last.start = std::min(last.start, r.start);
        last.end = std::max(last.end, r.end);
    }
    else
    {
        int at = s.numSegments;
        while (at > 0 && s.segments[static_cast<std::size_t>(at - 1)].start > r.start)
        {
            s.segments[static_cast<std::size_t>(at)] = s.segments[static_cast<std::size_t>(at - 1)];
            --at;
        }
        s.segments[static_cast<std::size_t>(at)] = r;
        ++s.numSegments;
    }

    int out = 0;
    for (int i = 1; i < s.numSegments; ++i)
    {
        auto& cur = s.segments[static_cast<std::size_t>(out)];
        const auto& next = s.segments[static_cast<std::size_t>(i)];
        if (next.start <= cur.end)
            cur.end = std::max(cur.end, next.end);
        else
            s.segments[static_cast<std::size_t>(++out)] = next;
    }
    s.numSegments = s.numSegments > 0 ? out + 1 : 0;
}

void CaptureStore::endPass(SlotData& s, PassEnd why) noexcept
{
    if (s.passActive && s.passSamples > 0)
        addSegment(s, { s.passStartOffset, s.passStartOffset + s.passSamples });
    s.passActive = false;
    s.owner = 0;
    s.lastEnd = why;
}

void CaptureStore::zeroGap(SlotData& s, std::int64_t upTo) noexcept
{
    // A later pass that starts past the end of what we have: the gap reads as silence.
    if (upTo <= s.extent)
        return;
    std::memset(s.buffer + s.extent, 0, static_cast<std::size_t>(upTo - s.extent) * sizeof(float));
    for (auto* channel : s.channel)
        if (channel != nullptr)
            std::memset(channel + s.extent, 0, static_cast<std::size_t>(upTo - s.extent) * sizeof(float));
    const auto from = s.extent;
    s.extent = upTo;
    recomputePeaks(s, from, upTo);
}

void CaptureStore::recomputePeaks(SlotData& s, std::int64_t from, std::int64_t to) noexcept
{
    if (to <= from)
        return;
    for (auto bin = from / kPeakHop; bin <= (to - 1) / kPeakHop; ++bin)
    {
        const auto a = bin * kPeakHop;
        const auto b = std::min(a + kPeakHop, s.extent);
        float m = 0.0f;
        for (auto i = a; i < b; ++i)
            m = std::max(m, std::abs(s.buffer[i]));
        s.peaks[bin] = m;
    }
}

void CaptureStore::beginPass(SlotData& s, BlockEvent& ev, double sampleRate, std::optional<std::int64_t> position,
                             std::uint32_t instanceId, std::optional<std::int64_t> selectionLength) noexcept
{
    const auto limit = limitFor(sampleRate);
    ev.passStarted = true;
    ev.hasPosition = position.has_value();
    ev.position = position.value_or(0);

    bool merged = false;
    // Merge by timeline position only when both this pass and the earlier capture know where
    // they are, the earlier capture's positions were sample-accurate (an approximate timeline
    // would splice the two passes out of line) and nothing else changed; otherwise the new pass
    // replaces the capture.
    if (position && s.timelineKnown && ! s.frozen && s.extent > 0 && sameRate(s.sampleRate, sampleRate))
    {
        const auto p = *position;
        if (p >= s.origin)
        {
            if (p - s.origin < limit)
            {
                s.writeOffset = p - s.origin;
                merged = true;
            }
        }
        else
        {
            // Starts before what we have: shift the capture up. Costs one move of the captured
            // audio on the audio thread, once per such pass.
            const auto shift = s.origin - p;
            if (shift + s.extent < limit)
            {
                std::memmove(s.buffer + shift, s.buffer, static_cast<std::size_t>(s.extent) * sizeof(float));
                std::memset(s.buffer, 0, static_cast<std::size_t>(shift) * sizeof(float));
                for (auto* channel : s.channel)
                {
                    if (channel == nullptr)
                        continue;
                    std::memmove(channel + shift, channel, static_cast<std::size_t>(s.extent) * sizeof(float));
                    std::memset(channel, 0, static_cast<std::size_t>(shift) * sizeof(float));
                }
                for (int i = 0; i < s.numSegments; ++i)
                {
                    s.segments[static_cast<std::size_t>(i)].start += shift;
                    s.segments[static_cast<std::size_t>(i)].end += shift;
                }
                s.extent += shift;
                s.origin = p;
                recomputePeaks(s, 0, s.extent);
                s.writeOffset = 0;
                merged = true;
                ev.prepended = true;
            }
        }
    }

    if (merged)
    {
        ev.merged = true;
    }
    else
    {
        ev.replaced = s.extent > 0;
        resetData(s);
        s.sampleRate = sampleRate;
        s.timelineKnown = position.has_value();
        s.origin = position.value_or(0);
        s.writeOffset = 0;
    }

    ++s.passes;
    s.passActive = true;
    s.owner = instanceId;
    s.capturedBy = instanceId;
    s.passHasTimeline = position.has_value();
    s.frozen = false;
    s.passBlocks = 0;
    s.passFirstPosition = position.value_or(0);
    s.lastPosition = s.passFirstPosition;
    s.expectedPosition = s.passFirstPosition;
    s.passStartOffset = s.writeOffset;
    s.passSamples = 0;
    s.passLimit = selectionLength.value_or(0) > 0 ? *selectionLength : 0;
    if (s.passLimit > 0)
    {
        // The selection is what the user meant to learn: it gives the fraction heard.
        const auto start = s.timelineKnown ? s.origin + s.passStartOffset : std::int64_t { 0 };
        s.expected = TimelineRange { start, start + s.passLimit };
    }
}

BlockEvent CaptureStore::captureBlock(Slot slot, const float* const* channels, int numChannels, int numSamples,
                                      double sampleRate, std::optional<std::int64_t> position,
                                      std::uint32_t instanceId, std::int64_t nowNs,
                                      std::optional<std::int64_t> selectionLength) noexcept
{
    BlockEvent ev;
    auto& s = slotData(slot);

    if (! s.armed.load(std::memory_order_acquire) || numSamples <= 0)
        return ev;
    if (! isAllocated())
    {
        ev.skippedUnallocated = true;
        return ev;
    }
    // The other side holds the lock for microseconds (a readout, a thumbnail copy), so a short
    // bounded spin nearly always gets it; only then give the block up rather than wait.
    bool locked = s.lock.tryLock();
    for (int tries = 0; ! locked && tries < kAudioSpinTries; ++tries)
        locked = s.lock.tryLock();
    if (! locked)
    {
        // Never wait on the audio thread. The lost samples are accounted for in the next block
        // (as a gap) so the timeline check does not mistake them for a jump.
        // Only the pass's owner counts them: another instance processing at the same time (a second
        // channel, say) has its own stream, and its lost blocks are not holes in the owner's.
        if (s.owner.load(std::memory_order_acquire) == instanceId)
        {
            s.pendingSkip.fetch_add(numSamples, std::memory_order_relaxed);
            s.missedBlocks.fetch_add(1, std::memory_order_relaxed);
        }
        ev.skippedLock = true;
        return ev;
    }

    struct Unlock { SpinLock& l; ~Unlock() { l.unlock(); } } unlock { s.lock };

    if (! s.armed.load(std::memory_order_relaxed))
        return ev;

    if (s.passActive && s.owner != instanceId)
    {
        // One writer per pass: a second instance processing at the same time (another channel,
        // or a render overlapping a preview) passes through without capturing.
        ++s.skippedOtherOwner;
        ev.skippedOtherOwner = true;
        ev.owner = s.owner;
        return ev;
    }

    const auto skipped = s.pendingSkip.exchange(0, std::memory_order_relaxed);
    const auto stopPass = [&](PassEnd why)
    {
        ev.ended = why;
        ev.passSamples = s.passSamples;
        endPass(s, why);
        s.armed.store(false, std::memory_order_release);
        s.generation.fetch_add(1, std::memory_order_release);
    };

    if (! s.passActive)
    {
        beginPass(s, ev, sampleRate, position, instanceId, selectionLength);
    }
    else
    {
        if (! sameRate(sampleRate, s.sampleRate))
        {
            stopPass(PassEnd::SampleRateChanged);
            return ev;
        }

        if (s.passHasTimeline && position)
        {
            // The audio a host delivers within a pass is contiguous; its reported position need
            // not be. AudioSuite Preview delivers bursts of blocks that all carry one position,
            // which then steps on by roughly (not exactly) a burst. So the position is only a
            // coarse clock: a pass ends when it goes backwards (Preview looped) or leaps far
            // ahead, and anything smaller just marks the timeline as approximate.
            const auto expected = s.expectedPosition + skipped;
            const auto p = *position;
            PassEnd jump = PassEnd::None;

            if (p != expected)
            {
                const auto leap = static_cast<std::int64_t>(kForwardLeapSeconds * s.sampleRate);
                if (p < s.lastPosition - kBackwardTolerance)
                    jump = PassEnd::TimelineJumpBack;
                else if (p - expected > leap)
                    jump = PassEnd::TimelineJump;
                else if (! s.frozen)
                {
                    s.frozen = true;
                    ev.timelineFrozen = true;
                }
            }

            if (jump != PassEnd::None)
            {
                ev.position = p;
                ev.expectedPosition = expected;
                stopPass(jump);
                return ev;
            }
        }
        s.writeOffset += skipped;
    }

    const auto limit = limitFor(sampleRate);
    // Blocks lost to a busy store are counted as a gap, and the gap may reach past the end of the
    // buffer: stop at the end (the pass then ends as full, below), never beyond it.
    s.writeOffset = std::min(s.writeOffset, limit);
    const auto room = std::max<std::int64_t>(0, limit - s.writeOffset);
    // Up to the end of the selection and no further: what follows is the loop starting again.
    const auto toSelectionEnd = s.passLimit > 0
                                    ? std::max<std::int64_t>(0, s.passLimit - (s.writeOffset - s.passStartOffset))
                                    : std::int64_t { numSamples };
    const auto n = static_cast<int>(std::min<std::int64_t>({ numSamples, room, toSelectionEnd }));

    zeroGap(s, s.writeOffset);

    float* dst = s.buffer + s.writeOffset;
    float peak = s.peak;
    for (int i = 0; i < n; ++i)
    {
        float sum = 0.0f;
        for (int c = 0; c < numChannels; ++c)
        {
            const float v = channels[c][i];
            sum += v;
            peak = std::max(peak, std::abs(v));
        }
        dst[i] = sum;
    }
    // The separate channels too, where the slot keeps them (a mono clip fills only the first).
    const int kept = s.channel[0] != nullptr ? std::min(numChannels, 2) : 0;
    for (int ch = 0; ch < kept; ++ch)
        std::memcpy(s.channel[ch] + s.writeOffset, channels[ch], static_cast<std::size_t>(n) * sizeof(float));
    if (kept == 1 && s.channels == 2)
        std::memcpy(s.channel[1] + s.writeOffset, channels[0], static_cast<std::size_t>(n) * sizeof(float));
    s.channels = std::max(s.channels, kept);
    s.peak = peak;

    const auto from = s.writeOffset;
    s.writeOffset += n;
    s.extent = std::max(s.extent, s.writeOffset);
    recomputePeaks(s, from, s.writeOffset);

    s.passSamples += n;
    s.samplesSinceArm += n;
    ++s.passBlocks;
    if (position)
    {
        s.lastPosition = *position;
        s.expectedPosition = *position + numSamples;
    }
    else
    {
        s.expectedPosition += numSamples;
    }
    s.lastBlockNs.store(nowNs, std::memory_order_release);

    if (s.passLimit > 0 && s.writeOffset - s.passStartOffset >= s.passLimit)
    {
        stopPass(PassEnd::SelectionEnd);
        return ev;
    }

    if (n < numSamples || s.writeOffset >= limit)
    {
        s.truncated = true;
        stopPass(PassEnd::BufferFull);
        return ev;
    }

    s.generation.fetch_add(1, std::memory_order_release);
    return ev;
}

std::array<PassEnd, kNumSlots> CaptureStore::checkStalls(std::int64_t nowNs)
{
    std::array<PassEnd, kNumSlots> ended {};
    for (std::size_t i = 0; i < m_slots.size(); ++i)
    {
        auto& s = m_slots[i];
        s.lock.lock();
        if (s.passActive && nowNs - s.lastBlockNs.load(std::memory_order_acquire) > kStallNs)
        {
            endPass(s, PassEnd::Stall);
            s.armed.store(false, std::memory_order_release);
            s.generation.fetch_add(1, std::memory_order_release);
            ended[i] = PassEnd::Stall;
        }
        s.lock.unlock();
    }
    return ended;
}

std::array<PassEnd, kNumSlots> CaptureStore::instanceGone(std::uint32_t instanceId)
{
    std::array<PassEnd, kNumSlots> ended {};
    for (std::size_t i = 0; i < m_slots.size(); ++i)
    {
        auto& s = m_slots[i];
        s.lock.lock();
        if (s.passActive && s.owner == instanceId)
        {
            endPass(s, PassEnd::OwnerDestroyed);
            s.armed.store(false, std::memory_order_release);
            s.generation.fetch_add(1, std::memory_order_release);
            ended[i] = PassEnd::OwnerDestroyed;
        }
        s.lock.unlock();
    }
    return ended;
}

void CaptureStore::snapshot(Slot slot, SlotSnapshot& out, std::vector<float>* peaks) const
{
    if (peaks != nullptr)
        peaks->resize(static_cast<std::size_t>(kCapacity / kPeakHop + 1));   // before the lock: may allocate

    const auto& s = slotData(slot);
    s.lock.lock();

    out = {};
    out.allocated = isAllocated();
    out.armed = s.armed.load(std::memory_order_relaxed);
    out.capturing = s.passActive;
    out.sampleRate = s.sampleRate;
    out.timelineKnown = s.timelineKnown;
    out.bufferExtent = s.extent;
    out.passes = s.passes;
    out.peak = s.peak;
    out.truncated = s.truncated;
    out.timelineFrozen = s.frozen;
    out.capturedBy = s.capturedBy;
    out.channels = s.channels;
    out.owner = s.owner;
    out.generation = s.generation.load(std::memory_order_relaxed);
    out.lastEnd = s.lastEnd;
    out.samplesSinceArm = s.samplesSinceArm;
    out.missedBlocks = s.missedBlocks.load(std::memory_order_relaxed);
    out.blocksSkippedOtherOwner = s.skippedOtherOwner;
    out.expected = s.expected;

    // Coverage: the finished segments plus the pass in progress.
    std::array<TimelineRange, kMaxSegments + 1> segs {};
    int count = 0;
    for (int i = 0; i < s.numSegments; ++i)
        segs[static_cast<std::size_t>(count++)] = s.segments[static_cast<std::size_t>(i)];
    if (s.passActive && s.passSamples > 0)
        segs[static_cast<std::size_t>(count++)] = { s.passStartOffset, s.passStartOffset + s.passSamples };
    std::sort(segs.begin(), segs.begin() + count, [](auto& a, auto& b) { return a.start < b.start; });

    std::int64_t covered = 0, lo = 0, hi = 0, runEnd = -1;
    int merged = 0;
    for (int i = 0; i < count; ++i)
    {
        const auto& r = segs[static_cast<std::size_t>(i)];
        if (i == 0)
            lo = r.start;
        hi = std::max(hi, r.end);
        const auto start = std::max(r.start, runEnd);
        if (r.end > start)
            covered += r.end - start;
        if (r.start > runEnd)
            ++merged;
        runEnd = std::max(runEnd, r.end);
    }
    out.samplesCaptured = covered;
    out.segments = merged;
    if (count > 0 && s.timelineKnown)
        out.captured = { s.origin + lo, s.origin + hi };

    if (s.expected && s.expected->end > s.expected->start)
    {
        const auto len = static_cast<double>(s.expected->end - s.expected->start);
        std::int64_t heard = 0;
        if (s.timelineKnown)
        {
            std::int64_t done = s.expected->start;   // counts overlapping segments once
            for (int i = 0; i < count; ++i)
            {
                const auto& r = segs[static_cast<std::size_t>(i)];
                const auto a = std::max({ s.origin + r.start, s.expected->start, done });
                const auto b = std::min(s.origin + r.end, s.expected->end);
                if (b > a)
                {
                    heard += b - a;
                    done = b;
                }
            }
        }
        else
        {
            heard = covered;   // no timeline: assume what we heard was the clip from its start
        }
        out.fractionHeard = std::min(1.0, static_cast<double>(heard) / len);
    }

    if (peaks != nullptr)
    {
        const auto bins = static_cast<std::size_t>((s.extent + kPeakHop - 1) / kPeakHop);
        if (s.peaks != nullptr)
            std::copy(s.peaks, s.peaks + bins, peaks->begin());
        peaks->resize(s.peaks != nullptr ? bins : 0);
    }

    s.lock.unlock();
}

void CaptureStore::copyAudio(Slot slot, std::vector<float>& out) const
{
    out.clear();
    const auto& s = slotData(slot);
    s.lock.lock();
    if (s.buffer != nullptr)
        out.assign(s.buffer, s.buffer + s.extent);
    s.lock.unlock();
}

void CaptureStore::copyChannels(Slot slot, std::vector<std::vector<float>>& out) const
{
    out.clear();
    const auto& s = slotData(slot);
    s.lock.lock();
    for (int ch = 0; ch < s.channels; ++ch)
        out.emplace_back(s.channel[ch], s.channel[ch] + s.extent);
    s.lock.unlock();
}

void CaptureStore::resetForTesting()
{
    for (auto& s : m_slots)
    {
        s.lock.lock();
        resetData(s);
        s.armed.store(false, std::memory_order_release);
        s.samplesSinceArm = 0;
        s.skippedOtherOwner = 0;
        s.lastEnd = PassEnd::None;
        s.expected.reset();
        s.writeOffset = 0;
        s.lastBlockNs.store(0, std::memory_order_relaxed);
        s.pendingSkip.store(0, std::memory_order_relaxed);
        s.missedBlocks.store(0, std::memory_order_relaxed);
        s.generation.fetch_add(1, std::memory_order_release);
        s.lock.unlock();
    }
}

} // namespace tonalrecall

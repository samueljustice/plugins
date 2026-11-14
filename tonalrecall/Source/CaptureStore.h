#pragma once

// The process-wide capture store. Pro Tools may build separate plug-in instances for Preview,
// Render, each channel or each clip, so what Learn hears must outlive any one instance: it lives
// here, in a static singleton, together with each slot's arm state. Any instance's editor arms a
// slot; whichever instance then gets audio captures it.
//
// Threading: the audio thread only ever try-locks a slot (a spin flag) and never allocates.
// Everything else (arm, clear, snapshots, the stall check) takes the same flag, briefly. The
// buffers are allocated once, off the audio thread (ensureAllocated()).
//
// No JUCE in here, so the rules can be unit-tested directly.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace tonalrecall
{

enum class Slot : int { Music = 0, Sfx = 1 };
constexpr int kNumSlots = 2;
const char* slotName(Slot slot) noexcept;

// Why the last pass over a slot ended.
enum class PassEnd : std::uint8_t
{
    None,
    TimelineJumpBack,     // the timeline position went backwards: Preview looped
    TimelineJump,         // the position leapt far ahead
    Stall,                // no audio for kStallNs while capturing: Preview (or Render) stopped
    BufferFull,           // kMaxSeconds reached: the capture is truncated
    Disarmed,             // the user disarmed Learn while audio was flowing
    Cleared,
    SampleRateChanged,
    OwnerDestroyed,       // the instance capturing the pass was deleted mid-pass
    SelectionEnd          // the whole selection was heard: the pass is complete
};
const char* passEndName(PassEnd end) noexcept;

struct TimelineRange
{
    std::int64_t start = 0;   // timeline samples, [start, end)
    std::int64_t end = 0;
};

// A copy of one slot's readouts (for editors, the log and tests).
struct SlotSnapshot
{
    bool allocated = false;
    bool armed = false;
    bool capturing = false;               // a pass is in progress
    double sampleRate = 0.0;
    bool timelineKnown = false;
    TimelineRange captured;               // first to last captured timeline sample (when known)
    std::int64_t samplesCaptured = 0;     // samples actually heard (gaps between passes excluded)
    std::int64_t bufferExtent = 0;        // samples in the buffer, gaps included
    int passes = 0;
    int segments = 0;                     // separate covered ranges (1 = contiguous)
    float peak = 0.0f;                    // linear, the loudest input channel sample
    bool truncated = false;
    bool timelineFrozen = false;          // the last pass's positions were approximate (not sample-accurate)
    std::uint32_t capturedBy = 0;         // instance id of the last pass (0 = none)
    int channels = 0;                     // separate channels kept (0 = only the sum)
    std::uint32_t owner = 0;              // instance id capturing now (0 = none)
    std::uint32_t generation = 0;
    PassEnd lastEnd = PassEnd::None;
    std::int64_t samplesSinceArm = 0;     // what the current/last armed period captured
    std::int64_t missedBlocks = 0;        // audio blocks lost to lock contention
    std::int64_t blocksSkippedOtherOwner = 0;
    std::optional<TimelineRange> expected;
    std::optional<double> fractionHeard;  // 0..1, when an expected range was given
};

// What one captureBlock() call did, so the processor can log it (no strings on the audio thread).
struct BlockEvent
{
    bool passStarted = false;
    bool hasPosition = false;     // the block had a timeline position
    bool merged = false;          // the new pass extends an earlier capture by timeline position
    bool replaced = false;        // the new pass replaced the earlier capture
    bool prepended = false;       // ... merged in front of it (the buffer was shifted)
    bool timelineFrozen = false;  // detected in this block
    bool skippedOtherOwner = false;
    bool skippedLock = false;
    bool skippedUnallocated = false;
    PassEnd ended = PassEnd::None;
    std::uint32_t owner = 0;
    std::int64_t position = 0;    // the block's timeline position, or the pass start for passStarted
    std::int64_t expectedPosition = 0;
    std::int64_t passSamples = 0; // samples in the pass (at its end)
};

class CaptureStore
{
public:
    static constexpr double kMaxSeconds = 60.0;
    static constexpr double kMaxSampleRate = 192000.0;
    static constexpr std::int64_t kCapacity = 60 * 192000;   // samples per slot
    static constexpr int kPeakHop = 256;                      // samples per thumbnail peak
    static constexpr std::int64_t kStallNs = 300'000'000;
    static constexpr int kMaxSegments = 64;
    static constexpr int kAudioSpinTries = 4000;              // how long the audio thread waits for a reader
    static constexpr std::int64_t kBackwardTolerance = 256;   // samples a coarse position may wobble back
    static constexpr double kForwardLeapSeconds = 0.5;        // a jump ahead this large is a new place

    static CaptureStore& instance();

    // Monotonic nanoseconds. Tests swap the clock to make stalls deterministic.
    using ClockFn = std::int64_t (*)();
    static std::int64_t now() noexcept;
    static void setClockForTesting(ClockFn fn) noexcept;

    // Off the audio thread only. Idempotent; the buffers are never freed while the process lives.
    void ensureAllocated();
    bool isAllocated() const noexcept { return m_allocated.load(std::memory_order_acquire); }

    void arm(Slot slot);
    void disarm(Slot slot);   // ends a pass in progress (PassEnd::Disarmed)
    void clear(Slot slot);
    bool isArmed(Slot slot) const noexcept;
    std::uint32_t generation(Slot slot) const noexcept;

    // The clip the user meant to learn, if the host ever tells us; enables fractionHeard.
    void setExpectedRange(Slot slot, std::optional<TimelineRange> range);

    // Audio thread. position is the block's timeline sample, if the host gave one.
    // selectionLength is the length in samples of what the user selected, if the host told us: a
    // pass then stops by itself once it has heard that much (a looping Preview keeps delivering
    // audio past the end of the selection, starting again from its top).
    BlockEvent captureBlock(Slot slot, const float* const* channels, int numChannels, int numSamples,
                            double sampleRate, std::optional<std::int64_t> position,
                            std::uint32_t instanceId, std::int64_t nowNs,
                            std::optional<std::int64_t> selectionLength = std::nullopt) noexcept;

    // Off the audio thread, from a processor's destructor: ends (and disarms) any pass that
    // instance owns, so another instance is not locked out until the stall time passes.
    std::array<PassEnd, kNumSlots> instanceGone(std::uint32_t instanceId);

    // Off the audio thread: ends (and disarms) passes that have had no audio for kStallNs.
    std::array<PassEnd, kNumSlots> checkStalls(std::int64_t nowNs);

    // peaks (optional) receives the thumbnail: one max-abs value per kPeakHop buffer samples.
    void snapshot(Slot slot, SlotSnapshot& out, std::vector<float>* peaks = nullptr) const;
    // The captured mono audio, buffer order (tests and later analysis).
    void copyAudio(Slot slot, std::vector<float>& out) const;
    // The captured audio channel by channel (one or two channels), where the slot keeps it: the
    // sound effect does, so that auditions and edits can be heard in stereo. Empty otherwise.
    void copyChannels(Slot slot, std::vector<std::vector<float>>& out) const;

    // Everything back to a fresh process (disarmed, empty). Tests only.
    void resetForTesting();

private:
    CaptureStore();

    struct SpinLock
    {
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
        bool tryLock() noexcept { return ! flag.test_and_set(std::memory_order_acquire); }
        void lock() noexcept;
        void unlock() noexcept { flag.clear(std::memory_order_release); }
    };

    struct SlotData
    {
        mutable SpinLock lock;
        std::atomic<bool> armed { false };
        std::atomic<std::uint32_t> generation { 0 };
        std::atomic<std::int64_t> lastBlockNs { 0 };
        std::atomic<std::int64_t> pendingSkip { 0 };   // samples the pass's owner lost to a failed try-lock
        std::atomic<std::int64_t> missedBlocks { 0 };

        // Guarded by lock.
        float* buffer = nullptr;
        float* channel[2] { nullptr, nullptr };   // separate channels, for slots that keep them
        int channels = 0;                         // how many of them hold audio
        float* peaks = nullptr;
        double sampleRate = 0.0;
        bool timelineKnown = false;
        std::int64_t origin = 0;            // timeline position of buffer[0]
        std::int64_t extent = 0;            // samples written (gaps zero-filled)
        std::array<TimelineRange, kMaxSegments> segments {};   // buffer offsets, sorted, disjoint
        int numSegments = 0;
        int passes = 0;
        float peak = 0.0f;
        bool truncated = false;
        std::uint32_t capturedBy = 0;
        PassEnd lastEnd = PassEnd::None;
        std::int64_t samplesSinceArm = 0;
        std::int64_t skippedOtherOwner = 0;
        std::optional<TimelineRange> expected;

        // The pass in progress.
        bool passActive = false;
        // Written with the lock held; atomic so an instance that fails the try-lock can still
        // tell whether the pass is its own (see pendingSkip).
        std::atomic<std::uint32_t> owner { 0 };
        bool passHasTimeline = false;
        bool frozen = false;
        int passBlocks = 0;
        std::int64_t passFirstPosition = 0;
        std::int64_t lastPosition = 0;
        std::int64_t expectedPosition = 0;
        std::int64_t writeOffset = 0;       // buffer offset of the next sample
        std::int64_t passStartOffset = 0;
        std::int64_t passSamples = 0;
        std::int64_t passLimit = 0;         // the selection's length in samples (0 = unknown)
    };

    SlotData& slotData(Slot slot) noexcept { return m_slots[static_cast<std::size_t>(slot)]; }
    const SlotData& slotData(Slot slot) const noexcept { return m_slots[static_cast<std::size_t>(slot)]; }

    // All of these run with the slot's lock held.
    static void resetData(SlotData& s) noexcept;
    static void beginPass(SlotData& s, BlockEvent& ev, double sampleRate, std::optional<std::int64_t> position,
                          std::uint32_t instanceId, std::optional<std::int64_t> selectionLength) noexcept;
    static void endPass(SlotData& s, PassEnd why) noexcept;
    static void addSegment(SlotData& s, TimelineRange r) noexcept;
    static void zeroGap(SlotData& s, std::int64_t upTo) noexcept;
    static void recomputePeaks(SlotData& s, std::int64_t from, std::int64_t to) noexcept;
    static std::int64_t limitFor(double sampleRate) noexcept;

    std::array<SlotData, kNumSlots> m_slots;
    std::atomic<bool> m_allocated { false };
    std::unique_ptr<float[]> m_storage;
    std::unique_ptr<float[]> m_channelStorage;   // the effect slot's separate channels
    std::unique_ptr<float[]> m_peakStorage;
};

} // namespace tonalrecall

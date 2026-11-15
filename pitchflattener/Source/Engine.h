#pragma once

// One plug-in instance's whole job, without the host: learn a sound, flatten it in the
// background, and play the flattened sound in place of the input whenever the input is that
// sound again.
//
//   Learn     armLearn(), then the sound is played once through process(). Capture starts at the
//             first block that is not silent (keeping 50 ms before it) and ends after a second of
//             silence, when the audio stops arriving for half a second (the host stopped), when
//             Learn is disarmed, or at two minutes. The audio passes through meanwhile.
//   Flatten   a background thread reads the sound's pitch (core::analyse) and renders it flat
//             (core::flatten). Changing the options renders again from the same reading.
//   Play      every block of input is kept in a ring that a second background thread searches
//             (Locator) while nothing is playing. When it says "input sample N is learned sample
//             S", the flattened sound is played so that the output for input sample t is
//             flattened[outputSampleFor(S) + (t - N)], faded in over 5 ms. The audio thread
//             then checks every block that the input still is the learned sound at the place it
//             expects, and fades back to the input when it is not (a jump, other audio, the
//             player stopped); the search starts again.
//
// Latency. The output is always the input delayed by a fixed kLatencySeconds (0.4 s). The
// output for input sample N leaves the plug-in 0.4 s after N arrives, and that is the time the
// search has: a probe of about 85 ms of the sound, plus the search itself (well under 0.25 s even
// for a two-minute sound at 96 kHz), plus a block. An answer that comes later than that still
// starts the flattened sound at the right place for that moment. Hosts that compensate latency
// see the flattened sound line up with the original.
//
// Threads:
//   - process() and processBypassed() run on the audio thread. They never allocate, lock or
//     free: results arrive as shared_ptr copies taken with a spin try-lock (a reference count
//     increment), and the engine keeps every result it published until nobody else holds it,
//     then frees it on its own thread.
//   - armLearn(), disarmLearn(), clear(), setOptions() and the readouts are for the message thread
//     (or tests); they take a mutex that the audio thread never touches.
//   - The analysis thread runs core::analyse / core::flatten; the house thread searches, ends
//     stalled captures and frees results nobody holds.
//
// One Engine per plug-in instance: several instances in one rack are independent. No JUCE.

#include "Locator.h"
#include "core/Flatten.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pitchflattener
{

/** One learned sound and its pitch reading. Shared by every render of it, never modified. */
struct Learned
{
    std::uint64_t id = 0;
    double sampleRate = 0.0;
    core::Channels channels;           // one or two
    core::Analysis analysis;
    Locator locator;                   // holds the channels' mix
};

/** One render of a learned sound. Shared, never modified. */
struct Result
{
    std::uint64_t generation = 0;
    std::shared_ptr<const Learned> learned;
    core::Options options;             // as rendered; line is the one actually used (-1 = none usable)
    core::Flattened flattened;

    bool playable() const noexcept
    {
        return flattened.valid && ! flattened.channels.empty() && ! flattened.channels[0].empty();
    }
};

class Engine
{
public:
    enum class State : int
    {
        Idle,               // nothing learned
        Armed,              // waiting for the sound to start
        Listening,          // capturing it
        Analysing,          // reading its pitch and rendering it
        Ready,              // learned and flattened: playing it again plays the flattened sound
        NothingToFlatten    // learned, but no steady pitch was found: it passes as it is
    };

    static constexpr double kLatencySeconds = 0.4;
    static constexpr double kMaxCaptureSeconds = 120.0;
    static constexpr double kPreRollSeconds = 0.05;
    static constexpr double kEndSilenceSeconds = 1.0;
    static constexpr double kStallSeconds = 0.5;
    static constexpr double kMinCaptureSeconds = 0.05;
    static constexpr double kFadeSeconds = 0.005;
    static constexpr int kMaxChannels = 2;
    // A window of the input this alike to the learned sound where it is expected still counts as
    // the same sound. Looser than the search's 0.98: the window is short, and once found, the
    // sound is only let go when it clearly is not there any more.
    static constexpr double kKeepCorrelation = 0.9;

    Engine();
    ~Engine();

    static int latencyFor(double sampleRate) noexcept;

    // ---- host side (never concurrent with process) ----
    void prepare(double sampleRate, int maxBlockSize);
    int latencySamples() const noexcept { return m_latency; }
    double sampleRate() const noexcept { return m_rate.load(); }
    /** Asks the audio thread to empty its delay line and stop playing (safe from any thread). */
    void requestReset() noexcept { m_resetRequested.store(true, std::memory_order_release); }
    /** Offline (non-realtime) the audio thread waits for the search instead of running ahead of it. */
    void setNonRealtime(bool nonRealtime) noexcept { m_nonRealtime.store(nonRealtime, std::memory_order_relaxed); }

    // ---- audio thread ----
    void process(float* const* channels, int numChannels, int numSamples) noexcept;
    /** Bypassed: the input delayed by the latency, nothing learned or played. */
    void processBypassed(float* const* channels, int numChannels, int numSamples) noexcept;

    // ---- message thread ----
    void armLearn();
    void disarmLearn();
    void clear();
    bool isLearnArmed() const noexcept;
    void setOptions(const core::Options& options);
    core::Options options() const;

    State state() const noexcept;
    /** The current result (null when nothing is learned). The caller may keep it as long as it likes. */
    std::shared_ptr<const Result> result() const;
    std::uint64_t resultGeneration() const noexcept { return m_resultGeneration.load(std::memory_order_acquire); }
    bool isRendering() const noexcept { return m_rendering.load(std::memory_order_acquire); }

    // ---- readouts (any thread) ----
    double captureSeconds() const noexcept;
    /** Where playback is in the flattened sound (samples), and the render it is in; -1 when none plays. */
    std::int64_t playPosition() const noexcept { return m_playPosition.load(std::memory_order_relaxed); }
    std::uint64_t playGeneration() const noexcept { return m_playGeneration.load(std::memory_order_relaxed); }
    bool isLocated() const noexcept { return m_located.load(std::memory_order_relaxed); }
    float inputLevel() const noexcept { return m_inputLevel.load(std::memory_order_relaxed); }
    /** The learned sound was recorded at another rate than the host runs at now: it is not played. */
    bool sampleRateMismatch() const noexcept { return m_rateMismatch.load(std::memory_order_relaxed); }

    // ---- tests ----
    /** Waits until nothing is captured, analysed or rendered any more (false on timeout). */
    bool waitUntilIdle(double timeoutSeconds) const;
    /** Searches finished (found or not), for the timings. */
    std::uint64_t searchesDone() const noexcept { return m_searches.load(std::memory_order_relaxed); }
    double lastSearchMilliseconds() const noexcept { return m_lastSearchMs.load(std::memory_order_relaxed); }
    /** The last place found: input sample and the learned sample it is. */
    std::int64_t lastMatchInput() const noexcept { return m_lastMatchInput.load(std::memory_order_relaxed); }
    std::int64_t lastMatchSource() const noexcept { return m_lastMatchSource.load(std::memory_order_relaxed); }

private:
    enum CaptureState : int { CapIdle = 0, CapArmed = 1, CapListening = 2, CapFinished = 3 };
    enum class Match : int { Off, Waiting, Playing };

    struct SpinLock
    {
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
        bool tryLock() noexcept { return ! flag.test_and_set(std::memory_order_acquire); }
        void lock() noexcept;
        void unlock() noexcept { flag.clear(std::memory_order_release); }
    };

    // What the output plays: the delayed input (result == nullptr) or a render from a point.
    struct Voice
    {
        const Result* result = nullptr;
        std::int64_t flatOffset = 0;   // flattened index = input position + flatOffset
        // The level the sound is being played at against the level it was learned at: the
        // flattened sound follows it, so turning the player down turns the result down too.
        float gain = 1.0f;
        bool operator==(const Voice& o) const noexcept { return result == o.result && flatOffset == o.flatOffset && ! (gain < o.gain) && ! (gain > o.gain); }
    };
    // A switch of the output to a voice at an input position (the output reaches it latency later).
    struct Event
    {
        std::int64_t at = 0;
        Voice voice;
    };

    // ---- capture (audio side) ----
    void captureBlock(const float* const* channels, int numChannels, int numSamples, float peak) noexcept;
    void waitForCaptureWriter() const noexcept;
    void finaliseCaptureLocked();                 // m_mutex held
    void ensureCaptureBufferLocked(double rate);  // m_mutex held

    // ---- playback (audio thread) ----
    void writeInput(const float* const* channels, int numChannels, int numSamples, float& peak) noexcept;
    void handleReset() noexcept;
    void updateMatching(std::int64_t t0, int numSamples) noexcept;
    void pickUpResult(std::int64_t outNext) noexcept;
    void beginSearch(std::int64_t from) noexcept;
    bool readAnswer(const Result& r, std::int64_t outNext) noexcept;
    void verify(const Result& r, std::int64_t tEnd, std::int64_t outNext) noexcept;
    void waitForSearch(std::int64_t tEnd) noexcept;
    void pushEvent(const Event& e) noexcept;
    void renderOutput(float* const* channels, int numChannels, int numSamples, std::int64_t t0) noexcept;
    float voiceSample(const Voice& v, int channel, int numChannels, std::int64_t u, float dry) const noexcept;
    bool isReferenced(const Result* r) const noexcept;

    // ---- results ----
    void publish(std::shared_ptr<const Result> result);   // m_mutex held
    bool tryAcquireResult(std::shared_ptr<const Result>& out) const noexcept;
    void freeUnusedResults();

    // ---- threads ----
    void analysisLoop();
    void houseLoop();
    void searchStep();
    void kickHouse() noexcept;

    // ---- host settings ----
    std::atomic<double> m_rate { 0.0 };   // read by the house thread too
    int m_latency = 0;
    int m_fadeLength = 1;
    int m_verifyWindow = 1024;
    std::atomic<bool> m_prepared { false };
    std::atomic<bool> m_nonRealtime { false };
    std::atomic<bool> m_resetRequested { false };

    // ---- control (guarded by m_mutex) ----
    mutable std::mutex m_mutex;
    core::Options m_options;
    std::atomic<std::uint64_t> m_optionsGeneration { 1 };
    std::uint64_t m_renderedOptions = 0;                 // the options generation last rendered
    std::shared_ptr<const Learned> m_learned;             // the reading renders are made from
    std::unique_ptr<core::Channels> m_pendingCapture;     // waiting for the analysis thread
    double m_pendingRate = 0.0;
    std::atomic<std::uint64_t> m_captureEpoch { 0 };      // bumped by every capture and Clear: older work stops
    std::atomic<bool> m_analysing { false };
    std::atomic<bool> m_rendering { false };
    bool m_disarmedByUser = false;
    std::vector<std::shared_ptr<const Result>> m_published;   // every result maybe still held
    std::uint64_t m_nextGeneration = 1;

    // ---- capture buffers (allocated off the audio thread; written by it while armed) ----
    std::array<std::unique_ptr<float[]>, kMaxChannels> m_capture;
    std::int64_t m_captureCapacity = 0;
    std::atomic<int> m_captureState { CapIdle };
    std::atomic<bool> m_captureWriting { false };
    std::atomic<std::int64_t> m_captureLength { 0 };
    std::atomic<int> m_captureChannels { 0 };
    std::atomic<double> m_captureRate { 0.0 };
    std::atomic<std::uint64_t> m_blocks { 0 };            // the house thread watches this for a stall
    // Audio thread: the 50 ms before the sound starts, and the silence at the end so far.
    std::array<std::vector<float>, kMaxChannels> m_preRoll;
    int m_preRollPosition = 0;
    int m_preRollFill = 0;
    std::int64_t m_silentRun = 0;
    int m_lastCaptureState = CapIdle;

    // ---- the published result ----
    mutable SpinLock m_resultLock;
    std::shared_ptr<const Result> m_result;               // guarded by m_resultLock
    std::atomic<std::uint64_t> m_resultGeneration { 0 };

    // ---- input ring (audio thread writes, house thread reads) ----
    static constexpr int kRingBits = 20;
    static constexpr std::int64_t kRingSize = std::int64_t { 1 } << kRingBits;   // 10.9 s at 96 kHz
    static constexpr std::int64_t kRingMask = kRingSize - 1;
    std::unique_ptr<std::atomic<float>[]> m_ring;
    std::atomic<std::int64_t> m_ringWritten { 0 };

    // ---- search hand-over ----
    std::atomic<bool> m_searching { false };
    std::atomic<std::uint32_t> m_searchEpoch { 0 };
    std::atomic<std::int64_t> m_searchFrom { 0 };
    // The answer, as a sequence lock: odd while the house thread writes it.
    std::atomic<std::uint32_t> m_answerSequence { 0 };
    std::atomic<std::uint32_t> m_answerEpoch { 0 };
    std::atomic<std::uint64_t> m_answerLearned { 0 };
    std::atomic<std::int64_t> m_answerInput { 0 }, m_answerSource { 0 };
    std::atomic<int> m_answerLength { 0 };
    std::atomic<double> m_answerGain { 1.0 };
    // What the house thread needs before it can do more for an epoch (offline waits on this).
    std::atomic<std::uint32_t> m_workerEpoch { 0 };
    std::atomic<std::int64_t> m_workerNeed { 0 };
    std::atomic<std::uint64_t> m_searches { 0 };
    std::atomic<double> m_lastSearchMs { 0.0 };

    // House thread's own search state.
    std::uint32_t m_wEpoch = 0xffffffffu;
    std::uint64_t m_wLearned = 0;
    std::int64_t m_wCursor = 0, m_wAttempt = -1, m_wRunStart = 0;

    // ---- audio thread only ----
    std::array<std::vector<float>, kMaxChannels> m_delay;
    int m_delayPosition = 0;
    std::int64_t m_inputPosition = 0;
    Voice m_current, m_previous;
    int m_fadePosition = 0;
    std::array<Event, 16> m_events {};
    int m_numEvents = 0;
    std::array<std::shared_ptr<const Result>, 8> m_held;   // keeps every render the output may use alive
    const Result* m_latest = nullptr;
    std::uint64_t m_seenGeneration = 0;
    Match m_match = Match::Off;
    std::uint32_t m_epoch = 0;
    std::int64_t m_segmentStart = 0, m_sourceOffset = 0, m_flatOffset = 0, m_verifyPosition = 0;
    double m_gain = 1.0;

    // ---- readouts ----
    std::atomic<std::int64_t> m_playPosition { -1 };
    std::atomic<std::uint64_t> m_playGeneration { 0 };
    std::atomic<bool> m_located { false };
    std::atomic<float> m_inputLevel { 0.0f };
    std::atomic<bool> m_rateMismatch { false };
    std::atomic<std::int64_t> m_lastMatchInput { -1 }, m_lastMatchSource { -1 };

    // ---- threads ----
    std::mutex m_threadMutex;
    std::condition_variable m_analysisWake, m_houseWake;
    std::atomic<bool> m_stop { false };
    std::atomic<bool> m_houseKick { false };
    std::thread m_analysisThread, m_houseThread;
};

/** True when two option sets render the same. */
bool sameOptions(const core::Options& a, const core::Options& b) noexcept;

} // namespace pitchflattener

#pragma once

// One plug-in instance's whole job, without the host: learn a sound, flatten it in the
// background, and play the flattened sound in place of the input whenever the input is that
// recording again.
//
//   Learn     armLearn(), then the sound is played once through process() (or processBypassed():
//             the input is still the input). Capture starts at the first block that is not silent
//             (keeping 50 ms before it) and ends after a second of silence, when the audio stops
//             arriving for half a second (the host stopped), when Learn is disarmed, or at two
//             minutes. A block is silent below -70 dBFS, or more than 50 dB under the loudest block
//             captured so far (a field recording's noise floor). The audio passes through meanwhile.
//   Flatten   a background thread reads the sound's pitch (core::analyse) and renders it flat
//             (core::flatten). Changing the options renders again from the same reading. While a
//             newly captured sound is analysed (the window says ANALYSING), the input passes dry:
//             the sound learned before stays published, unplayed, and is replaced only when the new
//             one is published, so cancelLearn() can simply go back to it.
//   Play      every block of input is kept in a ring that a second background thread searches
//             (Locator) while nothing is playing. When it says "input sample N is learned sample
//             S", the start of the flattened sound is scheduled for when the output reaches N
//             (the latency later): the output for input sample t is then
//             flattened[outputSampleFor(S) + (t - N)], faded in over 5 ms. Every window of input
//             (about 21 ms) is checked against the learned sound where it should be, from the
//             probe onwards, including the stretch before the start is heard; a window that fails
//             cancels a start not yet heard, or fades back to the input ending at the failure's
//             input position. What is checked is "this is the learned recording": see matching::
//             in Locator.h. The level is followed window by window, so a fade on the player comes
//             out as a fade.
//
// The two timelines. In Varispeed the flattened sound is shorter or longer than the learned one,
// so the output's moment of the sound runs ahead of, or behind, the input's. That is inherent: the
// engine plays the render one sample per sample from the matched point, and checks the input
// against the learned sound at the input's own moment. When the input stops being the learned
// sound, the output returns to the input at that input position, whatever moment of the render was
// being heard. With Keep Length the timelines are one to one, so nothing past the checked input is
// ever played. When the learned sound ends and its render is longer, the rest of the render plays
// out (the tail) until the input is no longer silent.
//
// Latency. The output is always the input delayed by latencySamples(): max(0.4 s, probe + block +
// search budget), with a probe of about 85 ms and a search budget of 0.25 s (a two-minute sound
// searched end to end takes 0.07 to 0.18 s here at any rate, see Locator.h). So it is 0.4 s for
// blocks of up to 2048 samples at 44.1 or 48 kHz and 4096 at 88.2 or 96 kHz; a 32768-sample block
// at 48 kHz needs 1.0 s. The output for input sample N leaves the plug-in that much later, which is
// the time the search and the first checks have. Blocks larger than prepared for still play
// correctly, but the start of a sound can then play unflattened while the answer is late (and on a
// machine too busy to search within the budget it starts late, at the right place for that
// moment). Hosts that compensate latency see the flattened sound line up with the original.
//
// Threads:
//   - process() and processBypassed() run on the audio thread. In real time they never allocate,
//     lock or free: results arrive as shared_ptr copies taken with a spin try-lock (a reference
//     count increment), and the engine keeps every result it published until nobody else holds
//     it, then frees it on its own thread. Offline (setNonRealtime(true)) the audio thread may also
//     wait for the search or for a render, and wakes the house thread with a condition variable
//     (which can take a lock inside): the host is rendering as fast as it can, and waiting is what
//     keeps a bounce identical to a real-time pass.
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
    // Where the sound itself ends in the mix (after its last sample above -70 dBFS): the capture
    // keeps 10 ms of quiet after it, which is not part of the sound when it is played again.
    std::int64_t soundEnd = 0;
    // Stereo only: half the difference of the channels, followed by a probe's length of zeros
    // (as the locator's mix is), so a copy with its channels swapped is told apart.
    std::vector<float> side;
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

    // What the output is doing, for the window (one answer instead of several guesses).
    enum class Playback : int
    {
        Idle,               // nothing playable (nothing learned, learning, another rate, bypassed)
        Searching,          // the input is passing through while the engine looks for the sound
        Flattened,          // the flattened sound is being heard now
        FlattenedEnded,     // the matched sound still plays in the input, but its shorter render has finished: the output is silent
        Tail                // the render is longer and still plays after the sound ended
    };

    static constexpr double kLatencySeconds = 0.4;        // the least latency (see the header)
    static constexpr double kSearchBudgetSeconds = 0.25;  // what the latency allows the search
    static constexpr double kMaxCaptureSeconds = 120.0;
    static constexpr double kPreRollSeconds = 0.05;
    static constexpr double kEndSilenceSeconds = 1.0;
    static constexpr double kStallSeconds = 0.5;
    static constexpr double kMinCaptureSeconds = 0.05;
    static constexpr double kFadeSeconds = 0.005;
    static constexpr double kRelativeSilenceDb = -50.0;   // a block this far under the loudest one captured is silent
    static constexpr int kGainSmoothingWindows = 3;   // about 60 ms, centred (the latency gives the look-ahead)
    static constexpr int kMaxChannels = 2;

    Engine();
    ~Engine();

    /** The latency for a rate and the largest block the host will send (see the header). */
    static int latencyFor(double sampleRate, int maxBlockSize) noexcept;

    // ---- host side (never concurrent with process) ----
    void prepare(double sampleRate, int maxBlockSize);
    int latencySamples() const noexcept { return m_latency; }
    double sampleRate() const noexcept { return m_rate.load(); }
    /** Asks the audio thread to empty its delay line and stop playing (safe from any thread). */
    void requestReset() noexcept { m_resetRequested.store(true, std::memory_order_release); }
    /** Offline (non-realtime) the audio thread waits for the search and renders instead of running ahead of them. */
    void setNonRealtime(bool nonRealtime) noexcept { m_nonRealtime.store(nonRealtime, std::memory_order_relaxed); }

    // ---- audio thread ----
    void process(float* const* channels, int numChannels, int numSamples) noexcept;
    /** Bypassed: the input delayed by the latency, nothing played; Learn still captures. */
    void processBypassed(float* const* channels, int numChannels, int numSamples) noexcept;

    // ---- message thread ----
    void armLearn();
    void disarmLearn();
    /**
     * Stops Learn without learning anything: disarms, throws away a capture in progress, abandons
     * an analysis in flight. The sound learned before (if any) stays, and plays again; the state
     * returns to Ready (or NothingToFlatten), or Idle if nothing was learned.
     */
    void cancelLearn();
    /** Forgets everything learned (and stops Learn). */
    void clear();
    bool isLearnArmed() const noexcept;
    void setOptions(const core::Options& options);
    core::Options options() const;

    State state() const noexcept;
    /** The current result (null when nothing is learned). The caller may keep it as long as it likes. */
    std::shared_ptr<const Result> result() const;
    std::uint64_t resultGeneration() const noexcept { return m_resultGeneration.load(std::memory_order_acquire); }
    bool isRendering() const noexcept { return m_rendering.load(std::memory_order_acquire); }

    // ---- readouts (any thread, lock-free) ----
    double captureSeconds() const noexcept;
    /** Where playback is in the flattened sound (samples), and the render it is in; -1 when none plays. */
    std::int64_t playPosition() const noexcept { return m_playPosition.load(std::memory_order_relaxed); }
    std::uint64_t playGeneration() const noexcept { return m_playGeneration.load(std::memory_order_relaxed); }
    /** What the output is doing (as of the last block). */
    Playback playback() const noexcept { return static_cast<Playback>(m_playback.load(std::memory_order_relaxed)); }
    /** The render being heard is the current one (false while an older render plays out, or none plays). */
    bool playingCurrentRender() const noexcept { return m_playingCurrent.load(std::memory_order_relaxed); }
    /** An offline wait (for the search or a render) gave up, so that bounce may not match a real-time pass. */
    bool offlineWaitGaveUp() const noexcept { return m_waitGaveUp.load(std::memory_order_relaxed); }
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
    /** Matches found but cancelled before they were heard (the input stopped being the sound). */
    std::uint64_t cancelledStarts() const noexcept { return m_cancelledStarts.load(std::memory_order_relaxed); }

private:
    enum CaptureState : int { CapIdle = 0, CapArmed = 1, CapListening = 2, CapFinished = 3 };
    enum class Match : int { Off, Waiting, Playing, Tail };

    struct SpinLock
    {
        std::atomic_flag flag;   // clear (C++20 default-initialises it)
        bool tryLock() noexcept { return ! flag.test_and_set(std::memory_order_acquire); }
        void lock() noexcept;
        void unlock() noexcept { flag.clear(std::memory_order_release); }
    };

    // What the output plays: the delayed input (result == nullptr) or a render from a point.
    struct Voice
    {
        const Result* result = nullptr;
        std::int64_t flatOffset = 0;     // flattened index = input position + flatOffset
        std::int64_t sourceOffset = 0;   // learned index = input position + sourceOffset (for the readouts)
        float gain = 1.0f;               // the level a new segment starts at
        bool newSegment = false;         // a new match (its level starts afresh), not a new render of the same one
        bool operator==(const Voice& o) const noexcept { return result == o.result && flatOffset == o.flatOffset; }
    };
    // A switch of the output to a voice at an input position (the output reaches it latency later).
    struct Event
    {
        std::int64_t at = 0;
        Voice voice;
    };
    // The level the input is played at against the learned sound, from an input position on.
    struct GainPoint
    {
        std::int64_t at = 0;
        float gain = 1.0f;
    };

    // ---- capture ----
    void captureBlock(const float* const* channels, int numChannels, int numSamples, float peak) noexcept;
    void waitForCaptureWriter() const noexcept;
    void finaliseCaptureLocked();                 // m_mutex held
    void ensureCaptureBufferLocked(double rate);  // m_mutex held

    // ---- playback (audio thread) ----
    void writeInput(const float* const* channels, int numChannels, int numSamples, float& peak) noexcept;
    void handleReset() noexcept;
    void updateMatching(std::int64_t t0, int numSamples) noexcept;
    void pickUpResult(std::int64_t outNext) noexcept;
    void releaseUnheld() noexcept;
    void beginSearch(std::int64_t from) noexcept;
    void stopMatching(std::int64_t outNext) noexcept;
    bool readAnswer(const Result& r, std::int64_t outNext) noexcept;
    void verify(const Result& r, std::int64_t tEnd, std::int64_t outNext) noexcept;
    void failMatch(std::int64_t at, std::int64_t outNext) noexcept;
    void watchTail(std::int64_t tEnd, std::int64_t outNext) noexcept;
    void pushGain(std::int64_t at, double gain) noexcept;
    void followGain(std::int64_t windowStart, double gain) noexcept;
    void waitForSearch(std::int64_t tEnd) noexcept;
    void waitForRender() noexcept;
    void pushEvent(const Event& e) noexcept;
    void renderOutput(float* const* channels, int numChannels, int numSamples, std::int64_t t0) noexcept;
    float voiceSample(const Voice& v, int channel, int numChannels, std::int64_t u, float dry) const noexcept;
    bool isReferenced(const Result* r) const noexcept;
    void publishReadouts(std::int64_t lastOutput) noexcept;
    bool rateDiffers(double learnedRate) const noexcept;

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
    std::atomic<bool> m_renderPending { false };          // options changed and their render is not published yet
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
    // Audio thread: the 50 ms before the sound starts, the silence at the end so far, the loudest block.
    std::array<std::vector<float>, kMaxChannels> m_preRoll;
    int m_preRollPosition = 0;
    int m_preRollFill = 0;
    std::int64_t m_silentRun = 0;
    float m_captureLoudest = 0.0f;
    int m_lastCaptureState = CapIdle;

    // ---- the published result ----
    mutable SpinLock m_resultLock;
    std::shared_ptr<const Result> m_result;               // guarded by m_resultLock
    std::atomic<std::uint64_t> m_resultGeneration { 0 };
    std::atomic<std::uint64_t> m_currentRender { 0 };     // the published render's generation (0 = none)

    // ---- input rings (audio thread writes, house thread reads): the mix, and half the difference ----
    static constexpr int kRingBits = 20;
    static constexpr std::int64_t kRingSize = std::int64_t { 1 } << kRingBits;   // 10.9 s at 96 kHz
    static constexpr std::int64_t kRingMask = kRingSize - 1;
    // Data older than this much short of a full ring is never read: the audio thread may be
    // writing over it while it is read (a block of up to this size can arrive meanwhile).
    static constexpr std::int64_t kRingGuard = 65536;
    std::unique_ptr<std::atomic<float>[]> m_ring;
    std::unique_ptr<std::atomic<float>[]> m_sideRing;
    std::atomic<std::int64_t> m_ringWritten { 0 };
    std::atomic<std::int64_t> m_fence { 0 };              // the input position of the last reset: nothing before it is used

    // ---- search hand-over ----
    std::atomic<bool> m_searching { false };
    std::atomic<bool> m_active { false };                 // something playable is being matched: the house thread keeps its quick pace
    std::atomic<std::uint32_t> m_searchEpoch { 0 };
    std::atomic<std::int64_t> m_searchFrom { 0 };
    std::atomic<std::uint64_t> m_failures { 0 };          // matches that stopped holding (the house thread backs off)
    std::atomic<std::uint64_t> m_holds { 0 };             // matches that held for a second (the back-off starts again)
    // The answer, as a sequence lock: odd while the house thread writes it.
    std::atomic<std::uint32_t> m_answerSequence { 0 };
    std::atomic<std::uint32_t> m_answerEpoch { 0 };
    std::atomic<std::uint64_t> m_answerLearned { 0 };
    std::atomic<std::int64_t> m_answerInput { 0 }, m_answerSource { 0 };
    std::atomic<int> m_answerLength { 0 };
    std::atomic<double> m_answerGain { 1.0 }, m_answerGainEnd { 1.0 };
    // What the house thread needs before it can do more for an epoch (offline waits on this).
    std::atomic<std::uint32_t> m_workerEpoch { 0 };
    std::atomic<std::int64_t> m_workerNeed { 0 };
    std::atomic<std::uint64_t> m_searches { 0 };
    std::atomic<double> m_lastSearchMs { 0.0 };

    // House thread's own search state.
    std::uint32_t m_wEpoch = 0xffffffffu;
    std::uint64_t m_wLearned = 0;
    std::int64_t m_wCursor = 0, m_wAttempt = -1, m_wRunStart = -1;
    std::int64_t m_wGate = 0;          // no attempt starts before this input position (the back-off)
    std::int64_t m_wBackoff = 0;       // the next back-off, in samples
    std::int64_t m_wSilentRun = 0;
    std::uint64_t m_wFailuresSeen = 0, m_wHoldsSeen = 0;

    // ---- audio thread only ----
    std::array<std::vector<float>, kMaxChannels> m_delay;
    int m_delayPosition = 0;
    std::int64_t m_inputPosition = 0;
    bool m_inputStereo = false;
    Voice m_current, m_previous;
    int m_fadePosition = 0;
    std::array<Event, 16> m_events {};
    int m_numEvents = 0;
    std::array<std::shared_ptr<const Result>, 8> m_held;   // keeps every render the output may use alive
    const Result* m_latest = nullptr;
    const Result* m_segmentResult = nullptr;               // the render the current match plays
    std::uint64_t m_seenGeneration = 0;
    Match m_match = Match::Off;
    std::uint32_t m_epoch = 0;
    std::int64_t m_matchInput = 0, m_segmentStart = 0, m_sourceOffset = 0, m_flatOffset = 0, m_verifyPosition = 0;
    std::int64_t m_tailEnd = 0;
    bool m_holdReported = false;
    double m_gainSmoothed = 1.0;
    std::array<double, 2> m_gainHistory {};   // the last two windows' levels, for a centred average
    int m_gainHistoryCount = 0;
    // The level, in input time, and how the output follows it.
    std::array<GainPoint, 256> m_gainPoints {};
    int m_gainHead = 0, m_gainTail = 0;
    float m_gainNow = 1.0f, m_gainStep = 0.0f;
    std::int64_t m_gainRampLeft = 0;

    // ---- readouts ----
    std::atomic<std::int64_t> m_playPosition { -1 };
    std::atomic<std::uint64_t> m_playGeneration { 0 };
    std::atomic<int> m_playback { static_cast<int>(Playback::Idle) };
    std::atomic<bool> m_playingCurrent { false };
    std::atomic<bool> m_waitGaveUp { false };
    std::atomic<bool> m_located { false };
    std::atomic<float> m_inputLevel { 0.0f };
    std::atomic<bool> m_rateMismatch { false };
    std::atomic<std::int64_t> m_lastMatchInput { -1 }, m_lastMatchSource { -1 };
    std::atomic<std::uint64_t> m_cancelledStarts { 0 };

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

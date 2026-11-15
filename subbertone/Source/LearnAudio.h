#pragma once

#include "LearnStore.h"
#include "LearnTypes.h"
#include "SubharmonicEngine.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

/**
 * LearnAudio: the part of LEARN that runs on the audio thread.
 *
 * The processor hands it what the host's play head said about each block (Transport) and then, chunk by chunk, the
 * mono sum the engine is about to process. LearnAudio
 *   - follows the timeline: whether the host reports a playing position at all, and whether each block continues
 *     the last one, jumps, wraps at a loop end or stalls (the VST3 pre-roll reports 0 again and again);
 *   - listens: a 3.2 kHz anti-alias low-pass at the host rate, then decimation to about 12 kHz on the absolute
 *     timeline grid (it keeps the samples at positions that are multiples of the decimation), and a fingerprint of
 *     every 10 ms hop of the timeline: its level and the share of six octave bands;
 *   - captures one pass of the loop into a buffer the message thread prepared, for the worker to analyse;
 *   - checks what it hears against the fingerprints of what was learned there, and against the learned pitch (a
 *     note moved by a semitone leaves the fingerprints nearly as they were), and falls back to live tracking where
 *     it no longer matches (an edit, another take, a change before Subbertone);
 *   - fills the engine's PitchGuide for every sample: the learned line at that sample's timeline position, and the
 *     line one glide time ahead.
 *
 * Every decision is tied to timeline positions and the audio, never to where a host block starts or ends, so a
 * bounce makes the same decisions as playback did, whatever its block size.
 *
 * It never locks, allocates, frees or waits. Its buffers are allocated on other threads before it uses them
 * (prepare(), ensureListening(), CaptureBuffer), and everything it shares with them goes through atomics. Until
 * LEARN is first used it does nothing at all: beginBlock() returns at once and processChunk() returns no guide, so
 * the engine runs the loop it always ran.
 *
 * Plain C++20, no JUCE: the processor translates the play head into a Transport.
 */
namespace subbertone::learn
{
    //==============================================================================
    /** What the host's play head reported for one block. */
    struct Transport
    {
        bool hasTime = false;            // a position for the block's first sample (in samples, or seconds converted)
        std::int64_t timeInSamples = 0;
        bool isPlaying = false;
        bool isLooping = false;
        bool hasLoopPoints = false;      // the loop's start and end, in quarter notes
        double loopStartPpq = 0.0;
        double loopEndPpq = 0.0;
        bool hasPpq = false;             // the quarter-note position of the block's first sample
        double ppq = 0.0;
        bool hasBpm = false;
        double bpm = 0.0;
    };

    //==============================================================================
    /** The fingerprint's signal path at one host rate: an 8th-order Butterworth low-pass at 3.2 kHz whatever the
        rate (a corner that moved with the rate would make prints taken at different rates disagree), the decimation
        factor to about 12 kHz, and the six octave bands, each a 4th-order Butterworth band-pass run on the decimated
        signal. Shared by the listener and the alignment job, so both measure exactly alike. */
    class PrintFilters
    {
    public:
        static constexpr int numBands = Fingerprint::numBands;
        using Bands = std::array<double, numBands>;

        void prepare(double hostRate);
        void reset() noexcept;

        int getDecimation() const noexcept { return m_decimation; }
        double getCaptureRate() const noexcept { return m_captureRate; }

        /** Every host sample. */
        float antiAlias(float x) noexcept
        {
            for (auto& stage : m_antiAlias)
                x = stage.process(x);

            return x;
        }

        /** Every decimated sample: the output of each band. */
        void bands(float y, Bands& out) noexcept
        {
            for (std::size_t b = 0; b < m_bands.size(); ++b)
                out[b] = m_bands[b][1].process(m_bands[b][0].process(static_cast<double>(y)));
        }

        /** Every decimated sample of a whole buffer from a reset state (the alignment job). */
        void bandsOf(const float* y, int count, std::vector<Bands>& out);

    private:
        struct LowPass
        {
            float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
            float z1 = 0.0f, z2 = 0.0f;

            float process(float x) noexcept
            {
                const float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
        };

        struct BandSection
        {
            double b0 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;   // b1 is 0: zeros at DC and Nyquist
            double z1 = 0.0, z2 = 0.0;

            double process(double x) noexcept
            {
                const double y = b0 * x + z1;
                z1 = -a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
        };

        std::array<LowPass, 4> m_antiAlias {};
        std::array<std::array<BandSection, 2>, numBands> m_bands {};
        int m_decimation = 4;
        double m_captureRate = 12000.0;
    };

    /** The energy of one hop, summed over its decimated samples. */
    struct HopEnergy
    {
        double total = 0.0;
        PrintFilters::Bands band {};
        int count = 0;
        bool clipped = false;

        void add(float y, const PrintFilters::Bands& bands) noexcept
        {
            total += static_cast<double>(y) * static_cast<double>(y);

            for (std::size_t b = 0; b < band.size(); ++b)
                band[b] += bands[b] * bands[b];

            ++count;
        }
    };

    /** The fingerprint of a hop (flagged complete; the caller decides whether it really was). */
    Fingerprint makeFingerprint(const HopEnergy& energy) noexcept;

    /** Whether a hop heard now differs from the learned one. Gain-invariant: the level itself is only compared as
        "audible or not" (-70 dBFS), its change from the previous hop must agree within 3 dB (pass -1 for a previous
        hop that was not compared, which skips this), and every band that carries a real share of either hop must
        agree within 2 dB. */
    bool printsDiffer(const Fingerprint& heard, const Fingerprint& learned, int previousHeardLevel,
                      int previousLearnedLevel) noexcept;

    /** The pitch test, for a note moved after learning (a MIDI edit, pitch correction): that leaves the fingerprints
        nearly as they were. `x` holds `count` decimated samples, oldest first; x[i] is compared with x[i + lag] for
        i < window and every lag within 350 cents of `period` (the learned curve's period in decimated samples, at the
        middle of what is compared) that fits. True where the audio repeats closely (normalised difference under 0.1)
        at a lag more than 70 cents from the period while it repeats clearly worse at the period itself, unless that
        lag lies on another line the analysis found (`candidates`, ratios to the curve) at any octave. */
    bool pitchDiffers(const float* x, int count, int window, double period, const Candidate* candidates,
                      int numCandidates) noexcept;

    /** What pitchDiffers() compares for a curve's period: the window (two periods, at least c_pitchMinWindow) and the
        samples it needs in all (the window plus the longest lag). False below about 25 Hz at the capture rate. */
    bool pitchWindowFor(double period, int& window, int& span) noexcept;

    inline constexpr int c_pitchMinWindow = 120;      // decimated samples: a hop at 12 kHz
    inline constexpr int c_pitchMaxSpan = 2560;       // the most samples it compares (a 25 Hz curve at 18 kHz)
    inline constexpr int c_pitchMaxLags = 320;

    /** The verifier's memory of the last compared hops: a region falls back to live tracking once 4 of the last 5
        hops differ, and follows again once at most 2 of the last 10 do. It starts trusting on every pass. */
    class MatchWindow
    {
    public:
        void reset() noexcept { m_bits = 0; m_count = 0; m_valid = true; }

        /** Adds a hop; true if this changed valid(). */
        bool push(bool differs) noexcept;

        bool valid() const noexcept { return m_valid; }

    private:
        std::uint32_t m_bits = 0;   // newest in bit 0
        int m_count = 0;
        bool m_valid = true;
    };

    //==============================================================================
    /**
     * One capture: the decimated audio and the fingerprints of the timeline the audio thread heard while LEARN was
     * armed, in contiguous segments (a jump starts a new one), plus what it should be learned as.
     *
     * Allocated on the message thread for one host rate. Ownership moves with `state`: the message thread offers it,
     * the audio thread takes it and fills it, stores `captured` (release) after its last write and never touches it
     * again; the worker loads `captured` (acquire), analyses it and keeps it (`retained`) so it can be analysed
     * again with another method. A buffer offered for a rate the host has left by the time the audio thread sees the
     * request is handed back (`free`) instead. One that was offered is freed only after LearnAudio::withdrawOffer().
     */
    class CaptureBuffer
    {
    public:
        enum State : int
        {
            free = 0,     // the message thread's
            offered,      // published for the audio thread to take
            capturing,    // the audio thread's
            captured,     // complete: waiting for the worker
            analysing,    // the worker's
            retained      // analysed, kept for another analysis; the message thread may reuse it
        };

        struct Segment
        {
            std::int64_t start = 0;            // timeline samples heard: [start, end)
            std::int64_t end = 0;
            std::int64_t firstSample = 0;      // timeline position of samples[sampleOffset]
            int sampleOffset = 0;              // decimated samples, at positions firstSample + i * decimation
            int numSamples = 0;
            std::int64_t firstHop = 0;         // hop of prints[printOffset]; hops follow on without gaps
            int printOffset = 0;               // only hops heard whole (after the filters settled) are kept
            int numPrints = 0;
        };

        static constexpr int c_maxSegments = 64;

        explicit CaptureBuffer(double hostRate);

        const double sampleRate;
        const int decimation;
        const int hopSamples;
        const double captureRate;

        std::vector<float> samples;
        std::vector<Fingerprint> prints;
        std::array<Segment, c_maxSegments> segments {};
        int numSegments = 0;

        // Stretches the host skipped right after the input fell silent: it stopped calling the plug-in, and called it
        // again with a jump forward when audio returned. They count as heard silence.
        struct Span
        {
            std::int64_t start = 0;            // timeline samples skipped: [start, end)
            std::int64_t end = 0;
        };

        std::array<Span, c_maxSegments> silentSpans {};
        int numSilentSpans = 0;

        // What to learn, written by the audio thread before it stores `captured`
        std::uint32_t generation = 0;     // the request it answered
        bool onLoop = false;              // the loop [loopStart, loopEnd) was heard (gaps of up to maxGapSamples) ...
        std::int64_t loopStart = 0;
        std::int64_t loopEnd = 0;
        std::int64_t maxGapSamples = 0;
        int segmentToLearn = -1;          // ... or, without a loop, this segment
        float thresholdDb = -40.0f;       // the Threshold knob when it was complete

        std::atomic<int> state { free };

        double getSeconds(std::int64_t samplesOfHost) const noexcept { return static_cast<double>(samplesOfHost) / sampleRate; }
    };

    //==============================================================================
    /** About a second of what the audio thread heard in a region whose pass did not match from its start, handed to
        an alignment job (it looks for the offset at which it does match). */
    struct AlignmentRequest
    {
        enum State : int { idle = 0, filled, taken };

        std::atomic<int> state { idle };

        // Requests the job did not get to the end of (stopped, out of memory, the region gone or moved meanwhile): bit
        // k for the audio thread's attempt k. The worker sets them, the audio thread takes them: such a request does
        // not count as tried, so the same region and alignment may be asked for again (say after UNDO).
        std::atomic<std::uint32_t> notMade { 0 };

        // Written by the audio thread before it stores `filled`
        int attempt = 0;                       // which of the audio thread's attempts this is (0 .. 31)
        std::uint32_t regionId = 0;
        double regionAlignmentSeconds = 0.0;   // the region's alignment while this was heard
        std::int64_t firstPosition = 0;        // timeline position of audio[0]; then one every `decimation` samples
        int numSamples = 0;
        int decimation = 1;
        double sampleRate = 0.0;
        std::vector<float> audio;              // the decimated (anti-aliased) signal
    };

    //==============================================================================
    /** One point of the trail the view draws over the pitch lane while the transport plays: what the live tracker
        hears at a timeline position, next to the sub that plays there. */
    struct TrailPoint
    {
        double seconds = 0.0;      // timeline position
        float detectorHz = 0.0f;   // the live tracker's latest estimate (0 = unpitched), also while a curve is followed
        float subHz = 0.0f;        // the sub's oscillator
        bool learned = false;      // the sub followed a learned curve there
    };

    //==============================================================================
    class LearnAudio
    {
    public:
        static constexpr int c_maxChunkSamples = 512;   // processChunk() takes at most this many samples at once
        static constexpr int c_trailLength = 4096;      // points kept for the view (one per chunk: 20-45 s)

        explicit LearnAudio(LearnStore& store);
        ~LearnAudio();

        LearnAudio(const LearnAudio&) = delete;
        LearnAudio& operator=(const LearnAudio&) = delete;

        //==============================================================================
        // Message thread, while the audio thread is not running (prepareToPlay).

        /** Sets the host rate. A capture in progress is dropped if the rate changes (its buffer goes back to free);
            the listener's buffers are reallocated for the new rate if they exist. */
        void prepare(double sampleRate);

        //==============================================================================
        // Any thread but the audio thread.

        /** The prepared host rate, or 0 if it is not one Learn can work at. */
        double getSampleRate() const noexcept { return m_preparedRate.load(std::memory_order_acquire); }

        /** Learn has been used in this instance (ensureListening() succeeded). Until then the audio thread does
            nothing at all; the processor need not even read the play head. Any thread. */
        bool isInUse() const noexcept { return m_resources.load(std::memory_order_acquire) != nullptr; }

        /** Allocates what the listener needs (once; again by prepare() after a rate change). After this the audio
            thread listens whenever something is learned or a capture is wanted. False if not prepared. */
        bool ensureListening();

        /** Asks for a capture into `buffer` (allocated for the prepared rate, state `offered`). Returns the request's
            generation, which the buffer carries when it comes back captured. */
        std::uint32_t requestCapture(CaptureBuffer& buffer);

        /** Withdraws the capture request: a capture in progress is dropped (its buffer goes back to free). */
        void requestStop();

        /** Before `buffer` (free or retained) is freed: no request offers it any more, and the audio thread is no
            longer looking at it from an earlier offer (this waits the few instructions that can take). */
        void withdrawOffer(CaptureBuffer& buffer) noexcept;

        /** The current request: its generation, and whether it asks for a capture. */
        std::uint32_t getRequestGeneration() const noexcept;
        bool isCaptureRequested() const noexcept;

        /** The alignment hand-off, for the worker (null until ensureListening()). The pointer stays valid while the
            shared_ptr is held, also across prepare(). */
        std::shared_ptr<AlignmentRequest> getAlignmentRequest() const;

        //==============================================================================
        // Audio thread, once per block: beginBlock(), processChunk() for consecutive chunks covering the block, then
        // endBlock(). glideMs and thresholdDb are the current Glide and Threshold.

        void beginBlock(const Transport& transport, int numSamples, bool nonRealtime, float glideMs, float thresholdDb) noexcept;

        /** Listens to `mono` (samples [offset, offset + numSamples) of the block, numSamples <= c_maxChunkSamples)
            and returns the engine's guide for them, or null where every sample is tracked live. The guide's arrays
            stay valid until the next call. */
        const SubharmonicEngine::PitchGuide* processChunk(const float* mono, int offset, int numSamples) noexcept;

        /** engineGuided: the engine's last sample followed the guide (for the status). */
        void endBlock(bool engineGuided) noexcept;

        /** This block is heard on a valid timeline (Learn is in use and the host plays): the processor adds a trail
            point after each chunk only then. */
        bool isListening() const noexcept { return m_active && m_blockValid; }

        /** After the engine processed the chunk last given to processChunk(). */
        void addTrailPoint(float detectorHz, float subHz, bool learned) noexcept;

        /** Any thread but the audio thread: the trail's points, oldest first (empty until Learn is used). A point
            being written meanwhile may come out half old, half new, which is harmless for a display. */
        void copyTrail(std::vector<TrailPoint>& dest) const;

        //==============================================================================
        // Status, for any thread (relaxed atomics: each is a display figure on its own).

        enum class TimelineState : int
        {
            none = 0,   // no position at all (no play head, or it gives no time)
            stopped,    // a position, but not a playing one (or not trusted yet after a stall)
            playing
        };

        enum class CapturePhase : int
        {
            idle = 0,   // no capture under way
            waiting,    // armed, waiting for a playing timeline
            capturing   // hearing a pass
        };

        struct CaptureStatus
        {
            std::uint32_t generation = 0;           // the request the audio thread has acted on
            CapturePhase phase = CapturePhase::idle;
            bool noBuffer = false;                  // it could not take the buffer of that request
            std::uint32_t completedGeneration = 0;  // the last request it completed (handed to the worker)
            float coverage = -1.0f;                 // share of the loop heard so far, -1 while the loop is unknown (or
                                                    // too short to learn)
            float heardSeconds = 0.0f;              // the longest stretch heard so far
            float loopSeconds = 0.0f;               // the loop's length, 0 while unknown
            bool loopTooShort = false;              // the loop that plays is shorter than c_minCaptureSeconds: this
                                                    // request will never complete
        };

        CaptureStatus getCaptureStatus() const noexcept;
        TimelineState getTimelineState() const noexcept { return static_cast<TimelineState>(m_statusTimeline.load(std::memory_order_relaxed)); }
        std::optional<double> getPlayheadSeconds() const noexcept;   // the latest block's position (it may lie before
                                                                       // 0), none when the host gave none
        std::uint32_t getRegionHere() const noexcept { return m_statusRegion.load(std::memory_order_relaxed); }
        bool isFollowing() const noexcept { return m_statusFollowing.load(std::memory_order_relaxed); }
        float getLearnedConfidence() const noexcept { return m_statusConfidence.load(std::memory_order_relaxed); }

    private:
        //==============================================================================
        struct Resources
        {
            explicit Resources(double captureRate);

            std::vector<float> ring;   // the last second of the decimated signal, for alignment
            std::shared_ptr<AlignmentRequest> alignment;
            std::vector<TrailPoint> trail;
            std::atomic<int> trailWritten { 0 };   // points written so far (the newest is trailWritten - 1)
        };

        // A stretch of the block with consecutive timeline positions. A block has two when it crosses the loop end
        // and the host did not split it there.
        struct Run
        {
            int offset = 0;
            int length = 0;
            std::int64_t position = 0;   // of the run's first sample
            bool newPass = false;        // the timeline jumped (or started) here
            bool resync = false;         // it continues, but a sample or two off where expected: positions resync
            std::uint32_t pass = 0;
        };

        // A region as the audio thread uses it, at the session rate.
        struct RegionView
        {
            const Region* region = nullptr;
            std::int64_t start = 0;            // where the sub follows it: [start, end) in session samples
            std::int64_t end = 0;
            std::int64_t alignment = 0;        // its alignment, in session samples
            bool sameRate = true;              // learned at this rate: hop grids line up exactly
            double framesOrigin = 0.0;         // session position of frames[0]
            double framesPerSample = 0.0;
            double hopsOrigin = 0.0;           // session position of the start of learned hop firstHop (other rate)
            double hopsPerSample = 0.0;

            void set(const Region* r, double rate) noexcept;
            void clear() noexcept { region = nullptr; start = end = 0; }

            /** The learned hop index (from the region's first hop) that the session sample t falls in. */
            std::int64_t learnedHop(std::int64_t t, int sessionHop) const noexcept;
        };

        // A hop being measured on one grid.
        struct HopMeter
        {
            HopEnergy energy;
            std::int64_t hop = 0;         // index on its grid
            int phase = 0;                // samples into the hop
            bool whole = false;           // every sample of it was heard (it started after the run started)

            void start(std::int64_t position, std::int64_t gridOffset, int hopLength) noexcept;
        };

        struct Transition
        {
            std::int64_t position = 0;    // the first sample the new state applies to
            std::uint32_t pass = 0;
            std::uint32_t regionId = 0;
            bool valid = true;
        };

        //==============================================================================
        void resetTimeline() noexcept;
        void refreshViews() noexcept;
        void classifyBlock(const Transport& transport, int numSamples, bool nonRealtime) noexcept;
        void noteJumpBack(const Transport& transport, std::int64_t start, std::int64_t end) noexcept;
        bool hostConfirmsWrap(const Transport& transport) const noexcept;
        void noteWrap(std::int64_t start, std::int64_t end) noexcept;
        void noteSilentSkip(std::int64_t start, std::int64_t end) noexcept;
        void updateLoopEstimate(const Transport& transport, int numSamples) noexcept;
        bool loopKnown(std::int64_t& start, std::int64_t& end) const noexcept;

        void handleRequest() noexcept;
        void releaseCapture() noexcept;
        void restartCapture() noexcept;
        void commitPending() noexcept;
        void discardPending() noexcept;
        void openSegment(std::int64_t position) noexcept;
        void closeSegment() noexcept;
        void checkCaptureComplete(bool stopped) noexcept;
        void completeCapture(bool onLoop, std::int64_t loopStart, std::int64_t loopEnd, int segment) noexcept;
        double coverageOf(std::int64_t start, std::int64_t end, std::int64_t& largestGap) const noexcept;
        void publishCaptureStatus() noexcept;

        void beginRun(const Run& run) noexcept;
        void listen(const float* mono, int count, std::int64_t position) noexcept;
        void finishCaptureHop() noexcept;
        void finishVerifyHop() noexcept;
        bool pitchMoved(const Region& region) noexcept;
        bool curveAt(const RegionView& view, std::int64_t t, double& logCurve) const noexcept;
        void relocateVerifier(std::int64_t position) noexcept;
        void endVerifyPass() noexcept;
        void pushTransition(std::int64_t position, bool valid) noexcept;
        void requestAlignment() noexcept;

        bool fill(int index, int count, std::int64_t position, bool newPass, std::uint32_t pass) noexcept;
        void relocateFill(std::int64_t position) noexcept;

        /** The followed line at t (ln Hz) and its confidence; false where the curve has no pitch, and then `silent`
            (if given) says whether that is because the analysis called the audio there silent. */
        bool lineAt(const RegionView& view, std::int64_t t, float& logLine, float& confidence, bool* silent = nullptr) const noexcept;
        float targetAt(std::int64_t t, float lineNow) const noexcept;
        void findRegion(std::int64_t position, RegionView& view, std::int64_t& gapStart, std::int64_t& gapEnd) const noexcept;

        //==============================================================================
        LearnStore& m_store;

        // Prepared (message thread, audio stopped)
        std::atomic<double> m_preparedRate { 0.0 };
        double m_rate = 0.0;
        int m_hop = 480;
        int m_decimation = 4;
        std::int64_t m_warmSamples = 960;      // after a jump the filters settle this long before a hop counts
        std::int64_t m_skipQuietSamples = 4800; // silence this long before a jump forward: the host skipped silence
        PrintFilters m_filters;

        // Listener buffers: allocated once Learn is used; the audio thread reads the raw pointer.
        mutable std::mutex m_resourcesLock;    // non-audio threads only
        std::shared_ptr<Resources> m_resourcesOwner;
        std::atomic<Resources*> m_resources { nullptr };

        // Requests from the message thread: (generation << 1) | wants a capture
        std::atomic<std::uint64_t> m_request { 0 };
        std::atomic<CaptureBuffer*> m_offered { nullptr };
        std::atomic<const CaptureBuffer*> m_offeredInUse { nullptr };   // the offered buffer handleRequest() looks at

        // This block. Region pointers are only kept from one block to the next while the snapshot is the same
        // (refreshViews()).
        Resources* m_res = nullptr;
        const Snapshot* m_snapshot = nullptr;
        std::uint64_t m_snapshotRevision = 0;
        bool m_active = false;
        bool m_blockValid = false;
        bool m_follow = true;
        std::array<Run, 2> m_runs {};
        int m_numRuns = 0;
        int m_blockLength = 0;
        int m_lookAhead = 0;                   // glide time in samples
        float m_thresholdDb = -40.0f;
        std::uint32_t m_passCounter = 0;

        // Timeline across blocks
        bool m_haveLast = false;
        std::int64_t m_lastTime = 0;           // reported time of the last block
        std::int64_t m_expected = 0;           // where the last block's last run ended
        int m_advances = 0;                    // blocks in a row that continued exactly from the one before
        int m_needAdvances = 0;                // ... needed before the time is trusted again after a stall
        bool m_lastValid = false;
        bool m_lastPlaying = false;            // the host said the last block played
        bool m_lastJumped = false;             // the last block was valid and did not continue the valid one before it
        bool m_lastCommitSafe = true;          // the last block cannot have straddled a loop end nobody reported
        bool m_lastHasPpq = false;             // the last valid block's quarter-note position and tempo were reported:
        double m_lastEndPpq = 0.0;             // ... where it ended in quarter notes
        double m_lastPerSample = 0.0;          // ... and quarter notes per sample at its tempo
        int m_lastBlockSamples = 0;

        // The loop: the last wrap heard (a jump back the host's loop agrees with, or the same jump back twice), and
        // the host's loop points (this block)
        bool m_haveWrap = false;
        std::int64_t m_wrapStart = 0;
        std::int64_t m_wrapEnd = 0;
        bool m_haveJump = false;               // a jump back not yet taken for the loop: [m_jumpStart, m_jumpEnd)
        std::int64_t m_jumpStart = 0;
        std::int64_t m_jumpEnd = 0;
        bool m_hostLooping = false;
        bool m_haveEstimate = false;
        std::int64_t m_estimateStart = 0;
        std::int64_t m_estimateEnd = 0;
        bool m_splitPending = false;           // the last block was split at the host's loop end, the part after it
        std::int64_t m_splitStart = 0;         // placed at m_splitStart until this block's time says where it played
        std::int64_t m_splitEnd = 0;
        std::int64_t m_splitAfter = 0;         // that part's length

        // Listener
        int m_decPhase = 0;
        std::int64_t m_warmUntil = 0;          // hops that start before this are not compared or kept
        std::int64_t m_quietRun = 0;           // input samples in a row, up to the last one heard, below -70 dBFS
        HopMeter m_captureMeter;               // on the timeline's own hop grid
        HopMeter m_verifyMeter;                // on the grid of the region being checked (moved by its alignment)
        std::uint32_t m_listenPass = 0;
        int m_ringWrite = 0;
        int m_ringCount = 0;                   // samples of this pass in the ring
        std::int64_t m_ringNewest = 0;         // timeline position of the newest one

        // Verifier
        RegionView m_verify;
        std::uint32_t m_verifyRegionId = 0;
        std::int64_t m_verifyGapStart = 0;     // when not in a region: the stretch without one around the position
        std::int64_t m_verifyGapEnd = 0;
        bool m_verifyInRegion = false;
        MatchWindow m_window;
        int m_previousHeardLevel = -1;
        int m_previousLearnedLevel = -1;
        std::int64_t m_previousLearnedHop = -2;
        std::uint32_t m_passCompared = 0;
        std::uint32_t m_passInvalid = 0;
        std::uint32_t m_firstCompared = 0;     // of the pass's first 50 compared hops ...
        std::uint32_t m_firstDiffered = 0;     // ... how many differed
        bool m_alignWanted = false;
        std::int64_t m_stretchFirst = -1;      // the open fallback stretch, learned hop index
        std::array<float, c_pitchMaxSpan> m_pitchSpan {};   // the pitch test's samples, copied out of the ring
        int m_pitchRun = 0;                    // tested hops in a row where the pitch test found another pitch (to 2)

        static constexpr int c_maxTransitions = 16;
        std::array<Transition, c_maxTransitions> m_transitions {};
        int m_transitionRead = 0;
        int m_transitionCount = 0;

        // Alignment attempts already made: (region id, alignment) pairs that are not asked again (one the worker did not
        // get to the end of is taken back: AlignmentRequest::notMade)
        static constexpr int c_maxAttempts = 8;
        static_assert(c_maxAttempts <= 32, "AlignmentRequest::notMade has a bit per attempt");
        std::array<std::uint32_t, c_maxAttempts> m_attemptedIds {};
        std::array<double, c_maxAttempts> m_attemptedAlignments {};
        int m_attemptWrite = 0;

        // Capture (the buffer is the audio thread's while it holds it)
        CaptureBuffer* m_capture = nullptr;
        std::uint32_t m_generation = 0;        // the last request acted on
        bool m_noBuffer = false;
        CapturePhase m_phase = CapturePhase::idle;
        int m_segment = -1;                    // the open segment, or -1
        int m_sampleWrite = 0;                 // next free sample (pending ones follow the committed ones)
        int m_printWrite = 0;
        std::int64_t m_pendingEnd = 0;         // timeline position after the last sample written to the open segment
        bool m_pendingAny = false;
        bool m_bufferFull = false;
        bool m_captureWrapped = false;         // the timeline wrapped while capturing
        bool m_loopTooShort = false;           // the request's loop is too short to learn (kept until the next request)
        int m_largestBlock = 0;
        std::uint32_t m_completedGeneration = 0;

        // Guide fill
        RegionView m_fillView;
        std::int64_t m_fillGapStart = 0;
        std::int64_t m_fillGapEnd = 0;
        bool m_fillInRegion = false;
        bool m_fillValid = true;
        std::uint32_t m_fillPass = 0;
        std::uint32_t m_fillRegionId = 0;
        bool m_flagPending = false;            // a pass started: its first guided sample is marked a discontinuity ...
        std::int64_t m_flagPassStart = 0;      // ... if it is no further from the pass's first sample than alignment explains
        std::int64_t m_chunkEnd = 0;           // timeline position after the last chunk heard
        bool m_loopForTarget = false;          // a known loop the look-ahead wraps across
        std::int64_t m_targetLoopStart = 0;
        std::int64_t m_targetLoopEnd = 0;
        RegionView m_wrapView;                 // the region at that loop's start
        bool m_wrapContinues = false;          // the line continues across that loop's wrap (within a semitone)
        float m_lastConfidence = 0.0f;

        std::array<std::uint8_t, c_maxChunkSamples> m_guideState {};
        std::array<float, c_maxChunkSamples> m_guideLine {};
        std::array<float, c_maxChunkSamples> m_guideTarget {};
        SubharmonicEngine::PitchGuide m_guide;
        bool m_followedThisBlock = false;

        // Status
        static constexpr std::int64_t c_noPosition = std::numeric_limits<std::int64_t>::min();   // no host reports it
        std::atomic<int> m_statusTimeline { 0 };
        std::atomic<std::int64_t> m_statusPosition { c_noPosition };   // one value, so position and "known" never tear
        std::atomic<std::uint32_t> m_statusRegion { 0 };
        std::atomic<bool> m_statusFollowing { false };
        std::atomic<float> m_statusConfidence { 0.0f };
        std::atomic<std::uint32_t> m_statusGeneration { 0 };
        std::atomic<int> m_statusPhase { 0 };
        std::atomic<bool> m_statusNoBuffer { false };
        std::atomic<std::uint32_t> m_statusCompleted { 0 };
        std::atomic<float> m_statusCoverage { -1.0f };
        std::atomic<float> m_statusHeard { 0.0f };
        std::atomic<float> m_statusLoop { 0.0f };
        std::atomic<bool> m_statusLoopTooShort { false };
    };
} // namespace subbertone::learn

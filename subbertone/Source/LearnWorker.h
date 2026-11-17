#pragma once

#include <JuceHeader.h>

#include "LearnAudio.h"
#include "LearnStore.h"
#include "LearnTypes.h"
#include "OfflinePitchTracker.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

/**
 * LEARN's background work and its controls.
 *
 * LearnWorker is one low-priority thread shared by every Subbertone instance in the process (held through a
 * juce::SharedResourcePointer). Analyses and alignments run on it one at a time, in the order they were posted, so
 * instances never compete with each other, or with the host's audio, for cores. About every 25 ms it also asks each
 * instance whether there is work: the audio thread cannot post a job itself (that would allocate), so it leaves a
 * finished capture or an alignment request where the worker looks.
 *
 * Learner is one instance's side of it, owned by the processor: the commands the editor gives (learn, stop, forget,
 * pick a line, change gap filling, analyse again), the capture buffers, the jobs, and the status the editor shows.
 * Its methods may be called from any thread but the audio thread.
 */
namespace subbertone::learn
{
    /** Progress and cancellation of one job. */
    struct JobState
    {
        std::atomic<bool> cancel { false };
        std::atomic<float> progress { 0.0f };
        std::atomic<bool> finished { false };
    };

    //==============================================================================
    class LearnWorker final : private juce::Thread
    {
    public:
        LearnWorker();
        ~LearnWorker() override;

        class Client
        {
        public:
            virtual ~Client() = default;

            /** On the worker thread, about every 25 ms: look for work and post it. */
            virtual void pollFromWorker() = 0;
        };

        void addClient(Client& client);

        /** Drops the client's queued jobs, tells a running one to cancel, and waits until the worker is running
            nothing of this client's (job or poll). Jobs the client posts meanwhile are refused. */
        void removeClient(Client& client);

        /** Runs `job` on the worker thread once the jobs posted before it are done. A job cancelled meanwhile still
            runs (it sees its flag and only hands back what it holds); only removeClient() drops jobs. A job for a
            client that is not registered (any more) is refused: its state reads cancelled and finished at once. */
        void post(Client& owner, std::shared_ptr<JobState> state, std::function<void(JobState&)> job);

    private:
        struct Job
        {
            Client* owner = nullptr;
            std::shared_ptr<JobState> state;
            std::function<void(JobState&)> run;
        };

        void run() override;
        void pollClients();
        bool runOneJob();

        std::mutex m_lock;
        std::condition_variable m_idle;
        std::vector<Client*> m_clients;
        std::deque<Job> m_queue;
        Client* m_busyWith = nullptr;           // whose poll or job is running now
        std::shared_ptr<JobState> m_running;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LearnWorker)
    };

    //==============================================================================
    /** What the LEARN button and the view show (a copy, made on request). */
    struct Status
    {
        enum class Activity
        {
            idle,
            armed,       // waiting for the loop to play
            capturing,   // hearing it
            analysing    // working out the pitch
        };

        enum class Outcome
        {
            none,        // nothing finished since LEARN was last clicked
            learned,
            uncertain,   // learned, but worth a look in the view
            failed
        };

        enum class Failure
        {
            none,
            nothingPitched,  // nothing in the capture had a pitch
            cancelled,
            outOfMemory,
            notKept,         // everything learned was cleared or replaced meanwhile
            loopTooShort     // the loop is shorter than c_minCaptureSeconds, so it can never be learned
        };

        Activity activity = Activity::idle;
        float progress = -1.0f;          // capturing: share of the loop heard (-1 until its length is known);
                                         // analysing: 0..1
        float heardSeconds = 0.0f;       // capturing: the longest stretch heard so far
        float loopSeconds = 0.0f;        // capturing: the loop's length, 0 while unknown
        Method method = Method::Auto;    // what the current or last capture asks for
        Outcome outcome = Outcome::none;
        Failure failure = Failure::none;

        LearnAudio::TimelineState timeline = LearnAudio::TimelineState::none;
        bool playheadKnown = false;      // the host gave the playhead's position ...
        double playheadSeconds = 0.0;    // ... this one (a host may report times before 0, as in a count-in)

        int numRegions = 0;
        double learnedSeconds = 0.0;
        bool follow = true;              // USE LEARNED
        bool canUndo = false;            // UNDO brings back what was just forgotten
        bool canResume = false;          // what the last stop interrupted can be taken up again

        std::uint32_t regionHere = 0;    // the region at the playhead (0 = none)
        bool followingHere = false;      // the sub follows a learned curve right now
        float confidence = 0.0f;         // the learned curve's confidence where it was followed last

        // About regionHere, or the newest region when the playhead is in none
        std::uint32_t region = 0;
        Method regionMethod = Method::Tonal;
        float regionLineRatio = 1.0f;
        bool regionUncertain = false;
        double regionSeconds = 0.0;
        double regionFallbackSeconds = 0.0;   // how long fell back to live tracking in its latest pass (or the pass
                                              // before, if more did there)
        bool partlyInvalid = false;           // at least 200 ms of it did
    };

    //==============================================================================
    class Learner final : private LearnWorker::Client
    {
    public:
        Learner(LearnStore& store, LearnAudio& audio);
        ~Learner() override;

        Learner(const Learner&) = delete;
        Learner& operator=(const Learner&) = delete;

        /** After LearnAudio::prepare() (prepareToPlay): a capture wanted at the old rate is asked for again. */
        void prepare();

        /** After LearnStore::takeState() replaced what is learned: captures and jobs in flight are dropped. */
        void stateLoaded();

        //==============================================================================
        // Commands

        /** Arms LEARN: the next complete pass of the loop is captured and analysed with `method`; what is learned
            replaces what was learned where they overlap. While a capture is already under way only its method
            changes. False if no capture buffer could be had. */
        bool startLearning(Method method = Method::Auto);

        /** Stops a capture or an analysis under way. resumeLearning() takes it up again (an interrupted capture
            starts again; an interrupted analysis runs again on the same audio). */
        void stopLearning();
        bool resumeLearning();

        /** Forgets everything learned (UNDO for c_undoSeconds), or one region. Captures and jobs stop too. */
        bool forgetAll();
        bool forgetRegion(std::uint32_t id);
        bool canUndoForget() const;
        bool undoForget();

        /** USE LEARNED. */
        void setFollow(bool follow);
        bool getFollow() const;

        /** Follows another line of a region: a candidate's ratio (1 = the analysis' own choice). */
        bool setLine(std::uint32_t id, float ratio);

        /** Bridges unpitched gaps of up to `seconds` (0 .. c_maxGapFillSeconds), from the stored frames. id 0: every
            region. */
        bool setGapFill(std::uint32_t id, float seconds);

        /** Analyses a region again with another method. Works from the audio of the newest capture, if the region
            came from it; otherwise LEARN is armed with that method and false is returned (play the loop again). */
        bool relearn(std::uint32_t id, Method method);

        //==============================================================================
        Status getStatus() const;
        std::shared_ptr<const Snapshot> getSnapshot() const { return m_store.getSnapshot(); }

    private:
        struct ArmRecord
        {
            std::uint32_t generation = 0;
            Method method = Method::Auto;
            std::uint64_t epoch = 0;
            bool valid = false;
        };

        enum class Interrupted
        {
            nothing,
            capture,
            analysis
        };

        void pollFromWorker() override;

        bool armLocked(Method method);
        CaptureBuffer* takeBufferLocked(double rate);
        ArmRecord* findRecordLocked(std::uint32_t generation);
        const ArmRecord* findRecordLocked(std::uint32_t generation) const;
        void postAnalysisLocked(CaptureBuffer& buffer, const ArmRecord& record, float gapFillSeconds);
        void analyse(CaptureBuffer& buffer, ArmRecord record, float gapFillSeconds, JobState& job);
        void align(const std::shared_ptr<AlignmentRequest>& request, JobState& job);
        void stopLocked(bool keepForResume);

        LearnStore& m_store;
        LearnAudio& m_audio;
        juce::SharedResourcePointer<LearnWorker> m_worker;

        mutable std::mutex m_lock;
        std::array<std::unique_ptr<CaptureBuffer>, 2> m_buffers;
        std::array<ArmRecord, 4> m_records {};
        std::size_t m_recordWrite = 0;
        bool m_armWhenPrepared = false;            // asked for before there was a usable sample rate
        Method m_armMethod = Method::Auto;

        std::shared_ptr<JobState> m_analysisJob;   // queued or running
        CaptureBuffer* m_analysisBuffer = nullptr;
        Method m_analysisMethod = Method::Auto;
        std::shared_ptr<JobState> m_alignmentJob;

        Status::Outcome m_outcome = Status::Outcome::none;
        Status::Failure m_failure = Status::Failure::none;

        Interrupted m_interrupted = Interrupted::nothing;
        Method m_interruptedMethod = Method::Auto;
        CaptureBuffer* m_interruptedBuffer = nullptr;
        ArmRecord m_interruptedRecord;
        float m_interruptedGapFill = static_cast<float>(c_defaultGapFillSeconds);
        bool m_resumeWhenReleased = false;         // resumed while the stopped analysis was still letting go

        CaptureBuffer* m_retainedBuffer = nullptr; // the audio the newest region was analysed from ...
        std::uint32_t m_retainedRegion = 0;        // ... and that region's id

        double m_lastGarbageMs = 0.0;
    };

    //==============================================================================
    // The steps of an analysis, exposed for tests.

    /** A capture, laid out for the analysis: the decimated audio of whole hops on the capture's timeline, gaps
        filled with silence, and a fingerprint per hop (only those heard whole are flagged complete). */
    struct AssembledCapture
    {
        std::vector<float> audio;          // at captureRate: audio[i] is at timeline position firstPosition + i * decimation
        std::int64_t firstPosition = 0;
        double captureRate = 0.0;
        std::int64_t firstHop = 0;
        std::vector<Fingerprint> prints;
    };

    bool assembleCapture(const CaptureBuffer& capture, AssembledCapture& out);

    /** Frames for a region from the analysis' curve: ln Hz, flags, confidence; each note pitched from its first
        audible frame. */
    std::vector<Frame> framesFromCurve(std::vector<offline::Frame> curve);

    /** Gap filling again from a region's stored frames (bridged frames cleared and bridged again). */
    void refillGaps(Region& region, float gapFillSeconds);

    /** The region a capture's analysis gives (not yet published: LearnStore::add() checks and numbers it). False if
        the analysis found nothing with a pitch. */
    bool regionFromAnalysis(const CaptureBuffer& capture, const AssembledCapture& assembled,
                            const offline::Analysis& analysis, Method requested, float gapFillSeconds, Region& region);

    /** Searches +-50 ms around a region's alignment for the offset at which `audio` (decimated, as the listener
        keeps it) matches its fingerprints. True, with the alignment in seconds, if one matches at least 90 % of
        the hops compared. */
    bool findAlignment(const Region& region, const std::vector<float>& audio, std::int64_t firstPosition, int decimation,
                       double sampleRate, const std::atomic<bool>* cancel, double& alignmentSeconds, float& matchShare);
} // namespace subbertone::learn

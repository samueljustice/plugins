#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "FlattenerTypes.h"
#include "SourceAdvisor.h"

namespace pf
{
    /**
     * ANALYSE: learn a sound once, then flatten it with what was learned.
     *
     *  1. Armed: waits for the next sound (input above the Gate).
     *  2. Recording: keeps the sound, from a quarter second before it starts until 1.5 s of quiet, a second press, or
     *     60 s. What is kept is the plugin's own analysis signal (mono, ~24 kHz), plus a fingerprint of it every
     *     frame: the broadband level and the level in 12 half-octave bands.
     *  3. Analysing, on a worker thread: the Source Type advisor picks the Source Type that flattens the take best (a
     *     run-up that spans more than an octave on a harmonic-sum type becomes Engine Run-up), and the hindsight
     *     tracker works out the take's pitch line as a whole. From the line come the suggested range, Max Shift and
     *     Shift Below, which the processor applies once (popSuggestion).
     *  4. Ready: on every later pass the incoming fingerprint is matched against the take's. Once it lines up (about a
     *     second into the sound, well within the 2.6 s lookahead) the learned line guides the tracker, frame by frame,
     *     as the hindsight guide does. If the audio stops matching (another sound, an edit) it lets go again, and the
     *     live tracker carries on alone.
     *
     * Matching uses the shape of the fingerprint (each band relative to the window's own mean), so a pass at another
     * gain still matches. Frames are PitchTracker frames: frame k is centred on analysis sample k * hop.
     *
     * Threading: arm/finish/forget/popSuggestion/state on the message thread; reset/push/service/lookup on the audio
     * thread (no locks, no allocation); analysis and matching on the worker (or in service() when rendering offline).
     */
    class LearnedLine : public TrackGuide
    {
    public:
        enum class State : int { Empty, Armed, Recording, Analysing, Ready, Failed };

        struct Suggestion
        {
            bool valid = false;
            int sourceType = -1;           // a SourceType
            float minHz = 0.0f, maxHz = 0.0f;
            float maxShiftSemitones = 0.0f;
            float shiftBelowHz = 0.0f;     // 0 = Off
            float lineLowHz = 0.0f, lineHighHz = 0.0f;   // 10th / 90th percentile of the learned line
            bool lineTrusted = false;      // off the edges of the search range and steady: it sets range and guides
        };

        static constexpr double maxTakeSeconds = 60.0;
        static constexpr double preRollSeconds = 0.25;
        static constexpr double endQuietSeconds = 1.5;
        static constexpr double minTakeSeconds = 0.5;
        static constexpr int numBands = 13;   // broadband + 12 half-octave bands, 100 Hz - 8 kHz

        LearnedLine();
        ~LearnedLine() override;

        /** Allocates and starts the worker. A learned take made at another analysis rate is forgotten. Message thread. */
        void prepare (double analysisRate, int hop);
        void release();

        // Message thread -----------------------------------------------------------
        /** Listen for the next sound and learn it. current/currentSource are what the advisor starts from. */
        void arm (const ProcessingSettings& current, int currentSource);
        /** While recording: stop here and analyse. While armed: give up. */
        void finish();
        void forget();
        State getState() const { return (State) state.load (std::memory_order_acquire); }
        float getProgress() const { return progress.load (std::memory_order_relaxed); }
        bool isLocked() const { return lockedOffset.load (std::memory_order_acquire) != notLocked; }
        /** For tests: plugin frame - take frame while matched, and the stream frame the take started at when recorded. */
        int64_t getLockedOffset() const { return lockedOffset.load (std::memory_order_acquire); }
        int64_t getTakeStartFrame() const { return takeStartFrame; }
        double getTakeSeconds() const;
        /** The settings worked out from the latest analysis, once. */
        bool popSuggestion (Suggestion& s);
        /** Message thread, regularly: ends a take whose audio has stopped arriving (the host stopped playing). */
        void messageTick();
        /** For tests and offline use: runs a pending analysis now, on the calling thread. */
        void analyseNow();

        /** The learned take (line and fingerprint) as text for the plugin state; empty when nothing is learned. */
        juce::String saveState() const;
        void loadState (const juce::String& text);

        // Audio thread ------------------------------------------------------------
        /** The stream restarts at analysis sample 0 (FlattenerCore::reset). Keeps what was learned. */
        void reset();
        void setNonRealtime (bool nonRealtime) { offline.store (nonRealtime, std::memory_order_relaxed); }
        void setGateDb (float db) { gateDb.store (db, std::memory_order_relaxed); }
        void push (float analysisSample);
        /** Offline: matches in step. Call after pushing samples. */
        void service();
        /** True, with the learned pitch in cents, when frame f is matched to the take and pitched there. */
        bool lookup (int64_t frame, float& cents) const override;
        /** The plugin has used frame f (kept for symmetry with the hindsight guide). */
        void markUsed (int64_t) override {}

    private:
        class Worker;
        using Print = std::array<float, numBands>;
        static constexpr int64_t notLocked = INT64_MIN;
        static constexpr int matchFrames = 200;          // one second of frames is compared
        static constexpr int printRingFrames = 1 << 13;  // incoming prints kept (~41 s)

        void finishFrame();
        void runAnalysis();
        void runMatch();
        float matchError (const Print* incoming, int64_t takeStart) const;

        double rate = 24000.0;
        int hop = 120;

        // Band filters for the fingerprint (audio thread)
        struct Biquad
        {
            double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
            float process (float x)
            {
                const double y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return (float) y;
            }
        };
        std::array<Biquad, numBands - 1> bands;
        std::array<double, numBands> frameEnergy {};
        int frameFill = 0;
        int64_t frameIndex = 0;      // frames completed since reset()

        // Incoming prints (audio thread writes, worker reads)
        std::vector<Print> printRing;
        std::atomic<int64_t> printsWritten { 0 };
        std::atomic<uint32_t> streamGeneration { 1 };

        // Pre-roll and the take (audio thread while recording; worker while analysing)
        std::vector<float> preRoll;   // last preRollSeconds of samples
        int64_t samplesSinceReset = 0;
        std::vector<float> takeAudio;
        std::vector<Print> takePrints;
        std::atomic<int> takeSamples { 0 };
        std::atomic<int> takeFrames { 0 };
        int quietFrames = 0;
        int64_t takeStartFrame = 0;
        int64_t tickSeenPrints = -1;    // message thread
        int idleTicks = 0;
        std::atomic<bool> finishRequested { false };

        // What was learned (written only while state is not Ready)
        std::vector<float> lineHz;    // per take frame, 0 = unpitched
        std::atomic<int> lineFrames { 0 };
        double learnedRate = 0.0;     // the analysis rate and hop the take was learned at
        int learnedHop = 0;

        // Matching. A lock holds from lockFrom on; the lock it replaced still answers for frames before prevUntil, which
        // the tracker may not have reached yet (it runs 2.6 s behind the input).
        std::atomic<int64_t> lockedOffset { notLocked };   // plugin frame - take frame
        std::atomic<int64_t> lockFrom { 0 };
        std::atomic<int64_t> prevOffset { notLocked };
        std::atomic<int64_t> prevUntil { 0 };
        std::atomic<uint32_t> matchGeneration { 0 };
        std::atomic<int64_t> lastMatchAt { 0 };   // prints seen by the last match
        int64_t lastSearchAt = -1000000;          // worker / service: when the last full search ran
        int64_t mismatchFrom = 0;
        int missedChecks = 0;
        void dropLock (int64_t until);

        std::atomic<int> state { (int) State::Empty };
        std::atomic<float> progress { 0.0f };
        std::atomic<float> gateDb { -60.0f };
        std::atomic<bool> offline { false };
        std::atomic<bool> cancel { false };       // stops an analysis: release, forget, loadState
        std::atomic<uint32_t> takeGeneration { 1 };   // bumped by arm, forget and loadState: older analyses publish nothing
        double takeRate = 0.0;                    // the analysis rate and hop the take being recorded or analysed is at
        int takeHop = 0;

        ProcessingSettings armedSettings;   // message thread writes before Armed; worker reads after Analysing
        int armedSource = -1;
        mutable std::mutex suggestionLock;
        Suggestion suggestion;
        bool suggestionPending = false;
        std::mutex workLock;                // analysis / matching run one at a time

        std::unique_ptr<Worker> worker;
    };
}

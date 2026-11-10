#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include "FlattenerTypes.h"

namespace pf
{

    /**
     * Turns frame-wise candidates into a correction curve, using lookahead.
     *
     * Stages, each a fixed number of frames behind the previous one:
     *   1. Fixed-lag Viterbi over the candidates (+ an unvoiced state).
     *   2. Octave repair against the local median, then a median-5.
     *   3. Gap fill: short unvoiced gaps are bridged, long ones held then released.
     *   4. Centred Gaussian smoothing in cents (sigma = smoothing time).
     *   5. Target (latch / fixed / auto) and correction ratio in cents. Where the hindsight guide
     *      (HindsightGuide.h) has a line for a frame, that line replaces the track from here on.
     *
     * The total lag is constant, so the plugin's latency never changes with
     * the settings. Frame f's final ratio is available once frame
     * f + totalLagFrames() has been pushed.
     */
    class PitchTracker
    {
    public:
        static constexpr int viterbiLag = 120;   // ~0.6 s at a 5 ms hop
        static constexpr int repairLag  = 20;
        static constexpr int gapLag     = 80;    // max gap fill / pre-roll
        static constexpr int smoothLag  = 30;    // 3 sigma of the max smoothing
        static constexpr int targetLag  = 200;   // half the max auto window
        static constexpr int totalLagFrames() { return viterbiLag + repairLag + gapLag + smoothLag + targetLag; }

        void prepare (double hopSeconds);
        void reset();

        /** Per-hop tuning. detector selects the internal costs. */
        void setSettings (const LiveSettings& s, DetectorType detector);
        void requestRelatch() { relatchPending = true; }
        /** The hindsight guide consulted in stage 5 (may be null). Frames are numbered as pushed here. */
        void setGuide (TrackGuide* g) { guide = g; }

        /** Pushes frame number (numPushed) and advances every stage by one frame. */
        void push (const FrameObservation& obs);

        /** Final correction for frame f, in cents. f must be <= lastFinalFrame(). */
        float ratioCents (int64_t f) const;
        int64_t lastFinalFrame() const { return numPushed - 1 - totalLagFrames(); }

        /** Telemetry of the most recently finalised frame. */
        const TelemetryFrame& lastTelemetry() const { return telemetry; }
        float getLatchedHz() const { return latched ? centsToHz (latchedCents) : 0.0f; }

    private:
        enum Flag : uint8_t { None = 0, Voiced = 1, Interp = 2, Held = 3 };

        static constexpr int ringSize = 1024;   // > totalLagFrames()
        static constexpr int ringMask = ringSize - 1;
        static constexpr int numStates = maxCandidates + 1;   // last = unvoiced
        static int idx (int64_t f) { return (int) (f & ringMask); }

        void stepViterbi (const FrameObservation& obs);
        void stageRepair (int64_t f);
        void stageGap (int64_t f);
        void stageSmooth (int64_t f);
        void stageTarget (int64_t f);
        /** Pitch of smoothed frame f for the target stage: the guide's where it has one. */
        bool targetTrack (int64_t f, float& cents, float& weight) const;
        struct Filled;
        static bool guideOverrides (const Filled& own, float guideCents);
        static bool registerOffset (float ownMinusGuideCents, float& offset);

        // Tuning
        double hopSec = 0.005;
        LiveSettings settings;
        float sigmaCents = 60.0f;      // allowed movement per hop before cost saturates
        float octaveCost = 1.0f;
        float voicedOn = 0.5f;
        int gapFrames = 30;
        float smoothSigmaFrames = 2.0f;
        int autoHalfFrames = 100;
        bool harmonicDetector = false;

        int64_t numPushed = 0;
        TrackGuide* guide = nullptr;
        bool guideOffsetSet = false;   // a guided stretch is under way, with its register fixed
        bool guideUsable = false;      // the guide is a simple ratio (octave, fifth) from the own track
        float guideOffset = 0.0f;      // cents added to the guide to put it in the own track's register
        int64_t lastGuideFrame = -1000000;
        // Who jumped: when the guide and the own track part, the guide takes over only if the own track hopped away
        // and the guide stayed on its line, until the two agree again.
        bool ownJumped = false, guideJumped = false;
        float lastOwnVoiced = 0.0f;     // the own track's latest voiced value (kept across its gaps)
        float lastGuideCents = 0.0f;    // the guide's value on the previous frame (0 = none)
        std::vector<float> ownHistory = std::vector<float> (ringSize, 0.0f);     // 0 = none
        bool guideMayLead (const Filled& own, float guideCents) const;

        // Raw observations (for telemetry) and Viterbi lattice
        struct Lattice
        {
            std::array<float, numStates> cents {};
            std::array<float, numStates> emission {};
            std::array<uint8_t, numStates> back {};
            int numVoiced = 0;
            float voicedProb = 0.0f;
            float rawHz = 0.0f;
            float rmsDb = -120.0f;
        };
        std::vector<Lattice> lattice = std::vector<Lattice> (ringSize);
        std::array<float, numStates> cost {};
        std::array<float, numStates> stateCents {};   // cents carried by each state (unvoiced carries last voiced)

        // Per-stage outputs
        struct Decided { float cents = 0.0f; float prob = 0.0f; bool voiced = false; };
        std::vector<Decided> decided   = std::vector<Decided> (ringSize);  // after Viterbi
        std::vector<Decided> repaired  = std::vector<Decided> (ringSize);  // after octave repair + median
        struct Filled { float cents = 0.0f; float weight = 0.0f; uint8_t flag = None; int heldFor = 0; };
        std::vector<Filled> filled     = std::vector<Filled> (ringSize);
        std::vector<Filled> smoothed   = std::vector<Filled> (ringSize);
        std::vector<float> ratio       = std::vector<float> (ringSize, 0.0f);

        int64_t lastVoicedRepaired = -1000000;
        bool latched = false;
        float latchedCents = 0.0f;
        int64_t latchUnvoicedRun = 0;
        bool relatchPending = false;
        float lastRatio = 0.0f;

        std::vector<float> scratch = std::vector<float> (64);
        TelemetryFrame telemetry;
    };
}

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "FlattenerTypes.h"
#include "Detectors.h"
#include "PitchTracker.h"
#include "RubberBandDriver.h"

namespace pf
{
    /**
     * The whole flattener: analysis front end, detector, lookahead tracker and
     * RubberBand driver, with a fixed, reported latency.
     *
     * Signal flow (sample counts are input-rate unless noted):
     *
     *   input ──┬──────────────────────────────── delay Ltot ── dry ──┐
     *           │                                                     mix ── out
     *           ├─ mono ─ decimate to ~24 kHz ─ filters ─ frames ─ detector ─ tracker ─ ratio track
     *           │                                                         │
     *           └─ delay D_a ─────────────── RubberBand R3 (pitch = ratio track) ── wet ┘
     *
     * The shifter is fed D_a samples behind the input, so every ratio it uses
     * has been decided with the full tracker lookahead. Its output is written
     * to a ring aligned with the input, and read out Ltot samples late.
     *
     * Threading:
     *  - prepare/reset/setDetectorConfig/setQuality/housekeeping: message thread.
     *  - process/setLiveSettings: audio thread.
     *  - setDetectorConfig/setQuality build new objects off the audio thread and
     *    hand them over through lock-free slots; retired objects are deleted by
     *    housekeeping() on the message thread.
     */
    class FlattenerCore
    {
    public:
        FlattenerCore();
        ~FlattenerCore();

        /** Allocates everything. numChannels is 1 or 2. */
        void prepare (double sampleRate, int maxBlockSize, int numChannels,
                      const DetectorConfig& detector, int quality);
        void releaseResources();
        /** Clears all audio/analysis state (keeps allocations). Call from prepare or the audio thread. */
        void reset();
        /** Asks the audio thread to reset() at the start of the next process(). Any thread. */
        void requestReset() { resetRequested.store (true); }
        bool isPrepared() const { return prepared.load(); }

        /** Requests a detector rebuild if cfg differs from the current one. Message thread. */
        void setDetectorConfig (DetectorType type, float minHz, float maxHz);
        /** 0 = Smooth (standard R3 window), 1 = Tight (short R3 window). Message thread. */
        void setQuality (int quality);
        /** Deletes retired objects. Call regularly on the message thread. */
        void housekeeping();

        /**
         * Applies structural settings synchronously on the calling (audio) thread.
         * Only for offline / non-realtime rendering, where allocation is acceptable
         * and the message thread may not run between blocks.
         */
        void applyStructuralNow (DetectorType type, float minHz, float maxHz, int quality);

        /** Audio thread, once per block before process(). */
        void setLiveSettings (const LiveSettings& s) { live = s; formantMirror.store (s.formantPreserve); }
        void setFilters (float highpassHz, float lowpassHz);

        /** Processes in place. numChannels must match prepare(). Audio thread. */
        void process (float* const* channels, int numChannels, int numSamples);

        /** Total latency of the plugin, in input samples. Constant for a given prepare(). */
        int getLatencySamples() const { return totalLatency; }

        void requestRelatch() { relatchRequested.store (true); }

        // Telemetry for the UI -------------------------------------------------
        bool popTelemetry (TelemetryFrame& f);
        int readAnalysisAudio (float* dest, int maxSamples);
        double getAnalysisRate() const { return analysisRate; }
        float getLatchedHz() const { return latchedHz.load(); }
        float getInputLevelDb() const { return inputLevelDb.load(); }
        int getUnderrunCount() const { return underruns.load(); }
        DetectorType getActiveDetector() const { return (DetectorType) activeDetectorType.load(); }

        /** For tests: the shifter's own alignment offset (input samples). */
        void setAlignmentOffset (int samples) { alignOffset = samples; }
        int getAlignmentOffset() const { return alignOffset; }

        static constexpr int numFixedTrackerFrames() { return PitchTracker::totalLagFrames(); }

    private:
        void processChunk (float* const* channels, int numChannels, int numSamples);
        void runAnalysis();
        void feedShifter();
        float ratioCentsAtInputSample (int64_t m) const;
        void installPending();
        void installDetector (IPitchDetector* d);
        void installShifter (RubberBandDriver* s);
        void housekeepingLocked();
        DetectorConfig makeConfig (DetectorType type, float minHz, float maxHz) const;

        std::atomic<bool> prepared { false };
        std::mutex structMutex;           // prepare/release vs. rebuilds (never taken by process())
        std::atomic<bool> resetRequested { false };
        std::atomic<bool> formantMirror { false };
        int shifterChunk = 128;
        int xfadeStart = 0;               // crossfade from dry into a newly installed shifter
        int64_t xfadeFrom = 0;
        static constexpr int xfadeLength = 512;
        double sampleRate = 48000.0;
        int maxBlock = 512;
        int numCh = 2;

        // Analysis front end ---------------------------------------------------
        int decimation = 2;               // input samples per analysis sample
        double analysisRate = 24000.0;
        int hop = 120;                    // analysis samples per frame
        int frameWindow = 8192;           // analysis samples spanned by every frame (centred)
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
            void clear() { z1 = z2 = 0; }
        };
        static void designRbj (Biquad& f, bool highpass, double hz, double fs, double q);

        std::array<Biquad, 4> aaFilters;  // anti-alias before decimation
        int aaStages = 0;
        Biquad hpFilter, lpFilter;
        float currentHp = -1.0f, currentLp = -1.0f;
        int decimPhase = 0;
        float decimAccum = 0.0f;          // level accumulator (sum of squares) per analysis sample

        std::vector<float> anaRing;       // mono filtered analysis signal
        std::vector<float> anaLevelRing;  // unfiltered mono, for frame level
        int anaMask = 0;
        int64_t anaWritten = 0;           // analysis samples written
        int64_t framesDone = 0;           // frames analysed
        std::vector<float> frameBuf, frameLevelBuf;

        std::unique_ptr<IPitchDetector> detector;
        std::atomic<IPitchDetector*> pendingDetector { nullptr };
        std::atomic<IPitchDetector*> retiredDetector { nullptr };
        std::atomic<int> activeDetectorType { (int) DetectorType::SpectralPeak };
        DetectorConfig currentConfig;     // message thread: last config posted
        DetectorConfig wantedConfig;      // message thread: config asked for

        PitchTracker tracker;

        // Shifter --------------------------------------------------------------
        std::unique_ptr<RubberBandDriver> shifter;
        std::atomic<RubberBandDriver*> pendingShifter { nullptr };
        std::atomic<RubberBandDriver*> retiredShifter { nullptr };
        int currentQuality = 0;           // message thread
        int wantedQuality = 0;            // message thread
        static constexpr int maxChunk = 1024;  // host blocks are processed in pieces of at most this

        int64_t inWritten = 0;            // input samples received
        std::vector<std::vector<float>> inRing;  // per channel, input-aligned
        std::vector<std::vector<float>> wetRing; // per channel, input-aligned shifter output
        int ringMask = 0;
        int64_t fed = 0;                  // input samples fed to the shifter (aligned index)
        int64_t wetWritten = 0;           // aligned wet samples written
        int analysisDelay = 0;            // D_a
        int shifterHeadroom = 0;          // B
        int totalLatency = 0;             // Ltot
        int alignOffset = 0;
        std::vector<std::vector<float>> feedScratch;
        std::vector<float*> feedPtrs;

        LiveSettings live;

        // Telemetry --------------------------------------------------------------
        juce::AbstractFifo telemetryFifo { 1024 };
        std::vector<TelemetryFrame> telemetryBuf;
        juce::AbstractFifo uiAudioFifo { 1 << 15 };
        std::vector<float> uiAudioBuf;

        std::atomic<bool> relatchRequested { false };
        std::atomic<float> latchedHz { 0.0f };
        std::atomic<float> inputLevelDb { -120.0f };
        std::atomic<int> underruns { 0 };

        JUCE_DECLARE_NON_COPYABLE (FlattenerCore)
    };
}

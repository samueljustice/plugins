#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "FlattenerTypes.h"

namespace pf
{
    /**
     * A second opinion on the pitch track, worked out in hindsight.
     *
     * The frame detectors judge every frame on its own, so through an engine run-up they can hop from one
     * order to another (the firing rate, a half order, a cylinder harmonic) and the correction then jumps by
     * the ratio between them. Here the last few seconds of the analysis signal are handed to the hindsight
     * tracker (HindsightTracker.h), which sees the whole stretch at once, follows the set of partials as they
     * move together and so keeps to one line through the run-up.
     *
     * Every stepSeconds a window of the latest historySeconds + aheadSeconds is analysed (on a worker thread,
     * or on the audio thread when rendering offline). The tracker's Auto method decides whether the window is
     * an engine-like sound at all: only when it chose its Engine method, and that stayed on its grid, does the
     * window guide the plugin. Otherwise the frames are left to the plugin's own tracker. Of the lines the
     * window offers (the chosen one and the parallel ones) the guide takes the one that best continues what it
     * has already published, so successive windows stay on the same partial.
     *
     * Frames are those of PitchTracker: frame k is centred on analysis sample k * hop of the input-aligned
     * stream pushed here. A frame keeps being refined by newer windows until the plugin uses it (markUsed).
     *
     * Threading: prepare/release on the message thread; everything else on the audio thread except the
     * worker, which only reads the audio ring and writes the guide slots.
     */
    class HindsightGuide : public TrackGuide
    {
    public:
        static constexpr double historySeconds = 6.0;
        static constexpr double aheadSeconds = 2.25;
        static constexpr double stepSeconds = 1.0;

        HindsightGuide();
        ~HindsightGuide() override;

        /** Allocates and starts the worker. hop = analysis samples per frame. Message thread. */
        void prepare (double analysisRate, int hop);
        /** Stops the worker (cancelling any analysis in progress). Message thread. */
        void release();

        // Audio thread ------------------------------------------------------------
        /** Forgets everything: the next pushed sample is analysis sample 0 again. */
        void reset();
        /** Turns the guide on or off and sets the search range of the followed line. */
        void setActive (bool on, float lowestHz, float highestHz);
        /** Non-realtime rendering: windows are analysed synchronously in service() instead of on the worker. */
        void setNonRealtime (bool nonRealtime) { offline.store (nonRealtime, std::memory_order_relaxed); }
        void push (float analysisSample);
        /** Offline only: analyses the window that is due, if any. Call after pushing samples. */
        void service();

        /** True, with the guide's pitch in cents, when frame f is guided. False: use the plugin's own track. */
        bool lookup (int64_t frame, float& cents) const override;
        /** The plugin has used frame f: later windows leave it (and earlier frames) as they are. */
        void markUsed (int64_t frame) override { used.store (frame, std::memory_order_release); }

        /** For tests and telemetry: windows analysed, and how many of them guided. */
        int getWindowsAnalysed() const { return windowsAnalysed.load(); }
        int getWindowsGuided() const { return windowsGuided.load(); }

    private:
        class Worker;
        void runDueWindow();

        struct Slot
        {
            std::atomic<uint64_t> tag { 0 };   // generation << 40 | (frame + 1); 0 = empty
            std::atomic<float> hz { 0.0f };    // 0 = not guided
        };
        static uint64_t makeTag (uint32_t gen, int64_t frame) { return ((uint64_t) gen << 40) | (uint64_t) (frame + 1); }

        double rate = 24000.0;
        int hop = 120;
        int windowSamples = 0, stepSamples = 0;

        std::vector<float> ring;            // input-aligned analysis signal
        int64_t ringMask = 0;
        std::atomic<int64_t> written { 0 };
        std::vector<Slot> slots;
        int64_t slotMask = 0;

        std::atomic<uint32_t> generation { 1 };
        std::atomic<int64_t> used { -1 };
        std::atomic<bool> active { false };
        std::atomic<bool> offline { false };
        std::atomic<float> loHz { 25.0f }, hiHz { 1000.0f };
        std::atomic<bool> cancel { false };

        // owned by whoever holds windowLock (the worker, or the audio thread offline)
        std::mutex windowLock;
        std::vector<float> window;
        uint32_t windowGen = 0;
        int64_t lastWindowEnd = 0;

        std::atomic<int> windowsAnalysed { 0 }, windowsGuided { 0 };
        std::unique_ptr<Worker> worker;
    };
}

#pragma once

#include <array>
#include <atomic>
#include <vector>
#include "FlattenerTypes.h"

namespace pf
{
    /** Everything that decides how a sound is flattened, as the processor's parameters set it. */
    struct ProcessingSettings
    {
        LiveSettings live;
        DetectorType detector = DetectorType::SpectralMpm;
        float minHz = 300.0f, maxHz = 3000.0f;
        int quality = 1;
        float highpassHz = 200.0f, lowpassHz = 10000.0f;
    };

    /** The same settings with a Source Type's own choices (detector, range, tracking, filters, shifter, Max Shift)
        swapped in, as picking that Source Type would set them. The target settings are kept. */
    ProcessingSettings withSourceType (ProcessingSettings s, SourceType type);

    /** How flat the flattener's output comes out on some audio. */
    struct TrialResult
    {
        bool valid = false;
        float coverage = 0.0f;        // share of the audible sound that got a pitch
        float metered = 0.0f;         // share of tracked moments where the output pitch could be measured near the target
        float medianOffCents = 0.0f;  // how far the measured output is from the target (median)
        float flatness = 0.0f;        // coverage * metered * exp (-medianOffCents / 40): 0..1, higher is flatter
    };

    /**
     * Plays mono audio through the whole flattener (detector, tracker and shifter) with these settings, then meters
     * the output pitch every 20 ms of audible, tracked sound and compares it with the target. This measures what is
     * heard, so a tracker that is steady but not following the sound scores badly. Audio above ~50 kHz is first
     * brought down to around 48 kHz (the pitch is unaffected) to keep it quick.
     *
     * Allocates; for a background thread. cancel, when set, makes it return early (invalid).
     */
    TrialResult runTrial (const float* mono, int numSamples, double sampleRate, const ProcessingSettings& settings,
                          const std::atomic<bool>* cancel = nullptr);

    /** Which Source Type, if any, flattens this audio clearly better than the current settings. */
    struct SourceAdvice
    {
        bool valid = false;
        int recommended = -1;  // a SourceType, or -1 when none does clearly better
        TrialResult current;
        std::array<TrialResult, (size_t) SourceType::NumSourceTypes> bySource {};
    };

    /**
     * Runs runTrial() for the current settings and for every Source Type but Custom, and recommends the flattest one
     * only when it is clearly flatter than what is set now (flatness at least 0.35, 0.15 above the current one and
     * half as high again). currentSource (a SourceType, or -1) is never recommended: picking it again changes nothing.
     */
    SourceAdvice adviseSourceType (const float* mono, int numSamples, double sampleRate, const ProcessingSettings& current,
                                   int currentSource, const std::atomic<bool>* cancel = nullptr);
}

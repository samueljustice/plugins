#pragma once

#include <JuceHeader.h>
#include "Cargo.h"

/**
 * Works out what a dropped sample is (role) and gives it a readable name.
 * Pure DSP features + a file-name prior; no ML, no allocation constraints
 * (runs on the message thread on load, analyses at most the first 60 s).
 *
 * Features (computed on the detected sound regions only, so silence doesn't skew them):
 *   duration of sound, sustain ratio (sound time / total), spectral flatness, spectral centroid,
 *   low-energy ratio (< 150 Hz), pitch clarity (normalised autocorrelation peak, YIN-style),
 *   onset rate (spectral-flux peaks per second), crest factor, attack time.
 *
 * Rules (soft scores, highest wins; confidence = margin to runner-up):
 *   Impact  : short sound (< ~1.2 s per region), fast attack (< 15 ms), high crest
 *   Rumble  : low-energy ratio high, centroid low
 *   Tonal   : pitch clarity high, flatness low (animals, voices, whistles, creatures)
 *   Crackle : high onset rate, high flatness, high crest
 *   Air     : noisy (high flatness), single smooth swell per region, medium length (0.3 - 4 s)
 *   Bed     : long sustained sound (sustain ratio high, regions long), steady level
 *   Generic : nothing scores clearly
 * File name prior: keywords ("squeal", "pig", "growl", "roar", "dog", "bird", "vox", "scream" -> Tonal;
 *   "hit", "impact", "punch", "boom", "thud" -> Impact; "whoosh", "swish", "swoosh", "cloth", "flap" -> Air;
 *   "wind", "amb", "room", "tone", "drone", "bed" -> Bed; "fire", "crackle", "debris", "gravel" -> Crackle;
 *   "rumble", "sub", "lfe" -> Rumble; UCS CatID prefixes like "ANMLFarm", "WHSH", "WIND", "FIRE", "IMPCT")
 *   adds a strong bonus. A filename hit alone may decide when the audio is ambiguous.
 */
struct SampleAnalysis
{
    CargoRole role = CargoRole::Generic;
    float confidence = 0.0f;     // 0..1
    juce::String label;          // human name, e.g. "Pig Squeal", "Wind Bed", "Metal Hit"

    // Features, exposed for tests and the UI tooltip
    float soundSeconds = 0.0f, sustainRatio = 0.0f, flatness = 0.0f, centroidHz = 0.0f, lowRatio = 0.0f,
          pitchClarity = 0.0f, onsetRate = 0.0f, crest = 0.0f, attackMs = 0.0f;
};

namespace SampleClassifier
{
    /** Classify mono audio. regions = CargoData::detectRegions() output. fileName may be empty. */
    SampleAnalysis analyse(const float* mono, int numSamples, double sampleRate,
                           const std::vector<CargoRegion>& regions, const juce::String& fileName);

    /** Fills each region's focus (loudest 20 ms window; for impacts the steepest rise). */
    void findFocusPoints(const float* mono, int numSamples, double sampleRate, std::vector<CargoRegion>& regions);

    /** Title-case label from a file name: strips numbers, takes, UCS codes, "_v2", "-01" etc. */
    juce::String labelFromFileName(const juce::String& fileNameWithoutExtension);

    juce::String roleName(CargoRole role);        // "Bed", "Tonal", ...
    juce::String roleDescription(CargoRole role); // one line for tooltips
}

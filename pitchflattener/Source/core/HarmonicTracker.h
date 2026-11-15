#pragma once

// The harmonic tracker: the pitch of a whole sound, read in hindsight.
//
// For sounds with a harmonic series (voices, engines, drones, a ricochet's whine): it reads the
// whole sound at once, so every moment is judged knowing what came before and after it. It looks
// up to about 1 kHz; a sound whose pitch lives higher is the ridge tracker's (RidgeTracker.h).
//
// Plain C++20, no JUCE, no global state. Allocates and can take a second or more on long sounds:
// for a background thread, never the audio thread.

#include <cmath>
#include <string>
#include <vector>

namespace pitchflattener::core
{

inline double hzToMidi(double hz) noexcept { return 69.0 + 12.0 * std::log2(hz / 440.0); }
inline double midiToHz(double midi) noexcept { return 440.0 * std::exp2((midi - 69.0) / 12.0); }

/** "C".."B" with sharps. */
const char* pitchClassName(int pitchClass) noexcept;
/** The nearest note's name ("B2") and how far the pitch is from it, in cents (-50..50). */
std::string noteName(double midi, int* centsOut = nullptr);

enum class SfxMethod
{
    Auto,       // Tonal, or Engine where that fits better; Resonance if neither finds a pitch
    Tonal,      // voices, single tones
    Engine,     // sounds whose partials all move together (engines, rich drones)
    Resonance   // unpitched sounds: the strongest ring of the average spectrum
};

struct SfxAnalysis
{
    bool valid = false;
    SfxMethod usedMethod = SfxMethod::Tonal;
    double durationSeconds = 0.0;
    double hopSeconds = 0.005;
    std::vector<float> hz;        // per frame; 0 where no pitch
    bool pitched = false;         // enough of it has a pitch to act on
    double centreMidi = 0.0;      // the pitch the sound sits on
    float driftSemitones = 0.0f;  // how far its pitch travels (5th to 95th percentile)
    float tonalness = 0.0f;       // 0..1: how much to trust the pitch
};

SfxAnalysis analyseSfx(const float* mono, int numSamples, double sampleRate, SfxMethod method = SfxMethod::Auto);

namespace detail
{
/** Mends breaks in a pitch line (hz per frame, 0 = no pitch) that a harmonic interval explains. */
void repairHarmonicSlips(std::vector<float>& hz, const std::vector<float>& level);
}

} // namespace pitchflattener::core

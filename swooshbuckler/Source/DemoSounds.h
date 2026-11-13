#pragma once

#include <JuceHeader.h>

#include <vector>

/**
 * The demo sounds: four sources made from nothing when Load Demo Sounds is clicked (there are no recordings in
 * the app), so that Play makes a whoosh on a first run. One of each kind the rig is built around:
 *
 *   Demo Growl      a creature's growl in five phrases (a pitched voice through moving formants)   -> Tonal Content, bus A
 *   Demo Wind Bed   eight seconds of gusting wind, in stereo, that loops without a seam             -> Energy Bed, bus B
 *   Demo Swish      six air swishes of different lengths and brightness                             -> Air / Swish, bus B
 *   Demo Hit        four different impacts (a thump, a crack, a clang and a boom)                   -> Impact, the hits
 *
 * Always the same samples (fixed seeds), at 48 kHz. Message thread: making all four takes a few hundredths of a second.
 */
namespace DemoSounds
{
    inline constexpr double c_sampleRate = 48000.0;
    inline constexpr int c_count = 4;

    struct Sound
    {
        juce::String name;                // as a file would be named: "Demo Growl"
        juce::AudioBuffer<float> audio;   // one channel, or two (the bed)
    };

    juce::AudioBuffer<float> growl();
    juce::AudioBuffer<float> windBed();
    juce::AudioBuffer<float> swish();
    juce::AudioBuffer<float> hit();

    /** All four, in the order they are loaded. */
    std::vector<Sound> all();
}

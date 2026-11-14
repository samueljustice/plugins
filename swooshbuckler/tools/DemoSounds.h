#pragma once

#include <JuceHeader.h>

#include <vector>

class SwooshbucklerAudioProcessor;

/**
 * The demo sounds: five sources made from nothing, for our own tools and tests only (the picture tool, the
 * README pictures and the tests that need the rig playing). They are not part of the app or the plug-in, which
 * start empty. One of each kind the rig is built around:
 *
 *   Demo Growl      a creature's growl in five phrases (a pitched voice through moving formants)   -> Tonal Content, bus A
 *   Demo Wind Bed   eight seconds of gusting wind, in stereo, that loops without a seam             -> Energy Bed, bus B
 *   Demo Swish      six air swishes of different lengths and brightness                             -> Air / Swish, bus B
 *   Demo Hit        four different impacts (a thump, a crack, a clang and a boom)                   -> Impact, the hits
 *   Demo Rumble     eight seconds of deep rumble that loops without a seam                         -> Rumble: Stowaway while it
 *                                                                                                     is on and a Sub, else bus A
 *
 * Always the same samples (fixed seeds), at 48 kHz. Message thread: making all five takes a few hundredths of a second.
 */
namespace DemoSounds
{
    inline constexpr double c_sampleRate = 48000.0;
    inline constexpr int c_count = 5;

    struct Sound
    {
        juce::String name;                // as a file would be named: "Demo Growl"
        juce::AudioBuffer<float> audio;   // one channel, or two (the bed)
    };

    juce::AudioBuffer<float> growl();
    juce::AudioBuffer<float> windBed();
    juce::AudioBuffer<float> swish();
    juce::AudioBuffer<float> hit();
    juce::AudioBuffer<float> rumble();

    /** All five, in the order they are loaded. */
    std::vector<Sound> all();

    /** Makes the five and puts them in the processor's first empty tracks, as one undo step ("Load Demo Sounds").
        Returns how many were loaded (0 when every track is in use). Message thread. */
    int loadInto(SwooshbucklerAudioProcessor& processor);
}

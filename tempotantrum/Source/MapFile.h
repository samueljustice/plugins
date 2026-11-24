#pragma once

#include <JuceHeader.h>

#include "core/Types.h"

#include <vector>

namespace tt
{

// What the user has told the app about one piece of music. The engine's map is worked out from
// this every time, so this is what undo steps back through and what the saved map restores.
struct MapInputs
{
    Hint hint;
    Options options;
    std::vector<Pin> userPins;          // kept exactly by the engine
    std::vector<double> suppressed;     // times of engine pins the user deleted
    bool listenToDrums = false;         // detect the hits on the drum stem instead of the mix

    bool operator== (const MapInputs&) const;
};

// Where music captured from Pro Tools came from: its time 0 is `startSample` on the session's
// timeline. Saved in the map, so the capture reopened later still knows.
struct ProToolsOrigin
{
    bool known = false;                 // the music came from Pro Tools
    bool startKnown = false;            // and where it starts on the timeline is known
    juce::int64 startSample = 0, endSample = 0;
    double sessionRate = 0.0;
    juce::String sessionName;           // the session it was captured in
    juce::String name;                  // shown in the header: "<track name> <start>"
    juce::String source;                // how it was made

    bool operator== (const ProToolsOrigin& o) const
    {
        return known == o.known && startKnown == o.startKnown && startSample == o.startSample && endSample == o.endSample
            && juce::exactlyEqual (sessionRate, o.sessionRate) && sessionName == o.sessionName && name == o.name && source == o.source;
    }
};

// The map saved beside the music file as "<music file name>.tempotantrum.json".
namespace mapfile
{
    struct Contents
    {
        int formatVersion = 1;
        juce::String appVersion;
        juce::String musicFileName;
        juce::int64 lengthInSamples = 0;
        double sampleRate = 0.0;
        juce::uint64 fingerprint = 0;
        juce::String drumStem;          // path of the drum stem relative to the music's folder, or empty
        MapInputs inputs;
        std::vector<Pin> pins;          // the final map
        ProToolsOrigin proTools;        // for music captured from Pro Tools
    };

    constexpr int currentFormatVersion = 1;

    juce::File fileFor (const juce::File& musicFile);

    // A short summary of the audio, to recognise the same audio again: a 64-bit hash of a coarse
    // envelope (the mean absolute value of every 1024 samples, all channels), and the length.
    juce::uint64 fingerprint (const juce::AudioBuffer<float>& audio);

    juce::Result save (const juce::File& file, const Contents& contents);
    juce::Result load (const juce::File& file, Contents& contents);

    // Writes the click as a mono 24-bit WAV at the music's sample rate, `numSamples` long.
    juce::Result writeClick (const juce::File& file, const std::vector<float>& click,
                             double sampleRate, juce::int64 numSamples);
}

} // namespace tt

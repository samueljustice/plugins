// tt-cli: runs the listening engine on an audio file and prints the map as JSON.
//
//   tt-cli <audio file> [--first-hit seconds] [--bar N] [--beat N] [--bpm X] [--beats-per-bar N]
//          [--bridge whole|half] [--no-jumps] [--tolerance ms] [--pin-every N] [--drums <file>]
//          [--confirm time:beat ...] [--restart time:beat ...]
//          [--dump-onsets] [--onsets-file <file>] [--no-refine] [--no-steady]
//
// --confirm adds a user pin; --restart adds a user pin that starts a section. --pin-every N keeps a
// pin on beat 1 of a bar at least every N bars where a hit is there (0 for only the pins the tempo
// needs). --dump-onsets (or --list-onsets) adds every hit to the output as "onsetList", each a
// [time, strength] pair. --onsets-file reads the hits ("time strength" per line) instead of
// detecting them, --no-refine leaves the hinted tempo as given and --no-steady follows every
// stretch hit by hit without trying a constant tempo first; these three are for checking the engine.

#include "../Source/core/Internal.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>

namespace
{

struct Audio
{
    std::vector<float> mono;
    double sampleRate = 0.0;
};

bool readMono (const juce::String& path, Audio& out)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File::getCurrentWorkingDirectory().getChildFile (path)));
    if (reader == nullptr)
        return false;
    const auto length = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> buffer ((int) reader->numChannels, length);
    reader->read (&buffer, 0, length, 0, true, true);
    out.sampleRate = reader->sampleRate;
    out.mono.assign ((size_t) length, 0.0f);
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        juce::FloatVectorOperations::add (out.mono.data(), buffer.getReadPointer (ch), length);
    juce::FloatVectorOperations::multiply (out.mono.data(), 1.0f / (float) juce::jmax (1, buffer.getNumChannels()), length);
    return true;
}

bool parsePin (const juce::String& text, bool restart, std::vector<tt::Pin>& pins)
{
    if (! text.containsChar (':'))
        return false;
    tt::Pin pin;
    pin.time = text.upToFirstOccurrenceOf (":", false, false).getDoubleValue();
    pin.beat = text.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
    pin.user = true;
    pin.restart = restart;
    pins.push_back (pin);
    return true;
}

double millisecondsSince (std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
}

const char* kindName (tt::Section::Kind k)
{
    switch (k)
    {
        case tt::Section::Kind::first: return "first";
        case tt::Section::Kind::gap:   return "gap";
        case tt::Section::Kind::jump:  return "jump";
        case tt::Section::Kind::user:  return "user";
    }
    return "";
}

const char* kindName (tt::Check::Kind k)
{
    switch (k)
    {
        case tt::Check::Kind::restart: return "restart";
        case tt::Check::Kind::noHits:  return "noHits";
        case tt::Check::Kind::unsure:  return "unsure";
    }
    return "";
}

int usage()
{
    std::cerr << "usage: tt-cli <audio file> [--first-hit seconds] [--bar N] [--beat N] [--bpm X] [--beats-per-bar N]\n"
                 "              [--bridge whole|half] [--no-jumps] [--tolerance ms] [--pin-every N] [--drums <file>]\n"
                 "              [--confirm time:beat ...] [--restart time:beat ...] [--dump-onsets]\n"
                 "              [--onsets-file <file>] [--no-refine] [--no-steady]\n";
    return 2;
}

} // namespace

int main (int argc, char* argv[])
{
    juce::String file, drums, onsetsFile;
    tt::Hint hint;
    tt::Options options;
    tt::detail::Tuning tuning;
    std::vector<tt::Pin> userPins;
    bool listOnsets = false;

    for (int i = 1; i < argc; ++i)
    {
        const auto arg = juce::String::fromUTF8 (argv[i]);
        auto value = [&] { return i + 1 < argc ? juce::String::fromUTF8 (argv[++i]) : juce::String(); };
        if (arg == "--first-hit")            hint.firstHitTime = value().getDoubleValue();
        else if (arg == "--bar")             hint.bar = value().getIntValue();
        else if (arg == "--beat")            hint.beat = value().getIntValue();
        else if (arg == "--bpm")             hint.bpm = value().getDoubleValue();
        else if (arg == "--beats-per-bar")   hint.beatsPerBar = value().getIntValue();
        else if (arg == "--bridge")          options.bridge = value() == "half" ? tt::Options::Bridge::halfBeat : tt::Options::Bridge::wholeBeat;
        else if (arg == "--no-jumps")        options.detectJumps = false;
        else if (arg == "--tolerance")       options.pinToleranceMs = value().getDoubleValue();
        else if (arg == "--drums")           drums = value();
        else if (arg == "--confirm")         { if (! parsePin (value(), false, userPins)) return usage(); }
        else if (arg == "--restart")         { if (! parsePin (value(), true, userPins)) return usage(); }
        else if (arg == "--pin-every")       options.pinEveryBars = value().getIntValue();
        else if (arg == "--list-onsets" || arg == "--dump-onsets")   listOnsets = true;
        else if (arg == "--onsets-file")     onsetsFile = value();
        else if (arg == "--no-refine")       tuning.refineTempo = false;
        else if (arg == "--no-steady")       tuning.steady = false;
        else if (arg.startsWith ("--"))      return usage();
        else                                 file = arg;
    }
    if (file.isEmpty())
        return usage();

    Audio mix;
    if (! readMono (file, mix))
    {
        std::cerr << "tt-cli: cannot read " << file << "\n";
        return 1;
    }
    const double duration = (double) mix.mono.size() / mix.sampleRate;

    std::vector<tt::Onset> onsets;
    auto clock = std::chrono::steady_clock::now();
    if (onsetsFile.isNotEmpty())
    {
        std::ifstream in (onsetsFile.toStdString());
        tt::Onset o;
        while (in >> o.time >> o.strength)
            onsets.push_back (o);
    }
    else if (drums.isNotEmpty())
    {
        Audio stem;
        if (! readMono (drums, stem) || ! juce::exactlyEqual (stem.sampleRate, mix.sampleRate))
        {
            std::cerr << "tt-cli: cannot read " << drums << " at the same sample rate\n";
            return 1;
        }
        stem.mono.resize (mix.mono.size(), 0.0f);
        clock = std::chrono::steady_clock::now();
        onsets = tt::findOnsets (stem.mono.data(), stem.mono.size(), mix.sampleRate, mix.mono.data());
    }
    else
        onsets = tt::findOnsets (mix.mono.data(), mix.mono.size(), mix.sampleRate);
    const double onsetsMs = millisecondsSince (clock);

    clock = std::chrono::steady_clock::now();
    const auto map = tt::detail::mapCueWith (onsets, duration, hint, userPins, options, tuning);
    const double mapMs = millisecondsSince (clock);
    const tt::BeatGrid grid (map.pins, map.bpm);

    std::printf ("{\n  \"bpm\": %.4f,\n  \"firstHitTime\": %.6f,\n  \"duration\": %.6f,\n  \"sampleRate\": %.1f,\n", map.bpm, map.firstHitTime, duration, mix.sampleRate);
    std::printf ("  \"pins\": [");
    for (size_t i = 0; i < map.pins.size(); ++i)
    {
        const auto& p = map.pins[i];
        std::printf ("%s\n    {\"beat\": %.4f, \"time\": %.6f, \"user\": %s, \"restart\": %s, \"position\": \"%s\"}", i ? "," : "",
                     p.beat, p.time, p.user ? "true" : "false", p.restart ? "true" : "false", tt::formatBarBeat (p.beat, hint.beatsPerBar).c_str());
    }
    std::printf ("\n  ],\n  \"sections\": [");
    for (size_t i = 0; i < map.sections.size(); ++i)
    {
        const auto& s = map.sections[i];
        std::printf ("%s\n    {\"kind\": \"%s\", \"startTime\": %.6f, \"endTime\": %.6f, \"startBeat\": %.4f, \"endBeat\": %.4f, \"bpm\": %.4f, \"confirmed\": %s}", i ? "," : "",
                     kindName (s.kind), s.startTime, s.endTime, s.startBeat, grid.beatAtTime (s.endTime), s.bpm, s.confirmed ? "true" : "false");
    }
    std::printf ("\n  ],\n  \"checks\": [");
    for (size_t i = 0; i < map.checks.size(); ++i)
    {
        const auto& c = map.checks[i];
        std::printf ("%s\n    {\"kind\": \"%s\", \"startTime\": %.6f, \"endTime\": %.6f, \"note\": \"%s\"}", i ? "," : "",
                     kindName (c.kind), c.startTime, c.endTime, c.note.c_str());
    }
    std::printf ("\n  ],\n");
    if (listOnsets)
    {
        std::printf ("  \"onsetList\": [");
        for (size_t i = 0; i < map.onsets.size(); ++i)
            std::printf ("%s[%.6f, %.4f]", i ? ", " : "", map.onsets[i].time, (double) map.onsets[i].strength);
        std::printf ("],\n");
    }
    std::printf ("  \"onsets\": %d,\n  \"timing\": {\"findOnsetsMs\": %.1f, \"mapCueMs\": %.1f}\n}\n", (int) map.onsets.size(), onsetsMs, mapMs);
    return 0;
}

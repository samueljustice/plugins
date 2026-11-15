// Flattens one WAV with the plug-in's core, for listening tests on real recordings (no host, no JUCE).
//
//   FlattenFile in.wav outFolder name
//
// Writes "<name> - 1 source.wav", "<name> - 2 flat (pick, <tracker> line).wav" and, where the
// other tracker's line is usable too, "<name> - 3 flat (other, <tracker> line).wav", flattened
// to the high point, and prints what it found. With PF_LINES set it also writes
// "<name>.lines.csv" (tracker,time,hz) for drawing the lines over a spectrogram.

#include "core/Flatten.h"
#include "core/HarmonicTracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace pitchflattener::core;

namespace
{
#include "wav.inc"
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::printf("usage: FlattenFile in.wav outFolder name\n");
        return 2;
    }
    Wav in;
    const std::string folder = argv[2], name = argv[3];
    if (! load(argv[1], in) || in.channels.empty())
    {
        std::printf("%s\n    could not be read\n", name.c_str());
        return 1;
    }
    save24(folder + "/" + name + " - 1 source.wav", in);
    std::printf("%s\n", name.c_str());

    const auto analysis = analyse(in.channels, in.rate);
    const char* names[kNumTrackers] { "voice", "engine", "tone" };
    if (std::getenv("PF_LINES") != nullptr)
    {
        std::ofstream csv(folder + "/" + name + ".lines.csv");
        for (int k = 0; k < kNumTrackers; ++k)
        {
            const auto& line = analysis.lines[static_cast<std::size_t>(k)];
            for (std::size_t f = 0; f < line.trackedHz.size(); ++f)
                csv << names[k] << ',' << (static_cast<double>(f) + 0.5) * line.hopSeconds << ',' << line.trackedHz[f] << '\n';
        }
        // The line the flattener would use (leaps out, gaps bridged, smoothed), to compare with the readings.
        if (const auto* used = analysis.picked())
            for (std::size_t f = 0; f < used->midi.size(); ++f)
                csv << "used" << ',' << (static_cast<double>(f) + 0.5) * used->hopSeconds << ',' << midiToHz(used->midi[f]) << '\n';
    }
    for (int k = 0; k < kNumTrackers; ++k)
    {
        const auto& line = analysis.lines[static_cast<std::size_t>(k)];
        if (line.midi.empty())
        {
            std::printf("    %-8s line: nothing found\n", names[k]);
            continue;
        }
        std::printf("    %-8s line: %s, found in %3.0f%% of the sound, travels %5.1f st (%s to %s), %d leaps taken out, unsteadiness %.3f st", names[k],
                    line.usable ? "usable" : "NOT usable", line.coverage * 100.0, line.highMidi - line.lowMidi, noteName(line.lowMidi).c_str(),
                    noteName(line.highMidi).c_str(), line.leaps, line.jitter);
        if (line.residual >= 0.0)
            std::printf("; flattened it still travels %.1f st", line.residual);
        std::printf("\n");
    }
    if (analysis.pick < 0)
    {
        std::printf("    no steady pitch found: left alone\n");
        return 0;
    }
    const double seconds = static_cast<double>(in.channels[0].size()) / in.rate;
    // The pick first, then every other usable line, so the choice can be judged by ear.
    int number = 2;
    for (int which = -1; which < kNumTrackers; ++which)
    {
        const int k = which < 0 ? analysis.pick : which;
        if (which == analysis.pick || ! analysis.lines[static_cast<std::size_t>(k)].usable)
            continue;
        Options options;
        options.line = k;
        const auto result = flatten(in.channels, in.rate, analysis, options);
        if (! result.valid)
            continue;
        Wav out;
        out.rate = in.rate;
        out.channels = result.channels;
        save24(folder + "/" + name + " - " + std::to_string(number++) + " flat (" + (which < 0 ? "pick, " : "other, ") + names[k] + " line).wav", out);
        if (which < 0)
        {
            // The pick again with its length kept, in each character, to compare by ear.
            const char* characters[3] { "harmonic", "transients", "voice" };
            for (int c = 0; c < 3; ++c)
            {
                options.keepLength = true;
                options.character = static_cast<Character>(c);
                const auto kept = flatten(in.channels, in.rate, analysis, options);
                if (! kept.valid)
                    continue;
                Wav k2;
                k2.rate = in.rate;
                k2.channels = kept.channels;
                save24(folder + "/" + name + " - " + std::to_string(number++) + " flat, length kept, " + characters[c] + " (pick, " + names[k] + " line).wav", k2);
            }
            options.keepLength = false;
        }
        std::printf("    %s: %s line, flat at %s, length %.2f -> %.2f s\n", which < 0 ? "pick" : "other", names[k], noteName(result.targetMidi).c_str(), seconds,
                    static_cast<double>(out.channels[0].size()) / in.rate);
    }
    return 0;
}

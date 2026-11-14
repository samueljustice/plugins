// Channel ident files: for checking which speaker each channel of a Swooshbuckler file lands on in Pro Tools,
// Soundminer or anything else.
//
// For every width (Mono to 7.1.4), Channel Order (Pro Tools (Film), SMPTE / WAV; one file where the two are the
// same) and File Layout (Interleaved, Multi-Mono), one take is written the way the tape writes its own
// (TapeRecorder::FileWriter): same headers, speaker masks, channel names and suffixes. The speakers take turns:
// the speaker that is number k in the Pro Tools (Film) order of its width beeps k times at a pitch of its own (the
// LFE at 50 Hz), the others are silent, and each turn starts with a marker named after the speaker. The whole
// round is played twice. A README.txt beside the files lists each width's speakers, beeps and pitches.
//
// Usage: SwooshbucklerIdent <folder> [--types wav,aif,flac] [--rate 48000] [--bits 24]

#include <JuceHeader.h>
#include "Room.h"
#include "TakeFile.h"
#include "TapeRecorder.h"

#include <cmath>
#include <iostream>

namespace
{
    constexpr double c_beepSeconds = 0.12, c_gapSeconds = 0.12, c_turnGapSeconds = 0.6, c_level = 0.25; // -12 dBFS

    double pitchOf(room::Speaker s)
    {
        if (s == room::Speaker::Lfe)
            return 50.0;
        return 300.0 * std::pow(2.0, static_cast<double>(static_cast<int>(s)) / 6.0); // a whole tone apart
    }

    juce::String fileWidthName(room::Width w)
    {
        return juce::String(room::layout(w).name).replaceCharacter('.', '-'); // a dot would read as a Multi-Mono suffix
    }

    /** The ident of one width in processing order, and its markers. */
    juce::AudioBuffer<float> identOf(room::Width w, double rate, std::vector<TapeRecorder::Cue>& markers)
    {
        const auto& l = room::layout(w);
        const auto order = room::fileChannels(w, room::FileOrder::ProTools); // the speaker numbers: Pro Tools' order
        const int beep = static_cast<int>(c_beepSeconds * rate), gap = static_cast<int>(c_gapSeconds * rate);
        const int turnGap = static_cast<int>(c_turnGapSeconds * rate);
        int length = turnGap;
        for (int k = 1; k <= l.numChannels; ++k)
            length += k * (beep + gap) + turnGap;
        juce::AudioBuffer<float> audio(l.numChannels, 2 * length);
        audio.clear();
        markers.clear();
        int at = turnGap;
        const int fade = static_cast<int>(0.005 * rate);
        for (int round = 0; round < 2; ++round)
        {
            for (int k = 1; k <= l.numChannels; ++k)
            {
                const int ch = order[static_cast<size_t>(k - 1)];
                const auto speaker = l.speakers[static_cast<size_t>(ch)];
                markers.push_back({ at, room::info(speaker).name });
                const double w0 = juce::MathConstants<double>::twoPi * pitchOf(speaker) / rate;
                for (int b = 0; b < k; ++b)
                {
                    for (int i = 0; i < beep; ++i)
                    {
                        const double env = std::min({ 1.0, static_cast<double>(i) / fade, static_cast<double>(beep - 1 - i) / fade });
                        audio.setSample(ch, at + i, static_cast<float>(c_level * env * std::sin(w0 * i)));
                    }
                    at += beep + gap;
                }
                at += turnGap;
            }
            if (round == 0)
                at += turnGap;
        }
        return audio;
    }

    juce::String describe(room::Width w)
    {
        const auto& l = room::layout(w);
        const auto order = room::fileChannels(w, room::FileOrder::ProTools);
        juce::String text;
        text << l.name << ":";
        for (int k = 1; k <= l.numChannels; ++k)
        {
            const auto s = l.speakers[static_cast<size_t>(order[static_cast<size_t>(k - 1)])];
            text << "  " << room::info(s).name << " " << k << (k == 1 ? " beep" : " beeps") << " at " << juce::roundToInt(pitchOf(s)) << " Hz;";
        }
        return text.dropLastCharacters(1);
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add(argv[i]);
    if (args.isEmpty() || args[0].startsWith("--"))
    {
        std::cout << "Usage: SwooshbucklerIdent <folder> [--types wav,aif,flac] [--rate 48000] [--bits 24]" << std::endl;
        return 1;
    }
    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile(args[0]);
    auto value = [&](const juce::String& key, const juce::String& fallback)
    {
        const int at = args.indexOf(key);
        return at >= 0 && at + 1 < args.size() ? args[at + 1] : fallback;
    };
    const double rate = value("--rate", "48000").getDoubleValue();
    const int bits = value("--bits", "24").getIntValue();
    auto typeNames = juce::StringArray::fromTokens(value("--types", "wav"), ",", "");
    typeNames.trim();
    if (! folder.isDirectory() && ! folder.createDirectory())
    {
        std::cout << "Couldn't make " << folder.getFullPathName() << std::endl;
        return 1;
    }

    juce::String readme;
    readme << "Swooshbuckler channel ident files\n\n"
           << "Each file plays its speakers in turn, twice round. The speaker that is number k in the Pro Tools (Film) order "
              "beeps k times, at a pitch of its own; every turn has a marker with the speaker's name. Import a file and check "
              "that each turn comes out of the speaker it names.\n\n"
           << "Names: Ident_<width>_<order>.<ext> for interleaved files (ProTools = Pro Tools (Film), SMPTE = SMPTE / WAV; "
              "Mono to Quad have one order, written as ProTools), Ident_<width>_MultiMono.<suffix>.<ext> for Multi-Mono sets.\n\n";

    int written = 0, failed = 0;
    constexpr room::Width widths[] = { room::Width::Mono, room::Width::Stereo, room::Width::Quad, room::Width::Surround51,
                                       room::Width::Surround71, room::Width::Surround712, room::Width::Surround714 };
    for (auto w : widths)
    {
        readme << describe(w) << "\n";
        readme << "  Pro Tools (Film) order: " << room::orderLabel(w, room::FileOrder::ProTools)
               << ".  SMPTE / WAV order: " << room::orderLabel(w, room::FileOrder::Smpte) << ".\n";
        std::vector<TapeRecorder::Cue> markers;
        const auto audio = identOf(w, rate, markers);
        for (const auto& typeName : typeNames)
        {
            const auto type = typeName == "aif" || typeName == "aiff" ? TapeRecorder::FileType::Aiff
                            : typeName == "flac" ? TapeRecorder::FileType::Flac : TapeRecorder::FileType::Wav;
            const auto ext = TapeRecorder::extensionOf(type);
            struct Variant
            {
                room::FileOrder order;
                takefile::Layout layout;
                juce::String name;
            };
            std::vector<Variant> variants { { room::FileOrder::ProTools, takefile::Layout::Interleaved, "ProTools" } };
            if (juce::String(room::orderLabel(w, room::FileOrder::ProTools)) != room::orderLabel(w, room::FileOrder::Smpte))
                variants.push_back({ room::FileOrder::Smpte, takefile::Layout::Interleaved, "SMPTE" });
            if (w != room::Width::Mono)
                variants.push_back({ room::FileOrder::ProTools, takefile::Layout::MultiMono, "MultiMono" });
            for (const auto& v : variants)
            {
                TapeRecorder::Options o;
                o.type = type;
                o.bitDepth = bits;
                o.order = v.order;
                o.layout = v.layout;
                TapeRecorder::Naming naming;
                naming.prefix = "Ident";
                naming.description = "Swooshbuckler channel ident, " + juce::String(room::layout(w).name) + ", "
                                   + (v.layout == takefile::Layout::MultiMono ? juce::String("Multi-Mono") : juce::String(room::fileOrderName(v.order)) + " order: "
                                      + room::orderLabel(w, v.order));
                naming.software = "Swooshbuckler ident";
                const auto file = folder.getChildFile("Ident_" + fileWidthName(w) + "_" + v.name + ext);
                TapeRecorder::FileWriter writer;
                juce::String error;
                const bool ok = writer.open(file, rate, audio.getNumChannels(), o, naming, error) && writer.write(audio, 0, audio.getNumSamples())
                             && writer.close(markers);
                if (ok)
                {
                    ++written;
                    std::cout << "  " << takefile::displayName(writer.files()) << std::endl;
                }
                else
                {
                    ++failed;
                    std::cout << "  skipped " << file.getFileName() << ": " << error << std::endl;
                }
            }
        }
        readme << "\n";
    }
    folder.getChildFile("README.txt").replaceWithText(readme);
    std::cout << written << " written, " << failed << " skipped, into " << folder.getFullPathName() << std::endl;
    return 0;
}

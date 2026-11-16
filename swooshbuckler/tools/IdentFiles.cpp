// Channel ident files: for checking which speaker each channel of a Swooshbuckler file lands on in Pro Tools,
// Soundminer or anything else.
//
// For every width (Mono to 7.1.4), Channel Order (Like Pro Tools, and WAV Standard where it is a different file:
// 7.1.4) and File Layout (Interleaved, Multi-Mono), one take is written the way the tape writes its own
// (TapeRecorder::FileWriter): same headers, speaker masks, iXML track lists, channel names and suffixes. The
// speakers take turns: the speaker that is number k in the order Pro Tools lists a track's channels (L C R ...)
// beeps k times at a pitch of its own (the LFE at 50 Hz), the others are silent, and each turn starts with a marker
// named after the speaker. The whole round is played twice. The README.txt written beside the files is the
// checklist for Pro Tools, Soundminer and the signed AAX, with each width's speakers, beeps and pitches.
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
        const auto order = room::trackChannels(w); // the speaker numbers: the order of a Pro Tools track
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
        const auto order = room::trackChannels(w);
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
    readme << "Swooshbuckler channel ident files: what to check\n"
              "=================================================\n\n"
              "Channel Order now defaults to Like Pro Tools: each width is written the way Pro Tools 2026 itself\n"
              "bounces it (Quad to 7.1.2 in the WAV order with the speaker mask in the header; 7.1.4 in Pro Tools'\n"
              "own order with a plain header and an iXML track list naming each channel). These files confirm that\n"
              "Pro Tools, Soundminer and the rest read them the way they read Pro Tools' own files, and that Pro\n"
              "Tools joins the Multi-Mono sets into one clip.\n\n"
              "HOW EACH FILE SOUNDS\n"
              "Every speaker takes a turn, twice round. The speaker that is number k in the order Pro Tools lists a\n"
              "track's channels (L C R ...) beeps k times, at a pitch of its own, and every turn has a marker with\n"
              "the speaker's name. So on a Pro Tools track of the right width, a correct file beeps 1, 2, 3 ... from\n"
              "the first meter to the last. The LFE beeps low (50 Hz): watch its meter if you can't hear it.\n\n"
              "THE FILES\n"
              "  Ident_<width>_LikeProTools.<ext>   interleaved, Channel Order Like Pro Tools (the default)\n"
              "  Ident_7-1-4_WavStandard.<ext>      interleaved 7.1.4 in the WAV order, mask 0x2D63F in the header\n"
              "                                     (below 7.1.4 WAV Standard is the same file as Like Pro Tools)\n"
              "  Ident_<width>_MultiMono.<speaker>.<ext>   one mono file per speaker\n"
              "  A FLAC file is always in FLAC's own order (the WAV order), so it has one variant.\n\n"
              "IN PRO TOOLS (a 48 kHz session)\n"
              " 1. Import each Ident_<width>_LikeProTools.wav (File > Import > Audio, or drag it into the clip list).\n"
              "    It should come in as ONE clip of its width. Put it on a track of that width and play it:\n"
              "    the beeps count 1, 2, 3 ... across the track's meters, at every width.\n"
              " 2. Ident_7-1-4_WavStandard.wav: the same check. If it does not count 1 to 12, Pro Tools ignores the\n"
              "    mask at 7.1.4 (which is why Like Pro Tools is the default).\n"
              " 3. 7.1.2: the two overhead speakers are marked top front in the header, as in Pro Tools' own 7.1.2\n"
              "    bounce. Beeps 9 and 10 land on the two overhead meters (Lts Rts).\n"
              " 4. Each Multi-Mono set: select all its files and import them together. Does Pro Tools join them\n"
              "    into one multichannel clip? Watch especially 7.1.2 (.Lts .Rts, Avid's names for its tops) and\n"
              "    7.1.4 (.Ltf .Rtf .Ltr .Rtr).\n"
              " 5. Do the markers show up, named after the speakers?\n\n"
              "IN SOUNDMINER\n"
              " 6. Open the folder. For each interleaved file: the right channel count and channel names, and each\n"
              "    turn on the speaker it names? (7.1.4 Like Pro Tools names its channels only in iXML, as Pro Tools'\n"
              "    own 7.1.4 files do.)\n"
              " 7. Spot / transfer the 5.1 and 7.1.4 files to Pro Tools from Soundminer and repeat step 1.\n"
              " 8. Does Soundminer show a Multi-Mono set as one sound or as separate files?\n\n"
              "WITH THE SIGNED AAX BUILD (in Pro Tools)\n"
              " 9. New instrument track at each width: mono, stereo, quad, 5.1, 7.1, 7.1.2, 7.1.4. Swooshbuckler is\n"
              "    offered on every one, and the room strip reads that width with \"From Track\".\n"
              "10. On a 7.1.4 track, Flight Path Overhead, Centre Speaker On, LFE Mode Mixdown: every meter of the\n"
              "    track moves, the centre and LFE included, and the plan of the room in the sea lights the same\n"
              "    speakers as Pro Tools' meters. A pass on Across is loudest in L and R.\n"
              "11. Change the track's format (or move the plug-in to a track of another width) with a saved\n"
              "    session: it reopens and plays at the new width.\n"
              "12. Bounce a 5.1 and a 7.1.4 track (offline too): the tails are kept and the result matches what you\n"
              "    heard.\n"
              "13. The AudioSuite version: it is an instrument, so check only that it does not appear broken in the\n"
              "    AudioSuite menu.\n\n"
              "WHAT EACH WIDTH PLAYS (speaker: beeps at pitch), AND ITS ORDER IN EACH FILE\n\n";

    int written = 0, failed = 0;
    constexpr room::Width widths[] = { room::Width::Mono, room::Width::Stereo, room::Width::Quad, room::Width::Surround51,
                                       room::Width::Surround71, room::Width::Surround712, room::Width::Surround714 };
    for (auto w : widths)
    {
        readme << describe(w) << "\n";
        readme << "  Like Pro Tools: " << room::orderLabel(w, room::FileOrder::ProTools);
        if (room::ordersDiffer(w))
            readme << ".  WAV Standard: " << room::orderLabel(w, room::FileOrder::WavStandard);
        readme << ".\n";
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
            std::vector<Variant> variants { { room::FileOrder::ProTools, takefile::Layout::Interleaved, "LikeProTools" } };
            if (room::ordersDiffer(w) && type != TapeRecorder::FileType::Flac) // a FLAC file has one order, its own
                variants.push_back({ room::FileOrder::WavStandard, takefile::Layout::Interleaved, "WavStandard" });
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
                const auto fileOrder = takefile::orderFor(TapeRecorder::containerOf(type), v.order);
                naming.description = "Swooshbuckler channel ident, " + juce::String(room::layout(w).name) + ", "
                                   + (v.layout == takefile::Layout::MultiMono ? juce::String("Multi-Mono")
                                                                              : juce::String(room::fileOrderName(fileOrder)) + " order: " + room::orderLabel(w, fileOrder));
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
    readme << "To write them again: SwooshbucklerIdent <folder> --types wav (add aif,flac for those types; FLAC above\n"
              "8 channels is refused). This README is written by the tool each time.\n";
    folder.getChildFile("README.txt").replaceWithText(readme);
    std::cout << written << " written, " << failed << " skipped, into " << folder.getFullPathName() << std::endl;
    return 0;
}

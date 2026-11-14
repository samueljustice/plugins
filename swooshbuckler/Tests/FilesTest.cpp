// Files test: takes of every room width on disk.
//
//   - Every width x Channel Order x File Layout x file type is written (TapeRecorder::FileWriter, what Render
//     Variations uses) and read back with a parser of this test's own: the WAV header (format tag, channel count,
//     the speaker mask: the right one, not JUCE's five-front 7.1 or none at all), the AIFF header, the channel
//     names in the metadata, and each channel's sound in the slot the order puts it (the orders are typed out
//     here, not taken from Room). The markers are there, a Multi-Mono set is complete and named the Pro Tools way
//     with nothing else beside it, and the take reader hands every one back in the room's order.
//   - FLAC: wider than eight channels is refused with a plain message and leaves nothing.
//   - All or nothing: a Multi-Mono set that can't be written whole, or is abandoned, leaves no file behind.
//   - The tape at 12 and 6 channels, interleaved and Multi-Mono, with its markers; a whoosh too short to keep
//     leaves no file of its set; Mono of a 7.1.4 take is the room's fold, lined up, and held under the Ceiling.
//   - The saved last minutes: capped at 512 MB with the real length reported, and saved at 12 channels.
//   - RF64: the mask is set in an RF64 header too.
//   - Audition: a 7.1.4 file and a Multi-Mono set play each channel to its own speaker; narrower and wider takes
//     are placed or folded.
//   - The rig: Channel Order and File Layout default to Pro Tools (Film) and Interleaved and are kept; a Multi-Mono
//     set is one take in the Takes, and deleting it deletes every file.
//
// Usage: SwooshbucklerFiles [scratch folder]. Run it from a scratch folder; nothing is read from or written to the
// user's settings.

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Room.h"
#include "TakeAudition.h"
#include "TakeFile.h"
#include "TapeRecorder.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <map>

namespace
{
    constexpr double c_fs = 48000.0;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok)
            ++g_failures;
    }

    using W = room::Width;
    constexpr W c_widths[] = { W::Mono, W::Stereo, W::Quad, W::Surround51, W::Surround71, W::Surround712, W::Surround714 };
    juce::String nameOf(W w) { return room::layout(w).name; }

    // The orders, typed out (file channel by file channel).
    juce::String typedOrder(W w, room::FileOrder order)
    {
        const bool film = order == room::FileOrder::ProTools;
        switch (w)
        {
            case W::Mono:        return "C";
            case W::Stereo:      return "L R";
            case W::Quad:        return "L R Ls Rs";
            case W::Surround51:  return film ? "L C R Ls Rs LFE" : "L R C LFE Ls Rs";
            case W::Surround71:  return film ? "L C R Lss Rss Lsr Rsr LFE" : "L R C LFE Lsr Rsr Lss Rss";
            case W::Surround712: return film ? "L C R Lss Rss Lsr Rsr LFE Ltm Rtm" : "L R C LFE Lsr Rsr Lss Rss Ltm Rtm";
            case W::Surround714: return film ? "L C R Lss Rss Lsr Rsr LFE Ltf Rtf Ltr Rtr" : "L R C LFE Lsr Rsr Lss Rss Ltf Rtf Ltr Rtr";
        }
        return {};
    }

    // The WAV speaker masks, typed out.
    std::uint32_t typedMask(W w)
    {
        switch (w)
        {
            case W::Quad:        return 0x33;
            case W::Surround51:  return 0x3F;
            case W::Surround71:  return 0x63F;
            case W::Surround712: return 0x563F;
            case W::Surround714: return 0x2D63F;
            case W::Mono:
            case W::Stereo:      return 0;
        }
    }

    // The processing channel a speaker name is on.
    int processingIndex(W w, const juce::String& name)
    {
        for (int s = 0; s < room::c_numSpeakers; ++s)
            if (name == room::info(static_cast<room::Speaker>(s)).name)
                return room::layout(w).indexOf(static_cast<room::Speaker>(s));
        return -1;
    }

    // What processing channel p carries in these tests: a level of its own.
    float levelOf(int p) { return 0.05f * static_cast<float>(p + 1); }

    juce::AudioBuffer<float> levels(int channels, int samples)
    {
        juce::AudioBuffer<float> b(channels, samples);
        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::fill(b.getWritePointer(ch), levelOf(ch), samples);
        return b;
    }

    // Which channel's level a run of samples has (-1 for none).
    int channelOfLevel(double mean)
    {
        for (int p = 0; p < room::c_maxChannels; ++p)
            if (std::abs(mean - levelOf(p)) < 2.0e-3)
                return p;
        return -1;
    }

    //==================================================================================================================
    // This test's own readers: the bytes, not JUCE.
    struct Parsed
    {
        bool ok = false;
        bool rf64 = false;
        int tag = 0, channels = 0, bits = 0;
        std::uint32_t rate = 0;
        bool extensible = false;
        std::uint32_t mask = 0;
        std::uint32_t subTag = 0;
        std::int64_t frames = 0;
        std::vector<double> means; // each file channel's mean level
        juce::String ixml;
        int cues = 0;
    };

    std::uint32_t le32(const unsigned char* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
    std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
    std::uint32_t be32(const unsigned char* p) { return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]); }
    std::uint16_t be16(const unsigned char* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

    Parsed parseWav(const juce::File& file)
    {
        Parsed r;
        juce::MemoryBlock mb;
        if (! file.loadFileAsData(mb) || mb.getSize() < 12)
            return r;
        const auto* d = static_cast<const unsigned char*>(mb.getData());
        const size_t size = mb.getSize();
        r.rf64 = std::memcmp(d, "RF64", 4) == 0;
        if ((! r.rf64 && std::memcmp(d, "RIFF", 4) != 0) || std::memcmp(d + 8, "WAVE", 4) != 0)
            return r;
        size_t pos = 12, dataStart = 0, dataSize = 0;
        bool haveFmt = false;
        while (pos + 8 <= size)
        {
            const auto* id = d + pos;
            std::uint32_t len = le32(d + pos + 4);
            const auto* body = d + pos + 8;
            if (std::memcmp(id, "fmt ", 4) == 0)
            {
                haveFmt = true;
                r.tag = le16(body);
                r.channels = le16(body + 2);
                r.rate = le32(body + 4);
                r.bits = le16(body + 14);
                r.extensible = r.tag == 0xfffe && len >= 40 && le16(body + 16) == 22;
                if (r.extensible)
                {
                    r.mask = le32(body + 20);
                    r.subTag = le32(body + 24);
                }
            }
            else if (std::memcmp(id, "data", 4) == 0)
            {
                dataStart = pos + 8;
                dataSize = (len == 0xffffffffu) ? size - dataStart : len;
            }
            else if (std::memcmp(id, "iXML", 4) == 0)
                r.ixml = juce::String::fromUTF8(reinterpret_cast<const char*>(body), static_cast<int>(len));
            else if (std::memcmp(id, "cue ", 4) == 0)
                r.cues = static_cast<int>(le32(body));
            pos += 8 + len + (len & 1);
        }
        if (! haveFmt || dataStart == 0 || r.channels <= 0)
            return r;
        const int bytes = r.bits / 8;
        r.frames = static_cast<std::int64_t>(dataSize / static_cast<size_t>(bytes * r.channels));
        r.means.assign(static_cast<size_t>(r.channels), 0.0);
        for (std::int64_t f = 0; f < r.frames; ++f)
            for (int ch = 0; ch < r.channels; ++ch)
            {
                const auto* s = d + dataStart + static_cast<size_t>((f * r.channels + ch) * bytes);
                double v = 0.0;
                if (bytes == 3)
                    v = static_cast<double>(static_cast<std::int32_t>((static_cast<std::uint32_t>(s[0]) << 8) | (static_cast<std::uint32_t>(s[1]) << 16) | (static_cast<std::uint32_t>(s[2]) << 24)) >> 8) / 8388608.0;
                else if (bytes == 4)
                {
                    float x;
                    std::memcpy(&x, s, 4);
                    v = x;
                }
                else if (bytes == 2)
                    v = static_cast<std::int16_t>(le16(s)) / 32768.0;
                r.means[static_cast<size_t>(ch)] += v;
            }
        for (auto& m : r.means)
            m /= static_cast<double>(juce::jmax<std::int64_t>(1, r.frames));
        r.ok = true;
        return r;
    }

    Parsed parseAiff(const juce::File& file)
    {
        Parsed r;
        juce::MemoryBlock mb;
        if (! file.loadFileAsData(mb) || mb.getSize() < 12)
            return r;
        const auto* d = static_cast<const unsigned char*>(mb.getData());
        const size_t size = mb.getSize();
        if (std::memcmp(d, "FORM", 4) != 0 || std::memcmp(d + 8, "AIFF", 4) != 0)
            return r;
        size_t pos = 12, dataStart = 0;
        while (pos + 8 <= size)
        {
            const auto* id = d + pos;
            const std::uint32_t len = be32(d + pos + 4);
            const auto* body = d + pos + 8;
            if (std::memcmp(id, "COMM", 4) == 0)
            {
                r.channels = be16(body);
                r.frames = be32(body + 2);
                r.bits = be16(body + 6);
            }
            else if (std::memcmp(id, "SSND", 4) == 0)
                dataStart = pos + 8 + 8 + be32(body);
            pos += 8 + len + (len & 1);
        }
        if (dataStart == 0 || r.channels <= 0)
            return r;
        const int bytes = r.bits / 8;
        r.means.assign(static_cast<size_t>(r.channels), 0.0);
        for (std::int64_t f = 0; f < r.frames; ++f)
            for (int ch = 0; ch < r.channels; ++ch)
            {
                const auto* s = d + dataStart + static_cast<size_t>((f * r.channels + ch) * bytes);
                const double v = bytes == 3 ? static_cast<double>(static_cast<std::int32_t>((static_cast<std::uint32_t>(s[0]) << 24) | (static_cast<std::uint32_t>(s[1]) << 16) | (static_cast<std::uint32_t>(s[2]) << 8)) >> 8) / 8388608.0
                                            : static_cast<std::int16_t>(be16(s)) / 32768.0;
                r.means[static_cast<size_t>(ch)] += v;
            }
        for (auto& m : r.means)
            m /= static_cast<double>(juce::jmax<std::int64_t>(1, r.frames));
        r.ok = true;
        return r;
    }

    // FLAC has no parser here: JUCE's own FLAC reader, which leaves the channels in the file's order.
    Parsed parseFlac(const juce::File& file)
    {
        Parsed r;
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::AudioFormatReader> reader(flac.createReaderFor(new juce::FileInputStream(file), true));
        if (reader == nullptr)
            return r;
        r.channels = static_cast<int>(reader->numChannels);
        r.frames = reader->lengthInSamples;
        juce::AudioBuffer<float> b(r.channels, static_cast<int>(r.frames));
        reader->read(b.getArrayOfWritePointers(), r.channels, 0, b.getNumSamples());
        for (int ch = 0; ch < r.channels; ++ch)
        {
            double sum = 0.0;
            for (int i = 0; i < b.getNumSamples(); ++i)
                sum += b.getSample(ch, i);
            r.means.push_back(sum / juce::jmax(1, b.getNumSamples()));
        }
        r.ok = true;
        return r;
    }

    Parsed parse(const juce::File& file, TapeRecorder::FileType type)
    {
        switch (type)
        {
            case TapeRecorder::FileType::Aiff: return parseAiff(file);
            case TapeRecorder::FileType::Flac: return parseFlac(file);
            case TapeRecorder::FileType::Wav:
            default: return parseWav(file);
        }
    }

    juce::String typeName(TapeRecorder::FileType type)
    {
        return type == TapeRecorder::FileType::Aiff ? "AIFF" : type == TapeRecorder::FileType::Flac ? "FLAC" : "WAV";
    }

    /** The take's channels, read back by takefile, as each one's mean level. */
    std::vector<double> readMeans(const juce::File& anyFile, int& channels)
    {
        std::vector<double> means;
        channels = 0;
        const auto reader = takefile::openReader(anyFile);
        if (reader == nullptr)
            return means;
        channels = static_cast<int>(reader->numChannels);
        juce::AudioBuffer<float> b(channels, static_cast<int>(reader->lengthInSamples));
        reader->read(b.getArrayOfWritePointers(), channels, 0, b.getNumSamples());
        for (int ch = 0; ch < channels; ++ch)
        {
            double sum = 0.0;
            for (int i = 0; i < b.getNumSamples(); ++i)
                sum += b.getSample(ch, i);
            means.push_back(sum / juce::jmax(1, b.getNumSamples()));
        }
        return means;
    }

    bool inProcessingOrder(const std::vector<double>& means, int channels)
    {
        if (static_cast<int>(means.size()) != channels)
            return false;
        for (int p = 0; p < channels; ++p)
            if (channelOfLevel(means[static_cast<size_t>(p)]) != p)
                return false;
        return true;
    }

    int filesIn(const juce::File& folder)
    {
        return folder.getNumberOfChildFiles(juce::File::findFiles);
    }

    juce::File fresh(const juce::File& parent, const juce::String& name)
    {
        auto f = parent.getChildFile(name);
        f.deleteRecursively();
        f.createDirectory();
        return f;
    }

    TapeRecorder::Naming naming()
    {
        TapeRecorder::Naming n;
        n.prefix = "Take";
        n.description = "Files test";
        return n;
    }

    // Feeds a tape `seconds` of the given block-maker and waits for its writer.
    void feed(TapeRecorder& t, int channels, double seconds, const std::function<float(int ch, std::int64_t i)>& value)
    {
        juce::AudioBuffer<float> block(channels, 512);
        std::int64_t at = 0;
        for (int b = 0; b < static_cast<int>(seconds * c_fs / 512); ++b)
        {
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < 512; ++i)
                    block.setSample(ch, i, value(ch, at + i));
            at += 512;
            t.process(block.getArrayOfReadPointers(), channels, 512);
            for (int wait = 0; wait < 200 && t.getBacklogSeconds() > 1.0; ++wait)
                juce::Thread::sleep(2);
        }
    }

    std::vector<TapeRecorder::FinishedTake> finished(TapeRecorder& t)
    {
        std::vector<TapeRecorder::FinishedTake> all;
        for (int wait = 0; wait < 100; ++wait)
        {
            for (auto& f : t.takeFinishedTakes())
                all.push_back(f);
            if (! all.empty())
                break;
            juce::Thread::sleep(10);
        }
        return all;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("SwooshbucklerFilesTest");
    folder.deleteRecursively();
    folder.createDirectory();

    //==========================================================================
    std::cout << "Every width, Channel Order, File Layout and file type" << std::endl;
    {
        constexpr int c_samples = 4800;
        const std::vector<TapeRecorder::Cue> cues { { 100, "One" }, { 2400, "Two" } };
        const TapeRecorder::FileType types[] = { TapeRecorder::FileType::Wav, TapeRecorder::FileType::Aiff, TapeRecorder::FileType::Flac };
        int cases = 0;
        for (auto type : types)
            for (auto w : c_widths)
                for (auto order : { room::FileOrder::ProTools, room::FileOrder::Smpte })
                    for (auto layout : { takefile::Layout::Interleaved, takefile::Layout::MultiMono })
                    {
                        const auto& l = room::layout(w);
                        const int n = l.numChannels;
                        const auto label = typeName(type) + " " + nameOf(w) + " " + room::fileOrderName(order) + " " + takefile::layoutName(layout);
                        const auto dir = fresh(folder, "case_" + juce::String(cases++));
                        TapeRecorder::Options o;
                        o.type = type;
                        o.order = order;
                        o.layout = layout;
                        const auto base = dir.getChildFile("Take_0001" + TapeRecorder::extensionOf(type));
                        const auto audio = levels(n, c_samples);
                        TapeRecorder::FileWriter writer;
                        juce::String error;
                        const bool multi = layout == takefile::Layout::MultiMono && w != W::Mono;

                        if (type == TapeRecorder::FileType::Flac && ! multi && n > 8)
                        {
                            const bool opened = writer.open(base, c_fs, n, o, naming(), error);
                            check(! opened && error.contains("at most 8 channels") && filesIn(dir) == 0,
                                  label + ": refused with a plain message, nothing written (\"" + error + "\")");
                            continue;
                        }

                        bool ok = writer.open(base, c_fs, n, o, naming(), error) && writer.write(audio, 0, c_samples) && writer.close(cues);
                        const auto files = writer.files();
                        // The files: one, or the whole set named <name>.<suffix>, and nothing else.
                        juce::StringArray want;
                        if (multi)
                            for (int ch = 0; ch < n; ++ch)
                                want.add(juce::String("Take_0001.") + room::info(l.speakers[static_cast<size_t>(ch)]).name + TapeRecorder::extensionOf(type));
                        else
                            want.add(base.getFileName());
                        juce::StringArray got;
                        for (const auto& f : files)
                            got.add(f.existsAsFile() ? f.getFileName() : "(missing)");
                        ok = ok && got == want && filesIn(dir) == want.size();

                        // Each file, read by this test: its header and where every channel's sound is.
                        const auto line = typedOrder(w, order);
                        const auto tokens = juce::StringArray::fromTokens(line, " ", "");
                        bool placed = true, header = true, named = true;
                        for (int fi = 0; fi < files.size() && ok; ++fi)
                        {
                            const auto parsed = parse(files[fi], type);
                            const int fileChannels = multi ? 1 : n;
                            placed = placed && parsed.ok && parsed.channels == fileChannels && parsed.frames == c_samples;
                            for (int i = 0; i < fileChannels && placed; ++i)
                            {
                                const int expect = multi ? fi : processingIndex(w, tokens[i]);
                                placed = channelOfLevel(parsed.means[static_cast<size_t>(i)]) == expect;
                            }
                            const juce::String expectLine = multi ? juce::String(room::info(l.speakers[static_cast<size_t>(fi)]).name)
                                                                  : n > 2 ? line : juce::String();
                            if (type == TapeRecorder::FileType::Wav)
                            {
                                if (fileChannels <= 2)
                                    header = header && ! parsed.extensible && parsed.tag == 1 && parsed.bits == 24;
                                else
                                {
                                    const std::uint32_t mask = (order == room::FileOrder::Smpte || w == W::Quad) ? typedMask(w) : 0u;
                                    header = header && parsed.extensible && parsed.mask == mask && parsed.subTag == 1 && parsed.bits == 24;
                                }
                                header = header && parsed.cues == 2;
                                named = named && (expectLine.isEmpty() ? ! parsed.ixml.contains("channelConfig")
                                                                       : parsed.ixml.contains("<channelConfig>" + expectLine + "</channelConfig>"));
                            }
                            else
                                named = named && takefile::readChannelLine(files[fi]) == expectLine;
                            if (type == TapeRecorder::FileType::Flac && fileChannels > 2)
                                header = header && takefile::flacComments(files[fi]).contains("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x"
                                                                                               + juce::String::toHexString(static_cast<int>((order == room::FileOrder::Smpte || w == W::Quad) ? typedMask(w) : 0u)).toUpperCase());
                            // The markers, in each file.
                            if (type == TapeRecorder::FileType::Flac)
                            {
                                int markers = 0;
                                for (const auto& c : takefile::flacComments(files[fi]))
                                    markers += c.startsWith("MARKER=") ? 1 : 0;
                                header = header && markers == 2;
                            }
                            else
                            {
                                const auto read = TapeRecorder::readCues(files[fi]);
                                header = header && read.size() == 2 && read[0].frame == 100 && read[0].label == "One" && read[1].frame == 2400 && read[1].label == "Two";
                            }
                        }

                        // Read back as a take, from its last file: every channel where the room had it.
                        int channels = 0;
                        const auto means = files.isEmpty() ? std::vector<double> {} : readMeans(files[files.size() - 1], channels);
                        const auto found = files.isEmpty() ? takefile::Found {} : takefile::find(files[files.size() - 1]);
                        const bool back = channels == n && inProcessingOrder(means, n) && found.files.size() == files.size()
                                       && found.format.width == w && found.format.layout == (multi ? takefile::Layout::MultiMono : takefile::Layout::Interleaved)
                                       && found.described == (multi || n > 2)
                                       && (multi || n <= 4 || found.format.order == order);
                        check(ok && placed && header && named && back,
                              label + ": " + juce::String(files.size()) + (files.size() == 1 ? " file" : " files") + (ok ? "" : " (files wrong)")
                                  + (placed ? "" : " (channels misplaced)") + (header ? "" : " (header or markers wrong)")
                                  + (named ? "" : " (channel names wrong)") + (back ? "" : " (read back wrong)"));
                    }
    }

    //==========================================================================
    std::cout << "All or nothing" << std::endl;
    {
        const auto dir = fresh(folder, "nothing");
        TapeRecorder::Options o;
        o.layout = takefile::Layout::MultiMono;
        // One file of the set can't be made (a folder has its name): none of the others are left.
        dir.getChildFile("Take_0001.Ls.wav").getChildFile("in the way").create();
        TapeRecorder::FileWriter writer;
        juce::String error;
        const bool opened = writer.open(dir.getChildFile("Take_0001.wav"), c_fs, 6, o, naming(), error);
        check(! opened && filesIn(dir) == 0, "a 5.1 set that can't be written whole leaves none of its files");
        dir.getChildFile("Take_0001.Ls.wav").deleteRecursively();

        // Abandoned half way (a cancelled render): nothing left.
        TapeRecorder::FileWriter cancelled;
        cancelled.open(dir.getChildFile("Take_0002.wav"), c_fs, 12, o, naming(), error);
        cancelled.write(levels(12, 1000), 0, 1000);
        const int whileOpen = filesIn(dir);
        cancelled.abandon();
        check(whileOpen == 12 && filesIn(dir) == 0, "an abandoned 7.1.4 set leaves no file (12 while it was written)");

        // writeFile of a count no width has: refused.
        juce::String why;
        check(! TapeRecorder::writeFile(dir.getChildFile("Three.wav"), levels(3, 100), 0, 100, c_fs, {}, naming(), why) && filesIn(dir) == 0,
              "three channels are refused: \"" + why + "\"");

        // writeFile hands back its set.
        juce::Array<juce::File> written;
        check(TapeRecorder::writeFile(dir.getChildFile("Quad.wav"), levels(4, 480), 0, 480, c_fs, o, naming(), why, &written)
                  && written.size() == 4 && written[2].getFileName() == "Quad.Ls.wav"
                  && TapeRecorder::filesFor(dir.getChildFile("Quad.wav"), 4, o) == written,
              "writeFile writes a Quad set and says which files: .L .R .Ls .Rs");
    }

    //==========================================================================
    std::cout << "The tape" << std::endl;
    {
        for (auto w : { W::Surround714, W::Surround51 })
            for (auto layout : { takefile::Layout::Interleaved, takefile::Layout::MultiMono })
            {
                const int n = room::layout(w).numChannels;
                const auto dir = fresh(folder, "tape_" + juce::String(n) + "_" + juce::String(static_cast<int>(layout)));
                TapeRecorder t;
                t.setNaming(naming());
                t.prepare(c_fs, n);
                TapeRecorder::Options o;
                o.order = room::FileOrder::Smpte;
                o.layout = layout;
                juce::String error;
                const bool started = t.start(dir, o, error);
                t.addMarker(1000, TapeRecorder::MarkHit);
                feed(t, n, 1.0, [](int ch, std::int64_t) { return levelOf(ch); });
                t.stop();
                const auto takes = finished(t);
                const bool one = takes.size() == 1 && takes[0].files.size() == (layout == takefile::Layout::MultiMono ? n : 1)
                              && takes[0].format.width == w && takes[0].format.layout == layout && takes[0].file == takes[0].files[0];
                int channels = 0;
                const auto means = one ? readMeans(takes[0].files[n > 1 && layout == takefile::Layout::MultiMono ? 3 : 0], channels) : std::vector<double> {};
                const auto cues = one ? TapeRecorder::readCues(takes[0].files[0]) : std::vector<TapeRecorder::Cue> {};
                const auto parsed = one ? parseWav(takes[0].files[0]) : Parsed {};
                const bool header = layout == takefile::Layout::MultiMono ? parsed.channels == 1 : parsed.mask == typedMask(w);
                check(started && one && channels == n && inProcessingOrder(means, n) && cues.size() == 1 && cues[0].label == "Hit"
                          && header && filesIn(dir) == (layout == takefile::Layout::MultiMono ? n : 1),
                      "the tape at " + nameOf(w) + ", " + takefile::layoutName(layout) + ": one take, every channel in its place, its marker" + (header ? "" : " (header wrong)"));
            }

        // One File per Whoosh: a whoosh too short to keep leaves none of its set.
        {
            const auto dir = fresh(folder, "tape_short");
            TapeRecorder t;
            t.setNaming(naming());
            t.prepare(c_fs, 12);
            t.setSignalGate(true);
            TapeRecorder::Options o;
            o.layout = takefile::Layout::MultiMono;
            o.perWhoosh = true;
            juce::String error;
            t.start(dir, o, error);
            // 0.05 s of sound, then quiet long enough for the gate to close.
            feed(t, 12, 2.5, [](int ch, std::int64_t i) { return i < 2400 ? levelOf(ch) : 0.0f; });
            t.stop();
            juce::Thread::sleep(50);
            check(t.takeFinishedTakes().empty() && filesIn(dir) == 0, "a 7.1.4 whoosh too short to keep leaves none of its 12 files");
        }

        // Names by date and time: two Multi-Mono sets in the same second get names of their own (a set has no
        // file of the plain name, and opening a take deletes what is in its way: the first set would be lost).
        {
            const auto dir = fresh(folder, "tape_dated");
            TapeRecorder t;
            auto dated = naming();
            dated.style = TapeRecorder::NameStyle::DateTime;
            t.setNaming(dated);
            TapeRecorder::Options o;
            o.layout = takefile::Layout::MultiMono;
            bool distinct = true;
            juce::StringArray names;
            for (int i = 0; i < 4; ++i) // four at once: at least two of them share a second
            {
                const auto name = t.nextFile(dir, "_retro", ".wav");
                distinct = distinct && ! names.contains(name.getFileName());
                names.add(name.getFileName());
                juce::String why;
                distinct = TapeRecorder::writeFile(name, levels(6, 480), 0, 480, c_fs, o, dated, why) && distinct;
            }
            check(distinct && filesIn(dir) == 24, "four 5.1 sets named by the clock in one go: four names, 24 files (" + juce::String(filesIn(dir)) + ")");
        }

        // Mono of a 7.1.4 take: the room's fold to Mono, lined up with the take (the limiter's delay taken out),
        // and held under the Ceiling.
        {
            const auto dir = fresh(folder, "tape_mono");
            TapeRecorder t;
            t.setNaming(naming());
            t.prepare(c_fs, 12);
            TapeRecorder::Options o;
            o.mono = true;
            o.bitDepth = 32;
            o.foldCeilingDb = -1.0f;
            juce::String error;
            t.start(dir, o, error);
            // Quiet, from sample 4096 on (the limiter leaves it alone); then every channel at full scale.
            feed(t, 12, 2.0, [](int ch, std::int64_t i) { return i < 4096 ? 0.0f : i < 48000 ? 0.01f * static_cast<float>(ch + 1) : 1.0f; });
            t.stop();
            const auto takes = finished(t);
            const auto parsed = takes.empty() ? Parsed {} : parseWav(takes[0].file);
            juce::AudioBuffer<float> mono;
            if (! takes.empty())
                if (const auto r = takefile::openReader(takes[0].file))
                {
                    mono.setSize(1, static_cast<int>(r->lengthInSamples));
                    r->read(mono.getArrayOfWritePointers(), 1, 0, mono.getNumSamples());
                }
            const auto& fold = room::fold(W::Surround714, W::Mono);
            float expect = 0.0f;
            for (int ch = 0; ch < 12; ++ch)
                expect += fold.gain[0][static_cast<size_t>(ch)] * 0.01f * static_cast<float>(ch + 1);
            const bool lined = mono.getNumSamples() == 2 * 48000 - (2 * 48000) % 512 && std::abs(mono.getSample(0, 4095)) < 1.0e-6f
                            && std::abs(mono.getSample(0, 4096) - expect) < 1.0e-5f && std::abs(mono.getSample(0, 40000) - expect) < 1.0e-5f;
            const float peak = mono.getNumSamples() > 0 ? mono.getMagnitude(0, 48000, mono.getNumSamples() - 48000) : 9.0f;
            std::cout << "         fold " << expect << ", file " << (mono.getNumSamples() > 4096 ? mono.getSample(0, 4096) : 0.0f)
                      << "; full scale on all twelve folds to a peak of " << juce::Decibels::gainToDecibels(peak) << " dBFS" << std::endl;
            check(parsed.channels == 1 && lined, "Mono of 7.1.4 is the room's fold, sample for sample where it starts");
            check(peak <= juce::Decibels::decibelsToGain(-1.0f) * 1.001f && peak > 0.8f, "and twelve full-scale channels folded stay under the Ceiling");
        }
    }

    //==========================================================================
    std::cout << "The saved last minutes" << std::endl;
    {
        TapeRecorder t;
        t.setRetroSeconds(300);
        t.prepare(192000.0, 12);
        const double wide = t.getRetroCapacitySeconds();
        t.prepare(192000.0, 2);
        const double stereo = t.getRetroCapacitySeconds();
        std::cout << "         five minutes asked: 7.1.4 at 192 kHz holds " << wide << " s, Stereo " << stereo << " s, 7.1.4 at 48 kHz "
                  << TapeRecorder::retroCapacityFor(48000.0, 12, 300) << " s" << std::endl;
        const double expectWide = static_cast<double>(TapeRecorder::c_maxRetroBytes / (12 * 4)) / 192000.0;
        check(std::abs(wide - expectWide) < 0.01 && wide > 58.0 && wide < 58.5, "7.1.4 at 192 kHz: capped at 512 MB, 58 s, and it says so");
        check(std::abs(stereo - 300.0) < 0.01 && std::abs(TapeRecorder::retroCapacityFor(48000.0, 12, 60) - 60.0) < 0.01,
              "Stereo keeps its five minutes; a minute of 7.1.4 at 48 kHz is a minute");

        const auto dir = fresh(folder, "retro");
        t.setNaming(naming());
        t.setRetroSeconds(60);
        t.prepare(c_fs, 12);
        feed(t, 12, 2.0, [](int ch, std::int64_t) { return levelOf(ch); });
        TapeRecorder::Options o;
        o.layout = takefile::Layout::MultiMono;
        juce::File written;
        juce::String error;
        const bool saved = t.saveRetro(dir, o, written, error);
        int channels = 0;
        const auto means = saved ? readMeans(written, channels) : std::vector<double> {};
        check(saved && filesIn(dir) == 12 && written.getFileName() == "Take_0001_retro.L.wav" && inProcessingOrder(means, 12),
              "saved at 7.1.4 as a Multi-Mono set: twelve files, every channel in its place " + error);
    }

    //==========================================================================
    std::cout << "RF64" << std::endl;
    {
        // An RF64 header as JUCE writes one past 4 GB: ds64, then an extensible format chunk (here with the
        // five-front 7.1 mask JUCE gives eight channels), then data.
        const auto file = folder.getChildFile("rf64.wav");
        juce::MemoryOutputStream m;
        m.write("RF64", 4); m.writeInt(-1); m.write("WAVE", 4);
        m.write("ds64", 4); m.writeInt(28); m.writeInt64(0); m.writeInt64(8 * 3 * 10); m.writeRepeatedByte(0, 12);
        m.write("fmt ", 4); m.writeInt(40);
        m.writeShort(static_cast<short>(0xfffe)); m.writeShort(8); m.writeInt(48000); m.writeInt(48000 * 24); m.writeShort(24); m.writeShort(24);
        m.writeShort(22); m.writeShort(24); m.writeInt(0xff);
        m.writeInt(1); m.writeShort(0); m.writeShort(0x10);
        const unsigned char tail[8] = { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
        m.write(tail, 8);
        m.write("data", 4); m.writeInt(-1); m.writeRepeatedByte(0, 8 * 3 * 10);
        file.replaceWithData(m.getData(), m.getDataSize());
        const bool set = takefile::setWavChannelMask(file, 0x63F);
        const auto parsed = parseWav(file);
        check(set && parsed.rf64 && parsed.extensible && parsed.mask == 0x63F && parsed.channels == 8 && parsed.frames == 10,
              "an RF64 file's mask is set in place (film 7.1, 0x63F), the rest untouched");
    }

    //==========================================================================
    std::cout << "Audition" << std::endl;
    {
        const auto dir = fresh(folder, "audition");
        juce::String error;
        TapeRecorder::Options smpte;
        smpte.order = room::FileOrder::Smpte;
        TapeRecorder::writeFile(dir.getChildFile("Wide.wav"), levels(12, 24000), 0, 24000, c_fs, smpte, naming(), error);
        TapeRecorder::Options multi;
        multi.layout = takefile::Layout::MultiMono;
        TapeRecorder::writeFile(dir.getChildFile("Set.wav"), levels(12, 24000), 0, 24000, c_fs, multi, naming(), error);
        TapeRecorder::writeFile(dir.getChildFile("Five.wav"), levels(6, 24000), 0, 24000, c_fs, {}, naming(), error);

        auto play = [&](const juce::File& file, W out, juce::AudioBuffer<float>& into)
        {
            TakeAudition a;
            a.prepare(c_fs, out);
            juce::String why;
            if (! a.play(file, why))
                return false;
            into.setSize(room::layout(out).numChannels, 4096);
            into.clear();
            a.topUp();
            a.process(into, 0, 4096);
            return true;
        };
        auto means = [](const juce::AudioBuffer<float>& b)
        {
            std::vector<double> m;
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                m.push_back(b.getSample(ch, 2000)); // past the fade in
            return m;
        };
        juce::AudioBuffer<float> out;
        check(play(dir.getChildFile("Wide.wav"), W::Surround714, out) && inProcessingOrder(means(out), 12),
              "a 7.1.4 file in the SMPTE / WAV order plays each channel to its own speaker");
        check(play(dir.getChildFile("Set.Rtr.wav"), W::Surround714, out) && inProcessingOrder(means(out), 12),
              "a 7.1.4 Multi-Mono set plays as one, from whichever of its files");
        // 5.1 on 7.1: the surrounds go to the sides and rears at -3 dB each, the rest as they are.
        const bool placed = play(dir.getChildFile("Five.wav"), W::Surround71, out);
        const auto& l71 = room::layout(W::Surround71);
        const float ls = levelOf(4) * room::c_foldStep;
        check(placed && std::abs(out.getSample(l71.indexOf(room::Speaker::L), 2000) - levelOf(0)) < 1.0e-4f
                  && std::abs(out.getSample(l71.indexOf(room::Speaker::Lfe), 2000) - levelOf(3)) < 1.0e-4f
                  && std::abs(out.getSample(l71.indexOf(room::Speaker::Lss), 2000) - ls) < 1.0e-4f
                  && std::abs(out.getSample(l71.indexOf(room::Speaker::Lsr), 2000) - ls) < 1.0e-4f,
              "5.1 on a 7.1 output: each speaker to its own, Ls to the side and rear at -3 dB");
        // 7.1.4 on Stereo: the room's fold.
        const bool folded = play(dir.getChildFile("Wide.wav"), W::Stereo, out);
        const auto& f = room::fold(W::Surround714, W::Stereo);
        float l = 0.0f, r = 0.0f;
        for (int ch = 0; ch < 12; ++ch)
        {
            l += f.gain[0][static_cast<size_t>(ch)] * levelOf(ch);
            r += f.gain[1][static_cast<size_t>(ch)] * levelOf(ch);
        }
        // These levels fold to more than full scale (L 0.96, R 1.12): the fold is held under the Ceiling, both
        // channels by the one gain (see the Output test for the limiter itself).
        const float held = juce::jmin(1.0f, juce::Decibels::decibelsToGain(-0.3f) / juce::jmax(l, r));
        check(folded && r > 1.0f && std::abs(out.getSample(0, 2000) - held * l) < 1.0e-3f && std::abs(out.getSample(1, 2000) - held * r) < 1.0e-3f,
              "7.1.4 on a stereo output plays the room's fold, held under the Ceiling");
        const auto monoOnStereo = TakeAudition::mixFor(1, W::Stereo);
        const auto stereoOnMono = TakeAudition::mixFor(2, W::Mono);
        check(monoOnStereo.gain[0][0] == 1.0f && monoOnStereo.gain[1][0] == 1.0f && stereoOnMono.gain[0][0] == 0.5f && stereoOnMono.gain[0][1] == 0.5f,
              "Mono plays from L and R, and Stereo on a mono output is the average, as before");
    }

    //==========================================================================
    std::cout << "The rig" << std::endl;
    {
        using Proc = SwooshbucklerAudioProcessor;
        const auto tapeFolder = fresh(folder, "rig");
        juce::PropertiesFile::Options options;
        options.applicationName = "SwooshbucklerFilesTest";
        options.filenameSuffix = ".settings";
        const auto settingsFile = folder.getChildFile("test.settings");
        settingsFile.deleteFile();
        {
            juce::PropertiesFile settings(settingsFile, options);
            settings.setValue("recordFolder", tapeFolder.getFullPathName());
            Proc::useSettings(&settings);
            std::unique_ptr<juce::AudioProcessor> base { createPluginFilter() };
            auto& p = *dynamic_cast<Proc*>(base.get());
            check(p.getChannelOrder() == room::FileOrder::ProTools && p.getFileLayout() == takefile::Layout::Interleaved
                      && p.tapeOptions().order == room::FileOrder::ProTools,
                  "Channel Order is Pro Tools (Film) and File Layout Interleaved unless chosen otherwise");
            p.setChannelOrder(room::FileOrder::Smpte);
            p.setFileLayout(takefile::Layout::MultiMono);
            check(p.tapeOptions().order == room::FileOrder::Smpte && p.tapeOptions().layout == takefile::Layout::MultiMono,
                  "the tape and the saved last minutes write what is chosen");

            // A Multi-Mono set is one take, whichever of its files is handed in, and deleting it deletes them all.
            juce::String error;
            TapeRecorder::Options multi;
            multi.layout = takefile::Layout::MultiMono;
            TapeRecorder::writeFile(tapeFolder.getChildFile("Set.wav"), levels(6, 4800), 0, 4800, c_fs, multi, naming(), error);
            p.registerTake(tapeFolder.getChildFile("Set.Ls.wav"), "Tape");
            p.registerTake(tapeFolder.getChildFile("Set.LFE.wav"), "Tape");
            const auto takes = p.getTakes();
            check(takes.size() == 1 && takes[0].files.size() == 6 && takes[0].file.getFileName() == "Set.L.wav"
                      && takes[0].format.layout == takefile::Layout::MultiMono && takes[0].format.width == W::Surround51
                      && std::abs(takes[0].seconds - 0.1) < 1.0e-6 && takefile::displayName(takes[0].files) == "Set (6 Mono Files)",
                  "a 5.1 Multi-Mono set is one take of 0.1 s, shown as \"Set (6 Mono Files)\"");
            juce::String why;
            const bool plays = ! takes.empty() && p.auditionTake(takes[0].id, why) && p.getAuditionTake() == takes[0].id;
            p.stopAudition();
            check(plays, "it plays " + why);
            check(! takes.empty() && p.deleteTake(takes[0].id, false) && p.getTakes().empty() && filesIn(tapeFolder) == 0,
                  "deleting it deletes all six files");
            Proc::useSettings(nullptr);
        }
        juce::PropertiesFile again(settingsFile, options);
        check(again.getIntValue("channelOrder", 0) == 1 && again.getIntValue("fileLayout", 0) == 1, "and the choices are kept in the settings");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

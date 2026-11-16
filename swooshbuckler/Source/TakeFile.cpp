#include "TakeFile.h"

#include <algorithm>
#include <cstring>

namespace takefile
{
namespace
{
    constexpr room::Width c_widestFirst[] = { room::Width::Surround714, room::Width::Surround712, room::Width::Surround71,
                                              room::Width::Surround51, room::Width::Quad, room::Width::Stereo };

    std::optional<room::Speaker> speakerNamed(const juce::String& name)
    {
        for (int s = 0; s < room::c_numSpeakers; ++s)
            if (name == room::info(static_cast<room::Speaker>(s)).name)
                return static_cast<room::Speaker>(s);
        return std::nullopt;
    }

    std::uint32_t readU32(const unsigned char* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
    std::uint16_t readU16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
    void putU32(unsigned char* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xff); }
    void putU16(unsigned char* p, std::uint16_t v) { p[0] = static_cast<unsigned char>(v & 0xff); p[1] = static_cast<unsigned char>(v >> 8); }

    /** Where the format chunk of a WAV file is, and the chunk just before it. */
    struct FmtPlace
    {
        bool ok = false;
        bool rf64 = false;
        juce::int64 fmtStart = 0;        // its "fmt " tag
        std::uint32_t fmtSize = 0;
        juce::int64 prevStart = -1;      // the chunk before it (its tag), -1 when none
        char prevId[4] {};
        std::uint32_t prevSize = 0;
        unsigned char fmt[40] {};        // the first (up to) 40 bytes of its body
    };

    FmtPlace findFmt(const juce::File& file)
    {
        FmtPlace place;
        juce::FileInputStream in(file);
        if (! in.openedOk())
            return place;
        char riff[4] {}, wave[4] {};
        if (in.read(riff, 4) != 4)
            return place;
        place.rf64 = std::memcmp(riff, "RF64", 4) == 0;
        if (! place.rf64 && std::memcmp(riff, "RIFF", 4) != 0)
            return place;
        in.readInt();
        if (in.read(wave, 4) != 4 || std::memcmp(wave, "WAVE", 4) != 0)
            return place;
        const auto length = in.getTotalLength();
        for (int guard = 0; guard < 64 && in.getPosition() + 8 <= length; ++guard)
        {
            const auto start = in.getPosition();
            char id[4] {};
            in.read(id, 4);
            const auto size = static_cast<std::uint32_t>(in.readInt());
            if (std::memcmp(id, "fmt ", 4) == 0)
            {
                place.fmtStart = start;
                place.fmtSize = size;
                const int want = static_cast<int>(std::min<std::uint32_t>(size, 40));
                if (in.read(place.fmt, want) != want)
                    return place;
                place.ok = true;
                return place;
            }
            if (std::memcmp(id, "data", 4) == 0)
                return place; // the format comes first in every file this reads
            place.prevStart = start;
            std::memcpy(place.prevId, id, 4);
            place.prevSize = size;
            if (! in.setPosition(start + 8 + static_cast<juce::int64>(size) + (size & 1)))
                return place;
        }
        return place;
    }

    std::unique_ptr<juce::AudioFormatReader> readerFor(const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        return std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(file));
    }

    struct FlacBlock
    {
        int type = 0;
        juce::MemoryBlock data;
    };

    /** Reads a FLAC file's metadata blocks; leaves the stream at the first audio frame. */
    bool readFlacBlocks(juce::InputStream& in, std::vector<FlacBlock>& blocks)
    {
        char tag[4] {};
        if (in.read(tag, 4) != 4 || std::memcmp(tag, "fLaC", 4) != 0)
            return false;
        for (bool last = false; ! last;)
        {
            unsigned char head[4] {};
            if (in.read(head, 4) != 4)
                return false;
            last = (head[0] & 0x80) != 0;
            const auto length = static_cast<size_t>((head[1] << 16) | (head[2] << 8) | head[3]);
            FlacBlock block;
            block.type = head[0] & 0x7f;
            if (in.readIntoMemoryBlock(block.data, static_cast<juce::pointer_sized_int>(length)) != length)
                return false;
            blocks.push_back(std::move(block));
        }
        return true;
    }

    /**
     * A take in processing order. An interleaved file has its channels put back where the rig had them; a
     * Multi-Mono set is read file by file. Reports itself as float data, so read() hands it float pointers.
     */
    class TakeReader : public juce::AudioFormatReader
    {
    public:
        TakeReader(std::vector<std::unique_ptr<juce::AudioFormatReader>> readers, const Found& take)
            : AudioFormatReader(nullptr, "Take")
            , m_readers(std::move(readers))
            , m_multi(take.format.layout == Layout::MultiMono)
            , m_processingOf(take.processingOf)
        {
            const auto& first = *m_readers.front();
            sampleRate = first.sampleRate;
            bitsPerSample = first.bitsPerSample;
            usesFloatingPointData = true;
            metadataValues = first.metadataValues;
            numChannels = m_multi ? static_cast<unsigned int>(m_readers.size()) : first.numChannels;
            lengthInSamples = first.lengthInSamples;
            for (const auto& r : m_readers)
                lengthInSamples = std::min(lengthInSamples, r->lengthInSamples);
        }

        bool readSamples(int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                         juce::int64 startSampleInFile, int numSamples) override
        {
            float* const* dest = reinterpret_cast<float* const*>(destChannels);
            for (int ch = 0; ch < numDestChannels; ++ch)
                if (dest[ch] != nullptr)
                    juce::FloatVectorOperations::clear(dest[ch] + startOffsetInDestBuffer, numSamples);
            if (numSamples <= 0)
                return true;
            bool ok = true;
            if (m_multi)
            {
                for (int ch = 0; ch < std::min(numDestChannels, static_cast<int>(m_readers.size())); ++ch)
                    if (dest[ch] != nullptr)
                    {
                        float* const into[1] = { dest[ch] + startOffsetInDestBuffer };
                        ok = m_readers[static_cast<size_t>(ch)]->read(into, 1, startSampleInFile, numSamples) && ok;
                    }
                return ok;
            }
            auto& r = *m_readers.front();
            const int fileChannels = static_cast<int>(r.numChannels);
            m_temp.setSize(fileChannels, numSamples, false, false, true);
            ok = r.read(m_temp.getArrayOfWritePointers(), fileChannels, startSampleInFile, numSamples);
            for (int i = 0; i < fileChannels; ++i)
            {
                const int to = i < room::c_maxChannels ? m_processingOf[static_cast<size_t>(i)] : i;
                if (to >= 0 && to < numDestChannels && dest[to] != nullptr)
                    juce::FloatVectorOperations::copy(dest[to] + startOffsetInDestBuffer, m_temp.getReadPointer(i), numSamples);
            }
            return ok;
        }

    private:
        std::vector<std::unique_ptr<juce::AudioFormatReader>> m_readers;
        bool m_multi = false;
        std::array<int, room::c_maxChannels> m_processingOf {};
        juce::AudioBuffer<float> m_temp;
    };
}

namespace
{
    std::optional<room::Width> widthNamed(const juce::String& name)
    {
        for (int w = 0; w < room::c_numWidths; ++w)
            if (name == room::layout(static_cast<room::Width>(w)).name)
                return static_cast<room::Width>(w);
        return std::nullopt;
    }

    /** A speaker of `width` from the word Pro Tools uses for it in an iXML FUNCTION (ixmlFunction), or from one of
        the names here ("Lss"). */
    std::optional<room::Speaker> speakerOfWord(room::Width width, const juce::String& word)
    {
        using S = room::Speaker;
        const auto& l = room::layout(width);
        auto has = [&l](S s) { return l.indexOf(s) >= 0; };
        if (word == "M")   return S::C;
        if (word == "Ls")  return has(S::Lsr) ? S::Lsr : S::Ls;
        if (word == "Rs")  return has(S::Rsr) ? S::Rsr : S::Rs;
        if (word == "Sl")  return S::Lss;
        if (word == "Sr")  return S::Rss;
        if (word == "Tl")  return has(S::Lts) ? S::Lts : S::Ltf;
        if (word == "Tr")  return has(S::Rts) ? S::Rts : S::Rtf;
        if (word == "Trl") return S::Ltr;
        if (word == "Trr") return S::Rtr;
        return speakerNamed(word);
    }

    /** The speakers a WAV speaker mask names, in mask order, as speakers of `width`; empty when it names one the
        width does not have. A 5.1 marked with the side pair (0x60F) is read as the same 5.1. */
    juce::Array<room::Speaker> speakersOfMask(room::Width width, std::uint32_t mask)
    {
        using S = room::Speaker;
        const auto& l = room::layout(width);
        juce::Array<room::Speaker> out;
        for (int bit = 0; bit < 32; ++bit)
        {
            const std::uint32_t b = 1u << bit;
            if ((mask & b) == 0)
                continue;
            std::optional<room::Speaker> speaker;
            for (int ch = 0; ch < l.numChannels && ! speaker.has_value(); ++ch)
                if (const auto s = l.speakers[static_cast<size_t>(ch)]; room::info(s).wavBit == b && ! out.contains(s))
                    speaker = s;
            if (! speaker.has_value() && (b == 0x200 || b == 0x400))
                if (const auto s = b == 0x200 ? S::Ls : S::Rs; l.indexOf(s) >= 0 && ! out.contains(s))
                    speaker = s;
            if (! speaker.has_value())
                return {};
            out.add(*speaker);
        }
        return out;
    }

    /** The speakers an iXML TRACK_LIST names, by INTERLEAVE_INDEX: from each FUNCTION ("Sl-7.1.4"), or failing
        that the end of its NAME ("Whoosh-Lss"). Empty unless every channel is named once. */
    juce::Array<room::Speaker> speakersOfIxml(const juce::String& text, int numChannels)
    {
        if (text.isEmpty() || numChannels < 1 || numChannels > room::c_maxChannels)
            return {};
        const auto xml = juce::parseXML(text);
        const auto* list = xml != nullptr ? xml->getChildByName("TRACK_LIST") : nullptr;
        if (list == nullptr)
            return {};
        const auto context = widthForChannels(numChannels).value_or(room::Width::Stereo);
        std::array<std::optional<room::Speaker>, room::c_maxChannels> at {};
        int tracks = 0;
        for (const auto* track : list->getChildWithTagNameIterator("TRACK"))
        {
            ++tracks;
            const int index = track->getChildElementAllSubText("INTERLEAVE_INDEX", {}).trim().getIntValue() - 1;
            if (index < 0 || index >= numChannels || at[static_cast<size_t>(index)].has_value())
                return {};
            const auto function = track->getChildElementAllSubText("FUNCTION", {}).trim();
            auto width = context;
            if (const auto named = widthNamed(function.fromFirstOccurrenceOf("-", false, false)))
                width = *named;
            auto speaker = speakerOfWord(width, function.upToFirstOccurrenceOf("-", false, false));
            if (! speaker.has_value())
            {
                const auto name = track->getChildElementAllSubText("NAME", {}).trim();
                speaker = speakerNamed(name.fromLastOccurrenceOf("-", false, false));
                if (! speaker.has_value())
                    speaker = speakerNamed(name.fromLastOccurrenceOf(".", false, false));
            }
            if (! speaker.has_value())
                return {};
            at[static_cast<size_t>(index)] = speaker;
        }
        if (tracks != numChannels)
            return {};
        juce::Array<room::Speaker> out;
        for (int i = 0; i < numChannels; ++i)
            out.add(*at[static_cast<size_t>(i)]);
        return out;
    }

    juce::Array<room::Speaker> speakersOfNames(const juce::String& line)
    {
        auto tokens = juce::StringArray::fromTokens(line, " ", "");
        tokens.removeEmptyStrings();
        juce::Array<room::Speaker> out;
        for (const auto& t : tokens)
        {
            const auto speaker = speakerNamed(t);
            if (! speaker.has_value())
                return {};
            out.add(*speaker);
        }
        return out;
    }

    // Core Audio's channel labels (CoreAudioBaseTypes.h): the side pair of 7.1 is its Ls Rs, the rear pair Rls Rrs.
    std::uint32_t coreAudioLabel(room::Speaker speaker) noexcept
    {
        using S = room::Speaker;
        switch (speaker)
        {
            case S::L:   return 1;
            case S::R:   return 2;
            case S::C:   return 3;
            case S::Lfe: return 4;
            case S::Ls:  case S::Lss: return 5;
            case S::Rs:  case S::Rss: return 6;
            case S::Lsr: return 33;
            case S::Rsr: return 34;
            case S::Ltf: return 13;
            case S::Rtf: return 15;
            case S::Ltr: return 52;
            case S::Rtr: return 54;
            case S::Lts: return 49;
            case S::Rts: return 51;
            default:     return 0;
        }
    }

    std::optional<room::Speaker> speakerOfCoreAudioLabel(room::Width width, std::uint32_t label)
    {
        using S = room::Speaker;
        const bool sides = room::layout(width).indexOf(S::Lss) >= 0;
        switch (label)
        {
            case 1:  return S::L;
            case 2:  return S::R;
            case 3:  return S::C;
            case 4:  return S::Lfe;
            case 5:  return sides ? S::Lss : S::Ls;
            case 6:  return sides ? S::Rss : S::Rs;
            case 10: return S::Lss;
            case 11: return S::Rss;
            case 33: return S::Lsr;
            case 34: return S::Rsr;
            case 13: return S::Ltf;
            case 15: return S::Rtf;
            case 16: case 52: return S::Ltr;
            case 18: case 54: return S::Rtr;
            case 49: return S::Lts;
            case 51: return S::Rts;
            default: return std::nullopt;
        }
    }

    /** What a file says each of its channels is, in file order: a WAV file's mask, else its iXML track list; an
        AIFF file's CHAN chunk, else its channel comment; a FLAC file's mask comment, else its CHANNELS comment.
        Empty when it says nothing (or not one speaker per channel). */
    juce::Array<room::Speaker> speakersSaid(const juce::File& file, int numChannels)
    {
        const auto width = widthForChannels(numChannels).value_or(room::Width::Stereo);
        auto fits = [numChannels](const juce::Array<room::Speaker>& s) { return s.size() == numChannels; };
        if (file.hasFileExtension("wav"))
        {
            if (const auto f = readWavFormat(file); f.extensible && f.mask != 0)
                if (auto s = speakersOfMask(width, f.mask); fits(s))
                    return s;
            if (auto s = speakersOfIxml(readIxml(file), numChannels); fits(s))
                return s;
            return {};
        }
        if (file.hasFileExtension("flac"))
        {
            const auto comments = flacComments(file);
            for (const auto& c : comments)
                if (c.startsWithIgnoreCase("WAVEFORMATEXTENSIBLE_CHANNEL_MASK="))
                    if (const auto mask = static_cast<std::uint32_t>(c.fromFirstOccurrenceOf("=", false, false).trim().getHexValue32()); mask != 0)
                        if (auto s = speakersOfMask(width, mask); fits(s))
                            return s;
            for (const auto& c : comments)
                if (c.startsWithIgnoreCase("CHANNELS="))
                    if (auto s = speakersOfNames(c.fromFirstOccurrenceOf("=", false, false)); fits(s))
                        return s;
            return {};
        }
        // AIFF: the channel comment written here names each speaker exactly (a lone channel of a set too); a CHAN
        // chunk, from here or another program, says it in Core Audio's words.
        if (const auto reader = readerFor(file))
        {
            const auto& meta = reader->metadataValues;
            const int notes = meta["NumCueNotes"].getIntValue();
            for (int i = 0; i < notes; ++i)
            {
                const auto text = meta["CueNote" + juce::String(i) + "Text"];
                if (text.startsWith("Channels: "))
                    if (auto s = speakersOfNames(text.fromFirstOccurrenceOf("Channels: ", false, false)); fits(s))
                        return s;
            }
        }
        if (auto s = readAiffChannelLayout(file); fits(s))
            return s;
        return {};
    }

    juce::uint32 readBigU32(juce::InputStream& in)
    {
        return static_cast<juce::uint32>(in.readIntBigEndian());
    }

}

const char* layoutName(Layout layout) noexcept
{
    return layout == Layout::MultiMono ? "Multi-Mono" : "Interleaved";
}

std::optional<room::Width> widthForChannels(int numChannels) noexcept
{
    for (int w = 0; w < room::c_numWidths; ++w)
        if (room::layout(static_cast<room::Width>(w)).numChannels == numChannels)
            return static_cast<room::Width>(w);
    return std::nullopt;
}

juce::String channelLine(room::Width width, room::FileOrder order)
{
    return room::orderLabel(width, order);
}

juce::String orderLine(room::Width width, room::FileOrder order, Layout layout, Container container)
{
    const juce::String name(room::layout(width).name);
    if (width == room::Width::Mono)
        return "Mono: one channel in one file.";
    if (layout == Layout::MultiMono)
    {
        // A Multi-Mono set has no order of its own: its files are listed the way Pro Tools lists a track's channels.
        const int n = room::layout(width).numChannels;
        const auto track = room::trackChannels(width);
        juce::StringArray suffixes;
        for (int i = 0; i < n; ++i)
            suffixes.add(room::multiMonoSuffix(width, track[static_cast<size_t>(i)]));
        return name + " as " + juce::String(n) + " files ending " + suffixes.joinIntoString(" ") + ".";
    }
    // FLAC stops at eight channels: there is no order to spell out for a file that can't be made.
    if (container == Container::Flac && room::layout(width).numChannels > c_maxFlacChannels)
        return name + " is more channels than one FLAC file holds.";
    const auto fileOrder = orderFor(container, order);
    juce::String line = name + " in one file: " + channelLine(width, fileOrder);
    if (width == room::Width::Stereo)
        return line + ".";
    if (container == Container::Flac)
        return line + ", FLAC's own order.";
    if (! orderMatters(width))
        return line + ". Channel Order is for 7.1.4.";
    return line + (fileOrder == room::FileOrder::ProTools && container == Container::Wav ? ", as Pro Tools writes it." : ".");
}

std::optional<std::uint32_t> maskFor(room::Width width, room::FileOrder order) noexcept
{
    const auto& wanted = room::fileChannels(width, order);
    const auto& maskOrder = room::fileChannels(width, room::FileOrder::WavStandard);
    const int n = room::layout(width).numChannels;
    if (std::equal(wanted.begin(), wanted.begin() + n, maskOrder.begin()))
        return room::wavChannelMask(width);
    return std::nullopt;
}

const char* ixmlFunction(room::Width width, room::Speaker speaker) noexcept
{
    using S = room::Speaker;
    switch (speaker)
    {
        case S::L:   return "L";
        case S::R:   return "R";
        case S::C:   return width == room::Width::Mono ? "M" : "C";
        case S::Lfe: return "LFE";
        case S::Ls:  case S::Lsr: return "Ls"; // Pro Tools' Ls is the rear pair at 7.1 and wider
        case S::Rs:  case S::Rsr: return "Rs";
        case S::Lss: return "Sl";
        case S::Rss: return "Sr";
        case S::Ltf: case S::Lts: return "Tl";
        case S::Rtf: case S::Rts: return "Tr";
        case S::Ltr: return "Trl";
        case S::Rtr: return "Trr";
        default:     return "";
    }
}

juce::String ixmlFor(room::Width width, room::FileOrder order, const juce::String& name, int processingChannel)
{
    const auto& l = room::layout(width);
    const juce::String format(l.name);
    auto escaped = [](juce::String text)
    {
        return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;");
    };
    juce::String x;
    auto track = [&](int channelIndex, int interleaveIndex, int processing)
    {
        const auto speaker = l.speakers[static_cast<size_t>(processing)];
        x << "        <TRACK>\n"
          << "            <CHANNEL_INDEX>" << channelIndex << "</CHANNEL_INDEX>\n"
          << "            <INTERLEAVE_INDEX>" << interleaveIndex << "</INTERLEAVE_INDEX>\n"
          << "            <NAME>" << escaped(name + "-" + room::info(speaker).name) << "</NAME>\n"
          << "            <FUNCTION>" << ixmlFunction(width, speaker) << "-" << format << "</FUNCTION>\n"
          << "        </TRACK>\n";
    };
    x << "<?xml version=\"1.0\"?>\n<BWFXML>\n    <IXML_VERSION>2.01</IXML_VERSION>\n    <TRACK_LIST>\n";
    if (processingChannel >= 0 && processingChannel < l.numChannels)
    {
        // One file of a Multi-Mono set: its place in the set, in the order Pro Tools lists the track's channels.
        const auto trackOrder = room::trackChannels(width);
        const int at = static_cast<int>(std::find(trackOrder.begin(), trackOrder.begin() + l.numChannels, processingChannel) - trackOrder.begin());
        x << "        <TRACK_COUNT>1</TRACK_COUNT>\n";
        track(at + 1, 1, processingChannel);
    }
    else
    {
        const auto& channels = room::fileChannels(width, order);
        x << "        <TRACK_COUNT>" << l.numChannels << "</TRACK_COUNT>\n";
        for (int i = 0; i < l.numChannels; ++i)
            track(i + 1, i + 1, channels[static_cast<size_t>(i)]);
    }
    x << "    </TRACK_LIST>\n</BWFXML>\n";
    return x;
}

juce::Array<juce::File> filesOf(const juce::File& file, room::Width width, Layout layout)
{
    if (layout == Layout::Interleaved || width == room::Width::Mono)
        return { file };
    juce::Array<juce::File> files;
    const auto stem = file.getFileNameWithoutExtension();
    const auto ext = file.getFileExtension();
    for (int ch = 0; ch < room::layout(width).numChannels; ++ch)
        files.add(file.getSiblingFile(stem + room::multiMonoSuffix(width, ch) + ext));
    return files;
}

bool anyExists(const juce::Array<juce::File>& files)
{
    return std::any_of(files.begin(), files.end(), [](const juce::File& f) { return f.exists(); });
}

Found find(const juce::File& anyFile)
{
    Found found;
    for (int i = 0; i < room::c_maxChannels; ++i)
        found.processingOf[static_cast<size_t>(i)] = i;

    // One of a Multi-Mono set: <stem>.<suffix><ext>, with every other file of the set beside it.
    const auto name = anyFile.getFileNameWithoutExtension();
    const auto ext = anyFile.getFileExtension();
    if (const int dot = name.lastIndexOfChar('.'); dot > 0)
    {
        const auto suffix = name.substring(dot), stem = name.substring(0, dot);
        for (auto w : c_widestFirst)
        {
            const auto& l = room::layout(w);
            juce::Array<juce::File> files;
            bool member = false, whole = true;
            for (int ch = 0; ch < l.numChannels && whole; ++ch)
            {
                const juce::String s = room::multiMonoSuffix(w, ch);
                const auto f = anyFile.getSiblingFile(stem + s + ext);
                member = member || s == suffix;
                whole = f.existsAsFile();
                files.add(f);
            }
            if (member && whole)
            {
                found.files = files;
                found.format = { w, room::FileOrder::ProTools, Layout::MultiMono };
                found.numChannels = l.numChannels;
                found.knownWidth = true;
                found.described = true; // the names say which speaker each file is
                return found;
            }
        }
    }

    // One file.
    found.files = { anyFile };
    found.format.layout = Layout::Interleaved;
    const auto reader = readerFor(anyFile);
    if (reader == nullptr)
        return found;
    found.numChannels = static_cast<int>(reader->numChannels);
    const auto width = widthForChannels(found.numChannels);
    if (! width.has_value())
        return found; // channels as they are
    found.knownWidth = true;
    found.format.width = *width;
    const auto& l = room::layout(*width);

    // What the file says, as processing channels: every channel a speaker of this width, none twice.
    const auto said = speakersSaid(anyFile, found.numChannels);
    std::array<int, room::c_maxChannels> map {};
    std::array<bool, room::c_maxChannels> used {};
    bool described = said.size() == l.numChannels;
    for (int i = 0; i < said.size() && described; ++i)
    {
        const int at = l.indexOf(said[i]);
        described = at >= 0 && ! used[static_cast<size_t>(at)];
        if (described)
        {
            used[static_cast<size_t>(at)] = true;
            map[static_cast<size_t>(i)] = at;
        }
    }
    if (described)
    {
        std::copy(map.begin(), map.begin() + l.numChannels, found.processingOf.begin());
        found.described = true;
    }
    else
    {
        // Said nothing: the mask order, which is how every program reads a file that says nothing, and FLAC's own.
        const auto& channels = room::fileChannels(*width, room::FileOrder::WavStandard);
        std::copy(channels.begin(), channels.begin() + l.numChannels, found.processingOf.begin());
    }
    auto same = [&](room::FileOrder order)
    {
        const auto& channels = room::fileChannels(*width, order);
        return std::equal(channels.begin(), channels.begin() + l.numChannels, found.processingOf.begin());
    };
    found.format.order = same(room::FileOrder::WavStandard) && ! same(room::FileOrder::ProTools) ? room::FileOrder::WavStandard : room::FileOrder::ProTools;
    return found;
}

juce::String readChannelLine(const juce::File& file)
{
    const auto reader = readerFor(file);
    if (reader == nullptr)
        return {};
    juce::StringArray names;
    for (const auto speaker : speakersSaid(file, static_cast<int>(reader->numChannels)))
        names.add(room::info(speaker).name);
    return names.joinIntoString(" ");
}

std::unique_ptr<juce::AudioFormatReader> openReader(const juce::File& anyFile, juce::String* error)
{
    return openReader(find(anyFile), error);
}

std::unique_ptr<juce::AudioFormatReader> openReader(const Found& take, juce::String* error)
{
    auto fail = [&]() -> std::unique_ptr<juce::AudioFormatReader>
    {
        if (error != nullptr)
            *error = "Couldn't read " + (take.files.isEmpty() ? juce::String("the take") : take.files[0].getFileName());
        return {};
    };
    if (take.files.isEmpty() || take.numChannels <= 0)
        return fail();
    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    for (const auto& f : take.files)
    {
        auto r = readerFor(f);
        if (r == nullptr || r->sampleRate <= 0.0 || (! readers.empty() && std::abs(r->sampleRate - readers.front()->sampleRate) > 0.5))
            return fail();
        readers.push_back(std::move(r));
    }
    return std::make_unique<TakeReader>(std::move(readers), take);
}

juce::String displayName(const juce::Array<juce::File>& files)
{
    if (files.isEmpty())
        return {};
    if (files.size() == 1)
        return files[0].getFileName();
    const auto name = files[0].getFileNameWithoutExtension();
    return name.upToLastOccurrenceOf(".", false, false) + " (" + juce::String(files.size()) + " Mono Files)";
}

//======================================================================================================================
WavFormat readWavFormat(const juce::File& file)
{
    WavFormat f;
    const auto place = findFmt(file);
    if (! place.ok || place.fmtSize < 16)
        return f;
    f.rf64 = place.rf64;
    f.formatTag = readU16(place.fmt);
    f.channels = readU16(place.fmt + 2);
    f.rate = static_cast<double>(readU32(place.fmt + 4));
    f.bits = readU16(place.fmt + 14);
    f.extensible = f.formatTag == 0xfffe && place.fmtSize >= 40;
    if (f.extensible)
        f.mask = readU32(place.fmt + 20);
    f.ok = true;
    return f;
}

bool setWavChannelMask(const juce::File& file, std::optional<std::uint32_t> mask)
{
    const auto place = findFmt(file);
    if (! place.ok)
        return false;
    const std::uint16_t tag = readU16(place.fmt);
    // The sample type: 1 PCM or 3 float, from a plain tag or an extensible chunk's sub-format.
    const std::uint32_t sampleType = tag == 0xfffe ? readU32(place.fmt + 24) : tag;
    if (sampleType != 1 && sampleType != 3)
        return false;

    // The 40-byte body: channels, rate, bytes a second, block align and bits as they are, then either the extension
    // with the mask and the sub-format GUID (PCM or IEEE float, then 0000-0010-8000-00AA00389B71), or Pro Tools'
    // plain form with no extension and the rest zero.
    unsigned char body[40] {};
    std::memcpy(body + 2, place.fmt + 2, 14);
    if (mask.has_value())
    {
        const unsigned char guidTail[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
        putU16(body, 0xfffe);
        putU16(body + 16, 22);                      // the size of the extension
        putU16(body + 18, readU16(place.fmt + 14)); // valid bits
        putU32(body + 20, *mask);
        putU32(body + 24, sampleType);
        std::memcpy(body + 28, guidTail, 12);
    }
    else
    {
        putU16(body, static_cast<std::uint16_t>(sampleType));
    }

    if (place.fmtSize == 40)
    {
        juce::FileOutputStream out(file);
        if (! out.openedOk() || ! out.setPosition(place.fmtStart + 8) || ! out.write(body, sizeof(body)))
            return false;
        out.flush();
        return out.getStatus().wasOk();
    }
    // A plain format chunk with JUCE's padding in front of it: JUNK (52) + fmt (16) becomes JUNK (28) + fmt (40).
    if (place.fmtSize != 16 || place.prevStart < 0 || std::memcmp(place.prevId, "JUNK", 4) != 0 || place.prevSize != 52
        || place.prevStart + 8 + 52 != place.fmtStart)
        return false;
    unsigned char block[84] {};
    std::memcpy(block, "JUNK", 4);
    putU32(block + 4, 28);
    std::memcpy(block + 36, "fmt ", 4);
    putU32(block + 40, 40);
    std::memcpy(block + 44, body, sizeof(body));
    juce::FileOutputStream out(file);
    if (! out.openedOk() || ! out.setPosition(place.prevStart) || ! out.write(block, sizeof(block)))
        return false;
    out.flush();
    return out.getStatus().wasOk();
}

juce::String readIxml(const juce::File& file)
{
    juce::FileInputStream in(file);
    if (! in.openedOk())
        return {};
    char riff[4] {}, wave[4] {};
    if (in.read(riff, 4) != 4)
        return {};
    const bool rf64 = std::memcmp(riff, "RF64", 4) == 0;
    if (! rf64 && std::memcmp(riff, "RIFF", 4) != 0)
        return {};
    in.readInt();
    if (in.read(wave, 4) != 4 || std::memcmp(wave, "WAVE", 4) != 0)
        return {};
    juce::int64 dataSize = -1; // an RF64 file's, from its ds64 chunk
    const auto length = in.getTotalLength();
    for (int guard = 0; guard < 256 && in.getPosition() + 8 <= length; ++guard)
    {
        const auto start = in.getPosition();
        char id[4] {};
        in.read(id, 4);
        juce::int64 size = static_cast<juce::uint32>(in.readInt());
        if (std::memcmp(id, "ds64", 4) == 0 && size >= 16)
        {
            in.readInt64();
            dataSize = in.readInt64();
        }
        else if (std::memcmp(id, "data", 4) == 0 && rf64 && size == 0xffffffffLL && dataSize >= 0)
        {
            size = dataSize;
        }
        else if (std::memcmp(id, "iXML", 4) == 0)
        {
            juce::MemoryBlock text;
            in.readIntoMemoryBlock(text, static_cast<juce::pointer_sized_int>(juce::jmin<juce::int64>(size, 1 << 20)));
            const auto* chars = static_cast<const char*>(text.getData());
            const auto bytes = text.getSize();
            size_t used = 0;
            while (used < bytes && chars[used] != 0)
                ++used;
            return juce::String::fromUTF8(chars, static_cast<int>(used));
        }
        if (! in.setPosition(start + 8 + size + (size & 1)))
            break;
    }
    return {};
}

bool appendWavChunk(const juce::File& file, const char* id, const void* data, size_t size)
{
    const auto fileSize = file.getSize();
    bool rf64 = false;
    {
        juce::FileInputStream in(file);
        char tag[4] {}, ds64[4] {};
        if (! in.openedOk() || in.read(tag, 4) != 4)
            return false;
        rf64 = std::memcmp(tag, "RF64", 4) == 0;
        if (! rf64 && std::memcmp(tag, "RIFF", 4) != 0)
            return false;
        // An RF64 file keeps its size in the ds64 chunk that must come first.
        if (rf64 && (! in.setPosition(12) || in.read(ds64, 4) != 4 || std::memcmp(ds64, "ds64", 4) != 0))
            return false;
    }
    juce::MemoryOutputStream chunk;
    if ((fileSize & 1) != 0)
        chunk.writeByte(0);
    chunk.write(id, 4);
    chunk.writeInt(static_cast<int>(static_cast<std::uint32_t>(size)));
    chunk.write(data, size);
    if ((size & 1) != 0)
        chunk.writeByte(0);
    const auto newSize = fileSize + static_cast<juce::int64>(chunk.getDataSize());
    if (! rf64 && newSize - 8 > 0xffffffffLL)
        return false;
    juce::FileOutputStream out(file); // opens at the end of the file
    if (! out.openedOk() || ! out.setPosition(fileSize) || ! out.write(chunk.getData(), chunk.getDataSize()))
        return false;
    if (rf64)
    {
        if (! out.setPosition(20))
            return false;
        out.writeInt64(newSize - 8);
    }
    else
    {
        if (! out.setPosition(4))
            return false;
        out.writeInt(static_cast<int>(static_cast<std::uint32_t>(newSize - 8)));
    }
    out.flush();
    return out.getStatus().wasOk();
}

//======================================================================================================================
bool appendAiffChannelLayout(const juce::File& file, const juce::Array<room::Speaker>& speakers)
{
    const auto size = file.getSize();
    {
        juce::FileInputStream in(file);
        char tag[4] {};
        if (! in.openedOk() || in.read(tag, 4) != 4 || std::memcmp(tag, "FORM", 4) != 0)
            return false;
    }
    // AudioChannelLayout, big-endian: kAudioChannelLayoutTag_UseChannelDescriptions (0), no bitmap, then one
    // AudioChannelDescription (label, flags, three coordinates) for each channel.
    juce::MemoryOutputStream body;
    body.writeIntBigEndian(0);
    body.writeIntBigEndian(0);
    body.writeIntBigEndian(speakers.size());
    for (const auto speaker : speakers)
    {
        body.writeIntBigEndian(static_cast<int>(coreAudioLabel(speaker)));
        body.writeIntBigEndian(0);
        for (int i = 0; i < 3; ++i)
            body.writeIntBigEndian(0);
    }
    juce::MemoryOutputStream chunk;
    if ((size & 1) != 0)
        chunk.writeByte(0);
    chunk.write("CHAN", 4);
    chunk.writeIntBigEndian(static_cast<int>(body.getDataSize()));
    chunk.write(body.getData(), body.getDataSize());
    const auto newSize = size + static_cast<juce::int64>(chunk.getDataSize());
    if (newSize - 8 > 0x7fffffffLL)
        return false;
    juce::FileOutputStream out(file); // opens at the end of the file
    if (! out.openedOk() || ! out.setPosition(size) || ! out.write(chunk.getData(), chunk.getDataSize()))
        return false;
    if (! out.setPosition(4))
        return false;
    out.writeIntBigEndian(static_cast<int>(newSize - 8));
    out.flush();
    return out.getStatus().wasOk();
}

juce::Array<room::Speaker> readAiffChannelLayout(const juce::File& file)
{
    juce::FileInputStream in(file);
    char form[4] {}, kind[4] {};
    if (! in.openedOk() || in.read(form, 4) != 4 || std::memcmp(form, "FORM", 4) != 0)
        return {};
    in.readIntBigEndian();
    if (in.read(kind, 4) != 4 || (std::memcmp(kind, "AIFF", 4) != 0 && std::memcmp(kind, "AIFC", 4) != 0))
        return {};
    const auto length = in.getTotalLength();
    for (int guard = 0; guard < 256 && in.getPosition() + 8 <= length; ++guard)
    {
        const auto start = in.getPosition();
        char id[4] {};
        in.read(id, 4);
        const juce::int64 size = static_cast<juce::uint32>(in.readIntBigEndian());
        if (std::memcmp(id, "CHAN", 4) == 0 && size >= 12)
        {
            const auto layoutTag = readBigU32(in);
            readBigU32(in); // bitmap
            const auto count = static_cast<int>(readBigU32(in));
            if (layoutTag != 0 || count < 1 || count > room::c_maxChannels || size < 12 + 20 * count)
                return {};
            const auto width = widthForChannels(count).value_or(room::Width::Stereo);
            juce::Array<room::Speaker> speakers;
            for (int i = 0; i < count; ++i)
            {
                const auto label = readBigU32(in);
                in.skipNextBytes(16);
                const auto speaker = speakerOfCoreAudioLabel(width, label);
                if (! speaker.has_value())
                    return {};
                speakers.add(*speaker);
            }
            return speakers;
        }
        if (! in.setPosition(start + 8 + size + (size & 1)))
            break;
    }
    return {};
}

//======================================================================================================================
juce::StringArray flacComments(const juce::File& file, juce::String* vendor)
{
    juce::StringArray comments;
    juce::FileInputStream in(file);
    std::vector<FlacBlock> blocks;
    if (! in.openedOk() || ! readFlacBlocks(in, blocks))
        return comments;
    for (const auto& block : blocks)
    {
        if (block.type != 4)
            continue;
        juce::MemoryInputStream data(block.data, false);
        const auto vendorLength = static_cast<size_t>(data.readInt());
        juce::MemoryBlock text;
        data.readIntoMemoryBlock(text, static_cast<juce::pointer_sized_int>(vendorLength));
        if (vendor != nullptr)
            *vendor = juce::String::fromUTF8(static_cast<const char*>(text.getData()), static_cast<int>(text.getSize()));
        const int count = data.readInt();
        for (int i = 0; i < count && ! data.isExhausted(); ++i)
        {
            const auto length = static_cast<size_t>(data.readInt());
            juce::MemoryBlock entry;
            data.readIntoMemoryBlock(entry, static_cast<juce::pointer_sized_int>(length));
            comments.add(juce::String::fromUTF8(static_cast<const char*>(entry.getData()), static_cast<int>(entry.getSize())));
        }
    }
    return comments;
}

bool setFlacComments(const juce::File& file, const juce::StringArray& comments)
{
    juce::String vendor;
    flacComments(file, &vendor);
    if (vendor.isEmpty())
        vendor = "Swooshbuckler";

    juce::MemoryOutputStream block;
    auto writeText = [&block](const juce::String& text)
    {
        const auto bytes = text.getNumBytesAsUTF8();
        block.writeInt(static_cast<int>(bytes));
        block.write(text.toRawUTF8(), bytes);
    };
    writeText(vendor);
    block.writeInt(comments.size());
    for (const auto& c : comments)
        writeText(c);

    juce::FileInputStream in(file);
    std::vector<FlacBlock> blocks;
    if (! in.openedOk() || ! readFlacBlocks(in, blocks))
        return false;
    bool replaced = false;
    for (auto& b : blocks)
        if (b.type == 4)
        {
            b.data = block.getMemoryBlock();
            replaced = true;
        }
    if (! replaced)
        blocks.push_back({ 4, block.getMemoryBlock() });

    juce::TemporaryFile temp(file);
    {
        juce::FileOutputStream out(temp.getFile());
        if (! out.openedOk())
            return false;
        out.write("fLaC", 4);
        for (size_t i = 0; i < blocks.size(); ++i)
        {
            const auto length = blocks[i].data.getSize();
            const unsigned char head[4] = { static_cast<unsigned char>((i + 1 == blocks.size() ? 0x80 : 0) | blocks[i].type),
                                            static_cast<unsigned char>((length >> 16) & 0xff),
                                            static_cast<unsigned char>((length >> 8) & 0xff),
                                            static_cast<unsigned char>(length & 0xff) };
            out.write(head, 4);
            out.write(blocks[i].data.getData(), length);
        }
        if (out.writeFromInputStream(in, -1) < 0)
            return false;
        out.flush();
        if (out.getStatus().failed())
            return false;
    }
    return temp.overwriteTargetFileWithTemporary();
}
}

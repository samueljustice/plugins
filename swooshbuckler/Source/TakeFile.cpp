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

juce::String orderLine(room::Width width, room::FileOrder order, Layout layout)
{
    const juce::String name(room::layout(width).name);
    if (width == room::Width::Mono)
        return "Mono: one channel in one file.";
    if (layout == Layout::Interleaved)
        return name + " in one file: " + channelLine(width, order) + "." + (orderMatters(width) ? "" : " Channel Order is for 5.1 and wider.");
    // A Multi-Mono set has no order of its own: its files are listed the way Pro Tools lists a track's channels.
    const int n = room::layout(width).numChannels;
    const auto& proTools = room::fileChannels(width, room::FileOrder::ProTools);
    juce::StringArray suffixes;
    for (int i = 0; i < n; ++i)
        suffixes.add(room::multiMonoSuffix(width, proTools[static_cast<size_t>(i)]));
    return name + " as " + juce::String(n) + " files ending " + suffixes.joinIntoString(" ") + ".";
}

std::uint32_t maskFor(room::Width width, room::FileOrder order) noexcept
{
    const auto& wanted = room::fileChannels(width, order);
    const auto& maskOrder = room::fileChannels(width, room::FileOrder::Smpte);
    const int n = room::layout(width).numChannels;
    return std::equal(wanted.begin(), wanted.begin() + n, maskOrder.begin()) ? room::wavChannelMask(width) : 0u;
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

    auto tokens = juce::StringArray::fromTokens(readChannelLine(anyFile), " ", "");
    tokens.removeEmptyStrings();
    bool described = tokens.size() == l.numChannels;
    std::array<int, room::c_maxChannels> map {};
    std::array<bool, room::c_maxChannels> used {};
    for (int i = 0; i < tokens.size() && described; ++i)
    {
        const auto speaker = speakerNamed(tokens[i]);
        const int at = speaker.has_value() ? l.indexOf(*speaker) : -1;
        described = at >= 0 && ! used[static_cast<size_t>(at)];
        if (described)
        {
            used[static_cast<size_t>(at)] = true;
            map[static_cast<size_t>(i)] = at;
        }
    }
    if (described)
    {
        for (int i = 0; i < l.numChannels; ++i)
            found.processingOf[static_cast<size_t>(i)] = map[static_cast<size_t>(i)];
        found.described = true;
    }
    else
    {
        // Said nothing: a mask (or no mask at all) means the order of the mask's bits, the WAV convention, which
        // is also FLAC's. Mono and Stereo have one order.
        const auto& channels = room::fileChannels(*width, room::FileOrder::Smpte);
        std::copy(channels.begin(), channels.begin() + l.numChannels, found.processingOf.begin());
    }
    auto same = [&](room::FileOrder order)
    {
        const auto& channels = room::fileChannels(*width, order);
        return std::equal(channels.begin(), channels.begin() + l.numChannels, found.processingOf.begin());
    };
    found.format.order = same(room::FileOrder::Smpte) && ! same(room::FileOrder::ProTools) ? room::FileOrder::Smpte : room::FileOrder::ProTools;
    return found;
}

juce::String readChannelLine(const juce::File& file)
{
    if (file.hasFileExtension("flac"))
    {
        for (const auto& c : flacComments(file))
            if (c.startsWithIgnoreCase("CHANNELS="))
                return c.fromFirstOccurrenceOf("=", false, false).trim();
        return {};
    }
    const auto reader = readerFor(file);
    if (reader == nullptr)
        return {};
    const auto& meta = reader->metadataValues;
    if (file.hasFileExtension("wav"))
        return meta[juce::WavAudioFormat::aswgChannelConfig].trim();
    const int notes = meta["NumCueNotes"].getIntValue();
    for (int i = 0; i < notes; ++i)
    {
        const auto text = meta["CueNote" + juce::String(i) + "Text"];
        if (text.startsWith("Channels: "))
            return text.fromFirstOccurrenceOf("Channels: ", false, false).trim();
    }
    return {};
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

bool setWavChannelMask(const juce::File& file, std::uint32_t mask)
{
    const auto place = findFmt(file);
    if (! place.ok)
        return false;
    if (place.fmtSize == 40 && readU16(place.fmt) == 0xfffe)
    {
        juce::FileOutputStream out(file);
        unsigned char bytes[4] {};
        putU32(bytes, mask);
        if (! out.openedOk() || ! out.setPosition(place.fmtStart + 8 + 20) || ! out.write(bytes, 4))
            return false;
        out.flush();
        return out.getStatus().wasOk();
    }
    // A plain format chunk with JUCE's padding in front of it: JUNK (52) + fmt (16) becomes JUNK (28) + fmt (40).
    const std::uint16_t tag = readU16(place.fmt);
    if (place.fmtSize != 16 || place.prevStart < 0 || std::memcmp(place.prevId, "JUNK", 4) != 0 || place.prevSize != 52
        || place.prevStart + 8 + 52 != place.fmtStart || (tag != 1 && tag != 3))
        return false;
    unsigned char block[84] {};
    std::memcpy(block, "JUNK", 4);
    putU32(block + 4, 28);
    unsigned char* fmt = block + 36;
    std::memcpy(fmt, "fmt ", 4);
    putU32(fmt + 4, 40);
    unsigned char* body = fmt + 8;
    putU16(body, 0xfffe);
    std::memcpy(body + 2, place.fmt + 2, 14); // channels, rate, bytes a second, block align, bits
    putU16(body + 16, 22);                   // the size of the extension
    putU16(body + 18, readU16(place.fmt + 14)); // valid bits
    putU32(body + 20, mask);
    // The sub-format GUID: PCM (1) or IEEE float (3), then 0000-0010-8000-00AA00389B71.
    const unsigned char guidTail[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
    putU32(body + 24, tag);
    std::memcpy(body + 28, guidTail, 12);
    juce::FileOutputStream out(file);
    if (! out.openedOk() || ! out.setPosition(place.prevStart) || ! out.write(block, sizeof(block)))
        return false;
    out.flush();
    return out.getStatus().wasOk();
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

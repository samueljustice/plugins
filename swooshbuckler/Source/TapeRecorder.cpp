#include "TapeRecorder.h"

namespace
{
    /**
     * Sits between the ThreadedWriter and the real WAV writer and converts the
     * session rate to the chosen file rate with a windowed-sinc interpolator.
     * Runs on the writer's background thread, never the audio thread.
     *
     * It reports itself as a float writer, so writeFromFloatArrays() hands it
     * the float pointers unchanged (cast to int**, the JUCE convention).
     */
    class ResamplingWriter : public juce::AudioFormatWriter
    {
    public:
        /** inputChannels arrive; the inner writer gets inner->getNumChannels() (1 = mono fold-down). */
        ResamplingWriter(std::unique_ptr<juce::AudioFormatWriter> inner, double inputRate, int inputChannels)
            : AudioFormatWriter(nullptr, "Converting", inner->getSampleRate(), static_cast<unsigned int>(inputChannels), 32)
            , m_inner(std::move(inner))
            , m_ratio(inputRate / m_inner->getSampleRate())
            , m_inputChannels(inputChannels)
            , m_pending(static_cast<size_t>(m_inner->getNumChannels()))
            , m_interpolators(static_cast<size_t>(m_inner->getNumChannels()))
        {
            usesFloatingPointData = true;
            m_passThrough = std::abs(m_ratio - 1.0) < 1.0e-9;
            m_skip = m_passThrough ? 0 : static_cast<std::int64_t>(std::round(juce::WindowedSincInterpolator::getBaseLatency() / m_ratio));
        }

        ~ResamplingWriter() override
        {
            // Flush the interpolator's latency with silence, then trim to the exact length.
            const int flush = m_passThrough ? 0 : static_cast<int>(juce::WindowedSincInterpolator::getBaseLatency()) + 32;
            for (auto& p : m_pending)
                p.insert(p.end(), static_cast<size_t>(flush), 0.0f);
            drain(true);
        }

        bool write(const int** samples, int numSamples) override
        {
            const auto** floats = reinterpret_cast<const float**>(samples);
            if (m_pending.size() == 1 && m_inputChannels > 1)
            {
                // Mono fold-down: plain average (keeps level-matched mono, no clipping)
                auto& p = m_pending[0];
                const float g = 1.0f / static_cast<float>(m_inputChannels);
                for (int i = 0; i < numSamples; ++i)
                {
                    float sum = 0.0f;
                    for (int ch = 0; ch < m_inputChannels; ++ch)
                        sum += floats[ch][i];
                    p.push_back(sum * g);
                }
            }
            else
            {
                for (size_t ch = 0; ch < m_pending.size(); ++ch)
                    m_pending[ch].insert(m_pending[ch].end(), floats[juce::jmin(static_cast<int>(ch), m_inputChannels - 1)],
                                         floats[juce::jmin(static_cast<int>(ch), m_inputChannels - 1)] + numSamples);
            }
            m_inputTotal += numSamples;
            return drain(false);
        }

    private:
        bool drain(bool final)
        {
            const int available = static_cast<int>(m_pending[0].size());
            const int margin = (final || m_passThrough) ? 0 : 8;
            int produce = m_passThrough ? available : static_cast<int>(static_cast<double>(available - margin) / m_ratio) - 1;
            if (produce <= 0)
                return true;

            m_out.setSize(static_cast<int>(m_pending.size()), produce, false, false, true);
            int used = produce;
            for (size_t ch = 0; ch < m_pending.size(); ++ch)
            {
                if (m_passThrough)
                    std::copy(m_pending[ch].begin(), m_pending[ch].begin() + produce, m_out.getWritePointer(static_cast<int>(ch)));
                else
                    used = m_interpolators[ch].process(m_ratio, m_pending[ch].data(), m_out.getWritePointer(static_cast<int>(ch)), produce);
            }
            for (auto& p : m_pending)
                p.erase(p.begin(), p.begin() + juce::jmin(used, static_cast<int>(p.size())));

            // Drop the interpolator's start-up latency; never write past the exact converted length.
            int offset = 0;
            if (m_skip > 0)
            {
                offset = static_cast<int>(juce::jmin<std::int64_t>(m_skip, produce));
                m_skip -= offset;
            }
            const auto target = static_cast<std::int64_t>(std::llround(static_cast<double>(m_inputTotal) / m_ratio));
            int count = produce - offset;
            if (final)
                count = static_cast<int>(juce::jlimit<std::int64_t>(0, count, target - m_written));
            if (count <= 0)
                return true;

            const float* chans[8] {};
            for (int ch = 0; ch < juce::jmin(8, m_out.getNumChannels()); ++ch)
                chans[ch] = m_out.getReadPointer(ch, offset);
            m_written += count;
            return m_inner->writeFromFloatArrays(chans, m_out.getNumChannels(), count);
        }

        std::unique_ptr<juce::AudioFormatWriter> m_inner;
        double m_ratio = 1.0; // input samples per output sample
        int m_inputChannels = 2;
        bool m_passThrough = false;
        std::vector<std::vector<float>> m_pending;
        std::vector<juce::WindowedSincInterpolator> m_interpolators;
        juce::AudioBuffer<float> m_out;
        std::int64_t m_skip = 0, m_inputTotal = 0, m_written = 0;
    };

    /**
     * 16-bit files: adds triangular (TPDF) dither of one step peak either side before the writer cuts the
     * samples down to 16 bits, so quiet fades end in a steady hiss instead of distortion that follows the sound.
     * JUCE's writers cut downwards; the half step added here makes that a rounding with no offset.
     * Writer thread (or the message thread for LAST 60s), never the audio thread.
     */
    class DitherWriter : public juce::AudioFormatWriter
    {
    public:
        explicit DitherWriter(std::unique_ptr<juce::AudioFormatWriter> inner)
            : AudioFormatWriter(nullptr, "Dither", inner->getSampleRate(), static_cast<unsigned int>(inner->getNumChannels()), 32)
            , m_inner(std::move(inner))
        {
            usesFloatingPointData = true;
        }

        bool write(const int** samples, int numSamples) override
        {
            const auto** floats = reinterpret_cast<const float**>(samples);
            const int channels = static_cast<int>(getNumChannels());
            constexpr float step = 1.0f / 32768.0f;
            constexpr int c_piece = 4096;
            m_buffer.setSize(channels, c_piece, false, false, true);
            for (int done = 0; done < numSamples; done += c_piece)
            {
                const int n = juce::jmin(c_piece, numSamples - done);
                for (int ch = 0; ch < channels; ++ch)
                {
                    const float* in = floats[ch] + done;
                    float* out = m_buffer.getWritePointer(ch);
                    for (int i = 0; i < n; ++i)
                        out[i] = in[i] + step * (0.5f + m_random.nextFloat() - m_random.nextFloat());
                }
                if (! m_inner->writeFromFloatArrays(m_buffer.getArrayOfReadPointers(), channels, n))
                    return false;
            }
            return true;
        }

    private:
        std::unique_ptr<juce::AudioFormatWriter> m_inner;
        juce::AudioBuffer<float> m_buffer;
        juce::Random m_random;
    };

    std::unique_ptr<juce::AudioFormatWriter> createWriter(const juce::File& file, double inputRate, int inputChannels,
                                                          const TapeRecorder::Options& o, const TapeRecorder::Naming& naming,
                                                          juce::String& error)
    {
        file.deleteFile();
        auto stream = std::make_unique<juce::FileOutputStream>(file);
        if (! stream->openedOk())
        {
            error = "Couldn't open " + file.getFullPathName() + " for writing.";
            return {};
        }

        const double fileRate = o.fileRate > 0.0 ? o.fileRate : inputRate;
        const int channels = o.mono ? 1 : inputChannels;
        int bits = (o.bitDepth == 16 || o.bitDepth == 24 || o.bitDepth == 32) ? o.bitDepth : 24;
        if (o.type != TapeRecorder::FileType::Wav && bits == 32)
            bits = 24; // AIFF and FLAC in JUCE are integer only (the UI says so)

        std::unique_ptr<juce::AudioFormat> format;
        juce::StringPairArray metadata;
        switch (o.type)
        {
            case TapeRecorder::FileType::Aiff:
                // AIFF: the description as a comment (COMT), and who made it with what as a second one.
                format = std::make_unique<juce::AiffAudioFormat>();
                metadata.set("NumCueNotes", "2");
                metadata.set("CueNote0TimeStamp", "0");
                metadata.set("CueNote0Identifier", "0");
                metadata.set("CueNote0Text", naming.description);
                metadata.set("CueNote1TimeStamp", "0");
                metadata.set("CueNote1Identifier", "0");
                metadata.set("CueNote1Text", "Creator: " + naming.creator + ". Software: " + naming.software);
                break;
            case TapeRecorder::FileType::Flac:
                format = std::make_unique<juce::FlacAudioFormat>(); // its Vorbis comments are written once it is closed (finishFile)
                break;
            case TapeRecorder::FileType::Wav:
            default:
                // WAV: BWF bext (description, originator) and the same again as LIST INFO, which more programs show.
                format = std::make_unique<juce::WavAudioFormat>();
                metadata = juce::WavAudioFormat::createBWAVMetadata(
                    naming.description, naming.creator, "Swooshbuckler", juce::Time::getCurrentTime(), 0,
                    "A=PCM,F=" + juce::String(juce::roundToInt(fileRate)) + ",W=" + juce::String(bits)
                        + ",M=" + (channels == 1 ? juce::String("mono") : juce::String("stereo")) + ",T=Swooshbuckler");
                metadata.set(juce::WavAudioFormat::riffInfoComment2, naming.description);
                metadata.set(juce::WavAudioFormat::riffInfoArtist, naming.creator);
                metadata.set(juce::WavAudioFormat::riffInfoSoftware, naming.software);
                break;
        }

        // JUCE writes 32-bit WAV as IEEE float.
        std::unique_ptr<juce::AudioFormatWriter> writer(
            format->createWriterFor(stream.get(), fileRate, static_cast<unsigned int>(channels), bits, metadata, 0));
        if (writer == nullptr)
        {
            error = format->getFormatName() + " can't be written at " + juce::String(fileRate / 1000.0, 1) + " kHz / "
                  + juce::String(bits) + "-bit.";
            return {};
        }
        stream.release(); // now owned by the writer
        if (bits == 16)
            writer = std::make_unique<DitherWriter>(std::move(writer));

        if (std::abs(fileRate - inputRate) < 0.5 && channels == inputChannels)
            return writer;
        return std::make_unique<ResamplingWriter>(std::move(writer), inputRate, inputChannels);
    }

    juce::String extensionFor(TapeRecorder::FileType type)
    {
        switch (type)
        {
            case TapeRecorder::FileType::Aiff: return ".aif";
            case TapeRecorder::FileType::Flac: return ".flac";
            case TapeRecorder::FileType::Wav:
            default: return ".wav";
        }
    }

    //==============================================================================
    // What is added to a file once its writer has closed it. Never on the audio thread.

    /** Appends a cue chunk and its labels to a closed WAV file (chunks after the audio are part of the format, and
        Reaper, Soundminer and the others read them there). Not for RF64 files (over 4 GB). */
    bool appendWavCues(const juce::File& file, const std::vector<TapeRecorder::Cue>& cues)
    {
        if (cues.empty())
            return true;
        const auto size = file.getSize();
        {
            juce::FileInputStream in(file);
            char tag[4] {};
            if (! in.openedOk() || in.read(tag, 4) != 4 || std::memcmp(tag, "RIFF", 4) != 0)
                return false;
        }

        juce::MemoryOutputStream chunks;
        if ((size & 1) != 0)
            chunks.writeByte(0);
        const auto count = static_cast<int>(cues.size());
        chunks.write("cue ", 4);
        chunks.writeInt(4 + 24 * count);
        chunks.writeInt(count);
        for (int i = 0; i < count; ++i)
        {
            const auto frame = static_cast<int>(static_cast<std::uint32_t>(juce::jlimit<std::int64_t>(0, 0xffffffffLL, cues[static_cast<size_t>(i)].frame)));
            chunks.writeInt(i + 1);       // identifier
            chunks.writeInt(frame);       // position in the play order
            chunks.write("data", 4);
            chunks.writeInt(0);           // chunk start
            chunks.writeInt(0);           // block start
            chunks.writeInt(frame);       // sample offset
        }
        juce::MemoryOutputStream labels;
        labels.write("adtl", 4);
        for (int i = 0; i < count; ++i)
        {
            const auto text = cues[static_cast<size_t>(i)].label;
            const auto bytes = static_cast<int>(text.getNumBytesAsUTF8()) + 1;
            labels.write("labl", 4);
            labels.writeInt(4 + bytes);
            labels.writeInt(i + 1);
            labels.write(text.toRawUTF8(), static_cast<size_t>(bytes));
            if ((bytes & 1) != 0)
                labels.writeByte(0);
        }
        chunks.write("LIST", 4);
        chunks.writeInt(static_cast<int>(labels.getDataSize()));
        chunks.write(labels.getData(), labels.getDataSize());

        const auto newSize = size + static_cast<juce::int64>(chunks.getDataSize());
        if (newSize - 8 > 0xffffffffLL)
            return false;
        juce::FileOutputStream out(file); // opens at the end of the file
        if (! out.openedOk() || ! out.setPosition(size) || ! out.write(chunks.getData(), chunks.getDataSize()))
            return false;
        if (! out.setPosition(4))
            return false;
        out.writeInt(static_cast<int>(static_cast<std::uint32_t>(newSize - 8)));
        out.flush();
        return true;
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

    juce::StringArray flacComments(const juce::File& file, juce::String* vendor = nullptr)
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

    /** Gives a closed FLAC file its Vorbis comments (NAME=value): the file is written again with the comment block
        replaced, next to the original, and swapped in. */
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

    /** After the writer has closed a file: FLAC gets its description, WAV its cue points. */
    void finishFile(const juce::File& file, TapeRecorder::FileType type, const TapeRecorder::Naming& naming,
                    const std::vector<TapeRecorder::Cue>& cues)
    {
        if (type == TapeRecorder::FileType::Flac)
            setFlacComments(file, { "DESCRIPTION=" + naming.description, "ARTIST=" + naming.creator, "ENCODER=" + naming.software });
        else if (type == TapeRecorder::FileType::Wav)
            appendWavCues(file, cues);
    }
}

//==============================================================================
/**
 * The open take, on the writer thread: gets the take's stream from the ThreadedWriter, opens and closes its files
 * (one for the whole take, or with One File per Whoosh one per stretch of sound, opened when its first sound
 * arrives), and collects the markers of each. The audio thread's commands say where in the stream a marker is or
 * a file ends.
 */
class TapeRecorder::Sink : public juce::AudioFormatWriter
{
public:
    Sink(TapeRecorder& owner, const juce::File& folder, const Options& options, double rate, int channels)
        : AudioFormatWriter(nullptr, "Tape", rate, static_cast<unsigned int>(channels), 32)
        , m_owner(owner), m_folder(folder), m_options(options)
    {
        usesFloatingPointData = true;
        m_marks.reserve(1024);
    }

    ~Sink() override
    {
        pump();
        // What is still open when the tape stops: the whole take, or a whoosh that was still sounding.
        closeFile(! m_options.perWhoosh
                  || static_cast<double>(m_owner.m_whooshLoud.load()) >= c_minWhooshSeconds * getSampleRate());
    }

    bool open(juce::String& error)
    {
        m_naming = m_owner.getNaming();
        m_file = m_owner.nextFile(m_folder, {}, extensionFor(m_options.type));
        m_inner = createWriter(m_file, getSampleRate(), static_cast<int>(getNumChannels()), m_options, m_naming, error);
        if (m_inner == nullptr)
            return false;
        m_fileStart = m_consumed;
        m_marks.clear();
        const juce::ScopedLock sl(m_owner.m_fileLock);
        m_owner.m_currentFile = m_file;
        return true;
    }

    bool write(const int** samples, int numSamples) override
    {
        const auto** floats = reinterpret_cast<const float**>(samples);
        int done = 0;
        for (;;)
        {
            pump();
            if (done >= numSamples)
                break;
            // Up to the next command that falls inside this block.
            int count = numSamples - done;
            Command next;
            if (m_owner.peekCommand(next) && next.position < m_consumed + count)
                count = static_cast<int>(next.position - m_consumed);
            if (m_inner == nullptr && ! m_failed)
            {
                juce::String error;
                if (! open(error))
                {
                    m_failed = true; // this whoosh is lost; the next one tries again
                    const juce::ScopedLock sl(m_owner.m_metaLock);
                    m_owner.m_writeError = error;
                }
            }
            if (m_inner != nullptr)
            {
                const float* chans[8] {};
                const int channels = juce::jmin(8, static_cast<int>(getNumChannels()));
                for (int ch = 0; ch < channels; ++ch)
                    chans[ch] = floats[ch] + done;
                m_inner->writeFromFloatArrays(chans, channels, count);
            }
            m_consumed += count;
            done += count;
        }
        return true;
    }

    /** Carries out the commands whose place in the stream has been reached. */
    void pump()
    {
        Command c;
        while (m_owner.peekCommand(c) && c.position <= m_consumed)
        {
            m_owner.popCommand();
            if (c.type == Command::Split)
            {
                closeFile(c.value != 0);
                m_failed = false;
            }
            else if (m_inner != nullptr)
                m_marks.push_back({ juce::jmax<std::int64_t>(0, c.position - m_fileStart), c.value });
        }
    }

private:
    struct Mark
    {
        std::int64_t position = 0; // samples from the start of the file, at the session rate
        int kind = 0;
    };

    void closeFile(bool keep)
    {
        if (m_inner == nullptr)
            return;
        const double fileRate = m_options.fileRate > 0.0 ? m_options.fileRate : getSampleRate();
        m_inner.reset(); // flushes the resampler and closes the file
        const auto samples = m_consumed - m_fileStart;
        if (! keep || samples <= 0)
        {
            m_file.deleteFile();
            return;
        }

        // Markers that fall together (a hit aimed at a pass's peak) become one cue with both names.
        std::vector<Cue> cues;
        std::stable_sort(m_marks.begin(), m_marks.end(), [](const Mark& a, const Mark& b) { return a.position < b.position; });
        std::int64_t groupStart = 0;
        int groupKinds = 0;
        for (const auto& mark : m_marks)
        {
            const auto label = markerLabel(mark.kind);
            const int bit = 1 << juce::jlimit(0, 30, mark.kind);
            if (! cues.empty() && static_cast<double>(mark.position - groupStart) < 0.05 * getSampleRate())
            {
                if ((groupKinds & bit) == 0)
                    cues.back().label << " + " << label; // "Pass A + Hit"
                groupKinds |= bit;
                continue;
            }
            groupStart = mark.position;
            groupKinds = bit;
            cues.push_back({ static_cast<std::int64_t>(std::llround(static_cast<double>(mark.position) * fileRate / getSampleRate())), label });
        }
        finishFile(m_file, m_options.type, m_naming, cues);

        {
            const juce::ScopedLock sl(m_owner.m_fileLock);
            m_owner.m_lastFile = m_file;
        }
        const juce::ScopedLock sl(m_owner.m_metaLock);
        m_owner.m_finished.push_back({ m_file, static_cast<double>(samples) / getSampleRate(),
                                       m_options.type == FileType::Wav ? static_cast<int>(cues.size()) : 0, m_options.perWhoosh });
        if (m_naming.style == NameStyle::Counter)
            m_owner.m_counter = juce::jmax(m_owner.m_counter, m_owner.numberOf(m_file) + 1);
    }

    TapeRecorder& m_owner;
    juce::File m_folder, m_file;
    Options m_options;
    Naming m_naming;
    std::unique_ptr<juce::AudioFormatWriter> m_inner;
    std::int64_t m_consumed = 0, m_fileStart = 0; // samples of the take's stream taken so far; where this file began
    std::vector<Mark> m_marks;
    bool m_failed = false;
};

int TapeRecorder::Pump::useTimeSlice()
{
    if (owner.m_sink != nullptr)
        owner.m_sink->pump();
    return 40;
}

TapeRecorder::TapeRecorder()
{
    m_thread.startThread();
}

TapeRecorder::~TapeRecorder()
{
    stop();
    m_thread.stopThread(2000);
}

juce::File TapeRecorder::defaultFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDesktopDirectory);
}

juce::String TapeRecorder::extensionOf(FileType type) { return extensionFor(type); }

bool TapeRecorder::writeFile(const juce::File& file, const juce::AudioBuffer<float>& audio, int startSample, int numSamples,
                             double rate, const Options& options, const Naming& naming, juce::String& error)
{
    auto writer = createWriter(file, rate, audio.getNumChannels(), options, naming, error);
    if (writer == nullptr)
        return false;
    const bool ok = writer->writeFromAudioSampleBuffer(audio, startSample, numSamples);
    writer.reset(); // flushes the resampler and closes the file
    if (! ok)
    {
        error = "Writing " + file.getFileName() + " failed.";
        file.deleteFile();
        return false;
    }
    finishFile(file, options.type, naming, {});
    return true;
}

void TapeRecorder::setNaming(const Naming& naming)
{
    const juce::ScopedLock sl(m_metaLock);
    m_naming = naming;
}

TapeRecorder::Naming TapeRecorder::getNaming() const
{
    const juce::ScopedLock sl(m_metaLock);
    return m_naming;
}

juce::String TapeRecorder::markerLabel(int kind)
{
    switch (kind)
    {
        case MarkPassA: return "Pass A";
        case MarkPassB: return "Pass B";
        case MarkThrow: return "Throw";
        case MarkHit:   return "Hit";
        default:        return "Marker";
    }
}

juce::File TapeRecorder::nextFile(const juce::File& folder, const juce::String& suffix, const juce::String& extension)
{
    const juce::ScopedLock sl(m_metaLock);
    auto prefix = juce::File::createLegalFileName(m_naming.prefix.trim()).substring(0, 80).trim();
    if (prefix.isEmpty())
        prefix = "Swooshbuckler";
    if (m_naming.style == NameStyle::DateTime)
        // <Name>_YYYY-MM-DD_HH-MM-SS (getNonexistentChildFile numbers two in the same second apart).
        return folder.getNonexistentChildFile(prefix + "_" + juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H-%M-%S") + suffix, extension, false);
    // <Name>_NNNN: the next number of this session that no file in the folder has yet (whatever its type or suffix
    // would be, a number is used once). A file that is thrown away gives its number back.
    for (int n = m_counter;; ++n)
    {
        const auto stem = prefix + "_" + juce::String::formatted("%04d", n);
        const auto file = folder.getChildFile(stem + suffix + extension);
        if (! file.exists() || n > m_counter + 100000)
            return file;
    }
}

int TapeRecorder::numberOf(const juce::File& file) const
{
    // The NNNN of <Name>_NNNN[_suffix]: the first all-digit part after an underscore, from the right.
    const auto parts = juce::StringArray::fromTokens(file.getFileNameWithoutExtension(), "_", {});
    for (int i = parts.size(); --i >= 1;)
        if (parts[i].length() == 4 && parts[i].containsOnly("0123456789"))
            return parts[i].getIntValue();
    return 0;
}

std::vector<TapeRecorder::FinishedTake> TapeRecorder::takeFinishedTakes()
{
    const juce::ScopedLock sl(m_metaLock);
    std::vector<FinishedTake> out;
    out.swap(m_finished);
    return out;
}

juce::String TapeRecorder::takeWriteError()
{
    const juce::ScopedLock sl(m_metaLock);
    const auto error = m_writeError;
    m_writeError.clear();
    return error;
}

juce::String TapeRecorder::readDescription(const juce::File& file)
{
    if (file.hasFileExtension("flac"))
    {
        for (const auto& c : flacComments(file))
            if (c.startsWithIgnoreCase("DESCRIPTION="))
                return c.fromFirstOccurrenceOf("=", false, false);
        return {};
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor(file) })
        return file.hasFileExtension("wav") ? reader->metadataValues[juce::WavAudioFormat::bwavDescription]
                                            : reader->metadataValues["CueNote0Text"];
    return {};
}

std::vector<TapeRecorder::Cue> TapeRecorder::readCues(const juce::File& file)
{
    std::vector<Cue> cues;
    juce::WavAudioFormat wav;
    auto stream = std::make_unique<juce::FileInputStream>(file);
    if (! stream->openedOk())
        return cues;
    std::unique_ptr<juce::AudioFormatReader> reader { wav.createReaderFor(stream.release(), true) };
    if (reader == nullptr)
        return cues;
    const auto& meta = reader->metadataValues;
    const int count = meta["NumCuePoints"].getIntValue(), labels = meta["NumCueLabels"].getIntValue();
    for (int i = 0; i < count; ++i)
    {
        const auto prefix = "Cue" + juce::String(i);
        Cue cue;
        cue.frame = meta[prefix + "Offset"].getLargeIntValue();
        for (int k = 0; k < labels; ++k)
            if (meta["CueLabel" + juce::String(k) + "Identifier"] == meta[prefix + "Identifier"])
                cue.label = meta["CueLabel" + juce::String(k) + "Text"];
        cues.push_back(cue);
    }
    return cues;
}

bool TapeRecorder::pushCommand(const Command& command)
{
    // Markers leave room for the splits, which must not be lost.
    if (m_commandFifo.getFreeSpace() < (command.type == Command::Split ? 1 : 64))
        return false;
    const auto scope = m_commandFifo.write(1);
    if (scope.blockSize1 > 0)
        m_commands[static_cast<size_t>(scope.startIndex1)] = command;
    else if (scope.blockSize2 > 0)
        m_commands[static_cast<size_t>(scope.startIndex2)] = command;
    return true;
}

bool TapeRecorder::peekCommand(Command& command)
{
    if (m_commandFifo.getNumReady() <= 0)
        return false;
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    m_commandFifo.prepareToRead(1, start1, size1, start2, size2);
    command = m_commands[static_cast<size_t>(size1 > 0 ? start1 : start2)];
    return size1 + size2 > 0;
}

void TapeRecorder::popCommand()
{
    m_commandFifo.finishedRead(1);
}

void TapeRecorder::addMarker(int samplesFromNow, int kind)
{
    if (m_numPendingMarkers < static_cast<int>(m_pendingMarkers.size()))
        m_pendingMarkers[static_cast<size_t>(m_numPendingMarkers++)] = { juce::jmax(0, samplesFromNow), kind };
}

void TapeRecorder::closeWhoosh()
{
    // One File per Whoosh: the file ends here, and is kept if there was enough sound in it.
    if (m_perWhoosh.load() && m_activeWriter != nullptr)
        pushCommand({ m_streamPos, Command::Split,
                      static_cast<double>(m_whooshLoud.load()) >= c_minWhooshSeconds * m_recordRate.load() ? 1 : 0 });
}

void TapeRecorder::setRetroSeconds(int seconds)
{
    seconds = juce::jlimit(c_retroSeconds, c_maxRetroSeconds, seconds);
    if (seconds == m_retroSeconds.load())
        return;
    m_retroSeconds = seconds;
    // A bigger (or smaller) ring, made here and swapped in under the lock (the audio thread only try-locks it).
    juce::AudioBuffer<float> ring(m_numChannels, static_cast<int>(m_sampleRate * seconds) + 1);
    ring.clear();
    const juce::SpinLock::ScopedLockType lock(m_retroLock);
    m_retroGeneration.fetch_add(1);
    std::swap(m_retro, ring);
    if (m_retro.getNumSamples() != static_cast<int>(m_sampleRate * m_retroSeconds.load()) + 1 || m_retro.getNumChannels() != m_numChannels)
        m_retro.setSize(m_numChannels, static_cast<int>(m_sampleRate * m_retroSeconds.load()) + 1, false, true, false); // prepared meanwhile
    m_retroWrite = 0;
    m_retroTotal = 0;
}

void TapeRecorder::prepare(double sampleRate, int numChannels)
{
    m_preroll.setSize(juce::jmax(1, numChannels), juce::jmax(16, static_cast<int>(c_gatePrerollSeconds * sampleRate)));
    m_preroll.clear();
    m_fade.setSize(juce::jmax(1, numChannels), juce::jmax(8, static_cast<int>(c_gateFadeSeconds * sampleRate)));
    m_fade.clear();
    m_prerollWrite = m_prerollFill = 0;
    m_capturing = false;
    m_gateRestart = true;
    // A take was opened at the old rate / channel count (file header, resampler ratio). Close it and carry on in
    // a new take at the new rate rather than streaming new-rate audio into an old-rate file.
    const int newChannels = juce::jmax(1, numChannels);
    if (m_recording.load() && (std::abs(sampleRate - m_recordRate.load()) > 0.5 || newChannels != m_numChannels))
    {
        const auto folder = m_recordFolder;
        const auto options = m_recordOptions;
        stop();
        {
            const juce::SpinLock::ScopedLockType lock(m_retroLock);
            m_sampleRate = sampleRate;
            m_numChannels = newChannels;
        }
        juce::String error;
        start(folder, options, error);
    }

    const juce::SpinLock::ScopedLockType lock(m_retroLock);
    m_retroGeneration.fetch_add(1);
    m_sampleRate = sampleRate;
    m_numChannels = juce::jmax(1, numChannels);
    m_retro.setSize(m_numChannels, static_cast<int>(sampleRate * m_retroSeconds.load()) + 1, false, true, false);
    m_retro.clear();
    m_retroWrite = 0;
    m_retroTotal = 0;
}

void TapeRecorder::beginTakeIfNew()
{
    // A take opened since the last look: its stream starts at 0, and the last take's levels are forgotten.
    if (m_streamRestart.exchange(false))
    {
        m_streamPos = 0;
        m_whooshLoud = 0;
    }
    if (m_gateRestart.exchange(false))
    {
        m_gatePeak = 0.0f;
        m_gatePeakHold = 0;
        m_hangLeft = 0;
        m_fadeInLeft = 0;
        m_prerollFill = 0;
        m_capturing = false;
    }
}

bool TapeRecorder::rest()
{
    const juce::ScopedTryLock lock(m_writerLock);
    if (! lock.isLocked())
        return false;
    // A take opened while the last one was still sounding, and the rig came to rest before any of it was fed:
    // there is no whoosh of this take to close (a split queued here would carry the last take's position).
    beginTakeIfNew();
    // What reached the tape last was already silent (the transport had faded out), so there is nothing to fade.
    if (m_capturing.load())
        closeWhoosh();
    m_numPendingMarkers = 0;
    m_capturing = false;
    m_hangLeft = 0;
    m_fadeInLeft = 0;
    m_prerollFill = 0;
    return true;
}

void TapeRecorder::process(const float* const* channels, int numChannels, int numSamples)
{
    // Rolling tape
    {
        // Where this block went in the take's stream (for the markers): its first sample, and how many of its
        // samples, from the first on, were written.
        std::int64_t blockBase = 0;
        int blockCount = 0;
        bool whooshEnded = false; // the gate closed in this block: the file ends after the block's markers
        const juce::ScopedTryLock lock(m_writerLock);
        if (lock.isLocked() && m_activeWriter != nullptr)
        {
            auto writeBlock = [this](const float* const* data, int n)
            {
                if (m_activeWriter->write(data, n))
                {
                    m_samplesWritten += n;
                    m_streamPos += n;
                }
                else
                    m_droppedSamples += n; // FIFO full (disk too slow): reported to the UI
            };

            beginTakeIfNew();

            if (! m_signalGate.load() && ! m_perWhoosh.load())
            {
                blockBase = m_streamPos;
                blockCount = numSamples;
                writeBlock(channels, numSamples);
                m_capturing = false; // Skip Silence switched on mid-take starts from the next sound
                m_prerollFill = 0;
                m_fadeInLeft = 0;
            }
            else
            {
                // Skip Silence: write only while there is sound (pre-roll in front, a hang behind, see the header).
                float peak = 0.0f;
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < numSamples; ++i)
                        peak = juce::jmax(peak, std::abs(channels[ch][i]));

                // The recent peak: up at once, remembered for a while, then down towards the level now.
                if (peak >= m_gatePeak)
                {
                    m_gatePeak = peak;
                    m_gatePeakHold = static_cast<int>(c_gatePeakHoldSeconds * m_sampleRate);
                }
                else if (m_gatePeakHold > 0)
                    m_gatePeakHold -= numSamples;
                else
                    m_gatePeak *= std::exp(-0.1151293f * c_gatePeakFallDbPerSecond * static_cast<float>(numSamples)
                                           / static_cast<float>(m_sampleRate));
                const float line = juce::jmax(c_gateFloor, m_gatePeak * juce::Decibels::decibelsToGain(-c_gateDepthDb));
                const bool loud = peak > line;

                const int prerollSize = m_preroll.getNumSamples();
                const int fadeLen = m_fade.getNumSamples();
                auto keepForPreroll = [&](int from, int count)
                {
                    for (int i = from; i < from + count; ++i)
                    {
                        for (int ch = 0; ch < m_preroll.getNumChannels(); ++ch)
                            m_preroll.setSample(ch, m_prerollWrite, channels[juce::jmin(ch, numChannels - 1)][i]);
                        m_prerollWrite = (m_prerollWrite + 1) % prerollSize;
                    }
                    m_prerollFill = juce::jmin(prerollSize, m_prerollFill + count);
                };

                if (loud && ! m_capturing.load() && m_prerollFill > 0)
                {
                    // Opening: the pre-roll first, faded in so the join with what was written before is clean.
                    const int first = (m_prerollWrite - m_prerollFill + prerollSize) % prerollSize;
                    const int fadeIn = juce::jmin(fadeLen, m_prerollFill);
                    for (int ch = 0; ch < m_preroll.getNumChannels(); ++ch)
                        for (int i = 0; i < fadeIn; ++i)
                        {
                            const int at = (first + i) % prerollSize;
                            m_preroll.setSample(ch, at, m_preroll.getSample(ch, at) * static_cast<float>(i) / static_cast<float>(fadeIn));
                        }
                    int read = first;
                    int left = m_prerollFill;
                    while (left > 0)
                    {
                        const int n = juce::jmin(left, prerollSize - read);
                        const float* seg[8] {};
                        for (int ch = 0; ch < juce::jmin(8, m_preroll.getNumChannels()); ++ch)
                            seg[ch] = m_preroll.getReadPointer(ch, read);
                        writeBlock(seg, n);
                        read = (read + n) % prerollSize;
                        left -= n;
                    }
                    m_prerollFill = 0;
                }
                if (loud || (m_capturing.load() && m_gateHold.load(std::memory_order_relaxed)))
                {
                    // Sound again while the closing fade was under way: come back up from where it had got to.
                    if (m_capturing.load() && m_hangLeft < fadeLen)
                        m_fadeInLeft = juce::jmax(m_fadeInLeft, fadeLen - m_hangLeft);
                    if (! m_capturing.load())
                        m_whooshLoud = 0; // a new stretch of sound
                    if (loud)
                        m_whooshLoud += numSamples;
                    m_capturing = true;
                    m_hangLeft = static_cast<int>((c_gateQuietSeconds + c_gateHangSeconds) * m_sampleRate);
                }

                blockBase = m_streamPos;
                if (m_capturing.load())
                {
                    // The last fadeLen samples of the hang fade out; whatever is left of the block after the
                    // gate has closed is the start of the next pre-roll.
                    const int count = juce::jmin(numSamples, m_hangLeft);
                    blockCount = count;
                    const int plain = juce::jlimit(0, count, m_hangLeft - fadeLen);
                    // (the hang is far longer than two fades, so a fade back in is over before the next fade out)
                    const int rising = juce::jmin(plain, m_fadeInLeft);
                    if (rising > 0)
                    {
                        const float* seg[8] {};
                        for (int ch = 0; ch < juce::jmin(8, m_fade.getNumChannels()); ++ch)
                        {
                            const float* in = channels[juce::jmin(ch, numChannels - 1)];
                            float* out = m_fade.getWritePointer(ch);
                            for (int i = 0; i < rising; ++i)
                                out[i] = in[i] * static_cast<float>(fadeLen - m_fadeInLeft + i) / static_cast<float>(fadeLen);
                            seg[ch] = out;
                        }
                        writeBlock(seg, rising);
                        m_fadeInLeft -= rising;
                    }
                    if (plain > rising)
                    {
                        const float* seg[8] {};
                        for (int ch = 0; ch < juce::jmin(8, numChannels); ++ch)
                            seg[ch] = channels[ch] + rising;
                        writeBlock(seg, plain - rising);
                    }
                    if (count > plain)
                    {
                        const float* seg[8] {};
                        for (int ch = 0; ch < juce::jmin(8, m_fade.getNumChannels()); ++ch)
                        {
                            const float* in = channels[juce::jmin(ch, numChannels - 1)];
                            float* out = m_fade.getWritePointer(ch);
                            for (int i = plain; i < count; ++i)
                                out[i - plain] = in[i] * static_cast<float>(m_hangLeft - i) / static_cast<float>(fadeLen);
                            seg[ch] = out;
                        }
                        writeBlock(seg, count - plain);
                    }
                    m_hangLeft -= count;
                    if (m_hangLeft <= 0)
                    {
                        whooshEnded = true;
                        m_capturing = false;
                        m_fadeInLeft = 0;
                        m_prerollFill = 0;
                        keepForPreroll(count, numSamples - count);
                    }
                }
                else
                    keepForPreroll(0, numSamples);
            }
        }

        // Markers that fall in this block: into the take if the tape wrote that sample. (Only with the writer in
        // hand: blockCount is 0 otherwise, and nothing is queued.)
        int kept = 0;
        for (int i = 0; i < m_numPendingMarkers; ++i)
        {
            auto marker = m_pendingMarkers[static_cast<size_t>(i)];
            if (marker.due >= numSamples)
            {
                marker.due -= numSamples;
                m_pendingMarkers[static_cast<size_t>(kept++)] = marker;
            }
            else if (marker.due < blockCount)
                pushCommand({ blockBase + marker.due, Command::Marker, marker.kind });
        }
        m_numPendingMarkers = kept;
        // After the markers, so the commands stay in the order of their place in the stream and a marker in the
        // last block of a whoosh goes into its file.
        if (whooshEnded)
            closeWhoosh();
    }

    // Retro ring
    const juce::SpinLock::ScopedTryLockType lock(m_retroLock);
    if (! lock.isLocked() || m_retro.getNumSamples() <= 1)
        return;

    const int size = m_retro.getNumSamples();
    int write = m_retroWrite.load();
    const int chans = juce::jmin(numChannels, m_retro.getNumChannels());
    int done = 0;
    while (done < numSamples)
    {
        const int n = juce::jmin(numSamples - done, size - write);
        for (int ch = 0; ch < m_retro.getNumChannels(); ++ch)
            m_retro.copyFrom(ch, write, channels[juce::jmin(ch, chans - 1)] + done, n);
        write = (write + n) % size;
        done += n;
    }
    m_retroWrite = write;
    m_retroTotal += numSamples;
}

bool TapeRecorder::start(const juce::File& folder, int bitDepth, juce::String& error, double fileRate)
{
    Options o;
    o.bitDepth = bitDepth;
    o.fileRate = fileRate;
    return start(folder, o, error);
}

bool TapeRecorder::start(const juce::File& folder, const Options& options, juce::String& error)
{
    stop();

    if (! folder.isDirectory() && ! folder.createDirectory())
    {
        error = "Couldn't create the folder " + folder.getFullPathName();
        return false;
    }

    {
        const juce::ScopedLock sl(m_fileLock);
        m_currentFile = juce::File();
        m_lastFile = juce::File(); // until this take has kept a file
    }
    // One take, one file: opened now, so a folder that can't be written says so at once. One File per Whoosh
    // opens each file when its first sound arrives.
    auto writer = std::make_unique<Sink>(*this, folder, options, m_sampleRate, m_numChannels);
    if (! options.perWhoosh && ! writer->open(error))
        return false;
    if (options.perWhoosh && ! folder.hasWriteAccess())
    {
        error = "Can't write to " + folder.getFullPathName();
        return false;
    }

    m_samplesWritten = 0;
    m_droppedSamples = 0;
    m_recordRate = m_sampleRate;
    m_recordFolder = folder;
    m_recordOptions = options;
    // The FIFO is sized by time, so a slow disk has the same grace at 192 kHz as at 48 kHz.
    m_commandFifo.reset(); // nothing reads or writes it now: the last take's writer has gone, and there is no active one
    m_sink = writer.get();
    m_writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), m_thread, getFifoSamples());
    m_thread.addTimeSliceClient(&m_pump);
    m_gateRestart = true;
    m_streamRestart = true;
    m_perWhoosh = options.perWhoosh;

    const juce::ScopedLock lock(m_writerLock);
    m_activeWriter = m_writer.get();
    m_recording = true;
    return true;
}

void TapeRecorder::stop()
{
    {
        const juce::ScopedLock lock(m_writerLock);
        m_activeWriter = nullptr;
    }

    if (m_writer != nullptr)
    {
        m_thread.removeTimeSliceClient(&m_pump); // the writer thread lets go of the sink first
        m_writer.reset(); // flushes the FIFO; the sink closes its file and notes it as the last file if it is kept
        m_sink = nullptr;
    }
    m_recording = false;
}

double TapeRecorder::getRecordedSeconds() const
{
    return static_cast<double>(m_samplesWritten.load()) / juce::jmax(1.0, m_recordRate.load());
}

bool TapeRecorder::saveRetro(const juce::File& folder, int bitDepth, juce::File& written, juce::String& error, double fileRate)
{
    Options o;
    o.bitDepth = bitDepth;
    o.fileRate = fileRate;
    return saveRetro(folder, o, written, error);
}

bool TapeRecorder::saveRetro(const juce::File& folder, const Options& options, juce::File& written, juce::String& error)
{
    juce::AudioBuffer<float> copy;
    int available = 0;
    double rate = 48000.0;
    {
        // Snapshot. The audio thread keeps writing meanwhile; we skip the oldest half second, which is the only
        // part that can be overwritten during the copy. prepare() may reallocate the ring from another thread,
        // so every read of it happens under m_retroLock, in small pieces (the audio thread only try-locks and at
        // worst skips one block of the retro ring), and the copy is abandoned if the ring was re-prepared.
        int size = 0, write = 0, generation = 0, channels = 0;
        std::int64_t total = 0;
        {
            const juce::SpinLock::ScopedLockType lock(m_retroLock);
            size = m_retro.getNumSamples();
            channels = m_retro.getNumChannels();
            total = m_retroTotal.load();
            write = m_retroWrite.load();
            rate = m_sampleRate;
            generation = m_retroGeneration.load();
        }
        if (size <= 1 || channels <= 0)
        {
            error = "Nothing in the retro buffer yet.";
            return false;
        }
        available = static_cast<int>(juce::jmin<std::int64_t>(total, size - 1));
        const int margin = juce::jmin(available, static_cast<int>(rate * 0.5));
        available -= margin;
        if (available <= static_cast<int>(rate * 0.1))
        {
            error = "Nothing in the retro buffer yet.";
            return false;
        }

        copy.setSize(channels, available);
        int read = ((write - available) % size + size) % size;
        int done = 0;
        constexpr int c_piece = 8192;
        while (done < available)
        {
            const int n = juce::jmin(available - done, size - read, c_piece);
            {
                const juce::SpinLock::ScopedLockType lock(m_retroLock);
                if (m_retroGeneration.load() != generation || m_retro.getNumSamples() != size)
                {
                    error = "The audio settings changed while saving. Try again.";
                    return false;
                }
                for (int ch = 0; ch < copy.getNumChannels(); ++ch)
                    copy.copyFrom(ch, done, m_retro, ch, read, n);
            }
            read = (read + n) % size;
            done += n;
        }
    }

    // Trim leading silence so the file starts at the first sound.
    int first = 0;
    for (; first < copy.getNumSamples(); ++first)
    {
        bool loud = false;
        for (int ch = 0; ch < copy.getNumChannels(); ++ch)
            loud = loud || std::abs(copy.getSample(ch, first)) > 1.0e-4f;
        if (loud) break;
    }
    if (first >= copy.getNumSamples())
    {
        error = "There was only silence to save.";
        return false;
    }

    if (! folder.isDirectory() && ! folder.createDirectory())
    {
        error = "Couldn't create the folder " + folder.getFullPathName();
        return false;
    }

    const auto naming = getNaming();
    written = nextFile(folder, "_retro", extensionFor(options.type));
    auto writer = createWriter(written, rate, copy.getNumChannels(), options, naming, error);
    if (writer == nullptr)
        return false;

    if (! writer->writeFromAudioSampleBuffer(copy, first, copy.getNumSamples() - first))
    {
        error = "Writing " + written.getFileName() + " failed.";
        return false;
    }
    writer.reset(); // flushes the resampler and closes the file
    finishFile(written, options.type, naming, {});
    if (naming.style == NameStyle::Counter)
    {
        const juce::ScopedLock sl(m_metaLock);
        m_counter = juce::jmax(m_counter, numberOf(written) + 1);
    }
    const juce::ScopedLock sl(m_fileLock);
    m_lastFile = written;
    return true;
}

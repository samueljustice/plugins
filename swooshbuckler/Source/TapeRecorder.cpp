#include "TapeRecorder.h"
#include "MasterProcessor.h"

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
                // Mono fold-down of Stereo: plain average (keeps level-matched mono, no clipping). A wider take is
                // folded before it gets here (TakeWriter), so this is never an average of more than two channels.
                jassert(m_inputChannels == 2);
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

            std::array<const float*, room::c_maxChannels> chans {};
            for (int ch = 0; ch < juce::jmin(room::c_maxChannels, m_out.getNumChannels()); ++ch)
                chans[static_cast<size_t>(ch)] = m_out.getReadPointer(ch, offset);
            m_written += count;
            return m_inner->writeFromFloatArrays(chans.data(), m_out.getNumChannels(), count);
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

    /** A BWF description holds 256 bytes: the text up to the last whole sentence or list item that fits, ended with
        a full stop, so it never stops mid-word. (The whole text goes into LIST INFO as well.) */
    juce::String bextDescription(const juce::String& text)
    {
        constexpr size_t c_bextBytes = 256;
        if (text.getNumBytesAsUTF8() <= c_bextBytes)
            return text;
        juce::String best;
        for (int i = 1; i < text.length(); ++i)
            if ((text[i] == '.' || text[i] == ',') && (i + 1 == text.length() || text[i + 1] == ' '))
            {
                auto candidate = text.substring(0, i) + ".";
                if (candidate.getNumBytesAsUTF8() > c_bextBytes)
                    break;
                best = std::move(candidate);
            }
        if (best.isNotEmpty())
            return best;
        // One long clause: up to the last space that fits.
        auto cut = text;
        while (cut.getNumBytesAsUTF8() > c_bextBytes)
            cut = cut.substring(0, cut.length() - 1);
        const int space = cut.lastIndexOfChar(' ');
        return space > 0 ? cut.substring(0, space) : cut;
    }

    /** channelLine: the file's channels by name, in file order ("L R C LFE Ls Rs"), for files wider than Stereo
        and each file of a Multi-Mono set; empty for the rest. */
    std::unique_ptr<juce::AudioFormatWriter> createWriter(const juce::File& file, double inputRate, int inputChannels,
                                                          const TapeRecorder::Options& o, const TapeRecorder::Naming& naming,
                                                          const juce::String& channelLine, juce::String& error)
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
                if (channelLine.isNotEmpty())
                {
                    metadata.set("NumCueNotes", "3");
                    metadata.set("CueNote2TimeStamp", "0");
                    metadata.set("CueNote2Identifier", "0");
                    metadata.set("CueNote2Text", "Channels: " + channelLine);
                }
                break;
            case TapeRecorder::FileType::Flac:
                format = std::make_unique<juce::FlacAudioFormat>(); // its Vorbis comments are written once it is closed (TakeWriter::close)
                break;
            case TapeRecorder::FileType::Wav:
            default:
                // WAV: BWF bext (description, originator) and the whole description as LIST INFO, which more programs
                // show. The channels are named in an iXML chunk added when the file is closed (TakeWriter::close).
                format = std::make_unique<juce::WavAudioFormat>();
                metadata = juce::WavAudioFormat::createBWAVMetadata(
                    bextDescription(naming.description), naming.creator, "Swooshbuckler", juce::Time::getCurrentTime(), 0,
                    "A=PCM,F=" + juce::String(juce::roundToInt(fileRate)) + ",W=" + juce::String(bits)
                        + ",M=" + (channels == 1 ? juce::String("mono") : channels == 2 ? juce::String("stereo") : juce::String("multichannel"))
                        + ",T=Swooshbuckler");
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

    /** Appends a marker chunk to a closed AIFF file (a chunk after the sound is part of the format). */
    bool appendAiffMarkers(const juce::File& file, const std::vector<TapeRecorder::Cue>& cues)
    {
        if (cues.empty())
            return true;
        const auto size = file.getSize();
        {
            juce::FileInputStream in(file);
            char tag[4] {};
            if (! in.openedOk() || in.read(tag, 4) != 4 || std::memcmp(tag, "FORM", 4) != 0)
                return false;
        }
        const auto count = juce::jmin(static_cast<int>(cues.size()), 0x7fff);
        juce::MemoryOutputStream body;
        body.writeShortBigEndian(static_cast<short>(count));
        for (int i = 0; i < count; ++i)
        {
            // A name is a count byte and up to 255 bytes, padded so the two together are an even number of bytes.
            const auto& cue = cues[static_cast<size_t>(i)];
            auto text = cue.label;
            while (text.getNumBytesAsUTF8() > 255)
                text = text.dropLastCharacters(1);
            const auto bytes = static_cast<int>(text.getNumBytesAsUTF8());
            body.writeShortBigEndian(static_cast<short>(i + 1));
            body.writeIntBigEndian(static_cast<int>(static_cast<std::uint32_t>(juce::jlimit<std::int64_t>(0, 0xffffffffLL, cue.frame))));
            body.writeByte(static_cast<char>(bytes));
            body.write(text.toRawUTF8(), static_cast<size_t>(bytes));
            if ((bytes & 1) == 0)
                body.writeByte(0);
        }
        juce::MemoryOutputStream chunk;
        if ((size & 1) != 0)
            chunk.writeByte(0);
        chunk.write("MARK", 4);
        chunk.writeIntBigEndian(static_cast<int>(body.getDataSize()));
        chunk.write(body.getData(), body.getDataSize());
        if ((body.getDataSize() & 1) != 0)
            chunk.writeByte(0);

        const auto newSize = size + static_cast<juce::int64>(chunk.getDataSize());
        if (newSize - 8 > 0xffffffffLL)
            return false;
        juce::FileOutputStream out(file); // opens at the end of the file
        if (! out.openedOk() || ! out.setPosition(size) || ! out.write(chunk.getData(), chunk.getDataSize()))
            return false;
        if (! out.setPosition(4))
            return false;
        out.writeIntBigEndian(static_cast<int>(static_cast<std::uint32_t>(newSize - 8)));
        out.flush();
        return true;
    }

    /** "0:07.250": where a marker is, for a file type that can only say so in words. */
    juce::String markerTime(std::int64_t frame, double rate)
    {
        const double seconds = static_cast<double>(frame) / juce::jmax(1.0, rate);
        const int minutes = static_cast<int>(seconds / 60.0);
        return juce::String(minutes) + ":" + juce::String(seconds - 60.0 * minutes, 3).paddedLeft('0', 6);
    }

    /** The pointers of an AudioBuffer's channels from a sample on. */
    std::array<const float*, room::c_maxChannels> pointersOf(const juce::AudioBuffer<float>& audio, int startSample)
    {
        std::array<const float*, room::c_maxChannels> p {};
        for (int ch = 0; ch < juce::jmin(room::c_maxChannels, audio.getNumChannels()); ++ch)
            p[static_cast<size_t>(ch)] = audio.getReadPointer(ch, startSample);
        return p;
    }
}

//==============================================================================
/**
 * One take on its way to disk: an interleaved file in the chosen Channel Order, or a Multi-Mono set, from audio in
 * processing order. Mono of a take wider than Stereo is folded here (room::fold) and held under the Ceiling by a
 * limiter whose look-ahead is taken back out, so the file lines up with the take. On close the files get their
 * speaker mask (WAV), channel names, description and markers; a take that is not kept, or that failed anywhere, is
 * deleted whole, every file of it. Never the audio thread.
 */
class TapeRecorder::TakeWriter
{
public:
    ~TakeWriter()
    {
        if (! m_closed)
            discard();
    }

    bool open(const juce::File& file, double inputRate, int inputChannels, const Options& options, const Naming& naming, juce::String& error)
    {
        m_options = options;
        m_naming = naming;
        m_inputChannels = inputChannels;
        m_fileRate = options.fileRate > 0.0 ? options.fileRate : inputRate;
        const auto width = takefile::widthForChannels(inputChannels);
        if (! width.has_value())
        {
            error = "A take has 1, 2, 4, 6, 8, 10 or 12 channels, not " + juce::String(inputChannels) + ".";
            return false;
        }
        m_inWidth = *width;
        const auto fileWidth = fileWidthOf(options, inputChannels);
        // A FLAC file is in FLAC's own order whatever Channel Order says (its format fixes what each channel is).
        m_format = { fileWidth, takefile::orderFor(containerOf(options.type), options.order),
                     fileWidth == room::Width::Mono ? takefile::Layout::Interleaved : options.layout };
        const bool multi = m_format.layout == takefile::Layout::MultiMono;
        const int fileChannels = room::layout(fileWidth).numChannels;
        if (const auto problem = formatProblem(options, inputChannels); problem.isNotEmpty())
        {
            error = problem;
            return false;
        }
        m_files = takefile::filesOf(file, fileWidth, m_format.layout);
        // A fold-down says so in every file of it.
        m_foldedFrom = TapeRecorder::foldedFrom(options, inputChannels);
        if (m_foldedFrom.has_value())
            m_naming.description = foldNote(*m_foldedFrom, fileWidth) + m_naming.description;

        // A take wider than Stereo folded to Mono or Stereo: room::fold, then a limiter linked across its channels.
        m_fold = fileWidth != m_inWidth && inputChannels > 2;
        m_foldChannels = fileChannels;
        if (m_fold)
        {
            m_limiter.prepare(inputRate, m_foldChannels);
            MasterProcessor::Parameters p;
            p.limiterOn = true;
            p.ceilingDb = juce::jlimit(-12.0f, 0.0f, options.foldCeilingDb);
            m_limiter.setParameters(p);
            m_skip = m_limiter.getLatencySamples();
        }

        // Each writer: the take's channels in file order, one channel of a Multi-Mono set, or the fold.
        Options each = options;
        each.mono = fileWidth == room::Width::Mono && ! m_fold; // Stereo to Mono stays the writer's own average
        const int inputs = multi ? 1 : m_fold ? m_foldChannels : inputChannels;
        for (int i = 0; i < m_files.size(); ++i)
        {
            juce::String line;
            if (multi)
                line = room::info(room::layout(fileWidth).speakers[static_cast<size_t>(i)]).name;
            else if (fileChannels > 2)
                line = takefile::channelLine(fileWidth, m_format.order);
            auto writer = createWriter(m_files[i], inputRate, inputs, each, m_naming, line, error);
            if (writer == nullptr)
            {
                discard();
                return false;
            }
            m_writers.push_back(std::move(writer));
        }
        for (int i = 0; i < room::c_maxChannels; ++i)
            m_order[static_cast<size_t>(i)] = i;
        if (! multi && fileWidth == m_inWidth)
            m_order = room::fileChannels(m_inWidth, m_format.order);
        m_closed = false;
        return true;
    }

    /** numSamples of every input channel, in processing order. */
    bool write(const float* const* channels, int numSamples)
    {
        if (m_closed || numSamples <= 0)
            return m_ok;
        if (m_fold)
        {
            m_buffer.setSize(m_foldChannels, numSamples, false, false, true);
            const auto& fold = room::fold(m_inWidth, m_format.width);
            for (int out = 0; out < m_foldChannels; ++out)
            {
                float* to = m_buffer.getWritePointer(out);
                juce::FloatVectorOperations::clear(to, numSamples);
                for (int ch = 0; ch < m_inputChannels; ++ch)
                    if (const float g = fold.gain[static_cast<size_t>(out)][static_cast<size_t>(ch)]; g != 0.0f)
                        juce::FloatVectorOperations::addWithMultiply(to, channels[ch], g, numSamples);
            }
            writeFolded(numSamples);
        }
        else if (m_format.layout == takefile::Layout::MultiMono)
        {
            for (size_t ch = 0; ch < m_writers.size(); ++ch)
                m_ok = m_writers[ch]->writeFromFloatArrays(&channels[ch], 1, numSamples) && m_ok;
        }
        else
        {
            std::array<const float*, room::c_maxChannels> inFileOrder {};
            for (int i = 0; i < m_inputChannels; ++i)
                inFileOrder[static_cast<size_t>(i)] = channels[m_order[static_cast<size_t>(i)]];
            m_ok = m_writers.front()->writeFromFloatArrays(inFileOrder.data(), m_inputChannels, numSamples) && m_ok;
        }
        return m_ok;
    }

    /**
     * Ends the take. keep false, or a write that failed: every file of it is deleted (keepPartial keeps an
     * interleaved file that was written up to a failure, as the rolling tape always has; a Multi-Mono set is never
     * kept in part). Otherwise each file gets its mask, channel names and description, and the markers: as WAV cue
     * points always, and when allMarkers also as AIFF markers and FLAC comments. markersAdded is false if a WAV or
     * AIFF file had no room for them (4 GB). Returns whether the take was kept.
     */
    bool close(bool keep, const std::vector<Cue>& markers, bool allMarkers, bool& markersAdded, bool keepPartial = false)
    {
        markersAdded = true;
        if (m_closed)
            return false;
        if (m_fold && m_ok)
        {
            // The limiter's look-ahead still holds the end of the take.
            const int tail = m_limiter.getLatencySamples();
            m_buffer.setSize(m_foldChannels, juce::jmax(1, tail), false, false, true);
            m_buffer.clear();
            writeFolded(tail);
        }
        m_writers.clear(); // flushes the resamplers and closes the files
        m_closed = true;
        const bool multi = m_format.layout == takefile::Layout::MultiMono;
        if (! keep || (! m_ok && (multi || ! keepPartial)))
        {
            deleteFiles();
            return false;
        }
        const auto& l = room::layout(m_format.width);
        const int fileChannels = l.numChannels;
        for (int i = 0; i < m_files.size(); ++i)
        {
            const auto& file = m_files[i];
            const juce::String line = multi ? juce::String(room::info(l.speakers[static_cast<size_t>(i)]).name)
                                            : fileChannels > 2 ? takefile::channelLine(m_format.width, m_format.order) : juce::String();
            switch (m_options.type)
            {
                case FileType::Flac:
                {
                    juce::StringArray comments { "DESCRIPTION=" + m_naming.description, "ARTIST=" + m_naming.creator, "ENCODER=" + m_naming.software };
                    if (line.isNotEmpty())
                        comments.add("CHANNELS=" + line);
                    if (! multi && fileChannels > 2)
                        comments.add("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x"
                                     + juce::String::toHexString(static_cast<int>(takefile::maskFor(m_format.width, m_format.order).value_or(0u))).toUpperCase());
                    if (allMarkers)
                        for (const auto& marker : markers)
                            comments.add("MARKER=" + markerTime(marker.frame, m_fileRate) + " " + marker.label);
                    takefile::setFlacComments(file, comments);
                    break;
                }
                case FileType::Aiff:
                {
                    // Core Audio's channel layout for every file that has speakers to name.
                    juce::Array<room::Speaker> speakers;
                    if (multi)
                        speakers.add(l.speakers[static_cast<size_t>(i)]);
                    else if (fileChannels > 2)
                        for (int ch = 0; ch < fileChannels; ++ch)
                            speakers.add(l.speakers[static_cast<size_t>(room::fileChannels(m_format.width, m_format.order)[static_cast<size_t>(ch)])]);
                    if (! speakers.isEmpty())
                        takefile::appendAiffChannelLayout(file, speakers);
                    if (allMarkers)
                        markersAdded = appendAiffMarkers(file, markers) && markersAdded;
                    break;
                }
                case FileType::Wav:
                default:
                {
                    // The format chunk as Pro Tools writes this width (a mask, or a plain header at 7.1.4 Like Pro
                    // Tools), then the channels named in iXML after the sound, as Pro Tools puts it.
                    if (! multi && fileChannels > 2 && ! takefile::setWavChannelMask(file, takefile::maskFor(m_format.width, m_format.order)))
                    {
                        deleteFiles();
                        return false;
                    }
                    markersAdded = appendWavCues(file, markers) && markersAdded;
                    const auto ixml = takefile::ixmlFor(m_format.width, m_format.order,
                                                        multi ? file.getFileNameWithoutExtension().upToLastOccurrenceOf(".", false, false)
                                                              : file.getFileNameWithoutExtension(),
                                                        multi ? i : -1);
                    takefile::appendWavChunk(file, "iXML", ixml.toRawUTF8(), ixml.getNumBytesAsUTF8());
                    break;
                }
            }
        }
        return true;
    }

    /** Closes and deletes every file of the take. */
    void discard()
    {
        m_writers.clear();
        m_closed = true;
        deleteFiles();
    }

    const juce::Array<juce::File>& files() const { return m_files; }
    const takefile::Format& format() const { return m_format; }
    std::optional<room::Width> foldedFrom() const { return m_foldedFrom; }

private:
    void writeFolded(int numSamples)
    {
        std::array<float*, room::c_maxChannels> chans {};
        for (int ch = 0; ch < m_foldChannels; ++ch)
            chans[static_cast<size_t>(ch)] = m_buffer.getWritePointer(ch);
        m_limiter.process(chans.data(), m_foldChannels, numSamples);
        // The first look-ahead's worth out of the limiter is the silence it started with.
        const int skip = static_cast<int>(juce::jmin<std::int64_t>(m_skip, numSamples));
        m_skip -= skip;
        if (numSamples <= skip)
            return;
        std::array<const float*, room::c_maxChannels> from {};
        for (int ch = 0; ch < m_foldChannels; ++ch)
            from[static_cast<size_t>(ch)] = chans[static_cast<size_t>(ch)] + skip;
        if (m_format.layout == takefile::Layout::MultiMono)
            for (size_t ch = 0; ch < m_writers.size(); ++ch)
                m_ok = m_writers[ch]->writeFromFloatArrays(&from[ch], 1, numSamples - skip) && m_ok;
        else
            m_ok = m_writers.front()->writeFromFloatArrays(from.data(), m_foldChannels, numSamples - skip) && m_ok;
    }

    void deleteFiles()
    {
        for (const auto& f : m_files)
            f.deleteFile();
    }

    Options m_options;
    Naming m_naming;
    takefile::Format m_format;
    room::Width m_inWidth = room::Width::Stereo;
    int m_inputChannels = 2;
    double m_fileRate = 48000.0;
    juce::Array<juce::File> m_files;
    std::vector<std::unique_ptr<juce::AudioFormatWriter>> m_writers;
    std::array<int, room::c_maxChannels> m_order {};
    bool m_fold = false;
    int m_foldChannels = 1;
    std::optional<room::Width> m_foldedFrom;
    MasterProcessor m_limiter;
    std::int64_t m_skip = 0;
    juce::AudioBuffer<float> m_buffer;
    bool m_ok = true;
    bool m_closed = true;
};

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
        auto writer = std::make_unique<TakeWriter>();
        if (! writer->open(m_owner.nextFile(m_folder, {}, extensionFor(m_options.type)), getSampleRate(),
                           static_cast<int>(getNumChannels()), m_options, m_naming, error))
            return false;
        m_inner = std::move(writer);
        m_fileStart = m_consumed;
        m_marks.clear();
        const juce::ScopedLock sl(m_owner.m_fileLock);
        m_owner.m_currentFile = m_inner->files()[0];
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
                std::array<const float*, room::c_maxChannels> chans {};
                const int channels = juce::jmin(room::c_maxChannels, static_cast<int>(getNumChannels()));
                for (int ch = 0; ch < channels; ++ch)
                    chans[static_cast<size_t>(ch)] = floats[ch] + done;
                m_inner->write(chans.data(), count);
            }
            m_consumed += count;
            m_owner.m_takenSamples.fetch_add(count, std::memory_order_relaxed);
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
        const auto samples = m_consumed - m_fileStart;
        if (! keep || samples <= 0)
        {
            m_inner->discard(); // every file of it
            m_inner.reset();
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
        bool markersAdded = true;
        const bool kept = m_inner->close(true, cues, false, markersAdded, true); // flushes the resampler, closes the files
        const auto files = m_inner->files();
        const auto format = m_inner->format();
        const auto folded = m_inner->foldedFrom();
        m_inner.reset();
        if (! kept)
        {
            const juce::ScopedLock sl(m_owner.m_metaLock);
            m_owner.m_writeError = "Writing " + files[0].getFileName() + " failed.";
            return;
        }

        {
            const juce::ScopedLock sl(m_owner.m_fileLock);
            m_owner.m_lastFile = files[0];
        }
        const juce::ScopedLock sl(m_owner.m_metaLock);
        FinishedTake take;
        take.file = files[0];
        take.files = files;
        take.format = format;
        take.foldedFrom = folded;
        take.seconds = static_cast<double>(samples) / getSampleRate();
        take.markers = m_options.type == FileType::Wav ? static_cast<int>(cues.size()) : 0;
        take.whoosh = m_options.perWhoosh;
        m_owner.m_finished.push_back(take);
        if (m_naming.style == NameStyle::Counter)
            m_owner.m_counter = juce::jmax(m_owner.m_counter, m_owner.numberOf(files[0]) + 1);
    }

    TapeRecorder& m_owner;
    juce::File m_folder;
    Options m_options;
    Naming m_naming;
    std::unique_ptr<TakeWriter> m_inner;
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

takefile::Container TapeRecorder::containerOf(FileType type)
{
    return type == FileType::Aiff ? takefile::Container::Aiff : type == FileType::Flac ? takefile::Container::Flac : takefile::Container::Wav;
}

bool TapeRecorder::writeFile(const juce::File& file, const juce::AudioBuffer<float>& audio, int startSample, int numSamples,
                             double rate, const Options& options, const Naming& naming, juce::String& error,
                             juce::Array<juce::File>* written, const std::vector<Cue>& cues)
{
    TakeWriter writer;
    if (! writer.open(file, rate, audio.getNumChannels(), options, naming, error))
        return false;
    const auto channels = pointersOf(audio, startSample);
    constexpr int c_piece = 65536;
    bool ok = true;
    for (int done = 0; done < numSamples && ok; done += c_piece)
    {
        std::array<const float*, room::c_maxChannels> at {};
        for (size_t ch = 0; ch < at.size(); ++ch)
            at[ch] = channels[ch] != nullptr ? channels[ch] + done : nullptr;
        ok = writer.write(at.data(), juce::jmin(c_piece, numSamples - done));
    }
    bool markersAdded = true;
    if (! writer.close(ok, cues, true, markersAdded))
    {
        error = "Writing " + file.getFileName() + " failed.";
        return false;
    }
    if (written != nullptr)
        *written = writer.files();
    return true;
}

juce::Array<juce::File> TapeRecorder::filesFor(const juce::File& file, int numChannels, const Options& options)
{
    return takefile::filesOf(file, fileWidthOf(options, numChannels), options.layout);
}

room::Width TapeRecorder::fileWidthOf(const Options& options, int numChannels)
{
    const auto own = takefile::widthForChannels(numChannels).value_or(room::Width::Stereo);
    if (options.mono)
        return room::Width::Mono;
    if (options.stereo && room::canFold(own, room::Width::Stereo))
        return room::Width::Stereo;
    return own;
}

std::optional<room::Width> TapeRecorder::foldedFrom(const Options& options, int numChannels)
{
    const auto played = options.played.value_or(takefile::widthForChannels(numChannels).value_or(room::Width::Stereo));
    const auto file = fileWidthOf(options, numChannels);
    if (file == played || ! room::canFold(played, file))
        return std::nullopt;
    return played;
}

juce::String TapeRecorder::formatProblem(const Options& options, int numChannels)
{
    const auto width = fileWidthOf(options, numChannels);
    const int channels = room::layout(width).numChannels;
    const bool multi = options.layout == takefile::Layout::MultiMono && width != room::Width::Mono;
    if (options.type == FileType::Flac && ! multi && channels > takefile::c_maxFlacChannels)
        return "A FLAC file holds at most 8 channels, and " + juce::String(room::layout(width).name) + " has " + juce::String(channels)
             + ". Choose WAV or AIFF, or Multi-Mono files.";
    return {};
}

juce::String TapeRecorder::foldNote(room::Width from, room::Width to)
{
    return "Folded down from " + juce::String(room::layout(from).name) + " to " + juce::String(room::layout(to).name)
         + (room::layout(from).numChannels > 2 ? ", held under the Ceiling. " : ". ");
}

//==============================================================================
TapeRecorder::FileWriter::FileWriter() = default;

TapeRecorder::FileWriter::~FileWriter()
{
    if (isOpen())
        close({});
}

bool TapeRecorder::FileWriter::open(const juce::File& file, double rate, int numChannels, const Options& options, const Naming& naming,
                                    juce::String& error)
{
    if (isOpen())
        close({});
    auto writer = std::make_unique<TakeWriter>();
    m_ok = writer->open(file, rate, numChannels, options, naming, error);
    m_files = m_ok ? writer->files() : juce::Array<juce::File> {};
    m_writer = m_ok ? std::move(writer) : nullptr;
    m_channels = numChannels;
    m_position = 0;
    m_markersAdded = true;
    return m_ok;
}

bool TapeRecorder::FileWriter::write(const juce::AudioBuffer<float>& audio, int startSample, int numSamples)
{
    if (! isOpen() || numSamples <= 0)
        return isOpen();
    jassert(audio.getNumChannels() >= m_channels);
    m_ok = m_writer->write(pointersOf(audio, startSample).data(), numSamples) && m_ok;
    m_position += numSamples;
    return m_ok;
}

bool TapeRecorder::FileWriter::writeSilence(int numSamples)
{
    if (! isOpen())
        return false;
    juce::AudioBuffer<float> quiet(m_channels, juce::jmin(4096, juce::jmax(1, numSamples)));
    quiet.clear();
    for (int left = numSamples; left > 0 && m_ok; left -= quiet.getNumSamples())
        write(quiet, 0, juce::jmin(left, quiet.getNumSamples()));
    return m_ok;
}

bool TapeRecorder::FileWriter::close(const std::vector<Cue>& markers)
{
    if (! isOpen())
        return false;
    const bool kept = m_writer->close(m_ok, markers, true, m_markersAdded); // every file goes if any write failed
    m_writer.reset();
    return kept;
}

void TapeRecorder::FileWriter::abandon()
{
    if (m_writer != nullptr)
        m_writer->discard();
    m_writer.reset();
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
        case MarkPassStow: return "Pass S";
        default:        return "Marker";
    }
}

juce::File TapeRecorder::nextFile(const juce::File& folder, const juce::String& suffix, const juce::String& extension)
{
    const juce::ScopedLock sl(m_metaLock);
    auto prefix = juce::File::createLegalFileName(m_naming.prefix.trim()).substring(0, 80).trim();
    if (prefix.isEmpty())
        prefix = "Swooshbuckler";
    // A name is taken if a file has it, or a Multi-Mono set does: a set has no file of that name, but every set of
    // Stereo or wider has an .L file. (Opening a take deletes what is in its way, so a name must never be given twice.)
    auto taken = [&](const juce::String& stem)
    {
        return folder.getChildFile(stem + extension).exists()
            || folder.getChildFile(stem + room::info(room::Speaker::L).suffix + extension).exists();
    };
    if (m_naming.style == NameStyle::DateTime)
    {
        // <Name>_YYYY-MM-DD_HH-MM-SS; two in the same second are numbered apart (..._2, ..._3).
        const auto stem = prefix + "_" + juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H-%M-%S") + suffix;
        auto name = stem;
        const juce::String join = juce::CharacterFunctions::isDigit(stem.getLastCharacter()) ? "_" : "";
        for (int n = 2; taken(name) && n < 100000; ++n)
            name = stem + join + juce::String(n);
        return folder.getChildFile(name + extension);
    }
    // <Name>_NNNN: the next number of this session that no file in the folder has yet (whatever its type or suffix
    // would be, a number is used once). A file that is thrown away gives its number back.
    for (int n = m_counter;; ++n)
    {
        const auto stem = prefix + "_" + juce::String::formatted("%04d", n) + suffix;
        if (! taken(stem) || n > m_counter + 100000)
            return folder.getChildFile(stem + extension);
    }
}

int TapeRecorder::numberOf(const juce::File& file) const
{
    // The NNNN of <Name>_NNNN[_suffix]: the first all-digit part after an underscore, from the right.
    auto name = file.getFileNameWithoutExtension();
    if (const int dot = name.lastIndexOfChar('.'); dot > 0)
        name = name.substring(0, dot); // a file of a Multi-Mono set: <Name>_NNNN.Ls
    const auto parts = juce::StringArray::fromTokens(name, "_", {});
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
        for (const auto& c : takefile::flacComments(file))
            if (c.startsWithIgnoreCase("DESCRIPTION="))
                return c.fromFirstOccurrenceOf("=", false, false);
        return {};
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor(file) })
    {
        // A WAV file's whole description is its LIST INFO comment; the BWF one is cut to 256 bytes.
        if (! file.hasFileExtension("wav"))
            return reader->metadataValues["CueNote0Text"];
        const auto full = reader->metadataValues[juce::WavAudioFormat::riffInfoComment2];
        return full.isNotEmpty() ? full : reader->metadataValues[juce::WavAudioFormat::bwavDescription];
    }
    return {};
}

std::vector<TapeRecorder::Cue> TapeRecorder::readCues(const juce::File& file)
{
    std::vector<Cue> cues;
    juce::WavAudioFormat wav;
    juce::AiffAudioFormat aiff;
    auto stream = std::make_unique<juce::FileInputStream>(file);
    if (! stream->openedOk())
        return cues;
    const bool isAiff = file.hasFileExtension("aif;aiff");
    std::unique_ptr<juce::AudioFormatReader> reader { isAiff ? aiff.createReaderFor(stream.release(), true)
                                                             : wav.createReaderFor(stream.release(), true) };
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

double TapeRecorder::retroCapacityFor(double sampleRate, int numChannels, int seconds)
{
    const auto asked = static_cast<std::int64_t>(sampleRate * seconds);
    const auto fits = c_maxRetroBytes / (static_cast<std::int64_t>(juce::jmax(1, numChannels)) * static_cast<std::int64_t>(sizeof(float)));
    return static_cast<double>(juce::jmin(asked, fits)) / juce::jmax(1.0, sampleRate);
}

int TapeRecorder::retroSamples(double sampleRate, int numChannels, int seconds)
{
    const double capacity = retroCapacityFor(sampleRate, numChannels, seconds);
    m_retroCapacitySeconds.store(capacity);
    return static_cast<int>(capacity * sampleRate) + 1;
}

void TapeRecorder::setRetroSeconds(int seconds)
{
    seconds = juce::jlimit(c_retroSeconds, c_maxRetroSeconds, seconds);
    if (seconds == m_retroSeconds.load())
        return;
    m_retroSeconds = seconds;
    // A bigger (or smaller) ring, made here and swapped in under the lock (the audio thread only try-locks it).
    juce::AudioBuffer<float> ring(m_numChannels, retroSamples(m_sampleRate, m_numChannels, seconds));
    ring.clear();
    const juce::SpinLock::ScopedLockType lock(m_retroLock);
    m_retroGeneration.fetch_add(1);
    std::swap(m_retro, ring);
    if (const int wanted = retroSamples(m_sampleRate, m_numChannels, m_retroSeconds.load());
        m_retro.getNumSamples() != wanted || m_retro.getNumChannels() != m_numChannels)
        m_retro.setSize(m_numChannels, wanted, false, true, false); // prepared meanwhile
    m_retroWrite = 0;
    m_retroTotal = 0;
}

void TapeRecorder::prepare(double sampleRate, int numChannels)
{
    numChannels = juce::jlimit(1, room::c_maxChannels, numChannels);
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
    m_retro.setSize(m_numChannels, retroSamples(sampleRate, m_numChannels, m_retroSeconds.load()), false, true, false);
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
                    m_handedSamples.fetch_add(n, std::memory_order_relaxed);
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
                std::array<const float*, room::c_maxChannels> all {};
                for (int ch = 0; ch < juce::jmin(room::c_maxChannels, m_numChannels); ++ch)
                    all[static_cast<size_t>(ch)] = channels[juce::jmin(ch, numChannels - 1)];
                writeBlock(all.data(), numSamples);
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
                        std::array<const float*, room::c_maxChannels> seg {};
                        for (int ch = 0; ch < juce::jmin(room::c_maxChannels, m_preroll.getNumChannels()); ++ch)
                            seg[static_cast<size_t>(ch)] = m_preroll.getReadPointer(ch, read);
                        writeBlock(seg.data(), n);
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
                        std::array<const float*, room::c_maxChannels> seg {};
                        for (int ch = 0; ch < juce::jmin(room::c_maxChannels, m_fade.getNumChannels()); ++ch)
                        {
                            const float* in = channels[juce::jmin(ch, numChannels - 1)];
                            float* out = m_fade.getWritePointer(ch);
                            for (int i = 0; i < rising; ++i)
                                out[i] = in[i] * static_cast<float>(fadeLen - m_fadeInLeft + i) / static_cast<float>(fadeLen);
                            seg[static_cast<size_t>(ch)] = out;
                        }
                        writeBlock(seg.data(), rising);
                        m_fadeInLeft -= rising;
                    }
                    if (plain > rising)
                    {
                        // Every channel the tape was prepared with (a caller with fewer repeats its last one).
                        std::array<const float*, room::c_maxChannels> seg {};
                        for (int ch = 0; ch < juce::jmin(room::c_maxChannels, m_numChannels); ++ch)
                            seg[static_cast<size_t>(ch)] = channels[juce::jmin(ch, numChannels - 1)] + rising;
                        writeBlock(seg.data(), plain - rising);
                    }
                    if (count > plain)
                    {
                        std::array<const float*, room::c_maxChannels> seg {};
                        for (int ch = 0; ch < juce::jmin(room::c_maxChannels, m_fade.getNumChannels()); ++ch)
                        {
                            const float* in = channels[juce::jmin(ch, numChannels - 1)];
                            float* out = m_fade.getWritePointer(ch);
                            for (int i = plain; i < count; ++i)
                                out[i - plain] = in[i] * static_cast<float>(m_hangLeft - i) / static_cast<float>(fadeLen);
                            seg[static_cast<size_t>(ch)] = out;
                        }
                        writeBlock(seg.data(), count - plain);
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

    // A format that can't hold this take says so now, not at the first whoosh.
    if (const auto problem = formatProblem(options, m_numChannels); problem.isNotEmpty())
    {
        error = problem;
        return false;
    }
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
    m_handedSamples = 0;
    m_takenSamples = 0;
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
    if (const auto problem = formatProblem(options, m_numChannels); problem.isNotEmpty())
    {
        error = problem;
        return false;
    }
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
    juce::Array<juce::File> files;
    const auto name = nextFile(folder, "_retro", extensionFor(options.type));
    if (! writeFile(name, copy, first, copy.getNumSamples() - first, rate, options, naming, error, &files))
        return false;
    written = files[0];
    if (naming.style == NameStyle::Counter)
    {
        const juce::ScopedLock sl(m_metaLock);
        m_counter = juce::jmax(m_counter, numberOf(written) + 1);
    }
    const juce::ScopedLock sl(m_fileLock);
    m_lastFile = written;
    return true;
}

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
                                                          const TapeRecorder::Options& o, juce::String& error)
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
            case TapeRecorder::FileType::Aiff: format = std::make_unique<juce::AiffAudioFormat>(); break;
            case TapeRecorder::FileType::Flac: format = std::make_unique<juce::FlacAudioFormat>(); break;
            case TapeRecorder::FileType::Wav:
            default:
                format = std::make_unique<juce::WavAudioFormat>();
                metadata = juce::WavAudioFormat::createBWAVMetadata(
                    "Swooshbuckler log", "Swooshbuckler", "Samuel Justice", juce::Time::getCurrentTime(), 0,
                    "A=PCM,F=" + juce::String(juce::roundToInt(fileRate)) + ",W=" + juce::String(bits)
                        + ",M=" + (channels == 1 ? juce::String("mono") : juce::String("stereo")) + ",T=Swooshbuckler");
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

juce::File TapeRecorder::makeFileName(const juce::File& folder, const juce::String& suffix, const juce::String& extension)
{
    // Swooshbuckler_YYYY-MM-DD_HH-MM-SS_NNNNN: a random 5-digit tag so two takes in the same second never clash
    // (getNonexistentChildFile is the last-resort guard).
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H-%M-%S");
    const auto tag = juce::String::formatted("%05d", juce::Random::getSystemRandom().nextInt(100000));
    return folder.getNonexistentChildFile("Swooshbuckler_" + stamp + "_" + tag + suffix, extension, false);
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
    m_retro.setSize(m_numChannels, static_cast<int>(sampleRate * c_retroSeconds) + 1, false, true, false);
    m_retro.clear();
    m_retroWrite = 0;
    m_retroTotal = 0;
}

bool TapeRecorder::rest()
{
    const juce::ScopedTryLock lock(m_writerLock);
    if (! lock.isLocked())
        return false;
    // What reached the tape last was already silent (the transport had faded out), so there is nothing to fade.
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
        const juce::ScopedTryLock lock(m_writerLock);
        if (lock.isLocked() && m_activeWriter != nullptr)
        {
            auto writeBlock = [this](const float* const* data, int n)
            {
                if (m_activeWriter->write(data, n))
                    m_samplesWritten += n;
                else
                    m_droppedSamples += n; // FIFO full (disk too slow): reported to the UI
            };

            if (m_gateRestart.exchange(false))
            {
                m_gatePeak = 0.0f;
                m_gatePeakHold = 0;
                m_hangLeft = 0;
                m_fadeInLeft = 0;
                m_prerollFill = 0;
                m_capturing = false;
            }

            if (! m_signalGate.load())
            {
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
                    m_capturing = true;
                    m_hangLeft = static_cast<int>((c_gateQuietSeconds + c_gateHangSeconds) * m_sampleRate);
                }

                if (m_capturing.load())
                {
                    // The last fadeLen samples of the hang fade out; whatever is left of the block after the
                    // gate has closed is the start of the next pre-roll.
                    const int count = juce::jmin(numSamples, m_hangLeft);
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

    const auto file = makeFileName(folder, {}, extensionFor(options.type));
    {
        const juce::ScopedLock sl(m_fileLock);
        m_currentFile = file;
    }
    auto writer = createWriter(file, m_sampleRate, m_numChannels, options, error);
    if (writer == nullptr)
        return false;

    m_samplesWritten = 0;
    m_droppedSamples = 0;
    m_recordRate = m_sampleRate;
    m_recordFolder = folder;
    m_recordOptions = options;
    // The FIFO is sized by time, so a slow disk has the same grace at 192 kHz as at 48 kHz.
    m_writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), m_thread, getFifoSamples());
    m_gateRestart = true;

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
        m_writer.reset(); // flushes the FIFO and closes the file
        const juce::ScopedLock sl(m_fileLock);
        m_lastFile = m_currentFile;
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
        error = "The last minute was silent.";
        return false;
    }

    if (! folder.isDirectory() && ! folder.createDirectory())
    {
        error = "Couldn't create the folder " + folder.getFullPathName();
        return false;
    }

    written = makeFileName(folder, "_retro", extensionFor(options.type));
    auto writer = createWriter(written, rate, copy.getNumChannels(), options, error);
    if (writer == nullptr)
        return false;

    if (! writer->writeFromAudioSampleBuffer(copy, first, copy.getNumSamples() - first))
    {
        error = "Writing " + written.getFileName() + " failed.";
        return false;
    }
    writer.reset(); // flushes the resampler and closes the file
    const juce::ScopedLock sl(m_fileLock);
    m_lastFile = written;
    return true;
}

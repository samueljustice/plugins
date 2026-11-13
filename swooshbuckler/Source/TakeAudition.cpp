#include "TakeAudition.h"

namespace
{
    constexpr int c_fadeSamples = 256;
}

TakeAudition::TakeAudition()
{
    m_formats.registerBasicFormats();
}

TakeAudition::~TakeAudition()
{
    stop();
    m_thread.removeTimeSliceClient(this);
    m_thread.stopThread(2000);
    const juce::SpinLock::ScopedLockType live(m_liveLock);
    m_live = nullptr;
}

void TakeAudition::prepare(double deviceRate)
{
    {
        const juce::SpinLock::ScopedLockType live(m_liveLock);
        m_live = nullptr;
    }
    const juce::ScopedLock sl(m_streamLock);
    m_stream.reset();
    m_deviceRate = deviceRate > 0.0 ? deviceRate : 48000.0;
}

bool TakeAudition::play(const juce::File& file, juce::String& error)
{
    auto stream = std::make_shared<Stream>();
    stream->reader.reset(m_formats.createReaderFor(file));
    if (stream->reader == nullptr || stream->reader->lengthInSamples <= 0 || stream->reader->sampleRate <= 0.0)
    {
        error = "Couldn't read " + file.getFileName();
        return false;
    }
    stream->file = file;
    stream->ratio = stream->reader->sampleRate / m_deviceRate;
    stream->total = juce::jmax<juce::int64>(1, static_cast<juce::int64>(static_cast<double>(stream->reader->lengthInSamples) / stream->ratio));
    const int ringSize = juce::jmax(1 << 15, static_cast<int>(m_deviceRate)); // a second of sound ahead
    stream->ring.setSize(2, ringSize);
    stream->ring.clear();
    stream->fifo = std::make_unique<juce::AbstractFifo>(ringSize);
    const int piece = 4096;
    stream->converted.setSize(2, piece);
    stream->in.setSize(2, static_cast<int>(piece * stream->ratio) + 16);
    fill(*stream); // the first second, so it starts at once

    std::shared_ptr<Stream> old;
    {
        const juce::ScopedLock sl(m_streamLock);
        {
            const juce::SpinLock::ScopedLockType live(m_liveLock);
            m_live = stream.get();
        }
        old = std::move(m_stream); // the audio thread has let go of it: freed here, on the message thread
        m_stream = std::move(stream);
    }
    if (! m_thread.isThreadRunning())
        m_thread.startThread(juce::Thread::Priority::normal);
    m_thread.addTimeSliceClient(this);
    return true;
}

void TakeAudition::stop()
{
    const juce::ScopedLock sl(m_streamLock);
    if (m_stream != nullptr)
        m_stream->stopping = true; // the audio thread fades it out; if no audio is running it just stays silent
}

void TakeAudition::close()
{
    stop();
    // The fade out takes a few milliseconds of audio; without audio running there is nothing to wait for.
    for (int tries = 0; tries < 20; ++tries)
    {
        {
            const juce::ScopedLock sl(m_streamLock);
            if (m_stream == nullptr || m_stream->finished.load())
                break;
        }
        juce::Thread::sleep(2);
    }
    std::shared_ptr<Stream> old;
    {
        const juce::ScopedLock sl(m_streamLock);
        {
            const juce::SpinLock::ScopedLockType live(m_liveLock);
            m_live = nullptr;
        }
        old = std::move(m_stream); // the audio thread has let go of it: the reader closes here
    }
}

bool TakeAudition::isPlaying() const
{
    const juce::ScopedLock sl(m_streamLock);
    return m_stream != nullptr && ! m_stream->finished.load() && ! m_stream->stopping.load();
}

juce::File TakeAudition::getFile() const
{
    const juce::ScopedLock sl(m_streamLock);
    return m_stream != nullptr ? m_stream->file : juce::File();
}

double TakeAudition::getProgress() const
{
    const juce::ScopedLock sl(m_streamLock);
    return m_stream != nullptr ? juce::jlimit(0.0, 1.0, static_cast<double>(m_stream->played.load()) / static_cast<double>(m_stream->total)) : 0.0;
}

int TakeAudition::useTimeSlice()
{
    const juce::ScopedLock sl(m_streamLock);
    if (m_stream == nullptr || m_stream->finished.load() || m_stream->stopping.load())
        return 200;
    fill(*m_stream);
    return 20;
}

void TakeAudition::fill(Stream& s)
{
    // Read, convert to the device rate, and put into the ring for as long as there is room for a whole piece.
    const int piece = s.converted.getNumSamples();
    while (! s.sourceDone.load() && s.fifo->getFreeSpace() >= piece)
    {
        const auto left = s.reader->lengthInSamples - s.readPos;
        if (left <= 0)
        {
            s.sourceDone = true;
            break;
        }
        // The interpolator says how much input each piece used, so ask for what a piece needs and keep the place.
        const int wanted = static_cast<int>(juce::jmin<juce::int64>(left, static_cast<juce::int64>(piece * s.ratio) + 8));
        const int outCount = left > wanted ? piece : juce::jmax(1, juce::jmin(piece, static_cast<int>(static_cast<double>(wanted) / s.ratio)));
        s.in.clear();
        s.reader->read(&s.in, 0, wanted, s.readPos, true, true);
        if (s.reader->numChannels < 2)
            s.in.copyFrom(1, 0, s.in, 0, 0, wanted);
        int used = wanted;
        for (int ch = 0; ch < 2; ++ch)
            used = s.resamplers[static_cast<size_t>(ch)].process(s.ratio, s.in.getReadPointer(ch), s.converted.getWritePointer(ch), outCount, wanted, 0);
        s.readPos += juce::jmax(1, used);

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        s.fifo->prepareToWrite(outCount, start1, size1, start2, size2);
        for (int ch = 0; ch < 2; ++ch)
        {
            if (size1 > 0) s.ring.copyFrom(ch, start1, s.converted, ch, 0, size1);
            if (size2 > 0) s.ring.copyFrom(ch, start2, s.converted, ch, size1, size2);
        }
        s.fifo->finishedWrite(size1 + size2);
    }
}

void TakeAudition::process(juce::AudioBuffer<float>& out, int start, int numSamples)
{
    const juce::SpinLock::ScopedTryLockType live(m_liveLock);
    if (! live.isLocked() || m_live == nullptr || m_live->finished.load(std::memory_order_relaxed))
        return;
    auto& s = *m_live;
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    s.fifo->prepareToRead(numSamples, start1, size1, start2, size2);
    const int got = size1 + size2;
    const bool stopping = s.stopping.load(std::memory_order_relaxed);
    const float step = 1.0f / static_cast<float>(c_fadeSamples);
    const int channels = out.getNumChannels();
    for (int i = 0; i < got; ++i)
    {
        const int at = i < size1 ? start1 + i : start2 + (i - size1);
        s.gain = stopping ? juce::jmax(0.0f, s.gain - step) : juce::jmin(1.0f, s.gain + step);
        const float l = s.ring.getSample(0, at) * s.gain, r = s.ring.getSample(1, at) * s.gain;
        if (channels >= 2)
        {
            out.addSample(0, start + i, l);
            out.addSample(1, start + i, r);
        }
        else if (channels == 1)
            out.addSample(0, start + i, 0.5f * (l + r));
    }
    s.fifo->finishedRead(got);
    s.played.fetch_add(got, std::memory_order_relaxed);
    // Over: the fade out has finished, or the file has been played to its end.
    if ((stopping && s.gain <= 0.0f) || (got < numSamples && s.sourceDone.load(std::memory_order_relaxed) && s.fifo->getNumReady() == 0))
        s.finished.store(true, std::memory_order_relaxed);
}

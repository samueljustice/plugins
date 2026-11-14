#include "TakeAudition.h"
#include "TakeFile.h"

namespace
{
    constexpr int c_fadeSamples = 256;
}

TakeAudition::TakeAudition() = default;

TakeAudition::~TakeAudition()
{
    stop();
    m_thread.removeTimeSliceClient(this);
    m_thread.stopThread(2000);
    const juce::SpinLock::ScopedLockType live(m_liveLock);
    m_live = nullptr;
}

TakeAudition::Mix TakeAudition::mixFor(int takeChannels, room::Width outputWidth)
{
    Mix mix;
    const auto& out = room::layout(outputWidth);
    mix.numOut = out.numChannels;
    mix.numIn = juce::jlimit(1, room::c_maxChannels, takeChannels);
    const auto width = takefile::widthForChannels(takeChannels);
    auto put = [&](int to, int from, float g)
    {
        if (to >= 0 && to < mix.numOut && from < mix.numIn)
            mix.gain[static_cast<size_t>(to)][static_cast<size_t>(from)] += g;
    };
    if (! width.has_value() || *width == room::Width::Mono)
    {
        // Mono (or channels of no known width: the first two): L and R, as a stereo device always played them.
        if (outputWidth == room::Width::Mono)
        {
            put(0, 0, mix.numIn > 1 ? 0.5f : 1.0f);
            put(0, 1, 0.5f);
            return mix;
        }
        put(out.indexOf(room::Speaker::L), 0, 1.0f);
        put(out.indexOf(room::Speaker::R), mix.numIn > 1 ? 1 : 0, 1.0f);
        return mix;
    }
    if (room::canFold(*width, outputWidth))
    {
        const auto& fold = room::fold(*width, outputWidth);
        for (int o = 0; o < fold.numOut; ++o)
            for (int i = 0; i < fold.numIn; ++i)
                put(o, i, fold.gain[static_cast<size_t>(o)][static_cast<size_t>(i)]);
        return mix;
    }
    // Narrower than the output: each speaker to the same one, or to the two it stands between.
    const auto& in = room::layout(*width);
    for (int i = 0; i < in.numChannels; ++i)
    {
        const auto s = in.speakers[static_cast<size_t>(i)];
        if (const int to = out.indexOf(s); to >= 0)
        {
            put(to, i, 1.0f);
            continue;
        }
        auto pair = [&](room::Speaker a, room::Speaker b)
        {
            put(out.indexOf(a), i, room::c_foldStep);
            put(out.indexOf(b), i, room::c_foldStep);
        };
        using S = room::Speaker;
        if (s == S::Ls)
            pair(S::Lss, S::Lsr);
        else if (s == S::Rs)
            pair(S::Rss, S::Rsr);
        else if (s == S::Ltm)
            pair(S::Ltf, S::Ltr);
        else if (s == S::Rtm)
            pair(S::Rtf, S::Rtr);
    }
    return mix;
}

void TakeAudition::prepare(double deviceRate, room::Width outputWidth)
{
    {
        const juce::SpinLock::ScopedLockType live(m_liveLock);
        m_live = nullptr;
    }
    const juce::ScopedLock sl(m_streamLock);
    m_stream.reset();
    m_deviceRate = deviceRate > 0.0 ? deviceRate : 48000.0;
    m_outputWidth = outputWidth;
}

bool TakeAudition::play(const juce::File& file, juce::String& error)
{
    auto stream = std::make_shared<Stream>();
    stream->reader = takefile::openReader(file); // in processing order; a Multi-Mono set as one
    if (stream->reader == nullptr || stream->reader->lengthInSamples <= 0 || stream->reader->sampleRate <= 0.0)
    {
        error = "Couldn't read " + file.getFileName();
        return false;
    }
    stream->file = file;
    stream->channels = juce::jlimit(1, room::c_maxChannels, static_cast<int>(stream->reader->numChannels));
    {
        const auto mix = mixFor(stream->channels, m_outputWidth);
        stream->outChannels = mix.numOut;
        for (int o = 0; o < mix.numOut; ++o)
            for (int i = 0; i < mix.numIn; ++i)
                if (const float g = mix.gain[static_cast<size_t>(o)][static_cast<size_t>(i)]; g != 0.0f)
                    stream->routes[static_cast<size_t>(stream->numRoutes++)] = { o, i, g };
    }
    stream->ratio = stream->reader->sampleRate / m_deviceRate;
    stream->total = juce::jmax<juce::int64>(1, static_cast<juce::int64>(static_cast<double>(stream->reader->lengthInSamples) / stream->ratio));
    const int ringSize = juce::jmax(1 << 15, static_cast<int>(m_deviceRate)); // a second of sound ahead
    stream->ring.setSize(stream->channels, ringSize);
    stream->ring.clear();
    stream->fifo = std::make_unique<juce::AbstractFifo>(ringSize);
    const int piece = 4096;
    stream->converted.setSize(stream->channels, piece);
    stream->in.setSize(stream->channels, static_cast<int>(piece * stream->ratio) + 16);
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

void TakeAudition::topUp()
{
    const juce::ScopedLock sl(m_streamLock);
    if (m_stream != nullptr && ! m_stream->finished.load() && ! m_stream->stopping.load())
        fill(*m_stream);
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
        s.reader->read(s.in.getArrayOfWritePointers(), s.channels, s.readPos, wanted);
        int used = wanted;
        for (int ch = 0; ch < s.channels; ++ch)
            used = s.resamplers[static_cast<size_t>(ch)].process(s.ratio, s.in.getReadPointer(ch), s.converted.getWritePointer(ch), outCount, wanted, 0);
        s.readPos += juce::jmax(1, used);

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        s.fifo->prepareToWrite(outCount, start1, size1, start2, size2);
        for (int ch = 0; ch < s.channels; ++ch)
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
    const bool routed = channels == s.outChannels; // the width it was started for (else: its first two as L and R)
    for (int i = 0; i < got; ++i)
    {
        const int at = i < size1 ? start1 + i : start2 + (i - size1);
        s.gain = stopping ? juce::jmax(0.0f, s.gain - step) : juce::jmin(1.0f, s.gain + step);
        if (routed)
        {
            for (int k = 0; k < s.numRoutes; ++k)
            {
                const auto& route = s.routes[static_cast<size_t>(k)];
                out.addSample(route.out, start + i, route.gain * s.gain * s.ring.getSample(route.in, at));
            }
            continue;
        }
        const float l = s.ring.getSample(0, at) * s.gain, r = s.ring.getSample(juce::jmin(1, s.channels - 1), at) * s.gain;
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

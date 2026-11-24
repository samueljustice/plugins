#include "CaptureRecorder.h"

#include <thread>

namespace tt::audiosuite
{

namespace
{
    constexpr int waitStepMs = 1;
    constexpr int giveUpAfterMs = 30000;

    constexpr int chunkFrames = 8192;
}

CaptureRecorder::CaptureRecorder (juce::File folderToUse, int capacityFrames, Timing timingToUse)
    : juce::Thread ("Tempo Tantrum Capture writer"),
      folder (std::move (folderToUse)),
      timing (timingToUse),
      sampleFifo (capacityFrames),
      ring (maxChannels, capacityFrames),
      events ((size_t) eventFifo.getTotalSize()),
      chunk (maxChannels, chunkFrames)
{
    ring.clear();

    // The ring is used through raw pointers from here on, so neither thread touches the buffer's
    // own bookkeeping while the other works.
    for (int ch = 0; ch < maxChannels; ++ch)
        ringData[ch] = ring.getWritePointer (ch);

    startThread (juce::Thread::Priority::normal);
}

CaptureRecorder::~CaptureRecorder()
{
    // The writer finishes what is queued and closes the capture in progress before it stops.
    signalThreadShouldExit();
    stopThread (10000);
}

//==============================================================================
template <typename HasRoom>
bool CaptureRecorder::waitForRoom (HasRoom hasRoom)
{
    waits.fetch_add (1, std::memory_order_relaxed);

    for (int waited = 0; waited < giveUpAfterMs; waited += waitStepMs)
    {
        if (hasRoom())
            return true;

        std::this_thread::sleep_for (std::chrono::milliseconds (waitStepMs));
    }

    return hasRoom();
}

bool CaptureRecorder::pushEvent (const Event& event)
{
    if (eventFifo.getFreeSpace() < 1 && ! waitForRoom ([this] { return eventFifo.getFreeSpace() >= 1; }))
        return false;

    const auto scope = eventFifo.write (1);
    events[(size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)] = event;
    return true;
}

void CaptureRecorder::record (const juce::AudioBuffer<float>& block, int numChannels, int numSamples, double sampleRate,
                              const PlayHeadFacts& facts)
{
    const int channels = juce::jmin (numChannels, block.getNumChannels(), maxChannels);
    numSamples = juce::jmin (numSamples, block.getNumSamples());

    if (numSamples <= 0 || channels <= 0)
        return;

    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto requests = endRequests.load (std::memory_order_acquire);

    const bool startNew = ! capturing
                       || requests != endRequestsSeen
                       || channels != captureChannels
                       || ! juce::exactlyEqual (sampleRate, captureRate)
                       || now - lastBlockMs > timing.newCaptureAfterMs;

    endRequestsSeen = requests;

    if (startNew)
    {
        if (capturing)
            pushEvent (Event { EventType::end, 0, 0, 0.0, {} });

        capturing = true;
        abandoned = false;
        captureChannels = channels;
        captureRate = sampleRate;

        Event start { EventType::start, 0, 0, 0.0, {} };
        start.endRequests = requests;
        start.channels = channels;
        start.sampleRate = sampleRate;
        start.facts = facts;

        if (! pushEvent (start))
        {
            abandoned = true;
            lostSamples.store (true);
        }
    }

    // The gap to the next block is counted from here, when this one is done: a block that waited
    // for room in the ring does not count its own wait.
    struct StampOnReturn
    {
        double& at;
        ~StampOnReturn() { at = juce::Time::getMillisecondCounterHiRes(); }
    };

    const StampOnReturn stamp { lastBlockMs };

    if (abandoned)
        return;

    for (int done = 0; done < numSamples;)
    {
        // Room for the event first, so frames in the ring always have an event that counts them.
        const bool room = (eventFifo.getFreeSpace() >= 1 || waitForRoom ([this] { return eventFifo.getFreeSpace() >= 1; }))
                       && (sampleFifo.getFreeSpace() >= 1 || waitForRoom ([this] { return sampleFifo.getFreeSpace() >= 1; }));

        if (! room)
        {
            abandoned = true;
            lostSamples.store (true);
            return;
        }

        const int todo = juce::jmin (numSamples - done, sampleFifo.getFreeSpace());

        {
            const auto scope = sampleFifo.write (todo);

            for (int ch = 0; ch < channels; ++ch)
            {
                const float* source = block.getReadPointer (ch, done);

                if (scope.blockSize1 > 0)
                    juce::FloatVectorOperations::copy (ringData[ch] + scope.startIndex1, source, scope.blockSize1);

                if (scope.blockSize2 > 0)
                    juce::FloatVectorOperations::copy (ringData[ch] + scope.startIndex2, source + scope.blockSize1, scope.blockSize2);
            }
        }

        Event data { EventType::data, 0, 0, 0.0, {} };
        data.frames = todo;
        pushEvent (data);
        done += todo;
    }
}

void CaptureRecorder::notRecording()
{
    if (! capturing)
        return;

    capturing = false;
    pushEvent (Event { EventType::end, 0, 0, 0.0, {} });
}

void CaptureRecorder::requestEnd()
{
    endRequests.fetch_add (1, std::memory_order_acq_rel);
}

CaptureRecorder::Finished CaptureRecorder::lastFinished() const
{
    const std::lock_guard<std::mutex> l (finishedLock);
    return last;
}

//==============================================================================
void CaptureRecorder::run()
{
    while (! threadShouldExit())
    {
        if (drainOnce())
            continue;

        // Nothing queued. The open capture is closed when an end was asked for after the audio
        // thread started it (an end asked for before that belongs to an earlier capture, and the
        // audio thread has already queued its end), or, as a fallback for a host that never says
        // a Render has ended, when nothing has come for a long while.
        if (writer != nullptr)
        {
            if (endRequests.load (std::memory_order_acquire) != openEndRequests)
                finish();
            else if (juce::Time::getMillisecondCounterHiRes() - lastEventMs > timing.closeIdleCaptureAfterMs)
                finish();
        }

        wait (5);
    }

    while (drainOnce()) {}

    if (writer != nullptr)
        finish();
}

bool CaptureRecorder::drainOnce()
{
    const int ready = eventFifo.getNumReady();

    if (ready == 0)
        return false;

    const int take = juce::jmin (ready, 256);
    int done = 0;

    while (done < take)
    {
        Event event;

        {
            const auto scope = eventFifo.read (1);
            event = events[(size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)];
        }

        ++done;
        lastEventMs = juce::Time::getMillisecondCounterHiRes();

        switch (event.type)
        {
            case EventType::start:
                if (writer != nullptr)
                    finish();

                begin (event);
                break;

            case EventType::data:
                if (writer != nullptr)
                    append (event.frames);
                else
                    discardFrames (event.frames);
                break;

            case EventType::end:
                if (writer != nullptr)
                    finish();
                break;
        }
    }

    return true;
}

void CaptureRecorder::begin (const Event& event)
{
    folder.createDirectory();

    const auto now = juce::Time::getCurrentTime();
    auto stem = juce::String (filePrefix) + " " + now.formatted ("%Y-%m-%d %H%M%S");

    for (int n = 2; folder.getChildFile (stem + ".wav").exists() || folder.getChildFile (stem + ".json").exists(); ++n)
        stem = juce::String (filePrefix) + " " + now.formatted ("%Y-%m-%d %H%M%S") + " " + juce::String (n);

    current = {};
    current.wav = folder.getChildFile (stem + ".wav");
    current.json = folder.getChildFile (stem + ".json");
    current.sampleRate = event.sampleRate;
    current.channels = event.channels;
    current.playHead = event.facts;
    current.captured = now;
    openEndRequests = event.endRequests;
   #ifdef JucePlugin_VersionString
    current.plugInVersion = JucePlugin_VersionString;
   #endif
    written = 0;

    std::unique_ptr<juce::OutputStream> stream (new juce::FileOutputStream (current.wav));

    if (static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
    {
        juce::WavAudioFormat wav;
        writer.reset (wav.createWriterFor (stream.get(), event.sampleRate, (unsigned int) event.channels, 32, {}, 0));

        if (writer != nullptr)
            stream.release();   // the writer owns it now
    }
}

void CaptureRecorder::append (int frames)
{
    const int channels = current.channels;

    while (frames > 0)
    {
        const int todo = juce::jmin (frames, chunkFrames);

        {
            const auto scope = sampleFifo.read (todo);

            for (int ch = 0; ch < channels; ++ch)
            {
                float* dest = chunk.getWritePointer (ch);

                if (scope.blockSize1 > 0)
                    juce::FloatVectorOperations::copy (dest, ringData[ch] + scope.startIndex1, scope.blockSize1);

                if (scope.blockSize2 > 0)
                    juce::FloatVectorOperations::copy (dest + scope.blockSize1, ringData[ch] + scope.startIndex2, scope.blockSize2);
            }
        }

        if (! writer->writeFromFloatArrays (chunk.getArrayOfReadPointers(), channels, todo))
            lostSamples.store (true);

        written += todo;
        frames -= todo;
    }
}

void CaptureRecorder::discardFrames (int frames)
{
    const auto scope = sampleFifo.read (frames);
    juce::ignoreUnused (scope);
}

void CaptureRecorder::finish()
{
    writer.reset();     // flushes and closes the WAV

    Finished done;
    done.any = true;
    done.complete = written > 0 && ! lostSamples.exchange (false);
    current.lengthSamples = written;

    if (done.complete)
    {
        // Pro Tools does not tell an AAX plug-in the handle length it rendered with.
        current.handleSamples = juce::roundToInt (defaultHandleSeconds * current.sampleRate);
        current.handleAssumed = true;

        // The JSON goes last, and by a temporary name, so a capture with a JSON is always whole.
        const auto temporary = current.json.withFileExtension (".json.part");

        if (! temporary.replaceWithText (toJson (current)) || ! temporary.moveFileTo (current.json))
        {
            temporary.deleteFile();
            done.complete = false;
        }
    }

    if (! done.complete)
    {
        current.wav.deleteFile();
        current.json.deleteFile();
    }

    done.info = current;

    {
        const std::lock_guard<std::mutex> l (finishedLock);
        last = done;
    }

    if (onFinished)
        onFinished (done);

    if (done.complete && notify.load())
        postCaptureNotification (current.json);

    finished.fetch_add (1);
    written = 0;
}

} // namespace tt::audiosuite

// The AudioSuite capture plug-in's processor, driven as Pro Tools drives it in a Render: offline
// (setNonRealtime (true)), in blocks of odd sizes, then the end of the Render. The WAV it writes and
// the audio it passes on must both equal the input, sample for sample. Also: a Render that pauses
// (still one capture), a Render of two passes, a Render that ends by going quiet (the fallback),
// Preview (nothing recorded), mono, a long Render that fills the recorder's ring, the placing of a
// capture on the timeline, and which files in the captures folder are the plug-in's.
// main() returns non-zero on a failure.

// CoreFoundation first: after JuceHeader.h's "using namespace juce", its Point would be ambiguous.
#if defined (__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include <JuceHeader.h>

#include "plugin/CaptureHandOff.h"
#include "plugin/CaptureProcessor.h"

#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

using namespace tt::audiosuite;

namespace
{
    int failures = 0;

    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;

        if (! ok)
            ++failures;
    }

    // A signal no two samples of which are likely to be equal: noise over a sweep, a different one per channel.
    juce::AudioBuffer<float> makeSignal (int channels, int frames, int seed)
    {
        juce::AudioBuffer<float> b (channels, frames);
        juce::Random random (seed);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, 0.5f * std::sin (0.001f * (float) i * (float) (ch + 1) + 0.00000002f * (float) i * (float) i)
                                        + 0.3f * (random.nextFloat() * 2.0f - 1.0f));

        return b;
    }

    bool sameBits (const float* a, const float* b, int n)
    {
        return std::memcmp (a, b, sizeof (float) * (size_t) n) == 0;
    }

    struct RunResult
    {
        bool outputExact = true;
        juce::int64 frames = 0;
    };

    // Feeds `signal` through processBlock in the given block sizes, in turn, checking each output block.
    // With `pauseEvery`, it sleeps `pauseMs` after every that many blocks, as a host reading from a
    // slow drive would.
    RunResult feed (CaptureProcessor& p, const juce::AudioBuffer<float>& signal, const std::vector<int>& sizes,
                    int pauseEvery = 0, int pauseMs = 0)
    {
        RunResult r;
        const int channels = signal.getNumChannels();
        juce::AudioBuffer<float> block (channels, 8192);
        juce::MidiBuffer midi;
        size_t next = 0;

        for (int at = 0; at < signal.getNumSamples();)
        {
            const int n = juce::jmin (sizes[next++ % sizes.size()], signal.getNumSamples() - at);
            block.setSize (channels, n, false, false, true);

            for (int ch = 0; ch < channels; ++ch)
                block.copyFrom (ch, 0, signal, ch, at, n);

            p.processBlock (block, midi);

            for (int ch = 0; ch < channels; ++ch)
                r.outputExact = r.outputExact && sameBits (block.getReadPointer (ch), signal.getReadPointer (ch, at), n);

            at += n;
            r.frames += n;

            if (pauseEvery > 0 && next % (size_t) pauseEvery == 0)
                std::this_thread::sleep_for (std::chrono::milliseconds (pauseMs));
        }

        return r;
    }

    // Every capture a recorder finishes, in order (set before the first block).
    struct Collected
    {
        std::mutex lock;
        std::vector<CaptureRecorder::Finished> all;

        void attach (CaptureRecorder& recorder)
        {
            recorder.onFinished = [this] (const CaptureRecorder::Finished& f)
            {
                const std::lock_guard<std::mutex> l (lock);
                all.push_back (f);
            };
        }

        std::vector<CaptureRecorder::Finished> get()
        {
            const std::lock_guard<std::mutex> l (lock);
            return all;
        }
    };

    bool waitForFinished (CaptureProcessor& p, int count, int timeoutMs)
    {
        for (int waited = 0; waited < timeoutMs; waited += 10)
        {
            if (p.getRecorder().finishedCount() >= count)
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        return p.getRecorder().finishedCount() >= count;
    }

    // Whether the WAV holds exactly `signal`, as 32-bit float.
    bool wavEquals (const juce::File& file, const juce::AudioBuffer<float>& signal, juce::String& detail)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (file.createInputStream().release(), true));

        if (reader == nullptr)
        {
            detail = "unreadable";
            return false;
        }

        detail = juce::String (reader->numChannels) + " ch, " + juce::String (reader->lengthInSamples) + " frames, "
                 + juce::String (reader->bitsPerSample) + "-bit" + (reader->usesFloatingPointData ? " float" : "");

        if ((int) reader->numChannels != signal.getNumChannels() || reader->lengthInSamples != signal.getNumSamples()
            || ! reader->usesFloatingPointData || reader->bitsPerSample != 32)
            return false;

        juce::AudioBuffer<float> read (signal.getNumChannels(), signal.getNumSamples());
        reader->read (&read, 0, signal.getNumSamples(), 0, true, true);

        for (int ch = 0; ch < signal.getNumChannels(); ++ch)
            if (! sameBits (read.getReadPointer (ch), signal.getReadPointer (ch), signal.getNumSamples()))
            {
                detail << ", channel " << ch << " differs";
                return false;
            }

        return true;
    }

    int filesIn (const juce::File& folder)
    {
        return folder.getNumberOfChildFiles (juce::File::findFiles);
    }

    void checkCapture (CaptureProcessor& p, const juce::AudioBuffer<float>& signal, double rate, const juce::String& label)
    {
        const auto last = p.getRecorder().lastFinished();
        check (last.any && last.complete, label + ": capture finished and complete");

        juce::String detail;
        check (wavEquals (last.info.wav, signal, detail), label + ": WAV equals the input exactly (" + detail + ")");

        CaptureInfo info;
        juce::String problem;
        const bool read = readCaptureInfo (last.info.json, info, problem);
        check (read, label + ": JSON reads back" + (read ? juce::String() : " (" + problem + ")"));
        check (info.lengthSamples == signal.getNumSamples() && info.channels == signal.getNumChannels() && juce::exactlyEqual (info.sampleRate, rate),
               label + ": JSON says " + juce::String (info.lengthSamples) + " frames, " + juce::String (info.channels)
                   + " ch at " + juce::String (info.sampleRate));
        check (info.handleSamples == juce::roundToInt (2.0 * rate) && info.handleAssumed,
               label + ": handle " + juce::String (info.handleSamples) + " samples, assumed");
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("TempoTantrumCaptureTests-" + juce::String (juce::Time::currentTimeMillis()));
    folder.deleteRecursively();
    folder.createDirectory();

    const double rate = 48000.0;
    const std::vector<int> oddSizes { 1, 7, 513, 1024, 4093, 2, 333, 8191, 64 };

    {
        std::cout << "Stereo Render, odd blocks, ended by setNonRealtime (false)" << std::endl;
        CaptureProcessor p (folder);
        p.getRecorder().notify = false;
        p.setRateAndBufferSizeDetails (rate, 8192);
        p.prepareToPlay (rate, 8192);
        p.setNonRealtime (true);

        // Two seconds of handle, a selection, two seconds of handle, and an odd number of frames.
        const auto signal = makeSignal (2, (int) (rate * 7.3) + 37, 1);
        const auto run = feed (p, signal, oddSizes);
        check (run.outputExact, "output equals the input exactly (" + juce::String (run.frames) + " frames)");

        p.setNonRealtime (false);
        check (waitForFinished (p, 1, 5000), "the Render's end closed the capture");
        checkCapture (p, signal, rate, "stereo");

        // Preview afterwards: real time, nothing recorded.
        std::cout << "Preview (real time)" << std::endl;
        const int before = filesIn (folder);
        const auto preview = makeSignal (2, 48000, 2);
        const auto previewRun = feed (p, preview, oddSizes);
        check (previewRun.outputExact, "Preview output equals the input exactly");
        std::this_thread::sleep_for (std::chrono::milliseconds (800));
        check (p.getRecorder().finishedCount() == 1 && filesIn (folder) == before, "Preview recorded nothing");

        // A second Render that pauses for longer than half a second part way, as a slow drive would,
        // and is then ended by the host: one capture, not two.
        std::cout << "Second Render, with pauses of 450 ms, ended by setNonRealtime (false)" << std::endl;
        p.setNonRealtime (true);
        const auto second = makeSignal (2, 1000000, 3);
        const auto secondRun = feed (p, second, { 1024 }, 300, 450);
        check (secondRun.outputExact, "output equals the input exactly");
        p.setNonRealtime (false);
        check (waitForFinished (p, 2, 5000), "the Render's end closed the capture");
        std::this_thread::sleep_for (std::chrono::milliseconds (300));
        check (p.getRecorder().finishedCount() == 2, "the pauses did not split it (" + juce::String (p.getRecorder().finishedCount() - 1)
                                                         + " capture(s) for this Render)");
        checkCapture (p, second, rate, "paused");

        p.releaseResources();
    }

    {
        // JUCE's AAX wrapper turns AudioSuite's StartPass and StopPass into setNonRealtime (true)
        // and (false). The second pass starts at once, and the writer falls behind in it now and
        // then; the first pass's end must not close the second capture.
        std::cout << "A Render of two passes, the writer behind in the second" << std::endl;
        CaptureProcessor p (folder);
        p.getRecorder().notify = false;
        Collected collected;
        collected.attach (p.getRecorder());
        p.setRateAndBufferSizeDetails (rate, 1024);
        p.prepareToPlay (rate, 1024);
        p.setNonRealtime (true);
        p.setNonRealtime (true);

        const auto first = makeSignal (2, 1000000, 11);
        const auto second = makeSignal (2, 1000000, 12);
        const auto run1 = feed (p, first, { 1024 });
        p.setNonRealtime (false);
        p.setNonRealtime (true);
        const auto run2 = feed (p, second, { 1024 }, 200, 20);
        p.setNonRealtime (false);
        check (run1.outputExact && run2.outputExact, "output equals the input exactly in both passes");
        check (waitForFinished (p, 2, 10000), "both passes were closed");
        std::this_thread::sleep_for (std::chrono::milliseconds (300));

        const auto all = collected.get();
        check (all.size() == 2, juce::String ((int) all.size()) + " captures for 2 passes");

        if (all.size() == 2)
        {
            juce::String detail;
            check (all[0].complete && wavEquals (all[0].info.wav, first, detail), "pass 1: WAV equals its input exactly (" + detail + ")");
            check (all[1].complete && wavEquals (all[1].info.wav, second, detail), "pass 2: WAV equals its input exactly (" + detail + ")");
        }
    }

    {
        // A host that never says the Render has ended: the long gap closes the capture. The gaps
        // are made short here so the test does not wait for the real ones.
        std::cout << "A Render that is never ended, closed by the fallback gap" << std::endl;
        CaptureProcessor p (folder, CaptureRecorder::Timing { 400.0, 500.0 });
        p.getRecorder().notify = false;
        p.setRateAndBufferSizeDetails (rate, 8192);
        p.prepareToPlay (rate, 8192);
        p.setNonRealtime (true);
        const auto signal = makeSignal (2, 100003, 13);
        const auto run = feed (p, signal, { 997, 3, 1500 });
        check (run.outputExact, "output equals the input exactly");
        check (waitForFinished (p, 1, 3000), "the gap closed the capture");
        checkCapture (p, signal, rate, "gap end");
    }

    {
        std::cout << "Mono Render at 44.1 kHz, ended by releaseResources()" << std::endl;
        CaptureProcessor p (folder);
        p.getRecorder().notify = false;
        CaptureProcessor::BusesLayout mono;
        mono.inputBuses.add (juce::AudioChannelSet::mono());
        mono.outputBuses.add (juce::AudioChannelSet::mono());
        check (p.setBusesLayout (mono), "mono layout taken");
        p.setRateAndBufferSizeDetails (44100.0, 4096);
        p.prepareToPlay (44100.0, 4096);
        p.setNonRealtime (true);

        const auto signal = makeSignal (1, 44100 * 3 + 11, 4);
        const auto run = feed (p, signal, { 4096, 17, 1023 });
        check (run.outputExact, "output equals the input exactly");
        p.releaseResources();
        check (waitForFinished (p, 1, 5000), "releaseResources() closed the capture");
        checkCapture (p, signal, 44100.0, "mono");
    }

    {
        std::cout << "Long Render as fast as it can go (the ring fills and the block waits for room)" << std::endl;
        CaptureProcessor p (folder);
        p.getRecorder().notify = false;
        p.setRateAndBufferSizeDetails (rate, 8192);
        p.prepareToPlay (rate, 8192);
        p.setNonRealtime (true);

        const auto signal = makeSignal (2, (int) (rate * 150.0) + 5, 5);
        const auto started = juce::Time::getMillisecondCounterHiRes();
        const auto run = feed (p, signal, { 8191, 4093, 1024 });
        const auto seconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
        check (run.outputExact, "output equals the input exactly (" + juce::String (run.frames) + " frames in "
                                    + juce::String (seconds, 2) + " s, " + juce::String (150.0 / seconds, 0) + "x real time; the block waited for room "
                                    + juce::String (p.getRecorder().timesWaited()) + " times)");
        p.setNonRealtime (false);
        check (waitForFinished (p, 1, 20000), "the Render's end closed the capture");
        checkCapture (p, signal, rate, "long");
    }

    {
        std::cout << "A ring far too small for the Render (the blocks wait for the writer)" << std::endl;
        CaptureRecorder recorder (folder, 4096);
        recorder.notify = false;
        const auto signal = makeSignal (2, 1000003, 7);
        juce::AudioBuffer<float> block (2, 8192);

        for (int at = 0; at < signal.getNumSamples();)
        {
            const int n = juce::jmin (7919, signal.getNumSamples() - at);
            block.setSize (2, n, false, false, true);

            for (int ch = 0; ch < 2; ++ch)
                block.copyFrom (ch, 0, signal, ch, at, n);

            recorder.record (block, 2, n, rate, {});
            at += n;
        }

        recorder.requestEnd();

        for (int waited = 0; waited < 10000 && recorder.finishedCount() < 1; waited += 10)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));

        const auto last = recorder.lastFinished();
        juce::String detail;
        check (recorder.timesWaited() > 0, "blocks waited for room " + juce::String (recorder.timesWaited()) + " times");
        check (last.complete && wavEquals (last.info.wav, signal, detail), "WAV equals the input exactly (" + detail + ")");
    }

    {
        std::cout << "Placing a capture on the timeline" << std::endl;
        CaptureInfo c;
        c.sampleRate = 48000.0;
        c.channels = 2;
        c.handleSamples = 96000;
        c.lengthSamples = 1825824 + 2 * 96000;

        EditSelection s { true, 2882880, 4708704, {} };
        auto p = placeCapture (c, s);
        check (p.startKnown && p.startSample == 2882880 && p.trimStart == 96000 && p.length == 1825824,
               "selection plus 2 s handles: start " + juce::String (p.startSample) + ", trim " + juce::String (p.trimStart)
                   + ", length " + juce::String (p.length));

        c.lengthSamples = 1825824 + 2 * 96000 + 3;
        p = placeCapture (c, s);
        check (p.startKnown && p.length == 1825824, "3 samples over still matches");

        c.lengthSamples = 1825824;
        p = placeCapture (c, s);
        check (p.startKnown && p.trimStart == 0 && p.length == 1825824, "no handles: start known, nothing trimmed");

        c.lengthSamples = 1825824 + 2 * 48000;
        p = placeCapture (c, s);
        check (! p.startKnown && p.trimStart == 96000 && p.length == 1825824 + 96000 - 192000,
               "1 s handles against an assumed 2 s: start not known (" + p.description + ")");

        p = placeCapture (c, EditSelection { false, 0, 0, "no link" });
        check (! p.startKnown && p.description.contains ("no link"), "no selection: start not known (" + p.description + ")");

        // The trimmed copy of a capture holds exactly that part of it.
        std::cout << "Trimming the handles off a capture" << std::endl;
        CaptureProcessor proc (folder);
        proc.getRecorder().notify = false;
        proc.setRateAndBufferSizeDetails (rate, 4096);
        proc.prepareToPlay (rate, 4096);
        proc.setNonRealtime (true);
        const auto signal = makeSignal (2, 96000 * 2 + 12345, 6);
        feed (proc, signal, { 4096, 3 });
        proc.setNonRealtime (false);
        waitForFinished (proc, 1, 5000);
        const auto last = proc.getRecorder().lastFinished();

        CaptureInfo info;
        juce::String problem;
        readCaptureInfo (last.info.json, info, problem);
        p = placeCapture (info, EditSelection { true, 1000, 1000 + 12345, {} });
        const auto trimmed = folder.getChildFile ("trimmed.wav");
        const bool wrote = writeSelection (info.wav, p.trimStart, p.length, trimmed, problem);
        check (wrote, "the selection was written" + (wrote ? juce::String() : " (" + problem + ")"));

        juce::AudioBuffer<float> expected (2, 12345);
        for (int ch = 0; ch < 2; ++ch)
            expected.copyFrom (ch, 0, signal, ch, 96000, 12345);

        juce::String detail;
        check (wavEquals (trimmed, expected, detail), "it equals the input between the handles exactly (" + detail + ")");
    }

    {
        // The app keeps its own files in the captures folder too: the trimmed copy and the map it
        // saves beside it. Only the plug-in's descriptions are captures.
        std::cout << "Which files are capture descriptions" << std::endl;
        bool handled = true;
        check (isCaptureDescription ("AudioSuite capture 2026-11-24 101500.json", &handled) && ! handled, "a capture, unread");
        check (isCaptureDescription ("AudioSuite capture 2026-11-24 101500 2.json", &handled) && ! handled, "the second capture of a second, unread");
        check (isCaptureDescription ("AudioSuite capture 2026-11-24 101500.handled.json", &handled) && handled, "a capture, used");
        check (! isCaptureDescription ("AudioSuite capture 2026-11-24 101500 selection.wav.tempotantrum.json"), "the map of a trimmed copy is not");
        check (! isCaptureDescription ("AudioSuite capture 2026-11-24 101500.wav.tempotantrum.json"), "the map of a capture is not");
        check (! isCaptureDescription ("AudioSuite capture 2026-11-24 101500.json.part"), "a description being written is not");
        check (! isCaptureDescription ("Pro Tools capture 2026-11-24 101500.json"), "another prefix is not");
    }

   #if JUCE_MAC
    {
        // The notification the plug-in posts reaches an observer like the app's, with the JSON's path.
        std::cout << "The distributed notification" << std::endl;
        juce::String received;
        auto center = CFNotificationCenterGetDistributedCenter();
        auto name = CFStringCreateWithCString (kCFAllocatorDefault, notificationName, kCFStringEncodingUTF8);
        CFNotificationCenterAddObserver (center, &received,
                                         [] (CFNotificationCenterRef, void* observer, CFNotificationName, const void* object, CFDictionaryRef)
                                         {
                                             if (object != nullptr && CFGetTypeID (object) == CFStringGetTypeID())
                                                 *static_cast<juce::String*> (observer) = juce::String::fromCFString (static_cast<CFStringRef> (object));
                                         },
                                         name, nullptr, CFNotificationSuspensionBehaviorDeliverImmediately);
        const auto path = folder.getChildFile ("AudioSuite capture test.json");
        postCaptureNotification (path);

        for (int i = 0; i < 50 && received.isEmpty(); ++i)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.1, true);

        CFNotificationCenterRemoveEveryObserver (center, &received);
        CFRelease (name);
        check (received == path.getFullPathName(), "received with the path: " + received);
    }
   #endif

    folder.deleteRecursively();

    std::cout << (failures == 0 ? "All capture tests passed" : juce::String (failures) + " capture test(s) FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}

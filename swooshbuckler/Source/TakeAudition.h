#pragma once

#include <JuceHeader.h>

#include <atomic>

/**
 * Plays a recording back through the output, on top of whatever the rig is doing (after the tape, so it is never
 * recorded). For auditioning takes.
 *
 * The file is read and converted to the device rate on a background thread into a ring; the audio thread only
 * takes from the ring (no locks, no file access, no allocation). Starting and stopping swap the stream under a
 * spin lock the audio thread only tries, and a stop fades out over a few milliseconds.
 */
class TakeAudition : private juce::TimeSliceClient
{
public:
    TakeAudition();
    ~TakeAudition() override;

    /** The device's sample rate (stops anything playing). Not while process() may run with the old rate. */
    void prepare(double deviceRate);

    /** Message thread. play() replaces whatever was playing. */
    bool play(const juce::File& file, juce::String& error);
    void stop();
    /** Stops and lets go of the file (after the short fade if audio is running), so it can be moved or deleted. */
    void close();
    bool isPlaying() const;
    juce::File getFile() const;
    /** How far through the file it is, 0..1. */
    double getProgress() const;

    /** Audio thread: adds the playback to numSamples samples of every channel of out from sample start. */
    void process(juce::AudioBuffer<float>& out, int start, int numSamples);

private:
    struct Stream
    {
        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::File file;
        juce::AudioBuffer<float> ring { 2, 1 }, in, converted;
        std::unique_ptr<juce::AbstractFifo> fifo;
        std::array<juce::LagrangeInterpolator, 2> resamplers;
        double ratio = 1.0;                 // file samples per device sample
        juce::int64 readPos = 0;            // next file sample to read (fill thread)
        std::atomic<bool> sourceDone { false }, stopping { false }, finished { false };
        std::atomic<juce::int64> played { 0 };
        juce::int64 total = 1;              // device samples in the whole file
        float gain = 0.0f;                  // audio thread: fades in at the start and out at a stop
    };

    int useTimeSlice() override;
    static void fill(Stream& stream);

    juce::TimeSliceThread m_thread { "Swooshbuckler audition" };
    juce::AudioFormatManager m_formats;
    double m_deviceRate = 48000.0;
    mutable juce::CriticalSection m_streamLock; // the message thread and the fill thread
    std::shared_ptr<Stream> m_stream;           // guarded by m_streamLock
    juce::SpinLock m_liveLock;                  // the audio thread tries it; the message thread holds it to swap
    Stream* m_live = nullptr;                   // guarded by m_liveLock

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TakeAudition)
};

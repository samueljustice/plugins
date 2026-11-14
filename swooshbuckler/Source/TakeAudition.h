#pragma once

#include <JuceHeader.h>

#include "Room.h"

#include <array>
#include <atomic>

/**
 * Plays a recording back through the output, on top of whatever the rig is doing (after the tape, so it is never
 * recorded). For auditioning takes.
 *
 * The file is read and converted to the device rate on a background thread into a ring; the audio thread only
 * takes from the ring (no locks, no file access, no allocation). Starting and stopping swap the stream under a
 * spin lock the audio thread only tries, and a stop fades out over a few milliseconds.
 *
 * Any take plays at any output width: it is read in processing order (TakeFile: a Multi-Mono set as one take, an
 * interleaved file in whatever Channel Order it was written in), each speaker goes to the same speaker of the
 * output, a wider take is folded down (room::fold), and a narrower one's speakers that the output does not have go
 * to their neighbours (5.1's surrounds to the sides and rears of 7.1, 7.1.2's tops to the tops of 7.1.4). Mono
 * plays from L and R, as it always did.
 */
class TakeAudition : private juce::TimeSliceClient
{
public:
    TakeAudition();
    ~TakeAudition() override;

    /** The device's sample rate and the output's width (stops anything playing). Not while process() may run with
        the old ones. */
    void prepare(double deviceRate, room::Width outputWidth = room::Width::Stereo);

    /** How much of each take channel (processing order) reaches each output channel: gain[out][take]. */
    struct Mix
    {
        int numIn = 0, numOut = 0;
        std::array<std::array<float, room::c_maxChannels>, room::c_maxChannels> gain {};
    };
    /** takeChannels that are not one of the widths' counts play their first two channels as L and R. */
    static Mix mixFor(int takeChannels, room::Width outputWidth);

    /** Message thread. play() replaces whatever was playing. */
    bool play(const juce::File& file, juce::String& error);
    void stop();
    /** Stops and lets go of the file (after the short fade if audio is running), so it can be moved or deleted. */
    void close();
    bool isPlaying() const;
    juce::File getFile() const;
    /** How far through the file it is, 0..1. */
    double getProgress() const;

    /** Reads ahead now, as the reader thread does every few milliseconds. Not the audio thread. For whatever runs
        the audio faster than real time (an offline render, the tests), where that thread cannot be counted on to
        keep ahead. */
    void topUp();

    /** Audio thread: adds the playback to numSamples samples of every channel of out from sample start. */
    void process(juce::AudioBuffer<float>& out, int start, int numSamples);

private:
    struct Stream
    {
        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::File file;
        juce::AudioBuffer<float> ring { 2, 1 }, in, converted;
        std::unique_ptr<juce::AbstractFifo> fifo;
        int channels = 2;                   // the take's
        std::array<juce::LagrangeInterpolator, room::c_maxChannels> resamplers;
        struct Route
        {
            int out = 0, in = 0;
            float gain = 0.0f;
        };
        std::array<Route, room::c_maxChannels * room::c_maxChannels> routes {};
        int numRoutes = 0, outChannels = 2;
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
    double m_deviceRate = 48000.0;
    room::Width m_outputWidth = room::Width::Stereo;
    mutable juce::CriticalSection m_streamLock; // the message thread and the fill thread
    std::shared_ptr<Stream> m_stream;           // guarded by m_streamLock
    juce::SpinLock m_liveLock;                  // the audio thread tries it; the message thread holds it to swap
    Stream* m_live = nullptr;                   // guarded by m_liveLock

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TakeAudition)
};

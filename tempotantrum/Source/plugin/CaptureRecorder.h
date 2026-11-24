#pragma once

#include "CaptureHandOff.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

// Records what an AudioSuite Render streams through the plug-in to a WAV, as it arrives.
//
// The audio thread copies each block into a fixed ring of samples and queues a small event
// saying how many frames it wrote (or that a capture starts or ends); it takes no lock and
// allocates nothing. A writer thread empties the ring into the WAV and, when the capture ends,
// writes the JSON description beside it and posts the notification for Tempo Tantrum.
//
// A capture ends at whichever comes first: requestEnd() (the plug-in calls it from setNonRealtime
// (false), which JUCE's AAX wrapper calls at the end of each AudioSuite pass, and from reset() and
// releaseResources()), a real-time block, or a change of channel count or rate. Only as a fallback
// for a host that never says a Render has ended does a long gap end it (Timing): a Render that
// pauses, on a slow drive say, stays one capture.
//
// A Render runs far faster than real time, so the writer can fall behind. When the ring is full the
// audio thread waits for room (in short sleeps, without a lock) rather than lose samples: a Render
// is not real time, and Pro Tools simply waits for the block. After 30 s without room the capture
// is abandoned and the audio passes on unrecorded.
namespace tt::audiosuite
{

// The fallback gaps. A block this long after the last one starts a new capture (audio thread);
// the writer closes a capture after a little longer with nothing queued, so it never closes one
// the audio thread still counts as running.
struct CaptureTiming
{
    double newCaptureAfterMs = 20000.0;
    double closeIdleCaptureAfterMs = 25000.0;
};

class CaptureRecorder : private juce::Thread
{
public:
    static constexpr int maxChannels = 2;

    using Timing = CaptureTiming;

    // Captures go in `folder`. The ring holds `capacityFrames` frames of every channel.
    explicit CaptureRecorder (juce::File folder, int capacityFrames = 1 << 19, Timing timing = {});
    ~CaptureRecorder() override;

    //==============================================================================
    // Audio thread only.

    // Records the first `numChannels` channels of one block of a Render. `facts` is read only when
    // the block starts a capture.
    void record (const juce::AudioBuffer<float>& block, int numChannels, int numSamples, double sampleRate, const PlayHeadFacts& facts);

    // A block that is not recorded (Preview): ends a capture in progress.
    void notRecording();

    //==============================================================================
    // Any thread.

    // Ends the capture in progress, if any.
    void requestEnd();

    struct Finished
    {
        bool any = false;
        bool complete = false;      // false: abandoned, nothing was handed over
        CaptureInfo info;
    };

    Finished lastFinished() const;
    int finishedCount() const       { return finished.load(); }

    // How many times a block has had to wait for the writer (for tests and diagnosis).
    int timesWaited() const         { return waits.load(); }

    // Called on the writer thread after each capture is written (before the notification).
    // Set it before the first block.
    std::function<void (const Finished&)> onFinished;

    // Whether to post the distributed notification for each capture (tests turn it off).
    std::atomic<bool> notify { true };

    juce::File getFolder() const    { return folder; }

private:
    enum class EventType { start, data, end };

    struct Event
    {
        EventType type = EventType::data;
        int frames = 0;
        int channels = 0;
        double sampleRate = 0.0;
        PlayHeadFacts facts;
        juce::uint32 endRequests = 0;   // start: the end requests the audio thread had seen
    };

    const juce::File folder;
    const Timing timing;

    // The audio thread writes, the writer reads.
    juce::AbstractFifo sampleFifo;
    juce::AudioBuffer<float> ring;
    float* ringData[maxChannels] {};
    juce::AbstractFifo eventFifo { 4096 };
    std::vector<Event> events;

    // The audio thread's own state.
    bool capturing = false;
    bool abandoned = false;         // the capture in progress gave up; blocks pass until it ends
    int captureChannels = 0;
    double captureRate = 0.0;
    double lastBlockMs = 0.0;
    juce::uint32 endRequestsSeen = 0;

    std::atomic<juce::uint32> endRequests { 0 };
    std::atomic<bool> lostSamples { false };     // set by the audio thread when it gave up on a capture

    // The writer's state.
    std::unique_ptr<juce::AudioFormatWriter> writer;
    CaptureInfo current;
    juce::int64 written = 0;
    double lastEventMs = 0.0;
    juce::uint32 openEndRequests = 0;   // the end requests seen when the open capture started
    juce::AudioBuffer<float> chunk;

    mutable std::mutex finishedLock;
    Finished last;
    std::atomic<int> finished { 0 };
    std::atomic<int> waits { 0 };

    bool pushEvent (const Event&);
    template <typename HasRoom>
    bool waitForRoom (HasRoom hasRoom);

    void run() override;
    bool drainOnce();
    void begin (const Event&);
    void append (int frames);
    void finish();
    void discardFrames (int frames);
};

} // namespace tt::audiosuite

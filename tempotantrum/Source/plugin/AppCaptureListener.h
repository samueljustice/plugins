#pragma once

#include "CaptureHandOff.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

// The app's side of the AudioSuite hand-off. Made once the window is up: it listens for the
// plug-in's notification and, at start, takes the newest capture the app has not used yet.
//
// Where a capture starts on the Pro Tools timeline is taken from an edit selection only when that
// selection is surely the one rendered:
//  - while "Get from Pro Tools" renders with the plug-in (capture/AudioSuiteRoute.h), captures are
//    held and not read: the route decides which one opens, and places it against the selection it
//    read before Render;
//  - a capture from a Render started by hand is placed against the edit selection read through
//    the scripting link when it arrives, if its description was written in the last few seconds;
//  - a capture rendered while the app was closed is never placed: its start is not known.
// When the capture without its handles is not as long as the selection, the start is not known
// either, and the note says so. The handles are trimmed off into "<capture> selection.wav", and
// the result goes to the capture inbox (capture/CaptureInbox.h), which opens it as the music.
//
// One Render can make several captures, one for each pass (several tracks, multi-mono, clip by
// clip). Captures that come in close together are taken as one Render: the first is opened with
// its start not known, and the note says why.
namespace tt::audiosuite
{

class AppCaptureListener : private juce::Thread
{
public:
    AppCaptureListener();
    ~AppCaptureListener() override;

    // Queues a capture by the path of its JSON. Only the plug-in's own descriptions in the captures
    // folder are taken: anything on the Mac can post the notification. False when it was not taken.
    bool take (const juce::String& jsonPath);

    //==============================================================================
    // For "Get from Pro Tools" while it renders with Tempo Tantrum Capture. Any thread.

    // From now on captures are held: nothing is read from Pro Tools and nothing is opened.
    static void startHolding();

    // The captures held so far, in the order they came.
    static std::vector<juce::File> heldCaptures();

    // Ends the holding. `open`, one of the held captures, is opened, placed against `selection`
    // (the edit selection that was rendered) and named as from `sessionName`; every other held
    // capture is marked as used. With no `open`, none is opened. Captures that come in during the
    // next `ignoreLaterForMs` are marked as used and not opened either (a render that was still
    // running when the route gave up).
    static void stopHolding (const juce::File& open = {}, const EditSelection& selection = {}, const juce::String& sessionName = {},
                             int ignoreLaterForMs = 0);

private:
    enum class Kind { live, atStart, placed };

    struct Job
    {
        juce::File json;
        Kind kind = Kind::live;
        EditSelection selection;
        juce::String sessionName;
    };

    std::mutex queueLock;
    std::deque<Job> queue;
    double lastLiveMs = 0.0;

    void add (Job job);
    void run() override;
    void handle (const Job& job, int capturesInRender);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppCaptureListener)
};

} // namespace tt::audiosuite

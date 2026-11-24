#pragma once

#include <JuceHeader.h>

#include <functional>

// Where captured music comes in. A capture is a WAV of exactly the edit selection in Pro Tools as
// it sounds on the selected track(s), at the session's rate, with the sample of the timeline it
// starts at. Tempo Tantrum opens it as the music, and the music's time 0 is that sample, so Write
// to Pro Tools needs no clip matching for it.
//
// "Get from Pro Tools" (an offline Track Bounce through the scripting link) delivers its captures
// here, and so can any other route that makes the same kind of file (the AudioSuite capture).
namespace tt::capture
{

struct Capture
{
    juce::File audio;               // the WAV
    long long startSample = 0;      // where its first sample is on the Pro Tools timeline (session samples)
    bool startKnown = true;         // false (or a negative startSample) when where it came from is not known
    long long endSample = 0;        // the end of the selection it was made from (0: not known)
    double sessionRate = 0.0;       // the session's sample rate (0: not known)
    juce::String sessionName;       // the session it was captured in (empty: not known)
    juce::String name;              // shown in the header: "<track name> <start>"
    juce::String source;            // how it was made, for the status line: "an offline Track Bounce of Music"
    juce::String note;              // anything more to say in the status line, or empty
};

// The window that opens captures registers here (on the message thread); nullptr unregisters.
void setReceiver (std::function<void (const Capture&)> receiver);

// Opens a capture as the music in the window. Can be called from any thread: the capture is
// handed over on the message thread. False when no window has registered to take it.
bool deliver (const Capture& capture);

} // namespace tt::capture

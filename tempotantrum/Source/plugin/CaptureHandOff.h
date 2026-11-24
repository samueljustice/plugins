#pragma once

#include <JuceHeader.h>

#include <optional>

// What the Tempo Tantrum Capture plug-in and the Tempo Tantrum app agree on: where captures go,
// what the description beside each one says, the notification that tells the app about one, and
// how the app places a capture on the Pro Tools timeline. Built into both the plug-in and the app.
//
// A capture is the audio an AudioSuite Render streamed through the plug-in: the edit selection
// with the AudioSuite handles before and after it. The plug-in writes it as a 32-bit float WAV and
// then a JSON file beside it with the same name; the JSON is written last, so a capture whose JSON
// exists is complete. The app renames the JSON to "<name>.handled.json" once it has used it.
namespace tt::audiosuite
{

// The macOS distributed notification posted when a capture is finished. Its object is the full
// path of the capture's JSON file.
inline constexpr const char* notificationName = "com.samueljustice.tempotantrum.capture";

// ~/Library/Caches/Sammy J/Tempo Tantrum/Captures (shared with "Get from Pro Tools", whose files
// have a prefix of their own).
juce::File capturesFolder();

// Every AudioSuite capture's file name starts with this.
inline constexpr const char* filePrefix = "AudioSuite capture";

// What Pro Tools puts on each side of the selection in an AudioSuite Render when nothing else is
// known: its default handle length.
inline constexpr double defaultHandleSeconds = 2.0;

struct PlayHeadFacts
{
    bool hasBpm = false;
    double bpm = 0.0;
    bool hasTimeSignature = false;
    int numerator = 4, denominator = 4;
    bool hasPpq = false;
    double ppqPosition = 0.0;
    bool hasTimeInSamples = false;
    juce::int64 timeInSamples = 0;
};

struct CaptureInfo
{
    juce::File json, wav;
    double sampleRate = 0.0;
    int channels = 0;
    juce::int64 lengthSamples = 0;
    juce::int64 handleSamples = 0;      // on each side of the selection
    bool handleAssumed = true;          // the default handle length, not one Pro Tools reported
    PlayHeadFacts playHead;
    juce::Time captured;
    juce::String plugInVersion;
};

// The description as JSON text, and back. Reading fails, with a sentence, when the file is not a
// capture description or its WAV is missing.
juce::String toJson (const CaptureInfo&);
bool readCaptureInfo (const juce::File& json, CaptureInfo& info, juce::String& problem);

// Whether a file name is a capture description the plug-in wrote, "<filePrefix> <date> <time>[ n].json",
// used or not. The app's own files in the folder (the trimmed copy and the map saved beside it,
// "<capture> selection.wav.tempotantrum.json") are not.
bool isCaptureDescription (const juce::String& fileName, bool* handled = nullptr);

// The captures whose JSON the app has not used yet, oldest first.
juce::Array<juce::File> unreadCaptures();

// Marks a capture as used: its JSON becomes "<name>.handled.json".
void markHandled (const juce::File& json);

// Removes captures (WAV, JSON and any trimmed copy) that were handled more than `days` ago.
void removeOldCaptures (int days);

// Tells Tempo Tantrum, if it is running, that a capture is ready. Does nothing elsewhere than macOS.
void postCaptureNotification (const juce::File& json);

//==============================================================================
// Where a capture sits on the timeline, from the edit selection Pro Tools gives when it arrives.
struct Placement
{
    bool startKnown = false;
    juce::int64 startSample = 0;        // the selection's start on the timeline, when known

    juce::int64 trimStart = 0;          // the part of the capture that is the selection
    juce::int64 length = 0;

    juce::String description;           // one or two plain sentences for the person
};

struct EditSelection
{
    bool known = false;
    juce::int64 in = 0, out = 0;        // samples; out is the first sample after the selection
    juce::String problem;               // why it is not known
};

// With the selection known and as long as the capture without its handles (or as the whole capture,
// when it has none), within `tolerance` samples, the start is the selection's start and the handles
// are trimmed. Otherwise the handles are still trimmed when the capture is longer than both of them,
// and the start is not known.
Placement placeCapture (const CaptureInfo& capture, const EditSelection& selection, juce::int64 tolerance = 4);

// The part of a capture that is the selection, as a 32-bit float WAV of its own: `trimStart` and `length` in
// samples. False, with a sentence, when it could not be written.
bool writeSelection (const juce::File& source, juce::int64 trimStart, juce::int64 length, const juce::File& destination,
                     juce::String& problem);


} // namespace tt::audiosuite

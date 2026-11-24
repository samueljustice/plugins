#pragma once

#include "CaptureInbox.h"
#include "CaptureMaths.h"
#include "ptsl/PtslClient.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// "Get from Pro Tools": renders the edit selection of the selected track(s) through the scripting
// link's offline Track Bounce, into the cache folder, and makes one WAV of it to open as the music.
// Nothing is imported into the session and nothing in the session is changed: the selection, the tracks and the settings are only read. Several selected tracks are
// each rendered and summed into one mono or stereo file.
//
// Everything here that talks to Pro Tools blocks, so it runs on a background thread.
namespace tt::capture
{

// ~/Library/Caches/Sammy J/Tempo Tantrum/Captures, made if it is not there.
juce::File capturesFolder();

// Deletes the oldest captures (each WAV with the map saved beside it) so that `keep` stay, and
// any folder a capture left behind. Returns how many captures went.
int pruneCaptures (const juce::File& folder, std::size_t keep = capturesKept, const juce::File& keepAlways = {});

// One track as Pro Tools rendered it.
struct RenderedTrack
{
    std::string name, id;
    std::vector<std::string> files;     // as Pro Tools listed them
    double seconds = 0.0;               // how long the bounce took
    int channels = 0;
    double sampleRate = 0.0;
    int bitsPerSample = 0;
    bool floatingPoint = false;
    long long length = 0;               // samples in the (first) file
    int outputs = 1;                    // the track's outputs: one file each, of which one is used
};

struct Outcome
{
    bool ok = false;
    juce::String error;                 // a plain sentence for the status line when it failed
    bool useAudioSuite = false;         // this Pro Tools cannot bounce through the link

    Capture capture;
    std::vector<RenderedTrack> tracks;
    long long selectionIn = 0, selectionOut = 0;
    double seconds = 0.0;               // the whole capture
    std::vector<std::string> log;       // what was sent and what came back, for --ptsl-capture
};

struct Options
{
    bool keepRenders = false;           // keep Pro Tools' own files beside the capture (for checking)
    juce::File folder;                  // where captures go; capturesFolder() when empty
};

// The status line while a capture is being made.
inline constexpr const char* renderingText = "Rendering the selection in Pro Tools\xe2\x80\xa6";

// Makes a capture. `progress` is called (on this thread) with a line for the status; `stop`
// ends the wait for Pro Tools early.
Outcome captureSelection (ptsl::Client& client, const std::function<void (const juce::String&)>& progress = {},
                          const std::function<bool()>& stop = {}, const Options& options = {});

// Makes one capture at a time on a thread of its own, for the window. `onProgress` and `onDone`
// are called on the message thread, and never after the job is deleted (which stops a capture
// waiting for Pro Tools and waits for its thread).
class CaptureJob : private juce::Thread
{
public:
    CaptureJob();
    ~CaptureJob() override;

    // False when a capture is already being made.
    bool start (std::function<void (const juce::String&)> onProgress, std::function<void (const Outcome&)> onDone);
    bool isBusy() const                 { return isThreadRunning(); }

private:
    std::function<void (const juce::String&)> progressCallback;
    std::function<void (const Outcome&)> doneCallback;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    void run() override;

    JUCE_DECLARE_NON_COPYABLE (CaptureJob)
};

} // namespace tt::capture

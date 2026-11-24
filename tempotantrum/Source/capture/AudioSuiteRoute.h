#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// "Get from Pro Tools" when the offline Track Bounce cannot be used: Tempo Tantrum has Pro Tools
// render the edit selection with the Tempo Tantrum Capture AudioSuite plug-in, takes the capture
// the plug-in hands over, and then undoes the render, so the session is left as it was.
//
// It drives Pro Tools from outside through Accessibility (protools/ProToolsDriver.h, with its
// rules: Pro Tools must be the front application before every press, a stop ends it between steps,
// and everything is written to the log). It presses the AudioSuite menu item, Render once, Edit >
// Undo once (only when the Undo item names AudioSuite and only after a render), and the AudioSuite
// window's close button when it opened that window. No setting in the AudioSuite window is changed.
// When anything it expects is missing it stops without pressing anything more.
//
// The capture itself comes in through the app's listener (plugin/AppCaptureListener.h) and the
// capture inbox, which opens it as the music. Everything here blocks, so it runs on a thread.
namespace tt::capture
{

inline constexpr const char* capturePlugInName = "Tempo Tantrum Capture";

// The question asked once in a run of the app before the first AudioSuite render.
inline constexpr const char* audioSuiteQuestion = "Pro Tools will render the selection with Tempo Tantrum Capture and then undo it. Go ahead?";

struct AudioSuiteOutcome
{
    bool ok = false;                    // look: the plug-in was found. render: captured, undone and closed
    juce::String error;                 // a plain sentence for the status line when it failed
    bool noAccessibility = false;       // macOS has not let Tempo Tantrum control other applications
    bool found = false;                 // Tempo Tantrum Capture is in the AudioSuite menu
    std::string menuPath;               // where it is, "AudioSuite > Other > Tempo Tantrum Capture"
    bool rendered = false;              // Render was pressed and Pro Tools rendered
    bool captured = false;              // the plug-in's capture came in
    bool undone = false;                // Undo was pressed for the render
    bool confirmed = false;             // the scripting link read the clip count and edit selection as before
    std::vector<std::string> log;
};

// Looks for Tempo Tantrum Capture in Pro Tools' AudioSuite menu. Presses nothing.
AudioSuiteOutcome findCapturePlugIn (const std::function<void (const std::string&)>& log, const std::function<bool()>& stop);

// The whole AudioSuite route, as described above. Before Render it needs Pro Tools ready, stopped
// and with one audio track selected (when the scripting link answers) and no dialog open. While
// Pro Tools renders, the app's listener holds the captures that come in; nothing is read from or
// pressed in Pro Tools until its windows are back as they were, and if that takes too long the
// route stops there and says so. Exactly one capture is opened, placed against the edit selection
// read before Render. `progress` gets status lines.
AudioSuiteOutcome renderWithCapturePlugIn (const std::function<void (const std::string&)>& log,
                                           const std::function<void (const juce::String&)>& progress,
                                           const std::function<bool()>& stop);

// The longest wait for the capture after Render: five seconds for each minute of the selection and
// ten more, at most longestCaptureWaitMs. Pro Tools' windows are then given at least a minute, or
// as long again, to close.
inline constexpr int longestCaptureWaitMs = 15 * 60 * 1000;
int captureWaitMilliseconds (double selectionSeconds);

// ~/Library/Logs/Sammy J/Tempo Tantrum/Get from Pro Tools.log: every "Get from Pro Tools" adds to it.
juce::File getFromProToolsLog();

// Adds lines to that log, each with the time. Any thread.
void appendToLog (const std::vector<std::string>& lines);

// Runs one look or one render on a thread of its own, for the window. `onProgress` and `onDone`
// are called on the message thread, and never after the job is deleted. Once Render has been
// pressed the run does not stop early: it goes on to undo the render.
class AudioSuiteJob : private juce::Thread
{
public:
    enum class Task { look, render };

    AudioSuiteJob();
    ~AudioSuiteJob() override;

    bool start (Task task, std::function<void (const juce::String&)> onProgress, std::function<void (const AudioSuiteOutcome&)> onDone);
    bool isBusy() const                 { return isThreadRunning(); }

private:
    Task task = Task::look;
    std::function<void (const juce::String&)> progressCallback;
    std::function<void (const AudioSuiteOutcome&)> doneCallback;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    void run() override;

    JUCE_DECLARE_NON_COPYABLE (AudioSuiteJob)
};

} // namespace tt::capture

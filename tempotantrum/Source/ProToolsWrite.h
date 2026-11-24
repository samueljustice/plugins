#pragma once

#include <JuceHeader.h>

#include "MapFile.h"
#include "core/Types.h"
#include "protools/IdentifyBeatWrite.h"
#include "ui/Controls.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace tt
{

// The sheet behind "Write to Pro Tools". It covers the window's contents with a small card: what
// to do in Pro Tools first and what will be sent, then the progress of the run, then how it ended,
// with the check, the undo and the log.
//
// A run happens on a background thread and drives Pro Tools while Pro Tools is the front
// application. Meanwhile a small panel floats above the other applications with the progress and
// a Stop button. When the run ends Tempo Tantrum comes forward again.
class ProToolsWriteSheet : public juce::Component,
                           private juce::Timer
{
public:
    ProToolsWriteSheet();
    ~ProToolsWriteSheet() override;

    // The pins a run writes and where they come from. `unchecked` is how many places in the map
    // are still listed to check. The sample rate box is filled with the music's rate whenever
    // that changes, and with the session's when Pro Tools' scripting link gives it, until a rate
    // is typed in it.
    void setSource (const std::vector<Pin>& pins, int beatsPerBar, double musicSampleRate, long long musicLengthSamples,
                    const juce::String& musicName, int unchecked);

    // Where the music came from when it was captured from Pro Tools: a write then places it at
    // the start of the selection it was captured from, with no clip to select.
    void setProToolsOrigin (const ProToolsOrigin& origin);

    void open();
    void close();
    bool isRunning() const                              { return runner != nullptr; }

    std::function<void()> onClose;

    //==============================================================================
    // For pictures of the sheet and for trying it without Pro Tools. Any of these stops the sheet
    // from looking at Pro Tools by itself and from showing the floating panel.
    void showPermissionAs (bool allowed);
    void showCounterAs (const pt::CounterReading& reading);
    void showLinkAs (const pt::SessionReading& reading);
    void showMoreOpen (bool open);
    void showRunning (pt::Phase phase, int done, int total);
    void showFinished (const pt::WriteOutcome& outcome, const juce::String& logText);

    // The floating panel as it is during a run, as a picture at twice its size.
    static juce::Image floatingPanelPicture (pt::Phase phase, int done, int total);

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class Runner;
    class Watcher;
    class LinkWatcher;
    class Tick;
    class Disclosure;
    class FloatingPanel;

    enum class Stage { ready, running, finished };

    std::vector<Pin> pins;
    int beatsPerBar = 4;
    int unchecked = 0;
    double shownMusicRate = 0.0;
    long long musicLength = 0;
    juce::String musicName;
    bool rateTyped = false;             // a rate was typed in the box: it is used whatever the session says
    ProToolsOrigin origin;              // for music captured from Pro Tools

    ui::PillButton cancelButton { "Cancel" };
    ui::PillButton writeButton { "Write", true };
    ui::PillButton askButton { "Ask macOS for permission" };
    ui::NumberBox startBox { "Music starts at sample" };
    ui::NumberBox rateBox { "Session sample rate" };
    ui::PillButton stopButton { "Stop" };
    ui::PillButton undoButton { "Undo in Pro Tools" };
    ui::PillButton copyButton { "Copy log" };
    ui::PillButton saveButton { juce::String::fromUTF8 ("Save log\xe2\x80\xa6") };
    ui::PillButton closeButton { "Close" };
    ui::PillButton againButton { "Write again" };
    std::unique_ptr<Disclosure> moreToggle;
    std::unique_ptr<Tick> checkTick, slowTick;

    std::unique_ptr<Runner> runner;
    std::unique_ptr<Watcher> watcher;
    std::unique_ptr<LinkWatcher> linkWatcher;
    std::unique_ptr<FloatingPanel> floating;
    std::unique_ptr<juce::FileChooser> chooser;

    Stage stage = Stage::ready;
    bool forPicture = false;            // set by the show... functions
    bool permissionAllowed = false;
    bool moreOpen = false;

    // What the Main Counter showed when last read, while the sheet is waiting for Write.
    bool counterKnown = false;
    pt::CounterReading counter;

    // What Pro Tools' scripting link said about the session when last read.
    bool linkKnown = false;
    pt::SessionReading link;

    // The run under way, written by its thread and read by the timer.
    std::atomic<int> progressPhase { 0 }, progressDone { 0 }, progressTotal { 0 };
    bool undoing = false;

    // The last run.
    pt::WriteOutcome outcome;
    bool hasOutcome = false;
    int undoSteps = 0;                  // Identify Beat steps of the last run that have not been undone
    juce::String undoLine;              // "Sent 24 undos. ..." once an undo has been sent
    bool undoLineBad = false;
    juce::uint32 confirmUndoUntil = 0;  // while "Really send N undos?" is shown
    juce::uint32 confirmUndoSince = 0;

    // Where the last run that wrote anything put its pins, to warn about writing there again.
    bool hasLastStart = false;
    long long lastStart = 0;
    std::vector<long long> lastPinSamples;
    std::vector<long long> runPinOffsets;   // each pin of the run under way, in samples from the music start

    juce::CriticalSection logLock;
    juce::String pendingLog;            // lines from the run's thread, not yet added to fullLog
    juce::String fullLog;

    std::vector<juce::Rectangle<int>> lineAreas;   // one for each of readyLines() or finishedLines()
    juce::Rectangle<int> card, titleArea, instructionArea, pinsArea, permissionArea,
                         permissionNoteArea, progressTextArea, progressBarArea, progressNoteArea,
                         outcomeArea, undoArea;

    void timerCallback() override;

    void startWrite();
    void startUndo();
    void undoClicked();
    void writeFinished (const pt::WriteOutcome&);
    void undoFinished (const pt::UndoOutcome&);
    void runEnded();
    void requestStop();
    void backToReady();

    void addLineFromThread (const std::string& line);
    void flushLog();
    void copyLog();
    void saveLog();

    void startWatching();
    void stopWatching();
    void setCounter (const pt::CounterReading&);
    void setLink (const pt::SessionReading&);
    pt::MusicFacts musicFacts() const;
    bool linkInUse() const;
    bool captured() const               { return origin.known && origin.startKnown; }
    pt::MusicStart musicStart() const;

    void updateControls();
    void layout();
    void showFloatingPanel();
    void updateFloatingPanel();

    // A line of text on the card: dim, in the text colour, or a warning.
    struct Line
    {
        juce::String text;
        bool warning = false;
        bool strong = false;
    };

    juce::String pinsLine() const;
    juce::String instructionText() const;
    juce::String counterLine (bool& warning) const;
    std::vector<Line> readyLines() const;
    juce::String lastRunNote (long long start, bool typed) const;
    std::vector<Line> finishedLines() const;
    juce::String outcomeText() const;
    static juce::String progressText (pt::Phase phase, int done, int total);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProToolsWriteSheet)
};

} // namespace tt

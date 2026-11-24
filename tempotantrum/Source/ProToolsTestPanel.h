#pragma once

#include <JuceHeader.h>

#include "core/Types.h"
#include "protools/IdentifyBeatTest.h"
#include "ui/Controls.h"

#include <functional>
#include <memory>
#include <vector>

namespace tt
{

// The test of sending pins to Pro Tools with Event > Identify Beat. It covers the window's
// contents: what to do first, the Accessibility permission, the two numbers a run needs, the
// buttons that start a run, and the log of what happened.
//
// A run happens on a background thread and drives Pro Tools while Pro Tools is the front
// application. The panel never brings its own window forward.
class ProToolsTestPanel : public juce::Component,
                          private juce::Timer
{
public:
    ProToolsTestPanel();
    ~ProToolsTestPanel() override;

    // The pins a run sends, and where they come from. The sample rate box is filled with the
    // music's rate whenever that changes.
    void setSource (const std::vector<Pin>& pins, int beatsPerBar, double musicSampleRate, const juce::String& musicName);

    void open();
    void close();
    bool isRunning() const                              { return runner != nullptr; }

    std::function<void()> onClose;

    // For pictures of the panel: shows the permission as allowed or missing whatever it really
    // is, and shows a finished run without running one.
    void showPermissionAs (bool allowed);
    void showFinishedRun (const juce::String& logText, const pt::TestOutcome& outcome);

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class Runner;

    std::vector<Pin> pins;
    int beatsPerBar = 4;
    double shownMusicRate = 0.0;
    juce::String musicName;

    ui::PillButton closeButton { "Close" };
    ui::PillButton askButton { "Ask macOS for permission" };
    ui::NumberBox startBox { "Music starts at sample" };
    ui::NumberBox rateBox { "Session sample rate" };
    ui::SegmentedSwitch entrySwitch;
    ui::PillButton lookButton { "Look only" };
    ui::PillButton oneButton { "Send one pin" };
    ui::PillButton allButton { "Send all pins" };
    ui::PillButton stopButton { "Stop" };
    ui::PillButton copyButton { "Copy log" };
    ui::PillButton saveButton { juce::String::fromUTF8 ("Save log\xe2\x80\xa6") };
    juce::TextEditor logView;

    std::unique_ptr<Runner> runner;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::CriticalSection logLock;
    juce::String pendingLog;            // lines from the run's thread, not yet shown
    juce::String fullLog;

    juce::String summary;
    juce::Colour summaryColour;
    bool permissionAllowed = false;
    bool permissionFixed = false;       // set by showPermissionAs
    juce::uint32 confirmAllUntil = 0;   // while "Really send N pins?" is shown
    juce::uint32 confirmAllSince = 0;   // when it was first shown

    juce::Rectangle<int> card, titleArea, summaryArea, listArea, noteArea, entryCaption,
                         permissionArea, permissionNoteArea, pinsNoteArea;

    void timerCallback() override;

    void start (pt::TestMode mode);
    void allButtonClicked();
    void runFinished (const pt::TestOutcome& outcome);
    void addLineFromThread (const std::string& line);
    void flushLog();
    void appendToLog (const juce::String& text);
    void setSummary (const juce::String& text, juce::Colour colour);
    void updateButtons();
    void copyLog();
    void saveLog();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProToolsTestPanel)
};

} // namespace tt

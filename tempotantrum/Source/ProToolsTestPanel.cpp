#include "ProToolsTestPanel.h"

#include "ui/Theme.h"

#include <cmath>

namespace tt
{

namespace
{
    constexpr int cardMargin = 12;
    constexpr int pad = 16;
    constexpr int leftWidth = 326;
    constexpr int columnGap = 20;
    constexpr int listNumberWidth = 20;
    constexpr int listItemGap = 5;

    const char* const beforeYouStart[] = {
        "Use a scratch session or a copy, not a session in use.",
        "Stop the transport.",
        "Set the Main Counter to Samples.",
        "Put the cursor at the start of the music clip, or type the sample where the music starts.",
        "Do not touch the keyboard or mouse during the run."
    };

    const char* const boxesNote = "Leave the first box empty to read the start from the Main Counter. "
                                  "The sample rate must match the session.";
    const char* const permissionMissing = "macOS has not given Tempo Tantrum permission to control other apps.";
    const char* const permissionGiven = "macOS lets Tempo Tantrum control other apps.";
    const char* const permissionNote = "Then switch Tempo Tantrum on in System Settings > Privacy & Security > Accessibility, and reopen the app.";
    const char* const idleSummary = "Start with Look only. It opens the Identify Beat dialog, writes down what it finds and cancels it.";
    const char* const runningSummary = "Running. Leave the keyboard and mouse alone until it ends. Stop, or Escape in this window, ends the run.";

    juce::TextLayout wrapped (const juce::String& text, const juce::Font& font, juce::Colour colour, int width)
    {
        juce::AttributedString string;
        string.append (text, font, colour);
        string.setWordWrap (juce::AttributedString::byWord);
        string.setJustification (juce::Justification::topLeft);

        juce::TextLayout layout;
        layout.createLayout (string, (float) juce::jmax (20, width));
        return layout;
    }

    int heightOf (const juce::String& text, const juce::Font& font, int width)
    {
        return (int) std::ceil (wrapped (text, font, juce::Colours::white, width).getHeight());
    }

    juce::Font bodyFont()       { return ui::Theme::ui (12.5f); }
    juce::Font summaryFont()    { return ui::Theme::ui (13.0f, true); }
}

//==============================================================================
// One run of the test, on its own thread. Its log lines and its result go back to the panel on
// the message thread.
class ProToolsTestPanel::Runner : public juce::Thread
{
public:
    Runner (ProToolsTestPanel& owner, pt::TestSettings settingsToUse)
        : juce::Thread ("Pro Tools test"), panel (owner), safePanel (&owner), settings (std::move (settingsToUse))
    {
    }

    ~Runner() override
    {
        // Every wait in a run is short and asks whether to stop, no call to Pro Tools waits
        // longer than two seconds, and a walk over many elements ends at the first call that
        // gets no answer. So a stopped run ends within a few calls.
        stopThread (15000);
    }

    void run() override
    {
        const auto outcome = pt::runIdentifyBeatTest (settings,
                                                      [this] (const std::string& line) { panel.addLineFromThread (line); },
                                                      [this] { return threadShouldExit(); });

        juce::MessageManager::callAsync ([safe = safePanel, outcome]
        {
            if (safe != nullptr)
                safe->runFinished (outcome);
        });
    }

private:
    // The panel outlives this thread: its destructor waits for the thread to end.
    ProToolsTestPanel& panel;
    juce::Component::SafePointer<ProToolsTestPanel> safePanel;
    pt::TestSettings settings;
};

//==============================================================================
ProToolsTestPanel::ProToolsTestPanel()
{
    setWantsKeyboardFocus (true);
    setTitle ("Test in Pro Tools");

    for (auto* b : { &closeButton, &askButton, &lookButton, &oneButton, &allButton, &stopButton, &copyButton, &saveButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }

    closeButton.onClick = [this] { close(); };
    askButton.onClick = [] { pt::Driver::requestAccessibility(); };
    lookButton.onClick = [this] { start (pt::TestMode::lookOnly); };
    oneButton.onClick = [this] { start (pt::TestMode::onePin); };
    allButton.onClick = [this] { allButtonClicked(); };
    copyButton.onClick = [this] { copyLog(); };
    saveButton.onClick = [this] { saveLog(); };
    stopButton.onClick = [this]
    {
        if (runner != nullptr)
            runner->signalThreadShouldExit();
    };

    lookButton.setTooltip ("Finds Pro Tools, the menu item and the counter, opens the Identify Beat dialog and cancels it");
    oneButton.setTooltip ("Sends the first pin with Identify Beat");
    allButton.setTooltip ("Sends every pin with Identify Beat, left to right, and stops at the first one that fails");
    askButton.setTooltip ("Shows the macOS prompt that opens System Settings");

    startBox.setRange (0.0, 1.0e13, 1.0, 0);
    startBox.setEmptyText ("from Pro Tools");
    startBox.setText ("from Pro Tools");
    addAndMakeVisible (startBox);

    rateBox.setRange (8000.0, 768000.0, 1.0, 0);
    rateBox.setValue (48000.0, juce::dontSendNotification);
    addAndMakeVisible (rateBox);

    entrySwitch.setItems ({ "Set the field", "Type it" });
    entrySwitch.setSelected (0, juce::dontSendNotification);
    entrySwitch.setTitle ("How the bar and beat are entered in the dialog");
    addAndMakeVisible (entrySwitch);

    logView.setMultiLine (true, false);
    logView.setReadOnly (true);
    logView.setCaretVisible (false);
    logView.setScrollbarsShown (true);
    logView.setFont (ui::Theme::mono (11.0f));
    logView.setIndents (10, 8);
    logView.setTitle ("Log");
    logView.setColour (juce::TextEditor::backgroundColourId, ui::Theme::panel());
    logView.setColour (juce::TextEditor::outlineColourId, ui::Theme::line());
    logView.setColour (juce::TextEditor::focusedOutlineColourId, ui::Theme::line());
    logView.setColour (juce::TextEditor::textColourId, ui::Theme::text());
    logView.setColour (juce::TextEditor::highlightColourId, ui::Theme::wave().withAlpha (0.45f));
    logView.setColour (juce::TextEditor::highlightedTextColourId, ui::Theme::text());
    logView.setTextToShowWhenEmpty ("The log of each run appears here.", ui::Theme::dim());
    logView.onEscapeKey = [this] { keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)); };
    addAndMakeVisible (logView);

    permissionAllowed = pt::Driver::accessibilityAllowed();
    setSummary (idleSummary, ui::Theme::dim());
    updateButtons();
    startTimerHz (10);
}

ProToolsTestPanel::~ProToolsTestPanel()
{
    stopTimer();
    runner = nullptr;
}

//==============================================================================
void ProToolsTestPanel::setSource (const std::vector<Pin>& newPins, int newBeatsPerBar, double musicSampleRate,
                                   const juce::String& newMusicName)
{
    pins = newPins;
    beatsPerBar = newBeatsPerBar;
    musicName = newMusicName;

    if (musicSampleRate > 0.0 && ! juce::exactlyEqual (musicSampleRate, shownMusicRate))
    {
        shownMusicRate = musicSampleRate;
        rateBox.setValue (musicSampleRate, juce::dontSendNotification);
    }

    confirmAllUntil = 0;
    updateButtons();
    repaint();
}

void ProToolsTestPanel::open()
{
    setVisible (true);
    toFront (false);
    grabKeyboardFocus();
}

void ProToolsTestPanel::close()
{
    if (isRunning())
        return;

    setVisible (false);

    if (onClose != nullptr)
        onClose();
}

void ProToolsTestPanel::showPermissionAs (bool allowed)
{
    permissionFixed = true;
    permissionAllowed = allowed;
    updateButtons();
    resized();
    repaint();
}

void ProToolsTestPanel::showFinishedRun (const juce::String& logText, const pt::TestOutcome& outcome)
{
    appendToLog (logText);
    logView.moveCaretToTop (false);
    setSummary (outcome.summary, outcome.ok ? ui::Theme::ok() : ui::Theme::check());
}

//==============================================================================
void ProToolsTestPanel::start (pt::TestMode mode)
{
    if (runner != nullptr)
        return;

    pt::TestSettings settings;
    settings.mode = mode;
    settings.pins = pins;
    settings.beatsPerBar = beatsPerBar;
    settings.sessionSampleRate = rateBox.getValue();
    settings.hasMusicStart = startBox.hasValue();
    settings.musicStartSample = std::llround (startBox.getValue());
    settings.typeLocation = entrySwitch.getSelected() == 1;
    settings.appVersion = JUCE_APPLICATION_VERSION_STRING;
    settings.musicName = musicName.toStdString();

    confirmAllUntil = 0;

    // Runs are kept one after another in the log, so one copy holds them all.
    if (fullLog.isNotEmpty())
        appendToLog ("\n");

    setSummary (runningSummary, ui::Theme::pin());
    runner = std::make_unique<Runner> (*this, std::move (settings));
    updateButtons();
    runner->startThread();
}

void ProToolsTestPanel::allButtonClicked()
{
    const auto now = juce::Time::getMillisecondCounter();

    if (confirmAllUntil != 0 && now < confirmAllUntil)
    {
        // The second click of a double-click comes before the changed button can be read.
        if (now - confirmAllSince >= 600)
            start (pt::TestMode::allPins);

        return;
    }

    // One more click, after a moment and within four seconds, sends them.
    confirmAllSince = now;
    confirmAllUntil = now + 4000;
    updateButtons();
}

void ProToolsTestPanel::runFinished (const pt::TestOutcome& outcome)
{
    runner = nullptr;
    flushLog();
    setSummary (outcome.summary, outcome.ok ? ui::Theme::ok() : ui::Theme::check());
    updateButtons();
}

void ProToolsTestPanel::addLineFromThread (const std::string& line)
{
    const juce::ScopedLock lock (logLock);
    pendingLog << juce::String::fromUTF8 (line.c_str()) << "\n";
}

void ProToolsTestPanel::flushLog()
{
    juce::String text;

    {
        const juce::ScopedLock lock (logLock);
        text.swapWith (pendingLog);
    }

    if (text.isNotEmpty())
        appendToLog (text);
}

void ProToolsTestPanel::appendToLog (const juce::String& text)
{
    fullLog << text;
    logView.moveCaretToEnd (false);
    logView.insertTextAtCaret (text);
    updateButtons();
}

void ProToolsTestPanel::setSummary (const juce::String& text, juce::Colour colour)
{
    summary = text;
    summaryColour = colour;
    resized();
    repaint();
}

void ProToolsTestPanel::updateButtons()
{
    const bool running = isRunning();
    const bool hasPins = ! pins.empty();
    const bool confirming = confirmAllUntil != 0;

    lookButton.setEnabled (! running);
    oneButton.setEnabled (! running && hasPins);
    allButton.setEnabled (! running && hasPins);
    stopButton.setEnabled (running);
    closeButton.setEnabled (! running);
    startBox.setEnabled (! running);
    rateBox.setEnabled (! running);
    entrySwitch.setEnabled (! running);
    copyButton.setEnabled (fullLog.isNotEmpty());
    saveButton.setEnabled (fullLog.isNotEmpty());
    askButton.setVisible (! permissionAllowed);

    allButton.setButtonText (confirming ? "Really send " + juce::String ((int) pins.size()) + (pins.size() == 1 ? " pin?" : " pins?")
                                        : juce::String ("Send all pins"));
    allButton.setPrimary (confirming);
}

void ProToolsTestPanel::timerCallback()
{
    flushLog();

    if (confirmAllUntil != 0 && juce::Time::getMillisecondCounter() >= confirmAllUntil)
    {
        confirmAllUntil = 0;
        updateButtons();
    }

    if (! permissionFixed && isVisible())
    {
        const bool allowed = pt::Driver::accessibilityAllowed();

        if (allowed != permissionAllowed)
        {
            permissionAllowed = allowed;
            updateButtons();
            resized();
            repaint();
        }
    }
}

//==============================================================================
void ProToolsTestPanel::copyLog()
{
    flushLog();
    juce::SystemClipboard::copyTextToClipboard (fullLog);
}

void ProToolsTestPanel::saveLog()
{
    flushLog();

    const auto name = "Tempo Tantrum Pro Tools test " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M") + ".txt";
    const auto start = juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (name);

    chooser = std::make_unique<juce::FileChooser> ("Save the log", start, "*.txt");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file == juce::File())
                                  return;

                              if (! file.hasFileExtension ("txt"))
                                  file = file.withFileExtension ("txt");

                              if (! file.replaceWithText (fullLog))
                                  setSummary ("The log could not be saved to " + file.getFileName(), ui::Theme::check());
                          });
}

bool ProToolsTestPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress (juce::KeyPress::escapeKey))
    {
        if (isRunning())
            runner->signalThreadShouldExit();
        else
            close();

        return true;
    }

    return false;
}

//==============================================================================
void ProToolsTestPanel::resized()
{
    using ui::Theme;

    card = getLocalBounds().reduced (cardMargin);
    card = card.withSizeKeepingCentre (juce::jmin (card.getWidth(), 1120), juce::jmin (card.getHeight(), 760));

    auto r = card.reduced (pad);

    auto top = r.removeFromTop (30);
    closeButton.setBounds (top.removeFromRight (72));
    titleArea = top;
    r.removeFromTop (6);

    summaryArea = r.removeFromTop (juce::jlimit (18, 56, heightOf (summary, summaryFont(), r.getWidth())));
    r.removeFromTop (20);

    auto left = r.removeFromLeft (juce::jmin (leftWidth, r.getWidth() / 2));
    r.removeFromLeft (columnGap);
    auto right = r;

    // Left: what to do first, then the two numbers and how the bar|beat is entered.
    int listHeight = 20;

    for (const auto* item : beforeYouStart)
        listHeight += heightOf (item, bodyFont(), left.getWidth() - listNumberWidth) + listItemGap;

    listArea = left.removeFromTop (listHeight);
    left.removeFromTop (10);

    auto boxes = left.removeFromTop (48);
    startBox.setBounds (boxes.removeFromLeft (boxes.getWidth() - 8 - 148));
    boxes.removeFromLeft (8);
    rateBox.setBounds (boxes);
    left.removeFromTop (7);
    noteArea = left.removeFromTop (heightOf (boxesNote, bodyFont(), left.getWidth()));
    left.removeFromTop (14);
    entryCaption = left.removeFromTop (14);
    left.removeFromTop (4);
    entrySwitch.setBounds (left.removeFromTop (30));

    // Right: the permission, the buttons that start a run, the log, and what to do with the log.
    auto permissionRow = right.removeFromTop (30);

    if (! permissionAllowed)
    {
        askButton.setBounds (permissionRow.removeFromRight (askButton.getBestWidth()));
        permissionRow.removeFromRight (10);
    }

    permissionArea = permissionRow;
    permissionNoteArea = {};

    if (! permissionAllowed)
    {
        right.removeFromTop (4);
        permissionNoteArea = right.removeFromTop (heightOf (permissionNote, bodyFont(), right.getWidth()));
    }

    right.removeFromTop (10);

    auto actions = right.removeFromTop (32);
    lookButton.setBounds (actions.removeFromLeft (lookButton.getBestWidth()));
    actions.removeFromLeft (8);
    oneButton.setBounds (actions.removeFromLeft (oneButton.getBestWidth()));
    actions.removeFromLeft (8);
    allButton.setBounds (actions.removeFromLeft (184));
    actions.removeFromLeft (8);
    stopButton.setBounds (actions.removeFromLeft (64));
    right.removeFromTop (10);

    auto bottom = right.removeFromBottom (30);
    copyButton.setBounds (bottom.removeFromLeft (copyButton.getBestWidth()));
    bottom.removeFromLeft (8);
    saveButton.setBounds (bottom.removeFromLeft (saveButton.getBestWidth()));
    bottom.removeFromLeft (12);
    pinsNoteArea = bottom;
    right.removeFromBottom (10);

    logView.setBounds (right);
}

void ProToolsTestPanel::paint (juce::Graphics& g)
{
    using ui::Theme;

    // The window behind is dimmed and takes no clicks while the panel is up.
    g.fillAll (juce::Colours::black.withAlpha (0.6f));

    const auto cardBounds = card.toFloat().reduced (0.5f);
    g.setColour (Theme::bg());
    g.fillRoundedRectangle (cardBounds, 10.0f);
    g.setColour (Theme::line());
    g.drawRoundedRectangle (cardBounds, 10.0f, 1.0f);

    g.setColour (Theme::text());
    g.setFont (Theme::ui (17.0f, true));
    g.drawText ("Test in Pro Tools", titleArea, juce::Justification::centredLeft, true);

    wrapped (summary, summaryFont(), summaryColour, summaryArea.getWidth()).draw (g, summaryArea.toFloat());

    g.setColour (Theme::line());
    g.fillRect (card.getX() + pad, summaryArea.getBottom() + 9, card.getWidth() - 2 * pad, 1);

    // Before you start
    {
        auto r = listArea;
        g.setColour (Theme::dim());
        g.setFont (Theme::fieldLabel());
        g.drawText ("BEFORE YOU START", r.removeFromTop (14), juce::Justification::centredLeft, true);
        r.removeFromTop (6);
        int number = 1;

        for (const auto* item : beforeYouStart)
        {
            const auto layout = wrapped (item, bodyFont(), Theme::text(), r.getWidth() - listNumberWidth);
            auto row = r.removeFromTop ((int) std::ceil (layout.getHeight()));
            g.setColour (Theme::pin());
            g.setFont (Theme::mono (11.0f));
            g.drawText (juce::String (number++), row.removeFromLeft (listNumberWidth).removeFromTop (16),
                        juce::Justification::centredLeft, false);
            layout.draw (g, row.toFloat());
            r.removeFromTop (listItemGap);
        }
    }

    wrapped (boxesNote, bodyFont(), Theme::dim(), noteArea.getWidth()).draw (g, noteArea.toFloat());

    g.setColour (Theme::dim());
    g.setFont (Theme::fieldLabel());
    g.drawText ("BAR|BEAT ENTRY", entryCaption, juce::Justification::centredLeft, true);

    // Permission
    {
        auto r = permissionArea;
        const auto colour = permissionAllowed ? Theme::ok() : Theme::check();
        const float dot = 8.0f;
        g.setColour (colour);
        g.fillEllipse ((float) r.getX() + 1.0f, (float) r.getCentreY() - dot * 0.5f, dot, dot);
        r.removeFromLeft (17);

        const auto text = juce::String (permissionAllowed ? permissionGiven : permissionMissing);
        const auto layout = wrapped (text, bodyFont(), Theme::text(), r.getWidth());
        layout.draw (g, r.toFloat().withSizeKeepingCentre ((float) r.getWidth(), layout.getHeight()));

        if (! permissionNoteArea.isEmpty())
            wrapped (permissionNote, bodyFont(), Theme::dim(), permissionNoteArea.getWidth()).draw (g, permissionNoteArea.toFloat());
    }

    // How many pins a run would send.
    {
        const int count = (int) pins.size();
        const auto text = count == 0 ? juce::String ("No pins yet: open music to send pins")
                                     : juce::String (count) + (count == 1 ? " pin in the map" : " pins in the map");
        g.setColour (Theme::dim());
        g.setFont (bodyFont());
        g.drawText (text, pinsNoteArea, juce::Justification::centredRight, true);
    }
}

} // namespace tt

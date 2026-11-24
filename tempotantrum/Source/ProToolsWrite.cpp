#include "ProToolsWrite.h"

#include "ui/Theme.h"

#include <algorithm>
#include <cmath>

namespace tt
{

namespace
{
    constexpr int cardMargin = 12;
    constexpr int pad = 18;
    constexpr int cardWidth = 548;
    constexpr int floatWidth = 330;
    constexpr int floatHeight = 108;

    const char* const instruction = "In Pro Tools, select the music clip so the cursor is at its start, with the Main Counter set to "
                                    "Samples. Then press Write.";
    const char* const permissionMissing = "macOS has not given Tempo Tantrum permission to control other apps.";
    const char* const permissionNote = "Then switch Tempo Tantrum on in System Settings > Privacy & Security > Accessibility, and reopen the app.";
    const char* const handsOff = "Leave the keyboard and mouse alone";

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

    juce::Font bodyFont()       { return ui::Theme::ui (13.0f); }
    juce::Font strongFont()     { return ui::Theme::ui (14.0f, true); }

    // 2880000 as "2,880,000".
    juce::String grouped (long long value)
    {
        auto digits = juce::String (std::abs (value));
        juce::String out;

        for (int i = 0; i < digits.length(); ++i)
        {
            if (i > 0 && (digits.length() - i) % 3 == 0)
                out << ",";

            out << digits[i];
        }

        return value < 0 ? "-" + out : out;
    }

    juce::String countOf (int count, const char* one, const char* many)
    {
        return juce::String (count) + " " + (count == 1 ? one : many);
    }

    void drawProgressBar (juce::Graphics& g, juce::Rectangle<int> area, double fraction)
    {
        const auto r = area.toFloat();
        g.setColour (ui::Theme::line());
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (ui::Theme::pin());
        g.fillRoundedRectangle (r.withWidth (std::max (r.getHeight(), r.getWidth() * (float) juce::jlimit (0.0, 1.0, fraction))),
                                r.getHeight() * 0.5f);
    }

    double fractionOf (int done, int total)
    {
        return total > 0 ? (double) done / (double) total : 0.0;
    }
}

//==============================================================================
// One run on its own thread. What it does is given as a function, which gets the thread to ask
// whether to stop. Its result goes back to the sheet on the message thread.
class ProToolsWriteSheet::Runner : public juce::Thread
{
public:
    explicit Runner (std::function<void (juce::Thread&)> whatToRun)
        : juce::Thread ("Pro Tools write"), body (std::move (whatToRun))
    {
    }

    ~Runner() override
    {
        // Every wait in a run is short and asks whether to stop, and no call to Pro Tools waits
        // longer than two seconds, so a stopped run ends within a few calls.
        stopThread (15000);
    }

    void run() override
    {
        body (*this);
    }

private:
    std::function<void (juce::Thread&)> body;
};

//==============================================================================
// Reads the Main Counter while the sheet waits for Write, so the sheet can say where the pins
// would go. It only reads: Pro Tools is not brought forward and nothing is pressed.
class ProToolsWriteSheet::Watcher : public juce::Thread
{
public:
    explicit Watcher (ProToolsWriteSheet& owner)
        : juce::Thread ("Pro Tools counter"), safeSheet (&owner)
    {
    }

    ~Watcher() override
    {
        stopThread (15000);
    }

    void run() override
    {
        pt::CounterReader reader (pt::Driver::defaultBundleIdentifier, [this] { return threadShouldExit(); });

        while (! threadShouldExit())
        {
            const auto reading = reader.read();

            if (threadShouldExit())
                break;

            juce::MessageManager::callAsync ([safe = safeSheet, reading]
            {
                if (safe != nullptr)
                    safe->setCounter (reading);
            });

            wait (700);
        }
    }

private:
    juce::Component::SafePointer<ProToolsWriteSheet> safeSheet;
};

//==============================================================================
// A box to tick with its text beside it.
class ProToolsWriteSheet::Tick : public juce::Component,
                                 public juce::SettableTooltipClient
{
public:
    explicit Tick (const juce::String& textToShow) : text (textToShow)
    {
        setTitle (text);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void setTicked (bool shouldBeTicked)
    {
        ticked = shouldBeTicked;
        repaint();
    }

    bool isTicked() const       { return ticked; }

    void paint (juce::Graphics& g) override
    {
        const float alpha = isEnabled() ? 1.0f : 0.4f;
        const auto box = juce::Rectangle<float> (0.5f, ((float) getHeight() - 16.0f) * 0.5f, 16.0f, 16.0f);

        g.setColour ((ticked ? ui::Theme::pin() : ui::Theme::panel()).withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour ((ticked ? ui::Theme::pin() : ui::Theme::dim()).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (box, 4.0f, 1.0f);

        if (ticked)
        {
            juce::Path mark;
            mark.startNewSubPath (box.getX() + 3.6f, box.getCentreY() + 0.3f);
            mark.lineTo (box.getX() + 6.8f, box.getBottom() - 4.2f);
            mark.lineTo (box.getRight() - 3.4f, box.getY() + 4.4f);
            g.setColour (ui::Theme::inkOnPin().withMultipliedAlpha (alpha));
            g.strokePath (mark, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setColour (ui::Theme::text().withMultipliedAlpha (alpha));
        g.setFont (bodyFont());
        g.drawText (text, getLocalBounds().withTrimmedLeft (25), juce::Justification::centredLeft, true);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isEnabled() && e.mouseWasClicked() && getLocalBounds().contains (e.getPosition()))
            setTicked (! ticked);
    }

private:
    juce::String text;
    bool ticked = false;
};

//==============================================================================
// "More" with a triangle that points down while what it holds is shown.
class ProToolsWriteSheet::Disclosure : public juce::Component
{
public:
    std::function<void()> onClick;

    Disclosure()
    {
        setTitle ("More");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void setOpen (bool shouldBeOpen)
    {
        isOpen = shouldBeOpen;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const float alpha = isEnabled() ? 1.0f : 0.4f;
        const float cy = (float) getHeight() * 0.5f;
        juce::Path triangle;

        if (isOpen)
            triangle.addTriangle (1.0f, cy - 3.0f, 10.0f, cy - 3.0f, 5.5f, cy + 3.5f);
        else
            triangle.addTriangle (2.5f, cy - 4.5f, 2.5f, cy + 4.5f, 9.0f, cy);

        g.setColour (ui::Theme::dim().withMultipliedAlpha (alpha));
        g.fillPath (triangle);
        g.setColour (ui::Theme::text().withMultipliedAlpha (alpha));
        g.setFont (ui::Theme::ui (13.0f, true));
        g.drawText ("More", getLocalBounds().withTrimmedLeft (17), juce::Justification::centredLeft, true);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isEnabled() && e.mouseWasClicked() && getLocalBounds().contains (e.getPosition()) && onClick != nullptr)
            onClick();
    }

private:
    bool isOpen = false;
};

//==============================================================================
// The small panel that stays above the other applications during a run. It is a window of its
// own that never takes the keyboard.
class ProToolsWriteSheet::FloatingPanel : public juce::Component
{
public:
    std::function<void()> onStop;

    FloatingPanel()
    {
        setOpaque (true);
        setSize (floatWidth, floatHeight);
        stopButton.setWantsKeyboardFocus (false);
        stopButton.setTriggeredOnMouseDown (true);   // the click brings Tempo Tantrum forward; the stop is asked for before the run sees that
        stopButton.onClick = [this]
        {
            if (onStop != nullptr)
                onStop();
        };
        addAndMakeVisible (stopButton);
    }

    void setProgress (const juce::String& newText, double newFraction)
    {
        if (newText != text || ! juce::exactlyEqual (newFraction, fraction))
        {
            text = newText;
            fraction = newFraction;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (ui::Theme::bg());
        g.setColour (ui::Theme::pin());
        g.drawRect (getLocalBounds(), 1);

        auto r = getLocalBounds().reduced (16, 14);
        auto top = r.removeFromTop (30);
        top.removeFromRight (stopButton.getWidth() + 12);
        g.setColour (ui::Theme::text());
        g.setFont (ui::Theme::ui (17.0f, true));
        g.drawText (text, top, juce::Justification::centredLeft, true);

        r.removeFromTop (10);
        drawProgressBar (g, r.removeFromTop (6), fraction);
        r.removeFromTop (10);
        g.setColour (ui::Theme::dim());
        g.setFont (bodyFont());
        g.drawText (handsOff, r.removeFromTop (18), juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        stopButton.setBounds (getLocalBounds().reduced (16, 14).removeFromTop (30).removeFromRight (70));
    }

private:
    ui::PillButton stopButton { "Stop" };
    juce::String text;
    double fraction = 0.0;
};

//==============================================================================
ProToolsWriteSheet::ProToolsWriteSheet()
    : moreToggle (std::make_unique<Disclosure>()),
      checkTick (std::make_unique<Tick> ("Check each pin afterwards")),
      slowTick (std::make_unique<Tick> ("Type slowly"))
{
    setWantsKeyboardFocus (true);
    setTitle ("Write to Pro Tools");

    for (auto* b : { &cancelButton, &writeButton, &askButton, &stopButton, &undoButton, &copyButton, &saveButton, &closeButton,
                     &againButton })
    {
        b->setWantsKeyboardFocus (false);
        addChildComponent (*b);
    }

    cancelButton.onClick = [this] { close(); };
    closeButton.onClick = [this] { close(); };
    writeButton.onClick = [this] { startWrite(); };
    againButton.onClick = [this] { backToReady(); };
    askButton.onClick = [] { pt::Driver::requestAccessibility(); };
    stopButton.onClick = [this] { requestStop(); };
    undoButton.onClick = [this] { undoClicked(); };
    copyButton.onClick = [this] { copyLog(); };
    saveButton.onClick = [this] { saveLog(); };

    writeButton.setTooltip ("Brings Pro Tools to the front and runs Event > Identify Beat once for each pin");
    askButton.setTooltip ("Shows the macOS prompt that opens System Settings");
    undoButton.setTooltip ("Brings Pro Tools to the front and presses Edit > Undo once for each pin that was written");
    againButton.setTooltip ("Goes back to the start of this sheet");

    startBox.setRange (0.0, 1.0e13, 1.0, 0);
    startBox.setEmptyText ("from Pro Tools");
    startBox.setText ("from Pro Tools");
    startBox.onChange = [this] (double) { layout(); repaint(); };
    addChildComponent (startBox);

    rateBox.setRange (8000.0, 768000.0, 1.0, 0);
    rateBox.setValue (48000.0, juce::dontSendNotification);
    addChildComponent (rateBox);

    checkTick->setTicked (true);
    checkTick->setTooltip ("Once every pin is written, opens Identify Beat at each one, reads its location and cancels");
    slowTick->setTooltip ("Holds each key for longer. Try this if a run stops because a number did not arrive");
    addChildComponent (*checkTick);
    addChildComponent (*slowTick);

    moreToggle->onClick = [this]
    {
        moreOpen = ! moreOpen;
        updateControls();
    };
    addChildComponent (*moreToggle);

    permissionAllowed = pt::Driver::accessibilityAllowed();
    updateControls();
    startTimerHz (10);
}

ProToolsWriteSheet::~ProToolsWriteSheet()
{
    stopTimer();
    watcher = nullptr;
    runner = nullptr;
    floating = nullptr;
}

//==============================================================================
void ProToolsWriteSheet::setSource (const std::vector<Pin>& newPins, int newBeatsPerBar, double musicSampleRate,
                                    const juce::String& newMusicName, int newUnchecked)
{
    pins = newPins;
    beatsPerBar = newBeatsPerBar;
    musicName = newMusicName;
    unchecked = newUnchecked;

    if (musicSampleRate > 0.0 && ! juce::exactlyEqual (musicSampleRate, shownMusicRate))
    {
        shownMusicRate = musicSampleRate;
        rateBox.setValue (musicSampleRate, juce::dontSendNotification);
    }

    updateControls();
}

void ProToolsWriteSheet::open()
{
    // A run that can still be undone is shown again as it ended. Otherwise the sheet starts afresh.
    if (! isRunning() && ! (stage == Stage::finished && hasOutcome && undoSteps > 0))
        stage = Stage::ready;

    if (! forPicture)
        permissionAllowed = pt::Driver::accessibilityAllowed();

    setVisible (true);
    toFront (false);
    grabKeyboardFocus();
    updateControls();

    if (stage == Stage::ready)
        startWatching();
}

void ProToolsWriteSheet::close()
{
    if (isRunning())
        return;

    stopWatching();
    confirmUndoUntil = 0;
    setVisible (false);

    if (onClose != nullptr)
        onClose();
}

void ProToolsWriteSheet::backToReady()
{
    if (isRunning())
        return;

    stage = Stage::ready;
    confirmUndoUntil = 0;
    updateControls();
    startWatching();
}

//==============================================================================
void ProToolsWriteSheet::showPermissionAs (bool allowed)
{
    forPicture = true;
    stopWatching();
    permissionAllowed = allowed;
    updateControls();
}

void ProToolsWriteSheet::showCounterAs (const pt::CounterReading& reading)
{
    forPicture = true;
    stopWatching();
    counterKnown = true;
    counter = reading;
    updateControls();
}

void ProToolsWriteSheet::showMoreOpen (bool shouldBeOpen)
{
    moreOpen = shouldBeOpen;
    updateControls();
}

void ProToolsWriteSheet::showRunning (pt::Phase phase, int done, int total)
{
    forPicture = true;
    stopWatching();
    stage = Stage::running;
    progressPhase = (int) phase;
    progressDone = done;
    progressTotal = total;
    updateControls();
}

void ProToolsWriteSheet::showFinished (const pt::WriteOutcome& finished, const juce::String& logText)
{
    forPicture = true;
    stopWatching();
    fullLog << logText;
    outcome = finished;
    hasOutcome = true;
    undoSteps = finished.written;
    undoLine.clear();
    stage = Stage::finished;
    updateControls();
}

juce::Image ProToolsWriteSheet::floatingPanelPicture (pt::Phase phase, int done, int total)
{
    FloatingPanel panel;
    panel.setProgress (progressText (phase, done, total), fractionOf (done, total));
    return panel.createComponentSnapshot (panel.getLocalBounds(), true, 2.0f);
}

//==============================================================================
void ProToolsWriteSheet::startWatching()
{
    if (forPicture || watcher != nullptr || ! permissionAllowed || ! isVisible())
        return;

    counterKnown = false;
    watcher = std::make_unique<Watcher> (*this);
    watcher->startThread();
}

void ProToolsWriteSheet::stopWatching()
{
    watcher = nullptr;
}

void ProToolsWriteSheet::setCounter (const pt::CounterReading& reading)
{
    if (watcher == nullptr)
        return;

    const bool changed = ! counterKnown || reading.running != counter.running || reading.read != counter.read
                         || reading.isSamples != counter.isSamples || reading.sample != counter.sample
                         || reading.text != counter.text;
    counterKnown = true;
    counter = reading;

    if (changed && stage == Stage::ready)
    {
        layout();
        repaint();
    }
}

//==============================================================================
void ProToolsWriteSheet::startWrite()
{
    if (runner != nullptr || pins.empty() || ! permissionAllowed)
        return;

    pt::WriteSettings settings;
    settings.pins = pins;
    settings.beatsPerBar = beatsPerBar;
    settings.sessionSampleRate = rateBox.getValue();
    settings.hasMusicStart = startBox.hasValue();
    settings.musicStartSample = std::llround (startBox.getValue());
    settings.checkAfterwards = checkTick->isTicked();
    settings.slowKeys = slowTick->isTicked();
    settings.hasLastStart = hasLastStart;
    settings.lastStartSample = lastStart;
    settings.appVersion = JUCE_APPLICATION_VERSION_STRING;
    settings.musicName = musicName.toStdString();

    runPinOffsets.clear();

    for (const auto& pin : pins)
        runPinOffsets.push_back (std::llround (pin.time * settings.sessionSampleRate));

    stopWatching();
    flushLog();

    // Runs are kept one after another in the log, so one copy holds them all.
    if (fullLog.isNotEmpty())
        fullLog << "\n";

    stage = Stage::running;
    undoing = false;
    progressPhase = (int) pt::Phase::starting;
    progressDone = 0;
    progressTotal = (int) pins.size();

    runner = std::make_unique<Runner> ([this, safe = juce::Component::SafePointer<ProToolsWriteSheet> (this), settings] (juce::Thread& thread)
    {
        // The sheet outlives the thread: its destructor waits for the thread to end.
        const auto result = pt::runWrite (settings,
                                          [this] (const std::string& line) { addLineFromThread (line); },
                                          [&thread] { return thread.threadShouldExit(); },
                                          [this] (pt::Phase phase, int done, int total)
                                          {
                                              progressPhase = (int) phase;
                                              progressDone = done;
                                              progressTotal = total;
                                          });

        juce::MessageManager::callAsync ([safe, result]
        {
            if (safe != nullptr)
                safe->writeFinished (result);
        });
    });

    showFloatingPanel();
    updateControls();
    runner->startThread();
}

void ProToolsWriteSheet::undoClicked()
{
    if (runner != nullptr || undoSteps <= 0)
        return;

    const auto now = juce::Time::getMillisecondCounter();

    if (confirmUndoUntil != 0 && now < confirmUndoUntil)
    {
        // The second click of a double-click comes before the changed button can be read.
        if (now - confirmUndoSince >= 600)
            startUndo();

        return;
    }

    // One more click, after a moment and within four seconds, sends them.
    confirmUndoSince = now;
    confirmUndoUntil = now + 4000;
    updateControls();
}

void ProToolsWriteSheet::startUndo()
{
    if (runner != nullptr || undoSteps <= 0)
        return;

    const int steps = undoSteps;
    confirmUndoUntil = 0;
    flushLog();

    if (fullLog.isNotEmpty())
        fullLog << "\n";

    stage = Stage::running;
    undoing = true;
    progressPhase = (int) pt::Phase::undoing;
    progressDone = 0;
    progressTotal = steps;

    runner = std::make_unique<Runner> ([this, safe = juce::Component::SafePointer<ProToolsWriteSheet> (this), steps] (juce::Thread& thread)
    {
        const auto result = pt::runUndo (pt::Driver::defaultBundleIdentifier, steps, JUCE_APPLICATION_VERSION_STRING,
                                         [this] (const std::string& line) { addLineFromThread (line); },
                                         [&thread] { return thread.threadShouldExit(); },
                                         [this] (pt::Phase phase, int done, int total)
                                         {
                                             progressPhase = (int) phase;
                                             progressDone = done;
                                             progressTotal = total;
                                         });

        juce::MessageManager::callAsync ([safe, result]
        {
            if (safe != nullptr)
                safe->undoFinished (result);
        });
    });

    showFloatingPanel();
    updateControls();
    runner->startThread();
}

void ProToolsWriteSheet::writeFinished (const pt::WriteOutcome& finished)
{
    outcome = finished;
    hasOutcome = true;
    undoSteps = finished.written;
    undoLine.clear();
    undoLineBad = false;

    if (finished.hasMusicStart && (finished.written > 0 || finished.okPressedOnFailedPin))
    {
        hasLastStart = true;
        lastStart = finished.musicStart;
        lastPinSamples.clear();

        for (const auto offset : runPinOffsets)
            lastPinSamples.push_back (finished.musicStart + offset);
    }

    runEnded();
}

void ProToolsWriteSheet::undoFinished (const pt::UndoOutcome& finished)
{
    undoSteps = std::max (0, undoSteps - finished.sent);
    undoLine = finished.summary + ". Look in Pro Tools: what it did with them cannot be read from here.";
    undoLineBad = ! finished.ok;

    // With every step undone, writing to the same place again is no longer a second write.
    if (undoSteps == 0 && ! outcome.okPressedOnFailedPin)
    {
        hasLastStart = false;
        lastPinSamples.clear();
    }

    runEnded();
}

// On the message thread, once the run's thread has posted its result.
void ProToolsWriteSheet::runEnded()
{
    runner = nullptr;
    floating = nullptr;
    undoing = false;
    stage = Stage::finished;
    flushLog();
    updateControls();

    // The run left Pro Tools in front. The result is in this window, so it comes forward.
    if (! forPicture)
    {
        juce::Process::makeForegroundProcess();

        if (auto* window = getTopLevelComponent())
            window->toFront (true);

        grabKeyboardFocus();
    }
}

void ProToolsWriteSheet::requestStop()
{
    if (runner != nullptr)
        runner->signalThreadShouldExit();
}

//==============================================================================
void ProToolsWriteSheet::showFloatingPanel()
{
    if (forPicture)
        return;

    floating = std::make_unique<FloatingPanel>();
    floating->onStop = [this] { requestStop(); };
    updateFloatingPanel();

    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const auto screen = display != nullptr ? display->userArea : juce::Rectangle<int> (0, 0, 1280, 800);
    floating->setTopLeftPosition (screen.getCentreX() - floatWidth / 2, screen.getY() + 48);

    // Temporary and ignoring key presses: the window cannot become the key window, and showing
    // it does not bring Tempo Tantrum forward.
    floating->setAlwaysOnTop (true);
    floating->addToDesktop (juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowIgnoresKeyPresses
                            | juce::ComponentPeer::windowHasDropShadow);
    floating->setVisible (true);

    if (auto* peer = floating->getPeer())
        pt::Driver::keepInViewEverywhere (peer->getNativeHandle());
}

void ProToolsWriteSheet::updateFloatingPanel()
{
    if (floating != nullptr)
        floating->setProgress (progressText ((pt::Phase) progressPhase.load(), progressDone.load(), progressTotal.load()),
                               fractionOf (progressDone.load(), progressTotal.load()));
}

//==============================================================================
void ProToolsWriteSheet::addLineFromThread (const std::string& line)
{
    const juce::ScopedLock lock (logLock);
    pendingLog << juce::String::fromUTF8 (line.c_str()) << "\n";
}

void ProToolsWriteSheet::flushLog()
{
    juce::String text;

    {
        const juce::ScopedLock lock (logLock);
        text.swapWith (pendingLog);
    }

    fullLog << text;
}

void ProToolsWriteSheet::copyLog()
{
    flushLog();
    juce::SystemClipboard::copyTextToClipboard (fullLog);
}

void ProToolsWriteSheet::saveLog()
{
    flushLog();

    const auto name = "Tempo Tantrum Pro Tools write " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M") + ".txt";
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
                              {
                                  undoLine = "The log could not be saved to " + file.getFileName() + ".";
                                  undoLineBad = true;
                                  updateControls();
                              }
                          });
}

//==============================================================================
void ProToolsWriteSheet::timerCallback()
{
    if (stage == Stage::running)
    {
        updateFloatingPanel();
        repaint (card);
    }
    else if (stage == Stage::ready && isVisible())
    {
        // The line about the music start follows the Main Counter and the box under More.
        layout();
        repaint (card);
    }

    if (confirmUndoUntil != 0 && juce::Time::getMillisecondCounter() >= confirmUndoUntil)
    {
        confirmUndoUntil = 0;
        updateControls();
    }

    // The permission is given in System Settings while the sheet is open.
    if (! forPicture && isVisible() && stage == Stage::ready)
    {
        const bool allowed = pt::Driver::accessibilityAllowed();

        if (allowed != permissionAllowed)
        {
            permissionAllowed = allowed;
            updateControls();

            if (allowed)
                startWatching();
            else
                stopWatching();
        }
    }
}

bool ProToolsWriteSheet::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress (juce::KeyPress::escapeKey))
    {
        if (isRunning())
            requestStop();
        else
            close();

        return true;
    }

    return false;
}

//==============================================================================
juce::String ProToolsWriteSheet::pinsLine() const
{
    if (pins.empty())
        return "There are no pins to write";

    const int first = pt::barOf (pins.front(), beatsPerBar), last = pt::barOf (pins.back(), beatsPerBar);

    return countOf ((int) pins.size(), "pin", "pins")
           + (first == last ? ", bar " + juce::String (first) : ", bars " + juce::String (first) + " to " + juce::String (last));
}

// Where the pins would go, with what to look out for. `warning` is set when the line asks for a
// second look.
juce::String ProToolsWriteSheet::counterLine (bool& warning) const
{
    warning = false;
    const bool typed = startBox.hasValue();

    if (counterKnown && ! counter.running)
    {
        warning = true;
        return "Pro Tools is not running.";
    }

    long long start = 0;
    juce::String text;

    if (typed)
    {
        start = std::llround (startBox.getValue());
        text = "The music starts at sample " + grouped (start) + ", as typed under More.";
    }
    else if (! counterKnown)
    {
        return "Reading the Main Counter in Pro Tools.";
    }
    else if (! counter.read)
    {
        warning = true;
        return "The Main Counter could not be read in Pro Tools. Open a session.";
    }
    else if (! counter.isSamples)
    {
        warning = true;
        return "The Main Counter shows " + juce::String::fromUTF8 (counter.text.c_str()).trim() + ". Set it to Samples.";
    }
    else
    {
        start = counter.sample;
        text = "The music starts at sample " + grouped (start) + ", where the cursor is.";
    }

    if (hasLastStart && start == lastStart)
    {
        warning = true;
        text << " The last run wrote from this same sample.";
    }
    else if (std::find (lastPinSamples.begin(), lastPinSamples.end(), start) != lastPinSamples.end())
    {
        warning = true;
        text << " That is a pin of the last run, not the start of the music clip.";
    }
    else if (start == 0 && ! typed)
    {
        warning = true;
        text << " That is the start of the session: check that the music clip starts there.";
    }

    return text;
}

juce::String ProToolsWriteSheet::outcomeText() const
{
    auto text = juce::String::fromUTF8 (outcome.summary.c_str());

    if (outcome.okPressedOnFailedPin)
        text << ". OK was pressed for that pin, so it may have been written: look in Pro Tools";

    return text;
}

// What follows the outcome: how much was written before a stop, then the check.
juce::String ProToolsWriteSheet::checkText() const
{
    juce::StringArray lines;

    if (! outcome.ok && outcome.total > 0 && outcome.failedPin > 0)
        lines.add (juce::String (outcome.written) + " of " + countOf (outcome.total, "pin", "pins")
                   + (outcome.written == 1 ? " was written." : " were written."));

    if (outcome.checkSummary.empty())
        return lines.joinIntoString ("\n");

    lines.add (juce::String::fromUTF8 (outcome.checkSummary.c_str()) + (outcome.off.empty() ? "." : ":"));
    const int shown = std::min ((int) outcome.off.size(), 5);

    for (int i = 0; i < shown; ++i)
    {
        const auto& off = outcome.off[(size_t) i];
        const auto ticks = std::abs (off.ticksOff);
        lines.add ("Pin " + juce::String (off.pin) + ", " + juce::String (off.wanted) + ": " + juce::String (ticks)
                   + (ticks == 1 ? " tick " : " ticks ") + (off.ticksOff > 0 ? "late" : "early"));
    }

    if ((int) outcome.off.size() > shown)
        lines.add ("and " + juce::String ((int) outcome.off.size() - shown) + " more, in the log.");

    return lines.joinIntoString ("\n");
}

juce::String ProToolsWriteSheet::progressText (pt::Phase phase, int done, int total)
{
    const auto count = juce::String (std::min (done + 1, std::max (1, total))) + " of " + juce::String (total);

    switch (phase)
    {
        case pt::Phase::starting:   return "Starting";
        case pt::Phase::writing:    return "Writing " + count;
        case pt::Phase::checking:   return "Checking " + count;
        case pt::Phase::undoing:    return "Sending undo " + count;
    }

    return {};
}

//==============================================================================
void ProToolsWriteSheet::updateControls()
{
    const bool ready = stage == Stage::ready, running = stage == Stage::running, finished = stage == Stage::finished;
    const bool confirming = confirmUndoUntil != 0;

    cancelButton.setVisible (ready);
    writeButton.setVisible (ready && permissionAllowed);
    askButton.setVisible (ready && ! permissionAllowed);
    moreToggle->setVisible (ready);
    startBox.setVisible (ready && moreOpen);
    rateBox.setVisible (ready && moreOpen);
    checkTick->setVisible (ready && moreOpen);
    slowTick->setVisible (ready && moreOpen);
    stopButton.setVisible (running);
    undoButton.setVisible (finished && undoSteps > 0);
    copyButton.setVisible (finished);
    saveButton.setVisible (finished);
    closeButton.setVisible (finished);
    againButton.setVisible (finished);

    moreToggle->setOpen (moreOpen);
    writeButton.setEnabled (! pins.empty());
    writeButton.setButtonText (unchecked > 0 ? "Write (" + juce::String (unchecked) + " unchecked)" : juce::String ("Write"));
    againButton.setEnabled (! pins.empty());
    copyButton.setEnabled (fullLog.isNotEmpty());
    saveButton.setEnabled (fullLog.isNotEmpty());

    undoButton.setButtonText (confirming ? "Really send " + countOf (undoSteps, "undo", "undos") + "?"
                                         : "Undo in Pro Tools (" + countOf (undoSteps, "step", "steps") + ")");
    undoButton.setPrimary (confirming);

    layout();
    repaint();
}

void ProToolsWriteSheet::resized()
{
    layout();
}

// Works out the card and everything on it for the stage that is showing. The contents are laid
// out once from the top of a tall strip to find their height, and again inside the card.
void ProToolsWriteSheet::layout()
{
    const int width = std::max (260, std::min (cardWidth, getWidth() - 2 * cardMargin));
    const int inner = width - 2 * pad;

    const auto place = [this, inner] (int x, int top)
    {
        int y = top;
        const auto row = [&] (int height) { const juce::Rectangle<int> r (x, y, inner, height); y += height; return r; };

        titleArea = row (26);
        y += 10;

        instructionArea = pinsArea = counterArea = permissionArea = permissionNoteArea = {};
        progressTextArea = progressBarArea = progressNoteArea = outcomeArea = checkArea = undoArea = {};

        if (stage == Stage::ready)
        {
            instructionArea = row (heightOf (instruction, bodyFont(), inner));
            y += 14;
            pinsArea = row (20);
            y += 6;

            if (permissionAllowed)
            {
                bool warning = false;
                counterArea = row (std::max (17, heightOf (counterLine (warning), bodyFont(), inner)));
            }
            else
            {
                auto permissionRow = row (30);
                askButton.setBounds (permissionRow.removeFromRight (askButton.getBestWidth()));
                permissionRow.removeFromRight (10);
                permissionArea = permissionRow;
                y += 6;
                permissionNoteArea = row (heightOf (permissionNote, bodyFont(), inner));
            }

            y += 14;
            moreToggle->setBounds (row (22).removeFromLeft (70));

            if (moreOpen)
            {
                y += 10;
                auto boxes = row (48);
                rateBox.setBounds (boxes.removeFromRight (160));
                boxes.removeFromRight (10);
                startBox.setBounds (boxes);
                y += 12;
                checkTick->setBounds (row (22).removeFromLeft (230));
                y += 6;
                slowTick->setBounds (row (22).removeFromLeft (140));
            }

            y += 18;
            auto buttons = row (32);

            if (permissionAllowed)
            {
                writeButton.setBounds (buttons.removeFromRight (std::max (96, writeButton.getBestWidth())));
                buttons.removeFromRight (8);
            }

            cancelButton.setBounds (buttons.removeFromRight (84));
        }
        else if (stage == Stage::running)
        {
            auto top2 = row (32);
            stopButton.setBounds (top2.removeFromRight (76));
            top2.removeFromRight (12);
            progressTextArea = top2;
            y += 12;
            progressBarArea = row (6);
            y += 12;
            progressNoteArea = row (18);
        }
        else
        {
            outcomeArea = row (heightOf (outcomeText(), strongFont(), inner));

            if (const auto text = checkText(); text.isNotEmpty())
            {
                y += 10;
                checkArea = row (heightOf (text, bodyFont(), inner));
            }

            if (undoLine.isNotEmpty())
            {
                y += 10;
                undoArea = row (heightOf (undoLine, bodyFont(), inner));
            }

            y += 18;

            if (undoSteps > 0)
            {
                // Wide enough for the question it turns into.
                undoButton.setBounds (row (32).removeFromLeft (std::min (inner, 270)));
                y += 8;
            }

            auto buttons = row (32);
            copyButton.setBounds (buttons.removeFromLeft (copyButton.getBestWidth()));
            buttons.removeFromLeft (8);
            saveButton.setBounds (buttons.removeFromLeft (saveButton.getBestWidth()));
            closeButton.setBounds (buttons.removeFromRight (76));
            buttons.removeFromRight (8);
            againButton.setBounds (buttons.removeFromRight (std::min (againButton.getBestWidth(), std::max (0, buttons.getWidth() - 8))));
        }

        return y - top;
    };

    const int height = place (0, 0) + 2 * pad;
    card = juce::Rectangle<int> (0, 0, width, height).withCentre (getLocalBounds().getCentre());
    card.setY (std::max (cardMargin, card.getY()));
    place (card.getX() + pad, card.getY() + pad);
}

void ProToolsWriteSheet::paint (juce::Graphics& g)
{
    using ui::Theme;

    // The window behind is dimmed and takes no clicks while the sheet is up.
    g.fillAll (juce::Colours::black.withAlpha (0.6f));

    const auto cardBounds = card.toFloat().reduced (0.5f);
    g.setColour (Theme::bg());
    g.fillRoundedRectangle (cardBounds, 10.0f);
    g.setColour (Theme::line());
    g.drawRoundedRectangle (cardBounds, 10.0f, 1.0f);

    g.setColour (Theme::text());
    g.setFont (Theme::ui (17.0f, true));
    g.drawText ("Write to Pro Tools", titleArea, juce::Justification::centredLeft, true);

    if (stage == Stage::ready)
    {
        wrapped (instruction, bodyFont(), Theme::text(), instructionArea.getWidth()).draw (g, instructionArea.toFloat());

        g.setColour (pins.empty() ? Theme::dim() : Theme::text());
        g.setFont (strongFont());
        g.drawText (pinsLine(), pinsArea, juce::Justification::centredLeft, true);

        if (permissionAllowed)
        {
            bool warning = false;
            const auto text = counterLine (warning);
            wrapped (text, bodyFont(), warning ? Theme::check() : Theme::dim(), counterArea.getWidth()).draw (g, counterArea.toFloat());
        }
        else
        {
            auto r = permissionArea;
            const float dot = 8.0f;
            g.setColour (Theme::check());
            g.fillEllipse ((float) r.getX() + 1.0f, (float) r.getCentreY() - dot * 0.5f, dot, dot);
            r.removeFromLeft (17);

            const auto layoutOfLine = wrapped (permissionMissing, bodyFont(), Theme::text(), r.getWidth());
            layoutOfLine.draw (g, r.toFloat().withSizeKeepingCentre ((float) r.getWidth(), layoutOfLine.getHeight()));
            wrapped (permissionNote, bodyFont(), Theme::dim(), permissionNoteArea.getWidth()).draw (g, permissionNoteArea.toFloat());
        }
    }
    else if (stage == Stage::running)
    {
        const int done = progressDone.load(), total = progressTotal.load();

        g.setColour (Theme::text());
        g.setFont (Theme::ui (20.0f, true));
        g.drawText (progressText ((pt::Phase) progressPhase.load(), done, total), progressTextArea, juce::Justification::centredLeft, true);

        drawProgressBar (g, progressBarArea, fractionOf (done, total));

        g.setColour (Theme::dim());
        g.setFont (bodyFont());
        g.drawText (handsOff, progressNoteArea, juce::Justification::centredLeft, true);
    }
    else
    {
        const bool good = outcome.ok && ! outcome.okPressedOnFailedPin;
        wrapped (outcomeText(), strongFont(), good ? Theme::ok() : Theme::check(), outcomeArea.getWidth()).draw (g, outcomeArea.toFloat());

        if (! checkArea.isEmpty())
        {
            const bool checkGood = outcome.checked && outcome.off.empty();
            wrapped (checkText(), bodyFont(), checkGood ? Theme::text() : (outcome.checkSummary.empty() ? Theme::dim() : Theme::check()),
                     checkArea.getWidth()).draw (g, checkArea.toFloat());
        }

        if (! undoArea.isEmpty())
            wrapped (undoLine, bodyFont(), undoLineBad ? Theme::check() : Theme::dim(), undoArea.getWidth()).draw (g, undoArea.toFloat());
    }
}

} // namespace tt

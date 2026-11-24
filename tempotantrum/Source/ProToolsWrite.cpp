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
    const char* const linkedInstruction = "In Pro Tools, select the music clip. Then press Write.";
    const char* const capturedInstruction = "In Pro Tools, set the Main Counter to Samples. Then press Write.";
    const char* const capturedLinkedInstruction = "The music came from Pro Tools, so the pins go back where it was captured. Press Write.";
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

    juce::String utf8 (const std::string& text)
    {
        return juce::String::fromUTF8 (text.c_str());
    }

    // 44100 as "44.1 kHz", 48000 as "48 kHz".
    juce::String kiloHertz (double rate)
    {
        auto text = juce::String (rate / 1000.0, 3);

        while (text.containsChar ('.') && (text.endsWithChar ('0') || text.endsWithChar ('.')))
            text = text.dropLastCharacters (1);

        return text + " kHz";
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
        // longer than three seconds, so a stopped run ends within a few calls, after putting
        // back what it changed.
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
// Reads the session through Pro Tools' scripting link about once a second while the sheet waits
// for Write: the session, the selected clip, the counter and the transport. It only reads. It is
// stopped before a run starts, so it never sends a command while the run has a dialog open.
class ProToolsWriteSheet::LinkWatcher : public juce::Thread
{
public:
    LinkWatcher (ProToolsWriteSheet& owner, pt::MusicFacts facts)
        : juce::Thread ("Pro Tools session"), safeSheet (&owner), music (std::move (facts))
    {
    }

    ~LinkWatcher() override
    {
        // A call waiting for Pro Tools asks whether to stop every tenth of a second, and a read
        // asks between calls, so this returns at once.
        stopThread (15000);
    }

    void setMusic (const pt::MusicFacts& facts)
    {
        const juce::ScopedLock lock (musicLock);
        music = facts;
    }

    void run() override
    {
        ptsl::Client client;
        client.setStopCheck ([this] { return threadShouldExit(); });
        pt::SessionReader reader;

        while (! threadShouldExit())
        {
            pt::MusicFacts facts;

            {
                const juce::ScopedLock lock (musicLock);
                facts = music;
            }

            const auto reading = reader.read (client, facts, [this] { return threadShouldExit(); });

            if (threadShouldExit())
                break;

            juce::MessageManager::callAsync ([safe = safeSheet, reading]
            {
                if (safe != nullptr)
                    safe->setLink (reading);
            });

            // Pro Tools not answering in time usually has a dialog open. Every new connection
            // sends a registration it would run later, so it is asked less often then.
            wait (reading.timedOut ? 5000 : reading.linked ? 1000 : 2000);
        }
    }

private:
    juce::Component::SafePointer<ProToolsWriteSheet> safeSheet;
    juce::CriticalSection musicLock;
    pt::MusicFacts music;
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
    rateBox.onChange = [this] (double)
    {
        rateTyped = true;
        layout();
        repaint();
    };
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
    linkWatcher = nullptr;
    runner = nullptr;
    floating = nullptr;
}

//==============================================================================
void ProToolsWriteSheet::setSource (const std::vector<Pin>& newPins, int newBeatsPerBar, double musicSampleRate,
                                    long long musicLengthSamples, const juce::String& newMusicName, int newUnchecked)
{
    pins = newPins;
    beatsPerBar = newBeatsPerBar;
    musicName = newMusicName;
    musicLength = musicLengthSamples;
    unchecked = newUnchecked;

    // Other music: the rate box follows it again, or the session's rate when that is known.
    if (musicSampleRate > 0.0 && ! juce::exactlyEqual (musicSampleRate, shownMusicRate))
    {
        shownMusicRate = musicSampleRate;
        rateTyped = false;
        rateBox.setValue (linkInUse() && link.sampleRate > 0.0 ? link.sampleRate : musicSampleRate, juce::dontSendNotification);
    }

    if (linkWatcher != nullptr)
        linkWatcher->setMusic (musicFacts());

    updateControls();
}

void ProToolsWriteSheet::setProToolsOrigin (const ProToolsOrigin& newOrigin)
{
    if (newOrigin == origin)
        return;

    origin = newOrigin;
    updateControls();
}

pt::MusicStart ProToolsWriteSheet::musicStart() const
{
    return pt::musicStartFor (startBox.hasValue(), std::llround (startBox.getValue()), origin.known, origin.startKnown,
                              (long long) origin.startSample);
}

pt::MusicFacts ProToolsWriteSheet::musicFacts() const
{
    pt::MusicFacts facts;
    facts.fileName = musicName.toStdString();
    facts.sampleRate = shownMusicRate;
    facts.lengthSamples = musicLength;
    return facts;
}

bool ProToolsWriteSheet::linkInUse() const
{
    return linkKnown && link.linked && link.sessionOpen;
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

void ProToolsWriteSheet::showLinkAs (const pt::SessionReading& reading)
{
    forPicture = true;
    stopWatching();
    linkKnown = true;
    link = reading;

    if (! rateTyped && linkInUse() && link.sampleRate > 0.0)
        rateBox.setValue (link.sampleRate, juce::dontSendNotification);

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
    if (forPicture || ! isVisible())
        return;

    if (linkWatcher == nullptr)
    {
        if (ptsl::Client::hasLink())
        {
            linkKnown = false;
            linkWatcher = std::make_unique<LinkWatcher> (*this, musicFacts());
            linkWatcher->startThread();
        }
        else
        {
            linkKnown = true;
            link = {};
            link.problem = ptsl::noLinkMessage;
        }
    }

    if (watcher != nullptr || ! permissionAllowed)
        return;

    counterKnown = false;
    watcher = std::make_unique<Watcher> (*this);
    watcher->startThread();
}

void ProToolsWriteSheet::stopWatching()
{
    if (watcher != nullptr)
        watcher->signalThreadShouldExit();

    if (linkWatcher != nullptr)
        linkWatcher->signalThreadShouldExit();

    watcher = nullptr;
    linkWatcher = nullptr;
}

void ProToolsWriteSheet::setLink (const pt::SessionReading& reading)
{
    if (linkWatcher == nullptr)
        return;

    linkKnown = true;
    link = reading;

    // The session's rate fills the box, unless a rate has been typed there.
    if (! rateTyped && linkInUse() && link.sampleRate > 0.0 && ! juce::exactlyEqual (rateBox.getValue(), link.sampleRate))
        rateBox.setValue (link.sampleRate, juce::dontSendNotification);

    if (stage == Stage::ready)
    {
        layout();
        repaint();
    }
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
    settings.rateTyped = rateTyped;
    settings.music = musicFacts();
    const auto start = musicStart();
    settings.hasMusicStart = start.known;
    settings.musicStartSample = start.sample;
    settings.musicStartSource = pt::describeStart (start.from);
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
    else if (captured())
    {
        start = (long long) origin.startSample;
        text = "The music starts at sample " + grouped (start) + ", where the selection it was captured from starts.";

        if (counterKnown && counter.read && ! counter.isSamples)
        {
            warning = true;
            text << " The Main Counter shows " << juce::String::fromUTF8 (counter.text.c_str()).trim() << ". Set it to Samples.";
        }

        return text;
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

    if (const auto note = lastRunNote (start, typed); note.isNotEmpty())
    {
        warning = true;
        text << " " << note;
    }

    return text;
}

// What to look out for about a music start: the same as the last run's, one of its pins, or 0.
juce::String ProToolsWriteSheet::lastRunNote (long long start, bool typed) const
{
    if (hasLastStart && start == lastStart)
        return "The last run wrote from this same sample.";

    if (std::find (lastPinSamples.begin(), lastPinSamples.end(), start) != lastPinSamples.end())
        return "That is a pin of the last run, not the start of the music clip.";

    if (start == 0 && ! typed)
        return "That is the start of the session: check that the music clip starts there.";

    return {};
}

juce::String ProToolsWriteSheet::instructionText() const
{
    if (captured() && ! startBox.hasValue())
        return linkInUse() ? capturedLinkedInstruction : capturedInstruction;

    return linkInUse() ? linkedInstruction : instruction;
}

// The lines under the pins while the sheet waits for Write. With the scripting link: the session,
// the selected clip and what the write will change. Without it: what the Main Counter shows.
std::vector<ProToolsWriteSheet::Line> ProToolsWriteSheet::readyLines() const
{
    std::vector<Line> lines;
    const bool typed = startBox.hasValue();

    if (linkInUse())
    {
        lines.push_back ({ "Session: " + utf8 (link.sessionName), false, true });

        long long start = link.musicStart;
        const auto clipFile = link.selectedClips.size() == 1 ? utf8 (link.selectedClips.front()) : juce::String();

        if (typed)
        {
            start = std::llround (startBox.getValue());
            lines.push_back ({ "The music starts at sample " + grouped (start) + ", as typed under More." });
        }
        else if (captured())
        {
            start = (long long) origin.startSample;
            lines.push_back ({ "The music came from Pro Tools: it starts at sample " + grouped (start)
                               + ", where the selection it was captured from starts." });
        }
        else if (link.selectedClips.size() == 1 && link.hasSelection)
        {
            auto text = "Music clip selected: " + clipFile + ", starting at sample " + grouped (link.selectionIn) + ".";

            if (link.trim == pt::Trim::found && link.trimSamples != 0)
                text << " It starts " << grouped (link.trimSamples) << " samples into its file, so the music starts at sample "
                     << grouped (link.musicStart) << ".";

            lines.push_back ({ text });

            if (link.trim == pt::Trim::unknown)
                lines.push_back ({ "If the clip has been trimmed at its start, the grid will be off by the trimmed amount.", true });
        }
        else if (link.selectedClips.size() > 1)
        {
            lines.push_back ({ "More than one clip is selected. Select only the music clip.", true });
        }
        else
        {
            lines.push_back ({ "Select the music clip in Pro Tools.", true });
        }

        if (captured() && pt::otherSession (origin.sessionName.toStdString(), link.sessionName))
            lines.push_back ({ "The music was captured in the session " + origin.sessionName + ", not this one.", true });

        if (! captured() && clipFile.isNotEmpty() && ! link.fileIsMusic && musicName.isNotEmpty())
            lines.push_back ({ "The selected clip's file is " + clipFile + ", but the music open here is " + musicName + ".", true });

        if (! captured() && link.sampleRate > 0.0 && shownMusicRate > 0.0 && std::abs (link.sampleRate - shownMusicRate) > 0.5)
            lines.push_back ({ "The session runs at " + kiloHertz (link.sampleRate) + " and the music file at " + kiloHertz (shownMusicRate) + ".", true });

        if (rateTyped && link.sampleRate > 0.0 && std::abs (rateBox.getValue() - link.sampleRate) > 0.5)
            lines.push_back ({ "The rate typed under More is not the session's rate, " + kiloHertz (link.sampleRate) + ".", true });

        if (! link.counterType.empty() && ! link.counterIsSamples)
            lines.push_back ({ "The Main Counter will be switched to Samples for the write and put back afterwards." });

        if (link.transport.find ("Recording") != std::string::npos)
            lines.push_back ({ "Pro Tools is recording. Stop it before writing.", true });
        else if (! link.transport.empty() && ! link.stopped)
            lines.push_back ({ "Pro Tools is playing. It will be stopped for the write." });

        if (link.hasMusicStart || typed || captured())
            if (const auto note = lastRunNote (start, typed || captured()); note.isNotEmpty())
                lines.push_back ({ note, true });

        return lines;
    }

    if (linkKnown && link.linked && ! link.sessionOpen)
    {
        const auto problem = link.problem.empty() ? std::string ("The session could not be read") : link.problem;
        lines.push_back ({ utf8 (problem) + (problem.back() == '?' ? "" : "."), true });
        return lines;
    }

    if (! linkKnown && ptsl::Client::hasLink())
    {
        lines.push_back ({ "Reading the session in Pro Tools." });
        return lines;
    }

    bool warning = false;
    auto text = counterLine (warning);
    lines.push_back ({ text, warning });

    if (linkKnown && ! link.linked && ! link.problem.empty() && ! (counterKnown && ! counter.running))
        lines.push_back ({ "Scripting link: " + utf8 (link.problem) + (link.problem.back() == '?' ? "" : ".") });

    return lines;
}

juce::String ProToolsWriteSheet::outcomeText() const
{
    auto text = juce::String::fromUTF8 (outcome.summary.c_str());

    if (outcome.okPressedOnFailedPin)
        text << ". OK was pressed for that pin, so it may have been written: look in Pro Tools";

    return text;
}

// What follows the outcome: how much was written before a stop, how the cursor was moved, the
// check, the rest of the session and what was put back.
std::vector<ProToolsWriteSheet::Line> ProToolsWriteSheet::finishedLines() const
{
    std::vector<Line> lines;

    if (! outcome.ok && outcome.total > 0 && outcome.failedPin > 0)
        lines.push_back ({ juce::String (outcome.written) + " of " + countOf (outcome.total, "pin", "pins")
                           + (outcome.written == 1 ? " was written." : " were written.") });

    if (! outcome.linkNote.empty())
        lines.push_back ({ utf8 (outcome.linkNote) });

    if (! outcome.moveNote.empty())
        lines.push_back ({ utf8 (outcome.moveNote) + "." });

    if (! outcome.checkSummary.empty())
    {
        const bool good = outcome.checked && outcome.off.empty();
        juce::String text = utf8 (outcome.checkSummary) + (outcome.off.empty() ? "." : ":");
        const int shown = std::min ((int) outcome.off.size(), 5);

        for (int i = 0; i < shown; ++i)
        {
            const auto& off = outcome.off[(size_t) i];
            const auto amount = std::abs (off.inSamples ? off.samplesOff : off.ticksOff);
            const auto unit = off.inSamples ? (amount == 1 ? " sample " : " samples ") : (amount == 1 ? " tick " : " ticks ");
            const bool late = (off.inSamples ? off.samplesOff : off.ticksOff) > 0;
            text << "\nPin " << off.pin << ", " << utf8 (off.wanted) << ": " << amount << unit << (late ? "late" : "early");
        }

        if ((int) outcome.off.size() > shown)
            text << "\nand " << ((int) outcome.off.size() - shown) << " more, in the log.";

        lines.push_back ({ text, ! good, good });
    }

    if (! outcome.restSummary.empty())
        lines.push_back ({ utf8 (outcome.restSummary) + (outcome.restChanged ? "" : "."), outcome.restChanged, ! outcome.restChanged });

    if (! outcome.beforeSummary.empty())
        lines.push_back ({ utf8 (outcome.beforeSummary) });

    if (! outcome.restoredNote.empty())
        lines.push_back ({ utf8 (outcome.restoredNote), outcome.restoreFailed });

    return lines;
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

        instructionArea = pinsArea = permissionArea = permissionNoteArea = {};
        progressTextArea = progressBarArea = progressNoteArea = outcomeArea = undoArea = {};
        lineAreas.clear();

        // The lines of text under the pins or the outcome, 4 pixels apart.
        const auto lines = [&] (const std::vector<Line>& toPlace)
        {
            for (size_t i = 0; i < toPlace.size(); ++i)
            {
                if (i > 0)
                    y += 4;

                lineAreas.push_back (row (std::max (17, heightOf (toPlace[i].text, bodyFont(), inner))));
            }
        };

        if (stage == Stage::ready)
        {
            instructionArea = row (heightOf (instructionText(), bodyFont(), inner));
            y += 14;
            pinsArea = row (20);
            y += 6;

            if (permissionAllowed)
            {
                lines (readyLines());
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

            if (const auto more = finishedLines(); ! more.empty())
            {
                y += 10;
                lines (more);
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

    // The lines were laid out from the same list, so there is an area for each.
    const auto drawLines = [&] (const std::vector<Line>& toDraw)
    {
        for (size_t i = 0; i < toDraw.size() && i < lineAreas.size(); ++i)
        {
            const auto& line = toDraw[i];
            const auto colour = line.warning ? Theme::check() : line.strong ? Theme::text() : Theme::dim();
            wrapped (line.text, bodyFont(), colour, lineAreas[i].getWidth()).draw (g, lineAreas[i].toFloat());
        }
    };

    if (stage == Stage::ready)
    {
        wrapped (instructionText(), bodyFont(), Theme::text(), instructionArea.getWidth()).draw (g, instructionArea.toFloat());

        g.setColour (pins.empty() ? Theme::dim() : Theme::text());
        g.setFont (strongFont());
        g.drawText (pinsLine(), pinsArea, juce::Justification::centredLeft, true);

        if (permissionAllowed)
        {
            drawLines (readyLines());
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

        drawLines (finishedLines());

        if (! undoArea.isEmpty())
            wrapped (undoLine, bodyFont(), undoLineBad ? Theme::check() : Theme::dim(), undoArea.getWidth()).draw (g, undoArea.toFloat());
    }
}

} // namespace tt

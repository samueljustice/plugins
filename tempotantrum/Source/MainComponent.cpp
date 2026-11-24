#include "MainComponent.h"

#include "ProToolsTestPanel.h"
#include "ProToolsWrite.h"
#include "capture/AudioSuiteRoute.h"
#include "capture/ProToolsCapture.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace tt
{

namespace
{
    constexpr int headerHeight = 48;
    constexpr int footerHeight = 60;
    constexpr int statusHeight = 32;
    constexpr int sidePad = 16;
    constexpr int fieldRowHeight = 50;
    constexpr int fieldCaptionHeight = 14;
    constexpr int controlHeight = 30;

    juce::String formatLength (double seconds)
    {
        const int minutes = (int) (seconds / 60.0);
        const double rest = seconds - minutes * 60.0;
        return juce::String (minutes) + ":" + juce::String (rest, 1).paddedLeft ('0', 4);
    }

    void drawCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area)
    {
        g.setColour (ui::Theme::dim());
        g.setFont (ui::Theme::mono (10.0f));
        g.drawText (text.toUpperCase(), area, juce::Justification::centredLeft, true);
    }

    void drawValueBox (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text,
                       const juce::Font& font, juce::Colour textColour)
    {
        const auto r = area.toFloat().reduced (0.5f);
        g.setColour (ui::Theme::panel());
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (ui::Theme::line());
        g.drawRoundedRectangle (r, 6.0f, 1.0f);
        g.setColour (textColour);
        g.setFont (font);
        g.drawText (text, area.reduced (10, 0), juce::Justification::centredLeft, true);
    }

    // Selects a pin in the view when the view offers a way to (the shared contract does not).
    template <typename View>
    void selectPinInView (View& view, int index)
    {
        if constexpr (requires { view.setSelectedPin (index); })
            view.setSelectedPin (index);
    }

    // The choices of "Pin every", as numbers of bars. Zero is pins only where the tempo needs them.
    constexpr int pinEveryChoices[] = { 0, 8, 4, 2, 1 };

    // True for this key with exactly these modifiers. Letters match in either case.
    bool keyIs (const juce::KeyPress& key, int code, int modifiers)
    {
        const int held = key.getModifiers().getRawFlags()
                       & (juce::ModifierKeys::shiftModifier | juce::ModifierKeys::ctrlModifier
                          | juce::ModifierKeys::altModifier | juce::ModifierKeys::commandModifier);
        const int pressed = key.getKeyCode();

        return held == modifiers
            && (pressed == code || (pressed < 256 && code < 256
                                    && juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) pressed)
                                           == juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) code)));
    }

    bool isAudioFileName (juce::AudioFormatManager& formats, const juce::String& path)
    {
        const auto extension = juce::File (path).getFileExtension().toLowerCase();

        if (extension.isEmpty())
            return false;

        for (int i = 0; i < formats.getNumKnownFormats(); ++i)
            for (const auto& e : formats.getKnownFormat (i)->getFileExtensions())
                if (e.toLowerCase() == extension)
                    return true;

        return false;
    }
}

//==============================================================================
// "N places to check": what the engine was unsure of. Clicking it jumps to the next one.
class MainComponent::CheckBadge : public juce::Component,
                                  public juce::SettableTooltipClient
{
public:
    std::function<void()> onClick;

    void setState (int numChecks, bool mapped)
    {
        if (numChecks == checks && mapped == hasMap)
            return;

        checks = numChecks;
        hasMap = mapped;
        setTooltip (checks > 0 ? "N jumps to the next one and loops two bars either side of it" : juce::String());
        setMouseCursor (checks > 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (! hasMap)
            return;

        const auto colour = checks > 0 ? ui::Theme::check() : ui::Theme::ok();
        auto text = checks == 0 ? juce::String ("Nothing to check")
                  : checks == 1 ? juce::String ("1 place to check")
                                : juce::String (checks) + " places to check";

        const auto font = ui::Theme::ui (13.0f, true);
        float textWidth = juce::GlyphArrangement::getStringWidth (font, text);

        // Fewer words when the footer has no room for them all.
        if (textWidth + 34.0f > (float) getWidth())
        {
            text = checks == 0 ? juce::String ("No checks") : juce::String (checks) + " to check";
            textWidth = juce::GlyphArrangement::getStringWidth (font, text);
        }
        auto r = getLocalBounds().toFloat().reduced (0.5f, 4.0f);
        r = r.withWidth (std::min (r.getWidth(), textWidth + 34.0f));

        g.setColour (colour.withAlpha (0.14f));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (colour.withAlpha (0.6f));
        g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);

        const float dot = 7.0f;
        g.setColour (colour);
        g.fillEllipse (r.getX() + 11.0f, r.getCentreY() - dot * 0.5f, dot, dot);

        g.setFont (font);
        g.drawText (text, r.withTrimmedLeft (24.0f).withTrimmedRight (8.0f), juce::Justification::centredLeft, true);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (checks > 0 && onClick != nullptr && getLocalBounds().contains (e.getPosition()))
            onClick();
    }

private:
    int checks = -1;
    bool hasMap = false;
};

//==============================================================================
// Asked when an audio file is dropped while music is already open.
class MainComponent::DropChoice : public juce::Component
{
public:
    std::function<void (const juce::File&, bool asDrumStem)> onChoice;

    DropChoice()
    {
        for (auto* b : { &musicButton, &stemButton, &cancelButton })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (*b);
        }

        musicButton.onClick = [this] { choose (false); };
        stemButton.onClick = [this] { choose (true); };
        cancelButton.onClick = [this] { setVisible (false); };
    }

    void show (const juce::File& f)
    {
        file = f;
        setVisible (true);
        toFront (false);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (ui::Theme::panel());
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (ui::Theme::line());
        g.drawRoundedRectangle (r, 8.0f, 1.0f);

        g.setColour (ui::Theme::text());
        g.setFont (ui::Theme::ui (13.0f));
        g.drawText ("Use " + file.getFileName() + " as", textArea, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 7);
        cancelButton.setBounds (r.removeFromRight (74));
        r.removeFromRight (8);
        stemButton.setBounds (r.removeFromRight (100));
        r.removeFromRight (8);
        musicButton.setBounds (r.removeFromRight (100));
        r.removeFromRight (10);
        textArea = r.withTrimmedLeft (4);
    }

private:
    juce::File file;
    juce::Rectangle<int> textArea;
    ui::PillButton musicButton { "New music" }, stemButton { "Drum stem" }, cancelButton { "Cancel" };

    void choose (bool asStem)
    {
        setVisible (false);
        if (onChoice != nullptr)
            onChoice (file, asStem);
    }
};

//==============================================================================
MainComponent::MainComponent (bool withAudio)
    : audioEnabled (withAudio),
      checkBadge (std::make_unique<CheckBadge>()),
      dropChoice (std::make_unique<DropChoice>()),
      proToolsTest (std::make_unique<ProToolsTestPanel>()),
      proToolsWrite (std::make_unique<ProToolsWriteSheet>())
{
    setWantsKeyboardFocus (true);
    setOpaque (true);

    document.addChangeListener (this);
    document.onAudioAboutToChange = [this] (bool musicToo)
    {
        // Adding a drum stem leaves the music as it is, so the same stretch stays in view.
        rangeToKeep = musicToo ? juce::Range<double>() : juce::Range<double> (waveform.getViewStart(), waveform.getViewEnd());
        waveform.setAudio (nullptr, nullptr, 0.0);

        if (musicToo)
        {
            player.setMusic (nullptr, 0.0);
            needsPlayerMusic = true;
            checkIndex = -1;
            updatePlayButton();
        }
    };

    // Buttons and sliders never take the keyboard focus, so the window's keys keep working.
    for (auto* b : { &openButton, &getButton, &addStemButton, &tapButton, &playButton, &moreButton, &saveButton, &writeButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }

    openButton.onClick = [this] { chooseMusic(); };
    getButton.onClick = [this] { getFromProTools(); };
    getButton.setTooltip ("Renders the selection on the selected track in Pro Tools offline and opens it as the music (Cmd+Shift+G)");
    addStemButton.onClick = [this] { chooseDrumStem(); };
    tapButton.onClick = [this] { tap(); };
    playButton.onClick = [this] { togglePlay(); };
    moreButton.onClick = [this] { showMoreMenu(); };
    saveButton.onClick = [this] { saveMap(); };
    writeButton.onClick = [this] { showProToolsWrite(); };

    tapButton.setTooltip ("Tap along with the beat (or press Shift+T) to give a rough tempo");
    moreButton.setTooltip ("Export the click as a WAV file, or test the Pro Tools link step by step");
    writeButton.setTooltip ("Writes the pins into the open Pro Tools session with Event > Identify Beat");

    listenSwitch.setItems ({ "Full mix", "Drum stem" });
    listenSwitch.setSelected (0, juce::dontSendNotification);
    listenSwitch.setItemEnabled (1, false);
    listenSwitch.onChange = [this] (int index) { document.setListenToDrums (index == 1); };
    addAndMakeVisible (listenSwitch);

    barBox.setRange (1.0, 9999.0, 1.0, 0);
    barBox.setValue (1.0, juce::dontSendNotification);
    barBox.onChange = [this] (double v) { document.setFirstHitBar ((int) std::lround (v)); };
    addAndMakeVisible (barBox);

    beatBox.setRange (1.0, (double) Document::beatsPerBar, 1.0, 0);
    beatBox.setValue (1.0, juce::dontSendNotification);
    beatBox.onChange = [this] (double v) { document.setFirstHitBeat ((int) std::lround (v)); };
    addAndMakeVisible (beatBox);

    tempoBox.setRange (0.0, 400.0, 0.01, 2);
    tempoBox.setValue (0.0, juce::dontSendNotification);
    tempoBox.setText ("auto");
    tempoBox.onChange = [this] (double v)
    {
        // Below 20 there is no tempo to follow: the box goes back to "auto" and the tempo is estimated.
        if (v < 20.0)
        {
            v = 0.0;
            tempoBox.setValue (0.0, juce::dontSendNotification);
            tempoBox.setText ("auto");
        }

        document.setTempo (v);
    };
    addAndMakeVisible (tempoBox);

    pinEveryBox.setItems ({ "Only where needed", "8 bars", "4 bars", "2 bars", "1 bar" });
    pinEveryBox.setSelected (2, juce::dontSendNotification);
    pinEveryBox.onChange = [this] (int index)
    {
        if (index >= 0 && index < (int) std::size (pinEveryChoices))
            document.setPinEveryBars (pinEveryChoices[index]);
    };
    addAndMakeVisible (pinEveryBox);

    snapSwitch.setItems ({ "Hits", "Grid", "Off" });
    snapSwitch.setSelected (0, juce::dontSendNotification);
    snapSwitch.setTitle ("Snap");
    snapSwitch.onChange = [this] (int index)
    {
        using Snap = ui::WaveformView::Snap;
        waveform.setSnap (index == 0 ? Snap::hits : index == 1 ? Snap::grid : Snap::off);
    };
    addAndMakeVisible (snapSwitch);

    for (auto* s : { &clickLevel, &musicLevel })
    {
        s->setRange (0.0, 1.0, 0.0);
        s->setWantsKeyboardFocus (false);
        addAndMakeVisible (*s);
    }

    clickLevel.setValue (0.8, juce::dontSendNotification);
    musicLevel.setValue (1.0, juce::dontSendNotification);
    clickLevel.setTooltip ("Click level");
    musicLevel.setTooltip ("Music level");
    clickLevel.onValueChange = [this] { player.setClickGain ((float) clickLevel.getValue()); };
    musicLevel.onValueChange = [this] { player.setMusicGain ((float) musicLevel.getValue()); };
    player.setClickGain (0.8f);
    player.setMusicGain (1.0f);

    checkBadge->onClick = [this] { jumpToCheck (1); };
    addAndMakeVisible (*checkBadge);

    // The view's edits go straight to the document.
    waveform.onSeek = [this] (double seconds)
    {
        player.setPosition (seconds);
        waveform.setPlayhead (seconds);
    };
    waveform.onPinMoved = [this] (int index, double time) { document.movePin (index, time); };
    waveform.onPinNudged = [this] (int index, double time)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const int before = document.getEditSerial();
        document.movePin (index, time, before == nudgeSerial && index == nudgePin && now - lastNudgeMs < 1000.0);

        // A nudge that was refused starts no run.
        if (document.getEditSerial() != before)
        {
            nudgeSerial = document.getEditSerial();
            nudgePin = index;
            lastNudgeMs = now;

            if (! player.isPlaying())
            {
                player.setPosition (time);
                waveform.setPlayhead (time);
            }
        }
    };
    waveform.onPinAdded = [this] (double time) { document.addPin (time); };
    waveform.onPinDeleted = [this] (int index) { document.deletePin (index); };
    waveform.onRestartHere = [this] (double time) { document.restartHere (time); };
    waveform.onFirstHitHere = [this] (double time) { document.firstHitHere (time); };
    waveform.onPinBeatChanged = [this] (int index, double beat) { document.setPinBeat (index, beat); };
    addChildComponent (waveform);

    dropChoice->onChoice = [this] (const juce::File& file, bool asStem)
    {
        if (asStem)
            openDrumStem (file);
        else
            openMusic (file);
    };
    addChildComponent (*dropChoice);

    // Back to the window's own keys when the panel closes.
    proToolsTest->onClose = [this] { grabKeyboardFocus(); };
    addChildComponent (*proToolsTest);
    proToolsWrite->onClose = [this] { grabKeyboardFocus(); };
    addChildComponent (*proToolsWrite);

    if (audioEnabled)
    {
        const auto error = player.openDevice();
        if (error.isNotEmpty())
            document.setStatus ("No audio output: " + error);
    }

    // Captures from Pro Tools (this window's own, and the AudioSuite capture's) open here.
    capture::setReceiver ([safe = juce::Component::SafePointer<MainComponent> (this)] (const capture::Capture& captured)
    {
        if (safe != nullptr)
            safe->openCapture (captured);
    });

    setSize (1280, 760);
    refresh();
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    capture::setReceiver (nullptr);
    captureJob = nullptr;
    audioSuiteJob = nullptr;
    stopTimer();
    document.removeChangeListener (this);
    document.onAudioAboutToChange = nullptr;
    player.stop();
    player.setMusic (nullptr, 0.0);
    waveform.setAudio (nullptr, nullptr, 0.0);
}

//==============================================================================
void MainComponent::openMusic (const juce::File& file)
{
    askToSaveThen ([this, file]
    {
        player.stop();
        const auto result = document.loadMusic (file);

        if (result.failed())
            document.setStatus (result.getErrorMessage());

        updatePlayButton();
    });
}

void MainComponent::getFromProTools()
{
    if (captureJob == nullptr)
        captureJob = std::make_unique<capture::CaptureJob>();

    if (captureJob->isBusy() || (audioSuiteJob != nullptr && audioSuiteJob->isBusy()))
        return;

    getButton.setEnabled (false);
    document.setStatus (capture::renderingText);

    captureJob->start ([this] (const juce::String& line) { document.setStatus (line); },
                       [this] (const capture::Outcome& outcome) { captureFinished (outcome); });
}

void MainComponent::captureFinished (const capture::Outcome& outcome)
{
    {
        std::vector<std::string> lines { "Get from Pro Tools: offline Track Bounce" };
        lines.insert (lines.end(), outcome.log.begin(), outcome.log.end());
        lines.push_back (outcome.ok ? std::string ("Bounce: made") : "Bounce: " + outcome.error.toStdString());
        capture::appendToLog (lines);
    }

    if (outcome.ok)
    {
        getButton.setEnabled (true);
        openCapture (outcome.capture);
        return;
    }

    if (! outcome.useAudioSuite)
    {
        getButton.setEnabled (true);
        document.setStatus (outcome.error);
        return;
    }

    // The bounce cannot be used: Tempo Tantrum Capture, if Pro Tools has it.
    if (audioSuiteJob == nullptr)
        audioSuiteJob = std::make_unique<capture::AudioSuiteJob>();

    document.setStatus (juce::String::fromUTF8 ("Looking for Tempo Tantrum Capture in Pro Tools\xe2\x80\xa6"));

    const auto bounceError = outcome.error;
    audioSuiteJob->start (capture::AudioSuiteJob::Task::look, {},
                          [this, bounceError] (const capture::AudioSuiteOutcome& look) { audioSuiteLookFinished (look, bounceError); });
}

void MainComponent::audioSuiteLookFinished (const capture::AudioSuiteOutcome& outcome, const juce::String& bounceError)
{
    if (! outcome.found)
    {
        getButton.setEnabled (true);

        if (outcome.noAccessibility || outcome.error == "Pro Tools is not open")
            document.setStatus (bounceError + ". " + outcome.error);
        else
            document.setStatus (bounceError + ". To get the music another way, install Tempo Tantrum Capture (AAX), restart Pro Tools, "
                                "and press Get from Pro Tools again, or open the music from a file");
        return;
    }

    if (audioSuiteAgreed)
    {
        renderWithAudioSuite();
        return;
    }

    const auto options = juce::MessageBoxOptions()
                             .withIconType (juce::MessageBoxIconType::QuestionIcon)
                             .withTitle ("Get from Pro Tools")
                             .withMessage (capture::audioSuiteQuestion)
                             .withButton ("Go ahead")
                             .withButton ("Cancel")
                             .withAssociatedComponent (this);

    // 1 is Go ahead, 0 is Cancel (or Escape).
    audioSuiteQuestion = juce::AlertWindow::showScopedAsync (options, [this] (int choice)
    {
        if (choice != 1)
        {
            getButton.setEnabled (true);
            document.setStatus ("Nothing was sent to Pro Tools");
            return;
        }

        audioSuiteAgreed = true;
        renderWithAudioSuite();
    });
}

void MainComponent::renderWithAudioSuite()
{
    if (audioSuiteJob == nullptr)
        audioSuiteJob = std::make_unique<capture::AudioSuiteJob>();

    getButton.setEnabled (false);
    document.setStatus (juce::String::fromUTF8 ("Rendering the selection with Tempo Tantrum Capture\xe2\x80\xa6"));

    if (! audioSuiteJob->start (capture::AudioSuiteJob::Task::render, [this] (const juce::String& line) { document.setStatus (line); },
                                [this] (const capture::AudioSuiteOutcome& outcome) { audioSuiteRenderFinished (outcome); }))
        getButton.setEnabled (true);
}

void MainComponent::audioSuiteRenderFinished (const capture::AudioSuiteOutcome& outcome)
{
    getButton.setEnabled (true);

    // Back from Pro Tools, which was brought to the front for the render.
    juce::Process::makeForegroundProcess();

    if (auto* top = getTopLevelComponent())
        top->toFront (true);

    // On success the capture has opened through the inbox, with its own status line.
    if (! outcome.ok)
        document.setStatus (outcome.error);
}

void MainComponent::openCapture (const capture::Capture& captured)
{
    askToSaveThen ([this, captured]
    {
        player.stop();
        const auto result = document.loadCapture (captured);

        if (result.failed())
            document.setStatus (result.getErrorMessage());

        updatePlayButton();
    });
}

void MainComponent::showProToolsTest()
{
    proToolsTest->setSource (document.getMap().pins, Document::beatsPerBar, document.getSampleRate(),
                             document.hasMusic() ? document.getMusicFile().getFileName() : juce::String());
    proToolsTest->open();
}

void MainComponent::showProToolsWrite()
{
    if (! document.hasMap() || document.getMap().pins.empty())
    {
        document.setStatus ("There are no pins to write yet");
        return;
    }

    player.stop();
    updatePlayButton();
    proToolsWrite->setSource (document.getMap().pins, Document::beatsPerBar, document.getSampleRate(), (long long) document.getLengthInSamples(),
                              document.getMusicFile().getFileName(), (int) document.getMap().checks.size());
    proToolsWrite->setProToolsOrigin (document.getProToolsOrigin());
    proToolsWrite->open();
}

// The footer's More button: what is used less often than the buttons beside it.
void MainComponent::showMoreMenu()
{
    juce::PopupMenu menu;
    menu.addItem (juce::String::fromUTF8 ("Export click\xe2\x80\xa6"), document.hasMap(), false, [this] { exportClick(); });
    menu.addItem (juce::String::fromUTF8 ("Test in Pro Tools\xe2\x80\xa6"), [this] { showProToolsTest(); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (moreButton).withParentComponent (this));
}

void MainComponent::askToSaveThen (std::function<void()> then)
{
    if (! document.hasUnsavedChanges())
    {
        then();
        return;
    }

    const auto options = juce::MessageBoxOptions()
                             .withIconType (juce::MessageBoxIconType::QuestionIcon)
                             .withTitle ("Save the map?")
                             .withMessage ("The map for " + document.getMusicName() + " has changes that are not saved.")
                             .withButton ("Save")
                             .withButton ("Don't save")
                             .withButton ("Cancel")
                             .withAssociatedComponent (this);

    // 1 is Save, 2 is Don't save, 0 is Cancel (or Escape).
    saveQuestion = juce::AlertWindow::showScopedAsync (options, [this, next = std::move (then)] (int choice)
    {
        if (choice == 2 || (choice == 1 && document.saveMap().wasOk()))
            next();
    });
}

void MainComponent::openDrumStem (const juce::File& file)
{
    const auto result = document.loadDrumStem (file);

    if (result.failed())
        document.setStatus (result.getErrorMessage());
}

void MainComponent::chooseMusic()
{
    const auto start = document.getMusicFile() != juce::File() ? document.getMusicFile().getParentDirectory()
                                                               : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser> ("Open the music", start, document.getFormats().getWildcardForAllFormats());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file != juce::File())
                                  openMusic (file);
                          });
}

void MainComponent::chooseDrumStem()
{
    if (! document.hasMusic())
        return;

    chooser = std::make_unique<juce::FileChooser> ("Add a drum stem", document.getMusicFile().getParentDirectory(),
                                                   document.getFormats().getWildcardForAllFormats());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file != juce::File())
                                  openDrumStem (file);
                          });
}

void MainComponent::exportClick()
{
    if (! document.hasMap())
    {
        document.setStatus ("There is no map to make a click from yet");
        return;
    }

    chooser = std::make_unique<juce::FileChooser> ("Export the click", document.getDefaultClickFile(), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();
                              if (file == juce::File())
                                  return;

                              if (! file.hasFileExtension ("wav"))
                                  file = file.withFileExtension ("wav");

                              document.exportClick (file);
                          });
}

void MainComponent::saveMap()
{
    if (document.hasMusic())
        document.saveMap();
}

//==============================================================================
void MainComponent::togglePlay()
{
    if (! document.hasMusic())
        return;

    if (player.isPlaying())
    {
        player.stop();
        player.clearLoop();
    }
    else
    {
        if (! player.isDeviceOpen())
        {
            document.setStatus ("There is no audio output to play to");
            return;
        }

        player.play();
    }

    updatePlayButton();
}

void MainComponent::stopLooping()
{
    if (! player.isLooping())
        return;

    player.clearLoop();
    document.setStatus ("Stopped looping");
}

void MainComponent::jumpToCheck (int direction)
{
    const auto& checks = document.getMap().checks;

    if (checks.empty())
        return;

    const int n = (int) checks.size();
    checkIndex = checkIndex < 0 ? (direction > 0 ? 0 : n - 1) : (checkIndex + direction + n) % n;

    const auto& check = checks[(size_t) checkIndex];
    const auto& grid = document.getGrid();
    const double length = document.getLengthSeconds();

    double start = check.startTime - 4.0, end = std::max (check.endTime, check.startTime) + 4.0;

    if (! grid.empty())
    {
        const double twoBars = 2.0 * Document::beatsPerBar;
        start = grid.timeAtBeat (grid.beatAtTime (check.startTime) - twoBars);
        end = grid.timeAtBeat (grid.beatAtTime (std::max (check.endTime, check.startTime)) + twoBars);
    }

    start = juce::jlimit (0.0, length, start);
    end = juce::jlimit (0.0, length, end);

    if (end <= start)
        return;

    waveform.showRange (start, end);

    if (player.isDeviceOpen())
    {
        player.setLoop (start, end);
        player.setPosition (start);
        player.play();
        updatePlayButton();
    }

    const auto note = check.note.empty() ? juce::String ("Check this part") : juce::String (check.note);
    document.setStatus (note + "  (" + juce::String (checkIndex + 1) + " of " + juce::String (n) + ")");
}

void MainComponent::selectNeighbourPin (int direction)
{
    const auto& pins = document.getMap().pins;

    if (pins.empty())
        return;

    int index = waveform.getSelectedPin();

    if (index >= 0 && index < (int) pins.size())
    {
        index = juce::jlimit (0, (int) pins.size() - 1, index + direction);
    }
    else
    {
        const double now = player.getPlayheadSeconds();
        index = direction > 0 ? (int) pins.size() - 1 : 0;

        if (direction > 0)
        {
            for (int i = 0; i < (int) pins.size(); ++i)
                if (pins[(size_t) i].time > now + 0.001) { index = i; break; }
        }
        else
        {
            for (int i = (int) pins.size(); --i >= 0;)
                if (pins[(size_t) i].time < now - 0.001) { index = i; break; }
        }
    }

    selectPinInView (waveform, index);
    waveform.reveal (pins[(size_t) index].time);

    if (! player.isPlaying())
    {
        player.setPosition (pins[(size_t) index].time);
        waveform.setPlayhead (pins[(size_t) index].time);
    }
}

void MainComponent::seekTo (double seconds)
{
    if (! document.hasMusic())
        return;

    player.setPosition (seconds);
    waveform.setPlayhead (player.getPlayheadSeconds());
    waveform.reveal (seconds);
}

// Moves the playhead to the next detected hit, or the one before.
void MainComponent::moveToHit (int direction)
{
    if (! document.hasMusic())
        return;

    const double now = player.getPlayheadSeconds();
    double found = -1.0;

    for (const auto& hit : document.getMap().onsets)
    {
        if (direction > 0 ? (hit.time > now + 0.0005 && (found < 0.0 || hit.time < found))
                          : (hit.time < now - 0.0005 && hit.time > found))
            found = hit.time;
    }

    if (found >= 0.0)
        seekTo (found);
}

void MainComponent::nudgeSelectedPin (double seconds)
{
    if (document.hasMap() && ! waveform.nudgeSelectedPin (seconds))
        document.setStatus ("Select a pin to nudge it");
}

void MainComponent::setSnap (ui::WaveformView::Snap snap)
{
    using Snap = ui::WaveformView::Snap;
    snapSwitch.setSelected (snap == Snap::hits ? 0 : snap == Snap::grid ? 1 : 2, juce::sendNotification);
}

// What is in view when music has just been mapped: from just before the first pin to 16 bars
// after it.
void MainComponent::showOpeningView()
{
    const auto& pins = document.getMap().pins;
    const auto& grid = document.getGrid();

    if (pins.empty() || grid.empty())
        return;

    const double first = pins.front().time;
    const double end = grid.timeAtBeat (pins.front().beat + 16.0 * Document::beatsPerBar);

    if (end > first)
        waveform.showRange (first - (end - first) * 0.03, end);
}

void MainComponent::tap()
{
    if (! document.hasMusic())
        return;

    const double now = juce::Time::getMillisecondCounterHiRes();

    if (! taps.empty() && now - taps.back() > 2000.0)
        taps.clear();

    taps.push_back (now);

    if (taps.size() > 8)
        taps.erase (taps.begin());

    if (taps.size() < 3)
    {
        document.setStatus ("Tap along with the beat");
        return;
    }

    const double bpm = 60000.0 * (double) (taps.size() - 1) / (taps.back() - taps.front());
    const double rounded = std::round (bpm * 100.0) / 100.0;

    // The first tempo of a run of taps is its own undo step; the rest join it.
    document.setTempo (rounded, taps.size() > 3);
    document.setStatus ("Tapped " + juce::String (rounded, 2) + " bpm");
}

void MainComponent::updatePlayButton()
{
    playButton.setButtonText (player.isPlaying() ? "Stop" : "Play with click");
}

void MainComponent::updateCounter()
{
    juce::String counter, tempo;

    if (document.hasMusic() && document.hasMap())
    {
        const double t = player.getPlayheadSeconds();
        const auto& grid = document.getGrid();
        counter = juce::String (formatBarBeat (grid.beatAtTime (t), Document::beatsPerBar));
        tempo = juce::String (grid.bpmAtTime (t), 2) + " bpm";
    }

    if (counter != counterText || tempo != tempoText)
    {
        counterText = counter;
        tempoText = tempo;
        repaint (headerArea);
    }
}

//==============================================================================
void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void MainComponent::refresh()
{
    const bool hasMusic = document.hasMusic();

    if (document.getAudioVersion() != shownAudioVersion)
    {
        shownAudioVersion = document.getAudioVersion();

        if (hasMusic)
        {
            waveform.setAudio (&document.getMix(), document.hasDrums() ? &document.getDrums() : nullptr,
                               document.getSampleRate());

            if (! needsPlayerMusic && rangeToKeep.getLength() > 0.0)
                waveform.showRange (rangeToKeep.getStart(), rangeToKeep.getEnd());

            rangeToKeep = {};

            if (needsPlayerMusic)
            {
                player.setMusic (&document.getMix(), document.getSampleRate());
                needsPlayerMusic = false;
                needsOpeningView = true;
                waveform.showAll();
                waveform.setPlayhead (0.0);
            }
        }
        else
        {
            waveform.setAudio (nullptr, nullptr, 0.0);
        }
    }

    waveform.setVisible (hasMusic);

    if (document.getMapVersion() != mapVersionShown)
    {
        mapVersionShown = document.getMapVersion();
        waveform.setMap (document.getMap(), Document::beatsPerBar);

        if (checkIndex >= (int) document.getMap().checks.size())
            checkIndex = -1;

        if (needsOpeningView && document.hasMap())
        {
            needsOpeningView = false;
            showOpeningView();
        }
    }

    waveform.setBusy (document.isListening());

    // A run that has started keeps the pins it was given.
    if (proToolsTest->isVisible())
        proToolsTest->setSource (document.getMap().pins, Document::beatsPerBar, document.getSampleRate(),
                                 hasMusic ? document.getMusicFile().getFileName() : juce::String());

    if (proToolsWrite->isVisible() && ! proToolsWrite->isRunning())
        proToolsWrite->setSource (document.getMap().pins, Document::beatsPerBar, document.getSampleRate(),
                                  hasMusic ? (long long) document.getLengthInSamples() : 0LL,
                                  hasMusic ? document.getMusicFile().getFileName() : juce::String(),
                                  (int) document.getMap().checks.size());

    if (proToolsWrite->isVisible() && ! proToolsWrite->isRunning())
        proToolsWrite->setProToolsOrigin (document.getProToolsOrigin());

    const auto click = document.getClick();
    if (click.get() != shownClick)
    {
        shownClick = click.get();
        player.setClick (click);
    }

    // Fields: only touched when the value changed, so a box being typed in is left alone.
    const auto& in = document.getInputs();

    if (in.hint.bar != shownHint.bar)
        barBox.setValue ((double) in.hint.bar, juce::dontSendNotification);

    if (in.hint.beat != shownHint.beat)
        beatBox.setValue ((double) in.hint.beat, juce::dontSendNotification);

    if (! juce::exactlyEqual (in.hint.bpm, shownHint.bpm) || ! hintShown)
    {
        tempoBox.setValue (in.hint.bpm, juce::dontSendNotification);
        if (in.hint.bpm <= 0.0)
            tempoBox.setText ("auto");
    }

    shownHint = in.hint;
    hintShown = true;

    listenSwitch.setItemEnabled (1, document.hasDrums());
    listenSwitch.setSelected (in.listenToDrums && document.hasDrums() ? 1 : 0, juce::dontSendNotification);

    for (int i = 0; i < (int) std::size (pinEveryChoices); ++i)
        if (pinEveryChoices[i] == in.options.pinEveryBars)
            pinEveryBox.setSelected (i, juce::dontSendNotification);

    for (juce::Component* c : { (juce::Component*) &listenSwitch, (juce::Component*) &addStemButton,
                                (juce::Component*) &barBox, (juce::Component*) &beatBox, (juce::Component*) &tempoBox,
                                (juce::Component*) &tapButton, (juce::Component*) &clickLevel, (juce::Component*) &musicLevel,
                                (juce::Component*) &saveButton, (juce::Component*) &pinEveryBox, (juce::Component*) &snapSwitch })
        c->setEnabled (hasMusic);

    playButton.setEnabled (hasMusic && player.isDeviceOpen());
    writeButton.setEnabled (document.hasMap() && ! document.getMap().pins.empty());
    checkBadge->setState ((int) document.getMap().checks.size(), document.hasMap());

    const double refined = document.getMap().bpm;
    tempoBox.setStepStart (refined > 0.0 ? std::round (refined * 100.0) / 100.0 : 120.0);
    refinedTempoText = (hasMusic && refined > 0.0) ? "found " + juce::String (refined, 2) : juce::String();

    updatePlayButton();
    updateCounter();
    repaint();
}

void MainComponent::timerCallback()
{
    if (player.takeReachedEnd())
        updatePlayButton();

    if (document.hasMusic())
        waveform.setPlayhead (player.getPlayheadSeconds(), player.isPlaying());

    updateCounter();

    // Keep the keys working: if nothing has the focus (a box finished editing, a menu closed),
    // the window takes it back. Only while this is the front application: during a test run
    // Pro Tools is in front and keeps the keyboard.
    if (isShowing() && juce::Component::getCurrentlyFocusedComponent() == nullptr
        && juce::Process::isForegroundProcess())
    {
        if (proToolsTest->isVisible())
            proToolsTest->grabKeyboardFocus();
        else if (proToolsWrite->isVisible())
            proToolsWrite->grabKeyboardFocus();
        else
            grabKeyboardFocus();
    }
}

//==============================================================================
bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    using juce::KeyPress;
    using juce::ModifierKeys;

    const int cmd = ModifierKeys::commandModifier;
    const int shift = ModifierKeys::shiftModifier;
    const int alt = ModifierKeys::altModifier;

    // While the Pro Tools test or the write sheet is up, the window's own keys are off.
    if (proToolsTest->isVisible() || proToolsWrite->isVisible())
        return true;

    if (key == KeyPress ('o', cmd, 0))          { chooseMusic(); return true; }
    if (key == KeyPress ('g', cmd | shift, 0))  { getFromProTools(); return true; }
    if (key == KeyPress ('s', cmd, 0))          { saveMap(); return true; }

    // While a value is being typed, the other keys belong to the box. Tab is left unused there
    // and in the other fields, so it moves the focus on (which also finishes the typing).
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    const bool inField = focused != nullptr && focused != this && focused != &waveform && ! waveform.isParentOf (focused);

    if (dynamic_cast<juce::TextEditor*> (focused) != nullptr)
        return false;

    if (inField && key.isKeyCode (KeyPress::tabKey))
        return false;

    if (inField && key == KeyPress (KeyPress::escapeKey))
    {
        grabKeyboardFocus();   // out of the fields, back to the window's own keys
        return true;
    }
    if (key == KeyPress ('z', cmd, 0))          { document.undo(); return true; }
    if (key == KeyPress ('z', cmd | shift, 0))  { document.redo(); return true; }

    if (key == KeyPress (KeyPress::spaceKey))
    {
        if (player.isLooping())
        {
            player.stop();
            player.clearLoop();
            updatePlayButton();
            document.setStatus ("Stopped looping");
        }
        else
        {
            togglePlay();
        }
        return true;
    }

    if (key == KeyPress (KeyPress::escapeKey))      { stopLooping(); return true; }
    if (keyIs (key, 'n', 0))                        { jumpToCheck (1); return true; }
    if (keyIs (key, 'n', shift))                    { jumpToCheck (-1); return true; }
    if (keyIs (key, 't', shift))                    { tap(); return true; }

    // The view, with Pro Tools' keys.
    if (keyIs (key, 't', 0) || keyIs (key, ']', cmd))   { waveform.zoomBy (2.0); return true; }
    if (keyIs (key, 'r', 0) || keyIs (key, '[', cmd))   { waveform.zoomBy (0.5); return true; }
    if (keyIs (key, 'a', alt))                      { waveform.showAllAnimated(); return true; }
    if (keyIs (key, 'f', alt))                      { waveform.zoomToPinsAround(); return true; }
    if (keyIs (key, 'e', 0))                        { waveform.toggleZoom(); return true; }
    if (keyIs (key, ']', cmd | alt))                { waveform.stepWaveHeight (1); return true; }
    if (keyIs (key, '[', cmd | alt))                { waveform.stepWaveHeight (-1); return true; }

    // The playhead. Shift makes no difference to Tab: there is no selection to extend.
    if (keyIs (key, KeyPress::tabKey, 0) || keyIs (key, KeyPress::tabKey, shift))              { moveToHit (1); return true; }
    if (keyIs (key, KeyPress::tabKey, alt) || keyIs (key, KeyPress::tabKey, alt | shift))      { moveToHit (-1); return true; }
    if (keyIs (key, KeyPress::returnKey, 0))        { seekTo (0.0); return true; }

    // Pins. A full stop or comma with Shift arrives as its shifted character.
    if (keyIs (key, KeyPress::leftKey, 0))          { selectNeighbourPin (-1); return true; }
    if (keyIs (key, KeyPress::rightKey, 0))         { selectNeighbourPin (1); return true; }

    {
        const double oneSample = document.getSampleRate() > 0.0 ? 1.0 / document.getSampleRate() : 0.0;

        if (keyIs (key, ',', 0))                                { nudgeSelectedPin (-0.001); return true; }
        if (keyIs (key, '.', 0))                                { nudgeSelectedPin (0.001); return true; }
        // Shift with the comma key types '<' on US and UK keyboards and ';' on German and Swiss ones;
        // with the full stop key, '>' and ':'.
        if (keyIs (key, ',', shift) || keyIs (key, '<', shift) || keyIs (key, ';', shift)) { nudgeSelectedPin (-0.010); return true; }
        if (keyIs (key, '.', shift) || keyIs (key, '>', shift) || keyIs (key, ':', shift)) { nudgeSelectedPin (0.010); return true; }
        if (keyIs (key, ',', alt))                              { nudgeSelectedPin (-oneSample); return true; }
        if (keyIs (key, '.', alt))                              { nudgeSelectedPin (oneSample); return true; }
    }

    // Snap, on Pro Tools' Slip and Grid keys, with the hits between them.
    if (keyIs (key, KeyPress::F2Key, 0))            { setSnap (ui::WaveformView::Snap::off); return true; }
    if (keyIs (key, KeyPress::F3Key, 0))            { setSnap (ui::WaveformView::Snap::hits); return true; }
    if (keyIs (key, KeyPress::F4Key, 0))            { setSnap (ui::WaveformView::Snap::grid); return true; }

    if (key == KeyPress (KeyPress::backspaceKey) || key == KeyPress (KeyPress::deleteKey))
    {
        const int selected = waveform.getSelectedPin();
        if (selected >= 0)
            document.deletePin (selected);
        return true;
    }

    return false;
}

//==============================================================================
bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isAudioFileName (document.getFormats(), f))
            return true;

    return false;
}

void MainComponent::fileDragEnter (const juce::StringArray&, int, int)
{
    dragOver = true;
    repaint();
}

void MainComponent::fileDragExit (const juce::StringArray&)
{
    dragOver = false;
    repaint();
}

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    repaint();

    juce::File file;
    for (const auto& f : files)
        if (isAudioFileName (document.getFormats(), f))
        {
            file = juce::File (f);
            break;
        }

    if (file == juce::File())
        return;

    if (! document.hasMusic())
        openMusic (file);
    else
        dropChoice->show (file);
}

//==============================================================================
juce::String MainComponent::describeFile() const
{
    if (! document.hasMusic())
        return {};

    juce::String s = formatLength (document.getLengthSeconds()) + juce::String::fromUTF8 ("  \xc2\xb7  ")
                   + juce::String (document.getSampleRate() / 1000.0, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
                   + " kHz";

    if (document.hasDrums())
        s << juce::String::fromUTF8 ("  \xc2\xb7  drum stem ") << document.getDrumFile().getFileName();

    return s;
}

void MainComponent::paint (juce::Graphics& g)
{
    using ui::Theme;

    g.fillAll (Theme::bg());

    // Header
    {
        auto r = headerArea.reduced (sidePad, 0);

        g.setColour (Theme::text());
        g.setFont (Theme::ui (17.0f, true));
        const juce::String appName ("Tempo Tantrum");
        const int nameWidth = (int) std::ceil (juce::GlyphArrangement::getStringWidth (Theme::ui (17.0f, true), appName));
        g.drawText (appName, r.removeFromLeft (nameWidth + 4), juce::Justification::centredLeft, false);
        r.removeFromLeft (20);

        // Playhead counter at the right.
        if (counterText.isNotEmpty())
        {
            const auto tempoFont = Theme::mono (12.0f);
            const int tempoWidth = (int) std::ceil (juce::GlyphArrangement::getStringWidth (tempoFont, tempoText)) + 2;
            g.setColour (Theme::dim());
            g.setFont (tempoFont);
            g.drawText (tempoText, r.removeFromRight (tempoWidth), juce::Justification::centredRight, false);
            r.removeFromRight (14);

            const auto counterFont = Theme::mono (17.0f);
            const int counterWidth = (int) std::ceil (juce::GlyphArrangement::getStringWidth (counterFont, counterText)) + 2;
            g.setColour (Theme::text());
            g.setFont (counterFont);
            g.drawText (counterText, r.removeFromRight (counterWidth), juce::Justification::centredRight, false);
            r.removeFromRight (20);
        }

        if (document.hasMusic())
        {
            const auto nameFont = Theme::ui (13.0f, true);
            const auto name = document.getMusicName();
            const int w = std::min (r.getWidth(), (int) std::ceil (juce::GlyphArrangement::getStringWidth (nameFont, name)) + 2);
            g.setColour (Theme::text());
            g.setFont (nameFont);
            g.drawText (name, r.removeFromLeft (w), juce::Justification::centredLeft, true);
            r.removeFromLeft (12);

            g.setColour (Theme::dim());
            g.setFont (Theme::ui (13.0f));
            g.drawText (describeFile(), r, juce::Justification::centredLeft, true);
        }

        g.setColour (Theme::line());
        g.fillRect (headerArea.withTop (headerArea.getBottom() - 1));
    }

    // Fields
    drawCaption (g, "Music", musicCaption);
    drawValueBox (g, musicNameArea,
                  document.hasMusic() ? document.getMusicName() : juce::String ("No music open"),
                  Theme::ui (13.0f), document.hasMusic() ? Theme::text() : Theme::dim());

    drawCaption (g, "Listen to", listenCaption);

    if (refinedTempoText.isNotEmpty())
    {
        g.setColour (Theme::dim());
        g.setFont (Theme::mono (12.0f));
        g.drawText (refinedTempoText, tempoNoteArea, juce::Justification::centredLeft, true);
    }

    drawCaption (g, "Meter", meterCaption);
    drawValueBox (g, meterArea, "4/4", Theme::mono (13.0f), Theme::text());

    g.setColour (Theme::line());
    g.fillRect (fieldsArea.withTop (fieldsArea.getBottom() - 1));

    // Empty state
    if (! document.hasMusic())
    {
        g.setColour (Theme::dim());
        g.setFont (Theme::ui (15.0f));
        g.drawText ("Drop the trailer music here, or press Open", waveArea, juce::Justification::centred, true);
    }

    // Status line
    g.setColour (Theme::dim());
    g.setFont (Theme::ui (13.0f));
    g.drawText (document.getStatus(), statusArea.reduced (sidePad, 0).withRight (snapLabelArea.getX() - 12),
                juce::Justification::centredLeft, true);
    drawCaption (g, "Snap", snapLabelArea);

    // Footer
    g.setColour (Theme::line());
    g.fillRect (footerArea.withHeight (1));

    g.setColour (Theme::dim());
    g.setFont (Theme::mono (10.0f));
    g.drawText ("CLICK", clickLabelArea, juce::Justification::centredRight, false);
    g.drawText ("MUSIC", musicLabelArea, juce::Justification::centredRight, false);
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    if (! dragOver)
        return;

    const auto r = waveArea.reduced (10).toFloat();
    g.setColour (ui::Theme::pin().withAlpha (0.08f));
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (ui::Theme::pin());
    g.drawRoundedRectangle (r, 10.0f, 2.0f);
}

void MainComponent::layoutFields (juce::Rectangle<int> area)
{
    struct Group
    {
        int width;
        std::function<void (int x, int y)> place;
    };

    const int captionGap = 4;
    const int controlTop = fieldCaptionHeight + captionGap;
    const int boxHeight = controlTop + controlHeight;   // a NumberBox: its label above its value

    const int left = area.getX() + sidePad;
    const int right = area.getRight() - sidePad;
    const int pinEveryWidth = pinEveryBox.getBestWidth();

    // The groups sit 22 px apart and the music's name has 200 px. When that is a little too wide
    // for one row, the name gives up width (down to 96 px) and the gaps close (down to 14 px)
    // before the row is broken in two.
    const int numGaps = 5;
    const int getWidth = getButton.getBestWidth();
    const int withoutName = (8 + 76 + 8 + getWidth) + (184 + 8 + 144) + (120 + 8 + 64) + (96 + 8 + 96 + 6 + 56) + 56 + pinEveryWidth;
    int nameWidth = 200, groupGap = 22;

    if (withoutName + 200 + numGaps * 22 > right - left && withoutName + 96 + numGaps * 14 <= right - left)
    {
        nameWidth = std::min (200, right - left - withoutName - numGaps * 14);
        groupGap = std::min (22, (right - left - withoutName - nameWidth) / numGaps);
    }

    const std::vector<Group> groups {
        { nameWidth + 8 + 76 + 8 + getWidth, [&] (int x, int y)
            {
                musicCaption = { x, y, nameWidth, fieldCaptionHeight };
                musicNameArea = { x, y + controlTop, nameWidth, controlHeight };
                openButton.setBounds (x + nameWidth + 8, y + controlTop, 76, controlHeight);
                getButton.setBounds (x + nameWidth + 8 + 76 + 8, y + controlTop, getWidth, controlHeight);
            } },
        { 184 + 8 + 144, [&] (int x, int y)
            {
                listenCaption = { x, y, 184, fieldCaptionHeight };
                listenSwitch.setBounds (x, y + controlTop, 184, controlHeight);
                addStemButton.setBounds (x + 192, y + controlTop, 144, controlHeight);
            } },
        { 120 + 8 + 64, [&] (int x, int y)
            {
                barBox.setBounds (x, y, 120, boxHeight);
                beatBox.setBounds (x + 128, y, 64, boxHeight);
            } },
        { 96 + 8 + 96 + 6 + 56, [&] (int x, int y)
            {
                tempoBox.setBounds (x, y, 96, boxHeight);
                tempoNoteArea = { x + 104, y + controlTop, 96, controlHeight };
                tapButton.setBounds (x + 206, y + controlTop, 56, controlHeight);
            } },
        { 56, [&] (int x, int y)
            {
                meterCaption = { x, y, 56, fieldCaptionHeight };
                meterArea = { x, y + controlTop, 56, controlHeight };
            } },
        { pinEveryWidth, [&] (int x, int y)
            {
                pinEveryBox.setBounds (x, y, pinEveryWidth, boxHeight);
            } },
    };

    int x = left, y = area.getY() + 10;

    for (const auto& group : groups)
    {
        if (x > left && x + group.width > right)
        {
            x = left;
            y += fieldRowHeight + 8;
        }

        group.place (x, y);
        x += group.width + groupGap;
    }
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    headerArea = r.removeFromTop (headerHeight);

    // Lay the fields out once to see how many rows they need at this width, then for real.
    layoutFields ({ r.getX(), r.getY(), r.getWidth(), 1000 });
    const int fieldsBottom = std::max ({ meterArea.getBottom(), musicNameArea.getBottom(), tempoBox.getBottom(),
                                         listenSwitch.getBottom(), barBox.getBottom(), pinEveryBox.getBottom() });
    fieldsArea = r.removeFromTop (fieldsBottom - r.getY() + 12);

    footerArea = r.removeFromBottom (footerHeight);
    statusArea = r.removeFromBottom (statusHeight);
    waveArea = r;
    waveform.setBounds (waveArea);

    // The Snap switch at the right of the status row, with its label before it.
    {
        auto s = statusArea.reduced (sidePad, 4);
        snapSwitch.setBounds (s.removeFromRight (std::min (snapSwitch.getBestWidth(), std::max (0, s.getWidth() - 44))));
        s.removeFromRight (8);
        snapLabelArea = s.removeFromRight (34);
    }

    // Footer: play and levels at the left, the file buttons at the right, the check badge between.
    auto f = footerArea.reduced (sidePad, (footerHeight - 32) / 2);

    // In a narrow window the buttons and level sliders give up some width, and the check badge
    // shortens its words when it has to.
    const bool narrow = getWidth() < 1200;
    const int gap = narrow ? 6 : 8;
    const int sliderWidth = narrow ? 32 : 70;

    // A narrow button is its text and 10 px either side.
    const auto widthOf = [narrow] (const ui::PillButton& b, int roomy) { return narrow ? b.getBestWidth() - 10 : roomy; };

    writeButton.setBounds (f.removeFromRight (widthOf (writeButton, 156)));
    f.removeFromRight (gap);
    saveButton.setBounds (f.removeFromRight (widthOf (saveButton, 96)));
    f.removeFromRight (gap);
    moreButton.setBounds (f.removeFromRight (widthOf (moreButton, 72)));
    f.removeFromRight (narrow ? 8 : 16);

    playButton.setBounds (f.removeFromLeft (narrow ? 114 : 132));
    f.removeFromLeft (narrow ? 8 : 14);
    clickLabelArea = f.removeFromLeft (narrow ? 34 : 38);
    f.removeFromLeft (narrow ? 4 : 6);
    clickLevel.setBounds (f.removeFromLeft (sliderWidth));
    f.removeFromLeft (narrow ? 6 : 12);
    musicLabelArea = f.removeFromLeft (narrow ? 34 : 40);
    f.removeFromLeft (narrow ? 4 : 6);
    musicLevel.setBounds (f.removeFromLeft (sliderWidth));
    f.removeFromLeft (narrow ? 8 : 16);
    checkBadge->setBounds (f.removeFromLeft (std::max (0, std::min (190, f.getWidth()))));

    dropChoice->setBounds (juce::Rectangle<int> (0, 0, std::min (560, waveArea.getWidth() - 2 * sidePad), 46)
                               .withCentre ({ waveArea.getCentreX(), waveArea.getY() + 40 }));
    proToolsTest->setBounds (getLocalBounds());
    proToolsWrite->setBounds (getLocalBounds());
}

} // namespace tt

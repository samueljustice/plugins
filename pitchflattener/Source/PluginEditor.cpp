#include "PluginEditor.h"

#include "AboutWindow.h"
#include "UpdatePopup.h"

namespace
{
#ifdef PLUGIN_VERSION
    const char* const c_version = PLUGIN_VERSION;
#else
    const char* const c_version = JucePlugin_VersionString;
#endif

    using State = pitchflattener::Engine::State;
    using Processor = PitchFlattenerAudioProcessor;

    constexpr int c_margin = 16;
    constexpr int c_gap = 10;

    const char* const c_targetLabels[] = { "HIGH POINT", "CENTRE", "LOW POINT", "START", "NOTE" };
    const char* const c_targetTips[] = {
        "High Point: flattens the sound to the top of its pitch (a pass-by's approach, a siren's peak). "
        "The flattened sound is never longer than the original.",
        "Centre: flattens the sound to the pitch it spends most of its time at, so it changes least overall.",
        "Low Point: flattens the sound to the bottom of its pitch. The flattened sound is longer than the original.",
        "Start: flattens the sound to the pitch it opens on.",
        "Note: flattens the sound to the pitch set with the Note control."
    };
    const char* const c_sourceLabels[] = { "AUTO", "VOICE", "ENGINE", "TONE" };
    const char* const c_sourceTips[] = {
        "Source, Auto: reads the sound all three ways and uses the reading that flattens it best. Choose a kind of sound when it gets it wrong.",
        "Source, Voice: a sound with one clear pitch: voices, animal calls, single tones and whines.",
        "Source, Engine: engines, props and rich drones, where many partials rise and fall together.",
        "Source, Tone: sirens, whistles and feedback, or a whine inside noise: follows the strongest partial at any height."
    };
    const char* const c_lengthLabels[] = { "VARISPEED", "KEEP LENGTH" };
    const char* const c_lengthTips[] = {
        "Varispeed: flattens by changing the speed, like slowing and speeding a tape. The cleanest result; the sound's length changes.",
        "Keep length: flattens the pitch and keeps the sound's own timing and length. Choose a character to suit the sound."
    };
    const char* const c_characterLabels[] = { "HARMONIC", "TRANSIENTS", "VOICE" };
    const char* const c_characterTips[] = {
        "Character, Harmonic: for sounds that are mostly tone: sirens, drones, engines, pads.",
        "Character, Transients: for sounds with hits in them: ricochets, impacts, anything percussive. Keeps each hit sharp and in place.",
        "Character, Voice: for voices, creatures and animal calls. Keeps the voice's own size and character while its pitch is flattened."
    };
    const char* const c_pullHelp = "DRAG THE LINE TO CORRECT IT. DOUBLE-CLICK A POINT TO REMOVE IT.";
    const char* const c_drawHelp = "DRAW ALONG THE GRAPH TO REDRAW THE LINE. ALT WHILE DRAGGING LETS GO OF THE SNAP.";

    juce::String seconds (double s) { return juce::String (s, 2) + " S"; }

    const char* const c_learnTip =
        "Learn: press, then play the sound once through the plug-in. It lets the sound through while it records, stops after a "
        "second of silence (or when Learn is pressed again, keeping what it heard) and flattens it. From then on it recognises "
        "that same recording, played at the same speed and at any level, from the start or from any point in it, and plays the "
        "flattened copy in its place; any other audio passes through unchanged. To hear the input itself again, bypass the "
        "plug-in or press Clear.";
    const char* const c_clearTip = "Clear: forgets the learned sound. Audio then passes through unchanged until another sound is learned.";
    const char* const c_cancelTip = "Stops Learn without learning anything. A sound learned before is kept.";
    const char* const c_amountTip =
        "Amount: how much of the pitch movement is taken out. 100% leaves the sound flat; 50% leaves half of its movement in. "
        "At 0% the learned copy still plays in place of the input, untouched in pitch.";
    const char* const c_learnedStatus = "LEARNED: PLAY THE SAME RECORDING AGAIN, AT THE SAME SPEED, TO HEAR IT FLATTENED. ANYTHING ELSE PASSES THROUGH.";
    const char* const c_noPitchStatus = "NO STEADY PITCH FOUND IN THIS SOUND: IT PLAYS AS IT IS. LEARN ANOTHER SOUND.";
    const char* const c_noPitchSourceStatus = "NO STEADY PITCH FOUND IN THIS SOUND: IT PLAYS AS IT IS. LEARN ANOTHER, OR TRY A SOURCE.";
}

//==============================================================================
PitchFlattenerAudioProcessorEditor::PitchFlattenerAudioProcessorEditor (PitchFlattenerAudioProcessor& p)
    : AudioProcessorEditor (&p)
    , m_processor (p)
    , m_lookAndFeel (*m_fonts)
{
    using namespace Theme::Colours;
    setLookAndFeel (&m_lookAndFeel);
    setOpaque (true);
    Hud::setFramebufferHost (*this);   // the children draw only through the framebuffer
    m_tooltipWindow.setLookAndFeel (&m_lookAndFeel);
    PitchFlattenerLookAndFeel::showFocusRingsAfterTab (*this);

    m_learn.onClick = [this] { learnClicked(); };
    m_learn.setTooltip (c_learnTip);
    addAndMakeVisible (m_learn);

    m_clear.onClick = [this]
    {
        // CANCEL (while Learn is armed, listening or analysing) stops Learn and keeps a sound
        // learned before; CLEAR forgets the learned sound.
        auto& engine = m_processor.getEngine();
        const auto state = engine.state();
        if (state == State::Armed || state == State::Listening || state == State::Analysing)
            m_processor.cancelLearn();
        else
            engine.clear();
        refreshNow();
    };
    m_clear.setTooltip (c_clearTip);
    addAndMakeVisible (m_clear);

    m_about.onClick = [this] { showAbout(); };
    m_about.setTooltip ("About Pitch Flattener: version, credits and the update check.");
    addAndMakeVisible (m_about);

    for (size_t i = 0; i < m_targets.size(); ++i)
    {
        auto& b = m_targets[i];
        b = std::make_unique<HudButton> (c_targetLabels[i], HudButton::Style::Segment);
        b->setTooltip (c_targetTips[i]);
        b->onClick = [this, i] { m_processor.setParameterFromEditor (Processor::targetId, (float) i); refreshNow(); };
        addAndMakeVisible (*b);
    }

    setupKnob (m_note, Processor::noteId,
               "Note: the pitch the sound is flattened to when the target is Note. Type a note name such as A3, or C#4 +20 for cents.",
               m_noteAttachment);
    setupKnob (m_amount, Processor::amountId,
               c_amountTip,
               m_amountAttachment);

    for (size_t i = 0; i < m_sources.size(); ++i)
    {
        auto& b = m_sources[i];
        b = std::make_unique<HudButton> (c_sourceLabels[i], HudButton::Style::Segment);
        b->setTooltip (c_sourceTips[i]);
        b->onClick = [this, i]
        {
            m_processor.setParameterFromEditor (Processor::sourceId, (float) i);
            m_processor.syncOptions();
            refreshNow();
        };
        addAndMakeVisible (*b);
    }
    for (size_t i = 0; i < m_lengthModes.size(); ++i)
    {
        auto& b = m_lengthModes[i];
        b = std::make_unique<HudButton> (c_lengthLabels[i], HudButton::Style::Segment);
        b->setTooltip (c_lengthTips[i]);
        b->onClick = [this, i]
        {
            m_processor.setParameterFromEditor (Processor::keepLengthId, (float) i);
            m_processor.syncOptions();
            refreshNow();
        };
        addAndMakeVisible (*b);
    }
    for (size_t i = 0; i < m_characters.size(); ++i)
    {
        auto& b = m_characters[i];
        b = std::make_unique<HudButton> (c_characterLabels[i], HudButton::Style::Segment);
        b->setTooltip (c_characterTips[i]);
        b->onClick = [this, i]
        {
            m_processor.setParameterFromEditor (Processor::characterId, (float) i);
            m_processor.syncOptions();
            refreshNow();
        };
        addAndMakeVisible (*b);
    }

    // The curve editor's tools.
    m_drawTool.setTooltip ("Draw: drag across the graph to redraw the line freehand. Off, a drag pulls the line at one point.");
    m_drawTool.onClick = [this] { m_display.setDrawMode (! m_display.drawMode()); refreshNow(); };
    m_snapTool.setTooltip ("Snap: corrections put the pitch on the nearest semitone. Hold Alt while dragging to let go of it.");
    m_snapTool.onClick = [this] { m_display.setSnap (! m_display.snap()); refreshNow(); };
    m_undo.setTooltip ("Undo the last correction (Cmd or Ctrl and Z, once the graph has been clicked).");
    m_undo.onClick = [this] { undoEdit(); };
    m_redo.setTooltip ("Redo the correction last undone (Shift, Cmd or Ctrl and Z).");
    m_redo.onClick = [this] { redoEdit(); };
    m_resetCurve.setTooltip ("Reset curve: takes every correction off this reading, so the sound is flattened by the line as it was read.");
    m_resetCurve.onClick = [this] { resetCurve(); };
    m_zoomOut.setTooltip ("Zoom out. Scrolling over the graph zooms time about the pointer; with Alt, the pitch.");
    m_zoomOut.onClick = [this] { m_display.zoomTime (0.5 * (m_display.viewFrom() + m_display.viewTo()), 2.0); };
    m_zoomIn.setTooltip ("Zoom in. Scrolling over the graph zooms time about the pointer; with Alt, the pitch.");
    m_zoomIn.onClick = [this] { m_display.zoomTime (0.5 * (m_display.viewFrom() + m_display.viewTo()), 0.5); };
    m_fit.setTooltip ("Fit: shows the whole sound, with the pitch range set to suit it.");
    m_fit.onClick = [this] { m_display.fit(); };
    for (auto* b : { &m_drawTool, &m_snapTool, &m_undo, &m_redo, &m_resetCurve, &m_zoomOut, &m_zoomIn, &m_fit })
        addChildComponent (*b);

    m_scopeTab.setTooltip ("Scope: the learned sound as an exact graph of its pitch over time, the target and the play head.");
    m_flightTab.setTooltip ("Flight: the learned sound flown as a course, its pitch as a track that rises and falls beside the "
                            "level lane of the flattened sound.");
    m_scopeTab.onClick = [this] { chooseView (Processor::kViewScope); };
    m_flightTab.onClick = [this] { chooseView (Processor::kViewFlight); };
    addAndMakeVisible (m_scopeTab);
    addAndMakeVisible (m_flightTab);

    m_display.onLineClicked = [this] (int line)
    {
        m_processor.setParameterFromEditor (Processor::sourceId, (float) (line + 1));
        m_processor.syncOptions();
        refreshNow();
    };
    // While a line is dragged the engine renders a few times a second; the finished edit at once.
    m_display.onEditsChanging = [this] (const PitchDisplay::CurvePoints& points)
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (now - m_lastLivePush >= kLivePushSeconds)
        {
            m_lastLivePush = now;
            m_processor.setEdits (m_processor.lineInUse(), points);
        }
        refreshNow();
    };
    m_display.onEdited = [this] (const PitchDisplay::CurvePoints& before, const PitchDisplay::CurvePoints& after)
    {
        const int line = m_processor.lineInUse();
        m_undoSteps.push_back ({ line, before });
        m_redoSteps.clear();
        applyEdits (line, after);
    };
    m_display.onUndoKey = [this] (bool redo) { redo ? redoEdit() : undoEdit(); };
    m_display.onViewChanged = [this] { refreshNow(); };
    addChildComponent (m_display);
    addChildComponent (m_flight);
    addAndMakeVisible (m_banner);   // over both views, never taking a click

    // Children were made before they had this LookAndFeel's parent; rebuild their parts with it.
    sendLookAndFeelChange();

    setSize (width, height);
    refreshNow();
    startTimerHz (30);

    // Looks for a newer release on a background thread (at most once a day, silent on any failure).
    m_updateCheck = std::make_unique<sammyj::UpdateCheck> ("pitchflattener", "pitchflattener-v", c_version,
        [this] (const sammyj::UpdateInfo& info) { showUpdatePopup (info); });
}

PitchFlattenerAudioProcessorEditor::~PitchFlattenerAudioProcessorEditor()
{
    stopTimer();
    m_updateCheck.reset();
    m_updatePopup.reset();
    m_aboutWindow.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    m_tooltipWindow.hideTip();
    m_tooltipWindow.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void PitchFlattenerAudioProcessorEditor::setupKnob (juce::Slider& slider, const char* paramId, const juce::String& tooltip,
                                                    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& attachment)
{
    slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 40);   // drawn in the gauge's middle (see the LookAndFeel)
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    slider.setColour (juce::Slider::rotarySliderFillColourId, Theme::Colours::orange);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (m_processor.parameters, paramId, slider);
    if (auto* param = m_processor.parameters.getParameter (paramId))
        slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
    slider.setTooltip (tooltip);
    addAndMakeVisible (slider);
}

//==============================================================================
void PitchFlattenerAudioProcessorEditor::timerCallback()
{
    // Hidden (minimised, another tab of the host): nothing moves and nothing is drawn; a slow tick
    // only notices when it shows again.
    if (! isShowing())
    {
        if (getTimerInterval() != 250)
            startTimer (250);
        return;
    }
    if (getTimerInterval() == 250)
        startTimerHz (30);

    refreshNow();
}

void PitchFlattenerAudioProcessorEditor::learnClicked()
{
    auto& engine = m_processor.getEngine();
    if (engine.isLearnArmed())
        engine.disarmLearn();
    else
        engine.armLearn();
    refreshNow();
}

void PitchFlattenerAudioProcessorEditor::showAbout()
{
    if (m_aboutWindow != nullptr)
    {
        m_aboutWindow->setMinimised (false);
        m_aboutWindow->toFront (true);
        return;
    }

    m_aboutWindow = std::make_unique<AboutWindow> (m_lookAndFeel, [this] { m_aboutWindow.reset(); });
}

juce::Component* PitchFlattenerAudioProcessorEditor::aboutContent() const
{
    return m_aboutWindow != nullptr ? m_aboutWindow->getContentComponent() : nullptr;
}

int PitchFlattenerAudioProcessorEditor::shownView() const
{
    const int chosen = m_processor.getDisplayView();
    if (chosen != Processor::kViewUnset)
        return chosen;
    // Not chosen yet: the attract loop while nothing is learned, the exact graph once something is.
    return m_processor.getEngine().result() != nullptr ? Processor::kViewScope : Processor::kViewFlight;
}

void PitchFlattenerAudioProcessorEditor::chooseView (int view)
{
    m_processor.setDisplayView (view);
    refreshNow();
}

PitchFlattenerAudioProcessorEditor::Playback PitchFlattenerAudioProcessorEditor::playbackOf (pitchflattener::Engine::Playback mode,
                                                                                           bool playingCurrentRender) noexcept
{
    using Mode = pitchflattener::Engine::Playback;
    Playback p;
    p.mode = mode;
    p.playing = mode == Mode::Flattened || mode == Mode::Tail;
    p.current = p.playing && playingCurrentRender;
    return p;
}

juce::String PitchFlattenerAudioProcessorEditor::learnedStatus (pitchflattener::Engine::Playback mode)
{
    using Mode = pitchflattener::Engine::Playback;
    switch (mode)
    {
        case Mode::Flattened:      return "PLAYING THE FLATTENED SOUND";
        case Mode::Tail:           return "PLAYING THE END OF THE FLATTENED SOUND: IT IS LONGER THAN THE ORIGINAL";
        case Mode::FlattenedEnded: return "THE FLATTENED SOUND HAS ENDED: IT IS SHORTER THAN THE ORIGINAL";
        case Mode::Idle:
        case Mode::Searching:      break;
    }
    return c_learnedStatus;
}

bool PitchFlattenerAudioProcessorEditor::BounceWarning::update (bool waitGaveUp, const pitchflattener::Result* result) noexcept
{
    if (result == nullptr)
        shown = false;                                   // Clear (or nothing learned)
    else if (waitGaveUp)
    {
        shown = true;
        generation = result->generation;
    }
    else if (shown && result->generation != generation)
        shown = false;                                   // a newer render arrived, and the wait did not give up again
    return shown;
}

juce::String PitchFlattenerAudioProcessorEditor::renderingText (const Playback& playback, bool rendering)
{
    // Playing while a newer render is on its way (being made, or made and not yet taken over):
    // what is heard is the version before the last change.
    if (playback.playing && (rendering || ! playback.current))
        return "RENDERING: STILL PLAYING THE PREVIOUS VERSION";
    return rendering ? "RENDERING" : "";
}

void PitchFlattenerAudioProcessorEditor::refreshNow()
{
    using namespace Theme::Colours;
    auto& engine = m_processor.getEngine();
    const auto state = engine.state();
    const auto result = engine.result();
    const auto playback = playbackOf (engine.playback(), engine.playingCurrentRender());
    const bool playing = playback.playing;
    const bool bounceWarning = m_bounceWarning.update (engine.offlineWaitGaveUp(), result.get());
    const double now = m_clock.now();

    // The delay the plug-in reports (longer than the least for very large host blocks).
    {
        const double rate = engine.sampleRate();
        const double delay = rate > 0.0 ? (double) m_processor.getLatencySamples() / rate : pitchflattener::Engine::kLatencySeconds;
        if (const auto t = juce::String (delay, 2) + " S"; t != m_delayText)
        {
            m_delayText = t;
            repaint (m_header);
        }
    }

    // What to do or what is happening (the guidance bar), and the Learn button's two lines.
    juce::String learnHint, status;
    juce::Colour statusColour = text, learnColour = orange;
    bool learnFilled = false;
    switch (state)
    {
        case State::Idle:
            m_learnText = "LEARN";
            learnHint = "PLAY THE SOUND ONCE";
            status = "PRESS LEARN, THEN PLAY THE SOUND ONCE";
            break;
        case State::Armed:
            m_learnText = "ARMED";
            learnHint = "WAITING FOR SOUND";
            learnColour = lime;
            learnFilled = true;
            status = "LEARN IS ARMED: PLAY THE SOUND ONCE";
            statusColour = lime;
            break;
        case State::Listening:
            m_learnText = "LISTENING";
            learnHint = juce::String (engine.captureSeconds(), 1) + " S";
            learnColour = magenta;
            learnFilled = true;
            status = "LISTENING: LET THE SOUND PLAY TO ITS END";
            statusColour = magenta;
            break;
        case State::Analysing:
            m_learnText = "ANALYSING";
            learnHint = "READING THE PITCH";
            learnColour = cyan;
            learnFilled = true;
            status = "ANALYSING THE SOUND";
            statusColour = cyan;
            break;
        case State::NothingToFlatten:
        {
            m_learnText = "NO PITCH";
            learnHint = "PRESS TO LEARN ANOTHER";
            learnColour = magenta;
            // Choosing a SOURCE helps only when some reading is usable (otherwise every choice
            // falls back to the pick, and there is none).
            bool anyUsable = false;
            if (result != nullptr && result->learned != nullptr)
                for (const auto& l : result->learned->analysis.lines)
                    anyUsable = anyUsable || l.usable;
            status = anyUsable ? c_noPitchSourceStatus : c_noPitchStatus;
            statusColour = magenta;
            break;
        }
        case State::Ready:
            m_learnText = "LEARNED";
            learnHint = "PRESS TO LEARN ANOTHER";
            if (engine.sampleRateMismatch() && result != nullptr)
            {
                const auto learnedAt = juce::String (juce::roundToInt (result->learned->sampleRate));
                status = "LEARNED AT " + learnedAt + " HZ, THE SESSION IS AT " + juce::String (juce::roundToInt (engine.sampleRate()))
                       + " HZ: SET THE SESSION BACK TO " + learnedAt + " HZ, OR LEARN THE SOUND AGAIN";
                statusColour = magenta;
            }
            else if (bounceWarning)
            {
                status = kBounceWarning;
                statusColour = magenta;
            }
            else
            {
                status = learnedStatus (playback.mode);
                statusColour = playback.mode == pitchflattener::Engine::Playback::FlattenedEnded ? text : lime;
            }
            break;
    }
    // Pressing Learn is the next step only while nothing is learned or armed.
    m_learn.setContent (m_learnText, learnHint, learnColour, learnFilled, state == State::Idle);

    juce::String before, after, flatText;
    if (result != nullptr && result->playable())
    {
        const double rate = result->learned->sampleRate;
        before = seconds ((double) result->learned->locator.length() / rate);
        after = seconds ((double) result->flattened.channels[0].size() / rate);
        const auto events = result->learned->analysis.events.size();
        flatText = events > 1 && result->flattened.eventTargets.size() == events ? "SEVERAL " + juce::String ((int) events)
                                                                                : PitchFlattenerAudioProcessor::noteText (result->flattened.targetMidi);
    }
    else if (result != nullptr)
        before = seconds ((double) result->learned->locator.length() / result->learned->sampleRate);

    // CLEAR, or CANCEL while Learn is under way.
    {
        const bool capturing = state == State::Armed || state == State::Listening || state == State::Analysing;
        const juce::String label = capturing ? "CANCEL" : "CLEAR";
        const juce::String tip = capturing ? c_cancelTip : c_clearTip;
        if (m_clear.getButtonText() != label)
            m_clear.setButtonText (label);
        if (m_clear.getTooltip() != tip)
            m_clear.setTooltip (tip);
        m_clear.setEnabled (capturing || result != nullptr);
    }

    // Nothing to flatten: the controls do nothing, so they are dimmed (keeping their values).
    const bool live = state != State::NothingToFlatten;
    if (live != m_controlsLive)
    {
        m_controlsLive = live;
        repaint (m_targetCell.getUnion (m_characterCell));
    }
    for (auto& b : m_targets)
        b->setEnabled (live);
    for (auto& b : m_sources)
        b->setEnabled (live);
    for (auto& b : m_lengthModes)
        b->setEnabled (live);
    m_amount.setEnabled (live);

    // Learned at another rate: the readouts describe a sound that does not play.
    if (const bool dim = state == State::Ready && engine.sampleRateMismatch(); dim != m_readoutsDim)
    {
        m_readoutsDim = dim;
        repaint (m_readout);
    }

    const int chosen = juce::roundToInt (m_processor.parameters.getRawParameterValue (Processor::targetId)->load());
    for (size_t i = 0; i < m_targets.size(); ++i)
        if (m_targets[i]->getToggleState() != ((int) i == chosen))
            m_targets[i]->setToggleState ((int) i == chosen, juce::dontSendNotification);
    m_note.setEnabled (chosen == 4 && live);

    const int source = juce::roundToInt (m_processor.parameters.getRawParameterValue (Processor::sourceId)->load());
    for (size_t i = 0; i < m_sources.size(); ++i)
        if (m_sources[i]->getToggleState() != ((int) i == source))
            m_sources[i]->setToggleState ((int) i == source, juce::dontSendNotification);
    const int keep = m_processor.parameters.getRawParameterValue (Processor::keepLengthId)->load() > 0.5f ? 1 : 0;
    for (size_t i = 0; i < m_lengthModes.size(); ++i)
        if (m_lengthModes[i]->getToggleState() != ((int) i == keep))
            m_lengthModes[i]->setToggleState ((int) i == keep, juce::dontSendNotification);
    // Character only shapes Keep length: dimmed (and keeping its value) under Varispeed.
    const int character = juce::roundToInt (m_processor.parameters.getRawParameterValue (Processor::characterId)->load());
    for (size_t i = 0; i < m_characters.size(); ++i)
    {
        if (m_characters[i]->getToggleState() != ((int) i == character))
            m_characters[i]->setToggleState ((int) i == character, juce::dontSendNotification);
        if (m_characters[i]->isEnabled() != (keep == 1 && live))
        {
            m_characters[i]->setEnabled (keep == 1 && live);
            repaint (m_characterCell);
        }
    }

    // Undo belongs to the sound it was made on.
    if (const auto id = result != nullptr && result->learned != nullptr ? result->learned->id : 0; id != m_stepsFor)
    {
        m_stepsFor = id;
        m_undoSteps.clear();
        m_redoSteps.clear();
    }

    // The two views: only the one shown is fed and drawn.
    const int view = shownView();
    m_display.setVisible (view == Processor::kViewScope);
    m_flight.setVisible (view == Processor::kViewFlight);
    m_scopeTab.setToggleState (view == Processor::kViewScope, juce::dontSendNotification);
    m_flightTab.setToggleState (view == Processor::kViewFlight, juce::dontSendNotification);

    PitchDisplay::View displayView;
    displayView.result = result;
    displayView.state = state;
    displayView.captureSeconds = engine.captureSeconds();
    displayView.playPosition = playback.current ? engine.playPosition() : -1;
    displayView.playGeneration = engine.playGeneration();
    displayView.rendering = engine.isRendering();
    displayView.autoSource = source == 0;
    displayView.lineInUse = m_processor.lineInUse();
    displayView.edits = m_processor.getEdits (displayView.lineInUse);
    m_display.setView (displayView);
    updateTools();

    // Editing on the graph: the guidance says how.
    if (view == Processor::kViewScope && state == State::Ready && (m_display.isEditing() || m_display.isHoveringEditable()))
    {
        status = m_display.drawMode() ? c_drawHelp : c_pullHelp;
        statusColour = orange;
    }

    if (view == Processor::kViewFlight)
    {
        double playSeconds = -1.0;
        if (playback.current)
            playSeconds = result->flattened.sourceSampleFor ((double) engine.playPosition()) / result->learned->sampleRate;
        m_flight.setView (result, playSeconds);
        m_flight.renderFrame();
    }

    // The banners.
    BannerLogic::Input in;
    in.state = state;
    in.playing = playing;
    in.rateMismatch = engine.sampleRateMismatch();
    in.learnedId = result != nullptr && result->learned != nullptr ? result->learned->id : 0;
    m_bannerLogic.update (in, now);
    m_banner.show (m_bannerLogic.kind(), m_bannerLogic.since(), engine.captureSeconds());
    const auto rendering = m_renderingOverride.isNotEmpty() ? m_renderingOverride : renderingText (playback, engine.isRendering());
    if (m_bannerLogic.flatTag() != m_flatTagShown || rendering != m_renderingText)
    {
        m_flatTagShown = m_bannerLogic.flatTag();
        m_renderingText = rendering;
        repaint (m_statusSlot.expanded (2));
    }
    m_banner.tick();
    if (m_learn.isPulsing())
        m_learn.repaint();

    if (status != m_status || before != m_lengthBefore || after != m_lengthAfter || flatText != m_flatText || statusColour != m_statusColour)
    {
        m_status = status;
        m_lengthBefore = before;
        m_lengthAfter = after;
        m_flatText = flatText;
        m_statusColour = statusColour;
        repaint (m_readout);
        repaint (m_guide);
    }
    repaint (m_guide.withWidth (60));   // only its chevron marker moves
}

//==============================================================================
void PitchFlattenerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (c_margin);

    m_header = area.removeFromTop (50);
    m_about.setBounds (m_header.withTrimmedLeft (m_header.getWidth() - 40).withSizeKeepingCentre (40, 30));

    area.removeFromTop (8);
    auto action = area.removeFromTop (64);
    m_learn.setBounds (action.removeFromLeft (330));
    action.removeFromLeft (c_gap);
    m_clear.setBounds (action.removeFromLeft (130).reduced (0, 8));
    action.removeFromLeft (c_gap);
    m_readout = action;

    area.removeFromTop (c_gap);
    m_displayPanel = area.removeFromTop (290);
    {
        const auto header = m_displayPanel.withHeight (30).reduced (8, 5);
        auto tabs = header;
        m_flightTab.setBounds (tabs.removeFromRight (58));
        tabs.removeFromRight (4);
        m_scopeTab.setBounds (tabs.removeFromRight (54));

        // From the left after the heading: the editing tools, then the zoom; then the tag place
        // (FLAT, or RENDERING ...) running up to the tabs, where it covers nothing.
        auto tools = header.withTrimmedLeft (104);
        const auto place = [&tools] (HudButton& b, int w, int gap)
        {
            b.setBounds (tools.removeFromLeft (w));
            tools.removeFromLeft (gap);
        };
        place (m_drawTool, 40, 4);
        place (m_snapTool, 40, 6);
        place (m_undo, 48, 4);
        place (m_redo, 48, 4);
        place (m_resetCurve, 104, 6);
        place (m_zoomOut, 26, 3);
        place (m_zoomIn, 26, 3);
        place (m_fit, 40, 8);
        m_statusSlot = tools.withRight (m_scopeTab.getX() - 8);
        m_viewArea = m_displayPanel.withTrimmedTop (30).reduced (3, 0).withTrimmedBottom (3);
    }
    m_display.setBounds (m_viewArea);
    m_flight.setBounds (m_viewArea);
    m_banner.setBounds (m_viewArea);
    m_banner.setLearnButtonX ((float) (m_learn.getBounds().getCentreX() - m_viewArea.getX()));

    area.removeFromTop (8);
    m_guide = area.removeFromTop (40);
    area.removeFromTop (8);

    auto controls = area;
    m_targetCell = controls.removeFromLeft (250);
    controls.removeFromLeft (c_gap);
    m_noteCell = controls.removeFromLeft (114);
    controls.removeFromLeft (c_gap);
    m_amountCell = controls.removeFromLeft (114);
    controls.removeFromLeft (c_gap);
    m_sourceCell = controls.removeFromLeft (170);
    controls.removeFromLeft (c_gap);
    m_lengthCell = controls.removeFromLeft (130);
    controls.removeFromLeft (c_gap);
    m_characterCell = controls;

    {
        // The two long names on top, the three short ones below, so every name fits its segment.
        auto cell = m_targetCell.withTrimmedTop (40).reduced (14, 0);
        auto top = cell.removeFromTop (38);
        cell.removeFromTop (8);
        auto bottom = cell.removeFromTop (38);
        const int w2 = (top.getWidth() - 8) / 2;
        m_targets[0]->setBounds (top.removeFromLeft (w2));   // HIGH POINT
        top.removeFromLeft (8);
        m_targets[2]->setBounds (top);                       // LOW POINT
        const int w3 = (bottom.getWidth() - 2 * 8) / 3;
        m_targets[1]->setBounds (bottom.removeFromLeft (w3)); // CENTRE
        bottom.removeFromLeft (8);
        m_targets[3]->setBounds (bottom.removeFromLeft (w3)); // START
        bottom.removeFromLeft (8);
        m_targets[4]->setBounds (bottom);                    // NOTE
    }
    m_note.setBounds (m_noteCell.withTrimmedTop (34).reduced (14, 6));
    m_amount.setBounds (m_amountCell.withTrimmedTop (34).reduced (14, 6));
    {
        auto cell = m_sourceCell.withTrimmedTop (40).reduced (14, 0);
        auto top = cell.removeFromTop (38);
        cell.removeFromTop (8);
        auto bottom = cell.removeFromTop (38);
        const int w = (top.getWidth() - 8) / 2;
        m_sources[0]->setBounds (top.removeFromLeft (w));
        top.removeFromLeft (8);
        m_sources[1]->setBounds (top);
        m_sources[2]->setBounds (bottom.removeFromLeft (w));
        bottom.removeFromLeft (8);
        m_sources[3]->setBounds (bottom);
    }
    {
        auto cell = m_lengthCell.withTrimmedTop (40).reduced (14, 0);
        m_lengthModes[0]->setBounds (cell.removeFromTop (38));
        cell.removeFromTop (8);
        m_lengthModes[1]->setBounds (cell.removeFromTop (38));
    }
    {
        auto cell = m_characterCell.withTrimmedTop (40).reduced (14, 0);
        for (size_t i = 0; i < m_characters.size(); ++i)
        {
            m_characters[i]->setBounds (cell.removeFromTop (32));
            cell.removeFromTop (6);
        }
    }

    if (m_updatePopup != nullptr)
        m_updatePopup->setBounds (getLocalBounds());
}

void PitchFlattenerAudioProcessorEditor::paintHeader (juce::Graphics& g)
{
    using namespace Theme::Colours;
    auto header = m_header.toFloat();

    // The mark, then the name, sheared and large, with the idea drawn under it: a cyan wobble that
    // runs into a flat lime line.
    PitchFlattenerLookAndFeel::drawMark (g, header.removeFromLeft (40.0f).withSizeKeepingCentre (40.0f, 26.0f));
    header.removeFromLeft (12.0f);
    const auto titleFont = m_fonts->display (27.0f);
    const juce::String title = "PITCH FLATTENER";
    const float titleWidth = Hud::shearedWidth (titleFont, title) + 2.0f;
    const auto titleBox = header.removeFromLeft (titleWidth).withTrimmedBottom (12.0f);
    Hud::drawSheared (g, title, titleBox, titleFont, text, juce::Justification::centredLeft);
    {
        const float y = titleBox.getBottom() + 4.0f;
        const float x0 = titleBox.getX(), x1 = titleBox.getRight(), split = x0 + (x1 - x0) * 0.42f;
        juce::Path wobble;
        for (int i = 0; i <= 60; ++i)
        {
            const float t = (float) i / 60.0f;
            const float x = x0 + (split - x0) * t;
            const float yy = y + 3.5f * (1.0f - t) * std::sin (t * juce::MathConstants<float>::twoPi * 4.0f);
            if (i == 0) wobble.startNewSubPath (x, yy);
            else        wobble.lineTo (x, yy);
        }
        g.setColour (cyan);
        g.strokePath (wobble, juce::PathStrokeType (2.0f));
        g.setColour (lime);
        g.fillRect (juce::Rectangle<float> (split, y - 1.5f, x1 - split, 3.0f));
        // A second, thinner stripe below, as a speed line.
        g.fillRect (juce::Rectangle<float> (split + (x1 - split) * 0.35f, y + 4.0f, (x1 - split) * 0.65f, 1.5f));
    }
    header.removeFromLeft (16.0f);
    Hud::drawText (g, "SAMMYJS", header.removeFromLeft (80.0f).withTrimmedBottom (12.0f), m_fonts->label (11.0f), textDim, juce::Justification::centredLeft);

    // Right: the delay the plug-in reports and the version, beside the About button.
    auto right = m_header.toFloat().withTrimmedRight (52.0f);
    const auto small = m_fonts->label (11.0f);
    const auto value = m_fonts->display (12.0f);
    const auto readout = [&] (const juce::String& label, const juce::String& v, juce::Colour colour)
    {
        const float vw = Hud::shearedWidth (value, v) + 2.0f;
        Hud::drawSheared (g, v, right.removeFromRight (vw), value, colour, juce::Justification::centredRight);
        right.removeFromRight (6.0f);
        const float lw = Hud::textWidth (small, label) + 2.0f;
        Hud::drawText (g, label, right.removeFromRight (lw), small, textDim, juce::Justification::centredRight);
        right.removeFromRight (22.0f);
    };
    readout ("VERSION", juce::String (c_version), text.withAlpha (0.85f));
    readout ("DELAY", m_delayText, text.withAlpha (0.85f));
}

void PitchFlattenerAudioProcessorEditor::paintReadout (juce::Graphics& g)
{
    using namespace Theme::Colours;
    const auto r = m_readout.toFloat();
    Hud::frame (g, r, 12.0f, panel, panelEdge);

    auto inner = r.reduced (18.0f, 8.0f);
    const auto labelFont = m_fonts->label (11.0f);
    const auto valueFont = m_fonts->display (18.0f);
    const float labelWidth = 74.0f;

    const auto row = [&] (juce::Rectangle<float> line, const juce::String& label)
    {
        Hud::drawText (g, label, line.removeFromLeft (labelWidth), labelFont, textDim, juce::Justification::centredLeft);
        return line;
    };

    // Length: as heard (cyan), and flattened (lime). Learned at another rate, these describe a
    // sound that is not played: dimmed.
    const auto shade = [this] (juce::Colour c) { return m_readoutsDim ? textDim.withAlpha (0.6f) : c; };
    auto top = row (inner.removeFromTop (inner.getHeight() * 0.5f), "LENGTH");
    if (m_lengthBefore.isEmpty())
        Hud::drawSheared (g, "--", top.withWidth (60.0f), valueFont, textDim);
    else
    {
        const float w1 = Hud::shearedWidth (valueFont, m_lengthBefore) + 2.0f;
        Hud::drawSheared (g, m_lengthBefore, top.removeFromLeft (w1), valueFont, shade (cyan));
        if (m_lengthAfter.isNotEmpty())
        {
            top.removeFromLeft (8.0f);
            Hud::chevron (g, { top.getX() + 6.0f, top.getCentreY() }, 5.0f, 0.0f, 2.0f, textDim);
            Hud::chevron (g, { top.getX() + 13.0f, top.getCentreY() }, 5.0f, 0.0f, 2.0f, textDim);
            top.removeFromLeft (26.0f);
            Hud::drawSheared (g, m_lengthAfter, top.removeFromLeft (Hud::shearedWidth (valueFont, m_lengthAfter) + 2.0f), valueFont, shade (lime));
        }
    }

    // The pitch it is flattened to: the user's target, orange (several sounds: how many).
    m_flatRow = inner.toNearestInt();
    auto bottom = row (inner, "FLAT ON");
    Hud::drawSheared (g, m_flatText.isEmpty() ? juce::String ("--") : m_flatText,
                      bottom.withWidth (std::min (bottom.getWidth(), Hud::shearedWidth (valueFont, m_flatText.isEmpty() ? juce::String ("--") : m_flatText) + 2.0f)),
                      valueFont, m_flatText.isEmpty() ? textDim : shade (orange));
}

void PitchFlattenerAudioProcessorEditor::paintGuide (juce::Graphics& g)
{
    using namespace Theme::Colours;
    const auto r = m_guide.toFloat();
    Hud::frame (g, r, 10.0f, panel, m_statusColour.withAlpha (0.45f));

    // The marker: three chevrons with a light running through them, pointing at the words.
    auto area = r.reduced (16.0f, 0.0f);
    auto marker = area.removeFromLeft (40.0f);
    const int lit = (int) std::floor (m_clock.now() * 6.0) % 4;
    for (int i = 0; i < 3; ++i)
        Hud::chevron (g, { marker.getX() + 6.0f + (float) i * 11.0f, marker.getCentreY() }, 6.0f, 0.0f, 2.5f,
                      m_statusColour.withAlpha (i == lit ? 1.0f : 0.35f));
    area.removeFromLeft (10.0f);

    const float h = Hud::fitHeight ([this] (float fh) { return m_fonts->body (fh); }, m_status, area.getWidth(), 17.0f, 11.0f, false);
    Hud::drawText (g, m_status, area, m_fonts->body (h), m_statusColour, juce::Justification::centredLeft);
}

void PitchFlattenerAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Only the area being repainted is drawn afresh into the framebuffer, then shown.
    const auto region = g.getClipBounds().getIntersection (getLocalBounds());
    m_framebuffer.render (*this, region, [this] (juce::Graphics& fb) { paintContent (fb); });
    m_framebuffer.draw (g, region);

    // A repaint area with an odd edge cuts through blocks: the blocks were finished afresh on both
    // sides of it, but the screen only took the inside. The outside pixels are shown at once by a
    // repaint of the whole blocks, so no stale strip is ever left along the edge.
    if (const auto aligned = followUpRepaint (region); ! aligned.isEmpty())
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<PitchFlattenerAudioProcessorEditor> (this), aligned]
        {
            if (safeThis != nullptr)
                safeThis->repaint (aligned);
        });
}

juce::Rectangle<int> PitchFlattenerAudioProcessorEditor::followUpRepaint (juce::Rectangle<int> region) const
{
    const auto aligned = Framebuffer::blockAligned (region).getIntersection (getLocalBounds());
    return aligned == region ? juce::Rectangle<int>() : aligned;
}

void PitchFlattenerAudioProcessorEditor::paintContent (juce::Graphics& g)
{
    using namespace Theme::Colours;
    g.fillAll (space);

    // Only the parts in the area being repainted (a pulse or a marker repaints a few pixels).
    const auto in = [&g] (juce::Rectangle<int> r) { return g.clipRegionIntersects (r); };
    if (in (m_header))
        paintHeader (g);
    if (in (m_readout))
        paintReadout (g);

    // The display's frame and heading (the views fill its inside).
    if (in (m_displayPanel))
    {
        const auto r = m_displayPanel.toFloat();
        Hud::frame (g, r, 12.0f, space, panelEdge);
        const bool scope = shownView() == PitchFlattenerAudioProcessor::kViewScope;
        // (SCOPE's tools take the header's middle, so its heading is the one word.)
        Hud::heading (g, juce::Rectangle<float> (r.getX() + 16.0f, r.getY() + 6.0f, scope ? 90.0f : 420.0f, 18.0f),
                      scope ? "SCOPE" : "FLIGHT  /  THE SOUND AS A COURSE", m_fonts->display (12.0f), orange);

        // The tag place before the tabs: RENDERING (a render on its way, and what is heard
        // meanwhile), or FLAT (the flattened sound is playing).
        const auto slot = m_statusSlot.toFloat();
        if (m_renderingText.isNotEmpty())
        {
            const auto font = m_fonts->body (10.0f);
            const float w = std::min (slot.getWidth(), Hud::textWidth (font, m_renderingText) + 14.0f);
            const auto tag = slot.withLeft (slot.getRight() - w);
            Hud::frame (g, tag, 5.0f, space, lime.withAlpha (0.85f), 1.5f);
            Hud::drawText (g, m_renderingText, tag.reduced (6.0f, 0.0f), font, lime, juce::Justification::centred);
        }
        else if (m_flatTagShown)
        {
            const auto font = m_fonts->display (12.0f);
            const juce::String word = "FLAT";
            const float w = Hud::shearedWidth (font, word) + 30.0f;
            const auto tag = slot.withLeft (slot.getRight() - w);
            Hud::frame (g, tag, 5.0f, lime, juce::Colours::transparentBlack);
            g.setColour (space);
            g.fillRect (juce::Rectangle<float> (tag.getX() + 8.0f, tag.getCentreY() - 3.0f, 6.0f, 6.0f));
            Hud::drawSheared (g, word, tag.withTrimmedLeft (18.0f).withTrimmedRight (5.0f), font, space, juce::Justification::centred);
        }
    }

    if (in (m_guide))
        paintGuide (g);

    // (Dimmed headings for controls that do nothing now.)
    const auto heading = m_controlsLive ? orange : orange.withAlpha (0.4f);
    if (in (m_targetCell))
        m_lookAndFeel.drawPanel (g, m_targetCell.toFloat(), "TARGET", heading, 32.0f);
    if (in (m_noteCell))
        m_lookAndFeel.drawPanel (g, m_noteCell.toFloat(), "NOTE", heading, 32.0f);
    if (in (m_amountCell))
        m_lookAndFeel.drawPanel (g, m_amountCell.toFloat(), "AMOUNT", heading, 32.0f);
    if (in (m_sourceCell))
        m_lookAndFeel.drawPanel (g, m_sourceCell.toFloat(), "SOURCE", heading, 32.0f);
    if (in (m_lengthCell))
        m_lookAndFeel.drawPanel (g, m_lengthCell.toFloat(), "LENGTH", heading, 32.0f);
    if (in (m_characterCell))
        m_lookAndFeel.drawPanel (g, m_characterCell.toFloat(), "CHARACTER",
                                 m_characters[0]->isEnabled() ? orange : orange.withAlpha (0.4f), 32.0f);
}

juce::String PitchFlattenerAudioProcessorEditor::getTooltip()
{
    if (m_flatText.startsWith ("SEVERAL") && m_flatRow.contains (getMouseXYRelative()))
        return "Each separate sound in the recording is flattened to its own pitch.";
    return {};
}

//==============================================================================
// The curve editor's undo, redo and reset, and its tools' state.

void PitchFlattenerAudioProcessorEditor::applyEdits (int line, PitchDisplay::CurvePoints points)
{
    m_lastLivePush = juce::Time::getMillisecondCounterHiRes() * 0.001;
    m_processor.setEdits (line, std::move (points));
    refreshNow();
}

void PitchFlattenerAudioProcessorEditor::undoEdit()
{
    if (m_undoSteps.empty())
        return;
    auto step = std::move (m_undoSteps.back());
    m_undoSteps.pop_back();
    m_redoSteps.push_back ({ step.line, m_processor.getEdits (step.line) });
    applyEdits (step.line, std::move (step.points));
}

void PitchFlattenerAudioProcessorEditor::redoEdit()
{
    if (m_redoSteps.empty())
        return;
    auto step = std::move (m_redoSteps.back());
    m_redoSteps.pop_back();
    m_undoSteps.push_back ({ step.line, m_processor.getEdits (step.line) });
    applyEdits (step.line, std::move (step.points));
}

void PitchFlattenerAudioProcessorEditor::resetCurve()
{
    const int line = m_processor.lineInUse();
    auto before = m_processor.getEdits (line);
    if (before.empty())
        return;
    m_undoSteps.push_back ({ line, std::move (before) });
    m_redoSteps.clear();
    applyEdits (line, {});
}

void PitchFlattenerAudioProcessorEditor::updateTools()
{
    const auto result = m_processor.getEngine().result();
    const bool scope = shownView() == Processor::kViewScope && result != nullptr && result->learned != nullptr;
    const bool editable = scope && result->playable() && m_processor.lineInUse() >= 0;
    const bool edited = editable && (! m_processor.getEdits (m_processor.lineInUse()).empty() || ! m_undoSteps.empty() || ! m_redoSteps.empty());
    const auto show = [] (juce::Component& c, bool visible)
    {
        if (c.isVisible() != visible)
            c.setVisible (visible);
    };
    show (m_drawTool, editable);
    show (m_snapTool, editable);
    show (m_undo, edited);
    show (m_redo, edited);
    show (m_resetCurve, edited);
    show (m_zoomOut, scope);
    show (m_zoomIn, scope);
    show (m_fit, scope);
    m_undo.setEnabled (! m_undoSteps.empty());
    m_redo.setEnabled (! m_redoSteps.empty());
    m_resetCurve.setEnabled (! m_processor.getEdits (m_processor.lineInUse()).empty());
    m_drawTool.setToggleState (m_display.drawMode(), juce::dontSendNotification);
    m_snapTool.setToggleState (m_display.snap(), juce::dontSendNotification);
    m_fit.setEnabled (m_display.isZoomed());
    m_zoomIn.setEnabled (scope && m_display.viewTo() - m_display.viewFrom() > PitchDisplay::kMinSpanSeconds + 1.0e-6);
    m_zoomOut.setEnabled (scope && m_display.isZoomed());
}

//==============================================================================
void PitchFlattenerAudioProcessorEditor::showUpdatePopup (const sammyj::UpdateInfo& info)
{
    if (m_updatePopup != nullptr)
        return;

    m_updatePopup = std::make_unique<UpdatePopup> (info, [safeThis = juce::Component::SafePointer<PitchFlattenerAudioProcessorEditor> (this)]
    {
        // Deferred: this runs inside one of the popup's own button callbacks.
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr)
                safeThis->m_updatePopup.reset();
        });
    });

    addAndMakeVisible (*m_updatePopup);
    m_updatePopup->toFront (false);
    m_updatePopup->setEditorScale (1.0f);
    resized();

    if (isShowing())
        m_updatePopup->grabKeyboardFocus();
}

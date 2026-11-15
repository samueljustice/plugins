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
    const char* const c_lineLabels[] = { "AUTO", "HARMONIC", "RIDGE" };
    const char* const c_lineTip =
        "Line: which reading of the sound's pitch it is flattened by. Auto takes the one that flattens it better. "
        "Harmonic suits voices, engines and whines; Ridge suits sirens, whistles and higher sounds. "
        "Clicking a line in the display picks it too.";

    juce::String seconds (double s) { return juce::String (s, 2) + " S"; }
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
    m_learn.setTooltip ("Learn: press, then play the sound once through the plug-in. It records the sound (letting it through "
                        "unchanged), stops after a second of silence, and flattens it. Press again while it listens to stop there. "
                        "Afterwards, whenever that sound plays through the plug-in again, from the start or from any point in it, "
                        "the flattened sound plays in its place.");
    addAndMakeVisible (m_learn);

    m_clear.onClick = [this] { m_processor.getEngine().clear(); refreshNow(); };
    m_clear.setTooltip ("Clear: forgets the learned sound. Audio then passes through unchanged until another sound is learned.");
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
               "Amount: how much of the pitch movement is taken out. 100% leaves the sound flat; 50% leaves half of its movement in.",
               m_amountAttachment);

    for (size_t i = 0; i < m_lines.size(); ++i)
    {
        auto& b = m_lines[i];
        b = std::make_unique<HudButton> (c_lineLabels[i], HudButton::Style::Segment);
        b->setTooltip (c_lineTip);
        b->onClick = [this, i]
        {
            m_processor.setParameterFromEditor (Processor::lineId, (float) i);
            m_processor.syncOptions();
            refreshNow();
        };
        addAndMakeVisible (*b);
    }

    m_scopeTab.setTooltip ("Scope: the learned sound as an exact graph of its pitch over time, the target and the play head.");
    m_flightTab.setTooltip ("Flight: the learned sound flown as a course, its pitch as a track that rises and falls beside the "
                            "level lane of the flattened sound.");
    m_scopeTab.onClick = [this] { chooseView (Processor::kViewScope); };
    m_flightTab.onClick = [this] { chooseView (Processor::kViewFlight); };
    addAndMakeVisible (m_scopeTab);
    addAndMakeVisible (m_flightTab);

    m_display.onLineClicked = [this] (int line)
    {
        m_processor.setParameterFromEditor (Processor::lineId, (float) (line + 1));
        m_processor.syncOptions();
        refreshNow();
    };
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

void PitchFlattenerAudioProcessorEditor::refreshNow()
{
    using namespace Theme::Colours;
    auto& engine = m_processor.getEngine();
    const auto state = engine.state();
    const auto result = engine.result();
    const bool playing = engine.playPosition() >= 0 && result != nullptr && engine.playGeneration() == result->generation;
    const double now = m_clock.now();

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
            m_learnText = "LEARNED";
            learnHint = "PRESS TO LEARN ANOTHER";
            status = "NO STEADY PITCH FOUND: THE SOUND IS LEFT AS IT IS";
            statusColour = magenta;
            break;
        case State::Ready:
            m_learnText = "LEARNED";
            learnHint = "PRESS TO LEARN ANOTHER";
            if (engine.sampleRateMismatch() && result != nullptr)
            {
                status = "LEARNED AT " + juce::String (juce::roundToInt (result->learned->sampleRate)) + " HZ: THE SESSION IS AT "
                       + juce::String (juce::roundToInt (engine.sampleRate())) + " HZ";
                statusColour = magenta;
            }
            else if (playing)
            {
                status = "PLAYING THE FLATTENED SOUND";
                statusColour = lime;
            }
            else
            {
                status = "LEARNED: PLAY THE SOUND AGAIN TO HEAR IT FLATTENED";
                statusColour = lime;
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
        flatText = PitchFlattenerAudioProcessor::noteText (result->flattened.targetMidi);
    }
    else if (result != nullptr)
        before = seconds ((double) result->learned->locator.length() / result->learned->sampleRate);

    const bool armed = state == State::Armed || state == State::Listening;
    m_clear.setEnabled (result != nullptr || armed || state == State::Analysing);

    const int chosen = juce::roundToInt (m_processor.parameters.getRawParameterValue (Processor::targetId)->load());
    for (size_t i = 0; i < m_targets.size(); ++i)
        if (m_targets[i]->getToggleState() != ((int) i == chosen))
            m_targets[i]->setToggleState ((int) i == chosen, juce::dontSendNotification);
    m_note.setEnabled (chosen == 4);

    const int line = juce::roundToInt (m_processor.parameters.getRawParameterValue (Processor::lineId)->load());
    for (size_t i = 0; i < m_lines.size(); ++i)
        if (m_lines[i]->getToggleState() != ((int) i == line))
            m_lines[i]->setToggleState ((int) i == line, juce::dontSendNotification);

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
    displayView.playPosition = playing ? engine.playPosition() : -1;
    displayView.playGeneration = engine.playGeneration();
    displayView.rendering = engine.isRendering();
    displayView.autoLine = line == 0;
    m_display.setView (displayView);

    if (view == Processor::kViewFlight)
    {
        double playSeconds = -1.0;
        if (playing && result->playable())
            playSeconds = result->flattened.sourceSampleFor ((double) engine.playPosition()) / result->learned->sampleRate;
        m_flight.setView (result, playSeconds);
        m_flight.renderFrame();
    }

    // The banners.
    BannerLogic::Input in;
    in.state = state;
    in.playing = engine.playPosition() >= 0 && result != nullptr;
    in.rateMismatch = engine.sampleRateMismatch();
    in.learnedId = result != nullptr && result->learned != nullptr ? result->learned->id : 0;
    m_bannerLogic.update (in, now);
    m_banner.show (m_bannerLogic.kind(), m_bannerLogic.since(), engine.captureSeconds());
    if (m_bannerLogic.flatTag() != m_flatTagShown)
    {
        m_flatTagShown = m_bannerLogic.flatTag();
        repaint (m_displayPanel.withHeight (30));
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
        m_flightTab.setBounds (tabs.removeFromRight (80));
        tabs.removeFromRight (6);
        m_scopeTab.setBounds (tabs.removeFromRight (76));
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
    m_targetCell = controls.removeFromLeft (380);
    controls.removeFromLeft (c_gap);
    m_noteCell = controls.removeFromLeft (140);
    controls.removeFromLeft (c_gap);
    m_amountCell = controls.removeFromLeft (140);
    controls.removeFromLeft (c_gap);
    m_lineCell = controls;

    {
        auto cell = m_targetCell.withTrimmedTop (40).reduced (14, 0);
        auto top = cell.removeFromTop (38);
        cell.removeFromTop (8);
        auto bottom = cell.removeFromTop (38);
        const int w = (top.getWidth() - 2 * 8) / 3;
        for (int i = 0; i < 3; ++i)
        {
            m_targets[(size_t) i]->setBounds (top.removeFromLeft (w));
            top.removeFromLeft (8);
        }
        const int w2 = (bottom.getWidth() - 8) / 2;
        m_targets[3]->setBounds (bottom.removeFromLeft (w2));
        bottom.removeFromLeft (8);
        m_targets[4]->setBounds (bottom);
    }
    m_note.setBounds (m_noteCell.withTrimmedTop (34).reduced (14, 6));
    m_amount.setBounds (m_amountCell.withTrimmedTop (34).reduced (14, 6));
    {
        auto row = m_lineCell.withTrimmedTop (40).reduced (14, 0).withHeight (38);
        const int w = (row.getWidth() - 2 * 6) / 3;
        for (size_t i = 0; i < m_lines.size(); ++i)
        {
            m_lines[i]->setBounds (i + 1 < m_lines.size() ? row.removeFromLeft (w) : row);
            row.removeFromLeft (6);
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
    readout ("DELAY", juce::String (pitchflattener::Engine::kLatencySeconds, 2) + " S", text.withAlpha (0.85f));
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

    // Length: as heard (cyan), and flattened (lime).
    auto top = row (inner.removeFromTop (inner.getHeight() * 0.5f), "LENGTH");
    if (m_lengthBefore.isEmpty())
        Hud::drawSheared (g, "--", top.withWidth (60.0f), valueFont, textDim);
    else
    {
        const float w1 = Hud::shearedWidth (valueFont, m_lengthBefore) + 2.0f;
        Hud::drawSheared (g, m_lengthBefore, top.removeFromLeft (w1), valueFont, cyan);
        if (m_lengthAfter.isNotEmpty())
        {
            top.removeFromLeft (8.0f);
            Hud::chevron (g, { top.getX() + 6.0f, top.getCentreY() }, 5.0f, 0.0f, 2.0f, textDim);
            Hud::chevron (g, { top.getX() + 13.0f, top.getCentreY() }, 5.0f, 0.0f, 2.0f, textDim);
            top.removeFromLeft (26.0f);
            Hud::drawSheared (g, m_lengthAfter, top.removeFromLeft (Hud::shearedWidth (valueFont, m_lengthAfter) + 2.0f), valueFont, lime);
        }
    }

    // The pitch it is flattened to: the user's target, orange.
    auto bottom = row (inner, "FLAT ON");
    Hud::drawSheared (g, m_flatText.isEmpty() ? juce::String ("--") : m_flatText,
                      bottom.withWidth (std::min (bottom.getWidth(), Hud::shearedWidth (valueFont, m_flatText.isEmpty() ? juce::String ("--") : m_flatText) + 2.0f)),
                      valueFont, m_flatText.isEmpty() ? textDim : orange);
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
        Hud::heading (g, juce::Rectangle<float> (r.getX() + 16.0f, r.getY() + 6.0f, 420.0f, 18.0f),
                      scope ? "SCOPE  /  PITCH OVER TIME" : "FLIGHT  /  THE SOUND AS A COURSE", m_fonts->display (12.0f), orange);

        // FLAT: the flattened sound is playing (a tag at the display's corner, beside the tabs).
        if (m_flatTagShown)
        {
            const auto font = m_fonts->display (12.0f);
            const juce::String word = "FLAT";
            const float w = Hud::shearedWidth (font, word) + 30.0f;
            const auto tag = juce::Rectangle<float> ((float) m_scopeTab.getX() - 14.0f - w, (float) m_scopeTab.getY(), w, (float) m_scopeTab.getHeight());
            Hud::frame (g, tag, 5.0f, lime, juce::Colours::transparentBlack);
            g.setColour (space);
            g.fillRect (juce::Rectangle<float> (tag.getX() + 8.0f, tag.getCentreY() - 3.0f, 6.0f, 6.0f));
            Hud::drawSheared (g, word, tag.withTrimmedLeft (18.0f).withTrimmedRight (5.0f), font, space, juce::Justification::centred);
        }
    }

    if (in (m_guide))
        paintGuide (g);

    if (in (m_targetCell))
        m_lookAndFeel.drawPanel (g, m_targetCell.toFloat(), "TARGET", orange, 32.0f);
    if (in (m_noteCell))
        m_lookAndFeel.drawPanel (g, m_noteCell.toFloat(), "NOTE", orange, 32.0f);
    if (in (m_amountCell))
        m_lookAndFeel.drawPanel (g, m_amountCell.toFloat(), "AMOUNT", orange, 32.0f);
    if (in (m_lineCell))
        m_lookAndFeel.drawPanel (g, m_lineCell.toFloat(), "LINE", orange, 32.0f);

    const auto caption = m_lineCell.toFloat().withTrimmedTop (90.0f).reduced (14.0f, 0.0f);
    const auto small = m_fonts->body (11.0f);
    Hud::drawText (g, "THE PITCH READING", caption.withHeight (15.0f), small, textDim);
    Hud::drawText (g, "IT IS FLATTENED BY", caption.withTrimmedTop (16.0f).withHeight (15.0f), small, textDim);
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

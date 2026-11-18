#include "PluginEditor.h"

#include "Parameters.h"

using namespace dehonk;

//==============================================================================
ListenButton::ListenButton (const juce::String& name, juce::Colour c, juce::Colour whenActive) : juce::Button (name), colour (c), activeColour (whenActive)
{
    setButtonText (name);
}

void ListenButton::setPulse (float amount)
{
    if (! juce::approximatelyEqual (pulse, amount))
    {
        pulse = amount;
        repaint();
    }
}

void ListenButton::paintButton (juce::Graphics& g, bool, bool isButtonDown)
{
    // A flat disc. On, it turns to ink with pale lettering, and a dashed ring turns slowly round it.
    const bool activeNow = isActive();
    const auto bounds = getLocalBounds().toFloat();
    const float d = juce::jmin (bounds.getWidth(), bounds.getHeight()) - 14.0f;
    const auto disc = bounds.withSizeKeepingCentre (d, d);
    const auto fill = activeNow ? toy::ink : (isButtonDown ? colour.darker (0.08f) : colour);

    g.setColour (fill);
    g.fillEllipse (disc);
    g.setColour (toy::ink);
    g.drawEllipse (disc, toy::outline);

    if (activeNow)
    {
        juce::Path ring;
        ring.addEllipse (disc.expanded (5.0f));
        juce::Path dashed;
        const float dashes[] = { 3.0f, 5.0f };
        juce::PathStrokeType (1.5f).createDashedStroke (dashed, ring, dashes, 2,
                                                        juce::AffineTransform::rotation (pulse * 0.5f, disc.getCentreX(), disc.getCentreY()));
        g.setColour (activeColour);
        g.fillPath (dashed);
    }

    if (! activeNow)
        if (auto* look = dynamic_cast<DeHonkLookAndFeel*> (&getLookAndFeel()))
            look->drawFocus (g, *this, disc, d * 0.5f);

    g.setColour (activeNow ? toy::paper : toy::ink);
    g.setFont (fonts->name (juce::jlimit (10.5f, 13.0f, d * 0.12f)));
    g.drawFittedText (getButtonText().toLowerCase(), disc.toNearestInt().reduced (10, 4), juce::Justification::centred, 2, 0.85f);
}

//==============================================================================
DiffButton::DiffButton() : ListenButton ("Diff", toy::mint, toy::mint) {}

void DiffButton::setActive (bool shouldBeOn)
{
    if (on == shouldBeOn)
        return;

    on = shouldBeOn;
    repaint();

    if (onActiveChange != nullptr)
        onActiveChange (on);
}

void DiffButton::release()
{
    on = latched = releasingLatch = false;
    repaint();
}

void DiffButton::latch()
{
    on = latched = true;
    releasingLatch = false;
    repaint();
}

void DiffButton::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isLeftButtonDown())
        return;

    dragged = false;
    pressTime = juce::Time::getMillisecondCounter();

    if (latched)
    {
        // A click on a latched Diff releases it.
        latched = false;
        releasingLatch = true;
        setActive (false);
        return;
    }

    releasingLatch = false;
    setActive (true);
}

void DiffButton::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > 4)
        dragged = true;
}

void DiffButton::mouseUp (const juce::MouseEvent&)
{
    if (releasingLatch)
    {
        releasingLatch = false;
        return;
    }

    if (! on)
        return;

    const bool wasClick = ! dragged && juce::Time::getMillisecondCounter() - pressTime <= clickMs;
    if (wasClick)
        latched = true;  // stays on until the next click
    else
        setActive (false);
}

void DiffButton::clicked()
{
    // Keyboard and accessibility: toggle the latch.
    latched = ! on;
    setActive (! on);
}

//==============================================================================
DetectionSelector::DetectionSelector (juce::RangedAudioParameter& detection, const Theme::Fonts& f)
    : fonts (f),
      attachment (detection, [this] (float v)
                  {
                      const int now = juce::jlimit (0, ParamRanges::numDetectionModes - 1, juce::roundToInt (v));
                      if (now != mode)
                      {
                          mode = now;
                          repaint();
                      }
                  })
{
    setTitle ("Detection");
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    attachment.sendInitialUpdate();
}

int DetectionSelector::segmentAt (juce::Point<float> at) const noexcept
{
    const auto r = Toy::body (getLocalBounds());
    if (! r.contains (at))
        return -1;
    return juce::jlimit (0, ParamRanges::numDetectionModes - 1,
                         (int) ((at.x - r.getX()) / r.getWidth() * (float) ParamRanges::numDetectionModes));
}

void DetectionSelector::choose (int newMode)
{
    if (newMode != mode)
        attachment.setValueAsCompleteGesture ((float) newMode);
}

void DetectionSelector::paint (juce::Graphics& g)
{
    const auto r = Toy::body (getLocalBounds());
    const float corner = r.getHeight() * 0.5f;
    constexpr int n = ParamRanges::numDetectionModes;
    const float w = r.getWidth() / (float) n;

    g.setColour (toy::paper);
    g.fillRoundedRectangle (r, corner);
    {
        juce::Path clip;
        clip.addRoundedRectangle (r, corner);
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (clip);
        for (int s = 0; s < n; ++s)
            if (s == mode || s == hotSegment)
            {
                g.setColour (s == mode ? toy::sunYellow : toy::sunYellow.withAlpha (0.28f));
                g.fillRect (juce::Rectangle<float> (r.getX() + w * (float) s, r.getY(), w, r.getHeight()));
            }
    }
    g.setColour (toy::ink);
    for (int s = 1; s < n; ++s)
        g.fillRect (juce::Rectangle<float> (r.getX() + w * (float) s - toy::outline * 0.5f, r.getY(), toy::outline, r.getHeight()));
    g.drawRoundedRectangle (r, corner, toy::outline);

    g.setFont (fonts.value (juce::jlimit (9.5f, 12.0f, w / 6.4f)));
    for (int s = 0; s < n; ++s)
    {
        const auto seg = juce::Rectangle<float> (r.getX() + w * (float) s, r.getY(), w, r.getHeight());
        g.setColour (toy::ink.withAlpha (s == mode ? 1.0f : 0.7f));
        g.drawFittedText (juce::String (ParamRanges::detectionNames[s]).toLowerCase(),
                          seg.toNearestInt().reduced (s == 0 || s == n - 1 ? 5 : 3, 0), juce::Justification::centred, 1, 0.8f);
    }

    if (auto* look = dynamic_cast<DeHonkLookAndFeel*> (&getLookAndFeel()))
        look->drawFocus (g, *this, r, 100.0f);
}

void DetectionSelector::mouseMove (const juce::MouseEvent& e)
{
    const int now = segmentAt (e.position);
    if (now != hotSegment)
    {
        hotSegment = now;
        repaint();
    }
}

void DetectionSelector::mouseExit (const juce::MouseEvent&)
{
    if (hotSegment >= 0)
    {
        hotSegment = -1;
        repaint();
    }
}

void DetectionSelector::mouseUp (const juce::MouseEvent& e)
{
    const int segment = segmentAt (e.position);
    if (segment >= 0 && ! e.mods.isPopupMenu())
        choose (segment);
}

bool DetectionSelector::keyPressed (const juce::KeyPress& key)
{
    const int step = key == juce::KeyPress::leftKey || key == juce::KeyPress::downKey ? -1
                   : key == juce::KeyPress::rightKey || key == juce::KeyPress::upKey ? 1 : 0;
    if (step == 0)
        return false;
    choose (juce::jlimit (0, ParamRanges::numDetectionModes - 1, mode + step));
    return true;
}

//==============================================================================
DeHonkAudioProcessorEditor::DeHonkAudioProcessorEditor (DeHonkAudioProcessor& p)
    : AudioProcessorEditor (&p), deHonk (p), display (p), shelf (p.getParameters(), *fonts),
      detectionSelector (*p.getParameters().getParameter (ParamIDs::detection), *fonts),
      linkSelector (*p.getParameters().getParameter (ParamIDs::linkMode), *fonts)
{
    setLookAndFeel (&lookAndFeel);
    auto& apvts = deHonk.getParameters();

    addAndMakeVisible (display);

    // Header: name, presets, bypass
    title.onAbout = [this] { showAbout(); };
    title.onSite = [] { juce::URL (TitleBlock::siteUrl).launchInDefaultBrowser(); };
    addAndMakeVisible (title);

    previousButton.setTooltip ("Previous preset");
    nextButton.setTooltip ("Next preset");
    presetButton.setTooltip ("The current preset. Click for the list. A * means the settings have changed since it was loaded or saved.");
    saveButton.setTooltip ("Save the current settings as a user preset");
    deleteButton.setTooltip ("Delete the current user preset. Factory presets cannot be deleted.");
    previousButton.onClick = [this] { deHonk.getPresetManager().loadPrevious(); updatePresetBar(); };
    nextButton.onClick = [this] { deHonk.getPresetManager().loadNext(); updatePresetBar(); };
    presetButton.onClick = [this] { showPresetMenu(); };
    saveButton.onClick = [this] { savePreset(); };
    deleteButton.onClick = [this] { deletePreset(); };
    for (auto* b : { &previousButton, &nextButton, &presetButton, &saveButton, &deleteButton })
        addAndMakeVisible (b);

    bypassButton.setClickingTogglesState (true);
    bypassButton.setColour (juce::TextButton::buttonOnColourId, toy::jacketRed);
    presetButton.getProperties().set ("keepCase", true);
    bypassButton.setTooltip ("Passes the signal through untreated, with the same latency and without a click.");
    bypassAttachment = std::make_unique<ButtonAttachment> (apvts, ParamIDs::bypass, bypassButton);
    addAndMakeVisible (bypassButton);

    // Main knobs
    setUpKnob (thresholdKnob, thresholdCaption, "Threshold", ParamIDs::threshold,
               "De-Honk acts only while the mix is louder than this. It is fully off below it and fully on 6 dB above. "
               "At Always (the bottom of the range) it acts at any level. It can also be dragged in the band view: "
               "the grip on the level bar at the left, or its line across the bands.");
    setUpKnob (depthKnob, depthCaption, "Depth", ParamIDs::depth,
               "How much of the detected excess is removed. No cut goes deeper than Max Cut. At 0 nothing is cut.");
    setUpKnob (maxCutKnob, maxCutCaption, "Max Cut", ParamIDs::maxCut,
               "The deepest any one cut may go, whatever Depth is set to. Two cuts side by side never add up to more. "
               "It is the dashed line across the top of the band view, which can be dragged too.");
    setUpKnob (sharpnessKnob, sharpnessCaption, "Sharpness", ParamIDs::sharpness,
               "How wide a build-up has to be to count. Broad responds to a whole bump; Narrow responds only to "
               "single ringing resonances. The cuts themselves are narrow either way.");
    setUpKnob (speedKnob, speedCaption, "Speed", ParamIDs::speed,
               "How quickly a cut goes in once a build-up has been found. Release sets how it lets go. "
               "A range whose timing is unlocked uses its own attack.");
    setUpKnob (releaseKnob, releaseCaption, "Release", ParamIDs::release,
               "How slowly a cut lets go once its build-up has gone. Longer holds each cut steadier; shorter follows the mix more closely. "
               "It is for cuts that have settled on a clear build-up: a light cut that comes and goes, or one whose build-up has moved to the next note, "
               "lets go quickly whatever is set here. Under the Threshold every cut closes straight away. "
               "A range whose timing is unlocked uses its own release.");
    setUpCaption (detectionCaption, "Detection");
    const juce::String detectionTip ("Fast cuts a build-up as soon as it shows. Steady waits until a build-up has held for a while, "
                                     "and leaves a broad lift alone: steadier, and kinder to dense, noisy textures, "
                                     "but slower to catch a new build-up, and it may leave a mild one alone in a dense mix.");
    detectionSelector.setTooltip (detectionTip);
    detectionCaption.setTooltip (detectionTip);
    addAndMakeVisible (detectionSelector);
    thresholdKnob.getProperties().set ("toyColour", (juce::int64) toy::orange.getARGB());
    depthKnob.getProperties().set ("toyColour", (juce::int64) toy::sunYellow.getARGB());
    sharpnessKnob.getProperties().set ("toyColour", (juce::int64) toy::mint.getARGB());
    speedKnob.getProperties().set ("toyColour", (juce::int64) toy::lavender.getARGB());
    maxCutKnob.getProperties().set ("toyColour", (juce::int64) toy::pink.getARGB());
    releaseKnob.getProperties().set ("toyColour", (juce::int64) toy::click.getARGB());
    thresholdAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::threshold, thresholdKnob);
    depthAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::depth, depthKnob);
    sharpnessAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::sharpness, sharpnessKnob);
    speedAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::speed, speedKnob);
    maxCutAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::maxCut, maxCutKnob);
    releaseAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::release, releaseKnob);
    thresholdKnob.setComponentID ("threshold");

    // The band view: fed from the display's clock, and where the edges, the Threshold and Max Cut
    // are dragged. Each drag is one gesture on its parameter.
    addAndMakeVisible (bandView);
    display.onTick = [this] (const dehonk::scene::BandViewState& state, double seconds) { bandView.update (state, seconds); };
    // The view is told of new values with every picture of the pond, so the two move together.
    bandView.onHandleDragStarted = [this] (int h)
    {
        if (auto* param = handleParam (h))
            param->beginChangeGesture();
    };
    bandView.onHandleDragEnded = [this] (int h)
    {
        if (auto* param = handleParam (h))
            param->endChangeGesture();
    };
    bandView.onHandleReset = [this] (int h)
    {
        if (auto* param = handleParam (h))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->getDefaultValue());
            param->endChangeGesture();
        }
    };
    bandView.onHandleDragged = [this] (int h, float value)
    {
        auto* param = handleParam (h);
        if (param == nullptr)
            return;
        if (h < dehonk::kNumEdges)
        {
            // Inside its own range, and never within a tenth of its neighbours.
            const auto used = display.edgesAsUsed();
            float lo = dehonk::kEdgeMinHz[h], hi = dehonk::kEdgeMaxHz[h];
            if (h > 0)                       lo = juce::jmax (lo, used[(size_t) h - 1] * ParamRanges::minEdgeRatio);
            if (h + 1 < dehonk::kNumEdges)   hi = juce::jmin (hi, used[(size_t) h + 1] / ParamRanges::minEdgeRatio);
            if (hi <= lo)
                return;
            value = juce::jlimit (lo, hi, value);
        }
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    };

    // The shelf of range tabs. Selecting one puts the pond and the band view on that range alone.
    addAndMakeVisible (shelf);
    shelf.onSelectionChanged = [this] (int range)
    {
        display.setFocusModule (range);
        bandView.setFocus (range);
    };

    display.onFrame = [this] (const dehonk::DisplayFrame& frame) { showFrame (frame); };

    // Listening tools
    soloButton.setClickingTogglesState (true);
    soloButton.setTooltip ("Plays only what is being removed. If it sounds only ugly, the settings are right. "
                           "If you hear wanted bass or body in it, lower Depth or switch off the lowest range. Not saved with the session.");
    soloButton.onClick = [this] { setSoloHonk (soloButton.getToggleState()); };
    addAndMakeVisible (soloButton);

    diffButton.setTooltip ("Hold to hear the untreated signal, latency-matched. Click to keep it on; click again to return. "
                           "The host's bypass is not touched. Not saved with the session.");
    diffButton.onActiveChange = [this] (bool on) { setDiff (on); };
    addAndMakeVisible (diffButton);

    setUpBar (mixBar, mixCaption, "Mix", ParamIDs::mix,
              "Balance between the untreated signal (0%) and the treated signal (100%).");
    setUpBar (trimBar, trimCaption, "Trim", ParamIDs::trim, "Output level, after the mix.");
    mixAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::mix, mixBar);
    trimAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::trim, trimBar);

    // The bottom row: Impact Guard, Link, Lookahead, Dialogue.
    const juce::String guardTip ("Keeps the cuts out of big hits (an explosion, a slam): no new cut while one sounds, and open cuts eased off by up to this amount. "
                                 "The square lamp switches it off and on, and fills with yellow while the guard is up.");
    setUpBar (guardBar, guardCaption, "Impact Guard", ParamIDs::impactGuard, guardTip);
    guardAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::impactGuard, guardBar);
    guardSwitch.setTooltip (guardTip);
    guardSwitch.setClickingTogglesState (false);     // it follows the amount: see toggleGuard()
    guardSwitch.onClick = [this] { toggleGuard(); };
    addAndMakeVisible (guardSwitch);

    setUpCaption (linkCaption, "Link");
    const juce::String linkTip ("Linked: every channel gets the same cuts, so the image does not move. Grouped (surround only): the front pair, the centre, "
                                "the surrounds and the heights each share theirs. Unlinked: each channel on its own.");
    linkSelector.setTooltip (linkTip);
    linkCaption.setTooltip (linkTip);
    addAndMakeVisible (linkSelector);

    setUpCaption (lookaheadCaption, "Lookahead");
    const juce::String lookaheadTip ("Delays the audio so each cut is in place when a transient arrives. "
                                     "Adds the chosen latency, which is reported to the host.");
    lookaheadBox.addItemList (juce::StringArray { "Off", "5 ms", "10 ms", "20 ms" }, 1);
    lookaheadBox.setTooltip (lookaheadTip);
    lookaheadCaption.setTooltip (lookaheadTip);
    lookaheadAttachment = std::make_unique<ComboBoxAttachment> (apvts, ParamIDs::lookahead, lookaheadBox);
    addAndMakeVisible (lookaheadBox);

    const juce::String dialogueTip ("With the dialogue stem on the sidechain (key input) and this switched on, bands and clicks that are the dialogue's are left alone. "
                                    "The bar sets how far. The round lamp is lit while the stem arrives, and turns red if the stem is not found in the mix or is most of it.");
    setUpBar (dialogueBar, dialogueCaption, "Dialogue", ParamIDs::dialogueAmount, dialogueTip);
    dialogueBar.getProperties().set ("lamp", 0.0);
    dialogueBar.getProperties().set ("toyColour", (juce::int64) toy::lavender.getARGB());
    dialogueAmountAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::dialogueAmount, dialogueBar);
    dialogueSwitch.setTooltip (dialogueTip);
    dialogueAttachment = std::make_unique<ButtonAttachment> (apvts, ParamIDs::dialogue, dialogueSwitch);
    addAndMakeVisible (dialogueSwitch);

    setResizable (true, true);
    setResizeLimits (minWidth, minHeight, 1800, 1400);
    setSize (defaultWidth, defaultHeight);

    // Adopt the processor's listening state (normally both off: the editor clears them on close).
    soloButton.setToggleState (deHonk.isSoloHonk(), juce::dontSendNotification);
    if (deHonk.isDiff())
        diffButton.latch();

    // The controls were made before the look was set on this window: have them take it up now.
    sendLookAndFeelChange();

    // The focus ring shows only while the keyboard is being used to get about.
    addMouseListener (this, true);

    updatePresetBar();
    timerCallback();
    startTimerHz (30);
}

DeHonkAudioProcessorEditor::~DeHonkAudioProcessorEditor()
{
    stopTimer();
    updateCheck.reset();        // cancels a request in flight: its callback never runs after this
    display.setActive (false);
    if (showing)
        deHonk.setEditorOpen (false);

    // Solo Honk and Diff are only heard with their pressed buttons and the band view's label in view.
    deHonk.setSoloHonk (false);
    deHonk.setDiff (false);

    juce::PopupMenu::dismissAllActiveMenus();
    overlay.reset();
    setLookAndFeel (nullptr);
}

void DeHonkAudioProcessorEditor::setUpCaption (juce::Label& label, const juce::String& text)
{
    label.setText (text.toLowerCase(), juce::dontSendNotification);
    label.setFont (fonts->name (11.0f));
    label.setColour (juce::Label::textColourId, toy::ink);
    label.setJustificationType (juce::Justification::centred);
    label.setMinimumHorizontalScale (0.8f);
    addAndMakeVisible (label);
}

void DeHonkAudioProcessorEditor::setUpKnob (juce::Slider& knob, juce::Label& caption, const juce::String& text,
                                            const char* paramID, const juce::String& tip)
{
    setUpCaption (caption, text);

    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 104, 24);
    knob.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    knob.setTooltip (tip);
    caption.setTooltip (tip);

    if (auto* param = deHonk.getParameters().getParameter (paramID))
        knob.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));

    addAndMakeVisible (knob);
}

void DeHonkAudioProcessorEditor::setUpBar (juce::Slider& bar, juce::Label& caption, const juce::String& text,
                                           const char* paramID, const juce::String& tip)
{
    setUpCaption (caption, text);

    bar.setSliderStyle (juce::Slider::LinearBar);
    bar.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 20);
    if (juce::String (paramID) == ParamIDs::trim)
        bar.getProperties().set ("fromCentre", true);
    bar.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    bar.setTooltip (tip);
    caption.setTooltip (tip);

    if (auto* param = deHonk.getParameters().getParameter (paramID))
        bar.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));

    addAndMakeVisible (bar);
}

//==============================================================================
void DeHonkAudioProcessorEditor::setSoloHonk (bool on)
{
    deHonk.setSoloHonk (on);

    // One listening tool at a time, so the label always says what is heard.
    if (on && diffButton.active())
    {
        diffButton.release();
        deHonk.setDiff (false);
    }
}

void DeHonkAudioProcessorEditor::setDiff (bool on)
{
    deHonk.setDiff (on);

    if (on && soloButton.getToggleState())
    {
        soloButton.setToggleState (false, juce::dontSendNotification);
        deHonk.setSoloHonk (false);
    }
}

void DeHonkAudioProcessorEditor::toggleGuard()
{
    // Off is the amount at 0. Switching it on again brings back the amount it had.
    auto* param = deHonk.getParameters().getParameter (ParamIDs::impactGuard);
    if (param == nullptr)
        return;
    const float now = param->convertFrom0to1 (param->getValue());
    float next = 0.0f;
    if (now > 0.5f)
        deHonk.setGuardAmountMemory (now);
    else
        next = juce::jmax (1.0f, deHonk.getGuardAmountMemory());
    param->beginChangeGesture();
    param->setValueNotifyingHost (param->convertTo0to1 (next));
    param->endChangeGesture();
}

void DeHonkAudioProcessorEditor::showFrame (const dehonk::DisplayFrame& frame)
{
    // The tabs' lamps: a range is cutting from half a decibel, and has stopped under a quarter.
    // Snap's cuts are over in milliseconds, so its lamp stays lit for a moment after each.
    auto now = cutting;
    for (int m = 0; m < dehonk::kNumModules; ++m)
        now[(size_t) m] = frame.moduleCutDb[m] > (cutting[(size_t) m] ? 0.25f : 0.5f);
    const double ms = juce::Time::getMillisecondCounterHiRes();
    if (frame.snapCutDb > 0.5f)
        snapLitUntilMs = ms + 300.0;
    now[dehonk::kNumModules] = ms < snapLitUntilMs;
    if (now != cutting)
    {
        cutting = now;
        shelf.setCutting (cutting);
    }

    guardSwitch.setGlow (frame.guard);

    // The dialogue lamp: from -60 dBFS (just lit) to -20 (full), in five steps.
    // It turns to a warning (red) while Dialogue is listening and the key is not doing what it is
    // there for: it is not found in the mix (the wrong stem, or far out of time with it), or it is
    // most of the mix (the mix itself on the key), so that De-Honk is being switched off by it.
    // Held a moment, so that it does not flicker.
    const float lamp = frame.keyDb > -60.0f ? std::ceil (juce::jlimit (0.0f, 1.0f, (frame.keyDb + 60.0f) / 40.0f) * 5.0f) / 5.0f : 0.0f;
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (frame.keyed && frame.keyDb > -50.0f && frame.keyMatch < 0.3f)
        keyUnfoundMs = keyUnfoundMs <= 0.0 ? nowMs : keyUnfoundMs;
    else
        keyUnfoundMs = 0.0;
    if (frame.keyed && (frame.keyOver || (keyUnfoundMs > 0.0 && nowMs - keyUnfoundMs > 2500.0)))
        keyWarnUntilMs = nowMs + 1500.0;
    const bool warn = nowMs < keyWarnUntilMs;
    if (! juce::approximatelyEqual (lamp, shownKeyLamp) || warn != shownKeyWarn)
    {
        shownKeyLamp = lamp;
        shownKeyWarn = warn;
        dialogueBar.getProperties().set ("lamp", (double) lamp);
        dialogueBar.getProperties().set ("lampWarn", warn);
        dialogueBar.repaint();
    }
}

bool DeHonkAudioProcessorEditor::keyStateChanged (bool isKeyDown)
{
    if (isKeyDown)
        tooltip.hideNow();
    if (isKeyDown && ! lookAndFeel.showFocus)
    {
        lookAndFeel.showFocus = true;
        repaint();
    }
    return false;
}

void DeHonkAudioProcessorEditor::mouseDown (const juce::MouseEvent&)
{
    tooltip.hideNow();
    if (lookAndFeel.showFocus)
    {
        lookAndFeel.showFocus = false;
        repaint();
    }
}

void DeHonkAudioProcessorEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // (Also called for every control in the window: the editor listens to them all.)
    tooltip.hideNow();
    if (e.eventComponent == this)
        juce::AudioProcessorEditor::mouseWheelMove (e, wheel);
}

void DeHonkAudioProcessorEditor::visibilityChanged()      { updateShowing(); }
void DeHonkAudioProcessorEditor::parentHierarchyChanged() { updateShowing(); }

void DeHonkAudioProcessorEditor::updateShowing()
{
    // isShowing() is false while the window is hidden or minimised. Minimising sends no callback,
    // so the timer also calls this.
    const bool nowShowing = isShowing();
    if (nowShowing == showing)
        return;

    showing = nowShowing;
    deHonk.setEditorOpen (showing);  // display frames are written only while something shows them
    display.setActive (showing);
    startTimerHz (showing ? 30 : 4);  // while hidden, only this check runs

    if (showing)
    {
        startUpdateCheckWhenShown();
    }
    else
    {
        // Out of sight, a listening tool would be engaged with nothing saying so.
        soloButton.setToggleState (false, juce::dontSendNotification);
        diffButton.release();
        deHonk.setSoloHonk (false);
        deHonk.setDiff (false);
        tooltip.hideNow();
    }
}

void DeHonkAudioProcessorEditor::startUpdateCheckWhenShown()
{
    // Once per editor, and only when it is on screen in a host or the app: an editor that is only
    // built around a bare processor (a test, a picture) never goes online. Never from the
    // processor or the audio thread.
    if (updateCheck != nullptr || deHonk.wrapperType == juce::AudioProcessor::wrapperType_Undefined)
        return;
    // A test build (0.0.x) does not look by itself. The shared check only ever reports a full
    // release, and until De-Honk has one it finds nothing to remember, so it would ask the
    // release server again each time a window opened. About's own check still works.
    if (juce::String (JucePlugin_VersionString).startsWith ("0.0."))
        return;
    updateCheck = std::make_unique<sammyj::UpdateCheck> ("dehonk", "dehonk-v", JucePlugin_VersionString,
                                                         [this] (const sammyj::UpdateInfo& info) { showUpdate (info); });
}

void DeHonkAudioProcessorEditor::timerCallback()
{
    updateShowing();
    if (! showing)
        return;

    // The processor is the truth (a restored session switches both off).
    if (soloButton.getToggleState() != deHonk.isSoloHonk())
        soloButton.setToggleState (deHonk.isSoloHonk(), juce::dontSendNotification);
    if (diffButton.active() != deHonk.isDiff())
    {
        if (deHonk.isDiff())
            diffButton.latch();
        else
            diffButton.release();
    }

    const bool solo = soloButton.getToggleState(), diffOn = diffButton.active();
    // The dashed ring round an engaged listening button turns slowly.
    const float pulse = (float) std::fmod (juce::Time::getMillisecondCounterHiRes() * 0.001, 62.83);   // seconds: turns the ring
    soloButton.setPulse (solo ? pulse : 0.0f);
    diffButton.setPulse (diffOn ? pulse : 0.0f);

    // The guard's switch follows its amount, and remembers the last amount that was not off.
    const float guard = (float) guardBar.getValue();
    if (guard > 0.5f)
        deHonk.setGuardAmountMemory (guard);
    guardSwitch.setToggleState (guard > 0.5f, juce::dontSendNotification);

    // Grouped means something only with more than two channels.
    linkSelector.setGroupedShown (deHonk.getMainBusNumInputChannels() > 2);

    // The dialogue amount is greyed while Dialogue is off (its lamp still shows the key arriving).
    dialogueBar.setEnabled (dialogueSwitch.getToggleState());

    shelf.refresh();
    updatePresetBar();
}

//==============================================================================
void DeHonkAudioProcessorEditor::updatePresetBar()
{
    auto& presets = deHonk.getPresetManager();
    const auto text = presets.getCurrentName() + (presets.isModified() ? " *" : "");

    if (text != shownPresetText)
    {
        shownPresetText = text;
        presetButton.setButtonText (text);
    }

    deleteButton.setEnabled (presets.isCurrentUserPreset());
}

void DeHonkAudioProcessorEditor::showPresetMenu()
{
    auto& presets = deHonk.getPresetManager();
    presets.refresh();

    const auto list = presets.getPresets();
    const int current = presets.getCurrentIndex();

    juce::PopupMenu menu;
    menu.setLookAndFeel (&lookAndFeel);
    menu.addSectionHeader ("Factory");
    for (int i = 0; i < list.size(); ++i)
    {
        if (i == PresetManager::getNumFactory())
            menu.addSectionHeader ("User");
        menu.addItem (i + 1, list[i].name, true, i == current);
    }

    // Drawn inside the editor, as a card, under the preset's name.
    tooltip.hideNow();
    juce::Component::SafePointer<DeHonkAudioProcessorEditor> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withParentComponent (this).withTargetComponent (&presetButton)
                            .withMinimumWidth (presetButton.getWidth()).withStandardItemHeight (26).withMaximumNumColumns (1),
                        [safe] (int result)
                        {
                            if (safe != nullptr && result > 0)
                            {
                                safe->deHonk.getPresetManager().loadPreset (result - 1);
                                safe->updatePresetBar();
                            }
                        });
}

void DeHonkAudioProcessorEditor::showOverlay (std::unique_ptr<OverlayCard> card)
{
    tooltip.hideNow();
    juce::PopupMenu::dismissAllActiveMenus();

    if (overlay != nullptr)
    {
        // The card being replaced may be the one whose button was just clicked: it goes once this
        // call has unwound.
        overlay->setVisible (false);
        overlay->onClose = nullptr;
        juce::MessageManager::callAsync ([old = std::shared_ptr<OverlayCard> (std::move (overlay))] {});
    }

    overlay = std::move (card);
    auto* raw = overlay.get();
    juce::Component::SafePointer<DeHonkAudioProcessorEditor> safe (this);
    overlay->onClose = [safe, raw] { if (safe != nullptr) safe->overlayClosed (raw); };
    addAndMakeVisible (*overlay);
    overlay->setBounds (getLocalBounds());
    overlay->toFront (true);
}

void DeHonkAudioProcessorEditor::overlayClosed (OverlayCard* card)
{
    if (overlay.get() != card)
        return;
    overlay.reset();

    // A newer version found while another card was up is offered now.
    if (pendingUpdate.has_value())
    {
        const auto info = *pendingUpdate;
        pendingUpdate.reset();
        showUpdate (info);
    }
}

void DeHonkAudioProcessorEditor::showAbout()
{
    showOverlay (std::make_unique<AboutCard> (lookAndFeel));
}

void DeHonkAudioProcessorEditor::showUpdate (const sammyj::UpdateInfo& info)
{
    if (overlay != nullptr)
    {
        pendingUpdate = info;
        return;
    }
    showOverlay (std::make_unique<UpdateCard> (lookAndFeel, info));
}

void DeHonkAudioProcessorEditor::savePreset()
{
    auto& presets = deHonk.getPresetManager();
    juce::Component::SafePointer<DeHonkAudioProcessorEditor> safe (this);

    const auto couldNotSave = [safe]
    {
        if (safe != nullptr)
            safe->showOverlay (std::make_unique<PromptCard> (safe->lookAndFeel, "save preset",
                                                             "Could not save the preset: the presets folder could not be written to.",
                                                             false, juce::String(), "OK", [] (bool, juce::String) {}));
    };
    auto card = std::make_unique<PromptCard> (lookAndFeel, "save preset", "Name for the user preset:", true,
                                               presets.isCurrentUserPreset() ? presets.getCurrentName() : juce::String(), "Save",
                                               [safe, couldNotSave] (bool accepted, juce::String typed)
    {
        if (safe == nullptr)
            return;
        const auto name = typed.trim();
        auto& pm = safe->deHonk.getPresetManager();
        if (! accepted || ! PresetManager::isValidPresetName (name))
            return;

        const bool overwritesOther = pm.userPresetExists (name) && ! (pm.isCurrentUserPreset() && pm.getCurrentName() == name);
        if (! overwritesOther)
        {
            const bool saved = pm.saveUserPreset (name);
            safe->updatePresetBar();
            if (! saved)
                couldNotSave();
            return;
        }

        safe->showOverlay (std::make_unique<PromptCard> (safe->lookAndFeel, "replace preset",
                                                         "A user preset called \"" + name + "\" already exists. Replace it?", false, juce::String(), "Replace",
                                                         [safe, name, couldNotSave] (bool confirmed, juce::String)
        {
            if (safe == nullptr)
                return;
            const bool saved = ! confirmed || safe->deHonk.getPresetManager().saveUserPreset (name);
            safe->updatePresetBar();
            if (! saved)
                couldNotSave();
        }));
    });
    card->setValidator ([] (const juce::String& typed)
    {
        const auto name = typed.trim();
        return name.isEmpty() ? juce::String ("Type a name.")
             : PresetManager::isValidPresetName (name) ? juce::String()
             : juce::String ("That name has no letters or numbers a file can be called.");
    });
    showOverlay (std::move (card));
}

void DeHonkAudioProcessorEditor::deletePreset()
{
    auto& presets = deHonk.getPresetManager();
    if (! presets.isCurrentUserPreset())
        return;

    const auto file = presets.getCurrentFile();
    juce::Component::SafePointer<DeHonkAudioProcessorEditor> safe (this);
    showOverlay (std::make_unique<PromptCard> (lookAndFeel, "delete preset",
                                               "Delete the user preset \"" + presets.getCurrentName() + "\"? This cannot be undone.", false, juce::String(), "Delete",
                                               [safe, file] (bool confirmed, juce::String)
    {
        if (safe == nullptr)
            return;
        if (confirmed)
            safe->deHonk.getPresetManager().deleteUserPreset (file);
        safe->updatePresetBar();
    }));
}

//==============================================================================
juce::RangedAudioParameter* DeHonkAudioProcessorEditor::handleParam (int h) const
{
    using View = dehonk::scene::BandView;
    auto& apvts = deHonk.getParameters();
    if (h >= 0 && h < dehonk::kNumEdges)
        return apvts.getParameter (ParamIDs::edges[h]);
    if (h == View::thresholdHandle)
        return apvts.getParameter (ParamIDs::threshold);
    if (h == View::maxCutHandle)
        return apvts.getParameter (ParamIDs::maxCut);
    return nullptr;
}

void DeHonkAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Cream, with pale rays fanning out from below the window's right-hand corner.
    g.fillAll (toy::cream);
    const juce::Point<float> origin ((float) getWidth() * 0.86f, (float) getHeight() + 60.0f);
    const float reach = (float) (getWidth() + getHeight()) * 1.2f;
    constexpr int rays = 22;
    g.setColour (toy::creamDeep);
    for (int i = 0; i < rays; i += 2)
    {
        const float a0 = juce::MathConstants<float>::pi * (1.0f + (float) i / (float) rays);
        const float a1 = juce::MathConstants<float>::pi * (1.0f + (float) (i + 1) / (float) rays);
        juce::Path ray;
        ray.addTriangle (origin.x, origin.y, origin.x + reach * std::cos (a0), origin.y + reach * std::sin (a0),
                         origin.x + reach * std::cos (a1), origin.y + reach * std::sin (a1));
        g.fillPath (ray);
    }
}

void DeHonkAudioProcessorEditor::resized()
{
    constexpr int margin = 12, gap = 10;
    auto r = getLocalBounds().reduced (margin);

    // Top bar: name, presets, bypass.
    auto header = r.removeFromTop (40);
    r.removeFromTop (gap);
    title.setBounds (header.removeFromLeft (236));
    header.removeFromLeft (gap);
    bypassButton.setBounds (header.removeFromRight (90));
    header.removeFromRight (gap);
    deleteButton.setBounds (header.removeFromRight (74));
    header.removeFromRight (6);
    saveButton.setBounds (header.removeFromRight (64));
    header.removeFromRight (gap);
    previousButton.setBounds (header.removeFromLeft (36));
    header.removeFromLeft (6);
    nextButton.setBounds (header.removeFromRight (36));
    header.removeFromRight (6);
    presetButton.setBounds (header);

    // From the bottom up: the small controls, the knobs with the listening buttons, the shelf of
    // range tabs, the band view. The pond takes everything that is left, up to 16:9.
    auto small = r.removeFromBottom (28);
    r.removeFromBottom (gap);
    constexpr int detectionRow = 24;
    auto knobs = r.removeFromBottom (132 + detectionRow);
    r.removeFromBottom (gap);
    auto shelfArea = r.removeFromBottom (RangeShelf::height);
    r.removeFromBottom (5);
    auto band = r.removeFromBottom (dehonk::scene::BandView::heightForWidth (r.getWidth()));
    r.removeFromBottom (gap);
    const int pondMax = r.getWidth() * 9 / 16 + DisplayComponent::treeRoom;
    display.setBounds (r.withHeight (juce::jmin (r.getHeight(), pondMax)));
    if (r.getHeight() > pondMax)
    {
        // A very tall window: everything below the pond moves up under it.
        const int shift = r.getHeight() - pondMax;
        for (auto* area : { &band, &shelfArea, &knobs, &small })
            area->translate (0, -shift);
    }
    bandView.setBounds (band);
    shelf.setBounds (shelfArea);
    // The pond's frequency axis is laid over the band view's plot, so that each tree and post
    // stands over its handle.
    {
        const auto plot = bandView.plotArea().translated ((float) bandView.getX(), 0.0f);
        const auto card = display.pictureBounds().toFloat().translated ((float) display.getX(), 0.0f);
        if (card.getWidth() > 1.0f)
            display.setAxisSpan ((plot.getX() - card.getX()) / card.getWidth(), (plot.getRight() - card.getX()) / card.getWidth());
    }

    auto captionWidth = [] (const juce::Label& label)
    {
        return juce::roundToInt (std::ceil (juce::GlyphArrangement::getStringWidth (label.getFont(), label.getText()))) + 6;
    };

    // Six knobs on the left, each with its name above. On the right, what is heard and what goes
    // out: the two listening buttons, and Mix and Trim under them.
    auto listen = knobs.removeFromRight (juce::jlimit (252, 320, knobs.getWidth() * 3 / 10));
    knobs.removeFromRight (gap);
    listen.removeFromBottom (detectionRow);
    auto output = listen.removeFromBottom (28);
    listen.removeFromBottom (6);
    const int listenWidth = (listen.getWidth() - gap) / 2;
    soloButton.setBounds (listen.withWidth (listenWidth));
    diffButton.setBounds (listen.withLeft (listen.getRight() - listenWidth));
    {
        constexpr int tagGap = 4;
        const int mixCaptionW = captionWidth (mixCaption), trimCaptionW = captionWidth (trimCaption);
        // Trim's longest value ("-12.0 dB") is wider than Mix's ("100 %"): it has the larger share.
        const int barWidth = juce::jmax (40, (output.getWidth() - mixCaptionW - trimCaptionW - 2 * tagGap - gap) * 9 / 20);
        mixCaption.setBounds (output.removeFromLeft (mixCaptionW));
        output.removeFromLeft (tagGap);
        mixBar.setBounds (output.removeFromLeft (barWidth));
        output.removeFromLeft (gap);
        trimCaption.setBounds (output.removeFromLeft (trimCaptionW));
        output.removeFromLeft (tagGap);
        trimBar.setBounds (output);
        mixCaption.setJustificationType (juce::Justification::centredRight);
        trimCaption.setJustificationType (juce::Justification::centredRight);
    }

    // Detection's row under the knobs: its pill under Sharpness, its name under Max Cut, to the left.
    auto detectionArea = knobs.removeFromBottom (detectionRow);
    const int knobWidth = knobs.getWidth() / 6;
    {
        auto row = detectionArea.withTrimmedTop (2);
        const int pillWidth = juce::jmin (knobWidth - 4, 118);
        const int pillX = knobs.getX() + 3 * knobWidth + (knobWidth - pillWidth) / 2;
        detectionSelector.setBounds (pillX, row.getY(), pillWidth, row.getHeight());
        const int captionW = captionWidth (detectionCaption);
        detectionCaption.setBounds (pillX - 6 - captionW, row.getY(), captionW, row.getHeight());
        detectionCaption.setJustificationType (juce::Justification::centredRight);
    }
    for (auto [knob, caption] : { std::pair<juce::Slider*, juce::Label*> { &thresholdKnob, &thresholdCaption },
                                  { &depthKnob, &depthCaption },
                                  { &maxCutKnob, &maxCutCaption },
                                  { &sharpnessKnob, &sharpnessCaption },
                                  { &speedKnob, &speedCaption },
                                  { &releaseKnob, &releaseCaption } })
    {
        auto column = knobs.removeFromLeft (knobWidth);
        caption->setBounds (column.removeFromTop (20));
        knob->setBounds (column);
    }

    // The bottom row: Impact Guard and Dialogue each have their switch before the name and their
    // amount after it; Link is one pill in three parts; Lookahead is a choice. Every name gets the
    // width its own text needs; Link's pill gets about twice a share of what is left.
    struct Cell
    {
        juce::Component* lamp;
        juce::Label* caption;
        juce::Component* control;
        float share;
    };
    const Cell cells[] { { &guardSwitch, &guardCaption, &guardBar, 1.0f },
                         { nullptr, &linkCaption, &linkSelector, 1.9f },
                         { nullptr, &lookaheadCaption, &lookaheadBox, 1.0f },
                         { &dialogueSwitch, &dialogueCaption, &dialogueBar, 1.0f } };
    constexpr int numCells = (int) std::size (cells), tagGap = 6, lampWidth = 20;
    int fixed = (numCells - 1) * (gap + 4);
    float shares = 0.0f;
    for (const auto& cell : cells)
    {
        fixed += captionWidth (*cell.caption) + tagGap + (cell.lamp != nullptr ? lampWidth : 0);
        shares += cell.share;
    }
    const float shareWidth = (float) juce::jmax (0, small.getWidth() - fixed) / shares;
    for (int i = 0; i < numCells; ++i)
    {
        const auto& cell = cells[i];
        if (cell.lamp != nullptr)
            cell.lamp->setBounds (small.removeFromLeft (lampWidth));
        cell.caption->setBounds (small.removeFromLeft (captionWidth (*cell.caption)));
        cell.caption->setJustificationType (juce::Justification::centredRight);
        small.removeFromLeft (tagGap);
        cell.control->setBounds (i + 1 < numCells ? small.removeFromLeft (juce::jmax (48, juce::roundToInt (shareWidth * cell.share))) : small);
        small.removeFromLeft (gap + 4);
    }

    if (overlay != nullptr)
        overlay->setBounds (getLocalBounds());
}

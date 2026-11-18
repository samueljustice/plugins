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
ModuleButton::ModuleButton (const juce::String& name, juce::Colour c, float fullScaleDb, double holdSeconds)
    : juce::Button (name), colour (c), fullScale (fullScaleDb), hold (holdSeconds)
{
    setButtonText (name);
    setClickingTogglesState (true);
}

void ModuleButton::show (float db)
{
    if (std::abs (db - shown) >= 0.05f || (db == 0.0f && shown != 0.0f))
    {
        shown = db;
        repaint();
    }
}

void ModuleButton::setFullScaleDb (float db)
{
    db = juce::jmax (0.5f, std::isfinite (db) ? db : fullScale);
    if (std::abs (db - fullScale) >= 0.05f)
    {
        fullScale = db;
        repaint();
    }
}

void ModuleButton::setCutDb (float db)
{
    db = juce::jlimit (0.0f, fullScale, std::isfinite (db) ? db : 0.0f);
    if (hold <= 0.0)
    {
        show (db);
    }
    else if (db >= shown && db > 0.0f)
    {
        peak = db;
        sincePeak = 0.0;
        show (db);
    }
}

void ModuleButton::advance (double seconds)
{
    if (hold <= 0.0 || peak <= 0.0f)
        return;

    sincePeak += juce::jmax (0.0, seconds);
    const float db = peak * (float) juce::jmax (0.0, 1.0 - sincePeak / hold);
    if (db <= 0.0f)
        peak = 0.0f;
    show (db);
}

void ModuleButton::paintButton (juce::Graphics& g, bool, bool isButtonDown)
{
    // A flat tab: a small square lamp, the name, and a thin meter of what it is taking out.
    const bool on = getToggleState();
    const auto body = Toy::body (getLocalBounds());
    const auto fill = on ? (isButtonDown ? colour.darker (0.08f) : colour) : toy::paper;
    Toy::shape (g, body, 8.0f, fill);
    if (auto* look = dynamic_cast<DeHonkLookAndFeel*> (&getLookAndFeel()))
        look->drawFocus (g, *this, body, 8.0f);

    auto inner = body.reduced (12.0f, 0.0f);

    // The lamp: red while the module is cutting.
    const auto lamp = juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ inner.getX() + 4.5f, body.getCentreY() });
    g.setColour (on && shown > 0.5f ? toy::jacketRed : toy::paper);
    g.fillRect (lamp);
    g.setColour (toy::ink.withAlpha (on ? 1.0f : 0.4f));
    g.drawRect (lamp, toy::outline);
    inner.removeFromLeft (9.0f + 9.0f);

    const auto font = fonts->name (12.0f);
    const auto text = getButtonText().toLowerCase();
    const float nameWidth = juce::GlyphArrangement::getStringWidth (font, text) + 4.0f;
    g.setFont (font);
    g.setColour (toy::ink.withAlpha (on ? 1.0f : 0.45f));
    g.drawText (text, inner.removeFromLeft (nameWidth), juce::Justification::centredLeft, false);
    inner.removeFromLeft (10.0f);

    if (inner.getWidth() >= 16.0f)
    {
        // A thin line that fills from the left.
        const auto track = inner.withSizeKeepingCentre (inner.getWidth(), 5.0f);
        const float t = juce::jlimit (0.0f, 1.0f, shown / fullScale);
        g.setColour (toy::ink.withAlpha (on ? 0.22f : 0.12f));
        g.fillRect (track.withSizeKeepingCentre (track.getWidth(), 1.0f));
        if (t > 0.0f)
        {
            g.setColour (toy::jacketRed);
            g.fillRect (track.withWidth (juce::jmax (4.0f, t * track.getWidth())));
        }
        g.setColour (toy::ink.withAlpha (on ? 1.0f : 0.4f));
        g.fillRect (juce::Rectangle<float> (track.getRight() - 1.0f, track.getY() - 2.0f, 1.0f, track.getHeight() + 4.0f));
    }
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
void GuardLamp::setHeld (float amount)
{
    amount = juce::jlimit (0.0f, 1.0f, amount);
    if (std::abs (amount - held) > 0.01f || (amount == 0.0f) != (held == 0.0f))
    {
        held = amount;
        repaint();
    }
}

void GuardLamp::paint (juce::Graphics& g)
{
    // A small round lamp, a disc like the big buttons: cream when off, mustard while the guard is
    // up (more of it, the further up).
    const float d = juce::jmin (11.0f, (float) juce::jmin (getWidth(), getHeight()) - toy::outline);
    const auto lamp = juce::Rectangle<float> (d, d).withCentre (getLocalBounds().toFloat().getCentre());
    g.setColour (held > 0.02f ? toy::paper.interpolatedWith (toy::sunYellow, juce::jlimit (0.35f, 1.0f, held)) : toy::paper);
    g.fillEllipse (lamp);
    g.setColour (toy::ink);
    g.drawEllipse (lamp, toy::outline);
}

//==============================================================================
DeHonkAudioProcessorEditor::DeHonkAudioProcessorEditor (DeHonkAudioProcessor& p)
    : AudioProcessorEditor (&p), deHonk (p), display (p),
      snapButton ("Snap", toy::click, dehonk::Tuning {}.clickMaxCutDb, 0.3)
{
    setLookAndFeel (&lookAndFeel);
    auto& apvts = deHonk.getParameters();

    addAndMakeVisible (display);

    // Header: name, presets, bypass
    title.setText ("De-Honk", juce::dontSendNotification);
    title.setText ("de-honk", juce::dontSendNotification);
    title.setFont (fonts->title (22.0f));
    title.setJustificationType (juce::Justification::centredLeft);
    title.setColour (juce::Label::textColourId, toy::ink);
    title.setTooltip ("De-Honk by Samuel Justice. Finds build-ups in the low end and low mids as they happen and takes "
                      "them down with narrow cuts, while the mix is louder than the threshold.");
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
               "At Always (the bottom of the range) it acts at any level. The bar at the left of the band view shows "
               "the mix level with the threshold marked on it.");
    setUpKnob (depthKnob, depthCaption, "Depth", ParamIDs::depth,
               "How much of the detected excess is removed. No cut goes deeper than Max Cut. At 0 nothing is cut.");
    setUpKnob (maxCutKnob, maxCutCaption, "Max Cut", ParamIDs::maxCut,
               "The deepest any one cut may go, whatever Depth is set to. Two cuts side by side never add up to more. "
               "The dashed line across the top of the band view marks it.");
    setUpKnob (sharpnessKnob, sharpnessCaption, "Sharpness", ParamIDs::sharpness,
               "How wide a build-up has to be to count. Broad responds to a whole bump; Narrow responds only to "
               "single ringing resonances. The cuts themselves are narrow either way.");
    setUpKnob (speedKnob, speedCaption, "Speed", ParamIDs::speed,
               "How quickly a cut goes in once a build-up has been found. Release sets how it lets go.");
    setUpKnob (releaseKnob, releaseCaption, "Release", ParamIDs::release,
               "How slowly a cut lets go once its build-up has gone. Longer holds each cut steadier; shorter follows the mix more closely. "
               "It is for cuts that have settled on a clear build-up: a light cut that comes and goes, or one whose build-up has moved to the next note, "
               "lets go quickly whatever is set here. Under the Threshold every cut closes straight away.");
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

    // Modules
    static const char* const moduleTips[dehonk::kNumModules]
    {
        "the low end barking on every hit, about 60 to 110 Hz.",
        "tubby weight just above the bass, about 110 to 200 Hz. This is where added bass harmonics pile up first.",
        "low mids clouding over when music and effects stack up, about 200 to 400 Hz.",
        "hard, shouty midrange, about 400 Hz to 1.5 kHz.",
    };
    for (int m = 0; m < dehonk::kNumModules; ++m)
    {
        auto& button = moduleButtons[(size_t) m];
        button = std::make_unique<ModuleButton> (ParamRanges::moduleNames[m], toy::module[m], ParamRanges::maxCutDefault);
        button->setTooltip (juce::String (ParamRanges::moduleNames[m]) + ": " + moduleTips[m]
                            + " Click to switch it on or off. The light is on while it is cutting and the meter shows how deep: full is Max Cut. "
                              "Drag the handles in the band view to move its edges.");
        moduleAttachments[(size_t) m] = std::make_unique<ButtonAttachment> (apvts, ParamIDs::moduleOn[m], *button);
        addAndMakeVisible (*button);
    }
    snapButton.setTooltip ("Snap: sharp clicks and pops around 2.5 kHz. Takes the edge off a pop that the rest of the sound does not share, "
                           "and leaves drum hits and steady sounds alone. Works at any level.");
    snapAttachment = std::make_unique<ButtonAttachment> (apvts, ParamIDs::snapOn, snapButton);
    addAndMakeVisible (snapButton);

    // The band view: fed from the display's clock, and where the edges are dragged.
    addAndMakeVisible (bandView);
    display.onTick = [this] (const dehonk::scene::BandViewState& state, double seconds) { bandView.update (state, seconds); };
    bandView.onEdgeDragStarted = [this] (int e) { if (auto* edge = edgeParam (e)) edge->beginChangeGesture(); };
    bandView.onEdgeDragEnded = [this] (int e) { if (auto* edge = edgeParam (e)) edge->endChangeGesture(); };
    bandView.onEdgeDragged = [this] (int e, float hz)
    {
        auto* edge = edgeParam (e);
        if (edge == nullptr)
            return;
        // Inside its own range, and never within a tenth of its neighbours.
        const auto used = display.edgesAsUsed();
        float lo = dehonk::kEdgeMinHz[e], hi = dehonk::kEdgeMaxHz[e];
        if (e > 0)                       lo = juce::jmax (lo, used[(size_t) e - 1] * ParamRanges::minEdgeRatio);
        if (e + 1 < dehonk::kNumEdges)   hi = juce::jmin (hi, used[(size_t) e + 1] / ParamRanges::minEdgeRatio);
        if (hi > lo)
            edge->setValueNotifyingHost (edge->convertTo0to1 (juce::jlimit (lo, hi, hz)));
    };

    display.onFrame = [this] (const dehonk::DisplayFrame& frame)
    {
        // A full meter is Max Cut as it is set now.
        const auto maxCut = (float) maxCutKnob.getValue();
        for (int m = 0; m < dehonk::kNumModules; ++m)
        {
            moduleButtons[(size_t) m]->setFullScaleDb (maxCut);
            moduleButtons[(size_t) m]->setCutDb (frame.moduleCutDb[m]);
        }
        snapButton.setCutDb (frame.snapCutDb);
        guardLamp.setHeld (frame.guard);
    };

    // Listening tools
    soloButton.setClickingTogglesState (true);
    soloButton.setTooltip ("Plays only what is being removed. If it sounds only ugly, the settings are right. "
                           "If you hear wanted bass or body in it, lower Depth or switch off the lowest module. Not saved with the session.");
    soloButton.onClick = [this] { setSoloHonk (soloButton.getToggleState()); };
    addAndMakeVisible (soloButton);

    diffButton.setTooltip ("Hold to hear the untreated signal, latency-matched. Click to keep it on; click again to return. "
                           "The host's bypass is not touched. Not saved with the session.");
    diffButton.onActiveChange = [this] (bool on) { setDiff (on); };
    addAndMakeVisible (diffButton);

    // Secondary row
    const juce::String guardTip ("Keeps the cuts out of big hits. When something lands across the whole spectrum at once (an explosion, a slam, a hit), "
                                 "no new cut is made while it sounds, and a cut that is already open is eased off as far as the hit drowns out what it was there for, "
                                 "by up to this much. The cuts come back as the hit dies away. At Off, hits are treated like anything else. "
                                 "The lamp is lit while the guard is up.");
    setUpBar (guardBar, guardCaption, "Impact Guard", ParamIDs::impactGuard, guardTip);
    guardLamp.setTooltip (guardTip);
    guardLamp.setInterceptsMouseClicks (true, false);
    addAndMakeVisible (guardLamp);
    guardAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::impactGuard, guardBar);
    setUpBar (linkBar, linkCaption, "Link", ParamIDs::link,
              "At 100% every channel shares one detection, so the image does not move. Lower values let each channel be cut on its own.");
    setUpBar (mixBar, mixCaption, "Mix", ParamIDs::mix,
              "Balance between the untreated signal (0%) and the treated signal (100%).");
    setUpBar (trimBar, trimCaption, "Trim", ParamIDs::trim, "Output level, after the mix.");
    linkAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::link, linkBar);
    mixAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::mix, mixBar);
    trimAttachment = std::make_unique<SliderAttachment> (apvts, ParamIDs::trim, trimBar);

    setUpCaption (lookaheadCaption, "Lookahead");
    lookaheadBox.addItemList (juce::StringArray { "Off", "5 ms", "10 ms", "20 ms" }, 1);
    lookaheadBox.setTooltip ("Delays the audio so each cut is in place when a transient arrives. "
                             "Adds the chosen latency, which is reported to the host.");
    lookaheadAttachment = std::make_unique<ComboBoxAttachment> (apvts, ParamIDs::lookahead, lookaheadBox);
    addAndMakeVisible (lookaheadBox);

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
    startTimerHz (30);
}

DeHonkAudioProcessorEditor::~DeHonkAudioProcessorEditor()
{
    stopTimer();
    display.setActive (false);
    if (showing)
        deHonk.setEditorOpen (false);

    // Solo Honk and Diff are only heard with their pressed buttons and the band view's label in view.
    deHonk.setSoloHonk (false);
    deHonk.setDiff (false);

    juce::PopupMenu::dismissAllActiveMenus();
    dialog.reset();
    setLookAndFeel (nullptr);
}

void DeHonkAudioProcessorEditor::setUpCaption (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setText (text.toLowerCase(), juce::dontSendNotification);
    label.setFont (fonts->name (11.0f));
    label.setColour (juce::Label::textColourId, toy::ink);
    label.getProperties().set ("pill", true);
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

bool DeHonkAudioProcessorEditor::keyStateChanged (bool isKeyDown)
{
    if (isKeyDown && ! lookAndFeel.showFocus)
    {
        lookAndFeel.showFocus = true;
        repaint();
    }
    return false;
}

void DeHonkAudioProcessorEditor::mouseDown (const juce::MouseEvent&)
{
    if (lookAndFeel.showFocus)
    {
        lookAndFeel.showFocus = false;
        repaint();
    }
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

    if (! showing)
    {
        // Out of sight, a listening tool would be engaged with nothing saying so.
        soloButton.setToggleState (false, juce::dontSendNotification);
        diffButton.release();
        deHonk.setSoloHonk (false);
        deHonk.setDiff (false);
    }
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

    // Click's held meter falls back on its own.
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    snapButton.advance (lastTimerMs > 0.0 ? (nowMs - lastTimerMs) * 0.001 : 0.0);
    lastTimerMs = nowMs;

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
    menu.addSectionHeader ("Factory");
    for (int i = 0; i < list.size(); ++i)
    {
        if (i == PresetManager::getNumFactory())
            menu.addSectionHeader ("User");
        menu.addItem (i + 1, list[i].name, true, i == current);
    }

    juce::Component::SafePointer<DeHonkAudioProcessorEditor> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetButton).withMinimumWidth (presetButton.getWidth()),
                        [safe] (int result)
                        {
                            if (safe != nullptr && result > 0)
                            {
                                safe->deHonk.getPresetManager().loadPreset (result - 1);
                                safe->updatePresetBar();
                            }
                        });
}

void DeHonkAudioProcessorEditor::savePreset()
{
    auto& presets = deHonk.getPresetManager();

    dialog = std::make_unique<juce::AlertWindow> ("Save preset", "Name for the user preset:", juce::MessageBoxIconType::NoIcon, this);
    dialog->setLookAndFeel (&lookAndFeel);
    dialog->addTextEditor ("name", presets.isCurrentUserPreset() ? presets.getCurrentName() : juce::String());
    dialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<DeHonkAudioProcessorEditor> safe (this);
    dialog->enterModalState (true, juce::ModalCallbackFunction::create ([safe] (int result)
    {
        if (safe == nullptr || safe->dialog == nullptr)
            return;

        const auto name = safe->dialog->getTextEditorContents ("name").trim();
        safe->dialog.reset();

        auto& pm = safe->deHonk.getPresetManager();
        if (result != 1 || ! PresetManager::isValidPresetName (name))
            return;

        const bool overwritesOther = pm.userPresetExists (name) && ! (pm.isCurrentUserPreset() && pm.getCurrentName() == name);
        if (! overwritesOther)
        {
            pm.saveUserPreset (name);
            safe->updatePresetBar();
            return;
        }

        safe->dialog = std::make_unique<juce::AlertWindow> ("Replace preset", "A user preset called \"" + name + "\" already exists. Replace it?",
                                                            juce::MessageBoxIconType::QuestionIcon, safe.getComponent());
        safe->dialog->setLookAndFeel (&safe->lookAndFeel);
        safe->dialog->addButton ("Replace", 1, juce::KeyPress (juce::KeyPress::returnKey));
        safe->dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        safe->dialog->enterModalState (true, juce::ModalCallbackFunction::create ([safe, name] (int confirmed)
        {
            if (safe == nullptr)
                return;
            safe->dialog.reset();
            if (confirmed == 1)
                safe->deHonk.getPresetManager().saveUserPreset (name);
            safe->updatePresetBar();
        }), false);
    }), false);
}

void DeHonkAudioProcessorEditor::deletePreset()
{
    auto& presets = deHonk.getPresetManager();
    if (! presets.isCurrentUserPreset())
        return;

    const auto file = presets.getCurrentFile();
    dialog = std::make_unique<juce::AlertWindow> ("Delete preset", "Delete the user preset \"" + presets.getCurrentName() + "\"? This cannot be undone.",
                                                  juce::MessageBoxIconType::WarningIcon, this);
    dialog->setLookAndFeel (&lookAndFeel);
    dialog->addButton ("Delete", 1);
    dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<DeHonkAudioProcessorEditor> safe (this);
    dialog->enterModalState (true, juce::ModalCallbackFunction::create ([safe, file] (int result)
    {
        if (safe == nullptr)
            return;
        safe->dialog.reset();
        if (result == 1)
            safe->deHonk.getPresetManager().deleteUserPreset (file);
        safe->updatePresetBar();
    }), false);
}

//==============================================================================
juce::RangedAudioParameter* DeHonkAudioProcessorEditor::edgeParam (int e) const
{
    return e >= 0 && e < dehonk::kNumEdges ? deHonk.getParameters().getParameter (ParamIDs::edges[e]) : nullptr;
}

void DeHonkAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Cream, with pale rays fanning out from below the window's right-hand corner.
    g.fillAll (toy::cream);
    {
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

    // A small rainbow rule under the name, and a dot before it.
    const auto name = title.getBounds().toFloat();
    for (int i = 0; i < 5; ++i)
    {
        g.setColour (toy::rainbow[i]);
        g.fillRect (juce::Rectangle<float> (name.getX() + 4.0f, name.getBottom() - 9.0f + (float) i * 2.0f, 118.0f, 2.0f));
    }

    // A rainbow rule between the pictures and the controls.
    const float y = (float) bandView.getBottom() + 7.0f;
    for (int i = 0; i < 5; ++i)
    {
        g.setColour (toy::rainbow[i].withAlpha (0.9f));
        g.fillRect (juce::Rectangle<float> ((float) bandView.getX(), y + (float) i * 1.5f, (float) bandView.getWidth(), 1.5f));
    }
}

void DeHonkAudioProcessorEditor::resized()
{
    constexpr int margin = 12, gap = 10, drop = (int) toy::dropY;
    auto r = getLocalBounds().reduced (margin);

    // Top bar: name, presets, bypass.
    auto header = r.removeFromTop (40 + drop);
    r.removeFromTop (gap - drop);
    title.setBounds (header.removeFromLeft (150).withTrimmedBottom (8));
    header.removeFromLeft (gap);
    bypassButton.setBounds (header.removeFromRight (104));
    header.removeFromRight (gap);
    deleteButton.setBounds (header.removeFromRight (88));
    header.removeFromRight (6);
    saveButton.setBounds (header.removeFromRight (76));
    header.removeFromRight (gap);
    previousButton.setBounds (header.removeFromLeft (header.getHeight() - drop + 4));
    header.removeFromLeft (6);
    nextButton.setBounds (header.removeFromRight (header.getHeight() - drop + 4));
    header.removeFromRight (6);
    presetButton.setBounds (header);

    // From the bottom up: the small controls, the knobs with the listening buttons, the modules,
    // the band view. The pond takes everything that is left, up to 16:9.
    auto small = r.removeFromBottom (28 + drop);
    r.removeFromBottom (gap);
    auto knobs = r.removeFromBottom (132);
    r.removeFromBottom (gap);
    auto modules = r.removeFromBottom (36 + drop);
    r.removeFromBottom (gap + 12);       // room for the rainbow rule
    auto band = r.removeFromBottom (dehonk::scene::BandView::heightForWidth (r.getWidth()));
    r.removeFromBottom (gap - drop);
    const int pondMax = r.getWidth() * 9 / 16 + drop;
    display.setBounds (r.withHeight (juce::jmin (r.getHeight(), pondMax)));
    if (r.getHeight() > pondMax)
    {
        // A very tall window: everything below the pond moves up under it and the lawn shows beneath.
        const int shift = r.getHeight() - pondMax;
        for (auto* area : { &band, &modules, &knobs, &small })
            area->translate (0, -shift);
    }
    bandView.setBounds (band);

    // The five module pills.
    constexpr int numSwitches = dehonk::kNumModules + 1;
    const int moduleWidth = (modules.getWidth() - (numSwitches - 1) * gap) / numSwitches;
    for (auto& button : moduleButtons)
    {
        button->setBounds (modules.removeFromLeft (moduleWidth));
        modules.removeFromLeft (gap);
    }
    snapButton.setBounds (modules);

    // Six knobs on the left, each with its name above; the listening buttons on the right.
    auto listen = knobs.removeFromRight (juce::jlimit (200, 300, knobs.getWidth() / 4));
    knobs.removeFromRight (gap);
    const int listenWidth = (listen.getWidth() - gap) / 2;
    const auto listenRow = listen.withSizeKeepingCentre (listen.getWidth(), juce::jmin (listen.getHeight(), 104));
    soloButton.setBounds (listenRow.withWidth (listenWidth));
    diffButton.setBounds (listenRow.withLeft (listenRow.getRight() - listenWidth));

    const int knobWidth = knobs.getWidth() / 6;
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

    // Impact Guard (with its lamp), Link, Lookahead, Mix, Trim: each a name tag with its pill beside
    // it. Every name gets the width its own text needs, and the pills share what is left.
    struct Cell
    {
        juce::Label* caption;
        juce::Component* control;
        juce::Component* lamp;
    };
    const Cell cells[] { { &guardCaption, &guardBar, &guardLamp }, { &linkCaption, &linkBar, nullptr }, { &lookaheadCaption, &lookaheadBox, nullptr },
                         { &mixCaption, &mixBar, nullptr }, { &trimCaption, &trimBar, nullptr } };
    constexpr int numCells = (int) std::size (cells), tagGap = 6, lampWidth = 13;
    int captionWidth[numCells] {}, fixed = (numCells - 1) * gap;
    for (int i = 0; i < numCells; ++i)
    {
        captionWidth[i] = juce::roundToInt (std::ceil (juce::GlyphArrangement::getStringWidth (cells[i].caption->getFont(), cells[i].caption->getText()))) + 6;
        fixed += captionWidth[i] + tagGap + (cells[i].lamp != nullptr ? lampWidth + 2 : 0);
    }
    const int controlWidth = juce::jmax (48, (small.getWidth() - fixed) / numCells);
    for (int i = 0; i < numCells; ++i)
    {
        cells[i].caption->setBounds (small.removeFromLeft (captionWidth[i]).withTrimmedBottom (drop));
        cells[i].caption->setJustificationType (juce::Justification::centredRight);
        if (cells[i].lamp != nullptr)
        {
            small.removeFromLeft (2);
            cells[i].lamp->setBounds (small.removeFromLeft (lampWidth).withTrimmedBottom (drop));
        }
        small.removeFromLeft (tagGap);
        cells[i].control->setBounds (i + 1 < numCells ? small.removeFromLeft (controlWidth) : small);
        small.removeFromLeft (gap);
    }
}

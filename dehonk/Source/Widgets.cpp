#include "Widgets.h"

#include "Parameters.h"

using namespace dehonk;

namespace
{
    float textWidth (const juce::Font& font, const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth (font, text);
    }

    juce::String milliseconds (double ms)
    {
        return (ms < 9.95 ? juce::String (ms, 1) : juce::String (juce::roundToInt (ms))) + " ms";
    }

    /** A frequency the way the tabs show it: "110", "1.5 k" (the unit is added by the caller). */
    juce::String hzNumber (float hz)
    {
        if (hz < 1000.0f)
            return juce::String (juce::roundToInt (hz));
        const float k = std::round (hz / 100.0f) / 10.0f;
        return juce::String (k, juce::approximatelyEqual (k, std::round (k)) ? 0 : 1);
    }

    /** "110-200 Hz", "400 Hz-1.5 kHz": a range from lo to hi. */
    juce::String hzRange (float lo, float hi, const char* joiner)
    {
        const auto unit = [] (float hz) { return hz < 1000.0f ? juce::String (" Hz") : juce::String (" kHz"); };
        if ((lo < 1000.0f) == (hi < 1000.0f))
            return hzNumber (lo) + joiner + hzNumber (hi) + unit (hi);
        return hzNumber (lo) + unit (lo) + joiner + hzNumber (hi) + unit (hi);
    }
}

//============================================================================== SwitchLamp
SwitchLamp::SwitchLamp (const juce::String& name, juce::Colour glowColour) : juce::Button (name), colour (glowColour)
{
    setClickingTogglesState (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void SwitchLamp::setGlow (float amount)
{
    // Six steps are plenty for a lamp this small, and keep it from repainting on every frame.
    amount = std::round (juce::jlimit (0.0f, 1.0f, amount) * 6.0f) / 6.0f;
    if (! juce::approximatelyEqual (amount, glow))
    {
        glow = amount;
        repaint();
    }
}

void SwitchLamp::draw (juce::Graphics& g, juce::Rectangle<float> r, bool on, float glow, juce::Colour glowColour, bool hot)
{
    const float corner = r.getWidth() * 0.28f;
    g.setColour (on && glow > 0.0f ? toy::paper.interpolatedWith (glowColour, juce::jlimit (0.4f, 1.0f, glow)) : toy::paper);
    g.fillRoundedRectangle (r, corner);
    g.setColour (toy::ink.withAlpha (on || hot ? 1.0f : 0.5f));
    g.drawRoundedRectangle (r, corner, hot ? toy::outline + 0.6f : toy::outline);
    if (on)
    {
        const float d = r.getWidth() * 0.42f;
        g.setColour (toy::ink);
        g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (r.getCentre()));
    }
}

void SwitchLamp::paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown)
{
    const float d = juce::jmin (15.0f, (float) juce::jmin (getWidth(), getHeight()) - 2.0f);
    const auto r = juce::Rectangle<float> (d, d).withCentre (getLocalBounds().toFloat().getCentre());
    draw (g, r, getToggleState(), glow, colour, isMouseOver || isButtonDown);
    if (auto* look = dynamic_cast<DeHonkLookAndFeel*> (&getLookAndFeel()))
        look->drawFocus (g, *this, r.expanded (4.0f), 6.0f);
}

//============================================================================== RangeTab
RangeTab::RangeTab (const juce::String& n, juce::Colour c, juce::RangedAudioParameter& onParameter)
    : name (n), colour (c),
      attachment (onParameter, [this] (float v)
                  {
                      const bool nowOn = v >= 0.5f;
                      if (nowOn != on)
                      {
                          on = nowOn;
                          repaint();
                          if (auto* shelf = getParentComponent())
                              shelf->repaint();   // the drawer under a selected tab takes its colour
                      }
                  })
{
    setTitle (n);
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    attachment.sendInitialUpdate();
}

juce::Colour RangeTab::fillFor (juce::Colour rangeColour, bool isOn) noexcept
{
    return isOn ? rangeColour : rangeColour.interpolatedWith (toy::paper, 0.72f);
}

juce::Colour RangeTab::fillColour() const noexcept { return fillFor (colour, on); }

void RangeTab::setSelected (bool shouldBeSelected)
{
    if (selected != shouldBeSelected)
    {
        selected = shouldBeSelected;
        repaint();
    }
}

void RangeTab::setCutting (bool isCutting)
{
    if (cutting != isCutting)
    {
        cutting = isCutting;
        repaint (lampBounds().expanded (3.0f).toNearestInt());
    }
}

void RangeTab::setRangeText (const juce::String& text)
{
    if (text != rangeText)
    {
        rangeText = text;
        repaint();
    }
}

juce::Rectangle<float> RangeTab::lampBounds() const noexcept
{
    const float top = selected ? 0.0f : (float) rise;
    const float cy = top + ((float) getHeight() - top) * 0.5f;
    return juce::Rectangle<float> (13.0f, 13.0f).withCentre ({ 20.0f, cy });
}

bool RangeTab::overLamp (juce::Point<float> at) const noexcept
{
    // Generous: the whole left end of the tab.
    return at.x < lampBounds().getRight() + 8.0f;
}

void RangeTab::paint (juce::Graphics& g)
{
    // A tab standing on the shelf: round at the top, square where it meets the line. The selected
    // one stands a little taller and has no line under it, so it runs into the drawer below.
    const float line = toy::outline;
    const float top = (selected ? 0.0f : (float) rise) + line * 0.5f;
    const auto r = juce::Rectangle<float> (line * 0.5f, top, (float) getWidth() - line, (float) getHeight() - top);
    const float corner = 9.0f;

    juce::Path shape;
    shape.startNewSubPath (r.getX(), r.getBottom());
    shape.lineTo (r.getX(), r.getY() + corner);
    shape.quadraticTo (r.getX(), r.getY(), r.getX() + corner, r.getY());
    shape.lineTo (r.getRight() - corner, r.getY());
    shape.quadraticTo (r.getRight(), r.getY(), r.getRight(), r.getY() + corner);
    shape.lineTo (r.getRight(), r.getBottom());

    auto fill = fillColour();
    if (hot && ! selected)
        fill = fill.brighter (0.12f);
    g.setColour (fill);
    {
        juce::Path closed (shape);
        closed.closeSubPath();
        g.fillPath (closed);
    }
    g.setColour (toy::ink);
    g.strokePath (shape, juce::PathStrokeType (line));
    if (! selected)
        g.fillRect (juce::Rectangle<float> (0.0f, (float) getHeight() - line, (float) getWidth(), line));

    SwitchLamp::draw (g, lampBounds(), on, cutting ? 1.0f : 0.0f, toy::jacketRed, hotLamp);

    // The name, and under it the frequencies the range covers now.
    const auto text = name.toLowerCase();
    const auto font = fonts->name (12.0f);
    const auto small = fonts->body (10.0f);
    const auto textArea = juce::Rectangle<float> (lampBounds().getRight() + 9.0f, top, r.getRight() - lampBounds().getRight() - 15.0f, r.getHeight());
    const float nameHeight = 14.0f, gapBelowName = 3.0f, rangeHeight = 12.0f;
    const float y0 = textArea.getCentreY() - (nameHeight + gapBelowName + rangeHeight) * 0.5f;
    const auto nameArea = textArea.withY (y0).withHeight (nameHeight);
    g.setFont (font);
    g.setColour (toy::ink.withAlpha (on ? 1.0f : 0.5f));
    g.drawText (text, nameArea, juce::Justification::centredLeft, false);

    if (rangeText.isNotEmpty())
    {
        g.setFont (small);
        g.setColour (toy::ink.withAlpha (on ? 0.72f : 0.4f));
        g.drawFittedText (rangeText, textArea.withY (nameArea.getBottom() + gapBelowName).withHeight (rangeHeight).toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.8f);
    }

    if (selected)
    {
        // A short rule under the name of the range that is being worked on.
        const float w = juce::jmin (textArea.getWidth(), textWidth (font, text));
        g.setColour (toy::ink);
        g.fillRect (juce::Rectangle<float> (nameArea.getX(), nameArea.getBottom() + 0.5f, w - 2.0f, 1.5f));
    }

    if (auto* look = dynamic_cast<DeHonkLookAndFeel*> (&getLookAndFeel()))
        look->drawFocus (g, *this, r, corner);
}

void RangeTab::mouseMove (const juce::MouseEvent& e)
{
    const bool nowLamp = overLamp (e.position);
    if (nowLamp != hotLamp || ! hot)
    {
        hotLamp = nowLamp;
        hot = true;
        repaint();
    }
}

void RangeTab::mouseExit (const juce::MouseEvent&)
{
    hot = hotLamp = false;
    repaint();
}

void RangeTab::toggle()
{
    attachment.setValueAsCompleteGesture (on ? 0.0f : 1.0f);
}

void RangeTab::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || e.getDistanceFromDragStart() > 6 || ! getLocalBounds().toFloat().contains (e.position))
        return;

    if (overLamp (e.position))
        toggle();
    else if (onSelect != nullptr)
        onSelect();
}

bool RangeTab::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey)
    {
        if (onSelect != nullptr)
            onSelect();
        return true;
    }
    if (key == juce::KeyPress::spaceKey)
    {
        toggle();
        return true;
    }
    return false;
}

std::unique_ptr<juce::AccessibilityHandler> RangeTab::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::button,
        juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [this] { if (onSelect != nullptr) onSelect(); })
                                    .addAction (juce::AccessibilityActionType::toggle, [this] { toggle(); }));
}

//============================================================================== RangeShelf
/** A bar for a range's attack or release. While the range is locked it is greyed, cannot be moved,
    and shows what Attack or Release comes to. */
class RangeShelf::TimingBar : public juce::Slider
{
public:
    TimingBar()
    {
        setSliderStyle (juce::Slider::LinearBar);
        setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 20);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        getProperties().set ("toyColour", (juce::int64) toy::paper.getARGB());
    }

    /** After the attachment is made (it sets the slider's own text function): wraps that function
        so that a locked bar reads the global value. */
    void adoptTextFunction()
    {
        ownText = textFromValueFunction;
        textFromValueFunction = [this] (double v)
        {
            if (locked)
                return lockedText;
            return ownText != nullptr ? ownText (v) : milliseconds (v);
        };
        updateText();
    }

    void showLocked (bool isLocked, double globalValue)
    {
        const auto text = milliseconds (globalValue);
        if (isLocked == locked && (! isLocked || text == lockedText))
            return;

        locked = isLocked;
        lockedText = text;
        setEnabled (! locked);
        if (locked)
            getProperties().set ("shownProportion", valueToProportionOfLength (juce::jlimit (getMinimum(), getMaximum(), globalValue)));
        else
            getProperties().remove ("shownProportion");
        updateText();
        repaint();
    }

private:
    std::function<juce::String (double)> ownText;
    juce::String lockedText;
    bool locked = false;
};

/** The lock on a range's timing: a pill with a padlock. Shut, the range follows Attack and Release. */
class RangeShelf::LockButton : public juce::Button
{
public:
    explicit LockButton (const Theme::Fonts& f) : juce::Button ("Timing lock"), fonts (f)
    {
        setClickingTogglesState (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        const bool locked = getToggleState();
        const auto r = Toy::body (getLocalBounds());
        // Shut, it is dark, like any button that is on; open, it is paper.
        const auto fill = locked ? toy::ink : toy::paper;
        const auto mark = locked ? toy::paper : toy::ink;
        Toy::pill (g, r, fill, false, isMouseOver || isButtonDown ? toy::outline + 0.5f : toy::outline);
        if (! locked)
            if (auto* look = dynamic_cast<DeHonkLookAndFeel*> (&getLookAndFeel()))
                look->drawFocus (g, *this, r, 100.0f);

        // The padlock: a body and a shackle, the shackle lifted clear of the body when unlocked.
        const float cx = r.getX() + r.getHeight() * 0.5f + 6.0f, cy = r.getCentreY() + 1.0f;
        const juce::Rectangle<float> body (cx - 5.0f, cy - 1.0f, 10.0f, 7.5f);
        juce::Path shackle;
        const float lift = locked ? 0.0f : 3.0f;
        shackle.startNewSubPath (cx - 3.2f, cy - 1.0f);
        shackle.lineTo (cx - 3.2f, cy - 3.0f - lift);
        shackle.addCentredArc (cx, cy - 3.0f - lift, 3.2f, 3.4f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, false);
        shackle.lineTo (cx + 3.2f, cy - (locked ? 1.0f : 2.2f + lift * 0.4f));
        g.setColour (mark);
        g.strokePath (shackle, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.fillRoundedRectangle (body, 2.0f);

        g.setFont (fonts.name (10.5f));
        const float left = cx + 11.0f;
        g.drawText (locked ? "locked" : "unlocked", juce::Rectangle<float> (left, r.getY(), r.getRight() - left - 6.0f, r.getHeight()),
                    juce::Justification::centredLeft, false);
    }

private:
    const Theme::Fonts& fonts;
};

RangeShelf::RangeShelf (juce::AudioProcessorValueTreeState& state, const Theme::Fonts& f) : apvts (state), fonts (f)
{
    for (int m = 0; m <= kNumModules; ++m)
    {
        const bool snap = m == kNumModules;
        auto* param = apvts.getParameter (snap ? ParamIDs::snapOn : ParamIDs::moduleOn[m]);
        jassert (param != nullptr);
        auto& tab = tabs[(size_t) m];
        tab = std::make_unique<RangeTab> (snap ? "Snap" : ParamRanges::moduleNames[m], snap ? toy::click : toy::module[m], *param);
        tab->onSelect = [this, m] { select (selectedRange == m ? -1 : m); };
        addAndMakeVisible (*tab);
    }
    shownEdges.fill (-1.0f);
    updateRangeTexts();

    lock = std::make_unique<LockButton> (fonts);
    lock->setTooltip ("Locked: this range goes in and lets go as Attack and Release say, like every other locked range. "
                      "Unlocked: it uses its own attack and release, set with the two bars beside this.");
    attack = std::make_unique<TimingBar>();
    attack->setTooltip ("This range's own attack: how quickly a cut goes in here, from 2 to 100 ms. "
                        "While the range is locked it shows what Attack is set to and cannot be moved.");
    release = std::make_unique<TimingBar>();
    release->setTooltip ("This range's own release: how slowly a cut lets go here, from 50 ms to 1.5 s. "
                         "While the range is locked it shows Release and cannot be moved.");
    for (auto* c : std::initializer_list<juce::Component*> { lock.get(), attack.get(), release.get() })
        addChildComponent (c);

    for (auto [label, text] : { std::pair<juce::Label*, const char*> { &attackCaption, "attack" }, { &releaseCaption, "release" } })
    {
        label->setText (text, juce::dontSendNotification);
        label->setFont (fonts.name (11.0f));
        label->setColour (juce::Label::textColourId, toy::ink);
        label->setJustificationType (juce::Justification::centredRight);
        label->setInterceptsMouseClicks (false, false);
        addChildComponent (label);
    }

    showControlsFor (-1);
}

RangeShelf::~RangeShelf() = default;

void RangeShelf::select (int range)
{
    range = juce::jlimit (-1, (int) kNumModules, range);
    if (range == selectedRange)
        return;

    selectedRange = range;
    for (int m = 0; m <= kNumModules; ++m)
        tabs[(size_t) m]->setSelected (m == range);
    showControlsFor (range);
    repaint();
    if (onSelectionChanged != nullptr)
        onSelectionChanged (range);
}

void RangeShelf::setCutting (const std::array<bool, kNumModules + 1>& cutting)
{
    for (size_t m = 0; m < tabs.size(); ++m)
        tabs[m]->setCutting (cutting[m]);
}

void RangeShelf::showControlsFor (int range)
{
    lockAttachment.reset();
    attackAttachment.reset();
    releaseAttachment.reset();

    const bool timed = range >= 0 && range < kNumModules;
    for (auto* c : std::initializer_list<juce::Component*> { lock.get(), attack.get(), release.get(), &attackCaption, &releaseCaption })
        c->setVisible (timed);

    if (timed)
    {
        using State = juce::AudioProcessorValueTreeState;
        lockAttachment = std::make_unique<State::ButtonAttachment> (apvts, ParamIDs::timingLocked[range], *lock);
        attackAttachment = std::make_unique<State::SliderAttachment> (apvts, ParamIDs::rangeAttack[range], *attack);
        releaseAttachment = std::make_unique<State::SliderAttachment> (apvts, ParamIDs::rangeRelease[range], *release);
        attack->adoptTextFunction();
        release->adoptTextFunction();
        for (auto [bar, id] : { std::pair<TimingBar*, const char*> { attack.get(), ParamIDs::rangeAttack[range] },
                                { release.get(), ParamIDs::rangeRelease[range] } })
            if (auto* param = apvts.getParameter (id))
                bar->setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
        shownLocked = ! lock->getToggleState();   // so that refresh() sets both bars
        refresh();
    }
    resized();
}

double RangeShelf::globalAttackMs() const
{
    return ParamRanges::attackMsForSpeed (apvts.getRawParameterValue (ParamIDs::speed)->load());
}

void RangeShelf::updateRangeTexts()
{
    // Each tab's frequencies, and its tooltip with them, as the edges are now (as the core uses
    // them). Only when an edge has moved.
    std::array<float, kNumEdges> raw {};
    for (int e = 0; e < kNumEdges; ++e)
        raw[(size_t) e] = apvts.getRawParameterValue (ParamIDs::edges[e])->load();
    auto used = ParamRanges::edgesAsUsed (raw);
    for (auto& hz : used)
        hz = (float) juce::roundToInt (hz);
    if (used == shownEdges)
        return;
    shownEdges = used;

    static const char* const tips[kNumModules + 1]
    {
        "Woof: the low end booming on every hit",
        "Tub: tubby weight just above the bass",
        "Fog: low mids clouding over when music and effects stack up",
        "Bark: hard, shouty midrange",
        "Snap: sharp clicks and pops around 2.5 kHz. Takes the edge off a pop that the rest of the sound does not share, "
        "and leaves drum hits and steady sounds alone. Works at any level.",
    };
    static const char* const after[kNumModules] { ".", ". This is where added bass harmonics pile up first.", ".", "." };

    for (int m = 0; m <= kNumModules; ++m)
    {
        const bool snap = m == kNumModules;
        auto& tab = *tabs[(size_t) m];
        juce::String tip (tips[m]);
        if (snap)
        {
            tab.setRangeText ("clicks, 2.5 kHz");
        }
        else
        {
            const float lo = used[(size_t) m], hi = used[(size_t) m + 1];
            tab.setRangeText (hzRange (lo, hi, "-"));
            tip << ", " << hzRange (lo, hi, " to ") << after[m]
                << " The handles between the ranges in the band view move its edges.";
        }
        tab.setTooltip (tip + " Its lamp switches it on or off; the dot is red while it is cutting. "
                        "Click the name to work on this range alone"
                        + (snap ? "." : ", with its own attack and release underneath.") + " Click again for all of them.");
    }
}

double RangeShelf::globalReleaseMs() const
{
    return (double) apvts.getRawParameterValue (ParamIDs::release)->load();
}

void RangeShelf::refresh()
{
    updateRangeTexts();
    if (selectedRange < 0 || selectedRange >= kNumModules)
        return;

    const bool locked = lock->getToggleState();
    attack->showLocked (locked, globalAttackMs());
    release->showLocked (locked, globalReleaseMs());
    if (locked != shownLocked)
    {
        shownLocked = locked;
        attackCaption.setColour (juce::Label::textColourId, toy::ink.withAlpha (locked ? 0.55f : 1.0f));
        releaseCaption.setColour (juce::Label::textColourId, toy::ink.withAlpha (locked ? 0.55f : 1.0f));
        repaint();
    }
}

juce::Rectangle<int> RangeShelf::drawerBounds() const noexcept
{
    return getLocalBounds().withTrimmedTop (tabHeight);
}

void RangeShelf::resized()
{
    // Five tabs of one width along the shelf.
    constexpr int gap = 8, inset = 10;
    auto row = getLocalBounds().withHeight (tabHeight + 1).reduced (inset, 0);
    const int n = (int) tabs.size();
    const int w = (row.getWidth() - (n - 1) * gap) / n;
    for (int m = 0; m < n; ++m)
    {
        tabs[(size_t) m]->setBounds (m + 1 < n ? row.removeFromLeft (w) : row);
        row.removeFromLeft (gap);
    }

    // The drawer: the lock, then attack and release, each a name with its bar.
    auto inner = drawerBounds().reduced (12, 5);
    lock->setBounds (inner.removeFromLeft (118));
    inner.removeFromLeft (14);
    const int captionA = juce::roundToInt (std::ceil (textWidth (attackCaption.getFont(), attackCaption.getText()))) + 8;
    const int captionR = juce::roundToInt (std::ceil (textWidth (releaseCaption.getFont(), releaseCaption.getText()))) + 8;
    const int barWidth = juce::jmax (60, (inner.getWidth() - captionA - captionR - 6 - 6 - 14) / 2);
    attackCaption.setBounds (inner.removeFromLeft (captionA));
    inner.removeFromLeft (6);
    attack->setBounds (inner.removeFromLeft (barWidth));
    inner.removeFromLeft (14);
    releaseCaption.setBounds (inner.removeFromLeft (captionR));
    inner.removeFromLeft (6);
    release->setBounds (inner);
}

void RangeShelf::paint (juce::Graphics& g)
{
    // The drawer under the shelf line. Its top edge is the line the tabs stand on; the selected
    // tab is drawn over that line, so the two are one shape.
    const float line = toy::outline;
    const auto drawer = drawerBounds().toFloat().reduced (line * 0.5f, 0.0f).withTrimmedBottom (line * 0.5f).translated (0.0f, -line * 0.5f);
    const bool any = selectedRange >= 0;
    const auto fill = any ? tabs[(size_t) selectedRange]->fillColour() : toy::paper.withAlpha (0.55f);

    juce::Path shape;
    const float corner = toy::cardCorner;
    shape.startNewSubPath (drawer.getX(), drawer.getY());
    shape.lineTo (drawer.getRight(), drawer.getY());
    shape.lineTo (drawer.getRight(), drawer.getBottom() - corner);
    shape.quadraticTo (drawer.getRight(), drawer.getBottom(), drawer.getRight() - corner, drawer.getBottom());
    shape.lineTo (drawer.getX() + corner, drawer.getBottom());
    shape.quadraticTo (drawer.getX(), drawer.getBottom(), drawer.getX(), drawer.getBottom() - corner);
    shape.closeSubPath();
    g.setColour (fill);
    g.fillPath (shape);
    g.setColour (toy::ink);
    g.strokePath (shape, juce::PathStrokeType (line));

    const auto textArea = drawerBounds().reduced (16, 4).toFloat();
    if (! any)
    {
        g.setFont (fonts.body (11.5f));
        g.setColour (toy::ink.withAlpha (0.62f));
        g.drawFittedText ("click a range's name to work on it alone and set its own attack and release. its lamp switches it on and off.",
                          textArea.toNearestInt(), juce::Justification::centredLeft, 2, 0.85f);
    }
    else if (selectedRange == kNumModules)
    {
        g.setFont (fonts.body (11.5f));
        g.setColour (toy::ink.withAlpha (0.8f));
        g.drawFittedText ("snap works on single clicks, each over in a few milliseconds, so it has no attack or release to set.",
                          textArea.toNearestInt(), juce::Justification::centredLeft, 2, 0.85f);
    }
}

void RangeShelf::mouseUp (const juce::MouseEvent& e)
{
    // Empty shelf (between the tabs, or beside them) clears the selection. The drawer does not:
    // a missed click between its controls should not close it.
    if (e.position.y < (float) tabHeight && ! e.mouseWasDraggedSinceMouseDown())
        select (-1);
}

//============================================================================== LinkSelector
LinkSelector::LinkSelector (juce::RangedAudioParameter& linkMode, const Theme::Fonts& f)
    : fonts (f),
      attachment (linkMode, [this] (float v)
                  {
                      const int now = juce::jlimit (0, ParamRanges::numLinkModes - 1, juce::roundToInt (v));
                      if (now != mode)
                      {
                          mode = now;
                          repaint();
                      }
                  })
{
    setTitle ("Link");
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    attachment.sendInitialUpdate();
}

void LinkSelector::setGroupedShown (bool shouldShowGrouped)
{
    if (groupedShown != shouldShowGrouped)
    {
        groupedShown = shouldShowGrouped;
        hotSegment = -1;
        repaint();
    }
}

int LinkSelector::shownSegment() const noexcept
{
    if (groupedShown)
        return mode;
    return mode == 2 ? 1 : 0;      // Grouped is Linked while there are no groups
}

int LinkSelector::segmentAt (juce::Point<float> at) const noexcept
{
    const auto r = Toy::body (getLocalBounds());
    if (! r.contains (at))
        return -1;
    return juce::jlimit (0, numShown() - 1, (int) ((at.x - r.getX()) / r.getWidth() * (float) numShown()));
}

void LinkSelector::choose (int newMode)
{
    if (newMode != mode)
        attachment.setValueAsCompleteGesture ((float) newMode);
}

void LinkSelector::paint (juce::Graphics& g)
{
    const auto r = Toy::body (getLocalBounds());
    const float corner = r.getHeight() * 0.5f;
    const int n = numShown(), chosen = shownSegment();
    const float w = r.getWidth() / (float) n;

    g.setColour (toy::paper);
    g.fillRoundedRectangle (r, corner);
    {
        juce::Path clip;
        clip.addRoundedRectangle (r, corner);
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (clip);
        for (int s = 0; s < n; ++s)
        {
            const auto seg = juce::Rectangle<float> (r.getX() + w * (float) s, r.getY(), w, r.getHeight());
            if (s == chosen)
            {
                g.setColour (toy::sunYellow);
                g.fillRect (seg);
            }
            else if (s == hotSegment)
            {
                g.setColour (toy::sunYellow.withAlpha (0.28f));
                g.fillRect (seg);
            }
        }
    }
    g.setColour (toy::ink);
    for (int s = 1; s < n; ++s)
        g.fillRect (juce::Rectangle<float> (r.getX() + w * (float) s - toy::outline * 0.5f, r.getY(), toy::outline, r.getHeight()));
    g.drawRoundedRectangle (r, corner, toy::outline);

    const auto font = fonts.value (juce::jlimit (9.5f, 12.0f, w / 6.4f));
    g.setFont (font);
    for (int s = 0; s < n; ++s)
    {
        const auto seg = juce::Rectangle<float> (r.getX() + w * (float) s, r.getY(), w, r.getHeight());
        g.setColour (toy::ink.withAlpha (s == chosen ? 1.0f : 0.7f));
        g.drawFittedText (juce::String (ParamRanges::linkModeNames[modeOfSegment (s)]).toLowerCase(),
                          seg.toNearestInt().reduced (s == 0 || s == n - 1 ? 5 : 3, 0), juce::Justification::centred, 1, 0.8f);
    }

    if (auto* look = dynamic_cast<DeHonkLookAndFeel*> (&getLookAndFeel()))
        look->drawFocus (g, *this, r, 100.0f);
}

void LinkSelector::mouseMove (const juce::MouseEvent& e)
{
    const int now = segmentAt (e.position);
    if (now != hotSegment)
    {
        hotSegment = now;
        repaint();
    }
}

void LinkSelector::mouseExit (const juce::MouseEvent&)
{
    if (hotSegment >= 0)
    {
        hotSegment = -1;
        repaint();
    }
}

void LinkSelector::mouseUp (const juce::MouseEvent& e)
{
    const int segment = segmentAt (e.position);
    if (segment >= 0 && ! e.mods.isPopupMenu())
        choose (modeOfSegment (segment));
}

bool LinkSelector::keyPressed (const juce::KeyPress& key)
{
    const int step = key == juce::KeyPress::leftKey || key == juce::KeyPress::downKey ? -1
                   : key == juce::KeyPress::rightKey || key == juce::KeyPress::upKey ? 1 : 0;
    if (step == 0)
        return false;
    choose (modeOfSegment (juce::jlimit (0, numShown() - 1, shownSegment() + step)));
    return true;
}

//============================================================================== TitleBlock
TitleBlock::TitleBlock (const Theme::Fonts& f) : fonts (f)
{
    setTitle ("De-Honk");
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

juce::Rectangle<float> TitleBlock::nameArea() const noexcept
{
    return { 2.0f, 0.0f, textWidth (fonts.title (22.0f), "de-honk") + 8.0f, juce::jmin (36.0f, (float) getHeight()) };
}

juce::Rectangle<float> TitleBlock::siteArea() const noexcept
{
    const float x = nameArea().getRight() + 10.0f;
    return { x, 18.0f, juce::jmin ((float) getWidth() - x, textWidth (fonts.body (9.5f), siteText) + 6.0f), 14.0f };
}

int TitleBlock::partAt (juce::Point<float> at) const noexcept
{
    if (siteArea().expanded (2.0f, 3.0f).contains (at))
        return 1;
    if (at.x < siteArea().getX() || at.y < siteArea().getY() - 3.0f)
        return 0;
    return -1;
}

juce::String TitleBlock::getTooltip()
{
    return hot == 1 ? juce::String ("Opens sweetjusticesound.com in your browser.")
                    : juce::String ("De-Honk by Samuel Justice. Click for the About page: the version, the links, and a check for a newer one.");
}

void TitleBlock::paint (juce::Graphics& g)
{
    const auto name = nameArea();
    g.setColour (toy::ink);
    g.setFont (fonts.title (22.0f));
    g.drawText ("de-honk", name.withTrimmedLeft (2.0f).withHeight (26.0f), juce::Justification::centredLeft, false);

    // The little rainbow under the name: it grows a stripe taller under the pointer.
    const float stripe = hot == 0 ? 2.4f : 2.0f;
    for (int i = 0; i < 5; ++i)
    {
        g.setColour (toy::rainbow[i]);
        g.fillRect (juce::Rectangle<float> (name.getX() + 2.0f, 25.0f + (float) i * stripe, name.getWidth() - 8.0f, stripe));
    }

    const auto site = siteArea();
    g.setFont (fonts.name (9.5f));
    g.setColour (toy::ink.withAlpha (hot == 0 ? 1.0f : 0.75f));
    g.drawText ("by sammy j", juce::Rectangle<float> (site.getX(), 4.0f, (float) getWidth() - site.getX(), 13.0f), juce::Justification::centredLeft, false);

    g.setFont (fonts.body (9.5f));
    g.setColour (toy::ink.withAlpha (hot == 1 ? 1.0f : 0.7f));
    g.drawText (siteText, site, juce::Justification::centredLeft, false);
    if (hot == 1)
        g.fillRect (juce::Rectangle<float> (site.getX(), site.getBottom() - 1.5f, site.getWidth() - 6.0f, 1.0f));
}

void TitleBlock::mouseMove (const juce::MouseEvent& e)
{
    const int now = partAt (e.position);
    if (now != hot)
    {
        hot = now;
        setMouseCursor (hot >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void TitleBlock::mouseExit (const juce::MouseEvent&)
{
    if (hot >= 0)
    {
        hot = -1;
        repaint();
    }
}

void TitleBlock::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || e.mouseWasDraggedSinceMouseDown())
        return;
    const int part = partAt (e.position);
    if (part == 0 && onAbout != nullptr)
        onAbout();
    else if (part == 1 && onSite != nullptr)
        onSite();
}

//============================================================================== ToyTooltip
namespace
{
    constexpr int tipMargin = 12;      // room round the card for the bounce
    constexpr float tipPadX = 12.0f, tipPadY = 9.0f;

    /** Past the end and back: the small bounce the card opens with. */
    float easeOutBack (float t) noexcept
    {
        constexpr float c1 = 1.70158f, c3 = c1 + 1.0f;
        const float u = t - 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }
}

ToyTooltip::ToyTooltip (juce::Component& o, const Theme::Fonts& f) : owner (o), fonts (f)
{
    setInterceptsMouseClicks (false, false);
    setAlwaysOnTop (true);
    setAccessible (false);
    owner.addChildComponent (this);
    startTimerHz (30);
}

ToyTooltip::~ToyTooltip()
{
    stopTimer();
}

juce::TextLayout ToyTooltip::layoutFor (const juce::String& text) const
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::topLeft);
    s.setLineSpacing (2.0f);
    s.append (text, fonts.body (12.0f), toy::ink);
    juce::TextLayout layout;
    // Short tips are set in lines of even length (a neat small card); one that would run past
    // five lines that way takes the full width, so that it is wide and low and hides less.
    const float width = juce::jmin (300.0f, (float) owner.getWidth() - 60.0f);
    layout.createLayoutWithBalancedLineLengths (s, width);
    if (layout.getNumLines() > 5)
        layout.createLayout (s, juce::jmin (340.0f, (float) owner.getWidth() - 60.0f));
    return layout;
}

juce::Component* ToyTooltip::tipOwner (juce::Component* under, juce::String& text)
{
    for (auto* c = under; c != nullptr; c = c->getParentComponent())
    {
        if (auto* client = dynamic_cast<juce::TooltipClient*> (c))
        {
            text = client->getTooltip();
            if (text.isNotEmpty())
                return c;
        }
    }
    text.clear();
    return nullptr;
}

void ToyTooltip::hideNow()
{
    if (open)
    {
        open = false;
        setVisible (false);
        startTimerHz (30);
    }
    shownFor = nullptr;
}

void ToyTooltip::showNow (juce::Component& target, juce::Point<int> at)
{
    juce::String text;
    if (auto* c = tipOwner (&target, text))
    {
        openFor (*c, text, at, false);
        suppressed = true;      // it stays until hideNow(): the timer leaves it alone
    }
}

void ToyTooltip::openFor (juce::Component& target, const juce::String& text, juce::Point<int> at, bool animate)
{
    const auto layout = layoutFor (text);
    const int w = (int) std::ceil (layout.getWidth() + 2.0f * tipPadX), h = (int) std::ceil (layout.getHeight() + 2.0f * tipPadY);
    const auto area = owner.getLocalBounds().reduced (6);
    // The control is everything that shares this text: a knob and its name, a switch, its name
    // and its bar. The tip keeps clear of all of it.
    auto control = owner.getLocalArea (&target, target.getLocalBounds());
    {
        std::function<void (juce::Component&)> gather = [&] (juce::Component& c)
        {
            for (auto* child : c.getChildren())
            {
                if (child == this || ! child->isVisible())
                    continue;
                if (auto* client = dynamic_cast<juce::TooltipClient*> (child); client != nullptr && child != &target && client->getTooltip() == text)
                    control = control.getUnion (owner.getLocalArea (child, child->getLocalBounds()));
                gather (*child);
            }
        };
        if (target.getParentComponent() != nullptr)
            gather (*target.getParentComponent());
    }
    constexpr int gap = 8;

    // Beside the control, never over it: under it, over it, to its right, to its left, whichever
    // has the room, kept as near the pointer as the window allows.
    const int alongX = juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - w), at.x - w / 3);
    const int alongY = juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - h), at.y - h / 2);
    juce::Rectangle<int> place;
    const bool big = control.getHeight() > 150 && control.getWidth() > 400;     // the pictures: nothing in the window is beside them
    if (! big)
    {
        const juce::Rectangle<int> candidates[] { { alongX, control.getBottom() + gap, w, h },
                                                  { alongX, control.getY() - gap - h, w, h },
                                                  { control.getRight() + gap, alongY, w, h },
                                                  { control.getX() - gap - w, alongY, w, h } };
        for (const auto& c : candidates)
        {
            if (area.contains (c))
            {
                place = c;
                break;
            }
        }
    }
    if (place.isEmpty())
    {
        // Clear of the pointer at least: below and to the right of it, or the other way if there is no room.
        const int x = at.x + 18 + w <= area.getRight() ? at.x + 18 : at.x - 18 - w;
        const int y = at.y + 20 + h <= area.getBottom() ? at.y + 20 : at.y - 14 - h;
        place = juce::Rectangle<int> (x, y, w, h).constrainedWithin (area);
    }

    card = place;
    origin = card.toFloat().getConstrainedPoint (at.toFloat());
    shownFor = &target;
    shownText = text;
    shownAt = at;
    openedAt = juce::Time::getMillisecondCounter();
    grown = animate ? 0.0f : 1.0f;
    open = true;
    setBounds (card.expanded (tipMargin));
    setVisible (true);
    toFront (false);
    repaint();
    if (animate)
        startTimerHz (60);
}

void ToyTooltip::timerCallback()
{
    auto& desktop = juce::Desktop::getInstance();
    const auto source = desktop.getMainMouseSource();
    const auto screen = source.getScreenPosition().roundToInt();
    const auto at = owner.getLocalPoint (nullptr, screen);
    const bool buttonDown = source.getCurrentModifiers().isAnyMouseButtonDown();
    const auto now = juce::Time::getMillisecondCounter();

    if (open && grown < 1.0f)
    {
        grown = juce::jlimit (0.0f, 1.0f, (float) (now - openedAt) / (float) growMs);
        repaint();
        if (grown >= 1.0f)
            startTimerHz (30);
    }

    auto* under = source.getComponentUnderMouse();
    if (under == this || (under != nullptr && ! (under == &owner || owner.isParentOf (under))))
        under = nullptr;

    juce::String text;
    auto* target = tipOwner (under, text);

    if (suppressed && open)
        return;     // shown by showNow(): stays until hideNow()

    if (buttonDown)
    {
        // A click closes it at once, and it does not come back until the pointer has moved on.
        hideNow();
        suppressed = true;
        restingOn = nullptr;
        restingAt = at;
        return;
    }

    if (open)
    {
        // Gone as soon as the pointer leaves the control, or wanders well away from where it rested.
        if (target != shownFor.getComponent() || text != shownText || at.getDistanceFrom (shownAt) > 36)
        {
            hideNow();
            restingOn = nullptr;
        }
        return;
    }

    if (suppressed)
    {
        if (at.getDistanceFrom (restingAt) > 3)
            suppressed = false;
        else
            return;
    }

    if (target == nullptr)
    {
        restingOn = nullptr;
        return;
    }

    if (target != restingOn.getComponent() || text != restingText || at.getDistanceFrom (restingAt) > 3)
    {
        // The wait starts again each time the pointer moves: it has to come to rest.
        restingOn = target;
        restingText = text;
        restingAt = at;
        restingSince = now;
        return;
    }

    if (now - restingSince >= (juce::uint32) delayMs)
        openFor (*target, text, at, true);
}

void ToyTooltip::paint (juce::Graphics& g)
{
    if (! open)
        return;

    // Grown from the pointer: the card is drawn scaled about the point on its edge nearest to
    // where the pointer rested, a little past full size and back.
    const auto local = card.toFloat().translated ((float) -getX(), (float) -getY());
    const auto from = origin.translated ((float) -getX(), (float) -getY());
    // The bounce goes a little past full size: never by more than the room round the card, so
    // that the far edge of a large card is not cut off.
    const float most = 1.0f + ((float) tipMargin - 2.0f) / juce::jmax (40.0f, local.getWidth(), local.getHeight());
    const float scale = grown >= 1.0f ? 1.0f : juce::jlimit (0.05f, most, 0.35f + 0.65f * easeOutBack (grown));
    const float alpha = juce::jlimit (0.0f, 1.0f, grown * 3.0f);

    juce::Graphics::ScopedSaveState state (g);
    g.addTransform (juce::AffineTransform::scale (scale, scale, from.x, from.y));
    g.setOpacity (alpha);

    const auto r = local.reduced (toy::outline * 0.5f);
    g.setColour (toy::paper.withAlpha (alpha));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (toy::ink.withAlpha (alpha));
    g.drawRoundedRectangle (r, 8.0f, toy::outline);
    // A mustard pin on the edge where the pointer rested: what it grew from.
    const auto pin = juce::Rectangle<float> (7.0f, 7.0f).withCentre (r.getConstrainedPoint (from));
    g.setColour (toy::sunYellow.withAlpha (alpha));
    g.fillEllipse (pin);
    g.setColour (toy::ink.withAlpha (alpha));
    g.drawEllipse (pin, 1.0f);

    if (grown > 0.45f)
        layoutFor (shownText).draw (g, local.reduced (tipPadX, tipPadY));
}

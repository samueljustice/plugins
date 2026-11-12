#include "MorphPad.h"

#include <cmath>

namespace
{
    const char* const c_modeNames[MorphPad::c_numModes] = { "Blend", "Glide", "Cross" };
    const char* const c_topLabels[MorphPad::c_numModes] = { "Fuse", "Shape", "A thru B" };
    const char* const c_bottomLabels[MorphPad::c_numModes] = { "Mix", "Move", "B thru A" };
    const char* const c_modeTips[MorphPad::c_numModes] = {
        "Blend\nMorphs the tone of bus A into bus B's: left is A, right is B. Higher up the pad fuses the two into one texture.",
        "Glide\nThe pitches inside the sound slide from A's to B's as you move across. Higher up keeps the character (formants) in place while the pitch moves.",
        "Cross\nA vocoder: towards the top plays A through B's tone, towards the bottom plays B through A's.",
    };
}

//==============================================================================
/** Transparent, click-through layer over the whole editor that draws the rope while dragging. */
class MorphPad::RopeOverlay : public juce::Component
{
public:
    explicit RopeOverlay(MorphPad& o) : owner(o)
    {
        setInterceptsMouseClicks(false, false);
        setOpaque(false);
    }

    juce::Path makeRope() const
    {
        juce::Path p;
        if (owner.m_ropeSlot < 0)
            return p;

        const auto startScreen = owner.localPointToGlobal(owner.m_handles[static_cast<size_t>(owner.m_ropeSlot)].getCentre());
        const auto start = getLocalPoint(nullptr, startScreen);
        const auto end = getLocalPoint(nullptr, owner.m_ropeEndScreen);
        const auto mid = (start + end) * 0.5f;
        const auto ctrl = mid + owner.m_ropeOffset * 2.0f; // the curve's midpoint sits at mid + offset
        p.startNewSubPath(start);
        p.quadraticTo(ctrl, end);
        return p;
    }

    void update()
    {
        const auto now = makeRope().getBounds().expanded(16.0f).getSmallestIntegerContainer();
        repaint(lastArea.getUnion(now));
        lastArea = now;
    }

    void paint(juce::Graphics& g) override
    {
        if (owner.m_ropeSlot < 0)
            return;

        const auto rope = makeRope();
        const auto colour = owner.laneColour(owner.m_ropeSlot);

        g.setColour(Theme::Colours::shadow);
        g.strokePath(rope, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                     juce::AffineTransform::translation(0.0f, 2.0f));
        g.setColour(colour.darker(0.35f));
        g.strokePath(rope, juce::PathStrokeType(3.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Twist: short light dashes along the rope.
        juce::Path twist;
        const float dashes[] = { 3.0f, 4.0f };
        juce::PathStrokeType(1.4f).createDashedStroke(twist, rope, dashes, 2);
        g.setColour(colour.brighter(0.4f).withAlpha(0.85f));
        g.fillPath(twist);

        // The loose end: a ring, filled with the colour of the lane it would land on.
        const auto end = getLocalPoint(nullptr, owner.m_ropeEndScreen);
        const auto ring = juce::Rectangle<float>(18.0f, 18.0f).withCentre(end);
        const int lane = owner.m_ropeHoverLane;
        g.setColour(lane >= 0 ? Theme::trackColour(lane) : Theme::Colours::panelRaised);
        g.fillEllipse(ring);
        g.setColour(colour);
        g.drawEllipse(ring, 2.0f);
        g.setColour(lane >= 0 ? Theme::Colours::background : Theme::Colours::text);
        g.setFont(owner.m_fonts.heading(11.0f));
        g.drawText(owner.m_ropeSlot == 0 ? "A" : "B", ring, juce::Justification::centred, false);
    }

    MorphPad& owner;
    juce::Rectangle<int> lastArea;
};

//==============================================================================
/** Wander toggle: a plain play / stop button. */
class MorphPad::WanderButton : public juce::Button
{
public:
    WanderButton() : juce::Button("Wander")
    {
        setClickingTogglesState(true);
        setTooltip("Wander\nPlay: the dot wanders along the chosen shape by itself, so the morph keeps moving, with this window closed and in a bounce too. Stop: it stays where the wander left it.");
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced(0.5f);
        const bool on = getToggleState();
        const auto accent = Theme::Colours::accent;

        g.setColour(on ? accent.withAlpha(0.3f) : (over || down ? Theme::Colours::hover : Theme::Colours::panelRaised));
        g.fillRoundedRectangle(b, 4.0f);
        g.setColour(on ? accent : Theme::Colours::outline);
        g.drawRoundedRectangle(b, 4.0f, 1.0f);

        const auto c = b.getCentre();
        const float r = juce::jmin(b.getWidth(), b.getHeight()) * 0.26f;
        g.setColour(on ? accent : Theme::Colours::textDim);
        if (on)
        {
            g.fillRect(juce::Rectangle<float>(r * 1.7f, r * 1.7f).withCentre(c)); // stop
        }
        else
        {
            juce::Path play;
            play.addTriangle(c.x - r * 0.75f, c.y - r, c.x - r * 0.75f, c.y + r, c.x + r, c.y);
            g.fillPath(play);
        }
    }
};

//==============================================================================
MorphPad::MorphPad(const Theme::Fonts& fonts) : m_fonts(fonts)
{
    for (int i = 0; i < c_numModes; ++i)
    {
        auto& b = m_modeButtons[static_cast<size_t>(i)];
        b.setButtonText(c_modeNames[i]);
        b.setTooltip(c_modeTips[i]);
        b.setClickingTogglesState(true);
        b.setRadioGroupId(0x6d6f7270); // "morp"
        b.setConnectedEdges((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < c_numModes - 1 ? juce::Button::ConnectedOnRight : 0));
        b.setToggleState(i == m_mode, juce::dontSendNotification);
        b.onClick = [this, i]
        {
            if (! m_modeButtons[static_cast<size_t>(i)].getToggleState() || m_mode == i)
                return;
            m_mode = i;
            repaint();
            if (onModeChanged) onModeChanged(i);
        };
        addAndMakeVisible(b);
    }

    m_enable.setClickingTogglesState(true);
    m_enable.setColour(juce::TextButton::buttonOnColourId, Theme::Colours::go); // green when on, greyed when off
    m_enable.setTooltip("Morph On / Off\nOn (green): buses A and B are morphed together, as the dot says. Off: they play side by side as usual and the morph rests.");
    m_enable.onClick = [this]
    {
        applyEnabledLook(m_enable.getToggleState());
        if (onEnableChanged) onEnableChanged(m_enable.getToggleState());
    };
    addAndMakeVisible(m_enable);

    m_wanderButton = std::make_unique<WanderButton>();
    m_wanderButton->onClick = [this]
    {
        const bool on = m_wanderButton->getToggleState();
        setWander(on, static_cast<float>(m_speed.getValue()));
        if (onWanderChanged) onWanderChanged(on, static_cast<float>(m_speed.getValue()));
    };
    addAndMakeVisible(*m_wanderButton);

    m_speed.setSliderStyle(juce::Slider::LinearHorizontal);
    m_speed.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_speed.setRange(0.0, 1.0);
    m_speed.setValue(0.5, juce::dontSendNotification);
    m_speed.setDoubleClickReturnValue(true, 0.5);
    m_speed.setTooltip("Wander Speed\nHow fast the dot travels along its shape.\nDouble-click to reset it.");
    m_speed.onValueChange = [this]
    {
        if (onWanderChanged) onWanderChanged(m_wander, static_cast<float>(m_speed.getValue()));
    };
    addAndMakeVisible(m_speed);

    m_shapeBox.addItemList(shapeNames(), 1);
    m_shapeBox.setSelectedItemIndex(0, juce::dontSendNotification);
    m_shapeBox.setTooltip("Wander Shape\nThe path the dot follows when it wanders.");
    m_shapeBox.onChange = [this]
    {
        setShape(m_shapeBox.getSelectedItemIndex());
        if (onShapeChanged) onShapeChanged(m_shape);
    };
    addAndMakeVisible(m_shapeBox);

    MorphWander::prepareTables();
    m_enabled = true;
    applyEnabledLook(false); // starts off (greyed) until the parameter says otherwise
}

void MorphPad::setMorphEnabled(bool on)
{
    m_enable.setToggleState(on, juce::dontSendNotification);
    applyEnabledLook(on);
}

void MorphPad::applyEnabledLook(bool on)
{
    m_enable.setButtonText(on ? "ON" : "OFF");
    if (on == m_enabled)
        return;
    m_enabled = on;
    // Off: everything but the switch greys back (still usable, so you can set it up before switching on).
    for (auto& mb : m_modeButtons) mb.setAlpha(on ? 1.0f : 0.45f);
    m_wanderButton->setAlpha(on ? 1.0f : 0.45f);
    m_speed.setAlpha(on ? 1.0f : 0.45f);
    m_shapeBox.setAlpha(on ? 1.0f : 0.45f);
    repaint();
}

void MorphPad::paintOverChildren(juce::Graphics& g)
{
    if (m_enabled)
        return;
    // A grey veil over the pad and the bus labels while the morph is off.
    // The pad and each label on their own, so the wander controls between the labels only get their usual dimming.
    g.setColour(Theme::Colours::background.withAlpha(0.5f));
    g.fillRoundedRectangle(m_pad.expanded(2.0f), 4.0f);
    for (const auto& h : m_handles)
        g.fillRoundedRectangle(h.expanded(2.0f), 4.0f);
    g.setFont(m_fonts.label(11.0f));
    g.setColour(Theme::Colours::textDim);
    g.drawText("MORPH OFF", m_pad.withTrimmedTop(m_pad.getHeight() * 0.62f), juce::Justification::centredTop);
}

void MorphPad::setShape(int shape)
{
    shape = juce::jlimit(0, MorphWander::NumShapes - 1, shape);
    m_shapeBox.setSelectedItemIndex(shape, juce::dontSendNotification);
    if (shape == m_shape)
        return;
    m_shape = shape;
    m_trailCount = 0;
    repaint();
}

MorphPad::~MorphPad()
{
    stopTimer();
    if (m_overlay != nullptr)
        if (auto* parent = m_overlay->getParentComponent())
            parent->removeChildComponent(m_overlay.get());
}

//==============================================================================
void MorphPad::setXY(float x, float y)
{
    if (m_draggingDot || m_wander)
        return;
    x = juce::jlimit(0.0f, 1.0f, x);
    y = juce::jlimit(0.0f, 1.0f, y);
    if (std::abs(x - m_x) > 1.0e-6f || std::abs(y - m_y) > 1.0e-6f)
    {
        m_x = x;
        m_y = y;
        repaint();
    }
}

void MorphPad::setMode(int mode)
{
    mode = juce::jlimit(0, c_numModes - 1, mode);
    m_mode = mode;
    m_modeButtons[static_cast<size_t>(mode)].setToggleState(true, juce::dontSendNotification);
    repaint();
}

void MorphPad::setWander(bool on, float speed)
{
    m_speed.setValue(juce::jlimit(0.0f, 1.0f, speed), juce::dontSendNotification);
    m_wanderButton->setToggleState(on, juce::dontSendNotification);
    if (on == m_wander)
        return;

    m_wander = on;
    m_trailCount = 0;
    updateTimer();
    repaint();
}

void MorphPad::setSources(int laneA, int laneB, juce::String nameA, juce::String nameB)
{
    m_lanes = { laneA, laneB };
    m_names = { std::move(nameA), std::move(nameB) };
    repaint();
}

void MorphPad::setRopeOverlayParent(juce::Component* topLevel)
{
    if (m_overlay != nullptr)
        if (auto* parent = m_overlay->getParentComponent(); parent != nullptr && parent != topLevel)
            parent->removeChildComponent(m_overlay.get());
    m_overlayParent = topLevel;
}

void MorphPad::setFixedSources(juce::String nameA, juce::Colour colourA, juce::String nameB, juce::Colour colourB)
{
    m_fixedSources = true;
    m_names = { nameA, nameB };
    m_fixedColours = { colourA, colourB };
    resized(); // narrower labels, with the wander shape and speed between them
    repaint();
}

juce::Colour MorphPad::laneColour(int slot) const
{
    if (m_fixedSources)
        return m_fixedColours[static_cast<size_t>(slot)];
    const int lane = m_lanes[static_cast<size_t>(slot)];
    return lane >= 0 ? Theme::trackColour(lane) : Theme::Colours::textFaint;
}

//==============================================================================
void MorphPad::resized()
{
    auto b = getLocalBounds().reduced(2);

    auto top = b.removeFromTop(22);
    m_enable.setBounds(top.removeFromLeft(40));
    top.removeFromLeft(4);
    const int modeW = juce::jmin(52, (top.getWidth() - 100) / c_numModes);
    for (auto& mb : m_modeButtons)
        mb.setBounds(top.removeFromLeft(modeW));
    m_wanderButton->setBounds(top.removeFromRight(24));
    top.removeFromRight(4);

    auto bottom = b.removeFromBottom(24);
    if (m_fixedSources)
    {
        // Fixed sources are just labels, so they can be narrow: the wander shape and speed sit between them,
        // where the shape names have room to be read.
        const int handleW = juce::jmin(84, bottom.getWidth() / 4);
        m_handles[0] = bottom.removeFromLeft(handleW).toFloat().reduced(0.0f, 2.0f);
        m_handles[1] = bottom.removeFromRight(handleW).toFloat().reduced(0.0f, 2.0f);
        auto middle = bottom.reduced(6, 1);
        m_speed.setBounds(middle.removeFromRight(juce::jmin(56, middle.getWidth() / 3)));
        middle.removeFromRight(4);
        m_shapeBox.setBounds(middle);
    }
    else
    {
        // Rope handles show lane names and need the width: the wander shape and speed go in the top row.
        m_speed.setBounds(top.removeFromRight(juce::jmin(64, top.getWidth())));
        top.removeFromRight(4);
        m_shapeBox.setBounds(top.removeFromRight(juce::jmin(110, top.getWidth())));
        const float handleW = juce::jmin(160.0f, static_cast<float>(bottom.getWidth()) * 0.5f - 4.0f);
        auto bf = bottom.toFloat();
        m_handles[0] = bf.removeFromLeft(handleW).reduced(0.0f, 2.0f);
        m_handles[1] = bf.removeFromRight(handleW).reduced(0.0f, 2.0f);
    }

    m_pad = b.toFloat().reduced(0.0f, 4.0f);
}

juce::Point<float> MorphPad::padToLocal(float x, float y) const
{
    return { m_pad.getX() + x * m_pad.getWidth(), m_pad.getBottom() - y * m_pad.getHeight() };
}

void MorphPad::paint(juce::Graphics& g)
{
    // Pad
    g.setColour(Theme::Colours::scope.withAlpha(0.45f)); // the waves show through
    g.fillRoundedRectangle(m_pad, 4.0f);

    g.setColour(Theme::Colours::grid);
    for (int i = 1; i < 4; ++i)
    {
        const float fx = m_pad.getX() + m_pad.getWidth() * static_cast<float>(i) / 4.0f;
        const float fy = m_pad.getY() + m_pad.getHeight() * static_cast<float>(i) / 4.0f;
        g.drawVerticalLine(juce::roundToInt(fx), m_pad.getY() + 1.0f, m_pad.getBottom() - 1.0f);
        g.drawHorizontalLine(juce::roundToInt(fy), m_pad.getX() + 1.0f, m_pad.getRight() - 1.0f);
    }

    // Axis labels: y per mode (top / bottom), x is A (left) to B (right).
    g.setFont(m_fonts.label(11.0f));
    g.setColour(Theme::Colours::textFaint);
    const auto labels = m_pad.reduced(6.0f, 3.0f);
    g.drawText(c_topLabels[m_mode], labels, juce::Justification::centredTop, false);
    g.drawText(c_bottomLabels[m_mode], labels, juce::Justification::centredBottom, false);
    g.setFont(m_fonts.heading(12.0f));
    g.setColour(laneColour(0));
    g.drawText("A", labels, juce::Justification::centredLeft, false);
    g.setColour(laneColour(1));
    g.drawText("B", labels, juce::Justification::centredRight, false);

    const auto dot = padToLocal(m_x, m_y);
    const auto accent = Theme::Colours::accent;

    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(m_pad.toNearestInt());

        if (m_wander)
        {
            // The whole shape, faintly.
            juce::Path figure;
            const auto& points = MorphWander::curve(m_shape);
            const auto box = MorphWander::bounds(m_shape, m_rotation);
            for (int i = 0; i < MorphWander::c_curvePoints; ++i)
            {
                const auto n = MorphWander::toPad(points[static_cast<size_t>(i)], m_rotation, box);
                const auto p = padToLocal(n.x, n.y);
                if (i == 0) figure.startNewSubPath(p);
                else        figure.lineTo(p);
            }
            g.setColour(accent.withAlpha(0.12f));
            g.strokePath(figure, juce::PathStrokeType(1.0f));

            // Short bright trail behind the dot.
            for (int i = 1; i < m_trailCount; ++i)
            {
                const int a = (m_trailHead - m_trailCount + i - 1 + c_trail) % c_trail;
                const int b = (a + 1) % c_trail;
                const float age = static_cast<float>(i) / static_cast<float>(m_trailCount);
                g.setColour(accent.withAlpha(0.85f * age));
                const auto pa = m_trail[static_cast<size_t>(a)], pb = m_trail[static_cast<size_t>(b)];
                g.drawLine(juce::Line<float>(padToLocal(pa.x, pa.y), padToLocal(pb.x, pb.y)), 1.0f + 1.5f * age);
            }
        }

        // Crosshair
        g.setColour(Theme::Colours::outline);
        g.drawVerticalLine(juce::roundToInt(dot.x), m_pad.getY(), m_pad.getBottom());
        g.drawHorizontalLine(juce::roundToInt(dot.y), m_pad.getX(), m_pad.getRight());
    }

    g.setColour(Theme::Colours::outline);
    g.drawRoundedRectangle(m_pad.reduced(0.5f), 4.0f, 1.0f);

    // Dot, coloured between the two sources.
    const auto dotColour = laneColour(0).interpolatedWith(laneColour(1), m_x);
    const auto dotArea = juce::Rectangle<float>(12.0f, 12.0f).withCentre(dot);
    g.setColour(dotColour);
    g.fillEllipse(dotArea);
    g.setColour(m_wander ? accent : Theme::Colours::text);
    g.drawEllipse(dotArea, 1.5f);

    // Rope handles
    for (int slot = 0; slot < 2; ++slot)
    {
        const auto r = m_handles[static_cast<size_t>(slot)];
        const auto colour = laneColour(slot);
        const bool active = m_ropeSlot == slot;
        const bool hover = m_hoverHandle == slot;

        g.setColour(hover || active ? Theme::Colours::hover : Theme::Colours::panelRaised);
        g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
        g.setColour(active ? accent : colour.withAlpha(0.8f));
        g.drawRoundedRectangle(r.reduced(0.5f), r.getHeight() * 0.5f, 1.0f);

        auto inner = r.reduced(3.0f, 2.0f);
        const auto knot = slot == 0 ? inner.removeFromLeft(inner.getHeight()) : inner.removeFromRight(inner.getHeight());
        g.setColour(colour);
        g.fillEllipse(knot);
        g.setColour(Theme::Colours::background);
        g.setFont(m_fonts.heading(11.0f));
        g.drawText(slot == 0 ? "A" : "B", knot, juce::Justification::centred, false);

        inner.reduce(5.0f, 0.0f);
        const bool assigned = m_fixedSources || m_lanes[static_cast<size_t>(slot)] >= 0;
        g.setFont(m_fonts.label(12.0f));
        g.setColour(assigned ? Theme::Colours::text : Theme::Colours::textFaint);
        g.drawText(assigned ? m_names[static_cast<size_t>(slot)] : juce::String("Drag to a track"), inner,
                   slot == 0 ? juce::Justification::centredLeft : juce::Justification::centredRight, true);
    }
}

//==============================================================================
void MorphPad::setXYFromPad(juce::Point<float> local, bool notify)
{
    if (m_pad.isEmpty())
        return;
    m_x = juce::jlimit(0.0f, 1.0f, (local.x - m_pad.getX()) / m_pad.getWidth());
    m_y = juce::jlimit(0.0f, 1.0f, (m_pad.getBottom() - local.y) / m_pad.getHeight());
    repaint();
    if (notify && onXYChanged)
        onXYChanged(m_x, m_y);
}

void MorphPad::mouseDown(const juce::MouseEvent& e)
{
    const auto p = e.position;
    for (int slot = 0; slot < 2; ++slot)
        if (m_handles[static_cast<size_t>(slot)].contains(p))
        {
            if (! m_fixedSources)
                startRope(slot, e.getScreenPosition());
            return;
        }

    if (m_pad.expanded(4.0f).contains(p))
    {
        if (m_wander)
        {
            setWander(false, static_cast<float>(m_speed.getValue()));
            if (onWanderChanged) onWanderChanged(false, static_cast<float>(m_speed.getValue()));
        }
        m_draggingDot = true;
        setMouseCursor(juce::MouseCursor::NoCursor);
        setXYFromPad(p, true);
    }
}

void MorphPad::mouseDrag(const juce::MouseEvent& e)
{
    if (m_ropeSlot >= 0)
        dragRope(e.getScreenPosition());
    else if (m_draggingDot)
        setXYFromPad(e.position, true);
}

void MorphPad::mouseUp(const juce::MouseEvent& e)
{
    if (m_ropeSlot >= 0)
        endRope(e.getScreenPosition());
    if (m_draggingDot)
    {
        m_draggingDot = false;
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
    }
}

void MorphPad::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (m_pad.contains(e.position) && ! m_wander)
    {
        m_x = m_y = 0.5f;
        repaint();
        if (onXYChanged) onXYChanged(m_x, m_y);
    }
}

void MorphPad::mouseMove(const juce::MouseEvent& e)
{
    int hover = -1;
    for (int slot = 0; slot < 2; ++slot)
        if (m_handles[static_cast<size_t>(slot)].contains(e.position))
            hover = slot;
    if (hover != m_hoverHandle)
    {
        m_hoverHandle = hover;
        repaint();
    }
    setMouseCursor(hover >= 0 ? juce::MouseCursor::DraggingHandCursor
                              : (m_pad.contains(e.position) ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor));
}

void MorphPad::mouseExit(const juce::MouseEvent&)
{
    if (m_hoverHandle >= 0)
    {
        m_hoverHandle = -1;
        repaint();
    }
}

//==============================================================================
// Rope
void MorphPad::startRope(int slot, juce::Point<int> screenPos)
{
    juce::Component* parent = m_overlayParent != nullptr ? m_overlayParent.getComponent() : getTopLevelComponent();
    if (parent == nullptr)
        return;

    if (m_overlay == nullptr)
        m_overlay = std::make_unique<RopeOverlay>(*this);
    if (m_overlay->getParentComponent() != parent)
    {
        if (auto* old = m_overlay->getParentComponent())
            old->removeChildComponent(m_overlay.get());
        parent->addChildComponent(*m_overlay);
    }
    m_overlay->setBounds(parent->getLocalBounds());
    m_overlay->toFront(false);
    m_overlay->setVisible(true);

    m_ropeSlot = slot;
    m_ropeEndScreen = m_ropeLastScreen = screenPos.toFloat();
    m_ropeOffset = { 0.0f, 0.0f };
    m_ropeVelocity = { 0.0f, 0.0f };
    m_ropeHoverLane = -1;
    m_lastTick = juce::Time::getMillisecondCounterHiRes();
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    updateTimer();
    repaint();
}

void MorphPad::dragRope(juce::Point<int> screenPos)
{
    m_ropeEndScreen = screenPos.toFloat();
    m_ropeHoverLane = laneAtScreenPosition ? laneAtScreenPosition(screenPos) : -1;
    if (m_overlay != nullptr)
        m_overlay->update();
}

void MorphPad::endRope(juce::Point<int> screenPos)
{
    const int slot = m_ropeSlot;
    m_ropeSlot = -1;
    if (m_overlay != nullptr)
    {
        m_overlay->repaint(m_overlay->lastArea);
        m_overlay->setVisible(false);
    }
    updateTimer();
    repaint();

    const int lane = laneAtScreenPosition ? laneAtScreenPosition(screenPos) : -1;
    if (slot >= 0 && lane >= 0 && onSourceAssigned)
        onSourceAssigned(slot, lane);
}

//==============================================================================
void MorphPad::updateTimer()
{
    const bool needed = m_wander || m_ropeSlot >= 0;
    if (needed && ! isTimerRunning())
    {
        m_lastTick = juce::Time::getMillisecondCounterHiRes();
        startTimerHz(60);
    }
    else if (! needed && isTimerRunning())
    {
        stopTimer();
    }
}

void MorphPad::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float dt = juce::jlimit(0.001f, 0.05f, static_cast<float>((now - m_lastTick) * 0.001));
    m_lastTick = now;

    if (m_wander && wanderPosition)
    {
        // The dot is flown elsewhere: show where it is, with a short trail behind it.
        float x = m_x, y = m_y, rotation = m_rotation;
        wanderPosition(x, y, rotation);
        m_x = juce::jlimit(0.0f, 1.0f, x);
        m_y = juce::jlimit(0.0f, 1.0f, y);
        m_rotation = rotation;
        m_trail[static_cast<size_t>(m_trailHead)] = { m_x, m_y };
        m_trailHead = (m_trailHead + 1) % c_trail;
        m_trailCount = std::min(m_trailCount + 1, c_trail);
        repaint(m_pad.expanded(8.0f).getSmallestIntegerContainer());
    }

    if (m_ropeSlot >= 0 && m_overlay != nullptr)
    {
        // Sagging rope with a little spring wobble: the midpoint chases a sag below the chord and
        // is flicked the other way when the hand moves.
        const auto startScreen = localPointToGlobal(m_handles[static_cast<size_t>(m_ropeSlot)].getCentre());
        const float length = startScreen.getDistanceFrom(m_ropeEndScreen);
        const juce::Point<float> target { 0.0f, juce::jlimit(8.0f, 90.0f, 0.18f * length) };

        const auto handDelta = m_ropeEndScreen - m_ropeLastScreen;
        m_ropeLastScreen = m_ropeEndScreen;
        m_ropeVelocity -= handDelta * 6.0f; // the rope lags behind the hand

        constexpr float omega = juce::MathConstants<float>::twoPi * 2.2f, damping = 0.3f;
        const auto accel = (target - m_ropeOffset) * (omega * omega) - m_ropeVelocity * (2.0f * damping * omega);
        m_ropeVelocity += accel * dt;
        m_ropeOffset += m_ropeVelocity * dt;
        m_ropeOffset = { juce::jlimit(-150.0f, 150.0f, m_ropeOffset.x), juce::jlimit(-150.0f, 200.0f, m_ropeOffset.y) };

        m_overlay->update();
    }
}

#include "MorphPad.h"
#include "Icons.h"

#include <cmath>

namespace
{
    const char* const c_modeNames[MorphPad::c_numModes] = { "Blend", "Glide", "Cross" };
    const char* const c_topLabels[MorphPad::c_numModes] = { "Fuse", "Shape", "A Through B" };
    const char* const c_bottomLabels[MorphPad::c_numModes] = { "Mix", "Move", "B Through A" };
    const char* const c_modeTips[MorphPad::c_numModes] = {
        "Blend\nMorphs the tone of side A into side B's: left is A, right is B. Higher up the pad fuses the two into one texture.",
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

    juce::Point<float> handlePoint(int slot) const
    {
        return getLocalPoint(nullptr, owner.localPointToGlobal(owner.m_handles[static_cast<size_t>(slot)].getCentre()));
    }

    /** The rope being dragged from a handle. */
    juce::Path makeRope() const
    {
        juce::Path p;
        if (owner.m_ropeSlot < 0)
            return p;

        const auto start = handlePoint(owner.m_ropeSlot);
        const auto end = getLocalPoint(nullptr, owner.m_ropeEndScreen);
        const auto mid = (start + end) * 0.5f;
        const auto ctrl = mid + owner.m_ropeOffset * 2.0f; // the curve's midpoint sits at mid + offset
        p.startNewSubPath(start);
        p.quadraticTo(ctrl, end);
        return p;
    }

    /** A rope that hangs between a handle and its track (false: that side has none to show). */
    bool hangingRope(int slot, juce::Path& p, juce::Point<float>& end) const
    {
        const int lane = owner.m_lanes[static_cast<size_t>(slot)];
        juce::Point<int> anchor;
        if (lane < 0 || slot == owner.m_ropeSlot || ! Theme::isShownInWindow(owner) || ! owner.laneAnchor || ! owner.laneAnchor(lane, anchor))
            return false;
        const auto start = handlePoint(slot);
        end = getLocalPoint(nullptr, anchor.toFloat());
        const float sag = juce::jlimit(6.0f, 48.0f, 0.1f * start.getDistanceFrom(end)); // hangs a little: it crosses other panels
        p.startNewSubPath(start);
        p.quadraticTo((start + end) * 0.5f + juce::Point<float>(0.0f, 2.0f * sag), end);
        return true;
    }

    void update()
    {
        auto bounds = makeRope().getBounds();
        for (int slot = 0; slot < 2; ++slot)
        {
            juce::Path p;
            juce::Point<float> end;
            if (hangingRope(slot, p, end))
                bounds = bounds.isEmpty() ? p.getBounds() : bounds.getUnion(p.getBounds());
        }
        const auto now = bounds.expanded(16.0f).getSmallestIntegerContainer();
        repaint(lastArea.getUnion(now));
        lastArea = now;
    }

    void drawRope(juce::Graphics& g, const juce::Path& rope, juce::Colour colour) const
    {
        g.setColour(Theme::Colours::shadow.withAlpha(0.5f));
        g.strokePath(rope, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), // round object: a rope
                     juce::AffineTransform::translation(0.0f, 2.0f));
        g.setColour(colour.darker(0.35f));
        g.strokePath(rope, juce::PathStrokeType(3.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)); // round object: a rope

        // Twist: short light dashes along the rope.
        juce::Path twist;
        const float dashes[] = { 3.0f, 4.0f };
        juce::PathStrokeType(1.4f).createDashedStroke(twist, rope, dashes, 2);
        g.setColour(colour.brighter(0.4f).withAlpha(0.85f));
        g.fillPath(twist);
    }

    void drawEnd(juce::Graphics& g, juce::Point<float> end, int slot, int lane, juce::Colour colour) const
    {
        // The end of the rope: a square tag (a lettered label is never a roundel), filled with the colour of the
        // lane it is on (or would land on).
        const auto ring = juce::Rectangle<float>(18.0f, 18.0f).withCentre(end);
        g.setColour(lane >= 0 ? owner.trackColour(lane) : Theme::Colours::panelRaised);
        g.fillRect(ring);
        g.setColour(colour);
        g.drawRect(ring, 2.0f);
        g.setColour(lane >= 0 ? Theme::Colours::window : Theme::Colours::text);
        Theme::fitText(g, slot == 0 ? "A" : "B", ring, owner.m_fonts.caption(), juce::Justification::centred);
    }

    /** A handle's plate, in this layer's coordinates. */
    juce::Rectangle<int> handleArea(int slot) const
    {
        const auto r = owner.m_handles[static_cast<size_t>(slot)];
        const auto topLeft = getLocalPoint(nullptr, owner.localPointToGlobal(r.getTopLeft()));
        const auto bottomRight = getLocalPoint(nullptr, owner.localPointToGlobal(r.getBottomRight()));
        return juce::Rectangle<float>(topLeft, bottomRight).getSmallestIntegerContainer();
    }

    void paint(juce::Graphics& g) override
    {
        // Every rope is tied under its handle's plate: the plates are kept out of the way so that no rope is
        // ever drawn over a handle's letter or the name of the track it is roped to.
        juce::Graphics::ScopedSaveState clipped(g);
        for (int slot = 0; slot < 2; ++slot)
            g.excludeClipRegion(handleArea(slot));

        // The ropes that hang between a handle and its track (a little see-through: they cross other controls).
        for (int slot = 0; slot < 2; ++slot)
        {
            juce::Path p;
            juce::Point<float> end;
            if (hangingRope(slot, p, end))
            {
                g.beginTransparencyLayer(0.8f);
                drawRope(g, p, owner.laneColour(slot));
                g.endTransparencyLayer();
                drawEnd(g, end, slot, owner.m_lanes[static_cast<size_t>(slot)], owner.laneColour(slot));
            }
        }

        if (owner.m_ropeSlot < 0)
            return;

        const auto rope = makeRope();
        const auto colour = owner.laneColour(owner.m_ropeSlot);
        drawRope(g, rope, colour);
        drawEnd(g, getLocalPoint(nullptr, owner.m_ropeEndScreen), owner.m_ropeSlot, owner.m_ropeHoverLane, colour);
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
        setColour(juce::TextButton::buttonOnColourId, Theme::Colours::brass);
        setTooltip("Wander\nPlay: the dot wanders along the chosen shape by itself, so the morph keeps moving, with this window closed and in a bounce too. Stop: it stays where the wander left it.");
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        // The buttons' own plate, with the play or stop glyph the header's Play button uses.
        const bool on = getToggleState();
        const auto accent = Theme::Colours::brass;
        getLookAndFeel().drawButtonBackground(g, *this, Theme::Colours::raised, over, down);
        const auto b = getLocalBounds().toFloat();
        const float side = juce::jmin(b.getWidth(), b.getHeight()) * 0.5f;
        const bool resting = Theme::getLook(*this) == Theme::Look::Resting;
        Icons::draw(g, on ? Icons::Icon::Stop : Icons::Icon::Play, juce::Rectangle<float>(side, side).withCentre(b.getCentre().translated(0.0f, -0.75f)),
                    on ? accent.brighter(0.4f) : (resting ? Theme::Colours::textDim : Theme::Colours::text));
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
    m_enable.setTooltip("Morph On / Off\nOn (green): sides A and B are morphed together, as the dot says. A side is its whole bus, or the one track its rope is tied to. Off: everything plays side by side as usual and the morph rests.");
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
    m_speed.setTooltip("Wander Speed\nHow fast the dot travels along its shape.");
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
    m_enable.setButtonText(on ? "On" : "Off");
    if (on == m_enabled)
        return;
    m_enabled = on;
    // Off: everything but the switch rests (hollow, still usable, so you can set it up before switching on).
    const auto look = on ? Theme::Look::Normal : Theme::Look::Resting;
    for (auto& mb : m_modeButtons) Theme::setLook(mb, look);
    Theme::setLook(*m_wanderButton, look);
    m_speed.setAlpha(on ? 1.0f : 0.5f);
    Theme::setLook(m_shapeBox, look);
    repaint();
}

void MorphPad::paintOverChildren(juce::Graphics& g)
{
    if (m_enabled)
        return;
    // A grey veil over the pad and the bus labels while the morph is off.
    // The pad and each label on their own, so the wander controls between the labels only get their usual dimming.
    g.setColour(Theme::Colours::window.withAlpha(0.5f));
    Theme::Plate::fill(g, m_pad.expanded(2.0f), Theme::Cut::control);
    for (const auto& h : m_handles)
        Theme::Plate::fill(g, h.expanded(2.0f), Theme::Cut::control);
    g.setColour(Theme::Colours::text);
    Theme::fitText(g, "Morph Off", m_pad.withTrimmedTop(m_pad.getHeight() * 0.62f).withHeight(16.0f), m_fonts.control(), juce::Justification::centred);
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
    if (m_lanes[0] == laneA && m_lanes[1] == laneB && m_names[0] == nameA && m_names[1] == nameB)
        return;
    m_lanes = { laneA, laneB };
    m_names = { std::move(nameA), std::move(nameB) };
    repaint();
    updateOverlay();
}

bool MorphPad::anyRopeShown() const
{
    return m_ropeSlot >= 0 || (Theme::isShownInWindow(*this) && (m_lanes[0] >= 0 || m_lanes[1] >= 0));
}

void MorphPad::updateOverlay()
{
    const bool wanted = anyRopeShown();
    if (m_overlay == nullptr)
    {
        if (! wanted)
            return;
        m_overlay = std::make_unique<RopeOverlay>(*this);
    }
    juce::Component* parent = m_overlayParent != nullptr ? m_overlayParent.getComponent() : getTopLevelComponent();
    if (wanted && parent != nullptr)
    {
        if (m_overlay->getParentComponent() != parent)
        {
            if (auto* old = m_overlay->getParentComponent())
                old->removeChildComponent(m_overlay.get());
            parent->addChildComponent(*m_overlay);
        }
        m_overlay->setBounds(parent->getLocalBounds());
        if (! m_overlay->isVisible())
        {
            m_overlay->toFront(false);
            m_overlay->setVisible(true);
        }
        m_overlay->update();
    }
    else if (m_overlay->isVisible())
    {
        m_overlay->repaint(m_overlay->lastArea);
        m_overlay->setVisible(false);
    }
}

void MorphPad::ropesMoved()
{
    if (m_overlay != nullptr && m_overlay->isVisible())
        m_overlay->update();
}

juce::Component* MorphPad::ropeLayer() const noexcept { return m_overlay.get(); }

void MorphPad::visibilityChanged() { updateOverlay(); }
void MorphPad::moved() { ropesMoved(); }
void MorphPad::parentHierarchyChanged() { updateOverlay(); }

juce::String MorphPad::getTooltip()
{
    const auto pos = getMouseXYRelative().toFloat();
    for (int slot = 0; slot < 2; ++slot)
        if (m_handles[static_cast<size_t>(slot)].contains(pos))
        {
            const juce::String side = slot == 0 ? "A" : "B";
            const bool roped = m_lanes[static_cast<size_t>(slot)] >= 0;
            return "Morph Source " + side + "\nWhat side " + side + " of the morph listens to: "
                   + (roped ? "the track \"" + m_names[static_cast<size_t>(slot)] + "\" alone. The rest of its bus plays on as it is."
                            : "the whole of " + m_busNames[static_cast<size_t>(slot)].replace("Bus", "bus") + ".")
                   + "\nDrag the rope from here onto a track to morph just that sound."
                   + (roped ? " Double-click to let the rope go." : "");
        }
    if (m_pad.contains(pos))
        return "Morph Pad\nLeft is side A, right is side B; up and down is what the chosen mode says at the top and bottom.\n"
               "Drag the dot. Double-click to centre it.";
    return {};
}

void MorphPad::setRopeOverlayParent(juce::Component* topLevel)
{
    if (m_overlay != nullptr)
        if (auto* parent = m_overlay->getParentComponent(); parent != nullptr && parent != topLevel)
            parent->removeChildComponent(m_overlay.get());
    m_overlayParent = topLevel;
}

void MorphPad::setBusLabels(juce::String nameA, juce::Colour colourA, juce::String nameB, juce::Colour colourB)
{
    m_busNames = { std::move(nameA), std::move(nameB) };
    m_busColours = { colourA, colourB };
    repaint();
}

juce::Colour MorphPad::laneColour(int slot) const
{
    // A roped side takes its track's colour; an unroped one is its bus.
    const int lane = m_lanes[static_cast<size_t>(slot)];
    return lane >= 0 ? trackColour(lane) : m_busColours[static_cast<size_t>(slot)];
}

//==============================================================================
void MorphPad::resized()
{
    // A dock over the sea: a plate with its own padding, so nothing sits on the plate's pressed edge or bolts.
    auto b = getLocalBounds().reduced(10);

    auto top = b.removeFromTop(22);
    m_enable.setBounds(top.removeFromLeft(40));
    top.removeFromLeft(4);
    const int modeW = juce::jmin(52, (top.getWidth() - 100) / c_numModes);
    for (auto& mb : m_modeButtons)
        mb.setBounds(top.removeFromLeft(modeW));
    m_wanderButton->setBounds(top.removeFromRight(24));
    top.removeFromRight(4);

    auto bottom = b.removeFromBottom(24);
    {
        // The rope handles (the side's bus, or the track it is roped to) at the two ends, with the wander shape
        // and speed between them.
        const int handleW = juce::jmin(100, (bottom.getWidth() * 3) / 10);
        m_handles[0] = bottom.removeFromLeft(handleW).toFloat().reduced(0.0f, 2.0f);
        m_handles[1] = bottom.removeFromRight(handleW).toFloat().reduced(0.0f, 2.0f);
        auto middle = bottom.reduced(6, 1);
        m_speed.setBounds(middle.removeFromRight(juce::jmin(44, middle.getWidth() / 3)));
        middle.removeFromRight(4);
        m_shapeBox.setBounds(middle);
    }

    m_pad = b.toFloat().reduced(0.0f, 4.0f);
}

juce::Point<float> MorphPad::padToLocal(float x, float y) const
{
    return { m_pad.getX() + x * m_pad.getWidth(), m_pad.getBottom() - y * m_pad.getHeight() };
}

void MorphPad::paint(juce::Graphics& g)
{
    // The dock: a plate bolted over the sea, near enough opaque so everything on it reads over the water.
    {
        const auto plate = getLocalBounds().toFloat();
        g.setColour(Theme::Colours::panel.withAlpha(0.95f));
        Theme::Plate::fill(g, plate, Theme::Cut::panel);
        Theme::Plate::edge(g, plate, Theme::Cut::panel);
        const float in = Theme::Plate::boltInset;
        for (const auto& c : { plate.getTopLeft().translated(in, in), plate.getTopRight().translated(-in, in),
                               plate.getBottomLeft().translated(in, -in), plate.getBottomRight().translated(-in, -in) })
            Theme::Plate::drawBolt(g, c);
    }

    // Pad
    g.setColour(Theme::Colours::well);
    Theme::Plate::fill(g, m_pad, Theme::Cut::control);
    Theme::Plate::wellEdge(g, m_pad, Theme::Cut::control);

    g.setColour(Theme::Colours::grid);
    for (int i = 1; i < 4; ++i)
    {
        const float fx = m_pad.getX() + m_pad.getWidth() * static_cast<float>(i) / 4.0f;
        const float fy = m_pad.getY() + m_pad.getHeight() * static_cast<float>(i) / 4.0f;
        g.drawVerticalLine(juce::roundToInt(fx), m_pad.getY() + 1.0f, m_pad.getBottom() - 1.0f);
        g.drawHorizontalLine(juce::roundToInt(fy), m_pad.getX() + 1.0f, m_pad.getRight() - 1.0f);
    }

    // Axis labels: y per mode (top / bottom), x is A (left) to B (right).
    g.setColour(Theme::Colours::textDim);
    const auto labels = m_pad.reduced(6.0f, 3.0f);
    Theme::fitText(g, c_topLabels[m_mode], labels.withHeight(14.0f), m_fonts.caption(), juce::Justification::centred);
    Theme::fitText(g, c_bottomLabels[m_mode], labels.withTop(labels.getBottom() - 14.0f), m_fonts.caption(), juce::Justification::centred);
    g.setColour(laneColour(0));
    Theme::fitText(g, "A", labels.withWidth(14.0f).withSizeKeepingCentre(14.0f, 16.0f), m_fonts.title(), juce::Justification::centredLeft);
    g.setColour(laneColour(1));
    Theme::fitText(g, "B", labels.withLeft(labels.getRight() - 14.0f).withSizeKeepingCentre(14.0f, 16.0f), m_fonts.title(), juce::Justification::centredRight);

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
        g.setColour(Theme::Colours::stroke.withAlpha(0.8f));
        g.drawVerticalLine(juce::roundToInt(dot.x), m_pad.getY(), m_pad.getBottom());
        g.drawHorizontalLine(juce::roundToInt(dot.y), m_pad.getX(), m_pad.getRight());
    }


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

        // A tag: its edge takes a colour while the mouse is over it or its rope is in hand.
        g.setColour(hover || active ? Theme::Colours::hover : Theme::Colours::raised);
        Theme::Plate::fill(g, r, Theme::Cut::control);
        g.setColour(active ? accent : (hover ? colour.withAlpha(0.8f) : Theme::Colours::edgeDark));
        Theme::Plate::stroke(g, r, Theme::Cut::control);

        auto inner = r.reduced(3.0f, 2.0f);
        const auto knot = slot == 0 ? inner.removeFromLeft(inner.getHeight()) : inner.removeFromRight(inner.getHeight());
        g.setColour(colour);
        Theme::Plate::fill(g, knot, Theme::Cut::small);
        g.setColour(Theme::Colours::window);
        Theme::fitText(g, slot == 0 ? "A" : "B", knot, m_fonts.caption(), juce::Justification::centred);

        inner.reduce(5.0f, 0.0f);
        // The bus, or the roped track's name (squeezed a little, then cut short, if it is long).
        const bool roped = m_lanes[static_cast<size_t>(slot)] >= 0;
        g.setColour(Theme::Colours::text);
        Theme::fitText(g, roped ? m_names[static_cast<size_t>(slot)] : m_busNames[static_cast<size_t>(slot)], inner, m_fonts.caption(),
                       slot == 0 ? juce::Justification::centredLeft : juce::Justification::centredRight, Theme::Fit::Name);
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
            if (e.getNumberOfClicks() < 2) // a double-click lets the rope go (see mouseDoubleClick)
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
    for (int slot = 0; slot < 2; ++slot)
        if (m_handles[static_cast<size_t>(slot)].contains(e.position))
        {
            // Let the rope go: the side is its bus again.
            if (m_lanes[static_cast<size_t>(slot)] >= 0 && onSourceAssigned)
                onSourceAssigned(slot, -1);
            return;
        }
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

    m_ropeSlot = slot;
    m_ropeMoved = false;
    updateOverlay();
    if (m_overlay == nullptr)
    {
        m_ropeSlot = -1;
        return;
    }
    m_overlay->toFront(false);
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
    if (screenPos.toFloat().getDistanceFrom(m_ropeEndScreen) > 0.5f)
        m_ropeMoved = true;
    m_ropeEndScreen = screenPos.toFloat();
    m_ropeHoverLane = laneAtScreenPosition ? laneAtScreenPosition(screenPos) : -1;
    if (m_overlay != nullptr)
        m_overlay->update();
}

void MorphPad::endRope(juce::Point<int> screenPos)
{
    const int slot = m_ropeSlot;
    m_ropeSlot = -1;
    updateTimer();
    repaint();

    // Dropped on a track: that track is the side's source. Dropped anywhere else, the rope goes back where it was.
    const int lane = m_ropeMoved && laneAtScreenPosition ? laneAtScreenPosition(screenPos) : -1;
    if (slot >= 0 && lane >= 0 && onSourceAssigned)
        onSourceAssigned(slot, lane);
    updateOverlay();
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

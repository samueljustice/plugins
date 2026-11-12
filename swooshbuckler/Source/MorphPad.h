#pragma once

#include <JuceHeader.h>
#include "MorphWander.h"
#include "Theme.h"

#include <array>
#include <functional>
#include <memory>

/**
 * Morph pad: the control surface for SpectralMorph. Kept simple: the pad IS the control.
 *
 *   [ON] [Blend|Glide|Cross]                              [play]
 *   +-------------------------------------------------------------+
 *   |            Fuse / Shape / A thru B   (top label)            |
 *   | A                          o (dot = x, y)                 B |
 *   |            Mix / Move / B thru A     (bottom label)         |
 *   +-------------------------------------------------------------+
 *   (A  Bus A)            [shape] [speed]             (Energy Bed  B)
 *
 *  - Click / drag on the pad: sets x (0 = A, 1 = B) and y (mode axis). Double-click: centre.
 *  - Mode: tiny segmented control, Blend (default) / Glide / Cross.
 *  - Rope handles A and B. A side with no rope is its bus (the handle says so, in the bus's colour). Press
 *    and drag a handle to pull a sagging, wobbly rope across the editor and drop it on a track: that track
 *    alone is the side's source, the handle takes its name and colour, and the rope stays hanging between
 *    the two while the pad is showing. Double-click a handle to let its rope go. The ropes are drawn on a
 *    transparent, click-through overlay added to the component given to setRopeOverlayParent() (falls back
 *    to the top-level component).
 *  - Wander (play button): the dot flies along the chosen shape (see MorphWander) at the speed set by
 *    the Speed slider. The flying is done by whoever owns the morph (the processor, so it carries on
 *    with the window closed): the pad asks wanderPosition for the dot's place and draws it, with the
 *    whole shape faintly behind it and a short bright trail. Dragging the pad stops wandering.
 *
 * Independent of the processor: wire it through the callbacks / setters below (message thread only).
 */
class MorphPad : public juce::Component, public juce::TooltipClient, private juce::Timer
{
public:
    explicit MorphPad(const Theme::Fonts& fonts);
    ~MorphPad() override;

    //==========================================================================
    // Wiring (all message thread)
    std::function<void(float x, float y)> onXYChanged;
    std::function<void(int mode)> onModeChanged;
    std::function<void(bool on, float speed)> onWanderChanged;          // speed 0..1
    std::function<void(int shape)> onShapeChanged;
    std::function<void(bool on)> onEnableChanged;
    void setMorphEnabled(bool on);
    std::function<int(juce::Point<int> screenPos)> laneAtScreenPosition; // the track under a point, or -1
    /** A rope was dropped on a track, or let go (lane -1: the side is its bus again). */
    std::function<void(int slot /*0 = A, 1 = B*/, int lane)> onSourceAssigned;
    /** Where a roped track's lane is on screen (false: it isn't shown, so its rope isn't drawn). */
    std::function<bool(int lane, juce::Point<int>& screenPos)> laneAnchor;
    /** The lanes moved (scrolled, added, removed): the hanging ropes are drawn again. */
    void ropesMoved();

    /** While wandering: where the dot is now (0..1 across and up) and how far its shape has turned (radians). */
    std::function<void(float& x, float& y, float& rotation)> wanderPosition;

    /** Host -> UI. Ignored while the user drags the dot or while wandering (the dot shows the wander then). */
    void setXY(float x, float y);
    void setMode(int mode);
    void setWander(bool on, float speed);

    /** The path the dot wanders along. */
    static juce::StringArray shapeNames() { return MorphWander::shapeNames(); }
    void setShape(int shape);
    /** The track each side is roped to and its name (lane -1 = no rope: the side is its bus). */
    void setSources(int laneA, int laneB, juce::String nameA, juce::String nameB);
    void setRopeOverlayParent(juce::Component* topLevel);
    /** What an unroped side is called and its colour ("Bus A" / "Bus B"). */
    void setBusLabels(juce::String nameA, juce::Colour colourA, juce::String nameB, juce::Colour colourB);
    juce::String getTooltip() override;

    float getX() const { return m_x; }
    float getY() const { return m_y; }
    int getMode() const { return m_mode; }
    bool isWandering() const { return m_wander; }

    static constexpr int c_numModes = 3;

    //==========================================================================
    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void visibilityChanged() override;
    void moved() override;
    void parentHierarchyChanged() override;

private:
    class RopeOverlay;
    class WanderButton;

    void timerCallback() override;
    void updateTimer();

    void setXYFromPad(juce::Point<float> local, bool notify);
    juce::Point<float> padToLocal(float x, float y) const;
    juce::Colour laneColour(int slot) const;

    // Rope
    void startRope(int slot, juce::Point<int> screenPos);
    void dragRope(juce::Point<int> screenPos);
    void endRope(juce::Point<int> screenPos);

    const Theme::Fonts& m_fonts;

    std::array<juce::TextButton, c_numModes> m_modeButtons;
    std::unique_ptr<WanderButton> m_wanderButton;
    juce::Slider m_speed;
    juce::ComboBox m_shapeBox;
    juce::TextButton m_enable { "OFF" };
    bool m_enabled = false;
    void applyEnabledLook(bool on);
    int m_shape = MorphWander::Lorenz;

    juce::Rectangle<float> m_pad;
    std::array<juce::Rectangle<float>, 2> m_handles;
    int m_hoverHandle = -1;

    float m_x = 0.5f, m_y = 0.5f;
    int m_mode = 0;
    bool m_draggingDot = false;

    std::array<int, 2> m_lanes { -1, -1 };
    std::array<juce::String, 2> m_names;
    std::array<juce::String, 2> m_busNames { "Bus A", "Bus B" };
    std::array<juce::Colour, 2> m_busColours { Theme::Colours::textFaint, Theme::Colours::textFaint };

    // Wander state
    bool m_wander = false;
    float m_rotation = 0.0f;                 // radians: how far the shape has turned
    static constexpr int c_trail = 40;
    std::array<juce::Point<float>, c_trail> m_trail {};
    int m_trailCount = 0, m_trailHead = 0;
    double m_lastTick = 0.0;

    // Rope state (overlay coordinates are recomputed from screen positions)
    juce::Component::SafePointer<juce::Component> m_overlayParent;
    std::unique_ptr<RopeOverlay> m_overlay;
    int m_ropeSlot = -1;
    juce::Point<float> m_ropeEndScreen, m_ropeLastScreen, m_ropeOffset, m_ropeVelocity;
    int m_ropeHoverLane = -1;
    bool m_ropeMoved = false;       // the handle was dragged (a plain click leaves the rope as it is)
    /** Makes the overlay if there is somewhere to put it, and shows it while a rope hangs or is being dragged. */
    void updateOverlay();
    bool anyRopeShown() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorphPad)
};

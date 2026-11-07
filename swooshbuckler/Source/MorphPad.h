#pragma once

#include <JuceHeader.h>
#include "Theme.h"

#include <array>
#include <functional>
#include <memory>

/**
 * Morph pad: the control surface for SpectralMorph. Kept simple: the pad IS the control.
 *
 *   [ON] [Blend|Glide|Cross]           [shape] [speed] [play]
 *   +-------------------------------------------------------------+
 *   |            Fuse / Shape / A thru B   (top label)            |
 *   | A                          o (dot = x, y)                 B |
 *   |            Mix / Move / B thru A     (bottom label)         |
 *   +-------------------------------------------------------------+
 *   (A  Air)                                             (B  Sample 2)
 *
 *  - Click / drag on the pad: sets x (0 = A, 1 = B) and y (mode axis). Double-click: centre.
 *  - Mode: tiny segmented control, Blend (default) / Glide / Cross.
 *  - Rope handles A and B, in their source lanes' colours: press and drag to pull a sagging, wobbly
 *    rope across the editor, drop it on a lane to make that lane the source. The rope is drawn on a
 *    transparent, click-through overlay added to the component given to setRopeOverlayParent()
 *    (falls back to the top-level component).
 *  - Wander (play button): the dot flies along the chosen shape, normalised into the pad, at a
 *    constant visual speed set by the Speed slider. The default, Lorenz, is the Lorenz attractor
 *      dx/dt = 10 (y - x),  dy/dt = x (28 - z) - y,  dz/dt = x y - 8/3 z
 *    seen side on (x across, z up): integrated live, so the dot loops round one wing or the other
 *    chaotically and never repeats. The closed shapes precess slowly (1 degree per lap) so they
 *    never repeat exactly either. The whole shape is drawn faintly while wandering, with a short
 *    bright trail behind the dot. Dragging the pad stops wandering.
 *
 * Independent of the processor: wire it through the callbacks / setters below (message thread only).
 */
class MorphPad : public juce::Component, private juce::Timer
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
    std::function<int(juce::Point<int> screenPos)> laneAtScreenPosition; // lane 0..11 or -1
    std::function<void(int slot /*0 = A, 1 = B*/, int lane)> onSourceAssigned;

    /** Host -> UI. Ignored while the user drags the dot or while wandering (the pad is the source then). */
    void setXY(float x, float y);
    void setMode(int mode);
    void setWander(bool on, float speed);

    /** The path the dot wanders along. */
    enum Shape { Lorenz = 0, SweetJustice, FigureEight, Rose, Circle, NumShapes };
    static juce::StringArray shapeNames() { return { "Lorenz", "Sweet Justice", "Figure eight", "Rose", "Circle" }; }
    void setShape(int shape);
    /** lane -1 = unassigned. */
    void setSources(int laneA, int laneB, juce::String nameA, juce::String nameB);
    void setRopeOverlayParent(juce::Component* topLevel);
    /** Fixed sources (e.g. "Bus A" / "Bus B"): the handles become labels and can't be dragged. */
    void setFixedSources(juce::String nameA, juce::Colour colourA, juce::String nameB, juce::Colour colourB);

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

private:
    class RopeOverlay;
    class WanderButton;

    void timerCallback() override;
    void updateTimer();

    void setXYFromPad(juce::Point<float> local, bool notify);
    juce::Point<float> padToLocal(float x, float y) const;
    juce::Colour laneColour(int slot) const;

    // Wander curve (the shape's points, precomputed; for Lorenz, a long stretch of the attractor)
    juce::Point<float> shapeRaw(double t) const;
    void updateShapeTransform();
    juce::Point<float> shapeNorm(double t) const; // 0..1 in pad space (y up)
    void stepWander(float dt);
    void stepLorenz(float distance);
    juce::Point<float> lorenzNorm(const juce::Vector3D<double>& v) const;
    float wanderUnitsPerSecond() const;

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
    int m_shape = Lorenz;
    void buildCurve();

    juce::Rectangle<float> m_pad;
    std::array<juce::Rectangle<float>, 2> m_handles;
    int m_hoverHandle = -1;

    float m_x = 0.5f, m_y = 0.5f;
    int m_mode = 0;
    bool m_draggingDot = false;

    std::array<int, 2> m_lanes { -1, -1 };
    std::array<juce::String, 2> m_names;
    bool m_fixedSources = false;
    std::array<juce::Colour, 2> m_fixedColours;

    // Wander state
    bool m_wander = false;
    double m_t = 0.0;
    double m_laps = 0.0;
    float m_rotation = 0.0f;                 // radians
    juce::Rectangle<float> m_bbox;           // rotated raw-curve bounds
    static constexpr int c_curvePoints = 2400;
    std::array<juce::Point<float>, c_curvePoints> m_curve; // raw curve, precomputed
    std::vector<juce::Vector3D<double>> m_lorenzStates;    // Lorenz only: the 3-D states behind m_curve
    juce::Vector3D<double> m_lorenz { 1.0, 1.0, 20.0 };    // Lorenz only: the live state
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorphPad)
};

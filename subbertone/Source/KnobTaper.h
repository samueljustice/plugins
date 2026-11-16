#pragma once

#include <JuceHeader.h>

#include <cmath>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

/**
 * Where on a knob's travel each value of its parameter sits. Editor only.
 *
 * Hosts store automation, MIDI and controller mappings and macros as a parameter's normalised 0..1 value, so a
 * parameter's range, skew and step stay as they are: changing them would move every automated value. A taper changes
 * the knob instead. The knob's arc, drag (fine drag too) and mouse wheel follow the taper; its value box, typing,
 * double-click and Alt-click work in the parameter's own values exactly as before; host automation lanes and generic
 * editors keep the parameter's own curve.
 *
 * A taper is a list of breakpoints, knob position (0..1) -> value, running from the parameter's minimum at 0 to its
 * maximum at 1, so every value keeps a position of its own. Between two breakpoints the value moves logarithmically
 * (frequencies and times: equal travel for equal ratios) or linearly.
 */
class KnobTaper
{
public:
    struct Point
    {
        double position, value;
    };

    KnobTaper(std::initializer_list<Point> points, bool logarithmic)
        : m_points(points), m_logarithmic(logarithmic)
    {
        // From position 0 to 1, positions and values both rising (and above 0 for a logarithmic taper).
        jassert(m_points.size() >= 2);
        jassert(juce::exactlyEqual(m_points.front().position, 0.0) && juce::exactlyEqual(m_points.back().position, 1.0));
        jassert(! logarithmic || m_points.front().value > 0.0);

        for (std::size_t i = 1; i < m_points.size(); ++i)
            jassert(m_points[i].position > m_points[i - 1].position && m_points[i].value > m_points[i - 1].value);
    }

    /** Logarithmic from minimum to maximum, with their geometric mean at 12 o'clock. */
    static KnobTaper logarithmic(double minimum, double maximum)
    {
        return KnobTaper({ { 0.0, minimum }, { 1.0, maximum } }, true);
    }

    double getMinimum() const noexcept { return m_points.front().value; }
    double getMaximum() const noexcept { return m_points.back().value; }

    /** The value at a knob position (0..1). At a breakpoint's position it is exactly that breakpoint's value. */
    double toValue(double position) const noexcept
    {
        const auto [a, b] = segment(position, &Point::position);

        if (position >= b.position)
            return b.value;

        if (! (position > a.position))   // at or before the segment's start (or not a number)
            return a.value;

        const double t = (position - a.position) / (b.position - a.position);
        return m_logarithmic ? a.value * std::pow(b.value / a.value, t) : a.value + t * (b.value - a.value);
    }

    /** The knob position (0..1) of a value; a value beyond either end sits at that end. */
    double toPosition(double value) const noexcept
    {
        const auto [a, b] = segment(value, &Point::value);

        if (value >= b.value)
            return b.position;

        if (! (value > a.value))
            return a.position;

        const double t = m_logarithmic ? std::log(value / a.value) / std::log(b.value / a.value)
                                       : (value - a.value) / (b.value - a.value);
        return a.position + t * (b.position - a.position);
    }

private:
    // The segment that holds x (a position or a value): the first whose upper end reaches x, else the last.
    std::pair<const Point&, const Point&> segment(double x, double Point::* field) const noexcept
    {
        std::size_t upper = 1;

        while (upper + 1 < m_points.size() && x > m_points[upper].*field)
            ++upper;

        return { m_points[upper - 1], m_points[upper] };
    }

    std::vector<Point> m_points;
    bool m_logarithmic = false;
};

/**
 * A knob (a rotary juce::Slider, dragged up and down) with an optional taper.
 *
 * The taper lives in the knob's own position hooks, not in its NormalisableRange: a SliderAttachment sets the slider's
 * range from its parameter whenever it is made, so a taper kept there would be lost as soon as an attachment was made
 * again; here it stays.
 *
 * A drag moves the knob through its whole travel in getMouseDragSensitivity() pixels, as JUCE's own drag does; with
 * Shift, Cmd or Ctrl held the same move goes a tenth as far, for fine steps. A fine drag follows how far the mouse has
 * moved, not how fast, so a slow move moves the knob too. (JUCE's own fine drag, its velocity mode, ignores moves of a
 * pixel or two, and at most editor sizes a slow move is made of nothing else.) The keys can be pressed or let go
 * mid-drag: the knob carries on from where it is, without a jump.
 *
 * A press that does not move keeps the value as it is. As a drag starts, JUCE works the value out from the knob's
 * position even before the mouse moves, and a value taken to its position and back can land on a neighbouring step.
 */
class TaperedKnob : public juce::Slider
{
public:
    TaperedKnob()
    {
        // Every drag is the knob's own (mouseDrag()): JUCE's velocity mode stays off, whatever keys are held.
        setVelocityModeParameters(1.0, 1, 0.0, false);
    }

    void setTaper(std::optional<KnobTaper> taper)
    {
        m_taper = std::move(taper);
        repaint();
    }

    const KnobTaper* getTaper() const noexcept { return m_taper.has_value() ? &*m_taper : nullptr; }

    double proportionOfLengthToValue(double proportion) override
    {
        return m_taper.has_value() ? m_taper->toValue(proportion) : juce::Slider::proportionOfLengthToValue(proportion);
    }

    double valueToProportionOfLength(double value) override
    {
        return m_taper.has_value() ? m_taper->toPosition(value) : juce::Slider::valueToProportionOfLength(value);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        m_moved = false;
        juce::Slider::mouseDown(e);   // starts a drag (startedDragging()), unless the press resets the knob (Alt-click)

        m_fine = isFineDrag(e.mods);
        m_anchorPosition = m_lastPosition = valueToProportionOfLength(getValue());
        m_anchorY = m_lastY = e.position.y;
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        m_moved = m_moved || e.getDistanceFromDragStart() > 0;

        // Only a drag JUCE started: not after an Alt-click reset, nor on a disabled knob.
        if (! m_dragging || getSliderStyle() != RotaryVerticalDrag)
        {
            juce::Slider::mouseDrag(e);
            return;
        }

        // A key pressed or let go mid-drag: carry on from where the knob is, at the new rate.
        if (const bool fine = isFineDrag(e.mods); fine != m_fine)
        {
            m_fine = fine;
            m_anchorPosition = m_lastPosition;
            m_anchorY = m_lastY;
        }

        // From the anchor, not from the last value: a parameter's steps never swallow small moves.
        const double travel = static_cast<double>(getMouseDragSensitivity()) * (m_fine ? c_fineTravel : 1.0);
        m_lastPosition = juce::jlimit(0.0, 1.0, m_anchorPosition + static_cast<double>(m_anchorY - e.position.y) / travel);
        m_lastY = e.position.y;
        setValue(snapValue(proportionOfLengthToValue(m_lastPosition), absoluteDrag), juce::sendNotificationSync);
    }

    void startedDragging() override { m_dragging = true; }
    void stoppedDragging() override { m_dragging = false; }

    double snapValue(double attemptedValue, DragMode mode) override
    {
        return (mode != notDragging && ! m_moved) ? getValue() : attemptedValue;
    }

    static constexpr double c_fineTravel = 10.0;   // a fine drag needs this many times the mouse travel

private:
    static bool isFineDrag(const juce::ModifierKeys& mods) noexcept
    {
        return mods.isShiftDown() || mods.isCommandDown() || mods.isCtrlDown();
    }

    std::optional<KnobTaper> m_taper;
    bool m_moved = false;
    bool m_dragging = false;           // between JUCE's startedDragging() and stoppedDragging()
    bool m_fine = false;
    double m_anchorPosition = 0.0;     // the knob's position (0..1) where the drag, or its last change of rate, began
    double m_lastPosition = 0.0;
    float m_anchorY = 0.0f, m_lastY = 0.0f;
};

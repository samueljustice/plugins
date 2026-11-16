#pragma once

#include <JuceHeader.h>
#include "Theme.h"

#include <array>
#include <functional>
#include <memory>

/**
 * The harmonic editor: eighteen bars drawn like a small clickable spectrum, lowest pitch on the left. S3 and S2 are the
 * deeper subs, sines three and two octaves below the input; H1 is the sub itself, an octave below, and H2..H16 are its
 * harmonics. Each bar is bound to its parameter ("subOctave3", "subOctave2", "harmonic1".."harmonic16", 0..100 %).
 * Harmonic k sounds at k/2 times the input pitch, so H2 is the input note, H3 a fifth above it and H4 an octave above;
 * S2 sounds at 1/4 of it and S3 at 1/8.
 *
 * Bars are named by a number: k for Hk (1..16), c_subOctave2 and c_subOctave3 for S2 and S3, 0 for none.
 *
 * Mouse: click toggles a bar off and back on, dragging sets its level (top = 100 %),
 * dragging across bars draws a spectrum (one host gesture per bar), double-click or
 * alt-click resets to the default, the wheel nudges by 5 % per notch (one host gesture per
 * scroll) and shift-drag is a fine adjustment that stays on its bar. The click tolerance is
 * in screen pixels, so it holds when the editor is scaled.
 *
 * Message thread only: meterSource is polled at 30 Hz and the strip repaints only
 * the bars whose meter glow moved visibly.
 */
class HarmonicStrip : public juce::Component, private juce::Timer
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HarmonicStrip)

public:
    static constexpr int c_numHarmonics = 16;
    static constexpr int c_subOctave2 = -2;   // S2: two octaves below the input
    static constexpr int c_subOctave3 = -3;   // S3: three octaves below the input
    static constexpr int c_numBars = c_numHarmonics + 2;

    /** meterSource(bar) returns 0..1, how much of that bar's set level is sounding now. inputPitchSource returns the
        input's pitch in Hz, 0 while there is none: an S2 or S3 readout then says when that pitch puts the bar below
        20 Hz, where it is not generated. */
    HarmonicStrip(juce::AudioProcessorValueTreeState& parameters, const Theme::Fonts& fonts,
                  std::function<float(int bar)> meterSource, std::function<float()> inputPitchSource = {});
    ~HarmonicStrip() override;

    /** Outline "ghost" bars behind the fills: levels01[k] (0..1) for H1..H16 (k = 1..16, index 0 unused). */
    void setGhostProfile(const std::array<float, 17>& levels01);
    void clearGhostProfile();

    /** Polls meterSource now (the timer calls this) and repaints the bars whose glow moved visibly. */
    void refreshMeters();

    /** A bar's level area in local coordinates: its bottom edge is 0 % and its top edge 100 %. Empty for no bar. */
    juce::Rectangle<float> getBarArea(int bar) const;

    /** The bar whose column contains local x, or 0 outside the strip. */
    int getBarAt(float x) const;

    /** A bar relative to the input pitch: "1/8", "1/4", "1/2", "1x", "1.5x" ... "8x". */
    static juce::String getRatioText(int bar);

    /** Nearest interval from the input pitch to a bar, e.g. "octave + major 3rd (-14 cents)" or "two octaves below". */
    static juce::String getIntervalText(int bar);

    /** The hover readout, e.g. "H5 - 2.5x input - octave + major 3rd (-14 cents) - 24%", or
        "S2 - 1/4 of the input - two octaves below - 40%" (plus "below 20 Hz at this pitch" while that silences it). */
    juce::String getReadoutText(int bar) const;

    /** Where a bar's hover readout is drawn, in local coordinates (empty for no bar). */
    juce::Rectangle<float> getReadoutBounds(int bar) const;

    void paint(juce::Graphics&) override;
    void resized() override;

    void mouseEnter(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Bar
    {
        juce::RangedAudioParameter* parameter = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attachment;

        float value = 0.0f;        // parameter value (%), mirrored by the attachment
        float level = 0.0f;        // the same value normalised to 0..1: the fill height
        float remembered = 0.0f;   // normalised level before a click turned the bar off
        float meter = 0.0f;        // meterSource reading with a short visual release
        float paintedMeter = 0.0f; // the meter value the last repaint drew
        float ghost = 0.0f;
        bool belowFloor = false;   // S2 / S3: the input's pitch puts it at or below 20 Hz (as last polled)

        juce::Rectangle<float> column, caption, slot, area, label;
        juce::Rectangle<float> captionText; // the ratio caption's text extent, which the hover readout must not cut through
    };

    struct ReadoutParts
    {
        juce::String name, details, value, note;
    };

    struct ReadoutLayout
    {
        ReadoutParts parts;
        float nameWidth = 0.0f, detailsWidth = 0.0f, valueWidth = 0.0f, noteWidth = 0.0f, padding = 0.0f;
        juce::Rectangle<float> bounds;
    };

    void timerCallback() override;

    // Columns run left to right: S3, S2, H1 .. H16.
    static bool isBar(int bar);
    static bool isSubOctave(int bar) { return bar == c_subOctave2 || bar == c_subOctave3; }
    static int columnOf(int bar);
    static int barInColumn(int column);
    static juce::String labelFor(int bar);

    Bar& barFor(int bar);
    const Bar& barFor(int bar) const;

    void parameterChanged(int bar, float value);
    bool isBelowFloor(int bar) const;

    int clampedBarAt(float x) const;
    int hotBar() const;
    void setHovered(int bar);

    // Edits, each sent to the host as one complete gesture.
    void setLevel(int bar, float level);
    void toggle(int bar);
    void resetToDefault(int bar);

    // A scroll of the wheel keeps one gesture open on its bar until the wheel pauses.
    void nudge(int bar, int steps);
    void beginWheelGesture(int bar);
    void endWheelGesture();

    // Drags keep one gesture open on the bar under the mouse.
    void beginDragGesture(int bar, float y);
    void endDragGesture();
    void dragActiveBarTo(float y);
    void drawAcrossTo(int bar, juce::Point<float> from, juce::Point<float> to);

    ReadoutParts getReadoutParts(int bar) const;
    ReadoutLayout layoutReadout(int bar) const;
    void paintBar(juce::Graphics&, int bar, bool hot, juce::Rectangle<float> readoutBounds) const;
    void paintReadout(juce::Graphics&, int bar, const ReadoutLayout&) const;

    const Theme::Fonts& m_fonts;
    std::function<float(int)> m_meterSource;
    std::function<float()> m_inputPitchSource;

    std::array<Bar, c_numBars> m_bars;               // by column
    std::array<juce::String, c_numBars> m_labelTexts, m_ratioTexts;
    bool m_hasGhost = false;

    // Layout, derived from the bounds in resized().
    juce::Font m_captionFont { juce::FontOptions() };
    juce::Font m_labelFont { juce::FontOptions() };
    juce::Font m_readoutFont { juce::FontOptions() };
    float m_cornerRadius = 3.0f;
    float m_capThickness = 2.0f;
    float m_readoutHeight = 14.0f;

    // Mouse state (bars, 0 = none)
    int m_hoveredBar = 0;
    int m_pressedBar = 0;  // bar under the last mouse-down
    int m_activeBar = 0;   // bar with an open drag gesture
    int m_paintBar = 0;    // bar a plain drag draws from; differs from the active bar only after a fine drag drifted off it
    juce::Point<float> m_pressPosition, m_lastDragPosition;
    float m_clickTolerance = 3.0f; // in local units: 3 screen pixels at the editor's current scale
    bool m_isDragging = false;
    bool m_pressWasDrag = false; // so the double-click that ends a drag doesn't also reset
    bool m_fineDrag = false;
    float m_anchorLevel = 0.0f;  // drag level = anchor level + (anchor y - y) / bar height * scale
    float m_anchorY = 0.0f;
    float m_dragScale = 1.0f;

    float m_wheelAccumulator = 0.0f; // the part of a notch not stepped yet, this scroll only
    int m_wheelBar = 0;              // bar under the last wheel event
    bool m_wheelWasSmooth = false;
    juce::Time m_lastWheelTime;
    int m_wheelGestureBar = 0;       // bar with an open wheel gesture (0 = none)
    juce::TimedCallback m_wheelGestureTimer { [this] { endWheelGesture(); } }; // ends it when the wheel pauses
};

#pragma once

#include <JuceHeader.h>
#include "Theme.h"

#include <array>
#include <functional>
#include <memory>

/**
 * The harmonic editor: sixteen bars for the sub oscillator's harmonics, H1 (the
 * sub itself) to H16, drawn like a small clickable spectrum. Each bar is bound to
 * the "harmonicK" parameter (0..100 %). Harmonic k sounds at k/2 times the input
 * pitch, so H2 is the input note, H3 a fifth above it and H4 an octave above.
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

    /** meterSource(k) returns 0..1, how much of harmonic k's set level is sounding now. */
    HarmonicStrip(juce::AudioProcessorValueTreeState& parameters, const Theme::Fonts& fonts,
                  std::function<float(int harmonic)> meterSource);
    ~HarmonicStrip() override;

    /** Outline "ghost" bars behind the fills: levels01[k] (0..1) for k = 1..16, index 0 unused. */
    void setGhostProfile(const std::array<float, 17>& levels01);
    void clearGhostProfile();

    /** Polls meterSource now (the timer calls this) and repaints the bars whose glow moved visibly. */
    void refreshMeters();

    /** Bar k's level area in local coordinates: its bottom edge is 0 % and its top edge 100 %. */
    juce::Rectangle<float> getBarArea(int harmonic) const;

    /** The harmonic whose column contains local x, or 0 outside the strip. */
    int getHarmonicAt(float x) const;

    /** Harmonic k relative to the input pitch: "1/2", "1x", "1.5x" ... "8x". */
    static juce::String getRatioText(int harmonic);

    /** Nearest interval from the input pitch to harmonic k, e.g. "octave + major 3rd (-14 cents)". */
    static juce::String getIntervalText(int harmonic);

    /** The hover readout, e.g. "H5 - 2.5x input - octave + major 3rd (-14 cents) - 24%". */
    juce::String getReadoutText(int harmonic) const;

    /** Where harmonic k's hover readout is drawn, in local coordinates (empty outside 1..16). */
    juce::Rectangle<float> getReadoutBounds(int harmonic) const;

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

        juce::Rectangle<float> column, caption, slot, area, label;
        juce::Rectangle<float> captionText; // the ratio caption's text extent, which the hover readout must not cut through
    };

    struct ReadoutParts
    {
        juce::String name, details, value;
    };

    struct ReadoutLayout
    {
        ReadoutParts parts;
        float nameWidth = 0.0f, detailsWidth = 0.0f, valueWidth = 0.0f, padding = 0.0f;
        juce::Rectangle<float> bounds;
    };

    void timerCallback() override;

    Bar& barFor(int harmonic);
    const Bar& barFor(int harmonic) const;

    void parameterChanged(int harmonic, float value);

    int clampedHarmonicAt(float x) const;
    int hotHarmonic() const;
    void setHovered(int harmonic);

    // Edits, each sent to the host as one complete gesture.
    void setLevel(int harmonic, float level);
    void toggle(int harmonic);
    void resetToDefault(int harmonic);

    // A scroll of the wheel keeps one gesture open on its bar until the wheel pauses.
    void nudge(int harmonic, int steps);
    void beginWheelGesture(int harmonic);
    void endWheelGesture();

    // Drags keep one gesture open on the bar under the mouse.
    void beginDragGesture(int harmonic, float y);
    void endDragGesture();
    void dragActiveBarTo(float y);
    void drawAcrossTo(int harmonic, juce::Point<float> from, juce::Point<float> to);

    ReadoutParts getReadoutParts(int harmonic) const;
    ReadoutLayout layoutReadout(int harmonic) const;
    void paintBar(juce::Graphics&, int harmonic, bool hot, juce::Rectangle<float> readoutBounds) const;
    void paintReadout(juce::Graphics&, int harmonic, const ReadoutLayout&) const;

    const Theme::Fonts& m_fonts;
    std::function<float(int)> m_meterSource;

    std::array<Bar, c_numHarmonics> m_bars;
    std::array<juce::String, c_numHarmonics> m_labelTexts, m_ratioTexts;
    bool m_hasGhost = false;

    // Layout, derived from the bounds in resized().
    juce::Font m_captionFont { juce::FontOptions() };
    juce::Font m_labelFont { juce::FontOptions() };
    juce::Font m_readoutFont { juce::FontOptions() };
    float m_cornerRadius = 3.0f;
    float m_capThickness = 2.0f;
    float m_readoutHeight = 14.0f;

    // Mouse state
    int m_hoveredHarmonic = 0;
    int m_pressedHarmonic = 0;  // bar under the last mouse-down (0 = none)
    int m_activeHarmonic = 0;   // bar with an open drag gesture (0 = none)
    int m_paintHarmonic = 0;    // column a plain drag draws from; differs from the active bar only after a fine drag drifted off it
    juce::Point<float> m_pressPosition, m_lastDragPosition;
    float m_clickTolerance = 3.0f; // in local units: 3 screen pixels at the editor's current scale
    bool m_isDragging = false;
    bool m_pressWasDrag = false; // so the double-click that ends a drag doesn't also reset
    bool m_fineDrag = false;
    float m_anchorLevel = 0.0f;  // drag level = anchor level + (anchor y - y) / bar height * scale
    float m_anchorY = 0.0f;
    float m_dragScale = 1.0f;

    float m_wheelAccumulator = 0.0f; // the part of a notch not stepped yet, this scroll only
    int m_wheelHarmonic = 0;         // bar under the last wheel event
    bool m_wheelWasSmooth = false;
    juce::Time m_lastWheelTime;
    int m_wheelGestureHarmonic = 0;  // bar with an open wheel gesture (0 = none)
    juce::TimedCallback m_wheelGestureTimer { [this] { endWheelGesture(); } }; // ends it when the wheel pauses
};

#pragma once

#include <JuceHeader.h>
#include "EqGraph.h"
#include "Theme.h"

#include <array>

class SwooshbucklerAudioProcessor;

/**
 * THE "OUTPUT EQ" PLATE (design 2.2): a hard-edged plate that opens directly above Output, over Bus B, so Output's
 * GR, Out, Ceiling, Trim and speaker meters stay in view while a band is boosted.
 *
 * On it: the graph (EqGraph, 576 x 110) and, under it, one row: the readout of the band touched last
 * ("Low Mid  320 Hz  -3.0 dB  Q 1.0"; a cut reads its frequency or "Off" and its slope; before anything is touched,
 * a line saying what to do), then the keys
 *   On      ParamIDs::eqOn: off, the EQ passes the sound untouched and the curve is drawn unlit
 *   24 dB   ParamIDs::eqLowCutSlope: Low Cut at 24 dB an octave (off: 12). It rests while Low Cut is off
 *   Flat    every gain to 0 dB and both cuts parked, one undo step; frequencies and Qs stay
 *   Close   calls onClose
 * all in the Output colour.
 *
 * What the callers rely on (PluginEditor): it constructs one, places it at bounds() (window pixels at 1.0 scale, in
 * the scaled content), shows it when the EQ key is clicked and hides Bus B's controls while it is up; onClose is
 * called by the Close key and by Esc (the plate takes the keyboard when it is shown), and the editor then hides it
 * and brings Bus B back. refresh() about 30 times a second while shown.
 */
class EqPlate : public juce::Component
{
public:
    static constexpr int c_x = 908, c_y = 430, c_width = 600, c_height = 156;
    static juce::Rectangle<int> bounds() { return { c_x, c_y, c_width, c_height }; }
    /** The layout inside it: the graph, then the row under it (the readout at its left, the four keys at its right). */
    static constexpr int c_graphX = 12, c_graphY = 9, c_rowY = 125, c_rowHeight = 22, c_keyGap = 4;
    static constexpr int c_onWidth = 52, c_slopeWidth = 62, c_flatWidth = 48, c_closeWidth = 56;

    EqPlate(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts);
    ~EqPlate() override;

    std::function<void()> onClose;
    /** Message thread, about 30 times a second while shown. */
    void refresh();

    EqGraph& getGraph() { return m_graph; }
    /** Where the readout row's text goes, in this component's coordinates. */
    juce::Rectangle<int> readoutArea() const;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void visibilityChanged() override;

private:
    void updateKeys();

    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
    EqGraph m_graph;
    juce::TextButton m_on { "On" }, m_slope { "24 dB" }, m_flat { "Flat" }, m_close { "Close" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_onAttachment, m_slopeAttachment;
    EqGraph::Readout m_readout; // what the row shows now
    int m_readoutBand = -2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqPlate)
};

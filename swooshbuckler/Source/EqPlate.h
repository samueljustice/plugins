#pragma once

#include <JuceHeader.h>
#include "EqGraph.h"
#include "Theme.h"

class SwooshbucklerAudioProcessor;

/**
 * THE "OUTPUT EQ" PLATE (a stub: it draws nothing; filled in by the EQ view's work package).
 *
 * What the filler must provide (design 2.2): a plate of c_bounds (window pixels, over Bus B, directly above Output,
 * so Output's GR, Out, Ceiling, Trim and speaker meters stay in view) holding the EqGraph and, under it, the readout
 * row ("Low Mid  320 Hz  -3.0 dB  Q 1.0" for the selected band) with the keys On (ParamIDs::eqOn), 24 dB
 * (eqLowCutSlope), Flat (every gain to 0 dB and both cuts parked, one undo step) and Close. Hard-edged plate, text
 * inside its box at 0.85, 1.0 and 1.5 scale.
 *
 * What the callers rely on (PluginEditor, the layers' package): it constructs one, places it at c_bounds (scaled
 * with the rest of the window), shows it when the EQ key is clicked and hides Bus B's controls while it is up;
 * onClose is called by the Close key and by Esc (keyPressed), and the editor then hides it and brings Bus B back.
 * refresh() about 30 times a second while shown.
 */
class EqPlate : public juce::Component
{
public:
    static constexpr int c_x = 908, c_y = 430, c_width = 600, c_height = 156;
    static juce::Rectangle<int> bounds() { return { c_x, c_y, c_width, c_height }; }

    EqPlate(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts);
    ~EqPlate() override;

    std::function<void()> onClose;
    void refresh() {}

    void paint(juce::Graphics&) override {}
    void resized() override {}

private:
    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
};

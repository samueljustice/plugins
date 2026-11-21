#pragma once

#include <JuceHeader.h>
#include "OutputEq.h"
#include "Theme.h"

class SwooshbucklerAudioProcessor;

/**
 * THE OUTPUT EQ'S GRAPH (a stub: it draws nothing; filled in by the EQ view's work package, which also owns
 * EqPlate, OffsetMeter and SeaVisualiser).
 *
 * What the filler must provide (design 2.2): 576 x 110, 20 Hz .. 20 kHz on a log axis, +-12 dB; the output's
 * spectrum faint behind; the curve as OutputEq::responseDb() gives it at the processor's render rate; six square
 * handles in the Output colour, one per OutputEq::Band. Drag a handle for frequency and gain (a cut slides sideways
 * only, and pulling one in from its parked end stop turns it on); the mouse wheel or Cmd-drag sets a bell's Q;
 * double-click flattens the band or parks the cut; right-click types values. Each drag is one undo step
 * (EditHistory::Group). It writes the ParamIDs::eq* parameters with begin/end gestures.
 *
 * What it reads: the settings from SwooshbucklerAudioProcessor::getOutputEqParams() and the rate from
 * getRenderRate() (the curve is the true discrete response there: OutputEq::responseDb, one band or all of them);
 * the spectrum from copyScope() (what the sea reads: the output's mono sum after the EQ and the output stage, at
 * getSampleRate(), the device's rate), through the helper its package factors out of SeaVisualiser::analyse().
 *
 * What the callers rely on: the plate (EqPlate) owns one and reads getSelectedBand() for its readout row; the EQ
 * key in Output's title bar (PluginEditor, the layers' package) draws its miniature with paintMiniature().
 */
class EqGraph : public juce::Component
{
public:
    static constexpr int c_width = 576, c_height = 110;
    static constexpr float c_minHz = 20.0f, c_maxHz = 20000.0f, c_rangeDb = 12.0f;

    EqGraph(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts);
    ~EqGraph() override;

    /** The band whose handle was touched last (OutputEq::Band), or -1. */
    int getSelectedBand() const { return m_selected; }
    /** Called when the selected band or its values change (the plate's readout follows). */
    std::function<void()> onSelectionChanged;
    /** Message thread, about 30 times a second while shown: the spectrum and the curve. */
    void refresh() {}

    /** The EQ key's picture: the curve of `p` in `area` (a flat line when isFlat(p)), drawn unlit when `lit` is false. */
    static void paintMiniature(juce::Graphics& g, juce::Rectangle<float> area, const OutputEq::Params& p, double sampleRate,
                               juce::Colour colour, bool lit);

    void paint(juce::Graphics&) override {}

private:
    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
    int m_selected = -1;
};

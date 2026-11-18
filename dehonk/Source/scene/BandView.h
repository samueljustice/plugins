// The band view: the working picture of what De-Honk is doing, band by band. Where the pond is
// the fun reading, this is the exact one: the spectrum going in, what is taken out of each band,
// where the modules' edges are, the programme level against the threshold, and the Click module.
// Its frequency axis is the same as the pond's, so the two line up when stacked.
#pragma once

#include <JuceHeader.h>

#include "../../dsp/DeHonkCore.h"

#include <functional>

namespace dehonk::scene
{

struct BandViewState
{
    DisplayFrame frame;                         // latest detector frame
    bool hasFrame = false;                      // false until audio has run
    bool moduleOn[kNumModules] {};
    bool snapOn = false;
    float edgeHz[kNumEdges] {};                 // edges as the core is using them
    float thresholdDb = -16.0f;
    float maxCutDb = 6.0f;                      // the ceiling on any one cut: a line across the cut lane
    bool soloHonk = false, diff = false, bypass = false;
};

class BandView : public juce::Component,
                 public juce::SettableTooltipClient
{
public:
    BandView();

    /** Call at the display rate with the latest state and the time since the last call. The view
        smooths the levels and holds the cuts itself, and repaints. */
    void update (const BandViewState&, double seconds);

    /** Dragging an edge. The host begins and ends the parameter gesture, and sets the parameter
        from onEdgeDragged (the view asks for a frequency; the host applies its limits, and the new
        value comes back through update()). */
    std::function<void (int edge)> onEdgeDragStarted, onEdgeDragEnded;
    std::function<void (int edge, float hz)> onEdgeDragged;

    /** A height that suits a given width. */
    static int heightForWidth (int width) noexcept { return juce::jlimit (112, 170, width / 7); }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> plot() const noexcept;
    float xForHz (float hz) const noexcept;
    float hzForX (float x) const noexcept;
    int edgeAt (juce::Point<float>) const noexcept;

    BandViewState state;
    float level[kMaxBands] {};      // smoothed input level, dB
    float cut[kMaxBands] {};        // held cut, dB
    float clickHeld = 0.0f, programme = -120.0f;
    int hovered = -1, dragging = -1;
};

} // namespace dehonk::scene

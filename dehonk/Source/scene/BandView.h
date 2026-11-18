// The band view: the working picture of what De-Honk is doing, band by band. Where the pond is
// the fun reading, this is the exact one: the spectrum going in, what is taken out of each band,
// where the ranges' edges are, the programme level against the threshold, and Snap. The edges,
// the Threshold and Max Cut are set here by dragging them.
// Its frequency axis is the same as the pond's, so the two line up when stacked.
#pragma once

#include <JuceHeader.h>

#include "../../dsp/DeHonkCore.h"
#include "../Theme.h"

#include <array>
#include <functional>
#include <vector>

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

    /** What can be taken hold of in the picture: the five range edges (0 to 4), the Threshold (its
        grip on the level bar and its line across the bands) and Max Cut (the dashed line across the
        cut lane). */
    enum Handle { edge0 = 0, thresholdHandle = kNumEdges, maxCutHandle, numHandles, noHandle = -1 };

    /** Dragging a handle. The host begins and ends the parameter gesture, and sets the parameter
        from onHandleDragged (the view asks for a value: Hz for an edge, dBFS for the Threshold, dB
        for Max Cut; the host applies its limits, and the new value comes back through update()).
        onHandleReset is a double-click: put it back to its default. */
    std::function<void (int handle)> onHandleDragStarted, onHandleDragEnded, onHandleReset;
    std::function<void (int handle, float value)> onHandleDragged;

    /** The range being worked on alone: 0 Woof, 1 Tub, 2 Fog, 3 Bark, 4 Snap, -1 none. The others
        are drawn paler. */
    void setFocus (int range);

    /** A height that suits a given width. */
    static int heightForWidth (int width) noexcept { return juce::jlimit (112, 170, width / 7); }

    /** The rectangle the bands are plotted in, in this component's coordinates. The frequency
        axis runs across its width. */
    juce::Rectangle<float> plotArea() const noexcept { return plot(); }

    /** Says what is under the pointer: a handle's own help, or the picture's. */
    juce::String getTooltip() override;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> plot() const noexcept;
    juce::Rectangle<float> lane() const noexcept;
    juce::Rectangle<float> levels() const noexcept;
    juce::Rectangle<float> meter() const noexcept;
    float xForHz (float hz) const noexcept;
    float hzForX (float x) const noexcept;
    float yForLevel (float db) const noexcept;
    float yForMaxCut() const noexcept;
    int handleAt (juce::Point<float>) const noexcept;
    void setHovered (int handle);
    bool inFocus (int range) const noexcept { return focus < 0 || focus == range; }

    // Text is shaped once and kept: laying out a line of text costs far more than drawing it, and
    // the same few lines are drawn at every picture.
    enum Face { nameFace, valueFace, bodyFace };
    struct Shaped
    {
        juce::String text;
        Face face = nameFace;
        float size = 0.0f, width = 0.0f, centreToBaseline = 0.0f, height = 0.0f;
        juce::GlyphArrangement glyphs;      // laid out from x = 0 on the baseline y = 0
    };
    const Shaped& shaped (const juce::String& text, Face face, float size) const;
    static void drawShaped (juce::Graphics&, const Shaped&, juce::Rectangle<float> area, bool centred);
    void chip (juce::Graphics&, const juce::String& text, juce::Point<float> anchor, juce::Colour fill, bool fromLeft = false) const;

    using BackdropKey = std::array<float, kNumEdges + kNumModules + 10>;
    void renderBackdrop (float scale);
    void paintMoving (juce::Graphics&);     // everything that is not in the backdrop

    juce::SharedResourcePointer<Theme::Fonts> fonts;
    mutable std::vector<Shaped> shapedTexts;
    juce::Image backdrop;                   // what stays put, at the screen's own scale
    juce::Image picture;                    // the backdrop with everything else drawn over it, made at each paint
    BackdropKey backdropKey {};
    juce::RectangleList<int> nameAreas;     // where the names are, so the Max Cut line can step round them
    struct Name { juce::String text; juce::Rectangle<float> area; float alpha = 1.0f; };
    std::vector<Name> names;                // the ranges' names, placed with the backdrop and drawn over the bars
    juce::Point<float> dragFrom;            // where the pointer was at the last step of a drag
    float dragValue = 0.0f;                 // what the handle being dragged has got to, before rounding
    bool over = false;                      // the mix is over the Threshold (see paint)
    BandViewState state;
    float level[kMaxBands] {};      // smoothed input level, dB
    float cut[kMaxBands] {};        // held cut, dB
    float clickHeld = 0.0f, programme = -120.0f;
    int hovered = noHandle, dragging = noHandle, focus = -1;
    bool draggedSinceDown = false;
};

} // namespace dehonk::scene

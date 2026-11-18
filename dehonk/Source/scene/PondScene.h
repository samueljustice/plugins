// The picture in De-Honk's window: a low-polygon pond with smooth shading and chunky, rounded
// figures, drawn in software into an image the size of its card in the window, in device pixels.
//
// This file is the contract between the scene (which only draws) and the display component that
// hosts it (which owns the clock, the mouse and the frame round the picture). The host may run the
// scene on a thread of its own: one thread at a time calls advance, render, click and the other
// members; Hits, and the static members, may be used anywhere.
#pragma once

#include <JuceHeader.h>

#include "../../dsp/DeHonkCore.h"

#include <array>
#include <memory>

namespace dehonk::scene
{

/** Everything the scene needs to know each frame. */
struct SceneInput
{
    DisplayFrame frame;                         // latest detector frame (levels, notches, module cuts, click)
    bool moduleOn[kNumModules] {};
    bool snapOn = false;
    float edgeHz[kNumEdges] {};                 // edges as the core is using them
    float thresholdDb = -16.0f;
    float maxCutDb = 6.0f;                      // the Max Cut control
    float guardAmount = 1.0f;                   // the Impact Guard control, 0..1 (0 = off)
    bool soloHonk = false, diff = false, bypass = false;
    bool calm = false;                          // true: no walking or chasing; the picture holds still
    int focusModule = -1;                       // the range in focus: 0 Woof, 1 Tub, 2 Fog, 3 Bark, 4 Snap; -1 none.
                                                // With one in focus only its creatures move; the rest hold still, faded back.
    float axisLeft = 0.0f, axisRight = 1.0f;    // where the frequency axis starts and ends across the picture (0..1): the
                                                // span of the band view's plot, so that the trees and posts stand over its handles
};

class PondScene
{
public:
    PondScene();
    ~PondScene();

    /** A good size to draw the scene at. The host may use any size and shape of at least
        160 x 90: the frequency axis always spans the full width and the bank stays on kAxisY, and in
        a very wide picture the characters are made smaller so that they still fit. */
    static constexpr int kWidth = 480, kHeight = 270;

    /** Moves the scene on by `seconds` (clamped inside to a sensible step).
        Everything moves by the time that has passed, so any frame rate looks the same. */
    void advance (const SceneInput&, double seconds);

    /** How the scene's own picture is fitted to the image it is drawn into. */
    enum class Fit
    {
        same,       // the scene is drawn at the image's size
        halved,     // at twice the size each way and averaged down: smooth edges (four samples to the pixel)
        doubled     // at half the size each way and doubled smoothly: the cheapest, and the softest
    };

    /** Draws the current state into `target`, an ARGB image (every pixel is written opaque).
        The image should be the size of the card in device pixels, so that it goes to the window
        pixel for pixel; `pixelsPerPoint` is how many of the scene's own pixels make a point of
        the window (the display's scale, times two when halved, half when doubled). The scene's
        own size must come to at least 160 x 90. */
    void render (juce::Image& target, Fit fit = Fit::same, float pixelsPerPoint = 1.0f);

    /** True while anything in the scene is still moving. When false the host may drop to a few
        frames a second. */
    bool isAnimating() const noexcept;
    /** True while rain is falling with nothing else moving: the host may then draw at a modest
        rate (twenty-odd pictures a second is plenty for rain). */
    bool isRaining() const noexcept;

    /** The mouse. Positions are 0..1 across and down the picture, as last drawn. */
    bool somethingAt (float x01, float y01) const noexcept;

    /** Where the mouse can find someone, as last drawn: a small copy, to keep with the picture so
        that the pointer is asked about what is on the screen, from any thread. */
    struct Hits
    {
        struct Spot { float x = 0, y = 0, rx = 1, ry = 1; int kind = 0, index = 0; };
        std::array<Spot, 24> spots {};
        int count = 0, width = 1, height = 1;     // the picture's own size, as drawn
        bool somethingAt (float x01, float y01) const noexcept;
    };
    Hits hits() const noexcept;

    /** A click on the picture. True if someone was there to take it. What they do is for show
        only and changes nothing the mix drives; it starts at the next advance(). */
    bool click (float x01, float y01) noexcept;

    /** The crown of the tree that stands at each range edge, as last drawn: its centre (0..1
        across; `y` and `radius` in widths of the picture, y measured down from the top edge and
        negative above it) and the colour of its top. The host draws whatever rises above the
        picture's top edge. */
    struct TreeTop { float x = 0, y = 0, radius = 0; juce::Colour colour; };
    std::array<TreeTop, kNumEdges> treeTops() const noexcept;

    /** The frequency axis runs straight across the picture, left to right, logarithmic, between
        SceneInput::axisLeft and axisRight. Position 0..1 along the axis for a frequency, and back. */
    static float xForHz (float hz) noexcept;
    static float hzForX (float x01) noexcept;

    /** The vertical position (0..1 down the image) of the water's edge along the frequency axis.
        Below about 0.89 runs a stream with the small creatures in it, so nothing should be drawn
        over the bottom of the picture either. */
    static constexpr float kAxisY = 0.74f;

    /** How long the last render() took, in milliseconds (drawing, and copying into the image). */
    double lastRenderMs() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace dehonk::scene

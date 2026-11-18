// The picture in De-Honk's window: a low-polygon pond with smooth shading and chunky, rounded
// figures, drawn in software into an image that the editor scales up smoothly to fill its panel.
//
// This file is the contract between the scene (which only draws) and the display component that
// hosts it (which owns the timer, the mouse and the readouts drawn on top).
#pragma once

#include <JuceHeader.h>

#include "../../dsp/DeHonkCore.h"

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

    /** Moves the scene on by `seconds` (clamped inside to a sensible step). Message thread. */
    void advance (const SceneInput&, double seconds);

    /** Draws the current state into `target`: a software image (juce::SoftwareImageType) in
        juce::Image::RGB format, at least 160 x 90. */
    void render (juce::Image& target);

    /** True while anything in the scene is still moving. When false the host may drop to a few
        frames a second. */
    bool isAnimating() const noexcept;

    /** The frequency axis runs straight across the picture, left to right, logarithmic.
        Position 0..1 across the image width for a frequency, and back. */
    static float xForHz (float hz) noexcept;
    static float hzForX (float x01) noexcept;

    /** The vertical position (0..1 down the image) of the water's edge along the frequency axis.
        Below about 0.89 runs a stream with the small creatures in it, so nothing should be drawn
        over the bottom of the picture either. */
    static constexpr float kAxisY = 0.74f;

    /** How long the last render() took, in milliseconds. */
    double lastRenderMs() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace dehonk::scene

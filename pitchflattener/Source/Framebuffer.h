#pragma once

#include <JuceHeader.h>

#include "Hud.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

/**
 * The console framebuffer: the whole editor drawn the way a 32-bit console's screen looked.
 *
 * The host (the editor) draws itself and its children into a 1x image through a FramebufferContext.
 * The image is then finished, only over the area being repainted:
 *   - chunky pixels (shapes, frames, big type, the scope's traces) are reduced to 2 x 2 blocks. Each
 *     block takes the one of its four pixels that differs most from their average, so a thin line
 *     of either brightness survives as a solid block and an edge steps hard, with no smoothing;
 *   - fine pixels (text under 16 px) stay at 1x, so small words keep their shapes;
 *   - raw pixels (the flight view, already a 5-bit picture on its own coarser grid) pass as they are;
 *   - then a faint vignette and scanlines, and the same ordered 4 x 4 dither to 5 bits a channel as
 *     the flight view's (flight::dither5), over everything but the raw pixels.
 * The result is drawn scaled up with no smoothing, so on a Retina screen a fine pixel is 2 x 2 device
 * pixels and a chunky block 4 x 4.
 *
 * Only what changed is drawn again: the area JUCE asks to repaint, widened to whole blocks.
 */
class Framebuffer
{
public:
    void setSize (int width, int height);

    /** Draws `region` of the host afresh: paintHost draws the host's own content; its visible
        children are drawn over it, then the region is finished. */
    void render (juce::Component& host, juce::Rectangle<int> region, const std::function<void (juce::Graphics&)>& paintHost);
    /** An area widened to whole 2 x 2 blocks. A repaint with an odd edge redraws the blocks across
        that edge, so the screen outside it must be redrawn too (see the editor's paint()). */
    static juce::Rectangle<int> blockAligned (juce::Rectangle<int>) noexcept;

    /** Draws the finished region, scaled with no smoothing. */
    void draw (juce::Graphics&, juce::Rectangle<int> region) const;

    double lastMilliseconds() const noexcept { return m_lastMs; }
    /** The finished picture (for the tests). */
    const juce::Image& output() const noexcept { return m_output; }

    /** An image ordered-dithered to 5 bits a channel in place (tooltips, which are windows of their own). */
    static void dither (juce::Image&);

private:
    void finish (juce::Rectangle<int> region);

    juce::Image m_source, m_output;
    std::vector<std::uint8_t> m_mask;
    std::vector<std::uint16_t> m_shade;   // vignette and scanline, 0..256 per pixel
    double m_lastMs = 0.0;
};

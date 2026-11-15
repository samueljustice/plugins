#pragma once

#include <JuceHeader.h>
#include "Theme.h"

#include <atomic>
#include <cstdint>
#include <vector>

/**
 * The drawing vocabulary every Pitch Flattener surface shares: chamfered panels (45-degree cuts at
 * the top-left and bottom-right corners, 1.5 px edges), speed stripes at headings, upright and
 * sheared text that is counted when it does not fit its box, and one animation clock.
 *
 * Text goes through drawText()/drawSheared() so the tests can ask whether anything anywhere was cut
 * or overran (overflowCount()); a picture that looks right at one size can still clip at another.
 */
namespace Hud
{
    //==========================================================================
    // The console framebuffer (Framebuffer.h). The editor draws everything into a 1x image through a
    // FramebufferContext, which keeps a mask of what each pixel is: chunky (shapes, big type: reduced
    // to 2 x 2 blocks), fine (small text: kept at 1x so it stays readable) or raw (the flight view's
    // own pixels, already dithered). Drawing code says which with the mark... functions below; they
    // do nothing when drawing anywhere else (a snapshot of a child, the About window).
    enum MaskValue : std::uint8_t { kChunky = 0, kFine = 1, kRaw = 2 };
    /** Text smaller than this is fine text (kept at 1x); bigger type is drawn as a 2 x 2 pixel font. */
    inline constexpr float kFineTextHeight = 24.0f;

    /** Marks an area (in the Graphics' own coordinates) fine or raw. */
    void markFine (juce::Graphics&, juce::Rectangle<float>);
    void markRaw (juce::Graphics&, juce::Rectangle<float>);
    /** Something opaque covers this shape: what was marked under it is chunky again. */
    void markCovered (juce::Graphics&, const juce::Path&);
    void markCovered (juce::Graphics&, juce::Rectangle<float>);
    /** Writes an image straight into the framebuffer, each pixel a `scale` x `scale` block, at the
        Graphics' origin (marked raw). False when not drawing into a framebuffer. */
    bool blitRaw (juce::Graphics&, const juce::Image&, int scale);

    /** A component inside a framebuffer host draws only into the framebuffer, never straight to the screen. */
    bool skipOnScreen (const juce::Component&);
    /** Marks a component as a framebuffer host (its children then draw only through it). */
    void setFramebufferHost (juce::Component&);
    /** While one of these exists, components draw for real (the framebuffer is being drawn). */
    struct FramebufferPass
    {
        FramebufferPass();
        ~FramebufferPass();
    };

    /** tan 12 degrees: how far sheared text leans forward per pixel of height. */
    inline constexpr float kShear = 0.2126f;
    inline constexpr float kEdge = 1.5f;

    /** The chamfered shape: top-left and bottom-right corners cut at 45 degrees by `cut`. */
    juce::Path chamfer (juce::Rectangle<float> r, float cut);

    /** A chamfered panel: optional fill, then its edge (inside the bounds, so it never clips). */
    void frame (juce::Graphics&, juce::Rectangle<float> r, float cut, juce::Colour fill, juce::Colour edge, float thickness = kEdge);

    /** Two or three parallel bars slanted forward, filling `area` from its left; returns their width. */
    float speedStripes (juce::Graphics&, juce::Rectangle<float> area, juce::Colour colour, int count = 3);

    /** A heading: speed stripes, then the title sheared, in `colour`. Returns the area it used. */
    juce::Rectangle<float> heading (juce::Graphics&, juce::Rectangle<float> row, const juce::String& title, const juce::Font&, juce::Colour colour);

    /** Width of a line of text as drawn (getStringWidth skips ligature placeholders, so this adds a space and takes it off). */
    float textWidth (const juce::Font&, const juce::String&);
    /** The width sheared text needs: its own plus the lean over its height. */
    float shearedWidth (const juce::Font&, const juce::String&);

    /** Upright text in one line; counted as an overflow when it does not fit `area`. */
    void drawText (juce::Graphics&, const juce::String&, juce::Rectangle<float> area, const juce::Font&, juce::Colour,
                   juce::Justification = juce::Justification::centredLeft);
    /** Text leaning forward 12 degrees (racing italic), kept inside `area`; counted when it does not fit. */
    void drawSheared (juce::Graphics&, const juce::String&, juce::Rectangle<float> area, const juce::Font&, juce::Colour,
                      juce::Justification = juce::Justification::centredLeft);

    /** The biggest size up to `maxHeight` (in half pixels) at which `text` in the font `make (h)` fits `width`, never below `minHeight`. */
    template <typename MakeFont>
    float fitHeight (MakeFont&& make, const juce::String& text, float width, float maxHeight, float minHeight = 10.0f, bool sheared = true)
    {
        for (float h = maxHeight; h > minHeight; h -= 0.5f)
        {
            const juce::Font f = make (h);
            if ((sheared ? shearedWidth (f, text) : textWidth (f, text)) <= width)
                return h;
        }
        return minHeight;
    }

    /** fitHeight for the display face (sheared) or the body face (upright). */
    float fittedHeight (const Theme::Fonts&, bool displayFace, const juce::String& text, float width, float maxHeight, float minHeight = 10.0f, bool sheared = true);

    /** Texts that did not fit their box since the process started (the tests require none). */
    int overflowCount() noexcept;
    void resetOverflowCount() noexcept;

    /** A ">" chevron pointing along `angle` (0 = right), stroked. */
    void chevron (juce::Graphics&, juce::Point<float> centre, float size, float angle, float thickness, juce::Colour);

    /**
     * One clock for every pulse, blink, wipe and flight frame, so they stay in step. Seconds since it
     * was made; tests set it by hand to draw any moment of an animation (a banner held and lit).
     */
    class Clock
    {
    public:
        Clock() : m_start (juce::Time::getMillisecondCounterHiRes()) {}

        double now() const noexcept
        {
            return m_manual ? m_manualTime : (juce::Time::getMillisecondCounterHiRes() - m_start) * 0.001;
        }

        void setManual (double seconds) noexcept
        {
            m_manual = true;
            m_manualTime = seconds;
        }


    private:
        double m_start;
        bool m_manual = false;
        double m_manualTime = 0.0;
    };

    /** A slow pulse 0..1 (a triangle wave, so it reads as a beat rather than a fade). */
    float pulse (double seconds, double hz = 1.2) noexcept;
}

/**
 * The software renderer the framebuffer draws through: JUCE's own, plus the mask of chunky, fine and
 * raw pixels, kept in the image's coordinates whatever transform the drawing code is under.
 */
class FramebufferContext final : public juce::LowLevelGraphicsSoftwareRenderer
{
public:
    FramebufferContext (const juce::Image& image, const juce::RectangleList<int>& clip, std::vector<std::uint8_t>& mask);

    void mark (juce::Rectangle<float> local, std::uint8_t value);
    void markPath (const juce::Path& local, std::uint8_t value);

    /** Big type as a pixel font: drawn at half size with no smoothing (each glyph pixel is on or off)
        and written as 2 x 2 blocks on the window's block grid. `lean` is applied before the
        context's transform (the shear of sheared type). */
    bool blitRaw (const juce::Image&, int scale);

    void drawPixelText (const juce::String&, juce::Rectangle<float> box, const juce::Font&, juce::Colour, juce::Justification,
                        const juce::AffineTransform& lean);

private:
    juce::Rectangle<int> absoluteClip() const;

    juce::Image m_image;
    std::vector<std::uint8_t>& m_mask;
    int m_width;
};


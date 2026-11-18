#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"
#include "scene/ToyPalette.h"

/**
 * De-Honk's window: flat off-beat pastels on cream with thin brown lines and small lowercase
 * type, after Japanese graphic design of the early 2000s. Nothing has a shadow or a gradient. The colours are in scene/ToyPalette.h (shared with the pond and the
 * band view); this file has the typefaces and the few drawing helpers every control uses.
 */
namespace Theme
{
    /** Typefaces loaded once and shared by every editor instance (SharedResourcePointer). */
    struct Fonts
    {
        Fonts()
            : regular (juce::Typeface::createSystemTypefaceFor (BinaryData::AzeretMonoRegular_ttf, BinaryData::AzeretMonoRegular_ttfSize)),
              medium  (juce::Typeface::createSystemTypefaceFor (BinaryData::AzeretMonoMedium_ttf,  BinaryData::AzeretMonoMedium_ttfSize)),
              bold    (juce::Typeface::createSystemTypefaceFor (BinaryData::AzeretMonoBold_ttf,    BinaryData::AzeretMonoBold_ttfSize))
        {
        }

        juce::Font make (const juce::Typeface::Ptr& face, float height, float kerning = 0.0f) const
        {
            if (face == nullptr)
                return juce::Font (juce::FontOptions (height));

            return juce::Font (juce::FontOptions (face).withHeight (height)).withExtraKerningFactor (kerning);
        }

        // A tiny tracking makes JUCE switch the font's ligatures off ("fi", "<-").
        juce::Font body (float height) const    { return make (regular, height, 0.002f); }
        juce::Font value (float height) const   { return make (medium, height, 0.02f); }
        juce::Font name (float height) const    { return make (medium, height, 0.16f); }  // control names: small, lowercase, widely spaced
        juce::Font title (float height) const   { return make (bold, height, 0.06f); }

        juce::Typeface::Ptr regular, medium, bold;
    };
}

/** The toy-box shapes. A control's bounds include room for its shadow along the bottom. */
namespace Toy
{
    using namespace dehonk;

    inline constexpr float pressBy = 0.0f;    // nothing moves when pressed: it changes colour instead

    /** The shape's own rectangle inside a control's bounds. */
    inline juce::Rectangle<float> body (juce::Rectangle<float> bounds, bool pressed = false) noexcept
    {
        auto r = bounds.reduced (toy::outline * 0.5f + 0.5f).withTrimmedBottom (toy::dropY);
        return pressed ? r.translated (0.0f, pressBy) : r;
    }
    inline juce::Rectangle<float> body (juce::Rectangle<int> bounds, bool pressed = false) noexcept { return body (bounds.toFloat(), pressed); }

    /** A rounded shape with its flat shadow and ink outline. `r` is the shape (see body()). */
    inline void shape (juce::Graphics& g, juce::Rectangle<float> r, float corner, juce::Colour fill, bool pressed = false, float outline = toy::outline)
    {
        corner = juce::jmin (corner, r.getHeight() * 0.5f, r.getWidth() * 0.5f);
        g.setColour (pressed ? fill.darker (0.08f) : fill);
        g.fillRoundedRectangle (r, corner);
        g.setColour (toy::ink);
        g.drawRoundedRectangle (r, corner, outline);
    }
    inline void pill (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill, bool pressed = false, float outline = toy::outline)
    {
        shape (g, r, r.getHeight() * 0.5f, fill, pressed, outline);
    }

    /** A shape with no shadow: wells, tags, the parts inside a control. */
    inline void flat (juce::Graphics& g, juce::Rectangle<float> r, float corner, juce::Colour fill, float outline = toy::outline)
    {
        corner = juce::jmin (corner, r.getHeight() * 0.5f, r.getWidth() * 0.5f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, corner);
        g.setColour (toy::ink);
        g.drawRoundedRectangle (r, corner, outline);
    }

    /** A pill well filled from x0 to x1 (both inside it) in `fill`, cut to the pill's shape. */
    inline void filledWell (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour well, juce::Colour fill, float x0, float x1, float outline = toy::outline)
    {
        const float corner = r.getHeight() * 0.5f;
        g.setColour (well);
        g.fillRoundedRectangle (r, corner);
        if (x1 > x0 + 0.5f)
        {
            juce::Path clip;
            clip.addRoundedRectangle (r, corner);
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (clip);
            g.setColour (fill);
            g.fillRect (juce::Rectangle<float> (x0, r.getY(), x1 - x0, r.getHeight()));
        }
        g.setColour (toy::ink);
        g.drawRoundedRectangle (r, corner, outline);
    }

    /** Ink on light colours, paper on strong ones. */
    inline juce::Colour textOn (juce::Colour fill) noexcept
    {
        return fill.getPerceivedBrightness() > 0.45f ? toy::ink : toy::paper;
    }
}

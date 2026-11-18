#pragma once

#include <JuceHeader.h>

/**
 * Art pulled straight out of the Freedoom IWAD (BSD-licensed): floor/ceiling flats for
 * the panel textures, the HUD font, the menu skull and the status-bar faces.
 * Loaded once per process via SharedResourcePointer.
 */
class WadArt
{
public:
    WadArt();

    bool isLoaded() const { return loaded; }

    /** 64x64 flat, or an empty image. */
    juce::Image flat (const juce::String& name) const;

    /** A patch (column-format graphic) as ARGB, or an empty image. */
    juce::Image patch (const juce::String& name) const;

    //==========================================================================
    /** The STCFN HUD font. Uppercase only (lowercase is mapped up). */
    struct PixelFont
    {
        std::array<juce::Image, 128> glyphs;
        int height = 7;
        int spaceWidth = 4;
        bool valid = false;

        int widthOf (const juce::String& text) const;

        /** Draws at an integer-ish pixel scale, tinted by colour (shading kept). */
        void draw (juce::Graphics&, const juce::String& text, float x, float y, float scale, juce::Colour colour, bool shadow = true) const;

        /** Picks the largest of 3, 2, 1.5, 1 that fits, then justifies inside area. */
        void drawFitted (juce::Graphics&, const juce::String& text, juce::Rectangle<float> area, juce::Justification,
                         juce::Colour colour, float maxScale = 2.0f, bool shadow = true) const;

        mutable std::map<uint32_t, std::array<juce::Image, 128>> tinted;
        const juce::Image& tintedGlyph (int c, juce::Colour colour) const;
    };

    const PixelFont& font() const { return pixelFont; }

private:
    struct Lump { int offset = 0, size = 0; };

    /** Reads one lump from the WAD file (the WAD itself is never held in memory). */
    juce::MemoryBlock readLump (const juce::String& name) const;
    juce::Image decodePatch (const uint8_t* data, int size) const;

    juce::File wadFile;
    std::map<juce::String, Lump> lumps;
    std::array<juce::PixelARGB, 256> palette {};
    PixelFont pixelFont;
    bool loaded = false;
};

//==============================================================================
/** Drawing helpers for the old-school look. */
namespace DoomStyle
{
    /** Tiles a flat with chunky nearest-neighbour pixels and darkens it. */
    void fillTextured (juce::Graphics&, juce::Rectangle<float> area, const juce::Image& tile, float pixelScale,
                       float brightness, juce::Point<float> anchor = {});

    /** Classic 2-tone bevel: light top/left, dark bottom/right. */
    void bevel (juce::Graphics&, juce::Rectangle<int> r, int thickness, juce::Colour light, juce::Colour dark);

    /** Little square bolts in each corner. */
    void rivets (juce::Graphics&, juce::Rectangle<int> r, int inset, int size);

    /** Snaps alpha to 0/255 so anything drawn into a small image upscales as crisp pixels. */
    void crispen (juce::Image&);
}

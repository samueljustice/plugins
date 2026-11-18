#include "WadArt.h"
#include "DoomEngine.h"

namespace
{
    int32_t readLE32 (const uint8_t* p) { return (int32_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24)); }
    int16_t readLE16 (const uint8_t* p) { return (int16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8)); }
}

WadArt::WadArt()
{
    const auto file = DoomEngine::getWadFile();
    if (! file.existsAsFile() || ! file.loadFileAsData (wad) || wad.getSize() < 12)
        return;

    const auto* d = static_cast<const uint8_t*> (wad.getData());
    const int total = (int) wad.getSize();
    const int numLumps = readLE32 (d + 4), dirOffset = readLE32 (d + 8);
    if (dirOffset < 0 || dirOffset + numLumps * 16 > total)
        return;

    for (int i = 0; i < numLumps; ++i)
    {
        const auto* e = d + dirOffset + i * 16;
        char name[9] = {};
        std::memcpy (name, e + 8, 8);
        Lump l { readLE32 (e), readLE32 (e + 4) };
        if (l.offset >= 0 && l.size >= 0 && l.offset + l.size <= total)
            lumps.emplace (juce::String (name).toUpperCase(), l); // first wins, like vanilla for our purposes
    }

    int palSize = 0;
    if (const auto* pal = lumpData ("PLAYPAL", palSize); pal != nullptr && palSize >= 768)
        for (int i = 0; i < 256; ++i)
            palette[(size_t) i] = juce::PixelARGB (255, pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]);
    else
        return;

    // HUD font: STCFN033..STCFN095 (! through _)
    for (int c = 33; c <= 95; ++c)
    {
        auto img = patch ("STCFN" + juce::String (c).paddedLeft ('0', 3));
        if (img.isValid())
        {
            pixelFont.glyphs[(size_t) c] = img;
            pixelFont.height = juce::jmax (pixelFont.height, img.getHeight());
        }
    }
    // No backtick in the HUD font; mirror the apostrophe.
    if (auto& apos = pixelFont.glyphs[(size_t) '\'']; apos.isValid())
    {
        juce::Image tick (juce::Image::ARGB, apos.getWidth(), apos.getHeight(), true);
        for (int y = 0; y < apos.getHeight(); ++y)
            for (int x = 0; x < apos.getWidth(); ++x)
                tick.setPixelAt (apos.getWidth() - 1 - x, y, apos.getPixelAt (x, y));
        pixelFont.glyphs[(size_t) '`'] = tick;
    }

    pixelFont.valid = pixelFont.glyphs[(size_t) 'A'].isValid();

    loaded = true;
}

const uint8_t* WadArt::lumpData (const juce::String& name, int& size) const
{
    auto it = lumps.find (name.toUpperCase());
    if (it == lumps.end())
        return nullptr;
    size = it->second.size;
    return static_cast<const uint8_t*> (wad.getData()) + it->second.offset;
}

juce::Image WadArt::flat (const juce::String& name) const
{
    int size = 0;
    const auto* d = lumpData (name, size);
    if (d == nullptr || size < 4096)
        return {};

    juce::Image img (juce::Image::ARGB, 64, 64, false);
    juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            bd.setPixelColour (x, y, juce::Colour (palette[d[y * 64 + x]].getNativeARGB()));
    return img;
}

juce::Image WadArt::patch (const juce::String& name) const
{
    int size = 0;
    const auto* d = lumpData (name, size);
    return d != nullptr ? decodePatch (d, size) : juce::Image();
}

juce::Image WadArt::decodePatch (const uint8_t* d, int size) const
{
    if (size < 8)
        return {};

    const int w = readLE16 (d), h = readLE16 (d + 2);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || 8 + w * 4 > size)
        return {};

    juce::Image img (juce::Image::ARGB, w, h, true);
    juce::Image::BitmapData bd (img, juce::Image::BitmapData::readWrite);

    for (int x = 0; x < w; ++x)
    {
        int p = readLE32 (d + 8 + x * 4);
        while (p >= 0 && p < size)
        {
            const int top = d[p];
            if (top == 255)
                break;
            if (p + 3 >= size)
                break;
            const int len = d[p + 1];
            for (int i = 0; i < len && p + 3 + i < size; ++i)
            {
                const int y = top + i;
                if (y >= 0 && y < h)
                    bd.setPixelColour (x, y, juce::Colour (palette[d[p + 3 + i]].getNativeARGB()));
            }
            p += len + 4;
        }
    }

    return img;
}

//==============================================================================
static int glyphIndex (juce::juce_wchar c)
{
    if (c >= 'a' && c <= 'z')
        c = c - 'a' + 'A';
    return (c >= 33 && c <= 96) ? (int) c : -1;
}

int WadArt::PixelFont::widthOf (const juce::String& text) const
{
    int w = 0;
    for (auto c : text)
    {
        const int i = glyphIndex (c);
        w += (i >= 0 && glyphs[(size_t) i].isValid()) ? glyphs[(size_t) i].getWidth() : spaceWidth;
    }
    return w;
}

const juce::Image& WadArt::PixelFont::tintedGlyph (int c, juce::Colour colour) const
{
    auto& set = tinted[colour.getARGB()];
    auto& img = set[(size_t) c];
    if (! img.isValid() && glyphs[(size_t) c].isValid())
    {
        // Keep the glyph's shading (luminance) but recolour it.
        const auto& src = glyphs[(size_t) c];
        img = juce::Image (juce::Image::ARGB, src.getWidth(), src.getHeight(), true);
        float maxLum = 0.01f;
        for (int y = 0; y < src.getHeight(); ++y)
            for (int x = 0; x < src.getWidth(); ++x)
                maxLum = juce::jmax (maxLum, src.getPixelAt (x, y).getPerceivedBrightness());
        for (int y = 0; y < src.getHeight(); ++y)
            for (int x = 0; x < src.getWidth(); ++x)
            {
                const auto p = src.getPixelAt (x, y);
                if (p.getAlpha() == 0)
                    continue;
                const float lum = juce::jlimit (0.0f, 1.0f, 0.35f + 0.65f * p.getPerceivedBrightness() / maxLum);
                img.setPixelAt (x, y, colour.withMultipliedBrightness (lum).withAlpha (1.0f));
            }
    }
    return img;
}

void WadArt::PixelFont::draw (juce::Graphics& g, const juce::String& text, float x, float y, float scale, juce::Colour colour, bool shadow) const
{
    juce::Graphics::ScopedSaveState ss (g);
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);

    for (int pass = shadow ? 0 : 1; pass < 2; ++pass)
    {
        float cx = x;
        const float off = pass == 0 ? juce::jmax (1.0f, scale) : 0.0f;
        for (auto c : text)
        {
            const int i = glyphIndex (c);
            if (i >= 0 && glyphs[(size_t) i].isValid())
            {
                const auto& src = glyphs[(size_t) i];
                const juce::Rectangle<float> dest (std::round (cx + off), std::round (y + off),
                                                   src.getWidth() * scale, src.getHeight() * scale);
                // Pre-tinted glyph images only: alpha-mask fills are very slow on the GL renderer.
                g.setOpacity (pass == 0 ? 0.85f * colour.getFloatAlpha() : colour.getFloatAlpha());
                g.drawImage (tintedGlyph (i, pass == 0 ? juce::Colours::black : colour.withAlpha (1.0f)), dest);
                cx += src.getWidth() * scale;
            }
            else
            {
                cx += spaceWidth * scale;
            }
        }
    }
}

void WadArt::PixelFont::drawFitted (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                                    juce::Justification just, juce::Colour colour, float maxScale, bool shadow) const
{
    if (! valid)
    {
        g.setColour (colour);
        g.setFont (juce::Font (juce::FontOptions (area.getHeight() * 0.7f, juce::Font::bold)));
        g.drawText (text.toUpperCase(), area, just);
        return;
    }

    const int w = widthOf (text);
    float scale = 1.0f;
    for (float s : { 4.0f, 3.0f, 2.0f, 1.5f, 1.0f })
        if (s <= maxScale && w * s <= area.getWidth() && height * s <= area.getHeight() + 1.0f)
        {
            scale = s;
            break;
        }

    const juce::Rectangle<float> box (0.0f, 0.0f, w * scale, height * scale);
    const auto placed = just.appliedToRectangle (box, area);
    draw (g, text, placed.getX(), placed.getY(), scale, colour, shadow);
}

//==============================================================================
namespace DoomStyle
{
    void fillTextured (juce::Graphics& g, juce::Rectangle<float> area, const juce::Image& tile, float pixelScale,
                       float brightness, juce::Point<float> anchor)
    {
        if (! tile.isValid())
        {
            g.setColour (juce::Colour (0xff2a2622).withMultipliedBrightness (brightness * 1.5f));
            g.fillRect (area);
            return;
        }

        juce::Graphics::ScopedSaveState ss (g);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.setFillType (juce::FillType (tile, juce::AffineTransform::scale (pixelScale).translated (anchor)));
        g.fillRect (area);
        if (brightness < 1.0f)
        {
            g.setColour (juce::Colours::black.withAlpha (1.0f - brightness));
            g.fillRect (area);
        }
    }

    void bevel (juce::Graphics& g, juce::Rectangle<int> r, int t, juce::Colour light, juce::Colour dark)
    {
        for (int i = 0; i < t; ++i)
        {
            const auto rr = r.reduced (i);
            g.setColour (light);
            g.fillRect (rr.getX(), rr.getY(), rr.getWidth() - 1, 1);
            g.fillRect (rr.getX(), rr.getY(), 1, rr.getHeight() - 1);
            g.setColour (dark);
            g.fillRect (rr.getX() + 1, rr.getBottom() - 1, rr.getWidth() - 1, 1);
            g.fillRect (rr.getRight() - 1, rr.getY() + 1, 1, rr.getHeight() - 1);
        }
    }

    void rivets (juce::Graphics& g, juce::Rectangle<int> r, int inset, int size)
    {
        const juce::Point<int> pts[] = { { r.getX() + inset, r.getY() + inset },
                                         { r.getRight() - inset - size, r.getY() + inset },
                                         { r.getX() + inset, r.getBottom() - inset - size },
                                         { r.getRight() - inset - size, r.getBottom() - inset - size } };
        for (auto p : pts)
        {
            g.setColour (juce::Colour (0xff111111));
            g.fillRect (p.x + 1, p.y + 1, size, size);
            g.setColour (juce::Colour (0xff8a8478));
            g.fillRect (p.x, p.y, size, size);
            g.setColour (juce::Colour (0xffc9c2b0));
            g.fillRect (p.x, p.y, size - 1, 1);
            g.fillRect (p.x, p.y, 1, size - 1);
            g.setColour (juce::Colour (0xff4a463e));
            g.fillRect (p.x + size - 1, p.y + 1, 1, size - 1);
            g.fillRect (p.x + 1, p.y + size - 1, size - 1, 1);
        }
    }

    void crispen (juce::Image& img)
    {
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::readWrite);
        for (int y = 0; y < bd.height; ++y)
            for (int x = 0; x < bd.width; ++x)
            {
                auto c = bd.getPixelColour (x, y);
                bd.setPixelColour (x, y, c.getAlpha() >= 110 ? c.withAlpha ((juce::uint8) 255) : juce::Colours::transparentBlack);
            }
    }
}

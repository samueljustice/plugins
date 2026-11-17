#include "Framebuffer.h"

#include "FlightView.h"

#include <chrono>
#include <cmath>

namespace
{
    // The dither as a table: for each of the 16 Bayer cells and each 8-bit level, the 5-bit level it
    // becomes, widened back to 8 bits. Built once from the flight view's own dither, so both match.
    const std::array<std::uint8_t, 16 * 256>& ditherTable()
    {
        static const auto table = []
        {
            std::array<std::uint8_t, 16 * 256> t {};
            for (int cell = 0; cell < 16; ++cell)
                for (int v = 0; v < 256; ++v)
                    t[(std::size_t) (cell * 256 + v)] = flight::expand5 (flight::dither5 (v, cell & 3, cell >> 2));
            return t;
        }();
        return table;
    }

    inline const std::uint8_t* ditherRow (int x, int y) noexcept
    {
        return ditherTable().data() + (std::size_t) ((((y & 3) << 2) | (x & 3)) * 256);
    }

    constexpr float c_scanline = 0.94f;    // every third row, 6% darker
    constexpr float c_vignette = 0.22f;    // how far the corners sink
}

void Framebuffer::setSize (int width, int height)
{
    if (m_source.getWidth() == width && m_source.getHeight() == height)
        return;

    m_source = juce::Image (juce::Image::ARGB, width, height, true, juce::SoftwareImageType());
    m_output = juce::Image (juce::Image::ARGB, width, height, true, juce::SoftwareImageType());
    m_mask.assign ((std::size_t) (width * height), Hud::kChunky);
    m_shade.resize ((std::size_t) (width * height));

    const float cx = (float) width * 0.5f, cy = (float) height * 0.5f;
    const float corner = std::sqrt (cx * cx + cy * cy);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const float d = std::sqrt (((float) x - cx) * ((float) x - cx) + ((float) y - cy) * ((float) y - cy)) / corner;
            const float t = juce::jlimit (0.0f, 1.0f, (d - 0.62f) / 0.38f);
            float shade = 1.0f - c_vignette * t * t;
            if (y % 3 == 2)
                shade *= c_scanline;
            m_shade[(std::size_t) (y * width + x)] = (std::uint16_t) std::lround (shade * 256.0f);
        }
}

void Framebuffer::render (juce::Component& host, juce::Rectangle<int> region, const std::function<void (juce::Graphics&)>& paintHost)
{
    setSize (host.getWidth(), host.getHeight());
    const auto start = std::chrono::steady_clock::now();

    // Whole blocks only: widen to even edges.
    const int x0 = region.getX() & ~1, y0 = region.getY() & ~1;
    const int x1 = std::min (m_source.getWidth(), (region.getRight() + 1) & ~1);
    const int y1 = std::min (m_source.getHeight(), (region.getBottom() + 1) & ~1);
    const auto area = juce::Rectangle<int> (x0, y0, x1 - x0, y1 - y0).getIntersection (host.getLocalBounds());
    if (area.isEmpty())
        return;

    // Drawn a little beyond the area and finished only inside it: where a clip edge cuts through
    // a smoothed path, JUCE's renderer can shade the edge column slightly differently, and that
    // column must never be one that is shown (or the same pixel would differ between repaints).
    const auto drawn = area.expanded (4).getIntersection (host.getLocalBounds());
    for (int y = drawn.getY(); y < drawn.getBottom(); ++y)
        std::fill_n (m_mask.begin() + (std::ptrdiff_t) (y * m_source.getWidth() + drawn.getX()), drawn.getWidth(), Hud::kChunky);

    {
        Hud::FramebufferPass pass;
        FramebufferContext context (m_source, juce::RectangleList<int> (drawn), m_mask);
        juce::Graphics g (context);
        paintHost (g);

        // The children, as JUCE would paint them, in the same order.
        for (auto* child : host.getChildren())
        {
            // A tooltip draws itself over the framebuffer and comes and goes on its own: kept out
            // of it, so its edges are never folded into the blocks around it.
            if (! child->isVisible() || ! child->getBounds().intersects (drawn) || dynamic_cast<juce::TooltipWindow*> (child) != nullptr)
                continue;
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (child->getBounds());
            g.setOrigin (child->getPosition());
            if (child->isTransformed())
                g.addTransform (child->getTransform());
            child->paintEntireComponent (g, false);
        }
    }

    finish (area);
    m_lastMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
}

void Framebuffer::finish (juce::Rectangle<int> area)
{
    const int width = m_source.getWidth();
    const juce::Image::BitmapData src (m_source, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData out (m_output, juce::Image::BitmapData::writeOnly);

    // Opaque premultiplied pixels, read and written whole; channels by JUCE's own shifts.
    using P = juce::PixelARGB;
    const auto red   = [] (std::uint32_t c) { return (int) ((c >> P::indexR * 8) & 0xff); };
    const auto green = [] (std::uint32_t c) { return (int) ((c >> P::indexG * 8) & 0xff); };
    const auto blue  = [] (std::uint32_t c) { return (int) ((c >> P::indexB * 8) & 0xff); };
    const auto pack  = [] (int r, int g, int b)
    {
        return (std::uint32_t) (0xffu << (P::indexA * 8)) | ((std::uint32_t) r << (P::indexR * 8))
             | ((std::uint32_t) g << (P::indexG * 8)) | ((std::uint32_t) b << (P::indexB * 8));
    };
    const auto distance = [&] (std::uint32_t a, std::uint32_t b)
    {
        return std::abs (red (a) - red (b)) + std::abs (green (a) - green (b)) + std::abs (blue (a) - blue (b));
    };
    const auto brightness = [&] (std::uint32_t c) { return red (c) * 2 + green (c) * 3 + blue (c); };

    for (int y = area.getY(); y < area.getBottom(); y += 2)
    {
        const std::uint32_t* srcRow[2] = { reinterpret_cast<const std::uint32_t*> (src.getLinePointer (y)),
                                           reinterpret_cast<const std::uint32_t*> (src.getLinePointer (y + 1)) };
        std::uint32_t* outRow[2] = { reinterpret_cast<std::uint32_t*> (out.getLinePointer (y)),
                                     reinterpret_cast<std::uint32_t*> (out.getLinePointer (y + 1)) };
        const std::uint8_t* maskRow[2] = { m_mask.data() + y * width, m_mask.data() + (y + 1) * width };
        const std::uint16_t* shadeRow[2] = { m_shade.data() + y * width, m_shade.data() + (y + 1) * width };
        const std::uint8_t* ditherRows[2][2] = { { ditherRow (0, y), ditherRow (1, y) }, { ditherRow (0, y + 1), ditherRow (1, y + 1) } };

        for (int x = area.getX(); x < area.getRight(); x += 2)
        {
            const std::uint32_t p[4] = { srcRow[0][x], srcRow[0][x + 1], srcRow[1][x], srcRow[1][x + 1] };
            const std::uint8_t m[4] = { maskRow[0][x], maskRow[0][x + 1], maskRow[1][x], maskRow[1][x + 1] };

            // The block's colour, always one of its own pixels (so edges step hard, never blend):
            // its four pixels split into the two most different colours, and the side with more of
            // them wins; a tie (a one-pixel line through the block) goes to the brighter, so thin
            // lines on the dark panels survive as whole blocks.
            std::uint32_t block = p[0];
            if ((m[0] == Hud::kChunky || m[1] == Hud::kChunky || m[2] == Hud::kChunky || m[3] == Hud::kChunky)
                && ! (p[0] == p[1] && p[0] == p[2] && p[0] == p[3]))
            {
                int sa = 0, sb = 0, far = 0;
                for (int a = 0; a < 4; ++a)
                    for (int b = a + 1; b < 4; ++b)
                        if (const int d = distance (p[a], p[b]); d > far)
                        {
                            far = d;
                            sa = a;
                            sb = b;
                        }
                int votesA = 0;
                for (int i = 0; i < 4; ++i)
                    votesA += distance (p[i], p[sa]) <= distance (p[i], p[sb]) ? 1 : 0;
                block = p[votesA > 2 ? sa : votesA < 2 ? sb : (brightness (p[sa]) >= brightness (p[sb]) ? sa : sb)];
            }

            for (int i = 0; i < 4; ++i)
            {
                const int row = i >> 1, px = x + (i & 1);
                const std::uint32_t c = m[i] == Hud::kChunky ? block : p[i];
                const int shade = shadeRow[row][px];
                const int r = (red (c) * shade) >> 8, g = (green (c) * shade) >> 8, b = (blue (c) * shade) >> 8;
                if (m[i] == Hud::kRaw)
                    outRow[row][px] = pack (std::min (r, 255), std::min (g, 255), std::min (b, 255));
                else
                {
                    const auto* d = ditherRows[row][px & 1] + ((px & 2) != 0 ? 2 * 256 : 0);
                    outRow[row][px] = pack (d[std::min (r, 255)], d[std::min (g, 255)], d[std::min (b, 255)]);
                }
            }
        }
    }
}

juce::Rectangle<int> Framebuffer::blockAligned (juce::Rectangle<int> r) noexcept
{
    const int x0 = r.getX() & ~1, y0 = r.getY() & ~1;
    return { x0, y0, ((r.getRight() + 1) & ~1) - x0, ((r.getBottom() + 1) & ~1) - y0 };
}

void Framebuffer::draw (juce::Graphics& g, juce::Rectangle<int> region) const
{
    if (m_output.isNull())
        return;
    const auto r = region.getIntersection (m_output.getBounds());
    juce::Graphics::ScopedSaveState save (g);
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    // Only the repainted part goes to the screen (a sub-image shares the pixels, nothing is copied here).
    g.drawImageAt (m_output.getClippedImage (r), r.getX(), r.getY());
}

void Framebuffer::dither (juce::Image& image)
{
    juce::Image::BitmapData data (image, juce::Image::BitmapData::readWrite);
    for (int y = 0; y < data.height; ++y)
        for (int x = 0; x < data.width; ++x)
        {
            auto c = data.getPixelColour (x, y);
            const auto* d = ditherRow (x, y);
            data.setPixelColour (x, y, juce::Colour (d[c.getRed()], d[c.getGreen()], d[c.getBlue()], c.getAlpha()));
        }
}

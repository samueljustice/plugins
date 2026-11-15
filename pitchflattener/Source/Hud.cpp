#include "Hud.h"

#include <unordered_map>

namespace
{
    std::atomic<int> g_overflows { 0 };
    int g_framebufferPasses = 0;   // message thread only
    const juce::Identifier c_hostProperty { "pitchFlattenerFramebufferHost" };

    FramebufferContext* framebufferOf (juce::Graphics& g)
    {
        return dynamic_cast<FramebufferContext*> (&g.getInternalContext());
    }

    // The box a line of text actually covers inside its area, as Graphics::drawText places it.
    juce::Rectangle<float> textBox (juce::Rectangle<float> area, float width, float height, juce::Justification just)
    {
        const float x = just.testFlags (juce::Justification::right) ? area.getRight() - width
                      : just.testFlags (juce::Justification::horizontallyCentred) ? area.getCentreX() - width * 0.5f
                      : area.getX();
        const float y = just.testFlags (juce::Justification::bottom) ? area.getBottom() - height
                      : just.testFlags (juce::Justification::top) ? area.getY()
                      : area.getCentreY() - height * 0.5f;
        return { x, y, width, height };
    }

    // Graphics::drawText drops a word that overruns its area by any amount: half a pixel of slack.
    constexpr float c_slack = 0.5f;
}

namespace Hud
{
juce::Path chamfer (juce::Rectangle<float> r, float cut)
{
    cut = std::max (0.0f, std::min (cut, 0.5f * std::min (r.getWidth(), r.getHeight())));
    juce::Path p;
    p.startNewSubPath (r.getX() + cut, r.getY());
    p.lineTo (r.getRight(), r.getY());
    p.lineTo (r.getRight(), r.getBottom() - cut);
    p.lineTo (r.getRight() - cut, r.getBottom());
    p.lineTo (r.getX(), r.getBottom());
    p.lineTo (r.getX(), r.getY() + cut);
    p.closeSubPath();
    return p;
}

void frame (juce::Graphics& g, juce::Rectangle<float> r, float cut, juce::Colour fill, juce::Colour edge, float thickness)
{
    if (! fill.isTransparent())
    {
        g.setColour (fill);
        g.fillPath (chamfer (r, cut));
    }
    if (! edge.isTransparent() && thickness > 0.0f)
    {
        // The stroke is centred on the path: inset by half of it so the whole edge stays inside.
        const auto inner = r.reduced (thickness * 0.5f);
        g.setColour (edge);
        g.strokePath (chamfer (inner, std::max (0.0f, cut - thickness * 0.2f)), juce::PathStrokeType (thickness, juce::PathStrokeType::mitered));
    }
}

float speedStripes (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour, int count)
{
    // Parallelograms leaning forward like the sheared type, each a little fainter than the last,
    // so they read as motion trailing the title they sit before.
    // On the framebuffer's 2 px block grid: bars and gaps whole blocks wide, so they stay crisp bars.
    const float h = area.getHeight();
    const float barWidth = 2.0f * std::max (2.0f, std::round (h * 0.14f)), gap = barWidth;
    const float lean = 2.0f * std::round (h * kShear * 0.8f);
    float x = 2.0f * std::round (area.getX() * 0.5f);
    for (int i = 0; i < count; ++i)
    {
        juce::Path p;
        p.startNewSubPath (x + lean, area.getY());
        p.lineTo (x + lean + barWidth, area.getY());
        p.lineTo (x + barWidth, area.getBottom());
        p.lineTo (x, area.getBottom());
        p.closeSubPath();
        g.setColour (colour.withMultipliedAlpha (1.0f - 0.28f * (float) i));
        g.fillPath (p);
        x += barWidth + gap;
    }
    return x - gap + lean - area.getX();
}

juce::Rectangle<float> heading (juce::Graphics& g, juce::Rectangle<float> row, const juce::String& title, const juce::Font& font, juce::Colour colour)
{
    if (! g.clipRegionIntersects (row.getSmallestIntegerContainer()))
        return row;
    // In a narrow panel: two stripes instead of three, then the title a little smaller, so the
    // whole word always fits.
    const float stripeHeight = std::round (font.getHeight() * 0.8f);
    const auto stripeRow = row.withSizeKeepingCentre (row.getWidth(), stripeHeight).withX (row.getX());
    const auto stripesWidth = [&stripeRow] (int count)
    {
        const float h = stripeRow.getHeight(), bar = 2.0f * std::max (2.0f, std::round (h * 0.14f));
        return (float) count * bar * 2.0f - bar + 2.0f * std::round (h * kShear * 0.8f);
    };
    const int count = shearedWidth (font, title) + 2.0f <= row.getWidth() - stripesWidth (3) - 7.0f ? 3 : 2;
    const float used = speedStripes (g, stripeRow, colour, count);
    auto rest = row.withTrimmedLeft (used + 7.0f);
    const float h = fitHeight ([&font] (float fh) { return font.withHeight (fh); }, title, rest.getWidth() - 2.0f, font.getHeight(), 10.0f);
    const auto fitted = font.withHeight (h);
    const float w = shearedWidth (fitted, title) + 2.0f;
    const auto box = rest.withWidth (std::min (w, rest.getWidth()));
    drawSheared (g, title, box, fitted, colour);
    return row.withRight (box.getRight());
}

float textWidth (const juce::Font& font, const juce::String& text)
{
    if (text.isEmpty())
        return 0.0f;

    // Measuring shapes the text, which is slow next to drawing it, and the same few strings are
    // measured on every repaint (and many times over while a size is fitted): remembered here.
    // Painting happens on the message thread only.
    static std::unordered_map<juce::String, float> cache;
    const auto* face = font.getTypefacePtr().get();
    const auto key = text + "|" + juce::String (font.getHeight(), 3) + "|" + juce::String (font.getExtraKerningFactor(), 4)
                   + "|" + juce::String::toHexString ((juce::pointer_sized_int) face);
    if (const auto found = cache.find (key); found != cache.end())
        return found->second;
    if (cache.size() > 8192)
        cache.clear();
    const float w = juce::GlyphArrangement::getStringWidth (font, text + " ") - juce::GlyphArrangement::getStringWidth (font, " ");
    cache.emplace (key, w);
    return w;
}

float shearedWidth (const juce::Font& font, const juce::String& text)
{
    return textWidth (font, text) + kShear * font.getHeight();
}

void drawText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, const juce::Font& font, juce::Colour colour,
               juce::Justification just)
{
    if (text.isEmpty() || ! g.clipRegionIntersects (area.getSmallestIntegerContainer()))
        return;
    if (textWidth (font, text) > area.getWidth() + c_slack || font.getHeight() > area.getHeight() + c_slack)
        g_overflows.fetch_add (1, std::memory_order_relaxed);
    if (font.getHeight() < kFineTextHeight)
        markFine (g, textBox (area, textWidth (font, text), font.getHeight(), just).expanded (1.0f));
    else if (auto* fb = framebufferOf (g))
    {
        fb->drawPixelText (text, area.expanded (c_slack, 0.0f), font, colour, just, {});
        return;
    }
    g.setFont (font);
    g.setColour (colour);
    g.drawText (text, area.expanded (c_slack, 0.0f), just, false);
}

void drawSheared (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, const juce::Font& font, juce::Colour colour,
                  juce::Justification just)
{
    if (text.isEmpty() || ! g.clipRegionIntersects (area.getSmallestIntegerContainer().expanded (4, 0)))
        return;

    // The glyphs lean by kShear per pixel about the box's middle: half the lean each side is kept
    // free, so the top of the first letter and the foot of the last stay inside the box.
    const float lean = kShear * font.getHeight();
    const auto box = area.reduced (lean * 0.5f, 0.0f);
    if (textWidth (font, text) > box.getWidth() + c_slack || font.getHeight() > area.getHeight() + c_slack)
        g_overflows.fetch_add (1, std::memory_order_relaxed);

    const float cy = box.getCentreY();
    const auto shear = juce::AffineTransform::shear (-kShear, 0.0f).translated (kShear * cy, 0.0f);
    if (font.getHeight() < kFineTextHeight)
        markFine (g, textBox (box, textWidth (font, text), font.getHeight(), just).expanded (lean * 0.5f + 1.0f, 1.0f));
    else if (auto* fb = framebufferOf (g))
    {
        fb->drawPixelText (text, box.expanded (c_slack, 0.0f), font, colour, just, shear);
        return;
    }

    juce::Graphics::ScopedSaveState save (g);
    g.addTransform (shear);
    g.setFont (font);
    g.setColour (colour);
    g.drawText (text, box.expanded (c_slack, 0.0f), just, false);
}

float fittedHeight (const Theme::Fonts& fonts, bool displayFace, const juce::String& text, float width, float maxHeight, float minHeight, bool sheared)
{
    return fitHeight ([&] (float h) { return displayFace ? fonts.display (h) : fonts.body (h); }, text, width, maxHeight, minHeight, sheared);
}

int overflowCount() noexcept { return g_overflows.load (std::memory_order_relaxed); }
void resetOverflowCount() noexcept { g_overflows.store (0, std::memory_order_relaxed); }

void chevron (juce::Graphics& g, juce::Point<float> centre, float size, float angle, float thickness, juce::Colour colour)
{
    juce::Path p;
    p.startNewSubPath (-size * 0.5f, -size);
    p.lineTo (size * 0.5f, 0.0f);
    p.lineTo (-size * 0.5f, size);
    p.applyTransform (juce::AffineTransform::rotation (angle).translated (centre));
    g.setColour (colour);
    g.strokePath (p, juce::PathStrokeType (thickness, juce::PathStrokeType::mitered, juce::PathStrokeType::square));
}

float pulse (double seconds, double hz) noexcept
{
    const double phase = seconds * hz - std::floor (seconds * hz);
    return (float) (phase < 0.5 ? phase * 2.0 : 2.0 - phase * 2.0);
}

void markFine (juce::Graphics& g, juce::Rectangle<float> r)
{
    if (auto* fb = framebufferOf (g))
        fb->mark (r, kFine);
}

void markRaw (juce::Graphics& g, juce::Rectangle<float> r)
{
    if (auto* fb = framebufferOf (g))
        fb->mark (r, kRaw);
}

void markCovered (juce::Graphics& g, const juce::Path& p)
{
    if (auto* fb = framebufferOf (g))
        fb->markPath (p, kChunky);
}

void markCovered (juce::Graphics& g, juce::Rectangle<float> r)
{
    if (auto* fb = framebufferOf (g))
        fb->mark (r, kChunky);
}

bool blitRaw (juce::Graphics& g, const juce::Image& image, int scale)
{
    auto* fb = framebufferOf (g);
    return fb != nullptr && fb->blitRaw (image, scale);
}

bool skipOnScreen (const juce::Component& c)
{
    if (g_framebufferPasses > 0)
        return false;
    for (auto* p = c.getParentComponent(); p != nullptr; p = p->getParentComponent())
        if (p->getProperties().contains (c_hostProperty))
            return true;
    return false;
}

void setFramebufferHost (juce::Component& c)
{
    c.getProperties().set (c_hostProperty, true);
}

FramebufferPass::FramebufferPass()  { ++g_framebufferPasses; }
FramebufferPass::~FramebufferPass() { --g_framebufferPasses; }
}

//==============================================================================
FramebufferContext::FramebufferContext (const juce::Image& image, const juce::RectangleList<int>& clip, std::vector<std::uint8_t>& mask)
    : juce::LowLevelGraphicsSoftwareRenderer (image, {}, clip)
    , m_image (image)
    , m_mask (mask)
    , m_width (image.getWidth())
{
    jassert ((int) mask.size() == image.getWidth() * image.getHeight());
}

juce::Rectangle<int> FramebufferContext::absoluteClip() const
{
    return getClipBounds().toFloat().transformedBy (stack->transform.getTransform()).getSmallestIntegerContainer()
               .getIntersection ({ 0, 0, m_width, (int) m_mask.size() / std::max (1, m_width) });
}

void FramebufferContext::mark (juce::Rectangle<float> local, std::uint8_t value)
{
    const auto r = local.transformedBy (stack->transform.getTransform()).getSmallestIntegerContainer().getIntersection (absoluteClip());
    for (int y = r.getY(); y < r.getBottom(); ++y)
        std::fill_n (m_mask.begin() + (std::ptrdiff_t) (y * m_width + r.getX()), r.getWidth(), value);
}

void FramebufferContext::markPath (const juce::Path& local, std::uint8_t value)
{
    const auto clip = absoluteClip();
    if (clip.isEmpty())
        return;

    // The covered pixels exactly (an edge pixel counts when at least half covered), so text just
    // beside a slanted band stays fine.
    struct Writer
    {
        std::vector<std::uint8_t>& mask;
        int width;
        std::uint8_t value;
        int row = 0;
        void setEdgeTableYPos (int y) noexcept { row = y * width; }
        void handleEdgeTablePixel (int x, int alpha) noexcept       { if (alpha >= 128) mask[(std::size_t) (row + x)] = value; }
        void handleEdgeTablePixelFull (int x) noexcept              { mask[(std::size_t) (row + x)] = value; }
        void handleEdgeTableLine (int x, int w, int alpha) noexcept { if (alpha >= 128) handleEdgeTableLineFull (x, w); }
        void handleEdgeTableLineFull (int x, int w) noexcept        { std::fill_n (mask.begin() + (std::ptrdiff_t) (row + x), w, value); }
    };
    juce::EdgeTable table (clip, local, stack->transform.getTransform());
    Writer writer { m_mask, m_width, value };
    table.iterate (writer);
}

void FramebufferContext::drawPixelText (const juce::String& text, juce::Rectangle<float> box, const juce::Font& font, juce::Colour colour,
                                        juce::Justification just, const juce::AffineTransform& lean)
{
    const auto toAbsolute = lean.followedBy (stack->transform.getTransform());
    const auto clip = absoluteClip();
    auto bounds = box.transformedBy (toAbsolute).getSmallestIntegerContainer().expanded (2).getIntersection (clip.expanded (2));
    if (bounds.isEmpty() || colour.isTransparent())
        return;

    // Even origin: the blocks land on the same grid as every other chunky pixel.
    const int ox = bounds.getX() & ~1, oy = bounds.getY() & ~1;
    const int w = (bounds.getRight() - ox + 1) / 2 + 1, h = (bounds.getBottom() - oy + 1) / 2 + 1;
    juce::Image glyphs (juce::Image::SingleChannel, w, h, true, juce::SoftwareImageType());
    {
        juce::Graphics tg (glyphs);
        tg.addTransform (toAbsolute.translated ((float) -ox, (float) -oy).scaled (0.5f));
        tg.setFont (font);
        tg.setColour (juce::Colours::white);
        tg.drawText (text, box, just, false);
    }

    const juce::Image::BitmapData coverage (glyphs, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData dest (m_image, juce::Image::BitmapData::readWrite);
    const int alpha = colour.getAlpha();
    const int cr = colour.getRed(), cg = colour.getGreen(), cb = colour.getBlue();
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
        {
            if (*coverage.getPixelPointer (i, j) < 128)   // on or off: no smoothing
                continue;
            for (int k = 0; k < 4; ++k)
            {
                const int x = ox + 2 * i + (k & 1), y = oy + 2 * j + (k >> 1);
                if (! clip.contains (x, y))
                    continue;
                auto* p = reinterpret_cast<juce::PixelARGB*> (dest.getPixelPointer (x, y));
                const int inv = 255 - alpha;
                p->setARGB (255, (juce::uint8) ((cr * alpha + p->getRed() * inv) / 255),
                                 (juce::uint8) ((cg * alpha + p->getGreen() * inv) / 255),
                                 (juce::uint8) ((cb * alpha + p->getBlue() * inv) / 255));
            }
        }
}

bool FramebufferContext::blitRaw (const juce::Image& image, int scale)
{
    const auto t = stack->transform.getTransform();
    if (! t.isOnlyTranslation() || image.isNull())
        return false;
    const int ox = juce::roundToInt (t.getTranslationX()), oy = juce::roundToInt (t.getTranslationY());
    const auto clip = absoluteClip().getIntersection ({ ox, oy, image.getWidth() * scale, image.getHeight() * scale });
    if (clip.isEmpty())
        return true;

    const juce::Image::BitmapData src (image, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData dest (m_image, juce::Image::BitmapData::readWrite);
    std::vector<juce::PixelARGB> row ((std::size_t) image.getWidth());
    int lastSourceRow = -1;
    for (int y = clip.getY(); y < clip.getBottom(); ++y)
    {
        // One source row read once, then each of its pixels written scale times.
        if (const int sy = (y - oy) / scale; sy != lastSourceRow)
        {
            lastSourceRow = sy;
            for (int sx = 0; sx < image.getWidth(); ++sx)
            {
                const auto c = src.getPixelColour (sx, sy);
                row[(std::size_t) sx].setARGB (255, c.getRed(), c.getGreen(), c.getBlue());
            }
        }
        auto* out = reinterpret_cast<juce::PixelARGB*> (dest.getLinePointer (y));
        for (int x = clip.getX(); x < clip.getRight(); ++x)
            out[x] = row[(std::size_t) ((x - ox) / scale)];
        std::fill_n (m_mask.begin() + (std::ptrdiff_t) (y * m_width + clip.getX()), clip.getWidth(), Hud::kRaw);
    }
    return true;
}


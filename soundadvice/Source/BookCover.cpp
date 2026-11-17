#include "BookCover.h"

namespace
{
    float easeInOut (float t)
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow (-2.0f * t + 2.0f, 3.0f) * 0.5f;
    }

    /** Smooth noise in 0..1 from a lattice of hashed values. */
    float valueNoise (float x, float y, int seed)
    {
        auto hash = [seed] (int ix, int iy)
        {
            auto n = (juce::uint32) (ix * 374761393 + iy * 668265263 + seed * 1274126177);
            n = (n ^ (n >> 13)) * 1274126177u;
            return (float) ((n ^ (n >> 16)) & 0xffff) / 65535.0f;
        };
        const auto fx = std::floor (x), fy = std::floor (y);
        const auto ix = (int) fx, iy = (int) fy;
        auto tx = x - fx, ty = y - fy;
        tx = tx * tx * (3.0f - 2.0f * tx);
        ty = ty * ty * (3.0f - 2.0f * ty);
        const auto top = hash (ix, iy) + (hash (ix + 1, iy) - hash (ix, iy)) * tx;
        const auto bottom = hash (ix, iy + 1) + (hash (ix + 1, iy + 1) - hash (ix, iy + 1)) * tx;
        return top + (bottom - top) * ty;
    }

    /** Three octaves of it, still 0..1. */
    float fbm (float x, float y, int seed)
    {
        return valueNoise (x, y, seed) * 0.57f + valueNoise (x * 2.1f, y * 2.1f, seed + 1) * 0.29f
             + valueNoise (x * 4.3f, y * 4.3f, seed + 2) * 0.14f;
    }

    void lozenge (juce::Path& p, juce::Point<float> c, float size)
    {
        p.startNewSubPath (c.x, c.y - size);
        p.lineTo (c.x + size, c.y);
        p.lineTo (c.x, c.y + size);
        p.lineTo (c.x - size, c.y);
        p.closeSubPath();
    }
}

BookCover::BookCover()
{
    setInterceptsMouseClicks (false, false);
    setVisible (false);
}

void BookCover::setNight (bool shouldBeNight)
{
    if (night != shouldBeNight)
    {
        night = shouldBeNight;
        cover = {};
        repaint();
    }
}

void BookCover::openWhenShown (std::function<void()> onOpening)
{
    onOpen = std::move (onOpening);
    stage = Stage::waitingToAppear;
    frozen = false;
    setVisible (true);
    toFront (false);
}

void BookCover::shutAndReopen (std::function<void()> whileShut)
{
    onShut = std::move (whileShut);   // a newer request replaces one that hasn't happened yet
    frozen = false;
    lastTick = juce::Time::getMillisecondCounterHiRes();

    if (stage == Stage::open)
    {
        stage = Stage::shutting;
        position = 0.0f;
    }
    else if (stage == Stage::opening)
    {
        // Caught part-way open: swing back from where it is.
        stage = Stage::shutting;
        position = 1.0f - position;
    }
    // waiting, held or already shutting: it will run `whileShut` when it gets there.

    setVisible (true);
    toFront (false);
    repaint();
}

void BookCover::freezeAt (float openness)
{
    frozen = true;
    frozenOpenness = juce::jlimit (0.0f, 1.0f, openness);
    setVisible (true);
    toFront (false);
    repaint();
}

void BookCover::tick()
{
    if (frozen || stage == Stage::open)
        return;

    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto dt = (float) juce::jlimit (0.0, 0.1, (now - lastTick) / 1000.0);
    lastTick = now;

    auto startOpening = [this]
    {
        if (auto callback = std::exchange (onShut, nullptr))
            callback();
        if (auto callback = std::exchange (onOpen, nullptr))
            callback();
        stage = Stage::opening;
        position = 0.0f;
    };

    switch (stage)
    {
        case Stage::waitingToAppear:      // first tick on screen: start the pause before opening
            stage = Stage::held;
            holdSeconds = introHoldSeconds;
            position = 0.0f;
            break;

        case Stage::held:
            position += dt / holdSeconds;
            if (position >= 1.0f)
                startOpening();
            break;

        case Stage::shutting:
            position += dt / shutSeconds;
            if (position >= 1.0f)
            {
                stage = Stage::held;
                holdSeconds = shutHoldSeconds;
                position = 0.0f;
            }
            break;

        case Stage::opening:
            position += dt / openSeconds;
            if (position >= 1.0f)
            {
                stage = Stage::open;
                setVisible (false);
            }
            break;

        case Stage::open:
            break;
    }
    repaint();
}

//==============================================================================
void BookCover::renderCover (float scale)
{
    const auto w = getWidth(), h = getHeight();
    coverScale = scale;
    cover = juce::Image (juce::Image::ARGB, juce::roundToInt ((float) w * scale), juce::roundToInt ((float) h * scale), true);
    const auto r = juce::Rectangle<float> ((float) w, (float) h);
    const auto pw = cover.getWidth(), ph = cover.getHeight();

    // Leather: oxblood by day, near-black at night. An old binding: the colour is uneven, the edges
    // and corners are rubbed pale where hands and shelves have worn them, and the grain is coarse.
    const auto leather = night ? juce::Colour (0xff17181f) : juce::Colour (0xff4b1f1b);
    const auto rubbed  = night ? juce::Colour (0xff53545c) : juce::Colour (0xff9a6f56);
    {
        juce::Graphics g (cover);
        g.addTransform (juce::AffineTransform::scale (scale));
        g.setGradientFill (juce::ColourGradient (leather.brighter (0.10f), r.getCentreX(), r.getHeight() * 0.4f,
                                                 leather.darker (0.55f), 0.0f, r.getBottom() * 1.25f, true));
        g.fillRect (r);
    }
    {
        juce::Image::BitmapData data (cover, juce::Image::BitmapData::readWrite);
        juce::Random rng (77);
        const auto fw = (float) w, fh = (float) h;
        for (int y = 0; y < ph; ++y)
        {
            const auto py = (float) y / scale;
            for (int x = 0; x < pw; ++x)
            {
                const auto px = (float) x / scale;
                auto c = data.getPixelColour (x, y);

                // Blotchy colour, large and small.
                const auto mottle = fbm (px / 85.0f, py / 85.0f, 11) - 0.5f;
                const auto pores  = fbm (px / 5.0f, py / 5.0f, 23) - 0.5f;
                const auto shade = mottle * 0.55f + pores * 0.22f + (rng.nextFloat() - 0.5f) * 0.10f;
                c = shade > 0.0f ? c.brighter (shade * 0.9f) : c.darker (-shade * 1.3f);

                // Rubbed edges: strongest right at the board's edge and round the corners, broken up by noise.
                const auto edge = juce::jmin (px - 16.0f, fw - px, py, fh - py);   // the spine side wears differently
                const auto cornerX = juce::jmin (px - 16.0f, fw - px), cornerY = juce::jmin (py, fh - py);
                const auto corner = std::sqrt (cornerX * cornerX + cornerY * cornerY);
                const auto ragged = fbm (px / 11.0f, py / 11.0f, 37);
                auto wear = juce::jlimit (0.0f, 1.0f, 1.0f - edge / (5.0f + 16.0f * ragged));
                wear = juce::jmax (wear, juce::jlimit (0.0f, 1.0f, 1.0f - corner / (22.0f + 34.0f * ragged)));
                // A few scuffed patches out on the board.
                const auto scuff = juce::jlimit (0.0f, 1.0f, (fbm (px / 46.0f + 9.0f, py / 30.0f, 51) - 0.66f) * 5.0f) * (0.35f + 0.65f * ragged);
                wear = juce::jmax (wear * wear, scuff * 0.55f);
                if (px > 16.0f)
                    c = c.interpolatedWith (rubbed, wear * 0.62f);

                data.setPixelColour (x, y, c);
            }
        }
    }

    juce::Graphics g (cover);
    g.addTransform (juce::AffineTransform::scale (scale));
    juce::Random rng (2005);

    // Old stains: soft dark blooms, one with the ring a cup leaves.
    for (int i = 0; i < 4; ++i)
    {
        const juce::Point<float> at (r.getWidth() * (0.15f + 0.7f * rng.nextFloat()), r.getHeight() * (0.12f + 0.76f * rng.nextFloat()));
        const auto size = 40.0f + 70.0f * rng.nextFloat();
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.16f), at.x, at.y,
                                                 juce::Colours::transparentBlack, at.x + size, at.y, true));
        g.fillEllipse (juce::Rectangle<float> (size * 2.0f, size * 1.5f).withCentre (at));
    }
    g.setColour (juce::Colours::black.withAlpha (0.10f));
    g.drawEllipse (juce::Rectangle<float> (58.0f, 56.0f).withCentre ({ r.getWidth() * 0.80f, r.getHeight() * 0.73f }), 2.2f);

    // Scratches and nicks: mostly pale where the surface has been scored, a few dark where it has cut in.
    for (int i = 0; i < 46; ++i)
    {
        juce::Point<float> p (r.getWidth() * rng.nextFloat(), r.getHeight() * rng.nextFloat());
        const auto angle = rng.nextFloat() * juce::MathConstants<float>::twoPi;
        const auto length = i < 8 ? 60.0f + 120.0f * rng.nextFloat() : 6.0f + 40.0f * rng.nextFloat();
        const auto bend = (rng.nextFloat() - 0.5f) * 0.5f;
        juce::Path scratch;
        scratch.startNewSubPath (p);
        const auto mid = p + juce::Point<float> (std::cos (angle + bend), std::sin (angle + bend)) * (length * 0.5f);
        const auto end = p + juce::Point<float> (std::cos (angle), std::sin (angle)) * length;
        scratch.quadraticTo (mid, end);
        const bool dark = rng.nextFloat() < 0.3f;
        g.setColour ((dark ? juce::Colours::black : rubbed.brighter (0.3f)).withAlpha (dark ? 0.22f : 0.05f + 0.13f * rng.nextFloat()));
        g.strokePath (scratch, juce::PathStrokeType (0.4f + 0.5f * rng.nextFloat()));
    }

    // The spine edge: dark where the board hinges, and cracked from being opened.
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.62f), 0.0f, 0.0f,
                                             juce::Colours::transparentBlack, 30.0f, 0.0f, false));
    g.fillRect (r.withWidth (30.0f));
    g.setColour (rubbed.withAlpha (0.16f));
    g.fillRect (16.5f, 0.0f, 1.2f, r.getHeight());
    for (int i = 0; i < 26; ++i)
    {
        auto y = r.getHeight() * rng.nextFloat();
        auto x = 12.0f + 14.0f * rng.nextFloat();
        juce::Path crack;
        crack.startNewSubPath (x, y);
        const auto steps = 3 + rng.nextInt (5);
        for (int k = 0; k < steps; ++k)
        {
            x += (rng.nextFloat() - 0.5f) * 3.0f;
            y += 3.0f + 7.0f * rng.nextFloat();
            crack.lineTo (x, y);
        }
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.strokePath (crack, juce::PathStrokeType (0.6f));
        g.setColour (rubbed.withAlpha (0.10f));
        g.strokePath (crack, juce::PathStrokeType (0.5f), juce::AffineTransform::translation (0.7f, 0.0f));
    }

    // Gilt, on a layer of its own so it can be worn away afterwards.
    juce::Image gilt (juce::Image::ARGB, pw, ph, true);
    {
        juce::Graphics gg (gilt);
        gg.addTransform (juce::AffineTransform::scale (scale));
        gg.setGradientFill (juce::ColourGradient (juce::Colour (0xffd9bb7c), 0.0f, r.getHeight() * 0.2f,
                                                  juce::Colour (0xff94703a), 0.0f, r.getHeight() * 0.85f, false));

        const auto outer = r.reduced (26.0f, 20.0f).withTrimmedLeft (8.0f);
        gg.drawRect (outer, 1.4f);
        const auto inner = outer.reduced (5.0f);
        gg.drawRect (inner, 0.7f);

        juce::Path corners;
        for (auto c : { inner.getTopLeft(), inner.getTopRight(), inner.getBottomLeft(), inner.getBottomRight() })
        {
            const juce::Point<float> in (c.x < inner.getCentreX() ? 9.0f : -9.0f, c.y < inner.getCentreY() ? 9.0f : -9.0f);
            lozenge (corners, c + in, 3.2f);
        }
        gg.fillPath (corners);

        // The device: an opening quotation mark, the title, a rule with a lozenge.
        const auto centre = inner.getCentre();
        const auto unit = juce::jmin (inner.getWidth() / 9.0f, inner.getHeight() / 5.2f);

        juce::GlyphArrangement mark;
        mark.addLineOfText (Theme::italic (unit * 2.6f), juce::String::fromUTF8 ("\xe2\x80\x9c"), 0.0f, 0.0f);
        juce::Path markPath;
        mark.createPath (markPath);
        markPath.applyTransform (markPath.getTransformToScaleToFit (
            juce::Rectangle<float> (unit * 1.5f, unit * 1.1f).withCentre ({ centre.x - unit * 0.06f, centre.y - unit * 1.15f }), true));
        gg.fillPath (markPath);

        gg.setFont (Theme::caps (unit * 0.74f, 0.3f));
        gg.drawText ("SOUND ADVICE", inner.withSizeKeepingCentre (inner.getWidth(), unit).translated (unit * 0.1f, unit * 0.2f),
                     juce::Justification::centred, false);

        const juce::Point<float> rule (centre.x, centre.y + unit * 1.05f);
        for (const float dir : { -1.0f, 1.0f })
        {
            juce::Path stroke;
            stroke.startNewSubPath (rule.x + dir * 8.0f, rule.y - 0.7f);
            stroke.lineTo (rule.x + dir * unit * 1.5f, rule.y);
            stroke.lineTo (rule.x + dir * 8.0f, rule.y + 0.7f);
            stroke.closeSubPath();
            gg.fillPath (stroke);
        }
        juce::Path centreLozenge;
        lozenge (centreLozenge, rule, 3.6f);
        gg.strokePath (centreLozenge, juce::PathStrokeType (1.0f));
    }
    {
        // The gold has flaked: gone in patches, thin elsewhere, and rubbed off most towards the edges.
        juce::Image::BitmapData data (gilt, juce::Image::BitmapData::readWrite);
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x)
            {
                auto c = data.getPixelColour (x, y);
                if (c.getAlpha() == 0)
                    continue;
                const auto px = (float) x / scale, py = (float) y / scale;
                const auto patch = fbm (px / 34.0f, py / 34.0f, 71), fleck = fbm (px / 3.2f, py / 3.2f, 83);
                const auto edge = juce::jmin (px, (float) w - px, py, (float) h - py);
                const auto nearEdge = juce::jlimit (0.0f, 1.0f, 1.0f - edge / 60.0f);
                auto keep = juce::jlimit (0.0f, 1.0f, (patch * 0.75f + fleck * 0.45f - 0.34f - nearEdge * 0.16f) * 4.5f);
                keep = 0.12f + 0.80f * keep;   // a ghost of the tooling stays where the gold has gone
                data.setPixelColour (x, y, c.withMultipliedAlpha (keep));
            }
    }
    g.setOpacity (1.0f);
    g.drawImageTransformed (gilt, juce::AffineTransform::scale (1.0f / scale));

    // Darkened towards the edges, as an old board is.
    g.setGradientFill (juce::ColourGradient (juce::Colours::transparentBlack, r.getCentreX(), r.getCentreY(),
                                             juce::Colours::black.withAlpha (0.2f), 0.0f, 0.0f, true));
    g.fillRect (r);
}

void BookCover::paintCover (juce::Graphics& g, float openness)
{
    if (openness >= 0.999f)
        return;

    const auto w = (float) getWidth(), h = (float) getHeight();
    const auto angle = openness * juce::MathConstants<float>::halfPi;   // 0 = flat on the book, 90 degrees = edge on
    const auto across = std::cos (angle), towards = std::sin (angle);
    const auto visible = w * across;

    // The shadow the board throws across the page as it lifts.
    const auto reach = 18.0f + 70.0f * towards;
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.42f * (1.0f - openness * 0.5f)), visible, 0.0f,
                                             juce::Colours::transparentBlack, visible + reach, 0.0f, false));
    g.fillRect (visible, 0.0f, reach, h);

    // The board, hinged on the left: narrower as it swings, and taller towards its free edge as that
    // edge comes towards you. Drawn in vertical strips so the far edge can be a different height.
    // Narrow strips, so the slanted top and bottom edges do not visibly step.
    const auto strips = juce::jlimit (8, 200, juce::roundToInt (visible * 0.5f));
    const auto imageWidth = cover.getWidth();
    const auto imageHeight = cover.getHeight();
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    for (int i = 0; i < strips; ++i)
    {
        const auto u0 = (float) i / (float) strips, u1 = (float) (i + 1) / (float) strips;
        const auto height = h * (1.0f + 0.2f * towards * (u0 + u1) * 0.5f);
        const auto sx0 = juce::jlimit (0, imageWidth - 1, (int) (u0 * (float) imageWidth));
        const auto sx1 = juce::jlimit (sx0 + 1, imageWidth, (int) std::ceil (u1 * (float) imageWidth));
        // A hair of overlap so no seams show between strips.
        const juce::Rectangle<float> target (u0 * visible, (h - height) * 0.5f, (u1 - u0) * visible + 0.5f, height);
        g.drawImage (cover.getClippedImage ({ sx0, 0, sx1 - sx0, imageHeight }), target, juce::RectanglePlacement::stretchToFit);
    }

    // It darkens as it turns away from the light, with a glint down the free edge.
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.55f * towards), 0.0f, 0.0f,
                                             juce::Colours::black.withAlpha (0.2f * towards), visible, 0.0f, false));
    g.fillRect (0.0f, -h, visible, h * 3.0f);
    g.setColour (juce::Colours::white.withAlpha (0.22f * towards));
    g.fillRect (visible - 1.2f, -h, 1.2f, h * 3.0f);
}

void BookCover::paint (juce::Graphics& g)
{
    float openness = 1.0f;

    if (frozen)
        openness = frozenOpenness;
    else if (! isShowing())
        return;   // an off-screen picture of the window shows the page, not the cover
    else
        switch (stage)
        {
            case Stage::waitingToAppear:
            case Stage::held:      openness = 0.0f; break;
            case Stage::shutting:  openness = 1.0f - easeInOut (position); break;
            case Stage::opening:   openness = easeInOut (position); break;
            case Stage::open:      return;
        }

    const auto scale = juce::jlimit (1.0f, 3.0f, (float) g.getInternalContext().getPhysicalPixelScaleFactor());
    if (! cover.isValid() || std::abs (coverScale - scale) > 0.01f)
        renderCover (scale);

    paintCover (g, openness);
}

#include "BookCover.h"

namespace
{
    float easeInOut (float t)
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow (-2.0f * t + 2.0f, 3.0f) * 0.5f;
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
    juce::Graphics g (cover);
    g.addTransform (juce::AffineTransform::scale (scale));
    const auto r = juce::Rectangle<float> ((float) w, (float) h);

    // Leather: oxblood by day, near-black at night, darker at the edges, with a grain.
    const auto leather = night ? juce::Colour (0xff17181f) : juce::Colour (0xff4b1f1b);
    g.setGradientFill (juce::ColourGradient (leather.brighter (0.10f), r.getCentreX(), r.getHeight() * 0.4f,
                                             leather.darker (0.55f), 0.0f, r.getBottom() * 1.25f, true));
    g.fillRect (r);
    {
        juce::Image grain (juce::Image::ARGB, cover.getWidth(), cover.getHeight(), true);
        juce::Image::BitmapData data (grain, juce::Image::BitmapData::writeOnly);
        juce::Random rng (77);
        for (int y = 0; y < data.height; ++y)
            for (int x = 0; x < data.width; ++x)
            {
                const auto v = rng.nextFloat();
                data.setPixelColour (x, y, v > 0.5f ? juce::Colours::white.withAlpha ((v - 0.5f) * 0.07f)
                                                    : juce::Colours::black.withAlpha ((0.5f - v) * 0.22f));
            }
        g.drawImageTransformed (grain, juce::AffineTransform::scale (1.0f / scale));
    }

    // The spine edge, with a worn highlight where the board hinges.
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.55f), 0.0f, 0.0f,
                                             juce::Colours::transparentBlack, 26.0f, 0.0f, false));
    g.fillRect (r.withWidth (26.0f));
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.fillRect (17.0f, 0.0f, 1.0f, r.getHeight());

    // Gilt.
    const auto gold = juce::ColourGradient (juce::Colour (0xffe6c98b), 0.0f, r.getHeight() * 0.2f,
                                            juce::Colour (0xffa57a3c), 0.0f, r.getHeight() * 0.85f, false);
    g.setGradientFill (gold);

    const auto outer = r.reduced (26.0f, 20.0f).withTrimmedLeft (8.0f);
    g.drawRect (outer, 1.4f);
    const auto inner = outer.reduced (5.0f);
    g.drawRect (inner, 0.7f);

    juce::Path corners;
    for (auto c : { inner.getTopLeft(), inner.getTopRight(), inner.getBottomLeft(), inner.getBottomRight() })
    {
        const juce::Point<float> in (c.x < inner.getCentreX() ? 9.0f : -9.0f, c.y < inner.getCentreY() ? 9.0f : -9.0f);
        lozenge (corners, c + in, 3.2f);
    }
    g.fillPath (corners);

    // The device: an opening quotation mark, the title, a rule with a lozenge.
    const auto centre = inner.getCentre();
    const auto unit = juce::jmin (inner.getWidth() / 9.0f, inner.getHeight() / 5.2f);

    juce::GlyphArrangement mark;
    mark.addLineOfText (Theme::italic (unit * 2.6f), juce::String::fromUTF8 ("\xe2\x80\x9c"), 0.0f, 0.0f);
    juce::Path markPath;
    mark.createPath (markPath);
    markPath.applyTransform (markPath.getTransformToScaleToFit (
        juce::Rectangle<float> (unit * 1.5f, unit * 1.1f).withCentre ({ centre.x - unit * 0.06f, centre.y - unit * 1.15f }), true));
    g.fillPath (markPath);

    const auto titleFont = Theme::caps (unit * 0.74f, 0.3f);
    g.setFont (titleFont);
    g.drawText ("SOUND ADVICE", inner.withSizeKeepingCentre (inner.getWidth(), unit).translated (unit * 0.1f, unit * 0.2f),
                juce::Justification::centred, false);

    const juce::Point<float> rule (centre.x, centre.y + unit * 1.05f);
    for (const float dir : { -1.0f, 1.0f })
    {
        juce::Path stroke;
        stroke.startNewSubPath (rule.x + dir * 8.0f, rule.y - 0.7f);
        stroke.lineTo (rule.x + dir * unit * 1.5f, rule.y);
        stroke.lineTo (rule.x + dir * 8.0f, rule.y + 0.7f);
        stroke.closeSubPath();
        g.fillPath (stroke);
    }
    juce::Path centreLozenge;
    lozenge (centreLozenge, rule, 3.6f);
    g.strokePath (centreLozenge, juce::PathStrokeType (1.0f));
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

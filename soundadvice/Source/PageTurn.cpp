#include "PageTurn.h"

namespace
{
    float easeInOut (float t)
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow (-2.0f * t + 2.0f, 3.0f) * 0.5f;
    }

    /** Everything on the side of the line (through `onLine`, unit normal `n`) that `n` points away from. */
    juce::Path halfPlaneBehind (juce::Point<float> onLine, juce::Point<float> n, float reach)
    {
        const juce::Point<float> along (-n.y, n.x);
        juce::Path p;
        p.startNewSubPath (onLine + along * reach);
        p.lineTo (onLine - along * reach);
        p.lineTo (onLine - along * reach - n * reach);
        p.lineTo (onLine + along * reach - n * reach);
        p.closeSubPath();
        return p;
    }
}

PageTurn::PageTurn()
{
    setInterceptsMouseClicks (false, false);
    setVisible (false);
}

void PageTurn::begin (juce::Image oldPage, float scale, Kind k, const Theme::Palette& oldPalette)
{
    page = std::move (oldPage);
    pageScale = scale;
    kind = k;
    paper = oldPalette.background;
    ink = oldPalette.ink;
    progress = 0.0f;
    frozen = false;
    lastTick = juce::Time::getMillisecondCounterHiRes();
    setVisible (true);
    toFront (false);
    repaint();
}

void PageTurn::freezeAt (float position)
{
    frozen = true;
    progress = juce::jlimit (0.0f, 0.999f, position);
    setVisible (true);
    repaint();
}

void PageTurn::tick()
{
    if (frozen || progress >= 1.0f)
        return;

    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto dt = (float) juce::jlimit (0.0, 0.1, (now - lastTick) / 1000.0);
    lastTick = now;
    progress += dt / (kind == Kind::dissolve ? dissolveSeconds : turnSeconds);

    if (progress >= 1.0f)
    {
        progress = 1.0f;
        setVisible (false);
        page = {};
    }
    repaint();
}

void PageTurn::paint (juce::Graphics& g)
{
    if (! page.isValid() || progress >= 1.0f)
        return;

    if (kind == Kind::dissolve)
    {
        g.setOpacity (1.0f - easeInOut (progress));
        g.drawImageTransformed (page, juce::AffineTransform::scale (1.0f / pageScale));
        return;
    }

    paintTurn (g, easeInOut (progress));
}

void PageTurn::paintTurn (juce::Graphics& g, float e)
{
    const auto w = (float) getWidth(), h = (float) getHeight();
    const auto reach = (w + h) * 2.0f;

    // Work as if turning forwards; a backward turn is the same picture mirrored left to right.
    const auto mirror = kind == Kind::backward ? juce::AffineTransform::scale (-1.0f, 1.0f).translated (w, 0.0f)
                                               : juce::AffineTransform();
    const auto drawPage = juce::AffineTransform::scale (1.0f / pageScale).followedBy (mirror);
    juce::Graphics::ScopedSaveState state (g);
    g.addTransform (mirror);

    // The fold: a line leaning so the bottom corner lifts first, straightening as it crosses the page.
    const auto lean = 0.2f * (1.0f - 0.85f * e);
    const juce::Point<float> n (std::cos (lean), std::sin (lean));
    const auto startOffset = w * n.x + h * n.y + 2.0f;                 // just past the bottom-right corner
    const auto offset = startOffset + (-2.0f - startOffset) * e;        // ...to just past the top-left
    const auto onLine = n * offset;
    const auto lift = std::sin (juce::MathConstants<float>::pi * e);    // 0 at rest, 1 mid-turn

    // 1. The part of the old page still lying flat.
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (halfPlaneBehind (onLine, n, reach));
        g.drawImageTransformed (page, drawPage);
    }

    // 2. The shadow the lifted leaf throws on the page beneath it.
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (halfPlaneBehind (onLine, n * -1.0f, reach));
        const auto far = onLine + n * (30.0f + 50.0f * lift);
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.26f * lift), onLine.x, onLine.y,
                                                 juce::Colours::transparentBlack, far.x, far.y, false));
        g.fillRect (0.0f, 0.0f, w, h);
    }

    // 3. The leaf folded over: the back of the page, reflected in the fold.
    const juce::AffineTransform reflect (1.0f - 2.0f * n.x * n.x, -2.0f * n.x * n.y, 2.0f * offset * n.x,
                                         -2.0f * n.x * n.y, 1.0f - 2.0f * n.y * n.y, 2.0f * offset * n.y);
    juce::Path leaf;
    leaf.addRectangle (0.0f, 0.0f, w, h);
    leaf.applyTransform (reflect);

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (halfPlaneBehind (onLine, n, reach));

        juce::DropShadow (juce::Colours::black.withAlpha (0.3f * lift), 22, { -6, 8 }).drawForPath (g, leaf);

        g.setColour (paper.interpolatedWith (ink, 0.05f));
        g.fillPath (leaf);

        // The print shows faintly through the paper.
        {
            juce::Graphics::ScopedSaveState through (g);
            g.reduceClipRegion (leaf);
            g.setOpacity (0.06f);
            g.drawImageTransformed (page, juce::AffineTransform::scale (1.0f / pageScale).followedBy (reflect));
        }

        // The curl: dark in the crease, a soft sheen a little way in, then flat paper.
        const auto into = onLine - n * 90.0f;
        juce::ColourGradient curl (juce::Colours::black.withAlpha (0.24f), onLine.x, onLine.y,
                                   juce::Colours::transparentBlack, into.x, into.y, false);
        curl.addColour (0.22, juce::Colours::white.withAlpha (0.10f));
        curl.addColour (0.55, juce::Colours::black.withAlpha (0.04f));
        g.setGradientFill (curl);
        g.fillPath (leaf);
    }

    // A fine light along the crease.
    const juce::Point<float> along (-n.y, n.x);
    g.setColour (juce::Colours::white.withAlpha (0.16f * lift));
    g.drawLine ({ onLine + along * reach, onLine - along * reach }, 0.8f);
}

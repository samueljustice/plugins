#include "QuoteView.h"

namespace
{
    constexpr float fadeOutSeconds = 0.16f;
    constexpr float maxGlyphSeconds = 0.42f;   // how long one letter takes to be drawn and filled
    constexpr float minGlyphSeconds = 0.24f;   // ...in a long entry, so the wet edge stays a line or so wide
    constexpr float maxGlyphStep = 0.022f;     // pause between letters, for short entries
    constexpr float bodyBudget = 1.0f;         // long entries are written within about this long
    constexpr double inkFinished = 1.0e6;
    constexpr float maxBodySize = 31.0f;
    constexpr float minBodySize = 13.0f;
    constexpr float maxMeasure = 600.0f;   // longest comfortable line, in pixels

    const juce::String openQuote  = juce::String::fromUTF8 ("\xe2\x80\x9c");
    const juce::String closeQuote = juce::String::fromUTF8 ("\xe2\x80\x9d");
    const juce::String middleDot  = juce::String::fromUTF8 (" \xc2\xb7 ");

    float easeOut (float t) { return 1.0f - std::pow (1.0f - juce::jlimit (0.0f, 1.0f, t), 3.0f); }
    float smooth (float t)  { t = juce::jlimit (0.0f, 1.0f, t); return t * t * (3.0f - 2.0f * t); }

    /** The first `fraction` of a polyline, by length, added to `path`. */
    void addPartialContour (juce::Path& path, const std::vector<juce::Point<float>>& points, float length,
                            float fraction, juce::Point<float> offset)
    {
        if (points.size() < 2 || length <= 0.0f)
            return;

        auto remaining = length * fraction;
        path.startNewSubPath (points.front() + offset);
        for (size_t i = 1; i < points.size() && remaining > 0.0f; ++i)
        {
            const auto segment = points[i - 1].getDistanceFrom (points[i]);
            if (segment <= remaining)
                path.lineTo (points[i] + offset);
            else if (segment > 0.0f)
                path.lineTo (points[i - 1] + (points[i] - points[i - 1]) * (remaining / segment) + offset);
            remaining -= segment;
        }
    }
}

QuoteView::QuoteView (const QuoteLibrary& lib) : library (lib)
{
    setOpaque (false);
    setRepaintsOnMouseActivity (false);
}

void QuoteView::setPalette (const Theme::Palette& p)
{
    palette = p;
    rebuildLayout();
    repaint();
}

void QuoteView::showEntry (int entryIndex, bool animate)
{
    const bool midWriting = phase >= 2.0f && ink < (double) layout.inkEnd;

    if (! animate || shown < 0 || ! isShowing() || midWriting)
    {
        // Nothing settled on screen to fade out: switch now (and start writing, if animating).
        shown = entryIndex;
        incoming = -1;
        phase = 2.0f;
        introPending = false;
        ink = (animate && isShowing()) ? 0.0 : inkFinished;
        lastTick = juce::Time::getMillisecondCounterHiRes();
        rebuildLayout();
        repaint();
        return;
    }

    incoming = entryIndex;
    if (phase >= 1.0f)
        phase = 0.0f;
    lastTick = juce::Time::getMillisecondCounterHiRes();
}

void QuoteView::showEntryAfter (int entryIndex, double delaySeconds)
{
    shown = entryIndex;
    incoming = -1;
    phase = 2.0f;
    introPending = false;
    ink = -delaySeconds;
    lastTick = juce::Time::getMillisecondCounterHiRes();
    rebuildLayout();
    repaint();
}

juce::String QuoteView::getClipboardText() const
{
    if (! juce::isPositiveAndBelow (shown, (int) library.getEntries().size()))
        return {};
    const auto& e = library.getEntries()[(size_t) shown];
    const auto& p = library.getPerson (e);
    auto text = e.verbatim ? openQuote + e.text + closeQuote : e.text;
    if (e.verbatim && e.context.isNotEmpty())
        text = "On " + e.context + ": " + text;
    text << "\n— " << p.name;
    if (p.role.isNotEmpty())
        text << ", " << p.role;
    if (e.source.url.isNotEmpty())
        text << "\n" << e.source.url;
    return text;
}

double QuoteView::currentInk() const
{
    // Until the first tick of an intro, an on-screen page shows nothing written yet.
    return (introPending && isShowing()) ? 0.0 : ink;
}

void QuoteView::tick()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto dt = juce::jlimit (0.0, 0.1, (now - lastTick) / 1000.0);

    if (introPending)
    {
        introPending = false;
        phase = 2.0f;
        ink = 0.0;
        lastTick = now;
        repaint();
        return;
    }

    if (phase < 1.0f)
    {
        lastTick = now;
        phase += (float) dt / fadeOutSeconds;
        if (phase >= 1.0f)
        {
            // The old entry has gone: put the new one down and start writing it.
            phase = 2.0f;
            if (incoming >= 0)
            {
                shown = incoming;
                incoming = -1;
                rebuildLayout();
            }
            ink = 0.0;
        }
        repaint();
    }
    else if (ink < (double) layout.inkEnd)
    {
        lastTick = now;
        ink += dt;
        repaint();
    }
}

//==============================================================================
void QuoteView::resized()
{
    rebuildLayout();
}

void QuoteView::rebuildLayout()
{
    layout = {};
    if (! juce::isPositiveAndBelow (shown, (int) library.getEntries().size()) || getWidth() <= 0)
        return;

    const auto& e = library.getEntries()[(size_t) shown];
    const auto& p = library.getPerson (e);
    auto area = getLocalBounds().toFloat();

    // Bottom up: source line, role line, name, a short rule.
    layout.sourceArea = area.removeFromBottom (20.0f).reduced ((float) cornerReserve, 0.0f);
    area.removeFromBottom (12.0f);
    layout.roleArea = area.removeFromBottom (18.0f);
    area.removeFromBottom (1.0f);
    layout.nameArea = area.removeFromBottom (19.0f);
    area.removeFromBottom (10.0f);
    layout.ruleArea = area.removeFromBottom (1.0f).withSizeKeepingCentre (26.0f, 1.0f);
    area.removeFromBottom (16.0f);

    layout.name = p.name.toUpperCase();
    layout.role = p.role;
    if (p.credits.isNotEmpty())
        layout.role << (layout.role.isNotEmpty() ? middleDot : juce::String()) << p.credits;

    const auto& s = e.source;
    layout.source = s.publication.isNotEmpty() ? s.publication : s.title;
    if (s.year > 0)
        layout.source << middleDot << juce::String (s.year);
    layout.source = layout.source.toUpperCase();

    if (! e.verbatim)
        layout.label = "FIELD NOTE";
    else if (e.context.isNotEmpty())
        layout.label = ("On " + e.context).toUpperCase();
    const float labelHeight = layout.label.isNotEmpty() ? 22.0f : 0.0f;

    // The words: the largest size that fits, on a comfortable measure.
    const float measure = juce::jmin (area.getWidth() - 8.0f, maxMeasure);
    const float available = area.getHeight() - labelHeight;
    const auto words = e.verbatim ? openQuote + e.text + closeQuote : e.text;

    for (float size = maxBodySize; size >= minBodySize; size -= 0.5f)
    {
        juce::AttributedString as;
        as.setJustification (juce::Justification::centredTop);
        as.setWordWrap (juce::AttributedString::byWord);
        as.setLineSpacing (size * 0.14f);
        as.append (words, e.verbatim ? Theme::italic (size) : Theme::text (size), palette.ink);

        layout.body = {};
        layout.body.createLayout (as, measure);
        if (layout.body.getHeight() <= available || size - 0.5f < minBodySize)
            break;
    }

    const float bodyHeight = juce::jmin (layout.body.getHeight(), available);
    const float blockHeight = labelHeight + bodyHeight;
    // Sit a touch above centre: the eye reads optical centre higher than geometric.
    const float top = area.getY() + juce::jmax (0.0f, (area.getHeight() - blockHeight) * 0.44f);
    layout.labelArea = { area.getX(), top, area.getWidth(), labelHeight };
    layout.bodyArea = juce::Rectangle<float> (measure, bodyHeight)
                          .withCentre ({ area.getCentreX(), top + labelHeight + bodyHeight * 0.5f });

    // The letters, in reading order, for writing in.
    for (const auto& line : layout.body)
    {
        for (const auto* run : line.runs)
        {
            layout.bodyFont = run->font;
            const auto typeface = run->font.getTypefacePtr();
            if (typeface == nullptr)
                continue;
            const auto scale = juce::AffineTransform::scale (run->font.getHeight() * run->font.getHorizontalScale(),
                                                             run->font.getHeight());
            for (const auto& glyph : run->glyphs)
            {
                InkGlyph ig;
                ig.code = (juce::uint16) glyph.glyphCode;
                ig.position = line.lineOrigin + glyph.anchor;

                juce::Path raw;
                typeface->getOutlineForGlyph (run->font.getMetricsKind(), glyph.glyphCode, raw);
                if (raw.isEmpty())
                    continue;   // a space
                ig.outline.addPath (raw, scale.translated (ig.position));

                int subPath = -1;
                for (juce::PathFlatteningIterator it (ig.outline, {}, 0.25f); it.next();)
                {
                    if (it.subPathIndex != subPath)
                    {
                        subPath = it.subPathIndex;
                        ig.contours.push_back ({ { it.x1, it.y1 } });
                        ig.lengths.push_back (0.0f);
                    }
                    const juce::Point<float> end (it.x2, it.y2);
                    ig.lengths.back() += ig.contours.back().back().getDistanceFrom (end);
                    ig.contours.back().push_back (end);
                }
                layout.glyphs.push_back (std::move (ig));
            }
        }
    }

    const auto count = (float) juce::jmax ((size_t) 1, layout.glyphs.size());
    const auto step = juce::jmin (maxGlyphStep, bodyBudget / count);
    for (size_t i = 0; i < layout.glyphs.size(); ++i)
        layout.glyphs[i].start = step * (float) i;

    layout.glyphSeconds = juce::jlimit (minGlyphSeconds, maxGlyphSeconds, step * 30.0f);
    layout.bodyEnd = step * count;
    layout.flourishStart = juce::jmax (0.25f, layout.bodyEnd * 0.7f);
    layout.inkEnd = juce::jmax (layout.bodyEnd + layout.glyphSeconds, layout.flourishStart + 0.35f + 0.45f);
}

//==============================================================================
void QuoteView::paint (juce::Graphics& g)
{
    if (shown < 0)
        return;

    if (phase < 1.0f)
        paintEntry (g, 1.0f - easeOut (phase), -3.0f * easeOut (phase));
    else
        paintWriting (g, currentInk());
}

void QuoteView::paintFlourish (juce::Graphics& g, float grow, float alpha)
{
    if (grow <= 0.0f || alpha <= 0.0f)
        return;

    // A fine rule that swells towards a small lozenge at its centre, drawn outwards from the middle.
    const auto centre = layout.ruleArea.getCentre();
    const auto half = 38.0f * grow, gap = 7.0f;
    g.setColour (palette.accent.withMultipliedAlpha (0.8f * alpha));

    for (const float dir : { -1.0f, 1.0f })
    {
        if (half <= gap)
            break;
        juce::Path stroke;
        stroke.startNewSubPath (centre.x + dir * gap, centre.y - 0.6f);
        stroke.lineTo (centre.x + dir * half, centre.y);
        stroke.lineTo (centre.x + dir * gap, centre.y + 0.6f);
        stroke.closeSubPath();
        g.fillPath (stroke);
    }

    const auto size = 3.1f * juce::jmin (1.0f, grow * 2.5f);
    juce::Path lozenge;
    lozenge.startNewSubPath (centre.x, centre.y - size);
    lozenge.lineTo (centre.x + size, centre.y);
    lozenge.lineTo (centre.x, centre.y + size);
    lozenge.lineTo (centre.x - size, centre.y);
    lozenge.closeSubPath();
    g.strokePath (lozenge, juce::PathStrokeType (0.9f));
}

void QuoteView::paintBodyInk (juce::Graphics& g, juce::Point<float> origin, float inkTime)
{
    std::vector<uint16_t> doneCodes;
    std::vector<juce::Point<float>> donePositions;
    const auto penWidth = juce::jmax (0.75f, layout.bodyFont.getHeight() * 0.032f);
    const juce::PathStrokeType pen (penWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    for (const auto& glyph : layout.glyphs)
    {
        const auto u = (inkTime - glyph.start) / layout.glyphSeconds;
        if (u <= 0.0f)
            break;   // in reading order: nothing later has started

        if (u >= 1.0f)
        {
            doneCodes.push_back (glyph.code);
            donePositions.push_back (glyph.position);
            continue;
        }

        // Being written: the outline is drawn in the accent ink, then the letter fills and the line fades.
        const auto drawn = easeOut (u / 0.65f);
        const auto fill = smooth ((u - 0.3f) / 0.7f);
        const auto line = 1.0f - smooth ((u - 0.55f) / 0.45f);

        if (fill > 0.0f)
        {
            g.setColour (palette.ink.withMultipliedAlpha (fill));
            g.fillPath (glyph.outline, juce::AffineTransform::translation (origin));
        }

        juce::Path partial;
        for (size_t c = 0; c < glyph.contours.size(); ++c)
            addPartialContour (partial, glyph.contours[c], glyph.lengths[c], drawn, origin);
        g.setColour (palette.accent.withMultipliedAlpha (line));
        g.strokePath (partial, pen);
    }

    if (! doneCodes.empty())
    {
        // Finished letters go through the same glyph renderer as the settled page, so nothing shifts at the end.
        auto& context = g.getInternalContext();
        context.saveState();
        context.setFont (layout.bodyFont);
        context.setFill (palette.ink);
        context.drawGlyphs (doneCodes, donePositions, juce::AffineTransform::translation (origin));
        context.restoreState();
    }
}

void QuoteView::paintWriting (juce::Graphics& g, double inkTime)
{
    if (inkTime >= (double) layout.inkEnd)
    {
        paintEntry (g, 1.0f, 0.0f);
        return;
    }

    const auto t = (float) inkTime;
    const auto after = t - layout.flourishStart;

    // TextLayout::draw puts a centred-top layout here.
    const juce::Point<float> origin (layout.bodyArea.getCentreX() - layout.body.getWidth() * 0.5f, layout.bodyArea.getY());
    paintBodyInk (g, origin, t);

    paintFlourish (g, easeOut (after / 0.55f), smooth (after / 0.3f));
    paintFurniture (g, smooth (t / 0.35f), smooth ((after - 0.12f) / 0.4f), smooth ((after - 0.22f) / 0.4f),
                    smooth ((after - 0.32f) / 0.4f), 0.0f);
}

void QuoteView::paintEntry (juce::Graphics& g, float alpha, float drift)
{
    if (alpha <= 0.001f)
        return;

    {
        juce::Graphics::ScopedSaveState save (g);
        g.setOpacity (alpha);
        layout.body.draw (g, layout.bodyArea.translated (0.0f, drift));
    }

    paintFlourish (g, 1.0f, alpha);
    paintFurniture (g, alpha, alpha, alpha, alpha, drift);
}

void QuoteView::paintFurniture (juce::Graphics& g, float labelAlpha, float nameAlpha, float roleAlpha,
                                float sourceAlpha, float drift)
{
    if (layout.label.isNotEmpty() && labelAlpha > 0.0f)
    {
        g.setColour (palette.accent.withMultipliedAlpha (labelAlpha));
        g.setFont (Theme::caps (10.5f, 0.32f));
        g.drawText (layout.label, layout.labelArea.translated (0.0f, drift), juce::Justification::centredTop, false);
    }

    if (nameAlpha > 0.0f)
    {
        const auto nameFont = Theme::caps (13.5f, 0.2f);
        const auto rise = 4.0f * (1.0f - nameAlpha);
        g.setFont (nameFont);
        g.setColour ((hover == Hover::name ? palette.accent : palette.ink).withMultipliedAlpha (nameAlpha));
        g.drawText (layout.name, layout.nameArea.translated (0.0f, rise), juce::Justification::centred, true);
        if (hover == Hover::name)
        {
            const auto w = juce::GlyphArrangement::getStringWidth (nameFont, layout.name);
            g.fillRect (juce::Rectangle<float> (w, 0.75f).withCentre ({ layout.nameArea.getCentreX(), layout.nameArea.getBottom() - 1.0f }));
        }
    }

    if (roleAlpha > 0.0f)
    {
        g.setFont (Theme::italic (14.5f));
        g.setColour (palette.muted.withMultipliedAlpha (roleAlpha));
        g.drawFittedText (layout.role, layout.roleArea.translated (0.0f, 3.0f * (1.0f - roleAlpha)).toNearestInt(),
                          juce::Justification::centred, 1, 0.85f);
    }

    if (layout.source.isNotEmpty() && sourceAlpha > 0.0f)
    {
        const auto sourceFont = Theme::caps (10.5f, 0.16f);
        g.setFont (sourceFont);
        const bool hasLink = shown >= 0 && library.getEntries()[(size_t) shown].source.url.isNotEmpty();
        g.setColour ((hover == Hover::source ? palette.accent : palette.muted.withMultipliedAlpha (0.85f)).withMultipliedAlpha (sourceAlpha));
        g.drawFittedText (layout.source, layout.sourceArea.toNearestInt(), juce::Justification::centred, 1, 0.8f);
        if (hover == Hover::source && hasLink)
        {
            const auto w = juce::jmin (layout.sourceArea.getWidth(), juce::GlyphArrangement::getStringWidth (sourceFont, layout.source));
            g.fillRect (juce::Rectangle<float> (w, 0.75f).withCentre ({ layout.sourceArea.getCentreX(), layout.sourceArea.getCentreY() + 7.0f }));
        }
    }
}

//==============================================================================
QuoteView::Hover QuoteView::linkAt (juce::Point<float> pos) const
{
    if (shown < 0)
        return Hover::none;
    const auto& e = library.getEntries()[(size_t) shown];
    const auto& p = library.getPerson (e);

    auto textBox = [] (juce::Rectangle<float> r, const juce::Font& f, const juce::String& s)
    {
        const auto w = juce::jmin (r.getWidth(), juce::GlyphArrangement::getStringWidth (f, s) + 12.0f);
        return r.withSizeKeepingCentre (w, r.getHeight() + 4.0f);
    };

    if (p.imdb.isNotEmpty() && textBox (layout.nameArea, Theme::caps (13.5f, 0.2f), layout.name).contains (pos))
        return Hover::name;
    if (e.source.url.isNotEmpty() && textBox (layout.sourceArea, Theme::caps (10.5f, 0.16f), layout.source).contains (pos))
        return Hover::source;
    return Hover::none;
}

void QuoteView::mouseMove (const juce::MouseEvent& event)
{
    const auto h = linkAt (event.position);
    if (h != hover)
    {
        hover = h;
        setMouseCursor (hover == Hover::none ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);
        repaint();
    }
}

void QuoteView::mouseExit (const juce::MouseEvent&)
{
    if (hover != Hover::none)
    {
        hover = Hover::none;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void QuoteView::openLink (Hover which)
{
    if (shown < 0)
        return;
    const auto& e = library.getEntries()[(size_t) shown];
    const auto url = which == Hover::name ? library.getPerson (e).imdb : e.source.url;
    if (url.startsWith ("https://") || url.startsWith ("http://"))
        juce::URL (url).launchInDefaultBrowser();
}

void QuoteView::mouseUp (const juce::MouseEvent& event)
{
    if (event.mouseWasDraggedSinceMouseDown() || shown < 0)
        return;

    if (event.mods.isPopupMenu())
    {
        const auto& e = library.getEntries()[(size_t) shown];
        const auto& p = library.getPerson (e);
        juce::PopupMenu menu;
        menu.addItem ("Copy", [this] { juce::SystemClipboard::copyTextToClipboard (getClipboardText()); });
        menu.addSeparator();
        menu.addItem ("Read the source", e.source.url.isNotEmpty(), false, [this] { openLink (Hover::source); });
        menu.addItem (p.name + (p.imdb.contains ("imdb.com") ? " on IMDb" : " (profile)"), p.imdb.isNotEmpty(), false,
                      [this] { openLink (Hover::name); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
        return;
    }

    const auto h = linkAt (event.position);
    if (h != Hover::none)
        openLink (h);
}

juce::String QuoteView::getTooltip()
{
    if (shown < 0)
        return {};
    const auto& e = library.getEntries()[(size_t) shown];
    if (hover == Hover::source)
        return e.source.title.isNotEmpty() ? e.source.title : e.source.url;
    if (hover == Hover::name)
    {
        const auto& p = library.getPerson (e);
        return p.name + (p.imdb.contains ("imdb.com") ? " on IMDb" : " (profile)");
    }
    return {};
}

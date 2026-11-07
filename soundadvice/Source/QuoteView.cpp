#include "QuoteView.h"

namespace
{
    constexpr float fadeOutSeconds = 0.16f;
    constexpr float fadeInSeconds = 0.34f;
    constexpr float maxBodySize = 31.0f;
    constexpr float minBodySize = 13.0f;
    constexpr float maxMeasure = 600.0f;   // longest comfortable line, in pixels

    const juce::String openQuote  = juce::String::fromUTF8 ("\xe2\x80\x9c");
    const juce::String closeQuote = juce::String::fromUTF8 ("\xe2\x80\x9d");
    const juce::String middleDot  = juce::String::fromUTF8 (" \xc2\xb7 ");

    float easeOut (float t) { return 1.0f - std::pow (1.0f - juce::jlimit (0.0f, 1.0f, t), 3.0f); }
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
    if (! animate || shown < 0 || ! isShowing())
    {
        shown = entryIndex;
        incoming = -1;
        phase = 2.0f;
        rebuildLayout();
        repaint();
        return;
    }

    incoming = entryIndex;
    if (phase >= 1.0f)
        phase = phase >= 2.0f ? 0.0f : 2.0f - phase;   // mid-fade-in: fade back out from where we are
    lastTick = juce::Time::getMillisecondCounterHiRes();
}

juce::String QuoteView::getClipboardText() const
{
    if (! juce::isPositiveAndBelow (shown, (int) library.getEntries().size()))
        return {};
    const auto& e = library.getEntries()[(size_t) shown];
    const auto& p = library.getPerson (e);
    auto text = e.verbatim ? openQuote + e.text + closeQuote : e.text;
    text << "\n— " << p.name;
    if (p.role.isNotEmpty())
        text << ", " << p.role;
    if (e.source.url.isNotEmpty())
        text << "\n" << e.source.url;
    return text;
}

void QuoteView::tick()
{
    if (phase >= 2.0f)
        return;

    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto dt = (float) juce::jlimit (0.0, 0.1, (now - lastTick) / 1000.0);
    lastTick = now;

    if (phase < 1.0f)
    {
        phase += dt / fadeOutSeconds;
        if (phase >= 1.0f)
        {
            phase = 1.0f;
            if (incoming >= 0)
            {
                shown = incoming;
                incoming = -1;
                rebuildLayout();
            }
        }
    }
    else
    {
        phase = juce::jmin (2.0f, phase + dt / fadeInSeconds);
    }
    repaint();
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

    layout.label = e.verbatim ? juce::String() : juce::String ("FIELD NOTE");
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
}

//==============================================================================
void QuoteView::paint (juce::Graphics& g)
{
    if (shown < 0)
        return;

    float alpha = 1.0f, drift = 0.0f;
    if (phase < 1.0f)
    {
        alpha = 1.0f - easeOut (phase);
        drift = -3.0f * easeOut (phase);
    }
    else if (phase < 2.0f)
    {
        const auto t = easeOut (phase - 1.0f);
        alpha = t;
        drift = 5.0f * (1.0f - t);
    }

    paintEntry (g, alpha, drift);
}

void QuoteView::paintEntry (juce::Graphics& g, float alpha, float drift)
{
    if (alpha <= 0.001f)
        return;

    auto withAlpha = [alpha] (juce::Colour c) { return c.withMultipliedAlpha (alpha); };

    if (layout.label.isNotEmpty())
    {
        g.setColour (withAlpha (palette.accent));
        g.setFont (Theme::caps (10.5f, 0.32f));
        g.drawText (layout.label, layout.labelArea.translated (0.0f, drift), juce::Justification::centredTop, false);
    }

    {
        juce::Graphics::ScopedSaveState save (g);
        g.setOpacity (alpha);
        layout.body.draw (g, layout.bodyArea.translated (0.0f, drift));
    }

    g.setColour (withAlpha (palette.accent.withMultipliedAlpha (0.7f)));
    g.fillRect (layout.ruleArea);

    const auto nameFont = Theme::caps (13.5f, 0.2f);
    g.setFont (nameFont);
    g.setColour (withAlpha (hover == Hover::name ? palette.accent : palette.ink));
    g.drawText (layout.name, layout.nameArea, juce::Justification::centred, true);
    if (hover == Hover::name)
    {
        const auto w = juce::GlyphArrangement::getStringWidth (nameFont, layout.name);
        g.fillRect (juce::Rectangle<float> (w, 0.75f).withCentre ({ layout.nameArea.getCentreX(), layout.nameArea.getBottom() - 1.0f }));
    }

    g.setFont (Theme::italic (14.5f));
    g.setColour (withAlpha (palette.muted));
    g.drawFittedText (layout.role, layout.roleArea.toNearestInt(), juce::Justification::centred, 1, 0.85f);

    if (layout.source.isNotEmpty())
    {
        const auto sourceFont = Theme::caps (10.5f, 0.16f);
        g.setFont (sourceFont);
        const bool hasLink = shown >= 0 && library.getEntries()[(size_t) shown].source.url.isNotEmpty();
        g.setColour (withAlpha (hover == Hover::source ? palette.accent : palette.muted.withMultipliedAlpha (0.85f)));
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
        menu.addItem (p.name + " on IMDb", p.imdb.isNotEmpty(), false, [this] { openLink (Hover::name); });
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
        return library.getPerson (e).name + " on IMDb";
    return {};
}

#include "PluginEditor.h"

namespace
{
    constexpr int defaultWidth = 560, defaultHeight = 360;
    constexpr int gutter = 24;

    float easeOut (float t) { return 1.0f - std::pow (1.0f - juce::jlimit (0.0f, 1.0f, t), 3.0f); }

    juce::Path chevron (juce::Rectangle<float> r, bool pointsLeft)
    {
        juce::Path p;
        const auto cx = r.getCentreX(), cy = r.getCentreY();
        const auto w = r.getWidth() * 0.22f, h = r.getHeight() * 0.36f;
        const auto dir = pointsLeft ? 1.0f : -1.0f;
        p.startNewSubPath (cx + w * dir, cy - h);
        p.lineTo (cx - w * dir, cy);
        p.lineTo (cx + w * dir, cy + h);
        return p;
    }
}

//==============================================================================
void SoundAdviceLookAndFeel::setPalette (const Theme::Palette& p)
{
    palette = p;
    setColour (juce::PopupMenu::backgroundColourId, p.background);
    setColour (juce::PopupMenu::textColourId, p.ink);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, p.hairline.withAlpha (0.5f));
    setColour (juce::PopupMenu::highlightedTextColourId, p.accent);
    setColour (juce::TooltipWindow::backgroundColourId, p.background);
    setColour (juce::TooltipWindow::textColourId, p.ink);
    setColour (juce::TooltipWindow::outlineColourId, p.hairline);
}

void SoundAdviceLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (palette.background);
    g.setColour (palette.hairline);
    g.drawRect (0, 0, width, height, 1);
}

void SoundAdviceLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                                                bool isActive, bool isHighlighted, bool isTicked, bool,
                                                const juce::String& text, const juce::String& shortcutKeyText,
                                                const juce::Drawable*, const juce::Colour*)
{
    if (isSeparator)
    {
        g.setColour (palette.hairline);
        g.fillRect (area.reduced (14, 0).withSizeKeepingCentre (area.getWidth() - 28, 1));
        return;
    }

    auto r = area.reduced (16, 0);
    if (isHighlighted && isActive)
    {
        g.setColour (palette.hairline.withAlpha (0.45f));
        g.fillRect (area.reduced (4, 1));
    }

    g.setColour (! isActive ? palette.faint : (isHighlighted || isTicked) ? palette.accent : palette.ink);
    g.setFont (isTicked ? Theme::italic (17.0f) : Theme::text (17.0f));
    g.drawText (text, r, juce::Justification::centredLeft, true);

    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour (palette.muted);
        g.setFont (Theme::caps (10.5f, 0.12f));
        g.drawText (shortcutKeyText, r, juce::Justification::centredRight, false);
    }
}

void SoundAdviceLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int,
                                                        int& idealWidth, int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth = 60;
        idealHeight = 9;
        return;
    }
    idealHeight = 25;
    idealWidth = (int) juce::GlyphArrangement::getStringWidth (Theme::text (17.0f), text) + 96;
}

juce::Rectangle<int> SoundAdviceLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                                               juce::Rectangle<int> parentArea)
{
    const auto w = (int) juce::GlyphArrangement::getStringWidth (Theme::italic (15.0f), tipText) + 20;
    const auto h = 24;
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 16,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 8) : screenPos.y + 14, w, h)
        .constrainedWithin (parentArea);
}

void SoundAdviceLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto r = juce::Rectangle<float> ((float) width, (float) height);
    g.setColour (palette.background);
    g.fillRect (r);
    g.setColour (palette.hairline);
    g.drawRect (r, 1.0f);
    g.setColour (palette.ink);
    g.setFont (Theme::italic (15.0f));
    g.drawText (text, r.reduced (10.0f, 0.0f), juce::Justification::centred, true);
}

void SoundAdviceLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
{
    // Two short strokes tucked inside the frame corner.
    g.setColour ((isMouseOver || isMouseDragging) ? palette.accent : palette.faint);
    const auto fw = (float) w, fh = (float) h;
    for (int i = 1; i <= 2; ++i)
    {
        const auto o = 5.0f * (float) i + 4.0f;
        g.drawLine (fw - o, fh - 8.0f, fw - 8.0f, fh - o, 1.0f);
    }
}

//==============================================================================
GlyphButton::GlyphButton (Glyph g) : juce::Button ({}), glyph (g)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void GlyphButton::roll()
{
    spin = 0.0f;
    lastTick = juce::Time::getMillisecondCounterHiRes();
}

void GlyphButton::tick()
{
    if (spin >= 1.0f)
        return;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto before = spin;
    spin = juce::jmin (1.0f, spin + (float) ((now - lastTick) / 1000.0) / 0.42f);
    lastTick = now;

    // A few faces flash past while it tumbles, then it lands.
    if ((int) (before * 5.0f) != (int) (spin * 5.0f) || spin >= 1.0f)
    {
        int next;
        do { next = 1 + random.nextInt (6); } while (next == face);
        face = next;
    }
    repaint();
}

void GlyphButton::drawDie (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    const auto angle = spin < 1.0f ? easeOut (spin) * juce::MathConstants<float>::halfPi * 2.0f : 0.0f;
    const auto lift = spin < 1.0f ? std::sin (spin * juce::MathConstants<float>::pi) * 2.5f : 0.0f;

    juce::Graphics::ScopedSaveState save (g);
    g.addTransform (juce::AffineTransform::rotation (angle, r.getCentreX(), r.getCentreY())
                        .translated (0.0f, -lift));

    const auto box = r.withSizeKeepingCentre (19.0f, 19.0f);
    g.setColour (c);
    g.drawRoundedRectangle (box, 4.0f, 1.25f);

    const auto pip = 1.75f;
    const auto inset = box.reduced (5.0f);
    auto dot = [&] (float fx, float fy)
    {
        g.fillEllipse (juce::Rectangle<float> (pip * 2.0f, pip * 2.0f)
                           .withCentre ({ inset.getX() + inset.getWidth() * fx, inset.getY() + inset.getHeight() * fy }));
    };

    switch (face)
    {
        case 1: dot (0.5f, 0.5f); break;
        case 2: dot (0, 0); dot (1, 1); break;
        case 3: dot (0, 0); dot (0.5f, 0.5f); dot (1, 1); break;
        case 4: dot (0, 0); dot (1, 0); dot (0, 1); dot (1, 1); break;
        case 5: dot (0, 0); dot (1, 0); dot (0.5f, 0.5f); dot (0, 1); dot (1, 1); break;
        default: dot (0, 0); dot (1, 0); dot (0, 0.5f); dot (1, 0.5f); dot (0, 1); dot (1, 1); break;
    }
}

void GlyphButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat();
    auto c = ! isEnabled() ? palette.faint.withMultipliedAlpha (0.6f)
           : highlighted   ? palette.accent
                           : palette.muted;
    if (down)
        r = r.reduced (0.5f);

    switch (glyph)
    {
        case Glyph::die:
            drawDie (g, r, isEnabled() && ! highlighted ? palette.ink.withMultipliedAlpha (0.8f) : c);
            break;

        case Glyph::back:
            g.setColour (c);
            g.strokePath (chevron (r.withSizeKeepingCentre (18.0f, 18.0f), true),
                          juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;

        case Glyph::moon:
        {
            const auto disc = r.withSizeKeepingCentre (13.0f, 13.0f);
            juce::Path crescent;
            crescent.addEllipse (disc);
            juce::Path bite;
            bite.addEllipse (disc.translated (4.5f, -3.5f));
            // Even-odd fill of the disc minus the bite, clipped to the disc.
            g.saveState();
            g.reduceClipRegion (crescent);
            juce::Path shape;
            shape.addRectangle (disc.expanded (2.0f));
            shape.addPath (bite);
            shape.setUsingNonZeroWinding (false);
            g.setColour (c);
            g.fillPath (shape);
            g.restoreState();
            break;
        }

        case Glyph::sun:
        {
            const auto centre = r.getCentre();
            g.setColour (c);
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (centre));
            for (int i = 0; i < 8; ++i)
            {
                const auto a = (float) i * juce::MathConstants<float>::twoPi / 8.0f;
                const auto dir = juce::Point<float> (std::cos (a), std::sin (a));
                g.drawLine ({ centre + dir * 6.5f, centre + dir * 8.75f }, 1.2f);
            }
            break;
        }
    }
}

//==============================================================================
CategoryBar::CategoryBar (const QuoteLibrary& lib) : library (lib) {}

void CategoryBar::setCategory (int index)
{
    category = index;
    repaint();
}

juce::Rectangle<float> CategoryBar::nameBounds() const
{
    return getLocalBounds().toFloat().reduced (30.0f, 0.0f);
}

CategoryBar::Zone CategoryBar::zoneAt (juce::Point<float> pos) const
{
    const auto r = getLocalBounds().toFloat();
    if (pos.x < 30.0f)
        return Zone::prev;
    if (pos.x > r.getRight() - 30.0f)
        return Zone::next;

    const auto& name = library.getCategories()[(size_t) category].name;
    const auto w = juce::GlyphArrangement::getStringWidth (Theme::caps (12.5f, 0.24f), name.toUpperCase()) + 20.0f;
    return nameBounds().withSizeKeepingCentre (w, r.getHeight()).contains (pos) ? Zone::name : Zone::none;
}

void CategoryBar::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    const auto stroke = juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    g.setColour (hover == Zone::prev ? palette.accent : palette.faint);
    g.strokePath (chevron (r.withWidth (30.0f).withSizeKeepingCentre (14.0f, 14.0f), true), stroke);
    g.setColour (hover == Zone::next ? palette.accent : palette.faint);
    g.strokePath (chevron (r.withTrimmedLeft (r.getWidth() - 30.0f).withSizeKeepingCentre (14.0f, 14.0f), false), stroke);

    const auto name = library.getCategories()[(size_t) category].name.toUpperCase();
    g.setColour (hover == Zone::name ? palette.ink : palette.accent);
    g.setFont (Theme::caps (12.5f, 0.24f));
    g.drawFittedText (name, nameBounds().toNearestInt(), juce::Justification::centred, 1, 0.8f);
}

void CategoryBar::mouseMove (const juce::MouseEvent& e)
{
    const auto z = zoneAt (e.position);
    if (z != hover)
    {
        hover = z;
        setMouseCursor (z == Zone::none ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);
        repaint();
    }
}

void CategoryBar::mouseExit (const juce::MouseEvent&)
{
    hover = Zone::none;
    repaint();
}

void CategoryBar::step (int delta)
{
    const auto n = (int) library.getCategories().size();
    if (onChoose)
        onChoose ((category + delta + n) % n);
}

void CategoryBar::mouseUp (const juce::MouseEvent& e)
{
    switch (zoneAt (e.position))
    {
        case Zone::prev: step (-1); break;
        case Zone::next: step (1); break;
        case Zone::name: showMenu(); break;
        case Zone::none: break;
    }
}

void CategoryBar::showMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel (&getLookAndFeel());
    const auto& cats = library.getCategories();
    for (size_t i = 0; i < cats.size(); ++i)
    {
        juce::PopupMenu::Item item (cats[i].name);
        item.setTicked ((int) i == category);
        item.shortcutKeyDescription = juce::String ((int) cats[i].entries.size());
        item.setAction ([this, i] { if (onChoose) onChoose ((int) i); });
        menu.addItem (std::move (item));
        if (i == 0)
            menu.addSeparator();
    }
    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (this)
                            .withMinimumWidth (getWidth() - 60)
                            .withPreferredPopupDirection (juce::PopupMenu::Options::PopupDirection::downwards));
}

//==============================================================================
SoundAdviceEditor::SoundAdviceEditor (SoundAdviceProcessor& p)
    : AudioProcessorEditor (p), owner (p), library (p.getLibrary()), quote (library), categories (library)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (quote);
    addAndMakeVisible (categories);
    addAndMakeVisible (dice);
    addAndMakeVisible (backButton);
    addAndMakeVisible (nightButton);

    dice.setTooltip ("Another (Return)");
    backButton.setTooltip ("The one before");
    categories.onChoose = [this] (int index) { chooseCategory (index); };
    dice.onClick = [this] { rollDice(); };
    backButton.onClick = [this] { goBack(); };
    nightButton.onClick = [this]
    {
        owner.setNightMode (! owner.isNightMode());
        applyTheme();
    };

    categories.setCategory (owner.getCategory());
    quote.showEntry (owner.getCurrentEntry(), false);

    setWantsKeyboardFocus (true);
    setResizable (true, true);
    setResizeLimits (440, 300, 960, 640);
    setSize (defaultWidth, defaultHeight);
    applyTheme();
    refreshButtons();
}

SoundAdviceEditor::~SoundAdviceEditor()
{
    setLookAndFeel (nullptr);
}

void SoundAdviceEditor::applyTheme()
{
    const bool night = owner.isNightMode();
    palette = Theme::palette (night);
    lookAndFeel.setPalette (palette);
    quote.setPalette (palette);
    categories.setPalette (palette);
    dice.setPalette (palette);
    backButton.setPalette (palette);
    nightButton.setPalette (palette);
    nightButton.setGlyph (night ? GlyphButton::Glyph::sun : GlyphButton::Glyph::moon);
    nightButton.setTooltip (night ? "Paper" : "Night");
    rebuildGrain();
    repaint();
}

void SoundAdviceEditor::chooseCategory (int index)
{
    const auto entry = owner.selectCategory (index);
    categories.setCategory (owner.getCategory());
    quote.showEntry (entry, true);
    dice.roll();
    refreshButtons();
}

void SoundAdviceEditor::rollDice()
{
    quote.showEntry (owner.roll(), true);
    dice.roll();
    refreshButtons();
}

void SoundAdviceEditor::goBack()
{
    if (! owner.canGoBack())
        return;
    quote.showEntry (owner.back(), true);
    refreshButtons();
}

void SoundAdviceEditor::refreshButtons()
{
    backButton.setEnabled (owner.canGoBack());
}

bool SoundAdviceEditor::keyPressed (const juce::KeyPress& key)
{
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::returnKey || code == juce::KeyPress::rightKey)
        rollDice();
    else if (code == juce::KeyPress::leftKey || code == juce::KeyPress::backspaceKey)
        goBack();
    else if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        const auto n = (int) library.getCategories().size();
        chooseCategory ((owner.getCategory() + (code == juce::KeyPress::downKey ? 1 : -1) + n) % n);
    }
    else if (key == juce::KeyPress ('c', juce::ModifierKeys::commandModifier, 0))
        juce::SystemClipboard::copyTextToClipboard (quote.getClipboardText());
    else
        return false;
    return true;
}

//==============================================================================
void SoundAdviceEditor::rebuildGrain()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    // A faint paper grain, fixed per size so it never shimmers.
    grain = juce::Image (juce::Image::ARGB, getWidth(), getHeight(), true);
    juce::Random rng (1234);
    const auto speck = owner.isNightMode() ? juce::Colours::white : juce::Colour (0xff5a4a3a);
    const auto strength = owner.isNightMode() ? 0.018f : 0.035f;
    juce::Image::BitmapData data (grain, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < data.height; ++y)
        for (int x = 0; x < data.width; ++x)
            data.setPixelColour (x, y, speck.withAlpha (rng.nextFloat() * strength));
}

void SoundAdviceEditor::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (palette.background, r.getCentreX(), r.getHeight() * 0.42f,
                                             palette.backgroundEdge, 0.0f, r.getBottom() + r.getHeight() * 0.2f, true));
    g.fillAll();
    if (grain.isValid())
        g.drawImageAt (grain, 0, 0);

    // A single hairline frame, inset, like a printed card.
    g.setColour (palette.hairline.withMultipliedAlpha (0.8f));
    g.drawRect (r.reduced (8.0f), 1.0f);

    if (getWidth() >= 520)
    {
        g.setColour (palette.muted.withMultipliedAlpha (0.9f));
        g.setFont (Theme::caps (11.0f, 0.3f));
        g.drawText ("SOUND ADVICE", juce::Rectangle<float> ((float) gutter, 18.0f, 140.0f, 26.0f),
                    juce::Justification::centredLeft, false);
    }
}

void SoundAdviceEditor::resized()
{
    auto r = getLocalBounds().reduced (gutter, 0);

    auto header = r.removeFromTop (44).withTrimmedTop (16);
    nightButton.setBounds (header.removeFromRight (26).withSizeKeepingCentre (26, 26));
    const auto barWidth = juce::jmin (300, getWidth() - (getWidth() >= 520 ? 2 * 136 : 2 * 50));
    categories.setBounds (juce::Rectangle<int> (barWidth, 26).withCentre ({ getWidth() / 2, header.getCentreY() }));

    r.removeFromBottom (18);
    quote.setBounds (r.withTrimmedTop (14));

    // The corner buttons line up with the source line at the foot of the page.
    const auto footY = r.getBottom() - 10;
    backButton.setBounds (juce::Rectangle<int> (30, 30).withCentre ({ r.getX() + 15, footY }));
    dice.setBounds (juce::Rectangle<int> (34, 34).withCentre ({ r.getRight() - 17, footY }));

    rebuildGrain();
}

#include "RoarschachLookAndFeel.h"

namespace
{
    using namespace Theme::Colours;
    namespace Type = Theme::Type;
    namespace Metrics = Theme::Metrics;

    // Popups dropping from the editor follow its scale, but never get smaller than at its default size.
    constexpr float c_minimumPopupScale = 0.8f;

    constexpr float c_menuRadius = Metrics::radius;   // design-size menu metrics, scaled by menuScale()
    constexpr float c_menuBorder = 5.0f;
    constexpr float c_menuTickColumn = 22.0f;
    constexpr float c_menuTextInset = 24.0f;
    constexpr float c_menuArrowColumn = 20.0f;
    constexpr float c_menuHeaderHeight = 28.0f;
    constexpr float c_menuSeparatorHeight = 9.0f;

    constexpr float c_dialogRadius = Metrics::radius;
    constexpr float c_dialogHeaderHeight = 36.0f;
    constexpr int c_dialogButtonSlot = 40;        // JUCE lays out this height; the chip is c_dialogButtonHeight
    constexpr float c_dialogButtonHeight = 28.0f;
    constexpr int c_dialogButtonMinWidth = 96;

    constexpr float c_tipRadius = Metrics::radius;
    constexpr float c_tipPadX = 11.0f, c_tipPadY = 7.0f;
    constexpr float c_tipMaxWidth = 440.0f;

    const juce::Identifier c_focusRingsProperty { "roarschachFocusRings" };
    const juce::Identifier c_capitalsProperty { "roarschachCapitals" };
    const juce::Identifier c_valueFontProperty { "roarschachValueFont" };
    const juce::Identifier c_knobStyleProperty { "roarschachKnobStyle" };
    const juce::Identifier c_quietProperty { "roarschachQuiet" };
    const juce::Identifier c_displayFaceProperty { "roarschachDisplayFace" };
    const juce::Identifier c_captionLiveProperty { "roarschachCaptionLive" };
    const juce::Identifier c_menuChipProperty { "roarschachMenuChip" };   // 1: a chip with a caret, 2: ... of an Off module

    constexpr float c_caretColumn = 11.0f;   // a menu chip's caret, at its right end

    // A colour that looks opaque but makes JUCE create a transparent window (menus), so the rounded corners
    // show what is behind them. The body itself is drawn with full alpha.
    juce::Colour nearlyOpaque(juce::Colour c) { return c.withAlpha(static_cast<juce::uint8>(0xfe)); }

    // Width of a line of text as drawn: getStringWidth() skips ligature placeholders, so text ending in a
    // ligature (the sans joins "fi", "fl", ...) would measure short.
    float lineWidth(const juce::Font& font, const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth(font, text + " ") - juce::GlyphArrangement::getStringWidth(font, " ");
    }

    // One line, never squashed: too long gets "...", keeping a trailing " *" (the edited marker).
    juce::String fitToWidth(const juce::Font& font, const juce::String& text, float width)
    {
        width -= 1.0f; // Graphics::drawText() wraps away (drops) a word that overruns its area by any amount

        if (lineWidth(font, text) <= width)
            return text;

        const juce::String suffix = text.endsWith(" *") ? " *" : "";
        auto stem = text.dropLastCharacters(suffix.length());
        while (stem.isNotEmpty() && lineWidth(font, stem.trimEnd() + "..." + suffix) > width)
            stem = stem.dropLastCharacters(1);
        return stem.trimEnd() + "..." + suffix;
    }

    // A ">" chevron, rotated by angle.
    void strokeChevron(juce::Graphics& g, juce::Point<float> centre, float size, float angle, float thickness)
    {
        juce::Path p;
        p.startNewSubPath(-size * 0.5f, -size);
        p.lineTo(size * 0.5f, 0.0f);
        p.lineTo(-size * 0.5f, size);
        p.applyTransform(juce::AffineTransform::rotation(angle).translated(centre));
        g.strokePath(p, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Scale of a component relative to its window: for a control in the editor, the editor's content scale.
    float scaleWithinWindow(const juce::Component& c)
    {
        // Relative to the plugin editor, not to the top-level window: a VST3 host on a scaled Windows
        // display applies its scale as a transform on the AudioProcessorEditor, and that must not count
        // towards the user's own size (otherwise the "never smaller than at default size" floor breaks).
        const juce::Component* reference = c.findParentComponentOfClass<juce::AudioProcessorEditor>();
        if (reference == nullptr)
            reference = c.getTopLevelComponent();

        const float own = juce::Component::getApproximateScaleFactorForComponent(&c);
        const float base = juce::Component::getApproximateScaleFactorForComponent(reference);
        return base > 0.0f && own > 0.0f ? own / base : 1.0f;
    }

    float legibilityFactor(const juce::Component& target)
    {
        const float s = scaleWithinWindow(target);
        return s < c_minimumPopupScale ? c_minimumPopupScale / s : 1.0f;
    }

    // Every menu metric follows the menu's row height, so a submenu (which inherits it) matches its parent.
    float menuScale(const juce::PopupMenu::Options& options)
    {
        const int h = options.getStandardItemHeight();
        return h > 0 ? static_cast<float>(h) / static_cast<float>(RoarschachLookAndFeel::c_menuItemHeight) : 1.0f;
    }

    bool isInAlert(const juce::Component& c)
    {
        return dynamic_cast<const juce::AlertWindow*>(c.getParentComponent()) != nullptr;
    }

    // A text button with a transparent fill is a text link (the About window's website and e-mail).
    bool isLinkStyle(const juce::Button& b)
    {
        return b.findColour(juce::TextButton::buttonColourId).isTransparent();
    }

    juce::Rectangle<float> pillBounds(const juce::Button& b)
    {
        auto r = b.getLocalBounds().toFloat();
        if (isInAlert(b))
            r = r.withSizeKeepingCentre(r.getWidth(), std::min(r.getHeight(), c_dialogButtonHeight));
        else if (static_cast<int>(b.getProperties().getWithDefault(c_menuChipProperty, 0)) != 0)
            r = r.withSizeKeepingCentre(r.getWidth(), std::min(r.getHeight(), static_cast<float>(Metrics::chipHeight)));   // (the button is its hit target)
        return r.reduced(0.5f);
    }

    float fieldRadius(int height) { return std::min(Metrics::radius, static_cast<float>(height) * 0.25f); }

    bool knobIsLive(RoarschachLookAndFeel::KnobStyle s)
    {
        return s == RoarschachLookAndFeel::KnobStyle::full || s == RoarschachLookAndFeel::KnobStyle::weak;
    }

    //==============================================================================
    // Tab in a window shows its focus rings, a click in it hides them (stateless: the state is a property
    // of the window, so one instance serves every window).
    struct FocusRingTracker final : public juce::KeyListener, public juce::MouseListener
    {
        bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override
        {
            if (origin != nullptr && key.isKeyCode(juce::KeyPress::tabKey))
                setVisible(*origin, true);
            return false; // never consumed: Tab still moves the focus
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            if (e.eventComponent != nullptr)
                setVisible(*e.eventComponent, false);
        }

        static void setVisible(juce::Component& c, bool visible)
        {
            auto* window = c.getTopLevelComponent();
            if (static_cast<bool>(window->getProperties().getWithDefault(c_focusRingsProperty, false)) == visible)
                return;

            window->getProperties().set(c_focusRingsProperty, visible);
            if (auto* focused = juce::Component::getCurrentlyFocusedComponent())
                if (window->isParentOf(focused) || focused == window)
                    focused->repaint();
        }
    };

    FocusRingTracker& focusRingTracker()
    {
        static FocusRingTracker tracker; // stateless, so it can outlive every window it listens to
        return tracker;
    }

    juce::LookAndFeel_V4::ColourScheme themeScheme()
    {
        // Anything JUCE draws that this class does not override still comes out in the theme's colours.
        return { paper, sheet, sheet, rule, ink, accent, ink, selection, ink };
    }
}

//==============================================================================
RoarschachLookAndFeel::RoarschachLookAndFeel(const Theme::Fonts& fonts)
    : juce::LookAndFeel_V4(themeScheme())
    , m_fonts(fonts)
{
    if (fonts.regular != nullptr)
        setDefaultSansSerifTypeface(fonts.regular);

    refreshColours();
}

void RoarschachLookAndFeel::refreshColours()
{
    const auto transparent = juce::Colours::transparentBlack;
    setColourScheme(themeScheme());

    setColour(juce::ResizableWindow::backgroundColourId, paper);
    setColour(juce::DocumentWindow::textColourId, ink);
    setColour(juce::Label::textColourId, ink);

    setColour(juce::Slider::textBoxTextColourId, ink);
    setColour(juce::Slider::textBoxOutlineColourId, transparent);
    setColour(juce::Slider::textBoxBackgroundColourId, transparent);
    setColour(juce::Slider::textBoxHighlightColourId, selection);

    setColour(juce::ComboBox::backgroundColourId, sheet);
    setColour(juce::ComboBox::outlineColourId, rule);
    setColour(juce::ComboBox::textColourId, ink);
    setColour(juce::ComboBox::arrowColourId, inkDim);
    setColour(juce::ComboBox::focusedOutlineColourId, focus);

    setColour(juce::PopupMenu::backgroundColourId, nearlyOpaque(sheet));
    setColour(juce::PopupMenu::textColourId, ink);
    setColour(juce::PopupMenu::headerTextColourId, inkDim);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, selection);
    setColour(juce::PopupMenu::highlightedTextColourId, ink);

    setColour(juce::TextButton::buttonColourId, sheet);
    setColour(juce::TextButton::buttonOnColourId, ink);
    setColour(juce::TextButton::textColourOffId, ink);
    setColour(juce::TextButton::textColourOnId, sheet);

    setColour(juce::HyperlinkButton::textColourId, inkDim);

    setColour(juce::ToggleButton::tickColourId, accent);
    setColour(juce::ToggleButton::tickDisabledColourId, inkFaint);
    setColour(juce::ToggleButton::textColourId, inkDim);

    setColour(juce::AlertWindow::backgroundColourId, sheet);
    setColour(juce::AlertWindow::textColourId, ink);
    setColour(juce::AlertWindow::outlineColourId, rule);

    setColour(juce::TextEditor::backgroundColourId, sheet);
    setColour(juce::TextEditor::textColourId, ink);
    setColour(juce::TextEditor::outlineColourId, rule);
    setColour(juce::TextEditor::focusedOutlineColourId, focus);
    setColour(juce::TextEditor::highlightColourId, selection);
    setColour(juce::TextEditor::highlightedTextColourId, ink);
    setColour(juce::TextEditor::shadowColourId, transparent);
    setColour(juce::CaretComponent::caretColourId, ink);

    setColour(juce::ScrollBar::thumbColourId, ink.withAlpha(0.35f));
    setColour(juce::ScrollBar::trackColourId, transparent);
    setColour(juce::ScrollBar::backgroundColourId, transparent);

    setColour(juce::TooltipWindow::backgroundColourId, sheet);
    setColour(juce::TooltipWindow::textColourId, ink);
    setColour(juce::TooltipWindow::outlineColourId, rule);
}

//==============================================================================
void RoarschachLookAndFeel::setKnobStyle(juce::Slider& slider, KnobStyle style)
{
    if (getKnobStyle(slider) == style)
        return;
    slider.getProperties().set(c_knobStyleProperty, static_cast<int>(style));
    slider.repaint();
    for (auto* child : slider.getChildren())
        child->repaint();
}

RoarschachLookAndFeel::KnobStyle RoarschachLookAndFeel::getKnobStyle(const juce::Slider& slider)
{
    return static_cast<KnobStyle>(static_cast<int>(slider.getProperties().getWithDefault(c_knobStyleProperty, 0)));
}

void RoarschachLookAndFeel::setCaptionLive(juce::Label& label, bool live)
{
    const juce::var v(live);
    if (label.getProperties()[c_captionLiveProperty] == v)
        return;
    label.getProperties().set(c_captionLiveProperty, v);
    label.repaint();
}

bool RoarschachLookAndFeel::isCaptionLive(const juce::Label& label)
{
    return static_cast<bool>(label.getProperties().getWithDefault(c_captionLiveProperty, true));
}

int RoarschachLookAndFeel::getMenuChip(const juce::Button& b)
{
    return static_cast<int>(b.getProperties().getWithDefault(c_menuChipProperty, 0));
}

void RoarschachLookAndFeel::setMenuChip(juce::Button& b, bool off)
{
    const juce::var v(off ? 2 : 1);
    if (b.getProperties()[c_menuChipProperty] == v)
        return;
    b.getProperties().set(c_menuChipProperty, v);
    b.repaint();
}

void RoarschachLookAndFeel::setQuiet(juce::Button& b)
{
    b.getProperties().set(c_quietProperty, true);
    b.repaint();
}

void RoarschachLookAndFeel::setDisplayFace(juce::Button& b)
{
    b.getProperties().set(c_displayFaceProperty, true);
    b.repaint();
}

void RoarschachLookAndFeel::strokeBrush(juce::Graphics& g, juce::Point<float> c, float radius, float from, float to, float width)
{
    // A filled outline: the inner and outer edges of the stroke, its width easing from 35 % at the
    // start to full at the end, closed by a round end (the brush lifting off). At most 40 points.
    const auto at = [&](float a, float r) { return c + juce::Point<float>(std::sin(a), -std::cos(a)) * r; };
    const float span = to - from;
    if (std::abs(span) < 1.0e-3f)
    {
        g.fillEllipse(juce::Rectangle<float>(width, width).withCentre(at(to, radius)));
        return;
    }
    const int n = juce::jlimit(6, 40, static_cast<int>(std::abs(span) * radius / 3.0f));
    const auto half = [&](int i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        return width * 0.5f * (0.35f + 0.65f * std::sqrt(t));
    };
    juce::Path p;
    for (int i = 0; i <= n; ++i)
    {
        const float a = from + span * static_cast<float>(i) / static_cast<float>(n);
        const auto pt = at(a, radius + half(i));
        if (i == 0)
            p.startNewSubPath(pt);
        else
            p.lineTo(pt);
    }
    for (int i = n; i >= 0; --i)
        p.lineTo(at(from + span * static_cast<float>(i) / static_cast<float>(n), radius - half(i)));
    p.closeSubPath();
    g.fillPath(p);
    g.fillEllipse(juce::Rectangle<float>(width, width).withCentre(at(to, radius)));
}

//==============================================================================
juce::PopupMenu::Options RoarschachLookAndFeel::menuOptionsFor(juce::Component& target, int minimumWidth)
{
    return juce::PopupMenu::Options().withTargetComponent(&target)
                                     .withMinimumWidth(minimumWidth)
                                     .withStandardItemHeight(juce::roundToInt(static_cast<float>(c_menuItemHeight) * legibilityFactor(target)));
}

void RoarschachLookAndFeel::showFocusRingsAfterTab(juce::Component& window)
{
    window.addKeyListener(&focusRingTracker());
    window.addMouseListener(&focusRingTracker(), true);
}

bool RoarschachLookAndFeel::shouldShowFocusRing(const juce::Component& c)
{
    if (!c.hasKeyboardFocus(false))
        return false;

    const auto* window = c.getTopLevelComponent();
    return static_cast<bool>(window->getProperties().getWithDefault(c_focusRingsProperty, false));
}

void RoarschachLookAndFeel::setLabelDrawnInCapitals(juce::Button& button)
{
    button.getProperties().set(c_capitalsProperty, true);
    button.repaint();
}

juce::Colour RoarschachLookAndFeel::accentFor(juce::MessageBoxIconType type)
{
    return type == juce::MessageBoxIconType::WarningIcon ? warn : accentInk;
}

//==============================================================================
// The mark
//==============================================================================
namespace
{
    /** The mark's shapes, designed on a 30 x 22 grid and mirrored about the vertical centre line, like a
        folded ink blot (every shape is built for the left half and reflected, so it stays exactly
        symmetric), placed in `area`. */
    struct MarkShapes
    {
        juce::Path head;    // the blot and its horns, with the maw and the eye slits cut out
        juce::Path blot, horns, maw, fangs, eyes;
        float stroke = 1.0f;

        explicit MarkShapes(juce::Rectangle<float> area)
        {
            const float sx = area.getWidth() / 30.0f, sy = area.getHeight() / 22.0f;
            stroke = std::min(sx, sy);
            const auto toArea = juce::AffineTransform::scale(sx, sy).translated(area.getX(), area.getY());
            const auto mirror = juce::AffineTransform::scale(-1.0f, 1.0f).translated(30.0f, 0.0f);
            const auto mirrored = [&](const juce::Path& half)
            {
                juce::Path both(half);
                both.addPath(half, mirror);
                both.applyTransform(toArea);
                return both;
            };

            // The blot: a head that bulges at the brow and the cheeks and tapers to a chin, with an inky drip.
            juce::Path blotHalf;
            blotHalf.startNewSubPath(15.0f, 0.6f);
            blotHalf.cubicTo(11.0f, 0.4f, 6.5f, 0.8f, 3.6f, 3.2f);   // brow
            blotHalf.cubicTo(1.2f, 5.0f, 0.2f, 7.6f, 1.6f, 9.4f);    // brow tip
            blotHalf.cubicTo(0.4f, 11.6f, 1.0f, 14.6f, 3.6f, 16.2f); // cheek
            blotHalf.cubicTo(6.0f, 17.8f, 8.6f, 19.4f, 11.2f, 20.2f); // jaw
            blotHalf.cubicTo(12.6f, 20.6f, 13.6f, 21.6f, 15.0f, 21.4f); // chin drip
            blotHalf.closeSubPath();

            // Horns: short, curling out of the brow.
            juce::Path hornHalf;
            hornHalf.startNewSubPath(8.4f, 1.4f);
            hornHalf.quadraticTo(4.6f, 2.0f, 1.6f, 1.0f);
            hornHalf.quadraticTo(2.6f, 3.0f, 4.4f, 4.6f);
            hornHalf.closeSubPath();

            // The maw: a wide open mouth, upper lip arched, lower lip sagging.
            juce::Path mawHalf;
            mawHalf.startNewSubPath(15.0f, 9.6f);
            mawHalf.cubicTo(11.5f, 9.4f, 7.4f, 9.6f, 4.6f, 11.4f);
            mawHalf.cubicTo(6.2f, 14.6f, 10.0f, 17.4f, 15.0f, 17.6f);
            mawHalf.closeSubPath();

            // Fangs: two long ones from the upper lip, one shorter from the lower lip, per side.
            juce::Path fangsHalf;
            fangsHalf.addTriangle(12.2f, 9.5f, 13.8f, 9.5f, 13.0f, 12.6f);
            fangsHalf.addTriangle(8.0f, 9.8f, 9.8f, 9.6f, 9.0f, 13.2f);
            fangsHalf.addTriangle(10.0f, 16.9f, 11.8f, 17.3f, 11.0f, 14.6f);

            // Eyes: slanted slits under the brow.
            juce::Path eyeHalf;
            eyeHalf.startNewSubPath(12.6f, 6.6f);
            eyeHalf.quadraticTo(9.8f, 4.6f, 7.0f, 5.0f);
            eyeHalf.quadraticTo(9.4f, 7.2f, 12.6f, 6.6f);
            eyeHalf.closeSubPath();

            blot = mirrored(blotHalf);
            horns = mirrored(hornHalf);
            maw = mirrored(mawHalf);
            fangs = mirrored(fangsHalf);
            eyes = mirrored(eyeHalf);

            // The head as one shape with real holes (even-odd), so whatever is behind the mark shows
            // through the maw and the eyes: nothing is painted in a background colour.
            head.setUsingNonZeroWinding(false);
            head.addPath(blot);
            head.addPath(maw);
            head.addPath(eyes);
        }
    };
}

void RoarschachLookAndFeel::drawMark(juce::Graphics& g, juce::Rectangle<float> area)
{
    // One ink, as the creature itself: the blot solid, the maw and the eye slits cut through it, the
    // fangs biting back in the same ink. Nothing behind it.
    const MarkShapes m(area);
    g.setColour(ink);
    g.fillPath(m.horns);
    g.fillPath(m.head);
    const juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(m.maw); // fangs never poke out of the mouth
    g.fillPath(m.fangs);
}

void RoarschachLookAndFeel::drawColourMark(juce::Graphics& g, juce::Rectangle<float> area)
{
    // The mark in colour (the About window only): ember fading to rose at the chin, a soft violet bleed
    // round its edge, the maw and the eyes cut through, the fangs in pale ink.
    const MarkShapes m(area);
    const juce::PathStrokeType bleed(1.6f * m.stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour(juce::Colour(Theme::Brand::markBleed).withAlpha(0.35f));
    g.strokePath(m.horns, bleed);
    g.strokePath(m.blot, bleed);
    g.setGradientFill(juce::ColourGradient(juce::Colour(Theme::Brand::markTop), area.getCentreX(), area.getY(),
                                           juce::Colour(Theme::Brand::markChin), area.getCentreX(), area.getBottom(), false));
    g.fillPath(m.horns);
    g.fillPath(m.head);
    const juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(m.maw);
    g.setColour(juce::Colour(Theme::Brand::markFangs));
    g.fillPath(m.fangs);
}

juce::Rectangle<float> RoarschachLookAndFeel::drawPanel(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title,
                                                        float headerHeight) const
{
    // A wash on the paper: no outline; the title in the display face with a short ink rule under it.
    g.setColour(wash);
    g.fillRoundedRectangle(r, Metrics::radius);

    auto headerRow = r.withHeight(headerHeight).reduced(14.0f, 0.0f);
    g.setFont(m_fonts.title(Type::menuHeader));
    g.setColour(ink);
    g.drawText(title, headerRow.translated(0.0f, 1.0f), juce::Justification::centredLeft, false);

    if (headerHeight < r.getHeight() - 1.0f) // a panel that is only a header row has no rule
    {
        g.setColour(accent);
        g.fillRect(juce::Rectangle<float>(r.getX() + 14.0f, std::round(r.getY() + headerHeight - 5.0f), 24.0f, 1.5f));
    }
    return headerRow;
}

//==============================================================================
// Knobs
//==============================================================================
void RoarschachLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                             float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
{
    // Ink on paper: a hairline track, the value as a brush stroke in the mood's dye that swells towards
    // the value, and a flat ink disc with a paper pointer. The state is drawn by shape, never by fading:
    //  - full: the disc solid, the brushed arc;
    //  - weak (does little): the disc solid, the arc a fine line;
    //  - unused: the disc an outline, the arc a fine line in the faint ink, the pointer in ink;
    //  - off (its module is Off): the disc an outline, no arc at all, the pointer in the faint ink.
    const auto style = getKnobStyle(slider);
    const bool hot = slider.isMouseOverOrDragging() && slider.isEnabled();

    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(4.0f);
    const float diameter = std::min(area.getWidth(), area.getHeight());
    const auto centre = area.getCentre();
    const float radius = diameter * 0.5f;
    const float brush = std::max(3.0f, radius * 0.16f);
    const float arcRadius = radius - brush * 0.5f;
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    juce::Path track;
    track.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(rule);
    g.strokePath(track, juce::PathStrokeType(Metrics::hairline, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // The value: bipolar parameters (dB gain) grow from the centre.
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0 && juce::approximatelyEqual(slider.getMinimum(), -slider.getMaximum());
    const float fromAngle = bipolar ? (rotaryStartAngle + rotaryEndAngle) * 0.5f : rotaryStartAngle;
    if (style == KnobStyle::full)
    {
        g.setColour(accent);
        strokeBrush(g, centre, arcRadius, fromAngle, angle, brush);
    }
    else if (style != KnobStyle::off)
    {
        juce::Path fine;
        fine.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, fromAngle, angle, true);
        g.setColour(style == KnobStyle::weak ? accent : inkFaint);
        g.strokePath(fine, juce::PathStrokeType(std::max(1.5f, brush * 0.4f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // The disc: flat ink (a little bleed under the mouse); for an unused or Off knob, its outline only.
    const bool solid = knobIsLive(style);
    const float bodyRadius = arcRadius - brush * 1.45f;
    const auto body = juce::Rectangle<float>(bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre(centre);
    if (!solid)
    {
        g.setColour(hot ? ink : inkFaint);
        g.drawEllipse(body.reduced(0.75f), 1.5f);
    }
    else
    {
        if (hot)
        {
            g.setColour(selection);
            g.fillEllipse(body.expanded(3.0f));
        }
        g.setColour(ink);
        g.fillEllipse(body);
    }

    // The pointer: from a third of the way out to the rim, so it reads at a glance at any size.
    const auto direction = juce::Point<float>(std::sin(angle), -std::cos(angle));
    const auto p1 = centre + direction * (bodyRadius * (solid ? 0.30f : 0.20f));
    const auto p2 = centre + direction * (bodyRadius * 0.86f);
    g.setColour(solid ? sheet : style == KnobStyle::unused ? ink : inkFaint);
    g.drawLine({ p1, p2 }, solid ? Metrics::pointer : 1.6f);

    if (shouldShowFocusRing(slider))
    {
        g.setColour(focus);
        g.drawEllipse(juce::Rectangle<float>(diameter + 2.0f, diameter + 2.0f).withCentre(centre), Metrics::focusRing);
    }
}

juce::Slider::SliderLayout RoarschachLookAndFeel::getSliderLayout(juce::Slider& slider)
{
    juce::Slider::SliderLayout layout;

    if (slider.getTextBoxPosition() == juce::Slider::NoTextBox)
    {
        layout.sliderBounds = slider.getLocalBounds();
        return layout;
    }

    auto bounds = slider.getLocalBounds();
    layout.textBoxBounds = bounds.removeFromBottom(18).withSizeKeepingCentre(std::min(bounds.getWidth(), 84), 18);
    bounds.removeFromBottom(2);
    layout.sliderBounds = bounds;
    return layout;
}

void RoarschachLookAndFeel::setValueFontHeight(juce::Slider& slider, float pointHeight)
{
    slider.getProperties().set(c_valueFontProperty, pointHeight);
}

juce::Label* RoarschachLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
    label->setFont(m_fonts.value(static_cast<float>(slider.getProperties().getWithDefault(c_valueFontProperty, Type::value))));
    label->setJustificationType(juce::Justification::centred);
    label->setColour(juce::Label::textColourId, ink);
    label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

    // While a value is typed: a raised paper field with an ink outline, the selection in the mood's dye,
    // the text centred like the value it replaces. Label::createEditorComponent() copies these to its
    // TextEditor (the colours are read again when the palette changes: see drawLabel).
    label->setColour(juce::Label::textWhenEditingColourId, ink);
    label->setColour(juce::Label::backgroundWhenEditingColourId, sheet);
    label->setColour(juce::Label::outlineWhenEditingColourId, focus);
    label->setColour(juce::TextEditor::highlightColourId, selection);
    label->setColour(juce::TextEditor::highlightedTextColourId, ink);
    label->setColour(juce::CaretComponent::caretColourId, ink);
    label->onEditorShow = [label]
    {
        if (auto* editor = label->getCurrentTextEditor())
        {
            editor->setJustification(juce::Justification::centred);
            editor->setIndents(2, 0);
        }
    };
    return label;
}

void RoarschachLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
{
    // A knob's value: ink while the knob is used, the faint rank (still 4.5:1 or more) when it is not or its
    // module is Off, whatever colours the box was built with (the palette moves with the mood).
    if (auto* slider = dynamic_cast<juce::Slider*>(label.getParentComponent()); slider != nullptr && !label.isBeingEdited())
    {
        g.setColour(knobIsLive(getKnobStyle(*slider)) ? ink : inkFaint);
        g.setFont(getLabelFont(label));
        g.drawFittedText(label.getText(), label.getLocalBounds(), label.getJustificationType(), 1, 1.0f);
        return;
    }
    if (const auto live = label.getProperties()[c_captionLiveProperty]; !live.isVoid() && !label.isBeingEdited())
    {
        g.setColour(static_cast<bool>(live) ? inkDim : inkFaint);
        g.setFont(getLabelFont(label));
        g.drawFittedText(label.getText(), label.getBorderSize().subtractedFrom(label.getLocalBounds()), label.getJustificationType(),
                         1, label.getMinimumHorizontalScale());
        return;
    }
    juce::LookAndFeel_V4::drawLabel(g, label);
}

juce::Font RoarschachLookAndFeel::getLabelFont(juce::Label& label)
{
    // Knob value boxes show values; every other editor label is a caption. A label whose code set one of the
    // theme's fonts keeps it (e.g. the About window's status line); JUCE's default label font never shows.
    if (auto* slider = dynamic_cast<juce::Slider*>(label.getParentComponent()))
        return m_fonts.value(static_cast<float>(slider->getProperties().getWithDefault(c_valueFontProperty, Type::value)));

    if (dynamic_cast<juce::ComboBox*>(label.getParentComponent()) == nullptr && m_fonts.isThemeFont(label.getFont()))
        return label.getFont();

    return m_fonts.caption(Type::caption);
}

//==============================================================================
// Toggle buttons (On lamps)
//==============================================================================
void RoarschachLookAndFeel::drawLed(juce::Graphics& g, juce::Rectangle<float> area, bool on, bool hot, bool focused)
{
    // An ink lamp: on, a filled blot in the mood's dye inside an ink ring, with a soft dye halo; off, the
    // ring alone (hollow). The shape carries the state; the dye only adds to it.
    const float d = std::min(area.getWidth(), area.getHeight());
    const auto c = area.getCentre();
    const auto lamp = juce::Rectangle<float>(d, d).withCentre(c).reduced(d * 0.18f);

    if (on)
    {
        g.setColour(accent.withAlpha(0.24f));
        g.fillEllipse(lamp.expanded(d * 0.12f));
        g.setColour(accentInk);
        g.fillEllipse(lamp);
        g.setColour(ink);
        g.drawEllipse(lamp.reduced(0.5f), 1.2f);
    }
    else
    {
        g.setColour(sheet);
        g.fillEllipse(lamp);
        g.setColour(hot ? ink : inkFaint);
        g.drawEllipse(lamp.reduced(0.6f), hot ? 1.6f : 1.3f);
    }

    if (focused)
    {
        g.setColour(focus);
        g.drawEllipse(juce::Rectangle<float>(d, d).withCentre(c).reduced(0.75f), Metrics::focusRing);
    }
}

void RoarschachLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool isMouseOverButton, bool isButtonDown)
{
    // The lamp is drawn at its own size; the button around it is the hit target (Metrics::hitTarget).
    auto r = button.getLocalBounds().toFloat();
    const float ledSize = std::min(r.getHeight(), Metrics::lamp + 4.0f);
    const bool hot = button.isEnabled() && (isMouseOverButton || isButtonDown);
    drawLed(g, r.removeFromLeft(std::max(ledSize, std::min(r.getWidth(), r.getHeight()))).withSizeKeepingCentre(ledSize, ledSize),
            button.getToggleState() && button.isEnabled(), hot, shouldShowFocusRing(button));

    if (button.getButtonText().isNotEmpty())
    {
        r.removeFromLeft(6.0f);
        const auto font = m_fonts.label(std::min(Type::button, r.getHeight() * 0.75f));
        g.setFont(font);
        g.setColour(!button.isEnabled() ? inkFaint : button.getToggleState() ? ink : inkDim);
        g.drawText(fitToWidth(font, button.getButtonText().toUpperCase(), r.getWidth()), r, juce::Justification::centredLeft, false);
    }
}

//==============================================================================
// Combo boxes
//==============================================================================
void RoarschachLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool,
                                         int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    const bool hot = box.isMouseOver(true) || box.isPopupActive();

    g.setColour(sheet);
    g.fillRoundedRectangle(r, Metrics::radius);

    if (shouldShowFocusRing(box))
    {
        g.setColour(focus);
        g.drawRoundedRectangle(r.reduced(0.25f), Metrics::radius, Metrics::focusRing);
    }
    else
    {
        g.setColour(hot ? ink.withAlpha(0.6f) : rule);
        g.drawRoundedRectangle(r, Metrics::radius, Metrics::hairline);
    }

    // The caret: this opens a list.
    const float cx = r.getRight() - 13.0f;
    const float cy = r.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath(cx - 4.0f, cy - 2.0f);
    chevron.lineTo(cx, cy + 2.0f);
    chevron.lineTo(cx + 4.0f, cy - 2.0f);
    g.setColour(hot ? ink : inkDim);
    g.strokePath(chevron, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font RoarschachLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return m_fonts.body(Type::body);
}

void RoarschachLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(8, 0, box.getWidth() - 30, box.getHeight());
    label.setFont(getComboBoxFont(box));
}

juce::PopupMenu::Options RoarschachLookAndFeel::getOptionsForComboBoxPopupMenu(juce::ComboBox& box, juce::Label& label)
{
    // JUCE's options (rows as high as the box, at least as wide), with the same legibility floor as the
    // editor's own menus.
    return juce::LookAndFeel_V4::getOptionsForComboBoxPopupMenu(box, label)
               .withStandardItemHeight(juce::roundToInt(static_cast<float>(label.getHeight()) * legibilityFactor(box)));
}

//==============================================================================
// Buttons
//==============================================================================
bool RoarschachLookAndFeel::isPrimaryDialogButton(const juce::Button& button)
{
    return isInAlert(button) && button.isRegisteredForShortcut(juce::KeyPress(juce::KeyPress::returnKey));
}

void RoarschachLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                 const juce::Colour& backgroundColour,
                                                 bool isMouseOverButton, bool isButtonDown)
{
    if (backgroundColour.isTransparent())
        return; // a text link: no chip (see drawButtonText)

    // Paper chips: raised paper with an ink hairline. Pressed in (on, or a dialog's primary action):
    // filled with ink, its label in paper. A quiet header glyph has no chip until the mouse is over it.
    const auto r = pillBounds(button);
    const float corner = std::min(Metrics::radius, std::min(r.getWidth(), r.getHeight()) * 0.5f);
    const bool enabled = button.isEnabled();
    const bool quiet = static_cast<bool>(button.getProperties().getWithDefault(c_quietProperty, false));
    const bool filled = (button.getToggleState() && button.getClickingTogglesState()) || isPrimaryDialogButton(button);
    const int menuChip = static_cast<int>(button.getProperties().getWithDefault(c_menuChipProperty, 0));

    if (menuChip != 0)
    {
        // A mode chip: its choices on a ruled line, a caret at its end (no box). Under the mouse a wash
        // of raised paper. A chip of an Off module stands on a dotted line instead (its state by shape).
        if (enabled && (isMouseOverButton || isButtonDown))
        {
            g.setColour(isButtonDown ? selection : sheet);
            g.fillRoundedRectangle(r, corner);
        }
        const float y = std::round(r.getBottom()) - 0.5f;
        if (menuChip == 2)
        {
            juce::Path line, dotted;
            line.startNewSubPath(r.getX() + 1.0f, y);
            line.lineTo(r.getRight() - 1.0f, y);
            const float dots[] { 1.2f, 3.0f };
            juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createDashedStroke(dotted, line, dots, 2);
            g.setColour(inkFaint);
            g.fillPath(dotted);
        }
        else
        {
            g.setColour(isMouseOverButton ? ink : rule);
            g.fillRect(juce::Rectangle<float>(r.getX(), y - 0.5f, r.getWidth(), Metrics::hairline));
        }
    }
    else if (filled)
    {
        const auto fill = isPrimaryDialogButton(button) ? accentFor(dynamic_cast<juce::AlertWindow*>(button.getParentComponent())->getAlertType())
                                                         : ink;
        g.setColour(!enabled ? inkFaint : isButtonDown ? fill.withAlpha(0.8f) : fill == ink ? ink : fill);
        g.fillRoundedRectangle(r, corner);
    }
    else if (!quiet || isMouseOverButton || isButtonDown)
    {
        g.setColour(isButtonDown ? sheet.darker(0.06f) : sheet);
        g.fillRoundedRectangle(r, corner);
        g.setColour(!enabled ? ruleSoft : isMouseOverButton ? ink.withAlpha(0.65f) : rule);
        g.drawRoundedRectangle(r, corner, Metrics::hairline);
    }

    if (shouldShowFocusRing(button))
    {
        g.setColour(focus);
        g.drawRoundedRectangle(r.expanded(1.5f), corner + 1.5f, Metrics::focusRing);
    }
}

void RoarschachLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool isMouseOverButton, bool isButtonDown)
{
    const auto font = getTextButtonFont(button, button.getHeight());
    const bool capitals = isInAlert(button) || static_cast<bool>(button.getProperties().getWithDefault(c_capitalsProperty, false));
    const auto label = capitals ? button.getButtonText().toUpperCase() : button.getButtonText();

    if (isLinkStyle(button))
    {
        // A text link: dim (or its own colour) when idle, the dyed ink under the mouse, underlined on hover
        // or focus.
        auto colour = button.findColour(juce::TextButton::textColourOffId);
        if (!button.isEnabled())
            colour = inkFaint;
        else if (isButtonDown || isMouseOverButton)
            colour = accentInk;

        const auto area = button.getLocalBounds().toFloat();
        const auto shown = fitToWidth(font, label, area.getWidth());
        g.setFont(font);
        g.setColour(colour);
        g.drawText(shown, area, juce::Justification::centred, false);

        if (button.isEnabled() && (isMouseOverButton || shouldShowFocusRing(button)))
        {
            const float w = lineWidth(font, shown);
            const float y = std::round(area.getCentreY() + font.getAscent() * 0.5f + 2.0f);
            g.setColour(shouldShowFocusRing(button) ? focus : colour);
            g.fillRect(juce::Rectangle<float>(area.getCentreX() - w * 0.5f, y, w, 1.0f));
        }
        return;
    }

    const bool filled = (button.getToggleState() && button.getClickingTogglesState()) || isPrimaryDialogButton(button);
    const int menuChip = static_cast<int>(button.getProperties().getWithDefault(c_menuChipProperty, 0));
    auto colour = filled ? sheet : isMouseOverButton || isButtonDown ? ink : button.findColour(juce::TextButton::textColourOffId);
    if (!button.isEnabled() || menuChip == 2)
        colour = filled ? sheet : inkFaint;

    // The preset bar's arrows are drawn as ink chevrons, the same stroke as the menus'.
    if (label == "<" || label == ">")
    {
        g.setColour(!button.isEnabled() ? inkFaint : isMouseOverButton ? ink : inkDim);
        strokeChevron(g, button.getLocalBounds().toFloat().getCentre(), 4.5f,
                      label == "<" ? juce::MathConstants<float>::pi : 0.0f, 1.7f);
        return;
    }

    // The label keeps clear of the chip's ends, but a short label in a small chip ("?", CHOOSE) takes the
    // room it needs: a label is only cut when the chip is too narrow for it.
    // (Graphics::drawText drops a word that overruns its area, hence the pixel of slack.)
    auto r = pillBounds(button);
    if (menuChip != 0)
    {
        // The caret: this chip opens a menu.
        const auto caret = r.removeFromRight(c_caretColumn).translated(-2.0f, 0.0f);
        const auto c = caret.getCentre();
        juce::Path p;
        p.startNewSubPath(c.x - 3.5f, c.y - 1.75f);
        p.lineTo(c.x, c.y + 1.75f);
        p.lineTo(c.x + 3.5f, c.y - 1.75f);
        g.setColour(menuChip == 2 ? inkFaint : isMouseOverButton ? ink : inkDim);
        g.strokePath(p, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    // The preset's name: an edited preset is marked with a drop of the dye after its name (its text
    // keeps the " *" for screen readers and the tests).
    if (static_cast<bool>(button.getProperties().getWithDefault(c_displayFaceProperty, false)) && label.endsWith(" *"))
    {
        const auto stem = label.dropLastCharacters(2);
        const float drop = std::max(5.0f, font.getHeight() * 0.3f);
        const auto area = r.reduced(4.0f, 0.0f);
        const auto shown = fitToWidth(font, stem, area.getWidth() - drop - 6.0f);
        const float w = lineWidth(font, shown);
        const float x = area.getCentreX() - (w + drop + 6.0f) * 0.5f;
        g.setFont(font);
        g.setColour(colour);
        g.drawText(shown, juce::Rectangle<float>(x, area.getY(), w + 1.0f, area.getHeight()), juce::Justification::centredLeft, false);
        g.setColour(accentInk);
        const float baseline = area.getCentreY() + (font.getAscent() - font.getDescent()) * 0.5f;
        g.fillEllipse(juce::Rectangle<float>(drop, drop).withCentre({ x + w + 6.0f + drop * 0.5f, baseline - font.getAscent() * 0.32f }));
        return;
    }

    const float preferredPad = std::max(4.0f, std::round(r.getHeight() * 0.35f));
    const float pad = juce::jlimit(1.0f, preferredPad, (r.getWidth() - lineWidth(font, label)) * 0.5f - 1.0f);
    const auto area = r.reduced(pad, 0.0f);

    g.setFont(font);
    g.setColour(colour);
    g.drawText(fitToWidth(font, label, area.getWidth()), area, juce::Justification::centred, false);
}

juce::Font RoarschachLookAndFeel::getTextButtonFont(juce::TextButton& button, int buttonHeight)
{
    if (static_cast<bool>(button.getProperties().getWithDefault(c_displayFaceProperty, false)))
        return m_fonts.title(std::min(Type::presetName, static_cast<float>(buttonHeight) * 0.66f));
    if (isInAlert(button))
        return m_fonts.label(Type::button);
    if (isLinkStyle(button))
        return m_fonts.body(Type::body);
    if (static_cast<int>(button.getProperties().getWithDefault(c_menuChipProperty, 0)) != 0)
        return m_fonts.value(Type::chip);   // (untracked: mode names are long)
    return m_fonts.label(std::min(Type::button, static_cast<float>(buttonHeight) * 0.66f));
}

//==============================================================================
// Popup menus
//==============================================================================
juce::Font RoarschachLookAndFeel::menuFont(float scale) const
{
    return m_fonts.body(Type::menu * scale);
}

juce::Font RoarschachLookAndFeel::getPopupMenuFont()
{
    return menuFont(1.0f);
}

int RoarschachLookAndFeel::getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options& options)
{
    return std::max(2, juce::roundToInt(c_menuBorder * menuScale(options)));
}

void RoarschachLookAndFeel::drawPopupMenuBackgroundWithOptions(juce::Graphics& g, int width, int height,
                                                               const juce::PopupMenu::Options& options)
{
    const float s = menuScale(options);
    const auto r = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    const auto colour = findColour(juce::PopupMenu::backgroundColourId);
    const auto fill = colour.withAlpha(1.0f);

    // JUCE makes the window opaque when it can't be transparent: then it has no corners to show through.
    if (colour.isOpaque() || !juce::Desktop::canUseSemiTransparentWindows())
        g.fillAll(fill);

    g.setColour(fill);
    g.fillRoundedRectangle(r, c_menuRadius * s);
    g.setColour(rule);
    g.drawRoundedRectangle(r.reduced(0.5f), c_menuRadius * s, Metrics::hairline);
}

void RoarschachLookAndFeel::drawPopupMenuItemWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
                                                         bool isHighlighted, const juce::PopupMenu::Item& item,
                                                         const juce::PopupMenu::Options& options)
{
    const float s = menuScale(options);
    const auto border = static_cast<float>(getPopupMenuBorderSizeWithOptions(options));
    const auto row = area.toFloat().reduced(border, 0.0f); // rows span the column; their content sits inside the border

    if (item.isSeparator)
    {
        g.setColour(ruleSoft);
        g.fillRect(juce::Rectangle<float>(row.getX() + 8.0f * s, std::floor(row.getCentreY()), row.getWidth() - 16.0f * s, 1.0f));
        return;
    }

    const bool enabled = item.isEnabled;
    const bool hasSubMenu = item.subMenu != nullptr && (item.itemID == 0 || item.subMenu->getNumItems() > 0);
    const bool highlighted = isHighlighted && enabled;

    if (highlighted)
    {
        // A dye wash under the row, and a short dye mark at its left edge.
        g.setColour(selection);
        g.fillRoundedRectangle(row.reduced(0.0f, 1.0f), 3.0f * s);
        g.setColour(accent);
        g.fillRoundedRectangle(juce::Rectangle<float>(row.getX() + 2.0f * s, row.getY() + 5.0f * s, 2.0f * s, row.getHeight() - 10.0f * s), 1.0f * s);
    }

    auto content = row;
    const auto tickArea = content.removeFromLeft(c_menuTickColumn * s);
    content.removeFromLeft((c_menuTextInset - c_menuTickColumn) * s);
    const auto arrowArea = content.removeFromRight(c_menuArrowColumn * s);

    if (item.image != nullptr)
    {
        item.image->drawWithin(g, tickArea.reduced(5.0f * s), juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize,
                               enabled ? 1.0f : 0.4f);
    }
    else if (item.isTicked)
    {
        // A stroked check in the dyed ink.
        const auto c = tickArea.getCentre().translated(1.0f * s, 0.0f);
        juce::Path tick;
        tick.startNewSubPath(c.x - 4.0f * s, c.y + 0.2f * s);
        tick.lineTo(c.x - 1.3f * s, c.y + 3.0f * s);
        tick.lineTo(c.x + 4.0f * s, c.y - 3.2f * s);
        g.setColour(enabled ? accentInk : inkFaint);
        g.strokePath(tick, juce::PathStrokeType(1.8f * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (hasSubMenu)
    {
        g.setColour(!enabled ? inkFaint : highlighted ? ink : inkDim);
        strokeChevron(g, arrowArea.getCentre().translated(-2.0f * s, 0.0f), 3.5f * s, 0.0f, 1.4f * s);
    }

    // Text, with a trailing "(...)" qualifier in the dim ink.
    const auto font = menuFont(s);
    const auto main = !enabled ? inkFaint
                    : item.colour != juce::Colour() ? item.colour
                    : findColour(highlighted ? juce::PopupMenu::highlightedTextColourId : juce::PopupMenu::textColourId);
    const auto shown = fitToWidth(font, item.text, content.getWidth());
    const int open = shown.lastIndexOf(" (");

    g.setFont(font);
    if (shown == item.text && open > 0 && shown.endsWithChar(')') && enabled)
    {
        const auto head = shown.substring(0, open);
        g.setColour(main);
        g.drawText(head, content, juce::Justification::centredLeft, false);
        g.setColour(inkDim);
        g.drawText(shown.substring(open), content.withTrimmedLeft(lineWidth(font, head)), juce::Justification::centredLeft, false);
    }
    else
    {
        g.setColour(main);
        g.drawText(shown, content, juce::Justification::centredLeft, false);
    }

    if (item.shortcutKeyDescription.isNotEmpty())
    {
        g.setFont(m_fonts.body(Type::caption * s));
        g.setColour(inkFaint);
        g.drawText(item.shortcutKeyDescription, content, juce::Justification::centredRight, false);
    }
}

void RoarschachLookAndFeel::drawPopupMenuSectionHeaderWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
                                                                  const juce::String& sectionName,
                                                                  const juce::PopupMenu::Options& options)
{
    // The section's name in the display face, aligned with the items' text, and a ruled line running
    // on from it to the menu's edge. (JUCE already insets a header by the menu border.)
    const float s = menuScale(options);
    const auto r = area.toFloat().withTrimmedTop(8.0f * s).withTrimmedBottom(2.0f * s);
    const auto font = m_fonts.title(Type::menuHeader * s);
    const auto textArea = r.withTrimmedLeft(c_menuTextInset * s).withTrimmedRight(10.0f * s);
    const auto shown = fitToWidth(font, sectionName, textArea.getWidth());
    g.setFont(font);
    g.setColour(findColour(juce::PopupMenu::headerTextColourId));
    g.drawText(shown, textArea, juce::Justification::centredLeft, false);

    const float ruleX = textArea.getX() + lineWidth(font, shown) + 8.0f * s;
    if (ruleX < textArea.getRight() - 8.0f * s)
    {
        g.setColour(ruleSoft);
        g.fillRect(juce::Rectangle<float>(ruleX, std::round(textArea.getCentreY() + 1.0f * s), textArea.getRight() - ruleX, 1.0f));
    }
}

void RoarschachLookAndFeel::getIdealPopupMenuItemSizeWithOptions(const juce::String& itemText, bool isSeparator,
                                                                 int standardMenuItemHeight, int& idealWidth,
                                                                 int& idealHeight, const juce::PopupMenu::Options& options)
{
    const float s = menuScale(options);

    if (isSeparator)
    {
        idealWidth = juce::roundToInt(50.0f * s);
        idealHeight = std::max(5, juce::roundToInt(c_menuSeparatorHeight * s));
        return;
    }

    idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight : c_menuItemHeight;
    idealWidth = juce::roundToInt(lineWidth(menuFont(s), itemText) + (c_menuTextInset + c_menuArrowColumn + 4.0f) * s);
}

void RoarschachLookAndFeel::getIdealPopupMenuSectionHeaderSizeWithOptions(const juce::String& headerText, int,
                                                                          int& idealWidth, int& idealHeight,
                                                                          const juce::PopupMenu::Options& options)
{
    const float s = menuScale(options);
    idealHeight = juce::roundToInt(c_menuHeaderHeight * s);
    idealWidth = juce::roundToInt(lineWidth(m_fonts.title(Type::menuHeader * s), headerText) + (c_menuTextInset + 40.0f) * s);
}

void RoarschachLookAndFeel::drawPopupMenuUpDownArrowWithOptions(juce::Graphics& g, int width, int height,
                                                                bool isScrollUpArrow, const juce::PopupMenu::Options& options)
{
    // A menu taller than the screen: the body colour fading out over the rows, and a chevron.
    const float s = menuScale(options);
    const float h = static_cast<float>(height);
    const float inset = c_menuRadius * s;
    const auto fill = findColour(juce::PopupMenu::backgroundColourId).withAlpha(1.0f);

    g.setGradientFill(juce::ColourGradient(fill, 0.0f, isScrollUpArrow ? 0.0f : h,
                                           fill.withAlpha(0.0f), 0.0f, isScrollUpArrow ? h : 0.0f, false));
    g.fillRect(juce::Rectangle<float>(inset, isScrollUpArrow ? 1.0f : 0.0f, static_cast<float>(width) - 2.0f * inset, h - 1.0f));

    g.setColour(inkDim);
    strokeChevron(g, { static_cast<float>(width) * 0.5f, h * 0.5f }, 3.0f * s,
                  juce::MathConstants<float>::halfPi * (isScrollUpArrow ? -1.0f : 1.0f), 1.5f * s);
}

int RoarschachLookAndFeel::getPopupMenuColumnSeparatorWidthWithOptions(const juce::PopupMenu::Options&)
{
    return 1;
}

void RoarschachLookAndFeel::drawPopupMenuColumnSeparatorWithOptions(juce::Graphics& g, const juce::Rectangle<int>& bounds,
                                                                    const juce::PopupMenu::Options& options)
{
    g.setColour(ruleSoft);
    g.fillRect(bounds.toFloat().reduced(0.0f, 6.0f * menuScale(options)));
}

//==============================================================================
// Alerts
//==============================================================================
juce::AlertWindow* RoarschachLookAndFeel::createAlertWindow(const juce::String& title, const juce::String& message,
                                                            const juce::String& button1, const juce::String& button2,
                                                            const juce::String& button3, juce::MessageBoxIconType iconType,
                                                            int numButtons, juce::Component* associatedComponent)
{
    // JUCE's own layout, without the V4 enlargement that its icon drawing needs.
    auto* alert = juce::LookAndFeel_V2::createAlertWindow(title, message, button1, button2, button3,
                                                          iconType, numButtons, associatedComponent);
    alert->setOpaque(false);
    return alert;
}

void RoarschachLookAndFeel::drawAlertBox(juce::Graphics& g, juce::AlertWindow& alert,
                                         const juce::Rectangle<int>&, juce::TextLayout& textLayout)
{
    const auto bounds = alert.getLocalBounds().toFloat();

    if (alert.isOpaque())
        g.fillAll(paper); // an opaque window can't show what is behind its rounded corners

    // A sheet of raised paper with an ink hairline: no icon.
    g.setColour(alert.findColour(juce::AlertWindow::backgroundColourId));
    g.fillRoundedRectangle(bounds, c_dialogRadius);
    g.setColour(rule);
    g.drawRoundedRectangle(bounds.reduced(0.5f), c_dialogRadius, Metrics::hairline);

    // Header row: the title in the display face, a short rule under it (the warning ink for a warning).
    auto header = bounds.withHeight(c_dialogHeaderHeight).reduced(16.0f, 0.0f);
    const auto titleFont = getAlertWindowTitleFont();
    g.setFont(titleFont);
    g.setColour(ink);
    g.drawText(fitToWidth(titleFont, alert.getName(), header.getWidth()), header.translated(0.0f, 2.0f), juce::Justification::centredLeft, false);
    g.setColour(alert.getAlertType() == juce::MessageBoxIconType::WarningIcon ? warn : accent);
    g.fillRect(juce::Rectangle<float>(16.0f, c_dialogHeaderHeight - 3.0f, 24.0f, 1.5f));

    // JUCE lays the title and the message out as one TextLayout. The title is drawn above, so its runs are
    // hidden; the message lines are centred across the dialog, between the header and the first control.
    const auto messageColour = alert.findColour(juce::AlertWindow::textColourId);
    juce::Range<float> messageY;
    bool hasMessage = false;

    for (int i = 0; i < textLayout.getNumLines(); ++i)
    {
        auto& line = textLayout.getLine(i);
        bool isMessageLine = false;

        for (auto* run : line.runs)
        {
            const bool isTitle = run->font.getTypefacePtr() == m_fonts.displayFace;
            run->colour = isTitle ? juce::Colours::transparentBlack : messageColour;
            isMessageLine = isMessageLine || (!isTitle && !run->glyphs.isEmpty());
        }

        if (!isMessageLine)
            continue;

        const auto x = line.getLineBoundsX();
        line.lineOrigin.x += (bounds.getWidth() - x.getLength()) * 0.5f - x.getStart();
        const auto y = line.getLineBoundsY();
        messageY = hasMessage ? messageY.getUnionWith(y) : y;
        hasMessage = true;
    }

    if (!hasMessage)
        return;

    float controlsTop = bounds.getBottom() - 12.0f;
    for (auto* child : alert.getChildren())
    {
        if (!child->isVisible())
            continue;
        if (dynamic_cast<juce::Button*>(child) != nullptr)
            controlsTop = std::min(controlsTop, static_cast<float>(child->getY()) + (static_cast<float>(child->getHeight()) - c_dialogButtonHeight) * 0.5f);
        else if (dynamic_cast<juce::TextEditor*>(child) != nullptr)
            controlsTop = std::min(controlsTop, static_cast<float>(child->getY()));
    }

    const float top = c_dialogHeaderHeight + 10.0f;
    const float bottom = controlsTop - 10.0f;
    const float y = top + std::max(0.0f, (bottom - top - messageY.getLength()) * 0.5f);
    textLayout.draw(g, juce::Rectangle<float>(0.0f, y - messageY.getStart(), textLayout.getWidth(), textLayout.getHeight()));
}

int RoarschachLookAndFeel::getAlertWindowButtonHeight()
{
    return c_dialogButtonSlot;
}

juce::Array<int> RoarschachLookAndFeel::getWidthsForTextButtons(juce::AlertWindow&, const juce::Array<juce::TextButton*>& buttons)
{
    // One width for every button of a dialog, so the row reads as a set.
    const auto font = m_fonts.label(Type::button);
    int width = c_dialogButtonMinWidth;
    for (auto* b : buttons)
        width = std::max(width, juce::roundToInt(lineWidth(font, b->getButtonText().toUpperCase())) + 44);

    juce::Array<int> widths;
    widths.insertMultiple(0, width, buttons.size());
    return widths;
}

juce::Font RoarschachLookAndFeel::getAlertWindowTitleFont()   { return m_fonts.title(Type::heading); }
juce::Font RoarschachLookAndFeel::getAlertWindowMessageFont() { return m_fonts.body(Type::body); }
juce::Font RoarschachLookAndFeel::getAlertWindowFont()        { return m_fonts.body(Type::body); }

//==============================================================================
// Text fields
//==============================================================================
void RoarschachLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    const auto fill = editor.findColour(juce::TextEditor::backgroundColourId);
    if (fill.isTransparent())
        return;

    if (editor.isOpaque()) // an opaque component must cover its corners
    {
        g.fillAll(fill);
        return;
    }

    g.setColour(fill);
    g.fillRoundedRectangle(juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height)), fieldRadius(height));
}

void RoarschachLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (!editor.isEnabled())
        return;

    // A field being typed in shows its focus however it got it; read-only text (the About licence) only
    // when Tab moved the focus there, like the buttons.
    const bool focused = editor.hasKeyboardFocus(true) && (!editor.isReadOnly() || shouldShowFocusRing(editor));
    const auto colour = editor.findColour(focused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId);
    if (colour.isTransparent())
        return;

    const float thickness = focused ? Metrics::focusRing : Metrics::hairline;
    const auto r = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height)).reduced(thickness * 0.5f);
    g.setColour(colour);
    if (editor.isOpaque())
        g.drawRect(r, thickness);
    else
        g.drawRoundedRectangle(r, fieldRadius(height), thickness);
}

//==============================================================================
// Scrollbars
//==============================================================================
void RoarschachLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar, int x, int y, int width, int height,
                                          bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                                          bool isMouseOver, bool isMouseDown)
{
    if (thumbSize <= 0)
        return;

    // No track: a thin ink stroke.
    auto thumb = (isScrollbarVertical ? juce::Rectangle<int>(x, thumbStartPosition, width, thumbSize)
                                      : juce::Rectangle<int>(thumbStartPosition, y, thumbSize, height)).toFloat();
    thumb = isScrollbarVertical ? thumb.withSizeKeepingCentre(4.0f, std::max(4.0f, thumb.getHeight() - 4.0f))
                                : thumb.withSizeKeepingCentre(std::max(4.0f, thumb.getWidth() - 4.0f), 4.0f);

    g.setColour(isMouseOver || isMouseDown ? ink.withAlpha(0.6f) : scrollbar.findColour(juce::ScrollBar::thumbColourId));
    g.fillRoundedRectangle(thumb, 2.0f);
}

//==============================================================================
// Tooltips
//==============================================================================
juce::TextLayout RoarschachLookAndFeel::layoutTooltip(const juce::String& tipText, juce::Colour colour) const
{
    juce::AttributedString s;
    s.setJustification(juce::Justification::topLeft);
    s.setLineSpacing(2.0f);
    s.append(tipText, m_fonts.body(Type::body), colour);

    juce::TextLayout layout;

    // Balanced line lengths suit a single paragraph. A list (explicit line breaks) is laid out as it
    // is: JUCE's balancing compares only the last two lines, so it would push a word of an earlier,
    // longer line onto a line of its own.
    if (tipText.containsChar('\n'))
        layout.createLayout(s, c_tipMaxWidth);
    else
        layout.createLayoutWithBalancedLineLengths(s, c_tipMaxWidth);

    return layout;
}

juce::Rectangle<int> RoarschachLookAndFeel::getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                                             juce::Rectangle<int> parentArea)
{
    const auto layout = layoutTooltip(tipText, juce::Colours::black);
    const int w = static_cast<int>(std::ceil(layout.getWidth() + 2.0f * c_tipPadX));
    const int h = static_cast<int>(std::ceil(layout.getHeight() + 2.0f * c_tipPadY));

    return juce::Rectangle<int>(screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6,
                                w, h)
        .constrainedWithin(parentArea);
}

void RoarschachLookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& tipText, int width, int height)
{
    // The editor's tooltip window is transparent outside this rounded sheet.
    const auto r = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    const auto fill = findColour(juce::TooltipWindow::backgroundColourId);

    if (!juce::Desktop::canUseSemiTransparentWindows())
        g.fillAll(fill);

    g.setColour(fill);
    g.fillRoundedRectangle(r, c_tipRadius);
    g.setColour(rule);
    g.drawRoundedRectangle(r.reduced(0.5f), c_tipRadius, Metrics::hairline);

    layoutTooltip(tipText, findColour(juce::TooltipWindow::textColourId)).draw(g, r.reduced(c_tipPadX, c_tipPadY));
}

//==============================================================================
// Corner resizer, shadows, focus outlines
//==============================================================================
void RoarschachLookAndFeel::drawCornerResizer(juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
{
    // Three short ink strokes in the corner, the faint rank until the mouse is over them.
    const float right = static_cast<float>(w) - 3.5f, bottom = static_cast<float>(h) - 3.5f;
    juce::Path grip;
    for (const float d : { 3.5f, 7.5f, 11.5f })
    {
        grip.startNewSubPath(right - d, bottom);
        grip.lineTo(right, bottom - d);
    }
    g.setColour(isMouseOver || isMouseDragging ? ink : inkFaint);
    g.strokePath(grip, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

std::unique_ptr<juce::DropShadower> RoarschachLookAndFeel::createDropShadowerForComponent(juce::Component&)
{
    return std::make_unique<juce::DropShadower>(juce::DropShadow(shadow, 14, { 0, 4 }));
}

std::unique_ptr<juce::FocusOutline> RoarschachLookAndFeel::createFocusOutlineForComponent(juce::Component&)
{
    struct Properties final : public juce::FocusOutline::OutlineWindowProperties
    {
        juce::Rectangle<int> getOutlineBounds(juce::Component& c) override { return c.getScreenBounds().expanded(2); }

        void drawOutline(juce::Graphics& g, int width, int height) override
        {
            g.setColour(focus);
            g.drawRoundedRectangle(juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height)).reduced(0.75f),
                                   Metrics::radius, Metrics::focusRing);
        }
    };

    return std::make_unique<juce::FocusOutline>(std::make_unique<Properties>());
}

//==============================================================================
// ThemeLink
//==============================================================================
ThemeLink::ThemeLink(const juce::String& linkText, const juce::URL& linkUrl)
    : juce::HyperlinkButton(linkText, linkUrl)
{
}

void ThemeLink::setFont(const juce::Font& newFont, bool resizeToMatchComponentHeight, juce::Justification newJustification)
{
    m_font = newFont;
    m_justification = newJustification;
    juce::HyperlinkButton::setFont(newFont, resizeToMatchComponentHeight, newJustification);
}

void ThemeLink::paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown)
{
    // Idle in its own colour (textColourId), the dyed ink under the mouse (read live: it follows the mood).
    auto colour = findColour(juce::HyperlinkButton::textColourId);
    if (!isEnabled())
        colour = Theme::Colours::inkFaint;
    else if (isButtonDown || isMouseOverButton)
        colour = Theme::Colours::accentInk;

    const auto area = getLocalBounds().reduced(1, 0).toFloat();
    g.setColour(colour);
    g.setFont(m_font);
    g.drawText(getButtonText(), area, m_justification.getOnlyHorizontalFlags() | juce::Justification::verticallyCentred, true);

    if (RoarschachLookAndFeel::shouldShowFocusRing(*this) || (isMouseOverButton && isEnabled()))
    {
        // An underline under the text (hover, and keyboard focus).
        const float w = std::min(area.getWidth(), lineWidth(m_font, getButtonText()));
        const float x = m_justification.testFlags(juce::Justification::left) ? area.getX()
                      : m_justification.testFlags(juce::Justification::right) ? area.getRight() - w
                      : area.getCentreX() - w * 0.5f;
        g.setColour(RoarschachLookAndFeel::shouldShowFocusRing(*this) ? Theme::Colours::focus : colour);
        g.fillRect(juce::Rectangle<float>(x, std::round(area.getCentreY() + m_font.getAscent() * 0.5f + 2.0f), w, 1.0f));
    }
}

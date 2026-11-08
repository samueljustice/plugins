#include "Controls.h"

namespace
{
    using namespace Theme::Colours;

    const juce::Identifier c_styleProperty { "reversinatorStyle" };
    const juce::Identifier c_focusRingsProperty { "reversinatorFocusRings" };
    const juce::Identifier c_capitalsProperty { "reversinatorCapitals" };

    constexpr float c_valueBoxHeight = 18.0f;
    constexpr float c_valueBoxGap = 10.0f;

    // The house style's metrics (as Subbertone's LookAndFeel). Menu metrics are at design size and
    // scaled by menuScale().
    constexpr float c_menuRadius = 6.0f;
    constexpr float c_menuBorder = 5.0f;
    constexpr float c_menuTickColumn = 22.0f;
    constexpr float c_menuTextInset = 24.0f;
    constexpr float c_menuArrowColumn = 20.0f;
    constexpr float c_menuHeaderHeight = 26.0f;
    constexpr float c_menuSeparatorHeight = 9.0f;
    constexpr float c_menuFontHeight = 13.0f;

    constexpr float c_tipRadius = 5.0f;
    constexpr float c_tipPadX = 10.0f, c_tipPadY = 7.0f;
    constexpr float c_tipMaxWidth = 420.0f;

    juce::Colour accentOf(const juce::Slider& slider)
    {
        return slider.findColour(juce::Slider::trackColourId);
    }

    // A colour that looks opaque but makes JUCE create a transparent window (menus), so the rounded
    // corners show what is behind them. The body itself is drawn with full alpha.
    juce::Colour nearlyOpaque(juce::Colour c) { return c.withAlpha(static_cast<juce::uint8>(0xfe)); }

    // Width of a line of text as drawn: getStringWidth() skips ligature placeholders, so text ending in a
    // ligature would measure short.
    float lineWidth(const juce::Font& font, const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth(font, text + " ") - juce::GlyphArrangement::getStringWidth(font, " ");
    }

    // One line, never squashed: too long gets "...".
    juce::String fitToWidth(const juce::Font& font, const juce::String& text, float width)
    {
        width -= 1.0f; // Graphics::drawText() drops a word that overruns its area by any amount

        if (lineWidth(font, text) <= width)
            return text;

        auto stem = text;
        while (stem.isNotEmpty() && lineWidth(font, stem.trimEnd() + "...") > width)
            stem = stem.dropLastCharacters(1);
        return stem.trimEnd() + "...";
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

    // Every menu metric follows the menu's row height, so a submenu (which inherits it) matches its parent.
    float menuScale(const juce::PopupMenu::Options& options)
    {
        const int h = options.getStandardItemHeight();
        return h > 0 ? static_cast<float>(h) / static_cast<float>(Controls::LookAndFeel::menuItemHeight) : 1.0f;
    }

    // The accent of a menu: the colour its control asks for, the logo blue for everything else.
    juce::Colour menuAccent(const juce::PopupMenu::Options& options)
    {
        if (auto* target = options.getTargetComponent())
            if (target->isColourSpecified(juce::PopupMenu::highlightedBackgroundColourId))
                return target->findColour(juce::PopupMenu::highlightedBackgroundColourId).withAlpha(1.0f);

        return reverser;
    }

    // A text button with a transparent fill is a text link (the About panel's website and e-mail).
    bool isLinkStyle(const juce::Button& b)
    {
        return b.findColour(juce::TextButton::buttonColourId).isTransparent();
    }

    float fieldRadius(int height) { return std::min(5.0f, static_cast<float>(height) * 0.25f); }

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
        return { background, panelRaised, panelRaised, outline, text, reverser, text, reverser.withAlpha(0.3f), text };
    }

    /** Polyline along an ellipse from angle a0 to a1 (radians, counter-clockwise on screen). */
    juce::Path ellipseArc(juce::Point<float> centre, float rx, float ry, float a0, float a1)
    {
        juce::Path p;
        const int steps = juce::jmax(2, static_cast<int>(std::abs(a1 - a0) * 24.0f));

        for (int i = 0; i <= steps; ++i)
        {
            const float a = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(steps);
            const juce::Point<float> pt { centre.x + rx * std::cos(a), centre.y - ry * std::sin(a) };
            if (i == 0) p.startNewSubPath(pt);
            else        p.lineTo(pt);
        }

        return p;
    }
}

void Controls::setStyle(juce::Slider& slider, SliderStyle style)
{
    slider.getProperties().set(c_styleProperty, static_cast<int>(style));
    slider.setSliderStyle(juce::Slider::LinearVertical);
    slider.setTextBoxStyle(juce::Slider::TextBoxAbove, false, 84, static_cast<int>(c_valueBoxHeight));
}

//==============================================================================
// LookAndFeel
//==============================================================================
Controls::LookAndFeel::LookAndFeel(const Theme::Fonts& fonts)
    : juce::LookAndFeel_V4(themeScheme())
    , m_fonts(fonts)
{
    const auto transparent = juce::Colours::transparentBlack;

    if (fonts.medium != nullptr)
        setDefaultSansSerifTypeface(fonts.medium);

    setColour(juce::ResizableWindow::backgroundColourId, background);
    setColour(juce::Label::textColourId, text);

    setColour(juce::Slider::textBoxTextColourId, text);
    setColour(juce::Slider::textBoxOutlineColourId, transparent);
    setColour(juce::Slider::textBoxBackgroundColourId, transparent);
    setColour(juce::Slider::textBoxHighlightColourId, reverser.withAlpha(0.3f));

    setColour(juce::PopupMenu::backgroundColourId, nearlyOpaque(panelRaised));
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::headerTextColourId, textDim);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, reverser.withAlpha(0.16f));
    setColour(juce::PopupMenu::highlightedTextColourId, text);

    setColour(juce::TextButton::buttonColourId, panelRaised);
    setColour(juce::TextButton::buttonOnColourId, panelRaised.brighter(0.1f));
    setColour(juce::TextButton::textColourOffId, textDim);
    setColour(juce::TextButton::textColourOnId, text);

    setColour(juce::TextEditor::backgroundColourId, panelRaised);
    setColour(juce::TextEditor::textColourId, text);
    setColour(juce::TextEditor::outlineColourId, outline);
    setColour(juce::TextEditor::focusedOutlineColourId, reverser.withAlpha(0.75f));
    setColour(juce::TextEditor::highlightColourId, reverser.withAlpha(0.3f));
    setColour(juce::TextEditor::highlightedTextColourId, text);
    setColour(juce::TextEditor::shadowColourId, transparent);
    setColour(juce::Label::textWhenEditingColourId, text);
    setColour(juce::CaretComponent::caretColourId, reverser);

    setColour(juce::ScrollBar::thumbColourId, textFaint);
    setColour(juce::ScrollBar::trackColourId, transparent);
    setColour(juce::ScrollBar::backgroundColourId, transparent);

    setColour(juce::TooltipWindow::backgroundColourId, panelRaised);
    setColour(juce::TooltipWindow::textColourId, text);
    setColour(juce::TooltipWindow::outlineColourId, outline);
}

void Controls::LookAndFeel::showFocusRingsAfterTab(juce::Component& window)
{
    window.addKeyListener(&focusRingTracker());
    window.addMouseListener(&focusRingTracker(), true);
}

bool Controls::LookAndFeel::shouldShowFocusRing(const juce::Component& c)
{
    if (! c.hasKeyboardFocus(false))
        return false;

    const auto* window = c.getTopLevelComponent();
    return static_cast<bool>(window->getProperties().getWithDefault(c_focusRingsProperty, false));
}

void Controls::LookAndFeel::setLabelDrawnInCapitals(juce::Button& button)
{
    button.getProperties().set(c_capitalsProperty, true);
    button.repaint();
}

int Controls::LookAndFeel::getSliderThumbRadius(juce::Slider& slider)
{
    const int style = slider.getProperties().getWithDefault(c_styleProperty, -1);
    return style == static_cast<int>(SliderStyle::Hourglass) ? 12 : 8;
}

juce::Slider::SliderLayout Controls::LookAndFeel::getSliderLayout(juce::Slider& slider)
{
    juce::Slider::SliderLayout layout;
    auto bounds = slider.getLocalBounds();
    const int boxHeight = static_cast<int>(c_valueBoxHeight);
    layout.textBoxBounds = bounds.removeFromTop(boxHeight).withSizeKeepingCentre(std::min(bounds.getWidth(), 84), boxHeight);
    bounds.removeFromTop(static_cast<int>(c_valueBoxGap));

    // Inset the track by the thumb radius (as JUCE's own layouts do), so the thumb, the
    // hourglass caps and the loop's ends are drawn in full at both ends of the range.
    layout.sliderBounds = bounds.reduced(0, getSliderThumbRadius(slider));
    return layout;
}

juce::Label* Controls::LookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
    label->setFont(m_fonts.body(12.5f));
    label->setJustificationType(juce::Justification::centred);
    label->setColour(juce::Label::textColourId, text);
    label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

    // While a value is typed: a rounded field in the slider's own accent (outline, selection, caret),
    // with the text centred like the value it replaces. Label::createEditorComponent() copies these to
    // its TextEditor.
    const auto accent = accentOf(slider);
    label->setColour(juce::Label::textWhenEditingColourId, text);
    label->setColour(juce::Label::backgroundWhenEditingColourId, panelRaised);
    label->setColour(juce::Label::outlineWhenEditingColourId, accent.withAlpha(0.6f));
    label->setColour(juce::TextEditor::highlightColourId, accent.withAlpha(0.35f));
    label->setColour(juce::TextEditor::highlightedTextColourId, text);
    label->setColour(juce::CaretComponent::caretColourId, accent);
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

juce::Font Controls::LookAndFeel::getLabelFont(juce::Label& label)
{
    // Value boxes show values; a label whose code set one of the theme's fonts keeps it (the About
    // panel's status line); any other label is a caption. JUCE's default label font never shows.
    if (dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr)
        return m_fonts.body(12.5f);

    const auto face = label.getFont().getTypefacePtr();
    if (face != nullptr && (face == m_fonts.regular || face == m_fonts.medium || face == m_fonts.bold))
        return label.getFont();

    return m_fonts.label(10.5f);
}

//==============================================================================
// Text fields
//==============================================================================
void Controls::LookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
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

void Controls::LookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (! editor.isEnabled())
        return;

    // A field being typed in shows its focus however it got it; read-only text (the About licence) only
    // when Tab moved the focus there, like the buttons.
    const bool focused = editor.hasKeyboardFocus(true) && (! editor.isReadOnly() || shouldShowFocusRing(editor));
    const auto colour = editor.findColour(focused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId);
    if (colour.isTransparent())
        return;

    const float thickness = focused ? 1.5f : 1.0f;
    const auto r = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height)).reduced(thickness * 0.5f);
    g.setColour(colour);
    if (editor.isOpaque())
        g.drawRect(r, thickness);
    else
        g.drawRoundedRectangle(r, fieldRadius(height), thickness);
}

//==============================================================================
// Popup menus
//==============================================================================
juce::Font Controls::LookAndFeel::menuFont(float scale) const
{
    return m_fonts.body(c_menuFontHeight * scale);
}

juce::Font Controls::LookAndFeel::getPopupMenuFont()
{
    return menuFont(1.0f);
}

int Controls::LookAndFeel::getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options& options)
{
    return std::max(2, juce::roundToInt(c_menuBorder * menuScale(options)));
}

void Controls::LookAndFeel::drawPopupMenuBackgroundWithOptions(juce::Graphics& g, int width, int height,
                                                               const juce::PopupMenu::Options& options)
{
    const float s = menuScale(options);
    const auto r = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    const auto colour = findColour(juce::PopupMenu::backgroundColourId);
    const auto fill = colour.withAlpha(1.0f);

    // JUCE makes the window opaque when it can't be transparent: then it has no corners to show through.
    if (colour.isOpaque() || ! juce::Desktop::canUseSemiTransparentWindows())
        g.fillAll(fill);

    g.setColour(fill);
    g.fillRoundedRectangle(r, c_menuRadius * s);
    g.setColour(outline);
    g.drawRoundedRectangle(r.reduced(0.5f), c_menuRadius * s, 1.0f);
}

void Controls::LookAndFeel::drawPopupMenuItemWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
                                                         bool isHighlighted, const juce::PopupMenu::Item& item,
                                                         const juce::PopupMenu::Options& options)
{
    const float s = menuScale(options);
    const auto border = static_cast<float>(getPopupMenuBorderSizeWithOptions(options));
    const auto row = area.toFloat().reduced(border, 0.0f); // rows span the column; their content sits inside the border

    if (item.isSeparator)
    {
        g.setColour(outline);
        g.fillRect(juce::Rectangle<float>(row.getX() + 8.0f * s, std::floor(row.getCentreY()), row.getWidth() - 16.0f * s, 1.0f));
        return;
    }

    const auto accent = menuAccent(options);
    const bool enabled = item.isEnabled;
    const bool hasSubMenu = item.subMenu != nullptr && (item.itemID == 0 || item.subMenu->getNumItems() > 0);
    const bool highlighted = isHighlighted && enabled;

    if (highlighted)
    {
        g.setColour(accent.withAlpha(0.16f));
        g.fillRoundedRectangle(row.reduced(0.0f, 1.0f), 4.0f * s);
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
        // A stroked check in the menu's accent.
        const auto c = tickArea.getCentre().translated(1.0f * s, 0.0f);
        juce::Path tick;
        tick.startNewSubPath(c.x - 4.0f * s, c.y + 0.2f * s);
        tick.lineTo(c.x - 1.3f * s, c.y + 3.0f * s);
        tick.lineTo(c.x + 4.0f * s, c.y - 3.2f * s);
        g.setColour(enabled ? accent : textFaint);
        g.strokePath(tick, juce::PathStrokeType(1.7f * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (hasSubMenu)
    {
        g.setColour(! enabled ? textFaint : highlighted ? text : textDim);
        strokeChevron(g, arrowArea.getCentre().translated(-2.0f * s, 0.0f), 3.5f * s, 0.0f, 1.4f * s);
    }

    // Text, with a trailing "(...)" qualifier in the dim colour.
    const auto font = menuFont(s);
    const auto main = ! enabled ? textFaint
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
        g.setColour(textDim);
        g.drawText(shown.substring(open), content.withTrimmedLeft(lineWidth(font, head)), juce::Justification::centredLeft, false);
    }
    else
    {
        g.setColour(main);
        g.drawText(shown, content, juce::Justification::centredLeft, false);
    }

    if (item.shortcutKeyDescription.isNotEmpty())
    {
        g.setFont(m_fonts.label(10.0f * s));
        g.setColour(textFaint);
        g.drawText(item.shortcutKeyDescription, content, juce::Justification::centredRight, false);
    }
}

void Controls::LookAndFeel::drawPopupMenuSectionHeaderWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
                                                                  const juce::String& sectionName,
                                                                  const juce::PopupMenu::Options& options)
{
    // In the panel-header style: an accent dot in the tick column, then the name in the heading font, aligned
    // with the items' text. (JUCE already insets a header by the menu border.)
    const float s = menuScale(options);
    const auto r = area.toFloat().withTrimmedTop(8.0f * s).withTrimmedBottom(2.0f * s);
    const float dot = 5.0f * s;

    g.setColour(menuAccent(options));
    g.fillEllipse(juce::Rectangle<float>(dot, dot).withCentre({ r.getX() + c_menuTickColumn * s * 0.5f + 1.0f * s, r.getCentreY() }));

    const auto font = m_fonts.heading(10.5f * s);
    const auto textArea = r.withTrimmedLeft(c_menuTextInset * s).withTrimmedRight(8.0f * s);
    g.setFont(font);
    g.setColour(findColour(juce::PopupMenu::headerTextColourId));
    g.drawText(fitToWidth(font, sectionName, textArea.getWidth()), textArea, juce::Justification::centredLeft, false);
}

void Controls::LookAndFeel::getIdealPopupMenuItemSizeWithOptions(const juce::String& itemText, bool isSeparator,
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

    idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight : menuItemHeight;
    idealWidth = juce::roundToInt(lineWidth(menuFont(s), itemText) + (c_menuTextInset + c_menuArrowColumn + 4.0f) * s);
}

void Controls::LookAndFeel::getIdealPopupMenuSectionHeaderSizeWithOptions(const juce::String& headerText, int,
                                                                          int& idealWidth, int& idealHeight,
                                                                          const juce::PopupMenu::Options& options)
{
    const float s = menuScale(options);
    idealHeight = juce::roundToInt(c_menuHeaderHeight * s);
    idealWidth = juce::roundToInt(lineWidth(m_fonts.heading(10.5f * s), headerText) + (c_menuTextInset + 12.0f) * s);
}

void Controls::LookAndFeel::drawPopupMenuUpDownArrowWithOptions(juce::Graphics& g, int width, int height,
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

    g.setColour(textDim);
    strokeChevron(g, { static_cast<float>(width) * 0.5f, h * 0.5f }, 3.0f * s,
                  juce::MathConstants<float>::halfPi * (isScrollUpArrow ? -1.0f : 1.0f), 1.5f * s);
}

int Controls::LookAndFeel::getPopupMenuColumnSeparatorWidthWithOptions(const juce::PopupMenu::Options&)
{
    return 1;
}

void Controls::LookAndFeel::drawPopupMenuColumnSeparatorWithOptions(juce::Graphics& g, const juce::Rectangle<int>& bounds,
                                                                    const juce::PopupMenu::Options& options)
{
    g.setColour(outline);
    g.fillRect(bounds.toFloat().reduced(0.0f, 6.0f * menuScale(options)));
}

void Controls::LookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                             float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                             juce::Slider& slider)
{
    const int ourStyle = slider.getProperties().getWithDefault(c_styleProperty, -1);
    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat();

    // minSliderPos/maxSliderPos are the thumbs of two-value sliders, not the track ends.
    const float bottom = slider.getPositionOfValue(slider.getMinimum());
    const float top = slider.getPositionOfValue(slider.getMaximum());

    switch (ourStyle)
    {
        case static_cast<int>(SliderStyle::Hourglass): drawHourglass(g, area, sliderPos, bottom, top, slider); break;
        case static_cast<int>(SliderStyle::Loop):      drawLoop(g, area, sliderPos, bottom, top, slider); break;
        case static_cast<int>(SliderStyle::Fader):     drawFader(g, area, sliderPos, bottom, top, slider); break;
        default:
            juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
            break;
    }
}

void Controls::LookAndFeel::drawThumb(juce::Graphics& g, juce::Point<float> centre, float width, juce::Colour accent, bool hot)
{
    using namespace Theme::Colours;

    const auto r = juce::Rectangle<float>(width, 10.0f).withCentre(centre);
    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.fillRoundedRectangle(r.translated(0.0f, 1.5f), 5.0f);
    g.setGradientFill(juce::ColourGradient(panelRaised.brighter(hot ? 0.45f : 0.32f), r.getCentreX(), r.getY(),
                                           panelRaised.brighter(0.05f), r.getCentreX(), r.getBottom(), false));
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(outline.brighter(0.3f));
    g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);
    g.setColour(accent);
    g.fillRoundedRectangle(r.withSizeKeepingCentre(width * 0.45f, 2.0f), 1.0f);
}

void Controls::LookAndFeel::drawHourglass(juce::Graphics& g, juce::Rectangle<float> area, float sliderPos,
                                          float minPos, float maxPos, juce::Slider& slider)
{
    using namespace Theme::Colours;

    const auto accent = accentOf(slider);
    const bool hot = slider.isMouseOverOrDragging();

    const float glassWidth = std::min(area.getWidth() - 8.0f, 66.0f);
    const float cx = area.getCentreX();
    const float top = maxPos - 2.0f;
    const float bottom = minPos + 2.0f;
    const float cy = (top + bottom) * 0.5f;
    const float h = bottom - top;
    const float halfW = glassWidth * 0.5f;
    const float neck = std::max(2.5f, glassWidth * 0.06f);

    juce::Path glass;
    glass.startNewSubPath(cx - halfW, top);
    glass.cubicTo(cx - halfW, top + h * 0.30f, cx - neck, cy - h * 0.14f, cx - neck, cy);
    glass.cubicTo(cx - neck, cy + h * 0.14f, cx - halfW, bottom - h * 0.30f, cx - halfW, bottom);
    glass.lineTo(cx + halfW, bottom);
    glass.cubicTo(cx + halfW, bottom - h * 0.30f, cx + neck, cy + h * 0.14f, cx + neck, cy);
    glass.cubicTo(cx + neck, cy - h * 0.14f, cx + halfW, top + h * 0.30f, cx + halfW, top);
    glass.closeSubPath();

    // Glass body
    g.setColour(scope.withAlpha(0.85f));
    g.fillPath(glass);

    // Sand: the glass fills with amber from the bottom up to the value.
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(glass);

        g.setColour(accent.withAlpha(0.07f));
        g.fillRect(juce::Rectangle<float>(cx - halfW, top, glassWidth, sliderPos - top));

        const auto sandArea = juce::Rectangle<float>(cx - halfW, sliderPos, glassWidth, bottom - sliderPos);
        g.setGradientFill(juce::ColourGradient(accent.brighter(0.25f), cx, sliderPos,
                                               accent.darker(0.55f), cx, bottom, false));
        g.fillRect(sandArea);

        // Grain texture: a few faint horizontal strata.
        g.setColour(juce::Colours::black.withAlpha(0.10f));
        for (float sy = bottom - 5.0f; sy > sliderPos + 2.0f; sy -= 7.0f)
            g.drawHorizontalLine(static_cast<int>(sy), cx - halfW, cx + halfW);

        // Surface highlight
        g.setColour(accent.brighter(0.6f).withAlpha(hot ? 0.95f : 0.75f));
        g.fillRect(juce::Rectangle<float>(cx - halfW, sliderPos - 1.0f, glassWidth, 2.0f));
    }

    // Outline and a soft reflection on the left bulbs
    g.setColour(hot ? textDim : textFaint);
    g.strokePath(glass, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path reflection;
    reflection.startNewSubPath(cx - halfW + 5.0f, top + 6.0f);
    reflection.cubicTo(cx - halfW + 5.0f, top + h * 0.22f, cx - neck - 3.0f, cy - h * 0.16f, cx - neck - 2.0f, cy - h * 0.06f);
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.strokePath(reflection, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // End caps
    for (const float capY : { top, bottom })
    {
        const auto cap = juce::Rectangle<float>(glassWidth + 12.0f, 6.0f).withCentre({ cx, capY });
        g.setGradientFill(juce::ColourGradient(panelRaised.brighter(0.35f), cx, cap.getY(), panelRaised.darker(0.2f), cx, cap.getBottom(), false));
        g.fillRoundedRectangle(cap, 3.0f);
        g.setColour(outline.brighter(0.2f));
        g.drawRoundedRectangle(cap.reduced(0.5f), 3.0f, 1.0f);
    }
}

void Controls::LookAndFeel::drawLoop(juce::Graphics& g, juce::Rectangle<float> area, float sliderPos,
                                     float minPos, float maxPos, juce::Slider& slider)
{
    using namespace Theme::Colours;

    const auto accent = accentOf(slider);
    const bool hot = slider.isMouseOverOrDragging();
    const float cx = area.getCentreX();
    const float proportion = minPos > maxPos ? juce::jlimit(0.0f, 1.0f, (minPos - sliderPos) / (minPos - maxPos)) : 0.0f;

    // The loop: up the right side, over the top, arrowhead pointing back down into the slider.
    const juce::Point<float> centre { cx, (minPos + maxPos) * 0.5f };
    const float rx = std::min(area.getWidth() * 0.5f - 6.0f, 40.0f);
    const float ry = (minPos - maxPos) * 0.5f + 5.0f;
    const float start = -juce::MathConstants<float>::halfPi;               // bottom
    const float end = juce::MathConstants<float>::pi * 0.86f;              // upper left
    const float stroke = 3.0f;

    g.setColour(outline);
    g.strokePath(ellipseArc(centre, rx, ry, start, end), juce::PathStrokeType(stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (proportion > 0.0f)
    {
        const auto active = ellipseArc(centre, rx, ry, start, start + (end - start) * proportion);
        g.setColour(accent.withAlpha(0.22f));
        g.strokePath(active, juce::PathStrokeType(stroke + 5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(accent.withAlpha(hot ? 1.0f : 0.9f));
        g.strokePath(active, juce::PathStrokeType(stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Arrowhead at the end of the loop, along the direction of travel.
    {
        const juce::Point<float> tip { centre.x + rx * std::cos(end), centre.y - ry * std::sin(end) };
        const auto dir = juce::Point<float>(-rx * std::sin(end), -ry * std::cos(end)) / std::hypot(rx * std::sin(end), ry * std::cos(end));
        const juce::Point<float> normal { -dir.y, dir.x };
        const float size = 7.0f;

        juce::Path head;
        head.addTriangle(tip + dir * size, tip - dir * (size * 0.4f) + normal * size, tip - dir * (size * 0.4f) - normal * size);
        g.setColour(proportion >= 0.999f ? accent : (proportion > 0.0f ? accent.withAlpha(0.55f) : outline.brighter(0.15f)));
        g.fillPath(head);
    }

    // Rail and thumb
    const auto rail = juce::Rectangle<float>(4.0f, minPos - maxPos + 8.0f).withCentre({ cx, centre.y });
    g.setColour(scope);
    g.fillRoundedRectangle(rail, 2.0f);
    g.setColour(accent.withAlpha(0.85f));
    g.fillRoundedRectangle(rail.withTop(sliderPos), 2.0f);

    drawThumb(g, { cx, sliderPos }, 26.0f, accent, hot);
}

void Controls::LookAndFeel::drawFader(juce::Graphics& g, juce::Rectangle<float> area, float sliderPos,
                                      float minPos, float maxPos, juce::Slider& slider)
{
    using namespace Theme::Colours;

    const auto accent = accentOf(slider);
    const bool hot = slider.isMouseOverOrDragging();
    const float cx = area.getCentreX();

    // Scale ticks at 0, 25, 50, 75 and 100 %.
    g.setColour(textFaint.withAlpha(0.7f));
    for (int i = 0; i <= 4; ++i)
    {
        const float ty = juce::jmap(static_cast<float>(i), 0.0f, 4.0f, minPos, maxPos);
        const float len = (i % 2 == 0) ? 8.0f : 5.0f;
        g.fillRect(juce::Rectangle<float>(cx - 13.0f - len, ty - 0.5f, len, 1.0f));
        g.fillRect(juce::Rectangle<float>(cx + 13.0f, ty - 0.5f, len, 1.0f));
    }

    const auto rail = juce::Rectangle<float>(6.0f, minPos - maxPos + 8.0f).withCentre({ cx, (minPos + maxPos) * 0.5f });
    g.setColour(scope);
    g.fillRoundedRectangle(rail, 3.0f);
    g.setColour(outlineSoft);
    g.drawRoundedRectangle(rail.reduced(0.5f), 3.0f, 1.0f);

    const auto fill = rail.withTop(sliderPos);
    g.setGradientFill(juce::ColourGradient(accent.brighter(0.2f), cx, sliderPos, accent.darker(0.5f), cx, rail.getBottom(), false));
    g.fillRoundedRectangle(fill, 3.0f);

    drawThumb(g, { cx, sliderPos }, 30.0f, accent, hot);
}

//==============================================================================
// Buttons
//==============================================================================
void Controls::LookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                                 bool isMouseOverButton, bool isButtonDown)
{
    if (backgroundColour.isTransparent())
        return; // a text link: no pill (see drawButtonText)

    const auto r = button.getLocalBounds().toFloat().reduced(0.5f);
    const float corner = std::min(r.getWidth(), r.getHeight()) * 0.5f;
    const bool enabled = button.isEnabled();
    const auto fill = enabled ? backgroundColour : backgroundColour.withMultipliedAlpha(0.5f);

    g.setColour(fill.brighter(isButtonDown ? 0.2f : isMouseOverButton && enabled ? 0.1f : 0.0f));
    g.fillRoundedRectangle(r, corner);

    if (shouldShowFocusRing(button))
    {
        g.setColour(reverser);
        g.drawRoundedRectangle(r.reduced(0.25f), corner, 1.5f);
    }
    else
    {
        g.setColour(! enabled ? outline.withMultipliedAlpha(0.6f) : isMouseOverButton ? textDim : outline);
        g.drawRoundedRectangle(r, corner, 1.0f);
    }
}

void Controls::LookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool isMouseOverButton, bool isButtonDown)
{
    const auto font = getTextButtonFont(button, button.getHeight());
    const bool capitals = static_cast<bool>(button.getProperties().getWithDefault(c_capitalsProperty, false));
    const auto label = capitals ? button.getButtonText().toUpperCase() : button.getButtonText();

    if (isLinkStyle(button))
    {
        // A text link: its own colour when idle, the accent under the mouse, underlined on hover or focus.
        auto colour = button.findColour(juce::TextButton::textColourOffId);
        if (! button.isEnabled())
            colour = textFaint;
        else if (isButtonDown)
            colour = reverser.darker(0.15f);
        else if (isMouseOverButton)
            colour = reverser;

        const auto area = button.getLocalBounds().toFloat();
        const auto shown = fitToWidth(font, label, area.getWidth());
        g.setFont(font);
        g.setColour(colour);
        g.drawText(shown, area, juce::Justification::centred, false);

        if (button.isEnabled() && (isMouseOverButton || shouldShowFocusRing(button)))
        {
            const float w = lineWidth(font, shown);
            const float y = std::round(area.getCentreY() + font.getAscent() * 0.5f + 2.0f);
            g.setColour(shouldShowFocusRing(button) ? reverser : colour);
            g.fillRect(juce::Rectangle<float>(area.getCentreX() - w * 0.5f, y, w, 1.0f));
        }
        return;
    }

    auto colour = button.findColour(button.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
    if (! button.isEnabled())
        colour = textFaint;

    // The label keeps clear of the pill's rounded ends, but a short label in a small pill ("?", the
    // About panel's close button) takes the room it needs.
    const auto r = button.getLocalBounds().toFloat().reduced(0.5f);
    const float preferredPad = std::max(4.0f, std::round(r.getHeight() * 0.35f));
    const float pad = juce::jlimit(1.0f, preferredPad, (r.getWidth() - lineWidth(font, label)) * 0.5f - 1.0f);
    const auto area = r.reduced(pad, 0.0f);

    g.setFont(font);
    g.setColour(colour);
    g.drawText(fitToWidth(font, label, area.getWidth()), area, juce::Justification::centred, false);
}

juce::Font Controls::LookAndFeel::getTextButtonFont(juce::TextButton& button, int buttonHeight)
{
    if (isLinkStyle(button))
        return m_fonts.body(12.0f);
    return m_fonts.label(std::min(13.0f, static_cast<float>(buttonHeight) * 0.5f));
}

//==============================================================================
// Scrollbars
//==============================================================================
void Controls::LookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar, int x, int y, int width, int height,
                                          bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                                          bool isMouseOver, bool isMouseDown)
{
    if (thumbSize <= 0)
        return;

    // No track: a thin pill.
    auto thumb = (isScrollbarVertical ? juce::Rectangle<int>(x, thumbStartPosition, width, thumbSize)
                                      : juce::Rectangle<int>(thumbStartPosition, y, thumbSize, height)).toFloat();
    thumb = isScrollbarVertical ? thumb.withSizeKeepingCentre(4.0f, std::max(4.0f, thumb.getHeight() - 4.0f))
                                : thumb.withSizeKeepingCentre(std::max(4.0f, thumb.getWidth() - 4.0f), 4.0f);

    g.setColour(isMouseOver || isMouseDown ? textDim : scrollbar.findColour(juce::ScrollBar::thumbColourId));
    g.fillRoundedRectangle(thumb, 2.0f);
}

//==============================================================================
// Tooltips
//==============================================================================
juce::TextLayout Controls::LookAndFeel::layoutTooltip(const juce::String& tipText, juce::Colour colour) const
{
    juce::AttributedString s;
    s.setJustification(juce::Justification::topLeft);
    s.setLineSpacing(2.0f);
    s.append(tipText, m_fonts.body(11.5f), colour);

    juce::TextLayout layout;

    // Balanced line lengths suit a single paragraph; a list (explicit line breaks) is laid out as it is.
    if (tipText.containsChar('\n'))
        layout.createLayout(s, c_tipMaxWidth);
    else
        layout.createLayoutWithBalancedLineLengths(s, c_tipMaxWidth);

    return layout;
}

juce::Rectangle<int> Controls::LookAndFeel::getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
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

void Controls::LookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& tipText, int width, int height)
{
    // The editor's tooltip window is transparent outside this rounded panel.
    const auto r = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    const auto fill = findColour(juce::TooltipWindow::backgroundColourId);

    if (! juce::Desktop::canUseSemiTransparentWindows())
        g.fillAll(fill);

    g.setColour(fill);
    g.fillRoundedRectangle(r, c_tipRadius);
    g.setColour(findColour(juce::TooltipWindow::outlineColourId));
    g.drawRoundedRectangle(r.reduced(0.5f), c_tipRadius, 1.0f);

    layoutTooltip(tipText, findColour(juce::TooltipWindow::textColourId)).draw(g, r.reduced(c_tipPadX, c_tipPadY));
}

//==============================================================================
// Corner resizer, shadows, focus outlines
//==============================================================================
void Controls::LookAndFeel::drawCornerResizer(juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
{
    // Three short grip strokes in the corner, faint until the mouse is over them.
    const float right = static_cast<float>(w) - 3.5f, bottom = static_cast<float>(h) - 3.5f;
    juce::Path grip;
    for (const float d : { 3.5f, 7.5f, 11.5f })
    {
        grip.startNewSubPath(right - d, bottom);
        grip.lineTo(right, bottom - d);
    }
    g.setColour(isMouseOver || isMouseDragging ? textDim : textFaint);
    g.strokePath(grip, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

std::unique_ptr<juce::DropShadower> Controls::LookAndFeel::createDropShadowerForComponent(juce::Component&)
{
    return std::make_unique<juce::DropShadower>(juce::DropShadow(juce::Colours::black.withAlpha(0.5f), 14, { 0, 4 }));
}

std::unique_ptr<juce::FocusOutline> Controls::LookAndFeel::createFocusOutlineForComponent(juce::Component&)
{
    struct Properties final : public juce::FocusOutline::OutlineWindowProperties
    {
        juce::Rectangle<int> getOutlineBounds(juce::Component& c) override { return c.getScreenBounds().expanded(2); }

        void drawOutline(juce::Graphics& g, int width, int height) override
        {
            g.setColour(reverser);
            g.drawRoundedRectangle(juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height)).reduced(0.75f), 5.0f, 1.5f);
        }
    };

    return std::make_unique<juce::FocusOutline>(std::make_unique<Properties>());
}

//==============================================================================
// ModeSelector
//==============================================================================
Controls::ModeSelector::Lamp::Lamp(const juce::String& title, const juce::String& subtitle, const Theme::Fonts& fonts)
    : juce::Button(title)
    , m_title(title)
    , m_subtitle(subtitle)
    , m_fonts(fonts)
{
    setClickingTogglesState(false);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void Controls::ModeSelector::Lamp::paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown)
{
    using namespace Theme::Colours;

    const bool on = getToggleState();
    auto r = getLocalBounds().toFloat().reduced(2.0f);

    if (on || isMouseOver)
    {
        g.setColour(on ? panelRaised : panelRaised.withAlpha(0.6f));
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(on ? mode.withAlpha(0.35f) : outlineSoft);
        g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, 1.0f);
    }

    // Lamp
    auto lampArea = r.removeFromLeft(54.0f);
    const float d = std::min(26.0f, lampArea.getHeight() - 10.0f);
    const auto lamp = juce::Rectangle<float>(d, d).withCentre(lampArea.getCentre());

    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillEllipse(lamp.expanded(3.0f).translated(0.0f, 1.0f));
    g.setColour(outline.brighter(0.25f));
    g.fillEllipse(lamp.expanded(2.5f));

    if (on)
    {
        for (int i = 3; i >= 1; --i)
        {
            g.setColour(mode.withAlpha(0.07f * static_cast<float>(4 - i)));
            g.fillEllipse(lamp.expanded(static_cast<float>(i) * 3.5f));
        }
        g.setGradientFill(juce::ColourGradient(mode.brighter(0.7f), lamp.getCentreX(), lamp.getY() + d * 0.25f,
                                               mode.darker(0.45f), lamp.getCentreX(), lamp.getBottom(), true));
    }
    else
    {
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff3a342c), lamp.getCentreX(), lamp.getY(),
                                               juce::Colour(0xff1f1b16), lamp.getCentreX(), lamp.getBottom(), false));
    }
    g.fillEllipse(lamp);

    // Specular highlight
    g.setColour(juce::Colours::white.withAlpha(on ? 0.55f : (isButtonDown ? 0.2f : 0.12f)));
    g.fillEllipse(lamp.withSizeKeepingCentre(d * 0.45f, d * 0.22f).translated(0.0f, -d * 0.25f));

    // Text (Button has a member called "text", so qualify the colour)
    r.removeFromRight(6.0f);
    auto textArea = r.withSizeKeepingCentre(r.getWidth(), 34.0f);
    const auto& textColour = Theme::Colours::text;
    g.setColour(on ? textColour : (isMouseOver ? textColour.withAlpha(0.85f) : textDim));
    g.setFont(m_fonts.heading(11.5f));
    g.drawFittedText(m_title.toUpperCase(), textArea.removeFromTop(17.0f).toNearestInt(), juce::Justification::centredLeft, 1);
    g.setColour(on ? textDim : textFaint);
    g.setFont(m_fonts.body(10.5f));
    g.drawFittedText(m_subtitle, textArea.toNearestInt(), juce::Justification::centredLeft, 1);

    if (Controls::LookAndFeel::shouldShowFocusRing(*this))
    {
        g.setColour(reverser);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(2.75f), 8.0f, 1.5f);
    }
}

Controls::ModeSelector::ModeSelector(juce::RangedAudioParameter& modeParameter, const Theme::Fonts& fonts)
    : m_attachment(modeParameter, [this](float value) { select(juce::roundToInt(value)); })
{
    const std::array<std::pair<const char*, const char*>, 3> modes { {
        { "Reverse Playback",  "Overlapping backward snippets" },
        { "Forward Backwards", "Fades forward into backward" },
        { "Reverse Repeat",    "Each snippet twice, + vibrato" },
    } };

    for (size_t i = 0; i < m_lamps.size(); ++i)
    {
        m_lamps[i] = std::make_unique<Lamp>(modes[i].first, modes[i].second, fonts);
        m_lamps[i]->onClick = [this, i] { m_attachment.setValueAsCompleteGesture(static_cast<float>(i)); };
        addAndMakeVisible(*m_lamps[i]);
    }

    m_lamps[0]->setTooltip("Reverse Playback: reverses and overlaps snippets of the incoming audio for an ongoing backwards effect.");
    m_lamps[1]->setTooltip("Forward Backwards: smoothly cross-fades between forward and backward versions of the incoming audio.");
    m_lamps[2]->setTooltip("Reverse Repeat: plays each snippet backwards twice. The second time, a slight vibrato is added.");

    m_attachment.sendInitialUpdate();
}

void Controls::ModeSelector::select(int index)
{
    for (size_t i = 0; i < m_lamps.size(); ++i)
        m_lamps[i]->setToggleState(static_cast<int>(i) == index, juce::dontSendNotification);
}

void Controls::ModeSelector::resized()
{
    auto area = getLocalBounds();
    const int rowHeight = area.getHeight() / static_cast<int>(m_lamps.size());

    for (auto& lamp : m_lamps)
        lamp->setBounds(area.removeFromTop(rowHeight));
}

//==============================================================================
// ReverserButton
//==============================================================================
Controls::ReverserButton::ReverserButton()
    : juce::Button("Reverser")
{
    setClickingTogglesState(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void Controls::ReverserButton::paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown)
{
    using namespace Theme::Colours;

    const bool on = getToggleState();
    const auto bounds = getLocalBounds().toFloat();
    const float d = std::min(bounds.getWidth(), bounds.getHeight()) - 10.0f;
    const auto body = juce::Rectangle<float>(d, d).withCentre(bounds.getCentre());

    // Glow when reversing
    if (on)
    {
        for (int i = 3; i >= 1; --i)
        {
            g.setColour(reverser.withAlpha(0.06f * static_cast<float>(4 - i)));
            g.fillEllipse(body.expanded(static_cast<float>(i) * 2.0f));
        }
    }

    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.fillEllipse(body.translated(0.0f, 2.0f));

    // Bezel and face
    g.setGradientFill(juce::ColourGradient(panelRaised.brighter(0.55f), body.getCentreX(), body.getY(),
                                           panelRaised.darker(0.3f), body.getCentreX(), body.getBottom(), false));
    g.fillEllipse(body);

    const auto face = body.reduced(d * 0.09f);
    g.setGradientFill(juce::ColourGradient(panelRaised.brighter(isButtonDown ? 0.05f : (isMouseOver ? 0.28f : 0.2f)),
                                           face.getCentreX(), face.getY(),
                                           panelRaised.darker(0.15f), face.getCentreX(), face.getBottom(), false));
    g.fillEllipse(face);
    g.setColour(on ? reverser.withAlpha(0.9f) : outline.brighter(0.2f));
    g.drawEllipse(face, on ? 1.6f : 1.0f);

    // Arrow: left while reversing, right when playing forwards.
    const float s = face.getWidth() * 0.22f;
    const auto c = face.getCentre();
    juce::Path arrow;
    if (on) arrow.addTriangle(c.x - s * 1.05f, c.y, c.x + s * 0.75f, c.y - s, c.x + s * 0.75f, c.y + s);
    else    arrow.addTriangle(c.x + s * 1.05f, c.y, c.x - s * 0.75f, c.y - s, c.x - s * 0.75f, c.y + s);

    g.setColour(on ? reverser : textDim);
    g.strokePath(arrow, juce::PathStrokeType(2.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
    if (on)
    {
        g.setColour(reverser.withAlpha(0.25f));
        g.fillPath(arrow);
    }

    if (Controls::LookAndFeel::shouldShowFocusRing(*this))
    {
        g.setColour(reverser);
        g.drawEllipse(body.expanded(3.5f), 1.5f);
    }
}

//==============================================================================
void Controls::drawMirroredText(juce::Graphics& g, const juce::String& line, const juce::Font& font,
                                juce::Rectangle<float> area, const juce::Array<int>& mirroredIndices)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText(font, line, 0.0f, 0.0f);

    const auto box = glyphs.getBoundingBox(0, -1, true);
    const float dx = area.getCentreX() - box.getCentreX();
    const float dy = area.getCentreY() - box.getCentreY();

    for (int i = 0; i < glyphs.getNumGlyphs(); ++i)
    {
        const auto& glyph = glyphs.getGlyph(i);
        auto transform = juce::AffineTransform::translation(dx, dy);

        if (mirroredIndices.contains(i))
        {
            // Pivot on the visible outline, not the (kerned) advance, so the glyph stays in place.
            juce::Path glyphOutline;
            glyph.createPath(glyphOutline);
            const float pivot = glyphOutline.isEmpty() ? glyph.getLeft() + glyph.getBounds().getWidth() * 0.5f
                                                       : glyphOutline.getBounds().getCentreX();
            transform = juce::AffineTransform::scale(-1.0f, 1.0f, pivot, 0.0f).followedBy(transform);
        }

        glyph.draw(g, transform);
    }
}

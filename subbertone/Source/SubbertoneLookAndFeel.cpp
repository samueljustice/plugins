#include "SubbertoneLookAndFeel.h"

namespace
{
    using namespace Theme::Colours;

    // Popups dropping from the editor follow its scale, but never get smaller than at its default size.
    constexpr float c_minimumPopupScale = 0.8f;

    constexpr float c_menuRadius = 6.0f;          // design-size menu metrics, scaled by menuScale()
    constexpr float c_menuBorder = 5.0f;
    constexpr float c_menuTickColumn = 22.0f;
    constexpr float c_menuTextInset = 24.0f;
    constexpr float c_menuArrowColumn = 20.0f;
    constexpr float c_menuHeaderHeight = 26.0f;
    constexpr float c_menuSeparatorHeight = 9.0f;
    constexpr float c_menuFontHeight = 13.0f;

    constexpr float c_dialogRadius = 8.0f;        // the editor's panel radius
    constexpr float c_dialogHeaderHeight = 32.0f; // the editor's section header height
    constexpr int c_dialogButtonSlot = 40;        // JUCE lays out this height; the pill is c_dialogButtonHeight
    constexpr float c_dialogButtonHeight = 28.0f;
    constexpr int c_dialogButtonMinWidth = 96;

    constexpr float c_tipRadius = 5.0f;
    constexpr float c_tipPadX = 10.0f, c_tipPadY = 7.0f;
    constexpr float c_tipMaxWidth = 420.0f;

    const juce::Identifier c_focusRingsProperty { "subbertoneFocusRings" };
    const juce::Identifier c_capitalsProperty { "subbertoneCapitals" };

    // A colour that looks opaque but makes JUCE create a transparent window (menus), so the rounded corners
    // show what is behind them. The body itself is drawn with full alpha.
    juce::Colour nearlyOpaque(juce::Colour c) { return c.withAlpha(static_cast<juce::uint8>(0xfe)); }

    // Width of a line of text as drawn: getStringWidth() skips ligature placeholders, so text ending in a
    // ligature (the body font joins "ff", "fi", ...) would measure short.
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

    // A ">" chevron (as between the editor's sections), rotated by angle.
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
        return h > 0 ? static_cast<float>(h) / static_cast<float>(SubbertoneLookAndFeel::c_menuItemHeight) : 1.0f;
    }

    // The accent of a menu: the colour of the control it drops from; amber for everything else.
    juce::Colour menuAccent(const juce::PopupMenu::Options& options)
    {
        if (auto* target = options.getTargetComponent())
        {
            if (target->isColourSpecified(juce::PopupMenu::highlightedBackgroundColourId))
                return target->findColour(juce::PopupMenu::highlightedBackgroundColourId).withAlpha(1.0f);

            if (auto* box = dynamic_cast<juce::ComboBox*>(target))
                return box->findColour(juce::ComboBox::arrowColourId);
        }
        return output;
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
        return r.reduced(0.5f);
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
        return { background, panelRaised, panelRaised, outline, text, output, text, output.withAlpha(0.3f), text };
    }
}

//==============================================================================
SubbertoneLookAndFeel::SubbertoneLookAndFeel(const Theme::Fonts& fonts)
    : juce::LookAndFeel_V4(themeScheme())
    , m_fonts(fonts)
{
    const auto transparent = juce::Colours::transparentBlack;

    if (fonts.medium != nullptr)
        setDefaultSansSerifTypeface(fonts.medium);

    setColour(juce::ResizableWindow::backgroundColourId, background);
    setColour(juce::DocumentWindow::textColourId, text);
    setColour(juce::Label::textColourId, text);

    setColour(juce::Slider::textBoxTextColourId, text);
    setColour(juce::Slider::textBoxOutlineColourId, transparent);
    setColour(juce::Slider::textBoxBackgroundColourId, transparent);
    setColour(juce::Slider::textBoxHighlightColourId, output.withAlpha(0.3f));

    setColour(juce::ComboBox::backgroundColourId, panelRaised);
    setColour(juce::ComboBox::outlineColourId, outline);
    setColour(juce::ComboBox::textColourId, text);
    setColour(juce::ComboBox::arrowColourId, harmonics);
    setColour(juce::ComboBox::focusedOutlineColourId, harmonics.withAlpha(0.6f));

    setColour(juce::PopupMenu::backgroundColourId, nearlyOpaque(panelRaised));
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::headerTextColourId, textDim);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, output.withAlpha(0.16f));
    setColour(juce::PopupMenu::highlightedTextColourId, text);

    setColour(juce::TextButton::buttonColourId, panelRaised);
    setColour(juce::TextButton::buttonOnColourId, panelRaised.brighter(0.1f));
    setColour(juce::TextButton::textColourOffId, textDim);
    setColour(juce::TextButton::textColourOnId, text);

    setColour(juce::HyperlinkButton::textColourId, textDim);

    setColour(juce::AlertWindow::backgroundColourId, panel);
    setColour(juce::AlertWindow::textColourId, text);
    setColour(juce::AlertWindow::outlineColourId, outline);

    setColour(juce::TextEditor::backgroundColourId, panelRaised);
    setColour(juce::TextEditor::textColourId, text);
    setColour(juce::TextEditor::outlineColourId, outline);
    setColour(juce::TextEditor::focusedOutlineColourId, output.withAlpha(0.75f));
    setColour(juce::TextEditor::highlightColourId, output.withAlpha(0.3f));
    setColour(juce::TextEditor::highlightedTextColourId, text);
    setColour(juce::TextEditor::shadowColourId, transparent);
    setColour(juce::CaretComponent::caretColourId, output);

    setColour(juce::ScrollBar::thumbColourId, textFaint);
    setColour(juce::ScrollBar::trackColourId, transparent);
    setColour(juce::ScrollBar::backgroundColourId, transparent);

    setColour(juce::TooltipWindow::backgroundColourId, panelRaised);
    setColour(juce::TooltipWindow::textColourId, text);
    setColour(juce::TooltipWindow::outlineColourId, outline);
}

//==============================================================================
juce::PopupMenu::Options SubbertoneLookAndFeel::menuOptionsFor(juce::Component& target, int minimumWidth)
{
    return juce::PopupMenu::Options().withTargetComponent(&target)
                                     .withMinimumWidth(minimumWidth)
                                     .withStandardItemHeight(juce::roundToInt(static_cast<float>(c_menuItemHeight) * legibilityFactor(target)));
}

void SubbertoneLookAndFeel::showFocusRingsAfterTab(juce::Component& window)
{
    window.addKeyListener(&focusRingTracker());
    window.addMouseListener(&focusRingTracker(), true);
}

bool SubbertoneLookAndFeel::shouldShowFocusRing(const juce::Component& c)
{
    if (!c.hasKeyboardFocus(false))
        return false;

    const auto* window = c.getTopLevelComponent();
    return static_cast<bool>(window->getProperties().getWithDefault(c_focusRingsProperty, false));
}

void SubbertoneLookAndFeel::setLabelDrawnInCapitals(juce::Button& button)
{
    button.getProperties().set(c_capitalsProperty, true);
    button.repaint();
}

juce::Colour SubbertoneLookAndFeel::accentFor(juce::MessageBoxIconType type)
{
    switch (type)
    {
        case juce::MessageBoxIconType::WarningIcon:  return bad;
        case juce::MessageBoxIconType::InfoIcon:     return input;
        case juce::MessageBoxIconType::QuestionIcon:
        case juce::MessageBoxIconType::NoIcon:
        default:                                     return output;
    }
}

void SubbertoneLookAndFeel::drawMark(juce::Graphics& g, juce::Rectangle<float> mark)
{
    // As in the editor header: a 30 x 22 mark, scaled to the area.
    const float sx = mark.getWidth() / 30.0f, sy = mark.getHeight() / 22.0f;
    juce::Path inputWave, subWave;
    for (int i = 0; i <= 40; ++i)
    {
        const float t = static_cast<float>(i) / 40.0f;
        const float x = mark.getX() + t * mark.getWidth();
        const juce::Point<float> a { x, mark.getCentreY() + sy * (-5.0f + 4.0f * std::sin(t * juce::MathConstants<float>::twoPi * 2.0f)) };
        const juce::Point<float> b { x, mark.getCentreY() + sy * (4.0f + 5.0f * std::sin(t * juce::MathConstants<float>::twoPi)) };
        if (i == 0) { inputWave.startNewSubPath(a); subWave.startNewSubPath(b); }
        else        { inputWave.lineTo(a);          subWave.lineTo(b); }
    }
    const float stroke = std::min(sx, sy);
    g.setColour(input.withAlpha(0.8f));
    g.strokePath(inputWave, juce::PathStrokeType(1.5f * stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(output);
    g.strokePath(subWave, juce::PathStrokeType(2.2f * stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Rectangle<float> SubbertoneLookAndFeel::drawPanel(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title,
                                                        juce::Colour accent, float headerHeight) const
{
    // The editor's section panels (PluginEditor::paintContent): fill, soft outline, dot, title, divider.
    g.setColour(panel);
    g.fillRoundedRectangle(r, 8.0f);
    g.setColour(outlineSoft);
    g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, 1.0f);

    auto headerRow = r.withHeight(headerHeight).reduced(12.0f, 0.0f);
    g.setColour(accent);
    g.fillEllipse(headerRow.removeFromLeft(6.0f).withSizeKeepingCentre(6.0f, 6.0f));
    headerRow.removeFromLeft(8.0f);
    g.setFont(m_fonts.heading(11.0f));
    g.setColour(text.withAlpha(0.85f));
    g.drawText(title, headerRow, juce::Justification::centredLeft, false);

    if (headerHeight < r.getHeight() - 1.0f) // a panel that is only a header row has no divider
    {
        g.setColour(outlineSoft);
        g.fillRect(juce::Rectangle<float>(r.getX() + 12.0f, std::round(r.getY() + headerHeight), r.getWidth() - 24.0f, 1.0f));
    }
    return headerRow;
}

//==============================================================================
// Knobs
//==============================================================================
void SubbertoneLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                             float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
{
    const auto accent = slider.findColour(juce::Slider::rotarySliderFillColourId);
    const bool hot = slider.isMouseOverOrDragging();

    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(4.0f);
    const float diameter = std::min(area.getWidth(), area.getHeight());
    const auto centre = area.getCentre();
    const float radius = diameter * 0.5f;
    const float trackWidth = std::max(2.5f, radius * 0.09f);
    const float arcRadius = radius - trackWidth * 0.5f;
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // Track
    juce::Path track;
    track.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(outline);
    g.strokePath(track, juce::PathStrokeType(trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc: bipolar parameters (dB gain) grow from the centre.
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0 && juce::approximatelyEqual(slider.getMinimum(), -slider.getMaximum());
    const float fromAngle = bipolar ? (rotaryStartAngle + rotaryEndAngle) * 0.5f : rotaryStartAngle;

    juce::Path value;
    value.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, std::min(fromAngle, angle), std::max(fromAngle, angle), true);
    g.setColour(accent.withAlpha(hot ? 1.0f : 0.9f));
    g.strokePath(value, juce::PathStrokeType(trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Body
    const float bodyRadius = arcRadius - trackWidth * 1.6f;
    const auto body = juce::Rectangle<float>(bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre(centre);

    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillEllipse(body.translated(0.0f, 2.0f));

    g.setGradientFill(juce::ColourGradient(panelRaised.brighter(hot ? 0.18f : 0.1f), centre.x, body.getY(),
                                           panelRaised.darker(0.25f), centre.x, body.getBottom(), false));
    g.fillEllipse(body);
    g.setColour(outline.brighter(0.15f));
    g.drawEllipse(body.reduced(0.5f), 1.0f);

    // Pointer
    const auto direction = juce::Point<float>(std::sin(angle), -std::cos(angle));
    const auto p1 = centre + direction * (bodyRadius * 0.35f);
    const auto p2 = centre + direction * (bodyRadius * 0.82f);
    g.setColour(text);
    g.drawLine({ p1, p2 }, 2.0f);
}

juce::Slider::SliderLayout SubbertoneLookAndFeel::getSliderLayout(juce::Slider& slider)
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

juce::Label* SubbertoneLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
    label->setFont(m_fonts.body(12.0f));
    label->setJustificationType(juce::Justification::centred);
    label->setColour(juce::Label::textColourId, text);
    label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

    // While a value is typed: a rounded field in the knob's own accent (outline, selection, caret), with the
    // text centred like the value it replaces. Label::createEditorComponent() copies these to its TextEditor.
    const auto accent = slider.findColour(juce::Slider::rotarySliderFillColourId);
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

juce::Font SubbertoneLookAndFeel::getLabelFont(juce::Label& label)
{
    // Knob value boxes show values; every other editor label is a caption. A label whose code set one of the
    // theme's fonts keeps it (e.g. the About window's status line); JUCE's default label font never shows.
    if (dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr)
        return m_fonts.body(12.0f);

    if (dynamic_cast<juce::ComboBox*>(label.getParentComponent()) == nullptr)
    {
        const auto face = label.getFont().getTypefacePtr();
        if (face != nullptr && (face == m_fonts.regular || face == m_fonts.medium || face == m_fonts.bold))
            return label.getFont();
    }

    return m_fonts.label(10.5f);
}

//==============================================================================
// Combo boxes
//==============================================================================
void SubbertoneLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool,
                                         int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    const bool hot = box.isMouseOver(true) || box.isPopupActive();
    const bool ring = shouldShowFocusRing(box);

    g.setColour(box.findColour(juce::ComboBox::backgroundColourId).brighter(hot ? 0.08f : 0.0f));
    g.fillRoundedRectangle(r, 5.0f);

    if (ring)
    {
        g.setColour(output);
        g.drawRoundedRectangle(r.reduced(0.25f), 5.0f, 1.5f);
    }
    else
    {
        g.setColour(hot ? box.findColour(juce::ComboBox::focusedOutlineColourId) : box.findColour(juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle(r, 5.0f, 1.0f);
    }

    const float cx = r.getRight() - 13.0f;
    const float cy = r.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath(cx - 4.0f, cy - 2.0f);
    chevron.lineTo(cx, cy + 2.0f);
    chevron.lineTo(cx + 4.0f, cy - 2.0f);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId));
    g.strokePath(chevron, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font SubbertoneLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return m_fonts.body(12.0f);
}

void SubbertoneLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(8, 0, box.getWidth() - 30, box.getHeight());
    label.setFont(getComboBoxFont(box));
}

juce::PopupMenu::Options SubbertoneLookAndFeel::getOptionsForComboBoxPopupMenu(juce::ComboBox& box, juce::Label& label)
{
    // JUCE's options (rows as high as the box, at least as wide), with the same legibility floor as the
    // editor's own menus.
    return juce::LookAndFeel_V4::getOptionsForComboBoxPopupMenu(box, label)
               .withStandardItemHeight(juce::roundToInt(static_cast<float>(label.getHeight()) * legibilityFactor(box)));
}

//==============================================================================
// Buttons
//==============================================================================
bool SubbertoneLookAndFeel::isPrimaryDialogButton(const juce::Button& button)
{
    return isInAlert(button) && button.isRegisteredForShortcut(juce::KeyPress(juce::KeyPress::returnKey));
}

void SubbertoneLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                 const juce::Colour& backgroundColour,
                                                 bool isMouseOverButton, bool isButtonDown)
{
    if (backgroundColour.isTransparent())
        return; // a text link: no pill (see drawButtonText)

    const auto r = pillBounds(button);
    const float corner = std::min(r.getWidth(), r.getHeight()) * 0.5f;
    const bool enabled = button.isEnabled();

    auto fill = backgroundColour;
    auto edge = isMouseOverButton && enabled ? textDim : outline;

    if (auto* alert = dynamic_cast<juce::AlertWindow*>(button.getParentComponent()))
    {
        // The Return-key button is the dialog's primary action, tinted like LIMIT / LO CUT when on.
        if (isPrimaryDialogButton(button))
        {
            const auto accent = accentFor(alert->getAlertType());
            fill = accent.withAlpha(0.28f);
            edge = isMouseOverButton ? accent : accent.withAlpha(0.6f);
        }
    }

    if (!enabled)
        fill = fill.withMultipliedAlpha(0.5f);

    g.setColour(fill.brighter(isButtonDown ? 0.2f : isMouseOverButton && enabled ? 0.1f : 0.0f));
    g.fillRoundedRectangle(r, corner);

    if (shouldShowFocusRing(button))
    {
        auto* alert = dynamic_cast<juce::AlertWindow*>(button.getParentComponent());
        g.setColour(alert != nullptr ? accentFor(alert->getAlertType()) : output);
        g.drawRoundedRectangle(r.reduced(0.25f), corner, 1.5f);
    }
    else
    {
        g.setColour(enabled ? edge : outline.withMultipliedAlpha(0.6f));
        g.drawRoundedRectangle(r, corner, 1.0f);
    }
}

void SubbertoneLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool isMouseOverButton, bool isButtonDown)
{
    const auto font = getTextButtonFont(button, button.getHeight());
    const bool capitals = isInAlert(button) || static_cast<bool>(button.getProperties().getWithDefault(c_capitalsProperty, false));
    const auto label = capitals ? button.getButtonText().toUpperCase() : button.getButtonText();

    if (isLinkStyle(button))
    {
        // A text link: dim (or its own colour) when idle, amber under the mouse, underlined on hover or focus.
        auto colour = button.findColour(juce::TextButton::textColourOffId);
        if (!button.isEnabled())
            colour = textFaint;
        else if (isButtonDown)
            colour = output.darker(0.15f);
        else if (isMouseOverButton)
            colour = output;

        const auto area = button.getLocalBounds().toFloat();
        const auto shown = fitToWidth(font, label, area.getWidth());
        g.setFont(font);
        g.setColour(colour);
        g.drawText(shown, area, juce::Justification::centred, false);

        if (button.isEnabled() && (isMouseOverButton || shouldShowFocusRing(button)))
        {
            const float w = lineWidth(font, shown);
            const float y = std::round(area.getCentreY() + font.getAscent() * 0.5f + 2.0f);
            g.setColour(shouldShowFocusRing(button) ? output : colour);
            g.fillRect(juce::Rectangle<float>(area.getCentreX() - w * 0.5f, y, w, 1.0f));
        }
        return;
    }

    auto colour = button.findColour(button.getToggleState() || isPrimaryDialogButton(button) ? juce::TextButton::textColourOnId
                                                                                            : juce::TextButton::textColourOffId);
    if (!button.isEnabled())
        colour = textFaint;

    // The label keeps clear of the pill's rounded ends, but a short label in a small pill (the preset bar's
    // "<" and ">", "?", CHOOSE) takes the room it needs: a label is only cut when the pill is too narrow for it.
    // (Graphics::drawText drops a word that overruns its area, hence the pixel of slack.)
    const auto r = pillBounds(button);
    const float preferredPad = std::max(4.0f, std::round(r.getHeight() * 0.35f));
    const float pad = juce::jlimit(1.0f, preferredPad, (r.getWidth() - lineWidth(font, label)) * 0.5f - 1.0f);
    const auto area = r.reduced(pad, 0.0f);

    g.setFont(font);
    g.setColour(colour);
    g.drawText(fitToWidth(font, label, area.getWidth()), area, juce::Justification::centred, false);
}

juce::Font SubbertoneLookAndFeel::getTextButtonFont(juce::TextButton& button, int buttonHeight)
{
    if (isInAlert(button))
        return m_fonts.label(12.0f);
    if (isLinkStyle(button))
        return m_fonts.body(12.0f);
    return m_fonts.label(std::min(13.0f, static_cast<float>(buttonHeight) * 0.5f));
}

//==============================================================================
// Popup menus
//==============================================================================
juce::Font SubbertoneLookAndFeel::menuFont(float scale) const
{
    return m_fonts.body(c_menuFontHeight * scale);
}

juce::Font SubbertoneLookAndFeel::getPopupMenuFont()
{
    return menuFont(1.0f);
}

int SubbertoneLookAndFeel::getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options& options)
{
    return std::max(2, juce::roundToInt(c_menuBorder * menuScale(options)));
}

void SubbertoneLookAndFeel::drawPopupMenuBackgroundWithOptions(juce::Graphics& g, int width, int height,
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
    g.setColour(outline);
    g.drawRoundedRectangle(r.reduced(0.5f), c_menuRadius * s, 1.0f);
}

void SubbertoneLookAndFeel::drawPopupMenuItemWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
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
        g.setColour(!enabled ? textFaint : highlighted ? text : textDim);
        strokeChevron(g, arrowArea.getCentre().translated(-2.0f * s, 0.0f), 3.5f * s, 0.0f, 1.4f * s);
    }

    // Text, with a trailing "(...)" qualifier in the dim colour.
    const auto font = menuFont(s);
    const auto main = !enabled ? textFaint
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

void SubbertoneLookAndFeel::drawPopupMenuSectionHeaderWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
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

void SubbertoneLookAndFeel::getIdealPopupMenuItemSizeWithOptions(const juce::String& itemText, bool isSeparator,
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

void SubbertoneLookAndFeel::getIdealPopupMenuSectionHeaderSizeWithOptions(const juce::String& headerText, int,
                                                                          int& idealWidth, int& idealHeight,
                                                                          const juce::PopupMenu::Options& options)
{
    const float s = menuScale(options);
    idealHeight = juce::roundToInt(c_menuHeaderHeight * s);
    idealWidth = juce::roundToInt(lineWidth(m_fonts.heading(10.5f * s), headerText) + (c_menuTextInset + 12.0f) * s);
}

void SubbertoneLookAndFeel::drawPopupMenuUpDownArrowWithOptions(juce::Graphics& g, int width, int height,
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

int SubbertoneLookAndFeel::getPopupMenuColumnSeparatorWidthWithOptions(const juce::PopupMenu::Options&)
{
    return 1;
}

void SubbertoneLookAndFeel::drawPopupMenuColumnSeparatorWithOptions(juce::Graphics& g, const juce::Rectangle<int>& bounds,
                                                                    const juce::PopupMenu::Options& options)
{
    g.setColour(outline);
    g.fillRect(bounds.toFloat().reduced(0.0f, 6.0f * menuScale(options)));
}

//==============================================================================
// Alerts
//==============================================================================
juce::AlertWindow* SubbertoneLookAndFeel::createAlertWindow(const juce::String& title, const juce::String& message,
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

void SubbertoneLookAndFeel::drawAlertBox(juce::Graphics& g, juce::AlertWindow& alert,
                                         const juce::Rectangle<int>&, juce::TextLayout& textLayout)
{
    const auto bounds = alert.getLocalBounds().toFloat();
    const auto accent = accentFor(alert.getAlertType());

    if (alert.isOpaque())
        g.fillAll(background); // an opaque window can't show what is behind its rounded corners

    // A panel, as the editor's sections: no icon.
    g.setColour(alert.findColour(juce::AlertWindow::backgroundColourId));
    g.fillRoundedRectangle(bounds, c_dialogRadius);
    g.setColour(alert.findColour(juce::AlertWindow::outlineColourId));
    g.drawRoundedRectangle(bounds.reduced(0.5f), c_dialogRadius, 1.0f);

    // Header row: accent dot, the title in capitals, a divider.
    auto header = bounds.withHeight(c_dialogHeaderHeight).reduced(14.0f, 0.0f);
    g.setColour(accent);
    g.fillEllipse(header.removeFromLeft(6.0f).withSizeKeepingCentre(6.0f, 6.0f));
    header.removeFromLeft(8.0f);
    const auto titleFont = m_fonts.heading(11.5f);
    g.setFont(titleFont);
    g.setColour(text.withAlpha(0.85f));
    g.drawText(fitToWidth(titleFont, alert.getName().toUpperCase(), header.getWidth()), header, juce::Justification::centredLeft, false);
    g.setColour(outlineSoft);
    g.fillRect(juce::Rectangle<float>(12.0f, c_dialogHeaderHeight, bounds.getWidth() - 24.0f, 1.0f));

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
            const bool isTitle = run->font.getTypefacePtr() == m_fonts.bold;
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

int SubbertoneLookAndFeel::getAlertWindowButtonHeight()
{
    return c_dialogButtonSlot;
}

juce::Array<int> SubbertoneLookAndFeel::getWidthsForTextButtons(juce::AlertWindow&, const juce::Array<juce::TextButton*>& buttons)
{
    // One width for every button of a dialog, so the row reads as a set.
    const auto font = m_fonts.label(12.0f);
    int width = c_dialogButtonMinWidth;
    for (auto* b : buttons)
        width = std::max(width, juce::roundToInt(lineWidth(font, b->getButtonText().toUpperCase())) + 44);

    juce::Array<int> widths;
    widths.insertMultiple(0, width, buttons.size());
    return widths;
}

juce::Font SubbertoneLookAndFeel::getAlertWindowTitleFont()   { return m_fonts.heading(11.5f); }
juce::Font SubbertoneLookAndFeel::getAlertWindowMessageFont() { return m_fonts.body(12.5f); }
juce::Font SubbertoneLookAndFeel::getAlertWindowFont()        { return m_fonts.label(10.5f); }

//==============================================================================
// Text fields
//==============================================================================
void SubbertoneLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
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

void SubbertoneLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (!editor.isEnabled())
        return;

    // A field being typed in shows its focus however it got it; read-only text (the About licence) only
    // when Tab moved the focus there, like the buttons.
    const bool focused = editor.hasKeyboardFocus(true) && (!editor.isReadOnly() || shouldShowFocusRing(editor));
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
// Scrollbars
//==============================================================================
void SubbertoneLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar, int x, int y, int width, int height,
                                          bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                                          bool isMouseOver, bool isMouseDown)
{
    if (thumbSize <= 0)
        return;

    // No track: a thin pill, like the editor's meter tracks.
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
juce::TextLayout SubbertoneLookAndFeel::layoutTooltip(const juce::String& tipText, juce::Colour colour) const
{
    juce::AttributedString s;
    s.setJustification(juce::Justification::topLeft);
    s.setLineSpacing(2.0f);
    s.append(tipText, m_fonts.body(11.5f), colour);

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

juce::Rectangle<int> SubbertoneLookAndFeel::getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
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

void SubbertoneLookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& tipText, int width, int height)
{
    // The editor's tooltip window is transparent outside this rounded panel.
    const auto r = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    const auto fill = findColour(juce::TooltipWindow::backgroundColourId);

    if (!juce::Desktop::canUseSemiTransparentWindows())
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
void SubbertoneLookAndFeel::drawCornerResizer(juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
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

std::unique_ptr<juce::DropShadower> SubbertoneLookAndFeel::createDropShadowerForComponent(juce::Component&)
{
    return std::make_unique<juce::DropShadower>(juce::DropShadow(juce::Colours::black.withAlpha(0.5f), 14, { 0, 4 }));
}

std::unique_ptr<juce::FocusOutline> SubbertoneLookAndFeel::createFocusOutlineForComponent(juce::Component&)
{
    struct Properties final : public juce::FocusOutline::OutlineWindowProperties
    {
        juce::Rectangle<int> getOutlineBounds(juce::Component& c) override { return c.getScreenBounds().expanded(2); }

        void drawOutline(juce::Graphics& g, int width, int height) override
        {
            g.setColour(output);
            g.drawRoundedRectangle(juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height)).reduced(0.75f), 5.0f, 1.5f);
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

void ThemeLink::setHoverColour(juce::Colour colour)
{
    m_hoverColour = colour;
    repaint();
}

void ThemeLink::paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown)
{
    auto colour = findColour(juce::HyperlinkButton::textColourId);
    if (!isEnabled())
        colour = colour.withMultipliedAlpha(0.4f);
    else if (isButtonDown)
        colour = m_hoverColour.darker(0.15f);
    else if (isMouseOverButton)
        colour = m_hoverColour;

    const auto area = getLocalBounds().reduced(1, 0).toFloat();
    g.setColour(colour);
    g.setFont(m_font);
    g.drawText(getButtonText(), area, m_justification.getOnlyHorizontalFlags() | juce::Justification::verticallyCentred, true);

    if (SubbertoneLookAndFeel::shouldShowFocusRing(*this))
    {
        // Keyboard focus: an amber underline under the text.
        const float w = std::min(area.getWidth(), juce::GlyphArrangement::getStringWidth(m_font, getButtonText()));
        const float x = m_justification.testFlags(juce::Justification::left) ? area.getX()
                      : m_justification.testFlags(juce::Justification::right) ? area.getRight() - w
                      : area.getCentreX() - w * 0.5f;
        g.setColour(Theme::Colours::output);
        g.fillRect(juce::Rectangle<float>(x, std::round(area.getCentreY() + m_font.getAscent() * 0.5f + 2.0f), w, 1.0f));
    }
}

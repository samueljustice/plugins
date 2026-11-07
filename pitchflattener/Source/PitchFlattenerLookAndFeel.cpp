#include "PitchFlattenerLookAndFeel.h"

namespace
{
    using namespace Theme::Colours;

    // Chrome that belongs to no section: focus rings, selection, caret, links, menus that drop from no box.
    const juce::Colour c_chrome = target;

    // Popups dropping from the editor follow its scale, but never get smaller than at its default size (1:1).
    constexpr float c_minimumPopupScale = 1.0f;

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

    constexpr float c_disabledAlpha = 0.4f;       // Pitch Flattener dims a disabled control as a whole

    const juce::Identifier c_focusRingsProperty { "pitchFlattenerFocusRings" };
    const juce::Identifier c_capitalsProperty { "pitchFlattenerCapitals" };

    // A colour that looks opaque but makes JUCE create a transparent window (menus), so the rounded corners
    // show what is behind them. The body itself is drawn with full alpha.
    juce::Colour nearlyOpaque (juce::Colour c) { return c.withAlpha ((juce::uint8) 0xfe); }

    // Width of a line of text as drawn: getStringWidth() skips ligature placeholders, so text ending in a
    // ligature would measure short.
    float lineWidth (const juce::Font& font, const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth (font, text + " ") - juce::GlyphArrangement::getStringWidth (font, " ");
    }

    // One line, never squashed: too long gets "...", keeping a trailing " *" (an edited marker).
    juce::String fitToWidth (const juce::Font& font, const juce::String& text, float width)
    {
        width -= 1.0f; // Graphics::drawText() wraps away (drops) a word that overruns its area by any amount

        if (lineWidth (font, text) <= width)
            return text;

        const juce::String suffix = text.endsWith (" *") ? " *" : "";
        auto stem = text.dropLastCharacters (suffix.length());
        while (stem.isNotEmpty() && lineWidth (font, stem.trimEnd() + "..." + suffix) > width)
            stem = stem.dropLastCharacters (1);
        return stem.trimEnd() + "..." + suffix;
    }

    // A ">" chevron, rotated by angle.
    void strokeChevron (juce::Graphics& g, juce::Point<float> centre, float size, float angle, float thickness)
    {
        juce::Path p;
        p.startNewSubPath (-size * 0.5f, -size);
        p.lineTo (size * 0.5f, 0.0f);
        p.lineTo (-size * 0.5f, size);
        p.applyTransform (juce::AffineTransform::rotation (angle).translated (centre));
        g.strokePath (p, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Scale of a component relative to its window: for a control in the editor, the editor's content scale.
    float scaleWithinWindow (const juce::Component& c)
    {
        // Relative to the plugin editor, not to the top-level window: a VST3 host on a scaled Windows
        // display applies its scale as a transform on the AudioProcessorEditor, and that must not count
        // towards the user's own size (otherwise the "never smaller than at default size" floor breaks).
        const juce::Component* reference = c.findParentComponentOfClass<juce::AudioProcessorEditor>();
        if (reference == nullptr)
            reference = c.getTopLevelComponent();

        const float own = juce::Component::getApproximateScaleFactorForComponent (&c);
        const float base = juce::Component::getApproximateScaleFactorForComponent (reference);
        return base > 0.0f && own > 0.0f ? own / base : 1.0f;
    }

    float legibilityFactor (const juce::Component& target)
    {
        const float s = scaleWithinWindow (target);
        return s < c_minimumPopupScale ? c_minimumPopupScale / s : 1.0f;
    }

    // Every menu metric follows the menu's row height, so a submenu (which inherits it) matches its parent.
    float menuScale (const juce::PopupMenu::Options& options)
    {
        const int h = options.getStandardItemHeight();
        return h > 0 ? (float) h / (float) PitchFlattenerLookAndFeel::c_menuItemHeight : 1.0f;
    }

    // The accent of a menu: the colour of the control it drops from; the chrome colour for everything else.
    juce::Colour menuAccent (const juce::PopupMenu::Options& options)
    {
        if (auto* targetComponent = options.getTargetComponent())
        {
            if (targetComponent->isColourSpecified (juce::PopupMenu::highlightedBackgroundColourId))
                return targetComponent->findColour (juce::PopupMenu::highlightedBackgroundColourId).withAlpha (1.0f);

            if (auto* box = dynamic_cast<juce::ComboBox*> (targetComponent))
                return box->findColour (juce::ComboBox::arrowColourId);
        }
        return c_chrome;
    }

    bool isInAlert (const juce::Component& c)
    {
        return dynamic_cast<const juce::AlertWindow*> (c.getParentComponent()) != nullptr;
    }

    // A text button with a transparent fill is a text link (the About window's website and e-mail).
    bool isLinkStyle (const juce::Button& b)
    {
        return b.findColour (juce::TextButton::buttonColourId).isTransparent();
    }

    juce::Rectangle<float> pillBounds (const juce::Button& b)
    {
        auto r = b.getLocalBounds().toFloat();
        if (isInAlert (b))
            r = r.withSizeKeepingCentre (r.getWidth(), std::min (r.getHeight(), c_dialogButtonHeight));
        return r.reduced (0.5f);
    }

    float fieldRadius (int height) { return std::min (5.0f, (float) height * 0.25f); }

    //==============================================================================
    // Tab in a window shows its focus rings, a click in it hides them (stateless: the state is a property
    // of the window, so one instance serves every window).
    struct FocusRingTracker final : public juce::KeyListener, public juce::MouseListener
    {
        bool keyPressed (const juce::KeyPress& key, juce::Component* origin) override
        {
            if (origin != nullptr && key.isKeyCode (juce::KeyPress::tabKey))
                setVisible (*origin, true);
            return false; // never consumed: Tab still moves the focus
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.eventComponent != nullptr)
                setVisible (*e.eventComponent, false);
        }

        static void setVisible (juce::Component& c, bool visible)
        {
            auto* window = c.getTopLevelComponent();
            if ((bool) window->getProperties().getWithDefault (c_focusRingsProperty, false) == visible)
                return;

            window->getProperties().set (c_focusRingsProperty, visible);
            if (auto* focused = juce::Component::getCurrentlyFocusedComponent())
                if (window->isParentOf (focused) || focused == window)
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
        return { background, panelRaised, panelRaised, outline, text, c_chrome, text, c_chrome.withAlpha (0.3f), text };
    }
}

//==============================================================================
PitchFlattenerLookAndFeel::PitchFlattenerLookAndFeel (const Theme::Fonts& fonts)
    : juce::LookAndFeel_V4 (themeScheme())
    , m_fonts (fonts)
{
    const auto transparent = juce::Colours::transparentBlack;

    if (fonts.medium != nullptr)
        setDefaultSansSerifTypeface (fonts.medium);

    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::DocumentWindow::textColourId, text);
    setColour (juce::Label::textColourId, text);

    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxOutlineColourId, transparent);
    setColour (juce::Slider::textBoxBackgroundColourId, transparent);
    setColour (juce::Slider::textBoxHighlightColourId, c_chrome.withAlpha (0.3f));

    // Every editor box sets its own section accent; detect (teal) is the default, as before.
    setColour (juce::ComboBox::backgroundColourId, panelRaised);
    setColour (juce::ComboBox::outlineColourId, outline);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, detect);
    setColour (juce::ComboBox::focusedOutlineColourId, detect.withAlpha (0.6f));

    setColour (juce::PopupMenu::backgroundColourId, nearlyOpaque (panelRaised));
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, textDim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, c_chrome.withAlpha (0.16f));
    setColour (juce::PopupMenu::highlightedTextColourId, text);

    setColour (juce::TextButton::buttonColourId, panelRaised);
    setColour (juce::TextButton::buttonOnColourId, panelRaised.brighter (0.1f));
    setColour (juce::TextButton::textColourOffId, textDim);
    setColour (juce::TextButton::textColourOnId, text);

    setColour (juce::ToggleButton::textColourId, textDim);
    setColour (juce::ToggleButton::tickColourId, detect);

    setColour (juce::HyperlinkButton::textColourId, textDim);

    setColour (juce::AlertWindow::backgroundColourId, panel);
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::AlertWindow::outlineColourId, outline);

    setColour (juce::TextEditor::backgroundColourId, panelRaised);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, outline);
    setColour (juce::TextEditor::focusedOutlineColourId, c_chrome.withAlpha (0.75f));
    setColour (juce::TextEditor::highlightColourId, c_chrome.withAlpha (0.3f));
    setColour (juce::TextEditor::highlightedTextColourId, text);
    setColour (juce::TextEditor::shadowColourId, transparent);
    setColour (juce::CaretComponent::caretColourId, c_chrome);

    setColour (juce::ScrollBar::thumbColourId, textFaint);
    setColour (juce::ScrollBar::trackColourId, transparent);
    setColour (juce::ScrollBar::backgroundColourId, transparent);

    setColour (juce::TooltipWindow::backgroundColourId, panelRaised);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, outline);
}

//==============================================================================
juce::PopupMenu::Options PitchFlattenerLookAndFeel::menuOptionsFor (juce::Component& targetComponent, int minimumWidth)
{
    return juce::PopupMenu::Options().withTargetComponent (&targetComponent)
                                     .withMinimumWidth (minimumWidth)
                                     .withStandardItemHeight (juce::roundToInt ((float) c_menuItemHeight * legibilityFactor (targetComponent)));
}

void PitchFlattenerLookAndFeel::showFocusRingsAfterTab (juce::Component& window)
{
    window.addKeyListener (&focusRingTracker());
    window.addMouseListener (&focusRingTracker(), true);
}

bool PitchFlattenerLookAndFeel::shouldShowFocusRing (const juce::Component& c)
{
    if (! c.hasKeyboardFocus (false))
        return false;

    const auto* window = c.getTopLevelComponent();
    return (bool) window->getProperties().getWithDefault (c_focusRingsProperty, false);
}

void PitchFlattenerLookAndFeel::setLabelDrawnInCapitals (juce::Button& button)
{
    button.getProperties().set (c_capitalsProperty, true);
    button.repaint();
}

juce::Colour PitchFlattenerLookAndFeel::accentFor (juce::MessageBoxIconType type)
{
    switch (type)
    {
        case juce::MessageBoxIconType::WarningIcon:  return warning;
        case juce::MessageBoxIconType::InfoIcon:     return detect;
        case juce::MessageBoxIconType::QuestionIcon:
        case juce::MessageBoxIconType::NoIcon:
        default:                                     return target;
    }
}

void PitchFlattenerLookAndFeel::drawMark (juce::Graphics& g, juce::Rectangle<float> mark)
{
    // A wobbling teal line that settles into a flat amber line, designed at 34 x 22.
    const float sx = mark.getWidth() / 34.0f, sy = mark.getHeight() / 22.0f;
    const float split = 0.55f;
    juce::Path wobble, flat;

    for (int i = 0; i <= 40; ++i)
    {
        const float t = (float) i / 40.0f;
        if (t > split)
            break;

        const float fade = 1.0f - t / split;
        const float y = mark.getCentreY() + sy * 7.0f * fade * std::sin (t * juce::MathConstants<float>::twoPi * 3.2f);
        const juce::Point<float> p { mark.getX() + t * mark.getWidth(), y };
        if (i == 0) wobble.startNewSubPath (p);
        else        wobble.lineTo (p);
    }

    flat.startNewSubPath (mark.getX() + split * mark.getWidth(), mark.getCentreY());
    flat.lineTo (mark.getRight(), mark.getCentreY());

    const float stroke = std::min (sx, sy);
    g.setColour (detect.withAlpha (0.85f));
    g.strokePath (wobble, juce::PathStrokeType (1.6f * stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (target);
    g.strokePath (flat, juce::PathStrokeType (2.4f * stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Rectangle<float> PitchFlattenerLookAndFeel::drawPanel (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title,
                                                             juce::Colour accent, float headerHeight) const
{
    // The editor's section panels: fill, soft outline, dot, title, divider.
    g.setColour (panel);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (outlineSoft);
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

    auto headerRow = r.withHeight (headerHeight).reduced (12.0f, 0.0f);
    g.setColour (accent);
    g.fillEllipse (headerRow.removeFromLeft (6.0f).withSizeKeepingCentre (6.0f, 6.0f));
    headerRow.removeFromLeft (8.0f);
    g.setFont (m_fonts.heading (11.0f));
    g.setColour (text.withAlpha (0.85f));
    g.drawText (title, headerRow, juce::Justification::centredLeft, false);

    if (headerHeight < r.getHeight() - 1.0f) // a panel that is only a header row has no divider
    {
        g.setColour (outlineSoft);
        g.fillRect (juce::Rectangle<float> (r.getX() + 12.0f, std::round (r.getY() + headerHeight), r.getWidth() - 24.0f, 1.0f));
    }
    return headerRow;
}

//==============================================================================
// Knobs
//==============================================================================
void PitchFlattenerLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                                  float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                                  juce::Slider& slider)
{
    const bool enabled = slider.isEnabled();
    const float alpha = enabled ? 1.0f : 0.3f;
    const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
    const bool hot = enabled && slider.isMouseOverOrDragging();

    const auto area = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
    const float diameter = std::min (area.getWidth(), area.getHeight());
    const auto centre = area.getCentre();
    const float radius = diameter * 0.5f;
    const float trackWidth = std::max (2.5f, radius * 0.09f);
    const float arcRadius = radius - trackWidth * 0.5f;
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // Track
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (outline);
    g.strokePath (track, juce::PathStrokeType (trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc: bipolar parameters grow from the centre.
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0
                      && juce::approximatelyEqual (slider.getMinimum(), -slider.getMaximum());
    const float fromAngle = bipolar ? (rotaryStartAngle + rotaryEndAngle) * 0.5f : rotaryStartAngle;

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, std::min (fromAngle, angle), std::max (fromAngle, angle), true);
    g.setColour (accent.withMultipliedAlpha ((hot ? 1.0f : 0.9f) * alpha));
    g.strokePath (value, juce::PathStrokeType (trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Body
    const float bodyRadius = arcRadius - trackWidth * 1.6f;
    const auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);

    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillEllipse (body.translated (0.0f, 2.0f));

    g.setGradientFill (juce::ColourGradient (panelRaised.brighter (hot ? 0.18f : 0.1f), centre.x, body.getY(),
                                             panelRaised.darker (0.25f), centre.x, body.getBottom(), false));
    g.fillEllipse (body);
    g.setColour (outline.brighter (0.15f));
    g.drawEllipse (body.reduced (0.5f), 1.0f);

    // Pointer
    const auto direction = juce::Point<float> (std::sin (angle), -std::cos (angle));
    g.setColour (text.withAlpha (enabled ? 1.0f : 0.35f));
    g.drawLine ({ centre + direction * (bodyRadius * 0.35f), centre + direction * (bodyRadius * 0.82f) }, 2.0f);
}

juce::Slider::SliderLayout PitchFlattenerLookAndFeel::getSliderLayout (juce::Slider& slider)
{
    juce::Slider::SliderLayout layout;

    if (slider.getTextBoxPosition() == juce::Slider::NoTextBox)
    {
        layout.sliderBounds = slider.getLocalBounds();
        return layout;
    }

    auto bounds = slider.getLocalBounds();
    layout.textBoxBounds = bounds.removeFromBottom (18).withSizeKeepingCentre (std::min (bounds.getWidth(), 92), 18);
    bounds.removeFromBottom (2);
    layout.sliderBounds = bounds;
    return layout;
}

juce::Label* PitchFlattenerLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (m_fonts.body (12.0f));
    label->setJustificationType (juce::Justification::centred);
    label->setColour (juce::Label::textColourId, text);
    label->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);

    // While a value is typed: a rounded field in the knob's own accent (outline, selection, caret), with the
    // text centred like the value it replaces. Label::createEditorComponent() copies these to its TextEditor.
    const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
    label->setColour (juce::Label::textWhenEditingColourId, text);
    label->setColour (juce::Label::backgroundWhenEditingColourId, panelRaised);
    label->setColour (juce::Label::outlineWhenEditingColourId, accent.withAlpha (0.6f));
    label->setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
    label->setColour (juce::TextEditor::highlightedTextColourId, text);
    label->setColour (juce::CaretComponent::caretColourId, accent);
    label->onEditorShow = [label]
    {
        if (auto* editor = label->getCurrentTextEditor())
        {
            editor->setJustification (juce::Justification::centred);
            editor->setIndents (2, 0);
        }
    };
    return label;
}

juce::Font PitchFlattenerLookAndFeel::getLabelFont (juce::Label& label)
{
    // Knob value boxes show values; every other editor label is a caption. A label whose code set one of the
    // theme's fonts keeps it (e.g. the About window's update status); JUCE's default label font never shows.
    if (dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr)
        return m_fonts.body (12.0f);

    if (dynamic_cast<juce::ComboBox*> (label.getParentComponent()) == nullptr)
    {
        const auto face = label.getFont().getTypefacePtr();
        if (face != nullptr && (face == m_fonts.regular || face == m_fonts.medium || face == m_fonts.bold))
            return label.getFont();
    }

    return m_fonts.label (10.5f);
}

//==============================================================================
// Combo boxes
//==============================================================================
void PitchFlattenerLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                              int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    const bool enabled = box.isEnabled();
    const bool hot = enabled && (box.isMouseOver (true) || box.isPopupActive());
    const bool ring = shouldShowFocusRing (box);

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId).brighter (hot ? 0.08f : 0.0f));
    g.fillRoundedRectangle (r, 5.0f);

    if (ring)
    {
        g.setColour (c_chrome);
        g.drawRoundedRectangle (r.reduced (0.25f), 5.0f, 1.5f);
    }
    else
    {
        g.setColour (hot ? box.findColour (juce::ComboBox::focusedOutlineColourId) : box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
    }

    const float cx = r.getRight() - 13.0f;
    const float cy = r.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
    chevron.lineTo (cx, cy + 2.0f);
    chevron.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (enabled ? 1.0f : 0.35f));
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font PitchFlattenerLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return m_fonts.body (12.0f);
}

void PitchFlattenerLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (8, 0, box.getWidth() - 30, box.getHeight());
    label.setFont (getComboBoxFont (box));
}

juce::PopupMenu::Options PitchFlattenerLookAndFeel::getOptionsForComboBoxPopupMenu (juce::ComboBox& box, juce::Label& label)
{
    // JUCE's options (rows as high as the box, at least as wide), with the same legibility floor as
    // menuOptionsFor().
    return juce::LookAndFeel_V4::getOptionsForComboBoxPopupMenu (box, label)
               .withStandardItemHeight (juce::roundToInt ((float) label.getHeight() * legibilityFactor (box)));
}

//==============================================================================
// Buttons
//==============================================================================
bool PitchFlattenerLookAndFeel::isPrimaryDialogButton (const juce::Button& button)
{
    return isInAlert (button) && button.isRegisteredForShortcut (juce::KeyPress (juce::KeyPress::returnKey));
}

void PitchFlattenerLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                                      const juce::Colour& backgroundColour,
                                                      bool isMouseOverButton, bool isButtonDown)
{
    if (backgroundColour.isTransparent())
        return; // a text link: no pill (see drawButtonText)

    const auto r = pillBounds (button);
    const float corner = std::min (r.getWidth(), r.getHeight()) * 0.5f;
    const bool enabled = button.isEnabled();
    const bool hot = isMouseOverButton && enabled;

    auto fill = backgroundColour;
    auto edge = hot ? textDim : outline;
    auto ringColour = c_chrome;

    if (auto* alert = dynamic_cast<juce::AlertWindow*> (button.getParentComponent()))
    {
        // The Return-key button is the dialog's primary action, tinted in the dialog's accent.
        const auto accent = accentFor (alert->getAlertType());
        ringColour = accent;

        if (isPrimaryDialogButton (button))
        {
            fill = accent.withAlpha (0.28f);
            edge = hot ? accent : accent.withAlpha (0.6f);
        }
    }

    const float alpha = enabled ? 1.0f : c_disabledAlpha;

    g.setColour (fill.brighter (isButtonDown && enabled ? 0.2f : hot ? 0.1f : 0.0f).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (r, corner);

    if (shouldShowFocusRing (button))
    {
        g.setColour (ringColour);
        g.drawRoundedRectangle (r.reduced (0.25f), corner, 1.5f);
    }
    else
    {
        g.setColour (edge.withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (r, corner, 1.0f);
    }
}

void PitchFlattenerLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool isMouseOverButton, bool isButtonDown)
{
    const auto font = getTextButtonFont (button, button.getHeight());
    const bool capitals = isInAlert (button) || (bool) button.getProperties().getWithDefault (c_capitalsProperty, false);
    const auto label = capitals ? button.getButtonText().toUpperCase() : button.getButtonText();

    if (isLinkStyle (button))
    {
        // A text link: dim (or its own colour) when idle, amber under the mouse, underlined on hover or focus.
        auto colour = button.findColour (juce::TextButton::textColourOffId);
        if (! button.isEnabled())
            colour = textFaint;
        else if (isButtonDown)
            colour = c_chrome.darker (0.15f);
        else if (isMouseOverButton)
            colour = c_chrome;

        const auto area = button.getLocalBounds().toFloat();
        const auto shown = fitToWidth (font, label, area.getWidth());
        g.setFont (font);
        g.setColour (colour);
        g.drawText (shown, area, juce::Justification::centred, false);

        if (button.isEnabled() && (isMouseOverButton || shouldShowFocusRing (button)))
        {
            const float w = lineWidth (font, shown);
            const float y = std::round (area.getCentreY() + font.getAscent() * 0.5f + 2.0f);
            g.setColour (shouldShowFocusRing (button) ? c_chrome : colour);
            g.fillRect (juce::Rectangle<float> (area.getCentreX() - w * 0.5f, y, w, 1.0f));
        }
        return;
    }

    auto colour = button.findColour (button.getToggleState() || isPrimaryDialogButton (button) ? juce::TextButton::textColourOnId
                                                                                              : juce::TextButton::textColourOffId);
    if (! button.isEnabled())
        colour = colour.withMultipliedAlpha (0.5f); // as JUCE's V4 buttons, which Pitch Flattener used before

    // The label keeps clear of the pill's rounded ends, but a short label in a small pill ("?") takes the
    // room it needs: a label is only cut when the pill is too narrow for it.
    // (Graphics::drawText drops a word that overruns its area, hence the pixel of slack.)
    const auto r = pillBounds (button);
    const float preferredPad = std::max (4.0f, std::round (r.getHeight() * 0.35f));
    const float pad = juce::jlimit (1.0f, preferredPad, (r.getWidth() - lineWidth (font, label)) * 0.5f - 1.0f);
    const auto area = r.reduced (pad, 0.0f);

    g.setFont (font);
    g.setColour (colour);
    g.drawText (fitToWidth (font, label, area.getWidth()), area, juce::Justification::centred, false);
}

juce::Font PitchFlattenerLookAndFeel::getTextButtonFont (juce::TextButton& button, int buttonHeight)
{
    if (isInAlert (button))
        return m_fonts.label (12.0f);
    if (isLinkStyle (button))
        return m_fonts.body (12.0f);
    return m_fonts.label (std::min (12.0f, (float) buttonHeight * 0.5f));
}

void PitchFlattenerLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                                  bool isMouseOverButton, bool isButtonDown)
{
    // A pill switch in the button's accent (tickColourId), dimmed when disabled.
    const bool enabled = button.isEnabled();
    const bool on = button.getToggleState();
    const float alpha = enabled ? 1.0f : 0.35f;
    const auto accent = button.findColour (juce::ToggleButton::tickColourId);

    auto bounds = button.getLocalBounds().toFloat();
    constexpr float trackW = 32.0f, trackH = 16.0f;
    const auto buttonText = button.getButtonText();

    // Centred switch when there is no text, otherwise switch on the left with the label after it.
    const auto track = buttonText.isEmpty() ? juce::Rectangle<float> (trackW, trackH).withCentre (bounds.getCentre())
                                            : bounds.removeFromLeft (trackW).withSizeKeepingCentre (trackW, trackH);

    const float corner = trackH * 0.5f;
    g.setColour ((on ? accent.withAlpha (0.85f) : panelRaised.brighter (isMouseOverButton ? 0.08f : 0.0f)).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (track, corner);

    if (shouldShowFocusRing (button))
    {
        g.setColour (c_chrome);
        g.drawRoundedRectangle (track.expanded (2.0f), corner + 2.0f, 1.5f);
    }

    g.setColour ((on ? accent : (isMouseOverButton ? textDim : outline)).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (track.reduced (0.5f), corner, 1.0f);

    const float thumb = trackH - 4.0f + (isButtonDown ? 1.0f : 0.0f);
    const float thumbX = on ? track.getRight() - 2.0f - thumb : track.getX() + 2.0f;
    const auto thumbBounds = juce::Rectangle<float> (thumbX, track.getCentreY() - thumb * 0.5f, thumb, thumb);

    g.setColour (juce::Colours::black.withAlpha (0.3f * alpha));
    g.fillEllipse (thumbBounds.translated (0.0f, 1.0f));
    g.setColour ((on ? text : textDim).withMultipliedAlpha (alpha));
    g.fillEllipse (thumbBounds);

    if (buttonText.isNotEmpty())
    {
        bounds.removeFromLeft (8.0f);
        g.setFont (m_fonts.label (10.5f));
        g.setColour (button.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (alpha));
        g.drawText (buttonText.toUpperCase(), bounds, juce::Justification::centredLeft, true);
    }
}

//==============================================================================
// Popup menus
//==============================================================================
juce::Font PitchFlattenerLookAndFeel::menuFont (float scale) const
{
    return m_fonts.body (c_menuFontHeight * scale);
}

juce::Font PitchFlattenerLookAndFeel::getPopupMenuFont()
{
    return menuFont (1.0f);
}

int PitchFlattenerLookAndFeel::getPopupMenuBorderSizeWithOptions (const juce::PopupMenu::Options& options)
{
    return std::max (2, juce::roundToInt (c_menuBorder * menuScale (options)));
}

void PitchFlattenerLookAndFeel::drawPopupMenuBackgroundWithOptions (juce::Graphics& g, int width, int height,
                                                                    const juce::PopupMenu::Options& options)
{
    const float s = menuScale (options);
    const auto r = juce::Rectangle<float> ((float) width, (float) height);
    const auto colour = findColour (juce::PopupMenu::backgroundColourId);
    const auto fill = colour.withAlpha (1.0f);

    // JUCE makes the window opaque when it can't be transparent: then it has no corners to show through.
    if (colour.isOpaque() || ! juce::Desktop::canUseSemiTransparentWindows())
        g.fillAll (fill);

    g.setColour (fill);
    g.fillRoundedRectangle (r, c_menuRadius * s);
    g.setColour (outline);
    g.drawRoundedRectangle (r.reduced (0.5f), c_menuRadius * s, 1.0f);
}

void PitchFlattenerLookAndFeel::drawPopupMenuItemWithOptions (juce::Graphics& g, const juce::Rectangle<int>& area,
                                                              bool isHighlighted, const juce::PopupMenu::Item& item,
                                                              const juce::PopupMenu::Options& options)
{
    const float s = menuScale (options);
    const auto border = (float) getPopupMenuBorderSizeWithOptions (options);
    const auto row = area.toFloat().reduced (border, 0.0f); // rows span the column; their content sits inside the border

    if (item.isSeparator)
    {
        g.setColour (outline);
        g.fillRect (juce::Rectangle<float> (row.getX() + 8.0f * s, std::floor (row.getCentreY()), row.getWidth() - 16.0f * s, 1.0f));
        return;
    }

    const auto accent = menuAccent (options);
    const bool enabled = item.isEnabled;
    const bool hasSubMenu = item.subMenu != nullptr && (item.itemID == 0 || item.subMenu->getNumItems() > 0);
    const bool highlighted = isHighlighted && enabled;

    if (highlighted)
    {
        g.setColour (accent.withAlpha (0.16f));
        g.fillRoundedRectangle (row.reduced (0.0f, 1.0f), 4.0f * s);
    }

    auto content = row;
    const auto tickArea = content.removeFromLeft (c_menuTickColumn * s);
    content.removeFromLeft ((c_menuTextInset - c_menuTickColumn) * s);
    const auto arrowArea = content.removeFromRight (c_menuArrowColumn * s);

    if (item.image != nullptr)
    {
        item.image->drawWithin (g, tickArea.reduced (5.0f * s), juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize,
                                enabled ? 1.0f : 0.4f);
    }
    else if (item.isTicked)
    {
        // A stroked check in the menu's accent.
        const auto c = tickArea.getCentre().translated (1.0f * s, 0.0f);
        juce::Path tick;
        tick.startNewSubPath (c.x - 4.0f * s, c.y + 0.2f * s);
        tick.lineTo (c.x - 1.3f * s, c.y + 3.0f * s);
        tick.lineTo (c.x + 4.0f * s, c.y - 3.2f * s);
        g.setColour (enabled ? accent : textFaint);
        g.strokePath (tick, juce::PathStrokeType (1.7f * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (hasSubMenu)
    {
        g.setColour (! enabled ? textFaint : highlighted ? text : textDim);
        strokeChevron (g, arrowArea.getCentre().translated (-2.0f * s, 0.0f), 3.5f * s, 0.0f, 1.4f * s);
    }

    // Text, with a trailing "(...)" qualifier in the dim colour.
    const auto font = menuFont (s);
    const auto main = ! enabled ? textFaint
                    : item.colour != juce::Colour() ? item.colour
                    : findColour (highlighted ? juce::PopupMenu::highlightedTextColourId : juce::PopupMenu::textColourId);
    const auto shown = fitToWidth (font, item.text, content.getWidth());
    const int open = shown.lastIndexOf (" (");

    g.setFont (font);
    if (shown == item.text && open > 0 && shown.endsWithChar (')') && enabled)
    {
        const auto head = shown.substring (0, open);
        g.setColour (main);
        g.drawText (head, content, juce::Justification::centredLeft, false);
        g.setColour (textDim);
        g.drawText (shown.substring (open), content.withTrimmedLeft (lineWidth (font, head)), juce::Justification::centredLeft, false);
    }
    else
    {
        g.setColour (main);
        g.drawText (shown, content, juce::Justification::centredLeft, false);
    }

    if (item.shortcutKeyDescription.isNotEmpty())
    {
        g.setFont (m_fonts.label (10.0f * s));
        g.setColour (textFaint);
        g.drawText (item.shortcutKeyDescription, content, juce::Justification::centredRight, false);
    }
}

void PitchFlattenerLookAndFeel::drawPopupMenuSectionHeaderWithOptions (juce::Graphics& g, const juce::Rectangle<int>& area,
                                                                       const juce::String& sectionName,
                                                                       const juce::PopupMenu::Options& options)
{
    // In the panel-header style: an accent dot in the tick column, then the name in the heading font, aligned
    // with the items' text. (JUCE already insets a header by the menu border.)
    const float s = menuScale (options);
    const auto r = area.toFloat().withTrimmedTop (8.0f * s).withTrimmedBottom (2.0f * s);
    const float dot = 5.0f * s;

    g.setColour (menuAccent (options));
    g.fillEllipse (juce::Rectangle<float> (dot, dot).withCentre ({ r.getX() + c_menuTickColumn * s * 0.5f + 1.0f * s, r.getCentreY() }));

    const auto font = m_fonts.heading (10.5f * s);
    const auto textArea = r.withTrimmedLeft (c_menuTextInset * s).withTrimmedRight (8.0f * s);
    g.setFont (font);
    g.setColour (findColour (juce::PopupMenu::headerTextColourId));
    g.drawText (fitToWidth (font, sectionName, textArea.getWidth()), textArea, juce::Justification::centredLeft, false);
}

void PitchFlattenerLookAndFeel::getIdealPopupMenuItemSizeWithOptions (const juce::String& itemText, bool isSeparator,
                                                                      int standardMenuItemHeight, int& idealWidth,
                                                                      int& idealHeight, const juce::PopupMenu::Options& options)
{
    const float s = menuScale (options);

    if (isSeparator)
    {
        idealWidth = juce::roundToInt (50.0f * s);
        idealHeight = std::max (5, juce::roundToInt (c_menuSeparatorHeight * s));
        return;
    }

    idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight : c_menuItemHeight;
    idealWidth = juce::roundToInt (lineWidth (menuFont (s), itemText) + (c_menuTextInset + c_menuArrowColumn + 4.0f) * s);
}

void PitchFlattenerLookAndFeel::getIdealPopupMenuSectionHeaderSizeWithOptions (const juce::String& headerText, int,
                                                                               int& idealWidth, int& idealHeight,
                                                                               const juce::PopupMenu::Options& options)
{
    const float s = menuScale (options);
    idealHeight = juce::roundToInt (c_menuHeaderHeight * s);
    idealWidth = juce::roundToInt (lineWidth (m_fonts.heading (10.5f * s), headerText) + (c_menuTextInset + 12.0f) * s);
}

void PitchFlattenerLookAndFeel::drawPopupMenuUpDownArrowWithOptions (juce::Graphics& g, int width, int height,
                                                                     bool isScrollUpArrow, const juce::PopupMenu::Options& options)
{
    // A menu taller than the screen: the body colour fading out over the rows, and a chevron.
    const float s = menuScale (options);
    const float h = (float) height;
    const float inset = c_menuRadius * s;
    const auto fill = findColour (juce::PopupMenu::backgroundColourId).withAlpha (1.0f);

    g.setGradientFill (juce::ColourGradient (fill, 0.0f, isScrollUpArrow ? 0.0f : h,
                                             fill.withAlpha (0.0f), 0.0f, isScrollUpArrow ? h : 0.0f, false));
    g.fillRect (juce::Rectangle<float> (inset, isScrollUpArrow ? 1.0f : 0.0f, (float) width - 2.0f * inset, h - 1.0f));

    g.setColour (textDim);
    strokeChevron (g, { (float) width * 0.5f, h * 0.5f }, 3.0f * s,
                   juce::MathConstants<float>::halfPi * (isScrollUpArrow ? -1.0f : 1.0f), 1.5f * s);
}

int PitchFlattenerLookAndFeel::getPopupMenuColumnSeparatorWidthWithOptions (const juce::PopupMenu::Options&)
{
    return 1;
}

void PitchFlattenerLookAndFeel::drawPopupMenuColumnSeparatorWithOptions (juce::Graphics& g, const juce::Rectangle<int>& bounds,
                                                                         const juce::PopupMenu::Options& options)
{
    g.setColour (outline);
    g.fillRect (bounds.toFloat().reduced (0.0f, 6.0f * menuScale (options)));
}

//==============================================================================
// Alerts
//==============================================================================
juce::AlertWindow* PitchFlattenerLookAndFeel::createAlertWindow (const juce::String& title, const juce::String& message,
                                                                 const juce::String& button1, const juce::String& button2,
                                                                 const juce::String& button3, juce::MessageBoxIconType iconType,
                                                                 int numButtons, juce::Component* associatedComponent)
{
    // JUCE's own layout, without the V4 enlargement that its icon drawing needs.
    auto* alert = juce::LookAndFeel_V2::createAlertWindow (title, message, button1, button2, button3,
                                                           iconType, numButtons, associatedComponent);
    alert->setOpaque (false);
    return alert;
}

void PitchFlattenerLookAndFeel::drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert,
                                              const juce::Rectangle<int>&, juce::TextLayout& textLayout)
{
    const auto bounds = alert.getLocalBounds().toFloat();
    const auto accent = accentFor (alert.getAlertType());

    if (alert.isOpaque())
        g.fillAll (background); // an opaque window can't show what is behind its rounded corners

    // A panel, as the editor's sections: no icon.
    g.setColour (alert.findColour (juce::AlertWindow::backgroundColourId));
    g.fillRoundedRectangle (bounds, c_dialogRadius);
    g.setColour (alert.findColour (juce::AlertWindow::outlineColourId));
    g.drawRoundedRectangle (bounds.reduced (0.5f), c_dialogRadius, 1.0f);

    // Header row: accent dot, the title in capitals, a divider.
    auto header = bounds.withHeight (c_dialogHeaderHeight).reduced (14.0f, 0.0f);
    g.setColour (accent);
    g.fillEllipse (header.removeFromLeft (6.0f).withSizeKeepingCentre (6.0f, 6.0f));
    header.removeFromLeft (8.0f);
    const auto titleFont = m_fonts.heading (11.5f);
    g.setFont (titleFont);
    g.setColour (text.withAlpha (0.85f));
    g.drawText (fitToWidth (titleFont, alert.getName().toUpperCase(), header.getWidth()), header, juce::Justification::centredLeft, false);
    g.setColour (outlineSoft);
    g.fillRect (juce::Rectangle<float> (12.0f, c_dialogHeaderHeight, bounds.getWidth() - 24.0f, 1.0f));

    // JUCE lays the title and the message out as one TextLayout. The title is drawn above, so its runs are
    // hidden; the message lines are centred across the dialog, between the header and the first control.
    const auto messageColour = alert.findColour (juce::AlertWindow::textColourId);
    juce::Range<float> messageY;
    bool hasMessage = false;

    for (int i = 0; i < textLayout.getNumLines(); ++i)
    {
        auto& line = textLayout.getLine (i);
        bool isMessageLine = false;

        for (auto* run : line.runs)
        {
            const bool isTitle = run->font.getTypefacePtr() == m_fonts.bold;
            run->colour = isTitle ? juce::Colours::transparentBlack : messageColour;
            isMessageLine = isMessageLine || (! isTitle && ! run->glyphs.isEmpty());
        }

        if (! isMessageLine)
            continue;

        const auto x = line.getLineBoundsX();
        line.lineOrigin.x += (bounds.getWidth() - x.getLength()) * 0.5f - x.getStart();
        const auto y = line.getLineBoundsY();
        messageY = hasMessage ? messageY.getUnionWith (y) : y;
        hasMessage = true;
    }

    if (! hasMessage)
        return;

    float controlsTop = bounds.getBottom() - 12.0f;
    for (auto* child : alert.getChildren())
    {
        if (! child->isVisible())
            continue;
        if (dynamic_cast<juce::Button*> (child) != nullptr)
            controlsTop = std::min (controlsTop, (float) child->getY() + ((float) child->getHeight() - c_dialogButtonHeight) * 0.5f);
        else if (dynamic_cast<juce::TextEditor*> (child) != nullptr)
            controlsTop = std::min (controlsTop, (float) child->getY());
    }

    const float top = c_dialogHeaderHeight + 10.0f;
    const float bottom = controlsTop - 10.0f;
    const float y = top + std::max (0.0f, (bottom - top - messageY.getLength()) * 0.5f);
    textLayout.draw (g, juce::Rectangle<float> (0.0f, y - messageY.getStart(), textLayout.getWidth(), textLayout.getHeight()));
}

int PitchFlattenerLookAndFeel::getAlertWindowButtonHeight()
{
    return c_dialogButtonSlot;
}

juce::Array<int> PitchFlattenerLookAndFeel::getWidthsForTextButtons (juce::AlertWindow&, const juce::Array<juce::TextButton*>& buttons)
{
    // One width for every button of a dialog, so the row reads as a set.
    const auto font = m_fonts.label (12.0f);
    int width = c_dialogButtonMinWidth;
    for (auto* b : buttons)
        width = std::max (width, juce::roundToInt (lineWidth (font, b->getButtonText().toUpperCase())) + 44);

    juce::Array<int> widths;
    widths.insertMultiple (0, width, buttons.size());
    return widths;
}

juce::Font PitchFlattenerLookAndFeel::getAlertWindowTitleFont()   { return m_fonts.heading (11.5f); }
juce::Font PitchFlattenerLookAndFeel::getAlertWindowMessageFont() { return m_fonts.body (12.5f); }
juce::Font PitchFlattenerLookAndFeel::getAlertWindowFont()        { return m_fonts.label (10.5f); }

//==============================================================================
// Text fields
//==============================================================================
void PitchFlattenerLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    const auto fill = editor.findColour (juce::TextEditor::backgroundColourId);
    if (fill.isTransparent())
        return;

    if (editor.isOpaque()) // an opaque component must cover its corners
    {
        g.fillAll (fill);
        return;
    }

    g.setColour (fill);
    g.fillRoundedRectangle (juce::Rectangle<float> ((float) width, (float) height), fieldRadius (height));
}

void PitchFlattenerLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (! editor.isEnabled())
        return;

    // A field being typed in shows its focus however it got it; read-only text (the About credits) only
    // when Tab moved the focus there, like the buttons.
    const bool focused = editor.hasKeyboardFocus (true) && (! editor.isReadOnly() || shouldShowFocusRing (editor));
    const auto colour = editor.findColour (focused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId);
    if (colour.isTransparent())
        return;

    const float thickness = focused ? 1.5f : 1.0f;
    const auto r = juce::Rectangle<float> ((float) width, (float) height).reduced (thickness * 0.5f);
    g.setColour (colour);
    if (editor.isOpaque())
        g.drawRect (r, thickness);
    else
        g.drawRoundedRectangle (r, fieldRadius (height), thickness);
}

//==============================================================================
// Scrollbars
//==============================================================================
void PitchFlattenerLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& scrollbar, int x, int y, int width, int height,
                                               bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                                               bool isMouseOver, bool isMouseDown)
{
    if (thumbSize <= 0)
        return;

    // No track: a thin pill, like the level meter's track.
    auto thumb = (isScrollbarVertical ? juce::Rectangle<int> (x, thumbStartPosition, width, thumbSize)
                                      : juce::Rectangle<int> (thumbStartPosition, y, thumbSize, height)).toFloat();
    thumb = isScrollbarVertical ? thumb.withSizeKeepingCentre (4.0f, std::max (4.0f, thumb.getHeight() - 4.0f))
                                : thumb.withSizeKeepingCentre (std::max (4.0f, thumb.getWidth() - 4.0f), 4.0f);

    g.setColour (isMouseOver || isMouseDown ? textDim : scrollbar.findColour (juce::ScrollBar::thumbColourId));
    g.fillRoundedRectangle (thumb, 2.0f);
}

//==============================================================================
// Tooltips
//==============================================================================
juce::TextLayout PitchFlattenerLookAndFeel::layoutTooltip (const juce::String& tipText, juce::Colour colour) const
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::topLeft);
    s.setLineSpacing (2.0f);
    s.append (tipText, m_fonts.body (11.5f), colour);

    juce::TextLayout layout;

    // Balanced line lengths suit a single paragraph. A list (explicit line breaks) is laid out as it
    // is: JUCE's balancing compares only the last two lines, so it would push a word of an earlier,
    // longer line onto a line of its own.
    if (tipText.containsChar ('\n'))
        layout.createLayout (s, c_tipMaxWidth);
    else
        layout.createLayoutWithBalancedLineLengths (s, c_tipMaxWidth);

    return layout;
}

juce::Rectangle<int> PitchFlattenerLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                                                  juce::Rectangle<int> parentArea)
{
    const auto layout = layoutTooltip (tipText, juce::Colours::black);
    const int w = (int) std::ceil (layout.getWidth() + 2.0f * c_tipPadX);
    const int h = (int) std::ceil (layout.getHeight() + 2.0f * c_tipPadY);

    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6,
                                 w, h)
        .constrainedWithin (parentArea);
}

void PitchFlattenerLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& tipText, int width, int height)
{
    // The editor's tooltip window is transparent outside this rounded panel.
    const auto r = juce::Rectangle<float> ((float) width, (float) height);
    const auto fill = findColour (juce::TooltipWindow::backgroundColourId);

    if (! juce::Desktop::canUseSemiTransparentWindows())
        g.fillAll (fill);

    g.setColour (fill);
    g.fillRoundedRectangle (r, c_tipRadius);
    g.setColour (findColour (juce::TooltipWindow::outlineColourId));
    g.drawRoundedRectangle (r.reduced (0.5f), c_tipRadius, 1.0f);

    layoutTooltip (tipText, findColour (juce::TooltipWindow::textColourId)).draw (g, r.reduced (c_tipPadX, c_tipPadY));
}

//==============================================================================
// Corner resizer, shadows, focus outlines
//==============================================================================
void PitchFlattenerLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
{
    // Three short grip strokes in the corner, faint until the mouse is over them.
    const float right = (float) w - 3.5f, bottom = (float) h - 3.5f;
    juce::Path grip;
    for (const float d : { 3.5f, 7.5f, 11.5f })
    {
        grip.startNewSubPath (right - d, bottom);
        grip.lineTo (right, bottom - d);
    }
    g.setColour (isMouseOver || isMouseDragging ? textDim : textFaint);
    g.strokePath (grip, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

std::unique_ptr<juce::DropShadower> PitchFlattenerLookAndFeel::createDropShadowerForComponent (juce::Component&)
{
    return std::make_unique<juce::DropShadower> (juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 14, { 0, 4 }));
}

std::unique_ptr<juce::FocusOutline> PitchFlattenerLookAndFeel::createFocusOutlineForComponent (juce::Component&)
{
    struct Properties final : public juce::FocusOutline::OutlineWindowProperties
    {
        juce::Rectangle<int> getOutlineBounds (juce::Component& c) override { return c.getScreenBounds().expanded (2); }

        void drawOutline (juce::Graphics& g, int width, int height) override
        {
            g.setColour (c_chrome);
            g.drawRoundedRectangle (juce::Rectangle<float> ((float) width, (float) height).reduced (0.75f), 5.0f, 1.5f);
        }
    };

    return std::make_unique<juce::FocusOutline> (std::make_unique<Properties>());
}

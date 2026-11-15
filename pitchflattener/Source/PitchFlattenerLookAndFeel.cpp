#include "PitchFlattenerLookAndFeel.h"

#include "Framebuffer.h"
#include "Hud.h"

namespace
{
    using namespace Theme::Colours;

    // Chrome that belongs to no section: focus rings, selection, caret, links, menus that drop from no box.
    // Amber, orange's highlight: these are all the user's own pointer and keys at work.
    const juce::Colour c_chrome = amber;

    // The old surfaces, in the HUD palette.
    const juce::Colour background = space;
    const juce::Colour panelRaised { 0xff101a2c };     // panel, a step up: fields, menus, tooltips
    const juce::Colour outline = panelEdge;
    const juce::Colour outlineSoft = panelEdge;
    const juce::Colour textFaint = textDim.withAlpha (0.7f);

    // Popups dropping from the editor follow its scale, but never get smaller than at its default size (1:1).
    constexpr float c_minimumPopupScale = 1.0f;

    constexpr float c_menuRadius = 6.0f;          // design-size menu metrics, scaled by menuScale() (the chamfer cut)
    constexpr float c_menuBorder = 5.0f;
    constexpr float c_menuTickColumn = 22.0f;
    constexpr float c_menuTextInset = 24.0f;
    constexpr float c_menuArrowColumn = 20.0f;
    constexpr float c_menuHeaderHeight = 26.0f;
    constexpr float c_menuSeparatorHeight = 9.0f;
    constexpr float c_menuFontHeight = 13.0f;

    constexpr float c_dialogRadius = 10.0f;       // the editor's panel chamfer
    constexpr float c_dialogHeaderHeight = 32.0f; // the editor's section header height
    constexpr int c_dialogButtonSlot = 40;        // JUCE lays out this height; the pill is c_dialogButtonHeight
    constexpr float c_dialogButtonHeight = 28.0f;
    constexpr int c_dialogButtonMinWidth = 96;

    constexpr float c_tipRadius = 7.0f;           // chamfer cut
    constexpr float c_tipPadX = 10.0f, c_tipPadY = 7.0f;
    constexpr float c_tipMaxWidth = 420.0f;

    constexpr float c_disabledAlpha = 0.4f;       // Pitch Flattener dims a disabled control as a whole

    const juce::Identifier c_focusRingsProperty { "pitchFlattenerFocusRings" };
    const juce::Identifier c_capitalsProperty { "pitchFlattenerCapitals" };
    const juce::Identifier c_dialogAccentProperty { "pitchFlattenerDialogAccent" };   // ARGB of a dialog button outside an alert
    const juce::Identifier c_dialogPrimaryProperty { "pitchFlattenerDialogPrimary" }; // ... and whether it is the primary action

    // A colour that looks opaque but makes JUCE create a transparent window (menus), so the cut corners
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

    // A button of a dialog: in an AlertWindow, or marked with setDialogButton() (the update popup's).
    bool isInAlert (const juce::Component& c)
    {
        return dynamic_cast<const juce::AlertWindow*> (c.getParentComponent()) != nullptr
            || c.getProperties().contains (c_dialogAccentProperty);
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

    float fieldCut (int height) { return std::min (6.0f, (float) height * 0.3f); }

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

    // A box is a control the user sets: orange.
    setColour (juce::ComboBox::backgroundColourId, panelRaised);
    setColour (juce::ComboBox::outlineColourId, outline);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, orange);
    setColour (juce::ComboBox::focusedOutlineColourId, orange.withAlpha (0.6f));

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
    setColour (juce::ToggleButton::tickColourId, orange);

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

    setColour (juce::TooltipWindow::backgroundColourId, nearlyOpaque (panelRaised));
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, panelEdge.brighter (0.4f));
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
        case juce::MessageBoxIconType::WarningIcon:  return magenta;   // a problem
        case juce::MessageBoxIconType::InfoIcon:     return lime;      // good news, "go"
        case juce::MessageBoxIconType::QuestionIcon:
        case juce::MessageBoxIconType::NoIcon:
        default:                                     return orange;    // a choice the user makes
    }
}

void PitchFlattenerLookAndFeel::drawMark (juce::Graphics& g, juce::Rectangle<float> mark)
{
    // The plug-in's idea in one mark: a cyan wobble (the pitch as heard) that settles into a flat
    // lime line (the flattened sound), leaning forward like the title. Designed at 34 x 22.
    const float sx = mark.getWidth() / 34.0f, sy = mark.getHeight() / 22.0f;
    const float split = 0.5f;
    juce::Path wobble, flat;

    for (int i = 0; i <= 40; ++i)
    {
        const float t = (float) i / 40.0f;
        if (t > split)
            break;

        const float fade = 1.0f - t / split;
        const float y = mark.getCentreY() + sy * 8.0f * fade * std::sin (t * juce::MathConstants<float>::twoPi * 3.0f);
        const juce::Point<float> p { mark.getX() + t * mark.getWidth(), y };
        if (i == 0) wobble.startNewSubPath (p);
        else        wobble.lineTo (p);
    }

    flat.startNewSubPath (mark.getX() + split * mark.getWidth(), mark.getCentreY());
    flat.lineTo (mark.getRight(), mark.getCentreY());

    const float stroke = std::min (sx, sy);
    const auto lean = juce::AffineTransform::shear (-Hud::kShear, 0.0f).translated (Hud::kShear * mark.getCentreY(), 0.0f);
    g.setColour (cyan);
    g.strokePath (wobble, juce::PathStrokeType (2.0f * stroke, juce::PathStrokeType::mitered, juce::PathStrokeType::butt), lean);
    g.setColour (lime);
    g.strokePath (flat, juce::PathStrokeType (3.0f * stroke, juce::PathStrokeType::mitered, juce::PathStrokeType::butt), lean);
}

juce::Rectangle<float> PitchFlattenerLookAndFeel::drawPanel (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title,
                                                             juce::Colour accent, float headerHeight) const
{
    // The editor's panels: chamfered, a heading with speed stripes, a divider.
    Hud::frame (g, r, 10.0f, panel, panelEdge);

    auto headerRow = r.withHeight (headerHeight).reduced (14.0f, 0.0f);
    const auto font = m_fonts.display (12.0f);
    Hud::heading (g, headerRow.withSizeKeepingCentre (headerRow.getWidth(), 18.0f), title, font, accent);

    if (headerHeight < r.getHeight() - 1.0f) // a panel that is only a header row has no divider
    {
        g.setColour (panelEdge);
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
    if (Hud::skipOnScreen (slider))
        return;

    // An arc gauge, as on a cockpit: a ring of segments lit up to the value in the slider's accent,
    // a thin inner scale, and a needle tick at the value. Dim as a whole when it does nothing.
    const bool enabled = slider.isEnabled();
    const float alpha = enabled ? 1.0f : 0.28f;
    const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
    const bool hot = enabled && slider.isMouseOverOrDragging();

    const auto area = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
    const float radius = std::min (area.getWidth(), area.getHeight()) * 0.5f;
    const auto centre = area.getCentre();
    const float outer = radius, inner = radius * 0.8f;

    constexpr int segments = 25;
    const float span = rotaryEndAngle - rotaryStartAngle;
    const float gap = 0.022f * span;
    for (int i = 0; i < segments; ++i)
    {
        const float a0 = rotaryStartAngle + span * (float) i / (float) segments + gap * 0.5f;
        const float a1 = rotaryStartAngle + span * (float) (i + 1) / (float) segments - gap * 0.5f;
        const bool lit = ((float) i + 0.5f) / (float) segments <= sliderPos;
        juce::Path seg;
        seg.addCentredArc (centre.x, centre.y, outer, outer, 0.0f, a0, a1, true);
        seg.addCentredArc (centre.x, centre.y, inner, inner, 0.0f, a1, a0, false);
        seg.closeSubPath();
        const auto c = lit ? (hot ? amber : accent) : panelEdge;
        g.setColour (c.withMultipliedAlpha (alpha));
        g.fillPath (seg);
    }

    // Inner scale and the needle.
    juce::Path scale;
    const float scaleRadius = inner - 5.0f;
    scale.addCentredArc (centre.x, centre.y, scaleRadius, scaleRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (panelEdge.brighter (0.2f).withMultipliedAlpha (alpha));
    g.strokePath (scale, juce::PathStrokeType (1.0f));

    const float angle = rotaryStartAngle + sliderPos * span;
    const auto dir = juce::Point<float> (std::sin (angle), -std::cos (angle));
    g.setColour ((hot ? amber : text).withMultipliedAlpha (alpha));
    g.drawLine ({ centre + dir * (scaleRadius - 6.0f), centre + dir * (outer + 1.0f) }, 2.0f);
}

juce::Slider::SliderLayout PitchFlattenerLookAndFeel::getSliderLayout (juce::Slider& slider)
{
    juce::Slider::SliderLayout layout;
    layout.sliderBounds = slider.getLocalBounds();

    // The value sits in the middle of the gauge.
    if (slider.getTextBoxPosition() != juce::Slider::NoTextBox)
    {
        const auto b = slider.getLocalBounds();
        const int d = std::min (b.getWidth(), b.getHeight());
        layout.textBoxBounds = b.withSizeKeepingCentre (juce::roundToInt ((float) d * 0.62f), juce::roundToInt ((float) d * 0.42f));
    }
    return layout;
}

juce::Label* PitchFlattenerLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (m_fonts.display (16.0f));
    label->setJustificationType (juce::Justification::centred);
    label->setColour (juce::Label::textColourId, text);
    label->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);

    // The value box fills the gauge's middle: a drag that starts on it still turns the gauge (a click
    // without a drag types a value, as the box always did).
    label->addMouseListener (&slider, false);

    // While a value is typed: a chamfered field in the gauge's own accent (outline, selection, caret),
    // with the text centred like the value it replaces. Label::createEditorComponent() copies these.
    const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
    label->setColour (juce::Label::textWhenEditingColourId, text);
    label->setColour (juce::Label::backgroundWhenEditingColourId, panelRaised);
    label->setColour (juce::Label::outlineWhenEditingColourId, accent.withAlpha (0.8f));
    label->setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
    label->setColour (juce::TextEditor::highlightedTextColourId, text);
    label->setColour (juce::CaretComponent::caretColourId, accent);
    label->onEditorShow = [label, this]
    {
        if (auto* editor = label->getCurrentTextEditor())
        {
            editor->setJustification (juce::Justification::centred);
            editor->setIndents (2, 0);
            editor->applyFontToAllText (m_fonts.body (13.0f));
        }
    };
    return label;
}

void PitchFlattenerLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    if (Hud::skipOnScreen (label))
        return;

    auto* slider = dynamic_cast<juce::Slider*> (label.getParentComponent());
    if (slider == nullptr || label.isBeingEdited())
    {
        juce::LookAndFeel_V4::drawLabel (g, label);
        return;
    }

    // A gauge's value: the main part large ("G#6", "100%"), what follows it small ("+36 CENTS").
    const bool enabled = slider->isEnabled();
    const auto colour = (slider->isMouseOverOrDragging() && enabled ? amber : text).withMultipliedAlpha (enabled ? 1.0f : 0.4f);
    const auto value = label.getText().trim();
    const int split = value.indexOfChar (' ');
    const juce::String main = split > 0 ? value.substring (0, split) : value;
    juce::String sub = split > 0 ? value.substring (split + 1).trim() : juce::String();
    const bool isNote = main.isNotEmpty() && juce::CharacterFunctions::isLetter (main[0]);
    if (isNote)
        sub = (sub.isEmpty() ? juce::String ("0") : sub) + " CENTS";

    auto area = label.getLocalBounds().toFloat();
    const float mainHeight = Hud::fitHeight ([this] (float h) { return m_fonts.display (h); }, main, area.getWidth(), sub.isEmpty() ? 22.0f : 20.0f, 10.0f);
    if (sub.isEmpty())
    {
        Hud::drawSheared (g, main, area, m_fonts.display (mainHeight), colour, juce::Justification::centred);
        return;
    }
    auto top = area.removeFromTop (area.getHeight() * 0.62f);
    Hud::drawSheared (g, main, top, m_fonts.display (mainHeight), colour, juce::Justification::centredBottom);
    const auto subFont = m_fonts.label (Hud::fitHeight ([this] (float h) { return m_fonts.label (h); }, sub, area.getWidth(), 10.5f, 10.0f, false));
    Hud::drawText (g, sub, area, subFont, textDim.withMultipliedAlpha (enabled ? 1.0f : 0.5f), juce::Justification::centredTop);
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

    Hud::frame (g, r, 6.0f, box.findColour (juce::ComboBox::backgroundColourId).brighter (hot ? 0.08f : 0.0f),
                ring ? c_chrome : hot ? box.findColour (juce::ComboBox::focusedOutlineColourId) : box.findColour (juce::ComboBox::outlineColourId));

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
void PitchFlattenerLookAndFeel::setDialogButton (juce::Button& button, juce::Colour accent, bool primary)
{
    button.getProperties().set (c_dialogAccentProperty, (juce::int64) accent.getARGB());
    button.getProperties().set (c_dialogPrimaryProperty, primary);
    button.repaint();
}

bool PitchFlattenerLookAndFeel::isPrimaryDialogButton (const juce::Button& button)
{
    if (button.getProperties().contains (c_dialogAccentProperty))
        return (bool) button.getProperties().getWithDefault (c_dialogPrimaryProperty, false);

    return isInAlert (button) && button.isRegisteredForShortcut (juce::KeyPress (juce::KeyPress::returnKey));
}

juce::Colour PitchFlattenerLookAndFeel::dialogAccentFor (const juce::Button& button)
{
    if (const auto* argb = button.getProperties().getVarPointer (c_dialogAccentProperty))
        return juce::Colour ((juce::uint32) (juce::int64) *argb);

    if (auto* alert = dynamic_cast<const juce::AlertWindow*> (button.getParentComponent()))
        return accentFor (alert->getAlertType());

    return c_chrome;
}

void PitchFlattenerLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                                      const juce::Colour& backgroundColour,
                                                      bool isMouseOverButton, bool isButtonDown)
{
    if (Hud::skipOnScreen (button))
        return;
    if (backgroundColour.isTransparent())
        return; // a text link: no pill (see drawButtonText)

    const auto r = pillBounds (button);
    const float corner = std::min (8.0f, std::min (r.getWidth(), r.getHeight()) * 0.3f);   // the chamfer cut
    const bool enabled = button.isEnabled();
    const bool hot = isMouseOverButton && enabled;

    auto fill = backgroundColour;
    auto edge = hot ? amber : orange.withAlpha (0.7f);
    auto ringColour = c_chrome;

    if (isInAlert (button))
    {
        // The Return-key button is the dialog's primary action, tinted in the dialog's accent.
        const auto accent = dialogAccentFor (button);
        ringColour = accent;

        edge = hot ? accent : accent.withAlpha (0.55f);
        if (isPrimaryDialogButton (button))
        {
            fill = hot ? accent.brighter (0.15f) : accent;
            edge = fill;
        }
    }

    const float alpha = enabled ? 1.0f : c_disabledAlpha;

    Hud::frame (g, r, corner, fill.brighter (isButtonDown && enabled ? 0.2f : hot ? 0.1f : 0.0f).withMultipliedAlpha (alpha),
                shouldShowFocusRing (button) ? ringColour : edge.withMultipliedAlpha (alpha));
}

void PitchFlattenerLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool isMouseOverButton, bool isButtonDown)
{
    if (Hud::skipOnScreen (button))
        return;
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

    auto colour = button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
    if (isInAlert (button))
        colour = isPrimaryDialogButton (button) ? space : (isMouseOverButton ? amber : dialogAccentFor (button));
    else if (isMouseOverButton && button.isEnabled())
        colour = amber;
    if (! button.isEnabled())
        colour = colour.withMultipliedAlpha (0.5f);

    // Bold, wide and leaning forward, like every command in the plug-in; shrunk (never cut) to fit.
    const auto r = pillBounds (button);
    const auto area = r.reduced (std::max (6.0f, std::round (r.getHeight() * 0.3f)), 0.0f);
    const float h = Hud::fitHeight ([this] (float fh) { return m_fonts.display (fh); }, label, area.getWidth(), font.getHeight(), 10.0f);
    Hud::drawSheared (g, label, area, m_fonts.display (h), colour, juce::Justification::centred);
    juce::ignoreUnused (isButtonDown);
}

juce::Font PitchFlattenerLookAndFeel::getTextButtonFont (juce::TextButton& button, int buttonHeight)
{
    if (isInAlert (button))
        return m_fonts.display (12.0f);
    if (isLinkStyle (button))
        return m_fonts.body (12.0f);
    return m_fonts.display (std::min (12.0f, (float) buttonHeight * 0.45f));
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

    Hud::frame (g, track, 4.0f, (on ? accent.withAlpha (0.85f) : panelRaised.brighter (isMouseOverButton ? 0.08f : 0.0f)).withMultipliedAlpha (alpha),
                (shouldShowFocusRing (button) ? c_chrome : on ? accent : (isMouseOverButton ? textDim : outline)).withMultipliedAlpha (alpha));

    const float thumb = trackH - 6.0f + (isButtonDown ? 1.0f : 0.0f);
    const float thumbX = on ? track.getRight() - 3.0f - thumb : track.getX() + 3.0f;
    const auto thumbBounds = juce::Rectangle<float> (thumbX, track.getCentreY() - thumb * 0.5f, thumb, thumb);
    g.setColour ((on ? space : textDim).withMultipliedAlpha (alpha));
    g.fillRect (thumbBounds);

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

    Hud::frame (g, r, c_menuRadius * s, fill, outline.brighter (0.2f));
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
        g.fillPath (Hud::chamfer (row.reduced (0.0f, 1.0f), 4.0f * s));
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
    g.fillRect (juce::Rectangle<float> (dot, dot).withCentre ({ r.getX() + c_menuTickColumn * s * 0.5f + 1.0f * s, r.getCentreY() }));

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
    Hud::frame (g, bounds, c_dialogRadius, alert.findColour (juce::AlertWindow::backgroundColourId), accent);

    // Header row: speed stripes and the title in the accent, a divider.
    auto header = bounds.withHeight (c_dialogHeaderHeight).reduced (14.0f, 0.0f);
    const auto titleFont = m_fonts.display (12.0f);
    Hud::heading (g, header.withSizeKeepingCentre (header.getWidth(), 18.0f), fitToWidth (titleFont, alert.getName().toUpperCase(), header.getWidth() - 60.0f), titleFont, accent);
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
    g.fillPath (Hud::chamfer (juce::Rectangle<float> ((float) width, (float) height), fieldCut (height)));
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
        g.strokePath (Hud::chamfer (r, fieldCut (height)), juce::PathStrokeType (thickness));
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

    // No track: a thin bar.
    auto thumb = (isScrollbarVertical ? juce::Rectangle<int> (x, thumbStartPosition, width, thumbSize)
                                      : juce::Rectangle<int> (thumbStartPosition, y, thumbSize, height)).toFloat();
    thumb = isScrollbarVertical ? thumb.withSizeKeepingCentre (4.0f, std::max (4.0f, thumb.getHeight() - 4.0f))
                                : thumb.withSizeKeepingCentre (std::max (4.0f, thumb.getWidth() - 4.0f), 4.0f);

    g.setColour (isMouseOver || isMouseDown ? textDim : scrollbar.findColour (juce::ScrollBar::thumbColourId));
    g.fillRect (thumb);
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
    const int w = (int) std::ceil (layout.getWidth() + 2.0f * c_tipPadX + 2.0f);
    const int h = (int) std::ceil (layout.getHeight() + 2.0f * c_tipPadY);

    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6,
                                 w, h)
        .constrainedWithin (parentArea);
}

void PitchFlattenerLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& tipText, int width, int height)
{
    // The tooltip is small text, so it is drawn at 1x, dithered to 5 bits like the editor's
    // framebuffer and shown with no smoothing. A chamfered panel with an orange bar down its left
    // edge as a speed stripe; the window is transparent outside it.
    juce::Image image (juce::Image::ARGB, std::max (1, width), std::max (1, height), true, juce::SoftwareImageType());
    {
        juce::Graphics ig (image);
        const auto r = juce::Rectangle<float> ((float) width, (float) height);
        const auto fill = findColour (juce::TooltipWindow::backgroundColourId).withAlpha (1.0f);
        if (! juce::Desktop::canUseSemiTransparentWindows())
            ig.fillAll (fill);
        Hud::frame (ig, r, c_tipRadius, fill, findColour (juce::TooltipWindow::outlineColourId));
        ig.setColour (orange);
        ig.fillRect (juce::Rectangle<float> (3.0f, c_tipRadius + 2.0f, 2.0f, r.getHeight() - 2.0f * c_tipRadius - 4.0f));
        layoutTooltip (tipText, findColour (juce::TooltipWindow::textColourId)).draw (ig, r.reduced (c_tipPadX, c_tipPadY).translated (2.0f, 0.0f));
    }
    Framebuffer::dither (image);
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    g.drawImageAt (image, 0, 0);
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
            g.strokePath (Hud::chamfer (juce::Rectangle<float> ((float) width, (float) height).reduced (0.75f), 6.0f), juce::PathStrokeType (1.5f));
        }
    };

    return std::make_unique<juce::FocusOutline> (std::make_unique<Properties>());
}

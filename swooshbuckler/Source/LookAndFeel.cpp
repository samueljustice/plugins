#include "LookAndFeel.h"
#include "TypedValue.h"

namespace TC = Theme::Colours;

namespace
{
    constexpr float c_startAngle = juce::MathConstants<float>::pi * -0.75f;
    constexpr float c_endAngle = juce::MathConstants<float>::pi * 0.75f;

    // One rubber-band step at the UI frame rate. Returns true while still moving.
    bool springStep(float& shown, float& velocity, float target, float hz = 4.5f, float damping = 0.5f)
    {
        constexpr float dt = 1.0f / 60.0f;
        const float omega = juce::MathConstants<float>::twoPi * hz;
        velocity += (omega * omega * (target - shown) - 2.0f * damping * omega * velocity) * dt;
        shown += velocity * dt;
        if (std::abs(target - shown) < 1.0e-4f && std::abs(velocity) < 1.0e-3f)
        {
            shown = target;
            velocity = 0.0f;
            return false;
        }
        return true;
    }
}

SwooshLookAndFeel::SwooshLookAndFeel(const Theme::Fonts& fonts) : m_fonts(fonts)
{
    setColour(juce::ResizableWindow::backgroundColourId, TC::window);
    setColour(juce::Label::textColourId, TC::text);
    setColour(juce::TextButton::buttonColourId, TC::raised);
    setColour(juce::TextButton::buttonOnColourId, TC::brass);
    setColour(juce::TextButton::textColourOffId, TC::text);
    setColour(juce::TextButton::textColourOnId, TC::text);
    setColour(juce::ComboBox::backgroundColourId, TC::raised);
    setColour(juce::ComboBox::outlineColourId, TC::stroke);
    setColour(juce::ComboBox::textColourId, TC::text);
    setColour(juce::ComboBox::arrowColourId, TC::textDim);
    // Not opaque, so a menu's window can have round corners (it is opaque where windows cannot be see-through).
    setColour(juce::PopupMenu::backgroundColourId, TC::raised.withAlpha(0.0f));
    setColour(juce::PopupMenu::textColourId, TC::text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, TC::brass.withAlpha(0.2f));
    setColour(juce::PopupMenu::highlightedTextColourId, TC::text);
    setColour(juce::ToggleButton::textColourId, TC::text);
    setColour(juce::ToggleButton::tickColourId, TC::brass);
    setColour(juce::AlertWindow::backgroundColourId, TC::panel);
    setColour(juce::AlertWindow::textColourId, TC::text);
    setColour(juce::AlertWindow::outlineColourId, TC::stroke);
    setColour(juce::TextEditor::backgroundColourId, TC::well);
    setColour(juce::TextEditor::textColourId, TC::text);
    setColour(juce::TextEditor::highlightColourId, TC::brass.withAlpha(0.35f));
    setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::focusedOutlineColourId, TC::brass);
    setColour(juce::CaretComponent::caretColourId, TC::brass);
    setColour(juce::TooltipWindow::backgroundColourId, TC::raised);
    setColour(juce::TooltipWindow::textColourId, TC::text);
    setColour(juce::TooltipWindow::outlineColourId, TC::stroke);
    setColour(juce::Slider::textBoxTextColourId, TC::text);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::HyperlinkButton::textColourId, TC::textDim);
    setColour(juce::ScrollBar::thumbColourId, TC::stroke);
}

//==============================================================================
void SwooshLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                         float, float, juce::Slider::SliderStyle style, juce::Slider& slider)
{
    // A fader: a groove cut into the surface, filled in the control's colour up to a sand thumb.
    const bool enabled = slider.isEnabled();
    const auto accent = enabled ? slider.findColour(juce::Slider::trackColourId) : TC::textFaint.withAlpha(0.5f);
    const auto r = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height));
    const bool vertical = style == juce::Slider::LinearVertical;
    const bool over = slider.isMouseOverOrDragging();
    const float thumb = vertical ? 14.0f : 12.0f;
    const auto groove = vertical ? r.withSizeKeepingCentre(5.0f, r.getHeight()) : r.withSizeKeepingCentre(r.getWidth(), 5.0f);
    const auto filled = vertical ? groove.withTop(sliderPos) : groove.withRight(sliderPos);
    const juce::Point<float> at = vertical ? juce::Point<float>(r.getCentreX(), sliderPos) : juce::Point<float>(sliderPos, r.getCentreY());

    g.setColour(TC::well);
    g.fillRoundedRectangle(groove.expanded(vertical ? 0.0f : 6.0f, vertical ? 7.0f : 0.0f), 2.5f);
    g.setColour(accent.withAlpha(0.9f));
    g.fillRoundedRectangle(filled.expanded(vertical ? 0.0f : 3.0f, vertical ? 3.0f : 0.0f), 2.5f);
    g.setColour(TC::shadow.withAlpha(0.45f));
    g.fillEllipse(juce::Rectangle<float>(thumb, thumb).withCentre(at.translated(0.0f, 1.0f)));
    g.setColour(enabled ? (over ? TC::text.brighter(0.2f) : TC::text) : TC::textFaint);
    g.fillEllipse(juce::Rectangle<float>(thumb, thumb).withCentre(at));
    g.setColour(enabled ? accent.darker(0.2f) : TC::well);
    g.fillEllipse(juce::Rectangle<float>(thumb * 0.36f, thumb * 0.36f).withCentre(at));
}

juce::Slider::SliderLayout SwooshLookAndFeel::getSliderLayout(juce::Slider& slider)
{
    juce::Slider::SliderLayout layout;
    layout.sliderBounds = slider.getLocalBounds();
    // The thumb stays inside the control at both ends of its travel.
    if (slider.getSliderStyle() == juce::Slider::LinearHorizontal)
        layout.sliderBounds = layout.sliderBounds.reduced(6, 0);
    else if (slider.getSliderStyle() == juce::Slider::LinearVertical)
        layout.sliderBounds = layout.sliderBounds.reduced(0, 7);
    return layout;
}

juce::Label* SwooshLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* l = LookAndFeel_V4::createSliderTextBox(slider);
    l->setFont(m_fonts.caption());
    return l;
}

namespace
{
    /** The face of a plate that can be pressed: it sits a little above its own hard shadow, and down on it when pressed. */
    juce::Rectangle<float> plateFace(juce::Rectangle<float> r, bool down)
    {
        return down ? r.withTrimmedTop(1.5f) : r.withTrimmedBottom(1.5f);
    }

    /** The plate of anything raised (a button, a menu): its fill by state, a hard little shadow under it so it
        reads as a key you can press, and a stroke only when it is hovered, on, focused or resting. */
    void drawPlate(juce::Graphics& g, juce::Rectangle<float> r, float radius, juce::Colour accent, Theme::Look look,
                   bool on, bool over, bool down, bool focused = false)
    {
        if (look == Theme::Look::Disabled || look == Theme::Look::Linked)
        {
            g.setColour(TC::raised.withAlpha(0.45f));
            g.fillRoundedRectangle(plateFace(r, false), radius);
            return;
        }
        if (look == Theme::Look::Resting)
        {
            // Hollow: it can be set, and waits for something else to be switched on.
            const auto face = plateFace(r, down);
            g.setColour(TC::raised.withAlpha(over ? 0.7f : 0.35f));
            g.fillRoundedRectangle(face, radius);
            juce::Path edge, dashed;
            edge.addRoundedRectangle(face.reduced(0.5f), radius);
            const float dashes[] = { 3.0f, 3.0f };
            juce::PathStrokeType(1.0f).createDashedStroke(dashed, edge, dashes, 2);
            g.setColour(on ? accent.withAlpha(0.8f) : TC::stroke);
            g.fillPath(dashed);
            return;
        }
        if (! down)
        {
            g.setColour(TC::shadow.withAlpha(0.55f));
            g.fillRoundedRectangle(r.withTrimmedTop(2.0f), radius);
        }
        const auto face = plateFace(r, down);
        // A solid base first, so nothing behind shows through a lit button (the moon behind Morph).
        g.setColour(over && ! on ? TC::hover : TC::raised);
        g.fillRoundedRectangle(face, radius);
        if (on)
        {
            g.setColour(accent.withAlpha(over ? 0.34f : 0.24f));
            g.fillRoundedRectangle(face, radius);
        }
        if (on || over || focused)
        {
            g.setColour(focused ? TC::brass : (on ? accent.withAlpha(0.9f) : TC::stroke));
            g.drawRoundedRectangle(face.reduced(0.5f), radius, 1.0f);
        }
    }

    /** The little open arrow that says "this opens a menu". */
    void drawChevron(juce::Graphics& g, juce::Point<float> centre, juce::Colour colour)
    {
        juce::Path p;
        p.startNewSubPath(centre.x - 3.5f, centre.y - 1.8f);
        p.lineTo(centre.x, centre.y + 1.8f);
        p.lineTo(centre.x + 3.5f, centre.y - 1.8f);
        g.setColour(colour);
        g.strokePath(p, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void SwooshLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& background,
                                             bool over, bool down)
{
    const auto r = button.getLocalBounds().toFloat();
    const auto accent = button.findColour(juce::TextButton::buttonOnColourId);
    const bool on = button.getToggleState();
    const auto look = Theme::getLook(button);
    const auto style = getStyle(button);

    if (style == ButtonStyle::Chip)
    {
        // A chip is a state, not a key: a pill, dark and quiet when off, filled in its colour when on.
        const float radius = juce::jmin(r.getHeight() * 0.5f, 9.0f);
        const bool usable = look == Theme::Look::Normal || look == Theme::Look::Resting;
        g.setColour(! usable ? TC::well.withAlpha(0.4f) : (on ? accent : (over ? TC::hover : TC::well.withAlpha(0.75f))));
        g.fillRoundedRectangle(r, radius);
        if (usable && over && ! on)
        {
            g.setColour(TC::stroke);
            g.drawRoundedRectangle(r.reduced(0.5f), radius, 1.0f);
        }
        return;
    }
    if (style == ButtonStyle::Primary)
    {
        const float radius = Theme::Radius::control + 2.0f;
        if (look != Theme::Look::Normal)
        {
            // Not to be pressed now: the brass is gone, and so is the shadow that makes it a key.
            g.setColour(TC::raised.withAlpha(0.45f));
            g.fillRoundedRectangle(r.withTrimmedBottom(2.0f), radius);
            return;
        }
        if (! down)
        {
            g.setColour(TC::brassDark);
            g.fillRoundedRectangle(r.withTrimmedTop(3.0f), radius);
        }
        g.setColour(over ? TC::brass.brighter(0.25f) : TC::brass);
        g.fillRoundedRectangle(down ? r.withTrimmedTop(2.0f) : r.withTrimmedBottom(2.0f), radius);
        return;
    }
    drawPlate(g, r, Theme::Radius::control, accent, look, on, over, down);
    // A button whose plate has a colour of its own (Throw) keeps it as a tint.
    if (background != TC::raised && look == Theme::Look::Normal && ! on)
    {
        g.setColour(background);
        g.fillRoundedRectangle(plateFace(r, down), Theme::Radius::control);
    }
}

void SwooshLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool over, bool down)
{
    const bool on = button.getToggleState();
    const auto accent = button.findColour(juce::TextButton::buttonOnColourId);
    const auto look = Theme::getLook(button);
    const auto style = getStyle(button);
    const auto icon = Icons::get(button);
    const auto text = button.getButtonText();
    const bool single = text.length() <= 2; // a single-letter chip (A, B, M, S)
    // A button that switches something (and is not a chip, whose fill says it) carries a lamp.
    // One of a row of choices (a radio group) is lit by its plate alone.
    const bool lamp = style == ButtonStyle::Button && button.getClickingTogglesState() && icon == Icons::Icon::None
                      && button.getRadioGroupId() == 0 && ! button.getProperties().contains("plain");
    const bool chevron = style == ButtonStyle::Menu;

    juce::Colour colour;
    if (look == Theme::Look::Disabled || look == Theme::Look::Linked)
        colour = TC::textFaint.withAlpha(0.55f);
    else if (style == ButtonStyle::Primary || (style == ButtonStyle::Chip && on))
        colour = TC::window;
    else if (on)
        colour = accent.brighter(0.45f);
    else if (style == ButtonStyle::Chip)
        colour = over ? TC::text : TC::textDim;
    else if (look == Theme::Look::Resting)
        colour = TC::textDim;
    else
        colour = button.findColour(juce::TextButton::textColourOffId);

    auto r = button.getLocalBounds().toFloat();
    if (style == ButtonStyle::Primary)
        r = down ? r.withTrimmedTop(2.0f) : r.withTrimmedBottom(2.0f);
    else if (style != ButtonStyle::Chip)
        r = plateFace(r, down);
    const auto font = getTextButtonFont(button, button.getHeight());
    const auto fit = static_cast<bool>(button.getProperties().getWithDefault("name", false)) ? Theme::Fit::Name : Theme::Fit::Whole;
    const float pad = single ? 1.0f : (r.getWidth() < 50.0f ? 4.0f : 7.0f);
    const float iconSide = icon != Icons::Icon::None ? juce::jmin(13.0f, r.getHeight() - 6.0f) : 0.0f;
    const float lead = icon != Icons::Icon::None ? iconSide : (lamp ? 6.0f : 0.0f);
    const float gap = lead > 0.0f && text.isNotEmpty() ? 5.0f : 0.0f;
    const float tail = chevron ? 13.0f : 0.0f;
    const float room = r.getWidth() - 2.0f * pad - lead - gap - tail;
    // The lamp or the icon, then the text, centred together (a menu's text starts at the left).
    const float textWidth = juce::jmin(room, Theme::fitToWidth(text, font, room, fit).width);
    const float x = chevron ? r.getX() + pad + 1.0f : juce::jmax(r.getX() + pad, r.getCentreX() - (lead + gap + textWidth) * 0.5f);

    if (icon != Icons::Icon::None)
        Icons::draw(g, icon, juce::Rectangle<float>(x, r.getCentreY() - iconSide * 0.5f, iconSide, iconSide), colour, on);
    else if (lamp)
    {
        const auto dot = juce::Rectangle<float>(6.0f, 6.0f).withCentre({ x + 3.0f, r.getCentreY() });
        if (on && look == Theme::Look::Normal)
        {
            g.setColour(accent.withAlpha(0.3f));
            g.fillEllipse(dot.expanded(2.5f));
            g.setColour(accent.brighter(0.3f));
            g.fillEllipse(dot);
        }
        else
        {
            g.setColour(colour.withMultipliedAlpha(0.6f));
            g.drawEllipse(dot.reduced(0.5f), 1.2f);
        }
    }
    g.setColour(colour);
    const float left = x + lead + gap;
    Theme::fitText(g, text, juce::Rectangle<float>(left, r.getY(), r.getRight() - pad - tail - left, r.getHeight()), font,
                   juce::Justification::centredLeft, fit);
    if (chevron)
        drawChevron(g, { r.getRight() - 11.0f, r.getCentreY() }, colour.withMultipliedAlpha(0.8f));
}

juce::Font SwooshLookAndFeel::getTextButtonFont(juce::TextButton& button, int)
{
    // Low buttons and chips are set in the caption size; the rest in the control size.
    return button.getHeight() < 20 ? m_fonts.caption() : m_fonts.control();
}

void SwooshLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool over, bool)
{
    // A switch: a pill with a thumb that sits right, in the control's colour, when on.
    auto r = button.getLocalBounds().toFloat();
    const auto accent = button.findColour(juce::ToggleButton::tickColourId);
    const bool on = button.getToggleState();
    const auto look = Theme::getLook(button);
    const bool usable = look == Theme::Look::Normal || look == Theme::Look::Resting;
    const float trackH = juce::jmin(16.0f, r.getHeight() - 2.0f), trackW = std::floor(trackH * 1.75f);
    const auto track = juce::Rectangle<float>(r.getX() + 1.0f, r.getCentreY() - trackH * 0.5f, trackW, trackH);
    const float thumbD = trackH - 5.0f;
    const auto thumb = juce::Rectangle<float>(thumbD, thumbD).withCentre({ on ? track.getRight() - trackH * 0.5f : track.getX() + trackH * 0.5f, track.getCentreY() });

    g.setColour(! usable ? TC::well.withAlpha(0.5f) : (on ? accent : TC::well));
    g.fillRoundedRectangle(track, trackH * 0.5f);
    if (usable && (over || ! on))
    {
        g.setColour(over ? (on ? accent.brighter(0.4f) : TC::stroke.brighter(0.3f)) : TC::stroke);
        g.drawRoundedRectangle(track.reduced(0.5f), trackH * 0.5f, 1.0f);
    }
    g.setColour(! usable ? TC::textFaint.withAlpha(0.5f) : (on ? TC::window : TC::textDim));
    g.fillEllipse(thumb);

    if (button.getButtonText().isNotEmpty())
    {
        g.setColour(! usable ? TC::textFaint.withAlpha(0.55f) : (on || over ? TC::text : TC::textDim));
        Theme::fitText(g, button.getButtonText(), r.withTrimmedLeft(trackW + 8.0f), m_fonts.control(), juce::Justification::centredLeft);
    }
}

void SwooshLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool down, int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    const auto look = Theme::getLook(box);
    drawPlate(g, r, Theme::Radius::control, TC::brass, look, false, box.isMouseOver(true), down, box.hasKeyboardFocus(false));
    const juce::Point<float> mark { r.getRight() - 11.0f, plateFace(r, down).getCentreY() };
    if (look == Theme::Look::Linked)
        Icons::draw(g, Icons::Icon::Link, juce::Rectangle<float>(12.0f, 12.0f).withCentre(mark), TC::textDim);
    else
        drawChevron(g, mark, look == Theme::Look::Disabled ? TC::textFaint.withAlpha(0.5f) : TC::textDim);
}

juce::Font SwooshLookAndFeel::getComboBoxFont(juce::ComboBox&) { return m_fonts.control(); }

void SwooshLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(8, 0, box.getWidth() - 8 - 18, box.getHeight() - 1);
    label.setFont(getComboBoxFont(box));
    label.setBorderSize({});
}

juce::Font SwooshLookAndFeel::getPopupMenuFont() { return m_fonts.control(); }
juce::Font SwooshLookAndFeel::getLabelFont(juce::Label&) { return m_fonts.control(); }

void SwooshLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
{
    g.fillAll(label.findColour(juce::Label::backgroundColourId));
    if (label.isBeingEdited())
        return;
    // The label inside a menu takes the menu's look (a linked or switched-off menu reads fainter).
    const auto* owner = dynamic_cast<juce::ComboBox*>(label.getParentComponent());
    const auto look = owner != nullptr ? Theme::getLook(*owner) : Theme::getLook(label);
    g.setColour(look == Theme::Look::Disabled || look == Theme::Look::Linked ? TC::textFaint.withAlpha(0.55f)
                : look == Theme::Look::Resting ? TC::textDim : label.findColour(juce::Label::textColourId));
    Theme::fitText(g, label.getText(), label.getBorderSize().subtractedFrom(label.getLocalBounds()).toFloat(), getLabelFont(label),
                   label.getJustificationType(), owner != nullptr ? Theme::Fit::Whole : Theme::Fit::Name);
}

void SwooshLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    if (! juce::Desktop::canUseSemiTransparentWindows())
        g.fillAll(TC::raised); // the window is opaque: its corners are filled too
    g.setColour(TC::raised.brighter(0.03f));
    g.fillRoundedRectangle(r, 10.0f);
}

void SwooshLookAndFeel::drawPopupMenuUpDownArrow(juce::Graphics& g, int width, int height, bool isScrollUpArrow)
{
    // A menu too long for the screen scrolls: the arrow sits on the menu's own colour, fading into the items.
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    g.setGradientFill(juce::ColourGradient(TC::raised, 0.0f, h * 0.5f, TC::raised.withAlpha(0.0f), 0.0f, isScrollUpArrow ? h : 0.0f, false));
    g.fillRect(0.0f, 0.0f, w, h);
    drawChevron(g, { w * 0.5f, h * 0.5f }, TC::textDim);
    if (isScrollUpArrow) // the chevron points down: turn it over
    {
        g.setColour(TC::raised);
        g.fillRect(w * 0.5f - 6.0f, h * 0.5f - 4.0f, 12.0f, 8.0f);
        juce::Path p;
        p.startNewSubPath(w * 0.5f - 3.5f, h * 0.5f + 1.8f);
        p.lineTo(w * 0.5f, h * 0.5f - 1.8f);
        p.lineTo(w * 0.5f + 3.5f, h * 0.5f + 1.8f);
        g.setColour(TC::textDim);
        g.strokePath(p, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void SwooshLookAndFeel::drawPopupMenuSectionHeader(juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& name)
{
    g.setColour(TC::brass);
    Theme::fitText(g, name, area.reduced(14, 0).withTrimmedBottom(3).toFloat(), m_fonts.caption(), juce::Justification::bottomLeft);
}

void SwooshLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                          bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String&,
                                          const juce::Drawable*, const juce::Colour*)
{
    if (isSeparator)
    {
        drawRule(g, { static_cast<float>(area.getX() + 12), static_cast<float>(area.getCentreY()) },
                 { static_cast<float>(area.getRight() - 12), static_cast<float>(area.getCentreY()) });
        return;
    }
    auto r = area.reduced(4, 1);
    if (isHighlighted && isActive)
    {
        g.setColour(TC::brass.withAlpha(0.2f));
        g.fillRoundedRectangle(r.toFloat(), Theme::Radius::control);
    }
    const auto tick = r.removeFromLeft(22);
    if (isTicked)
    {
        g.setColour(TC::brass);
        g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre(tick.toFloat().getCentre()));
    }
    if (hasSubMenu)
    {
        const auto arrow = r.removeFromRight(16).toFloat().getCentre();
        juce::Path p;
        p.startNewSubPath(arrow.x - 1.8f, arrow.y - 3.5f);
        p.lineTo(arrow.x + 1.8f, arrow.y);
        p.lineTo(arrow.x - 1.8f, arrow.y + 3.5f);
        g.setColour(isActive ? TC::textDim : TC::textFaint.withAlpha(0.5f));
        g.strokePath(p, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.setColour(! isActive ? TC::textFaint.withAlpha(0.6f) : (isHighlighted ? TC::text : TC::text.withAlpha(0.9f)));
    Theme::fitText(g, text, r.withTrimmedRight(8).toFloat(), m_fonts.control(), juce::Justification::centredLeft);
}

void SwooshLookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int, int& idealWidth, int& idealHeight)
{
    idealHeight = isSeparator ? 10 : 26;
    idealWidth = juce::GlyphArrangement::getStringWidthInt(m_fonts.control(), text) + 58;
}

void SwooshLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool vertical,
                                      int thumbStart, int thumbSize, bool over, bool down)
{
    // A quiet pill in a groove: no arrows, no frame.
    const auto track = juce::Rectangle<int>(x, y, width, height).toFloat();
    const float thick = juce::jmin(6.0f, vertical ? track.getWidth() : track.getHeight());
    const auto groove = vertical ? track.withSizeKeepingCentre(thick, track.getHeight()) : track.withSizeKeepingCentre(track.getWidth(), thick);
    g.setColour(TC::well.withAlpha(0.6f));
    g.fillRoundedRectangle(groove, thick * 0.5f);
    if (thumbSize <= 0)
        return;
    const auto thumb = vertical ? groove.withY(static_cast<float>(thumbStart)).withHeight(static_cast<float>(thumbSize))
                                : groove.withX(static_cast<float>(thumbStart)).withWidth(static_cast<float>(thumbSize));
    g.setColour(down ? TC::brass : (over ? TC::stroke.brighter(0.4f) : TC::stroke));
    g.fillRoundedRectangle(thumb, thick * 0.5f);
}

void SwooshLookAndFeel::drawCornerResizer(juce::Graphics& g, int w, int h, bool over, bool dragging)
{
    // Three brass dots in the corner: quiet until the mouse is on them.
    g.setColour(TC::brass.withAlpha(over || dragging ? 0.9f : 0.45f));
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    for (const auto& p : { juce::Point<float>(0.72f, 0.72f), juce::Point<float>(0.72f, 0.36f), juce::Point<float>(0.36f, 0.72f) })
        g.fillEllipse(juce::Rectangle<float>(2.6f, 2.6f).withCentre({ fw * p.x, fh * p.y }));
}

void SwooshLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    // A text box paints every pixel of itself: its corners take the colour of the panel it is cut into.
    g.fillAll(TC::panel);
    g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), Theme::Radius::small);
}

void SwooshLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    // A stroke only while it has the keyboard (or the mouse is over it).
    const bool focused = editor.hasKeyboardFocus(true) && ! editor.isReadOnly();
    if (! focused && ! editor.isMouseOver(true))
        return;
    g.setColour(focused ? editor.findColour(juce::TextEditor::focusedOutlineColourId) : TC::stroke);
    g.drawRoundedRectangle(juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f),
                           Theme::Radius::small, 1.0f);
}

namespace
{
    // Tooltips: "Heading\nWhat it does\nHow to handle it" - the first line reads as a heading, an optional last line dimmer.
    constexpr float c_tipWidth = 380.0f, c_tipPadX = 12.0f, c_tipPadY = 8.0f;

    juce::TextLayout layoutTip(const juce::String& rawText, const Theme::Fonts& fonts, float width)
    {
        const auto text = Theme::localiseShortcuts(rawText);
        juce::AttributedString s;
        s.setWordWrap(juce::AttributedString::byWord);
        s.setLineSpacing(1.5f);
        const int nl = text.indexOfChar('\n');
        if (nl > 0)
        {
            // Heading, body, and (with a third part) a dimmer how-to line.
            s.append(text.substring(0, nl + 1), fonts.control(), TC::brass);
            const auto rest = text.substring(nl + 1);
            const int last = rest.lastIndexOfChar('\n');
            if (last > 0)
            {
                s.append(rest.substring(0, last + 1), fonts.body(), TC::text);
                s.append(rest.substring(last + 1), fonts.body(), TC::textDim);
            }
            else
            {
                s.append(rest, fonts.body(), TC::text);
            }
        }
        else
        {
            s.append(text, fonts.body(), TC::text);
        }
        juce::TextLayout layout;
        layout.createLayout(s, width);
        return layout;
    }
}

void SwooshLookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height)
{
    // The tooltip's window is a plain rectangle: filled edge to edge, with a quiet brass edge (it floats over anything).
    const auto r = juce::Rectangle<float>(0, 0, static_cast<float>(width), static_cast<float>(height));
    g.fillAll(TC::raised);
    g.setColour(TC::brass.withAlpha(0.5f));
    g.drawRect(r, 1.0f);
    const auto inner = r.reduced(c_tipPadX, c_tipPadY);
    layoutTip(text, m_fonts, inner.getWidth()).draw(g, inner);
}

juce::Rectangle<int> SwooshLookAndFeel::getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                                         juce::Rectangle<int> parentArea)
{
    // Short tips hug their text; long ones wrap at c_tipWidth.
    float natural = 0.0f;
    juce::StringArray lines;
    lines.addLines(Theme::localiseShortcuts(tipText));
    for (int i = 0; i < lines.size(); ++i)
        natural = juce::jmax(natural, static_cast<float>(juce::GlyphArrangement::getStringWidthInt(
                                          i == 0 && lines.size() > 1 ? m_fonts.control() : m_fonts.body(), lines[i])));
    const float textWidth = juce::jmin(c_tipWidth, natural + 4.0f);
    const auto layout = layoutTip(tipText, m_fonts, textWidth);
    const int width = static_cast<int>(std::ceil(layout.getWidth() + 2.0f * c_tipPadX)) + 2;
    const int height = static_cast<int>(std::ceil(layout.getHeight() + 2.0f * c_tipPadY)) + 1;
    return juce::Rectangle<int>(screenPos.x > parentArea.getCentreX() ? screenPos.x - (width + 12) : screenPos.x + 24,
                                screenPos.y > parentArea.getCentreY() ? screenPos.y - (height + 6) : screenPos.y + 6,
                                width, height).constrainedWithin(parentArea);
}

//==============================================================================
void SwooshLookAndFeel::drawSurface(juce::Graphics& g, juce::Rectangle<float> area, float radius, juce::Colour fill, float elevation)
{
    if (! g.clipRegionIntersects(area.expanded(8.0f, 10.0f).getSmallestIntegerContainer()))
        return; // nothing of it is being repainted
    // The shadow: a few wider, fainter copies below it. Cheap (no blur), and soft enough at this size.
    if (elevation > 0.0f)
    {
        const float alphas[] = { 0.07f, 0.10f, 0.16f };
        const float spreads[] = { 5.0f, 3.0f, 1.0f };
        for (int i = 0; i < 3; ++i)
        {
            g.setColour(TC::shadow.withAlpha(juce::jmin(1.0f, alphas[i] * elevation)));
            g.fillRoundedRectangle(area.expanded(spreads[i]).translated(0.0f, 1.0f + spreads[i] * 0.6f), radius + spreads[i]);
        }
    }
    g.setGradientFill(juce::ColourGradient(fill.brighter(0.045f), area.getX(), area.getY(), fill, area.getX(), area.getY() + 56.0f, false));
    g.fillRoundedRectangle(area, radius);
}

void SwooshLookAndFeel::drawWell(juce::Graphics& g, juce::Rectangle<float> area, float radius)
{
    g.setColour(TC::well);
    g.fillRoundedRectangle(area, radius);
}

int SwooshLookAndFeel::titleEnd(const Theme::Fonts& fonts, const juce::String& title)
{
    return c_titleX + juce::roundToInt(std::ceil(juce::GlyphArrangement::getStringWidth(fonts.display(), title)));
}

void SwooshLookAndFeel::drawPanel(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& title,
                                  const juce::String& subtitle, juce::Colour accent, const Theme::Fonts& fonts,
                                  Icons::Icon roundel, float subtitleRight)
{
    drawSurface(g, area);
    if (title.isEmpty())
        return;
    const auto bar = area.withHeight(static_cast<float>(Theme::Space::titleBar));
    if (! g.clipRegionIntersects(bar.getSmallestIntegerContainer()))
        return;

    // The roundel: a disc in the section's colour with its mark (or its letter) cut out dark.
    const auto disc = juce::Rectangle<float>(24.0f, 24.0f).withCentre({ area.getX() + 26.0f, area.getY() + 20.0f });
    if (roundel != Icons::Icon::None)
    {
        g.setColour(accent);
        g.fillEllipse(disc);
        if (roundel == Icons::Icon::LetterA || roundel == Icons::Icon::LetterB)
        {
            g.setColour(TC::window);
            Theme::fitText(g, roundel == Icons::Icon::LetterA ? "A" : "B", disc, fonts.title(), juce::Justification::centred);
        }
        else
            Icons::draw(g, roundel, disc.reduced(5.0f), TC::window);
    }
    else
    {
        g.setColour(accent);
        g.fillRoundedRectangle(area.getX() + 24.0f, area.getY() + 11.0f, 4.0f, 18.0f, 2.0f);
    }

    const float x = area.getX() + static_cast<float>(c_titleX);
    g.setColour(TC::text);
    const float titleWidth = Theme::fitText(g, title, juce::Rectangle<float>(x, area.getY() + 6.0f, area.getRight() - 16.0f - x, 28.0f),
                                            fonts.display(), juce::Justification::centredLeft);
    if (subtitle.isNotEmpty())
    {
        const float sx = x + titleWidth + 12.0f;
        const float right = subtitleRight > 0.0f ? subtitleRight : area.getRight() - 16.0f;
        g.setColour(TC::textFaint);
        Theme::fitText(g, subtitle, juce::Rectangle<float>(sx, area.getY() + 11.0f, right - sx, 20.0f), fonts.body(), juce::Justification::centredLeft,
                       Theme::Fit::Sentence);
    }
}

void SwooshLookAndFeel::drawRope(juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, juce::Colour colour)
{
    const auto line = juce::Line<float>(a, b);
    const float length = line.getLength();
    if (length < 2.0f || ! g.clipRegionIntersects(juce::Rectangle<float>(a, b).expanded(4.0f).getSmallestIntegerContainer()))
        return;

    // Two strands twisted together: a run of slanted, round-ended strokes, each lit along its upper edge.
    const auto dir = (b - a) / length;
    const juce::Point<float> normal { -dir.y, dir.x };
    juce::Path strands, lights;
    for (float t = 2.0f; t < length - 1.0f; t += 4.5f)
    {
        const auto c = a + dir * t;
        strands.startNewSubPath(c - normal * 1.5f - dir * 1.7f);
        strands.lineTo(c + normal * 1.5f + dir * 1.7f);
        lights.startNewSubPath(c - normal * 1.3f - dir * 1.4f);
        lights.lineTo(c - normal * 0.2f);
    }
    g.setColour(colour.withMultipliedAlpha(0.55f));
    g.strokePath(strands, juce::PathStrokeType(2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(colour.brighter(0.5f).withMultipliedAlpha(0.55f));
    g.strokePath(lights, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void SwooshLookAndFeel::drawRule(juce::Graphics& g, juce::Point<float> a, juce::Point<float> b)
{
    g.setColour(TC::outlineSoft);
    g.drawLine(juce::Line<float>(a, b), 1.0f);
}

void SwooshLookAndFeel::drawCutlasses(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour blade, juce::Colour hilt)
{
    auto drawOne = [&](bool mirror)
    {
        const auto t = mirror ? juce::AffineTransform::scale(-1.0f, 1.0f, area.getCentreX(), 0.0f) : juce::AffineTransform();
        const float x0 = area.getX(), y0 = area.getY(), w = area.getWidth(), h = area.getHeight();

        juce::Path b;
        b.startNewSubPath(x0 + 0.18f * w, y0 + 0.84f * h);
        b.quadraticTo(x0 + 0.55f * w, y0 + 0.55f * h, x0 + 0.95f * w, y0 + 0.02f * h);
        b.quadraticTo(x0 + 0.72f * w, y0 + 0.50f * h, x0 + 0.24f * w, y0 + 0.90f * h);
        b.closeSubPath();
        g.setColour(blade);
        g.fillPath(b, t);

        juce::Path guard;
        guard.startNewSubPath(x0 + 0.08f * w, y0 + 0.74f * h);
        guard.quadraticTo(x0 + 0.24f * w, y0 + 0.80f * h, x0 + 0.32f * w, y0 + 0.97f * h);
        g.setColour(hilt);
        g.strokePath(guard, juce::PathStrokeType(juce::jmax(1.5f, w * 0.07f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded), t);

        juce::Path grip;
        grip.startNewSubPath(x0 + 0.20f * w, y0 + 0.87f * h);
        grip.lineTo(x0 + 0.04f * w, y0 + 1.0f * h);
        g.strokePath(grip, juce::PathStrokeType(juce::jmax(1.5f, w * 0.06f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded), t);
    };
    drawOne(false);
    drawOne(true);
}

//==============================================================================
WheelKnob::WheelKnob(const Theme::Fonts& fonts, juce::Colour accent) : m_fonts(fonts), m_accent(accent)
{
    setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    setRotaryParameters(c_startAngle, c_endAngle, true);
    setVelocityBasedMode(false);
    setMouseDragSensitivity(220);
}

juce::Point<float> WheelKnob::getDialCentre() const
{
    auto bounds = getLocalBounds().toFloat();
    bounds.removeFromBottom(26.0f);
    return bounds.getCentre();
}

float WheelKnob::getDialRadius() const
{
    auto bounds = getLocalBounds().toFloat();
    bounds.removeFromBottom(26.0f);
    return juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f - 2.0f;
}

//==============================================================================
ValueSlider::ValueSlider() = default;
ValueSlider::~ValueSlider() = default;

void ValueSlider::setTypedParameter(juce::RangedAudioParameter* parameter, const Theme::Fonts& fonts)
{
    m_typedParameter = parameter;
    m_typedFonts = &fonts;
}

bool ValueSlider::isTyping() const { return m_typing != nullptr && m_typing->isVisible(); }

void ValueSlider::beginTyping()
{
    if (m_typedParameter == nullptr || m_typedFonts == nullptr || ! isEnabled())
        return;
    if (m_typing == nullptr)
    {
        m_typing = std::make_unique<juce::TextEditor>();
        m_typing->setColour(juce::TextEditor::backgroundColourId, TC::well);
        m_typing->setColour(juce::TextEditor::textColourId, TC::text);
        m_typing->setColour(juce::TextEditor::highlightColourId, TC::brass.withAlpha(0.35f));
        m_typing->setColour(juce::TextEditor::focusedOutlineColourId, TC::brass);
        m_typing->setJustification(juce::Justification::centred);
        m_typing->setBorder(juce::BorderSize<int>(1));
        m_typing->setIndents(2, 0);
        m_typing->setSelectAllWhenFocused(true);
        m_typing->onReturnKey = [this] { endTyping(true, true); };
        m_typing->onEscapeKey = [this] { endTyping(false, true); };
        m_typing->onFocusLost = [this] { endTyping(false, false); }; // clicked away: whatever was clicked keeps the keys
    }
    // The box lives in this control's parent, so it can be wider than a narrow control (a vertical fader).
    auto* parent = getParentComponent();
    if (parent == nullptr)
        return;
    if (m_typing->getParentComponent() != parent)
        parent->addChildComponent(*m_typing);
    placeTyping();
    const auto bounds = m_typing->getBounds();
    juce::ignoreUnused(bounds);
    m_typing->applyFontToAllText(m_typedFonts->caption());
    m_typing->setText(m_typedParameter->getText(m_typedParameter->getValue(), 0), false);
    m_typing->setVisible(true);
    m_typing->toFront(false);
    m_typing->grabKeyboardFocus();
    m_typing->selectAll();
}

void ValueSlider::endTyping(bool keep, bool takeKeysBack)
{
    if (! isTyping())
        return;
    const auto text = m_typing->getText();
    m_typing->setVisible(false); // first: hiding it loses the focus, which comes back here
    if (keep && m_typedParameter != nullptr)
    {
        auto* param = m_typedParameter;
        const float value = TypedValue::toNormalised([param](float n) { return param->getText(n, 0); }, text, param->getValue());
        if (value >= 0.0f)
        {
            if (onTypedValue)
                onTypedValue();
            // One gesture, so it is one undo step.
            param->beginChangeGesture();
            param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
            param->endChangeGesture();
        }
    }
    // The keys go back to the window (its shortcuts), not to nothing.
    if (takeKeysBack)
        if (auto* window = findParentComponentOfClass<juce::AudioProcessorEditor>())
            window->grabKeyboardFocus();
}

juce::Rectangle<int> ValueSlider::typingBounds() const
{
    // Over the control; a tall one (a vertical fader) gets a box across its middle.
    auto r = getLocalBounds();
    if (r.getHeight() > 30)
        r = r.withSizeKeepingCentre(r.getWidth(), 20);
    return r;
}

void ValueSlider::placeTyping()
{
    if (m_typing == nullptr)
        return;
    auto r = typingBounds();
    if (r.getWidth() < minTypingWidth)
        r = r.withSizeKeepingCentre(minTypingWidth, r.getHeight());
    m_typing->setBounds(r + getPosition());
}

void ValueSlider::resized()
{
    juce::Slider::resized();
    if (isTyping())
        placeTyping();
}

void ValueSlider::moved()
{
    juce::Slider::moved();
    if (isTyping())
        placeTyping();
}

void ValueSlider::mouseDown(const juce::MouseEvent& e)
{
    // A right-click: the control's menu, or the box to type a value into. Alt belongs to typing too (an
    // Alt-double-click opens the box), so an Alt-click never moves the control.
    m_swallowMouse = false;
    if (e.mods.isPopupMenu())
    {
        m_swallowMouse = true;
        if (onContextMenu)
            onContextMenu();
        else
            beginTyping();
        return;
    }
    if (e.mods.isAltDown() && m_typedParameter != nullptr)
    {
        m_swallowMouse = true;
        return;
    }
    juce::Slider::mouseDown(e);
}

void ValueSlider::mouseDrag(const juce::MouseEvent& e)
{
    if (! m_swallowMouse)
        juce::Slider::mouseDrag(e);
}

void ValueSlider::mouseUp(const juce::MouseEvent& e)
{
    if (std::exchange(m_swallowMouse, false))
        return;
    juce::Slider::mouseUp(e);
}

void ValueSlider::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.mods.isAltDown() && m_typedParameter != nullptr)
    {
        beginTyping();
        return;
    }
    if (! e.mods.isPopupMenu())
        juce::Slider::mouseDoubleClick(e);
}

juce::Rectangle<int> WheelKnob::typingBounds() const
{
    return getLocalBounds().removeFromBottom(26).reduced(2, 3);
}

void ModTarget::setModLive(float normalised)
{
    if (std::abs(normalised - m_modLive) < 0.002f)
        return;
    m_modLive = normalised;
    repaint();
}

void ModTarget::mouseDown(const juce::MouseEvent& e)
{
    // Cmd-drag (Ctrl on Windows) on a modulated control sets its wire's depth instead of the control's value.
    if (e.mods.isCommandDown() && isModulated() && onModDepthGesture)
    {
        m_modGesture = true;
        onModDepthGesture(e, 0);
        return;
    }
    ValueSlider::mouseDown(e);
}

void ModTarget::mouseDrag(const juce::MouseEvent& e)
{
    if (m_modGesture)
    {
        onModDepthGesture(e, 1);
        return;
    }
    ValueSlider::mouseDrag(e);
}

void ModTarget::mouseUp(const juce::MouseEvent& e)
{
    if (m_modGesture)
    {
        m_modGesture = false;
        onModDepthGesture(e, 2);
        return;
    }
    ValueSlider::mouseUp(e);
}

void ModTarget::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.mods.isCommandDown() && isModulated() && onModDepthGesture)
    {
        onModDepthGesture(e, 3);
        return;
    }
    ValueSlider::mouseDoubleClick(e);
}

void ModTarget::strokeModPath(juce::Graphics& g, const juce::Path& p, juce::Colour colour, float depth, float thickness)
{
    g.setColour(colour);
    const juce::PathStrokeType stroke(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    if (depth >= 0.0f)
    {
        g.strokePath(p, stroke);
        return;
    }
    const float dashes[] = { 3.0f, 2.6f };
    juce::Path dashed;
    stroke.createDashedStroke(dashed, p, dashes, 2);
    g.fillPath(dashed);
}

void WheelKnob::valueChanged()
{
    if (! isTimerRunning())
        startTimerHz(60);
}

void WheelKnob::timerCallback()
{
    const auto target = static_cast<float>(valueToProportionOfLength(getValue()));
    if (! springStep(m_shown, m_velocity, target))
        stopTimer();
    repaint();
}

void WheelKnob::mouseEnter(const juce::MouseEvent& e) { m_hover = true; repaint(); Slider::mouseEnter(e); }
void WheelKnob::mouseExit(const juce::MouseEvent& e) { m_hover = false; repaint(); Slider::mouseExit(e); }

void WheelKnob::paint(juce::Graphics& g)
{
    if (! isTimerRunning() && std::abs(m_velocity) < 1.0e-6f)
        m_shown = static_cast<float>(valueToProportionOfLength(getValue()));

    auto bounds = getLocalBounds().toFloat();
    auto textArea = bounds.removeFromBottom(26.0f);
    const auto centre = getDialCentre();
    const float radius = getDialRadius();
    const float angle = c_startAngle + m_shown * (c_endAngle - c_startAngle);

    // Three looks besides the usual one (Theme::Look). Disabled: drained of colour. Linked: drained, with a chain
    // link on its hub and a ring in the colour of what it follows. Resting: usable, so it keeps its shape and its
    // caption, but its wheel is unlit and its arc is dashed until what it waits for is switched on.
    const auto look = Theme::getLook(*this);
    const bool drained = look == Theme::Look::Disabled || look == Theme::Look::Linked;
    const bool resting = look == Theme::Look::Resting;
    const auto accent = drained ? TC::textFaint.withAlpha(0.4f) : (resting ? m_accent.withAlpha(0.6f) : m_accent);

    const float arcR = radius - 2.0f;
    juce::Path track;
    track.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, c_startAngle, c_endAngle, true);
    g.setColour(drained ? TC::well.withAlpha(0.7f) : TC::well);
    g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (look == Theme::Look::Linked)
    {
        g.setColour(m_linkColour.withAlpha(0.55f));
        g.strokePath(track, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const float valueAngle = c_startAngle + static_cast<float>(valueToProportionOfLength(getValue())) * (c_endAngle - c_startAngle);
    const bool bipolar = getMinimum() < 0.0 && getMaximum() > 0.0;
    const float from = bipolar ? 0.0f : c_startAngle;
    juce::Path arc;
    arc.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, juce::jmin(from, valueAngle), juce::jmax(from, valueAngle), true);
    if (resting)
    {
        const float dashes[] = { 3.0f, 3.5f };
        juce::Path dashed;
        juce::PathStrokeType(2.0f).createDashedStroke(dashed, arc, dashes, 2);
        g.setColour(accent);
        g.fillPath(dashed);
    }
    else if (! drained)
    {
        g.setColour(accent.withAlpha(0.22f));
        g.strokePath(arc, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(accent);
        g.strokePath(arc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Modulation: each wire's reach as a thin arc just inside the ring, and the live position as a dot.
    {
        const float base = static_cast<float>(valueToProportionOfLength(getValue()));
        auto angleAt = [](float p) { return c_startAngle + juce::jlimit(0.0f, 1.0f, p) * (c_endAngle - c_startAngle); };
        float r = arcR - 5.0f;
        for (const auto& m : m_modArcs)
        {
            const float lo = m.bipolar ? base - 0.5f * std::abs(m.depth) : juce::jmin(base, base + m.depth);
            const float hi = m.bipolar ? base + 0.5f * std::abs(m.depth) : juce::jmax(base, base + m.depth);
            juce::Path p;
            p.addCentredArc(centre.x, centre.y, r, r, 0.0f, angleAt(lo), angleAt(hi), true);
            strokeModPath(g, p, m.colour.withAlpha(0.9f), m.depth, 2.0f); // dashed: negative depth
            r -= 2.6f;
            if (r < radius * 0.78f)
                break;
        }
        if (m_modLive >= 0.0f && ! m_modArcs.empty())
        {
            const float a = angleAt(m_modLive);
            const juce::Point<float> dot { centre.x + std::sin(a) * arcR, centre.y - std::cos(a) * arcR };
            g.setColour(m_modArcs.front().colour.withAlpha(0.35f));
            g.fillEllipse(juce::Rectangle<float>(10.0f, 10.0f).withCentre(dot));
            g.setColour(TC::text);
            g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre(dot));
        }
        if (! m_wireHover.isTransparent())
        {
            g.setColour(m_wireHover.withAlpha(0.8f));
            g.drawEllipse(juce::Rectangle<float>(radius * 2.0f + 4.0f, radius * 2.0f + 4.0f).withCentre(centre), 2.0f);
        }
    }

    // Ship's wheel
    const float rim = radius * 0.56f;
    const float spokeEnd = radius * 0.74f;
    const auto brass = drained ? TC::textFaint.withAlpha(0.45f) : (resting ? TC::textDim : TC::brass);
    for (int i = 0; i < 8; ++i)
    {
        const float a = angle + juce::MathConstants<float>::twoPi * static_cast<float>(i) / 8.0f;
        const juce::Point<float> dir { std::sin(a), -std::cos(a) };
        const auto inner = centre + dir * (rim * 0.25f);
        const auto outer = centre + dir * spokeEnd;
        g.setColour(i == 0 ? accent : brass.withMultipliedAlpha(0.85f));
        g.drawLine(juce::Line<float>(inner, outer), i == 0 ? 2.6f : 1.8f);
        const float handle = i == 0 ? 3.4f : 2.6f;
        g.fillEllipse(juce::Rectangle<float>(handle * 2.0f, handle * 2.0f).withCentre(outer));
    }

    g.setColour(TC::panel);
    g.fillEllipse(juce::Rectangle<float>(rim * 2.0f, rim * 2.0f).withCentre(centre).reduced(2.0f));
    g.setColour(brass);
    g.drawEllipse(juce::Rectangle<float>(rim * 2.0f, rim * 2.0f).withCentre(centre), 2.6f);
    g.drawEllipse(juce::Rectangle<float>(rim * 1.2f, rim * 1.2f).withCentre(centre), 1.2f);

    g.setColour(brass);
    g.fillEllipse(juce::Rectangle<float>(rim * 0.42f, rim * 0.42f).withCentre(centre));
    g.setColour(TC::panel);
    g.fillEllipse(juce::Rectangle<float>(rim * 0.16f, rim * 0.16f).withCentre(centre));

    if (look == Theme::Look::Linked)
    {
        // The chain link, on a dark disc over the hub.
        g.setColour(TC::panel);
        g.fillEllipse(juce::Rectangle<float>(rim * 1.5f, rim * 1.5f).withCentre(centre));
        Icons::draw(g, Icons::Icon::Link, juce::Rectangle<float>(rim * 1.25f, rim * 1.25f).withCentre(centre), m_linkColour);
    }

    const bool showValue = (m_hover || isMouseButtonDown()) && ! drained;
    g.setColour(drained ? TC::textFaint.withAlpha(0.6f) : (showValue ? m_accent.brighter(0.3f) : TC::textDim));
    Theme::fitText(g, showValue ? getTextFromValue(getValue()) : caption, textArea.reduced(1.0f, 0.0f).withTrimmedBottom(4.0f), m_fonts.caption(),
                   juce::Justification::centred);
}

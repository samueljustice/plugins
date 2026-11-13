#include "LookAndFeel.h"
#include "TypedValue.h"

#include <map>
#include <tuple>

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
    // The menu's plate is drawn by drawPopupMenuBackground, square to its corners.
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
    // A fader: a slot cut into the plate, filled in the control's colour up to a sand block cap.
    const bool enabled = slider.isEnabled();
    const auto accent = enabled ? slider.findColour(juce::Slider::trackColourId) : TC::textFaint.withAlpha(0.5f);
    const auto r = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height));
    const bool vertical = style == juce::Slider::LinearVertical;
    const bool over = slider.isMouseOverOrDragging();
    const auto slot = (vertical ? r.withSizeKeepingCentre(6.0f, r.getHeight()).expanded(0.0f, 7.0f)
                                : r.withSizeKeepingCentre(r.getWidth(), 6.0f).expanded(6.0f, 0.0f));
    const juce::Point<float> at = vertical ? juce::Point<float>(r.getCentreX(), sliderPos) : juce::Point<float>(sliderPos, r.getCentreY());

    g.setColour(TC::well);
    g.fillRect(slot);
    g.setColour(accent.withAlpha(0.9f));
    g.fillRect((vertical ? slot.withTop(sliderPos) : slot.withRight(sliderPos)).reduced(1.0f));
    Theme::Plate::wellEdge(g, slot, 0.0f);

    // The cap: a block across the slot, with a grip line in the control's colour.
    const float across = juce::jmin(18.0f, (vertical ? r.getWidth() : r.getHeight()) - 2.0f), along = 10.0f;
    const auto cap = (vertical ? juce::Rectangle<float>(across, along) : juce::Rectangle<float>(along, across)).withCentre(at);
    g.setColour(TC::edgeDark);
    Theme::Plate::fill(g, cap.expanded(1.0f).translated(0.0f, 1.0f), Theme::Cut::small);
    g.setColour(enabled ? (over ? TC::text.brighter(0.2f) : TC::text) : TC::textFaint);
    Theme::Plate::fill(g, cap, Theme::Cut::small);
    g.setColour(enabled ? accent.darker(0.35f) : TC::well);
    g.fillRect(vertical ? cap.withSizeKeepingCentre(across - 6.0f, 2.0f) : cap.withSizeKeepingCentre(2.0f, across - 6.0f));
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
    /** The little open arrow that says "this opens a menu". */
    void drawChevron(juce::Graphics& g, juce::Point<float> centre, juce::Colour colour, bool up = false)
    {
        const float d = up ? -1.8f : 1.8f;
        juce::Path p;
        p.startNewSubPath(centre.x - 3.5f, centre.y - d);
        p.lineTo(centre.x, centre.y + d);
        p.lineTo(centre.x + 3.5f, centre.y - d);
        g.setColour(colour);
        g.strokePath(p, juce::PathStrokeType(1.5f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    }

    /** Dark lettering reads on a lit plate of this colour; on the few that are too deep for it, sand does. */
    juce::Colour letteringOn(juce::Colour plate)
    {
        return plate.getPerceivedBrightness() > 0.5f ? TC::window : TC::text;
    }
}

juce::Rectangle<float> SwooshLookAndFeel::plateFace(juce::Rectangle<float> r, bool down)
{
    return down ? r.withTrimmedTop(1.5f) : r.withTrimmedBottom(1.5f);
}

void SwooshLookAndFeel::drawControlPlate(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent, Theme::Look look,
                                         bool on, bool over, bool down, bool focused, float cut)
{
    namespace Plate = Theme::Plate;
    if (look == Theme::Look::Disabled || look == Theme::Look::Linked)
    {
        // Flat and faint: no edge, no lip.
        g.setColour(TC::raised.withAlpha(0.45f));
        Plate::fill(g, plateFace(r, false), cut);
        return;
    }
    if (look == Theme::Look::Resting)
    {
        // Hollow: it can be set, and waits for something else to be switched on.
        const auto face = plateFace(r, down);
        g.setColour(TC::raised.withAlpha(over ? 0.7f : 0.35f));
        Plate::fill(g, face, cut);
        juce::Path dashed;
        const float dashes[] = { 3.0f, 3.0f };
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, Plate::shape(face.reduced(0.5f), cut), dashes, 2);
        g.setColour(on ? accent.withAlpha(0.8f) : TC::stroke);
        g.fillPath(dashed);
        return;
    }
    // The lip the plate sits on (and down on, when pressed).
    g.setColour(on ? accent.darker(0.75f) : TC::edgeDark);
    Plate::fill(g, down ? r.withTrimmedTop(1.5f) : r.withTrimmedTop(2.0f), cut);
    const auto face = plateFace(r, down);
    // A solid plate, so nothing behind shows through it (the moon behind Morph).
    g.setColour(on ? (over ? accent.brighter(0.18f) : accent) : (down ? TC::raised.darker(0.12f) : (over ? TC::hover : TC::raised)));
    Plate::fill(g, face, cut);
    // The light line along its top edge, then the edge itself.
    g.setColour(on ? juce::Colours::white.withAlpha(0.32f) : TC::edgeLight);
    g.fillRect(juce::Rectangle<float>(face.getX() + cut, face.getY() + 1.0f, face.getWidth() - 2.0f * cut, 1.0f));
    g.setColour(focused ? TC::brass : (on ? accent.darker(0.6f) : (over ? TC::stroke.brighter(0.15f) : TC::edgeDark)));
    Plate::stroke(g, face, cut);
}

void SwooshLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& background,
                                             bool over, bool down)
{
    namespace Plate = Theme::Plate;
    const auto r = button.getLocalBounds().toFloat();
    const auto accent = button.findColour(juce::TextButton::buttonOnColourId);
    const bool on = button.getToggleState();
    const auto look = Theme::getLook(button);
    const auto style = getStyle(button);

    if (style == ButtonStyle::Chip)
    {
        // A chip is a state, not a key: a small tag with no lip, dark and quiet when off, its whole plate in its
        // colour when on.
        const bool usable = look == Theme::Look::Normal || look == Theme::Look::Resting;
        g.setColour(! usable ? TC::well.withAlpha(0.4f) : (on ? (over ? accent.brighter(0.18f) : accent) : (over ? TC::hover : TC::well.withAlpha(0.8f))));
        Plate::fill(g, r, Theme::Cut::small);
        if (usable)
        {
            g.setColour(on ? accent.darker(0.6f) : (over ? TC::stroke.brighter(0.15f) : TC::edgeDark));
            Plate::stroke(g, r, Theme::Cut::small);
        }
        return;
    }
    if (style == ButtonStyle::Primary)
    {
        if (look != Theme::Look::Normal)
        {
            // Not to be pressed now: the brass is gone, and so is the lip that makes it a key.
            g.setColour(TC::raised.withAlpha(0.45f));
            Plate::fill(g, r.withTrimmedBottom(2.0f), Theme::Cut::control);
            return;
        }
        // The one brass key: a deeper lip than the iron plates have.
        const auto face = down ? r.withTrimmedTop(2.0f) : r.withTrimmedBottom(2.0f);
        g.setColour(TC::brassDark.darker(0.3f));
        Plate::fill(g, r.withTrimmedTop(2.0f), Theme::Cut::control);
        g.setColour(over ? TC::brass.brighter(0.25f) : TC::brass);
        Plate::fill(g, face, Theme::Cut::control);
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.fillRect(juce::Rectangle<float>(face.getX() + Theme::Cut::control, face.getY() + 1.0f, face.getWidth() - 2.0f * Theme::Cut::control, 1.0f));
        g.setColour(TC::brassDark);
        Plate::stroke(g, face, Theme::Cut::control);
        return;
    }
    drawControlPlate(g, r, accent, look, on, over, down);
    // A button whose plate has a colour of its own (Throw) keeps it as a tint.
    if (background != TC::raised && look == Theme::Look::Normal && ! on)
    {
        g.setColour(background);
        Plate::fill(g, plateFace(r, down).reduced(1.0f), Theme::Cut::control - 0.5f);
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
    // A button that switches something (and is not a chip, whose plate says it) carries a lamp slot.
    // One of a row of choices (a radio group) is lit by its plate alone.
    const bool lamp = style == ButtonStyle::Button && button.getClickingTogglesState() && icon == Icons::Icon::None
                      && button.getRadioGroupId() == 0 && ! button.getProperties().contains("plain");
    const bool chevron = style == ButtonStyle::Menu;
    const bool lit = on && look == Theme::Look::Normal; // the whole plate is in its colour

    juce::Colour colour;
    if (look == Theme::Look::Disabled || look == Theme::Look::Linked)
        colour = TC::textFaint.withAlpha(0.55f);
    else if (style == ButtonStyle::Primary)
        colour = TC::window;
    else if (lit || (on && style == ButtonStyle::Chip))
        colour = letteringOn(accent);
    else if (on)
        colour = accent.brighter(0.45f); // resting and on: hollow, its lettering in its colour
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
        // The lamp: a small square slot punched in the plate, open when off, filled when on.
        const auto slot = juce::Rectangle<float>(6.0f, 6.0f).withCentre({ x + 3.0f, std::floor(r.getCentreY()) + 0.5f });
        g.setColour(colour.withMultipliedAlpha(lit ? 0.9f : 0.65f));
        if (on)
            g.fillRect(slot);
        else
            g.drawRect(slot, 1.2f);
    }
    const float left = x + lead + gap;
    Theme::stampText(g, text, juce::Rectangle<float>(left, r.getY(), r.getRight() - pad - tail - left, r.getHeight()), font,
                     juce::Justification::centredLeft, colour, fit);
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
    // A switch: a slot cut in the plate with a block that slides in it, to the right and in the control's
    // colour when on.
    auto r = button.getLocalBounds().toFloat();
    const auto accent = button.findColour(juce::ToggleButton::tickColourId);
    const bool on = button.getToggleState();
    const auto look = Theme::getLook(button);
    const bool usable = look == Theme::Look::Normal || look == Theme::Look::Resting;
    const float slotH = juce::jmin(16.0f, r.getHeight() - 2.0f), slotW = std::floor(slotH * 1.75f);
    const auto slot = juce::Rectangle<float>(r.getX() + 1.0f, std::floor(r.getCentreY() - slotH * 0.5f), slotW, slotH);
    const float blockW = std::floor(slotW * 0.5f) - 1.0f;
    const auto block = juce::Rectangle<float>(on ? slot.getRight() - 2.0f - blockW : slot.getX() + 2.0f, slot.getY() + 2.0f, blockW, slotH - 5.0f);

    g.setColour(usable ? TC::well : TC::well.withAlpha(0.5f));
    Theme::Plate::fill(g, slot, Theme::Cut::small);
    if (usable)
    {
        if (on)
        {
            g.setColour(accent.withAlpha(0.28f));
            Theme::Plate::fill(g, slot, Theme::Cut::small);
        }
        Theme::Plate::wellEdge(g, slot, Theme::Cut::small);
        if (over || on)
        {
            g.setColour(on ? accent.withAlpha(over ? 1.0f : 0.75f) : TC::stroke.brighter(0.3f));
            Theme::Plate::stroke(g, slot.withTrimmedBottom(1.0f), Theme::Cut::small);
        }
    }
    g.setColour(! usable ? TC::textFaint.withAlpha(0.5f) : (on ? accent : TC::textDim));
    g.fillRect(block);
    g.setColour(TC::well.withAlpha(usable ? 0.7f : 0.3f));
    g.fillRect(block.withSizeKeepingCentre(1.5f, block.getHeight() - 4.0f));

    if (button.getButtonText().isNotEmpty())
    {
        g.setColour(! usable ? TC::textFaint.withAlpha(0.55f) : (on || over ? TC::text : TC::textDim));
        Theme::fitText(g, button.getButtonText(), r.withTrimmedLeft(slotW + 8.0f), m_fonts.control(), juce::Justification::centredLeft);
    }
}

void SwooshLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool down, int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    const auto look = Theme::getLook(box);
    drawControlPlate(g, r, TC::brass, look, false, box.isMouseOver(true), down, box.hasKeyboardFocus(false));
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
    const auto colour = look == Theme::Look::Disabled || look == Theme::Look::Linked ? TC::textFaint.withAlpha(0.55f)
                        : look == Theme::Look::Resting ? TC::textDim : label.findColour(juce::Label::textColourId);
    const auto box = label.getBorderSize().subtractedFrom(label.getLocalBounds()).toFloat();
    if (owner != nullptr) // on a menu's plate: stamped, like a button's
        Theme::stampText(g, label.getText(), box, getLabelFont(label), label.getJustificationType(), colour);
    else
    {
        g.setColour(colour);
        Theme::fitText(g, label.getText(), box, getLabelFont(label), label.getJustificationType(), Theme::Fit::Name);
    }
}

void SwooshLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    // A plate of its own, square like every other.
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    const float cut = Theme::Cut::panel;
    g.setColour(TC::raised.brighter(0.03f));
    Theme::Plate::fill(g, r, cut);
    Theme::Plate::edge(g, r, cut);
}

void SwooshLookAndFeel::drawPopupMenuUpDownArrow(juce::Graphics& g, int width, int height, bool isScrollUpArrow)
{
    // A menu too long for the screen scrolls: the arrow sits on the menu's own colour, fading into the items.
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    g.setGradientFill(juce::ColourGradient(TC::raised, 0.0f, h * 0.5f, TC::raised.withAlpha(0.0f), 0.0f, isScrollUpArrow ? h : 0.0f, false));
    g.fillRect(2.0f, 0.0f, w - 4.0f, h);
    drawChevron(g, { w * 0.5f, h * 0.5f }, TC::textDim, isScrollUpArrow);
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
        Theme::Plate::fill(g, r.toFloat(), Theme::Cut::small);
    }
    const auto tick = r.removeFromLeft(22);
    if (isTicked)
    {
        // The chosen one: a small brass block.
        g.setColour(TC::brass);
        g.fillRect(juce::Rectangle<float>(6.0f, 6.0f).withCentre(tick.toFloat().getCentre()));
    }
    if (hasSubMenu)
    {
        const auto arrow = r.removeFromRight(16).toFloat().getCentre();
        juce::Path p;
        p.startNewSubPath(arrow.x - 1.8f, arrow.y - 3.5f);
        p.lineTo(arrow.x + 1.8f, arrow.y);
        p.lineTo(arrow.x - 1.8f, arrow.y + 3.5f);
        g.setColour(isActive ? TC::textDim : TC::textFaint.withAlpha(0.5f));
        g.strokePath(p, juce::PathStrokeType(1.5f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
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
    // A block in a slot: no arrows, no frame.
    const auto track = juce::Rectangle<int>(x, y, width, height).toFloat();
    const float thick = juce::jmin(6.0f, vertical ? track.getWidth() : track.getHeight());
    const auto slot = vertical ? track.withSizeKeepingCentre(thick, track.getHeight()) : track.withSizeKeepingCentre(track.getWidth(), thick);
    g.setColour(TC::well.withAlpha(0.7f));
    g.fillRect(slot);
    if (thumbSize <= 0)
        return;
    const auto thumb = vertical ? slot.withY(static_cast<float>(thumbStart)).withHeight(static_cast<float>(thumbSize))
                                : slot.withX(static_cast<float>(thumbStart)).withWidth(static_cast<float>(thumbSize));
    g.setColour(down ? TC::brass : (over ? TC::stroke.brighter(0.4f) : TC::stroke));
    g.fillRect(thumb);
}

void SwooshLookAndFeel::drawCornerResizer(juce::Graphics& g, int w, int h, bool over, bool dragging)
{
    // Three brass cuts across the corner: quiet until the mouse is on them.
    g.setColour(TC::brass.withAlpha(over || dragging ? 0.9f : 0.45f));
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    for (const float t : { 0.3f, 0.52f, 0.74f })
        g.drawLine(fw * t, fh * 0.86f, fw * 0.86f, fh * t, 1.3f);
}

void SwooshLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    // A text box paints every pixel of itself, square to its corners.
    g.fillAll(TC::panel);
    g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
    Theme::Plate::fill(g, juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)), Theme::Cut::small);
}

void SwooshLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    // A well; its edge turns brass while it has the keyboard (and lighter under the mouse).
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    const bool focused = editor.hasKeyboardFocus(true) && ! editor.isReadOnly();
    if (! focused && ! editor.isMouseOver(true))
    {
        Theme::Plate::wellEdge(g, r, Theme::Cut::small);
        return;
    }
    g.setColour(focused ? editor.findColour(juce::TextEditor::focusedOutlineColourId) : TC::stroke);
    Theme::Plate::stroke(g, r, Theme::Cut::small);
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
    // The tooltip's window is a plain rectangle: a plate filled edge to edge, a dark edge and a quiet brass line
    // inside it (it floats over anything).
    const auto r = juce::Rectangle<float>(0, 0, static_cast<float>(width), static_cast<float>(height));
    g.fillAll(TC::raised);
    g.setColour(TC::brass.withAlpha(0.5f));
    g.drawRect(r.reduced(1.0f), 1.0f);
    g.setColour(TC::edgeDark);
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

void SwooshLookAndFeel::drawAlertBox(juce::Graphics& g, juce::AlertWindow& alert, const juce::Rectangle<int>&, juce::TextLayout& textLayout)
{
    // A panel plate with its bolts. The window is opaque, so the plate is square and filled edge to edge.
    const auto r = alert.getLocalBounds().toFloat();
    g.fillAll(alert.findColour(juce::AlertWindow::backgroundColourId));
    Theme::Plate::edge(g, r, 0.0f);
    drawBolts(g, r);
    // The title and the message, where the window has left room for them above its text box and buttons.
    g.setColour(alert.findColour(juce::AlertWindow::textColourId));
    textLayout.draw(g, juce::Rectangle<float>(r.getX() + 1.0f, 30.0f, r.getWidth() - 2.0f,
                                              r.getHeight() - static_cast<float>(getAlertWindowButtonHeight()) - 22.0f));
}

//==============================================================================
void SwooshLookAndFeel::drawSurface(juce::Graphics& g, juce::Rectangle<float> area, float cut, juce::Colour fill, float elevation)
{
    if (! g.clipRegionIntersects(area.expanded(4.0f, 6.0f).getSmallestIntegerContainer()))
        return; // nothing of it is being repainted
    // A plate: a hard, short shadow under it (it is bolted on, not floating), the flat tone, the pressed edge.
    if (elevation > 0.0f)
    {
        g.setColour(TC::shadow.withAlpha(juce::jmin(1.0f, 0.5f * elevation)));
        Theme::Plate::fill(g, area.expanded(1.0f, 0.0f).translated(0.0f, 2.0f + elevation), cut);
    }
    g.setColour(fill);
    Theme::Plate::fill(g, area, cut);
    Theme::Plate::edge(g, area, cut);
}

void SwooshLookAndFeel::drawWell(juce::Graphics& g, juce::Rectangle<float> area, float cut)
{
    g.setColour(TC::well);
    Theme::Plate::fill(g, area, cut);
    Theme::Plate::wellEdge(g, area, cut);
}

void SwooshLookAndFeel::drawBolts(juce::Graphics& g, juce::Rectangle<float> area, float inset)
{
    for (const auto& c : { area.getTopLeft().translated(inset, inset), area.getTopRight().translated(-inset, inset),
                           area.getBottomLeft().translated(inset, -inset), area.getBottomRight().translated(-inset, -inset) })
        Theme::Plate::drawBolt(g, c);
}

int SwooshLookAndFeel::titleEnd(const Theme::Fonts& fonts, const juce::String& title)
{
    // The right end of the nameplate: the title, then the room its rivet takes.
    return c_titleX + juce::roundToInt(std::ceil(Theme::textWidth(fonts.display(), title))) + c_plateTail;
}

void SwooshLookAndFeel::drawPanel(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& title,
                                  const juce::String& subtitle, juce::Colour accent, const Theme::Fonts& fonts,
                                  Icons::Icon mark, float subtitleRight)
{
    namespace Plate = Theme::Plate;
    drawSurface(g, area);
    if (g.clipRegionIntersects(area.getSmallestIntegerContainer()))
        drawBolts(g, area);
    if (title.isEmpty())
        return;
    const auto bar = area.withHeight(static_cast<float>(Theme::Space::titleBar));
    if (! g.clipRegionIntersects(bar.getSmallestIntegerContainer()))
        return;

    // The nameplate: a dark plate riveted to the panel head, with a tally in the section's colour at its left
    // carrying the section's mark (or its letter), and the name in the blackletter face.
    const float x = area.getX() + static_cast<float>(c_titleX);
    const float titleWidth = juce::jmin(area.getRight() - 16.0f - static_cast<float>(c_plateTail) - x, std::ceil(Theme::textWidth(fonts.display(), title)));
    const auto plate = juce::Rectangle<float>(area.getX() + 18.0f, area.getY() + 8.0f, x + titleWidth + static_cast<float>(c_plateTail) - (area.getX() + 18.0f), 24.0f);
    g.setColour(TC::well.interpolatedWith(TC::panel, 0.35f));
    Plate::fill(g, plate, Theme::Cut::control);
    auto tally = plate.withWidth(24.0f);
    g.setColour(accent);
    {
        // The tally fills the plate's left end.
        g.saveState();
        g.reduceClipRegion(tally.getSmallestIntegerContainer());
        Plate::fill(g, plate, Theme::Cut::control);
        g.restoreState();
    }
    const auto ink = letteringOn(accent);
    if (mark == Icons::Icon::LetterA || mark == Icons::Icon::LetterB)
        Theme::stampText(g, mark == Icons::Icon::LetterA ? "A" : "B", tally, fonts.title(), juce::Justification::centred, ink);
    else if (mark != Icons::Icon::None)
        Icons::draw(g, mark, tally.reduced(5.0f), ink);
    else
    {
        g.setColour(ink.withAlpha(0.8f));
        g.fillRect(tally.withSizeKeepingCentre(3.0f, 12.0f));
    }
    Plate::edge(g, plate, Theme::Cut::control);
    Plate::drawRivet(g, { plate.getRight() - 6.0f, plate.getCentreY() });

    Theme::stampText(g, title, juce::Rectangle<float>(x, area.getY() + 6.0f, titleWidth, 28.0f), fonts.display(), juce::Justification::centredLeft, TC::text);
    if (subtitle.isNotEmpty())
    {
        const float sx = plate.getRight() + 10.0f;
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

    // Two strands twisted together: a run of slanted, round-ended strokes (a rope is round), each lit along its upper edge.
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
    g.strokePath(strands, juce::PathStrokeType(2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)); // round object: the rope
    g.setColour(colour.brighter(0.5f).withMultipliedAlpha(0.55f));
    g.strokePath(lights, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)); // round object: the rope
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
        g.strokePath(guard, juce::PathStrokeType(juce::jmax(1.5f, w * 0.07f), juce::PathStrokeType::mitered, juce::PathStrokeType::butt), t);

        juce::Path grip;
        grip.startNewSubPath(x0 + 0.20f * w, y0 + 0.87f * h);
        grip.lineTo(x0 + 0.04f * w, y0 + 1.0f * h);
        g.strokePath(grip, juce::PathStrokeType(juce::jmax(1.5f, w * 0.06f), juce::PathStrokeType::mitered, juce::PathStrokeType::butt), t);
    };
    drawOne(false);
    drawOne(true);
}

//==============================================================================
namespace
{
    /** The wheel's own proportions, from its value ring's radius (so both sizes are the same wheel). */
    struct WheelShape
    {
        explicit WheelShape(float ring)
            : rim(ring * 0.54f), rimThick(juce::jmax(2.4f, ring * 0.14f)), spoke(juce::jmax(1.8f, ring * 0.105f)),
              handleEnd(ring - 4.2f), hub(juce::jmax(2.4f, ring * 0.17f)), pointerEnd(ring - 2.2f)
        {
        }
        float rim, rimThick; // the rim's centre line and how thick it is
        float spoke;         // a spoke's thickness (its handle, outside the rim, is a little thicker)
        float handleEnd, hub, pointerEnd;
    };

    constexpr float c_ringThick = 3.0f;

    /** The part of a knob that never moves, kept as an image for each size, colour and screen scale. */
    struct WheelLayers final : public juce::DeletedAtShutdown
    {
        struct Key
        {
            int ring = 0, scale = 0;
            juce::uint32 wheel = 0, track = 0;
            bool operator<(const Key& o) const noexcept { return std::tie(ring, scale, wheel, track) < std::tie(o.ring, o.scale, o.wheel, o.track); }
        };
        ~WheelLayers() override { slot() = nullptr; }
        static WheelLayers*& slot()
        {
            static WheelLayers* layers = nullptr;
            return layers;
        }
        std::map<Key, juce::Image> images;
    };

    /** The socket, the empty ring, the rim and the hub, centred on `centre`. */
    void drawWheelBase(juce::Graphics& g, juce::Point<float> centre, float ring, juce::Colour wheel, juce::Colour track)
    {
        const WheelShape shape(ring);
        auto disc = [&centre](float radius) { return juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre); };
        // The socket the wheel sits in, and around it the groove the value fills: both pressed into the plate.
        g.setColour(juce::Colour(0xff0c212b));
        g.fillEllipse(disc(ring - c_ringThick * 0.5f - 1.0f));
        juce::Path arc;
        arc.addCentredArc(centre.x, centre.y, ring, ring, 0.0f, c_startAngle, c_endAngle, true);
        g.setColour(track);
        g.strokePath(arc, juce::PathStrokeType(c_ringThick, juce::PathStrokeType::curved, juce::PathStrokeType::butt)); // round object: a ring on the wheel
        g.setColour(wheel);
        g.drawEllipse(disc(shape.rim), shape.rimThick);
        g.fillEllipse(disc(shape.hub));
    }

    /** That, from the kept image at the context's own resolution (pixel for pixel, so it stays crisp). */
    void drawWheelLayer(juce::Graphics& g, juce::Point<float> centre, float ring, juce::Colour wheel, juce::Colour track)
    {
        const float scale = juce::jlimit(0.25f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        auto*& layers = WheelLayers::slot();
        if (layers == nullptr)
            layers = new WheelLayers(); // deleted with JUCE (DeletedAtShutdown)
        const WheelLayers::Key key { juce::roundToInt(ring * 100.0f), juce::roundToInt(scale * 1000.0f), wheel.getARGB(), track.getARGB() };
        auto found = layers->images.find(key);
        if (found == layers->images.end())
        {
            if (layers->images.size() >= 48) // a window being resized goes through many scales
                layers->images.clear();
            const float half = ring + c_ringThick * 0.5f + 2.0f;
            const int side = 2 * static_cast<int>(std::ceil(half * scale));
            juce::Image image(juce::Image::ARGB, side, side, true);
            {
                juce::Graphics ig(image);
                ig.addTransform(juce::AffineTransform::scale(scale));
                const float mid = static_cast<float>(side) * 0.5f / scale;
                drawWheelBase(ig, { mid, mid }, ring, wheel, track);
            }
            found = layers->images.emplace(key, std::move(image)).first;
        }
        const auto& image = found->second;
        const float half = static_cast<float>(image.getWidth()) * 0.5f / scale;
        juce::Graphics::ScopedSaveState state(g);
        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality); // one image pixel to one screen pixel
        g.setOpacity(1.0f);
        g.drawImageTransformed(image, juce::AffineTransform::scale(1.0f / scale).translated(centre.x - half, centre.y - half));
    }
}

WheelKnob::WheelKnob(const Theme::Fonts& fonts, juce::Colour accent, Size size) : m_fonts(fonts), m_accent(accent), m_size(size)
{
    setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    setRotaryParameters(c_startAngle, c_endAngle, true);
    setVelocityBasedMode(false);
    setMouseDragSensitivity(220);
}

int WheelKnob::keptLayerCount()
{
    const auto* layers = WheelLayers::slot();
    return layers != nullptr ? static_cast<int>(layers->images.size()) : 0;
}

float WheelKnob::getRingRadius() const
{
    const float room = juce::jmin(static_cast<float>(getWidth()), static_cast<float>(getHeight() - c_textHeight)) * 0.5f - c_modRoom;
    return juce::jmax(4.0f, juce::jmin(ringFor(m_size), room));
}

juce::Point<float> WheelKnob::getDialCentre() const
{
    // The dial sits on the text: whatever spare height the box has is above it.
    return { static_cast<float>(getWidth()) * 0.5f, static_cast<float>(getHeight() - c_textHeight) - c_modRoom - getRingRadius() };
}

float WheelKnob::getDialRadius() const { return getRingRadius() + c_ringThick * 0.5f; }

juce::Point<float> WheelKnob::getPlugPosition(int index) const
{
    // On the rim at the wheel's right: the first at three o'clock, the next ones either side of it.
    static const float offsets[] = { 0.0f, -34.0f, 34.0f, -68.0f, 68.0f, -102.0f };
    const float a = juce::degreesToRadians(90.0f + offsets[juce::jlimit(0, 5, index)]);
    return getDialCentre() + juce::Point<float>(std::sin(a), -std::cos(a)) * (getRingRadius() + 3.0f);
}

bool WheelKnob::getCableGate(int, juce::Point<float>& gate) const
{
    // Up the right edge of the knob's box, past its caption and value, and in from there.
    gate = { static_cast<float>(getWidth()), static_cast<float>(getHeight()) + 2.0f };
    return true;
}

juce::Rectangle<float> WheelKnob::getCaptionArea() const
{
    return juce::Rectangle<int>(0, getHeight() - c_textHeight, getWidth(), c_lineHeight).toFloat();
}

juce::Rectangle<float> WheelKnob::getValueArea() const
{
    return juce::Rectangle<int>(0, getHeight() - c_lineHeight, getWidth(), c_lineHeight).toFloat();
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
    return getLocalBounds().removeFromBottom(c_textHeight).reduced(1, 3);
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
    const juce::PathStrokeType stroke(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt); // round object: a ring on the wheel
    if (depth >= 0.0f)
    {
        g.strokePath(p, stroke);
        return;
    }
    const float dashes[] = { 2.6f, 2.2f };
    juce::Path dashed;
    stroke.createDashedStroke(dashed, p, dashes, 2);
    g.fillPath(dashed);
}

void WheelKnob::valueChanged()
{
    m_valueTextStale = true;
    if (m_held)
    {
        // Under the hand the wheel is where the value is, with no lag; how fast it turns is kept for the coast.
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        const float position = valuePosition();
        const float dt = static_cast<float>(juce::jlimit(1.0 / 240.0, 0.25, now - m_dragTime));
        m_dragVelocity += ((position - m_dragLast) / dt - m_dragVelocity) * 0.5f;
        m_dragLast = position;
        m_dragTime = now;
        m_shown = position;
        m_velocity = 0.0f;
        stopTimer();
        repaint();
        return;
    }
    if (! isTimerRunning())
        startTimerHz(60);
}

void WheelKnob::timerCallback()
{
    const float before = m_shown;
    if (m_held || ! springStep(m_shown, m_velocity, valuePosition()))
        stopTimer();
    if (! juce::exactlyEqual(m_shown, before))
        repaint();
}

void WheelKnob::mouseEnter(const juce::MouseEvent& e) { m_hover = true; repaint(); Slider::mouseEnter(e); }
void WheelKnob::mouseExit(const juce::MouseEvent& e) { m_hover = false; repaint(); Slider::mouseExit(e); }

void WheelKnob::mouseDown(const juce::MouseEvent& e)
{
    // A right-click (the menu, or the box to type in) and an Alt-click never turn the wheel, so they are not a hold:
    // a value typed or picked after one swings in on the spring like any other that changes on its own.
    m_held = ! (e.mods.isPopupMenu() || e.mods.isAltDown());
    m_dragVelocity = 0.0f;
    m_dragLast = valuePosition();
    m_dragTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
    m_shown = m_dragLast;
    m_velocity = 0.0f;
    stopTimer();
    ModTarget::mouseDown(e);
}

void WheelKnob::mouseUp(const juce::MouseEvent& e)
{
    ModTarget::mouseUp(e);
    if (! std::exchange(m_held, false))
        return;
    // Let go while turning: the wheel carries on a little way and the spring brings it back to the value.
    const double idle = juce::Time::getMillisecondCounterHiRes() * 0.001 - m_dragTime;
    m_velocity = idle < 0.12 ? juce::jlimit(-2.5f, 2.5f, m_dragVelocity * 0.5f) : 0.0f;
    m_shown = valuePosition();
    if (std::abs(m_velocity) > 1.0e-3f)
        startTimerHz(60);
    repaint();
}

void WheelKnob::paint(juce::Graphics& g)
{
    const float position = valuePosition();
    if (m_held || (! isTimerRunning() && std::abs(m_velocity) < 1.0e-6f))
        m_shown = position;

    const auto centre = getDialCentre();
    const float ring = getRingRadius();
    const WheelShape shape(ring);
    auto angleAt = [](float p) { return c_startAngle + juce::jlimit(0.0f, 1.0f, p) * (c_endAngle - c_startAngle); };
    const float angle = c_startAngle + m_shown * (c_endAngle - c_startAngle); // the coast may carry it past an end

    // Three looks besides the usual one (Theme::Look). Disabled: drained of colour. Linked: drained, with a chain
    // link on its hub and a ring in the colour of what it follows. Resting: usable, so it keeps its shape and its
    // text, but its wheel is unlit and its arc is dashed until what it waits for is switched on.
    const auto look = Theme::getLook(*this);
    const bool drained = look == Theme::Look::Disabled || look == Theme::Look::Linked;
    const bool resting = look == Theme::Look::Resting;
    const auto accent = drained ? TC::textFaint.withAlpha(0.4f) : (resting ? m_accent.withAlpha(0.7f) : m_accent);
    const auto wheel = drained ? juce::Colour(0xff4a6670) : (resting ? juce::Colour(0xff8aa3aa) : TC::brass);
    const auto pointer = drained ? juce::Colour(0xff7f989f) : TC::text;

    // What never moves, from the kept image.
    drawWheelLayer(g, centre, ring, wheel, TC::well);
    if (look == Theme::Look::Linked)
    {
        m_scratch.clear();
        m_scratch.addCentredArc(centre.x, centre.y, ring, ring, 0.0f, c_startAngle, c_endAngle, true);
        g.setColour(m_linkColour.withAlpha(0.6f));
        g.strokePath(m_scratch, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::butt)); // round object: a ring on the wheel
    }

    // The value: the ring filled from its start (from the top, when the value goes either side of zero).
    const float valueAngle = angleAt(position);
    const bool bipolar = getMinimum() < 0.0 && getMaximum() > 0.0;
    const float from = bipolar ? angleAt(static_cast<float>(valueToProportionOfLength(0.0))) : c_startAngle;
    if (! drained && std::abs(valueAngle - from) > 0.01f)
    {
        m_scratch.clear();
        m_scratch.addCentredArc(centre.x, centre.y, ring, ring, 0.0f, juce::jmin(from, valueAngle), juce::jmax(from, valueAngle), true);
        g.setColour(accent);
        const juce::PathStrokeType stroke(c_ringThick, juce::PathStrokeType::curved, juce::PathStrokeType::butt); // round object: a ring on the wheel
        if (resting)
        {
            const float dashes[] = { 3.0f, 3.0f };
            juce::Path dashed;
            juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt).createDashedStroke(dashed, m_scratch, dashes, 2); // round object: a ring on the wheel
            g.fillPath(dashed);
        }
        else
            g.strokePath(m_scratch, stroke);
    }

    // The wheel: five brass spokes with their handles, and the pointer.
    for (int i = 1; i < 6; ++i)
    {
        const float a = angle + juce::MathConstants<float>::twoPi * static_cast<float>(i) / 6.0f;
        const juce::Point<float> dir { std::sin(a), -std::cos(a) };
        g.setColour(wheel);
        g.drawLine(juce::Line<float>(centre + dir * (shape.hub * 0.6f), centre + dir * shape.handleEnd), shape.spoke);
        g.drawLine(juce::Line<float>(centre + dir * (shape.rim + shape.rimThick * 0.5f - 0.5f), centre + dir * shape.handleEnd), shape.spoke + 1.4f);
    }
    {
        const juce::Point<float> dir { std::sin(angle), -std::cos(angle) };
        const float thick = shape.spoke + 0.8f;
        g.setColour(TC::edgeDark.withAlpha(0.85f));
        g.drawLine(juce::Line<float>(centre, centre + dir * (shape.pointerEnd + 0.8f)), thick + 2.0f);
        g.setColour(pointer);
        g.drawLine(juce::Line<float>(centre, centre + dir * shape.pointerEnd), thick);
        g.drawLine(juce::Line<float>(centre + dir * (shape.rim + shape.rimThick * 0.5f), centre + dir * shape.pointerEnd), thick + 1.4f);
        g.setColour(TC::edgeDark);
        g.fillEllipse(juce::Rectangle<float>(shape.hub * 0.8f, shape.hub * 0.8f).withCentre(centre));
    }

    if (look == Theme::Look::Linked)
    {
        // The chain link, on a dark disc over the hub.
        const float disc = shape.rim + shape.rimThick * 0.5f + 0.5f;
        g.setColour(juce::Colour(0xff0c212b));
        g.fillEllipse(juce::Rectangle<float>(disc * 2.0f, disc * 2.0f).withCentre(centre));
        Icons::draw(g, Icons::Icon::Link, juce::Rectangle<float>(disc * 1.7f, disc * 1.7f).withCentre(centre), m_linkColour);
    }

    // Modulation: a ring for each wire outside the value ring, stacked outwards, and the live position as a dot.
    if (! m_modArcs.empty())
    {
        const int count = static_cast<int>(m_modArcs.size());
        const float first = ring + c_ringThick * 0.5f + 2.0f;
        const float step = count > 1 ? juce::jmin(2.4f, (c_modRoom - c_ringThick * 0.5f - 2.8f) / static_cast<float>(count - 1)) : 0.0f;
        const float thick = count > 1 ? juce::jlimit(1.0f, 1.6f, step - 0.5f) : 1.6f;
        for (int i = 0; i < count; ++i)
        {
            const auto& m = m_modArcs[static_cast<size_t>(i)];
            const float lo = m.bipolar ? position - 0.5f * std::abs(m.depth) : juce::jmin(position, position + m.depth);
            const float hi = m.bipolar ? position + 0.5f * std::abs(m.depth) : juce::jmax(position, position + m.depth);
            const float r = first + step * static_cast<float>(i);
            m_scratch.clear();
            m_scratch.addCentredArc(centre.x, centre.y, r, r, 0.0f, angleAt(lo), angleAt(hi), true);
            strokeModPath(g, m_scratch, m.colour, m.depth, thick); // dashed: negative depth
        }
        if (m_modLive >= 0.0f)
        {
            const float a = angleAt(m_modLive);
            const juce::Point<float> dot { centre.x + std::sin(a) * first, centre.y - std::cos(a) * first };
            g.setColour(TC::edgeDark);
            g.fillEllipse(juce::Rectangle<float>(6.6f, 6.6f).withCentre(dot));
            g.setColour(TC::text);
            g.fillEllipse(juce::Rectangle<float>(4.4f, 4.4f).withCentre(dot));
        }
    }
    if (! m_wireHover.isTransparent())
    {
        const float r = ring + c_modRoom - 1.0f;
        g.setColour(m_wireHover);
        g.drawEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(centre), 2.0f);
    }

    // The caption, and under it the value in the control's own units: always there, and live while it is dragged.
    if (m_valueTextStale || ! juce::exactlyEqual(m_valueTextFor, getValue()))
    {
        m_valueText = getTextFromValue(getValue());
        m_valueTextFor = getValue();
        m_valueTextStale = false;
    }
    const auto font = m_fonts.caption();
    g.setColour(drained ? TC::textFaint.withAlpha(0.7f) : TC::textDim);
    Theme::fitText(g, caption, getCaptionArea().reduced(1.0f, 0.0f), font, juce::Justification::centred);
    g.setColour(drained ? TC::textFaint.withAlpha(0.7f) : ((m_hover || m_held) ? m_accent.brighter(0.4f) : (resting ? TC::textDim : TC::text)));
    Theme::fitText(g, m_valueText, getValueArea().reduced(1.0f, 0.0f), font, juce::Justification::centred);
}

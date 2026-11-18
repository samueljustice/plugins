#include "DeHonkLookAndFeel.h"

using namespace dehonk;

namespace
{
    juce::Colour toyColourOf (const juce::Component& c, juce::Colour fallback)
    {
        if (const auto* v = c.getProperties().getVarPointer ("toyColour"))
            return juce::Colour ((juce::uint32) (juce::int64) *v);
        return fallback;
    }
}

DeHonkLookAndFeel::DeHonkLookAndFeel (const Theme::Fonts& f) : fonts (f)
{
    setColour (juce::ResizableWindow::backgroundColourId, toy::lawnA);

    setColour (juce::Slider::textBoxTextColourId, toy::ink);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, toy::sunYellow);

    setColour (juce::Label::textColourId, toy::ink);
    setColour (juce::Label::textWhenEditingColourId, toy::ink);
    setColour (juce::Label::backgroundWhenEditingColourId, toy::paper);
    setColour (juce::Label::outlineWhenEditingColourId, juce::Colours::transparentBlack);

    setColour (juce::TextButton::buttonColourId, toy::paper);
    setColour (juce::TextButton::buttonOnColourId, toy::ink);
    setColour (juce::TextButton::textColourOffId, toy::ink);
    setColour (juce::TextButton::textColourOnId, toy::ink);

    setColour (juce::ComboBox::backgroundColourId, toy::paper);
    setColour (juce::ComboBox::textColourId, toy::ink);
    setColour (juce::ComboBox::arrowColourId, toy::ink);

    setColour (juce::PopupMenu::backgroundColourId, toy::paper);
    setColour (juce::PopupMenu::textColourId, toy::ink);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, toy::sunYellow);
    setColour (juce::PopupMenu::highlightedTextColourId, toy::ink);
    setColour (juce::PopupMenu::headerTextColourId, toy::ink);

    setColour (juce::TextEditor::backgroundColourId, toy::paper);
    setColour (juce::TextEditor::textColourId, toy::ink);
    setColour (juce::TextEditor::outlineColourId, toy::ink);
    setColour (juce::TextEditor::focusedOutlineColourId, toy::ink);
    setColour (juce::TextEditor::highlightColourId, toy::sunYellow);
    setColour (juce::TextEditor::highlightedTextColourId, toy::ink);
    setColour (juce::CaretComponent::caretColourId, toy::ink);

    setColour (juce::AlertWindow::backgroundColourId, toy::paper);
    setColour (juce::AlertWindow::textColourId, toy::ink);
    setColour (juce::AlertWindow::outlineColourId, toy::ink);

    setColour (juce::TooltipWindow::backgroundColourId, toy::paper);
    setColour (juce::TooltipWindow::textColourId, toy::ink);
}

void DeHonkLookAndFeel::drawFocus (juce::Graphics& g, const juce::Component& c, juce::Rectangle<float> shape, float corner) const
{
    if (! showFocus || ! c.hasKeyboardFocus (false))
        return;
    juce::Path ring, dashed;
    const auto r = shape.reduced (3.0f);
    ring.addRoundedRectangle (r, juce::jmax (0.0f, juce::jmin (corner, r.getHeight() * 0.5f) - 3.0f));
    const float dashes[] = { 3.0f, 3.0f };
    juce::PathStrokeType (1.25f).createDashedStroke (dashed, ring, dashes, 2);
    g.setColour (toy::ink);
    g.fillPath (dashed);
}

//============================================================================== knobs
void DeHonkLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                          float startAngle, float endAngle, juce::Slider& slider)
{
    // A flat disc like a record, with a pale centre and one thin hand; round it, a row of dots that
    // fill in up to the value.
    const auto area = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const float size = juce::jmin (area.getWidth(), area.getHeight());
    const auto centre = area.getCentre();
    const float dotsR = size * 0.5f - 4.0f;
    const float discR = dotsR - 9.0f;
    const float angle = startAngle + sliderPos * (endAngle - startAngle);
    const auto colour = toyColourOf (slider, toy::sunYellow);

    constexpr int dots = 15;
    for (int i = 0; i < dots; ++i)
    {
        const float t = (float) i / (float) (dots - 1);
        const float a = startAngle + t * (endAngle - startAngle);
        const juce::Point<float> at (centre.x + dotsR * std::sin (a), centre.y - dotsR * std::cos (a));
        const bool lit = t <= sliderPos + 0.001f;
        const float r = lit ? 2.4f : 1.5f;
        g.setColour (lit ? toy::ink : toy::ink.withAlpha (0.28f));
        g.fillEllipse (at.x - r, at.y - r, r * 2.0f, r * 2.0f);
    }

    g.setColour (slider.isMouseButtonDown() ? colour.darker (0.08f) : colour);
    g.fillEllipse (centre.x - discR, centre.y - discR, discR * 2.0f, discR * 2.0f);
    g.setColour (toy::ink);
    g.drawEllipse (centre.x - discR, centre.y - discR, discR * 2.0f, discR * 2.0f, toy::outline);

    const float hubR = discR * 0.30f;
    const juce::Point<float> tip (centre.x + discR * 0.86f * std::sin (angle), centre.y - discR * 0.86f * std::cos (angle));
    g.drawLine ({ centre, tip }, 1.5f);
    g.fillEllipse (tip.x - 2.6f, tip.y - 2.6f, 5.2f, 5.2f);
    g.setColour (toy::paper);
    g.fillEllipse (centre.x - hubR, centre.y - hubR, hubR * 2.0f, hubR * 2.0f);
    g.setColour (toy::ink);
    g.drawEllipse (centre.x - hubR, centre.y - hubR, hubR * 2.0f, hubR * 2.0f, toy::outline);
    g.fillEllipse (centre.x - 1.5f, centre.y - 1.5f, 3.0f, 3.0f);
}

juce::Slider::SliderLayout DeHonkLookAndFeel::getSliderLayout (juce::Slider& slider)
{
    auto layout = juce::LookAndFeel_V4::getSliderLayout (slider);
    if (slider.getSliderStyle() == juce::Slider::LinearBar)
    {
        // The value sits in the middle of the pill, over the fill.
        layout.sliderBounds = slider.getLocalBounds();
        layout.textBoxBounds = Toy::body (slider.getLocalBounds()).toNearestInt().reduced (10, 0);
    }
    return layout;
}

//============================================================================== bars
void DeHonkLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                                          float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearBar)
    {
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const auto r = Toy::body (slider.getLocalBounds(), slider.isMouseButtonDown());
    const float t = (float) slider.valueToProportionOfLength (slider.getValue());
    const float xv = r.getX() + t * r.getWidth();
    const bool fromCentre = slider.getProperties().contains ("fromCentre");
    const float x0 = fromCentre ? juce::jmin (xv, r.getCentreX()) : r.getX();
    const float x1 = fromCentre ? juce::jmax (xv, r.getCentreX()) : xv;
    Toy::filledWell (g, r, toy::paper, toyColourOf (slider, toy::sunYellow), x0, x1, toy::outline);
    juce::ignoreUnused (x, y, width, height, sliderPos);
}

juce::Label* DeHonkLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (fonts.value (12.0f));
    label->setJustificationType (juce::Justification::centred);
    label->setColour (juce::Label::textColourId, toy::ink);
    label->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    if (slider.getSliderStyle() != juce::Slider::LinearBar)
        label->getProperties().set ("pill", true);      // a knob's value sits on its own white pill
    return label;
}

//============================================================================== labels
juce::Font DeHonkLookAndFeel::getLabelFont (juce::Label& label) { return label.getFont(); }

void DeHonkLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    // No boxes round text: names and values sit straight on the cream. Only a value being typed
    // gets a paper box.
    if (label.isBeingEdited())
        Toy::flat (g, label.getLocalBounds().toFloat().reduced (1.0f), 4.0f, toy::paper);

    if (label.isBeingEdited())
        return;

    g.setColour (label.findColour (juce::Label::textColourId).withMultipliedAlpha (label.isEnabled() ? 1.0f : 0.5f));
    g.setFont (label.getFont());
    g.drawFittedText (label.getText(), label.getLocalBounds().reduced (4, 0), label.getJustificationType(), 1, 0.8f);
}

//============================================================================== buttons
void DeHonkLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& colour, bool, bool isDown)
{
    // Flat pills. On, or held down, a button turns to ink with pale text.
    const bool pressed = isDown || button.getToggleState();
    auto fill = button.getToggleState() ? button.findColour (juce::TextButton::buttonOnColourId) : (isDown ? toy::ink : colour);
    if (! button.isEnabled())
        fill = toy::cream;
    Toy::pill (g, Toy::body (button.getLocalBounds(), pressed), fill, false);
    if (fill != toy::ink)
        drawFocus (g, button, Toy::body (button.getLocalBounds()), 100.0f);

}

juce::Font DeHonkLookAndFeel::getTextButtonFont (juce::TextButton& button, int buttonHeight)
{
    if (button.getProperties().contains ("keepCase"))
        return fonts.value (14.0f);
    return fonts.name (juce::jlimit (10.5f, 12.0f, (float) buttonHeight * 0.3f));
}

void DeHonkLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool isDown)
{
    const bool pressed = isDown || button.getToggleState();
    const auto fill = button.getToggleState() ? button.findColour (juce::TextButton::buttonOnColourId)
                                              : (isDown ? toy::ink : button.findColour (juce::TextButton::buttonColourId));
    g.setFont (getTextButtonFont (button, button.getHeight()));
    g.setColour (Toy::textOn (fill).withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.45f));
    const auto text = button.getProperties().contains ("keepCase") ? button.getButtonText() : button.getButtonText().toLowerCase();
    g.drawFittedText (text, Toy::body (button.getLocalBounds(), pressed).toNearestInt().reduced (10, 0),
                      juce::Justification::centred, 1, 0.75f);
}

//============================================================================== choice box and menus
void DeHonkLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown, int, int, int, int, juce::ComboBox& box)
{
    const auto r = Toy::body (juce::Rectangle<int> (width, height), isButtonDown);
    Toy::pill (g, r, toy::paper, isButtonDown);
    drawFocus (g, box, r, 100.0f);

    juce::Path chevron;
    const float cx = r.getRight() - r.getHeight() * 0.55f, cy = r.getCentreY();
    chevron.startNewSubPath (cx - 5.0f, cy - 2.5f);
    chevron.lineTo (cx, cy + 2.5f);
    chevron.lineTo (cx + 5.0f, cy - 2.5f);
    g.setColour (toy::ink);
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    juce::ignoreUnused (box);
}

juce::Font DeHonkLookAndFeel::getComboBoxFont (juce::ComboBox&) { return fonts.value (12.0f); }

void DeHonkLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    const auto r = Toy::body (box.getLocalBounds()).toNearestInt();
    label.setBounds (r.withTrimmedLeft (r.getHeight() / 2).withTrimmedRight (r.getHeight()));
    label.setFont (getComboBoxFont (box));
    label.setJustificationType (juce::Justification::centredLeft);
}

juce::Font DeHonkLookAndFeel::getPopupMenuFont() { return fonts.value (13.0f); }

void DeHonkLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (toy::paper);
    g.setColour (toy::ink);
    g.drawRect (0, 0, width, height, 1);
}

void DeHonkLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                                           bool isTicked, bool, const juce::String& text, const juce::String&, const juce::Drawable*, const juce::Colour*)
{
    if (isSeparator)
    {
        g.setColour (toy::ink.withAlpha (0.25f));
        g.fillRect (area.reduced (10, 0).withSizeKeepingCentre (area.getWidth() - 20, 2));
        return;
    }

    const auto row = area.toFloat().reduced (5.0f, 1.5f);
    if (isHighlighted && isActive)
        Toy::flat (g, row, row.getHeight() * 0.5f, toy::sunYellow);

    g.setColour (toy::ink.withAlpha (isActive ? 1.0f : 0.4f));
    g.setFont (getPopupMenuFont());
    g.drawFittedText (text, area.reduced (20, 0).withTrimmedLeft (10), juce::Justification::centredLeft, 1, 0.8f);
    if (isTicked)
        g.fillEllipse (row.getX() + 8.0f, row.getCentreY() - 4.0f, 8.0f, 8.0f);
}

void DeHonkLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    g.setFont (fonts.name (11.0f));
    g.setColour (toy::ink.withAlpha (0.6f));
    g.drawFittedText (sectionName.toLowerCase(), area.reduced (14, 0), juce::Justification::centredLeft, 1);
}

//============================================================================== tooltips
juce::TextLayout DeHonkLookAndFeel::layoutTooltip (const juce::String& text) const
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::centredLeft);
    s.append (text, fonts.body (13.0f), toy::ink);
    juce::TextLayout layout;
    layout.createLayoutWithBalancedLineLengths (s, 330.0f);
    return layout;
}

juce::Rectangle<int> DeHonkLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    const auto layout = layoutTooltip (tipText);
    const int w = (int) std::ceil (layout.getWidth()) + 28, h = (int) std::ceil (layout.getHeight()) + 22 + (int) toy::dropY;
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
        .constrainedWithin (parentArea);
}

void DeHonkLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto r = Toy::body (juce::Rectangle<int> (width, height));
    Toy::shape (g, r, 6.0f, toy::paper);
    layoutTooltip (text).draw (g, r.reduced (11.0f, 8.0f));
}

//============================================================================== alerts
void DeHonkLookAndFeel::drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert, const juce::Rectangle<int>& textArea, juce::TextLayout& layout)
{
    g.fillAll (toy::paper);
    g.setColour (toy::ink);
    g.drawRect (alert.getLocalBounds(), 1);
    for (int i = 0; i < 5; ++i)
    {
        g.setColour (toy::rainbow[i]);
        g.fillRect (1, 1 + i * 2, alert.getWidth() - 2, 2);
    }
    layout.draw (g, textArea.toFloat());
}

juce::Font DeHonkLookAndFeel::getAlertWindowTitleFont()   { return fonts.name (15.0f); }
juce::Font DeHonkLookAndFeel::getAlertWindowMessageFont() { return fonts.body (13.5f); }
juce::Font DeHonkLookAndFeel::getAlertWindowFont()        { return fonts.value (13.0f); }

void DeHonkLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool, bool)
{
    g.setColour (toy::ink.withAlpha (0.55f));
    for (int i = 0; i < 3; ++i)
        g.fillEllipse ((float) w - 6.0f - (float) i * 5.0f, (float) h - 6.0f, 3.0f, 3.0f);
    for (int i = 1; i < 3; ++i)
        g.fillEllipse ((float) w - 6.0f, (float) h - 6.0f - (float) i * 5.0f, 3.0f, 3.0f);
}

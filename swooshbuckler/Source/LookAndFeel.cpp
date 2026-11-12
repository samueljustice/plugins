#include "LookAndFeel.h"

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
    setColour(juce::ResizableWindow::backgroundColourId, TC::background);
    setColour(juce::Label::textColourId, TC::text);
    setColour(juce::TextButton::buttonColourId, TC::panelRaised);
    setColour(juce::TextButton::buttonOnColourId, TC::flight);
    setColour(juce::TextButton::textColourOffId, TC::textDim);
    setColour(juce::TextButton::textColourOnId, TC::text);
    setColour(juce::ComboBox::backgroundColourId, TC::panelRaised);
    setColour(juce::ComboBox::outlineColourId, TC::outline);
    setColour(juce::ComboBox::textColourId, TC::text);
    setColour(juce::ComboBox::arrowColourId, TC::textDim);
    setColour(juce::PopupMenu::backgroundColourId, TC::panelRaised);
    setColour(juce::PopupMenu::textColourId, TC::text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, TC::flight.withAlpha(0.25f));
    setColour(juce::PopupMenu::highlightedTextColourId, TC::text);
    setColour(juce::ToggleButton::textColourId, TC::textDim);
    setColour(juce::ToggleButton::tickColourId, TC::flight);
    setColour(juce::AlertWindow::backgroundColourId, TC::panel);
    setColour(juce::AlertWindow::textColourId, TC::text);
    setColour(juce::AlertWindow::outlineColourId, TC::outline);
    setColour(juce::TextEditor::backgroundColourId, TC::scope);
    setColour(juce::TextEditor::textColourId, TC::text);
    setColour(juce::TextEditor::outlineColourId, TC::outline);
    setColour(juce::TextEditor::focusedOutlineColourId, TC::flight);
    setColour(juce::TooltipWindow::backgroundColourId, TC::panelRaised);
    setColour(juce::TooltipWindow::textColourId, TC::text);
    setColour(juce::TooltipWindow::outlineColourId, TC::outline);
    setColour(juce::Slider::textBoxTextColourId, TC::text);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::HyperlinkButton::textColourId, TC::textDim);
}

//==============================================================================
void SwooshLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                         float, float, juce::Slider::SliderStyle style, juce::Slider& slider)
{
    const auto accent = slider.findColour(juce::Slider::trackColourId);
    auto r = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height));

    if (style == juce::Slider::LinearHorizontal)
    {
        const auto track = r.withSizeKeepingCentre(r.getWidth(), 4.0f);
        g.setColour(TC::outline);
        g.fillRoundedRectangle(track, 2.0f);
        g.setColour(accent.withAlpha(0.85f));
        g.fillRoundedRectangle(track.withRight(sliderPos), 2.0f);
        g.setColour(TC::brass);
        g.fillEllipse(juce::Rectangle<float>(10.0f, 10.0f).withCentre({ sliderPos, r.getCentreY() }));
        g.setColour(TC::background);
        g.fillEllipse(juce::Rectangle<float>(4.0f, 4.0f).withCentre({ sliderPos, r.getCentreY() }));
        return;
    }

    LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, 0, 0, style, slider);
}

juce::Slider::SliderLayout SwooshLookAndFeel::getSliderLayout(juce::Slider& slider)
{
    juce::Slider::SliderLayout layout;
    layout.sliderBounds = slider.getLocalBounds();
    return layout;
}

juce::Label* SwooshLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* l = LookAndFeel_V4::createSliderTextBox(slider);
    l->setFont(m_fonts.label(11.0f));
    return l;
}

void SwooshLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                             bool over, bool down)
{
    auto r = button.getLocalBounds().toFloat().reduced(0.5f);
    const auto accent = button.findColour(juce::TextButton::buttonOnColourId);
    const bool on = button.getToggleState();

    // A solid base first, so nothing behind shows through a lit button (e.g. the moon behind MORPH ON).
    g.setColour(TC::panelRaised);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(on ? accent.withAlpha(down ? 0.38f : 0.22f) : TC::panelRaised.brighter(over ? 0.12f : 0.0f).darker(down ? 0.2f : 0.0f));
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(on ? accent.withAlpha(0.9f) : (over ? TC::outline.brighter(0.5f) : TC::outline));
    g.drawRoundedRectangle(r, 4.0f, 1.0f);
}

void SwooshLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool over, bool)
{
    const bool on = button.getToggleState();
    const auto accent = button.findColour(juce::TextButton::buttonOnColourId);
    auto font = getTextButtonFont(button, button.getHeight());
    {
        // Text that would not fit even squeezed (a long preset name) is set smaller, so it is never cut short.
        const float room = static_cast<float>(button.getWidth() - 8);
        const float width = juce::GlyphArrangement::getStringWidth(font, button.getButtonText()) * 0.8f;
        if (width > room && room > 0.0f)
            font = font.withHeight(juce::jmax(7.0f, font.getHeight() * room / width * 0.98f));
    }
    g.setFont(font);
    // A button can light its text while off (e.g. MORPH ON with the pad hidden) by setting textColourOffId.
    const auto offText = button.isColourSpecified(juce::TextButton::textColourOffId) ? button.findColour(juce::TextButton::textColourOffId)
                                                                                      : (over ? TC::text : TC::textDim);
    g.setColour(! button.isEnabled() ? TC::textFaint : (on ? accent.brighter(0.4f) : offText));
    g.drawFittedText(button.getButtonText(), button.getLocalBounds().reduced(4, 0), juce::Justification::centred, 1, 0.8f);
}

juce::Font SwooshLookAndFeel::getTextButtonFont(juce::TextButton&, int height)
{
    return m_fonts.label(juce::jmin(12.0f, static_cast<float>(height) * 0.48f));
}

void SwooshLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool over, bool)
{
    auto r = button.getLocalBounds().toFloat();
    auto box = r.removeFromLeft(r.getHeight()).reduced(r.getHeight() * 0.22f);
    const auto accent = button.findColour(juce::ToggleButton::tickColourId);

    g.setColour(button.getToggleState() ? accent.withAlpha(0.25f) : TC::scope);
    g.fillRoundedRectangle(box, 3.0f);
    g.setColour(button.getToggleState() ? accent : (over ? TC::outline.brighter(0.5f) : TC::outline));
    g.drawRoundedRectangle(box, 3.0f, 1.0f);
    if (button.getToggleState())
    {
        juce::Path tick;
        tick.startNewSubPath(box.getX() + box.getWidth() * 0.22f, box.getCentreY());
        tick.lineTo(box.getX() + box.getWidth() * 0.43f, box.getBottom() - box.getHeight() * 0.25f);
        tick.lineTo(box.getRight() - box.getWidth() * 0.2f, box.getY() + box.getHeight() * 0.25f);
        g.strokePath(tick, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    g.setColour(over ? TC::text : TC::textDim);
    g.setFont(m_fonts.label(11.0f));
    g.drawFittedText(button.getButtonText(), r.toNearestInt().withTrimmedLeft(4), juce::Justification::centredLeft, 1);
}

void SwooshLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    g.setColour(TC::panelRaised.brighter(box.isMouseOver(true) ? 0.1f : 0.0f));
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(box.hasKeyboardFocus(false) ? TC::flight : TC::outline);
    g.drawRoundedRectangle(r, 4.0f, 1.0f);

    juce::Path arrow;
    const float cx = r.getRight() - 12.0f, cy = r.getCentreY();
    arrow.addTriangle(cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
    g.setColour(TC::textDim);
    g.fillPath(arrow);
}

juce::Font SwooshLookAndFeel::getComboBoxFont(juce::ComboBox&) { return m_fonts.label(12.0f); }

void SwooshLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(8, 1, box.getWidth() - 26, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
}

juce::Font SwooshLookAndFeel::getPopupMenuFont() { return m_fonts.label(13.0f); }
juce::Font SwooshLookAndFeel::getLabelFont(juce::Label&) { return m_fonts.label(12.0f); }

void SwooshLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    g.fillAll(TC::panelRaised);
    g.setColour(TC::outline);
    g.drawRect(0, 0, width, height);
}

void SwooshLookAndFeel::drawPopupMenuSectionHeader(juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& name)
{
    g.setFont(m_fonts.label(10.5f));
    g.setColour(TC::flight);
    g.drawFittedText(name.toUpperCase(), area.reduced(14, 0), juce::Justification::bottomLeft, 1);
}

void SwooshLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                          bool isHighlighted, bool isTicked, bool, const juce::String& text, const juce::String&,
                                          const juce::Drawable*, const juce::Colour*)
{
    if (isSeparator)
    {
        SwooshLookAndFeel::drawRope(g, { static_cast<float>(area.getX() + 12), static_cast<float>(area.getCentreY()) },
                                    { static_cast<float>(area.getRight() - 12), static_cast<float>(area.getCentreY()) }, TC::outline);
        return;
    }
    auto r = area.reduced(4, 1);
    if (isHighlighted && isActive)
    {
        g.setColour(TC::flight.withAlpha(0.18f));
        g.fillRoundedRectangle(r.toFloat(), 4.0f);
    }
    auto tick = r.removeFromLeft(22);
    if (isTicked)
    {
        g.setColour(TC::flight);
        g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre(tick.toFloat().getCentre()));
    }
    g.setFont(m_fonts.body(13.0f));
    g.setColour(! isActive ? TC::textFaint : (isHighlighted ? TC::text : TC::textDim.brighter(0.3f)));
    g.drawFittedText(text, r.withTrimmedRight(8), juce::Justification::centredLeft, 1);
}

void SwooshLookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int, int& idealWidth, int& idealHeight)
{
    idealHeight = isSeparator ? 10 : 26;
    idealWidth = juce::GlyphArrangement::getStringWidthInt(m_fonts.body(13.0f), text) + 52;
}

namespace
{
    // Tooltips: "Heading\nWhat it does\nHow to handle it" - the first line reads as a heading, an optional last line dimmer.
    constexpr float c_tipWidth = 380.0f, c_tipPadX = 10.0f, c_tipPadY = 7.0f;

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
            s.append(text.substring(0, nl + 1), fonts.label(12.5f), TC::flight);
            const auto rest = text.substring(nl + 1);
            const int last = rest.lastIndexOfChar('\n');
            if (last > 0)
            {
                s.append(rest.substring(0, last + 1), fonts.body(12.5f), TC::text);
                s.append(rest.substring(last + 1), fonts.body(11.5f), TC::textDim);
            }
            else
            {
                s.append(rest, fonts.body(12.5f), TC::text);
            }
        }
        else
        {
            s.append(text, fonts.body(12.5f), TC::text);
        }
        juce::TextLayout layout;
        layout.createLayout(s, width);
        return layout;
    }
}

void SwooshLookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height)
{
    auto r = juce::Rectangle<float>(0, 0, static_cast<float>(width), static_cast<float>(height));
    g.setColour(TC::panelRaised);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(TC::flight.withAlpha(0.6f));
    g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);
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
                                          i == 0 && lines.size() > 1 ? m_fonts.label(12.5f) : m_fonts.body(12.5f), lines[i])));
    const float textWidth = juce::jmin(c_tipWidth, natural + 4.0f);
    const auto layout = layoutTip(tipText, m_fonts, textWidth);
    const int width = static_cast<int>(std::ceil(layout.getWidth() + 2.0f * c_tipPadX)) + 2;
    const int height = static_cast<int>(std::ceil(layout.getHeight() + 2.0f * c_tipPadY)) + 1;
    return juce::Rectangle<int>(screenPos.x > parentArea.getCentreX() ? screenPos.x - (width + 12) : screenPos.x + 24,
                                screenPos.y > parentArea.getCentreY() ? screenPos.y - (height + 6) : screenPos.y + 6,
                                width, height).constrainedWithin(parentArea);
}

//==============================================================================
void SwooshLookAndFeel::drawPanel(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& title,
                                  const juce::String& subtitle, juce::Colour accent, const Theme::Fonts& fonts)
{
    g.setColour(TC::panel);
    g.fillRoundedRectangle(area, 8.0f);
    g.setColour(TC::outline);
    g.drawRoundedRectangle(area.reduced(0.5f), 8.0f, 1.0f);

    for (auto p : { area.getTopLeft() + juce::Point<float>(9, 9), area.getTopRight() + juce::Point<float>(-9, 9),
                    area.getBottomLeft() + juce::Point<float>(9, -9), area.getBottomRight() + juce::Point<float>(-9, -9) })
    {
        g.setColour(TC::brass.withAlpha(0.55f));
        g.fillEllipse(juce::Rectangle<float>(4.0f, 4.0f).withCentre(p));
        g.setColour(TC::panel);
        g.fillEllipse(juce::Rectangle<float>(1.6f, 1.6f).withCentre(p + juce::Point<float>(-0.5f, -0.5f)));
    }

    if (title.isEmpty())
        return;

    const float x = area.getX() + 20.0f;
    const float y = area.getY() + 10.0f;
    g.setColour(accent);
    g.fillRoundedRectangle(x, y + 3.0f, 3.0f, 13.0f, 1.5f);

    g.setFont(fonts.display(21.0f));
    g.setColour(TC::text);
    const int titleWidth = juce::GlyphArrangement::getStringWidthInt(fonts.display(21.0f), title);
    g.drawText(title, juce::Rectangle<float>(x + 10.0f, y - 2.0f, static_cast<float>(titleWidth) + 4.0f, 24.0f), juce::Justification::centredLeft);

    if (subtitle.isNotEmpty())
    {
        g.setFont(fonts.body(11.0f));
        g.setColour(TC::textFaint);
        g.drawText(subtitle, juce::Rectangle<float>(x + 20.0f + static_cast<float>(titleWidth), y, 420.0f, 20.0f), juce::Justification::centredLeft);
    }

    drawRope(g, { x, area.getY() + 34.0f }, { area.getRight() - 20.0f, area.getY() + 34.0f }, TC::outline);
}

void SwooshLookAndFeel::drawRope(juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, juce::Colour colour)
{
    const auto line = juce::Line<float>(a, b);
    const float length = line.getLength();
    if (length < 2.0f)
        return;

    const auto dir = (b - a) / length;
    const juce::Point<float> normal { -dir.y, dir.x };
    g.setColour(colour);
    for (float t = 0.0f; t < length; t += 5.0f)
    {
        const auto c = a + dir * t;
        g.drawLine(juce::Line<float>(c - normal * 1.6f - dir * 1.4f, c + normal * 1.6f + dir * 1.4f), 1.1f);
    }
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
    juce::Slider::mouseDown(e);
}

void ModTarget::mouseDrag(const juce::MouseEvent& e)
{
    if (m_modGesture)
    {
        onModDepthGesture(e, 1);
        return;
    }
    juce::Slider::mouseDrag(e);
}

void ModTarget::mouseUp(const juce::MouseEvent& e)
{
    if (m_modGesture)
    {
        m_modGesture = false;
        onModDepthGesture(e, 2);
        return;
    }
    juce::Slider::mouseUp(e);
}

void ModTarget::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.mods.isCommandDown() && isModulated() && onModDepthGesture)
    {
        onModDepthGesture(e, 3);
        return;
    }
    juce::Slider::mouseDoubleClick(e);
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

    const float arcR = radius - 2.0f;
    juce::Path track;
    track.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, c_startAngle, c_endAngle, true);
    g.setColour(TC::outline);
    g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float valueAngle = c_startAngle + static_cast<float>(valueToProportionOfLength(getValue())) * (c_endAngle - c_startAngle);
    const bool bipolar = getMinimum() < 0.0 && getMaximum() > 0.0;
    const float from = bipolar ? 0.0f : c_startAngle;
    juce::Path arc;
    arc.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, juce::jmin(from, valueAngle), juce::jmax(from, valueAngle), true);
    g.setColour(m_accent.withAlpha(0.22f));
    g.strokePath(arc, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(m_accent);
    g.strokePath(arc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

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
    const auto brass = isEnabled() ? TC::brass : TC::textFaint;
    for (int i = 0; i < 8; ++i)
    {
        const float a = angle + juce::MathConstants<float>::twoPi * static_cast<float>(i) / 8.0f;
        const juce::Point<float> dir { std::sin(a), -std::cos(a) };
        const auto inner = centre + dir * (rim * 0.25f);
        const auto outer = centre + dir * spokeEnd;
        g.setColour(i == 0 ? m_accent : brass.withAlpha(0.85f));
        g.drawLine(juce::Line<float>(inner, outer), i == 0 ? 2.6f : 1.8f);
        const float handle = i == 0 ? 3.4f : 2.6f;
        g.fillEllipse(juce::Rectangle<float>(handle * 2.0f, handle * 2.0f).withCentre(outer));
    }

    g.setColour(TC::panel);
    g.fillEllipse(juce::Rectangle<float>(rim * 2.0f, rim * 2.0f).withCentre(centre).reduced(2.0f));
    juce::ColourGradient rimGrad(brass.brighter(0.25f), centre.x, centre.y - rim, TC::brassDark, centre.x, centre.y + rim, false);
    g.setGradientFill(rimGrad);
    g.drawEllipse(juce::Rectangle<float>(rim * 2.0f, rim * 2.0f).withCentre(centre), 2.6f);
    g.drawEllipse(juce::Rectangle<float>(rim * 1.2f, rim * 1.2f).withCentre(centre), 1.2f);

    g.setColour(brass);
    g.fillEllipse(juce::Rectangle<float>(rim * 0.42f, rim * 0.42f).withCentre(centre));
    g.setColour(TC::panel);
    g.fillEllipse(juce::Rectangle<float>(rim * 0.16f, rim * 0.16f).withCentre(centre));

    const bool showValue = m_hover || isMouseButtonDown();
    g.setFont(m_fonts.label(11.0f));
    g.setColour(showValue ? m_accent.brighter(0.3f) : TC::textDim);
    g.drawFittedText(showValue ? getTextFromValue(getValue()) : caption.toUpperCase(), textArea.toNearestInt(),
                     juce::Justification::centred, 1, 0.8f);
}

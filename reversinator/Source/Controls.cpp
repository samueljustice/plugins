#include "Controls.h"

namespace
{
    const juce::Identifier c_styleProperty { "reversinatorStyle" };

    constexpr float c_valueBoxHeight = 18.0f;
    constexpr float c_valueBoxGap = 10.0f;

    juce::Colour accentOf(const juce::Slider& slider)
    {
        return slider.findColour(juce::Slider::trackColourId);
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
    : m_fonts(fonts)
{
    using namespace Theme::Colours;

    if (fonts.medium != nullptr)
        setDefaultSansSerifTypeface(fonts.medium);

    setColour(juce::ResizableWindow::backgroundColourId, background);
    setColour(juce::Label::textColourId, text);

    setColour(juce::Slider::textBoxTextColourId, text);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxHighlightColourId, reverser.withAlpha(0.35f));
    setColour(juce::TextEditor::focusedOutlineColourId, outline);
    setColour(juce::CaretComponent::caretColourId, text);

    setColour(juce::TextButton::buttonColourId, panelRaised);
    setColour(juce::TextButton::buttonOnColourId, panelRaised.brighter(0.1f));
    setColour(juce::TextButton::textColourOffId, textDim);
    setColour(juce::TextButton::textColourOnId, text);

    setColour(juce::TooltipWindow::backgroundColourId, panelRaised);
    setColour(juce::TooltipWindow::textColourId, text);
    setColour(juce::TooltipWindow::outlineColourId, outline);

    setColour(juce::ScrollBar::thumbColourId, outline);
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
    layout.sliderBounds = bounds;
    return layout;
}

juce::Label* Controls::LookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
    label->setFont(m_fonts.body(12.5f));
    label->setJustificationType(juce::Justification::centred);
    label->setColour(juce::Label::textColourId, Theme::Colours::text);
    label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    label->setColour(juce::Label::outlineWhenEditingColourId, Theme::Colours::outline);
    label->setColour(juce::Label::backgroundWhenEditingColourId, Theme::Colours::panelRaised);
    return label;
}

juce::Font Controls::LookAndFeel::getLabelFont(juce::Label& label)
{
    if (dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr)
        return m_fonts.body(12.5f);

    return m_fonts.label(10.5f);
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

void Controls::LookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                                 bool isMouseOverButton, bool isButtonDown)
{
    const auto r = button.getLocalBounds().toFloat().reduced(0.5f);
    const float corner = std::min(r.getWidth(), r.getHeight()) * 0.5f;

    g.setColour(backgroundColour.brighter(isButtonDown ? 0.2f : isMouseOverButton ? 0.1f : 0.0f));
    g.fillRoundedRectangle(r, corner);
    g.setColour(isMouseOverButton ? Theme::Colours::textDim : Theme::Colours::outline);
    g.drawRoundedRectangle(r, corner, 1.0f);
}

juce::Font Controls::LookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return m_fonts.label(std::min(13.0f, static_cast<float>(buttonHeight) * 0.5f));
}

juce::TextLayout Controls::LookAndFeel::layoutTooltip(const juce::String& text) const
{
    juce::AttributedString s;
    s.setJustification(juce::Justification::centredLeft);
    s.append(text, m_fonts.body(12.0f), Theme::Colours::text);

    juce::TextLayout layout;
    layout.createLayoutWithBalancedLineLengths(s, 300.0f);
    return layout;
}

juce::Rectangle<int> Controls::LookAndFeel::getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                                             juce::Rectangle<int> parentArea)
{
    const auto layout = layoutTooltip(tipText);
    const int w = static_cast<int>(layout.getWidth() + 22.0f);
    const int h = static_cast<int>(layout.getHeight() + 14.0f);

    return juce::Rectangle<int>(screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6,
                                w, h).constrainedWithin(parentArea);
}

void Controls::LookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    g.setColour(Theme::Colours::panelRaised);
    g.fillRoundedRectangle(r, 6.0f);
    g.setColour(Theme::Colours::outline);
    g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.0f);

    layoutTooltip(text).draw(g, r.reduced(11.0f, 7.0f));
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
}

//==============================================================================
void Controls::drawMirroredText(juce::Graphics& g, const juce::String& text, const juce::Font& font,
                                juce::Rectangle<float> area, const juce::Array<int>& mirroredIndices)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText(font, text, 0.0f, 0.0f);

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
            juce::Path outline;
            glyph.createPath(outline);
            const float pivot = outline.isEmpty() ? glyph.getLeft() + glyph.getBounds().getWidth() * 0.5f
                                                  : outline.getBounds().getCentreX();
            transform = juce::AffineTransform::scale(-1.0f, 1.0f, pivot, 0.0f).followedBy(transform);
        }

        glyph.draw(g, transform);
    }
}

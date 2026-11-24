#include "Theme.h"

#include "TempoTantrumBinaryData.h"

namespace tt::ui
{

namespace
{
    enum class MonoWeight { regular, medium, bold };

    // The embedded mono face. Each weight is loaded once and kept for the life of the app.
    juce::Typeface::Ptr monoTypeface (MonoWeight weight)
    {
        static const juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (
            TempoTantrumBinaryData::AzeretMonoRegular_ttf,
            (size_t) TempoTantrumBinaryData::AzeretMonoRegular_ttfSize);
        static const juce::Typeface::Ptr medium = juce::Typeface::createSystemTypefaceFor (
            TempoTantrumBinaryData::AzeretMonoMedium_ttf,
            (size_t) TempoTantrumBinaryData::AzeretMonoMedium_ttfSize);
        static const juce::Typeface::Ptr bold = juce::Typeface::createSystemTypefaceFor (
            TempoTantrumBinaryData::AzeretMonoBold_ttf,
            (size_t) TempoTantrumBinaryData::AzeretMonoBold_ttfSize);

        switch (weight)
        {
            case MonoWeight::regular: return regular;
            case MonoWeight::bold:    return bold;
            case MonoWeight::medium:  break;
        }

        return medium;
    }

    juce::Font monoFont (MonoWeight weight, float size)
    {
        if (auto face = monoTypeface (weight))
            return juce::Font (juce::FontOptions (face).withPointHeight (size));

        // The embedded face failed to load: use the system's fixed-width face.
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size,
                                              weight == MonoWeight::bold ? juce::Font::bold : juce::Font::plain)
                               .withPointHeight (size));
    }

    juce::TextLayout tooltipLayout (const juce::String& text)
    {
        juce::AttributedString s;
        s.setJustification (juce::Justification::centredLeft);
        s.append (text, Theme::ui (12.0f), Theme::text());

        juce::TextLayout layout;
        layout.createLayout (s, 300.0f);
        return layout;
    }
}

//==============================================================================
juce::Colour Theme::bg()        { return juce::Colour (0xff141a23); }
juce::Colour Theme::panel()     { return juce::Colour (0xff1b232f); }
juce::Colour Theme::line()      { return juce::Colour (0xff2c3746); }
juce::Colour Theme::text()      { return juce::Colour (0xffe6ecf3); }
juce::Colour Theme::dim()       { return juce::Colour (0xff8a98a9); }
juce::Colour Theme::wave()      { return juce::Colour (0xff5f86b3); }
juce::Colour Theme::drum()      { return juce::Colour (0xff3e5672); }
juce::Colour Theme::pin()       { return juce::Colour (0xfff2b441); }
juce::Colour Theme::check()     { return juce::Colour (0xffe2607a); }
juce::Colour Theme::ok()        { return juce::Colour (0xff62c08c); }
juce::Colour Theme::inkOnPin()  { return juce::Colour (0xff1a1405); }
juce::Colour Theme::inkOnWave() { return juce::Colour (0xff0c1420); }

juce::Font Theme::ui (float size, bool bold)
{
   #if JUCE_MAC
    // The system's own interface face.
    return juce::Font (juce::FontOptions (".AppleSystemUIFont", size, bold ? juce::Font::bold : juce::Font::plain)
                           .withPointHeight (size));
   #else
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain)
                           .withPointHeight (size));
   #endif
}

juce::Font Theme::mono (float size)      { return monoFont (MonoWeight::medium, size); }
juce::Font Theme::monoBold (float size)  { return monoFont (MonoWeight::bold, size); }

juce::Font Theme::fieldLabel()
{
    return mono (10.0f).withExtraKerningFactor (0.08f);
}

float Theme::textWidth (const juce::Font& font, const juce::String& text)
{
    return juce::GlyphArrangement::getStringWidth (font, text);
}

void Theme::apply (juce::LookAndFeel_V4& laf)
{
    // The scheme covers widgets not listed below (alert windows, file browsers, combo boxes).
    laf.setColourScheme ({ bg(), panel(), panel(), line(), text(), wave(), inkOnWave(), wave(), text() });

    const auto clear = juce::Colours::transparentBlack;

    laf.setColour (juce::ResizableWindow::backgroundColourId, bg());
    laf.setColour (juce::DocumentWindow::textColourId, text());

    laf.setColour (juce::TextButton::buttonColourId, panel());
    laf.setColour (juce::TextButton::buttonOnColourId, wave());
    laf.setColour (juce::TextButton::textColourOffId, text());
    laf.setColour (juce::TextButton::textColourOnId, inkOnWave());
    laf.setColour (juce::ComboBox::outlineColourId, line());
    laf.setColour (juce::ComboBox::backgroundColourId, panel());
    laf.setColour (juce::ComboBox::textColourId, text());
    laf.setColour (juce::ComboBox::arrowColourId, dim());

    laf.setColour (juce::ToggleButton::textColourId, text());
    laf.setColour (juce::ToggleButton::tickColourId, pin());
    laf.setColour (juce::ToggleButton::tickDisabledColourId, dim());

    laf.setColour (juce::Label::textColourId, text());
    laf.setColour (juce::Label::backgroundColourId, clear);
    laf.setColour (juce::Label::outlineColourId, clear);
    laf.setColour (juce::Label::textWhenEditingColourId, text());
    laf.setColour (juce::Label::backgroundWhenEditingColourId, panel());
    laf.setColour (juce::Label::outlineWhenEditingColourId, pin());

    laf.setColour (juce::TextEditor::backgroundColourId, panel());
    laf.setColour (juce::TextEditor::textColourId, text());
    laf.setColour (juce::TextEditor::highlightColourId, wave().withAlpha (0.45f));
    laf.setColour (juce::TextEditor::highlightedTextColourId, text());
    laf.setColour (juce::TextEditor::outlineColourId, line());
    laf.setColour (juce::TextEditor::focusedOutlineColourId, pin());
    laf.setColour (juce::TextEditor::shadowColourId, clear);
    laf.setColour (juce::CaretComponent::caretColourId, pin());

    laf.setColour (juce::Slider::backgroundColourId, line());
    laf.setColour (juce::Slider::trackColourId, wave());
    laf.setColour (juce::Slider::thumbColourId, text());
    laf.setColour (juce::Slider::textBoxTextColourId, text());
    laf.setColour (juce::Slider::textBoxBackgroundColourId, clear);
    laf.setColour (juce::Slider::textBoxOutlineColourId, clear);
    laf.setColour (juce::Slider::textBoxHighlightColourId, wave().withAlpha (0.45f));

    laf.setColour (juce::PopupMenu::backgroundColourId, panel());
    laf.setColour (juce::PopupMenu::textColourId, text());
    laf.setColour (juce::PopupMenu::headerTextColourId, dim());
    laf.setColour (juce::PopupMenu::highlightedBackgroundColourId, line());
    laf.setColour (juce::PopupMenu::highlightedTextColourId, text());

    laf.setColour (juce::TooltipWindow::backgroundColourId, panel());
    laf.setColour (juce::TooltipWindow::textColourId, text());
    laf.setColour (juce::TooltipWindow::outlineColourId, line());

    laf.setColour (juce::ScrollBar::thumbColourId, dim());
    laf.setColour (juce::ScrollBar::trackColourId, clear);
    laf.setColour (juce::ScrollBar::backgroundColourId, clear);

    laf.setColour (juce::ListBox::backgroundColourId, bg());
    laf.setColour (juce::ListBox::textColourId, text());
    laf.setColour (juce::ListBox::outlineColourId, line());

    laf.setColour (juce::AlertWindow::backgroundColourId, panel());
    laf.setColour (juce::AlertWindow::textColourId, text());
    laf.setColour (juce::AlertWindow::outlineColourId, line());

    laf.setColour (juce::BubbleComponent::backgroundColourId, panel());
    laf.setColour (juce::BubbleComponent::outlineColourId, line());
}

//==============================================================================
TantrumLookAndFeel::TantrumLookAndFeel()
{
    Theme::apply (*this);
}

juce::Font TantrumLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return Theme::ui (13.0f, true);
}

void TantrumLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                               const juce::Colour& backgroundColour,
                                               bool highlighted, bool down)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const float radius = juce::jmin (7.0f, bounds.getHeight() * 0.5f);
    const float alpha = button.isEnabled() ? 1.0f : 0.4f;

    auto fill = backgroundColour;

    if (down)
        fill = fill.brighter (0.22f);
    else if (highlighted)
        fill = fill.brighter (0.1f);

    g.setColour (fill.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (bounds, radius);

    const bool on = button.getToggleState();
    g.setColour ((on ? backgroundColour : Theme::line()).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (bounds, radius, 1.0f);

    if (button.hasKeyboardFocus (false))
    {
        g.setColour (Theme::text().withAlpha (0.7f));
        g.drawRoundedRectangle (bounds.reduced (1.5f), juce::jmax (1.0f, radius - 1.5f), 1.0f);
    }
}

int TantrumLookAndFeel::getSliderThumbRadius (juce::Slider&)
{
    return 6;
}

void TantrumLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float minSliderPos, float maxSliderPos,
                                           juce::Slider::SliderStyle style, juce::Slider& slider)
{
    const bool plain = style == juce::Slider::LinearHorizontal || style == juce::Slider::LinearVertical;

    if (! plain)
    {
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                                minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const float alpha = slider.isEnabled() ? 1.0f : 0.4f;
    const float thickness = 3.0f;
    const float thumb = 5.0f;
    const auto area = juce::Rectangle<int> (x, y, width, height).toFloat();

    juce::Rectangle<float> track, filled;
    juce::Point<float> centre;

    if (style == juce::Slider::LinearHorizontal)
    {
        track = { area.getX(), area.getCentreY() - thickness * 0.5f, area.getWidth(), thickness };
        filled = track.withRight (juce::jlimit (track.getX(), track.getRight(), sliderPos));
        centre = { sliderPos, area.getCentreY() };
    }
    else
    {
        track = { area.getCentreX() - thickness * 0.5f, area.getY(), thickness, area.getHeight() };
        filled = track.withTop (juce::jlimit (track.getY(), track.getBottom(), sliderPos));
        centre = { area.getCentreX(), sliderPos };
    }

    g.setColour (slider.findColour (juce::Slider::backgroundColourId).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (track, thickness * 0.5f);

    g.setColour (slider.findColour (juce::Slider::trackColourId).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (filled, thickness * 0.5f);

    g.setColour (slider.findColour (juce::Slider::thumbColourId).withMultipliedAlpha (alpha));
    g.fillEllipse (juce::Rectangle<float> (thumb * 2.0f, thumb * 2.0f).withCentre (centre));

    if (slider.hasKeyboardFocus (false))
    {
        g.setColour (Theme::pin());
        g.drawEllipse (juce::Rectangle<float> (thumb * 2.0f + 4.0f, thumb * 2.0f + 4.0f).withCentre (centre), 1.0f);
    }
}

void TantrumLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    const auto fill = editor.findColour (juce::TextEditor::backgroundColourId);

    if (fill.isTransparent())
        return;

    g.setColour (fill);
    g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), Theme::cornerRadius);
}

void TantrumLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (! editor.isEnabled())
        return;

    const bool focused = editor.hasKeyboardFocus (true) && ! editor.isReadOnly();
    const auto colour = editor.findColour (focused ? juce::TextEditor::focusedOutlineColourId
                                                   : juce::TextEditor::outlineColourId);

    if (colour.isTransparent())
        return;

    g.setColour (colour);
    g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (0.5f), Theme::cornerRadius, 1.0f);
}

juce::Font TantrumLookAndFeel::getPopupMenuFont()
{
    return Theme::ui (13.0f);
}

int TantrumLookAndFeel::getPopupMenuBorderSize()
{
    return 4;
}

void TantrumLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
    g.setColour (Theme::line());
    g.drawRect (0, 0, width, height, 1);
}

void TantrumLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                                            bool isSeparator, bool isActive, bool isHighlighted,
                                            bool isTicked, bool hasSubMenu,
                                            const juce::String& text, const juce::String& shortcutKeyText,
                                            const juce::Drawable*, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        g.setColour (Theme::line());
        g.fillRect (area.getX() + 8, area.getCentreY(), juce::jmax (0, area.getWidth() - 16), 1);
        return;
    }

    auto row = area.reduced (4, 1);

    if (isHighlighted && isActive)
    {
        g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
        g.fillRoundedRectangle (row.toFloat(), 4.0f);
    }

    auto colour = textColour != nullptr ? *textColour : findColour (juce::PopupMenu::textColourId);

    if (! isActive)
        colour = colour.withMultipliedAlpha (0.4f);

    auto content = row.reduced (8, 0);
    auto tickArea = content.removeFromLeft (14);

    if (isTicked)
    {
        g.setColour (Theme::pin().withMultipliedAlpha (isActive ? 1.0f : 0.4f));
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (tickArea.toFloat().getCentre()
                                                                           .translated (-3.0f, 0.0f)));
    }

    if (hasSubMenu)
    {
        const auto arrow = content.removeFromRight (10).toFloat();
        juce::Path p;
        p.addTriangle (arrow.getX() + 2.0f, arrow.getCentreY() - 4.0f,
                       arrow.getX() + 2.0f, arrow.getCentreY() + 4.0f,
                       arrow.getX() + 7.0f, arrow.getCentreY());
        g.setColour (colour.withMultipliedAlpha (0.7f));
        g.fillPath (p);
    }

    if (shortcutKeyText.isNotEmpty())
    {
        const auto font = Theme::mono (11.0f);
        const int w = juce::jmin (content.getWidth() / 2,
                                  (int) std::ceil (Theme::textWidth (font, shortcutKeyText)) + 4);
        g.setFont (font);
        g.setColour (Theme::dim().withMultipliedAlpha (isActive ? 1.0f : 0.4f));
        g.drawText (shortcutKeyText, content.removeFromRight (w), juce::Justification::centredRight, true);
        content.removeFromRight (10);
    }

    g.setFont (getPopupMenuFont());
    g.setColour (colour);
    g.drawText (text, content, juce::Justification::centredLeft, true);
}

juce::Rectangle<int> TantrumLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                                           juce::Rectangle<int> parentArea)
{
    const auto layout = tooltipLayout (tipText);
    const int w = (int) std::ceil (layout.getWidth()) + 18;
    const int h = (int) std::ceil (layout.getHeight()) + 12;

    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6,
                                 w, h)
        .constrainedWithin (parentArea);
}

void TantrumLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto bounds = juce::Rectangle<int> (width, height).toFloat();

    g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle (bounds, Theme::cornerRadius);
    g.setColour (findColour (juce::TooltipWindow::outlineColourId));
    g.drawRoundedRectangle (bounds.reduced (0.5f), Theme::cornerRadius, 1.0f);

    tooltipLayout (text).draw (g, bounds.reduced (9.0f, 6.0f));
}

void TantrumLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int width, int height,
                                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                                        bool isMouseOver, bool isMouseDown)
{
    if (thumbSize <= 0)
        return;

    juce::Rectangle<int> thumb = isScrollbarVertical
                                     ? juce::Rectangle<int> (x, thumbStartPosition, width, thumbSize)
                                     : juce::Rectangle<int> (thumbStartPosition, y, thumbSize, height);

    const auto r = thumb.toFloat().reduced (2.0f);
    const float alpha = isMouseDown ? 0.75f : (isMouseOver ? 0.6f : 0.4f);

    g.setColour (bar.findColour (juce::ScrollBar::thumbColourId).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (r, juce::jmin (r.getWidth(), r.getHeight()) * 0.5f);
}

} // namespace tt::ui

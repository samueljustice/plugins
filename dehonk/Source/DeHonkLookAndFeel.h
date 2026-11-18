#pragma once

#include <JuceHeader.h>
#include "Theme.h"

/** De-Honk's LookAndFeel: the toy-box versions of knobs, bar sliders, buttons, the choice box, menus,
    text boxes, scrollbars and alerts. Flat colour, ink outlines; nothing blurred, nothing cached.
    Menus are drawn inside the editor (not as windows of their own), as rounded cards.

    A control picks its colour with a property: slider.getProperties().set ("toyColour", (juce::int64) argb).
    A bar slider with the property "fromCentre" fills from its middle (Trim); with "shownProportion"
    (0..1) it fills that far whatever its value (a locked range's timing); with "lamp" (0..1) it
    has a small round lamp at its right-hand end, lit that far (the dialogue key arriving). A
    disabled bar is greyed. A text button with the property "link" is drawn as a line of text,
    underlined under the pointer; with "keepCase" its text is not lowered. A text box with the
    property "card" is a quiet panel (the licence). */
class DeHonkLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit DeHonkLookAndFeel (const Theme::Fonts& fonts);

    const Theme::Fonts& getFonts() const noexcept { return fonts; }

    /** True while the keyboard is being used to move between controls: the control with the focus
        then shows a dashed ring just inside its edge. The editor sets it. */
    bool showFocus = false;
    void drawFocus (juce::Graphics&, const juce::Component&, juce::Rectangle<float> shape, float corner) const;

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                           float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
    juce::Slider::SliderLayout getSliderLayout (juce::Slider&) override;

    juce::Font getLabelFont (juce::Label&) override;
    void drawLabel (juce::Graphics&, juce::Label&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isMouseOverButton, bool isButtonDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    int getPopupMenuBorderSize() override { return 7; }
    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu (juce::ComboBox&, juce::Label&) override;
    void drawPopupMenuUpDownArrow (juce::Graphics&, int width, int height, bool isScrollUpArrow) override;
    /** A menu drawn inside the editor asks for this round its edge: the card needs no second frame. */
    void drawResizableFrame (juce::Graphics&, int, int, const juce::BorderSize<int>&) override {}

    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    bool areScrollbarButtonsVisible() override { return false; }
    int getDefaultScrollbarWidth() override { return 12; }
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                        int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    void drawAlertBox (juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>& textArea, juce::TextLayout&) override;
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;

    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

private:
    juce::TextLayout layoutTooltip (const juce::String& text) const;

    const Theme::Fonts& fonts;

    JUCE_DECLARE_NON_COPYABLE (DeHonkLookAndFeel)
};

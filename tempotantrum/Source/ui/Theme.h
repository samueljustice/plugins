#pragma once

#include <JuceHeader.h>

// Colours, fonts and the LookAndFeel shared by every part of the window.
namespace tt::ui
{

struct Theme
{
    // The palette.
    static juce::Colour bg();        // window background
    static juce::Colour panel();     // fields, buttons, menus
    static juce::Colour line();      // 1 px borders and grid lines
    static juce::Colour text();
    static juce::Colour dim();       // labels and secondary text
    static juce::Colour wave();      // music waveform, selected segment
    static juce::Colour drum();      // drum waveform
    static juce::Colour pin();       // pins and the primary button
    static juce::Colour check();     // places the user should look at
    static juce::Colour ok();

    // Text colours for use on top of a pin() or wave() fill.
    static juce::Colour inkOnPin();
    static juce::Colour inkOnWave();

    // Fonts. Sizes are in points, so 13 here matches 13 px in the design.
    static juce::Font ui (float size, bool bold = false);   // the system face
    static juce::Font mono (float size);                    // Azeret Mono Medium, for numbers
    static juce::Font monoBold (float size);
    static juce::Font fieldLabel();                         // the small spaced capitals above a field

    static constexpr float cornerRadius = 6.0f;

    // Width of one line of text in the given font.
    static float textWidth (const juce::Font& font, const juce::String& text);

    // Sets the palette on every standard widget colour of a LookAndFeel.
    static void apply (juce::LookAndFeel_V4&);
};

// Draws the standard JUCE widgets in the palette above.
class TantrumLookAndFeel : public juce::LookAndFeel_V4
{
public:
    TantrumLookAndFeel();

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    int getSliderThumbRadius (juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    juce::Font getPopupMenuFont() override;
    int getPopupMenuBorderSize() override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                            const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override;
};

} // namespace tt::ui

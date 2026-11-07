#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"

//==============================================================================
/** Subbertone's LookAndFeel, plus a pill switch for toggles and dimmed disabled states. */
class PitchFlattenerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit PitchFlattenerLookAndFeel (const Theme::Fonts& fonts);

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    juce::Slider::SliderLayout getSliderLayout (juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;

    juce::Font getLabelFont (juce::Label&) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool isMouseOverButton, bool isButtonDown) override;

    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;

private:
    const Theme::Fonts& m_fonts;
};

//==============================================================================
/**
 * The editor scales one fixed-size content component (designed at
 * baseWidth x baseHeight) with an AffineTransform, so every size the host or
 * the corner resizer picks renders the same layout. The aspect ratio is fixed.
 */
class PitchFlattenerAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit PitchFlattenerAudioProcessorEditor (PitchFlattenerAudioProcessor&);
    ~PitchFlattenerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int baseWidth = 1000;
    static constexpr int baseHeight = 640;

private:
    class Content;

    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    PitchFlattenerLookAndFeel m_lookAndFeel;
    std::unique_ptr<Content> m_content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchFlattenerAudioProcessorEditor)
};

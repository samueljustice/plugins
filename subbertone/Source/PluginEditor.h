#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#include "WaveformVisualizer.h"

class SubbertoneAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubbertoneAudioProcessorEditor)

public:
    explicit SubbertoneAudioProcessorEditor(SubbertoneAudioProcessor& audioProcessor);
    ~SubbertoneAudioProcessorEditor() override;

    void resized() override;
    void paint(juce::Graphics&) override;
    void timerCallback() override;

private:
    enum class Format { Percent, Hertz, Decibels, Octaves, Milliseconds };

    void setupKnob(juce::Slider& slider, juce::Label& label, const juce::String& name, const juce::String& paramId,
                   const juce::String& tooltip, juce::Colour accent, Format format);
    void layoutKnobRow(juce::Rectangle<int> area, std::initializer_list<std::pair<juce::Slider*, juce::Label*>> knobs);
    void setupChoice(juce::ComboBox& combo, const juce::String& paramId, const juce::String& tooltip,
                     std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& attachment);
    void showAboutWindow();

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        explicit LookAndFeel(const Theme::Fonts& fonts);

        void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                              float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
        juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;
        juce::Label* createSliderTextBox(juce::Slider&) override;

        juce::Font getLabelFont(juce::Label&) override;

        void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                          int buttonW, int buttonH, juce::ComboBox&) override;
        juce::Font getComboBoxFont(juce::ComboBox&) override;
        void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
        juce::Font getPopupMenuFont() override;

        void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                  bool isMouseOverButton, bool isButtonDown) override;
        juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;

    private:
        const Theme::Fonts& m_fonts;
    };

    struct Section
    {
        juce::String title;
        juce::Colour accent;
        juce::Rectangle<int> bounds;
    };

    SubbertoneAudioProcessor& m_audioProcessor;

    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    LookAndFeel m_lookAndFeel;

    WaveformVisualizer m_waveformVisualizer;

    juce::Slider m_mixSlider, m_distortionSlider, m_toneSlider, m_postDriveLowpassSlider,
                 m_outputGainSlider, m_pitchThresholdSlider, m_fundamentalLimitSlider,
                 m_resonanceSlider, m_envAmountSlider, m_envReleaseSlider;

    juce::Label m_mixLabel, m_distortionLabel, m_toneLabel, m_postDriveLowpassLabel,
                m_outputGainLabel, m_pitchThresholdLabel, m_fundamentalLimitLabel,
                m_resonanceLabel, m_envAmountLabel, m_envReleaseLabel;

    juce::ComboBox m_distortionTypeCombo;
    juce::ComboBox m_filterModeCombo;

    // Attachments are declared after the controls they bind so they are destroyed first.
    juce::AudioProcessorValueTreeState::SliderAttachment   m_mixAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_distortionAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_toneAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_postDriveLowpassAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_outputGainAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_pitchThresholdAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_fundamentalLimitAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_resonanceAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_envAmountAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_envReleaseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_distortionTypeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_filterModeAttachment;

    juce::TextButton m_aboutButton;
    juce::TooltipWindow m_tooltipWindow;

    std::array<Section, 4> m_sections;
};

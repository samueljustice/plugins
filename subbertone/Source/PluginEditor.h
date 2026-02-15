#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "WaveformVisualizer.h"

class SubbertoneAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubbertoneAudioProcessorEditor)

public:
    explicit SubbertoneAudioProcessorEditor(SubbertoneAudioProcessor& audioProcessor);
    ~SubbertoneAudioProcessorEditor();


    void resized() override;
    void paint(juce::Graphics&) override;
    void timerCallback() override;

private:
    void layoutTopBar(juce::Rectangle<int>& bounds);
    void layoutVisualizer(juce::Rectangle<int>& bounds);
    void layoutControls(juce::Rectangle<int>& bounds);

    void setupRotarySlider(juce::Slider& slider, juce::Label& label, const juce::String& text, const juce::String& tooltip, const juce::String& suffix, int decimalPlaces);
    
    void showAboutWindow();

    // Custom look and feel
    class SubbertoneLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        SubbertoneLookAndFeel();
        
        void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider) override;
        juce::Slider::SliderLayout getSliderLayout(juce::Slider& slider) override;
    };

    SubbertoneAudioProcessor& m_audioProcessor;

    SubbertoneLookAndFeel m_lookAndFeel;
    
    // Visualizer
    WaveformVisualizer m_waveformVisualizer;
    
    // Controls
    juce::Slider m_mixSlider;
    juce::Slider m_distortionSlider;
    juce::Slider m_toneSlider;
    juce::Slider m_postDriveLowpassSlider;
    juce::Slider m_outputGainSlider;
    juce::Slider m_pitchThresholdSlider;
    juce::Slider m_fundamentalLimitSlider;
    
    juce::ComboBox m_distortionTypeCombo;
    
    juce::Label m_mixLabel;
    juce::Label m_distortionLabel;
    juce::Label m_toneLabel;
    juce::Label m_postDriveLowpassLabel;
    juce::Label m_distortionTypeLabel;
    juce::Label m_outputGainLabel;
    juce::Label m_pitchThresholdLabel;
    juce::Label m_fundamentalLimitLabel;
    
    // Parameter attachments (constructed after controls)
    juce::AudioProcessorValueTreeState::SliderAttachment   m_mixAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_distortionAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_toneAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_postDriveLowpassAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_outputGainAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_pitchThresholdAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_fundamentalLimitAttachment;
    juce::AudioProcessorValueTreeState::ComboBoxAttachment m_distortionTypeAttachment;
    
    // About button
    juce::TextButton m_aboutButton;
    
    // Tooltip window
    juce::TooltipWindow m_tooltipWindow;
};


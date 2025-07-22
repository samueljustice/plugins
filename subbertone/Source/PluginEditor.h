#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "WaveformVisualizer.h"

class SubbertoneAudioProcessorEditor : public juce::AudioProcessorEditor,
                                       private juce::Timer
{
public:
    SubbertoneAudioProcessorEditor(SubbertoneAudioProcessor&);
    ~SubbertoneAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    SubbertoneAudioProcessor& audioProcessor;
    
    // Custom look and feel
    class SubbertoneLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        SubbertoneLookAndFeel();
        
        void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                             float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                             juce::Slider& slider) override;
    };
    
    SubbertoneLookAndFeel lookAndFeel;
    
    // Visualizer
    std::unique_ptr<WaveformVisualizer> waveformVisualizer;
    
    // Controls
    juce::Slider mixSlider;
    juce::Slider distortionSlider;
    juce::Slider toneSlider;
    juce::Slider postDriveLowpassSlider;
    juce::Slider outputGainSlider;
    juce::Slider yinThresholdSlider;
    
    juce::ComboBox distortionTypeCombo;
    
    juce::Label mixLabel;
    juce::Label distortionLabel;
    juce::Label toneLabel;
    juce::Label postDriveLowpassLabel;
    juce::Label distortionTypeLabel;
    juce::Label outputGainLabel;
    juce::Label yinThresholdLabel;
    juce::Label signalLevelLabel;
    
    // Parameter attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> distortionAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> toneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> postDriveLowpassAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputGainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> yinThresholdAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> distortionTypeAttachment;
    
    // About button
    juce::TextButton aboutButton;
    void showAboutWindow();
    
    // Visualizer toggles
    juce::ToggleButton showInputToggle;
    juce::ToggleButton showOutputToggle;
    
    // Tooltip window
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    
    // Signal level monitoring
    float currentSignalDb = -100.0f;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubbertoneAudioProcessorEditor)
};
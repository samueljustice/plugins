#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class PitchMeter : public juce::Component, public juce::Timer
{
public:
    PitchMeter();
    void paint(juce::Graphics& g) override;
    void setFrequency(float freq);
    void setTargetFrequency(float freq);
    void timerCallback() override;
    
    static juce::String frequencyToNote(float frequency);
    
private:
    float currentFrequency = 0.0f;
    float targetFrequency = 440.0f;
    float displayFrequency = 0.0f;
    
    float frequencyToCents(float frequency, float referenceFreq = 440.0f);
};

class PitchFlattenerAudioProcessorEditor : public juce::AudioProcessorEditor, 
                                            public juce::Timer
{
public:
    PitchFlattenerAudioProcessorEditor (PitchFlattenerAudioProcessor&);
    ~PitchFlattenerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    PitchFlattenerAudioProcessor& audioProcessor;
    void updateAlgorithmControls();
    
    // UI Components
    juce::Slider targetPitchSlider;
    juce::Label targetPitchLabel;
    juce::Label targetPitchValueLabel;
    
    juce::Slider smoothingTimeSlider;
    juce::Label smoothingTimeLabel;
    
    juce::Slider mixSlider;
    juce::Label mixLabel;
    
    juce::Slider lookaheadSlider;
    juce::Label lookaheadLabel;
    
    juce::ToggleButton manualOverrideButton;
    juce::Slider overrideFreqSlider;
    juce::Label overrideFreqLabel;
    juce::Label overrideFreqValueLabel;
    
    // Base pitch latch controls
    juce::ToggleButton basePitchLatchButton;
    juce::TextButton resetBasePitchButton;
    juce::Label latchStatusLabel;
    juce::Label latchStatusValueLabel;
    juce::Slider flattenSensitivitySlider;
    juce::Label flattenSensitivityLabel;
    juce::ToggleButton hardFlattenModeButton;
    
    juce::Label basePitchLabel;
    juce::Label basePitchValueLabel;
    
    // Pitch detection controls
    juce::Slider detectionRateSlider;
    juce::Label detectionRateLabel;
    
    juce::Slider pitchThresholdSlider;
    juce::Label pitchThresholdLabel;
    
    juce::Slider minFreqSlider;
    juce::Label minFreqLabel;
    
    juce::Slider maxFreqSlider;
    juce::Label maxFreqLabel;
    
    juce::Slider volumeThresholdSlider;
    juce::Label volumeThresholdLabel;
    juce::Label volumeLevelLabel;
    
    // Advanced pitch detection controls
    juce::Slider pitchHoldTimeSlider;
    juce::Label pitchHoldTimeLabel;
    
    juce::Slider pitchJumpThresholdSlider;
    juce::Label pitchJumpThresholdLabel;
    
    juce::Slider minConfidenceSlider;
    juce::Label minConfidenceLabel;
    
    juce::Slider pitchSmoothingSlider;
    juce::Label pitchSmoothingLabel;
    
    // Detection filter controls
    juce::Slider detectionHighpassSlider;
    juce::Label detectionHighpassLabel;
    
    juce::Slider detectionLowpassSlider;
    juce::Label detectionLowpassLabel;
    
    // Pitch algorithm selector
    juce::ComboBox pitchAlgorithmSelector;
    juce::Label pitchAlgorithmLabel;
    
    // DIO-specific controls (hidden when YIN is selected)
    juce::Slider dioSpeedSlider;
    juce::Label dioSpeedLabel;
    juce::Slider dioFramePeriodSlider;
    juce::Label dioFramePeriodLabel;
    juce::Slider dioAllowedRangeSlider;
    juce::Label dioAllowedRangeLabel;
    juce::Slider dioChannelsSlider;
    juce::Label dioChannelsLabel;
    juce::Slider dioBufferTimeSlider;
    juce::Label dioBufferTimeLabel;
    
    PitchMeter pitchMeter;
    
    juce::Label titleLabel;
    juce::Label statusLabel;
    juce::HyperlinkButton websiteLink;
    
    // Section labels
    std::unique_ptr<juce::Label> mainControlsLabel;
    std::unique_ptr<juce::Label> overrideLabel;
    std::unique_ptr<juce::Label> detectionLabel;
    std::unique_ptr<juce::Label> advancedLabel;
    
    // Tooltip window
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    
    // Attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> targetPitchAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> smoothingTimeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lookaheadAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> manualOverrideAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> overrideFreqAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> detectionRateAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchThresholdAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> minFreqAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> maxFreqAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeThresholdAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchHoldTimeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchJumpThresholdAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> minConfidenceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchSmoothingAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> basePitchLatchAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> flattenSensitivityAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> hardFlattenModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> detectionHighpassAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> detectionLowpassAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> pitchAlgorithmAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> dioSpeedAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> dioFramePeriodAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> dioAllowedRangeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> dioChannelsAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> dioBufferTimeAttachment;
    
    // Look and Feel
    juce::LookAndFeel_V4 lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchFlattenerAudioProcessorEditor)
};
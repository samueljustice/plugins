#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class SliderWithReset : public juce::Component
{
public:
    SliderWithReset(const juce::String& paramID, juce::AudioProcessorValueTreeState& apvts)
        : parameterID(paramID), valueTreeState(apvts)
    {
        addAndMakeVisible(slider);
        
        resetButton.setButtonText("R");
        resetButton.setTooltip("Reset to default value");
        resetButton.onClick = [this]()
        {
            if (auto* param = valueTreeState.getParameter(parameterID))
            {
                auto defaultValue = param->getDefaultValue();
                param->setValueNotifyingHost(defaultValue);
            }
        };
        // Reset button should always be enabled, even if slider is disabled
        resetButton.setAlwaysOnTop(true);
        addAndMakeVisible(resetButton);
    }
    
    juce::Slider slider;
    
    void resized() override
    {
        auto area = getLocalBounds();
        resetButton.setBounds(area.removeFromRight(18).reduced(1));
        slider.setBounds(area);
    }
    
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> createAttachment()
    {
        return std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(valueTreeState, parameterID, slider);
    }
    
private:
    juce::TextButton resetButton;
    juce::String parameterID;
    juce::AudioProcessorValueTreeState& valueTreeState;
};

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

class PresetManager : public juce::Component
{
public:
    PresetManager(PitchFlattenerAudioProcessor& p);
    
    void resized() override;
    void savePreset();
    void deletePreset();
    void loadPresetFromFile(const juce::File& file);
    void savePresetToFile(const juce::File& file);
    void resetToDefaults();
    
private:
    PitchFlattenerAudioProcessor& processor;
    juce::ComboBox presetSelector;
    juce::TextButton saveButton{"Save Preset"};
    juce::TextButton deleteButton{"Delete"};
    juce::TextButton resetAllButton{"Reset All"};
    
    juce::File getPresetsDirectory();
    void refreshPresetList();
    void loadFactoryPresets();
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetManager)
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
    void updateAlgorithmControls();

private:
    PitchFlattenerAudioProcessor& audioProcessor;
    
    // Preset Manager
    std::unique_ptr<PresetManager> presetManager;
    
    // UI Components with reset buttons
    std::unique_ptr<SliderWithReset> targetPitchSlider;
    juce::Label targetPitchLabel;
    juce::Label targetPitchValueLabel;
    
    std::unique_ptr<SliderWithReset> smoothingTimeSlider;
    juce::Label smoothingTimeLabel;
    
    std::unique_ptr<SliderWithReset> mixSlider;
    juce::Label mixLabel;
    
    std::unique_ptr<SliderWithReset> lookaheadSlider;
    juce::Label lookaheadLabel;
    
    juce::ToggleButton manualOverrideButton;
    std::unique_ptr<SliderWithReset> overrideFreqSlider;
    juce::Label overrideFreqLabel;
    juce::Label overrideFreqValueLabel;
    
    // Base pitch latch controls
    juce::ToggleButton basePitchLatchButton;
    juce::TextButton resetBasePitchButton;
    juce::Label latchStatusLabel;
    juce::Label latchStatusValueLabel;
    std::unique_ptr<SliderWithReset> flattenSensitivitySlider;
    juce::Label flattenSensitivityLabel;
    juce::ToggleButton hardFlattenModeButton;
    
    juce::Label basePitchLabel;
    juce::Label basePitchValueLabel;
    
    // Pitch detection controls
    std::unique_ptr<SliderWithReset> detectionRateSlider;
    juce::Label detectionRateLabel;
    
    std::unique_ptr<SliderWithReset> pitchThresholdSlider;
    juce::Label pitchThresholdLabel;
    
    std::unique_ptr<SliderWithReset> minFreqSlider;
    juce::Label minFreqLabel;
    
    std::unique_ptr<SliderWithReset> maxFreqSlider;
    juce::Label maxFreqLabel;
    
    std::unique_ptr<SliderWithReset> volumeThresholdSlider;
    juce::Label volumeThresholdLabel;
    juce::Label volumeLevelLabel;
    
    // Advanced pitch detection controls
    std::unique_ptr<SliderWithReset> pitchHoldTimeSlider;
    juce::Label pitchHoldTimeLabel;
    
    std::unique_ptr<SliderWithReset> pitchJumpThresholdSlider;
    juce::Label pitchJumpThresholdLabel;
    
    std::unique_ptr<SliderWithReset> minConfidenceSlider;
    juce::Label minConfidenceLabel;
    
    std::unique_ptr<SliderWithReset> pitchSmoothingSlider;
    juce::Label pitchSmoothingLabel;
    
    // Detection filter controls
    std::unique_ptr<SliderWithReset> detectionHighpassSlider;
    juce::Label detectionHighpassLabel;
    
    std::unique_ptr<SliderWithReset> detectionLowpassSlider;
    juce::Label detectionLowpassLabel;
    
    // Pitch algorithm selector
    juce::ComboBox pitchAlgorithmSelector;
    juce::Label pitchAlgorithmLabel;
    
    // DIO-specific controls (hidden when YIN is selected)
    std::unique_ptr<SliderWithReset> dioSpeedSlider;
    juce::Label dioSpeedLabel;
    std::unique_ptr<SliderWithReset> dioFramePeriodSlider;
    juce::Label dioFramePeriodLabel;
    std::unique_ptr<SliderWithReset> dioAllowedRangeSlider;
    juce::Label dioAllowedRangeLabel;
    std::unique_ptr<SliderWithReset> dioChannelsSlider;
    juce::Label dioChannelsLabel;
    std::unique_ptr<SliderWithReset> dioBufferTimeSlider;
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
#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "WaveformVisualizer.h"
#include "PresetManager.h"

class StretchArmstrongAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    StretchArmstrongAudioProcessorEditor(StretchArmstrongAudioProcessor&);
    ~StretchArmstrongAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    StretchArmstrongAudioProcessor& audioProcessor;

    // Waveform visualizer
    std::unique_ptr<WaveformVisualizer> waveformVisualizer;

    // Preset manager
    std::unique_ptr<PresetManager> presetManager;

    // Parameter controls
    juce::Slider thresholdSlider;
    juce::Slider attackSlider;
    juce::Slider sustainSlider;
    juce::Slider releaseSlider;
    juce::Slider stretchRatioSlider;
    juce::ComboBox stretchTypeCombo;
    juce::Slider mixSlider;
    juce::Slider outputGainSlider;

    // Labels
    juce::Label thresholdLabel{"", "Threshold"};
    juce::Label attackLabel{"", "Attack"};
    juce::Label sustainLabel{"", "Sustain"};
    juce::Label releaseLabel{"", "Release"};
    juce::Label stretchRatioLabel{"", "Stretch"};
    juce::Label stretchTypeLabel{"", "Type"};
    juce::Label mixLabel{"", "Mix"};
    juce::Label outputGainLabel{"", "Output"};

    // Attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> thresholdAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sustainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> releaseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> stretchRatioAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> stretchTypeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputGainAttachment;

    // About button
    juce::TextButton aboutButton{"?"};

    void setupSlider(juce::Slider& slider, juce::Label& label, const juce::String& suffix = "");
    void showAboutWindow();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StretchArmstrongAudioProcessorEditor)
};

#pragma once

#include <JuceHeader.h>

class StretchArmstrongAudioProcessor;

class PresetManager : public juce::Component
{
public:
    PresetManager(StretchArmstrongAudioProcessor& processor);
    ~PresetManager() override = default;

    void resized() override;

private:
    StretchArmstrongAudioProcessor& processor;

    juce::ComboBox presetSelector;
    juce::TextButton saveButton{"Save"};
    juce::TextButton deleteButton{"Delete"};
    juce::TextButton resetAllButton{"Reset"};

    void savePreset();
    void deletePreset();
    void loadPresetFromFile(const juce::File& file);
    void savePresetToFile(const juce::File& file);
    void resetToDefaults();
    juce::File getPresetsDirectory();
    void refreshPresetList();
    void loadFactoryPresets();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetManager)
};

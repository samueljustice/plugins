#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <map>

/**
 * Factory and user presets for Subbertone.
 *
 * - Factory presets are compiled in and are also exposed to the host as programs.
 * - User presets are small XML files (one per preset) in the per-user presets
 *   folder, so they can be shared, backed up or edited by hand:
 *     macOS:   ~/Library/Audio/Presets/SammyJs/Subbertone
 *     Windows: %APPDATA%\SammyJs\Subbertone\Presets
 * - A preset stores every parameter by ID with its real (not normalised) value.
 *   Parameters missing from a preset fall back to their defaults, so presets
 *   made by older versions keep loading as parameters are added.
 *
 * The current preset name and whether it has been edited are written into the
 * plugin state, so a host session (or a Soundminer/VST rack recall) restores
 * exactly what was showing.
 */
class PresetManager : private juce::AudioProcessorValueTreeState::Listener
{
public:
    struct Preset
    {
        juce::String name;
        bool isFactory = false;
        juce::File file; // user presets only
        juce::String category; // factory presets: menu group ("" = top level)
    };

    static constexpr const char* c_fileExtension = ".subbertonepreset";

    explicit PresetManager(juce::AudioProcessorValueTreeState& parameters);
    ~PresetManager() override;

    /** Rescans the user folder. Factory presets always come first, in a fixed order. */
    void refresh();

    const juce::Array<Preset>& getPresets() const { return m_presets; }
    int getNumFactoryPresets() const;

    int getCurrentIndex() const { return m_currentIndex; }
    juce::String getCurrentName() const { return m_currentName; }
    bool isCurrentUserPreset() const;
    bool isModified() const { return m_modified.load(); }

    bool loadPreset(int index);
    void loadNext();
    void loadPrevious();

    /** Saves the current parameter values as a user preset (overwrites one of the same name). */
    bool saveUserPreset(const juce::String& name);
    bool deleteUserPreset(int index);
    bool userPresetExists(const juce::String& name) const;

    static juce::File getUserPresetDirectory();

    /** Store / restore the preset name and modified flag alongside the parameters. */
    void writeToState(juce::ValueTree& state) const;
    void restoreFromState(const juce::ValueTree& state);

private:
    using Values = std::map<juce::String, float>;

    void applyValues(const Values& values);
    Values captureValues() const;
    static bool readPresetFile(const juce::File& file, Values& values);
    int findIndex(const juce::String& name, bool factory) const;

    void parameterChanged(const juce::String& parameterID, float newValue) override;

    juce::AudioProcessorValueTreeState& m_parameters;
    juce::Array<Preset> m_presets;

    int m_currentIndex = 0;
    juce::String m_currentName;
    std::atomic<bool> m_modified { false };
    std::atomic<bool> m_applying { false };
};

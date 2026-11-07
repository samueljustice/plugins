#pragma once

#include <JuceHeader.h>
#include "PitchFlattenerLookAndFeel.h"

/**
 * Factory and user presets for the APVTS.
 *
 * Factory presets are generated in memory (one per Source Type, from the
 * Source Type defaults plus a sensible target setup); extra .xml files dropped
 * into Presets/Factory are listed after them. User presets are .xml files in
 * Presets/User. Nothing touches the parameters until a preset is loaded, and
 * loading wraps the state change in host gestures.
 *
 * Message thread only.
 */
class PresetManager
{
public:
    explicit PresetManager (juce::AudioProcessorValueTreeState& state);

    static constexpr const char* presetNameProperty = "presetName";

    int getNumFactoryPresets() const noexcept { return (int) m_factory.size(); }
    juce::String getFactoryPresetName (int index) const;
    juce::StringArray getUserPresetNames() const;

    bool loadFactoryPreset (int index);
    bool loadUserPreset (const juce::String& name);
    bool saveUserPreset (const juce::String& name);
    bool deleteUserPreset (const juce::String& name);
    bool userPresetExists (const juce::String& name) const;
    void resetToDefaults();

    /** Name of the last loaded/saved preset (stored in the plugin state), or empty. */
    juce::String getCurrentPresetName() const;

    /** Trimmed, file-system-safe version of a preset name (may be empty). */
    static juce::String sanitiseName (const juce::String& name);

    static juce::File getPresetRoot();
    static juce::File getUserDirectory()    { return getPresetRoot().getChildFile ("User"); }
    static juce::File getFactoryDirectory() { return getPresetRoot().getChildFile ("Factory"); }

private:
    juce::ValueTree makeDefaultState() const;
    void buildFactoryPresets();
    void applyState (const juce::ValueTree& newState, const juce::String& name);
    juce::File userFileFor (const juce::String& name) const;
    juce::File existingUserFileFor (const juce::String& listedName) const;

    juce::AudioProcessorValueTreeState& m_state;
    std::vector<std::pair<juce::String, juce::ValueTree>> m_factory;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};

//==============================================================================
/**
 * Header preset bar: preset menu plus Save / Delete / Reset.
 *
 * Its dialogs are async (plugins can't run modal loops) and drawn with the editor's LookAndFeel. The bar
 * owns the one dialog showing, so no dialog outlives the bar or that LookAndFeel.
 */
class PresetBar : public juce::Component
{
public:
    /** lookAndFeel (the editor's) must outlive the bar. */
    PresetBar (juce::AudioProcessorValueTreeState& state, PitchFlattenerLookAndFeel& lookAndFeel);
    ~PresetBar() override;

    void resized() override;

    /** Rebuilds the menu and shows the current preset name. */
    void refresh();
    /** Cheap poll: refreshes when the stored preset name changed (e.g. the host restored a session). */
    void refreshIfNameChanged();

    juce::ComboBox& getMenu() noexcept        { return m_menu; }
    juce::Button& getSaveButton() noexcept    { return m_saveButton; }
    juce::Button& getDeleteButton() noexcept  { return m_deleteButton; }
    juce::Button& getResetButton() noexcept   { return m_resetButton; }

private:
    static constexpr int c_userIdBase = 1000;

    void menuChanged();
    void showSaveDialog (const juce::String& suggestedName);
    void confirmOverwrite (const juce::String& name);
    void confirmDelete();
    void confirmReset();
    void save (const juce::String& name);
    void showError (const juce::String& title, const juce::String& message);

    /** A dialog in the theme, centred over the editor and drawn at its own scale (not the scaled content's). */
    std::unique_ptr<juce::AlertWindow> makeAlert (const juce::String& title, const juce::String& message,
                                                  juce::MessageBoxIconType icon);
    /** Shows the alert (owned by the bar, replacing any other) and calls onResult with the button's result. */
    void showAlert (std::unique_ptr<juce::AlertWindow> alert, std::function<void (int, juce::AlertWindow&)> onResult);

    PresetManager m_manager;
    PitchFlattenerLookAndFeel& m_lookAndFeel;

    juce::ComboBox m_menu;
    juce::TextButton m_saveButton { "SAVE" }, m_deleteButton { "DELETE" }, m_resetButton { "RESET" };

    juce::StringArray m_userNames;
    juce::String m_shownName;

    std::unique_ptr<juce::AlertWindow> m_alert; // owned so it never outlives the LookAndFeel

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBar)
};

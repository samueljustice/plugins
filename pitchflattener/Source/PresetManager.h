#pragma once

#include <JuceHeader.h>
#include "PitchFlattenerLookAndFeel.h"

/**
 * Factory and user presets for the APVTS.
 *
 * Factory presets are generated in memory (one per Source Type, from the
 * Source Type defaults plus a sensible target setup); extra .xml files dropped
 * into Presets/Factory are listed after them. User presets are .xml files in
 * Presets/User, listed (and named) by their file name. Nothing touches the
 * parameters until a preset is loaded, and loading wraps the state change in
 * host gestures.
 *
 * The current preset is stored in the plugin state: its name, and for a user
 * preset the file it came from, so a user preset is told apart from a factory
 * preset of the same name, and Delete acts on that file.
 *
 * Message thread only.
 */
class PresetManager
{
public:
    explicit PresetManager (juce::AudioProcessorValueTreeState& state);

    static constexpr const char* presetNameProperty = "presetName";
    static constexpr const char* presetFileProperty = "presetFile"; // user presets: file name in the User folder

    struct UserPreset
    {
        juce::File file;
        juce::String name; // the file name without its extension
    };

    int getNumFactoryPresets() const noexcept { return (int) m_factory.size(); }
    juce::String getFactoryPresetName (int index) const;
    /** The User folder's presets, sorted by name (the menu's and Previous / Next's order). */
    std::vector<UserPreset> getUserPresets() const;

    bool loadFactoryPreset (int index);
    bool loadUserPreset (const juce::File& file);
    /** Step through the factory presets, then the user presets (menu order), wrapping around. Presets that
        cannot be read are skipped. False if none could be loaded. */
    bool loadNext()     { return step (1); }
    bool loadPrevious() { return step (-1); }

    bool saveUserPreset (const juce::String& name);
    /** Moves exactly this file to the trash (or deletes it) if it is a preset in the User folder. */
    bool deleteUserPreset (const juce::File& file);
    bool userPresetExists (const juce::String& name) const;
    void resetToDefaults();

    /** Name of the last loaded/saved preset (stored in the plugin state), or empty. */
    juce::String getCurrentPresetName() const;
    /** The current preset's file when it is a user preset still in the User folder, else juce::File(). */
    juce::File getCurrentUserFile() const;
    /** Index of the current factory preset, or -1 when the current preset is not a factory preset. */
    int getCurrentFactoryIndex() const;

    /** Trimmed, file-system-safe version of a preset name (may be empty). */
    static juce::String sanitiseName (const juce::String& name);

    static juce::File getPresetRoot();
    static juce::File getUserDirectory()    { return getPresetRoot().getChildFile ("User"); }
    static juce::File getFactoryDirectory() { return getPresetRoot().getChildFile ("Factory"); }

private:
    juce::ValueTree makeDefaultState() const;
    void buildFactoryPresets();
    void applyState (const juce::ValueTree& newState, const juce::String& name, const juce::String& fileName);
    bool step (int direction);
    juce::File userFileFor (const juce::String& name) const;
    static bool isInUserFolder (const juce::File& file);

    juce::AudioProcessorValueTreeState& m_state;
    std::vector<std::pair<juce::String, juce::ValueTree>> m_factory;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};

//==============================================================================
/**
 * Header preset bar, as in Subbertone:  <  [preset name]  >  SAVE.
 *
 * The name opens the preset menu (FACTORY presets, USER presets in a submenu, then Save As, Delete, Reset
 * and Show Presets Folder); the arrows step through the presets in menu order.
 *
 * Its menu and dialogs are async (plugins can't run modal loops) and drawn with the editor's LookAndFeel.
 * The bar owns the one dialog showing, so no dialog outlives the bar or that LookAndFeel.
 */
class PresetBar : public juce::Component
{
public:
    /** lookAndFeel (the editor's) must outlive the bar. */
    PresetBar (juce::AudioProcessorValueTreeState& state, PitchFlattenerLookAndFeel& lookAndFeel);
    ~PresetBar() override;

    void resized() override;

    /** Shows the current preset name. */
    void refresh();
    /** Cheap poll: refreshes when the stored preset changed (e.g. the host restored a session). */
    void refreshIfNameChanged();

    juce::Button& getPreviousButton() noexcept { return m_previousButton; }
    juce::Button& getNameButton() noexcept     { return m_nameButton; }
    juce::Button& getNextButton() noexcept     { return m_nextButton; }
    juce::Button& getSaveButton() noexcept     { return m_saveButton; }

private:
    // Menu item IDs: factory presets are 1..getNumFactoryPresets() (they lead the list in a fixed order, so
    // their index is stable), user presets count up from c_firstUserItemId.
    static constexpr int c_saveAsItemId = 1001, c_deleteItemId = 1002, c_resetItemId = 1003, c_revealItemId = 1004;
    static constexpr int c_firstUserItemId = 100000;

    /** The preset menu and what its items act on, read once when it opens. */
    struct Menu
    {
        juce::PopupMenu menu;
        juce::Array<juce::File> userFiles; // item c_firstUserItemId + i loads userFiles[i]
        juce::File fileToDelete;           // the current user preset, or juce::File()
        juce::String nameToDelete;
    };

    Menu buildMenu();
    void showMenu();
    void menuItemChosen (int result, const Menu& menu);
    void step (int direction);

    void showSaveDialog (const juce::String& suggestedName);
    void confirmOverwrite (const juce::String& name);
    void confirmDelete (const juce::File& file, const juce::String& name);
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

    juce::TextButton m_previousButton { "<" }, m_nameButton, m_nextButton { ">" }, m_saveButton { "SAVE" };

    juce::String m_shownName, m_shownFile;

    std::unique_ptr<juce::AlertWindow> m_alert; // owned so it never outlives the LookAndFeel

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBar)
};

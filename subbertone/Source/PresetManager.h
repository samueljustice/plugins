#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

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
 * - A user preset is identified by its FILE, not by its name: two files may carry
 *   the same name (a Finder duplicate, a downloaded copy), and Save / Delete always
 *   act on the file that was loaded.
 *
 * The current preset (name, factory / user file / unsaved, edited flag) is written
 * into the plugin state, so a host session (or a Soundminer/VST rack recall)
 * restores exactly what was showing.
 *
 * Threading: every public method may be called from any thread except the audio
 * thread (which never needs to: the edited flag is tracked with atomics only).
 * Operations that change the preset (load, save, delete, refresh, state restore)
 * are serialised; getters only take a short lock and never wait for file I/O.
 * Preset loads apply the values synchronously, on the calling thread.
 */
class PresetManager : private juce::AudioProcessorParameter::Listener
{
public:
    struct Preset
    {
        juce::String name;      // factory name, or the name stored in the user preset file
        bool isFactory = false;
        juce::File file;        // user presets only
        juce::String category;  // factory presets: menu group ("" = top level)
        juce::String menuName;  // what a menu should show: the name, plus the file name when
                                // another user preset has exactly the same name
    };

    /** Everything a preset bar or menu needs, read under one lock (so it is consistent). */
    struct Snapshot
    {
        juce::Array<Preset> presets;
        int currentIndex = -1;      // index into presets, -1 if the current sound is not a listed preset
        juce::String currentName;
        juce::File currentFile;     // current user preset file, juce::File() otherwise
        bool currentIsFactory = false;
        bool currentIsUser = false;
        bool modified = false;
    };

    /** Preset information read from a plugin state, before the parameters are restored. */
    struct StateInfo
    {
        enum class Kind { factory, user, unsaved };

        bool hasPresetInfo = false; // false: a state from before presets existed (Subbertone 1.0 / 1.1)
        Kind kind = Kind::unsaved;
        juce::String name;
        juce::String fileName;      // user presets: file name inside the user preset folder
        bool modified = true;
    };

    using HostNotifier = std::function<void(const juce::AudioProcessorListener::ChangeDetails&)>;

    static constexpr const char* c_fileExtension = ".subbertonepreset";

    /** notifyHost is called (from the thread that made the change) after preset changes made
        through the plugin's own UI, so the host can update its program display and dirty flag. */
    explicit PresetManager(juce::AudioProcessorValueTreeState& parameters, HostNotifier notifyHost = {});
    ~PresetManager() override;

    /** Rescans the user folder. Factory presets always come first, in a fixed order. */
    void refresh();

    /** A copy of the current list (factory presets first, then user presets sorted by file name). */
    juce::Array<Preset> getPresets() const;
    Snapshot getSnapshot() const;

    int getNumFactoryPresets() const { return getNumFactory(); }
    static int getNumFactory();
    static juce::String getFactoryPresetName(int factoryIndex); // immutable table, safe from any thread

    int getCurrentIndex() const;         // -1 when the current sound is not a listed preset
    juce::String getCurrentName() const;
    juce::File getCurrentFile() const;   // current user preset file, juce::File() for factory / none
    bool isCurrentUserPreset() const;
    int getCurrentFactoryIndex() const;  // -1 unless the current preset is a factory preset (edited or not)
    bool isModified() const { return m_modified.load(); }

    /** Index into getPresets(). Only valid for the list it came from: prefer loadUserPreset(file). */
    bool loadPreset(int index);
    bool loadUserPreset(const juce::File& file);
    /** Step through the list; presets that cannot be read are skipped. False if none could be loaded. */
    bool loadNext();
    bool loadPrevious();

    /** Saves the current values under this name. Saving the current user preset under its own
        name writes back to its file; otherwise the file is the one userPresetExists() describes. */
    bool saveUserPreset(const juce::String& name);
    /** Deletes exactly this file if it is a user preset in the user folder, then refreshes. */
    bool deleteUserPreset(const juce::File& file);
    bool deleteUserPreset(int index);

    /** True when saving under this name would replace an existing file: a preset with exactly
        this name, or a file whose legal file name matches (case-insensitively). */
    bool userPresetExists(const juce::String& name) const;
    /** Display name of the preset that saving under 'name' would replace ("" if none). */
    juce::String getNameOfPresetSavedAs(const juce::String& name) const;
    /** False for names that cannot be saved (empty, or nothing left once illegal characters go). */
    static bool isValidPresetName(const juce::String& name);

    static juce::File getUserPresetDirectory();

    /** Host program change (setCurrentProgram): applied synchronously on the calling thread. */
    bool loadFactoryPresetForHost(int factoryIndex);

    /** Store the current preset alongside the parameters (into a copy of the state). */
    void writeToState(juce::ValueTree& state) const;
    static StateInfo readStateInfo(const juce::XmlElement& state);
    /** Restores parameters (replaceParameters, e.g. APVTS::replaceState) and then the preset. */
    void restoreState(const StateInfo& info, const std::function<void()>& replaceParameters);

private:
    enum class Kind { factory, user, unsaved };
    enum class Origin { ui, host };

    struct UserEntry
    {
        juce::File file;
        juce::String name;
    };

    struct SaveTarget
    {
        juce::File file;          // juce::File() if the name cannot be saved
        bool replacesExisting = false;
        juce::String replacedName;
    };

    struct ScopedApply
    {
        explicit ScopedApply(PresetManager& o) : owner(o) { owner.m_applyDepth.fetch_add(1); }
        ~ScopedApply() { owner.m_applyDepth.fetch_sub(1); }
        PresetManager& owner;
    };

    using Values = std::vector<std::pair<juce::String, float>>;

    // All of these expect m_operationLock to be held.
    void refreshLocked();
    bool applyFactory(int factoryIndex, Origin origin);
    bool applyUserFile(const juce::File& file, Origin origin);
    bool step(int direction);
    void applyValues(const Values& values);
    Values captureValues() const;
    SaveTarget resolveSaveTarget(const juce::String& name, const std::vector<UserEntry>& users) const;
    int currentProgram() const; // factory index, or getNumFactory() for anything else
    void publishCurrentLocked(); // m_lock held: mirrors the current factory index into m_currentFactory
    void notify(bool programChanged, bool stateChanged) const;

    static std::vector<UserEntry> scanUserFolder();
    static juce::Array<Preset> buildList(const std::vector<UserEntry>& users);
    static bool readPresetFile(const juce::File& file, Values& values, juce::String& name);
    static bool isInUserFolder(const juce::File& file);
    static int findFactory(const juce::String& name);

    void parameterValueChanged(int parameterIndex, float newValue) override;
    void parameterGestureChanged(int, bool) override {}

    juce::AudioProcessorValueTreeState& m_parameters;
    const HostNotifier m_notifyHost;

    juce::CriticalSection m_operationLock; // serialises everything that changes the preset
    mutable juce::CriticalSection m_lock;  // guards the members below; held only briefly

    juce::Array<Preset> m_presets;
    Kind m_kind = Kind::factory;
    int m_factoryIndex = 0;                // valid when m_kind == factory
    juce::File m_file;                     // valid when m_kind == user
    juce::String m_name { "Init" };
    int m_listIndex = 0;                   // position in m_presets, -1 if not listed

    std::atomic<int> m_currentFactory { 0 }; // factory index of the current preset, -1 for any other
    std::atomic<bool> m_modified { false };
    std::atomic<int> m_applyDepth { 0 };

    // Last value seen per parameter, so notifications that do not change a value are ignored.
    int m_numParameters = 0;
    std::unique_ptr<std::atomic<float>[]> m_lastValues;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetManager)
};

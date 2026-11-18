#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

/**
 * Factory and user presets for De-Honk, on the pattern of Subbertone's preset manager.
 *
 * - Factory presets are data in Presets.h. They are also the host's programs.
 * - User presets are small XML files, one per preset, in
 *     macOS:   ~/Library/Application Support/Sammy J/De-Honk/Presets
 *     Windows: %APPDATA%\Sammy J\De-Honk\Presets
 * - A preset stores every parameter except Bypass by ID, with its real (not normalised) value. A
 *   parameter a preset does not list takes its default. Solo Honk and Diff are not parameters and
 *   are never stored (SPEC 4.1).
 * - A user preset is identified by its file, not its name: Save and Delete act on the file that
 *   was loaded.
 * - The current preset (kind, name, file, edited flag) is written into the plug-in state, so a
 *   session restores what was showing.
 *
 * Threading: every public method may be called from any thread except the audio thread. Changes
 * are serialised by one lock; getters take a short lock. The edited flag is tracked with atomics,
 * so host automation on the audio thread only touches atomics.
 */
class PresetManager : private juce::AudioProcessorParameter::Listener
{
public:
    struct Preset
    {
        juce::String name;
        bool isFactory = false;
        juce::File file;          // user presets only
    };

    using HostNotifier = std::function<void (const juce::AudioProcessorListener::ChangeDetails&)>;

    static constexpr const char* fileExtension = ".dehonkpreset";

    explicit PresetManager (juce::AudioProcessorValueTreeState&, HostNotifier notifyHost = {});
    ~PresetManager() override;

    /** Rescans the user folder. Factory presets come first, in a fixed order. */
    void refresh();

    juce::Array<Preset> getPresets() const;
    int getCurrentIndex() const;             // -1 when the current sound is not a listed preset
    juce::String getCurrentName() const;
    juce::File getCurrentFile() const;       // the current user preset file, or juce::File()
    bool isCurrentUserPreset() const;
    bool isModified() const noexcept { return modified.load(); }

    static int getNumFactory() noexcept;
    static juce::String getFactoryName (int factoryIndex);
    int getCurrentFactoryIndex() const noexcept { return currentFactory.load(); }  // -1 for anything else

    bool loadPreset (int index);             // index into getPresets()
    bool loadNext();
    bool loadPrevious();
    bool loadFactoryPresetForHost (int factoryIndex);

    /** Saves the current values. Saving the current user preset under its own name writes back to
        its file; otherwise the file is named after the preset. */
    bool saveUserPreset (const juce::String& name);
    bool deleteUserPreset (const juce::File& file);
    bool userPresetExists (const juce::String& name) const;
    static bool isValidPresetName (const juce::String& name);

    static juce::File getUserPresetDirectory();

    void writeToState (juce::ValueTree& state) const;
    /** Restores the parameters (replaceParameters) and then the preset information from the state. */
    void restoreState (const juce::XmlElement& state, const std::function<void()>& replaceParameters);

private:
    enum class Kind { factory, user, unsaved };
    using Values = std::vector<std::pair<juce::String, float>>;

    struct ScopedApply
    {
        explicit ScopedApply (PresetManager& o) : owner (o) { owner.applyDepth.fetch_add (1); }
        ~ScopedApply() { owner.applyDepth.fetch_sub (1); }
        PresetManager& owner;
    };

    void refreshLocked();
    bool applyFactory (int factoryIndex, bool fromHost);
    bool applyUserFile (const juce::File& file);
    bool step (int direction);
    void applyValues (const Values& values);
    Values captureValues() const;
    juce::File targetFileFor (const juce::String& name) const;
    void notifyHost (bool programChanged) const;

    static Values factoryValues (int factoryIndex);
    static bool readPresetFile (const juce::File& file, Values& values, juce::String& name);
    static bool isPresetParameter (const juce::RangedAudioParameter&);

    void parameterValueChanged (int parameterIndex, float newValue) override;
    void parameterGestureChanged (int, bool) override {}

    juce::AudioProcessorValueTreeState& parameters;
    const HostNotifier hostNotifier;

    juce::CriticalSection operationLock;     // serialises everything that changes the preset
    mutable juce::CriticalSection dataLock;  // guards the members below; held only briefly

    juce::Array<Preset> presets;
    Kind kind = Kind::factory;
    int factoryIndex = 0;
    juce::File file;
    juce::String name;
    int listIndex = 0;

    std::atomic<int> currentFactory { 0 };
    std::atomic<bool> modified { false };
    std::atomic<int> applyDepth { 0 };

    int numParameters = 0;
    int bypassIndex = -1;
    std::unique_ptr<std::atomic<float>[]> lastValues;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};

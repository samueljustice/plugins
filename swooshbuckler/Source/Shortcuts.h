#pragma once

#include <JuceHeader.h>

#include <array>

/**
 * The keys, in one place.
 *
 * Every key the window answers to is an action here: it has an id (what the settings file calls it), a name in
 * Title Case (what Settings, the menus and the tooltips call it) and a default key. Nothing in the interface
 * tests a key code of its own - it asks the registry which action a key press is, so a key the user has moved
 * moves everywhere at once: in the window, in the menu bar, beside the item in every popup menu, and in the
 * tooltip of the control it belongs to.
 *
 * Keys are read and written on the message thread only. Nothing here touches the audio thread.
 */
namespace Shortcuts
{
    enum class Action
    {
        PlayStop = 0,
        Throw,
        Swell,
        ArmTape,
        SaveLast,
        Randomise,
        PreviousRoll,
        NextRoll,
        KeepRoll,
        PreviousPreset,
        NextPreset,
        Undo,
        Redo,
        NewSession,
        OpenSession,
        SaveSession,
        SaveSessionAs,
        Takes,
        Variations,
        Settings,
        MorphPad,
        Cables,
        Count
    };

    constexpr int numActions = static_cast<int>(Action::Count);

    /** What an action is called, and what key it has when nothing has been changed. */
    struct Info
    {
        Action action;
        const char* id;       // the settings key, and what the tests name
        const char* name;     // Title Case: what the user reads in Settings, the menus and the tooltips
        const char* fallback; // the default key, written the way the user reads it on a Mac ("Cmd+Z", "Space")
        const char* what;     // one sentence: what the action does, for the Settings list
    };

    /** Every action, in the order Settings lists them. */
    const std::array<Info, numActions>& all();

    /** The action's entry. */
    const Info& info(Action action);

    /** How a key press is written for the user: "Cmd+Z", "Shift+Cmd+S", "Space", "Return", "R", "Cmd+,".
        Mac wording; Theme::localiseShortcuts turns Cmd into Ctrl on Windows and Linux. Empty for no key. */
    juce::String describe(const juce::KeyPress& key);

    /** A key press with anything that is not Shift / Cmd / Alt / Ctrl taken off, and letters upper-cased, so
        two ways of pressing the same key compare equal. */
    juce::KeyPress normalise(const juce::KeyPress& key);

    /** Whether a key press could be a shortcut at all: not a bare modifier, not a dead key. */
    bool isBindable(const juce::KeyPress& key);

    /** Says that this control does the same thing as an action, so its tooltip can show the key. */
    inline const juce::Identifier actionProperty { "action" };
    inline void tag(juce::Component& c, Action action)
    {
        c.getProperties().set(actionProperty, static_cast<int>(action) + 1);
    }
    /** The action a control was tagged with, or Action::Count. */
    inline Action tagged(const juce::Component& c)
    {
        const int stored = static_cast<int>(c.getProperties().getWithDefault(actionProperty, 0));
        return stored > 0 && stored <= numActions ? static_cast<Action>(stored - 1) : Action::Count;
    }

    /**
     * The keys as they are now. One per editor; it reads and writes the settings file it is given (and works
     * without one, at the defaults, which is what the offline tools and the tests use).
     */
    class Registry
    {
    public:
        Registry();

        /** Reads the keys from a settings file and writes changes back to it. Pass nullptr for the defaults
            with nothing stored. */
        void attach(juce::PropertiesFile* settings);

        juce::KeyPress key(Action action) const { return m_keys[static_cast<size_t>(action)]; }
        juce::KeyPress defaultKey(Action action) const;

        /** The key as the user reads it ("Cmd+Z"), already localised: ready to draw. Empty when it has none. */
        juce::String keyText(Action action) const;

        /** The action a key press fires, or Action::Count for none. */
        Action find(const juce::KeyPress& key) const;

        /** Gives an action a key (an empty key press takes its key away). Any other action that had that key
            loses it. Returns the action it was taken from, or Action::Count. */
        Action set(Action action, const juce::KeyPress& key);

        /** Which action would lose its key if `action` were given this one, or Action::Count for none. */
        Action clash(Action action, const juce::KeyPress& key) const;

        /** Every key back to its default. */
        void resetToDefaults();

        /** Whether anything has been moved from its default. */
        bool isDefault() const;

        /** The name with its key after it, for a menu item: "Undo" with the key shown at the right of the row
            is what a popup menu does for itself, so this is only for places that draw their own rows. */
        juce::String nameWithKey(Action action) const;

        /** Adds the item to a popup menu with its key shown at the right of the row. */
        void addToMenu(juce::PopupMenu& menu, Action action, int itemId, bool enabled = true, bool ticked = false) const;

    private:
        void store(Action action);
        std::array<juce::KeyPress, numActions> m_keys;
        juce::PropertiesFile* m_settings = nullptr;
    };
}

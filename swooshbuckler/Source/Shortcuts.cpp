#include "Shortcuts.h"
#include "Theme.h"

namespace Shortcuts
{

namespace
{
    // The named keys, written the way the user reads them. Anything else is its own character ("R", "[", ",").
    struct NamedKey { int code; const char* name; };
    const NamedKey c_named[] =
    {
        { juce::KeyPress::spaceKey,     "Space" },
        { juce::KeyPress::returnKey,    "Return" },
        { juce::KeyPress::tabKey,       "Tab" },
        { juce::KeyPress::escapeKey,    "Esc" },
        { juce::KeyPress::backspaceKey, "Backspace" },
        { juce::KeyPress::deleteKey,    "Delete" },
        { juce::KeyPress::leftKey,      "Left" },
        { juce::KeyPress::rightKey,     "Right" },
        { juce::KeyPress::upKey,        "Up" },
        { juce::KeyPress::downKey,      "Down" },
        { juce::KeyPress::homeKey,      "Home" },
        { juce::KeyPress::endKey,       "End" },
        { juce::KeyPress::pageUpKey,    "Page Up" },
        { juce::KeyPress::pageDownKey,  "Page Down" },
    };

    juce::String keyName(int code)
    {
        for (const auto& named : c_named)
            if (named.code == code)
                return named.name;
        for (int i = 0; i < 12; ++i)
            if (code == juce::KeyPress::F1Key + i)
                return "F" + juce::String(i + 1);
        if (code >= 32 && code < 127)
            return juce::String::charToString(static_cast<juce::juce_wchar>(juce::CharacterFunctions::toUpperCase(
                       static_cast<juce::juce_wchar>(code))));
        return {};
    }

    int codeFromName(const juce::String& name)
    {
        for (const auto& named : c_named)
            if (name.equalsIgnoreCase(named.name))
                return named.code;
        for (int i = 0; i < 12; ++i)
            if (name.equalsIgnoreCase("F" + juce::String(i + 1)))
                return juce::KeyPress::F1Key + i;
        if (name.length() == 1)
            return static_cast<int>(juce::CharacterFunctions::toUpperCase(name[0]));
        return 0;
    }

    /** "Shift+Cmd+S" and the like, as the fallbacks are written and as describe() writes them back. */
    juce::KeyPress parse(const juce::String& text)
    {
        if (text.trim().isEmpty())
            return {};
        auto parts = juce::StringArray::fromTokens(text.trim(), "+", "");
        int mods = 0;
        while (parts.size() > 1)
        {
            const auto part = parts[0].trim();
            if (part.equalsIgnoreCase("Shift"))        mods |= juce::ModifierKeys::shiftModifier;
            else if (part.equalsIgnoreCase("Cmd") || part.equalsIgnoreCase("Command")) mods |= juce::ModifierKeys::commandModifier;
            else if (part.equalsIgnoreCase("Ctrl"))    mods |= juce::ModifierKeys::ctrlModifier;
            else if (part.equalsIgnoreCase("Alt") || part.equalsIgnoreCase("Opt"))     mods |= juce::ModifierKeys::altModifier;
            else break;
            parts.remove(0);
        }
        // "Cmd++" and "Cmd+" both mean the plus key and the key named by what is left.
        const auto rest = parts.joinIntoString("+").trim();
        const int code = codeFromName(rest.isEmpty() ? juce::String("+") : rest);
        if (code == 0)
            return {};
        return juce::KeyPress(code, juce::ModifierKeys(mods), 0);
    }

    const std::array<Info, numActions> c_all
    {{
        { Action::PlayStop,       "playStop",      "Play / Stop",            "Return",      "Starts and stops the rig." },
        { Action::Throw,          "throw",         "Throw",                  "F",           "Throws one whoosh past the listener." },
        { Action::Swell,          "swell",         "Swell",                  "Space",       "Held: every track's fader rides up." },
        { Action::ArmTape,        "armTape",       "Arm the Tape",           "T",           "Arms the tape, or stops it." },
        { Action::SaveLast,       "saveLast",      "Save the Last Minutes",  "L",           "Saves what has just been played." },
        { Action::Randomise,      "randomise",     "Randomise",              "R",           "Rolls a new set of settings." },
        { Action::PreviousRoll,   "previousRoll",  "Previous Roll",          "[",           "Goes back through the rolls." },
        { Action::NextRoll,       "nextRoll",      "Next Roll",              "]",           "Goes forward through the rolls." },
        { Action::KeepRoll,       "keepRoll",      "Keep This Roll",         "K",           "Keeps the roll as a preset of yours." },
        { Action::PreviousPreset, "previousPreset","Previous Preset",        ",",           "Loads the preset before this one." },
        { Action::NextPreset,     "nextPreset",    "Next Preset",            ".",           "Loads the preset after this one." },
        { Action::Undo,           "undo",          "Undo",                   "Cmd+Z",       "Takes back the last change." },
        { Action::Redo,           "redo",          "Redo",                   "Shift+Cmd+Z", "Puts back a change you took back." },
        { Action::NewSession,     "newSession",    "New Session",            "Cmd+N",       "Clears the rig and starts again." },
        { Action::OpenSession,    "openSession",   "Open Session...",        "Cmd+O",       "Opens a session from a file." },
        { Action::SaveSession,    "saveSession",   "Save Session",           "Cmd+S",       "Saves the session over its file." },
        { Action::SaveSessionAs,  "saveSessionAs", "Save Session As...",     "Shift+Cmd+S", "Saves the session to a new file." },
        { Action::Takes,          "takes",         "Takes",                  "Cmd+1",       "Shows what the tape has saved." },
        { Action::Variations,     "variations",    "Render Variations",      "Cmd+2",       "Shows the batch renderer." },
        { Action::Settings,       "settings",      "Settings",               "Cmd+,",       "Shows the settings." },
        { Action::MorphPad,       "morphPad",      "Morph Pad",              "M",           "Shows the morph pad over the sea." },
        { Action::Cables,         "cables",        "Cables",                 "C",           "Shows and hides the modulation cables." },
    }};
}

const std::array<Info, numActions>& all() { return c_all; }

const Info& info(Action action)
{
    const auto index = juce::jlimit(0, numActions - 1, static_cast<int>(action));
    return c_all[static_cast<size_t>(index)];
}

juce::KeyPress normalise(const juce::KeyPress& key)
{
    if (! key.isValid())
        return {};
    int code = key.getKeyCode();
    if (code >= 'a' && code <= 'z')
        code = code - 'a' + 'A';
    const auto mods = key.getModifiers();
    int kept = 0;
    if (mods.isShiftDown())   kept |= juce::ModifierKeys::shiftModifier;
    if (mods.isCommandDown()) kept |= juce::ModifierKeys::commandModifier;
    if (mods.isAltDown())     kept |= juce::ModifierKeys::altModifier;
   #if JUCE_MAC
    if (mods.isCtrlDown())    kept |= juce::ModifierKeys::ctrlModifier; // Ctrl is its own key on a Mac
   #endif
    return juce::KeyPress(code, juce::ModifierKeys(kept), 0);
}

bool isBindable(const juce::KeyPress& key)
{
    if (! key.isValid())
        return false;
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::escapeKey) // Esc closes the rebinding row instead
        return false;
    return keyName(code).isNotEmpty();
}

juce::String describe(const juce::KeyPress& key)
{
    const auto k = normalise(key);
    if (! k.isValid())
        return {};
    const auto name = keyName(k.getKeyCode());
    if (name.isEmpty())
        return {};
    const auto mods = k.getModifiers();
    juce::String text;
    // Always in this order, so the same keys are always written the same way.
    if (mods.isCtrlDown() && ! mods.isCommandDown()) text << "Ctrl+";
    if (mods.isAltDown())                            text << "Alt+";
    if (mods.isShiftDown())                          text << "Shift+";
    if (mods.isCommandDown())                        text << "Cmd+";
    return text + name;
}

//==============================================================================
Registry::Registry()
{
    resetToDefaults();
}

juce::KeyPress Registry::defaultKey(Action action) const
{
    return normalise(parse(info(action).fallback));
}

void Registry::resetToDefaults()
{
    for (int i = 0; i < numActions; ++i)
        m_keys[static_cast<size_t>(i)] = defaultKey(static_cast<Action>(i));
    if (m_settings != nullptr)
        for (int i = 0; i < numActions; ++i)
            store(static_cast<Action>(i));
}

bool Registry::isDefault() const
{
    for (int i = 0; i < numActions; ++i)
        if (! (m_keys[static_cast<size_t>(i)] == defaultKey(static_cast<Action>(i))))
            return false;
    return true;
}

void Registry::attach(juce::PropertiesFile* settings)
{
    m_settings = nullptr;   // so the reading below writes nothing back
    resetToDefaults();
    m_settings = settings;
    if (m_settings == nullptr)
        return;
    for (int i = 0; i < numActions; ++i)
    {
        const auto action = static_cast<Action>(i);
        const auto name = "shortcut_" + juce::String(info(action).id);
        if (! m_settings->containsKey(name))
            continue;
        const auto stored = m_settings->getValue(name);
        // An empty value is a key the user took away, on purpose: it is kept empty.
        m_keys[static_cast<size_t>(i)] = stored.trim().isEmpty() ? juce::KeyPress() : normalise(parse(stored));
    }
    // Two actions must never answer to one key: the later one in the list gives way.
    for (int i = 0; i < numActions; ++i)
    {
        if (! m_keys[static_cast<size_t>(i)].isValid())
            continue;
        for (int j = 0; j < i; ++j)
            if (m_keys[static_cast<size_t>(j)] == m_keys[static_cast<size_t>(i)])
            {
                m_keys[static_cast<size_t>(i)] = juce::KeyPress();
                store(static_cast<Action>(i));
                break;
            }
    }
}

void Registry::store(Action action)
{
    if (m_settings == nullptr)
        return;
    m_settings->setValue("shortcut_" + juce::String(info(action).id), describe(key(action)));
}

juce::String Registry::keyText(Action action) const
{
    return Theme::localiseShortcuts(describe(key(action)));
}

Action Registry::find(const juce::KeyPress& key) const
{
    const auto wanted = normalise(key);
    if (! wanted.isValid())
        return Action::Count;
    for (int i = 0; i < numActions; ++i)
        if (m_keys[static_cast<size_t>(i)].isValid() && m_keys[static_cast<size_t>(i)] == wanted)
            return static_cast<Action>(i);
    return Action::Count;
}

Action Registry::clash(Action action, const juce::KeyPress& key) const
{
    const auto found = find(key);
    return found == action ? Action::Count : found;
}

Action Registry::set(Action action, const juce::KeyPress& key)
{
    const auto wanted = normalise(key);
    const auto taken = clash(action, wanted);
    if (taken != Action::Count)
    {
        m_keys[static_cast<size_t>(taken)] = juce::KeyPress();
        store(taken);
    }
    m_keys[static_cast<size_t>(action)] = wanted;
    store(action);
    return taken;
}

juce::String Registry::nameWithKey(Action action) const
{
    const auto text = keyText(action);
    return text.isEmpty() ? juce::String(info(action).name) : juce::String(info(action).name) + "  " + text;
}

void Registry::addToMenu(juce::PopupMenu& menu, Action action, int itemId, bool enabled, bool ticked) const
{
    juce::PopupMenu::Item item(info(action).name);
    item.itemID = itemId;
    item.isEnabled = enabled;
    item.isTicked = ticked;
    // The key is drawn at the right of the row by the look and feel, from the key press itself.
    if (key(action).isValid())
        item.shortcutKeyDescription = keyText(action);
    menu.addItem(item);
}

} // namespace Shortcuts

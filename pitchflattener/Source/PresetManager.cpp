#include "PresetManager.h"
#include "StateMigration.h"
#include "ParamIDs.h"
#include "dsp/SourcePresets.h"

namespace
{
    const juce::Identifier c_idProperty { "id" };
    const juce::Identifier c_valueProperty { "value" };

    void setParamValue (juce::ValueTree& state, const char* paramId, float value)
    {
        auto child = state.getChildWithProperty (c_idProperty, juce::String (paramId));
        if (child.isValid())
            child.setProperty (c_valueProperty, value, nullptr);
    }

    /** Target setup for each factory preset (everything else comes from the Source Type). */
    struct FactoryTarget { pf::TargetMode mode; float autoWindowMs; };

    FactoryTarget factoryTargetFor (pf::SourceType type)
    {
        // Steady sources keep their overall contour; everything else latches.
        if (type == pf::SourceType::Drone)      return { pf::TargetMode::Auto, 1500.0f };
        if (type == pf::SourceType::NoisyField) return { pf::TargetMode::Auto, 1000.0f };
        return { pf::TargetMode::Latch, 1000.0f };
    }

    /**
     * 1.x presets have no sourceType. Mirrors the processor's session migration
     * (which is private) so an old preset doesn't silently load Siren defaults
     * for every parameter it lacks.
     */
    void migrateLegacyPreset (juce::ValueTree& state)
    {
        if (state.getChildWithProperty (c_idProperty, juce::String (ParamIDs::sourceType)).isValid())
            return;

        const auto valueOf = [&state] (const char* id, float fallback)
        {
            const auto c = state.getChildWithProperty (c_idProperty, juce::String (id));
            return c.isValid() ? (float) c.getProperty (c_valueProperty) : fallback;
        };

        const bool wasYin = valueOf ("pitchAlgorithm", 1.0f) < 0.5f;
        const bool wasLatch = valueOf ("basePitchLatch", 1.0f) > 0.5f;

        const auto addParam = [&state] (const char* id, float value)
        {
            juce::ValueTree p ("PARAM");
            p.setProperty (c_idProperty, juce::String (id), nullptr);
            p.setProperty (c_valueProperty, value, nullptr);
            state.appendChild (p, nullptr);
        };

        addParam (ParamIDs::sourceType, (float) (int) pf::SourceType::Custom);
        addParam (ParamIDs::detector, (float) (int) (wasYin ? pf::DetectorType::PYin : pf::DetectorType::SpectralPeak));
        addParam (ParamIDs::targetMode, (float) (int) (wasLatch ? pf::TargetMode::Latch : pf::TargetMode::Fixed));
        addParam (ParamIDs::quality, 0.0f);
    }
}

//==============================================================================
PresetManager::PresetManager (juce::AudioProcessorValueTreeState& state)
    : m_state (state)
{
    buildFactoryPresets();
}

juce::File PresetManager::getPresetRoot()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("SammyJs").getChildFile ("PitchFlattener").getChildFile ("Presets");
}

juce::String PresetManager::sanitiseName (const juce::String& name)
{
    return juce::File::createLegalFileName (name.trim()).trim();
}

juce::File PresetManager::userFileFor (const juce::String& name) const
{
    return getUserDirectory().getChildFile (sanitiseName (name) + ".xml");
}

bool PresetManager::isInUserFolder (const juce::File& file)
{
    return file != juce::File() && file.hasFileExtension ("xml") && file.getParentDirectory() == getUserDirectory();
}

juce::ValueTree PresetManager::makeDefaultState() const
{
    // A copy of the live state with every parameter at its default. The live
    // parameters are not touched.
    auto state = m_state.copyState();

    for (auto child : state)
    {
        if (auto* param = m_state.getParameter (child.getProperty (c_idProperty).toString()))
            child.setProperty (c_valueProperty, param->convertFrom0to1 (param->getDefaultValue()), nullptr);
    }

    state.removeProperty (presetNameProperty, nullptr);
    state.removeProperty (presetFileProperty, nullptr);
    return state;
}

void PresetManager::buildFactoryPresets()
{
    m_factory.clear();

    for (int i = 0; i < (int) pf::SourceType::NumSourceTypes; ++i)
    {
        const auto type = (pf::SourceType) i;
        const auto& p = pf::getSourcePreset (type);
        const auto target = factoryTargetFor (type);

        auto state = makeDefaultState();
        setParamValue (state, ParamIDs::sourceType, (float) i);
        setParamValue (state, ParamIDs::detector, (float) (int) p.detector);
        setParamValue (state, ParamIDs::minFreq, p.minHz);
        setParamValue (state, ParamIDs::maxFreq, p.maxHz);
        setParamValue (state, ParamIDs::sensitivity, p.sensitivity * 100.0f);
        setParamValue (state, ParamIDs::trackingSpeed, p.trackingSpeed);
        setParamValue (state, ParamIDs::trackSmoothing, p.smoothingMs);
        setParamValue (state, ParamIDs::gapFill, p.gapFillMs);
        setParamValue (state, ParamIDs::detectionHighpass, p.highpassHz);
        setParamValue (state, ParamIDs::detectionLowpass, p.lowpassHz);
        setParamValue (state, ParamIDs::rbFormantPreserve, p.formant ? 1.0f : 0.0f);
        setParamValue (state, ParamIDs::quality, (float) p.quality);
        setParamValue (state, ParamIDs::maxShift, p.maxShiftSemitones);
        setParamValue (state, ParamIDs::targetMode, (float) (int) target.mode);
        setParamValue (state, ParamIDs::autoWindow, target.autoWindowMs);

        m_factory.emplace_back (juce::String (p.name), state);
    }

    // Extra factory presets shipped as files (optional).
    const auto dir = getFactoryDirectory();
    if (dir.isDirectory())
    {
        auto files = dir.findChildFiles (juce::File::findFiles, false, "*.xml");
        files.sort();

        for (const auto& file : files)
            if (auto xml = juce::XmlDocument::parse (file))
                if (xml->hasTagName (m_state.state.getType().toString()))
                {
                    auto state = juce::ValueTree::fromXml (*xml);
                    migrateLegacyPreset (state);
                    StateMigration::overrideToFixed (state);
                    m_factory.emplace_back (file.getFileNameWithoutExtension(), state);
                }
    }
}

juce::String PresetManager::getFactoryPresetName (int index) const
{
    return juce::isPositiveAndBelow (index, (int) m_factory.size()) ? m_factory[(size_t) index].first : juce::String();
}

std::vector<PresetManager::UserPreset> PresetManager::getUserPresets() const
{
    std::vector<UserPreset> presets;
    const auto dir = getUserDirectory();

    if (dir.isDirectory())
        for (const auto& file : dir.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles, false, "*.xml"))
            presets.push_back ({ file, file.getFileNameWithoutExtension() });

    std::sort (presets.begin(), presets.end(), [] (const UserPreset& a, const UserPreset& b)
               { return a.name.compareNatural (b.name) < 0; });
    return presets;
}

void PresetManager::applyState (const juce::ValueTree& newState, const juce::String& name, const juce::String& fileName)
{
    auto state = newState.createCopy();
    state.setProperty (presetNameProperty, name, nullptr);
    state.setProperty (presetFileProperty, fileName, nullptr); // empty for a factory preset

    // Drop parameters that no longer exist (old presets), so they don't ride
    // along in the session state forever.
    for (int i = state.getNumChildren(); --i >= 0;)
    {
        const auto child = state.getChild (i);
        if (child.hasType ("PARAM") && m_state.getParameter (child.getProperty (c_idProperty).toString()) == nullptr)
            state.removeChild (i, nullptr);
    }

    // replaceState sets every parameter; bracket it in gestures so hosts record
    // the change as one edit.
    auto& params = m_state.processor.getParameters();
    for (auto* p : params) p->beginChangeGesture();
    m_state.replaceState (state);
    for (auto* p : params) p->endChangeGesture();
}

bool PresetManager::loadFactoryPreset (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) m_factory.size()))
        return false;

    applyState (m_factory[(size_t) index].second, m_factory[(size_t) index].first, {});
    return true;
}

bool PresetManager::loadUserPreset (const juce::File& file)
{
    if (! isInUserFolder (file))
        return false;

    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (m_state.state.getType().toString()))
        return false;

    auto state = juce::ValueTree::fromXml (*xml);
    migrateLegacyPreset (state);
    StateMigration::overrideToFixed (state);
    applyState (state, file.getFileNameWithoutExtension(), file.getFileName());
    return true;
}

bool PresetManager::step (int direction)
{
    // The menu's order: factory presets, then user presets by name.
    const auto users = getUserPresets();
    const int numFactory = getNumFactoryPresets();
    const int size = numFactory + (int) users.size();
    if (size == 0)
        return false;

    int current = getCurrentFactoryIndex();
    if (current < 0)
        if (const auto file = getCurrentUserFile(); file != juce::File())
            for (size_t i = 0; i < users.size(); ++i)
                if (users[i].file == file)
                    current = numFactory + (int) i;

    // From a sound that is not a listed preset, Next starts at the first preset and Previous at the last.
    const int origin = current >= 0 ? current : (direction > 0 ? -1 : size);

    for (int n = 1; n <= size; ++n)
    {
        const int index = ((origin + direction * n) % size + size) % size;
        if (index < numFactory ? loadFactoryPreset (index) : loadUserPreset (users[(size_t) (index - numFactory)].file))
            return true;
    }

    return false;
}

bool PresetManager::saveUserPreset (const juce::String& name)
{
    const auto clean = sanitiseName (name);
    if (clean.isEmpty())
        return false;

    const auto dir = getUserDirectory();
    if (! dir.isDirectory() && ! dir.createDirectory().wasOk())
        return false;

    auto state = m_state.copyState();
    state.setProperty (presetNameProperty, clean, nullptr);
    state.removeProperty (presetFileProperty, nullptr); // the file is the preset's identity, not its content

    auto xml = state.createXml();
    if (xml == nullptr || ! xml->writeTo (userFileFor (clean)))
        return false;

    // File names ignore case on macOS and Windows: "siren" may have been written over "Siren.xml". The
    // current preset is the file as it is now listed.
    auto file = userFileFor (clean);
    for (const auto& preset : getUserPresets())
        if (preset.file.getFileName().equalsIgnoreCase (file.getFileName()))
            file = preset.file;

    m_state.state.setProperty (presetNameProperty, file.getFileNameWithoutExtension(), nullptr);
    m_state.state.setProperty (presetFileProperty, file.getFileName(), nullptr);
    return true;
}

bool PresetManager::deleteUserPreset (const juce::File& file)
{
    if (! isInUserFolder (file) || ! file.existsAsFile())
        return false;

    const bool wasCurrent = getCurrentUserFile() == file;

    // Move to the trash where possible (recoverable), else delete.
    if (! file.moveToTrash() && ! file.deleteFile())
        return false;

    if (wasCurrent)
    {
        m_state.state.removeProperty (presetNameProperty, nullptr);
        m_state.state.removeProperty (presetFileProperty, nullptr);
    }

    return true;
}

bool PresetManager::userPresetExists (const juce::String& name) const
{
    const auto clean = sanitiseName (name);
    return clean.isNotEmpty() && userFileFor (clean).existsAsFile();
}

void PresetManager::resetToDefaults()
{
    applyState (makeDefaultState(), {}, {});
    m_state.state.removeProperty (presetNameProperty, nullptr);
    m_state.state.removeProperty (presetFileProperty, nullptr);
}

juce::String PresetManager::getCurrentPresetName() const
{
    return m_state.state.getProperty (presetNameProperty).toString();
}

juce::File PresetManager::getCurrentUserFile() const
{
    const auto& state = m_state.state;
    juce::String fileName;

    if (state.hasProperty (presetFileProperty))
        fileName = state.getProperty (presetFileProperty).toString();
    else if (const auto name = getCurrentPresetName(); name.isNotEmpty())
        fileName = name + ".xml"; // a session saved before the file was recorded: the name was the file's, and won

    if (fileName.isEmpty())
        return {};

    const auto file = getUserDirectory().getChildFile (fileName);
    return isInUserFolder (file) && file.existsAsFile() ? file : juce::File();
}

int PresetManager::getCurrentFactoryIndex() const
{
    // A user preset (even one whose file has gone since) is never a factory preset of the same name.
    if (m_state.state.getProperty (presetFileProperty).toString().isNotEmpty() || getCurrentUserFile() != juce::File())
        return -1;

    const auto name = getCurrentPresetName();
    if (name.isEmpty())
        return -1;

    for (size_t i = 0; i < m_factory.size(); ++i)
        if (m_factory[i].first == name)
            return (int) i;

    return -1;
}

//==============================================================================
// PresetBar
//==============================================================================
PresetBar::PresetBar (juce::AudioProcessorValueTreeState& state, PitchFlattenerLookAndFeel& lookAndFeel)
    : m_manager (state)
    , m_lookAndFeel (lookAndFeel)
{
    // As Subbertone's preset bar:  <  [name]  >  SAVE
    m_previousButton.setTooltip ("Previous preset");
    m_previousButton.onClick = [this] { step (-1); };

    m_nextButton.setTooltip ("Next preset");
    m_nextButton.onClick = [this] { step (1); };

    m_nameButton.setTooltip ("Choose a preset, or save, delete, reset and find your presets");
    m_nameButton.onClick = [this] { showMenu(); };

    m_saveButton.setTooltip ("Save the current settings as a preset");
    m_saveButton.onClick = [this] { showSaveDialog (m_manager.getCurrentPresetName()); };

    for (auto* b : { &m_previousButton, &m_nameButton, &m_nextButton, &m_saveButton })
        addAndMakeVisible (b);

    refresh();
}

PresetBar::~PresetBar()
{
    // Close any open dialog before the callbacks' target goes away (the callbacks also hold
    // SafePointers), and before the editor's LookAndFeel it is drawn with.
    m_alert.reset();
}

void PresetBar::resized()
{
    // Subbertone's geometry: SAVE 64 on the right, then 26-wide arrows either side of the name.
    auto r = getLocalBounds();
    m_saveButton.setBounds (r.removeFromRight (64));
    r.removeFromRight (8);
    m_nextButton.setBounds (r.removeFromRight (26));
    r.removeFromRight (4);
    m_previousButton.setBounds (r.removeFromLeft (26));
    r.removeFromLeft (4);
    m_nameButton.setBounds (r);
}

void PresetBar::refresh()
{
    m_shownName = m_manager.getCurrentPresetName();

    // A sound that is no saved preset (after Reset to Defaults, or once its preset is deleted) says so.
    m_nameButton.setButtonText (m_shownName.isEmpty() ? juce::String ("No preset") : m_shownName);
}

void PresetBar::refreshIfNameChanged()
{
    if (m_manager.getCurrentPresetName() != m_shownName)
        refresh();
}

void PresetBar::step (int direction)
{
    if (! (direction > 0 ? m_manager.loadNext() : m_manager.loadPrevious()))
        showError ("Couldn't Load Preset", "No preset could be loaded.");

    refresh();
}

PresetBar::Menu PresetBar::buildMenu()
{
    // Subbertone's preset menu: FACTORY presets, USER presets in a submenu (keeps long lists tidy), then
    // the actions. Ticks mark the current preset, and the submenu that holds it.
    Menu m;
    auto& menu = m.menu;
    menu.setLookAndFeel (&m_lookAndFeel); // a menu without a parent window would use JUCE's default look; submenus take it from here
    menu.addSectionHeader ("FACTORY");

    const int currentFactory = m_manager.getCurrentFactoryIndex();
    for (int i = 0; i < m_manager.getNumFactoryPresets() && i + 1 < c_saveAsItemId; ++i)
        menu.addItem (i + 1, m_manager.getFactoryPresetName (i), true, i == currentFactory);

    // User presets are told apart by their file: items load (and Delete removes) exactly that file.
    const auto currentFile = m_manager.getCurrentUserFile();
    juce::PopupMenu user;
    bool userContainsCurrent = false;

    for (const auto& preset : m_manager.getUserPresets())
    {
        const bool isCurrent = currentFile != juce::File() && preset.file == currentFile;
        user.addItem (c_firstUserItemId + m.userFiles.size(), preset.name, true, isCurrent);
        m.userFiles.add (preset.file);
        userContainsCurrent = userContainsCurrent || isCurrent;
    }

    menu.addSectionHeader ("USER");
    if (! m.userFiles.isEmpty())
        menu.addSubMenu ("User Presets", user, true, nullptr, userContainsCurrent);
    else
        menu.addItem (-1, "(none saved yet)", false);

    // Delete is bound to the preset named here, even if another one is current by the time it is confirmed.
    m.fileToDelete = currentFile;
    m.nameToDelete = currentFile != juce::File() ? currentFile.getFileNameWithoutExtension() : m_manager.getCurrentPresetName();

    menu.addSeparator();
    menu.addItem (c_saveAsItemId, "Save As...");
    menu.addItem (c_deleteItemId, m.nameToDelete.isNotEmpty() ? "Delete \"" + m.nameToDelete + "\"" : juce::String ("Delete Preset"),
                  m.fileToDelete != juce::File());
    menu.addItem (c_resetItemId, "Reset to Defaults");
    menu.addItem (c_revealItemId, "Show Presets Folder");
    return m;
}

void PresetBar::showMenu()
{
    auto built = buildMenu();
    auto menu = std::move (built.menu);
    built.menu = {};

    // At least as wide as the name button, wider only for an item that needs it.
    menu.showMenuAsync (PitchFlattenerLookAndFeel::menuOptionsFor (m_nameButton, m_nameButton.getWidth()),
                        [safeThis = juce::Component::SafePointer<PresetBar> (this), m = std::move (built)] (int result)
    {
        if (safeThis != nullptr && result != 0)
            safeThis->menuItemChosen (result, m);
    });
}

void PresetBar::menuItemChosen (int result, const Menu& m)
{
    if (result == c_saveAsItemId)
    {
        showSaveDialog (m_manager.getCurrentUserFile() != juce::File() ? m_manager.getCurrentPresetName() : juce::String());
    }
    else if (result == c_deleteItemId)
    {
        confirmDelete (m.fileToDelete, m.nameToDelete);
    }
    else if (result == c_resetItemId)
    {
        confirmReset();
    }
    else if (result == c_revealItemId)
    {
        const auto dir = PresetManager::getUserDirectory();
        dir.createDirectory();
        dir.startAsProcess(); // opens the folder in Finder / Explorer
    }
    else if (result >= c_firstUserItemId)
    {
        if (const int i = result - c_firstUserItemId; i < m.userFiles.size() && ! m_manager.loadUserPreset (m.userFiles[i]))
            showError ("Couldn't Load Preset", "The preset could not be loaded.");

        refresh();
    }
    else if (result >= 1 && result <= m_manager.getNumFactoryPresets())
    {
        m_manager.loadFactoryPreset (result - 1); // factory presets lead the list, in a fixed order
        refresh();
    }
}

void PresetBar::showSaveDialog (const juce::String& suggestedName)
{
    auto alert = makeAlert ("Save Preset", "Enter a name for the preset:", juce::MessageBoxIconType::NoIcon);
    alert->addTextEditor ("name", suggestedName, {});
    if (auto* field = alert->getTextEditor ("name"))
    {
        field->setJustification (juce::Justification::centredLeft); // vertically centred in the field
        field->setIndents (8, 0);
    }
    alert->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    showAlert (std::move (alert), [safeThis = juce::Component::SafePointer<PresetBar> (this)] (int result, juce::AlertWindow& window)
    {
        if (safeThis == nullptr || result != 1)
            return;

        // Read before another alert replaces this one.
        const auto typed = window.getTextEditorContents ("name");
        const auto name = PresetManager::sanitiseName (typed);

        if (name.isEmpty())
            safeThis->showError ("Can't Use This Name", typed.trim().isEmpty() ? juce::String ("Please enter a preset name.")
                                                                               : "\"" + typed.trim() + "\" has no characters a file name can hold. "
                                                                                 "Please use letters or numbers.");
        else if (safeThis->m_manager.userPresetExists (name))
            safeThis->confirmOverwrite (name);
        else
            safeThis->save (name);
    });
}

void PresetBar::confirmOverwrite (const juce::String& name)
{
    auto alert = makeAlert ("Overwrite Preset", "A preset called \"" + name + "\" already exists. Replace it?",
                            juce::MessageBoxIconType::QuestionIcon);
    alert->addButton ("Replace", 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    showAlert (std::move (alert), [safeThis = juce::Component::SafePointer<PresetBar> (this), name] (int result, juce::AlertWindow&)
    {
        if (safeThis != nullptr && result == 1)
            safeThis->save (name);
    });
}

void PresetBar::confirmDelete (const juce::File& file, const juce::String& name)
{
    if (file == juce::File())
        return;

    // Bound to the file named in the dialog, even if another preset is loaded by the time it is confirmed.
    auto alert = makeAlert ("Delete Preset", "Move the preset \"" + name + "\" to the trash?",
                            juce::MessageBoxIconType::WarningIcon);
    alert->addButton ("Delete", 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    showAlert (std::move (alert), [safeThis = juce::Component::SafePointer<PresetBar> (this), file] (int result, juce::AlertWindow&)
    {
        if (safeThis == nullptr || result != 1)
            return;

        // Nothing to report if the file has gone meanwhile.
        if (! safeThis->m_manager.deleteUserPreset (file) && file.existsAsFile())
            safeThis->showError ("Couldn't Delete Preset", "The preset could not be deleted.");

        safeThis->refresh();
    });
}

void PresetBar::confirmReset()
{
    auto alert = makeAlert ("Reset All", "Set every parameter back to its default?", juce::MessageBoxIconType::QuestionIcon);
    alert->addButton ("Reset", 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    showAlert (std::move (alert), [safeThis = juce::Component::SafePointer<PresetBar> (this)] (int result, juce::AlertWindow&)
    {
        if (safeThis == nullptr || result != 1)
            return;

        safeThis->m_manager.resetToDefaults();
        safeThis->refresh();
    });
}

void PresetBar::save (const juce::String& name)
{
    if (! m_manager.saveUserPreset (name))
        showError ("Couldn't Save Preset", "The preset could not be saved to\n" + PresetManager::getUserDirectory().getFullPathName());

    refresh();
}

void PresetBar::showError (const juce::String& title, const juce::String& message)
{
    auto alert = makeAlert (title, message, juce::MessageBoxIconType::WarningIcon);
    alert->addButton ("OK", 0, juce::KeyPress (juce::KeyPress::returnKey), juce::KeyPress (juce::KeyPress::escapeKey));
    showAlert (std::move (alert), {});
}

std::unique_ptr<juce::AlertWindow> PresetBar::makeAlert (const juce::String& title, const juce::String& message,
                                                         juce::MessageBoxIconType icon)
{
    // Centred over the plugin window and drawn at the editor's own scale (not the design-size content's).
    juce::Component* associated = findParentComponentOfClass<juce::AudioProcessorEditor>();
    if (associated == nullptr)
        associated = this;

    auto alert = std::make_unique<juce::AlertWindow> (title, message, icon, associated);

    // Before addButton() / addTextEditor(): they take button sizes and the field's font and outline from the
    // alert's LookAndFeel as they are called.
    alert->setLookAndFeel (&m_lookAndFeel);
    alert->setOpaque (false); // a rounded panel: the corners show what is behind the dialog
    PitchFlattenerLookAndFeel::showFocusRingsAfterTab (*alert);
    return alert;
}

void PresetBar::showAlert (std::unique_ptr<juce::AlertWindow> alert, std::function<void (int, juce::AlertWindow&)> onResult)
{
    // Plugins can't run modal loops, so alerts are async. The bar owns the window so it is always
    // destroyed before the LookAndFeel it uses.
    m_alert = std::move (alert);
    m_alert->setLookAndFeel (&m_lookAndFeel);

    m_alert->enterModalState (true, juce::ModalCallbackFunction::create (
        [safeThis = juce::Component::SafePointer<PresetBar> (this), onResult] (int result)
        {
            if (safeThis == nullptr || safeThis->m_alert == nullptr)
                return;

            if (onResult)
                onResult (result, *safeThis->m_alert);

            // Defer the delete: we're inside the window's own callback. A dialog that onResult opened
            // in its place is modal, so it stays.
            juce::MessageManager::callAsync ([safeThis]
            {
                if (safeThis != nullptr && safeThis->m_alert != nullptr && ! safeThis->m_alert->isCurrentlyModal())
                    safeThis->m_alert.reset();
            });
        }), false);
}

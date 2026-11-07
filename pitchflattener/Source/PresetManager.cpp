#include "PresetManager.h"
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

juce::File PresetManager::existingUserFileFor (const juce::String& listedName) const
{
    // Names in the menu are file names from the directory listing; use them
    // as-is (re-sanitising would miss files whose names contain characters
    // createLegalFileName strips), but never step outside the User folder.
    const auto dir = getUserDirectory();
    const auto file = dir.getChildFile (listedName + ".xml");

    if (listedName.isNotEmpty() && file.getParentDirectory() == dir && file.existsAsFile())
        return file;

    return userFileFor (listedName);
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
                    m_factory.emplace_back (file.getFileNameWithoutExtension(), juce::ValueTree::fromXml (*xml));
    }
}

juce::String PresetManager::getFactoryPresetName (int index) const
{
    return juce::isPositiveAndBelow (index, (int) m_factory.size()) ? m_factory[(size_t) index].first : juce::String();
}

juce::StringArray PresetManager::getUserPresetNames() const
{
    juce::StringArray names;
    const auto dir = getUserDirectory();

    if (dir.isDirectory())
        for (const auto& file : dir.findChildFiles (juce::File::findFiles, false, "*.xml"))
            names.add (file.getFileNameWithoutExtension());

    names.sortNatural();
    return names;
}

void PresetManager::applyState (const juce::ValueTree& newState, const juce::String& name)
{
    auto state = newState.createCopy();
    state.setProperty (presetNameProperty, name, nullptr);

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

    applyState (m_factory[(size_t) index].second, m_factory[(size_t) index].first);
    return true;
}

bool PresetManager::loadUserPreset (const juce::String& name)
{
    const auto file = existingUserFileFor (name);
    auto xml = juce::XmlDocument::parse (file);

    if (xml == nullptr || ! xml->hasTagName (m_state.state.getType().toString()))
        return false;

    auto state = juce::ValueTree::fromXml (*xml);
    migrateLegacyPreset (state);
    applyState (state, file.getFileNameWithoutExtension());
    return true;
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

    auto xml = state.createXml();
    if (xml == nullptr || ! xml->writeTo (userFileFor (clean)))
        return false;

    m_state.state.setProperty (presetNameProperty, clean, nullptr);
    return true;
}

bool PresetManager::deleteUserPreset (const juce::String& name)
{
    const auto file = existingUserFileFor (name);
    if (! file.existsAsFile())
        return false;

    // Move to the trash where possible (recoverable), else delete.
    if (! file.moveToTrash() && ! file.deleteFile())
        return false;

    if (getCurrentPresetName() == name)
        m_state.state.removeProperty (presetNameProperty, nullptr);

    return true;
}

bool PresetManager::userPresetExists (const juce::String& name) const
{
    const auto clean = sanitiseName (name);
    return clean.isNotEmpty() && userFileFor (clean).existsAsFile();
}

void PresetManager::resetToDefaults()
{
    applyState (makeDefaultState(), {});
    m_state.state.removeProperty (presetNameProperty, nullptr);
}

juce::String PresetManager::getCurrentPresetName() const
{
    return m_state.state.getProperty (presetNameProperty).toString();
}

//==============================================================================
// PresetBar
//==============================================================================
PresetBar::PresetBar (juce::AudioProcessorValueTreeState& state, PitchFlattenerLookAndFeel& lookAndFeel)
    : m_manager (state)
    , m_lookAndFeel (lookAndFeel)
{
    m_menu.setTextWhenNothingSelected ("No preset");
    m_menu.onChange = [this] { menuChanged(); };
    addAndMakeVisible (m_menu);

    m_saveButton.onClick = [this] { showSaveDialog (m_manager.getCurrentPresetName()); };
    m_deleteButton.onClick = [this] { confirmDelete(); };
    m_resetButton.onClick = [this] { confirmReset(); };

    for (auto* b : { &m_saveButton, &m_deleteButton, &m_resetButton })
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
    auto r = getLocalBounds();
    m_resetButton.setBounds (r.removeFromRight (62));
    r.removeFromRight (6);
    m_deleteButton.setBounds (r.removeFromRight (70));
    r.removeFromRight (6);
    m_saveButton.setBounds (r.removeFromRight (58));
    r.removeFromRight (10);
    m_menu.setBounds (r);
}

void PresetBar::refresh()
{
    m_menu.clear (juce::dontSendNotification);
    m_userNames = m_manager.getUserPresetNames();

    const auto current = m_manager.getCurrentPresetName();
    m_shownName = current;
    int selectedId = 0;

    m_menu.addSectionHeading ("FACTORY");
    for (int i = 0; i < m_manager.getNumFactoryPresets(); ++i)
    {
        const auto name = m_manager.getFactoryPresetName (i);
        m_menu.addItem (name, i + 1);
        if (selectedId == 0 && name == current)
            selectedId = i + 1;
    }

    m_menu.addSeparator();
    m_menu.addSectionHeading ("USER");

    if (m_userNames.isEmpty())
    {
        m_menu.addItem ("(none saved yet)", c_userIdBase - 1);
        m_menu.setItemEnabled (c_userIdBase - 1, false);
    }

    for (int i = 0; i < m_userNames.size(); ++i)
    {
        m_menu.addItem (m_userNames[i], c_userIdBase + i);
        if (m_userNames[i] == current)
            selectedId = c_userIdBase + i; // user presets take precedence over a same-named factory one
    }

    if (selectedId != 0)
        m_menu.setSelectedId (selectedId, juce::dontSendNotification);
    else
        m_menu.setText (current, juce::dontSendNotification);

    m_deleteButton.setEnabled (m_menu.getSelectedId() >= c_userIdBase);
}

void PresetBar::refreshIfNameChanged()
{
    if (m_manager.getCurrentPresetName() != m_shownName && ! m_menu.isPopupActive())
        refresh();
}

void PresetBar::menuChanged()
{
    const int id = m_menu.getSelectedId();
    bool ok = false;

    if (id >= c_userIdBase)
        ok = m_manager.loadUserPreset (m_userNames[id - c_userIdBase]);
    else if (id > 0)
        ok = m_manager.loadFactoryPreset (id - 1);

    if (id > 0 && ! ok)
        showError ("Couldn't Load Preset", "The preset could not be loaded.");

    refresh();
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

void PresetBar::confirmDelete()
{
    const int id = m_menu.getSelectedId();
    if (id < c_userIdBase)
        return;

    // Bound to the preset named in the dialog, even if another one is loaded by the time it is confirmed.
    const auto name = m_userNames[id - c_userIdBase];
    auto alert = makeAlert ("Delete Preset", "Move the preset \"" + name + "\" to the trash?",
                            juce::MessageBoxIconType::WarningIcon);
    alert->addButton ("Delete", 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    showAlert (std::move (alert), [safeThis = juce::Component::SafePointer<PresetBar> (this), name] (int result, juce::AlertWindow&)
    {
        if (safeThis == nullptr || result != 1)
            return;

        if (! safeThis->m_manager.deleteUserPreset (name))
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

#include "PluginEditor.h"
#include "dsp/KnobLaws.h"

#include "AboutWindow.h"
#include "Params.h"

namespace
{
    // Design-size layout (everything is scaled uniformly from here).
    constexpr int c_margin = 16;
    constexpr int c_headerHeight = 56;
    constexpr int c_stripHeight = 336;
    constexpr int c_sectionGap = 12;
    constexpr float c_viewRadius = 8.0f;
    constexpr float c_minScale = 0.5f, c_maxScale = 1.25f, c_defaultScale = 0.8f;

    const juce::Identifier c_viewEditorScale { "viewEditorScale" };

    const juce::String c_title = "ROARSCHACH";
    const juce::String c_taglineText = juce::String(Theme::tagline) + "  /";

    // Preset menu item IDs: factory presets are 1..getNumFactory() (they lead the preset list in a
    // fixed order, so their index is stable), user presets count up from c_firstUserItemId.
    constexpr int c_saveAsItemId = 1001, c_deleteItemId = 1002, c_revealItemId = 1003;
    constexpr int c_firstUserItemId = 100000;
}

//==============================================================================
RoarschachAudioProcessorEditor::RoarschachAudioProcessorEditor(RoarschachAudioProcessor& audioProcessor)
    : AudioProcessorEditor(&audioProcessor)
    , m_audioProcessor(audioProcessor)
    , m_lookAndFeel(*m_fonts)
    , m_monsterView(audioProcessor.getMonsterFeatures(), &audioProcessor.getCreatureShared())
    , m_strip(audioProcessor.getValueTreeState(), audioProcessor.getMonsterFeatures(), *m_fonts, &audioProcessor.getPositionWrites())
{
    setLookAndFeel(&m_lookAndFeel);
    m_tooltipWindow.setLookAndFeel(&m_lookAndFeel);
    RoarschachLookAndFeel::showFocusRingsAfterTab(*this); // keyboard focus rings after Tab, hidden by a click

    m_content.onPaint = [this](juce::Graphics& g) { paintContent(g); };
    m_content.onResized = [this] { layoutContent(); };
    addAndMakeVisible(m_content);

    m_monsterView.setCorner(c_viewRadius, Theme::Colours::background);
    m_monsterView.isCreatureFrozen = [&audioProcessor] { return audioProcessor.isCreatureFrozen(); };
    m_monsterView.setCreatureFrozen = [&audioProcessor](bool frozen) { audioProcessor.setCreatureFrozen(frozen); };
    m_monsterView.resetCreature = [&audioProcessor] { audioProcessor.resetCreature(); };
    updateMonsterParams();
    m_content.addAndMakeVisible(m_monsterView);
    m_content.addAndMakeVisible(m_strip);

    m_aboutButton.setButtonText("?");
    m_aboutButton.setTooltip("About Roarschach");
    m_aboutButton.onClick = [this] { showAboutWindow(); };
    m_content.addAndMakeVisible(m_aboutButton);

    setupPresetBar();

    m_websiteLink.setFont(m_fonts->label(10.5f), false, juce::Justification::centredLeft);
    m_websiteLink.setColour(juce::HyperlinkButton::textColourId, Theme::Colours::textDim); // amber on hover (ThemeLink)
    m_websiteLink.setTooltip("Open sweetjusticesound.com");
    m_content.addAndMakeVisible(m_websiteLink);

    // Resizable, uniformly scaled, fixed aspect ratio; the size is part of the saved state.
    // Read the saved scale first: setting the limits below resizes the editor, which
    // would otherwise overwrite it. A damaged value (NaN, inf) must not give a 0 x 0 editor.
    const auto savedScale = static_cast<double>(m_audioProcessor.getViewValue(c_viewEditorScale, c_defaultScale));
    const float scale = std::isfinite(savedScale) ? static_cast<float>(juce::jlimit<double>(c_minScale, c_maxScale, savedScale))
                                                  : c_defaultScale;
    setResizable(true, true);
    setResizeLimits(juce::roundToInt(c_baseWidth * c_minScale), juce::roundToInt(c_baseHeight * c_minScale),
                    juce::roundToInt(c_baseWidth * c_maxScale), juce::roundToInt(c_baseHeight * c_maxScale));
    if (auto* sizeConstrainer = getConstrainer())
        sizeConstrainer->setFixedAspectRatio(static_cast<double>(c_baseWidth) / c_baseHeight);

    setSize(juce::roundToInt(c_baseWidth * scale), juce::roundToInt(c_baseHeight * scale));
    m_sizeRestored = true;

    startTimerHz(30);
}

RoarschachAudioProcessorEditor::~RoarschachAudioProcessorEditor()
{
    stopTimer();

    // Nothing the editor opened may outlive it: the About window (and its update check), any
    // popup menu (it would keep using m_lookAndFeel), alerts and the tooltip window.
    m_aboutWindow.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    m_alert.reset();
    m_tooltipWindow.hideTip();
    m_tooltipWindow.setLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

//==============================================================================
RoarschachAudioProcessorEditor::EditorTooltipWindow::EditorTooltipWindow(juce::Component& editor)
    : juce::TooltipWindow(nullptr, 700) // no parent: a desktop window of its own, above the GL view
    , m_editor(editor)
{
    setOpaque(false); // the LookAndFeel draws a rounded panel; the corners show what is behind the tip
}

juce::String RoarschachAudioProcessorEditor::EditorTooltipWindow::getTipFor(juce::Component& component)
{
    return m_editor.isParentOf(&component) ? juce::TooltipWindow::getTipFor(component) : juce::String();
}

float RoarschachAudioProcessorEditor::EditorTooltipWindow::getDesktopScaleFactor() const
{
    return juce::Component::getApproximateScaleFactorForComponent(&m_editor);
}

//==============================================================================
void RoarschachAudioProcessorEditor::resized()
{
    // Uniform scale of the design-size content to the editor's current size.
    const float scale = static_cast<float>(getWidth()) / static_cast<float>(c_baseWidth);
    m_content.setBounds(0, 0, c_baseWidth, c_baseHeight);
    m_content.setTransform(juce::AffineTransform::scale(scale));

    if (m_sizeRestored)
        m_audioProcessor.setViewValue(c_viewEditorScale, scale);
}

void RoarschachAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(Theme::Colours::background);
}

void RoarschachAudioProcessorEditor::layoutContent()
{
    auto bounds = m_content.getLocalBounds();

    auto header = bounds.removeFromTop(c_headerHeight).reduced(c_margin, 0);
    m_aboutButton.setBounds(header.removeFromRight(26).withSizeKeepingCentre(26, 26));

    // Website link sits after the title and tagline (widths match paintContent()).
    {
        const float titleWidth = juce::GlyphArrangement::getStringWidth(m_fonts->heading(20.0f), c_title);
        const float taglineWidth = juce::GlyphArrangement::getStringWidth(m_fonts->label(10.5f), c_taglineText);
        const int linkX = c_margin + 30 + 12 + juce::roundToInt(titleWidth) + 4 + 14 + juce::roundToInt(taglineWidth) + 8;
        const int linkWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(m_fonts->label(10.5f), "SWEETJUSTICESOUND.COM")) + 6;
        m_websiteLink.setBounds(linkX, 2 + (c_headerHeight - 20) / 2, linkWidth, 20);
    }

    // Preset bar, right-aligned next to the About button.
    header.removeFromRight(14);
    auto presetRow = header.removeFromRight(400).withSizeKeepingCentre(400, 26);
    m_presetSaveButton.setBounds(presetRow.removeFromRight(64));
    presetRow.removeFromRight(8);
    m_presetNextButton.setBounds(presetRow.removeFromRight(26));
    presetRow.removeFromRight(4);
    m_presetPrevButton.setBounds(presetRow.removeFromLeft(26));
    presetRow.removeFromLeft(4);
    m_presetNameButton.setBounds(presetRow);

    bounds.reduce(c_margin, 0);
    bounds.removeFromBottom(c_margin);

    m_strip.setBounds(bounds.removeFromBottom(c_stripHeight));
    bounds.removeFromBottom(c_sectionGap);
    m_monsterView.setBounds(bounds);
}

void RoarschachAudioProcessorEditor::paintContent(juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll(background);

    // Header: the mark, the name and the tagline (the website link and the preset bar are components).
    auto header = m_content.getLocalBounds().removeFromTop(c_headerHeight).reduced(c_margin, 0).toFloat();

    RoarschachLookAndFeel::drawMark(g, header.removeFromLeft(30.0f).withSizeKeepingCentre(30.0f, 22.0f));
    header.removeFromLeft(12.0f);

    const auto titleFont = m_fonts->heading(20.0f);
    g.setFont(titleFont);
    g.setColour(text);
    const float titleWidth = juce::GlyphArrangement::getStringWidth(titleFont, c_title);
    g.drawText(c_title, header.removeFromLeft(titleWidth + 4.0f), juce::Justification::centredLeft, false);

    header.removeFromLeft(14.0f);
    g.setFont(m_fonts->label(10.5f));
    g.setColour(textFaint);
    g.drawText(c_taglineText, header.withTrimmedTop(4.0f), juce::Justification::centredLeft, false);
}

void RoarschachAudioProcessorEditor::timerCallback()
{
    updatePresetDisplay();
    updateMonsterParams();
}

void RoarschachAudioProcessorEditor::updateMonsterParams()
{
    auto& parameters = m_audioProcessor.getValueTreeState();
    const auto value = [&parameters](const char* id)
    {
        const auto* p = parameters.getRawParameterValue(id);
        const float v = p != nullptr ? p->load(std::memory_order_relaxed) : Params::defaultOf(id);
        return std::isfinite(v) ? v : Params::defaultOf(id);
    };
    const auto on = [&value](const char* id) { return value(id) >= 0.5f ? 1.0f : 0.0f; };
    const auto percent = [&value](const char* id) { return juce::jlimit(0.0f, 1.0f, value(id) / 100.0f); };

    // The creature reads the knobs of the modules that are On (an Off module reads 0), as the
    // effect they have (roar::laws::view), so it follows the sound rather than the knob travel.
    namespace view = roar::laws::view;
    const auto effect = [&percent](double (*law)(double), const char* id) { return static_cast<float>(law(static_cast<double>(percent(id)))); };
    MonsterView::Params params;
    params.pitchSt = on(Params::pitchOn) * juce::jlimit(-24.0f, 12.0f, value(Params::pitch));
    params.size = on(Params::pitchOn) * percent(Params::pitchSize);
    params.growl = on(Params::growlOn) * effect(view::growl, Params::growl);
    params.liquid = on(Params::shimmerOn) * effect(view::shimmer, Params::shimmer);   // skin ripple and drips
    params.swirl = on(Params::swirlOn) * effect(view::swirl, Params::swirl);
    params.ensemble = on(Params::ensembleOn) * effect(view::ensemble, Params::ensemble);
    params.overtone = on(Params::overtoneOn) * effect(view::overtone, Params::overtone);
    params.ring = on(Params::ringOn) * percent(Params::ring);
    params.cleanOn = on(Params::cleanOn);
    params.roar = on(Params::roarOn) * effect(view::roar, Params::roar);
    params.sub = on(Params::subOn) * effect(view::sub, Params::sub);
    params.gravel = on(Params::vocoderOn) * effect(view::gravel, Params::gravel);
    params.detailLow = on(Params::detailOn) * effect(view::detail, Params::detailLow);
    params.detailHigh = on(Params::detailOn) * effect(view::detail, Params::detailHigh);
    m_monsterView.setParams(params);
}

void RoarschachAudioProcessorEditor::showAboutWindow()
{
    // One About window per editor: another click brings it back to the front. The editor owns it,
    // so it is closed (and its update check stopped) when the editor goes.
    if (m_aboutWindow != nullptr)
    {
        m_aboutWindow->setMinimised(false);
        m_aboutWindow->toFront(true);
        return;
    }

    m_aboutWindow = std::make_unique<AboutWindow>(m_lookAndFeel, [this] { m_aboutWindow.reset(); });
}

//==============================================================================
// Presets
//==============================================================================
void RoarschachAudioProcessorEditor::setupPresetBar()
{
    auto& presets = m_audioProcessor.getPresetManager();

    m_presetPrevButton.setButtonText("<");
    m_presetPrevButton.setTooltip("Previous preset");
    m_presetPrevButton.onClick = [this] { m_audioProcessor.getPresetManager().loadPrevious(); updatePresetDisplay(); };

    m_presetNextButton.setButtonText(">");
    m_presetNextButton.setTooltip("Next preset");
    m_presetNextButton.onClick = [this] { m_audioProcessor.getPresetManager().loadNext(); updatePresetDisplay(); };

    m_presetNameButton.setTooltip("Choose a preset, or save, delete and find your presets");
    m_presetNameButton.onClick = [this] { showPresetMenu(); };

    m_presetSaveButton.setButtonText("SAVE");
    m_presetSaveButton.setTooltip("Save the current settings as a preset");
    m_presetSaveButton.onClick = [this]
    {
        auto& pm = m_audioProcessor.getPresetManager();
        pm.refresh(); // a preset renamed or removed outside the plugin is not written back blindly
        const auto current = pm.getSnapshot();

        // Editing a user preset that is still in the folder: Save writes back to its file.
        // Otherwise ask for a name (and before replacing anything).
        if (current.currentIsUser && current.currentIndex >= 0)
            savePresetAs(current.currentName);
        else
            promptSavePreset(current.currentName == "Init" ? juce::String() : current.currentName);
    };

    for (auto* b : { &m_presetPrevButton, &m_presetNameButton, &m_presetNextButton, &m_presetSaveButton })
        m_content.addAndMakeVisible(b);

    presets.refresh();
    updatePresetDisplay();
}

void RoarschachAudioProcessorEditor::updatePresetDisplay()
{
    const auto& pm = m_audioProcessor.getPresetManager();
    const auto text = pm.getCurrentName() + (pm.isModified() ? " *" : "");

    if (m_presetNameButton.getButtonText() != text)
        m_presetNameButton.setButtonText(text);
}

void RoarschachAudioProcessorEditor::showPresetMenu()
{
    auto& pm = m_audioProcessor.getPresetManager();
    pm.refresh(); // pick up presets added or removed outside the plugin

    // One consistent view of the list and the current preset (a host may change either meanwhile).
    const auto snapshot = pm.getSnapshot();
    const auto& presets = snapshot.presets;
    const auto isCurrentFactory = [&snapshot](int i) { return snapshot.currentIsFactory && i == snapshot.currentIndex; };

    juce::PopupMenu menu;
    menu.setLookAndFeel(&m_lookAndFeel); // submenus take it from their parent menu
    menu.addSectionHeader("FACTORY");

    // Top-level factory presets (Init), then one submenu per category, in a fixed order.
    for (int i = 0; i < presets.size(); ++i)
        if (presets[i].isFactory && presets[i].category.isEmpty())
            menu.addItem(i + 1, presets[i].name, true, isCurrentFactory(i));

    // The categories in the order the factory list first names them (every category gets its submenu).
    juce::StringArray categories;
    for (const auto& preset : presets)
        if (preset.isFactory && preset.category.isNotEmpty())
            categories.addIfNotAlreadyThere(preset.category);

    for (const auto& category : categories)
    {
        juce::PopupMenu sub;
        bool containsCurrent = false;

        for (int i = 0; i < presets.size(); ++i)
        {
            if (presets[i].isFactory && presets[i].category == category)
            {
                sub.addItem(i + 1, presets[i].name, true, isCurrentFactory(i));
                containsCurrent = containsCurrent || isCurrentFactory(i);
            }
        }

        if (sub.getNumItems() > 0)
            menu.addSubMenu(category, sub, true, nullptr, containsCurrent);
    }

    // User presets are told apart by their file: two can carry the same name, in which case
    // menuName adds the file name. Items load (and Delete removes) exactly that file.
    juce::Array<juce::File> userFiles;
    juce::PopupMenu user;
    bool userContainsCurrent = false;
    juce::String currentMenuName = snapshot.currentName;

    for (const auto& preset : presets)
    {
        if (preset.isFactory)
            continue;

        const bool isCurrent = snapshot.currentIsUser && preset.file == snapshot.currentFile;
        user.addItem(c_firstUserItemId + userFiles.size(), preset.menuName, true, isCurrent);
        userFiles.add(preset.file);

        if (isCurrent)
        {
            userContainsCurrent = true;
            currentMenuName = preset.menuName;
        }
    }

    menu.addSectionHeader("USER");
    if (!userFiles.isEmpty())
        menu.addSubMenu("User Presets", user, true, nullptr, userContainsCurrent); // keeps long lists tidy
    else
        menu.addItem(-1, "(none saved yet)", false);

    // Delete is bound to the preset named here, even if another one is current by the time it is confirmed.
    const auto fileToDelete = snapshot.currentIsUser ? snapshot.currentFile : juce::File();

    menu.addSeparator();
    menu.addItem(c_saveAsItemId, "Save As...");
    menu.addItem(c_deleteItemId, "Delete \"" + currentMenuName + "\"", fileToDelete != juce::File());
    menu.addItem(c_revealItemId, "Show Presets Folder");

    menu.showMenuAsync(RoarschachLookAndFeel::menuOptionsFor(m_presetNameButton, m_presetNameButton.getWidth()),
                       [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this), userFiles, fileToDelete,
                        nameToDelete = currentMenuName](int result)
    {
        if (safe == nullptr || result == 0)
            return;

        auto& presetManager = safe->m_audioProcessor.getPresetManager();

        if (result == c_saveAsItemId)
        {
            safe->promptSavePreset(presetManager.isCurrentUserPreset() ? presetManager.getCurrentName() : juce::String());
        }
        else if (result == c_deleteItemId)
        {
            safe->confirmDeletePreset(fileToDelete, nameToDelete);
        }
        else if (result == c_revealItemId)
        {
            const auto dir = PresetManager::getUserPresetDirectory();
            dir.createDirectory();
            dir.startAsProcess(); // opens the folder in Finder / Explorer
        }
        else if (result >= c_firstUserItemId)
        {
            if (const int i = result - c_firstUserItemId; i < userFiles.size())
                presetManager.loadUserPreset(userFiles[i]);

            safe->updatePresetDisplay();
        }
        else if (result >= 1 && result <= PresetManager::getNumFactory())
        {
            presetManager.loadPreset(result - 1); // factory presets lead the list, in a fixed order
            safe->updatePresetDisplay();
        }
    });
}

void RoarschachAudioProcessorEditor::confirmDeletePreset(const juce::File& file, const juce::String& displayName)
{
    auto alert = makeAlert("Delete Preset", "Delete \"" + displayName + "\"? This removes the preset file.",
                           juce::MessageBoxIconType::WarningIcon);
    alert->addButton("Delete", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    showAlert(std::move(alert), [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this), file](int result, juce::AlertWindow&)
    {
        if (safe == nullptr || result != 1)
            return;

        // Exactly the file the dialog named, whatever the host has recalled since. Nothing
        // happens if it has gone (deleteUserPreset() returns false).
        safe->m_audioProcessor.getPresetManager().deleteUserPreset(file);
        safe->updatePresetDisplay();
    });
}

void RoarschachAudioProcessorEditor::promptSavePreset(const juce::String& suggestedName)
{
    auto alert = makeAlert("Save Preset", "Name this preset:", juce::MessageBoxIconType::NoIcon);
    alert->addTextEditor("name", suggestedName);
    if (auto* field = alert->getTextEditor("name"))
    {
        field->setJustification(juce::Justification::centredLeft); // vertically centred in the field
        field->setIndents(8, 0);
    }
    alert->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    showAlert(std::move(alert), [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this)](int result, juce::AlertWindow& window)
    {
        if (safe == nullptr || result != 1)
            return;

        const auto name = window.getTextEditorContents("name").trim(); // read before another alert replaces this one
        if (name.isNotEmpty())
            safe->checkNameAndSave(name);
    });
}

void RoarschachAudioProcessorEditor::checkNameAndSave(const juce::String& name)
{
    auto& pm = m_audioProcessor.getPresetManager();

    if (!PresetManager::isValidPresetName(name))
    {
        // E.g. "???": nothing is left once the characters a file name can't hold are dropped.
        auto alert = makeAlert("Can't Use This Name",
                               "\"" + name + "\" can't be used as a preset name, as it has no characters "
                               "a file name can hold. Please use letters or numbers.",
                               juce::MessageBoxIconType::WarningIcon);
        alert->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        showAlert(std::move(alert), [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this), name](int result, juce::AlertWindow&)
        {
            if (safe != nullptr && result == 1)
                safe->promptSavePreset(name); // try again, with the name as typed
        });
        return;
    }

    // Ask before replacing ANY existing file: a preset with this name, or a file this name maps to
    // (file names ignore case, and characters such as '/' are dropped, so "AB" replaces "A/B").
    if (pm.userPresetExists(name))
        confirmOverwrite(name, pm.getNameOfPresetSavedAs(name));
    else
        savePresetAs(name);
}

void RoarschachAudioProcessorEditor::confirmOverwrite(const juce::String& name, const juce::String& replacedName)
{
    const auto message = replacedName.isEmpty() || replacedName == name
                       ? "A preset called \"" + name + "\" already exists. Replace it?"
                       : "Saving as \"" + name + "\" replaces the preset \"" + replacedName + "\" (the two names give the same "
                         "file name). Replace \"" + replacedName + "\"?";

    auto alert = makeAlert("Replace Preset", message, juce::MessageBoxIconType::QuestionIcon);
    alert->addButton("Replace", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    showAlert(std::move(alert), [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this), name, replacedName](int result, juce::AlertWindow&)
    {
        if (safe == nullptr || result != 1)
            return;

        // The folder may have changed while the question was up: ask again if the save would now
        // replace a different preset.
        const auto nowReplaced = safe->m_audioProcessor.getPresetManager().getNameOfPresetSavedAs(name);
        if (nowReplaced.isNotEmpty() && nowReplaced != replacedName)
            safe->confirmOverwrite(name, nowReplaced);
        else
            safe->savePresetAs(name);
    });
}

void RoarschachAudioProcessorEditor::savePresetAs(const juce::String& name)
{
    if (!m_audioProcessor.getPresetManager().saveUserPreset(name))
    {
        auto alert = makeAlert("Couldn't Save Preset",
                               "The preset could not be written to:\n" + PresetManager::getUserPresetDirectory().getFullPathName(),
                               juce::MessageBoxIconType::WarningIcon);
        alert->addButton("OK", 0, juce::KeyPress(juce::KeyPress::returnKey));
        showAlert(std::move(alert), {});
    }

    updatePresetDisplay();
}

std::unique_ptr<juce::AlertWindow> RoarschachAudioProcessorEditor::makeAlert(const juce::String& title, const juce::String& message,
                                                                            juce::MessageBoxIconType icon)
{
    // Centred over the plugin window and drawn at the editor's own scale (not the design-size content's).
    auto alert = std::make_unique<juce::AlertWindow>(title, message, icon, this);

    // Before addButton() / addTextEditor(): they take button sizes and the field's font and outline from the
    // alert's LookAndFeel as they are called.
    alert->setLookAndFeel(&m_lookAndFeel);
    alert->setOpaque(false); // a rounded panel: the corners show what is behind the dialog
    RoarschachLookAndFeel::showFocusRingsAfterTab(*alert);
    return alert;
}

void RoarschachAudioProcessorEditor::showAlert(std::unique_ptr<juce::AlertWindow> alert,
                                               std::function<void(int, juce::AlertWindow&)> onResult)
{
    // Plugins can't run modal loops, so alerts are async. The editor owns the
    // window so it is always destroyed before the LookAndFeel it uses.
    m_alert = std::move(alert);
    m_alert->setLookAndFeel(&m_lookAndFeel); // already set by makeAlert(); kept for any alert made elsewhere

    m_alert->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this), onResult](int result)
        {
            if (safe == nullptr || safe->m_alert == nullptr)
                return;

            if (onResult)
                onResult(result, *safe->m_alert);

            // Defer the delete: we're inside the window's own callback.
            juce::MessageManager::callAsync([safe]
            {
                if (safe != nullptr && safe->m_alert != nullptr && !safe->m_alert->isCurrentlyModal())
                    safe->m_alert.reset();
            });
        }), false);
}

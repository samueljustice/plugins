#include "PresetManager.h"
#include "PluginProcessor.h"

PresetManager::PresetManager(StretchArmstrongAudioProcessor& p)
    : processor(p)
{
    presetSelector.setTextWhenNothingSelected("Select Preset...");
    presetSelector.onChange = [this]
    {
        auto selectedIndex = presetSelector.getSelectedId();
        if (selectedIndex >= 100)
        {
            auto presetName = presetSelector.getText();
            auto presetsDir = getPresetsDirectory();

            juce::File presetFile;
            if (selectedIndex < 200)
            {
                presetFile = presetsDir.getChildFile("Factory").getChildFile(presetName + ".xml");
            }
            else
            {
                presetFile = presetsDir.getChildFile("User").getChildFile(presetName + ".xml");
            }

            if (presetFile.existsAsFile())
            {
                loadPresetFromFile(presetFile);
            }

            deleteButton.setEnabled(selectedIndex >= 200);
        }
        else
        {
            deleteButton.setEnabled(false);
        }
    };
    addAndMakeVisible(presetSelector);

    saveButton.onClick = [this] { savePreset(); };
    addAndMakeVisible(saveButton);

    deleteButton.onClick = [this] { deletePreset(); };
    deleteButton.setEnabled(false);
    addAndMakeVisible(deleteButton);

    resetAllButton.onClick = [this] { resetToDefaults(); };
    resetAllButton.setTooltip("Reset all parameters to default values");
    addAndMakeVisible(resetAllButton);

    loadFactoryPresets();
    refreshPresetList();
}

void PresetManager::resized()
{
    auto area = getLocalBounds();

    presetSelector.setBounds(area.removeFromLeft(static_cast<int>(area.getWidth() * 0.45f)));

    resetAllButton.setBounds(area.removeFromRight(70).reduced(1));
    deleteButton.setBounds(area.removeFromRight(55).reduced(1));
    saveButton.setBounds(area.removeFromRight(70).reduced(1));
}

void PresetManager::savePreset()
{
    auto* alertWindow = new juce::AlertWindow("Save Preset",
                                               "Enter a name for your preset:",
                                               juce::AlertWindow::NoIcon);

    alertWindow->addTextEditor("presetName", "My Preset");
    alertWindow->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alertWindow->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    alertWindow->enterModalState(true, juce::ModalCallbackFunction::create([this, alertWindow](int result)
    {
        if (result == 1)
        {
            auto presetName = alertWindow->getTextEditorContents("presetName");
            if (presetName.isNotEmpty())
            {
                auto userDir = getPresetsDirectory().getChildFile("User");

                if (!userDir.exists() && !userDir.createDirectory())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Preset Save Error",
                        "Unable to create user presets folder.");
                    return;
                }

                auto presetFile = userDir.getChildFile(presetName + ".xml");

                if (presetFile.exists())
                {
                    auto overwrite = juce::NativeMessageBox::showOkCancelBox(
                        juce::AlertWindow::WarningIcon,
                        "Overwrite Preset?",
                        "A preset named '" + presetName + "' already exists. Overwrite it?",
                        nullptr,
                        nullptr);

                    if (!overwrite)
                        return;
                }

                savePresetToFile(presetFile);
                refreshPresetList();

                for (int i = 0; i < presetSelector.getNumItems(); ++i)
                {
                    if (presetSelector.getItemText(i) == presetName)
                    {
                        presetSelector.setSelectedItemIndex(i);
                        break;
                    }
                }
            }
        }
        delete alertWindow;
    }));
}

void PresetManager::deletePreset()
{
    auto selectedId = presetSelector.getSelectedId();
    if (selectedId >= 200)
    {
        auto presetName = presetSelector.getText();

        auto result = juce::NativeMessageBox::showOkCancelBox(
            juce::AlertWindow::QuestionIcon,
            "Delete Preset",
            "Are you sure you want to delete '" + presetName + "'?",
            nullptr,
            nullptr);

        if (result)
        {
            auto userDir = getPresetsDirectory().getChildFile("User");
            auto presetFile = userDir.getChildFile(presetName + ".xml");

            if (presetFile.deleteFile())
            {
                refreshPresetList();
                presetSelector.setSelectedId(1);
            }
        }
    }
}

void PresetManager::loadPresetFromFile(const juce::File& file)
{
    juce::XmlDocument doc(file);
    auto xml = doc.getDocumentElement();

    if (xml == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Preset Load Error",
            "Failed to load preset: " + doc.getLastParseError());
        return;
    }

    if (!xml->hasTagName(processor.parameters.state.getType()))
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Preset Load Error",
            "Invalid preset file format.");
        return;
    }

    try
    {
        processor.parameters.replaceState(juce::ValueTree::fromXml(*xml));
    }
    catch (...)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Preset Load Error",
            "Failed to apply preset.");
    }
}

void PresetManager::savePresetToFile(const juce::File& file)
{
    auto state = processor.parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());

    if (!xml->writeTo(file))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
            "Preset Save Error", "Failed to save preset file!");
    }
}

void PresetManager::resetToDefaults()
{
    auto& params = processor.parameters;

    for (auto* param : params.processor.getParameters())
    {
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(param))
        {
            p->setValueNotifyingHost(p->getDefaultValue());
        }
    }
}

juce::File PresetManager::getPresetsDirectory()
{
    juce::File documentsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    juce::File presetsDir = documentsDir.getChildFile("StretchArmstrong Presets");

    if (!presetsDir.exists())
    {
        if (!presetsDir.createDirectory())
        {
            #if JUCE_MAC
                presetsDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("Application Support")
                    .getChildFile("StretchArmstrong")
                    .getChildFile("Presets");
            #elif JUCE_WINDOWS
                presetsDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("StretchArmstrong")
                    .getChildFile("Presets");
            #else
                presetsDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile(".StretchArmstrong")
                    .getChildFile("Presets");
            #endif

            if (!presetsDir.exists() && !presetsDir.createDirectory())
            {
                static bool hasShownError = false;
                if (!hasShownError)
                {
                    hasShownError = true;
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Preset Folder Error",
                        "Unable to create preset folder.");
                }
                return juce::File::getSpecialLocation(juce::File::tempDirectory);
            }
        }

        presetsDir.getChildFile("Factory").createDirectory();
        presetsDir.getChildFile("User").createDirectory();
    }

    return presetsDir;
}

void PresetManager::refreshPresetList()
{
    presetSelector.clear();
    presetSelector.addItem("-- Select Preset --", 1);
    presetSelector.addSeparator();

    auto presetsDir = getPresetsDirectory();

    auto factoryDir = presetsDir.getChildFile("Factory");
    auto factoryPresets = factoryDir.findChildFiles(juce::File::findFiles, false, "*.xml");

    if (factoryPresets.size() > 0)
    {
        presetSelector.addSectionHeading("Factory Presets");
        int id = 100;
        for (const auto& file : factoryPresets)
        {
            presetSelector.addItem(file.getFileNameWithoutExtension(), id++);
        }
    }

    auto userDir = presetsDir.getChildFile("User");
    auto userPresets = userDir.findChildFiles(juce::File::findFiles, false, "*.xml");

    if (userPresets.size() > 0)
    {
        presetSelector.addSeparator();
        presetSelector.addSectionHeading("User Presets");
        int id = 200;
        for (const auto& file : userPresets)
        {
            presetSelector.addItem(file.getFileNameWithoutExtension(), id++);
        }
    }
}

void PresetManager::loadFactoryPresets()
{
    auto factoryDir = getPresetsDirectory().getChildFile("Factory");

    if (factoryDir.getChildFile("Default.xml").existsAsFile())
        return;

    resetToDefaults();

    auto presetFile = factoryDir.getChildFile("Default.xml");
    savePresetToFile(presetFile);

    resetToDefaults();
}

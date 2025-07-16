#include "PluginEditor.h"
#include "PluginProcessor.h"

PresetManager::PresetManager(PitchFlattenerAudioProcessor& p)
    : processor(p)
{
    // Set up preset selector
    presetSelector.setTextWhenNothingSelected("Select Preset...");
    presetSelector.onChange = [this]
    {
        auto selectedIndex = presetSelector.getSelectedId();
        if (selectedIndex >= 100)
        {
            auto presetName = presetSelector.getText();
            auto presetsDir = getPresetsDirectory();
            
            juce::File presetFile;
            if (selectedIndex < 200) // Factory preset
            {
                presetFile = presetsDir.getChildFile("Factory").getChildFile(presetName + ".xml");
            }
            else // User preset
            {
                presetFile = presetsDir.getChildFile("User").getChildFile(presetName + ".xml");
            }
            
            if (presetFile.existsAsFile())
            {
                loadPresetFromFile(presetFile);
            }
            
            // Enable/disable delete button based on selection
            deleteButton.setEnabled(selectedIndex >= 200); // Only user presets can be deleted
        }
        else
        {
            deleteButton.setEnabled(false);
        }
    };
    addAndMakeVisible(presetSelector);
    
    // Set up buttons
    saveButton.onClick = [this] { savePreset(); };
    addAndMakeVisible(saveButton);
    
    deleteButton.onClick = [this] { deletePreset(); };
    deleteButton.setEnabled(false); // Disabled until a user preset is selected
    addAndMakeVisible(deleteButton);
    
    resetAllButton.onClick = [this] { resetToDefaults(); };
    resetAllButton.setTooltip("Reset all parameters to default values");
    addAndMakeVisible(resetAllButton);
    
    // Load factory presets and refresh list
    loadFactoryPresets();
    refreshPresetList();
}

void PresetManager::resized()
{
    auto area = getLocalBounds();
    
    // Preset selector takes appropriate width
    presetSelector.setBounds(area.removeFromLeft(static_cast<int>(area.getWidth() * 0.45f)));
    
    // Buttons on the right - more compact
    resetAllButton.setBounds(area.removeFromRight(70).reduced(1));
    deleteButton.setBounds(area.removeFromRight(55).reduced(1));
    saveButton.setBounds(area.removeFromRight(70).reduced(1));
}

void PresetManager::savePreset()
{
    // Use an AlertWindow to get preset name
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
                
                // Ensure user directory exists
                if (!userDir.exists() && !userDir.createDirectory())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Preset Save Error",
                        "Unable to create user presets folder. Please check permissions.");
                    return;
                }
                
                auto presetFile = userDir.getChildFile(presetName + ".xml");
                
                // Check if file already exists
                if (presetFile.exists())
                {
                    auto result = juce::NativeMessageBox::showOkCancelBox(
                        juce::AlertWindow::WarningIcon,
                        "Overwrite Preset?",
                        "A preset named '" + presetName + "' already exists. Overwrite it?",
                        nullptr,
                        nullptr);
                    
                    if (!result)
                        return;
                }
                
                savePresetToFile(presetFile);
                refreshPresetList();
                
                // Select the newly saved preset
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
    if (selectedId >= 200) // Only user presets can be deleted
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
                presetSelector.setSelectedId(1); // Select "-- Select Preset --"
            }
        }
    }
}

void PresetManager::loadPresetFromFile(const juce::File& file)
{
    // Add error handling for malformed XML
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
            "Invalid preset file format - this doesn't appear to be a PitchFlattener preset.");
        return;
    }
    
    // Try to load the preset
    try
    {
        processor.parameters.replaceState(juce::ValueTree::fromXml(*xml));
        
        // Force update of UI controls after loading preset
        if (auto* editor = dynamic_cast<PitchFlattenerAudioProcessorEditor*>(processor.getActiveEditor()))
        {
            editor->updateAlgorithmControls();
        }
    }
    catch (...)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Preset Load Error",
            "Failed to apply preset - the file may be corrupted.");
    }
}

void PresetManager::savePresetToFile(const juce::File& file)
{
    // Save as XML for better readability and editability
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
    
    // Also reset the latched base pitch
    processor.resetLatchedBasePitch();
}

juce::File PresetManager::getPresetsDirectory()
{
    // Use Documents folder for better user accessibility
    juce::File documentsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    juce::File presetsDir = documentsDir.getChildFile("PitchFlattener Presets");
    
    if (!presetsDir.exists())
    {
        // Try to create the directory with error handling
        if (!presetsDir.createDirectory())
        {
            // If we can't create in Documents, fall back to app data directory
            #if JUCE_MAC
                presetsDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("Application Support")
                    .getChildFile("PitchFlattener")
                    .getChildFile("Presets");
            #elif JUCE_WINDOWS
                presetsDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("PitchFlattener")
                    .getChildFile("Presets");
            #else
                presetsDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile(".PitchFlattener")
                    .getChildFile("Presets");
            #endif
            
            // Try to create the fallback directory
            if (!presetsDir.exists() && !presetsDir.createDirectory())
            {
                // If both fail, show error once and return temp directory
                static bool hasShownError = false;
                if (!hasShownError)
                {
                    hasShownError = true;
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Preset Folder Error",
                        "Unable to create preset folder. Presets will be temporarily stored but may not persist.");
                }
                return juce::File::getSpecialLocation(juce::File::tempDirectory);
            }
        }
        
        // Create subdirectories for organization (with error handling)
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
    
    // Add factory presets
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
    
    // Add user presets
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
    
    // Check if default preset already exists
    if (factoryDir.getChildFile("Default.xml").existsAsFile())
        return;
    
    // Create only the default preset
    resetToDefaults();
    
    // Save to file in Factory folder
    auto presetFile = factoryDir.getChildFile("Default.xml");
    savePresetToFile(presetFile);
    
    // Reset to defaults after creating preset
    resetToDefaults();
}
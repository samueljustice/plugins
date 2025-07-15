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
    
    // Check if factory presets already exist
    if (factoryDir.getChildFile("Subtle Flattening.xml").existsAsFile())
        return;
    
    // Create factory presets
    struct FactoryPreset
    {
        juce::String name;
        std::vector<std::pair<juce::String, float>> values;
    };
    
    std::vector<FactoryPreset> factoryPresets =
    {
        {
            "Default",
            {} // Empty - will use all default values
        },
        {
            "Subtle Flattening",
            {
                {"targetPitch", 1200.0f},
                {"smoothingTimeMs", 150.0f},
                {"mix", 0.5f},
                {"pitchSmoothing", 0.85f},
                {"minConfidence", 0.4f}
            }
        },
        {
            "Aggressive Flattening",
            {
                {"targetPitch", 1200.0f},
                {"smoothingTimeMs", 50.0f},
                {"mix", 1.0f},
                {"pitchSmoothing", 0.5f},
                {"minConfidence", 0.2f},
                {"pitchHoldTime", 100.0f}
            }
        },
        {
            "Pitch Sweep Tamer",
            {
                {"targetPitch", 1200.0f},
                {"smoothingTimeMs", 200.0f},
                {"mix", 0.75f},
                {"pitchJumpThreshold", 150.0f},
                {"pitchHoldTime", 800.0f},
                {"pitchSmoothing", 0.9f}
            }
        },
        {
            "Fast Tracking",
            {
                {"targetPitch", 1200.0f},
                {"smoothingTimeMs", 20.0f},
                {"mix", 1.0f},
                {"detectionRate", 64.0f},
                {"pitchSmoothing", 0.3f},
                {"minConfidence", 0.15f}
            }
        },
        {
            "Vocoder Helper",
            {
                {"targetPitch", 800.0f},
                {"smoothingTimeMs", 100.0f},
                {"mix", 0.7f},
                {"minFreq", 400.0f},
                {"maxFreq", 1600.0f},
                {"detectionHighpass", 400.0f},
                {"detectionLowpass", 1600.0f},
                {"pitchAlgorithm", 0.0f}  // YIN algorithm
            }
        },
        {
            "DIO FFT Mode",
            {
                {"targetPitch", 1200.0f},
                {"smoothingTimeMs", 100.0f},
                {"mix", 1.0f},
                {"pitchAlgorithm", 1.0f},  // WORLD DIO algorithm
                {"dioSpeed", 1.0f},
                {"dioFramePeriod", 2.0f}
            }
        }
    };
    
    // Save each factory preset
    for (const auto& preset : factoryPresets)
    {
        // First reset to defaults
        resetToDefaults();
        
        // Apply preset values
        auto& params = processor.parameters;
        for (const auto& [paramID, value] : preset.values)
        {
            if (auto* param = params.getParameter(paramID))
            {
                auto range = param->getNormalisableRange();
                auto normalizedValue = range.convertTo0to1(value);
                param->setValueNotifyingHost(normalizedValue);
            }
        }
        
        // Save to file in Factory folder
        auto presetFile = factoryDir.getChildFile(preset.name + ".xml");
        savePresetToFile(presetFile);
    }
    
    // Reset to defaults after creating presets
    resetToDefaults();
}
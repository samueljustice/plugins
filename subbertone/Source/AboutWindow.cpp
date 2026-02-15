#include "AboutWindow.h"
#include <JuceHeader.h>

//-----------------------------------------------------------------
// AboutWindow
//-----------------------------------------------------------------
AboutWindow::AboutWindow()
    : DocumentWindow("About SammyJs Subbertone", juce::Colour(0xff0a0a0a), DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    setContentNonOwned(&m_content, false);
    
    centreWithSize(500, 650);
    setVisible(true);
    setResizable(false, false);
    setAlwaysOnTop(true);
    toFront(true);
}

void AboutWindow::closeButtonPressed()
{
    delete this;
}


//-----------------------------------------------------------------
// AboutContent
//-----------------------------------------------------------------
AboutWindow::AboutContent::AboutContent()
{
    m_websiteButton.setButtonText("samueljustice.com");
    m_websiteButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    m_websiteButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff00ffff));
    m_websiteButton.onClick = []
    {
        juce::URL("https://samueljustice.com").launchInDefaultBrowser();
    };
    addAndMakeVisible(m_websiteButton);
    
    m_emailButton.setButtonText("sam@samueljustice.com");
    m_emailButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    m_emailButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff00ffff));
    m_emailButton.onClick = []
    {
        juce::URL("mailto:sam@samueljustice.com").launchInDefaultBrowser();
    };
    addAndMakeVisible(m_emailButton);
    
    m_checkUpdatesButton.setButtonText("Check for Updates");
    m_checkUpdatesButton.onClick = [this] { checkForUpdates(); };
    addAndMakeVisible(m_checkUpdatesButton);
    
    m_updateStatusLabel.setText("", juce::dontSendNotification);
    m_updateStatusLabel.setJustificationType(juce::Justification::centred);
    m_updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    addAndMakeVisible(m_updateStatusLabel);
    
    m_licenseText.setMultiLine(true);
    m_licenseText.setReadOnly(true);
    m_licenseText.setScrollbarsShown(true);
    m_licenseText.setCaretVisible(false);
    m_licenseText.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff0f0f0f));
    m_licenseText.setColour(juce::TextEditor::textColourId, juce::Colour(0xffffffff));
    m_licenseText.setText(
        "MIT License\n\n"
        "Copyright (c) 2025 Samuel Justice\n\n"
        "Permission is hereby granted, free of charge, to any person obtaining a copy "
        "of this software and associated documentation files (the \"Software\"), to deal "
        "in the Software without restriction, including without limitation the rights "
        "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell "
        "copies of the Software, and to permit persons to whom the Software is "
        "furnished to do so, subject to the following conditions:\n\n"
        "The above copyright notice and this permission notice shall be included in all "
        "copies or substantial portions of the Software.\n\n"
        "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR "
        "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, "
        "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT."
    );
    addAndMakeVisible(m_licenseText);
}

void AboutWindow::AboutContent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0a0a0a));
    
    g.setColour(juce::Colour(0xff00ffff));
    g.setFont(juce::Font(juce::FontOptions("Courier New", 24.0f, juce::Font::bold)));
    g.drawText("SAMMYJS SUBBERTONE", 0, 20, getWidth(), 30, juce::Justification::centred);
    
    g.setColour(juce::Colour(0xffff00ff));
    g.setFont(juce::Font(juce::FontOptions(16.0f)));
    g.drawText(juce::String("Version ") + JucePlugin_VersionString, 0, 60, getWidth(), 20,
               juce::Justification::centred);
    
    g.setColour(juce::Colour(0xffffffff));
    g.setFont(juce::Font(juce::FontOptions(14.0f)));
    g.drawText("Subharmonic Generator Plugin", 0, 90, getWidth(), 20, juce::Justification::centred);
    
    g.drawText("Created by Samuel Justice", 0, 120, getWidth(), 20, juce::Justification::centred);
    
    g.setColour(juce::Colour(0xff1a3a3a));
    g.drawLine(20.0f, 280.0f, static_cast<float>(getWidth() - 20), 280.0f, 2.0f);
    
    g.setColour(juce::Colour(0xffffffff));
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawText("License Information:", 20, 290, getWidth() - 40, 20, juce::Justification::left);
}

void AboutWindow::AboutContent::resized()
{
    m_websiteButton.setBounds(150, 150, 200, 25);
    m_emailButton.setBounds(150, 180, 200, 25);
    m_checkUpdatesButton.setBounds(150, 215, 200, 30);
    m_updateStatusLabel.setBounds(50, 250, getWidth() - 100, 25);
    m_licenseText.setBounds(20, 320, getWidth() - 40, getHeight() - 340);
}

void AboutWindow::AboutContent::checkForUpdates()
{
    m_updateStatusLabel.setText("Checking for updates...", juce::dontSendNotification);
    m_updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);
    
    // Create URL for GitHub API to check latest release with subbertone tag
    const juce::URL apiUrl("https://api.github.com/repos/samueljustice/plugins/releases");
    
    juce::Component::SafePointer<AboutContent> safeThis(this);

    // Use a thread to download the data
    juce::Thread::launch([safeThis, apiUrl]
    {
        if (!safeThis)
            return;

        const std::unique_ptr<juce::InputStream> stream = apiUrl.createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress).withConnectionTimeoutMs(5000)
        );
        
        if (!stream)
        {
            juce::MessageManager::callAsync([safeThis]
            {
                if (!safeThis)
                    return;

                safeThis->m_updateStatusLabel.setText("Failed to check for updates", juce::dontSendNotification);
                safeThis->m_updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::red);
            });

            return;
        }
        
        const juce::String responseContent = stream->readEntireStreamAsString();
        const juce::var releases = juce::JSON::parse(responseContent);
        
        if (const juce::Array<juce::var>* releasesArray = releases.getArray())
        {
            juce::String latestVersion;
            
            // Look for releases with subbertone tag
            for (const juce::var& release : *releasesArray)
            {
                if (const juce::DynamicObject* obj = release.getDynamicObject())
                {
                    const juce::String tagName = obj->getProperty("tag_name").toString();
                    
                    // Check if this is a subbertone release
                    if (tagName.startsWith("subbertone-v"))
                    {
                        latestVersion = tagName.substring(12); // Skip "subbertone-v"
                        break; // Found the latest subbertone release
                    }
                }
            }
            
            if (latestVersion.isNotEmpty())
            {
                const juce::String currentVersion = juce::String(JucePlugin_VersionString);
                
                juce::MessageManager::callAsync([safeThis, latestVersion, currentVersion]
                {
                    if (!safeThis)
                        return;

                    // Parse version numbers for proper comparison
                    auto parseVersion = [](const juce::String& version) -> int
                    {
                        juce::StringArray parts = juce::StringArray::fromTokens(version, ".", "");
                        if (parts.size() >= 3)
                        {
                            return parts[0].getIntValue() * 10000 + 
                                   parts[1].getIntValue() * 100   + 
                                   parts[2].getIntValue();
                        }
                        return 0;
                    };
                    
                    const int latestVersionNum  = parseVersion(latestVersion);
                    const int currentVersionNum = parseVersion(currentVersion);
                    
                    if (latestVersionNum > currentVersionNum)
                    {
                        safeThis->m_updateStatusLabel.setText("New version " + latestVersion + " available!", juce::dontSendNotification);
                        safeThis->m_updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
                    }
                    else
                    {
                        safeThis->m_updateStatusLabel.setText("You have the latest version", juce::dontSendNotification);
                        safeThis->m_updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
                    }
                });
            }
            else
            {
                juce::MessageManager::callAsync([safeThis]
                {
                    if (!safeThis)
                        return;

                    safeThis->m_updateStatusLabel.setText("No releases found", juce::dontSendNotification);
                    safeThis->m_updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
                });
            }
        }
        else
        {
            juce::MessageManager::callAsync([safeThis]
            {
                if (!safeThis)
                    return;

                safeThis->m_updateStatusLabel.setText("Invalid response from server", juce::dontSendNotification);
                safeThis->m_updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::red);
            });
        }
    });
}

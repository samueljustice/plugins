#include "AboutWindow.h"
#include <JuceHeader.h>

AboutWindow::AboutWindow()
    : DocumentWindow("About SammyJs Stretch Armstrong",
                    juce::Colour(0xff0a0a0a),
                    DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    content = std::make_unique<AboutContent>();
    setContentOwned(content.release(), true);

    centreWithSize(500, 650);
    setVisible(true);
    setResizable(false, false);
    setAlwaysOnTop(true);
    toFront(true);
}

AboutWindow::~AboutWindow()
{
}

void AboutWindow::closeButtonPressed()
{
    delete this;
}

AboutWindow::AboutContent::AboutContent()
{
    websiteButton.setButtonText("sweetjusticesound.com");
    websiteButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    websiteButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff00ffff));
    websiteButton.onClick = []
    {
        juce::URL("https://sweetjusticesound.com").launchInDefaultBrowser();
    };
    addAndMakeVisible(websiteButton);

    emailButton.setButtonText("sam@sweetjusticesound.com");
    emailButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    emailButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff00ffff));
    emailButton.onClick = []
    {
        juce::URL("mailto:sam@sweetjusticesound.com").launchInDefaultBrowser();
    };
    addAndMakeVisible(emailButton);

    checkUpdatesButton.setButtonText("Check for Updates");
    checkUpdatesButton.onClick = [this] { checkForUpdates(); };
    addAndMakeVisible(checkUpdatesButton);

    updateStatusLabel.setText("", juce::dontSendNotification);
    updateStatusLabel.setJustificationType(juce::Justification::centred);
    updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    addAndMakeVisible(updateStatusLabel);

    licenseText.setMultiLine(true);
    licenseText.setReadOnly(true);
    licenseText.setScrollbarsShown(true);
    licenseText.setCaretVisible(false);
    licenseText.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff0f0f0f));
    licenseText.setColour(juce::TextEditor::textColourId, juce::Colour(0xffffffff));
    licenseText.setText(
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
    addAndMakeVisible(licenseText);
}

void AboutWindow::AboutContent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0a0a0a));

    g.setColour(juce::Colour(0xff00ffff));
    g.setFont(juce::Font(juce::FontOptions("Courier New", 24.0f, juce::Font::bold)));
    g.drawText("SAMMYJS STRETCH ARMSTRONG", 0, 20, getWidth(), 30, juce::Justification::centred);

    g.setColour(juce::Colour(0xffff00ff));
    g.setFont(juce::Font(juce::FontOptions(16.0f)));
    g.drawText("Version " PLUGIN_VERSION, 0, 60, getWidth(), 20, juce::Justification::centred);

    g.setColour(juce::Colour(0xffffffff));
    g.setFont(juce::Font(juce::FontOptions(14.0f)));
    g.drawText("Threshold-Triggered Time Stretcher", 0, 90, getWidth(), 20, juce::Justification::centred);

    g.drawText("Created by Samuel Justice", 0, 120, getWidth(), 20, juce::Justification::centred);

    g.setColour(juce::Colour(0xff1a3a3a));
    g.drawLine(20, 280, getWidth() - 20, 280, 2);

    g.setColour(juce::Colour(0xffffffff));
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawText("License Information:", 20, 290, getWidth() - 40, 20, juce::Justification::left);
}

void AboutWindow::AboutContent::resized()
{
    websiteButton.setBounds(150, 150, 200, 25);
    emailButton.setBounds(150, 180, 200, 25);
    checkUpdatesButton.setBounds(150, 215, 200, 30);
    updateStatusLabel.setBounds(50, 250, getWidth() - 100, 25);
    licenseText.setBounds(20, 320, getWidth() - 40, getHeight() - 340);
}

void AboutWindow::AboutContent::checkForUpdates()
{
    updateStatusLabel.setText("Checking for updates...", juce::dontSendNotification);
    updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);

    juce::URL apiUrl("https://api.github.com/repos/samueljustice/plugins/releases");

    juce::Thread::launch([this, apiUrl]
    {
        auto stream = apiUrl.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                               .withConnectionTimeoutMs(5000));

        if (!stream)
        {
            juce::MessageManager::callAsync([this]
            {
                updateStatusLabel.setText("Failed to check for updates", juce::dontSendNotification);
                updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::red);
            });
            return;
        }

        auto responseContent = stream->readEntireStreamAsString();
        auto releases = juce::JSON::parse(responseContent);

        if (auto* releasesArray = releases.getArray())
        {
            juce::String latestVersion;

            for (const auto& release : *releasesArray)
            {
                if (auto* obj = release.getDynamicObject())
                {
                    auto tagName = obj->getProperty("tag_name").toString();

                    if (tagName.startsWith("stretcharmstrong-v"))
                    {
                        latestVersion = tagName.substring(18);
                        break;
                    }
                }
            }

            if (latestVersion.isNotEmpty())
            {
                auto currentVersion = juce::String(PLUGIN_VERSION);

                juce::MessageManager::callAsync([this, latestVersion, currentVersion]
                {
                    auto parseVersion = [](const juce::String& version) -> int
                    {
                        auto parts = juce::StringArray::fromTokens(version, ".", "");
                        if (parts.size() >= 3)
                        {
                            return parts[0].getIntValue() * 10000 +
                                   parts[1].getIntValue() * 100 +
                                   parts[2].getIntValue();
                        }
                        return 0;
                    };

                    int latestVersionNum = parseVersion(latestVersion);
                    int currentVersionNum = parseVersion(currentVersion);

                    if (latestVersionNum > currentVersionNum)
                    {
                        updateStatusLabel.setText("New version " + latestVersion + " available!",
                                                juce::dontSendNotification);
                        updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
                    }
                    else
                    {
                        updateStatusLabel.setText("You have the latest version",
                                                juce::dontSendNotification);
                        updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
                    }
                });
            }
            else
            {
                juce::MessageManager::callAsync([this]
                {
                    updateStatusLabel.setText("No releases found", juce::dontSendNotification);
                    updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
                });
            }
        }
        else
        {
            juce::MessageManager::callAsync([this]
            {
                updateStatusLabel.setText("Invalid response from server", juce::dontSendNotification);
                updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::red);
            });
        }
    });
}

#include "AboutWindow.h"

namespace
{
#ifdef PLUGIN_VERSION
    const char* const c_version = PLUGIN_VERSION;
#else
    const char* const c_version = JucePlugin_VersionString;
#endif

    constexpr const char* c_tagPrefix = "pitchflattener-v";

    /** "1.10.2" -> comparable integer (major, minor, patch up to 999 each). */
    int64_t versionNumber (const juce::String& version)
    {
        const auto parts = juce::StringArray::fromTokens (version.trim(), ".", "");
        int64_t n = 0;

        for (int i = 0; i < 3; ++i)
            n = n * 1000 + juce::jlimit (0, 999, parts[i].getIntValue());

        return n;
    }

    const char* const c_credits =
        "SammyJs Pitch Flattener\n"
        "Copyright (c) 2025-2026 Samuel Justice\n\n"
        "BUILT WITH\n"
        "- JUCE (juce.com), Raw Material Software Limited.\n"
        "- Rubber Band Library (breakfastquay.com/rubberband), Particular Programs Ltd. "
        "Used under the GNU GPL v2 or later, or a commercial licence.\n"
        "- Azeret Mono typeface, The Azeret Project Authors, SIL Open Font License 1.1.\n\n"
        "PITCH DETECTION ALGORITHMS\n"
        "- pYIN: M. Mauch and S. Dixon, \"pYIN: A fundamental frequency estimator using "
        "probabilistic threshold distributions\", ICASSP 2014.\n"
        "- YIN: A. de Cheveigne and H. Kawahara, \"YIN, a fundamental frequency estimator "
        "for speech and music\", JASA 111(4), 2002.\n"
        "- MPM: P. McLeod and G. Wyvill, \"A smarter way to find pitch\", ICMC 2005.\n"
        "- Subharmonic summation: D. J. Hermes, \"Measurement of pitch by subharmonic "
        "summation\", JASA 83(1), 1988.\n\n"
        "LICENSE (MIT)\n"
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
        "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.\n\n"
        "Third-party components keep their own licences (see above).";
}

//==============================================================================
// AboutWindow
//==============================================================================
AboutWindow::AboutWindow (juce::LookAndFeel& lookAndFeel)
    : DocumentWindow ("About SammyJs Pitch Flattener", Theme::Colours::background, DocumentWindow::closeButton)
{
    setLookAndFeel (&lookAndFeel);
    setUsingNativeTitleBar (true);
    setContentNonOwned (&m_content, false);
    centreWithSize (500, 640);
    setResizable (false, false);
    setAlwaysOnTop (true);
}

AboutWindow::~AboutWindow()
{
    clearContentComponent();
    setLookAndFeel (nullptr);
}

void AboutWindow::closeButtonPressed()
{
    setVisible (false);
}

//==============================================================================
// UpdateChecker
//==============================================================================
AboutWindow::UpdateChecker::UpdateChecker()
    : juce::Thread ("PitchFlattener update check")
{
}

AboutWindow::UpdateChecker::~UpdateChecker()
{
    stopThread (7000);
    cancelPendingUpdate();
}

void AboutWindow::UpdateChecker::start()
{
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::low);
}

void AboutWindow::UpdateChecker::post (const juce::String& message, juce::Colour colour)
{
    {
        const juce::ScopedLock sl (m_lock);
        m_message = message;
        m_colour = colour;
    }

    triggerAsyncUpdate();
}

void AboutWindow::UpdateChecker::handleAsyncUpdate()
{
    juce::String message;
    juce::Colour colour;

    {
        const juce::ScopedLock sl (m_lock);
        message = m_message;
        colour = m_colour;
    }

    if (onResult != nullptr)
        onResult (message, colour);
}

void AboutWindow::UpdateChecker::run()
{
    using namespace Theme::Colours;

    const juce::URL url ("https://api.github.com/repos/samueljustice/plugins/releases?per_page=100");
    const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                             .withConnectionTimeoutMs (5000)
                             .withExtraHeaders ("Accept: application/vnd.github+json\r\nUser-Agent: SammyJs-PitchFlattener");

    const auto stream = url.createInputStream (options);

    if (threadShouldExit())
        return;

    if (stream == nullptr)
    {
        post ("Could not reach GitHub", bad);
        return;
    }

    const auto response = stream->readEntireStreamAsString();

    if (threadShouldExit())
        return;

    const auto releases = juce::JSON::parse (response);
    const auto* list = releases.getArray();

    if (list == nullptr)
    {
        post ("Unexpected response from GitHub", bad);
        return;
    }

    juce::String latest;
    int64_t latestNumber = -1;

    for (const auto& release : *list)
    {
        if ((bool) release.getProperty ("draft", false) || (bool) release.getProperty ("prerelease", false))
            continue;

        const auto tag = release.getProperty ("tag_name", {}).toString();
        if (! tag.startsWith (c_tagPrefix))
            continue;

        const auto version = tag.substring ((int) std::strlen (c_tagPrefix));
        const auto number = versionNumber (version);

        if (number > latestNumber)
        {
            latestNumber = number;
            latest = version;
        }
    }

    if (latest.isEmpty())
        post ("No releases found", textDim);
    else if (latestNumber > versionNumber (c_version))
        post ("Version " + latest + " is available", target);
    else
        post ("You have the latest version", good);
}

//==============================================================================
// AboutContent
//==============================================================================
AboutWindow::AboutContent::AboutContent()
{
    using namespace Theme::Colours;

    const auto setupLink = [this] (juce::TextButton& b, const juce::String& caption, const juce::String& link)
    {
        b.setButtonText (caption);
        b.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        b.setColour (juce::TextButton::textColourOffId, target);
        b.onClick = [link] { juce::URL (link).launchInDefaultBrowser(); };
        addAndMakeVisible (b);
    };

    setupLink (m_websiteButton, "sweetjusticesound.com", "https://sweetjusticesound.com");
    setupLink (m_emailButton, "sam@sweetjusticesound.com", "mailto:sam@sweetjusticesound.com");

    m_checkUpdatesButton.setButtonText ("CHECK FOR UPDATES");
    m_checkUpdatesButton.onClick = [this]
    {
        if (m_updateChecker.isChecking())
            return;

        m_updateStatusLabel.setText ("Checking...", juce::dontSendNotification);
        m_updateStatusLabel.setColour (juce::Label::textColourId, Theme::Colours::textDim);
        m_updateChecker.start();
    };
    addAndMakeVisible (m_checkUpdatesButton);

    m_updateChecker.onResult = [this] (const juce::String& message, juce::Colour colour)
    {
        m_updateStatusLabel.setText (message, juce::dontSendNotification);
        m_updateStatusLabel.setColour (juce::Label::textColourId, colour);
    };

    m_updateStatusLabel.setJustificationType (juce::Justification::centred);
    m_updateStatusLabel.setColour (juce::Label::textColourId, textDim);
    m_updateStatusLabel.setFont (m_fonts->body (12.0f));
    addAndMakeVisible (m_updateStatusLabel);

    m_creditsText.setMultiLine (true);
    m_creditsText.setReadOnly (true);
    m_creditsText.setScrollbarsShown (true);
    m_creditsText.setCaretVisible (false);
    m_creditsText.setColour (juce::TextEditor::backgroundColourId, panel);
    m_creditsText.setColour (juce::TextEditor::textColourId, textDim);
    m_creditsText.setColour (juce::TextEditor::outlineColourId, outlineSoft);
    m_creditsText.setColour (juce::TextEditor::focusedOutlineColourId, outlineSoft);
    m_creditsText.setFont (m_fonts->body (11.0f));
    m_creditsText.setText (c_credits, false);
    addAndMakeVisible (m_creditsText);
}

void AboutWindow::AboutContent::paint (juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll (background);
    const int w = getWidth();

    g.setColour (text);
    g.setFont (m_fonts->heading (22.0f));
    g.drawText ("PITCH FLATTENER", 0, 22, w, 30, juce::Justification::centred);

    g.setColour (target);
    g.setFont (m_fonts->body (13.0f));
    g.drawText (juce::String ("Version ") + c_version, 0, 60, w, 20, juce::Justification::centred);

    g.setColour (textFaint);
    g.setFont (m_fonts->body (11.0f));
#ifdef BUILD_TIMESTAMP
    g.drawText (juce::String ("Built ") + BUILD_TIMESTAMP, 0, 80, w, 16, juce::Justification::centred);
#else
    g.drawText (juce::String ("Built ") + __DATE__ + " " + __TIME__, 0, 80, w, 16, juce::Justification::centred);
#endif

    g.setColour (textDim);
    g.setFont (m_fonts->label (11.5f));
    g.drawText ("SAMMYJS  /  PITCH FLATTENING FOR SOUND DESIGN", 0, 104, w, 20, juce::Justification::centred);
    g.drawText ("CREATED BY SAMUEL JUSTICE", 0, 126, w, 20, juce::Justification::centred);

    g.setColour (outlineSoft);
    g.fillRect (20, 292, w - 40, 1);

    g.setColour (textDim);
    g.setFont (m_fonts->label (10.5f));
    g.drawText ("CREDITS", 20, 300, w - 40, 20, juce::Justification::centredLeft);
}

void AboutWindow::AboutContent::resized()
{
    const int cx = getWidth() / 2;
    m_websiteButton.setBounds (cx - 120, 160, 240, 24);
    m_emailButton.setBounds (cx - 120, 188, 240, 24);
    m_checkUpdatesButton.setBounds (cx - 100, 224, 200, 28);
    m_updateStatusLabel.setBounds (40, 258, getWidth() - 80, 24);
    m_creditsText.setBounds (20, 324, getWidth() - 40, getHeight() - 344);
}

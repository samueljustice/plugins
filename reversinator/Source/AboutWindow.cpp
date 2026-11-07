#include "AboutWindow.h"

namespace
{
    constexpr int c_width = 480;
    constexpr int c_height = 600;
    const char* const c_releasesUrl = "https://api.github.com/repos/samueljustice/plugins/releases";
    const char* const c_tagPrefix = "reversinator-v";
}

int Version::toNumber(const juce::String& version)
{
    const auto parts = juce::StringArray::fromTokens(version.trim(), ".", "");
    int number = 0;

    for (int i = 0; i < 3; ++i)
        number = number * 1000 + (i < parts.size() ? juce::jlimit(0, 999, parts[i].getIntValue()) : 0);

    return number;
}

//==============================================================================
AboutWindow::AboutWindow()
    : DocumentWindow("About SammyJs Reversinator", Theme::Colours::background, DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    setContentNonOwned(&m_content, false);
    centreWithSize(c_width, c_height);
    setResizable(false, false);
    setAlwaysOnTop(true);
}

void AboutWindow::closeButtonPressed()
{
    setVisible(false);
}

//==============================================================================
AboutWindow::AboutContent::AboutContent()
{
    const auto linkButton = [this](juce::TextButton& button, const juce::String& text, const juce::String& url)
    {
        button.setButtonText(text);
        button.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        button.setColour(juce::TextButton::textColourOffId, Theme::Colours::reverser);
        button.onClick = [url] { juce::URL(url).launchInDefaultBrowser(); };
        addAndMakeVisible(button);
    };

    linkButton(m_websiteButton, "sweetjusticesound.com", "https://www.sweetjusticesound.com");
    linkButton(m_emailButton, "sam@sweetjusticesound.com", "mailto:sam@sweetjusticesound.com");

    m_checkUpdatesButton.setButtonText("Check for Updates");
    m_checkUpdatesButton.onClick = [this] { checkForUpdates(); };
    addAndMakeVisible(m_checkUpdatesButton);

    m_updateStatusLabel.setJustificationType(juce::Justification::centred);
    m_updateStatusLabel.setFont(m_fonts->body(12.0f));
    addAndMakeVisible(m_updateStatusLabel);

    m_licenseText.setMultiLine(true);
    m_licenseText.setReadOnly(true);
    m_licenseText.setScrollbarsShown(true);
    m_licenseText.setCaretVisible(false);
    m_licenseText.setColour(juce::TextEditor::backgroundColourId, Theme::Colours::panel);
    m_licenseText.setColour(juce::TextEditor::textColourId, Theme::Colours::textDim);
    m_licenseText.setColour(juce::TextEditor::outlineColourId, Theme::Colours::outlineSoft);
    m_licenseText.setColour(juce::TextEditor::focusedOutlineColourId, Theme::Colours::outline);
    m_licenseText.setFont(m_fonts->body(11.0f));
    m_licenseText.setText(juce::String::fromUTF8(BinaryData::LICENSE, BinaryData::LICENSESize).trim()
                          + "\n\n"
                            "Reversinator is an independent recreation of the effects in Backwards Machine "
                            "by The Sound Guy, Inc. (2007). It is not affiliated with or endorsed by them.\n\n"
                            "Built with JUCE (juce.com), used under the JUCE licence.");
    addAndMakeVisible(m_licenseText);
}

AboutWindow::AboutContent::~AboutContent()
{
    if (m_updateChecker != nullptr)
        m_updateChecker->stopThread(6000);
}

void AboutWindow::AboutContent::paint(juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll(background);

    g.setColour(text);
    g.setFont(m_fonts->heading(22.0f));
    g.drawText("REVERSINATOR", 0, 24, getWidth(), 30, juce::Justification::centred);

    g.setColour(reverser);
    g.setFont(m_fonts->body(13.0f));
    g.drawText(juce::String("Version ") + JucePlugin_VersionString, 0, 62, getWidth(), 20, juce::Justification::centred);

    g.setColour(textFaint);
    g.setFont(m_fonts->body(11.0f));
#ifdef BUILD_TIMESTAMP
    g.drawText(juce::String("Built: ") + BUILD_TIMESTAMP, 0, 82, getWidth(), 16, juce::Justification::centred);
#else
    g.drawText(juce::String("Built: ") + __DATE__ + " " + __TIME__, 0, 82, getWidth(), 16, juce::Justification::centred);
#endif

    g.setColour(textDim);
    g.setFont(m_fonts->label(12.0f));
    g.drawText("SAMMYJS  /  REAL-TIME REVERSER", 0, 108, getWidth(), 20, juce::Justification::centred);
    g.drawText("Created by Samuel Justice", 0, 134, getWidth(), 20, juce::Justification::centred);

    g.setColour(outlineSoft);
    g.drawLine(20.0f, 292.0f, static_cast<float>(getWidth() - 20), 292.0f, 1.0f);

    g.setColour(textDim);
    g.setFont(m_fonts->label(11.0f));
    g.drawText("LICENSE", 20, 302, getWidth() - 40, 20, juce::Justification::centredLeft);
}

void AboutWindow::AboutContent::resized()
{
    const int buttonX = (getWidth() - 240) / 2;
    m_websiteButton.setBounds(buttonX, 168, 240, 24);
    m_emailButton.setBounds(buttonX, 196, 240, 24);
    m_checkUpdatesButton.setBounds(buttonX, 230, 240, 28);
    m_updateStatusLabel.setBounds(20, 262, getWidth() - 40, 22);
    m_licenseText.setBounds(20, 328, getWidth() - 40, getHeight() - 348);
}

void AboutWindow::AboutContent::showStatus(const juce::String& text, juce::Colour colour)
{
    m_updateStatusLabel.setColour(juce::Label::textColourId, colour);
    m_updateStatusLabel.setText(text, juce::dontSendNotification);
}

void AboutWindow::AboutContent::checkForUpdates()
{
    if (m_updateChecker != nullptr && m_updateChecker->isThreadRunning())
        return;

    showStatus("Checking for updates...", Theme::Colours::textDim);
    m_updateChecker = std::make_unique<UpdateChecker>(juce::Component::SafePointer<AboutContent>(this));
    m_updateChecker->startThread();
}

//==============================================================================
AboutWindow::AboutContent::UpdateChecker::UpdateChecker(juce::Component::SafePointer<AboutContent> owner)
    : juce::Thread("Reversinator update check")
    , m_owner(owner)
{
}

void AboutWindow::AboutContent::UpdateChecker::run()
{
    juce::String message;
    juce::Colour colour = Theme::Colours::feedback;

    const auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                             .withConnectionTimeoutMs(5000)
                             .withProgressCallback([this](int, int) { return ! threadShouldExit(); });

    if (const auto stream = juce::URL(c_releasesUrl).createInputStream(options))
    {
        const auto releases = juce::JSON::parse(stream->readEntireStreamAsString());
        juce::String latest;

        if (const auto* list = releases.getArray())
        {
            // Releases are listed newest first; take the first Reversinator tag.
            for (const auto& release : *list)
            {
                const auto tag = release.getProperty("tag_name", {}).toString();
                if (tag.startsWith(c_tagPrefix))
                {
                    latest = tag.fromFirstOccurrenceOf(c_tagPrefix, false, false);
                    break;
                }
            }

            if (latest.isEmpty())
                message = "No releases found";
            else if (Version::toNumber(latest) > Version::toNumber(JucePlugin_VersionString))
                { message = "Version " + latest + " is available"; colour = Theme::Colours::mode; }
            else
                { message = "You have the latest version"; colour = Theme::Colours::textDim; }
        }
        else
        {
            message = "Invalid response from server";
        }
    }
    else
    {
        message = "Could not reach GitHub";
    }

    if (threadShouldExit())
        return;

    juce::MessageManager::callAsync([owner = m_owner, message, colour]
    {
        if (auto* content = owner.getComponent())
            content->showStatus(message, colour);
    });
}

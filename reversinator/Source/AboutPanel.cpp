#include "AboutPanel.h"

namespace
{
    constexpr int c_cardWidth = 440;
    constexpr int c_cardHeight = 508;
    // The repository holds several plugins' releases, so ask for a long page.
    const char* const c_releasesUrl = "https://api.github.com/repos/samueljustice/plugins/releases?per_page=100";
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

juce::String Version::latestRelease(const juce::var& releases)
{
    juce::String latest;

    if (const auto* list = releases.getArray())
    {
        for (const auto& release : *list)
        {
            const auto tag = release.getProperty("tag_name", {}).toString();
            if (! tag.startsWith(c_tagPrefix) || static_cast<bool>(release.getProperty("prerelease", false)))
                continue;

            const auto version = tag.fromFirstOccurrenceOf(c_tagPrefix, false, false);
            if (latest.isEmpty() || toNumber(version) > toNumber(latest))
                latest = version;
        }
    }

    return latest;
}

//==============================================================================
AboutPanel::AboutPanel(const Theme::Fonts& fonts)
    : m_fonts(fonts)
{
    using namespace Theme::Colours;

    const auto linkButton = [this](juce::TextButton& button, const juce::String& caption, const juce::String& url)
    {
        button.setButtonText(caption);
        button.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        button.setColour(juce::TextButton::textColourOffId, reverser);
        button.onClick = [url] { juce::URL(url).launchInDefaultBrowser(); };
        addAndMakeVisible(button);
    };

    linkButton(m_websiteButton, "sweetjusticesound.com", "https://www.sweetjusticesound.com");
    linkButton(m_emailButton, "sam@sweetjusticesound.com", "mailto:sam@sweetjusticesound.com");

    m_closeButton.setButtonText(juce::String::fromUTF8("\xc3\x97"));   // ×
    m_closeButton.setTooltip("Close");
    m_closeButton.onClick = [this] { setVisible(false); };
    addAndMakeVisible(m_closeButton);

    m_checkUpdatesButton.setButtonText("Check for Updates");
    m_checkUpdatesButton.onClick = [this] { checkForUpdates(); };
    addAndMakeVisible(m_checkUpdatesButton);

    m_updateStatusLabel.setJustificationType(juce::Justification::centred);
    m_updateStatusLabel.setFont(m_fonts.body(12.0f));
    addAndMakeVisible(m_updateStatusLabel);

    m_licenseText.setMultiLine(true);
    m_licenseText.setReadOnly(true);
    m_licenseText.setScrollbarsShown(true);
    m_licenseText.setCaretVisible(false);
    m_licenseText.setColour(juce::TextEditor::backgroundColourId, background);
    m_licenseText.setColour(juce::TextEditor::textColourId, textDim);
    m_licenseText.setColour(juce::TextEditor::outlineColourId, outlineSoft);
    m_licenseText.setColour(juce::TextEditor::focusedOutlineColourId, outline);
    m_licenseText.setFont(m_fonts.body(11.0f));

    // The LICENSE file is hard-wrapped at ~80 columns; reflow its paragraphs to the box width.
    juce::StringArray paragraphs;
    paragraphs.addTokens(juce::String::fromUTF8(BinaryData::LICENSE, BinaryData::LICENSESize)
                             .replace("\r\n", "\n").trim().replace("\n\n", "\x01"),
                         "\x01", "");
    for (auto& paragraph : paragraphs)
        paragraph = paragraph.replace("\n", " ").trim();

    m_licenseText.setText(paragraphs.joinIntoString("\n\n")
                          + "\n\n"
                            "Reversinator is an independent recreation of the effects in Backwards Machine "
                            "by The Sound Guy, Inc. (2007). It is not affiliated with or endorsed by them.\n\n"
                            "Built with JUCE (juce.com), used under the JUCE licence.");
    addAndMakeVisible(m_licenseText);

    setInterceptsMouseClicks(true, true);
    setVisible(false);
}

AboutPanel::~AboutPanel()
{
    m_updateChecker.reset();   // cancels any request in flight, then joins
}

void AboutPanel::show()
{
    setVisible(true);
    toFront(false);
}

juce::Rectangle<int> AboutPanel::cardBounds() const
{
    return getLocalBounds().withSizeKeepingCentre(c_cardWidth, std::min(c_cardHeight, getHeight() - 24));
}

void AboutPanel::mouseUp(const juce::MouseEvent& e)
{
    if (! cardBounds().contains(e.getPosition()))
        setVisible(false);
}

void AboutPanel::paint(juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll(juce::Colours::black.withAlpha(0.6f));

    const auto card = cardBounds();
    const auto r = card.toFloat();
    g.setColour(panel);
    g.fillRoundedRectangle(r, 10.0f);
    g.setColour(outline);
    g.drawRoundedRectangle(r.reduced(0.5f), 10.0f, 1.0f);

    const auto row = [&](int y, int h) { return juce::Rectangle<int>(card.getX(), card.getY() + y, card.getWidth(), h); };

    g.setColour(text);
    g.setFont(m_fonts.heading(22.0f));
    g.drawText("REVERSINATOR", row(24, 30), juce::Justification::centred);

    g.setColour(reverser);
    g.setFont(m_fonts.body(13.0f));
    g.drawText(juce::String("Version ") + JucePlugin_VersionString, row(58, 20), juce::Justification::centred);

    g.setColour(textFaint);
    g.setFont(m_fonts.body(11.0f));
#ifdef BUILD_TIMESTAMP
    g.drawText(juce::String("Built: ") + BUILD_TIMESTAMP, row(78, 16), juce::Justification::centred);
#else
    g.drawText(juce::String("Built: ") + __DATE__ + " " + __TIME__, row(78, 16), juce::Justification::centred);
#endif

    g.setColour(textDim);
    g.setFont(m_fonts.label(12.0f));
    g.drawText("SAMMYJS  /  REAL-TIME REVERSER", row(102, 20), juce::Justification::centred);
    g.drawText("Created by Samuel Justice", row(124, 20), juce::Justification::centred);

    g.setColour(outlineSoft);
    g.drawHorizontalLine(card.getY() + 276, static_cast<float>(card.getX() + 20), static_cast<float>(card.getRight() - 20));

    g.setColour(textDim);
    g.setFont(m_fonts.label(11.0f));
    g.drawText("LICENSE", row(284, 20).reduced(20, 0), juce::Justification::centredLeft);
}

void AboutPanel::resized()
{
    const auto card = cardBounds();
    const int x = card.getX() + (card.getWidth() - 240) / 2;

    m_closeButton.setBounds(card.getRight() - 38, card.getY() + 10, 28, 28);
    m_websiteButton.setBounds(x, card.getY() + 152, 240, 24);
    m_emailButton.setBounds(x, card.getY() + 180, 240, 24);
    m_checkUpdatesButton.setBounds(x, card.getY() + 214, 240, 28);
    m_updateStatusLabel.setBounds(card.getX() + 20, card.getY() + 246, card.getWidth() - 40, 22);
    m_licenseText.setBounds(card.getX() + 20, card.getY() + 308, card.getWidth() - 40, card.getHeight() - 324);
}

void AboutPanel::showStatus(const juce::String& message, juce::Colour colour)
{
    m_updateStatusLabel.setColour(juce::Label::textColourId, colour);
    m_updateStatusLabel.setText(message, juce::dontSendNotification);
}

void AboutPanel::checkForUpdates()
{
    if (m_updateChecker != nullptr && m_updateChecker->isThreadRunning())
        return;

    showStatus("Checking for updates...", Theme::Colours::textDim);
    m_updateChecker = std::make_unique<UpdateChecker>(juce::Component::SafePointer<AboutPanel>(this));
    m_updateChecker->startThread();
}

//==============================================================================
AboutPanel::UpdateChecker::UpdateChecker(juce::Component::SafePointer<AboutPanel> owner)
    : juce::Thread("Reversinator update check")
    , m_owner(owner)
{
}

AboutPanel::UpdateChecker::~UpdateChecker()
{
    // A cancel can land before the request exists and be lost, so keep cancelling until the
    // thread has gone (normally at once). The force-kill in stopThread() is the last resort.
    for (int i = 0; i < 200 && isThreadRunning(); ++i)
    {
        cancel();
        waitForThreadToExit(10);
    }
    stopThread(1000);
}

void AboutPanel::UpdateChecker::cancel()
{
    signalThreadShouldExit();

    // WebInputStream::cancel() unblocks connect() and read() from another thread.
    const juce::ScopedLock sl(m_lock);
    if (m_stream != nullptr)
        m_stream->cancel();
}

void AboutPanel::UpdateChecker::run()
{
    juce::String body;
    int status = 0;

    {
        juce::WebInputStream stream(juce::URL(c_releasesUrl), false);
        stream.withConnectionTimeout(5000)
              .withExtraHeaders("User-Agent: SammyJs-Reversinator\r\nAccept: application/vnd.github+json");

        {
            const juce::ScopedLock sl(m_lock);
            if (threadShouldExit())
                return;
            m_stream = &stream;
        }

        if (stream.connect(this))
        {
            status = stream.getStatusCode();
            if (status == 200)
                body = stream.readEntireStreamAsString();
        }

        const juce::ScopedLock sl(m_lock);
        m_stream = nullptr;
    }

    if (threadShouldExit())
        return;

    juce::String message;
    juce::Colour colour = Theme::Colours::feedback;

    // Keep the parsed tree in a named local: it owns the array that latestRelease() walks.
    const auto releases = status == 200 ? juce::JSON::parse(body) : juce::var();

    if (status != 200)
    {
        message = status == 0 ? juce::String("Could not reach GitHub") : "GitHub answered " + juce::String(status);
    }
    else if (! releases.isArray())
    {
        message = "Invalid response from server";
    }
    else if (const auto latest = Version::latestRelease(releases); latest.isEmpty())
    {
        message = "No releases found";
    }
    else if (Version::toNumber(latest) > Version::toNumber(JucePlugin_VersionString))
    {
        message = "Version " + latest + " is available";
        colour = Theme::Colours::mode;
    }
    else
    {
        message = "You have the latest version";
        colour = Theme::Colours::textDim;
    }

    juce::MessageManager::callAsync([owner = m_owner, message, colour]
    {
        if (auto* panel = owner.getComponent())
            panel->showStatus(message, colour);
    });
}

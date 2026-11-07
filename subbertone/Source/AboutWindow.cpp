#include "AboutWindow.h"
#include <JuceHeader.h>

namespace
{
    const char* const c_releasesUrl = "https://api.github.com/repos/samueljustice/plugins/releases";
    constexpr int c_connectionTimeoutMs = 5000;
    constexpr int c_stopTimeoutMs = 10000; // a cancelled request unwinds in milliseconds; this is a backstop

    // Layout of the About window's content (500 x 650), in the editor's design units.
    constexpr int c_margin = 16;
    constexpr int c_headerBlockHeight = 66;  // mark + name, tagline, version
    constexpr int c_panelGap = 12;
    constexpr int c_panelHeaderHeight = 32;  // as the editor's sections
    constexpr int c_creditsHeight = 98;
    constexpr int c_textInset = 26;          // body text lines up with the panel title, after its dot
    constexpr int c_linkHeight = 22;

    const char* const c_tagline = "SAMMYJS  /  SUBHARMONIC GENERATOR";

    juce::String buildText()
    {
#ifdef BUILD_TIMESTAMP
        return juce::String(BUILD_TIMESTAMP);
#else
        return (juce::String(__DATE__) + " " + __TIME__).replace("  ", " ");
#endif
    }
}

//-----------------------------------------------------------------
// AboutWindow
//-----------------------------------------------------------------
AboutWindow::AboutWindow(SubbertoneLookAndFeel& editorLookAndFeel, std::function<void()> onClose)
    : DocumentWindow("About SammyJs Subbertone", Theme::Colours::background, DocumentWindow::closeButton)
    , m_onClose(std::move(onClose))
    , m_content(editorLookAndFeel)
{
    setLookAndFeel(&editorLookAndFeel); // the editor's: it outlives this window (the editor deletes it first)
    SubbertoneLookAndFeel::showFocusRingsAfterTab(*this);

    setUsingNativeTitleBar(true);
    setContentNonOwned(&m_content, false);

    centreWithSize(500, 650);
    setVisible(true);
    setResizable(false, false);
    setAlwaysOnTop(true);
    toFront(true);
}

AboutWindow::~AboutWindow()
{
    m_content.stopUpdateCheck(); // aborts a request in flight and joins its thread
    clearContentComponent();
    setLookAndFeel(nullptr);
}

void AboutWindow::closeButtonPressed()
{
    // The owner deletes the window, and with it m_onClose: call a copy.
    if (auto onClose = m_onClose)
        onClose();
    else
        setVisible(false);
}


//-----------------------------------------------------------------
// UpdateCheck
//-----------------------------------------------------------------
AboutWindow::UpdateCheck::UpdateCheck(juce::URL releasesUrl, juce::String currentVersion,
                                      std::function<void(const Result&)> onResult)
    : juce::Thread("Subbertone update check")
    , m_url(std::move(releasesUrl))
    , m_currentVersion(std::move(currentVersion))
    , m_onResult(std::move(onResult))
{
}

AboutWindow::UpdateCheck::~UpdateCheck()
{
    cancel();
}

bool AboutWindow::UpdateCheck::start()
{
    if (isThreadRunning())
        return false;

    cancelPendingUpdate(); // an earlier result not shown yet is out of date
    return startThread();
}

void AboutWindow::UpdateCheck::cancel()
{
    signalThreadShouldExit();

    {
        const juce::ScopedLock sl(m_lock);
        if (m_request != nullptr)
            m_request->cancel(); // wakes a connect() or read() blocked in the network
    }

    stopThread(c_stopTimeoutMs);
    cancelPendingUpdate(); // a result posted just before the thread stopped is dropped
}

void AboutWindow::UpdateCheck::run()
{
    juce::WebInputStream request(m_url, false);
    request.withConnectionTimeout(c_connectionTimeoutMs);

    {
        // Checked under the lock, so cancel() either sees the request or stops it from starting.
        const juce::ScopedLock sl(m_lock);
        if (threadShouldExit())
            return;
        m_request = &request;
    }

    const bool connected = request.connect(nullptr) && !request.isError();
    const auto response = connected && !threadShouldExit() ? request.readEntireStreamAsString() : juce::String();

    {
        const juce::ScopedLock sl(m_lock);
        m_request = nullptr;
    }

    if (threadShouldExit())
        return;

    auto result = connected ? interpret(response, m_currentVersion)
                            : Result { "Failed to check for updates", Theme::Colours::bad };
    {
        const juce::ScopedLock sl(m_lock);
        m_result = std::move(result);
    }

    triggerAsyncUpdate(); // the window is only ever touched on the message thread
}

void AboutWindow::UpdateCheck::handleAsyncUpdate()
{
    Result result;
    {
        const juce::ScopedLock sl(m_lock);
        result = m_result;
    }

    if (m_onResult)
        m_onResult(result);
}

AboutWindow::UpdateCheck::Result AboutWindow::UpdateCheck::interpret(const juce::String& response, const juce::String& currentVersion)
{
    // Status colours as in the editor: failure 'bad', success 'good', something to act on amber,
    // anything else (no Subbertone release listed) the dim text colour.
    const juce::var releases = juce::JSON::parse(response);
    const juce::Array<juce::var>* releasesArray = releases.getArray();

    if (releasesArray == nullptr)
        return { "Invalid response from server", Theme::Colours::bad };

    // Releases are listed newest first; the first one tagged for Subbertone is the latest.
    juce::String latestVersion;
    for (const juce::var& release : *releasesArray)
    {
        if (const juce::DynamicObject* obj = release.getDynamicObject())
        {
            const juce::String tagName = obj->getProperty("tag_name").toString();
            if (tagName.startsWith("subbertone-v"))
            {
                latestVersion = tagName.substring(12); // Skip "subbertone-v"
                break;
            }
        }
    }

    if (latestVersion.isEmpty())
        return { "No releases found", Theme::Colours::textDim };

    // Parse version numbers for proper comparison
    auto parseVersion = [](const juce::String& version) -> int
    {
        const juce::StringArray parts = juce::StringArray::fromTokens(version, ".", "");
        if (parts.size() >= 3)
        {
            return parts[0].getIntValue() * 10000 +
                   parts[1].getIntValue() * 100   +
                   parts[2].getIntValue();
        }
        return 0;
    };

    if (parseVersion(latestVersion) > parseVersion(currentVersion))
        return { "New version " + latestVersion + " available!", Theme::Colours::output };

    return { "You have the latest version", Theme::Colours::good };
}


//-----------------------------------------------------------------
// AboutContent
//-----------------------------------------------------------------
AboutWindow::AboutContent::AboutContent(SubbertoneLookAndFeel& editorLookAndFeel)
    : m_lookAndFeel(editorLookAndFeel)
    , m_updateCheck(juce::URL(c_releasesUrl), JucePlugin_VersionString, [this](const UpdateCheck::Result& result)
      {
          m_updateStatusLabel.setText(result.text, juce::dontSendNotification);
          m_updateStatusLabel.setColour(juce::Label::textColourId, result.colour);
          m_checkUpdatesButton.setEnabled(true);
      })
{
    // Set here as well as on the window, so the children are drawn with it from the start.
    setLookAndFeel(&m_lookAndFeel);

    // Links: dim text, amber and underlined under the mouse (the LookAndFeel's link style for a text
    // button with a transparent fill).
    for (auto* link : { &m_websiteButton, &m_emailButton })
    {
        link->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        link->setColour(juce::TextButton::textColourOffId, Theme::Colours::textDim);
        link->setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    m_websiteButton.setButtonText("sweetjusticesound.com");
    m_websiteButton.onClick = []
    {
        juce::URL("https://sweetjusticesound.com").launchInDefaultBrowser();
    };
    addAndMakeVisible(m_websiteButton);

    m_emailButton.setButtonText("sam@sweetjusticesound.com");
    m_emailButton.onClick = []
    {
        juce::URL("mailto:sam@sweetjusticesound.com").launchInDefaultBrowser();
    };
    addAndMakeVisible(m_emailButton);

    // A header-row switch like LIMIT; the label is drawn in capitals, its text is kept.
    m_checkUpdatesButton.setButtonText("Check for Updates");
    SubbertoneLookAndFeel::setLabelDrawnInCapitals(m_checkUpdatesButton);
    m_checkUpdatesButton.onClick = [this] { checkForUpdates(); };
    addAndMakeVisible(m_checkUpdatesButton);

    // The result, in the header row next to the button (like the limiter's readout in MASTER).
    m_updateStatusLabel.setText("", juce::dontSendNotification);
    m_updateStatusLabel.setJustificationType(juce::Justification::centredRight);
    m_updateStatusLabel.setColour(juce::Label::textColourId, Theme::Colours::textDim);
    m_updateStatusLabel.setFont(m_fonts->body(11.0f));
    m_updateStatusLabel.setBorderSize(juce::BorderSize<int>(0));
    m_updateStatusLabel.setMinimumHorizontalScale(1.0f); // never squashed
    m_updateStatusLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(m_updateStatusLabel);

    // The licence text sits on its panel, in the dim text colour, with the theme's scrollbar.
    m_licenseText.setMultiLine(true);
    m_licenseText.setReadOnly(true);
    m_licenseText.setScrollbarsShown(true);
    m_licenseText.setCaretVisible(false);
    m_licenseText.setBorder(juce::BorderSize<int>(0));
    m_licenseText.setIndents(6, 4);
    m_licenseText.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    m_licenseText.setColour(juce::TextEditor::textColourId, Theme::Colours::textDim);
    m_licenseText.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack); // amber ring after Tab only
    m_licenseText.setFont(m_fonts->body(11.5f));
    m_licenseText.setLineSpacing(1.25f);
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

AboutWindow::AboutContent::~AboutContent()
{
    setLookAndFeel(nullptr);
}

void AboutWindow::AboutContent::paint(juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll(background);

    // Header, as the editor's: the mark and the name, then the tagline and the version (amber).
    const float x = static_cast<float>(c_margin);
    const float width = static_cast<float>(getWidth()) - 2.0f * x;
    const auto titleRow = juce::Rectangle<float>(x, static_cast<float>(c_margin), width, 30.0f);
    SubbertoneLookAndFeel::drawMark(g, juce::Rectangle<float>(30.0f, 22.0f).withPosition(x, titleRow.getCentreY() - 11.0f));

    const float textX = x + 30.0f + 12.0f;
    g.setFont(m_fonts->heading(22.0f));
    g.setColour(text);
    g.drawText("SUBBERTONE", titleRow.withLeft(textX), juce::Justification::centredLeft, false);

    const auto small = m_fonts->label(10.5f);
    g.setFont(small);
    g.setColour(textFaint);
    g.drawText(c_tagline, juce::Rectangle<float>(textX, titleRow.getBottom() + 2.0f, width, 16.0f),
               juce::Justification::centredLeft, false);

    juce::AttributedString version;
    version.setJustification(juce::Justification::centredLeft);
    version.append(juce::String("VERSION ") + JucePlugin_VersionString, small, output);
    version.append("  /  BUILT " + buildText().toUpperCase(), small, textFaint);
    juce::TextLayout versionLayout;
    versionLayout.createLayout(version, width);
    versionLayout.draw(g, juce::Rectangle<float>(textX, titleRow.getBottom() + 18.0f, width, 16.0f));

    // Panels like the editor's sections.
    m_lookAndFeel.drawPanel(g, m_creditsPanel.toFloat(), "CREATED BY", output);
    m_lookAndFeel.drawPanel(g, m_updatesPanel.toFloat(), "UPDATES", input, static_cast<float>(m_updatesPanel.getHeight()));
    m_lookAndFeel.drawPanel(g, m_licencePanel.toFloat(), "LICENSE", master);

    // Credits: the name, then the links separated as in the editor header.
    const auto body = m_creditsPanel.withTrimmedTop(c_panelHeaderHeight).withTrimmedLeft(c_textInset);
    g.setFont(m_fonts->body(13.0f));
    g.setColour(text);
    g.drawText("Samuel Justice", body.withTrimmedTop(10).withHeight(18).toFloat(), juce::Justification::centredLeft, false);

    g.setFont(small);
    g.setColour(textFaint);
    g.drawText("/", juce::Rectangle<int>(m_websiteButton.getRight(), m_websiteButton.getY(),
                                         m_emailButton.getX() - m_websiteButton.getRight(), m_websiteButton.getHeight()).toFloat(),
               juce::Justification::centred, false);
}

void AboutWindow::AboutContent::resized()
{
    auto area = getLocalBounds().reduced(c_margin);
    area.removeFromTop(c_headerBlockHeight + 14);

    m_creditsPanel = area.removeFromTop(c_creditsHeight);
    area.removeFromTop(c_panelGap);
    m_updatesPanel = area.removeFromTop(c_panelHeaderHeight);
    area.removeFromTop(c_panelGap);
    m_licencePanel = area;

    // Links, sized to their text (plus a little slack each side) so they read and respond as text, and the
    // first lines up with the name above it.
    const auto linkFont = m_lookAndFeel.getTextButtonFont(m_websiteButton, c_linkHeight);
    constexpr int linkSlack = 4;
    auto linkWidth = [&linkFont](const juce::Button& b)
    {
        const auto t = b.getButtonText();
        const float w = juce::GlyphArrangement::getStringWidth(linkFont, t + " ") - juce::GlyphArrangement::getStringWidth(linkFont, " ");
        return static_cast<int>(std::ceil(w)) + 2 * linkSlack;
    };
    auto linkRow = m_creditsPanel.withTrimmedTop(c_panelHeaderHeight + 34).withHeight(c_linkHeight)
                                 .withTrimmedLeft(c_textInset - linkSlack);
    m_websiteButton.setBounds(linkRow.removeFromLeft(linkWidth(m_websiteButton)));
    linkRow.removeFromLeft(20);
    m_emailButton.setBounds(linkRow.removeFromLeft(linkWidth(m_emailButton)));

    // UPDATES header row: the title, the status, the button (as LIMIT in the editor's MASTER header).
    auto header = m_updatesPanel.reduced(8, 5);
    const int buttonWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(m_fonts->label(11.0f), "CHECK FOR UPDATES")) + 30;
    m_checkUpdatesButton.setBounds(header.removeFromRight(buttonWidth));
    header.removeFromRight(10);
    const int titleRight = m_updatesPanel.getX() + c_textInset
                         + juce::roundToInt(juce::GlyphArrangement::getStringWidth(m_fonts->heading(11.0f), "UPDATES")) + 12;
    m_updateStatusLabel.setBounds(header.withLeft(titleRight));

    // The licence text inside its panel, text aligned with the title.
    m_licenseText.setBounds(m_licencePanel.withTrimmedTop(c_panelHeaderHeight + 6)
                                          .withTrimmedLeft(c_textInset - 6).withTrimmedRight(8).withTrimmedBottom(8));
}

void AboutWindow::AboutContent::checkForUpdates()
{
    if (!m_updateCheck.start())
        return; // one check at a time

    m_checkUpdatesButton.setEnabled(false);
    m_updateStatusLabel.setText("Checking for updates...", juce::dontSendNotification);
    m_updateStatusLabel.setColour(juce::Label::textColourId, Theme::Colours::textDim);
}

#include "AboutWindow.h"

namespace
{
#ifdef PLUGIN_VERSION
    const char* const c_version = PLUGIN_VERSION;
#else
    const char* const c_version = JucePlugin_VersionString;
#endif

    const char* const c_releasesUrl = "https://api.github.com/repos/samueljustice/plugins/releases?per_page=100";
    constexpr const char* c_tagPrefix = "pitchflattener-v";
    constexpr int c_connectionTimeoutMs = 5000;
    constexpr int c_stopTimeoutMs = 2000; // a cancelled request unwinds in milliseconds; this is a backstop

    // Layout of the About window's content (500 x 650), in the editor's design units.
    constexpr int c_margin = 16;
    constexpr int c_headerBlockHeight = 66;  // mark + name, tagline, version
    constexpr int c_panelGap = 12;
    constexpr int c_panelHeaderHeight = 32;  // as the editor's sections
    constexpr int c_creditsHeight = 98;
    constexpr int c_textInset = 26;          // body text lines up with the panel title, after its dot
    constexpr int c_linkHeight = 22;

    const char* const c_tagline = "SAMMYJS  /  PITCH FLATTENING FOR SOUND DESIGN";

    juce::String buildText()
    {
#ifdef BUILD_TIMESTAMP
        return juce::String (BUILD_TIMESTAMP);
#else
        return (juce::String (__DATE__) + " " + __TIME__).replace ("  ", " ");
#endif
    }

    /** "1.10.2" -> comparable integer (major, minor, patch up to 999 each). */
    int64_t versionNumber (const juce::String& version)
    {
        const auto parts = juce::StringArray::fromTokens (version.trim(), ".", "");
        int64_t n = 0;

        for (int i = 0; i < 3; ++i)
            n = n * 1000 + juce::jlimit (0, 999, parts[i].getIntValue());

        return n;
    }

    /** "Copyright (c) 2025-<build year> Samuel Justice", so the notice never needs updating. */
    juce::String copyrightLine()
    {
        constexpr int firstYear = 2025;
        const int year = juce::String (__DATE__).getLastCharacters (4).getIntValue(); // __DATE__ is "Mmm dd yyyy"
        const auto years = year > firstYear ? juce::String (firstYear) + "-" + juce::String (year) : juce::String (firstYear);
        return "Copyright (c) " + years + " Samuel Justice";
    }

    const char* const c_credits =
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
AboutWindow::AboutWindow (PitchFlattenerLookAndFeel& editorLookAndFeel, std::function<void()> onClose)
    : DocumentWindow ("About SammyJs Pitch Flattener", Theme::Colours::background, DocumentWindow::closeButton)
    , m_onClose (std::move (onClose))
    , m_content (editorLookAndFeel)
{
    setLookAndFeel (&editorLookAndFeel); // the editor's: it outlives this window (the editor deletes it first)
    PitchFlattenerLookAndFeel::showFocusRingsAfterTab (*this);

    setUsingNativeTitleBar (true);
    setContentNonOwned (&m_content, false);

    centreWithSize (500, 650);
    setVisible (true);
    setResizable (false, false);
    setAlwaysOnTop (true);
    toFront (true);
}

AboutWindow::~AboutWindow()
{
    m_content.stopUpdateCheck(); // aborts a request in flight and joins its thread
    clearContentComponent();
    setLookAndFeel (nullptr);
}

void AboutWindow::closeButtonPressed()
{
    // The owner deletes the window, and with it m_onClose: call a copy.
    if (auto onClose = m_onClose)
        onClose();
    else
        setVisible (false);
}

//==============================================================================
// UpdateCheck
//==============================================================================
AboutWindow::UpdateCheck::UpdateCheck (juce::URL releasesUrl, juce::String currentVersion,
                                       std::function<void (const Result&)> onResult)
    : juce::Thread ("PitchFlattener update check")
    , m_url (std::move (releasesUrl))
    , m_currentVersion (std::move (currentVersion))
    , m_onResult (std::move (onResult))
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
    return startThread (juce::Thread::Priority::low);
}

void AboutWindow::UpdateCheck::abortRequest()
{
    const juce::ScopedLock sl (m_lock);
    if (m_request != nullptr)
        m_request->cancel(); // wakes a connect() or read() blocked in the network
}

void AboutWindow::UpdateCheck::cancel()
{
    signalThreadShouldExit();

    // A cancel can land before the request has a connection and be lost, so keep cancelling until
    // the thread has gone (normally at once). The force-kill in stopThread() is the last resort.
    for (int i = 0; i < 200 && isThreadRunning(); ++i)
    {
        abortRequest();
        waitForThreadToExit (10);
    }

    stopThread (c_stopTimeoutMs);
    cancelPendingUpdate(); // a result posted just before the thread stopped is dropped
}

void AboutWindow::UpdateCheck::run()
{
    juce::WebInputStream request (m_url, false);
    request.withConnectionTimeout (c_connectionTimeoutMs)
           .withExtraHeaders ("Accept: application/vnd.github+json\r\nUser-Agent: SammyJs-PitchFlattener");

    {
        // Checked under the lock, so cancel() either sees the request or stops it from starting.
        const juce::ScopedLock sl (m_lock);
        if (threadShouldExit())
            return;
        m_request = &request;
    }

    // No listener: a listener that aborts connect() makes JUCE (macOS) destroy the connection on this
    // thread while cancel() may be using it.
    const bool connected = request.connect (nullptr) && ! request.isError() && request.getStatusCode() == 200;
    const auto response = connected && ! threadShouldExit() ? request.readEntireStreamAsString() : juce::String();

    {
        const juce::ScopedLock sl (m_lock);
        m_request = nullptr;
    }

    if (threadShouldExit())
        return;

    auto result = connected ? interpret (response, m_currentVersion)
                            : Result { "Could not reach GitHub", Theme::Colours::bad };
    {
        const juce::ScopedLock sl (m_lock);
        m_result = std::move (result);
    }

    triggerAsyncUpdate(); // the window is only ever touched on the message thread
}

void AboutWindow::UpdateCheck::handleAsyncUpdate()
{
    Result result;
    {
        const juce::ScopedLock sl (m_lock);
        result = m_result;
    }

    if (m_onResult)
        m_onResult (result);
}

AboutWindow::UpdateCheck::Result AboutWindow::UpdateCheck::interpret (const juce::String& response, const juce::String& currentVersion)
{
    using namespace Theme::Colours;

    // Keep the parsed tree in a named local: it owns the array walked below.
    const auto releases = juce::JSON::parse (response);
    const auto* list = releases.getArray();

    if (list == nullptr)
        return { "Unexpected response from GitHub", bad };

    // The newest published (not draft, not pre-release) release tagged for Pitch Flattener.
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
        return { "No releases found", textDim };

    if (latestNumber > versionNumber (currentVersion))
        return { "Version " + latest + " is available", target };

    return { "You have the latest version", good };
}

//==============================================================================
// AboutContent
//==============================================================================
AboutWindow::AboutContent::AboutContent (PitchFlattenerLookAndFeel& editorLookAndFeel)
    : m_lookAndFeel (editorLookAndFeel)
    , m_updateCheck (juce::URL (c_releasesUrl), c_version, [this] (const UpdateCheck::Result& result)
      {
          m_updateStatusLabel.setText (result.text, juce::dontSendNotification);
          m_updateStatusLabel.setColour (juce::Label::textColourId, result.colour);
          m_checkUpdatesButton.setEnabled (true);
      })
{
    using namespace Theme::Colours;

    // Set here as well as on the window, so the children are drawn with it from the start.
    setLookAndFeel (&m_lookAndFeel);

    // Links: dim text, amber and underlined under the mouse (the LookAndFeel's link style for a text
    // button with a transparent fill).
    const auto setupLink = [this] (juce::TextButton& b, const juce::String& caption, const juce::String& link)
    {
        b.setButtonText (caption);
        b.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        b.setColour (juce::TextButton::textColourOffId, textDim);
        b.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        b.onClick = [link] { juce::URL (link).launchInDefaultBrowser(); };
        addAndMakeVisible (b);
    };

    setupLink (m_websiteButton, "sweetjusticesound.com", "https://sweetjusticesound.com");
    setupLink (m_emailButton, "sam@sweetjusticesound.com", "mailto:sam@sweetjusticesound.com");

    // A header-row button, as RE-LATCH in the editor; the label is drawn in capitals, its text is kept.
    m_checkUpdatesButton.setButtonText ("Check for Updates");
    PitchFlattenerLookAndFeel::setLabelDrawnInCapitals (m_checkUpdatesButton);
    m_checkUpdatesButton.onClick = [this] { checkForUpdates(); };
    addAndMakeVisible (m_checkUpdatesButton);

    // The result, in the header row next to the button.
    m_updateStatusLabel.setJustificationType (juce::Justification::centredRight);
    m_updateStatusLabel.setColour (juce::Label::textColourId, textDim);
    m_updateStatusLabel.setFont (m_fonts->body (11.0f));
    m_updateStatusLabel.setBorderSize (juce::BorderSize<int> (0));
    m_updateStatusLabel.setMinimumHorizontalScale (1.0f); // never squashed
    m_updateStatusLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (m_updateStatusLabel);

    // The credits and licence sit on their panel, in the dim text colour, with the theme's scrollbar.
    m_licenceText.setMultiLine (true);
    m_licenceText.setReadOnly (true);
    m_licenceText.setScrollbarsShown (true);
    m_licenceText.setCaretVisible (false);
    m_licenceText.setBorder (juce::BorderSize<int> (0));
    m_licenceText.setIndents (6, 4);
    m_licenceText.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    m_licenceText.setColour (juce::TextEditor::textColourId, textDim);
    m_licenceText.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack); // amber ring after Tab only
    m_licenceText.setFont (m_fonts->body (11.5f));
    m_licenceText.setLineSpacing (1.25f);
    m_licenceText.setText (copyrightLine() + "\n\n" + c_credits, false);
    addAndMakeVisible (m_licenceText);
}

AboutWindow::AboutContent::~AboutContent()
{
    setLookAndFeel (nullptr);
}

void AboutWindow::AboutContent::paint (juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll (background);

    // Header, as the editor's: the mark and the name, then the tagline and the version (amber).
    const float x = (float) c_margin;
    const float width = (float) getWidth() - 2.0f * x;
    const auto titleRow = juce::Rectangle<float> (x, (float) c_margin, width, 30.0f);
    PitchFlattenerLookAndFeel::drawMark (g, juce::Rectangle<float> (34.0f, 22.0f).withPosition (x, titleRow.getCentreY() - 11.0f));

    const float textX = x + 34.0f + 12.0f;
    g.setFont (m_fonts->heading (22.0f));
    g.setColour (text);
    g.drawText ("PITCH FLATTENER", titleRow.withLeft (textX), juce::Justification::centredLeft, false);

    const auto small = m_fonts->label (10.5f);
    g.setFont (small);
    g.setColour (textFaint);
    g.drawText (c_tagline, juce::Rectangle<float> (textX, titleRow.getBottom() + 2.0f, width, 16.0f),
                juce::Justification::centredLeft, false);

    juce::AttributedString version;
    version.setJustification (juce::Justification::centredLeft);
    version.append (juce::String ("VERSION ") + c_version, small, target);
    version.append ("  /  BUILT " + buildText().toUpperCase(), small, textFaint);
    juce::TextLayout versionLayout;
    versionLayout.createLayout (version, width);
    versionLayout.draw (g, juce::Rectangle<float> (textX, titleRow.getBottom() + 18.0f, width, 16.0f));

    // Panels like the editor's sections.
    m_lookAndFeel.drawPanel (g, m_creditsPanel.toFloat(), "CREATED BY", target);
    m_lookAndFeel.drawPanel (g, m_updatesPanel.toFloat(), "UPDATES", detect, (float) m_updatesPanel.getHeight());
    m_lookAndFeel.drawPanel (g, m_licencePanel.toFloat(), "CREDITS & LICENSE", textDim);

    // Credits: the name, then the links.
    const auto body = m_creditsPanel.withTrimmedTop (c_panelHeaderHeight).withTrimmedLeft (c_textInset);
    g.setFont (m_fonts->body (13.0f));
    g.setColour (text);
    g.drawText ("Samuel Justice", body.withTrimmedTop (10).withHeight (18).toFloat(), juce::Justification::centredLeft, false);

    g.setFont (small);
    g.setColour (textFaint);
    g.drawText ("/", juce::Rectangle<int> (m_websiteButton.getRight(), m_websiteButton.getY(),
                                           m_emailButton.getX() - m_websiteButton.getRight(), m_websiteButton.getHeight()).toFloat(),
                juce::Justification::centred, false);
}

void AboutWindow::AboutContent::resized()
{
    auto area = getLocalBounds().reduced (c_margin);
    area.removeFromTop (c_headerBlockHeight + 14);

    m_creditsPanel = area.removeFromTop (c_creditsHeight);
    area.removeFromTop (c_panelGap);
    m_updatesPanel = area.removeFromTop (c_panelHeaderHeight);
    area.removeFromTop (c_panelGap);
    m_licencePanel = area;

    // Links, sized to their text (plus a little slack each side) so they read and respond as text, and the
    // first lines up with the name above it.
    const auto linkFont = m_lookAndFeel.getTextButtonFont (m_websiteButton, c_linkHeight);
    static constexpr int linkSlack = 4; // static: MSVC will not use an uncaptured local constexpr in the lambda
    const auto linkWidth = [&linkFont] (const juce::Button& b)
    {
        const auto t = b.getButtonText();
        const float w = juce::GlyphArrangement::getStringWidth (linkFont, t + " ") - juce::GlyphArrangement::getStringWidth (linkFont, " ");
        return (int) std::ceil (w) + 2 * linkSlack;
    };
    auto linkRow = m_creditsPanel.withTrimmedTop (c_panelHeaderHeight + 34).withHeight (c_linkHeight)
                                 .withTrimmedLeft (c_textInset - linkSlack);
    m_websiteButton.setBounds (linkRow.removeFromLeft (linkWidth (m_websiteButton)));
    linkRow.removeFromLeft (20);
    m_emailButton.setBounds (linkRow.removeFromLeft (linkWidth (m_emailButton)));

    // UPDATES header row: the title, the status, the button.
    auto header = m_updatesPanel.reduced (8, 5);
    const int buttonWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (m_fonts->label (11.0f), "CHECK FOR UPDATES")) + 30;
    m_checkUpdatesButton.setBounds (header.removeFromRight (buttonWidth));
    header.removeFromRight (10);
    const int titleRight = m_updatesPanel.getX() + c_textInset
                         + juce::roundToInt (juce::GlyphArrangement::getStringWidth (m_fonts->heading (11.0f), "UPDATES")) + 12;
    m_updateStatusLabel.setBounds (header.withLeft (titleRight));

    // The credits and licence text inside its panel, text aligned with the title.
    m_licenceText.setBounds (m_licencePanel.withTrimmedTop (c_panelHeaderHeight + 6)
                                           .withTrimmedLeft (c_textInset - 6).withTrimmedRight (8).withTrimmedBottom (8));
}

void AboutWindow::AboutContent::checkForUpdates()
{
    if (! m_updateCheck.start())
        return; // one check at a time

    m_checkUpdatesButton.setEnabled (false);
    m_updateStatusLabel.setText ("Checking for updates...", juce::dontSendNotification);
    m_updateStatusLabel.setColour (juce::Label::textColourId, Theme::Colours::textDim);
}

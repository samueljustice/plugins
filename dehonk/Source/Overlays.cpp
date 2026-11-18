#include "Overlays.h"

#include "BinaryData.h"

using namespace dehonk;

namespace
{
    constexpr int titleHeight = 44, pad = 20, buttonHeight = 30;
    const char* const releasesUrl = "https://api.github.com/repos/samueljustice/plugins/releases?per_page=100";
    const char* const tagPrefix = "dehonk-v";

    juce::String buildText()
    {
       #ifdef BUILD_TIMESTAMP
        return juce::String (BUILD_TIMESTAMP);
       #else
        return juce::String (__DATE__).replace ("  ", " ");
       #endif
    }

    int buttonWidthFor (const juce::Font& font, const juce::String& text)
    {
        return juce::roundToInt (std::ceil (juce::GlyphArrangement::getStringWidth (font, text.toLowerCase()))) + 34;
    }
}

//============================================================================== OverlayCard
OverlayCard::OverlayCard (DeHonkLookAndFeel& l, const juce::String& t, int cardWidth, int cardHeight)
    : look (l), fonts (l.getFonts()), title (t), width (cardWidth), height (cardHeight)
{
    setLookAndFeel (&look);
    setOpaque (false);
    setInterceptsMouseClicks (true, true);      // the veil takes the clicks meant for the editor beneath
    setWantsKeyboardFocus (true);
    setFocusContainerType (FocusContainerType::keyboardFocusContainer);   // Tab stays among the card's own controls
    setTitle (t);
}

OverlayCard::~OverlayCard()
{
    setLookAndFeel (nullptr);
}

juce::Rectangle<int> OverlayCard::cardBounds() const noexcept
{
    return getLocalBounds().withSizeKeepingCentre (juce::jmin (width, getWidth() - 24), juce::jmin (height, getHeight() - 24));
}

juce::Rectangle<int> OverlayCard::contentBounds() const noexcept
{
    return cardBounds().withTrimmedTop (titleHeight).reduced (pad, 0).withTrimmedBottom (pad - 4);
}

void OverlayCard::parentHierarchyChanged()
{
    if (isShowing() && ! hasKeyboardFocus (true))
        grabKeyboardFocus();
}

void OverlayCard::styleButton (juce::TextButton& button, bool primary)
{
    if (primary)
        button.setColour (juce::TextButton::buttonColourId, toy::sunYellow);
    addAndMakeVisible (button);
}

void OverlayCard::paint (juce::Graphics& g)
{
    // The veil: the window's cream over everything, so the editor shows faintly through.
    g.fillAll (toy::cream.withAlpha (0.78f));

    const auto card = cardBounds().toFloat().reduced (toy::outline * 0.5f);
    g.setColour (toy::paper);
    g.fillRoundedRectangle (card, 12.0f);
    g.setColour (toy::ink);
    g.drawRoundedRectangle (card, 12.0f, toy::outline);

    // The title, and the little rainbow under it.
    g.setFont (fonts.title (16.0f));
    g.drawText (title, card.withHeight ((float) titleHeight - 8.0f).reduced ((float) pad, 0.0f).translated (0.0f, 4.0f), juce::Justification::centredLeft, true);
    for (int i = 0; i < 5; ++i)
    {
        g.setColour (toy::rainbow[i]);
        g.fillRect (juce::Rectangle<float> (card.getX() + (float) pad, card.getY() + (float) titleHeight - 9.0f + (float) i * 1.5f, 64.0f, 1.5f));
    }

    paintCard (g, contentBounds());
}

void OverlayCard::mouseDown (const juce::MouseEvent& e)
{
    if (closesOnVeilClick() && ! cardBounds().contains (e.getPosition()))
        cancel();
}

bool OverlayCard::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        cancel();
        return true;
    }
    if (key == juce::KeyPress::returnKey)
    {
        // The button that has the keyboard focus (after Tab), otherwise the card's own answer.
        for (auto* child : getChildren())
        {
            if (auto* button = dynamic_cast<juce::Button*> (child); button != nullptr && button->hasKeyboardFocus (false))
            {
                button->triggerClick();
                return true;
            }
        }
        accept();
        return true;
    }
    return key != juce::KeyPress::tabKey;    // nothing typed here reaches the host or the editor beneath
}

void OverlayCard::close()
{
    if (closing)
        return;
    closing = true;
    setVisible (false);

    // Deferred: this may be inside one of the card's own buttons' callbacks.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<OverlayCard> (this)]
    {
        if (safe == nullptr)
            return;
        const auto callback = safe->onClose;     // a copy: calling it deletes the card
        if (callback != nullptr)
            callback();
    });
}

//============================================================================== PromptCard
PromptCard::PromptCard (DeHonkLookAndFeel& l, const juce::String& t, const juce::String& m, bool withText,
                        const juce::String& initialText, const juce::String& acceptText, std::function<void (bool, juce::String)> onDone)
    : OverlayCard (l, t, 420, withText ? 214 : 176), message (m), done (std::move (onDone)), acceptButton (acceptText), hasText (withText)
{
    if (hasText)
    {
        editor.setFont (fonts.value (14.0f));
        editor.setText (initialText, false);
        editor.setSelectAllWhenFocused (true);
        editor.setInputRestrictions (64);
        editor.setJustification (juce::Justification::centredLeft);
        editor.setIndents (12, 0);
        editor.onReturnKey = [this] { accept(); };
        editor.onTextChange = [this] { checkText(); };
        editor.onEscapeKey = [this] { cancel(); };
        addAndMakeVisible (editor);
    }
    acceptButton.onClick = [this] { accept(); };
    cancelButton.onClick = [this] { cancel(); };
    styleButton (acceptButton, true);
    styleButton (cancelButton, false);
}

void PromptCard::parentHierarchyChanged()
{
    // The line to type in takes the keyboard as soon as the card is up.
    if (hasText && isShowing() && ! editor.hasKeyboardFocus (true))
        editor.grabKeyboardFocus();
    else
        OverlayCard::parentHierarchyChanged();
}

void PromptCard::resized()
{
    auto content = contentBounds();
    auto buttons = content.removeFromBottom (buttonHeight);
    const auto font = look.getTextButtonFont (acceptButton, buttonHeight);
    const int w = juce::jmax (92, buttonWidthFor (font, acceptButton.getButtonText()), buttonWidthFor (font, cancelButton.getButtonText()));
    acceptButton.setBounds (buttons.removeFromRight (w));
    buttons.removeFromRight (10);
    cancelButton.setBounds (buttons.removeFromRight (w));
    if (hasText)
    {
        content.removeFromBottom (14 + 18);       // the gap, and the line that says what is wrong
        editor.setBounds (content.removeFromBottom (32));
    }
}

void PromptCard::setValidator (std::function<juce::String (const juce::String&)> whatIsWrong)
{
    validator = std::move (whatIsWrong);
    checkText();
}

void PromptCard::checkText()
{
    const auto wrong = validator != nullptr && hasText ? validator (editor.getText()) : juce::String();
    acceptButton.setEnabled (wrong.isEmpty());
    if (wrong != note)
    {
        note = wrong;
        repaint();
    }
}

void PromptCard::paintCard (juce::Graphics& g, juce::Rectangle<int> content)
{
    auto area = content.withTrimmedTop (8).withTrimmedBottom (buttonHeight + 14 + (hasText ? 64 : 0));
    g.setFont (fonts.body (13.0f));
    g.setColour (toy::ink);
    g.drawFittedText (message, area, juce::Justification::topLeft, 4, 0.9f);
    if (hasText && note.isNotEmpty())
    {
        g.setFont (fonts.body (11.5f));
        g.setColour (toy::jacketRed.darker (0.15f));
        g.drawFittedText (note, editor.getBounds().translated (0, editor.getHeight() + 3).withHeight (16).reduced (12, 0), juce::Justification::centredLeft, 1, 0.9f);
    }
}

void PromptCard::finish (bool accepted)
{
    if (finished)
        return;
    finished = true;
    const auto text = editor.getText();
    const auto callback = done;
    close();
    if (callback != nullptr)
        callback (accepted, text);
}

void PromptCard::accept()
{
    checkText();
    if (acceptButton.isEnabled())
        finish (true);
}
void PromptCard::cancel() { finish (false); }

//============================================================================== AboutCard
/** Looks up De-Honk's newest release on a worker thread. The thread never touches the card: it
    hands its result to the message thread, where it is delivered only while this object exists.
    Deleting it aborts the request in flight and waits for the thread. */
class AboutCard::Lookup : private juce::Thread,
                          private juce::AsyncUpdater
{
public:
    explicit Lookup (std::function<void (const juce::String& text, const juce::String& url)> onResult)
        : juce::Thread ("De-Honk update check"), result (std::move (onResult))
    {
    }

    ~Lookup() override
    {
        signalThreadShouldExit();
        // A cancel can land before the request exists and be lost, so keep cancelling until the
        // thread has gone (normally at once).
        for (int i = 0; i < 200 && isThreadRunning(); ++i)
        {
            {
                const juce::ScopedLock sl (lock);
                if (request != nullptr)
                    request->cancel();
            }
            waitForThreadToExit (10);
        }
        stopThread (2000);
        cancelPendingUpdate();
    }

    bool start()
    {
        if (isThreadRunning())
            return false;
        cancelPendingUpdate();
        return startThread (juce::Thread::Priority::low);
    }

    /** What a releases reply (GitHub's JSON list) means for this version. */
    static void interpret (const juce::String& body, const juce::String& current, juce::String& text, juce::String& url)
    {
        url.clear();
        const auto releases = juce::JSON::parse (body);
        const auto* list = releases.getArray();
        if (list == nullptr)
        {
            text = "Could not read the reply.";
            return;
        }

        juce::String best, bestUrl;
        for (const auto& release : *list)
        {
            const auto tag = release.getProperty ("tag_name", {}).toString();
            if (! tag.startsWith (tagPrefix) || static_cast<bool> (release.getProperty ("draft", false)))
                continue;
            const auto candidate = tag.substring ((int) std::strlen (tagPrefix));
            if (! candidate.containsOnly ("0123456789."))
                continue;
            if (best.isEmpty() || sammyj::UpdateCheck::isNewer (candidate, best))
            {
                best = candidate;
                bestUrl = release.getProperty ("html_url", {}).toString();
            }
        }

        if (best.isEmpty())
            text = "No release found.";
        else if (sammyj::UpdateCheck::isNewer (best, current) && bestUrl.startsWith ("https://github.com/"))
        {
            text = "Version " + best + " is available.";
            url = bestUrl;
        }
        else
            text = "This is the newest version.";
    }

private:
    void run() override
    {
        juce::String body;
        int statusCode = 0;
        {
            juce::WebInputStream stream (juce::URL (releasesUrl), false);
            // The same request the shared update check makes (common/SammyJUpdateCheck.cpp).
            stream.withConnectionTimeout (5000)
                  .withExtraHeaders ("User-Agent: SammyJ-Plugins-UpdateCheck\r\nAccept: application/vnd.github+json");
            {
                const juce::ScopedLock sl (lock);
                if (threadShouldExit())
                    return;
                request = &stream;
            }
            if (stream.connect (nullptr))
            {
                statusCode = stream.getStatusCode();
                if (statusCode == 200)
                    body = stream.readEntireStreamAsString();
            }
            const juce::ScopedLock sl (lock);
            request = nullptr;
        }

        if (threadShouldExit())
            return;

        juce::String text, url;
        if (statusCode == 200)
            interpret (body, JucePlugin_VersionString, text, url);
        else
            text = "Could not check just now.";

        {
            const juce::ScopedLock sl (lock);
            pendingText = text;
            pendingUrl = url;
        }
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override
    {
        juce::String text, url;
        {
            const juce::ScopedLock sl (lock);
            text = pendingText;
            url = pendingUrl;
        }
        if (result != nullptr)
            result (text, url);
    }

    const std::function<void (const juce::String&, const juce::String&)> result;
    juce::CriticalSection lock;
    juce::WebInputStream* request = nullptr;
    juce::String pendingText, pendingUrl;
};

AboutCard::AboutCard (DeHonkLookAndFeel& l) : OverlayCard (l, "about", 470, 456)
{
    siteLink.getProperties().set ("link", true);
    siteLink.getProperties().set ("keepCase", true);
    siteLink.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    siteLink.setTooltip ("Opens sweetjusticesound.com in your browser.");
    siteLink.onClick = [] { juce::URL ("https://sweetjusticesound.com").launchInDefaultBrowser(); };
    addAndMakeVisible (siteLink);

    mailLink.getProperties().set ("link", true);
    mailLink.getProperties().set ("keepCase", true);
    mailLink.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    mailLink.setTooltip ("Starts an email to Samuel in your mail program.");
    mailLink.onClick = [] { juce::URL ("mailto:sam@sweetjusticesound.com").launchInDefaultBrowser(); };
    addAndMakeVisible (mailLink);

    checkButton.setTooltip ("Looks on the release page for a newer De-Honk, test builds included. Nothing is downloaded.");
    checkButton.onClick = [this] { checkNow(); };
    styleButton (checkButton, false);

    downloadButton.setTooltip ("Opens the newer version's release page in your browser.");
    downloadButton.onClick = [this]
    {
        if (downloadUrl.startsWith ("https://github.com/"))
            juce::URL (downloadUrl).launchInDefaultBrowser();
    };
    styleButton (downloadButton, true);
    downloadButton.setVisible (false);

    closeButton.onClick = [this] { close(); };
    styleButton (closeButton, false);

    licence.setMultiLine (true);
    licence.setReadOnly (true);
    licence.setScrollbarsShown (true);
    licence.setCaretVisible (false);
    licence.setPopupMenuEnabled (false);
    licence.setFont (fonts.body (10.5f));
    licence.setLineSpacing (1.2f);
    licence.setIndents (10, 8);
    licence.getProperties().set ("card", true);
    licence.setColour (juce::TextEditor::textColourId, toy::ink.withAlpha (0.8f));
    {
        // The file is wrapped at 80 columns: make each paragraph one line again, so it wraps to the box.
        auto text = juce::String::fromUTF8 (BinaryData::LICENSE, BinaryData::LICENSESize).replace ("\r\n", "\n").trim();
        text = text.replace ("\n\n", "\r").replace ("\n", " ").replace ("\r", "\n\n");
        licence.setText (text, false);
        licence.setCaretPosition (0);
    }
    addAndMakeVisible (licence);

    lookup = std::make_unique<Lookup> ([this] (const juce::String& text, const juce::String& url) { showCheckResult (text, url); });
}

AboutCard::~AboutCard()
{
    lookup.reset();     // aborts a request in flight and joins its thread before the card goes
}

void AboutCard::checkNow()
{
    if (lookup == nullptr || ! lookup->start())
        return;
    checkButton.setEnabled (false);
    status = "Checking...";
    downloadUrl.clear();
    downloadButton.setVisible (false);
    resized();
    repaint();
}

void AboutCard::showCheckResult (const juce::String& text, const juce::String& url)
{
    status = text;
    downloadUrl = url;
    checkButton.setEnabled (true);
    downloadButton.setVisible (url.isNotEmpty());
    resized();
    repaint();
}

void AboutCard::resized()
{
    auto content = contentBounds();

    // From the bottom: Close; the licence; the update row. The name and the credits are painted above.
    auto bottom = content.removeFromBottom (buttonHeight);
    const auto font = look.getTextButtonFont (closeButton, buttonHeight);
    closeButton.setBounds (bottom.removeFromRight (buttonWidthFor (font, closeButton.getButtonText())));

    content.removeFromBottom (12);
    licence.setBounds (content.removeFromBottom (118));
    licence.moveCaretToTop (false);                // it opens at its first line
    content.removeFromBottom (12);

    auto updates = content.removeFromBottom (buttonHeight);
    checkButton.setBounds (updates.removeFromLeft (buttonWidthFor (font, checkButton.getButtonText())));
    if (downloadButton.isVisible())
        downloadButton.setBounds (updates.removeFromRight (buttonWidthFor (font, downloadButton.getButtonText())));

    // The link sits under the credit line (see paintCard for the same numbers).
    const auto top = contentBounds();
    const auto linkFont = fonts.value (12.0f);
    siteLink.setBounds (top.getX() - 5, top.getY() + 90, juce::roundToInt (std::ceil (juce::GlyphArrangement::getStringWidth (linkFont, siteLink.getButtonText()))) + 10, 20);
    mailLink.setBounds (siteLink.getRight() + 8, siteLink.getY(), juce::roundToInt (std::ceil (juce::GlyphArrangement::getStringWidth (linkFont, mailLink.getButtonText()))) + 10, 20);
}

void AboutCard::paintCard (juce::Graphics& g, juce::Rectangle<int> content)
{
    auto area = content.toFloat();

    g.setColour (toy::ink);
    g.setFont (fonts.title (28.0f));
    g.drawText ("de-honk", area.removeFromTop (34.0f), juce::Justification::centredLeft, false);

    g.setFont (fonts.value (12.0f));
    g.setColour (toy::ink.withAlpha (0.8f));
    g.drawText (juce::String ("version ") + JucePlugin_VersionString + "  /  built " + buildText().toLowerCase(),
                area.removeFromTop (18.0f), juce::Justification::centredLeft, true);

    area.removeFromTop (14.0f);
    g.setColour (toy::ink);
    g.setFont (fonts.value (14.0f));
    g.drawText ("by Samuel Justice", area.removeFromTop (20.0f), juce::Justification::centredLeft, false);

    area.removeFromTop (4.0f + 20.0f + 12.0f);      // the link
    g.setFont (fonts.body (12.0f));
    g.setColour (toy::ink.withAlpha (0.85f));
    g.drawFittedText ("Takes the honk out of the low mids of a finished mix, with narrow cuts that come and go with it. "
                      "For the master bus, before the limiter.",
                      area.removeFromTop (48.0f).toNearestInt(), juce::Justification::topLeft, 3, 0.9f);

    // The result of a check, beside its button.
    if (status.isNotEmpty())
    {
        const int left = checkButton.getRight() + 12;
        const int right = downloadButton.isVisible() ? downloadButton.getX() - 10 : content.getRight();
        g.setFont (fonts.value (12.0f));
        g.setColour (toy::ink);
        g.drawFittedText (status, juce::Rectangle<int> (left, checkButton.getY(), right - left, checkButton.getHeight()),
                          juce::Justification::centredLeft, 2, 0.8f);
    }
}

//============================================================================== UpdateCard
UpdateCard::UpdateCard (DeHonkLookAndFeel& l, sammyj::UpdateInfo i)
    : OverlayCard (l, "De-Honk " + i.latestVersion + " is available", 460, 164), info (std::move (i))
{
    setDescription ("You have " + info.currentVersion + ".");

    downloadButton.setTooltip ("Opens the download page for the Sammy J's Plugins installer in your browser.");
    laterButton.setTooltip ("Closes this for now. It is offered again another day.");
    skipButton.setTooltip ("Does not offer this version again. A newer one is still offered.");
    downloadButton.onClick = [this] { accept(); };
    laterButton.onClick = [this] { close(); };
    skipButton.onClick = [this]
    {
        sammyj::UpdateCheck::skipVersion (info);
        close();
    };

    // Tab order: Download (the answer Return gives), Later, Skip this version.
    int order = 1;
    for (auto* b : { &downloadButton, &laterButton, &skipButton })
    {
        b->setExplicitFocusOrder (order++);
        styleButton (*b, b == &downloadButton);
    }
}

void UpdateCard::accept()
{
    info.openDownloadPage();
    close();
}

void UpdateCard::resized()
{
    auto row = contentBounds().removeFromBottom (buttonHeight);
    const auto font = look.getTextButtonFont (downloadButton, buttonHeight);
    downloadButton.setBounds (row.removeFromRight (buttonWidthFor (font, downloadButton.getButtonText())));
    row.removeFromRight (10);
    laterButton.setBounds (row.removeFromRight (buttonWidthFor (font, laterButton.getButtonText())));
    row.removeFromRight (10);
    skipButton.setBounds (row.removeFromRight (juce::jmin (row.getWidth(), buttonWidthFor (font, skipButton.getButtonText()))));
}

void UpdateCard::paintCard (juce::Graphics& g, juce::Rectangle<int> content)
{
    g.setFont (fonts.body (13.0f));
    g.setColour (toy::ink);
    g.drawFittedText ("You have " + info.currentVersion + ". Download opens the page with the installer.",
                      content.withTrimmedTop (8).withTrimmedBottom (buttonHeight + 12), juce::Justification::topLeft, 3, 0.9f);
}

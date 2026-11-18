#pragma once

#include <JuceHeader.h>

#include "DeHonkLookAndFeel.h"
#include "SammyJUpdateCheck.h"
#include "Theme.h"

#include <functional>
#include <memory>

/** A card drawn over the whole editor on a veil of the window's cream: the window's own dialogs,
    its About page and its "new version" message. They are children of the editor, not windows of
    their own, so they look the same in every host and can never be lost behind one.

    The editor owns the card, keeps it the size of the editor and deletes it when onClose is called
    (always after the click that caused it has returned). Escape cancels; Return accepts. Clicks on
    the veil are swallowed, and close the card only if the subclass says so. */
class OverlayCard : public juce::Component
{
public:
    OverlayCard (DeHonkLookAndFeel&, const juce::String& title, int cardWidth, int cardHeight);
    ~OverlayCard() override;

    std::function<void()> onClose;

    juce::Rectangle<int> cardBounds() const noexcept;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void parentHierarchyChanged() override;

protected:
    /** Inside the card, under the title. */
    juce::Rectangle<int> contentBounds() const noexcept;
    void setCardHeight (int h) { height = h; resized(); repaint(); }
    void styleButton (juce::TextButton&, bool primary);
    void close();

    virtual void accept() { close(); }
    virtual void cancel() { close(); }
    virtual bool closesOnVeilClick() const { return false; }
    /** Anything the subclass draws on the card besides its child components. */
    virtual void paintCard (juce::Graphics&, juce::Rectangle<int> /*content*/) {}

    DeHonkLookAndFeel& look;
    const Theme::Fonts& fonts;

private:
    const juce::String title;
    const int width;
    int height;
    bool closing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OverlayCard)
};

//==============================================================================
/** A question with two answers, and a line of text to type if `withText`. onDone is called with
    the answer (and the text) before the card closes. */
class PromptCard : public OverlayCard
{
public:
    PromptCard (DeHonkLookAndFeel&, const juce::String& title, const juce::String& message, bool withText,
                const juce::String& initialText, const juce::String& acceptText, std::function<void (bool accepted, juce::String text)> onDone);

    /** For a card with a line to type in: says what is wrong with the text as it stands (an
        empty string if nothing is). While something is wrong the accept button is off, Return
        does nothing, and the reason is shown under the line. */
    void setValidator (std::function<juce::String (const juce::String&)> whatIsWrong);

    void resized() override;
    void parentHierarchyChanged() override;

private:
    void accept() override;
    void cancel() override;
    void paintCard (juce::Graphics&, juce::Rectangle<int>) override;
    void finish (bool accepted);
    void checkText();

    std::function<juce::String (const juce::String&)> validator;
    juce::String note;

    const juce::String message;
    std::function<void (bool, juce::String)> done;
    juce::TextEditor editor;
    juce::TextButton acceptButton, cancelButton { "Cancel" };
    bool hasText = false, finished = false;
};

//==============================================================================
/** The About page: the name and version, who made it, the link to his site, a check for a newer
    version (it looks at every De-Honk release, test builds included) and the licence. */
class AboutCard : public OverlayCard
{
public:
    explicit AboutCard (DeHonkLookAndFeel&);
    ~AboutCard() override;

    void resized() override;

    /** For pictures: shows a result without going online. */
    void showCheckResult (const juce::String& text, const juce::String& downloadUrl);

private:
    class Lookup;

    bool closesOnVeilClick() const override { return true; }
    void paintCard (juce::Graphics&, juce::Rectangle<int>) override;
    void checkNow();

    juce::TextButton siteLink { "sweetjusticesound.com" }, mailLink { "sam@sweetjusticesound.com" };
    juce::TextButton checkButton { "Check for updates" }, downloadButton { "Download" }, closeButton { "Close" };
    juce::TextEditor licence;
    juce::String status, downloadUrl;
    std::unique_ptr<Lookup> lookup;
};

//==============================================================================
/** "De-Honk x.y.z is available": shown when the update check the Sammy J plug-ins share finds a
    newer release. Download (Return) opens the installer's page, Later (Escape) closes it until
    another day, Skip this version never offers that one again. */
class UpdateCard : public OverlayCard
{
public:
    UpdateCard (DeHonkLookAndFeel&, sammyj::UpdateInfo);

    void resized() override;

private:
    void accept() override;
    void paintCard (juce::Graphics&, juce::Rectangle<int>) override;

    const sammyj::UpdateInfo info;
    juce::TextButton downloadButton { "Download" }, laterButton { "Later" }, skipButton { "Skip this version" };
};

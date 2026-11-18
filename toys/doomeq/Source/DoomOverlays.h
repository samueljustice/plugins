#pragma once

#include <JuceHeader.h>
#include "DoomLook.h"
#include "SammyJUpdateCheck.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

/*
 * DOOM EQ's in-editor overlays: the preset dialogs (DoomDialog), the "new version" notice
 * (UpdatePopup) and the About card (AboutPanel).
 *
 * JUCE's own AlertWindow and TextEditor can't draw in the WAD's pixel font, so these are drawn
 * here instead. Each one is a child the editor sizes to its whole bounds: a dimmed backdrop that
 * takes every click (the controls under it can't be used), with a riveted panel card in the middle.
 * Buttons are plain TextButtons, so the editor's DoomEQLookAndFeel draws them like every other button.
 * The overlay keeps the keyboard focus (its buttons don't take it), so Return and Esc always reach it.
 *
 * The owner deletes an overlay from its result callback; those callbacks are always posted
 * (MessageManager::callAsync), never called from inside the overlay's own button or key handler.
 */

//==============================================================================
/** The backdrop, card and focus handling every overlay shares. */
class DoomOverlay : public juce::Component
{
public:
    ~DoomOverlay() override;

    void mouseDown (const juce::MouseEvent&) override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

protected:
    DoomOverlay (const WadArt&, const Textures&);

    /** What Esc and a click outside the card do. */
    virtual void cancel() = 0;

    /** Dim backdrop, then the card at m_card with the hazard strip under its title row. */
    void paintBackdropAndCard (juce::Graphics&, int titleRowHeight) const;

    /** Every button on a card: drawn by the editor's LookAndFeel, never takes the keyboard focus. */
    static void setUpButton (juce::TextButton&, bool primary);

    const WadArt& m_art;
    const Textures& m_tex;
    juce::Rectangle<int> m_card; // set by each overlay's resized()

private:
    void grabFocusIfShowing();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DoomOverlay)
};

//==============================================================================
struct DoomDialogOptions
{
    juce::String title, message;             // upper-case text; message may wrap over several lines
    bool withTextEntry = false;
    juce::String initialText;                // for text entry
    int maxTextLength = 24;
    juce::StringArray buttons;               // e.g. {"SAVE","CANCEL"}: buttons[0] is the primary (Return), the LAST is the cancel (Esc / click outside)
    std::function<void (int buttonIndex, const juce::String& text)> onResult; // called once, async, on the message thread
};

/**
 * A DOOM-styled alert or name prompt. The name prompt is drawn like DOOM's save-game slot
 * (LSLEFT / LSCNTR / LSRGHT from the WAD) with a blinking "_" cursor, and takes only the
 * characters the pixel font has and a file name allows.
 */
class DoomDialog : public DoomOverlay,
                   private juce::Timer
{
public:
    DoomDialog (const WadArt&, const Textures&, DoomDialogOptions);
    ~DoomDialog() override;

    /** The text typed so far (empty without text entry). */
    const juce::String& getText() const { return m_text; }

    /** A button, for a tooltip (nullptr when out of range). */
    juce::TextButton* getButton (int index) const { return m_buttons[index]; }

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

protected:
    /** Which button Esc and a click outside press (the last one unless set). */
    void setCancelButton (int index);

    void cancel() override;

private:
    void timerCallback() override;
    void finish (int buttonIndex);
    void paintTextSlot (juce::Graphics&) const;
    static bool isAllowedCharacter (juce::juce_wchar);

    const DoomDialogOptions m_options;
    juce::String m_text;
    juce::OwnedArray<juce::TextButton> m_buttons;
    int m_cancelIndex = 0;
    bool m_done = false;
    bool m_cursorOn = true;

    juce::Image m_slotLeft, m_slotCentre, m_slotRight;
    juce::Rectangle<int> m_titleArea, m_messageArea, m_slotArea;
    juce::StringArray m_messageLines;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DoomDialog)
};

//==============================================================================
/**
 * "DOOM EQ x.y.z IS AVAILABLE": shown when sammyj::UpdateCheck finds a newer release.
 * DOWNLOAD (Return) opens the installer's page, LATER (Esc, or a click outside) just closes,
 * SKIP THIS VERSION stops this version being offered again. onClose runs afterwards, async;
 * the owner deletes the popup there.
 */
class UpdatePopup : public DoomDialog
{
public:
    UpdatePopup (const WadArt&, const Textures&, const sammyj::UpdateInfo&, std::function<void()> onClose);
    ~UpdatePopup() override;

private:
    static DoomDialogOptions makeOptions (const sammyj::UpdateInfo&, std::function<void()> onClose);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdatePopup)
};

//==============================================================================
namespace DoomOverlayParts
{
    /** A web or mail link in the pixel font: dim, gold under the mouse. */
    class PixelLink : public juce::Button
    {
    public:
        PixelLink (const WadArt&, const juce::String& text, juce::URL target);

        /** The width the text wants at the link's scale. */
        int getIdealWidth() const;

        void paintButton (juce::Graphics&, bool highlighted, bool down) override;

    private:
        const WadArt& m_art;
        const juce::URL m_target;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PixelLink)
    };

    /** Paragraphs of word-wrapped pixel text, sized to its content (lives in a Viewport). */
    class PixelTextView : public juce::Component
    {
    public:
        explicit PixelTextView (const WadArt&);

        void addParagraph (const juce::String& text, juce::Colour, float scale, int gapAfter);

        /** Wraps everything to this width and sets the height to fit. */
        void layout (int width);

        void paint (juce::Graphics&) override;

    private:
        struct Paragraph { juce::String text; juce::Colour colour; float scale; int gapAfter; };
        struct Line { juce::String text; juce::Colour colour; float scale; int y; };

        const WadArt& m_art;
        std::vector<Paragraph> m_paragraphs;
        std::vector<Line> m_lines;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PixelTextView)
    };
}

//==============================================================================
/**
 * About SammyJs DOOM EQ: wordmark, version, credits, the manual update check and the licences.
 * Esc, CLOSE or a click outside call onClose (async); the owner deletes the panel there.
 */
class AboutPanel : public DoomOverlay,
                   private juce::Timer
{
public:
    AboutPanel (const WadArt&, const Textures&, std::function<void()> onClose);
    ~AboutPanel() override;  // stops the manual update-check thread

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class ManualCheck;
    struct CheckResult;

    void cancel() override;
    void timerCallback() override;
    void close();
    void startCheck();
    void showCheckResult (const CheckResult&);
    void download();

    const std::function<void()> m_onClose;
    bool m_closing = false;

    juce::Image m_skull[2];
    int m_skullFrame = 0;

    DoomOverlayParts::PixelLink m_webLink, m_mailLink;
    juce::TextButton m_checkButton { "CHECK FOR UPDATES" };
    juce::TextButton m_downloadButton { "DOWNLOAD" };
    juce::TextButton m_closeButton { "CLOSE" };
    juce::Viewport m_viewport;
    DoomOverlayParts::PixelTextView m_credits;

    juce::String m_status;
    juce::Colour m_statusColour;
    juce::String m_newVersion, m_newUrl;
    bool m_newIsPrerelease = false;

    juce::Rectangle<int> m_titleRow, m_skullArea, m_taglineRow, m_versionRow, m_createdHeading, m_nameRow,
                         m_updatesHeading, m_statusArea, m_creditsHeading, m_creditsBox;

    // Last: destroyed first, so a check finishing during teardown never sees a half-gone panel.
    std::unique_ptr<ManualCheck> m_check;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutPanel)
};

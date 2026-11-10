#pragma once

#include <JuceHeader.h>
#include "SammyJUpdateCheck.h"
#include "SubbertoneLookAndFeel.h"
#include "Theme.h"

#include <functional>

/**
 * "Subbertone x.y.z is available": shown inside the editor when the update check finds a newer release.
 *
 * It covers the whole editor (a dim backdrop that takes every click, so the controls under it can't be
 * used) with a card in the middle, drawn like the editor's alerts: panel, accent dot and title in the
 * header row, the message, then DOWNLOAD (the primary action), LATER and SKIP THIS VERSION.
 * Return is DOWNLOAD (or the focused button), Escape is LATER; Tab cycles through the card's buttons.
 *
 * The owner (the editor) sizes it to its bounds and deletes it in onClose, which is called
 * asynchronously after a button, so the popup is never deleted from inside its own callback.
 */
class UpdatePopup final : public juce::Component
{
public:
    UpdatePopup(const SubbertoneLookAndFeel& lookAndFeel, sammyj::UpdateInfo info, std::function<void()> onClose);
    ~UpdatePopup() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void mouseDown(const juce::MouseEvent&) override {} // the backdrop takes the click; nothing happens

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    juce::Rectangle<int> cardBounds() const;
    void download();
    void later();
    void skip();
    void close();

    const SubbertoneLookAndFeel& m_lookAndFeel;
    const sammyj::UpdateInfo m_info;
    const std::function<void()> m_onClose;
    const juce::String m_title, m_message;
    const juce::Colour m_accent;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;

    juce::TextButton m_downloadButton { "Download" };
    juce::TextButton m_laterButton { "Later" };
    juce::TextButton m_skipButton { "Skip this version" };
    bool m_closing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UpdatePopup)
};

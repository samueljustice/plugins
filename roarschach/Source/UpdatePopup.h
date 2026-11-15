#pragma once

#include <JuceHeader.h>
#include "RoarschachLookAndFeel.h"
#include "Theme.h"

#include "SammyJUpdateCheck.h"

#include <functional>

/**
 * "A new version is available", drawn inside the editor: a dim backdrop over the whole editor and a
 * card in the middle, styled like the editor's alerts (raised paper, title in the display face, a row of
 * chips with the Return-key action filled with the dyed ink).
 *
 * The editor owns it, keeps it the size of the editor (the card stays centred) and deletes it when
 * onClose is called. Clicks on the backdrop do nothing, as with the editor's alerts. Escape is Later;
 * Return is Download (or the button that has the keyboard focus).
 */
class UpdatePopup : public juce::Component
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UpdatePopup)

public:
    /** onClose is called (asynchronously, after a button's action) when the popup should be deleted. */
    UpdatePopup(RoarschachLookAndFeel& lookAndFeel, sammyj::UpdateInfo info, std::function<void()> onClose);
    ~UpdatePopup() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    juce::Rectangle<int> getCardBounds() const;
    int buttonWidth() const;

private:
    /** A chip like the alerts' buttons; the primary one is filled with the dyed ink. */
    class DialogButton : public juce::TextButton
    {
    public:
        DialogButton(const juce::String& buttonText, bool primary);
        void paintButton(juce::Graphics&, bool isMouseOverButton, bool isButtonDown) override;

    private:
        const bool m_primary;
        juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    };

    void close();
    void download();
    void later();
    void skip();

    RoarschachLookAndFeel& m_lookAndFeel;
    const sammyj::UpdateInfo m_info;
    const std::function<void()> m_onClose;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;

    DialogButton m_downloadButton { "Download", true };
    DialogButton m_laterButton { "Later", false };
    DialogButton m_skipButton { "Skip this version", false };
    bool m_closing = false;
};

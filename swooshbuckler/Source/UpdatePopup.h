#pragma once

#include <JuceHeader.h>
#include "SammyJUpdateCheck.h"
#include "Theme.h"

/**
 * "A new version is available": dims the whole ship and shows a riveted card like the About and Settings panels,
 * with DOWNLOAD (lit, opens the release page), LATER and SKIP THIS VERSION. Escape or a click outside the card is
 * Later, Return is Download. Laid out at the editor's design size and scaled with it.
 */
class UpdatePopup : public juce::Component
{
public:
    explicit UpdatePopup(const Theme::Fonts& fonts);

    /** Fills in the versions, shows the popup and takes the keyboard focus. */
    void show(const sammyj::UpdateInfo& info);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    void close();

    const Theme::Fonts& m_fonts;
    sammyj::UpdateInfo m_info;
    juce::TextButton m_download { "DOWNLOAD" }, m_later { "LATER" }, m_skip { "SKIP THIS VERSION" };
    juce::Rectangle<int> m_panel, m_textArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UpdatePopup)
};

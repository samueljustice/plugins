#pragma once

#include <JuceHeader.h>
#include "Theme.h"
#include "SammyJUpdateCheck.h"

/**
 * "A new version is available", drawn as an overlay inside the editor like the About panel: a dim
 * scrim over the whole plug-in and a centred card in the theme's colours. It scales with the editor
 * and can't outlive it.
 *
 * DOWNLOAD opens the release page, LATER closes it (as do Escape and a click outside the card), and
 * SKIP THIS VERSION closes it and never offers that version again.
 */
class UpdatePopup : public juce::Component
{
public:
    explicit UpdatePopup(const Theme::Fonts& fonts);

    void show(const sammyj::UpdateInfo& info);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    juce::Rectangle<int> cardBounds() const;
    void close();

    const Theme::Fonts& m_fonts;
    sammyj::UpdateInfo m_info;
    juce::TextButton m_downloadButton;
    juce::TextButton m_laterButton;
    juce::TextButton m_skipButton;
    juce::Component::SafePointer<juce::Component> m_focusBeforeShow;   // given back on close (e.g. the About card's)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UpdatePopup)
};

#pragma once

#include <JuceHeader.h>
#include "PitchFlattenerLookAndFeel.h"
#include "SammyJUpdateCheck.h"
#include "Theme.h"

#include <functional>

/**
 * "A new version is available", drawn inside the editor like the plug-in's dialogs (a panel with an accent dot,
 * its title in capitals, a centred message and a row of pill buttons), over a dimmed backdrop that covers the
 * whole editor. It is a child of the editor, not a window, so it never outlives it and needs no native window.
 *
 * Like the dialogs: Return downloads, Escape is Later, a click on the backdrop does nothing, and Tab moves
 * between the buttons only. The card follows the editor's scale (as its menus do), never smaller than at the
 * editor's default size unless the editor is too small for it.
 *
 * onDismiss is called (after the chosen action) when the user closes it; the owner deletes it there.
 */
class UpdatePopup final : public juce::Component
{
public:
    UpdatePopup (const sammyj::UpdateInfo& info, std::function<void()> onDismiss);

    /** The editor's content scale (1 at its default size), so the card follows it. */
    void setEditorScale (float scale);

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class Card final : public juce::Component
    {
    public:
        explicit Card (const sammyj::UpdateInfo& info);

        void paint (juce::Graphics&) override;
        void resized() override;

        juce::TextButton m_download { "Download" }, m_later { "Later" }, m_skip { "Skip this version" };

    private:
        juce::SharedResourcePointer<Theme::Fonts> m_fonts;
        const juce::String m_title, m_message;
    };

    void dismiss();

    const sammyj::UpdateInfo m_info;
    const std::function<void()> m_onDismiss;
    Card m_card;
    float m_editorScale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdatePopup)
};

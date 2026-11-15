#pragma once

#include <JuceHeader.h>

#include "SammyJUpdateCheck.h"
#include "Theme.h"

#include <functional>

/** "Sound Advice x.y.z is available": a small printed card over a dimmed page, drawn inside the
    editor in the page's own paper, ink and type. Download opens the release page, Later closes it,
    Skip this version stops it being offered again. Escape is Later; Return is Download unless
    another button has focus. */
class UpdatePopup : public juce::Component
{
public:
    explicit UpdatePopup (sammyj::UpdateInfo info);

    /** Called after any of the three buttons (or Escape); the owner hides and deletes the popup. */
    std::function<void()> onDismiss;

    void setPalette (const Theme::Palette& p);

    /** Gives the Download button keyboard focus, if the editor is on screen. */
    void focusDefaultButton();

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    /** A line of small capitals; the primary one sits in an accent hairline box. */
    class TextButton : public juce::Button
    {
    public:
        TextButton (const juce::String& label, bool primary);
        void setPalette (const Theme::Palette& p) { palette = p; repaint(); }
        int idealWidth() const;
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        void focusGained (FocusChangeType) override { repaint(); }
        void focusLost (FocusChangeType) override { repaint(); }

    private:
        bool primary;
        Theme::Palette palette = Theme::palette (false);
    };

    juce::Rectangle<int> cardBounds() const;
    void dismiss();

    sammyj::UpdateInfo info;
    Theme::Palette palette = Theme::palette (false);
    TextButton download { "Download", true };
    TextButton later { "Later", false };
    TextButton skip { "Skip this version", false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdatePopup)
};

#pragma once

#include <JuceHeader.h>
#include "Theme.h"

/**
 * About box, drawn as an overlay inside the editor (so it scales with it, never
 * floats above other apps, and cannot outlive the plugin). Click outside the
 * card or on the close button to dismiss.
 *
 * The update check runs on its own thread and is cancellable: destroying the
 * panel cancels the network request before joining the thread, so closing the
 * plugin never waits on a slow network.
 */
class AboutPanel : public juce::Component
{
public:
    explicit AboutPanel(const Theme::Fonts& fonts);
    ~AboutPanel() override;

    void show();

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    class UpdateChecker : public juce::Thread
    {
    public:
        explicit UpdateChecker(juce::Component::SafePointer<AboutPanel> owner);
        ~UpdateChecker() override;

        void run() override;
        void cancel();

    private:
        juce::Component::SafePointer<AboutPanel> m_owner;
        juce::CriticalSection m_lock;
        juce::WebInputStream* m_stream = nullptr;   // guarded by m_lock
    };

    juce::Rectangle<int> cardBounds() const;
    void checkForUpdates();
    void finishUpdateCheck(const juce::String& message, juce::Colour colour);
    void showStatus(const juce::String& text, juce::Colour colour);

    const Theme::Fonts& m_fonts;
    bool m_returnFocusAfterCheck = false;   // CHECK FOR UPDATES had the keyboard focus when pressed
    juce::TextButton m_closeButton;
    juce::TextButton m_websiteButton;
    juce::TextButton m_emailButton;
    juce::TextButton m_checkUpdatesButton;
    juce::Label m_updateStatusLabel;
    juce::TextEditor m_licenseText;
    std::unique_ptr<UpdateChecker> m_updateChecker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutPanel)
};

namespace Version
{
    /** "1.10.2" -> comparable integer; missing parts count as 0. */
    int toNumber(const juce::String& version);

    /** The newest Reversinator version in a GitHub releases listing (parsed JSON),
        ignoring prereleases and other plugins' tags; empty if there is none. */
    juce::String latestRelease(const juce::var& releases);
}

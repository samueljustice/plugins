#pragma once

#include <JuceHeader.h>
#include "Theme.h"

/**
 * About window. Owned by the editor (never self-deleting), so it cannot
 * outlive the plugin; closing it just hides it.
 */
class AboutWindow : public juce::DocumentWindow
{
public:
    explicit AboutWindow (juce::LookAndFeel& lookAndFeel);
    ~AboutWindow() override;

    void closeButtonPressed() override;

private:
    /** Fetches the GitHub releases list on its own thread; joined on destruction. */
    class UpdateChecker : private juce::Thread, private juce::AsyncUpdater
    {
    public:
        UpdateChecker();
        ~UpdateChecker() override;

        /** Called on the message thread with (message, colour) when a check finishes. */
        std::function<void (const juce::String&, juce::Colour)> onResult;

        void start();
        bool isChecking() const { return isThreadRunning(); }

    private:
        void run() override;
        void handleAsyncUpdate() override;
        void post (const juce::String& message, juce::Colour colour);

        juce::CriticalSection m_lock;
        juce::String m_message;
        juce::Colour m_colour;
    };

    class AboutContent : public juce::Component
    {
    public:
        AboutContent();

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        juce::SharedResourcePointer<Theme::Fonts> m_fonts;

        juce::TextButton m_websiteButton, m_emailButton, m_checkUpdatesButton;
        juce::Label m_updateStatusLabel;
        juce::TextEditor m_creditsText;
        UpdateChecker m_updateChecker;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutContent)
    };

    AboutContent m_content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutWindow)
};

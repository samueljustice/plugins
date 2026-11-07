#pragma once

#include <JuceHeader.h>
#include "Theme.h"

/**
 * About box. Owned by the editor (so it can never outlive the plugin); closing
 * it only hides it. The update check runs on a joinable thread that is stopped
 * before the content is destroyed, and posts its result back through a
 * SafePointer, so nothing touches a deleted component.
 */
class AboutWindow : public juce::DocumentWindow
{
public:
    AboutWindow();
    ~AboutWindow() override = default;

    void closeButtonPressed() override;

private:
    class AboutContent : public juce::Component
    {
    public:
        AboutContent();
        ~AboutContent() override;

        void paint(juce::Graphics& g) override;
        void resized() override;

    private:
        class UpdateChecker : public juce::Thread
        {
        public:
            explicit UpdateChecker(juce::Component::SafePointer<AboutContent> owner);
            void run() override;

        private:
            juce::Component::SafePointer<AboutContent> m_owner;
        };

        void checkForUpdates();
        void showStatus(const juce::String& text, juce::Colour colour);

        juce::SharedResourcePointer<Theme::Fonts> m_fonts;
        juce::TextButton m_websiteButton;
        juce::TextButton m_emailButton;
        juce::TextButton m_checkUpdatesButton;
        juce::Label m_updateStatusLabel;
        juce::TextEditor m_licenseText;
        std::unique_ptr<UpdateChecker> m_updateChecker;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutContent)
    };

    AboutContent m_content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutWindow)
};

namespace Version
{
    /** "1.10.2" -> comparable integer; missing parts count as 0. */
    int toNumber(const juce::String& version);
}

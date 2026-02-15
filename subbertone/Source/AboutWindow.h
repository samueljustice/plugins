#pragma once

#include <JuceHeader.h>

class AboutWindow : public juce::DocumentWindow
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutWindow)

public:
    AboutWindow();
    ~AboutWindow() = default;
    
    void closeButtonPressed() override;
    
private:
    class AboutContent : public juce::Component
    {
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutContent)

    public:
        AboutContent();

        void paint(juce::Graphics& g) override;
        void resized() override;
        
    private:
        void checkForUpdates();

        juce::TextButton m_websiteButton;
        juce::TextButton m_emailButton;
        juce::TextButton m_checkUpdatesButton;
        juce::Label m_updateStatusLabel;
        juce::TextEditor m_licenseText;
        
    };
    
    AboutContent m_content;
};

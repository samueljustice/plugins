#pragma once

#include <JuceHeader.h>

class AboutWindow : public juce::DocumentWindow
{
public:
    AboutWindow();
    ~AboutWindow() override;
    
    void closeButtonPressed() override;
    
private:
    class AboutContent : public juce::Component
    {
    public:
        AboutContent();
        void paint(juce::Graphics& g) override;
        void resized() override;
        
    private:
        juce::TextButton websiteButton;
        juce::TextButton emailButton;
        juce::TextButton checkUpdatesButton;
        juce::Label updateStatusLabel;
        juce::TextEditor licenseText;
        
        void checkForUpdates();
        
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutContent)
    };
    
    std::unique_ptr<AboutContent> content;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutWindow)
};
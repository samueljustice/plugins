#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

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
        juce::Label titleLabel;
        juce::Label versionLabel;
        juce::Label authorLabel;
        juce::HyperlinkButton emailButton;
        juce::HyperlinkButton websiteButton;
        juce::TextButton checkUpdateButton{"Check for Updates"};
        juce::Label updateStatusLabel;
        juce::TextEditor licenseInfo;
        
        void checkForUpdates();
        
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutContent)
    };
    
    std::unique_ptr<AboutContent> content;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutWindow)
};

class ReversinatorAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    ReversinatorAudioProcessorEditor (ReversinatorAudioProcessor&);
    ~ReversinatorAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    ReversinatorAudioProcessor& audioProcessor;
    
    // Main controls
    juce::ToggleButton reverserButton;
    juce::Slider timeSlider;
    juce::Slider feedbackSlider;
    juce::Slider wetMixSlider;
    juce::Slider dryMixSlider;
    juce::ComboBox modeSelector;
    juce::Slider crossfadeSlider;
    
    // Labels
    juce::Label titleLabel;
    juce::Label reverserLabel;
    juce::Label timeLabel;
    juce::Label feedbackLabel;
    juce::Label wetMixLabel;
    juce::Label dryMixLabel;
    juce::Label modeLabel;
    juce::Label crossfadeLabel;
    
    // Value labels
    juce::Label timeValueLabel;
    juce::Label feedbackValueLabel;
    juce::Label wetMixValueLabel;
    juce::Label dryMixValueLabel;
    juce::Label crossfadeValueLabel;
    
    // Website link
    juce::HyperlinkButton websiteLink;
    
    // About button
    juce::TextButton aboutButton{"About"};
    std::unique_ptr<AboutWindow> aboutWindow;
    
    // Attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> reverserAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> timeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> feedbackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> wetMixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> dryMixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> crossfadeAttachment;
    
    // Scaling
    static constexpr int defaultWidth = 500;
    static constexpr int defaultHeight = 380;
    float currentScale = 1.0f;
    
    // Look and feel
    juce::LookAndFeel_V4 lookAndFeel;
    
    void setupSlider(juce::Slider& slider, juce::Label& label, juce::Label& valueLabel, 
                     const juce::String& labelText, const juce::String& suffix = "");

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReversinatorAudioProcessorEditor)
};
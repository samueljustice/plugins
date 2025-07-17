#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class ModeButton : public juce::Component
{
public:
    ModeButton(const juce::String& text) : buttonText(text) {}
    
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent&) override;
    
    std::function<void()> onClick;
    void setSelected(bool shouldBeSelected) { isSelected = shouldBeSelected; repaint(); }
    bool getSelected() const { return isSelected; }
    
private:
    juce::String buttonText;
    bool isSelected = false;
};

class HourglassDisplay : public juce::Component, public juce::Timer
{
public:
    HourglassDisplay();
    void paint(juce::Graphics& g) override;
    void timerCallback() override;
    void setTimeValue(float seconds) { timeValue = seconds; repaint(); }
    
private:
    float timeValue = 2.0f;
    float animationPhase = 0.0f;
};

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

class ReversinatorAudioProcessorEditor : public juce::AudioProcessorEditor, public juce::Slider::Listener
{
public:
    ReversinatorAudioProcessorEditor (ReversinatorAudioProcessor&);
    ~ReversinatorAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void sliderValueChanged(juce::Slider* slider) override;

private:
    ReversinatorAudioProcessor& audioProcessor;
    
    juce::ToggleButton reverserButton;
    juce::Slider timeSlider;
    juce::Slider feedbackSlider;
    juce::Slider wetMixSlider;
    juce::Slider dryMixSlider;
    juce::Slider volumeSlider;
    
    ModeButton reversePlaybackButton{"Reverse Playback"};
    ModeButton forwardBackwardsButton{"Forward Backwards"};
    ModeButton reverseRepeatButton{"Reverse Repeat"};
    
    HourglassDisplay hourglassDisplay;
    
    juce::Label timeLabel;
    juce::Label feedbackLabel;
    juce::Label wetMixLabel;
    juce::Label dryMixLabel;
    juce::Label volumeLabel;
    juce::Label timeValueLabel;
    
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> reverserAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> timeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> feedbackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> wetMixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> dryMixAttachment;
    
    void updateModeButtons(int mode);
    
    // Website link
    juce::HyperlinkButton websiteLink;
    
    // About button
    juce::TextButton aboutButton{"About"};
    std::unique_ptr<AboutWindow> aboutWindow;
    
    // Scaling
    static constexpr int defaultWidth = 600;
    static constexpr int defaultHeight = 500;
    float currentScale = 1.0f;
    
    juce::Colour backgroundColour{0xff9b8e7e};
    juce::Colour panelColour{0xffc4b5a0};
    juce::Colour darkPanelColour{0xff7a6e60};
    juce::Colour textColour{0xff000000};
    juce::Colour lightTextColour{0xffffffff};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReversinatorAudioProcessorEditor)
};
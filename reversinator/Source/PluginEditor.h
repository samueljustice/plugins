#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#include "Controls.h"
#include "SnippetDisplay.h"
#include "AboutPanel.h"
#include "UpdatePopup.h"
#include "SammyJUpdateCheck.h"

/**
 * Laid out at a fixed base size and scaled uniformly, so the window can be
 * resized (keeping its aspect ratio) without re-flowing anything.
 */
class ReversinatorAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit ReversinatorAudioProcessorEditor(ReversinatorAudioProcessor&);
    ~ReversinatorAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    /** Shows the "new version available" card over the editor (called by the update check). */
    void showUpdatePopup(const sammyj::UpdateInfo&);

    static constexpr int baseWidth = 820;
    static constexpr int baseHeight = 560;

private:
    class Content : public juce::Component, private juce::Value::Listener
    {
    public:
        Content(ReversinatorAudioProcessor&, const Theme::Fonts&);
        ~Content() override;

        void paint(juce::Graphics&) override;
        void resized() override;

        void showUpdatePopup(const sammyj::UpdateInfo& info) { m_updatePopup.show(info); }

    private:
        void valueChanged(juce::Value&) override;

        struct Section
        {
            juce::String title;
            juce::Colour accent;
            juce::Rectangle<int> bounds;
        };

        struct Caption
        {
            juce::String text;
            juce::Rectangle<int> bounds;
        };

        void setupSlider(juce::Slider&, Controls::SliderStyle, const juce::String& paramId, juce::Colour accent,
                         const juce::String& tooltip, std::function<juce::String(double)> format);

        ReversinatorAudioProcessor& m_processor;
        const Theme::Fonts& m_fonts;

        SnippetDisplay m_display;
        Controls::ModeSelector m_modeSelector;
        Controls::ReverserButton m_reverserButton;
        juce::Slider m_timeSlider, m_feedbackSlider, m_wetSlider, m_drySlider;
        juce::TextButton m_aboutButton;

        // Attachments after the controls they bind, so they are destroyed first.
        juce::AudioProcessorValueTreeState::ButtonAttachment m_reverserAttachment;
        juce::AudioProcessorValueTreeState::SliderAttachment m_timeAttachment;
        juce::AudioProcessorValueTreeState::SliderAttachment m_feedbackAttachment;
        juce::AudioProcessorValueTreeState::SliderAttachment m_wetAttachment;
        juce::AudioProcessorValueTreeState::SliderAttachment m_dryAttachment;

        AboutPanel m_about;           // overlays, added last so they sit on top
        UpdatePopup m_updatePopup;

        std::array<Section, 3> m_sections;
        std::array<Caption, 4> m_captions;
        juce::Rectangle<int> m_reverserLabelBounds;
    };

    ReversinatorAudioProcessor& m_processor;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    Controls::LookAndFeel m_lookAndFeel;
    Content m_content;
    juce::TooltipWindow m_tooltipWindow;
    bool m_constructed = false;   // don't record the window size until construction has finished

    // Last, so it is destroyed first: its destructor stops the check before the popup it calls goes away.
    std::unique_ptr<sammyj::UpdateCheck> m_updateCheck;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReversinatorAudioProcessorEditor)
};

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#include "WaveformVisualizer.h"

#include "HarmonicStrip.h"

/**
 * The editor is laid out once at a fixed design size (c_baseWidth x c_baseHeight)
 * inside a single content component, which is then scaled uniformly with an
 * AffineTransform. Resizing therefore scales text, knobs and the OpenGL view
 * together and nothing reflows; the chosen size is saved with the plugin state.
 */
class SubbertoneAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubbertoneAudioProcessorEditor)

public:
    explicit SubbertoneAudioProcessorEditor(SubbertoneAudioProcessor& audioProcessor);
    ~SubbertoneAudioProcessorEditor() override;

    void resized() override;
    void paint(juce::Graphics&) override;
    void timerCallback() override;

    static constexpr int c_baseWidth = 1600;
    static constexpr int c_baseHeight = 780;

private:
    enum class Format { Percent, Hertz, Decibels, Octaves, Milliseconds, Signed };

    // Everything visible lives in here, at design size; the editor just scales it.
    struct Content : public juce::Component
    {
        std::function<void(juce::Graphics&)> onPaint;
        std::function<void()> onResized;
        void paint(juce::Graphics& g) override { if (onPaint) onPaint(g); }
        void resized() override { if (onResized) onResized(); }
    };

    void paintContent(juce::Graphics& g);
    void layoutContent();

    void setupKnob(juce::Slider& slider, juce::Label& label, const juce::String& name, const juce::String& paramId,
                   const juce::String& tooltip, juce::Colour accent, Format format);
    void setupCaption(juce::Label& label, const juce::String& text);
    void layoutKnobRow(juce::Rectangle<int> area, std::initializer_list<std::pair<juce::Slider*, juce::Label*>> knobs);
    void setupChoice(juce::ComboBox& combo, const juce::String& paramId, const juce::String& tooltip,
                     std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& attachment);
    void showAboutWindow();

    // Harmonic patterns (one-shot actions that set the 16 harmonic bars)
    void showPatternMenu();
    void applyHarmonicPattern(const std::array<float, 17>& percent, bool keepSubLevel);

    // Presets
    void setupPresetBar();
    void showPresetMenu();
    void promptSavePreset(const juce::String& suggestedName);
    void confirmOverwrite(const juce::String& name);
    void savePresetAs(const juce::String& name);
    void showAlert(std::unique_ptr<juce::AlertWindow> alert, std::function<void(int, juce::AlertWindow&)> onResult);
    void updatePresetDisplay();

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        explicit LookAndFeel(const Theme::Fonts& fonts);

        void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                              float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
        juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;
        juce::Label* createSliderTextBox(juce::Slider&) override;

        juce::Font getLabelFont(juce::Label&) override;

        void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                          int buttonW, int buttonH, juce::ComboBox&) override;
        juce::Font getComboBoxFont(juce::ComboBox&) override;
        void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
        juce::Font getPopupMenuFont() override;

        void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                  bool isMouseOverButton, bool isButtonDown) override;
        juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;

    private:
        const Theme::Fonts& m_fonts;
    };

    struct Section
    {
        juce::String title;
        juce::Colour accent;
        juce::Rectangle<int> bounds;
    };

    SubbertoneAudioProcessor& m_audioProcessor;

    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    LookAndFeel m_lookAndFeel;

    Content m_content;
    WaveformVisualizer m_waveformVisualizer;

    std::unique_ptr<HarmonicStrip> m_harmonicStrip;
    juce::Rectangle<int> m_stripPanel;       // panel around the harmonic strip
    juce::Rectangle<int> m_masterHeader;     // repainted for the gain-reduction readout
    float m_lastGainReductionDb = 0.0f;
    bool m_sizeRestored = false;  // don't save the size until the saved one has been applied

    juce::Slider m_mixSlider, m_distortionSlider, m_toneSlider, m_postDriveLowpassSlider,
                 m_outputGainSlider, m_pitchThresholdSlider, m_fundamentalLimitSlider,
                 m_resonanceSlider, m_envAmountSlider, m_envReleaseSlider,
                 m_glideSlider, m_overtoneAmountSlider,
                 m_inflateSlider, m_inflateCurveSlider, m_ceilingSlider, m_limiterReleaseSlider,
                 m_loCutSlider;

    juce::Label m_mixLabel, m_distortionLabel, m_toneLabel, m_postDriveLowpassLabel,
                m_outputGainLabel, m_pitchThresholdLabel, m_fundamentalLimitLabel,
                m_resonanceLabel, m_envAmountLabel, m_envReleaseLabel,
                m_glideLabel, m_overtoneAmountLabel,
                m_inflateLabel, m_inflateCurveLabel, m_ceilingLabel, m_limiterReleaseLabel, m_loCutLabel,
                m_distortionTypeLabel, m_patternLabel;

    juce::ComboBox m_distortionTypeCombo;
    juce::ComboBox m_filterModeCombo;
    juce::TextButton m_patternButton;
    juce::TextButton m_limiterButton;
    juce::TextButton m_loCutButton;
    juce::Rectangle<int> m_outputHeader;

    // Attachments are declared after the controls they bind so they are destroyed first.
    juce::AudioProcessorValueTreeState::SliderAttachment   m_mixAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_distortionAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_toneAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_postDriveLowpassAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_outputGainAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_pitchThresholdAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_fundamentalLimitAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_resonanceAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_envAmountAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_envReleaseAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_glideAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_overtoneAmountAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_inflateAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_inflateCurveAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_ceilingAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_limiterReleaseAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment   m_limiterAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment   m_loCutAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment   m_loCutButtonAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_distortionTypeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_filterModeAttachment;

    juce::TextButton m_aboutButton;
    juce::HyperlinkButton m_websiteLink { "SWEETJUSTICESOUND.COM", juce::URL("https://sweetjusticesound.com") };

    juce::TextButton m_presetPrevButton, m_presetNameButton, m_presetNextButton, m_presetSaveButton;
    std::unique_ptr<juce::AlertWindow> m_alert; // owned so it never outlives the LookAndFeel
    juce::TooltipWindow m_tooltipWindow;

    std::array<Section, 6> m_sections;
};

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "SubbertoneLookAndFeel.h"
#include "Theme.h"
#include "WaveformVisualizer.h"

#include "HarmonicStrip.h"

#include <memory>

class AboutWindow;
class UpdatePopup;

namespace sammyj
{
    class UpdateCheck;
    struct UpdateInfo;
}

/**
 * The editor is laid out once at a fixed design size (c_baseWidth x c_baseHeight)
 * inside a single content component, which is then scaled uniformly with an
 * AffineTransform. Resizing therefore scales text, knobs and the OpenGL view
 * together and nothing reflows; the chosen size is saved with the plugin state.
 *
 * Everything the editor opens (menus, alerts, tooltips, the About window) is drawn with its
 * SubbertoneLookAndFeel, and is owned by it or dismissed when it is deleted, so nothing outlives
 * it or its LookAndFeel.
 */
class SubbertoneAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubbertoneAudioProcessorEditor)

    friend struct SubbertoneEditorInspector; // lets a test harness reach the tooltip window; not defined in the plugin

public:
    explicit SubbertoneAudioProcessorEditor(SubbertoneAudioProcessor& audioProcessor);
    ~SubbertoneAudioProcessorEditor() override;

    void resized() override;
    void paint(juce::Graphics&) override;
    void timerCallback() override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;

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

    // New-version popup: the check starts once the editor is on screen, and calls showUpdatePopup()
    // (message thread) if a newer release is out.
    void startUpdateCheckWhenShown();
    void showUpdatePopup(const sammyj::UpdateInfo& info);
    void closeUpdatePopup();

    // Harmonic patterns (one-shot actions that set the 16 harmonic bars)
    void showPatternMenu();
    void applyHarmonicPattern(const std::array<float, 17>& percent, bool keepSubLevel);

    // Presets
    void setupPresetBar();
    void showPresetMenu();
    void promptSavePreset(const juce::String& suggestedName);
    void checkNameAndSave(const juce::String& name);
    void confirmOverwrite(const juce::String& name, const juce::String& replacedName);
    void confirmDeletePreset(const juce::File& file, const juce::String& displayName);
    void savePresetAs(const juce::String& name);
    std::unique_ptr<juce::AlertWindow> makeAlert(const juce::String& title, const juce::String& message,
                                                 juce::MessageBoxIconType icon);
    void showAlert(std::unique_ptr<juce::AlertWindow> alert, std::function<void(int, juce::AlertWindow&)> onResult);
    void updatePresetDisplay();

    // MASTER header: repaints the gain-reduction readout when it appears, goes or moves.
    void updateLimiterReadout();

    /** Tooltips in a desktop window of their own: the scope's OpenGL view is a native view on
        top of the editor's window, so it would cover any part of a tip the editor paints itself. */
    class EditorTooltipWindow final : public juce::TooltipWindow
    {
    public:
        explicit EditorTooltipWindow(juce::Component& editor);

        // Only this editor's controls: another instance's editor has its own tooltip window.
        juce::String getTipFor(juce::Component&) override;
        // The editor's scale, not that of its design-size content, so tips keep their size.
        float getDesktopScaleFactor() const override;

    private:
        juce::Component& m_editor;
    };

    struct Section
    {
        juce::String title;
        juce::Colour accent;
        juce::Rectangle<int> bounds;
    };

    SubbertoneAudioProcessor& m_audioProcessor;

    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    SubbertoneLookAndFeel m_lookAndFeel; // before every component, menu, alert and window that uses it

    Content m_content;
    WaveformVisualizer m_waveformVisualizer;

    std::unique_ptr<HarmonicStrip> m_harmonicStrip;
    juce::Rectangle<int> m_stripPanel;       // panel around the harmonic strip
    juce::Rectangle<int> m_masterHeader;     // repainted for the gain-reduction readout
    float m_lastGainReductionDb = 0.0f;      // the reading the readout shows
    bool m_limiterReadoutShown = false;      // whether the last repaint asked for showed the readout
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
    juce::TextButton m_phaseAlignButton;     // in the HARMONICS header
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
    juce::AudioProcessorValueTreeState::ButtonAttachment   m_phaseAlignAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_distortionTypeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_filterModeAttachment;

    juce::TextButton m_aboutButton;
    ThemeLink m_websiteLink { "SWEETJUSTICESOUND.COM", juce::URL("https://sweetjusticesound.com") };

    juce::TextButton m_presetPrevButton, m_presetNameButton, m_presetNextButton, m_presetSaveButton;
    std::unique_ptr<juce::AlertWindow> m_alert; // owned so it never outlives the LookAndFeel
    EditorTooltipWindow m_tooltipWindow { *this }; // after m_lookAndFeel, so it is deleted first
    std::unique_ptr<AboutWindow> m_aboutWindow;     // at most one per editor, closed with it
    std::unique_ptr<UpdatePopup> m_updatePopup;     // the new-version popup, over everything in the editor
    std::unique_ptr<sammyj::UpdateCheck> m_updateCheck; // after the popup: stopped before anything its callback uses goes

    std::array<Section, 6> m_sections;
};

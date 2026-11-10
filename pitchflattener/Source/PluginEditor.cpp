#include "PluginEditor.h"

#include "AboutWindow.h"
#include "PresetManager.h"
#include "SpectrogramVisualizer.h"
#include "UpdatePopup.h"

namespace
{
    constexpr int c_width  = PitchFlattenerAudioProcessorEditor::baseWidth;
    constexpr int c_height = PitchFlattenerAudioProcessorEditor::baseHeight;
    constexpr int c_margin = 16;
    constexpr int c_headerHeight = 56;
    constexpr int c_sourceHeight = 46;
    constexpr int c_controlsHeight = 230;
    constexpr int c_helpHeight = 26;
    constexpr int c_gap = 10;
    constexpr int c_sectionGap = 12;
    constexpr int c_sectionHeaderHeight = 32;
    constexpr int c_labelHeight = 16;

#ifdef PLUGIN_VERSION
    const char* const c_version = PLUGIN_VERSION;
#else
    const char* const c_version = JucePlugin_VersionString;
#endif

    const char* const c_defaultHelp = "Hover over a control for help.  Double-click a knob to reset it, Alt-click a menu to reset it.";

    enum class Format { Percent, MixPercent, Hertz, HertzNote, Decibels, OctavesPerSecond, Milliseconds, Semitones };

    /** "A4", "c#3", "Bb2" -> Hz, or 0 when the text is not a note name. */
    double parseNoteName (const juce::String& text)
    {
        const auto t = text.trim();
        if (t.isEmpty())
            return 0.0;

        static const int pitchClasses[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
        const auto first = juce::CharacterFunctions::toUpperCase (t[0]);
        if (first < 'A' || first > 'G')
            return 0.0;

        int pc = pitchClasses[first - 'A'];
        int pos = 1;

        if (t[pos] == '#')      { ++pc; ++pos; }
        else if (t[pos] == 'b') { --pc; ++pos; }

        const auto octaveText = t.substring (pos).trim();
        if (octaveText.isEmpty() || ! octaveText.containsOnly ("-0123456789"))
            return 0.0;

        const int midi = (octaveText.getIntValue() + 1) * 12 + pc;
        return 440.0 * std::exp2 ((midi - 69) / 12.0);
    }

    double parseValue (const juce::String& text, Format format)
    {
        const auto t = text.trim();

        if (format == Format::Hertz || format == Format::HertzNote)
            if (const double hz = parseNoteName (t); hz > 0.0)
                return hz;

        double v = t.getDoubleValue(); // leading number; units and note names after it are ignored

        switch (format)
        {
            case Format::Hertz:
            case Format::HertzNote:
                if (t.containsIgnoreCase ("k"))
                    v *= 1000.0;
                break;
            case Format::Milliseconds:
                if (t.endsWithIgnoreCase ("s") && ! t.endsWithIgnoreCase ("ms"))
                    v *= 1000.0;
                break;
            case Format::MixPercent:
                v /= 100.0;
                break;
            case Format::Percent:
            case Format::Decibels:
            case Format::OctavesPerSecond:
            case Format::Semitones:
                break;
        }

        return v;
    }

    juce::String formatValue (double v, Format format)
    {
        switch (format)
        {
            case Format::Percent:          return juce::String (juce::roundToInt (v)) + "%";
            case Format::MixPercent:       return juce::String (juce::roundToInt (v * 100.0)) + "%";
            case Format::Hertz:            return Theme::formatHertz (v);
            case Format::HertzNote:        return Theme::formatHertz (v) + " " + Theme::noteName ((float) v);
            case Format::Decibels:         return Theme::signedText (v, " dB");
            case Format::OctavesPerSecond: return juce::String (v, v < 10.0 ? 1 : 0) + " oct/s";
            case Format::Milliseconds:     return v >= 1000.0 ? juce::String (v / 1000.0, 2) + " s"
                                                              : juce::String (juce::roundToInt (v)) + " ms";
            case Format::Semitones:        return juce::String (v, std::abs (v - std::round (v)) < 0.05 ? 0 : 1) + " st";
        }

        return juce::String (v);
    }

    void resetParameterToDefault (juce::RangedAudioParameter* param)
    {
        if (param == nullptr)
            return;

        param->beginChangeGesture();
        param->setValueNotifyingHost (param->getDefaultValue());
        param->endChangeGesture();
    }

    //==========================================================================
    /** ComboBox that resets its parameter on Alt-click (a double-click would open the menu). */
    class ResetComboBox : public juce::ComboBox
    {
    public:
        std::function<void()> onResetRequest;

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isAltDown() && onResetRequest != nullptr && isEnabled())
            {
                onResetRequest();
                return;
            }

            juce::ComboBox::mouseDown (e);
        }
    };

    //==========================================================================
    /** Vertical input level meter with the gate threshold marked. */
    class GateMeter : public juce::Component
    {
    public:
        void setLevels (float levelDb, float gateDb)
        {
            const float level = juce::jlimit (-80.0f, 0.0f, levelDb);
            // Compare the raw level too: below -80 dB the bar is pinned but the
            // readout text (and the "-inf" switch at -100 dB) still changes.
            if (std::abs (level - m_levelDb) > 0.05f || std::abs (levelDb - m_rawDb) > 0.05f
                || ! juce::approximatelyEqual (gateDb, m_gateDb))
            {
                m_levelDb = level;
                m_gateDb = gateDb;
                m_rawDb = levelDb;
                repaint();
            }
        }

        void paint (juce::Graphics& g) override
        {
            using namespace Theme::Colours;

            auto r = getLocalBounds().toFloat();
            auto textRow = r.removeFromBottom (18.0f);
            const auto bar = r.reduced (0.0f, 6.0f).withSizeKeepingCentre (8.0f, r.getHeight() - 12.0f);
            const bool open = m_rawDb > m_gateDb;

            const auto toY = [&bar] (float db) { return bar.getBottom() - bar.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 80.0f) / 80.0f); };

            g.setColour (outline);
            g.fillRoundedRectangle (bar, 4.0f);
            g.setColour ((open ? good : textFaint).withAlpha (0.9f));
            g.fillRoundedRectangle (bar.withTop (toY (m_levelDb)), 4.0f);

            g.setColour (Theme::Colours::detect);
            const float gy = toY (m_gateDb);
            g.fillRoundedRectangle (juce::Rectangle<float> (bar.getX() - 5.0f, gy - 1.0f, bar.getWidth() + 10.0f, 2.0f), 1.0f);

            g.setFont (m_fonts->body (12.0f));
            g.setColour (open ? good : textDim);
            g.drawText (m_rawDb <= -100.0f ? juce::String ("-inf dB") : juce::String (m_rawDb, 1) + " dB",
                        textRow, juce::Justification::centred, false);
        }

    private:
        juce::SharedResourcePointer<Theme::Fonts> m_fonts;
        float m_levelDb = -80.0f, m_gateDb = -50.0f, m_rawDb = -120.0f;
    };
}

//==============================================================================
// Content: everything, laid out at the fixed base size
//==============================================================================
class PitchFlattenerAudioProcessorEditor::Content : public juce::Component, private juce::Timer
{
public:
    Content (PitchFlattenerAudioProcessor& processor, PitchFlattenerLookAndFeel& lookAndFeel);
    ~Content() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseEnter (const juce::MouseEvent&) override { updateHelp(); }
    void mouseMove (const juce::MouseEvent&) override  { updateHelp(); }
    void mouseExit (const juce::MouseEvent&) override  { updateHelp(); }

private:
    enum KnobId
    {
        targetPitchKnob, autoWindowKnob, amountKnob, mixKnob,      // FLATTEN
        minFreqKnob, maxFreqKnob, gateKnob,                         // RANGE
        sensitivityKnob, maxShiftKnob, highpassKnob, lowpassKnob,   // ADVANCED (Custom only)
        numKnobs
    };

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    struct Section
    {
        juce::String title;
        juce::Colour accent;
        juce::Rectangle<int> bounds;
    };

    void timerCallback() override;

    void setupKnob (KnobId id, const char* paramId, const juce::String& name, juce::Colour accent, Format format,
                    const juce::String& help);
    void setupChoice (ResetComboBox& combo, const char* paramId, juce::Colour accent, const juce::String& help,
                      std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& attachment);
    void setupToggle (juce::ToggleButton& button, juce::Label& label, const char* paramId, const juce::String& name,
                      juce::Colour accent, const juce::String& help,
                      std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>& attachment);
    void setupCaption (juce::Label& label, const juce::String& name);
    void setupSourceType();

    void userPickedSourceType (int index);
    void updateSourceBlurb();
    void updateEnablement();
    void showAbout();

    void setHelp (juce::Component& c, const juce::String& text) { m_help[&c] = text; }
    void updateHelp();
    juce::String findHelpFor (juce::Component* c) const;

    juce::Rectangle<int> bodyOf (const Section& s) const { return s.bounds.withTrimmedTop (c_sectionHeaderHeight).reduced (4, 4); }
    static void placeInCell (juce::Rectangle<int> cell, juce::Component& control, juce::Label& label, int controlHeight = 0);

    PitchFlattenerAudioProcessor& m_processor;
    PitchFlattenerLookAndFeel& m_lookAndFeel;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;

    PitchDisplay m_display;
    PresetBar m_presetBar;
    juce::TextButton m_aboutButton { "?" };

    // Source strip
    ResetComboBox m_sourceCombo, m_detectorCombo;
    std::unique_ptr<juce::ParameterAttachment> m_sourceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_detectorAttachment;
    juce::String m_sourceBlurb;
    pf::DetectorType m_shownActiveDetector = pf::DetectorType::NumDetectors;
    juce::Rectangle<int> m_sourceStrip, m_blurbArea, m_activeArea;

    // Knobs
    std::array<Knob, numKnobs> m_knobs;

    // Target
    ResetComboBox m_targetModeCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_targetModeAttachment;

    // Input
    GateMeter m_gateMeter;
    juce::Label m_levelLabel;

    // Output
    ResetComboBox m_qualityCombo;
    juce::Label m_qualityLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_qualityAttachment;
    juce::ToggleButton m_formantToggle;
    juce::Label m_formantLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_formantAttachment;

    std::array<Section, 3> m_sections;   // FLATTEN, RANGE, ADVANCED
    juce::Rectangle<int> m_helpArea;
    juce::String m_helpText { c_defaultHelp };
    std::map<const juce::Component*, juce::String> m_help;

    std::atomic<float>* m_targetModeValue = nullptr;
    std::atomic<float>* m_sourceTypeValue = nullptr;
    bool m_advancedShown = true;
    std::atomic<float>* m_gateValue = nullptr;

    std::unique_ptr<AboutWindow> m_aboutWindow; // at most one per editor, closed with it

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Content)
};

//==============================================================================
PitchFlattenerAudioProcessorEditor::Content::Content (PitchFlattenerAudioProcessor& processor,
                                                      PitchFlattenerLookAndFeel& lookAndFeel)
    : m_processor (processor)
    , m_lookAndFeel (lookAndFeel)
    , m_display (processor)
    , m_presetBar (processor.parameters, lookAndFeel)
{
    using namespace Theme::Colours;
    auto& params = m_processor.parameters;

    // Set our LookAndFeel before any child creates LookAndFeel-dependent parts
    // (slider value boxes, combo labels). This component is not yet inside the
    // editor here, so it would otherwise inherit the default LookAndFeel.
    setLookAndFeel (&m_lookAndFeel);

    m_targetModeValue = params.getRawParameterValue (ParamIDs::targetMode);
    m_sourceTypeValue = params.getRawParameterValue (ParamIDs::sourceType);
    m_gateValue = params.getRawParameterValue (ParamIDs::volumeThreshold);

    m_sections = { Section { "FLATTEN",  target,     {} },
                   Section { "RANGE",    detect,     {} },
                   Section { "ADVANCED", correction, {} } };

    addAndMakeVisible (m_display);
    addAndMakeVisible (m_presetBar);

    // The wobble warning's "Try ..." button picks a Source Type exactly as the Source menu does.
    m_display.onTrySourceType = [this] (int type) { userPickedSourceType (type); };

    m_aboutButton.onClick = [this] { showAbout(); };
    m_aboutButton.setTooltip ("About SammyJs Pitch Flattener");
    addAndMakeVisible (m_aboutButton);

    // --- Source strip ----------------------------------------------------
    setupSourceType();
    setupChoice (m_detectorCombo, ParamIDs::detector, detect,
                 "Detector: the pitch detection algorithm. Each Source Type picks one; switch Source Type to Custom to change it. "
                 "pYIN suits voices, MPM solo instruments, Spectral + MPM sirens, whistles and feedback, Harmonic Sum engines and drones.",
                 m_detectorAttachment);

    // --- Target ------------------------------------------------------------
    setupChoice (m_targetModeCombo, ParamIDs::targetMode, target,
                 "Target mode. Latch: lock to the first stable pitch of each sound. Fixed: flatten everything to Target Hz. "
                 "Auto: remove movement faster than the Auto Window but keep the overall contour.",
                 m_targetModeAttachment);

    setupKnob (targetPitchKnob, ParamIDs::targetPitch, "Target Hz", target, Format::HertzNote,
               "Target Hz (Fixed mode): the pitch everything is flattened to. Type a note name such as A4 to enter it directly.");
    setupKnob (autoWindowKnob, ParamIDs::autoWindow, "Auto Win", target, Format::Milliseconds,
               "Auto Window (Auto mode): pitch movement faster than this is removed, slower movement is kept. Longer = flatter.");
    setupKnob (amountKnob, ParamIDs::flattenAmount, "Amount", target, Format::Percent,
               "Amount: how much of the pitch deviation is removed. 100% = completely flat, 50% = half the movement.");
    setupKnob (mixKnob, ParamIDs::mix, "Mix", target, Format::MixPercent,
               "Mix: blend between the dry input and the flattened signal (both latency-aligned).");
    setupToggle (m_formantToggle, m_formantLabel, ParamIDs::rbFormantPreserve, "Formant", target,
                 "Formant: preserve the spectral envelope while shifting (keeps voices natural). Off is usually better for sirens and machines.",
                 m_formantAttachment);

    // --- Range ---------------------------------------------------------------
    setupKnob (minFreqKnob, ParamIDs::minFreq, "Min Freq", detect, Format::Hertz,
               "Min Freq: lowest pitch the detector looks for. Keep the range tight around the source for the most reliable tracking.");
    setupKnob (maxFreqKnob, ParamIDs::maxFreq, "Max Freq", detect, Format::Hertz,
               "Max Freq: highest pitch the detector looks for.");

    setupKnob (gateKnob, ParamIDs::volumeThreshold, "Gate", detect, Format::Decibels,
               "Gate: input below this level is treated as unpitched (the teal line on the level meter). Raise it to ignore room tone between sounds.");

    setupCaption (m_levelLabel, "Level");
    addAndMakeVisible (m_gateMeter);
    setHelp (m_gateMeter, "Live input level. Green when above the gate (teal line), so the detector is listening.");

    // --- Advanced: set by the Source Type, adjustable on Custom -------------------
    setupKnob (sensitivityKnob, ParamIDs::sensitivity, "Sensitivity", correction, Format::Percent,
               "Sensitivity (Custom): how readily frames are treated as pitched. Higher tracks quieter or noisier sounds; lower rejects more noise.");
    setupKnob (maxShiftKnob, ParamIDs::maxShift, "Max Shift", correction, Format::Semitones,
               "Max Shift (Custom): the largest correction allowed, in semitones. Larger deviations are only corrected this far.");
    setupKnob (highpassKnob, ParamIDs::detectionHighpass, "Highpass", correction, Format::Hertz,
               "Detection Highpass (Custom): filters rumble out of the signal the detector hears. The audio itself is not filtered.");
    setupKnob (lowpassKnob, ParamIDs::detectionLowpass, "Lowpass", correction, Format::Hertz,
               "Detection Lowpass (Custom): filters hiss and high harmonics out of the signal the detector hears. The audio itself is not filtered.");

    setupCaption (m_qualityLabel, "Quality");
    setupChoice (m_qualityCombo, ParamIDs::quality, correction,
                 "Shifter quality (Custom). Smooth: standard window, best for sustained tones. Tight: short window, better for fast movement and transients.",
                 m_qualityAttachment);

    setHelp (m_presetBar.getNameButton(), "Presets: one factory preset per Source Type, plus your own saved presets. "
                                          "Click for the menu: load, save, delete, reset to defaults, show the presets folder.");
    setHelp (m_presetBar.getPreviousButton(), "Previous preset: steps back through the factory presets, then your own, in menu order.");
    setHelp (m_presetBar.getNextButton(), "Next preset: steps through the factory presets, then your own, in menu order.");
    setHelp (m_presetBar.getSaveButton(), "Save the current settings as a user preset.");
    setHelp (m_aboutButton, "About Pitch Flattener: version, credits and update check.");

    // Help text for every child (and grandchild) under the mouse.
    addMouseListener (this, true);

    // Children were created before they had a parent (so with the default
    // LookAndFeel); rebuild their LookAndFeel-created parts with ours.
    sendLookAndFeelChange();

    updateEnablement();
    setSize (c_width, c_height);
    startTimerHz (30);
}

PitchFlattenerAudioProcessorEditor::Content::~Content()
{
    stopTimer();
    removeMouseListener (this);
    m_aboutWindow.reset();
    setLookAndFeel (nullptr);
}

//==============================================================================
void PitchFlattenerAudioProcessorEditor::Content::setupKnob (KnobId id, const char* paramId, const juce::String& name,
                                                             juce::Colour accent, Format format, const juce::String& help)
{
    auto& knob = m_knobs[(size_t) id];
    auto& slider = knob.slider;

    slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 92, 18);
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    // The attachment sets the range (with skew) from the parameter, and installs
    // the parameter's own text conversion, so ours must be set after it.
    knob.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (m_processor.parameters, paramId, slider);
    slider.textFromValueFunction = [format] (double v) { return formatValue (v, format); };
    slider.valueFromTextFunction = [format] (const juce::String& t) { return parseValue (t, format); };

    // Double-click (or Alt-click) resets; the slider brackets it in drag
    // notifications, so the attachment records it as one host gesture.
    if (auto* param = m_processor.parameters.getParameter (paramId))
        slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));

    slider.updateText();
    addAndMakeVisible (slider);

    // The value box was created before the slider had a parent (default
    // LookAndFeel); changing a colour makes the slider rebuild it with ours.
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    setupCaption (knob.label, name);
    setHelp (slider, help);
}

void PitchFlattenerAudioProcessorEditor::Content::setupChoice (ResetComboBox& combo, const char* paramId, juce::Colour accent,
                                                               const juce::String& help,
                                                               std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& attachment)
{
    auto* param = m_processor.parameters.getParameter (paramId);

    // Items must exist before the attachment so it can restore the selection.
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
        combo.addItemList (choice->choices, 1);

    combo.setColour (juce::ComboBox::arrowColourId, accent);
    combo.setColour (juce::ComboBox::focusedOutlineColourId, accent.withAlpha (0.6f));
    combo.onResetRequest = [param] { resetParameterToDefault (param); };
    addAndMakeVisible (combo);
    setHelp (combo, help);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (m_processor.parameters, paramId, combo);
}

void PitchFlattenerAudioProcessorEditor::Content::setupToggle (juce::ToggleButton& button, juce::Label& label, const char* paramId,
                                                               const juce::String& name, juce::Colour accent, const juce::String& help,
                                                               std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>& attachment)
{
    button.setColour (juce::ToggleButton::tickColourId, accent);
    button.setButtonText ({});
    addAndMakeVisible (button);
    setHelp (button, help);
    setupCaption (label, name);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (m_processor.parameters, paramId, button);
}

void PitchFlattenerAudioProcessorEditor::Content::setupCaption (juce::Label& label, const juce::String& name)
{
    label.setText (name.toUpperCase(), juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setColour (juce::Label::textColourId, Theme::Colours::textDim);
    label.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (label);
}

void PitchFlattenerAudioProcessorEditor::Content::setupSourceType()
{
    using namespace Theme::Colours;

    auto* param = m_processor.parameters.getParameter (ParamIDs::sourceType);
    jassert (param != nullptr);

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
        m_sourceCombo.addItemList (choice->choices, 1);

    m_sourceCombo.setColour (juce::ComboBox::arrowColourId, detect);
    m_sourceCombo.setColour (juce::ComboBox::focusedOutlineColourId, detect.withAlpha (0.6f));
    addAndMakeVisible (m_sourceCombo);
    setHelp (m_sourceCombo, "Source Type: what you are flattening. Picking one loads tuned settings for that kind of sound. "
                            "Flatten and Range stay adjustable; switch to Custom to adjust the Detector and the Advanced settings.");

    if (param == nullptr)
        return;

    // Not a ComboBoxAttachment: parameter -> combo updates must never count as
    // a user pick, otherwise loading a session or preset would overwrite the
    // detector settings. The attachment updates the combo silently; onChange
    // therefore only fires for the user's own menu selection.
    m_sourceAttachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float value)
    {
        m_sourceCombo.setSelectedItemIndex (juce::roundToInt (value), juce::dontSendNotification);
        updateSourceBlurb();
    });

    m_sourceCombo.onChange = [this] { userPickedSourceType (m_sourceCombo.getSelectedItemIndex()); };
    m_sourceCombo.onResetRequest = [this, param]
    {
        userPickedSourceType (juce::roundToInt (param->convertFrom0to1 (param->getDefaultValue())));
    };

    m_sourceAttachment->sendInitialUpdate();
}

void PitchFlattenerAudioProcessorEditor::Content::userPickedSourceType (int index)
{
    if (index < 0 || index >= (int) pf::SourceType::NumSourceTypes || m_sourceAttachment == nullptr)
        return;

    m_sourceAttachment->setValueAsCompleteGesture ((float) index);
    m_processor.applySourceTypeDefaults ((pf::SourceType) index);
    updateSourceBlurb();
}

void PitchFlattenerAudioProcessorEditor::Content::updateSourceBlurb()
{
    const int index = m_sourceCombo.getSelectedItemIndex();
    m_sourceBlurb = index >= 0 ? juce::String (pf::getSourcePreset ((pf::SourceType) index).blurb) : juce::String();
    repaint (m_blurbArea);
}

void PitchFlattenerAudioProcessorEditor::Content::updateEnablement()
{
    const int mode = m_targetModeValue != nullptr ? juce::roundToInt (m_targetModeValue->load()) : 0;
    const bool custom = m_sourceTypeValue != nullptr && juce::roundToInt (m_sourceTypeValue->load()) == (int) pf::SourceType::Custom;

    const auto enable = [] (juce::Component& c, juce::Label& label, bool on)
    {
        c.setEnabled (on);
        label.setEnabled (on);
    };

    auto& k = m_knobs;
    enable (k[targetPitchKnob].slider, k[targetPitchKnob].label, mode == (int) pf::TargetMode::Fixed);
    enable (k[autoWindowKnob].slider, k[autoWindowKnob].label, mode == (int) pf::TargetMode::Auto);

    // The Source Type sets these; they are for rescuing a sound on Custom.
    for (auto id : { sensitivityKnob, maxShiftKnob, highpassKnob, lowpassKnob })
        enable (k[(size_t) id].slider, k[(size_t) id].label, custom);
    enable (m_qualityCombo, m_qualityLabel, custom);
    m_detectorCombo.setEnabled (custom);
    if (custom != m_advancedShown)
    {
        m_advancedShown = custom;
        repaint (m_sections[2].bounds);
        repaint (m_sourceStrip);
    }
}

void PitchFlattenerAudioProcessorEditor::Content::timerCallback()
{
    updateEnablement();
    m_presetBar.refreshIfNameChanged();

    m_gateMeter.setLevels (m_processor.getInputLevelDb(), m_gateValue != nullptr ? m_gateValue->load() : -50.0f);

    const auto active = m_processor.getActiveDetector();
    if (active != m_shownActiveDetector)
    {
        m_shownActiveDetector = active;
        repaint (m_activeArea);
    }
}

void PitchFlattenerAudioProcessorEditor::Content::showAbout()
{
    // One About window per editor: another click brings it back to the front. The content owns it,
    // so it is closed (and its update check stopped) when the editor goes.
    if (m_aboutWindow != nullptr)
    {
        m_aboutWindow->setMinimised (false);
        m_aboutWindow->toFront (true);
        return;
    }

    m_aboutWindow = std::make_unique<AboutWindow> (m_lookAndFeel, [this] { m_aboutWindow.reset(); });
}

//==============================================================================
juce::String PitchFlattenerAudioProcessorEditor::Content::findHelpFor (juce::Component* c) const
{
    for (; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (c == &m_display)
            return m_display.getHelpTextAt (m_display.getMouseXYRelative());

        if (const auto it = m_help.find (c); it != m_help.end())
            return it->second;
    }

    return c_defaultHelp;
}

void PitchFlattenerAudioProcessorEditor::Content::updateHelp()
{
    // Look up whatever is under the mouse now (robust to enter/exit ordering).
    juce::Component* under = nullptr;
    const auto mouse = getMouseXYRelative();

    if (getLocalBounds().contains (mouse))
        under = getComponentAt (mouse);

    const auto text = findHelpFor (under);

    if (text != m_helpText)
    {
        m_helpText = text;
        repaint (m_helpArea);
    }
}

//==============================================================================
void PitchFlattenerAudioProcessorEditor::Content::placeInCell (juce::Rectangle<int> cell, juce::Component& control,
                                                               juce::Label& label, int controlHeight)
{
    label.setBounds (cell.removeFromTop (c_labelHeight));

    if (controlHeight > 0)
        control.setBounds (cell.withSizeKeepingCentre (cell.getWidth(), controlHeight));
    else
        control.setBounds (cell);
}

void PitchFlattenerAudioProcessorEditor::Content::resized()
{
    auto bounds = getLocalBounds();

    // Header: title (painted), preset bar (<  name  >  SAVE), about
    auto header = bounds.removeFromTop (c_headerHeight).reduced (c_margin, 0);
    m_aboutButton.setBounds (header.removeFromRight (26).withSizeKeepingCentre (26, 26));
    header.removeFromRight (14);
    m_presetBar.setBounds (header.removeFromRight (400).withSizeKeepingCentre (400, 26)); // as Subbertone's

    bounds.reduce (c_margin, 0);

    // Help bar
    m_helpArea = bounds.removeFromBottom (c_helpHeight + 6).withTrimmedBottom (6);

    // Source strip
    m_sourceStrip = bounds.removeFromTop (c_sourceHeight);
    {
        auto r = m_sourceStrip.reduced (12, 0);
        r.removeFromLeft (84); // "SOURCE" caption (painted)
        m_sourceCombo.setBounds (r.removeFromLeft (190).withSizeKeepingCentre (190, 26));
        r.removeFromLeft (14);

        m_activeArea = r.removeFromRight (104);
        r.removeFromRight (12);
        m_detectorCombo.setBounds (r.removeFromRight (150).withSizeKeepingCentre (150, 26));
        r.removeFromRight (86); // "DETECTOR" caption (painted)
        m_blurbArea = r.withTrimmedRight (10);
    }

    bounds.removeFromTop (c_gap);

    // Controls
    auto controls = bounds.removeFromBottom (c_controlsHeight);
    bounds.removeFromBottom (c_sectionGap);
    m_display.setBounds (bounds);

    // FLATTEN 4 + RANGE 2 + ADVANCED 4 units
    const int unit = (controls.getWidth() - 2 * c_sectionGap) / 10;
    m_sections[0].bounds = controls.removeFromLeft (unit * 4);
    controls.removeFromLeft (c_sectionGap);
    m_sections[1].bounds = controls.removeFromLeft (unit * 2);
    controls.removeFromLeft (c_sectionGap);
    m_sections[2].bounds = controls;

    const auto grid = [this] (const Section& s, int columns)
    {
        // Returns a lambda giving the cell at (column, row) of a 2-row grid.
        const auto body = bodyOf (s);
        const int cw = body.getWidth() / columns;
        const int ch = body.getHeight() / 2;
        return [body, cw, ch] (int col, int row, int span = 1)
        {
            return juce::Rectangle<int> (body.getX() + col * cw, body.getY() + row * ch, cw * span, ch);
        };
    };

    auto& k = m_knobs;
    const auto placeKnob = [&k] (KnobId id, juce::Rectangle<int> cell)
    {
        k[(size_t) id].label.setBounds (cell.removeFromTop (c_labelHeight));
        k[(size_t) id].slider.setBounds (cell);
    };

    // FLATTEN: target mode in the header; the target, how much, and how it sounds
    {
        const auto cell = grid (m_sections[0], 3);
        placeKnob (targetPitchKnob, cell (0, 0));
        placeKnob (autoWindowKnob, cell (1, 0));
        placeKnob (amountKnob, cell (2, 0));
        placeKnob (mixKnob, cell (0, 1));
        placeInCell (cell (1, 1), m_formantToggle, m_formantLabel, 26);

        auto row = m_sections[0].bounds.withHeight (c_sectionHeaderHeight).reduced (8, 5);
        m_targetModeCombo.setBounds (row.removeFromRight (std::min (120, row.getWidth() - 90)));
    }

    // RANGE: what to listen for in this particular file
    {
        const auto cell = grid (m_sections[1], 2);
        placeKnob (minFreqKnob, cell (0, 0));
        placeKnob (maxFreqKnob, cell (1, 0));
        placeKnob (gateKnob, cell (0, 1));
        placeInCell (cell (1, 1), m_gateMeter, m_levelLabel);
    }

    // ADVANCED: set by the Source Type, adjustable on Custom
    {
        const auto cell = grid (m_sections[2], 3);
        placeKnob (sensitivityKnob, cell (0, 0));
        placeKnob (maxShiftKnob, cell (1, 0));
        placeInCell (cell (2, 0).reduced (6, 0), m_qualityCombo, m_qualityLabel, 26);
        placeKnob (highpassKnob, cell (0, 1));
        placeKnob (lowpassKnob, cell (1, 1));
    }
}

void PitchFlattenerAudioProcessorEditor::Content::paint (juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll (background);

    // --- Header ----------------------------------------------------------------
    auto header = getLocalBounds().removeFromTop (c_headerHeight).reduced (c_margin, 0).toFloat();

    // Mark: a wobbling teal line that settles into a flat amber line.
    PitchFlattenerLookAndFeel::drawMark (g, header.removeFromLeft (34.0f).withSizeKeepingCentre (34.0f, 22.0f));

    header.removeFromLeft (12.0f);

    const auto titleFont = m_fonts->heading (20.0f);
    const juce::String title = "PITCH FLATTENER";
    const float titleWidth = juce::GlyphArrangement::getStringWidth (titleFont, title);
    g.setFont (titleFont);
    g.setColour (text);
    g.drawText (title, header.removeFromLeft (titleWidth + 4.0f), juce::Justification::centredLeft, false);

    header.removeFromLeft (14.0f);
    g.setFont (m_fonts->label (10.5f));
    g.setColour (textFaint);
    g.drawText ("SAMMYJS", header.withTrimmedTop (4.0f), juce::Justification::centredLeft, false);

    // --- Cards -------------------------------------------------------------------
    const auto card = [&g] (juce::Rectangle<int> bounds)
    {
        const auto r = bounds.toFloat();
        g.setColour (panel);
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (outlineSoft);
        g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
    };

    const auto dotTitle = [this, &g] (juce::Rectangle<float> row, const juce::String& name, juce::Colour accent)
    {
        g.setColour (accent);
        g.fillEllipse (row.removeFromLeft (6.0f).withSizeKeepingCentre (6.0f, 6.0f));
        row.removeFromLeft (8.0f);
        g.setFont (m_fonts->heading (11.0f));
        g.setColour (text.withAlpha (0.85f));
        g.drawText (name, row, juce::Justification::centredLeft, false);
    };

    // Source strip
    {
        card (m_sourceStrip);
        auto r = m_sourceStrip.toFloat().reduced (12.0f, 0.0f);
        dotTitle (r.removeFromLeft (84.0f), "SOURCE", detect);

        g.setFont (m_fonts->label (10.0f));
        g.setColour (textDim);
        g.drawFittedText (m_sourceBlurb, m_blurbArea, juce::Justification::centredLeft, 2, 1.0f);

        // Detector caption
        const auto caption = juce::Rectangle<int> (m_detectorCombo.getX() - 86, m_sourceStrip.getY(), 80, m_sourceStrip.getHeight());
        g.setFont (m_fonts->label (10.5f));
        g.setColour (textDim);
        g.drawText ("DETECTOR", caption.withTrimmedBottom (caption.getHeight() / 2).withTrimmedTop (4), juce::Justification::bottomRight, false);
        g.setFont (m_fonts->label (8.5f));
        g.setColour (textFaint);
        g.drawText (m_advancedShown ? "CUSTOM" : "SET BY SOURCE", caption.withTrimmedTop (caption.getHeight() / 2 + 2), juce::Justification::topRight, false);

        // Running detector (it can lag a parameter change while it is rebuilt)
        const auto active = m_activeArea;
        g.setFont (m_fonts->label (8.5f));
        g.setColour (textFaint);
        g.drawText ("RUNNING", active.withTrimmedBottom (active.getHeight() / 2).withTrimmedTop (4), juce::Justification::bottomLeft, false);

        const bool known = m_shownActiveDetector != pf::DetectorType::NumDetectors;
        g.setFont (m_fonts->label (10.5f));
        g.setColour (known ? detect : textFaint);
        g.drawText (known ? juce::String (pf::getDetectorName (m_shownActiveDetector)).toUpperCase() : juce::String ("--"),
                    active.withTrimmedTop (active.getHeight() / 2 + 1), juce::Justification::topLeft, false);
    }

    // Sections
    for (const auto& section : m_sections)
    {
        card (section.bounds);
        dotTitle (section.bounds.toFloat().withHeight ((float) c_sectionHeaderHeight).reduced (12.0f, 0.0f), section.title, section.accent);

        g.setColour (outlineSoft);
        g.drawHorizontalLine (section.bounds.getY() + c_sectionHeaderHeight,
                              (float) section.bounds.getX() + 12.0f, (float) section.bounds.getRight() - 12.0f);
    }

    // ADVANCED says why its controls are dimmed.
    {
        const auto row = m_sections[2].bounds.toFloat().withHeight ((float) c_sectionHeaderHeight).reduced (14.0f, 0.0f);
        g.setFont (m_fonts->label (9.0f));
        g.setColour (textFaint);
        g.drawText (m_advancedShown ? juce::String ("CUSTOM") : juce::String ("SET BY SOURCE  /  PICK CUSTOM TO CHANGE"),
                    row, juce::Justification::centredRight, false);
    }

    // Help bar
    {
        auto r = m_helpArea.toFloat();
        g.setColour (outlineSoft);
        g.drawHorizontalLine (m_helpArea.getY(), r.getX(), r.getRight());

        auto versionArea = r.removeFromRight (90.0f);
        g.setFont (m_fonts->body (11.0f));
        g.setColour (target.withAlpha (0.8f));
        g.drawText (juce::String ("v") + c_version, versionArea, juce::Justification::centredRight, false);

        g.setFont (m_fonts->body (11.5f));
        g.setColour (m_helpText == c_defaultHelp ? textFaint : textDim);
        g.drawFittedText (m_helpText, r.toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
    }
}

//==============================================================================
// Editor
//==============================================================================
PitchFlattenerAudioProcessorEditor::PitchFlattenerAudioProcessorEditor (PitchFlattenerAudioProcessor& p)
    : AudioProcessorEditor (&p)
    , m_lookAndFeel (*m_fonts)
{
    setLookAndFeel (&m_lookAndFeel);
    m_tooltipWindow.setLookAndFeel (&m_lookAndFeel);
    PitchFlattenerLookAndFeel::showFocusRingsAfterTab (*this); // keyboard focus rings after Tab, hidden by a click

    m_content = std::make_unique<Content> (p, m_lookAndFeel);
    addAndMakeVisible (*m_content);

    setResizable (true, true);
    setResizeLimits (baseWidth * 6 / 10, baseHeight * 6 / 10, baseWidth * 2, baseHeight * 2);

    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);

    setSize (baseWidth, baseHeight);

    // Looks for a newer release on a background thread (at most once a day, silent on any failure) and
    // shows the popup on the message thread if there is one.
    m_updateCheck = std::make_unique<sammyj::UpdateCheck> ("pitchflattener", "pitchflattener-v", c_version,
        [this] (const sammyj::UpdateInfo& info) { showUpdatePopup (info); });
}

PitchFlattenerAudioProcessorEditor::~PitchFlattenerAudioProcessorEditor()
{
    // The update check first (it joins its thread and drops a pending callback), then its popup.
    m_updateCheck.reset();
    m_updatePopup.reset();

    // Nothing the editor opened may outlive it: the content closes the About window (and its update
    // check) and the preset dialogs; then any popup menu (it would keep using m_lookAndFeel) and the
    // tooltip window go.
    m_content.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    m_tooltipWindow.hideTip();
    m_tooltipWindow.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

//==============================================================================
PitchFlattenerAudioProcessorEditor::EditorTooltipWindow::EditorTooltipWindow (juce::Component& editor)
    : juce::TooltipWindow (&editor, 700)
{
    setOpaque (false);                        // the LookAndFeel draws a rounded panel; its corners show the editor
    setInterceptsMouseClicks (false, false);  // a tip pushed under the mouse near an edge never takes a click
}

void PitchFlattenerAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::Colours::background);
}

void PitchFlattenerAudioProcessorEditor::resized()
{
    if (m_content == nullptr || getWidth() <= 0 || getHeight() <= 0)
        return;

    // Uniform scale of the fixed-size content, centred if the host ignores the aspect ratio.
    const float scale = std::min ((float) getWidth() / (float) baseWidth, (float) getHeight() / (float) baseHeight);
    const float x = ((float) getWidth() - (float) baseWidth * scale) * 0.5f;
    const float y = ((float) getHeight() - (float) baseHeight * scale) * 0.5f;

    m_content->setBounds (0, 0, baseWidth, baseHeight);
    m_content->setTransform (juce::AffineTransform::scale (scale).translated (x, y));

    if (m_updatePopup != nullptr)
    {
        m_updatePopup->setBounds (getLocalBounds());
        m_updatePopup->setEditorScale (scale);
    }
}

void PitchFlattenerAudioProcessorEditor::showUpdatePopup (const sammyj::UpdateInfo& info)
{
    if (m_updatePopup != nullptr)
        return;

    m_updatePopup = std::make_unique<UpdatePopup> (info, [safeThis = juce::Component::SafePointer<PitchFlattenerAudioProcessorEditor> (this)]
    {
        // Deferred: this runs inside one of the popup's own button callbacks.
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr)
                safeThis->m_updatePopup.reset();
        });
    });

    addAndMakeVisible (*m_updatePopup); // the last child: above the content and the resizer
    m_updatePopup->toFront (false);
    resized();

    if (isShowing())
        m_updatePopup->grabKeyboardFocus(); // Return and Escape reach it, as a dialog; rings only after Tab
}

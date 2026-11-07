#include "PluginEditor.h"

#include "AboutWindow.h"
#include "PresetManager.h"
#include "SpectrogramVisualizer.h"

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
        targetPitchKnob, autoWindowKnob, amountKnob, overrideFreqKnob,
        sensitivityKnob, speedKnob, smoothingKnob, gapFillKnob, minFreqKnob, maxFreqKnob,
        gateKnob, highpassKnob, lowpassKnob,
        maxShiftKnob, mixKnob,
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
    juce::TextButton m_relatchButton { "RE-LATCH" };
    juce::Label m_relatchLabel;
    juce::ToggleButton m_overrideToggle;
    juce::Label m_overrideLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_overrideAttachment;

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

    std::array<Section, 4> m_sections;
    juce::Rectangle<int> m_helpArea;
    juce::String m_helpText { c_defaultHelp };
    std::map<const juce::Component*, juce::String> m_help;

    std::atomic<float>* m_targetModeValue = nullptr;
    std::atomic<float>* m_overrideValue = nullptr;
    std::atomic<float>* m_gateValue = nullptr;

    std::unique_ptr<AboutWindow> m_aboutWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Content)
};

//==============================================================================
PitchFlattenerAudioProcessorEditor::Content::Content (PitchFlattenerAudioProcessor& processor,
                                                      PitchFlattenerLookAndFeel& lookAndFeel)
    : m_processor (processor)
    , m_lookAndFeel (lookAndFeel)
    , m_display (processor)
    , m_presetBar (processor.parameters)
{
    using namespace Theme::Colours;
    auto& params = m_processor.parameters;

    // Set our LookAndFeel before any child creates LookAndFeel-dependent parts
    // (slider value boxes, combo labels). This component is not yet inside the
    // editor here, so it would otherwise inherit the default LookAndFeel.
    setLookAndFeel (&m_lookAndFeel);

    m_targetModeValue = params.getRawParameterValue (ParamIDs::targetMode);
    m_overrideValue = params.getRawParameterValue (ParamIDs::manualOverride);
    m_gateValue = params.getRawParameterValue (ParamIDs::volumeThreshold);

    m_sections = { Section { "TARGET",   target,     {} },
                   Section { "TRACKING", detect,     {} },
                   Section { "INPUT",    detect,     {} },
                   Section { "OUTPUT",   correction, {} } };

    addAndMakeVisible (m_display);
    addAndMakeVisible (m_presetBar);

    m_aboutButton.onClick = [this] { showAbout(); };
    addAndMakeVisible (m_aboutButton);

    // --- Source strip ----------------------------------------------------
    setupSourceType();
    setupChoice (m_detectorCombo, ParamIDs::detector, detect,
                 "Detector: the pitch detection algorithm. Each Source Type picks one; change it here to override. "
                 "pYIN suits voices, MPM solo instruments, Spectral Peak sirens and tones, Harmonic Sum engines and drones.",
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
    setupKnob (overrideFreqKnob, ParamIDs::overrideFreq, "Override Hz", target, Format::HertzNote,
               "Override Hz: the target used while Override is on, replacing the target mode.");

    setupCaption (m_relatchLabel, "Latch");
    m_relatchButton.onClick = [this] { m_processor.relatch(); };
    addAndMakeVisible (m_relatchButton);
    setHelp (m_relatchButton, "Re-latch (Latch mode): forget the latched pitch so the next stable pitch is latched again.");

    setupToggle (m_overrideToggle, m_overrideLabel, ParamIDs::manualOverride, "Override", target,
                 "Override: flatten to Override Hz instead of the target mode.", m_overrideAttachment);

    // --- Tracking ----------------------------------------------------------
    setupKnob (sensitivityKnob, ParamIDs::sensitivity, "Sensitivity", detect, Format::Percent,
               "Sensitivity: how readily frames are treated as pitched. Higher tracks quieter or noisier sounds; lower rejects more noise.");
    setupKnob (speedKnob, ParamIDs::trackingSpeed, "Speed", detect, Format::OctavesPerSecond,
               "Tracking Speed: the fastest pitch movement the tracker follows, in octaves per second. Raise it for fast sirens and yelps.");
    setupKnob (smoothingKnob, ParamIDs::trackSmoothing, "Smoothing", detect, Format::Milliseconds,
               "Smoothing: smooths the pitch track (centred, using the lookahead, so it adds no lag).");
    setupKnob (gapFillKnob, ParamIDs::gapFill, "Gap Fill", detect, Format::Milliseconds,
               "Gap Fill: bridges unpitched gaps up to this long so the correction does not drop out mid-sound.");
    setupKnob (minFreqKnob, ParamIDs::minFreq, "Min Freq", detect, Format::Hertz,
               "Min Freq: lowest pitch the detector looks for. Keep the range tight around the source for the most reliable tracking.");
    setupKnob (maxFreqKnob, ParamIDs::maxFreq, "Max Freq", detect, Format::Hertz,
               "Max Freq: highest pitch the detector looks for.");

    // --- Input ---------------------------------------------------------------
    setupKnob (gateKnob, ParamIDs::volumeThreshold, "Gate", detect, Format::Decibels,
               "Gate: input below this level is treated as unpitched (the teal line on the level meter).");
    setupKnob (highpassKnob, ParamIDs::detectionHighpass, "Highpass", detect, Format::Hertz,
               "Detection Highpass: filters rumble out of the signal the detector hears. The audio itself is not filtered.");
    setupKnob (lowpassKnob, ParamIDs::detectionLowpass, "Lowpass", detect, Format::Hertz,
               "Detection Lowpass: filters hiss and high harmonics out of the signal the detector hears. The audio itself is not filtered.");

    setupCaption (m_levelLabel, "Level");
    addAndMakeVisible (m_gateMeter);
    setHelp (m_gateMeter, "Live input level. Green when above the gate (teal line), so the detector is listening.");

    // --- Output --------------------------------------------------------------
    setupKnob (maxShiftKnob, ParamIDs::maxShift, "Max Shift", correction, Format::Semitones,
               "Max Shift: the largest correction allowed, in semitones. Larger deviations are only corrected this far.");
    setupKnob (mixKnob, ParamIDs::mix, "Mix", correction, Format::MixPercent,
               "Mix: blend between the dry input and the flattened signal (both latency-aligned).");

    setupCaption (m_qualityLabel, "Quality");
    setupChoice (m_qualityCombo, ParamIDs::quality, correction,
                 "Shifter quality. Smooth: standard window, best for sustained tones. Tight: short window, better for fast movement and transients.",
                 m_qualityAttachment);

    setupToggle (m_formantToggle, m_formantLabel, ParamIDs::rbFormantPreserve, "Formant", correction,
                 "Formant: preserve the spectral envelope while shifting (keeps voices natural). Off is usually better for sirens and machines.",
                 m_formantAttachment);

    setHelp (m_presetBar.getMenu(), "Presets: one factory preset per Source Type, plus your own saved presets.");
    setHelp (m_presetBar.getSaveButton(), "Save the current settings as a user preset.");
    setHelp (m_presetBar.getDeleteButton(), "Move the selected user preset to the trash.");
    setHelp (m_presetBar.getResetButton(), "Reset every parameter to its default.");
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
    setHelp (m_sourceCombo, "Source Type: what you are flattening. Picking one loads tuned detector, range, tracking, filter "
                            "and shifter settings for that kind of sound; adjust them afterwards as needed.");

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
    const bool overrideOn = m_overrideValue != nullptr && m_overrideValue->load() > 0.5f;

    const auto enable = [] (juce::Component& c, juce::Label& label, bool on)
    {
        c.setEnabled (on);
        label.setEnabled (on);
    };

    auto& k = m_knobs;
    enable (k[targetPitchKnob].slider, k[targetPitchKnob].label, ! overrideOn && mode == (int) pf::TargetMode::Fixed);
    enable (k[autoWindowKnob].slider, k[autoWindowKnob].label, ! overrideOn && mode == (int) pf::TargetMode::Auto);
    enable (k[overrideFreqKnob].slider, k[overrideFreqKnob].label, overrideOn);
    enable (m_relatchButton, m_relatchLabel, ! overrideOn && mode == (int) pf::TargetMode::Latch);
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
    if (m_aboutWindow == nullptr)
        m_aboutWindow = std::make_unique<AboutWindow> (m_lookAndFeel);

    m_aboutWindow->setVisible (true);
    m_aboutWindow->toFront (true);
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

    // Header: title (painted), preset bar, about
    auto header = bounds.removeFromTop (c_headerHeight).reduced (c_margin, 0);
    m_aboutButton.setBounds (header.removeFromRight (26).withSizeKeepingCentre (26, 26));
    header.removeFromRight (12);
    m_presetBar.setBounds (header.removeFromRight (430).withSizeKeepingCentre (430, 26));

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

    // 3 + 3 + 2 + 2 columns
    const int unit = (controls.getWidth() - 3 * c_sectionGap) / 10;
    m_sections[0].bounds = controls.removeFromLeft (unit * 3);
    controls.removeFromLeft (c_sectionGap);
    m_sections[1].bounds = controls.removeFromLeft (unit * 3);
    controls.removeFromLeft (c_sectionGap);
    m_sections[3].bounds = controls.removeFromRight (unit * 2);
    controls.removeFromRight (c_sectionGap);
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

    // TARGET
    {
        const auto cell = grid (m_sections[0], 3);
        placeKnob (targetPitchKnob, cell (0, 0));
        placeKnob (autoWindowKnob, cell (1, 0));
        placeKnob (amountKnob, cell (2, 0));

        placeInCell (cell (0, 1).reduced (6, 0), m_relatchButton, m_relatchLabel, 26);
        placeInCell (cell (1, 1), m_overrideToggle, m_overrideLabel, 26);
        placeKnob (overrideFreqKnob, cell (2, 1));

        auto row = m_sections[0].bounds.withHeight (c_sectionHeaderHeight).reduced (8, 5);
        m_targetModeCombo.setBounds (row.removeFromRight (std::min (120, row.getWidth() - 90)));
    }

    // TRACKING
    {
        const auto cell = grid (m_sections[1], 3);
        placeKnob (sensitivityKnob, cell (0, 0));
        placeKnob (speedKnob, cell (1, 0));
        placeKnob (smoothingKnob, cell (2, 0));
        placeKnob (gapFillKnob, cell (0, 1));
        placeKnob (minFreqKnob, cell (1, 1));
        placeKnob (maxFreqKnob, cell (2, 1));
    }

    // INPUT
    {
        const auto cell = grid (m_sections[2], 2);
        placeKnob (gateKnob, cell (0, 0));
        placeInCell (cell (1, 0), m_gateMeter, m_levelLabel);
        placeKnob (highpassKnob, cell (0, 1));
        placeKnob (lowpassKnob, cell (1, 1));
    }

    // OUTPUT
    {
        const auto cell = grid (m_sections[3], 2);
        placeKnob (maxShiftKnob, cell (0, 0));
        placeKnob (mixKnob, cell (1, 0));
        placeInCell (cell (0, 1).reduced (4, 0), m_qualityCombo, m_qualityLabel, 26);
        placeInCell (cell (1, 1), m_formantToggle, m_formantLabel, 26);
    }
}

void PitchFlattenerAudioProcessorEditor::Content::paint (juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll (background);

    // --- Header ----------------------------------------------------------------
    auto header = getLocalBounds().removeFromTop (c_headerHeight).reduced (c_margin, 0).toFloat();

    // Mark: a wobbling teal line that settles into a flat amber line.
    {
        const auto mark = header.removeFromLeft (34.0f).withSizeKeepingCentre (34.0f, 22.0f);
        const float split = 0.55f;
        juce::Path wobble, flat;

        for (int i = 0; i <= 40; ++i)
        {
            const float t = (float) i / 40.0f;
            if (t > split)
                break;

            const float fade = 1.0f - t / split;
            const float y = mark.getCentreY() + 7.0f * fade * std::sin (t * juce::MathConstants<float>::twoPi * 3.2f);
            const juce::Point<float> p { mark.getX() + t * mark.getWidth(), y };
            if (i == 0) wobble.startNewSubPath (p);
            else        wobble.lineTo (p);
        }

        flat.startNewSubPath (mark.getX() + split * mark.getWidth(), mark.getCentreY());
        flat.lineTo (mark.getRight(), mark.getCentreY());

        g.setColour (detect.withAlpha (0.85f));
        g.strokePath (wobble, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (target);
        g.strokePath (flat, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

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
        g.drawText ("SET BY SOURCE", caption.withTrimmedTop (caption.getHeight() / 2 + 2), juce::Justification::topRight, false);

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

    m_content = std::make_unique<Content> (p, m_lookAndFeel);
    addAndMakeVisible (*m_content);

    setResizable (true, true);
    setResizeLimits (baseWidth * 6 / 10, baseHeight * 6 / 10, baseWidth * 2, baseHeight * 2);

    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);

    setSize (baseWidth, baseHeight);
}

PitchFlattenerAudioProcessorEditor::~PitchFlattenerAudioProcessorEditor()
{
    m_content.reset();
    setLookAndFeel (nullptr);
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
}

//==============================================================================
// LookAndFeel
//==============================================================================
PitchFlattenerLookAndFeel::PitchFlattenerLookAndFeel (const Theme::Fonts& fonts)
    : m_fonts (fonts)
{
    using namespace Theme::Colours;
    const auto accent = detect;

    if (fonts.medium != nullptr)
        setDefaultSansSerifTypeface (fonts.medium);

    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::DocumentWindow::textColourId, text);
    setColour (juce::Label::textColourId, text);

    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.35f));

    setColour (juce::TextEditor::backgroundColourId, panelRaised);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, outline);
    setColour (juce::TextEditor::focusedOutlineColourId, accent.withAlpha (0.6f));
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
    setColour (juce::CaretComponent::caretColourId, text);

    setColour (juce::ComboBox::backgroundColourId, panelRaised);
    setColour (juce::ComboBox::outlineColourId, outline);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, accent);
    setColour (juce::ComboBox::focusedOutlineColourId, accent.withAlpha (0.6f));

    setColour (juce::PopupMenu::backgroundColourId, panelRaised);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, textFaint);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.18f));
    setColour (juce::PopupMenu::highlightedTextColourId, text);

    setColour (juce::TextButton::buttonColourId, panelRaised);
    setColour (juce::TextButton::buttonOnColourId, panelRaised.brighter (0.1f));
    setColour (juce::TextButton::textColourOffId, textDim);
    setColour (juce::TextButton::textColourOnId, text);

    setColour (juce::ToggleButton::textColourId, textDim);
    setColour (juce::ToggleButton::tickColourId, accent);

    setColour (juce::AlertWindow::backgroundColourId, panel);
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::AlertWindow::outlineColourId, outline);

    setColour (juce::TooltipWindow::backgroundColourId, panelRaised);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, outline);

    setColour (juce::ScrollBar::thumbColourId, outline.brighter (0.2f));
}

void PitchFlattenerLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                                  float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                                  juce::Slider& slider)
{
    using namespace Theme::Colours;

    const bool enabled = slider.isEnabled();
    const float alpha = enabled ? 1.0f : 0.3f;
    const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
    const bool hot = enabled && slider.isMouseOverOrDragging();

    const auto area = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
    const float diameter = std::min (area.getWidth(), area.getHeight());
    const auto centre = area.getCentre();
    const float radius = diameter * 0.5f;
    const float trackWidth = std::max (2.5f, radius * 0.09f);
    const float arcRadius = radius - trackWidth * 0.5f;
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // Track
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (outline);
    g.strokePath (track, juce::PathStrokeType (trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc: bipolar parameters grow from the centre.
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0
                      && juce::approximatelyEqual (slider.getMinimum(), -slider.getMaximum());
    const float fromAngle = bipolar ? (rotaryStartAngle + rotaryEndAngle) * 0.5f : rotaryStartAngle;

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, std::min (fromAngle, angle), std::max (fromAngle, angle), true);
    g.setColour (accent.withMultipliedAlpha ((hot ? 1.0f : 0.9f) * alpha));
    g.strokePath (value, juce::PathStrokeType (trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Body
    const float bodyRadius = arcRadius - trackWidth * 1.6f;
    const auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);

    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillEllipse (body.translated (0.0f, 2.0f));

    g.setGradientFill (juce::ColourGradient (panelRaised.brighter (hot ? 0.18f : 0.1f), centre.x, body.getY(),
                                             panelRaised.darker (0.25f), centre.x, body.getBottom(), false));
    g.fillEllipse (body);
    g.setColour (outline.brighter (0.15f));
    g.drawEllipse (body.reduced (0.5f), 1.0f);

    // Pointer
    const auto direction = juce::Point<float> (std::sin (angle), -std::cos (angle));
    g.setColour (text.withAlpha (enabled ? 1.0f : 0.35f));
    g.drawLine ({ centre + direction * (bodyRadius * 0.35f), centre + direction * (bodyRadius * 0.82f) }, 2.0f);
}

juce::Slider::SliderLayout PitchFlattenerLookAndFeel::getSliderLayout (juce::Slider& slider)
{
    juce::Slider::SliderLayout layout;
    auto bounds = slider.getLocalBounds();
    layout.textBoxBounds = bounds.removeFromBottom (18).withSizeKeepingCentre (std::min (bounds.getWidth(), 92), 18);
    bounds.removeFromBottom (2);
    layout.sliderBounds = bounds;
    return layout;
}

juce::Label* PitchFlattenerLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (m_fonts.body (12.0f));
    label->setJustificationType (juce::Justification::centred);
    label->setColour (juce::Label::textColourId, Theme::Colours::text);
    label->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    label->setColour (juce::Label::outlineWhenEditingColourId, Theme::Colours::outline);
    label->setColour (juce::Label::backgroundWhenEditingColourId, Theme::Colours::panelRaised);
    return label;
}

juce::Font PitchFlattenerLookAndFeel::getLabelFont (juce::Label& label)
{
    if (dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr)
        return m_fonts.body (12.0f);

    return m_fonts.label (10.5f);
}

void PitchFlattenerLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                              int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    const bool enabled = box.isEnabled();
    const bool hot = enabled && (box.isMouseOver (true) || box.isPopupActive());

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId).brighter (hot ? 0.08f : 0.0f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (hot ? box.findColour (juce::ComboBox::focusedOutlineColourId) : box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, 5.0f, 1.0f);

    const float cx = r.getRight() - 13.0f;
    const float cy = r.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
    chevron.lineTo (cx, cy + 2.0f);
    chevron.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (enabled ? 1.0f : 0.35f));
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font PitchFlattenerLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return m_fonts.body (12.0f);
}

void PitchFlattenerLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (8, 0, box.getWidth() - 30, box.getHeight());
    label.setFont (getComboBoxFont (box));
}

juce::Font PitchFlattenerLookAndFeel::getPopupMenuFont()
{
    return m_fonts.body (13.0f);
}

void PitchFlattenerLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                                      const juce::Colour& backgroundColour,
                                                      bool isMouseOverButton, bool isButtonDown)
{
    const auto r = button.getLocalBounds().toFloat().reduced (0.5f);
    const float corner = std::min (r.getWidth(), r.getHeight()) * 0.5f;
    const float alpha = button.isEnabled() ? 1.0f : 0.4f;

    g.setColour (backgroundColour.brighter (isButtonDown ? 0.2f : isMouseOverButton ? 0.1f : 0.0f).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (r, corner);
    g.setColour ((isMouseOverButton ? Theme::Colours::textDim : Theme::Colours::outline).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (r, corner, 1.0f);
}

juce::Font PitchFlattenerLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return m_fonts.label (std::min (12.0f, (float) buttonHeight * 0.5f));
}

void PitchFlattenerLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                                  bool isMouseOverButton, bool isButtonDown)
{
    using namespace Theme::Colours;

    const bool enabled = button.isEnabled();
    const bool on = button.getToggleState();
    const float alpha = enabled ? 1.0f : 0.35f;
    const auto accent = button.findColour (juce::ToggleButton::tickColourId);

    auto bounds = button.getLocalBounds().toFloat();
    constexpr float trackW = 32.0f, trackH = 16.0f;
    const auto buttonText = button.getButtonText();

    // Centred switch when there is no text, otherwise switch on the left with the label after it.
    const auto track = buttonText.isEmpty() ? juce::Rectangle<float> (trackW, trackH).withCentre (bounds.getCentre())
                                      : bounds.removeFromLeft (trackW).withSizeKeepingCentre (trackW, trackH);

    const float corner = trackH * 0.5f;
    g.setColour ((on ? accent.withAlpha (0.85f) : panelRaised.brighter (isMouseOverButton ? 0.08f : 0.0f)).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (track, corner);
    g.setColour ((on ? accent : (isMouseOverButton ? textDim : outline)).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (track.reduced (0.5f), corner, 1.0f);

    const float thumb = trackH - 4.0f + (isButtonDown ? 1.0f : 0.0f);
    const float thumbX = on ? track.getRight() - 2.0f - thumb : track.getX() + 2.0f;
    const auto thumbBounds = juce::Rectangle<float> (thumbX, track.getCentreY() - thumb * 0.5f, thumb, thumb);

    g.setColour (juce::Colours::black.withAlpha (0.3f * alpha));
    g.fillEllipse (thumbBounds.translated (0.0f, 1.0f));
    g.setColour ((on ? Theme::Colours::text : textDim).withMultipliedAlpha (alpha));
    g.fillEllipse (thumbBounds);

    if (buttonText.isNotEmpty())
    {
        bounds.removeFromLeft (8.0f);
        g.setFont (m_fonts.label (10.5f));
        g.setColour (button.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (alpha));
        g.drawText (buttonText.toUpperCase(), bounds, juce::Justification::centredLeft, true);
    }
}

juce::Font PitchFlattenerLookAndFeel::getAlertWindowTitleFont()   { return m_fonts.heading (14.0f); }
juce::Font PitchFlattenerLookAndFeel::getAlertWindowMessageFont() { return m_fonts.body (13.0f); }
juce::Font PitchFlattenerLookAndFeel::getAlertWindowFont()        { return m_fonts.body (12.0f); }

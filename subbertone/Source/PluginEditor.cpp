#include "PluginEditor.h"

#include "AboutWindow.h"
#include "UpdatePopup.h"
#include "SammyJUpdateCheck.h"

namespace
{
    // Design-size layout (everything is scaled uniformly from here).
    constexpr int c_margin = 16;
    constexpr int c_headerHeight = 56;
    constexpr int c_controlsHeight = 186;
    constexpr int c_stripPanelHeight = 108;
    constexpr int c_sectionGap = 12;
    constexpr int c_sectionHeaderHeight = 32;
    constexpr float c_minScale = 0.55f, c_maxScale = 1.4f, c_defaultScale = 0.8f;

    const juce::Identifier c_viewEditorScale { "viewEditorScale" };

    juce::String formatHertz(double v)
    {
        return v >= 1000.0 ? juce::String(v / 1000.0, v >= 10000.0 ? 1 : 2) + " kHz"
                           : juce::String(juce::roundToInt(v)) + " Hz";
    }

    // One decimal with an explicit sign, and no "-0.0" for values that round to zero.
    juce::String signedText(double v, const char* unit)
    {
        const double rounded = std::round(v * 10.0) / 10.0;
        if (std::abs(rounded) < 0.05)
            return juce::String("0.0") + unit;
        return (rounded > 0.0 ? "+" : "") + juce::String(rounded, 1) + unit;
    }

    // Preset menu item IDs: factory presets are 1..getNumFactory() (they lead the preset list in a
    // fixed order, so their index is stable), user presets count up from c_firstUserItemId.
    constexpr int c_saveAsItemId = 1001, c_deleteItemId = 1002, c_revealItemId = 1003;
    constexpr int c_firstUserItemId = 100000;

    // The limiter's gain-reduction meter in the MASTER header.
    constexpr float c_grMeterRangeDb = 12.0f;   // a full bar
    constexpr float c_grReadoutRangeDb = 24.0f; // the readout shows "<-24" beyond this

    // One-shot harmonic patterns for the PATTERN menu (percent per harmonic, index 1..16).
    struct HarmonicPattern
    {
        const char* name;
        std::array<float, 17> levels;
        bool keepSubLevel;
    };

    const std::vector<HarmonicPattern>& harmonicPatterns()
    {
        static const std::vector<HarmonicPattern> patterns
        {
            { "Default (H2-H6)",              { 0, 100, 60, 40, 30, 24, 20 }, false },
            { "Even only - smooth",           { 0, 0, 60, 0, 40, 0, 30, 0, 24 }, true },
            { "Odd only - burble",            { 0, 0, 0, 50, 0, 34, 0, 26, 0, 20 }, true },
            { "Full series (H2-H8)",          { 0, 0, 60, 40, 30, 24, 20, 17, 15 }, true },
            { "Small speaker (H2-H5)",        { 0, 0, 70, 50, 38, 30 }, true },
            { "Upper partials (H5-H10)",      { 0, 0, 0, 0, 0, 30, 25, 21, 19, 17, 15 }, true },
            { "Sparkle (H8-H16)",             { 0, 0, 0, 0, 0, 0, 0, 0, 20, 18, 16, 15, 14, 13, 12, 11, 10 }, true },
            { "Clear overtones",              { 0 }, true },
        };
        return patterns;
    }
}

//==============================================================================
SubbertoneAudioProcessorEditor::SubbertoneAudioProcessorEditor(SubbertoneAudioProcessor& audioProcessor)
    : AudioProcessorEditor(&audioProcessor)
    , m_audioProcessor(audioProcessor)
    , m_lookAndFeel(*m_fonts)
    , m_waveformVisualizer(audioProcessor)
    , m_mixAttachment(m_audioProcessor.m_parameters, "mix", m_mixSlider)
    , m_distortionAttachment(m_audioProcessor.m_parameters, "distortion", m_distortionSlider)
    , m_toneAttachment(m_audioProcessor.m_parameters, "distortionTone", m_toneSlider)
    , m_postDriveLowpassAttachment(m_audioProcessor.m_parameters, "postDriveLowpass", m_postDriveLowpassSlider)
    , m_outputGainAttachment(m_audioProcessor.m_parameters, "outputGain", m_outputGainSlider)
    , m_pitchThresholdAttachment(m_audioProcessor.m_parameters, "pitchThreshold", m_pitchThresholdSlider)
    , m_fundamentalLimitAttachment(m_audioProcessor.m_parameters, "fundamentalLimit", m_fundamentalLimitSlider)
    , m_resonanceAttachment(m_audioProcessor.m_parameters, "filterResonance", m_resonanceSlider)
    , m_envAmountAttachment(m_audioProcessor.m_parameters, "filterEnvAmount", m_envAmountSlider)
    , m_envReleaseAttachment(m_audioProcessor.m_parameters, "filterEnvRelease", m_envReleaseSlider)
    , m_glideAttachment(m_audioProcessor.m_parameters, "pitchGlide", m_glideSlider)
    , m_overtoneAmountAttachment(m_audioProcessor.m_parameters, "overtoneAmount", m_overtoneAmountSlider)
    , m_inflateAttachment(m_audioProcessor.m_parameters, "inflateEffect", m_inflateSlider)
    , m_inflateCurveAttachment(m_audioProcessor.m_parameters, "inflateCurve", m_inflateCurveSlider)
    , m_ceilingAttachment(m_audioProcessor.m_parameters, "limiterCeiling", m_ceilingSlider)
    , m_limiterReleaseAttachment(m_audioProcessor.m_parameters, "limiterRelease", m_limiterReleaseSlider)
    , m_limiterAttachment(m_audioProcessor.m_parameters, "limiterOn", m_limiterButton)
    , m_loCutAttachment(m_audioProcessor.m_parameters, "loCutFreq", m_loCutSlider)
    , m_loCutButtonAttachment(m_audioProcessor.m_parameters, "loCutOn", m_loCutButton)
    , m_phaseAlignAttachment(m_audioProcessor.m_parameters, "phaseAlign", m_phaseAlignButton)
{
    setLookAndFeel(&m_lookAndFeel);
    m_tooltipWindow.setLookAndFeel(&m_lookAndFeel);
    SubbertoneLookAndFeel::showFocusRingsAfterTab(*this); // keyboard focus rings after Tab, hidden by a click

    m_content.onPaint = [this](juce::Graphics& g) { paintContent(g); };
    m_content.onResized = [this] { layoutContent(); };
    addAndMakeVisible(m_content);

    // Sections run left to right in signal order.
    m_sections = { Section { "DETECT",    Theme::Colours::input, {} },
                   Section { "DRIVE",     Theme::Colours::harmonics, {} },
                   Section { "FILTER",    Theme::Colours::filter, {} },
                   Section { "OVERTONES", Theme::Colours::overtones, {} },
                   Section { "OUTPUT",    Theme::Colours::output, {} },
                   Section { "MASTER",    Theme::Colours::master, {} } };

    m_content.addAndMakeVisible(m_waveformVisualizer);

    m_harmonicStrip = std::make_unique<HarmonicStrip>(m_audioProcessor.m_parameters, *m_fonts,
                                                      [this](int k) { return m_audioProcessor.getHarmonicActivity(k); });
    m_content.addAndMakeVisible(*m_harmonicStrip);

    // HARMONICS header: the Phase Align switch, mint like the strip.
    m_phaseAlignButton.setButtonText("PHASE ALIGN");
    m_phaseAlignButton.setClickingTogglesState(true);
    m_phaseAlignButton.setTooltip("Aligns the generated harmonics with the bass's own harmonics, so they add up instead of "
                                  "cancelling: notes thin out far less, and repeats of the same hit come out at the same "
                                  "level. Works best on bass lines and hits; it helps much less on engines.");
    m_phaseAlignButton.setColour(juce::TextButton::buttonOnColourId, Theme::Colours::overtones.withAlpha(0.28f));
    m_phaseAlignButton.setColour(juce::TextButton::textColourOnId, Theme::Colours::text);
    m_content.addAndMakeVisible(m_phaseAlignButton);

    // DETECT
    setupKnob(m_pitchThresholdSlider, m_pitchThresholdLabel, "Threshold", "pitchThreshold",
              "Input level needed before the sub is generated. Lower catches quieter notes.",
              Theme::Colours::input, Format::Decibels);
    setupKnob(m_fundamentalLimitSlider, m_fundamentalLimitLabel, "Max Pitch", "fundamentalLimit",
              "Highest input fundamental that is tracked. Notes above this are ignored.",
              Theme::Colours::input, Format::Hertz);
    setupKnob(m_glideSlider, m_glideLabel, "Glide", "pitchGlide",
              "How smoothly the sub follows pitch changes. Raise it for noisy or wandering sources such as engines.",
              Theme::Colours::input, Format::Milliseconds);

    // DRIVE
    setupKnob(m_distortionSlider, m_distortionLabel, "Drive", "distortion",
              "Crossfades the clean sub into its saturated harmonics while increasing drive.",
              Theme::Colours::harmonics, Format::Percent);
    setupChoice(m_distortionTypeCombo, "distortionType",
                "Saturation character:\n"
                "Tape - smooth, odd harmonics\n"
                "Valve - warm, strong 2nd harmonic\n"
                "Console - harder knee, brighter\n"
                "Transformer - soft, mostly odd with a touch of 2nd\n"
                "Rectifier - even harmonics only; reinforces the input note\n"
                "Wavefolder - harmonics sweep upward as drive rises",
                m_distortionTypeAttachment);
    m_distortionTypeCombo.setColour(juce::ComboBox::arrowColourId, Theme::Colours::harmonics);
    setupCaption(m_distortionTypeLabel, "TYPE");

    // FILTER
    setupKnob(m_toneSlider, m_toneLabel, "Cutoff", "distortionTone",
              "Filter cutoff for the harmonics. The envelope sweeps up or down from here.",
              Theme::Colours::filter, Format::Hertz);
    setupKnob(m_resonanceSlider, m_resonanceLabel, "Reso", "filterResonance",
              "Filter resonance. The resonant peak is driven into saturation: subtle overtones up to full overdrive.",
              Theme::Colours::filter, Format::Percent);
    setupKnob(m_envAmountSlider, m_envAmountLabel, "Env", "filterEnvAmount",
              "How far each note's envelope sweeps the cutoff, in octaves (negative sweeps down).",
              Theme::Colours::filter, Format::Octaves);
    setupKnob(m_envReleaseSlider, m_envReleaseLabel, "Release", "filterEnvRelease",
              "How quickly the filter envelope falls back after each note.",
              Theme::Colours::filter, Format::Milliseconds);
    setupChoice(m_filterModeCombo, "filterMode",
                "Filter response for the harmonics: lowpass, bandpass (vocal, focused) or notch (hollow).",
                m_filterModeAttachment);
    m_filterModeCombo.setColour(juce::ComboBox::arrowColourId, Theme::Colours::filter);
    m_filterModeCombo.setColour(juce::ComboBox::focusedOutlineColourId, Theme::Colours::filter.withAlpha(0.6f));

    // OVERTONES
    setupKnob(m_overtoneAmountSlider, m_overtoneAmountLabel, "Amount", "overtoneAmount",
              "Master level for the harmonic bars H2-H16 (the MaxxBass/RBass trick: the sub is heard on small speakers).",
              Theme::Colours::overtones, Format::Percent);
    setupCaption(m_patternLabel, "PATTERN");
    m_patternButton.setButtonText("CHOOSE");
    m_patternButton.setTooltip("Set the harmonic bars to a pattern: even (smooth), odd (burble), small speaker, ...");
    m_patternButton.setColour(juce::PopupMenu::highlightedBackgroundColourId, Theme::Colours::overtones); // its menu's accent
    m_patternButton.onClick = [this] { showPatternMenu(); };
    m_content.addAndMakeVisible(m_patternButton);

    // OUTPUT
    setupKnob(m_loCutSlider, m_loCutLabel, "Lo Cut", "loCutFreq",
              "Steep 48 dB/oct low cut on the generated sub (switch it on with LO CUT). Subs below ~30 Hz mostly use up "
              "headroom; when a very low sub is cut, the harmonic bars still carry its pitch.",
              Theme::Colours::output, Format::Hertz);
    m_loCutButton.setButtonText("LO CUT");
    m_loCutButton.setClickingTogglesState(true);
    m_loCutButton.setTooltip("Switch the 48 dB/oct low cut on the generated sub on or off");
    m_loCutButton.setColour(juce::TextButton::buttonOnColourId, Theme::Colours::output.withAlpha(0.28f));
    m_loCutButton.setColour(juce::TextButton::textColourOnId, Theme::Colours::text);
    m_content.addAndMakeVisible(m_loCutButton);

    setupKnob(m_postDriveLowpassSlider, m_postDriveLowpassLabel, "Hi Cut", "postDriveLowpass",
              "Low-pass on the whole sub signal, after drive, filter and overtones.",
              Theme::Colours::output, Format::Hertz);
    setupKnob(m_mixSlider, m_mixLabel, "Mix", "mix",
              "Blend between the dry input and the generated sub.",
              Theme::Colours::output, Format::Percent);
    setupKnob(m_outputGainSlider, m_outputGainLabel, "Output", "outputGain",
              "Output level, before the master stage (so it also drives the limiter).",
              Theme::Colours::output, Format::Decibels);

    // MASTER
    setupKnob(m_inflateSlider, m_inflateLabel, "Inflate", "inflateEffect",
              "Inflator-style density: makes the mix louder and fuller without raising the peaks.",
              Theme::Colours::master, Format::Percent);
    setupKnob(m_inflateCurveSlider, m_inflateCurveLabel, "Curve", "inflateCurve",
              "Character of the inflate curve: negative is gentler, positive is more aggressive.",
              Theme::Colours::master, Format::Signed);
    setupKnob(m_ceilingSlider, m_ceilingLabel, "Ceiling", "limiterCeiling",
              "Brickwall limiter ceiling: the output never goes above this when LIMIT is on.",
              Theme::Colours::master, Format::Decibels);
    setupKnob(m_limiterReleaseSlider, m_limiterReleaseLabel, "Release", "limiterRelease",
              "How quickly the limiter lets go after a peak.",
              Theme::Colours::master, Format::Milliseconds);
    m_limiterButton.setButtonText("LIMIT");
    m_limiterButton.setClickingTogglesState(true);
    m_limiterButton.setTooltip("Look-ahead brickwall limiter on the final output");
    m_limiterButton.setColour(juce::TextButton::buttonOnColourId, Theme::Colours::master.withAlpha(0.28f));
    m_limiterButton.setColour(juce::TextButton::textColourOnId, Theme::Colours::text);
    // Called for every toggle change (a click, automation, a preset load), so the readout follows LIMIT.
    m_limiterButton.onStateChange = [this] { updateLimiterReadout(); };
    m_content.addAndMakeVisible(m_limiterButton);
    m_limiterReadoutShown = m_limiterButton.getToggleState();
    if (const float gr = m_audioProcessor.getGainReductionDb(); std::isfinite(gr))
        m_lastGainReductionDb = gr;

    m_aboutButton.setButtonText("?");
    m_aboutButton.setTooltip("About SammyJs Subbertone");
    m_aboutButton.onClick = [this] { showAboutWindow(); };
    m_content.addAndMakeVisible(m_aboutButton);

    setupPresetBar();

    m_websiteLink.setFont(m_fonts->label(10.5f), false, juce::Justification::centredLeft);
    m_websiteLink.setColour(juce::HyperlinkButton::textColourId, Theme::Colours::textDim); // amber on hover (ThemeLink)
    m_websiteLink.setTooltip("Open sweetjusticesound.com");
    m_content.addAndMakeVisible(m_websiteLink);

    // Resizable, uniformly scaled, fixed aspect ratio; the size is part of the saved state.
    // Read the saved scale first: setting the limits below resizes the editor, which
    // would otherwise overwrite it. A damaged value (NaN, inf) must not give a 0 x 0 editor.
    const auto savedScale = static_cast<double>(m_audioProcessor.getViewValue(c_viewEditorScale, c_defaultScale));
    const float scale = std::isfinite(savedScale) ? static_cast<float>(juce::jlimit<double>(c_minScale, c_maxScale, savedScale))
                                                  : c_defaultScale;
    setResizable(true, true);
    setResizeLimits(juce::roundToInt(c_baseWidth * c_minScale), juce::roundToInt(c_baseHeight * c_minScale),
                    juce::roundToInt(c_baseWidth * c_maxScale), juce::roundToInt(c_baseHeight * c_maxScale));
    if (auto* sizeConstrainer = getConstrainer())
        sizeConstrainer->setFixedAspectRatio(static_cast<double>(c_baseWidth) / c_baseHeight);

    setSize(juce::roundToInt(c_baseWidth * scale), juce::roundToInt(c_baseHeight * scale));
    m_sizeRestored = true;

    startTimerHz(20);

    // The update check waits until the editor is on screen (see startUpdateCheckWhenShown()).
    startUpdateCheckWhenShown();
}

SubbertoneAudioProcessorEditor::~SubbertoneAudioProcessorEditor()
{
    stopTimer();
    m_limiterButton.onStateChange = nullptr;

    // Nothing the editor opened may outlive it: the update check (stopped first, so its callback can't
    // run), the update popup, the About window (and its update check), any popup menu (it would keep
    // using m_lookAndFeel), alerts and the tooltip window.
    m_updateCheck.reset();
    m_updatePopup.reset();
    m_aboutWindow.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    m_alert.reset();
    m_tooltipWindow.hideTip();
    m_tooltipWindow.setLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

//==============================================================================
SubbertoneAudioProcessorEditor::EditorTooltipWindow::EditorTooltipWindow(juce::Component& editor)
    : juce::TooltipWindow(nullptr, 700) // no parent: a desktop window of its own, above the GL view
    , m_editor(editor)
{
    setOpaque(false); // the LookAndFeel draws a rounded panel; the corners show what is behind the tip
}

juce::String SubbertoneAudioProcessorEditor::EditorTooltipWindow::getTipFor(juce::Component& component)
{
    return m_editor.isParentOf(&component) ? juce::TooltipWindow::getTipFor(component) : juce::String();
}

float SubbertoneAudioProcessorEditor::EditorTooltipWindow::getDesktopScaleFactor() const
{
    return juce::Component::getApproximateScaleFactorForComponent(&m_editor);
}

void SubbertoneAudioProcessorEditor::setupKnob(juce::Slider& slider, juce::Label& label, const juce::String& name,
                                               const juce::String& paramId, const juce::String& tooltip,
                                               juce::Colour accent, Format format)
{
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 84, 18);
    slider.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    slider.setColour(juce::Slider::rotarySliderFillColourId, accent);
    slider.setTooltip(tooltip);

    switch (format)
    {
        case Format::Percent:
            slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v)) + "%"; };
            break;
        case Format::Hertz:
            slider.textFromValueFunction = [](double v) { return formatHertz(v); };
            break;
        case Format::Decibels:
            slider.textFromValueFunction = [](double v) { return signedText(v, " dB"); };
            break;
        case Format::Octaves:
            slider.textFromValueFunction = [](double v) { return signedText(v, " oct"); };
            break;
        case Format::Milliseconds:
            slider.textFromValueFunction = [](double v) { return v >= 1000.0 ? juce::String(v / 1000.0, 2) + " s"
                                                                               : juce::String(juce::roundToInt(v)) + " ms"; };
            break;
        case Format::Signed:
            slider.textFromValueFunction = [](double v) { const int i = juce::roundToInt(v); return (i > 0 ? "+" : "") + juce::String(i); };
            break;
    }

    if (auto* param = m_audioProcessor.m_parameters.getParameter(paramId))
    {
        // Typed values go through the parameter's own parser, so the knob box reads text exactly
        // as the host does (and as it is displayed): "1.2 s" is 1200 ms, "2.5 kHz" is 2500 Hz.
        slider.valueFromTextFunction = [param](const juce::String& text)
        {
            return static_cast<double>(param->convertFrom0to1(param->getValueForText(text)));
        };
        slider.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));
    }

    slider.updateText();

    m_content.addAndMakeVisible(slider);

    // The value box was created before the slider had a parent, i.e. with the
    // default LookAndFeel. Setting a colour makes the slider rebuild it via ours.
    slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    label.setText(name.toUpperCase(), juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, Theme::Colours::textDim);
    label.setInterceptsMouseClicks(false, false);
    m_content.addAndMakeVisible(label);
}

void SubbertoneAudioProcessorEditor::setupChoice(juce::ComboBox& combo, const juce::String& paramId, const juce::String& tooltip,
                                                 std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& attachment)
{
    // Items must exist before the attachment is made so it can restore the selection.
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(m_audioProcessor.m_parameters.getParameter(paramId)))
        combo.addItemList(choice->choices, 1);

    combo.setTooltip(tooltip);
    m_content.addAndMakeVisible(combo);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(m_audioProcessor.m_parameters, paramId, combo);
}

void SubbertoneAudioProcessorEditor::setupCaption(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, Theme::Colours::textDim);
    label.setInterceptsMouseClicks(false, false);
    m_content.addAndMakeVisible(label);
}

//==============================================================================
void SubbertoneAudioProcessorEditor::resized()
{
    // Uniform scale of the design-size content to the editor's current size.
    const float scale = static_cast<float>(getWidth()) / static_cast<float>(c_baseWidth);
    m_content.setBounds(0, 0, c_baseWidth, c_baseHeight);
    m_content.setTransform(juce::AffineTransform::scale(scale));

    if (m_sizeRestored)
        m_audioProcessor.setViewValue(c_viewEditorScale, scale);

    // The update popup covers the editor at its own scale (like the alerts), its card centred.
    if (m_updatePopup != nullptr)
        m_updatePopup->setBounds(getLocalBounds());
}

void SubbertoneAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(Theme::Colours::background);
}

void SubbertoneAudioProcessorEditor::layoutContent()
{
    auto bounds = m_content.getLocalBounds();

    auto header = bounds.removeFromTop(c_headerHeight).reduced(c_margin, 0);
    m_aboutButton.setBounds(header.removeFromRight(26).withSizeKeepingCentre(26, 26));

    // Website link sits after the title and tagline (widths match paintContent()).
    {
        const float titleWidth = juce::GlyphArrangement::getStringWidth(m_fonts->heading(20.0f), "SUBBERTONE");
        const float taglineWidth = juce::GlyphArrangement::getStringWidth(m_fonts->label(10.5f), "SAMMYJS  /  SUBHARMONIC GENERATOR  /");
        const int linkX = c_margin + 30 + 12 + juce::roundToInt(titleWidth) + 4 + 14 + juce::roundToInt(taglineWidth) + 8;
        const int linkWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(m_fonts->label(10.5f), "SWEETJUSTICESOUND.COM")) + 6;
        m_websiteLink.setBounds(linkX, 2 + (c_headerHeight - 20) / 2, linkWidth, 20);
    }

    // Preset bar, right-aligned next to the About button.
    header.removeFromRight(14);
    auto presetRow = header.removeFromRight(400).withSizeKeepingCentre(400, 26);
    m_presetSaveButton.setBounds(presetRow.removeFromRight(64));
    presetRow.removeFromRight(8);
    m_presetNextButton.setBounds(presetRow.removeFromRight(26));
    presetRow.removeFromRight(4);
    m_presetPrevButton.setBounds(presetRow.removeFromLeft(26));
    presetRow.removeFromLeft(4);
    m_presetNameButton.setBounds(presetRow);

    bounds.reduce(c_margin, 0);
    bounds.removeFromBottom(c_margin);

    auto controls = bounds.removeFromBottom(c_controlsHeight);
    bounds.removeFromBottom(c_sectionGap);
    m_stripPanel = bounds.removeFromBottom(c_stripPanelHeight);
    bounds.removeFromBottom(c_sectionGap);
    m_waveformVisualizer.setBounds(bounds);

    m_harmonicStrip->setBounds(m_stripPanel.withTrimmedTop(26).reduced(12, 6).withTrimmedBottom(2));

    // PHASE ALIGN: a pill in the HARMONICS header, after the title (dot at +12, title at +26 in
    // heading(11), as paintContent() draws them); the usage hint stays right-aligned.
    {
        const float titleWidth = juce::GlyphArrangement::getStringWidth(m_fonts->heading(11.0f), "HARMONICS");
        const float labelWidth = juce::GlyphArrangement::getStringWidth(m_fonts->label(10.0f), "PHASE ALIGN");
        m_phaseAlignButton.setBounds(m_stripPanel.getX() + 26 + juce::roundToInt(titleWidth) + 14, m_stripPanel.getY() + 3,
                                     juce::roundToInt(labelWidth) + 20, 20);
    }

    // Units per section: Detect 3, Drive 3, Filter 4, Overtones 2, Output 4, Master 4.
    constexpr std::array<int, 6> units { 3, 3, 4, 2, 4, 4 };
    const int totalUnits = 20;
    const int unit = (controls.getWidth() - 5 * c_sectionGap) / totalUnits;

    for (size_t i = 0; i < m_sections.size(); ++i)
    {
        m_sections[i].bounds = (i + 1 < m_sections.size()) ? controls.removeFromLeft(unit * units[i]) : controls;
        controls.removeFromLeft(c_sectionGap);
    }

    auto bodyOf = [](const Section& s) { return s.bounds.withTrimmedTop(c_sectionHeaderHeight).reduced(4, 6); };

    // A knob cell, then a caption + control filling the rest (Drive type, Overtones pattern).
    auto knobAndControl = [this](juce::Rectangle<int> body, juce::Slider& knob, juce::Label& knobLabel,
                                 juce::Label& caption, juce::Component& control, int knobCells)
    {
        const int cell = body.getWidth() / knobCells;
        layoutKnobRow(body.removeFromLeft(cell), { { &knob, &knobLabel } });
        caption.setBounds(body.removeFromTop(16));
        control.setBounds(body.reduced(6, 0).withSizeKeepingCentre(body.getWidth() - 12, 24).translated(0, -10));
    };

    layoutKnobRow(bodyOf(m_sections[0]), { { &m_pitchThresholdSlider, &m_pitchThresholdLabel },
                                           { &m_fundamentalLimitSlider, &m_fundamentalLimitLabel },
                                           { &m_glideSlider, &m_glideLabel } });

    knobAndControl(bodyOf(m_sections[1]), m_distortionSlider, m_distortionLabel, m_distortionTypeLabel, m_distortionTypeCombo, 3);

    layoutKnobRow(bodyOf(m_sections[2]), { { &m_toneSlider, &m_toneLabel },
                                           { &m_resonanceSlider, &m_resonanceLabel },
                                           { &m_envAmountSlider, &m_envAmountLabel },
                                           { &m_envReleaseSlider, &m_envReleaseLabel } });

    knobAndControl(bodyOf(m_sections[3]), m_overtoneAmountSlider, m_overtoneAmountLabel, m_patternLabel, m_patternButton, 2);

    layoutKnobRow(bodyOf(m_sections[4]), { { &m_loCutSlider, &m_loCutLabel },
                                           { &m_postDriveLowpassSlider, &m_postDriveLowpassLabel },
                                           { &m_mixSlider, &m_mixLabel },
                                           { &m_outputGainSlider, &m_outputGainLabel } });

    layoutKnobRow(bodyOf(m_sections[5]), { { &m_inflateSlider, &m_inflateLabel },
                                           { &m_inflateCurveSlider, &m_inflateCurveLabel },
                                           { &m_ceilingSlider, &m_ceilingLabel },
                                           { &m_limiterReleaseSlider, &m_limiterReleaseLabel } });

    // Header-row controls: filter mode selector, limiter switch (+ gain-reduction readout area).
    auto filterHeader = m_sections[2].bounds.withHeight(c_sectionHeaderHeight).reduced(8, 5);
    m_filterModeCombo.setBounds(filterHeader.removeFromRight(std::min(128, filterHeader.getWidth() - 76)));

    m_outputHeader = m_sections[4].bounds.withHeight(c_sectionHeaderHeight);
    m_loCutButton.setBounds(m_outputHeader.reduced(8, 5).removeFromRight(64));

    m_masterHeader = m_sections[5].bounds.withHeight(c_sectionHeaderHeight);
    auto masterHeader = m_masterHeader.reduced(8, 5);
    m_limiterButton.setBounds(masterHeader.removeFromRight(64));
}

void SubbertoneAudioProcessorEditor::layoutKnobRow(juce::Rectangle<int> area,
                                                   std::initializer_list<std::pair<juce::Slider*, juce::Label*>> knobs)
{
    const int cellWidth = area.getWidth() / static_cast<int>(knobs.size());

    for (const auto& [slider, label] : knobs)
    {
        auto cell = area.removeFromLeft(cellWidth);
        label->setBounds(cell.removeFromTop(16));
        slider->setBounds(cell);
    }
}

void SubbertoneAudioProcessorEditor::paintContent(juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll(background);

    // Header
    auto header = m_content.getLocalBounds().removeFromTop(c_headerHeight).reduced(c_margin, 0).toFloat();

    // Mark: amber sub wave under a teal input wave (one octave apart).
    const auto mark = header.removeFromLeft(30.0f).withSizeKeepingCentre(30.0f, 22.0f);
    juce::Path inputWave, subWave;
    for (int i = 0; i <= 40; ++i)
    {
        const float t = static_cast<float>(i) / 40.0f;
        const float x = mark.getX() + t * mark.getWidth();
        const juce::Point<float> a { x, mark.getCentreY() - 5.0f + 4.0f * std::sin(t * juce::MathConstants<float>::twoPi * 2.0f) };
        const juce::Point<float> b { x, mark.getCentreY() + 4.0f + 5.0f * std::sin(t * juce::MathConstants<float>::twoPi) };
        if (i == 0) { inputWave.startNewSubPath(a); subWave.startNewSubPath(b); }
        else        { inputWave.lineTo(a);          subWave.lineTo(b); }
    }
    g.setColour(input.withAlpha(0.8f));
    g.strokePath(inputWave, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(output);
    g.strokePath(subWave, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    header.removeFromLeft(12.0f);

    const auto titleFont = m_fonts->heading(20.0f);
    g.setFont(titleFont);
    g.setColour(text);
    const juce::String title = "SUBBERTONE";
    const float titleWidth = juce::GlyphArrangement::getStringWidth(titleFont, title);
    g.drawText(title, header.removeFromLeft(titleWidth + 4.0f), juce::Justification::centredLeft, false);

    header.removeFromLeft(14.0f);
    g.setFont(m_fonts->label(10.5f));
    g.setColour(textFaint);
    g.drawText("SAMMYJS  /  SUBHARMONIC GENERATOR  /", header.withTrimmedTop(4.0f), juce::Justification::centredLeft, false);

    auto drawPanel = [&](juce::Rectangle<int> bounds, const juce::String& panelTitle, juce::Colour accent, int headerHeight)
    {
        const auto r = bounds.toFloat();
        g.setColour(panel);
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(outlineSoft);
        g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, 1.0f);

        auto headerRow = r.withHeight(static_cast<float>(headerHeight)).reduced(12.0f, 0.0f);
        g.setColour(accent);
        g.fillEllipse(headerRow.removeFromLeft(6.0f).withSizeKeepingCentre(6.0f, 6.0f));
        headerRow.removeFromLeft(8.0f);
        g.setFont(m_fonts->heading(11.0f));
        g.setColour(text.withAlpha(0.85f));
        g.drawText(panelTitle, headerRow, juce::Justification::centredLeft, false);

        g.setColour(outlineSoft);
        g.drawHorizontalLine(bounds.getY() + headerHeight, r.getX() + 12.0f, r.getRight() - 12.0f);
        return headerRow;
    };

    // Harmonic strip panel, with a usage hint on the right of its header.
    {
        auto hintRow = drawPanel(m_stripPanel, "HARMONICS", overtones, 26);
        g.setFont(m_fonts->label(9.5f));
        g.setColour(textFaint);
        g.drawText("H1 = THE SUB   /   CLICK TO TOGGLE   /   DRAG TO SET   /   DRAG ACROSS TO DRAW   /   DOUBLE-CLICK RESETS",
                   hintRow.withTrimmedRight(4.0f), juce::Justification::centredRight, false);
    }

    for (const auto& section : m_sections)
        drawPanel(section.bounds, section.title, section.accent, c_sectionHeaderHeight);

    // Limiter gain-reduction readout in the MASTER header.
    if (m_limiterButton.getToggleState())
    {
        auto readout = m_masterHeader.reduced(8, 5).withTrimmedRight(64 + 8).toFloat();
        readout = readout.withTrimmedLeft(readout.getWidth() * 0.45f);
        const float reading = std::isfinite(m_lastGainReductionDb) ? std::min(0.0f, m_lastGainReductionDb) : 0.0f;
        const float gr = std::max(-c_grReadoutRangeDb, reading);

        auto meter = readout.removeFromRight(readout.getWidth() * 0.55f).withSizeKeepingCentre(readout.getWidth() * 1.2f, 4.0f);
        meter = meter.withX(readout.getRight() + 4.0f);
        g.setColour(outline);
        g.fillRoundedRectangle(meter, 2.0f);

        // The bar is full at c_grMeterRangeDb and stays inside its track (and off the text) beyond that.
        const float fill = juce::jlimit(0.0f, 1.0f, -gr / c_grMeterRangeDb);
        if (fill > 0.0f)
        {
            g.setColour(master);
            g.fillRoundedRectangle(meter.withLeft(meter.getRight() - meter.getWidth() * fill), 2.0f);
        }

        const auto font = m_fonts->body(10.0f);
        g.setFont(font);
        g.setColour(textDim);

        if (reading < -c_grReadoutRangeDb)
        {
            // "GR <-24", in two pieces: drawn as one, the font would join "<-" into an arrow.
            const auto limit = "-" + juce::String(juce::roundToInt(c_grReadoutRangeDb));
            g.drawText(limit, readout, juce::Justification::centredRight, false);
            g.drawText("GR <", readout.withTrimmedRight(juce::GlyphArrangement::getStringWidth(font, limit)),
                       juce::Justification::centredRight, false);
        }
        else
        {
            g.drawText("GR " + (gr > -0.05f ? juce::String("0.0") : juce::String(gr, 1)), readout,
                       juce::Justification::centredRight, false);
        }
    }

    // Signal-flow chevrons in the gaps between sections.
    for (size_t i = 0; i + 1 < m_sections.size(); ++i)
    {
        const float x = (static_cast<float>(m_sections[i].bounds.getRight()) + static_cast<float>(m_sections[i + 1].bounds.getX())) * 0.5f;
        const float y = static_cast<float>(m_sections[i].bounds.getY() + c_sectionHeaderHeight / 2);
        juce::Path chevron;
        chevron.startNewSubPath(x - 2.0f, y - 4.0f);
        chevron.lineTo(x + 2.0f, y);
        chevron.lineTo(x - 2.0f, y + 4.0f);
        g.setColour(textFaint);
        g.strokePath(chevron, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void SubbertoneAudioProcessorEditor::timerCallback()
{
    updatePresetDisplay();

    // A host can show its window without telling the editor (no visibility or hierarchy change); this
    // catches that. Once the check exists this is a single pointer test.
    if (m_updateCheck == nullptr)
        startUpdateCheckWhenShown();

    const float rms = m_audioProcessor.getCurrentSignalLevel();
    const float signalDb = rms > 1.0e-8f ? 20.0f * std::log10(rms) : -160.0f;
    const float threshold = static_cast<float>(m_pitchThresholdSlider.getValue());

    const juce::String levelText = signalDb <= -100.0f ? juce::String("IN  -inf dB")
                                                       : "IN  " + juce::String(signalDb, 1) + " dB";

    m_waveformVisualizer.setSignalText(levelText, signalDb > threshold);

    updateLimiterReadout();
}

void SubbertoneAudioProcessorEditor::updateLimiterReadout()
{
    // Repaint only the master header: when the readout appears or goes (LIMIT switched by a click,
    // automation or a preset; the button follows the parameter) and, while it shows, when it moves.
    const bool limiterOn = m_limiterButton.getToggleState();
    const float reading = m_audioProcessor.getGainReductionDb();
    const float gr = std::isfinite(reading) ? reading : 0.0f;

    if (limiterOn != m_limiterReadoutShown || (limiterOn && std::abs(gr - m_lastGainReductionDb) > 0.05f))
    {
        m_limiterReadoutShown = limiterOn;
        m_lastGainReductionDb = gr;
        m_content.repaint(m_masterHeader);
    }
}

//==============================================================================
// Harmonic patterns
//==============================================================================
void SubbertoneAudioProcessorEditor::showPatternMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel(&m_lookAndFeel); // a menu without a parent window would use JUCE's default look
    menu.addSectionHeader("HARMONIC PATTERN");

    const auto& patterns = harmonicPatterns();
    for (size_t i = 0; i < patterns.size(); ++i)
        menu.addItem(static_cast<int>(i) + 1, patterns[i].name);

    menu.addSeparator();
    const int subOffId = 1000, subOnId = 1001;
    menu.addItem(subOffId, "Sub off (overtones only, small-speaker trick)");
    menu.addItem(subOnId, "Sub on (H1 = 100%)");

    menu.showMenuAsync(SubbertoneLookAndFeel::menuOptionsFor(m_patternButton, m_patternButton.getWidth()),
                       [safe = juce::Component::SafePointer<SubbertoneAudioProcessorEditor>(this)](int result)
    {
        if (safe == nullptr || result == 0)
            return;

        if (result == 1000 || result == 1001)
        {
            if (auto* h1 = safe->m_audioProcessor.m_parameters.getParameter("harmonic1"))
            {
                h1->beginChangeGesture();
                h1->setValueNotifyingHost(h1->convertTo0to1(result == 1001 ? 100.0f : 0.0f));
                h1->endChangeGesture();
            }
            return;
        }

        const auto& p = harmonicPatterns()[static_cast<size_t>(result - 1)];
        safe->applyHarmonicPattern(p.levels, p.keepSubLevel);
    });
}

void SubbertoneAudioProcessorEditor::applyHarmonicPattern(const std::array<float, 17>& percent, bool keepSubLevel)
{
    for (int k = keepSubLevel ? 2 : 1; k <= 16; ++k)
    {
        if (auto* param = m_audioProcessor.m_parameters.getParameter("harmonic" + juce::String(k)))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost(param->convertTo0to1(percent[static_cast<size_t>(k)]));
            param->endChangeGesture();
        }
    }
}

//==============================================================================
// Presets
//==============================================================================
void SubbertoneAudioProcessorEditor::setupPresetBar()
{
    auto& presets = m_audioProcessor.getPresetManager();

    m_presetPrevButton.setButtonText("<");
    m_presetPrevButton.setTooltip("Previous preset");
    m_presetPrevButton.onClick = [this] { m_audioProcessor.getPresetManager().loadPrevious(); updatePresetDisplay(); };

    m_presetNextButton.setButtonText(">");
    m_presetNextButton.setTooltip("Next preset");
    m_presetNextButton.onClick = [this] { m_audioProcessor.getPresetManager().loadNext(); updatePresetDisplay(); };

    m_presetNameButton.setTooltip("Choose a preset, or save, delete and find your presets");
    m_presetNameButton.onClick = [this] { showPresetMenu(); };

    m_presetSaveButton.setButtonText("SAVE");
    m_presetSaveButton.setTooltip("Save the current settings as a preset");
    m_presetSaveButton.onClick = [this]
    {
        auto& pm = m_audioProcessor.getPresetManager();
        pm.refresh(); // a preset renamed or removed outside the plugin is not written back blindly
        const auto current = pm.getSnapshot();

        // Editing a user preset that is still in the folder: Save writes back to its file.
        // Otherwise ask for a name (and before replacing anything).
        if (current.currentIsUser && current.currentIndex >= 0)
            savePresetAs(current.currentName);
        else
            promptSavePreset(current.currentName == "Init" ? juce::String() : current.currentName);
    };

    for (auto* b : { &m_presetPrevButton, &m_presetNameButton, &m_presetNextButton, &m_presetSaveButton })
        m_content.addAndMakeVisible(b);

    presets.refresh();
    updatePresetDisplay();
}

void SubbertoneAudioProcessorEditor::updatePresetDisplay()
{
    const auto& pm = m_audioProcessor.getPresetManager();
    const auto text = pm.getCurrentName() + (pm.isModified() ? " *" : "");

    if (m_presetNameButton.getButtonText() != text)
        m_presetNameButton.setButtonText(text);
}

void SubbertoneAudioProcessorEditor::showPresetMenu()
{
    auto& pm = m_audioProcessor.getPresetManager();
    pm.refresh(); // pick up presets added or removed outside the plugin

    // One consistent view of the list and the current preset (a host may change either meanwhile).
    const auto snapshot = pm.getSnapshot();
    const auto& presets = snapshot.presets;
    const auto isCurrentFactory = [&snapshot](int i) { return snapshot.currentIsFactory && i == snapshot.currentIndex; };

    juce::PopupMenu menu;
    menu.setLookAndFeel(&m_lookAndFeel); // submenus take it from their parent menu
    menu.addSectionHeader("FACTORY");

    // Top-level factory presets (Init), then one submenu per category, in a fixed order.
    for (int i = 0; i < presets.size(); ++i)
        if (presets[i].isFactory && presets[i].category.isEmpty())
            menu.addItem(i + 1, presets[i].name, true, isCurrentFactory(i));

    for (const char* category : { "Music", "Character", "Translation", "Engines", "Utility" })
    {
        juce::PopupMenu sub;
        bool containsCurrent = false;

        for (int i = 0; i < presets.size(); ++i)
        {
            if (presets[i].isFactory && presets[i].category == category)
            {
                sub.addItem(i + 1, presets[i].name, true, isCurrentFactory(i));
                containsCurrent = containsCurrent || isCurrentFactory(i);
            }
        }

        if (sub.getNumItems() > 0)
            menu.addSubMenu(category, sub, true, nullptr, containsCurrent);
    }

    // User presets are told apart by their file: two can carry the same name, in which case
    // menuName adds the file name. Items load (and Delete removes) exactly that file.
    juce::Array<juce::File> userFiles;
    juce::PopupMenu user;
    bool userContainsCurrent = false;
    juce::String currentMenuName = snapshot.currentName;

    for (const auto& preset : presets)
    {
        if (preset.isFactory)
            continue;

        const bool isCurrent = snapshot.currentIsUser && preset.file == snapshot.currentFile;
        user.addItem(c_firstUserItemId + userFiles.size(), preset.menuName, true, isCurrent);
        userFiles.add(preset.file);

        if (isCurrent)
        {
            userContainsCurrent = true;
            currentMenuName = preset.menuName;
        }
    }

    menu.addSectionHeader("USER");
    if (!userFiles.isEmpty())
        menu.addSubMenu("User Presets", user, true, nullptr, userContainsCurrent); // keeps long lists tidy
    else
        menu.addItem(-1, "(none saved yet)", false);

    // Delete is bound to the preset named here, even if another one is current by the time it is confirmed.
    const auto fileToDelete = snapshot.currentIsUser ? snapshot.currentFile : juce::File();

    menu.addSeparator();
    menu.addItem(c_saveAsItemId, "Save As...");
    menu.addItem(c_deleteItemId, "Delete \"" + currentMenuName + "\"", fileToDelete != juce::File());
    menu.addItem(c_revealItemId, "Show Presets Folder");

    menu.showMenuAsync(SubbertoneLookAndFeel::menuOptionsFor(m_presetNameButton, m_presetNameButton.getWidth()),
                       [safe = juce::Component::SafePointer<SubbertoneAudioProcessorEditor>(this), userFiles, fileToDelete,
                        nameToDelete = currentMenuName](int result)
    {
        if (safe == nullptr || result == 0)
            return;

        auto& presetManager = safe->m_audioProcessor.getPresetManager();

        if (result == c_saveAsItemId)
        {
            safe->promptSavePreset(presetManager.isCurrentUserPreset() ? presetManager.getCurrentName() : juce::String());
        }
        else if (result == c_deleteItemId)
        {
            safe->confirmDeletePreset(fileToDelete, nameToDelete);
        }
        else if (result == c_revealItemId)
        {
            const auto dir = PresetManager::getUserPresetDirectory();
            dir.createDirectory();
            dir.startAsProcess(); // opens the folder in Finder / Explorer
        }
        else if (result >= c_firstUserItemId)
        {
            if (const int i = result - c_firstUserItemId; i < userFiles.size())
                presetManager.loadUserPreset(userFiles[i]);

            safe->updatePresetDisplay();
        }
        else if (result >= 1 && result <= PresetManager::getNumFactory())
        {
            presetManager.loadPreset(result - 1); // factory presets lead the list, in a fixed order
            safe->updatePresetDisplay();
        }
    });
}

void SubbertoneAudioProcessorEditor::confirmDeletePreset(const juce::File& file, const juce::String& displayName)
{
    auto alert = makeAlert("Delete Preset", "Delete \"" + displayName + "\"? This removes the preset file.",
                           juce::MessageBoxIconType::WarningIcon);
    alert->addButton("Delete", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    showAlert(std::move(alert), [safe = juce::Component::SafePointer<SubbertoneAudioProcessorEditor>(this), file](int result, juce::AlertWindow&)
    {
        if (safe == nullptr || result != 1)
            return;

        // Exactly the file the dialog named, whatever the host has recalled since. Nothing
        // happens if it has gone (deleteUserPreset() returns false).
        safe->m_audioProcessor.getPresetManager().deleteUserPreset(file);
        safe->updatePresetDisplay();
    });
}

void SubbertoneAudioProcessorEditor::promptSavePreset(const juce::String& suggestedName)
{
    auto alert = makeAlert("Save Preset", "Name this preset:", juce::MessageBoxIconType::NoIcon);
    alert->addTextEditor("name", suggestedName);
    if (auto* field = alert->getTextEditor("name"))
    {
        field->setJustification(juce::Justification::centredLeft); // vertically centred in the field
        field->setIndents(8, 0);
    }
    alert->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    showAlert(std::move(alert), [safe = juce::Component::SafePointer<SubbertoneAudioProcessorEditor>(this)](int result, juce::AlertWindow& window)
    {
        if (safe == nullptr || result != 1)
            return;

        const auto name = window.getTextEditorContents("name").trim(); // read before another alert replaces this one
        if (name.isNotEmpty())
            safe->checkNameAndSave(name);
    });
}

void SubbertoneAudioProcessorEditor::checkNameAndSave(const juce::String& name)
{
    auto& pm = m_audioProcessor.getPresetManager();

    if (!PresetManager::isValidPresetName(name))
    {
        // E.g. "???": nothing is left once the characters a file name can't hold are dropped.
        auto alert = makeAlert("Can't Use This Name",
                               "\"" + name + "\" can't be used as a preset name, as it has no characters "
                               "a file name can hold. Please use letters or numbers.",
                               juce::MessageBoxIconType::WarningIcon);
        alert->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        showAlert(std::move(alert), [safe = juce::Component::SafePointer<SubbertoneAudioProcessorEditor>(this), name](int result, juce::AlertWindow&)
        {
            if (safe != nullptr && result == 1)
                safe->promptSavePreset(name); // try again, with the name as typed
        });
        return;
    }

    // Ask before replacing ANY existing file: a preset with this name, or a file this name maps to
    // (file names ignore case, and characters such as '/' are dropped, so "AB" replaces "A/B").
    if (pm.userPresetExists(name))
        confirmOverwrite(name, pm.getNameOfPresetSavedAs(name));
    else
        savePresetAs(name);
}

void SubbertoneAudioProcessorEditor::confirmOverwrite(const juce::String& name, const juce::String& replacedName)
{
    const auto message = replacedName.isEmpty() || replacedName == name
                       ? "A preset called \"" + name + "\" already exists. Replace it?"
                       : "Saving as \"" + name + "\" replaces the preset \"" + replacedName + "\" (the two names give the same "
                         "file name). Replace \"" + replacedName + "\"?";

    auto alert = makeAlert("Replace Preset", message, juce::MessageBoxIconType::QuestionIcon);
    alert->addButton("Replace", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    showAlert(std::move(alert), [safe = juce::Component::SafePointer<SubbertoneAudioProcessorEditor>(this), name, replacedName](int result, juce::AlertWindow&)
    {
        if (safe == nullptr || result != 1)
            return;

        // The folder may have changed while the question was up: ask again if the save would now
        // replace a different preset.
        const auto nowReplaced = safe->m_audioProcessor.getPresetManager().getNameOfPresetSavedAs(name);
        if (nowReplaced.isNotEmpty() && nowReplaced != replacedName)
            safe->confirmOverwrite(name, nowReplaced);
        else
            safe->savePresetAs(name);
    });
}

void SubbertoneAudioProcessorEditor::savePresetAs(const juce::String& name)
{
    if (!m_audioProcessor.getPresetManager().saveUserPreset(name))
    {
        auto alert = makeAlert("Couldn't Save Preset",
                               "The preset could not be written to:\n" + PresetManager::getUserPresetDirectory().getFullPathName(),
                               juce::MessageBoxIconType::WarningIcon);
        alert->addButton("OK", 0, juce::KeyPress(juce::KeyPress::returnKey));
        showAlert(std::move(alert), {});
    }

    updatePresetDisplay();
}

std::unique_ptr<juce::AlertWindow> SubbertoneAudioProcessorEditor::makeAlert(const juce::String& title, const juce::String& message,
                                                                            juce::MessageBoxIconType icon)
{
    // Centred over the plugin window and drawn at the editor's own scale (not the design-size content's).
    auto alert = std::make_unique<juce::AlertWindow>(title, message, icon, this);

    // Before addButton() / addTextEditor(): they take button sizes and the field's font and outline from the
    // alert's LookAndFeel as they are called.
    alert->setLookAndFeel(&m_lookAndFeel);
    alert->setOpaque(false); // a rounded panel: the corners show what is behind the dialog
    SubbertoneLookAndFeel::showFocusRingsAfterTab(*alert);
    return alert;
}

void SubbertoneAudioProcessorEditor::showAlert(std::unique_ptr<juce::AlertWindow> alert,
                                               std::function<void(int, juce::AlertWindow&)> onResult)
{
    // Plugins can't run modal loops, so alerts are async. The editor owns the
    // window so it is always destroyed before the LookAndFeel it uses.
    m_alert = std::move(alert);
    m_alert->setLookAndFeel(&m_lookAndFeel); // already set by makeAlert(); kept for any alert made elsewhere

    m_alert->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe = juce::Component::SafePointer<SubbertoneAudioProcessorEditor>(this), onResult](int result)
        {
            if (safe == nullptr || safe->m_alert == nullptr)
                return;

            if (onResult)
                onResult(result, *safe->m_alert);

            // Defer the delete: we're inside the window's own callback.
            juce::MessageManager::callAsync([safe]
            {
                if (safe != nullptr && safe->m_alert != nullptr && !safe->m_alert->isCurrentlyModal())
                    safe->m_alert.reset();
            });
        }), false);
}

void SubbertoneAudioProcessorEditor::showAboutWindow()
{
    // One About window per editor: another click brings it back to the front. The editor owns it,
    // so it is closed (and its update check stopped) when the editor goes.
    if (m_aboutWindow != nullptr)
    {
        m_aboutWindow->setMinimised(false);
        m_aboutWindow->toFront(true);
        return;
    }

    m_aboutWindow = std::make_unique<AboutWindow>(m_lookAndFeel, [this] { m_aboutWindow.reset(); });
}

//==============================================================================
void SubbertoneAudioProcessorEditor::parentHierarchyChanged()
{
    juce::AudioProcessorEditor::parentHierarchyChanged();
    startUpdateCheckWhenShown();
}

void SubbertoneAudioProcessorEditor::visibilityChanged()
{
    juce::AudioProcessorEditor::visibilityChanged();
    startUpdateCheckWhenShown();
}

void SubbertoneAudioProcessorEditor::startUpdateCheckWhenShown()
{
    // Once per editor, and only when it is on screen in a host: an editor that is only built (a test,
    // a snapshot, an offline render) never goes online. Never from the processor or the audio thread.
    if (m_updateCheck != nullptr || !isShowing())
        return;

    m_updateCheck = std::make_unique<sammyj::UpdateCheck>("subbertone", "subbertone-v", JucePlugin_VersionString,
        [this](const sammyj::UpdateInfo& info) { showUpdatePopup(info); });
}

void SubbertoneAudioProcessorEditor::showUpdatePopup(const sammyj::UpdateInfo& info)
{
    if (m_updatePopup != nullptr)
        return;

    m_updatePopup = std::make_unique<UpdatePopup>(m_lookAndFeel, info, [this] { closeUpdatePopup(); });

    // The scope's OpenGL view is a native view on top of the editor's window, so nothing the editor paints
    // can cover it: it is hidden while the popup is up (which also pauses its rendering).
    m_waveformVisualizer.setVisible(false);

    addAndMakeVisible(*m_updatePopup); // last child: above the scaled content
    m_updatePopup->setBounds(getLocalBounds());
    m_updatePopup->toFront(false);

    // Return / Escape answer it, as in the alerts; not while an alert is up (it keeps the keys until it closes).
    if (m_updatePopup->isShowing() && !m_updatePopup->isCurrentlyBlockedByAnotherModalComponent())
        m_updatePopup->grabKeyboardFocus();
}

void SubbertoneAudioProcessorEditor::closeUpdatePopup()
{
    m_updatePopup.reset();
    m_waveformVisualizer.setVisible(true);
}

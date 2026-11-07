#include "PluginEditor.h"

#include "AboutWindow.h"

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

    double parseNumber(const juce::String& text)
    {
        const double value = text.retainCharacters("0123456789.-").getDoubleValue();
        return text.containsIgnoreCase("k") ? value * 1000.0 : value;
    }

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
    , m_tooltipWindow(this, 700)
{
    setLookAndFeel(&m_lookAndFeel);
    m_tooltipWindow.setLookAndFeel(&m_lookAndFeel);

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
    m_content.addAndMakeVisible(m_limiterButton);

    m_aboutButton.setButtonText("?");
    m_aboutButton.setTooltip("About SammyJs Subbertone");
    m_aboutButton.onClick = [this] { showAboutWindow(); };
    m_content.addAndMakeVisible(m_aboutButton);

    setupPresetBar();

    m_websiteLink.setFont(m_fonts->label(10.5f), false, juce::Justification::centredLeft);
    m_websiteLink.setColour(juce::HyperlinkButton::textColourId, Theme::Colours::textDim);
    m_websiteLink.setTooltip("Open sweetjusticesound.com");
    m_websiteLink.onStateChange = [this]
    {
        m_websiteLink.setColour(juce::HyperlinkButton::textColourId,
                                m_websiteLink.isOver() ? Theme::Colours::output : Theme::Colours::textDim);
    };
    m_content.addAndMakeVisible(m_websiteLink);

    // Resizable, uniformly scaled, fixed aspect ratio; the size is part of the saved state.
    // Read the saved scale first: setting the limits below resizes the editor, which
    // would otherwise overwrite it.
    const float scale = juce::jlimit(c_minScale, c_maxScale,
                                     static_cast<float>(m_audioProcessor.getViewValue(c_viewEditorScale, c_defaultScale)));
    setResizable(true, true);
    setResizeLimits(juce::roundToInt(c_baseWidth * c_minScale), juce::roundToInt(c_baseHeight * c_minScale),
                    juce::roundToInt(c_baseWidth * c_maxScale), juce::roundToInt(c_baseHeight * c_maxScale));
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio(static_cast<double>(c_baseWidth) / c_baseHeight);

    setSize(juce::roundToInt(c_baseWidth * scale), juce::roundToInt(c_baseHeight * scale));
    m_sizeRestored = true;

    startTimerHz(20);
}

SubbertoneAudioProcessorEditor::~SubbertoneAudioProcessorEditor()
{
    stopTimer();
    m_alert.reset();
    m_tooltipWindow.setLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
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

    slider.valueFromTextFunction = [](const juce::String& text) { return parseNumber(text); };
    slider.updateText();

    if (auto* param = m_audioProcessor.m_parameters.getParameter(paramId))
        slider.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));

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
        const float gr = juce::jlimit(-24.0f, 0.0f, m_lastGainReductionDb);

        auto meter = readout.removeFromRight(readout.getWidth() * 0.55f).withSizeKeepingCentre(readout.getWidth() * 1.2f, 4.0f);
        meter = meter.withX(readout.getRight() + 4.0f);
        g.setColour(outline);
        g.fillRoundedRectangle(meter, 2.0f);
        g.setColour(master);
        g.fillRoundedRectangle(meter.withLeft(meter.getRight() - meter.getWidth() * (-gr / 12.0f)).withRight(meter.getRight()), 2.0f);

        g.setFont(m_fonts->body(10.0f));
        g.setColour(textDim);
        g.drawText("GR " + juce::String(gr, 1), readout, juce::Justification::centredRight, false);
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

    const float rms = m_audioProcessor.getCurrentSignalLevel();
    const float signalDb = rms > 1.0e-8f ? 20.0f * std::log10(rms) : -160.0f;
    const float threshold = static_cast<float>(m_pitchThresholdSlider.getValue());

    const juce::String levelText = signalDb <= -100.0f ? juce::String("IN  -inf dB")
                                                       : "IN  " + juce::String(signalDb, 1) + " dB";

    m_waveformVisualizer.setSignalText(levelText, signalDb > threshold);

    // Gain-reduction readout: repaint only the master header, only when it moves.
    const float gr = m_audioProcessor.getGainReductionDb();
    if (std::abs(gr - m_lastGainReductionDb) > 0.05f)
    {
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
    menu.addSectionHeader("HARMONIC PATTERN");

    const auto& patterns = harmonicPatterns();
    for (size_t i = 0; i < patterns.size(); ++i)
        menu.addItem(static_cast<int>(i) + 1, patterns[i].name);

    menu.addSeparator();
    const int subOffId = 1000, subOnId = 1001;
    menu.addItem(subOffId, "Sub off (overtones only, small-speaker trick)");
    menu.addItem(subOnId, "Sub on (H1 = 100%)");

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&m_patternButton),
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
        // Editing a user preset: Save updates it. Otherwise ask for a name.
        if (pm.isCurrentUserPreset())
            savePresetAs(pm.getCurrentName());
        else
            promptSavePreset(pm.getCurrentName() == "Init" ? juce::String() : pm.getCurrentName());
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

    const auto& presets = pm.getPresets();
    const int current = pm.getCurrentIndex();

    juce::PopupMenu menu;
    menu.addSectionHeader("FACTORY");

    // Top-level factory presets (Init), then one submenu per category, in a fixed order.
    for (int i = 0; i < presets.size(); ++i)
        if (presets[i].isFactory && presets[i].category.isEmpty())
            menu.addItem(i + 1, presets[i].name, true, i == current);

    for (const char* category : { "Music", "Character", "Translation", "Engines", "Utility" })
    {
        juce::PopupMenu sub;
        bool containsCurrent = false;

        for (int i = 0; i < presets.size(); ++i)
        {
            if (presets[i].isFactory && presets[i].category == category)
            {
                sub.addItem(i + 1, presets[i].name, true, i == current);
                containsCurrent = containsCurrent || (i == current && !pm.isCurrentUserPreset());
            }
        }

        if (sub.getNumItems() > 0)
            menu.addSubMenu(category, sub, true, nullptr, containsCurrent);
    }

    juce::PopupMenu user;
    bool anyUser = false;
    for (int i = 0; i < presets.size(); ++i)
        if (!presets[i].isFactory)
        {
            user.addItem(i + 1, presets[i].name, true, i == current && pm.isCurrentUserPreset());
            anyUser = true;
        }

    menu.addSectionHeader("USER");
    if (anyUser)
        menu.addSubMenu("User Presets", user); // keeps long lists tidy
    else
        menu.addItem(-1, "(none saved yet)", false);

    constexpr int saveAsId = 10001, deleteId = 10002, revealId = 10003;
    menu.addSeparator();
    menu.addItem(saveAsId, "Save As...");
    menu.addItem(deleteId, "Delete \"" + pm.getCurrentName() + "\"", pm.isCurrentUserPreset());
    menu.addItem(revealId, "Show Presets Folder");

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&m_presetNameButton),
                       [safe = juce::Component::SafePointer<SubbertoneAudioProcessorEditor>(this)](int result)
    {
        if (safe == nullptr || result == 0)
            return;

        auto& presetManager = safe->m_audioProcessor.getPresetManager();

        if (result == saveAsId)
        {
            safe->promptSavePreset(presetManager.isCurrentUserPreset() ? presetManager.getCurrentName() : juce::String());
        }
        else if (result == deleteId)
        {
            auto alert = std::make_unique<juce::AlertWindow>("Delete Preset",
                                                             "Delete \"" + presetManager.getCurrentName() + "\"? This removes the preset file.",
                                                             juce::MessageBoxIconType::WarningIcon);
            alert->addButton("Delete", 1, juce::KeyPress(juce::KeyPress::returnKey));
            alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            safe->showAlert(std::move(alert), [safe](int r, juce::AlertWindow&)
            {
                if (safe != nullptr && r == 1)
                {
                    auto& p = safe->m_audioProcessor.getPresetManager();
                    p.deleteUserPreset(p.getCurrentIndex());
                    safe->updatePresetDisplay();
                }
            });
        }
        else if (result == revealId)
        {
            const auto dir = PresetManager::getUserPresetDirectory();
            dir.createDirectory();
            dir.startAsProcess(); // opens the folder in Finder / Explorer
        }
        else if (result > 0)
        {
            presetManager.loadPreset(result - 1);
            safe->updatePresetDisplay();
        }
    });
}

void SubbertoneAudioProcessorEditor::promptSavePreset(const juce::String& suggestedName)
{
    auto alert = std::make_unique<juce::AlertWindow>("Save Preset", "Name this preset:", juce::MessageBoxIconType::NoIcon);
    alert->addTextEditor("name", suggestedName);
    alert->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    showAlert(std::move(alert), [safe = juce::Component::SafePointer<SubbertoneAudioProcessorEditor>(this)](int result, juce::AlertWindow& window)
    {
        if (safe == nullptr || result != 1)
            return;

        const auto name = window.getTextEditorContents("name").trim();
        if (name.isEmpty())
            return;

        if (safe->m_audioProcessor.getPresetManager().userPresetExists(name))
            safe->confirmOverwrite(name);
        else
            safe->savePresetAs(name);
    });
}

void SubbertoneAudioProcessorEditor::confirmOverwrite(const juce::String& name)
{
    auto alert = std::make_unique<juce::AlertWindow>("Replace Preset",
                                                     "A preset called \"" + name + "\" already exists. Replace it?",
                                                     juce::MessageBoxIconType::QuestionIcon);
    alert->addButton("Replace", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    showAlert(std::move(alert), [safe = juce::Component::SafePointer<SubbertoneAudioProcessorEditor>(this), name](int result, juce::AlertWindow&)
    {
        if (safe != nullptr && result == 1)
            safe->savePresetAs(name);
    });
}

void SubbertoneAudioProcessorEditor::savePresetAs(const juce::String& name)
{
    if (!m_audioProcessor.getPresetManager().saveUserPreset(name))
    {
        auto alert = std::make_unique<juce::AlertWindow>("Couldn't Save Preset",
                                                         "The preset could not be written to:\n" + PresetManager::getUserPresetDirectory().getFullPathName(),
                                                         juce::MessageBoxIconType::WarningIcon);
        alert->addButton("OK", 0, juce::KeyPress(juce::KeyPress::returnKey));
        showAlert(std::move(alert), {});
    }

    updatePresetDisplay();
}

void SubbertoneAudioProcessorEditor::showAlert(std::unique_ptr<juce::AlertWindow> alert,
                                               std::function<void(int, juce::AlertWindow&)> onResult)
{
    // Plugins can't run modal loops, so alerts are async. The editor owns the
    // window so it is always destroyed before the LookAndFeel it uses.
    m_alert = std::move(alert);
    m_alert->setLookAndFeel(&m_lookAndFeel);

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
    new AboutWindow(); // owns and deletes itself on close
}

//==============================================================================
// LookAndFeel
//==============================================================================
SubbertoneAudioProcessorEditor::LookAndFeel::LookAndFeel(const Theme::Fonts& fonts)
    : m_fonts(fonts)
{
    using namespace Theme::Colours;

    if (fonts.medium != nullptr)
        setDefaultSansSerifTypeface(fonts.medium);

    setColour(juce::ResizableWindow::backgroundColourId, background);
    setColour(juce::Label::textColourId, text);

    setColour(juce::Slider::textBoxTextColourId, text);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxHighlightColourId, harmonics.withAlpha(0.35f));
    setColour(juce::TextEditor::focusedOutlineColourId, outline);

    setColour(juce::ComboBox::backgroundColourId, panelRaised);
    setColour(juce::ComboBox::outlineColourId, outline);
    setColour(juce::ComboBox::textColourId, text);
    setColour(juce::ComboBox::arrowColourId, harmonics);
    setColour(juce::ComboBox::focusedOutlineColourId, harmonics.withAlpha(0.6f));

    setColour(juce::PopupMenu::backgroundColourId, panelRaised);
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, harmonics.withAlpha(0.18f));
    setColour(juce::PopupMenu::highlightedTextColourId, text);

    setColour(juce::TextButton::buttonColourId, panelRaised);
    setColour(juce::TextButton::buttonOnColourId, panelRaised.brighter(0.1f));
    setColour(juce::TextButton::textColourOffId, textDim);
    setColour(juce::TextButton::textColourOnId, text);

    setColour(juce::AlertWindow::backgroundColourId, panel);
    setColour(juce::AlertWindow::textColourId, text);
    setColour(juce::AlertWindow::outlineColourId, outline);
    setColour(juce::TextEditor::backgroundColourId, panelRaised);
    setColour(juce::TextEditor::textColourId, text);
    setColour(juce::TextEditor::outlineColourId, outline);
    setColour(juce::TextEditor::highlightColourId, output.withAlpha(0.3f));
    setColour(juce::CaretComponent::caretColourId, output);
    setColour(juce::PopupMenu::headerTextColourId, textFaint);

    setColour(juce::TooltipWindow::backgroundColourId, panelRaised);
    setColour(juce::TooltipWindow::textColourId, text);
    setColour(juce::TooltipWindow::outlineColourId, outline);
}

void SubbertoneAudioProcessorEditor::LookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                                                   float sliderPos, float rotaryStartAngle,
                                                                   float rotaryEndAngle, juce::Slider& slider)
{
    using namespace Theme::Colours;

    const auto accent = slider.findColour(juce::Slider::rotarySliderFillColourId);
    const bool hot = slider.isMouseOverOrDragging();

    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(4.0f);
    const float diameter = std::min(area.getWidth(), area.getHeight());
    const auto centre = area.getCentre();
    const float radius = diameter * 0.5f;
    const float trackWidth = std::max(2.5f, radius * 0.09f);
    const float arcRadius = radius - trackWidth * 0.5f;
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // Track
    juce::Path track;
    track.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(outline);
    g.strokePath(track, juce::PathStrokeType(trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc: bipolar parameters (dB gain) grow from the centre.
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0 && juce::approximatelyEqual(slider.getMinimum(), -slider.getMaximum());
    const float fromAngle = bipolar ? (rotaryStartAngle + rotaryEndAngle) * 0.5f : rotaryStartAngle;

    juce::Path value;
    value.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, std::min(fromAngle, angle), std::max(fromAngle, angle), true);
    g.setColour(accent.withAlpha(hot ? 1.0f : 0.9f));
    g.strokePath(value, juce::PathStrokeType(trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Body
    const float bodyRadius = arcRadius - trackWidth * 1.6f;
    const auto body = juce::Rectangle<float>(bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre(centre);

    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillEllipse(body.translated(0.0f, 2.0f));

    g.setGradientFill(juce::ColourGradient(panelRaised.brighter(hot ? 0.18f : 0.1f), centre.x, body.getY(),
                                           panelRaised.darker(0.25f), centre.x, body.getBottom(), false));
    g.fillEllipse(body);
    g.setColour(outline.brighter(0.15f));
    g.drawEllipse(body.reduced(0.5f), 1.0f);

    // Pointer
    const auto direction = juce::Point<float>(std::sin(angle), -std::cos(angle));
    const auto p1 = centre + direction * (bodyRadius * 0.35f);
    const auto p2 = centre + direction * (bodyRadius * 0.82f);
    g.setColour(text);
    g.drawLine({ p1, p2 }, 2.0f);
}

juce::Slider::SliderLayout SubbertoneAudioProcessorEditor::LookAndFeel::getSliderLayout(juce::Slider& slider)
{
    juce::Slider::SliderLayout layout;

    if (slider.getTextBoxPosition() == juce::Slider::NoTextBox)
    {
        layout.sliderBounds = slider.getLocalBounds();
        return layout;
    }

    auto bounds = slider.getLocalBounds();
    layout.textBoxBounds = bounds.removeFromBottom(18).withSizeKeepingCentre(std::min(bounds.getWidth(), 84), 18);
    bounds.removeFromBottom(2);
    layout.sliderBounds = bounds;
    return layout;
}

juce::Label* SubbertoneAudioProcessorEditor::LookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
    label->setFont(m_fonts.body(12.0f));
    label->setJustificationType(juce::Justification::centred);
    label->setColour(juce::Label::textColourId, Theme::Colours::text);
    label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    label->setColour(juce::Label::outlineWhenEditingColourId, Theme::Colours::outline);
    label->setColour(juce::Label::backgroundWhenEditingColourId, Theme::Colours::panelRaised);
    return label;
}

juce::Font SubbertoneAudioProcessorEditor::LookAndFeel::getLabelFont(juce::Label& label)
{
    if (dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr)
        return m_fonts.body(12.0f);

    return m_fonts.label(10.5f);
}

void SubbertoneAudioProcessorEditor::LookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool,
                                                               int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    const bool hot = box.isMouseOver(true) || box.isPopupActive();

    g.setColour(box.findColour(juce::ComboBox::backgroundColourId).brighter(hot ? 0.08f : 0.0f));
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(hot ? box.findColour(juce::ComboBox::focusedOutlineColourId) : box.findColour(juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle(r, 5.0f, 1.0f);

    const float cx = r.getRight() - 13.0f;
    const float cy = r.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath(cx - 4.0f, cy - 2.0f);
    chevron.lineTo(cx, cy + 2.0f);
    chevron.lineTo(cx + 4.0f, cy - 2.0f);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId));
    g.strokePath(chevron, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font SubbertoneAudioProcessorEditor::LookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return m_fonts.body(12.0f);
}

void SubbertoneAudioProcessorEditor::LookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(8, 0, box.getWidth() - 30, box.getHeight());
    label.setFont(getComboBoxFont(box));
}

juce::Font SubbertoneAudioProcessorEditor::LookAndFeel::getPopupMenuFont()
{
    return m_fonts.body(13.0f);
}

void SubbertoneAudioProcessorEditor::LookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                                       const juce::Colour& backgroundColour,
                                                                       bool isMouseOverButton, bool isButtonDown)
{
    const auto r = button.getLocalBounds().toFloat().reduced(0.5f);
    const float corner = std::min(r.getWidth(), r.getHeight()) * 0.5f;

    g.setColour(backgroundColour.brighter(isButtonDown ? 0.2f : isMouseOverButton ? 0.1f : 0.0f));
    g.fillRoundedRectangle(r, corner);
    g.setColour(isMouseOverButton ? Theme::Colours::textDim : Theme::Colours::outline);
    g.drawRoundedRectangle(r, corner, 1.0f);
}

juce::Font SubbertoneAudioProcessorEditor::LookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return m_fonts.label(std::min(13.0f, static_cast<float>(buttonHeight) * 0.5f));
}

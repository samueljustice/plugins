#include "PluginEditor.h"

#include "AboutWindow.h"

namespace
{
    constexpr int c_width  = 1000;
    constexpr int c_height = 560;
    constexpr int c_margin = 16;
    constexpr int c_headerHeight = 56;
    constexpr int c_controlsHeight = 186;
    constexpr int c_sectionGap = 12;
    constexpr int c_sectionHeaderHeight = 32;

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
    , m_tooltipWindow(this, 700)
{
    setLookAndFeel(&m_lookAndFeel);
    m_tooltipWindow.setLookAndFeel(&m_lookAndFeel);

    m_sections = { Section { "DETECT", Theme::Colours::input, {} },
                   Section { "DRIVE",  Theme::Colours::harmonics, {} },
                   Section { "FILTER", Theme::Colours::filter, {} },
                   Section { "OUTPUT", Theme::Colours::output, {} } };

    addAndMakeVisible(m_waveformVisualizer);

    setupKnob(m_pitchThresholdSlider, m_pitchThresholdLabel, "Threshold", "pitchThreshold",
              "Input level needed before the sub is generated. Lower catches quieter notes.",
              Theme::Colours::input, Format::Decibels);
    setupKnob(m_fundamentalLimitSlider, m_fundamentalLimitLabel, "Max Pitch", "fundamentalLimit",
              "Highest input fundamental that is tracked. Notes above this are ignored.",
              Theme::Colours::input, Format::Hertz);

    setupKnob(m_distortionSlider, m_distortionLabel, "Drive", "distortion",
              "Crossfades the clean sub into its saturated harmonics while increasing drive.",
              Theme::Colours::harmonics, Format::Percent);
    setupKnob(m_postDriveLowpassSlider, m_postDriveLowpassLabel, "Hi Cut", "postDriveLowpass",
              "Low-pass on the whole sub signal.",
              Theme::Colours::harmonics, Format::Hertz);

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

    setupKnob(m_mixSlider, m_mixLabel, "Mix", "mix",
              "Blend between the dry input and the generated sub.",
              Theme::Colours::output, Format::Percent);
    setupKnob(m_outputGainSlider, m_outputGainLabel, "Output", "outputGain",
              "Final output level.",
              Theme::Colours::output, Format::Decibels);

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

    setupChoice(m_filterModeCombo, "filterMode",
                "Filter response for the harmonics: lowpass, bandpass (vocal, focused) or notch (hollow).",
                m_filterModeAttachment);
    m_filterModeCombo.setColour(juce::ComboBox::arrowColourId, Theme::Colours::filter);
    m_filterModeCombo.setColour(juce::ComboBox::focusedOutlineColourId, Theme::Colours::filter.withAlpha(0.6f));

    m_aboutButton.setButtonText("?");
    m_aboutButton.setTooltip("About SammyJs Subbertone");
    m_aboutButton.onClick = [this] { showAboutWindow(); };
    addAndMakeVisible(m_aboutButton);

    setSize(c_width, c_height);
    startTimerHz(20);
}

SubbertoneAudioProcessorEditor::~SubbertoneAudioProcessorEditor()
{
    stopTimer();
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
    }

    slider.valueFromTextFunction = [](const juce::String& text) { return parseNumber(text); };
    slider.updateText();

    if (auto* param = m_audioProcessor.m_parameters.getParameter(paramId))
        slider.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));

    addAndMakeVisible(slider);

    // The value box was created before the slider had a parent, i.e. with the
    // default LookAndFeel. Setting a colour makes the slider rebuild it via ours.
    slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    label.setText(name.toUpperCase(), juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, Theme::Colours::textDim);
    label.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label);
}

void SubbertoneAudioProcessorEditor::setupChoice(juce::ComboBox& combo, const juce::String& paramId, const juce::String& tooltip,
                                                 std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& attachment)
{
    // Items must exist before the attachment is made so it can restore the selection.
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(m_audioProcessor.m_parameters.getParameter(paramId)))
        combo.addItemList(choice->choices, 1);

    combo.setTooltip(tooltip);
    addAndMakeVisible(combo);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(m_audioProcessor.m_parameters, paramId, combo);
}

//==============================================================================
void SubbertoneAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    auto header = bounds.removeFromTop(c_headerHeight).reduced(c_margin, 0);
    m_aboutButton.setBounds(header.removeFromRight(26).withSizeKeepingCentre(26, 26));

    bounds.reduce(c_margin, 0);
    bounds.removeFromBottom(c_margin);

    auto controls = bounds.removeFromBottom(c_controlsHeight);
    bounds.removeFromBottom(c_sectionGap);
    m_waveformVisualizer.setBounds(bounds);

    // 2 + 2 + 4 + 2 knobs: size sections proportionally.
    const int unit = (controls.getWidth() - 3 * c_sectionGap) / 10;
    m_sections[0].bounds = controls.removeFromLeft(unit * 2);
    controls.removeFromLeft(c_sectionGap);
    m_sections[1].bounds = controls.removeFromLeft(unit * 2);
    controls.removeFromLeft(c_sectionGap);
    m_sections[3].bounds = controls.removeFromRight(unit * 2);
    controls.removeFromRight(c_sectionGap);
    m_sections[2].bounds = controls;

    auto bodyOf = [](const Section& s) { return s.bounds.withTrimmedTop(c_sectionHeaderHeight).reduced(4, 6); };

    layoutKnobRow(bodyOf(m_sections[0]), { { &m_pitchThresholdSlider, &m_pitchThresholdLabel },
                                           { &m_fundamentalLimitSlider, &m_fundamentalLimitLabel } });

    layoutKnobRow(bodyOf(m_sections[1]), { { &m_distortionSlider, &m_distortionLabel },
                                           { &m_postDriveLowpassSlider, &m_postDriveLowpassLabel } });

    layoutKnobRow(bodyOf(m_sections[2]), { { &m_toneSlider, &m_toneLabel },
                                           { &m_resonanceSlider, &m_resonanceLabel },
                                           { &m_envAmountSlider, &m_envAmountLabel },
                                           { &m_envReleaseSlider, &m_envReleaseLabel } });

    layoutKnobRow(bodyOf(m_sections[3]), { { &m_mixSlider, &m_mixLabel },
                                           { &m_outputGainSlider, &m_outputGainLabel } });

    // Selectors sit in their section headers, right-aligned after the title.
    const auto placeInHeader = [](juce::ComboBox& combo, const Section& section, int maxWidth)
    {
        auto row = section.bounds.withHeight(c_sectionHeaderHeight).reduced(8, 5);
        combo.setBounds(row.removeFromRight(std::min(maxWidth, row.getWidth() - 76)));
    };

    placeInHeader(m_distortionTypeCombo, m_sections[1], 128);
    placeInHeader(m_filterModeCombo, m_sections[2], 128);
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

void SubbertoneAudioProcessorEditor::paint(juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll(background);

    // Header
    auto header = getLocalBounds().removeFromTop(c_headerHeight).reduced(c_margin, 0).toFloat();

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
    g.drawText("SAMMYJS  /  SUBHARMONIC GENERATOR", header.withTrimmedTop(4.0f), juce::Justification::centredLeft, false);

    // Section panels
    for (const auto& section : m_sections)
    {
        const auto r = section.bounds.toFloat();
        g.setColour(panel);
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(outlineSoft);
        g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, 1.0f);

        auto headerRow = r.withHeight(static_cast<float>(c_sectionHeaderHeight)).reduced(12.0f, 0.0f);
        g.setColour(section.accent);
        g.fillEllipse(headerRow.removeFromLeft(6.0f).withSizeKeepingCentre(6.0f, 6.0f));
        headerRow.removeFromLeft(8.0f);
        g.setFont(m_fonts->heading(11.0f));
        g.setColour(text.withAlpha(0.85f));
        g.drawText(section.title, headerRow, juce::Justification::centredLeft, false);

        g.setColour(outlineSoft);
        g.drawHorizontalLine(section.bounds.getY() + c_sectionHeaderHeight, r.getX() + 12.0f, r.getRight() - 12.0f);
    }
}

void SubbertoneAudioProcessorEditor::timerCallback()
{
    const float rms = m_audioProcessor.getCurrentSignalLevel();
    const float signalDb = rms > 1.0e-8f ? 20.0f * std::log10(rms) : -160.0f;
    const float threshold = static_cast<float>(m_pitchThresholdSlider.getValue());

    const juce::String levelText = signalDb <= -100.0f ? juce::String("IN  -inf dB")
                                                       : "IN  " + juce::String(signalDb, 1) + " dB";

    m_waveformVisualizer.setSignalText(levelText, signalDb > threshold);
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

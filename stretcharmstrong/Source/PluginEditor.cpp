#include "PluginEditor.h"
#include "AboutWindow.h"

StretchArmstrongAudioProcessorEditor::StretchArmstrongAudioProcessorEditor(StretchArmstrongAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    // Set up waveform visualizer
    waveformVisualizer = std::make_unique<WaveformVisualizer>(audioProcessor);
    addAndMakeVisible(*waveformVisualizer);

    // Set up preset manager
    presetManager = std::make_unique<PresetManager>(audioProcessor);
    addAndMakeVisible(*presetManager);

    // Set up sliders with Stretch Armstrong toy theme colors
    setupSlider(thresholdSlider, thresholdLabel, " dB");
    setupSlider(attackSlider, attackLabel, " ms");
    setupSlider(sustainSlider, sustainLabel, " ms");
    setupSlider(releaseSlider, releaseLabel, " ms");
    setupSlider(stretchRatioSlider, stretchRatioLabel, "x");
    setupSlider(mixSlider, mixLabel, "%");
    setupSlider(outputGainSlider, outputGainLabel, " dB");

    // Stretch Armstrong flesh/skin tone colors for main controls
    juce::Colour fleshTone(0xffe8c4a0);
    juce::Colour stretchyPink(0xffff6b9d);
    juce::Colour muscleRed(0xffcc3333);
    juce::Colour heroBlue(0xff4488ff);

    thresholdSlider.setColour(juce::Slider::rotarySliderFillColourId, muscleRed);
    attackSlider.setColour(juce::Slider::rotarySliderFillColourId, fleshTone);
    sustainSlider.setColour(juce::Slider::rotarySliderFillColourId, fleshTone);
    releaseSlider.setColour(juce::Slider::rotarySliderFillColourId, fleshTone);
    stretchRatioSlider.setColour(juce::Slider::rotarySliderFillColourId, stretchyPink);
    mixSlider.setColour(juce::Slider::rotarySliderFillColourId, heroBlue);
    outputGainSlider.setColour(juce::Slider::rotarySliderFillColourId, heroBlue);

    // Set up envelope follower controls
    envFollowEnableButton.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffffaa00));
    envFollowEnableButton.setColour(juce::ToggleButton::tickColourId, juce::Colour(0xffffaa00));
    addAndMakeVisible(envFollowEnableButton);

    setupSlider(envFollowAmountSlider, envFollowAmountLabel, "%");
    setupSlider(envFollowAttackSlider, envFollowAttackLabel, " ms");
    setupSlider(envFollowReleaseSlider, envFollowReleaseLabel, " ms");

    juce::Colour envOrange(0xffffaa00);
    envFollowAmountSlider.setColour(juce::Slider::rotarySliderFillColourId, envOrange);
    envFollowAttackSlider.setColour(juce::Slider::rotarySliderFillColourId, envOrange);
    envFollowReleaseSlider.setColour(juce::Slider::rotarySliderFillColourId, envOrange);

    // Set up pitch follower controls
    pitchFollowEnableButton.setColour(juce::ToggleButton::textColourId, juce::Colour(0xff00ff88));
    pitchFollowEnableButton.setColour(juce::ToggleButton::tickColourId, juce::Colour(0xff00ff88));
    addAndMakeVisible(pitchFollowEnableButton);

    setupSlider(pitchFollowAmountSlider, pitchFollowAmountLabel, "%");
    setupSlider(pitchFollowRefSlider, pitchFollowRefLabel, " Hz");

    juce::Colour pitchGreen(0xff00ff88);
    pitchFollowAmountSlider.setColour(juce::Slider::rotarySliderFillColourId, pitchGreen);
    pitchFollowRefSlider.setColour(juce::Slider::rotarySliderFillColourId, pitchGreen);

    // Set up slew control
    setupSlider(modulationSlewSlider, modulationSlewLabel, " ms");
    modulationSlewSlider.setColour(juce::Slider::rotarySliderFillColourId, stretchyPink);

    // Stretch type combo box
    stretchTypeCombo.addItem("Varispeed", 1);
    stretchTypeCombo.addItem("Time Stretch", 2);
    stretchTypeCombo.setSelectedId(2);
    stretchTypeCombo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff2a2a2a));
    stretchTypeCombo.setColour(juce::ComboBox::textColourId, juce::Colours::white);
    addAndMakeVisible(stretchTypeCombo);
    stretchTypeLabel.setJustificationType(juce::Justification::centred);
    stretchTypeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffffffff));
    addAndMakeVisible(stretchTypeLabel);

    // Create attachments
    thresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "threshold", thresholdSlider);
    attackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "attack", attackSlider);
    sustainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "sustain", sustainSlider);
    releaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "release", releaseSlider);
    stretchRatioAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "stretchRatio", stretchRatioSlider);
    stretchTypeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.parameters, "stretchType", stretchTypeCombo);
    mixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "mix", mixSlider);
    outputGainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "outputGain", outputGainSlider);

    // Envelope follower attachments
    envFollowEnableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.parameters, "envFollowEnable", envFollowEnableButton);
    envFollowAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "envFollowAmount", envFollowAmountSlider);
    envFollowAttackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "envFollowAttack", envFollowAttackSlider);
    envFollowReleaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "envFollowRelease", envFollowReleaseSlider);

    // Pitch follower attachments
    pitchFollowEnableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.parameters, "pitchFollowEnable", pitchFollowEnableButton);
    pitchFollowAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "pitchFollowAmount", pitchFollowAmountSlider);
    pitchFollowRefAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "pitchFollowRef", pitchFollowRefSlider);

    // Slew attachment
    modulationSlewAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "modulationSlew", modulationSlewSlider);

    // About button - styled like a muscle
    aboutButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffcc3333));
    aboutButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    aboutButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    aboutButton.onClick = [this] { showAboutWindow(); };
    addAndMakeVisible(aboutButton);

    setSize(950, 580);
    setResizable(true, true);
    setResizeLimits(800, 480, 1600, 1000);
}

StretchArmstrongAudioProcessorEditor::~StretchArmstrongAudioProcessorEditor()
{
}

void StretchArmstrongAudioProcessorEditor::setupSlider(juce::Slider& slider, juce::Label& label, const juce::String& suffix)
{
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 18);
    slider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffe8c4a0));
    slider.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff3a3a3a));
    slider.setColour(juce::Slider::thumbColourId, juce::Colour(0xffff6b9d));
    slider.setTextValueSuffix(suffix);
    addAndMakeVisible(slider);

    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, juce::Colour(0xffffffff));
    addAndMakeVisible(label);
}

void StretchArmstrongAudioProcessorEditor::paint(juce::Graphics& g)
{
    // Background gradient - like stretched skin/rubber
    juce::ColourGradient gradient(
        juce::Colour(0xff1a1520), 0, 0,
        juce::Colour(0xff2a2030), 0, static_cast<float>(getHeight()),
        false);
    g.setGradientFill(gradient);
    g.fillAll();

    // Stretchy wavy lines in background
    g.setColour(juce::Colour(0x15ff6b9d));
    for (int i = 0; i < 8; ++i)
    {
        juce::Path wavyLine;
        float y = static_cast<float>(getHeight()) * (0.1f + i * 0.12f);
        wavyLine.startNewSubPath(0, y);
        for (float x = 0; x < getWidth(); x += 20)
        {
            float offset = std::sin(x * 0.02f + i) * 15.0f;
            wavyLine.lineTo(x, y + offset);
        }
        g.strokePath(wavyLine, juce::PathStrokeType(2.0f));
    }

    // Title with playful stretchy look
    g.setColour(juce::Colour(0xffff6b9d));
    g.setFont(juce::Font(juce::FontOptions("Arial Black", 26.0f, juce::Font::bold)));
    g.drawText("S T R E T C H", 15, 8, 200, 28, juce::Justification::left);

    g.setColour(juce::Colour(0xffe8c4a0));
    g.setFont(juce::Font(juce::FontOptions("Arial Black", 18.0f, juce::Font::bold)));
    g.drawText("ARMSTRONG", 15, 32, 150, 22, juce::Justification::left);

    // Calculate layout dimensions
    float w = static_cast<float>(getWidth());
    float h = static_cast<float>(getHeight());
    float topBarHeight = 55.0f;
    float controlsHeight = h * 0.42f;
    float visualizerHeight = h - topBarHeight - controlsHeight;
    float controlsY = topBarHeight + visualizerHeight;

    // Section divider - stretchy band look
    g.setColour(juce::Colour(0xffff6b9d));
    g.fillRoundedRectangle(10.0f, controlsY - 3.0f, w - 20.0f, 6.0f, 3.0f);

    // Section labels
    float sectionWidth = (w - 40.0f) / 4.0f;
    g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));

    g.setColour(juce::Colour(0xffcc3333));
    g.drawText("TRIGGER", 20.0f, controlsY + 5.0f, sectionWidth * 0.6f, 15.0f, juce::Justification::centred);

    g.setColour(juce::Colour(0xffe8c4a0));
    g.drawText("ENVELOPE", 20.0f + sectionWidth * 0.6f, controlsY + 5.0f, sectionWidth * 1.2f, 15.0f, juce::Justification::centred);

    g.setColour(juce::Colour(0xffff6b9d));
    g.drawText("STRETCH!", 20.0f + sectionWidth * 1.8f, controlsY + 5.0f, sectionWidth * 0.9f, 15.0f, juce::Justification::centred);

    g.setColour(juce::Colour(0xff4488ff));
    g.drawText("OUTPUT", 20.0f + sectionWidth * 2.7f, controlsY + 5.0f, sectionWidth * 0.8f, 15.0f, juce::Justification::centred);

    // Modulation section divider
    float modY = controlsY + controlsHeight * 0.52f;
    g.setColour(juce::Colour(0xff3a3a3a));
    g.drawLine(15.0f, modY, w - 15.0f, modY, 1.0f);

    // Modulation section labels
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));

    g.setColour(juce::Colour(0xffffaa00));
    g.drawText("ENV FOLLOWER", 20.0f, modY + 3.0f, sectionWidth * 1.2f, 14.0f, juce::Justification::centred);

    g.setColour(juce::Colour(0xff00ff88));
    g.drawText("PITCH FOLLOWER", 20.0f + sectionWidth * 1.4f, modY + 3.0f, sectionWidth * 1.0f, 14.0f, juce::Justification::centred);

    g.setColour(juce::Colour(0xffff6b9d));
    g.drawText("SMOOTH", 20.0f + sectionWidth * 2.6f, modY + 3.0f, sectionWidth * 0.6f, 14.0f, juce::Justification::centred);
}

void StretchArmstrongAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    float w = static_cast<float>(bounds.getWidth());
    float h = static_cast<float>(bounds.getHeight());

    // Top bar
    float topBarHeight = 55.0f;
    auto topBar = bounds.removeFromTop(static_cast<int>(topBarHeight));
    aboutButton.setBounds(topBar.removeFromRight(35).reduced(5));
    presetManager->setBounds(topBar.removeFromRight(static_cast<int>(w * 0.35f)).reduced(5));

    // Calculate control area proportions
    float controlsHeightRatio = 0.42f;
    float controlsHeight = h * controlsHeightRatio;
    float visualizerHeight = h - topBarHeight - controlsHeight;

    // Waveform visualizer
    auto visualizerArea = bounds.removeFromTop(static_cast<int>(visualizerHeight));
    waveformVisualizer->setBounds(visualizerArea.reduced(10));

    // Controls area
    float controlsY = topBarHeight + visualizerHeight;
    float knobSize = std::min(w * 0.07f, controlsHeight * 0.35f);
    float labelHeight = knobSize * 0.22f;
    float spacing = w * 0.008f;

    // Row 1: Main controls
    float row1Y = controlsY + 22.0f;
    float x = 20.0f;

    // Threshold
    thresholdLabel.setBounds(static_cast<int>(x), static_cast<int>(row1Y), static_cast<int>(knobSize), static_cast<int>(labelHeight));
    thresholdSlider.setBounds(static_cast<int>(x), static_cast<int>(row1Y + labelHeight), static_cast<int>(knobSize), static_cast<int>(knobSize));
    x += knobSize + spacing * 2;

    // ASR Envelope
    attackLabel.setBounds(static_cast<int>(x), static_cast<int>(row1Y), static_cast<int>(knobSize), static_cast<int>(labelHeight));
    attackSlider.setBounds(static_cast<int>(x), static_cast<int>(row1Y + labelHeight), static_cast<int>(knobSize), static_cast<int>(knobSize));
    x += knobSize + spacing;

    sustainLabel.setBounds(static_cast<int>(x), static_cast<int>(row1Y), static_cast<int>(knobSize), static_cast<int>(labelHeight));
    sustainSlider.setBounds(static_cast<int>(x), static_cast<int>(row1Y + labelHeight), static_cast<int>(knobSize), static_cast<int>(knobSize));
    x += knobSize + spacing;

    releaseLabel.setBounds(static_cast<int>(x), static_cast<int>(row1Y), static_cast<int>(knobSize), static_cast<int>(labelHeight));
    releaseSlider.setBounds(static_cast<int>(x), static_cast<int>(row1Y + labelHeight), static_cast<int>(knobSize), static_cast<int>(knobSize));
    x += knobSize + spacing * 2;

    // Stretch section
    stretchRatioLabel.setBounds(static_cast<int>(x), static_cast<int>(row1Y), static_cast<int>(knobSize), static_cast<int>(labelHeight));
    stretchRatioSlider.setBounds(static_cast<int>(x), static_cast<int>(row1Y + labelHeight), static_cast<int>(knobSize), static_cast<int>(knobSize));
    x += knobSize + spacing;

    float comboWidth = knobSize * 1.3f;
    stretchTypeLabel.setBounds(static_cast<int>(x), static_cast<int>(row1Y), static_cast<int>(comboWidth), static_cast<int>(labelHeight));
    stretchTypeCombo.setBounds(static_cast<int>(x), static_cast<int>(row1Y + labelHeight + knobSize * 0.3f), static_cast<int>(comboWidth), static_cast<int>(knobSize * 0.35f));
    x += comboWidth + spacing * 2;

    // Output section
    mixLabel.setBounds(static_cast<int>(x), static_cast<int>(row1Y), static_cast<int>(knobSize), static_cast<int>(labelHeight));
    mixSlider.setBounds(static_cast<int>(x), static_cast<int>(row1Y + labelHeight), static_cast<int>(knobSize), static_cast<int>(knobSize));
    x += knobSize + spacing;

    outputGainLabel.setBounds(static_cast<int>(x), static_cast<int>(row1Y), static_cast<int>(knobSize), static_cast<int>(labelHeight));
    outputGainSlider.setBounds(static_cast<int>(x), static_cast<int>(row1Y + labelHeight), static_cast<int>(knobSize), static_cast<int>(knobSize));

    // Row 2: Modulation controls
    float modY = controlsY + controlsHeight * 0.52f;
    float row2Y = modY + 18.0f;
    float smallKnobSize = knobSize * 0.85f;
    x = 20.0f;

    // Envelope follower
    envFollowEnableButton.setBounds(static_cast<int>(x), static_cast<int>(row2Y + smallKnobSize * 0.3f), static_cast<int>(smallKnobSize * 0.8f), static_cast<int>(smallKnobSize * 0.35f));
    x += smallKnobSize * 0.85f;

    envFollowAmountLabel.setBounds(static_cast<int>(x), static_cast<int>(row2Y), static_cast<int>(smallKnobSize), static_cast<int>(labelHeight));
    envFollowAmountSlider.setBounds(static_cast<int>(x), static_cast<int>(row2Y + labelHeight), static_cast<int>(smallKnobSize), static_cast<int>(smallKnobSize));
    x += smallKnobSize + spacing;

    envFollowAttackLabel.setBounds(static_cast<int>(x), static_cast<int>(row2Y), static_cast<int>(smallKnobSize), static_cast<int>(labelHeight));
    envFollowAttackSlider.setBounds(static_cast<int>(x), static_cast<int>(row2Y + labelHeight), static_cast<int>(smallKnobSize), static_cast<int>(smallKnobSize));
    x += smallKnobSize + spacing;

    envFollowReleaseLabel.setBounds(static_cast<int>(x), static_cast<int>(row2Y), static_cast<int>(smallKnobSize), static_cast<int>(labelHeight));
    envFollowReleaseSlider.setBounds(static_cast<int>(x), static_cast<int>(row2Y + labelHeight), static_cast<int>(smallKnobSize), static_cast<int>(smallKnobSize));
    x += smallKnobSize + spacing * 3;

    // Pitch follower
    pitchFollowEnableButton.setBounds(static_cast<int>(x), static_cast<int>(row2Y + smallKnobSize * 0.3f), static_cast<int>(smallKnobSize * 0.9f), static_cast<int>(smallKnobSize * 0.35f));
    x += smallKnobSize * 0.95f;

    pitchFollowAmountLabel.setBounds(static_cast<int>(x), static_cast<int>(row2Y), static_cast<int>(smallKnobSize), static_cast<int>(labelHeight));
    pitchFollowAmountSlider.setBounds(static_cast<int>(x), static_cast<int>(row2Y + labelHeight), static_cast<int>(smallKnobSize), static_cast<int>(smallKnobSize));
    x += smallKnobSize + spacing;

    pitchFollowRefLabel.setBounds(static_cast<int>(x), static_cast<int>(row2Y), static_cast<int>(smallKnobSize), static_cast<int>(labelHeight));
    pitchFollowRefSlider.setBounds(static_cast<int>(x), static_cast<int>(row2Y + labelHeight), static_cast<int>(smallKnobSize), static_cast<int>(smallKnobSize));
    x += smallKnobSize + spacing * 3;

    // Slew
    modulationSlewLabel.setBounds(static_cast<int>(x), static_cast<int>(row2Y), static_cast<int>(smallKnobSize), static_cast<int>(labelHeight));
    modulationSlewSlider.setBounds(static_cast<int>(x), static_cast<int>(row2Y + labelHeight), static_cast<int>(smallKnobSize), static_cast<int>(smallKnobSize));
}

void StretchArmstrongAudioProcessorEditor::showAboutWindow()
{
    new AboutWindow();
}

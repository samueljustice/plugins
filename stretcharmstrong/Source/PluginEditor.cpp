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

    // Set up sliders
    setupSlider(thresholdSlider, thresholdLabel, " dB");
    setupSlider(attackSlider, attackLabel, " ms");
    setupSlider(sustainSlider, sustainLabel, " ms");
    setupSlider(releaseSlider, releaseLabel, " ms");
    setupSlider(stretchRatioSlider, stretchRatioLabel, "x");
    setupSlider(mixSlider, mixLabel, "%");
    setupSlider(outputGainSlider, outputGainLabel, " dB");

    // Set up envelope follower controls
    envFollowEnableButton.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffff9900));
    envFollowEnableButton.setColour(juce::ToggleButton::tickColourId, juce::Colour(0xffff9900));
    addAndMakeVisible(envFollowEnableButton);

    setupSlider(envFollowAmountSlider, envFollowAmountLabel, "%");
    setupSlider(envFollowAttackSlider, envFollowAttackLabel, " ms");
    setupSlider(envFollowReleaseSlider, envFollowReleaseLabel, " ms");

    // Different color for envelope follower knobs
    envFollowAmountSlider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffff9900));
    envFollowAttackSlider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffff9900));
    envFollowReleaseSlider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffff9900));

    // Set up pitch follower controls
    pitchFollowEnableButton.setColour(juce::ToggleButton::textColourId, juce::Colour(0xff00ff88));
    pitchFollowEnableButton.setColour(juce::ToggleButton::tickColourId, juce::Colour(0xff00ff88));
    addAndMakeVisible(pitchFollowEnableButton);

    setupSlider(pitchFollowAmountSlider, pitchFollowAmountLabel, "%");
    setupSlider(pitchFollowRefSlider, pitchFollowRefLabel, " Hz");

    // Different color for pitch follower knobs
    pitchFollowAmountSlider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff00ff88));
    pitchFollowRefSlider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff00ff88));

    // Set up slew control
    setupSlider(modulationSlewSlider, modulationSlewLabel, " ms");
    modulationSlewSlider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffff00ff));

    // Stretch type combo box
    stretchTypeCombo.addItem("Varispeed", 1);
    stretchTypeCombo.addItem("Time Stretch", 2);
    stretchTypeCombo.setSelectedId(2);
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

    // About button
    aboutButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333333));
    aboutButton.onClick = [this] { showAboutWindow(); };
    addAndMakeVisible(aboutButton);

    setSize(1000, 600);
    setResizable(true, true);
    setResizeLimits(900, 500, 1600, 900);
}

StretchArmstrongAudioProcessorEditor::~StretchArmstrongAudioProcessorEditor()
{
}

void StretchArmstrongAudioProcessorEditor::setupSlider(juce::Slider& slider, juce::Label& label, const juce::String& suffix)
{
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 20);
    slider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff00ffff));
    slider.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff333333));
    slider.setColour(juce::Slider::thumbColourId, juce::Colour(0xffff00ff));
    slider.setTextValueSuffix(suffix);
    addAndMakeVisible(slider);

    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, juce::Colour(0xffffffff));
    addAndMakeVisible(label);
}

void StretchArmstrongAudioProcessorEditor::paint(juce::Graphics& g)
{
    // Background gradient
    juce::ColourGradient gradient(juce::Colour(0xff0a0a0a), 0, 0,
                                   juce::Colour(0xff1a1a2a), 0, static_cast<float>(getHeight()),
                                   false);
    g.setGradientFill(gradient);
    g.fillAll();

    // Title
    g.setColour(juce::Colour(0xff00ffff));
    g.setFont(juce::Font(juce::FontOptions("Courier New", 20.0f, juce::Font::bold)));
    g.drawText("STRETCH ARMSTRONG", 10, 5, 250, 30, juce::Justification::left);

    // Section dividers
    g.setColour(juce::Colour(0xff333333));
    int controlsY = getHeight() - 280;
    g.drawLine(10, static_cast<float>(controlsY - 5), static_cast<float>(getWidth() - 10), static_cast<float>(controlsY - 5), 1);

    // Upper row section labels
    g.setColour(juce::Colour(0xff888888));
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText("THRESHOLD", 20, controlsY, 80, 15, juce::Justification::centred);
    g.drawText("ASR ENVELOPE", 120, controlsY, 200, 15, juce::Justification::centred);
    g.drawText("STRETCH", 340, controlsY, 120, 15, juce::Justification::centred);
    g.drawText("OUTPUT", 480, controlsY, 150, 15, juce::Justification::centred);

    // Modulation section divider
    int modY = getHeight() - 140;
    g.setColour(juce::Colour(0xff333333));
    g.drawLine(10, static_cast<float>(modY - 5), static_cast<float>(getWidth() - 10), static_cast<float>(modY - 5), 1);

    // Modulation section labels
    g.setColour(juce::Colour(0xffff9900));
    g.drawText("ENV FOLLOWER", 20, modY, 250, 15, juce::Justification::centred);
    g.setColour(juce::Colour(0xff00ff88));
    g.drawText("PITCH FOLLOWER", 300, modY, 200, 15, juce::Justification::centred);
    g.setColour(juce::Colour(0xffff00ff));
    g.drawText("SLEW", 520, modY, 80, 15, juce::Justification::centred);
}

void StretchArmstrongAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    // Top bar with preset manager and about button
    auto topBar = bounds.removeFromTop(35);
    aboutButton.setBounds(topBar.removeFromRight(30).reduced(3));
    presetManager->setBounds(topBar.removeFromRight(350).reduced(3));

    // Waveform visualizer takes most of the space
    int controlsHeight = 280;
    auto visualizerArea = bounds.removeFromTop(bounds.getHeight() - controlsHeight);
    waveformVisualizer->setBounds(visualizerArea.reduced(10));

    // Controls area
    auto controlsArea = bounds.reduced(10);
    int knobWidth = 70;
    int knobHeight = 80;
    int labelHeight = 18;
    int spacing = 8;

    int y = controlsArea.getY() + 20;

    // Threshold
    int x = 20;
    thresholdLabel.setBounds(x, y, knobWidth, labelHeight);
    thresholdSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);

    // ASR Envelope section
    x += knobWidth + spacing + 15;

    attackLabel.setBounds(x, y, knobWidth, labelHeight);
    attackSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);
    x += knobWidth + spacing;

    sustainLabel.setBounds(x, y, knobWidth, labelHeight);
    sustainSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);
    x += knobWidth + spacing;

    releaseLabel.setBounds(x, y, knobWidth, labelHeight);
    releaseSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);

    // Stretch section
    x += knobWidth + spacing + 15;

    stretchRatioLabel.setBounds(x, y, knobWidth, labelHeight);
    stretchRatioSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);
    x += knobWidth + spacing;

    stretchTypeLabel.setBounds(x, y, knobWidth + 10, labelHeight);
    stretchTypeCombo.setBounds(x, y + labelHeight + 25, knobWidth + 30, 25);

    // Output section
    x += knobWidth + spacing + 40;

    mixLabel.setBounds(x, y, knobWidth, labelHeight);
    mixSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);
    x += knobWidth + spacing;

    outputGainLabel.setBounds(x, y, knobWidth, labelHeight);
    outputGainSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);

    // ===== MODULATION ROW =====
    int modY = controlsArea.getY() + 140;

    // Envelope follower section
    x = 20;
    envFollowEnableButton.setBounds(x, modY + 10, 60, 25);
    x += 65;

    envFollowAmountLabel.setBounds(x, modY, knobWidth, labelHeight);
    envFollowAmountSlider.setBounds(x, modY + labelHeight, knobWidth, knobHeight);
    x += knobWidth + spacing;

    envFollowAttackLabel.setBounds(x, modY, knobWidth, labelHeight);
    envFollowAttackSlider.setBounds(x, modY + labelHeight, knobWidth, knobHeight);
    x += knobWidth + spacing;

    envFollowReleaseLabel.setBounds(x, modY, knobWidth, labelHeight);
    envFollowReleaseSlider.setBounds(x, modY + labelHeight, knobWidth, knobHeight);

    // Pitch follower section
    x += knobWidth + spacing + 20;

    pitchFollowEnableButton.setBounds(x, modY + 10, 70, 25);
    x += 75;

    pitchFollowAmountLabel.setBounds(x, modY, knobWidth, labelHeight);
    pitchFollowAmountSlider.setBounds(x, modY + labelHeight, knobWidth, knobHeight);
    x += knobWidth + spacing;

    pitchFollowRefLabel.setBounds(x, modY, knobWidth, labelHeight);
    pitchFollowRefSlider.setBounds(x, modY + labelHeight, knobWidth, knobHeight);

    // Slew section
    x += knobWidth + spacing + 20;

    modulationSlewLabel.setBounds(x, modY, knobWidth, labelHeight);
    modulationSlewSlider.setBounds(x, modY + labelHeight, knobWidth, knobHeight);
}

void StretchArmstrongAudioProcessorEditor::showAboutWindow()
{
    new AboutWindow();
}

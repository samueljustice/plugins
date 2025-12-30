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

    // About button
    aboutButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333333));
    aboutButton.onClick = [this] { showAboutWindow(); };
    addAndMakeVisible(aboutButton);

    setSize(700, 500);
    setResizable(true, true);
    setResizeLimits(600, 400, 1200, 800);
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
    int controlsY = getHeight() - 180;
    g.drawLine(10, static_cast<float>(controlsY - 5), static_cast<float>(getWidth() - 10), static_cast<float>(controlsY - 5), 1);

    // Section labels
    g.setColour(juce::Colour(0xff888888));
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText("THRESHOLD", 20, controlsY, 80, 15, juce::Justification::centred);
    g.drawText("ASR ENVELOPE", 120, controlsY, 200, 15, juce::Justification::centred);
    g.drawText("STRETCH", 360, controlsY, 120, 15, juce::Justification::centred);
    g.drawText("OUTPUT", 520, controlsY, 150, 15, juce::Justification::centred);
}

void StretchArmstrongAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    // Top bar with preset manager and about button
    auto topBar = bounds.removeFromTop(35);
    aboutButton.setBounds(topBar.removeFromRight(30).reduced(3));
    presetManager->setBounds(topBar.removeFromRight(350).reduced(3));

    // Waveform visualizer takes most of the space
    int controlsHeight = 180;
    auto visualizerArea = bounds.removeFromTop(bounds.getHeight() - controlsHeight);
    waveformVisualizer->setBounds(visualizerArea.reduced(10));

    // Controls area
    auto controlsArea = bounds.reduced(10);
    int knobWidth = 80;
    int knobHeight = 90;
    int labelHeight = 20;
    int spacing = 10;

    int y = controlsArea.getY() + 20;

    // Threshold
    int x = 20;
    thresholdLabel.setBounds(x, y, knobWidth, labelHeight);
    thresholdSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);

    // ASR Envelope section
    x += knobWidth + spacing + 20;

    attackLabel.setBounds(x, y, knobWidth, labelHeight);
    attackSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);
    x += knobWidth + spacing;

    sustainLabel.setBounds(x, y, knobWidth, labelHeight);
    sustainSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);
    x += knobWidth + spacing;

    releaseLabel.setBounds(x, y, knobWidth, labelHeight);
    releaseSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);

    // Stretch section
    x += knobWidth + spacing + 20;

    stretchRatioLabel.setBounds(x, y, knobWidth, labelHeight);
    stretchRatioSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);
    x += knobWidth + spacing;

    stretchTypeLabel.setBounds(x, y, knobWidth, labelHeight);
    stretchTypeCombo.setBounds(x, y + labelHeight + 30, knobWidth + 20, 25);

    // Output section
    x += knobWidth + spacing + 30;

    mixLabel.setBounds(x, y, knobWidth, labelHeight);
    mixSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);
    x += knobWidth + spacing;

    outputGainLabel.setBounds(x, y, knobWidth, labelHeight);
    outputGainSlider.setBounds(x, y + labelHeight, knobWidth, knobHeight);
}

void StretchArmstrongAudioProcessorEditor::showAboutWindow()
{
    new AboutWindow();
}

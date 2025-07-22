#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "AboutWindow.h"

SubbertoneAudioProcessorEditor::SubbertoneLookAndFeel::SubbertoneLookAndFeel()
{
    // PS1 Wipeout inspired colors
    setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff00ffff));
    setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff1a3a3a));
    setColour(juce::Slider::thumbColourId, juce::Colour(0xffff00ff));
    setColour(juce::Label::textColourId, juce::Colour(0xffffffff));
}

void SubbertoneAudioProcessorEditor::SubbertoneLookAndFeel::drawRotarySlider(
    juce::Graphics& g, int x, int y, int width, int height,
    float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
    juce::Slider& slider)
{
    auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(10);
    auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) / 2.0f;
    auto toAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    auto lineW = juce::jmin(8.0f, radius * 0.5f);
    auto arcRadius = radius - lineW * 0.5f;

    // Background arc
    juce::Path backgroundArc;
    backgroundArc.addCentredArc(bounds.getCentreX(), bounds.getCentreY(),
                               arcRadius, arcRadius, 0.0f,
                               rotaryStartAngle, rotaryEndAngle, true);

    g.setColour(juce::Colour(0xff1a3a3a));
    g.strokePath(backgroundArc, juce::PathStrokeType(lineW, 
        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (slider.isEnabled())
    {
        // Value arc
        juce::Path valueArc;
        valueArc.addCentredArc(bounds.getCentreX(), bounds.getCentreY(),
                              arcRadius, arcRadius, 0.0f,
                              rotaryStartAngle, toAngle, true);

        g.setColour(juce::Colour(0xff00ffff));
        g.strokePath(valueArc, juce::PathStrokeType(lineW, 
            juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Center indicator
    auto thumbWidth = lineW * 2.0f;
    juce::Point<float> thumbPoint(bounds.getCentreX() + arcRadius * std::cos(toAngle - juce::MathConstants<float>::halfPi),
                                  bounds.getCentreY() + arcRadius * std::sin(toAngle - juce::MathConstants<float>::halfPi));

    g.setColour(juce::Colour(0xffff00ff));
    g.fillEllipse(juce::Rectangle<float>(thumbWidth, thumbWidth).withCentre(thumbPoint));
}

SubbertoneAudioProcessorEditor::SubbertoneAudioProcessorEditor(SubbertoneAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setLookAndFeel(&lookAndFeel);
    
    // Enable tooltips
    tooltipWindow = std::make_unique<juce::TooltipWindow>(this, 700);
    
    // Create visualizer
    waveformVisualizer = std::make_unique<WaveformVisualizer>(audioProcessor);
    addAndMakeVisible(waveformVisualizer.get());
    
    // Setup sliders
    auto setupSlider = [this](juce::Slider& slider, juce::Label& label, const juce::String& text)
    {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 80, 20);
        addAndMakeVisible(slider);
        
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.attachToComponent(&slider, false);
        addAndMakeVisible(label);
    };
    
    setupSlider(mixSlider, mixLabel, "Mix");
    mixSlider.setTooltip("Blend between dry input signal and processed subharmonic signal (0-100%)");
    mixSlider.setTextValueSuffix("%");
    
    setupSlider(distortionSlider, distortionLabel, "Distortion");
    distortionSlider.setTooltip("Amount of harmonic distortion applied to the subharmonic signal (0-100%)");
    distortionSlider.setTextValueSuffix("%");
    
    
    setupSlider(toneSlider, toneLabel, "Tone Filter");
    toneSlider.setTooltip("Low-pass filter frequency for shaping the harmonic content before mixing (20Hz-20kHz)");
    toneSlider.setTextValueSuffix(" Hz");
    toneSlider.setNumDecimalPlacesToDisplay(0);
    
    setupSlider(postDriveLowpassSlider, postDriveLowpassLabel, "Lowpass");
    postDriveLowpassSlider.setTooltip("Post-drive low-pass filter - removes upper harmonics created by distortion (20Hz-20kHz)");
    postDriveLowpassSlider.setTextValueSuffix(" Hz");
    postDriveLowpassSlider.setNumDecimalPlacesToDisplay(0);
    
    setupSlider(outputGainSlider, outputGainLabel, "Output");
    outputGainSlider.setTooltip("Final output gain control (-24dB to +24dB)");
    outputGainSlider.setTextValueSuffix(" dB");
    
    setupSlider(pitchThresholdSlider, pitchThresholdLabel, "Pitch Threshold");
    pitchThresholdSlider.setTooltip("Threshold for pitch detection (-60dB to -20dB). Lower values detect quieter signals but may be less accurate");
    pitchThresholdSlider.setTextValueSuffix(" dB");
    
    setupSlider(fundamentalLimitSlider, fundamentalLimitLabel, "Max Freq");
    fundamentalLimitSlider.setTooltip("Maximum fundamental frequency to process (100Hz to 800Hz). Frequencies above this limit will be ignored");
    fundamentalLimitSlider.setTextValueSuffix(" Hz");
    
    // Setup signal level label
    signalLevelLabel.setText("Signal: -∞ dB", juce::dontSendNotification);
    signalLevelLabel.setJustificationType(juce::Justification::centred);
    signalLevelLabel.setColour(juce::Label::textColourId, juce::Colours::red);
    addAndMakeVisible(signalLevelLabel);
    
    // Setup distortion type combo
    distortionTypeCombo.addItem("Tape Saturation", 1);
    distortionTypeCombo.addItem("Valve Warmth", 2);
    distortionTypeCombo.addItem("Console Drive", 3);
    distortionTypeCombo.addItem("Transformer", 4);
    distortionTypeCombo.setTooltip("Harmonic character:\n"
                                  "• Tape Saturation: Smooth, musical compression\n"
                                  "• Valve Warmth: Tube-style even harmonics\n"
                                  "• Console Drive: Preamp-style soft clipping\n"
                                  "• Transformer: Gentle S-curve saturation");
    addAndMakeVisible(distortionTypeCombo);
    
    distortionTypeLabel.setText("Distortion Type", juce::dontSendNotification);
    distortionTypeLabel.setJustificationType(juce::Justification::centred);
    distortionTypeLabel.attachToComponent(&distortionTypeCombo, false);
    addAndMakeVisible(distortionTypeLabel);
    
    // Create parameter attachments
    mixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "mix", mixSlider);
    distortionAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "distortion", distortionSlider);
    toneAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "distortionTone", toneSlider);
    postDriveLowpassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "postDriveLowpass", postDriveLowpassSlider);
    outputGainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "outputGain", outputGainSlider);
    pitchThresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "pitchThreshold", pitchThresholdSlider);
    fundamentalLimitAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "fundamentalLimit", fundamentalLimitSlider);
    distortionTypeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.parameters, "distortionType", distortionTypeCombo);
    
    // Setup about button
    aboutButton.setButtonText("?");
    aboutButton.setTooltip("About SammyJs Subbertone - Version info and help");
    aboutButton.onClick = [this] { showAboutWindow(); };
    addAndMakeVisible(aboutButton);
    
    // Setup visualizer toggles
    showInputToggle.setButtonText("Show Input");
    showInputToggle.setToggleState(true, juce::dontSendNotification);
    showInputToggle.onClick = [this] { 
        waveformVisualizer->showInput = showInputToggle.getToggleState(); 
        waveformVisualizer->repaint();
    };
    addAndMakeVisible(showInputToggle);
    
    showOutputToggle.setButtonText("Show Output");
    showOutputToggle.setToggleState(true, juce::dontSendNotification);
    showOutputToggle.onClick = [this] { 
        waveformVisualizer->showOutput = showOutputToggle.getToggleState();
        waveformVisualizer->repaint();
    };
    addAndMakeVisible(showOutputToggle);
    
    setSize(900, 550);
    
    // Start timer for signal level updates
    startTimer(50); // Update every 50ms
}

SubbertoneAudioProcessorEditor::~SubbertoneAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

void SubbertoneAudioProcessorEditor::paint(juce::Graphics& g)
{
    // Dark PS1-style background
    g.fillAll(juce::Colour(0xff0a0a0a));
    
    // Title
    g.setColour(juce::Colour(0xff00ffff));
    g.setFont(juce::Font(juce::FontOptions("Courier New", 24.0f, juce::Font::bold)));
    g.drawText("SAMMYJS SUBBERTONE", getLocalBounds().removeFromTop(40),
               juce::Justification::centred);
    
    // Subtitle describing the unique inverse mix feature
    g.setColour(juce::Colour(0xff00ffff).withAlpha(0.7f));
    g.setFont(juce::Font("Courier New", 12.0f, juce::Font::plain));
    g.drawText("Subtracts clean signal from distorted to isolate pure harmonic artifacts", 
               getLocalBounds().removeFromTop(55).removeFromBottom(15),
               juce::Justification::centred);
    
    // Control panel background
    auto controlBounds = getLocalBounds().removeFromBottom(200);
    g.setColour(juce::Colour(0xff0f0f0f));
    g.fillRect(controlBounds);
    
    g.setColour(juce::Colour(0xff1a3a3a));
    g.drawRect(controlBounds, 2);
}

void SubbertoneAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    
    // About button in top right
    aboutButton.setBounds(bounds.getWidth() - 40, 10, 30, 30);
    
    // Visualizer toggles - position them below the title/subtitle area
    showInputToggle.setBounds(10, 60, 100, 25);
    showOutputToggle.setBounds(120, 60, 100, 25);
    
    // Visualizer takes up most of the space
    bounds.removeFromTop(90); // Title, subtitle and checkbox space
    auto visualizerBounds = bounds.removeFromTop(bounds.getHeight() - 200);
    waveformVisualizer->setBounds(visualizerBounds);
    
    // Controls at bottom - two rows
    auto controlBounds = bounds.reduced(20, 10);
    
    // First row of controls
    auto firstRow = controlBounds.removeFromTop(controlBounds.getHeight() / 2);
    auto sliderWidth = firstRow.getWidth() / 3;
    
    mixSlider.setBounds(firstRow.removeFromLeft(sliderWidth).reduced(10));
    distortionSlider.setBounds(firstRow.removeFromLeft(sliderWidth).reduced(10));
    outputGainSlider.setBounds(firstRow.removeFromLeft(sliderWidth).reduced(10));
    
    // Second row of controls
    auto secondRow = controlBounds;
    auto secondRowWidth = secondRow.getWidth() / 5;  // Changed from 4 to 5 controls
    
    distortionTypeCombo.setBounds(secondRow.removeFromLeft(secondRowWidth).reduced(20, 30));
    toneSlider.setBounds(secondRow.removeFromLeft(secondRowWidth).reduced(10));
    postDriveLowpassSlider.setBounds(secondRow.removeFromLeft(secondRowWidth).reduced(10));
    pitchThresholdSlider.setBounds(secondRow.removeFromLeft(secondRowWidth).reduced(10));
    fundamentalLimitSlider.setBounds(secondRow.removeFromLeft(secondRowWidth).reduced(10));
    
    // Signal level label positioned below pitch threshold slider
    auto pitchBounds = pitchThresholdSlider.getBounds();
    signalLevelLabel.setBounds(pitchBounds.getX(), pitchBounds.getBottom() - 25, pitchBounds.getWidth(), 20);
}

void SubbertoneAudioProcessorEditor::timerCallback()
{
    // Update signal level display
    float signalDb = audioProcessor.getCurrentSignalLevel();
    float threshold = pitchThresholdSlider.getValue();
    
    juce::String levelText;
    if (signalDb <= -99.0f)
        levelText = "Signal: -\u221e dB";
    else
        levelText = juce::String::formatted("Signal: %.1f dB", signalDb);
    
    signalLevelLabel.setText(levelText, juce::dontSendNotification);
    
    // Change color based on threshold
    if (signalDb > threshold)
        signalLevelLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
    else
        signalLevelLabel.setColour(juce::Label::textColourId, juce::Colours::red);
}

void SubbertoneAudioProcessorEditor::showAboutWindow()
{
    auto aboutWindow = new AboutWindow();
    aboutWindow->setVisible(true);
}
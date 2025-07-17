#include "PluginProcessor.h"
#include "PluginEditor.h"

// PitchMeter implementation
PitchMeter::PitchMeter()
{
    spectrogramVisualizer = std::make_unique<SpectrogramVisualizer>();
    addAndMakeVisible(spectrogramVisualizer.get());
    spectrogramVisualizer->setVisible(false); // Start hidden, will be enabled for DIO
    startTimerHz(30);
}

void PitchMeter::paint(juce::Graphics& g)
{
    // Validate component size
    if (getWidth() <= 0 || getHeight() <= 0)
        return;
        
    auto bounds = getLocalBounds().reduced(5);
    
    // Ensure bounds are valid
    if (bounds.isEmpty())
        return;
    
    // Only draw dark background if spectrogram is not visible
    if (!spectrogramVisualizer || !spectrogramVisualizer->isVisible())
    {
        g.setColour(juce::Colours::darkgrey.darker());
        g.fillRoundedRectangle(bounds.toFloat(), 5.0f);
    }
    
    // Draw frequency display with background for visibility
    if (displayFrequency > 0)
    {
        juce::String freqText = juce::String(displayFrequency, 1) + " Hz";
        juce::String noteText = frequencyToNote(displayFrequency);
        
        // Draw semi-transparent background for text
        auto textArea = bounds.removeFromTop(60).reduced(20, 5);
        g.setColour(juce::Colours::black.withAlpha(0.7f));
        g.fillRoundedRectangle(textArea.toFloat(), 5.0f);
        
        // Draw frequency text
        g.setColour(juce::Colours::white);
        g.setFont(24.0f);
        auto freqBounds = textArea.removeFromTop(30);
        g.drawText(freqText, freqBounds, juce::Justification::centred);
        
        // Draw note text
        g.setFont(18.0f);
        g.setColour(juce::Colours::lightblue);
        g.drawText(noteText, textArea, juce::Justification::centred);
    }
    else
    {
        g.setColour(juce::Colours::grey);
        g.drawText("No Input", bounds.removeFromTop(60), juce::Justification::centred);
    }
    
    // Draw target indicator
    if (targetFrequency > 0 && displayFrequency > 0 && getHeight() > 20)
    {
        float cents = frequencyToCents(displayFrequency, targetFrequency);
        float meterWidth = static_cast<float>(getWidth() - 10);
        float centerX = static_cast<float>(getWidth()) / 2.0f;
        
        // Draw center line
        g.setColour(juce::Colours::green.withAlpha(0.5f));
        g.drawLine(centerX, static_cast<float>(getHeight() - 20), 
                   centerX, static_cast<float>(getHeight() - 5), 2.0f);
        
        // Draw pitch deviation meter
        float deviationX = centerX + (cents / 50.0f) * (meterWidth / 2.0f);
        deviationX = std::clamp(deviationX, 5.0f, static_cast<float>(getWidth() - 5));
        
        g.setColour(std::abs(cents) < 10 ? juce::Colours::green : juce::Colours::orange);
        
        // Ensure we're drawing within bounds
        float ellipseY = static_cast<float>(getHeight() - 17);
        if (ellipseY >= 0 && deviationX >= 5 && deviationX <= getWidth() - 5)
        {
            g.fillEllipse(deviationX - 5, ellipseY, 10, 10);
        }
    }
}

void PitchMeter::setFrequency(float freq)
{
    if (!juce::MessageManager::existsAndIsCurrentThread())
    {
        juce::MessageManager::callAsync([this, freq]() { setFrequency(freq); });
        return;
    }
    currentFrequency = freq;
}

void PitchMeter::setTargetFrequency(float freq)
{
    if (!juce::MessageManager::existsAndIsCurrentThread())
    {
        juce::MessageManager::callAsync([this, freq]() { setTargetFrequency(freq); });
        return;
    }
    targetFrequency = freq;
}

void PitchMeter::timerCallback()
{
    // Smooth display update
    if (std::abs(displayFrequency - currentFrequency) > 0.1f)
    {
        displayFrequency += (currentFrequency - displayFrequency) * 0.3f;
        if (spectrogramVisualizer)
        {
            spectrogramVisualizer->setDetectedFrequency(displayFrequency);
            
            // Calculate actual output frequency based on pitch ratio
            // RubberBand uses: output = input / ratio
            float outputFrequency = displayFrequency;
            if (currentPitchRatio > 0.0f && displayFrequency > 0.0f)
            {
                // Output frequency = detected frequency / pitch ratio
                // This shows the actual real-time processed pitch
                outputFrequency = displayFrequency / currentPitchRatio;
            }
            
            spectrogramVisualizer->setProcessedFrequency(outputFrequency);
        }
        repaint();
    }
}

void PitchMeter::resized()
{
    if (spectrogramVisualizer)
        spectrogramVisualizer->setBounds(getLocalBounds());
}

void PitchMeter::pushAudioSample(float sample)
{
    if (spectrogramVisualizer)
        spectrogramVisualizer->pushSample(sample);
}

void PitchMeter::setVisualizerEnabled(bool enabled)
{
    if (spectrogramVisualizer)
        spectrogramVisualizer->setVisible(enabled);
}

juce::String PitchMeter::frequencyToNote(float frequency)
{
    if (frequency <= 0) return "";
    
    const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    
    float a4 = 440.0f;
    float c0 = a4 * std::pow(2.0f, -4.75f);
    
    int midiNote = static_cast<int>(std::round(12.0f * std::log2(frequency / c0)));
    int octave = midiNote / 12;
    int noteIndex = midiNote % 12;
    
    return juce::String(noteNames[noteIndex]) + juce::String(octave);
}

float PitchMeter::frequencyToCents(float frequency, float referenceFreq)
{
    if (frequency <= 0 || referenceFreq <= 0) return 0;
    return 1200.0f * std::log2(frequency / referenceFreq);
}

// Main Editor
PitchFlattenerAudioProcessorEditor::PitchFlattenerAudioProcessorEditor (PitchFlattenerAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      manualOverrideButton("manualOverride", audioProcessor.parameters),
      basePitchLatchButton("basePitchLatch", audioProcessor.parameters),
      hardFlattenModeButton("hardFlattenMode", audioProcessor.parameters),
      pitchAlgorithmSelector("pitchAlgorithm", audioProcessor.parameters),
      rbPitchModeSelector("rbPitchMode", audioProcessor.parameters),
      rbTransientsSelector("rbTransients", audioProcessor.parameters),
      rbPhaseSelector("rbPhase", audioProcessor.parameters),
      rbWindowSelector("rbWindow", audioProcessor.parameters)
{
    // Enable mouse tracking for help text
    setRepaintsOnMouseActivity(true);
    setMouseClickGrabsKeyboardFocus(false);
    
    // Configure look and feel
    lookAndFeel.setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
    lookAndFeel.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::darkgrey);
    lookAndFeel.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setLookAndFeel(&lookAndFeel);
    
    // Website link
    websiteLink.setButtonText("www.sweetjusticesound.com");
    websiteLink.setURL(juce::URL("https://www.sweetjusticesound.com"));
    websiteLink.setJustificationType(juce::Justification::centred);
    websiteLink.setColour(juce::HyperlinkButton::textColourId, juce::Colours::lightblue);
    websiteLink.setTooltip("Visit Sweet Justice Sound website for more plugins and music");
    addAndMakeVisible(websiteLink);
    
    // Title
    titleLabel.setText("SammyJs Pitch Flattener", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(24.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centred);
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    titleLabel.setTooltip("Real-time pitch flattening for Doppler effects and pitch modulation");
    addAndMakeVisible(titleLabel);
    
    // Pitch meter
    addAndMakeVisible(pitchMeter);
    
    // Preset Manager
    presetManager = std::make_unique<PresetManager>(audioProcessor);
    addAndMakeVisible(presetManager.get());
    
    // Create section labels with consistent styling
    auto createSectionLabel = [this](const juce::String& text, const juce::String& tooltip = "") {
        auto* label = new juce::Label();
        label->setText(text, juce::dontSendNotification);
        label->setFont(juce::Font(16.0f, juce::Font::bold));
        label->setJustificationType(juce::Justification::centred);
        label->setColour(juce::Label::textColourId, juce::Colours::lightblue);
        if (tooltip.isNotEmpty())
            label->setTooltip(tooltip);
        addAndMakeVisible(label);
        return label;
    };
    
    // Section labels
    mainControlsLabel.reset(createSectionLabel("Main Controls", "Core pitch flattening parameters"));
    overrideLabel.reset(createSectionLabel("Manual Override", "Override automatic pitch detection"));
    detectionLabel.reset(createSectionLabel("Pitch Detection", "Configure pitch detection behavior"));
    advancedLabel.reset(createSectionLabel("Advanced Detection", "Fine-tune pitch tracking stability"));
    rubberBandLabel.reset(createSectionLabel("RubberBand", "Pitch shifting engine parameters"));
    
    // Target pitch slider
    targetPitchSlider = std::make_unique<SliderWithReset>("targetPitch", audioProcessor.parameters);
    targetPitchSlider->slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    targetPitchSlider->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 80, 20);
    targetPitchSlider->slider.setTextValueSuffix(" Hz");
    targetPitchSlider->slider.setTooltip("The target frequency to flatten all pitches to. "
                                 "For example, set to 440Hz to make everything sound like an A4 note.");
    addAndMakeVisible(targetPitchSlider.get());
    
    targetPitchLabel.setText("Flatten To", juce::dontSendNotification);
    targetPitchLabel.setJustificationType(juce::Justification::centred);
    targetPitchLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    targetPitchLabel.setTooltip("The frequency that all detected pitches will be flattened to");
    addAndMakeVisible(targetPitchLabel);
    
    // Smoothing time slider
    smoothingTimeSlider = std::make_unique<SliderWithReset>("smoothingTimeMs", audioProcessor.parameters);
    smoothingTimeSlider->slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    smoothingTimeSlider->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 80, 20);
    smoothingTimeSlider->slider.setTextValueSuffix(" ms");
    smoothingTimeSlider->slider.setTooltip("Time for pitch detection to adapt to changes. "
                                   "5-50ms = fast response, 100-150ms = natural Doppler flattening, "
                                   "150-200ms = smooth ambient drift.");
    addAndMakeVisible(smoothingTimeSlider.get());
    
    smoothingTimeLabel.setText("Smoothing Time", juce::dontSendNotification);
    smoothingTimeLabel.setJustificationType(juce::Justification::centred);
    smoothingTimeLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    smoothingTimeLabel.setTooltip("How quickly the pitch correction responds to changes");
    addAndMakeVisible(smoothingTimeLabel);
    
    // Mix slider
    mixSlider = std::make_unique<SliderWithReset>("mix", audioProcessor.parameters);
    mixSlider->slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    mixSlider->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
    mixSlider->slider.setTextValueSuffix(" %");
    mixSlider->slider.setTooltip("Blend between the original (dry) and pitch-flattened (wet) signal. "
                         "100% = fully processed, 0% = original signal.");
    addAndMakeVisible(mixSlider.get());
    
    mixLabel.setText("Mix", juce::dontSendNotification);
    mixLabel.setJustificationType(juce::Justification::centred);
    mixLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    mixLabel.setTooltip("Wet/dry mix control");
    addAndMakeVisible(mixLabel);
    
    // Lookahead slider
    lookaheadSlider = std::make_unique<SliderWithReset>("lookahead", audioProcessor.parameters);
    lookaheadSlider->slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lookaheadSlider->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
    lookaheadSlider->slider.setTextValueSuffix("x");
    lookaheadSlider->slider.setTooltip("Lookahead buffer multiplier. Higher values provide more consistent processing "
                               "but increase latency. 2x = buffer 2x the block size ahead.");
    addAndMakeVisible(lookaheadSlider.get());
    
    lookaheadLabel.setText("Lookahead", juce::dontSendNotification);
    lookaheadLabel.setJustificationType(juce::Justification::centred);
    lookaheadLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    lookaheadLabel.setTooltip("Buffer lookahead for smoother processing");
    addAndMakeVisible(lookaheadLabel);
    
    // Manual override controls
    manualOverrideButton.setButtonText("Manual Override");
    manualOverrideButton.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
    manualOverrideButton.setTooltip("Enable to use a fixed frequency instead of auto-detected base pitch. Double-click to reset to default.");
    addAndMakeVisible(manualOverrideButton);
    
    overrideFreqSlider = std::make_unique<SliderWithReset>("overrideFreq", audioProcessor.parameters);
    overrideFreqSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    overrideFreqSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    overrideFreqSlider->slider.setTextValueSuffix(" Hz");
    overrideFreqSlider->slider.setTooltip("Manual frequency to flatten to when override is enabled");
    addAndMakeVisible(overrideFreqSlider.get());
    
    overrideFreqLabel.setText("Override Freq:", juce::dontSendNotification);
    overrideFreqLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    overrideFreqLabel.setTooltip("The frequency to use when manual override is enabled");
    addAndMakeVisible(overrideFreqLabel);
    
    overrideFreqValueLabel.setText("", juce::dontSendNotification);
    overrideFreqValueLabel.setJustificationType(juce::Justification::centredLeft);
    overrideFreqValueLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    overrideFreqValueLabel.setTooltip("Musical note name of the override frequency");
    addAndMakeVisible(overrideFreqValueLabel);
    
    // Base pitch latch controls
    basePitchLatchButton.setButtonText("Base Pitch Latch");
    basePitchLatchButton.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
    basePitchLatchButton.setTooltip("Enable to latch onto the first stable pitch detected and use it as the flattening target. Double-click to reset to default.");
    addAndMakeVisible(basePitchLatchButton);
    
    resetBasePitchButton.setButtonText("Reset Latch");
    resetBasePitchButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);
    resetBasePitchButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    resetBasePitchButton.setTooltip("Reset the base pitch latch to capture a new reference pitch");
    addAndMakeVisible(resetBasePitchButton);
    
    latchStatusLabel.setText("Latch Status:", juce::dontSendNotification);
    latchStatusLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    latchStatusLabel.setTooltip("Shows whether base pitch is currently latched");
    addAndMakeVisible(latchStatusLabel);
    
    latchStatusValueLabel.setText("Unlocked", juce::dontSendNotification);
    latchStatusValueLabel.setJustificationType(juce::Justification::centredLeft);
    latchStatusValueLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    latchStatusValueLabel.setTooltip("Current latch status and locked frequency");
    addAndMakeVisible(latchStatusValueLabel);
    
    // Flatten sensitivity control
    flattenSensitivitySlider = std::make_unique<SliderWithReset>("flattenSensitivity", audioProcessor.parameters);
    flattenSensitivitySlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    flattenSensitivitySlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    flattenSensitivitySlider->slider.setTextValueSuffix(" %");
    flattenSensitivitySlider->slider.setTooltip("Ignore pitch variations smaller than this percentage. 0% = flatten all variations, 5% = ignore small wobbles");
    addAndMakeVisible(flattenSensitivitySlider.get());
    
    flattenSensitivityLabel.setText("Sensitivity:", juce::dontSendNotification);
    flattenSensitivityLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    flattenSensitivityLabel.setTooltip("Minimum pitch variation to trigger flattening");
    addAndMakeVisible(flattenSensitivityLabel);
    
    // Hard flatten mode
    hardFlattenModeButton.setButtonText("Freeze Ratio");
    hardFlattenModeButton.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
    hardFlattenModeButton.setTooltip("Freeze the pitch ratio when base pitch is latched. This completely locks the output pitch, "
                                     "ignoring all pitch variations for true flattening. Double-click to reset to default.");
    addAndMakeVisible(hardFlattenModeButton);
    
    // Flattening target display
    basePitchLabel.setText("Flattening To:", juce::dontSendNotification);
    basePitchLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    basePitchLabel.setTooltip("The target frequency that all detected pitches are being flattened to.");
    addAndMakeVisible(basePitchLabel);
    
    basePitchValueLabel.setText("--", juce::dontSendNotification);
    basePitchValueLabel.setJustificationType(juce::Justification::centredLeft);
    basePitchValueLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
    basePitchValueLabel.setTooltip("The current frequency all pitches are being flattened to");
    addAndMakeVisible(basePitchValueLabel);
    
    // Pitch detection controls
    detectionRateSlider = std::make_unique<SliderWithReset>("detectionRate", audioProcessor.parameters);
    detectionRateSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    detectionRateSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    detectionRateSlider->slider.setTextValueSuffix(" smp");
    detectionRateSlider->slider.setTooltip("How often pitch detection runs (in samples). Lower = more responsive but more CPU. 64-128 samples recommended.");
    addAndMakeVisible(detectionRateSlider.get());
    
    detectionRateLabel.setText("Detection Rate:", juce::dontSendNotification);
    detectionRateLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    detectionRateLabel.setTooltip("How often pitch detection runs (in samples)");
    addAndMakeVisible(detectionRateLabel);
    
    pitchThresholdSlider = std::make_unique<SliderWithReset>("pitchThreshold", audioProcessor.parameters);
    pitchThresholdSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    pitchThresholdSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    pitchThresholdSlider->slider.setTooltip("Pitch detection confidence threshold. Lower = more sensitive but may get false detections. 0.10-0.15 recommended.");
    addAndMakeVisible(pitchThresholdSlider.get());
    
    pitchThresholdLabel.setText("Threshold:", juce::dontSendNotification);
    pitchThresholdLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    pitchThresholdLabel.setTooltip("Detection sensitivity - lower values detect weaker pitches");
    addAndMakeVisible(pitchThresholdLabel);
    
    minFreqSlider = std::make_unique<SliderWithReset>("minFreq", audioProcessor.parameters);
    minFreqSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    minFreqSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    minFreqSlider->slider.setTextValueSuffix(" Hz");
    minFreqSlider->slider.setTooltip("Minimum frequency to detect. Set this below your source's lowest expected pitch.");
    addAndMakeVisible(minFreqSlider.get());
    
    minFreqLabel.setText("Min Freq:", juce::dontSendNotification);
    minFreqLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    minFreqLabel.setTooltip("Lowest frequency the detector will look for");
    addAndMakeVisible(minFreqLabel);
    
    maxFreqSlider = std::make_unique<SliderWithReset>("maxFreq", audioProcessor.parameters);
    maxFreqSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    maxFreqSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    maxFreqSlider->slider.setTextValueSuffix(" Hz");
    maxFreqSlider->slider.setTooltip("Maximum frequency to detect. Set this above your source's highest expected pitch.");
    addAndMakeVisible(maxFreqSlider.get());
    
    maxFreqLabel.setText("Max Freq:", juce::dontSendNotification);
    maxFreqLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    maxFreqLabel.setTooltip("Highest frequency the detector will look for");
    addAndMakeVisible(maxFreqLabel);
    
    volumeThresholdSlider = std::make_unique<SliderWithReset>("volumeThreshold", audioProcessor.parameters);
    volumeThresholdSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    volumeThresholdSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    volumeThresholdSlider->slider.setTextValueSuffix(" dB");
    volumeThresholdSlider->slider.setTooltip("Minimum volume level for pitch detection to activate. Signal must be louder than this to detect pitch.");
    addAndMakeVisible(volumeThresholdSlider.get());
    
    volumeThresholdLabel.setText("Volume Gate:", juce::dontSendNotification);
    volumeThresholdLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    volumeThresholdLabel.setTooltip("Volume threshold for pitch detection");
    addAndMakeVisible(volumeThresholdLabel);
    
    volumeLevelLabel.setText("Current: -60.0 dB", juce::dontSendNotification);
    volumeLevelLabel.setJustificationType(juce::Justification::centred);
    volumeLevelLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    volumeLevelLabel.setTooltip("Current input volume level");
    addAndMakeVisible(volumeLevelLabel);
    
    // Advanced pitch detection controls
    pitchHoldTimeSlider = std::make_unique<SliderWithReset>("pitchHoldTime", audioProcessor.parameters);
    pitchHoldTimeSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    pitchHoldTimeSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    pitchHoldTimeSlider->slider.setTextValueSuffix(" ms");
    pitchHoldTimeSlider->slider.setTooltip("Time to hold current pitch before accepting a new one. Prevents rapid jumping between pitches. 200-500ms recommended.");
    addAndMakeVisible(pitchHoldTimeSlider.get());
    
    pitchHoldTimeLabel.setText("Hold Time:", juce::dontSendNotification);
    pitchHoldTimeLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    pitchHoldTimeLabel.setTooltip("Minimum time before switching to a new pitch");
    addAndMakeVisible(pitchHoldTimeLabel);
    
    pitchJumpThresholdSlider = std::make_unique<SliderWithReset>("pitchJumpThreshold", audioProcessor.parameters);
    pitchJumpThresholdSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    pitchJumpThresholdSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    pitchJumpThresholdSlider->slider.setTextValueSuffix(" Hz");
    pitchJumpThresholdSlider->slider.setTooltip("Maximum allowed pitch jump in Hz. Larger jumps are rejected as false detections. 50-200Hz prevents octave errors.");
    addAndMakeVisible(pitchJumpThresholdSlider.get());
    
    pitchJumpThresholdLabel.setText("Jump Limit:", juce::dontSendNotification);
    pitchJumpThresholdLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    pitchJumpThresholdLabel.setTooltip("Maximum Hz change allowed between detections");
    addAndMakeVisible(pitchJumpThresholdLabel);
    
    minConfidenceSlider = std::make_unique<SliderWithReset>("minConfidence", audioProcessor.parameters);
    minConfidenceSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    minConfidenceSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    minConfidenceSlider->slider.setTooltip("Minimum confidence level to accept a pitch. Higher values = more stable but may miss quick changes. 0.5-0.8 is typical.");
    addAndMakeVisible(minConfidenceSlider.get());
    
    minConfidenceLabel.setText("Confidence:", juce::dontSendNotification);
    minConfidenceLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    minConfidenceLabel.setTooltip("How certain the detector must be before accepting a pitch");
    addAndMakeVisible(minConfidenceLabel);
    
    pitchSmoothingSlider = std::make_unique<SliderWithReset>("pitchSmoothing", audioProcessor.parameters);
    pitchSmoothingSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    pitchSmoothingSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    pitchSmoothingSlider->slider.setTooltip("Additional smoothing for pitch detection. 0 = no smoothing, 0.99 = maximum smoothing. 0.8-0.9 reduces jitter.");
    addAndMakeVisible(pitchSmoothingSlider.get());
    
    pitchSmoothingLabel.setText("Smoothing:", juce::dontSendNotification);
    pitchSmoothingLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    pitchSmoothingLabel.setTooltip("How much to smooth the detected pitch values");
    addAndMakeVisible(pitchSmoothingLabel);
    
    // Detection filter controls
    detectionHighpassSlider = std::make_unique<SliderWithReset>("detectionHighpass", audioProcessor.parameters);
    detectionHighpassSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    detectionHighpassSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    detectionHighpassSlider->slider.setTextValueSuffix(" Hz");
    detectionHighpassSlider->slider.setTooltip("High-pass filter for pitch detection. Cuts out low frequencies to improve detection accuracy.");
    addAndMakeVisible(detectionHighpassSlider.get());
    
    detectionHighpassLabel.setText("Detection HP:", juce::dontSendNotification);
    detectionHighpassLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    detectionHighpassLabel.setTooltip("High-pass filter frequency for pitch detection signal");
    addAndMakeVisible(detectionHighpassLabel);
    
    detectionLowpassSlider = std::make_unique<SliderWithReset>("detectionLowpass", audioProcessor.parameters);
    detectionLowpassSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    detectionLowpassSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    detectionLowpassSlider->slider.setTextValueSuffix(" Hz");
    detectionLowpassSlider->slider.setTooltip("Low-pass filter for pitch detection. Cuts out high frequencies to reduce noise. 6kHz gives good results.");
    addAndMakeVisible(detectionLowpassSlider.get());
    
    detectionLowpassLabel.setText("Detection LP:", juce::dontSendNotification);
    detectionLowpassLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    detectionLowpassLabel.setTooltip("Low-pass filter frequency for pitch detection signal");
    addAndMakeVisible(detectionLowpassLabel);
    
    // Pitch algorithm selector
    pitchAlgorithmSelector.addItem("YIN", 1);
    pitchAlgorithmSelector.addItem("WORLD (DIO) FFT", 2);
    pitchAlgorithmSelector.setTooltip("Choose pitch detection algorithm. YIN is fast autocorrelation-based for clean signals. WORLD DIO uses FFT-based analysis for better performance with noisy field recordings. Double-click to reset to default.");
    addAndMakeVisible(pitchAlgorithmSelector);
    
    pitchAlgorithmLabel.setText("Algorithm:", juce::dontSendNotification);
    pitchAlgorithmLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    pitchAlgorithmLabel.setTooltip("Pitch detection algorithm to use");
    addAndMakeVisible(pitchAlgorithmLabel);
    
    // DIO-specific controls
    dioSpeedSlider = std::make_unique<SliderWithReset>("dioSpeed", audioProcessor.parameters);
    dioSpeedSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    dioSpeedSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    dioSpeedSlider->slider.setTooltip("DIO analysis speed. 1=fastest (best for real-time), 12=most accurate (slower). Lower values are more responsive.");
    addAndMakeVisible(dioSpeedSlider.get());
    
    dioSpeedLabel.setText("DIO Speed:", juce::dontSendNotification);
    dioSpeedLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    dioSpeedLabel.setTooltip("Processing speed vs accuracy tradeoff");
    addAndMakeVisible(dioSpeedLabel);
    
    dioFramePeriodSlider = std::make_unique<SliderWithReset>("dioFramePeriod", audioProcessor.parameters);
    dioFramePeriodSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    dioFramePeriodSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    dioFramePeriodSlider->slider.setTextValueSuffix(" ms");
    dioFramePeriodSlider->slider.setTooltip("Frame analysis period in milliseconds. Lower values = more responsive but more CPU.");
    addAndMakeVisible(dioFramePeriodSlider.get());
    
    dioFramePeriodLabel.setText("Frame Period:", juce::dontSendNotification);
    dioFramePeriodLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    dioFramePeriodLabel.setTooltip("How often DIO analyzes pitch");
    addAndMakeVisible(dioFramePeriodLabel);
    
    dioAllowedRangeSlider = std::make_unique<SliderWithReset>("dioAllowedRange", audioProcessor.parameters);
    dioAllowedRangeSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    dioAllowedRangeSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    dioAllowedRangeSlider->slider.setTooltip("Threshold for fixing F0 contour. Lower = more strict pitch tracking, higher = allows more variation.");
    addAndMakeVisible(dioAllowedRangeSlider.get());
    
    dioAllowedRangeLabel.setText("Allowed Range:", juce::dontSendNotification);
    dioAllowedRangeLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    dioAllowedRangeLabel.setTooltip("F0 contour smoothing threshold");
    addAndMakeVisible(dioAllowedRangeLabel);
    
    dioChannelsSlider = std::make_unique<SliderWithReset>("dioChannelsInOctave", audioProcessor.parameters);
    dioChannelsSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    dioChannelsSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    dioChannelsSlider->slider.setTooltip("Frequency resolution. More channels = better frequency accuracy but more CPU. 2-4 recommended.");
    addAndMakeVisible(dioChannelsSlider.get());
    
    dioChannelsLabel.setText("Channels/Oct:", juce::dontSendNotification);
    dioChannelsLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    dioChannelsLabel.setTooltip("Frequency analysis resolution");
    addAndMakeVisible(dioChannelsLabel);
    
    dioBufferTimeSlider = std::make_unique<SliderWithReset>("dioBufferTime", audioProcessor.parameters);
    dioBufferTimeSlider->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    dioBufferTimeSlider->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    dioBufferTimeSlider->slider.setTextValueSuffix(" s");
    dioBufferTimeSlider->slider.setTooltip("Analysis buffer time. Larger = better accuracy but more latency. You'll get silence for this duration when switching to DIO.");
    addAndMakeVisible(dioBufferTimeSlider.get());
    
    dioBufferTimeLabel.setText("Buffer Time:", juce::dontSendNotification);
    dioBufferTimeLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    dioBufferTimeLabel.setTooltip("DIO analysis buffer duration");
    addAndMakeVisible(dioBufferTimeLabel);
    
    // RubberBand controls
    rbExpandButton.setTooltip("Show/hide RubberBand settings");
    rbExpandButton.onClick = [this]() {
        rbSectionExpanded = !rbSectionExpanded;
        rbExpandButton.setButtonText(rbSectionExpanded ? "▼" : "▶");
        
        // Show/hide RubberBand controls
        rbFormantPreserveButton.setVisible(rbSectionExpanded);
        rbFormantPreserveLabel.setVisible(rbSectionExpanded);
        rbPitchModeSelector.setVisible(rbSectionExpanded);
        rbPitchModeLabel.setVisible(rbSectionExpanded);
        rbTransientsSelector.setVisible(rbSectionExpanded);
        rbTransientsLabel.setVisible(rbSectionExpanded);
        rbPhaseSelector.setVisible(rbSectionExpanded);
        rbPhaseLabel.setVisible(rbSectionExpanded);
        rbWindowSelector.setVisible(rbSectionExpanded);
        rbWindowLabel.setVisible(rbSectionExpanded);
        
        // Resize window
        // RubberBand section adds: 20 (spacing) + 25 (header) + 5*32 (controls) = 205 pixels
        int newHeight = rbSectionExpanded ? (870 + 180) : 870;
        setSize(getWidth(), static_cast<int>(newHeight * currentScale));
        resized();  // Force layout recalculation
        repaint();  // Force redraw to update background
    };
    addAndMakeVisible(rbExpandButton);
    
    rbFormantPreserveButton.setButtonText("Preserve");
    rbFormantPreserveButton.setTooltip("Preserve formants during pitch shifting for more natural sound");
    rbFormantPreserveButton.setVisible(false);  // Initially hidden
    addAndMakeVisible(rbFormantPreserveButton);
    
    rbFormantPreserveLabel.setText("Formants:", juce::dontSendNotification);
    rbFormantPreserveLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    rbFormantPreserveLabel.setTooltip("Formant preservation mode");
    rbFormantPreserveLabel.setVisible(false);
    addAndMakeVisible(rbFormantPreserveLabel);
    
    rbPitchModeSelector.addItem("High Speed", 1);
    rbPitchModeSelector.addItem("High Quality", 2);
    rbPitchModeSelector.addItem("High Consistency", 3);
    rbPitchModeSelector.setSelectedId(3);
    rbPitchModeSelector.setTooltip("Pitch shifting algorithm mode");
    rbPitchModeSelector.setVisible(false);
    addAndMakeVisible(rbPitchModeSelector);
    
    rbPitchModeLabel.setText("Pitch Mode:", juce::dontSendNotification);
    rbPitchModeLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    rbPitchModeLabel.setVisible(false);
    addAndMakeVisible(rbPitchModeLabel);
    
    rbTransientsSelector.addItem("Crisp", 1);
    rbTransientsSelector.addItem("Mixed", 2);
    rbTransientsSelector.addItem("Smooth", 3);
    rbTransientsSelector.setSelectedId(2);
    rbTransientsSelector.setTooltip("Transient handling mode");
    rbTransientsSelector.setVisible(false);
    addAndMakeVisible(rbTransientsSelector);
    
    rbTransientsLabel.setText("Transients:", juce::dontSendNotification);
    rbTransientsLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    rbTransientsLabel.setVisible(false);
    addAndMakeVisible(rbTransientsLabel);
    
    rbPhaseSelector.addItem("Laminar", 1);
    rbPhaseSelector.addItem("Independent", 2);
    rbPhaseSelector.setSelectedId(1);
    rbPhaseSelector.setTooltip("Phase coherence mode - Laminar keeps channels together");
    rbPhaseSelector.setVisible(false);
    addAndMakeVisible(rbPhaseSelector);
    
    rbPhaseLabel.setText("Phase:", juce::dontSendNotification);
    rbPhaseLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    rbPhaseLabel.setVisible(false);
    addAndMakeVisible(rbPhaseLabel);
    
    rbWindowSelector.addItem("Standard", 1);
    rbWindowSelector.addItem("Short", 2);
    rbWindowSelector.addItem("Long", 3);
    rbWindowSelector.setSelectedId(1);
    rbWindowSelector.setTooltip("Analysis window size");
    rbWindowSelector.setVisible(false);
    addAndMakeVisible(rbWindowSelector);
    
    rbWindowLabel.setText("Window:", juce::dontSendNotification);
    rbWindowLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    rbWindowLabel.setVisible(false);
    addAndMakeVisible(rbWindowLabel);
    
    // Status label
    statusLabel.setText("Ready", juce::dontSendNotification);
    statusLabel.setJustificationType(juce::Justification::centred);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    statusLabel.setFont(juce::Font(14.0f)); // Smaller font
    statusLabel.setTooltip("Current processing status");
    addAndMakeVisible(statusLabel);
    
    // Attachments
    targetPitchAttachment = targetPitchSlider->createAttachment();
    smoothingTimeAttachment = smoothingTimeSlider->createAttachment();
    mixAttachment = mixSlider->createAttachment();
    lookaheadAttachment = lookaheadSlider->createAttachment();
    
    manualOverrideAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.parameters, "manualOverride", manualOverrideButton);
    
    overrideFreqAttachment = overrideFreqSlider->createAttachment();
    detectionRateAttachment = detectionRateSlider->createAttachment();
    pitchThresholdAttachment = pitchThresholdSlider->createAttachment();
    minFreqAttachment = minFreqSlider->createAttachment();
    maxFreqAttachment = maxFreqSlider->createAttachment();
    
    volumeThresholdAttachment = volumeThresholdSlider->createAttachment();
    pitchHoldTimeAttachment = pitchHoldTimeSlider->createAttachment();
    pitchJumpThresholdAttachment = pitchJumpThresholdSlider->createAttachment();
    minConfidenceAttachment = minConfidenceSlider->createAttachment();
    pitchSmoothingAttachment = pitchSmoothingSlider->createAttachment();
    
    basePitchLatchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.parameters, "basePitchLatch", basePitchLatchButton);
    
    flattenSensitivityAttachment = flattenSensitivitySlider->createAttachment();
    
    hardFlattenModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.parameters, "hardFlattenMode", hardFlattenModeButton);
    
    detectionHighpassAttachment = detectionHighpassSlider->createAttachment();
    detectionLowpassAttachment = detectionLowpassSlider->createAttachment();
    
    pitchAlgorithmAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.parameters, "pitchAlgorithm", pitchAlgorithmSelector);
    
    dioSpeedAttachment = dioSpeedSlider->createAttachment();
    dioFramePeriodAttachment = dioFramePeriodSlider->createAttachment();
    dioAllowedRangeAttachment = dioAllowedRangeSlider->createAttachment();
    dioChannelsAttachment = dioChannelsSlider->createAttachment();
    dioBufferTimeAttachment = dioBufferTimeSlider->createAttachment();
    
    // RubberBand attachments
    rbFormantPreserveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.parameters, "rbFormantPreserve", rbFormantPreserveButton);
    
    rbPitchModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.parameters, "rbPitchMode", rbPitchModeSelector);
    
    rbTransientsAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.parameters, "rbTransients", rbTransientsSelector);
    
    rbPhaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.parameters, "rbPhase", rbPhaseSelector);
    
    rbWindowAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.parameters, "rbWindow", rbWindowSelector);
    
    // Setup algorithm change callback
    pitchAlgorithmSelector.onChange = [this]() { updateAlgorithmControls(); };
    
    // Setup reset button click handler
    resetBasePitchButton.onClick = [this]() {
        auto* param = audioProcessor.parameters.getParameter("resetBasePitch");
        if (param)
            param->setValueNotifyingHost(1.0f);
    };
    
    // Convert mix slider to percentage display
    mixSlider->slider.textFromValueFunction = [](double value) {
        return juce::String(static_cast<int>(value * 100));
    };
    
    mixSlider->slider.valueFromTextFunction = [](const juce::String& text) {
        return text.getDoubleValue() / 100.0;
    };
    
    // Start timer for updates
    startTimerHz(30);
    
    // Tooltips will work automatically with the tooltipWindow member
    
    // Setup help text label
    helpTextLabel.setJustificationType(juce::Justification::centred);
    helpTextLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    helpTextLabel.setText("Hover over controls for help", juce::dontSendNotification);
    addAndMakeVisible(helpTextLabel);
    
    // Setup about button
    aboutButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgrey.darker());
    aboutButton.setColour(juce::TextButton::textColourOnId, juce::Colours::lightgrey);
    aboutButton.onClick = [this] 
    { 
        if (!aboutWindow)
            aboutWindow = std::make_unique<AboutWindow>();
        else
            aboutWindow->setVisible(true);
    };
    addAndMakeVisible(aboutButton);
    
    // Initial algorithm control visibility
    updateAlgorithmControls();
    
    // Set size at the end after all components are created
    setSize (defaultWidth, defaultHeight);
    setResizable(true, true);
    setResizeLimits(600, 510, 2000, 1700);  // Allow flexible resizing with proper scaling
}

PitchFlattenerAudioProcessorEditor::~PitchFlattenerAudioProcessorEditor()
{
    setLookAndFeel(nullptr);
}

void PitchFlattenerAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Fill the entire background first
    g.fillAll(juce::Colour(0xff1a1a1a));
    
    // Save the current graphics state
    juce::Graphics::ScopedSaveState state(g);
    
    // Calculate centering offset
    float scaledWidth = defaultWidth * currentScale;
    float scaledHeight = defaultHeight * currentScale;
    float xOffset = (getWidth() - scaledWidth) * 0.5f;
    float yOffset = (getHeight() - scaledHeight) * 0.5f;
    
    // Apply scaling and centering transform
    g.addTransform(juce::AffineTransform::scale(currentScale)
                                        .translated(xOffset, yOffset));
    
    // Draw at default size (the transform will scale it)
    auto scaledBounds = juce::Rectangle<int>(0, 0, defaultWidth, defaultHeight);
    
    // Gradient background for more depth
    auto bgColour1 = juce::Colour(0xff1a1a1a);
    auto bgColour2 = juce::Colour(0xff2a2a2a);
    g.setGradientFill(juce::ColourGradient(bgColour1, 0, 0, 
                                           bgColour2, 0, static_cast<float>(defaultHeight), false));
    g.fillRect(scaledBounds);
    
    // Draw pitch meter background with subtle gradient
    auto meterBounds = juce::Rectangle<float>(10, 65, defaultWidth - 20, 140);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff2a2a2a), meterBounds.getX(), meterBounds.getY(),
                                           juce::Colour(0xff1f1f1f), meterBounds.getX(), meterBounds.getBottom(), false));
    g.fillRoundedRectangle(meterBounds, 12);
    
    // Draw subtle inner shadow for depth
    g.setColour(juce::Colours::black.withAlpha(0.3f));
    g.drawRoundedRectangle(meterBounds.reduced(1), 11, 1);
    
    // Draw subtle highlight on top edge
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.drawLine(meterBounds.getX() + 12, meterBounds.getY() + 1, 
               meterBounds.getRight() - 12, meterBounds.getY() + 1, 1);
    
    // Draw subtle backgrounds for panels
    // Use dynamic height when RubberBand is expanded
    int layoutHeight = rbSectionExpanded ? (defaultHeight + 180) : defaultHeight;
    auto bounds = juce::Rectangle<int>(0, 0, defaultWidth, layoutHeight);
    bounds.removeFromTop(235); // Skip header, preset bar, meter and status label
    auto mainArea = bounds.reduced(15, 0);
    auto leftPanelWidth = static_cast<int>(mainArea.getWidth() * 0.52f);
    
    // Left panel background with subtle gradient
    auto leftPanelBounds = juce::Rectangle<float>(mainArea.getX(), mainArea.getY(), 
                                                  leftPanelWidth - 8, mainArea.getHeight() - 15);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff252525), leftPanelBounds.getCentreX(), leftPanelBounds.getY(),
                                           juce::Colour(0xff1f1f1f), leftPanelBounds.getCentreX(), leftPanelBounds.getBottom(), false));
    g.fillRoundedRectangle(leftPanelBounds, 12);
    
    // Right panel background
    auto rightPanelBounds = juce::Rectangle<float>(mainArea.getX() + leftPanelWidth + 8, mainArea.getY(), 
                                                   mainArea.getWidth() - leftPanelWidth - 8, mainArea.getHeight() - 15);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff252525), rightPanelBounds.getCentreX(), rightPanelBounds.getY(),
                                           juce::Colour(0xff1f1f1f), rightPanelBounds.getCentreX(), rightPanelBounds.getBottom(), false));
    g.fillRoundedRectangle(rightPanelBounds, 12);
    
    // Draw subtle separator with gradient
    auto sepX = mainArea.getX() + leftPanelWidth;
    g.setGradientFill(juce::ColourGradient(juce::Colours::transparentBlack, 0, mainArea.getY() + 20,
                                           juce::Colours::grey.withAlpha(0.2f), 0, mainArea.getCentreY(),
                                           false));
    g.drawLine(sepX, mainArea.getY() + 20, sepX, mainArea.getBottom() - 20, 1.0f);
}

void PitchFlattenerAudioProcessorEditor::resized()
{
    // Calculate scale factor based on current window size
    // Use actual height when RubberBand is expanded
    int actualHeight = rbSectionExpanded ? (defaultHeight + 180) : defaultHeight;
    float widthScale = static_cast<float>(getWidth()) / static_cast<float>(defaultWidth);
    float heightScale = static_cast<float>(getHeight()) / static_cast<float>(actualHeight);
    currentScale = juce::jmin(widthScale, heightScale);
    
    
    // Calculate centering offset
    float scaledWidth = defaultWidth * currentScale;
    float scaledHeight = actualHeight * currentScale;
    float xOffset = (getWidth() - scaledWidth) * 0.5f;
    float yOffset = (getHeight() - scaledHeight) * 0.5f;
    
    // Create a transform to scale and center all components
    auto transform = juce::AffineTransform::scale(currentScale)
                                          .translated(xOffset, yOffset);
    
    // Apply the transform to all child components except the resize corner
    for (auto* child : getChildren())
    {
        // Skip the ResizableCornerComponent which JUCE adds automatically
        if (dynamic_cast<juce::ResizableCornerComponent*>(child) == nullptr)
        {
            child->setTransform(transform);
        }
    }
    
    // Layout components at their default positions (unscaled)
    // Use actual height when RubberBand is expanded
    int layoutHeight = rbSectionExpanded ? (defaultHeight + 180) : defaultHeight;
    auto area = juce::Rectangle<int>(0, 0, defaultWidth, layoutHeight);
    
    // Top section - website link and title
    websiteLink.setBounds(area.removeFromTop(20));
    titleLabel.setBounds(area.removeFromTop(40));
    
    // Preset Manager section - more compact
    area.removeFromTop(3);
    presetManager->setBounds(area.removeFromTop(30).reduced(15, 0));
    
    // Pitch meter section - full width with proper height
    area.removeFromTop(5); // Space before pitch meter
    auto meterArea = area.removeFromTop(140).reduced(15, 5);
    pitchMeter.setBounds(meterArea);
    
    // Status label positioned in the lower part of the dark grey meter area
    // Position it above the pitch deviation meter line
    auto statusArea = meterArea;
    // Place it 25 pixels from the bottom of the meter area
    statusArea.setY(meterArea.getBottom() - 25);
    statusArea.setHeight(20);
    statusLabel.setBounds(statusArea.reduced(20, 0));
    
    area.removeFromTop(20); // Space after status label
    
    // Split into left and right sections
    auto mainArea = area.reduced(15, 0);
    auto leftPanelWidth = static_cast<int>(mainArea.getWidth() * 0.52f); // 52% for main controls to accommodate reset buttons
    auto leftPanel = mainArea.removeFromLeft(leftPanelWidth);
    mainArea.removeFromLeft(10); // Gap between panels
    auto rightPanel = mainArea;
    
    // LEFT PANEL - Main controls
    auto leftContent = leftPanel;
    
    // Main controls section
    if (mainControlsLabel) 
        mainControlsLabel->setBounds(leftContent.removeFromTop(25));
    leftContent.removeFromTop(5);
    
    // Target pitch (larger, centered)
    auto targetArea = leftContent.removeFromTop(125);
    targetPitchLabel.setBounds(targetArea.removeFromTop(20));
    targetPitchSlider->setBounds(targetArea.withSizeKeepingCentre(140, 100));
    
    // Base pitch display
    auto basePitchArea = leftContent.removeFromTop(35).reduced(20, 0);
    basePitchLabel.setBounds(basePitchArea.removeFromLeft(150));
    basePitchValueLabel.setBounds(basePitchArea);
    
    // Smoothing, Mix and Lookahead side by side
    leftContent.removeFromTop(10);
    auto bottomControls = leftContent.removeFromTop(95);
    auto thirdWidth = bottomControls.getWidth() / 3;
    
    auto smoothingArea = bottomControls.removeFromLeft(thirdWidth);
    smoothingTimeLabel.setBounds(smoothingArea.removeFromTop(20));
    smoothingTimeSlider->setBounds(smoothingArea.withSizeKeepingCentre(120, 80));
    
    auto mixArea = bottomControls.removeFromLeft(thirdWidth);
    mixLabel.setBounds(mixArea.removeFromTop(20));
    mixSlider->setBounds(mixArea.withSizeKeepingCentre(120, 80));
    
    auto lookaheadArea = bottomControls;
    lookaheadLabel.setBounds(lookaheadArea.removeFromTop(20));
    lookaheadSlider->setBounds(lookaheadArea.withSizeKeepingCentre(120, 80));
    
    // Override section
    leftContent.removeFromTop(10);
    if (overrideLabel)
        overrideLabel->setBounds(leftContent.removeFromTop(25));
    leftContent.removeFromTop(5);
    
    auto overrideArea = leftContent.removeFromTop(75);
    manualOverrideButton.setBounds(overrideArea.removeFromTop(30));
    overrideArea.removeFromTop(5);
    auto freqArea = overrideArea.removeFromTop(30);
    overrideFreqLabel.setBounds(freqArea.removeFromLeft(100));
    overrideFreqSlider->setBounds(freqArea.removeFromLeft(240));
    freqArea.removeFromLeft(10);
    overrideFreqValueLabel.setBounds(freqArea);
    
    // Base pitch latch section
    leftContent.removeFromTop(10);
    auto latchSectionLabel = std::make_unique<juce::Label>();
    latchSectionLabel->setText("Base Pitch Latch", juce::dontSendNotification);
    latchSectionLabel->setFont(juce::Font(16.0f, juce::Font::bold));
    latchSectionLabel->setJustificationType(juce::Justification::left);
    latchSectionLabel->setColour(juce::Label::textColourId, juce::Colours::lightblue);
    latchSectionLabel->setBounds(leftContent.removeFromTop(25));
    
    leftContent.removeFromTop(5);
    auto latchArea = leftContent.removeFromTop(105);  // Reduced since hard flatten is on same line
    
    // First row - toggle and reset button
    auto latchRow1 = latchArea.removeFromTop(35);
    basePitchLatchButton.setBounds(latchRow1.removeFromLeft(180));
    latchRow1.removeFromLeft(20);
    resetBasePitchButton.setBounds(latchRow1.removeFromLeft(100));
    
    // Second row - status display
    latchArea.removeFromTop(10);
    auto latchRow2 = latchArea.removeFromTop(30);
    latchStatusLabel.setBounds(latchRow2.removeFromLeft(80));
    latchStatusValueLabel.setBounds(latchRow2);
    
    // Third row - sensitivity control and hard flatten
    latchArea.removeFromTop(10);
    auto latchRow3 = latchArea.removeFromTop(30);
    flattenSensitivityLabel.setBounds(latchRow3.removeFromLeft(80));
    flattenSensitivitySlider->setBounds(latchRow3.removeFromLeft(170));
    latchRow3.removeFromLeft(10);
    hardFlattenModeButton.setBounds(latchRow3);
    
    // RIGHT PANEL - Detection controls
    auto rightContent = rightPanel;
    
    // Pitch algorithm selector at the top
    rightContent.removeFromTop(5);
    auto algorithmArea = rightContent.removeFromTop(30);
    pitchAlgorithmLabel.setBounds(algorithmArea.removeFromLeft(80));
    pitchAlgorithmSelector.setBounds(algorithmArea.reduced(0, 2));
    
    rightContent.removeFromTop(10);
    
    // Pitch detection section
    if (detectionLabel)
        detectionLabel->setBounds(rightContent.removeFromTop(25));
    rightContent.removeFromTop(5);
    
    auto detectionArea = rightContent.removeFromTop(190);
    
    // Detection rate
    auto rateArea = detectionArea.removeFromTop(32);
    detectionRateLabel.setBounds(rateArea.removeFromLeft(100));
    detectionRateSlider->setBounds(rateArea);
    
    // Threshold
    auto thresholdArea = detectionArea.removeFromTop(32);
    pitchThresholdLabel.setBounds(thresholdArea.removeFromLeft(100));
    pitchThresholdSlider->setBounds(thresholdArea);
    
    // Min frequency
    auto minFreqArea = detectionArea.removeFromTop(32);
    minFreqLabel.setBounds(minFreqArea.removeFromLeft(100));
    minFreqSlider->setBounds(minFreqArea);
    
    // Max frequency
    auto maxFreqArea = detectionArea.removeFromTop(32);
    maxFreqLabel.setBounds(maxFreqArea.removeFromLeft(100));
    maxFreqSlider->setBounds(maxFreqArea);
    
    // Volume threshold
    auto volumeArea = detectionArea.removeFromTop(32);
    volumeThresholdLabel.setBounds(volumeArea.removeFromLeft(100));
    volumeThresholdSlider->setBounds(volumeArea);
    
    // Volume level display
    detectionArea.removeFromTop(5);
    volumeLevelLabel.setBounds(detectionArea.removeFromTop(25).withTrimmedLeft(100));
    
    // Advanced detection section
    rightContent.removeFromTop(10);
    if (advancedLabel)
        advancedLabel->setBounds(rightContent.removeFromTop(25));
    rightContent.removeFromTop(5);
    
    auto advancedArea = rightContent;
    
    // Check which algorithm is selected
    int algorithm = pitchAlgorithmSelector.getSelectedId() - 1; // 0 = YIN, 1 = DIO
    bool isDIO = (algorithm == 1);
    
    if (!isDIO)
    {
        // YIN-specific controls
        auto holdTimeArea = advancedArea.removeFromTop(32);
        pitchHoldTimeLabel.setBounds(holdTimeArea.removeFromLeft(100));
        pitchHoldTimeSlider->setBounds(holdTimeArea);
        
        auto jumpArea = advancedArea.removeFromTop(32);
        pitchJumpThresholdLabel.setBounds(jumpArea.removeFromLeft(100));
        pitchJumpThresholdSlider->setBounds(jumpArea);
        
        auto confidenceArea = advancedArea.removeFromTop(32);
        minConfidenceLabel.setBounds(confidenceArea.removeFromLeft(100));
        minConfidenceSlider->setBounds(confidenceArea);
        
        auto pitchSmoothArea = advancedArea.removeFromTop(32);
        pitchSmoothingLabel.setBounds(pitchSmoothArea.removeFromLeft(100));
        pitchSmoothingSlider->setBounds(pitchSmoothArea);
    }
    else
    {
        // DIO-specific controls
        auto speedArea = advancedArea.removeFromTop(32);
        dioSpeedLabel.setBounds(speedArea.removeFromLeft(100));
        dioSpeedSlider->setBounds(speedArea);
        
        auto periodArea = advancedArea.removeFromTop(32);
        dioFramePeriodLabel.setBounds(periodArea.removeFromLeft(100));
        dioFramePeriodSlider->setBounds(periodArea);
        
        auto rangeArea = advancedArea.removeFromTop(32);
        dioAllowedRangeLabel.setBounds(rangeArea.removeFromLeft(100));
        dioAllowedRangeSlider->setBounds(rangeArea);
        
        auto channelsArea = advancedArea.removeFromTop(32);
        dioChannelsLabel.setBounds(channelsArea.removeFromLeft(100));
        dioChannelsSlider->setBounds(channelsArea);
        
        auto bufferTimeArea = advancedArea.removeFromTop(32);
        dioBufferTimeLabel.setBounds(bufferTimeArea.removeFromLeft(100));
        dioBufferTimeSlider->setBounds(bufferTimeArea);
    }
    
    // Detection filters at the bottom
    advancedArea.removeFromTop(10);
    auto highpassArea = advancedArea.removeFromTop(32);
    detectionHighpassLabel.setBounds(highpassArea.removeFromLeft(100));
    detectionHighpassSlider->setBounds(highpassArea);
    
    auto lowpassArea = advancedArea.removeFromTop(32);
    detectionLowpassLabel.setBounds(lowpassArea.removeFromLeft(100));
    detectionLowpassSlider->setBounds(lowpassArea);
    
    // RubberBand section
    advancedArea.removeFromTop(20);
    auto rbHeaderArea = advancedArea.removeFromTop(25);
    auto rbLabelBounds = rbHeaderArea;
    rbExpandButton.setBounds(rbLabelBounds.removeFromLeft(25));
    rubberBandLabel->setBounds(rbLabelBounds);
    
    if (rbSectionExpanded)
    {
        auto formantArea = advancedArea.removeFromTop(32);
        rbFormantPreserveLabel.setBounds(formantArea.removeFromLeft(100));
        rbFormantPreserveButton.setBounds(formantArea.removeFromLeft(80));
        
        auto pitchModeArea = advancedArea.removeFromTop(32);
        rbPitchModeLabel.setBounds(pitchModeArea.removeFromLeft(100));
        rbPitchModeSelector.setBounds(pitchModeArea.removeFromLeft(150));
        
        auto transientsArea = advancedArea.removeFromTop(32);
        rbTransientsLabel.setBounds(transientsArea.removeFromLeft(100));
        rbTransientsSelector.setBounds(transientsArea.removeFromLeft(150));
        
        auto phaseArea = advancedArea.removeFromTop(32);
        rbPhaseLabel.setBounds(phaseArea.removeFromLeft(100));
        rbPhaseSelector.setBounds(phaseArea.removeFromLeft(150));
        
        auto windowArea = advancedArea.removeFromTop(32);
        rbWindowLabel.setBounds(windowArea.removeFromLeft(100));
        rbWindowSelector.setBounds(windowArea.removeFromLeft(150));
    }
    
    // Help text and about button at the bottom
    auto bottomArea = juce::Rectangle<int>(0, getHeight() - 20, getWidth(), 20);
    auto aboutArea = bottomArea.removeFromRight(60).reduced(2);
    aboutButton.setBounds(aboutArea);
    helpTextLabel.setBounds(bottomArea);
}

void PitchFlattenerAudioProcessorEditor::timerCallback()
{
    // Update pitch meter
    float detectedPitch = audioProcessor.getDetectedPitch();
    
    // Get the actual target frequency based on override state
    bool manualOverride = *audioProcessor.parameters.getRawParameterValue("manualOverride") > 0.5f;
    float targetPitch;
    if (manualOverride)
    {
        targetPitch = *audioProcessor.parameters.getRawParameterValue("overrideFreq");
    }
    else
    {
        targetPitch = *audioProcessor.parameters.getRawParameterValue("targetPitch");
    }
    
    pitchMeter.setFrequency(detectedPitch);
    pitchMeter.setTargetFrequency(targetPitch);
    pitchMeter.setPitchRatio(audioProcessor.getCurrentPitchRatio());
    
    // Update FFT visualization only if using DIO
    bool usingDIO = audioProcessor.isUsingDIO();
    pitchMeter.setVisualizerEnabled(usingDIO);
    
    if (usingDIO)
    {
        const int samplesNeeded = 64; // Process small chunks for real-time display
        float audioData[samplesNeeded];
        audioProcessor.getLatestAudioBlock(audioData, samplesNeeded);
        
        for (int i = 0; i < samplesNeeded; ++i)
            pitchMeter.pushAudioSample(audioData[i]);
            
        // Update the pitch meter with the actual target frequency being used
        pitchMeter.setTargetFrequency(targetPitch);
    }
    
    // Get base pitch latch info before using it
    bool isLocked = audioProcessor.isBasePitchLocked();
    float latchedPitch = audioProcessor.getLatchedBasePitch();
    
    // Update flattening target display
    bool basePitchLatchEnabled = *audioProcessor.parameters.getRawParameterValue("basePitchLatch") > 0.5f;
    float actualTargetPitch = targetPitch;
    
    // If base pitch latch is enabled and locked, show the latched pitch
    if (basePitchLatchEnabled && isLocked && latchedPitch > 0 && !manualOverride)
    {
        actualTargetPitch = latchedPitch;
    }
    
    if (actualTargetPitch > 0)
    {
        juce::String noteStr = PitchMeter::frequencyToNote(actualTargetPitch);
        basePitchValueLabel.setText(juce::String(actualTargetPitch, 1) + " Hz (" + noteStr + ")", 
                                   juce::dontSendNotification);
    }
    else
    {
        basePitchValueLabel.setText("--", juce::dontSendNotification);
    }
    
    // Update override frequency note display
    float overrideFreq = *audioProcessor.parameters.getRawParameterValue("overrideFreq");
    juce::String overrideNote = PitchMeter::frequencyToNote(overrideFreq);
    overrideFreqValueLabel.setText(overrideNote, juce::dontSendNotification);
    
    // Update base pitch latch status
    if (isLocked && latchedPitch > 0)
    {
        juce::String latchedNote = PitchMeter::frequencyToNote(latchedPitch);
        latchStatusValueLabel.setText("Locked at " + juce::String(latchedPitch, 1) + " Hz (" + latchedNote + ")", 
                                      juce::dontSendNotification);
        latchStatusValueLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
    }
    else
    {
        latchStatusValueLabel.setText("Unlocked", juce::dontSendNotification);
        latchStatusValueLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    }
    
    // Update volume level display
    float currentVolumeDb = audioProcessor.getCurrentVolumeDb();
    float volumeThreshold = *audioProcessor.parameters.getRawParameterValue("volumeThreshold");
    
    volumeLevelLabel.setText("Current: " + juce::String(currentVolumeDb, 1) + " dB", 
                            juce::dontSendNotification);
    
    // Color code based on whether we're above threshold
    if (currentVolumeDb > volumeThreshold)
    {
        volumeLevelLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
    }
    else
    {
        volumeLevelLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    }
    
    // Update status
    if (audioProcessor.isProcessing())
    {
        if (detectedPitch > 0)
        {
            statusLabel.setText("Processing - Detected: " + 
                               juce::String(detectedPitch, 1) + " Hz", 
                               juce::dontSendNotification);
            statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
        }
        else
        {
            statusLabel.setText("Processing - Waiting for pitch...", 
                               juce::dontSendNotification);
            statusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);
        }
    }
    else
    {
        statusLabel.setText("Bypassed", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    }
}

void PitchFlattenerAudioProcessorEditor::updateAlgorithmControls()
{
    int algorithm = pitchAlgorithmSelector.getSelectedId() - 1; // 0 = YIN, 1 = DIO
    bool isDIO = (algorithm == 1);
    
    // Enable/disable controls in Pitch Detection section based on algorithm
    // Detection Rate and Pitch Threshold are only used by YIN
    detectionRateSlider->slider.setEnabled(!isDIO);
    detectionRateLabel.setEnabled(!isDIO);
    pitchThresholdSlider->slider.setEnabled(!isDIO);
    pitchThresholdLabel.setEnabled(!isDIO);
    
    // Min/Max Freq and Volume Threshold are used by both
    minFreqSlider->slider.setEnabled(true);
    minFreqLabel.setEnabled(true);
    maxFreqSlider->slider.setEnabled(true);
    maxFreqLabel.setEnabled(true);
    volumeThresholdSlider->slider.setEnabled(true);
    volumeThresholdLabel.setEnabled(true);
    
    // Detection filters are used by both YIN and DIO
    detectionHighpassSlider->slider.setEnabled(true);
    detectionHighpassLabel.setEnabled(true);
    detectionLowpassSlider->slider.setEnabled(true);
    detectionLowpassLabel.setEnabled(true);
    
    // Update colors for disabled controls
    auto greyedColor = juce::Colours::grey;
    auto normalColor = juce::Colours::white;
    
    detectionRateLabel.setColour(juce::Label::textColourId, isDIO ? greyedColor : normalColor);
    pitchThresholdLabel.setColour(juce::Label::textColourId, isDIO ? greyedColor : normalColor);
    detectionHighpassLabel.setColour(juce::Label::textColourId, normalColor); // Used by both
    detectionLowpassLabel.setColour(juce::Label::textColourId, normalColor);   // Used by both
    
    // Show/hide YIN-specific controls in Advanced section
    pitchHoldTimeSlider->setVisible(!isDIO);
    pitchHoldTimeLabel.setVisible(!isDIO);
    pitchJumpThresholdSlider->setVisible(!isDIO);
    pitchJumpThresholdLabel.setVisible(!isDIO);
    minConfidenceSlider->setVisible(!isDIO);
    minConfidenceLabel.setVisible(!isDIO);
    pitchSmoothingSlider->setVisible(!isDIO);
    pitchSmoothingLabel.setVisible(!isDIO);
    
    // Show/hide DIO-specific controls
    dioSpeedSlider->setVisible(isDIO);
    dioSpeedLabel.setVisible(isDIO);
    dioFramePeriodSlider->setVisible(isDIO);
    dioFramePeriodLabel.setVisible(isDIO);
    dioAllowedRangeSlider->setVisible(isDIO);
    dioAllowedRangeLabel.setVisible(isDIO);
    dioChannelsSlider->setVisible(isDIO);
    dioChannelsLabel.setVisible(isDIO);
    dioBufferTimeSlider->setVisible(isDIO);
    dioBufferTimeLabel.setVisible(isDIO);
    
    // Update section label tooltip
    if (advancedLabel)
    {
        if (isDIO)
        {
            advancedLabel->setTooltip("Fine-tune WORLD DIO FFT-based pitch detection parameters");
        }
        else
        {
            advancedLabel->setTooltip("Fine-tune YIN autocorrelation-based pitch tracking stability");
        }
    }
    
    // Update tooltips for disabled controls
    if (isDIO)
    {
        detectionRateSlider->slider.setTooltip("Detection rate is not used by WORLD DIO algorithm");
        pitchThresholdSlider->slider.setTooltip("Pitch threshold is not used by WORLD DIO algorithm");
        detectionHighpassSlider->slider.setTooltip("Detection filters are not used by WORLD DIO algorithm");
        detectionLowpassSlider->slider.setTooltip("Detection filters are not used by WORLD DIO algorithm");
    }
    else
    {
        // Restore original tooltips for YIN
        detectionRateSlider->slider.setTooltip("How often pitch detection runs (in samples). Lower = more responsive but more CPU. 64-128 samples recommended.");
        pitchThresholdSlider->slider.setTooltip("Pitch detection confidence threshold. Lower = more sensitive but may get false detections. 0.10-0.15 recommended.");
        detectionHighpassSlider->slider.setTooltip("High-pass filter before pitch detection. Helps remove low frequency noise and improve detection.");
        detectionLowpassSlider->slider.setTooltip("Low-pass filter before pitch detection. Reduces high frequency noise for cleaner detection.");
    }
    
    // Trigger layout update
    resized();
}

void PitchFlattenerAudioProcessorEditor::mouseEnter(const juce::MouseEvent& event)
{
    auto* source = event.eventComponent;
    
    // Check which component the mouse is over and update help text
    if (source == &targetPitchSlider->slider)
        helpTextLabel.setText("Target Pitch: Set the target frequency for pitch flattening", juce::dontSendNotification);
    else if (source == &smoothingTimeSlider->slider)
        helpTextLabel.setText("Smoothing Time: Controls how quickly pitch changes are applied", juce::dontSendNotification);
    else if (source == &mixSlider->slider)
        helpTextLabel.setText("Mix: Blend between dry and processed signal", juce::dontSendNotification);
    else if (source == &lookaheadSlider->slider)
        helpTextLabel.setText("Lookahead: Buffer lookahead for smoother processing", juce::dontSendNotification);
    else if (source == &manualOverrideButton)
        helpTextLabel.setText("Manual Override: Use a fixed frequency instead of auto-detected pitch", juce::dontSendNotification);
    else if (source == &overrideFreqSlider->slider)
        helpTextLabel.setText("Override Frequency: Manual frequency to flatten to when override is enabled", juce::dontSendNotification);
    else if (source == &basePitchLatchButton)
        helpTextLabel.setText("Base Pitch Latch: Lock onto the first stable pitch detected", juce::dontSendNotification);
    else if (source == &flattenSensitivitySlider->slider)
        helpTextLabel.setText("Sensitivity: How aggressively to flatten pitch variations", juce::dontSendNotification);
    else if (source == &hardFlattenModeButton)
        helpTextLabel.setText("Freeze Ratio: Lock the pitch ratio to completely flatten pitch variations", juce::dontSendNotification);
    else if (source == &pitchAlgorithmSelector)
        helpTextLabel.setText("Algorithm: YIN for clean signals, WORLD DIO for noisy recordings", juce::dontSendNotification);
    else if (source == &detectionRateSlider->slider)
        helpTextLabel.setText("Detection Rate: How often pitch is analyzed (lower = more CPU)", juce::dontSendNotification);
    else if (source == &pitchThresholdSlider->slider)
        helpTextLabel.setText("Threshold: Confidence threshold for pitch detection", juce::dontSendNotification);
    else if (source == &minFreqSlider->slider)
        helpTextLabel.setText("Min Frequency: Lowest frequency to detect", juce::dontSendNotification);
    else if (source == &maxFreqSlider->slider)
        helpTextLabel.setText("Max Frequency: Highest frequency to detect", juce::dontSendNotification);
    else if (source == &volumeThresholdSlider->slider)
        helpTextLabel.setText("Volume Gate: Minimum level for pitch detection", juce::dontSendNotification);
    else if (source == &pitchHoldTimeSlider->slider)
        helpTextLabel.setText("Pitch Hold Time: Time to hold pitch before accepting new detection", juce::dontSendNotification);
    else if (source == &pitchJumpThresholdSlider->slider)
        helpTextLabel.setText("Jump Threshold: Maximum allowed pitch change in Hz", juce::dontSendNotification);
    else if (source == &minConfidenceSlider->slider)
        helpTextLabel.setText("Min Confidence: Minimum detection confidence required", juce::dontSendNotification);
    else if (source == &pitchSmoothingSlider->slider)
        helpTextLabel.setText("Pitch Smoothing: Smooths pitch detection results", juce::dontSendNotification);
    else if (source == &detectionHighpassSlider->slider)
        helpTextLabel.setText("Detection HP: High-pass filter for pitch detection signal", juce::dontSendNotification);
    else if (source == &detectionLowpassSlider->slider)
        helpTextLabel.setText("Detection LP: Low-pass filter for pitch detection signal", juce::dontSendNotification);
    else if (source == &dioSpeedSlider->slider)
        helpTextLabel.setText("DIO Speed: Speed vs accuracy trade-off for WORLD DIO", juce::dontSendNotification);
    else if (source == &dioFramePeriodSlider->slider)
        helpTextLabel.setText("Frame Period: Analysis frame period in ms", juce::dontSendNotification);
    else if (source == &dioAllowedRangeSlider->slider)
        helpTextLabel.setText("Allowed Range: Pitch detection range multiplier", juce::dontSendNotification);
    else if (source == &dioChannelsSlider->slider)
        helpTextLabel.setText("Channels/Oct: Frequency resolution", juce::dontSendNotification);
    else if (source == &dioBufferTimeSlider->slider)
        helpTextLabel.setText("Buffer Time: Extra buffering for DIO algorithm", juce::dontSendNotification);
    else if (source == &rbFormantPreserveButton)
        helpTextLabel.setText("Formant Preserve: Maintain voice characteristics during pitch shifting", juce::dontSendNotification);
    else if (source == &rbPitchModeSelector)
        helpTextLabel.setText("Pitch Mode: Speed vs Quality tradeoff for pitch shifting", juce::dontSendNotification);
    else if (source == &rbTransientsSelector)
        helpTextLabel.setText("Transients: How to handle percussive sounds", juce::dontSendNotification);
    else if (source == &rbPhaseSelector)
        helpTextLabel.setText("Phase: Channel processing mode (Laminar keeps stereo image)", juce::dontSendNotification);
    else if (source == &rbWindowSelector)
        helpTextLabel.setText("Window: Analysis window size (affects frequency/time resolution)", juce::dontSendNotification);
}

void PitchFlattenerAudioProcessorEditor::mouseExit(const juce::MouseEvent& event)
{
    helpTextLabel.setText("Hover over controls for help", juce::dontSendNotification);
}

//==============================================================================
// AboutWindow implementation
AboutWindow::AboutWindow()
    : DocumentWindow("About SammyJs Pitch Flattener", 
                     juce::Colours::darkgrey, 
                     DocumentWindow::closeButton)
{
    content = std::make_unique<AboutContent>();
    setContentOwned(content.get(), false);
    
    centreWithSize(500, 650);
    setVisible(true);
    setResizable(false, false);
    setAlwaysOnTop(true);
    toFront(true);
}

AboutWindow::~AboutWindow()
{
}

void AboutWindow::closeButtonPressed()
{
    setVisible(false);
}

//==============================================================================
// AboutContent implementation
AboutWindow::AboutContent::AboutContent()
{
    titleLabel.setText("SammyJs Pitch Flattener", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(24.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel);
    
    juce::String versionText = "Version " PLUGIN_VERSION;
    versionLabel.setText(versionText, juce::dontSendNotification);
    versionLabel.setFont(juce::Font(16.0f));
    versionLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(versionLabel);
    
    authorLabel.setText("Created by Samuel Justice", juce::dontSendNotification);
    authorLabel.setFont(juce::Font(14.0f));
    authorLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(authorLabel);
    
    emailButton.setButtonText("sam@sweetjusticesound.com");
    emailButton.setURL(juce::URL("mailto:sam@sweetjusticesound.com"));
    emailButton.setFont(juce::Font(14.0f), false);
    addAndMakeVisible(emailButton);
    
    websiteButton.setButtonText("www.sweetjusticesound.com");
    websiteButton.setURL(juce::URL("https://www.sweetjusticesound.com"));
    websiteButton.setFont(juce::Font(14.0f), false);
    addAndMakeVisible(websiteButton);
    
    checkUpdateButton.onClick = [this] { checkForUpdates(); };
    addAndMakeVisible(checkUpdateButton);
    
    updateStatusLabel.setText("", juce::dontSendNotification);
    updateStatusLabel.setJustificationType(juce::Justification::centred);
    updateStatusLabel.setFont(juce::Font(12.0f));
    addAndMakeVisible(updateStatusLabel);
    
    licenseInfo.setMultiLine(true);
    licenseInfo.setReadOnly(true);
    licenseInfo.setScrollbarsShown(true);
    licenseInfo.setCaretVisible(false);
    licenseInfo.setColour(juce::TextEditor::backgroundColourId, juce::Colours::darkgrey.darker());
    licenseInfo.setColour(juce::TextEditor::textColourId, juce::Colours::lightgrey);
    licenseInfo.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    
    juce::String licenseText = 
        "Technologies Used:\n\n"
        "JUCE Framework\n"
        "Copyright (c) 2022 - Raw Material Software Limited\n"
        "Licensed under the GPL/Commercial license\n\n"
        
        "Rubber Band Library\n"
        "Copyright (c) 2007-2024 Particular Programs Ltd.\n"
        "Licensed under the GPL/Commercial license\n\n"
        
        "WORLD Vocoder (DIO)\n"
        "Copyright (c) 2017 Masanori Morise\n"
        "Licensed under the modified BSD license\n\n"
        
        "YIN Pitch Detection\n"
        "Based on: De Cheveigné, A., & Kawahara, H. (2002)\n"
        "YIN, a fundamental frequency estimator for speech and music\n"
        "The Journal of the Acoustical Society of America, 111(4), 1917-1930";
    
    licenseInfo.setText(licenseText);
    addAndMakeVisible(licenseInfo);
}

void AboutWindow::AboutContent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::darkgrey);
}

void AboutWindow::AboutContent::resized()
{
    auto area = getLocalBounds().reduced(20);
    
    titleLabel.setBounds(area.removeFromTop(40));
    versionLabel.setBounds(area.removeFromTop(30));
    area.removeFromTop(10);
    
    authorLabel.setBounds(area.removeFromTop(25));
    emailButton.setBounds(area.removeFromTop(25).withSizeKeepingCentre(250, 25));
    websiteButton.setBounds(area.removeFromTop(25).withSizeKeepingCentre(250, 25));
    area.removeFromTop(20);
    
    checkUpdateButton.setBounds(area.removeFromTop(30).withSizeKeepingCentre(150, 30));
    updateStatusLabel.setBounds(area.removeFromTop(25));
    area.removeFromTop(20);
    
    licenseInfo.setBounds(area);
}

void AboutWindow::AboutContent::checkForUpdates()
{
    updateStatusLabel.setText("Checking for updates...", juce::dontSendNotification);
    updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);
    
    // Create URL for GitHub API to check latest release with pitchflattener tag
    juce::URL apiUrl("https://api.github.com/repos/samueljustice/plugins/releases");
    
    // Use a thread to download the data
    juce::Thread::launch([this, apiUrl]
    {
        auto stream = apiUrl.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                               .withConnectionTimeoutMs(5000));
        
        if (!stream)
        {
            juce::MessageManager::callAsync([this]
            {
                updateStatusLabel.setText("Failed to check for updates", juce::dontSendNotification);
                updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::red);
            });
            return;
        }
        
        auto content = stream->readEntireStreamAsString();
        auto releases = juce::JSON::parse(content);
        
        if (auto* releasesArray = releases.getArray())
        {
            juce::String latestVersion;
            
            // Look for releases with pitchflattener tag
            for (const auto& release : *releasesArray)
            {
                if (auto* obj = release.getDynamicObject())
                {
                    auto tagName = obj->getProperty("tag_name").toString();
                    
                    // Check if this is a pitchflattener release
                    if (tagName.startsWith("pitchflattener-v"))
                    {
                        latestVersion = tagName.substring(16); // Skip "pitchflattener-v"
                        break; // Found the latest pitchflattener release
                    }
                }
            }
            
            if (latestVersion.isNotEmpty())
            {
                auto currentVersion = juce::String(PLUGIN_VERSION);
                
                juce::MessageManager::callAsync([this, latestVersion, currentVersion]
                {
                    // Simple version comparison
                    if (latestVersion > currentVersion)
                    {
                        updateStatusLabel.setText("New version " + latestVersion + " available!", 
                                                juce::dontSendNotification);
                        updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
                    }
                    else
                    {
                        updateStatusLabel.setText("You have the latest version", 
                                                juce::dontSendNotification);
                        updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
                    }
                });
            }
            else
            {
                juce::MessageManager::callAsync([this]
                {
                    updateStatusLabel.setText("No releases found", juce::dontSendNotification);
                    updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
                });
            }
        }
    });
}
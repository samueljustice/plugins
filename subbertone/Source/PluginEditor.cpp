#include "PluginEditor.h"

#include "AboutWindow.h"
#include "PluginProcessor.h"

//-----------------------------------------------------------------
// SubbertoneAudioProcessorEditor
//-----------------------------------------------------------------
SubbertoneAudioProcessorEditor::SubbertoneAudioProcessorEditor(SubbertoneAudioProcessor& audioProcessor)
    : AudioProcessorEditor(&audioProcessor)
    , m_audioProcessor(audioProcessor)
    , m_waveformVisualizer(audioProcessor)
    , m_tooltipWindow(this, 700)
    , m_mixAttachment(m_audioProcessor.m_parameters, "mix", m_mixSlider)
    , m_distortionAttachment(m_audioProcessor.m_parameters, "distortion", m_distortionSlider)
    , m_toneAttachment(m_audioProcessor.m_parameters, "distortionTone", m_toneSlider)
    , m_postDriveLowpassAttachment(m_audioProcessor.m_parameters, "postDriveLowpass", m_postDriveLowpassSlider)
    , m_outputGainAttachment(m_audioProcessor.m_parameters, "outputGain", m_outputGainSlider)
    , m_pitchThresholdAttachment(m_audioProcessor.m_parameters, "pitchThreshold", m_pitchThresholdSlider)
    , m_fundamentalLimitAttachment(m_audioProcessor.m_parameters, "fundamentalLimit", m_fundamentalLimitSlider)
    , m_distortionTypeAttachment(m_audioProcessor.m_parameters, "distortionType", m_distortionTypeCombo)
{
    setLookAndFeel(&m_lookAndFeel);

    addAndMakeVisible(m_waveformVisualizer);
    
    // Setup sliders
    setupRotarySlider(m_mixSlider, m_mixLabel, "Mix", "Blend between dry input signal and processed subharmonic signal (0-100%)", "%", 1);
    setupRotarySlider(m_distortionSlider, m_distortionLabel, "Distortion", "Amount of harmonic distortion applied to the subharmonic signal (0-100%)", "%", 1);
    setupRotarySlider(m_toneSlider, m_toneLabel, "Tone Filter", "Low-pass filter frequency for shaping the harmonic content before mixing (20Hz-20kHz)", " Hz", 0);
    setupRotarySlider(m_postDriveLowpassSlider, m_postDriveLowpassLabel, "Lowpass", "Post-drive low-pass filter - removes upper harmonics created by distortion (20Hz-20kHz)", " Hz", 0);
    setupRotarySlider(m_outputGainSlider, m_outputGainLabel, "Output", "Final output gain control (-24dB to +24dB)", " dB", 1);
    setupRotarySlider(m_pitchThresholdSlider, m_pitchThresholdLabel, "Pitch Threshold", "Threshold for pitch detection (-60dB to -20dB). Lower values detect quieter signals but may be less accurate", " dB", 1);
    setupRotarySlider(m_fundamentalLimitSlider, m_fundamentalLimitLabel, "Max Freq", "Maximum fundamental frequency to process (100Hz to 800Hz). Frequencies above this limit will be ignored", " Hz", 0);
   
    
    // Setup distortion type combo
    m_distortionTypeCombo.addItem("Tape Saturation", 1);
    m_distortionTypeCombo.addItem("Valve Warmth", 2);
    m_distortionTypeCombo.addItem("Console Drive", 3);
    m_distortionTypeCombo.addItem("Transformer", 4);
    m_distortionTypeCombo.setTooltip("Harmonic character:\n"
                                  "- Tape Saturation: Smooth, musical compression\n"
                                  "- Valve Warmth: Tube-style even harmonics\n"
                                  "- Console Drive: Preamp-style soft clipping\n"
                                  "- Transformer: Gentle S-curve saturation");

    if (const std::atomic<float>* const distortionTypeParam =
            m_audioProcessor.m_parameters.getRawParameterValue("distortionType"))
    {
        const int restoredIndex = juce::jlimit(0, m_distortionTypeCombo.getNumItems() - 1,
                                               static_cast<int>(distortionTypeParam->load()));
        m_distortionTypeCombo.setSelectedItemIndex(restoredIndex, juce::dontSendNotification);
    }

    addAndMakeVisible(m_distortionTypeCombo);
    
    m_distortionTypeLabel.setText("Distortion Type", juce::dontSendNotification);
    m_distortionTypeLabel.setJustificationType(juce::Justification::centred);
    m_distortionTypeLabel.attachToComponent(&m_distortionTypeCombo, false);
    addAndMakeVisible(m_distortionTypeLabel);
    
    // Setup about button
    m_aboutButton.setButtonText("?");
    m_aboutButton.setTooltip("About SammyJs Subbertone - Version info and help");
    m_aboutButton.onClick = [this] { showAboutWindow(); };
    addAndMakeVisible(m_aboutButton);
    
    m_pitchThresholdLabel.setMinimumHorizontalScale(0.85f);

    setSize(900, 550);
    
    // Start timer for signal level updates
    startTimer(50); // Update every 50ms
}

SubbertoneAudioProcessorEditor::~SubbertoneAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

void SubbertoneAudioProcessorEditor::layoutTopBar(juce::Rectangle<int>& bounds)
{
    m_aboutButton.setBounds(bounds.getWidth() - 40, 10, 30, 30);
}

void SubbertoneAudioProcessorEditor::layoutVisualizer(juce::Rectangle<int>& bounds)
{
    bounds.removeFromTop(65); // Title, subtitle and checkbox space
    const juce::Rectangle<int> visualizerBounds = bounds.removeFromTop(bounds.getHeight() - 180);
    m_waveformVisualizer.setBounds(visualizerBounds);
}

void SubbertoneAudioProcessorEditor::layoutControls(juce::Rectangle<int>& bounds)
{
    constexpr int knobsTopInset = 5;
    constexpr int topRowHeight = 100;
    constexpr int secondRowTopInset = 15;

    constexpr int pairGap = 2;
    constexpr int middleGap = 22;
    constexpr int totalGap = (2 * pairGap) + (2 * middleGap);
    constexpr int maxSlotWidth = 100;
    constexpr int sliderPaddingX = 0;
    constexpr int sliderPaddingY = 4;
    constexpr int edgeInset = 0;

    juce::Rectangle<int> rowsArea = bounds.withTrimmedTop(knobsTopInset);
    const juce::Rectangle<int> topRow = rowsArea.removeFromTop(topRowHeight);

    const int slotWidth = std::min(maxSlotWidth, (topRow.getWidth() - totalGap) / 7);

    const auto placeSlider = [&](juce::Slider& slider, int sliderX)
    {
        slider.setBounds(sliderX + sliderPaddingX,
                         topRow.getY() + sliderPaddingY,
                         slotWidth - (2 * sliderPaddingX),
                         topRow.getHeight() - (2 * sliderPaddingY));
    };

    const int leftStart        = topRow.getX() + edgeInset;
    const int rightStart       = topRow.getRight() - edgeInset - ((2 * slotWidth) + pairGap);
    const int middleTotalWidth = (3 * slotWidth) + (2 * middleGap);
    const int middleStart      = topRow.getCentreX() - (middleTotalWidth / 2);

    placeSlider(m_pitchThresholdSlider, leftStart);
    placeSlider(m_fundamentalLimitSlider, leftStart + slotWidth + pairGap);

    placeSlider(m_toneSlider, middleStart);
    placeSlider(m_distortionSlider, middleStart + slotWidth + middleGap);
    placeSlider(m_postDriveLowpassSlider, middleStart + (2 * (slotWidth + middleGap)));

    placeSlider(m_mixSlider, rightStart);
    placeSlider(m_outputGainSlider, rightStart + slotWidth + pairGap);

    juce::Rectangle<int> comboRow = rowsArea.withTrimmedTop(secondRowTopInset);

    constexpr int comboWidth = 130;
    constexpr int comboHeight = 24;
    m_distortionTypeCombo.setBounds(comboRow.removeFromTop(comboHeight).withSizeKeepingCentre(comboWidth, comboHeight));
}

void SubbertoneAudioProcessorEditor::resized()
{
    juce::Rectangle<int> bounds = getLocalBounds();

    layoutTopBar(bounds);
    layoutVisualizer(bounds);

    juce::Rectangle<int> controlBounds = bounds.reduced(20, 10).withTrimmedTop(10);
    layoutControls(controlBounds);
}

void SubbertoneAudioProcessorEditor::setupRotarySlider(juce::Slider& slider, juce::Label& label, const juce::String& text, const juce::String& tooltip, const juce::String& suffix, int decimalPlaces)
{
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 80, 18);
    slider.setTooltip(tooltip);
    slider.setTextValueSuffix(suffix);
    slider.setNumDecimalPlacesToDisplay(decimalPlaces);
    addAndMakeVisible(slider);

    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centredBottom);
    label.setMinimumHorizontalScale(1.0f);
    label.setBorderSize(juce::BorderSize<int>(0));
    label.attachToComponent(&slider, false);
    addAndMakeVisible(label);
}

void SubbertoneAudioProcessorEditor::showAboutWindow()
{
    AboutWindow* const aboutWindow = new AboutWindow();
    aboutWindow->setVisible(true);
}

void SubbertoneAudioProcessorEditor::paint(juce::Graphics& g)
{
    // Dark PS1-style background
    g.fillAll(juce::Colour(0xff0a0a0a));
    
    // Title
    g.setColour(juce::Colour(0xff00ffff));
    g.setFont(juce::Font(juce::FontOptions("Courier New", 24.0f, juce::Font::bold)));
    g.drawText("SAMMYJS SUBBERTONE", getLocalBounds().removeFromTop(40), juce::Justification::centred);
    
    // Subtitle describing the unique inverse mix feature
    g.setColour(juce::Colour(0xff00ffff).withAlpha(0.7f));
    g.setFont(juce::Font(juce::FontOptions("Courier New", 12.0f, juce::Font::plain)));
    g.drawText("Subtracts clean signal from distorted to isolate pure harmonic artifacts", getLocalBounds().removeFromTop(55).removeFromBottom(15), juce::Justification::centred);
    
    // Top bar separator line (1px above child boundary so it remains visible)
    constexpr int topBarHeight = 65;
    g.setColour(juce::Colour(0xff1a3a3a));
    g.fillRect(0, topBarHeight - 2, getWidth(), 2);

    // Control panel background
    const juce::Rectangle<int> controlBounds = getLocalBounds().removeFromBottom(180);
    g.setColour(juce::Colour(0xff0f0f0f));
    g.fillRect(controlBounds);
    
    g.setColour(juce::Colour(0xff1a3a3a));
    g.drawRect(controlBounds, 2);
}

void SubbertoneAudioProcessorEditor::timerCallback()
{
    // Update signal level display
    const float signalDb  = 20.0f * std::log10(m_audioProcessor.getCurrentSignalLevel());
    const float threshold = static_cast<float>(m_pitchThresholdSlider.getValue());
    
    const juce::String levelText = (signalDb <= -160.0f)
                                    ? juce::CharPointer_UTF8("Signal (RMS): -inf dB")
                                    : juce::String::formatted("Signal (RMS): %.1f dB", signalDb);
    
    m_waveformVisualizer.setSignalText(levelText, signalDb > threshold);
}


//-----------------------------------------------------------------
// SubbertoneLookAndFeel
//-----------------------------------------------------------------
SubbertoneAudioProcessorEditor::SubbertoneLookAndFeel::SubbertoneLookAndFeel()
{
    // PS1 Wipeout inspired colors
    setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff00ffff));
    setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff1a3a3a));
    setColour(juce::Slider::thumbColourId, juce::Colour(0xffff00ff));
    setColour(juce::Label::textColourId, juce::Colour(0xffffffff));
}

void SubbertoneAudioProcessorEditor::SubbertoneLookAndFeel::drawRotarySlider(juce::Graphics& g,
                                                                             int x,
                                                                             int y,
                                                                             int width,
                                                                             int height,
                                                                             float sliderPos,
                                                                             float rotaryStartAngle,
                                                                             float rotaryEndAngle,
                                                                             juce::Slider& slider)
{
    const juce::Rectangle<float> bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(20.0f).translated(0.0f, -10.0f);

    const float radius = std::min(bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const float toAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    const float lineW = std::min(6.0f, radius * 0.42f);
    const float arcRadius = radius - lineW * 0.5f;

    // Background arc
    juce::Path backgroundArc;
    backgroundArc.addCentredArc(bounds.getCentreX(), bounds.getCentreY(), arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);

    g.setColour(juce::Colour(0xff1a3a3a));
    g.strokePath(backgroundArc, juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (slider.isEnabled())
    {
        // Value arc
        juce::Path valueArc;
        valueArc.addCentredArc(bounds.getCentreX(), bounds.getCentreY(), arcRadius, arcRadius, 0.0f, rotaryStartAngle, toAngle, true);

        g.setColour(juce::Colour(0xff00ffff));
        g.strokePath(valueArc, juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Center indicator
    const float thumbWidth = lineW * 2.0f;

    const juce::Point<float> thumbPoint(bounds.getCentreX() + arcRadius * std::cos(toAngle - juce::MathConstants<float>::halfPi),
        bounds.getCentreY() + arcRadius * std::sin(toAngle - juce::MathConstants<float>::halfPi));

    g.setColour(juce::Colour(0xffff00ff));
    g.fillEllipse(juce::Rectangle<float>(thumbWidth, thumbWidth).withCentre(thumbPoint));
}

juce::Slider::SliderLayout SubbertoneAudioProcessorEditor::SubbertoneLookAndFeel::getSliderLayout(juce::Slider& slider)
{
    juce::Slider::SliderLayout layout = juce::LookAndFeel_V4::getSliderLayout(slider);

    if (slider.getSliderStyle() == juce::Slider::RotaryVerticalDrag)
    {
        // Pull value box slightly closer to the rotary graphic.
        layout.textBoxBounds = layout.textBoxBounds.translated(0, -20);
    }

    return layout;
}


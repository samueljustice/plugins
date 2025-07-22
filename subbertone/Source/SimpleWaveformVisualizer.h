#pragma once

#include <JuceHeader.h>

class SubbertoneAudioProcessor;

class SimpleWaveformVisualizer : public juce::Component,
                                 public juce::Timer
{
public:
    SimpleWaveformVisualizer(SubbertoneAudioProcessor& processor);
    ~SimpleWaveformVisualizer() override;
    
    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;
    
private:
    SubbertoneAudioProcessor& audioProcessor;
    
    std::vector<float> inputWaveform;
    std::vector<float> outputWaveform;
    
    const juce::Colour bgColor{ 0xff000510 };
    const juce::Colour inputColor{ 0xff00ffff };
    const juce::Colour outputColor{ 0xffff00ff };
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SimpleWaveformVisualizer)
};
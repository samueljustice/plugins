#pragma once

#include <JuceHeader.h>

class FFTVisualizer : public juce::Component, public juce::Timer
{
public:
    FFTVisualizer();
    ~FFTVisualizer() override;
    
    void paint(juce::Graphics& g) override;
    void timerCallback() override;
    
    void pushSample(float sample);
    void setStrongestFrequency(float freq) { strongestFrequency = freq; }
    
    static constexpr int fftOrder = 11;
    static constexpr int fftSize = 1 << fftOrder; // 2048
    static constexpr int scopeSize = 512;
    
private:
    juce::dsp::FFT forwardFFT;
    juce::dsp::WindowingFunction<float> window;
    
    float fifo[fftSize];
    float fftData[2 * fftSize];
    float scopeData[scopeSize];
    int fifoIndex = 0;
    bool nextFFTBlockReady = false;
    
    float strongestFrequency = 0.0f;
    
    void pushNextSampleIntoFifo(float sample);
    void drawNextFrameOfSpectrum();
    void drawFrame(juce::Graphics& g);
    
    float skewedProportionToX(float proportion, float skewFactor) const;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FFTVisualizer)
};
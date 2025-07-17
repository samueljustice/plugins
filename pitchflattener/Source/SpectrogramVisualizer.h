#pragma once

#include <JuceHeader.h>
#include <deque>

class SpectrogramVisualizer : public juce::Component, public juce::Timer
{
public:
    SpectrogramVisualizer();
    ~SpectrogramVisualizer() override;
    
    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;
    
    void pushSample(float sample);
    void setDetectedFrequency(float freq) { detectedFrequency = freq; }
    void setProcessedFrequency(float freq) { processedFrequency = freq; }
    
    static constexpr int fftOrder = 10; // 1024 samples for better frequency resolution
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int frequencyBins = fftSize / 2;
    
private:
    juce::dsp::FFT forwardFFT;
    juce::dsp::WindowingFunction<float> window;
    
    float fifo[fftSize];
    float fftData[2 * fftSize];
    int fifoIndex = 0;
    bool nextFFTBlockReady = false;
    
    // Pitch trail data
    static constexpr int trailLength = 512;
    struct TrailPoint {
        float frequency = 0.0f;
        float intensity = 1.0f;
        float age = 0.0f;
    };
    std::deque<TrailPoint> detectedPitchTrail;
    std::deque<TrailPoint> processedPitchTrail;
    
    // Spectrogram data for background
    static constexpr int spectrogramWidth = 512;
    static constexpr int spectrogramHeight = 256;
    std::deque<std::vector<float>> spectrogramData;
    
    float detectedFrequency = 0.0f;
    float processedFrequency = 0.0f;
    float sampleRate = 48000.0f;
    
    // Piano keyboard mapping and view settings
    float minFreq = 80.0f;   // E2
    float maxFreq = 2000.0f; // ~B6
    float viewMinFreq = 80.0f;   // Current view minimum frequency
    float viewMaxFreq = 2000.0f; // Current view maximum frequency
    float targetViewMin = 80.0f;  // Target for smooth scrolling
    float targetViewMax = 2000.0f;
    
    void pushNextSampleIntoFifo(float sample);
    void drawNextFrameOfSpectrum();
    void drawPianoKeys(juce::Graphics& g);
    void drawSpectrogram(juce::Graphics& g);
    void drawPitchTrail(juce::Graphics& g, const std::deque<TrailPoint>& trail, juce::Colour baseColour);
    void updatePitchTrails();
    void drawLegend(juce::Graphics& g);
    void updateViewRange();
    
    float frequencyToY(float frequency) const;
    float yToFrequency(float y) const;
    int frequencyToMidiNote(float frequency) const;
    juce::String getMidiNoteName(int midiNote) const;
    juce::Colour getHeatmapColour(float value) const;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrogramVisualizer)
};
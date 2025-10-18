#pragma once

#include <JuceHeader.h>
#include <vector>

class PitchDetector
{
public:
    PitchDetector();
    ~PitchDetector() = default;
    
    void prepare(double sampleRate, int maxBlockSize);
    float detectPitch(const float* inputBuffer, int numSamples, float threshold = 0.1f);
    
private:
    double sampleRate = 44100.0;
    int windowSize = 2048;
    
    // Buffers
    std::vector<float> windowBuffer;
    std::vector<float> autocorrelation;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> window;
    
    // Pitch smoothing
    float previousPitch = 0.0f;
    float smoothedPitch = 0.0f;
    juce::dsp::IIR::Filter<float> preFilter;
    static constexpr float pitchSmoothingFactor = 0.9f;
    
    // Detection parameters
    static constexpr float minFrequency = 50.0f;   // 50 Hz minimum
    static constexpr float maxFrequency = 800.0f;  // 800 Hz maximum
    
    // Autocorrelation-based pitch detection
    float detectPitchAutocorrelation(const float* buffer, int bufferSize);
    
    
    // Helper functions
    void calculateAutocorrelation(const float* buffer, int bufferSize);
    int findPeakInRange(int minLag, int maxLag);
    float refineWithParabolicInterpolation(int peakIndex);
    float normalizeAutocorrelation(int lag);
};

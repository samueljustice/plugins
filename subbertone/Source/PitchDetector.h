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
    void setDetectionMethod(bool useYIN) { currentMethod = useYIN ? DetectionMethod::YIN : DetectionMethod::Autocorrelation; }
    
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
    
    // YIN Pitch-Detection (better for subharmonics)
    std::vector<float> yin_buffer;
    static constexpr float yinThreshold = 0.1f;
    enum class DetectionMethod { Autocorrelation, YIN };
    DetectionMethod currentMethod = DetectionMethod::YIN;
    
    // Helper functions
    void calculateAutocorrelation(const float* buffer, int bufferSize);
    int findPeakInRange(int minLag, int maxLag);
    float refineWithParabolicInterpolation(int peakIndex);
    float normalizeAutocorrelation(int lag);
    
    // YIN Helper functions
    float detectPitchYIN(const float* buffer, int bufferSize);
    void calculateDifferenceFunction(const float* buffer, int bufferSize);
    void calculateCumulativeMeanNormalizedDifference();
    int findAbsoluteMinimum(int minLag, int maxLag);
    int findBestLocalMinimum(int minLag, int maxLag);
};

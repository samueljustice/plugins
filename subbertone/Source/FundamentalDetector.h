#pragma once

#include <JuceHeader.h>
#include <memory>
#include <vector>

class FundamentalDetector
{
public:
    FundamentalDetector();
    ~FundamentalDetector();
    
    void prepare(double sampleRate, int maxBlockSize);
    float detectFundamental(const float* inputBuffer, int numSamples, float thresholdDb = -40.0f);
    
private:
    double sampleRate = 44100.0;
    int frameLength = 2048;
    
    // Buffers for processing
    std::vector<float> processBuffer;
    std::vector<double> accumulator;  // Accumulator for samples
    
    // Yin algorithm buffers
    std::vector<float> yinBuffer;
    
    // Smoothing
    float lastFundamental = 0.0f;
    static constexpr float smoothingFactor = 0.98f; // Increased for more stable tracking
    
    // Add frame counter to only update fundamental periodically
    int framesSinceLastUpdate = 0;
    static constexpr int updateInterval = 4; // Only update every 4 buffers
    float stableFundamental = 0.0f; // The stable fundamental we output
    
    // Yin algorithm parameters
    static constexpr float yinThreshold = 0.15f;  // Threshold for Yin algorithm
    static constexpr float minFrequency = 40.0f;  // 40 Hz minimum
    static constexpr float maxFrequency = 1000.0f; // 1000 Hz maximum
    
    // Yin pitch detection algorithm
    float detectPitchYin(const float* buffer, int bufferSize);
    
    // Helper functions for Yin
    void differenceFunction(const float* buffer, int bufferSize, float* yinBuffer);
    void cumulativeMeanNormalizedDifferenceFunction(float* yinBuffer, int yinBufferSize);
    int absoluteThreshold(float* yinBuffer, int yinBufferSize, float threshold);
    float parabolicInterpolation(int tauEstimate, float* yinBuffer, int yinBufferSize);
};
#pragma once

#include <JuceHeader.h>
#include <vector>
#include <memory>
#include <mutex>

class PitchDetector
{
public:
    enum class Algorithm
    {
        YIN = 0,
        WORLD_DIO = 1
    };
    
    PitchDetector();
    ~PitchDetector();
    
    void prepare(double sampleRate);
    float detectPitch(const float* buffer, int numSamples);
    
    void setThreshold(float threshold) { yinThreshold = threshold; }
    void setFrequencyBounds(float minFreq, float maxFreq);
    void setAlgorithm(Algorithm algo);
    void resetDIOState();
    
    // DIO-specific parameter setters
    void setDIOSpeed(int speed);
    void setDIOFramePeriod(float framePeriod);
    void setDIOAllowedRange(float allowedRange);
    void setDIOChannelsInOctave(float channels);
    void setDIOBufferTime(float bufferTime);
    
    // Getters for debug
    bool isDIOBufferFilled() const { return dioBufferFilled; }
    int getDIOTotalSamplesReceived() const { return dioTotalSamplesReceived; }
    
private:
    mutable std::mutex dioBufferMutex;
    double sampleRate = 48000.0;
    Algorithm algorithm = Algorithm::YIN;
    
    // Yin algorithm parameters
    float yinThreshold = 0.15f;
    int minPeriod = 24;  // ~2000 Hz at 48kHz (will be updated based on actual sample rate)
    int maxPeriod = 1200; // ~40 Hz at 48kHz (will be updated based on actual sample rate)
    float minFrequency = 40.0f;
    float maxFrequency = 2000.0f;
    
    std::vector<float> yinBuffer;
    
    // Yin algorithm implementation
    void differenceFunction(const float* buffer, int numSamples, float* result, int maxLag);
    void cumulativeMeanNormalizedDifferenceFunction(float* df, int size);
    int absoluteThreshold(const float* yinBuffer, int size, float threshold);
    float parabolicInterpolation(int tauEstimate, const float* yinBuffer, int yinBufferSize);
    
    // WORLD DIO implementation
    float detectPitchWORLD(const float* buffer, int numSamples);
    void* worldOption = nullptr;  // Use void* to avoid including WORLD headers here
    std::vector<double> worldBuffer;
    std::vector<double> worldF0;
    std::vector<double> worldTimeAxis;
    int worldSamplesPerFrame = 0;
    
    // Rolling buffer for DIO
    std::vector<double> dioRollingBuffer;
    int dioBufferWritePos = 0;
    int dioBufferSize = 0;
    float dioBufferTimeSeconds = 0.5f;
    int dioSamplesAccumulated = 0;
    int dioProcessingInterval = 0;
    int dioTotalSamplesReceived = 0;
    bool dioBufferFilled = false;
};
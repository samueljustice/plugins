#pragma once

#include <JuceHeader.h>
#include <rubberband/RubberBandStretcher.h>
#include <memory>
#include <vector>

class PitchFlattenerEngine
{
public:
    PitchFlattenerEngine();
    ~PitchFlattenerEngine();
    
    void prepare(double sampleRate, int maxBlockSize);
    void reset();
    
    void setParameters(float detectedPitch, float targetPitch, float smoothing, float lookaheadMultiplier = 2.0f);
    void process(juce::AudioBuffer<float>& buffer, float mixAmount);
    
private:
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    
    std::unique_ptr<RubberBand::RubberBandStretcher> rubberBandLeft;
    std::unique_ptr<RubberBand::RubberBandStretcher> rubberBandRight;
    
    // Processing buffers
    std::vector<float> inputBufferLeft;
    std::vector<float> inputBufferRight;
    std::vector<float> outputBufferLeft;
    std::vector<float> outputBufferRight;
    
    // Pre-buffering for smoother operation
    std::vector<float> preBufferLeft;
    std::vector<float> preBufferRight;
    int preBufferSamples = 0;
    
    // Lookahead buffer for consistent feeding
    juce::AudioBuffer<float> lookaheadBuffer;
    int lookaheadSize = 0;
    float lookaheadMultiplier = 2.0f;
    int lookaheadWritePos = 0;
    int lookaheadReadPos = 0;
    
    // Latency compensation
    juce::AudioBuffer<float> delayBuffer;
    int delayBufferWritePos = 0;
    int latencyInSamples = 0;
    int framesPushed = 0;
    bool isWarmedUp = false;
    
    // Pitch shift parameters
    float currentPitchRatio = 1.0f;
    float targetPitchRatio = 1.0f;
    float lastPitchRatio = 1.0f;
    float smoothingFactor = 0.95f;
    
    // Convert input/output pointers for RubberBand
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;
    
    void updatePitchRatio(float detectedPitch, float targetPitch);
    void warmUpRubberBand();
};
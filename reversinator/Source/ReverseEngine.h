#pragma once

#include <JuceHeader.h>
#include <vector>
#include <memory>

class ReverseEngine
{
public:
    ReverseEngine();
    ~ReverseEngine();
    
    void prepare(double sampleRate, int samplesPerBlock, int numChannels);
    void reset();
    void process(juce::AudioBuffer<float>& buffer);
    
    void setParameters(float windowTimeSeconds, float feedbackAmount, float wetMix, float dryMix, int mode, float crossfadePercent = 20.0f, float envelopeSeconds = 0.03f);
    
    enum EffectMode
    {
        ReversePlayback = 0,
        ForwardBackwards,
        ReverseRepeat
    };

private:
    double sampleRate = 44100.0;
    int numChannels = 2;
    
    float windowTime = 2.0f;
    float feedback = 0.0f;
    float wetMix = 1.0f;
    float dryMix = 0.0f;
    int effectMode = ReversePlayback;
    float crossfadeTime = 0.2f; // percentage of window time for crossfade
    float envelopeTime = 0.03f; // envelope time in seconds
    
    struct Grain
    {
        std::vector<float> buffer;
        int readPosition = 0;
        int grainSize = 0;
        bool active = false;
        float amplitude = 0.0f;
    };
    
    struct Channel
    {
        // Input delay line
        std::vector<float> delayLine;
        int delayWritePos = 0;
        
        // Grain system for overlap-add
        static constexpr int NUM_GRAINS = 4;
        Grain grains[NUM_GRAINS];
        int currentGrain = 0;
        int grainCounter = 0;
        
        // Output accumulation buffer
        std::vector<float> outputBuffer;
        int outputReadPos = 0;
        int outputWritePos = 0;
        
        // Window function
        std::vector<float> windowFunction;
        
        // Parameters
        int windowSamples = 0;
        int hopSize = 0;
        
        // For other modes
        std::vector<float> repeatBuffer;
        int repeatPosition = 0;
        bool isRepeating = false;
        
        float feedbackSample = 0.0f;
        float crossfadePosition = 0.0f;
        bool crossfadeDirection = true;
        
        // Feedback parameter smoothing
        juce::SmoothedValue<float> feedbackGainSmoothed;
        
        juce::SmoothedValue<float> vibratoPhase;
        float vibratoRate = 5.0f;
        
        // For preventing feedback loops in Forward Backwards mode
        int grainSpawnOffset = 0;
    };
    
    std::vector<std::unique_ptr<Channel>> channels;
    
    void processReversePlayback(Channel& ch, float* channelData, int numSamples);
    void processForwardBackwards(Channel& ch, float* channelData, int numSamples);
    void processReverseRepeat(Channel& ch, float* channelData, int numSamples);
    
    void createWindowFunction(std::vector<float>& window, int length);
    float getVibratoModulation(Channel& ch);
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverseEngine)
};
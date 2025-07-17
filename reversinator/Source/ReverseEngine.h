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
    
    void setParameters(float windowTimeSeconds, float feedbackAmount, float wetMix, float dryMix, int mode);
    
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
    
    struct Channel
    {
        std::vector<float> circularBuffer;
        std::vector<float> fadeInBuffer;
        std::vector<float> fadeOutBuffer;
        std::vector<float> repeatBuffer;
        
        int writePosition = 0;
        int readPosition = 0;
        int windowSamples = 0;
        int fadeLength = 0;
        int repeatPosition = 0;
        bool isRepeating = false;
        
        float feedbackSample = 0.0f;
        float crossfadePosition = 0.0f;
        bool crossfadeDirection = true;
        
        juce::SmoothedValue<float> vibratoPhase;
        float vibratoRate = 5.0f;
    };
    
    std::vector<std::unique_ptr<Channel>> channels;
    
    void processReversePlayback(Channel& ch, float* channelData, int numSamples);
    void processForwardBackwards(Channel& ch, float* channelData, int numSamples);
    void processReverseRepeat(Channel& ch, float* channelData, int numSamples);
    
    void createFadeWindow(std::vector<float>& fadeIn, std::vector<float>& fadeOut, int length);
    float getVibratoModulation(Channel& ch);
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverseEngine)
};
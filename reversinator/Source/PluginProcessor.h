#pragma once

#include <JuceHeader.h>
#include "ReverseEngine.h"

class ReversinatorAudioProcessor : public juce::AudioProcessor
{
public:
    ReversinatorAudioProcessor();
    ~ReversinatorAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getValueTreeState() { return valueTreeState; }
    
    enum EffectMode
    {
        ReversePlayback = 0,
        ForwardBackwards,
        ReverseRepeat
    };

private:
    juce::AudioProcessorValueTreeState valueTreeState;
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    std::unique_ptr<ReverseEngine> reverseEngine;
    
    std::atomic<float>* reverserEnabled = nullptr;
    std::atomic<float>* windowTime = nullptr;
    std::atomic<float>* feedbackDepth = nullptr;
    std::atomic<float>* wetMix = nullptr;
    std::atomic<float>* dryMix = nullptr;
    std::atomic<float>* effectMode = nullptr;
    std::atomic<float>* crossfadeTime = nullptr;
    
    bool previousReverserState = false;
    juce::SmoothedValue<float> reverserCrossfade;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReversinatorAudioProcessor)
};
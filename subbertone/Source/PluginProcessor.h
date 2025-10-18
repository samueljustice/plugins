#pragma once

#include <JuceHeader.h>
#include "SubharmonicEngine.h"
#include "PitchDetector.h"

class SubbertoneAudioProcessor : public juce::AudioProcessor
{
public:
    SubbertoneAudioProcessor();
    ~SubbertoneAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

#ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
#endif

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Parameters
    juce::AudioProcessorValueTreeState parameters;
    
    // Get current waveform data for visualization
    std::vector<float> getInputWaveform() const;
    std::vector<float> getOutputWaveform() const;
    std::vector<float> getHarmonicResidualWaveform() const;
    float getCurrentFundamental() const { return currentFundamental.load(); }
    float getCurrentSignalLevel() const { return currentSignalLevel.load(); }
    
    //Solo
    enum class SoloMode { None, Input, Harmonics, Output };
    SoloMode getSoloMode() const;

private:
    std::unique_ptr<PitchDetector> pitchDetector;
    std::unique_ptr<SubharmonicEngine> subharmonicEngine;
    
    // Circular buffers for visualization
    static constexpr int visualBufferSize = 2048;
    std::vector<float> inputVisualBuffer;
    std::vector<float> outputVisualBuffer;
    std::vector<float> harmonicResidualVisualBuffer;
    std::atomic<int> visualBufferWritePos{ 0 };
    mutable juce::CriticalSection visualBufferLock;
    
    std::atomic<float> currentFundamental{ 0.0f };
    std::atomic<float> currentSignalLevel{ -100.0f };
    
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubbertoneAudioProcessor)
};

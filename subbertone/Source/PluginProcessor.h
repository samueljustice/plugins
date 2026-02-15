#pragma once

#include <JuceHeader.h>

#include "SubharmonicEngine.h"
#include "PitchDetector.h"

#include <array>

class SubbertoneAudioProcessor : public juce::AudioProcessor
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubbertoneAudioProcessor)

public:
    SubbertoneAudioProcessor();
    ~SubbertoneAudioProcessor() = default;

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

#ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
#endif
    void releaseResources() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // Get current waveform data for visualization
    void getInputWaveform(std::vector<float>& dest) const;
    void getOutputWaveform(std::vector<float>& dest) const;
    void getHarmonicResidualWaveform(std::vector<float>& dest) const;

    float getCurrentSignalLevel() const { return m_currentSignalLevelDb.load(); }
    float getCurrentFundamental() const { return m_currentFundamental.load(); }

    // Parameters
    juce::AudioProcessorValueTreeState m_parameters;

private:
    struct ParameterCache
    {
        float m_mix = 0.5f;
        float m_distortion = 0.5f;
        int m_distortionType = 0;
        float m_distortionTone = 1000.0f;
        float m_postDriveLowpass = 20000.0f;
        float m_outputGain = 1.0f;
        float m_pitchThreshold = -40.0f;
        float m_fundamentalLimit = 250.0f;
    };

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void updateParameterCache();
    void updateVisualizerBuffers(juce::AudioBuffer<float>& buffer);

    PitchDetector m_pitchDetector;
    std::vector<float> m_subharmonicBuffer;

    SubharmonicEngine m_subharmonicEngine;
    std::vector<float> m_pitchDetectBuffer;
    
    // Circular buffers for visualization
    static constexpr int c_visualBufferSize = 2048;
    static constexpr int c_maxProcessBlockSize = 8192;

    int m_currentMaxProcessBlockSize = c_maxProcessBlockSize;

    // Double buffering for visualization
    std::array<std::vector<float>, 2> m_inputVisualBuffer;
    std::array<std::vector<float>, 2> m_outputVisualBuffer;
    std::array<std::vector<float>, 2> m_harmonicResidualVisualBuffer;
    std::array<std::atomic<int>, 2> m_visualBufferWritePos{ 0, 0 };

    std::atomic<int> m_visualReadIndex{ 0 };
    std::atomic<float> m_currentFundamental{ 0.0f };
    std::atomic<float> m_currentSignalLevelDb{ -100.0f };
    
    ParameterCache m_parameterCache;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_mixSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_distortionSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_toneSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_postDriveLowpassSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_outputGainSmoothed;
};

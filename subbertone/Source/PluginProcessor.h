#pragma once

#include <JuceHeader.h>

#include "SubharmonicEngine.h"
#include "PitchDetector.h"

#include <array>
#include <atomic>
#include <vector>

class SubbertoneAudioProcessor : public juce::AudioProcessor
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubbertoneAudioProcessor)

public:
    SubbertoneAudioProcessor();
    ~SubbertoneAudioProcessor() override = default;

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
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    SubharmonicEngine::Parameters readEngineParameters() const;
    void pushVisualSamples(const float* input, const float* output, const float* residual, int numSamples);
    void copyVisualRing(const std::vector<float>& ring, std::vector<float>& dest) const;

    SubharmonicEngine m_subharmonicEngine;

    // Scratch buffers, sized in prepareToPlay. Blocks larger than this are processed in chunks.
    static constexpr int c_chunkSize = 512;
    std::vector<float> m_monoBuffer;
    std::vector<float> m_wetBuffer;
    std::vector<float> m_residualBuffer;

    // Raw parameter pointers (looked up once)
    std::atomic<float>* m_mixParam = nullptr;
    std::atomic<float>* m_distortionParam = nullptr;
    std::atomic<float>* m_distortionTypeParam = nullptr;
    std::atomic<float>* m_toneParam = nullptr;
    std::atomic<float>* m_postDriveLowpassParam = nullptr;
    std::atomic<float>* m_outputGainParam = nullptr;
    std::atomic<float>* m_pitchThresholdParam = nullptr;
    std::atomic<float>* m_fundamentalLimitParam = nullptr;
    std::atomic<float>* m_filterResonanceParam = nullptr;
    std::atomic<float>* m_filterModeParam = nullptr;
    std::atomic<float>* m_filterEnvAmountParam = nullptr;
    std::atomic<float>* m_filterEnvReleaseParam = nullptr;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_mixSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> m_outputGainSmoothed;

    // Visualisation: single-writer ring buffers. The reader may see a torn frame,
    // which is harmless for a display and keeps the audio thread lock-free.
    static constexpr int c_visualBufferSize = 2048;
    std::vector<float> m_inputVisualBuffer;
    std::vector<float> m_outputVisualBuffer;
    std::vector<float> m_harmonicResidualVisualBuffer;
    std::atomic<int> m_visualWritePos{ 0 };

    std::atomic<float> m_currentFundamental{ 0.0f };
    std::atomic<float> m_currentSignalLevelDb{ 0.0f }; // linear RMS despite the name (editor converts)
};

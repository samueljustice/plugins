#pragma once

#include <JuceHeader.h>

#include "SubharmonicEngine.h"
#include "PitchDetector.h"
#include "PresetManager.h"
#include "MasterProcessor.h"

#include <array>
#include <atomic>
#include <vector>

class SubbertoneAudioProcessor : public juce::AudioProcessor
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubbertoneAudioProcessor)
    JUCE_DECLARE_WEAK_REFERENCEABLE(SubbertoneAudioProcessor)

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

    /** Editor view options (e.g. visualiser trace toggles), saved with the plugin state. */
    bool getViewOption(const juce::Identifier& option, bool defaultValue) const;
    void setViewOption(const juce::Identifier& option, bool value);
    juce::var getViewValue(const juce::Identifier& option, const juce::var& defaultValue) const;
    void setViewValue(const juce::Identifier& option, const juce::var& value);

    /** Master limiter gain reduction in dB (<= 0), for the editor's meter. */
    float getGainReductionDb() const { return m_gainReductionDb.load(std::memory_order_relaxed); }

    PresetManager& getPresetManager() { return m_presetManager; }

    /** 0..1: how much of harmonic k's level is sounding right now (message thread, for meters). */
    float getHarmonicActivity(int k) const
    {
        return (k >= 1 && k <= 16) ? m_harmonicActivity[static_cast<size_t>(k)].load(std::memory_order_relaxed) : 0.0f;
    }

    // Parameters
    juce::AudioProcessorValueTreeState m_parameters;

private:
    PresetManager m_presetManager; // after m_parameters: it listens to them

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    SubharmonicEngine::Parameters readEngineParameters() const;
    void pushVisualSamples(const float* input, const float* output, const float* residual, int numSamples);
    void copyVisualRing(const std::vector<float>& ring, std::vector<float>& dest) const;

    SubharmonicEngine m_subharmonicEngine;
    MasterProcessor m_master; // final stage: inflate + look-ahead limiter (constant latency)
    MasterProcessor::Parameters readMasterParameters() const;

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
    std::atomic<float>* m_pitchGlideParam = nullptr;
    std::atomic<float>* m_overtoneAmountParam = nullptr;
    std::array<std::atomic<float>*, 17> m_harmonicParams {};
    std::atomic<float>* m_inflateEffectParam = nullptr;
    std::atomic<float>* m_inflateCurveParam = nullptr;
    std::atomic<float>* m_limiterOnParam = nullptr;
    std::atomic<float>* m_limiterCeilingParam = nullptr;
    std::atomic<float>* m_limiterReleaseParam = nullptr;
    std::atomic<float>* m_loCutOnParam = nullptr;
    std::atomic<float>* m_loCutFreqParam = nullptr;

    // Live per-harmonic activity (0..1) for the harmonic strip, written once per block.
    std::array<std::atomic<float>, 17> m_harmonicActivity {};
    std::atomic<float> m_gainReductionDb { 0.0f };

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

#pragma once

#include <JuceHeader.h>
#include "StretchEngine.h"
#include "PitchDetector.h"
#include <atomic>
#include <vector>

class StretchArmstrongAudioProcessor : public juce::AudioProcessor
{
public:
    StretchArmstrongAudioProcessor();
    ~StretchArmstrongAudioProcessor() override;

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

    // Parameter tree
    juce::AudioProcessorValueTreeState parameters;

    // Visual feedback
    float getCurrentSignalLevel() const { return currentSignalLevel.load(); }
    float getThresholdDb() const { return parameters.getRawParameterValue("threshold")->load(); }
    float getEnvelopeValue() const { return currentEnvelopeValue.load(); }
    bool isStretching() const { return stretchActive.load(); }
    float getCurrentStretchRatio() const { return currentStretchRatio.load(); }

    // Waveform data for visualization
    std::vector<float> getInputWaveform() const;
    std::vector<float> getOutputWaveform() const;

    // Latency reporting
    int getLatencySamples() const { return stretchEngine ? stretchEngine->getLatencySamples() : 0; }

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Stretch engine
    std::unique_ptr<StretchEngine> stretchEngine;

    // Threshold detection
    std::atomic<float> currentSignalLevel{-100.0f};
    std::atomic<float> currentEnvelopeValue{0.0f};
    std::atomic<bool> stretchActive{false};
    std::atomic<float> currentStretchRatio{1.0f};

    // ASR envelope state
    enum class EnvelopeState { Idle, Attack, Sustain, Release };
    EnvelopeState envelopeState = EnvelopeState::Idle;
    float envelopeValue = 0.0f;
    int sustainSamplesRemaining = 0;

    // Signal detection
    bool signalAboveThreshold = false;
    int samplesAboveThreshold = 0;
    int samplesBelowThreshold = 0;
    static constexpr int hysteresisOnSamples = 64;   // ~1.5ms at 44.1kHz
    static constexpr int hysteresisOffSamples = 2048; // ~46ms at 44.1kHz

    // Envelope follower state
    float envFollowerValue = 0.0f;
    float envFollowerAttackCoeff = 0.0f;
    float envFollowerReleaseCoeff = 0.0f;
    float slewedEnvFollower = 0.0f; // Slewed version for smooth modulation

    // Pitch follower state
    std::unique_ptr<PitchDetector> pitchDetector;
    float pitchFollowerValue = 0.0f;
    float slewedPitchFollower = 0.0f; // Slewed version for smooth modulation

    // Slewing coefficients
    float modulationSlewCoeff = 0.0f;

    // Processing state
    double currentSampleRate = 44100.0;

    // Visualization buffers
    static constexpr int visualBufferSize = 4096;
    std::vector<float> inputVisualBuffer;
    std::vector<float> outputVisualBuffer;
    std::atomic<int> visualBufferWritePos{0};
    mutable juce::CriticalSection visualBufferLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StretchArmstrongAudioProcessor)
};

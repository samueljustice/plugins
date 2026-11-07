#pragma once

#include <JuceHeader.h>
#include "ReverseEngine.h"

namespace ParamIDs
{
    // These IDs are shared with v1.x sessions; do not rename them.
    inline constexpr const char* mode     = "mode";
    inline constexpr const char* reverser = "reverser";
    inline constexpr const char* time     = "time";
    inline constexpr const char* feedback = "feedback";
    inline constexpr const char* wetMix   = "wetmix";
    inline constexpr const char* dryMix   = "drymix";
}

/**
 * Hosts the reverse engine and does what the original's host glue did:
 * the engine always runs; REVERSER chooses between its output and the
 * forwards input (both at the wet level) with a 10 ms crossfade; the dry
 * input is mixed in on top. Wet and dry use a squared (x/100)^2 law.
 */
class ReversinatorAudioProcessor : public juce::AudioProcessor
{
public:
    ReversinatorAudioProcessor();
    ~ReversinatorAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    /** Host bypass: the engine keeps running and the output crossfades to the input. */
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getParameters() { return m_parameters; }

    //==========================================================================
    // Display feed (lock-free; written on the audio thread, read on the message thread)
    struct DisplayState
    {
        std::array<ReverseEngine::VoiceView, ReverseEngine::maxVoices> voices {};
        int numVoices = 0;
        float timeSeconds = 2.0f;
        int mode = 0;
        bool reversing = true;
    };

    /** Copies the most recent `count` input peaks (one per display bin) into dest, oldest first. */
    void copyPeakHistory(float* dest, int count) const;
    double getDisplayBinSeconds() const { return m_displayBinSeconds.load(); }
    DisplayState getDisplayState() const;

    int getEditorWidth() const { return m_editorWidth.load(); }
    void setEditorWidth(int width) { m_editorWidth.store(width); }

    static constexpr double displayBinSeconds = 0.0058;   // ~256 samples at 44.1 kHz
    static constexpr int peakHistorySize = 4096;          // ~24 s of history at any rate

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void process(juce::AudioBuffer<float>& buffer, bool bypassed);
    void processChunk(float* const* channels, int numChannels, int numSamples);
    void pushDisplayPeaks(const float* const* channels, int numChannels, int numSamples);

    juce::AudioProcessorValueTreeState m_parameters;

    std::atomic<float>* m_modeParam = nullptr;
    std::atomic<float>* m_reverserParam = nullptr;
    std::atomic<float>* m_timeParam = nullptr;
    std::atomic<float>* m_feedbackParam = nullptr;
    std::atomic<float>* m_wetParam = nullptr;
    std::atomic<float>* m_dryParam = nullptr;

    ReverseEngine m_engine;
    juce::AudioBuffer<float> m_input;   // copy of the input (the engine's source and the dry path)
    juce::AudioBuffer<float> m_wet;     // engine output
    int m_maxBlock = 0;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_reverserMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_wetGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_dryGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_bypassMix;   // 1 = host bypass

    // Display feed: a ring of input peaks the editor can read at any time.
    std::array<std::atomic<float>, peakHistorySize> m_peakRing {};
    std::atomic<int> m_peakWrite { 0 };
    float m_peakAccumulator = 0.0f;
    int m_peakCount = 0;
    int m_binSamples = 256;
    std::atomic<double> m_displayBinSeconds { displayBinSeconds };
    std::array<std::atomic<float>, ReverseEngine::maxVoices * 2> m_voiceState {};
    std::atomic<int> m_numVoices { 0 };

    std::atomic<int> m_editorWidth { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReversinatorAudioProcessor)
};

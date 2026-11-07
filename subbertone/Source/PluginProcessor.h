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

    /** The factory presets, plus one last program, "User Preset", that stands for any sound
        that is not a factory preset (a user preset, an unsaved sound, a 1.1.0 session).
        Selecting it does nothing; it only exists so the host's program display is right and
        so that every factory program, "Init" included, can always be selected. */
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
    void reset() override;

    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    /** Host bypass: the input delayed by exactly getLatencySamples(), crossfaded in and out.
        The engine and MASTER keep running underneath, so leaving bypass is seamless. */
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // Get current waveform data for visualization
    void getInputWaveform(std::vector<float>& dest) const;
    void getOutputWaveform(std::vector<float>& dest) const;
    void getHarmonicResidualWaveform(std::vector<float>& dest) const;

    float getCurrentSignalLevel() const { return m_currentSignalLevelDb.load(); }
    float getCurrentFundamental() const { return m_currentFundamental.load(); }

    /** Editor view options (e.g. visualiser trace toggles, editor scale), saved with the plugin
        state. Safe to call on the message thread while the host saves or restores the state on
        another thread. */
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
    // Reports preset / state changes to the host from the message thread, coalesced.
    class HostDisplayUpdater final : private juce::AsyncUpdater
    {
    public:
        explicit HostDisplayUpdater(juce::AudioProcessor& p) : m_processor(p) {}
        ~HostDisplayUpdater() override { cancelPendingUpdate(); }

        void post(const juce::AudioProcessorListener::ChangeDetails& details);

    private:
        void handleAsyncUpdate() override;

        static constexpr int c_programChanged = 1, c_stateChanged = 2;
        juce::AudioProcessor& m_processor;
        std::atomic<int> m_flags { 0 };
    };

    HostDisplayUpdater m_hostDisplay { *this };   // before m_presetManager: it reports through this
    PresetManager m_presetManager;                // after m_parameters: it listens to them

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    SubharmonicEngine::Parameters readEngineParameters() const;
    void pushVisualSamples(const float* input, const float* output, const float* residual, int numSamples);
    void copyVisualRing(const std::vector<float>& ring, std::vector<float>& dest) const;

    void render(juce::AudioBuffer<float>& buffer, bool bypassed);
    void handlePendingReset(bool bypassed);
    void delayDry(const juce::AudioBuffer<float>& buffer, int start, int numSamples, int numInputs, int numChannels);
    void applyBypassFade(juce::AudioBuffer<float>& buffer, int start, int numSamples, int numChannels, float target);

    SubharmonicEngine m_subharmonicEngine;
    MasterProcessor m_master; // final stage: inflate + look-ahead limiter (constant latency)
    MasterProcessor::Parameters readMasterParameters() const;

    // Scratch buffers, sized in prepareToPlay. Blocks larger than this are processed in chunks.
    static constexpr int c_chunkSize = 512;
    std::vector<float> m_monoBuffer;
    std::vector<float> m_wetBuffer;
    std::vector<float> m_residualBuffer;

    // Bypass: the input through a delay of exactly the reported latency (always fed, so it is
    // warm when bypass engages), crossfaded against the processed output.
    static constexpr double c_bypassFadeSeconds = 0.015;
    std::vector<std::vector<float>> m_dryDelay;   // per channel, getLatencySamples() long
    std::vector<std::vector<float>> m_dryChunk;   // per channel, c_chunkSize: delayed input for this chunk
    std::vector<float*> m_channelPointers;        // MASTER channel pointers for the current chunk
    std::array<float, c_chunkSize> m_fadeWeights {};
    int m_dryDelayLength = 0;
    int m_dryDelayPosition = 0;
    float m_bypassFade = 0.0f;                    // 0 = processed, 1 = bypassed (delayed input)
    float m_bypassFadeStep = 1.0f;
    bool m_wasBypassed = false;
    bool m_snapBypassFade = true;                 // first block after prepareToPlay: no fade

    // AudioProcessor::reset() may come from any thread (AU Reset on the main thread while
    // rendering, VST3 setProcessing(false)); it only counts the request, and the audio thread
    // acts on new requests at the start of the next block.
    std::atomic<juce::uint32> m_resetRequests { 0 };
    juce::uint32 m_resetRequestsHandled = 0;     // audio thread

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
    std::atomic<float>* m_phaseAlignParam = nullptr;

    // Live per-harmonic activity (0..1) for the harmonic strip, written once per block.
    std::array<std::atomic<float>, 17> m_harmonicActivity {};
    std::atomic<float> m_gainReductionDb { 0.0f };

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_mixSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> m_outputGainSmoothed;

    // Editor view options. Kept out of the APVTS tree (which the host may replace on another
    // thread at any time) and written into / read from the state XML.
    mutable juce::CriticalSection m_viewLock;
    juce::NamedValueSet m_viewOptions;

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

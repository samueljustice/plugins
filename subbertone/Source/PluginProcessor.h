#pragma once

#include <JuceHeader.h>

#include "SubharmonicEngine.h"
#include "PitchDetector.h"
#include "PresetManager.h"
#include "MasterProcessor.h"
#include "LearnAudio.h"
#include "LearnStore.h"
#include "LearnWorker.h"

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

    /** The same for the deeper subs, octaves 2 (S2) or 3 (S3) below the input; 0 while the Sub Floor silences one. */
    float getSubOctaveActivity(int octaves) const
    {
        return (octaves == 2 || octaves == 3) ? m_subOctaveActivity[static_cast<size_t>(octaves - 2)].load(std::memory_order_relaxed) : 0.0f;
    }

    /** LEARN: its commands (learn, stop, forget, pick a line, gap filling, USE LEARNED), its status and what is
        learned. Any thread but the audio thread; see LearnWorker.h. */
    subbertone::learn::Learner& getLearner() { return m_learner; }
    const subbertone::learn::Learner& getLearner() const { return m_learner; }

    /** Where the sub's pitch comes from right now: a learned curve (true) or live tracking (false), and how sure
        that source is, 0..1 (the curve's confidence there, or the tracker's, smoothed). For the visualiser. */
    bool isPitchLearned() const { return m_pitchLearned.load(std::memory_order_relaxed); }
    float getPitchConfidence() const { return m_pitchConfidence.load(std::memory_order_relaxed); }

    /** LEARN's trail for the view: what the live tracker heard, and the sub played, at each timeline position while
        the transport played (oldest first; empty until Learn is used). Message thread. */
    void getLearnTrail(std::vector<subbertone::learn::TrailPoint>& dest) const { m_learnAudio.copyTrail(dest); }

    /** Whether the host gives LEARN a timeline to line up what it hears with: false in the standalone app (its
        player counts samples but never plays), in Pro Tools' AudioSuite, in a host whose play head reports no
        position, and in one that has never played while its time runs on in real time or stays at a bare 0 without
        tempo (a host with no transport: JUCE's wrappers report a position all the same); true otherwise, also before
        any block has said (a plug-in host nearly always has one). Any thread. */
    bool hasHostTimeline() const;
    bool isStandaloneApp() const { return wrapperType == wrapperType_Standalone; }

    /** While an editor shows, the play head is read on every block for hasHostTimeline(), even before Learn is used
        (reading it changes nothing in the sound). */
    void watchHostTimeline(bool watch) { m_timelineWatched.store(watch, std::memory_order_relaxed); }

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
    void readTransport(subbertone::learn::Transport& transport) const;
    bool isUsableTimeline(const subbertone::learn::Transport& transport, int numSamples);
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
    std::atomic<float>* m_subOctave2Param = nullptr;
    std::atomic<float>* m_subOctave3Param = nullptr;

    // Live per-harmonic activity (0..1) for the harmonic strip, written once per block.
    std::array<std::atomic<float>, 17> m_harmonicActivity {};
    std::array<std::atomic<float>, 2> m_subOctaveActivity {};   // S2, S3
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

    // LEARN (non-parameter state, saved as the LEARN child of the state). The store holds what is learned; the
    // audio side listens, captures and fills the engine's pitch guide; the learner runs the commands and the
    // background jobs. Declared last, so the learner goes first and its jobs have stopped before the rest goes.
    static_assert(subbertone::learn::LearnAudio::c_maxChunkSamples == c_chunkSize);
    subbertone::learn::LearnStore m_learnStore;
    subbertone::learn::LearnAudio m_learnAudio { m_learnStore };
    std::atomic<bool> m_pitchLearned { false };
    std::atomic<float> m_pitchConfidence { 0.0f };
    float m_liveConfidence = 0.0f;                // audio thread: the tracker's confidence, smoothed
    std::atomic<bool> m_timelineWatched { false }; // an editor shows: read the play head even before Learn is used
    std::atomic<int> m_hostTimeline { -1 };        // the play head's latest report: -1 none read, 0 no usable
                                                   // timeline, 1 a timeline (see isUsableTimeline())
    std::atomic<bool> m_inAudioSuite { false };    // Pro Tools' AudioSuite: no timeline to learn on
    bool m_hostHasPlayed = false;                  // audio thread: the host has said it plays at least once
    bool m_haveHostTime = false;                   // audio thread: the last block read had a position ...
    std::int64_t m_nextHostTime = 0;               // ... and this is where the next one continues it
    subbertone::learn::Learner m_learner { m_learnStore, m_learnAudio };
};

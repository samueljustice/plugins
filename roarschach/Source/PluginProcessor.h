#pragma once

#include <JuceHeader.h>

#include "PresetManager.h"
#include "dsp/CreatureChain.h"
#include "dsp/DryPhaseMatch.h"
#include "dsp/MonsterFeatures.h"

#include <array>
#include <atomic>
#include <vector>

class RoarschachAudioProcessor : public juce::AudioProcessor
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RoarschachAudioProcessor)
    JUCE_DECLARE_WEAK_REFERENCEABLE(RoarschachAudioProcessor)

public:
    RoarschachAudioProcessor();
    ~RoarschachAudioProcessor() override = default;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;

    double getTailLengthSeconds() const override;

    /** The factory presets, plus one last program, "User Preset", that stands for any sound
        that is not a factory preset (a user preset or an unsaved sound). Selecting it does
        nothing; it only exists so the host's program display is right and so that every
        factory program, "Init" included, can always be selected. */
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void releaseResources() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void reset() override;

    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    /** Host bypass: the input delayed by exactly getLatencySamples(), crossfaded in and out.
        The chain keeps running underneath, so leaving bypass is seamless. */
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    /** Editor view options (e.g. the editor scale), saved with the plugin state. Safe to call on
        the message thread while the host saves or restores the state on another thread. */
    bool getViewOption(const juce::Identifier& option, bool defaultValue) const;
    void setViewOption(const juce::Identifier& option, bool value);
    juce::var getViewValue(const juce::Identifier& option, const juce::var& defaultValue) const;
    void setViewValue(const juce::Identifier& option, const juce::var& value);

    PresetManager& getPresetManager() { return m_presetManager; }
    juce::AudioProcessorValueTreeState& getValueTreeState() { return m_parameters; }

    /** What the audio is doing, for the visualiser (written by the audio thread once per chunk). */
    const roar::MonsterFeatures& getMonsterFeatures() const { return m_features; }

    /** Slot each movable module is running in right now (during a move it lags the parameter). */
    int getDetailSlotInUse() const { return m_detailSlotInUse.load(std::memory_order_relaxed); }
    int getVocoderSlotInUse() const { return m_vocoderSlotInUse.load(std::memory_order_relaxed); }

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

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    roar::CreatureChain::Settings readSettings() const;

    void render(juce::AudioBuffer<float>& buffer, bool bypassed);
    void handlePendingReset(bool bypassed);
    void delayDry(const juce::AudioBuffer<float>& buffer, int start, int numSamples, int numInputs, int numOutChannels);
    void applyBypassFade(juce::AudioBuffer<float>& buffer, int start, int numSamples, int numChannels, float target);

    roar::CreatureChain m_chain;
    roar::MonsterFeatures m_features;
    std::atomic<int> m_detailSlotInUse { 0 }, m_vocoderSlotInUse { 0 };

    // Scratch buffers, sized in prepareToPlay. Blocks larger than this are processed in chunks.
    static constexpr int c_chunkSize = 512;
    std::vector<float> m_wetL, m_wetR;

    // Dry path: the input through a delay of exactly the reported latency (always fed), used by
    // MIX and by the host bypass crossfade. Two channels: a mono output takes the mono fold.
    static constexpr double c_bypassFadeSeconds = 0.015;
    static constexpr int c_maxChannels = 2;
    std::array<std::vector<float>, c_maxChannels> m_dryDelay;   // getLatencySamples() long
    std::array<std::vector<float>, c_maxChannels> m_dryChunk;   // c_chunkSize: delayed input for this chunk
    // The delayed input with the phase of the wet chain's fixed filters (see DryPhaseMatch): the
    // dry that partial MIX settings and the bypass crossfade blend with, so they do not comb.
    std::array<std::vector<float>, c_maxChannels> m_dryMatchedChunk;
    std::array<roar::DryPhaseMatch, c_maxChannels> m_dryMatch;
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
    std::atomic<float>* m_pitchParam = nullptr;
    std::atomic<float>* m_sizeParam = nullptr;
    std::atomic<float>* m_growlParam = nullptr;
    std::atomic<float>* m_liquidParam = nullptr;
    std::atomic<float>* m_roarParam = nullptr;
    std::atomic<float>* m_subParam = nullptr;
    std::atomic<float>* m_detailLowParam = nullptr;
    std::atomic<float>* m_detailHighParam = nullptr;
    std::atomic<float>* m_gravelParam = nullptr;
    std::atomic<float>* m_mixParam = nullptr;
    std::atomic<float>* m_detailSlotParam = nullptr;
    std::atomic<float>* m_vocoderSlotParam = nullptr;
    std::atomic<float>* m_vocoderOctaveParam = nullptr;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_mixSmoothed;
    // 0: MIX blends the plain delayed input (exactly the input at Mix 0); 1: the phase-matched
    // one. Glides over 20 ms when Mix leaves or reaches 0, so the switch never clicks.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_dryMatchAmount;

    // Editor view options. Kept out of the APVTS tree (which the host may replace on another
    // thread at any time) and written into / read from the state XML.
    mutable juce::CriticalSection m_viewLock;
    juce::NamedValueSet m_viewOptions;
};

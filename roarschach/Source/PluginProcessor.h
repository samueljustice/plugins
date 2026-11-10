#pragma once

#include <JuceHeader.h>

#include "CreatureEvolution.h"
#include "Dice.h"
#include "Params.h"
#include "PresetManager.h"
#include "dsp/CreatureChain.h"
#include "dsp/MonsterFeatures.h"

#include <array>
#include <atomic>
#include <optional>
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
    /** The preset, ROLL ALL and module dice, with their undo history (message thread). */
    DiceRoller& getDice() { return m_dice; }
    /** Hold a Scope on this while writing several module positions (see PositionWrites). */
    roar::PositionWrites& getPositionWrites() { return m_positionWrites; }
    juce::AudioProcessorValueTreeState& getValueTreeState() { return m_parameters; }

    /** What the audio is doing, for the visualiser (written by the audio thread once per chunk). */
    const roar::MonsterFeatures& getMonsterFeatures() const { return m_features; }

    /** The creature's evolved form, published ten times a second by the message thread. */
    const roar::CreatureShared& getCreatureShared() const { return m_creatureShared; }
    /** Evolution Live / Frozen (message thread; saved with the session). */
    bool isCreatureFrozen() const;
    void setCreatureFrozen(bool frozen);
    /** Eases the creature home over a few seconds and grows a new one (message thread). */
    void resetCreature();
    /** The colour a Frozen creature holds (its colour mood's four axes), saved with the session so it
        survives the editor closing and the project reloading; nullopt when none is kept (message thread). */
    std::optional<std::array<float, 4>> getCreatureColour() const;
    void setCreatureColour(std::optional<std::array<float, 4>> colour);

    /** Tests: one evolution step with the features as they are now (what the timer does). */
    void stepCreatureForTests(float dt) { stepCreature(dt); }
    /** Tests: direct access (hold no other lock while using it). */
    template <typename Fn> void withCreatureForTests(Fn&& fn)
    {
        const juce::ScopedLock sl(m_creatureLock);
        fn(m_creature);
        m_creatureShared.publish(m_creature.evaluate());
    }

    /** Test and render hooks: the chain itself (audio thread state; use it only while not processing). */
    const roar::CreatureChain& getChain() const { return m_chain; }
    roar::CreatureChain& getChainForTests() { return m_chain; }

    /** The chain settings the parameters describe right now (any thread; atomics only). */
    roar::ChainSettings readSettings() const;

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
    roar::PositionWrites m_positionWrites;         // before m_presetManager: it holds scopes on this
    Params::Order m_heldPositions {};             // audio thread: the last complete order read
    bool m_haveHeldPositions = false;
    PresetManager m_presetManager;                // after m_parameters: it listens to them
    DiceRoller m_dice;                            // after m_presetManager: it loads and restores presets

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /**
     * A parameter's index in Params::all, resolved at compile time from its ID, so the audio
     * thread's readSettings() never searches the table. An unknown ID does not compile.
     */
    struct ParamIndex
    {
        consteval ParamIndex(const char* id) : index(Params::indexOf(id))
        {
            if (index < 0)
                unknownParameterId();
        }
        int index;

    private:
        static void unknownParameterId() {}   // not constexpr: calling it makes the ID a compile error
    };

    float value(ParamIndex p) const { return valueAt(p.index); }
    float valueAt(int index) const;
    float percent(ParamIndex p) const { return valueAt(p.index) * 0.01f; }
    bool toggle(ParamIndex p) const { return valueAt(p.index) >= 0.5f; }
    int choice(ParamIndex p) const;

    void render(juce::AudioBuffer<float>& buffer, bool bypassed);
    void handlePendingReset(bool bypassed);
    void delayDry(const juce::AudioBuffer<float>& buffer, int start, int numSamples, int numInputs, int numOutChannels);
    void applyBypassFade(juce::AudioBuffer<float>& buffer, int start, int numSamples, int numChannels, float target);

    roar::CreatureChain m_chain;
    roar::MonsterFeatures m_features;

    // Scratch buffers, sized in prepareToPlay. Blocks larger than this are processed in chunks.
    static constexpr int c_chunkSize = 512;
    std::vector<float> m_inL, m_inR, m_wetL, m_wetR;

    // Host bypass: the input through a delay of exactly the reported latency (always fed), which
    // the output crossfades to while bypassed. Two channels: a mono output takes the mono fold.
    static constexpr double c_bypassFadeSeconds = 0.020;
    static constexpr int c_maxChannels = 2;
    std::array<std::vector<float>, c_maxChannels> m_dryDelay;   // getLatencySamples() long
    std::array<std::vector<float>, c_maxChannels> m_dryChunk;   // c_chunkSize: delayed input for this chunk
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

    // Raw parameter values, in Params::all order (looked up once).
    std::array<std::atomic<float>*, Params::numParameters> m_values {};

    // Editor view options. Kept out of the APVTS tree (which the host may replace on another
    // thread at any time) and written into / read from the state XML.
    mutable juce::CriticalSection m_viewLock;
    juce::NamedValueSet m_viewOptions;

    // The creature's evolution: stepped on the message thread (so it grows with the editor shut),
    // read by the GL thread through m_creatureShared. The lock covers the host saving or loading
    // the state on another thread.
    void stepCreature(float dt);
    roar::CreatureEvolution::Inputs readCreatureInputs();
    mutable juce::CriticalSection m_creatureLock;
    roar::CreatureEvolution m_creature;
    roar::CreatureShared m_creatureShared;
    int m_creatureGeneration = -1;
    float m_creatureStaleSeconds = 0.0f;

    struct CreatureClock final : private juce::Timer
    {
        explicit CreatureClock(RoarschachAudioProcessor& p) : m_processor(p) { startTimerHz(10); }
        ~CreatureClock() override { stopTimer(); }

    private:
        void timerCallback() override;
        RoarschachAudioProcessor& m_processor;
        double m_last = juce::Time::getMillisecondCounterHiRes();
    };
    CreatureClock m_creatureClock { *this };   // last: stops before anything it reads goes away
};

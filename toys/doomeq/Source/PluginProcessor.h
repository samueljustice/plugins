#pragma once

#include <JuceHeader.h>
#include "EqDsp.h"
#include "PresetManager.h"

class DoomEQProcessor : public juce::AudioProcessor
{
public:
    static constexpr int numBands = 6;
    static constexpr int maxChannels = 8;

    DoomEQProcessor();
    ~DoomEQProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    // A Q-10 band at 20 Hz takes about a second to ring down by 60 dB.
    double getTailLengthSeconds() const override { return 1.0; }

    /** Host programs: the factory presets, then "User Preset" for any other sound. */
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    PresetManager& getPresetManager() { return presets; }

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::String pid (int band, const char* name) { return "b" + juce::String (band + 1) + "_" + name; }

    /** Reads post-EQ mono samples for the analyser (message thread). */
    int pullAnalyserSamples (float* dest, int maxSamples);

    double getCurrentSampleRate() const { return currentSampleRate.load(); }

    /** Output peak (linear), decaying; read by the editor for the HUD face. */
    float getOutputPeak() const { return outputPeak.load(); }

    /** Counts processed blocks, so the editor can tell when the host has stopped calling us. */
    uint32_t getBlockCount() const { return blockCount.load(); }

    /** Editor-only settings saved with the session (not parameters). Message thread. */
    float getOverlay() const            { return uiOverlay.load(); }
    void setOverlay (float percent);
    juce::Point<int> getEditorSize() const { return { uiWidth.load(), uiHeight.load() }; }
    void setEditorSize (int w, int h)   { uiWidth = w; uiHeight = h; }
    bool ownsDoomAudio() const;
    bool doomAudioUsedElsewhere() const;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // Reports preset and state changes to the host from the message thread, coalesced
    // (the JUCE 8 wrappers expect updateHostDisplay there).
    class HostDisplayUpdater final : private juce::AsyncUpdater
    {
    public:
        explicit HostDisplayUpdater (juce::AudioProcessor& p) : processor (p) {}
        ~HostDisplayUpdater() override { cancelPendingUpdate(); }

        void post (const juce::AudioProcessorListener::ChangeDetails& details)
        {
            const int f = (details.programChanged ? programChanged : 0) | (details.nonParameterStateChanged ? stateChanged : 0);
            if (f == 0)
                return;
            flags.fetch_or (f);
            triggerAsyncUpdate();
        }

    private:
        void handleAsyncUpdate() override
        {
            const int f = flags.exchange (0);
            if (f != 0)
                processor.updateHostDisplay (juce::AudioProcessorListener::ChangeDetails()
                                                 .withProgramChanged ((f & programChanged) != 0)
                                                 .withNonParameterStateChanged ((f & stateChanged) != 0));
        }

        static constexpr int programChanged = 1, stateChanged = 2;
        juce::AudioProcessor& processor;
        std::atomic<int> flags { 0 };
    };

    HostDisplayUpdater hostDisplay { *this }; // before presets: they report through it
    PresetManager presets;                    // after apvts: it listens to the parameters

    void updateTargets();
    void processChunk (juce::AudioBuffer<float>&, int numCh, double sr);

    struct Band
    {
        std::atomic<float>* on = nullptr;
        std::atomic<float>* type = nullptr;
        std::atomic<float>* freq = nullptr;
        std::atomic<float>* gain = nullptr;
        std::atomic<float>* q = nullptr;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> f, qs;
        juce::SmoothedValue<float> g, mix;

        // A type change dips the band out and back over ~2 x 8 ms instead of clicking.
        juce::SmoothedValue<float> typeFade;
        int pendingType = 0;

        int currentType = 0;
        bool dirty = true;
        bool wasSilent = false;
        eq::BandCoeffs coeffs;
        std::array<std::array<eq::StageState, eq::maxStages>, maxChannels> state {};

        void resetState() { for (auto& ch : state) ch.fill ({}); }
        bool isSilent() const { return ! mix.isSmoothing() && mix.getTargetValue() == 0.0f; }

        void switchToPendingType()
        {
            // Every type shares the SVF core, so stage 0 carries over. Stages the new type
            // doesn't use are cleared, so a steeper cut later starts them from rest.
            currentType = pendingType;
            const int stagesNow = eq::numStages ((eq::Type) currentType);
            for (auto& ch : state)
                for (int st = stagesNow; st < eq::maxStages; ++st)
                    ch[(size_t) st] = {};
            dirty = true;
        }
    };

    std::array<Band, numBands> bands;
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* doomSoundParam = nullptr;
    std::atomic<float>* doomLevelParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;   // the host's bypass
    juce::AudioParameterBool* eqBypassParam = nullptr; // the BYPASS button: EQ out, DOOM still in
    juce::SmoothedValue<float> outputGain, doomGain, wetMix;
    juce::AudioBuffer<float> doomBuffer, dryBuffer;
    int maxChunk = 512;
    std::atomic<float> outputPeak { 0.0f };
    std::atomic<uint32_t> blockCount { 0 };

    std::atomic<float> uiOverlay { 70.0f };
    std::atomic<int> uiWidth { 0 }, uiHeight { 0 };

    std::atomic<double> currentSampleRate { 44100.0 };

    juce::AbstractFifo analyserFifo { 1 << 15 };
    std::vector<float> analyserBuffer = std::vector<float> (1 << 15);
    std::vector<float> monoScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DoomEQProcessor)
};

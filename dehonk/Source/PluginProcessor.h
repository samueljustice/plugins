#pragma once

#include <JuceHeader.h>

#include "DeHonkCore.h"
#include "PresetManager.h"

#include <array>
#include <atomic>
#include <type_traits>

/**
 * De-Honk's processor: parameters, state, presets, latency reporting and the display FIFO around
 * one dehonk::Core, which does all of the signal processing (SPEC section 3).
 *
 * Threads:
 * - Audio thread: processBlock / processBlockBypassed. Reads parameters from atomics, runs the
 *   core, and while an editor is open pushes display frames into a lock-free FIFO. No allocation,
 *   locks or system calls.
 * - Message thread: latency reporting (the Lookahead choice changes the latency, which is only
 *   reported from here), state, presets, the editor.
 * Buses: the main input and output (the same layout, mono to 7.1.4) and a second input, the
 * dialogue sidechain (mono, which is what Pro Tools offers, or stereo; it may be off). With
 * Dialogue switched on, what arrives there is taken as the dialogue stem (SPEC 13.17).
 *
 * - reset() may be called on the UI thread while audio renders (JUCE's AU wrapper does it when
 *   bypass is switched on), so it only raises a flag that the audio thread acts on.
 */
class DeHonkAudioProcessor : public juce::AudioProcessor,
                             private juce::AudioProcessorValueTreeState::Listener,
                             private juce::Timer
{
public:
    DeHonkAudioProcessor();
    ~DeHonkAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processorLayoutsChanged() override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParameter; }

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }
    PresetManager& getPresetManager() noexcept { return presetManager; }

    /** Listening tools (SPEC 4.1). Not parameters: never automated, saved or recalled as on. */
    void setSoloHonk (bool on) noexcept { soloHonk.store (on); }
    bool isSoloHonk() const noexcept { return soloHonk.load(); }
    void setDiff (bool on) noexcept { diff.store (on); }
    bool isDiff() const noexcept { return diff.load(); }

    /** The editor says when it is showing (open and not hidden or minimised); the audio thread
        writes display frames only while one is. Calls must be balanced. */
    void setEditorOpen (bool open) noexcept;

    /** Where one reader of the display frames has got to. Every window (or anything else that
        shows the frames) keeps its own, so that each sees every frame however many are open. */
    struct DisplayReader
    {
        std::uint64_t next = 0;
        bool started = false;
    };

    /** Message thread, any number of readers, each with its own DisplayReader: takes the frames
        this reader has not seen yet and leaves the newest taken in 'latest', with snapCutDb the
        deepest of all the frames taken. False if there was none. Paced (for a reader that calls at
        the frame rate or faster), it takes one and leaves up to a host buffer's worth waiting, so
        that frames which arrived together are shown one after another; otherwise it takes them all.
        A reader that falls more than the ring's length behind skips the oldest. */
    bool popLatestDisplayFrame (DisplayReader& reader, dehonk::DisplayFrame& latest, bool paced = false) noexcept;
    /** The same with a reader of the processor's own (for tools with a single reader). */
    bool popLatestDisplayFrame (dehonk::DisplayFrame& latest, bool paced = false) noexcept { return popLatestDisplayFrame (ownReader, latest, paced); }

    /** The Impact Guard amount its switch goes back to when it is switched on again (percent; the
        switch sets the amount to 0 for off). Kept here so it outlives the editor; not saved. */
    void setGuardAmountMemory (float percent) noexcept { guardAmountMemory.store (percent, std::memory_order_relaxed); }
    float getGuardAmountMemory() const noexcept { return guardAmountMemory.load (std::memory_order_relaxed); }

    /** True while the host has the dialogue sidechain connected (it may still be silent). Any thread. */
    bool isSidechainConnected() const noexcept { return sidechainConnected.load (std::memory_order_relaxed); }

    /** 1: the first test build. 2: Link is a mode (linkMode) and not a percentage (link); the
        dialogue sidechain, each range's own timing and Detection are there. An older state is brought up to
        date when it is restored (migrateState). */
    static constexpr int stateVersion = 2;
    static constexpr int displayFifoSize = 64;
    static constexpr double displayFramesPerSecond = 60.0;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void render (juce::AudioBuffer<float>& buffer, bool hostBypassed) noexcept;
    void handlePendingReset (bool bypassed) noexcept;
    void pushDisplayFrame (int numSamples) noexcept;
    void collectClick() noexcept;
    dehonk::Params readParams (bool bypassed) const noexcept;
    void migrateState (juce::ValueTree& state) const;

    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void timerCallback() override;
    void updateReportedLatency();

    juce::AudioProcessorValueTreeState parameters;
    PresetManager presetManager;

    std::atomic<float>* thresholdValue = nullptr;
    std::atomic<float>* depthValue = nullptr;
    std::atomic<float>* sharpnessValue = nullptr;
    std::atomic<float>* speedValue = nullptr;
    std::atomic<float>* maxCutValue = nullptr;
    std::atomic<float>* releaseValue = nullptr;
    std::atomic<float>* impactGuardValue = nullptr;
    std::array<std::atomic<float>*, dehonk::kNumModules> moduleOnValues {};
    std::atomic<float>* clickOnValue = nullptr;
    std::array<std::atomic<float>*, dehonk::kNumEdges> edgeValues {};
    std::atomic<float>* linkModeValue = nullptr;
    std::atomic<float>* dialogueValue = nullptr;
    std::atomic<float>* dialogueAmountValue = nullptr;
    std::array<std::atomic<float>*, dehonk::kNumModules> timingLockedValues {};
    std::array<std::atomic<float>*, dehonk::kNumModules> rangeAttackValues {};
    std::array<std::atomic<float>*, dehonk::kNumModules> rangeReleaseValues {};
    std::atomic<float>* detectionValue = nullptr;
    std::atomic<float>* lookaheadValue = nullptr;
    std::atomic<float>* mixValue = nullptr;
    std::atomic<float>* trimValue = nullptr;
    juce::AudioParameterBool* bypassParameter = nullptr;

    dehonk::Core core;
    int preparedChannels = 0;
    // Raised by processorLayoutsChanged(); prepareToPlay() records the one it prepared for.
    std::atomic<std::uint32_t> layoutGeneration { 0 };
    std::uint32_t preparedLayoutGeneration = 0;
    int preparedBlockSize = 0;
    double preparedSampleRate = 0.0;

    std::atomic<bool> sidechainConnected { false };
    std::atomic<int> displayBurst { 1 };     // how many display frames one host buffer makes
    std::atomic<float> guardAmountMemory { 100.0f };
    std::atomic<bool> soloHonk { false };
    std::atomic<bool> diff { false };

    // Reset requests from reset(), handled on the audio thread (see handlePendingReset()).
    std::atomic<std::uint32_t> resetRequests { 0 };
    std::uint32_t resetRequestsHandled = 0;
    bool wasBypassed = false;
    std::int64_t samplesSinceBypassOn = 0;

    // Display frames, audio thread -> message thread.
    std::atomic<int> editorsOpen { 0 };
    // A ring of the last frames, written by the audio thread only and read by any number of
    // readers, none of which holds the writer up. Each slot is a sequence lock: its stamp is odd
    // while frame n is being written (2n + 1) and 2n + 2 once it is there, and a reader keeps a
    // copy only if the stamp was the same before and after. The frame is kept as atomic words, so
    // that a read that overlaps a write is not a data race (it is thrown away). Preallocated with
    // the processor.
    static_assert (std::is_trivially_copyable_v<dehonk::DisplayFrame>);
    static constexpr int frameWords = (int) ((sizeof (dehonk::DisplayFrame) + 3) / 4);
    struct FrameSlot
    {
        std::atomic<std::uint64_t> stamp { 0 };
        std::array<std::atomic<std::uint32_t>, (size_t) frameWords> words {};
    };
    std::array<FrameSlot, displayFifoSize> frameSlots;
    std::atomic<std::uint64_t> framesWritten { 0 };
    DisplayReader ownReader;
    bool readFrame (std::uint64_t n, dehonk::DisplayFrame& out) const noexcept;
    std::uint32_t lastFrameCounter = 0;
    int samplesSinceDisplayFrame = 0;
    int samplesPerDisplayFrame = 1600;

    // The core reports Click's deepest cut per detector tick (about a millisecond). While Click is
    // on and an editor shows it, the core is run in pieces no longer than a tick and the deepest
    // cut is collected here until the next frame goes to the display.
    static constexpr int clickPieceSamples = 16;
    std::uint32_t clickFrameSeen = 0;
    float clickHeldDb = 0.0f;

    std::atomic<bool> latencyDirty { false };
    std::atomic<bool> programDisplayDirty { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeHonkAudioProcessor)
};

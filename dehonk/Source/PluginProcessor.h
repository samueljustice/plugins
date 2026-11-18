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

    /** Message thread (one consumer): takes every frame waiting in the FIFO and leaves the newest
        in 'latest', with snapCutDb the deepest of all the frames taken. False if there was none. */
    bool popLatestDisplayFrame (dehonk::DisplayFrame& latest) noexcept;

    static constexpr int stateVersion = 1;
    static constexpr int displayFifoSize = 64;
    static constexpr double displayFramesPerSecond = 30.0;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void render (juce::AudioBuffer<float>& buffer, bool hostBypassed) noexcept;
    void handlePendingReset (bool bypassed) noexcept;
    void pushDisplayFrame (int numSamples) noexcept;
    void collectClick() noexcept;
    dehonk::Params readParams (bool bypassed) const noexcept;

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
    std::atomic<float>* linkValue = nullptr;
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

    std::atomic<bool> soloHonk { false };
    std::atomic<bool> diff { false };

    // Reset requests from reset(), handled on the audio thread (see handlePendingReset()).
    std::atomic<std::uint32_t> resetRequests { 0 };
    std::uint32_t resetRequestsHandled = 0;
    bool wasBypassed = false;
    std::int64_t samplesSinceBypassOn = 0;

    // Display frames, audio thread -> message thread.
    std::atomic<int> editorsOpen { 0 };
    juce::AbstractFifo displayFifo { displayFifoSize };
    // Preallocated with the processor; frames are copied in and out by assignment.
    static_assert (std::is_trivially_copyable_v<dehonk::DisplayFrame>);
    std::array<dehonk::DisplayFrame, displayFifoSize> displayFrames {};
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

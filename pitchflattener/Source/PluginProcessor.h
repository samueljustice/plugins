#pragma once

#include <JuceHeader.h>

#include "Engine.h"

#include <atomic>

/**
 * Pitch Flattener's processor: the host's side of one Engine (Engine.h does the work).
 *
 * Parameters (state version 1): target (High Point, Centre, Low Point, Start, Note), note (the
 * pitch for Note, a MIDI note with cents), amount (0..100 %) and line (Auto, Harmonic, Ridge).
 * Learn and Clear are actions, not parameters: they are not automated or saved.
 *
 * The saved state holds the parameters and the editor's choice of view. The learned sound lives in
 * memory for as long as the plug-in instance does; a session reopened later has to learn it again.
 *
 * The plug-in reports a fixed latency of Engine::kLatencySeconds (set once, in prepareToPlay).
 */
class PitchFlattenerAudioProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    PitchFlattenerAudioProcessor();
    ~PitchFlattenerAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    void setNonRealtime (bool isNonRealtime) noexcept override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
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
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    static constexpr int stateVersion = 1;

    // Parameter IDs.
    static constexpr const char* targetId = "target";
    static constexpr const char* noteId = "note";
    static constexpr const char* amountId = "amount";
    static constexpr const char* lineId = "line";

    juce::AudioProcessorValueTreeState parameters;

    pitchflattener::Engine& getEngine() noexcept { return m_engine; }
    const pitchflattener::Engine& getEngine() const noexcept { return m_engine; }

    /** The options the parameters set now. */
    pitchflattener::core::Options optionsFromParameters() const;
    /** Hands the parameters to the engine (the timer does this 20 times a second; tests call it directly). */
    void syncOptions();

    /** Sets a parameter as one host gesture (the editor's buttons, clicks on the display). */
    void setParameterFromEditor (const char* id, float plainValue);

    static juce::String noteText (double midi);

    /** The editor's display view as the user last chose it: kViewUnset (none yet: SCOPE once something
        is learned, FLIGHT while nothing is), kViewScope or kViewFlight. Saved with the state; not a
        parameter, so hosts neither automate it nor show it. Message thread. */
    static constexpr int kViewUnset = -1, kViewScope = 0, kViewFlight = 1;
    int getDisplayView() const;
    void setDisplayView (int view);

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void timerCallback() override;

    pitchflattener::Engine m_engine;
    std::atomic<double> m_tailSeconds { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchFlattenerAudioProcessor)
};

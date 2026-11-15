#pragma once

#include <JuceHeader.h>

#include "Engine.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

/**
 * Pitch Flattener's processor: the host's side of one Engine (Engine.h does the work).
 *
 * Parameters (state version 1): target (High Point, Centre, Low Point, Start, Note), note (the
 * pitch for Note, a MIDI note with cents), amount (0..100 %), source (Auto, Voice, Engine, Tone:
 * what kind of sound it is, so which reading of its pitch is used), keepLength (Varispeed or
 * Keep length) and character (Harmonic, Transients, Voice: how the length is kept, Keep length only). Learn and Clear are actions, not parameters: they are not automated or saved.
 *
 * Hand corrections to the line (the scope's curve editor) are kept here too, for the learned sound
 * and per reading. They are not parameters and are not saved with the session: the learned sound
 * itself is not saved either, so there would be nothing to apply them to on reopening.
 *
 * The saved state holds the parameters and the editor's choice of view. The learned sound lives in
 * memory for as long as the plug-in instance does; a session reopened later has to learn it again.
 *
 * The plug-in reports a fixed latency, set once in prepareToPlay: Engine::latencyFor(rate, block),
 * 0.4 s unless the host's blocks are very large (see Engine.h).
 *
 * State: saved with stateVersion. A state from a newer version is read only for the parameters
 * this version knows, by ID (anything else it holds is left alone); an older or equal one is
 * restored as it is.
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
    static constexpr const char* sourceId = "source";
    static constexpr const char* keepLengthId = "keepLength";
    static constexpr const char* characterId = "character";

    juce::AudioProcessorValueTreeState parameters;

    pitchflattener::Engine& getEngine() noexcept { return m_engine; }

    // Learn's actions (message thread).
    /** Arms Learn: the next sound played through is learned. */
    void armLearn() { m_engine.armLearn(); }
    /** Stops Learn without learning anything; the sound learned before (if any) stays and plays. */
    void cancelLearn() { m_engine.cancelLearn(); }
    /** Forgets the learned sound. */
    void clear() { m_engine.clear(); }
    const pitchflattener::Engine& getEngine() const noexcept { return m_engine; }

    /** The options the parameters set now. */
    pitchflattener::core::Options optionsFromParameters() const;
    /** Hands the parameters to the engine (the timer does this 20 times a second; tests call it directly). */
    void syncOptions();

    /** Sets a parameter as one host gesture (the editor's buttons, clicks on the display). */
    void setParameterFromEditor (const char* id, float plainValue);

    static juce::String noteText (double midi);

    //==========================================================================
    // Hand corrections (message thread). Each reading of the learned sound keeps its own, so
    // choosing another kind of sound and coming back finds them as they were; learning another
    // sound or Clear forgets them all.

    /** The reading the options use now: the one SOURCE names if it is usable, else the analysis'
        pick; -1 with nothing learned or no usable reading. */
    int lineInUse() const;
    /** The corrections on a reading of the sound learned now (empty for any other). */
    std::vector<pitchflattener::core::CurvePoint> getEdits (int line) const;
    /** Replaces a reading's corrections; the engine renders again at its next sync. */
    void setEdits (int line, std::vector<pitchflattener::core::CurvePoint> edits);
    /** The most corrections a reading can hold (a freehand line is many points). */
    static constexpr std::size_t kMaxEdits = 512;

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

    // The corrections and the learned sound they belong to (Learned::id).
    std::uint64_t m_editsFor = 0;
    std::array<std::vector<pitchflattener::core::CurvePoint>, pitchflattener::core::kNumTrackers> m_edits;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchFlattenerAudioProcessor)
};

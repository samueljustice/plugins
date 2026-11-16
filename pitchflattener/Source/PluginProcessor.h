#pragma once

#include <JuceHeader.h>

#include "Engine.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

/**
 * Pitch Flattener's processor: the host's side of one Engine (Engine.h does the work).
 *
 * Parameters: target (High Point, Centre, Low Point, Start, Note), note (the pitch for Note, a MIDI
 * note with cents), amount (0..100 %), source (Auto, Voice, Engine, Tone: what kind of sound it is,
 * so which reading of its pitch is used), keepLength (Varispeed or Keep length) and character
 * (Harmonic, Transients, Voice: how the length is kept, Keep length only). Learn and Clear are
 * actions, not parameters: they are not automated.
 *
 * Hand corrections to the line (the scope's curve editor) are kept here, for the learned sound and
 * per reading. They are not parameters.
 *
 * The state (version 2) carries the parameters, the editor's choice of view, and the learned sound:
 * its id, its hand corrections, and the sound itself compressed (SoundCodec.h; compressed once when
 * it is learned and reused for every save; a sound over Engine::kMaxStoredBytes compressed is
 * carried by its id only). A host that renders with another instance made from the state (or a
 * session reopened later) therefore has the sound: taken at once from an instance in the same
 * process that has it (the registry, SoundRegistry.h), or else decoded and analysed again in the
 * background, as after a Learn. Layout: "PFS2", int32 version, int32 XML size, the XML, int64 sound
 * size, the sound. Version 1 states (JUCE's XML binary, no sound) load as they always did; a state
 * from a newer version gives only the parameters this version knows, by ID.
 *
 * The plug-in reports a fixed latency, set once in prepareToPlay: Engine::latencyFor(rate, block),
 * 0.4 s unless the host's blocks are very large (see Engine.h).
 *
 * Every instance writes what the host does to the diagnostic log (DiagnosticLog.h).
 */
class PitchFlattenerAudioProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    PitchFlattenerAudioProcessor();
    ~PitchFlattenerAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
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
    static constexpr int stateVersion = 2;

    // Parameter IDs.
    static constexpr const char* targetId = "target";
    static constexpr const char* noteId = "note";
    static constexpr const char* amountId = "amount";
    static constexpr const char* sourceId = "source";
    static constexpr const char* keepLengthId = "keepLength";
    static constexpr const char* characterId = "character";

    juce::AudioProcessorValueTreeState parameters;

    pitchflattener::Engine& getEngine() noexcept { return m_engine; }
    const pitchflattener::Engine& getEngine() const noexcept { return m_engine; }

    // Learn's actions (message thread).
    /** Arms Learn: the next sound played through is learned. */
    void armLearn() { m_engine.armLearn(); }
    /** Stops Learn without learning anything; the sound learned before (if any) stays and plays. */
    void cancelLearn() { m_engine.cancelLearn(); }
    /** Forgets the learned sound (here only: another instance that took it keeps it). */
    void clear() { m_engine.clear(); }

    /** The options the parameters set now, for the sound learned now. */
    pitchflattener::core::Options optionsFromParameters() const;
    /** ...and for a given learned sound (its hand corrections, if it is the one they were made on). */
    pitchflattener::core::Options optionsFor (const pitchflattener::Learned* learned) const;
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

    /** This instance's number in the log. */
    std::uint32_t getInstanceNumber() const noexcept { return m_instance; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void timerCallback() override;
    int lineFor (const pitchflattener::core::Analysis& analysis) const;
    void noteBypass (bool bypassed) noexcept;
    static void engineSink (void* context, const pitchflattener::EngineEvent& event) noexcept;

    const std::uint32_t m_instance;
    std::atomic<double> m_tailSeconds { 0.0 };
    bool m_wasBypassed = false;   // audio thread
    std::atomic<int> m_nonRealtimeLogged { -1 };

    // The corrections and the learned sound they belong to (Learned::id). The engine's analysis
    // thread reads them too (Engine::Hooks::optionsFor), hence the lock.
    mutable std::mutex m_editsMutex;
    std::uint64_t m_editsFor = 0;
    std::array<std::vector<pitchflattener::core::CurvePoint>, pitchflattener::core::kNumTrackers> m_edits;

    // Last: deleted first, so its threads (which call back into this object) stop before anything
    // they touch goes.
    pitchflattener::Engine m_engine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchFlattenerAudioProcessor)
};

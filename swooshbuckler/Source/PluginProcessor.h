#pragma once

#include <JuceHeader.h>
#include "Cargo.h"
#include "Downsampler.h"
#include "MasterProcessor.h"
#include "Parameters.h"
#include "TapeRecorder.h"
#include "RigEngine.h"
#include "Modulators.h"
#include "MorphWander.h"

/**
 * Swooshbuckler: a whoosh generator / movement processor with a built-in tape
 * machine. Runs as a standalone app, or as an AU/VST3 (MIDI notes trigger
 * whooshes; audio on the input can be used as a source).
 *
 *   engine (sources -> flights -> rigging) -> inflate -> push -> limiter -> out
 *                                                                        \-> Captain's Log (tape)
 */
class SwooshbucklerAudioProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    SwooshbucklerAudioProcessor();
    ~SwooshbucklerAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    /** What rings on after the sound stops: the tail after Stop (at most c_tailMaxSeconds) and its closing fade. */
    double getTailLengthSeconds() const override { return c_tailMaxSeconds + 0.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState& getState() { return m_state; }
    CargoHold& getCargo() { return m_cargo; }
    const RigEngine& getEngine() const { return m_engine; }
    ModMatrix& getMods() { return m_mods; }
    /** The output (after the limiter), for the Output section's little scope. */
    const ScopeRing& getOutputScope() const { return m_outRing; }

    /** Fire a whoosh from the UI (any thread). */
    /** Transport. The rig opens stopped. Stop lets go of the sources and lets the echoes and Space ring out, into
        what you hear and into the tape, until they have fallen under c_tailFloorDb or c_tailMaxSeconds have passed;
        then a quarter-second fade and the engine rests. Any thread. */
    static constexpr float c_tailFloorDb = -70.0f;
    static constexpr double c_tailMaxSeconds = 6.0;
    void setPlaying(bool on);
    bool isPlaying() const { return m_playing.load(); }
    /** Follow Host (plug-in only; on unless switched off): the rig plays while the host's transport plays and stops
        (the tail rings out) when it stops, and every transport start begins the ride again from the same seed, so a
        bounce repeats. Play still starts and stops it by hand in between. Saved with the session. Any thread. */
    bool getFollowHost() const { return m_followHost.load(); }
    void setFollowHost(bool on) { m_followHost.store(on); }
    /** False in the standalone app (there is no host transport to follow). */
    bool canFollowHost() const { return wrapperType != wrapperType_Standalone; }

    /** Throw: fire one whoosh now (button, the F key; MIDI notes with Keys set to Throw do the same). Any thread. */
    void throwWhoosh() { m_uiThrows.fetch_add(1); }
    /** What MIDI keys do (the Keys parameter). */
    enum KeysMode { KeysSwell = 0, KeysPitch, KeysThrow };
    /** The keys that set the pitch right now: bit n of half 0 is MIDI note n, of half 1 note 64 + n. Any thread. */
    std::uint64_t getHeldKeys(int half) const { return m_keysView[static_cast<size_t>(half & 1)].load(std::memory_order_relaxed); }
    /** Where the morph dot is right now (0..1 across and up): the X / Y controls, or the wander's place while it
        wanders. rotation: how far a closed wander shape has turned (radians). Any thread. */
    void getMorphPosition(float& x, float& y, float& rotation) const
    {
        x = m_morphViewX.load(std::memory_order_relaxed);
        y = m_morphViewY.load(std::memory_order_relaxed);
        rotation = m_morphViewRotation.load(std::memory_order_relaxed);
    }
    /** Whether a change of this parameter (its index in getParameters()) right now is something the user did, to
        be saved. Always true: the morph wander flies the dot inside the processor and leaves X and Y alone. Any thread. */
    bool isUserEdit(int parameterIndex) const;
    /** New session: stops, empties every track and puts every parameter back to its default. Message thread. */
    void newSession();

    /** Swell: while held, every track's fader rides up (button, space bar, MIDI notes). Any thread. */
    void setSwell(bool held) { m_uiSwell.store(held); }

    // Captain's Log -----------------------------------------------------------
    bool startRecording(juce::String& error);
    void stopRecording();
    bool isRecording() const { return m_recorder.isRecording(); }
    /** Arm: a take is open. With Skip Silence it writes whenever there is sound; without, all the time. */
    bool arm(juce::String& error);
    bool isCapturing() const { return m_recorder.isCapturing(); }
    double getRecordedSeconds() const { return m_recorder.getRecordedSeconds(); }
    juce::File getLastRecording() const { return m_recorder.getLastFile(); }
    juce::File getCurrentRecording() const { return m_recorder.getCurrentFile(); }
    bool saveRetro(juce::File& written, juce::String& error);

    // App settings (shared across sessions, not saved with presets).
    juce::File getRecordFolder() const;
    void setRecordFolder(const juce::File& folder);
    int getBitDepth() const;
    void setBitDepth(int bits);
    /** Recording sample rate in Hz, 0 = the session rate. Above the device rate the rig renders at this rate. */
    int getRecordRate() const;
    void setRecordRate(int rate);
    /** Uses this recording rate without saving it in the settings (offline tools and tests); -1 goes back to
        the saved setting. Call before prepareToPlay. */
    void overrideRecordRate(int rate) { m_recordRateOverride = rate; }
    double getRenderRate() const { return m_renderRate; }
    double getDeviceRate() const { return m_deviceRate; }
    int getFileType() const;          // 0 WAV, 1 AIFF, 2 FLAC
    void setFileType(int type);
    bool getMono() const;
    void setMono(bool mono);
    bool getAutoRecord() const;
    void setAutoRecord(bool on);
    /** Skip Silence (on unless switched off): the tape leaves the quiet stretches out. Off: one continuous take. */
    bool getSkipSilence() const;
    void setSkipSilence(bool on);

    /** The settings file to use instead of opening our own: the standalone app shares its file (two objects on
        one file would overwrite each other's values), and tests point it at a scratch file. Not owned; set it
        before the processor is made and clear it after the processor is gone. Message thread. */
    static void useSettings(juce::PropertiesFile* shared) { s_sharedSettings = shared; }

    // Metering / visuals -------------------------------------------------------
    float getGainReductionDb() const { return m_master.getGainReductionDb(); }
    /** The deepest gain reduction since the last call (dB, <= 0), then starts again: for the one meter that polls. */
    float takeGainReductionDb() { return m_master.takeGainReductionDb(); }
    /** The level going to tape (after the limiter, before Master Trim), falling back at the same speed whatever
        the buffer size. */
    float getOutputPeak() const { return m_outputPeak.load(std::memory_order_relaxed); }
    /** The highest such level since the last call, then starts again: for the meter's peak-hold line. */
    float takeOutputPeak() { return m_outputPeakHold.exchange(0.0f, std::memory_order_relaxed); }

    static constexpr int c_scopeSize = 1 << 13;
    static constexpr int c_maxSection = 512; // the most samples processed between parameter and modulator updates
    /** Copies the most recent count mono output samples (count <= c_scopeSize). */
    void copyScope(float* dest, int count) const;

    /** Set by the standalone app: opens the audio device settings (empty in a plugin host). */
    static std::function<void()> showAudioSettings;

    // Editor state (message thread only: a restore from another thread is applied on the message thread)
    int savedEditorWidth = 0;
    juce::String currentPresetName { "Default" };

    /** Bumped (message thread) each time a restored state has been applied, so an open editor re-reads
        savedEditorWidth / currentPresetName. */
    std::atomic<int> stateGeneration { 0 };


    /** Seconds the rolling take lost because the disk couldn't keep up (0 = none). Any thread. */
    double getRecordingDroppedSeconds() const;

private:
    void timerCallback() override;
    void updateEngineParams();
    void prepareInternal();
    void renderAtRenderRate(const float* inL, const float* inR, float* l, float* r, int n);
    void applyRestoredState(const juce::ValueTree& cargo, int editorWidth, const juce::String& presetName); //
    void applyPendingState();                                                                              //

    // Restore handling. A state set off the message thread is kept here until the message thread has
    // applied it, and getStateInformation returns it meanwhile (a save straight after a load keeps the samples).
    struct PendingState
    {
        bool valid = false;
        int id = 0;
        juce::ValueTree cargo;
        int editorWidth = 0;
        juce::String presetName;
    };
    mutable juce::CriticalSection m_stateLock;
    PendingState m_pending;            // guarded by m_stateLock
    int m_pendingCounter = 0;          // guarded by m_stateLock
    int m_mirrorWidth = 0;             // copies of the editor fields for saves off the message thread (m_stateLock)
    juce::String m_mirrorPreset { "Default" };

    juce::AudioProcessorValueTreeState m_state;
    CargoHold m_cargo;
    RigEngine m_engine;
    std::atomic<bool> m_uiSwell { false };
    std::atomic<bool> m_playing { false }, m_restartPending { false };
    // The Play parameter (host automation): a change of it starts or stops the rig. m_playParamWas is its value
    // in the last block (audio thread).
    bool m_playParamWas = false;
    std::atomic<bool> m_followHost { true };
    std::atomic<bool> m_hostStopped { false }; // the transport stopped the rig: the timer puts the Play parameter back
    bool m_hostWasPlaying = false;   // audio thread
    bool m_reseedPending = false;    // audio thread: the transport started, begin again from the seed
    std::atomic<int> m_uiThrows { 0 };
    // MIDI keys (audio thread): down right now, and for Throw the keys of the last chord (they keep its pitches
    // after the keys are let go, until the next one starts).
    std::array<std::uint64_t, 2> m_keysDown {}, m_keysLatched {};
    std::array<std::atomic<std::uint64_t>, 2> m_keysView {};
    int m_sinceMidiThrow = 1 << 30;  // device samples since the last throw from a key (a chord is one throw)
    void handleMidi(const juce::MidiMessage&, int offsetSamples, bool mayThrow);
    int keysMode() const;
    // Morph wander (audio thread): the dot's own flight, blended in over the X / Y controls.
    MorphWander m_wander;
    bool m_wanderOn = false;
    float m_wanderBlend = 0.0f, m_morphX = 0.5f, m_morphY = 0.5f;
    std::atomic<float> m_morphViewX { 0.5f }, m_morphViewY { 0.5f }, m_morphViewRotation { 0.0f };
    void runWander(int numSamples);
    float m_transportGain = 0.0f;
    bool m_tapeRested = false; // the tape has been told the rig is at rest (audio thread)
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> m_trimGain { 1.0f }; // Master Trim, device rate
    MasterProcessor m_master;
    // Stop: the tail rings out (render-rate samples; audio thread)
    std::int64_t m_tailElapsed = 0, m_tailQuiet = 0;
    bool m_tailDone = false;
    int m_drainLeft = 0; // device samples still to run after the fade, so the downsampler and limiters empty
    TapeRecorder m_recorder;
    static juce::PropertiesFile* s_sharedSettings;
    std::unique_ptr<juce::PropertiesFile> m_ownSettings;
    juce::PropertiesFile* m_settings = nullptr;

    juce::AudioBuffer<float> m_work;
    double m_deviceRate = 0.0, m_renderRate = 48000.0;
    int m_blockSize = 512;
    // Rendering above the device rate: the render, the band-limited downsampler that makes what you hear from
    // it, and a limiter after that (removing the top of the spectrum moves the peaks, so the ceiling is held again).
    juce::AudioBuffer<float> m_highRate;
    Downsampler m_down;
    MasterProcessor m_monitorLimiter;
    int m_recordRateOverride = -1;

    struct Raw
    {
        std::array<std::atomic<float>*, ParamIDs::numCargoTracks> tracks {}, trackSemis {}, trackCents {}, trackVari {};
        std::atomic<float>* pitch = nullptr;
        std::atomic<float>* rideOn = nullptr, *energy = nullptr, *rate = nullptr, *fill = nullptr, *chaos = nullptr;
        std::atomic<float>* bus[ParamIDs::numBuses][ParamIDs::numBusControls] {};
        std::atomic<float>* hitSense = nullptr, *hitBoost = nullptr, *hitRelease = nullptr, *hitDuck = nullptr, *hitSync = nullptr;
        std::atomic<float>* gateMode = nullptr, *hitHold = nullptr, *hitFloor = nullptr, *duckMode = nullptr, *duckReturn = nullptr,
                          *takeTurns = nullptr;
        std::atomic<float>* space = nullptr, *inflate = nullptr, *push = nullptr, *ceiling = nullptr;
        std::atomic<float>* morphOn = nullptr, *morphMode = nullptr, *morphX = nullptr, *morphY = nullptr;
        std::atomic<float>* morphWander = nullptr, *morphSpeed = nullptr, *morphShape = nullptr;
        std::atomic<float>* throwLength = nullptr, *play = nullptr, *keysMode = nullptr;
        std::atomic<float>* linkBuses = nullptr, *hitAim = nullptr;
        std::atomic<float>* passMode[ParamIDs::numBuses] {};
        std::atomic<float>* tremOn = nullptr, *tremDepth = nullptr, *tremRate = nullptr, *tremThreshold = nullptr, *tremShape = nullptr, *tremTarget = nullptr;
        std::atomic<float>* masterTrim = nullptr;
        std::atomic<float>* modType[ParamIDs::numMods] {}, *modShape[ParamIDs::numMods] {}, *modKnob[ParamIDs::numMods] {},
                            *modFollow[ParamIDs::numMods] {};
    } m_raw;

    // Modulation: the wires add offsets to the knobs' values on the way to the engine (parameters stay untouched).
    ModMatrix m_mods;
    ScopeRing m_outRing;
    struct ModTarget
    {
        juce::RangedAudioParameter* param = nullptr;
        std::atomic<float>* raw = nullptr;
    };
    std::vector<ModTarget> m_modTargets;   // aligned with ModMatrix::targets()
    ModMatrix::FollowerInputs m_followerLevels {}; // RMS of the output, bus A, bus B and the hits (the Followers' inputs)
    std::array<int, ParamIDs::numCargoTracks> m_trackPitchTarget {}; // each track's pitch as a modulation target index
    /** A parameter's value with its wires applied (audio thread, no allocation). */
    float modValue(std::atomic<float>* raw);
    void runModulators(int numSamples);
    void publishModViews(); // the knobs' live dots while stopped

    std::array<float, c_scopeSize> m_scope {};
    std::atomic<int> m_scopeWrite { 0 };
    std::atomic<float> m_outputPeak { 0.0f }, m_outputPeakHold { 0.0f };
    void publishOutputPeak(float peak, int numSamples);
    bool m_autoRecordDone = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE(SwooshbucklerAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwooshbucklerAudioProcessor)
};

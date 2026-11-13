#pragma once

#include <JuceHeader.h>
#include "Cargo.h"
#include "Downsampler.h"
#include "EditHistory.h"
#include "MasterProcessor.h"
#include "Parameters.h"
#include "TapeRecorder.h"
#include "TakeAudition.h"
#include "RigEngine.h"
#include "Modulators.h"
#include "MorphWander.h"
#include "Presets.h"
#include "UserPresets.h"

class VariationRenderer;

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
    /** New session: stops, empties every track and puts every parameter back to its default, as one undo step
        (Undo brings the tracks, the settings, the wires and the session's name back). Message thread. */
    void newSession();

    // Keeping work: undo, presets, Randomise, A/B and session files. Message thread unless said otherwise. --------
    /** One undo history for the parameters, presets, Randomise, A/B, New Session and the track edits. */
    EditHistory& getHistory() { return *m_history; }
    /** Undo / redo the latest step. `what` gets the step's name. False when there was nothing to undo / redo. */
    bool undoEdit(juce::String* what = nullptr);
    bool redoEdit(juce::String* what = nullptr);
    /** The track the last undo or redo of a track edit acted on (-1 = none), then forgets it. */
    int takeRestoredTrack() { return std::exchange(m_restoredTrack, -1); }

    // Routing: the Master track, each track's bus, and the morph's ropes. They live in the session (CargoHold);
    // these change them as undo steps. Message thread.
    /** Makes a track the Master (-1 = none): the one sound the Followers and Follow Master listen to. */
    void setMasterTrack(int trackOrMinusOne);
    /** The bus a track plays on: -1 = Auto (by its type), 0 = bus A, 1 = bus B, CargoHold::c_hitsBus = the hits. */
    void setTrackBus(int track, int busOrMinusOne);
    /** Ropes one side of the morph (0 = A, 1 = B) to a track, or back to its whole bus (-1). */
    void setMorphSource(int side, int trackOrMinusOne);
    /** Match Pitch to Master: sets the track's Semitones and Cents so the pitch its sound sits at is the Master's
        (as the Master is tuned), in the nearest octave the two-octave range reaches. One undo step. Returns a
        line for the status bar; `done` says whether anything was set. */
    juce::String matchPitchToMaster(int track, bool* done = nullptr);

    /** The settings a preset owns, as plain values: every parameter that isn't the session's. withMods adds the
        modulators' own controls; outputStage adds Inflate and Push (what the Output die rolls). */
    using Look = std::map<juce::String, float>;
    Look captureLook(bool withMods = false, bool outputStage = false) const;
    /** Puts the settings there at once, as one undo step called undoName (the editor glides instead; this is
        for A/B, tests and anything without a window). presetName: the new preset name, empty = unchanged. */
    void applyLook(const Presets::Preset& look, Presets::Unlisted unlisted, const juce::String& undoName,
                   const juce::String& presetName = {}, bool outputStage = false);
    /** The name in the preset box, recorded in the undo history (so undoing a preset brings its name back). */
    void setPresetName(const juce::String& name);

    // Randomise: the header's roll (every section that isn't locked) or one section's die, and the way back.
    /** Rolls and remembers it. section -1 = the header's Randomise. Returns the values to go to (the caller moves
        the parameters there, leaving every knob the roll doesn't name where it is). */
    Presets::Preset rollLook(int section = -1);
    static constexpr int c_maxRolls = 20;
    /** Whether there is a roll before (-1) / after (+1) the one you are on. */
    bool canStepRoll(int delta) const;
    /** Steps back or forward through the last c_maxRolls rolls and the settings before the first of them. How
        you left the one you step away from is kept. Returns the settings to go to (its name is the preset name
        to show), or null when there is nothing that way. */
    const Presets::Preset* stepRoll(int delta);
    int getRollCount() const { return juce::jmax(0, static_cast<int>(m_rolls.size()) - 1); }
    int getRollPosition() const { return m_rollAt; }
    void setRollSeed(juce::int64 seed) { m_random.setSeed(seed); }
    /** A locked section is left out of the header's Randomise (its own die still rolls it). Saved with the session. */
    bool isSectionLocked(int section) const { return (m_lockedSections.load() & (1u << section)) != 0; }
    void setSectionLocked(int section, bool locked);
    unsigned getLockedSections() const { return m_lockedSections.load(); }

    // Your own presets: files in a folder beside the settings (see UserPresets).
    juce::File getPresetFolder() const;
    std::vector<juce::File> getUserPresets() const { return UserPresets::list(getPresetFolder()); }
    /** Saves the settings as they are under this name (an existing preset of that name is replaced). withMods:
        the modulators and their wires are saved too. */
    bool saveUserPreset(const juce::String& name, bool withMods, juce::File& written, juce::String& error);
    /** Loads a preset file's wires (when it has them) as part of the undo step in progress; the caller moves the
        parameters. */
    void applyPresetWires(const UserPresets::Loaded& loaded);

    // A/B: two sets of the settings a preset owns. You are always on one; the other waits as you left it.
    int getABSlot() const { return m_abSlot; }
    /** Goes to the other set (it starts as a copy of this one), at once, as one undo step. */
    void switchAB();
    /** Copies the settings as they are to the other set. */
    void copyABToOther();

    // Session files: the whole state, audio included, in one file.
    static constexpr const char* c_sessionExtension = ".swooshsession";
    bool saveSession(const juce::File& file, juce::String& error);
    bool openSession(const juce::File& file, juce::String& error);
    /** The file this session was last saved to or opened from (none for a new session). Any thread. */
    juce::File getSessionFile() const;
    /** The session files opened or saved lately that still exist, newest first (the settings file). */
    juce::Array<juce::File> getRecentSessions() const;
    void clearRecentSessions();
    /** Whether these bytes are a saved state (either form: the binary one written now, or the XML one before it). */
    static bool readState(const void* data, int sizeInBytes, juce::ValueTree& state);

    /** Swell: while held, every track's fader rides up (button, space bar, MIDI notes). Any thread. */
    void setSwell(bool held) { m_uiSwell.store(held); }
    /** Is Swell held right now (the sea raises its swell while it is). Any thread. */
    bool isSwellHeld() const { return m_uiSwell.load(); }

    /** Load Demo Sounds: makes the four demo sounds (see DemoSounds) and puts them in the first empty tracks, as
        one undo step. Returns how many were loaded (0 when every track is in use). Message thread. */
    int loadDemoSounds();

    // MIDI control: a track's fader, or Energy, follows a MIDI controller (CC) once it has been learned. There is
    // no mapping until you make one; the mappings are kept in the settings file (the same in every session).
    static constexpr int c_ccEnergy = ParamIDs::numCargoTracks;      // the targets: 0..9 the track faders, then Energy
    static constexpr int c_numCcTargets = ParamIDs::numCargoTracks + 1;
    /** "Track 3 Fader" / "Energy". */
    static juce::String midiTargetName(int target);
    /** The next controller that moves is learned for this target (-1 = stop waiting). Message thread. */
    void learnMidiControl(int target);
    /** The target waiting for a controller (-1 = none). Any thread. */
    int getMidiLearnTarget() const { return m_ccLearn.load(std::memory_order_relaxed); }
    /** The controller number a target follows (-1 = none). Any thread. */
    int getMidiControl(int target) const;
    /** Sets a target's controller (-1 = none) and saves the mappings. Message thread. */
    void setMidiControl(int target, int ccOrMinusOne);
    /** Puts the controller moves the audio thread has seen into the parameters, and saves a mapping that has just
        been learned. The processor does it by itself 60 times a second while there is anything to follow; tests
        call it. Returns the target that was just learned (-1 = none). Message thread. */
    int applyMidiControls();
    /** Goes up whenever a mapping changes (learned, forgotten). */
    int getMidiControlGeneration() const { return m_ccGeneration; }

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
    /** How many files the tape has kept since it was last armed (One File per Whoosh makes many). Message thread. */
    int getFilesThisTake() const { return m_filesThisTake; }
    /** A file the tape could not open while rolling: the message, once (empty = none). */
    juce::String takeRecordingError() { return m_recorder.takeWriteError(); }

    // Takes: the recordings of this run (tape takes, whoosh files, LAST 60s saves, and whatever else registers
    // itself), newest first, at most c_maxTakes. Message thread unless said otherwise. --------------------------
    struct Take
    {
        int id = 0;
        juce::File file;
        juce::String kind;      // "Tape", "Whoosh", "Last 60 s", ...
        double seconds = 0.0;
        int markers = 0;
        bool starred = false;   // kept: it stays in the list when newer takes push the others out
    };
    static constexpr int c_maxTakes = 20;
    /** Adds a file to the takes (any thread: off the message thread it is added a moment later). kind is the short
        tag shown beside it; seconds < 0 = read the length from the file. */
    void registerTake(const juce::File& file, const juce::String& kind, double seconds = -1.0, int markers = 0);
    const std::vector<Take>& getTakes() const { return m_takes; }
    /** Puts the files the tape has closed (and Render Variations has written) since the last look into the takes.
        The processor does it by itself four times a second and when the tape stops; this is for looking sooner. */
    void refreshTakes();
    /** Goes up whenever the list changes. */
    int getTakesGeneration() const { return m_takesGeneration; }
    void setTakeStarred(int id, bool starred);
    /** Moves the file to the system trash and takes it off the list. False if the file could not be moved.
        toTrash false deletes the file outright (tests: nothing is left in the trash). */
    bool deleteTake(int id, bool toTrash = true);
    /** Plays a take through the output, on top of the rig (it is not recorded). Click again, or another, to stop. */
    bool auditionTake(int id, juce::String& error);
    void stopAudition() { m_audition.stop(); }
    /** The take being played (-1 = none), and how far through it is (0..1). */
    int getAuditionTake() const;
    double getAuditionProgress() const { return m_audition.getProgress(); }

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
    /** One File per Whoosh (off unless switched on): the armed tape closes its file when a whoosh has ended and
        opens the next for the next one. An open take is closed and armed again when this changes. */
    bool getOneFilePerWhoosh() const;
    void setOneFilePerWhoosh(bool on);
    /** The <Name> of recorded files. Empty (the default): the preset's name. */
    juce::String getTakeName() const;
    void setTakeName(const juce::String& name);
    /** The name files get right now: the Name setting, or the preset (Swooshbuckler for the default one). */
    juce::String getEffectiveTakeName() const;
    /** Who the files say made them. Empty (the default): Samuel Justice. */
    juce::String getCreator() const;
    void setCreator(const juce::String& creator);
    /** 0 = <Name>_0001 (a counter), 1 = <Name>_<date>_<time>. */
    int getNameStyle() const;
    void setNameStyle(int style);
    /** How much LAST 60s keeps: 1, 3 or 5 minutes. */
    int getRetroMinutes() const;
    void setRetroMinutes(int minutes);
    /** The description written into every recorded file: version, preset and the main settings. Message thread. */
    juce::String describeSettings() const;

    // Render Variations ---------------------------------------------------------
    /** Renders batches of thrown whooshes straight to files on its own thread, through a second rig (see
        VariationRenderer). Made the first time it is asked for. Message thread. */
    VariationRenderer& getVariations();
    /** What the Render Variations panel was last set to (the settings file). */
    int getVariationCount() const;                // 1 .. 1000, 100 unless changed
    void setVariationCount(int count);
    int getVariationMode() const;                 // VariationRenderer::Mode
    void setVariationMode(int mode);
    float getVariationAmount() const;             // Small Changes: 0 .. 1
    void setVariationAmount(float amount);
    /** The folder batches go to; empty (the default) = a new dated folder inside the recordings folder each time. */
    juce::String getVariationFolder() const;
    void setVariationFolder(const juce::String& path);

    /** A second rig for rendering offline: the same parameters, wires, mixer and samples as this one right now
        (the sample data is shared, not copied), stopped, with no tape, no timers and nothing saved to the
        settings. It lends from this rig's Cargo Hold: call getCargo().endLending() once the copy has been
        destroyed. Make and destroy it on the message thread; in between one other thread may drive it. */
    std::unique_ptr<SwooshbucklerAudioProcessor> makeOfflineCopy();
    bool isOffline() const { return m_offline; }
    /** Offline copies only: everything starts again from silence at this sample rate (512-sample blocks), with
        every random source drawn from `seed`, Master Trim out of the way (files get the level the tape gets) and
        the rig stopped. The same seed and settings give the same samples. The thread that calls processBlock. */
    void prepareOffline(double sampleRate, std::uint32_t seed);
    /** The rig is stopped, has rung out and has faded: the next block would be silence. The thread that calls
        processBlock, after a block. */
    bool isAtRest() const { return ! m_playing.load() && m_uiThrows.load() == 0 && m_transportGain <= 0.0f && m_drainLeft <= 0; }

    /** The settings file to use instead of opening our own: the standalone app shares its file (two objects on
        one file would overwrite each other's values), and tests point it at a scratch file. Not owned; set it
        before the processor is made and clear it after the processor is gone. Message thread. */
    static void useSettings(juce::PropertiesFile* shared) { s_sharedSettings = shared; }
    /** The settings file this rig reads and writes (its own, the standalone app's, or a test's). It may be
        nullptr. For the few things the interface keeps for itself, such as the keys. Message thread. */
    juce::PropertiesFile* getSettingsFile() const { return m_settings; }

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
    /** offlineSettings: null for a real plug-in; the settings an offline copy reads (see makeOfflineCopy). */
    explicit SwooshbucklerAudioProcessor(juce::PropertiesFile* offlineSettings);
    const bool m_offline;
    std::unique_ptr<VariationRenderer> m_variations;
    void timerCallback() override;
    void updateEngineParams();
    void prepareInternal();
    void renderAtRenderRate(const float* inL, const float* inR, float* l, float* r, int n);
    void applyRestoredState(const juce::ValueTree& cargo, int editorWidth, const juce::String& presetName); //
    void restoreStateTree(juce::ValueTree tree);
    void noteRecentSession(const juce::File& file);

    // Keeping work (message thread, except where a lock or an atomic says otherwise)
    std::unique_ptr<EditHistory> m_history;
    int m_restoredTrack = -1;
    struct Roll
    {
        Look look;
        juce::String name;
    };
    std::vector<Roll> m_rolls;      // [0] is the settings before the first roll
    int m_rollAt = -1;
    Presets::Preset m_rollTarget;   // what stepRoll hands out
    juce::Random m_random;
    std::atomic<unsigned> m_lockedSections { 0 };
    std::array<Look, 2> m_ab;       // guarded by m_stateLock (a host may save from another thread)
    std::array<juce::String, 2> m_abNames;
    int m_abSlot = 0;               // guarded by m_stateLock for writes
    juce::String m_sessionPath;     // guarded by m_stateLock
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
    // MIDI control. The audio thread reads m_ccOf, learns into it when m_ccLearn is set, and leaves the latest
    // position of each target's controller in m_ccValue (-1 = nothing new); the message thread moves the parameters.
    std::array<std::atomic<int>, c_numCcTargets> m_ccOf;
    std::array<std::atomic<float>, c_numCcTargets> m_ccValue;
    std::atomic<int> m_ccLearn { -1 }, m_ccLearned { -1 };
    int m_ccGeneration = 0;
    juce::RangedAudioParameter* midiTargetParameter(int target) const;
    void saveMidiControls();
    struct CcPump : juce::Timer
    {
        explicit CcPump(SwooshbucklerAudioProcessor& p) : owner(p) {}
        void timerCallback() override { owner.applyMidiControls(); }
        SwooshbucklerAudioProcessor& owner;
    } m_ccPump { *this };
    void updateCcPump();
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
        std::atomic<float>* rideFollowMaster = nullptr;
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

    // Takes (message thread)
    std::vector<Take> m_takes;
    int m_takesGeneration = 0, m_nextTakeId = 1, m_filesThisTake = 0;
    void updateTapeNaming();   // the name and description the tape gives its next file
    TakeAudition m_audition;

    JUCE_DECLARE_WEAK_REFERENCEABLE(SwooshbucklerAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwooshbucklerAudioProcessor)
};

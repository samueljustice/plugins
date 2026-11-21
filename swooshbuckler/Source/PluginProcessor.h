#pragma once

#include <JuceHeader.h>
#include "Cargo.h"
#include "Downsampler.h"
#include "EditHistory.h"
#include "MasterProcessor.h"
#include "Room.h"
#include "RoomMaster.h"
#include "Parameters.h"
#include "TapeRecorder.h"
#include "TakeAudition.h"
#include "SumGuard.h"
#include "RigEngine.h"
#include "Modulators.h"
#include "MorphWander.h"
#include "OutputEq.h"
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
class SwooshbucklerAudioProcessor : public juce::AudioProcessor, private juce::Timer,
                                    private juce::AudioProcessorValueTreeState::Listener
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

    /** Stowaway's switch (ParamIDs::stowOn). Off, the bus is silent and the tracks sent to it wait. Any thread. */
    bool isStowawayOn() const;
    /** Switches Stowaway on or off as one host gesture (and one undo step). Message thread. */
    void setStowawayOn(bool on);
    /** Stowaway's Mode (ParamIDs::StowMode). Any thread. */
    int getStowawayMode() const { return stowMode(); }
    /** Message thread. Tells the host to read the parameters' texts again if Stowaway's Mode has changed since it
        last did (Filter and Width read by the Mode). The processor's own timer calls it four times a second. */
    void refreshHostReadouts();
    /** Stowaway's plate in the window: -1 not opened yet (it opens by itself the first time Stowaway is switched
        on), 0 folded, 1 open. Saved with the session. */
    int getStowawayPanel() const { return m_stowPanel.load(); }
    void setStowawayPanel(int state) { m_stowPanel.store(juce::jlimit(-1, 1, state)); }

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

    /** Swell: while held, every track's fader rides up. This one is a hold the host does not record (a key held, a
        pulse from the window). Any thread. */
    void setSwell(bool held) { m_uiSwell.store(held); }
    /** The Swell host parameter (ParamIDs::swell), written as a gesture so a DAW records the press: begins on
        true, ends on false. The button, its key and a click on the sea use this. Message thread. */
    void setSwellParameter(bool held);
    /** What holds Swell right now: a bit for each (any thread). The parameter, a press the host does not record
        (setSwell), MIDI (a note with Keys set to Swell, or the sustain pedal, CC64, in every Keys mode), a wire
        (ModMatrix::swellGate) and Auto Swell (the engine's). */
    enum SwellSource { SwellFromParameter = 1, SwellFromPress = 2, SwellFromMidi = 4, SwellFromWire = 8, SwellFromAuto = 16 };
    int getSwellSources() const;
    /** "Swell now": anything holds it (the button's squash, its lamp and the sea read this). Any thread. */
    bool isSwellHeld() const { return getSwellSources() != 0; }

    // Faders Together, Varispeed and the Output EQ, for the window. Any thread.
    /** Seconds to the next peak of Faders Together's swell, < 0 when the faders are not together (RigEngine). */
    float getCyclePeakIn() const { return m_engine.getCyclePeakIn(); }
    /** A bus's held Varispeed offset (-1 .. 1 of its travel either side; 0 unless its Swirl is a Tape Flanger). */
    float getVarispeedOffset(int bus) const { return m_engine.getVarispeedOffset(bus); }
    /** The offset meter was double-clicked: that bus's held offset slides home. */
    void recentreVarispeed(int bus) { m_varispeedHome[static_cast<size_t>(juce::jlimit(0, ParamIDs::numBuses - 1, bus))].store(true); }
    /** The Output EQ's settings as its parameters stand (the cuts' parked end stops as 0 Hz, Off). */
    OutputEq::Params getOutputEqParams() const;
    /** The Output EQ does something: on, and not flat. */
    bool isOutputEqActive() const { return ! OutputEq::isFlat(getOutputEqParams()); }
    /** For a file's description: each bus's Swirl and Filter detail that is off its default, and the Output EQ when
        it does something (empty when neither). Any thread that may read the parameters. */
    juce::String describeDetail() const;

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
    /** Sound the tape's writer thread has been handed and not written yet (it may fall 3 s behind). Any thread. */
    double getTapeBacklogSeconds() const { return m_recorder.getBacklogSeconds(); }
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
        juce::File file;        // the first of its files: the one shown and played from
        juce::Array<juce::File> files; // every file of it (a Multi-Mono set has one per channel), in processing order
        takefile::Format format; // its width, Channel Order and File Layout, as its files say
        std::optional<room::Width> foldedFrom; // a fold-down of a wider room: the width it was played at
        juce::String kind;      // "Tape", "Whoosh", "Last 60 s", "Variation", "Variations", ...
        double seconds = 0.0;
        int markers = 0;
        bool starred = false;   // kept: it stays in the list when newer takes push the others out
    };
    static constexpr int c_maxTakes = 20;
    /** Adds a file to the takes (any thread: off the message thread it is added a moment later). kind is the short
        tag shown beside it; seconds < 0 = read the length from the file. */
    void registerTake(const juce::File& file, const juce::String& kind, double seconds = -1.0, int markers = 0,
                      std::optional<room::Width> foldedFrom = std::nullopt);
    /** What the Takes list says a take's width is: "7.1.4", "Stereo", or for a fold-down "Stereo Fold". */
    static juce::String widthTag(const Take& take);
    const std::vector<Take>& getTakes() const { return m_takes; }
    /** Puts the files the tape has closed (and Render Variations has written) since the last look into the takes.
        The processor does it by itself four times a second and when the tape stops; this is for looking sooner. */
    void refreshTakes();
    /** Goes up whenever the list changes. */
    int getTakesGeneration() const { return m_takesGeneration; }
    void setTakeStarred(int id, bool starred);
    /** Moves its files to the system trash and takes it off the list. False if a file could not be moved.
        toTrash false deletes the file outright (tests: nothing is left in the trash). */
    bool deleteTake(int id, bool toTrash = true);
    /** Plays a take through the output, on top of the rig (it is not recorded). Click again, or another, to stop. */
    bool auditionTake(int id, juce::String& error);
    void stopAudition() { m_audition.stop(); }
    /** Reads the auditioned take ahead now (see TakeAudition::topUp). Not the audio thread. */
    void topUpAudition() { m_audition.topUp(); }
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
    /** The width of the tape's files (and the saved last minutes'): the room as played (the default), or folded
        down to Stereo (a room wider than Stereo) or to Mono. */
    enum class TapeFold { AsPlayed = 0, Stereo, Mono };
    TapeFold getTapeFold() const;
    void setTapeFold(TapeFold fold);
    /** The width the tape's files have right now: the room's, or the fold chosen when it is narrower. */
    room::Width getTapeWidth() const;
    /** Channel Order of interleaved WAV and AIFF files (Like Pro Tools unless chosen otherwise; the two differ only
        at 7.1.4), and File Layout
        (Interleaved unless chosen otherwise): for the tape, the saved last minutes and Render Variations (one
        setting, shown in Settings and in the Render Variations panel). */
    room::FileOrder getChannelOrder() const;
    void setChannelOrder(room::FileOrder order);
    takefile::Layout getFileLayout() const;
    void setFileLayout(takefile::Layout layout);
    /** How the tape and the saved last minutes write files now: the settings above, the file type, bit depth and
        rate, the room it is played in and the Ceiling for a fold of a wider room. */
    TapeRecorder::Options tapeOptions() const;
    /** How much the saved last minutes really hold at this width and rate (the memory for them is capped). */
    double getRetroSecondsKept() const { return m_recorder.getRetroCapacitySeconds(); }
    /** The memory the saved last minutes have taken: none until the first Play, arm or sound. Message thread. */
    std::int64_t getRetroBytes() const { return m_recorder.getRetroBytes(); }
    /** The Last 60 s memory holds something that a change of Speakers (a re-prepare at another width) would empty. */
    bool hasRetro() const { return m_recorder.hasRetro(); }
    /** "Last 60 s", "Last 3 Min", or the real length when the memory cap holds less: "Last 55 s", "Last 3:42". */
    juce::String retroLabel() const;
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
    /** The description written into every recorded file: version, the room (wider than Stereo), preset and the main
        settings, the most telling first (a WAV file's BWF description keeps the first 256 bytes). Message thread. */
    juce::String describeSettings() const;
    /** The room's part of a description, for a width wider than Stereo (empty otherwise): "Flight Paths A Overhead,
        B Across, Stowaway Around. Spread 40%, Height 100%, Centre Speaker Off, LFE Off." Any thread that may read
        the parameters (an offline copy's own render thread). */
    juce::String describeRoom(room::Width width) const;

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
    /** Everything else the panel keeps between runs (which choice each menu is on, the batches rendered lately,
        how fast the last batch went), each under a key of its own in the settings file. */
    juce::String getVariationValue(const juce::String& key, const juce::String& fallback = {}) const;
    void setVariationValue(const juce::String& key, const juce::String& value);

    /** A second rig for rendering offline: the same parameters, wires, mixer and samples as this one right now
        (the sample data is shared, not copied), stopped, with no tape, no timers and nothing saved to the
        settings. It lends from this rig's Cargo Hold: call getCargo().endLending() once the copy has been
        destroyed. Make and destroy it on the message thread; in between one other thread may drive it. */
    std::unique_ptr<SwooshbucklerAudioProcessor> makeOfflineCopy();
    bool isOffline() const { return m_offline; }
    /** Offline copies only: everything starts again from silence at this sample rate (512-sample blocks), with
        every random source drawn from `seed`, Master Trim out of the way (files get the level the tape gets) and
        the rig stopped, playing in a room of `width` (its output bus is made that width: Mono and Stereo use the
        stereo engine, Quad and wider the room's). The same seed, settings and width give the same samples. The
        thread that calls processBlock, with buffers of room::layout(width).numChannels channels. */
    void prepareOffline(double sampleRate, std::uint32_t seed, room::Width width = room::Width::Stereo);
    /** Offline copies only, the thread that calls processBlock, between blocks. Where every track is in its sounds
        and where each fader's ride is, so the next render of a batch can carry on from there (see carryOn). */
    RigEngine::Places getPlaces() const { return m_engine.getPlaces(); }
    /** Offline copies only, after prepareOffline (and after setPlaying, whose return to the start markers it takes
        the place of): the tracks carry on from these places, and the rides from theirs. */
    void carryOn(const RigEngine::Places& places);
    /** Offline copies only, after prepareOffline: every modulator, and every fader's ride, from a moment drawn
        from `seed` (see ModMatrix::scatter and RigEngine::scatterRide). */
    void scatter(std::uint32_t seed);
    /** Offline copies only: where Faders Together's clock starts (RigEngine::setCycleStart). */
    void setCycleStart(double phase) { jassert(m_offline); m_engine.setCycleStart(phase); }
    /** Offline copies only: throws one whoosh that flies as `flight` says (the Throw button's flies as
        RigEngine::ThrowFlight is made). The thread that calls processBlock. */
    void throwWhoosh(const RigEngine::ThrowFlight& flight)
    {
        jassert(m_offline);
        m_offlineFlight = flight;
        m_uiThrows.fetch_add(1);
    }
    /** The rig is stopped, has rung out and has faded: the next block would be silence. The thread that calls
        processBlock, after a block. */
    bool isAtRest() const { return ! m_playing.load() && m_uiThrows.load() == 0 && m_transportGain <= 0.0f && m_drainLeft <= 0; }

    /** The settings file to use instead of opening our own: the standalone app gives its rig the object it uses
        itself, and tests point it at a scratch file. Not owned; set it
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

    // The room ------------------------------------------------------------------
    /** How wide the output is. In a plug-in host it is the track's width (the output bus); in the app it is the
        Listening setting (AppOutput sets the bus to it). Set when the processor is prepared. Any thread. */
    room::Width getRoomWidth() const { return static_cast<room::Width>(m_roomWidth.load(std::memory_order_relaxed)); }

    // Listening (the app) --------------------------------------------------------
    /** The app itself, not a plug-in in a host and not Render Variations' offline copy. */
    bool isApp() const { return wrapperType == wrapperType_Standalone && ! m_offline; }
    /** What the app plays in: Mono, Stereo, Quad, 5.1, 7.1, 7.1.2 or 7.1.4 (settings key "listening", Stereo on a
        fresh install). The app's rig takes this width and no other (isBusesLayoutSupported). In a host the track
        decides and this is not used. Any thread. */
    room::Width getListening() const { return static_cast<room::Width>(m_listening.load(std::memory_order_relaxed)); }
    /** Chooses it and remembers it; then onListeningChanged (the app fades the sound out, opens the outputs the
        room needs and prepares the rig again at the new width). Refused (false) in a host and while the tape
        records: a take is written at the room's width. Message thread. */
    bool setListening(room::Width width);
    /** Set by the app (AppAudio); called by setListening on the message thread. */
    std::function<void()> onListeningChanged;
    /** The app's audio device: how many outputs are switched on (0: no device playing) and how many it has.
        Set by AppOutput when it is prepared and by AppAudio when the device changes. Any thread. */
    void setDeviceOutputs(int switchedOn, int available);
    int getDeviceOutputs() const { return m_deviceOutputs.load(std::memory_order_relaxed); }
    int getDeviceOutputsAvailable() const { return m_deviceOutputsAvailable.load(std::memory_order_relaxed); }
    /** The app plays a fold-down of the room: the device has fewer outputs switched on than the room needs. */
    bool isListeningFolded() const { return isApp() && listeningFolded(getListening(), getDeviceOutputs()); }
    static bool listeningFolded(room::Width width, int deviceOutputs);
    /** The status line when the app plays a fold-down ("Playing 7.1.4 folded to stereo: this audio device has 2
        outputs."), empty otherwise and in a host. */
    juce::String listeningStatus() const;
    static juce::String listeningStatus(room::Width width, int deviceOutputs, int available);
    /** The line under the Listening choice in Settings: where the speakers go, or the fold and why. */
    static juce::String listeningLine(room::Width width, int deviceOutputs, int available);
    /** The width has an LFE (5.1 and wider): LFE Mode and LFE Level do something. Mono, Stereo and Quad ignore them
        (the room strip dims them there). */
    bool roomHasLfe() const { return room::hasLfe(getRoomWidth()); }
    /** How many speaker meters the width has (room::layout(getRoomWidth()).numChannels), in its processing order. */
    int getMeterChannels() const { return room::layout(getRoomWidth()).numChannels; }
    /** One speaker's level, as getOutputPeak is for the whole output (after the limiter, before Master Trim);
        0 for a channel the width does not have. */
    float getChannelPeak(int channel) const;
    /** The highest level of that speaker since the last call, then starts again (one reader per channel). */
    float takeChannelPeak(int channel);
    /** The LFE's own limiter: its deepest reduction in the last block (dB, <= 0; 0 without an LFE). */
    float getLfeGainReductionDb() const { return m_master.getLfeGainReductionDb(); }

    /** What the LFE is fed, for whatever shows it (the room strip, Render Variations).
          NoLfe       the width has no LFE (Mono, Stereo, Quad)
          Off         LFE Mode is Off: the LFE is silent because it was asked to be
          Mixdown     the low end of the whole mix
          Sub         Discrete, and Stowaway's Sub is there to play: Stowaway is on, in Sub, with a track sent to it
          NoSub       Discrete, but Stowaway is off or not in Sub: NOTHING FEEDS THE LFE
          NoSubTrack  Discrete, Stowaway on in Sub, but no loaded track plays on it: NOTHING FEEDS THE LFE
        Discrete never falls back to anything else (the Sub is the only thing it was asked to play, and the low
        end of a whole mix arriving in the LFE unasked is worse than silence): it stays silent and says so here. */
    enum class LfeFeed { NoLfe, Off, Mixdown, Sub, NoSub, NoSubTrack };
    /** For a rig that is not this one's live state (a batch about to be rendered at another width). */
    static LfeFeed lfeFeedFor(room::Width width, int lfeMode, bool stowawayOn, int stowawayMode, bool trackOnStowaway);
    /** This rig's, as its parameters and its Cargo Hold stand. Any thread. */
    LfeFeed getLfeFeed() const { return getLfeFeedAt(getRoomWidth()); }
    /** The same at another width (Render Variations renders at the width it is given). Any thread. */
    LfeFeed getLfeFeedAt(room::Width width) const;
    /** LFE Mode is Discrete or Mixdown and yet nothing reaches the LFE. */
    static bool lfeIsUnfed(LfeFeed feed) { return feed == LfeFeed::NoSub || feed == LfeFeed::NoSubTrack; }
    /** Why nothing reaches the LFE and what to do about it, in a sentence; empty when it is fed, Off, or the width
        has none. */
    static juce::String lfeFeedReason(LfeFeed feed);
    /** A loaded track plays on Stowaway (sent there by hand, or a Rumble on Auto while Stowaway is on in Sub). */
    bool hasTrackOnStowaway() const;
    /** The main speakers' low end is being kept in step with the LFE (the LFE carries a copy of what they carry):
        see RigEngine::lfeCarriesMains. */
    bool lfeCarriesMains() const { return m_engine.lfeCarriesMains(); }

    static constexpr int c_scopeSize = 1 << 13;
    static constexpr int c_maxSection = 512; // the samples between parameter and modulator updates (a multiple of c_quantum)
    static constexpr int c_quantum = 32;     // the rig is rendered this many samples at a time, whatever the host's block size (see processBlock)
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
    /** Renders n samples into the room's pipe (m_pipeChannels channels, see RoomMaster) at the render rate. */
    void renderAtRenderRate(const float* inL, const float* inR, float* const* chans, int n);
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
    std::atomic<int> m_stowPanel { -1 }; // see getStowawayPanel
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

    std::shared_ptr<Params::StowModeSource> m_stowModeSource = std::make_shared<Params::StowModeSource>(); // before m_state: its layout reads it
    juce::AudioProcessorValueTreeState m_state;
    CargoHold m_cargo;
    RigEngine m_engine;
    std::atomic<bool> m_uiSwell { false };
    std::atomic<bool> m_midiSwell { false }; // a note with Keys set to Swell, or the sustain pedal (audio thread writes)
    bool m_sustain = false;                  // CC64 is down (audio thread)
    bool m_swellGesture = false;             // setSwellParameter has begun a gesture (message thread)
    std::array<std::atomic<bool>, ParamIDs::numBuses> m_varispeedHome {};
    OutputEq m_outputEq;                     // after the engine, before the output stage (renderAtRenderRate)
    /** Whether MIDI holds Swell now (m_keysDown, m_sustain and Keys); keeps m_midiSwell up to date. Audio thread. */
    bool updateMidiSwell();
    std::atomic<bool> m_playing { false }, m_restartPending { false };
    // The Play parameter (host automation): a change of it starts or stops the rig. m_playParamWas is its value
    // in the last block (audio thread).
    bool m_playParamWas = false;
    std::atomic<bool> m_followHost { true };
    std::atomic<bool> m_stowReadoutsStale { false }; // Stowaway's Mode changed: the timer tells the host to read the knobs' texts again
    std::atomic<bool> m_hostStopped { false }; // the transport stopped the rig: the timer puts the Play parameter back
    bool m_hostWasPlaying = false;   // audio thread
    bool m_reseedPending = false;    // audio thread: the transport started, begin again from the seed
    std::atomic<int> m_uiThrows { 0 };
    RigEngine::ThrowFlight m_offlineFlight; // how those throws fly: as made, except in an offline copy (throwWhoosh(flight))
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
    // The saved last minutes' memory, when the rig is first played from the audio thread (a host's transport, a
    // key): the audio thread raises a flag at the first sound and this takes the memory within a fiftieth of a
    // second, so only the very start of that first fade-in is missed. It stops once the memory is taken.
    struct RetroPump : juce::Timer
    {
        explicit RetroPump(SwooshbucklerAudioProcessor& p) : owner(p) {}
        void timerCallback() override;
        SwooshbucklerAudioProcessor& owner;
    } m_retroPump { *this };
    void updateCcPump();
    int keysMode() const;
    int stowMode() const; // ParamIDs::StowMode
    bool rumblesRideStowaway() const { return isStowawayOn() && stowMode() == ParamIDs::StowSub; }
    void parameterChanged(const juce::String& parameterID, float newValue) override; // any thread
    // Morph wander (audio thread): the dot's own flight, blended in over the X / Y controls.
    MorphWander m_wander;
    bool m_wanderOn = false;
    float m_wanderBlend = 0.0f, m_morphX = 0.5f, m_morphY = 0.5f;
    std::atomic<float> m_morphViewX { 0.5f }, m_morphViewY { 0.5f }, m_morphViewRotation { 0.0f };
    void runWander(int numSamples);
    float m_transportGain = 0.0f;
    bool m_tapeRested = false; // the tape has been told the rig is at rest (audio thread)
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> m_trimGain { 1.0f }; // Master Trim, device rate
    RoomMaster m_master;
    // Stop: the tail rings out (render-rate samples; audio thread)
    std::int64_t m_tailElapsed = 0, m_tailQuiet = 0;
    bool m_tailDone = false;
    int m_drainLeft = 0; // device samples still to run after the fade, so the downsampler and limiters empty
    TapeRecorder m_recorder;
    static juce::PropertiesFile* s_sharedSettings;
    std::unique_ptr<juce::PropertiesFile> m_ownSettings;
    juce::PropertiesFile* m_settings = nullptr;
    /** Writes one setting: into the shared file as it is on disk now, nothing else (SettingsFile.h). */
    void storeSetting(const juce::String& key, const juce::var& value);

    // The room (set in prepareInternal, while nothing is processed). The pipe is the width the audio is carried
    // at, Mono carried as Stereo and folded at the output (see RoomMaster).
    std::atomic<int> m_roomWidth { static_cast<int>(room::Width::Stereo) };
    std::atomic<int> m_listening { static_cast<int>(room::Width::Stereo) }; // the app's Listening (from the settings)
    std::atomic<int> m_deviceOutputs { 0 }, m_deviceOutputsAvailable { 0 };  // the app's audio device
    room::Width m_width = room::Width::Stereo;
    int m_pipeChannels = 2;
    std::array<float, room::c_maxChannels> m_loudnessWeight {}; // per pipe channel, BS.1770 (room::loudnessWeight)
    std::array<float, room::c_maxChannels> m_monoFold {};       // per pipe channel: its share of the mono scope
    std::array<std::atomic<float>, room::c_maxChannels> m_channelPeak {}, m_channelPeakHold {};
    void publishChannelPeaks(const float* peaks, int numSamples);

    juce::AudioBuffer<float> m_work;
    int m_carry = 0;        // samples of the last quantum (the end of m_work's first c_quantum) the host has not had yet
    int m_sinceControl = 0; // samples until the modulators and parameters are next updated (0: now)
    double m_followSquares = 0.0; // the output's power since that update, for the Output Follower
    int m_followSamples = 0;
    bool m_keysMoved = false; // a key arrived since the parameters were last handed to the engine
    double m_deviceRate = 0.0, m_renderRate = 48000.0;
    int m_blockSize = 512;
    // Rendering above the device rate: the render, the band-limited downsampler that makes what you hear from
    // it, and a limiter after that (removing the top of the spectrum moves the peaks, so the ceiling is held again).
    juce::AudioBuffer<float> m_highRate;
    Downsampler m_down;
    RoomMaster m_monitorLimiter;
    int m_recordRateOverride = -1;

    struct Raw
    {
        std::array<std::atomic<float>*, ParamIDs::numCargoTracks> tracks {}, trackSemis {}, trackCents {}, trackVari {};
        std::atomic<float>* pitch = nullptr;
        std::atomic<float>* rideOn = nullptr, *energy = nullptr, *rate = nullptr, *fill = nullptr, *chaos = nullptr;
        std::atomic<float>* rideMode = nullptr;
        std::atomic<float>* togetherPeakAt = nullptr, *togetherStagger = nullptr, *togetherHitEvery = nullptr;
        std::atomic<float>* swell = nullptr, *swellAuto = nullptr, *swellEvery = nullptr, *swellHold = nullptr, *swellChance = nullptr, *swellLoose = nullptr;
        std::atomic<float>* busDetail[ParamIDs::numBuses][ParamIDs::numBusDetails] {};
        std::atomic<float>* eq[ParamIDs::numEqParams] {};
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
        std::atomic<float>* flightPath[ParamIDs::numBuses] {}, *stowFlightPath = nullptr;
        std::atomic<float>* roomSpread = nullptr, *roomHeight = nullptr, *roomCentre = nullptr;
        std::atomic<float>* direction[ParamIDs::numBuses] {}, *side[ParamIDs::numBuses] {}, *stowDirection = nullptr, *stowSide = nullptr;
        std::atomic<float>* roomHeightVaries = nullptr;
        std::atomic<float>* hitPosition = nullptr, *hitSpread = nullptr;
        std::atomic<float>* roomLfeMode = nullptr, *roomLfeLevel = nullptr;
        std::atomic<float>* stowOn = nullptr, *stowMode = nullptr, *stow[ParamIDs::numStowControls] {};
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
    int m_directionTarget[ParamIDs::numBuses + 1] {}, m_sideTarget[ParamIDs::numBuses + 1] {}; // Direction and Side as modulation targets (bus A, bus B, Stowaway)
    /** A parameter's value with its wires applied (audio thread, no allocation). */
    float modValue(std::atomic<float>* raw);
    void runModulators(int numSamples);
    void publishModViews();
    void publishDirectionViews(); // the knobs' live dots while stopped

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
    SumGuard m_sumGuard; // the rig and an auditioned take together, held under the Ceiling (see SumGuard)

    JUCE_DECLARE_WEAK_REFERENCEABLE(SwooshbucklerAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwooshbucklerAudioProcessor)
};

#pragma once

#include <JuceHeader.h>
#include "TapeRecorder.h"

#include <vector>

class SwooshbucklerAudioProcessor;

/**
 * Render Variations: a batch of sounds straight to files.
 *
 * What each file is (Each):
 *   One Whoosh : one thrown whoosh (the Throw of the Ride panel), rendered from silence until its echoes and Space
 *                have rung out, with the silence in front taken off and a short fade at the very end
 *   Whole Ride : a set number of seconds of the rig as it sounds while it plays (the ride, the passes, the hits and
 *                the modulators all running), taken after a pre-roll so it does not start from silence, with a
 *                short fade at each end
 * The files are written the way the tape writes its own (file type, bit depth, sample rate, mono or stereo,
 * description) and join the Takes.
 *
 * The batch is rendered faster than real time, on this object's own thread, through a second rig made from the
 * session as it is when Render is pressed (SwooshbucklerAudioProcessor::makeOfflineCopy: the same parameters, wires
 * and mixer, and the same sample data, shared rather than copied). The rig you hear is never touched: it keeps
 * playing and can be changed meanwhile, and nothing changed after Render reaches the batch.
 *
 * What makes the files differ (Mode):
 *   My Settings, New Passes    : the settings as they are; only the rig's random draws differ (which passes, how
 *                                close, which sound of each track)
 *   My Settings, Small Changes : the settings as they are, each sound-shaping control nudged by up to How Different, each
 *                                track starting a little further into its sample
 *   Randomise Everything       : a fresh Randomise for every file (Presets::roll)
 *
 * Every batch has a number (the panel never shows it as something to set: it offers "New Batch Every Time" or the
 * batches rendered lately to repeat). Every variation's random draws come from the batch's number and its place in
 * the batch, so no two are alike, and the same number, settings and samples give the same batch again.
 *
 * Where the variations go (Files): a file each, one file that holds them all with a named marker at the start of
 * each and a gap of silence between, or both. Names never write over a file that is already there: numbers carry
 * on after the highest in the folder, and the one file takes the next free name.
 *
 * Threading: start(), cancel(), the destructor and everything that reads progress belong to the message thread
 * (cancel() and the readers are safe from any thread). The second rig is made and destroyed on the message thread.
 */
class VariationRenderer : private juce::Thread, private juce::AsyncUpdater
{
public:
    static constexpr int c_maxCount = 1000;
    static constexpr double c_minSeconds = 0.3, c_maxSeconds = 6.0;         // One Whoosh: the range of Throw Length
    static constexpr double c_minRideSeconds = 1.0, c_maxRideSeconds = 120.0; // Whole Ride: how long each file is
    static constexpr double c_ridePreRollSeconds = 2.0;                     // Whole Ride: played first and thrown away
    static constexpr double c_rideFadeInSeconds = 0.01, c_rideFadeOutSeconds = 0.05;
    static constexpr double c_maxGapSeconds = 10.0;                         // One File With Markers: the silence between
    static constexpr double c_endFadeSeconds = 0.01;                        // the fade at the very end of every whoosh
    static constexpr float c_silence = 1.0e-4f;                             // -80 dBFS: under this the front of a whoosh is cut off
    static constexpr int c_maxBatches = 6;                                  // how many batches are remembered to repeat

    enum class Mode { NewPasses = 0, SmallChanges, RandomiseEverything };
    enum class Each { OneWhoosh = 0, WholeRide };
    enum class Names { Counter = 0, DateAndTime, PresetAndCounter, BatchNameAndCounter };
    enum class Files { PerVariation = 0, OneWithMarkers, Both };
    static juce::String modeName(Mode mode);
    static juce::String eachName(Each each);
    static juce::String namesName(Names names);
    static juce::String filesName(Files files);

    struct Job
    {
        int count = 100;                       // 1 .. c_maxCount
        Mode mode = Mode::NewPasses;
        float amount = 0.3f;                   // Small Changes: 0 .. 1
        Each each = Each::OneWhoosh;
        double seconds = 3.0;                  // One Whoosh: its Throw Length; the tail rings on after it
        double rideSeconds = 10.0;             // Whole Ride: the length of each file
        std::uint32_t batchSeed = 1;           // the batch's number
        juce::File folder;                     // made if it isn't there
        Names names = Names::Counter;
        juce::String name { "Swooshbuckler" }; // Counter, Date And Time: the take name
        juce::String preset { "Default" };     // Preset And Counter
        juce::String batchName;                // Batch Name And Counter
        juce::Time when;                       // Date And Time: the stamp
        Files files = Files::PerVariation;
        double gapSeconds = 0.5;               // One File With Markers: the silence between two variations
        double rate = 48000.0;                 // the rate the rig renders at, and the files' rate
        TapeRecorder::Options options;         // file type, bit depth, mono (its fileRate is not used: see rate)
        juce::String creator { "Samuel Justice" };
    };

    /** What a batch's files will be called. */
    struct Plan
    {
        juce::String stem;        // the name before the number: "Whoosh", "Whoosh_2026-11-13_1432", "Jet Passes"
        int firstNumber = 1;      // one past the highest number the folder already has under that name
        int lastNumber = 1;
        juce::File combined;      // One File With Markers / Both: the one file, "<stem>_All" or the next free "<stem>_All_2"
        juce::String firstName;   // "<stem>_001"
    };
    static Plan planOf(const Job& job);

    /** A batch that was rendered, kept so it can be rendered again. */
    struct Batch
    {
        std::uint32_t number = 0;
        juce::String name;        // what its files were called (Plan::stem)
        juce::Time when;
        int count = 100;
        Mode mode = Mode::NewPasses;
        float amount = 0.3f;
        Each each = Each::OneWhoosh;
        double seconds = 3.0, rideSeconds = 10.0;
    };
    /** The batches rendered lately, newest first (from the settings file). Message thread. */
    std::vector<Batch> recentBatches() const;
    /** "Jet Passes, 13 Nov 2026, 14:32". */
    static juce::String batchTitle(const Batch& batch);

    /** How much a batch comes to, before it is rendered. */
    struct Estimate
    {
        int files = 0;
        double soundSeconds = 0.0;   // every variation, and the gaps of the one file
        double renderSeconds = 0.0;  // going by how fast the last batch went on this machine
        double oneFileBytes = 0.0;   // One File With Markers / Both: how big the one file comes to (0 otherwise)
    };
    /** A WAV or AIFF file has no room for markers at 4 GB or more: the one file is kept under this, with room to spare
        for the estimate being short. FLAC lists its markers in its comments and has no such limit. */
    static constexpr double c_maxOneFileBytes = 0.9 * 4294967295.0;
    static bool oneFileTooBig(const Job& job, const Estimate& estimate);
    Estimate estimate(const Job& job) const;
    /** "100 files, one whoosh each: about 5 minutes of sound, ready in about 20 seconds." */
    static juce::String estimateText(const Job& job, const Estimate& estimate);
    /** "about 20 seconds", "about 5 minutes". */
    static juce::String roughly(double seconds);

    enum class State { Idle = 0, Rendering, Finished, Cancelled, Failed };
    struct Progress
    {
        State state = State::Idle;
        int done = 0, total = 0;       // variations rendered, variations asked for
        int silent = 0;                // renders that had no sound in them (nothing is written for those)
        int files = 0;                 // files written (one each, the one file that holds them all, or both)
        Each each = Each::OneWhoosh;
        Files filing = Files::PerVariation;
        juce::String currentFile;      // the variation being rendered now
        juce::String combinedFile;     // the one file that holds them all, when there is one
        juce::File folder;
        double elapsedSeconds = 0.0;
        double secondsLeft = -1.0;     // an estimate; < 0 = not known yet
        double audioSeconds = 0.0;     // how much sound has been rendered and kept
        std::uint32_t batchSeed = 0;
        juce::String error;            // Failed: why
    };

    explicit VariationRenderer(SwooshbucklerAudioProcessor& rig);
    ~VariationRenderer() override;

    /** A job for the rig as it is now: what the panel was last set to, Throw Length (or Rate's pass length) as
        the whoosh's length, the tape's file settings and name, a new dated folder (or the chosen one), the time
        now and a new batch number. Message thread. */
    Job defaultJob() const;
    /** The folder a batch gets when none has been chosen: "<name> Variations <date> <time>" in `recordings`. */
    static juce::File datedFolder(const juce::File& recordings, const juce::String& name, juce::Time when);

    /** Starts a batch. False (and why) if one is running or the folder can't be made. Message thread. */
    bool start(const Job& job, juce::String& error);
    /** Stops after the render in hand: the files already written stay, nothing half-written is left. */
    void cancel();
    bool isRendering() const { return m_rendering.load(); }
    Progress getProgress() const;
    /** The files written by the batch that is running or ran last, in order. */
    std::vector<juce::File> getFiles() const;
    /** The files written since the last call, oldest first, for the Takes (the rig collects them four times a
        second, like the tape's). */
    struct Written
    {
        juce::File file;
        double seconds = 0.0;
        int markers = 0; // the one file that holds every variation: how many
    };
    std::vector<Written> takeWritten();
    /** Blocks until the batch has ended (tests and tools; never the message thread of an app in use). */
    bool waitUntilDone(int timeoutMs) { return waitForThreadToExit(timeoutMs); }

    /** The seed of variation `index` (from 0) of a batch. */
    static std::uint32_t seedOf(std::uint32_t batchSeed, int index);
    /** "<name>_<number>": at least three digits, more when the highest number of the batch needs them. */
    static juce::String fileNameOf(const juce::String& name, int number, int highestNumber);

private:
    void run() override;
    void handleAsyncUpdate() override;
    void releaseCopy(); // message thread: the second rig goes, and the live Cargo Hold stops lending
    /** Sets the second rig up for file `index` and renders it into m_take. Returns the number of samples from
        `first` on that belong in the file (0 = there was no sound), or -1 if it was cancelled. */
    int renderOne(int index, int& first, juce::String& description);
    juce::String describeHow() const;      // "My Settings, Small Changes 30%, Batch 4711, One Whoosh, Length 3.0 s"
    juce::String describeSettings() const; // every sound-shaping control of the second rig as it is now
    void rememberBatch(const Batch& batch); // message thread
    void setProgress(const std::function<void(Progress&)>& change);

    SwooshbucklerAudioProcessor& m_rig;
    std::unique_ptr<SwooshbucklerAudioProcessor> m_copy; // made and destroyed on the message thread, driven by run()
    Job m_job;
    std::vector<juce::RangedAudioParameter*> m_params;   // every parameter of the copy,
    std::vector<float> m_base;                           // and where each was when Render was pressed
    std::vector<float> m_baseStarts;                     // the tracks' start markers likewise
    juce::AudioBuffer<float> m_take;                     // one render: a whoosh and the longest tail, or a pre-roll and a ride
    std::atomic<bool> m_rendering { false };

    mutable juce::CriticalSection m_lock;
    Progress m_progress;                                 // guarded by m_lock
    std::vector<juce::File> m_files;                     //
    std::vector<Written> m_written;                      //

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VariationRenderer)
};

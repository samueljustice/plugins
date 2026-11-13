#pragma once

#include <JuceHeader.h>
#include "TapeRecorder.h"

#include <vector>

class SwooshbucklerAudioProcessor;

/**
 * Render Variations: a batch of whooshes straight to files.
 *
 * Each file is one thrown whoosh (the Throw of the Ride panel), rendered from silence until its echoes and Space
 * have rung out, with the silence in front taken off and a short fade at the very end. The files are written the
 * way the tape writes its own (file type, bit depth, sample rate, mono or stereo, description) and join the Takes.
 *
 * The batch is rendered offline, faster than real time, on this object's own thread, through a second rig made
 * from the session as it is when Render is pressed (SwooshbucklerAudioProcessor::makeOfflineCopy: the same
 * parameters, wires and mixer, and the same sample data, shared rather than copied). The rig you hear is never
 * touched: it keeps playing and can be changed meanwhile, and nothing changed after Render reaches the batch.
 *
 * What makes the files differ (Mode):
 *   Randomise Everything : a fresh Randomise roll for every file (Presets::roll)
 *   Small Changes        : the settings as they are, each sound-shaping control nudged by up to Amount, each
 *                          track starting a little further into its sample
 *   Seed Only            : the settings as they are; only the rig's random draws differ (which passes, how close,
 *                          which sound of each track)
 * Every file gets its own seed, made from the batch seed and the file's place in the batch, so no two files are
 * alike and the same batch seed, settings and samples give the same batch again.
 *
 * Threading: start(), cancel(), the destructor and everything that reads progress belong to the message thread
 * (cancel() and the readers are safe from any thread). The second rig is made and destroyed on the message thread.
 */
class VariationRenderer : private juce::Thread, private juce::AsyncUpdater
{
public:
    static constexpr int c_maxCount = 1000;
    static constexpr double c_minSeconds = 0.3, c_maxSeconds = 6.0; // the range of Throw Length
    static constexpr double c_endFadeSeconds = 0.01;                // the fade at the very end of every file
    static constexpr float c_silence = 1.0e-4f;                     // -80 dBFS: under this the front of a render is cut off

    enum class Mode { RandomiseEverything = 0, SmallChanges, SeedOnly };
    static juce::String modeName(Mode mode);

    struct Job
    {
        int count = 100;                       // 1 .. c_maxCount
        double seconds = 3.0;                  // each whoosh's Throw Length; the tail rings on after it
        Mode mode = Mode::RandomiseEverything;
        float amount = 0.3f;                   // Small Changes: 0 .. 1
        std::uint32_t batchSeed = 1;
        juce::File folder;                     // made if it isn't there
        juce::String name { "Swooshbuckler" }; // files are <name>_001 .. <name>_<count>
        double rate = 48000.0;                 // the rate the rig renders at, and the files' rate
        TapeRecorder::Options options;         // file type, bit depth, mono (its fileRate is not used: see rate)
        juce::String creator { "Samuel Justice" };
    };

    enum class State { Idle = 0, Rendering, Finished, Cancelled, Failed };
    struct Progress
    {
        State state = State::Idle;
        int done = 0, total = 0;       // files written, files asked for
        int silent = 0;                // renders that had no sound in them (no file is written for those)
        juce::String currentFile;      // the file being rendered now
        juce::File folder;
        double elapsedSeconds = 0.0;
        double secondsLeft = -1.0;     // an estimate; < 0 = not known yet
        double audioSeconds = 0.0;     // how much sound has been written
        std::uint32_t batchSeed = 0;
        juce::String error;            // Failed: why
    };

    explicit VariationRenderer(SwooshbucklerAudioProcessor& rig);
    ~VariationRenderer() override;

    /** A job for the rig as it is now: the panel's last count, mode and amount, Throw Length (or Rate's pass
        length) as the length, the tape's file settings and name, a new dated folder (or the chosen one) and a
        new batch seed. Message thread. */
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
    };
    std::vector<Written> takeWritten();
    /** Blocks until the batch has ended (tests and tools; never the message thread of an app in use). */
    bool waitUntilDone(int timeoutMs) { return waitForThreadToExit(timeoutMs); }

    /** The seed of file `index` (from 0) of a batch. */
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
    void setProgress(const std::function<void(Progress&)>& change);

    SwooshbucklerAudioProcessor& m_rig;
    std::unique_ptr<SwooshbucklerAudioProcessor> m_copy; // made and destroyed on the message thread, driven by run()
    Job m_job;
    std::vector<juce::RangedAudioParameter*> m_params;   // every parameter of the copy,
    std::vector<float> m_base;                           // and where each was when Render was pressed
    std::vector<float> m_baseStarts;                     // the tracks' start markers likewise
    juce::AudioBuffer<float> m_take;                     // one render, at most the length plus the longest tail
    std::atomic<bool> m_rendering { false };

    mutable juce::CriticalSection m_lock;
    Progress m_progress;                                 // guarded by m_lock
    std::vector<juce::File> m_files;                     //
    std::vector<Written> m_written;                      //

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VariationRenderer)
};

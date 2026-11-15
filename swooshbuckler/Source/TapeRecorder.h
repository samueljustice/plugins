#pragma once

#include <JuceHeader.h>

#include "Room.h"
#include "TakeFile.h"

#include <atomic>

/**
 * "Captain's Log": a tape machine on the output.
 *
 *  - Rolling record: while on, everything that leaves the app streams to a WAV
 *    file (background thread via AudioFormatWriter::ThreadedWriter; the audio
 *    thread only copies into a FIFO and never blocks).
 *  - Retro capture: the last c_retroSeconds are kept in RAM, so a happy
 *    accident can be saved after the fact even if the tape was not rolling.
 *    With setRetroOnDemand() (the plug-in and the app) the memory for it is only
 *    taken once it can be used: at the first Play, the first arm, or the first
 *    sound the tape hears (useRetro, allocateRetroIfAsked; never on the audio thread).
 *
 *  - One File per Whoosh: the take is cut into one file for each stretch of sound (the Skip Silence gate finds
 *    them), each opened and closed on the writer thread; stretches shorter than c_minWhooshSeconds are thrown away.
 *  - Markers: the audio thread tells the tape where the pass peaks and the hits are (addMarker); they are handed
 *    to the writer thread through a lock-free queue and written into WAV files as cue points with labels.
 *
 * Files are named <Name>_NNNN.wav (.aif / .flac; a counter that goes on through the session), or
 * <Name>_YYYY-MM-DD_HH-MM-SS.wav, in the chosen folder (see Naming). Every file carries a description (BWF bext and
 * LIST INFO in WAV, a comment in AIFF, Vorbis comments in FLAC).
 * Rate (e.g. 96 kHz), bit depth, file type and mono/stereo are chosen per recording (Options).
 *
 * WIDTH: the tape takes whatever it is prepared with, 1 to 12 channels in PROCESSING ORDER (room::layout: the
 * width is known from the count, which is different for every width). Files of the room's width are written in the
 * chosen Channel Order, interleaved or as a Multi-Mono set (one file per channel, all or none of them kept), with
 * the speaker mask and channel names described in TakeFile.h. Mono or Stereo folds a wider take with room::fold
 * (Stereo to Mono is averaged as it always was) and holds the fold under the Ceiling with a limiter; a file
 * narrower than the room it was played in is a fold-down and says so in its description (foldNote).
 */
class TapeRecorder
{
public:
    static constexpr int c_retroSeconds = 60;         // how much the retro buffer keeps unless setRetroSeconds says more
    static constexpr int c_maxRetroSeconds = 300;
    /** The retro buffer of Mono or Stereo never holds more than this: 512 MB is the whole 300 s of Stereo at
        192 kHz. getRetroCapacitySeconds() says what it holds. */
    static constexpr std::int64_t c_maxRetroBytes = 512LL * 1024 * 1024;
    /** In a room (Quad and wider) it holds no more than a minute of 7.1.2 at 48 kHz, 115 MB: a whole minute of
        Quad, 5.1, 7.1 and 7.1.2 at 48 kHz, 50 s of 7.1.4, less at higher rates. A session of several instances in
        a room would otherwise take hundreds of MB each. */
    static constexpr std::int64_t c_maxRetroBytesRoom = 60LL * 48000 * 10 * static_cast<std::int64_t>(sizeof(float));
    static constexpr double c_minWhooshSeconds = 0.2; // One File per Whoosh: less sound than this is not kept

    enum class FileType { Wav = 0, Aiff, Flac };

    /** How a recording is written. Conversion (rate, mono fold) happens on the writer thread. */
    struct Options
    {
        int bitDepth = 24;            // 16, 24, or 32 (float, WAV only; AIFF/FLAC fall back to 24)
        double fileRate = 0.0;        // 0 = session rate; otherwise windowed-sinc conversion, e.g. 96000
        FileType type = FileType::Wav;
        bool mono = false;            // fold the output down to one channel
        bool stereo = false;          // fold a take wider than Stereo down to Stereo (mono wins when both are set)
        bool perWhoosh = false;       // One File per Whoosh (the gate is on whatever setSignalGate says)
        room::FileOrder order = room::FileOrder::ProTools;           // Channel Order of an interleaved WAV or AIFF file (7.1.4)
        takefile::Layout layout = takefile::Layout::Interleaved;     // one file, or one mono file per channel
        float foldCeilingDb = -0.3f;  // the limiter on a fold of a take wider than Stereo
        /** The width the take was played at, when it is not the width of the audio handed in (a Mono room is
            carried as Stereo): a file narrower than this is a fold-down, and says so. */
        std::optional<room::Width> played;
    };

    /** The width a take of numChannels is written at with these options: Mono, Stereo (a wider take folded), or
        the take's own. */
    static room::Width fileWidthOf(const Options& options, int numChannels);
    /** The width the take was played at (options.played, or the width of numChannels), when the file is a
        fold-down of it; nothing otherwise. */
    static std::optional<room::Width> foldedFrom(const Options& options, int numChannels);
    /** Why a take of numChannels can't be written with these options, in a plain sentence; empty when it can. For
        saying so before anything is rendered or recorded (FLAC holds at most eight channels in one file). */
    static juce::String formatProblem(const Options& options, int numChannels);
    /** The sentence a folded file's description ends with: " Folded down from 7.1.4 to Stereo, held under the
        Ceiling." */
    static juce::String foldNote(room::Width from, room::Width to);

    /** How files are named and described. Message thread; it can change while a take is open (the next file uses it). */
    enum class NameStyle { Counter = 0, DateTime };
    struct Naming
    {
        juce::String prefix { "Swooshbuckler" };   // the <Name> of the file name (made safe for a file name)
        NameStyle style = NameStyle::Counter;
        juce::String description { "Swooshbuckler log" }; // the same text in every file type
        juce::String creator { "Samuel Justice" };        // BWF originator, the artist elsewhere
        juce::String software { "Swooshbuckler" };        // name and version
    };
    void setNaming(const Naming& naming);
    Naming getNaming() const;
    /** The file the next recording would get: <prefix>_NNNN<suffix> with the next free number, or
        <prefix>_<date>_<time><suffix>. */
    juce::File nextFile(const juce::File& folder, const juce::String& suffix, const juce::String& extension);

    /** Markers. Audio thread, before process(): a moment samplesFromNow samples into (or after) the next process()
        call. If the tape is writing at that moment the take gets a cue point there. */
    enum MarkerKind { MarkPassA = 0, MarkPassB, MarkThrow, MarkHit, MarkPassStow, numMarkerKinds };
    static juce::String markerLabel(int kind);
    void addMarker(int samplesFromNow, int kind);

    /** Files the tape has closed and kept since the last call (newest last). Any thread but the audio thread. */
    struct FinishedTake
    {
        juce::File file;              // the first of its files (a Multi-Mono set's first channel)
        juce::Array<juce::File> files; // every file of it, in processing order
        takefile::Format format;
        std::optional<room::Width> foldedFrom; // a fold-down: the width it was played at
        double seconds = 0.0;
        int markers = 0;
        bool whoosh = false; // one of One File per Whoosh's files
    };
    std::vector<FinishedTake> takeFinishedTakes();
    /** A file could not be opened while the tape was rolling (One File per Whoosh opens them as it goes): the
        message, once. */
    juce::String takeWriteError();

    /** What a file says about itself (the description written by this class), and its cue points (a WAV file's)
        or markers (an AIFF file's). For tests and tools. */
    static juce::String readDescription(const juce::File& file);
    struct Cue
    {
        std::int64_t frame = 0;
        juce::String label;
    };
    static std::vector<Cue> readCues(const juce::File& file);

    TapeRecorder();
    ~TapeRecorder();

    void prepare(double sampleRate, int numChannels);

    /** Audio thread. */
    void process(const float* const* channels, int numChannels, int numSamples);

    /**
     * Message thread. bitDepth: 16, 24 or 32 (float). fileRate: the WAV's sample rate (0 = the session rate);
     * if it differs from the session rate the output is converted with a windowed-sinc resampler on the
     * writer thread. Files carry BWF metadata. Returns false (and an error) on failure.
     */
    bool start(const juce::File& folder, int bitDepth, juce::String& error, double fileRate = 0.0);
    bool start(const juce::File& folder, const Options& options, juce::String& error);
    void stop();
    bool isRecording() const { return m_recording.load(); }

    /**
     * Skip Silence: while a take is open, only write when there is sound. "Sound" is judged against the recent
     * peak: the gate closes once the level has sat c_gateDepthDb under it (or under c_gateFloor, whichever is
     * higher) for c_gateQuietSeconds, plus a hang of c_gateHangSeconds for the tails, and opens again, with
     * c_gatePrerollSeconds of pre-roll for the attack, as soon as the level comes back over that line. The file is
     * faded out and in over c_gateFadeSeconds at each join (sound that returns during the fade out fades back in
     * from there). Off: the take is continuous. Any thread; it can be
     * changed while a take is open.
     */
    static constexpr float c_gateDepthDb = 25.0f;        // how far under the recent peak counts as quiet
    static constexpr float c_gateFloor = 0.0015f;        // about -56 dBFS: under this is quiet whatever the peak was
    static constexpr double c_gateQuietSeconds = 0.5;    // how long it must stay quiet
    static constexpr double c_gateHangSeconds = 0.25;    // and how much longer the tape keeps rolling after that
    static constexpr double c_gatePrerollSeconds = 0.25; // kept in front of the sound that opens the gate
    static constexpr double c_gateFadeSeconds = 0.03;    // the fade at each end of a skipped stretch
    static constexpr double c_gatePeakHoldSeconds = 10.0; // the recent peak is remembered this long,
    static constexpr float c_gatePeakFallDbPerSecond = 3.0f; // then falls towards the current level
    static constexpr double c_fifoSeconds = 3.0;         // audio the writer thread may fall behind by
    void setSignalGate(bool on) { m_signalGate.store(on); }
    /** While set, a gate that is open stays open whatever the level (the rig ringing out after Stop: the take
        keeps the whole tail). It never opens a closed gate. Any thread. */
    void setGateHold(bool hold) { m_gateHold.store(hold, std::memory_order_relaxed); }
    /** Audio thread: the output is silent and process() won't be called until there is sound again (the rig is at
        rest). An open gate closes now, and nothing from before the rest is kept as pre-roll. Returns false if it
        could not look this time (a take is being opened or closed): call it again on the next block. */
    bool rest();
    bool getSignalGate() const { return m_signalGate.load(); }
    /** True while audio is actually being written (always true when the gate is off and a take is open). */
    bool isCapturing() const { return m_recording.load() && (! (m_signalGate.load() || m_perWhoosh.load()) || m_capturing.load()); }
    double getRecordedSeconds() const;
    juce::File getCurrentFile() const { const juce::ScopedLock sl(m_fileLock); return m_currentFile; }
    juce::File getLastFile() const { const juce::ScopedLock sl(m_fileLock); return m_lastFile; }

    /** Writes the retro buffer (last minute) to a new file. Message thread. */
    bool saveRetro(const juce::File& folder, int bitDepth, juce::File& written, juce::String& error, double fileRate = 0.0);
    bool saveRetro(const juce::File& folder, const Options& options, juce::File& written, juce::String& error);
    double getSessionRate() const { return m_sampleRate; }
    /** How much the retro buffer is asked to keep (60 to c_maxRetroSeconds). The buffer is emptied when this
        changes. Float at the session rate: 23 MB a minute for Stereo at 48 kHz, 92 MB at 192 kHz, six times that
        for 7.1.4, never more than c_maxRetroBytes (Mono, Stereo) or c_maxRetroBytesRoom (wider). Message thread. */
    void setRetroSeconds(int seconds);
    int getRetroSeconds() const { return m_retroSeconds.load(); }
    /** What it really holds at the prepared width and rate: getRetroSeconds(), or less where the cap cuts it
        short (the same whether or not the memory has been taken yet). Any thread. */
    double getRetroCapacitySeconds() const { return m_retroCapacitySeconds.load(); }
    /** The seconds the buffer holds for this many channels at this rate, asked for `seconds`. */
    static double retroCapacityFor(double sampleRate, int numChannels, int seconds);
    /** The most the buffer takes for this many channels (c_maxRetroBytes or c_maxRetroBytesRoom). */
    static std::int64_t retroCapFor(int numChannels) { return numChannels > 2 ? c_maxRetroBytesRoom : c_maxRetroBytes; }

    /** From now on prepare() takes no memory for the retro buffer until it can be used: useRetro() or
        allocateRetroIfAsked() takes it. Without this call (tools, tests) prepare() takes it as always. Call it
        before the first prepare. */
    void setRetroOnDemand() { m_retroInUse.store(false); }
    /** The retro buffer is wanted (Play, arm): takes its memory now if it hasn't yet, and from then on prepare()
        takes it too. Message thread (never the audio thread: it allocates). */
    void useRetro();
    /** Takes the memory if the tape has heard sound since it was prepared without it (a host's transport, a key,
        a throw: the audio thread only raises a flag). Message thread, from a timer. */
    void allocateRetroIfAsked();
    /** The retro buffer's memory, in bytes (0 until it is taken). Message thread. */
    std::int64_t getRetroBytes() const;

    /** Samples the rolling take lost because the writer FIFO was full (reset by start()). Any thread. */
    std::int64_t getDroppedSamples() const { return m_droppedSamples.load(); }
    /** The same as time: the tape runs at the session rate the take was opened at. Any thread. */
    double getDroppedSeconds() const { return static_cast<double>(m_droppedSamples.load()) / juce::jmax(1.0, m_recordRate.load()); }
    /** How many samples the writer thread may fall behind by before audio is dropped (3 s at the session rate). */
    int getFifoSamples() const { return static_cast<int>(c_fifoSeconds * m_sampleRate); }
    /** Samples handed to the writer thread that it has not put in a file yet. Any thread. Whatever feeds the tape
        faster than real time (an offline render, the tests) waits on this rather than guessing how long the writer
        needs. */
    double getBacklogSeconds() const { return static_cast<double>(getBacklogSamples()) / juce::jmax(1.0, m_recordRate.load()); }
    std::int64_t getBacklogSamples() const { return juce::jmax<std::int64_t>(0, m_handedSamples.load() - m_takenSamples.load()); }

    static juce::File defaultFolder();

    /** Writes finished audio to `file` the way the tape writes its own: the same file types, bit depths, mono fold,
        rate conversion (options.fileRate, 0 = `rate`), Channel Order, File Layout and description. `audio` is in
        processing order, 1 to 12 channels. A Multi-Mono set is written beside `file` (<name>.L.wav ...). Replaces
        files that are already there; nothing is left of a take that could not be written whole. `written` (if
        given) gets its files; `cues` (frames of the file) become its markers, as for FileWriter. Any thread but
        the audio thread. */
    static bool writeFile(const juce::File& file, const juce::AudioBuffer<float>& audio, int startSample, int numSamples,
                          double rate, const Options& options, const Naming& naming, juce::String& error,
                          juce::Array<juce::File>* written = nullptr, const std::vector<Cue>& cues = {});
    /** The files writing `file` with these options and this many channels makes. */
    static juce::Array<juce::File> filesFor(const juce::File& file, int numChannels, const Options& options);
    /** ".wav", ".aif" or ".flac". */
    static juce::String extensionOf(FileType type);
    /** The file type as takefile knows it. */
    static takefile::Container containerOf(FileType type);

    /**
     * A file written a piece at a time, with named markers: what Render Variations uses for one file that holds
     * every variation. The same file types, bit depths, mono fold and description as writeFile(). The markers are
     * WAV cue points with labels and AIFF markers; FLAC has no markers, so there the list of names and times goes
     * into the file's comments. Marker positions are frames of the file, which are frames of what is written as
     * long as options.fileRate is 0 or `rate`. Any thread but the audio thread.
     */
    class TakeWriter;
    class FileWriter
    {
    public:
        FileWriter();
        ~FileWriter(); // a file still open is closed without markers

        bool open(const juce::File& file, double rate, int numChannels, const Options& options, const Naming& naming, juce::String& error);
        bool isOpen() const { return m_writer != nullptr; }
        /** The files it writes (one, or a Multi-Mono set), in processing order. */
        juce::Array<juce::File> files() const { return m_files; }
        bool write(const juce::AudioBuffer<float>& audio, int startSample, int numSamples);
        bool writeSilence(int numSamples);
        /** Frames written so far: where a marker for what is written next belongs. */
        std::int64_t position() const { return m_position; }
        /** Closes the file and adds the markers. False if anything written to it failed (the file is then deleted). */
        bool close(const std::vector<Cue>& markers);
        /** After close(): false if the markers could not be added (a WAV or AIFF file of 4 GB or more has no room
            for them), though the sound itself was written. */
        bool markersAdded() const { return m_markersAdded; }
        /** Closes the file and deletes it. */
        void abandon();

    private:
        std::unique_ptr<TakeWriter> m_writer;
        juce::Array<juce::File> m_files;
        int m_channels = 2;
        std::int64_t m_position = 0;
        bool m_ok = true;
        bool m_markersAdded = true;
    };

private:
    class Sink;
    friend class Sink;
    /** Lets the writer thread close a whoosh's file while no audio is arriving. */
    struct Pump : public juce::TimeSliceClient
    {
        explicit Pump(TapeRecorder& o) : owner(o) {}
        int useTimeSlice() override;
        TapeRecorder& owner;
    };

    juce::TimeSliceThread m_thread { "Captain's Log" };
    Pump m_pump { *this };
    Sink* m_sink = nullptr; // the open take's file writer: used on the writer thread, and by stop() once that has let go
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> m_writer;
    juce::CriticalSection m_writerLock;
    juce::AudioFormatWriter::ThreadedWriter* m_activeWriter = nullptr; // guarded by m_writerLock

    std::atomic<bool> m_recording { false };
    std::atomic<std::int64_t> m_samplesWritten { 0 };
    std::atomic<bool> m_signalGate { false }, m_capturing { false }, m_perWhoosh { false };

    // From the audio thread to the writer thread, in the order of their place in the take (lock-free).
    struct Command
    {
        enum Type { Marker = 0, Split };
        std::int64_t position = 0; // samples of the take's stream (what the audio thread has handed to the writer)
        int type = Marker;
        int value = 0;             // Marker: its kind. Split: 1 = keep the file, 0 = too short
    };
    static constexpr int c_commandSlots = 1024;
    juce::AbstractFifo m_commandFifo { c_commandSlots };
    std::array<Command, c_commandSlots> m_commands;
    bool pushCommand(const Command& command);   // audio thread
    bool peekCommand(Command& command);         // writer thread
    void popCommand();                          //
    void closeWhoosh();                         // audio thread: the gate has closed
    void beginTakeIfNew();                      // audio thread, with the writer in hand: a new take starts clean
    std::int64_t m_streamPos = 0;               // audio thread: samples handed to the writer in this take
    std::atomic<std::int64_t> m_whooshLoud { 0 }; // samples of sound since the gate opened
    struct PendingMarker
    {
        int due = 0, kind = 0;
    };
    std::array<PendingMarker, 32> m_pendingMarkers {}; // audio thread
    int m_numPendingMarkers = 0;

    // Names, metadata and what has been written (never touched by the audio thread).
    mutable juce::CriticalSection m_metaLock;
    Naming m_naming;
    int m_counter = 1;
    int numberOf(const juce::File& file) const; // the NNNN of a file named by the counter (0 = none)
    std::vector<FinishedTake> m_finished;
    juce::String m_writeError;
    // The gate (audio thread only; sized in prepare)
    int m_hangLeft = 0;
    int m_fadeInLeft = 0;                 // samples left of a fade back in (sound returned during the closing fade)
    juce::AudioBuffer<float> m_preroll, m_fade;
    int m_prerollWrite = 0, m_prerollFill = 0;
    float m_gatePeak = 0.0f;              // the recent peak the gate measures against
    int m_gatePeakHold = 0;               // samples until it starts to fall
    std::atomic<bool> m_gateRestart { false }; // a new take: forget the last one's levels
    std::atomic<bool> m_streamRestart { false }; // a new take: its stream starts at 0
    std::atomic<bool> m_gateHold { false };    // see setGateHold
    std::atomic<std::int64_t> m_handedSamples { 0 }, m_takenSamples { 0 }; // into the writer's FIFO, and out of it
    std::atomic<std::int64_t> m_droppedSamples { 0 };   //
    std::atomic<double> m_recordRate { 48000.0 };        // session rate the current take was opened at
    juce::File m_recordFolder;                           // for restarting a take on a rate change
    Options m_recordOptions;                             //
    double m_sampleRate = 48000.0;
    int m_numChannels = 2;
    // Written by start / stop / saveRetro (and prepare, from whichever thread prepares) and read by the UI.
    // Its own lock, never the writer's: the audio thread try-locks that one and would skip a block of tape.
    mutable juce::CriticalSection m_fileLock;
    juce::File m_currentFile, m_lastFile;

    // Retro ring (written on the audio thread, read on the message thread).
    juce::AudioBuffer<float> m_retro;
    std::atomic<int> m_retroSeconds { c_retroSeconds };
    std::atomic<double> m_retroCapacitySeconds { static_cast<double>(c_retroSeconds) };
    int retroSamples(double sampleRate, int numChannels, int seconds); // sets m_retroCapacitySeconds
    std::atomic<int> m_retroWrite { 0 };
    std::atomic<std::int64_t> m_retroTotal { 0 };
    mutable juce::SpinLock m_retroLock; // held while preparing / reallocating, and briefly per piece while saveRetro copies
    std::atomic<int> m_retroGeneration { 0 }; // bumped by prepare (saveRetro aborts if it changes mid-copy)
    std::atomic<bool> m_retroInUse { true };  // false until it can be used (setRetroOnDemand)
    std::atomic<bool> m_retroAsked { false }; // the audio thread heard sound while the buffer had no memory
};

#pragma once

#include <JuceHeader.h>

#include <atomic>

/**
 * "Captain's Log": a tape machine on the output.
 *
 *  - Rolling record: while on, everything that leaves the app streams to a WAV
 *    file (background thread via AudioFormatWriter::ThreadedWriter; the audio
 *    thread only copies into a FIFO and never blocks).
 *  - Retro capture: the last c_retroSeconds are always kept in RAM, so a happy
 *    accident can be saved after the fact even if the tape was not rolling.
 *
 * Files are named Swooshbuckler_YYYY-MM-DD_HH-MM-SS_NNNNN.wav (.aif / .flac; NNNNN = random 5 digits)
 * in the chosen folder.
 * Rate (e.g. 96 kHz), bit depth, file type and mono/stereo are chosen per recording (Options).
 */
class TapeRecorder
{
public:
    static constexpr int c_retroSeconds = 60;

    enum class FileType { Wav = 0, Aiff, Flac };

    /** How a recording is written. Conversion (rate, mono fold) happens on the writer thread. */
    struct Options
    {
        int bitDepth = 24;            // 16, 24, or 32 (float, WAV only; AIFF/FLAC fall back to 24)
        double fileRate = 0.0;        // 0 = session rate; otherwise windowed-sinc conversion, e.g. 96000
        FileType type = FileType::Wav;
        bool mono = false;            // fold the stereo output down to one channel
    };

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
     * faded out and in over c_gateFadeSeconds at each join. Off: the take is continuous. Any thread; it can be
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
    bool getSignalGate() const { return m_signalGate.load(); }
    /** True while audio is actually being written (always true when the gate is off and a take is open). */
    bool isCapturing() const { return m_recording.load() && (! m_signalGate.load() || m_capturing.load()); }
    double getRecordedSeconds() const;
    juce::File getCurrentFile() const { const juce::ScopedLock sl(m_fileLock); return m_currentFile; }
    juce::File getLastFile() const { const juce::ScopedLock sl(m_fileLock); return m_lastFile; }

    /** Writes the retro buffer (last minute) to a new file. Message thread. */
    bool saveRetro(const juce::File& folder, int bitDepth, juce::File& written, juce::String& error, double fileRate = 0.0);
    bool saveRetro(const juce::File& folder, const Options& options, juce::File& written, juce::String& error);
    double getSessionRate() const { return m_sampleRate; }

    /** Samples the rolling take lost because the writer FIFO was full (reset by start()). Any thread. */
    std::int64_t getDroppedSamples() const { return m_droppedSamples.load(); }
    /** The same as time: the tape runs at the session rate the take was opened at. Any thread. */
    double getDroppedSeconds() const { return static_cast<double>(m_droppedSamples.load()) / juce::jmax(1.0, m_recordRate.load()); }
    /** How many samples the writer thread may fall behind by before audio is dropped (3 s at the session rate). */
    int getFifoSamples() const { return static_cast<int>(c_fifoSeconds * m_sampleRate); }

    static juce::File defaultFolder();
    static juce::File makeFileName(const juce::File& folder, const juce::String& suffix = {}, const juce::String& extension = ".wav");

private:
    juce::TimeSliceThread m_thread { "Captain's Log" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> m_writer;
    juce::CriticalSection m_writerLock;
    juce::AudioFormatWriter::ThreadedWriter* m_activeWriter = nullptr; // guarded by m_writerLock

    std::atomic<bool> m_recording { false };
    std::atomic<std::int64_t> m_samplesWritten { 0 };
    std::atomic<bool> m_signalGate { false }, m_capturing { false };
    // The gate (audio thread only; sized in prepare)
    int m_hangLeft = 0;
    juce::AudioBuffer<float> m_preroll, m_fade;
    int m_prerollWrite = 0, m_prerollFill = 0;
    float m_gatePeak = 0.0f;              // the recent peak the gate measures against
    int m_gatePeakHold = 0;               // samples until it starts to fall
    std::atomic<bool> m_gateRestart { false }; // a new take: forget the last one's levels
    std::atomic<bool> m_gateHold { false };    // see setGateHold
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
    std::atomic<int> m_retroWrite { 0 };
    std::atomic<std::int64_t> m_retroTotal { 0 };
    juce::SpinLock m_retroLock; // held while preparing / reallocating, and briefly per piece while saveRetro copies
    std::atomic<int> m_retroGeneration { 0 }; // bumped by prepare (saveRetro aborts if it changes mid-copy)
};

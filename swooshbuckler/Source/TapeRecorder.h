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
     * Armed mode: while a take is open, only write when there is sound. A short pre-roll keeps the attack and a
     * 2 s hang keeps the tails; silent stretches are left out of the file. Set before start(). Any thread.
     */
    void setSignalGate(bool on) { m_signalGate.store(on); }
    bool getSignalGate() const { return m_signalGate.load(); }
    /** True while audio is actually being written (always true when the gate is off and a take is open). */
    bool isCapturing() const { return m_recording.load() && (! m_signalGate.load() || m_capturing.load()); }
    double getRecordedSeconds() const;
    juce::File getCurrentFile() const { return m_currentFile; }
    juce::File getLastFile() const { return m_lastFile; }

    /** Writes the retro buffer (last minute) to a new file. Message thread. */
    bool saveRetro(const juce::File& folder, int bitDepth, juce::File& written, juce::String& error, double fileRate = 0.0);
    bool saveRetro(const juce::File& folder, const Options& options, juce::File& written, juce::String& error);
    double getSessionRate() const { return m_sampleRate; }

    /** Samples the rolling take lost because the writer FIFO was full (reset by start()). Any thread. */
    std::int64_t getDroppedSamples() const { return m_droppedSamples.load(); }

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
    int m_hangLeft = 0;
    juce::AudioBuffer<float> m_preroll;   // audio thread only (sized in prepare)
    int m_prerollWrite = 0, m_prerollFill = 0;
    std::atomic<std::int64_t> m_droppedSamples { 0 };   //
    double m_recordRate = 48000.0;                       // session rate the current take was opened at
    juce::File m_recordFolder;                           // for restarting a take on a rate change
    Options m_recordOptions;                             //
    double m_sampleRate = 48000.0;
    int m_numChannels = 2;
    juce::File m_currentFile, m_lastFile;

    // Retro ring (written on the audio thread, read on the message thread).
    juce::AudioBuffer<float> m_retro;
    std::atomic<int> m_retroWrite { 0 };
    std::atomic<std::int64_t> m_retroTotal { 0 };
    juce::SpinLock m_retroLock; // held while preparing / reallocating, and briefly per piece while saveRetro copies
    std::atomic<int> m_retroGeneration { 0 }; // bumped by prepare (saveRetro aborts if it changes mid-copy)
};

#pragma once

#include <JuceHeader.h>
#include "Theme.h"
#include "dsp/FlattenerTypes.h"

class PitchFlattenerAudioProcessor;

//==============================================================================
/**
 * Hop-based STFT spectrogram of the processor's analysis-rate audio.
 *
 * Every hop of new samples produces one column, written into a ring image.
 * Rows are in absolute log-frequency (minHz..maxHz), so a column never has to
 * be redrawn when the view range changes; the view just maps a slice of the
 * image. Message thread only.
 */
class Spectrogram
{
public:
    static constexpr int fftOrder = 11;
    static constexpr int fftSize = 1 << fftOrder;  // 2048
    static constexpr int hop = 256;
    static constexpr int rows = 384;
    static constexpr int columns = 2048;           // ~22 s of history at 24 kHz
    static constexpr float minHz = 20.0f;
    static constexpr float maxHz = 10000.0f;

    Spectrogram();

    /** Clears everything and rebuilds the row-to-bin map for a new rate. */
    void reset (double analysisRate);
    double getRate() const noexcept { return m_rate; }

    /** Feeds samples (any count); runs an FFT frame every hop samples. */
    void push (const float* samples, int numSamples);

    /** Samples pushed since the last reset. */
    int64_t getSamplesPushed() const noexcept { return m_samplesPushed; }
    /** Columns written since the last reset. Column k lives at image x = k % columns. */
    int64_t getColumnsWritten() const noexcept { return m_columnsWritten; }
    /** Centre of column k, in pushed-sample units. */
    static double columnCentre (int64_t k) noexcept { return (double) k * hop + fftSize * 0.5; }

    const juce::Image& getImage() const noexcept { return m_image; }

    /** Continuous row coordinate (0 = minHz, rows - 1 = maxHz) as log2(Hz). */
    static float rowToLog2Hz (float row) noexcept;

private:
    void processFrame();

    juce::dsp::FFT m_fft { fftOrder };
    juce::dsp::WindowingFunction<float> m_window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false };

    std::vector<float> m_fifo;
    std::vector<float> m_fftData;
    std::vector<float> m_dbRow;
    int m_fifoPos = 0;
    int m_samplesUntilFrame = fftSize;

    int64_t m_samplesPushed = 0;
    int64_t m_columnsWritten = 0;
    double m_rate = 0.0;

    // Row r reads bins [m_binLo[r], m_binHi[r]] (max), or interpolates when they are equal.
    std::vector<int> m_binLo, m_binHi;
    std::vector<float> m_binFrac;
    std::array<juce::PixelARGB, 256> m_lut {};

    juce::Image m_image { juce::Image::ARGB, columns, rows, true };
};

//==============================================================================
/**
 * The main display: a scrolling pitch-trace graph (log-frequency y axis with
 * note gridlines) over a dim spectrogram, plus a readout column.
 *
 * Traces (each toggled by its legend chip):
 *   RAW    dim teal dots  - the raw detector best guess
 *   TRACK  teal line      - the final cleaned pitch
 *   TARGET amber line     - the pitch being flattened to
 *   OUTPUT violet line    - track shifted by the applied correction
 *
 * Telemetry arrives after the tracker's lookahead; the spectrogram is delayed
 * by the reported latency so both line up on the same time axis.
 */
class PitchDisplay : public juce::Component, private juce::Timer
{
public:
    explicit PitchDisplay (PitchFlattenerAudioProcessor& processor);
    ~PitchDisplay() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** Help text for whatever part of the display is under the given local point. */
    juce::String getHelpTextAt (juce::Point<int> localPoint) const;

    static constexpr double historySeconds = 8.0;

private:
    void timerCallback() override;
    void drainTelemetry();
    void drainAudio();
    void updateViewRange();

    void paintGraph (juce::Graphics&, juce::Rectangle<float> area);
    void paintReadouts (juce::Graphics&, juce::Rectangle<float> area);
    void paintChips (juce::Graphics&);

    double samplesPerFrame() const noexcept { return (double) m_hopEstimate; }
    double analysisRate() const noexcept;
    float yForLog2 (float log2Hz, juce::Rectangle<float> area) const noexcept;

    enum Trace { spectrumTrace = 0, rawTrace, trackTrace, targetTrace, outputTrace, numTraces };

    struct Chip
    {
        juce::String name;
        juce::Colour colour;
        juce::String help;
        juce::Rectangle<float> bounds;
    };

    PitchFlattenerAudioProcessor& m_processor;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;

    std::atomic<float>* m_minFreq = nullptr;
    std::atomic<float>* m_maxFreq = nullptr;
    std::atomic<float>* m_gate = nullptr;
    std::atomic<float>* m_targetMode = nullptr;
    std::atomic<float>* m_manualOverride = nullptr;
    std::atomic<float>* m_maxShift = nullptr;

    // Telemetry history (ring, oldest overwritten)
    static constexpr int c_historyCapacity = 8192;
    std::vector<pf::TelemetryFrame> m_history;
    int64_t m_historyCount = 0;
    pf::TelemetryFrame m_latest;

    // Frame period estimate (analysis samples per telemetry frame)
    int m_hopEstimate = 120;
    int64_t m_hopAudioAccum = 0, m_hopFrameAccum = 0;
    bool m_primed = false;

    Spectrogram m_spectrogram;
    std::vector<float> m_audioScratch;

    // View
    juce::Rectangle<float> m_graphArea, m_readoutArea;
    float m_viewLo = 0.0f, m_viewHi = 0.0f; // log2(Hz)
    bool m_autoZoom = true;
    std::array<bool, numTraces> m_show { { true, true, true, true, true } };
    std::array<Chip, numTraces> m_chips;
    juce::Rectangle<float> m_zoomChip;
    int m_hoverChip = -1; // 0..numTraces-1 = trace chip, numTraces = zoom chip

    // Readouts (refreshed at ~10 Hz so the digits are legible)
    int m_readoutCountdown = 0;
    pf::TelemetryFrame m_shown;
    float m_inputDb = -120.0f;

    juce::Path m_rawPath, m_trackPath, m_targetPath, m_outputPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchDisplay)
};

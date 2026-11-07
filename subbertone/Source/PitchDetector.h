#pragma once

#include <array>
#include <vector>

/**
 * Streaming YIN pitch detector.
 *
 * Fed one full-rate sample at a time (already low-passed by the caller), it
 * decimates internally to ~11 kHz and runs YIN on a sliding window every few
 * milliseconds. Analysis cadence is independent of the host block size, and
 * the cost is bounded and small because YIN runs at the decimated rate.
 *
 * No JUCE dependency, no allocation after prepare(): safe for the audio thread.
 */
class PitchDetector
{
public:
    void prepare(double sampleRate);
    void reset();

    /** Upper bound for detection; candidates above this are rejected. */
    void setMaxFrequency(float hz);

    /** Push one band-limited, full-rate sample. Returns true when a new estimate is ready. */
    bool pushSample(float x);

    /** Most recent estimate in Hz, or 0 when the window was judged unvoiced. */
    float getFrequency() const  { return m_frequency; }
    float getConfidence() const { return m_confidence; }

    /** Lag of the analysis, useful for the caller's voicing logic. */
    int getHopSize() const { return m_hop * m_decimation; }

    static constexpr float c_minFrequency = 40.0f;
    static constexpr float c_maxFrequency = 1000.0f;

private:
    void analyse();

    double m_sampleRate = 44100.0;
    double m_decimatedRate = 11025.0;
    int m_decimation = 4;
    int m_decimationCounter = 0;

    int m_tauMin = 2;
    int m_tauMax = 300;
    int m_window = 300;
    int m_hop = 64;
    int m_hopCounter = 0;

    std::vector<float> m_ring;
    int m_ringWrite = 0;
    int m_ringFilled = 0;

    std::vector<float> m_frame;
    std::vector<float> m_diff;

    float m_maxFrequency = 250.0f;
    float m_frequency = 0.0f;
    float m_confidence = 0.0f;

    // Median-of-three on voiced estimates to reject isolated octave errors.
    std::array<float, 3> m_history { 0.0f, 0.0f, 0.0f };
    int m_historyCount = 0;

    static constexpr float c_yinThreshold = 0.2f;
    static constexpr float c_yinFallbackThreshold = 0.4f;
};

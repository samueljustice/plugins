#pragma once

#include <vector>

/**
 * YIN Pitch Detection Algorithm
 * Based on: http://audition.ens.fr/adc/pdf/2002_JASA_YIN.pdf
 * Adapted from: https://github.com/ashokfernandez/Yin-Pitch-Tracking
 */
class PitchDetector
{
public:
    PitchDetector()  = default;
    ~PitchDetector() = default;

    void prepare(double sampleRate);
    float detectPitch(const float* inputBuffer, int numSamples, float threshold = 0.1f);

    // Get the probability/confidence of the last detected pitch (0.0 - 1.0)
    float getProbability() const { return m_probability; }

private:
    // YIN algorithm steps
    void yinDifference(const float* buffer, int halfLength);
    void yinCumulativeMeanNormalizedDifference(int halfLength);
    int yinAbsoluteThreshold(int halfLength);
    float yinParabolicInterpolation(int tauEstimate, int halfLength);

    // Core detection
    float detectPitchYIN(const float* buffer, int bufferLength);

    double m_sampleRate = 0.0;
    int m_bufferSize = 0;
    int m_halfBufferSize = 0;

    // YIN algorithm buffers
    std::vector<float> m_yinBuffer;           // Stores intermediate YIN processing results
    std::vector<float> m_inputAccumulator;    // Accumulates input samples

    float m_probability = 0.0f;   // Confidence of detected pitch (0.0 - 1.0)
    float m_yinThreshold = 0.15f; // YIN threshold (lower = stricter detection)

    // Pitch smoothing
    float m_previousPitch = 0.0f;
    float m_smoothedPitch = 0.0f;
    static constexpr float c_pitchSmoothingFactor = 0.85f;

    // Detection parameters
    static constexpr float c_minFrequency = 40.0f;   // 40 Hz minimum (bass range)
    static constexpr float c_maxFrequency = 1000.0f; // 1000 Hz maximum

    bool m_isPrepared = false;
    int m_samplesSinceLastAnalysis = 0;
};

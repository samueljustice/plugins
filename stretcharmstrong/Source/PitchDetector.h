#pragma once

#include <JuceHeader.h>
#include <vector>

/**
 * YIN Pitch Detection Algorithm
 * Based on: http://audition.ens.fr/adc/pdf/2002_JASA_YIN.pdf
 * Adapted from: https://github.com/ashokfernandez/Yin-Pitch-Tracking
 */
class PitchDetector
{
public:
    PitchDetector();
    ~PitchDetector();

    void prepare(double sampleRate, int maxBlockSize);
    float detectPitch(const float* inputBuffer, int numSamples, float threshold = 0.1f);

    // Get the probability/confidence of the last detected pitch (0.0 - 1.0)
    float getProbability() const { return probability; }

private:
    double sampleRate = 44100.0;
    int bufferSize = 2048;
    int halfBufferSize = 1024;

    // YIN algorithm buffers
    std::vector<float> yinBuffer;      // Stores intermediate YIN processing results
    std::vector<float> inputAccumulator; // Accumulates input samples

    float probability = 0.0f;          // Confidence of detected pitch (0.0 - 1.0)
    float yinThreshold = 0.15f;        // YIN threshold (lower = stricter detection)

    // Pitch smoothing
    float previousPitch = 0.0f;
    float smoothedPitch = 0.0f;
    static constexpr float pitchSmoothingFactor = 0.85f;

    // Detection parameters
    static constexpr float minFrequency = 40.0f;   // 40 Hz minimum (bass range)
    static constexpr float maxFrequency = 1000.0f; // 1000 Hz maximum

    // YIN algorithm steps
    void yinDifference(const float* buffer);
    void yinCumulativeMeanNormalizedDifference();
    int yinAbsoluteThreshold();
    float yinParabolicInterpolation(int tauEstimate);

    // Core detection
    float detectPitchYIN(const float* buffer, int bufferLength);
};

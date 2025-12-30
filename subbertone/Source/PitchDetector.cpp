#include "PitchDetector.h"
#include <algorithm>
#include <cmath>

PitchDetector::PitchDetector()
{
}

PitchDetector::~PitchDetector()
{
}

void PitchDetector::prepare(double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;

    // Buffer size should be large enough for lowest frequency detection
    // At 40Hz minimum, we need at least 2 periods: 2 * (sampleRate / 40) samples
    // Using 2048 samples at 44.1kHz gives us down to ~43Hz, 4096 gives ~21Hz
    bufferSize = static_cast<int>(sampleRate * 0.05); // 50ms window
    bufferSize = std::max(bufferSize, 2048);
    bufferSize = std::min(bufferSize, 4096);

    halfBufferSize = bufferSize / 2;

    // Allocate YIN buffer (only needs half the buffer size)
    yinBuffer.resize(halfBufferSize);
    std::fill(yinBuffer.begin(), yinBuffer.end(), 0.0f);

    // Allocate input accumulator
    inputAccumulator.resize(bufferSize);
    std::fill(inputAccumulator.begin(), inputAccumulator.end(), 0.0f);

    // Reset pitch tracking
    previousPitch = 0.0f;
    smoothedPitch = 0.0f;
    probability = 0.0f;
}

float PitchDetector::detectPitch(const float* inputBuffer, int numSamples, float threshold)
{
    // Check signal level
    float rms = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        rms += inputBuffer[i] * inputBuffer[i];
    }
    rms = std::sqrt(rms / static_cast<float>(numSamples));

    // Return smoothed previous pitch if signal is too quiet
    if (rms < threshold * 0.1f)
    {
        smoothedPitch *= 0.95f;
        if (smoothedPitch < minFrequency)
            smoothedPitch = 0.0f;
        probability = 0.0f;
        return smoothedPitch;
    }

    // Accumulate input samples (shift and add new samples)
    int samplesToShift = std::min(numSamples, bufferSize);
    if (samplesToShift < bufferSize)
    {
        std::memmove(inputAccumulator.data(),
                     inputAccumulator.data() + samplesToShift,
                     (bufferSize - samplesToShift) * sizeof(float));
    }

    int copyStart = std::max(0, numSamples - samplesToShift);
    std::memcpy(inputAccumulator.data() + (bufferSize - samplesToShift),
                inputBuffer + copyStart,
                samplesToShift * sizeof(float));

    // Run YIN pitch detection
    float detectedPitch = detectPitchYIN(inputAccumulator.data(), bufferSize);

    // Apply smoothing
    if (detectedPitch > 0.0f)
    {
        if (previousPitch == 0.0f)
        {
            // Initialize immediately if no previous pitch
            smoothedPitch = detectedPitch;
        }
        else
        {
            // Check if pitch jump is reasonable (within 1 octave)
            float ratio = detectedPitch / previousPitch;
            if (ratio > 0.5f && ratio < 2.0f)
            {
                // Normal smoothing
                smoothedPitch = smoothedPitch * pitchSmoothingFactor +
                               detectedPitch * (1.0f - pitchSmoothingFactor);
            }
            else
            {
                // Large jump - update more quickly
                smoothedPitch = smoothedPitch * 0.5f + detectedPitch * 0.5f;
            }
        }
        previousPitch = detectedPitch;
    }
    else
    {
        // No pitch detected - decay smoothly
        smoothedPitch *= 0.9f;
        if (smoothedPitch < minFrequency)
        {
            smoothedPitch = 0.0f;
            previousPitch = 0.0f;
        }
    }

    return smoothedPitch;
}

float PitchDetector::detectPitchYIN(const float* buffer, int bufferLength)
{
    // Clear YIN buffer
    std::fill(yinBuffer.begin(), yinBuffer.end(), 0.0f);

    // Step 1: Calculates the squared difference of the signal with a shifted version of itself
    yinDifference(buffer);

    // Step 2: Calculate the cumulative mean on the normalized difference
    yinCumulativeMeanNormalizedDifference();

    // Step 3: Search through the normalized cumulative mean array and find values below threshold
    int tauEstimate = yinAbsoluteThreshold();

    // Step 4: Parabolic interpolation to improve pitch estimate
    if (tauEstimate != -1)
    {
        float betterTau = yinParabolicInterpolation(tauEstimate);
        float pitchInHz = static_cast<float>(sampleRate) / betterTau;

        // Clamp to valid frequency range
        if (pitchInHz >= minFrequency && pitchInHz <= maxFrequency)
        {
            return pitchInHz;
        }
    }

    return -1.0f;
}

/**
 * Step 1: Calculates the squared difference of the signal with a shifted version of itself.
 * This is the YIN algorithm's tweak on autocorrelation.
 * See: http://audition.ens.fr/adc/pdf/2002_JASA_YIN.pdf
 */
void PitchDetector::yinDifference(const float* buffer)
{
    for (int tau = 0; tau < halfBufferSize; ++tau)
    {
        yinBuffer[tau] = 0.0f;

        for (int i = 0; i < halfBufferSize; ++i)
        {
            float delta = buffer[i] - buffer[i + tau];
            yinBuffer[tau] += delta * delta;
        }
    }
}

/**
 * Step 2: Calculate the cumulative mean on the normalized difference calculated in step 1.
 * This normalization helps find the true period rather than just the first minimum.
 */
void PitchDetector::yinCumulativeMeanNormalizedDifference()
{
    float runningSum = 0.0f;
    yinBuffer[0] = 1.0f;

    for (int tau = 1; tau < halfBufferSize; ++tau)
    {
        runningSum += yinBuffer[tau];

        if (runningSum != 0.0f)
        {
            yinBuffer[tau] *= static_cast<float>(tau) / runningSum;
        }
        else
        {
            yinBuffer[tau] = 1.0f;
        }
    }
}

/**
 * Step 3: Search through the normalized cumulative mean array and find values below threshold.
 * Returns the tau (lag) which produces the best autocorrelation, or -1 if not found.
 */
int PitchDetector::yinAbsoluteThreshold()
{
    int tau;

    // Calculate the minimum tau based on maximum frequency
    int minTau = static_cast<int>(sampleRate / maxFrequency);
    minTau = std::max(2, minTau); // At least 2 to avoid edge cases

    // Calculate the maximum tau based on minimum frequency
    int maxTau = static_cast<int>(sampleRate / minFrequency);
    maxTau = std::min(maxTau, halfBufferSize);

    // Search through the array of cumulative mean values
    // Start from minTau to ignore high frequencies outside our range
    for (tau = minTau; tau < maxTau; ++tau)
    {
        if (yinBuffer[tau] < yinThreshold)
        {
            // Look for the local minimum
            while (tau + 1 < maxTau && yinBuffer[tau + 1] < yinBuffer[tau])
            {
                ++tau;
            }

            // Store the probability
            // From the YIN paper: The threshold determines the list of
            // candidates admitted to the set, and can be interpreted as the
            // proportion of aperiodic power tolerated within a periodic signal.
            // Since we want periodicity and not aperiodicity:
            // periodicity = 1 - aperiodicity
            probability = 1.0f - yinBuffer[tau];
            return tau;
        }
    }

    // No pitch found
    probability = 0.0f;
    return -1;
}

/**
 * Step 4: Interpolate the shift value (tau) to improve the pitch estimate.
 * The 'best' shift value for autocorrelation is most likely not an integer shift.
 * As we only autocorrelated using integer shifts, we should check for a better
 * fractional shift value using parabolic interpolation.
 */
float PitchDetector::yinParabolicInterpolation(int tauEstimate)
{
    float betterTau;
    int x0, x2;

    // Calculate the first polynomial coefficient based on the current estimate of tau
    if (tauEstimate < 1)
    {
        x0 = tauEstimate;
    }
    else
    {
        x0 = tauEstimate - 1;
    }

    // Calculate the second polynomial coefficient based on the current estimate of tau
    if (tauEstimate + 1 < halfBufferSize)
    {
        x2 = tauEstimate + 1;
    }
    else
    {
        x2 = tauEstimate;
    }

    // Algorithm to parabolically interpolate the shift value tau
    if (x0 == tauEstimate)
    {
        if (yinBuffer[tauEstimate] <= yinBuffer[x2])
        {
            betterTau = static_cast<float>(tauEstimate);
        }
        else
        {
            betterTau = static_cast<float>(x2);
        }
    }
    else if (x2 == tauEstimate)
    {
        if (yinBuffer[tauEstimate] <= yinBuffer[x0])
        {
            betterTau = static_cast<float>(tauEstimate);
        }
        else
        {
            betterTau = static_cast<float>(x0);
        }
    }
    else
    {
        float s0 = yinBuffer[x0];
        float s1 = yinBuffer[tauEstimate];
        float s2 = yinBuffer[x2];

        // Fixed AUBIO implementation (thanks to Karl Helgason)
        float denominator = 2.0f * (2.0f * s1 - s2 - s0);

        if (std::abs(denominator) > 1e-10f)
        {
            betterTau = static_cast<float>(tauEstimate) + (s2 - s0) / denominator;
        }
        else
        {
            betterTau = static_cast<float>(tauEstimate);
        }
    }

    return betterTau;
}

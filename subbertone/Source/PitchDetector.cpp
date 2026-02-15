#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

void PitchDetector::prepare(double sampleRate)
{
    m_sampleRate = sampleRate;

    // Buffer size should be large enough for lowest frequency detection
    // At 40Hz minimum, we need at least 2 periods: 2 * (m_sampleRate / 40) samples
    // Using 2048 samples at 44.1kHz gives us down to ~43Hz, 4096 gives ~21Hz
    if (m_sampleRate <= 0.0)
    {
        m_isPrepared = false;
        return;
    }

    m_bufferSize = static_cast<int>(m_sampleRate * 0.05); // 50ms window
    m_bufferSize = std::clamp(m_bufferSize, 2048, 4096);

    m_halfBufferSize = m_bufferSize / 2;

    // Allocate YIN buffer (only needs half the buffer size)
    m_yinBuffer.resize(m_halfBufferSize);
    std::fill(m_yinBuffer.begin(), m_yinBuffer.end(), 0.0f);

    // Allocate input accumulator
    m_inputAccumulator.resize(m_bufferSize);
    std::fill(m_inputAccumulator.begin(), m_inputAccumulator.end(), 0.0f);

    // Reset pitch tracking
    m_previousPitch = 0.0f;
    m_smoothedPitch = 0.0f;
    m_probability = 0.0f;
    m_samplesSinceLastAnalysis = 0;
    m_isPrepared = true;
}

float PitchDetector::detectPitch(const float* inputBuffer, int numSamples, float threshold)
{
    if (!m_isPrepared || inputBuffer == nullptr || numSamples <= 0 || m_bufferSize <= 0)
        return 0.0f;

    // Check signal level
    float rms = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        rms += inputBuffer[i] * inputBuffer[i];
    }

    rms = std::sqrt(rms / static_cast<float>(numSamples));

    // Return smoothed previous pitch if signal is too quiet
    if (rms < threshold)
    {
        m_smoothedPitch *= 0.95f;

        if (m_smoothedPitch < c_minFrequency)
            m_smoothedPitch = 0.0f;

        m_probability = 0.0f;

        return m_smoothedPitch;
    }

    // Accumulate input samples (shift and add new samples)
    const int samplesToShift = std::min(numSamples, m_bufferSize);

    if (samplesToShift < m_bufferSize)
        std::memmove(m_inputAccumulator.data(), m_inputAccumulator.data() + samplesToShift, (m_bufferSize - samplesToShift) * sizeof(float));

    const int copyStart = std::max(0, numSamples - samplesToShift);

    std::memcpy(m_inputAccumulator.data() + (m_bufferSize - samplesToShift), inputBuffer + copyStart, samplesToShift * sizeof(float));

    m_samplesSinceLastAnalysis += samplesToShift;

    // Not enough new data yet; return current smoothed pitch
    if (m_samplesSinceLastAnalysis < m_halfBufferSize)
        return m_smoothedPitch;

    m_samplesSinceLastAnalysis = 0;

    // Run YIN pitch detection
    const float detectedPitch = detectPitchYIN(m_inputAccumulator.data(), m_bufferSize);

    // Apply smoothing
    if (detectedPitch > 0.0f)
    {
        if (m_previousPitch == 0.0f)
        {
            // Initialize immediately if no previous pitch
            m_smoothedPitch = detectedPitch;
        }
        else
        {
            // Check if pitch jump is reasonable (within 1 octave)
            const float ratio = detectedPitch / m_previousPitch;

            m_smoothedPitch = (ratio > 0.5f && ratio < 2.0f)
                ? m_smoothedPitch * c_pitchSmoothingFactor + detectedPitch * (1.0f - c_pitchSmoothingFactor)    // Normal smoothing
                : m_smoothedPitch * 0.5f + detectedPitch * 0.5f;                                                // Large jump - update more quickly
        }

        m_previousPitch = detectedPitch;
    }
    else
    {
        // No pitch detected - decay smoothly
        m_smoothedPitch *= 0.9f;

        if (m_smoothedPitch < c_minFrequency)
        {
            m_smoothedPitch = 0.0f;
            m_previousPitch = 0.0f;
        }
    }

    return m_smoothedPitch;
}

float PitchDetector::detectPitchYIN(const float* buffer, int bufferLength)
{
    const int halfLength = std::min(m_halfBufferSize, bufferLength / 2);

    if (halfLength <= 1)
        return -1.0f;

    // Clear YIN buffer
    std::fill(m_yinBuffer.begin(), m_yinBuffer.end(), 0.0f);

    // Step 1: Calculates the squared difference of the signal with a shifted version of itself
    yinDifference(buffer, halfLength);

    // Step 2: Calculate the cumulative mean on the normalized difference
    yinCumulativeMeanNormalizedDifference(halfLength);

    // Step 3: Search through the normalized cumulative mean array and find values below threshold
    const int tauEstimate = yinAbsoluteThreshold(halfLength);

    // Step 4: Parabolic interpolation to improve pitch estimate
    if (tauEstimate != -1)
    {
        const float betterTau = yinParabolicInterpolation(tauEstimate, halfLength);
        const float pitchInHz = static_cast<float>(m_sampleRate) / betterTau;

        // Clamp to valid frequency range
        if (pitchInHz >= c_minFrequency && pitchInHz <= c_maxFrequency)
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
void PitchDetector::yinDifference(const float* buffer, int halfLength)
{
    for (int tau = 0; tau < halfLength; ++tau)
    {
        m_yinBuffer[tau] = 0.0f;

        for (int i = 0; i < halfLength; ++i)
        {
            const float delta = buffer[i] - buffer[i + tau];
            m_yinBuffer[tau] += delta * delta;
        }
    }
}

/**
 * Step 2: Calculate the cumulative mean on the normalized difference calculated in step 1.
 * This normalization helps find the true period rather than just the first minimum.
 */
void PitchDetector::yinCumulativeMeanNormalizedDifference(int halfLength)
{
    float runningSum = 0.0f;
    m_yinBuffer[0] = 1.0f;

    for (int tau = 1; tau < halfLength; ++tau)
    {
        runningSum += m_yinBuffer[tau];

        m_yinBuffer[tau] = (runningSum != 0.0f) ? m_yinBuffer[tau] * static_cast<float>(tau) / runningSum : 1.0f;
    }
}

/**
 * Step 3: Search through the normalized cumulative mean array and find values below threshold.
 * Returns the tau (lag) which produces the best autocorrelation, or -1 if not found.
 */
int PitchDetector::yinAbsoluteThreshold(int halfLength)
{
    // Calculate the minimum tau based on maximum frequency
    int minTau = static_cast<int>(m_sampleRate / c_maxFrequency);
    minTau = std::max(2, minTau); // At least 2 to avoid edge cases

    // Calculate the maximum tau based on minimum frequency
    int maxTau = static_cast<int>(m_sampleRate / c_minFrequency);
    maxTau = std::min(maxTau, halfLength);

    // Search through the array of cumulative mean values
    // Start from minTau to ignore high frequencies outside our range
    for (int tau = minTau; tau < maxTau; ++tau)
    {
        if (m_yinBuffer[tau] < m_yinThreshold)
        {
            // Look for the local minimum
            while (tau + 1 < maxTau && m_yinBuffer[tau + 1] < m_yinBuffer[tau])
            {
                ++tau;
            }

            // Store the m_probability
            // From the YIN paper: The threshold determines the list of
            // candidates admitted to the set, and can be interpreted as the
            // proportion of aperiodic power tolerated within a periodic signal.
            // Since we want periodicity and not aperiodicity:
            // periodicity = 1 - aperiodicity
            m_probability = 1.0f - m_yinBuffer[tau];

            return tau;
        }
    }

    // No pitch found
    m_probability = 0.0f;

    return -1;
}

/**
 * Step 4: Interpolate the shift value (tau) to improve the pitch estimate.
 * The 'best' shift value for autocorrelation is most likely not an integer shift.
 * As we only autocorrelated using integer shifts, we should check for a better
 * fractional shift value using parabolic interpolation.
 */
float PitchDetector::yinParabolicInterpolation(int tauEstimate, int halfLength)
{
    const int x0 = (tauEstimate < 1) ? tauEstimate : tauEstimate - 1;
    const int x2 = (tauEstimate + 1 < halfLength) ? tauEstimate + 1 : tauEstimate;

    // Algorithm to parabolically interpolate the shift value tau
    if (x0 == tauEstimate)
    {
        return (m_yinBuffer[tauEstimate] <= m_yinBuffer[x2]) ? static_cast<float>(tauEstimate) : static_cast<float>(x2);
    }
    else if (x2 == tauEstimate)
    {
        return (m_yinBuffer[tauEstimate] <= m_yinBuffer[x0]) ? static_cast<float>(tauEstimate) : static_cast<float>(x0);
    }

    const float s0 = m_yinBuffer[x0];
    const float s1 = m_yinBuffer[tauEstimate];
    const float s2 = m_yinBuffer[x2];

    // Fixed AUBIO implementation (thanks to Karl Helgason)
    const float denominator = 2.0f * (2.0f * s1 - s2 - s0);

    return (std::abs(denominator) > 1e-10f) ? static_cast<float>(tauEstimate) + (s2 - s0) / denominator : static_cast<float>(tauEstimate);
}

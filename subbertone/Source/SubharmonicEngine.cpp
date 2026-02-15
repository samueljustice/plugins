#include "SubharmonicEngine.h"
#include <cmath>
#include <algorithm>

void SubharmonicEngine::prepare(double newSampleRate, int maxBlockSize)
{
    m_isPrepared = false;

    // Validate parameters
    if (newSampleRate <= 0 || maxBlockSize <= 0 || maxBlockSize > c_maxBlockSizeSamples)
        return;

    m_sampleRate = newSampleRate;
    m_currentMaxBlockSize = maxBlockSize;

    // Calculate envelope coefficients
    calculateEnvelopeCoefficients();

    m_distortionSmoothed.reset(m_sampleRate, c_parameterSmoothingSeconds);
    m_toneSmoothed.reset(m_sampleRate, c_parameterSmoothingSeconds);
    m_postDriveLowpassSmoothed.reset(m_sampleRate, c_parameterSmoothingSeconds);
    m_distortionSmoothed.setCurrentAndTargetValue(0.0f);
    m_toneSmoothed.setCurrentAndTargetValue(1000.0f);
    m_postDriveLowpassSmoothed.setCurrentAndTargetValue(20000.0f);

    // Reset state
    m_currentFrequency = 0.0;
    m_targetFrequency = 0.0;
    m_lastSetFrequency = 0.0;
    m_envelopeFollower = 0.0;
    m_envelopeTarget = 0.0;
    m_signalPresent = false;
    m_releaseGain = 1.0;
    m_signalOnCounter = 0;
    m_signalOffCounter = 0;
    m_filterUpdateCounter = 0;

    m_signalOnThreshold = static_cast<int>(m_sampleRate * 0.0013);
    m_signalOffThreshold = static_cast<int>(m_sampleRate * 0.5);

    // Resize buffers with error checking
    const size_t normalSize = static_cast<size_t>(maxBlockSize);

    m_sineBuffer.resize(normalSize);
    m_cleanSineBuffer.resize(normalSize);
    m_harmonicResidualBuffer.resize(normalSize);

    // Clear buffers
    std::fill(m_sineBuffer.begin(), m_sineBuffer.end(), 0.0f);
    std::fill(m_cleanSineBuffer.begin(), m_cleanSineBuffer.end(), 0.0f);
    std::fill(m_harmonicResidualBuffer.begin(), m_harmonicResidualBuffer.end(), 0.0f);

    // Prepare filters and oscillator
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = m_sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(maxBlockSize);
    spec.numChannels = 1;

    // Initialize the band-limited sine oscillator with 512-point wavetable
    m_sineOscillator.prepare(spec);
    // Use a larger wavetable for better quality at low frequencies
    m_sineOscillator.initialise([](float x) { return std::sin(x); }, 2048);
    m_sineOscillator.setFrequency(100.0f); // Start with a default frequency to avoid startup artifacts

    // Tone filter
    m_toneFilter.prepare(spec);
    m_toneFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    m_toneFilter.reset();

    // Post-drive lowpass
    m_postDriveLowpassFilter.prepare(spec);
    m_postDriveLowpassFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    m_postDriveLowpassFilter.reset();

    // High-pass filter for DC blocking
    m_highpassFilter.prepare(spec);
    m_highpassFilter.setType(juce::dsp::StateVariableTPTFilterType::highpass);
    m_highpassFilter.setCutoffFrequency(20.0f);
    m_highpassFilter.reset();

    // Post-subtraction filter to smooth harmonic extraction artifacts
    m_postSubtractionFilter.prepare(spec);
    m_postSubtractionFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    m_postSubtractionFilter.setCutoffFrequency(4000.0f);  // Gentle roll-off
    m_postSubtractionFilter.reset();

    // Low frequency smoothing filter - helps with sub-100Hz oscillation
    m_lowFreqSmoothingFilter.prepare(spec);
    m_lowFreqSmoothingFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    m_lowFreqSmoothingFilter.setCutoffFrequency(200.0f);  // Smooth low frequencies
    m_lowFreqSmoothingFilter.reset();

    // Anti-aliasing filters - prepare at native rate
    const float cutoffFreq = static_cast<float>(spec.sampleRate * 0.45);

    const juce::dsp::IIR::Coefficients<float>::Ptr coeffs1 = juce::dsp::IIR::Coefficients<float>::makeLowPass(spec.sampleRate, cutoffFreq, 0.3f);
    const juce::dsp::IIR::Coefficients<float>::Ptr coeffs2 = juce::dsp::IIR::Coefficients<float>::makeLowPass(spec.sampleRate, cutoffFreq, 0.3f);

    m_antiAliasingFilter1.prepare(spec);
    m_antiAliasingFilter1.coefficients = coeffs1;
    m_antiAliasingFilter1.reset();

    m_antiAliasingFilter2.prepare(spec);
    m_antiAliasingFilter2.coefficients = coeffs2;
    m_antiAliasingFilter2.reset();

    // Pre-distortion filter
    const juce::dsp::IIR::Coefficients<float>::Ptr preDistCoeffs = juce::dsp::IIR::Coefficients<float>::makeLowPass(spec.sampleRate, 2000.0f, 0.7f);
    m_preDistortionFilter.prepare(spec);
    m_preDistortionFilter.coefficients = preDistCoeffs;
    m_preDistortionFilter.reset();

    // DC blocking filter - 20Hz high-pass
    const juce::dsp::IIR::Coefficients<float>::Ptr dcBlockCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass(spec.sampleRate, 20.0f, 0.7f);
    m_dcBlockingFilter.prepare(spec);
    m_dcBlockingFilter.coefficients = dcBlockCoeffs;
    m_dcBlockingFilter.reset();

    m_isPrepared = true;  // Mark as prepared after all setup is complete
}

void SubharmonicEngine::calculateEnvelopeCoefficients()
{
    // Convert time constants to filter coefficients
    // Using the standard one-pole filter formula: coeff = 1 - exp(-1/(time_in_samples))
    const double attackSamples = (c_attackTimeMs / 1000.0) * m_sampleRate;
    const double releaseSamples = (c_releaseTimeMs / 1000.0) * m_sampleRate;

    m_attackCoeff  = 1.0 - std::exp(-1.0 / attackSamples);
    m_releaseCoeff = 1.0 - std::exp(-1.0 / releaseSamples);
}

void SubharmonicEngine::updateEnvelope(bool signalDetected)
{
    // Make envelope floor frequency-dependent - lower frequencies get lower floor
    // This prevents rumble at very low frequencies
    double dynamicFloor = 0.0;

    if (m_currentFrequency > c_minSignalFrequency)
    {
        dynamicFloor = juce::jmap(m_currentFrequency, 20.0, 100.0, 0.0, c_envelopeFloor);
    }

    // If no signal is present, release smoothly to silence
    if (!m_signalPresent)
    {
        m_envelopeTarget = 0.0;
    }

    // Only start fading if signal has been lost for the hold time
    // Otherwise keep envelope at full volume
    if (m_signalPresent && (signalDetected || m_signalOffCounter < m_signalOffThreshold))
    {
        m_envelopeTarget = 1.0;  // Keep at full volume during hold time
    }
    else if (m_signalPresent)
    {
        m_envelopeTarget = dynamicFloor;  // Fade to floor level, not zero
    }

    // Apply one-pole filter for smooth transitions
    const double coeff = (m_envelopeTarget > m_envelopeFollower) ? m_attackCoeff : m_releaseCoeff;
    m_envelopeFollower += coeff * (m_envelopeTarget - m_envelopeFollower);

    // Ensure we never go below the floor
    if (m_signalPresent && m_envelopeFollower < dynamicFloor)
        m_envelopeFollower = dynamicFloor;
}

void SubharmonicEngine::process(float* outputBuffer,
                                int numSamples,
                                float fundamental,
                                float distortionAmount,
                                int distortionType,
                                float toneFreq,
                                float postDriveLowpass,
                                bool inputActive)
{
    // Validate parameters and check if engine is prepared
    if (!outputBuffer || numSamples <= 0 || numSamples > c_maxBlockSizeSamples || !m_isPrepared)
    {
        if (outputBuffer && numSamples > 0 && numSamples <= c_maxBlockSizeSamples)
            std::fill_n(outputBuffer, numSamples, 0.0f);

        return;
    }

    // Check if block size is within expected range
    if (numSamples > m_currentMaxBlockSize)
    {
        std::fill_n(outputBuffer, numSamples, 0.0f);

        return;
    }

    // Ensure buffers are properly sized
    const size_t requiredSize = static_cast<size_t>(numSamples);

    // Only clear the harmonic residual buffer - don't clear sine buffers as it causes discontinuities
    if (m_sineBuffer.size() < requiredSize || m_cleanSineBuffer.size() < requiredSize || m_harmonicResidualBuffer.size() < requiredSize)
    {
        // Buffers not properly initialized
        std::fill_n(outputBuffer, numSamples, 0.0f);

        return;
    }

    std::fill(m_harmonicResidualBuffer.begin(), m_harmonicResidualBuffer.begin() + requiredSize, 0.0f);

    // Signal detection with hysteresis
    const bool currentSignalDetected = inputActive && fundamental > c_minSignalFrequency && fundamental < c_maxSignalFrequency;

    if (currentSignalDetected)
    {
        m_signalOnCounter += numSamples;  // Count samples, not blocks!
        m_signalOffCounter = 0;  // Reset off counter when signal is present

        // Require signal to be present for a few samples before turning on
        if (!m_signalPresent && m_signalOnCounter >= m_signalOnThreshold)
        {
            m_signalPresent = true;
            // Start envelope at a reasonable level to avoid ramping artifacts
            // This prevents the square-wave appearance during attack
            // Don't jump the envelope - let it ramp smoothly
            // This prevents clicks at low frequencies
            m_envelopeTarget = 1.0;
        }

        // Update target frequency with Nyquist limit to prevent aliasing
        const float nyquist = static_cast<float>(m_sampleRate * 0.5);
        const float subFreq = fundamental * 0.5f; // One octave below

        m_targetFrequency = std::clamp(subFreq, c_minSignalFrequency, nyquist * 0.9f); // Clamp to 90% of Nyquist
    }
    else
    {
        m_signalOnCounter = 0;

        // Use hysteresis for turning off to prevent flickering
        if (m_signalPresent)
        {
            m_signalOffCounter += numSamples;

            // Only turn off if signal has been absent for the threshold duration
            if (m_signalOffCounter >= m_signalOffThreshold || !inputActive)
            {
                m_signalPresent = false;
                m_signalOffCounter = 0;
            }
        }
    }

    // Set smoothed targets for sample-accurate control
    float clampedToneFreq = std::clamp(toneFreq, c_minToneHz, c_maxToneHz);

    if (m_currentFrequency > c_minSignalFrequency)
        clampedToneFreq = std::min(clampedToneFreq, static_cast<float>(m_currentFrequency * 2.5));

    m_distortionSmoothed.setTargetValue(distortionAmount);
    m_toneSmoothed.setTargetValue(clampedToneFreq);
    m_postDriveLowpassSmoothed.setTargetValue(postDriveLowpass);

    // Update frequency once per block for smooth transitions
    if (m_signalPresent && m_targetFrequency > 0.0)
    {
        // Initialize m_currentFrequency if it's zero
        if (m_currentFrequency == 0.0)
        {
            m_currentFrequency = m_targetFrequency;
        }
        else
        {
            // Use adaptive frequency smoothing - faster for low frequencies
            double adaptiveSmoothingCoeff = c_frequencySmoothingCoeff;
            if (m_currentFrequency < 200.0)
            {
                // Less smoothing for low frequencies to prevent pitch drift
                const double clampedFrequency = std::clamp(m_currentFrequency, 30.0, 200.0);
                adaptiveSmoothingCoeff = juce::jmap(clampedFrequency, 30.0, 200.0, 0.9, 0.99);
            }

            m_currentFrequency = m_currentFrequency * adaptiveSmoothingCoeff + m_targetFrequency * (1.0 - adaptiveSmoothingCoeff);
        }

        // Only update oscillator frequency if it has changed significantly
        // For low frequencies, be even more conservative to maintain phase
        const double frequencyTolerance = (m_currentFrequency < 100.0) ? 0.5 : 0.1;

        if (std::abs(m_currentFrequency - m_lastSetFrequency) > frequencyTolerance)
        {
            m_sineOscillator.setFrequency(static_cast<float>(m_currentFrequency));
            m_lastSetFrequency = m_currentFrequency;
        }
    }
    // Keep the oscillator running and fade with envelope when no signal is present.

    // Update envelope only once per block to prevent stepping artifacts
    // This ensures consistent amplitude throughout the buffer
    updateEnvelope(currentSignalDetected);

    const float blockEnvelope = static_cast<float>(m_envelopeFollower);

    // If we've fully faded out, stop the oscillator to avoid low-level leakage
    if (!m_signalPresent && m_envelopeFollower <= c_envelopeSilenceThreshold)
    {
        m_currentFrequency = 0.0;
        m_targetFrequency = 0.0;
        m_lastSetFrequency = 0.0;
        m_sineOscillator.reset();
    }

    // Generate sine wave with consistent envelope for this block
    for (int i = 0; i < numSamples; ++i)
    {
        // Always generate sine wave to maintain phase continuity
        float filtered = 0.0f;

        if (m_currentFrequency > c_minSignalFrequency)  // Only if we have a valid frequency
        {
            // Generate band-limited sine wave using JUCE's oscillator
            // The oscillator expects a zero input for pure sine generation
            float sineSample = m_sineOscillator.processSample(0.0f);

            // Ensure the sine wave is properly scaled with more headroom
            sineSample *= c_sineHeadroom; // More headroom to prevent clipping

            // Apply DC blocker first to prevent any DC offset issues
            const float dcCorrected = m_dcBlockingFilter.processSample(sineSample);

            // Apply tone filter after DC blocking
            if ((m_filterUpdateCounter++ % c_filterUpdateInterval) == 0)
                m_toneFilter.setCutoffFrequency(m_toneSmoothed.getNextValue());

            const float toneFiltered = m_toneFilter.processSample(0, dcCorrected);

            // THIS WAS PRODUCING A DISCONTINUITY IN THE SIGNAL WHEN DOING BIG JUMPS IN PITCH
            // For low frequencies, apply additional smoothing last
            //filtered = (m_currentFrequency < c_lowFreqSmoothingHz) ? m_lowFreqSmoothingFilter.processSample(0, toneFiltered) : toneFiltered;
            filtered = toneFiltered;
        }
        else
        {
            // No valid frequency - output silence and keep filters in sync
            filtered = m_toneFilter.processSample(0, 0.0f);
        }

        // Store clean sine AFTER tone filter for better phase alignment
        m_cleanSineBuffer[static_cast<size_t>(i)] = filtered;

        // Only copy to m_sineBuffer if we're actually going to distort
        // This prevents any artifacts when distortion is disabled
        m_sineBuffer[static_cast<size_t>(i)] = (m_distortionSmoothed.getCurrentValue() > 0.01f) ? filtered : 0.0f;
    }

    // If there's no signal present, let the envelope fade out smoothly.

    // Process without oversampling

    for (int i = 0; i < numSamples; ++i)
    {
        float sample = m_sineBuffer[static_cast<size_t>(i)];

        // Pre-distortion filtering
        sample = m_preDistortionFilter.processSample(sample);

        // Apply distortion
        sample = applyDistortion(sample, m_distortionSmoothed.getNextValue(), distortionType);

        // Post-drive lowpass
        if ((m_filterUpdateCounter++ % c_filterUpdateInterval) == 0)
            m_postDriveLowpassFilter.setCutoffFrequency(m_postDriveLowpassSmoothed.getNextValue());

        sample = m_postDriveLowpassFilter.processSample(0, sample);

        // Anti-aliasing filters (light filtering at native rate)
        sample = m_antiAliasingFilter1.processSample(sample);
        sample = m_antiAliasingFilter2.processSample(sample);

        m_sineBuffer[static_cast<size_t>(i)] = sample;
    }

    // Data flow: clean sine -> optional distortion -> harmonic extraction -> mix -> output.
    // Mix original sine and distorted signals with bounds checking
    for (int i = 0; i < numSamples; ++i)
    {
        const size_t idx = static_cast<size_t>(i);

        // Bounds check
        if (idx >= m_cleanSineBuffer.size() || idx >= m_sineBuffer.size())
        {
            outputBuffer[i] = 0.0f;
            continue;
        }

        // Get the original clean sine wave
        float sine = m_cleanSineBuffer[idx];

        // Get the distorted signal
        float distorted = m_sineBuffer[idx]; // Now contains the processed signal

        // Clamp values to prevent extreme outputs
        sine = std::max(-1.0f, std::min(1.0f, sine));
        distorted = std::max(-1.0f, std::min(1.0f, distorted));

        // Apply the consistent block envelope
        float envelopedSine = sine * blockEnvelope;
        float envelopedDistorted = distorted * blockEnvelope;

        // When distortion is minimal, output the clean sine wave
        // When distortion is high, extract the harmonic content
        float output;

        if (m_distortionSmoothed.getCurrentValue() < 0.01f)
        {
            // No distortion - output clean subharmonic sine wave
            output = envelopedSine;

            // Keep filters in sync
            m_highpassFilter.processSample(0, 0.0f);
            m_postSubtractionFilter.processSample(0, 0.0f);

            if (idx < m_harmonicResidualBuffer.size())
            {
                m_harmonicResidualBuffer[idx] = 0.0f;
            }
        }
        else
        {
            // Harmonic extraction for distorted signals
            // Subtract clean from distorted to isolate harmonics
            const float harmonics = envelopedDistorted - envelopedSine;

            // Apply high-pass filter to remove DC
            const float highPassed = m_highpassFilter.processSample(0, harmonics);

            // Apply post-subtraction lowpass to smooth any harsh artifacts
            const float smoothedHarmonics = m_postSubtractionFilter.processSample(0, highPassed);

            // Store harmonic residual for visualization
            if (idx < m_harmonicResidualBuffer.size())
            {
                m_harmonicResidualBuffer[idx] = smoothedHarmonics;
            }

            // Mix between clean sine and extracted harmonics based on distortion amount
            const float harmonicMix = std::min(1.0f, m_distortionSmoothed.getCurrentValue() * 2.0f); // Scale up for more audible effect
            output = (envelopedSine * (1.0f - harmonicMix) + smoothedHarmonics * harmonicMix);
        }

        // Final output clamping
        outputBuffer[i] = std::max(-1.0f, std::min(1.0f, output));
    }

    // Apply release gain when signal is lost to fade to silence smoothly.
    if (m_signalPresent)
    {
        m_releaseGain = 1.0;
    }
    else
    {
        const double releaseMultiplier = 1.0 - m_releaseCoeff;

        for (int i = 0; i < numSamples; ++i)
        {
            outputBuffer[i] *= static_cast<float>(m_releaseGain);
            m_releaseGain *= releaseMultiplier;
        }

        if (m_releaseGain <= c_envelopeSilenceThreshold)
        {
            m_releaseGain = 0.0;
            m_envelopeFollower = 0.0;
            m_envelopeTarget = 0.0;
            m_currentFrequency = 0.0;
            m_targetFrequency = 0.0;
            m_lastSetFrequency = 0.0;

            m_sineOscillator.reset();
        }
    }
}

float SubharmonicEngine::applyDistortion(float sample, float amount, int type)
{
    if (amount < 0.001f)
        return sample;

    // More gentle pre-gain for analogue-style saturation
    const float drive = 1.0f + amount * 1.5f;  // Less aggressive drive

    switch (type)
    {
        case SoftClip:  // Renamed to "Tape Saturation" in UI
        {
            // Smooth tanh saturation - classic analogue tape emulation
            const float x = sample * drive;
            return std::tanh(x);  // Direct tanh without normalization
        }

        case HardClip:  // Renamed to "Valve Warmth" in UI
        {
            // Valve/tube-style saturation with 2nd and 3rd harmonics
            const float x = sample * drive;

            // Asymmetric tube curve for even harmonics
            if (x > 0)
            {
                const float out = 1.5f * x - 0.5f * x * x * x;
                return std::clamp(out, -1.0f, 1.0f);  // Clamp instead of normalize
            }
            else
            {
                // Slightly different curve for negative side (tube asymmetry)
                const float out = 1.4f * x - 0.6f * x * x * x;
                return std::clamp(out, -1.0f, 1.0f);
            }
        }

        case Tube:  // Renamed to "Console Drive" in UI
        {
            // Console preamp-style saturation
            const float x = sample * drive;

            // Soft knee compression curve
            const float threshold = 0.7f;

            if (std::abs(x) <= threshold)
            {
                return x;
            }
            else
            {
                const float sign = x > 0 ? 1.0f : -1.0f;
                const float excess = std::abs(x) - threshold;

                // Smooth logarithmic curve above threshold
                return sign * (threshold + std::log1p(excess) * 0.5f);
            }
        }

        case Foldback:  // Renamed to "Transformer" in UI
        {
            // Transformer-style saturation with gentle S-curve
            const float x = sample * drive * 0.7f;  // Reduced gain for gentler effect

            // Classic S-curve using arctangent
            return (2.0f / juce::MathConstants<float>::pi) * std::atan(x * juce::MathConstants<float>::pi / 2.0f);
        }

        default:
            break;
    }

    return sample;
}

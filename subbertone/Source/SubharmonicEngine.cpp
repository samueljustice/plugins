#include "SubharmonicEngine.h"

SubharmonicEngine::SubharmonicEngine()
{
    // 8x oversampling for anti-aliasing
    oversampler = std::make_unique<juce::dsp::Oversampling<float>>(
        1, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true);
}

SubharmonicEngine::~SubharmonicEngine() = default;

void SubharmonicEngine::prepare(double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    
    // Calculate envelope rates
    attackRate = 1.0 / (attackTime * sampleRate);
    releaseRate = 1.0 / (releaseTime * sampleRate);
    
    // Reset state
    phase = 0.0;
    currentFrequency = 0.0;
    targetFrequency = 0.0;
    envelopeLevel = 0.0;
    envelopeState = EnvelopeState::Idle;
    signalDetected = false;
    previousSignalDetected = false;
    signalHoldoffCounter = 0;
    
    // Reset DC blocker
    dcBlockerX1 = 0.0f;
    dcBlockerY1 = 0.0f;
    
    // Resize buffers for oversampling (8x)
    sineBuffer.resize(maxBlockSize * 8);
    cleanSineBuffer.resize(maxBlockSize);  // Store at native rate
    distortedBuffer.resize(maxBlockSize * 8);
    
    // Initialize oversampling
    oversampler->initProcessing(static_cast<size_t>(maxBlockSize));
    oversampler->reset();
    
    // Prepare filters
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = maxBlockSize;
    spec.numChannels = 1;
    
    // Prepare oversampled spec for filters that run at higher rate
    juce::dsp::ProcessSpec oversampledSpec = spec;
    oversampledSpec.sampleRate = sampleRate * 8.0;
    oversampledSpec.maximumBlockSize = maxBlockSize * 8;
    
    // Tone filter runs at native rate
    lowpassFilter.prepare(spec);
    lowpassFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    
    // Post-drive lowpass at oversampled rate
    postDriveLowpassFilter.prepare(oversampledSpec);
    postDriveLowpassFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    
    // High-pass filter for DC blocking at native rate
    highpassFilter.prepare(spec);
    highpassFilter.setType(juce::dsp::StateVariableTPTFilterType::highpass);
    highpassFilter.setCutoffFrequency(20.0f);
    
    // Anti-aliasing filters
    float cutoffFreq = static_cast<float>(oversampledSpec.sampleRate * 0.1);
    
    auto coeffs1 = juce::dsp::IIR::Coefficients<float>::makeLowPass(
        oversampledSpec.sampleRate, cutoffFreq, 0.3f);
    auto coeffs2 = juce::dsp::IIR::Coefficients<float>::makeLowPass(
        oversampledSpec.sampleRate, cutoffFreq, 0.3f);
    auto coeffs3 = juce::dsp::IIR::Coefficients<float>::makeLowPass(
        oversampledSpec.sampleRate, cutoffFreq, 0.3f);
    auto coeffs4 = juce::dsp::IIR::Coefficients<float>::makeLowPass(
        oversampledSpec.sampleRate, cutoffFreq, 0.3f);
    
    antiAliasingFilter1.prepare(oversampledSpec);
    antiAliasingFilter1.coefficients = coeffs1;
    antiAliasingFilter2.prepare(oversampledSpec);
    antiAliasingFilter2.coefficients = coeffs2;
    antiAliasingFilter3.prepare(oversampledSpec);
    antiAliasingFilter3.coefficients = coeffs3;
    antiAliasingFilter4.prepare(oversampledSpec);
    antiAliasingFilter4.coefficients = coeffs4;
    
    // Pre-distortion filter
    auto preDistCoeffs = juce::dsp::IIR::Coefficients<float>::makeLowPass(
        oversampledSpec.sampleRate, 2000.0f, 0.7f);
    preDistortionFilter.prepare(oversampledSpec);
    preDistortionFilter.coefficients = preDistCoeffs;
}

void SubharmonicEngine::process(const float* inputBuffer, float* outputBuffer, int numSamples, 
                               float fundamental, float distortionAmount, float inverseMixAmount,
                               int distortionType, float toneFreq, float postDriveLowpass)
{
    // Signal detection with hysteresis
    bool currentSignalDetected = fundamental > 20.0f && fundamental < 2000.0f;
    
    // Apply holdoff to prevent flutter
    if (currentSignalDetected)
    {
        signalHoldoffCounter = signalHoldoffSamples;
        signalDetected = true;
    }
    else if (signalHoldoffCounter > 0)
    {
        signalHoldoffCounter--;
        signalDetected = true; // Keep signal active during holdoff
    }
    else
    {
        signalDetected = false;
    }
    
    // Update target frequency when signal is detected
    if (signalDetected && fundamental > 20.0f)
    {
        float newTargetFreq = fundamental * 0.5f; // One octave below
        
        // Only update if change is significant
        if (std::abs(newTargetFreq - targetFrequency) > frequencyThreshold)
        {
            targetFrequency = newTargetFreq;
        }
    }
    
    // Set tone filter frequency
    lowpassFilter.setCutoffFrequency(toneFreq);
    
    // Generate sine wave with envelope
    for (int i = 0; i < numSamples; ++i)
    {
        // Update envelope
        updateEnvelope();
        
        // Apply frequency slewing
        if (targetFrequency > 0.0)
        {
            currentFrequency = currentFrequency * frequencySlewRate + 
                              targetFrequency * (1.0 - frequencySlewRate);
        }
        
        // Generate sine wave
        float sineSample = static_cast<float>(generateSineWave() * envelopeLevel);
        
        // Apply tone filter
        sineSample = lowpassFilter.processSample(0, sineSample);
        
        // Store clean sine for mixing later
        cleanSineBuffer[i] = sineSample;
        
        // Store in buffer for oversampling
        sineBuffer[i] = sineSample;
    }
    
    // Oversample for distortion processing
    float* sineData = sineBuffer.data();
    auto oversampledBlock = oversampler->processSamplesUp(
        juce::dsp::AudioBlock<float>(&sineData, 1, 0, static_cast<size_t>(numSamples)));
    
    float* oversampledData = oversampledBlock.getChannelPointer(0);
    size_t oversampledLength = oversampledBlock.getNumSamples();
    
    // Apply distortion and filters at oversampled rate
    postDriveLowpassFilter.setCutoffFrequency(postDriveLowpass);
    
    for (size_t i = 0; i < oversampledLength; ++i)
    {
        float sample = oversampledData[i];
        
        // Pre-distortion filtering
        sample = preDistortionFilter.processSample(sample);
        
        // Apply distortion
        float distorted = applyDistortion(sample, distortionAmount, distortionType);
        
        // Post-drive lowpass
        distorted = postDriveLowpassFilter.processSample(0, distorted);
        
        // Cascaded anti-aliasing filters
        distorted = antiAliasingFilter1.processSample(distorted);
        distorted = antiAliasingFilter2.processSample(distorted);
        distorted = antiAliasingFilter3.processSample(distorted);
        distorted = antiAliasingFilter4.processSample(distorted);
        
        distortedBuffer[i] = distorted;
    }
    
    // Copy distorted signal back
    std::copy(distortedBuffer.begin(), distortedBuffer.begin() + oversampledLength, oversampledData);
    
    // Downsample back to original rate
    oversampler->processSamplesDown(oversampledBlock);
    
    // Mix original sine and distorted signals
    for (int i = 0; i < numSamples; ++i)
    {
        // Get the original clean sine wave
        float sine = cleanSineBuffer[i];
        
        // Get the distorted signal
        float distorted = sineBuffer[i]; // Now contains the processed signal
        
        // Inverse mix: blend between clean sine and distorted
        float output = sine * (1.0f - inverseMixAmount) + distorted * inverseMixAmount;
        
        // DC blocker
        float dcBlockedOutput = output - dcBlockerX1 + dcBlockerCutoff * dcBlockerY1;
        dcBlockerY1 = dcBlockedOutput;
        dcBlockerX1 = output;
        
        // Apply high-pass filter for additional DC removal
        output = highpassFilter.processSample(0, dcBlockedOutput);
        
        outputBuffer[i] = output;
    }
    
    previousSignalDetected = signalDetected;
}

void SubharmonicEngine::updateEnvelope()
{
    switch (envelopeState)
    {
        case EnvelopeState::Idle:
            if (signalDetected)
            {
                envelopeState = EnvelopeState::Attack;
            }
            break;
            
        case EnvelopeState::Attack:
            envelopeLevel += attackRate;
            if (envelopeLevel >= 1.0)
            {
                envelopeLevel = 1.0;
                envelopeState = EnvelopeState::Sustain;
            }
            break;
            
        case EnvelopeState::Sustain:
            if (!signalDetected)
            {
                envelopeState = EnvelopeState::Release;
            }
            break;
            
        case EnvelopeState::Release:
            envelopeLevel -= releaseRate;
            if (envelopeLevel <= 0.0)
            {
                envelopeLevel = 0.0;
                envelopeState = EnvelopeState::Idle;
            }
            else if (signalDetected)
            {
                // If signal comes back during release, go back to attack
                envelopeState = EnvelopeState::Attack;
            }
            break;
    }
}

double SubharmonicEngine::generateSineWave()
{
    if (currentFrequency <= 0.0)
        return 0.0;
    
    // Calculate phase increment
    double phaseIncrement = (2.0 * juce::MathConstants<double>::pi * currentFrequency) / sampleRate;
    
    // Generate sine wave
    double sineValue = std::sin(phase);
    
    // Update phase
    phase += phaseIncrement;
    
    // Wrap phase to prevent numerical issues
    while (phase >= 2.0 * juce::MathConstants<double>::pi)
        phase -= 2.0 * juce::MathConstants<double>::pi;
    
    return sineValue;
}

float SubharmonicEngine::applyDistortion(float sample, float amount, int type)
{
    if (amount < 0.001f) return sample;
    
    // Increase pre-gain to ensure distortion actually adds harmonics
    float gainedSample = sample * (1.0f + amount * 2.0f);
    
    switch (type)
    {
        case SoftClip:
        {
            // Band-limited cubic soft clipper
            float x = gainedSample * amount;
            if (std::abs(x) > 1.5f)
            {
                // Soft limit for extreme values
                return (x > 0 ? 1.0f : -1.0f) * (1.0f - std::exp(-std::abs(x) + 1.5f));
            }
            else
            {
                // Cubic polynomial - generates only 3rd harmonic
                return x - (x * x * x) / 3.0f;
            }
        }
            
        case HardClip:
        {
            // Band-limited hard clipper using polynomial
            float threshold = 1.0f - amount * 0.3f;
            
            if (std::abs(gainedSample) < threshold)
            {
                return gainedSample;
            }
            else
            {
                // Cubic limiting at threshold
                float sign = gainedSample > 0 ? 1.0f : -1.0f;
                float excess = (std::abs(gainedSample) - threshold) / threshold;
                excess = std::min(excess, 1.0f);
                
                // Smooth polynomial transition
                float limited = threshold * (1.0f + excess * (1.0f - excess * excess / 3.0f));
                return sign * limited;
            }
        }
            
        case Tube:
        {
            // Band-limited tube using low-order polynomials
            float drive = 1.0f + amount * 1.5f;
            float x = gainedSample * drive;
            
            // 5th order polynomial approximation - smoother than tanh
            if (std::abs(x) < 1.7f)
            {
                float x2 = x * x;
                float x3 = x2 * x;
                float x5 = x3 * x2;
                return (x - x3 / 3.0f + x5 / 5.0f) / drive;
            }
            else
            {
                // Soft limiting for extreme values
                float sign = x > 0 ? 1.0f : -1.0f;
                return sign * (1.0f - std::exp(-std::abs(x) + 1.7f)) / drive;
            }
        }
            
        case Foldback:
        {
            // Band-limited foldback using parabolic folding
            float threshold = 1.0f - amount * 0.3f;
            float x = gainedSample;
            
            // Normalize to threshold units
            x = x / threshold;
            
            // Parabolic folding function - smoother than sine
            while (std::abs(x) > 1.0f)
            {
                if (x > 1.0f)
                {
                    float excess = x - 1.0f;
                    x = 1.0f - excess * excess * 0.5f;
                }
                else if (x < -1.0f)
                {
                    float excess = -x - 1.0f;
                    x = -(1.0f - excess * excess * 0.5f);
                }
                
                // Prevent infinite loops
                if (std::abs(x) > 2.0f)
                {
                    x = x > 0 ? 0.9f : -0.9f;
                    break;
                }
            }
            
            return x * threshold;
        }
            
        default:
            return sample;
    }
}
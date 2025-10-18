#include "SubharmonicEngine.h"
#include <cmath>
#include <algorithm>

SubharmonicEngine::SubharmonicEngine()
{
    // Don't create oversampler in constructor - wait for prepare
    oversampler = nullptr;
    currentMaxBlockSize = 0;
}

SubharmonicEngine::~SubharmonicEngine() = default;

void SubharmonicEngine::prepare(double newSampleRate, int maxBlockSize)
{
    isPrepared = false;
    oversamplerReady = false;
    
    // Validate parameters
    if (newSampleRate <= 0 || maxBlockSize <= 0 || maxBlockSize > 8192)
        return;
        
    sampleRate = newSampleRate;
    currentMaxBlockSize = maxBlockSize;
    
    // Calculate envelope coefficients
    calculateEnvelopeCoefficients();
    
    // Reset state
    currentFrequency = 0.0;
    targetFrequency = 0.0;
    lastSetFrequency = 0.0;
    envelopeFollower = 0.0;
    envelopeTarget = 0.0;
    signalPresent = false;
    signalOnCounter = 0;
    signalOffCounter = 0;
    
    // Resize buffers for oversampling (8x) with error checking
    try {
        size_t oversampledSize = static_cast<size_t>(maxBlockSize * 8);
        size_t normalSize = static_cast<size_t>(maxBlockSize);
        
        sineBuffer.resize(oversampledSize);
        cleanSineBuffer.resize(normalSize);
        distortedBuffer.resize(oversampledSize);
        harmonicResidualBuffer.resize(normalSize);
        
        // Clear buffers
        std::fill(sineBuffer.begin(), sineBuffer.end(), 0.0f);
        std::fill(cleanSineBuffer.begin(), cleanSineBuffer.end(), 0.0f);
        std::fill(distortedBuffer.begin(), distortedBuffer.end(), 0.0f);
        std::fill(harmonicResidualBuffer.begin(), harmonicResidualBuffer.end(), 0.0f);
    }
    catch (const std::exception& e) {
        // If allocation fails, create minimal buffers
        sineBuffer.resize(1024);
        cleanSineBuffer.resize(128);
        distortedBuffer.resize(1024);
        harmonicResidualBuffer.resize(128);
        std::fill(sineBuffer.begin(), sineBuffer.end(), 0.0f);
        std::fill(cleanSineBuffer.begin(), cleanSineBuffer.end(), 0.0f);
        std::fill(distortedBuffer.begin(), distortedBuffer.end(), 0.0f);
        std::fill(harmonicResidualBuffer.begin(), harmonicResidualBuffer.end(), 0.0f);
    }
    
    // Create oversampler for anti-aliasing when using distortion
    try {
        oversampler.reset();
        oversampler = std::make_unique<juce::dsp::Oversampling<float>>(
            1, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true);
        
        if (oversampler)
        {
            // Ensure proper initialization with valid block size
            size_t blockSize = static_cast<size_t>(maxBlockSize);
            if (blockSize > 0 && blockSize <= 8192)
            {
                oversampler->initProcessing(blockSize);
                oversampler->reset();
                
                // Verify oversampler is properly initialized
                juce::dsp::AudioBlock<float> testBlock;
                try {
                    float testData = 0.0f;
                    float* testPtr = &testData;
                    testBlock = juce::dsp::AudioBlock<float>(&testPtr, 1, 0, 1);
                    auto testResult = oversampler->processSamplesUp(testBlock);
                    if (testResult.getNumSamples() > 0)
                    {
                        oversamplerReady = true;
                    }
                    else
                    {
                        oversamplerReady = false;
                    }
                }
                catch (const std::exception& e)
                {
                    oversamplerReady = false;
                }
            }
            else
            {
                // Invalid block size - don't use oversampling
                oversampler.reset();
                oversamplerReady = false;
            }
        }
    }
    catch (const std::exception& e) {
        oversampler.reset();
        oversamplerReady = false;
    }
    
    // Prepare filters and oscillator
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(maxBlockSize);
    spec.numChannels = 1;
    
    // Initialize the band-limited sine oscillator
    currentAngle = 0.0;
    angleDelta = 0.0;
    
    // Prepare oversampled spec for filters that run at higher rate
    juce::dsp::ProcessSpec oversampledSpec = spec;
    oversampledSpec.sampleRate = sampleRate * 8.0;
    oversampledSpec.maximumBlockSize = static_cast<juce::uint32>(maxBlockSize * 8);
    
    // Tone filter runs at native rate
    toneFilter.prepare(spec);
    toneFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    
    // Post-drive lowpass at native rate (no oversampling)
    postDriveLowpassFilter.prepare(spec);
    postDriveLowpassFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    
    // High-pass filter for DC blocking at native rate
    highpassFilter.prepare(spec);
    highpassFilter.setType(juce::dsp::StateVariableTPTFilterType::highpass);
    highpassFilter.setCutoffFrequency(20.0f);
    
    // Post-subtraction filter to smooth harmonic extraction artifacts
    postSubtractionFilter.prepare(spec);
    postSubtractionFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    postSubtractionFilter.setCutoffFrequency(4000.0f);  // Gentle roll-off
    
    // Low frequency smoothing filter - helps with sub-100Hz oscillation
    lowFreqSmoothingFilter.prepare(spec);
    lowFreqSmoothingFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    lowFreqSmoothingFilter.setCutoffFrequency(200.0f);  // Smooth low frequencies
    
    // Anti-aliasing filters - prepare at native rate
    float cutoffFreq = static_cast<float>(spec.sampleRate * 0.45);
    
    auto coeffs1 = juce::dsp::IIR::Coefficients<float>::makeLowPass(
        spec.sampleRate, cutoffFreq, 0.3f);
    auto coeffs2 = juce::dsp::IIR::Coefficients<float>::makeLowPass(
        spec.sampleRate, cutoffFreq, 0.3f);
    auto coeffs3 = juce::dsp::IIR::Coefficients<float>::makeLowPass(
        spec.sampleRate, cutoffFreq, 0.3f);
    auto coeffs4 = juce::dsp::IIR::Coefficients<float>::makeLowPass(
        spec.sampleRate, cutoffFreq, 0.3f);
    
    antiAliasingFilter1.prepare(spec);
    antiAliasingFilter1.coefficients = coeffs1;
    antiAliasingFilter2.prepare(spec);
    antiAliasingFilter2.coefficients = coeffs2;
    antiAliasingFilter3.prepare(spec);
    antiAliasingFilter3.coefficients = coeffs3;
    antiAliasingFilter4.prepare(spec);
    antiAliasingFilter4.coefficients = coeffs4;
    
    // Pre-distortion filter
    auto preDistCoeffs = juce::dsp::IIR::Coefficients<float>::makeLowPass(
        spec.sampleRate, 2000.0f, 0.7f);
    preDistortionFilter.prepare(spec);
    preDistortionFilter.coefficients = preDistCoeffs;
    
    // DC blocking filter - 20Hz high-pass
    auto dcBlockCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass(
        spec.sampleRate, 20.0f, 0.7f);
    dcBlockingFilter.prepare(spec);
    dcBlockingFilter.coefficients = dcBlockCoeffs;
    
    isPrepared = true;  // Mark as prepared after all setup is complete
}

void SubharmonicEngine::calculateEnvelopeCoefficients()
{
    // Convert time constants to filter coefficients
    // Using the standard one-pole filter formula: coeff = 1 - exp(-1/(time_in_samples))
    double attackSamples = (attackTimeMs / 1000.0) * sampleRate;
    double releaseSamples = (releaseTimeMs / 1000.0) * sampleRate;
    
    attackCoeff = 1.0 - std::exp(-1.0 / attackSamples);
    releaseCoeff = 1.0 - std::exp(-1.0 / releaseSamples);
}


void SubharmonicEngine::updateEnvelope(bool signalDetected)
{

    double dynamicFloor = 0.0;
    if (currentFrequency > 20.0)
    {
        dynamicFloor = juce::jmap(currentFrequency, 20.0, 100.0, 0.0, 0.05);
    }
    
    if (!signalPresent)
    {
        envelopeTarget = dynamicFloor;  // Keep at floor level instead of zero
        envelopeFollower = dynamicFloor;  // Maintain phase continuity
        return;
    }
    
    if (signalDetected || signalOffCounter < signalOffThreshold)
    {
        envelopeTarget = 1.0;
    }
    else
    {
        envelopeTarget = dynamicFloor;  // Fade to floor level, not zero
    }
    
    double coeff = (envelopeTarget > envelopeFollower) ? attackCoeff : releaseCoeff;
    envelopeFollower += coeff * (envelopeTarget - envelopeFollower);
    
    if (envelopeFollower < dynamicFloor)
        envelopeFollower = dynamicFloor;
}

void SubharmonicEngine::process(float* outputBuffer, int numSamples,
                                float fundamental, float distortionAmount, float inverseMixAmount,
                                int distortionType, float toneFreq, float postDriveLowpass, bool inverseMixMode)
{
    if (!validateInputs(outputBuffer, numSamples)) return;
    
    updateSignalState(fundamental, numSamples);
    generateAudioBlock(outputBuffer, numSamples, distortionAmount, inverseMixAmount,
                       distortionType, toneFreq, postDriveLowpass, inverseMixMode);
}

bool SubharmonicEngine::validateInputs(float* outputBuffer, int numSamples)
{
    if (!outputBuffer || numSamples <= 0 || numSamples > 8192 || !isPrepared)
    {
        if (outputBuffer && numSamples > 0 && numSamples <= 8192)
            std::fill_n(outputBuffer, numSamples, 0.0f);
        return false;
    }
    
    if (numSamples > currentMaxBlockSize)
    {
        std::fill_n(outputBuffer, numSamples, 0.0f);
        return false;
    }
    
    size_t requiredSize = static_cast<size_t>(numSamples);
    std::fill(harmonicResidualBuffer.begin(), harmonicResidualBuffer.begin() + requiredSize, 0.0f);
    
    if (sineBuffer.size() < requiredSize || cleanSineBuffer.size() < requiredSize)
    {
        std::fill_n(outputBuffer, numSamples, 0.0f);
        return false;
    }
    
    return true;
}

void SubharmonicEngine::updateSignalState(float fundamental, int numSamples)
{
    bool currentSignalDetected = fundamental > 20.0f && fundamental < 2000.0f;
    
    if (currentSignalDetected)
    {
        signalOnCounter += numSamples;
        signalOffCounter = 0;
        
        if (!signalPresent && signalOnCounter >= signalOnThreshold)
        {
            signalPresent = true;
            envelopeTarget = 1.0;
        }
        
        float nyquist = static_cast<float>(sampleRate * 0.5);
        float subFreq = fundamental * 0.5f;
        targetFrequency = juce::jlimit(20.0f, nyquist * 0.9f, subFreq);
    }
    else
    {
        signalOnCounter = 0;
        
        if (signalPresent)
        {
            signalOffCounter += numSamples;
            if (signalOffCounter >= signalOffThreshold)
            {
                signalPresent = false;
                signalOffCounter = 0;
            }
        }
    }
    
    // Update frequency with smooth phase increment
    if (signalPresent && targetFrequency > 0.0)
    {
        if (currentFrequency == 0.0)
        {
            currentFrequency = targetFrequency;
            currentAngle = 0.0; // Reset phase on new signal
        }
        else
        {
            double adaptiveSmoothingCoeff = frequencySmoothingCoeff;
            if (currentFrequency < 200.0)
            {
                adaptiveSmoothingCoeff = juce::jmap(currentFrequency, 30.0, 200.0, 0.9, 0.99);
            }
            currentFrequency = currentFrequency * adaptiveSmoothingCoeff +
            targetFrequency * (1.0 - adaptiveSmoothingCoeff);
        }
        
        // Update angleDelta for smooth frequency changes
        double frequencyTolerance = (currentFrequency < 100.0) ? 0.5 : 0.1;
        if (std::abs(currentFrequency - lastSetFrequency) > frequencyTolerance)
        {
            angleDelta = (currentFrequency * 2.0 * juce::MathConstants<double>::pi) / sampleRate;
            lastSetFrequency = currentFrequency;
        }
    }
}

void SubharmonicEngine::generateAudioBlock(float* outputBuffer, int numSamples,
                                           float distortionAmount, float inverseMixAmount,
                                           int distortionType, float toneFreq, float postDriveLowpass, bool inverseMixMode)
{
    // Update filters
    float clampedToneFreq = juce::jlimit(40.0f, 20000.0f, toneFreq);
    if (currentFrequency > 20.0)
    {
        clampedToneFreq = std::min(clampedToneFreq, static_cast<float>(currentFrequency * 2.5));
    }
    toneFilter.setCutoffFrequency(clampedToneFreq);
    
    // Update envelope
    updateEnvelope(signalPresent);
    const float blockEnvelope = static_cast<float>(envelopeFollower);
    
    // Generate audio with PHASE CONTINUITY
    for (int i = 0; i < numSamples; ++i)
    {
        float sineSample = 0.0f;
        
        if (currentFrequency > 20.0 && blockEnvelope > 0.0001f)
        {
            // Use the existing currentAngle and angleDelta variables
            sineSample = static_cast<float>(std::sin(currentAngle)) * 0.7f;
            
            // Update phase using the existing angleDelta
            currentAngle += angleDelta;
            
            // Keep phase in range
            if (currentAngle >= 2.0 * juce::MathConstants<double>::pi)
                currentAngle -= 2.0 * juce::MathConstants<double>::pi;
            
            // Apply filters
            float dcCorrected = dcBlockingFilter.processSample(sineSample);
            float toneFiltered = toneFilter.processSample(0, dcCorrected);
            
            if (currentFrequency < 100.0)
            {
                sineSample = lowFreqSmoothingFilter.processSample(0, toneFiltered);
            }
            else
            {
                sineSample = toneFiltered;
            }
        }
        else
        {
            toneFilter.processSample(0, 0.0f);
            sineSample = 0.0f;
        }
        
        cleanSineBuffer[i] = sineSample * blockEnvelope;
        
        float distorted = sineSample;
        if (distortionAmount > 0.01f)
        {
            distorted = applyDistortion(sineSample, distortionAmount, distortionType);
        }
        
        outputBuffer[i] = distorted * blockEnvelope * inverseMixAmount;
    }
}


float SubharmonicEngine::applyDistortion(float sample, float amount, int type)
{
    if (amount < 0.001f) return sample;
    
    // More gentle pre-gain for analogue-style saturation
    float drive = 1.0f + amount * 1.5f;  // Less aggressive drive
    
    switch (type)
    {
        case SoftClip:  // Renamed to "Tape Saturation" in UI
        {
            // Smooth tanh saturation - classic analogue tape emulation
            float x = sample * drive;
            return std::tanh(x);  // Direct tanh without normalization
        }
            
        case HardClip:  // Renamed to "Valve Warmth" in UI
        {
            // Valve/tube-style saturation with 2nd and 3rd harmonics
            float x = sample * drive;
            
            // Asymmetric tube curve for even harmonics
            if (x > 0)
            {
                float out = 1.5f * x - 0.5f * x * x * x;
                return juce::jlimit(-1.0f, 1.0f, out);  // Clamp instead of normalize
            }
            else
            {
                // Slightly different curve for negative side (tube asymmetry)
                float out = 1.4f * x - 0.6f * x * x * x;
                return juce::jlimit(-1.0f, 1.0f, out);
            }
        }
            
        case Tube:  // Renamed to "Console Drive" in UI
        {
            // Console preamp-style saturation
            float x = sample * drive;
            
            // Soft knee compression curve
            float threshold = 0.7f;
            if (std::abs(x) <= threshold)
            {
                return x;
            }
            else
            {
                float sign = x > 0 ? 1.0f : -1.0f;
                float excess = std::abs(x) - threshold;
                
                // Smooth logarithmic curve above threshold
                return sign * (threshold + std::log1p(excess) * 0.5f);
            }
        }
            
        case Foldback:  // Renamed to "Transformer" in UI
        {
            // Transformer-style saturation with gentle S-curve
            float x = sample * drive * 0.7f;  // Reduced gain for gentler effect
            
            // Classic S-curve using arctangent
            return (2.0f / juce::MathConstants<float>::pi) * std::atan(x * juce::MathConstants<float>::pi / 2.0f);
        }
            
        default:
            return sample;
    }
}


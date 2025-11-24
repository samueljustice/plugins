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
    currentAngle = 0.0;
    angleDelta = 0.0;
    targetAngleDelta = 0.0;
    phaseIncrement = 0.0;
    envelopeFollower = 0.0;
    envelopeTarget = 0.0;
    sampleEnvelope = 0.0;
    signalPresent = false;
    signalOnCounter = 0;
    signalOffCounter = 0;
    settlingCounter = 0;
    
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
    
    // Tone filter runs at native rate
    toneFilter.prepare(spec);
    toneFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    
    // Post-drive lowpass at native rate
    postDriveLowpassFilter.prepare(spec);
    postDriveLowpassFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    
    // Post-subtraction filter to smooth harmonic extraction artifacts
    postSubtractionFilter.prepare(spec);
    postSubtractionFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    postSubtractionFilter.setCutoffFrequency(4000.0f);
    
    // Highpass filter for harmonic separation
    highpassFilter.prepare(spec);
    highpassFilter.setType(juce::dsp::StateVariableTPTFilterType::highpass);
    highpassFilter.setCutoffFrequency(40.0f);
    
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
        envelopeTarget = dynamicFloor;
        envelopeFollower = dynamicFloor;
        sampleEnvelope = dynamicFloor;
        return;
    }
    
    if (signalDetected || signalOffCounter < signalOffThreshold)
    {
        envelopeTarget = 1.0;
    }
    else
    {
        envelopeTarget = dynamicFloor;
    }
    
    double coeff = (envelopeTarget > envelopeFollower) ? attackCoeff : releaseCoeff;
    envelopeFollower += coeff * (envelopeTarget - envelopeFollower);
    sampleEnvelope = envelopeFollower;
    
    if (envelopeFollower < dynamicFloor)
        envelopeFollower = dynamicFloor;
}

void SubharmonicEngine::process(float* outputBuffer, int numSamples,
                                float fundamental, float distortionAmount, float inverseMixAmount,
                                int distortionType, float toneFreq, float postDriveLowpass, bool inverseMixMode)
{
    // Validation
    if (!outputBuffer || numSamples <= 0 || numSamples > 8192 || !isPrepared)
    {
        if (outputBuffer && numSamples > 0 && numSamples <= 8192)
            std::fill_n(outputBuffer, numSamples, 0.0f);
        return;
    }
    
    if (numSamples > currentMaxBlockSize)
    {
        std::fill_n(outputBuffer, numSamples, 0.0f);
        return;
    }
    
    size_t requiredSize = static_cast<size_t>(numSamples);
    size_t oversampledSize = requiredSize * 8;
    
    std::fill(harmonicResidualBuffer.begin(), harmonicResidualBuffer.begin() + requiredSize, 0.0f);
    
    if (sineBuffer.size() < oversampledSize ||
        cleanSineBuffer.size() < requiredSize ||
        distortedBuffer.size() < oversampledSize ||
        harmonicResidualBuffer.size() < requiredSize)
    {
        std::fill_n(outputBuffer, numSamples, 0.0f);
        return;
    }
    
    // Signal detection
    bool currentSignalDetected = fundamental > 20.0f && fundamental < 2000.0f;
    
    if (currentSignalDetected)
    {
        signalOnCounter += numSamples;
        signalOffCounter = 0;
        
        if (!signalPresent && signalOnCounter >= signalOnThreshold)
        {
            signalPresent = true;
            envelopeTarget = 1.0;
            settlingCounter = 0;
        }
        
        if (signalPresent && settlingCounter < settlingThreshold)
        {
            settlingCounter += numSamples;
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
                settlingCounter = 0;
            }
        }
    }
    
    toneFilter.setCutoffFrequency(juce::jlimit(40.0f, 20000.0f, toneFreq));
    
    if (signalPresent && targetFrequency > 0.0)
    {
        targetAngleDelta = (targetFrequency / sampleRate) * juce::MathConstants<double>::twoPi;
    }
    else
    {
        targetAngleDelta = 0.0;
    }
    
    updateEnvelope(signalPresent);
    
    static bool wasSignalPresent = false;
    if (signalPresent != wasSignalPresent)
    {
        toneFilter.reset();
        highpassFilter.reset();
        postSubtractionFilter.reset();
        wasSignalPresent = signalPresent;
    }
    
    double deltaStep = (targetAngleDelta - angleDelta) / static_cast<double>(numSamples);
    
    if (std::abs(deltaStep) > maxDeltaChangePerSample)
    {
        deltaStep = (deltaStep > 0.0) ? maxDeltaChangePerSample : -maxDeltaChangePerSample;
    }
    
    for (int i = 0; i < numSamples; ++i)
    {
        double coeff = (envelopeTarget > sampleEnvelope) ? attackCoeff : releaseCoeff;
        sampleEnvelope += coeff * (envelopeTarget - sampleEnvelope);
        
        angleDelta += deltaStep;
        angleDelta = juce::jlimit(0.0, targetAngleDelta * 1.1, angleDelta);
        
        float sineSample = 0.0f;
        if (angleDelta > 0.0)
        {
            sineSample = static_cast<float>(std::sin(currentAngle)) * 0.7f;
            currentAngle += angleDelta;
            
            while (currentAngle >= juce::MathConstants<double>::twoPi)
                currentAngle -= juce::MathConstants<double>::twoPi;
        }
        
        float filtered = toneFilter.processSample(0, sineSample);
        
        cleanSineBuffer[static_cast<size_t>(i)] = filtered * static_cast<float>(sampleEnvelope);
        
        if (distortionAmount > 0.01f)
        {
            sineBuffer[static_cast<size_t>(i)] = filtered * static_cast<float>(sampleEnvelope);
        }
        else
        {
            sineBuffer[static_cast<size_t>(i)] = 0.0f;
        }
    }
    
    postDriveLowpassFilter.setCutoffFrequency(postDriveLowpass);
    
    for (int i = 0; i < numSamples; ++i)
    {
        float sample = sineBuffer[static_cast<size_t>(i)];
        
        if (distortionAmount > 0.01f)
        {
            sample = applyDistortion(sample, distortionAmount, distortionType);
            sample = postDriveLowpassFilter.processSample(0, sample);
        }
        
        sineBuffer[static_cast<size_t>(i)] = sample;
    }
    
    for (int i = 0; i < numSamples; ++i)
    {
        size_t idx = static_cast<size_t>(i);
        
        if (idx >= cleanSineBuffer.size() || idx >= sineBuffer.size())
        {
            outputBuffer[i] = 0.0f;
            continue;
        }
        
        float sine = std::clamp(cleanSineBuffer[idx], -1.0f, 1.0f);
        float distorted = std::clamp(sineBuffer[idx], -1.0f, 1.0f);
        
        float output;
        
        if (distortionAmount < 0.01f)
        {
            output = inverseMixMode ? 0.0f : sine * inverseMixAmount;
            
            highpassFilter.processSample(0, 0.0f);
            postSubtractionFilter.processSample(0, 0.0f);
            
            if (idx < harmonicResidualBuffer.size())
            {
                harmonicResidualBuffer[idx] = 0.0f;
            }
        }
        else
        {
            float harmonics = distorted - sine;
            float highPassed = highpassFilter.processSample(0, harmonics);
            float smoothedHarmonics = postSubtractionFilter.processSample(0, highPassed);
            
            if (idx < harmonicResidualBuffer.size())
            {
                harmonicResidualBuffer[idx] = smoothedHarmonics;
            }
            
            if (inverseMixMode)
            {
                output = smoothedHarmonics * inverseMixAmount;
            }
            else
            {
                float harmonicMix = std::min(1.0f, distortionAmount * 2.0f);
                output = (sine * (1.0f - harmonicMix) + smoothedHarmonics * harmonicMix) * inverseMixAmount;
            }
        }
        
        outputBuffer[i] = std::clamp(output, -1.0f, 1.0f);
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

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
    
    // Initialize the band-limited sine oscillator with 512-point wavetable
    sineOscillator.prepare(spec);
    // Use a larger wavetable for better quality at low frequencies
    sineOscillator.initialise([](float x) { return std::sin(x); }, 2048);
    sineOscillator.setFrequency(100.0f); // Start with a default frequency to avoid startup artifacts
    
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
    // Make envelope floor frequency-dependent - lower frequencies get lower floor
    // This prevents rumble at very low frequencies
    double dynamicFloor = 0.0;
    if (currentFrequency > 20.0)
    {
        dynamicFloor = juce::jmap(currentFrequency, 20.0, 100.0, 0.0, 0.05);
    }
    
    // Only update envelope if signal is actually present
    // This prevents ramping up when there's no signal
    if (!signalPresent)
    {
        envelopeTarget = dynamicFloor;  // Keep at floor level instead of zero
        envelopeFollower = dynamicFloor;  // Maintain phase continuity
        return;
    }
    
    // Only start fading if signal has been lost for the hold time
    // Otherwise keep envelope at full volume
    if (signalDetected || signalOffCounter < signalOffThreshold)
    {
        envelopeTarget = 1.0;  // Keep at full volume during hold time
    }
    else
    {
        envelopeTarget = dynamicFloor;  // Fade to floor level, not zero
    }
    
    // Apply one-pole filter for smooth transitions
    double coeff = (envelopeTarget > envelopeFollower) ? attackCoeff : releaseCoeff;
    envelopeFollower += coeff * (envelopeTarget - envelopeFollower);
    
    // Ensure we never go below the floor
    if (envelopeFollower < dynamicFloor)
        envelopeFollower = dynamicFloor;
}

void SubharmonicEngine::process(float* outputBuffer, int numSamples, 
                               float fundamental, float distortionAmount, float inverseMixAmount,
                               int distortionType, float toneFreq, float postDriveLowpass)
{
    // Validate parameters and check if engine is prepared
    if (!outputBuffer || numSamples <= 0 || numSamples > 8192 || !isPrepared)
    {
        if (outputBuffer && numSamples > 0 && numSamples <= 8192)
            std::fill_n(outputBuffer, numSamples, 0.0f);
        return;
    }
    
    // Check if block size is within expected range
    if (numSamples > currentMaxBlockSize)
    {
        std::fill_n(outputBuffer, numSamples, 0.0f);
        return;
    }
        
    // Ensure buffers are properly sized
    size_t requiredSize = static_cast<size_t>(numSamples);
    size_t oversampledSize = requiredSize * 8;
    
    // Only clear the harmonic residual buffer - don't clear sine buffers as it causes discontinuities
    std::fill(harmonicResidualBuffer.begin(), harmonicResidualBuffer.begin() + requiredSize, 0.0f);
    
    if (sineBuffer.size() < oversampledSize || 
        cleanSineBuffer.size() < requiredSize ||
        distortedBuffer.size() < oversampledSize ||
        harmonicResidualBuffer.size() < requiredSize)
    {
        // Buffers not properly initialized
        std::fill_n(outputBuffer, numSamples, 0.0f);
        return;
    }
    
    // Signal detection with hysteresis
    bool currentSignalDetected = fundamental > 20.0f && fundamental < 2000.0f;
    
    if (currentSignalDetected)
    {
        signalOnCounter += numSamples;  // Count samples, not blocks!
        signalOffCounter = 0;  // Reset off counter when signal is present
        
        // Require signal to be present for a few samples before turning on
        if (!signalPresent && signalOnCounter >= signalOnThreshold)
        {
            signalPresent = true;
            // Start envelope at a reasonable level to avoid ramping artifacts
            // This prevents the square-wave appearance during attack
            // Don't jump the envelope - let it ramp smoothly
            // This prevents clicks at low frequencies
            envelopeTarget = 1.0;
        }
        
        // Update target frequency with Nyquist limit to prevent aliasing
        float nyquist = static_cast<float>(sampleRate * 0.5);
        float subFreq = fundamental * 0.5f; // One octave below
        targetFrequency = juce::jlimit(20.0f, nyquist * 0.9f, subFreq); // Clamp to 90% of Nyquist
    }
    else
    {
        signalOnCounter = 0;
        
        // Use hysteresis for turning off to prevent flickering
        if (signalPresent)
        {
            signalOffCounter += numSamples;
            
            // Only turn off if signal has been absent for the threshold duration
            if (signalOffCounter >= signalOffThreshold)
            {
                signalPresent = false;
                signalOffCounter = 0;
                
                // Don't reset target frequency - keep it for potential resume
            }
            else
            {
                // Keep running during hold time
            }
        }
    }
    
    // Set tone filter frequency - use frequency-dependent filtering for cleaner output
    // For subharmonics, limit the tone filter to 2.5x the fundamental frequency
    float clampedToneFreq = juce::jlimit(40.0f, 20000.0f, toneFreq);
    if (currentFrequency > 20.0)
    {
        // Apply frequency-dependent filtering to remove potential aliasing
        clampedToneFreq = std::min(clampedToneFreq, static_cast<float>(currentFrequency * 2.5));
    }
    toneFilter.setCutoffFrequency(clampedToneFreq);
    
    // Update frequency once per block for smooth transitions
    if (signalPresent && targetFrequency > 0.0)
    {
        // Initialize currentFrequency if it's zero
        if (currentFrequency == 0.0)
        {
            currentFrequency = targetFrequency;
        }
        else
        {
            // Use adaptive frequency smoothing - faster for low frequencies
            double adaptiveSmoothingCoeff = frequencySmoothingCoeff;
            if (currentFrequency < 200.0)
            {
                // Less smoothing for low frequencies to prevent pitch drift
                adaptiveSmoothingCoeff = juce::jmap(currentFrequency, 30.0, 200.0, 0.9, 0.99);
            }
            currentFrequency = currentFrequency * adaptiveSmoothingCoeff + 
                              targetFrequency * (1.0 - adaptiveSmoothingCoeff);
        }
        
        // Only update oscillator frequency if it has changed significantly
        // For low frequencies, be even more conservative to maintain phase
        double frequencyTolerance = (currentFrequency < 100.0) ? 0.5 : 0.1;
        
        if (std::abs(currentFrequency - lastSetFrequency) > frequencyTolerance)
        {
            sineOscillator.setFrequency(static_cast<float>(currentFrequency));
            lastSetFrequency = currentFrequency;
        }
    }
    else
    {
        // Don't stop the oscillator - just fade it out with envelope
        // This prevents discontinuities when the signal returns
        // Keep the last valid frequency to avoid startup artifacts
        if (currentFrequency > 20.0)
        {
            // Keep oscillator running at last valid frequency
        }
    }
    
    // Update envelope only once per block to prevent stepping artifacts
    // This ensures consistent amplitude throughout the buffer
    updateEnvelope(signalPresent);
    const float blockEnvelope = static_cast<float>(envelopeFollower);
    
    // Generate sine wave with consistent envelope for this block
    for (int i = 0; i < numSamples; ++i)
    {
        // Always generate sine wave to maintain phase continuity
        float filtered = 0.0f;
        
        if (currentFrequency > 20.0)  // Only if we have a valid frequency
        {
            // Generate band-limited sine wave using JUCE's oscillator
            // The oscillator expects a zero input for pure sine generation
            float sineSample = sineOscillator.processSample(0.0f);
            
            // Ensure the sine wave is properly scaled with more headroom
            sineSample *= 0.7f; // More headroom to prevent clipping
            
            // Apply DC blocker first to prevent any DC offset issues
            float dcCorrected = dcBlockingFilter.processSample(sineSample);
            
            // Apply tone filter after DC blocking
            float toneFiltered = toneFilter.processSample(0, dcCorrected);
            
            // For low frequencies, apply additional smoothing last
            if (currentFrequency < 100.0)
            {
                filtered = lowFreqSmoothingFilter.processSample(0, toneFiltered);
            }
            else
            {
                filtered = toneFiltered;
            }
        }
        else
        {
            // No valid frequency - output silence and keep filters in sync
            filtered = toneFilter.processSample(0, 0.0f);
        }
        
        // Store clean sine AFTER tone filter for better phase alignment
        cleanSineBuffer[static_cast<size_t>(i)] = filtered;
        
        // Only copy to sineBuffer if we're actually going to distort
        // This prevents any artifacts when distortion is disabled
        if (distortionAmount > 0.01f)
        {
            sineBuffer[static_cast<size_t>(i)] = filtered;
        }
        else
        {
            sineBuffer[static_cast<size_t>(i)] = 0.0f; // Keep it clean
        }
    }
    
    // If there's no signal present, skip all processing and output silence
    if (!signalPresent || envelopeFollower < 0.0001)
    {
        std::fill_n(outputBuffer, numSamples, 0.0f);
        // Clear sine buffers to prevent stale data from being used
        std::fill(sineBuffer.begin(), sineBuffer.begin() + numSamples, 0.0f);
        std::fill(cleanSineBuffer.begin(), cleanSineBuffer.begin() + numSamples, 0.0f);
        return;
    }
    
    // Check if there's any actual signal in the buffer
    float maxSample = 0.0f;
    for (int i = 0; i < std::min(32, numSamples); ++i)
    {
        maxSample = std::max(maxSample, std::abs(sineBuffer[static_cast<size_t>(i)]));
    }
    
    // Enable oversampling only when distortion is used AND there's actual signal
    bool useOversampling = false;
    
    // Defensive check - ensure block size hasn't changed
    if (useOversampling && numSamples > currentMaxBlockSize)
    {
        useOversampling = false;
    }
    
    if (!useOversampling)
    {
        // Process without oversampling for safety
        postDriveLowpassFilter.setCutoffFrequency(postDriveLowpass);
        
        for (int i = 0; i < numSamples; ++i)
        {
            float sample = sineBuffer[static_cast<size_t>(i)];
            
            // Pre-distortion filtering
            sample = preDistortionFilter.processSample(sample);
            
            // Apply distortion
            sample = applyDistortion(sample, distortionAmount, distortionType);
            
            // Post-drive lowpass
            sample = postDriveLowpassFilter.processSample(0, sample);
            
            // Anti-aliasing filters (light filtering at native rate)
            sample = antiAliasingFilter1.processSample(sample);
            sample = antiAliasingFilter2.processSample(sample);
            
            sineBuffer[static_cast<size_t>(i)] = sample;
        }
    }
    else
    {
        // Use oversampling for better quality when distortion is used
        try {
            
            // Create AudioBlock from the first numSamples of sineBuffer
            float* channelData = sineBuffer.data();
            juce::dsp::AudioBlock<float> inputBlock(&channelData, 1, 0, static_cast<size_t>(numSamples));
            
            // Additional defensive check before oversampling
            if (!oversampler || !oversamplerReady)
            {
                std::fill_n(outputBuffer, numSamples, 0.0f);
                return;
            }
            
            // Check if input has any non-zero samples
            bool hasSignal = false;
            for (size_t i = 0; i < static_cast<size_t>(numSamples); ++i)
            {
                if (std::abs(channelData[i]) > 0.0001f)
                {
                    hasSignal = true;
                    break;
                }
            }
            
            if (!hasSignal)
            {
                // Just process without oversampling
                // Don't use oversampling on silence
                return;
            }
            
            // Process oversampling - this returns a reference to internal oversampled buffer
            auto oversampledBlock = oversampler->processSamplesUp(inputBlock);
            
            if (oversampledBlock.getNumSamples() == 0)
            {
                return;
            }
                
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
                
                // Clamp before storing to avoid overshooting
                distortedBuffer[i] = juce::jlimit(-1.0f, 1.0f, distorted);
            }
            
            // Copy distorted signal back to oversampled block
            if (oversampledLength > 0 && oversampledLength <= distortedBuffer.size() && oversampledData)
            {
                // Use safer memory copy with size validation
                size_t copySize = std::min(oversampledLength, distortedBuffer.size());
                for (size_t i = 0; i < copySize; ++i)
                {
                    oversampledData[i] = distortedBuffer[i];
                }
                
                // Zero-pad the tail of the input block to avoid aliasing
                auto* ptr = inputBlock.getChannelPointer(0);
                if (inputBlock.getNumSamples() > static_cast<size_t>(numSamples))
                {
                    std::fill(ptr + numSamples, ptr + inputBlock.getNumSamples(), 0.0f);
                }
                
                // Downsample back to original rate using only the valid portion
                auto downBlock = inputBlock.getSubBlock(0, static_cast<size_t>(numSamples));
                oversampler->processSamplesDown(downBlock);
                
                // Copy downsampled result back to sineBuffer
                auto* downsampledData = inputBlock.getChannelPointer(0);
                for (int i = 0; i < numSamples; ++i)
                {
                    sineBuffer[static_cast<size_t>(i)] = downsampledData[i];
                }
            }
        }
        catch (const std::exception& e) {
            // If oversampling fails, fill output with silence
            oversamplerReady = false; // Mark oversampler as not ready to prevent future crashes
            std::fill_n(outputBuffer, numSamples, 0.0f);
            return;
        }
    }
    
    // Mix original sine and distorted signals with bounds checking
    for (int i = 0; i < numSamples; ++i)
    {
        size_t idx = static_cast<size_t>(i);
        
        // Bounds check
        if (idx >= cleanSineBuffer.size() || idx >= sineBuffer.size())
        {
            outputBuffer[i] = 0.0f;
            continue;
        }
        
        // Get the original clean sine wave
        float sine = cleanSineBuffer[idx];
        
        // Get the distorted signal
        float distorted = sineBuffer[idx]; // Now contains the processed signal
        
        // Clamp values to prevent extreme outputs
        sine = std::max(-1.0f, std::min(1.0f, sine));
        distorted = std::max(-1.0f, std::min(1.0f, distorted));
        
        // Apply the consistent block envelope
        float envelopedSine = sine * blockEnvelope;
        float envelopedDistorted = distorted * blockEnvelope;
        
        // When distortion is minimal, output the clean sine wave
        // When distortion is high, extract the harmonic content
        float output;
        
        if (distortionAmount < 0.01f)
        {
            // No distortion - output clean subharmonic sine wave
            output = envelopedSine * inverseMixAmount;
            
            // Keep filters in sync
            highpassFilter.processSample(0, 0.0f);
            postSubtractionFilter.processSample(0, 0.0f);
            
            if (idx < harmonicResidualBuffer.size())
            {
                harmonicResidualBuffer[idx] = 0.0f;
            }
        }
        else
        {
            // Harmonic extraction for distorted signals
            // Subtract clean from distorted to isolate harmonics
            float harmonics = envelopedDistorted - envelopedSine;
            
            // Apply high-pass filter to remove DC
            float highPassed = highpassFilter.processSample(0, harmonics);
            
            // Apply post-subtraction lowpass to smooth any harsh artifacts
            float smoothedHarmonics = postSubtractionFilter.processSample(0, highPassed);
            
            // Store harmonic residual for visualization
            if (idx < harmonicResidualBuffer.size())
            {
                harmonicResidualBuffer[idx] = smoothedHarmonics;
            }
            
            // Mix between clean sine and extracted harmonics based on distortion amount
            float harmonicMix = std::min(1.0f, distortionAmount * 2.0f); // Scale up for more audible effect
            output = (envelopedSine * (1.0f - harmonicMix) + smoothedHarmonics * harmonicMix) * inverseMixAmount;
        }
        
        // Final output clamping
        outputBuffer[i] = std::max(-1.0f, std::min(1.0f, output));
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


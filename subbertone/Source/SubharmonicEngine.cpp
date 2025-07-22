#include "SubharmonicEngine.h"

SubharmonicEngine::SubharmonicEngine()
{
    // Back to 8x oversampling as requested
    oversampler = std::make_unique<juce::dsp::Oversampling<float>>(
        1, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true);
    
    // DON'T open debug file in constructor - wait for prepare() to ensure clean start
}

SubharmonicEngine::~SubharmonicEngine()
{
    closeDebugFile();
}

void SubharmonicEngine::prepare(double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    
    // Debug output to console
    DBG("SubharmonicEngine::prepare called - Sample rate: " << sampleRate << ", Block size: " << maxBlockSize);
    
    // Open debug file on prepare to ensure clean start
    openDebugFile();
    
    if (debugFileStream && debugFileStream->openedOk())
    {
        debugFileStream->writeText("\n=== PREPARE CALLED ===\n", false, false, nullptr);
        debugFileStream->writeText("Sample rate: " + juce::String(sampleRate) + " Hz\n", false, false, nullptr);
        debugFileStream->writeText("Max block size: " + juce::String(maxBlockSize) + "\n\n", false, false, nullptr);
        debugFileStream->flush();
    }
    
    // Reset phase and smoothing
    phase = 0.0;
    lastPhase = 0.0;
    phaseReset = false;
    phaseIncrement = 0.0;
    targetPhaseIncrement = 0.0;
    lastStableIncrement = 0.0;
    currentAmplitude = 0.0f;
    targetAmplitude = 0.0f;
    lastFundamental = 0.0f;
    smoothedFrequency = 0.0f;
    lockedFundamental = 0.0f;
    lockCounter = 0;
    amplitudeRampSamplesRemaining = 0;
    outputSmoothPrev = 0.0f;
    
    // Reset DC blocker
    dcBlockerX1 = 0.0f;
    dcBlockerY1 = 0.0f;
    
    // Resize buffers for oversampling (8x)
    sineBuffer.resize(maxBlockSize * 8);
    distortedBuffer.resize(maxBlockSize * 8);
    
    // Initialize delay compensation buffer
    delayBuffer.resize(delayBufferSize);
    std::fill(delayBuffer.begin(), delayBuffer.end(), 0.0f);
    delayWritePos = 0;
    
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
    oversampledSpec.sampleRate = sampleRate * 8.0; // 8x oversampling
    oversampledSpec.maximumBlockSize = maxBlockSize * 8;
    
    // Tone filter runs at native rate, not oversampled
    lowpassFilter.prepare(spec);
    lowpassFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    
    // Much more aggressive cutoff - 10% of oversampled rate
    float cutoffFreq = static_cast<float>(oversampledSpec.sampleRate * 0.1);
    
    // Create 4-stage lowpass with lower resonance for steeper rolloff
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
    
    // Pre-distortion filter to limit input bandwidth
    auto preDistCoeffs = juce::dsp::IIR::Coefficients<float>::makeLowPass(
        oversampledSpec.sampleRate, oversampledSpec.sampleRate * 0.4f, 0.7071f);
    preDistortionFilter.prepare(oversampledSpec);
    preDistortionFilter.coefficients = preDistCoeffs;
    
    postDriveLowpassFilter.prepare(spec);
    postDriveLowpassFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    
    highpassFilter.prepare(spec);
    highpassFilter.setType(juce::dsp::StateVariableTPTFilterType::highpass);
    highpassFilter.setCutoffFrequency(20.0f); // High-pass at 20Hz to remove DC
}

void SubharmonicEngine::process(const float* inputBuffer, float* outputBuffer, int numSamples, float fundamental,
                               float distortionAmount, float inverseMixAmount,
                               int distortionType, float toneFreq, float postDriveLowpass)
{
    juce::ScopedNoDenormals noDenormals;
    
    // Debug buffer processing
    static int processCallCount = 0;
    processCallCount++;
    
    // Console debug output
    if (processCallCount <= 5)
    {
        DBG("SubharmonicEngine::process called - Count: " << processCallCount << ", Samples: " << numSamples << ", Fundamental: " << fundamental);
        DBG("  Current amplitude: " << currentAmplitude << ", Target amplitude: " << targetAmplitude);
        DBG("  Phase: " << phase << ", Phase increment: " << phaseIncrement);
    }
    
    // Force create debug file if not open
    if (!isDebugFileOpen() && processCallCount == 1)
    {
        DBG("Debug file not open on first process call, attempting to open...");
        openDebugFile();
    }
    
    if (isDebugFileOpen() && processCallCount <= 10)
    {
        writeDebugLine("\n=== PROCESS CALL " + juce::String(processCallCount) + " ===");
        writeDebugLine("NumSamples: " + juce::String(numSamples));
        writeDebugLine("Fundamental: " + juce::String(fundamental) + " Hz");
        writeDebugLine("Sample Rate: " + juce::String(sampleRate) + " Hz");
        writeDebugLine("Phase before processing: " + juce::String(phase));
        writeDebugLine("Phase increment: " + juce::String(phaseIncrement));
        debugFileStream->flush();
    }
    
    // DISABLED: Early exit if no fundamental or very quiet
    // Allowing all frequencies for testing
    /*
    if (fundamental < 20.0f || fundamental > 2000.0f)
    {
        // Fade out amplitude smoothly
        targetAmplitude = 0.0f;
        amplitudeRampSamplesRemaining = 0;
        lockCounter = 0; // Reset lock when no fundamental
        lockedFundamental = 0.0f;
        smoothedFrequency = 0.0f; // Reset smoothed frequency
        
        // Still process to apply fade out AND keep phase running
        for (int i = 0; i < numSamples; ++i)
        {
            currentAmplitude = currentAmplitude * amplitudeRelease + 
                             targetAmplitude * (1.0f - amplitudeRelease);
            outputBuffer[i] = 0.0f;
            
            // Debug logging
            logDebugData(fundamental, phase, phaseIncrement, outputBuffer[i]);
            
            // IMPORTANT: Keep phase running even during silence to maintain continuity
            phase += phaseIncrement;
            phase = std::fmod(phase, juce::MathConstants<double>::twoPi);
            if (phase < 0.0)
                phase += juce::MathConstants<double>::twoPi;
        }
        return;
    }
    */
    
    // Clamp micro-jitter in fundamental detection
    float effectiveFundamental = fundamental;
    if (lastFundamental > 0.0f && std::abs(fundamental - lastFundamental) < frequencyJitterThreshold)
    {
        // Ignore small jitter - keep using last fundamental
        effectiveFundamental = lastFundamental;
    }
    else
    {
        // Significant change - update last fundamental
        lastFundamental = fundamental;
    }
    
    // Initialize smoothed frequency on first use
    if (smoothedFrequency == 0.0f && effectiveFundamental > 0.0f)  // Changed from 20.0f to 0.0f
    {
        smoothedFrequency = effectiveFundamental;
        if (isDebugFileOpen())
        {
            writeDebugLine("!!! INITIAL SMOOTHED FREQUENCY: " + juce::String(smoothedFrequency) + " Hz");
        }
    }
    
    // Apply frequency smoothing with adaptive rate
    if (smoothedFrequency > 0.0f)
    {
        float frequencyRatio = effectiveFundamental / smoothedFrequency;
        float smoothingFactor;
        
        // Adaptive smoothing based on frequency change magnitude
        if (frequencyRatio > 2.0f || frequencyRatio < 0.5f)
        {
            // Octave jump - faster smoothing
            smoothingFactor = 0.9f;
            if (isDebugFileOpen())
            {
                writeDebugLine("!!! OCTAVE JUMP DETECTED - Fast smoothing");
            }
        }
        else if (frequencyRatio > 1.2f || frequencyRatio < 0.83f)
        {
            // Significant change - medium smoothing
            smoothingFactor = 0.95f;
        }
        else
        {
            // Small change - slow smoothing
            smoothingFactor = 0.99f;
        }
        
        // Apply smoothing
        smoothedFrequency = smoothedFrequency * smoothingFactor + 
                           effectiveFundamental * (1.0f - smoothingFactor);
    }
    else
    {
        smoothedFrequency = effectiveFundamental;
    }
    
    
    // Generate subharmonic (one octave below) using SMOOTHED frequency
    float subharmonicFreq = smoothedFrequency * 0.5f;
    
    // Calculate target phase increment from smoothed frequency
    targetPhaseIncrement = (2.0 * juce::MathConstants<double>::pi * subharmonicFreq) / sampleRate;
    
    // Debug target phase increment right after calculation
    if (isDebugFileOpen() && processCallCount <= 20 && smoothedFrequency > 0)
    {
        writeDebugLine("!!! Target phase increment calculated: " + juce::String(targetPhaseIncrement) + 
                      " for freq " + juce::String(subharmonicFreq) + " Hz");
    }
    
    // Debug phase increment calculation
    if (isDebugFileOpen() && subharmonicFreq > 0.0f)
    {
        static int calcDebugCount = 0;
        if (calcDebugCount < 10)
        {
            writeDebugLine("\n=== PHASE INCREMENT CALCULATION ===");
            writeDebugLine("Smoothed frequency: " + juce::String(smoothedFrequency) + " Hz");
            writeDebugLine("Subharmonic freq (smoothed * 0.5): " + juce::String(subharmonicFreq) + " Hz");
            writeDebugLine("Sample rate: " + juce::String(sampleRate) + " Hz");
            writeDebugLine("Target phase inc = (2π * " + juce::String(subharmonicFreq) + ") / " + juce::String(sampleRate));
            writeDebugLine("Target phase inc = " + juce::String(targetPhaseIncrement));
            writeDebugLine("Expected samples per cycle: " + juce::String(sampleRate / subharmonicFreq));
            writeDebugLine("Expected phase inc per sample: " + juce::String(targetPhaseIncrement) + " radians\n");
            calcDebugCount++;
        }
    }
    
    // Check if we're starting from silence (new note attack)
    // Consider both amplitude and ramp state to avoid false positives
    bool isStartingFromSilence = currentAmplitude < 0.001f && targetAmplitude < 0.01f && amplitudeRampSamplesRemaining == 0;
    
    // SIMPLIFIED: Always generate at full amplitude when fundamental is valid
    if (fundamental > 0.0f)  // Changed from 20.0f to 0.0f for testing
    {
        // Set amplitudes first, before checking for phase reset
        float previousTargetAmplitude = targetAmplitude;
        targetAmplitude = 1.0f;
        currentAmplitude = 1.0f; // Instant full amplitude for testing
        
        // Only reset phase on first detection (when going from silent to active)
        if (previousTargetAmplitude < 0.5f)
        {
            phase = 0.0;
            if (isDebugFileOpen())
            {
                writeDebugLine("!!! PHASE RESET TO 0 (first detection)");
            }
        }
            
        
        // Reopen debug file to clear it for new generation - only when truly starting from silence
        if (isStartingFromSilence)
        {
            closeDebugFile();
            openDebugFile();
            
            // Reset debug counter when starting new generation
            debugSampleCount = 0;
            generationSampleCount = 0; // Reset generation sample counter
            if (isDebugFileOpen())
            {
                writeDebugLine("\n=== NEW GENERATION STARTED ===");
                writeDebugLine("Initial fundamental: " + juce::String(fundamental) + " Hz");
                writeDebugLine("Effective (clamped) fundamental: " + juce::String(effectiveFundamental) + " Hz");
                writeDebugLine("Smoothed frequency: " + juce::String(smoothedFrequency) + " Hz");
                writeDebugLine("Subharmonic frequency: " + juce::String(smoothedFrequency * 0.5f) + " Hz");
                writeDebugLine("Sample rate: " + juce::String(sampleRate) + " Hz");
                writeDebugLine("Target phase increment: " + juce::String(targetPhaseIncrement));
                writeDebugLine("Current phase: " + juce::String(phase));
                writeDebugLine("Actual phase increment: " + juce::String(phaseIncrement));
                writeDebugLine("Expected phase inc: " + juce::String((2.0 * juce::MathConstants<double>::pi * smoothedFrequency * 0.5) / sampleRate));
                debugFileStream->flush();
            }
        }
    }
    /* DISABLED for testing
    else if (fundamental < 20.0f || fundamental > 2000.0f)
    {
        targetAmplitude = 0.0f;
    }
    */
    else
    {
        targetAmplitude = 1.0f;
        lastStableIncrement = phaseIncrement;
    }
    
    // Debug logging at start of generation loop
    static int bufferCount = 0;
    bufferCount++;
    if (isDebugFileOpen() && bufferCount <= 20)
    {
        writeDebugLine("\n=== BUFFER " + juce::String(bufferCount) + " START ===");
        writeDebugLine("Fundamental: " + juce::String(fundamental) + " Hz");
        writeDebugLine("Effective fundamental: " + juce::String(effectiveFundamental) + " Hz");
        writeDebugLine("Smoothed frequency: " + juce::String(smoothedFrequency) + " Hz");
        writeDebugLine("Subharmonic freq (smoothed * 0.5): " + juce::String(smoothedFrequency * 0.5f) + " Hz");
        writeDebugLine("Target phase increment: " + juce::String(targetPhaseIncrement));
        writeDebugLine("Current phase increment: " + juce::String(phaseIncrement));
        writeDebugLine("Phase before loop: " + juce::String(phase));
        writeDebugLine("Current amplitude: " + juce::String(currentAmplitude));
        writeDebugLine("Target amplitude: " + juce::String(targetAmplitude));
        writeDebugLine("Ramp samples remaining: " + juce::String(amplitudeRampSamplesRemaining));
        debugFileStream->flush();
    }
    
    // SIMPLIFIED SINE GENERATION - just generate a basic sine wave
    // Ignore all smoothing and ramping for now
    phaseIncrement = targetPhaseIncrement;
    
    // Write debug samples
    if (isDebugFileOpen() && processCallCount <= 20)
    {
        writeDebugLine("\n=== SINE GENERATION START ===");
        writeDebugLine("Target phase increment: " + juce::String(targetPhaseIncrement));
        writeDebugLine("Phase increment (after assignment): " + juce::String(phaseIncrement));
        writeDebugLine("Starting phase: " + juce::String(phase));
        writeDebugLine("Fundamental: " + juce::String(fundamental) + " Hz");
        writeDebugLine("Smoothed frequency: " + juce::String(smoothedFrequency) + " Hz");
        debugFileStream->flush();
    }
    
    for (int i = 0; i < numSamples; ++i)
    {
        // Generate pure sine wave at full amplitude
        float sineValue = static_cast<float>(std::sin(phase));
        sineBuffer[i] = sineValue; // Full amplitude, no envelope
        
        // Debug first few samples
        if (i < 10 && isDebugFileOpen() && bufferCount <= 5)
        {
            writeDebugLine("Sample[" + juce::String(i) + "]: phase=" + juce::String(phase) +
                          ", sin(phase)=" + juce::String(sineValue) +
                          ", output=" + juce::String(sineBuffer[i]) +
                          ", phaseInc=" + juce::String(phaseIncrement));
        }
        
        // Simple debug logging
        if (isDebugFileOpen() && i % 100 == 0 && bufferCount <= 10)
        {
            writeDebugLine("Sample " + juce::String(i) + ": sine=" + juce::String(sineValue) +
                          ", phase=" + juce::String(phase) + ", freq=" + 
                          juce::String(phaseIncrement * sampleRate / (2.0 * juce::MathConstants<double>::pi)) + " Hz");
        }
        
        // Update phase
        phase += phaseIncrement;
        
        // Wrap phase
        while (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;
        while (phase < 0.0)
            phase += juce::MathConstants<double>::twoPi;
    }
    
    // ALWAYS output pure sine wave for debugging - ignore distortion
    for (int i = 0; i < numSamples; ++i)
    {
        outputBuffer[i] = sineBuffer[i];
    }
    
    // Write actual sine wave samples to debug file
    if (isDebugFileOpen() && processCallCount <= 3)
    {
        writeDebugLine("\n=== SINE WAVE OUTPUT ===");
        writeDebugLine("First 20 samples:");
        for (int i = 0; i < std::min(20, numSamples); ++i)
        {
            writeDebugLine("Sample[" + juce::String(i) + "] = " + juce::String(outputBuffer[i]));
        }
        debugFileStream->flush();
    }
    
    // Debug final output
    if (isDebugFileOpen() && bufferCount <= 10)
    {
        juce::String outputSamples = "First 5 output samples: ";
        for (int i = 0; i < std::min(5, numSamples); ++i)
        {
            outputSamples += juce::String(outputBuffer[i]) + " ";
        }
        writeDebugLine(outputSamples);
        
        // Calculate RMS and peak values
        float rms = 0.0f;
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            float absVal = std::abs(outputBuffer[i]);
            peak = std::max(peak, absVal);
            rms += outputBuffer[i] * outputBuffer[i];
        }
        rms = std::sqrt(rms / numSamples);
        
        writeDebugLine("Output stats - Peak: " + juce::String(peak) + ", RMS: " + juce::String(rms));
        writeDebugLine("Phase after buffer: " + juce::String(phase));
        writeDebugLine("Samples processed: " + juce::String(numSamples));
        debugFileStream->flush();
    }
    
    // DISABLED post-drive lowpass filter for debugging
    // postDriveLowpassFilter.setCutoffFrequency(postDriveLowpass);
    // float* outputChannelData[] = { outputBuffer };
    // juce::dsp::AudioBlock<float> outputBlock(outputChannelData, 1, 0, numSamples);
    // juce::dsp::ProcessContextReplacing<float> outputContext(outputBlock);
    // postDriveLowpassFilter.process(outputContext);
    
    // Debug the output stage - ALWAYS log the first buffer to see what's happening
    if (isDebugFileOpen() && processCallCount <= 5)
    {
        writeDebugLine("\n=== BUFFER " + juce::String(processCallCount) + " OUTPUT ===");
        writeDebugLine("Fundamental: " + juce::String(fundamental) + " Hz");
        writeDebugLine("Target freq: " + juce::String(fundamental * 0.5f) + " Hz");
        writeDebugLine("Phase increment: " + juce::String(phaseIncrement));
        writeDebugLine("Current phase: " + juce::String(phase));
        
        juce::String sineSamples = "First 10 sine samples: ";
        for (int i = 0; i < std::min(10, numSamples); ++i)
        {
            sineSamples += juce::String(sineBuffer[i]) + " ";
        }
        writeDebugLine(sineSamples);
        
        juce::String outputSamples = "First 10 output samples: ";
        for (int i = 0; i < std::min(10, numSamples); ++i)
        {
            outputSamples += juce::String(outputBuffer[i]) + " ";
        }
        writeDebugLine(outputSamples + "\n");
        debugFileStream->flush();
    }
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

void SubharmonicEngine::openDebugFile()
{
    // Create debug file in user's home directory for easy access
    auto homeDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    debugFile = homeDir.getChildFile("subbertone_debug.txt");
    
    DBG("Attempting to create debug file at: " << debugFile.getFullPathName());
    
    // Delete existing file first to ensure clean start
    if (debugFile.existsAsFile())
    {
        debugFile.deleteFile();
        DBG("Deleted existing debug file");
    }
    
    // Create new file output stream
    debugFileStream = std::make_unique<juce::FileOutputStream>(debugFile);
    
    if (debugFileStream->openedOk())
    {
        DBG("Debug file opened successfully!");
        debugFileStream->setPosition(0);
        debugFileStream->truncate();
        
        // Write header
        debugFileStream->writeText("=== SUBBERTONE DEBUG LOG - NEW SESSION ===\n", false, false, nullptr);
        debugFileStream->writeText("Timestamp: " + juce::Time::getCurrentTime().toString(true, true) + "\n\n", false, false, nullptr);
        debugFileStream->writeText("Sample#,Fundamental,Phase,PhaseIncrement,Output,PhaseDelta\n", false, false, nullptr);
        debugFileStream->flush();
    }
    else
    {
        DBG("Failed to open debug file for writing! Error: " << debugFileStream->getStatus().getErrorMessage());
        debugFileStream.reset();
    }
    
    debugSampleCount = 0;
    generationSampleCount = 0; // Reset generation counter too
}

void SubharmonicEngine::closeDebugFile()
{
    if (debugFileStream)
    {
        debugFileStream->flush();
        debugFileStream.reset();
    }
}

void SubharmonicEngine::writeDebugLine(const juce::String& text)
{
    if (debugFileStream && debugFileStream->openedOk())
    {
        debugFileStream->writeText(text + "\n", false, false, nullptr);
    }
}

bool SubharmonicEngine::isDebugFileOpen() const
{
    return debugFileStream && debugFileStream->openedOk();
}

void SubharmonicEngine::logDebugData(float fundamental, float currentPhase, float phaseInc, float output)
{
    if (isDebugFileOpen() && debugSampleCount < debugMaxSamples)
    {
        static float lastPhase = 0.0f;
        static float lastFundamental = 0.0f;
        static float lastPhaseIncrement = 0.0f;
        static float lastOutput = 0.0f;
        static int bufferCount = 0;
        
        // Track buffer changes
        if (debugSampleCount == 0 || (debugSampleCount % 512 == 0))
        {
            bufferCount++;
            writeDebugLine("\n--- Buffer " + juce::String(bufferCount) + " ---");
            writeDebugLine("Sample Rate: " + juce::String(sampleRate) + " Hz");
            writeDebugLine("Buffer size: 512 samples");
            writeDebugLine("Phase at buffer start: " + juce::String(currentPhase));
            writeDebugLine("Phase increment: " + juce::String(phaseInc));
            writeDebugLine("Current amplitude: " + juce::String(currentAmplitude));
            
            // Check for buffer boundary issues
            if (bufferCount > 1 && std::abs(currentPhase - lastPhase) > juce::MathConstants<float>::pi)
            {
                writeDebugLine("!!! BUFFER BOUNDARY PHASE JUMP: " + juce::String(currentPhase - lastPhase));
            }
        }
        
        // Log fundamental changes
        if (std::abs(fundamental - lastFundamental) > 0.1f)
        {
            writeDebugLine("!!! FUNDAMENTAL CHANGED: " + juce::String(lastFundamental) + " -> " + juce::String(fundamental) + " Hz");
            writeDebugLine("!!! Phase at change: " + juce::String(currentPhase) + ", Last phase: " + juce::String(lastPhase));
            writeDebugLine("!!! Phase increment changed: " + juce::String(lastPhaseIncrement) + " -> " + juce::String(phaseInc));
        }
        
        float phaseDelta = currentPhase - lastPhase;
        
        // Handle phase wrap
        if (phaseDelta < -juce::MathConstants<float>::pi)
            phaseDelta += 2.0f * juce::MathConstants<float>::pi;
        else if (phaseDelta > juce::MathConstants<float>::pi)
            phaseDelta -= 2.0f * juce::MathConstants<float>::pi;
        
        // Detect phase discontinuities
        if (std::abs(phaseDelta) > 0.1f && debugSampleCount > 0)
        {
            writeDebugLine("!!! PHASE DISCONTINUITY: delta = " + juce::String(phaseDelta));
        }
        
        // Detect output discontinuities (potential source of crackling)
        float outputDelta = output - lastOutput;
        if (std::abs(outputDelta) > 0.5f && debugSampleCount > 0)
        {
            writeDebugLine("!!! OUTPUT DISCONTINUITY: delta = " + juce::String(outputDelta) + 
                          ", output = " + juce::String(output) + ", lastOutput = " + juce::String(lastOutput));
        }
        
        // Log with more precision when there's activity
        if (fundamental > 0.0f || output != 0.0f)
        {
            writeDebugLine(juce::String(debugSampleCount) + "," +
                          juce::String(fundamental) + "," +
                          juce::String(currentPhase) + "," +
                          juce::String(phaseInc) + "," +
                          juce::String(output) + "," +
                          juce::String(phaseDelta));
            
            // Flush more frequently when there's activity
            if (debugSampleCount % 100 == 0 && debugFileStream)
                debugFileStream->flush();
        }
        
        lastPhase = currentPhase;
        lastFundamental = fundamental;
        lastPhaseIncrement = phaseInc;
        lastOutput = output;
        debugSampleCount++;
        
        if (debugSampleCount >= debugMaxSamples)
        {
            writeDebugLine("Debug logging stopped at sample limit.");
            if (debugFileStream)
                debugFileStream->flush();
        }
    }
}
#include "PitchFlattenerEngine.h"
#include <algorithm>

PitchFlattenerEngine::PitchFlattenerEngine()
{
}

PitchFlattenerEngine::~PitchFlattenerEngine()
{
}

void PitchFlattenerEngine::prepare(double newSampleRate, int newMaxBlockSize)
{
    sampleRate = newSampleRate;
    maxBlockSize = newMaxBlockSize;
    
    // Create RubberBand stretchers with settings optimized for real-time pitch shifting
    RubberBand::RubberBandStretcher::Options options = 
        RubberBand::RubberBandStretcher::OptionProcessRealTime |
        RubberBand::RubberBandStretcher::OptionPitchHighConsistency |  // Better for continuous sounds
        RubberBand::RubberBandStretcher::OptionFormantPreserved |
        RubberBand::RubberBandStretcher::OptionChannelsTogether;
    
    rubberBandLeft = std::make_unique<RubberBand::RubberBandStretcher>(
        static_cast<size_t>(sampleRate), 1, options);
    
    rubberBandRight = std::make_unique<RubberBand::RubberBandStretcher>(
        static_cast<size_t>(sampleRate), 1, options);
    
    // Set processing parameters for real-time
    rubberBandLeft->setMaxProcessSize(maxBlockSize);
    rubberBandRight->setMaxProcessSize(maxBlockSize);
    
    // Get latency for compensation
    latencyInSamples = static_cast<int>(rubberBandLeft->getLatency());
    
    // Allocate buffers with extra space for pre-buffering
    const int bufferSize = maxBlockSize * 4;  // Larger buffer for smoother operation
    inputBufferLeft.resize(bufferSize);
    inputBufferRight.resize(bufferSize);
    outputBufferLeft.resize(bufferSize);
    outputBufferRight.resize(bufferSize);
    
    // Pre-buffer for feeding RubberBand ahead of time
    preBufferLeft.resize(bufferSize);
    preBufferRight.resize(bufferSize);
    preBufferSamples = 0;
    
    // Initialize lookahead buffer
    lookaheadSize = static_cast<int>(maxBlockSize * lookaheadMultiplier);
    lookaheadBuffer.setSize(2, lookaheadSize + maxBlockSize * 2);
    lookaheadBuffer.clear();
    lookaheadWritePos = 0;
    lookaheadReadPos = 0;
    
    // Delay buffer for latency compensation
    delayBuffer.setSize(2, latencyInSamples + maxBlockSize);
    delayBuffer.clear();
    delayBufferWritePos = 0;
    
    // Setup pointer arrays for RubberBand
    inputPointers.resize(1);
    outputPointers.resize(1);
    
    reset();
    warmUpRubberBand();
    isWarmedUp = true;  // Mark as warmed up after initial warm-up
}

void PitchFlattenerEngine::reset()
{
    if (rubberBandLeft)
        rubberBandLeft->reset();
    if (rubberBandRight)
        rubberBandRight->reset();
    
    currentPitchRatio = 1.0f;
    targetPitchRatio = 1.0f;
    framesPushed = 0;
    isWarmedUp = false;
    delayBufferWritePos = 0;
    delayBuffer.clear();
    preBufferSamples = 0;
    lastPitchRatio = 1.0f;
    lookaheadWritePos = 0;
    lookaheadReadPos = 0;
    lookaheadBuffer.clear();
    
    // Clear all buffers
    std::fill(inputBufferLeft.begin(), inputBufferLeft.end(), 0.0f);
    std::fill(inputBufferRight.begin(), inputBufferRight.end(), 0.0f);
    std::fill(outputBufferLeft.begin(), outputBufferLeft.end(), 0.0f);
    std::fill(outputBufferRight.begin(), outputBufferRight.end(), 0.0f);
    std::fill(preBufferLeft.begin(), preBufferLeft.end(), 0.0f);
    std::fill(preBufferRight.begin(), preBufferRight.end(), 0.0f);
}

void PitchFlattenerEngine::warmUpRubberBand()
{
    // Push silence through RubberBand to prime it
    std::vector<float> silence(maxBlockSize, 0.0f);
    const float* silencePtr = silence.data();
    inputPointers[0] = silencePtr;
    
    // Push enough blocks to fill the internal buffers and create a cushion
    int blocksToWarmUp = (latencyInSamples / maxBlockSize) + 16;  // Even more blocks to prevent underruns
    
    for (int i = 0; i < blocksToWarmUp; ++i)
    {
        rubberBandLeft->process(&silencePtr, maxBlockSize, false);
        rubberBandRight->process(&silencePtr, maxBlockSize, false);
    }
    
    // Clear any output that might be available
    if (rubberBandLeft && outputBufferLeft.size() >= maxBlockSize)
    {
        while (rubberBandLeft->available() > 0)
        {
            int toRetrieve = std::min(rubberBandLeft->available(), maxBlockSize);
            outputPointers[0] = outputBufferLeft.data();
            rubberBandLeft->retrieve(outputPointers.data(), static_cast<size_t>(toRetrieve));
        }
    }
    
    if (rubberBandRight && outputBufferRight.size() >= maxBlockSize)
    {
        while (rubberBandRight->available() > 0)
        {
            int toRetrieve = std::min(rubberBandRight->available(), maxBlockSize);
            outputPointers[0] = outputBufferRight.data();
            rubberBandRight->retrieve(outputPointers.data(), static_cast<size_t>(toRetrieve));
        }
    }
}

void PitchFlattenerEngine::setParameters(float detectedPitch, float targetPitch, float smoothing, float lookaheadMultiplier)
{
    // For Doppler flattening, we need much faster response
    // Map smoothing: 0 = instant, 1 = smooth (but capped at 0.3 for very fast response)
    smoothingFactor = smoothing * 0.3f;  // Linear mapping, max 0.3 for fast tracking
    
    // Update lookahead settings
    this->lookaheadMultiplier = lookaheadMultiplier;
    int newLookaheadSize = static_cast<int>(maxBlockSize * lookaheadMultiplier);
    
    // Resize lookahead buffer if needed
    if (newLookaheadSize != lookaheadSize)
    {
        lookaheadSize = newLookaheadSize;
        lookaheadBuffer.setSize(2, lookaheadSize + maxBlockSize * 2);
        lookaheadBuffer.clear();
        lookaheadWritePos = 0;
        lookaheadReadPos = 0;
    }
    
    updatePitchRatio(detectedPitch, targetPitch);
}

void PitchFlattenerEngine::updatePitchRatio(float detectedPitch, float targetPitch)
{
    if (detectedPitch > 0.0f && targetPitch > 0.0f)
    {
        // Calculate the pitch ratio needed to shift from detected to target
        targetPitchRatio = targetPitch / detectedPitch;
        
        // Clamp to reasonable bounds (±2 octaves)
        targetPitchRatio = std::clamp(targetPitchRatio, 0.25f, 4.0f);
        
        DBG("Pitch ratio calculation - Detected: " << detectedPitch << " Target: " << targetPitch << " Ratio: " << targetPitchRatio);
    }
    else
    {
        // Always set a fallback ratio to ensure processing continues
        targetPitchRatio = 1.0f;
        DBG("Using fallback ratio 1.0 - Detected: " << detectedPitch << " Target: " << targetPitch);
    }
}

void PitchFlattenerEngine::process(juce::AudioBuffer<float>& buffer, float mixAmount)
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    
    if (numChannels == 0 || numSamples == 0)
        return;
    
    // Store dry signal for fallback
    juce::AudioBuffer<float> dryBuffer;
    dryBuffer.makeCopyOf(buffer);
    
    
    // If not warmed up yet, pass through dry signal
    if (!isWarmedUp)
    {
        buffer.makeCopyOf(dryBuffer);
        DBG("Still warming up, using dry signal");
        return;
    }
    
    // Store previous ratio for smooth transitions
    lastPitchRatio = currentPitchRatio;
    
    // Update pitch ratio with smoothing
    if (smoothingFactor < 0.01f)
    {
        // Instant lock when smoothing is essentially zero
        currentPitchRatio = targetPitchRatio;
    }
    else
    {
        // Adaptive smoothing - faster response for larger pitch changes
        float ratioDiff = std::abs(targetPitchRatio - currentPitchRatio);
        float adaptiveFactor = smoothingFactor;
        if (ratioDiff > 0.1f) // Large pitch change
        {
            adaptiveFactor *= 0.3f; // Much faster response
        }
        
        float ratioStep = (targetPitchRatio - currentPitchRatio) * (1.0f - adaptiveFactor);
        currentPitchRatio += ratioStep;
    }
    
    // Apply additional smoothing to prevent crackling from rapid changes
    const float maxRatioChange = 0.005f;  // Maximum change per sample
    float ratioChange = currentPitchRatio - lastPitchRatio;
    if (std::abs(ratioChange) > maxRatioChange)
    {
        currentPitchRatio = lastPitchRatio + (ratioChange > 0 ? maxRatioChange : -maxRatioChange);
    }
    
    // Debug output
    static int debugCounter = 0;
    if (++debugCounter % 50 == 0)  // Log every 50th process block
    {
        DBG("PitchFlattener - Current ratio: " << currentPitchRatio << " Target ratio: " << targetPitchRatio);
        DBG("Mix: " << mixAmount << " Smoothing: " << smoothingFactor);
    }
    
    // Only skip processing if mix is essentially zero
    if (mixAmount < 0.001f)
    {
        buffer.makeCopyOf(dryBuffer);
        DBG("Skipping processing - mix is zero");
        return;
    }
    
    // Update RubberBand pitch scale
    if (rubberBandLeft)
        rubberBandLeft->setPitchScale(static_cast<double>(currentPitchRatio));
    if (numChannels > 1 && rubberBandRight)
        rubberBandRight->setPitchScale(static_cast<double>(currentPitchRatio));
    
    // Write input to lookahead buffer
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* input = buffer.getReadPointer(ch);
        float* lookaheadData = lookaheadBuffer.getWritePointer(ch);
        
        for (int i = 0; i < numSamples; ++i)
        {
            lookaheadData[(lookaheadWritePos + i) % lookaheadBuffer.getNumSamples()] = input[i];
        }
    }
    lookaheadWritePos = (lookaheadWritePos + numSamples) % lookaheadBuffer.getNumSamples();
    
    // Calculate how many samples we can feed to RubberBand
    int samplesInLookahead = (lookaheadWritePos - lookaheadReadPos + lookaheadBuffer.getNumSamples()) % lookaheadBuffer.getNumSamples();
    int samplesToFeed = std::min(samplesInLookahead, static_cast<int>(maxBlockSize * lookaheadMultiplier));
    
    // Only process if we have enough lookahead
    if (samplesToFeed >= numSamples)
    {
        // Process left channel with lookahead
        float* tempBufferLeft = inputBufferLeft.data();
        const float* lookaheadLeft = lookaheadBuffer.getReadPointer(0);
        for (int i = 0; i < samplesToFeed; ++i)
        {
            tempBufferLeft[i] = lookaheadLeft[(lookaheadReadPos + i) % lookaheadBuffer.getNumSamples()];
        }
        
        inputPointers[0] = tempBufferLeft;
        outputPointers[0] = outputBufferLeft.data();
        
        // Feed lookahead audio to RubberBand left
        if (rubberBandLeft && inputBufferLeft.size() >= static_cast<size_t>(samplesToFeed))
        {
            rubberBandLeft->process(inputPointers.data(), static_cast<size_t>(samplesToFeed), false);
        }
        
        // Process right channel with lookahead if stereo
        if (numChannels > 1 && rubberBandRight && inputBufferRight.size() >= static_cast<size_t>(samplesToFeed))
        {
            float* tempBufferRight = inputBufferRight.data();
            const float* lookaheadRight = lookaheadBuffer.getReadPointer(1);
            for (int i = 0; i < samplesToFeed; ++i)
            {
                tempBufferRight[i] = lookaheadRight[(lookaheadReadPos + i) % lookaheadBuffer.getNumSamples()];
            }
            
            inputPointers[0] = tempBufferRight;
            rubberBandRight->process(inputPointers.data(), static_cast<size_t>(samplesToFeed), false);
        }
        
        // Update read position after feeding
        lookaheadReadPos = (lookaheadReadPos + numSamples) % lookaheadBuffer.getNumSamples();
    }
    else
    {
        // Not enough lookahead yet, just process normally
        const float* inputLeft = buffer.getReadPointer(0);
        std::copy(inputLeft, inputLeft + numSamples, inputBufferLeft.data());
        
        inputPointers[0] = inputBufferLeft.data();
        outputPointers[0] = outputBufferLeft.data();
        
        if (rubberBandLeft && inputBufferLeft.size() >= static_cast<size_t>(numSamples))
        {
            rubberBandLeft->process(inputPointers.data(), static_cast<size_t>(numSamples), false);
        }
        
        if (numChannels > 1 && rubberBandRight && inputBufferRight.size() >= static_cast<size_t>(numSamples))
        {
            const float* inputRight = buffer.getReadPointer(1);
            std::copy(inputRight, inputRight + numSamples, inputBufferRight.data());
            
            inputPointers[0] = inputBufferRight.data();
            rubberBandRight->process(inputPointers.data(), static_cast<size_t>(numSamples), false);
        }
    }
    
    framesPushed += numSamples;
    
    // Check if we have enough samples to retrieve
    int availableLeft = rubberBandLeft ? rubberBandLeft->available() : 0;
    int availableRight = (numChannels > 1 && rubberBandRight) ? rubberBandRight->available() : availableLeft;
    
    DBG("Available samples - Left: " << availableLeft << " Right: " << availableRight << " Needed: " << numSamples);
    DBG("Frames pushed: " << framesPushed << " Warmed up: " << isWarmedUp);
    
    // Allow smaller chunks but require at least 75% of requested samples to avoid underruns
    int minSamplesRequired = (numSamples * 3) / 4;
    int samplesToProcess = std::min(availableLeft, numSamples);
    if (numChannels > 1)
        samplesToProcess = std::min(samplesToProcess, availableRight);
    
    // If we don't have enough samples, wait for more to avoid crackling
    if (samplesToProcess < minSamplesRequired && framesPushed < latencyInSamples * 4)
    {
        samplesToProcess = 0;  // Wait for more samples
    }
    
    bool canProcessStereo = (samplesToProcess > 0) && (numChannels > 1) && 
                           (outputBufferLeft.size() >= static_cast<size_t>(samplesToProcess)) && 
                           (outputBufferRight.size() >= static_cast<size_t>(samplesToProcess));
    bool canProcessMono = (numChannels == 1) && (samplesToProcess > 0) && 
                         (outputBufferLeft.size() >= static_cast<size_t>(samplesToProcess));
    
    DBG("Samples to process: " << samplesToProcess << " out of " << numSamples);
    
    if (canProcessStereo || canProcessMono)
    {
        DBG("Processing wet signal - canProcessStereo: " << canProcessStereo << " canProcessMono: " << canProcessMono);
        
        // Safely retrieve left channel
        if (rubberBandLeft && samplesToProcess > 0 && outputBufferLeft.size() >= static_cast<size_t>(samplesToProcess))
        {
            outputPointers[0] = outputBufferLeft.data();
            rubberBandLeft->retrieve(outputPointers.data(), static_cast<size_t>(samplesToProcess));
            
            // Mix processed with time-aligned dry signal for available samples
            float* outputLeft = buffer.getWritePointer(0);
            const float* dryLeft = dryBuffer.getReadPointer(0); // Now time-aligned
            for (int i = 0; i < samplesToProcess; ++i)
            {
                outputLeft[i] = dryLeft[i] * (1.0f - mixAmount) + outputBufferLeft[i] * mixAmount;
            }
            
            // If we processed less than numSamples, copy the last valid sample to avoid discontinuity
            if (samplesToProcess < numSamples)
            {
                // Instead of switching to dry, repeat the last processed sample with decay
                float lastSample = outputLeft[samplesToProcess - 1];
                for (int i = samplesToProcess; i < numSamples; ++i)
                {
                    outputLeft[i] = lastSample * 0.999f;  // Slight decay to avoid DC buildup
                    lastSample = outputLeft[i];
                }
            }
            
            DBG("Successfully processed and mixed left channel - " << samplesToProcess << " samples");
        }
        else
        {
            // Fallback to dry signal if left channel fails
            buffer.makeCopyOf(dryBuffer);
            DBG("Left channel failed - using dry signal");
        }
    }
    else
    {
        // Not enough samples available - use dry signal
        buffer.makeCopyOf(dryBuffer);
        DBG("Not enough samples available - using dry signal");
        
        if (!isWarmedUp && framesPushed > latencyInSamples * 2)
        {
            DBG("Warning: RubberBand not producing enough output. Available: " << availableLeft);
        }
    }
    
    // Process right channel if stereo and we successfully processed left
    if (numChannels > 1 && canProcessStereo && rubberBandRight && 
        samplesToProcess > 0 && outputBufferRight.size() >= static_cast<size_t>(samplesToProcess))
    {
        outputPointers[0] = outputBufferRight.data();
        rubberBandRight->retrieve(outputPointers.data(), static_cast<size_t>(samplesToProcess));
        
        // Mix processed with time-aligned dry signal for available samples
        float* outputRight = buffer.getWritePointer(1);
        const float* dryRight = dryBuffer.getReadPointer(1); // Now time-aligned
        for (int i = 0; i < samplesToProcess; ++i)
        {
            outputRight[i] = dryRight[i] * (1.0f - mixAmount) + outputBufferRight[i] * mixAmount;
        }
        
        // If we processed less than numSamples, copy the last valid sample to avoid discontinuity
        if (samplesToProcess < numSamples)
        {
            // Instead of switching to dry, repeat the last processed sample with decay
            float lastSample = outputRight[samplesToProcess - 1];
            for (int i = samplesToProcess; i < numSamples; ++i)
            {
                outputRight[i] = lastSample * 0.999f;  // Slight decay to avoid DC buildup
                lastSample = outputRight[i];
            }
        }
    }
}

void PitchFlattenerEngine::processDryDelay(juce::AudioBuffer<float>& dryBuffer)
{
    const int numChannels = dryBuffer.getNumChannels();
    const int numSamples = dryBuffer.getNumSamples();
    
    if (totalProcessingLatency == 0)
    {
        // No delay needed
        return;
    }
    
    // Process each channel
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* input = dryBuffer.getReadPointer(ch);
        float* output = dryBuffer.getWritePointer(ch);
        
        // Write new samples to delay buffer and read delayed samples
        for (int i = 0; i < numSamples; ++i)
        {
            // Write to delay buffer
            dryDelayBuffer.setSample(ch, dryDelayWritePos, input[i]);
            
            // Read from delay buffer (with latency offset)
            int readPos = dryDelayWritePos - totalProcessingLatency;
            if (readPos < 0)
                readPos += dryDelayBuffer.getNumSamples();
                
            output[i] = dryDelayBuffer.getSample(ch, readPos);
            
            // Advance write position
            dryDelayWritePos = (dryDelayWritePos + 1) % dryDelayBuffer.getNumSamples();
        }
    }
}
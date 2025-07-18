#include "ReverseEngine.h"

ReverseEngine::ReverseEngine()
{
}

ReverseEngine::~ReverseEngine()
{
}

void ReverseEngine::prepare(double newSampleRate, int samplesPerBlock, int newNumChannels)
{
    sampleRate = newSampleRate;
    numChannels = newNumChannels;
    
    channels.clear();
    for (int i = 0; i < numChannels; ++i)
    {
        auto channel = std::make_unique<Channel>();
        channel->windowSamples = static_cast<int>(windowTime * sampleRate);
        channel->hopSize = channel->windowSamples / 2;  // 50% overlap
        
        // Initialize delay line (2x window size for safety)
        channel->delayLine.resize(channel->windowSamples * 2);
        channel->delayWritePos = 0;
        
        // Initialize grains
        for (int g = 0; g < Channel::NUM_GRAINS; ++g)
        {
            channel->grains[g].buffer.resize(channel->windowSamples);
            channel->grains[g].grainSize = channel->windowSamples;
            channel->grains[g].active = false;
            channel->grains[g].readPosition = 0;
        }
        
        // Initialize output buffer (2x window size for overlap accumulation)
        channel->outputBuffer.resize(channel->windowSamples * 2);
        channel->outputReadPos = 0;
        channel->outputWritePos = channel->windowSamples; // Start with latency
        
        // Initialize window function
        channel->windowFunction.resize(channel->windowSamples);
        channel->repeatBuffer.resize(channel->windowSamples);
        
        channel->vibratoPhase.reset(sampleRate, 0.001);
        channel->feedbackGainSmoothed.reset(sampleRate, 0.001); // 1ms smoothing like original
        
        // Create window function
        createWindowFunction(channel->windowFunction, channel->windowSamples);
        
        channels.push_back(std::move(channel));
    }
    
    reset();
}

void ReverseEngine::reset()
{
    for (auto& channel : channels)
    {
        std::fill(channel->delayLine.begin(), channel->delayLine.end(), 0.0f);
        std::fill(channel->outputBuffer.begin(), channel->outputBuffer.end(), 0.0f);
        std::fill(channel->repeatBuffer.begin(), channel->repeatBuffer.end(), 0.0f);
        
        channel->delayWritePos = 0;
        channel->outputReadPos = 0;
        channel->outputWritePos = channel->windowSamples;
        channel->currentGrain = 0;
        channel->grainCounter = 0;
        
        // Reset all grains
        for (int g = 0; g < Channel::NUM_GRAINS; ++g)
        {
            std::fill(channel->grains[g].buffer.begin(), channel->grains[g].buffer.end(), 0.0f);
            channel->grains[g].active = false;
            channel->grains[g].readPosition = 0;
        }
        
        channel->feedbackSample = 0.0f;
        channel->crossfadePosition = 0.0f;
        channel->crossfadeDirection = true;
        channel->repeatPosition = 0;
        channel->isRepeating = false;
        channel->vibratoPhase.setCurrentAndTargetValue(0.0f);
        channel->grainSpawnOffset = 0;
        channel->feedbackGainSmoothed.setCurrentAndTargetValue(0.0f);
    }
}

void ReverseEngine::setParameters(float windowTimeSeconds, float feedbackAmount, float wetMixAmount, float dryMixAmount, int mode, float crossfadePercent, float envelopeSeconds)
{
    // Clamp minimum window time to 30ms to prevent excessive crackling
    windowTime = std::max(0.03f, windowTimeSeconds);
    feedback = feedbackAmount;
    wetMix = wetMixAmount;
    dryMix = dryMixAmount;
    effectMode = mode;
    crossfadeTime = crossfadePercent / 100.0f;
    
    // Limit envelope time to maximum 50% of window time to prevent overlap distortion
    float maxEnvelopeTime = windowTime * 0.5f;
    envelopeTime = std::min(envelopeSeconds, maxEnvelopeTime);
    
    int newWindowSamples = static_cast<int>(windowTime * sampleRate);
    
    for (auto& channel : channels)
    {
        if (channel->windowSamples != newWindowSamples)
        {
            channel->windowSamples = newWindowSamples;
            channel->hopSize = channel->windowSamples / 2;
            
            // Resize buffers
            channel->delayLine.resize(channel->windowSamples * 2);
            channel->outputBuffer.resize(channel->windowSamples * 2);
            channel->windowFunction.resize(channel->windowSamples);
            channel->repeatBuffer.resize(channel->windowSamples);
            
            // Reset positions
            channel->delayWritePos = 0;
            channel->outputReadPos = 0;
            channel->outputWritePos = channel->windowSamples;
            channel->currentGrain = 0;
            channel->grainCounter = 0;
            
            // Resize grain buffers
            for (int g = 0; g < Channel::NUM_GRAINS; ++g)
            {
                channel->grains[g].buffer.resize(channel->windowSamples);
                channel->grains[g].grainSize = channel->windowSamples;
                channel->grains[g].active = false;
                channel->grains[g].readPosition = 0;
            }
            
            // Recreate window function
            createWindowFunction(channel->windowFunction, channel->windowSamples);
        }
    }
}

void ReverseEngine::process(juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    
    // Process each channel
    for (int ch = 0; ch < numChannels && ch < buffer.getNumChannels(); ++ch)
    {
        float* channelData = buffer.getWritePointer(ch);
        
        switch (effectMode)
        {
            case ReversePlayback:
                processReversePlayback(*channels[ch], channelData, numSamples);
                break;
            case ForwardBackwards:
                processForwardBackwards(*channels[ch], channelData, numSamples);
                break;
            case ReverseRepeat:
                processReverseRepeat(*channels[ch], channelData, numSamples);
                break;
        }
    }
}

void ReverseEngine::processReversePlayback(Channel& ch, float* channelData, int numSamples)
{
    // Match original: feedback * 0.5 safety factor
    const float FEEDBACK_SAFETY_FACTOR = 0.5f;
    const float FEEDBACK_HARD_LIMIT = 0.95f;
    
    // Update smoothed feedback gain
    ch.feedbackGainSmoothed.setTargetValue(feedback * FEEDBACK_SAFETY_FACTOR);
    
    for (int i = 0; i < numSamples; ++i)
    {
        float inputSample = channelData[i];
        
        // Get smoothed feedback gain
        float currentFeedbackGain = ch.feedbackGainSmoothed.getNextValue();
        
        // Write to delay line WITHOUT feedback (feedback comes from output)
        ch.delayLine[ch.delayWritePos] = inputSample;
        ch.delayWritePos = (ch.delayWritePos + 1) % ch.delayLine.size();
        
        // Clear output buffer position
        ch.outputBuffer[ch.outputWritePos] = 0.0f;
        
        // Process active grains
        for (int g = 0; g < Channel::NUM_GRAINS; ++g)
        {
            if (ch.grains[g].active)
            {
                int readPos = ch.grains[g].readPosition;
                if (readPos < ch.grains[g].grainSize)
                {
                    // Read from grain buffer in reverse
                    int reverseIndex = ch.grains[g].grainSize - 1 - readPos;
                    float grainSample = ch.grains[g].buffer[reverseIndex];
                    
                    // Apply window
                    float windowGain = ch.windowFunction[readPos];
                    
                    // Accumulate to output
                    ch.outputBuffer[ch.outputWritePos] += grainSample * windowGain * ch.grains[g].amplitude;
                    
                    ch.grains[g].readPosition++;
                }
                else
                {
                    ch.grains[g].active = false;
                }
            }
        }
        
        // Spawn new grain at hop intervals
        ch.grainCounter++;
        if (ch.grainCounter >= ch.hopSize)
        {
            ch.grainCounter = 0;
            
            // Find inactive grain
            for (int g = 0; g < Channel::NUM_GRAINS; ++g)
            {
                if (!ch.grains[g].active)
                {
                    // Copy from delay line to grain buffer
                    int delayReadPos = (ch.delayWritePos - ch.windowSamples + ch.delayLine.size()) % ch.delayLine.size();
                    
                    for (int s = 0; s < ch.windowSamples; ++s)
                    {
                        ch.grains[g].buffer[s] = ch.delayLine[(delayReadPos + s) % ch.delayLine.size()];
                    }
                    
                    ch.grains[g].active = true;
                    ch.grains[g].readPosition = 0;
                    ch.grains[g].amplitude = 1.0f;
                    ch.grains[g].grainSize = ch.windowSamples;
                    
                    break;
                }
            }
        }
        
        // Read from output buffer
        float outputSample = ch.outputBuffer[ch.outputReadPos];
        
        // Add feedback from previous output (matching original)
        float wetSignal = outputSample + (ch.feedbackSample * currentFeedbackGain);
        
        // Apply soft limiting like original
        if (std::fabs(wetSignal) > FEEDBACK_HARD_LIMIT)
        {
            wetSignal = std::tanh(wetSignal * 0.7f) * 1.4286f;
        }
        
        // Mix with dry signal
        float processedSample = inputSample * dryMix + wetSignal * wetMix;
        
        // Store output for next sample's feedback (matching original)
        ch.feedbackSample = processedSample;
        
        channelData[i] = processedSample;
        
        // Update output buffer positions
        ch.outputReadPos = (ch.outputReadPos + 1) % ch.outputBuffer.size();
        ch.outputWritePos = (ch.outputWritePos + 1) % ch.outputBuffer.size();
    }
}

void ReverseEngine::processForwardBackwards(Channel& ch, float* channelData, int numSamples)
{
    // Match original with 0.5 safety factor
    const float FEEDBACK_SAFETY_FACTOR = 0.5f;
    const float FEEDBACK_HARD_LIMIT = 0.95f;
    const int crossfadeSamples = static_cast<int>(ch.windowSamples * crossfadeTime);
    
    // Update smoothed feedback gain
    ch.feedbackGainSmoothed.setTargetValue(feedback * FEEDBACK_SAFETY_FACTOR);
    
    for (int i = 0; i < numSamples; ++i)
    {
        float inputSample = channelData[i];
        float currentFeedbackGain = ch.feedbackGainSmoothed.getNextValue();
        
        // Write to delay line without feedback
        ch.delayLine[ch.delayWritePos] = inputSample;
        
        // Clear output buffer
        ch.outputBuffer[ch.outputWritePos] = 0.0f;
        
        // Process active grains
        for (int g = 0; g < Channel::NUM_GRAINS; ++g)
        {
            if (ch.grains[g].active)
            {
                int readPos = ch.grains[g].readPosition;
                if (readPos < ch.grains[g].grainSize)
                {
                    float grainSample;
                    
                    // First half: forward playback
                    if (readPos < ch.grains[g].grainSize / 2)
                    {
                        grainSample = ch.grains[g].buffer[readPos];
                    }
                    // Second half: backward playback
                    else
                    {
                        int reversePos = ch.grains[g].grainSize - 1 - (readPos - ch.grains[g].grainSize / 2);
                        grainSample = ch.grains[g].buffer[reversePos];
                    }
                    
                    float windowGain = ch.windowFunction[readPos];
                    
                    // Apply crossfade at the transition point
                    float grainFade = 1.0f;
                    int halfGrain = ch.grains[g].grainSize / 2;
                    
                    if (readPos >= halfGrain - crossfadeSamples && readPos < halfGrain)
                    {
                        // Fade out forward
                        float fadePos = (float)(readPos - (halfGrain - crossfadeSamples)) / crossfadeSamples;
                        grainFade = 1.0f - fadePos;
                    }
                    else if (readPos >= halfGrain && readPos < halfGrain + crossfadeSamples)
                    {
                        // Fade in backward
                        float fadePos = (float)(readPos - halfGrain) / crossfadeSamples;
                        grainFade = fadePos;
                    }
                    
                    ch.outputBuffer[ch.outputWritePos] += grainSample * windowGain * grainFade * ch.grains[g].amplitude;
                    
                    ch.grains[g].readPosition++;
                }
                else
                {
                    ch.grains[g].active = false;
                }
            }
        }
        
        // Spawn new grain pair at hop intervals
        ch.grainCounter++;
        if (ch.grainCounter >= ch.hopSize)
        {
            ch.grainCounter = 0;
            
            // Find two inactive grains for forward and backward
            int forwardGrain = -1, reverseGrain = -1;
            
            for (int g = 0; g < Channel::NUM_GRAINS; ++g)
            {
                if (!ch.grains[g].active)
                {
                    if (forwardGrain == -1)
                        forwardGrain = g;
                    else if (reverseGrain == -1)
                    {
                        reverseGrain = g;
                        break;
                    }
                }
            }
            
            if (forwardGrain != -1)
            {
                // Vary the read position to prevent feedback loops
                int baseReadPos = (ch.delayWritePos - ch.windowSamples + ch.delayLine.size()) % ch.delayLine.size();
                int delayReadPos = (baseReadPos - ch.grainSpawnOffset + ch.delayLine.size()) % ch.delayLine.size();
                
                // Copy from delay line
                for (int s = 0; s < ch.windowSamples; ++s)
                {
                    float sample = ch.delayLine[(delayReadPos + s) % ch.delayLine.size()];
                    ch.grains[forwardGrain].buffer[s] = sample;
                    if (reverseGrain != -1)
                        ch.grains[reverseGrain].buffer[s] = sample;
                }
                
                ch.grains[forwardGrain].active = true;
                ch.grains[forwardGrain].readPosition = 0;
                ch.grains[forwardGrain].amplitude = 0.7f;  // Reduce amplitude to prevent buildup
                ch.grains[forwardGrain].grainSize = ch.windowSamples;
                
                // Update spawn offset for next grain pair (cycle through 25% of window)
                ch.grainSpawnOffset = (ch.grainSpawnOffset + ch.windowSamples / 4) % (ch.windowSamples / 2);
            }
        }
        
        // Read from output buffer
        float outputSample = ch.outputBuffer[ch.outputReadPos];
        
        // Add feedback from previous output
        float wetSignal = outputSample + (ch.feedbackSample * currentFeedbackGain);
        
        // Apply soft limiting like original
        if (std::fabs(wetSignal) > FEEDBACK_HARD_LIMIT)
        {
            wetSignal = std::tanh(wetSignal * 0.7f) * 1.4286f;
        }
        
        // Mix with dry signal
        float processedSample = inputSample * dryMix + wetSignal * wetMix;
        
        // Store output for next sample's feedback
        ch.feedbackSample = processedSample;
        
        channelData[i] = processedSample;
        
        // Update positions
        ch.delayWritePos = (ch.delayWritePos + 1) % ch.delayLine.size();
        ch.outputReadPos = (ch.outputReadPos + 1) % ch.outputBuffer.size();
        ch.outputWritePos = (ch.outputWritePos + 1) % ch.outputBuffer.size();
    }
}

void ReverseEngine::processReverseRepeat(Channel& ch, float* channelData, int numSamples)
{
    // Match original with 0.5 safety factor
    const float FEEDBACK_SAFETY_FACTOR = 0.5f;
    const float FEEDBACK_HARD_LIMIT = 0.95f;
    
    // Update smoothed feedback gain
    ch.feedbackGainSmoothed.setTargetValue(feedback * FEEDBACK_SAFETY_FACTOR);
    
    for (int i = 0; i < numSamples; ++i)
    {
        float inputSample = channelData[i];
        float currentFeedbackGain = ch.feedbackGainSmoothed.getNextValue();
        
        // Write to delay line without feedback
        ch.delayLine[ch.delayWritePos] = inputSample;
        
        // Clear output buffer
        ch.outputBuffer[ch.outputWritePos] = 0.0f;
        
        // Process active grains
        for (int g = 0; g < Channel::NUM_GRAINS; ++g)
        {
            if (ch.grains[g].active)
            {
                int readPos = ch.grains[g].readPosition;
                if (readPos < ch.grains[g].grainSize)
                {
                    float grainSample;
                    
                    // Always read in reverse
                    int reverseIndex = ch.grains[g].grainSize - 1 - readPos;
                    
                    // Apply vibrato only on second repeat
                    if (ch.isRepeating && readPos >= ch.grains[g].grainSize / 2)
                    {
                        // Apply subtle vibrato modulation
                        float vibratoMod = getVibratoModulation(ch);
                        float vibratoDepth = 0.005f; // 0.5% pitch variation - much more subtle
                        
                        // Apply vibrato to forward position first, then reverse
                        float modulatedPos = (float)readPos + vibratoMod * vibratoDepth * ch.grains[g].grainSize;
                        int modulatedIndex = (int)modulatedPos;
                        float frac = modulatedPos - modulatedIndex;
                        
                        // Clamp and reverse the modulated position
                        modulatedIndex = std::clamp(modulatedIndex, 0, ch.grains[g].grainSize - 1);
                        int modulatedReverseIndex = ch.grains[g].grainSize - 1 - modulatedIndex;
                        int nextIndex = std::max(0, modulatedReverseIndex - 1);
                        
                        float sample1 = ch.grains[g].buffer[modulatedReverseIndex];
                        float sample2 = ch.grains[g].buffer[nextIndex];
                        grainSample = sample1 * (1.0f - frac) + sample2 * frac;
                    }
                    else
                    {
                        grainSample = ch.grains[g].buffer[reverseIndex];
                    }
                    
                    float windowGain = ch.windowFunction[readPos];
                    ch.outputBuffer[ch.outputWritePos] += grainSample * windowGain * ch.grains[g].amplitude;
                    
                    ch.grains[g].readPosition++;
                }
                else
                {
                    // Check if we should repeat
                    if (!ch.isRepeating)
                    {
                        ch.isRepeating = true;
                        ch.grains[g].readPosition = 0;  // Restart for second pass
                    }
                    else
                    {
                        ch.grains[g].active = false;
                        ch.isRepeating = false;
                    }
                }
            }
        }
        
        // Spawn new grain at hop intervals
        ch.grainCounter++;
        if (ch.grainCounter >= ch.hopSize)
        {
            ch.grainCounter = 0;
            
            // Find inactive grain
            int grainToUse = -1;
            for (int g = 0; g < Channel::NUM_GRAINS; ++g)
            {
                if (!ch.grains[g].active)
                {
                    grainToUse = g;
                    break;
                }
            }
            
            if (grainToUse != -1)
            {
                int delayReadPos = (ch.delayWritePos - ch.windowSamples + ch.delayLine.size()) % ch.delayLine.size();
                
                for (int s = 0; s < ch.windowSamples; ++s)
                {
                    ch.grains[grainToUse].buffer[s] = ch.delayLine[(delayReadPos + s) % ch.delayLine.size()];
                }
                
                ch.grains[grainToUse].active = true;
                ch.grains[grainToUse].readPosition = 0;
                ch.grains[grainToUse].amplitude = 1.0f;
                ch.grains[grainToUse].grainSize = ch.windowSamples;
                ch.isRepeating = false;  // Reset repeat state for new grain
            }
        }
        
        // Read from output buffer
        float outputSample = ch.outputBuffer[ch.outputReadPos];
        
        // Add feedback from previous output
        float wetSignal = outputSample + (ch.feedbackSample * currentFeedbackGain);
        
        // Apply soft limiting like original
        if (std::fabs(wetSignal) > FEEDBACK_HARD_LIMIT)
        {
            wetSignal = std::tanh(wetSignal * 0.7f) * 1.4286f;
        }
        
        // Mix with dry signal
        float processedSample = inputSample * dryMix + wetSignal * wetMix;
        
        // Store output for next sample's feedback
        ch.feedbackSample = processedSample;
        
        channelData[i] = processedSample;
        
        // Update positions
        ch.delayWritePos = (ch.delayWritePos + 1) % ch.delayLine.size();
        ch.outputReadPos = (ch.outputReadPos + 1) % ch.outputBuffer.size();
        ch.outputWritePos = (ch.outputWritePos + 1) % ch.outputBuffer.size();
    }
}

void ReverseEngine::createWindowFunction(std::vector<float>& window, int length)
{
    // Create Hann window
    for (int i = 0; i < length; ++i)
    {
        float value = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * i / (length - 1));
        
        // Apply envelope at edges based on envelope time
        if (envelopeTime > 0.0f)
        {
            int fadeLength = static_cast<int>(envelopeTime * sampleRate);
            fadeLength = std::min(fadeLength, length / 2);  // Limit to half window
            
            if (i < fadeLength)
            {
                float fadeIn = static_cast<float>(i) / static_cast<float>(fadeLength);
                value *= fadeIn * fadeIn;  // Square for smoother fade
            }
            else if (i >= length - fadeLength)
            {
                float fadeOut = static_cast<float>(length - 1 - i) / static_cast<float>(fadeLength);
                value *= fadeOut * fadeOut;  // Square for smoother fade
            }
        }
        
        window[i] = value;
    }
}

float ReverseEngine::getVibratoModulation(Channel& ch)
{
    ch.vibratoPhase.setTargetValue(ch.vibratoPhase.getTargetValue() + ch.vibratoRate / sampleRate);
    if (ch.vibratoPhase.getTargetValue() >= 1.0f)
        ch.vibratoPhase.setCurrentAndTargetValue(ch.vibratoPhase.getTargetValue() - 1.0f);
    
    return std::sin(2.0f * juce::MathConstants<float>::pi * ch.vibratoPhase.getNextValue());
}
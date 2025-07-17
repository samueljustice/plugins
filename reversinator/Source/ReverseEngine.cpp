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
        channel->circularBuffer.resize(channel->windowSamples);
        
        // Fade length - 5ms for smoother transitions
        channel->fadeLength = static_cast<int>(0.005f * sampleRate);
        channel->fadeInBuffer.resize(channel->fadeLength);
        channel->fadeOutBuffer.resize(channel->fadeLength);
        channel->repeatBuffer.resize(channel->windowSamples);
        channel->vibratoPhase.reset(sampleRate, 0.001);
        
        createFadeWindow(channel->fadeInBuffer, channel->fadeOutBuffer, channel->fadeLength);
        
        channels.push_back(std::move(channel));
    }
    
    reset();
}

void ReverseEngine::reset()
{
    for (auto& channel : channels)
    {
        std::fill(channel->circularBuffer.begin(), channel->circularBuffer.end(), 0.0f);
        std::fill(channel->repeatBuffer.begin(), channel->repeatBuffer.end(), 0.0f);
        channel->writePosition = 0;
        channel->readPosition = channel->windowSamples - 1;
        channel->feedbackSample = 0.0f;
        channel->crossfadePosition = 0.0f;
        channel->crossfadeDirection = true;
        channel->repeatPosition = 0;
        channel->isRepeating = false;
        channel->vibratoPhase.setCurrentAndTargetValue(0.0f);
    }
}

void ReverseEngine::setParameters(float windowTimeSeconds, float feedbackAmount, float wetMixAmount, float dryMixAmount, int mode, float crossfadePercent)
{
    // Clamp minimum window time to 30ms to prevent excessive crackling
    windowTime = std::max(0.03f, windowTimeSeconds);
    feedback = feedbackAmount;
    wetMix = wetMixAmount;
    dryMix = dryMixAmount;
    effectMode = mode;
    crossfadeTime = crossfadePercent / 100.0f;
    
    int newWindowSamples = static_cast<int>(windowTime * sampleRate);
    
    for (auto& channel : channels)
    {
        if (newWindowSamples != channel->windowSamples)
        {
            channel->windowSamples = newWindowSamples;
            channel->circularBuffer.resize(channel->windowSamples);
            channel->repeatBuffer.resize(channel->windowSamples);
            std::fill(channel->circularBuffer.begin(), channel->circularBuffer.end(), 0.0f);
            std::fill(channel->repeatBuffer.begin(), channel->repeatBuffer.end(), 0.0f);
            channel->writePosition = 0;
            channel->readPosition = channel->windowSamples - 1;
            
            // Update fade buffers - use 5ms for smoother transitions
            channel->fadeLength = static_cast<int>(0.005f * sampleRate);
            channel->fadeInBuffer.resize(channel->fadeLength);
            channel->fadeOutBuffer.resize(channel->fadeLength);
            createFadeWindow(channel->fadeInBuffer, channel->fadeOutBuffer, channel->fadeLength);
        }
    }
}

void ReverseEngine::process(juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    
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
    for (int i = 0; i < numSamples; ++i)
    {
        float inputSample = channelData[i];
        
        // Write input to buffer
        ch.circularBuffer[ch.writePosition] = inputSample + (ch.feedbackSample * feedback);
        
        // Read in reverse
        float outputSample = ch.circularBuffer[ch.readPosition];
        
        // Apply fade at chunk boundaries using precomputed windows
        if (ch.readPosition < ch.fadeLength)
        {
            outputSample *= ch.fadeInBuffer[ch.readPosition];
        }
        else if (ch.readPosition >= ch.windowSamples - ch.fadeLength)
        {
            outputSample *= ch.fadeOutBuffer[ch.windowSamples - 1 - ch.readPosition];
        }
        
        ch.feedbackSample = outputSample;
        
        // Mix output
        channelData[i] = outputSample * wetMix + inputSample * dryMix;
        
        // Update positions
        ch.writePosition++;
        if (ch.writePosition >= ch.windowSamples)
            ch.writePosition = 0;
            
        ch.readPosition--;
        if (ch.readPosition < 0)
            ch.readPosition = ch.windowSamples - 1;
    }
}

void ReverseEngine::processForwardBackwards(Channel& ch, float* channelData, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        float inputSample = channelData[i];
        
        // Write to buffer
        ch.circularBuffer[ch.writePosition] = inputSample + (ch.feedbackSample * feedback);
        
        // Determine position in cycle (0-1)
        float cyclePos = static_cast<float>(ch.writePosition) / static_cast<float>(ch.windowSamples);
        
        float outputSample;
        
        // First half: play forward
        if (cyclePos < 0.5f)
        {
            outputSample = ch.circularBuffer[ch.writePosition];
        }
        // Second half: play backward
        else
        {
            int reversePos = ch.windowSamples - 1 - ch.writePosition;
            outputSample = ch.circularBuffer[reversePos];
        }
        
        // Apply crossfade at transition point
        float transitionZone = crossfadeTime * 0.5f; // crossfade zone size
        if (cyclePos > 0.5f - transitionZone && cyclePos < 0.5f + transitionZone)
        {
            float crossfadePos = (cyclePos - (0.5f - transitionZone)) / (2.0f * transitionZone);
            float forward = ch.circularBuffer[ch.writePosition];
            int reversePos = ch.windowSamples - 1 - ch.writePosition;
            float backward = ch.circularBuffer[reversePos];
            outputSample = forward * (1.0f - crossfadePos) + backward * crossfadePos;
        }
        
        ch.feedbackSample = outputSample;
        
        channelData[i] = outputSample * wetMix + inputSample * dryMix;
        
        ch.writePosition++;
        if (ch.writePosition >= ch.windowSamples)
            ch.writePosition = 0;
    }
}

void ReverseEngine::processReverseRepeat(Channel& ch, float* channelData, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        float inputSample = channelData[i];
        
        // Always write to circular buffer
        ch.circularBuffer[ch.writePosition] = inputSample + (ch.feedbackSample * feedback);
        
        // Copy to repeat buffer when not repeating
        if (!ch.isRepeating)
        {
            ch.repeatBuffer[ch.writePosition] = ch.circularBuffer[ch.writePosition];
        }
        
        float outputSample;
        
        if (ch.isRepeating)
        {
            // Play reversed from repeat buffer
            int reversePos = ch.windowSamples - 1 - ch.repeatPosition;
            outputSample = ch.repeatBuffer[reversePos];
            
            // Add vibrato in second half
            if (ch.repeatPosition >= ch.windowSamples / 2)
            {
                float vibrato = getVibratoModulation(ch);
                int vibratoOffset = static_cast<int>(vibrato * 5.0f);
                int modulatedPos = reversePos + vibratoOffset;
                if (modulatedPos >= 0 && modulatedPos < ch.windowSamples)
                {
                    outputSample = ch.repeatBuffer[modulatedPos];
                }
            }
            
            ch.repeatPosition++;
            if (ch.repeatPosition >= ch.windowSamples)
            {
                ch.repeatPosition = 0;
                ch.isRepeating = false;
            }
        }
        else
        {
            // Normal forward playback
            outputSample = ch.circularBuffer[ch.writePosition];
        }
        
        ch.feedbackSample = outputSample;
        
        channelData[i] = outputSample * wetMix + inputSample * dryMix;
        
        ch.writePosition++;
        if (ch.writePosition >= ch.windowSamples)
        {
            ch.writePosition = 0;
            // Start repeat cycle
            if (!ch.isRepeating)
            {
                ch.isRepeating = true;
                ch.repeatPosition = 0;
            }
        }
    }
}

void ReverseEngine::createFadeWindow(std::vector<float>& fadeIn, std::vector<float>& fadeOut, int length)
{
    for (int i = 0; i < length; ++i)
    {
        float position = static_cast<float>(i) / static_cast<float>(length - 1);
        // Raised cosine window for smoother fades
        float value = 0.5f * (1.0f - std::cos(position * juce::MathConstants<float>::pi));
        fadeIn[i] = value;
        fadeOut[i] = 1.0f - value;
    }
}

float ReverseEngine::getVibratoModulation(Channel& ch)
{
    ch.vibratoPhase.setTargetValue(ch.vibratoPhase.getTargetValue() + ch.vibratoRate / sampleRate);
    if (ch.vibratoPhase.getTargetValue() >= 1.0f)
        ch.vibratoPhase.setCurrentAndTargetValue(ch.vibratoPhase.getTargetValue() - 1.0f);
    
    return std::sin(2.0f * juce::MathConstants<float>::pi * ch.vibratoPhase.getNextValue());
}
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
        channel->circularBuffer.resize(channel->windowSamples * 3);  // Extra space to prevent wrap errors
        // Dynamic fade ratio: smaller percentage for tiny windows
        float fadeRatio = juce::jlimit(0.05f, 0.15f, 0.15f * static_cast<float>(windowTime * sampleRate / 44100.0));
        channel->fadeLength = std::max(
            static_cast<int>(windowTime * sampleRate * fadeRatio),
            static_cast<int>(0.005f * sampleRate)  // 5ms minimum
        );
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
        
        // Pre-fill buffers with silence to avoid initial clicks
        for (int i = 0; i < channel->fadeLength; ++i)
        {
            channel->circularBuffer[i] = 0.0f;
            channel->circularBuffer[channel->windowSamples - 1 - i] = 0.0f;
        }
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
    crossfadeTime = crossfadePercent / 100.0f; // Convert percentage to 0-1 range
    
    int newWindowSamples = static_cast<int>(windowTime * sampleRate);
    
    for (auto& channel : channels)
    {
        if (newWindowSamples != channel->windowSamples)
        {
            channel->windowSamples = newWindowSamples;
            channel->circularBuffer.resize(channel->windowSamples * 3);  // Extra space to prevent wrap errors
            channel->repeatBuffer.resize(channel->windowSamples);
            std::fill(channel->circularBuffer.begin(), channel->circularBuffer.end(), 0.0f);
            std::fill(channel->repeatBuffer.begin(), channel->repeatBuffer.end(), 0.0f);
            channel->writePosition = 0;
            channel->readPosition = channel->windowSamples - 1;
            
            // Update fade buffers for new window size
            // Dynamic fade ratio: smaller percentage for tiny windows
        float fadeRatio = juce::jlimit(0.05f, 0.15f, 0.15f * static_cast<float>(windowTime * sampleRate / 44100.0));
        channel->fadeLength = std::max(
            static_cast<int>(windowTime * sampleRate * fadeRatio),
            static_cast<int>(0.005f * sampleRate)  // 5ms minimum
        );
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
        
        // Add anti-denormal noise to prevent silence clicks
        const float noise = (float) juce::Random::getSystemRandom().nextDouble() * 1.0e-7f - 5.0e-8f;
        inputSample += noise;
        
        ch.circularBuffer[ch.writePosition] = inputSample + (ch.feedbackSample * feedback);
        
        float outputSample = ch.circularBuffer[ch.readPosition];
        
        int fadePosition = (ch.windowSamples - 1 - ch.readPosition);
        if (fadePosition < ch.fadeLength)
        {
            outputSample *= ch.fadeInBuffer[fadePosition];
        }
        else if (ch.readPosition < ch.fadeLength)
        {
            outputSample *= ch.fadeOutBuffer[ch.readPosition];
        }
        
        ch.feedbackSample = outputSample;
        
        channelData[i] = outputSample * wetMix + inputSample * dryMix;
        
        ch.writePosition = (ch.writePosition + 1) % ch.windowSamples;
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
        
        // Add anti-denormal noise to prevent silence clicks
        const float noise = (float) juce::Random::getSystemRandom().nextDouble() * 1.0e-7f - 5.0e-8f;
        inputSample += noise;
        
        ch.circularBuffer[ch.writePosition] = inputSample + (ch.feedbackSample * feedback);
        ch.circularBuffer[ch.writePosition + ch.windowSamples] = ch.circularBuffer[ch.writePosition];
        
        int forwardPos = ch.writePosition;
        int backwardPos = ch.windowSamples - 1 - (ch.writePosition % ch.windowSamples);
        
        float forwardSample = ch.circularBuffer[forwardPos];
        float backwardSample = ch.circularBuffer[backwardPos + ch.windowSamples];
        
        // Apply fade windows to prevent clicks at boundaries
        float fadeFactor = 1.0f;
        if (ch.writePosition < ch.fadeLength)
        {
            fadeFactor = ch.fadeInBuffer[ch.writePosition];
        }
        else if (ch.writePosition >= ch.windowSamples - ch.fadeLength)
        {
            int fadeIndex = ch.writePosition - (ch.windowSamples - ch.fadeLength);
            fadeFactor = ch.fadeOutBuffer[fadeIndex];
        }
        
        forwardSample *= fadeFactor;
        backwardSample *= fadeFactor;
        
        // Simple sine wave crossfade
        float crossfade = 0.5f + 0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * ch.crossfadePosition);
        
        // Apply user crossfade setting
        if (crossfadeTime > 0.0f)
        {
            // Smooth the crossfade transitions
            float smoothness = crossfadeTime;
            crossfade = 0.5f + (crossfade - 0.5f) * smoothness;
        }
        
        float outputSample = forwardSample * (1.0f - crossfade) + backwardSample * crossfade;
        
        ch.feedbackSample = outputSample;
        
        channelData[i] = outputSample * wetMix + inputSample * dryMix;
        
        ch.crossfadePosition += 1.0f / (ch.windowSamples * 2.0f);
        if (ch.crossfadePosition >= 1.0f)
            ch.crossfadePosition -= 1.0f;
        
        ch.writePosition = (ch.writePosition + 1) % ch.windowSamples;
    }
}

void ReverseEngine::processReverseRepeat(Channel& ch, float* channelData, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        float inputSample = channelData[i];
        
        // Add anti-denormal noise to prevent silence clicks
        const float noise = (float) juce::Random::getSystemRandom().nextDouble() * 1.0e-7f - 5.0e-8f;
        inputSample += noise;
        
        ch.circularBuffer[ch.writePosition] = inputSample + (ch.feedbackSample * feedback);
        
        if (!ch.isRepeating)
        {
            ch.repeatBuffer[ch.writePosition] = ch.circularBuffer[ch.writePosition];
        }
        
        float outputSample;
        
        if (ch.isRepeating)
        {
            int reversePos = ch.windowSamples - 1 - ch.repeatPosition;
            outputSample = ch.repeatBuffer[reversePos];
            
            if (ch.repeatPosition >= ch.windowSamples / 2)
            {
                float vibrato = getVibratoModulation(ch);
                int vibratoSampleDelay = static_cast<int>(vibrato * 10.0f);
                if (reversePos - vibratoSampleDelay >= 0)
                {
                    outputSample = ch.repeatBuffer[reversePos - vibratoSampleDelay];
                }
            }
            
            // Apply fade at repeat boundaries
            float fadeFactor = 1.0f;
            if (ch.repeatPosition < ch.fadeLength)
            {
                fadeFactor = ch.fadeInBuffer[ch.repeatPosition];
            }
            else if (ch.repeatPosition >= ch.windowSamples - ch.fadeLength)
            {
                int fadeIndex = ch.repeatPosition - (ch.windowSamples - ch.fadeLength);
                fadeFactor = ch.fadeOutBuffer[fadeIndex];
            }
            outputSample *= fadeFactor;
            
            ch.repeatPosition++;
            if (ch.repeatPosition >= ch.windowSamples)
            {
                ch.repeatPosition = 0;
                ch.isRepeating = false;
            }
        }
        else
        {
            outputSample = ch.circularBuffer[ch.writePosition];
        }
        
        ch.feedbackSample = outputSample;
        
        channelData[i] = outputSample * wetMix + inputSample * dryMix;
        
        ch.writePosition = (ch.writePosition + 1) % ch.windowSamples;
        
        if (ch.writePosition == 0 && !ch.isRepeating)
        {
            ch.isRepeating = true;
            ch.repeatPosition = 0;
        }
    }
}

void ReverseEngine::createFadeWindow(std::vector<float>& fadeIn, std::vector<float>& fadeOut, int length)
{
    for (int i = 0; i < length; ++i)
    {
        float position = static_cast<float>(i) / static_cast<float>(length - 1);
        // Use Hann window for smoother transitions
        float hannValue = 0.5f * (1.0f - std::cos(2.0f * position * juce::MathConstants<float>::pi));
        fadeIn[i] = hannValue;
        fadeOut[i] = 1.0f - hannValue;
    }
}

float ReverseEngine::getVibratoModulation(Channel& ch)
{
    ch.vibratoPhase.setTargetValue(ch.vibratoPhase.getTargetValue() + ch.vibratoRate / sampleRate);
    if (ch.vibratoPhase.getTargetValue() >= 1.0f)
        ch.vibratoPhase.setCurrentAndTargetValue(ch.vibratoPhase.getTargetValue() - 1.0f);
    
    return std::sin(2.0f * juce::MathConstants<float>::pi * ch.vibratoPhase.getNextValue());
}
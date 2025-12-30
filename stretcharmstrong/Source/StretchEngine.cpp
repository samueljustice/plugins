#include "StretchEngine.h"
#include <cmath>
#include <algorithm>

StretchEngine::StretchEngine()
{
}

StretchEngine::~StretchEngine()
{
}

void StretchEngine::prepare(double newSampleRate, int newMaxBlockSize, StretchType type, float ratio)
{
    sampleRate = newSampleRate;
    maxBlockSize = newMaxBlockSize;
    stretchType = type;
    stretchRatio = ratio;
    targetStretchRatio = ratio;
    smoothedStretchRatio = 1.0f;

    // Initialize Rubber Band for time-stretching with smoother settings
    RubberBand::RubberBandStretcher::Options options =
        RubberBand::RubberBandStretcher::OptionProcessRealTime |
        RubberBand::RubberBandStretcher::OptionStretchElastic |
        RubberBand::RubberBandStretcher::OptionTransientsSmooth |
        RubberBand::RubberBandStretcher::OptionPhaseIndependent |
        RubberBand::RubberBandStretcher::OptionWindowLong |
        RubberBand::RubberBandStretcher::OptionSmoothingOn;

    rubberBand = std::make_unique<RubberBand::RubberBandStretcher>(
        static_cast<size_t>(sampleRate),
        2, // stereo
        options,
        1.0, // initial time ratio
        1.0  // initial pitch scale
    );

    rubberBand->setMaxProcessSize(static_cast<size_t>(maxBlockSize));
    rubberBandPrimed = false;

    // Initialize output ring buffer for time stretching
    outputRingBuffer.resize(2);
    for (auto& channelBuffer : outputRingBuffer)
    {
        channelBuffer.resize(ringBufferSize, 0.0f);
    }
    ringBufferWritePos = 0;
    ringBufferReadPos = 0;
    ringBufferAvailable = 0;

    // Initialize varispeed circular buffer
    varispeedBuffer.resize(2);
    for (auto& channelBuffer : varispeedBuffer)
    {
        channelBuffer.resize(varispeedBufferSize, 0.0f);
    }
    varispeedReadPos = 0.0;
    varispeedWritePos = 0;

    // Initialize working buffers
    inputBuffers.resize(2);
    inputPtrs.resize(2);
    retrieveBuffers.resize(2);
    retrievePtrs.resize(2);

    for (int ch = 0; ch < 2; ++ch)
    {
        inputBuffers[ch].resize(static_cast<size_t>(maxBlockSize));
        retrieveBuffers[ch].resize(static_cast<size_t>(maxBlockSize * 8)); // Extra room for stretched output
        inputPtrs[ch] = inputBuffers[ch].data();
        retrievePtrs[ch] = retrieveBuffers[ch].data();
    }

    // Initialize crossfade buffer
    crossfadeBuffer.resize(2);
    for (auto& channelBuffer : crossfadeBuffer)
    {
        channelBuffer.resize(crossfadeLength, 0.0f);
    }
    needsCrossfade = false;
    crossfadeSamples = 0;

    currentEnvelope = 0.0f;
    previousEnvelope = 0.0f;
}

void StretchEngine::reset()
{
    if (rubberBand)
    {
        rubberBand->reset();
    }
    rubberBandPrimed = false;

    ringBufferWritePos = 0;
    ringBufferReadPos = 0;
    ringBufferAvailable = 0;

    for (auto& channelBuffer : outputRingBuffer)
    {
        std::fill(channelBuffer.begin(), channelBuffer.end(), 0.0f);
    }

    varispeedReadPos = 0.0;
    varispeedWritePos = 0;

    currentEnvelope = 0.0f;
    previousEnvelope = 0.0f;
    smoothedStretchRatio = 1.0f;
}

void StretchEngine::setStretchType(StretchType type)
{
    if (stretchType != type)
    {
        stretchType = type;
        reset();
    }
}

void StretchEngine::setStretchRatio(float ratio)
{
    targetStretchRatio = juce::jlimit(0.25f, 4.0f, ratio);
}

int StretchEngine::getLatencySamples() const
{
    if (rubberBand && stretchType == StretchType::TimeStretch)
    {
        return static_cast<int>(rubberBand->getLatency()) + 512;
    }
    return 256;
}

void StretchEngine::process(juce::AudioBuffer<float>& buffer, float envelopeValue)
{
    previousEnvelope = currentEnvelope;

    // Smooth the envelope to prevent clicks
    currentEnvelope = currentEnvelope * envelopeSmoothingCoeff + envelopeValue * (1.0f - envelopeSmoothingCoeff);

    // Detect transitions
    bool wasActive = previousEnvelope > 0.01f;
    bool isActive = currentEnvelope > 0.01f;

    if (wasActive != isActive)
    {
        needsCrossfade = true;
        crossfadeSamples = 0;

        // Store current output for crossfade
        int numChannels = std::min(buffer.getNumChannels(), 2);
        int samplesToStore = std::min(buffer.getNumSamples(), crossfadeLength);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* src = buffer.getReadPointer(ch);
            for (int i = 0; i < samplesToStore; ++i)
            {
                crossfadeBuffer[ch][i] = src[i];
            }
        }
    }

    // If envelope is essentially zero, pass through dry signal
    if (currentEnvelope < 0.001f && previousEnvelope < 0.001f)
    {
        return;
    }

    switch (stretchType)
    {
        case StretchType::Varispeed:
            processVarispeed(buffer, currentEnvelope);
            break;

        case StretchType::TimeStretch:
            processTimeStretch(buffer, currentEnvelope);
            break;
    }

    // Apply crossfade if needed
    if (needsCrossfade)
    {
        int numChannels = std::min(buffer.getNumChannels(), 2);
        int numSamples = buffer.getNumSamples();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* output = buffer.getWritePointer(ch);

            for (int i = 0; i < numSamples && crossfadeSamples < crossfadeLength; ++i)
            {
                float crossfadeProgress = static_cast<float>(crossfadeSamples) / static_cast<float>(crossfadeLength);
                float fadeIn = crossfadeProgress;
                float fadeOut = 1.0f - crossfadeProgress;

                // Cosine crossfade for smoother transition
                fadeIn = 0.5f * (1.0f - std::cos(fadeIn * juce::MathConstants<float>::pi));
                fadeOut = 0.5f * (1.0f + std::cos(fadeIn * juce::MathConstants<float>::pi));

                output[i] = output[i] * fadeIn + crossfadeBuffer[ch][crossfadeSamples] * fadeOut;
                crossfadeSamples++;
            }
        }

        if (crossfadeSamples >= crossfadeLength)
        {
            needsCrossfade = false;
        }
    }
}

void StretchEngine::processVarispeed(juce::AudioBuffer<float>& buffer, float envelope)
{
    int numSamples = buffer.getNumSamples();
    int numChannels = std::min(buffer.getNumChannels(), 2);

    // Smoothly interpolate stretch ratio
    float targetRatio = 1.0f + (targetStretchRatio - 1.0f) * envelope;
    smoothedStretchRatio = smoothedStretchRatio * 0.995f + targetRatio * 0.005f;

    // Calculate playback rate (inverse of stretch ratio for varispeed)
    double playbackRate = 1.0 / static_cast<double>(smoothedStretchRatio);

    // Write input to circular buffer
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* inputData = buffer.getReadPointer(ch);
        auto& channelBuffer = varispeedBuffer[static_cast<size_t>(ch)];

        for (int i = 0; i < numSamples; ++i)
        {
            int writeIdx = (varispeedWritePos + i) % varispeedBufferSize;
            channelBuffer[writeIdx] = inputData[i];
        }
    }

    // Read from circular buffer with interpolation
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* outputData = buffer.getWritePointer(ch);
        const auto& channelBuffer = varispeedBuffer[static_cast<size_t>(ch)];

        double readPos = varispeedReadPos;

        for (int i = 0; i < numSamples; ++i)
        {
            // Wrap read position
            while (readPos >= varispeedBufferSize)
                readPos -= varispeedBufferSize;
            while (readPos < 0)
                readPos += varispeedBufferSize;

            outputData[i] = hermiteInterpolate(channelBuffer.data(), varispeedBufferSize, readPos);
            readPos += playbackRate;
        }
    }

    // Update positions
    varispeedWritePos = (varispeedWritePos + numSamples) % varispeedBufferSize;
    varispeedReadPos += static_cast<double>(numSamples) * playbackRate;

    // Keep read position wrapped
    while (varispeedReadPos >= varispeedBufferSize)
        varispeedReadPos -= varispeedBufferSize;
    while (varispeedReadPos < 0)
        varispeedReadPos += varispeedBufferSize;
}

void StretchEngine::processTimeStretch(juce::AudioBuffer<float>& buffer, float envelope)
{
    if (!rubberBand)
        return;

    int numSamples = buffer.getNumSamples();
    int numChannels = std::min(buffer.getNumChannels(), 2);

    // Smoothly interpolate the effective ratio
    float targetRatio = 1.0f + (targetStretchRatio - 1.0f) * envelope;
    smoothedStretchRatio = smoothedStretchRatio * 0.99f + targetRatio * 0.01f;

    // Update Rubber Band time ratio
    rubberBand->setTimeRatio(static_cast<double>(smoothedStretchRatio));

    // Copy input to working buffers
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* src = buffer.getReadPointer(ch);
        std::copy(src, src + numSamples, inputBuffers[ch].begin());
    }

    // If we don't have 2 channels, duplicate mono to stereo for Rubber Band
    if (numChannels == 1)
    {
        std::copy(inputBuffers[0].begin(), inputBuffers[0].begin() + numSamples, inputBuffers[1].begin());
    }

    // Feed input to Rubber Band
    const float* ptrs[2] = { inputBuffers[0].data(), inputBuffers[1].data() };
    rubberBand->process(ptrs, static_cast<size_t>(numSamples), false);

    // Retrieve all available output and store in ring buffer
    while (rubberBand->available() > 0)
    {
        int available = static_cast<int>(rubberBand->available());
        int toRetrieve = std::min(available, static_cast<int>(retrieveBuffers[0].size()));

        float* rptrs[2] = { retrieveBuffers[0].data(), retrieveBuffers[1].data() };
        size_t retrieved = rubberBand->retrieve(rptrs, static_cast<size_t>(toRetrieve));

        if (retrieved > 0)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                writeToRingBuffer(ch, retrieveBuffers[ch].data(), static_cast<int>(retrieved));
            }
        }
    }

    // Read from ring buffer to output
    // We want to output numSamples, but we may not have enough yet
    int availableInRing = ringBufferAvailable;

    if (availableInRing >= numSamples)
    {
        // We have enough samples
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* dest = buffer.getWritePointer(ch);
            readFromRingBuffer(ch, dest, numSamples);
        }
    }
    else if (availableInRing > 0)
    {
        // Partial output - crossfade with input to avoid clicks
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* dest = buffer.getWritePointer(ch);
            const float* src = buffer.getReadPointer(ch);

            // Read what we have
            std::vector<float> tempBuf(availableInRing);
            readFromRingBuffer(ch, tempBuf.data(), availableInRing);

            // Copy stretched samples
            for (int i = 0; i < availableInRing; ++i)
            {
                dest[i] = tempBuf[i];
            }

            // Crossfade remaining samples with dry input
            int crossfadeLen = std::min(64, numSamples - availableInRing);
            for (int i = 0; i < crossfadeLen; ++i)
            {
                float fade = static_cast<float>(i) / static_cast<float>(crossfadeLen);
                int idx = availableInRing + i;
                if (idx < numSamples)
                {
                    dest[idx] = src[idx] * fade;
                }
            }

            // Fill rest with faded dry
            for (int i = availableInRing + crossfadeLen; i < numSamples; ++i)
            {
                dest[i] = src[i] * 0.5f; // Attenuate to reduce artifacts
            }
        }
    }
    else
    {
        // No output yet - we're still priming, pass through with attenuation
        float atten = rubberBandPrimed ? 0.0f : 1.0f;
        if (!rubberBandPrimed && ringBufferWritePos > 0)
        {
            rubberBandPrimed = true;
        }

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* dest = buffer.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                dest[i] *= atten;
            }
        }
    }
}

void StretchEngine::writeToRingBuffer(int channel, const float* data, int numSamples)
{
    if (channel < 0 || channel >= 2 || numSamples <= 0)
        return;

    auto& ringBuf = outputRingBuffer[channel];

    for (int i = 0; i < numSamples; ++i)
    {
        ringBuf[ringBufferWritePos] = data[i];
        ringBufferWritePos = (ringBufferWritePos + 1) % ringBufferSize;
    }

    ringBufferAvailable = std::min(ringBufferAvailable + numSamples, ringBufferSize);
}

int StretchEngine::readFromRingBuffer(int channel, float* data, int numSamples)
{
    if (channel < 0 || channel >= 2 || numSamples <= 0)
        return 0;

    int toRead = std::min(numSamples, ringBufferAvailable);
    auto& ringBuf = outputRingBuffer[channel];

    for (int i = 0; i < toRead; ++i)
    {
        data[i] = ringBuf[ringBufferReadPos];
        ringBufferReadPos = (ringBufferReadPos + 1) % ringBufferSize;
    }

    // Only update available count once (for channel 0)
    if (channel == 0)
    {
        ringBufferAvailable -= toRead;
    }

    return toRead;
}

float StretchEngine::hermiteInterpolate(const float* buffer, int bufferSize, double position)
{
    int idx0 = static_cast<int>(position);
    float frac = static_cast<float>(position - idx0);

    // Get 4 points for Hermite interpolation
    int idxM1 = (idx0 - 1 + bufferSize) % bufferSize;
    int idx1 = (idx0 + 1) % bufferSize;
    int idx2 = (idx0 + 2) % bufferSize;
    idx0 = idx0 % bufferSize;

    float xm1 = buffer[idxM1];
    float x0 = buffer[idx0];
    float x1 = buffer[idx1];
    float x2 = buffer[idx2];

    // Hermite interpolation coefficients
    float c0 = x0;
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);

    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

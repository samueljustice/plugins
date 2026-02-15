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

    // Initialize Rubber Band for time-stretching
    // Using OptionEngineFiner for better quality (R3 engine)
    RubberBand::RubberBandStretcher::Options options =
        RubberBand::RubberBandStretcher::OptionProcessRealTime |
        RubberBand::RubberBandStretcher::OptionEngineFiner |
        RubberBand::RubberBandStretcher::OptionWindowLong |
        RubberBand::RubberBandStretcher::OptionSmoothingOn |
        RubberBand::RubberBandStretcher::OptionFormantPreserved |
        RubberBand::RubberBandStretcher::OptionPitchHighConsistency;

    rubberBand = std::make_unique<RubberBand::RubberBandStretcher>(
        static_cast<size_t>(sampleRate),
        2, // stereo
        options,
        1.0, // initial time ratio
        1.0  // initial pitch scale
    );

    rubberBand->setMaxProcessSize(static_cast<size_t>(maxBlockSize));
    rubberBandPrimed = false;
    primingSamplesNeeded = static_cast<int>(rubberBand->getLatency()) + maxBlockSize * 2;
    primingSamplesFed = 0;

    // Initialize output ring buffer for time stretching (per-channel positions)
    outputRingBuffer.resize(2);
    for (auto& channelBuffer : outputRingBuffer)
    {
        channelBuffer.resize(ringBufferSize, 0.0f);
    }
    ringWritePos.fill(0);
    ringReadPos.fill(0);
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
    retrieveBuffers.resize(2);

    for (int ch = 0; ch < 2; ++ch)
    {
        inputBuffers[ch].resize(static_cast<size_t>(maxBlockSize));
        retrieveBuffers[ch].resize(static_cast<size_t>(maxBlockSize * 8));
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
    primingSamplesFed = 0;

    ringWritePos.fill(0);
    ringReadPos.fill(0);
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
        return static_cast<int>(rubberBand->getLatency()) + 1024;
    }
    return 256;
}

void StretchEngine::process(juce::AudioBuffer<float>& buffer, float envelopeValue)
{
    previousEnvelope = currentEnvelope;
    currentEnvelope = currentEnvelope * envelopeSmoothingCoeff + envelopeValue * (1.0f - envelopeSmoothingCoeff);

    // Detect transitions for crossfade
    bool wasActive = previousEnvelope > 0.01f;
    bool isActive = currentEnvelope > 0.01f;

    if (wasActive != isActive)
    {
        needsCrossfade = true;
        crossfadeSamples = 0;

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
                float t = static_cast<float>(crossfadeSamples) / static_cast<float>(crossfadeLength);
                // Equal power crossfade
                float fadeIn = std::sin(t * juce::MathConstants<float>::halfPi);
                float fadeOut = std::cos(t * juce::MathConstants<float>::halfPi);

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
            while (readPos >= varispeedBufferSize)
                readPos -= varispeedBufferSize;
            while (readPos < 0)
                readPos += varispeedBufferSize;

            outputData[i] = hermiteInterpolate(channelBuffer.data(), varispeedBufferSize, readPos);
            readPos += playbackRate;
        }
    }

    varispeedWritePos = (varispeedWritePos + numSamples) % varispeedBufferSize;
    varispeedReadPos += static_cast<double>(numSamples) * playbackRate;

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

    // Very smooth ratio interpolation to prevent glitches
    float targetRatio = 1.0f + (targetStretchRatio - 1.0f) * envelope;
    smoothedStretchRatio = smoothedStretchRatio * 0.998f + targetRatio * 0.002f;

    // Update Rubber Band time ratio
    rubberBand->setTimeRatio(static_cast<double>(smoothedStretchRatio));

    // Copy input to working buffers
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* src = buffer.getReadPointer(ch);
        std::copy(src, src + numSamples, inputBuffers[ch].begin());
    }

    // If mono, duplicate to stereo for Rubber Band
    if (numChannels == 1)
    {
        std::copy(inputBuffers[0].begin(), inputBuffers[0].begin() + numSamples, inputBuffers[1].begin());
    }

    // Feed input to Rubber Band
    const float* ptrs[2] = { inputBuffers[0].data(), inputBuffers[1].data() };
    rubberBand->process(ptrs, static_cast<size_t>(numSamples), false);

    primingSamplesFed += numSamples;

    // Retrieve all available output
    while (rubberBand->available() > 0)
    {
        int available = static_cast<int>(rubberBand->available());
        int toRetrieve = std::min(available, static_cast<int>(retrieveBuffers[0].size()));

        float* rptrs[2] = { retrieveBuffers[0].data(), retrieveBuffers[1].data() };
        size_t retrieved = rubberBand->retrieve(rptrs, static_cast<size_t>(toRetrieve));

        if (retrieved > 0)
        {
            writeToRingBuffer(retrieveBuffers[0].data(), retrieveBuffers[1].data(), static_cast<int>(retrieved));
        }
    }

    // Check if we're still priming
    if (!rubberBandPrimed)
    {
        if (primingSamplesFed >= primingSamplesNeeded && ringBufferAvailable >= numSamples)
        {
            rubberBandPrimed = true;
        }
        else
        {
            // Still priming - output silence or attenuated dry
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* dest = buffer.getWritePointer(ch);
                for (int i = 0; i < numSamples; ++i)
                {
                    dest[i] *= 0.0f; // Silence during priming
                }
            }
            return;
        }
    }

    // Read from ring buffer
    if (ringBufferAvailable >= numSamples)
    {
        readFromRingBuffer(buffer.getWritePointer(0),
                          numChannels > 1 ? buffer.getWritePointer(1) : nullptr,
                          numSamples, numChannels);
    }
    else
    {
        // Buffer underrun - blend with dry signal
        int available = ringBufferAvailable;

        if (available > 0)
        {
            // Temp buffers for what we have
            std::vector<float> tempL(available), tempR(available);
            readFromRingBuffer(tempL.data(), tempR.data(), available, 2);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* dest = buffer.getWritePointer(ch);
                const float* temp = (ch == 0) ? tempL.data() : tempR.data();
                const float* dry = buffer.getReadPointer(ch);

                // Output what we have
                for (int i = 0; i < available; ++i)
                {
                    dest[i] = temp[i];
                }

                // Crossfade to dry for the rest
                int fadeLen = std::min(128, numSamples - available);
                for (int i = 0; i < fadeLen; ++i)
                {
                    float t = static_cast<float>(i) / static_cast<float>(fadeLen);
                    int idx = available + i;
                    dest[idx] = dry[idx] * t;
                }

                // Dry signal for remainder
                for (int i = available + fadeLen; i < numSamples; ++i)
                {
                    dest[i] = dry[i];
                }
            }
        }
    }
}

void StretchEngine::writeToRingBuffer(const float* dataL, const float* dataR, int numSamples)
{
    if (numSamples <= 0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        int writePos = (ringWritePos[0] + i) % ringBufferSize;
        outputRingBuffer[0][writePos] = dataL[i];
        outputRingBuffer[1][writePos] = dataR[i];
    }

    ringWritePos[0] = (ringWritePos[0] + numSamples) % ringBufferSize;
    ringWritePos[1] = ringWritePos[0]; // Keep in sync

    ringBufferAvailable = std::min(ringBufferAvailable + numSamples, ringBufferSize);
}

void StretchEngine::readFromRingBuffer(float* dataL, float* dataR, int numSamples, int numChannels)
{
    if (numSamples <= 0)
        return;

    int toRead = std::min(numSamples, ringBufferAvailable);

    for (int i = 0; i < toRead; ++i)
    {
        int readPos = (ringReadPos[0] + i) % ringBufferSize;
        dataL[i] = outputRingBuffer[0][readPos];
        if (dataR && numChannels > 1)
        {
            dataR[i] = outputRingBuffer[1][readPos];
        }
    }

    ringReadPos[0] = (ringReadPos[0] + toRead) % ringBufferSize;
    ringReadPos[1] = ringReadPos[0]; // Keep in sync

    ringBufferAvailable -= toRead;
}

float StretchEngine::hermiteInterpolate(const float* buffer, int bufferSize, double position)
{
    int idx0 = static_cast<int>(position);
    float frac = static_cast<float>(position - idx0);

    int idxM1 = (idx0 - 1 + bufferSize) % bufferSize;
    int idx1 = (idx0 + 1) % bufferSize;
    int idx2 = (idx0 + 2) % bufferSize;
    idx0 = idx0 % bufferSize;

    float xm1 = buffer[idxM1];
    float x0 = buffer[idx0];
    float x1 = buffer[idx1];
    float x2 = buffer[idx2];

    float c0 = x0;
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);

    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

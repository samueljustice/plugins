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

    // Initialize Rubber Band for time-stretching
    RubberBand::RubberBandStretcher::Options options =
        RubberBand::RubberBandStretcher::OptionProcessRealTime |
        RubberBand::RubberBandStretcher::OptionStretchPrecise |
        RubberBand::RubberBandStretcher::OptionTransientsCrisp |
        RubberBand::RubberBandStretcher::OptionPitchHighQuality |
        RubberBand::RubberBandStretcher::OptionChannelsTogether;

    rubberBand = std::make_unique<RubberBand::RubberBandStretcher>(
        static_cast<size_t>(sampleRate),
        2, // stereo
        options,
        1.0, // initial time ratio
        1.0  // initial pitch scale
    );

    rubberBand->setMaxProcessSize(static_cast<size_t>(maxBlockSize));

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
    outputBuffers.resize(2);
    for (int ch = 0; ch < 2; ++ch)
    {
        inputBuffers[static_cast<size_t>(ch)].resize(static_cast<size_t>(maxBlockSize * 4));
        outputBuffers[static_cast<size_t>(ch)].resize(static_cast<size_t>(maxBlockSize * 4));
    }

    currentEnvelope = 0.0f;
}

void StretchEngine::setStretchType(StretchType type)
{
    stretchType = type;
}

void StretchEngine::setStretchRatio(float ratio)
{
    targetStretchRatio = juce::jlimit(0.1f, 4.0f, ratio);
}

int StretchEngine::getLatencySamples() const
{
    if (rubberBand && stretchType == StretchType::TimeStretch)
    {
        return static_cast<int>(rubberBand->getLatency());
    }
    return 0;
}

void StretchEngine::process(juce::AudioBuffer<float>& buffer, float envelopeValue)
{
    // Smooth the envelope to prevent clicks
    currentEnvelope = currentEnvelope * envelopeSmoothingCoeff + envelopeValue * (1.0f - envelopeSmoothingCoeff);

    // Smoothly interpolate stretch ratio
    stretchRatio = stretchRatio * 0.99f + targetStretchRatio * 0.01f;

    // Calculate effective stretch ratio based on envelope
    // When envelope is 0, ratio is 1.0 (no stretch)
    // When envelope is 1, ratio is the target stretch ratio
    float effectiveRatio = 1.0f + (stretchRatio - 1.0f) * currentEnvelope;

    if (std::abs(effectiveRatio - 1.0f) < 0.001f)
    {
        // No stretching needed, pass through
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
}

void StretchEngine::processVarispeed(juce::AudioBuffer<float>& buffer, float envelope)
{
    int numSamples = buffer.getNumSamples();
    int numChannels = std::min(buffer.getNumChannels(), 2);

    // Calculate playback rate (inverse of stretch ratio for varispeed)
    // stretch ratio > 1 means slower playback (stretched)
    float effectiveRatio = 1.0f + (targetStretchRatio - 1.0f) * envelope;
    double playbackRate = 1.0 / static_cast<double>(effectiveRatio);

    // Write input to circular buffer
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* inputData = buffer.getReadPointer(ch);
        auto& channelBuffer = varispeedBuffer[static_cast<size_t>(ch)];

        for (int i = 0; i < numSamples; ++i)
        {
            channelBuffer[static_cast<size_t>((varispeedWritePos + i) % varispeedBufferSize)] = inputData[i];
        }
    }

    // Read from circular buffer with interpolation
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* outputData = buffer.getWritePointer(ch);
        const auto& channelBuffer = varispeedBuffer[static_cast<size_t>(ch)];

        for (int i = 0; i < numSamples; ++i)
        {
            double readPosition = varispeedReadPos + static_cast<double>(i) * playbackRate;

            // Wrap read position
            while (readPosition >= varispeedBufferSize)
                readPosition -= varispeedBufferSize;
            while (readPosition < 0)
                readPosition += varispeedBufferSize;

            // Use Hermite interpolation for high quality
            outputData[i] = hermiteInterpolate(channelBuffer.data(), varispeedBufferSize, readPosition);
        }
    }

    // Update positions
    varispeedWritePos = (varispeedWritePos + numSamples) % varispeedBufferSize;
    varispeedReadPos += static_cast<double>(numSamples) * playbackRate;

    // Keep read position in sync with write position
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

    // Calculate time ratio based on envelope
    // stretch ratio > 1 means slower (more stretched)
    float effectiveRatio = 1.0f + (targetStretchRatio - 1.0f) * envelope;

    // Update Rubber Band time ratio
    rubberBand->setTimeRatio(static_cast<double>(effectiveRatio));

    // Prepare input pointers
    std::vector<const float*> inputPtrs(static_cast<size_t>(numChannels));
    for (int ch = 0; ch < numChannels; ++ch)
    {
        inputPtrs[static_cast<size_t>(ch)] = buffer.getReadPointer(ch);
    }

    // Process through Rubber Band
    rubberBand->process(inputPtrs.data(), static_cast<size_t>(numSamples), false);

    // Retrieve available output
    int available = static_cast<int>(rubberBand->available());

    if (available > 0)
    {
        // Prepare output pointers
        std::vector<float*> outputPtrs(static_cast<size_t>(numChannels));
        for (int ch = 0; ch < numChannels; ++ch)
        {
            outputBuffers[static_cast<size_t>(ch)].resize(static_cast<size_t>(available));
            outputPtrs[static_cast<size_t>(ch)] = outputBuffers[static_cast<size_t>(ch)].data();
        }

        // Retrieve samples
        size_t retrieved = rubberBand->retrieve(outputPtrs.data(), static_cast<size_t>(available));

        // Copy to output buffer (take only what we need)
        int samplesToCopy = std::min(static_cast<int>(retrieved), numSamples);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* dest = buffer.getWritePointer(ch);
            const float* src = outputBuffers[static_cast<size_t>(ch)].data();

            // Copy available samples
            for (int i = 0; i < samplesToCopy; ++i)
            {
                dest[i] = src[i];
            }

            // Zero-pad if we don't have enough samples
            for (int i = samplesToCopy; i < numSamples; ++i)
            {
                dest[i] = 0.0f;
            }
        }
    }
    else
    {
        // No output available yet, output silence
        buffer.clear();
    }
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

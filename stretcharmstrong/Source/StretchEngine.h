#pragma once

#include <JuceHeader.h>
#include <rubberband/RubberBandStretcher.h>
#include <vector>
#include <memory>

class StretchEngine
{
public:
    enum class StretchType
    {
        Varispeed = 0,  // Changes pitch with speed (like tape)
        TimeStretch = 1  // Maintains pitch while changing speed
    };

    StretchEngine();
    ~StretchEngine();

    void prepare(double sampleRate, int maxBlockSize, StretchType type, float ratio);
    void process(juce::AudioBuffer<float>& buffer, float envelopeValue);

    void setStretchType(StretchType type);
    void setStretchRatio(float ratio);

    int getLatencySamples() const;

private:
    double sampleRate = 44100.0;
    int maxBlockSize = 512;
    StretchType stretchType = StretchType::TimeStretch;
    float stretchRatio = 1.0f;
    float targetStretchRatio = 1.0f;

    // Rubber Band for time-stretching (pitch-preserving)
    std::unique_ptr<RubberBand::RubberBandStretcher> rubberBand;

    // Circular buffer for varispeed
    std::vector<std::vector<float>> varispeedBuffer;
    double varispeedReadPos = 0.0;
    int varispeedWritePos = 0;
    static constexpr int varispeedBufferSize = 131072; // ~3 seconds at 44.1kHz

    // Input/output buffers for Rubber Band
    std::vector<std::vector<float>> inputBuffers;
    std::vector<std::vector<float>> outputBuffers;

    // Crossfade for smooth transitions
    float currentEnvelope = 0.0f;
    static constexpr float envelopeSmoothingCoeff = 0.995f;

    // Process methods for each stretch type
    void processVarispeed(juce::AudioBuffer<float>& buffer, float envelope);
    void processTimeStretch(juce::AudioBuffer<float>& buffer, float envelope);

    // Hermite interpolation for high-quality varispeed
    float hermiteInterpolate(const float* buffer, int bufferSize, double position);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StretchEngine)
};

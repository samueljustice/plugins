#pragma once

#include <JuceHeader.h>
#include <rubberband/RubberBandStretcher.h>
#include <vector>
#include <memory>
#include <array>

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
    void reset();

    void setStretchType(StretchType type);
    void setStretchRatio(float ratio);

    int getLatencySamples() const;

private:
    double sampleRate = 44100.0;
    int maxBlockSize = 512;
    StretchType stretchType = StretchType::TimeStretch;
    float stretchRatio = 1.0f;
    float targetStretchRatio = 1.0f;
    float smoothedStretchRatio = 1.0f;

    // Rubber Band for time-stretching (pitch-preserving)
    std::unique_ptr<RubberBand::RubberBandStretcher> rubberBand;
    bool rubberBandPrimed = false;
    int primingSamplesNeeded = 0;
    int primingSamplesFed = 0;

    // Output ring buffer for Rubber Band (per-channel positions for stereo)
    std::vector<std::vector<float>> outputRingBuffer;
    std::array<int, 2> ringWritePos = {0, 0};
    std::array<int, 2> ringReadPos = {0, 0};
    int ringBufferAvailable = 0;
    static constexpr int ringBufferSize = 65536;

    // Circular buffer for varispeed
    std::vector<std::vector<float>> varispeedBuffer;
    double varispeedReadPos = 0.0;
    int varispeedWritePos = 0;
    static constexpr int varispeedBufferSize = 131072;

    // Working buffers
    std::vector<std::vector<float>> inputBuffers;
    std::vector<std::vector<float>> retrieveBuffers;

    // Crossfade for smooth transitions
    float currentEnvelope = 0.0f;
    float previousEnvelope = 0.0f;
    static constexpr float envelopeSmoothingCoeff = 0.99f;

    // Crossfade buffer for transitions
    std::vector<std::vector<float>> crossfadeBuffer;
    bool needsCrossfade = false;
    int crossfadeSamples = 0;
    static constexpr int crossfadeLength = 256;

    // Process methods for each stretch type
    void processVarispeed(juce::AudioBuffer<float>& buffer, float envelope);
    void processTimeStretch(juce::AudioBuffer<float>& buffer, float envelope);

    // Ring buffer operations (writes both channels together)
    void writeToRingBuffer(const float* dataL, const float* dataR, int numSamples);
    void readFromRingBuffer(float* dataL, float* dataR, int numSamples, int numChannels);

    // Hermite interpolation for high-quality varispeed
    float hermiteInterpolate(const float* buffer, int bufferSize, double position);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StretchEngine)
};

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

/**
 * Mastering output stage: an "inflate" loudness/density shaper followed by a
 * look-ahead brickwall limiter.
 *
 *   in --> inflate (memoryless curve, mixed by Effect) --+--> peak detector --> gain computer
 *                                                         |                         |
 *                                                         +--> look-ahead delay --> x gain --> safety clamp --> out
 *
 * Inflate raises the body of the waveform while leaving full-scale peaks where
 * they are, so material gets denser and louder without new overs. The limiter
 * then catches whatever does go over the ceiling.
 *
 * The look-ahead delay is always in the path, so the latency is the same with
 * the limiter on or off and for every parameter value.
 *
 * No JUCE dependency, no allocation after prepare(): safe for the audio thread
 * and easy to test offline.
 */
class MasterProcessor
{
public:
    struct Parameters
    {
        float inflateEffect = 0.0f;  // 0..1  (0 = bit-transparent)
        float inflateCurve  = 0.0f;  // -1..+1 character of the inflate curve
        bool  limiterOn     = false;
        float ceilingDb     = -0.3f; // -12..0
        float releaseMs     = 100.0f;// 1..1000
    };

    void prepare(double sampleRate, int maxChannels);   // allocate here only
    void reset();
    void setParameters(const Parameters& p);            // called once per block from the audio thread
    void process(float* const* channels, int numChannels, int numSamples); // in place, any block size

    /** Constant after prepare(); independent of every parameter. */
    int getLatencySamples() const { return m_latency; }

    /** Deepest limiter gain reduction applied during the last process() call (dB, <= 0). Safe from any thread. */
    float getGainReductionDb() const { return m_gainReductionDb.load(std::memory_order_relaxed); }

    /** How often the final safety clamp has had to act since reset() (diagnostics; expected to stay 0). */
    std::uint64_t getSafetyClampCount() const { return m_safetyClampCount; }

    /** The static inflate transfer curve f(x) at full effect (for tests and for drawing the curve in a UI). */
    static float inflateTransfer(float x, float curve);

private:
    // Shape coefficients of the inflate curve (see MasterProcessor.cpp for the maths).
    struct Shape
    {
        float alpha = 0.0f; // small-signal gain is 1 + alpha
        float beta = 0.0f;  // moves the lift towards higher levels
        float mu = 0.0f;    // even (asymmetric) component
    };

    static Shape shapeForCurve(float curve);

    // Linear parameter ramp that lands exactly on its target, so Effect = 0 is bit-exact.
    struct Ramp
    {
        float current = 0.0f;
        float target = 0.0f;
        float step = 0.0f;
        int remaining = 0;

        void snap(float value);
        void setTarget(float value, int lengthSamples);
        bool isRamping() const { return remaining > 0; }

        float next()
        {
            if (remaining > 0)
                current = (--remaining == 0) ? target : current + step;

            return current;
        }
    };

    // Sliding-window minimum (monotonic deque), O(1) amortised per sample.
    struct SlidingMinimum
    {
        std::vector<float> values;
        std::vector<std::int64_t> stamps;
        int capacity = 0;
        int window = 1;
        int head = 0;
        int count = 0;
        std::int64_t now = 0;

        void prepare(int windowLength);
        void reset();
        float push(float value);
    };

    // Moving average. The running sum is re-added exactly once per lap, so it cannot drift.
    struct BoxFilter
    {
        std::vector<double> ring;
        int length = 1;
        int position = 0;
        double sum = 1.0;

        void prepare(int lengthSamples);
        void reset();
        double push(double value);
    };

    struct Channel
    {
        std::vector<float> delay; // look-ahead delay of the inflated signal
        float dcIn = 0.0f;        // AC coupling of the even (asymmetric) part
        float dcOut = 0.0f;
    };

    void renderControl(int numFrames);
    void inflate(float* data, Channel& channel, int numFrames);
    void computeGain(int numFrames);
    void applyGain(float* data, Channel& channel, int numFrames, int delayPosition);

    static constexpr int c_blockSize = 64;

    double m_sampleRate = 44100.0;
    int m_numChannels = 0;
    int m_latency = 1;

    std::vector<Channel> m_channels;
    int m_delayPosition = 0;

    // Targets and smoothing
    Parameters m_target;
    bool m_snapNextParameters = true;
    int m_rampLength = 1;
    Ramp m_effect;
    Ramp m_curve;
    Ramp m_ceilingDb;
    float m_ceilingGain = 1.0f;
    float m_limiterMix = 0.0f;
    float m_limiterMixStep = 1.0f;
    double m_releaseCoeff = 0.0;
    float m_dcCoeff = 0.0f;
    bool m_inflateActive = false;

    // Limiter gain computer
    SlidingMinimum m_minimum;
    BoxFilter m_box1, m_box2;
    double m_releaseGain = 1.0;
    std::vector<float> m_ceilingDelay; // ceiling in force when each delayed sample entered
    std::uint64_t m_safetyClampCount = 0;
    float m_blockMinGain = 1.0f;
    std::atomic<float> m_gainReductionDb { 0.0f };

    // Per-frame control data for the current block (shared by all channels)
    float m_frameEffect[c_blockSize] {};
    float m_frameAlpha[c_blockSize] {};
    float m_frameBeta[c_blockSize] {};
    float m_frameMu[c_blockSize] {};
    float m_framePeak[c_blockSize] {};
    float m_frameGain[c_blockSize] {};
    float m_frameClamp[c_blockSize] {};
};

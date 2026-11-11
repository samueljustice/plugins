#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

/**
 * Mastering output stage: an "inflate" loudness/density shaper, a drive gain
 * (Push) and a look-ahead true-peak brickwall limiter.
 *
 *   in --> inflate (memoryless curve, mixed by Effect) --> x push --+--> true-peak detector --> gain computer
 *                                                                    |                               |
 *                                                                    +--> look-ahead delay ------> x gain --> safety clamp --> out
 *
 * Inflate raises the body of the waveform while leaving full-scale peaks where
 * they are, so material gets denser and louder without new overs. It sits ahead
 * of Push, so it always works on the signal at its own level and does the same
 * thing wherever Push is. Push then drives into the limiter, which catches
 * whatever goes over the ceiling.
 *
 * The detector reads the peak of the waveform between the samples as well as at
 * them (four points per sample from a short polyphase filter), so the level a
 * converter or a sample-rate conversion reconstructs stays at the ceiling too.
 * The samples themselves are part of that reading, so no sample ever goes over.
 *
 * The release is programme dependent: a fast stage lets go quickly after a short
 * over (a hit), and a slow stage settles on reduction that lasts and lets go
 * slowly, so sustained loud material is held steadily instead of being pumped.
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
        float releaseMs     = 100.0f;// 1..1000: recovery after a short over
        float sustainReleaseMs = 0.0f; // 0 = one release for everything; otherwise (up to 5000) how slowly
                                       // reduction that has lasted lets go (releaseMs then covers short overs only)
        float pushDb        = 0.0f;  // 0..24 drive into the limiter, applied after inflate
    };

    void prepare(double sampleRate, int maxChannels);   // allocate here only
    void reset();
    void setParameters(const Parameters& p);            // called once per block from the audio thread
    void process(float* const* channels, int numChannels, int numSamples); // in place, any block size

    /** Constant after prepare(); independent of every parameter. */
    int getLatencySamples() const { return m_latency; }

    /** Deepest limiter gain reduction applied during the last process() call (dB, <= 0). Safe from any thread. */
    float getGainReductionDb() const { return m_gainReductionDb.load(std::memory_order_relaxed); }

    /** Deepest gain reduction since the last call of this (dB, <= 0), then starts again: a meter that polls
        slower than the audio blocks still sees every reduction. One reader (the meter), any thread. */
    float takeGainReductionDb() { return m_gainReductionHoldDb.exchange(0.0f, std::memory_order_relaxed); }

    /** The true-peak estimate the limiter works from, for a signal x at sample n: the largest of |x[n]| and
        three points between x[n] and x[n+1]. `window` holds c_detectorTaps samples, x[n - c_detectorTaps/2 + 1]
        first (tests). */
    static float detectPeak(const float* window);
    static constexpr int c_detectorTaps = 48;       // samples each in-between point is read from

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
        std::vector<float> delay; // look-ahead delay of the inflated, pushed signal
        float history[2 * c_detectorTaps] {}; // the detector's last samples, written twice so a window is contiguous
        float dcIn = 0.0f;        // AC coupling of the even (asymmetric) part
        float dcOut = 0.0f;
    };

    void updateReleaseCoefficients();
    void renderControl(int numFrames);
    void inflate(float* data, Channel& channel, int numFrames);
    void pushAndDetect(float* data, Channel& channel, int numFrames);
    void computeGain(int numFrames);
    void applyGain(float* data, Channel& channel, int numFrames, int delayPosition);

    static constexpr int c_blockSize = 64;

    double m_sampleRate = 44100.0;
    int m_numChannels = 0;
    int m_latency = 1;   // look-ahead plus the detector's delay: the delay of the audio path
    int m_lookahead = 1; // the gain computer's share of it

    std::vector<Channel> m_channels;
    int m_delayPosition = 0;
    int m_lookPosition = 0;
    int m_historyPosition = 0;

    // Targets and smoothing
    Parameters m_target;
    bool m_snapNextParameters = true;
    int m_rampLength = 1;
    Ramp m_effect;
    Ramp m_curve;
    Ramp m_ceilingDb;
    Ramp m_push;         // linear gain
    int m_pushRampLength = 1;
    float m_ceilingGain = 1.0f;
    float m_limiterMix = 0.0f;
    float m_limiterMixStep = 1.0f;
    double m_releaseCoeff = 0.0;
    double m_sustainAttackCoeff = 0.0;
    double m_sustainReleaseCoeff = 0.0;
    float m_dcCoeff = 0.0f;
    bool m_inflateActive = false;

    // Limiter gain computer
    SlidingMinimum m_minimum;
    BoxFilter m_box1, m_box2;
    double m_releaseGain = 1.0; // fast stage: instant attack, quick release
    double m_sustainGain = 1.0; // slow stage: settles on reduction that lasts
    double m_sustainActivity = 0.0; // 0..1: how much of the recent past needed reduction
    double m_activityUpCoeff = 0.0, m_activityDownCoeff = 0.0;
    std::vector<float> m_ceilingDelay; // ceiling in force when each delayed sample entered
    std::uint64_t m_safetyClampCount = 0;
    float m_blockMinGain = 1.0f;
    std::atomic<float> m_gainReductionDb { 0.0f };
    std::atomic<float> m_gainReductionHoldDb { 0.0f };

    // Per-frame control data for the current block (shared by all channels)
    float m_frameEffect[c_blockSize] {};
    float m_frameAlpha[c_blockSize] {};
    float m_frameBeta[c_blockSize] {};
    float m_frameMu[c_blockSize] {};
    float m_framePush[c_blockSize] {};
    float m_framePeak[c_blockSize] {};
    float m_frameGain[c_blockSize] {};
    float m_frameClamp[c_blockSize] {};
};

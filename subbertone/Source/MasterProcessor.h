#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

/**
 * Mastering output stage: an "inflate" loudness/density shaper followed by a
 * look-ahead brickwall limiter.
 *
 *   in --+--> 2x up --> curve deviation f(x) - x --> 2x down --> x Effect --+
 *        +--> delay (matches the filters) ------------------------------------+--> inflate clip --> inflated
 *
 *   inflated --+--> sample-peak detector --> hold / release / smoothing --> gain --+
 *              +--> look-ahead delay ----------------------------------------> x gain --> safety clamp --> out
 *
 * Inflate raises the body of the waveform so material gets denser and louder. Its
 * static curve keeps full-scale samples at full scale (f(+-1) = +-1), but it does
 * NOT stop overs on its own: the curve's harmonics are band-limited to 20 kHz, and a
 * fattened, band-limited waveform peaks higher than the curve suggests. With the
 * limiter off and Effect at 100 %, a mix already limited to 0 dBFS would come out at
 * about +0.2 / +0.3 / +0.5 dBFS (Curve -1 / 0 / +1; denser, clipped mixes up to about
 * +1.2 dBFS), and full-scale sines above ~8 kHz at up to +0.7 / +1.1 / +1.4 dBFS. So
 * while the limiter is not fully on, the inflate clip holds every inflated sample to the
 * larger of 0 dBFS and the input's own peak around it (the 2K - 1 samples either side;
 * K below):
 *   - material within 0 dBFS stays within 0 dBFS (like Inflator's "Clip 0 dB"),
 *   - overs that were already in the input pass as they were: the clip never holds a
 *     sample below its own input level, and Inflate cannot raise their peak,
 *   - it only acts on what Inflate adds, so nothing changes at Effect 0 (still bit-exact)
 *     and the clipping grows continuously from zero with Effect: there is no step when
 *     Effect leaves or returns to 0.
 * The limiter, when on, handles all of this cleanly instead. NaN and +-inf never reach
 * the output, limiter on or off (they become silence; with the limiter on, +-inf becomes
 * the ceiling).
 *
 * Only the curve's deviation from the input is oversampled: it holds all the
 * harmonics (and so all the aliasing), while the dry path is an exact delay, which is
 * what makes Effect = 0 bit-transparent.
 *
 * The oversampling filters and the look-ahead are always in the path, so the
 * latency is the same with the limiter on or off and for every parameter value.
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
    void process(float* const* channels, int numChannels, int numSamples); // in place, any block size;
                                                                           // channels past maxChannels are left untouched

    /** Constant after prepare(); independent of every parameter (oversampling filters + look-ahead). */
    int getLatencySamples() const { return m_latency; }

    /** Limiter gain reduction for a meter (dB, <= 0), with peak-meter ballistics run at audio rate:
        instant attack, the deepest reduction is held for 150 ms, then it falls back at 20 dB/s
        (never above the reduction actually being applied). A UI that polls this at 20 Hz sees every
        transient at full depth for host blocks up to 4096 samples at 44.1 kHz, so it needs no
        ballistics of its own. Updated at the end of each process() call; safe from any thread. */
    float getGainReductionDb() const { return m_gainReductionDb.load(std::memory_order_relaxed); }

    /** Limiter look-ahead (and attack) length in samples; part of getLatencySamples(). */
    int getLookaheadSamples() const { return m_lookahead; }

    /** How often the final safety clamp has acted since reset(): the limiter's ceiling clamp, and any NaN or
        +-inf sample turned into silence (diagnostics; expected to stay 0 for finite input). */
    std::uint64_t getSafetyClampCount() const { return m_safetyClampCount.load(std::memory_order_relaxed); }

    /** How often the inflate clip (limiter not fully on: inflated samples held to max(0 dBFS, the input's
        own nearby peak)) has acted since reset() (diagnostics). */
    std::uint64_t getInflateClipCount() const { return m_inflateClipCount.load(std::memory_order_relaxed); }

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

    // Critically damped two-pole glide. Unlike a linear ramp it has no corners (its slope is
    // continuous) for jumps and for block-by-block automation alike, and it never overshoots.
    // Double precision so it can actually arrive; it then lands exactly (Effect 0 is bit-exact).
    struct Glide
    {
        double fast = 0.0;
        double value = 0.0;
        double target = 0.0;
        double coeff = 1.0;
        bool moving = false;

        void snap(double v);
        void setTarget(double v);

        double next()
        {
            if (moving)
            {
                fast += coeff * (target - fast);
                value += coeff * (fast - value);

                if (std::abs(target - value) < 1.0e-9 && std::abs(target - fast) < 1.0e-9)
                    snap(target);
            }

            return value;
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
        // Oversampling histories. The two FIR windows are written twice (at i and i + taps)
        // so the most recent `taps` samples are always contiguous.
        std::vector<float> input;     // base-rate input; its oldest sample is also the delayed dry signal
        std::vector<float> shaped;    // even-phase (2x) samples of the curve deviation
        std::vector<float> shapedOdd; // odd-phase samples, delayed to meet the decimator's centre tap
        int inputPosition = 0;
        int shapedPosition = 0;
        int oddPosition = 0;
        float dcIn = 0.0f;            // AC coupling of the even (asymmetric) part, at 2x
        float dcOut = 0.0f;
        std::vector<float> pastPeak;  // |input| of the 2K - 1 samples before the dry one (inflate clip window)
        int pastPosition = 0;

        std::vector<float> delay;     // limiter look-ahead
    };

    void designHalfband();
    void renderControl(int numFrames);
    void inflate(float* data, Channel& channel, int numFrames);
    float computeGain(int numFrames); // returns the smallest gain it applies in the chunk
    void applyGain(float* data, Channel& channel, int numFrames, int delayPosition);
    void updateMeter(float minGain, int numFrames);

    static constexpr int c_blockSize = 64;

    double m_sampleRate = 44100.0;
    int m_numChannels = 0;
    int m_lookahead = 1;
    int m_latency = 1;

    // Halfband FIR (2x up and down share it): only the 2K off-centre taps are stored,
    // the centre tap is 0.5 and every other tap is zero.
    int m_halfbandHalf = 2; // K
    std::vector<float> m_upTaps;
    std::vector<float> m_downTaps;

    std::vector<Channel> m_channels;
    int m_delayPosition = 0;

    // Targets and smoothing
    Parameters m_target;
    bool m_snapNextParameters = true;
    Glide m_effect;
    Glide m_curve;
    Glide m_ceilingDb;
    float m_ceilingGain = 1.0f;
    float m_limiterFade = 0.0f;     // 0 = off .. 1 = on, linear in time; shaped by smootherstep
    float m_limiterFadeStep = 1.0f;
    double m_releaseCoeff = 0.0;
    float m_dcCoeff = 0.0f;
    bool m_inflateActive = false;
    int m_inflateWarmup = 0;        // frames Effect is held at 0 while the filters refill after bypass

    // Limiter gain computer
    SlidingMinimum m_minimum;
    BoxFilter m_box1, m_box2;
    double m_releaseGain = 1.0;
    std::vector<float> m_ceilingDelay; // ceiling in force when each delayed sample entered
    std::atomic<std::uint64_t> m_safetyClampCount { 0 };
    std::atomic<std::uint64_t> m_inflateClipCount { 0 };

    // Gain-reduction meter (audio-rate ballistics, see getGainReductionDb())
    float m_meterDb = 0.0f;
    int m_meterHoldLeft = 0;        // samples until the held reading may start to fall
    int m_meterHoldSamples = 0;
    float m_meterFallPerSample = 0.0f; // dB
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

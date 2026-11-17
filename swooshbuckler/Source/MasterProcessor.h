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
 * them, so the level a converter or a sample-rate conversion reconstructs stays
 * at the ceiling too: four points per sample, each from up to 128 samples with
 * weights fitted to the band sound fills, and where a point stands above its
 * neighbours, the crest of the curve through the three (a top that falls
 * between two points is found as well). Measured with a sixteen-point,
 * 256-sample reference the true peak stays within 0.1 dB of the ceiling on
 * tones, hits and noise, and within 0.2 dB on the one signal built to defeat any
 * detector of finite length: white noise with full power right up to half the
 * sample rate (at 176.4 and 192 kHz, where the reader is shorter, 0.65 dB: full
 * power above 80 kHz). The samples themselves are part of the reading, so no
 * sample ever goes over.
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

    /** Allocate here only. holdSeconds: how long each reduction is kept before it starts to let go (0: not at
        all, as a limiter for full-range sound wants). A stage that only ever carries low frequencies (the LFE)
        holds for longer than half a cycle of its lowest note, so its gain does not move within a note and adds
        no harmonics to it. */
    void prepare(double sampleRate, int maxChannels, double holdSeconds = 0.0);
    void reset();
    void setParameters(const Parameters& p);            // called once per block from the audio thread
    void process(float* const* channels, int numChannels, int numSamples); // in place, any block size

    /** SIDES: for a room of speakers. Without them (the default after prepare(): Mono, Stereo, the LFE) every
        channel is shaped by its own level and the limiter reads each channel's own peak.

        In a room one sound is shared between several speakers, so each speaker carries less of it than a Stereo
        channel would, and a shaper or limiter that looked at each speaker alone would do less the more speakers
        the sound is spread over. With sides, the speakers of each side of the room are treated as the ONE Stereo
        channel they stand in for. The peak the limiter reads is the side's speakers summed by power, sample by
        sample (for one sound shared between them that is exactly the sound's own waveform, whatever the number
        of speakers). The level that drives the Inflate curve in a speaker is the same sum taken over the speakers
        that carry the same sound as it does (how alike two speakers' signals have been over the last 25 ms decides:
        see inflateSides): the whole sound for a pass shared between speakers, the speaker's own level for a sound
        that is in it alone, so sounds that have nothing to do with each other do not shape each other. So a pass
        gets the Inflate, the Push and the limiting it would get in Stereo, at every width and Spread. A centre
        speaker (side 0) counts half to each side, as a centre folds into L and R, and takes the mean of the two
        sides' shaping; the limiter also reads its own peak. Each channel's peak is never above its side's, so the
        Ceiling holds on every channel.

        side[c]: below 0 left, above 0 right, 0 centre; numChannels of them (channels past that are left). Pass
        nullptr to go back to every channel on its own. Allocation-free; not while process() runs. */
    void setSides(const float* side, int numChannels);
    bool hasSides() const { return m_sided; }

    /** Constant after prepare(); independent of every parameter. */
    int getLatencySamples() const { return m_latency; }

    /** Deepest limiter gain reduction applied during the last process() call (dB, <= 0). Safe from any thread. */
    float getGainReductionDb() const { return m_gainReductionDb.load(std::memory_order_relaxed); }

    /** Deepest gain reduction since the last call of this (dB, <= 0), then starts again: a meter that polls
        slower than the audio blocks still sees every reduction. One reader (the meter), any thread. */
    float takeGainReductionDb() { return m_gainReductionHoldDb.exchange(0.0f, std::memory_order_relaxed); }

    /** The true-peak estimate the limiter works from, for a signal x at sample n: the top of the waveform from
        x[n] up to x[n+1], read from x[n], three points between the two and the crest of the curve through them
        (see refinePeak). `window` holds taps + 1 samples, x[n - taps/2] first (tests). */
    static float detectPeak(const float* window, int taps = c_detectorTaps);
    /** The top of the waveform around `point`, given the readings a quarter of a sample before and after it:
        the point's own size, or, where it stands above both neighbours, the crest of the curve through the
        three (never below the point). */
    static float refinePeak(float before, float point, float after);
    /** The largest top around four readings in a row (a sample and the three points after it), given the
        reading before them and the sample after them. */
    static float peakOf(float before, const float points[4], float after);
    /** The four readings one by one, signed: x[n] and the three points after it. `window` holds taps samples,
        x[n - taps/2 + 1] first. */
    static void detectPoints(const float* window, float points[4], int taps = c_detectorTaps);
    /** How many samples each in-between point is read from: 128 up to 96 kHz, where sound reaches right up to
        half the sample rate, 64 at 176.4 and 192 kHz. */
    static int detectorTapsFor(double sampleRate);
    static constexpr int c_detectorTaps = 128;      // the most (the detector up to 96 kHz)

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

    static constexpr int c_blockSize = 64;
    static constexpr int c_alikeGrid = 16; // samples between two readings of which speakers carry the same sound

    struct Channel
    {
        std::vector<float> delay; // look-ahead delay of the inflated, pushed signal
        float history[2 * c_detectorTaps] {}; // the detector's last samples (m_taps of them), written twice so a window is contiguous
        float dcIn = 0.0f;        // AC coupling of the even (asymmetric) part
        float dcOut = 0.0f;
        float lastPoint = 0.0f;   // the detector's reading a quarter of a sample before the next sample

        // With sides (see inflateSides): this block's whole sound the channel is part of, per side it counts to,
        // as a power and signed; this run's squares; the channel's running power.
        float whole[2][c_blockSize] {};
        float signedWhole[2][c_blockSize] {};
        float square[c_alikeGrid] {};
        float signedSquare[c_alikeGrid] {};
        float power = 0.0f;    // its running power (a 25 ms average, taken in on the grid)
        float powerSum = 0.0f; // ... and its squares added up since the last grid point
        float left = 0.0f;        // with sides: how much of this channel counts to the left side and the right
        float right = 0.0f;       // (1 and 0, 0 and 1, or a half each for a centre)
    };

    void updateReleaseCoefficients();
    void renderControl(int numFrames);
    void inflate(float* data, Channel& channel, int numFrames);
    void readChannel(float* data, Channel& channel, int numFrames, float (*points)[4], float* next);
    void pushAndDetect(float* data, Channel& channel, int numFrames);
    void computeGain(int numFrames);
    void applyGain(float* data, Channel& channel, int numFrames, int delayPosition);
    void inflateSides(float* const* channels, int numChannels, int offset, int numFrames);
    void pushAndDetectSides(float* data, Channel& channel, int numFrames);
    void finishSidePeaks(int numFrames);

    // Two speakers that count to the same side, and how alike what they carry has been (see inflateSides).
    struct Pair
    {
        int a = 0, b = 0;
        float cross = 0.0f;  // the running average of a * b (taken in on the grid)
        float crossSum = 0.0f; // ... and a * b added up since the last grid point
        float weight = 1.0f; // 1 the same sound, 0 nothing to do with each other, below 0 the same sound inverted
        float target = 1.0f; // where the weight is gliding to
        float step = 0.0f;
    };

    void resetAlike();

    double m_sampleRate = 44100.0;
    int m_numChannels = 0;
    int m_latency = 1;   // look-ahead plus the detector's delay: the delay of the audio path
    int m_lookahead = 1; // the gain computer's share of it
    int m_taps = c_detectorTaps;      // the detector's length at this sample rate
    const void* m_detector = nullptr; // its weights (a table shared by every stage at this length)

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

    // Sides (see setSides): per frame and side, the four readings' power sums for the limiter.
    bool m_sided = false;
    float m_frameSidePower[2][c_blockSize][5] {}; // (the fifth: the next sample, for the top after the last reading)
    float m_sideLastPoint[2] {};
    std::vector<Pair> m_pairs;
    float m_alikeCoeff = 0.0f;
    int m_alikeCount = 0;
};

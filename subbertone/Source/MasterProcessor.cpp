#include "MasterProcessor.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace
{
    constexpr double c_pi = 3.141592653589793;
    constexpr double c_twoPi = 6.283185307179586;

    constexpr double c_lookaheadSeconds   = 0.0015;
    constexpr double c_glideSeconds       = 0.008; // per pole: effect, curve, ceiling are ~90% there in 30 ms
    constexpr double c_limiterFadeSeconds = 0.030; // limiter on/off crossfade (exact length)
    constexpr double c_dcBlockHz          = 5.0;   // AC coupling of the even part of the curve

    // Meter ballistics. The hold has to outlast one UI poll plus one host block, or a
    // polling UI misses transients: 150 ms covers a 20 Hz UI with blocks up to 4096 at 44.1 kHz.
    constexpr double c_meterHoldSeconds     = 0.150;
    constexpr double c_meterFallDbPerSecond = 20.0;

    // Host values outside this are bugs; they must not reach the filter design or buffer sizes.
    constexpr double c_minSampleRate = 1000.0;
    constexpr double c_maxSampleRate = 1536000.0;

    // Halfband stopband design target (Kaiser estimate; the filters built from it measure
    // 57-59 dB). Only the curve's deviation goes through the filters, and the part that
    // would alias is already 25-50 dB under the signal, so what is left of it ends up
    // around -87..-103 dBFS.
    constexpr double c_halfbandStopbandDb = 60.0;
    constexpr int c_maxHalfbandHalf = 32;

    // The gain computer aims 2^-20 (~0.00001 dB) under the ceiling, so float
    // rounding in the gain path can never carry a sample over it.
    constexpr float c_detectionMargin = 1.0f - 1.0f / 1048576.0f;

    // NaN/inf in the detector are treated as a huge peak so they cannot poison the gain state.
    constexpr float c_maxDetectPeak = 1.0e20f;
    constexpr float c_noClamp = std::numeric_limits<float>::max(); // "no clamp": only NaN and inf exceed it

    float sanitise(float value, float lo, float hi)
    {
        // Written so that NaN falls to `lo` instead of propagating.
        return value >= lo ? (value <= hi ? value : hi) : lo;
    }

    float dbToGain(float db)
    {
        return std::exp(db * 0.115129255f); // ln(10) / 20
    }

    // Exact comparison on purpose (unchanged host values); std::equal_to keeps -Wfloat-equal quiet.
    bool exactlyEqual(float a, float b)
    {
        return std::equal_to<float>()(a, b);
    }

    // n must be a multiple of 4. Four partial sums break the dependency chain (and allow
    // SIMD) without needing -ffast-math.
    float dot(const float* a, const float* b, int n)
    {
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;

        for (int j = 0; j < n; j += 4)
        {
            s0 += a[j] * b[j];
            s1 += a[j + 1] * b[j + 1];
            s2 += a[j + 2] * b[j + 2];
            s3 += a[j + 3] * b[j + 3];
        }

        return (s0 + s1) + (s2 + s3);
    }

    double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        const double q = 0.25 * x * x;

        for (int k = 1; k < 64; ++k)
        {
            term *= q / (static_cast<double>(k) * static_cast<double>(k));
            sum += term;

            if (term < 1.0e-12 * sum)
                break;
        }

        return sum;
    }
}

//==============================================================================
// Inflate curve (designed for this plugin):
//
//   f(x) = x + w(x) * ( x * (alpha + beta * x^2) + mu * x^2 ),   w(x) = (1 - x^2)^2,   |x| < 1
//   f(x) = x                                                                            |x| >= 1
//
// - w(+-1) = 0 and w'(+-1) = 0, so f(+-1) = +-1 and f'(+-1) = 1: full-scale peaks stay
//   where they are and the curve joins the identity C1-smoothly. Overs pass with slope 1
//   (nothing is clipped here; the limiter deals with peaks).
// - f'(0) = 1 + alpha: quiet material is lifted by up to 20 log10(1 + alpha) dB.
// - Because f(1) = 1 the lift must be paid back higher up: the slope dips below 1 in the
//   upper range, which squeezes loud passages towards full scale (the "density"), and
//   climbs back to exactly 1 at full scale.
// - The odd part expands to (1 + alpha) x + (beta - 2 alpha) x^3 + (alpha - 2 beta) x^5 + beta x^7,
//   so a sine only gains odd harmonics up to the 7th. beta moves the lift towards higher
//   levels and trades 5th harmonic for 7th.
// - mu x^2 w(x) is even: it fattens positive half-waves, adding 2nd (and 4th, 6th) harmonic.
//   It is zero with zero slope at 0 and +-1, so neither the small-signal gain nor the
//   full-scale points move. Its DC is removed by a 5 Hz AC coupling, as the coupling
//   capacitor after an asymmetric analogue stage would. (Once the DC is gone a full-scale
//   low-frequency sine at Curve +1 peaks at 1 + mu/16, +0.065 dB.)
//
// f(+-1) = +-1 holds per sample for the full-band curve only. The processed deviation is
// band-limited (below), and that fills in the notch the deviation has at the peaks, so the
// output CAN go over full scale. Above ~fs/5 a sine's deviation keeps only its fundamental,
// alpha/8 + 3 beta/64 at full scale (sin^3, sin^5, sin^7 hold 3/4, 5/8, 35/64 of it), i.e.
// +0.77 dB at Curve 0; the 2nd harmonic and the 5th's alias add to that near 15-20 kHz.
// Measured worst cases are in MasterProcessor.h. The curve itself never clips: with the limiter
// on, the limiter after this stage is the peak control; with it off, inflate() holds each output
// sample to max(0 dBFS, the input's own peak around it), which only ever acts on what Inflate added.
//
// f is linear in (alpha, beta, mu), so f' is too: any blend of monotonic anchor curves is
// itself monotonic. Curve morphs piecewise-linearly between three anchors whose minimum
// slope over [-1, 1] was checked numerically (1 + alpha at 0, minimum in the upper range):
//
//   curve -1  gentle      alpha 0.45  beta 0     mu 0     +3.2 dB lift, min slope 0.64, odd only
//   curve  0  neutral     alpha 0.70  beta 0.10  mu 0     +4.6 dB lift, min slope 0.41, odd only
//   curve +1  aggressive  alpha 0.90  beta 0.20  mu 0.12  +5.6 dB lift, min slope 0.16, + 2nd harmonic
//
// Output = x + effect * (f(x) - x). The deviation f(x) - x carries every harmonic up to
// the 7th, which folds badly at 44.1/48 kHz (measured at Effect 1 on noise peaking at
// -1 dBFS: aliasing -40..-54 dBFS, 27-40 dB under the signal), so the deviation alone runs
// at 2x through linear-phase halfbands, which brings it to -86..-103 dBFS. The dry path is a
// plain delay matched to the filters, so Effect = 0 stays bit-exact and the paths stay aligned.
namespace
{
    struct Anchor
    {
        float alpha, beta, mu;
    };

    constexpr Anchor c_gentle     { 0.45f, 0.00f, 0.00f };
    constexpr Anchor c_neutral    { 0.70f, 0.10f, 0.00f };
    constexpr Anchor c_aggressive { 0.90f, 0.20f, 0.12f };

    struct Deviation
    {
        float odd, even;
    };

    // f(x) - x, split so the even part can be AC-coupled on its own.
    Deviation curveDeviation(float x, float alpha, float beta, float mu)
    {
        const float x2 = x * x;

        // Beyond full scale f(x) = x. NaN/inf land here too, so they never reach the filter states.
        if (!(x2 < 1.0f))
            return { 0.0f, 0.0f };

        const float w = (1.0f - x2) * (1.0f - x2);
        return { w * x * (alpha + beta * x2), w * mu * x2 };
    }
}

MasterProcessor::Shape MasterProcessor::shapeForCurve(float curve)
{
    const float c = sanitise(curve, -1.0f, 1.0f);
    const Anchor& to = c < 0.0f ? c_gentle : c_aggressive;
    const float t = std::abs(c);

    Shape s;
    s.alpha = c_neutral.alpha + t * (to.alpha - c_neutral.alpha);
    s.beta  = c_neutral.beta  + t * (to.beta  - c_neutral.beta);
    s.mu    = c_neutral.mu    + t * (to.mu    - c_neutral.mu);
    return s;
}

float MasterProcessor::inflateTransfer(float x, float curve)
{
    const float x2 = x * x;

    if (!(x2 < 1.0f))
        return x;

    const Shape s = shapeForCurve(curve);
    const float w = (1.0f - x2) * (1.0f - x2);
    return x + w * (x * (s.alpha + s.beta * x2) + s.mu * x2);
}

//==============================================================================
void MasterProcessor::Glide::snap(double v)
{
    fast = value = target = v;
    moving = false;
}

void MasterProcessor::Glide::setTarget(double v)
{
    // A new target mid-glide carries on from the current position and slope: still no corner.
    if (!std::equal_to<double>()(v, target))
    {
        target = v;
        moving = true;
    }
}

//==============================================================================
void MasterProcessor::SlidingMinimum::prepare(int windowLength)
{
    window = std::max(1, windowLength);
    capacity = window + 1;
    values.assign(static_cast<size_t>(capacity), 1.0f);
    stamps.assign(static_cast<size_t>(capacity), 0);
    reset();
}

void MasterProcessor::SlidingMinimum::reset()
{
    head = 0;
    count = 0;
    now = 0;
}

float MasterProcessor::SlidingMinimum::push(float value)
{
    // The front leaves once it is older than the window [now - window + 1, now].
    if (count > 0 && stamps[static_cast<size_t>(head)] <= now - window)
    {
        if (++head == capacity)
            head = 0;

        --count;
    }

    // Anything at the back that is not below the new value can never be the minimum again,
    // so the deque stays sorted and the front is always the window minimum.
    while (count > 0)
    {
        int back = head + count - 1;
        if (back >= capacity)
            back -= capacity;

        if (values[static_cast<size_t>(back)] < value)
            break;

        --count;
    }

    int slot = head + count;
    if (slot >= capacity)
        slot -= capacity;

    values[static_cast<size_t>(slot)] = value;
    stamps[static_cast<size_t>(slot)] = now;
    ++count;
    ++now;

    return values[static_cast<size_t>(head)];
}

//==============================================================================
void MasterProcessor::BoxFilter::prepare(int lengthSamples)
{
    length = std::max(1, lengthSamples);
    ring.assign(static_cast<size_t>(length), 1.0);
    reset();
}

void MasterProcessor::BoxFilter::reset()
{
    std::fill(ring.begin(), ring.end(), 1.0);
    position = 0;
    sum = static_cast<double>(length);
}

double MasterProcessor::BoxFilter::push(double value)
{
    double& slot = ring[static_cast<size_t>(position)];
    sum += value - slot;
    slot = value;

    if (++position == length)
    {
        position = 0;

        // Re-add exactly once per lap: O(1) amortised, and rounding can never accumulate.
        sum = 0.0;
        for (const double v : ring)
            sum += v;
    }

    return sum / static_cast<double>(length);
}

//==============================================================================
void MasterProcessor::designHalfband()
{
    // Kaiser-windowed sinc halfband for 2x, flat to 20 kHz (0.4535 fs below 44.1 kHz). It is
    // symmetric about the base-rate Nyquist, so the stopband starts at fs - passband: products
    // in between alias to above the passband edge, out of the way. A lower edge at 44.1 kHz
    // would save 12 samples of latency but leaves images of 19-20 kHz content in the
    // transition band, where they intermodulate in the curve (measured 10-18 dB more junk).
    const double passband = std::min(20000.0, 0.4535 * m_sampleRate);
    const double transition = c_twoPi * (m_sampleRate - 2.0 * passband) / (2.0 * m_sampleRate);
    const double minTaps = (c_halfbandStopbandDb - 7.95) / (2.285 * transition) + 1.0;

    // 4K - 1 taps; K even so the 2K-tap dot products run in fours.
    int half = static_cast<int>(std::ceil((minTaps + 1.0) / 4.0));
    half = std::clamp(half + (half & 1), 2, c_maxHalfbandHalf);
    m_halfbandHalf = half;

    const int centre = 2 * half - 1;
    const double beta = 0.1102 * (c_halfbandStopbandDb - 8.7);
    const double windowNorm = besselI0(beta);

    // Nonzero off-centre taps sit at even indices 2m (odd distance from the centre).
    std::vector<double> taps(static_cast<size_t>(2 * half));
    double sum = 0.0;

    for (int m = 0; m < 2 * half; ++m)
    {
        const double t = static_cast<double>(2 * m - centre);
        const double ratio = t / static_cast<double>(centre);
        const double window = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) / windowNorm;
        const double tap = std::sin(0.5 * c_pi * t) / (c_pi * t) * window;
        taps[static_cast<size_t>(m)] = tap;
        sum += tap;
    }

    // Exactly 0.5 per polyphase branch, so neither 2x phase has a DC error (no fs/2 ripple).
    m_upTaps.resize(taps.size());
    m_downTaps.resize(taps.size());

    for (size_t m = 0; m < taps.size(); ++m)
    {
        const double tap = taps[m] * 0.5 / sum;
        m_downTaps[m] = static_cast<float>(tap);
        m_upTaps[m] = static_cast<float>(2.0 * tap); // zero-stuffing halves the level
    }
}

void MasterProcessor::prepare(double sampleRate, int maxChannels)
{
    // NaN, inf, 0 and negative rates fall back to 44.1 kHz; absurd rates are clamped.
    m_sampleRate = (std::isfinite(sampleRate) && sampleRate > 0.0) ? std::clamp(sampleRate, c_minSampleRate, c_maxSampleRate) : 44100.0;
    m_numChannels = std::max(1, maxChannels);

    designHalfband();
    const int taps = 2 * m_halfbandHalf;

    m_lookahead = std::max(1, static_cast<int>(std::lround(c_lookaheadSeconds * m_sampleRate)));

    // Up + down halfband (2 x (2K - 1) samples at 2x) is 2K - 1 at the base rate, then the look-ahead.
    m_latency = (taps - 1) + m_lookahead;

    m_channels.assign(static_cast<size_t>(m_numChannels), Channel {});

    for (auto& channel : m_channels)
    {
        channel.input.assign(static_cast<size_t>(2 * taps), 0.0f);
        channel.shaped.assign(static_cast<size_t>(2 * taps), 0.0f);
        channel.shapedOdd.assign(static_cast<size_t>(m_halfbandHalf), 0.0f);
        channel.pastPeak.assign(static_cast<size_t>(taps - 1), 0.0f);
        channel.delay.assign(static_cast<size_t>(m_lookahead), 0.0f);
    }

    m_ceilingDelay.assign(static_cast<size_t>(m_lookahead), 1.0f);

    // No-overs guarantee: the gain leaving the smoothing at time n is a weighted average
    // (positive weights summing to 1) of held gains from n - lookahead .. n, and each of
    // those holds the minimum target over a window that contains sample n - lookahead, the
    // one being output. So the applied gain is never above what that sample needs.
    // Two cascaded moving averages (lengths summing to lookahead + 2, span lookahead + 1)
    // give an S-shaped attack instead of a linear ramp with corners.
    m_minimum.prepare(m_lookahead + 1);
    const int boxTotal = m_lookahead + 2;
    m_box1.prepare(boxTotal / 2);
    m_box2.prepare(boxTotal - boxTotal / 2);

    const double glideCoeff = 1.0 - std::exp(-1.0 / (c_glideSeconds * m_sampleRate));
    m_effect.coeff = m_curve.coeff = m_ceilingDb.coeff = glideCoeff;
    m_limiterFadeStep = static_cast<float>(1.0 / std::max(1.0, c_limiterFadeSeconds * m_sampleRate));
    m_dcCoeff = static_cast<float>(std::exp(-c_twoPi * c_dcBlockHz / (2.0 * m_sampleRate))); // runs at 2x
    m_releaseCoeff = 1.0 - std::exp(-1.0 / (static_cast<double>(m_target.releaseMs) * 0.001 * m_sampleRate));
    m_meterHoldSamples = static_cast<int>(std::lround(c_meterHoldSeconds * m_sampleRate));
    m_meterFallPerSample = static_cast<float>(c_meterFallDbPerSecond / m_sampleRate);

    reset();
}

void MasterProcessor::reset()
{
    for (auto& channel : m_channels)
    {
        std::fill(channel.input.begin(), channel.input.end(), 0.0f);
        std::fill(channel.shaped.begin(), channel.shaped.end(), 0.0f);
        std::fill(channel.shapedOdd.begin(), channel.shapedOdd.end(), 0.0f);
        std::fill(channel.pastPeak.begin(), channel.pastPeak.end(), 0.0f);
        std::fill(channel.delay.begin(), channel.delay.end(), 0.0f);
        channel.inputPosition = channel.shapedPosition = channel.oddPosition = channel.pastPosition = 0;
        channel.dcIn = channel.dcOut = 0.0f;
    }

    m_delayPosition = 0;

    m_effect.snap(static_cast<double>(m_target.inflateEffect));
    m_curve.snap(static_cast<double>(m_target.inflateCurve));
    m_ceilingDb.snap(static_cast<double>(m_target.ceilingDb));
    m_ceilingGain = dbToGain(m_target.ceilingDb);
    std::fill(m_ceilingDelay.begin(), m_ceilingDelay.end(), m_ceilingGain);
    m_limiterFade = m_target.limiterOn ? 1.0f : 0.0f;
    // Every history is zero after a reset, which already matches silence before the start,
    // so the curve can start fully active: no bypass warm-up (and no start-up step).
    m_inflateActive = m_target.inflateEffect > 0.0f;
    m_inflateWarmup = 0;

    m_minimum.reset();
    m_box1.reset();
    m_box2.reset();
    m_releaseGain = 1.0;

    m_safetyClampCount.store(0, std::memory_order_relaxed);
    m_inflateClipCount.store(0, std::memory_order_relaxed);
    m_meterDb = 0.0f;
    m_meterHoldLeft = 0;
    m_gainReductionDb.store(0.0f, std::memory_order_relaxed);

    // The host's first parameters after a reset are the session state, not a change: jump to them.
    m_snapNextParameters = true;
}

void MasterProcessor::setParameters(const Parameters& p)
{
    Parameters t;
    t.inflateEffect = sanitise(p.inflateEffect, 0.0f, 1.0f);
    t.inflateCurve = sanitise(p.inflateCurve, -1.0f, 1.0f);
    t.limiterOn = p.limiterOn;
    t.ceilingDb = sanitise(p.ceilingDb, -12.0f, 0.0f);
    t.releaseMs = sanitise(p.releaseMs, 1.0f, 1000.0f);

    if (m_snapNextParameters)
    {
        m_snapNextParameters = false;
        m_effect.snap(static_cast<double>(t.inflateEffect));
        m_curve.snap(static_cast<double>(t.inflateCurve));
        m_ceilingDb.snap(static_cast<double>(t.ceilingDb));
        m_ceilingGain = dbToGain(t.ceilingDb);
        std::fill(m_ceilingDelay.begin(), m_ceilingDelay.end(), m_ceilingGain);
        m_limiterFade = t.limiterOn ? 1.0f : 0.0f;
        // Only runs before the first process() after a reset, so the histories are still zero.
        m_inflateActive = t.inflateEffect > 0.0f;
        m_inflateWarmup = 0;
    }
    else
    {
        m_effect.setTarget(static_cast<double>(t.inflateEffect));
        m_curve.setTarget(static_cast<double>(t.inflateCurve));
        m_ceilingDb.setTarget(static_cast<double>(t.ceilingDb));
    }

    // The release rate can change abruptly: the gain itself stays continuous.
    if (!exactlyEqual(t.releaseMs, m_target.releaseMs))
        m_releaseCoeff = 1.0 - std::exp(-1.0 / (static_cast<double>(t.releaseMs) * 0.001 * m_sampleRate));

    m_target = t;
}

//==============================================================================
void MasterProcessor::process(float* const* channels, int numChannels, int numSamples)
{
    const int activeChannels = std::min(numChannels, m_numChannels);

    if (channels == nullptr || activeChannels <= 0 || numSamples <= 0)
        return;

    for (int offset = 0; offset < numSamples; offset += c_blockSize)
    {
        const int numFrames = std::min(c_blockSize, numSamples - offset);

        // The curve is skipped (output is the delayed input, bit for bit) while Effect rests at 0.
        const bool inflateActive = m_effect.value > 0.0 || m_effect.target > 0.0;

        if (inflateActive && !m_inflateActive)
        {
            // Coming back from bypass the oversampled path restarts from silence. Effect is held
            // at exactly 0 until the filters have refilled, so their start-up is never heard.
            for (auto& channel : m_channels)
            {
                std::fill(channel.shaped.begin(), channel.shaped.end(), 0.0f);
                std::fill(channel.shapedOdd.begin(), channel.shapedOdd.end(), 0.0f);
                std::fill(channel.pastPeak.begin(), channel.pastPeak.end(), 0.0f);
                channel.dcIn = channel.dcOut = 0.0f;
            }

            m_inflateWarmup = 4 * m_halfbandHalf;
        }

        m_inflateActive = inflateActive;

        if (m_inflateActive)
            renderControl(numFrames);
        else
            m_curve.snap(m_curve.target); // inaudible while bypassed, so no need to glide

        std::fill_n(m_framePeak, numFrames, 0.0f);

        for (int ch = 0; ch < activeChannels; ++ch)
            inflate(channels[ch] + offset, m_channels[static_cast<size_t>(ch)], numFrames);

        updateMeter(computeGain(numFrames), numFrames);

        for (int ch = 0; ch < activeChannels; ++ch)
            applyGain(channels[ch] + offset, m_channels[static_cast<size_t>(ch)], numFrames, m_delayPosition);

        m_delayPosition = (m_delayPosition + numFrames) % m_lookahead;
    }

    m_gainReductionDb.store(m_meterDb, std::memory_order_relaxed);

    // From here on parameter changes are changes, even if setParameters() was skipped so far.
    m_snapNextParameters = false;
}

void MasterProcessor::renderControl(int numFrames)
{
    if (m_inflateWarmup == 0 && !m_effect.moving && !m_curve.moving)
    {
        const Shape s = shapeForCurve(static_cast<float>(m_curve.value));
        std::fill_n(m_frameEffect, numFrames, static_cast<float>(m_effect.value));
        std::fill_n(m_frameAlpha, numFrames, s.alpha);
        std::fill_n(m_frameBeta, numFrames, s.beta);
        std::fill_n(m_frameMu, numFrames, s.mu);
        return;
    }

    for (int i = 0; i < numFrames; ++i)
    {
        if (m_inflateWarmup > 0)
        {
            --m_inflateWarmup;
            m_frameEffect[i] = 0.0f;
        }
        else
        {
            m_frameEffect[i] = static_cast<float>(m_effect.next());
        }

        const Shape s = shapeForCurve(static_cast<float>(m_curve.next()));
        m_frameAlpha[i] = s.alpha;
        m_frameBeta[i] = s.beta;
        m_frameMu[i] = s.mu;
    }
}

void MasterProcessor::inflate(float* data, Channel& channel, int numFrames)
{
    const int taps = 2 * m_halfbandHalf;
    float* const input = channel.input.data();
    int inputPosition = channel.inputPosition;

    if (!m_inflateActive)
    {
        // Bypass still runs the input history: it is the delay that keeps the latency constant.
        for (int i = 0; i < numFrames; ++i)
        {
            input[inputPosition] = input[inputPosition + taps] = data[i];
            const float dry = input[inputPosition + 1];

            if (++inputPosition == taps)
                inputPosition = 0;

            data[i] = dry;
            m_framePeak[i] = std::max(m_framePeak[i], std::abs(dry));
        }

        channel.inputPosition = inputPosition;
        return;
    }

    float* const shaped = channel.shaped.data();
    float* const shapedOdd = channel.shapedOdd.data();
    int shapedPosition = channel.shapedPosition;
    int oddPosition = channel.oddPosition;
    float dcIn = channel.dcIn;
    float dcOut = channel.dcOut;
    int pastPosition = channel.pastPosition;

    const int half = m_halfbandHalf;
    const float* const upTaps = m_upTaps.data();
    const float* const downTaps = m_downTaps.data();
    const float dcCoeff = m_dcCoeff;

    // While the limiter is not fully on, what Inflate adds may not take a sample past full scale,
    // nor past where the dry sample already was: overs that were in the input pass as they were.
    const bool clipInflate = m_limiterFade < 1.0f;
    std::uint64_t clips = 0;

    for (int i = 0; i < numFrames; ++i)
    {
        // Oldest..newest base-rate inputs. The oldest, 2K - 1 samples back, is the dry signal
        // aligned with what comes out of the up/down filter pair.
        input[inputPosition] = input[inputPosition + taps] = data[i];
        const float* const recent = input + inputPosition + 1;

        if (++inputPosition == taps)
            inputPosition = 0;

        const float dry = recent[0];

        // 2x up: the even phase is the 2K-tap FIR, the odd phase is the centre tap (a delay).
        const float upEven = dot(upTaps, recent, taps);
        const float upOdd = recent[half];

        const Deviation a = curveDeviation(upEven, m_frameAlpha[i], m_frameBeta[i], m_frameMu[i]);
        const Deviation b = curveDeviation(upOdd, m_frameAlpha[i], m_frameBeta[i], m_frameMu[i]);

        // AC-couple only the even part (in time order, at 2x): the odd part has no DC.
        const float acA = a.even - dcIn + dcCoeff * dcOut;
        const float acB = b.even - a.even + dcCoeff * acA;
        dcIn = b.even;
        dcOut = acB;

        // 2x down: FIR over the even-phase history plus the centre tap on the odd phase.
        shaped[shapedPosition] = shaped[shapedPosition + taps] = a.odd + acA;
        const float* const recentShaped = shaped + shapedPosition + 1;

        if (++shapedPosition == taps)
            shapedPosition = 0;

        const float centre = shapedOdd[oddPosition];
        shapedOdd[oddPosition] = b.odd + acB;

        if (++oddPosition == half)
            oddPosition = 0;

        const float deviation = dot(downTaps, recentShaped, taps) + 0.5f * centre;

        float y = dry + m_frameEffect[i] * deviation;

        if (clipInflate)
        {
            // Held to the input's own peak around this sample, and to full scale: Inflate may reshape
            // a waveform that was already over full scale, but never raises its peak, and never takes
            // material that was within full scale over it. Nothing is clipped at Effect 0, so this is
            // continuous in Effect; NaN and inf fail the comparisons and are left to the output stage.
            float limit = std::max(1.0f, std::abs(dry));

            if (std::abs(y) > limit)
            {
                for (int j = 1; j < taps; ++j)
                    limit = std::max(limit, std::abs(recent[j]));     // the inputs after this one ...
                for (const float v : channel.pastPeak)
                    limit = std::max(limit, v);                       // ... and before it

                if (std::abs(y) > limit)
                {
                    y = std::copysign(limit, y);
                    ++clips;
                }
            }
        }

        channel.pastPeak[static_cast<size_t>(pastPosition)] = std::abs(dry);
        if (++pastPosition == static_cast<int>(channel.pastPeak.size()))
            pastPosition = 0;

        data[i] = y;
        m_framePeak[i] = std::max(m_framePeak[i], std::abs(y));
    }

    if (clips > 0)
        m_inflateClipCount.fetch_add(clips, std::memory_order_relaxed);

    // Keep the decaying coupling state out of the denormal range (hosts usually set FTZ as well).
    if (std::abs(dcOut) < 1.0e-20f)
        dcOut = 0.0f;

    channel.inputPosition = inputPosition;
    channel.shapedPosition = shapedPosition;
    channel.oddPosition = oddPosition;
    channel.dcIn = dcIn;
    channel.dcOut = dcOut;
    channel.pastPosition = pastPosition;
}

float MasterProcessor::computeGain(int numFrames)
{
    float minGain = 1.0f;
    const bool limiterOn = m_target.limiterOn;
    int position = m_delayPosition;

    for (int i = 0; i < numFrames; ++i)
    {
        if (m_ceilingDb.moving)
            m_ceilingGain = dbToGain(static_cast<float>(m_ceilingDb.next()));

        // The clamp uses the ceiling that was in force when the outgoing sample came in,
        // the same one its gain was computed against, so a moving ceiling never clips.
        float& ceilingSlot = m_ceilingDelay[static_cast<size_t>(position)];
        const float outgoingCeiling = ceilingSlot;
        ceilingSlot = m_ceilingGain;

        if (++position == m_lookahead)
            position = 0;

        // --- Target gain for the incoming sample ------------------------------------
        float peak = m_framePeak[i];
        if (!(peak < c_maxDetectPeak))
            peak = c_maxDetectPeak;

        const float threshold = m_ceilingGain * c_detectionMargin;
        const float target = peak > threshold ? threshold / peak : 1.0f;

        // --- Hold over the look-ahead, instant attack / one-pole release, then smoothing
        const double held = static_cast<double>(m_minimum.push(target));

        double released = m_releaseGain;
        if (held <= released)
        {
            released = held; // the moving averages below shape the attack
        }
        else
        {
            released += m_releaseCoeff * (held - released);
            if (held - released < 1.0e-9)
                released = held; // land exactly, so unity gain is exactly 1
        }
        m_releaseGain = released;

        const float gain = static_cast<float>(m_box2.push(m_box1.push(released)));

        // --- On/off crossfade. The gain computer always runs, so it is ready when switched on;
        // smootherstep (C2 at both ends) keeps the fade itself free of corners.
        m_limiterFade = limiterOn ? std::min(1.0f, m_limiterFade + m_limiterFadeStep)
                                  : std::max(0.0f, m_limiterFade - m_limiterFadeStep);

        float applied = 1.0f;
        if (m_limiterFade >= 1.0f)
        {
            applied = gain;
        }
        else if (m_limiterFade > 0.0f)
        {
            const float p = m_limiterFade;
            const float mix = p * p * p * (p * (6.0f * p - 15.0f) + 10.0f);
            applied = 1.0f + mix * (gain - 1.0f);
        }

        m_frameGain[i] = applied;
        // Limiter fully on: clamp to its ceiling (a safety net). Otherwise nothing is clamped here:
        // the overs Inflate itself would create are already held back in inflate().
        m_frameClamp[i] = m_limiterFade >= 1.0f ? outgoingCeiling : c_noClamp;
        minGain = std::min(minGain, applied);
    }

    return minGain;
}

void MasterProcessor::updateMeter(float minGain, int numFrames)
{
    // Peak-meter ballistics at audio rate: instant attack, hold, then a steady fall, never
    // showing less reduction than is being applied right now. A UI polling at any rate then
    // sees every transient at full depth.
    const float nowDb = 20.0f * std::log10(std::max(minGain, 1.0e-6f)); // <= 0

    if (nowDb <= m_meterDb)
    {
        m_meterDb = nowDb;
        m_meterHoldLeft = m_meterHoldSamples;
        return;
    }

    if (m_meterHoldLeft > 0)
    {
        m_meterHoldLeft = std::max(0, m_meterHoldLeft - numFrames);
        return;
    }

    m_meterDb = std::min(nowDb, m_meterDb + m_meterFallPerSample * static_cast<float>(numFrames));
}

void MasterProcessor::applyGain(float* data, Channel& channel, int numFrames, int delayPosition)
{
    float* const delay = channel.delay.data();
    int position = delayPosition;

    for (int i = 0; i < numFrames; ++i)
    {
        const float delayed = delay[position];
        delay[position] = data[i];

        if (++position == m_lookahead)
            position = 0;

        float y = delayed * m_frameGain[i];
        const float limit = m_frameClamp[i];

        // Limiter on: a safety net only (the gain computer already keeps y under the ceiling), and
        // NaN becomes silence. Limiter off: the limit is the largest float, so only NaN and +-inf get
        // in here, and they become silence too: a non-finite sample never reaches the host.
        if (!(std::abs(y) <= limit))
        {
            y = (limit < c_noClamp && std::abs(y) > limit) ? std::copysign(limit, y) : 0.0f;
            m_safetyClampCount.fetch_add(1, std::memory_order_relaxed);
        }

        data[i] = y;
    }
}

#include "MasterProcessor.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace
{
    constexpr double c_twoPi = 6.283185307179586;

    constexpr double c_lookaheadSeconds   = 0.0015;
    constexpr double c_paramRampSeconds   = 0.012; // effect, curve and ceiling glide this long (short: they can be modulated)
    constexpr double c_limiterFadeSeconds = 0.030; // limiter on/off crossfade
    constexpr double c_dcBlockHz          = 5.0;   // AC coupling of the even part of the curve
    constexpr double c_pushRampSeconds    = 0.020; // Push glides this long (short: it can be modulated)
    constexpr double c_sustainAttackSeconds = 0.030; // how quickly the slow stage settles once reduction is lasting
    constexpr double c_activityUpSeconds    = 0.020; // how the "is this lasting?" measure builds up and falls away
    constexpr double c_activityDownSeconds  = 0.060;
    constexpr double c_activityStart        = 0.45;  // the slow stage starts to move here ...
    constexpr double c_activityFull         = 0.75;  // ... and moves at its full rate from here

    // The gain computer aims 2^-20 (~0.00001 dB) under the ceiling, so float
    // rounding in the gain path can never carry a sample over it.
    constexpr float c_detectionMargin = 1.0f - 1.0f / 1048576.0f;

    // NaN/inf in the detector are treated as a huge peak so they cannot poison the gain state.
    constexpr float c_maxDetectPeak = 1.0e20f;
    constexpr float c_noClamp = std::numeric_limits<float>::infinity();

    float sanitise(float value, float lo, float hi)
    {
        // Written so that NaN falls to `lo` instead of propagating.
        return value >= lo ? (value <= hi ? value : hi) : lo;
    }

    float dbToGain(float db)
    {
        return std::exp(db * 0.115129255f); // ln(10) / 20
    }

    //==============================================================================
    // True-peak detector: three points between each pair of samples, each read from c_detectorTaps samples with
    // a Kaiser-windowed sinc (a 4x polyphase interpolator without the phase that is the samples themselves).
    // 48 samples per point is longer than a meter needs for music; it is what keeps full-band noise bursts
    // (hits), whose peaks between the samples are built from many samples either side, within 0.1 dB.
    static_assert(MasterProcessor::c_detectorTaps % 4 == 0, "the detector sums in fours");
    constexpr int c_taps = MasterProcessor::c_detectorTaps;
    constexpr int c_centre = c_taps / 2 - 1; // index in a window of the sample the estimate belongs to

    double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k)
        {
            term *= (0.5 * x / k) * (0.5 * x / k);
            sum += term;
        }
        return sum;
    }

    struct DetectorTable
    {
        float phase[3][c_taps];

        DetectorTable()
        {
            constexpr double beta = 5.0;
            const double norm = besselI0(beta);
            for (int p = 0; p < 3; ++p)
            {
                double total = 0.0;
                double h[c_taps];
                for (int j = 0; j < c_taps; ++j)
                {
                    const double d = static_cast<double>(j - c_centre) - 0.25 * (p + 1); // distance from the point, in samples
                    const double px = 0.5 * c_twoPi * d;
                    const double r = d / (0.5 * c_taps);
                    h[j] = (std::sin(px) / px) * besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / norm;
                    total += h[j];
                }
                for (int j = 0; j < c_taps; ++j)
                    phase[p][j] = static_cast<float>(h[j] / total); // a steady level reads as itself
            }
        }
    };

    const DetectorTable& detectorTable()
    {
        static const DetectorTable table;
        return table;
    }

    // Exact comparison on purpose (unchanged host values); std::equal_to keeps -Wfloat-equal quiet.
    bool exactlyEqual(float a, float b)
    {
        return std::equal_to<float>()(a, b);
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
//   capacitor after an asymmetric analogue stage would.
//
// f is linear in (alpha, beta, mu), so f' is too: any blend of monotonic anchor curves is
// itself monotonic. Curve morphs piecewise-linearly between three anchors whose minimum
// slope over [-1, 1] was checked numerically (1 + alpha at 0, minimum in the upper range):
//
//   curve -1  gentle      alpha 0.45  beta 0     mu 0     +3.2 dB lift, min slope 0.64, odd only
//   curve  0  neutral     alpha 0.70  beta 0.10  mu 0     +4.6 dB lift, min slope 0.41, odd only
//   curve +1  aggressive  alpha 0.90  beta 0.20  mu 0.12  +5.6 dB lift, min slope 0.16, + 2nd harmonic
//
// Output = x + effect * (f(x) - x): Effect is a straight blend, and Effect = 0 is the input.
namespace
{
    struct Anchor
    {
        float alpha, beta, mu;
    };

    constexpr Anchor c_gentle     { 0.45f, 0.00f, 0.00f };
    constexpr Anchor c_neutral    { 0.70f, 0.10f, 0.00f };
    constexpr Anchor c_aggressive { 0.90f, 0.20f, 0.12f };
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
void MasterProcessor::Ramp::snap(float value)
{
    current = target = value;
    step = 0.0f;
    remaining = 0;
}

void MasterProcessor::Ramp::setTarget(float value, int lengthSamples)
{
    if (exactlyEqual(value, target))
        return;

    target = value;

    if (lengthSamples <= 0)
    {
        snap(value);
        return;
    }

    // Always the full ramp length from wherever we are, so a new target mid-glide stays smooth.
    step = (target - current) / static_cast<float>(lengthSamples);
    remaining = lengthSamples;
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
float MasterProcessor::detectPeak(const float* window)
{
    const DetectorTable& table = detectorTable();

    // Four running sums per point, so the compiler can keep each point's sum in one vector register.
    float sumA[4] {}, sumB[4] {}, sumC[4] {};

    for (int j = 0; j < c_taps; j += 4)
    {
        for (int k = 0; k < 4; ++k)
        {
            const float x = window[j + k];
            sumA[k] += x * table.phase[0][j + k];
            sumB[k] += x * table.phase[1][j + k];
            sumC[k] += x * table.phase[2][j + k];
        }
    }

    const float a = (sumA[0] + sumA[1]) + (sumA[2] + sumA[3]);
    const float b = (sumB[0] + sumB[1]) + (sumB[2] + sumB[3]);
    const float c = (sumC[0] + sumC[1]) + (sumC[2] + sumC[3]);

    // std::max keeps its first argument when the second is NaN, so the comparisons are ordered to let a NaN
    // through to the caller (which treats it as a huge peak) instead of hiding it.
    float peak = std::abs(window[c_centre]);
    for (const float v : { std::abs(a), std::abs(b), std::abs(c) })
        peak = v > peak || v != v ? v : peak;

    return peak;
}

//==============================================================================
void MasterProcessor::prepare(double sampleRate, int maxChannels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    m_numChannels = std::max(1, maxChannels);
    m_lookahead = std::max(1, static_cast<int>(std::lround(c_lookaheadSeconds * m_sampleRate)));

    // The detector reports the peak around a sample once it has seen the c_detectorTaps / 2 samples after it,
    // so the audio waits that much longer than the gain computer's look-ahead.
    m_latency = m_lookahead + c_taps / 2;
    detectorTable(); // built here, not on the audio thread

    m_channels.assign(static_cast<size_t>(m_numChannels), Channel {});
    for (auto& channel : m_channels)
        channel.delay.assign(static_cast<size_t>(m_latency), 0.0f);

    m_ceilingDelay.assign(static_cast<size_t>(m_lookahead), 1.0f);

    // No-overs guarantee: the gain leaving the smoothing at time n is a weighted average
    // (positive weights summing to 1) of held gains from n - lookahead .. n, and each of
    // those holds the minimum target over a window that contains the target of the sample
    // being output (the detector's reading for it, which is never below the sample itself).
    // So the applied gain is never above what that sample needs.
    // Two cascaded moving averages (lengths summing to lookahead + 2, span lookahead + 1) give
    // an S-shaped attack instead of a linear ramp with corners.
    m_minimum.prepare(m_lookahead + 1);
    const int boxTotal = m_lookahead + 2;
    m_box1.prepare(boxTotal / 2);
    m_box2.prepare(boxTotal - boxTotal / 2);

    m_rampLength = std::max(1, static_cast<int>(std::lround(c_paramRampSeconds * m_sampleRate)));
    m_limiterMixStep = static_cast<float>(1.0 / std::max(1.0, c_limiterFadeSeconds * m_sampleRate));
    m_dcCoeff = static_cast<float>(std::exp(-c_twoPi * c_dcBlockHz / m_sampleRate));
    m_pushRampLength = std::max(1, static_cast<int>(std::lround(c_pushRampSeconds * m_sampleRate)));
    updateReleaseCoefficients();

    reset();
}

void MasterProcessor::updateReleaseCoefficients()
{
    auto coeff = [this](double seconds) { return 1.0 - std::exp(-1.0 / (std::max(1.0e-4, seconds) * m_sampleRate)); };
    m_releaseCoeff = coeff(m_target.releaseMs * 0.001);
    m_sustainAttackCoeff = coeff(c_sustainAttackSeconds);
    m_sustainReleaseCoeff = m_target.sustainReleaseMs > 0.0f ? coeff(m_target.sustainReleaseMs * 0.001) : 0.0;
    m_activityUpCoeff = coeff(c_activityUpSeconds);
    m_activityDownCoeff = coeff(c_activityDownSeconds);
}

void MasterProcessor::reset()
{
    for (auto& channel : m_channels)
    {
        std::fill(channel.delay.begin(), channel.delay.end(), 0.0f);
        std::fill(std::begin(channel.history), std::end(channel.history), 0.0f);
        channel.dcIn = channel.dcOut = 0.0f;
    }

    m_delayPosition = 0;
    m_lookPosition = 0;
    m_historyPosition = 0;
    m_push.snap(dbToGain(m_target.pushDb));

    m_effect.snap(m_target.inflateEffect);
    m_curve.snap(m_target.inflateCurve);
    m_ceilingDb.snap(m_target.ceilingDb);
    m_ceilingGain = dbToGain(m_target.ceilingDb);
    std::fill(m_ceilingDelay.begin(), m_ceilingDelay.end(), m_ceilingGain);
    m_limiterMix = m_target.limiterOn ? 1.0f : 0.0f;
    m_inflateActive = false;

    m_minimum.reset();
    m_box1.reset();
    m_box2.reset();
    m_releaseGain = 1.0;
    m_sustainGain = 1.0;
    m_sustainActivity = 0.0;

    m_safetyClampCount = 0;
    m_gainReductionDb.store(0.0f, std::memory_order_relaxed);
    m_gainReductionHoldDb.store(0.0f, std::memory_order_relaxed);

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
    t.sustainReleaseMs = sanitise(p.sustainReleaseMs, 0.0f, 5000.0f);
    t.pushDb = sanitise(p.pushDb, 0.0f, 24.0f);

    if (m_snapNextParameters)
    {
        m_snapNextParameters = false;
        m_effect.snap(t.inflateEffect);
        m_curve.snap(t.inflateCurve);
        m_ceilingDb.snap(t.ceilingDb);
        m_ceilingGain = dbToGain(t.ceilingDb);
        std::fill(m_ceilingDelay.begin(), m_ceilingDelay.end(), m_ceilingGain);
        m_limiterMix = t.limiterOn ? 1.0f : 0.0f;
        m_push.snap(dbToGain(t.pushDb));
    }
    else
    {
        if (!exactlyEqual(t.pushDb, m_target.pushDb))
            m_push.setTarget(dbToGain(t.pushDb), m_pushRampLength);

        m_effect.setTarget(t.inflateEffect, m_rampLength);
        m_curve.setTarget(t.inflateCurve, m_rampLength);
        m_ceilingDb.setTarget(t.ceilingDb, m_rampLength);
    }

    // The release rates can change abruptly: the gain itself stays continuous.
    const bool releaseChanged = !exactlyEqual(t.releaseMs, m_target.releaseMs)
                             || !exactlyEqual(t.sustainReleaseMs, m_target.sustainReleaseMs);
    m_target = t;

    if (releaseChanged)
        updateReleaseCoefficients();
}

//==============================================================================
void MasterProcessor::process(float* const* channels, int numChannels, int numSamples)
{
    const int activeChannels = std::min(numChannels, m_numChannels);

    if (channels == nullptr || activeChannels <= 0 || numSamples <= 0)
        return;

    m_blockMinGain = 1.0f;

    for (int offset = 0; offset < numSamples; offset += c_blockSize)
    {
        const int numFrames = std::min(c_blockSize, numSamples - offset);

        // Inflate is skipped (output is the input, bit for bit) while Effect rests at 0.
        const bool inflateActive = m_effect.isRamping() || m_effect.current > 0.0f;

        if (inflateActive && !m_inflateActive)
        {
            // Coming back from bypass: the AC coupling restarts from silence. Effect ramps
            // up from 0, so this cannot click.
            for (auto& channel : m_channels)
                channel.dcIn = channel.dcOut = 0.0f;
        }

        m_inflateActive = inflateActive;

        if (m_inflateActive)
            renderControl(numFrames);
        else
            m_curve.snap(m_curve.target); // inaudible while bypassed, so no need to glide

        // Push: after inflate, into the limiter. At rest on 0 dB the gain is exactly 1.
        for (int i = 0; i < numFrames; ++i)
            m_framePush[i] = m_push.next();

        std::fill_n(m_framePeak, numFrames, 0.0f);

        for (int ch = 0; ch < activeChannels; ++ch)
        {
            Channel& channel = m_channels[static_cast<size_t>(ch)];

            if (m_inflateActive)
                inflate(channels[ch] + offset, channel, numFrames);

            pushAndDetect(channels[ch] + offset, channel, numFrames);
        }

        m_historyPosition = (m_historyPosition + numFrames) % c_taps;

        computeGain(numFrames);

        for (int ch = 0; ch < activeChannels; ++ch)
            applyGain(channels[ch] + offset, m_channels[static_cast<size_t>(ch)], numFrames, m_delayPosition);

        m_delayPosition = (m_delayPosition + numFrames) % m_latency;
    }

    const float reductionDb = 20.0f * std::log10(std::max(m_blockMinGain, 1.0e-6f));
    m_gainReductionDb.store(reductionDb, std::memory_order_relaxed);

    // The meter's hold. Its reader may reset it between this load and store; the worst that does is show one
    // reduction for one more reading.
    if (reductionDb < m_gainReductionHoldDb.load(std::memory_order_relaxed))
        m_gainReductionHoldDb.store(reductionDb, std::memory_order_relaxed);
}

void MasterProcessor::renderControl(int numFrames)
{
    if (!m_effect.isRamping() && !m_curve.isRamping())
    {
        const Shape s = shapeForCurve(m_curve.current);
        std::fill_n(m_frameEffect, numFrames, m_effect.current);
        std::fill_n(m_frameAlpha, numFrames, s.alpha);
        std::fill_n(m_frameBeta, numFrames, s.beta);
        std::fill_n(m_frameMu, numFrames, s.mu);
        return;
    }

    for (int i = 0; i < numFrames; ++i)
    {
        m_frameEffect[i] = m_effect.next();

        const Shape s = shapeForCurve(m_curve.next());
        m_frameAlpha[i] = s.alpha;
        m_frameBeta[i] = s.beta;
        m_frameMu[i] = s.mu;
    }
}

void MasterProcessor::pushAndDetect(float* data, Channel& channel, int numFrames)
{
    float* const history = channel.history;
    int position = m_historyPosition;

    for (int i = 0; i < numFrames; ++i)
    {
        const float y = data[i] * m_framePush[i];
        data[i] = y;

        history[position] = y;
        history[position + c_taps] = y;

        if (++position == c_taps)
            position = 0;

        // The window now ends at this sample; the reading belongs to the sample c_taps / 2 back.
        const float peak = detectPeak(history + position);
        m_framePeak[i] = peak > m_framePeak[i] || peak != peak ? peak : m_framePeak[i];
    }
}

void MasterProcessor::inflate(float* data, Channel& channel, int numFrames)
{
    float dcIn = channel.dcIn;
    float dcOut = channel.dcOut;
    const float dcCoeff = m_dcCoeff;

    for (int i = 0; i < numFrames; ++i)
    {
        const float x = data[i];
        const float x2 = x * x;
        float odd = 0.0f;
        float even = 0.0f;

        if (x2 < 1.0f) // false for NaN too, which keeps the coupling filter finite
        {
            const float w = (1.0f - x2) * (1.0f - x2);
            odd = w * x * (m_frameAlpha[i] + m_frameBeta[i] * x2);
            even = w * m_frameMu[i] * x2;
        }

        // Only the even part is AC-coupled: the odd part has no DC and stays memoryless.
        const float evenAc = even - dcIn + dcCoeff * dcOut;
        dcIn = even;
        dcOut = evenAc;

        data[i] = x + m_frameEffect[i] * (odd + evenAc);
    }

    // Keep the decaying coupling state out of the denormal range (hosts usually set FTZ as well).
    if (std::abs(dcOut) < 1.0e-20f)
        dcOut = 0.0f;

    channel.dcIn = dcIn;
    channel.dcOut = dcOut;
}

void MasterProcessor::computeGain(int numFrames)
{
    const bool limiterOn = m_target.limiterOn;
    const bool sustainStage = m_sustainReleaseCoeff > 0.0;
    int position = m_lookPosition;

    for (int i = 0; i < numFrames; ++i)
    {
        if (m_ceilingDb.isRamping())
            m_ceilingGain = dbToGain(m_ceilingDb.next());

        // The clamp uses the ceiling that was in force when the outgoing sample came in,
        // the same one its gain was computed against, so a moving ceiling never clips.
        float& ceilingSlot = m_ceilingDelay[static_cast<size_t>(position)];
        const float outgoingCeiling = ceilingSlot;
        ceilingSlot = m_ceilingGain;

        if (++position == m_lookahead)
            position = 0;

        // --- Target gain for the sample the detector has just finished reading ------------------------------------
        float peak = m_framePeak[i];
        if (!(peak < c_maxDetectPeak))
            peak = c_maxDetectPeak;

        const float threshold = m_ceilingGain * c_detectionMargin;
        const float target = peak > threshold ? threshold / peak : 1.0f;

        // --- Hold over the look-ahead, instant attack / programme-dependent release, then smoothing
        const float held = m_minimum.push(target);

        // Fast stage: takes every over at once and lets go quickly, so a short over (a hit) leaves no hole.
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

        // Slow stage: drifts down to reduction that lasts and lets go slowly, so sustained loud material is
        // held at a steady gain instead of being let go and caught again on every cycle. It only ever takes
        // the gain lower than the fast stage would, so the no-overs argument is unchanged.
        if (sustainStage)
        {
            // "Lasting" is judged by how much of the recent past needed any reduction at all: the activity
            // rises while there is some and falls away without it. Until it has built up, the slow stage does
            // not move, so one short over leaves nothing behind for it to let go of slowly.
            const bool reducing = held < 1.0f;
            m_sustainActivity += (reducing ? m_activityUpCoeff : m_activityDownCoeff) * ((reducing ? 1.0 : 0.0) - m_sustainActivity);
            if (m_sustainActivity < 1.0e-9)
                m_sustainActivity = 0.0; // rest exactly, out of the denormal range

            const double engaged = std::clamp((m_sustainActivity - c_activityStart) / (c_activityFull - c_activityStart), 0.0, 1.0);

            double sustained = m_sustainGain;
            sustained += (held < sustained ? engaged * m_sustainAttackCoeff : m_sustainReleaseCoeff) * (held - sustained);
            if (std::abs(held - sustained) < 1.0e-9)
                sustained = held;
            m_sustainGain = sustained;

            if (sustained < released)
                released = sustained;
        }
        else
        {
            m_sustainGain = 1.0;
            m_sustainActivity = 0.0;
        }

        const float gain = static_cast<float>(m_box2.push(m_box1.push(released)));

        // --- On/off crossfade (the gain computer always runs, so it is ready when switched on)
        m_limiterMix = limiterOn ? std::min(1.0f, m_limiterMix + m_limiterMixStep)
                                 : std::max(0.0f, m_limiterMix - m_limiterMixStep);

        float applied = 1.0f;
        if (m_limiterMix >= 1.0f)
            applied = gain;
        else if (m_limiterMix > 0.0f)
            applied = 1.0f + m_limiterMix * (gain - 1.0f);

        m_frameGain[i] = applied;
        m_frameClamp[i] = m_limiterMix >= 1.0f ? outgoingCeiling : c_noClamp;
        m_blockMinGain = std::min(m_blockMinGain, applied);
    }

    m_lookPosition = position;
}

void MasterProcessor::applyGain(float* data, Channel& channel, int numFrames, int delayPosition)
{
    float* const delay = channel.delay.data();
    int position = delayPosition;

    for (int i = 0; i < numFrames; ++i)
    {
        const float delayed = delay[position];
        delay[position] = data[i];

        if (++position == m_latency)
            position = 0;

        float y = delayed * m_frameGain[i];
        const float limit = m_frameClamp[i];

        // Safety net only; the gain computer already keeps y under the ceiling. NaN becomes silence.
        if (!(std::abs(y) <= limit))
        {
            y = y > limit ? limit : (y < -limit ? -limit : 0.0f);
            ++m_safetyClampCount;
        }

        data[i] = y;
    }
}

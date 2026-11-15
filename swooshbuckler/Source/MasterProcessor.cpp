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

    // Sides: which speakers carry the same sound (see inflateSides).
    constexpr double c_alikeSeconds = 0.025;  // how far back two speakers are compared
    constexpr float c_alikeFrom = 0.1f;       // likeness (rho^2) from which a neighbour starts to count ...
    constexpr float c_alikeTo = 0.6f;         // ... and from which it counts whole
    constexpr float c_alikeFloor = 1.0e-10f;  // a speaker under this power (-100 dB) has nothing to compare

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
    // True-peak detector: three points between each pair of samples, each read from `taps` samples, and the crest
    // of the curve through the points where one stands above its neighbours (refinePeak).
    //
    // Each point is the value of the waveform a quarter, a half or three quarters of a sample after the centre
    // sample: a fractional delay of d samples. The weights h are the least-squares best over the band the signal
    // can fill, weighted by how much it matters there:
    //
    //   minimise  sum over the bands of  weight * integral over the band of |H(w) - e^(-j w d)|^2
    //
    // whose normal equations are  P h = p, with (per band from a to b of half the sample rate, weight u)
    // P[j][k] += u (b sinc(b (j - k)) - a sinc(a (j - k))),  p[j] += u (b sinc(b (j - d)) - a sinc(a (j - d))).
    // Nearly all of the weight is where sound is, less towards half the sample rate, and a little on the very top
    // so that what is up there is not blown up. A windowed sinc spends its accuracy on a stop band a peak reading
    // has no use for: 48 samples of one are 1 dB down at 0.47 of the sample rate.
    //
    // How many samples: what the top of the band needs. Up to 96 kHz sound reaches right up to half the sample
    // rate (a 96 kHz recording is made to keep what is up there), and 128 samples read a point within 0.1% up to
    // 0.46 of the sample rate and within 1.5% at 0.48. At 176.4 and 192 kHz the top of the band is far above
    // anything heard, or kept by a conversion to a lower rate, and 64 samples (within 0.1% up to 0.4 of the sample
    // rate, 77 kHz; 3% at 0.48) are plenty.
    struct Band
    {
        double edge, weight;
    };
    constexpr Band c_bands128[] = { { 0.94, 1000.0 }, { 0.98, 30.0 }, { 0.995, 1.0 }, { 1.0, 0.01 } };
    constexpr Band c_bands64[]  = { { 0.90, 1000.0 }, { 0.97, 10.0 }, { 0.99, 1.0 }, { 1.0, 0.01 } };

    // The most a top between two points can stand above the larger of them: a tone at half the sample rate,
    // its crest midway between two points an eighth of a cycle apart (1 / cos(pi / 8)), and a little over.
    constexpr float c_refineMost = 1.09f;

    struct DetectorTable
    {
        int taps = 0;
        int centre = 0;            // index in a window of the sample the estimate belongs to
        std::vector<float> phase;  // three rows of `taps` weights

        DetectorTable(int numTaps, const Band* bands, int numBands)
            : taps(numTaps), centre(numTaps / 2 - 1), phase(static_cast<size_t>(3 * numTaps))
        {
            auto sinc = [](double x) { return std::abs(x) < 1.0e-12 ? 1.0 : std::sin(0.5 * c_twoPi * x) / (0.5 * c_twoPi * x); };
            const auto n = static_cast<size_t>(taps);
            std::vector<double> m(n * (n + 1)), h(n);
            for (int p = 0; p < 3; ++p)
            {
                const double d = static_cast<double>(centre) + 0.25 * (p + 1);
                std::fill(m.begin(), m.end(), 0.0);
                for (size_t j = 0; j < n; ++j)
                {
                    double from = 0.0;
                    for (int b = 0; b < numBands; ++b)
                    {
                        const double edge = bands[b].edge, weight = bands[b].weight;
                        for (size_t k = 0; k < n; ++k)
                        {
                            const double x = static_cast<double>(j) - static_cast<double>(k);
                            m[j * (n + 1) + k] += weight * (edge * sinc(edge * x) - from * sinc(from * x));
                        }
                        const double x = static_cast<double>(j) - d;
                        m[j * (n + 1) + n] += weight * (edge * sinc(edge * x) - from * sinc(from * x));
                        from = edge;
                    }
                }
                // Gaussian elimination with partial pivoting (the matrix is symmetric positive definite).
                for (size_t col = 0; col < n; ++col)
                {
                    size_t best = col;
                    for (size_t row = col + 1; row < n; ++row)
                        if (std::abs(m[row * (n + 1) + col]) > std::abs(m[best * (n + 1) + col]))
                            best = row;
                    if (best != col)
                        for (size_t k = 0; k <= n; ++k)
                            std::swap(m[col * (n + 1) + k], m[best * (n + 1) + k]);
                    for (size_t row = col + 1; row < n; ++row)
                    {
                        const double f = m[row * (n + 1) + col] / m[col * (n + 1) + col];
                        for (size_t k = col; k <= n; ++k)
                            m[row * (n + 1) + k] -= f * m[col * (n + 1) + k];
                    }
                }
                double total = 0.0;
                for (size_t row = n; row-- > 0;)
                {
                    double v = m[row * (n + 1) + n];
                    for (size_t k = row + 1; k < n; ++k)
                        v -= m[row * (n + 1) + k] * h[k];
                    h[row] = v / m[row * (n + 1) + row];
                    total += h[row];
                }
                for (size_t j = 0; j < n; ++j)
                    phase[static_cast<size_t>(p) * n + j] = static_cast<float>(h[j] / total); // a steady level reads as itself
            }
        }
    };

    // Built the first time each is asked for (in prepare(), never on the audio thread).
    const DetectorTable& detectorTable(int taps)
    {
        if (taps >= 128)
        {
            static const DetectorTable table(128, c_bands128, 4);
            return table;
        }
        static const DetectorTable table(64, c_bands64, 4);
        return table;
    }

    // A block of one channel through the detector's history: Push applied in place, and for each sample the
    // four readings and the sample after them. The length is fixed at compile time (one copy of this for each),
    // so the sums are unrolled and kept in vector registers.
    template <int Taps>
    void readBlock(const float* phase, float* history, int& historyPosition, float* data, const float* push, int numFrames,
                   float (*points)[4], float* next)
    {
        const float* const pa = phase;
        const float* const pb = pa + Taps;
        const float* const pc = pb + Taps;
        int position = historyPosition;

        for (int i = 0; i < numFrames; ++i)
        {
            const float y = data[i] * push[i];
            data[i] = y;

            history[position] = y;
            history[position + Taps] = y;

            if (++position == Taps)
                position = 0;

            // The window now ends at this sample; the reading belongs to the sample Taps / 2 back.
            const float* const window = history + position;
            float sumA[4] {}, sumB[4] {}, sumC[4] {};
            for (int j = 0; j < Taps; j += 4)
            {
                for (int k = 0; k < 4; ++k)
                {
                    const float x = window[j + k];
                    sumA[k] += x * pa[j + k];
                    sumB[k] += x * pb[j + k];
                    sumC[k] += x * pc[j + k];
                }
            }

            points[i][0] = window[Taps / 2 - 1];
            points[i][1] = (sumA[0] + sumA[1]) + (sumA[2] + sumA[3]);
            points[i][2] = (sumB[0] + sumB[1]) + (sumB[2] + sumB[3]);
            points[i][3] = (sumC[0] + sumC[1]) + (sumC[2] + sumC[3]);
            next[i] = window[Taps / 2];
        }

        historyPosition = position;
    }

    void readPoints(const DetectorTable& table, const float* window, float points[4])
    {
        const float* const pa = table.phase.data();
        const float* const pb = pa + table.taps;
        const float* const pc = pb + table.taps;

        // Four running sums per point, so the compiler can keep each point's sum in one vector register.
        float sumA[4] {}, sumB[4] {}, sumC[4] {};
        for (int j = 0; j < table.taps; j += 4)
        {
            for (int k = 0; k < 4; ++k)
            {
                const float x = window[j + k];
                sumA[k] += x * pa[j + k];
                sumB[k] += x * pb[j + k];
                sumC[k] += x * pc[j + k];
            }
        }

        points[0] = window[table.centre];
        points[1] = (sumA[0] + sumA[1]) + (sumA[2] + sumA[3]);
        points[2] = (sumB[0] + sumB[1]) + (sumB[2] + sumB[3]);
        points[3] = (sumC[0] + sumC[1]) + (sumC[2] + sumC[3]);
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
float MasterProcessor::refinePeak(float before, float point, float after)
{
    // The point's size, and with the sign taken out the two either side of it.
    const float size = std::abs(point);
    const float sign = point < 0.0f ? -1.0f : 1.0f;
    const float l = sign * before, r = sign * after;
    if (l > size || r > size)
        return size; // not the top: the larger neighbour is read as a top itself

    const float bend = 2.0f * size - l - r;
    if (bend <= 1.0e-30f)
        return size;

    const float lean = l - r;
    const float top = size + lean * lean / (8.0f * bend);
    const float most = size * c_refineMost;
    return top < most || top != top ? top : most; // (a NaN goes through to the caller, which reads it as a huge peak)
}

float MasterProcessor::peakOf(float before, const float points[4], float after)
{
    float peak = refinePeak(before, points[0], points[1]);
    const float b = refinePeak(points[0], points[1], points[2]);
    const float c = refinePeak(points[1], points[2], points[3]);
    const float d = refinePeak(points[2], points[3], after);
    // std::max keeps its first argument when the second is NaN, so the comparisons are ordered to let a NaN
    // through to the caller (which treats it as a huge peak) instead of hiding it.
    for (const float v : { b, c, d })
        peak = v > peak || v != v ? v : peak;
    return peak;
}

int MasterProcessor::detectorTapsFor(double sampleRate)
{
    return sampleRate < 120000.0 ? 128 : 64;
}

float MasterProcessor::detectPeak(const float* window, int taps)
{
    const DetectorTable& table = detectorTable(taps);
    float before[4], points[4];
    readPoints(table, window, before);
    readPoints(table, window + 1, points);
    return peakOf(before[3], points, window[1 + table.centre + 1]);
}

void MasterProcessor::detectPoints(const float* window, float points[4], int taps)
{
    readPoints(detectorTable(taps), window, points);
}

//==============================================================================
void MasterProcessor::setSides(const float* side, int numChannels)
{
    m_sided = side != nullptr && numChannels > 0;
    for (size_t ch = 0; ch < m_channels.size(); ++ch)
    {
        Channel& channel = m_channels[ch];
        channel.left = channel.right = 0.0f;
        if (! m_sided || static_cast<int>(ch) >= numChannels)
            continue;
        const float s = side[ch];
        channel.left = s < 0.0f ? 1.0f : (s > 0.0f ? 0.0f : 0.5f);
        channel.right = 1.0f - channel.left;
    }

    // Every two speakers that count to the same side (a centre speaker counts to both): see inflateSides.
    m_pairs.clear(); // (within the room prepare() made: no allocation)
    if (m_sided)
        for (int a = 0; a < std::min(numChannels, m_numChannels); ++a)
            for (int b = a + 1; b < std::min(numChannels, m_numChannels); ++b)
            {
                const Channel& ca = m_channels[static_cast<size_t>(a)];
                const Channel& cb = m_channels[static_cast<size_t>(b)];
                if (ca.left * cb.left > 0.0f || ca.right * cb.right > 0.0f)
                {
                    Pair pair;
                    pair.a = a;
                    pair.b = b;
                    m_pairs.push_back(pair);
                }
            }
    resetAlike();
}

void MasterProcessor::resetAlike()
{
    for (auto& channel : m_channels)
        channel.power = channel.powerSum = 0.0f;
    for (Pair& pair : m_pairs)
    {
        pair.cross = pair.crossSum = 0.0f;
        pair.weight = pair.target = 1.0f;
        pair.step = 0.0f;
    }
    m_alikeCount = 0;
}

//==============================================================================
void MasterProcessor::prepare(double sampleRate, int maxChannels, double holdSeconds)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    m_numChannels = std::max(1, maxChannels);
    m_lookahead = std::max(1, static_cast<int>(std::lround(c_lookaheadSeconds * m_sampleRate)));

    // The detector reports the peak around a sample once it has seen the half of its samples that come after it,
    // so the audio waits that much longer than the gain computer's look-ahead.
    m_taps = detectorTapsFor(m_sampleRate);
    m_detector = &detectorTable(m_taps); // built here, not on the audio thread
    m_latency = m_lookahead + m_taps / 2;

    m_channels.assign(static_cast<size_t>(m_numChannels), Channel {});
    m_sided = false; // every channel on its own until setSides()
    m_pairs.clear();
    m_pairs.reserve(static_cast<size_t>(m_numChannels * (m_numChannels - 1) / 2));
    m_alikeCoeff = static_cast<float>(1.0 - std::exp(-static_cast<double>(c_alikeGrid) / (c_alikeSeconds * m_sampleRate))); // per grid
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
    // A hold keeps each reduction for that much longer before the release begins: the minimum is simply taken
    // over a longer past, which can only lower the gain, so the argument above stands.
    m_minimum.prepare(m_lookahead + 1 + std::max(0, static_cast<int>(std::lround(holdSeconds * m_sampleRate))));
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
        channel.dcIn = channel.dcOut = 0.0f; // (the sides stay: they are the layout, not state)
        channel.lastPoint = 0.0f;
    }
    m_sideLastPoint[0] = m_sideLastPoint[1] = 0.0f;
    resetAlike();

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

        if (m_sided)
        {
            // A room: each side's speakers as the one Stereo channel they stand in for (see setSides).
            if (m_inflateActive)
                inflateSides(channels, activeChannels, offset, numFrames);

            std::fill_n(&m_frameSidePower[0][0][0], 2 * c_blockSize * 5, 0.0f);
            for (int ch = 0; ch < activeChannels; ++ch)
                pushAndDetectSides(channels[ch] + offset, m_channels[static_cast<size_t>(ch)], numFrames);
            finishSidePeaks(numFrames);
        }
        else
        {
            for (int ch = 0; ch < activeChannels; ++ch)
            {
                Channel& channel = m_channels[static_cast<size_t>(ch)];

                if (m_inflateActive)
                    inflate(channels[ch] + offset, channel, numFrames);

                pushAndDetect(channels[ch] + offset, channel, numFrames);
            }
        }

        m_historyPosition = (m_historyPosition + numFrames) % m_taps;

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

void MasterProcessor::readChannel(float* data, Channel& channel, int numFrames, float (*points)[4], float* next)
{
    const DetectorTable& table = *static_cast<const DetectorTable*>(m_detector);
    int position = m_historyPosition;
    if (m_taps == 128)
        readBlock<128>(table.phase.data(), channel.history, position, data, m_framePush, numFrames, points, next);
    else
        readBlock<64>(table.phase.data(), channel.history, position, data, m_framePush, numFrames, points, next);
}

void MasterProcessor::pushAndDetect(float* data, Channel& channel, int numFrames)
{
    float points[c_blockSize][4], next[c_blockSize];
    readChannel(data, channel, numFrames, points, next);

    float last = channel.lastPoint;
    for (int i = 0; i < numFrames; ++i)
    {
        const float peak = peakOf(last, points[i], next[i]);
        last = points[i][3];
        m_framePeak[i] = peak > m_framePeak[i] || peak != peak ? peak : m_framePeak[i];
    }

    channel.lastPoint = last;
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

//==============================================================================
// Sides. The curve written as a gain on x: with r the level of the whole sound,
//
//   odd part    w(r) * x * (alpha + beta * r^2)   = x * [ w * (alpha + beta * r^2) ]
//   even part   w(r) * mu * x^2                   = x * [ w * mu * s ],   s = the whole sound, signed
//
// For one channel on its own r^2 = x^2 and s = x, which is the curve as inflate() computes it. In a room the
// whole sound a speaker's signal is part of is what the speakers of its side carry OF THAT SOUND: for speaker c
//
//   r_c^2 = sum over the side's speakers d of  w_cd * x_d^2        s_c = sum of  +-w_cd * x_d |x_d|  /  r_c
//
// with w_cd = 1 where d carries the same sound as c and 0 where what it carries has nothing to do with it
// (w_cc = 1; a centre speaker counts half to each side). When one sound is shared between the speakers with gains
// g_c (their squares adding up to 1), x_c = g_c * sound and every w is 1, so r^2 is sound^2 and s is the sound
// itself, and each speaker comes out as g_c * f(sound): the sound shaped whole, then shared, exactly as its Stereo
// channel would shape it, however many speakers it is spread over. A speaker carrying something else (another
// sound, the reverb, which is different in every speaker) has w = 0 with the rest and is shaped by its own
// level: a loud sound in one speaker no longer moves the gain of a quiet, unrelated one in its neighbour.
//
// "The same sound" is measured: w_cd comes from how alike the two signals have been over the last few tens of
// milliseconds,
//
//   rho^2 = <x_c x_d>^2 / (<x_c^2> <x_d^2>)        (< > a 25 ms average, the same for all three; 0 .. 1 by Cauchy-Schwarz)
//
// which is exactly 1 for one sound shared by any gains, and near 0 for unrelated sounds. w rises from 0 at
// rho^2 = 0.1 to 1 at 0.6 (so a pass with some reverb under it still counts whole, and chance likeness between
// unrelated sounds counts for nothing), is taken on a grid of 16 samples of the stage's own stream and glides
// between them. A speaker that has been silent counts as related (there is nothing in it to shape), so a sound
// starting from silence is whole from its first sample. Only the weights are averaged: the levels they weigh are
// the samples themselves, so the curve is still a curve of the waveform, the same 7th-order polynomial as in
// Stereo, and makes no harmonics higher than Stereo's does.
void MasterProcessor::inflateSides(float* const* channels, int numChannels, int offset, int numFrames)
{
    // The signed sums are only for the even part of the curve (the top of Inflate's travel).
    bool evenPart = false;
    for (int i = 0; i < numFrames; ++i)
        evenPart = evenPart || m_frameMu[i] > 0.0f;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        Channel& channel = m_channels[static_cast<size_t>(ch)];
        std::fill_n(channel.whole[0], numFrames, 0.0f);
        std::fill_n(channel.whole[1], numFrames, 0.0f);
        std::fill_n(channel.signedWhole[0], numFrames, 0.0f);
        std::fill_n(channel.signedWhole[1], numFrames, 0.0f);
    }

    // In runs that end on the weights' grid.
    for (int start = 0; start < numFrames;)
    {
        const int run = std::min(numFrames - start, c_alikeGrid - m_alikeCount);

        // Each speaker's own share of its side(s), and its power over this stretch of the grid. (The sums are
        // added up sample by sample in order, so they come out the same however the stream is cut into calls.)
        for (int ch = 0; ch < numChannels; ++ch)
        {
            Channel& channel = m_channels[static_cast<size_t>(ch)];
            const float* const data = channels[ch] + offset + start;
            const float left = channel.left, right = channel.right;
            float sum = channel.powerSum;
            for (int i = 0; i < run; ++i)
            {
                const float x = data[i];
                const float x2 = x * x;
                channel.square[i] = x2;
                sum += x2;
                channel.whole[0][start + i] += left * x2;
                channel.whole[1][start + i] += right * x2;
            }
            channel.powerSum = sum;
            if (evenPart)
                for (int i = 0; i < run; ++i)
                {
                    const float xa = data[i] * std::abs(data[i]);
                    channel.signedSquare[i] = xa;
                    channel.signedWhole[0][start + i] += left * xa;
                    channel.signedWhole[1][start + i] += right * xa;
                }
        }

        // What each speaker's neighbours carry of the same sound.
        for (Pair& pair : m_pairs)
        {
            if (pair.a >= numChannels || pair.b >= numChannels)
                continue;
            Channel& a = m_channels[static_cast<size_t>(pair.a)];
            Channel& b = m_channels[static_cast<size_t>(pair.b)];
            const float* const da = channels[pair.a] + offset + start;
            const float* const db = channels[pair.b] + offset + start;

            float sum = pair.crossSum;
            for (int i = 0; i < run; ++i)
                sum += da[i] * db[i];
            pair.crossSum = sum;

            const float weight = pair.weight, step = pair.step;
            pair.weight = weight + step * static_cast<float>(run);
            const bool steady = exactlyEqual(step, 0.0f); // (a weight at rest has a step of exactly nothing)
            if (steady && exactlyEqual(weight, 0.0f))
                continue; // nothing to do with each other: neither counts in the other's sound

            for (int side = 0; side < 2; ++side)
            {
                const float ma = side == 0 ? a.left : a.right, mb = side == 0 ? b.left : b.right;
                if (! (ma > 0.0f && mb > 0.0f))
                    continue;
                float* const wholeA = a.whole[side] + start;
                float* const wholeB = b.whole[side] + start;
                float* const signedA = a.signedWhole[side] + start;
                float* const signedB = b.signedWhole[side] + start;
                if (steady)
                {
                    // At rest (the same sound, or the same sound inverted): one weight for the whole run.
                    const float size = std::abs(weight);
                    const float toA = size * mb, toB = size * ma;
                    for (int i = 0; i < run; ++i)
                    {
                        wholeA[i] += toA * b.square[i];
                        wholeB[i] += toB * a.square[i];
                    }
                    if (evenPart)
                    {
                        const float signedToA = weight * mb, signedToB = weight * ma;
                        for (int i = 0; i < run; ++i)
                        {
                            signedA[i] += signedToA * b.signedSquare[i];
                            signedB[i] += signedToB * a.signedSquare[i];
                        }
                    }
                    continue;
                }
                float w = weight;
                for (int i = 0; i < run; ++i)
                {
                    w += step;
                    const float size = std::abs(w);
                    wholeA[i] += size * mb * b.square[i];
                    wholeB[i] += size * ma * a.square[i];
                    if (evenPart)
                    {
                        signedA[i] += w * mb * b.signedSquare[i];
                        signedB[i] += w * ma * a.signedSquare[i];
                    }
                }
            }
        }

        start += run;
        m_alikeCount += run;
        if (m_alikeCount >= c_alikeGrid)
        {
            // On the grid: the averages take in this stretch (all three kinds with the same weights, so the
            // likeness below is never over 1), and each pair's weight is told where to be a grid from now.
            m_alikeCount = 0;
            const float smooth = m_alikeCoeff, scale = 1.0f / static_cast<float>(c_alikeGrid);
            for (int ch = 0; ch < numChannels; ++ch)
            {
                Channel& channel = m_channels[static_cast<size_t>(ch)];
                const float mean = channel.powerSum * scale;
                channel.power = mean == mean ? channel.power + smooth * (mean - channel.power) : 0.0f; // (a NaN does not stay in the average)
                channel.powerSum = 0.0f;
            }
            for (Pair& pair : m_pairs)
            {
                const float mean = pair.crossSum * scale;
                pair.cross = mean == mean ? pair.cross + smooth * (mean - pair.cross) : 0.0f;
                pair.crossSum = 0.0f;

                const float pa = m_channels[static_cast<size_t>(pair.a)].power, pb = m_channels[static_cast<size_t>(pair.b)].power;
                float to = 1.0f; // one of them silent: nothing to tell apart
                if (pa > c_alikeFloor && pb > c_alikeFloor)
                {
                    const float alike = pair.cross * pair.cross / (pa * pb);
                    to = std::clamp((alike - c_alikeFrom) / (c_alikeTo - c_alikeFrom), 0.0f, 1.0f);
                    if (pair.cross < 0.0f)
                        to = -to; // the same sound the other way up
                }
                pair.weight = pair.target; // (lands exactly: no rounding carried from glide to glide)
                pair.target = to;
                pair.step = (to - pair.weight) / static_cast<float>(c_alikeGrid);
            }
        }
    }

    for (int ch = 0; ch < numChannels; ++ch)
    {
        Channel& channel = m_channels[static_cast<size_t>(ch)];
        float* const data = channels[ch] + offset;
        const float left = channel.left, right = channel.right;
        float dcIn = channel.dcIn;
        float dcOut = channel.dcOut;
        const float dcCoeff = m_dcCoeff;

        for (int i = 0; i < numFrames; ++i)
        {
            float odd = 0.0f, even = 0.0f;
            for (int side = 0; side < 2; ++side)
            {
                const float share = side == 0 ? left : right;
                if (! (share > 0.0f))
                    continue;
                const float r2 = channel.whole[side][i];
                if (r2 < 1.0f) // false for NaN too
                {
                    const float w = (1.0f - r2) * (1.0f - r2);
                    odd += share * w * (m_frameAlpha[i] + m_frameBeta[i] * r2);
                    if (m_frameMu[i] > 0.0f && r2 > 1.0e-20f)
                        even += share * w * m_frameMu[i] * channel.signedWhole[side][i] / std::sqrt(r2);
                }
            }

            const float x = data[i];
            const float o = x * odd;
            float e = x * even;
            if (! (e == e))
                e = 0.0f; // a NaN sample stays out of the coupling filter

            const float evenAc = e - dcIn + dcCoeff * dcOut;
            dcIn = e;
            dcOut = evenAc;

            data[i] = x + m_frameEffect[i] * (o + evenAc);
        }

        if (std::abs(dcOut) < 1.0e-20f)
            dcOut = 0.0f;

        channel.dcIn = dcIn;
        channel.dcOut = dcOut;
    }
}

// The limiter's reading with sides: the true peak of each side's speakers summed by power. Each of the four
// readings (the sample and the three points after it) is taken per speaker and summed by power per side, so the
// side's level is followed between the samples as well as at them. A centre speaker's own peak is read too (it
// counts only half to each side).
void MasterProcessor::pushAndDetectSides(float* data, Channel& channel, int numFrames)
{
    float points[c_blockSize][4], next[c_blockSize];
    readChannel(data, channel, numFrames, points, next);

    const float left = channel.left, right = channel.right;
    const bool centre = left > 0.0f && right > 0.0f;
    float last = channel.lastPoint;

    for (int i = 0; i < numFrames; ++i)
    {
        for (int k = 0; k < 4; ++k)
        {
            const float p2 = points[i][k] * points[i][k];
            m_frameSidePower[0][i][k] += left * p2;
            m_frameSidePower[1][i][k] += right * p2;
        }
        m_frameSidePower[0][i][4] += left * next[i] * next[i];
        m_frameSidePower[1][i][4] += right * next[i] * next[i];
        if (centre)
        {
            const float peak = peakOf(last, points[i], next[i]);
            m_framePeak[i] = peak > m_framePeak[i] || peak != peak ? peak : m_framePeak[i];
        }
        last = points[i][3];
    }

    channel.lastPoint = last;
}

void MasterProcessor::finishSidePeaks(int numFrames)
{
    for (int side = 0; side < 2; ++side)
    {
        float last = m_sideLastPoint[side];
        for (int i = 0; i < numFrames; ++i)
        {
            // The side's level at the four readings and at the next sample (NaN and infinity pass through:
            // computeGain reads them as a huge peak), then the top between them as for one channel.
            float level[4];
            for (int k = 0; k < 4; ++k)
                level[k] = std::sqrt(m_frameSidePower[side][i][k]);
            const float peak = peakOf(last, level, std::sqrt(m_frameSidePower[side][i][4]));
            last = level[3];
            m_framePeak[i] = peak > m_framePeak[i] || peak != peak ? peak : m_framePeak[i];
        }
        m_sideLastPoint[side] = last;
    }
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

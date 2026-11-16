#include "Locator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace pitchflattener
{
namespace
{
// FFT correlation peaks at least this high are kept as candidates. Well under the accept figure:
// the float FFT's error is relative to a whole block's energy, so a quiet stretch next to a loud
// one can read a little low. Every candidate is computed exactly before it is accepted.
constexpr double kCandidate = 0.9;
// On the decimated copy (high rates) a match can be a sample off the grid, which lowers its
// correlation further for bright sounds.
constexpr double kCandidateDecimated = 0.75;
// A peak this high might pass the accept figure (which needs a correlation of 0.9995): it is
// computed exactly at once, so the search can stop there.
constexpr double kMaybeAccept = 0.998;
constexpr double kMaybeAcceptDecimated = 0.97;
// An exact residual this small is the same audio (a float gain change leaves about 1e-12): with
// an ordinary level the search stops there, as nothing can be a better answer.
constexpr double kIdentical = 1.0e-6;
// Among accepted places, ones whose residual is within this factor of the best count as equally
// good (a steady tone), and the earliest is taken.
constexpr double kAsGoodFactor = 10.0;
constexpr std::size_t kMaxCandidates = std::size_t { 1 } << 20;
constexpr int kTopCandidates = 8;

// A plain radix-2 complex FFT (decimation in time), split real and imaginary arrays. Twiddles
// for each stage are stored next to each other so the inner loop reads them in order.
class Fft
{
public:
    explicit Fft(int size) : m_size(size), m_rev(static_cast<std::size_t>(size)), m_cos(static_cast<std::size_t>(size)), m_sin(static_cast<std::size_t>(size))
    {
        int bits = 0;
        while ((1 << bits) < size)
            ++bits;
        for (int i = 0; i < size; ++i)
        {
            std::uint32_t r = 0;
            for (int b = 0; b < bits; ++b)
                if ((static_cast<std::uint32_t>(i) >> b) & 1u)
                    r |= 1u << (bits - 1 - b);
            m_rev[static_cast<std::size_t>(i)] = r;
        }
        constexpr double kPi = 3.14159265358979323846;
        for (int half = 1; half < size; half *= 2)
            for (int k = 0; k < half; ++k)
            {
                const double angle = -kPi * static_cast<double>(k) / static_cast<double>(half);
                m_cos[static_cast<std::size_t>(half - 1 + k)] = static_cast<float>(std::cos(angle));
                m_sin[static_cast<std::size_t>(half - 1 + k)] = static_cast<float>(std::sin(angle));
            }
    }

    /** Forward transform in place. The inverse (unscaled) is forward(im, re): swapping the parts conjugates. */
    void forward(float* re, float* im) const noexcept
    {
        const int n = m_size;
        for (int i = 0; i < n; ++i)
        {
            const int j = static_cast<int>(m_rev[static_cast<std::size_t>(i)]);
            if (j > i)
            {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (int half = 1; half < n; half *= 2)
        {
            const float* wc = m_cos.data() + (half - 1);
            const float* ws = m_sin.data() + (half - 1);
            for (int start = 0; start < n; start += 2 * half)
            {
                float* ar = re + start;
                float* ai = im + start;
                float* br = ar + half;
                float* bi = ai + half;
                for (int k = 0; k < half; ++k)
                {
                    const float tr = br[k] * wc[k] - bi[k] * ws[k];
                    const float ti = br[k] * ws[k] + bi[k] * wc[k];
                    br[k] = ar[k] - tr;
                    bi[k] = ai[k] - ti;
                    ar[k] += tr;
                    ai[k] += ti;
                }
            }
        }
    }

private:
    int m_size;
    std::vector<std::uint32_t> m_rev;
    std::vector<float> m_cos, m_sin;
};

/**
 * The normalised correlation of probe (P samples) at every start position [0, positions) of x
 * (which has at least P zeros after it), by FFT. Calls onPeak(position, value) at each local
 * maximum of at least threshold, in position order; onPeak returns true to stop. Returns false
 * when stopped (by onPeak or cancelled).
 */
template <typename OnPeak>
bool scan(const float* x, std::int64_t positions, const float* probe, int P, double threshold, const std::function<bool()>& cancelled, OnPeak&& onPeak)
{
    double probeEnergy = 0.0;
    for (int i = 0; i < P; ++i)
        probeEnergy += static_cast<double>(probe[i]) * probe[i];
    if (! (probeEnergy > 0.0))
        return true;

    const int M = P <= 8192 ? 32768 : 65536;
    const int V = M - P + 1;   // positions each block yields
    const std::int64_t padded = positions + P;
    const Fft fft(M);

    // The probe's spectrum, conjugated (correlation rather than convolution).
    std::vector<float> qr(static_cast<std::size_t>(M), 0.0f), qi(static_cast<std::size_t>(M), 0.0f);
    std::copy(probe, probe + P, qr.begin());
    fft.forward(qr.data(), qi.data());
    for (auto& v : qi)
        v = -v;
    std::vector<float> re(static_cast<std::size_t>(M)), im(static_cast<std::size_t>(M));

    // The energy of x under the probe, slid along one position at a time.
    double windowEnergy = 0.0;
    for (int i = 0; i < P; ++i)
        windowEnergy += static_cast<double>(x[i]) * x[i];
    const double energyFloor = static_cast<double>(P) * 1.0e-10;   // -100 dBFS: digital silence
    const double scale = 1.0 / static_cast<double>(M);

    double prevValue = -2.0, curValue = -2.0;
    std::int64_t curPosition = -1;
    for (std::int64_t k0 = 0; k0 < positions; k0 += 2 * static_cast<std::int64_t>(V))
    {
        if (cancelled && cancelled())
            return false;

        // Two blocks in one transform: this one in the real part, the next in the imaginary.
        for (int i = 0; i < M; ++i)
        {
            const std::int64_t a = k0 + i, b = k0 + V + i;
            re[static_cast<std::size_t>(i)] = a < padded ? x[a] : 0.0f;
            im[static_cast<std::size_t>(i)] = b < padded ? x[b] : 0.0f;
        }
        fft.forward(re.data(), im.data());
        for (int i = 0; i < M; ++i)
        {
            const std::size_t u = static_cast<std::size_t>(i);
            const float r = re[u] * qr[u] - im[u] * qi[u];
            const float j = re[u] * qi[u] + im[u] * qr[u];
            re[u] = r;
            im[u] = j;
        }
        fft.forward(im.data(), re.data());   // the inverse (see Fft::forward)

        for (int half = 0; half < 2; ++half)
        {
            const std::int64_t base = k0 + static_cast<std::int64_t>(half) * V;
            const float* r = half == 0 ? re.data() : im.data();
            for (int k = 0; k < V; ++k)
            {
                const std::int64_t s = base + k;
                if (s >= positions)
                    break;
                const double value = windowEnergy > energyFloor ? static_cast<double>(r[k]) * scale / std::sqrt(windowEnergy * probeEnergy) : 0.0;
                const double out = x[s], in = x[s + P];
                windowEnergy += in * in - out * out;
                if (windowEnergy < 0.0)
                    windowEnergy = 0.0;

                if (curPosition >= 0 && curValue >= threshold && curValue >= prevValue && curValue > value)
                    if (onPeak(curPosition, curValue))
                        return false;
                prevValue = curValue;
                curValue = value;
                curPosition = s;
            }
        }
    }
    if (curPosition >= 0 && curValue >= threshold && curValue >= prevValue)
        if (onPeak(curPosition, curValue))
            return false;
    return true;
}

struct Candidate
{
    std::int64_t position;
    float fast;            // from the FFT
    double residual;       // < 0: not computed yet
    double gain;
    double gainEnd;
};
} // namespace

int Locator::nominalProbeLength(double sampleRate) noexcept
{
    // 4096 samples at 44.1 and 48 kHz; the same length of time (about 85 ms) at higher rates,
    // so the probe holds as many cycles of a sound whatever the rate.
    int length = 4096;
    while (sampleRate > 0.0 && static_cast<double>(length) / sampleRate < 0.08 && length < 16384)
        length *= 2;
    return length;
}

void Locator::prepare(const float* mono, std::int64_t numSamples, double sampleRate)
{
    m_sampleRate = sampleRate;
    m_length = std::max<std::int64_t>(0, numSamples);
    m_probeLength = static_cast<int>(std::min<std::int64_t>(nominalProbeLength(sampleRate), m_length));
    // The zeros after the sound let a probe that runs past its end (a short sound followed by
    // silence) be compared without bounds checks.
    m_samples.assign(static_cast<std::size_t>(m_length + m_probeLength), 0.0f);
    for (std::int64_t i = 0; i < m_length; ++i)
        m_samples[static_cast<std::size_t>(i)] = std::isfinite(mono[i]) ? mono[i] : 0.0f;

    // The loudest probe-length stretch, for the ambiguity rule.
    m_loudestEnergy = 0.0;
    double energy = 0.0;
    for (std::int64_t i = 0; i < m_length + m_probeLength; ++i)
    {
        const double v = m_samples[static_cast<std::size_t>(i)];
        energy += v * v;
        if (i >= m_probeLength)
        {
            const double old = m_samples[static_cast<std::size_t>(i - m_probeLength)];
            energy -= old * old;
        }
        m_loudestEnergy = std::max(m_loudestEnergy, energy);
    }

    // At the highest rates the FFT pass runs on the sound averaged in fours (see the header).
    m_decimation = sampleRate >= 150000.0 && m_probeLength >= 4096 ? 4 : 1;
    m_decimated.clear();
    if (m_decimation > 1)
    {
        const std::int64_t n = m_length / m_decimation + 1 + m_probeLength / m_decimation + 1;
        m_decimated.assign(static_cast<std::size_t>(n), 0.0f);
        for (std::int64_t k = 0; k * m_decimation < m_length; ++k)
        {
            float sum = 0.0f;
            for (int j = 0; j < m_decimation; ++j)
                sum += m_samples[static_cast<std::size_t>(k * m_decimation + j)];
            m_decimated[static_cast<std::size_t>(k)] = sum / static_cast<float>(m_decimation);
        }
    }
}

double Locator::residualAt(const float* probe, int length, std::int64_t position, double* gainStart, double* gainEnd) const noexcept
{
    if (gainStart != nullptr)
        *gainStart = 0.0;
    if (gainEnd != nullptr)
        *gainEnd = 0.0;
    if (position < 0 || length <= 0 || position + length > m_length + m_probeLength)
        return 1.0;

    // Least squares for x = (a + b * t) * y, t running from -1/2 to 1/2 across the probe: a level
    // that changes in a straight line. Over 85 ms that fits any fade a player makes; it cannot
    // make another take, pitch or speed fit, because those differ in the waveform, not the level.
    const float* y = m_samples.data() + position;
    const double mid = 0.5 * static_cast<double>(length - 1), span = static_cast<double>(length);
    double xx = 0.0, yy = 0.0, tyy = 0.0, ttyy = 0.0, xy = 0.0, txy = 0.0;
    for (int i = 0; i < length; ++i)
    {
        const double a = probe[i], b = y[i], t = (static_cast<double>(i) - mid) / span;
        xx += a * a;
        yy += b * b;
        tyy += t * b * b;
        ttyy += t * t * b * b;
        xy += a * b;
        txy += t * a * b;
    }
    if (! (xx > 0.0) || ! (yy > 0.0))
        return 1.0;
    double a = xy / yy, b = 0.0;
    const double det = yy * ttyy - tyy * tyy;
    if (det > 1.0e-9 * yy * ttyy)
    {
        a = (xy * ttyy - txy * tyy) / det;
        b = (txy * yy - xy * tyy) / det;
    }
    const double first = a - 0.5 * b, last = a + 0.5 * b;
    if (gainStart != nullptr)
        *gainStart = first;
    if (gainEnd != nullptr)
        *gainEnd = last;
    // An inverted copy (a falling below zero anywhere) is not the learned recording.
    if (! (first > 0.0) || ! (last > 0.0))
        return 1.0;
    const double left = (xx - (a * xy + b * txy)) / xx;
    return left > 0.0 ? std::min(1.0, left) : 0.0;
}

Locator::Match Locator::locate(const float* probe, std::int64_t preferFrom, const std::function<bool()>& cancelled) const
{
    Match match;
    const int P = m_probeLength;
    if (P <= 0 || probe == nullptr)
        return match;

    double probeEnergy = 0.0;
    for (int i = 0; i < P; ++i)
        probeEnergy += static_cast<double>(probe[i]) * probe[i];
    // A silent probe matches every silent stretch equally: there is nothing to find. A probe with
    // a bad sample in it cannot be measured.
    if (! std::isfinite(probeEnergy) || probeEnergy < static_cast<double>(P) * kSilence * kSilence)
        return match;
    const bool loudEnough = probeEnergy >= matching::kAmbiguousLevel * m_loudestEnergy;

    std::vector<Candidate> candidates;
    candidates.reserve(1024);
    const auto finish = [&](const Candidate& c)
    {
        match.found = true;
        match.sourceSample = c.position;
        match.residual = c.residual;
        match.correlation = std::sqrt(std::max(0.0, 1.0 - c.residual));
        match.gain = c.gain;
        match.gainEnd = c.gainEnd;
        return match;
    };

    // The exact residual at a candidate; on the decimated grid, the best of the positions it can be.
    const int D = m_decimation;
    const auto exact = [&](Candidate& c)
    {
        if (c.residual >= 0.0)
            return;
        if (D == 1)
        {
            c.residual = residualAt(probe, P, c.position, &c.gain, &c.gainEnd);
            ++match.candidates;
            return;
        }
        double bestResidual = 2.0, bestGain = 0.0, bestGainEnd = 0.0;
        std::int64_t bestPosition = c.position;
        for (std::int64_t s = c.position - D / 2; s <= c.position + D / 2; ++s)
        {
            if (s < 0 || s >= m_length)
                continue;
            double g = 0.0, ge = 0.0;
            const double r = residualAt(probe, P, s, &g, &ge);
            ++match.candidates;
            if (r < bestResidual)
            {
                bestResidual = r;
                bestGain = g;
                bestGainEnd = ge;
                bestPosition = s;
            }
        }
        c.position = bestPosition;
        c.residual = std::min(1.0, bestResidual);
        c.gain = bestGain;
        c.gainEnd = bestGainEnd;
    };

    std::int64_t stopAt = -1;
    Candidate early {};
    const auto onPeak = [&](std::int64_t position, double value, double maybe) -> bool
    {
        Candidate c { position, static_cast<float>(value), -1.0, 0.0, 0.0 };
        if (value >= maybe)
        {
            exact(c);
            // The same audio at an ordinary level: nothing can be a better answer.
            if (c.residual <= kIdentical && loudEnough && c.position >= preferFrom)
            {
                early = c;
                stopAt = c.position;
                return true;
            }
        }
        if (candidates.size() < kMaxCandidates)
            candidates.push_back(c);
        return false;
    };

    if (D == 1)
    {
        if (! scan(m_samples.data(), m_length, probe, P, kCandidate, cancelled,
                   [&](std::int64_t s, double v) { return onPeak(s, v, kMaybeAccept); }))
        {
            if (stopAt >= 0)
                return finish(early);
            return match;   // cancelled
        }
    }
    else
    {
        // Two of the four sample phases of the probe against the decimated sound: a probe
        // averaged in fours from sample phase, matched at decimated position k, starts at
        // k * D - phase in the sound.
        const int Pd = P / D - 1;
        std::vector<float> reduced(static_cast<std::size_t>(Pd));
        const std::int64_t positions = m_length / D + 1;
        for (const int phase : { 0, D / 2 })
        {
            for (int j = 0; j < Pd; ++j)
            {
                float sum = 0.0f;
                for (int i = 0; i < D; ++i)
                    sum += probe[phase + j * D + i];
                reduced[static_cast<std::size_t>(j)] = sum / static_cast<float>(D);
            }
            if (! scan(m_decimated.data(), positions, reduced.data(), Pd, kCandidateDecimated, cancelled,
                       [&](std::int64_t k, double v) { return onPeak(std::max<std::int64_t>(0, k * D - phase), v, kMaybeAcceptDecimated); }))
            {
                if (stopAt >= 0)
                    return finish(early);
                return match;
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.position < b.position; });
    }

    match.searchedAll = true;
    if (candidates.empty())
        return match;

    // The best exact residual: the FFT's top few, computed exactly (its ranking is nearly right).
    std::vector<std::size_t> order(candidates.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    const std::size_t top = std::min<std::size_t>(kTopCandidates, order.size());
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(top), order.end(),
                      [&](std::size_t a, std::size_t b) { return candidates[a].fast > candidates[b].fast; });
    double best = 2.0;
    int accepted = 0;
    std::vector<std::int64_t> acceptedPositions;
    for (std::size_t i = 0; i < top; ++i)
    {
        auto& c = candidates[order[i]];
        exact(c);
        best = std::min(best, c.residual);
        if (c.residual <= matching::kAcceptResidual
            && std::none_of(acceptedPositions.begin(), acceptedPositions.end(), [&](std::int64_t p) { return std::abs(p - c.position) <= D; }))
        {
            acceptedPositions.push_back(c.position);
            ++accepted;
        }
    }
    match.residual = best;
    match.correlation = std::sqrt(std::max(0.0, 1.0 - std::min(1.0, best)));
    if (best > matching::kAcceptResidual)
        return match;

    // Matched at several places: only evidence when it is loud enough (see the header).
    match.ambiguous = accepted >= 2;
    if (match.ambiguous && ! loudEnough)
    {
        match.tooQuiet = true;
        return match;
    }

    // The earliest place as good as the best: from preferFrom on if any is, else from the start.
    const double wanted = std::min(matching::kAcceptResidual, std::max(best * kAsGoodFactor, 1.0e-9));
    const double fastWanted = static_cast<double>(candidates[order[0]].fast) - (D == 1 ? 0.002 : 0.05);
    for (int pass = 0; pass < 2; ++pass)
        for (auto& c : candidates)
        {
            if ((pass == 0 && c.position < preferFrom) || static_cast<double>(c.fast) < fastWanted)
                continue;
            exact(c);
            if (c.residual <= wanted)
                return finish(c);
        }
    for (std::size_t i = 0; i < top; ++i)
        if (candidates[order[i]].residual <= best)
            return finish(candidates[order[i]]);
    return match;
}

} // namespace pitchflattener

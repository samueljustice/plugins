#include "Locator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace pitchflattener
{
namespace
{
// Peaks of the FFT correlation at least this high are kept as candidates. Well under kAccept: the
// float FFT's error is relative to a whole block's energy, so a quiet stretch next to a loud one
// can read a little low. Every candidate is recomputed exactly before it is accepted.
constexpr double kCandidate = 0.9;
// An exact correlation this close to 1 is the same audio (a float gain change leaves it within
// about 1e-7 of 1): the search stops there.
constexpr double kIdentical = 0.999999;
// Among positions that all match, ones within this of the best count as equally good, and the
// earliest is taken (a steady tone).
constexpr double kAsGood = 1.0e-4;
constexpr std::size_t kMaxCandidates = std::size_t { 1 } << 20;

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

struct Candidate
{
    std::int64_t position;
    float fast;          // from the FFT
    double exact;        // < -1: not computed yet
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
    if (m_length > 0)
        std::copy(mono, mono + m_length, m_samples.begin());
}

double Locator::correlationAt(const float* probe, int length, std::int64_t position, double* gain) const noexcept
{
    if (position < 0 || length <= 0 || position + length > m_length + m_probeLength)
        return 0.0;
    const float* x = m_samples.data() + position;
    double xy = 0.0, xx = 0.0, yy = 0.0;
    for (int i = 0; i < length; ++i)
    {
        const double a = probe[i], b = x[i];
        xy += a * b;
        xx += b * b;
        yy += a * a;
    }
    if (gain != nullptr)
        *gain = xx > 0.0 ? std::sqrt(yy / xx) : 0.0;
    return xx > 0.0 && yy > 0.0 ? xy / std::sqrt(xx * yy) : 0.0;
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
    // A silent probe matches every silent stretch equally: there is nothing to find.
    if (probeEnergy < static_cast<double>(P) * kSilence * kSilence)
        return match;

    const int M = P <= 8192 ? 32768 : 65536;
    const int V = M - P + 1;                       // positions each block yields
    const std::int64_t positions = m_length;       // every start inside the sound
    const std::int64_t padded = m_length + P;
    const Fft fft(M);

    // The probe's spectrum, conjugated (correlation rather than convolution).
    std::vector<float> qr(static_cast<std::size_t>(M), 0.0f), qi(static_cast<std::size_t>(M), 0.0f);
    std::copy(probe, probe + P, qr.begin());
    fft.forward(qr.data(), qi.data());
    for (auto& v : qi)
        v = -v;

    std::vector<float> re(static_cast<std::size_t>(M)), im(static_cast<std::size_t>(M));
    std::vector<Candidate> candidates;
    candidates.reserve(1024);

    const float* x = m_samples.data();
    // The energy of the learned sound under the probe, slid along one position at a time.
    double windowEnergy = 0.0;
    for (int i = 0; i < P; ++i)
        windowEnergy += static_cast<double>(x[i]) * x[i];
    const double energyFloor = static_cast<double>(P) * 1.0e-10;   // -100 dBFS: digital silence
    const double scale = 1.0 / static_cast<double>(M);

    // A tiny peak finder over the stream of correlations, in position order.
    double prevValue = -2.0, curValue = -2.0;
    std::int64_t curPosition = -1;
    double bestFast = -1.0;

    const auto exactAt = [&](std::int64_t position) { return correlationAt(probe, P, position); };

    for (std::int64_t k0 = 0; k0 < positions; k0 += 2 * static_cast<std::int64_t>(V))
    {
        if (cancelled && cancelled())
            return match;

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
                // Slide the window on to s + 1.
                const double out = x[s], in = x[s + P];
                windowEnergy += in * in - out * out;
                if (windowEnergy < 0.0)
                    windowEnergy = 0.0;

                if (curPosition >= 0 && curValue >= kCandidate && curValue >= prevValue && curValue > value)
                {
                    bestFast = std::max(bestFast, curValue);
                    Candidate c { curPosition, static_cast<float>(curValue), -2.0 };
                    if (curValue >= kIdentical - 1.0e-3)
                    {
                        c.exact = exactAt(curPosition);
                        ++match.candidates;
                        if (c.exact >= kIdentical && curPosition >= preferFrom)
                        {
                            match.found = true;
                            match.sourceSample = curPosition;
                            match.correlation = correlationAt(probe, P, curPosition, &match.gain);
                            return match;
                        }
                    }
                    if (candidates.size() < kMaxCandidates)
                        candidates.push_back(c);
                }
                prevValue = curValue;
                curValue = value;
                curPosition = s;
            }
        }
    }
    // The last position can be a peak too.
    if (curPosition >= 0 && curValue >= kCandidate && curValue >= prevValue && candidates.size() < kMaxCandidates)
    {
        bestFast = std::max(bestFast, curValue);
        candidates.push_back({ curPosition, static_cast<float>(curValue), -2.0 });
    }
    match.searchedAll = true;
    if (candidates.empty())
    {
        match.correlation = std::max(0.0, bestFast);
        return match;
    }

    // The best exact correlation: the FFT's top few, recomputed (its ranking is nearly right).
    std::vector<std::size_t> order(candidates.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    const std::size_t top = std::min<std::size_t>(8, order.size());
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(top), order.end(),
                      [&](std::size_t a, std::size_t b) { return candidates[a].fast > candidates[b].fast; });
    double best = -1.0;
    for (std::size_t i = 0; i < top; ++i)
    {
        auto& c = candidates[order[i]];
        if (c.exact < -1.0)
        {
            c.exact = exactAt(c.position);
            ++match.candidates;
        }
        best = std::max(best, c.exact);
    }
    match.correlation = best;
    if (best < kAccept)
        return match;

    // The earliest position as good as the best: from preferFrom on if any is, else from the start.
    const double wanted = std::max(kAccept, best - kAsGood);
    const double fastWanted = static_cast<double>(candidates[order[0]].fast) - 0.002;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (auto& c : candidates)
        {
            if ((pass == 0 && c.position < preferFrom) || static_cast<double>(c.fast) < fastWanted)
                continue;
            if (c.exact < -1.0)
            {
                c.exact = exactAt(c.position);
                ++match.candidates;
            }
            if (c.exact >= wanted)
            {
                match.found = true;
                match.sourceSample = c.position;
                match.correlation = correlationAt(probe, P, c.position, &match.gain);
                return match;
            }
        }
    }
    return match;
}

} // namespace pitchflattener

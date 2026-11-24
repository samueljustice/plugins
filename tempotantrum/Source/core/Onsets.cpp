#include "Engine.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>

// Hit detection: spectral flux of a log-mel spectrogram in four frequency bands, peak picking,
// then each peak is moved back to the start of its attack.
//
// Every size is derived from milliseconds, so files at 44.1, 48 and 96 kHz are treated alike.
// At 48 kHz the numbers are: 1024-sample Hann window, 240-sample hop.
namespace tt
{
namespace
{

constexpr double hopSeconds = 0.005;            // one flux frame
constexpr double windowSeconds = 1024.0 / 48000.0;
constexpr double referenceBinHz = 48000.0 / 1024.0;
constexpr int numMels = 64;
constexpr double melFmin = 30.0;
constexpr double melFmax = 16000.0;
constexpr int fluxLag = 2;                      // frames: 10 ms
constexpr int localMeanFrames = 100;            // 0.5 s
constexpr int peakDistanceFrames = 6;           // 30 ms
constexpr double attackOffsetSeconds = 0.0024;  // the flux peak sits this far after the attack start
constexpr double attackAgreeSeconds = 0.006;    // the envelope's answer is used when it is this close to that
constexpr double refineWindowSeconds = 0.012;   // how far a hit may move onto the refine buffer's hit
constexpr double weakestStrength = 0.02;        // a peak this far below the strong hits is the noise floor, not a hit
constexpr double smallestFlux = 1.2;            // the least rise, before the bands are divided by their means, that is a hit
constexpr float quietestHit = 0.002f;           // a hit's level reaches at least this (-54 dBFS)

double hzToMel (double f)
{
    const double fSp = 200.0 / 3.0;
    const double minLogHz = 1000.0;
    const double minLogMel = minLogHz / fSp;
    const double logStep = std::log (6.4) / 27.0;
    return f >= minLogHz ? minLogMel + std::log (f / minLogHz) / logStep : f / fSp;
}

double melToHz (double m)
{
    const double fSp = 200.0 / 3.0;
    const double minLogHz = 1000.0;
    const double minLogMel = minLogHz / fSp;
    const double logStep = std::log (6.4) / 27.0;
    return m >= minLogMel ? minLogHz * std::exp (logStep * (m - minLogMel)) : m * fSp;
}

std::vector<double> melPoints (int count, double fmin, double fmax)
{
    std::vector<double> out ((size_t) count);
    const double a = hzToMel (fmin), b = hzToMel (fmax);
    for (int i = 0; i < count; ++i)
        out[(size_t) i] = melToHz (a + (b - a) * i / (double) (count - 1));
    return out;
}

// One triangular mel filter, stored as the run of FFT bins it covers.
struct MelFilter
{
    int firstBin = 0;
    std::vector<float> weights;
};

int reflectIndex (long long i, long long n)
{
    if (n <= 1)
        return 0;
    while (i < 0 || i >= n)
        i = i < 0 ? -i - 1 : 2 * n - i - 1;
    return (int) i;
}

// Mean over a window of `size` values centred on each value (edges mirrored).
std::vector<double> movingMean (const std::vector<double>& x, int size)
{
    const auto n = (long long) x.size();
    std::vector<double> out (x.size());
    if (n == 0)
        return out;
    const int before = size / 2, after = size - before - 1;
    double sum = 0.0;
    for (long long j = -before; j <= after; ++j)
        sum += x[(size_t) reflectIndex (j, n)];
    for (long long i = 0; i < n; ++i)
    {
        out[(size_t) i] = sum / size;
        sum += x[(size_t) reflectIndex (i + after + 1, n)] - x[(size_t) reflectIndex (i - before, n)];
    }
    return out;
}

double medianOf (std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    const auto mid = v.size() / 2;
    std::nth_element (v.begin(), v.begin() + (long) mid, v.end());
    double m = v[mid];
    if (v.size() % 2 == 0)
        m = 0.5 * (m + *std::max_element (v.begin(), v.begin() + (long) mid));
    return m;
}

double percentileOf (std::vector<double> v, double pct)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const double pos = pct / 100.0 * (double) (v.size() - 1);
    const auto lo = (size_t) std::floor (pos);
    const auto hi = std::min (v.size() - 1, lo + 1);
    return v[lo] + (v[hi] - v[lo]) * (pos - (double) lo);
}

// The flux curve, one value every 5 ms: each band divided by its own mean, then the four added.
// `raw` is the same sum without the division, for an absolute test.
struct Flux
{
    std::vector<double> flux, raw;
};

Flux computeFlux (const float* mono, std::size_t numSamples, double sampleRate)
{
    const int window = std::max (16, (int) std::lround (windowSeconds * sampleRate));
    int order = 4;
    while ((1 << order) < window)
        ++order;
    const int fftSize = 1 << order;
    const int numBins = fftSize / 2 + 1;
    const double binHz = sampleRate / fftSize;
    const double hop = hopSeconds * sampleRate;
    const int numFrames = 1 + (int) std::floor ((double) numSamples / hop);

    // Scaled so a steady tone gives the same level whatever the window length.
    std::vector<float> hann ((size_t) window);
    const float magnitudeScale = (float) (1024.0 / window);
    for (int i = 0; i < window; ++i)
        hann[(size_t) i] = magnitudeScale * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / window));

    // Mel filters (area-normalised triangles), and the band each one belongs to.
    const double fmax = std::min (melFmax, 0.5 * sampleRate);
    const auto edges = melPoints (numMels + 2, melFmin, fmax);
    const auto centres = melPoints (numMels, melFmin, fmax);
    const double densityScale = binHz / referenceBinHz;
    std::vector<MelFilter> filters ((size_t) numMels);
    std::vector<int> bandOf ((size_t) numMels);
    for (int m = 0; m < numMels; ++m)
    {
        const double lo = edges[(size_t) m], mid = edges[(size_t) m + 1], hi = edges[(size_t) m + 2];
        const double norm = 2.0 / (hi - lo) * densityScale;
        auto& f = filters[(size_t) m];
        f.firstBin = std::max (0, (int) std::ceil (lo / binHz));
        for (int k = f.firstBin; k < numBins && k * binHz < hi; ++k)
        {
            const double hz = k * binHz;
            const double w = std::max (0.0, std::min ((hz - lo) / (mid - lo), (hi - hz) / (hi - mid)));
            f.weights.push_back ((float) (w * norm));
        }
        const double c = centres[(size_t) m];
        bandOf[(size_t) m] = c < 150.0 ? 0 : (c < 800.0 ? 1 : (c < 4000.0 ? 2 : 3));
    }

    juce::dsp::FFT fft (order);
    std::vector<float> buffer ((size_t) fftSize * 2);
    std::vector<float> history ((size_t) numMels * (fluxLag + 1), 0.0f);   // ring of the last log-mel frames
    std::vector<double> band[4];
    for (auto& b : band)
        b.assign ((size_t) numFrames, 0.0);

    for (int t = 0; t < numFrames; ++t)
    {
        const long long start = std::llround (t * hop) - window / 2;
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        const long long a = std::max (0LL, start), b = std::min ((long long) numSamples, start + window);
        for (long long i = a; i < b; ++i)
            buffer[(size_t) (i - start)] = mono[i] * hann[(size_t) (i - start)];
        fft.performFrequencyOnlyForwardTransform (buffer.data(), true);
        for (int k = 0; k < numBins; ++k)
            buffer[(size_t) k] *= buffer[(size_t) k];

        float* now = history.data() + (size_t) (t % (fluxLag + 1)) * numMels;
        const float* then = history.data() + (size_t) ((t + 1) % (fluxLag + 1)) * numMels;   // two frames back
        for (int m = 0; m < numMels; ++m)
        {
            const auto& f = filters[(size_t) m];
            float sum = 0.0f;
            const float* p = buffer.data() + f.firstBin;
            for (size_t k = 0; k < f.weights.size(); ++k)
                sum += f.weights[k] * p[k];
            const float value = std::log1p (100.0f * std::sqrt (sum));
            if (t >= fluxLag)
                band[bandOf[(size_t) m]][(size_t) t] += std::max (0.0f, value - then[m]);
            now[m] = value;
        }
    }

    Flux out;
    out.flux.assign ((size_t) numFrames, 0.0);
    out.raw.assign ((size_t) numFrames, 0.0);
    for (auto& b : band)
    {
        const double mean = std::accumulate (b.begin(), b.end(), 0.0) / std::max (1, numFrames);
        for (int t = 0; t < numFrames; ++t)
        {
            out.flux[(size_t) t] += b[(size_t) t] / (mean + 1.0e-9);
            out.raw[(size_t) t] += b[(size_t) t];
        }
    }
    return out;
}

// Local maxima at least `distance` apart, the higher one winning.
std::vector<int> pickPeaks (const std::vector<double>& x, int distance)
{
    std::vector<int> peaks;
    const int n = (int) x.size();
    for (int i = 1; i < n - 1;)
    {
        if (x[(size_t) i - 1] < x[(size_t) i])
        {
            int ahead = i + 1;
            while (ahead < n - 1 && juce::exactlyEqual (x[(size_t) ahead], x[(size_t) i]))
                ++ahead;
            if (x[(size_t) ahead] < x[(size_t) i])
            {
                peaks.push_back ((i + ahead - 1) / 2);
                i = ahead;
                continue;
            }
        }
        ++i;
    }

    std::vector<int> byHeight (peaks.size());
    std::iota (byHeight.begin(), byHeight.end(), 0);
    std::stable_sort (byHeight.begin(), byHeight.end(), [&] (int a, int b) { return x[(size_t) peaks[(size_t) a]] > x[(size_t) peaks[(size_t) b]]; });
    std::vector<char> keep (peaks.size(), 1);
    for (int j : byHeight)
    {
        if (! keep[(size_t) j])
            continue;
        for (int k = j - 1; k >= 0 && peaks[(size_t) j] - peaks[(size_t) k] < distance; --k)
            keep[(size_t) k] = 0;
        for (int k = j + 1; k < (int) peaks.size() && peaks[(size_t) k] - peaks[(size_t) j] < distance; ++k)
            keep[(size_t) k] = 0;
    }
    std::vector<int> out;
    for (size_t i = 0; i < peaks.size(); ++i)
        if (keep[i])
            out.push_back (peaks[i]);
    return out;
}

// The level envelope for samples [a, b): a 1 ms peak-hold of the rectified signal, smoothed over
// 0.5 ms. Past either end of the audio the values mirror. Only the stretch asked for is worked
// out, so the memory used does not grow with the length of the file.
std::vector<float> envelope (const float* mono, long long n, double sampleRate, long long a, long long b)
{
    std::vector<float> out ((size_t) std::max (0LL, b - a));
    if (n <= 0 || out.empty())
        return out;

    const int hold = std::max (2, (int) std::lround (0.001 * sampleRate));
    const int smooth = std::max (2, (int) std::lround (0.0005 * sampleRate));

    // The held values the smoothing reads: those around [a, b), and those near an end of the
    // audio that the mirroring lands on.
    const long long c0 = std::max (0LL, std::min (a - smooth, n - 2LL * smooth));
    const long long c1 = std::min (n, std::max (b + smooth, 2LL * smooth));
    std::vector<float> held ((size_t) (c1 - c0));
    {
        const int before = hold / 2, after = hold - before - 1;
        std::deque<long long> q;   // positions of candidates for the maximum, values falling
        auto value = [&] (long long j) { return std::abs (mono[reflectIndex (j, n)]); };
        long long next = c0 - before;
        for (long long i = c0; i < c1; ++i)
        {
            for (; next <= i + after; ++next)
            {
                const float v = value (next);
                while (! q.empty() && value (q.back()) <= v)
                    q.pop_back();
                q.push_back (next);
            }
            while (q.front() < i - before)
                q.pop_front();
            held[(size_t) (i - c0)] = value (q.front());
        }
    }
    {
        const int before = smooth / 2, after = smooth - before - 1;
        auto heldAt = [&] (long long j) { return (double) held[(size_t) (reflectIndex (j, n) - c0)]; };
        double sum = 0.0;
        for (long long j = a - before; j <= a + after; ++j)
            sum += heldAt (j);
        for (long long i = a; i < b; ++i)
        {
            out[(size_t) (i - a)] = (float) (sum / smooth);
            sum += heldAt (i + after + 1) - heldAt (i - before);
        }
    }
    return out;
}

// The attack that produced the flux peak at sample `p`.
struct Attack
{
    double start = 0.0;   // in samples
    float peak = 0.0f;    // the envelope's highest value around the flux peak
};

// The attack starts at the last point before the envelope's local maximum where it is still
// within a tenth of the way up from the level before the hit.
Attack findAttack (const float* mono, long long n, long long p, double sampleRate)
{
    auto ms = [&] (double x) { return (long long) std::lround (x * 0.001 * sampleRate); };
    const long long a = std::max (0LL, p - ms (12)), b = std::min (n, p + ms (25));
    if (a >= b)
        return { (double) p, 0.0f };

    // Everything read below lies in [first, b).
    const long long first = std::max (0LL, p - ms (60));
    const auto env = envelope (mono, n, sampleRate, first, b);
    auto at = [&] (long long i) { return env[(size_t) (i - first)]; };

    const long long k = first + (std::max_element (env.begin() + (a - first), env.end()) - env.begin());
    const long long f1 = std::min (n, std::max (1LL, p - ms (30)));
    std::vector<double> before (env.begin(), env.begin() + std::max (0LL, std::min (b, f1) - first));
    const double floorLevel = medianOf (std::move (before));
    const long long lo = std::max (first, k - ms (30));
    const double threshold = floorLevel + 0.1 * (at (k) - floorLevel);
    for (long long i = k; i >= lo; --i)
        if (at (i) < threshold)
            return { (double) (i + 1), at (k) };
    return { (double) lo, at (k) };
}

std::vector<Onset> detect (const float* mono, std::size_t numSamples, double sampleRate)
{
    std::vector<Onset> out;
    if (mono == nullptr || numSamples < 64 || sampleRate <= 0.0)
        return out;

    const auto computed = computeFlux (mono, numSamples, sampleRate);
    const auto& flux = computed.flux;
    const auto localMean = movingMean (flux, localMeanFrames);
    const double median = medianOf (flux);

    std::vector<int> peaks;
    for (int p : pickPeaks (flux, peakDistanceFrames))
        if (flux[(size_t) p] > 1.3 * localMean[(size_t) p] + 0.3 * median)
            peaks.push_back (p);
    if (peaks.empty())
        return out;

    std::vector<double> heights;
    for (int p : peaks)
        heights.push_back (flux[(size_t) p]);
    const double reference = std::max (1.0e-12, percentileOf (heights, 90.0));

    const double hop = hopSeconds * sampleRate;
    for (int p : peaks)
    {
        // A peak the division by the band's mean has made out of nothing: a steady tone, or a noise floor.
        if (computed.raw[(size_t) p] < smallestFlux)
            continue;
        const long long sample = std::llround (p * hop);
        const auto attack = findAttack (mono, (long long) numSamples, sample, sampleRate);
        if (attack.peak < quietestHit)
            continue;
        const double byOffset = (double) sample - attackOffsetSeconds * sampleRate;
        const double start = std::abs (attack.start - byOffset) <= attackAgreeSeconds * sampleRate ? attack.start : byOffset;
        Onset o;
        o.time = start / sampleRate;
        o.strength = (float) (std::clamp (flux[(size_t) p] / reference, 0.0, 1.5) / 1.5);
        if (o.strength >= weakestStrength)
            out.push_back (o);
    }
    std::stable_sort (out.begin(), out.end(), [] (const Onset& a, const Onset& b) { return a.time < b.time; });
    return out;
}

} // namespace

std::vector<Onset> findOnsets (const float* mono, std::size_t numSamples, double sampleRate, const float* refineOn)
{
    auto onsets = detect (mono, numSamples, sampleRate);
    if (refineOn == nullptr || onsets.empty())
        return onsets;

    const auto target = detect (refineOn, numSamples, sampleRate);
    if (target.empty())
        return onsets;

    for (auto& o : onsets)
    {
        auto it = std::lower_bound (target.begin(), target.end(), o.time, [] (const Onset& a, double t) { return a.time < t; });
        double best = refineWindowSeconds, moved = o.time;
        for (auto c : { it, it == target.begin() ? it : it - 1 })
            if (c != target.end() && std::abs (c->time - o.time) <= best)
            {
                best = std::abs (c->time - o.time);
                moved = c->time;
            }
        o.time = moved;
    }
    std::stable_sort (onsets.begin(), onsets.end(), [] (const Onset& a, const Onset& b) { return a.time < b.time; });

    // Two hits moved onto the same place: keep the stronger.
    std::vector<Onset> merged;
    for (const auto& o : onsets)
    {
        if (! merged.empty() && o.time - merged.back().time < 1.0e-6)
            merged.back().strength = std::max (merged.back().strength, o.strength);
        else
            merged.push_back (o);
    }
    return merged;
}

} // namespace tt

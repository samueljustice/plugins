#include "RidgeTracker.h"
#include "HarmonicTracker.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>

namespace pitchflattener::core
{

namespace
{
void fft(std::vector<std::complex<float>>& a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const float ang = -2.0f * 3.14159265358979f / static_cast<float>(len);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len)
        {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl;
            }
        }
    }
}

// The partials standing out of one frame, as seen through one window length.
struct RidgePeak { float midi, hz, score; };

struct RidgeTuning
{
    double shortMs = 10.0, longMs = 32.0;   // a fast sweep needs the short window, a steady or noisy tone the long one
    double floorSpan = 2.0;                 // the floor a peak is measured against: the mean level an octave either side
    float minProminence = 10.0f;            // dB above that floor
    float promWeight = 0.0f, base = 14.0f;  // a peak scores base + promWeight * its prominence ...
    float relative = 1.0f;                  // ... less this much per dB below the frame's strongest peak
    float none = 8.0f;                      // what a frame with nothing followed is worth
    float move = 10.0f, freeMove = 0.9f;    // cost per semitone moved between frames, beyond this much for free
    float change = 6.0f;                    // cost of starting or stopping
};

constexpr int kPeaksPerWindow = 5;

// Adds one window length's peaks to every frame's list.
void addPeaks(const float* mono, int numSamples, double rate, int hopN, int F, double windowMs, double loHz, double hiHz, const RidgeTuning& t,
              std::vector<std::vector<RidgePeak>>& peaks, std::vector<float>* frameDb)
{
    const int win = std::max(16, static_cast<int>(std::lround(windowMs * 0.001 * rate)));
    std::size_t N = 1; while (N < static_cast<std::size_t>(win) * 4) N <<= 1;
    const double binHz = rate / static_cast<double>(N);
    // A window can not tell partials apart below about two of its own main lobes.
    const double lowest = std::max(loHz, 2.5 * rate / win);
    const int loBin = std::max(2, static_cast<int>(lowest / binHz)), hiBin = std::min(static_cast<int>(N / 2) - 2, static_cast<int>(std::min(hiHz, rate * 0.45) / binHz));
    if (hiBin <= loBin) return;
    std::vector<std::complex<float>> buf(N);
    std::vector<float> db(N / 2), window(static_cast<std::size_t>(win));
    std::vector<double> cum(N / 2 + 1, 0.0);
    for (int i = 0; i < win; ++i) window[static_cast<std::size_t>(i)] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * static_cast<float>(i) / static_cast<float>(win - 1));
    for (int f = 0; f < F; ++f)
    {
        const int start = f * hopN + hopN / 2 - win / 2;
        double e = 0;
        std::fill(buf.begin(), buf.end(), std::complex<float>(0.0f, 0.0f));
        for (int i = 0; i < win; ++i)
        {
            const int at = start + i;
            const float v = at >= 0 && at < numSamples ? mono[at] : 0.0f;
            e += v * v;
            buf[static_cast<std::size_t>(i)] = v * window[static_cast<std::size_t>(i)];
        }
        if (frameDb != nullptr) (*frameDb)[static_cast<std::size_t>(f)] = static_cast<float>(10.0 * std::log10(e / win + 1.0e-12));
        fft(buf);
        for (std::size_t b = 0; b < N / 2; ++b) { db[b] = 10.0f * std::log10(std::norm(buf[b]) + 1.0e-12f); cum[b + 1] = cum[b] + db[b]; }
        RidgePeak found[kPeaksPerWindow]; float foundDb[kPeaksPerWindow]; int n = 0; float top = -200.0f;
        for (int b = loBin; b <= hiBin; ++b)
        {
            const auto i = static_cast<std::size_t>(b);
            if (! (db[i] > db[i - 1] && db[i] >= db[i + 1])) continue;
            const int a = std::max(1, static_cast<int>(b / t.floorSpan)), z = std::min(static_cast<int>(N / 2) - 1, static_cast<int>(b * t.floorSpan) + 1);
            const float floorDb = static_cast<float>((cum[static_cast<std::size_t>(z) + 1] - cum[static_cast<std::size_t>(a)]) / (z - a + 1));
            const float prominence = db[i] - floorDb;
            if (prominence < t.minProminence) continue;
            const float d = 0.5f * (db[i - 1] - db[i + 1]) / (db[i - 1] - 2.0f * db[i] + db[i + 1] - 1.0e-9f);
            const float hz = static_cast<float>((static_cast<double>(b) + static_cast<double>(std::clamp(d, -0.5f, 0.5f))) * binHz);
            const RidgePeak p { static_cast<float>(hzToMidi(hz)), hz, t.base + t.promWeight * std::min(prominence, 30.0f) };
            top = std::max(top, db[i]);
            int slot = n;
            if (n == kPeaksPerWindow)
            {
                slot = 0;
                for (int k = 1; k < n; ++k) if (foundDb[k] < foundDb[slot]) slot = k;
                if (db[i] <= foundDb[slot]) continue;
            }
            else ++n;
            found[slot] = p; foundDb[slot] = db[i];
        }
        for (int k = 0; k < n; ++k)
        {
            found[k].score -= t.relative * (top - foundDb[k]);
            peaks[static_cast<std::size_t>(f)].push_back(found[k]);
        }
    }
}

}   // namespace

Ridge trackRidge(const float* mono, int numSamples, double rate, double loHz, double hiHz)
{
    Ridge r;
    const RidgeTuning t;
    const int hopN = std::max(1, static_cast<int>(std::lround(r.hop * rate)));
    const int F = numSamples / hopN;
    r.hz.assign(static_cast<std::size_t>(std::max(0, F)), 0.0f);
    r.strength.assign(r.hz.size(), 0.0f);
    if (F < 4) return r;
    std::vector<std::vector<RidgePeak>> peaks(static_cast<std::size_t>(F));
    std::vector<float> frameDb(static_cast<std::size_t>(F), -120.0f);
    addPeaks(mono, numSamples, rate, hopN, F, t.shortMs, loHz, hiHz, t, peaks, &frameDb);
    addPeaks(mono, numSamples, rate, hopN, F, t.longMs, loHz, hiHz, t, peaks, nullptr);
    const float loudest = *std::max_element(frameDb.begin(), frameDb.end());

    // The best path through the peaks: strong and continuous, with a "nothing" state (the last
    // one) for frames with no partial worth following. Moving costs by the semitone beyond a
    // free allowance, so a sweep is followed and a leap to another partial is not.
    std::vector<std::vector<float>> score(static_cast<std::size_t>(F));
    std::vector<std::vector<int>> from(static_cast<std::size_t>(F));
    for (int f = 0; f < F; ++f)
    {
        const auto uf = static_cast<std::size_t>(f);
        if (frameDb[uf] < loudest - 55.0f) peaks[uf].clear();
        const int n = static_cast<int>(peaks[uf].size());
        score[uf].assign(static_cast<std::size_t>(n) + 1, 0.0f);
        from[uf].assign(static_cast<std::size_t>(n) + 1, -1);
        for (int k = 0; k <= n; ++k)
        {
            const bool none = k == n;
            const float emit = none ? t.none : peaks[uf][static_cast<std::size_t>(k)].score;
            if (f == 0) { score[uf][static_cast<std::size_t>(k)] = emit; continue; }
            const int m = static_cast<int>(peaks[uf - 1].size());
            float best = -1.0e9f; int bestFrom = m;
            for (int j = 0; j <= m; ++j)
            {
                const bool wasNone = j == m;
                float cost = 0.0f;
                if (wasNone != none) cost = t.change;
                else if (! none) cost = t.move * std::max(0.0f, std::abs(peaks[uf][static_cast<std::size_t>(k)].midi - peaks[uf - 1][static_cast<std::size_t>(j)].midi) - t.freeMove);
                const float v = score[uf - 1][static_cast<std::size_t>(j)] - cost;
                if (v > best) { best = v; bestFrom = j; }
            }
            score[uf][static_cast<std::size_t>(k)] = best + emit; from[uf][static_cast<std::size_t>(k)] = bestFrom;
        }
    }
    int k = 0;
    {
        const auto& last = score[static_cast<std::size_t>(F - 1)];
        for (std::size_t j = 1; j < last.size(); ++j) if (last[j] > last[static_cast<std::size_t>(k)]) k = static_cast<int>(j);
    }
    for (int f = F - 1; f >= 0; --f)
    {
        const auto uf = static_cast<std::size_t>(f);
        if (k < static_cast<int>(peaks[uf].size())) { r.hz[uf] = peaks[uf][static_cast<std::size_t>(k)].hz; r.strength[uf] = peaks[uf][static_cast<std::size_t>(k)].score; }
        if (f > 0) k = from[uf][static_cast<std::size_t>(k)];
    }
    return r;
}

} // namespace pitchflattener::core

#include "Signals.h"
#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace dh
{
namespace
{
std::uint64_t splitmix (std::uint64_t& x)
{
    std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
std::uint64_t rotl (std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
constexpr double kTwoPi = 2.0 * std::numbers::pi;
} // namespace

Rng::Rng (std::uint64_t seed)
{
    for (auto& v : s)
        v = splitmix (seed);
}

std::uint64_t Rng::next()
{
    const std::uint64_t result = rotl (s[1] * 5, 7) * 9;
    const std::uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl (s[3], 45);
    return result;
}

double Rng::uniform() { return (double) (next() >> 11) * 0x1.0p-53; }

int Rng::uniformInt (int lo, int hiInclusive)
{
    const auto span = (std::uint64_t) (hiInclusive - lo + 1);
    return lo + (int) (next() % span);
}

double Rng::gauss()
{
    if (haveSpare)
    {
        haveSpare = false;
        return spare;
    }
    double u1 = uniform();
    while (u1 <= 1e-300)
        u1 = uniform();
    const double u2 = uniform();
    const double r = std::sqrt (-2.0 * std::log (u1));
    spare = r * std::sin (kTwoPi * u2);
    haveSpare = true;
    return r * std::cos (kTwoPi * u2);
}

std::vector<float> shapedNoise (double sampleRate, std::size_t n, std::uint64_t seed, const std::function<double (double)>& gainDb)
{
    const std::size_t N = nextPow2 (std::max<std::size_t> (n, 2));
    std::vector<std::complex<double>> spec (N, { 0.0, 0.0 });
    Rng rng (seed);
    const double binHz = sampleRate / (double) N;
    for (std::size_t k = 1; k < N / 2; ++k)
    {
        const double re = rng.gauss(), im = rng.gauss();
        const double g = std::pow (10.0, gainDb ((double) k * binHz) / 20.0);
        spec[k] = { re * g, im * g };
        spec[N - k] = std::conj (spec[k]);
    }
    fft (spec, true);
    std::vector<float> out (n);
    const double scale = 1.0 / std::sqrt ((double) N);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = (float) (spec[i].real() * scale);
    return out;
}

std::vector<float> pinkNoise (double sampleRate, std::size_t n, std::uint64_t seed)
{
    return shapedNoise (sampleRate, n, seed, [] (double hz) {
        const double r = hz / 20.0;
        const double hp = 10.0 * std::log10 (std::pow (r, 8.0) / (1.0 + std::pow (r, 8.0)) + 1e-300);
        return -10.0 * std::log10 (std::max (hz, 1.0) / 1000.0) + hp;
    });
}

double rmsDb (const std::vector<float>& x)
{
    double s = 0.0;
    for (float v : x)
        s += (double) v * v;
    return powerToDb (x.empty() ? 0.0 : s / (double) x.size());
}

void applyGain (std::vector<float>& x, double gain)
{
    for (auto& v : x)
        v = (float) ((double) v * gain);
}

double normaliseRms (std::vector<float>& x, double rmsDbFs)
{
    const double now = rmsDb (x);
    const double g = now <= -300.0 ? 1.0 : dbToAmp (rmsDbFs - now);
    applyGain (x, g);
    return g;
}

std::vector<float> sine (double sampleRate, std::size_t n, double freqHz, double amplitude, double phase)
{
    std::vector<float> x (n);
    const double w = kTwoPi * freqHz / sampleRate;
    for (std::size_t i = 0; i < n; ++i)
        x[i] = (float) (amplitude * std::sin (w * (double) i + phase));
    return x;
}

std::vector<float> square (double sampleRate, std::size_t n, double freqHz, double amplitude)
{
    std::vector<float> x (n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double ph = std::fmod ((double) i * freqHz / sampleRate, 1.0);
        x[i] = (float) (ph < 0.5 ? amplitude : -amplitude);
    }
    return x;
}

std::vector<float> logSweep (double sampleRate, std::size_t n, double f0, double f1, double amplitude)
{
    std::vector<float> x (n);
    const double T = (double) n / sampleRate;
    const double L = std::log (f1 / f0);
    const auto fade = (std::size_t) (0.01 * sampleRate);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sampleRate;
        const double ph = kTwoPi * f0 * T / L * (std::exp (t * L / T) - 1.0);
        double g = 1.0;
        if (i < fade)
            g = (double) i / (double) fade;
        else if (i + fade > n)
            g = (double) (n - i) / (double) fade;
        x[i] = (float) (amplitude * g * std::sin (ph));
    }
    return x;
}

std::vector<float> harmonicTone (double sampleRate, std::size_t n, double f0, const std::vector<double>& levelsDb,
                                 double fundAmp, std::uint64_t seed)
{
    std::vector<double> acc (n, 0.0);
    Rng rng (seed);
    for (std::size_t h = 0; h < levelsDb.size(); ++h)
    {
        const double f = f0 * (double) (h + 1);
        if (f >= sampleRate * 0.45)
            break;
        const double amp = fundAmp * (h == 0 ? 1.0 : dbToAmp (levelsDb[h]));
        const double ph = rng.uniform (0.0, kTwoPi);
        const double w = kTwoPi * f / sampleRate;
        for (std::size_t i = 0; i < n; ++i)
            acc[i] += amp * std::sin (w * (double) i + ph);
    }
    std::vector<float> x (n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = (float) acc[i];
    return x;
}

std::vector<float> musicLike (double sampleRate, std::size_t n, std::uint64_t seed, double rmsDbFs)
{
    Rng rng (seed);
    std::vector<double> acc (n, 0.0);
    const double sr = sampleRate;

    // Bass line with RBass-like harmonics (2nd -17, 3rd -22, 4th -40, 5th -42, ...).
    const double bassNotes[] = { 41.2, 46.25, 49.0, 55.0, 61.74, 65.41, 73.42 };
    const double bassHarm[] = { 0.0, -17.0, -22.0, -40.0, -42.0, -60.0, -55.0, -65.0 };
    for (std::size_t start = 0; start < n;)
    {
        const auto len = (std::size_t) (sr * rng.uniform (0.4, 1.0));
        const double f0 = bassNotes[rng.uniformInt (0, 6)];
        for (int h = 0; h < 8; ++h)
        {
            const double amp = 0.35 * dbToAmp (bassHarm[h]);
            const double w = kTwoPi * f0 * (h + 1) / sr;
            const double ph = rng.uniform (0.0, kTwoPi);
            for (std::size_t i = 0; i < len && start + i < n; ++i)
            {
                const double t = (double) i / sr;
                const double env = std::min (1.0, t / 0.01) * std::exp (-t / 0.8) * std::min (1.0, (double) (len - i) / (0.01 * sr));
                acc[start + i] += amp * env * std::sin (w * (double) i + ph);
            }
        }
        start += len;
    }

    // Kick every half second: 110 Hz falling to 45 Hz.
    for (std::size_t start = 0; start < n; start += (std::size_t) (0.5 * sr))
    {
        double ph = 0.0;
        for (std::size_t i = 0; i < (std::size_t) (0.3 * sr) && start + i < n; ++i)
        {
            const double t = (double) i / sr;
            const double f = 45.0 + 65.0 * std::exp (-t / 0.03);
            ph += kTwoPi * f / sr;
            acc[start + i] += 0.5 * std::exp (-t / 0.12) * std::sin (ph);
        }
    }

    // Chords every two seconds: three tones between 150 and 800 Hz with soft harmonics.
    const double scale[] = { 146.8, 164.8, 196.0, 220.0, 246.9, 293.7, 329.6, 392.0, 440.0, 493.9, 587.3, 659.3, 784.0 };
    for (std::size_t start = 0; start < n; start += (std::size_t) (2.0 * sr))
    {
        for (int v = 0; v < 3; ++v)
        {
            const double f0 = scale[rng.uniformInt (0, 12)];
            for (int h = 1; h <= 6; ++h)
            {
                const double f = f0 * h;
                if (f > sr * 0.45)
                    break;
                const double amp = 0.06 / std::pow ((double) h, 1.5);
                const double w = kTwoPi * f / sr;
                const double ph = rng.uniform (0.0, kTwoPi);
                for (std::size_t i = 0; i < (std::size_t) (2.0 * sr) && start + i < n; ++i)
                {
                    const double t = (double) i / sr;
                    acc[start + i] += amp * std::min (1.0, t / 0.03) * std::exp (-t / 1.5) * std::sin (w * (double) i + ph);
                }
            }
        }
    }

    // Hats every quarter second: short high-passed noise bursts.
    for (std::size_t start = (std::size_t) (0.125 * sr); start < n; start += (std::size_t) (0.25 * sr))
    {
        double prev = 0.0;
        for (std::size_t i = 0; i < (std::size_t) (0.03 * sr) && start + i < n; ++i)
        {
            const double t = (double) i / sr;
            const double w = rng.gauss();
            acc[start + i] += 0.03 * std::exp (-t / 0.008) * (w - prev);
            prev = w;
        }
    }

    std::vector<float> x (n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = (float) acc[i];
    normaliseRms (x, rmsDbFs - 0.5);

    auto bed = pinkNoise (sampleRate, n, seed ^ 0xA5A5A5A5ull);
    normaliseRms (bed, rmsDbFs - 30.0);
    for (std::size_t i = 0; i < n; ++i)
        x[i] += bed[i];
    normaliseRms (x, rmsDbFs);
    return x;
}

double bedShapeDb (double hz)
{
    if (hz <= 0.0)
        return -400.0;
    const double pink = -10.0 * std::log10 (hz / 1000.0);
    const double bassLift = 6.0 / (1.0 + std::pow (hz / 100.0, 2.0));
    const double r = hz / 25.0;
    const double hp = 10.0 * std::log10 (std::pow (r, 8.0) / (1.0 + std::pow (r, 8.0)) + 1e-300);
    const double lp = -10.0 * std::log10 (1.0 + std::pow (hz / 12000.0, 4.0));
    return pink + bassLift + hp + lp;
}

double bassHeavyBedDb (double hz)
{
    if (hz <= 0.0)
        return -400.0;
    const double pink = -10.0 * std::log10 (hz / 1000.0);
    const double x = std::log2 (hz / 100.0) / std::log2 (121.0 / 100.0);   // 0 at 100 Hz, 1 at 121 Hz
    const double lift = x <= 0.0 ? 12.0 : x >= 1.0 ? 0.0 : 6.0 * (1.0 + std::cos (std::numbers::pi * x));
    const double r = hz / 25.0;
    const double hp = 10.0 * std::log10 (std::pow (r, 8.0) / (1.0 + std::pow (r, 8.0)) + 1e-300);
    const double lp = -10.0 * std::log10 (1.0 + std::pow (hz / 12000.0, 4.0));
    return pink + lift + hp + lp;
}

std::vector<float> waveringTone (double sampleRate, std::size_t n, double hz, double wobble,
                                 const std::vector<float>& ampEnv, std::uint64_t seed)
{
    Rng rng (seed);
    const double rate = rng.uniform (0.2, 0.5), ph0 = rng.uniform (0.0, kTwoPi), wph = rng.uniform (0.0, kTwoPi);
    std::vector<float> x (n);
    double ph = ph0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sampleRate;
        const double f = hz * (1.0 + wobble * std::sin (kTwoPi * rate * t + wph));
        const double a = ampEnv.empty() ? 1.0 : (double) ampEnv[std::min (i, ampEnv.size() - 1)];
        x[i] = (float) (a * std::sin (ph));
        ph = std::fmod (ph + kTwoPi * f / sampleRate, kTwoPi);
    }
    return x;
}

std::vector<float> slowLevelDb (double sampleRate, std::size_t n, double loDb, double hiDb, double periodSec,
                                std::uint64_t seed)
{
    // Random targets every half period, joined with raised-cosine segments.
    Rng rng (seed);
    const auto seg = std::max<std::size_t> (1, (std::size_t) (0.5 * periodSec * sampleRate));
    std::vector<float> out (n);
    double from = rng.uniform (loDb, hiDb), to = rng.uniform (loDb, hiDb);
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::size_t j = i % seg;
        if (j == 0 && i > 0)
        {
            from = to;
            to = rng.uniform (loDb, hiDb);
        }
        const double u = 0.5 - 0.5 * std::cos (std::numbers::pi * (double) j / (double) seg);
        out[i] = (float) (from + (to - from) * u);
    }
    return out;
}

double honkExcessDb (double hz, const HonkShape& shape)
{
    if (hz <= 0.0)
        return 0.0;
    auto raisedCos = [hz] (double centre, double fwhmOct) {
        const double x = std::log2 (hz / centre) / fwhmOct; // half height at |x| = 0.5
        return std::abs (x) < 1.0 ? 0.5 * (1.0 + std::cos (std::numbers::pi * x)) : 0.0;
    };
    double excessPow = 1.0;
    if (shape.bump)
        excessPow *= std::pow (10.0, shape.bumpDb * raisedCos (shape.bumpHz, shape.bumpFwhmOct) / 10.0);
    for (double f : shape.resonanceHz)
        excessPow *= std::pow (10.0, shape.resonanceDb * raisedCos (f, shape.resonanceFwhmOct) / 10.0);
    return 10.0 * std::log10 (excessPow);
}

Audio honkSignal (double sampleRate, double seconds, int numChannels, const HonkShape& shape, double rmsDbFs,
                  std::uint64_t seed, Audio* bedOut)
{
    const auto n = (std::size_t) std::llround (seconds * sampleRate);
    Audio out = Audio::make (sampleRate, numChannels, n);
    if (bedOut != nullptr)
        *bedOut = Audio::make (sampleRate, numChannels, n);
    for (int c = 0; c < numChannels; ++c)
    {
        const std::uint64_t s = seed + 7919ull * (std::uint64_t) c;
        auto bedDb = [&shape] (double hz) { return shape.bed ? shape.bed (hz) : bedShapeDb (hz); };
        auto x = shapedNoise (sampleRate, n, s, [&] (double hz) { return bedDb (hz) + honkExcessDb (hz, shape); });
        const double g = normaliseRms (x, rmsDbFs);
        out.ch[(std::size_t) c] = std::move (x);
        if (bedOut != nullptr)
        {
            auto b = shapedNoise (sampleRate, n, s, bedDb);
            applyGain (b, g);
            bedOut->ch[(std::size_t) c] = std::move (b);
        }
    }
    return out;
}

} // namespace dh

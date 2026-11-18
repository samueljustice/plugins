// Deterministic test signals for the harness. Everything is generated from a seed with the
// harness's own PRNG, so a run is repeatable on any machine.
#pragma once

#include "Audio.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace dh
{

/** xoshiro256** seeded through splitmix64. */
class Rng
{
public:
    explicit Rng (std::uint64_t seed);
    std::uint64_t next();
    double uniform();                           // [0, 1)
    double uniform (double lo, double hi) { return lo + (hi - lo) * uniform(); }
    int uniformInt (int lo, int hiInclusive);
    double gauss();

private:
    std::uint64_t s[4];
    bool haveSpare = false;
    double spare = 0.0;
};

/** Stationary noise with the given magnitude response (dB as a function of Hz), synthesised in
    the frequency domain with random phase. Unnormalised: the same seed and length give the same
    random draws, so two shapes made from one seed differ only by their shape. */
std::vector<float> shapedNoise (double sampleRate, std::size_t n, std::uint64_t seed,
                                const std::function<double (double)>& gainDb);

/** Pink noise (-3 dB per octave from 20 Hz up). Unnormalised. */
std::vector<float> pinkNoise (double sampleRate, std::size_t n, std::uint64_t seed);

double rmsDb (const std::vector<float>& x);
void applyGain (std::vector<float>& x, double gain);
/** Scales x so its RMS is rmsDbFs; returns the gain applied. */
double normaliseRms (std::vector<float>& x, double rmsDbFs);

std::vector<float> sine (double sampleRate, std::size_t n, double freqHz, double amplitude, double phase = 0.0);
std::vector<float> square (double sampleRate, std::size_t n, double freqHz, double amplitude);
/** Exponential sine sweep from f0 to f1 over the whole length, 10 ms fades. */
std::vector<float> logSweep (double sampleRate, std::size_t n, double f0, double f1, double amplitude);
/** Harmonic tone: levelsDb[k] is harmonic k+1 relative to the fundamental (levelsDb[0] is ignored
    and taken as 0 dB). Fundamental peak amplitude fundAmp. Phases from the seed. */
std::vector<float> harmonicTone (double sampleRate, std::size_t n, double f0, const std::vector<double>& levelsDb,
                                 double fundAmp, std::uint64_t seed);
/** A stand-in for music: bass line with RBass-like harmonics, kick, sustained chords in the low
    mids, hats and a quiet pink bed. RMS rmsDbFs. */
std::vector<float> musicLike (double sampleRate, std::size_t n, std::uint64_t seed, double rmsDbFs = -16.0);

/** Spectral shapes for the honk tests. */
struct HonkShape
{
    bool bump = false;
    double bumpHz = 250.0;
    double bumpDb = 10.0;
    double bumpFwhmOct = 1.0;                // raised cosine in log frequency, full width at half height
    std::vector<double> resonanceHz;          // narrow peaks
    double resonanceDb = 12.0;
    double resonanceFwhmOct = 1.0 / 6.0;
    std::function<double (double)> bed;       // the bed's shape in dB; empty = bedShapeDb
};

/** The bed: pink with about 6 dB extra below 100 Hz, high-passed at 25 Hz, gently rolled off
    above 12 kHz. dB as a function of Hz. */
double bedShapeDb (double hz);

/** A bass-heavy bed: pink with everything below 100 Hz raised by 12 dB (a raised-cosine step
    in log frequency, complete by 121 Hz), high-passed at 25 Hz, rolled off above 12 kHz. */
double bassHeavyBedDb (double hz);

/** A slowly wavering tone: frequency hz * (1 + wobble * sin(...)) with the wobble at a rate
    between 0.2 and 0.5 Hz chosen from the seed, and a peak amplitude that follows ampEnv
    (one value per sample; empty = amplitude 1). */
std::vector<float> waveringTone (double sampleRate, std::size_t n, double hz, double wobble,
                                 const std::vector<float>& ampEnv, std::uint64_t seed);

/** A slow random level in dB: a smoothed random walk between loDb and hiDb, one value per
    sample, moving on a time scale of about periodSec. */
std::vector<float> slowLevelDb (double sampleRate, std::size_t n, double loDb, double hiDb, double periodSec,
                                std::uint64_t seed);
/** What the shape adds on top of the bed, dB as a function of Hz. */
double honkExcessDb (double hz, const HonkShape& shape);

/** Stereo or mono honk signal: bed plus shape, normalised to rmsDbFs. If bedOut is given it
    receives the bed alone, made from the same random draws and scaled by the same gain, so
    (signal - bed) in dB is exactly the designed excess. Channels use different seeds. */
Audio honkSignal (double sampleRate, double seconds, int numChannels, const HonkShape& shape, double rmsDbFs,
                  std::uint64_t seed, Audio* bedOut = nullptr);

} // namespace dh

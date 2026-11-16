#pragma once

// Where in the learned sound a stretch of incoming audio comes from.
//
// The plug-in plays the flattened sound in place of the input whenever the input is the learned
// recording again, from wherever in it the input happens to be. That needs the exact sample of the
// learned sound the input is, found quickly enough that the flattened sound can take over before
// the input reaches the output (the plug-in's latency is what gives it that time).
//
// What counts as "the same recording". Not resemblance: the input x must be the learned sound y
// at that place, at some playback level g, with almost nothing left over. With g = (x.y)/(y.y),
// the residual |x - g*y|^2 / |x|^2 is about 1e-12 for the same file played back (float rounding),
// (over the probe the level g may change in a straight line, so a fade on the player still fits)
// and it grows with anything that differs: another take, the same sound at another pitch or
// speed, other material layered under it. See matching:: below for the figures.
//
// Method. The residual at a position is 1 - c^2, where c is the normalised correlation there, so
// the search computes c at every position at once with the FFT (overlap-save in blocks of 32768
// or 65536 samples, two blocks per complex transform: the learned sound is real, so one block rides
// in the real part and the next in the imaginary part), and recomputes the residual exactly, in
// double precision, at the peaks near the top. At 176.4 and 192 kHz the FFT pass runs on a copy
// decimated by four (two of the four sample phases, so no position is more than a sample away)
// and the exact pass at the full rate: that keeps a two-minute sound at 192 kHz inside the
// latency's search budget.
//
// The search runs from the start of the sound and stops at the first position that is exactly the
// learned sound at an ordinary level (the common case: the sound played from its top), so playing
// from the start costs only the first block or two.
//
// Ambiguity. A sound that repeats itself (a steady tone, a hum, room tone) is the "same" at many
// places. A probe that matches at exactly one place is taken at any level. One that matches at
// several is taken only when it is within 12 dB of the learned sound's loudest 85 ms (a quiet hum
// that matches everywhere is evidence of nothing), and then the earliest place at or after the one
// the caller says is consistent with what has already been heard; the plug-in keeps checking as
// the sound goes on and lets go if it was wrong.
//
// Timings (Apple M-series, one core, plain C++ FFT): a 120 s learned sound searched end to end
// takes about 65 ms at 48 kHz, 135 ms at 96 kHz and 150 ms at 192 kHz; a match at the start under
// 5 ms. The tests print the figures for the machine they run on.
//
// Plain C++20, no JUCE. prepare() and locate() allocate: for a background thread.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace pitchflattener
{

// The two figures that decide "this is the learned recording", in one place.
namespace matching
{
// To accept a match (the search): the input is the learned sound with less than -30 dB left over.
// The same file played back leaves about -120 dB (float rounding); the same sound resampled by 2%,
// pitched a semitone, polarity-inverted or with a DC offset leaves -15 dB or more, and material
// layered under the learned sound shows up as its own level.
inline constexpr double kAcceptResidual = 1.0e-3;   // -30 dB
// To keep playing (every window of about 21 ms once matched): looser, because a window is short
// and ends of sounds are quiet, but tight enough that a layer louder than about -20 dB under the
// learned sound, a jump, or another file breaks it and the input comes back.
inline constexpr double kKeepResidual = 1.0e-2;     // -20 dB
// An ambiguous match is taken only when the probe is at least this loud against the learned
// sound's loudest probe-length stretch (energy ratio: -12 dB).
inline constexpr double kAmbiguousLevel = 0.063095734;

/** |x - g*y|^2 / |x|^2 with g = xy/yy, from the three sums. 1 when nothing fits (and for g <= 0:
    an inverted copy is not the learned recording). */
inline double residual(double xx, double yy, double xy) noexcept
{
    if (! (xx > 0.0) || ! (yy > 0.0) || ! (xy > 0.0))
        return 1.0;
    const double r = 1.0 - (xy / xx) * (xy / yy);
    return r > 0.0 ? r : 0.0;
}

// The same recording through a short linear filter. A host can play a file through an
// interpolating engine, or with a fraction of a sample of offset, even at its own rate: the result
// is the learned recording through a filter a few samples long, which a single gain cannot fit
// (the error grows with frequency: a bright sound misses by more). So the input x is also fitted,
// by least squares, as the learned sound y through 9 taps at offsets -4..+4:
//     x[t] = sum_k h[k] y[t + k]
// and the residual after that fit counts too, but only for a filter that is a delay with a gain:
// its gain at DC (the sum of the taps) positive, so an inverted copy stays refused, and at least
// kDelayLikeShare of its energy in 3 adjacent taps, so that no filter can stand in for a different
// recording. A different take, another pitch or speed, a layer or a hum are no closer through such
// a filter: what differs is not linear and time-invariant over a few samples.
inline constexpr int kFilterReach = 4;                     // taps at -4..+4
inline constexpr int kFilterTaps = 2 * kFilterReach + 1;
inline constexpr double kDelayLikeShare = 0.7;

struct Fit
{
    double residual = 1.0;   // the smaller of a plain gain's and (if delay-like) the filter's
    double gain = 0.0;       // the level: the plain gain, or the filter's gain at DC
    double plainResidual = 1.0;
    double filterResidual = 1.0;
    double delayShare = 0.0; // the filter's energy in its 3 strongest adjacent taps
    bool filtered = false;   // the filter gave the residual
};

/**
 * Fits x (n samples, x(t) for t in [0, n)) as y through a short filter (y(t) is read for t in
 * [-kFilterReach, n + kFilterReach); return 0 outside the sound). Fixed size: no allocation, about
 * 20 multiply-adds per sample. Safe on the audio thread.
 */
template <typename X, typename Y>
Fit fitRecording(const X& x, const Y& y, int n) noexcept
{
    constexpr int H = kFilterReach, K = kFilterTaps;
    Fit fit;
    if (n <= 0)
        return fit;
    // A[d] = sum over t of y(t + s) y(t + s + d) for the current s, slid from s = -H to +H.
    double A[K] {};
    for (int t = 0; t < n; ++t)
    {
        const double base = y(t - H);
        for (int d = 0; d < K; ++d)
            A[d] += base * y(t - H + d);
    }
    double R[K][K] {};
    for (int s = -H; s <= H; ++s)
    {
        for (int d = 0; s + d <= H; ++d)
        {
            R[s + H][s + d + H] = A[d];
            R[s + d + H][s + H] = A[d];
        }
        if (s < H)
            for (int d = 0; d < K; ++d)
                A[d] += y(s + n) * y(s + n + d) - y(s) * y(s + d);
    }
    double r[K] {}, xx = 0.0;
    for (int t = 0; t < n; ++t)
    {
        const double v = x(t);
        xx += v * v;
        for (int k = 0; k < K; ++k)
            r[k] += v * y(t + k - H);
    }
    fit.plainResidual = residual(xx, R[H][H], r[H]);
    fit.residual = fit.plainResidual;
    fit.gain = R[H][H] > 0.0 ? r[H] / R[H][H] : 0.0;
    if (! (xx > 0.0) || ! (R[H][H] > 0.0))
        return fit;

    // Least squares with a little ridge (a near-silent stretch cannot blow up), by Cholesky.
    double trace = 0.0;
    for (int k = 0; k < K; ++k)
        trace += R[k][k];
    const double ridge = 1.0e-6 * trace / K + 1.0e-30;
    double Lm[K][K] {};
    for (int i = 0; i < K; ++i)
        for (int j = 0; j <= i; ++j)
        {
            double sum = R[i][j] + (i == j ? ridge : 0.0);
            for (int k = 0; k < j; ++k)
                sum -= Lm[i][k] * Lm[j][k];
            if (i == j)
            {
                if (! (sum > 0.0))
                    return fit;
                Lm[i][i] = std::sqrt(sum);
            }
            else
                Lm[i][j] = sum / Lm[j][j];
        }
    double z[K] {}, h[K] {};
    for (int i = 0; i < K; ++i)
    {
        double sum = r[i];
        for (int k = 0; k < i; ++k)
            sum -= Lm[i][k] * z[k];
        z[i] = sum / Lm[i][i];
    }
    for (int i = K - 1; i >= 0; --i)
    {
        double sum = z[i];
        for (int k = i + 1; k < K; ++k)
            sum -= Lm[k][i] * h[k];
        h[i] = sum / Lm[i][i];
    }
    double explained = 0.0, gain = 0.0, energy = 0.0, best = 0.0;
    for (int k = 0; k < K; ++k)
    {
        explained += h[k] * r[k];
        gain += h[k];
        energy += h[k] * h[k];
    }
    for (int k = 0; k + 2 < K; ++k)
        best = std::max(best, h[k] * h[k] + h[k + 1] * h[k + 1] + h[k + 2] * h[k + 2]);
    fit.filterResidual = std::clamp((xx - explained) / xx, 0.0, 1.0);
    fit.delayShare = energy > 0.0 ? best / energy : 0.0;
    if (gain > 0.0 && fit.delayShare >= kDelayLikeShare && fit.filterResidual < fit.residual)
    {
        fit.residual = fit.filterResidual;
        fit.gain = gain;
        fit.filtered = true;
    }
    return fit;
}
} // namespace matching

class Locator
{
public:
    static constexpr float kSilence = 3.1622777e-4f;   // -70 dBFS: below this a sample counts as silence

    struct Match
    {
        bool found = false;
        std::int64_t sourceSample = 0;   // where the probe's first sample is in the learned sound
        double residual = 1.0;           // exact, at that position (or the best seen when not found)
        double correlation = 0.0;        // the same as a normalised correlation (for printing)
        double gain = 1.0;               // the probe's level over the learned sound's there, at the probe's start
        double gainEnd = 1.0;            // ...and at its end (a fade on the player changes it across the probe)
        int candidates = 0;              // positions computed exactly
        bool searchedAll = false;        // false: stopped early on an exact match
        bool ambiguous = false;          // it matched at several places
        bool tooQuiet = false;           // ...and was refused for being too quiet to tell (see the header)
        std::int64_t bestPosition = -1;  // the best candidate's place (found or not; -1: no candidate at all)
        int accepted = 0;                // distinct places that passed the accept figure
        double plainResidual = 1.0;      // at the best place, with a plain level only (no filter: see matching::)
    };

    /** Takes a copy of the learned sound (one channel: the mix). */
    void prepare(const float* mono, std::int64_t numSamples, double sampleRate);

    bool isPrepared() const noexcept { return m_length > 0; }
    std::int64_t length() const noexcept { return m_length; }
    double sampleRate() const noexcept { return m_sampleRate; }
    /** The learned sound (length() samples, followed by probeLength() zeros). */
    const float* samples() const noexcept { return m_samples.data(); }

    /** How many incoming samples a search wants: about 85 ms, never more than the sound. */
    int probeLength() const noexcept { return m_probeLength; }
    static int nominalProbeLength(double sampleRate) noexcept;

    /**
     * Finds where probe (probeLength() samples) is in the learned sound. preferFrom: among several
     * equally good places, ones before this are taken only when none after it is. cancelled is
     * asked between blocks.
     */
    Match locate(const float* probe, std::int64_t preferFrom = 0, const std::function<bool()>& cancelled = {}) const;

    /**
     * The exact residual of probe (length samples) against the learned sound at position, with a
     * level that may change in a straight line across the probe (a fade on the player), and that
     * level at the probe's start and end.
     */
    double residualAt(const float* probe, int length, std::int64_t position, double* gainStart = nullptr, double* gainEnd = nullptr,
                      bool withFilter = true, double* plain = nullptr) const noexcept;

private:
    std::vector<float> m_samples;
    std::vector<float> m_decimated;   // high rates only: the sound averaged in fours (plus padding)
    int m_decimation = 1;
    std::int64_t m_length = 0;
    double m_sampleRate = 0.0;
    int m_probeLength = 0;
    double m_loudestEnergy = 0.0;     // the loudest probe-length stretch of the sound
};

} // namespace pitchflattener

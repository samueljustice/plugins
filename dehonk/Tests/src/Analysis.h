// Independent level analysis for the harness: Welch-averaged power spectra, band levels,
// sine fits and residuals. Does not use anything from the DSP core.
#pragma once

#include "Audio.h"

#include <cstddef>
#include <vector>

namespace dh
{

/** One-sided power spectrum, averaged over time and channels, scaled so that the sum over all
    bins equals the signal's mean square (a full-scale sine reads -3 dB). */
struct Spectrum
{
    double sampleRate = 0.0;
    double binHz = 0.0;
    std::vector<double> power;

    /** Mean-square power in [loHz, hiHz). */
    double bandPower (double loHz, double hiHz) const;
    /** bandPower in dB. */
    double bandDb (double loHz, double hiHz) const { return powerToDb (bandPower (loHz, hiHz)); }
    /** Level of a band centred on centreHz, halfWidthOct octaves either side. */
    double centredDb (double centreHz, double halfWidthOct) const;
};

/** Welch average with a Hann window and 50% overlap over samples [start, end).
    The FFT is the largest power of two that is at most about 1.25 s and fits in the range. */
Spectrum powerSpectrum (const Audio& a, std::size_t start, std::size_t end);

/** Mean square over [start, end), all channels. */
double meanSquare (const Audio& a, std::size_t start, std::size_t end);

/** 10 log10( sum (x - g*ref)^2 / sum (g*ref)^2 ) over [start, end), all channels. */
double residualDb (const Audio& x, const Audio& ref, double g, std::size_t start, std::size_t end);
/** max |x - g*ref| over [start, end), all channels. */
double maxAbsDiff (const Audio& x, const Audio& ref, double g, std::size_t start, std::size_t end);

/** Least-squares amplitude of a sine at freqHz in x[0..n). */
double fitSineAmplitude (const float* x, std::size_t n, double freqHz, double sampleRate);

double peakAbs (const Audio& a, std::size_t start, std::size_t end);
bool allFinite (const Audio& a);

double percentile (std::vector<double> v, double p);

} // namespace dh

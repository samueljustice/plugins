#include "Engine.h"

#include <algorithm>
#include <cmath>

namespace tt
{
namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr double burstSeconds = 0.012, attackSeconds = 0.001;

double burstAt (double t, double frequency)
{
    const double decayPerSecond = std::log (1000.0) / (burstSeconds - attackSeconds);   // 60 dB down at the end
    const double envelope = t < attackSeconds ? 0.5 - 0.5 * std::cos (pi * t / attackSeconds)
                                              : std::exp (-decayPerSecond * (t - attackSeconds));
    return envelope * std::sin (2.0 * pi * frequency * t);
}

// The largest value the burst reaches, so it can be scaled to a stated peak.
double burstPeak (double frequency)
{
    double peak = 0.0;
    for (int i = 0; i < 12000; ++i)
        peak = std::max (peak, std::abs (burstAt (i * 1.0e-6, frequency)));
    return peak;
}
}

// One burst per beat: a sine that rises over 1 ms from the beat's exact (fractional) sample
// position and then dies away, 12 ms in all. Beat 1 of a bar is 2 kHz at -6 dBFS, the others
// 1 kHz at -10 dBFS.
std::vector<float> renderClick (const BeatGrid& grid, int beatsPerBar, double sampleRate, std::size_t numSamples)
{
    std::vector<float> out (numSamples, 0.0f);
    if (grid.empty() || sampleRate <= 0.0 || numSamples == 0)
        return out;

    const int perBar = std::max (1, beatsPerBar);
    const double accentGain = 0.5012 / burstPeak (2000.0);   // -6 dBFS
    const double beatGain = 0.3162 / burstPeak (1000.0);     // -10 dBFS
    const double endSeconds = (double) numSamples / sampleRate;

    // From one bar before the first pin to the end of the audio.
    // No more clicks than 1000 a minute over the whole length, whatever the pins say.
    const double firstBeat = std::ceil (grid.beatAtTime (grid.getPins().front().time) - perBar - 1.0e-9);
    const double lastBeat = firstBeat + endSeconds * 1000.0 / 60.0 + 64.0;
    for (double beat = firstBeat; beat <= lastBeat; beat += 1.0)
    {
        const double start = grid.timeAtBeat (beat);
        if (start >= endSeconds)
            break;
        if (start + burstSeconds <= 0.0)
            continue;

        const long long bar = (long long) std::floor (beat / perBar);
        const bool accent = beat - (double) bar * perBar == 0.0;
        const double frequency = accent ? 2000.0 : 1000.0;
        const double gain = accent ? accentGain : beatGain;

        const double startSample = start * sampleRate;
        const auto first = (long long) std::ceil (startSample);
        const auto last = std::min ((long long) numSamples, (long long) std::ceil ((start + burstSeconds) * sampleRate));
        for (long long i = std::max (0LL, first); i < last; ++i)
        {
            out[(size_t) i] += (float) (gain * burstAt (((double) i - startSample) / sampleRate, frequency));
        }
    }
    return out;
}

} // namespace tt

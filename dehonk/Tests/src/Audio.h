// Small shared types for the De-Honk offline harness. No JUCE.
#pragma once

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace dh
{

/** Planar float audio at one sample rate. */
struct Audio
{
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> ch;

    int numChannels() const { return (int) ch.size(); }
    std::size_t length() const { return ch.empty() ? 0 : ch[0].size(); }
    double seconds() const { return (double) length() / sampleRate; }

    static Audio make (double sr, int numChannels, std::size_t numSamples)
    {
        Audio a;
        a.sampleRate = sr;
        a.ch.assign ((std::size_t) numChannels, std::vector<float> (numSamples, 0.0f));
        return a;
    }
};

inline double powerToDb (double p) { return p > 0.0 ? 10.0 * std::log10 (p) : -400.0; }
inline double ampToDb (double a) { return a > 0.0 ? 20.0 * std::log10 (a) : -400.0; }
inline double dbToAmp (double db) { return std::pow (10.0, db / 20.0); }

/** printf into a std::string. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__ ((format (printf, 1, 2)))
#endif
inline std::string fmt (const char* f, ...)
{
    char buf[1024];
    va_list args;
    va_start (args, f);
    std::vsnprintf (buf, sizeof (buf), f, args);
    va_end (args);
    return buf;
}

/** dB value for printing; anything at or below -300 dB prints as -inf. */
inline std::string dbStr (double db, int decimals = 2)
{
    if (! std::isfinite (db) && db > 0) return "+inf";
    if (! std::isfinite (db) || db <= -300.0) return "-inf";
    return fmt ("%+.*f", decimals, db);
}

} // namespace dh

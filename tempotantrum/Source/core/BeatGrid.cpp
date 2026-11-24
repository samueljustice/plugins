#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tt
{

BeatGrid::BeatGrid (std::vector<Pin> pinsToUse, double fallback)
    : pins (std::move (pinsToUse)), fallbackBpm (fallback > 0.0 ? fallback : 120.0)
{
    std::stable_sort (pins.begin(), pins.end(), [] (const Pin& a, const Pin& b) { return a.time < b.time; });
}

namespace
{
// Seconds per beat of the segment that starts at pin i. A segment with no length in time or in
// beats has no tempo of its own and takes the fallback.
double secondsPerBeat (const std::vector<Pin>& pins, size_t i, double fallbackBpm)
{
    const double db = pins[i + 1].beat - pins[i].beat, dt = pins[i + 1].time - pins[i].time;
    return (db > 0.0 && dt > 0.0) ? dt / db : 60.0 / fallbackBpm;
}

// Can the segment that starts at pin i be carried on beyond the pins? Not when it ends on a
// restart (it spans a stretch with no beat, so its slope is no tempo), nor when it is shorter
// than a beat or its tempo is outside 20 to 400 BPM (two pins a moment apart).
bool carriesOn (const std::vector<Pin>& pins, size_t i)
{
    const double db = pins[i + 1].beat - pins[i].beat, dt = pins[i + 1].time - pins[i].time;
    if (pins[i + 1].restart || db < 1.0 - 1.0e-9 || dt <= 0.0)
        return false;
    const double bpm = 60.0 * db / dt;
    return bpm >= 20.0 && bpm <= 400.0;
}

// Seconds per beat before the first pin: the tempo of the first segment that can be carried on.
double leadSecondsPerBeat (const std::vector<Pin>& pins, double fallbackBpm)
{
    for (size_t i = 0; i + 1 < pins.size(); ++i)
        if (carriesOn (pins, i))
            return secondsPerBeat (pins, i, fallbackBpm);
    return 60.0 / fallbackBpm;
}

// Seconds per beat after the last pin: the tempo of the last segment that can be carried on.
double tailSecondsPerBeat (const std::vector<Pin>& pins, double fallbackBpm)
{
    for (size_t i = pins.size() - 1; i-- > 0;)
        if (carriesOn (pins, i))
            return secondsPerBeat (pins, i, fallbackBpm);
    return 60.0 / fallbackBpm;
}
}

double BeatGrid::timeAtBeat (double beat) const
{
    if (pins.empty())
        return beat * 60.0 / fallbackBpm;
    if (beat < pins.front().beat)
        return pins.front().time + (beat - pins.front().beat) * leadSecondsPerBeat (pins, fallbackBpm);
    if (beat >= pins.back().beat)
        return pins.back().time + (beat - pins.back().beat) * tailSecondsPerBeat (pins, fallbackBpm);

    // The segment whose beats hold this one.
    size_t i = 0;
    while (i + 2 < pins.size() && beat >= pins[i + 1].beat)
        ++i;
    return pins[i].time + (beat - pins[i].beat) * secondsPerBeat (pins, i, fallbackBpm);
}

double BeatGrid::beatAtTime (double time) const
{
    if (pins.empty())
        return time * fallbackBpm / 60.0;
    if (time < pins.front().time)
        return pins.front().beat + (time - pins.front().time) / leadSecondsPerBeat (pins, fallbackBpm);
    if (time >= pins.back().time)
        return pins.back().beat + (time - pins.back().time) / tailSecondsPerBeat (pins, fallbackBpm);

    size_t i = 0;
    while (i + 2 < pins.size() && time >= pins[i + 1].time)
        ++i;
    return pins[i].beat + (time - pins[i].time) / secondsPerBeat (pins, i, fallbackBpm);
}

double BeatGrid::bpmAtTime (double time) const
{
    if (pins.empty())
        return fallbackBpm;
    if (time < pins.front().time)
        return 60.0 / leadSecondsPerBeat (pins, fallbackBpm);
    if (time >= pins.back().time)
        return 60.0 / tailSecondsPerBeat (pins, fallbackBpm);

    size_t i = 0;
    while (i + 2 < pins.size() && time >= pins[i + 1].time)
        ++i;
    return 60.0 / secondsPerBeat (pins, i, fallbackBpm);
}

// Positions before bar 1 beat 1 count down through bar 0, bar -1 and so on, so every beat has
// its own text.
std::string formatBarBeat (double beat, int beatsPerBar)
{
    const long long perBeat = 960;
    const long long perBar = perBeat * std::max (1, beatsPerBar);
    const long long ticks = std::llround (beat * (double) perBeat);
    long long bar = ticks / perBar, inBar = ticks % perBar;
    if (inBar < 0)
    {
        inBar += perBar;
        --bar;
    }
    char text[64];
    std::snprintf (text, sizeof (text), "%lld|%lld|%03lld", bar + 1, inBar / perBeat + 1, inBar % perBeat);
    return text;
}

} // namespace tt

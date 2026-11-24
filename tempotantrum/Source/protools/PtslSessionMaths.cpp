#include "PtslSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

// The parts of PtslSession that only work on numbers and names, so the tests can build them
// without the scripting link.
namespace tt::pt
{

namespace
{
    std::string lowered (std::string text)
    {
        std::transform (text.begin(), text.end(), text.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        return text;
    }

    // The channel endings Pro Tools gives the mono files of a multi-mono clip, in lower case.
    bool isChannelName (const std::string& ending)
    {
        static const char* const names[] = { "l", "r", "c", "ls", "rs", "lfe", "lc", "rc", "cs", "lsr", "rsr", "lss", "rss",
                                             "lrs", "rrs", "lts", "rts", "ltf", "rtf", "ltr", "rtr", "lw", "rw", "sl", "sr" };

        // Numbered endings (".1", ".2") are left out: "Cue v1.2.wav" is more likely a version.
        const auto low = lowered (ending);
        return std::find (std::begin (names), std::end (names), low) != std::end (names);
    }

    // "Cue 3.L.wav" into "Cue 3" and "L". False, with the name less its extension as `base`, when
    // there is no channel ending.
    bool splitChannel (const std::string& fileName, std::string& base, std::string& channel)
    {
        const auto dot = fileName.find_last_of ('.');
        base = dot == std::string::npos || dot == 0 ? fileName : fileName.substr (0, dot);
        channel.clear();

        const auto channelDot = base.find_last_of ('.');

        if (channelDot == std::string::npos || channelDot == 0 || ! isChannelName (base.substr (channelDot + 1)))
            return false;

        channel = base.substr (channelDot + 1);
        base = base.substr (0, channelDot);
        return true;
    }
}

bool samplesOf (const std::string& text, long long& samples)
{
    std::string digits;
    bool negative = false;

    for (char c : text)
    {
        if (c >= '0' && c <= '9')
            digits.push_back (c);
        else if (c == '-' && digits.empty() && ! negative)
            negative = true;
        else if (c != ' ' && c != ',' && c != '\t')
            return false;
    }

    if (digits.empty() || digits.size() > 15)
        return false;

    samples = std::stoll (digits) * (negative ? -1 : 1);
    return true;
}

double secondsOfTimecode (const std::string& text)
{
    long long parts[3] = {};
    int found = 0;
    std::string piece;

    for (size_t i = 0; i <= text.size() && found < 3; ++i)
    {
        const char c = i < text.size() ? text[i] : ':';

        if (c >= '0' && c <= '9')
        {
            piece.push_back (c);
        }
        else if (c == ':' || c == ';' || c == '.')
        {
            if (piece.empty() || piece.size() > 6)
                return 0.0;

            parts[found++] = std::stoll (piece);
            piece.clear();
        }
        else if (c != ' ')
        {
            return 0.0;
        }
    }

    return found == 3 ? (double) (parts[0] * 3600 + parts[1] * 60 + parts[2]) : 0.0;
}

std::string minutesAndSeconds (double seconds)
{
    const auto whole = (long long) std::floor (std::max (0.0, seconds));
    char text[32];
    std::snprintf (text, sizeof (text), "%lld:%02lld", whole / 60, whole % 60);
    return text;
}

std::string clipNameOf (const std::string& fileName)
{
    std::string base, channel;
    splitChannel (fileName, base, channel);
    return lowered (base);
}

std::vector<std::string> clipsOf (const std::vector<std::string>& fileNames)
{
    struct Group
    {
        std::string shown, key, base;
        std::vector<std::string> channels;
    };

    std::vector<Group> groups;

    for (const auto& name : fileNames)
    {
        std::string base, channel;

        if (! splitChannel (name, base, channel))
        {
            groups.push_back ({ name, {}, {}, {} });
            continue;
        }

        const auto key = lowered (base);
        auto same = std::find_if (groups.begin(), groups.end(), [&key] (const Group& g) { return ! g.key.empty() && g.key == key; });

        if (same == groups.end())
            groups.push_back ({ {}, key, base, { channel } });
        else
            same->channels.push_back (channel);
    }

    std::vector<std::string> clips;

    for (const auto& g : groups)
    {
        if (g.key.empty())
        {
            clips.push_back (g.shown);
            continue;
        }

        std::string channels;

        for (const auto& c : g.channels)
            channels += (channels.empty() ? "" : ", ") + c;

        clips.push_back (g.base + " (" + channels + ")");
    }

    return clips;
}

//==============================================================================
std::vector<long long> probePoints (long long from, long long to, double sampleRate)
{
    std::vector<long long> points;

    if (to <= from || ! (sampleRate > 0.0))
        return points;

    const double span = (double) (to - from);
    const int count = std::clamp ((int) std::ceil (span / (2.0 * sampleRate)), 1, 299);

    for (int i = 0; i <= count; ++i)
        points.push_back (from + (long long) std::llround (span * i / count));

    return points;
}

TempoChange compareTempo (const TempoSurvey& before, const TempoSurvey& after)
{
    TempoChange change;

    if (! before.ok || ! after.ok)
    {
        change.problem = ! before.ok ? before.problem : after.problem;
        return change;
    }

    if (before.samples != after.samples || before.ticks.size() != after.ticks.size() || before.inTickUnits != after.inTickUnits
        || before.ticksPerBeat != after.ticksPerBeat)
    {
        change.problem = "the two readings do not match up";
        return change;
    }

    change.compared = true;

    if (before.ticks.empty())
        return change;

    const long long perBeat = std::max<long long> (1, before.ticksPerBeat);
    const long long allowed = std::max<long long> (2, std::llround (2.0 * (double) perBeat / 960.0));

    for (size_t i = 0; i + 1 < before.ticks.size(); ++i)
    {
        const long long was = before.ticks[i + 1] - before.ticks[i], is = after.ticks[i + 1] - after.ticks[i];

        if (std::llabs (is - was) > allowed)
        {
            if (! change.changed)
                change.firstSample = before.samples[i];

            change.changed = true;
            change.lastSample = before.samples[i + 1];
        }
    }

    change.shiftTicks = after.ticks.front() - before.ticks.front();
    change.shifted = std::llabs (change.shiftTicks) > allowed;

    if (change.shifted)
    {
        change.shiftBeats = std::llround ((double) change.shiftTicks / (double) perBeat);
        change.partBeat = std::llabs (change.shiftTicks - change.shiftBeats * perBeat) > allowed || change.shiftBeats == 0;
    }

    return change;
}

std::string describeShift (long long beats, int beatsPerBar)
{
    if (beats == 0)
        return {};

    const long long perBar = std::max (1, beatsPerBar);
    const auto count = [] (long long n, const char* one, const char* many) { return std::to_string (std::llabs (n)) + " " + (std::llabs (n) == 1 ? one : many); };
    const auto way = beats > 0 ? " later" : " earlier";
    return beats % perBar == 0 ? count (beats / perBar, "bar", "bars") + way : count (beats, "beat", "beats") + way;
}

} // namespace tt::pt

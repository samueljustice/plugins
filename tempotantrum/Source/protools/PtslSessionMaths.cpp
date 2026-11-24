#include "PtslSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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
namespace
{
    std::string trimmed (const std::string& text)
    {
        const auto first = text.find_first_not_of (" \t\r\n");

        if (first == std::string::npos)
            return {};

        return text.substr (first, text.find_last_not_of (" \t\r\n") - first + 1);
    }

    std::vector<std::string> splitTabs (const std::string& line)
    {
        std::vector<std::string> pieces;
        size_t from = 0;

        for (;;)
        {
            const auto tab = line.find ('\t', from);
            pieces.push_back (trimmed (line.substr (from, tab == std::string::npos ? std::string::npos : tab - from)));

            if (tab == std::string::npos)
                return pieces;

            from = tab + 1;
        }
    }

    bool startsWith (const std::string& text, const char* start)
    {
        return text.rfind (start, 0) == 0;
    }

    // A whole number of digits only, with no sign.
    bool digitsOf (const std::string& text, long long& value)
    {
        if (text.empty() || text.size() > 15 || ! std::all_of (text.begin(), text.end(), [] (char c) { return c >= '0' && c <= '9'; }))
            return false;

        value = std::stoll (text);
        return true;
    }
}

double sampleRateOfName (const std::string& name)
{
    const auto underscore = name.find_last_of ('_');
    long long rate = 0;

    if (underscore == std::string::npos || ! digitsOf (name.substr (underscore + 1), rate))
        return 0.0;

    return (double) rate;
}

bool isStoppedState (const std::string& state)
{
    return state == "TS_TransportStopped" || state == "TState_TransportStopped";
}

bool barsBeatsOf (const std::string& text, long long& bar, long long& beat, long long& tick)
{
    long long parts[3] = {};
    size_t start = 0;

    for (int part = 0; part < 3; ++part)
    {
        const size_t line = text.find ('|', start);

        if ((part < 2) == (line == std::string::npos))
            return false;

        // Pro Tools counts bars before bar 1 down through bar 0 and gives them a minus sign
        // ("-21| 3| 155" at the start of a session that starts before bar 1).
        auto piece = trimmed (text.substr (start, line == std::string::npos ? std::string::npos : line - start));
        const bool negative = part == 0 && ! piece.empty() && piece[0] == '-';

        if (! digitsOf (negative ? piece.substr (1) : piece, parts[part]))
            return false;

        if (negative)
            parts[part] = -parts[part];

        start = line + 1;
    }

    bar = parts[0];
    beat = parts[1];
    tick = parts[2];
    return true;
}

bool secondsOfMinSecs (const std::string& text, double& seconds)
{
    const auto clean = trimmed (text);
    const auto colon = clean.find (':');
    long long minutes = 0, whole = 0, fraction = 0;

    if (colon == std::string::npos || ! digitsOf (clean.substr (0, colon), minutes))
        return false;

    const auto rest = clean.substr (colon + 1);
    const auto dot = rest.find ('.');

    if (! digitsOf (rest.substr (0, dot), whole) || whole >= 60)
        return false;

    double part = 0.0;

    if (dot != std::string::npos)
    {
        const auto digits = rest.substr (dot + 1);

        if (! digitsOf (digits, fraction))
            return false;

        part = (double) fraction / std::pow (10.0, (double) digits.size());
    }

    seconds = (double) (minutes * 60 + whole) + part;
    return true;
}

std::vector<EdlEvent> edlEventsOf (const std::string& sessionInfo)
{
    std::vector<EdlEvent> events;
    std::string track;
    int clipColumn = -1, startColumn = -1, endColumn = -1;
    size_t from = 0;

    while (from < sessionInfo.size())
    {
        auto end = sessionInfo.find ('\n', from);

        if (end == std::string::npos)
            end = sessionInfo.size();

        const auto line = sessionInfo.substr (from, end - from);
        from = end + 1;
        const auto clean = trimmed (line);

        if (startsWith (clean, "TRACK NAME:"))
        {
            track = trimmed (clean.substr (11));
            clipColumn = startColumn = endColumn = -1;
            continue;
        }

        // The second channel of a stereo track follows its first after blank lines, with no
        // column names of its own: only the next track's name ends a track's events.
        if (clean.empty() || track.empty())
            continue;

        const auto pieces = splitTabs (line);

        if (startsWith (clean, "CHANNEL"))
        {
            for (int i = 0; i < (int) pieces.size(); ++i)
            {
                if (pieces[(size_t) i] == "CLIP NAME")   clipColumn = i;
                if (pieces[(size_t) i] == "START TIME")  startColumn = i;
                if (pieces[(size_t) i] == "END TIME")    endColumn = i;
            }

            continue;
        }

        if (clipColumn < 0 || startColumn < 0 || endColumn < 0)
            continue;

        if ((int) pieces.size() <= std::max ({ clipColumn, startColumn, endColumn }))
            continue;

        EdlEvent event;
        event.track = track;
        event.channel = (int) std::strtol (pieces[0].c_str(), nullptr, 10);
        event.clip = pieces[(size_t) clipColumn];

        if (digitsOf (pieces[(size_t) startColumn], event.start) && digitsOf (pieces[(size_t) endColumn], event.end) && event.end >= event.start)
            events.push_back (std::move (event));
    }

    return events;
}

MusicPlacement placeMusic (const std::vector<EdlEvent>& events, long long selectionIn, long long selectionOut,
                           const std::vector<ptsl::Clip>& clips, const std::vector<std::string>& fileIds)
{
    MusicPlacement place;
    const long long selectionEnd = std::max (selectionOut, selectionIn + 1);

    std::vector<const EdlEvent*> inside;

    for (const auto& event : events)
        if (event.end > selectionIn && event.start < selectionEnd)
            inside.push_back (&event);

    std::stable_sort (inside.begin(), inside.end(), [] (const EdlEvent* a, const EdlEvent* b)
    {
        return a->start != b->start ? a->start < b->start : a->channel < b->channel;
    });

    const auto clipNamed = [&clips] (const std::string& name) -> const ptsl::Clip*
    {
        for (const auto& clip : clips)
            if (clip.fullName == name)
                return &clip;

        return nullptr;
    };

    const auto offsetOf = [] (const ptsl::Clip& clip, long long& offset)
    {
        if (! clip.sourceStart.known || clip.sourceStart.type != "BTType_Samples")
            return false;

        offset = clip.sourceStart.position;
        return true;
    };

    const EdlEvent* first = nullptr;
    long long fileStart = 0;

    for (const auto* event : inside)
    {
        const auto* clip = clipNamed (event->clip);

        if (clip == nullptr || clip->fileId.empty())
            continue;

        if (! fileIds.empty() && std::find (fileIds.begin(), fileIds.end(), clip->fileId) == fileIds.end())
            continue;

        long long offset = 0;
        const bool known = offsetOf (*clip, offset);

        if (first == nullptr)
        {
            first = event;
            place.found = true;
            place.clipName = event->clip;
            place.fileId = clip->fileId;
            place.clipStart = event->start;
            place.offsetKnown = known;
            place.offset = offset;
            fileStart = event->start - offset;
        }

        if (event->channel != first->channel || clip->fileId != place.fileId)
            continue;

        ++place.clips;
        place.clipEnd = std::max (place.clipEnd, event->end);

        if (known && place.offsetKnown && std::llabs ((event->start - offset) - fileStart) > 1)
            ++place.clipsOffLine;
    }

    return place;
}

SurveyPlan planSurvey (long long musicStart, long long clipEnd, bool materialEndKnown, long long materialEnd, double sampleRate)
{
    SurveyPlan plan;

    if (! (sampleRate > 0.0))
        return plan;

    // The session starts at sample 0: Pro Tools converts sample 0 to the session's start time.
    if (musicStart > 0)
    {
        plan.before.from = 0;
        plan.before.to = musicStart;
        plan.before.use = true;
    }

    const auto cap = (long long) std::llround (2.0 * 3600.0 * sampleRate);

    if (materialEndKnown)
    {
        plan.afterEnd = std::min (materialEnd, cap);
        plan.afterSource = materialEnd > cap ? AfterEnd::cappedAtTwoHours : AfterEnd::lastClip;
    }
    else
    {
        plan.afterEnd = clipEnd + (long long) std::llround (30.0 * 60.0 * sampleRate);
        plan.afterSource = AfterEnd::halfHourAfterMusic;
    }

    if (plan.afterEnd > clipEnd)
    {
        plan.after.from = clipEnd;
        plan.after.to = plan.afterEnd;
        plan.after.use = true;
    }

    return plan;
}

double samplesOffInTicks (long long sampleTicks, long long barBeatTicks, double ticksPerSample)
{
    return ticksPerSample > 0.0 ? (double) (barBeatTicks - sampleTicks) / ticksPerSample : 0.0;
}

std::string describeAfterEnd (AfterEnd source)
{
    switch (source)
    {
        case AfterEnd::lastClip:            return "the end of the last clip in the session";
        case AfterEnd::cappedAtTwoHours:    return "two hours from the session start (the clips go on past it)";
        case AfterEnd::halfHourAfterMusic:  return "30 minutes after the music (the end of the session's clips could not be read)";
    }

    return {};
}

//==============================================================================
std::vector<long long> probePoints (long long from, long long to, double sampleRate)
{
    std::vector<long long> points;

    if (to <= from || ! (sampleRate > 0.0))
        return points;

    const double span = (double) (to - from);
    const int count = std::clamp ((int) std::ceil (span / (2.0 * sampleRate)), 1, maxProbePoints - 1);

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

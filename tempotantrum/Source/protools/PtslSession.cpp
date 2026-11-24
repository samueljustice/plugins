#include "PtslSession.h"

#include "IdentifyBeatWrite.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace tt::pt
{

namespace
{
    const char* const samplesType = "TLType_Samples";

    std::string fileNameOf (const std::string& path)
    {
        const auto slash = path.find_last_of ("/\\:");
        return slash == std::string::npos ? path : path.substr (slash + 1);
    }

    bool noSession (const ptsl::Reply& reply)
    {
        return reply.error.find ("NoOpenedSession") != std::string::npos || reply.message.find ("no open session") != std::string::npos;
    }
}

bool openLink (ptsl::Client& client, std::string& problem)
{
    if (! ptsl::Client::hasLink())
    {
        problem = ptsl::noLinkMessage;
        return false;
    }

    if (client.isConnected() && client.isRegistered())
        return true;

    if (! client.connect (problem))
        return false;

    const auto reply = client.registerConnection ("Sammy J", "Tempo Tantrum");

    if (! reply.ok)
    {
        problem = "Pro Tools did not take the scripting connection: " + reply.message;
        client.disconnect();
        return false;
    }

    return true;
}

//==============================================================================
SessionReading SessionReader::read (ptsl::Client& client, const MusicFacts& music, const std::function<bool()>& stop)
{
    SessionReading r;

    // A connection dropped by a timeout is opened and registered again here.
    if (! openLink (client, r.problem))
        return r;

    r.linked = true;

    // Pro Tools holds commands back while one of its dialogs is open. The first one that is not
    // answered in time ends the reading, so a reader never waits out every command in turn.
    const auto ended = [&] (const ptsl::Reply& reply)
    {
        const bool stopped = reply.stopped || (stop && stop());

        if (! reply.timedOut && ! stopped)
            return false;

        r = {};
        r.linked = true;
        r.timedOut = reply.timedOut;
        r.problem = reply.timedOut ? "Pro Tools did not answer in time. Is a dialog open in Pro Tools?" : "The reading was stopped";
        return true;
    };

    const auto name = client.sessionName();

    if (ended (name.reply))
        return r;

    if (! name.ok())
    {
        r.problem = noSession (name.reply) ? std::string ("No session is open in Pro Tools") : name.reply.message;
        return r;
    }

    r.sessionOpen = true;
    r.sessionName = name.value;

    const auto rate = client.sessionSampleRate();

    if (ended (rate.reply))
        return r;

    if (rate.ok())
        r.sampleRate = rate.value;

    const auto length = client.sessionLength();

    if (ended (length.reply))
        return r;

    if (length.ok())
        r.lengthSeconds = secondsOfTimecode (length.value);

    const auto transport = client.transportState();

    if (ended (transport.reply))
        return r;

    if (transport.ok())
    {
        r.transport = transport.value;
        r.stopped = ptsl::Client::isStopped (transport.value);
    }

    const auto counter = client.mainCounterFormat();

    if (ended (counter.reply))
        return r;

    if (counter.ok())
    {
        r.counterType = counter.value.type;
        r.counterIsSamples = counter.value.type == samplesType;
    }

    // The edit selection; Pro Tools before 2025.06 gives only the timeline selection.
    auto selection = client.editSelection (samplesType);

    if (ended (selection.reply))
        return r;

    if (! selection.ok() && ! noSession (selection.reply))
    {
        selection = client.timelineSelection (samplesType);

        if (ended (selection.reply))
            return r;
    }

    if (selection.ok() && samplesOf (selection.value.in, r.selectionIn))
    {
        r.hasSelection = true;

        if (! samplesOf (selection.value.out, r.selectionOut) || r.selectionOut < r.selectionIn)
            r.selectionOut = r.selectionIn;
    }

    const auto files = client.fileLocations ("SelectedClipsTimeline");

    if (ended (files.reply))
        return r;

    if (files.ok())
    {
        for (const auto& file : files.value)
            r.selectedFiles.push_back (fileNameOf (file.path));

        // A multi-mono clip gives one file for each channel: it is still one clip.
        r.selectedClips = clipsOf (r.selectedFiles);

        if (r.selectedClips.size() == 1)
        {
            r.selectedFileId = files.value.front().fileId;
            r.fileIsMusic = ! music.fileName.empty() && clipNameOf (r.selectedFiles.front()) == clipNameOf (music.fileName);
        }
    }

    if (! r.hasSelection)
        return r;

    // Where the music starts: the clip's start, less how far into its file the clip starts.
    r.hasMusicStart = true;
    r.musicStart = r.selectionIn;

    if (r.selectedClips.size() != 1 || ! (r.sampleRate > 0.0))
        return r;

    const long long clipLength = r.selectionOut - r.selectionIn;
    const double fileToSession = music.sampleRate > 0.0 ? r.sampleRate / music.sampleRate : 1.0;
    const auto musicLength = (long long) std::llround ((double) music.lengthSamples * fileToSession);

    if (r.fileIsMusic && music.lengthSamples > 0 && std::llabs (clipLength - musicLength) <= 2)
    {
        r.trim = Trim::whole;
        return r;
    }

    const auto key = r.selectedFileId + "/" + std::to_string (r.selectionIn) + "/" + std::to_string (r.selectionOut);

    if (key != cachedKey)
    {
        cachedKey = key;
        cachedTrim = Trim::unknown;
        cachedTrimSamples = 0;

        // The clips that use this file and are as long as the selection; they must agree on the offset.
        const auto clips = client.clipList();

        if (ended (clips.reply))
        {
            cachedKey.clear();
            return r;
        }

        if (clips.ok() && ! r.selectedFileId.empty())
        {
            bool found = false, agree = true;
            long long offset = 0;

            for (const auto& clip : clips.value)
            {
                if (clip.fileId != r.selectedFileId || ! clip.sourceStart.known || ! clip.sourceEnd.known
                    || clip.sourceStart.type != "BTType_Samples" || clip.sourceEnd.type != "BTType_Samples")
                    continue;

                const auto sourceLength = (long long) std::llround ((double) (clip.sourceEnd.position - clip.sourceStart.position) * fileToSession);

                if (std::llabs (sourceLength - clipLength) > 2)
                    continue;

                const auto start = (long long) std::llround ((double) clip.sourceStart.position * fileToSession);
                agree = agree && (! found || start == offset);
                offset = start;
                found = true;
            }

            if (found && agree)
            {
                cachedTrim = Trim::found;
                cachedTrimSamples = offset;
            }
        }
    }

    r.trim = cachedTrim;
    r.trimSamples = cachedTrimSamples;

    if (r.trim == Trim::found)
        r.musicStart = r.selectionIn - r.trimSamples;

    return r;
}

//==============================================================================
TempoSurvey surveyTempo (ptsl::Client& client, const std::vector<long long>& samples, int beatsPerBar,
                         const std::function<bool()>& stillAllowed, bool tryTickUnits)
{
    TempoSurvey survey;
    survey.samples = samples;

    const auto allowed = [&]
    {
        return ! stillAllowed || stillAllowed();
    };

    if (samples.empty())
    {
        survey.ok = true;
        return survey;
    }

    // Ticks if Pro Tools gives them, with how many there are to the beat; otherwise bars|beats.
    if (tryTickUnits && allowed())
    {
        const auto first = client.timeAsType ("1|1|000", "TLType_BarsBeats", "TLType_Ticks");
        const auto second = client.timeAsType ("1|2|000", "TLType_BarsBeats", "TLType_Ticks");
        long long a = 0, b = 0;

        if (first.ok() && second.ok() && samplesOf (first.value, a) && samplesOf (second.value, b) && b > a)
        {
            survey.inTickUnits = true;
            survey.ticksPerBeat = b - a;
        }
    }

    for (const auto sample : samples)
    {
        if (! allowed())
        {
            survey.problem = "the survey was stopped";
            return survey;
        }

        long long ticks = 0;
        bool read = false;
        std::string why;

        if (survey.inTickUnits)
        {
            const auto converted = client.timeAsType (std::to_string (sample), samplesType, "TLType_Ticks");
            read = converted.ok() && samplesOf (converted.value, ticks);
            why = converted.ok() ? "\"" + converted.value + "\" is not a number of ticks" : converted.reply.message;
        }
        else
        {
            const auto converted = client.timeAsType (std::to_string (sample), samplesType, "TLType_BarsBeats");
            read = converted.ok() && ticksOfLocation (converted.value, beatsPerBar, ticks);
            why = converted.ok() ? "\"" + converted.value + "\" is not bar|beat|ticks" : converted.reply.message;
        }

        if (! read)
        {
            survey.problem = "sample " + std::to_string (sample) + ": " + why;
            return survey;
        }

        survey.ticks.push_back (ticks);
    }

    survey.ok = true;
    return survey;
}

} // namespace tt::pt

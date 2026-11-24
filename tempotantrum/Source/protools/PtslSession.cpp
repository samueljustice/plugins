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

    // The files of the clips selected on the timeline.
    const auto files = client.fileLocations ("SelectedClipsTimeline");

    if (ended (files.reply))
        return r;

    std::vector<ptsl::FileLocation> selected;

    if (files.ok())
        selected = files.value;

    // Where the music is: the clips of the selected tracks inside the edit selection (from the
    // session info text, in samples) with how far into their file each starts (from the clip
    // list). Read again only when the selection or the selected files change, as the clip list
    // of a large session takes seconds.
    if (r.hasSelection)
    {
        std::vector<std::string> ids;
        std::string key = std::to_string (r.selectionIn) + "/" + std::to_string (r.selectionOut);

        for (const auto& file : selected)
        {
            ids.push_back (file.fileId);
            key += "/" + file.fileId;
        }

        if (key != placedKey)
        {
            bool stopped = false;
            placed = placeOnSelectedTracks (client, r.selectionIn, r.selectionOut, ids, [&] (const ptsl::Reply& reply)
            {
                return stopped = stopped || ended (reply);
            });

            if (stopped)
                return r;

            placedKey = key;
        }

        if (selected.empty() && placed.found && ! placed.file.path.empty())
            selected.push_back (placed.file);
    }

    r.clipFoundBy = ! files.value.empty() ? "selected clips" : ! selected.empty() ? "selected tracks" : "";

    for (const auto& file : selected)
        r.selectedFiles.push_back (fileNameOf (file.path));

    // A multi-mono clip gives one file for each channel: it is still one clip.
    r.selectedClips = clipsOf (r.selectedFiles);

    if (r.selectedClips.size() == 1)
    {
        r.selectedFileId = selected.front().fileId;
        r.selectedFilePath = selected.front().path;
        r.fileIsMusic = ! music.fileName.empty() && clipNameOf (r.selectedFiles.front()) == clipNameOf (music.fileName);
    }

    if (! r.hasSelection)
        return r;

    // Where the music starts: the clip's start, less how far into its file the clip starts.
    r.hasMusicStart = true;
    r.musicStart = r.selectionIn;
    const double fileToSession = music.sampleRate > 0.0 && r.sampleRate > 0.0 ? r.sampleRate / music.sampleRate : 1.0;

    if (placed.found && r.selectedClips.size() <= 1)
    {
        r.placedFromClips = true;
        r.clipName = placed.place.clipName;
        r.clipStart = placed.place.clipStart;
        r.clipEnd = placed.place.clipEnd;
        r.clipsInSelection = placed.place.clips;
        r.clipsOffLine = placed.place.clipsOffLine;
        r.musicStart = r.clipStart;

        if (placed.place.offsetKnown)
        {
            r.trimSamples = (long long) std::llround ((double) placed.place.offset * fileToSession);
            r.trim = r.trimSamples == 0 ? Trim::whole : Trim::found;
            r.musicStart = r.clipStart - r.trimSamples;
        }
        else
        {
            r.trim = Trim::unknown;
        }

        return r;
    }

    if (r.selectedClips.size() != 1 || ! (r.sampleRate > 0.0))
        return r;

    // Without the clips of the selected tracks: a selection as long as the music holds all of it.
    const long long clipLength = r.selectionOut - r.selectionIn;
    const auto musicLength = (long long) std::llround ((double) music.lengthSamples * fileToSession);

    r.trim = r.fileIsMusic && music.lengthSamples > 0 && std::llabs (clipLength - musicLength) <= 2 ? Trim::whole : Trim::unknown;
    return r;
}

SessionReader::Placed SessionReader::placeOnSelectedTracks (ptsl::Client& client, long long selectionIn, long long selectionOut,
                                                            const std::vector<std::string>& fileIds,
                                                            const std::function<bool (const ptsl::Reply&)>& ended)
{
    Placed placed;
    const auto info = client.sessionInfoText (true);

    if (ended (info.reply) || ! info.ok())
        return placed;

    const auto events = edlEventsOf (info.value);
    bool any = false;

    for (const auto& event : events)
        any = any || (event.end > selectionIn && event.start < std::max (selectionOut, selectionIn + 1));

    if (! any)
        return placed;

    const auto clips = client.clipList();

    if (ended (clips.reply) || ! clips.ok())
        return placed;

    placed.place = placeMusic (events, selectionIn, selectionOut, clips.value, fileIds);
    placed.found = placed.place.found;

    if (! placed.found)
        return placed;

    // The file's path, when no clip was in Pro Tools' list of selected clips.
    if (std::find (fileIds.begin(), fileIds.end(), placed.place.fileId) == fileIds.end())
    {
        const auto all = client.fileLocations ("All_Files");

        if (ended (all.reply))
            return placed;

        for (const auto& file : all.value)
            if (file.fileId == placed.place.fileId)
                placed.file = file;
    }

    return placed;
}

bool readMaterialEnd (ptsl::Client& client, long long& end, std::string& problem)
{
    const auto info = client.sessionInfoText (false);

    if (! info.ok())
    {
        problem = info.reply.message;
        return false;
    }

    const auto events = edlEventsOf (info.value);

    if (events.empty())
    {
        problem = "the session info has no clips in samples";
        return false;
    }

    end = 0;

    for (const auto& event : events)
        end = std::max (end, event.end);

    return true;
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

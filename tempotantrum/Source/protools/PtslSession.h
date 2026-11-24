#pragma once

#include "ptsl/PtslClient.h"

#include <functional>
#include <string>
#include <vector>

// What Write to Pro Tools reads from the session through the scripting link: the session's name
// and rate, the transport, the Main Counter, the selected clip and where the music starts, and
// the bars and beats of points across the session, to see whether a write changed the tempo
// elsewhere. Plain C++; every function that talks to Pro Tools blocks, so it is used on a
// background thread. The functions that only work on numbers and names are in
// PtslSessionMaths.cpp, which the tests build without the link.
namespace tt::pt
{

// The music open in Tempo Tantrum.
struct MusicFacts
{
    std::string fileName;
    double sampleRate = 0.0;
    long long lengthSamples = 0;    // at the music's own rate
};

enum class Trim
{
    none,           // no clip, or more than one, is selected
    whole,          // the clip is as long as the music: it starts at the start of the file
    found,          // the clip's offset into its file was read from the clip list
    unknown         // the clip may have been trimmed at its start; how much could not be read
};

struct SessionReading
{
    bool linked = false;            // the scripting link answered
    std::string problem;            // why it did not, or why the session could not be read
    bool timedOut = false;          // Pro Tools stopped answering part way (a dialog may be open in it)

    bool sessionOpen = false;
    std::string sessionName;
    double sampleRate = 0.0;
    double lengthSeconds = 0.0;     // 0: not known

    std::string transport;
    bool stopped = false;

    std::string counterType;        // "TLType_Samples", ...
    bool counterIsSamples = false;

    bool hasSelection = false;      // the edit selection, in samples
    long long selectionIn = 0, selectionOut = 0;

    std::vector<std::string> selectedFiles;     // the file name behind each selected clip
    std::vector<std::string> selectedClips;     // the same, with the mono files of one multi-mono clip as one
    std::string selectedFileId;     // the (first) file of the one selected clip
    std::string selectedFilePath;   // and its path
    bool fileIsMusic = false;       // the one selected clip's file has the music's name

    // "selected clips" when Pro Tools listed the clips selected on the timeline, "selected tracks"
    // when the file was found from the clips on the selected tracks instead. Empty for neither.
    std::string clipFoundBy;

    // The clips of the selected tracks inside the edit selection, with the clip list, placed the
    // music: the first clip's name, where it starts and where the last clip of its file ends.
    bool placedFromClips = false;
    std::string clipName;
    long long clipStart = 0, clipEnd = 0;
    int clipsInSelection = 0;       // clips of the file in the selection, on one channel
    int clipsOffLine = 0;           // of those, how many put the file's start somewhere else (the clips were moved)

    Trim trim = Trim::none;
    long long trimSamples = 0;      // how far into its file the clip starts, in session samples

    bool hasMusicStart = false;     // where the music file starts in the session, in samples
    long long musicStart = 0;
};

// "2880000" or "2,880,000" as a number. False when it is not one. Also reads ticks, which
// Pro Tools gives as a large whole number ("1000141611150").
bool samplesOf (const std::string& text, long long& samples);

// "SR_48000" or "SRate_48000" as 48000. 0 when the name does not end in a rate.
double sampleRateOfName (const std::string& name);

// "TS_TransportStopped" (or "TState_TransportStopped").
bool isStoppedState (const std::string& state);

// "   20| 4| 491" (bars|beats, padded with spaces) into 20, 4 and 491. False when it is not bar|beat|tick.
bool barsBeatsOf (const std::string& text, long long& bar, long long& beat, long long& tick);

// "   1:00.060" (min:sec, padded with spaces) as 60.06 seconds.
bool secondsOfMinSecs (const std::string& text, double& seconds);

// One clip on a track, from the session info text with the track EDLs in samples.
struct EdlEvent
{
    std::string track, clip;
    int channel = 0;                // 1, 2, ... for the channels of a stereo or wider track
    long long start = 0, end = 0;   // in samples from the session start
};

// Every clip of every track in a session info text. Lines whose times are not whole samples
// (the text was not asked for in samples) are left out.
std::vector<EdlEvent> edlEventsOf (const std::string& sessionInfo);

// Whether an event is a fade, "(fade in)", "(fade out)" or "(cross fade)": the session info text
// lists them as events of their own between the clips when asked to show crossfades.
bool isFadeEvent (const EdlEvent& event);

// Where a clip really starts and ends. The session info text gives a clip's times without its
// fades: a fade in or out is inside the clip, so the clip starts where its fade in starts and ends
// where its fade out ends. Under a crossfade the clip's edge is somewhere inside the crossfade; it
// is found from the clip's length (`clipLength`, 0 when not known) when its other end is known,
// and otherwise only the range is known.
struct ClipEdges
{
    bool startKnown = false;
    long long start = 0;                    // the start when known, otherwise the start the text gives
    long long earliest = 0, latest = 0;     // the range the start is in
    bool endKnown = false;
    long long end = 0;                      // the end when known, otherwise the end the text gives
};

ClipEdges clipEdges (const std::vector<EdlEvent>& events, const EdlEvent& clip, long long clipLength);

// The music on the timeline, from the clips (EDL events) inside an edit selection and the clip
// list, with their fades put back (clipEdges): the first clip, in time, that is in the list with a file (one of `fileIds`, when any
// are given). Its offset into its file is the clip list's source start, in the file's samples.
// The other clips of the same file on the same channel are counted, with how many of them put
// the start of the file somewhere else (they were moved after the file was cut up).
struct MusicPlacement
{
    bool found = false;
    std::string clipName, fileId;
    long long clipStart = 0, clipEnd = 0;   // the first clip's start, the last clip's end (samples)
    bool offsetKnown = false;
    long long offset = 0;                   // how far into its file the first clip starts (file samples)
    int clips = 0, clipsOffLine = 0;
};

MusicPlacement placeMusic (const std::vector<EdlEvent>& events, long long selectionIn, long long selectionOut,
                           const std::vector<ptsl::Clip>& clips, const std::vector<std::string>& fileIds);

// Connects and registers ("Sammy J", "Tempo Tantrum") when the client is not connected. False,
// with a sentence, when the link cannot be used.
bool openLink (ptsl::Client& client, std::string& problem);

// Reads everything above. The clips of the selected tracks and the clip list are read only when
// the edit selection or the selected files change, and kept while they stay the same.
// A read ends at the first command Pro Tools does not answer in time, with timedOut set and the
// session not counted as open, and when `stop` returns true (it is asked between commands).
class SessionReader
{
public:
    SessionReading read (ptsl::Client& client, const MusicFacts& music, const std::function<bool()>& stop = {});

private:
    struct Placed
    {
        bool found = false;
        MusicPlacement place;
        ptsl::FileLocation file;    // the file's path, when no clip was in the list of selected clips
    };

    std::string placedKey;
    Placed placed;

    static Placed placeOnSelectedTracks (ptsl::Client& client, long long selectionIn, long long selectionOut,
                                         const std::vector<std::string>& fileIds, const std::function<bool (const ptsl::Reply&)>& ended);
};

// The end of the last clip on any track, in samples, from the session info text of every track.
// False when the text could not be read or has no clips in it.
bool readMaterialEnd (ptsl::Client& client, long long& end, std::string& problem);

// A duration like "24:00:00:00" or "00:03:00:00.00" in whole seconds (frames are left out).
double secondsOfTimecode (const std::string& text);

// Seconds as "3:12".
std::string minutesAndSeconds (double seconds);

// A file name without its extension and without the channel ending of a multi-mono file, in
// lower case: "Cue 3.L.wav", "Cue 3.R.wav" and "cue 3.wav" all give "cue 3".
std::string clipNameOf (const std::string& fileName);

// The files behind the selected clips, as clips: the mono files of one multi-mono clip ("Cue.L.wav",
// "Cue.R.wav") become one entry, "Cue (L, R)". Other names are kept as they are, in order.
std::vector<std::string> clipsOf (const std::vector<std::string>& fileNames);

//==============================================================================
// Where a write places the music. Music captured from Pro Tools ("Get from Pro Tools", or the
// AudioSuite capture) starts at the selection it was captured from: its time 0 is that sample on
// the timeline, whatever clips made it up, so no clip is matched. Music opened from a file is
// placed by its clip in the session, read when the run starts.
enum class StartFrom
{
    typed,          // a sample typed under More, which always wins
    capture,        // the start of the selection the music was captured from
    session         // read from Pro Tools during the run (the selected clip, or else the cursor)
};

struct MusicStart
{
    StartFrom from = StartFrom::session;
    bool known = false;             // a sample is given before the run (typed or captured)
    long long sample = 0;
};

MusicStart musicStartFor (bool typed, long long typedSample, bool captured, bool captureStartKnown, long long captureStart);

// What the write log says about where the start came from.
std::string describeStart (StartFrom from);

// Whether a capture's session is not the one open now: both names known and different.
bool otherSession (const std::string& capturedIn, const std::string& openNow);

//==============================================================================
// The tempo of the rest of the session.

// About one point every two seconds from `from` to `to` (samples), at most maxProbePoints, both
// ends included. At about 13 ms a conversion, 400 points take about 5 s.
inline constexpr int maxProbePoints = 400;
std::vector<long long> probePoints (long long from, long long to, double sampleRate);

// Where the session's material is, for the surveys before and after the music. The session
// length Pro Tools gives is a default duration (24 hours), not where the material ends, so it
// is not used.
enum class AfterEnd
{
    lastClip,               // the end of the last clip on any track
    cappedAtTwoHours,       // the clips go on past two hours from the session start
    halfHourAfterMusic      // the clips could not be read: 30 minutes after the music
};

struct SurveySpan
{
    bool use = false;
    long long from = 0, to = 0;
};

struct SurveyPlan
{
    SurveySpan before, after;
    long long afterEnd = 0;
    AfterEnd afterSource = AfterEnd::halfHourAfterMusic;
};

// Before: the session start (sample 0) to the music start, when the music starts later.
// After: the end of the music's clip to the end of the last clip (at most two hours from the
// session start), or to 30 minutes after the music when that end is not known.
SurveyPlan planSurvey (long long musicStart, long long clipEnd, bool materialEndKnown, long long materialEnd, double sampleRate);
std::string describeAfterEnd (AfterEnd source);

// The check after a write, in the session's own ticks: the pin's sample and the pin's bar|beat
// are both turned into ticks (exact, unlike bar|beat to samples, which rounds to the display's
// ticks), and the difference is given in samples with the local ticks per sample.
double samplesOffInTicks (long long sampleTicks, long long barBeatTicks, double ticksPerSample);

struct TempoSurvey
{
    bool ok = false;
    std::string problem;
    std::vector<long long> samples;
    std::vector<long long> ticks;   // where each sample is in the session's bars and beats
    long long ticksPerBeat = 960;
    bool inTickUnits = false;       // read as TLType_Ticks; otherwise as bars|beats|ticks
};

// Reads the bars and beats of each point. `beatsPerBar` is used only when the ticks are worked out
// from bars|beats. `stillAllowed` is asked before every call and ends the survey when false.
TempoSurvey surveyTempo (ptsl::Client& client, const std::vector<long long>& samples, int beatsPerBar,
                         const std::function<bool()>& stillAllowed, bool tryTickUnits = true);

struct TempoChange
{
    bool compared = false;
    std::string problem;            // why it could not be compared

    bool changed = false;           // the tempo between two points changed
    long long firstSample = 0, lastSample = 0;   // the stretch that changed

    bool shifted = false;           // every point moved by the same number of ticks
    long long shiftTicks = 0;
    long long shiftBeats = 0;       // the shift in whole beats, rounded
    bool partBeat = false;          // the shift is not a whole number of beats: the beats no longer sit on the same samples
};

// Compares two surveys of the same points. Differences between neighbouring points that moved by
// more than 2 ticks (in the 960-to-the-beat ticks of the bar|beat display) are a change. A shift
// of every point by the same amount is not a change in tempo, but is marked partBeat when it is
// more than 2 ticks from a whole number of beats.
TempoChange compareTempo (const TempoSurvey& before, const TempoSurvey& after);

// A whole-beat shift as "2 bars later" or "3 beats earlier": bars when it is a whole number of
// them. Empty for no shift.
std::string describeShift (long long beats, int beatsPerBar);

} // namespace tt::pt

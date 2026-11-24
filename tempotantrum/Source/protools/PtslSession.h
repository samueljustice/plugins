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
    bool fileIsMusic = false;       // the one selected clip's file has the music's name

    Trim trim = Trim::none;
    long long trimSamples = 0;      // how far into its file the clip starts, in session samples

    bool hasMusicStart = false;     // where the music file starts in the session, in samples
    long long musicStart = 0;
};

// Connects and registers ("Sammy J", "Tempo Tantrum") when the client is not connected. False,
// with a sentence, when the link cannot be used.
bool openLink (ptsl::Client& client, std::string& problem);

// Reads everything above. The clip list is read only when the selected clip is shorter than the
// music, and is kept for as long as the same clip stays selected.
// A read ends at the first command Pro Tools does not answer in time, with timedOut set and the
// session not counted as open, and when `stop` returns true (it is asked between commands).
class SessionReader
{
public:
    SessionReading read (ptsl::Client& client, const MusicFacts& music, const std::function<bool()>& stop = {});

private:
    std::string cachedKey;
    Trim cachedTrim = Trim::unknown;
    long long cachedTrimSamples = 0;
};

// "2880000" or "2,880,000" as a number. False when it is not one.
bool samplesOf (const std::string& text, long long& samples);

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
// The tempo of the rest of the session.

// About one point every two seconds from `from` to `to` (samples), at most 300, both ends included.
std::vector<long long> probePoints (long long from, long long to, double sampleRate);

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

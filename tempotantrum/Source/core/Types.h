#pragma once

#include <string>
#include <vector>

// The data the engine and the window pass between them. Plain C++, no JUCE.
//
// Times are seconds from the start of the music file. Beats are quarter notes counted from
// bar 1 beat 1 = 0.0, so bar 40 beat 1 in 4/4 is beat 156.0 and beat 2.5 is the "and" of 3.
namespace tt
{

// A detected hit: the start of its attack, and how strong it is (0 to 1).
struct Onset
{
    double time = 0.0;
    float strength = 0.0f;
};

// A point where the grid is fixed to the audio. Between two pins the tempo is constant.
struct Pin
{
    double beat = 0.0;
    double time = 0.0;
    bool user = false;      // placed or moved by hand: the engine must keep it exactly
    bool restart = false;   // the beat starts afresh here (first pin of a section)
};

// A stretch of music with a continuous beat. A new section starts where the beat stops and
// comes back somewhere unrelated: after a gap with no hits, at an edit, or where the user says.
struct Section
{
    enum class Kind { first, gap, jump, user };

    Kind kind = Kind::first;
    double startTime = 0.0;      // time of the section's first hit
    double endTime = 0.0;        // time of its last tracked beat
    double startBeat = 0.0;      // beat given to the first hit
    double bpm = 0.0;            // the section's average tempo
    bool confirmed = false;      // the user has set or accepted startBeat
};

// Something the user should look at before trusting the grid.
struct Check
{
    enum class Kind { restart, noHits, unsure };

    Kind kind = Kind::unsure;
    double startTime = 0.0;
    double endTime = 0.0;
    std::string note;            // one short line for the window, e.g. "Beat restarts. Bar 46?"
};

// What the user tells the engine about the music.
struct Hint
{
    double firstHitTime = -1.0;  // below zero: use the first strong hit
    int bar = 1;                 // the bar and beat the first hit is given
    int beat = 1;
    double bpm = 0.0;            // zero: estimate it. Otherwise a rough tempo, refined from the audio
    int beatsPerBar = 4;
};

struct Options
{
    enum class Bridge { wholeBeat, halfBeat };

    Bridge bridge = Bridge::wholeBeat;   // how the beat count across a gap is rounded
    bool detectJumps = true;             // look for restarts with no silence
    double pinToleranceMs = 10.0;        // add pins until every snapped beat is this close to the grid
};

struct MapResult
{
    std::vector<Onset> onsets;
    std::vector<Pin> pins;               // sorted by time; beats strictly increasing
    std::vector<Section> sections;
    std::vector<Check> checks;
    double bpm = 0.0;                    // the tempo the engine settled on for the first section
    double firstHitTime = 0.0;
};

} // namespace tt

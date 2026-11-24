#pragma once

#include "Types.h"

#include <cstddef>
#include <string>
#include <vector>

// The listening engine: finds the hits, follows the beat section by section and chooses the pins.
// Implemented in Onsets.cpp, SectionMapper.cpp, BeatGrid.cpp and Click.cpp.
namespace tt
{

// Finds the hits in mono audio. The detection may run on a drum stem while `refineOn` (the original
// mix, same length and rate, or nullptr) is used to place each hit on the start of its attack.
std::vector<Onset> findOnsets (const float* mono, std::size_t numSamples, double sampleRate,
                               const float* refineOn = nullptr);

// Maps the music. `userPins` are hard: every one is kept exactly as given. A user pin with
// `restart` set starts a new section at that time with that beat. A user pin within 30 ms of a
// restart the engine found sets that restart's beat (the user confirming or correcting it).
// User pins must move forward in both time and beat. Of two that do not, the one given later in
// the list is kept and the other is left out, with a check at its time.
MapResult mapCue (const std::vector<Onset>& onsets, double durationSeconds, const Hint& hint,
                  const std::vector<Pin>& userPins = {}, const Options& options = {});

// A rough tempo from the hits alone, for when the user gives none. Zero if nothing periodic is found.
double estimateTempo (const std::vector<Onset>& onsets, double durationSeconds);

// The grid the pins define: straight lines between pins, carried on at the nearest tempo outside them.
class BeatGrid
{
public:
    BeatGrid() = default;
    BeatGrid (std::vector<Pin> pins, double fallbackBpm);

    bool empty() const                  { return pins.empty(); }
    double timeAtBeat (double beat) const;
    double beatAtTime (double time) const;
    double bpmAtTime (double time) const;
    const std::vector<Pin>& getPins() const { return pins; }

private:
    std::vector<Pin> pins;
    double fallbackBpm = 120.0;
};

// Renders a click for the grid at the file's sample rate: a short burst on every beat, higher
// and louder on beat 1 of each bar, each starting exactly on its beat.
std::vector<float> renderClick (const BeatGrid& grid, int beatsPerBar, double sampleRate,
                                std::size_t numSamples);

// "40|1|000" for a beat position (bar | beat | ticks, 960 ticks to the beat, as Pro Tools shows it).
std::string formatBarBeat (double beat, int beatsPerBar);

} // namespace tt

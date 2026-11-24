#pragma once

#include "Engine.h"

// Private to the engine and its tools: the mapper's constants and an entry point that takes them.
namespace tt::detail
{

struct Tuning
{
    double thrOn = 0.15;          // onset strength counted as a "confident" onset for gap finding
    double thrStrong = 0.30;      // strength a restart hit needs
    double gapBeats = 1.5;        // onset-free stretch (beats) that ends a section
    int minSec = 3;               // a section needs this many snapped beats, else it is treated as part of the gap
    double rescueMs = 20.0;       // a strong onset this close to an unsnapped beat line is taken as that beat (0 = off)
    int look = 12;                // beats of look-ahead when choosing a restart hit after a gap (3 bars)
    double jTolMs = 15.0;         // onset-to-line tolerance in the support tests
    int jAfter = 8;               // jump test: beats compared after the change point (2 bars)
    int jAfterShort = 4;          // least look-ahead (1 bar) allowed when the section ends sooner
    int jBefore = 8;              // jump test: beats of history examined
    double jMargin = 2.0;         // shifted grid support must beat the continuing grid by this factor
    double jMinShiftMs = 30.0;    // smallest phase shift called a jump (smaller ones the tracker absorbs)
    double jLock = 0.6;           // share of the last beats that had an onset (grid was locked)
    double jAfterMin = 0.75;      // shifted grid: at least this share of lines has an onset (every line when the look-ahead is short)
    double jBeforeMax = 0.4;      // support the shifted phase had before the change point, relative to after

    // Not in the prototype.
    bool refineTempo = true;      // settle the hinted tempo on the audio before tracking
    double refineRange = 0.06;    // search this far either side of the hint
    double refineStep = 0.001;    // in steps of this share of the hint
    int refineBeats = 16;         // over at most this many beats from the first hit
    int refineMinBeats = 5;       // and not at all with fewer than this many
    double refineKeepHint = 1.15; // another tempo replaces the hint only when its support is this much better
    double userSnapMs = 30.0;     // a user pin this close to a section start sets that start
    double unsureOnHit = 0.4;     // a section of two bars or more with a smaller share of its beats on a strong hit gets a check

    // The constant-tempo test (steadyHead in SectionMapper.cpp) and the regular pins.
    bool steady = true;           // try one constant tempo for a stretch before following it hit by hit
    int steadyMinBeats = 8;       // a constant grid needs this many beats on a hit to be used
    int steadyFirst = 16;         // beats fitted first; the fit is then carried on over twice as many, and so on
    double steadyWideMs = 35.0;   // a hit this close to a line, but outside the pin tolerance, is a beat that is off the grid
    int steadyRun = 4;            // this many beats in a row early, or late, end the constant grid
    int steadyRunStrong = 3;      // or this many strong hits in a row
    double steadyLimit = 0.6;     // early or late in that test: by more than this share of the pin tolerance
    double steadyFloor = 0.25;    // or, where the hits sit very evenly, by as little as this share of it
    double steadyAround = 0.4;    // a hit within this share of a beat of a line is near it, though not on it or off it
    int steadyLostWindow = 16;    // a constant grid is left where, of this many lines in a row with a hit near them,
    double steadyLostShare = 0.25; // no more than this share have the hit on them
    int steadyCarry = 3;          // after a gap, this many beats on the grid carried across it keep the section going
    double regularNear = 0.3;     // a regular pin goes on a hit within this share of the pin tolerance of the grid
    double regularTempo = 0.0005; // and close enough that the tempo either side of it changes by no more than this share
};

MapResult mapCueWith (const std::vector<Onset>& onsets, double durationSeconds, const Hint& hint,
                      const std::vector<Pin>& userPins, const Options& options, const Tuning& tuning);

} // namespace tt::detail

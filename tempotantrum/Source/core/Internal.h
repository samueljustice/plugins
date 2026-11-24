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
};

MapResult mapCueWith (const std::vector<Onset>& onsets, double durationSeconds, const Hint& hint,
                      const std::vector<Pin>& userPins, const Options& options, const Tuning& tuning);

} // namespace tt::detail

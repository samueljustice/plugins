// Measurements shared by dehonk_tests and dehonk_render: the harness's own filters, the impact
// figures of SPEC 13.2 (Body, Tail, Front), notch events from the display frames with note names
// and key-profile ranks (13.4), and a plain reference limiter for the headroom report (13.6).
// Nothing here uses the DSP core's own filters.
#pragma once

#include "Audio.h"
#include "Render.h"

#include <array>
#include <string>
#include <vector>

namespace dh
{

//==============================================================================
// Filters

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    static Biquad highpass (double sr, double f0, double q);
    static Biquad lowpass (double sr, double f0, double q);
    double tick (double x)
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

/** 4th-order Butterworth high-pass. */
std::vector<double> highpass4 (const std::vector<float>& x, double sr, double hz);
/** 4th-order Butterworth high-pass at loHz followed by a 4th-order Butterworth low-pass at hiHz. */
std::vector<double> bandpass8 (const std::vector<float>& x, double sr, double loHz, double hiHz);
/** bandpass8 of every channel. */
std::vector<std::vector<double>> bandpass8 (const Audio& a, double loHz, double hiHz);

//==============================================================================
// Notes

/** Note name for a frequency (A = 440 Hz, sharps: C, C#, D, ... B, with the octave number as in
    scientific pitch, so 261.6 Hz is C4) and the cents from that note, -50 to +50. */
struct Note
{
    int pitchClass = 0;   // 0 = C
    int octave = 4;
    double cents = 0.0;
    std::string name() const;   // "C#3"
};
Note noteOf (double hz);
const char* pitchClassName (int pc);

/** Rank of each pitch class in a profile: rank[pc] = 1 for the strongest, 12 for the weakest. */
std::array<int, 12> pitchClassRanks (const std::array<float, 12>& profile);

//==============================================================================
// Notch events from display frames

/** One notch: a band whose cut rose above openDb, until it fell under closeDb and stayed there
    for holdSec (or the file ended). A dip under closeDb that ends within holdSec, or a fall to
    between closeDb and openDb, is part of the same notch. closeSec is when the cut went under
    closeDb for good. */
struct NotchEvent
{
    int band = 0;
    double hz = 0.0;
    double openSec = 0.0, closeSec = 0.0;
    double deepestDb = 0.0;
    int pcRankAtOpen = 0;                 // 1 = strongest pitch class in DisplayFrame::pitchClass when it opened;
                                          // 0 = unknown: it opened before rankFromSec, while the profile was still filling
    std::array<float, 12> profileAtOpen {};
};

struct NotchRule
{
    double openDb = 1.0;
    double closeDb = 0.5;
    double holdSec = 0.1;
    double rankFromSec = 0.0;   // give a rank only to notches opening at or after this (Tuning::keySeconds)
};

/** The frames should be read at every tick (RenderSpec::frameEveryTick) for exact open and close
    times; at one frame per block they are quantised to the block. */
std::vector<NotchEvent> notchEvents (const RenderResult& r, const NotchRule& rule = {});

//==============================================================================
// Impacts (SPEC 13.2)

/** Tail is a decay time taken from a smooth curve, not from a threshold crossing of the
    envelope (which jumps as the beat lobes of close modes cross the line). The band signal's
    power, less the power the band had before the onset (the bed), is integrated backwards from
    the end of the window (Schroeder): E(t) = sum from t to the end. 10 log10 (E(t) / E(onset))
    falls smoothly and monotonically. A least-squares line is fitted to it from fitStartDb down
    to fitStartDb + tailFallDb (5 to 25 dB), and Tail is the time that line takes to fall
    tailFallDb (20 dB). Where the bed is close, the curve is only followed while the band's
    envelope stays floorMarginDb above the bed, so the fit stops higher up; the same span is
    used for input and output. The line's slope is what is compared, so the figure does not
    depend on where a beat lobe happens to cross a level. */
struct ImpactFigures
{
    double bodyDb = 0.0;        // energy 110-400 Hz over bodySec after the onset, output against input, dB
    double frontDb = 0.0;       // largest |sample| of the 110-400 Hz band in the first frontSec, output against input, dB
    double tailInMs = -1.0;     // time the fitted decay line takes to fall tailFallDb; -1 = not measurable
    double tailOutMs = -1.0;
    double tailChangePct = 0.0; // (out - in) / in, percent; 0 when not measurable
    double fallDb = 0.0;        // the span of the decay curve fitted, dB (0 or more): tailFallDb, or less where the bed is close
    bool tailMeasurable = false;
    double peakInDb = -400.0;   // band envelope peak in the first 300 ms, input, dBFS (envelopeMs RMS)
    double floorInDb = -400.0;  // band power over the 400 ms before the onset (to 30 ms before it), input, dBFS
};

struct ImpactSpec
{
    double loHz = 110.0, hiHz = 400.0;
    double bodySec = 1.5;
    double frontSec = 0.030;
    double fitStartDb = 5.0;     // the decay line is fitted from this far down the integrated curve
    double tailFallDb = 20.0;    // ... to this much further down, and Tail is the time it takes to fall this much
    double tailMaxSec = 2.4;     // the tail is followed at most this long after the onset (or to nextOnset)
    double envelopeMs = 20.0;    // the band envelope (for the peak and the bed guard) is the RMS over this long
    double floorMarginDb = 6.0;  // the curve is used only while the band envelope is this far above the bed
    double minFallDb = 8.0;      // a fitted span shorter than this is reported as not measurable
};

/** Band-passed input and output (from bandpass8), all channels, so many impacts can share them. */
struct BandPair
{
    std::vector<std::vector<double>> in, out;
    double sampleRate = 48000.0;
};
BandPair bandPair (const Audio& in, const Audio& out, double loHz, double hiHz);

/** Figures for one onset. nextOnset bounds the tail search (0 = none). */
ImpactFigures impactFigures (const BandPair& bp, std::size_t onset, std::size_t nextOnset, const ImpactSpec& spec = {});

/** Front only, for a second render at another Speed. */
double frontChangeDb (const BandPair& bp, std::size_t onset, double frontSec = 0.030);

//==============================================================================
// Reference limiter (SPEC 13.6)

struct LimiterResult
{
    Audio out;
    std::vector<float> grDb;   // gain reduction per sample, dB, positive
    double meanGrDb = 0.0;     // over every sample
    double meanGrWhileLimitingDb = 0.0;
    double maxGrDb = 0.0;
    double pctLimiting = 0.0;  // share of samples with more than 0.01 dB of reduction
};

/** Look-ahead lookaheadMs, instant attack to the largest peak in the look-ahead, one-pole release
    of releaseMs (on the gain in dB), ceiling in dBFS, linked across channels. Offline, so the
    look-ahead reads future samples and adds no delay. */
LimiterResult referenceLimiter (const Audio& in, double ceilingDb = -1.0, double lookaheadMs = 5.0, double releaseMs = 100.0);

/** Short-term level: RMS over windowSec windows hopping by hopSec, all channels, dBFS. */
std::vector<double> shortTermDb (const Audio& a, double windowSec = 0.4, double hopSec = 0.1);

} // namespace dh

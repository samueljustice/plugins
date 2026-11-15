#pragma once

// Pitch Flattener's core: what a learned sound's pitch does over its whole length, and the
// sound rendered with that movement taken out.
//
// The plug-in works on a sound it has heard all of. analyse() reads its pitch as one continuous
// line (two readings of it, see below); flatten() then plays the sound back at a speed that
// changes from moment to moment, slower where it is above the target and faster where it is
// below, the way slowing and speeding a tape would. That makes the pitch exactly as flat as the
// line is right, however fast the pitch bends, with nothing smeared and no warble. The length
// changes with it, and Flattened carries the map from the sound's own time to the result's.
//
// Two lines. No one tracker reads every sound: the harmonic tracker reads voices, engines and
// whines up to about 1 kHz, the ridge tracker follows the strongest partial of anything
// (sirens and whistles above that, a whine in a burst of noise). Both are kept. analyse() says
// which one flattens the sound better (it flattens with each and reads the results back), and
// the user can overrule it, because that judgement is sometimes wrong and the ear is not.
//
// Plain C++20, no JUCE, no global state. Everything here allocates and can take seconds on a
// long sound: for a background thread, never the audio thread. Results depend only on the
// samples, their rate and the options.

#include <array>
#include <functional>
#include <vector>

namespace pitchflattener::core
{

using Channels = std::vector<std::vector<float>>;   // every channel the same length
using Cancel = std::function<bool()>;               // asked now and then; true stops the work

enum class Tracker : int { Harmonic = 0, Ridge = 1 };
inline constexpr int kNumTrackers = 2;

/** One reading of a sound's pitch over its length. */
struct PitchLine
{
    bool usable = false;              // enough of the sound has a pitch to flatten by this line
    Tracker tracker = Tracker::Harmonic;
    double hopSeconds = 0.005;        // frame f is the sound around (f + 0.5) * hopSeconds

    std::vector<float> trackedHz;     // as the tracker read it; 0 where it found no pitch
    // The line the flattener uses, as MIDI note numbers: the tracked line with its leaps taken
    // out, its gaps bridged and its jitter smoothed. One value per frame, never 0.
    std::vector<float> midi;

    double coverage = 0.0;            // share of the audible sound the tracker found a pitch in
    int leaps = 0;                    // leaps taken out of the line (the tracker stepping between partials)
    // How unsteady the reading is from frame to frame, in semitones (the typical change in its
    // slope): near 0 for a clear pitch, large when the tracker is wandering through noise.
    double jitter = 0.0;

    // Where the line sits, counted by level so a loud part outweighs a long quiet one.
    double lowMidi = 0.0, centreMidi = 0.0, highMidi = 0.0;   // 5th, 50th and 95th percentile
    double startMidi = 0.0;           // the first 0.3 s that has a pitch

    // How far the sound's pitch still travels after flattening by this line, in semitones (two
    // trackers' readings of the result added together). -1 = not measured.
    double residual = -1.0;
};

struct Analysis
{
    bool valid = false;               // false: nothing to read (too short, silent, cancelled)
    double sampleRate = 0.0;
    double durationSeconds = 0.0;
    std::array<PitchLine, kNumTrackers> lines;
    int pick = -1;                    // the line that flattens it better; -1 = no steady pitch found

    const PitchLine* picked() const noexcept { return pick >= 0 ? &lines[static_cast<std::size_t>(pick)] : nullptr; }
};

/**
 * Reads the sound's pitch with both trackers. With score set, each usable line is also tried
 * (the sound flattened by it and read back) to choose between them; without, the pick is the
 * harmonic line if usable, else the ridge line.
 */
Analysis analyse(const Channels& sound, double sampleRate, bool score = true, const Cancel& cancelled = {});

//==============================================================================
enum class Target : int
{
    HighPoint,    // the top of the sound's pitch: a pass-by's approach, a siren's peak. The result is never longer than the sound.
    Centre,       // where it spends its time: the least change overall
    LowPoint,
    Start,        // the pitch it opens on
    Note          // a chosen pitch
};

/** A hand correction to the line: at this moment of the sound its pitch is this much off what was read. */
struct CurvePoint
{
    double seconds = 0.0;
    double semitones = 0.0;
};

struct Options
{
    int line = -1;                    // a Tracker (if that line is not usable the pick is used), or -1 for the analysis' own pick
    Target target = Target::HighPoint;
    double noteMidi = 69.0;           // for Target::Note
    double amount = 1.0;              // 1 = flat, 0.5 = half the movement left in, 0 = untouched
    // Hand corrections, added to the line. Between points they run in straight lines; they
    // ease to nothing over 50 ms beyond the first and last.
    std::vector<CurvePoint> edits;
};

/** The line these options use (null if none is usable). */
const PitchLine* lineFor(const Analysis& analysis, const Options& options) noexcept;
/** The pitch the sound is flattened to. */
double targetMidi(const PitchLine& line, const Options& options) noexcept;
/** What the hand corrections add at one moment. */
double editAt(const std::vector<CurvePoint>& edits, double seconds) noexcept;

struct Flattened
{
    bool valid = false;               // false: no usable line, or cancelled
    double sampleRate = 0.0;
    Channels channels;
    double targetMidi = 0.0;
    float gain = 1.0f;                // what the whole result was turned down by to stay under full scale (1 = nothing)

    // The time map: outputAtSource[k] is the result's sample position where source sample
    // k * kMapStep is heard. Rising; the last entry is for the end of the sound.
    static constexpr int kMapStep = 256;
    std::vector<double> outputAtSource;

    /** Where in the result a moment of the sound is heard (both in samples). */
    double outputSampleFor(double sourceSample) const noexcept;
    /** The other way: which moment of the sound a sample of the result is. */
    double sourceSampleFor(double outputSample) const noexcept;
};

/** Renders the sound with its pitch movement taken out. */
Flattened flatten(const Channels& sound, double sampleRate, const Analysis& analysis, const Options& options, const Cancel& cancelled = {});

} // namespace pitchflattener::core

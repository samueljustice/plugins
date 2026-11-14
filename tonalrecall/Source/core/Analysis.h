#pragma once

// Tonal Recall's analysis: what notes a piece of music sits on, what pitch a sound effect has,
// and the shift (fixed, plus a bend over time) that puts the effect on one of the music's notes.
//
// Plain C++20, no JUCE, no global state. Everything here allocates and can take a second or more
// on long buffers: call it from a background thread, never from the audio thread. The results
// depend only on the samples, their rate and the options.

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace tonalrecall::core
{

inline double hzToMidi(double hz) noexcept { return 69.0 + 12.0 * std::log2(hz / 440.0); }
inline double midiToHz(double midi) noexcept { return 440.0 * std::exp2((midi - 69.0) / 12.0); }

/** "C".."B" with sharps. */
const char* pitchClassName(int pitchClass) noexcept;
/** The nearest note's name ("B2") and how far the pitch is from it, in cents (-50..50). */
std::string noteName(double midi, int* centsOut = nullptr);

//==============================================================================
// Music

enum class NoteRole { Root, Third, Fifth, Other };

struct NoteCandidate
{
    int pitchClass = 0;         // 0 = C
    float strength = 0.0f;      // 0..1, relative to the strongest note of the music
    NoteRole role = NoteRole::Other;
    double midi = 0.0;          // where this note is strongest (its octave), tuning included
};

struct MusicAnalysis
{
    bool valid = false;         // false: too short or silent
    double durationSeconds = 0.0;

    // The music as a whole.
    int rootPitchClass = 0;     // the note the music rests on
    bool minor = false;
    float keyConfidence = 0.0f; // 0..1
    float tuningCents = 0.0f;   // how far the music sits from A = 440 Hz equal temperament (-50..50)
    std::array<float, 12> chroma {};      // strength of each pitch class, the strongest = 1
    std::vector<NoteCandidate> notes;     // the notes worth matching to, strongest first
    double rootMidi = 0.0;      // the root in the octave where it is strongest, tuning included

    // For the display: note strength over time. Frame f, pitch p is lane[f * lanePitches + p],
    // 0..1; pitch p is the note laneLowMidi + p (on the music's own tuning).
    double hopSeconds = 0.0;
    int laneLowMidi = 24;
    int lanePitches = 0;
    int laneFrames = 0;
    std::vector<float> lane;

    // How far the root note sits from its own average, in cents, per frame (risers, glides,
    // detuning). Held across frames where the root is not sounding.
    std::vector<float> rootDriftCents;

    // The lead line: the note that carries the music at each moment, as a run of notes with
    // their times (seconds from the start of the music; each note lasts until the next starts,
    // the last to the end). One entry when the music holds a single note throughout.
    struct LeadNote
    {
        double startSeconds = 0.0;
        double midi = 0.0;        // tuning included
    };
    std::vector<LeadNote> leadNotes;

    // The music's continuous pitch movement: the pitch of its lead voice, followed finely through
    // glides, scoops and bends (not rounded to notes). contourMidi[i] is the pitch at
    // contourStartSeconds + i * contourHopSeconds. Empty when no lead was found.
    std::vector<float> contourMidi;
    double contourStartSeconds = 0.0, contourHopSeconds = 0.0;

    // How strongly each pitch class sounds in each analysis frame (hopSeconds apart): frame f,
    // pitch class p is frameNoteLevels[f * 12 + p], 0..255 against that frame's strongest note.
    // A frame of all zeros was too quiet to say.
    std::vector<std::uint8_t> frameNoteLevels;

    std::string keyName() const;          // "D minor"
    /** The pitch (tuning included) of a pitch class in the octave nearest to nearMidi. */
    double pitchNear(int pitchClass, double nearMidi) const noexcept;
};

MusicAnalysis analyseMusic(const float* mono, int numSamples, double sampleRate);

//==============================================================================
// Sound effect

enum class SfxMethod
{
    Auto,       // Tonal, or Engine where that fits better; Resonance if neither finds a pitch
    Tonal,      // voices, single tones
    Engine,     // sounds whose partials all move together (engines, rich drones)
    Resonance   // unpitched sounds: the strongest ring of the average spectrum
};

struct SfxAnalysis
{
    bool valid = false;
    SfxMethod usedMethod = SfxMethod::Tonal;
    double durationSeconds = 0.0;
    double hopSeconds = 0.005;
    std::vector<float> hz;        // per frame; 0 where no pitch
    bool pitched = false;         // enough of it has a pitch to match on
    double centreMidi = 0.0;      // the pitch the effect sits on
    float driftSemitones = 0.0f;  // how far its pitch travels (5th to 95th percentile)
    float tonalness = 0.0f;       // 0..1: how much to trust the pitch
};

SfxAnalysis analyseSfx(const float* mono, int numSamples, double sampleRate, SfxMethod method = SfxMethod::Auto);

//==============================================================================
// Match

enum class Target
{
    Root, Fifth, Third,
    Nearest,      // the smallest move onto a strong note of the music
    PitchClass,   // a chosen note
    Follow        // Nearest, kept up with the music: when the note it rests on stops sounding, the
                  // effect moves to the nearest note that is (Nearest where the music does not change)
};

/** A hand-set adjustment to the shift at one moment of the effect. */
struct CurvePoint
{
    double seconds = 0.0;         // from the start of the effect
    double semitones = 0.0;       // added to the shift there
};

struct MatchOptions
{
    Target target = Target::Root;
    int pitchClass = 0;           // for Target::PitchClass
    int octaveShift = 0;          // whole octaves on top of the smallest move
    double bend = 0.0;            // -1 = flatten the effect's own drift, 0 = leave it, +1 = take on the music's movement in full
    // Where the effect starts relative to the start of the learned music, if known (for bend > 0).
    bool offsetKnown = false;
    double sfxOffsetInMusicSeconds = 0.0;
    // Following a lead line: 0 steps from note to note; up to 1, each step opens into a ramp
    // between the notes (at 1, a straight line from the middle of one note to the middle of the
    // next).
    double glide = 0.0;
    // Following: the note the effect rests on has to be gone from the music for this long before
    // the effect moves on, so it answers the notes the music rests on, not every passing one.
    double shortestNoteSeconds = 0.35;
    // Hand adjustments, added on top of everything else. Between points the adjustment moves in a
    // straight line; it returns to nothing at the effect's two ends unless a point sits there.
    std::vector<CurvePoint> edits;
};

struct MatchResult
{
    bool valid = false;
    int targetPitchClass = 0;
    double targetMidi = 0.0;
    double semitones = 0.0;       // the fixed shift
    double speedRatio = 1.0;      // the same as a playback speed
    double hopSeconds = 0.005;
    std::vector<float> curve;     // the whole shift per effect frame, semitones (fixed shift plus bend)
    std::vector<float> afterHz;   // the effect's pitch after the shift, per frame; 0 where no pitch
    std::vector<float> afterMidi; // where the shift puts the effect's centre, per frame (always set)

    // Target::Follow: how many notes the effect rests on in turn (0 when it is not following, or
    // the music gave it no reason to move). semitones and targetMidi describe the first of them.
    int notesFollowed = 0;

    // The effect's frames [coveredFrom, coveredTo) lie under the learned music. Outside them the
    // music was not heard: a bend that follows the music carries on from the music's last
    // movement there, easing to a stop (a prediction, for the user to check and adjust).
    int coveredFrom = 0;
    int coveredTo = 0;
};

MatchResult match(const MusicAnalysis& music, const SfxAnalysis& sfx, const MatchOptions& options);

/** What the hand adjustments add at one moment of an effect that is durationSeconds long. */
double editOffsetAt(const std::vector<CurvePoint>& edits, double seconds, double durationSeconds);

/**
 * The nearest pitch (MIDI, tuning included) that is a note of the music: any semitone of its
 * tuning, or with scaleOnly a note of its major or natural minor scale.
 */
double snapToMusic(const MusicAnalysis& music, double midi, bool scaleOnly) noexcept;

namespace detail
{
/** Mends breaks in a pitch line (hz per frame, 0 = no pitch) that a harmonic interval explains. Exposed for tests. */
void repairHarmonicSlips(std::vector<float>& hz, const std::vector<float>& level);
}

} // namespace tonalrecall::core

// De-Honk DSP core. Plain C++20, no JUCE, no allocation after prepare().
// Shared by the plug-in (dehonk/Source) and the offline harness (dehonk/Tests).
// See dehonk/SPEC.md section 3.
#pragma once

#include <cstdint>
#include <memory>

namespace dehonk
{

inline constexpr int kMaxChannels = 12;          // up to 7.1.4
inline constexpr int kMaxBands = 80;            // analysis bands (73 are used)
inline constexpr int kBandsPerOctave = 12;
// The bands sit on equal-tempered semitones (A = 440 Hz): band 0 is E1, band 72 is E7. A notch is
// then always on a note or, between two bands, between two notes, which the key measurement uses.
inline constexpr float kAnalysisLowHz = 41.2034f;  // centre of band 0
inline constexpr float kAnalysisHighHz = 2637.02f;
inline constexpr float kMaxLookaheadMs = 20.0f;

/** The problem areas. Each is a module that can be switched on or off. Neighbouring modules
    share an edge, so there are kNumModules + 1 edge frequencies. */
enum Module { Woof = 0, Tub, Fog, Bark, kNumModules };
inline constexpr int kNumEdges = kNumModules + 1;
inline constexpr float kDefaultEdgesHz[kNumEdges] = { 60.0f, 110.0f, 200.0f, 400.0f, 1500.0f };
inline constexpr float kEdgeMinHz[kNumEdges]      = { 45.0f,  90.0f, 160.0f, 300.0f,  800.0f };
inline constexpr float kEdgeMaxHz[kNumEdges]      = { 80.0f, 150.0f, 280.0f, 600.0f, 2200.0f };

/** Where a channel sits, for layouts of more than two channels. Used for two things only: an LFE
    channel is passed through untouched (delayed like the rest, never analysed or filtered), and
    the others are folded down to stereo to measure the programme level the Threshold compares
    (see ChannelLayout). */
enum class Position : std::uint8_t
{
    unknown = 0,  // folded to both sides at 0.5 (as middleOther)
    leftFront,    // folded to the left at 1
    rightFront,   // folded to the right at 1
    centre,       // folded to both sides at 0.7071 (-3 dB)
    leftOther,    // any other speaker on the left (side, rear, wide, height): to the left at 0.7071
    rightOther,   // the same on the right
    middleOther,  // on the centre line but not the front centre (rear centre, top centre): to both at 0.5
    lfe           // never analysed, never filtered, left out of the fold-down
};

/** The layout of the channels passed to Core::prepare(). Without one, every position is
    unknown: no channel is an LFE. With more than two channels an unknown position on channel 0
    is taken as left front and on channel 1 as right front. With one or two channels the positions
    are not used, except that an LFE is still passed through. If every channel is marked LFE, none
    is taken as one. */
struct ChannelLayout
{
    Position position[kMaxChannels] {};

    /** Convenience: no positions known, and channel lfeIndex (if 0 or more) is the LFE. */
    static ChannelLayout withLfe (int lfeIndex) noexcept
    {
        ChannelLayout l;
        if (lfeIndex >= 0 && lfeIndex < kMaxChannels)
            l.position[lfeIndex] = Position::lfe;
        return l;
    }
};

/** What the user sets. Plain values; the core smooths what needs smoothing. */
struct Params
{
    float depth = 0.5f;          // 0..1   how much of the detected excess is removed
    float sharpness = 0.2f;      // 0..1   0 = broad (whole bump), 1 = narrow (single resonances)
    float speed = 0.5f;          // 0..1   0 = slow, 1 = fast: how quickly a notch goes in (the attack)
    float releaseMs = 400.0f;    // 50..1500 ms: how slowly a notch lets go once its peak has gone
    float maxCutDb = 6.0f;       // 3..12 dB: no notch goes deeper than this, whatever Depth is
    float impactGuard = 1.0f;    // 0..1   how far the cut is kept out of a broadband impact (a hit, a slam,
                                 //          an explosion) while it sounds: no new depth, and notches already
                                 //          open eased by this much. 0 = impacts are treated like anything
                                 //          else. See DisplayFrame::guard.
    bool moduleOn[kNumModules] = { false, true, true, false };   // Woof, Tub, Fog, Bark
    float edgeHz[kNumEdges] = { 60.0f, 110.0f, 200.0f, 400.0f, 1500.0f };   // module m spans edgeHz[m]..edgeHz[m+1];
                                 //          each within kEdgeMinHz..kEdgeMaxHz
    bool snapOn = false;        // the Click module: takes the edge off sharp pops around 2.5 kHz
    bool keyOn = false;          // the threshold listens to the sidechain's level in place of the track's own.
                                 //          Has an effect only in blocks processed with a key (processWithKey).
    float thresholdDb = -16.0f;  // -60..0 dBFS. De-Honk acts only while the programme is above this
                                 //          (RMS below 2.8 kHz, louder channel; with more than two
                                 //          channels, the louder side of a stereo fold-down). -60 = always.
    float link = 1.0f;           // 0..1   1 = shared detection (every channel but an LFE gets the
                                 //          same notches), 0 = per channel
    float lookaheadMs = 0.0f;    // 0, 5, 10 or 20. Sets latencySamples().
    float mix = 1.0f;            // 0..1
    float trimDb = 0.0f;         // -12..+12
    bool soloHonk = false;       // output = what is being removed (an LFE is silent). Diff and Bypass win over it.
    bool diff = false;           // output = untreated signal, latency-matched, analysis keeps running,
                                 //          turned down to the loudness of the treated signal (see
                                 //          DisplayFrame::diffOffsetDb) so that louder does not win.
                                 //          An LFE is not treated, so it plays as it is, not turned down.
    bool bypass = false;         // output = untreated signal, latency-matched
};

/** Internal constants still to be settled against the corpus (SPEC "to measure").
    The harness can override them; the plug-in uses the defaults. */
struct Tuning
{
    float depthSlope = 3.2f;           // cut = depth * depthSlope * excess over the dead zone
    float deadZoneDb = 3.0f;           // excess below this is ignored
    float kneeDb = 3.0f;               // soft knee width around the dead zone
    float referenceExponent = 1.0f;    // 1 = average power, smaller = more compressive
    float broadWidthOctaves = 2.0f;    // widest feature treated as one thing, at sharpness 0
    float narrowWidthOctaves = 0.33f;  // ... at sharpness 1
    float surroundRatio = 2.5f;        // a band is compared with a ring this many times wider
    float thresholdKneeDb = 6.0f;      // full effect this far above the threshold
    float levelAttackMs = 30.0f, levelReleaseMs = 400.0f;   // programme level follower
    float parentRule = 1.0f;           // 0 = off, 1 = a band is only cut if something at least
                                       //     comparable sits at a subharmonic below it
    float parentStartDb = -6.0f;       // parent this far below the band: not eligible
    float parentFullDb = 6.0f;         // parent this far above the band: fully eligible
    float attackSlowMs = 40.0f, attackFastMs = 5.0f;   // Speed 0 and Speed 1
    float bellQ = 9.0f;                // Q of each notch (about 0.16 octave wide at half depth)
    float notchesPerModule = 2.0f;     // how many notches a module may have open at once
    float notchSpacingOctaves = 0.25f; // closest two notches may sit
    float notchHoldDb = 1.5f;          // a notch that is already open is preferred by this much, so it stays put
    // Click module
    float clickLowHz = 1800.0f, clickHighHz = 4000.0f;   // the band a click lives in
    float clickMarginDb = 14.0f;       // how far a spike must stand above the band's own running level
    float clickNeighbourDb = 3.0f;     // ... and above the spike in the bands either side, plus this
    float clickSlope = 2.0f;           // cut = depth * clickSlope * what is left over
    float clickMaxCutDb = 12.0f;
    float clickBodyMs = 40.0f;         // the running level is taken over this long
    float clickReleaseMs = 6.0f;
    float diffMatchSeconds = 3.0f;     // loudness of treated and untreated is compared over this long
    float keySeconds = 8.0f;           // the note profile is averaged over this long
    float peakTimeMs = 250.0f;         // notches are placed on peaks that last about this long
    float peakMinDb = 2.0f;            // a peak must stand this far above its neighbours to get a notch
    // Impact guard (Params::impactGuard). An impact is a rise of guardRiseDb or more over the
    // band's own short average, in at least guardShare of the bands from guardLowHz up, all at once.
    float guardRiseDb = 6.0f;          // how far a band must stand over where it was to count as risen
    float guardShare = 0.45f;          // the share of the bands that must have risen together
    float guardLowHz = 150.0f;         // bands below this are too slow to say anything in time
    float guardLagMs = 48.0f;          // "where it was": the band's level from this long ago ...
    float guardAverageMs = 100.0f;     // ... averaged over this long, so about 50 to 250 ms back
    float guardFloorDb = -25.0f;       // a band this far under the whole range's level is taken as empty
    float guardCeilingDb = 12.0f;      // no band is taken as louder than this (dBFS), so that one absurd
                                       //     sample cannot blind the guard for seconds
    float guardRefractoryMs = 150.0f;  // after something lands nothing new can land for this long
    float guardLevelMs = 25.0f;        // the level that is watched afterwards is averaged over this long
    float guardConfirmMs = 280.0f;     // an impact dies away: within this long of landing its level must
    float guardDecayDb = 6.0f;         //     have fallen this far from its highest, or it was an entry (a
                                       //     cut-in, a held chord) and the guard lets go
    float guardBackDb = 2.0f;          // an impact is over once the level is back within this of what was
                                       //     there before it landed ...
    float guardHoldMs = 600.0f;        // ... and in any case this long after it landed
    float guardReturnMs = 120.0f;      // the guard then lets go over this long, in a straight line
    float guardLiftMs = 2.0f;          // how fast it comes up when something lands
    float guardEase = 1.0f;            // a notch already open is eased off by up to this share while the
                                       //     guard is fully up (new depth is always held back in full) ...
    float guardOverStartDb = 2.0f;     // ... as far as the impact stands over what was there before it, in the
    float guardOverFullDb = 7.0f;      //     notch's band or over the whole range: not at all up to here, fully from here
    float guardEaseFallMs = 100.0f;    // a notch comes back over this long as the impact dies away
    float guardDutyLimit = 0.35f;      // the guard is never up for more than about this share of any
    float guardDutySeconds = 4.0f;     //     stretch this long: past 2/3 of it each new impact holds for less
    // How a notch moves and lets go (with handOver).
    float moveReleaseMs = 130.0f;      // a notch whose peak has moved to a band next door lets go at this,
                                       //     or at Release if that is quicker
    float unsettledReleaseMs = 45.0f;  // ... and one that never settled at this
    float movedWithinDb = 3.0f;        // "moved next door": the neighbour now chosen is within this of how
                                       //     strong the band was when it held the notch
    float thresholdCloseMs = 40.0f;    // under the Threshold the cut closes over this long, whatever Release is
    float earnMs = 250.0f;             // a notch has settled once it has been on a chosen peak this long
    float earnProudDb = 4.9f;          //     and as deep as a peak this far above its surroundings is cut
                                       //     (3 dB at the default Depth; never more than 3/4 of Max Cut)
    float depthFloor = 0.0f;           // a chosen notch is at least this share of its slow average deep
                                       //     (0.7 without handOver, whatever this is)
    bool handOver = true;              // the way a notch moves now: one that takes over from a neighbour
                                       //     grows only as the neighbour lets go, a notch that is letting
                                       //     go counts towards its module's number, Release applies in full
                                       //     only to a notch that has settled, and the Threshold closes
                                       //     the cut by itself and not through the release. false is the
                                       //     movement before Release was a control; no control reaches it,
                                       //     it is kept for the proof that the rest has not changed
    bool detrend = false;              // remove the overall tilt above the bass before comparing
};

/** One frame for the display. Written on the audio thread at the control rate;
    the plug-in copies it into its own lock-free FIFO. */
struct DisplayFrame
{
    int numBands = 0;
    float bandHz[kMaxBands] {};
    float levelDb[kMaxBands] {};   // input level per band, dBFS, loudest channel (an LFE never counts)
    float cutDb[kMaxBands] {};     // cut being applied per band, dB, positive = reduction, deepest channel
    float totalCutDb = 0.0f;       // largest cut across bands
    float moduleCutDb[kNumModules] {};   // deepest notch in each module, dB, positive = reduction
    float snapCutDb = 0.0f;       // deepest cut the Click module made since the last frame, dB
    float diffOffsetDb = 0.0f;    // the gain Diff applies to the untreated signal to match loudness, dB (0 or below)
    float keyDb = -120.0f;        // the sidechain's level, dBFS RMS (-120 when there is none)
    bool keyed = false;           // true while the threshold is listening to the sidechain
    float wash = 0.0f;            // 0..1: how much of the energy in the range is noise-like and not peaks.
                                  //       High means the build-up is wash, which is fixed upstream by taking
                                  //       layers out, not here. Meaningful only while activity is above 0.
    float pitchClass[12] {};      // band power summed across octaves, by note (0 = C), averaged over ~8 s
    float programDb = -120.0f;     // programme level the threshold is compared with, dBFS RMS
    float activity = 0.0f;         // 0..1: how far above the threshold the programme is
    float guard = 0.0f;            // 0..1: how far the impact guard is up now (0 = not at all; cutDb and
                                   //       the notches already have what it takes off taken off)
};

class Core
{
public:
    Core();
    ~Core();
    Core (const Core&) = delete;
    Core& operator= (const Core&) = delete;

    /** Allocates. Call before process() and whenever the sample rate, channel count or layout
        changes. numChannels is 1 to kMaxChannels. Not real-time safe. */
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void prepare (double sampleRate, int maxBlockSize, int numChannels, const ChannelLayout& layout);

    /** Clears filter and detector state. Real-time safe. */
    void reset();

    /** Real-time safe. Call on the audio thread before process(). */
    void setParams (const Params&) noexcept;
    void setTuning (const Tuning&) noexcept;
    const Tuning& tuning() const noexcept;

    /** In place. numSamples may be any size, numChannels as prepared (if fewer are passed only
        those are processed and analysed; extra ones are left untouched).
        Real-time safe: no allocation, locks or system calls. */
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    /** The same, with a sidechain. `key` is read only (numKeyChannels of them, any number from 1;
        nullptr or 0 for none). With Params::keyOn the threshold then compares the key's level, taken
        over the whole spectrum on its loudest channel, in place of the track's own. Nothing else
        about the processing changes. */
    void processWithKey (float* const* channels, int numChannels, const float* const* key, int numKeyChannels, int numSamples) noexcept;

    /** Delay through the core for the current lookahead setting, in samples.
        Changes only when Params::lookaheadMs changes. */
    int latencySamples() const noexcept;
    /** The latency a given lookahead would give at the prepared sample rate. Reads only the
        sample rate, so it may be called from any thread once prepare() has returned. */
    int latencySamplesFor (float lookaheadMs) const noexcept;

    int numBands() const noexcept;
    float bandHz (int band) const noexcept;

    /** Latest detector frame. Audio thread only. frameCounter() increases by one each time
        the frame is rewritten, so the caller can tell when there is a new one. */
    const DisplayFrame& displayFrame() const noexcept;
    std::uint32_t frameCounter() const noexcept;

    /** Test hook: replace the detector's output with fixed cuts (dB, positive = reduction,
        numBands() values). Each is applied as one notch at that band, so neighbouring bands'
        notches add; a depth above Params::maxCutDb is held to it; and only bands from 56 Hz to
        2.3 kHz have a notch (the rest are ignored). Pass nullptr to return to normal detection.
        Not real-time safe to toggle while processing on another thread. */
    void debugSetStaticCuts (const float* cutDbPerBand) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace dehonk

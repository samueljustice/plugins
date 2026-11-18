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

/** Which channels share their detection when Params::linkMode is LinkMode::grouped. `automatic`
    takes it from the position: the two fronts (and a channel of unknown position) are the front
    pair, the centre is on its own, and everything else that is not an LFE is a surround. A host
    that knows which speakers are overhead says so with `height`. */
enum class Group : std::uint8_t { automatic = 0, front, centre, surround, height };

/** How the channels share what the detector finds. */
enum class LinkMode : std::uint8_t
{
    linked = 0,   // every channel but an LFE gets the same notches (one detection, on the louder channel per band)
    grouped,      // the front pair, the centre, the surrounds and the heights each share within their group.
                  //   With one or two channels this is the same as linked.
    unlinked      // each channel on its own
};

/** How long a narrow peak must stand before it is believed. */
enum class Detection : std::uint8_t
{
    fast = 0,     // the band levels as they are (smoothed over a few ms): a peak is cut as soon as it shows
    steady        // each band's steady level (it rises over about a second and falls over about half of
                  //   one) is what is compared, and a lift that is wide along the spectrum is not taken
                  //   for a peak. Steadier and kinder to dense, noisy textures, slower to catch a new
                  //   build-up. See Tuning::steady*.
};

/** The layout of the channels passed to Core::prepare(). Without one, every position is
    unknown: no channel is an LFE. With more than two channels an unknown position on channel 0
    is taken as left front and on channel 1 as right front. With one or two channels the positions
    are not used, except that an LFE is still passed through. If every channel is marked LFE, none
    is taken as one. */
struct ChannelLayout
{
    Position position[kMaxChannels] {};
    Group group[kMaxChannels] {};     // for LinkMode::grouped only

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
    bool dialogueOn = false;     // the sidechain is the dialogue stem: a band the dialogue accounts for is not
                                 //          treated as build-up, and a cut that is open there eases off while
                                 //          the dialogue is in it; Snap leaves a click that is the dialogue's (a
                                 //          consonant) alone. Build-up well above the dialogue is cut as
                                 //          ever, and nothing else about the detection changes. Has an effect
                                 //          only in blocks processed with a key. See Tuning::dialogue*.
    float dialogueAmount = 1.0f; // 0..1   how far a band that is all dialogue is left alone (1 = entirely)
    float thresholdDb = -16.0f;  // -60..0 dBFS. De-Honk acts only while the programme is above this
                                 //          (RMS below 2.8 kHz, louder channel; with more than two
                                 //          channels, the louder side of a stereo fold-down). -60 = always.
    LinkMode linkMode = LinkMode::linked;   // which channels share their detection
    Detection detection = Detection::fast;  // how long a peak must stand before it is cut
    float link = 1.0f;           // 0..1   how far a channel follows what it shares (all the channels when
                                 //          linked, its group when grouped): 1 = entirely, 0 = not at all,
                                 //          which is the same as unlinked. No control reaches anything but 1;
                                 //          it is kept for the proof that the rest has not changed.
    // Each range's own timing. Locked (the default), a range follows Speed and Release like the
    // rest. Unlocked, its notches go in over its own attack and let go over its own release. Snap
    // has its own fixed timing either way.
    bool rangeTimingLocked[kNumModules] = { true, true, true, true };
    float rangeAttackMs[kNumModules] = { 14.0f, 14.0f, 14.0f, 14.0f };          // 2..100 ms
    float rangeReleaseMs[kNumModules] = { 400.0f, 400.0f, 400.0f, 400.0f };     // 50..1500 ms
    float lookaheadMs = 0.0f;    // 0, 5, 10 or 20. Sets latencySamples().
    float mix = 1.0f;            // 0..1
    float trimDb = 0.0f;         // -12..+12
    bool soloHonk = false;       // output = what is being removed (an LFE is silent). Diff and Bypass win over it.
    bool diff = false;           // output = untreated signal, latency-matched, analysis keeps running,
                                 //          turned down to the loudness of the treated signal (see
                                 //          DisplayFrame::diffOffsetDb) so that louder does not win. Switching
                                 //          it cross-fades over Tuning::diffFadeMs.
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
    float diffMatchSeconds = 1.5f;     // loudness of treated and untreated is compared over about this long: the
                                       //     power of each is averaged with this time constant, all the time, and
                                       //     Diff is turned down by the ratio of the two
    float diffFadeMs = 80.0f;          // switching Diff cross-fades over this long, along a curve with no corner in it
    float diffGainMs = 50.0f;          // the level match moves the gain it applies over this long
    // Dialogue sidechain (Params::dialogueOn). Per band, the dialogue's power is compared with the
    // power the detector is working from.
    float dialogueStartDb = -9.0f;     // dialogue this far under the band: the band is treated as ever
    float dialogueFullDb = -3.0f;      // ... this far under it (half the power) or more: the band is the dialogue's
    float dialogueSpread = 0.5f;       // a notch is about two bands wide: a band next to one that is the
                                       //     dialogue's is left alone by this share as well
    float dialogueAttackMs = 15.0f;    // how fast a band is handed to the dialogue ...
    float dialogueReleaseMs = 250.0f;  // ... and how slowly it is taken back, so the gaps between words do not pump
    // The level the dialogue has in each channel of the mix is measured, not assumed: the key is
    // looked for in the channel band by band (how far the two move together, whatever else is in
    // the band), and the gain that fits is what the key is taken at, up or down.
    float dialogueGainMs = 500.0f;     // ... averaged over this long
    float dialogueTrimMaxDb = 18.0f;   // a key that arrives quieter than the dialogue is in the mix is turned up by at most this
    float dialogueTrimMinDb = -40.0f;  // ... and one that arrives louder (a stereo stem summed to one channel, a hot
                                       //     send) down by at most this
    float dialogueMatchRatio = 4.0f;   // the key counts as found in a channel when what the two share, over what two
                                       //     unrelated signals would share by chance, is this much more than that
    float dialogueOverShare = 0.8f;    // the key is "most of the mix" when this share of the bands it is in are
    float dialogueOverSeconds = 1.5f;  //     the dialogue's nearly entirely, for this long (DisplayFrame::keyOver)
    float snapDialogueAttackMs = 1.0f; // Snap and the dialogue: the key's level in Snap's band against the mix's
    float snapDialogueReleaseMs = 50.0f; //   there, followed this fast up and down
    float keySeconds = 8.0f;           // the note profile is averaged over this long
    // Steady detection (Params::detection). Each band's power is smoothed over steadySmoothMs, and
    // its level in dB follows that with steadyRiseMs going up and steadyFallMs going down: the
    // steady level. The band's excess over its surroundings is taken on the steady levels, and
    // it must also stand over a floor drawn under the steady spectrum (each band no higher than
    // the mean of the two bands steadyLumpBands away, for 1 up to steadyLumpBands, in turn) by
    // more than the dead zone less steadyLumpMarginDb: a lift as wide as the floor's reach is
    // part of the floor, not a peak.
    float steadySmoothMs = 50.0f;
    float steadyRiseMs = 1000.0f;
    float steadyFallMs = 400.0f;
    float steadyLumpBands = 3.0f;      // a quarter of an octave; 0 = no floor test
    float steadyLumpMarginDb = 1.0f;
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
    bool keyed = false;           // true while the sidechain is in use: the dialogue is being listened for, or
                                  //       the threshold is listening to it
    float dialogue[kMaxBands] {}; // 0..1 per band: how far the band is being left to the dialogue (the share of
                                  //       the cut taken off; cutDb already has it taken off). 0 without a sidechain.
    float dialogueTrimDb = 0.0f;  // the gain the sidechain is being taken at to fit the mix, dB: below 0 for a key
                                  //       that arrives louder than the dialogue is in the mix, above for a quieter one
    float keyMatch = 0.0f;        // 0..1: how surely the key has been found in the mix. Near 0 with a key that is
                                  //       playing: it is not in the mix, or not in time with it, and is being
                                  //       taken at the level it arrives at. 0 without a sidechain.
    bool keyOver = false;         // the key accounts for nearly everything in most of the bands it is in, and has
                                  //       for a while (the mix itself on the key, or the wrong stem), or its gain is
                                  //       at the end of its range: De-Honk is being switched off by it
    float snapDialogue = 0.0f;    // 0..1: how far Snap is being held off by the dialogue (the most since the last frame)
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
        nullptr or 0 for none), and in time with the channels as they are passed in. With
        Params::dialogueOn it is the dialogue stem (its first two channels are listened to; a band
        counts as the dialogue's if it is in either). With Params::keyOn the threshold compares the
        key's level, taken over the whole spectrum on its loudest channel, in place of the track's
        own. With neither, the key does nothing. */
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

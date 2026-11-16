#pragma once

#include <JuceHeader.h>
#include "Cargo.h"
#include "ScopeRing.h"
#include "PitchShifter.h"
#include "DspUtils.h"
#include "SubFilter.h"
#include "SpectralMorph.h"
#include "SpaceReverb.h"
#include "LfeFilter.h"
#include "Room.h"

#include <array>
#include <atomic>

/**
 * The whoosh rig (layered looping tracks with ridden faders),
 * as one engine that runs continuously while the recorder rolls.
 *
 *   tracks (always playing, looped, silence skipped)
 *     -> ride: each track's fader is LFO'd at its own offset speed plus a random
 *        "hand on the fader" (Mondo-Mod style), scaled by Energy / Rate
 *     -> fill: long delays (about 0.5 / 1 / 2 s, a little different on every track) fill the gaps of unsteady tracks
 *     -> bus A (aggressive) | bus B (smooth) | hits      (routed automatically by detected type)
 *
 *   each bus:     doppler (two passes, always half a pass apart, with randomly automated speed, distance and
 *                 direction; Pass Mode Bed overlaps them into a steady level, Fly-By plays them one after the
 *                 other) -> low end (sub octave, overtones, lift at the loudest point) -> swirl (phaser + flanger)
 *                 -> envelope filter -> drive. Low end, swirl and filter follow the bus's level and, the stiller
 *                 the faders are, how near the pass is.
 *   stereo beds:  (as loud wide as narrow: the bus is levelled by its mono sum's power, see renderBus.)
 *                 A stereo sound on bus B keeps its width while B's Pass Mode is Bed: its side signal
 *                 (CargoData::hasSide) rides with the track and flies the same two passes (the same delays at
 *                 each ear), plus on the left and minus on the right, by Width x (1 - Doppler) and by how near
 *                 the middle the pass is (a pass far to one side is a point). It skips the colour chain and is
 *                 added back after it, level-matched to what the chain did. Everything else plays the mono sum.
 *   side-chains:  A ducks B, and A's level low-passes B
 *   stowaway:     one extra, lighter bus beside A and B (not a third rig bus: it has no bus colour chain, morph
 *                 side, Take Turns or LINK of its own, and only Fly has passes). Tracks sent to it, and their throw
 *                 feed, arrive in StowState; it renders after bus B (bus A's level and passes are final by then)
 *                 and before the hits, as one mono stream with a position (in Fly: one stream and its two passes'
 *                 positions), and joins the mix. A throw's share is shaped by the throw's own pass. The hits duck
 *                 it in every mode but Sub.
 *                 Sub: planted in the middle, mono. 20 Hz high-pass -> crossover (48 dB per octave low-pass at
 *                 Filter, 40 .. 250 Hz) -> Low End (sub octave, overtones) -> Drive -> safety low-pass just
 *                 above the crossover -> Follow (it swells with bus A: A's level against its recent peak and how
 *                 near A's pass is; with no track on A it goes by bus B, and with none there either by its own
 *                 tracks' ride). See SubFilter and renderSub.
 *                 Wake: opens as bus A falls away (how far A's level has dropped from what it was and, as far as the passes
 *                 are what moves A, how far out its outgoing pass is), so each pass leaves a trail; placed behind
 *                 that pass by Width. Its tracks are not ridden. Low End, Filter (a tilt) and Drive colour it. See
 *                 renderWake.
 *                 Dry: the tracks at their faders and rides (Follow: how much of the ride), no Fill echoes, through
 *                 Filter only, and added after Space and the Everything tremolo. A stereo sound keeps its sides
 *                 (by Width). See renderDry.
 *                 Fly: a third mover. The tracks (ridden, with their Fill echoes) are coloured by Low End, Filter
 *                 (the tilt) and Drive, and that one stream is flown by Stowaway's own two passes, always shaped
 *                 like Fly-Bys, through the same doppler core the buses use: its own delay line, its own random
 *                 draws (so nothing it does moves bus A's or bus B's passes), a fixed doppler, Width for how far
 *                 across it travels. Follow ties its passes to bus A's: 0% they keep their own time, 100% each is
 *                 at its closest as one of A's is. It sleeps while nothing sounds in it. See renderFly.
 *                 A change of Mode fades it out (30 ms), swaps, and fades it back in; a track that changes place
 *                 (by hand, or a rumble as Stowaway becomes a sub or stops being one) is faded across (10 ms).
 *   hit sync:     optional: every hit track fires its next sound on a shared beat, transients aligned
 *                 (the beat is as long as the longest hit, so none is cut short).
 *   hits:         a gate on A + B opens only at the loudest moments (a boost with hold
 *                 and release, not on/off); the gated hits then duck A and B.
 *                 The detector listens before the ducking, so there is no feedback loop.
 *   throw:        on demand: one more pass, the same on both buses, shaped like a Fly-By (approach, peak, tail).
 *                 It plays the tracks at their fader levels (not ridden) through its own delay line, so it is
 *                 the same whoosh whether the rig is playing or stopped, and AIM lands the hits on its peak.
 *   morph:        optional: what feeds bus A and bus B goes through a Zynaptiq-style spectral morph
 *                 (SpectralMorph); the result is handed back to the two buses in proportion to X. A side with
 *                 a rope (CargoHold::getMorphSource) morphs that one track instead of its whole bus: the rest
 *                 of the bus plays on as it is, and with the morph off the roped track plays through its bus.
 *   master:       optional: one track leads (CargoHold::getMaster). Its loudness, pitch and brightness
 *                 contours are read where it is playing, for the Followers, and with Follow Master the other
 *                 tracks' rides swell with its loudness instead of their own LFOs.
 *   -> ride make-up (the mix is eased down as the ride gets shallower, so Auto Ride off or a held Swell reaches
 *      the output stage about as loud as the ridden rig) -> space (echoes, reverb, width)
 *   -> out (the processor adds inflate + limiter)
 *
 * THE ROOM. At Mono and Stereo the above is all there is: each pass is heard by two ears (a delay between them,
 * the far one duller). At Quad and wider (setWidth) nothing makes a stereo image. Every pass, throw and hit is one
 * mono stream with a place in the room (room::Position), and room::pan() puts it on the speakers:
 *   a bus      keeps its passes apart as LANES all the way to the mix: lane 1 is its first pass, lane 2 its second,
 *              lane 3 its throws, lane 4 the sides of a stereo bed. Each pass is read from the line once (no ear
 *              delays, no head shadow: real speakers and a real head do that), with the same doppler, swell and air.
 *              The colour chain works on the lanes as it works on left and right in stereo (so Filter and Drive act
 *              on a whole pass, and it costs what it costs in stereo); ducking, the tremolo and the fades are gains
 *              on the lanes. Only the mix pans, with gains that are ramped across each chunk.
 *   Flight Path where a bus's passes fly (room::flight): Across, Through, Overhead, Around. A pass flies its path
 *              over the part of it that is heard (pathProgress), and Width is how much of the path it travels (0%:
 *              it stays at the path's half-way point). A change of path glides. Each pass has a way (Direction),
 *              a side (Side) and a height of its own (Height Varies): drawn pass by pass, held by the bus's
 *              choice, or decided by a modulator as the pass starts. Linked, bus B flies bus A's.
 *              Around has no approach: the pass keeps one distance, so it has no doppler bend and no swell of its
 *              own; it holds the level and the brightness a crossing pass has on average (orbitLoudness).
 *   hits       see WHERE A HIT LANDS below.
 *   Stowaway   Sub: at the front, equal in L and R, and its stem is kept for an LFE (getSubStem). Wake: on bus A's
 *              path, behind its outgoing pass. Fly: its own passes on its own Flight Path. Dry: straight to L and R.
 *   stereo beds  the side signal stays a left / right pair: at the front, and with Spread on every left and right
 *              speaker of the floor.
 *   Space      the echoes are sent a stereo fold of the room and come back in L and R. The reverb is the room's
 *              own (RoomReverb): the Space knob's reverb with a return on every main speaker, each uncorrelated
 *              with the others, fed by the room's mix. A pass's reverb leans toward where the pass is (half the
 *              reverb is shared by where the room's sound has been over the last third of a second, half evenly)
 *              and fills the room. The tops get their share as far as Height lets them, the centre speaker only
 *              with Centre Speaker on, the LFE never. All speakers together carry as much reverb as L and R do in
 *              stereo at the same Space.
 *   LFE        (renderLfe; widths with an LFE) LFE Mode: Off (silent); Discrete: Stowaway's Sub, which stays in L
 *              and R as well; Mixdown: half the sum of the main speakers (a sound in the middle of the front pair
 *              reaches it at the level each of the pair carries). Either way through the LFE's own fixed low-pass
 *              (lfe::LowPass, 120 Hz, 48 dB an octave) and LFE Level. While the LFE carries a copy of what the
 *              main speakers carry (Mixdown; Discrete with Stowaway on in Sub) the main speakers go through the
 *              matching all-pass (lfe::PhaseMatch), so the LFE and the mains' copy of the same low end are in phase
 *              at every frequency: bass management adds them, never cancels them. Otherwise (LFE Mode Off, Discrete
 *              without the Sub, a width without an LFE) the mains go straight through, with no delay in their low
 *              end; the all-pass comes and goes over half a second by moving its frequency, with no click and no
 *              change of level on the way. With LFE Level at its reference (-10 dB, which a calibrated room plays
 *              as loud as a main speaker) Discrete adds 3.5 dB under 80 Hz to what the mains' two copies give
 *              together: the Sub is weighted, not doubled. Nothing is ever taken from the mains.
 *
 * WHERE A HIT LANDS (Hit Position, Hit Spread; at every width from Stereo up). The hits are one sound with one place:
 *   With the Pass  where the pass it belongs to is, moment by moment, for as long as that pass flies (left / right,
 *              front / back and height in a room; left / right at Stereo). Which pass: with AIM, the pass or throw
 *              the hit was aimed at. Without AIM, the loudest pass in flight as the hit starts (the hit gate opening,
 *              or SYNC HITS' beat), a thrown whoosh before the rig's own; and when that pass has gone while the gate
 *              is still open, the loudest then. When its pass has gone the hit stays where the pass left it. With
 *              nothing in flight (the rig stopped and no throw, or no sound on either bus) it is at the front.
 *              Linked, bus B's passes are bus A's, so a hit that goes with one of B's is where A's is.
 *   Front      the front at ear level: a phantom centre, or the centre speaker with Centre Speaker on.
 *   Random     each hit at a place of its own, drawn as it starts from a source of its own (seeded with the engine,
 *              so the same seed gives the same places): anywhere round the walls, and up towards the speakers
 *              overhead as far as Height lets it; left to right at Stereo. It stays there until the next hit.
 * A hit is two objects, the left and the right of its sound (mid + side and mid - side; a mono sound's are the
 * same), a little apart either side of its place: c_hitApart of the room's half width at Hit Spread 0%, the whole
 * of it at 100%. Hit Spread also shares each object's power out, at constant power: the left one's over every
 * speaker on the left, the right one's over every speaker on the right (a centre speaker half each as far as
 * Centre Speaker lets it, the speakers overhead as far as Height lets them, never the LFE). So 0% is a point at
 * the hit's place and 100% is every main speaker alike, with a stereo sound's left on the left and its right on
 * the right. The two objects' sum is not a plain one: its level goes by how far the two share speakers and by
 * how alike the sound's two sides are (its mid and side powers, measured over the last tenth of a second, a hit
 * starting from what its files are as a whole: see renderHits and
 * mixHits), so that whatever Hit Spread is, the hits have the power they have as a point: that of their mono sum,
 * which is what every track is levelled by (CargoData::autoGain). A sound that is the same on both sides is not
 * louder as a point than spread out, a wide one is not louder spread out than as a point, and nothing here can
 * lift a sound: the level is never over 1. At Mono they are the mid alone, in the middle.
 *
 * Every continuous control is chased by a damped spring (rubber banding).
 * No allocation, locks or I/O on the audio thread.
 */
class RigEngine
{
public:
    static constexpr int c_tracks = CargoHold::c_numTracks;
    static constexpr int c_maxBuses = CargoHold::c_maxBuses;
    static constexpr int c_throws = 4;                  // thrown whooshes that can be in flight at once
    static constexpr int c_maxFlights = c_maxBuses * 2 + c_throws; // two doppler passes per bus, then the throws (the visualiser's comets)
    static constexpr int c_maxKeys = 16;                // held MIDI keys that set pitches

    /** LFE Mode, and LFE Level's reference: at -10 dB the LFE plays as loud as a main speaker in a calibrated room
        (which plays the LFE 10 dB hotter). */
    enum LfeMode { LfeOff = 0, LfeDiscrete, LfeMixdown, c_lfeModes };
    static constexpr float c_lfeReferenceDb = -10.0f;
    static constexpr float c_lfeMinDb = -40.0f, c_lfeMaxDb = 0.0f;
    /** How long the mains' phase match takes to come in or go out (see renderLfe). */
    static constexpr double c_lfeMatchSeconds = 0.5;

    /** Hit Position: where a hit lands (see the class comment). Hit Spread opens at c_hitSpreadDefault, a little
        bleed: measured (the Hits test), a hit at the front then has 5.6% of its power outside L and R at 5.1,
        7.6% at 7.1 and 9.1% at 7.1.4, each of the other speakers 15.5 to 19.4 dB under the whole hit. It has a
        size, and is still heard where it is. */
    enum HitPosition { HitWithPass = 0, HitFront, HitRandom, c_hitPositions };
    static constexpr float c_hitSpreadDefault = 0.2f;
    /** How far apart the two objects of a hit are at Hit Spread 0%: this much of the room's half width either side
        of its place (see the class comment). */
    static constexpr float c_hitApart = 0.1f;

    /** Stowaway: what the extra bus is, and its five knobs (the same names in every mode). */
    enum StowMode { StowSub = 0, StowWake, StowFly, StowDry, c_stowModes };
    enum StowKnob { StowFollow = 0, StowLow, StowFilter, StowDrive, StowWidth, c_stowKnobs };
    struct StowParams
    {
        bool on = true;      // off: silent, and the tracks sent here wait (they are not played anywhere else)
        int mode = StowSub;
        float follow = 0.7f; // how much it goes by bus A
        float low = 0.0f;    // Low End
        float filter = 0.5f; // Sub: the crossover (::Params::stowCrossoverHz); otherwise a tilt, open in the middle
        float drive = 0.0f;
        float width = 1.0f;
        int path = 0;        // Fly's Flight Path in a room (room::Path)
        // Fly's Direction and Side, as a bus's (see BusParams).
        int direction = 0, side = 0;
        bool directionWired = false, sideWired = false;
        float directionMod = 0.0f, sideMod = 0.0f;
    };

    struct BusParams
    {
        float doppler = 0.5f;   // how dramatic each pass is (closeness, loudness swell, air)
        float width = 0.8f;     // how far a pass travels across the stereo field
        float bend = 0.7f;      // how much doppler pitch bend
        float low = 0.3f, swirl = 0.2f, filter = 0.3f, drive = 0.1f;
        float duck = 0.4f;  // ducked by bus A (ignored on bus A)
        float key = 0.3f;   // low-passed by bus A (ignored on bus A)
        int passMode = 0;   // 0 Bed (two overlapped passes, a steady level), 1 Fly-By (one pass at a time). Linked: bus A's.
        int path = 0;       // Flight Path in a room (room::Path): 0 Across, 1 Through, 2 Overhead, 3 Around. Linked: bus A's.
        // Direction: which way a pass flies. 0 Random (each pass draws its own), 1 forwards (left to right on
        // Across, also in Stereo; front to back on Through and Overhead; clockwise on Around), 2 backwards.
        // Side: which side of the listener a pass takes (Through, Overhead), and where an Around pass is half way
        // round, at its loudest. 0 Random, 1 left (Around: loudest in front), 2 right (Around: loudest behind).
        // A modulator wired to either decides instead, pass by pass, as the pass starts: above its middle
        // (...Mod >= 0) is forwards / the first side, below it backwards / the other. Linked: bus A's.
        int direction = 0, side = 0;
        bool directionWired = false, sideWired = false;
        float directionMod = 0.0f, sideMod = 0.0f;
    };

    struct Params
    {
        bool rideOn = true;
        bool followMaster = false; // the rides swell with the Master track's loudness instead of their own LFOs
        float energy = 0.6f;   // how far the auto-ride pulls the faders
        float rate = 0.4f;     // how fast the faders move (and how long a doppler pass is)
        float fill = 0.3f;     // long delays into the gaps
        float chaos = 0.4f;    // randomness of the rides and the passes
        float pitch = 0.0f;    // semitones, every track
        // Held keys (semitones from middle C, lowest first): each is added to a track's own pitch. One key: every
        // track. Several: spread over the loaded tracks in order (the first takes the lowest), wrapping round.
        std::array<float, c_maxKeys> keys {};
        int numKeys = 0;
        float throwSeconds = 0.0f; // Throw Length (see ::Params::throwSeconds; under the minimum = follows Rate)
        // Modulator offsets for the knobs that glide on springs, in the knob's own units. They are added after the
        // spring, so a fast modulator isn't smoothed away by the glide meant for turning the knob.
        float energyMod = 0.0f, pitchMod = 0.0f, fillMod = 0.0f, spaceMod = 0.0f;
        std::array<std::array<float, 9>, c_maxBuses> busMod {}; // doppler width bend low swirl filter drive duck key
        std::array<float, c_stowKnobs> stowMod {};              // follow low filter drive width
        std::array<float, c_tracks> levels { 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f }; // gain (fader law applied)
        std::array<float, c_tracks> trackPitch {}; // semitones (cents included), each track on its own
        std::array<bool, c_tracks> trackVari { true, true, true, true, true, true, true, true, true, true }; // off = keep the speed

        std::array<BusParams, c_maxBuses> buses { BusParams { 0.55f, 0.7f, 0.7f, 0.35f, 0.3f, 0.3f, 0.2f, 0.0f, 0.0f },
                                                  BusParams { 0.4f, 0.7f, 0.6f, 0.3f, 0.3f, 0.4f, 0.0f, 0.35f, 0.3f } };
        StowParams stow;

        float hitSense = 0.5f; // how easily the hit gate opens
        float hitBoost = 6.0f; // dB when open (-3 .. +12)
        float hitRelease = 0.35f; // 0..1 -> 60..1200 ms
        float hitDuck = 0.4f;  // hits duck A and B (0..1 -> 0..-12 dB at the hits' peaks)
        bool hitSync = false;  // hit tracks fire together, their transients lined up
        // Gate and ducking options (the defaults are the original behaviour).
        int gateMode = 0;        // 0 Peaks (loudest moments, original) 1 Level 2 Jumps (sudden rises) 3 Open (no gate)
        float hitHold = 0.12f;   // 0..1 -> 0..500 ms the gate stays open (0.12 = 60 ms)
        float hitFloor = 0.0f;   // 0 = closes fully, else -28..-3 dB: hits never vanish completely
        int duckMode = 0;        // 0 Volume 1 Darken (low-pass the ducked bus) 2 Mids (duck only 250 Hz..4 kHz)
        float duckReturn = 0.0f; // 0 = fast (original) .. 1 = ducked buses take 1 s to come back
        bool takeTurns = false;  // A and B: only the louder one speaks, the other ducks under it
        bool linkBuses = false;  // B flies A's doppler passes, with A's Doppler and Width (its own Bend)
        bool hitAim = true;      // fire the hits so their transients land on the pass peaks (gate open)
        int hitPosition = HitWithPass;         // where a hit lands (see the class comment)
        float hitSpread = c_hitSpreadDefault;  // how far a hit bleeds into the other speakers (0 a point .. 1 every speaker alike)
        // Peak tremolo: deepens as the level nears its recent peak.
        bool tremOn = false;         // off: the tremolo fades out
        float tremDepth = 0.45f;     // 0..1 (knob; Params::tremDepthAmount gives the dip)
        float tremRate = 0.5f;       // 0..1 -> 1.5..30 Hz
        float tremThreshold = 0.6f;  // 0..1: how close to the peak it starts
        int tremShape = 0;           // 0 Sine, 1 Square, 2 Flutter
        int tremTarget = 0;          // 0 everything, 1 buses A + B, 2 hits

        float space = 0.3f;
        bool swell = false;    // held: every fader rides up

        // The room (Quad and wider; nothing at Mono and Stereo).
        float roomSpread = 0.0f;  // how far every object bleeds into the other speakers (0 a point .. 1 everywhere)
        float roomHeight = 1.0f;  // how much of an object's height reaches the top speakers
        float roomHeightVaries = 0.5f; // how far each pass's own altitude may differ: 0 every pass at ear level (Overhead: its full arch) ..
                                       // 1 anywhere from ear level to as high as an Overhead pass (Overhead: down to 60% of its arch)
        bool roomCentre = false;  // the centre speaker is used (off: a phantom centre between L and R)
        int lfeMode = LfeOff;     // what feeds the LFE (widths with one): see the class comment
        float lfeLevelDb = c_lfeReferenceDb; // the LFE's gain, -40 .. 0 dB

        // Morph: buses A and B are fed a spectral morph of the two (split back between them by X)
        bool morphOn = false;
        float morphX = 0.5f, morphY = 0.5f;
        int morphMode = 0;
    };

    struct FlightView
    {
        bool active = false;
        float env = 0.0f, x = 0.0f, closeness = 0.0f, age = 0.0f;
    };

    void prepare(double sampleRate, int maxBlockSize);
    void reset();
    void setCargoHold(CargoHold* hold) { m_cargo = hold; }
    /** The audio has stopped (reset() does this too): the players drop their sample data and tell the hold, so
        data replaced since is freed without waiting for the engine to run again. Not while process() may run. */
    void letGoOfCargo();
    void setParams(const Params& p);
    /** Start every track again from its start marker (audio thread; the processor calls it on Play). If the
        sources are still sounding they fade out first (5 ms), so the restart never cuts a wave short. */
    void restart();
    /** Stop pressed: the sources fade out over 60 ms and stay silent while the Fill echoes, the passes in flight
        and Space ring out. Play lets them back in. Audio thread (the processor sets it before each process()). */
    void setReleased(bool released) { m_released = released; }
    /** The transport started: fade the output out (5 ms, whether anything sounds or not, so the timing is always
        the same), then start everything again from the same seed and from empty delays and reverb, so the same
        settings give the same ride every time. Throws asked for meanwhile start with the new run. Audio thread. */
    void restartSeeded() { m_reseedPending = true; }
    /** The seed every run starts from (prepare() and a seeded restart use it): another seed draws other passes,
        other sounds from each track and other rides from the same settings. Before prepare(), or with a seeded
        restart after it. */
    static constexpr std::uint32_t c_defaultSeed = 0x5EA5EAu;
    void setSeed(std::uint32_t seed) { m_seed = seed != 0 ? seed : c_defaultSeed; }
    std::uint32_t getSeed() const { return m_seed; }
    /** A seeded restart has been asked for and the old sound is still fading out. Audio thread. */
    bool isRestarting() const { return m_reseedPending; }
    /** How a thrown whoosh flies, beyond what its velocity says. As it is made (and as the Throw button and MIDI
        throw): half the Throw Length coming in, as close and as loud as the bus draws it, the tracks at their
        fader levels. Render Variations flies each whoosh of a batch differently with it. */
    struct ThrowFlight
    {
        float approach = 0.5f; // the share of Throw Length spent coming in (0.2 .. 0.8), so where the closest point falls
        float nearer = 0.0f;   // -1 .. 1: nearer than the bus draws it (louder, brighter, a steeper bend), or further off
        bool ridden = false;   // the tracks in the balance their rides have them in when it is thrown: each at a moment
                               // of its own swell, the loudest at its fader and none more than 9 dB under its own;
                               // with Auto Ride off that is every track at its fader, as without it
        // Which way it flies and which side it takes, where bus A's Direction and Side are Random (a choice made
        // there, or a modulator wired to it, wins): +1 forwards / the right (Around: loudest behind), -1
        // backwards / the left (Around: loudest in front), 0 the engine's own draw. And how high it flies of what
        // Height Varies allows (0 .. 1; below 0: the engine's own draw).
        int direction = 0, side = 0;
        float lift = -1.0f;
    };
    /** Throw one whoosh: a single pass, the same on both buses, that starts delaySamples from the start of the
        next process() call and peaks half a Throw Length later (or as `flight` says). velocity 0..1 sets how close
        and how loud it is; below 0 the pass is as close as the bus would draw it (Doppler and Chaos), moved by
        flight.nearer. With every throw voice in flight it is dropped. Audio thread, before process(). */
    void throwWhoosh(int delaySamples, float velocity, const ThrowFlight& flight);
    void throwWhoosh(int delaySamples, float velocity);

    /** Where every track is in its sounds (the players), and where each fader's ride is. Render Variations takes it
        at the end of one render of a batch and gives it back for the next, so a batch carries on through the sounds
        the way the rig does when you throw several in a row, instead of every render starting from the start
        markers. Not on a live rig: only on one no other thread runs, between process() calls. */
    struct Places
    {
        std::array<CargoPlayer, c_tracks> players;
        std::array<double, c_tracks> ridePhase {};
        std::array<float, c_tracks> rideDepth {};
    };
    Places getPlaces() const;
    /** After prepare(): the tracks carry on from these places (one-shots still sounding are let go) and the rides
        from theirs. */
    void setPlaces(const Places& places);
    /** After prepare(): every fader's ride from a moment drawn from `seed` (where it is in its swell, and how big the
        swell is), so a throw that is ridden (ThrowFlight::ridden) gets the balance of the tracks at that moment. */
    void scatterRide(std::uint32_t seed);
    /** A throw is waiting, in flight, or its sources are still open. Audio thread. */
    bool isThrowing() const;
    /** The width to render: Mono and Stereo are the stereo engine (a Mono output folds it itself); Quad and wider
        is the room. Before prepare(), or on the audio thread between process() calls: from one room width to
        another the speakers' gains glide across the next chunk; between stereo and a room the output fades out
        (5 ms), swaps, and fades back in. */
    void setWidth(room::Width width) { m_widthWanted = width; }
    /** The width in use (the wanted one once a change has gone through). */
    room::Width getWidth() const { return m_width; }
    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples);
    /** The same into numChannels (2 or more) channels in the width's processing order (room::layout): the first
        two are L and R at every width from Stereo up. Channels the width does not have are silent, and so is its
        LFE. Fewer channels than the width has: the rest of the room is not written.
        The engine works in chunks of c_chunk samples counted from the start of each call (the last may be short),
        so what it renders depends on how a stream is cut into calls. The plug-in always calls it with the same
        lengths whatever the host's block sizes are (SwooshbucklerAudioProcessor::c_quantum). */
    void process(const float* inL, const float* inR, float* const* out, int numChannels, int numSamples);
    /** Audio thread, after process(), in a room: Stowaway's Sub as one stem: the same samples that went to L and
        R (each got exactly this), before the Everything tremolo and Space. (The LFE in Discrete is fed from its
        own copy, which goes on through those as L and R do: see renderLfe.) Silent in every other
        mode and at Mono and Stereo. Valid for the samples of the last process() call (up to c_stemSamples). */
    const float* getSubStem() const { return m_subStem.data(); }
    /** The LFE is carrying a copy of what the main speakers carry (Mixdown, or Discrete with Stowaway on in Sub,
        in a width with an LFE), so the mains' low end is being kept in step with it (lfe::PhaseMatch). False: the
        mains go straight through. As of the last chunk rendered; any thread. */
    bool lfeCarriesMains() const { return m_lfeCarries.load(std::memory_order_relaxed); }
    static constexpr int c_stemSamples = 8192;
    float getLatencySamples() const;

    /** The moments of the last process() call worth a marker in a recording: a doppler pass at its closest (while
        that bus sounds and the rig plays; a linked bus B flies bus A's passes, so only A's are told), a throw at
        its peak, and a hit: an aimed or synced hit where its transients land, or the hit gate opening. offset is
        the sample of that call. Audio thread, after process(). */
    enum MarkerKind { MarkPassA = 0, MarkPassB, MarkThrow, MarkHit, MarkPassStow }; // MarkPassStow: Stowaway's own pass (Fly) at its closest
    struct Marker
    {
        int offset = 0;
        int kind = MarkPassA;
    };
    int getNumMarkers() const { return m_numMarkers; }
    const Marker& getMarker(int index) const { return m_markers[static_cast<size_t>(index)]; }

    /** The objects of the room: what is where right now, and how loud (any thread). Bus A's two passes, bus B's,
        the four throws (bus A's copy: every bus flies the same one), the hits, and Stowaway (Fly: its two passes;
        Sub, Wake and Dry: the first only). Nothing is active at Mono and Stereo. */
    enum Object { ObjPassA1 = 0, ObjPassA2, ObjPassB1, ObjPassB2, ObjThrow1, ObjThrow2, ObjThrow3, ObjThrow4,
                  ObjHits, ObjStow1, ObjStow2, c_objects };
    struct ObjectView
    {
        bool active = false;
        float x = 0.0f, y = 1.0f, z = 0.0f; // room::Position
        float level = 0.0f;                 // 0..1, as the meters read (getBusLevel): its share of its section's level
    };
    ObjectView getObject(int object) const;
    /** The hits right now (any thread), at every width: where they are (room::Position; at Stereo x alone means
        anything, at Mono nothing does), Hit Spread as it is in use (with its modulation), how far apart the two
        objects of a stereo hit are (in x, either side of the place), and how loud they are (0..1, as the meters
        read). */
    struct HitView
    {
        float x = 0.0f, y = 1.0f, z = 0.0f;
        float spread = 0.0f, apart = 0.0f, level = 0.0f;
    };
    HitView getHitView() const;

    // UI (any thread)
    FlightView getFlightView(int index) const;
    float getEnergy() const { return m_energyView.load(std::memory_order_relaxed); }
    float getRide(int track) const { return m_rideView[static_cast<size_t>(track)].load(std::memory_order_relaxed); }
    float getHitGate() const { return m_gateView.load(std::memory_order_relaxed); }
    /** Seconds until the last throw's peak is heard (counts down), or < 0 when none is on its way in. */
    float getThrowPeakIn() const { return m_throwPeakView.load(std::memory_order_relaxed); }
    /** Goes up by one with every throw that starts. */
    int getThrowCount() const { return m_throwCountView.load(std::memory_order_relaxed); }
    /** Each track's pitch from the held keys right now, in semitones (0 with none held). */
    float getKeyPitch(int track) const { return m_keyPitchView[static_cast<size_t>(track)].load(std::memory_order_relaxed); }
    /** Hit sync, for the track view: which region (CargoPlayer::regionAt index) each synced hit track fired last
        (-1 = none / not synced), and a counter that goes up on every shared beat. */
    /** Published when the beat's transients are heard (not when it is cued, up to 1 s earlier). */
    int getHitRegion(int track) const { return m_hitRegionView[static_cast<size_t>(track)].load(std::memory_order_relaxed); }
    int getHitBeats() const { return m_hitBeatView.load(std::memory_order_relaxed); }
    /** The beat window the engine used last, in SOURCE seconds (pitch and caps applied), 0 = no beat yet. */
    double getHitPre() const { return m_hitPreView.load(std::memory_order_relaxed); }
    double getHitPost() const { return m_hitPostView.load(std::memory_order_relaxed); }
    /** The sections that add to the mix, as getScope and takeSectionLevels number them. */
    enum Section { SectionBusA = 0, SectionBusB, SectionHits, SectionStow, c_sections };
    /** Little waveforms for the section scopes: what each Section adds to the mix. */
    const ScopeRing& getScope(int which) const { return m_scopes[static_cast<size_t>(std::clamp(which, 0, c_sections - 1))]; }
    float getBusLevel(int bus) const { return m_busView[static_cast<size_t>(std::clamp(bus, 0, c_maxBuses - 1))].load(std::memory_order_relaxed); }
    /** Stowaway, for the UI (any thread): its level (0..1, as getBusLevel), and where its sound sits (-1 left ..
        1 right). */
    float getStowLevel() const { return m_stowLevelView.load(std::memory_order_relaxed); }
    float getStowPan() const { return m_stowPanView.load(std::memory_order_relaxed); }
    /** Audio thread, after process(): Stowaway's mode and knobs as the last chunk used them (the knobs after
        their glides, with the modulators' offsets; the mode is the one in use, which a change of Mode reaches
        once the old one has faded out). */
    const StowParams& getStowNow() const { return m_stow.now; }
    /** Audio thread, after process(): Stowaway's level at the end of the last chunk (its own follower, not
        scaled or clamped for a meter as getStowLevel is). */
    float getStowEnv() const { return m_stow.env; }
    /** Sub mode's Follow: Stowaway's gain for the swell it goes by (0 nothing .. 1 the loudest moment). The knob
        is how much of that swell, in dB, the sub takes on (0% none: a steady sub; 100% all of it). */
    static float subFollowGain(float follow, float key);
    /** The make-up that goes with Follow: the more of the swell the sub takes on, the longer it spends down in the
        dips, so it is turned up to match. On average it stays as loud wherever the knob is; what changes is how far
        it moves. How much that takes is measured as it plays (see FollowMakeup): 4.5 dB at 100% under a typical
        bed, which is where it starts, and never more than 6 dB. */
    static constexpr float c_subFollowTypicalDb = 4.5f;
    static constexpr float c_subFollowMakeupDb = 6.0f;
    /** Wake: how far open the trail is (0 shut .. 1 open) for a pass this far on its way out (0 = at its closest,
        1 = gone): it blooms just behind the pass and fades as the pass leaves. */
    static float wakeBloom(float out);
    /** Wake: the most the make-up that goes with Follow may turn the trail up (see renderWake: on average the
        trail is as loud wherever the knob is). */
    static constexpr float c_wakeFollowMakeupDb = 12.0f;
    /** Audio thread, after process(): how far open Wake's trail was at the end of the last chunk (0..1; 0 in the
        other modes). */
    float getWakeAmount() const { return m_stow.wake.amount; }
    /** Any thread: Stowaway's own two passes (Fly), as getFlightView gives the buses' (nothing in the other
        modes, or while Fly sleeps). Kept apart from those so the buses' and the throws' numbers stay as they are. */
    FlightView getStowFlight(int head) const;
    /** Fly: its passes' fixed Doppler and Bend (it has no knobs for them). */
    static constexpr float c_flyDoppler = 0.6f, c_flyBend = 0.7f;
    /** Fly's Follow: how long the next half of a pass is (seconds). free: the length its own draw gave. toPeak:
        how long until bus A's next pass is at its closest. toSecond: until the one after that, if that is known
        yet (otherwise < 0). halfA: half of bus A's pass in flight (the best guess at how far apart A's later
        peaks are). At 0% it is `free`; at 100% it ends on the peak of A's nearest to where `free` would have
        ended (so the pass lands with A's); in between, part of the way there. */
    static float flySegmentSeconds(float free, float toPeak, float toSecond, float halfA, float follow);
    /** Audio thread, after process(): Fly is awake (sound is in its line). */
    bool isFlyAwake() const { return m_stow.fly.on; }
    /** Audio thread: Stowaway is switched off and has faded right out. Its tracks are then not played at all. */
    bool isStowShut() const { return ! m_params.stow.on && m_stow.gate <= 0.0f; }
    /** The average power a ridden fader is left at (1 = not ridden) when `exponent` of its ride (in dB) is kept,
        for a ride of this depth (0 = still .. 1 = full dips) at this Chaos. */
    static float ridePower(float depth, float chaos, float exponent);
    /** A track's own VARI speed (1 = as the global Pitch plays it; 1 for VARI off), for the hit sync view. */
    float getTrackSpeed(int track) const { return m_trackSpeedView[static_cast<size_t>(track)].load(std::memory_order_relaxed); }
    /** The fill echo times of a track, in seconds (tap 0..2). Every track has its own, so layered tracks don't
        echo on one shared grid. */
    static float fillTapSeconds(int track, int tap);
    /** A pass's loudness window over its progress (0..1, 0.5 = closest). shape 0 = Bed: a sine, so two passes half
        a pass apart add up to a steady level. shape 1 = Fly-By: a long approach, the peak at the closest point and
        a shorter tail that is gone by the time the next pass is half-way in. In between: a blend (mode changes glide). */
    static float passWindow(float progress, float shape);
    /** The gain the mix is eased down by for a ride of this depth (0 = faders still .. 1 = full dips) at this
        Chaos: 1 from the default ride depth up, lower as the faders move less (they then sit at full level). */
    static float rideMakeup(float depth, float chaos);

    /** Audio thread: RMS of each Section (what it adds to the mix) since the last call, for the Follower
        modulators. */
    std::array<float, c_sections> takeSectionLevels();
    /** Audio thread, after process(): the Master track where it is playing now: its loudness, pitch and
        brightness, each 0..1 over the sound's own range (see CargoContours). All 0 with no Master; the loudness
        is 0 while the rig is stopped. */
    const std::array<float, 3>& getMasterFollow() const { return m_masterFollow; }
    /** Any thread: the Master's loudness as the rides follow it (0..1), for the UI. */
    float getMasterLevel() const { return m_masterView.load(std::memory_order_relaxed); }
    /** Any thread: the track each side of the morph is listening to right now (-1 = its whole bus). */
    int getMorphSource(int side) const { return m_ropeView[static_cast<size_t>(side & 1)].load(std::memory_order_relaxed); }

private:
    static constexpr int c_chunk = 32;

    struct Head
    {
        double time = 0.0;     // seconds since the pass started
        // A pass approaches for lenIn seconds, is closest (and loudest) at lenIn, and recedes for lenOut seconds.
        // The first head's halves are equal. The second head is kept exactly half a pass from the first, so its
        // approach is half of one of the first head's passes and its way out is half of the next.
        float lenIn = 1.5f, lenOut = 1.5f, length = 3.0f;
        float mach = 0.2f, closest = 0.02f, dir = 1.0f, closeness = 0.5f;
        float machBase = 0.2f; // the pass's speed before Bend (a linked bus applies its own Bend to the shared pass)
        float machRef = 0.2f;  // that speed within the limits: the swell and the air filter follow it, whatever Bend is
        bool aimed = false;    // AIM has decided about this pass's peak
        float side = 1.0f;     // a room: which side of the listener it passes (Through, Overhead: -1 left, +1 right), or where it is
                               // half way round (Around: +1 behind, -1 ahead). Drawn pass by pass, or as Side says
        float lift = 0.0f;     // a room: how high it flies of what Height Varies allows (0 .. 1), drawn pass by pass
        // Around: the nearness its level is held at and how bright it is held (see orbitLoudness), and what they
        // were worked out for.
        float orbitLoud = 0.25f, orbitAir = 1.0f;
        std::array<float, 7> orbitFor { -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        double lastBase = 0.0; // a room: the delay it was read at last (for the read's speed while a path glides)
        sj::OnePole airL, airR;
        sj::OnePole airSL, airSR; // the same air on a stereo bed's side signal, at each ear
        float view = 0.0f, x = 0.0f, age = 0.0f;

        /** How far through the pass (0..1, 0.5 = closest). */
        float progress() const
        {
            const double u = time < static_cast<double>(lenIn) ? 0.5 * time / static_cast<double>(lenIn)
                                                               : 0.5 + 0.5 * (time - static_cast<double>(lenIn)) / static_cast<double>(lenOut);
            return static_cast<float>(u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u));
        }
    };

    /** The doppler core: one instant of a pass. It is the pass as a point in space (how far through it is, where
        it is across the field, how near it is) and what that does to its sound (how long ago the sound heard now
        was sent, and the band limit reading it there needs). Bus A, bus B, the throws and Stowaway's Fly all fly
        their passes through it. For stereo the two ears read the line either side of `base` (hearPass); a room
        reads it once, at `base`, and places that one stream by `az` and `nearness` itself. */
    struct PassPoint
    {
        float u = 0.0f;        // how far through the pass (0..1, 0.5 = closest)
        float az = 0.0f;       // where it is across the field (-1 left .. 1 right), after Width
        float nearness = 0.0f; // its closest distance over its distance now (1 at its closest)
        float loud = 0.0f;     // the nearness its level is taken from: nearness itself, but for a pass going Around
        float panL = 0.0f, panR = 0.0f; // that place as stereo gains (constant power, 0.707 each in the middle)
        double base = 0.0;     // seconds: how long ago the sound heard now was sent
        double ear = 0.0;      // seconds: the left ear hears it this much later, the right this much sooner
        double stretch = 1.0;  // the band limit for the read (sj::SincKernel::stretchFor)
    };
    /** Width's law: how far a pass travels across the field for a Width of 0..1. */
    static float passWidthLaw(float width);
    /** Where a pass is across the field (-1 left .. 1 right) at progress u, flying in direction dir. */
    static float passAzimuth(float u, float dir, float widthLaw);
    /** The pass h at progress u (h.progress(), which the caller has already). */
    static PassPoint passPoint(const Head& h, float u, float widthLaw);
    /** Air, set once per chunk: the pass dulls with distance (by `doppler`), and the ear turned away from it hears
        it duller still. sides: also for a stereo bed's side signal. */
    static void passAir(Head& h, float doppler, float widthLaw, float fs, bool sides);
    /** The pass at pt, heard in stereo from `line` and added to l and r: each ear's band-limited read (back:
        how many samples before the newest one in the line this sample is), its air, the gain and the pan. */
    void hearPass(Head& h, const sj::DelayLine& line, const PassPoint& pt, double back, float gain, float& l, float& r) const;

    // The room.
    /** A bus's Flight Path as it is in use: a change glides from the old path to the new one (c_pathSeconds),
        and the next change waits for that to finish. */
    struct PathGlide
    {
        int from = 0, to = 0;
        float mix = 1.0f; // how far the glide has got (1 = arrived)
        void snap(int path) { from = to = path; mix = 1.0f; }
        void step(int wanted, float seconds);
        /** How far along the glide the pass is (eased). */
        float blend() const { return mix * mix * (3.0f - 2.0f * mix); }
        /** How far the path in use is Around (see Place::orbit). */
        float orbit() const
        {
            const float a = from == static_cast<int>(room::Path::Around) ? 1.0f : 0.0f;
            const float b = to == static_cast<int>(room::Path::Around) ? 1.0f : 0.0f;
            return a + (b - a) * blend();
        }
    };
    /** Where a pass is in the room, and what the room makes of its flight. */
    struct Place
    {
        room::Position position;
        float distance = 1.0f; // from the listener, in room units
        float orbit = 0.0f;    // how far the path is Around (0..1): no approach, so no bend and no swell
    };
    /** The pass h on a path, `along` of the way down it (see pathProgress): Width (its law) is how much of the path
        it travels. Around: the whole circle at a Width of c_orbitFullWidth, less of it under that, and further
        round over it. Its height: see Params::roomHeightVaries. */
    Place placeOf(const Head& h, float along, float widthLaw, const PathGlide& path) const;
    /** How far along its path a pass is (0 .. 1, 0.5 its nearest point) at progress u, for a window of this shape
        (0 Bed .. 1 Fly-By): the path is flown over the part of the pass that is heard, within 10 dB of the
        window's top, and the pass waits at the path's ends while it fades in and out. */
    static float pathProgress(float u, float shape);
    /** Around: the nearness a pass's level is held at so that it is as loud, all the way round, as the same pass
        is over its swell on the other paths (the same energy under the same window), and (h.orbitAir) how bright
        it is held: as bright as that swell is on average. Worked out again only when what they depend on changes. */
    static float orbitLoudness(Head& h, float doppler, float shape);
    /** The speakers' gains for a stream at a place (they carry the +3 dB a centred stream has in stereo, so a
        stream at the phantom centre is as loud in L and R as it is in the stereo engine). An object near the
        listener bleeds into more speakers, on top of Spread. */
    void roomGains(const room::Position& position, float distance, room::Gains& gains) const;
    /** The same pass point for a room: by `orbit`, the delay, the read's band limit and the nearness go to those
        of a pass that keeps its distance. Keeps h.lastBase. */
    PassPoint roomPoint(Head& h, float u, float widthLaw, float orbit) const;
    /** Air for a room: the pass dulls with distance as in stereo, with no head shadow (one filter, airL). */
    static void roomAir(Head& h, float doppler, float orbit, float fs, bool sides, float shape);
    /** The throws' lane: the speaker gains of the voices in flight (heads: this bus's copies), blended by their
        power. at: where each is, if wanted. False with none in flight (the lane keeps the gains it had). */
    bool throwGains(const std::array<Head, c_throws>& heads, float widthLaw, room::Gains& gains, room::Position* at, float* distance = nullptr) const;
    /** x, ramped from gains `from` to gains `to`, added to the room's mix. */
    void addToRoom(const float* x, const room::Gains& from, const room::Gains& to, int frames);
    /** The width in use follows the wanted one (see setWidth); returns the output's fade across this chunk. */
    void stepWidth(int frames, float& fade0, float& fade1);
    /** The room's settings glide. */
    void stepRoom(int frames);
    /** The hits' place this chunk (Hit Position), and their speakers' gains at the end of it (see hitGains). */
    void placeHits(int frames);
    /** The gains of the hits' two lanes, mid and side, on the speakers of the width in use, for a hit at `at`
        (see the class comment): its two objects' gains l and r on a speaker give the mid l + r and the side
        l - r there. (Their level together: see mixHits.) */
    void hitGains(const room::Position& at, float distance, float spread, room::Gains& mid, room::Gains& side) const;
    /** The hits' two lanes to the speakers (a room: into the mix; Mono and Stereo: into m_hitsOutL and
        m_hitsOutR), their gains ramped across the chunk, at the level that gives the two objects together the
        power the hit has as a point: sqrt(mid / (mid + side + c (mid - side))), where mid and side are the
        sound's mid and side powers and c is how far the two objects share speakers (the sum of l r, 0 .. 1).
        A mono sound: 1 / sqrt(1 + c). Two objects in the same speakers (c = 1): 1 / sqrt(2) whatever the sound. */
    void mixHits(int frames);
    /** Where a Random hit is, from its draws, at the width in use. */
    room::Position randomHitPlace() const;
    /** A room's mix: every lane to its speakers. */
    void mixRoom(int frames, bool stowSounds, bool stowDry);
    /** Space for a room: see the class comment. Returns false when Space is off (nothing done). */
    bool renderSpaceRoom(int frames);
    /** Space's amount for this chunk (the knob's spring stepped, plus its modulation), or -1 when Space is off:
        then the echoes and the reverbs are emptied once. wasOn: Space was on before this chunk. */
    float stepSpace(int frames, bool& wasOn);
    /** The ping-pong echoes, in place on a stereo pair. */
    void renderEchoes(int frames, float* left, float* right, float s, bool wasOn);
    /** The reverb's dry and wet gains for Space s: sets m_spaceDryFrom, m_spaceDry, m_spaceWet. Returns the
        wet gain at the start of the chunk. */
    float stepSpaceGains(float s, bool wasOn);
    /** The room's reverb taps this chunk: each speaker's share (see the class comment), glided. */
    void roomReverbTaps(int frames, RoomReverb::Taps& taps);
    /** A room: (a width with an LFE) the LFE channel from LFE Mode, and the mains through the phase match while
        the LFE carries a copy of them. */
    void renderLfe(int frames);

    /** What Low End adds under a low band: a note an octave below it and its overtones. The buses feed it their
        low band (under 130 Hz); Stowaway's Sub feeds it what its crossover let through. */
    struct LowAdd
    {
        sj::Svf detectHp, subA, subB, harm;
        sj::Follower follower;
        float flip = 1.0f;
        bool armed = false; // the sub octave's trigger: armed below the low band's level, fires above it

        /** detectHz: under this the trigger does not listen. subHz: the sub octave's own low-pass. harmHz: where
            the overtones are centred. */
        void tune(float detectHz, float subHz, float harmHz, float fs)
        {
            detectHp.set(detectHz, 0.7071f, fs);
            subA.set(subHz, 0.7071f, fs);
            subB.set(subHz, 0.7071f, fs);
            harm.set(harmHz, 0.8f, fs);
        }
        void prepare(float fs)
        {
            tune(40.0f, 90.0f, 170.0f, fs);
            follower.set(5.0f, 90.0f, fs);
        }
        void reset()
        {
            for (auto* f : { &detectHp, &subA, &subB, &harm })
                f->reset();
            follower.env = 0.0f;
            flip = 1.0f;
            armed = false;
        }
        /** One sample of the low band in, what to add to it out (before the Low End amount). */
        float process(float low)
        {
            const float level = follower.process(low);
            // The sub octave flips once per cycle of the low band. The trigger has to swing a tenth of the band's
            // level below zero and then above it, so ripples and noise riding on the wave don't flip it at random
            // (which is heard as rumble instead of a note an octave down).
            const float detect = detectHp.highpass(low);
            const float swing = 0.1f * level + 1.0e-6f;
            if (detect < -swing)
                armed = true;
            else if (armed && detect > swing)
            {
                flip = -flip;
                armed = false;
            }
            const float sub = subB.lowpass(subA.lowpass(flip * level));
            const float overtones = harm.bandpass(sj::fastTanh(4.0f * std::abs(low)));
            return 1.3f * sub + 0.45f * overtones;
        }
    };

    struct BusState
    {
        sj::DelayLine line;
        std::array<Head, 2> heads;
        // Throws: the tracks at their fader levels, and one pass per throw voice reading them.
        sj::DelayLine throwLine;
        std::array<Head, c_throws> throwHeads;
        std::array<float, c_chunk> throwIn {};
        // low end
        sj::Svf lowA, lowB, shelfL, shelfR;
        LowAdd lowAdd;
        // swirl
        std::array<sj::Allpass1, 6> phL, phR;
        float phFbL = 0, phFbR = 0, lfo = 0;
        sj::DelayLine flL, flR;
        float flFbL = 0, flFbR = 0;
        // What Swirl reached at the end of the last chunk (everything is ramped from there across the next one),
        // and its own slower copy of the level-against-peak.
        bool swirlOn = false;
        float swPeak = 0.0f, swAL = 0.0f, swAR = 0.0f, swMsL = 0.3f, swMsR = 0.3f, swPhMix = 0.0f, swFlMix = 0.0f, swFb = 0.0f;
        // filter, and Filtered by A (its own low-pass)
        sj::Svf fL, fR, keyL, keyR;
        // level matching (slow mean squares) around Swirl and Drive, and the gains they set
        float swirlPre = 0.0f, swirlPost = 0.0f, swirlGain = 1.0f;
        float drivePre = 0.0f, drivePost = 0.0f, driveGain = 1.0f;
        sj::Follower follower;
        // drive tone
        sj::OnePole toneL, toneR;
        // level tracking
        float env = 0.0f, peakRef = 1.0e-3f;
        float passShape = 0.0f; // Pass Mode in use: 0 Bed .. 1 Fly-By (glides, so a change mid-pass doesn't step the level)
        float prox = 1.0f;      // how near the nearer pass is right now (its window x nearness), smoothed
        std::array<sj::Spring, 9> springs; // doppler width bend low swirl filter drive duck key
        float duck = 0.0f;
        // Ducking state (one for being ducked by the other bus, one for being ducked by the hits).
        struct Duck
        {
            float amount = 0.0f;              // applied attenuation 0..1 (after the return glide)
            float lp[4] {};                   // per channel: left and right, or a room's lanes
            sj::Svf band[4];                  // Mids: the 250 Hz .. 4 kHz band (centred on 1 kHz)
            void reset()
            {
                amount = 0.0f;
                for (auto& x : lp) x = 0.0f;
                for (auto& b : band) b.reset();
            }
        };
        Duck byBus, byHits;
        // l and r: left and right in stereo. In a room they are lanes: l is the first pass, r the second, t the
        // throws, and sideL the sides of a stereo bed (see the class comment).
        std::array<float, c_chunk> l {}, r {}, in {};
        std::array<float, c_chunk> t {};
        // The third lane's own colour chain (the same filters l and r have), whether it holds state, and how many
        // more samples it is left to ring for after the last throw.
        sj::Svf shelfT, fT, keyT;
        std::array<sj::Allpass1, 6> phT;
        float phFbT = 0.0f, flFbT = 0.0f, swAT = 0.0f, swMsT = 0.3f;
        sj::DelayLine flT;
        sj::OnePole toneT;
        bool thirdOn = false;
        int thirdTail = 0;
        int lanes = 2; // the lanes the mix takes this chunk: 2, or 4 (t and sideL too, silent when not in use)
        bool sidesOn = false; // a room: sideL holds a stereo bed's sides this chunk
        std::array<float, 3> lowShare { 0.5f, 0.5f, 0.0f }; // each lane's share of what Low End adds (by its power), at the end of the last chunk
        PathGlide path;
        // Each lane's speaker gains at the end of the last chunk and of this one (lane 3: the throws, blended by
        // their power), and whether the last chunk's are known.
        std::array<room::Gains, 3> gain {}, gainTo {};
        bool placed = false;
        // The passes and throws as objects: where each is at the end of this chunk, how far from the listener,
        // and the Width (its law) the passes flew at.
        std::array<room::Position, 2 + c_throws> at {};
        std::array<float, 2 + c_throws> atDistance { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        float lawNow = 0.7f;
        void resetThird();
        // Stereo beds (bus B): the tracks' side signals, their own delay line for the passes to read, what the
        // passes made of them this chunk, how many more samples the line still holds sound for, the amount it
        // was mixed in at the end of the last chunk, and the level match across the colour chain.
        sj::DelayLine sideLine;
        std::array<float, c_chunk> sideIn {}, sideL {}, sideR {};
        int sideHold = 0;
        float sideAmount = 0.0f;
        float sidePre = 0.0f, sidePost = 0.0f, sideMatch = 1.0f;
        // A stereo bed as loud wide as narrow: the mono sum's power and the sides' (slow means), and the gain.
        float bedMid = 0.0f, bedSide = 0.0f, bedGain = 1.0f;
    };

    /** Stowaway's Filter outside Sub, on one stream: open in the middle of the knob, a low-pass that closes towards
        the bottom and a high-pass that rises towards the top (::Params::stowLowpassHz, stowHighpassHz; 12 dB per
        octave each). Either side comes in and goes out over a few milliseconds, so crossing the middle never
        steps. */
    struct StowTilt
    {
        sj::Svf lp, hp;
        float lpMix = 0.0f, hpMix = 0.0f; // how far each side is in (0..1) at the end of the last chunk
        void reset()
        {
            lp.reset();
            hp.reset();
            lpMix = hpMix = 0.0f;
        }
        /** snap: nothing to glide from (the stream starts here). */
        void process(float* x, int frames, float knob, float fs, bool snap);
    };
    /** Stowaway's Drive, on one stream. The stream is driven against its own level (a slow measure of it), so the
        knob does the same to a quiet sound as to a loud one, and its travel is even: the gain into the saturator
        rises 2 dB per 10%, from all but clean to squared off. The result is brought back to the level it went in
        at: Drive thickens the sound and flattens its swell, it does not turn it up. */
    struct StowSaturator
    {
        float pre = 0.0f, post = 0.0f, gain = 1.0f; // the level before and after (slow mean squares), and the level match
        void reset()
        {
            pre = post = 0.0f;
            gain = 1.0f;
        }
        void process(float* x, int frames, float drive, float fs);
    };
    /** Low End, Filter and Drive for one moving stream of Stowaway's (Wake): the buses' low end (a note an octave
        under the low band, its overtones and a lift), the tilt, and the drive. */
    struct StowColour
    {
        sj::Svf lowA, lowB, shelf;
        LowAdd lowAdd;
        bool lowOn = false;     // Low End holds state
        float lowAmount = 0.0f; // Low End at the end of the last chunk
        StowTilt tilt;
        StowSaturator drive;
        void prepare(float fs);
        void reset();
        void process(float* x, int frames, const StowParams& now, float fs, bool snap);
    };

    /** The make-up that goes with Follow in Sub and Wake. Follow shuts the sound down between the moments it opens
        for, and how much quieter that leaves it on average depends on what it follows. So that is measured: a slow
        average of the power of the sound before Follow, and of what Follow at 25, 50, 75 and 100% would keep of it,
        gives what the knob costs at each of those. The make-up is the cost at the knob's place: it answers the knob
        at once, and only learns the material slowly. */
    struct FollowMakeup
    {
        float sent = 0.0f;            // the power before Follow (a slow average; 0 = nothing measured yet)
        std::array<float, 4> kept {}; // and of what Follow at 25, 50, 75 and 100% keeps of it
        void reset()
        {
            sent = 0.0f;
            kept = {};
        }
        /** inSq: this chunk's mean square before Follow. keeps: the share of its power Follow at 25, 50, 75 and
            100% keeps now. typicalDb: what those cost a typical sound (where the measure starts from). c: the
            averages' coefficient for this chunk. */
        void measure(float inSq, const float keeps[4], const float typicalDb[4], float c);
        /** What Follow at this place costs, in dB (0 with nothing measured). */
        float costDb(float follow) const;
    };

    // Stowaway. Beside the two rig buses, not one of them: nothing that loops over m_buses (the hit gate's
    // detector, AIM, LINK, Take Turns, the morph, the passes' random draws) knows it is there.
    struct StowState
    {
        // In: the tracks sent here (ridden, with their Fill echoes), and the same tracks at their fader levels
        // while a throw's sources are open.
        std::array<float, c_chunk> in {}, throwIn {};
        bool fed = false;      // a track played into `in` or `throwIn` this chunk
        bool sounding = false; // this chunk's output holds sound (otherwise it is left out of the mix altogether)
        // Out: one mono stream and where it sits (-1 left .. 1 right, the end of the chunk; panFrom is where it
        // was at the start), and the stereo made of the two, which is what the mix takes. Kept apart so a room
        // can place the stream itself (and send a Sub to its LFE) instead of taking the stereo.
        std::array<float, c_chunk> mono {}, l {}, r {};
        float pan = 0.0f, panFrom = 0.0f;
        // A room. Sub and Dry go to L and R as in stereo. Wake is `mono` at a place. Fly is three lanes: l its
        // first pass, r its second, mono its throws. The lanes' speaker gains at the end of the last chunk and of
        // this one, and whether the last chunk's are known.
        std::array<room::Gains, 3> gain {}, gainTo {};
        bool placed = false;
        PathGlide path;                       // Fly's Flight Path
        std::array<room::Position, 2> at {};  // where it is (Fly: its two passes)
        room::Position wakeAt;                // Wake: its place, smoothed
        float wakeDistance = 1.0f;
        StowParams now;        // see getStowNow
        std::array<sj::Spring, c_stowKnobs> springs; // follow low filter drive width
        float throwKey = 0.0f; // the loudest throw's own swell (its pass window x its level) at the end of the last chunk
        // The mode in use. A change of Mode fades the sound out, swaps (here), and fades it back in.
        int mode = StowSub;
        float fade = 1.0f;
        // The switch (StowParams::on): 1 open, 0 shut. It fades each way; shut, nothing here runs and the tracks
        // sent here wait unplayed (see isStowShut).
        float gate = 1.0f;
        // Dry: the side signals of the stereo tracks sent here, and what Filter and the hits left of them.
        std::array<float, c_chunk> sideIn {}, side {};
        bool sideFed = false;  // a track played into `sideIn` this chunk
        BusState::Duck byHits; // ducked by the hits (not in Sub)
        bool ducked = false;   // byHits holds something
        sj::Follower follower;
        float env = 0.0f;
        // Sub. `mono` is then the sub itself: one stem, in the middle (a room sends it to its LFE).
        struct Sub
        {
            SubFilter filter;
            LowAdd low;
            sj::Svf lowHp;          // keeps what Low End adds above 20 Hz (the octave under a 30 Hz note is 15 Hz)
            bool on = false;        // the filters hold state (cleared when Sub is left, or once its tail has rung out)
            int tail = 0;           // samples the filters may still ring for after the last track played in
            float hz = 0.0f;        // the crossover the filters are set to
            float lowAmount = 0.0f; // Low End at the end of the last chunk
            float gain = 0.0f;      // Follow's gain at the end of the last chunk
            float key = 1.0f;       // what Follow goes by (0..1), smoothed
            float busA = 0.0f, busB = 0.0f; // how far bus A and bus B hold tracks to follow (0..1, glides)
            std::array<float, c_maxBuses> peak { 1.0e-3f, 1.0e-3f }; // each bus's recent peak, as the sub measures its swell
            FollowMakeup makeup;
            StowSaturator drive;
            void reset();
        } sub;
        // Wake. `mono` is the trail and `pan` where it sits (behind bus A's outgoing pass).
        struct Wake
        {
            StowColour colour;
            bool on = false;     // it holds state
            float envDb = -60.0f; // bus A's level at the end of the last chunk (dB, never under -60)
            float fall = 0.0f;    // how far bus A has fallen from what it was (dB): drops add, rises take away, time lets it go
            float amount = 0.0f; // how far open the trail is (0..1), smoothed
            float gain = 0.0f;   // Follow's gain at the end of the last chunk
            FollowMakeup makeup;
            float pan = 0.0f;    // where the trail sits before Width (-1..1), smoothed
            void reset();
        } wake;
        // Dry. `mono` is the tracks' sum and `side` their side signals: left = mono + side, right = mono - side.
        struct Dry
        {
            StowTilt filter, sideFilter;
            bool on = false;       // it holds state
            bool sideOn = false;   // the side filter holds state
            int sideTail = 0;      // samples the side filter may still ring for after the last side signal
            float gain = 1.0f;     // the make-up at the end of the last chunk
            float width = 1.0f;    // Width at the end of the last chunk
            float levelMid = 0.0f, levelSide = 0.0f, level = 1.0f; // as loud wide as narrow: the sum's power and the sides' (slow means), and the gain
            float power = 1.0f, powerDepth = -1.0f, powerChaos = -1.0f, powerFollow = -1.0f; // ridePower, and what it was worked out for
            void reset();
        } dry;
        // Fly. One stream (the tracks, coloured) in `line`, and two passes that read it: where each is, is in
        // its Head, and passPoint gives its place and delay sample by sample (that is all a room needs to place it
        // itself). Here they are heard in stereo, straight into `l` and `r` (placeStow leaves those alone).
        struct Fly
        {
            sj::DelayLine line, throwLine;          // the stream, and its tracks at their fader levels for a throw
            std::array<Head, 2> heads;
            std::array<Head, c_throws> throwHeads;  // bus A's throws, flown here at Fly's own Bend
            sj::Random rng;                         // its own draws: seeded with the engine, never shared
            StowColour colour, throwColour;
            bool on = false;       // it is awake: the lines, heads and colours hold state
            bool throwOn = false;  // throwColour holds state
            int hold = 0;          // samples its lines still hold sound for
            int throwHold = 0;     // and the throw line
            // The passes' clock: half-passes, one after the other. At the end of each the head on its way in is at
            // its closest, the other has gone and sets off again, and the length of the next half is chosen (Follow).
            double segTime = 0.0;  // seconds into the half in progress
            float segLen = 1.5f;   // how long it is
            int inbound = 0;       // the head on its way in
            float inGain = 0.0f;   // the stream's way into the line: it fades in from sleep, so the line never starts on a step
            float level = 1.0f;    // its passes' make-up (see renderFly), glided
            float power = 1.0f, powerDepth = -1.0f, powerChaos = -1.0f; // ridePower, and what it was worked out for
            void reset();
        } fly;
        /** Leave the mode in use: everything it holds is cleared. */
        void leaveMode();
    };
    /** Fly: what was sent here, coloured, flown by Stowaway's own two passes into m_stow.l and m_stow.r. */
    void renderFly(int frames);
    /** Sub: m_stow.mono (what was sent here this chunk) through the sub's filters, colours and Follow. */
    void renderSub(int frames);
    /** Wake: what was sent here, opened as bus A falls away, coloured, and where it sits. key0 .. key1: the throws'
        own trail across the chunk (their share arrives in throwIn). */
    void renderWake(int frames, float key0, float key1);
    /** Dry: m_stow.mono and m_stow.side (the tracks and their sides) through Filter, at the ride's make-up. */
    void renderDry(int frames);
    /** The mono stream and its place for this chunk, from what was sent here (and what bus A did). */
    void renderStow(int frames);
    /** The stream at its place, as the stereo the mix takes. After the hits have ducked it. */
    void placeStow(int frames);
    /** Where a track's sound goes for the place it plays on (CargoHold::getBus): bus A, bus B, the hits or
        Stowaway, each by name. Anything else is bus A. */
    float* trackInput(int bus);
    /** The same for its feed into a throw. The hits have no throw line: nullptr. */
    float* throwInput(int bus);

    /** lenIn: how long the approach is, or < 0 to draw a whole new pass length (the first head of a bus). */
    /** rng: whose draws the pass is made of (nullptr: the buses', m_rng). */
    void startPass(Head& h, const BusParams& p, float lenIn = -1.0f, sj::Random* rng = nullptr);
    /** A pass's length before Chaos varies it: follows Rate (6 s .. 1.6 s). */
    float nominalPassSeconds() const;
    void startThrows(int frames);
    void reseedNow();
    void renderTracks(int frames);
    /** How strongly a bus keys the other one: its level against its own recent peak, faded out below -40 dBFS so a
        quiet bus with an old peak can't duck the other. */
    static float keyLevel(const BusState& bus);
    /** lead / leadStart: with the buses linked, bus A's passes as they are now (after A rendered this chunk) and as
        they were at the start of it; B follows them instead of rolling its own. nullptr when not linked. */
    /** key: bus A's level against its own recent peak (0..1, floored at quiet levels), for Ducked / Filtered by A. */
    void renderBus(BusState& bus, int frames, float key, bool isKeyBus,
                   const std::array<Head, 2>* lead = nullptr, const std::array<Head, 2>* leadStart = nullptr);
    /** newPass: also take the pass's speed at this bus's Bend (set once per pass, as unlinked); otherwise just keep
        in step with the lead (the speed stays, so moving Bend mid-pass never steps the delay). */
    static void followPass(Head& h, const Head& from, float bend, bool newPass);
    static void snapBus(BusState& bus, const BusParams& p);
    void renderHits(int frames);
    /** l, r: the stereo mix, in place. Returns false when Space is off (nothing done). */
    bool renderSpace(int frames, float* l, float* r);
    void publishViews();

    double m_sampleRate = 48000.0;
    float m_fs = 48000.0f;
    Params m_params;
    sj::Random m_rng;
    sj::Random m_placeRng; // a room's own draws (which side, how high): apart from m_rng, so Stereo draws what it always did
    std::uint32_t m_seed = c_defaultSeed;          // see setSeed
    std::uint32_t rideSeed(int track) const { return static_cast<std::uint32_t>(4242 + 977 * track) + (m_seed - c_defaultSeed); }
    CargoHold* m_cargo = nullptr;

    // Tracks
    std::array<CargoPlayer, c_tracks> m_players;
    std::array<sj::DelayLine, c_tracks> m_fillLines;
    std::array<std::array<float, 3>, c_tracks> m_fillTaps {}; // each track's echo times, in samples
    std::array<sj::OnePole, c_tracks> m_fillTone;
    std::array<float, c_tracks> m_fillAmount {}, m_coverage {};
    std::array<const CargoData*, c_tracks> m_coverageFor {};
    std::array<double, c_tracks> m_ridePhase {};
    std::array<sj::Drift, c_tracks> m_rideDrift;
    std::array<float, c_tracks> m_gain {};          // gain applied at the end of the previous chunk
    bool m_released = false;                        // see setReleased
    bool m_reseedPending = false;                   // see restartSeeded
    float m_outFade = 1.0f;                         // the output's gain around a seeded restart
    // Throws. A voice is one thrown pass (its heads are in the buses); the queue holds throws that start later in
    // the block. The tracks feed the throw lines through their own gain (m_throwGain), open while a voice needs it.
    struct ThrowVoice
    {
        bool active = false, aimed = false;
        float level = 1.0f;   // from the velocity
        double gate = 0.0;    // seconds the sources stay open for it
    };
    struct QueuedThrow
    {
        bool pending = false;
        int delay = 0;
        float velocity = -1.0f;
        ThrowFlight flight;
    };
    std::array<ThrowVoice, c_throws> m_throws;
    std::array<QueuedThrow, c_throws> m_throwQueue;
    std::array<float, c_tracks> m_throwGain {};     // each track's gain into the throw lines at the end of the last chunk
    // A ridden throw (ThrowFlight::ridden): each track's share of the throw, from the ride as it is thrown (the
    // loudest track at 1). 1 for every track otherwise. Taken from the ride of the chunk the throw starts in
    // (renderTracks works it out track by track) and used from the next, while the throw's sources are still opening.
    std::array<float, c_tracks> m_throwBalance {};
    std::array<float, c_tracks> m_throwBalanceRide {};
    bool m_throwBalanceWanted = false;
    float m_throwOpen = 0.0f;                       // 0..1: the throw's sources are open
    float m_throwDoppler = 0.5f, m_throwWidthLaw = 0.7f; // bus A's Doppler and Width this chunk: a throw is one pass on both buses
    float m_throwOrbit0 = 0.0f, m_throwOrbit1 = 0.0f;    // and how far bus A's Flight Path is Around, at the chunk's start and end
    std::atomic<float> m_throwPeakView { -1.0f };
    std::atomic<int> m_throwCountView { 0 };
    std::array<std::atomic<float>, c_tracks> m_keyPitchView {};
    bool m_restartPending = false;                  // restart() asked: done once the sources are silent
    void restartNow();
    float m_releaseGain = 1.0f;                     // 1 = the sources play, 0 = released
    bool m_releaseSnap = true;                      // the first chunk after prepare(): the sources start where setReleased says (open or let go), no fade
    std::array<sj::Spring, c_tracks> m_levelSprings;
    sj::Spring m_swellSpring, m_energySpring, m_pitchSpring, m_fillSpring, m_spaceSpring;
    // How still the faders are (0 = the ride at full depth .. 1 = not moving): the stiller, the more Low End, Swirl
    // and Filter go by how near the pass is, not by the level alone.
    float m_proxBlend = 0.0f;
    // Ride make-up: the gain on the mix, and the slow copy of Swell it follows (a Swell is a push at first, and
    // settles to the ridden level if it is held).
    float m_makeup = 1.0f, m_makeupTarget = 1.0f, m_makeupSwell = 0.0f;
    float m_makeupDepth = -1.0f, m_makeupChaos = -1.0f; // what m_makeupTarget was worked out for
    std::array<float, c_tracks> m_rideDepth {};
    // The modulator offsets, glided per chunk (the modulators update once per host block; a step would click).
    float m_energyModNow = 0.0f, m_pitchModNow = 0.0f, m_fillModNow = 0.0f, m_spaceModNow = 0.0f;
    std::array<std::array<float, 9>, c_maxBuses> m_busModNow {};
    std::array<float, c_stowKnobs> m_stowModNow {};
    float m_morphMixNow = 0.0f; // how far the morph is in (B isn't ducked or filtered by A while morphing)
    // Morph ropes. A roped track is rendered into its side's tap instead of its bus; the morph works on the tap and
    // what it leaves of the dry sound goes back to the track's bus. A change of rope while the morph is in fades
    // what the change moves out (15 ms), swaps, and fades it back in.
    std::array<std::array<float, c_chunk>, 2> m_morphTap {};
    std::array<int, 2> m_ropeNow { -1, -1 };     // the rope in use on each side
    std::array<float, 2> m_ropeFade { 1.0f, 1.0f };
    // What the fade in progress applies to: a bit per track, and c_ropeFadesBus for the rest of the side's bus.
    static constexpr std::uint32_t c_ropeFadesBus = 1u << 31;
    std::array<std::uint32_t, 2> m_ropeFading { 0, 0 };
    std::array<std::atomic<int>, 2> m_ropeView { { { -1 }, { -1 } } };
    int wantedRope(int side) const;
    // The Master track (see getMasterFollow), the smoothed loudness the rides follow, and how far Follow Master is in.
    std::array<float, 3> m_masterFollow {};
    float m_masterRide = 0.0f, m_followMix = 0.0f;
    std::atomic<float> m_masterView { 0.0f };
    void glideMods(int frames);
    std::array<float, c_chunk> m_trackScratch {}, m_sideScratch {};
    std::array<float, c_tracks> m_sideGain {};      // each track's side signal into bus B or Stowaway (0..1, fades in and out)
    // Where each track plays (CargoHold::getBus) as it is in use, -1 = not known yet. A change of place fades the
    // track out, swaps, and fades it back in, so nothing steps in the bus it leaves or the one it joins.
    std::array<int, c_tracks> m_busNow {};
    std::array<float, c_tracks> m_busFade {};
    MorphInsert m_morph;
    sj::Spring m_morphMix;

    // Buses
    std::array<BusState, c_maxBuses> m_buses;
    StowState m_stow;
    // The hits: what the hit tracks play (their mono sum and, for stereo sounds, their side signal), the same two
    // after the gate (the two lanes the mix places), and at Mono and Stereo what the speakers get of them.
    std::array<float, c_chunk> m_hitsIn {}, m_hitsSideIn {}, m_hitsMid {}, m_hitsSide {}, m_hitsOutL {}, m_hitsOutR {};
    bool m_hitsSideFed = false; // a stereo hit track played into m_hitsSideIn this chunk
    int m_hitsSideHold = 0;     // samples the side lane is still mixed for after the last of them
    sj::Follower m_detect, m_hitsFollower;
    float m_slowEnv = 0.0f;
    int m_talker = 0;          // take turns: 0 = A speaks, 1 = B speaks
    float m_talkerHold = 0.0f; // seconds the other bus has been clearly louder  // Jumps gate: the slow envelope the fast one has to jump above

    float duckReturnCoef() const;
    /** full: the amount at which Darken reaches its darkest (the deepest the caller ever asks for). */
    /** ch: one sample of each channel (left and right, or a room's lanes), n of them (up to 4). */
    void duckSample(BusState::Duck& d, float target, float returnCoef, float full, float* const* ch, int n);
    void duckSample(BusState::Duck& d, float target, float returnCoef, float full, float& l, float& r)
    {
        float* ch[2] = { &l, &r };
        duckSample(d, target, returnCoef, full, ch, 2);
    }
    float m_peakRef = 1.0e-3f, m_average = 0.0f, m_gate = 0.0f, m_hold = 0.0f;
    double m_hitClock = 0.0;     // samples to the next synced hit
    // Peak tremolo, one per thing it can shake: 0 the whole mix, 1 bus A, 2 bus B, 3 the hits.
    struct PeakTremolo
    {
        double phase = 0.0;
        float env = 0.0f, peak = 1.0e-4f, depth = 0.0f;
        float from = 0.5f, to = 0.5f;  // Flutter: the random levels it glides between each cycle
        sj::Random rng;                // its own, so a copy of this state draws the same Flutter
    };
    std::array<PeakTremolo, 4> m_trem;
    float m_tremAttack = 0.0f, m_tremRelease = 0.0f, m_tremPeakDecay = 0.0f, m_tremDepthUp = 0.0f, m_tremDepthDown = 0.0f;
    /** selected: this is what the tremolo shakes now. The others keep following their level and their LFO so a switch
        (or turning it on) starts from the present, and they fade out instead of stopping dead. */
    /** ch: the n channels it shakes (left and right, a room's lanes, or its speakers). detect: what it listens
        to instead of the loudest of them (a room's whole mix as one stream), or nullptr. */
    void applyTremolo(PeakTremolo& t, float* const* ch, int n, int frames, bool selected, const float* detect = nullptr);

    // Markers (see getMarker): those of this process() call, the cued hits still on their way (samples until their
    // transients land, < 0 = free), and where the chunk being rendered starts in the call.
    std::array<Marker, 32> m_markers {};
    int m_numMarkers = 0, m_chunkOffset = 0;
    std::array<double, 4> m_hitMarks { -1.0, -1.0, -1.0, -1.0 };
    bool m_hasHitTracks = false;
    void mark(int kind, int offsetInChunk)
    {
        if (m_numMarkers < static_cast<int>(m_markers.size()))
            m_markers[static_cast<size_t>(m_numMarkers++)] = { m_chunkOffset + offsetInChunk, kind };
    }

    int m_lastHitTiming = -1;            // 0 gate, 1 sync beat, 2 aimed at pass peaks
    double m_aimClock = 0.0;             // samples rendered (for telling aimed peaks apart)
    double m_lastAimTarget = -1.0e12;    // when the last aimed peak lands (m_aimClock samples)
    sj::Svf m_hitsTone;

    // Mix + space
    std::array<std::array<float, c_chunk>, room::c_maxChannels> m_mix {}; // 0 and 1 are L and R at every width
    sj::DelayLine m_echoL, m_echoR;
    sj::OnePole m_echoLpL, m_echoLpR, m_echoHpL, m_echoHpR;
    SpaceReverb m_reverb;
    RoomReverb m_roomReverb;       // a room's (see the class comment)
    std::array<float, room::c_maxChannels> m_spaceLean {}; // each channel's power, smoothed: where the room's sound has been
    bool m_roomTapsSnap = true;    // the room reverb's taps have nothing to glide from
    float m_echoSend = 1.0f;       // the echoes' send: faded in when Space comes on, so they never start mid-wave
    // The LFE (see renderLfe).
    lfe::LowPass m_lfeLow;
    std::array<lfe::PhaseMatch, room::c_maxChannels> m_lfePhase;
    float m_lfeDiscrete = 0.0f, m_lfeMixdown = 0.0f; // each source's gain into the LFE, glided
    bool m_lfeSnap = true;
    double m_lfeMatch = 0.0;         // how much of the mains' phase match is in (0 a plain wire .. 1 the match)
    bool m_lfeMatchSnap = true;      // the next chunk starts with the match where it belongs, without the glide
    std::atomic<bool> m_lfeCarries { false };
    int m_lfeChannel = -1;           // the channel the LFE is fed into (kept while leaving for a width without one)
    std::array<float, c_chunk> m_stem {}; // Stowaway's Sub as L and R each carry it, this chunk (Discrete's source)
    float m_echoDriftL = 0.0f, m_echoDriftR = 0.5f; // phases (-1..1) of the slow drift of the two echo times
    float m_spaceDry = 1.0f, m_spaceWet = 0.0f;     // the reverb's dry and wet gains at the end of the last chunk
    bool m_spaceOn = false; // the echoes and reverb hold sound (cleared once when Space reaches 0)
    std::array<sj::DcBlock, room::c_maxChannels> m_dc;

    // The room (see the class comment).
    room::Width m_width = room::Width::Stereo, m_widthWanted = room::Width::Stereo;
    bool m_room = false;           // the width in use is a room (Quad or wider)
    float m_widthFade = 1.0f;      // the output's gain around a change between stereo and a room
    // The channels the room's mix holds: the width's, and for a moment after a change to a narrower room the
    // old width's too, while the speakers that have gone fade out (m_liveHold: samples left of that).
    int m_liveChannels = 2, m_liveHold = 0;
    float m_spreadNow = 0.0f, m_heightNow = 1.0f, m_centreNow = 0.0f; // the room's settings, glided
    float m_variesNow = 0.5f;      // Height Varies, glided
    bool m_roomSnap = true;        // nothing to glide from: the next chunk's gains start where they end
    // The hits (see the class comment): where they are and where they are going, the pass they go with, a Random
    // hit's draws, Hit Spread glided, the mid and side powers of their sound (slow means), and their two lanes'
    // speaker gains at the end of the last chunk.
    room::Position m_hitsAt, m_hitsTo;
    float m_hitsDistance = 1.0f, m_hitsDistanceTo = 1.0f;
    struct HitFollow
    {
        enum Kind { None = 0, Pass, Throw };
        int kind = None;
        int bus = 0, index = 0; // Pass: the bus and which of its two passes. Throw: the throw voice
        float age = 0.0f;       // how far through its pass it was last chunk (it has gone when this falls)
    };
    HitFollow m_hitFollow;
    bool m_hitStarted = false;     // a hit started this chunk (a cue, or the gate opening)
    bool m_hitWantsLoudest = false; // it goes with the loudest pass in flight (not aimed at one)
    sj::Random m_hitRng;
    float m_hitTurn = 0.0f, m_hitHigh = 0.0f; // a Random hit: how far round the room (0..1), how high (0..1)
    float m_hitSpreadNow = c_hitSpreadDefault;
    float m_hitMidPower = 0.0f, m_hitSidePower = 0.0f;
    float m_hitMidShare = 1.0f;    // the mid's share of the two (kept through the quiet; 1: a mono sound)
    float m_hitsFileMid = 0.0f, m_hitsFileAll = 0.0f; // this chunk's hit tracks: their files' mid share, by how loud each plays
    bool m_hitOnset = false;       // this chunk is a hit starting out of the quiet
    int m_hitSince = 0;            // samples the means are over since a hit started
    float m_hitLevel = 1.0f;       // the lanes' level at the end of the last chunk (see mixHits)
    room::Gains m_hitsGain {}, m_hitsSideGain {}, m_hitsGainTo {}, m_hitsSideGainTo {};
    bool m_hitsPlaced = false;     // the last chunk's gains are known (false: nothing to glide from)
    std::array<std::atomic<float>, 6> m_hitView {};
    // The sides of a stereo bed: plus on the left speakers, minus on the right (see mixRoom).
    room::Gains m_sidesGain {};
    std::array<float, c_chunk> m_roomMono {}, m_spaceL {}, m_spaceR {}; // the mix as one stream; Space's send
    float m_spaceDryFrom = 1.0f;   // the reverb's dry gain at the start of the chunk renderSpace just did
    std::array<std::array<float, room::c_maxChannels>, 2> m_spaceFold {}; // Space's send: each channel's gain into its left and its right, glided
    bool m_spaceFoldSet = false;   // (false: nothing to glide from)
    std::vector<float> m_subStem;  // see getSubStem
    std::array<float, c_objects> m_objectLevel {};
    std::array<std::atomic<float>, c_objects * 4> m_objectView {}; // x, y, z, level each
    sj::Follower m_mixFollower;

    // Views
    std::array<std::atomic<float>, c_maxFlights * 4> m_viewData {};
    std::array<std::atomic<float>, c_tracks> m_rideView {};
    std::array<std::atomic<float>, c_maxBuses> m_busView {};
    std::atomic<float> m_stowLevelView { 0.0f }, m_stowPanView { 0.0f };
    std::array<std::atomic<float>, 2 * 4> m_stowFlightView {}; // Fly's two passes: env, x, closeness, age each
    std::atomic<float> m_energyView { 0.0f }, m_gateView { 0.0f };
    std::array<std::atomic<int>, c_tracks> m_hitRegionView {};
    std::atomic<int> m_hitBeatView { 0 };
    std::atomic<double> m_hitPreView { 0.0 }, m_hitPostView { 0.0 };
    std::array<ScopeRing, c_sections> m_scopes;
    std::array<Head, 2> m_leadStart {}; // bus A's passes at the start of the chunk (linked buses)
    bool m_linkNow = false;             // the link in use: a change fades bus B out, swaps, and fades it back in
    float m_linkFade = 1.0f;

    // Per-track pitch: semitones glide; VARI on changes the playback speed, off goes through a pitch shifter.
    std::array<PitchShifter, c_tracks> m_shifters;
    std::array<float, c_tracks> m_trackPitchNow {};  // smoothed semitones
    std::array<bool, c_tracks> m_variNow {};         // the mode in use (a switch fades out, swaps, fades in)
    std::array<float, c_tracks> m_modeGain {};       // that fade, 0..1
    std::array<int, c_tracks> m_modeHold {};         // samples to stay silent after switching to the shifter
    std::array<bool, c_tracks> m_shifterUsed {};     // the shifter holds sound (flushed once when the track empties)
    std::array<std::atomic<float>, c_tracks> m_trackSpeedView {};
    float trackSpeed(int track) const;               // VARI ratio of a track right now (1 when VARI is off)
    std::array<double, c_sections> m_sectionSquares {}; // sums for takeSectionLevels()
    int m_sectionSamples = 0;
    std::array<float, c_sections> m_sectionLevels {};
    // The cued beat, shown once its transients play.
    double m_beatViewWait = -1.0;   // samples until the cued beat is heard (< 0 = nothing pending)
    std::array<int, c_tracks> m_pendingHitRegion {};
};

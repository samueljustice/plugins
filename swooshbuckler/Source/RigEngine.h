#pragma once

#include <JuceHeader.h>
#include "Cargo.h"
#include "ScopeRing.h"
#include "PitchShifter.h"
#include "DspUtils.h"
#include "SpectralMorph.h"
#include "SpaceReverb.h"

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
 *   side-chains:  A ducks B, and A's level low-passes B
 *   hit sync:     optional: every hit track fires its next sound on a shared beat, transients aligned
 *                 (the beat is as long as the longest hit, so none is cut short).
 *   hits:         a gate on A + B opens only at the loudest moments (a boost with hold
 *                 and release, not on/off); the gated hits then duck A and B.
 *                 The detector listens before the ducking, so there is no feedback loop.
 *   morph:        optional: what feeds bus A and bus B goes through a Zynaptiq-style spectral morph
 *                 (SpectralMorph); the result is handed back to the two buses in proportion to X.
 *   -> ride make-up (the mix is eased down as the ride gets shallower, so Auto Ride off or a held Swell reaches
 *      the output stage about as loud as the ridden rig) -> space (echoes, reverb, width)
 *   -> out (the processor adds inflate + limiter)
 *
 * Every continuous control is chased by a damped spring (rubber banding).
 * No allocation, locks or I/O on the audio thread.
 */
class RigEngine
{
public:
    static constexpr int c_tracks = CargoHold::c_numTracks;
    static constexpr int c_maxBuses = CargoHold::c_maxBuses;
    static constexpr int c_maxFlights = c_maxBuses * 2; // two doppler passes per bus, shown by the visualiser

    struct BusParams
    {
        float doppler = 0.5f;   // how dramatic each pass is (closeness, loudness swell, air)
        float width = 0.8f;     // how far a pass travels across the stereo field
        float bend = 0.7f;      // how much doppler pitch bend
        float low = 0.3f, swirl = 0.2f, filter = 0.3f, drive = 0.1f;
        float duck = 0.4f;  // ducked by bus A (ignored on bus A)
        float key = 0.3f;   // low-passed by bus A (ignored on bus A)
        int passMode = 0;   // 0 Bed (two overlapped passes, a steady level), 1 Fly-By (one pass at a time). Linked: bus A's.
    };

    struct Params
    {
        bool rideOn = true;
        float energy = 0.6f;   // how far the auto-ride pulls the faders
        float rate = 0.4f;     // how fast the faders move (and how long a doppler pass is)
        float fill = 0.3f;     // long delays into the gaps
        float chaos = 0.4f;    // randomness of the rides and the passes
        float pitch = 0.0f;    // semitones, every track
        // Modulator offsets for the knobs that glide on springs, in the knob's own units. They are added after the
        // spring, so a fast modulator isn't smoothed away by the glide meant for turning the knob.
        float energyMod = 0.0f, pitchMod = 0.0f, fillMod = 0.0f, spaceMod = 0.0f;
        std::array<std::array<float, 9>, c_maxBuses> busMod {}; // doppler width bend low swirl filter drive duck key
        std::array<float, c_tracks> levels { 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f }; // gain (fader law applied)
        std::array<float, c_tracks> trackPitch {}; // semitones (cents included), each track on its own
        std::array<bool, c_tracks> trackVari { true, true, true, true, true, true, true, true, true, true }; // off = keep the speed

        std::array<BusParams, c_maxBuses> buses { BusParams { 0.55f, 0.7f, 0.7f, 0.35f, 0.3f, 0.3f, 0.2f, 0.0f, 0.0f },
                                                  BusParams { 0.4f, 0.7f, 0.6f, 0.3f, 0.3f, 0.4f, 0.0f, 0.35f, 0.3f } };

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
        // Peak tremolo: deepens as the level nears its recent peak.
        bool tremOn = false;         // off: the tremolo fades out
        float tremDepth = 0.45f;     // 0..1 (knob; Params::tremDepthAmount gives the dip)
        float tremRate = 0.5f;       // 0..1 -> 1.5..30 Hz
        float tremThreshold = 0.6f;  // 0..1: how close to the peak it starts
        int tremShape = 0;           // 0 Sine, 1 Square, 2 Flutter
        int tremTarget = 0;          // 0 everything, 1 buses A + B, 2 hits

        float space = 0.3f;
        bool swell = false;    // held: every fader rides up

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
    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples);
    float getLatencySamples() const;

    // UI (any thread)
    FlightView getFlightView(int index) const;
    float getEnergy() const { return m_energyView.load(std::memory_order_relaxed); }
    float getRide(int track) const { return m_rideView[static_cast<size_t>(track)].load(std::memory_order_relaxed); }
    float getHitGate() const { return m_gateView.load(std::memory_order_relaxed); }
    /** Hit sync, for the track view: which region (CargoPlayer::regionAt index) each synced hit track fired last
        (-1 = none / not synced), and a counter that goes up on every shared beat. */
    /** Published when the beat's transients are heard (not when it is cued, up to 1 s earlier). */
    int getHitRegion(int track) const { return m_hitRegionView[static_cast<size_t>(track)].load(std::memory_order_relaxed); }
    int getHitBeats() const { return m_hitBeatView.load(std::memory_order_relaxed); }
    /** The beat window the engine used last, in SOURCE seconds (pitch and caps applied), 0 = no beat yet. */
    double getHitPre() const { return m_hitPreView.load(std::memory_order_relaxed); }
    double getHitPost() const { return m_hitPostView.load(std::memory_order_relaxed); }
    /** Little waveforms for the section scopes: 0 = bus A, 1 = bus B, 2 = the hits (what each adds to the mix). */
    const ScopeRing& getScope(int which) const { return m_scopes[static_cast<size_t>(std::clamp(which, 0, 2))]; }
    float getBusLevel(int bus) const { return m_busView[static_cast<size_t>(bus)].load(std::memory_order_relaxed); }
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

    /** Audio thread: RMS of bus A, bus B and the hits (what each adds to the mix) since the last call, for the
        Follower modulators. */
    std::array<float, 3> takeSectionLevels();

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
        sj::OnePole airL, airR;
        float view = 0.0f, x = 0.0f, age = 0.0f;

        /** How far through the pass (0..1, 0.5 = closest). */
        float progress() const
        {
            const double u = time < static_cast<double>(lenIn) ? 0.5 * time / static_cast<double>(lenIn)
                                                               : 0.5 + 0.5 * (time - static_cast<double>(lenIn)) / static_cast<double>(lenOut);
            return static_cast<float>(u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u));
        }
    };

    struct BusState
    {
        sj::DelayLine line;
        std::array<Head, 2> heads;
        // low end
        sj::Svf lowA, lowB, lowHp, subA, subB, harm, shelfL, shelfR;
        sj::Follower lowFollower;
        float flip = 1.0f;
        bool subArmed = false; // the sub octave's trigger: armed below the low band's level, fires above it
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
            float lp[2] {};
            sj::Svf band[2];                  // Mids: the 250 Hz .. 4 kHz band (centred on 1 kHz)
        };
        Duck byBus, byHits;
        std::array<float, c_chunk> l {}, r {}, in {};
    };

    /** lenIn: how long the approach is, or < 0 to draw a whole new pass length (the first head of a bus). */
    void startPass(Head& h, const BusParams& p, float lenIn = -1.0f);
    /** A pass's length before Chaos varies it: follows Rate (6 s .. 1.6 s). */
    float nominalPassSeconds() const;
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
    void renderSpace(int frames);
    void publishViews();

    double m_sampleRate = 48000.0;
    float m_fs = 48000.0f;
    Params m_params;
    sj::Random m_rng;
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
    bool m_restartPending = false;                  // restart() asked: done once the sources are silent
    void restartNow();
    float m_releaseGain = 1.0f;                     // 1 = the sources play, 0 = released
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
    float m_morphMixNow = 0.0f; // how far the morph is in (B isn't ducked or filtered by A while morphing)
    void glideMods(int frames);
    std::array<float, c_chunk> m_trackScratch {};
    MorphInsert m_morph;
    sj::Spring m_morphMix;

    // Buses
    std::array<BusState, c_maxBuses> m_buses;
    std::array<float, c_chunk> m_hitsIn {}, m_hitsL {}, m_hitsR {};
    sj::Follower m_detect, m_hitsFollower;
    float m_slowEnv = 0.0f;
    int m_talker = 0;          // take turns: 0 = A speaks, 1 = B speaks
    float m_talkerHold = 0.0f; // seconds the other bus has been clearly louder  // Jumps gate: the slow envelope the fast one has to jump above

    float duckReturnCoef() const;
    /** full: the amount at which Darken reaches its darkest (the deepest the caller ever asks for). */
    void duckSample(BusState::Duck& d, float target, float returnCoef, float full, float& l, float& r);
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
    void applyTremolo(PeakTremolo& t, float* l, float* r, int frames, bool selected);

    int m_lastHitTiming = -1;            // 0 gate, 1 sync beat, 2 aimed at pass peaks
    double m_aimClock = 0.0;             // samples rendered (for telling aimed peaks apart)
    double m_lastAimTarget = -1.0e12;    // when the last aimed peak lands (m_aimClock samples)
    sj::Svf m_hitsTone;

    // Mix + space
    std::array<float, c_chunk> m_mixL {}, m_mixR {};
    sj::DelayLine m_echoL, m_echoR;
    sj::OnePole m_echoLpL, m_echoLpR, m_echoHpL, m_echoHpR;
    SpaceReverb m_reverb;
    float m_echoDriftL = 0.0f, m_echoDriftR = 0.5f; // phases (-1..1) of the slow drift of the two echo times
    float m_spaceDry = 1.0f, m_spaceWet = 0.0f;     // the reverb's dry and wet gains at the end of the last chunk
    bool m_spaceOn = false; // the echoes and reverb hold sound (cleared once when Space reaches 0)
    sj::DcBlock m_dcL, m_dcR;
    sj::Follower m_mixFollower;

    // Views
    std::array<std::atomic<float>, c_maxFlights * 4> m_viewData {};
    std::array<std::atomic<float>, c_tracks> m_rideView {};
    std::array<std::atomic<float>, c_maxBuses> m_busView {};
    std::atomic<float> m_energyView { 0.0f }, m_gateView { 0.0f };
    std::array<std::atomic<int>, c_tracks> m_hitRegionView {};
    std::atomic<int> m_hitBeatView { 0 };
    std::atomic<double> m_hitPreView { 0.0 }, m_hitPostView { 0.0 };
    std::array<ScopeRing, 3> m_scopes;
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
    std::array<double, 3> m_sectionSquares {}; // sums for takeSectionLevels()
    int m_sectionSamples = 0;
    std::array<float, 3> m_sectionLevels {};
    // The cued beat, shown once its transients play.
    double m_beatViewWait = -1.0;   // samples until the cued beat is heard (< 0 = nothing pending)
    std::array<int, c_tracks> m_pendingHitRegion {};
};

#pragma once

#include <JuceHeader.h>
#include "Cargo.h"
#include "ScopeRing.h"
#include "DspUtils.h"
#include "SpectralMorph.h"

#include <array>
#include <atomic>

/**
 * The whoosh rig (layered looping tracks with ridden faders),
 * as one engine that runs continuously while the recorder rolls.
 *
 *   tracks (always playing, looped, silence skipped)
 *     -> ride: each track's fader is LFO'd at its own offset speed plus a random
 *        "hand on the fader" (Mondo-Mod style), scaled by Energy / Rate
 *     -> fill: long delays (0.5 / 1 / 2 s) fill the gaps of unsteady tracks
 *     -> bus A (aggressive) | bus B (smooth) | hits      (routed automatically by detected type)
 *
 *   each bus:     doppler (two overlapping passes with randomly automated speed,
 *                 distance and direction) -> low end (sub octave, overtones, lift at
 *                 the loudest point) -> swirl (phaser + flanger) -> envelope filter -> drive
 *   side-chains:  A ducks B, and A's level low-passes B
 *   hit sync:     optional: every hit track fires its next sound on a shared beat, transients aligned
 *                 (the beat is as long as the longest hit, so none is cut short).
 *   hits:         a gate on A + B opens only at the loudest moments (a boost with hold
 *                 and release, not on/off); the gated hits then duck A and B.
 *                 The detector listens before the ducking, so there is no feedback loop.
 *   morph:        optional: what feeds bus A and bus B goes through a Zynaptiq-style spectral morph
 *                 (SpectralMorph); the result is handed back to the two buses in proportion to X.
 *   -> space (echoes, reverb, width) -> out (the processor adds inflate + limiter)
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
    };

    struct Params
    {
        bool rideOn = true;
        float energy = 0.6f;   // how far the auto-ride pulls the faders
        float rate = 0.4f;     // how fast the faders move (and how long a doppler pass is)
        float fill = 0.3f;     // long delays into the gaps
        float chaos = 0.4f;    // randomness of the rides and the passes
        float pitch = 0.0f;    // semitones, every track
        float input = 0.0f;    // the audio input, mixed into bus A
        std::array<float, c_tracks> levels { 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f };

        std::array<BusParams, c_maxBuses> buses { BusParams { 0.55f, 0.8f, 0.7f, 0.35f, 0.2f, 0.3f, 0.15f, 0.0f, 0.0f },
                                                  BusParams { 0.4f, 0.8f, 0.6f, 0.2f, 0.3f, 0.4f, 0.0f, 0.4f, 0.3f } };

        float hitSense = 0.5f; // how easily the hit gate opens
        float hitBoost = 12.0f;// dB when open
        float hitRelease = 0.35f; // 0..1 -> 60..1200 ms
        float hitDuck = 0.5f;  // hits duck A and B
        bool hitSync = false;  // hit tracks fire together, their transients lined up
        // Gate and ducking options (the defaults are the original behaviour).
        int gateMode = 0;        // 0 Peaks (loudest moments, original) 1 Level 2 Jumps (sudden rises) 3 Open (no gate)
        float hitHold = 0.12f;   // 0..1 -> 0..500 ms the gate stays open (0.12 = 60 ms)
        float hitFloor = 0.0f;   // 0 = closes fully, else -40..-3 dB: hits never vanish completely
        int duckMode = 0;        // 0 Volume 1 Darken (low-pass the ducked bus) 2 Mids (duck only 250 Hz..4 kHz)
        float duckReturn = 0.0f; // 0 = fast (original) .. 1 = ducked buses take 1 s to come back
        bool takeTurns = false;  // A and B: only the louder one speaks, the other ducks under it

        float space = 0.25f;
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
    void setParams(const Params& p);
    /** Start every track again from its start marker (audio thread; the processor calls it on Play). */
    void restart();
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

private:
    static constexpr int c_chunk = 32;

    struct Head
    {
        double time = 0.0;
        float length = 3.0f, mach = 0.2f, closest = 0.02f, dir = 1.0f, closeness = 0.5f;
        sj::OnePole airL, airR;
        float view = 0.0f, x = 0.0f;
    };

    struct BusState
    {
        sj::DelayLine line;
        std::array<Head, 2> heads;
        // low end
        sj::Svf lowA, lowB, subA, subB, harm, shelfL, shelfR;
        sj::Follower lowFollower;
        float flip = 1.0f, lastLow = 0.0f;
        // swirl
        std::array<sj::Allpass1, 6> phL, phR;
        float phFbL = 0, phFbR = 0, lfo = 0;
        sj::DelayLine flL, flR;
        float flFbL = 0, flFbR = 0;
        // filter
        sj::Svf fL, fR;
        sj::Follower follower;
        // drive tone
        sj::OnePole toneL, toneR;
        // level tracking
        float env = 0.0f, peakRef = 1.0e-3f;
        std::array<sj::Spring, 9> springs; // doppler width bend low swirl filter drive duck key
        float duck = 0.0f;
        // Ducking state (one for being ducked by the other bus, one for being ducked by the hits).
        struct Duck
        {
            float amount = 0.0f;              // applied attenuation 0..1 (after the return glide)
            float lp[2] {}, lo[2] {}, mid[2] {};
        };
        Duck byBus, byHits;
        std::array<float, c_chunk> l {}, r {}, in {};
    };

    void startPass(Head& h, const BusParams& p);
    void renderTracks(const float* inL, const float* inR, int offset, int frames);
    void renderBus(BusState& bus, int frames, float keyEnv, bool isKeyBus);
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
    std::array<sj::OnePole, c_tracks> m_fillTone;
    std::array<float, c_tracks> m_fillAmount {}, m_coverage {};
    std::array<const CargoData*, c_tracks> m_coverageFor {};
    std::array<double, c_tracks> m_ridePhase {};
    std::array<sj::Drift, c_tracks> m_rideDrift;
    std::array<float, c_tracks> m_gain {};          // gain applied at the end of the previous chunk
    std::array<sj::Spring, c_tracks> m_levelSprings;
    sj::Spring m_swellSpring, m_energySpring, m_pitchSpring, m_fillSpring, m_spaceSpring, m_inputSpring;
    std::array<float, c_chunk> m_trackScratch {};
    SpectralMorph m_morph;
    sj::Spring m_morphMix;
    std::array<float, c_chunk> m_morphA {}, m_morphB {}, m_morphOut {};

    // Buses
    std::array<BusState, c_maxBuses> m_buses;
    std::array<float, c_chunk> m_hitsIn {}, m_hitsL {}, m_hitsR {};
    sj::Follower m_detect, m_hitsFollower;
    float m_slowEnv = 0.0f;
    int m_talker = 0;          // take turns: 0 = A speaks, 1 = B speaks
    float m_talkerHold = 0.0f; // seconds the other bus has been clearly louder  // Jumps gate: the slow envelope the fast one has to jump above
    float m_loCoef = 0.0f, m_midCoef = 0.0f; // Mids duck band edges (one-pole coefficients)
    float duckReturnCoef() const;
    void duckSample(BusState::Duck& d, float target, float returnCoef, float& l, float& r);
    float m_peakRef = 1.0e-3f, m_average = 0.0f, m_gate = 0.0f, m_hold = 0.0f;
    double m_hitClock = 0.0;     // samples to the next synced hit
    bool m_lastHitSync = false;
    sj::Svf m_hitsTone;

    // Mix + space
    std::array<float, c_chunk> m_mixL {}, m_mixR {};
    sj::DelayLine m_echoL, m_echoR;
    sj::OnePole m_echoLpL, m_echoLpR, m_echoHpL, m_echoHpR;
    juce::Reverb m_reverb;
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
    // The cued beat, shown once its transients play.
    double m_beatViewWait = -1.0;   // samples until the cued beat is heard (< 0 = nothing pending)
    std::array<int, c_tracks> m_pendingHitRegion {};
};

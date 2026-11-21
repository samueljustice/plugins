#pragma once

#include "DspUtils.h"

#include <array>
#include <cstdint>
#include <vector>

/**
 * SWIRL'S TAPE FLANGER AND DOUBLER (a stub: it passes the sound untouched; the DSP is filled in by its own work
 * package). Two of Swirl's five types, one bus at a time, one SwirlFx per bus (RigEngine::BusState::swirlFx).
 * JUCE-free, built on DspUtils.
 *
 * What the filler must provide (design 3.4 and 3.5):
 *   Tape Flanger, per lane: a clean ring of c_dryMs (whole samples) for the dry; the wet read from the line at
 *     D0 + d (cubic), low-passed at 15 kHz (two one-poles at 19 kHz), soft-clipped, inverted when Invert is on;
 *     feedback read at max(|d|, 0.25 ms) through the same low-pass, gain 0.9 x Feedback, never running away;
 *     out = cos(m pi/2) dry + sin(m pi/2) wet with m = mixFor(knob). d = settle(motion + hand), eased into end stops
 *     at +-c_maxOffsetMs. With the Pass: motion = Depth (1 - 1.3 passNear); On Its Own: a warped sine at Rate with
 *     its depth held under 8 ms Hz / rate; By Hand: 0. Varispeed adds `hand` in every Motion: speed =
 *     0.03 sgn(v) ((|v| - 0.05) / 0.95)^1.5 past a 5% dead zone, a 0.1 s one-pole, hand -= speed dt per sample,
 *     eased to a stop over the last 10% of the range; the hand is held when v returns to 0 (getOffsetMs).
 *     Wobble: two 15 ms one-poles plus Wobble x 0.35 x a 0.6 Hz resonance (damping 0.1). Second lane
 *     d2 = d (1 - 0.15 d / 6 ms), third lane the same with +0.15.
 *   Doubler, per lane: no dry delay; d = (1 - Wander) Delay + Wander (3 ms + P u(t)), P = min(70 ms, 50 ms Hz / rate),
 *     u in 0..1 from the Wave (Random: noise through two one-poles at 0.4 x rate, from its own sj::Random);
 *     Thicken on: feedback -0.65 (1 - d / 45 ms)^1.2 (0 above 45 ms); off: 0.68 x Feedback. Spread: lane 2 at
 *     1.3 x Delay with u inverted (independent for Random), lane 3 at 1.15 x with its own. Mix as above.
 *   One 110 ms line per lane shared by both types, and the 8 ms ring, all made in prepare(): nothing allocates
 *   after it, on a change of type or anywhere on the audio thread. Delays, gains and mixes ramp per sample across
 *   each call from where the last one ended. No latency is reported to anyone.
 *
 * What the callers rely on (RigEngine::renderBus, owned by the engine's work package):
 *   - prepare(sampleRate) from RigEngine::prepare; reset() from RigEngine::reset and on every seeded restart (the
 *     held Varispeed offset goes back to the middle on a transport start); setSeed(m_seed ^ const) there too, and
 *     scatter() from RigEngine::scatterRide (each rendered file starts its sweeps somewhere else).
 *   - processTape() / processDoubler() once per chunk (32 samples at most, c_maxFrames) on the bus's lanes, in
 *     place, while that type is the one in use; the engine fades the bus out and in over 30 ms around a change of
 *     type, and calls reset() while the type is neither. `knob` is the front Swirl wheel with its modulation
 *     (0..1) at the end of the chunk; the mix ramps to it from where the last call left it (the first call after
 *     reset() snaps). A lane pointer that is null is skipped.
 *   - Lanes::passNearFrom / passNearTo: the engine's, for With the Pass (see Lanes).
 *   - delaySides(): the stereo bed's side signal, delayed by the same dry ring while Tape Flanger is on (it rejoins
 *     after the colours). Called every chunk the type is Tape Flanger, after processTape().
 *   - dryDelaySamples(): how long the dry ring is (c_dryMs in whole samples at this rate; the same from prepare()
 *     on). It is the engine that knows the type: it counts this only while the bus's Swirl is a Tape Flanger (Aim
 *     adds it to that bus's time to its peak, Faders Together to its lead).
 *   - getOffsetMs(): the held Varispeed offset (-c_maxOffsetMs .. +c_maxOffsetMs), for the offset meter (the engine
 *     publishes it / c_maxOffsetMs as RigEngine::getVarispeedOffset).
 *   - recentre(): the meter was double-clicked: the offset slides home (audio thread: the engine calls it while
 *     Params::varispeedHome is set, which may be for several chunks running; calling it again changes nothing).
 */
class SwirlFx
{
public:
    static constexpr int c_lanes = 3;
    static constexpr int c_maxFrames = 32;
    static constexpr double c_lineSeconds = 0.110, c_dryMs = 8.0;
    static constexpr float c_maxOffsetMs = 6.0f;

    enum Motion { WithPass = 0, OnItsOwn, ByHand };
    enum Wave { WaveRandom = 0, WaveSine, WaveTriangle };

    /** Tape Flanger's settings (ParamIDs::busDetail TapeMotion .. Varispeed, in plain units). */
    struct Tape
    {
        int motion = WithPass;
        bool invert = true;
        float depthMs = 4.0f;   // 0 .. 6
        float rateHz = 0.3f;    // 0.01 .. 4
        float feedback = 0.0f;  // -1 .. 1
        float wobble = 0.25f;   // 0 .. 1
        float varispeed = 0.0f; // -1 .. 1, with its modulation
    };
    /** The Doubler's settings (ParamIDs::busDetail DoublerWave .. DoublerFeedback, in plain units). */
    struct Doubler
    {
        int wave = WaveRandom;
        bool thicken = true, spread = true;
        float delayMs = 20.0f;  // 3 .. 75
        float wander = 0.12f;   // 0 .. 1
        float rateHz = 0.6f;    // 0.01 .. 6
        float feedback = 0.0f;  // -1 .. 1 (the loop gain is 0.68 x this)
    };
    /** The lanes of one chunk: left and right (stereo) or a room's first pass, second pass and throws. passNear: for
        Tape Flanger With the Pass, each lane's nearness to its pass against its value at the closest point of that
        pass (0 far .. 1 at the closest), at the start and the end of the chunk. */
    struct Lanes
    {
        std::array<float*, c_lanes> lane {};
        int count = 2;
        std::array<float, c_lanes> passNearFrom {}, passNearTo {};
    };

    /** The wet's share for the front Swirl wheel (0..1): m = 0.5 knob^0.7, equal power. */
    static float mixFor(float knob);

    void prepare(double sampleRate);
    void reset();
    void setSeed(std::uint32_t seed) { m_rng.seed(seed); }
    /** Where its own sweeps start (0..1 of a cycle: Tape Flanger On Its Own, the Doubler's Sine and Triangle), for
        renders. After reset(). */
    void scatter(float phase) { m_startPhase = sj::clamp(phase, 0.0f, 1.0f); }

    void processTape(const Tape& settings, float knob, const Lanes& lanes, int frames);
    void processDoubler(const Doubler& settings, float knob, const Lanes& lanes, int frames);
    /** Tape Flanger's dry delay on a stereo bed's side signal (left and right), in place. */
    void delaySides(float* left, float* right, int frames);

    int dryDelaySamples() const { return m_drySamples; }
    float getOffsetMs() const { return m_hand; }
    void recentre() { m_recentre = true; }

private:
    double m_sampleRate = 48000.0;
    int m_drySamples = 384;  // c_dryMs at m_sampleRate
    float m_startPhase = 0.0f;
    sj::Random m_rng;
    float m_hand = 0.0f;     // the held Varispeed offset, ms
    bool m_recentre = false;
};

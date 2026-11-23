#pragma once

#include "DspUtils.h"

#include <array>
#include <cstdint>
#include <vector>

/**
 * SWIRL'S TAPE FLANGER AND DOUBLER. Two of Swirl's five types, one bus at a time, one SwirlFx per bus
 * (RigEngine::BusState::swirlFx). JUCE-free, built on DspUtils.
 *
 * Tape Flanger, per lane (design 3.4). A second copy of the lane on a moving delay that can run ahead of the first:
 *
 *     x -+-> clean ring, D0 = c_dryMs in whole samples -------------------------------> dry
 *        +->(+)-> line -+-> read at D0 + d -> +-1 ------------------------------------> wet
 *            ^          +-> cubic read at max(|d|, 0.25 ms) -> low-pass -> soft clip -> x g -+
 *            +-----------------------------------------------------------------------------+
 *     out = cos(m pi/2) dry + sin(m pi/2) wet,   m = mixFor(knob)
 *
 *   The copy is read as it is on the line: nothing is between it and the dry but the move, so at d = 0 (a whole
 *   number of samples) it is the dry sample for sample and, inverted, cancels it alike at every frequency, level
 *   and sample rate (with no feedback; the loop adds to the line what the dry does not have). The low-pass (two
 *   one-poles at 19 kHz) and the soft clip (2 tanh(x / 2)) are the loop's: g = 0.9 x Feedback is under 1 and the
 *   clip is inside the loop, so it never runs away. With no feedback the second read is not made.
 *   The reads of the moving copy (this one and the Doubler's) are the four-point cubic at 120 kHz and over and the
 *   line's 16-point sinc under it (c_sincBelowHz), where the cubic's loss at the top would be heard to change as
 *   the copy moves.
 *   d = stops(settle(motion + hand)), in ms, positive = the copy later than the dry.
 *     With the Pass: motion = Depth (1 - c_passLead passNear), and passNear is the engine's: passNearFor() of how
 *       near the pass is and how long it is until its closest point. Late when the pass is far ((1 - c_passHold)
 *       of the nearness, so the copy is never level with the dry out there), and through zero inside
 *       c_passWindowSeconds either side of the closest point whatever the pass's length and however near it comes:
 *       (c_passLead - 1) Depth early at the closest point itself. The engine asks for the pass as it will be
 *       c_settleLagSeconds on less the clean path's length, so that what settle makes of it sits on the middle of
 *       the whoosh as it is heard (measured in SwirlWireTest, "centre").
 *     With the Filter: motion = Depth (1 - c_filterLead filterOpen): the same, by how far up its present travel the
 *       bus's filter is (SweepFilter::travel, the engine's, with the filter's own rhythm read the settle's time
 *       ahead): late while the filter is closed, through zero as it nears its most open, (c_filterLead - 1) Depth
 *       early there. A filter that stands still, or is not in at all, leaves the copy Depth late.
 *     On Its Own: motion = min(Depth, 8 ms Hz / Rate) sin(2 pi w(phase)), w(p) = p - c_ownWarp sin^2(pi p): the late
 *       half takes 60% of the cycle and the turn on the early side is the quick one. The cap holds the pitch swing
 *       near +-6% (measured in SwirlFxTest).
 *     By Hand: motion = Depth: the copy waits that far behind, and the hand brings it through the dry.
 *     hand (Varispeed, in every Motion): speed = 0.03 sgn(v) ((|v| - 0.05) / 0.95)^1.5 past a 5% dead zone,
 *       followed over 0.1 s; hand -= speed dt, so the copy is pitched by exactly `speed` (51 cents at the ends)
 *       while it moves; the speed eases to nothing over the last 10% of the range; with v back at 0 the hand stays
 *       where it is (getOffsetMs), and recentre() slides it home (at the full lean's speed at most).
 *     settle (Wobble): two 15 ms one-poles, plus Wobble x 0.35 x the ring of a 0.6 Hz resonance (damping 0.1)
 *       that follows them. The ring is held under c_ringMaxMs (softly), so a sweep or a row of passes near 0.6 Hz
 *       cannot build it up to several times the Depth: a single move still rings by up to a third of its size.
 *     stops: unchanged up to 5.6 ms, eased into +-c_maxOffsetMs beyond.
 *     Second lane: d2 = d (1 - 0.15 d / 6 ms), third lane d3 = d (1 + 0.15 d / 6 ms): the same zero, a different
 *       sweep either side (so they reach 6.9 ms on one side).
 *
 * Doubler, per lane (design 3.5). One delayed, wandering copy; the dry is not delayed:
 *
 *     x -> dry;   x ->(+)-> line -> cubic read at d -> low-pass -> wet;   the wet x g back into the line
 *
 *   d = smooth((1 - Wander) Delay k + Wander (3 ms + P u)), P = min(70 ms, 50 ms Hz / Rate), u in 0..1 from the
 *   Wave: Sine and Triangle start at 0 and top out half way through the cycle; Random is noise through two
 *   one-poles at 0.4 x Rate, from its own sj::Random, folded into 0..1 by a tanh (its spread is a quarter of the
 *   range). smooth is two 5 ms one-poles (a turned wheel glides, a Triangle's corner does not click).
 *   Spread: k = 1.3 and u upside down in the second lane, k = 1.15 and u a quarter cycle on in the third (in
 *   Random each lane has its own noise); off, every lane is the first.
 *   Thicken on: g = -0.65 (1 - d / 45 ms)^1.2, 0 above 45 ms, following the moving delay. Off: g = 0.68 x Feedback.
 *
 * One 110 ms line per lane shared by both types, the 8 ms ring per lane and two more for the stereo bed's sides,
 * all made in prepare(): nothing allocates or locks after it. The motions are worked out once per call and every
 * delay, gain and mix ramps sample by sample across the call from where the last one ended. No latency is
 * reported to anyone.
 *
 * What the callers rely on (RigEngine::renderBus, owned by the engine's work package):
 *   - prepare(sampleRate) from RigEngine::prepare; reset() from RigEngine::reset and on every seeded restart (the
 *     held Varispeed offset goes back to the middle on a transport start); setSeed(m_seed ^ const) there too, and
 *     scatter() from RigEngine::scatterRide (each rendered file starts its sweeps somewhere else).
 *   - processTape() / processDoubler() once per chunk (32 samples at most, c_maxFrames) on the bus's lanes, in
 *     place, while that type is the one in use; the engine fades the bus out and in over 30 ms around a change of
 *     type, and calls reset() while the type is neither. reset() costs nothing when nothing has been processed
 *     since the last one, so it may be called every chunk. Going straight from one type to the other clears the
 *     lines and snaps (the held Varispeed offset is kept). `knob` is the front Swirl wheel with its modulation
 *     (0..1) at the end of the chunk; the mix ramps to it from where the last call left it (the first call after
 *     reset() snaps). A lane pointer that is null is skipped: its motion still runs and
 *     silence goes into its line, so a lane that comes back (a throw) starts clean.
 *   - Lanes::passNearFrom / passNearTo: the engine's, for With the Pass (see Lanes). Only passNearTo is read: the
 *     motion goes from where the last call ended to it.
 *   - Lanes::filterOpen: the engine's, for With the Filter (see Lanes): 0 while the bus's Filter wheel is at 0 or
 *     nothing moves the filter.
 *   - delaySides(): the stereo bed's side signal, delayed by the same dry ring while Tape Flanger is on (it rejoins
 *     after the colours). Called every chunk the type is Tape Flanger, after processTape().
 *   - dryDelaySamples(): how long the dry ring is (c_dryMs in whole samples at this rate; the same from prepare()
 *     on). It is the engine that knows the type: it counts this only while the bus's Swirl is a Tape Flanger (Aim
 *     adds it to that bus's time to its peak, Faders Together to its lead).
 *   - getOffsetMs(): the held Varispeed offset (-c_maxOffsetMs .. +c_maxOffsetMs), for the offset meter (the engine
 *     publishes it / c_maxOffsetMs as RigEngine::getVarispeedOffset).
 *   - recentre(): the meter was double-clicked: the offset slides home (audio thread: the engine calls it while
 *     Params::varispeedHome is set, which may be for several chunks running; calling it again changes nothing).
 *     Varispeed is not read while it slides.
 */
class SwirlFx
{
public:
    static constexpr int c_lanes = 3;
    static constexpr int c_maxFrames = 32;
    static constexpr double c_lineSeconds = 0.110, c_dryMs = 8.0;
    static constexpr float c_maxOffsetMs = 6.0f;

    // Tape Flanger, With the Pass: how far past zero the copy is at the pass's closest point (1.15 .. 1.5; see the
    // class comment and the measurements in SwirlFxTest).
    static constexpr double c_passLead = 1.3;
    // With the Pass: the time either side of the pass's closest point in which the copy is brought through zero,
    // and how much of the copy's lateness is left with the pass at its nearest outside that time.
    static constexpr double c_passWindowSeconds = 0.08, c_passHold = 0.2;
    // With the Filter: the same lead for the filter's place in its travel (zero at 1 / c_filterLead of the way up).
    static constexpr double c_filterLead = 1.15;
    // On Its Own: the warp that gives the late side 60% of the cycle (0.1 / sin^2(0.6 pi)).
    static constexpr double c_ownWarp = 0.11056;
    static constexpr double c_ownSwingMsHz = 8.0;
    // Varispeed: the speed at the end of the wheel, its dead zone, how long the speed takes to follow, and the
    // share of the range it eases to a stop over.
    static constexpr double c_handSpeed = 0.03, c_handDead = 0.05, c_handSeconds = 0.1, c_handEase = 0.1;
    // Wobble.
    static constexpr double c_settleSeconds = 0.015, c_ringHz = 0.6, c_ringDamping = 0.1, c_ringShare = 0.35, c_ringMaxMs = 4.0;
    // Where the end stops start to ease (ms short of c_maxOffsetMs), and the lanes' bend.
    static constexpr double c_stopKneeMs = 0.4, c_laneBend = 0.15;
    static constexpr double c_tapeFeedback = 0.9, c_feedbackFloorMs = 0.25, c_lowPassHz = 19000.0;
    // Under this sample rate the moving copy is read with the sinc.
    static constexpr double c_sincBelowHz = 120000.0;
    // Doubler.
    static constexpr double c_wanderFloorMs = 3.0, c_wanderMaxMs = 70.0, c_wanderMsHz = 50.0, c_randomRate = 0.4, c_randomFold = 0.68;
    static constexpr double c_thickenGain = 0.65, c_thickenEndMs = 45.0, c_thickenPower = 1.2, c_doublerFeedback = 0.68;
    static constexpr double c_spreadSecond = 1.3, c_spreadThird = 1.15, c_doublerSmoothSeconds = 0.005;

    enum Motion { WithPass = 0, OnItsOwn, ByHand, WithFilter };
    enum Wave { WaveRandom = 0, WaveSine, WaveTriangle };

    /** Tape Flanger's settings (ParamIDs::busDetail TapeMotion .. Varispeed, in plain units). */
    struct Tape
    {
        int motion = WithPass;
        bool invert = false;
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
        Tape Flanger With the Pass, how far each lane's pass has brought its copy (0 far .. 1 at the closest point;
        the copy is level with the dry at 1 / c_passLead): passNearFor() from the engine, or a plain nearness. At the
        start and the end of the chunk. filterOpen: for With the Filter, how far up its present travel the bus's
        filter is (0 closed .. 1 most open), every lane alike, at the end of the chunk. */
    struct Lanes
    {
        std::array<float*, c_lanes> lane {};
        int count = 2;
        std::array<float, c_lanes> passNearFrom {}, passNearTo {};
        float filterOpen = 0.0f;
    };

    /** The wet's share for the front Swirl wheel (0..1): m = 0.5 knob^0.7, equal power. */
    static float mixFor(float knob);
    /** The laws on their own, for the tests and the pictures: the speed a Varispeed value leans to (0.03 = 3%
        faster), On Its Own's place in its sweep (-1 .. 1) for a phase, the Doubler's widest wander (ms) at a rate,
        Thicken's loop gain at a delay, and the end stops. */
    static double leanSpeed(double varispeed);
    /** With the Pass: what Lanes::passNear is for a pass `near` its closest point (0 far .. 1 there) with
        `secondsFromClosest` to go to it (negative) or gone since (positive). Outside c_passWindowSeconds it is
        (1 - c_passHold) near / c_passLead; inside, a raised cosine brings it to 1 at the closest point. */
    static double passNearFor(double near, double secondsFromClosest);
    /** How long settle takes to pass a slow move on: its two one-poles, less what the engine's chunks and the ring
        take back (measured: with this the two zeros sit evenly either side of the middle, SwirlWireTest "centre"). */
    static constexpr double c_settleLagSeconds = 2.0 * c_settleSeconds - 0.0025;
    static double ownShape(double phase);
    static double wanderRangeMs(double rateHz);
    static double thickenGain(double delayMs);
    static double stops(double offsetMs);

    void prepare(double sampleRate);
    void reset();
    void setSeed(std::uint32_t seed) { m_rng.seed(seed); }
    /** Where its own sweeps start (0..1 of a cycle: Tape Flanger On Its Own, the Doubler's Sine and Triangle), for
        renders. After reset(). */
    void scatter(float phase);

    void processTape(const Tape& settings, float knob, const Lanes& lanes, int frames);
    void processDoubler(const Doubler& settings, float knob, const Lanes& lanes, int frames);
    /** Tape Flanger's dry delay on a stereo bed's side signal (left and right), in place. */
    void delaySides(float* left, float* right, int frames);

    int dryDelaySamples() const { return m_drySamples; }
    float getOffsetMs() const { return static_cast<float>(m_hand); }
    void recentre() { if (m_hand != 0.0) m_recentre = true; }
    /** Where a lane's copy sits against its dry at the end of the last call, in ms (Tape Flanger: d, either sign;
        Doubler: its delay). For the tests and the pictures. */
    float getLaneOffsetMs(int lane) const { return static_cast<float>(m_lane[static_cast<size_t>(lane < 0 ? 0 : (lane >= c_lanes ? c_lanes - 1 : lane))].offsetMs); }

private:
    enum Kind { None = 0, TapeFlanger, DoublerKind };

    struct Lane
    {
        sj::DelayLine line;
        std::vector<float> dry;      // the clean ring, m_drySamples long
        int dryAt = 0;
        std::array<sj::OnePole, 2> wetLp, loopLp;
        double fast1 = 0.0, fast2 = 0.0; // the two one-poles of settle (ms)
        double ringAt = 0.0, ringSpeed = 0.0; // the resonance that follows them (ms, ms/s)
        double noise1 = 0.0, noise2 = 0.0;    // Random's two one-poles
        double offsetMs = 0.0;       // d at the end of the last call
        double delay = 0.0;          // the wet read's delay at the end of the last call, samples
        float loopGain = 0.0f;       // the feedback gain there
    };

    void begin(Kind kind);
    void clearSound();
    void leanHand(double varispeed, double dt);

    double m_sampleRate = 48000.0;
    int m_drySamples = 384;  // c_dryMs at m_sampleRate
    bool m_sincRead = true;  // the moving copy is read with the sinc (see c_sincBelowHz)
    float m_startPhase = 0.0f;
    sj::Random m_rng;
    std::array<Lane, c_lanes> m_lane;
    std::array<std::vector<float>, 2> m_side;
    int m_sideAt = 0;
    Kind m_kind = None;
    bool m_snap = true;      // the next call starts from its own settings instead of ramping to them
    bool m_dirty = false;    // something has been through the lines since they were last cleared
    float m_dryGain = 1.0f, m_wetGain = 0.0f;
    double m_ownPhase = 0.0, m_wavePhase = 0.0;
    double m_speed = 0.0;    // Varispeed's speed now (0.03 = 3% faster)
    double m_hand = 0.0;     // the held Varispeed offset, ms
    bool m_recentre = false;
};

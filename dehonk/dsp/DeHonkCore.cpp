// De-Honk DSP core. See DeHonkCore.h and dehonk/SPEC.md section 3.
//
// Signal flow, per channel:
//
//   in ──┬─► lookahead delay ─► a few narrow notches ─► mode/mix/trim ─► out
//        │                              ▲
//        └─► low-pass, decimate to ~8 kHz ─► 1/12-octave band-pass bank ─► band powers
//                                                                            │
//       notch positions and depths ◄─ honk detector (every 4 decimated samples)
//                      ▲
//                      └─ impact guard: keeps the cuts out of a broadband impact while it sounds
//
// The analysis runs at a low sample rate whatever the session rate is. Only the delay,
// the anti-alias low-pass and the notches that are open run at full rate.
//
// Surround: every channel but an LFE is analysed and filtered as above. An LFE only goes
// through the delay, so it stays in time with the rest, and through Trim and the output modes.
// Linked, every other channel gets the same notches, so the image does not move.
//
// With a dialogue sidechain the key goes through the same low-pass, decimator and band bank as a
// channel, and each band's power is compared with the power the detector is working from.
//
// The cut is deliberately narrow. A wide cut takes a slice of everything in the mix out, and
// what is removed sounds like noise. A notch an eighth of an octave wide placed on the peak
// leaves the detail either side.
#include "DeHonkCore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
 #include <xmmintrin.h>
 #define DEHONK_X86 1
#endif

namespace dehonk
{
namespace
{
constexpr int kNumBands = 73;            // 40 Hz * 2^(k/12), k = 0..72 -> 40 Hz .. 2560 Hz
constexpr int kFirstFilterBand = 6;      // 56.6 Hz
constexpr int kLastFilterBand = 70;      // 2281 Hz
constexpr int kNumFilters = kLastFilterBand - kFirstFilterBand + 1;
constexpr int kDetectEvery = 8;          // decimated samples per detector tick (1 ms)
constexpr int kMaxChunk = 256;           // most samples handled in one go
constexpr int kLanes = 4;                // channels low-passed side by side
constexpr int kBankBands = 76;           // kNumBands rounded up to a multiple of four
constexpr double kPi = 3.14159265358979323846;
constexpr float kLn10Over10 = 0.23025851f;
constexpr float kFullScaleSineOffsetDb = 3.0103f;   // mean square of a full-scale sine is -3 dB
constexpr float kSilenceDb = -90.0f;     // loudest band below this: nothing to detect
constexpr float kNotchOpenDb = 0.1f;     // a notch shallower than this is closed
constexpr float kAlwaysBelowDb = -59.95f; // a threshold at the bottom of its range means always act
constexpr float kCeilingMaxDb = 18.0f;    // the most Params::maxCutDb is taken as
constexpr int kGuardLagSteps = 8;         // the impact guard's memory of where each band was
constexpr int kKeyLanes = 2;              // channels of a dialogue sidechain that are listened to
constexpr int kNumGroups = 4;             // front, centre, surround, height
constexpr int kDialogueTrimEvery = 8;     // detector ticks between two looks at the sidechain's level against the mix
constexpr float kChanceFactor = 1.0f;     // see Impl::dlgChance
constexpr double kChanceSlope = 4.0;
constexpr int kNumKeyDelays = 8;          // delays between key and mix that the fit tries
constexpr float kKeyDelaysMs[kNumKeyDelays] = { 0.0f, 3.0f, 6.0f, 10.0f, 15.0f, 20.0f, 30.0f, 45.0f };

static_assert (kNumBands <= kMaxBands);
static_assert (kBankBands >= kNumBands && kBankBands % 4 == 0);
static_assert (kMaxChannels % kLanes == 0);

/** Flush denormals to zero for the duration of process(). */
struct DenormalGuard
{
   #if DEHONK_X86
    unsigned int saved;
    DenormalGuard() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }
    ~DenormalGuard() noexcept { _mm_setcsr (saved); }
   #elif defined(__aarch64__)
    std::uint64_t saved;
    DenormalGuard() noexcept
    {
        __asm__ __volatile__ ("mrs %0, fpcr" : "=r" (saved));
        const std::uint64_t flushed = saved | (1ull << 24);
        __asm__ __volatile__ ("msr fpcr, %0" : : "r" (flushed));
    }
    ~DenormalGuard() noexcept { __asm__ __volatile__ ("msr fpcr, %0" : : "r" (saved)); }
   #else
    DenormalGuard() noexcept {}
   #endif
};

/** Exact comparison, on purpose: has the caller handed in a different value from last time. */
inline bool changed (float a, float b) noexcept { return std::memcmp (&a, &b, sizeof (float)) != 0; }

inline float smoothstep (float t) noexcept
{
    t = std::clamp (t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float onePoleCoef (double timeSeconds, double rate) noexcept
{
    return (float) (1.0 - std::exp (-1.0 / std::max (1.0e-6, timeSeconds * rate)));
}

/** Direct-form-1 biquad section of the anti-alias low-pass. */
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    void setLowpass (double fc, double q, double fs) noexcept
    {
        const double w = 2.0 * kPi * fc / fs, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
        const double a0 = 1.0 + al;
        b0 = (float) ((1.0 - cs) * 0.5 / a0);
        b1 = (float) ((1.0 - cs) / a0);
        b2 = b0;
        a1 = (float) (-2.0 * cs / a0);
        a2 = (float) ((1.0 - al) / a0);
    }
};

struct BiquadState { float x1 = 0, x2 = 0, y1 = 0, y2 = 0; };

inline void setHighpass (Biquad& q, double fc, double fs) noexcept
{
    const double w = 2.0 * kPi * std::min (fc, 0.45 * fs) / fs, cs = std::cos (w), al = std::sin (w) / std::sqrt (2.0);
    const double a0 = 1.0 + al;
    q.b0 = (float) ((1.0 + cs) * 0.5 / a0);
    q.b1 = (float) (-(1.0 + cs) / a0);
    q.b2 = q.b0;
    q.a1 = (float) (-2.0 * cs / a0);
    q.a2 = (float) ((1.0 - al) / a0);
}

inline float tickBiquad (const Biquad& q, BiquadState& z, float x) noexcept
{
    const float y = q.b0 * x + q.b1 * z.x1 + q.b2 * z.x2 - q.a1 * z.y1 - q.a2 * z.y2;
    z.x2 = z.x1; z.x1 = x; z.y2 = z.y1; z.y1 = y;
    return y;
}

/** A band: second-order high-pass then second-order low-pass. */
struct Band
{
    Biquad hp, lp;
    void set (double lo, double hi, double fs) noexcept
    {
        setHighpass (hp, lo, fs);
        lp.setLowpass (std::min (hi, 0.45 * fs), std::sqrt (0.5), fs);
    }
};
struct BandState
{
    BiquadState hp, lp;
    float tick (const Band& b, float x) noexcept { return tickBiquad (b.lp, lp, tickBiquad (b.hp, hp, x)); }
};

/** How sharply a band is spiking: its instant peak against its own running level, in dB. */
struct SpikeMeter
{
    float fast = 0.0f, slow = 0.0f;
    float tick (float v, float fastRelease, float slowCoef) noexcept
    {
        const float a = std::abs (v);
        fast = a > fast ? a : fast + fastRelease * (a - fast);
        slow += slowCoef * (a - slow);
        return fast / (slow + 1.0e-7f);        // ratio; the caller takes the log
    }
};

} // namespace

struct Core::Impl
{
    //============================================================================== setup
    double fs = 48000.0;
    int channels = 2;
    int decim = 6;               // full-rate samples per analysis sample
    double fa = 8000.0;          // analysis rate
    int tickPeriod = 24;         // full-rate samples per detector tick
    bool prepared = false;

    Params params;
    Tuning tune;
    bool dirtyAll = true;        // tuning or sample rate changed
    float lastSharpness = -1, lastSpeed = -1, lastRelease = -1;
    int activeChannels = 2;      // channels in the current process() call
    int numAnalysed = 1;         // of those, the ones that are not an LFE ...
    int analysed[kMaxChannels] {};   // ... and their indices, in order
    int lead = 0;                // the first of them: holds the shared detector state when linked
    bool lfeIn = false;          // an LFE is among the channels in this call
    bool lfeNow[kMaxChannels] {};    // which of this call's channels are played as an LFE

    // Layout, from prepare().
    bool isLfe[kMaxChannels] {};
    float foldL[kMaxChannels] {}, foldR[kMaxChannels] {};   // stereo fold-down weights
    float foldMono[kMaxChannels] {};                       // the same folded on to one channel
    bool lastModuleOn[kNumModules] {};
    float lastEdgeHz[kNumEdges] {};

    float bandF[kNumBands] {};

    //============================================================================== analysis
    Biquad aa[4];
    float aaZ[kMaxChannels / kLanes][4][4][kLanes] {};   // low-pass state: group of kLanes channels, section, (x1, x2, y1, y2), lane
    bool useAa = true;

    // band-pass bank coefficients (analysis rate), shared by channels
    // (kBankBands of them: the ones past kNumBands are all zero and do nothing)
    float bpA1[kBankBands] {}, bpA2[kBankBands] {}, bpA3[kBankBands] {}, bpK[kBankBands] {}, envC[kBankBands] {};
    // two cascaded sections per band, per channel
    float s1a[kMaxChannels][kBankBands] {}, s2a[kMaxChannels][kBankBands] {};
    float s1b[kMaxChannels][kBankBands] {}, s2b[kMaxChannels][kBankBands] {};
    float power[kMaxChannels][kBankBands] {};
    float programPower[kMaxChannels] {};          // mean square of the analysis signal, ~50 ms
    float foldPowerL = 0.0f, foldPowerR = 0.0f;   // the same for the two sides of the stereo fold-down
                                                  //   (more than two channels only)
    float decimated[kMaxChannels][kMaxChunk] {};  // each channel's analysis samples in the current chunk
    int numDecimated = 0;
    float zeros[kMaxChunk] {};                    // what the low-pass reads for lanes with no channel
    float spareBand[kBankBands] {};               // written and never read
    float programC = 0.01f;
    float keyPower = 0.0f, keyC = 0.001f;         // the sidechain: mean square of its loudest channel, ~50 ms
    bool keyPresent = false;                      // this block came with a sidechain

    // Dialogue sidechain: the key's own analysis (as a channel's), and what the detectors make of it.
    BiquadState keyAa[kKeyLanes][4];
    float keyS1a[kKeyLanes][kBankBands] {}, keyS2a[kKeyLanes][kBankBands] {};
    float keyS1b[kKeyLanes][kBankBands] {}, keyS2b[kKeyLanes][kBankBands] {};
    float keyPow[kKeyLanes][kBankBands] {};
    float keyDecimated[kKeyLanes][kMaxChunk] {};
    bool dlgRunning = false;                      // the key is being analysed in this call
    int dlgLanes = 0;
    bool dlgOnNow = false;                        // ... and the detectors are listening to it
    bool dlgAny = false;                          // or some band is still being handed back
    float dlgAmount = 1.0f;
    float dlgD[kNumBands] {};                     // the dialogue's power per band as it stands in the mix: the most of any channel
    float dlgDc[kMaxChannels][kNumBands] {};      // ... and in each channel (kept only when the channels do not all share)
    bool dlgPerChannel = false;
    // How loud the dialogue is in each channel: the key is looked for in the channel band by band.
    // Each band's output of the bank is kept for the chunk, the key's with its quadrature (the
    // band-pass's own low-pass output, a quarter of a cycle behind at the centre), and their
    // products are averaged. What the two share is then the same whatever small delay lies
    // between them, and does not depend on what else is in the band.
    float bankY[kMaxChannels][kDetectEvery][kBankBands] {};
    float keyY[kKeyLanes][kDetectEvery][kBankBands] {}, keyQ[kKeyLanes][kDetectEvery][kBankBands] {};
    float accR[kMaxChannels][kKeyLanes][kBankBands] {}, accI[kMaxChannels][kKeyLanes][kBankBands] {};   // sums since the last tick
    float accM[kMaxChannels][kBankBands] {}, accK[kKeyLanes][kBankBands] {};
    int accCount = 0;
    float crossR[kMaxChannels][kKeyLanes][kBankBands] {}, crossI[kMaxChannels][kKeyLanes][kBankBands] {};   // averaged over dialogueGainMs
    float slowM[kMaxChannels][kKeyLanes][kBankBands] {}, slowK[kKeyLanes][kBankBands] {};
    float dlgChance[kBankBands] {};               // the share of K x M two unrelated signals have in common by chance, per band
    float dlgKept[kNumKeyDelays][kBankBands] {};  // the share (as power) of what key and mix have in common that is
                                                  //   still found with the key this far out of time, per band
    float dlgGsq[kMaxChannels][kKeyLanes] {};     // the gain (as power) that fits the key to each channel, as measured
    float dlgGeff[kMaxChannels][kKeyLanes] {};    // ... and as used: 1 until the key has been found in the mix
    float dlgFit = 0.0f;                          // 0..1: the key has been found in some channel
    float dlgKeyTop[kKeyLanes] {};                // the loudest each key channel has been lately
    float dlgOverTime = 0.0f;                     // seconds the key has been most of the mix
    bool dlgOver = false;
    float dlgSnapGainSq = 1.0f;                   // the dialogue's gain (as power) in what Snap listens to
    float dlgGainMost = 1.0f;                     // the most of dlgGeff over the channels
    bool dlgPinned = false;                       // the gain is at an end of its range
    bool dlgMatched[kMaxChannels][kKeyLanes] {};  // the key has been found in this channel
    float dlgGood[kMaxChannels][kKeyLanes] {};    // the gain it was last found at
    bool dlgHearing[kKeyLanes] {};                // this key channel is playing now
    int dlgHeard[kKeyLanes] {};                   // ticks it has played for, in all
    int dlgCalCount = 0;
    float dlgEnv[kMaxChannels][kNumBands] {};     // 0..1 per detector and band: how far the band is the dialogue's
    bool dlgActive[kMaxChannels] {};              // some of it is not zero
    float dlgTaken[kMaxChannels][kNumBands] {};   // the same as the detector's own state has taken it in so far
    bool dlgTaking[kMaxChannels] {};              // some of that is not zero
    bool dlgShown = false;                        // the frame has some of it in
    float dlgLo = 0.126f, dlgInvSpan = 2.67f, dlgAttackC = 0.06f, dlgReleaseC = 0.004f, dlgSpread = 0.5f;
    float dlgGsqMin = 1.0e-4f, dlgGsqMax = 15.8f, dlgAvgC = 0.002f, dlgGainC = 0.03f, dlgFitUpC = 0.03f, dlgFitDownC = 0.008f;
    float dlgMatchRatio = 1.5f, dlgOverShare = 0.8f, dlgOverSeconds = 1.5f, dlgTickSeconds = 0.001f;
    float washSlow = 0.0f;
    float programDb = -120.0f;                    // after attack and release

    int decimPhase = 0;
    int decimatedSinceTick = 0;
    int samplesToTick = 24;

    //============================================================================== detector
    float refW[kNumBands][kNumBands] {};
    int refLo[kNumBands] {}, refHi[kNumBands] {};   // first and last band with a non-zero weight
    bool refValid[kNumBands] {};
    float focusW[kNumBands] {};
    float runLow[kNumBands] {};                      // nothing below this counts towards the band's reference
    int moduleOfBand[kNumBands] {};                 // -1: in no module
    float edge[kNumEdges] {};                        // edges as used: clamped and in order
    float attackC = 0.1f, releaseC = 0.01f;         // from Speed and Release
    float attackCk[kNumBands] {}, releaseCk[kNumBands] {};   // per band: its range's own if that is unlocked
    bool lastLocked[kNumModules] {};
    float lastRangeAttack[kNumModules] {}, lastRangeRelease[kNumModules] {};
    int groupOf[kMaxChannels] {};                   // 0..kNumGroups-1, for LinkMode::grouped
    float levelAttackC = 0.1f, levelReleaseC = 0.01f, peakC = 0.01f;
    float cut[kMaxChannels][kNumBands] {};          // smoothed requested cut, dB, >= 0
    float slowPower[kMaxChannels][kNumBands] {};    // band power averaged over tune.peakTimeMs
    float slowWanted[kMaxChannels][kNumBands] {};   // wanted cut averaged the same way
    bool evict[kNumBands] {};                       // set by keepPeaks: let go of this band quickly
    float quickC[kNumBands] {};                     // set by keepPeaks: this band lets go at this rate, if it is quicker than Release (0: it is not)
    bool earned[kMaxChannels][kNumBands] {};        // the notch on this band has settled: Release applies in full
    std::uint16_t chosenFor[kMaxChannels][kNumBands] {};   // ticks in a row keepPeaks has chosen this band
    float peakPower[kMaxChannels][kNumBands] {};    // the strongest the band has been lately while its notch was chosen (its slow average)
    // Steady detection (Params::detection), per detector slot. Run only while it is chosen; a slot
    // that was not run at the last tick starts again from its slow average (slowPower) if that was
    // brought up to date at the last tick, else from the band powers now.
    float steadyPow[kMaxChannels][kNumBands] {};    // band power smoothed over steadySmoothMs
    float steadyDb[kMaxChannels][kNumBands] {};     // the steady level, dB as wantedCuts reads levels
    std::uint32_t steadyStamp[kMaxChannels] {};     // the tick it was last run at
    std::uint32_t slowStamp[kMaxChannels] {};       // the tick keepPeaks last brought slowPower up to date at
    std::uint32_t tickCount = 1;
    float steadySmoothC = 0.02f, steadyRiseC = 0.001f, steadyFallC = 0.0025f, steadyMarginDb = 1.0f;
    int steadyReach = 3;
    int earnTicks = 250;
    float moveC = 0.01f, unsettledC = 0.02f, closeC = 0.02f, movedShare = 0.5f, peakFade = 0.999f;
    float actGain = 0.0f;                           // how far over the Threshold, as it reaches the notches
    float outGain[kMaxChannels][kNumBands] {};      // what the notches and the frame get of each channel's cuts:
                                                    //   actGain, less what the impact guard eases off that band
    float ceilingNow = 6.0f;                        // Params::maxCutDb as in force: it moves over a few ms
    float ceilingC = 0.05f;
    bool ceilingSnap = true;
    float guardAmountNow = 1.0f;                    // Params::impactGuard as in force: it moves the same way

    // Impact guard. See guardTick(). One detector when the channels are linked (kept in the lead
    // channel's slot), one per channel when they are not, like the notches themselves.
    struct Guard
    {
        float past[kGuardLagSteps][kNumBands] {};   // the band powers at the last few snapshots, a ring
        float avg[kNumBands] {};                    // where each band was: its short average, a lag ago
        float base[kNumBands] {};                   // the same when the impact landed: what was there before it
        float over[kNumBands] {};                   // 0..1 per band: how far the impact has taken the band over
                                                    //   (times Params::impactGuard and how far the guard is up)
        bool easing = false;                        // some of `over` is not zero
        int pastPos = 0, sinceSnap = 0, holdLeft = 0;
        int sinceLanded = 1 << 20;                  // ticks since something last landed
        bool watching = false;                      // waiting to see whether what landed dies away
        bool rose = false;                          // the level has gone up since the landing
        bool learning = false;                      // no notch was open when it landed: there is nothing to keep
                                                    //   as it was, so the detector goes on learning where the peaks are
        bool confirmed = false;                     // it did: an impact, held until it is over
        float level = 0.0f;                         // the level of the bands the guard listens to, smoothed
        float peak = 0.0f;                          // the highest it has been since the landing
        float before = 0.0f;                        // what it was before the landing
        float duty = 0.0f;                          // the share of the last few seconds the guard was up
        float env = 0.0f;                           // 0..1: how far the guard is up
        float held = 0.0f;                          // env times Params::impactGuard
    };
    Guard guards[kMaxChannels];
    bool wasKept[kMaxChannels][kNumBands] {};       // the bands keepPeaks chose at the last tick
    int guardSnapEvery = 6, guardFirstBand = 0, guardHoldTicks = 0, guardConfirmTicks = 0, guardRefractoryTicks = 0;
    float guardRatio = 4.0f, guardFloor = 0.003f, guardAvgC = 0.06f, guardFallen = 0.25f, guardBack = 1.6f;
    float guardCeiling = 8.0f, guardLevelC = 0.04f;
    float guardLiftC = 0.2f, guardReturnStep = 0.008f, guardDutyC = 0.00025f;
    float guardOverLo = 2.0f, guardOverSpan = 6.0f, guardEaseFallC = 0.02f, guardEaseShare = 1.0f;
    bool staticOn = false;
    float staticCut[kNumBands] {};

    //============================================================================== correction
    float fG[kNumFilters] {};                        // tan(pi f / fs)
    float fA1Flat[kNumFilters] {};                   // a1 at 0 dB
    double invQ = 1.0 / 9.0;
    struct Filt
    {
        float ic1 = 0, ic2 = 0;
        float a1 = 0, m1 = 0;                        // current
        float a1Target = 0, m1Target = 0;
        float dA1 = 0, dM1 = 0;                      // per-sample ramp
        bool active = false;
    };
    Filt filt[kMaxChannels][kNumFilters];
    // The notch coefficients worked out in the current tick, by filter, so that channels asking
    // for the same depth share the work.
    bool targetKnown[kNumFilters] {};
    // Set at each tick with more than two channels: notch j is the same on every treated channel.
    // Good for the channel count it was worked out for (sharedFlagsChannels) until the next tick.
    bool filterShared[kNumFilters] {};
    int sharedFlagsChannels = -1;
    float targetCut[kNumFilters] {}, targetA1[kNumFilters] {}, targetM1[kNumFilters] {};

    //============================================================================== delay, output
    std::vector<float> ring[kMaxChannels];
    float dry[kMaxChannels][kMaxChunk] {};       // the delayed input of the current chunk
    float wetBuf[kMaxChannels][kMaxChunk] {};    // the treated signal of the current chunk (more than two channels)
    float spareLane[kMaxChannels][kMaxChunk] {}; // what Snap's unused lanes work on
    int ringMask = 0, ringWrite = 0;
    int delayNow = 0, delayNext = 0, fadeLeft = 0, fadeLength = 480;   // lookahead changes cross-fade
    float gainWet = 1.0f, gainDry = 0.0f;
    float gainLfe = 1.0f;                        // an LFE's gain: Trim, nothing in Solo Honk, as it is in Diff and Bypass
    float loudWet = 0.0f, loudDry = 0.0f, diffMatchDb = 0.0f;   // running mean squares over diffMatchSeconds, and the offset they call for
    float diffGainFrom = 1.0f, diffGainTo = 1.0f;  // the gain Diff applies, at the start and the end of the current chunk
    // Diff's cross-fade: 0 = not in Diff, 1 = in Diff. Each time Diff is switched it sets off along a
    // cubic from where it is, at the speed it has, to arrive at rest diffFadeLength samples later,
    // so there is no corner in it even when Diff is switched again half-way.
    float diffFade = 0.0f, diffFadeSlope = 0.0f;   // where it is, and how fast it is moving (per sample)
    float diffFadeGoal = 0.0f;                     // 0 or 1
    float diffFadeStart = 0.0f, diffFadeStartSlope = 0.0f;   // where this stretch set off from, and how fast (per stretch)
    int diffFadeAt = 0, diffFadeLength = 3840;     // samples into this stretch (diffFadeLength: it has arrived)
    float pitchClass[12] {};
    bool snapGains = true;
    float modeC = 0.01f;

    //============================================================================== click module
    // A click is a spike in the 2.5 kHz region that the bands either side do not share. A drum
    // hit spikes everywhere at once and is left alone; a level-driven dynamic EQ could not tell
    // the two apart, nor a loud sustained sound from a pop.
    Band clickBand, lowerBand, upperBand;
    BandState clickDet, lowerDet[2], upperDet[2];    // detector, on the mono sum; the neighbours are
                                                     // filtered twice so the click itself does not leak in
    BandState clickPath[kMaxChannels];               // the band taken out of each channel
    // Snap and the dialogue: the key through Snap's own band, delayed as the channels are.
    std::vector<float> keyRing;
    BandState keyClickDet;
    float snapMixPow = 0.0f, snapKeyPow = 0.0f, snapDlgAtkC = 0.02f, snapDlgRelC = 0.0004f;
    float snapDlgPeak = 0.0f;                        // the most Snap was held off since the last frame
    SpikeMeter clickSpike, lowerSpike, upperSpike;
    float clickFastRel = 0.01f, clickSlowC = 0.001f, clickAttackC = 0.2f, clickReleaseC = 0.01f;
    float clickGain = 0.0f;                          // 0 = nothing removed, 1 = whole band removed
    float clickPeakCutDb = 0.0f;                     // deepest since the last frame
    bool clickWasOn = false;
    int clickWarmUp = 0;                             // samples left before a freshly started detector is trusted

    DisplayFrame frame;
    std::uint32_t frameCount = 0;

    //==============================================================================
    void prepare (double sampleRate, int numChannels, const ChannelLayout& layout)
    {
        fs = sampleRate;
        channels = std::clamp (numChannels, 1, kMaxChannels);
        setLayout (layout);
        decim = std::max (1, (int) std::floor (fs / 8000.0));
        fa = fs / decim;
        tickPeriod = decim * kDetectEvery;
        useAa = decim > 1;

        // 8th-order Butterworth low-pass at 2.8 kHz ahead of the decimator.
        static constexpr double qs[4] = { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };
        const double fc = std::min (2800.0, 0.35 * fa);
        for (int i = 0; i < 4; ++i)
            aa[i].setLowpass (fc, qs[i], fs);

        // Two identical band-pass sections per band: together -3 dB at +-1/24 octave. The edges
        // are pre-warped as well as the centre, or the bands narrow towards the top.
        const double half = std::pow (2.0, 0.5 / kBandsPerOctave);
        const double twoSections = std::sqrt (std::sqrt (2.0) - 1.0);
        for (int k = 0; k < kNumBands; ++k)
        {
            const double f = (double) kAnalysisLowHz * std::pow (2.0, (double) k / kBandsPerOctave);
            bandF[k] = (float) f;
            const double top = 0.47 * fa;
            const double gl = std::tan (kPi * std::min (f / half, top * 0.94) / fa);
            const double gh = std::tan (kPi * std::min (f * half, top) / fa);
            const double g = std::sqrt (gl * gh), kk = (gh - gl) / (g * twoSections);
            const double a1 = 1.0 / (1.0 + g * (g + kk));
            bpA1[k] = (float) a1;
            bpA2[k] = (float) (g * a1);
            bpA3[k] = (float) (g * g * a1);
            bpK[k] = (float) kk;
            envC[k] = onePoleCoef (std::clamp (2.0 / f, 0.004, 0.05), fa);
        }

        programC = onePoleCoef (0.05, fa);
        keyC = onePoleCoef (0.05, fs);

        for (int j = 0; j < kNumFilters; ++j)
            fG[j] = (float) std::tan (kPi * bandF[kFirstFilterBand + j] / fs);

        const int maxDelay = (int) std::ceil (kMaxLookaheadMs * 0.001 * fs) + 2;
        int size = 1;
        while (size < maxDelay + kMaxChunk)
            size <<= 1;
        for (auto& r : ring)
            r.assign ((size_t) size, 0.0f);
        keyRing.assign ((size_t) size, 0.0f);
        ringMask = size - 1;

        modeC = onePoleCoef (0.005, fs);
        fadeLength = std::max (1, (int) std::lround (0.010 * fs));

        frame.numBands = kNumBands;
        for (int k = 0; k < kNumBands; ++k)
            frame.bandHz[k] = bandF[k];

        dirtyAll = true;
        prepared = true;
        updateDerived();
        forgetKeyGain();
        reset();
    }

    /** The stereo fold-down that the programme level is measured on with more than two channels.
        The usual one: left and right fronts at full level, the centre and everything off to one
        side at -3 dB (0.7071) to that side, anything else on the centre line at -6 dB (0.5) to
        both, the LFE left out, and no normalisation, so it reads what a meter on the folded-down
        stereo would read. Coherent content in several channels adds as it would in the fold-down. */
    void setLayout (const ChannelLayout& layout) noexcept
    {
        int lfeCount = 0;
        for (int c = 0; c < channels; ++c)
            lfeCount += layout.position[c] == Position::lfe ? 1 : 0;
        for (int c = 0; c < kMaxChannels; ++c)
        {
            Position pos = c < channels ? layout.position[c] : Position::unknown;
            if (pos == Position::lfe && lfeCount >= channels)
                pos = Position::unknown;     // all LFE makes no sense: treat them as ordinary channels
            if (pos == Position::unknown && c == 0) pos = Position::leftFront;
            if (pos == Position::unknown && c == 1) pos = Position::rightFront;
            isLfe[c] = pos == Position::lfe;
            constexpr float h = 0.70710678f;
            float l = 0.0f, r = 0.0f;
            switch (pos)
            {
                case Position::leftFront:   l = 1.0f; break;
                case Position::rightFront:  r = 1.0f; break;
                case Position::centre:      l = r = h; break;
                case Position::leftOther:   l = h; break;
                case Position::rightOther:  r = h; break;
                case Position::lfe:         break;
                case Position::unknown:
                case Position::middleOther: l = r = 0.5f; break;
            }
            foldL[c] = l;
            foldR[c] = r;
            foldMono[c] = 0.5f * (l + r);

            Group grp = c < channels ? layout.group[c] : Group::automatic;
            if (grp == Group::automatic)
                grp = (pos == Position::leftFront || pos == Position::rightFront || pos == Position::unknown) ? Group::front
                    : (pos == Position::centre ? Group::centre : Group::surround);
            groupOf[c] = std::clamp ((int) grp - 1, 0, kNumGroups - 1);
        }
    }

    void reset() noexcept
    {
        foldPowerL = foldPowerR = 0.0f;
        std::memset (aaZ, 0, sizeof (aaZ));
        for (int c = 0; c < kMaxChannels; ++c)
        {
            programPower[c] = 0.0f;
            for (int k = 0; k < kBankBands; ++k)
                s1a[c][k] = s2a[c][k] = s1b[c][k] = s2b[c][k] = power[c][k] = 0.0f;
            for (int k = 0; k < kNumBands; ++k)
                cut[c][k] = slowPower[c][k] = slowWanted[c][k] = 0.0f;
            for (int j = 0; j < kNumFilters; ++j)
            {
                auto& f = filt[c][j];
                f = {};
                f.a1 = f.a1Target = fA1Flat[j];
            }
            std::fill (ring[c].begin(), ring[c].end(), 0.0f);
        }
        resetClick();
        for (auto& g : guards)
            g = {};
        std::memset (wasKept, 0, sizeof (wasKept));
        std::memset (earned, 0, sizeof (earned));
        std::memset (chosenFor, 0, sizeof (chosenFor));
        std::memset (peakPower, 0, sizeof (peakPower));
        std::memset (steadyStamp, 0, sizeof (steadyStamp));
        std::memset (slowStamp, 0, sizeof (slowStamp));
        tickCount = 1;
        std::memset (evict, 0, sizeof (evict));
        std::memset (quickC, 0, sizeof (quickC));
        actGain = 0.0f;
        for (auto& ch : outGain)
            for (auto& g : ch)
                g = 1.0f;
        ceilingSnap = true;
        frame.guard = 0.0f;
        sharedFlagsChannels = -1;
        loudWet = loudDry = diffMatchDb = 0.0f;
        diffGainFrom = diffGainTo = 1.0f;
        diffFade = diffFadeSlope = diffFadeGoal = 0.0f;
        diffFadeAt = diffFadeLength;
        keyPower = washSlow = 0.0f;
        clearKeyAnalysis();
        dlgRunning = dlgOnNow = dlgAny = false;
        frame.keyMatch = 0.0f;
        frame.keyOver = false;
        frame.snapDialogue = 0.0f;
        std::memset (dlgEnv, 0, sizeof (dlgEnv));
        std::memset (dlgActive, 0, sizeof (dlgActive));
        std::memset (dlgTaken, 0, sizeof (dlgTaken));
        std::memset (dlgTaking, 0, sizeof (dlgTaking));
        std::memset (dlgD, 0, sizeof (dlgD));
        for (int k = 0; k < kMaxBands; ++k)
            frame.dialogue[k] = 0.0f;
        frame.dialogueTrimDb = 0.0f;
        dlgShown = false;
        for (auto& n : pitchClass) n = 0.0f;
        programDb = -120.0f;
        ringWrite = 0;
        decimPhase = 0;
        decimatedSinceTick = 0;
        samplesToTick = tickPeriod;
        snapGains = true;
        for (int k = 0; k < kNumBands; ++k)
        {
            frame.levelDb[k] = -120.0f;
            frame.cutDb[k] = 0.0f;
        }
        frame.totalCutDb = 0.0f;
        frame.programDb = -120.0f;
        frame.activity = 0.0f;
        for (auto& m : frame.moduleCutDb)
            m = 0.0f;
        frame.snapCutDb = 0.0f;
        frame.diffOffsetDb = 0.0f;
        fadeLeft = 0;
        ++frameCount;
    }

    /** The gain the key was found at is not audio that has gone by but how the session is
        routed, so it is kept through reset() and through the key being switched; only prepare()
        starts it again. */
    void forgetKeyGain() noexcept
    {
        for (auto& ch : dlgGsq)  for (auto& g : ch) g = 1.0f;
        for (auto& ch : dlgGeff) for (auto& g : ch) g = 1.0f;
        for (auto& ch : dlgGood) for (auto& g : ch) g = 1.0f;
        dlgFit = 0.0f;
        dlgSnapGainSq = dlgGainMost = 1.0f;
        dlgPinned = false;
    }

    void clearKeyAnalysis() noexcept
    {
        for (auto& lane : keyAa)
            for (auto& z : lane)
                z = {};
        std::memset (keyS1a, 0, sizeof (keyS1a));
        std::memset (keyS2a, 0, sizeof (keyS2a));
        std::memset (keyS1b, 0, sizeof (keyS1b));
        std::memset (keyS2b, 0, sizeof (keyS2b));
        std::memset (keyPow, 0, sizeof (keyPow));
        std::memset (accR, 0, sizeof (accR));
        std::memset (accI, 0, sizeof (accI));
        std::memset (accM, 0, sizeof (accM));
        std::memset (accK, 0, sizeof (accK));
        std::memset (crossR, 0, sizeof (crossR));
        std::memset (crossI, 0, sizeof (crossI));
        std::memset (slowM, 0, sizeof (slowM));
        std::memset (slowK, 0, sizeof (slowK));
        accCount = 0;
        for (auto& v : dlgKeyTop) v = 0.0f;
        dlgOverTime = 0.0f;
        dlgOver = false;
        std::memset (dlgMatched, 0, sizeof (dlgMatched));
        std::memset (dlgHearing, 0, sizeof (dlgHearing));
        std::memset (dlgHeard, 0, sizeof (dlgHeard));
        dlgCalCount = 0;
        std::fill (keyRing.begin(), keyRing.end(), 0.0f);
        keyClickDet = {};
        snapMixPow = snapKeyPow = snapDlgPeak = 0.0f;
    }

    //============================================================================== derived values
    /** Recomputes whatever depends on parameters that changed. Cheap; called at each tick. */
    void updateDerived() noexcept
    {
        bool focusChanged = dirtyAll;
        for (int m = 0; m < kNumModules; ++m)
            focusChanged = focusChanged || params.moduleOn[m] != lastModuleOn[m];
        for (int e = 0; e < kNumEdges; ++e)
            focusChanged = focusChanged || changed (params.edgeHz[e], lastEdgeHz[e]);

        if (focusChanged)
        {
            for (int m = 0; m < kNumModules; ++m)
                lastModuleOn[m] = params.moduleOn[m];
            for (int e = 0; e < kNumEdges; ++e)
            {
                lastEdgeHz[e] = params.edgeHz[e];
                edge[e] = std::clamp (params.edgeHz[e], kEdgeMinHz[e], kEdgeMaxHz[e]);
                if (e > 0)
                    edge[e] = std::max (edge[e], edge[e - 1] * 1.1f);
            }

            // A module works strictly inside its own edges, whatever its neighbours are doing.
            // What a module is compared with does not depend on the others either: Woof sees
            // everything, the rest never see the bass.
            for (int k = 0; k < kNumBands; ++k)
            {
                moduleOfBand[k] = -1;
                focusW[k] = 0.0f;
                runLow[k] = 0.0f;
                if (k < kFirstFilterBand || k > kLastFilterBand)
                    continue;
                for (int m = 0; m < kNumModules; ++m)
                    if (params.moduleOn[m] && bandF[k] >= edge[m] && bandF[k] < edge[m + 1])
                    {
                        moduleOfBand[k] = m;
                        focusW[k] = 1.0f;
                        runLow[k] = m == Woof ? 0.0f : edge[std::max (1, m - 1)];
                    }
            }
        }

        if (focusChanged || changed (params.sharpness, lastSharpness))
        {
            // The reference for a band is the level of its surroundings: a ring around the band
            // that leaves out the band's own neighbourhood, so that a feature up to `inner` wide
            // stands clear of it. For every module but Woof the bass is left out
            // altogether: the bass is always louder than the low mids, and that must neither
            // hide honk nor count as it.
            lastSharpness = params.sharpness;
            const float innerOct = tune.broadWidthOctaves
                                 * std::pow (tune.narrowWidthOctaves / tune.broadWidthOctaves, std::clamp (params.sharpness, 0.0f, 1.0f));
            const float sigmaIn = std::max (0.4f, innerOct * (float) kBandsPerOctave / 2.3548f);
            const float sigmaOut = sigmaIn * std::max (1.2f, tune.surroundRatio);

            // The weight depends only on the distance between two bands.
            float ringWeight[kNumBands];
            for (int d = 0; d < kNumBands; ++d)
            {
                const float dd = (float) d * (float) d;
                ringWeight[d] = std::max (0.0f, std::exp (-0.5f * dd / (sigmaOut * sigmaOut)) - std::exp (-0.5f * dd / (sigmaIn * sigmaIn)));
            }

            for (int k = 0; k < kNumBands; ++k)
            {
                const float contextLow = runLow[k] * 0.97f;
                float sum = 0.0f;
                refLo[k] = kNumBands;
                refHi[k] = -1;
                for (int j = 0; j < kNumBands; ++j)
                {
                    const float w = (focusW[k] <= 0.0f || bandF[j] < contextLow) ? 0.0f : ringWeight[std::abs (j - k)];
                    refW[k][j] = w;
                    sum += w;
                    if (w > 0.0f)
                    {
                        refLo[k] = std::min (refLo[k], j);
                        refHi[k] = j;
                    }
                }
                refValid[k] = sum > 1.0e-3f;
                for (int j = 0; j < kNumBands; ++j)
                    refW[k][j] = refValid[k] ? refW[k][j] / sum : 0.0f;
            }
        }

        bool timingChanged = focusChanged;
        for (int m = 0; m < kNumModules; ++m)
            timingChanged = timingChanged || params.rangeTimingLocked[m] != lastLocked[m]
                         || changed (params.rangeAttackMs[m], lastRangeAttack[m]) || changed (params.rangeReleaseMs[m], lastRangeRelease[m]);

        if (dirtyAll || changed (params.speed, lastSpeed))
        {
            timingChanged = true;
            lastSpeed = params.speed;
            const float s = std::clamp (params.speed, 0.0f, 1.0f);
            const double tickRate = fs / tickPeriod;
            attackC = onePoleCoef (0.001 * tune.attackSlowMs * std::pow (tune.attackFastMs / tune.attackSlowMs, s), tickRate);
        }

        if (dirtyAll || changed (params.releaseMs, lastRelease))
        {
            timingChanged = true;
            lastRelease = params.releaseMs;
            releaseC = onePoleCoef (0.001 * (double) std::clamp (params.releaseMs, 10.0f, 5000.0f), fs / tickPeriod);
        }

        if (timingChanged)
        {
            // A range whose timing is unlocked goes in and lets go at its own rates; every other
            // band at the ones Speed and Release give.
            const double tickRate = fs / tickPeriod;
            float rangeAttack[kNumModules], rangeRelease[kNumModules];
            for (int m = 0; m < kNumModules; ++m)
            {
                lastLocked[m] = params.rangeTimingLocked[m];
                lastRangeAttack[m] = params.rangeAttackMs[m];
                lastRangeRelease[m] = params.rangeReleaseMs[m];
                rangeAttack[m] = lastLocked[m] ? attackC : onePoleCoef (0.001 * (double) std::clamp (params.rangeAttackMs[m], 0.5f, 1000.0f), tickRate);
                rangeRelease[m] = lastLocked[m] ? releaseC : onePoleCoef (0.001 * (double) std::clamp (params.rangeReleaseMs[m], 10.0f, 5000.0f), tickRate);
            }
            for (int k = 0; k < kNumBands; ++k)
            {
                const int m = moduleOfBand[k];
                attackCk[k] = m >= 0 ? rangeAttack[m] : attackC;
                releaseCk[k] = m >= 0 ? rangeRelease[m] : releaseC;
            }
        }

        if (dirtyAll)
        {
            const double tickRate = fs / tickPeriod;
            levelAttackC = onePoleCoef (0.001 * tune.levelAttackMs, tickRate);
            levelReleaseC = onePoleCoef (0.001 * tune.levelReleaseMs, tickRate);
            peakC = onePoleCoef (0.001 * tune.peakTimeMs, tickRate);
            ceilingC = onePoleCoef (0.015, tickRate);

            moveC = onePoleCoef (0.001 * (double) std::max (1.0f, tune.moveReleaseMs), tickRate);
            unsettledC = onePoleCoef (0.001 * (double) std::max (1.0f, tune.unsettledReleaseMs), tickRate);
            earnTicks = std::clamp ((int) std::lround (0.001 * tune.earnMs * tickRate), 0, 60000);
            closeC = onePoleCoef (0.001 * (double) std::max (1.0f, tune.thresholdCloseMs), tickRate);
            movedShare = std::exp (-kLn10Over10 * std::clamp (tune.movedWithinDb, 0.0f, 60.0f));
            peakFade = (float) std::exp (-1.0 / (1.0 * tickRate));      // what "as strong as it was" looks back over: a second
            steadySmoothC = onePoleCoef (0.001 * (double) std::max (1.0f, tune.steadySmoothMs), tickRate);
            steadyRiseC = onePoleCoef (0.001 * (double) std::max (1.0f, tune.steadyRiseMs), tickRate);
            steadyFallC = onePoleCoef (0.001 * (double) std::max (1.0f, tune.steadyFallMs), tickRate);
            steadyReach = std::clamp ((int) std::lround (tune.steadyLumpBands), 0, 24);
            steadyMarginDb = tune.steadyLumpMarginDb;

            guardSnapEvery = std::max (1, (int) std::lround (0.001 * std::max (1.0f, tune.guardLagMs) * tickRate / kGuardLagSteps));
            guardRatio = std::exp (kLn10Over10 * std::clamp (tune.guardRiseDb, 0.5f, 40.0f));
            guardFloor = std::exp (kLn10Over10 * std::clamp (tune.guardFloorDb, -80.0f, 0.0f));
            guardCeiling = std::exp (kLn10Over10 * (std::clamp (tune.guardCeilingDb, -40.0f, 40.0f) - kFullScaleSineOffsetDb));
            guardAvgC = onePoleCoef (0.001 * std::max (1.0f, tune.guardAverageMs), tickRate / guardSnapEvery);
            guardLevelC = onePoleCoef (0.001 * std::max (1.0f, tune.guardLevelMs), tickRate);
            guardHoldTicks = std::max (1, (int) std::lround (0.001 * tune.guardHoldMs * tickRate));
            guardConfirmTicks = std::clamp ((int) std::lround (0.001 * tune.guardConfirmMs * tickRate), 1, guardHoldTicks);
            guardRefractoryTicks = std::max (1, (int) std::lround (0.001 * tune.guardRefractoryMs * tickRate));
            guardFallen = std::exp (-kLn10Over10 * std::clamp (tune.guardDecayDb, 0.0f, 40.0f));
            guardBack = std::exp (kLn10Over10 * std::clamp (tune.guardBackDb, 0.0f, 40.0f));
            guardLiftC = onePoleCoef (0.001 * std::max (0.5f, tune.guardLiftMs), tickRate);
            guardReturnStep = (float) (1.0 / std::max (1.0, 0.001 * tune.guardReturnMs * tickRate));
            guardDutyC = onePoleCoef ((double) std::max (0.25f, tune.guardDutySeconds), tickRate);
            guardOverLo = std::exp (kLn10Over10 * std::clamp (tune.guardOverStartDb, 0.0f, 40.0f));
            guardOverSpan = std::max (1.0e-3f, std::exp (kLn10Over10 * std::clamp (tune.guardOverFullDb, 0.0f, 60.0f)) - guardOverLo);
            guardEaseShare = std::clamp (tune.guardEase, 0.0f, 1.0f);
            guardEaseFallC = onePoleCoef (0.001 * std::max (1.0f, tune.guardEaseFallMs), tickRate);
            guardFirstBand = 0;
            while (guardFirstBand < kNumBands - 8 && bandF[guardFirstBand] < tune.guardLowHz)
                ++guardFirstBand;

            {
                const float full = std::clamp (tune.dialogueFullDb, -40.0f, 20.0f);
                const float start = std::min (std::clamp (tune.dialogueStartDb, -60.0f, 20.0f), full - 0.1f);
                dlgLo = std::exp (kLn10Over10 * start);
                dlgInvSpan = 1.0f / (std::exp (kLn10Over10 * full) - dlgLo);
                dlgSpread = std::clamp (tune.dialogueSpread, 0.0f, 1.0f);
                dlgAttackC = onePoleCoef (0.001 * std::max (1.0f, tune.dialogueAttackMs), tickRate);
                dlgReleaseC = onePoleCoef (0.001 * std::max (1.0f, tune.dialogueReleaseMs), tickRate);
                dlgGsqMax = std::exp (kLn10Over10 * std::clamp (tune.dialogueTrimMaxDb, 0.0f, 40.0f));
                dlgGsqMin = std::exp (kLn10Over10 * std::clamp (tune.dialogueTrimMinDb, -80.0f, 0.0f));
                const double avg = 0.001 * (double) std::clamp (tune.dialogueGainMs, 50.0f, 5000.0f);
                dlgAvgC = onePoleCoef (avg, tickRate);
                dlgGainC = onePoleCoef (0.3 * avg, tickRate / kDialogueTrimEvery);
                dlgFitUpC = onePoleCoef (0.5 * avg, tickRate / kDialogueTrimEvery);
                dlgFitDownC = onePoleCoef (3.0 * avg, tickRate / kDialogueTrimEvery);
                dlgMatchRatio = std::max (0.0f, tune.dialogueMatchRatio);
                dlgOverShare = std::clamp (tune.dialogueOverShare, 0.1f, 1.0f);
                dlgOverSeconds = std::max (0.05f, tune.dialogueOverSeconds);
                dlgTickSeconds = (float) (1.0 / tickRate);
                // Two unrelated signals in one band still move together a little over a finite
                // time: by 1 / (1 + the averaging time x the band's width x a constant) of the
                // product of their powers (measured with unrelated noise on mix and key).
                for (int k = 0; k < kBankBands; ++k)
                    dlgChance[k] = k < kNumBands ? kChanceFactor / (1.0f + (float) (kChanceSlope * avg * 0.0578 * (double) bandF[k])) : 0.0f;
                // A key that is a little out of time with the mix: a band's two filter sections
                // remember for about as long as the band is narrow, so the higher the band the
                // less of what the two share is found. (1 + x) e^-x in amplitude, x = pi x one
                // section's width x the delay.
                for (int t = 0; t < kNumKeyDelays; ++t)
                    for (int k = 0; k < kBankBands; ++k)
                    {
                        const double x = kPi * 0.0898 * (double) bandF[std::min (k, kNumBands - 1)] * 0.001 * (double) kKeyDelaysMs[t];
                        const double kept = (1.0 + x) * std::exp (-x);
                        dlgKept[t][k] = (float) (kept * kept);
                    }
                snapDlgAtkC = onePoleCoef (0.001 * (double) std::max (0.05f, tune.snapDialogueAttackMs), fs);
                snapDlgRelC = onePoleCoef (0.001 * (double) std::max (1.0f, tune.snapDialogueReleaseMs), fs);
            }
            diffFadeLength = std::max (1, (int) std::lround (0.001 * (double) std::clamp (tune.diffFadeMs, 0.0f, 2000.0f) * fs));

            const double lo = std::max (200.0, (double) tune.clickLowHz), hi = std::max (lo * 1.2, (double) tune.clickHighHz);
            clickBand.set (lo, hi, fs);
            lowerBand.set (lo / 4.5, lo / 1.8, fs);
            upperBand.set (hi * 1.6, hi * 3.0, fs);
            clickFastRel = onePoleCoef (0.002, fs);
            clickSlowC = onePoleCoef (0.001 * std::max (1.0f, tune.clickBodyMs), fs);
            clickAttackC = onePoleCoef (0.0001, fs);
            clickReleaseC = onePoleCoef (0.001 * std::max (0.5f, tune.clickReleaseMs), fs);
            buildFilters();
            for (int c = 0; c < kMaxChannels; ++c)
                for (int j = 0; j < kNumFilters; ++j)
                    if (! filt[c][j].active)
                        filt[c][j].a1 = filt[c][j].a1Target = fA1Flat[j];
        }

        dirtyAll = false;
    }

    void buildFilters() noexcept
    {
        invQ = 1.0 / std::max (0.5, (double) tune.bellQ);
        for (int j = 0; j < kNumFilters; ++j)
        {
            const double g = (double) fG[j];
            fA1Flat[j] = (float) (1.0 / (1.0 + g * (g + invQ)));
        }
    }

    //============================================================================== analysis
    // The analysis is done in two passes per chunk. The anti-alias low-pass runs on every channel
    // at once, four channels to a vector (it is the same filter on each), and keeps one sample in
    // `decim`. The band bank then takes each treated channel's analysis samples in turn. Each
    // channel gets exactly the arithmetic it would get on its own, so mono and stereo are
    // unchanged by this and a 7.1.4 costs far less than six stereo instances.

    /** Low-passes `n` full-rate samples of channels 0 to nch - 1 from offset `done`, and writes
        each channel's analysis samples to decimated[c] (numDecimated of them) and its programme
        power. `phase` is the decimation phase at the start of the chunk. An LFE goes through the
        low-pass with the rest, as that is cheaper than leaving its lane out, but nothing reads it. */
    void antiAlias (float* const* io, int done, int n, int nch, int phase) noexcept
    {
        static_assert (kMaxChannels == 3 * kLanes);
        switch ((nch + kLanes - 1) / kLanes)
        {
            case 1:  antiAliasGroups<1> (io, done, n, nch, phase); break;
            case 2:  antiAliasGroups<2> (io, done, n, nch, phase); break;
            default: antiAliasGroups<3> (io, done, n, nch, phase); break;
        }
    }

    /** antiAlias() for G groups of kLanes channels. All of them go through each sample together,
        so the groups' filters overlap in time instead of waiting on one another. */
    template <int G>
    void antiAliasGroups (float* const* io, int done, int n, int nch, int phase) noexcept
    {
        constexpr int W = G * kLanes;
        float z[4][4][W];           // section, (x1, x2, y1, y2), lane
        for (int g = 0; g < G; ++g)
            for (int st = 0; st < 4; ++st)
                for (int v = 0; v < 4; ++v)
                    for (int l = 0; l < kLanes; ++l)
                        z[st][v][g * kLanes + l] = aaZ[g][st][v][l];
        const float* in[W];
        for (int l = 0; l < W; ++l)
            in[l] = l < nch ? io[l] + done : zeros;     // lanes past the last channel read silence
        float pp[W];
        for (int l = 0; l < W; ++l)
            pp[l] = programPower[l];

        int ph = phase, m = 0;
        for (int i = 0; i < n; ++i)
        {
            float x[W];
            for (int l = 0; l < W; ++l)
            {
                x[l] = in[l][i];
                if (! (std::abs (x[l]) < 1.0e8f))     // false for NaN and infinity: keep them out of the analysis
                    x[l] = 0.0f;
            }
            if (useAa)
            {
                for (int st = 0; st < 4; ++st)
                {
                    const auto& q = aa[st];
                    float* zx1 = z[st][0]; float* zx2 = z[st][1]; float* zy1 = z[st][2]; float* zy2 = z[st][3];
                    for (int l = 0; l < W; ++l)
                    {
                        const float y = q.b0 * x[l] + q.b1 * zx1[l] + q.b2 * zx2[l] - q.a1 * zy1[l] - q.a2 * zy2[l];
                        zx2[l] = zx1[l]; zx1[l] = x[l]; zy2[l] = zy1[l]; zy1[l] = y;
                        x[l] = y;
                    }
                }
            }

            if (ph == 0)
            {
                for (int l = 0; l < W; ++l)
                {
                    pp[l] += programC * (x[l] * x[l] - pp[l]);
                    decimated[l][m] = x[l];
                }
                ++m;
            }

            if (++ph >= decim)
                ph = 0;
        }

        for (int g = 0; g < G; ++g)
            for (int st = 0; st < 4; ++st)
                for (int v = 0; v < 4; ++v)
                    for (int l = 0; l < kLanes; ++l)
                        aaZ[g][st][v][l] = z[st][v][g * kLanes + l];
        for (int l = 0; l < W; ++l)
            programPower[l] = pp[l];
        numDecimated = m;
    }

    /** Runs channel c's analysis samples from this chunk through every band. The bands do not
        depend on one another, so the inner loop vectorises; the bank is padded to a multiple of
        the vector width with bands that do nothing. */
    void bandBank (int c, int m) noexcept
    {
        bandBankOn (s1a[c], s2a[c], s1b[c], s2b[c], power[c], decimated[c], m);
    }

    /** The same on any set of states: a channel's, or the dialogue sidechain's. */
    void bandBankOn (float* __restrict a1, float* __restrict a2, float* __restrict b1, float* __restrict b2,
                     float* __restrict pw, const float* __restrict xs, int m) noexcept
    {
        const float* __restrict ka1 = bpA1;
        const float* __restrict ka2 = bpA2;
        const float* __restrict ka3 = bpA3;
        const float* __restrict kk = bpK;
        const float* __restrict ke = envC;

        for (int i = 0; i < m; ++i)
        {
            const float x = xs[i];
            for (int k = 0; k < kBankBands; ++k)
            {
                const float ca1 = ka1[k], ca2 = ka2[k], ca3 = ka3[k], ck = kk[k];

                float v3 = x - a2[k];
                float v1 = ca1 * a1[k] + ca2 * v3;
                float v2 = a2[k] + ca2 * a1[k] + ca3 * v3;
                a1[k] = 2.0f * v1 - a1[k];
                a2[k] = 2.0f * v2 - a2[k];
                const float y1 = ck * v1;

                v3 = y1 - b2[k];
                v1 = ca1 * b1[k] + ca2 * v3;
                v2 = b2[k] + ca2 * b1[k] + ca3 * v3;
                b1[k] = 2.0f * v1 - b1[k];
                b2[k] = 2.0f * v2 - b2[k];
                const float y2 = ck * v1;

                pw[k] += ke[k] * (y2 * y2 - pw[k]);
            }
        }
    }

    /** bandBankOn(), keeping every band's output for the chunk in ys (m rows), and in qs, if it
        is given, the output a quarter of a cycle behind. The same arithmetic for pw. */
    void bandBankKeep (float* __restrict a1, float* __restrict a2, float* __restrict b1, float* __restrict b2,
                       float* __restrict pw, const float* __restrict xs, int m,
                       float (* __restrict ys)[kBankBands], float (* __restrict qs)[kBankBands]) noexcept
    {
        const float* __restrict ka1 = bpA1;
        const float* __restrict ka2 = bpA2;
        const float* __restrict ka3 = bpA3;
        const float* __restrict kk = bpK;
        const float* __restrict ke = envC;

        for (int i = 0; i < m; ++i)
        {
            const float x = xs[i];
            float* __restrict yo = ys[i];
            float* __restrict qo = qs != nullptr ? qs[i] : spareBand;
            for (int k = 0; k < kBankBands; ++k)
            {
                const float ca1 = ka1[k], ca2 = ka2[k], ca3 = ka3[k], ck = kk[k];

                float v3 = x - a2[k];
                float v1 = ca1 * a1[k] + ca2 * v3;
                float v2 = a2[k] + ca2 * a1[k] + ca3 * v3;
                a1[k] = 2.0f * v1 - a1[k];
                a2[k] = 2.0f * v2 - a2[k];
                const float y1 = ck * v1;

                v3 = y1 - b2[k];
                v1 = ca1 * b1[k] + ca2 * v3;
                v2 = b2[k] + ca2 * b1[k] + ca3 * v3;
                b1[k] = 2.0f * v1 - b1[k];
                b2[k] = 2.0f * v2 - b2[k];
                const float y2 = ck * v1;

                pw[k] += ke[k] * (y2 * y2 - pw[k]);
                yo[k] = y2;
                qo[k] = ck * v2;
            }
        }
    }

    /** What each analysed channel and each key channel had in common in this chunk, band by band. */
    void keyProducts (int m) noexcept
    {
        for (int l = 0; l < dlgLanes; ++l)
        {
            float* __restrict kAcc = accK[l];
            for (int i = 0; i < m; ++i)
            {
                const float* __restrict ky = keyY[l][i];
                for (int k = 0; k < kBankBands; ++k)
                    kAcc[k] += ky[k] * ky[k];
            }
        }
        for (int a = 0; a < numAnalysed; ++a)
        {
            const int c = analysed[a];
            float* __restrict mAcc = accM[c];
            for (int i = 0; i < m; ++i)
            {
                const float* __restrict y = bankY[c][i];
                for (int k = 0; k < kBankBands; ++k)
                    mAcc[k] += y[k] * y[k];
            }
            for (int l = 0; l < dlgLanes; ++l)
            {
                float* __restrict r = accR[c][l];
                float* __restrict q = accI[c][l];
                for (int i = 0; i < m; ++i)
                {
                    const float* __restrict y = bankY[c][i];
                    const float* __restrict ky = keyY[l][i];
                    const float* __restrict kq = keyQ[l][i];
                    for (int k = 0; k < kBankBands; ++k)
                    {
                        r[k] += y[k] * ky[k];
                        q[k] += y[k] * kq[k];
                    }
                }
            }
        }
        accCount += m;
    }

    /** With more than two channels: the programme level of the stereo fold-down, from the
        analysis samples every channel recorded in this chunk. */
    void foldDown() noexcept
    {
        for (int i = 0; i < numDecimated; ++i)
        {
            float l = 0.0f, r = 0.0f;
            for (int a = 0; a < numAnalysed; ++a)
            {
                const int c = analysed[a];
                l += foldL[c] * decimated[c][i];
                r += foldR[c] * decimated[c][i];
            }
            foldPowerL += programC * (l * l - foldPowerL);
            foldPowerR += programC * (r * r - foldPowerR);
        }
    }

    //============================================================================== detector
    /** Band powers in, wanted cut per band out (dB, >= 0, before time smoothing). With `steady`
        (Detection::steady: each band's steady level, dB), the excess is taken on that and must
        also clear the floor drawn under it; the parent rule and the silence test still read p. */
    void wantedCuts (const float* p, float* out, const float* steady = nullptr) const noexcept
    {
        float level[kNumBands], flat[kNumBands], comp[kNumBands];
        float steadyLevel[kNumBands], lumpFloor[kNumBands];

        float loudest = -200.0f;
        for (int k = 0; k < kNumBands; ++k)
        {
            level[k] = 10.0f * std::log10 (p[k] + 1.0e-14f) + kFullScaleSineOffsetDb;
            loudest = std::max (loudest, level[k]);
        }

        if (loudest < kSilenceDb)
        {
            std::fill (out, out + kNumBands, 0.0f);
            return;
        }

        // Empty bands must not drag the tilt fit or the parent test around.
        const float floorDb = loudest - 70.0f;
        for (int k = 0; k < kNumBands; ++k)
            level[k] = std::max (level[k], floorDb);

        // What the excess over the surroundings is taken on: the levels now, or the steady levels.
        const float* lv = level;
        if (steady != nullptr)
        {
            for (int k = 0; k < kNumBands; ++k)
                steadyLevel[k] = std::max (steady[k], floorDb);
            lv = steadyLevel;
            // The floor under the steady spectrum: each band no higher than the mean of the two
            // bands m away, for m = 1 up to steadyReach in turn. A peak narrower than the reach is
            // cut off by it; a lift as wide as the reach or wider is left in the floor.
            std::memcpy (lumpFloor, steadyLevel, sizeof (lumpFloor));
            float before[kNumBands];
            for (int m = 1; m <= steadyReach; ++m)
            {
                std::memcpy (before, lumpFloor, sizeof (before));
                for (int k = m; k + m < kNumBands; ++k)
                    lumpFloor[k] = std::min (before[k], 0.5f * (before[k - m] + before[k + m]));
            }
        }

        // Overall tilt: a mix falls away with frequency, and that slope is not honk.
        float slope = 0.0f, intercept = 0.0f;
        if (tune.detrend)
        {
            float n = 0.0f, meanK = 0.0f, meanL = 0.0f;
            for (int k = 0; k < kNumBands; ++k)
                if (refValid[k]) { n += 1.0f; meanK += (float) k; meanL += lv[k]; }
            if (n >= 6.0f)
            {
                meanK /= n; meanL /= n;
                float sxy = 0.0f, sxx = 0.0f;
                for (int k = 0; k < kNumBands; ++k)
                    if (refValid[k])
                    {
                        const float dk = (float) k - meanK;
                        sxy += dk * (lv[k] - meanL);
                        sxx += dk * dk;
                    }
                slope = std::clamp (sxy / sxx, -6.0f / (float) kBandsPerOctave, 0.0f);   // dB per band; at most 6 dB per octave
                intercept = meanL - slope * meanK;
            }
        }

        const float alpha = std::clamp (tune.referenceExponent, 0.1f, 1.0f);
        if (alpha >= 1.0f && ! tune.detrend && steady == nullptr)
        {
            // The usual case: the reference is an average of power, and the levels came from the
            // powers, so there is nothing to convert back.
            const float floorPower = std::exp (kLn10Over10 * floorDb);
            for (int k = 0; k < kNumBands; ++k)
            {
                flat[k] = level[k];
                comp[k] = std::max (floorPower, 2.0f * (p[k] + 1.0e-14f));   // 2 = the 3.01 dB sine offset
            }
        }
        else
        {
            for (int k = 0; k < kNumBands; ++k)
            {
                flat[k] = lv[k] - (intercept + slope * (float) k);
                comp[k] = std::exp (alpha * kLn10Over10 * flat[k]);
            }
        }

        const float deadZone = tune.deadZoneDb, knee = std::max (0.01f, tune.kneeDb);
        const float slopeOfCut = std::clamp (params.depth, 0.0f, 1.0f) * tune.depthSlope;
        const float ceiling = ceilingNow;
        const float parentSpan = std::max (0.1f, tune.parentFullDb - tune.parentStartDb);

        for (int k = 0; k < kNumBands; ++k)
        {
            if (focusW[k] <= 0.0f || slopeOfCut <= 0.0f || ! refValid[k])
            {
                out[k] = 0.0f;
                continue;
            }

            // Four running sums, so the additions do not wait on one another.
            float r0 = 0.0f, r1 = 0.0f, r2 = 0.0f, r3 = 0.0f;
            const float* w = refW[k];
            int j = refLo[k];
            const int end = refHi[k] + 1;
            for (; j + 4 <= end; j += 4)
            {
                r0 += w[j] * comp[j];
                r1 += w[j + 1] * comp[j + 1];
                r2 += w[j + 2] * comp[j + 2];
                r3 += w[j + 3] * comp[j + 3];
            }
            for (; j < end; ++j)
                r0 += w[j] * comp[j];
            const float ref = (r0 + r1) + (r2 + r3);
            float excess = flat[k] - (10.0f / alpha) * std::log10 (ref + 1.0e-20f);
            if (steady != nullptr && steadyReach > 0)
                excess = std::min (excess, steadyLevel[k] - lumpFloor[k] + steadyMarginDb);

            // Soft knee around the dead zone.
            const float x = excess - deadZone;
            float over;
            if (x <= -0.5f * knee)      over = 0.0f;
            else if (x >= 0.5f * knee)  over = x;
            else                        over = (x + 0.5f * knee) * (x + 0.5f * knee) / (2.0f * knee);

            // Parent rule: added harmonics always sit above something stronger at a subharmonic.
            // A band that is the lowest strong thing around (a fundamental) is left alone.
            float eligible = 1.0f;
            // Bands in the lowest octave of the analysis have no band an octave below to look at;
            // they are taken as eligible.
            if (tune.parentRule > 0.0f && k >= kBandsPerOctave)
            {
                static constexpr int below[] = { 12, 19, 24, 28 };     // octave, twelfth, two octaves, 5th harmonic
                float parent = -200.0f;
                for (int b : below)
                    if (k - b >= 0)
                        parent = std::max (parent, level[k - b]);
                const float e = std::clamp ((parent - level[k] - tune.parentStartDb) / parentSpan, 0.0f, 1.0f);
                eligible = 1.0f - std::clamp (tune.parentRule, 0.0f, 1.0f) * (1.0f - e);
            }

            out[k] = std::min (ceiling, slopeOfCut * over * eligible * focusW[k]);
        }
    }

    void smoothCuts (int c, const float* wanted) noexcept
    {
        if (dlgActive[c] || dlgTaking[c])
        {
            // How far the state below has taken in what the dialogue takes off: the same step,
            // at the same rate, as each band's cut is about to make.
            const float rate = guards[c].held > 0.0f ? 1.0f - guards[c].held : 1.0f;
            bool any = false;
            for (int k = 0; k < kNumBands; ++k)
            {
                const float down = evict[k] ? std::max (attackCk[k], releaseCk[k]) : std::max (releaseCk[k], quickC[k]);
                const float coef = wanted[k] > cut[c][k] ? attackCk[k] : down;
                float& h = dlgTaken[c][k];
                h += rate * coef * (dlgEnv[c][k] - h);
                if (! (h >= 1.0e-4f) && dlgEnv[c][k] <= 0.0f)
                    h = 0.0f;
                any = any || h > 0.0f;
            }
            dlgTaking[c] = any;
        }
        if (guards[c].held > 0.0f)
        {
            // An impact is sounding: the detector's own state moves only as far as the guard
            // lets it. No notch grows into the impact, and what was decided before it is still
            // there after it.
            const float rate = 1.0f - guards[c].held;
            for (int k = 0; k < kNumBands; ++k)
            {
                float& s = cut[c][k];
                const float down = evict[k] ? std::max (attackCk[k], releaseCk[k]) : std::max (releaseCk[k], quickC[k]);
                const float coef = wanted[k] > s ? attackCk[k] : down;
                s += rate * coef * (wanted[k] - s);
                if (! (s >= 1.0e-4f))
                    s = 0.0f;
                s = std::min (s, ceilingNow);
            }
            return;
        }
        for (int k = 0; k < kNumBands; ++k)
        {
            float& s = cut[c][k];
            const float down = evict[k] ? std::max (attackCk[k], releaseCk[k]) : std::max (releaseCk[k], quickC[k]);
            const float coef = wanted[k] > s ? attackCk[k] : down;
            s += coef * (wanted[k] - s);
            if (! (s >= 1.0e-4f))       // also catches NaN
                s = 0.0f;
            s = std::min (s, ceilingNow);   // only bites while Max Cut is being turned down
        }
    }

    /** The impact guard's detector, once per tick, on the band powers detector `c` works from
        (what every channel shares when they are linked, a channel's own when they are not).

        An impact (an explosion, a hit, a slam) comes up across the whole spectrum at once. A note,
        a chord or a resonance comes up in a few bands. So: count the bands that stand guardRiseDb
        or more above where they were. "Where it was" is the band's own short average, taken a lag
        ago so that the rise itself is not in it (about 50 to 250 ms back), and never less than a
        floor set by the level of the whole range, so that an empty band does not count for having
        gone from nothing to next to nothing. When at least guardShare of the bands have risen
        together, something has landed, and the guard comes up, quickly.

        While it is up, the detector's own state is held: no notch grows into the impact, and
        what was decided before it is still there after it. A notch that is already open is
        eased off as far as the impact stands over what was there before it landed, in the
        notch's own band or across the whole range, whichever is more (not at all up to
        guardOverStartDb over, fully from guardOverFullDb). A notch under an impact that has
        taken its band over would be cutting the impact, and what it was put there for cannot
        be heard under it; one on a band where the old sound still leads, under a hit that is
        no louder than the passage, stays as it is and goes on doing its job. As the impact
        dies away the notches come back with it, so nothing is switched.

        Meanwhile the level of the guard's bands is watched (averaged over guardLevelMs). An
        impact dies away: once the level is guardDecayDb under the highest it reached, it was
        one, and the guard stays up until guardHoldMs after the landing. If the level has not
        fallen by guardConfirmMs (a cut-in, a chord, a wall of sound starting), it was an entry,
        and the guard lets go then. Either way it lets go as soon as the level is back within
        guardBackDb of what was there before, and in a straight line over guardReturnMs.

        It cannot switch the plug-in off through a busy passage. The average a band is compared
        with catches up with a new level in a couple of hundred milliseconds, so a passage that
        is loud all the time raises nothing; a hit inside it is over as soon as the level is back
        to the passage's own, and touches only the bands it stood out in; nothing new can land
        for guardRefractoryMs after a landing; and the share
        of the last few seconds the guard was up is kept: past two thirds of guardDutyLimit each
        new landing holds for less, and at the limit for nothing. Nothing counts while the
        programme is under the Threshold, where there is no cut to keep out. Needs no lookahead:
        it uses the analysis bands that are there anyway. */
    void guardTick (int c, const float* p, bool acting) noexcept
    {
        auto& g = guards[c];
        float total = 0.0f;
        for (int k = guardFirstBand; k < kNumBands; ++k)
            total += g.avg[k];
        const float floor = total * guardFloor + 1.0e-12f;
        int risen = 0;
        float now = 0.0f;
        for (int k = guardFirstBand; k < kNumBands; ++k)
        {
            const float v = std::min (p[k], guardCeiling);
            now += v;
            risen += v > guardRatio * std::max (g.avg[k], floor) ? 1 : 0;
        }
        if (! (now >= 0.0f))        // NaN
            now = 0.0f;
        g.level += guardLevelC * (now - g.level);
        g.sinceLanded = std::min (g.sinceLanded + 1, 1 << 20);

        const float share = (float) risen / (float) (kNumBands - guardFirstBand);
        const float limit = std::clamp (tune.guardDutyLimit, 0.05f, 1.0f);
        const float room = std::clamp ((limit - g.duty) / (limit / 3.0f), 0.0f, 1.0f);
        if (acting && share >= tune.guardShare && room > 0.0f && g.sinceLanded > guardRefractoryTicks)
        {
            if (g.env <= 0.0f)
            {
                // What was there before it. (Something landing while the guard is still up for
                // the last one is measured against what was there before that one.)
                std::memcpy (g.base, g.avg, sizeof (g.base));
                bool open = false;
                for (int k = 0; k < kNumBands; ++k)
                {
                    g.base[k] = std::max (g.base[k], floor);
                    open = open || cut[c][k] >= kNotchOpenDb;
                }
                g.before = total;
                g.learning = ! open;
            }
            g.sinceLanded = 0;
            g.watching = true;
            g.confirmed = false;
            g.rose = false;
            g.peak = g.level;
            g.holdLeft = std::max (g.holdLeft, (int) ((float) guardConfirmTicks * room));
        }
        if (g.watching)
        {
            g.peak = std::max (g.peak, g.level);
            if (g.level < g.peak * guardFallen)
            {
                // It is dying away: an impact. Held until it is over, at most guardHoldMs from the landing.
                g.watching = false;
                g.confirmed = true;
                g.holdLeft = std::max (g.holdLeft, (int) ((float) guardHoldTicks * room) - g.sinceLanded);
            }
            else if (g.sinceLanded >= guardConfirmTicks)
                g.watching = false;     // an entry: the hold runs out now
        }
        if (g.watching || g.confirmed)
        {
            if (g.level >= g.before * guardBack)
                g.rose = true;
            else if (g.rose)
            {
                g.watching = g.confirmed = false;   // back to what was there before: it is over
                g.holdLeft = 0;
            }
        }
        if (g.holdLeft == 0)
            g.confirmed = false;

        if (++g.sinceSnap >= guardSnapEvery)
        {
            g.sinceSnap = 0;
            float* oldest = g.past[g.pastPos];
            for (int k = 0; k < kNumBands; ++k)
            {
                g.avg[k] += guardAvgC * (oldest[k] - g.avg[k]);
                oldest[k] = std::min (p[k], guardCeiling);
            }
            g.pastPos = (g.pastPos + 1) % kGuardLagSteps;
        }

        if (g.holdLeft > 0)
        {
            --g.holdLeft;
            g.env += guardLiftC * (1.0f - g.env);
            if (g.env > 0.999f) g.env = 1.0f;
        }
        else
            g.env -= guardReturnStep;
        if (! (g.env > 0.0f)) g.env = 0.0f;     // also catches NaN
        g.duty += guardDutyC * (g.env - g.duty);

        g.held = staticOn ? 0.0f : guardAmountNow * g.env;

        // How far the impact has taken over: each band, and the whole range (see above).
        if (g.held > 0.0f || g.easing)
        {
            bool any = false;
            const float whole = std::clamp ((now - guardOverLo * g.before) / (guardOverSpan * g.before + 1.0e-20f), 0.0f, 1.0f);
            for (int k = 0; k < kNumBands; ++k)
            {
                float target = 0.0f;
                if (g.held > 0.0f)
                    target = g.held * std::max (whole, std::clamp ((std::min (p[k], guardCeiling) - guardOverLo * g.base[k]) / (guardOverSpan * g.base[k]), 0.0f, 1.0f));
                float& e = g.over[k];
                // Up at once (the guard itself comes up smoothly), back down over guardEaseFallMs.
                e = target > e ? target : e + guardEaseFallC * (target - e);
                if (! (e >= 1.0e-3f) && target <= 0.0f)
                    e = 0.0f;
                any = any || e > 0.0f;
            }
            g.easing = any;
        }
    }

    /** Decides where the notches go. `wanted` says how much each band should come down (the
        area is too strong there); `p` is the band powers it was worked out from. A notch is put
        only on a peak: a band that has stood above its neighbours either side for a while. The
        few most prominent peaks are kept, a set distance apart, and everything else is left
        alone, so the cut is a few notches and not a slab. A notch already open in channel `c`
        is preferred, so that it does not hop between neighbours. */
    void keepPeaks (int c, float* wanted, const float* p) noexcept
    {
        // Where a notch goes is decided on slow averages, so that it stays put while the fast
        // detector flickers. How deep it is follows the fast detector.
        float* slow = slowPower[c];
        float* slowW = slowWanted[c];
        // An impact is not a peak to learn: while the guard is up the slow averages stand still,
        // so that the notches that were open are still the chosen ones after it. With none open
        // when it landed (the usual start of a loud passage) there is nothing to keep, and
        // learning goes on, so that the first notch is not kept waiting longer than the guard.
        const float slowC = guards[c].held > 0.0f && ! guards[c].learning ? peakC * (1.0f - guards[c].held) : peakC;
        for (int k = 0; k < kNumBands; ++k)
        {
            slow[k] += slowC * (p[k] - slow[k]);
            slowW[k] += slowC * (wanted[k] - slowW[k]);
            if (! (slowW[k] >= 1.0e-4f))
                slowW[k] = 0.0f;
        }
        slowStamp[c] = tickCount;

        // Prominence: the band against the bands 1/6 to 1/3 octave either side of it.
        const int nearB = std::max (1, kBandsPerOctave / 6), farB = std::max (nearB, kBandsPerOctave / 3);
        float score[kNumBands];
        for (int k = 0; k < kNumBands; ++k)
        {
            score[k] = 0.0f;
            if (slowW[k] <= 0.25f)
                continue;
            float below = 0.0f, above = 0.0f;
            int nb = 0, na = 0;
            for (int d = nearB; d <= farB; ++d)
            {
                if (k - d >= 0)         { below += slow[k - d]; ++nb; }
                if (k + d < kNumBands)  { above += slow[k + d]; ++na; }
            }
            if (nb == 0 || na == 0)
                continue;
            const float around = 0.5f * (below / (float) nb + above / (float) na);
            const float prominence = 10.0f * std::log10 ((slow[k] + 1.0e-20f) / (around + 1.0e-20f));
            // An open notch is preferred. With the hand-over, of two notches passing a peak
            // between them only the one that holds it now is: both are open.
            const bool mine = tune.handOver ? (wasKept[c][k] && cut[c][k] > 0.0f) : cut[c][k] > 0.5f;
            if (prominence >= tune.peakMinDb)
                score[k] = prominence + (mine ? tune.notchHoldDb : 0.0f);
            else if (mine && prominence >= tune.peakMinDb - tune.notchHoldDb)
                score[k] = 0.01f + std::max (0.0f, prominence);     // an open notch may linger a little
        }

        float kept[kNumBands] {};
        bool chosen[kNumBands] {};
        const float floorShare = tune.handOver ? std::clamp (tune.depthFloor, 0.0f, 1.0f) : 0.7f;
        const int spacing = std::max (1, (int) std::lround (tune.notchSpacingOctaves * (float) kBandsPerOctave));
        const int perModule = std::clamp ((int) std::lround (tune.notchesPerModule), 1, 8);
        int used[kNumModules] {};

        for (int n = 0; n < perModule * kNumModules; ++n)
        {
            int best = -1;
            float bestScore = 0.0f;
            for (int k = 0; k < kNumBands; ++k)
                if (score[k] > bestScore && moduleOfBand[k] >= 0 && used[moduleOfBand[k]] < perModule)
                    { bestScore = score[k]; best = k; }
            if (best < 0)
                break;
            ++used[moduleOfBand[best]];
            // How deep follows the fast detector. (Before the hand-over it was held up by the slow
            // average as well, which kept a notch open a quarter of a second after its peak had gone.)
            kept[best] = std::max (wanted[best], floorShare * slowW[best]);
            chosen[best] = true;
            for (int k = std::max (0, best - spacing + 1); k <= std::min (kNumBands - 1, best + spacing - 1); ++k)
                score[k] = 0.0f;
        }

        if (tune.handOver)
        {
            // A notch near a chosen band that is not chosen itself. The chosen one grows only as
            // this one lets go: the two never add up to more than the one that is wanted, and
            // nothing hops. How fast this one lets go depends on why it lost its place:
            //   its peak has moved next door (the chosen neighbour is about as strong as this band
            //   was while it was chosen, within movedWithinDb, and this band is now that far under
            //   the neighbour): at moveC, or at Release if that is quicker. The peak is no longer
            //   here, and a cut left behind on a band that is not a peak is a cut on the wrong thing;
            //   the peak is still here but a band next door is a little stronger for the moment (a
            //   bump two bands wide, a note between two bands): at Release, so the two cross over
            //   slowly and the cut stays where it is;
            //   its peak has faded where it was: at Release, if the notch had settled (been chosen
            //   for earnMs in a row). One that never settled, as on noise, goes at moveC too.
            // And a module never has more open than it may: of those over the number, the
            // shallowest go quickly.
            int lingering[kNumModules] {};
            const float earnAt = std::min (std::clamp (params.depth, 0.0f, 1.0f) * tune.depthSlope * std::max (0.0f, tune.earnProudDb - tune.deadZoneDb),
                                           0.75f * ceilingNow);
            for (int k = 0; k < kNumBands; ++k)
            {
                evict[k] = false;
                quickC[k] = 0.0f;
                if (chosen[k])
                {
                    // Settling: chosen, and as deep as a peak earnProudDb above its surroundings is
                    // cut, for earnMs in a row. (On plain noise the bands take turns to stand out
                    // and are chosen too, but shallowly.)
                    chosenFor[c][k] = cut[c][k] >= earnAt ? (std::uint16_t) std::min (60000, chosenFor[c][k] + 1) : (std::uint16_t) 0;
                    earned[c][k] = earned[c][k] || chosenFor[c][k] >= earnTicks;
                    peakPower[c][k] = std::max (slow[k], peakPower[c][k] * peakFade);
                    continue;
                }
                chosenFor[c][k] = 0;
                if (cut[c][k] <= 0.0f)
                {
                    earned[c][k] = false;
                    peakPower[c][k] = 0.0f;
                    continue;
                }
                quickC[k] = earned[c][k] ? 0.0f : unsettledC;
                if (moduleOfBand[k] < 0)
                {
                    quickC[k] = unsettledC; // its module was switched off or its edge moved: nothing to wait for
                    continue;
                }
                bool near = false;
                for (int j = std::max (0, k - spacing + 1); j <= std::min (kNumBands - 1, k + spacing - 1); ++j)
                    if (chosen[j])
                    {
                        kept[j] = std::max (0.0f, kept[j] - cut[c][k]);
                        near = true;
                        if (slow[j] >= movedShare * peakPower[c][k])
                            quickC[k] = std::max (quickC[k], moveC);
                    }
                if (! near && cut[c][k] >= kNotchOpenDb)
                    ++lingering[moduleOfBand[k]];
            }
            for (int m = 0; m < kNumModules; ++m)
                while (used[m] + lingering[m] > perModule && lingering[m] > 0)
                {
                    int shallowest = -1;
                    for (int k = 0; k < kNumBands; ++k)
                        if (moduleOfBand[k] == m && ! chosen[k] && ! evict[k] && cut[c][k] >= kNotchOpenDb
                            && (shallowest < 0 || cut[c][k] < cut[c][shallowest]))
                        {
                            bool near = false;
                            for (int j = std::max (0, k - spacing + 1); j <= std::min (kNumBands - 1, k + spacing - 1); ++j)
                                near = near || chosen[j];
                            if (! near)
                                shallowest = k;
                        }
                    if (shallowest < 0)
                        break;
                    evict[shallowest] = true;
                    --lingering[m];
                }
        }
        else
        {
            // As it was before the hand-over: a notch that has lost its place gets out of the way
            // quickly, whether a neighbour took it or its module is full.
            for (int k = 0; k < kNumBands; ++k)
            {
                evict[k] = false;
                quickC[k] = 0.0f;
                if (kept[k] > 0.0f || cut[c][k] <= 0.0f || moduleOfBand[k] < 0)
                    continue;
                if (used[moduleOfBand[k]] >= perModule)
                    evict[k] = true;
                for (int j = std::max (0, k - spacing + 1); j <= std::min (kNumBands - 1, k + spacing - 1) && ! evict[k]; ++j)
                    if (kept[j] > 0.0f)
                        evict[k] = true;
            }
        }

        for (int k = 0; k < kNumBands; ++k)
            wasKept[c][k] = chosen[k];
        std::memcpy (wanted, kept, sizeof (kept));
    }

    /** Sets one channel's notches ramping towards its cuts over the next tick period. */
    void setFilterTargets (int c) noexcept
    {
        const float invPeriod = 1.0f / (float) tickPeriod;
        const float* state = cut[c] + kFirstFilterBand;
        const float* scale = outGain[c] + kFirstFilterBand;   // 1 unless the Threshold or the impact guard is holding some of the cut back
        float s[kNumFilters];
        for (int i = 0; i < kNumFilters; ++i)
            s[i] = state[i] * scale[i];

        for (int i = 0; i < kNumFilters; ++i)
        {
            auto& f = filt[c][i];
            if (! f.active && s[i] < kNotchOpenDb)
                continue;            // closed and staying closed: already flat
            f.a1 = f.a1Target;       // land exactly on the last target before ramping again
            f.m1 = f.m1Target;

            const double gainDb = s[i] < kNotchOpenDb ? 0.0 : -(double) std::clamp (s[i], 0.0f, ceilingNow);
            if (gainDb == 0.0)
            {
                f.a1Target = fA1Flat[i];
                f.m1Target = 0.0f;
            }
            else if (targetKnown[i] && ! changed (targetCut[i], s[i]))
            {
                // Another channel asked for the same depth this tick (always so when linked).
                f.a1Target = targetA1[i];
                f.m1Target = targetM1[i];
            }
            else
            {
                const double A = std::exp (gainDb * (std::log (10.0) / 40.0));
                const double g = (double) fG[i], k = invQ / A;
                f.a1Target = (float) (1.0 / (1.0 + g * (g + k)));
                f.m1Target = (float) (k * (A * A - 1.0));
                targetKnown[i] = true;
                targetCut[i] = s[i];
                targetA1[i] = f.a1Target;
                targetM1[i] = f.m1Target;
            }

            const bool wasActive = f.active;
            f.active = (f.m1 != 0.0f || f.m1Target != 0.0f);
            if (! f.active)
            {
                if (wasActive)
                    f.ic1 = f.ic2 = 0.0f;
                f.a1 = f.a1Target;
                f.dA1 = f.dM1 = 0.0f;
            }
            else
            {
                f.dA1 = (f.a1Target - f.a1) * invPeriod;
                f.dM1 = (f.m1Target - f.m1) * invPeriod;
            }
        }
    }

    //============================================================================== dialogue sidechain
    // The user feeds the dialogue stem to the sidechain. Per band, the dialogue's power as it
    // stands in the mix is set against the power the detector is working from. A band the dialogue
    // accounts for (its power within dialogueFullDb of the band's) is the dialogue's: nothing is
    // asked for there, so it is not chosen for a notch and does not use up its range's notches, and
    // a notch already open there is eased off, quickly, and comes back slowly when the dialogue
    // has left, so the gaps between words do not pump. (The easing is the share the detector's own
    // state has not yet let go of: that state gives the band up at the Release, as it gives up
    // anything it is no longer asked for, and dlgTaken follows how far it has, at the same rates.
    // What reaches the filters is the state times what is still to come off, so the two together
    // are the cut that is asked for, times one less Amount times the share that is the dialogue's:
    // at once when the dialogue arrives, and exactly, whatever Release is.) A band that stands
    // dialogueStartDb or more over the dialogue is treated as ever: build-up that is well above the
    // voice is still cut while the voice is in it. Nothing else changes: what a band is compared
    // with, the Threshold and the impact guard see the mix as they always do.
    //
    // The dialogue's power in a channel is the key's power (through the same band bank, so in
    // time with the channel and smoothed the same) times the gain the dialogue has in that channel.
    // That gain is measured: over the last half second or so in which the key was playing, the
    // products of the channel's and the key's band outputs are averaged, with the key's
    // quadrature as well so that a delay of a few milliseconds between the two does not matter.
    // The part of a band the key accounts for moves with the key, whatever else is in the band,
    // so the sum of those products over the bands, less what two unrelated signals share by
    // chance, over the key's own power, is the gain: up or down, and nothing for a channel the
    // dialogue is not in (a surround, with the voice in the centre). Until the key has been found
    // in some channel (the first half second, or a key that is not in the mix or is far out of
    // time with it) it is taken at the level it arrives at.

    /** Every few ticks: the gain that fits each key channel to each analysed channel. */
    void fitKey() noexcept
    {
        // found: the key is in some channel. missing: it is in none, and it is loud enough
        // against the mix that it would have shown at the level it was last found at (so not
        // just covered by a loud passage).
        bool anyHeard = false, found = false, missing = false;
        const int warm = std::max (1, (int) (1.0f / std::max (1.0e-6f, dlgAvgC)));     // an averaging time of key
        for (int l = 0; l < dlgLanes; ++l)
        {
            if (dlgHeard[l] < warm)
                continue;
            float den = 0.0f;
            for (int k = kFirstFilterBand; k <= kLastFilterBand; ++k)
                den += slowK[l][k] * slowK[l][k];
            if (! (den > 1.0e-24f))
                continue;
            if (dlgHearing[l])
                anyHeard = true;
            for (int a = 0; a < numAnalysed; ++a)
            {
                const int c = analysed[a];
                const float* r = crossR[c][l];
                const float* q = crossI[c][l];
                const float* m = slowM[c][l];
                float shared = 0.0f, chance = 0.0f;
                float seen[kBankBands], keySq[kBankBands];
                for (int k = kFirstFilterBand; k <= kLastFilterBand; ++k)
                {
                    const float both = r[k] * r[k] + q[k] * q[k], byChance = dlgChance[k] * slowK[l][k] * m[k];
                    shared += both;
                    chance += byChance;
                    seen[k] = both - byChance;
                    keySq[k] = slowK[l][k] * slowK[l][k];
                }
                if (! std::isfinite (shared) || ! std::isfinite (chance))
                {
                    std::memset (crossR[c][l], 0, sizeof (crossR[c][l]));
                    std::memset (crossI[c][l], 0, sizeof (crossI[c][l]));
                    std::memset (slowM[c][l], 0, sizeof (slowM[c][l]));
                    continue;
                }
                if (! dlgHearing[l])
                    continue;           // nothing new: what was found stands

                // The gain, and with it the delay of those tried that leaves the least unexplained.
                float est = 0.0f, least = 0.0f;
                for (int t = 0; t < kNumKeyDelays; ++t)
                {
                    const float* kept = dlgKept[t];
                    float num = 0.0f, d2 = 0.0f;
                    for (int k = kFirstFilterBand; k <= kLastFilterBand; ++k)
                    {
                        num += seen[k] * kept[k];
                        d2 += keySq[k] * kept[k] * kept[k];
                    }
                    const float g = std::max (0.0f, num) / (d2 + 1.0e-30f);
                    float left = 0.0f;
                    for (int k = kFirstFilterBand; k <= kLastFilterBand; ++k)
                    {
                        const float e = seen[k] - g * keySq[k] * kept[k];
                        left += e * e;
                    }
                    if (t == 0 || left < least)
                    {
                        least = left;
                        est = g;
                    }
                }
                est = std::clamp (est, dlgGsqMin, dlgGsqMax);

                const bool matched = shared - chance > dlgMatchRatio * chance;
                dlgMatched[c][l] = matched;
                // The gain follows what is measured (in dB, so that 24 dB down takes no longer
                // than 6) while the key shows in the channel, or would show at the gain it is
                // held at and does not: then it is not there at that gain. Otherwise it cannot
                // be told, and the gain stays.
                if (matched || dlgGsq[c][l] * den > 2.0f * chance)
                    dlgGsq[c][l] *= std::exp (dlgGainC * std::log (est / dlgGsq[c][l]));
                if (matched)
                {
                    found = true;
                    dlgGood[c][l] = dlgGsq[c][l];
                }
                else if (dlgGood[c][l] * den > (1.0f + dlgMatchRatio) * chance)
                    missing = true;
            }
        }
        if (anyHeard && found)
            dlgFit += dlgFitUpC * (1.0f - dlgFit);
        else if (anyHeard && missing)
            dlgFit -= dlgFitDownC * dlgFit;
        if (! (dlgFit >= 1.0e-3f) && ! found)
            dlgFit = 0.0f;
        if (dlgFit > 0.999f)
            dlgFit = 1.0f;

        // What Snap listens to is the channels folded to one, and the key's channels likewise.
        float snapGain = 0.0f, most = 0.0f, least = 1.0e30f;
        bool pinned = false;
        for (int a = 0; a < numAnalysed; ++a)
        {
            const int c = analysed[a];
            float g = 0.0f;
            for (int l = 0; l < dlgLanes; ++l)
            {
                dlgGeff[c][l] = dlgFit * dlgGsq[c][l] + (1.0f - dlgFit);
                g = std::max (g, dlgGeff[c][l]);
                if (dlgMatched[c][l])
                    least = std::min (least, dlgGsq[c][l]);
            }
            most = std::max (most, g);
            const float w = activeChannels > 2 ? foldMono[c] : (numAnalysed > 1 ? 0.5f : 1.0f);
            snapGain += w * std::sqrt (g);
        }
        dlgSnapGainSq = snapGain * snapGain;
        dlgGainMost = most;
        pinned = dlgFit > 0.5f && (most >= 0.95f * dlgGsqMax || least <= 1.05f * dlgGsqMin);
        dlgPinned = pinned;
    }

    /** Once per tick, before the detectors: the averages the gain is fitted from, and the
        dialogue's band powers as they stand in the mix. */
    void dialoguePowers (bool perChannel) noexcept
    {
        dlgOnNow = dlgRunning && ! staticOn;
        dlgAmount = std::clamp (params.dialogueAmount, 0.0f, 1.0f);
        dlgPerChannel = perChannel;
        if (dlgOnNow)
        {
            for (int l = 0; l < dlgLanes; ++l)
                if (! std::isfinite (keyPow[l][kNumBands / 2]))
                    clearKeyAnalysis();

            if (accCount > 0)
            {
                const float inv = 1.0f / (float) accCount;
                for (int l = 0; l < dlgLanes; ++l)
                {
                    // Only while this key channel is playing: the averages are over the time the
                    // key was there to be compared, and wait as they are through its pauses.
                    float now = 0.0f;
                    for (int k = kFirstFilterBand; k <= kLastFilterBand; ++k)
                        now += accK[l][k];
                    now *= inv;
                    dlgKeyTop[l] = std::max (now, dlgKeyTop[l] * (1.0f - 0.03f * dlgAvgC));
                    const bool hearing = now > 1.0e-9f && now > 0.003f * dlgKeyTop[l];
                    dlgHearing[l] = hearing;
                    if (hearing)
                    {
                        dlgHeard[l] = std::min (dlgHeard[l] + 1, 1 << 28);
                        float* __restrict sk = slowK[l];
                        const float* __restrict ak = accK[l];
                        for (int k = 0; k < kBankBands; ++k)
                            sk[k] += dlgAvgC * (ak[k] * inv - sk[k]);
                        for (int a = 0; a < numAnalysed; ++a)
                        {
                            const int c = analysed[a];
                            float* __restrict r = crossR[c][l];
                            float* __restrict q = crossI[c][l];
                            float* __restrict m = slowM[c][l];
                            const float* __restrict ar = accR[c][l];
                            const float* __restrict aq = accI[c][l];
                            const float* __restrict am = accM[c];
                            for (int k = 0; k < kBankBands; ++k)
                            {
                                r[k] += dlgAvgC * (ar[k] * inv - r[k]);
                                q[k] += dlgAvgC * (aq[k] * inv - q[k]);
                                m[k] += dlgAvgC * (am[k] * inv - m[k]);
                            }
                        }
                    }
                    std::memset (accK[l], 0, sizeof (accK[l]));
                }
                for (int a = 0; a < numAnalysed; ++a)
                {
                    const int c = analysed[a];
                    std::memset (accM[c], 0, sizeof (accM[c]));
                    std::memset (accR[c], 0, sizeof (accR[c]));
                    std::memset (accI[c], 0, sizeof (accI[c]));
                }
                accCount = 0;
            }
            if (++dlgCalCount >= kDialogueTrimEvery)
            {
                dlgCalCount = 0;
                fitKey();
            }

            std::fill (dlgD, dlgD + kNumBands, 0.0f);
            float one[kNumBands];
            for (int a = 0; a < numAnalysed; ++a)
            {
                const int c = analysed[a];
                float* dc = perChannel ? dlgDc[c] : one;
                const float g0 = dlgGeff[c][0], g1 = dlgLanes > 1 ? dlgGeff[c][1] : 0.0f;
                const float* k1 = keyPow[dlgLanes > 1 ? 1 : 0];
                for (int k = 0; k < kNumBands; ++k)
                {
                    dc[k] = std::max (g0 * keyPow[0][k], g1 * k1[k]);
                    dlgD[k] = std::max (dlgD[k], dc[k]);
                }
            }
        }
        bool any = dlgOnNow;
        for (int a = 0; a < numAnalysed; ++a)
            any = any || dlgActive[analysed[a]];
        dlgAny = any;       // (dlgTaken runs itself out in smoothCuts, with or without this)
    }

    /** After the detectors: has the key been most of the mix for a while. */
    void dialogueOver (const float* shared) noexcept
    {
        if (! dlgOnNow)
        {
            dlgOverTime = 0.0f;
            dlgOver = false;
            return;
        }
        float top = 0.0f;
        for (int k = kFirstFilterBand; k <= kLastFilterBand; ++k)
            top = std::max (top, dlgD[k]);
        int in = 0, all = 0;
        if (top > 1.0e-9f)
            for (int k = kFirstFilterBand; k <= kLastFilterBand; ++k)
                if (dlgD[k] > 0.03f * top)
                {
                    ++in;
                    all += dlgD[k] > 0.8f * shared[k] ? 1 : 0;
                }
        if (in >= 6 && dlgFit >= 0.5f)
            dlgOverTime = (float) all >= dlgOverShare * (float) in ? std::min (dlgOverTime + dlgTickSeconds, dlgOverSeconds + 0.5f)
                                                                 : std::max (0.0f, dlgOverTime - 2.0f * dlgTickSeconds);
        dlgOver = dlgOverTime >= dlgOverSeconds || dlgPinned;
    }

    /** Detector c, on the band powers p it works from and the dialogue's powers d there: how far
        each band is the dialogue's, and that share taken off what the detector asks for. */
    void dialogueTick (int c, const float* p, const float* d, float* wanted) noexcept
    {
        float raw[kNumBands];
        if (dlgOnNow)
        {
            for (int k = 0; k < kNumBands; ++k)
            {
                const float t = std::clamp ((d[k] - dlgLo * p[k]) * dlgInvSpan / (p[k] + 1.0e-20f), 0.0f, 1.0f);
                raw[k] = d[k] > 1.0e-10f ? t * t * (3.0f - 2.0f * t) : 0.0f;
            }
        }
        else
            std::fill (raw, raw + kNumBands, 0.0f);

        float* env = dlgEnv[c];
        bool any = false;
        for (int k = 0; k < kNumBands; ++k)
        {
            const float beside = std::max (k > 0 ? raw[k - 1] : 0.0f, k + 1 < kNumBands ? raw[k + 1] : 0.0f);
            const float target = std::max (raw[k], dlgSpread * beside);
            float& e = env[k];
            e += (target > e ? dlgAttackC : dlgReleaseC) * (target - e);
            if (! (e >= 1.0e-3f) && target <= 0.0f)
                e = 0.0f;                   // also catches NaN
            any = any || e > 0.0f;
            wanted[k] *= 1.0f - dlgAmount * e;
        }
        dlgActive[c] = any;
    }

    /** Steady detection: detector c's steady level, from the band powers p it works from. Kept
        going under the Threshold too, so that it is ready when the programme comes up. A slot that
        was not run at the last tick (Steady just chosen, the link mode changed, after reset())
        starts from the band's slow average when keepPeaks brought it up to date at the last tick
        (the slot's detector ran). Otherwise (a channel that was not its own detector while Linked,
        whose slow average is as old as the last time it was) it starts from the band powers now,
        and the 50 ms smoothing and the rise and fall take it from there. */
    void steadyTick (int c, const float* p) noexcept
    {
        float* sp = steadyPow[c];
        float* sd = steadyDb[c];
        float level[kNumBands];
        float loudest = -200.0f;
        if (steadyStamp[c] + 1u != tickCount)
        {
            const bool slowFresh = slowStamp[c] + 1u == tickCount;
            for (int k = 0; k < kNumBands; ++k)
                sp[k] = slowFresh && slowPower[c][k] > 0.0f ? slowPower[c][k] : p[k];
            for (int k = 0; k < kNumBands; ++k)
            {
                level[k] = 10.0f * std::log10 (sp[k] + 1.0e-14f) + kFullScaleSineOffsetDb;
                loudest = std::max (loudest, level[k]);
            }
            for (int k = 0; k < kNumBands; ++k)
                sd[k] = std::max (level[k], loudest - 70.0f);
        }
        else
        {
            for (int k = 0; k < kNumBands; ++k)
            {
                sp[k] += steadySmoothC * (p[k] - sp[k]);
                if (! (sp[k] >= 0.0f))
                    sp[k] = 0.0f;           // also catches NaN
                level[k] = 10.0f * std::log10 (sp[k] + 1.0e-14f) + kFullScaleSineOffsetDb;
                loudest = std::max (loudest, level[k]);
            }
            const float floorDb = loudest - 70.0f;
            for (int k = 0; k < kNumBands; ++k)
            {
                const float l = std::max (level[k], floorDb);
                sd[k] += (l > sd[k] ? steadyRiseC : steadyFallC) * (l - sd[k]);
                if (! std::isfinite (sd[k]))
                    sd[k] = l;
            }
        }
        steadyStamp[c] = tickCount;
    }

    /** One detector: the impact guard, what each band should come down by, the dialogue, where the
        notches go, and the smoothing. `c` is the slot its state is kept in, `p` the band powers. */
    void detect (int c, const float* p, float activity, float* wanted, const float* dialogue = nullptr) noexcept
    {
        guardTick (c, p, activity > 0.0f);
        const bool steady = params.detection == Detection::steady;
        if (steady)
            steadyTick (c, p);
        if (activity > 0.0f) wantedCuts (p, wanted, steady ? steadyDb[c] : nullptr); else std::fill (wanted, wanted + kNumBands, 0.0f);
        if (! tune.handOver)
            for (int k = 0; k < kNumBands; ++k) wanted[k] *= activity;
        if (dlgAny)
            dialogueTick (c, p, dialogue != nullptr ? dialogue : dlgD, wanted);
        keepPeaks (c, wanted, p);
        smoothCuts (c, wanted);
    }

    /** Channel `to` gets what detector `from` decided. */
    void shareDetector (int from, int to) noexcept
    {
        std::memcpy (cut[to], cut[from], sizeof (float) * kNumBands);
        guards[to].held = guards[from].held;
        guards[to].easing = guards[from].easing;
        if (guards[from].easing)
            std::memcpy (guards[to].over, guards[from].over, sizeof (float) * kNumBands);
        dlgActive[to] = dlgActive[from];
        if (dlgActive[from])
            std::memcpy (dlgEnv[to], dlgEnv[from], sizeof (float) * kNumBands);
        dlgTaking[to] = dlgTaking[from];
        if (dlgTaking[from] || dlgActive[from])
            std::memcpy (dlgTaken[to], dlgTaken[from], sizeof (float) * kNumBands);
    }

    void tick() noexcept
    {
        updateDerived();
        ++tickCount;

        float wanted[kNumBands];
        float shared[kNumBands];
        // Unlinked is a link of nothing. Grouped needs more than two channels to differ from linked.
        const float link = params.linkMode == LinkMode::unlinked ? 0.0f : std::clamp (params.link, 0.0f, 1.0f);
        const bool grouped = params.linkMode == LinkMode::grouped && activeChannels > 2;
        // Only the channels this call is processing, and of those not an LFE. `lead` is the
        // first of them; with no LFE it is channel 0.
        const int na = numAnalysed;
        const int* an = analysed;

        // Backstop: if anything non-finite got into a channel's analysis, clear it.
        for (int a = 0; a < na; ++a)
        {
            const int c = an[a];
            if (! std::isfinite (programPower[c]) || ! std::isfinite (power[c][kNumBands / 2]))
            {
                for (auto& section : aaZ[c / kLanes])
                    for (auto& value : section)
                        value[c % kLanes] = 0.0f;
                programPower[c] = 0.0f;
                for (int k = 0; k < kBankBands; ++k)
                    s1a[c][k] = s2a[c][k] = s1b[c][k] = s2b[c][k] = power[c][k] = 0.0f;
                for (int k = 0; k < kNumBands; ++k)
                    slowPower[c][k] = 0.0f;
            }
        }
        if (! std::isfinite (foldPowerL) || ! std::isfinite (foldPowerR))
            foldPowerL = foldPowerR = 0.0f;

        for (int k = 0; k < kNumBands; ++k)
        {
            float m = power[lead][k];
            for (int a = 1; a < na; ++a)
                m = std::max (m, power[an[a]][k]);
            shared[k] = m;
            frame.levelDb[k] = std::max (-120.0f, 10.0f * std::log10 (m + 1.0e-14f) + kFullScaleSineOffsetDb);
        }

        // How busy the programme is. De-Honk is for the loud, dense moments. With one or two
        // channels: the louder channel. With more: the louder side of the stereo fold-down (see
        // setLayout), so that the Threshold means the same on a surround master as on its stereo
        // version. Taking the loudest single channel would read several dB low on a surround mix
        // spread over many speakers; adding every channel's power would read high.
        float prog;
        if (activeChannels <= 2)
        {
            prog = programPower[lead];
            for (int a = 1; a < na; ++a)
                prog = std::max (prog, programPower[an[a]]);
        }
        else
        {
            prog = std::max (foldPowerL, foldPowerR);
        }

        // With a sidechain switched in, the threshold listens to that instead.
        const bool keyed = params.keyOn && keyPresent;
        dialoguePowers (! staticOn && na > 1 && (grouped || link < 0.999f));
        frame.keyed = keyed || dlgOnNow;
        frame.keyDb = keyPresent ? std::max (-120.0f, 10.0f * std::log10 (keyPower + 1.0e-14f)) : -120.0f;
        if (keyed)
            prog = keyPower;
        const float progNow = std::max (-120.0f, 10.0f * std::log10 (prog + 1.0e-14f));
        programDb += (progNow > programDb ? levelAttackC : levelReleaseC) * (progNow - programDb);
        const bool always = params.thresholdDb <= kAlwaysBelowDb;
        if (! always)   // do not sink far below the threshold in a quiet stretch: the climb back is the delay
            programDb = std::max (programDb, params.thresholdDb - 12.0f);
        const float activity = always ? 1.0f
                             : smoothstep ((programDb - params.thresholdDb) / std::max (0.1f, tune.thresholdKneeDb));
        frame.programDb = programDb;
        frame.activity = activity;

        // Max Cut moves over a few milliseconds when it is changed, and every cut moves under it.
        // So does the amount of the impact guard.
        {
            const float target = std::clamp (params.maxCutDb, 0.0f, kCeilingMaxDb);
            if (ceilingSnap || std::abs (target - ceilingNow) < 1.0e-3f)
                ceilingNow = target;
            else
                ceilingNow += ceilingC * (target - ceilingNow);
            const float amount = std::clamp (params.impactGuard, 0.0f, 1.0f);
            if (ceilingSnap || std::abs (amount - guardAmountNow) < 1.0e-3f)
                guardAmountNow = amount;
            else
                guardAmountNow += ceilingC * (amount - guardAmountNow);
            ceilingSnap = false;
        }

        if (staticOn)
        {
            for (int a = 0; a < na; ++a)
                for (int k = 0; k < kNumBands; ++k)
                    cut[an[a]][k] = (k >= kFirstFilterBand && k <= kLastFilterBand) ? std::clamp (staticCut[k], 0.0f, ceilingNow) : 0.0f;
        }
        else if (grouped)
        {
            // Grouped: as linked, within each group. The front pair, the centre, the surrounds and
            // the heights each get one detector, on the louder of the group's channels per band,
            // kept in the slot of the group's first channel.
            float groupPower[kNumBands], mixed[kNumBands], groupDlg[kNumBands];
            for (int g = 0; g < kNumGroups; ++g)
            {
                int members[kMaxChannels], nm = 0;
                for (int a = 0; a < na; ++a)
                    if (groupOf[an[a]] == g)
                        members[nm++] = an[a];
                if (nm == 0)
                    continue;
                for (int k = 0; k < kNumBands; ++k)
                {
                    float m = power[members[0]][k];
                    for (int i = 1; i < nm; ++i)
                        m = std::max (m, power[members[i]][k]);
                    groupPower[k] = m;
                }
                // The dialogue as it stands in this group's channels: a group it is not in keeps its cuts.
                if (dlgOnNow)
                    for (int k = 0; k < kNumBands; ++k)
                    {
                        float m = dlgDc[members[0]][k];
                        for (int i = 1; i < nm; ++i)
                            m = std::max (m, dlgDc[members[i]][k]);
                        groupDlg[k] = m;
                    }
                if (link >= 0.999f || nm == 1)
                {
                    detect (members[0], groupPower, activity, wanted, groupDlg);
                    for (int i = 1; i < nm; ++i)
                        shareDetector (members[0], members[i]);
                }
                else
                {
                    for (int i = 0; i < nm; ++i)
                    {
                        const int c = members[i];
                        for (int k = 0; k < kNumBands; ++k)
                            mixed[k] = link * groupPower[k] + (1.0f - link) * power[c][k];
                        if (dlgOnNow)
                        {
                            float own[kNumBands];
                            for (int k = 0; k < kNumBands; ++k)
                                own[k] = link * groupDlg[k] + (1.0f - link) * dlgDc[c][k];
                            detect (c, mixed, activity, wanted, own);
                        }
                        else
                            detect (c, mixed, activity, wanted);
                    }
                }
            }
        }
        else if (link >= 0.999f || na == 1)
        {
            // Linked: one detector, on the louder channel per band, and every channel gets its notches.
            detect (lead, shared, activity, wanted);
            for (int a = 1; a < na; ++a)
                shareDetector (lead, an[a]);
        }
        else
        {
            float mixed[kNumBands];
            for (int a = 0; a < na; ++a)
            {
                const int c = an[a];
                for (int k = 0; k < kNumBands; ++k)
                    mixed[k] = link * shared[k] + (1.0f - link) * power[c][k];
                if (dlgOnNow)
                {
                    float own[kNumBands];
                    for (int k = 0; k < kNumBands; ++k)
                        own[k] = link * dlgD[k] + (1.0f - link) * dlgDc[c][k];
                    detect (c, mixed, activity, wanted, own);
                }
                else
                    detect (c, mixed, activity, wanted);
            }
        }

        // The Threshold. Before the hand-over it scaled what the detector asked for, so a notch
        // closed under the Threshold at the release. Now it is a gain of its own on what reaches
        // the notches: it closes them as the programme falls, whatever Release is, and the
        // detector keeps what it had for when the programme comes back. It opens at the attack
        // and closes over thresholdCloseMs.
        if (tune.handOver && ! staticOn)
        {
            actGain += (activity > actGain ? attackC : closeC) * (activity - actGain);
            if (! (actGain >= 1.0e-3f) && activity <= 0.0f)
                actGain = 0.0f;         // a one-pole never quite arrives (also catches NaN)
        }
        else
            actGain = 1.0f;

        float total = 0.0f, heldMost = 0.0f;
        for (int a = 0; a < na; ++a)
        {
            auto& g = guards[an[a]];
            if (staticOn)
            {
                g.held = 0.0f;
                g.easing = false;
            }
            heldMost = std::max (heldMost, g.held);
            float* og = outGain[an[a]];
            if (g.easing)
                for (int k = 0; k < kNumBands; ++k)
                    og[k] = actGain * (1.0f - guardEaseShare * g.over[k]);
            else
                std::fill (og, og + kNumBands, actGain);
            // A band that is the dialogue's: the notch that is open there is eased off by what
            // the detector's state has not let go of yet (see dialogueTick).
            if (dlgActive[an[a]] && ! staticOn)
            {
                const float* e = dlgEnv[an[a]];
                const float* h = dlgTaken[an[a]];
                for (int k = 0; k < kNumBands; ++k)
                    og[k] *= std::min (1.0f, (1.0f - dlgAmount * e[k]) / (1.0f - dlgAmount * h[k] + 1.0e-6f));
            }
        }
        frame.guard = heldMost;
        if (dlgAny)
        {
            for (int k = 0; k < kNumBands; ++k)
            {
                float m = 0.0f;
                for (int a = 0; a < na; ++a)
                    if (dlgActive[an[a]])
                        m = std::max (m, dlgAmount * dlgEnv[an[a]][k]);
                frame.dialogue[k] = m;
            }
            dlgShown = true;
        }
        else if (dlgShown)
        {
            for (int k = 0; k < kNumBands; ++k)
                frame.dialogue[k] = 0.0f;
            dlgShown = false;
        }
        dialogueOver (shared);
        frame.dialogueTrimDb = dlgOnNow ? 10.0f * std::log10 (std::max (1.0e-8f, dlgGainMost)) : 0.0f;
        frame.keyMatch = dlgOnNow ? dlgFit : 0.0f;
        frame.keyOver = dlgOver;
        frame.snapDialogue = snapDlgPeak;
        snapDlgPeak = 0.0f;
        for (int k = 0; k < kNumBands; ++k)
        {
            float m = cut[lead][k] * outGain[lead][k];
            for (int a = 1; a < na; ++a)
                m = std::max (m, cut[an[a]][k] * outGain[an[a]][k]);
            frame.cutDb[k] = m;
            total = std::max (total, m);
        }
        frame.totalCutDb = total;
        frame.snapCutDb = clickPeakCutDb;
        frame.diffOffsetDb = diffGainTo == 1.0f ? 0.0f : std::clamp (8.6858896f * std::log (diffGainTo), -9.0f, 0.0f);

        // Wash: of the energy in the range, how much is not in peaks. A band counts as a peak when
        // it has stood above the bands a sixth to a third of an octave either side for a while (the
        // same test that earns a notch). The rest is wash.
        {
            const float* slow = slowPower[lead];
            const int nearB = std::max (1, kBandsPerOctave / 6), farB = std::max (nearB, kBandsPerOctave / 3);
            float inRange = 0.0f, peaks = 0.0f;
            for (int k = farB; k + farB < kNumBands; ++k)
            {
                if (focusW[k] <= 0.0f)
                    continue;
                float around = 0.0f;
                for (int d = nearB; d <= farB; ++d)
                    around += slow[k - d] + slow[k + d];
                around /= (float) (2 * (farB - nearB + 1));
                inRange += slow[k];
                if (slow[k] > around * std::exp (kLn10Over10 * tune.peakMinDb))
                    peaks += slow[k] - around;          // what stands above its surroundings
            }
            const float now = inRange > 1.0e-12f ? std::clamp (1.0f - peaks / inRange, 0.0f, 1.0f) : 0.0f;
            washSlow += onePoleCoef (2.0, fs / tickPeriod) * (now - washSlow);
            frame.wash = washSlow;
        }

        // The note profile: band power by note, across octaves, over several seconds.
        {
            const float coef = onePoleCoef ((double) std::max (0.5f, tune.keySeconds), fs / tickPeriod);
            float now[12] {};
            for (int k = 0; k < kNumBands; ++k)
                now[(k + 4) % 12] += shared[k];          // band 0 is E, four semitones above C
            for (int n = 0; n < 12; ++n)
            {
                pitchClass[n] += coef * (now[n] - pitchClass[n]);
                frame.pitchClass[n] = pitchClass[n];
            }
        }
        clickPeakCutDb = 0.0f;
        for (int m = 0; m < kNumModules; ++m)
            frame.moduleCutDb[m] = 0.0f;
        for (int k = 0; k < kNumBands; ++k)
            if (moduleOfBand[k] >= 0)
                frame.moduleCutDb[moduleOfBand[k]] = std::max (frame.moduleCutDb[moduleOfBand[k]], frame.cutDb[k]);
        ++frameCount;

        std::fill (std::begin (targetKnown), std::end (targetKnown), false);
        for (int a = 0; a < na; ++a)
            setFilterTargets (an[a]);

        // Which notches are the same on every treated channel until the next tick: the same
        // coefficients and the same ramps, so they stay the same sample by sample.
        if (activeChannels > 2)
        {
            for (int j = 0; j < kNumFilters; ++j)
            {
                const auto& f0 = filt[lead][j];
                bool same = true;
                for (int a = 1; a < na && same; ++a)
                {
                    const auto& f = filt[an[a]][j];
                    same = f.active == f0.active && ! changed (f.a1, f0.a1) && ! changed (f.m1, f0.m1)
                        && ! changed (f.dA1, f0.dA1) && ! changed (f.dM1, f0.dM1);
                }
                filterShared[j] = same;
            }
            sharedFlagsChannels = activeChannels;
        }
    }

    //============================================================================== click module
    void resetClick() noexcept
    {
        clickDet = {};
        for (auto& d : lowerDet) d = {};
        for (auto& d : upperDet) d = {};
        for (auto& c : clickPath) c = {};
        clickSpike = lowerSpike = upperSpike = {};
        clickGain = 0.0f;
        clickPeakCutDb = 0.0f;
        // The running levels start from nothing, so at first everything looks like a spike.
        clickWarmUp = (int) (0.003 * (double) std::max (1.0f, tune.clickBodyMs) * fs);
    }

    /** `mono` is the signal the listener is about to hear (sum of the channels). Writes, per
        sample, how much of the click band to take out (0..1). With `listening` false no new cut
        starts and whatever is open is let go, which is how the module switches off without a jump. */
    void detectClicks (const float* mono, float* amount, int n, bool listening) noexcept
    {
        const float margin = tune.clickMarginDb, neighbour = tune.clickNeighbourDb;
        const float slope = std::clamp (params.depth, 0.0f, 1.0f) * tune.clickSlope;
        const float ceiling = std::max (0.0f, tune.clickMaxCutDb);
        float deepest = clickPeakCutDb;

        for (int i = 0; i < n; ++i)
        {
            float x = mono[i];
            if (! (std::abs (x) < 1.0e8f))
                x = 0.0f;
            const float c = clickSpike.tick (clickDet.tick (clickBand, x), clickFastRel, clickSlowC);
            const float l = lowerSpike.tick (lowerDet[1].tick (lowerBand, lowerDet[0].tick (lowerBand, x)), clickFastRel, clickSlowC);
            const float u = upperSpike.tick (upperDet[1].tick (upperBand, upperDet[0].tick (upperBand, x)), clickFastRel, clickSlowC);

            float target = 0.0f;
            // Cheap test first: only take logs when the band is spiking at all.
            if (listening && c > 2.0f && clickSpike.fast > 1.0e-4f)
            {
                const float spike = 20.0f * std::log10 (c);
                const float around = 20.0f * std::log10 (std::max (l, u)) + neighbour;
                const float cutDb = std::min (ceiling, slope * std::max (0.0f, spike - std::max (margin, around)));
                if (cutDb > 0.0f)
                {
                    target = 1.0f - std::exp (-0.11512925f * cutDb);      // 1 - 10^(-dB/20)
                    deepest = std::max (deepest, cutDb);
                }
            }
            clickGain += (target > clickGain ? clickAttackC : clickReleaseC) * (target - clickGain);
            if (! (clickGain >= 1.0e-5f))
                clickGain = 0.0f;
            amount[i] = clickGain;
        }
        clickPeakCutDb = deepest;
    }

    /** detectClicks() with the dialogue stem to hand: `keyMono` is the key folded to one channel,
        in time with `mono`. The key goes through Snap's own band, and where its power there (at
        the gain the dialogue has in the mix) accounts for the mix's, the click is the dialogue's
        (a consonant) and is left alone by Dialogue Amount times that share. */
    void detectClicksKeyed (const float* mono, const float* keyMono, float* amount, int n, bool listening) noexcept
    {
        const float margin = tune.clickMarginDb, neighbour = tune.clickNeighbourDb;
        const float slope = std::clamp (params.depth, 0.0f, 1.0f) * tune.clickSlope;
        const float ceiling = std::max (0.0f, tune.clickMaxCutDb);
        const float keyGain = dlgSnapGainSq, amt = dlgAmount;
        float deepest = clickPeakCutDb, held = snapDlgPeak;

        for (int i = 0; i < n; ++i)
        {
            float x = mono[i];
            if (! (std::abs (x) < 1.0e8f))
                x = 0.0f;
            const float band = clickDet.tick (clickBand, x);
            const float c = clickSpike.tick (band, clickFastRel, clickSlowC);
            const float l = lowerSpike.tick (lowerDet[1].tick (lowerBand, lowerDet[0].tick (lowerBand, x)), clickFastRel, clickSlowC);
            const float u = upperSpike.tick (upperDet[1].tick (upperBand, upperDet[0].tick (upperBand, x)), clickFastRel, clickSlowC);

            const float kb = keyClickDet.tick (clickBand, keyMono[i]);
            const float mp = band * band, kp = kb * kb;
            snapMixPow += (mp > snapMixPow ? snapDlgAtkC : snapDlgRelC) * (mp - snapMixPow);
            snapKeyPow += (kp > snapKeyPow ? snapDlgAtkC : snapDlgRelC) * (kp - snapKeyPow);
            float share = 0.0f;
            const float d = keyGain * snapKeyPow;
            if (d > 1.0e-12f)
            {
                const float t = std::clamp ((d - dlgLo * snapMixPow) * dlgInvSpan / (snapMixPow + 1.0e-20f), 0.0f, 1.0f);
                share = amt * t * t * (3.0f - 2.0f * t);
            }

            float target = 0.0f;
            if (listening && c > 2.0f && clickSpike.fast > 1.0e-4f)
            {
                const float spike = 20.0f * std::log10 (c);
                const float around = 20.0f * std::log10 (std::max (l, u)) + neighbour;
                const float cutDb = std::min (ceiling, slope * std::max (0.0f, spike - std::max (margin, around)));
                if (cutDb > 0.0f)
                {
                    target = 1.0f - std::exp (-0.11512925f * cutDb);      // 1 - 10^(-dB/20)
                    if (share > 0.0f)
                    {
                        held = std::max (held, share);
                        deepest = std::max (deepest, -8.6858896f * std::log (std::max (1.0e-6f, 1.0f - target * (1.0f - share))));
                    }
                    else
                        deepest = std::max (deepest, cutDb);
                }
            }
            clickGain += (target > clickGain ? clickAttackC : clickReleaseC) * (target - clickGain);
            if (! (clickGain >= 1.0e-5f))
                clickGain = 0.0f;
            amount[i] = clickGain * (1.0f - share);
        }
        if (! std::isfinite (snapMixPow) || ! std::isfinite (snapKeyPow))
        {
            snapMixPow = snapKeyPow = 0.0f;
            keyClickDet = {};
        }
        clickPeakCutDb = deepest;
        snapDlgPeak = held;
    }

    //============================================================================== surround output
    // With more than two channels the treated signal is worked out for every channel first, so
    // that Snap's cut can run on four channels side by side, then each channel's output is
    // written in a loop of its own (it vectorises) and the level match's sums are taken four at a
    // time (which changes only the rounding of Diff's offset). Each channel otherwise gets the
    // same arithmetic as in the one- and two-channel loop in process().

    /** The notches and Snap's cut of every treated channel into wetBuf. */
    void treatSurround (int n, bool click, const float* clickAmount) noexcept
    {
        for (int a = 0; a < numAnalysed; ++a)
        {
            const int c = analysed[a];
            std::memcpy (wetBuf[c], dry[c], sizeof (float) * (size_t) n);
        }

        // Each notch in turn, on every channel. When the channels' notch is the same (always so
        // when linked), it runs on four channels side by side; otherwise channel by channel.
        // Either way each channel's notches run in the same order as in runFilters().
        const bool flagsValid = sharedFlagsChannels == activeChannels;
        const int groups = (numAnalysed + kLanes - 1) / kLanes;
        for (int j = 0; j < kNumFilters; ++j)
        {
            if (flagsValid && filterShared[j])
            {
                if (! filt[lead][j].active)
                    continue;
                switch (groups)
                {
                    case 1:  runFilterLanes<1> (j, n); break;
                    case 2:  runFilterLanes<2> (j, n); break;
                    default: runFilterLanes<3> (j, n); break;
                }
            }
            else
            {
                for (int a = 0; a < numAnalysed; ++a)
                    runFilter (analysed[a], j, wetBuf[analysed[a]], n);
            }
        }

        if (click)
        {
            switch ((numAnalysed + kLanes - 1) / kLanes)
            {
                case 1:  clickCut<1> (n, clickAmount); break;
                case 2:  clickCut<2> (n, clickAmount); break;
                default: clickCut<3> (n, clickAmount); break;
            }
        }
    }

    /** Channel c's output from wetBuf and the delayed input, and its share of the level match. */
    void outputSurround (int c, float* x, int n, const float* gw, const float* gd, float& wetSq, float& drySq) noexcept
    {
        const float* wet = wetBuf[c];
        const float* d = dry[c];
        for (int i = 0; i < n; ++i)
            x[i] = gw[i] * wet[i] + gd[i] * d[i];

        float sw[4] {}, sd[4] {};
        int i = 0;
        for (; i + 4 <= n; i += 4)
            for (int j = 0; j < 4; ++j)
            {
                sw[j] += wet[i + j] * wet[i + j];
                sd[j] += d[i + j] * d[i + j];
            }
        for (; i < n; ++i)
        {
            sw[0] += wet[i] * wet[i];
            sd[0] += d[i] * d[i];
        }
        wetSq += (sw[0] + sw[1]) + (sw[2] + sw[3]);
        drySq += (sd[0] + sd[1]) + (sd[2] + sd[3]);
    }

    /** Snap's cut on G groups of kLanes treated channels at once (the same band filter on each,
        state per channel). Lanes past the last channel work on silence in a buffer of their own. */
    template <int G>
    void clickCut (int n, const float* clickAmount) noexcept
    {
        constexpr int W = G * kLanes;
        BandState st[W];
        float* w[W];
        for (int l = 0; l < W; ++l)
        {
            if (l < numAnalysed)
            {
                st[l] = clickPath[analysed[l]];
                w[l] = wetBuf[analysed[l]];
            }
            else
            {
                st[l] = {};
                w[l] = spareLane[l];
                std::fill (w[l], w[l] + n, 0.0f);
            }
        }
        for (int i = 0; i < n; ++i)
            for (int l = 0; l < W; ++l)
            {
                float& v = w[l][i];
                v -= clickAmount[i] * st[l].tick (clickBand, v);
            }
        for (int l = 0; l < numAnalysed && l < W; ++l)
            clickPath[analysed[l]] = st[l];
    }

    //============================================================================== dialogue sidechain, analysis
    /** The key's first channels through the low-pass, the decimator (in step with the channels')
        and the band bank, exactly as a channel goes. */
    void keyAnalysis (const float* const* key, int done, int n, int phase) noexcept
    {
        for (int l = 0; l < dlgLanes; ++l)
        {
            const float* in = key[l] + done;
            BiquadState z[4] = { keyAa[l][0], keyAa[l][1], keyAa[l][2], keyAa[l][3] };
            float* out = keyDecimated[l];
            int ph = phase, m = 0;
            for (int i = 0; i < n; ++i)
            {
                float x = in[i];
                if (! (std::abs (x) < 1.0e8f))
                    x = 0.0f;
                if (useAa)
                    for (int st = 0; st < 4; ++st)
                        x = tickBiquad (aa[st], z[st], x);
                if (ph == 0)
                    out[m++] = x;
                if (++ph >= decim)
                    ph = 0;
            }
            for (int st = 0; st < 4; ++st)
                keyAa[l][st] = z[st];
            bandBankKeep (keyS1a[l], keyS2a[l], keyS1b[l], keyS2b[l], keyPow[l], out, std::min (m, kDetectEvery), keyY[l], keyQ[l]);
        }
        keyProducts (std::min (numDecimated, kDetectEvery));
    }

    /** Diff's share of a channel's output: x is what the channel would play, d its untreated signal. */
    static void diffOutput (float* x, const float* d, int n, const float* df, const float* dg, bool full) noexcept
    {
        if (full)
            for (int i = 0; i < n; ++i)
                x[i] = dg[i] * d[i];
        else
            for (int i = 0; i < n; ++i)
                x[i] += df[i] * (dg[i] * d[i] - x[i]);
    }

    //============================================================================== audio path
    int latencyFor (float lookaheadMs) const noexcept
    {
        const float ms = std::isfinite (lookaheadMs) ? std::clamp (lookaheadMs, 0.0f, kMaxLookaheadMs) : 0.0f;
        return std::max (0, (int) std::lround ((double) ms * 0.001 * fs));
    }

    /** Any value that is not a number is replaced by its default, so that one bad value from
        the host cannot leave the core stuck. */
    void setParams (const Params& in) noexcept
    {
        const Params d;
        Params q = in;
        auto fix = [] (float& v, float fallback) { if (! std::isfinite (v)) v = fallback; };
        fix (q.depth, d.depth); fix (q.sharpness, d.sharpness); fix (q.speed, d.speed);
        fix (q.thresholdDb, d.thresholdDb); fix (q.link, d.link); fix (q.lookaheadMs, d.lookaheadMs);
        fix (q.mix, d.mix); fix (q.trimDb, d.trimDb);
        fix (q.releaseMs, d.releaseMs); fix (q.maxCutDb, d.maxCutDb); fix (q.impactGuard, d.impactGuard);
        for (int e = 0; e < kNumEdges; ++e)
            fix (q.edgeHz[e], d.edgeHz[e]);
        fix (q.dialogueAmount, d.dialogueAmount);
        for (int m = 0; m < kNumModules; ++m)
        {
            fix (q.rangeAttackMs[m], d.rangeAttackMs[m]);
            fix (q.rangeReleaseMs[m], d.rangeReleaseMs[m]);
        }
        if (q.linkMode != LinkMode::grouped && q.linkMode != LinkMode::unlinked)
            q.linkMode = LinkMode::linked;
        if (q.detection != Detection::steady)
            q.detection = Detection::fast;
        params = q;
    }

    /** Notch j of channel c, as in runFilters(). */
    void runFilter (int c, int j, float* x, int n) noexcept
    {
        auto& f = filt[c][j];
        if (! f.active)
            return;

        const float g = fG[j], dA1 = f.dA1, dM1 = f.dM1;
        float a1 = f.a1, m1 = f.m1, ic1 = f.ic1, ic2 = f.ic2;
        for (int i = 0; i < n; ++i)
        {
            a1 += dA1;
            m1 += dM1;
            const float a2 = g * a1, a3 = g * a2;
            const float v3 = x[i] - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            x[i] += m1 * v1;
        }
        f.a1 = a1; f.m1 = m1; f.ic1 = ic1; f.ic2 = ic2;
    }

    /** Notch j on every treated channel at once, G groups of kLanes, for when its coefficients
        and their ramps are the same on all of them (filterShared). The coefficients are stepped
        once and used by every lane; each lane's own arithmetic is that of runFilter(). */
    template <int G>
    void runFilterLanes (int j, int n) noexcept
    {
        constexpr int W = G * kLanes;
        const auto& f0 = filt[lead][j];
        const float g = fG[j], dA1 = f0.dA1, dM1 = f0.dM1;
        float a1 = f0.a1, m1 = f0.m1;
        float ic1[W], ic2[W];
        float* x[W];
        for (int l = 0; l < W; ++l)
        {
            if (l < numAnalysed)
            {
                const auto& f = filt[analysed[l]][j];
                ic1[l] = f.ic1;
                ic2[l] = f.ic2;
                x[l] = wetBuf[analysed[l]];
            }
            else
            {
                ic1[l] = ic2[l] = 0.0f;
                x[l] = spareLane[l];
                std::fill (x[l], x[l] + n, 0.0f);
            }
        }
        for (int i = 0; i < n; ++i)
        {
            a1 += dA1;
            m1 += dM1;
            const float a2 = g * a1, a3 = g * a2;
            for (int l = 0; l < W; ++l)
            {
                const float v3 = x[l][i] - ic2[l];
                const float v1 = a1 * ic1[l] + a2 * v3;
                const float v2 = ic2[l] + a2 * ic1[l] + a3 * v3;
                ic1[l] = 2.0f * v1 - ic1[l];
                ic2[l] = 2.0f * v2 - ic2[l];
                x[l][i] += m1 * v1;
            }
        }
        for (int l = 0; l < numAnalysed && l < W; ++l)
        {
            auto& f = filt[analysed[l]][j];
            f.a1 = a1; f.m1 = m1; f.ic1 = ic1[l]; f.ic2 = ic2[l];
        }
    }

    void runFilters (int c, float* x, int n) noexcept
    {
        for (int j = 0; j < kNumFilters; ++j)
        {
            auto& f = filt[c][j];
            if (! f.active)
                continue;

            const float g = fG[j], dA1 = f.dA1, dM1 = f.dM1;
            float a1 = f.a1, m1 = f.m1, ic1 = f.ic1, ic2 = f.ic2;
            for (int i = 0; i < n; ++i)
            {
                a1 += dA1;
                m1 += dM1;
                const float a2 = g * a1, a3 = g * a2;
                const float v3 = x[i] - ic2;
                const float v1 = a1 * ic1 + a2 * v3;
                const float v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0f * v1 - ic1;
                ic2 = 2.0f * v2 - ic2;
                x[i] += m1 * v1;
            }
            f.a1 = a1; f.m1 = m1; f.ic1 = ic1; f.ic2 = ic2;
        }
    }

    void process (float* const* io, int numChannels, int numSamples, const float* const* key = nullptr, int numKeyChannels = 0) noexcept
    {
        if (! prepared || numSamples <= 0)
            return;

        const DenormalGuard guard;
        const int nch = std::min (numChannels, channels);
        if (nch <= 0)
            return;
        keyPresent = key != nullptr && numKeyChannels > 0;
        if (! keyPresent)
            keyPower = 0.0f;
        activeChannels = nch;
        numAnalysed = 0;
        lfeIn = false;
        for (int c = 0; c < nch; ++c)
        {
            lfeNow[c] = isLfe[c];
            if (isLfe[c])
                lfeIn = true;
            else
                analysed[numAnalysed++] = c;
        }
        if (numAnalysed == 0)
        {
            // setLayout() makes sure not every prepared channel is an LFE, but a call may pass
            // fewer channels than were prepared, and those few may all be LFEs. Then, as for an
            // all-LFE layout, treat them as ordinary channels: something must be analysed.
            for (int c = 0; c < nch; ++c)
            {
                lfeNow[c] = false;
                analysed[numAnalysed++] = c;
            }
            lfeIn = false;
        }
        lead = analysed[0];
        const bool surround = nch > 2;

        // Output mode. wet is the filtered signal, dry the delayed input.
        float targetWet, targetDry, targetLfe;
        {
            const float trim = std::pow (10.0f, std::clamp (params.trimDb, -24.0f, 24.0f) / 20.0f);   // Diff and Bypass win over Solo Honk
            const float mix = std::clamp (params.mix, 0.0f, 1.0f);
            if (params.bypass)                  { targetWet = 0.0f;         targetDry = 1.0f; }
            else if (params.soloHonk)           { targetWet = -trim * mix;  targetDry = trim * mix; }
            else                                { targetWet = trim * mix;   targetDry = trim * (1.0f - mix); }
            // An LFE's wet is its dry, so Mix makes no difference to it and Solo Honk silences it.
            targetLfe = params.bypass ? 1.0f : (params.soloHonk ? 0.0f : trim);
        }
        // Diff is a cross-fade of its own, from whatever the output is to the untreated signal
        // turned down to the treated one's loudness (an LFE: to the untreated signal as it is,
        // since it was not treated). Treated and untreated are the same signal but for the
        // notches, so the two are faded with gains that add up to one; gains that kept the power
        // constant would lift the level by 3 dB half-way. Bypass wins.
        const bool diffWanted = params.diff && ! params.bypass;
        const bool dlgWant = params.dialogueOn && keyPresent;
        if (dlgWant)
        {
            const int lanes = std::min (numKeyChannels, kKeyLanes);
            if (! dlgRunning || lanes != dlgLanes)
                clearKeyAnalysis();
            dlgLanes = lanes;
        }
        dlgRunning = dlgWant;
        const int wantedDelay = std::min (latencyFor (params.lookaheadMs), ringMask + 1 - kMaxChunk);
        if (snapGains)
        {
            gainWet = targetWet;
            gainDry = targetDry;
            gainLfe = targetLfe;
            delayNow = delayNext = wantedDelay;
            fadeLeft = 0;
            diffFade = diffFadeGoal = diffWanted ? 1.0f : 0.0f;
            diffFadeSlope = 0.0f;
            diffFadeAt = diffFadeLength;
            snapGains = false;
        }
        if (fadeLeft == 0 && wantedDelay != delayNow)
        {
            delayNext = wantedDelay;     // a change of lookahead cross-fades between the two delays
            fadeLeft = fadeLength;
        }

        float wet[kMaxChunk], gw[kMaxChunk], gd[kMaxChunk], gl[kMaxChunk], fade[kMaxChunk], clickAmount[kMaxChunk];
        float df[kMaxChunk], dg[kMaxChunk];     // Diff: how far in, and the gain on the untreated signal
        float keyMono[kMaxChunk];               // the dialogue stem folded to one channel, delayed as the channels are

        int done = 0;
        while (done < numSamples)
        {
            const int n = std::min ({ numSamples - done, samplesToTick, kMaxChunk });

            for (int i = 0; i < n; ++i)
            {
                gainWet += modeC * (targetWet - gainWet);
                gainDry += modeC * (targetDry - gainDry);
                gw[i] = gainWet;
                gd[i] = gainDry;
            }
            // A one-pole never quite arrives. Land on the target once it is inaudibly close, so
            // that Diff and Bypass give back the input exactly.
            if (std::abs (targetWet - gainWet) < 1.0e-5f) gainWet = targetWet;
            if (std::abs (targetDry - gainDry) < 1.0e-5f) gainDry = targetDry;
            if (lfeIn)
            {
                for (int i = 0; i < n; ++i)
                {
                    gainLfe += modeC * (targetLfe - gainLfe);
                    gl[i] = gainLfe;
                }
                if (std::abs (targetLfe - gainLfe) < 1.0e-5f) gainLfe = targetLfe;
            }

            if (fadeLeft > 0)
                for (int i = 0; i < n; ++i)
                    fade[i] = std::min (1.0f, 1.0f - (float) (fadeLeft - 1 - i) / (float) fadeLength);

            if (changed (diffWanted ? 1.0f : 0.0f, diffFadeGoal))
            {
                diffFadeGoal = diffWanted ? 1.0f : 0.0f;
                diffFadeStart = diffFade;
                diffFadeStartSlope = diffFadeSlope * (float) diffFadeLength;
                diffFadeAt = 0;
            }
            const bool diffMoving = diffFadeAt < diffFadeLength;
            const bool diffing = diffMoving || diffFade > 0.0f;
            const bool diffFull = ! diffMoving && diffFade >= 1.0f;
            if (diffing)
            {
                if (diffMoving)
                {
                    // A cubic from (start, its slope) to (goal, at rest).
                    const float p0 = diffFadeStart, m0 = diffFadeStartSlope, p1 = diffFadeGoal, inv = 1.0f / (float) diffFadeLength;
                    const float c2 = 3.0f * (p1 - p0) - 2.0f * m0, c3 = 2.0f * (p0 - p1) + m0;
                    int at = diffFadeAt;
                    for (int i = 0; i < n; ++i)
                    {
                        at = std::min (at + 1, diffFadeLength);
                        const float u = (float) at * inv;
                        df[i] = p0 + u * (m0 + u * (c2 + u * c3));
                    }
                    diffFadeAt = at;
                    const float u = (float) at * inv;
                    diffFade = at >= diffFadeLength ? p1 : df[n - 1];
                    diffFadeSlope = at >= diffFadeLength ? 0.0f : (m0 + u * (2.0f * c2 + 3.0f * u * c3)) * inv;
                }
                else
                    std::fill (df, df + n, diffFade);
                const float slope = (diffGainTo - diffGainFrom) / (float) n;
                for (int i = 0; i < n; ++i)
                    dg[i] = diffGainFrom + slope * (float) (i + 1);
            }

            antiAlias (io, done, n, nch, decimPhase);
            if (dlgRunning)
            {
                // The same bank, keeping each band's output for the chunk to set against the key's.
                for (int a = 0; a < numAnalysed; ++a)
                {
                    const int c = analysed[a];
                    bandBankKeep (s1a[c], s2a[c], s1b[c], s2b[c], power[c], decimated[c], std::min (numDecimated, kDetectEvery), bankY[c], nullptr);
                }
                keyAnalysis (key, done, n, decimPhase);
            }
            else
                for (int a = 0; a < numAnalysed; ++a)
                    bandBank (analysed[a], numDecimated);

            for (int c = 0; c < nch; ++c)
            {
                float* x = io[c] + done;
                if (c == 0 && keyPresent)
                {
                    // The sidechain's level: its loudest channel, the whole spectrum.
                    for (int i = 0; i < n; ++i)
                    {
                        float loudest = 0.0f;
                        for (int kc = 0; kc < numKeyChannels; ++kc)
                        {
                            const float v = key[kc][done + i];
                            loudest = std::max (loudest, std::abs (v) < 1.0e8f ? v * v : 0.0f);
                        }
                        keyPower += keyC * (loudest - keyPower);
                    }
                }

                float* d = dry[c];
                float* r = ring[c].data();
                int w = ringWrite;
                if (fadeLeft == 0)
                {
                    // The usual case: copy in and out in at most two pieces each, around the end
                    // of the ring. The ring is longer than the longest delay plus a chunk, so
                    // writing the whole chunk first cannot overwrite anything still to be read.
                    const int size = ringMask + 1;
                    const int in1 = std::min (n, size - w);
                    std::memcpy (r + w, x, sizeof (float) * (size_t) in1);
                    std::memcpy (r, x + in1, sizeof (float) * (size_t) (n - in1));
                    const int from = (w - delayNow) & ringMask;
                    const int out1 = std::min (n, size - from);
                    std::memcpy (d, r + from, sizeof (float) * (size_t) out1);
                    std::memcpy (d + out1, r, sizeof (float) * (size_t) (n - out1));
                }
                else
                {
                    for (int i = 0; i < n; ++i)
                    {
                        r[w] = x[i];
                        const float a = r[(w - delayNow) & ringMask], b = r[(w - delayNext) & ringMask];
                        d[i] = a + fade[i] * (b - a);
                        w = (w + 1) & ringMask;
                    }
                }
                if (surround && ! lfeNow[c])
                {
                    // A NaN, an infinity or an absurd value in a treated channel becomes silence
                    // here, as the analysis already treats it, so that it cannot get into the
                    // notches' state (which would leave the channel NaN for seconds) or into
                    // Diff's level match. Only with more than two channels, so that mono and stereo
                    // stay exactly as they were. An LFE is played as it comes and holds no state.
                    for (int i = 0; i < n; ++i)
                        d[i] = std::abs (d[i]) < 1.0e8f ? d[i] : 0.0f;
                }
            }
            if (surround)
                foldDown();

            if (dlgRunning)
            {
                // The key for Snap: folded to one channel and through the same delay as the channels.
                float* r = keyRing.data();
                const float* k0 = key[0] + done;
                const float* k1 = dlgLanes > 1 ? key[1] + done : nullptr;
                int w = ringWrite;
                for (int i = 0; i < n; ++i)
                {
                    float v = k1 != nullptr ? 0.5f * (k0[i] + k1[i]) : k0[i];
                    if (! (std::abs (v) < 1.0e8f))
                        v = 0.0f;
                    r[w] = v;
                    keyMono[i] = r[(w - delayNow) & ringMask];
                    w = (w + 1) & ringMask;
                }
            }

            // Switched off in the middle of a cut, the module finishes letting go before it stops.
            const bool click = params.snapOn || clickGain > 0.0f;
            if (click)
            {
                if (! clickWasOn)
                    resetClick();
                // The detector listens to the channels folded down to one (the LFE left out).
                if (surround)
                {
                    for (int i = 0; i < n; ++i)
                        wet[i] = 0.0f;
                    for (int a = 0; a < numAnalysed; ++a)
                    {
                        const int c = analysed[a];
                        const float g = foldMono[c];
                        for (int i = 0; i < n; ++i)
                            wet[i] += g * dry[c][i];
                    }
                }
                else if (numAnalysed > 1)
                {
                    for (int i = 0; i < n; ++i)
                        wet[i] = 0.5f * (dry[0][i] + dry[1][i]);
                }
                else
                {
                    for (int i = 0; i < n; ++i)
                        wet[i] = dry[lead][i];
                }
                if (dlgRunning && ! staticOn)
                    detectClicksKeyed (wet, keyMono, clickAmount, n, params.snapOn && clickWarmUp <= 0);
                else
                    detectClicks (wet, clickAmount, n, params.snapOn && clickWarmUp <= 0);
                clickWarmUp = std::max (0, clickWarmUp - n);
            }
            clickWasOn = click;

            if (surround)
                treatSurround (n, click, clickAmount);

            float wetSq = 0.0f, drySq = 0.0f;
            for (int c = 0; c < nch; ++c)
            {
                float* x = io[c] + done;
                const float* d = dry[c];
                if (lfeNow[c])
                {
                    for (int i = 0; i < n; ++i)
                        x[i] = gl[i] * d[i];
                    if (diffFull)
                        std::memcpy (x, d, sizeof (float) * (size_t) n);
                    else if (diffing)
                        for (int i = 0; i < n; ++i)
                            x[i] += df[i] * (d[i] - x[i]);
                    continue;
                }
                if (surround)
                {
                    outputSurround (c, x, n, gw, gd, wetSq, drySq);
                    if (diffing)
                        diffOutput (x, d, n, df, dg, diffFull);
                    continue;
                }
                std::memcpy (wet, d, sizeof (float) * (size_t) n);
                runFilters (c, wet, n);

                if (click)
                {
                    auto& st = clickPath[c];
                    for (int i = 0; i < n; ++i)
                        wet[i] -= clickAmount[i] * st.tick (clickBand, wet[i]);
                }

                // One loop and one running sum each, exactly as it always was. Rearranging this
                // (or the code around it) can change how the compiler vectorises the sums, and
                // with that their rounding, and mono and stereo must not change by a bit.
                for (int i = 0; i < n; ++i)
                {
                    wetSq += wet[i] * wet[i];
                    drySq += d[i] * d[i];
                    x[i] = gw[i] * wet[i] + gd[i] * d[i];
                }
                if (diffing)
                    diffOutput (x, d, n, df, dg, diffFull);
            }

            // How much quieter the treated signal is than the untreated one, over a few seconds: Diff
            // plays the untreated signal turned down by that much, so the comparison is fair.
            {
                // How much quieter: the power of each, averaged over diffMatchSeconds, and the
                // ratio of the two. Power, not decibels, so a loud moment counts for as much as it
                // weighs in what is heard; and all the time, so that where nothing is cut (under
                // the Threshold) the two come together and Diff is not turned down there. Both
                // averages start from nothing with the same coefficient, so their ratio is right
                // from the first moment.
                const float scale = 1.0f / (float) (n * numAnalysed);   // an LFE is the same both ways and is left out
                const float coef = onePoleCoef ((double) std::max (0.2f, tune.diffMatchSeconds), fs / (double) n);
                loudWet += coef * (wetSq * scale - loudWet);
                loudDry += coef * (drySq * scale - loudDry);
                if (! std::isfinite (loudWet) || ! std::isfinite (loudDry))
                    loudWet = loudDry = 0.0f;
                diffMatchDb = loudDry > 1.0e-9f ? std::clamp (10.0f * std::log10 ((loudWet + 1.0e-12f) / loudDry), -9.0f, 0.0f) : 0.0f;
                // The gain itself moves smoothly towards what that calls for, and in a straight
                // line through each chunk, whether Diff is on or not, so it is there when Diff is.
                const float target = diffMatchDb == 0.0f ? 1.0f : std::exp (0.11512925f * diffMatchDb);
                diffGainFrom = diffGainTo;
                diffGainTo += onePoleCoef (0.001 * (double) std::max (1.0f, tune.diffGainMs), fs / (double) n) * (target - diffGainTo);
                if (std::abs (target - diffGainTo) < 1.0e-6f)
                    diffGainTo = target;
            }

            if (fadeLeft > 0)
            {
                fadeLeft = std::max (0, fadeLeft - n);
                if (fadeLeft == 0)
                    delayNow = delayNext;
            }

            ringWrite = (ringWrite + n) & ringMask;
            decimPhase = (decimPhase + n) % decim;
            samplesToTick -= n;
            done += n;

            if (samplesToTick <= 0)
            {
                tick();
                samplesToTick = tickPeriod;
            }
        }
    }
};

//==================================================================================================
Core::Core() : impl (std::make_unique<Impl>()) {}
Core::~Core() = default;

void Core::prepare (double sampleRate, int, int numChannels)   { impl->prepare (sampleRate, numChannels, ChannelLayout {}); }
void Core::prepare (double sampleRate, int, int numChannels, const ChannelLayout& layout)
{
    impl->prepare (sampleRate, numChannels, layout);
}
void Core::reset()                                             { if (impl->prepared) impl->reset(); }
void Core::setParams (const Params& p) noexcept                { impl->setParams (p); }
void Core::setTuning (const Tuning& t) noexcept                { impl->tune = t; impl->dirtyAll = true; }
const Tuning& Core::tuning() const noexcept                    { return impl->tune; }

void Core::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    impl->process (channels, numChannels, numSamples);
}

void Core::processWithKey (float* const* channels, int numChannels, const float* const* key, int numKeyChannels, int numSamples) noexcept
{
    impl->process (channels, numChannels, numSamples, key, key != nullptr ? numKeyChannels : 0);
}

int Core::latencySamples() const noexcept                      { return latencySamplesFor (impl->params.lookaheadMs); }
int Core::latencySamplesFor (float lookaheadMs) const noexcept { return impl->latencyFor (lookaheadMs); }

int Core::numBands() const noexcept                            { return kNumBands; }
float Core::bandHz (int band) const noexcept
{
    return kAnalysisLowHz * std::pow (2.0f, (float) std::clamp (band, 0, kNumBands - 1) / (float) kBandsPerOctave);
}

const DisplayFrame& Core::displayFrame() const noexcept        { return impl->frame; }
std::uint32_t Core::frameCounter() const noexcept              { return impl->frameCount; }

void Core::debugSetStaticCuts (const float* cutDbPerBand) noexcept
{
    impl->staticOn = cutDbPerBand != nullptr;   // bands without a filter and depths over the ceiling are dropped in tick()
    if (cutDbPerBand != nullptr)
        for (int k = 0; k < kNumBands; ++k)
            impl->staticCut[k] = std::max (0.0f, cutDbPerBand[k]);
}

} // namespace dehonk

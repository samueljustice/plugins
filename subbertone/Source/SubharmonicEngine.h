#pragma once

#include "PitchDetector.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

/**
 * Subharmonic generator.
 *
 * Signal flow, all per-sample (nothing is stepped at block boundaries):
 *
 *   input --> 4th-order lowpass (detection band) --+--> PitchDetector (YIN)
 *                                                  |
 *                                                  +--> RMS follower --> sub amplitude
 *
 *   phase-continuous sine at f0/2 --+------------------------------------> clean
 *                                   +--> saturator --> remove DC + fundamental
 *                                        --> resonant multimode filter (env-swept) --> resonance drive --> harmonics
 *
 *   wet = amplitude * voicingGate * equalPower(clean, harmonics, distortion)  --> post lowpass --> lo cut
 *
 * The filter section borrows the idea behind distortion-filter effects such as
 * NI Driver: resonance picks out individual partials and is then driven into
 * saturation, and an envelope follower sweeps the cutoff per note. It sits after
 * the shaper because filtering a pure sine only changes its level.
 *
 * The "harmonic residual" is computed exactly: for a memoryless shaper driven
 * by a sine, the DC and fundamental Fourier coefficients depend only on the
 * drive, so they are measured numerically and subtracted. No phase-shifting
 * filters sit between the clean and distorted paths, so the subtraction is
 * sample-aligned.
 *
 * Input samples that are NaN, infinite or absurdly large (|x| >= 1e4) are treated
 * as silence, so a bad host buffer cannot poison the filter states.
 *
 * Phase Align (Parameters::phaseAlign, off by default). Classic adds the sub's partials
 * at whatever phase the oscillator happens to have, so at a partial the input also has
 * (the input's harmonic m sits on the sub's partial 2m) the wet cancels the input about
 * as often as it reinforces it. With Phase Align on:
 *   - the oscillator knows its phase phi, so the input's partial at k * phi (k = 1..16)
 *     is measured by demodulating the input with exp(-j k phi) and averaging over exactly
 *     one sub period (a boxcar whose zeros null every other partial on the sub's grid).
 *     The measurement lags by half a sub period; the audio is never delayed (no latency);
 *   - every generated partial k <= 16 that lands on a coherent, phase-stable partial of
 *     the input is turned onto the input's phase there: the H2..H16 overtones, the sub,
 *     and the Drive path's partials, through an exact additive correction (the Drive
 *     path is modelled per partial: shaper Fourier series x filter responses; exact up
 *     to 2 % Resonance, above that only the additive partials are aligned);
 *   - at a note start the input's partials are estimated from one input period of
 *     history and the sub's phase is chosen from that estimate (repeatable hits). Pitch
 *     and phase only jump where that is inaudible: a tail still sounding is faded out
 *     over 8 ms first, and a stale tail is faded out when a new sound arrives before its
 *     pitch is known (released when the note starts, or after 300 ms);
 *   - partial magnitudes are unchanged: only phases move. The pitch is the tracker's.
 * Switching it while audio plays is click-free and keeps the level: switched on, the
 * engine starts from Classic's exact output, measures, and once the alignment has
 * settled (two sub periods plus its smoothing, so 0.1-0.3 s) turns the partials onto the
 * input's phases along an arc over 50 ms (constant magnitude); with nothing sounding it
 * engages at once. Switched off, they turn back the same way over 50 ms and the engine
 * returns to the Classic code path (about 70 ms after the switch); until then note starts
 * are still handled as above, so quick on / off automation never fades a note in and
 * out. A session that never switches it on runs the Classic code path only: bit-identical
 * to the engine without Phase Align, at the same cost.
 *
 * No JUCE dependency, no allocation after prepare(): safe for the audio thread
 * and easy to test offline.
 */
class SubharmonicEngine
{
public:
    enum Saturation
    {
        Tape = 0,
        Valve,
        Console,
        Transformer,
        Rectifier,
        Wavefolder,
        NumSaturationTypes
    };

    struct Parameters
    {
        float distortion = 0.5f;          // 0..1
        int saturationType = Tape;
        float toneHz = 1000.0f;           // filter cutoff for the harmonics
        float resonance = 0.0f;           // 0..1
        int filterMode = 0;               // 0 lowpass, 1 bandpass, 2 notch
        float envAmountOctaves = 0.0f;    // cutoff sweep, -4..+4 octaves
        float envReleaseMs = 150.0f;      // envelope follower release
        float postLowpassHz = 20000.0f;   // lowpass on the whole wet signal
        float thresholdDb = -40.0f;       // input level gate
        float maxFundamentalHz = 250.0f;  // ignore pitches above this
        float glideMs = 12.0f;            // how smoothly the sub follows pitch changes
        float overtoneAmount = 0.0f;      // 0..1 master for H2..H16 (MaxxBass-style overtones of the sub)
        // Harmonic editor levels, 0..1, index 1..16 (index 0 unused). H1 is the sub itself.
        // Hk (k >= 2) is the k-th harmonic of the sub, relative to the unit sub level BEFORE the
        // Drive crossfade and the H1 level: 1.0 is as loud as the sub at Drive 0 % and H1 100 %.
        // This is intended: turning Drive up (which crossfades the clean sub towards the
        // saturator) or H1 down leaves the overtones where they are.
        std::array<float, 17> harmonicLevels { 0.0f, 1.0f, 0.6f, 0.4f, 0.3f, 0.24f, 0.2f };
        bool loCutOn = false;             // steep high-pass on the generated sub
        float loCutHz = 30.0f;            // 10..80 Hz
        // Phase Align (see the top of this file). false = Classic, today's sound, bit for bit and at no
        // extra cost. true = every generated partial that lands on a partial of the input is turned onto
        // the input's phase there, so the wet reinforces the input instead of cancelling it, and each
        // note's sub starts at a phase chosen from the input. Costs about a third more engine CPU while on
        // and about 330 KB per instance (allocated in prepare() either way). No added latency. May be
        // switched at any time while audio plays (click-free, see above).
        bool phaseAlign = false;
    };

    void prepare(double sampleRate);
    void reset();
    void setParameters(const Parameters& p);

    /** in: mono input. out: wet subharmonic signal. residual: harmonics only (for display, may be null). */
    void process(const float* in, float* out, float* residual, int numSamples);

    /** Detected input fundamental (Hz), 0 when unvoiced. */
    float getFundamental() const { return m_fundamental; }

    /** Linear RMS of the full-band input (smoothed). */
    float getInputRms() const { return m_inputRms; }

    /** Diagnostics: frequency the sub oscillator is currently playing, and the voicing gate (0..1). */
    float getSubFrequency() const { return static_cast<float>(std::exp(m_logFreq)); }
    float getGate() const { return m_gate; }
    const PitchDetector& getDetector() const { return m_detector; }

    /** Phase Align state, for tests and an editor display (updated about every 0.7 ms on the audio thread: read it
        there, like the getters above, and pass on copies). */
    struct PhaseAlignDiagnostics
    {
        bool running = false;             // the Phase Align code path is running (on, or turning back to Classic)
        float engaged = 0.0f;             // 0..1 how far the partials are turned onto the input's phases
        bool measurementValid = false;    // the one-period window is full and spans one period
        bool usingOnsetEstimate = false;  // still running on the note-start estimate
        std::uint32_t noteStarts = 0;     // note starts at which the sub's phase was chosen
        std::uint32_t ducks = 0;          // ... and corrections that faded a sounding tail out first (8 ms)
        std::uint32_t tailGuards = 0;     // stale tails faded out when a new sound arrived before its pitch
        std::uint32_t pitchCorrections = 0; // a new note's pitch corrected by the tracker: phase chosen again
        std::uint32_t handOvers = 0;      // returns to the Classic code path after switching off
        float alignWeight[17] {};         // 0..1 per partial k: how much it follows the input's phase
    };
    const PhaseAlignDiagnostics& getPhaseAlignDiagnostics() const { return m_paDiag; }

private:
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1 = 0, z2 = 0;

        void setLowpass(double fs, double fc, double q);
        void reset() { z1 = z2 = 0.0f; }

        float process(float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    // Zavalishin TPT state-variable lowpass: stays stable under per-sample modulation.
    struct SvfLowpass
    {
        float g = 0.0f, ic1 = 0.0f, ic2 = 0.0f;
        static constexpr float k = 1.41421356f; // Q = 0.707

        void setCutoff(float fc, float fs);
        void reset() { ic1 = ic2 = 0.0f; }

        float process(float x)
        {
            const float a1 = 1.0f / (1.0f + g * (g + k));
            const float a2 = g * a1;
            const float a3 = g * a2;
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            return v2;
        }
    };

public:
    // Shaper maths is public so it can be tested offline.
    struct ShaperCoefficients
    {
        float drive = 1.0f;
        float norm = 1.0f;
        float dc = 0.0f;
        float fundamental = 1.0f;
        float harmonicGain = 1.0f; // scales the residual to the same RMS as the clean sine
    };

    static float shape(float x, int type, float drive);
    static ShaperCoefficients computeShaper(float drive, int type);

private:
    float harmonicResidual(float s, const ShaperCoefficients& c, int type) const;

    void updateDetectionBand(float maxFundamentalHz);
    void onPitchEstimate();
    void startTypeFade(int newType);
    bool stateIsFinite() const;

    double m_sampleRate = 44100.0;
    float m_fs = 44100.0f;

    Parameters m_target;

    // Detection band and analysis
    Biquad m_bandLp1, m_bandLp2;
    float m_bandCutoff = 0.0f;
    PitchDetector m_detector;

    // Level tracking
    float m_inputMeanSquare = 0.0f;
    double m_dcIn = 0.0;          // DC blocker on the analysis copy of the input
    double m_dcOut = 0.0;
    double m_dcCoeff = 0.0;
    float m_inputRms = 0.0f;
    float m_levelCoeff = 0.0f;
    bool m_levelOpen = false;

    float m_bandEnv1 = 0.0f, m_bandEnv2 = 0.0f;
    float m_bandEnvCoeff = 0.0f;

    // Voicing
    float m_fundamental = 0.0f;
    float m_gate = 0.0f;
    float m_gateTarget = 0.0f;
    float m_gateAttack = 0.0f;
    float m_gateRelease = 0.0f;
    // Pitch tracker: follows small moves, needs big jumps (and octave flips) to
    // persist before accepting them unless they are unmistakable, and holds
    // through short detection dropouts.
    void resetTracker();
    void acceptPitch(float semitones);
    void setTrackPitch(float semitones);

    bool m_hasTrack = false;
    float m_trackSt = 0.0f;       // tracked fundamental, in semitones (12 * log2 Hz)
    float m_candidateSt = 0.0f;
    int m_candidateHops = 0;
    int m_candidateSureHops = 0;  // consecutive near-perfect YIN dips behind the current candidate
    int m_startHops = 0;
    int m_holdHops = 0;
    int m_jumpConfirmHops = 8;
    int m_octaveConfirmHops = 24;
    int m_sureConfirmHops = 3;
    int m_holdLimitHops = 50;
    float m_strongSt = 0.0f;      // pitch of the last strong estimate the track used
    int m_hopsSinceStrong = 0;
    int m_weakWindowHops = 20;    // weak guesses only count this soon after a strong one ...
    int m_weakLongHops = 100;     // ... or this long while most recent hops are voiced
    unsigned m_voicedHistory = 0; // one bit per hop, newest in bit 0
    bool m_newNoteTrack = false;  // the track started as a new note (sub faded) ...
    int m_trackAgeHops = 0;       // ... this many hops ago
    int m_newNoteFixHops = 20;
    float m_lastGlideMs = -1.0f;

    // Oscillator. Double precision so long glides at high sample rates land exactly.
    double m_phase = 0.0;
    double m_logFreq = 0.0;
    double m_logFreqTarget = 0.0;
    double m_glideCoeff = 0.0;

    // Smoothed parameters
    float m_paramCoeff = 0.0f;
    float m_distortion = 0.5f;
    float m_toneHz = 1000.0f;
    float m_postLowpassHz = 20000.0f;

    // Saturation, with a short crossfade when the type changes. A change that arrives
    // mid-fade either reverses the fade (back to the type it came from) or waits for it
    // to finish, so the output never jumps.
    int m_type = Tape;
    int m_previousType = Tape;
    float m_typeFade = 1.0f;
    float m_typeFadeStep = 0.0f;
    ShaperCoefficients m_shaper, m_previousShaper;
    int m_shaperCounter = 0;

    SvfLowpass m_postFilter;

    // Multimode TPT SVF: all three responses from one state, so the mode can be
    // crossfaded click-free and cutoff/resonance modulated per sample.
    struct SvfMultimode
    {
        float ic1 = 0.0f, ic2 = 0.0f;
        void reset() { ic1 = ic2 = 0.0f; }

        struct Out { float lp, bp, hp; };

        Out process(float x, float g, float k)
        {
            const float a1 = 1.0f / (1.0f + g * (g + k));
            const float a2 = g * a1;
            const float a3 = g * a2;
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            return { v2, v1, x - k * v1 - v2 };
        }
    };

    SvfMultimode m_harmonicFilter;

    // Lo cut: 8th-order Butterworth high-pass (48 dB/oct) as four TPT SVF stages, always
    // running. Off is the dry sub, as before. Switching needs care because the high-pass is
    // far out of phase with the dry sub near its cutoff (a plain crossfade dips by up to
    // 25 dB), and gliding a steep filter's cutoff quickly is not clean either (the level
    // swells or dips while it moves). So:
    //   - off: a 30 ms crossfade to the 4th-order Butterworth allpass at the same cutoff,
    //     which is within 11 degrees of the high-pass in phase at every frequency, so the
    //     crossfade is a pure level change; once the sub has gone silent the allpass hands
    //     back to the dry sub (inaudible then);
    //   - on, from the allpass: the same crossfade back;
    //   - on, from the dry sub: a 30 ms crossfade to the high-pass resting at 2 Hz (nearly
    //     in phase with the dry sub), whose cutoff then glides up to the set frequency. The
    //     filter states are rescaled with the cutoff while it rises, which keeps the level
    //     within about 0.5 dB during the glide.
    std::array<SvfMultimode, 4> m_loCut;
    std::array<SvfMultimode, 2> m_loCutAllpass;
    double m_loCutLog = 0.0;      // current cutoff, ln(Hz)
    double m_loCutLogFast = 0.0;  // first stage of its two-pole glide
    float m_loCutG = 0.0f;        // its TPT coefficient
    float m_loCutFade = 0.0f;     // 0 = off path .. 1 = high-pass, linear in time
    float m_loCutFadeStep = 0.0f;
    double m_loCutGlideCoeff = 0.0;
    bool m_loCutDryOff = true;    // the off path is the dry sub (true) or the allpass (false)
    float m_resonance = 0.0f;
    float m_envAmount = 0.0f;
    float m_filterEnv = 0.0f;
    float m_filterEnvAttack = 0.0f;
    float m_filterEnvRelease = 0.0f;
    float m_lastReleaseMs = -1.0f;
    float m_modeWeights[3] { 1.0f, 0.0f, 0.0f };

    // Overtones: an exact harmonic series of the sub (sin(k*phase)), the
    // psychoacoustic "missing fundamental" trick used by MaxxBass/RBass so the
    // sub is heard on small speakers. Per-harmonic gains are smoothed so changing
    // the range never clicks.
    static constexpr int c_maxOvertone = 16;
    void updateOvertoneTargets(double subFrequency);
    float m_overtoneGain[c_maxOvertone + 1] {};
    float m_overtoneTarget[c_maxOvertone + 1] {};
    float m_overtoneAmount = 0.0f;
    float m_subLevel = 1.0f;     // smoothed H1 level
    float m_overtoneCapScale = 1.0f;
    float m_activity = 0.0f;     // 0..1 how much sub is sounding now (for meters)

public:
    /** 0..1: how much of harmonic k's set level is sounding right now (for the editor's meters). */
    float getHarmonicActivity(int k) const;

private:

    static constexpr int c_shaperUpdateInterval = 32;

    //==========================================================================
    // Phase Align. Allocated in prepare(); only touched while m_paRunning.
    struct Cpx { float re = 0.0f, im = 0.0f; };

    static constexpr int c_numPartials = c_maxOvertone;        // partials 1..16 of the sub
    static constexpr int c_energyIndex = 2 * c_numPartials;    // demodulator entry: re / im per partial, then x^2
    static constexpr int c_demodStride = c_energyIndex + 1;

    // Sliding boxcar over exactly one sub period of x * exp(-j k phi) for k = 1..16, plus x^2. Products are
    // summed over `decimation` samples per ring entry (the same cost and memory per second at any rate); the
    // running sums are double, so adding and later subtracting the same float does not drift.
    struct Demodulator
    {
        std::vector<float> ring;      // ringLength entries of c_demodStride floats
        int ringLength = 0;
        int position = 0;             // next entry to write
        int length = 0;               // entries currently summed (the window)
        int filled = 0;               // entries written since reset()
        double sum[c_demodStride] {};
        float acc[c_demodStride] {};  // the entry being accumulated
        int accCount = 0;

        void prepare(int entries);
        void reset();
        void push(int targetLength);  // store acc as a new entry, slide / resize the window (bounded work)
        const float* entry(int index) const;
    };

    // process() runs its sample loop as one of two instantiations of processSamples(): Classic, which contains no
    // Phase Align code at all (a session that never switches it on costs what the engine without it did), and
    // Phase Align, which returns early when it hands back to Classic mid-block.
    struct BlockConstants
    {
        float openMs = 0.0f, closeMs = 0.0f;
        float distortionTarget = 0.0f, toneTarget = 0.0f, postTarget = 0.0f, resonanceTarget = 0.0f, envAmountTarget = 0.0f;
        float thresholdDb = 0.0f;
        double loCutSetLog = 0.0, loCutRestLog = 0.0;
        float filterK = 0.0f;                             // updated every 8 samples: carried from one loop to the other
    };
    template <bool aligned>
    int processSamples(const float* in, float* out, float* residual, int start, int numSamples, BlockConstants& block);

    void phaseReset();                                    // all Phase Align state (prepare, reset, switching on)
    void phaseBeginExit();                                // switched off: turn back to Classic, then hand over
    void phaseLevelOnset();                               // the level gate has just opened
    void phaseNoteStart(bool correction);                 // a new note (or its corrected pitch): choose the phase
    void phaseControlUpdate();                            // every m_paUpdateInterval samples
    void advanceDuck();                                   // per sample: the fades around jumps, the tail guard
    void startRise();                                     // fade back in from the current gain
    void computeWetModel(double subHz);                   // the wet's partials k = 1..16 per unit level
    void batchEstimate(double f0, Cpx* X, float& rms) const; // the input's partials over its last period
    void updateAlignment(const Cpx* X, float rmsWindow);  // per-partial evidence and phase from a measurement
    void setCorrectionTargets(bool snap);                 // targets of the additive correction

    bool m_paRunning = false;         // the Phase Align code path runs (on, or turning back to Classic)
    bool m_paExiting = false;         // switched off: the partials turn back, then Classic takes over
    float m_engage = 0.0f;            // 0..1 ramp of the alignment (a smootherstep of it is applied)
    float m_engageStep = 0.0f;        // per control update
    double m_settleSeconds = 0.0;     // switched on: how long the alignment has followed a measurement
    float m_turn[c_numPartials + 1] {};     // while turning in / back: each partial's turn (unwrapped, radians) ...
    bool m_turnValid[c_numPartials + 1] {}; // ... and whether it holds one, so the direction never flips mid-turn
    int m_decimation = 1;
    int m_paUpdateInterval = 32;
    int m_paCounter = 0;
    std::int64_t m_sampleCounter = 0; // samples since Phase Align started
    std::int64_t m_onsetSample = 0;   // last rising edge of the level gate
    Demodulator m_demod;
    int m_windowTarget = 0;           // the window length the demodulator should have (entries)
    std::vector<float> m_history;     // the input for the note-start estimate
    int m_historyLength = 0;
    int m_historyPosition = 0;
    int m_historyCount = 0;           // samples written since Phase Align started
    bool m_prevValid = false;         // the last update had a valid window
    float m_rotation[c_numPartials + 1] {};    // smoothed rotation per update of each input partial / grid spacing
    float m_drift[c_numPartials + 1] {};       // smoothed signed drift of each input partial (radians per update)
    double m_lastRawPhase[c_numPartials + 1] {};
    bool m_batchActive = false;       // running on the note-start estimate until the window is valid
    Cpx m_batchX[c_numPartials + 1] {};
    float m_batchRms = 0.0f;
    float m_alignPhase[c_numPartials + 1] {};  // the input's partial k, phase in the oscillator's frame (last measured)
    float m_alignWeight[c_numPartials + 1] {}; // 0..1 how much partial k follows it

    // Fades: 0 idle, 1 falling to a pending phase / pitch jump, 2 rising, 3 held at zero (tail guard), 4 falling to it
    int m_duckState = 0;
    float m_duck = 1.0f;
    float m_duckFrom = 1.0f;          // the gain a fall or rise starts from
    float m_duckPos = 0.0f;
    float m_duckStep = 0.0f;
    int m_duckHold = 0;               // samples the tail guard may still hold
    float m_wetPeak = 0.0f;           // the wet's peak over about two sub periods: is a tail sounding?
    float m_wetPeakDecay = 0.0f;
    double m_pendingPhase = 0.0;      // phase (cycles) chosen for sample m_pendingSample - 1 ...
    double m_pendingSubHz = 0.0;      // ... carried forward at this pitch to the jump
    std::int64_t m_pendingSample = 0;

    // The wet per unit level before the post lowpass (S1 the clean sub, D the Drive path, O the overtones), and P the
    // post lowpass x Lo Cut, at the sub's partials
    Cpx m_modelS1 {};
    Cpx m_modelD[c_numPartials + 1] {};
    Cpx m_modelO[c_numPartials + 1] {};
    Cpx m_modelP[c_numPartials + 1] {};
    bool m_driveModelOk = true;

    // Additive correction, added to the Classic wet: sum_k corrA cos(k phi) + corrB sin(k phi). Its targets are the
    // per-update smoothed difference (aligned - Classic) of each partial, turned in by m_engage; per sample they are
    // reached by a linear ramp over the update interval (no update-rate staircase) and followed through two one-poles.
    float m_coefSmooth = 0.0f;
    Cpx m_diffSmoothed[c_numPartials + 1] {};
    float m_corrA[c_numPartials + 1] {}, m_corrB[c_numPartials + 1] {};
    float m_corrStepA[c_numPartials + 1] {}, m_corrStepB[c_numPartials + 1] {};
    float m_corrTargetA[c_numPartials + 1] {}, m_corrTargetB[c_numPartials + 1] {}; // ramped per sample ...
    float m_corrRampA[c_numPartials + 1] {}, m_corrRampB[c_numPartials + 1] {};     // ... by this much
    int m_corrRampLeft = 0;                                                          // ... for this many more samples

    PhaseAlignDiagnostics m_paDiag;
};

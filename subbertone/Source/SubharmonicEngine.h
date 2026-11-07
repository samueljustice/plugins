#pragma once

#include "PitchDetector.h"

#include <array>
#include <cmath>

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
};

#pragma once

#include "PitchDetector.h"

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
 *   wet = amplitude * voicingGate * equalPower(clean, harmonics, distortion)  --> post lowpass
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

    double m_sampleRate = 44100.0;
    float m_fs = 44100.0f;

    Parameters m_target;

    // Detection band and analysis
    Biquad m_bandLp1, m_bandLp2;
    float m_bandCutoff = 0.0f;
    PitchDetector m_detector;

    // Level tracking
    float m_inputMeanSquare = 0.0f;
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
    int m_unvoicedHops = 0;

    // Oscillator
    double m_phase = 0.0;
    float m_logFreq = 0.0f;
    float m_logFreqTarget = 0.0f;
    float m_glideCoeff = 0.0f;

    // Smoothed parameters
    float m_paramCoeff = 0.0f;
    float m_distortion = 0.5f;
    float m_toneHz = 1000.0f;
    float m_postLowpassHz = 20000.0f;

    // Saturation, with a short crossfade when the type changes
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
    float m_resonance = 0.0f;
    float m_envAmount = 0.0f;
    float m_filterEnv = 0.0f;
    float m_filterEnvAttack = 0.0f;
    float m_filterEnvRelease = 0.0f;
    float m_lastReleaseMs = -1.0f;
    float m_modeWeights[3] { 1.0f, 0.0f, 0.0f };

    static constexpr int c_shaperUpdateInterval = 32;
};

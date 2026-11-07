#include "SubharmonicEngine.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <utility>

namespace
{
    constexpr double c_twoPi = 6.283185307179586;
    constexpr double c_ln2 = 0.6931471805599453;
    constexpr float c_pi = 3.14159265358979f;

    constexpr double c_levelTimeSeconds    = 0.020;
    constexpr double c_bandEnvelopeHz      = 12.0;  // 2-pole smoothing of the band RMS
    constexpr double c_gateAttackSeconds   = 0.010;
    constexpr double c_gateReleaseSeconds  = 0.080;
    // Pitch tracker
    constexpr double c_trackHoldSeconds    = 0.25;  // keep the last pitch through dropouts this long
    constexpr double c_jumpConfirmSeconds  = 0.04;  // a big jump must persist this long ...
    constexpr double c_octaveConfirmSeconds = 0.12; // ... an octave flip this long ...
    constexpr double c_sureConfirmSeconds  = 0.015; // ... unless the detector is sure of it
    constexpr float  c_sureConfidence      = 0.98f; // "sure": a near-perfect YIN dip
    constexpr double c_weakWindowSeconds   = 0.10;  // weak guesses only bridge the hold this soon after a strong estimate ...
    constexpr double c_weakLongSeconds     = 0.50;  // ... or this long while the detector keeps hearing a pitch
    constexpr int    c_weakDenseHops       = 5;     // "keeps hearing": voiced in at least this many of the last 10 hops
    constexpr float  c_newNoteGate         = 0.6f;  // a track that starts with the sub faded below this is a new note
    constexpr double c_newNoteFixSeconds   = 0.10;  // a new note's pitch corrected this soon after it started jumps too
    constexpr float  c_followSemitones     = 3.0f;  // moves within this are followed directly
    constexpr float  c_weakFollowSemitones = 1.5f;  // ... or within this of the last strong pitch for low-confidence guesses
    constexpr float  c_candidateTolerance  = 1.0f;  // semitones: "the same" candidate
    constexpr float  c_steadySemitones     = 0.01f; // track changes smaller than this (1 cent) are estimate wobble
    constexpr float  c_strongConfidence    = 0.8f;  // strict YIN hits; fallback guesses are weaker
    constexpr double c_paramSeconds        = 0.030;
    constexpr double c_typeFadeSeconds     = 0.030;
    constexpr float  c_gateHysteresisDb    = 3.0f;
    constexpr float  c_maxDrive            = 10.0f;
    constexpr int    c_filterUpdateInterval = 8;
    constexpr double c_maxResidualPeak     = 1.5;  // relative to a unit sine
    constexpr double c_filterEnvAttackSecs = 0.003;
    constexpr float  c_maxResonanceDrive   = 5.0f;
    constexpr float  c_maxInput            = 1.0e4f; // |x| at or above this (+80 dBFS), NaN and inf are treated as silence
    constexpr double c_dcBlockHz           = 5.0;    // DC blocker on the input the engine analyses (never on the dry path)
    constexpr double c_loCutFadeSeconds    = 0.030; // Lo Cut on/off crossfade (exact length)
    constexpr double c_loCutGlideSeconds   = 0.060; // per pole of the cutoff's two-pole glide
    constexpr double c_loCutRestHz         = 2.0;   // where the high-pass rests while the off path is the dry sub
    constexpr float  c_loCutSilentGate     = 1.0e-5f; // below this the sub is silent: the allpass may hand back to dry

    constexpr SubharmonicEngine::Parameters c_defaults {};

    float onePoleCoeff(double seconds, double fs)
    {
        return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * fs)));
    }

    int hopsFor(double seconds, double hopSeconds, int minimum)
    {
        return std::max(minimum, static_cast<int>(std::lround(seconds / hopSeconds)));
    }

    // NaN and inf from a broken host fall back to the default instead of reaching a filter state.
    float finiteOr(float value, float fallback)
    {
        return std::isfinite(value) ? value : fallback;
    }

    // SVF damping from 0..1 resonance: Q 0.707 -> 20, exponential feel.
    float resonanceK(float resonance)
    {
        constexpr float maxK = 1.41421356f;
        constexpr float minK = 0.05f;
        return maxK * std::pow(minK / maxK, std::clamp(resonance, 0.0f, 1.0f));
    }

    float loCutCoefficient(double logHz, float fs)
    {
        const float hz = static_cast<float>(std::exp(logHz));
        return std::tan(c_pi * std::clamp(hz, 1.0f, fs * 0.25f) / fs);
    }

    //==========================================================================
    // Phase Align
    constexpr double c_minSubHz          = 0.5 * PitchDetector::c_minFrequency; // lowest sub the tracker asks for
    constexpr double c_minInputHz        = 38.0;   // the note-start history holds one period of this
    constexpr double c_semitoneLog       = 0.05776226504666211; // ln(2) / 12
    constexpr double c_duckFallSeconds   = 0.008;  // fade before a phase / pitch jump under a sounding tail ...
    constexpr double c_duckRiseSeconds   = 0.008;  // ... and the rise of the new note after it
    constexpr double c_tailGuardSeconds  = 0.300;  // longest hold of a stale tail at zero while no note starts
    constexpr double c_coefSmoothSeconds = 0.004;  // per-update smoothing of the alignment (or half a sub period)
    constexpr double c_coefAudioSeconds  = 0.0015; // then two audio-rate one-poles
    constexpr double c_engageSeconds     = 0.050;  // switching on / off: partials turn onto / back from the input's phases
    constexpr float  c_duckAudible       = 3.0e-3f; // a tail below this re the new note's band level may jump at once
    constexpr float  c_guardGate         = 1.0e-4f; // voicing gate above this when a new sound arrives: its tail is guarded
    constexpr float  c_silentLevel       = 1.0e-5f; // band amplitude x gates below this: nothing to hear
    constexpr float  c_coherenceLow      = 0.02f;  // |X_k| / (sqrt2 * window RMS): noise floor ...
    constexpr float  c_coherenceHigh     = 0.06f;  // ... and "clearly a partial"
    constexpr float  c_coherenceLowSub   = 0.04f;  // the same for k = 1 (the sub itself): stricter
    constexpr float  c_coherenceHighSub  = 0.10f;
    constexpr float  c_rotationStable    = 0.25f;  // rotation per update / grid spacing: a real partial mistuned by e
    constexpr float  c_rotationUnstable  = 0.50f;  // rotates at k e of it, leakage of a neighbour at about 1
    constexpr float  c_alignFloorDb      = -40.0f; // partials this far below the strongest are not followed
    constexpr float  c_alignFullDb       = -30.0f;
    constexpr int    c_gridSize          = 64;     // oscillator shifts searched at a note start
    constexpr int    c_tableDrives       = 145;    // shaper harmonic table: drive 1..10 in steps of 1/16
    constexpr int    c_tablePoints       = 1024;

    template <typename T> T wrapPi(T a) // to (-pi, pi]
    {
        constexpr T pi = static_cast<T>(3.14159265358979323846);
        a = std::remainder(a, static_cast<T>(2) * pi);
        return a <= -pi ? a + static_cast<T>(2) * pi : a;
    }

    float smoothstep(float x, float lo, float hi)
    {
        const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    struct ShaperHarmonicTable
    {
        // Fourier coefficient of shape(drive * sin phi) at harmonic k: cos amplitude for even k, sin amplitude for
        // odd k (a memoryless curve of sin phi has no other terms). Unnormalised: the engine scales it by
        // harmonicGain / norm of its current ShaperCoefficients, exactly as harmonicResidual() does.
        float g[SubharmonicEngine::NumSaturationTypes][c_tableDrives][17] {};
    };

    const ShaperHarmonicTable& shaperHarmonicTable()
    {
        static const ShaperHarmonicTable table = []
        {
            ShaperHarmonicTable t;
            std::vector<double> sine(c_tablePoints), basis(static_cast<size_t>(c_tablePoints) * 17);
            for (int i = 0; i < c_tablePoints; ++i)
            {
                const double phi = c_twoPi * (i + 0.5) / c_tablePoints;
                sine[static_cast<size_t>(i)] = std::sin(phi);
                for (int k = 1; k <= 16; ++k)
                    basis[static_cast<size_t>(i * 17 + k)] = (k % 2 == 0) ? std::cos(k * phi) : std::sin(k * phi);
            }
            for (int type = 0; type < SubharmonicEngine::NumSaturationTypes; ++type)
                for (int d = 0; d < c_tableDrives; ++d)
                {
                    const float drive = 1.0f + 9.0f * static_cast<float>(d) / static_cast<float>(c_tableDrives - 1);
                    double acc[17] {};
                    for (int i = 0; i < c_tablePoints; ++i)
                    {
                        const double v = SubharmonicEngine::shape(static_cast<float>(drive * sine[static_cast<size_t>(i)]), type, drive);
                        for (int k = 1; k <= 16; ++k)
                            acc[k] += v * basis[static_cast<size_t>(i * 17 + k)];
                    }
                    for (int k = 1; k <= 16; ++k)
                        t.g[type][d][k] = static_cast<float>(2.0 * acc[k] / c_tablePoints);
                }
            return t;
        }();
        return table;
    }

    void lookupShaperHarmonics(int type, float drive, float scale, float* out)
    {
        const auto& t = shaperHarmonicTable();
        const float pos = std::clamp((drive - 1.0f) / 9.0f * static_cast<float>(c_tableDrives - 1), 0.0f, static_cast<float>(c_tableDrives - 1));
        const int i0 = std::min(static_cast<int>(pos), c_tableDrives - 2);
        const float f = pos - static_cast<float>(i0);
        const int ty = std::clamp(type, 0, SubharmonicEngine::NumSaturationTypes - 1);
        for (int k = 1; k <= 16; ++k)
            out[k] = scale * (t.g[ty][i0][k] + f * (t.g[ty][i0 + 1][k] - t.g[ty][i0][k]));
    }

    struct PhaseGrid
    {
        float c[17][c_gridSize] {}, s[17][c_gridSize] {};
    };

    const PhaseGrid& phaseGrid()
    {
        static const PhaseGrid grid = []
        {
            PhaseGrid g;
            for (int k = 0; k <= 16; ++k)
                for (int j = 0; j < c_gridSize; ++j)
                {
                    const double a = c_twoPi * k * j / static_cast<double>(c_gridSize);
                    g.c[k][j] = static_cast<float>(std::cos(a));
                    g.s[k][j] = static_cast<float>(std::sin(a));
                }
            return g;
        }();
        return grid;
    }

    // Exact responses of the TPT state-variable filters (bilinear with prewarping) at a frequency, given
    // w = tan(pi f / fs) and the filter's g = tan(pi fc / fs).
    struct C2 { float re, im; };
    inline C2 cmul(C2 a, C2 b) { return { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re }; }
    inline C2 cdiv(C2 a, C2 b) { const float d = b.re * b.re + b.im * b.im; return { (a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d }; }

    void svfResponses(float w, float g, float k, C2& lp, C2& bp, C2& hp)
    {
        const float W = w / g;
        const C2 den { 1.0f - W * W, k * W };
        lp = cdiv({ 1.0f, 0.0f }, den);
        bp = cdiv({ 0.0f, W }, den);
        hp = cdiv({ -W * W, 0.0f }, den);
    }

    // The optimum of a periodic 64-point landscape: hill-climb from `start` (grid units), compare with the global
    // optimum and switch only if that is better by `margin`. Returns the fractional position (parabolic refinement)
    // and the value there.
    float trackOptimum(const float* v, float start, float margin, float& value)
    {
        int j = static_cast<int>(std::lround(start)) & (c_gridSize - 1);
        for (int step = 0; step < c_gridSize; ++step)
        {
            const int l = (j + c_gridSize - 1) & (c_gridSize - 1), r = (j + 1) & (c_gridSize - 1);
            if (v[l] > v[j] && v[l] >= v[r]) j = l;
            else if (v[r] > v[j]) j = r;
            else break;
        }
        int g = 0;
        for (int i = 1; i < c_gridSize; ++i)
            if (v[i] > v[g]) g = i;
        if (v[g] - v[j] > margin)
            j = g;

        const float a = v[(j + c_gridSize - 1) & (c_gridSize - 1)], b = v[j], c = v[(j + 1) & (c_gridSize - 1)];
        const float den = a - 2.0f * b + c;
        float delta = 0.0f;
        if (std::abs(den) > 1.0e-20f)
            delta = std::clamp(0.5f * (a - c) / den, -0.5f, 0.5f);
        value = b - 0.25f * (a - c) * delta;
        float pos = static_cast<float>(j) + delta;
        if (pos > 0.5f * c_gridSize) pos -= static_cast<float>(c_gridSize); // to (-32, 32]
        if (pos <= -0.5f * c_gridSize) pos += static_cast<float>(c_gridSize);
        return pos;
    }
}

//==============================================================================
void SubharmonicEngine::Biquad::setLowpass(double fs, double fc, double q)
{
    const double w0 = c_twoPi * fc / fs;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;

    b0 = static_cast<float>(((1.0 - cosw) * 0.5) / a0);
    b1 = static_cast<float>((1.0 - cosw) / a0);
    b2 = b0;
    a1 = static_cast<float>((-2.0 * cosw) / a0);
    a2 = static_cast<float>((1.0 - alpha) / a0);
}

void SubharmonicEngine::SvfLowpass::setCutoff(float fc, float fs)
{
    fc = std::clamp(fc, 10.0f, fs * 0.45f);
    g = std::tan(c_pi * fc / fs);
}

//==============================================================================
void SubharmonicEngine::prepare(double sampleRate)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    m_fs = static_cast<float>(m_sampleRate);

    m_detector.prepare(m_sampleRate);

    m_levelCoeff   = onePoleCoeff(c_levelTimeSeconds, m_sampleRate);
    m_dcCoeff      = std::exp(-c_twoPi * c_dcBlockHz / m_sampleRate);
    m_bandEnvCoeff = static_cast<float>(1.0 - std::exp(-c_twoPi * c_bandEnvelopeHz / m_sampleRate));
    m_gateAttack   = onePoleCoeff(c_gateAttackSeconds, m_sampleRate);
    m_gateRelease  = onePoleCoeff(c_gateReleaseSeconds, m_sampleRate);
    m_lastGlideMs  = -1.0f; // glide coefficient is set from the parameter in process()

    const double hopSeconds = m_detector.getHopSize() / m_sampleRate;
    m_jumpConfirmHops   = hopsFor(c_jumpConfirmSeconds, hopSeconds, 2);
    m_octaveConfirmHops = hopsFor(c_octaveConfirmSeconds, hopSeconds, 3);
    m_sureConfirmHops   = hopsFor(c_sureConfirmSeconds, hopSeconds, 2);
    m_holdLimitHops     = hopsFor(c_trackHoldSeconds, hopSeconds, 3);
    m_weakWindowHops    = hopsFor(c_weakWindowSeconds, hopSeconds, 1);
    m_weakLongHops      = std::max(m_weakWindowHops, hopsFor(c_weakLongSeconds, hopSeconds, 1));
    m_newNoteFixHops    = hopsFor(c_newNoteFixSeconds, hopSeconds, 1);
    m_paramCoeff   = onePoleCoeff(c_paramSeconds, m_sampleRate);
    m_typeFadeStep = static_cast<float>(1.0 / (c_typeFadeSeconds * m_sampleRate));
    m_loCutFadeStep = static_cast<float>(1.0 / (c_loCutFadeSeconds * m_sampleRate));
    m_loCutGlideCoeff = 1.0 - std::exp(-1.0 / (c_loCutGlideSeconds * m_sampleRate));
    m_filterEnvAttack = onePoleCoeff(c_filterEnvAttackSecs, m_sampleRate);
    m_lastReleaseMs = -1.0f; // force recompute

    // Phase Align: buffers are always allocated (it can be switched on at any time), and the static tables are
    // built here, never on the audio thread. One sub period at the lowest pitch: about 310 KB at 44.1..192 kHz.
    m_decimation = std::max(1, static_cast<int>(std::lround(m_sampleRate / 48000.0)));
    m_paUpdateInterval = 32 * m_decimation;
    m_demod.prepare(static_cast<int>(std::ceil(m_sampleRate / (m_decimation * c_minSubHz))) + 8);
    m_historyLength = static_cast<int>(std::ceil(m_sampleRate / c_minInputHz)) + 8;
    m_history.assign(static_cast<size_t>(m_historyLength), 0.0f);
    m_coefSmooth = onePoleCoeff(c_coefAudioSeconds, m_sampleRate);
    m_engageStep = static_cast<float>(m_paUpdateInterval / (c_engageSeconds * m_sampleRate));
    (void) shaperHarmonicTable();
    (void) phaseGrid();

    reset();
}

void SubharmonicEngine::reset()
{
    m_detector.reset();
    m_detector.setMaxFrequency(m_target.maxFundamentalHz);

    m_bandCutoff = 0.0f;
    updateDetectionBand(m_target.maxFundamentalHz);
    m_bandLp1.reset();
    m_bandLp2.reset();

    m_inputMeanSquare = 0.0f;
    m_dcIn = m_dcOut = 0.0;
    m_inputRms = 0.0f;
    m_levelOpen = false;
    m_bandEnv1 = m_bandEnv2 = 0.0f;

    m_fundamental = 0.0f;
    m_gate = 0.0f;
    m_gateTarget = 0.0f;
    resetTracker();

    m_phase = 0.0;
    m_logFreq = m_logFreqTarget = std::log(50.0);

    m_distortion = m_target.distortion;
    m_toneHz = m_target.toneHz;
    m_postLowpassHz = m_target.postLowpassHz;

    m_type = m_previousType = m_target.saturationType;
    m_typeFade = 1.0f;
    m_shaper = m_previousShaper = computeShaper(1.0f + m_distortion * (c_maxDrive - 1.0f), m_type);
    m_shaperCounter = 0;

    m_harmonicFilter.reset();
    for (auto& stage : m_loCut)
        stage.reset();
    for (auto& stage : m_loCutAllpass)
        stage.reset();
    m_loCutLog = m_loCutLogFast = std::log(m_target.loCutOn ? static_cast<double>(m_target.loCutHz) : c_loCutRestHz);
    m_loCutG = loCutCoefficient(m_loCutLog, m_fs);
    m_loCutFade = m_target.loCutOn ? 1.0f : 0.0f;
    m_loCutDryOff = true;

    m_overtoneAmount = m_target.overtoneAmount;
    m_subLevel = m_target.harmonicLevels[1];
    m_activity = 0.0f;
    updateOvertoneTargets(50.0);
    for (int k = 0; k <= c_maxOvertone; ++k)
        m_overtoneGain[k] = m_overtoneTarget[k];
    m_postFilter.reset();
    m_postFilter.setCutoff(m_postLowpassHz, m_fs);

    m_resonance = m_target.resonance;
    m_envAmount = m_target.envAmountOctaves;
    m_filterEnv = 0.0f;
    for (int m = 0; m < 3; ++m)
        m_modeWeights[m] = (m == m_target.filterMode) ? 1.0f : 0.0f;

    // Phase Align starts directly in the requested mode after a reset: there is nothing to turn from.
    m_paRunning = m_target.phaseAlign && m_demod.ringLength > 0;
    m_paExiting = false;
    phaseReset();
    m_engage = m_paRunning ? 1.0f : 0.0f;
}

void SubharmonicEngine::setParameters(const Parameters& p)
{
    m_target = p;
    m_target.distortion = std::clamp(finiteOr(p.distortion, c_defaults.distortion), 0.0f, 1.0f);
    m_target.saturationType = std::clamp(p.saturationType, 0, NumSaturationTypes - 1);
    m_target.toneHz = std::clamp(finiteOr(p.toneHz, c_defaults.toneHz), 1.0f, 1.0e5f); // wider than any use clamps
    m_target.resonance = std::clamp(finiteOr(p.resonance, c_defaults.resonance), 0.0f, 1.0f);
    m_target.filterMode = std::clamp(p.filterMode, 0, 2);
    m_target.envAmountOctaves = std::clamp(finiteOr(p.envAmountOctaves, c_defaults.envAmountOctaves), -6.0f, 6.0f);
    m_target.envReleaseMs = std::clamp(finiteOr(p.envReleaseMs, c_defaults.envReleaseMs), 1.0f, 5000.0f);
    m_target.postLowpassHz = std::clamp(finiteOr(p.postLowpassHz, c_defaults.postLowpassHz), 1.0f, 1.0e5f);
    m_target.thresholdDb = finiteOr(p.thresholdDb, c_defaults.thresholdDb);
    m_target.maxFundamentalHz = finiteOr(p.maxFundamentalHz, c_defaults.maxFundamentalHz);
    m_target.glideMs = std::clamp(finiteOr(p.glideMs, c_defaults.glideMs), 1.0f, 2000.0f);
    m_target.loCutHz = std::clamp(finiteOr(p.loCutHz, c_defaults.loCutHz), 10.0f, 200.0f);
    m_target.overtoneAmount = std::clamp(finiteOr(p.overtoneAmount, c_defaults.overtoneAmount), 0.0f, 1.0f);
    for (auto& level : m_target.harmonicLevels)
        level = std::clamp(finiteOr(level, 0.0f), 0.0f, 1.0f);

    const int wantedType = m_target.saturationType;

    if (m_typeFade >= 1.0f)
    {
        if (wantedType != m_type)
            startTypeFade(wantedType);
    }
    else if (wantedType == m_previousType)
    {
        // Back to the type the running fade started from: reverse the fade from where it
        // is (same mix of the same two types), so the output does not move.
        std::swap(m_type, m_previousType);
        std::swap(m_shaper, m_previousShaper);
        m_typeFade = 1.0f - m_typeFade;
    }
    // Any other type chosen mid-fade waits: process() starts it once this fade is done.

    m_detector.setMaxFrequency(m_target.maxFundamentalHz);
    updateDetectionBand(m_target.maxFundamentalHz);
}

void SubharmonicEngine::startTypeFade(int newType)
{
    // Crossfade from whatever we are currently producing.
    m_previousType = m_type;
    m_previousShaper = m_shaper;
    m_type = newType;
    m_shaper = computeShaper(m_shaper.drive, m_type);
    m_typeFade = 0.0f;
}

void SubharmonicEngine::updateDetectionBand(float maxFundamentalHz)
{
    // Lowpass a little above the highest fundamental we care about, which also
    // serves as the anti-alias filter for the detector's internal decimation.
    const float decimatedNyquist = m_fs / std::max(1.0f, std::round(m_fs / 11025.0f)) * 0.5f;
    const float fc = std::clamp(maxFundamentalHz * 1.25f, 60.0f, decimatedNyquist * 0.8f);

    if (std::abs(fc - m_bandCutoff) < 0.5f)
        return;

    m_bandCutoff = fc;
    // 4th-order Butterworth as two biquads.
    m_bandLp1.setLowpass(m_sampleRate, fc, 0.5411961);
    m_bandLp2.setLowpass(m_sampleRate, fc, 1.3065630);
}

//==============================================================================
// Every curve has shape(0) == 0 and unit slope at 0. Because the input is
// always a pure sine, each curve is effectively a harmonic recipe:
//   odd harmonics of the sub land at 1.5x, 2.5x... the input note (fifths/thirds)
//   even harmonics land at 1x, 2x... the input note (reinforce its own series)
float SubharmonicEngine::shape(float x, int type, float drive)
{
    switch (type)
    {
        case Valve:
        {
            // Bias that scales with drive shifts the clipping point, so the duty
            // cycle stays asymmetric and the 2nd harmonic survives at any drive.
            const float bias = 0.2f * drive;
            return std::tanh(x + bias) - std::tanh(bias);
        }

        case Console:
        {
            // Hard knee: 5th-order polynomial soft clip, flat (C1) at |x| = 1.25.
            // Flatter top than tanh, so brighter upper odd harmonics.
            constexpr float k = 1.25f;
            if (x >= k)  return 1.0f;
            if (x <= -k) return -1.0f;
            const float u = x / k;
            const float u2 = u * u;
            // p(u) = u - 2/3 u^3 + 1/5 u^5, p'(u) = (1 - u^2)^2, normalised by p(1) = 8/15.
            return (u - (2.0f / 3.0f) * u * u2 + 0.2f * u * u2 * u2) * (15.0f / 8.0f);
        }

        case Transformer:
        {
            // Soft atan knee with a small drive-scaled bias: mostly odd, a little 2nd.
            const float bias = 0.07f * drive;
            const auto curve = [](float v) { return (2.0f / c_pi) * std::atan(v * c_pi * 0.5f); };
            return curve(x + bias) - curve(bias);
        }

        case Rectifier:
        {
            // Smoothed full-wave rectifier: even harmonics only, so every partial
            // sits on the input note's own harmonic series. The corner radius is
            // fixed in absolute terms, so as drive scales x up the corner gets
            // relatively sharper: near-pure 2nd at low drive, full |sin| at high.
            constexpr float radius = 0.5f;
            return std::sqrt(x * x + radius * radius) - radius;
        }

        case Wavefolder:
        {
            // Sine folder: partials sweep upward as drive rises. Odd only.
            return std::sin(x * c_pi * 0.5f);
        }

        case Tape:
        default:
            return std::tanh(x);
    }
}

SubharmonicEngine::ShaperCoefficients SubharmonicEngine::computeShaper(float drive, int type)
{
    constexpr size_t n = 128;
    std::array<double, n> sine {}, shaped {};

    ShaperCoefficients c;
    c.drive = drive;

    // One cycle of shape(drive * sin). Peak is measured, not assumed at +/-drive,
    // because curves need not be monotonic (e.g. the folder).
    double peak = 0.0;

    for (size_t i = 0; i < n; ++i)
    {
        sine[i] = std::sin(c_twoPi * (static_cast<double>(i) + 0.5) / static_cast<double>(n));
        shaped[i] = shape(static_cast<float>(drive * sine[i]), type, drive);
        peak = std::max(peak, std::abs(shaped[i]));
    }

    c.norm = peak > 1.0e-6 ? static_cast<float>(peak) : 1.0f;

    // Fourier DC and fundamental of the normalised curve.
    double dc = 0.0, fund = 0.0;

    for (size_t i = 0; i < n; ++i)
    {
        dc += shaped[i] / c.norm;
        fund += shaped[i] / c.norm * sine[i];
    }

    c.dc = static_cast<float>(dc / static_cast<double>(n));
    c.fundamental = static_cast<float>(2.0 * fund / static_cast<double>(n));

    // Residual (harmonics only) RMS and peak.
    double meanSquare = 0.0, residualPeak = 0.0;

    for (size_t i = 0; i < n; ++i)
    {
        const double r = shaped[i] / c.norm - c.dc - c.fundamental * sine[i];
        meanSquare += r * r;
        residualPeak = std::max(residualPeak, std::abs(r));
    }

    meanSquare /= static_cast<double>(n);

    // Match the clean sine's RMS so the Distortion knob is a constant-loudness
    // crossfade, but cap the peak so spiky curves (rectifier) cannot jump out.
    if (meanSquare > 1.0e-10)
    {
        const double rmsGain  = std::sqrt(0.5 / meanSquare);
        const double peakGain = c_maxResidualPeak / residualPeak;
        c.harmonicGain = static_cast<float>(std::min({ rmsGain, peakGain, 8.0 }));
    }
    else
    {
        c.harmonicGain = 0.0f;
    }

    return c;
}

float SubharmonicEngine::harmonicResidual(float s, const ShaperCoefficients& c, int type) const
{
    return c.harmonicGain * (shape(c.drive * s, type, c.drive) / c.norm - c.dc - c.fundamental * s);
}

//==============================================================================
void SubharmonicEngine::updateOvertoneTargets(double subFrequency)
{
    // Each bar is an amplitude relative to the unit sub (1.0 = as loud as the sub at
    // Drive 0 and H1 100 %). Partials that would land above ~0.45 fs are left out (no
    // aliasing). If the bars add up to more than 1.5x the sub's RMS, all of them are
    // scaled down together, so 16 bars at 100% can't explode the level.
    constexpr double maxEnergy = 1.5 * 1.5;
    double sumSquares = 0.0;

    m_overtoneTarget[0] = m_overtoneTarget[1] = 0.0f;
    for (int k = 2; k <= c_maxOvertone; ++k)
    {
        const bool belowNyquist = subFrequency * k < m_sampleRate * 0.45;
        m_overtoneTarget[k] = belowNyquist ? m_target.harmonicLevels[static_cast<size_t>(k)] : 0.0f;
        sumSquares += static_cast<double>(m_overtoneTarget[k]) * m_overtoneTarget[k];
    }

    m_overtoneCapScale = sumSquares > maxEnergy ? static_cast<float>(std::sqrt(maxEnergy / sumSquares)) : 1.0f;
    for (int k = 2; k <= c_maxOvertone; ++k)
        m_overtoneTarget[k] *= m_overtoneCapScale;
}

float SubharmonicEngine::getHarmonicActivity(int k) const
{
    if (k < 1 || k > c_maxOvertone)
        return 0.0f;

    if (k == 1)
        return m_activity;

    const float set = m_target.harmonicLevels[static_cast<size_t>(k)];
    if (set <= 1.0e-4f)
        return 0.0f;

    // Includes the master amount, the energy cap and Nyquist muting.
    return std::clamp(m_activity * m_overtoneAmount * m_overtoneGain[k] / set, 0.0f, 1.0f);
}

void SubharmonicEngine::resetTracker()
{
    m_hasTrack = false;
    m_candidateHops = 0;
    m_candidateSureHops = 0;
    m_startHops = 0;
    m_holdHops = 0;
    m_hopsSinceStrong = 0;
    m_voicedHistory = 0;
    m_newNoteTrack = false;
}

void SubharmonicEngine::setTrackPitch(float semitones)
{
    // The estimates of a steady note wobble by a fraction of a cent from hop to hop. Moving
    // the oscillator after every one would frequency-modulate the sub (and its 16th harmonic
    // sixteen times as much), so changes smaller than a cent are ignored.
    if (!m_hasTrack || std::abs(semitones - m_trackSt) >= c_steadySemitones)
        m_trackSt = semitones;
}

void SubharmonicEngine::acceptPitch(float semitones)
{
    // Strong estimates only: they also become the reference for weak guesses.
    setTrackPitch(semitones);
    m_strongSt = m_trackSt;
    m_hopsSinceStrong = 0;
    m_hasTrack = true;
    m_candidateHops = 0;
    m_candidateSureHops = 0;
    m_holdHops = 0;
}

void SubharmonicEngine::onPitchEstimate()
{
    if (!m_levelOpen)
    {
        // Below the threshold: stop immediately, forget the track.
        resetTracker();
        m_gateTarget = 0.0f;
        m_fundamental = 0.0f;
        return;
    }

    const float f0 = m_detector.getFrequency();
    const float confidence = m_detector.getConfidence();
    const bool inRange = f0 >= PitchDetector::c_minFrequency && f0 <= m_target.maxFundamentalHz * 1.1f;
    const bool strong = confidence >= c_strongConfidence;
    bool used = false;
    bool started = false;
    bool corrected = false;

    if (m_hopsSinceStrong <= m_weakLongHops)
        ++m_hopsSinceStrong;

    if (m_trackAgeHops <= m_newNoteFixHops)
        ++m_trackAgeHops;

    m_voicedHistory = ((m_voicedHistory << 1) | (inRange ? 1u : 0u)) & 0x3FFu;
    const bool dense = std::popcount(m_voicedHistory) >= c_weakDenseHops;

    if (inRange)
    {
        const float st = 12.0f * std::log2(f0);

        if (!m_hasTrack)
        {
            // Start a track from two consistent strong estimates in a row.
            if (strong)
            {
                if (m_startHops > 0 && std::abs(st - m_candidateSt) < c_candidateTolerance)
                    ++m_startHops;
                else
                {
                    m_candidateSt = st;
                    m_startHops = 1;
                }

                if (m_startHops >= 2)
                {
                    acceptPitch(st);
                    m_startHops = 0;
                    used = true;
                    started = true;
                    m_trackAgeHops = 0;
                    m_newNoteTrack = m_gate < c_newNoteGate;
                }
            }
        }
        else if (strong)
        {
            const float distance = st - m_trackSt;

            if (std::abs(distance) <= c_followSemitones)
            {
                acceptPitch(st);
                used = true;
            }
            else
            {
                // A big jump only wins if it keeps coming back. Octave flips are the
                // classic detector error, so they need to persist longer. A near-perfect
                // YIN dip (a clean new note) is trusted after a few hops either way: the
                // detector's errors on noisy material do not look like that. 'confidence'
                // belongs to the newest raw estimate while 'st' is the median of the last
                // three, so a single sure hop could vouch for a stale octave error still
                // sitting in the median: the shortcut needs two sure hops in a row on the
                // same candidate.
                const bool octave = std::abs(std::abs(distance) - 12.0f) < c_candidateTolerance;
                const bool sure = confidence >= c_sureConfidence;

                if (m_candidateHops > 0 && std::abs(st - m_candidateSt) < c_candidateTolerance * 1.5f)
                    ++m_candidateHops;
                else
                {
                    m_candidateSt = st;
                    m_candidateHops = 1;
                    m_candidateSureHops = 0;
                }

                m_candidateSureHops = sure ? m_candidateSureHops + 1 : 0;
                const int needed = m_candidateSureHops >= 2 ? m_sureConfirmHops
                                                            : (octave ? m_octaveConfirmHops : m_jumpConfirmHops);

                if (m_candidateHops >= needed)
                {
                    acceptPitch(m_candidateSt);
                    used = true;
                    // The first estimates of a note are the ones most likely to be an octave
                    // off (YIN on a window that is only partly filled by the new note): a
                    // correction right after a new note started jumps like the start did.
                    corrected = m_newNoteTrack && m_trackAgeHops <= m_newNoteFixHops;
                }
            }
        }
        else if ((m_hopsSinceStrong <= m_weakWindowHops || (dense && m_hopsSinceStrong <= m_weakLongHops))
                 && std::abs(st - m_strongSt) <= c_weakFollowSemitones)
        {
            // Weak (fallback) guesses may only make small corrections around the last
            // strong pitch, so a run of them cannot walk the track away, and only bridge
            // the hold for a short while after it, so they cannot keep a phantom sub
            // going on noise.
            setTrackPitch(st);
            m_candidateHops = 0;
            m_candidateSureHops = 0;
            m_holdHops = 0;
            used = true;
        }
    }

    // Nothing usable this hop: hold the last pitch for a while before letting go.
    if (!used && m_hasTrack && ++m_holdHops > m_holdLimitHops)
        resetTracker();

    if (m_hasTrack)
    {
        m_fundamental = std::exp2(m_trackSt / 12.0f);
        m_logFreqTarget = (static_cast<double>(m_trackSt) / 12.0 - 1.0) * c_ln2; // ln(f0 / 2)

        // A new note (a track that starts once the last one's sub has faded: the first
        // note, or the next one after a rest) starts on its own pitch instead of gliding
        // in from wherever the last note was, and so does a correction of its pitch in its
        // first 100 ms. The phase stays continuous, so this cannot click. A track picked up
        // again while the sub is still sounding (a dropout in continuous material) glides
        // as before. While the Phase Align code path runs, its note start below makes the jump instead.
        if (((started && m_gate < c_newNoteGate) || corrected) && !m_paRunning)
            m_logFreq = m_logFreqTarget;

        m_gateTarget = 1.0f;

        if (m_paRunning)
        {
            // Phase Align: choose the new note's phase. Where Classic jumps the pitch, where the oscillator is more
            // than a semitone from the new note (Classic would glide in from a stale pitch) and where the tail guard
            // holds a stale tail, pitch and phase jump together, inaudibly: at once if nothing is sounding, else at
            // the bottom of an 8 ms fade. A track picked up again within a semitone of a sub that is still sounding
            // keeps sounding. A correction of a new note's pitch is a note start too (Classic jumps there as well).
            // This also runs while switched off and turning back (until Classic takes over), so switching off and
            // on again quickly never fades a note in and out.
            const bool stale = std::abs(m_logFreq - m_logFreqTarget) > c_semitoneLog;
            const bool guardHolding = m_duckState == 3 || m_duckState == 4;

            if (started && (m_gate < c_newNoteGate || stale || guardHolding))
                phaseNoteStart(false);
            else if (corrected)
                phaseNoteStart(true);
        }
    }
    else
    {
        m_gateTarget = 0.0f;
        m_fundamental = 0.0f;
    }
}

//==============================================================================
// Phase Align
//==============================================================================
void SubharmonicEngine::Demodulator::prepare(int entries)
{
    ringLength = std::max(16, entries);
    ring.assign(static_cast<size_t>(ringLength) * c_demodStride, 0.0f);
    reset();
}

void SubharmonicEngine::Demodulator::reset()
{
    position = 0;
    length = 0;
    filled = 0;
    accCount = 0;
    for (int j = 0; j < c_demodStride; ++j)
    {
        sum[j] = 0.0;
        acc[j] = 0.0f;
    }
}

const float* SubharmonicEngine::Demodulator::entry(int index) const
{
    index %= ringLength;
    if (index < 0)
        index += ringLength;
    return &ring[static_cast<size_t>(index) * c_demodStride];
}

void SubharmonicEngine::Demodulator::push(int targetLength)
{
    float* e = &ring[static_cast<size_t>(position) * c_demodStride];
    for (int j = 0; j < c_demodStride; ++j)
    {
        e[j] = acc[j];
        sum[j] += static_cast<double>(acc[j]);
        acc[j] = 0.0f;
    }
    accCount = 0;

    const int newest = position;
    position = (position + 1) % ringLength;
    filled = std::min(filled + 1, ringLength);

    // The sum now holds length + 1 entries; bring it towards the wanted length, at most 4 entries per push, so a
    // big period change is absorbed over a few entries instead of in one sample.
    int n = length + 1;
    const int wanted = std::clamp(targetLength, 1, std::min(filled, ringLength - 2));
    for (int step = 0; step < 4 && n != wanted; ++step)
    {
        if (n > wanted)
        {
            const float* old = entry(newest - n + 1);
            for (int j = 0; j < c_demodStride; ++j)
                sum[j] -= static_cast<double>(old[j]);
            --n;
        }
        else
        {
            const float* older = entry(newest - n);
            for (int j = 0; j < c_demodStride; ++j)
                sum[j] += static_cast<double>(older[j]);
            ++n;
        }
    }
    length = n;
}

void SubharmonicEngine::phaseReset()
{
    m_demod.reset();
    m_windowTarget = 0;
    m_historyPosition = 0;
    m_historyCount = 0;
    m_paCounter = 0;
    m_sampleCounter = 0;
    m_onsetSample = 0;
    m_prevValid = false;
    m_batchActive = false;
    m_batchRms = 0.0f;

    for (int k = 0; k <= c_numPartials; ++k)
    {
        m_rotation[k] = 1.0f;
        m_drift[k] = 0.0f;
        m_lastRawPhase[k] = 0.0;
        m_batchX[k] = {};
        m_alignPhase[k] = 0.0f;
        m_alignWeight[k] = 0.0f;
        m_diffSmoothed[k] = {};
        m_corrA[k] = m_corrB[k] = m_corrStepA[k] = m_corrStepB[k] = m_corrTargetA[k] = m_corrTargetB[k] = 0.0f;
        m_corrRampA[k] = m_corrRampB[k] = 0.0f;
        m_turn[k] = 0.0f;
        m_turnValid[k] = false;
        m_modelD[k] = m_modelO[k] = m_modelP[k] = {};
    }
    m_corrRampLeft = 0;
    m_modelS1 = {};
    m_driveModelOk = true;

    m_duckState = 0;
    m_duck = m_duckFrom = 1.0f;
    m_duckPos = m_duckStep = 0.0f;
    m_duckHold = 0;
    m_wetPeak = 0.0f;
    m_wetPeakDecay = static_cast<float>(std::exp(-50.0 / (2.0 * m_sampleRate))); // until the first update: 50 Hz
    m_pendingPhase = m_pendingSubHz = 0.0;
    m_pendingSample = 0;
    m_engage = 0.0f;
    m_settleSeconds = 0.0;

    // The counters run on across resets; everything else describes the new state.
    const auto counters = m_paDiag;
    m_paDiag = {};
    m_paDiag.noteStarts = counters.noteStarts;
    m_paDiag.ducks = counters.ducks;
    m_paDiag.tailGuards = counters.tailGuards;
    m_paDiag.pitchCorrections = counters.pitchCorrections;
    m_paDiag.handOvers = counters.handOvers;
    m_paDiag.running = m_paRunning;
}

void SubharmonicEngine::phaseBeginExit()
{
    // Switched off: the partials turn back to their Classic phases (m_engage falls to 0 over 50 ms), then Classic
    // takes over once no fade is under way (phaseControlUpdate). Until then note starts are still handled as with
    // Phase Align on (phase choice, tail guard), so a quick off / on never fades a note in and out.
    m_paExiting = true;
}

void SubharmonicEngine::startRise()
{
    m_duckFrom = m_duck;
    m_duckState = 2;
    m_duckPos = 0.0f;
    m_duckStep = static_cast<float>(1.0 / (c_duckRiseSeconds * m_sampleRate));
}

void SubharmonicEngine::phaseLevelOnset()
{
    m_onsetSample = m_sampleCounter;

    // Tail guard: a new sound arrives while the last note's tail is still releasing. Classic lets the new sound's
    // rising band level push that stale tail up for the 40-70 ms the tracker needs; fade it out instead and hold the
    // wet at zero until the new note starts (or 300 ms pass). After a long rest the wet may be silent right now (the
    // band level has decayed) while the voicing gate is still open enough for the new sound to push the tail up
    // (to -50 dB and more): that counts as a tail too.
    if (!m_hasTrack && (m_wetPeak > 1.0e-6f || m_gate > c_guardGate) && (m_duckState == 0 || m_duckState == 2))
    {
        m_duckFrom = m_duck;
        m_duckState = 4;
        m_duckPos = 0.0f;
        m_duckStep = static_cast<float>(1.0 / (c_duckFallSeconds * m_sampleRate));
        ++m_paDiag.tailGuards;
    }
}

void SubharmonicEngine::advanceDuck()
{
    // Raised-cosine fades from the current gain: a fall to a pending phase / pitch jump, then a rise (states 1, 2), or
    // a fall to a hold at zero until the new note starts (states 4, 3: the tail guard).
    if (m_duckState == 1 || m_duckState == 4)
    {
        m_duckPos += m_duckStep;

        if (m_duckPos >= 1.0f)
        {
            m_duck = 0.0f;

            if (m_duckState == 1)
            {
                // At the bottom: the new note's pitch and phase (the phase chosen at the note start, carried forward).
                m_logFreq = m_logFreqTarget;
                const double cycles = m_pendingPhase + m_pendingSubHz * static_cast<double>(m_sampleCounter - m_pendingSample) / m_sampleRate;
                m_phase = cycles - std::floor(cycles);
                m_demod.reset();
                m_prevValid = false;
                m_batchActive = true;
                computeWetModel(std::exp(m_logFreq));
                updateAlignment(m_batchX, m_batchRms);

                if (!m_paExiting)
                    m_engage = 1.0f; // nothing to hear here: no need to turn in

                setCorrectionTargets(true);
                startRise();
            }
            else
            {
                m_duckState = 3;
                m_duckHold = static_cast<int>(c_tailGuardSeconds * m_sampleRate);
            }
        }
        else
        {
            m_duck = m_duckFrom * (0.5f + 0.5f * std::cos(c_pi * m_duckPos));
        }
    }
    else if (m_duckState == 3)
    {
        m_duck = 0.0f;

        if (--m_duckHold <= 0) // no note came: let whatever is there come back
            startRise();
    }
    else if (m_duckState == 2)
    {
        m_duckPos += m_duckStep;

        if (m_duckPos >= 1.0f)
        {
            m_duck = 1.0f;
            m_duckState = 0;
        }
        else
        {
            m_duck = m_duckFrom + (1.0f - m_duckFrom) * (0.5f - 0.5f * std::cos(c_pi * m_duckPos));
        }
    }
}

void SubharmonicEngine::computeWetModel(double subHz)
{
    const float cleanWeight = std::cos(m_distortion * c_pi * 0.5f);
    const float harmWeight  = std::sin(m_distortion * c_pi * 0.5f);
    m_modelS1 = { 0.0f, -cleanWeight * m_subLevel };

    // Drive path: the shaper's Fourier series (table) x the harmonic filter's exact response x the small-signal gain
    // of the resonance drive. Exact for Resonance 0; not used for corrections above 2 %.
    float cur[17] {}, prev[17] {};
    lookupShaperHarmonics(m_type, m_shaper.drive, m_shaper.harmonicGain / m_shaper.norm, cur);
    if (m_typeFade < 1.0f)
        lookupShaperHarmonics(m_previousType, m_previousShaper.drive, m_previousShaper.harmonicGain / m_previousShaper.norm, prev);

    const float cutoff = std::clamp(m_toneHz * std::exp2(m_envAmount * m_filterEnv), 20.0f, m_fs * 0.45f);
    const float gHarm = std::tan(c_pi * cutoff / m_fs);
    const float kHarm = resonanceK(m_resonance);
    const float resDrive = 1.0f + (c_maxResonanceDrive - 1.0f) * m_resonance;
    const float resMix = std::min(1.0f, m_resonance * 3.0f);
    const float resGain = 1.0f + resMix * (resDrive / std::tanh(resDrive) - 1.0f);
    m_driveModelOk = m_resonance < 0.02f;

    // Post chain: the post lowpass, then the Lo Cut as it is mixed right now (off path = the dry sub or the matching
    // allpass, plus mix x (high-pass - off path)).
    static constexpr float stageK[4] { 1.0f / 0.50980f, 1.0f / 0.60135f, 1.0f / 0.89998f, 1.0f / 2.56292f };
    static constexpr float allpassK[2] { 1.0f / 0.54120f, 1.0f / 1.30656f };
    const float fade = m_loCutFade;
    const float loCutMix = fade * fade * fade * (fade * (6.0f * fade - 15.0f) + 10.0f);
    const bool overtonesOn = m_overtoneAmount > 1.0e-4f || m_target.overtoneAmount > 1.0e-4f;

    for (int k = 1; k <= c_numPartials; ++k)
    {
        const double f = subHz * k;
        if (!(f < m_sampleRate * 0.45))
        {
            m_modelD[k] = m_modelO[k] = m_modelP[k] = {};
            continue;
        }

        const float w = std::tan(static_cast<float>(c_pi * f / m_sampleRate));
        C2 lp, bp, hp;

        svfResponses(w, m_postFilter.g, SvfLowpass::k, lp, bp, hp);
        const C2 post = lp;

        C2 high { 1.0f, 0.0f };
        for (int s = 0; s < 4; ++s)
        {
            svfResponses(w, m_loCutG, stageK[s], lp, bp, hp);
            high = cmul(high, hp);
        }

        C2 off { 1.0f, 0.0f };
        if (!m_loCutDryOff)
            for (int s = 0; s < 2; ++s)
            {
                svfResponses(w, m_loCutG, allpassK[s], lp, bp, hp);
                off = cmul(off, { 1.0f - 2.0f * allpassK[s] * bp.re, -2.0f * allpassK[s] * bp.im });
            }

        const C2 loCut { off.re + loCutMix * (high.re - off.re), off.im + loCutMix * (high.im - off.im) };
        const C2 p = cmul(post, loCut);
        m_modelP[k] = { p.re, p.im };

        float g = cur[k];
        if (m_typeFade < 1.0f)
            g = prev[k] + m_typeFade * (cur[k] - prev[k]);
        if (k == 1)
            g = 0.0f; // the residual has no fundamental by construction

        svfResponses(w, gHarm, kHarm, lp, bp, hp);
        const C2 filt { (m_modeWeights[0] + m_modeWeights[2]) * lp.re + m_modeWeights[1] * kHarm * bp.re + m_modeWeights[2] * hp.re,
                        (m_modeWeights[0] + m_modeWeights[2]) * lp.im + m_modeWeights[1] * kHarm * bp.im + m_modeWeights[2] * hp.im };
        const C2 shaperPartial = (k % 2 == 0) ? C2 { g, 0.0f } : C2 { 0.0f, -g };
        const C2 d = cmul(shaperPartial, filt);
        m_modelD[k] = { harmWeight * resGain * d.re, harmWeight * resGain * d.im };

        m_modelO[k] = (k >= 2 && overtonesOn) ? Cpx { 0.0f, -m_overtoneAmount * m_overtoneGain[k] } : Cpx {};
    }
}

void SubharmonicEngine::batchEstimate(double f0, Cpx* X, float& rms) const
{
    // The input's even partials of the sub (k = 2m: its own harmonics m * f0) over exactly one input period ending at
    // the newest history sample, against a reference phasor that is 0 at that sample. One input period nulls every
    // other harmonic of f0 (and DC). Strided by the decimation factor, so the cost is the same at any sample rate.
    for (int k = 0; k <= c_numPartials; ++k)
        X[k] = {};
    rms = 0.0f;

    if (!(f0 > 1.0) || m_historyLength <= 0)
        return;

    const int stride = m_decimation;
    const double period = std::min(m_sampleRate / f0, static_cast<double>(m_historyLength - 2 * stride - 2));

    if (static_cast<double>(m_historyCount) < period + stride + 1)
        return; // switched on less than one input period ago: no estimate

    const int steps = static_cast<int>(period / stride);
    const double frac = period / stride - steps;
    const double w = c_twoPi * f0 / m_sampleRate * stride;
    const double cr = std::cos(w), ci = std::sin(w);

    double accRe[9] {}, accIm[9] {}, ms = 0.0, mean = 0.0, weightSum = 0.0;
    double zr = 1.0, zi = 0.0;
    for (int i = 0; i <= steps; ++i)
    {
        const double weight = (i < steps) ? 1.0 : frac;
        int index = (m_historyPosition - 1 - i * stride) % m_historyLength;
        if (index < 0)
            index += m_historyLength;
        const double v = m_history[static_cast<size_t>(index)];
        const double x = weight * v;

        double pr = zr, pi = zi;
        for (int m = 1; m <= 8; ++m)
        {
            accRe[m] += x * pr;
            accIm[m] += x * pi;
            const double nr = pr * zr - pi * zi;
            pi = pr * zi + pi * zr;
            pr = nr;
        }
        ms += x * v;
        mean += x;
        weightSum += weight;

        const double nr = zr * cr - zi * ci;
        zi = zr * ci + zi * cr;
        zr = nr;
    }

    if (weightSum <= 0.0)
        return;

    for (int m = 1; m <= 8; ++m)
        X[2 * m] = { static_cast<float>(2.0 * accRe[m] / weightSum), static_cast<float>(2.0 * accIm[m] / weightSum) };

    // RMS without the input's DC, like the level follower (which follows the DC-blocked copy)
    mean /= weightSum;
    rms = static_cast<float>(std::sqrt(std::max(0.0, ms / weightSum - mean * mean)));
}

void SubharmonicEngine::updateAlignment(const Cpx* X, float rmsWindow)
{
    // Per partial: how clearly the input has a partial there (coherence: its share of the input's amplitude), whether
    // it is phase-stable on the sub's grid (leakage of a neighbour rotates), and its level re the strongest partial.
    float xMax = 0.0f;
    for (int k = 1; k <= c_numPartials; ++k)
        xMax = std::max(xMax, std::sqrt(X[k].re * X[k].re + X[k].im * X[k].im));

    for (int k = 1; k <= c_numPartials; ++k)
    {
        const float mag = std::sqrt(X[k].re * X[k].re + X[k].im * X[k].im);
        // The sub itself (k = 1) is the wet's biggest partial and the input rarely has anything there (only with a
        // tracker octave error or real sub-octave content): it needs much stronger evidence.
        const float lo = (k == 1) ? c_coherenceLowSub : c_coherenceLow;
        const float hi = (k == 1) ? c_coherenceHighSub : c_coherenceHigh;
        const float coherence = smoothstep(mag / (1.41421356f * rmsWindow + 1.0e-12f), lo, hi);
        const float stable = smoothstep(-m_rotation[k], -c_rotationUnstable, -c_rotationStable);
        const float relDb = 20.0f * std::log10(mag / (xMax + 1.0e-20f) + 1.0e-12f);
        const float alpha = (mag > 1.0e-6f) ? coherence * stable * smoothstep(relDb, c_alignFloorDb, c_alignFullDb) : 0.0f;

        m_alignWeight[k] = alpha;
        if (alpha > 0.0f)
            m_alignPhase[k] = std::atan2(X[k].im, X[k].re);
    }
}

void SubharmonicEngine::setCorrectionTargets(bool snap)
{
    // Smoothed over half a sub period (at least 4 ms): a one-period measurement of a slightly mismatched input
    // wobbles at 1-2 grid steps per period, and passing that onto partials near 2 kHz would put sidebands above it.
    const double subHz = std::exp(m_logFreq);
    const double seconds = std::max(c_coefSmoothSeconds, 0.5 / std::max(subHz, 1.0));
    const float c = static_cast<float>(1.0 - std::exp(-m_paUpdateInterval / (seconds * m_sampleRate)));
    const float e = m_engage * m_engage * m_engage * (m_engage * (6.0f * m_engage - 15.0f) + 10.0f);
    auto flush = [](float& v) { if (std::abs(v) < 1.0e-15f) v = 0.0f; }; // decaying one-poles must not go denormal

    for (int k = 1; k <= c_numPartials; ++k)
    {
        // Classic's partial k (per unit level, before the post chain); the Drive path only where its model is exact
        Cpx base = m_modelO[k];
        if (k == 1) { base.re += m_modelS1.re; base.im += m_modelS1.im; }
        if (m_driveModelOk) { base.re += m_modelD[k].re; base.im += m_modelD[k].im; }

        // Aligned: turned (by alpha of the way) onto the input's phase at the output, after the post lowpass and the
        // Lo Cut (hence - arg P). The magnitude stays the preset's.
        Cpx aligned = base;
        const float pMag = std::sqrt(m_modelP[k].re * m_modelP[k].re + m_modelP[k].im * m_modelP[k].im);
        const float alpha = (pMag > 1.0e-4f) ? m_alignWeight[k] : 0.0f;
        const float baseMag = std::sqrt(base.re * base.re + base.im * base.im);

        if (alpha > 0.0f && baseMag > 1.0e-9f)
        {
            const float current = std::atan2(base.im, base.re);
            const float wanted = m_alignPhase[k] - std::atan2(m_modelP[k].im, m_modelP[k].re);
            const float angle = current + alpha * wrapPi(wanted - current);
            aligned = { baseMag * std::cos(angle), baseMag * std::sin(angle) };
        }

        m_paDiag.alignWeight[k] = alpha;

        Cpx& ds = m_diffSmoothed[k];
        const Cpx d { aligned.re - base.re, aligned.im - base.im };
        if (snap)
            ds = d;
        else
        {
            ds.re += c * (d.re - ds.re);
            ds.im += c * (d.im - ds.im);
        }
        flush(ds.re);
        flush(ds.im);

        // Engage: the partial turns from Classic (base) to base + ds along an arc (constant magnitude), so switching
        // on or off never passes a partial through a smaller level. The turn is unwrapped against the last update's:
        // a partial about half a turn away would otherwise flip between turning left and right as the measurement
        // wobbles, and jump across the circle mid-turn.
        Cpx corr {};
        if (e >= 1.0f)
        {
            corr = ds;
            m_turnValid[k] = false;
        }
        else if (e > 0.0f)
        {
            const float br = base.re + ds.re, bi = base.im + ds.im;
            const float ma = baseMag, mb = std::sqrt(br * br + bi * bi);

            if (ma < 1.0e-9f || mb < 1.0e-9f)
            {
                corr = { e * ds.re, e * ds.im };
                m_turnValid[k] = false;
            }
            else
            {
                constexpr float twoPi = 6.28318530717959f;
                const float pa = std::atan2(base.im, base.re);
                float turn = wrapPi(std::atan2(bi, br) - pa);

                if (m_turnValid[k])
                {
                    if (turn - m_turn[k] > 0.5f * twoPi)
                        turn -= twoPi;
                    else if (turn - m_turn[k] < -0.5f * twoPi)
                        turn += twoPi;
                }

                m_turn[k] = turn;
                m_turnValid[k] = true;

                const float m = ma + e * (mb - ma);
                const float a = pa + e * turn;
                corr = { m * std::cos(a) - base.re, m * std::sin(a) - base.im };
            }
        }
        else
        {
            m_turnValid[k] = false;
        }

        // Re{C e^{jk phi}} = C.re cos(k phi) - C.im sin(k phi)
        float ta = corr.re, tb = -corr.im;
        flush(ta);
        flush(tb);
        flush(m_corrA[k]); flush(m_corrB[k]); flush(m_corrStepA[k]); flush(m_corrStepB[k]);
        flush(m_corrTargetA[k]); flush(m_corrTargetB[k]);

        if (snap)
        {
            m_corrTargetA[k] = m_corrStepA[k] = m_corrA[k] = ta;
            m_corrTargetB[k] = m_corrStepB[k] = m_corrB[k] = tb;
            m_corrRampA[k] = m_corrRampB[k] = 0.0f;
        }
        else
        {
            // Reached linearly over the next update interval: a staircase at the update rate (about 1.4 kHz) would
            // put sidebands of the partials near 2 kHz above it while they turn.
            m_corrRampA[k] = (ta - m_corrTargetA[k]) / static_cast<float>(m_paUpdateInterval);
            m_corrRampB[k] = (tb - m_corrTargetB[k]) / static_cast<float>(m_paUpdateInterval);
        }
    }

    m_corrRampLeft = snap ? 0 : m_paUpdateInterval;
}

void SubharmonicEngine::phaseNoteStart(bool correction)
{
    // A new note: estimate the input's partials over its last period at the new pitch, choose the sub's phase, and
    // jump pitch and phase together where that is inaudible (at once if nothing is sounding, else at the bottom of an
    // 8 ms fade of the tail).
    const double subHz = std::exp(m_logFreqTarget);

    Cpx X[c_numPartials + 1];
    float rms = 0.0f;
    batchEstimate(m_fundamental, X, rms);
    computeWetModel(subHz);

    const float amplitude = std::min(4.0f, std::sqrt(2.0f * std::max(0.0f, m_bandEnv2)));

    // Reference for the 2-fold choice (and for no estimate at all): the phase the sub would have now had it started
    // at 0 when the level gate opened. Deterministic per hit, whatever the oscillator was doing before.
    const double reference = wrapPi(c_twoPi * subHz * static_cast<double>(m_sampleCounter - m_onsetSample) / m_sampleRate);
    double delta = reference;

    // Maximise the overlap of the wet's even partials with the input's harmonics (only those are known at a note
    // start): J(d) = sum_k Re{conj(X_k) P_k C_k e^{jkd}}. That fixes the phase modulo pi; the reference picks the branch.
    {
        const auto& grid = phaseGrid();
        float values[c_gridSize] {};
        float sumMagnitude = 0.0f;

        for (int k = 2; k <= c_numPartials; k += 2)
        {
            const C2 c { m_modelO[k].re + m_modelD[k].re, m_modelO[k].im + m_modelD[k].im };
            const C2 w = cmul({ m_modelP[k].re, m_modelP[k].im }, c);
            const C2 q { X[k].re * w.re + X[k].im * w.im, X[k].re * w.im - X[k].im * w.re }; // conj(X) * W
            sumMagnitude += std::sqrt(q.re * q.re + q.im * q.im);

            for (int j = 0; j < c_gridSize; ++j)
                values[j] += q.re * grid.c[k][j] - q.im * grid.s[k][j];
        }

        float value = 0.0f;
        const float pos = trackOptimum(values, 0.0f, 0.0f, value);

        if (sumMagnitude > 1.0e-12f && value > 0.2f * sumMagnitude)
        {
            const double d0 = c_twoPi * pos / c_gridSize;
            const double d1 = d0 + c_twoPi * 0.5;
            delta = std::abs(wrapPi(d0 - reference)) <= std::abs(wrapPi(d1 - reference)) ? d0 : d1;
        }
    }

    // The input's partials relative to the oscillator once it has the chosen phase.
    for (int k = 1; k <= c_numPartials; ++k)
    {
        const float a = static_cast<float>(-k * delta);
        const float ca = std::cos(a), sa = std::sin(a);
        m_batchX[k] = { X[k].re * ca - X[k].im * sa, X[k].re * sa + X[k].im * ca };
        m_rotation[k] = (k % 2 == 0) ? 0.0f : 1.0f; // one exact input period vouches for the even partials
        m_drift[k] = 0.0f;
    }
    m_batchRms = rms;

    // The chosen phase holds at the newest history sample (the last one processed, where m_phase is now); a later jump
    // carries it forward at the new pitch.
    m_pendingPhase = delta / c_twoPi;
    m_pendingSubHz = subHz;
    m_pendingSample = m_sampleCounter;

    if (correction)
        ++m_paDiag.pitchCorrections;
    else
        ++m_paDiag.noteStarts;

    // What the wet is actually putting out right now (its peak, held over about two sub periods); nothing if the tail
    // guard already holds it at zero. A jump under a tail more than 50 dB below the new note's band level is
    // inaudible (its energy above 2 kHz stays about 70 dB under the note); anything louder is faded out first.
    const float possible = (m_duckState == 3) ? 0.0f : m_wetPeak;

    if (possible < c_duckAudible * std::max(amplitude, 1.0e-4f))
    {
        m_logFreq = m_logFreqTarget;
        m_phase = m_pendingPhase - std::floor(m_pendingPhase);
        m_demod.reset();
        m_prevValid = false;
        m_batchActive = true;
        updateAlignment(m_batchX, m_batchRms);

        // Snap the synthesis to the new note only if nothing can be heard (held at zero, or the voicing gate is still
        // closed); a quiet tail can hide a level that is not, and a snap there would be a step.
        const bool held = m_duckState == 3;
        const bool snap = held || m_gate < 1.0e-3f;
        if (snap && !m_paExiting)
            m_engage = 1.0f; // nothing to hear: no need to turn in

        setCorrectionTargets(snap);

        // The new note rises out of the guard's fade or hold, or out of a fall whose jump has just been made here (from
        // where it is).
        if (m_duckState == 1 || m_duckState == 3 || m_duckState == 4)
            startRise();
    }
    else
    {
        // The fall must be slow next to the gap between the highest partials and 2 kHz (a raised cosine of T spreads
        // a partial by about 2 / T): 8 ms keeps a tail's H16 near 1.7 kHz below 2 kHz. It starts from the current
        // gain (a fall or rise may already be under way).
        m_duckFrom = m_duck;
        m_duckState = 1;
        m_duckPos = 0.0f;
        m_duckStep = static_cast<float>(1.0 / (c_duckFallSeconds * m_sampleRate));
        ++m_paDiag.ducks;
    }
}

void SubharmonicEngine::phaseControlUpdate()
{
    const double subHz = std::exp(m_logFreq);
    const double dt = m_paUpdateInterval / m_sampleRate;

    // The tail detector (the wet's peak) must hold over a whole waveform: decay over two sub periods.
    m_wetPeakDecay = static_cast<float>(std::exp(-subHz / (2.0 * m_sampleRate)));

    // Window: exactly one period of the oscillator, in demodulator entries.
    const double window = std::clamp(m_sampleRate / (m_decimation * subHz), 4.0, static_cast<double>(m_demod.ringLength - 3));
    m_windowTarget = static_cast<int>(window);

    // --- Measurement -------------------------------------------------------------------------------
    // Valid once the window has filled after the last reset and its length is within a few entries of one period
    // (0.5 %: leakage of a neighbour < -46 dB); the fractional entry is weighted in.
    Cpx X[c_numPartials + 1] {};
    float rmsWindow = 0.0f;
    bool haveX = false;
    const bool batchWasActive = m_batchActive;
    const int lengthError = std::abs(m_demod.length - m_windowTarget);
    const bool valid = m_demod.filled > m_demod.length && m_demod.length >= 4
                    && lengthError <= std::max(2, m_windowTarget / 200);

    if (valid)
    {
        const float* beyond = m_demod.entry(m_demod.position - 1 - m_demod.length);
        const double frac = std::clamp(window - m_demod.length, 0.0, 1.0);
        const double weight = (static_cast<double>(m_demod.length) + frac) * m_decimation;

        for (int k = 1; k <= c_numPartials; ++k)
        {
            const int j = 2 * (k - 1);
            X[k] = { static_cast<float>(2.0 * (m_demod.sum[j] + frac * beyond[j]) / weight),
                     static_cast<float>(2.0 * (m_demod.sum[j + 1] + frac * beyond[j + 1]) / weight) };
        }

        rmsWindow = static_cast<float>(std::sqrt(std::max(0.0, (m_demod.sum[c_energyIndex] + frac * beyond[c_energyIndex]) / weight)));
        haveX = true;
        m_batchActive = false;
    }
    else if (m_batchActive)
    {
        for (int k = 1; k <= c_numPartials; ++k)
            X[k] = m_batchX[k];
        rmsWindow = m_batchRms;
        haveX = true;
    }

    // Phase stability per partial: a real partial on the sub's grid barely rotates in the oscillator's frame; leakage
    // of a mistuned neighbour rotates at about one grid spacing per update. Smoothed over about one sub period.
    {
        const double spacing = c_twoPi * subHz * dt; // phase a neighbour one grid step away gains per update
        const float smooth = static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, window * m_decimation / m_paUpdateInterval)));

        for (int k = 1; k <= c_numPartials; ++k)
        {
            if (valid)
            {
                const double phase = std::atan2(static_cast<double>(X[k].im), static_cast<double>(X[k].re));

                if (m_prevValid)
                {
                    const double step = wrapPi(phase - m_lastRawPhase[k]); // the input's own drift
                    m_rotation[k] += smooth * (std::min(static_cast<float>(std::abs(step) / spacing), 2.0f) - m_rotation[k]);
                    m_drift[k] += smooth * (static_cast<float>(step) - m_drift[k]);
                    if (std::abs(m_drift[k]) < 1.0e-20f)
                        m_drift[k] = 0.0f;
                }
                else
                {
                    // First usable window after a reset: the note-start estimate vouched for the even partials (one exact
                    // input period at the tracker's pitch); anything else is unknown until measured twice.
                    m_rotation[k] = (batchWasActive && k % 2 == 0) ? 0.0f : 1.0f;
                    m_drift[k] = 0.0f;
                }

                m_lastRawPhase[k] = phase;
            }
            else if (m_batchActive)
            {
                m_rotation[k] = (k % 2 == 0) ? 0.0f : 1.0f; // as above, while running on the note-start estimate
                m_drift[k] = 0.0f;
            }
            else
            {
                m_rotation[k] = 1.0f;
                m_drift[k] = 0.0f;
            }
        }

        m_prevValid = valid;
    }

    // Predict each phase-stable partial forward over the measurement's lag (half a window) by its own measured drift,
    // so a partial that glides with the input (vibrato, a slow drop) is followed without lagging.
    if (valid)
    {
        const double ticksAgo = 0.5 * window * m_decimation / m_paUpdateInterval;

        for (int k = 1; k <= c_numPartials; ++k)
        {
            const float stable = smoothstep(-m_rotation[k], -c_rotationUnstable, -c_rotationStable);
            if (stable <= 0.0f)
                continue;

            const float a = static_cast<float>(stable * m_drift[k] * ticksAgo);
            const float ca = std::cos(a), sa = std::sin(a);
            X[k] = { X[k].re * ca - X[k].im * sa, X[k].re * sa + X[k].im * ca };
        }
    }

    // --- Model, evidence ---------------------------------------------------------------------------
    computeWetModel(subHz);

    const float amplitude = std::min(4.0f, std::sqrt(2.0f * std::max(0.0f, m_bandEnv2)));
    const bool playing = m_hasTrack && m_gateTarget > 0.5f;
    const bool usingOnsetEstimate = !valid && m_batchActive;

    // A one-period average is only exact for stationary partials. While the input level moves fast (attack, release,
    // a hit's first milliseconds) the window and the 20 ms level follower disagree: hold the alignment until they
    // agree within 3 dB (the note-start estimate covers the attack).
    bool steady = true;
    if (haveX && !usingOnsetEstimate)
    {
        const float windowMs = rmsWindow * rmsWindow, followerMs = std::max(m_inputMeanSquare, 1.0e-20f);
        steady = windowMs < 2.0f * followerMs && followerMs < 2.0f * windowMs;
    }

    // Otherwise the last alignment is held: during a release (a fading tail keeps its phases) and while the window is
    // briefly unusable (a sudden period change), rather than falling back to the Classic phases.
    const bool measured = playing && haveX && steady;
    if (measured)
        updateAlignment(X, rmsWindow);

    // --- Engage ------------------------------------------------------------------------------------
    // On: the partials turn onto the input's phases only once the alignment has settled behind a measurement (its
    // weights build up over about two sub periods and it is smoothed over half a period; turning in earlier would
    // turn them towards a moving point, along a chord through a smaller level), and only while there is a
    // measurement. Off: they always turn back. With nothing to hear, either end is reached at once.
    const bool silent = amplitude * m_gate * m_duck < c_silentLevel;
    if (m_paExiting)
        m_engage = silent ? 0.0f : std::max(0.0f, m_engage - m_engageStep);
    else if (silent)
        m_engage = 1.0f;
    else if (measured)
    {
        m_settleSeconds += dt;
        if (m_settleSeconds >= 2.0 / subHz + 4.0 * std::max(c_coefSmoothSeconds, 0.5 / subHz))
            m_engage = std::min(1.0f, m_engage + m_engageStep);
    }

    setCorrectionTargets(silent);

    m_paDiag.running = true;
    m_paDiag.engaged = m_engage * m_engage * m_engage * (m_engage * (6.0f * m_engage - 15.0f) + 10.0f);
    m_paDiag.measurementValid = valid;
    m_paDiag.usingOnsetEstimate = usingOnsetEstimate;

    // --- Hand back to Classic once nothing of the alignment is left ------------------------------------
    if (m_paExiting && m_engage <= 0.0f && m_duckState == 0)
    {
        bool settled = true;
        for (int k = 1; k <= c_numPartials && settled; ++k)
            settled = std::abs(m_corrA[k]) < 1.0e-7f && std::abs(m_corrB[k]) < 1.0e-7f
                   && std::abs(m_corrStepA[k]) < 1.0e-7f && std::abs(m_corrStepB[k]) < 1.0e-7f
                   && std::abs(m_corrTargetA[k]) < 1.0e-7f && std::abs(m_corrTargetB[k]) < 1.0e-7f;

        if (settled)
        {
            for (int k = 0; k <= c_numPartials; ++k)
            {
                m_corrA[k] = m_corrB[k] = m_corrStepA[k] = m_corrStepB[k] = m_corrTargetA[k] = m_corrTargetB[k] = 0.0f;
                m_corrRampA[k] = m_corrRampB[k] = 0.0f;
            }
            m_corrRampLeft = 0;

            m_paRunning = false;
            m_paExiting = false;
            ++m_paDiag.handOvers;
            m_paDiag.running = false;
            m_paDiag.engaged = 0.0f;
            m_paDiag.measurementValid = false;
            m_paDiag.usingOnsetEstimate = false;
            for (auto& weight : m_paDiag.alignWeight)
                weight = 0.0f;
        }
    }
}

bool SubharmonicEngine::stateIsFinite() const
{
    // Every recursive state in one sum: NaN or inf anywhere makes the sum non-finite.
    float sum = m_inputMeanSquare + m_bandLp1.z1 + m_bandLp1.z2 + m_bandLp2.z1 + m_bandLp2.z2
              + m_bandEnv1 + m_bandEnv2 + m_gate + m_filterEnv
              + m_harmonicFilter.ic1 + m_harmonicFilter.ic2 + m_postFilter.ic1 + m_postFilter.ic2
              + m_distortion + m_toneHz + m_postLowpassHz + m_resonance + m_envAmount
              + m_overtoneAmount + m_subLevel;

    for (const auto& stage : m_loCut)
        sum += stage.ic1 + stage.ic2;

    for (const auto& stage : m_loCutAllpass)
        sum += stage.ic1 + stage.ic2;

    sum += m_loCutFade + m_loCutG;

    for (const float gain : m_overtoneGain)
        sum += gain;

    if (m_paRunning)
    {
        float align = m_duck + m_wetPeak + m_engage;
        double sums = 0.0;

        for (int k = 0; k <= c_numPartials; ++k)
            align += m_corrA[k] + m_corrB[k] + m_corrStepA[k] + m_corrStepB[k] + m_corrTargetA[k] + m_corrTargetB[k]
                   + m_corrRampA[k] + m_corrRampB[k] + m_diffSmoothed[k].re + m_diffSmoothed[k].im + m_rotation[k]
                   + m_drift[k] + m_alignPhase[k];

        for (const double s : m_demod.sum)
            sums += s;

        if (!std::isfinite(align) || !std::isfinite(sums))
            return false;
    }

    return std::isfinite(sum) && std::isfinite(m_logFreq + m_phase + m_loCutLog + m_loCutLogFast + m_dcIn + m_dcOut);
}

template <bool aligned>
int SubharmonicEngine::processSamples(const float* in, float* out, float* residual, int start, int numSamples, BlockConstants& block)
{
    const float openMs = block.openMs;
    const float closeMs = block.closeMs;
    const float distortionTarget = block.distortionTarget;
    const float toneTarget = block.toneTarget;
    const float postTarget = block.postTarget;
    const float resonanceTarget = block.resonanceTarget;
    const float envAmountTarget = block.envAmountTarget;
    const float thresholdDb = block.thresholdDb;
    const double loCutSetLog = block.loCutSetLog;
    const double loCutRestLog = block.loCutRestLog;
    float filterK = block.filterK;

    int i = start;
    for (; i < numSamples; ++i)
    {
        // NaN, inf and absurd values are silence; nothing else can poison the states below.
        float x = in[i];
        if (!(std::abs(x) < c_maxInput))
            x = 0.0f;

        [[maybe_unused]] const float xDry = x; // Phase Align measures the partials of the input itself (the dry the host mixes)

        // DC is not a note, but it would pass the detection low-pass, hold the level gate
        // open and give YIN a "perfect" dip at its shortest lag (a phantom note at Max Pitch).
        // This copy of the input is only analysed; the sub itself is synthesised.
        const double blocked = static_cast<double>(x) - m_dcIn + m_dcCoeff * m_dcOut;
        m_dcIn = x;
        m_dcOut = blocked;
        x = static_cast<float>(blocked);

        // --- Input level gate (full band) --------------------------------------
        m_inputMeanSquare += m_levelCoeff * (x * x - m_inputMeanSquare);

        [[maybe_unused]] const bool wasOpen = m_levelOpen;

        if (m_levelOpen)
            m_levelOpen = m_inputMeanSquare >= closeMs;
        else
            m_levelOpen = m_inputMeanSquare >= openMs;

        if constexpr (aligned)
        {
            if (m_levelOpen && !wasOpen)
                phaseLevelOnset();
        }

        // --- Detection band: amplitude follower + pitch ------------------------
        const float band = m_bandLp2.process(m_bandLp1.process(x));

        m_bandEnv1 += m_bandEnvCoeff * (band * band - m_bandEnv1);
        m_bandEnv2 += m_bandEnvCoeff * (m_bandEnv1 - m_bandEnv2);

        if (m_detector.pushSample(band))
            onPitchEstimate();

        // --- Parameter smoothing ------------------------------------------------
        m_distortion    += m_paramCoeff * (distortionTarget - m_distortion);
        m_toneHz        += m_paramCoeff * (toneTarget - m_toneHz);
        m_postLowpassHz += m_paramCoeff * (postTarget - m_postLowpassHz);
        m_resonance     += m_paramCoeff * (resonanceTarget - m_resonance);
        m_envAmount     += m_paramCoeff * (envAmountTarget - m_envAmount);

        for (int m = 0; m < 3; ++m)
            m_modeWeights[m] += m_paramCoeff * ((m == m_target.filterMode ? 1.0f : 0.0f) - m_modeWeights[m]);

        if ((m_shaperCounter % c_filterUpdateInterval) == 0)
        {
            m_postFilter.setCutoff(m_postLowpassHz, m_fs);
            filterK = resonanceK(m_resonance);
        }

        if ((m_shaperCounter % c_shaperUpdateInterval) == 0)
        {
            updateOvertoneTargets(std::exp(m_logFreq));

            const float drive = 1.0f + m_distortion * (c_maxDrive - 1.0f);
            m_shaper = computeShaper(drive, m_type);

            if (m_typeFade < 1.0f)
                m_previousShaper = computeShaper(drive, m_previousType);
        }

        if (++m_shaperCounter >= c_shaperUpdateInterval * c_filterUpdateInterval)
            m_shaperCounter = 0;

        // --- Voicing gate and pitch glide ---------------------------------------
        m_gate += (m_gateTarget > m_gate ? m_gateAttack : m_gateRelease) * (m_gateTarget - m_gate);
        m_logFreq += m_glideCoeff * (m_logFreqTarget - m_logFreq);

        // --- Oscillator ------------------------------------------------------------
        if constexpr (aligned)
            advanceDuck(); // Phase Align: fades, and the inaudible pitch / phase jumps at their bottom

        const double freq = std::exp(m_logFreq);
        m_phase += freq / m_sampleRate;
        m_phase -= std::floor(m_phase);

        const float s = static_cast<float>(std::sin(c_twoPi * m_phase));

        // --- Harmonic residual ---------------------------------------------------
        float harm = harmonicResidual(s, m_shaper, m_type);

        if (m_typeFade < 1.0f)
        {
            const float prev = harmonicResidual(s, m_previousShaper, m_previousType);
            harm = prev + m_typeFade * (harm - prev);
            m_typeFade = std::min(1.0f, m_typeFade + m_typeFadeStep);

            // A type chosen while this fade ran starts as soon as it has finished.
            if (m_typeFade >= 1.0f && m_target.saturationType != m_type)
                startTypeFade(m_target.saturationType);
        }

        const float amplitude = std::min(4.0f, std::sqrt(2.0f * std::max(0.0f, m_bandEnv2)));
        const float level = amplitude * m_gate;

        // --- Envelope follower: sub level mapped threshold..0 dBFS -> 0..1 ---------
        const float levelDb = 20.0f * std::log10(level + 1.0e-9f);
        const float envIn = std::clamp((levelDb - thresholdDb) / -thresholdDb, 0.0f, 1.0f);
        m_filterEnv += (envIn > m_filterEnv ? m_filterEnvAttack : m_filterEnvRelease) * (envIn - m_filterEnv);

        // --- Resonant multimode filter on the harmonics ---------------------------------
        const float cutoff = std::clamp(m_toneHz * std::exp2(m_envAmount * m_filterEnv), 20.0f, m_fs * 0.45f);
        const float g = std::tan(c_pi * cutoff / m_fs);
        const auto f = m_harmonicFilter.process(harm, g, filterK);

        float harmFiltered = m_modeWeights[0] * f.lp
                           + m_modeWeights[1] * f.bp * filterK   // unity-gain bandpass
                           + m_modeWeights[2] * (f.lp + f.hp);   // notch

        // Resonance drive: the resonant peak is pushed into a soft clipper, from
        // pronounced overtones at low settings to full overdrive at maximum.
        const float resDrive = 1.0f + (c_maxResonanceDrive - 1.0f) * m_resonance;
        const float driven = std::tanh(harmFiltered * resDrive) / std::tanh(resDrive);
        harmFiltered += std::min(1.0f, m_resonance * 3.0f) * (driven - harmFiltered);

        // --- Overtones: sin(k*phase) by the Chebyshev recurrence -------------------
        m_overtoneAmount += m_paramCoeff * (m_target.overtoneAmount - m_overtoneAmount);
        float overtones = 0.0f;

        m_subLevel += m_paramCoeff * (m_target.harmonicLevels[1] - m_subLevel);

        if (m_overtoneAmount > 1.0e-4f || m_target.overtoneAmount > 1.0e-4f)
        {
            const float c2 = 2.0f * static_cast<float>(std::cos(c_twoPi * m_phase));
            float prev = 0.0f, curr = s; // sin(0*phase), sin(1*phase)

            for (int k = 2; k <= c_maxOvertone; ++k)
            {
                const float next = c2 * curr - prev;
                prev = curr;
                curr = next;

                m_overtoneGain[k] += m_paramCoeff * (m_overtoneTarget[k] - m_overtoneGain[k]);
                overtones += m_overtoneGain[k] * curr;
            }

            overtones *= m_overtoneAmount;
        }

        // --- Phase Align: additive correction of partials 1..16, and the measurement -----
        [[maybe_unused]] float correction = 0.0f;      // all partials (added to the wet)
        [[maybe_unused]] float correctionHigh = 0.0f;  // k >= 2 (for the harmonics display)

        if constexpr (aligned)
        {
            // cos(k phi), sin(k phi) for k = 1..16 (two Chebyshev recurrences sharing 2 cos phi): they drive both the
            // correction (any amplitude and phase per partial) and the demodulator.
            const float c1 = static_cast<float>(std::cos(c_twoPi * m_phase));
            const float c2 = 2.0f * c1;
            float ck[c_numPartials + 1], sk[c_numPartials + 1];
            ck[0] = 1.0f; sk[0] = 0.0f; ck[1] = c1; sk[1] = s;
            for (int k = 2; k <= c_numPartials; ++k)
            {
                ck[k] = c2 * ck[k - 1] - ck[k - 2];
                sk[k] = c2 * sk[k - 1] - sk[k - 2];
            }

            // The coefficients follow their (update-rate) targets, ramped linearly between updates, through two
            // audio-rate one-poles (1.5 ms each): their spectrum then falls 12 dB/octave above ~100 Hz on top of the
            // ramp's, so turning a partial near 2 kHz never puts energy above it.
            const float cs = m_coefSmooth;
            if (m_corrRampLeft > 0)
            {
                --m_corrRampLeft;
                for (int k = 1; k <= c_numPartials; ++k)
                {
                    m_corrTargetA[k] += m_corrRampA[k];
                    m_corrTargetB[k] += m_corrRampB[k];
                }
            }

            for (int k = 1; k <= c_numPartials; ++k)
            {
                m_corrStepA[k] += cs * (m_corrTargetA[k] - m_corrStepA[k]);
                m_corrStepB[k] += cs * (m_corrTargetB[k] - m_corrStepB[k]);
                m_corrA[k] += cs * (m_corrStepA[k] - m_corrA[k]);
                m_corrB[k] += cs * (m_corrStepB[k] - m_corrB[k]);
            }

            for (int k = 2; k <= c_numPartials; ++k)
                correctionHigh += m_corrA[k] * ck[k] + m_corrB[k] * sk[k];
            correction = correctionHigh + m_corrA[1] * c1 + m_corrB[1] * s;

            // Demodulate the input (the sanitised dry, denormals flushed): products x e^{-jk phi}, summed per ring
            // entry. The energy channel uses the DC-blocked copy, like the level follower it is compared with.
            const float xd = std::abs(xDry) >= 1.0e-15f ? xDry : 0.0f;
            const float xe = std::abs(x) >= 1.0e-15f ? x : 0.0f;
            float* acc = m_demod.acc;
            for (int k = 1; k <= c_numPartials; ++k)
            {
                acc[2 * (k - 1)]     += xd * ck[k];
                acc[2 * (k - 1) + 1] -= xd * sk[k];
            }
            acc[c_energyIndex] += xe * xe;

            if (++m_demod.accCount >= m_decimation)
                m_demod.push(m_windowTarget > 0 ? m_windowTarget : m_demod.length + 1);

            m_history[static_cast<size_t>(m_historyPosition)] = xd;
            if (++m_historyPosition >= m_historyLength)
                m_historyPosition = 0;
            m_historyCount = std::min(m_historyCount + 1, m_historyLength);
        }

        // --- Output ----------------------------------------------------------------

        // Equal-power crossfade: sine and harmonics are uncorrelated.
        const float cleanWeight = std::cos(m_distortion * c_pi * 0.5f);
        const float harmWeight  = std::sin(m_distortion * c_pi * 0.5f);
        // H1 sets how much of the clean sub itself is heard (0 = overtones only, the
        // MaxxBass "replace" trick for small speakers).
        float wet = level * (cleanWeight * m_subLevel * s + harmWeight * harmFiltered + overtones);

        if constexpr (aligned)
            wet = level * m_duck * (cleanWeight * m_subLevel * s + harmWeight * harmFiltered + overtones + correction);

        float sub = m_postFilter.process(wet);

        // --- Lo cut (48 dB/oct Butterworth: stage Qs 0.510, 0.601, 0.900, 2.563) ---
        // Off is the matching 4th-order Butterworth allpass (Qs 0.541, 1.307): flat in level and
        // in phase with the high-pass, so the on/off crossfade is a pure level change.
        {
            static constexpr float stageK[4] { 1.0f / 0.50980f, 1.0f / 0.60135f, 1.0f / 0.89998f, 1.0f / 2.56292f };
            static constexpr float allpassK[2] { 1.0f / 0.54120f, 1.0f / 1.30656f };

            // Off: once the crossfade to the allpass is over and the sub is silent, hand back to
            // the dry sub and park the (silent) filters at the resting cutoff.
            if (!m_target.loCutOn && !m_loCutDryOff && m_loCutFade <= 0.0f && m_gate < c_loCutSilentGate)
            {
                m_loCutDryOff = true;
                m_loCutLog = m_loCutLogFast = loCutRestLog;
                m_loCutG = loCutCoefficient(m_loCutLog, m_fs);
            }

            // Fully on: from now on switching off goes to the allpass.
            if (m_loCutFade >= 1.0f)
                m_loCutDryOff = false;

            // 30 ms crossfade shaped by smootherstep (C2 at both ends, so it cannot click).
            m_loCutFade = m_target.loCutOn ? std::min(1.0f, m_loCutFade + m_loCutFadeStep)
                                           : std::max(0.0f, m_loCutFade - m_loCutFadeStep);
            const float p = m_loCutFade;
            const float mix = p * p * p * (p * (6.0f * p - 15.0f) + 10.0f);

            // The cutoff follows the knob while on (two-pole glide in the log domain). While off it
            // holds still (the allpass that is heard must not move), or rests low behind the dry sub.
            const bool holdCutoff = !m_target.loCutOn && !m_loCutDryOff;
            const double cutoffTarget = m_target.loCutOn ? loCutSetLog : loCutRestLog;

            if (!holdCutoff && (std::abs(cutoffTarget - m_loCutLog) > 0.0 || std::abs(cutoffTarget - m_loCutLogFast) > 0.0))
            {
                m_loCutLogFast += m_loCutGlideCoeff * (cutoffTarget - m_loCutLogFast);
                m_loCutLog += m_loCutGlideCoeff * (m_loCutLogFast - m_loCutLog);

                if (std::abs(cutoffTarget - m_loCutLog) < 1.0e-6 && std::abs(cutoffTarget - m_loCutLogFast) < 1.0e-6)
                    m_loCutLog = m_loCutLogFast = cutoffTarget;

                const float newG = loCutCoefficient(m_loCutLog, m_fs);

                // Rising cutoff: scale the integrator states with it (first and second integrals),
                // so content above the cutoff keeps its level while the filter moves.
                if (newG > m_loCutG && m_loCutG > 0.0f)
                {
                    const float r = newG / m_loCutG;
                    for (auto& stage : m_loCut) { stage.ic1 *= r; stage.ic2 *= r * r; }
                    for (auto& stage : m_loCutAllpass) { stage.ic1 *= r; stage.ic2 *= r * r; }
                }

                m_loCutG = newG;
            }

            float hp = sub;
            for (size_t stage = 0; stage < m_loCut.size(); ++stage)
                hp = m_loCut[stage].process(hp, m_loCutG, stageK[stage]).hp;

            float ap = sub;
            for (size_t stage = 0; stage < m_loCutAllpass.size(); ++stage)
                ap -= 2.0f * allpassK[stage] * m_loCutAllpass[stage].process(ap, m_loCutG, allpassK[stage]).bp;

            const float off = m_loCutDryOff ? sub : ap;
            sub = mix >= 1.0f ? hp : (mix <= 0.0f ? off : off + mix * (hp - off));
        }

        out[i] = sub;

        if (residual != nullptr)
            residual[i] = level * (harmFiltered + overtones);

        if constexpr (aligned)
        {
            m_wetPeak = std::max(std::abs(sub), m_wetPeak * m_wetPeakDecay);

            if (residual != nullptr)
                residual[i] = level * m_duck * (harmFiltered + overtones + correctionHigh);

            ++m_sampleCounter;
            if (++m_paCounter >= m_paUpdateInterval)
            {
                m_paCounter = 0;
                phaseControlUpdate();

                if (!m_paRunning) // handed back to Classic: the rest of the block runs the Classic loop
                {
                    ++i;
                    break;
                }
            }
        }
    }

    block.filterK = filterK;
    return i;
}

void SubharmonicEngine::process(const float* in, float* out, float* residual, int numSamples)
{
    // Phase Align on / off takes effect here, at the block start. Switching on starts from Classic's exact output
    // (no partial has turned yet); switching on again while turning back carries on from where it is.
    if (m_target.phaseAlign && m_demod.ringLength > 0)
    {
        if (!m_paRunning)
        {
            m_paRunning = true;
            m_paExiting = false;
            phaseReset();
        }
        else if (m_paExiting)
            m_paExiting = false;
    }
    else if (m_paRunning && !m_paExiting)
        phaseBeginExit();

    BlockConstants block;
    block.openMs  = std::pow(10.0f, m_target.thresholdDb / 10.0f);
    block.closeMs = std::pow(10.0f, (m_target.thresholdDb - c_gateHysteresisDb) / 10.0f);

    block.distortionTarget = m_target.distortion;
    block.toneTarget = m_target.toneHz;
    block.postTarget = m_target.postLowpassHz;
    block.resonanceTarget = m_target.resonance;
    block.envAmountTarget = m_target.envAmountOctaves;
    block.thresholdDb = std::min(-1.0f, m_target.thresholdDb);

    if (std::abs(m_target.envReleaseMs - m_lastReleaseMs) > 1.0e-3f)
    {
        m_lastReleaseMs = m_target.envReleaseMs;
        m_filterEnvRelease = onePoleCoeff(m_lastReleaseMs * 0.001, m_sampleRate);
    }

    block.filterK = resonanceK(m_resonance);

    if (std::abs(m_target.glideMs - m_lastGlideMs) > 1.0e-3f)
    {
        m_lastGlideMs = m_target.glideMs;
        m_glideCoeff = 1.0 - std::exp(-1.0 / (static_cast<double>(m_lastGlideMs) * 0.001 * m_sampleRate));
    }

    block.loCutSetLog = std::log(static_cast<double>(m_target.loCutHz));
    block.loCutRestLog = std::log(c_loCutRestHz);

    // The sample loop: Classic, or Phase Align (which may hand back to Classic mid-block).
    int done = 0;
    while (done < numSamples)
        done = m_paRunning ? processSamples<true>(in, out, residual, done, numSamples, block)
                           : processSamples<false>(in, out, residual, done, numSamples, block);

    m_inputRms = std::sqrt(std::max(0.0f, m_inputMeanSquare));

    // Meter activity: the gate times how strong the sub is (full scale from about -12 dBFS).
    const float subAmplitude = std::min(4.0f, std::sqrt(2.0f * std::max(0.0f, m_bandEnv2)));
    m_activity = std::clamp(m_gate * subAmplitude * 4.0f, 0.0f, 1.0f);

    // Flush tiny filter states (belt and braces; the host also sets FTZ).
    if (std::abs(m_bandEnv2) < 1.0e-20f) m_bandEnv1 = m_bandEnv2 = 0.0f;
    if (std::abs(m_dcOut) < 1.0e-20 && std::abs(m_dcIn) < 1.0e-20) m_dcIn = m_dcOut = 0.0;
    if (m_paRunning && m_wetPeak < 1.0e-30f) m_wetPeak = 0.0f;

    // Backstop: with the input sanitised this should never fire. If a recursive state has
    // gone non-finite anyway, start again from silence instead of staying silent for good.
    if (!stateIsFinite())
    {
        reset();

        for (int i = 0; i < numSamples; ++i)
        {
            if (!std::isfinite(out[i]))
                out[i] = 0.0f;

            if (residual != nullptr && !std::isfinite(residual[i]))
                residual[i] = 0.0f;
        }
    }
}

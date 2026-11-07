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
        // as before.
        if ((started && m_gate < c_newNoteGate) || corrected)
            m_logFreq = m_logFreqTarget;

        m_gateTarget = 1.0f;
    }
    else
    {
        m_gateTarget = 0.0f;
        m_fundamental = 0.0f;
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

    return std::isfinite(sum) && std::isfinite(m_logFreq + m_phase + m_loCutLog + m_loCutLogFast + m_dcIn + m_dcOut);
}

void SubharmonicEngine::process(const float* in, float* out, float* residual, int numSamples)
{
    const float openMs  = std::pow(10.0f, m_target.thresholdDb / 10.0f);
    const float closeMs = std::pow(10.0f, (m_target.thresholdDb - c_gateHysteresisDb) / 10.0f);

    const float distortionTarget = m_target.distortion;
    const float toneTarget = m_target.toneHz;
    const float postTarget = m_target.postLowpassHz;
    const float resonanceTarget = m_target.resonance;
    const float envAmountTarget = m_target.envAmountOctaves;
    const float thresholdDb = std::min(-1.0f, m_target.thresholdDb);

    if (std::abs(m_target.envReleaseMs - m_lastReleaseMs) > 1.0e-3f)
    {
        m_lastReleaseMs = m_target.envReleaseMs;
        m_filterEnvRelease = onePoleCoeff(m_lastReleaseMs * 0.001, m_sampleRate);
    }

    float filterK = resonanceK(m_resonance);

    if (std::abs(m_target.glideMs - m_lastGlideMs) > 1.0e-3f)
    {
        m_lastGlideMs = m_target.glideMs;
        m_glideCoeff = 1.0 - std::exp(-1.0 / (static_cast<double>(m_lastGlideMs) * 0.001 * m_sampleRate));
    }

    const double loCutSetLog = std::log(static_cast<double>(m_target.loCutHz));
    const double loCutRestLog = std::log(c_loCutRestHz);

    for (int i = 0; i < numSamples; ++i)
    {
        // NaN, inf and absurd values are silence; nothing else can poison the states below.
        float x = in[i];
        if (!(std::abs(x) < c_maxInput))
            x = 0.0f;

        // DC is not a note, but it would pass the detection low-pass, hold the level gate
        // open and give YIN a "perfect" dip at its shortest lag (a phantom note at Max Pitch).
        // This copy of the input is only analysed; the sub itself is synthesised.
        const double blocked = static_cast<double>(x) - m_dcIn + m_dcCoeff * m_dcOut;
        m_dcIn = x;
        m_dcOut = blocked;
        x = static_cast<float>(blocked);

        // --- Input level gate (full band) --------------------------------------
        m_inputMeanSquare += m_levelCoeff * (x * x - m_inputMeanSquare);

        if (m_levelOpen)
            m_levelOpen = m_inputMeanSquare >= closeMs;
        else
            m_levelOpen = m_inputMeanSquare >= openMs;

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

        // --- Output ----------------------------------------------------------------

        // Equal-power crossfade: sine and harmonics are uncorrelated.
        const float cleanWeight = std::cos(m_distortion * c_pi * 0.5f);
        const float harmWeight  = std::sin(m_distortion * c_pi * 0.5f);
        // H1 sets how much of the clean sub itself is heard (0 = overtones only, the
        // MaxxBass "replace" trick for small speakers).
        const float wet = level * (cleanWeight * m_subLevel * s + harmWeight * harmFiltered + overtones);

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
    }

    m_inputRms = std::sqrt(std::max(0.0f, m_inputMeanSquare));

    // Meter activity: the gate times how strong the sub is (full scale from about -12 dBFS).
    const float subAmplitude = std::min(4.0f, std::sqrt(2.0f * std::max(0.0f, m_bandEnv2)));
    m_activity = std::clamp(m_gate * subAmplitude * 4.0f, 0.0f, 1.0f);

    // Flush tiny filter states (belt and braces; the host also sets FTZ).
    if (std::abs(m_bandEnv2) < 1.0e-20f) m_bandEnv1 = m_bandEnv2 = 0.0f;
    if (std::abs(m_dcOut) < 1.0e-20 && std::abs(m_dcIn) < 1.0e-20) m_dcIn = m_dcOut = 0.0;

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

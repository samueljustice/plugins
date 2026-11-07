#include "SubharmonicEngine.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    constexpr double c_twoPi = 6.283185307179586;
    constexpr float c_pi = 3.14159265358979f;

    constexpr double c_levelTimeSeconds    = 0.020;
    constexpr double c_bandEnvelopeHz      = 12.0;  // 2-pole smoothing of the band RMS
    constexpr double c_gateAttackSeconds   = 0.010;
    constexpr double c_gateReleaseSeconds  = 0.080;
    // Pitch tracker
    constexpr double c_trackHoldSeconds    = 0.25;  // keep the last pitch through dropouts this long
    constexpr double c_jumpConfirmSeconds  = 0.04;  // a big jump must persist this long
    constexpr double c_octaveConfirmSeconds = 0.12; // an octave flip must persist this long
    constexpr float  c_followSemitones     = 3.0f;  // moves within this are followed directly
    constexpr float  c_weakFollowSemitones = 1.5f;  // ... or within this for low-confidence guesses
    constexpr float  c_candidateTolerance  = 1.0f;  // semitones: "the same" candidate
    constexpr float  c_strongConfidence    = 0.8f;  // strict YIN hits; fallback guesses are weaker
    constexpr double c_paramSeconds        = 0.030;
    constexpr double c_typeFadeSeconds     = 0.030;
    constexpr float  c_gateHysteresisDb    = 3.0f;
    constexpr float  c_maxDrive            = 10.0f;
    constexpr int    c_filterUpdateInterval = 8;
    constexpr double c_maxResidualPeak     = 1.5;  // relative to a unit sine
    constexpr double c_filterEnvAttackSecs = 0.003;
    constexpr float  c_maxResonanceDrive   = 5.0f;

    float onePoleCoeff(double seconds, double fs)
    {
        return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * fs)));
    }

    // SVF damping from 0..1 resonance: Q 0.707 -> 20, exponential feel.
    float resonanceK(float resonance)
    {
        constexpr float maxK = 1.41421356f;
        constexpr float minK = 0.05f;
        return maxK * std::pow(minK / maxK, std::clamp(resonance, 0.0f, 1.0f));
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
    m_bandEnvCoeff = static_cast<float>(1.0 - std::exp(-c_twoPi * c_bandEnvelopeHz / m_sampleRate));
    m_gateAttack   = onePoleCoeff(c_gateAttackSeconds, m_sampleRate);
    m_gateRelease  = onePoleCoeff(c_gateReleaseSeconds, m_sampleRate);
    m_lastGlideMs  = -1.0f; // glide coefficient is set from the parameter in process()

    const double hopSeconds = m_detector.getHopSize() / m_sampleRate;
    m_jumpConfirmHops   = std::max(2, static_cast<int>(std::lround(c_jumpConfirmSeconds / hopSeconds)));
    m_octaveConfirmHops = std::max(3, static_cast<int>(std::lround(c_octaveConfirmSeconds / hopSeconds)));
    m_holdLimitHops     = std::max(3, static_cast<int>(std::lround(c_trackHoldSeconds / hopSeconds)));
    m_paramCoeff   = onePoleCoeff(c_paramSeconds, m_sampleRate);
    m_typeFadeStep = static_cast<float>(1.0 / (c_typeFadeSeconds * m_sampleRate));
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
    m_inputRms = 0.0f;
    m_levelOpen = false;
    m_bandEnv1 = m_bandEnv2 = 0.0f;

    m_fundamental = 0.0f;
    m_gate = 0.0f;
    m_gateTarget = 0.0f;
    resetTracker();

    m_phase = 0.0;
    m_logFreq = m_logFreqTarget = std::log(50.0f);

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
    m_loCutHz = m_target.loCutHz;
    m_loCutMix = m_target.loCutOn ? 1.0f : 0.0f;

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
    m_target.distortion = std::clamp(p.distortion, 0.0f, 1.0f);
    m_target.saturationType = std::clamp(p.saturationType, 0, NumSaturationTypes - 1);
    m_target.resonance = std::clamp(p.resonance, 0.0f, 1.0f);
    m_target.filterMode = std::clamp(p.filterMode, 0, 2);
    m_target.envAmountOctaves = std::clamp(p.envAmountOctaves, -6.0f, 6.0f);
    m_target.envReleaseMs = std::clamp(p.envReleaseMs, 1.0f, 5000.0f);
    m_target.glideMs = std::clamp(p.glideMs, 1.0f, 2000.0f);
    m_target.loCutHz = std::clamp(p.loCutHz, 10.0f, 200.0f);
    m_target.overtoneAmount = std::clamp(p.overtoneAmount, 0.0f, 1.0f);
    for (auto& level : m_target.harmonicLevels)
        level = std::clamp(level, 0.0f, 1.0f);

    if (m_target.saturationType != m_type)
    {
        // Start a crossfade from whatever we are currently producing.
        m_previousType = m_type;
        m_previousShaper = m_shaper;
        m_type = m_target.saturationType;
        m_shaper = computeShaper(m_shaper.drive, m_type);
        m_typeFade = 0.0f;
    }

    m_detector.setMaxFrequency(m_target.maxFundamentalHz);
    updateDetectionBand(m_target.maxFundamentalHz);
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
    // Each bar is an amplitude relative to the sub (1.0 = as loud as the sub).
    // Partials that would land above ~0.45 fs are left out (no aliasing). If the
    // bars add up to more than 1.5x the sub's RMS, all of them are scaled down
    // together, so 16 bars at 100% can't explode the level.
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
    m_startHops = 0;
    m_holdHops = 0;
}

void SubharmonicEngine::acceptPitch(float semitones)
{
    m_trackSt = semitones;
    m_hasTrack = true;
    m_candidateHops = 0;
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
    const bool inRange = f0 >= PitchDetector::c_minFrequency && f0 <= m_target.maxFundamentalHz * 1.1f;
    const bool strong = m_detector.getConfidence() >= c_strongConfidence;
    bool used = false;

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
                }
            }
        }
        else
        {
            const float distance = st - m_trackSt;

            // Weak (fallback) guesses may only make small corrections, so they
            // can't walk the track away from the real pitch.
            const float followLimit = strong ? c_followSemitones : c_weakFollowSemitones;

            if (std::abs(distance) <= followLimit)
            {
                acceptPitch(st);
                used = true;
            }
            else if (strong)
            {
                // A big jump only wins if it keeps coming back. Octave flips are
                // the classic detector error, so they need to persist longer.
                const bool octave = std::abs(std::abs(distance) - 12.0f) < c_candidateTolerance;
                const int needed = octave ? m_octaveConfirmHops : m_jumpConfirmHops;

                if (m_candidateHops > 0 && std::abs(st - m_candidateSt) < c_candidateTolerance * 1.5f)
                    ++m_candidateHops;
                else
                {
                    m_candidateSt = st;
                    m_candidateHops = 1;
                }

                if (m_candidateHops >= needed)
                {
                    acceptPitch(m_candidateSt);
                    used = true;
                }
            }
        }
    }

    // Nothing usable this hop: hold the last pitch for a while before letting go.
    if (!used && m_hasTrack && ++m_holdHops > m_holdLimitHops)
        resetTracker();

    if (m_hasTrack)
    {
        m_fundamental = std::exp2(m_trackSt / 12.0f);
        m_logFreqTarget = std::log(m_fundamental * 0.5f);

        // At note onset (gate closed) jump straight to the new pitch instead of
        // gliding from wherever the last note was.
        if (m_gate < 0.02f)
            m_logFreq = m_logFreqTarget;

        m_gateTarget = 1.0f;
    }
    else
    {
        m_gateTarget = 0.0f;
        m_fundamental = 0.0f;
    }
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
        m_glideCoeff = onePoleCoeff(m_lastGlideMs * 0.001, m_sampleRate);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float x = in[i];

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
            updateOvertoneTargets(std::exp(static_cast<double>(m_logFreq)));

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
        const double freq = std::exp(static_cast<double>(m_logFreq));
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
        {
            static constexpr float stageK[4] { 1.0f / 0.50980f, 1.0f / 0.60135f, 1.0f / 0.89998f, 1.0f / 2.56292f };
            m_loCutHz  += m_paramCoeff * (m_target.loCutHz - m_loCutHz);
            m_loCutMix += m_paramCoeff * ((m_target.loCutOn ? 1.0f : 0.0f) - m_loCutMix);

            const float g = std::tan(c_pi * std::clamp(m_loCutHz, 5.0f, m_fs * 0.25f) / m_fs);
            float hp = sub;
            for (size_t stage = 0; stage < m_loCut.size(); ++stage)
                hp = m_loCut[stage].process(hp, g, stageK[stage]).hp;

            sub += m_loCutMix * (hp - sub);
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
}

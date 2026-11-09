#include "SubModule.h"

#include "../MonsterFeatures.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace roar
{
    namespace
    {
        constexpr double c_sqrt2 = 1.41421356237309504880;

        constexpr double c_bandQ = 2.0;
        constexpr double c_band2Ratio = 1.5;            // Thick: band 2 a fifth above band 1
        constexpr float c_band2Gain = 0.5f;             // -6 dB
        constexpr double c_minConfidence = 0.4;
        constexpr double c_minF0Hz = 30.0, c_maxF0Hz = 1500.0;
        constexpr double c_minInputHz = 60.0, c_maxInputHz = 1500.0;   // band 1 centre (sub >= 30 Hz)
        constexpr double c_glideSeconds = 0.030;
        constexpr double c_octaveConfirmSeconds = 0.040;
        constexpr double c_octaveTolerance = 0.125;     // octaves
        constexpr double c_followSwitchHz = 84.0;       // Follow: sub = f0 / 2 below 42 Hz -> divide 2 f0
        constexpr double c_tightSwitchHz = 102.2;       // Tight: sqrt (2 * 55 * 95), the sub nearer 55..95 Hz
        constexpr double c_registerHysteresis = 1.03;
        constexpr double c_estimateRange = 0.68;        // octaves (x 1.6) a divider toggle rate may differ from fIn
        constexpr double c_estimateCoeff = 0.25;

        constexpr double c_envAttackSeconds = 0.001;
        constexpr double c_envReleaseSeconds = 0.030;
        constexpr float c_armFraction = 0.15f;

        constexpr double c_normReleaseSeconds = 0.300;
        constexpr double c_normFloor = 1.0e-3 * c_sqrt2;   // -60 dBFS RMS, as a sine peak

        constexpr double c_betaClean = 0.1, c_betaBiased = 0.25;
        constexpr double c_driveMinDb = -6.0;   // driveDbFor(): -6 + 24 u
        constexpr double c_driveTableStepDb = 0.25;
        constexpr double c_driveSeconds = 0.020;

        constexpr double c_evenQ = 0.7071;
        // Rotation of the doubler (cos, sin of 305 degrees): puts the 2nd harmonic in phase with
        // the divided input partial, so it adds to the voice's own partial instead of thinning it.
        constexpr double c_evenCos = 0.573576436, c_evenSin = -0.819152044;
        constexpr double c_evenDbAtZero = -25.0, c_evenDbPerDb = 5.0 / 6.0, c_evenMaxDb = -12.0;

        constexpr double c_hpRatio = 0.6, c_lpRatio = 1.5;
        constexpr double c_hpQClean = 0.7071, c_hpQResonant = 1.4;
        constexpr double c_lpQ1 = 0.5412, c_lpQ2 = 1.3066;   // 4th-order Butterworth

        constexpr double c_levelFadeBelow = 0.02;
        constexpr double c_gainSeconds = 0.020;

        constexpr float c_expanderThreshold = 1.0e-3f;   // -60 dBFS on band 1's envelope, 1:2
        constexpr double c_expanderAttackSeconds = 0.001;
        constexpr double c_expanderReleaseSeconds = 0.050;

        constexpr double c_levelSeconds = 0.080;

        inline double logCosh (double x)
        {
            const double a = std::abs (x);
            return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994530942;
        }

        inline float sanitizeInput (float x)
        {
            return std::abs (x) < 1.0e4f ? x : 0.0f;   // also false for NaN
        }

        double hpMagnitude (double ratio, double q)   // ratio = f / fc
        {
            const double r2 = ratio * ratio;
            return r2 / std::sqrt ((1.0 - r2) * (1.0 - r2) + r2 / (q * q));
        }

        double lpMagnitude (double ratio, double q)
        {
            const double r2 = ratio * ratio;
            return 1.0 / std::sqrt ((1.0 - r2) * (1.0 - r2) + r2 / (q * q));
        }
    }

    // ------------------------------------------------------------------------------ static design

    double SubModule::levelLawGain (double u)
    {
        if (! (u > 0.0))
            return 0.0;
        u = std::min (u, 1.0);
        // Calibrated on the reference take: on a low voice band 1 divides 2 f0, so "H1" here is the
        // 2nd harmonic, and the reference's octave layer sits well above it (Init's 40-80 Hz share
        // of -3 to -6 dB and 160-500 Hz share of -9 dB or less need about +4 dB at 50 %).
        const auto lawDb = [] (double x) { return -14.0 + 26.0 * std::pow (x, 0.55); };
        if (u < c_levelFadeBelow)
            return dsp::dbToGain (lawDb (c_levelFadeBelow)) * (u / c_levelFadeBelow);
        return dsp::dbToGain (lawDb (u));
    }

    double SubModule::evenDbFor (double driveDb)
    {
        return std::min (c_evenMaxDb, c_evenDbAtZero + c_evenDbPerDb * driveDb);
    }

    double SubModule::levelLawDb (double u)
    {
        const double g = levelLawGain (u);
        return g > 0.0 ? 20.0 * std::log10 (g) : -200.0;
    }

    // ------------------------------------------------------------------------------ lifecycle

    void SubModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);

        for (auto* f : { &m_band1a, &m_band1b, &m_band2a, &m_band2b, &m_hp, &m_lp1, &m_lp2, &m_even })
            f->prepare (m_fs);

        m_envAttack = static_cast<float> (dsp::onePoleCoeff (c_envAttackSeconds, m_fs));
        m_envRelease = static_cast<float> (dsp::onePoleCoeff (c_envReleaseSeconds, m_fs));
        m_expAttack = static_cast<float> (dsp::onePoleCoeff (c_expanderAttackSeconds, m_fs));
        m_expRelease = static_cast<float> (dsp::onePoleCoeff (c_expanderReleaseSeconds, m_fs));
        m_gainCoeff = static_cast<float> (dsp::onePoleCoeff (c_gainSeconds, m_fs));
        m_levelCoeff = static_cast<float> (dsp::onePoleCoeff (c_levelSeconds, m_fs));
        m_normRelease = dsp::poleFromTau (c_normReleaseSeconds, m_fs);
        m_glideCoeff = 1.0 - std::exp (-static_cast<double> (c_voiceFrameSize) / (c_glideSeconds * m_fs));
        m_driveCoeff = static_cast<float> (1.0 - std::exp (-static_cast<double> (c_voiceFrameSize) / (c_driveSeconds * m_fs)));
        m_octaveConfirmSamples = dsp::samplesFor (c_octaveConfirmSeconds, m_fs, 1);

        // The drive's gain on the divided waveform's f / 2 component, in the steady state of the
        // normaliser (level = the band's amplitude A = 1): u = sin (theta) over one input cycle and
        // -sin (theta) over the next, out = f (G sqrt2 u) / sqrt2. The table is its inverse.
        constexpr int points = 2048;
        for (int bi = 0; bi < 2; ++bi)
        {
            const double beta = bi == 0 ? c_betaClean : c_betaBiased;
            const double tb = std::tanh (beta);
            const double cb = std::cosh (beta);
            for (int k = 0; k < c_driveTableSize; ++k)
            {
                const double g = dsp::dbToGain (c_driveMinDb + c_driveTableStepDb * k);
                std::complex<double> acc { 0.0, 0.0 };
                for (int m = 0; m < points; ++m)
                {
                    const double phase = 2.0 * dsp::c_twoPi * (m + 0.5) / points;   // two input cycles
                    const double u = (m < points / 2 ? 1.0 : -1.0) * std::sin (phase);
                    const double y = (std::tanh (g * c_sqrt2 * u + beta) - tb) * cb * cb / c_sqrt2;
                    acc += y * std::polar (1.0, -dsp::c_twoPi * (m + 0.5) / points);
                }
                const double amplitude = 2.0 * std::abs (acc) / points;
                m_driveMakeup[static_cast<size_t> (bi)][static_cast<size_t> (k)] = static_cast<float> (1.0 / std::max (amplitude, 1.0e-6));
            }
        }

        // The output filters' gain at fsub, per character (Clean, Resonant, Thick).
        const double lp = lpMagnitude (1.0 / c_lpRatio, c_lpQ1) * lpMagnitude (1.0 / c_lpRatio, c_lpQ2);
        m_filterMakeup[0] = static_cast<float> (1.0 / (hpMagnitude (1.0 / c_hpRatio, c_hpQClean) * lp));
        m_filterMakeup[1] = static_cast<float> (1.0 / (hpMagnitude (1.0 / c_hpRatio, c_hpQResonant) * lp));
        m_filterMakeup[2] = m_filterMakeup[0];

        // The doubler's 2nd harmonic for a band amplitude of 1 (normaliser = 1): u's harmonics
        // (odd multiples of fsub) through the SVF's low-pass and band-pass at fsub, the rotated
        // doubler, its 2 fsub component. And the output filters' tilt between fsub and 2 fsub.
        {
            constexpr int harmonics = 15;
            std::array<std::complex<double>, harmonics + 1> uHarmonic {};   // complex amplitudes, k x fsub
            for (int k = 1; k <= harmonics; k += 2)
            {
                std::complex<double> acc { 0.0, 0.0 };
                for (int m = 0; m < points; ++m)
                {
                    const double phase = 2.0 * dsp::c_twoPi * (m + 0.5) / points;
                    const double u = (m < points / 2 ? 1.0 : -1.0) * std::sin (phase);
                    acc += u * std::polar (1.0, -dsp::c_twoPi * k * (m + 0.5) / points);
                }
                uHarmonic[static_cast<size_t> (k)] = acc * (2.0 / points);
            }
            std::complex<double> h2 { 0.0, 0.0 };
            for (int m = 0; m < points; ++m)
            {
                const double t = (m + 0.5) / points;   // one sub period
                double lpv = 0.0, bpv = 0.0;
                for (int k = 1; k <= harmonics; k += 2)
                {
                    const std::complex<double> s { 0.0, static_cast<double> (k) };
                    const std::complex<double> den = s * s + s / c_evenQ + 1.0;
                    const std::complex<double> c = uHarmonic[static_cast<size_t> (k)] * std::polar (1.0, dsp::c_twoPi * k * t);
                    lpv += std::real (c / den);
                    bpv += std::real (c * s / den);
                }
                h2 += (c_evenCos * (lpv * lpv - bpv * bpv) + c_evenSin * 2.0 * lpv * bpv) * std::polar (1.0, -dsp::c_twoPi * 2.0 * t);
            }
            const double unitH2 = std::max (2.0 * std::abs (h2) / points, 1.0e-6);
            const auto post = [] (double ratio, double hpQ)
            {
                return hpMagnitude (ratio / c_hpRatio, hpQ) * lpMagnitude (ratio / c_lpRatio, c_lpQ1) * lpMagnitude (ratio / c_lpRatio, c_lpQ2);
            };
            for (int c = 0; c < 3; ++c)
            {
                const double hpQ = c == 1 ? c_hpQResonant : c_hpQClean;
                m_evenScale[static_cast<size_t> (c)] = static_cast<float> (post (1.0, hpQ) / post (2.0, hpQ) / unitH2);
            }
        }

        m_fade.prepare (m_fs, maxBlock, getLatencySamples());
        m_fade.setOn (m_settings.on);
        reset();
    }

    void SubModule::reset()
    {
        m_fade.reset();

        m_hasPitch = false;
        m_f0Log = std::log2 (c_initialInputHz);
        m_candidateLog = 0.0;
        m_candidateSamples = 0;
        m_tracked = false;
        m_multiplier = 1;
        m_inLog = m_f0Log;
        m_subLog = m_f0Log - 1.0;
        m_estimateLog = m_inLog;
        m_controlsValid = false;

        m_driveDb = static_cast<float> (driveDbFor (static_cast<double> (m_settings.drive)));
        m_driveGain = dsp::dbToGain (m_driveDb);
        m_beta = betaIndex() == 0 ? c_betaClean : c_betaBiased;
        m_tanhBeta = std::tanh (m_beta);
        m_coshBeta2 = std::cosh (m_beta) * std::cosh (m_beta);

        resetWet();
        updateFilters();
    }

    void SubModule::resetWet()
    {
        for (auto* f : { &m_band1a, &m_band1b, &m_band2a, &m_band2b, &m_hp, &m_lp1, &m_lp2, &m_even })
            f->reset();
        m_div1 = {};
        m_div2 = {};
        m_norm = 0.0;
        m_adaaX1 = 0.0;
        m_adaaF1 = antiderivative (0.0);
        m_expGain = 0.0f;
        m_levelEnv = 0.0f;
        m_estimateLog = m_inLog;
        m_gainTarget = gainTarget();
        m_gain = m_gainTarget;
        m_evenGain = evenGainTarget();
    }

    void SubModule::setParams (const SubSettings& s)
    {
        m_settings = s;
        m_settings.level = std::isfinite (s.level) ? std::clamp (s.level, 0.0f, 1.0f) : 0.0f;
        m_settings.drive = std::isfinite (s.drive) ? std::clamp (s.drive, 0.0f, 1.0f) : 0.5f;
        if (static_cast<int> (m_settings.reg) < 0 || static_cast<int> (m_settings.reg) > 2)
            m_settings.reg = Params::SubRegister::follow;
        if (static_cast<int> (m_settings.character) < 0 || static_cast<int> (m_settings.character) > 2)
            m_settings.character = Params::SubCharacter::clean;
        m_fade.setOn (s.on);
    }

    // ------------------------------------------------------------------------------ controls

    double SubModule::driveMakeup (double driveDb, int index) const
    {
        const double pos = std::clamp ((driveDb - c_driveMinDb) / c_driveTableStepDb, 0.0, static_cast<double> (c_driveTableSize - 1));
        const int i = std::min (static_cast<int> (pos), c_driveTableSize - 2);
        const double t = pos - i;
        const auto& table = m_driveMakeup[static_cast<size_t> (index)];
        const double a = static_cast<double> (table[static_cast<size_t> (i)]);
        const double b = static_cast<double> (table[static_cast<size_t> (i + 1)]);
        return a + t * (b - a);
    }

    float SubModule::evenGainTarget() const
    {
        // Relative to the driven signal's fundamental, which is 1 / driveMakeup per unit band
        // amplitude; the output filters' tilt is in m_evenScale.
        return static_cast<float> (dsp::dbToGain (evenDbFor (static_cast<double> (m_driveDb)))
                                   * static_cast<double> (m_evenScale[static_cast<size_t> (m_settings.character)])
                                   / driveMakeup (static_cast<double> (m_driveDb), betaIndex()));
    }

    float SubModule::gainTarget() const
    {
        return static_cast<float> (levelLawGain (static_cast<double> (m_settings.level))
                                   * driveMakeup (static_cast<double> (m_driveDb), betaIndex())
                                   * static_cast<double> (m_filterMakeup[static_cast<size_t> (m_settings.character)]));
    }

    void SubModule::updateTracker (const VoiceFrame& frame)
    {
        const double f0 = static_cast<double> (frame.f0Hz);
        const bool usable = std::isfinite (f0) && f0 >= c_minF0Hz && f0 <= c_maxF0Hz
                            && static_cast<double> (frame.confidence) >= c_minConfidence;
        m_tracked = usable;

        if (usable)
        {
            const double lg = std::log2 (f0);
            if (! m_hasPitch)
            {
                m_f0Log = lg;
                m_hasPitch = true;
                m_candidateSamples = 0;
            }
            else
            {
                const double distance = std::abs (lg - m_f0Log);
                if (std::abs (distance - 1.0) >= c_octaveTolerance)
                {
                    m_f0Log = lg;
                    m_candidateSamples = 0;
                }
                else
                {
                    // An octave jump (the classic tracker error) must persist before it is followed.
                    if (m_candidateSamples > 0 && std::abs (lg - m_candidateLog) < c_octaveTolerance)
                        m_candidateSamples += c_voiceFrameSize;
                    else
                    {
                        m_candidateLog = lg;
                        m_candidateSamples = c_voiceFrameSize;
                    }
                    if (m_candidateSamples >= m_octaveConfirmSamples)
                    {
                        m_f0Log = m_candidateLog;
                        m_candidateSamples = 0;
                    }
                }
            }
        }
        else
        {
            m_candidateSamples = 0;
        }

        // Register: which harmonic band 1 divides.
        const double f0Followed = std::exp2 (m_f0Log);
        double switchHz = 0.0;
        if (m_settings.reg == Params::SubRegister::follow)
            switchHz = c_followSwitchHz;
        else if (m_settings.reg == Params::SubRegister::tight)
            switchHz = c_tightSwitchHz;

        if (switchHz <= 0.0)
            m_multiplier = 1;
        else if (! m_controlsValid)
            m_multiplier = f0Followed < switchHz ? 2 : 1;
        else if (m_multiplier == 1 && f0Followed < switchHz / c_registerHysteresis)
            m_multiplier = 2;
        else if (m_multiplier == 2 && f0Followed > switchHz * c_registerHysteresis)
            m_multiplier = 1;

        const double inTarget = std::clamp (m_f0Log + (m_multiplier == 2 ? 1.0 : 0.0),
                                            std::log2 (c_minInputHz), std::log2 (c_maxInputHz));

        double subTarget = inTarget - 1.0;
        if (! m_tracked)
        {
            // Not confident: the output filters follow what the dividers are actually dividing.
            subTarget = std::clamp (m_estimateLog, m_inLog - 0.5, m_inLog + 0.5) - 1.0;
        }

        if (! m_controlsValid)
        {
            m_inLog = inTarget;
            m_subLog = subTarget;
            m_estimateLog = inTarget;
        }
        else
        {
            m_inLog += m_glideCoeff * (inTarget - m_inLog);
            m_subLog += m_glideCoeff * (subTarget - m_subLog);
        }
    }

    void SubModule::updateFilters()
    {
        const double fIn = std::exp2 (m_inLog);
        const double fSub = std::exp2 (m_subLog);
        m_band1a.setParams (fIn, c_bandQ);
        m_band1b.setParams (fIn, c_bandQ);
        m_band2a.setParams (fIn * c_band2Ratio, c_bandQ);
        m_band2b.setParams (fIn * c_band2Ratio, c_bandQ);
        const bool resonant = m_settings.character == Params::SubCharacter::resonant;
        m_hp.setParams (fSub * c_hpRatio, resonant ? c_hpQResonant : c_hpQClean);
        m_lp1.setParams (fSub * c_lpRatio, c_lpQ1);
        m_lp2.setParams (fSub * c_lpRatio, c_lpQ2);
        m_even.setParams (fSub, c_evenQ);
    }

    void SubModule::updateControls (const VoiceFrame& frame)
    {
        updateTracker (frame);
        m_controlsValid = true;

        const float driveTarget = static_cast<float> (driveDbFor (static_cast<double> (m_settings.drive)));
        m_driveDb += m_driveCoeff * (driveTarget - m_driveDb);
        if (std::abs (driveTarget - m_driveDb) < 1.0e-3f)
            m_driveDb = driveTarget;
        m_driveGain = dsp::dbToGain (m_driveDb);

        const double beta = betaIndex() == 0 ? c_betaClean : c_betaBiased;
        if (beta != m_beta)
        {
            m_beta = beta;
            m_tanhBeta = std::tanh (beta);
            m_coshBeta2 = std::cosh (beta) * std::cosh (beta);
            m_adaaF1 = antiderivative (m_adaaX1);   // keep the ADAA step consistent
        }

        m_gainTarget = gainTarget();
        m_evenGain = evenGainTarget();
        updateFilters();
    }

    // ------------------------------------------------------------------------------ DSP

    double SubModule::curve (double v) const
    {
        return (std::tanh (v + m_beta) - m_tanhBeta) * m_coshBeta2;
    }

    double SubModule::antiderivative (double v) const
    {
        return (logCosh (v + m_beta) - m_tanhBeta * v) * m_coshBeta2;
    }

    float SubModule::processDivider (Divider& d, float b, bool estimate)
    {
        const float a = std::abs (b);
        d.env = dsp::flushTiny (d.env + (a > d.env ? m_envAttack : m_envRelease) * (a - d.env));
        if (b < -c_armFraction * d.env)
            d.armed = true;
        if (d.sinceToggle < (1 << 30))
            ++d.sinceToggle;
        if (d.armed && d.prev < 0.0f && b >= 0.0f)
        {
            d.q = -d.q;
            d.armed = false;
            if (estimate)
            {
                const double lg = std::log2 (m_fs / static_cast<double> (d.sinceToggle));
                if (std::abs (lg - m_inLog) < c_estimateRange)
                    m_estimateLog += c_estimateCoeff * (lg - m_estimateLog);
            }
            d.sinceToggle = 0;
        }
        d.prev = b;
        return d.q * b;
    }

    void SubModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice)
    {
        if (l == nullptr || n <= 0)
            return;
        stereo = stereo && r != nullptr;

        // Non-finite input never reaches the output (the layer is added to the dry).
        for (int i = 0; i < n; ++i)
            l[i] = dsp::finiteOrZero (l[i]);
        if (stereo)
            for (int i = 0; i < n; ++i)
                r[i] = dsp::finiteOrZero (r[i]);

        m_fade.captureDry (l, r, n, stereo);

        const auto isFrameStart = [&voice] (int i) { return ((voice.startOffset() + i) & (c_voiceFrameSize - 1)) == 0; };

        if (! m_fade.isActive())
        {
            // Keep following the pitch (cheap) so the layer comes back on the right note.
            for (int i = 0; i < n;)
            {
                if (isFrameStart (i) || ! m_controlsValid)
                    updateControls (voice.atSample (i));
                i = voice.frameEnd (i, n);
            }
            m_fade.outputDry (l, r, n, stereo);
            return;
        }

        if (m_fade.consumeRestart())
            resetWet();

        const bool thick = m_settings.character == Params::SubCharacter::thick;
        const double inverseSqrt2 = 1.0 / c_sqrt2;

        for (int i = 0; i < n;)
        {
            const int end = voice.frameEnd (i, n);
            if (isFrameStart (i) || ! m_controlsValid)
                updateControls (voice.atSample (i));

            const double drive = static_cast<double> (m_driveGain) * c_sqrt2;
            for (int k = i; k < end; ++k)
            {
                const float x = stereo ? sanitizeInput (0.5f * (l[k] + r[k])) : sanitizeInput (l[k]);

                // Band 1 (and band 2) and the dividers.
                const float b1 = m_band1b.bandpassNormalised (m_band1a.bandpassNormalised (x));
                const float u1 = processDivider (m_div1, b1, true);
                float s = u1;
                if (thick)
                {
                    const float b2 = m_band2b.bandpassNormalised (m_band2a.bandpassNormalised (x));
                    s += c_band2Gain * processDivider (m_div2, b2, false);
                }

                // Floor expander on band 1's envelope.
                const float expTarget = std::min (1.0f, m_div1.env / c_expanderThreshold);
                m_expGain = dsp::flushTiny (m_expGain + (expTarget > m_expGain ? m_expAttack : m_expRelease) * (expTarget - m_expGain));

                // Drive, normalised by the level of s.
                m_norm = dsp::flushTiny (std::max (static_cast<double> (std::abs (s)), m_norm * m_normRelease));
                const double norm = std::max (m_norm, c_normFloor);
                const double v = drive * static_cast<double> (s) / norm;
                double y;
                const double dv = v - m_adaaX1;
                const double fv = antiderivative (v);
                if (std::abs (dv) < 1.0e-6)
                    y = curve (0.5 * (v + m_adaaX1));
                else
                    y = (fv - m_adaaF1) / dv;
                m_adaaX1 = v;
                m_adaaF1 = fv;
                const auto quadrature = m_even.process (u1);
                const double lpq = static_cast<double> (quadrature.lowpass), bpq = static_cast<double> (quadrature.bandpass);
                const double even = (c_evenCos * (lpq * lpq - bpq * bpq) + c_evenSin * 2.0 * lpq * bpq) / norm;
                const float driven = static_cast<float> (y * norm * inverseSqrt2 + static_cast<double> (m_evenGain) * even);

                // Tight output band-pass.
                const float shaped = m_lp2.lowpass (m_lp1.lowpass (m_hp.highpass (driven)));

                m_gain += m_gainCoeff * (m_gainTarget - m_gain);
                if (std::abs (m_gainTarget - m_gain) < 1.0e-7f)
                    m_gain = m_gainTarget;

                const float sub = dsp::flushTiny (shaped * m_gain * m_expGain);
                if (sub != 0.0f)   // Level 0 is a bit-exact null (no -0 + 0 sign flips)
                {
                    l[k] += sub;
                    if (stereo)
                        r[k] += sub;
                }

                m_levelEnv = dsp::flushTiny (m_levelEnv + m_levelCoeff * (std::abs (sub) - m_levelEnv));
            }
            i = end;
        }

        if (! std::isfinite (m_levelEnv) || ! std::isfinite (m_norm) || ! std::isfinite (m_adaaF1) || ! std::isfinite (m_div1.env)
            || ! std::isfinite (m_div2.env) || ! std::isfinite (m_estimateLog))
        {
            m_estimateLog = m_inLog;
            resetWet();
        }

        m_fade.apply (l, r, n, stereo);
    }

    // ------------------------------------------------------------------------------ chain support

    void SubModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<SubModule> (other);
        m_settings = o.m_settings;
        m_fade.copyStateFrom (o.m_fade);
        m_band1a.copyStateFrom (o.m_band1a);
        m_band1b.copyStateFrom (o.m_band1b);
        m_band2a.copyStateFrom (o.m_band2a);
        m_band2b.copyStateFrom (o.m_band2b);
        m_hp.copyStateFrom (o.m_hp);
        m_lp1.copyStateFrom (o.m_lp1);
        m_lp2.copyStateFrom (o.m_lp2);
        m_even.copyStateFrom (o.m_even);
        m_evenGain = o.m_evenGain;
        m_div1 = o.m_div1;
        m_div2 = o.m_div2;
        m_norm = o.m_norm;
        m_adaaX1 = o.m_adaaX1;
        m_adaaF1 = o.m_adaaF1;
        m_beta = o.m_beta;
        m_tanhBeta = o.m_tanhBeta;
        m_coshBeta2 = o.m_coshBeta2;
        m_gain = o.m_gain;
        m_gainTarget = o.m_gainTarget;
        m_driveDb = o.m_driveDb;
        m_driveGain = o.m_driveGain;
        m_expGain = o.m_expGain;
        m_f0Log = o.m_f0Log;
        m_hasPitch = o.m_hasPitch;
        m_candidateLog = o.m_candidateLog;
        m_candidateSamples = o.m_candidateSamples;
        m_tracked = o.m_tracked;
        m_multiplier = o.m_multiplier;
        m_inLog = o.m_inLog;
        m_subLog = o.m_subLog;
        m_estimateLog = o.m_estimateLog;
        m_controlsValid = o.m_controlsValid;
        m_levelEnv = o.m_levelEnv;
        updateFilters();   // coefficients from the copied centres (same rate, so identical)
    }

    void SubModule::copyLeftStateToRight()
    {
        // The layer is computed once from the mono (mid) signal; only the On fade's dry delay is
        // per channel.
        m_fade.copyLeftStateToRight();
    }

    float SubModule::getLevelNormalised() const
    {
        const float db = dsp::gainToDb (m_levelEnv);
        return std::clamp ((db + 60.0f) / 48.0f, 0.0f, 1.0f) * m_fade.getValue();
    }

    float SubModule::getSubHz() const
    {
        return static_cast<float> (std::exp2 (m_subLog));
    }

    float SubModule::getInputHz() const
    {
        return static_cast<float> (std::exp2 (m_inLog));
    }

    void SubModule::publishFeatures (MonsterFeatures& features) const
    {
        features.subLevel.store (getLevelNormalised(), std::memory_order_relaxed);
        features.subHz.store (getSubHz() * (m_fade.getValue() > 0.0f ? 1.0f : 0.0f), std::memory_order_relaxed);
    }
}

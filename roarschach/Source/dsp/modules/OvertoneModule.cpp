#include "OvertoneModule.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace roar
{
    namespace
    {
        constexpr double c_off = OvertoneModule::c_offDb;

        // Centres at Frequency = 0 oct. Tracked: hz is the multiple of f0.
        const std::array<OvertonePatternSpec, 6> c_patterns {{
            // Chest: weight from the octave and the low fundamental.
            { { 70.0, 1.4, 0.0, { -16.0, -6.0, -18.0, -10.0, -22.0 }, 1.6, 6.0 },
              { 140.0, 1.4, -3.0, { -8.0, -12.0, -16.0, -22.0, c_off }, 1.6, 5.0 }, true, 90.0, false },
            // Throat: gruff upper growl, harmonics at 360 Hz .. 1.4 kHz.
            { { 120.0, 2.0, 0.0, { -14.0, -6.0, -12.0, -10.0, -16.0 }, 1.6, 7.0 },
              { 240.0, 2.0, -5.0, { -10.0, -9.0, -15.0, -20.0, c_off }, 1.7, 6.0 }, true, 150.0, false },
            // Beast: odd-heavy, hollow.
            { { 45.0, 1.2, 0.0, { -20.0, -4.0, -22.0, -7.0, -12.0 }, 1.8, 7.0 },
              { 90.0, 1.2, -2.0, { -16.0, -6.0, -20.0, -10.0, c_off }, 1.8, 6.0 }, true, 70.0, false },
            // Speaker: high orders only.
            { { 100.0, 1.0, 0.0, { -22.0, -10.0, -6.0, -6.0, -9.0 }, 2.5, 8.0 },
              { 200.0, 1.0, -3.0, { -16.0, -8.0, -10.0, -14.0, c_off }, 2.4, 7.0 }, true, 220.0, false },
            // Broad: one wide band, natural decay.
            { { 80.0, 0.7, 0.0, { -8.0, -10.0, -14.0, -18.0, -22.0 }, 1.5, 6.0 },
              { 160.0, 0.7, c_off, { c_off, c_off, c_off, c_off, c_off }, 1.5, 6.0 }, false, 120.0, false },
            // Tracked: A at f0 / 2, B at f0.
            { { 0.5, 3.0, 0.0, { -20.0, -5.0, -18.0, -9.0, -22.0 }, 1.3, 6.0 },
              { 1.0, 3.0, -3.0, { -8.0, -11.0, -15.0, -20.0, c_off }, 1.3, 5.0 }, true, 110.0, true },
        }};

        constexpr double c_amountFadeBelow = 0.02;
        constexpr double c_leanMakeupDb = 1.5, c_replaceMakeupDb = 3.0;
        constexpr float c_leanLowGain = 0.5f;

        constexpr double c_envAttackSeconds = 0.003, c_envReleaseSeconds = 0.040;
        constexpr double c_factorMinDb = -12.0, c_factorMaxDb = 6.0;
        // Voicing weight: floor + (1 - floor) v, v = smoothstep of the confidence over 0.2 .. 0.8.
        // Noise generates about 2 dB more than a vowel at the same band level (its envelope
        // fluctuates and its phase spreads the products), so the floor sits at -12 dB to keep
        // breaths and consonants >= 9 dB below the voiced case.
        constexpr double c_voicingFloor = 0.25;
        constexpr double c_voicingLow = 0.2, c_voicingHigh = 0.8;
        constexpr double c_confidenceSeconds = 0.020;
        constexpr double c_expanderThreshold = 1.0e-3;   // -60 dBFS on A_env, 1:2
        constexpr double c_expanderAttackSeconds = 0.001, c_expanderReleaseSeconds = 0.050;
        constexpr double c_epsilon = 1.0e-5;              // -100 dB

        // Extra 2nd-order low-pass on the band (at 3 fb, its gain and phase at fb compensated):
        // keeps midrange content out of the generator, so it is never modulated (T4).
        constexpr double c_isoRatio = 3.0;

        constexpr double c_hpQ1 = 0.54119610014619698, c_hpQ2 = 1.30656296487637652;
        constexpr double c_lpQ = 0.70710678118654752;
        constexpr double c_crossoverQ = 0.70710678118654752;
        constexpr double c_minFxHz = 30.0, c_maxFxHz = 1000.0;

        constexpr double c_minConfidence = 0.4;
        constexpr double c_minF0Hz = 30.0, c_maxF0Hz = 1500.0;
        constexpr double c_glideSeconds = 0.030;
        constexpr double c_glideSnap = 1.0e-5;            // octaves
        constexpr double c_coeffStep = 0.002;             // octaves a centre moves before its coefficients are redone
        constexpr double c_octSeconds = 0.010;
        constexpr double c_octaveConfirmSeconds = 0.040;
        constexpr double c_octaveTolerance = 0.125;

        constexpr double c_gainSeconds = 0.020;
        constexpr double c_swapSeconds = 0.010;
        constexpr double c_modeFadeSeconds = 0.020;
        constexpr double c_readoutSeconds = 0.080;

        inline float sanitizeInput (float x)
        {
            return std::abs (x) < 1.0e4f ? x : 0.0f;   // also false for NaN
        }

        inline std::array<float, 3> modeWeights (OvertoneMode m)
        {
            switch (m)
            {
                case OvertoneMode::lean: return { 0.0f, c_leanLowGain, 1.0f };
                case OvertoneMode::replace: return { 0.0f, 0.0f, 1.0f };
                case OvertoneMode::add:
                default: return { 1.0f, 0.0f, 0.0f };
            }
        }

        inline double warp (double hz, double fs)
        {
            return std::tan (dsp::c_pi * std::clamp (hz, 1.0, 0.45 * fs) / fs);
        }
    }

    // ------------------------------------------------------------------------------ static design

    const OvertonePatternSpec& OvertoneModule::patternSpec (OvertonePattern p)
    {
        const int i = std::clamp (static_cast<int> (p), 0, static_cast<int> (c_patterns.size()) - 1);
        return c_patterns[static_cast<size_t> (i)];
    }

    double OvertoneModule::amountLawGain (double u)
    {
        if (! (u > 0.0))
            return 0.0;
        u = std::min (u, 1.0);
        const auto lawDb = [] (double x) { return -20.0 + 26.0 * std::pow (x, 0.6); };
        if (u < c_amountFadeBelow)
            return dsp::dbToGain (lawDb (c_amountFadeBelow)) * (u / c_amountFadeBelow);
        return dsp::dbToGain (lawDb (u));
    }

    double OvertoneModule::amountLawDb (double u)
    {
        const double g = amountLawGain (u);
        return g > 0.0 ? 20.0 * std::log10 (g) : -200.0;
    }

    double OvertoneModule::modeMakeupDb (OvertoneMode m)
    {
        if (m == OvertoneMode::lean)
            return c_leanMakeupDb;
        if (m == OvertoneMode::replace)
            return c_replaceMakeupDb;
        return 0.0;
    }

    // ------------------------------------------------------------------------------ lifecycle

    void OvertoneModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);

        for (auto& b : m_s.bands)
        {
            for (auto* f : { &b.bp1, &b.bp2, &b.iso, &b.hp1, &b.hp2, &b.lp })
                f->prepare (m_fs);
            b.hilbert.prepare (m_fs);
        }
        for (auto* xo : { &m_s.xoL, &m_s.xoR })
            for (auto* f : { &xo->split, &xo->low, &xo->high })
                f->prepare (m_fs);
        m_s.bell.setCoeffs (Biquad::peaking (m_fs, c_bellHz, c_bellQ, c_bellDb));

        const double frame = static_cast<double> (c_voiceFrameSize);
        m_envAttackD = dsp::onePoleCoeff (c_envAttackSeconds, m_fs);
        m_envReleaseD = dsp::onePoleCoeff (c_envReleaseSeconds, m_fs);
        m_expAttack = 1.0 - std::exp (-frame / (c_expanderAttackSeconds * m_fs));
        m_expRelease = 1.0 - std::exp (-frame / (c_expanderReleaseSeconds * m_fs));
        m_glideCoeff = 1.0 - std::exp (-frame / (c_glideSeconds * m_fs));
        m_octCoeff = 1.0 - std::exp (-frame / (c_octSeconds * m_fs));
        m_confCoeff = 1.0 - std::exp (-frame / (c_confidenceSeconds * m_fs));
        m_gainCoeff = static_cast<float> (dsp::onePoleCoeff (c_gainSeconds, m_fs));
        m_readoutCoeff = static_cast<float> (dsp::onePoleCoeff (c_readoutSeconds, m_fs));
        m_swapStep = 1.0f / static_cast<float> (dsp::samplesFor (c_swapSeconds, m_fs, 1));
        m_modeFadeLength = dsp::samplesFor (c_modeFadeSeconds, m_fs, 1);
        m_octaveConfirmSamples = dsp::samplesFor (c_octaveConfirmSeconds, m_fs, 1);

        m_fade.prepare (m_fs, maxBlock, getLatencySamples());
        m_fade.setOn (m_settings.on);
        reset();
    }

    void OvertoneModule::reset()
    {
        m_fade.reset();

        m_s.activePattern = m_settings.pattern;
        m_s.pendingSwap = false;
        m_s.swapGain = 1.0f;
        m_s.modeTarget = m_settings.mode;
        m_s.modeFrom = m_s.modeTo = modeWeights (m_settings.mode);
        m_s.modePos = m_modeFadeLength;
        m_s.oct = static_cast<double> (m_settings.freqOct);
        m_s.confidence = 0.0;
        m_s.hasPitch = false;
        m_s.f0Log = std::log2 (c_initialF0Hz);
        m_s.candidateLog = 0.0;
        m_s.candidateSamples = 0;
        m_s.controlsValid = false;
        m_s.interpLeft = 0;
        m_s.inEnv = m_s.genEnv = 0.0f;

        for (auto* xo : { &m_s.xoL, &m_s.xoR })
            for (auto* f : { &xo->split, &xo->low, &xo->high })
                f->reset();

        applyPatternStatics();
        for (int b = 0; b < c_numBands; ++b)
            m_s.bands[static_cast<size_t> (b)].logHz = targetLogHz (b);
        m_s.logFx = targetLogFx();
        computeWeights (true);
        setFilters (true);

        m_s.gainTarget = gainTargetFor();
        m_s.gain = m_s.gainTarget;
        m_s.genRunning = m_s.gainTarget > 0.0f;
        resetGenerator();
    }

    void OvertoneModule::resetGenerator()
    {
        for (auto& b : m_s.bands)
        {
            for (auto* f : { &b.bp1, &b.bp2, &b.iso, &b.hp1, &b.hp2, &b.lp })
                f->reset();
            b.hilbert.reset();
            b.env = 0.0;
            b.factor = 0.0f;
            b.factorStep = 0.0f;
            b.expGain = 0.0f;
            b.outEnv = 0.0f;
        }
        m_s.bell.reset();
        m_s.interpLeft = 0;
        m_s.genEnv = 0.0f;
    }

    void OvertoneModule::restartFromOff()
    {
        // Coming back from fully off: every ramp snaps to the current settings and the wet starts
        // from clean state (the On fade hides the start). Nothing may depend on how many samples
        // the wet ran after the fade-out reached 0, which differs with the block size.
        m_s.activePattern = m_settings.pattern;
        m_s.pendingSwap = false;
        m_s.swapGain = 1.0f;
        applyPatternStatics();
        for (int b = 0; b < c_numBands; ++b)
            m_s.bands[static_cast<size_t> (b)].logHz = targetLogHz (b);
        m_s.logFx = targetLogFx();
        computeWeights (true);
        setFilters (true);

        m_s.modeTarget = m_settings.mode;
        m_s.modeFrom = m_s.modeTo = modeWeights (m_settings.mode);
        m_s.modePos = m_modeFadeLength;

        m_s.gainTarget = gainTargetFor();
        m_s.gain = m_s.gainTarget;
        m_s.genRunning = m_s.gainTarget > 0.0f;

        resetGenerator();
        for (auto* xo : { &m_s.xoL, &m_s.xoR })
            for (auto* f : { &xo->split, &xo->low, &xo->high })
                f->reset();
        m_s.inEnv = 0.0f;
    }

    void OvertoneModule::setParams (const OvertoneSettings& s)
    {
        m_settings = s;
        m_settings.amount = std::isfinite (s.amount) ? std::clamp (s.amount, 0.0f, 1.0f) : 0.0f;
        m_settings.freqOct = std::isfinite (s.freqOct) ? std::clamp (s.freqOct, -1.0f, 1.0f) : 0.0f;
        if (static_cast<int> (m_settings.pattern) < 0 || static_cast<int> (m_settings.pattern) > 5)
            m_settings.pattern = OvertonePattern::chest;
        if (static_cast<int> (m_settings.mode) < 0 || static_cast<int> (m_settings.mode) > 2)
            m_settings.mode = OvertoneMode::add;
        m_fade.setOn (s.on);
    }

    // ------------------------------------------------------------------------------ controls

    float OvertoneModule::gainTargetFor() const
    {
        const double law = amountLawGain (static_cast<double> (m_settings.amount));
        if (! (law > 0.0))
            return 0.0f;
        return static_cast<float> (law * dsp::dbToGain (modeMakeupDb (m_settings.mode)));
    }

    double OvertoneModule::targetLogHz (int band) const
    {
        const auto& p = patternSpec (m_s.activePattern);
        const auto& bs = band == 0 ? p.a : p.b;
        double lg;
        if (p.tracked)
            lg = m_s.f0Log + std::log2 (bs.hz);
        else
            lg = std::log2 (bs.hz) + m_s.oct;
        return std::clamp (lg, std::log2 (c_minCentreHz), std::log2 (c_maxCentreHz));
    }

    double OvertoneModule::targetLogFx() const
    {
        const auto& p = patternSpec (m_s.activePattern);
        return std::clamp (std::log2 (p.crossoverHz) + m_s.oct, std::log2 (c_minFxHz), std::log2 (c_maxFxHz));
    }

    void OvertoneModule::applyPatternStatics()
    {
        const auto& p = patternSpec (m_s.activePattern);
        m_s.bActive = p.hasB;
        m_s.bands[0].level = static_cast<float> (dsp::dbToGain (p.a.levelDb));
        m_s.bands[1].level = p.hasB ? static_cast<float> (dsp::dbToGain (p.b.levelDb)) : 0.0f;
    }

    void OvertoneModule::computeWeights (bool force)
    {
        const auto& p = patternSpec (m_s.activePattern);
        if (! force && m_s.weightsPattern == m_s.activePattern && (! p.tracked || m_s.weightsOct == m_s.oct))
            return;
        m_s.weightsPattern = m_s.activePattern;
        m_s.weightsOct = m_s.oct;
        float maxW = 0.0f;
        std::array<float, c_numHarmonics> shape {};
        for (int b = 0; b < c_numBands; ++b)
        {
            auto& band = m_s.bands[static_cast<size_t> (b)];
            const auto& bs = b == 0 ? p.a : p.b;
            const bool active = b == 0 || p.hasB;
            for (int k = 0; k < c_numHarmonics; ++k)
            {
                const double wDb = bs.weightsDb[static_cast<size_t> (k)];
                float w = 0.0f;
                if (active && wDb > c_offDb + 1.0)
                {
                    double db = wDb;
                    if (p.tracked)
                        db += 6.0 * m_s.oct * std::log2 (static_cast<double> (k + 2) / 3.0);
                    w = static_cast<float> (dsp::dbToGain (db));
                }
                band.weights[static_cast<size_t> (k)] = w;
                const float v = w * band.level;
                shape[static_cast<size_t> (k)] = std::max (shape[static_cast<size_t> (k)], v);
                maxW = std::max (maxW, v);
            }
        }
        for (int k = 0; k < c_numHarmonics; ++k)
            m_s.harmShape[static_cast<size_t> (k)] = maxW > 0.0f ? shape[static_cast<size_t> (k)] / maxW : 0.0f;
    }

    void OvertoneModule::setFilters (bool force)
    {
        // Coefficients are recomputed only when a centre or fx moved by c_coeffStep (or settled on
        // its target), or the pattern changed (control-rate CPU at high rates).
        const auto& p = patternSpec (m_s.activePattern);
        const bool patternChanged = force || m_s.appliedPattern != m_s.activePattern;
        m_s.appliedPattern = m_s.activePattern;
        for (int b = 0; b < c_numBands; ++b)
        {
            auto& band = m_s.bands[static_cast<size_t> (b)];
            if (! patternChanged && std::abs (band.appliedLogHz - band.logHz) < c_coeffStep && (band.appliedLogHz == band.logHz || band.logHz != targetLogHz (b)))
                continue;
            band.appliedLogHz = band.logHz;
            const auto& bs = b == 0 ? p.a : p.b;
            const double hz = std::exp2 (band.logHz);
            const double gBand = warp (hz, m_fs), gIso = warp (hz * c_isoRatio, m_fs), gHp = warp (hz * bs.hpRatio, m_fs);
            band.bp1.setGK (gBand, 1.0 / bs.q);
            band.bp2.setGK (gBand, 1.0 / bs.q);
            band.iso.setGK (gIso, 1.0 / c_lpQ);
            band.hp1.setGK (gHp, 1.0 / c_hpQ1);
            band.hp2.setGK (gHp, 1.0 / c_hpQ2);
            band.lp.setGK (warp (hz * bs.lpRatio, m_fs), 1.0 / c_lpQ);

            // Rotate z so that Re(z) is in phase with the input at the centre: the Hilbert pair's
            // I phase plus the isolation low-pass (bilinear: the analog response on the warped axis).
            const double w = gBand / gIso;
            const std::complex<double> iso = 1.0 / std::complex<double> (1.0 - w * w, w / c_lpQ);
            band.isoGain = static_cast<float> (1.0 / std::abs (iso));
            const auto ri = band.hilbert.responseI (hz) * iso;
            const double mag = std::abs (ri);
            band.rhoRe = mag > 0.0 ? ri.real() / mag : 1.0;
            band.rhoIm = mag > 0.0 ? -ri.imag() / mag : 0.0;
        }

        if (force || std::abs (m_s.appliedLogFx - m_s.logFx) >= c_coeffStep || (m_s.appliedLogFx != m_s.logFx && m_s.logFx == targetLogFx()))
        {
            m_s.appliedLogFx = m_s.logFx;
            const double g = std::tan (dsp::c_pi * std::clamp (std::exp2 (m_s.logFx), 1.0, 0.45 * m_fs) / m_fs);
            for (auto* xo : { &m_s.xoL, &m_s.xoR })
                for (auto* f : { &xo->split, &xo->low, &xo->high })
                    f->setGK (g, 1.0 / c_crossoverQ);
        }
    }

    void OvertoneModule::updateTracker (const VoiceFrame& frame)
    {
        const double f0 = static_cast<double> (frame.f0Hz);
        const bool usable = std::isfinite (f0) && f0 >= c_minF0Hz && f0 <= c_maxF0Hz
                            && static_cast<double> (frame.confidence) >= c_minConfidence;
        if (! usable)
        {
            m_s.candidateSamples = 0;
            return;
        }

        const double lg = std::log2 (f0);
        if (! m_s.hasPitch)
        {
            m_s.f0Log = lg;
            m_s.hasPitch = true;
            m_s.candidateSamples = 0;
            return;
        }

        const double distance = std::abs (lg - m_s.f0Log);
        if (std::abs (distance - 1.0) >= c_octaveTolerance)
        {
            m_s.f0Log = lg;
            m_s.candidateSamples = 0;
            return;
        }

        // An octave jump must persist before it is followed (the same rule as SUB).
        if (m_s.candidateSamples > 0 && std::abs (lg - m_s.candidateLog) < c_octaveTolerance)
            m_s.candidateSamples += c_voiceFrameSize;
        else
        {
            m_s.candidateLog = lg;
            m_s.candidateSamples = c_voiceFrameSize;
        }
        if (m_s.candidateSamples >= m_octaveConfirmSamples)
        {
            m_s.f0Log = m_s.candidateLog;
            m_s.candidateSamples = 0;
        }
    }

    void OvertoneModule::updateControls (const VoiceFrame& frame)
    {
        updateTracker (frame);

        const double conf = std::isfinite (frame.confidence) ? static_cast<double> (frame.confidence) : 0.0;
        const double voicedness = dsp::smoothstep01 ((conf - c_voicingLow) / (c_voicingHigh - c_voicingLow));
        m_s.confidence += m_confCoeff * (voicedness - m_s.confidence);

        const double octTarget = static_cast<double> (m_settings.freqOct);
        if (! m_s.controlsValid)
            m_s.oct = octTarget;
        else
        {
            m_s.oct += m_octCoeff * (octTarget - m_s.oct);
            if (std::abs (octTarget - m_s.oct) < 1.0e-5)
                m_s.oct = octTarget;
        }

        // Generator gain, and whether the generator runs at all.
        m_s.gainTarget = gainTargetFor();
        const bool wanted = ! (m_s.gain == 0.0f && m_s.gainTarget == 0.0f);
        if (wanted && ! m_s.genRunning)
            resetGenerator();   // fresh state on the rising edge (no stale blip)
        m_s.genRunning = wanted;

        // Pattern swap: instant while nothing is generated, otherwise at the bottom of the ramp.
        if (m_settings.pattern != m_s.activePattern && (! m_s.genRunning || ! m_fade.isActive()))
        {
            m_s.pendingSwap = true;
            m_s.swapGain = 0.0f;
        }
        if (m_s.pendingSwap)
        {
            const bool hadB = m_s.bActive;
            m_s.activePattern = m_settings.pattern;
            m_s.pendingSwap = false;
            applyPatternStatics();
            if (m_s.bActive && ! hadB)
            {
                auto& b = m_s.bands[1];
                for (auto* f : { &b.bp1, &b.bp2, &b.iso, &b.hp1, &b.hp2, &b.lp })
                    f->reset();
                b.hilbert.reset();
                b.env = 0.0;
                b.factor = b.factorStep = 0.0f;
                b.expGain = 0.0f;
                b.logHz = targetLogHz (1);
            }
        }

        // Mode: crossfade the dry variants.
        if (m_settings.mode != m_s.modeTarget)
        {
            if (m_s.modeTarget == OvertoneMode::add && m_s.modePos >= m_modeFadeLength)
                for (auto* xo : { &m_s.xoL, &m_s.xoR })
                    for (auto* f : { &xo->split, &xo->low, &xo->high })
                        f->reset();   // the split was idle in Add
            const float t = dsp::smoothstep01 (static_cast<float> (m_s.modePos) / static_cast<float> (m_modeFadeLength));
            for (size_t j = 0; j < 3; ++j)
                m_s.modeFrom[j] += t * (m_s.modeTo[j] - m_s.modeFrom[j]);
            m_s.modeTo = modeWeights (m_settings.mode);
            m_s.modeTarget = m_settings.mode;
            m_s.modePos = 0;
        }

        // Centres and the crossover glide in log2.
        for (int b = 0; b < c_numBands; ++b)
        {
            auto& band = m_s.bands[static_cast<size_t> (b)];
            const double target = targetLogHz (b);
            if (! m_s.controlsValid || std::abs (target - band.logHz) < c_glideSnap)
                band.logHz = target;
            else
                band.logHz += m_glideCoeff * (target - band.logHz);
        }
        {
            const double target = targetLogFx();
            if (! m_s.controlsValid || std::abs (target - m_s.logFx) < c_glideSnap)
                m_s.logFx = target;
            else
                m_s.logFx += m_glideCoeff * (target - m_s.logFx);
        }
        setFilters (false);
        computeWeights (false);

        // Dynamics, interpolated over the next frame.
        const double voicing = c_voicingFloor + (1.0 - c_voicingFloor) * m_s.confidence;
        const double fMin = dsp::dbToGain (c_factorMinDb), fMax = dsp::dbToGain (c_factorMaxDb);
        for (auto& band : m_s.bands)
        {
            const double expTarget = std::min (1.0, band.env / c_expanderThreshold);
            const double ec = expTarget > static_cast<double> (band.expGain) ? m_expAttack : m_expRelease;
            band.expGain = static_cast<float> (dsp::flushTiny (static_cast<double> (band.expGain) + ec * (expTarget - static_cast<double> (band.expGain))));
            const double slope = std::clamp (std::pow (std::max (band.env, 1.0e-12) / c_referenceAmplitude, c_gamma - 1.0), fMin, fMax);
            const float target = static_cast<float> (slope * voicing) * band.expGain;
            if (! m_s.controlsValid)
            {
                band.factor = target;
                band.factorStep = 0.0f;
            }
            else
                band.factorStep = (target - band.factor) / static_cast<float> (c_voiceFrameSize);
        }
        m_s.interpLeft = c_voiceFrameSize;

        m_s.controlsValid = true;
    }

    // ------------------------------------------------------------------------------ DSP

    void OvertoneModule::runBand (Band& band, const float* x, float* sum, int count, int interpCount)
    {
        // Stage by stage over the frame (at most 32 samples), with local copies of the state, so
        // each recursion pipelines on its own instead of the whole chain running per sample.
        std::array<float, c_voiceFrameSize> xb {}, iv {}, qv {}, g {};
        const auto n = static_cast<size_t> (count);

        {
            TptSvf bp1 = band.bp1, bp2 = band.bp2, iso = band.iso;
            const float isoGain = band.isoGain;
            for (size_t k = 0; k < n; ++k)
                xb[k] = iso.lowpass (bp2.bandpassNormalised (bp1.bandpassNormalised (x[k]))) * isoGain;
            band.bp1 = bp1; band.bp2 = bp2; band.iso = iso;
        }
        {
            HilbertPair hilbert = band.hilbert;
            for (size_t k = 0; k < n; ++k)
                hilbert.process (xb[k], iv[k], qv[k]);
            band.hilbert = hilbert;
        }
        {
            const double rhoRe = band.rhoRe, rhoIm = band.rhoIm;
            const double att = m_envAttackD, rel = m_envReleaseD;
            const auto w = band.weights;
            double env = band.env;
            float factor = band.factor;
            const float step = band.factorStep;
            for (size_t k = 0; k < n; ++k)
            {
                if (static_cast<int> (k) < interpCount)
                    factor += step;
                const double zr = rhoRe * static_cast<double> (iv[k]) - rhoIm * static_cast<double> (qv[k]);
                const double zi = rhoRe * static_cast<double> (qv[k]) + rhoIm * static_cast<double> (iv[k]);
                const double a = std::sqrt (zr * zr + zi * zi);
                env = dsp::flushTiny (env + (a > env ? att : rel) * (a - env));

                // Unit phasor u = z / |z| and Re(u^k), k = 2..6.
                const double inv = 1.0 / std::max (a, c_epsilon);
                const double ur = zr * inv, ui = zi * inv;
                double pr = ur * ur - ui * ui;
                double pi = 2.0 * ur * ui;
                double s = static_cast<double> (w[0]) * pr;
                for (size_t h = 1; h < static_cast<size_t> (c_numHarmonics); ++h)
                {
                    const double nr = pr * ur - pi * ui;
                    pi = pr * ui + pi * ur;
                    pr = nr;
                    s += static_cast<double> (w[h]) * pr;
                }
                g[k] = static_cast<float> (a * s) * factor;
            }
            band.env = env;
            band.factor = factor;
        }
        {
            TptSvf hp1 = band.hp1, hp2 = band.hp2, lp = band.lp;
            const float level = band.level, readout = m_readoutCoeff;
            float outEnv = band.outEnv;
            for (size_t k = 0; k < n; ++k)
            {
                const float y = lp.lowpass (hp2.highpass (hp1.highpass (g[k]))) * level;
                outEnv = dsp::flushTiny (outEnv + readout * (std::abs (y) - outEnv));
                sum[k] += y;
            }
            band.hp1 = hp1; band.hp2 = hp2; band.lp = lp;
            band.outEnv = outEnv;
        }
    }

    void OvertoneModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice)
    {
        if (l == nullptr || n <= 0)
            return;
        stereo = stereo && r != nullptr;

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
                if (isFrameStart (i) || ! m_s.controlsValid)
                    updateControls (voice.atSample (i));
                i = voice.frameEnd (i, n);
            }
            m_fade.outputDry (l, r, n, stereo);
            return;
        }

        if (m_fade.consumeRestart())
            restartFromOff();

        const float invModeLength = 1.0f / static_cast<float> (m_modeFadeLength);

        for (int i = 0; i < n;)
        {
            const int end = voice.frameEnd (i, n);
            if (isFrameStart (i) || ! m_s.controlsValid)
                updateControls (voice.atSample (i));

            const bool run = m_s.genRunning;
            const int count = end - i;

            // Mono analysis input (the mid when stereo).
            std::array<float, c_voiceFrameSize> x {}, h {};
            for (int k = 0; k < count; ++k)
            {
                const float xl = l[i + k];
                x[static_cast<size_t> (k)] = stereo ? sanitizeInput (0.5f * (xl + r[i + k])) : sanitizeInput (xl);
            }

            // ---- Generator
            if (run)
            {
                std::array<float, c_voiceFrameSize> sum {};
                const int interpCount = std::min (count, m_s.interpLeft);
                runBand (m_s.bands[0], x.data(), sum.data(), count, interpCount);
                if (m_s.bActive)
                    runBand (m_s.bands[1], x.data(), sum.data(), count, interpCount);
                m_s.interpLeft -= interpCount;

                for (int k = 0; k < count; ++k)
                {
                    // Pattern ramp.
                    if (m_settings.pattern != m_s.activePattern)
                    {
                        if (! m_s.pendingSwap)
                        {
                            m_s.swapGain -= m_swapStep;
                            if (m_s.swapGain <= 0.0f)
                            {
                                m_s.swapGain = 0.0f;
                                m_s.pendingSwap = true;
                            }
                        }
                    }
                    else if (m_s.swapGain < 1.0f && ! m_s.pendingSwap)
                        m_s.swapGain = std::min (1.0f, m_s.swapGain + m_swapStep);

                    m_s.gain += m_gainCoeff * (m_s.gainTarget - m_s.gain);
                    if (std::abs (m_s.gainTarget - m_s.gain) < 1.0e-7f)
                        m_s.gain = m_s.gainTarget;

                    const auto kk = static_cast<size_t> (k);
                    h[kk] = dsp::flushTiny (m_s.bell.process (sum[kk]) * m_s.gain * m_s.swapGain);
                }
            }

            // ---- Dry variants (per channel, shared coefficients). In Add, once any crossfade is
            // done, the dry is untouched and the split is not run; it restarts from clean state
            // when a crossfade away from Add begins (the smoothstep hides its first milliseconds).
            const bool fading = m_s.modePos < m_modeFadeLength;
            if (fading || m_s.modeTarget != OvertoneMode::add)
            {
                for (int ch = 0; ch < (stereo ? 2 : 1); ++ch)
                {
                    float* io = ch == 0 ? l + i : r + i;
                    Crossover xo = ch == 0 ? m_s.xoL : m_s.xoR;
                    int pos = m_s.modePos;
                    for (int k = 0; k < count; ++k)
                    {
                        const float in = io[k];
                        const auto o1 = xo.split.process (in);
                        const float low = xo.low.lowpass (o1.lowpass);
                        const float high = xo.high.highpass (o1.highpass);
                        std::array<float, 3> wm = m_s.modeTo;
                        if (pos < m_modeFadeLength)
                        {
                            ++pos;
                            const float t = dsp::smoothstep01 (static_cast<float> (pos) * invModeLength);
                            for (size_t j = 0; j < 3; ++j)
                                wm[j] = m_s.modeFrom[j] + t * (m_s.modeTo[j] - m_s.modeFrom[j]);
                        }
                        float out = wm[0] * in + wm[1] * low + wm[2] * high;
                        const float hk = h[static_cast<size_t> (k)];
                        if (hk != 0.0f)
                            out += hk;
                        io[k] = out;
                    }
                    (ch == 0 ? m_s.xoL : m_s.xoR) = xo;
                    if (ch == (stereo ? 1 : 0))
                        m_s.modePos = pos;
                }
            }
            else if (run)
            {
                for (int k = 0; k < count; ++k)
                {
                    const float hk = h[static_cast<size_t> (k)];
                    if (hk != 0.0f)   // Amount 0 in Add is a bit-exact null (no -0 + 0 sign flips)
                    {
                        l[i + k] += hk;
                        if (stereo)
                            r[i + k] += hk;
                    }
                }
            }

            // ---- Readouts
            for (int k = 0; k < count; ++k)
            {
                const auto kk = static_cast<size_t> (k);
                m_s.inEnv = dsp::flushTiny (m_s.inEnv + m_readoutCoeff * (std::abs (x[kk]) - m_s.inEnv));
                m_s.genEnv = dsp::flushTiny (m_s.genEnv + m_readoutCoeff * (std::abs (h[kk]) - m_s.genEnv));
            }
            i = end;
        }

        bool bad = ! std::isfinite (m_s.genEnv) || ! std::isfinite (m_s.inEnv);
        for (const auto& b : m_s.bands)
            bad = bad || ! std::isfinite (b.env) || ! std::isfinite (b.factor) || ! std::isfinite (b.outEnv);
        if (bad)
        {
            resetGenerator();
            m_s.inEnv = 0.0f;
        }

        m_fade.apply (l, r, n, stereo);
    }

    // ------------------------------------------------------------------------------ chain support

    void OvertoneModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<OvertoneModule> (other);
        m_settings = o.m_settings;
        m_fade.copyStateFrom (o.m_fade);
        m_s = o.m_s;   // plain values: filters, Hilbert pairs, glides, envelopes, ramps (same rate, so same coefficients)
    }

    void OvertoneModule::copyLeftStateToRight()
    {
        // The generator is mono (the mid); only the dry split and the On fade's delay are per channel.
        m_s.xoR = m_s.xoL;
        m_fade.copyLeftStateToRight();
    }

    // ------------------------------------------------------------------------------ readouts

    float OvertoneModule::getLevelNormalised() const
    {
        if (! (m_s.inEnv > 1.0e-6f))
            return 0.0f;
        const float db = dsp::gainToDb (m_s.genEnv / m_s.inEnv);
        return std::clamp ((db + 36.0f) / 36.0f, 0.0f, 1.0f) * m_fade.getValue();
    }

    float OvertoneModule::getHarmonicDrive (int k) const
    {
        if (k < 0 || k >= c_numHarmonics)
            return 0.0f;
        return getLevelNormalised() * m_s.harmShape[static_cast<size_t> (k)];
    }

    float OvertoneModule::getBandLevelNormalised (int band) const
    {
        if (band < 0 || band >= c_numBands || (band == 1 && ! m_s.bActive))
            return 0.0f;
        const auto& b = m_s.bands[static_cast<size_t> (band)];
        if (! (b.env > 1.0e-6))
            return 0.0f;
        const float db = dsp::gainToDb (b.outEnv * m_s.gain * m_s.swapGain / static_cast<float> (b.env));
        return std::clamp ((db + 24.0f) / 30.0f, 0.0f, 1.0f) * m_fade.getValue();
    }

    float OvertoneModule::getBandHz (int band) const
    {
        if (band < 0 || band >= c_numBands || (band == 1 && ! m_s.bActive))
            return 0.0f;
        return static_cast<float> (std::exp2 (m_s.bands[static_cast<size_t> (band)].logHz));
    }
}

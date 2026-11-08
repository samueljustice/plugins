#include "CleanStage.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_pi = 3.141592653589793;

        constexpr double c_hpfHz = CleanStage::c_highPassHz;
        constexpr double c_shelfHz = 150.0;
        constexpr double c_shelfDb = -4.0;

        // Expander: threshold rides 10 dB above the noise floor estimate, within these limits.
        constexpr float c_floorStartDb = -65.0f;
        constexpr float c_floorMarginDb = 10.0f;
        constexpr float c_thresholdMinDb = -62.0f;
        constexpr float c_thresholdMaxDb = -48.0f;
        constexpr float c_floorRiseDbPerSecond = 2.0f; // slow, so a long phrase barely moves it
        constexpr double c_floorFallSeconds = 0.050;
        constexpr float c_hysteresisDb = 4.0f;
        constexpr float c_expanderRatio = 3.0f;        // 1:3 below the threshold
        constexpr float c_maxGateDepthDb = -40.0f;
        constexpr double c_gateOpenSeconds = 0.002;
        constexpr double c_gateCloseSeconds = 0.120;
        constexpr double c_levelAttackSeconds = 0.0005;
        constexpr double c_levelReleaseSeconds = 0.030;

        // Compressor
        constexpr float c_compThresholdDb = -24.0f;
        constexpr float c_compRatio = 2.5f;
        constexpr float c_compKneeDb = 6.0f;
        constexpr double c_compAttackSeconds = 0.010;
        constexpr double c_compReleaseSeconds = 0.120;
        constexpr float c_compMakeupReferenceDb = -13.0f; // detector reading of a -18 dBFS RMS voiced line (its peaks)

        // De-esser
        constexpr double c_sibLowHz = 5500.0, c_sibHighHz = 9000.0;
        constexpr float c_sibRatioThresholdDb = -14.0f;   // band envelope vs full envelope
        constexpr float c_sibSlope = 1.0f;                // dB of cut per dB over the ratio threshold
        constexpr float c_sibMaxDb = 8.0f;
        constexpr float c_sibFloorDb = -50.0f;            // quieter "s" sounds are left alone
        constexpr double c_sibEnvAttackSeconds = 0.001, c_sibEnvReleaseSeconds = 0.060;
        constexpr double c_deEssAttackSeconds = 0.001, c_deEssReleaseSeconds = 0.050;

        float onePole (double seconds, double sampleRate)
        {
            return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * sampleRate)));
        }

        float gainToDb (float g)
        {
            return 20.0f * std::log10 (std::max (g, 1.0e-6f));
        }

        float powerToDb (float p)
        {
            return 10.0f * std::log10 (std::max (p, 1.0e-12f));
        }

        float dbToGain (float db)
        {
            return std::exp (db * 0.115129255f);
        }

        float compressorGainDb (float levelDb)
        {
            const float over = levelDb - c_compThresholdDb;
            const float slope = 1.0f - 1.0f / c_compRatio;

            if (over <= -0.5f * c_compKneeDb)
                return 0.0f;

            if (over >= 0.5f * c_compKneeDb)
                return -slope * over;

            const float x = over + 0.5f * c_compKneeDb;
            return -slope * x * x / (2.0f * c_compKneeDb);
        }

        template <typename Filter>
        void designShelfOrPass (Filter& f, bool isShelf, double hz, double gainDb, double sampleRate)
        {
            const double w = 2.0 * c_pi * hz / sampleRate;
            const double cw = std::cos (w), sw = std::sin (w);
            const double q = 0.7071067811865476;
            const double alpha = sw / (2.0 * q);
            double b0, b1, b2, a0, a1, a2;

            if (isShelf)
            {
                const double A = std::pow (10.0, gainDb / 40.0);
                const double sq = 2.0 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1.0) - (A - 1.0) * cw + sq);
                b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
                b2 = A * ((A + 1.0) - (A - 1.0) * cw - sq);
                a0 = (A + 1.0) + (A - 1.0) * cw + sq;
                a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
                a2 = (A + 1.0) + (A - 1.0) * cw - sq;
            }
            else
            {
                b0 = (1.0 + cw) * 0.5;
                b1 = -(1.0 + cw);
                b2 = (1.0 + cw) * 0.5;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cw;
                a2 = 1.0 - alpha;
            }

            f.b0 = b0 / a0;
            f.b1 = b1 / a0;
            f.b2 = b2 / a0;
            f.a1 = a1 / a0;
            f.a2 = a2 / a0;
        }
    }

    void CleanStage::prepare (double sampleRate, int /*maxBlockSize*/)
    {
        m_sampleRate = sampleRate > 1000.0 ? sampleRate : 48000.0;
        const double fs = m_sampleRate;

        // Keep the de-ess band below Nyquist at low sample rates.
        const double sibLow = std::min (c_sibLowHz, 0.30 * fs);
        const double sibHigh = std::min (c_sibHighHz, 0.45 * fs);
        const double centre = std::sqrt (sibLow * sibHigh);
        const double q = centre / std::max (1.0, sibHigh - sibLow);

        for (auto& c : m_channels)
        {
            designShelfOrPass (c.hpf, false, c_hpfHz, 0.0, fs);
            designShelfOrPass (c.shelf, true, c_shelfHz, c_shelfDb, fs);

            c.sibilance.g = static_cast<float> (std::tan (c_pi * centre / fs));
            c.sibilance.k = static_cast<float> (1.0 / q);
            c.sibilance.a1 = 1.0f / (1.0f + c.sibilance.g * (c.sibilance.g + c.sibilance.k));
            c.sibilance.a2 = c.sibilance.g * c.sibilance.a1;
            c.sibilance.a3 = c.sibilance.g * c.sibilance.a2;
        }

        m_levelAttack = onePole (c_levelAttackSeconds, fs);
        m_levelRelease = onePole (c_levelReleaseSeconds, fs);
        m_floorFall = onePole (c_floorFallSeconds, fs);
        m_floorRisePerSample = static_cast<float> (c_floorRiseDbPerSecond / fs);
        m_gateOpenCoeff = onePole (c_gateOpenSeconds, fs);
        m_gateCloseCoeff = onePole (c_gateCloseSeconds, fs);
        m_compAttack = onePole (c_compAttackSeconds, fs);
        m_compRelease = onePole (c_compReleaseSeconds, fs);
        m_sibAttack = onePole (c_sibEnvAttackSeconds, fs);
        m_sibRelease = onePole (c_sibEnvReleaseSeconds, fs);
        m_deEssAttack = onePole (c_deEssAttackSeconds, fs);
        m_deEssRelease = onePole (c_deEssReleaseSeconds, fs);

        reset();
    }

    void CleanStage::reset()
    {
        for (auto& c : m_channels)
        {
            c.hpf.clear();
            c.shelf.clear();
            c.sibilance.clear();
        }

        m_levelEnv = 0.0f;
        m_floorDb = c_floorStartDb;
        m_thresholdDb = std::clamp (m_floorDb + c_floorMarginDb, c_thresholdMinDb, c_thresholdMaxDb);
        m_gateOpen = false;
        m_gateGain = 1.0f;  // start open: the first word must not be swallowed while the gate learns
        m_gateGainDb = 0.0f;
        m_compMs = 0.0f;
        m_compGainDb = 0.0f;
        m_bandEnv = m_fullEnv = 0.0f;
        m_deEss = 0.0f;
        m_deEssDb = 0.0f;
    }

    bool CleanStage::stateIsFinite() const
    {
        for (const auto& c : m_channels)
            if (! std::isfinite (c.hpf.z1) || ! std::isfinite (c.hpf.z2) || ! std::isfinite (c.shelf.z1)
                || ! std::isfinite (c.shelf.z2) || ! std::isfinite (c.sibilance.ic1) || ! std::isfinite (c.sibilance.ic2))
                return false;

        return std::isfinite (m_levelEnv) && std::isfinite (m_compMs) && std::isfinite (m_gateGain)
            && std::isfinite (m_bandEnv) && std::isfinite (m_fullEnv) && std::isfinite (m_deEss)
            && std::isfinite (m_floorDb);
    }

    void CleanStage::process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo&)
    {
        const bool twoChannels = stereo && right != nullptr;
        const float makeupDb = -compressorGainDb (c_compMakeupReferenceDb);
        const float expanderSlope = c_expanderRatio - 1.0f;
        float minGate = 1.0f, minComp = 0.0f, maxDeEss = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float l = left[i];
            float r = twoChannels ? right[i] : 0.0f;
            l = std::isfinite (l) ? l : 0.0f;
            r = std::isfinite (r) ? r : 0.0f;

            l = m_channels[0].shelf.process (m_channels[0].hpf.process (l));
            if (twoChannels)
                r = m_channels[1].shelf.process (m_channels[1].hpf.process (r));

            const float peak = std::max (std::abs (l), std::abs (r));
            const float sq = std::max (l * l, r * r);

            // ---- expander (gain from the filtered level; threshold follows the noise floor)
            m_levelEnv += (peak > m_levelEnv ? m_levelAttack : m_levelRelease) * (peak - m_levelEnv);
            const float levelDb = gainToDb (m_levelEnv);

            if (levelDb < m_floorDb)
                m_floorDb += m_floorFall * (levelDb - m_floorDb);
            else
                m_floorDb += m_floorRisePerSample;

            m_floorDb = std::clamp (m_floorDb, -120.0f, c_thresholdMaxDb);
            m_thresholdDb = std::clamp (m_floorDb + c_floorMarginDb, c_thresholdMinDb, c_thresholdMaxDb);

            if (levelDb > m_thresholdDb)
                m_gateOpen = true;
            else if (levelDb < m_thresholdDb - c_hysteresisDb)
                m_gateOpen = false;

            const float effectiveThreshold = m_gateOpen ? m_thresholdDb - c_hysteresisDb : m_thresholdDb;
            const float gateTargetDb = std::max (c_maxGateDepthDb,
                                                 std::min (0.0f, (levelDb - effectiveThreshold) * expanderSlope));
            const float gateTarget = dbToGain (gateTargetDb);
            m_gateGain += (gateTarget > m_gateGain ? m_gateOpenCoeff : m_gateCloseCoeff) * (gateTarget - m_gateGain);

            // ---- compressor (RMS-ish detector on the gated signal)
            const float gatedSq = sq * m_gateGain * m_gateGain;
            m_compMs += (gatedSq > m_compMs ? m_compAttack : m_compRelease) * (gatedSq - m_compMs);
            const float compDb = compressorGainDb (powerToDb (m_compMs));
            const float gain = m_gateGain * dbToGain (compDb + makeupDb);

            l *= gain;
            r *= gain;

            // ---- de-esser: reduce only the 5.5-9 kHz band, only when it dominates
            const float bandL = m_channels[0].sibilance.bandPass (l);
            const float bandR = twoChannels ? m_channels[1].sibilance.bandPass (r) : 0.0f;
            const float bandAbs = std::max (std::abs (bandL), std::abs (bandR));
            const float fullAbs = std::max (std::abs (l), std::abs (r));
            m_bandEnv += (bandAbs > m_bandEnv ? m_sibAttack : m_sibRelease) * (bandAbs - m_bandEnv);
            m_fullEnv += (fullAbs > m_fullEnv ? m_sibAttack : m_sibRelease) * (fullAbs - m_fullEnv);

            float deEssTarget = 0.0f;
            const float bandDb = gainToDb (m_bandEnv);

            if (bandDb > c_sibFloorDb)
            {
                const float ratioDb = bandDb - gainToDb (m_fullEnv);
                deEssTarget = std::clamp ((ratioDb - c_sibRatioThresholdDb) * c_sibSlope, 0.0f, c_sibMaxDb);
            }

            m_deEss += (deEssTarget > m_deEss ? m_deEssAttack : m_deEssRelease) * (deEssTarget - m_deEss);
            const float removeFraction = 1.0f - dbToGain (-m_deEss);

            left[i] = l - removeFraction * bandL;
            if (twoChannels)
                right[i] = r - removeFraction * bandR;

            minGate = std::min (minGate, m_gateGain);
            minComp = std::min (minComp, compDb);
            maxDeEss = std::max (maxDeEss, m_deEss);
        }

        m_gateGainDb = gainToDb (minGate);
        m_compGainDb = minComp;
        m_deEssDb = maxDeEss;

        if (! stateIsFinite())
            reset();
    }
}

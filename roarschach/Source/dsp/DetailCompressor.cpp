#include "DetailCompressor.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr float c_upThresholdDb = -30.0f;
        constexpr float c_upRatio = 2.5f;
        constexpr float c_maxBoostDb = 24.0f;          // at LOW 100 %
        constexpr float c_guardStartDb = -65.0f;       // boost fades out below this ...
        constexpr float c_guardRangeDb = 12.0f;        // ... and is gone 12 dB further down

        constexpr double c_upAttackSeconds = 0.001;
        constexpr double c_fastReleaseSeconds = 0.040;
        constexpr double c_slowAttackSeconds = 0.060;  // only sustained material charges the slow detector
        constexpr double c_slowReleaseSeconds = 0.250;

        constexpr float c_downMaxThresholdDb = -24.0f; // at HIGH 100 %
        constexpr float c_downMinRatio = 4.0f, c_downMaxRatio = 10.0f;
        constexpr float c_downKneeDb = 6.0f;
        constexpr double c_downAttackSeconds = 0.003, c_downReleaseSeconds = 0.120;

        constexpr double c_gainSmoothSeconds = 0.002;
        constexpr double c_rmsWindowSeconds = 0.004;   // pre-average of x^2 so the 1 ms attack reads RMS, not glottal peaks
        constexpr double c_paramSeconds = 0.030;

        float onePole (double seconds, double sampleRate)
        {
            return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * sampleRate)));
        }

        float powerToDb (float p)
        {
            return 10.0f * std::log10 (std::max (p, 1.0e-14f));
        }
    }

    void DetailCompressor::prepare (double sampleRate, int /*maxBlockSize*/)
    {
        m_sampleRate = sampleRate > 1000.0 ? sampleRate : 48000.0;

        m_paramCoeff = onePole (c_paramSeconds, m_sampleRate);
        m_upAttack = onePole (c_upAttackSeconds, m_sampleRate);
        m_fastRelease = onePole (c_fastReleaseSeconds, m_sampleRate);
        m_slowAttack = onePole (c_slowAttackSeconds, m_sampleRate);
        m_slowRelease = onePole (c_slowReleaseSeconds, m_sampleRate);
        m_downAttack = onePole (c_downAttackSeconds, m_sampleRate);
        m_downRelease = onePole (c_downReleaseSeconds, m_sampleRate);
        m_gainCoeff = onePole (c_gainSmoothSeconds, m_sampleRate);
        m_rmsCoeff = onePole (c_rmsWindowSeconds, m_sampleRate);

        reset();
    }

    void DetailCompressor::reset()
    {
        m_rmsMs = 0.0f;
        m_fastMs = m_slowMs = 0.0f;
        m_downMs = 0.0f;
        m_gainSmoothedDb = 0.0f;
        m_boostDb = m_cutDb = m_gainDb = 0.0f;
        m_snap = true;
    }

    void DetailCompressor::setParams (float low01, float high01)
    {
        m_lowTarget = std::isfinite (low01) ? std::clamp (low01, 0.0f, 1.0f) : 0.0f;
        m_highTarget = std::isfinite (high01) ? std::clamp (high01, 0.0f, 1.0f) : 0.0f;

        if (m_snap)
        {
            m_low = m_lowTarget;
            m_high = m_highTarget;
            m_snap = false;
        }
    }

    void DetailCompressor::process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo&)
    {
        const bool twoChannels = stereo && right != nullptr;
        const float upSlope = 1.0f - 1.0f / c_upRatio;
        float boostDb = 0.0f, cutDb = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float l = std::isfinite (left[i]) ? left[i] : 0.0f;
            float r = twoChannels && std::isfinite (right[i]) ? right[i] : 0.0f;

            m_low += m_paramCoeff * (m_lowTarget - m_low);
            m_high += m_paramCoeff * (m_highTarget - m_high);

            const float sq = std::max (l * l, r * r);

            // ---- upward: program-dependent release from two detectors
            m_rmsMs += m_rmsCoeff * (sq - m_rmsMs);
            m_fastMs += (m_rmsMs > m_fastMs ? m_upAttack : m_fastRelease) * (m_rmsMs - m_fastMs);
            m_slowMs += (m_rmsMs > m_slowMs ? m_slowAttack : m_slowRelease) * (m_rmsMs - m_slowMs);
            const float upLevelDb = powerToDb (std::max (m_fastMs, m_slowMs));
            const float guard = std::clamp ((upLevelDb - (c_guardStartDb - c_guardRangeDb)) / c_guardRangeDb, 0.0f, 1.0f);

            boostDb = std::min (c_maxBoostDb * m_low, std::max (0.0f, (c_upThresholdDb - upLevelDb) * upSlope)) * guard;

            // ---- downward with soft knee
            m_downMs += (sq > m_downMs ? m_downAttack : m_downRelease) * (sq - m_downMs);
            const float downLevelDb = powerToDb (m_downMs);
            const float threshold = c_downMaxThresholdDb * m_high;
            const float ratio = c_downMinRatio + (c_downMaxRatio - c_downMinRatio) * m_high;
            const float downSlope = 1.0f - 1.0f / ratio;
            const float over = downLevelDb - threshold;

            if (over >= 0.5f * c_downKneeDb)
                cutDb = downSlope * over;
            else if (over > -0.5f * c_downKneeDb)
            {
                const float z = over + 0.5f * c_downKneeDb;
                cutDb = downSlope * z * z / (2.0f * c_downKneeDb);
            }
            else
                cutDb = 0.0f;

            // At HIGH 0 the threshold is 0 dBFS: fade the cut in with HIGH so 0 is truly off.
            cutDb *= std::min (1.0f, m_high * 20.0f);
            // The make-up is held back by the same guard: the noise floor is never raised.
            const float makeupDb = -0.5f * threshold * guard;

            const float targetDb = boostDb - cutDb + makeupDb;
            m_gainSmoothedDb += m_gainCoeff * (targetDb - m_gainSmoothedDb);
            const float gain = std::exp (m_gainSmoothedDb * 0.115129255f);

            left[i] = l * gain;
            if (twoChannels)
                right[i] = r * gain;
        }

        m_boostDb = boostDb;
        m_cutDb = cutDb;
        m_gainDb = m_gainSmoothedDb;

        if (! std::isfinite (m_rmsMs) || ! std::isfinite (m_fastMs) || ! std::isfinite (m_slowMs) || ! std::isfinite (m_downMs)
            || ! std::isfinite (m_gainSmoothedDb))
            reset();
    }
}

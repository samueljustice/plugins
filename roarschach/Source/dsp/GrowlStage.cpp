#include "GrowlStage.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_twoPi = 6.283185307179586;

        constexpr double c_baseDelaySeconds = 0.00035;  // jitter swings 0.1..0.6 ms around this
        constexpr double c_maxJitterSeconds = 0.00025;
        constexpr double c_jitterRateHz = 70.0;         // new random delay target this often

        constexpr float c_maxDoubling = 0.6f;           // d1 at Amount 100 %
        constexpr float c_maxTremDepth = 0.5f;
        constexpr float c_tremMinHz = 25.0f, c_tremMaxHz = 45.0f;
        constexpr float c_maxSquareness = 6.0f;         // tanh drive of the tremolo shape at 100 %
        constexpr float c_maxShimmer = 0.15f;

        constexpr float c_quietIntensity = 0.35f;       // share of the effect on quiet lines
        constexpr float c_quietDb = -42.0f, c_loudDb = -12.0f;

        constexpr double c_paramSeconds = 0.020;
        constexpr double c_f0GlideSeconds = 0.020;
        constexpr double c_voicedFadeSeconds = 0.015;
        constexpr double c_envAttackSeconds = 0.005, c_envReleaseSeconds = 0.150;
        constexpr double c_modDisplayHz = 30.0;

        constexpr float c_minF0 = 40.0f, c_maxF0 = 1000.0f;

        float onePole (double seconds, double sampleRate)
        {
            return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * sampleRate)));
        }

        float smoothstep01 (float t)
        {
            t = std::clamp (t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        int nextPowerOfTwo (int n)
        {
            int p = 1;
            while (p < n)
                p <<= 1;
            return p;
        }
    }

    void GrowlStage::prepare (double sampleRate, int /*maxBlockSize*/)
    {
        m_sampleRate = sampleRate > 1000.0 ? sampleRate : 48000.0;
        m_invSampleRate = static_cast<float> (1.0 / m_sampleRate);

        m_baseDelay = std::max (4, static_cast<int> (std::lround (c_baseDelaySeconds * m_sampleRate)));
        // Keep 2 samples clear of the write position for the 4-point interpolator.
        m_maxJitterSamples = std::min (static_cast<float> (c_maxJitterSeconds * m_sampleRate),
                                       static_cast<float> (m_baseDelay) - 2.0f);

        const int length = nextPowerOfTwo (m_baseDelay + static_cast<int> (std::ceil (m_maxJitterSamples)) + 8);
        m_lineL.assign (static_cast<size_t> (length), 0.0f);
        m_lineR.assign (static_cast<size_t> (length), 0.0f);
        m_mask = length - 1;

        m_jitterPeriod = std::max (1, static_cast<int> (std::lround (m_sampleRate / c_jitterRateHz)));
        m_paramCoeff = onePole (c_paramSeconds, m_sampleRate);
        m_f0Coeff = onePole (c_f0GlideSeconds, m_sampleRate);
        m_voicedCoeff = onePole (c_voicedFadeSeconds, m_sampleRate);
        m_envAttack = onePole (c_envAttackSeconds, m_sampleRate);
        m_envRelease = onePole (c_envReleaseSeconds, m_sampleRate);
        m_modCoeff = static_cast<float> (1.0 - std::exp (-c_twoPi * c_modDisplayHz / m_sampleRate));

        reset();
    }

    void GrowlStage::reset()
    {
        std::fill (m_lineL.begin(), m_lineL.end(), 0.0f);
        std::fill (m_lineR.begin(), m_lineR.end(), 0.0f);
        m_write = 0;
        m_snap = true;
        m_f0 = 110.0f;
        m_voicedGate = 0.0f;
        m_halfPhase = 0.0;
        m_tremPhase = 0.0;
        m_shimmerFrom = m_shimmerTo = 1.0f;
        m_jitterFrom = m_jitterTo = 0.0f;
        m_jitterCount = 0;
        m_env = 0.0f;
        m_modLowpass = 0.0f;
        m_modDisplay = 0.0f;
        m_random = 0x9e3779b9u;
    }

    void GrowlStage::setParams (float amount01)
    {
        m_target = std::isfinite (amount01) ? std::clamp (amount01, 0.0f, 1.0f) : 0.0f;

        if (m_snap)
        {
            m_amount = m_target;
            m_snap = false;
        }
    }

    float GrowlStage::nextRandom()
    {
        m_random ^= m_random << 13;
        m_random ^= m_random >> 17;
        m_random ^= m_random << 5;
        return static_cast<float> (m_random) * (2.0f / 4294967296.0f) - 1.0f;
    }

    float GrowlStage::readDelay (const std::vector<float>& line, float delaySamples) const
    {
        // 4-point Hermite around (write - delay).
        const float pos = static_cast<float> (m_write) - delaySamples;
        const float floorPos = std::floor (pos);
        const float t = pos - floorPos;
        const int i0 = static_cast<int> (floorPos);

        const float xm1 = line[static_cast<size_t> ((i0 - 1) & m_mask)];
        const float x0 = line[static_cast<size_t> (i0 & m_mask)];
        const float x1 = line[static_cast<size_t> ((i0 + 1) & m_mask)];
        const float x2 = line[static_cast<size_t> ((i0 + 2) & m_mask)];

        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    void GrowlStage::process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo& voice)
    {
        if (m_lineL.empty())
            return;

        const bool twoChannels = stereo && right != nullptr;

        // f0 follows the tracker; with no pitch yet the last one is held.
        const float f0Target = (std::isfinite (voice.f0Hz) && voice.f0Hz > 0.0f)
                                   ? std::clamp (voice.f0Hz, c_minF0, c_maxF0) : m_f0;
        const float voicedTarget = voice.voiced
                                       ? std::clamp (std::isfinite (voice.confidence) ? 0.4f + voice.confidence : 1.0f,
                                                     0.0f, 1.0f)
                                       : 0.0f;
        const double invFs = 1.0 / m_sampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            float l = left[i];
            float r = twoChannels ? right[i] : 0.0f;
            l = std::isfinite (l) ? l : 0.0f;
            r = std::isfinite (r) ? r : 0.0f;

            m_amount += m_paramCoeff * (m_target - m_amount);
            m_f0 += m_f0Coeff * (f0Target - m_f0);
            m_voicedGate += m_voicedCoeff * (voicedTarget - m_voicedGate);

            const float a = m_amount;
            const float level = std::max (std::abs (l), std::abs (r));
            m_env += (level > m_env ? m_envAttack : m_envRelease) * (level - m_env);
            const float envDb = 20.0f * std::log10 (m_env + 1.0e-9f);
            const float intensity = c_quietIntensity
                                    + (1.0f - c_quietIntensity) * smoothstep01 ((envDb - c_quietDb) / (c_loudDb - c_quietDb));

            // ---- jitter delay
            if (++m_jitterCount >= m_jitterPeriod)
            {
                m_jitterCount = 0;
                m_jitterFrom = m_jitterTo;
                m_jitterTo = nextRandom();
            }

            const float jt = smoothstep01 (static_cast<float> (m_jitterCount) / static_cast<float> (m_jitterPeriod));
            const float jitter = m_jitterFrom + (m_jitterTo - m_jitterFrom) * jt;
            const float delay = static_cast<float> (m_baseDelay) + jitter * a * m_maxJitterSamples;

            m_lineL[static_cast<size_t> (m_write)] = l;
            if (twoChannels)
                m_lineR[static_cast<size_t> (m_write)] = r;

            const float dl = readDelay (m_lineL, delay);
            const float dr = twoChannels ? readDelay (m_lineR, delay) : 0.0f;
            m_write = (m_write + 1) & m_mask;

            // ---- period doubling at f0 / 2
            m_halfPhase += 0.5 * static_cast<double> (m_f0) * invFs;
            m_halfPhase -= std::floor (m_halfPhase);
            const float d1 = c_maxDoubling * a * intensity * m_voicedGate;
            const float doubling = 1.0f + d1 * static_cast<float> (std::sin (c_twoPi * m_halfPhase));

            // ---- tremolo chop
            const double tremHz = c_tremMinHz + (c_tremMaxHz - c_tremMinHz) * a;
            m_tremPhase += tremHz * invFs;

            if (m_tremPhase >= 1.0)
            {
                m_tremPhase -= std::floor (m_tremPhase);
                m_shimmerFrom = m_shimmerTo;
                m_shimmerTo = 1.0f + c_maxShimmer * a * nextRandom();
            }

            const float shimmer = m_shimmerFrom
                                  + (m_shimmerTo - m_shimmerFrom) * smoothstep01 (static_cast<float> (m_tremPhase));
            const float squareness = 1.0f + (c_maxSquareness - 1.0f) * a;
            const float sine = static_cast<float> (std::sin (c_twoPi * m_tremPhase));
            const float shape = std::tanh (squareness * sine) / std::tanh (squareness);
            const float depth = std::min (0.95f, c_maxTremDepth * a * intensity * shimmer);
            const float trem = 1.0f - depth * (0.5f + 0.5f * shape);

            // Divide by the RMS of the modulation so Amount changes the texture, not the loudness:
            // E[(1 + d1 sin)^2] = 1 + d1^2 / 2; the tremolo's E[shape^2] runs 0.5 (sine) .. ~0.9 (squarish).
            const float shapePower = 0.5f + 0.4f * a;
            const float tremMean = 1.0f - 0.5f * depth;
            const float tremPower = tremMean * tremMean + 0.25f * depth * depth * shapePower;
            const float norm = 1.0f / std::sqrt ((1.0f + 0.5f * d1 * d1) * tremPower);

            const float gain = doubling * trem * norm;
            left[i] = dl * gain;
            if (twoChannels)
                right[i] = dr * gain;

            m_modLowpass += m_modCoeff * (gain - 1.0f - m_modLowpass);
        }

        m_modDisplay = std::clamp (m_modLowpass * 2.0f, -1.0f, 1.0f);

        if (! std::isfinite (m_env) || ! std::isfinite (m_modLowpass) || ! std::isfinite (m_lineL[static_cast<size_t> ((m_write - 1) & m_mask)])
            || ! std::isfinite (m_f0) || ! std::isfinite (m_amount))
            reset();
    }
}

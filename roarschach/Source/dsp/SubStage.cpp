#include "SubStage.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_twoPi = 6.28318530717958647692;
        constexpr float c_q = 6.0f;
        constexpr float c_k = 1.0f / c_q;
        constexpr float c_glideSeconds = 0.020f;
        constexpr float c_gateFadeSeconds = 0.015f;
        constexpr float c_octaveConfirmSeconds = 0.040f;
        constexpr float c_lockSeconds = 0.004f;      // phase-lock time constant
        constexpr float c_envAttackSeconds = 0.005f;
        constexpr float c_envReleaseSeconds = 0.050f;
        constexpr float c_levelSeconds = 0.080f;
        constexpr float c_amountSeconds = 0.020f;
        constexpr float c_minHz = 40.0f, c_maxHz = 1500.0f;
        constexpr float c_minConfidence = 0.4f;
        // Sub peak = fundamental amplitude x amount x c_subGain (so 25 % puts the octave a little
        // under the voice's own fundamental, 100 % well above it).
        constexpr float c_subGain = 2.0f;
        constexpr float c_h2 = 0.3f, c_h3 = 0.12f;
        constexpr float c_shapeNorm = 1.0f / 1.2f;
        constexpr int c_filterInterval = 8;

        inline float onePole (float seconds, double fs)
        {
            return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * fs)));
        }
    }

    void SubStage::prepare (double sampleRate, int /*maxBlockSize*/)
    {
        m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        m_fs = static_cast<float> (m_sampleRate);
        m_glideCoeff = onePole (c_glideSeconds, m_sampleRate);
        m_gateStep = static_cast<float> (1.0 / (c_gateFadeSeconds * m_sampleRate));
        m_octaveConfirmSamples = static_cast<int> (c_octaveConfirmSeconds * m_sampleRate);
        m_lockCoeff = static_cast<float> (1.0 / (c_lockSeconds * m_sampleRate));
        m_envAttack = onePole (c_envAttackSeconds, m_sampleRate);
        m_envRelease = onePole (c_envReleaseSeconds, m_sampleRate);
        m_levelCoeff = onePole (c_levelSeconds, m_sampleRate);
        m_amountCoeff = onePole (c_amountSeconds, m_sampleRate);
        reset();
    }

    void SubStage::reset()
    {
        m_ic1 = m_ic2 = 0.0f;
        m_hasPitch = false;
        m_targetLogHz = m_logHz = std::log2 (100.0f);
        m_candidateLogHz = 0.0f;
        m_candidateSamples = 0;
        m_phase = 0.0;
        m_env = 0.0f;
        m_gate = 0.0f;
        m_gateOpen = false;
        m_amount = m_amountTarget;
        m_level = 0.0f;
        updateFilter();
    }

    void SubStage::setParams (float amount)
    {
        m_amountTarget = amount >= 0.0f ? std::min (amount, 1.0f) : 0.0f;  // NaN -> 0
    }

    float SubStage::getSubFrequency() const
    {
        return 0.5f * std::exp2 (m_logHz);
    }

    void SubStage::updateFilter()
    {
        m_filterCounter = 0;
        const float hz = std::clamp (std::exp2 (m_logHz), 20.0f, 0.45f * m_fs);
        m_g = static_cast<float> (std::tan (0.5 * c_twoPi * hz / m_sampleRate));
        m_a1 = 1.0f / (1.0f + m_g * (m_g + c_k));
        m_a2 = m_g * m_a1;
        m_a3 = m_g * m_a2;
    }

    void SubStage::updateTarget (const VoiceInfo& voice, int numSamples)
    {
        const bool usable = voice.voiced && voice.f0Hz >= c_minHz && voice.f0Hz <= c_maxHz
                            && voice.confidence >= c_minConfidence;
        m_gateOpen = usable;

        if (! usable)
        {
            m_candidateSamples = 0;
            return;
        }

        const float lg = std::log2 (voice.f0Hz);

        if (! m_hasPitch || m_gate <= 0.0f)
        {
            // Nothing sounding: jump straight to the new pitch.
            m_targetLogHz = m_logHz = lg;
            m_hasPitch = true;
            m_candidateSamples = 0;
            updateFilter();
            return;
        }

        const float distance = std::abs (lg - m_targetLogHz);
        const bool octave = std::abs (distance - 1.0f) < 0.125f;

        if (! octave)
        {
            m_targetLogHz = lg;
            m_candidateSamples = 0;
            return;
        }

        if (m_candidateSamples > 0 && std::abs (lg - m_candidateLogHz) < 0.125f)
            m_candidateSamples += numSamples;
        else
        {
            m_candidateLogHz = lg;
            m_candidateSamples = numSamples;
        }

        if (m_candidateSamples >= m_octaveConfirmSamples)
        {
            m_targetLogHz = m_candidateLogHz;
            m_candidateSamples = 0;
        }
    }

    void SubStage::process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo& voice)
    {
        updateTarget (voice, numSamples);
        const bool both = stereo && right != nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            // Non-finite samples become silence here and in the output, so the sum below stays finite.
            if (! (std::abs (left[i]) < 1.0e4f))
                left[i] = 0.0f;
            if (both && ! (std::abs (right[i]) < 1.0e4f))
                right[i] = 0.0f;
            const float x = both ? 0.5f * (left[i] + right[i]) : left[i];

            m_logHz += m_glideCoeff * (m_targetLogHz - m_logHz);
            if (++m_filterCounter >= c_filterInterval)
                updateFilter();

            // TPT SVF: v1 band-pass, v2 low-pass. Scaled by k both have unity gain at the centre and
            // sit 90 degrees apart: (bp, lp) is the fundamental as a phasor, A (cos phi, sin phi).
            const float v3 = x - m_ic2;
            const float v1 = m_a1 * m_ic1 + m_a2 * v3;
            const float v2 = m_ic2 + m_a2 * m_ic1 + m_a3 * v3;
            m_ic1 = 2.0f * v1 - m_ic1;
            m_ic2 = 2.0f * v2 - m_ic2;
            if (! std::isfinite (m_ic1) || ! std::isfinite (m_ic2))
                m_ic1 = m_ic2 = 0.0f;
            if (std::abs (m_ic1) < 1.0e-20f)
                m_ic1 = 0.0f;
            if (std::abs (m_ic2) < 1.0e-20f)
                m_ic2 = 0.0f;

            const float re = c_k * v1;
            const float im = c_k * v2;
            const float mag = std::sqrt (re * re + im * im);
            m_env += (mag > m_env ? m_envAttack : m_envRelease) * (mag - m_env);
            if (m_env < 1.0e-20f)
                m_env = 0.0f;

            const float s = static_cast<float> (std::sin (m_phase));
            const float c = static_cast<float> (std::cos (m_phase));
            const float sin2 = 2.0f * s * c;
            const float cos2 = c * c - s * s;

            // Phase lock: drive 2 * theta onto phi. sin (phi - 2 theta) from the cross product, no atan.
            double advance = c_twoPi * 0.5 * std::exp2 (static_cast<double> (m_logHz)) / m_sampleRate;
            if (m_gateOpen && mag > 1.0e-6f)
            {
                const float err = (im * cos2 - re * sin2) / mag;
                advance += 0.5 * m_lockCoeff * err;
            }
            m_phase += advance;
            if (m_phase >= c_twoPi)
                m_phase -= c_twoPi;
            else if (m_phase < 0.0)
                m_phase += c_twoPi;

            m_gate = m_gateOpen ? std::min (1.0f, m_gate + m_gateStep) : std::max (0.0f, m_gate - m_gateStep);
            m_amount += m_amountCoeff * (m_amountTarget - m_amount);

            // cos 2 theta follows cos phi: the drive's 2nd harmonic adds in phase with the voice.
            const float sin3 = s * (3.0f - 4.0f * s * s);
            const float shape = (s + c_h2 * cos2 + c_h3 * sin3) * c_shapeNorm;
            const float sub = m_env * m_amount * c_subGain * m_gate * shape;

            left[i] += sub;
            if (both)
                right[i] += sub;

            m_level += m_levelCoeff * (std::abs (sub) - m_level);
        }

        if (! std::isfinite (m_env) || ! std::isfinite (static_cast<float> (m_phase)))
        {
            m_env = 0.0f;
            m_phase = 0.0;
        }
        m_level = std::clamp (m_level, 0.0f, 1.0f);
    }
}

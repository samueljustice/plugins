#include "FinishStage.h"

#include <algorithm>

namespace roar
{
    namespace
    {
        constexpr double c_pi = 3.141592653589793;

        constexpr double c_hpfHz = FinishStage::c_highPassHz, c_lpfHz = FinishStage::c_lowPassHz;

        constexpr double c_weightHighPassHz = 80.0;
        constexpr double c_weightShelfHz = 1500.0, c_weightShelfDb = 4.0;
        constexpr double c_loudnessSeconds = 0.400;
        constexpr double c_gainGlideSeconds = 0.400;
        constexpr double c_quickLearnSeconds = 0.250;   // after a reset (see FinishStage.h)
        constexpr double c_quickGlideSeconds = 0.015;
        constexpr double c_quickMinSeconds = 0.005;     // average over at least this before using it
        constexpr float c_maxAutoGainDb = 12.0f;
        constexpr double c_learnGateDb = -50.0;   // input loudness below this: hold the gain

        constexpr float c_limThresholdDb = -4.0f;
        constexpr float c_ceilingDb = -1.0f;
        constexpr double c_limAttackSeconds = 0.0005, c_limReleaseSeconds = 0.080;

        constexpr float c_clipKnee = 0.9f;        // soft clip starts here, never exceeds 1.0

        float onePole (double seconds, double sampleRate)
        {
            return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * sampleRate)));
        }

        enum class Shape { lowPass, highPass, highShelf };

        template <typename Filter>
        void design (Filter& f, Shape shape, double hz, double gainDb, double sampleRate)
        {
            const double w = 2.0 * c_pi * std::min (hz, 0.45 * sampleRate) / sampleRate;
            const double cw = std::cos (w), sw = std::sin (w);
            const double alpha = sw / (2.0 * 0.7071067811865476);
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

            if (shape == Shape::lowPass)
            {
                b0 = (1.0 - cw) * 0.5; b1 = 1.0 - cw; b2 = b0;
                a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
            }
            else if (shape == Shape::highPass)
            {
                b0 = (1.0 + cw) * 0.5; b1 = -(1.0 + cw); b2 = b0;
                a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
            }
            else
            {
                const double A = std::pow (10.0, gainDb / 40.0);
                const double sq = 2.0 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1.0) + (A - 1.0) * cw + sq);
                b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
                b2 = A * ((A + 1.0) + (A - 1.0) * cw - sq);
                a0 = (A + 1.0) - (A - 1.0) * cw + sq;
                a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
                a2 = (A + 1.0) - (A - 1.0) * cw - sq;
            }

            f.b0 = b0 / a0;
            f.b1 = b1 / a0;
            f.b2 = b2 / a0;
            f.a1 = a1 / a0;
            f.a2 = a2 / a0;
        }

        float softClip (float x)
        {
            const float ax = std::abs (x);

            if (! (ax > c_clipKnee))
                return std::isfinite (x) ? x : 0.0f;

            const float range = 1.0f - c_clipKnee;
            const float y = c_clipKnee + range * std::tanh ((ax - c_clipKnee) / range);
            return std::copysign (std::min (y, 1.0f), x);
        }
    }

    void FinishStage::prepare (double sampleRate, int maxBlockSize)
    {
        m_inputPower.assign (static_cast<size_t> (std::max (1, maxBlockSize)), 0.0f);
        m_sampleRate = sampleRate > 1000.0 ? sampleRate : 48000.0;

        auto designWeighting = [this] (Weighting& w)
        {
            design (w.highPass, Shape::highPass, c_weightHighPassHz, 0.0, m_sampleRate);
            design (w.shelf, Shape::highShelf, c_weightShelfHz, c_weightShelfDb, m_sampleRate);
        };

        for (auto& c : m_channels)
        {
            design (c.hpf, Shape::highPass, c_hpfHz, 0.0, m_sampleRate);
            design (c.lpf, Shape::lowPass, c_lpfHz, 0.0, m_sampleRate);
            designWeighting (c.weighting);
        }

        designWeighting (m_inputWeighting);

        m_msCoeff = 1.0 - std::exp (-1.0 / (c_loudnessSeconds * m_sampleRate));
        m_gainCoeff = onePole (c_gainGlideSeconds, m_sampleRate);
        m_quickGainCoeff = onePole (c_quickGlideSeconds, m_sampleRate);
        m_quickLength = static_cast<int> (c_quickLearnSeconds * m_sampleRate);
        m_limAttack = onePole (c_limAttackSeconds, m_sampleRate);
        m_limRelease = onePole (c_limReleaseSeconds, m_sampleRate);

        reset();
    }

    void FinishStage::reset()
    {
        for (auto& c : m_channels)
        {
            c.hpf.clear();
            c.lpf.clear();
            c.weighting.clear();
        }

        m_inputWeighting.clear();
        m_inputMs = m_wetMs = 0.0;
        m_gainDb = 0.0f;
        m_autoGainDb = 0.0f;
        m_quickLearn = true;
        m_wetArrived = false;
        m_inputPowerCount = 0;
        m_quickIn = m_quickWet = 0.0;
        m_quickSamples = 0;
        m_limGain = 1.0f;
        m_limiterDb = 0.0f;
    }

    void FinishStage::setParams (bool autoGain)
    {
        m_autoGain = autoGain;
    }

    void FinishStage::measureInput (const float* input, int numSamples)
    {
        if (input == nullptr)
            return;

        m_inputPowerCount = std::min (numSamples, static_cast<int> (m_inputPower.size()));

        for (int i = 0; i < numSamples; ++i)
        {
            const float x = std::isfinite (input[i]) ? input[i] : 0.0f;
            const double w = m_inputWeighting.process (x);
            m_inputMs += m_msCoeff * (w * w - m_inputMs);

            if (i < m_inputPowerCount)
                m_inputPower[static_cast<size_t> (i)] = static_cast<float> (w * w);
        }

        if (! std::isfinite (m_inputMs) || ! m_inputWeighting.highPass.isFinite() || ! m_inputWeighting.shelf.isFinite())
        {
            m_inputWeighting.clear();
            m_inputMs = 0.0;
        }
    }

    bool FinishStage::stateIsFinite() const
    {
        for (const auto& c : m_channels)
            if (! c.hpf.isFinite() || ! c.lpf.isFinite() || ! c.weighting.highPass.isFinite() || ! c.weighting.shelf.isFinite())
                return false;

        return std::isfinite (m_wetMs) && std::isfinite (m_gainDb) && std::isfinite (m_limGain);
    }

    void FinishStage::process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo&)
    {
        const bool twoChannels = stereo && right != nullptr;
        const double gateMs = std::pow (10.0, c_learnGateDb / 10.0);
        const float range = c_ceilingDb - c_limThresholdDb;
        float maxReduction = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float l = std::isfinite (left[i]) ? left[i] : 0.0f;
            float r = twoChannels && std::isfinite (right[i]) ? right[i] : 0.0f;

            l = m_channels[0].lpf.process (m_channels[0].hpf.process (l));
            if (twoChannels)
                r = m_channels[1].lpf.process (m_channels[1].hpf.process (r));

            // ---- auto gain (wet loudness vs the input's)
            const double wl = m_channels[0].weighting.process (l);
            const double wr = twoChannels ? m_channels[1].weighting.process (r) : 0.0;
            const double wetPower = twoChannels ? 0.5 * (wl * wl + wr * wr) : wl * wl;
            m_wetMs += m_msCoeff * (wetPower - m_wetMs);

            float targetDb = m_gainDb;
            float coeff = m_gainCoeff;

            if (m_quickLearn)
            {
                // Starts with the first sample of wet through the chain's latency.
                m_wetArrived = m_wetArrived || m_wetMs > gateMs;

                if (m_wetArrived && m_inputMs > gateMs && i < m_inputPowerCount)
                {
                    m_quickIn += m_inputPower[static_cast<size_t> (i)];
                    m_quickWet += wetPower;
                    ++m_quickSamples;
                }
            }

            if (! m_autoGain)
            {
                targetDb = 0.0f;
            }
            else if (m_quickLearn)
            {
                // Plain averages since the wet arrived: unbiased from the first few milliseconds.
                if (m_quickSamples >= static_cast<int> (c_quickMinSeconds * m_sampleRate) && m_quickIn > 0.0 && m_quickWet > 0.0)
                {
                    targetDb = std::clamp (static_cast<float> (10.0 * std::log10 (m_quickIn / m_quickWet)),
                                           -c_maxAutoGainDb, c_maxAutoGainDb);
                    coeff = m_quickGainCoeff;
                }
            }
            else if (m_inputMs > gateMs && m_wetMs > 1.0e-12)
            {
                targetDb = std::clamp (static_cast<float> (10.0 * std::log10 (m_inputMs / m_wetMs)),
                                       -c_maxAutoGainDb, c_maxAutoGainDb);
            }

            m_gainDb += coeff * (targetDb - m_gainDb);
            const float gain = std::exp (m_gainDb * 0.115129255f);
            l *= gain;
            r *= gain;

            // ---- peak limiter, no look-ahead
            const float peak = std::max (std::abs (l), std::abs (r));
            float targetGain = 1.0f;

            if (peak > 1.0e-6f)
            {
                const float levelDb = 20.0f * std::log10 (peak);

                if (levelDb > c_limThresholdDb)
                {
                    const float outDb = c_limThresholdDb + range * std::tanh ((levelDb - c_limThresholdDb) / range);
                    targetGain = std::exp ((outDb - levelDb) * 0.115129255f);
                }
            }

            m_limGain += (targetGain < m_limGain ? m_limAttack : m_limRelease) * (targetGain - m_limGain);
            l *= m_limGain;
            r *= m_limGain;

            left[i] = softClip (l);
            if (twoChannels)
                right[i] = softClip (r);

            maxReduction = std::max (maxReduction, -20.0f * std::log10 (std::max (m_limGain, 1.0e-6f)));
        }

        m_limiterDb = maxReduction;
        m_autoGainDb = m_gainDb;

        // The quick learn hands over to the slow meters once it has averaged enough signal.
        if (m_quickSamples >= m_quickLength)
            m_quickLearn = false;
        m_inputPowerCount = 0;

        if (! stateIsFinite())
            reset();
    }
}

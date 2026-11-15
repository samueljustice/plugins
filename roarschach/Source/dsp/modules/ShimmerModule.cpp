#include "ShimmerModule.h"

#include "../KnobLaws.h"

#include "../MonsterFeatures.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_minRateHz = 0.5;
        constexpr double c_maxRateHz = laws::shimmerMaxRateHz;
        constexpr double c_loopHighpassHz = 30.0;
        constexpr double c_loopLowpassHz = 16000.0;
        constexpr double c_amountSmoothingSeconds = 0.020;
        constexpr double c_weightSmoothingSeconds = 0.020;
        constexpr double c_rateSmoothingSeconds = 0.050;
        constexpr double c_driftRateHz = 0.2;
        constexpr double c_driftDepth = 0.12;
        constexpr std::uint32_t c_driftSeed = 0x5f3759dfu;
        constexpr float c_halfSqrt2 = 0.70710678f;
        // 0.5 would be full power compensation; 0.42 keeps the beat crest within +2.5 dB at the
        // default (it leaves the average level 0.3 dB low at Amount 25 %, 0.5 dB at 50 %).
        constexpr float c_makeupExponent = 0.42f;
        // Share of the wet's power that goes back round the loop: Up+Down feeds back
        // (up + down) / 2, which carries half the power of its (up + down) / sqrt 2 output.
        float loopPowerFor (Params::ShimmerMode mode)
        {
            return mode == Params::ShimmerMode::upDown ? 0.5f : 1.0f;
        }
    }

    float ShimmerModule::feedbackForAmount (float m)
    {
        return std::clamp (0.30f + 0.53f * std::max (0.0f, m - 0.25f), 0.0f, 0.75f);
    }

    float ShimmerModule::makeupForAmount (float m)
    {
        m = std::clamp (m, 0.0f, 1.0f);
        return std::pow ((1.0f - m) * (1.0f - m) + m * m, -c_makeupExponent);
    }

    void ShimmerModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        m_maxBlock = std::max (1, maxBlock);

        m_amountCoeff = static_cast<float> (dsp::onePoleCoeff (0.5 * c_amountSmoothingSeconds, m_fs));
        m_weightCoeff = static_cast<float> (dsp::onePoleCoeff (0.5 * c_weightSmoothingSeconds, m_fs));
        m_rateCoeff = dsp::onePoleCoeff (c_rateSmoothingSeconds, m_fs / c_controlBlock);

        for (auto& ch : m_channels)
        {
            ch.hilbert.prepare (m_fs);
            ch.loopHighpass.prepare (m_fs);
            ch.loopHighpass.setCutoff (c_loopHighpassHz);
            ch.loopLowpass.prepare (m_fs);
            ch.loopLowpass.setCutoff (std::min (c_loopLowpassHz, 0.4 * m_fs));
        }

        m_drift.prepare (m_fs / c_controlBlock);
        m_drift.setRate (c_driftRateHz);

        m_fade.prepare (m_fs, m_maxBlock, getLatencySamples());
        reset();
    }

    void ShimmerModule::reset()
    {
        m_fade.setOn (m_settings.on);
        m_fade.reset();
        m_drift.seed (c_driftSeed);
        m_state = State {};
        resetWet();
        snapSmoothers();
    }

    void ShimmerModule::resetWet()
    {
        for (auto& ch : m_channels)
        {
            ch.hilbert.reset();
            ch.loopHighpass.reset();
            ch.loopLowpass.reset();
            ch.feedback = 0.0f;
        }
    }

    void ShimmerModule::snapSmoothers()
    {
        m_state.amount.snap (static_cast<float> (laws::shimmerAmount (static_cast<double> (m_settings.amount))));
        m_state.loopPower.snap (loopPowerFor (m_settings.mode));
        m_state.rateSmoothed = std::clamp (static_cast<double> (m_settings.rateHz), c_minRateHz, c_maxRateHz);
        m_state.shiftHz = m_state.rateSmoothed;
        m_state.controlLeft = 0;
        for (int c = 0; c < 2; ++c)
        {
            float up = 1.0f, down = 0.0f;
            targetWeights (c, up, down);
            m_channels[static_cast<size_t> (c)].wUp.snap (up);
            m_channels[static_cast<size_t> (c)].wDown.snap (down);
        }
    }

    bool ShimmerModule::makesStereo() const
    {
        return m_settings.mode == Params::ShimmerMode::stereo;
    }

    void ShimmerModule::setParams (const ShimmerSettings& s)
    {
        m_settings = s;
        if (! std::isfinite (m_settings.amount))
            m_settings.amount = 0.0f;
        if (! std::isfinite (m_settings.rateHz))
            m_settings.rateHz = static_cast<float> (c_minRateHz);
        m_settings.amount = std::clamp (m_settings.amount, 0.0f, 1.0f);
        m_settings.rateHz = std::clamp (m_settings.rateHz, static_cast<float> (c_minRateHz), static_cast<float> (c_maxRateHz));
        m_fade.setOn (s.on);
    }

    void ShimmerModule::targetWeights (int channel, float& up, float& down) const
    {
        switch (m_settings.mode)
        {
            case Params::ShimmerMode::down:   up = 0.0f; down = 1.0f; break;
            case Params::ShimmerMode::upDown: up = c_halfSqrt2; down = c_halfSqrt2; break;
            case Params::ShimmerMode::stereo:
                up = channel == 0 ? 0.0f : 1.0f;
                down = channel == 0 ? 1.0f : 0.0f;
                break;
            case Params::ShimmerMode::up:
            default:                          up = 1.0f; down = 0.0f; break;
        }
    }

    void ShimmerModule::startControlBlock()
    {
        auto& st = m_state;
        st.rateSmoothed += m_rateCoeff * (static_cast<double> (m_settings.rateHz) - st.rateSmoothed);
        const double drift = static_cast<double> (m_drift.process());   // runs always: the clone stays in step
        st.shiftHz = st.rateSmoothed * (m_diagnostics.drift ? 1.0 + c_driftDepth * drift : 1.0);

        st.theta = std::fmod (st.theta, dsp::c_twoPi);
        if (st.theta < 0.0)
            st.theta += dsp::c_twoPi;

        st.delta = dsp::c_twoPi * st.shiftHz / m_fs;
        st.cosTheta = std::cos (st.theta);
        st.sinTheta = std::sin (st.theta);
        st.cosDelta = std::cos (st.delta);
        st.sinDelta = std::sin (st.delta);
        st.controlLeft = c_controlBlock;
    }

    void ShimmerModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& /*voice*/)
    {
        n = std::min (n, m_maxBlock);
        m_fade.captureDry (l, r, n, stereo);
        if (! m_fade.isActive())
        {
            m_fade.outputDry (l, r, n, stereo);
            return;
        }
        if (m_fade.consumeRestart())
            resetWet();

        const int numChannels = stereo ? 2 : 1;
        float targetUp[2], targetDown[2];
        for (int c = 0; c < 2; ++c)
            targetWeights (c, targetUp[c], targetDown[c]);

        float* io[2] = { l, r };
        auto& st = m_state;
        const auto amountTarget = static_cast<float> (laws::shimmerAmount (static_cast<double> (m_settings.amount)));   // u -> m
        const float loopPowerTarget = loopPowerFor (m_settings.mode);
        const bool fixedFeedback = m_diagnostics.feedback >= 0.0f;
        const float gFixed = std::clamp (m_diagnostics.feedback, 0.0f, 0.95f);

        for (int i = 0; i < n; ++i)
        {
            if (st.controlLeft <= 0)
                startControlBlock();

            const float m = st.amount.step (amountTarget, m_amountCoeff);
            const float q = st.loopPower.step (loopPowerTarget, m_weightCoeff);
            const float g = fixedFeedback ? gFixed : feedbackForAmount (m);
            if (m != m_gains.m || g != m_gains.g || q != m_gains.q)   // derived from (m, g, q) only, so a clone agrees
            {
                const float makeup = makeupForAmount (m);
                m_gains = { m, g, q, makeup * (1.0f - m), makeup * m * std::sqrt (std::max (0.0f, 1.0f - g * g * q)) };
            }
            st.g = g;
            const float dryGain = m_gains.dry;
            const float wetGain = m_gains.wet;
            const float cs = static_cast<float> (st.cosTheta);
            const float sn = static_cast<float> (st.sinTheta);

            for (int c = 0; c < numChannels; ++c)
            {
                auto& ch = m_channels[static_cast<size_t> (c)];
                const float x = dsp::finiteOrZero (io[c][i]);

                const float u = x + g * ch.loopLowpass.processLowpass (ch.loopHighpass.processHighpass (ch.feedback));
                float iPath = 0.0f, qPath = 0.0f;
                ch.hilbert.process (u, iPath, qPath);
                const float up = iPath * cs - qPath * sn;
                const float down = iPath * cs + qPath * sn;

                const float wUp = ch.wUp.step (targetUp[c], m_weightCoeff);
                const float wDown = ch.wDown.step (targetDown[c], m_weightCoeff);
                const float wet = wUp * up + wDown * down;
                ch.feedback = dsp::flushTiny (dsp::finiteOrZero (wet / std::max (1.0f, wUp + wDown)));

                io[c][i] = dsp::flushTiny (dryGain * x + wetGain * wet);
            }

            // Advance the oscillator one sample (rotator; re-seeded from theta every block).
            const double c0 = st.cosTheta;
            st.cosTheta = c0 * st.cosDelta - st.sinTheta * st.sinDelta;
            st.sinTheta = st.sinTheta * st.cosDelta + c0 * st.sinDelta;
            st.theta += st.delta;
            --st.controlLeft;
        }

        m_fade.apply (l, r, n, stereo);
    }

    void ShimmerModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<ShimmerModule> (other);
        m_settings = o.m_settings;
        m_diagnostics = o.m_diagnostics;
        m_state = o.m_state;
        for (size_t c = 0; c < m_channels.size(); ++c)
        {
            m_channels[c].hilbert.copyStateFrom (o.m_channels[c].hilbert);
            m_channels[c].loopHighpass.copyStateFrom (o.m_channels[c].loopHighpass);
            m_channels[c].loopLowpass.copyStateFrom (o.m_channels[c].loopLowpass);
            m_channels[c].feedback = o.m_channels[c].feedback;
            m_channels[c].wUp = o.m_channels[c].wUp;
            m_channels[c].wDown = o.m_channels[c].wDown;
        }
        m_drift.copyStateFrom (o.m_drift);
        m_fade.copyStateFrom (o.m_fade);
    }

    void ShimmerModule::copyLeftStateToRight()
    {
        auto& left = m_channels[0];
        auto& right = m_channels[1];
        right.hilbert.copyStateFrom (left.hilbert);
        right.loopHighpass.copyStateFrom (left.loopHighpass);
        right.loopLowpass.copyStateFrom (left.loopLowpass);
        right.feedback = left.feedback;
        // An exact copy, sideband weights included: in Stereo mode the right channel then glides
        // from the left's sideband to its own over 20 ms instead of jumping to the mirror image.
        right.wUp = left.wUp;
        right.wDown = left.wDown;
        m_fade.copyLeftStateToRight();
    }

    void ShimmerModule::publishFeatures (MonsterFeatures& features) const
    {
        features.shimmerPhase.store (getPhase() * m_fade.getValue(), std::memory_order_relaxed);
    }
}

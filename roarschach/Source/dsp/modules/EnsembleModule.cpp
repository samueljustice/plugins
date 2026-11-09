#include "EnsembleModule.h"

#include "../KnobLaws.h"

#include "../MonsterFeatures.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_motionSmoothingSeconds = 0.150;
        constexpr double c_phaseSmoothingSeconds = 0.300;
        constexpr double c_gainSmoothingSeconds = 0.050;
        constexpr double c_readoutSeconds = 0.300;
        constexpr double c_amountSmoothingSeconds = 0.020;
        constexpr double c_randomLowHz = 0.7;
        constexpr double c_randomHighHz = 3.0;
        constexpr double c_fastDepthMs = 0.18;   // the 6.2 Hz component's depth, the same for every motion
        constexpr float c_halfSqrt2 = 0.70710678f;

        // Equal-power pan per voice: v0 left, v1 right, v2 centre, then the same again.
        constexpr float c_panLeft[3] = { 1.0f, 0.0f, c_halfSqrt2 };
        constexpr float c_panRight[3] = { 0.0f, 1.0f, c_halfSqrt2 };

        double wrap01 (double p) { return p - std::floor (p); }

        // Control-rate two-stage glide (stage, then value): the value and its slope stay
        // continuous when a target changes, so a chip change starts without a kink.
        template <typename T>
        void glide2 (T& stage, T& value, T target, T coeff)
        {
            stage += coeff * (target - stage);
            value += coeff * (stage - value);
        }

    }

    EnsembleModule::MotionSpec EnsembleModule::motionSpec (Params::EnsembleMotion motion)
    {
        switch (motion)
        {
            case Params::EnsembleMotion::slow:    return { 0.25f, 2.5f, 15.0f, 1.0f };
            case Params::EnsembleMotion::fast:    return { 1.2f, 1.0f, 9.0f, 0.4f };
            case Params::EnsembleMotion::vibrato: return { 5.5f, 0.8f, 7.0f, 0.2f };
            case Params::EnsembleMotion::medium:
            default:                              return { 0.45f, 1.8f, 12.0f, 0.5f };
        }
    }

    float EnsembleModule::sideGainForWidth (float w)
    {
        // laws::ensembleSideGain: 0.859 (W / 1.5)^1.3, a gentle start (10 % is effectively mono),
        // and 150 % stops where the side is as strong as the shared low end allows (the old
        // curve's 105 %), so the top of the travel never turns the chorus out of phase.
        return static_cast<float> (laws::ensembleSideGain (static_cast<double> (w)));
    }

    float EnsembleModule::sideScaleFor (int numVoices)
    {
        // Pink noise (1/f, 20 Hz - 20 kHz), mix 50 %: correlation about 0.73 at W 60 % and 0.2 at
        // W 150 %, for either voice count.
        return numVoices >= 6 ? 2.55f : 2.08f;
    }

    float EnsembleModule::makeupForAmount (float m)
    {
        m = std::clamp (m, 0.0f, 1.0f);
        return 1.0f / std::sqrt ((1.0f - m) * (1.0f - m) + m * m);
    }

    float EnsembleModule::voiceGainFor (int numVoices, Params::EnsembleMotion motion)
    {
        // Pink noise (1/f, 20 Hz - 20 kHz): wet (W = 0) RMS = dry RMS, measured per motion. The
        // longer delays of the slower motions comb deeper into the low octaves, so they need more.
        const bool six = numVoices >= 6;
        switch (motion)
        {
            case Params::EnsembleMotion::slow:    return six ? 0.547f : 0.895f;
            case Params::EnsembleMotion::fast:    return six ? 0.491f : 0.821f;
            case Params::EnsembleMotion::vibrato: return six ? 0.471f : 0.802f;
            case Params::EnsembleMotion::medium:
            default:                              return six ? 0.519f : 0.863f;
        }
    }

    void EnsembleModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        m_fastDepth = c_fastDepthMs * 0.001 * m_fs;
        m_maxBlock = std::max (1, maxBlock);

        const double controlRate = m_fs / c_controlBlock;
        m_motionCoeff = dsp::onePoleCoeff (0.5 * c_motionSmoothingSeconds, controlRate);
        m_phaseCoeff = dsp::onePoleCoeff (0.5 * c_phaseSmoothingSeconds, controlRate);
        m_gainCoeff = dsp::onePoleCoeff (0.5 * c_gainSmoothingSeconds, controlRate);
        m_readoutCoeff = dsp::onePoleCoeff (c_readoutSeconds, controlRate);
        m_amountCoeff = static_cast<float> (dsp::onePoleCoeff (0.5 * c_amountSmoothingSeconds, m_fs));

        m_minDelay = static_cast<float> (c_minDelayMs * 0.001 * m_fs);
        m_maxDelay = static_cast<float> (c_maxDelayMs * 0.001 * m_fs);
        m_line.prepare (static_cast<int> (std::ceil (m_maxDelay)) + 4);

        for (int v = 0; v < c_maxVoices; ++v)
        {
            auto& rnd = m_random[static_cast<size_t> (v)];
            rnd.prepare (controlRate);
            const double t = static_cast<double> (v) / static_cast<double> (c_maxVoices - 1);
            rnd.setRate (c_randomLowHz * std::pow (c_randomHighHz / c_randomLowHz, t));
        }

        m_fade.prepare (m_fs, m_maxBlock, getLatencySamples());
        reset();
    }

    void EnsembleModule::reset()
    {
        m_fade.setOn (m_settings.on);
        m_fade.reset();
        m_line.reset();
        for (int v = 0; v < c_maxVoices; ++v)
            m_random[static_cast<size_t> (v)].seed (0x2545f491u + 0x9e3779b9u * static_cast<std::uint32_t> (v + 1));
        m_state = State {};
        snapSmoothers();
    }

    void EnsembleModule::setParams (const EnsembleSettings& s)
    {
        m_settings = s;
        if (! std::isfinite (m_settings.amount))
            m_settings.amount = 0.0f;
        if (! std::isfinite (m_settings.width))
            m_settings.width = 0.0f;
        m_settings.amount = std::clamp (m_settings.amount, 0.0f, 1.0f);
        m_settings.width = std::clamp (m_settings.width, 0.0f, 1.5f);
        m_fade.setOn (s.on);
    }

    float EnsembleModule::effectiveAmount() const
    {
        const double m = laws::ensembleAmount (static_cast<double> (m_settings.amount));
        if (m_settings.motion != Params::EnsembleMotion::vibrato)
            return static_cast<float> (m);
        return static_cast<float> (laws::ensembleVibratoAmount (m));
    }

    int EnsembleModule::activeVoices() const
    {
        return m_settings.voices == Params::EnsembleVoices::six ? 6 : 3;
    }

    float EnsembleModule::voiceDelayTarget (int v)
    {
        const auto& st = m_state;
        const auto& voice = st.voices[static_cast<size_t> (v)];
        const double phi = static_cast<double> (voice.phase);
        const double lfo = 0.8 * std::sin (dsp::c_twoPi * (st.slowPhase + phi));
        const double fast = m_fastDepth * std::sin (dsp::c_twoPi * (st.fastPhase + 1.7 * phi));
        // Always advance the generator (once per voice per block), so a clone stays in step.
        const double rndValue = static_cast<double> (m_random[static_cast<size_t> (v)].process());
        const double rnd = m_diagnostics.random ? rndValue : 0.0;
        const double tau = st.delay * (1.0 + 0.07 * static_cast<double> (v)) + st.depth * lfo + fast + st.scatter * rnd;
        return static_cast<float> (std::clamp (tau, static_cast<double> (m_minDelay), static_cast<double> (m_maxDelay)));
    }

    void EnsembleModule::snapSmoothers()
    {
        startControlBlock (true);
    }

    void EnsembleModule::startControlBlock (bool snap)
    {
        auto& st = m_state;
        const auto spec = motionSpec (m_settings.motion);
        const double msToSamples = 0.001 * m_fs;
        const int numVoices = activeVoices();

        const double kMotion = snap ? 1.0 : m_motionCoeff;
        const double kPhase = snap ? 1.0 : m_phaseCoeff;
        const double kGain = snap ? 1.0 : m_gainCoeff;

        glide2 (st.rateStage, st.rateHz, static_cast<double> (spec.rateHz), kMotion);
        glide2 (st.depthStage, st.depth, static_cast<double> (spec.depthMs) * msToSamples, kMotion);
        glide2 (st.delayStage, st.delay, static_cast<double> (spec.delayMs) * msToSamples, kMotion);
        glide2 (st.scatterStage, st.scatter, static_cast<double> (spec.scatterMs) * msToSamples, kMotion);
        // Gains are smoothed at the control rate and ramped linearly per sample inside the block.
        const float invBlock = 1.0f / static_cast<float> (c_controlBlock);
        const auto ramp = [snap, invBlock] (float& value, float& step, float end)
        {
            if (snap)
            {
                value = end;
                step = 0.0f;
            }
            else
            {
                step = (end - value) * invBlock;
            }
        };
        const float gTarget = m_diagnostics.soloVoice >= 0 ? 1.0f : voiceGainFor (numVoices, m_settings.motion);
        glide2 (st.voiceGainStage, st.voiceGainEnd, gTarget, static_cast<float> (kGain));
        glide2 (st.sideScaleStage, st.sideScaleEnd, sideScaleFor (numVoices), static_cast<float> (kGain));
        ramp (st.voiceGain, st.voiceGainStep, st.voiceGainEnd);
        ramp (st.sideScale, st.sideScaleStep, st.sideScaleEnd);

        if (snap)
        {
            st.amount.snap (effectiveAmount());
            st.width.snap (sideGainForWidth (m_settings.width));
        }
        else
        {
            st.slowPhase = wrap01 (st.slowPhase + st.rateHz * c_controlBlock / m_fs);
            st.fastPhase = wrap01 (st.fastPhase + c_fastRateHz * c_controlBlock / m_fs);
        }

        for (int v = 0; v < c_maxVoices; ++v)
        {
            auto& voice = st.voices[static_cast<size_t> (v)];
            const bool active = m_diagnostics.soloVoice >= 0 ? v == m_diagnostics.soloVoice : v < numVoices;
            const float phaseTarget = v < numVoices ? static_cast<float> (v) / static_cast<float> (numVoices) : voice.phaseStage;
            glide2 (voice.phaseStage, voice.phase, phaseTarget, static_cast<float> (kPhase));
            glide2 (voice.gainStage, voice.gainEnd, active ? 1.0f : 0.0f, static_cast<float> (kGain));
            if (voice.gainEnd < 1.0e-4f && voice.gainStage < 1.0e-4f && ! active)
                voice.gainEnd = voice.gainStage = 0.0f;
            if (voice.gainEnd == 0.0f && voice.gain < 1.0e-6f)
                voice.gain = 0.0f;   // fully faded: stop reading this voice
            ramp (voice.gain, voice.gainStep, voice.gainEnd);

            const float target = voiceDelayTarget (v);
            if (snap)
            {
                voice.tau = target;
                voice.tauStep = 0.0f;
            }
            else
            {
                voice.tauStep = (target - voice.tau) / static_cast<float> (c_controlBlock);
            }
        }

        // Readout: block energies into slow trackers.
        st.midPower += m_readoutCoeff * (st.midAccum - st.midPower);
        st.sidePower += m_readoutCoeff * (st.sideAccum - st.sidePower);
        st.midPower = dsp::flushTiny (st.midPower);
        st.sidePower = dsp::flushTiny (st.sidePower);
        st.midAccum = 0.0;
        st.sideAccum = 0.0;
        st.controlLeft = c_controlBlock;
    }

    void EnsembleModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& /*voice*/)
    {
        n = std::min (n, m_maxBlock);
        m_fade.captureDry (l, r, n, stereo);
        if (! m_fade.isActive())
        {
            // Keep the line filled while Off (cheap), so the voices come back on real history.
            // Restarting from an empty line would make every voice start with a step once its
            // delay has passed: an audible click under the rising fade.
            for (int i = 0; i < n; ++i)
                m_line.push (stereo ? 0.5f * (dsp::finiteOrZero (l[i]) + dsp::finiteOrZero (r[i])) : l[i]);
            m_fade.outputDry (l, r, n, stereo);
            return;
        }
        m_fade.consumeRestart();

        auto& st = m_state;
        const float amountTarget = effectiveAmount();
        const float widthTarget = sideGainForWidth (m_settings.width);

        for (int i = 0; i < n; ++i)
        {
            if (st.controlLeft <= 0)
                startControlBlock (false);

            const float xl = dsp::finiteOrZero (l[i]);
            const float xr = stereo ? dsp::finiteOrZero (r[i]) : xl;
            m_line.push (stereo ? 0.5f * (xl + xr) : xl);

            float wetL = 0.0f, wetR = 0.0f;
            for (int v = 0; v < c_maxVoices; ++v)
            {
                auto& voice = st.voices[static_cast<size_t> (v)];
                if (voice.gain > 0.0f)
                {
                    const float tap = voice.gain * m_line.readHermite (voice.tau);
                    if (m_diagnostics.soloVoice >= 0)
                    {
                        wetL += tap;
                        wetR += tap;
                    }
                    else
                    {
                        wetL += c_panLeft[v % 3] * tap;
                        wetR += c_panRight[v % 3] * tap;
                    }
                }
                voice.tau += voice.tauStep;
                voice.gain += voice.gainStep;
            }

            const float m = st.amount.step (amountTarget, m_amountCoeff);
            const float width = st.width.step (widthTarget, m_amountCoeff);
            st.voiceGain += st.voiceGainStep;
            st.sideScale += st.sideScaleStep;
            const float makeup = makeupForAmount (m);
            const float wetGain = makeup * m * st.voiceGain;
            const float dryGain = makeup * (1.0f - m);
            const float mid = 0.5f * (wetL + wetR);
            const float side = 0.5f * (wetL - wetR) * (width * st.sideScale);

            const float outL = dsp::flushTiny (dryGain * xl + wetGain * (mid + side));
            if (stereo)
            {
                const float outR = dsp::flushTiny (dryGain * xr + wetGain * (mid - side));
                l[i] = outL;
                r[i] = outR;
                const double om = 0.5 * static_cast<double> (outL + outR);
                const double os = 0.5 * static_cast<double> (outL - outR);
                st.midAccum += om * om;
                st.sideAccum += os * os;
            }
            else
            {
                l[i] = dsp::flushTiny (dryGain * xl + wetGain * mid);
                st.midAccum += static_cast<double> (l[i]) * static_cast<double> (l[i]);
            }
            --st.controlLeft;
        }

        m_fade.apply (l, r, n, stereo);
    }

    void EnsembleModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<EnsembleModule> (other);
        m_settings = o.m_settings;
        m_diagnostics = o.m_diagnostics;
        m_state = o.m_state;
        for (size_t v = 0; v < m_random.size(); ++v)
            m_random[v].copyStateFrom (o.m_random[v]);
        m_line.copyStateFrom (o.m_line);
        m_fade.copyStateFrom (o.m_fade);
    }

    void EnsembleModule::copyLeftStateToRight()
    {
        // The voices read one shared mono line; only the On fade has a right-channel state.
        m_fade.copyLeftStateToRight();
    }

    float EnsembleModule::getCorrelation() const
    {
        const double total = m_state.midPower + m_state.sidePower;
        if (total < 1.0e-12)
            return 1.0f;
        return static_cast<float> ((m_state.midPower - m_state.sidePower) / total);
    }

    float EnsembleModule::getWidthReadout() const
    {
        return std::clamp (1.0f - getCorrelation(), 0.0f, 1.0f);
    }

    float EnsembleModule::getVoiceDelaySamples (int v) const
    {
        return m_state.voices[static_cast<size_t> (std::clamp (v, 0, c_maxVoices - 1))].tau;
    }

    void EnsembleModule::publishFeatures (MonsterFeatures& features) const
    {
        features.ensembleWidth.store (getWidthReadout() * m_fade.getValue(), std::memory_order_relaxed);
    }
}

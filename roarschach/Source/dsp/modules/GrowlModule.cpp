#include "GrowlModule.h"

#include "../MonsterFeatures.h"
#include "../common/CharacterSaturator.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr int c_sine = static_cast<int> (Params::GrowlShape::sine);
        constexpr int c_opto = static_cast<int> (Params::GrowlShape::opto);
        constexpr int c_square = static_cast<int> (Params::GrowlShape::square);
        constexpr int c_throat = static_cast<int> (Params::GrowlShape::throat);
        constexpr int c_rattle = static_cast<int> (Params::GrowlShape::rattle);

        constexpr int c_clean = static_cast<int> (Params::GrowlCharacter::clean);
        constexpr int c_warm = static_cast<int> (Params::GrowlCharacter::warm);
        constexpr int c_grind = static_cast<int> (Params::GrowlCharacter::grind);

        // The up-sampler's 2x outputs describe base-rate times m - 11.5 and m - 11 (Oversampler2x).
        constexpr int c_upDelayLate = 11;
        constexpr int c_upDelayEarly = 12;

        // Character fades in over the first 10 % of Depth (D = 0.085).
        constexpr float c_characterFullDepth = 0.085f;

        // Non-finite or absurd input (|x| >= 1e4, +80 dBFS) becomes silence, so no filter state is
        // left holding a huge value that takes seconds to decay.
        inline float sanitizeInput (float x)
        {
            return std::abs (x) < 1.0e4f ? x : 0.0f;   // also false for NaN
        }

        inline float cosine01 (double phase)
        {
            return static_cast<float> (0.5 + 0.5 * std::cos (dsp::c_twoPi * phase));
        }

        inline bool isSineShape (int shape)
        {
            return shape == c_sine || shape == c_throat || shape == c_rattle;
        }

        inline float squareIdeal (double phase)
        {
            return (phase < 0.25 || phase >= 0.75) ? 1.0f : 0.0f;
        }
    }

    double GrowlModule::sineMakeupFor (double depth)
    {
        const double d = std::clamp (depth, 0.0, c_maxDepth);
        const double meanSquare = (1.0 - 0.5 * d) * (1.0 - 0.5 * d) + d * d / 8.0;
        return std::min (1.0 / std::sqrt (meanSquare), dsp::dbToGain (c_makeupCapDb));
    }

    float GrowlModule::sineMakeupFor (float depth)
    {
        constexpr float cap = 1.4125375f;   // +3 dB
        const float d = std::clamp (depth, 0.0f, static_cast<float> (c_maxDepth));
        const float h = 1.0f - 0.5f * d;
        return std::min (1.0f / std::sqrt (h * h + 0.125f * d * d), cap);
    }

    void GrowlModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        m_maxBlock = std::max (1, maxBlock);

        m_paramCoeff = static_cast<float> (dsp::onePoleCoeff (0.020, m_fs));
        m_depthEffCoeff = static_cast<float> (dsp::onePoleCoeff (0.002, m_fs));
        m_makeupCoeff = static_cast<float> (dsp::onePoleCoeff (0.050, m_fs));
        m_envAttack = static_cast<float> (dsp::onePoleCoeff (c_envAttack, m_fs));
        m_envRelease = static_cast<float> (dsp::onePoleCoeff (c_envRelease, m_fs));
        m_voicingCoeff = static_cast<float> (dsp::onePoleCoeff (c_voicingTau, m_fs));
        m_f0Coeff = static_cast<float> (dsp::onePoleCoeff (0.010, m_fs));
        m_fadeStep = 1.0f / static_cast<float> (std::max (1, dsp::samplesFor (c_fadeSeconds, m_fs)));
        m_maxCycleSamples = static_cast<int> (1.1 * m_fs);   // the slowest cycle is 1 Hz (-0.5 % drift)

        m_drive[0] = static_cast<float> (dsp::dbToGain (c_warmDriveDb));
        m_drive[1] = static_cast<float> (dsp::dbToGain (c_grindDriveDb));
        m_invDrive[0] = 1.0f / m_drive[0];
        m_invDrive[1] = 1.0f / m_drive[1];

        m_lamp.prepare (m_fs);
        m_lamp.setTimes (c_optoTauOn, c_optoTauOff, c_optoReferenceHz, 0.2f);
        m_drift.prepare (m_fs);
        m_drift.setRate (c_driftRateHz);

        const double osRate = 2.0 * m_fs;
        for (auto& ch : m_channels)
        {
            ch.os.prepare();
            ch.split.prepare (osRate);
            ch.split.setCutoff (CharacterSaturator::c_warmSplitHz);
            ch.dcBlock.prepare (osRate);
            ch.dcBlock.setCutoff (c_dcBlockHz);
            ch.linear.prepare (Oversampler2x::c_latency);
            ch.linear.setFixedDelay (Oversampler2x::c_latency);
        }

        m_fade.prepare (m_fs, m_maxBlock, getLatencySamples());
        reset();
    }

    void GrowlModule::reset()
    {
        m_fade.reset();
        resetWet();
    }

    void GrowlModule::resetWet()
    {
        for (auto& ch : m_channels)
        {
            ch.os.reset();
            ch.split.reset();
            ch.dcBlock.reset();
            ch.linear.reset();
        }
        resetModulation();
    }

    void GrowlModule::resetModulation()
    {
        m_state = State {};
        m_state.gRing.fill (1.0f);
        m_lamp.reset (0.0f);
        m_drift.seed (0x5bd1e995u);
        snapToTargets();
    }

    void GrowlModule::runLatencyOnly (const float* l, const float* r, int n, bool stereo)
    {
        // Fully Off: keep the oversampler, the split and the Clean delay fed with the input
        // (gain 1, no curve), so switching On starts from the live signal, not from silence.
        const float* const in[2] = { l, r };
        for (int c = 0; c < (stereo ? 2 : 1); ++c)
        {
            auto& ch = m_channels[static_cast<size_t> (c)];
            for (int i = 0; i < n; ++i)
            {
                const float v = in[c][i];
                ch.linear.push (v);
                float a, b;
                ch.os.upsample (v, a, b);
                ch.split.processLowpass (a);
                ch.split.processLowpass (b);
                ch.dcBlock.processHighpass (0.0f);
                ch.dcBlock.processHighpass (0.0f);
                ch.os.downsample (a, b);
            }
        }
    }

    void GrowlModule::snapToTargets()
    {
        const double depth = depthForAmount (static_cast<double> (m_settings.depth));
        m_state.depth = static_cast<float> (depth);
        m_state.depthEff = m_settings.shape == Params::GrowlShape::throat ? 0.0f : m_state.depth;   // Throat waits for voicing
        m_state.logRate = std::log (std::clamp (static_cast<double> (m_settings.rateHz), 1.0, 150.0));
        m_state.rateNow = std::exp (m_state.logRate);
        m_state.lampRate = m_state.rateNow;
        m_lamp.setRate (m_state.lampRate);
        m_state.squareCoeff = static_cast<float> (dsp::onePoleCoeff (0.5 * std::max (c_squareMinTau, c_squareSmooth / m_state.rateNow), m_fs));

        m_state.shapeFrom = m_state.shapeTo = std::clamp (static_cast<int> (m_settings.shape), 0, c_numShapes - 1);
        m_state.shapeMix = 1.0f;

        const int character = static_cast<int> (m_settings.character);
        m_state.charFrom = m_state.charTo = (character == c_clean) ? c_warm : std::clamp (character, c_warm, c_grind);
        m_state.charMix = 1.0f;
        m_state.osWeight = (character == c_clean) ? 0.0f : std::min (1.0f, m_state.depth / c_characterFullDepth);

        m_state.makeupTarget = m_state.makeupMeasured = static_cast<float> (sineMakeupFor (depth));
        m_state.makeup = m_diagnostics.makeup ? sineMakeupFor (m_state.depthEff) : 1.0f;
        m_state.modulation = 0.0f;
    }

    void GrowlModule::setParams (const GrowlSettings& s)
    {
        m_settings = s;
        m_settings.depth = std::clamp (dsp::finiteOrZero (s.depth), 0.0f, 1.0f);
        m_settings.rateHz = std::clamp (std::isfinite (s.rateHz) ? s.rateHz : 27.0f, 1.0f, 150.0f);
        m_fade.setOn (s.on);
    }

    void GrowlModule::startShape (int shape)
    {
        m_state.shapeFrom = m_state.shapeTo;
        m_state.shapeTo = shape;
        m_state.shapeMix = 0.0f;

        // Start the stateful shapes from where they would be running.
        if (shape == c_opto)
            m_lamp.reset (m_lamp.lampFor (m_state.phase));
        else if (shape == c_square)
            m_state.square1 = m_state.square2 = squareIdeal (m_state.phase);
    }

    void GrowlModule::startCharacter (int character)
    {
        m_state.charFrom = m_state.charTo;
        m_state.charTo = character;
        m_state.charMix = 0.0f;
    }

    float GrowlModule::nextUniform()
    {
        auto& x = m_state.random;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return static_cast<float> (static_cast<double> (x) * (2.0 / 4294967295.0) - 1.0);
    }

    float GrowlModule::shapeValue (int shape, double phase, double throatPhase)
    {
        switch (shape)
        {
            case c_opto:
            {
                const float light = m_lamp.process (phase);
                return 1.0f - (light > 0.0f ? std::pow (light, 0.8f) : 0.0f);
            }
            case c_square:
            {
                const float a = m_state.squareCoeff;
                m_state.square1 += a * (squareIdeal (phase) - m_state.square1);
                m_state.square2 += a * (m_state.square1 - m_state.square2);
                return m_state.square2;
            }
            case c_throat:
                return cosine01 (throatPhase);
            case c_sine:
            case c_rattle:
            default:
                return cosine01 (phase);
        }
    }

    float GrowlModule::characterSample (Channel& ch, float w, float lift)
    {
        const float lo = ch.split.processLowpass (w);
        const auto residual = [&] (int character)
        {
            if (character == c_grind)
            {
                const double b = c_grindBias * static_cast<double> (lift);
                const double d = static_cast<double> (m_drive[1]);
                const double z = d * static_cast<double> (w);
                const double y = CharacterSaturator::shape (CharacterSaturator::Curve::grind, z + b)
                               - CharacterSaturator::shape (CharacterSaturator::Curve::grind, b);
                return static_cast<float> (y / d - static_cast<double> (w));
            }
            const double b = c_warmBias * static_cast<double> (lift);
            const double d = static_cast<double> (m_drive[0]);
            const double z = d * static_cast<double> (lo);
            const double y = CharacterSaturator::shape (CharacterSaturator::Curve::warm, z + b)
                           - CharacterSaturator::shape (CharacterSaturator::Curve::warm, b);
            return static_cast<float> (y / d - static_cast<double> (lo));
        };

        float r = residual (m_state.charTo);
        if (m_state.charMix < 1.0f)
        {
            const float w1 = dsp::smoothstep01 (m_state.charMix);
            r = residual (m_state.charFrom) + w1 * (r - residual (m_state.charFrom));
        }
        return w + ch.dcBlock.processHighpass (r);
    }

    void GrowlModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice)
    {
        n = std::min (n, m_maxBlock);
        for (int i = 0; i < n; ++i)
        {
            l[i] = sanitizeInput (l[i]);
            if (stereo)
                r[i] = sanitizeInput (r[i]);
        }

        m_fade.captureDry (l, r, n, stereo);
        if (! m_fade.isActive())
        {
            runLatencyOnly (l, r, n, stereo);
            m_state.modulation = 0.0f;
            m_fade.outputDry (l, r, n, stereo);
            return;
        }
        if (m_fade.consumeRestart())
            resetModulation();   // the channel filters kept running on the input while Off

        auto& st = m_state;
        const float depthTarget = static_cast<float> (depthForAmount (static_cast<double> (m_settings.depth)));
        const double logRateTarget = std::log (static_cast<double> (m_settings.rateHz));
        const int shapeSetting = std::clamp (static_cast<int> (m_settings.shape), 0, c_numShapes - 1);
        const int charSetting = static_cast<int> (m_settings.character);
        const bool clean = charSetting == c_clean;
        const float envThreshold = static_cast<float> (c_envThresholdDb);
        const float envRange = static_cast<float> (c_envRangeDb);
        const float maxDepth = static_cast<float> (c_maxDepth);
        const float makeupCap = static_cast<float> (dsp::dbToGain (c_makeupCapDb));
        const double invFs = 1.0 / m_fs;
        const int numChannels = stereo ? 2 : 1;
        float* const io[2] = { l, r };

        for (int i = 0; i < n; ++i)
        {
            const float x0 = l[i];
            const float x1 = stereo ? r[i] : 0.0f;

            // Level: one detector for both channels.
            const float peak = std::max (std::abs (x0), std::abs (x1));
            st.env += (peak > st.env ? m_envAttack : m_envRelease) * (peak - st.env);
            st.env = dsp::flushTiny (st.env);
            const float mEnv = m_diagnostics.envelope
                                 ? std::clamp ((dsp::gainToDb (st.env) - envThreshold) / envRange, 0.0f, 1.0f)
                                 : 0.0f;

            st.depth += m_paramCoeff * (depthTarget - st.depth);
            st.logRate += static_cast<double> (m_paramCoeff) * (logRateTarget - st.logRate);

            // Voice: f0 (held) and the voicing gate for Throat.
            const VoiceFrame& frame = voice.atSample (i);
            if (frame.f0Hz > 0.0f && std::isfinite (frame.f0Hz))
                st.f0 += static_cast<double> (m_f0Coeff) * (std::clamp (static_cast<double> (frame.f0Hz), 20.0, 2000.0) - st.f0);
            const float voicedTarget = (frame.voiced && frame.f0Hz > 0.0f) ? 1.0f : 0.0f;
            if (voicedTarget > 0.0f && st.voicing < 1.0e-3f)
                st.throatPhase = 0.0;   // lock the f0 / 2 phase to the voicing onset
            st.voicing += m_voicingCoeff * (voicedTarget - st.voicing);

            // Shape crossfade (a change that arrives during a fade waits for it to finish).
            if (st.shapeMix < 1.0f)
                st.shapeMix = std::min (1.0f, st.shapeMix + m_fadeStep);
            else if (shapeSetting != st.shapeTo)
                startShape (shapeSetting);
            const float wTo = dsp::smoothstep01 (st.shapeMix);
            const float wFrom = 1.0f - wTo;
            const float wRattle = (st.shapeTo == c_rattle ? wTo : 0.0f) + (st.shapeFrom == c_rattle ? wFrom : 0.0f);
            const float wThroat = (st.shapeTo == c_throat ? wTo : 0.0f) + (st.shapeFrom == c_throat ? wFrom : 0.0f);

            // Rate.
            const double drift = c_driftAmount * static_cast<double> (m_drift.process());
            double rate = std::exp (st.logRate) * (m_diagnostics.drift ? 1.0 + drift : 1.0);
            if (wRattle > 0.0f)
                rate *= 1.0 + (std::exp2 (c_rattleOctaves * static_cast<double> (mEnv)) - 1.0) * static_cast<double> (wRattle);
            st.rateNow = wThroat > 0.5f ? 0.5 * st.f0 : rate;

            if ((st.controlCount++ & 31u) == 0u)
            {
                st.lampRate = rate;
                m_lamp.setRate (rate);
                st.squareCoeff = static_cast<float> (dsp::onePoleCoeff (0.5 * std::max (c_squareMinTau, c_squareSmooth / rate), m_fs));
            }

            // Phases.
            st.phase += rate * invFs;
            const bool wrapMain = st.phase >= 1.0;
            if (wrapMain)
                st.phase -= std::floor (st.phase);
            st.throatPhase += 0.5 * st.f0 * invFs;
            const bool wrapThroat = st.throatPhase >= 1.0;
            if (wrapThroat)
                st.throatPhase -= std::floor (st.throatPhase);

            // Once per cycle: the makeup for the cycle just finished, and the next depth jitter.
            const bool cycle = (st.shapeTo == c_throat) ? wrapThroat : wrapMain;
            if (cycle || st.gSquareCount >= m_maxCycleSamples)
            {
                if (st.gSquareCount > 0)
                {
                    const double meanSquare = st.gSquareSum / static_cast<double> (st.gSquareCount);
                    st.makeupTarget = meanSquare > 0.0
                                        ? std::clamp (static_cast<float> (1.0 / std::sqrt (meanSquare)), 1.0f, makeupCap)
                                        : makeupCap;
                }
                st.gSquareSum = 0.0;
                st.gSquareCount = 0;
                if (cycle)
                    st.humanize = m_diagnostics.humanize ? nextUniform() : 0.0f;
            }

            // Shape value and gain.
            float s = shapeValue (st.shapeTo, st.phase, st.throatPhase);
            if (wFrom > 0.0f)
                s = wTo * s + wFrom * shapeValue (st.shapeFrom, st.phase, st.throatPhase);

            float depthNow = st.depth * (1.0f + static_cast<float> (c_envDepthPush) * mEnv)
                                      * (1.0f + static_cast<float> (c_humanizeDepth) * st.humanize)
                                      * (1.0f + (st.voicing - 1.0f) * wThroat);
            depthNow = std::clamp (depthNow, 0.0f, maxDepth);
            st.depthEff += m_depthEffCoeff * (depthNow - st.depthEff);

            const float g = 1.0f - st.depthEff * (1.0f - s);
            st.gSquareSum += static_cast<double> (g) * static_cast<double> (g);
            ++st.gSquareCount;

            st.makeupMeasured += m_makeupCoeff * (st.makeupTarget - st.makeupMeasured);
            if (m_diagnostics.makeup)
            {
                const float wSine = (isSineShape (st.shapeTo) ? wTo : 0.0f) + (isSineShape (st.shapeFrom) ? wFrom : 0.0f);
                const float measured = wSine < 1.0f ? st.makeupMeasured : 1.0f;
                st.makeup = measured + wSine * (sineMakeupFor (st.depthEff) - measured);
            }
            else
                st.makeup = 1.0f;
            const float gm = g * st.makeup;

            st.gPos = (st.gPos + 1) & (c_gRingSize - 1);
            st.gRing[static_cast<size_t> (st.gPos)] = g;
            const float gLate = st.gRing[static_cast<size_t> ((st.gPos - c_upDelayLate) & (c_gRingSize - 1))];
            const float gEarly = st.gRing[static_cast<size_t> ((st.gPos - c_upDelayEarly) & (c_gRingSize - 1))];
            const float lift0 = 1.0f - 0.5f * (gLate + gEarly);
            const float lift1 = 1.0f - gLate;

            // Character weights.
            if (st.charMix < 1.0f)
                st.charMix = std::min (1.0f, st.charMix + m_fadeStep);
            else if (! clean && charSetting != st.charTo)
                startCharacter (std::clamp (charSetting, c_warm, c_grind));
            const float osTarget = clean ? 0.0f : std::min (1.0f, st.depth / c_characterFullDepth);
            st.osWeight = std::clamp (osTarget, st.osWeight - m_fadeStep, st.osWeight + m_fadeStep);

            for (int c = 0; c < numChannels; ++c)
            {
                auto& ch = m_channels[static_cast<size_t> (c)];
                const float v = io[c][i] * gm;
                const float linear = ch.linear.processFixed (v);

                float a, b;
                ch.os.upsample (v, a, b);
                if (st.osWeight > 0.0f)
                {
                    a = characterSample (ch, a, lift0);
                    b = characterSample (ch, b, lift1);
                }
                else
                {
                    ch.split.processLowpass (a);   // keep the split warm for a switch back
                    ch.split.processLowpass (b);
                    ch.dcBlock.processHighpass (0.0f);
                    ch.dcBlock.processHighpass (0.0f);
                }
                const float curved = ch.os.downsample (a, b);
                io[c][i] = st.osWeight >= 1.0f ? curved : linear + st.osWeight * (curved - linear);
            }

            st.modulation = gm - 1.0f;
        }

        m_fade.apply (l, r, n, stereo);
    }

    void GrowlModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<GrowlModule> (other);
        m_settings = o.m_settings;
        m_diagnostics = o.m_diagnostics;
        m_state = o.m_state;
        for (size_t c = 0; c < m_channels.size(); ++c)
        {
            m_channels[c].os.copyStateFrom (o.m_channels[c].os);
            m_channels[c].split.copyStateFrom (o.m_channels[c].split);
            m_channels[c].dcBlock.copyStateFrom (o.m_channels[c].dcBlock);
            m_channels[c].linear.copyStateFrom (o.m_channels[c].linear);
        }
        m_lamp.setRate (m_state.lampRate);
        m_lamp.copyStateFrom (o.m_lamp);
        m_drift.copyStateFrom (o.m_drift);
        m_fade.copyStateFrom (o.m_fade);
    }

    void GrowlModule::copyLeftStateToRight()
    {
        auto& left = m_channels[0];
        auto& right = m_channels[1];
        right.os.copyStateFrom (left.os);
        right.split.copyStateFrom (left.split);
        right.dcBlock.copyStateFrom (left.dcBlock);
        right.linear.copyStateFrom (left.linear);
        m_fade.copyLeftStateToRight();
    }

    void GrowlModule::publishFeatures (MonsterFeatures& features) const
    {
        const float mod = std::clamp (m_state.modulation, -1.0f, 1.0f) * m_fade.getValue();
        features.growlMod.store (mod, std::memory_order_relaxed);
    }
}

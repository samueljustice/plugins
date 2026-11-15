#include "SwirlModule.h"

#include "../KnobLaws.h"

#include "../MonsterFeatures.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr int c_lfoLevel = static_cast<int> (Params::SwirlMotion::lfoLevel);
        constexpr int c_random = static_cast<int> (Params::SwirlMotion::random);
        constexpr int c_numMotions = 3;
        constexpr int c_numCircuits = 6;
        constexpr int c_testCircuit = -1;

        // Non-finite or absurd input (|x| >= 1e4, +80 dBFS) becomes silence, so no filter state is
        // left holding a huge value that takes seconds to decay.
        inline float sanitizeInput (float x)
        {
            return std::abs (x) < 1.0e4f ? x : 0.0f;   // also false for NaN
        }

        /** 2 tanh (s / 2), with a rational tanh (exact at 0, monotonic, 1 at |s / 2| >= 3). */
        inline float softLimitState (float s)
        {
            const float h = std::clamp (0.5f * s, -3.0f, 3.0f);
            const float h2 = h * h;
            return 2.0f * h * (27.0f + h2) / (27.0f + 9.0f * h2);
        }
    }

    // ------------------------------------------------------------------------------------ core

    double SwirlModule::fastTan (double x)
    {
        // tan (x) = 2 t / (1 - t^2) with t = tan (x / 2) from its [5/4] Pade approximant.
        const double y = 0.5 * x;
        const double y2 = y * y;
        const double t = y * (945.0 - 105.0 * y2 + y2 * y2) / (945.0 - 420.0 * y2 + 15.0 * y2 * y2);
        return 2.0 * t / (1.0 - t * t);
    }

    void SwirlModule::Coeffs::setStage (int i, double g)
    {
        const double inv = 1.0 / (1.0 + g);
        const auto k = static_cast<size_t> (i);
        G[k] = static_cast<float> (g * inv);
        a[k] = static_cast<float> ((g - 1.0) * inv);
        b2[k] = static_cast<float> (2.0 * inv);
    }

    float SwirlModule::Chain::process (float x, const Coeffs& c, float k, bool modern)
    {
        const int n = c.stages;

        // The chain is affine in its input: A u = Gc u + Sc.
        float gc = 1.0f, sc = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const auto j = static_cast<size_t> (i);
            sc = c.a[j] * sc + c.b2[j] * m_s[j];
            gc *= c.a[j];
        }

        const float den = 1.0f - k * gc;
        const float y = modern ? (gc * x + sc - k * x) / den : (gc * x + sc) / den;
        const float u = x + k * y;

        float in = u;
        for (int i = 0; i < n; ++i)
        {
            const auto j = static_cast<size_t> (i);
            const float v = (in - m_s[j]) * c.G[j];
            const float lp = v + m_s[j];
            const float s = lp + v;
            m_s[j] = modern ? dsp::flushTiny (s) : dsp::flushTiny (softLimitState (s));
            in = 2.0f * lp - in;
        }
        return dsp::finiteOrZero (y);
    }

    // ------------------------------------------------------------------------------- circuits

    SwirlModule::Circuit SwirlModule::circuitFor (Params::SwirlCircuit c)
    {
        Circuit r;
        switch (c)
        {
            case Params::SwirlCircuit::smooth4: r = { 4, 0.0, 900.0, 3.0, 0.0, false, 1.0, 1.0, 2.0 }; break;
            case Params::SwirlCircuit::talky4:  r = { 4, 0.0, 800.0, 2.5, 0.65, false, 1.0, 1.0, 3.0 }; break;
            case Params::SwirlCircuit::deep6:   r = { 6, 0.3, 600.0, 3.0, 0.55, true, 1.0, 1.0, 3.0 }; break;
            case Params::SwirlCircuit::deep12:  r = { 12, 0.4, 500.0, 3.5, 0.5, true, 1.0, -1.0, 2.0 }; break;
            case Params::SwirlCircuit::dark5:   r = { 5, 0.6, 650.0, 2.5, 0.5, false, 1.0, 1.0, 4.0 }; break;
            case Params::SwirlCircuit::liquid4:
            default:                            r = { 4, 1.2, 700.0, 2.5, 0.45, false, 1.0, 1.0, 3.0 }; break;
        }
        return r;
    }

    double SwirlModule::kFor (double k0, double amount)
    {
        if (! (k0 > 0.0))
            return 0.0;
        // The resonance rises with the mix over the whole travel (1.74 k0 at full wet; it used to stop
        // at 1.3 k0 from a = 0.61), so the top of the travel deepens the swirl, not only the blend.
        // Below a = 0.3 it holds 0.94 k0, so a small mix is already a resonant swirl (it used to
        // fall to 0.6 k0, and the first steps were lost under the rest of a chain).
        const double k = k0 * std::max (0.94, 0.6 + 0.4 * std::max (0.0, amount) / c_defaultAmount);
        return std::min (k, c_maxK);
    }

    double SwirlModule::makeupFor (double k)
    {
        const double kk = std::min (std::abs (k), c_makeupKCap);
        return 1.0 / std::sqrt (0.5 * (1.0 + 1.0 / (1.0 - kk)));
    }

    void SwirlModule::Engine::setCircuit (const Circuit& c, int circuitId)
    {
        circuit = c;
        circuit.stages = std::clamp (c.stages, 1, c_maxStages);
        id = circuitId;
        const int n = circuit.stages;
        for (int i = 0; i < c_maxStages; ++i)
        {
            const double pos = n > 1 ? static_cast<double> (i) / static_cast<double> (n - 1) - 0.5 : 0.0;
            ratio[static_cast<size_t> (i)] = std::pow (10.0, circuit.spreadDecades * pos);
        }
        coeffs.stages = n;
        for (auto& ch : chains)
            ch.reset();
    }

    // --------------------------------------------------------------------------------- module

    void SwirlModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        m_maxBlock = std::max (1, maxBlock);

        m_paramCoeff = static_cast<float> (dsp::onePoleCoeff (0.020, m_fs));
        m_envAttack = static_cast<float> (dsp::onePoleCoeff (c_envAttack, m_fs));
        m_envRelease = static_cast<float> (dsp::onePoleCoeff (c_envRelease, m_fs));
        m_engineStep = 1.0f / static_cast<float> (std::max (1, dsp::samplesFor (0.020, m_fs)));
        m_motionStep = 1.0f / static_cast<float> (std::max (1, dsp::samplesFor (0.050, m_fs)));
        m_kneeLevel = static_cast<float> (dsp::dbToGain (c_safetyKneeDb));
        m_kneeRange = static_cast<float> (dsp::dbToGain (c_safetyCeilingDb)) - m_kneeLevel;

        m_lamp.prepare (m_fs);
        m_lamp.setTimes (c_lampTauOn, c_lampTauOff, c_lampReferenceHz, 0.2f);

        for (auto& ch : m_channels)
        {
            ch.os.prepare();
            ch.drive.prepare (2.0 * m_fs);
            ch.drive.setCurve (CharacterSaturator::Curve::warm);
        }

        m_fade.prepare (m_fs, m_maxBlock, getLatencySamples());
        reset();
    }

    void SwirlModule::reset()
    {
        m_fade.reset();
        resetWet();
    }

    void SwirlModule::resetWet()
    {
        for (auto& ch : m_channels)
        {
            ch.os.reset();
            ch.drive.reset();
        }
        resetMotion();
    }

    void SwirlModule::resetMotion()
    {
        m_state = State {};
        m_lamp.reset (0.0f);
        snapToTargets();
    }

    void SwirlModule::snapToTargets()
    {
        m_state.amount = static_cast<float> (laws::swirlMix (static_cast<double> (m_settings.amount)));
        m_state.logRate = std::log (static_cast<double> (m_settings.rateHz));
        m_state.lampRate = static_cast<double> (m_settings.rateHz);
        m_lamp.setRate (m_state.lampRate);
        m_state.randomCoeff = static_cast<float> (dsp::onePoleCoeff (c_randomSmooth / m_state.lampRate, m_fs));
        m_state.motionFrom = m_state.motionTo = std::clamp (static_cast<int> (m_settings.motion), 0, c_numMotions - 1);
        m_state.motionMix = 1.0f;

        m_state.engineTo = 0;
        m_state.engineMix = 1.0f;
        m_engines[0].setCircuit (requestedCircuit(), requestedCircuitId());
        m_engines[1].setCircuit (requestedCircuit(), requestedCircuitId());
        m_state.drive = static_cast<float> (dsp::dbToGain (m_engines[0].circuit.driveDb));
    }

    void SwirlModule::runLatencyOnly (const float* l, const float* r, int n, bool stereo)
    {
        // Fully Off: keep the driven, oversampled input path running (cheap), so switching On
        // starts from the live signal. The stage chains (the costly part) are skipped.
        const float* const in[2] = { l, r };
        for (int c = 0; c < (stereo ? 2 : 1); ++c)
        {
            auto& ch = m_channels[static_cast<size_t> (c)];
            for (int i = 0; i < n; ++i)
            {
                float a, b;
                ch.os.upsample (in[c][i] * m_state.drive, a, b);
                a = ch.drive.process (a);
                b = ch.drive.process (b);
                ch.os.downsample (a, b);
            }
        }
    }

    int SwirlModule::requestedCircuitId() const
    {
        return m_diagnostics.useCircuit ? c_testCircuit
                                        : std::clamp (static_cast<int> (m_settings.circuit), 0, c_numCircuits - 1);
    }

    SwirlModule::Circuit SwirlModule::requestedCircuit() const
    {
        return m_diagnostics.useCircuit ? m_diagnostics.circuit
                                        : circuitFor (static_cast<Params::SwirlCircuit> (requestedCircuitId()));
    }

    void SwirlModule::setParams (const SwirlSettings& s)
    {
        m_settings = s;
        m_settings.amount = std::clamp (dsp::finiteOrZero (s.amount), 0.0f, 1.0f);
        m_settings.rateHz = std::clamp (std::isfinite (s.rateHz) ? s.rateHz : 0.35f, 0.05f, 10.0f);
        m_fade.setOn (s.on);
    }

    void SwirlModule::setDiagnostics (const Diagnostics& d)
    {
        m_diagnostics = d;
        snapToTargets();
    }

    float SwirlModule::getNotchPosition() const
    {
        const double f = std::max (20.0, static_cast<double> (m_state.notchHz));
        return static_cast<float> (std::clamp (std::log (f / 20.0) / std::log (1000.0), 0.0, 1.0));
    }

    float SwirlModule::nextUnit()
    {
        auto& x = m_state.random;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return static_cast<float> (static_cast<double> (x) / 4294967295.0);
    }

    float SwirlModule::kForEngine (const Engine& e, float amount) const
    {
        const double k = m_diagnostics.k >= 0.0 ? std::min (m_diagnostics.k, 0.99)
                                                : kFor (e.circuit.k, static_cast<double> (amount));
        return static_cast<float> (k * (e.circuit.resPolarity < 0.0 ? -1.0 : 1.0));
    }

    void SwirlModule::updateCoeffs (Engine& e, double q, double envOct)
    {
        const double octaves = m_diagnostics.motion ? e.circuit.sweepOctaves * q + envOct : 0.0;
        const double base = e.circuit.freqHz * std::exp2 (octaves);
        const double hiHz = c_maxStageRatio * m_fs;
        const double piOverFs = dsp::c_pi / m_fs;
        for (int i = 0; i < e.circuit.stages; ++i)
        {
            const double f = std::clamp (base * e.ratio[static_cast<size_t> (i)], c_minStageHz, hiHz);
            e.coeffs.setStage (i, fastTan (piOverFs * f));
        }
    }

    float SwirlModule::safety (float p) const
    {
        if (! m_diagnostics.safety)
            return p;
        const float a = std::abs (p);
        if (a <= m_kneeLevel)
            return p;
        const float y = m_kneeLevel + m_kneeRange * std::tanh ((a - m_kneeLevel) / m_kneeRange);
        return p < 0.0f ? -y : y;
    }

    void SwirlModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& /*voice*/)
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
            m_fade.outputDry (l, r, n, stereo);
            return;
        }
        if (m_fade.consumeRestart())
            resetMotion();   // the oversampled input path kept running while Off

        auto& st = m_state;
        const auto amountTarget = static_cast<float> (laws::swirlMix (static_cast<double> (m_settings.amount)));   // u -> a
        const double logRateTarget = std::log (static_cast<double> (m_settings.rateHz));
        const int motionSetting = std::clamp (static_cast<int> (m_settings.motion), 0, c_numMotions - 1);
        const int circuitSetting = requestedCircuitId();
        const float envThreshold = static_cast<float> (c_envThresholdDb);
        const float envRange = static_cast<float> (c_envRangeDb);
        const double invFs = 1.0 / m_fs;
        const int numChannels = stereo ? 2 : 1;
        float* const io[2] = { l, r };
        const float* const dry[2] = { m_fade.dryLeft(), m_fade.dryRight() };

        for (int i = 0; i < n; ++i)
        {
            // Level: one detector for both channels.
            const float peak = std::max (std::abs (l[i]), stereo ? std::abs (r[i]) : 0.0f);
            st.env += (peak > st.env ? m_envAttack : m_envRelease) * (peak - st.env);
            st.env = dsp::flushTiny (st.env);
            const float mEnv = std::clamp ((dsp::gainToDb (st.env) - envThreshold) / envRange, 0.0f, 1.0f);

            st.amount += m_paramCoeff * (amountTarget - st.amount);
            st.logRate += static_cast<double> (m_paramCoeff) * (logRateTarget - st.logRate);
            const double rate = std::exp (st.logRate);

            if ((st.controlCount++ & 31u) == 0u)
            {
                st.lampRate = rate;
                m_lamp.setRate (rate);
                st.randomCoeff = static_cast<float> (dsp::onePoleCoeff (c_randomSmooth / rate, m_fs));
            }

            // Motion sources (all run, so a motion change can crossfade).
            st.phase += rate * invFs;
            if (st.phase >= 1.0)
            {
                st.phase -= std::floor (st.phase);
                st.randomTarget = nextUnit();
            }
            const float light = m_lamp.process (st.phase);
            const float lfo = light > 0.0f ? std::pow (light, static_cast<float> (c_lampExponent)) : 0.0f;
            st.random1 += st.randomCoeff * (st.randomTarget - st.random1);
            st.random2 += st.randomCoeff * (st.random1 - st.random2);

            if (st.motionMix < 1.0f)
                st.motionMix = std::min (1.0f, st.motionMix + m_motionStep);
            else if (motionSetting != st.motionTo)
            {
                st.motionFrom = st.motionTo;
                st.motionTo = motionSetting;
                st.motionMix = 0.0f;
            }
            const auto motionValue = [&] (int motion, double& q, double& envOct)
            {
                q = static_cast<double> ((motion == c_random ? st.random2 : lfo) - 0.5f);   // LFO and LFO+Level share the lamp
                envOct = motion == c_lfoLevel ? c_envOctaves * static_cast<double> (mEnv) : 0.0;
            };
            double q, envOct;
            motionValue (st.motionTo, q, envOct);
            if (st.motionMix < 1.0f)
            {
                double q0, e0;
                motionValue (st.motionFrom, q0, e0);
                const double w = static_cast<double> (dsp::smoothstep01 (st.motionMix));
                q = q0 + w * (q - q0);
                envOct = e0 + w * (envOct - e0);
            }

            // Circuit crossfade (a change that arrives during a fade waits for it to finish).
            if (st.engineMix < 1.0f)
                st.engineMix = std::min (1.0f, st.engineMix + m_engineStep);
            else if (circuitSetting != m_engines[static_cast<size_t> (st.engineTo)].id)
            {
                st.engineTo = 1 - st.engineTo;
                m_engines[static_cast<size_t> (st.engineTo)].setCircuit (requestedCircuit(), circuitSetting);
                st.engineMix = 0.0f;
            }
            Engine& eTo = m_engines[static_cast<size_t> (st.engineTo)];
            Engine& eFrom = m_engines[static_cast<size_t> (1 - st.engineTo)];
            const bool fading = st.engineMix < 1.0f;
            const float wTo = dsp::smoothstep01 (st.engineMix);

            const float driveTarget = static_cast<float> (dsp::dbToGain (eTo.circuit.driveDb));
            st.drive += m_paramCoeff * (driveTarget - st.drive);

            updateCoeffs (eTo, q, envOct);
            const float kTo = kForEngine (eTo, st.amount);
            const float makeupTo = (m_diagnostics.makeup && ! eTo.circuit.modern) ? static_cast<float> (makeupFor (static_cast<double> (kTo))) : 1.0f;
            const float polTo = eTo.circuit.polarity < 0.0 ? -1.0f : 1.0f;

            float kFrom = 0.0f, makeupFrom = 1.0f, polFrom = 1.0f;
            if (fading)
            {
                updateCoeffs (eFrom, q, envOct);
                kFrom = kForEngine (eFrom, st.amount);
                makeupFrom = (m_diagnostics.makeup && ! eFrom.circuit.modern) ? static_cast<float> (makeupFor (static_cast<double> (kFrom))) : 1.0f;
                polFrom = eFrom.circuit.polarity < 0.0 ? -1.0f : 1.0f;
            }

            for (int c = 0; c < numChannels; ++c)
            {
                auto& ch = m_channels[static_cast<size_t> (c)];
                const auto ci = static_cast<size_t> (c);

                // Driven input, 2x oversampled (exactly 23 samples late, like the aligned dry).
                float a, b;
                ch.os.upsample (io[c][i] * st.drive, a, b);
                a = ch.drive.process (a);
                b = ch.drive.process (b);
                const float xIn = ch.os.downsample (a, b);

                const float yTo = eTo.chains[ci].process (xIn, eTo.coeffs, kTo, eTo.circuit.modern);
                float p = 0.5f * (xIn + polTo * yTo) * makeupTo;
                if (fading)
                {
                    const float yFrom = eFrom.chains[ci].process (xIn, eFrom.coeffs, kFrom, eFrom.circuit.modern);
                    const float pFrom = 0.5f * (xIn + polFrom * yFrom) * makeupFrom;
                    p = pFrom + wTo * (p - pFrom);
                }
                p = safety (p);

                const float d = dry[c][i];
                io[c][i] = d + st.amount * (p - d);
            }

            if (i == n - 1)
            {
                const double base = eTo.circuit.freqHz * std::exp2 (m_diagnostics.motion ? eTo.circuit.sweepOctaves * q + envOct : 0.0);
                st.notchHz = static_cast<float> (base * std::tan (dsp::c_pi / (2.0 * eTo.circuit.stages)));
                st.modOctaves = static_cast<float> (eTo.circuit.sweepOctaves * q + envOct);
                st.kNow = kTo;
            }
        }

        m_fade.apply (l, r, n, stereo);
    }

    void SwirlModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<SwirlModule> (other);
        m_settings = o.m_settings;
        m_diagnostics = o.m_diagnostics;
        m_state = o.m_state;
        for (size_t e = 0; e < m_engines.size(); ++e)
        {
            m_engines[e].circuit = o.m_engines[e].circuit;
            m_engines[e].id = o.m_engines[e].id;
            m_engines[e].ratio = o.m_engines[e].ratio;
            m_engines[e].coeffs = o.m_engines[e].coeffs;
            for (size_t c = 0; c < 2; ++c)
                m_engines[e].chains[c].copyStateFrom (o.m_engines[e].chains[c]);
        }
        for (size_t c = 0; c < m_channels.size(); ++c)
        {
            m_channels[c].os.copyStateFrom (o.m_channels[c].os);
            m_channels[c].drive.copyStateFrom (o.m_channels[c].drive);
        }
        m_lamp.setRate (m_state.lampRate);
        m_lamp.copyStateFrom (o.m_lamp);
        m_fade.copyStateFrom (o.m_fade);
    }

    void SwirlModule::copyLeftStateToRight()
    {
        for (auto& e : m_engines)
            e.chains[1].copyStateFrom (e.chains[0]);
        m_channels[1].os.copyStateFrom (m_channels[0].os);
        m_channels[1].drive.copyStateFrom (m_channels[0].drive);
        m_fade.copyLeftStateToRight();
    }

    void SwirlModule::publishFeatures (MonsterFeatures& features) const
    {
        features.swirlNotch.store (getNotchPosition() * m_fade.getValue(), std::memory_order_relaxed);
    }
}

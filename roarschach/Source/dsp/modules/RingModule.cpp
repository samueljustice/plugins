#include "RingModule.h"

#include "../KnobLaws.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr int c_sine = static_cast<int> (RingCarrier::sine);
        constexpr int c_square = static_cast<int> (RingCarrier::square);
        constexpr int c_bodyOff = static_cast<int> (RingBody::off);

        constexpr float c_sqrt2 = 1.41421356237f;
        constexpr float c_sqrt3 = 1.73205080757f;
        constexpr float c_amMakeupCap = 1.41253754462f;   // +3 dB

        // Non-finite or absurd input (|x| >= 1e4, +80 dBFS) becomes silence on the wet path, so no
        // filter state is left holding a huge value.
        inline float sanitizeInput (float x);

        // The level the carrier-pitch gate sees: the louder channel of the input.
        inline float inputPeak (const float* l, const float* r, int i, bool stereo)
        {
            const float a = std::abs (sanitizeInput (l[i]));
            return stereo ? std::max (a, std::abs (sanitizeInput (r[i]))) : a;
        }

        inline float sanitizeInput (float x)
        {
            return std::abs (x) < 1.0e4f ? x : 0.0f;   // also false for NaN
        }

        inline double wrapPhase (double phase)
        {
            return phase >= 1.0 ? phase - 1.0 : phase;
        }

        // Group delay at DC of the TPT (bilinear) one-pole low-pass, in samples.
        inline double lowpassDelaySamples (double hz, double fs)
        {
            return 1.0 / (2.0 * TptOnePole::gFor (hz, fs));
        }
    }

    float RingModule::shapeMakeup (RingCarrier carrier)
    {
        switch (carrier)
        {
            case RingCarrier::triangle: return c_sqrt3;
            case RingCarrier::square:   return 1.0f;
            case RingCarrier::sine:     break;
        }
        return c_sqrt2;
    }

    float RingModule::amMakeupFor (float amount, float shapeMakeupValue)
    {
        const float a = std::clamp (amount, 0.0f, 1.0f);
        const float h = 1.0f - 0.5f * a;
        const float side = 0.5f * a / std::max (shapeMakeupValue, 1.0f);
        return std::min (1.0f / std::sqrt (h * h + side * side), c_amMakeupCap);
    }

    void RingModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        m_maxBlock = std::max (1, maxBlock);
        m_oversample = m_fs <= c_oversampleMaxRate;
        m_fsOs = m_oversample ? 2.0 * m_fs : m_fs;
        m_invFsOs = 1.0 / m_fsOs;
        m_harmonicMaxHz = std::min (c_harmonicMaxHz, c_harmonicMaxFraction * m_fs);

        m_logBase = std::log2 (c_levelBaseHz);
        m_logMin = std::log2 (c_minCarrierHz);
        m_logMax = std::log2 (c_maxCarrierFraction * m_fs);
        const int topLevel = static_cast<int> (std::floor (m_logMax - m_logBase));
        m_numLevels = std::clamp (topLevel + 2, 2, c_maxLevels);

        const double interval = static_cast<double> (c_controlInterval);
        m_amountCoeff = static_cast<float> (dsp::onePoleCoeff (c_paramSeconds, m_fs));
        m_ctlParamCoeff = 1.0 - std::exp (-interval / (c_paramSeconds * m_fs));
        m_f0Coeff = 1.0 - std::exp (-interval / (c_f0GlideTau * m_fs));
        m_trackStep = interval / (c_trackGlideSeconds * m_fs);
        m_octaveConfirmSamples = dsp::samplesFor (c_octaveConfirmSeconds, m_fs, 1);
        m_pitchGate.prepare (m_fs, c_controlInterval);
        m_shapeStep = 1.0f / static_cast<float> (dsp::samplesFor (c_shapeFadeSeconds, m_fs, 1));
        m_modeStep = 1.0f / static_cast<float> (dsp::samplesFor (c_modeFadeSeconds, m_fs, 1));
        m_bodyStep = 1.0f / static_cast<float> (dsp::samplesFor (c_bodyFadeSeconds, m_fs, 1));

        int maxCombDelay = 4;
        for (size_t b = 0; b < c_bodies.size(); ++b)
        {
            const auto& spec = c_bodies[b];
            const double delay = spec.delaySeconds * m_fs;
            // z[n - D] is read before z[n] is pushed (read (0) is z[n - 1]), and the loop low-pass
            // adds its own delay, so the tap sits that much earlier.
            m_combRead[b] = static_cast<float> (std::max (1.0, delay - 1.0 - lowpassDelaySamples (spec.lowpassHz, m_fs)));
            m_combNorm[b] = std::sqrt (1.0f - spec.feedback * spec.feedback);
            maxCombDelay = std::max (maxCombDelay, static_cast<int> (std::ceil (delay)) + 4);
        }

        for (auto& ch : m_channels)
        {
            ch.os.prepare();
            ch.direct.prepare (Oversampler2x::c_latency);
            ch.direct.setFixedDelay (Oversampler2x::c_latency);
            ch.dcIn.prepare (m_fs);
            ch.dcIn.setCutoff (c_inputDcHz);
            ch.dcOut.prepare (m_fs);
            ch.dcOut.setCutoff (c_wetDcHz);
            for (size_t b = 0; b < ch.combs.size(); ++b)
            {
                ch.combs[b].line.prepare (maxCombDelay);
                ch.combs[b].lowpass.prepare (m_fs);
                ch.combs[b].lowpass.setCutoff (c_bodies[b].lowpassHz);
            }
        }

        buildTables();

        m_fade.prepare (m_fs, m_maxBlock, getLatencySamples());
        reset();
    }

    void RingModule::buildTables()
    {
        struct Plan
        {
            int size;
            int maxHarmonic;
        };

        std::array<Plan, c_maxLevels> plans {};
        size_t total = static_cast<size_t> (c_minTableSize + 3);
        for (int level = 0; level < m_numLevels; ++level)
        {
            const double top = c_levelBaseHz * std::exp2 (static_cast<double> (level + 1));
            int k = std::max (1, static_cast<int> (std::floor (m_harmonicMaxHz / top)));
            if (k % 2 == 0)
                --k;
            const int size = std::max (c_minTableSize, dsp::nextPowerOfTwo (c_pointsPerHarmonic * k));
            plans[static_cast<size_t> (level)] = { size, k };
            total += 2 * static_cast<size_t> (size + 3);
        }

        m_tables.assign (total, 0.0f);
        std::vector<double> sines;
        std::vector<double> sum;
        int offset = 1;

        const auto fill = [&] (int size, int maxHarmonic, int shape) -> TableRef
        {
            sines.resize (static_cast<size_t> (size));
            for (int i = 0; i < size; ++i)
                sines[static_cast<size_t> (i)] = std::sin (dsp::c_twoPi * static_cast<double> (i) / static_cast<double> (size));

            sum.assign (static_cast<size_t> (size), 0.0);
            for (int k = 1; k <= maxHarmonic; k += 2)
            {
                double amp = 1.0;
                if (shape == c_square)
                    amp = 4.0 / (dsp::c_pi * static_cast<double> (k));
                else if (shape != c_sine)
                    amp = (((k - 1) / 2) % 2 == 0 ? 1.0 : -1.0) * 8.0 / (dsp::c_pi * dsp::c_pi * static_cast<double> (k) * static_cast<double> (k));
                const int mask = size - 1;
                for (int i = 0; i < size; ++i)
                    sum[static_cast<size_t> (i)] += amp * sines[static_cast<size_t> ((k * i) & mask)];
            }

            TableRef ref { offset, size };
            float* t = m_tables.data() + offset;
            for (int i = 0; i < size; ++i)
                t[i] = static_cast<float> (sum[static_cast<size_t> (i)]);
            t[-1] = t[size - 1];
            t[size] = t[0];
            t[size + 1] = t[1];
            offset += size + 3;
            return ref;
        };

        m_sineTable = fill (c_minTableSize, 1, c_sine);
        for (int shape = 1; shape <= 2; ++shape)
            for (int level = 0; level < m_numLevels; ++level)
            {
                const auto& plan = plans[static_cast<size_t> (level)];
                m_levels[static_cast<size_t> (shape - 1)][static_cast<size_t> (level)] = fill (plan.size, plan.maxHarmonic, shape);
            }
    }

    void RingModule::reset()
    {
        m_fade.reset();
        resetWet();
        snapToSettings();
    }

    void RingModule::resetWet()
    {
        for (auto& ch : m_channels)
        {
            ch.os.reset();
            ch.direct.reset();
            ch.dcIn.reset();
            ch.dcOut.reset();
            for (auto& comb : ch.combs)
            {
                comb.line.reset();
                comb.lowpass.reset();
            }
        }
    }

    void RingModule::snapMixState()
    {
        m_s.shapeFrom = m_s.shapeTo = static_cast<int> (m_settings.carrier);
        m_s.shapeMix = 1.0f;
        m_s.bodyFrom = m_s.bodyTo = static_cast<int> (m_settings.body);
        m_s.bodyMix = 1.0f;
        m_s.modeMix = m_settings.mode == RingMode::am ? 1.0f : 0.0f;
        m_s.amount = m_settings.amount;
    }

    void RingModule::snapToSettings()
    {
        State s;
        s.phase = 0.0;
        s.logFreq = std::log2 (static_cast<double> (m_settings.freqHz));
        s.logRatio = std::log2 (static_cast<double> (m_settings.ratio));
        s.f0Log = s.f0Target = std::log2 (c_initialF0Hz);
        s.hasPitch = false;
        s.trackMix = m_settings.track == RingTrack::follow ? 1.0 : 0.0;
        s.amount = m_settings.amount;
        s.shapeFrom = s.shapeTo = static_cast<int> (m_settings.carrier);
        s.shapeMix = 1.0f;
        s.modeMix = m_settings.mode == RingMode::am ? 1.0f : 0.0f;
        s.bodyFrom = s.bodyTo = static_cast<int> (m_settings.body);
        s.bodyMix = 1.0f;
        s.tickCountdown = 0;
        m_s = s;
        m_pitchGate.reset();
        m_s.logNow = targetLog();
        m_s.logStep = 0.0;
        m_s.fcNow = std::exp2 (m_s.logNow);
    }

    void RingModule::setParams (const RingSettings& s)
    {
        m_settings = s;
        m_settings.amount = std::clamp (dsp::finiteOrZero (s.amount), 0.0f, 1.0f);
        m_settings.freqHz = static_cast<float> (std::clamp (std::isfinite (s.freqHz) ? static_cast<double> (s.freqHz) : 150.0,
                                                            c_minFreqHz, c_maxFreqHz));
        m_settings.ratio = static_cast<float> (std::clamp (std::isfinite (s.ratio) ? static_cast<double> (s.ratio) : 1.0,
                                                           c_minRatio, c_maxRatio));
        m_settings.carrier = static_cast<RingCarrier> (std::clamp (static_cast<int> (s.carrier), 0, 2));
        m_settings.body = static_cast<RingBody> (std::clamp (static_cast<int> (s.body), 0, 2));
        m_fade.setOn (s.on);
    }

    float RingModule::getBodyFeature() const
    {
        const auto mixOf = [] (int body) { return body == c_bodyOff ? 0.0f : c_bodies[static_cast<size_t> (body - 1)].mix; };
        const float w = dsp::smoothstep01 (m_s.bodyMix);
        return ((1.0f - w) * mixOf (m_s.bodyFrom) + w * mixOf (m_s.bodyTo)) * m_fade.getValue();
    }

    float RingModule::getFollowedF0Hz() const
    {
        return static_cast<float> (std::exp2 (m_s.f0Log));
    }

    void RingModule::startShape (int shape)
    {
        if (m_s.shapeMix < 1.0f && shape == m_s.shapeFrom)
        {
            std::swap (m_s.shapeFrom, m_s.shapeTo);
            m_s.shapeMix = 1.0f - m_s.shapeMix;
            return;
        }
        m_s.shapeFrom = m_s.shapeMix >= 0.5f ? m_s.shapeTo : m_s.shapeFrom;
        m_s.shapeTo = shape;
        m_s.shapeMix = 0.0f;
    }

    void RingModule::startBody (int body)
    {
        if (m_s.bodyMix < 1.0f && body == m_s.bodyFrom)
        {
            std::swap (m_s.bodyFrom, m_s.bodyTo);
            m_s.bodyMix = 1.0f - m_s.bodyMix;
            return;
        }
        m_s.bodyFrom = m_s.bodyMix >= 0.5f ? m_s.bodyTo : m_s.bodyFrom;
        m_s.bodyTo = body;
        m_s.bodyMix = 0.0f;
    }

    void RingModule::applySettingChanges()
    {
        const int shape = static_cast<int> (m_settings.carrier);
        if (shape != m_s.shapeTo)
            startShape (shape);
        const int body = static_cast<int> (m_settings.body);
        if (body != m_s.bodyTo)
            startBody (body);
    }

    double RingModule::targetLog() const
    {
        const double w = dsp::smoothstep01 (m_s.trackMix);
        const double fixedLog = m_s.logFreq;
        const double followLog = m_s.f0Log + m_s.logRatio;
        return std::clamp ((1.0 - w) * fixedLog + w * followLog, m_logMin, m_logMax);
    }

    void RingModule::updateTracker (const VoiceFrame& frame)
    {
        // Only voiced, confident frames at a real level move the followed pitch (CarrierPitchGate):
        // the noise floor between phrases and the quiet tail of a word leave it where it was.
        const auto decision = m_pitchGate.tick (m_s.gatePeak, frame, static_cast<float> (c_minF0Hz), static_cast<float> (c_maxF0Hz));
        m_s.gatePeak = 0.0f;

        if (decision.accepted)
        {
            const double lg = std::log2 (static_cast<double> (frame.f0Hz));
            if (! m_s.hasPitch || decision.phraseStart)
            {
                // The first pitch, and the first pitch of a new phrase, are taken at once.
                m_s.f0Target = m_s.f0Log = lg;
                m_s.hasPitch = true;
                m_s.candidateSamples = 0;
            }
            else
            {
                const double distance = std::abs (lg - m_s.f0Target);
                if (std::abs (distance - 1.0) >= c_octaveTolerance)
                {
                    m_s.f0Target = lg;
                    m_s.candidateSamples = 0;
                }
                else
                {
                    // An octave jump (the classic tracker error) must persist before it is followed.
                    if (m_s.candidateSamples > 0 && std::abs (lg - m_s.candidateLog) < c_octaveTolerance)
                        m_s.candidateSamples += c_controlInterval;
                    else
                    {
                        m_s.candidateLog = lg;
                        m_s.candidateSamples = c_controlInterval;
                    }
                    if (m_s.candidateSamples >= m_octaveConfirmSamples)
                    {
                        m_s.f0Target = m_s.candidateLog;
                        m_s.candidateSamples = 0;
                    }
                }
            }
        }
        else
        {
            m_s.candidateSamples = 0;
            if (decision.restHz > 0.0f && m_s.hasPitch)
                m_s.f0Target = std::log2 (static_cast<double> (decision.restHz));   // in a pause: wait at the voice's usual pitch
        }

        m_s.f0Log += m_f0Coeff * (m_s.f0Target - m_s.f0Log);
    }

    void RingModule::controlTick (const VoiceFrame& frame)
    {
        m_s.logFreq += m_ctlParamCoeff * (std::log2 (static_cast<double> (m_settings.freqHz)) - m_s.logFreq);
        m_s.logRatio += m_ctlParamCoeff * (std::log2 (static_cast<double> (m_settings.ratio)) - m_s.logRatio);
        updateTracker (frame);

        if (m_settings.track == RingTrack::follow)
            m_s.trackMix = std::min (1.0, m_s.trackMix + m_trackStep);
        else
            m_s.trackMix = std::max (0.0, m_s.trackMix - m_trackStep);

        m_s.logStep = (targetLog() - m_s.logNow) / static_cast<double> (c_controlInterval);
    }

    void RingModule::advanceControl (int i, const VoiceFrameView& voice, float peak)
    {
        if (m_s.tickCountdown <= 0)
        {
            controlTick (voice.atSample (i));
            m_s.tickCountdown = c_controlInterval;
        }
        --m_s.tickCountdown;
        m_s.gatePeak = std::max (m_s.gatePeak, peak);
        m_s.logNow += m_s.logStep;
        m_s.fcNow = std::exp2 (m_s.logNow);
    }

    float RingModule::readTable (const TableRef& table, double phase) const
    {
        const double pos = phase * static_cast<double> (table.size);
        int i = static_cast<int> (pos);
        const float t = static_cast<float> (pos - static_cast<double> (i));
        if (i >= table.size)
            i -= table.size;
        const float* p = m_tables.data() + table.offset + i;
        return dsp::hermite4 (p[-1], p[0], p[1], p[2], t);
    }

    float RingModule::shapeAt (int shape, double phase, int level, float t) const
    {
        if (shape == c_sine)
            return readTable (m_sineTable, phase);

        const auto& levels = m_levels[static_cast<size_t> (shape - 1)];
        const float a = readTable (levels[static_cast<size_t> (level)], phase);
        if (t <= 0.0f)
            return a;
        const float b = readTable (levels[static_cast<size_t> (level + 1)], phase);
        return a + t * (b - a);
    }

    float RingModule::carrierAt (double phase, int level, float t) const
    {
        const float to = shapeMakeup (static_cast<RingCarrier> (m_s.shapeTo)) * shapeAt (m_s.shapeTo, phase, level, t);
        if (m_s.shapeMix >= 1.0f)
            return to;
        const float from = shapeMakeup (static_cast<RingCarrier> (m_s.shapeFrom)) * shapeAt (m_s.shapeFrom, phase, level, t);
        return from + m_s.shapeMix * (to - from);
    }

    float RingModule::carrierValue (RingCarrier carrier, double phase, double fcHz) const
    {
        const double lp = std::clamp (std::log2 (std::max (fcHz, 1.0e-3)), m_logMin, m_logMax) - m_logBase;
        int level = static_cast<int> (std::floor (lp));
        float t = static_cast<float> (lp - static_cast<double> (level));
        if (level >= m_numLevels - 1)
        {
            level = m_numLevels - 2;
            t = 1.0f;
        }
        level = std::max (level, 0);
        double ph = phase - std::floor (phase);
        if (ph >= 1.0)
            ph = 0.0;
        return shapeAt (static_cast<int> (carrier), ph, level, t);
    }

    float RingModule::combOutput (Channel& ch, int comb, float y)
    {
        const auto b = static_cast<size_t> (comb);
        const auto& spec = c_bodies[b];
        auto& c = ch.combs[b];
        const float back = c.lowpass.processLowpass (c.line.readHermite (m_combRead[b]));
        // finiteOrZero: a near-FLT_MAX input would otherwise overflow the loop sum to inf.
        const float z = dsp::flushTiny (dsp::finiteOrZero (y + spec.feedback * back));
        c.line.push (z);
        return (1.0f - spec.mix) * y + spec.mix * m_combNorm[b] * z;
    }

    void RingModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice)
    {
        n = std::min (n, m_maxBlock);
        m_fade.captureDry (l, r, n, stereo);

        if (! m_fade.isActive())
        {
            // Fully Off: keep the carrier control and the phase running (cheap), no audio DSP. The
            // phase advances exactly as on the active path, so the output never depends on where
            // a host block boundary fell relative to the end of a fade.
            snapMixState();
            for (int i = 0; i < n; ++i)
            {
                advanceControl (i, voice, inputPeak (l, r, i, stereo));
                const double inc = m_s.fcNow * m_invFsOs;
                m_s.phase = wrapPhase (m_s.phase + inc);
                if (m_oversample)
                    m_s.phase = wrapPhase (m_s.phase + inc);
            }
            m_fade.outputDry (l, r, n, stereo);
            return;
        }

        if (m_fade.consumeRestart())
        {
            resetWet();
            snapMixState();
        }

        applySettingChanges();

        const float* const dry[2] = { m_fade.dryLeft(), m_fade.dryRight() };
        float* const io[2] = { l, r };
        const int numChannels = stereo ? 2 : 1;
        const float amountTarget = m_settings.amount;
        const float modeTarget = m_settings.mode == RingMode::am ? 1.0f : 0.0f;

        for (int i = 0; i < n; ++i)
        {
            advanceControl (i, voice, inputPeak (l, r, i, stereo));

            const double inc = m_s.fcNow * m_invFsOs;
            const double lp = m_s.logNow - m_logBase;
            int level = static_cast<int> (std::floor (lp));
            float t = static_cast<float> (lp - static_cast<double> (level));
            if (level >= m_numLevels - 1)
            {
                level = m_numLevels - 2;
                t = 1.0f;
            }
            else if (level < 0)
            {
                level = 0;
                t = 0.0f;
            }

            const float c0 = carrierAt (m_s.phase, level, t);
            m_s.phase = wrapPhase (m_s.phase + inc);
            float c1 = 0.0f;
            if (m_oversample)
            {
                c1 = carrierAt (m_s.phase, level, t);
                m_s.phase = wrapPhase (m_s.phase + inc);
            }

            // Mix laws (the carrier crossfade is inside carrierAt).
            const float kS = m_s.shapeMix >= 1.0f
                                 ? shapeMakeup (static_cast<RingCarrier> (m_s.shapeTo))
                                 : shapeMakeup (static_cast<RingCarrier> (m_s.shapeFrom))
                                       + m_s.shapeMix * (shapeMakeup (static_cast<RingCarrier> (m_s.shapeTo))
                                                         - shapeMakeup (static_cast<RingCarrier> (m_s.shapeFrom)));
            if (m_s.shapeMix < 1.0f)
                m_s.shapeMix = std::min (1.0f, m_s.shapeMix + m_shapeStep);

            m_s.amount += m_amountCoeff * (amountTarget - m_s.amount);
            if (m_s.modeMix < modeTarget)
                m_s.modeMix = std::min (modeTarget, m_s.modeMix + m_modeStep);
            else if (m_s.modeMix > modeTarget)
                m_s.modeMix = std::max (modeTarget, m_s.modeMix - m_modeStep);

            const float a = m_s.amount;
            float dryGain = std::sqrt (1.0f - a);
            float wetGain = std::sqrt (a);
            const float mw = dsp::smoothstep01 (m_s.modeMix);
            if (mw > 0.0f)
            {
                const auto aAm = static_cast<float> (laws::ringAmAmount (static_cast<double> (a)));
                const float kAm = amMakeupFor (aAm, kS);
                const float amDry = kAm * (1.0f - 0.5f * aAm);
                const float amWet = kAm * 0.5f * aAm / kS;
                dryGain += mw * (amDry - dryGain);
                wetGain += mw * (amWet - wetGain);
            }

            const bool bodyFading = m_s.bodyMix < 1.0f;
            const float wb = dsp::smoothstep01 (m_s.bodyMix);

            for (int c = 0; c < numChannels; ++c)
            {
                auto& ch = m_channels[static_cast<size_t> (c)];
                const float x = ch.dcIn.processHighpass (sanitizeInput (io[c][i]));
                float p;
                if (m_oversample)
                {
                    float u0, u1;
                    ch.os.upsample (x, u0, u1);
                    p = ch.os.downsample (u0 * c0, u1 * c1);
                }
                else
                {
                    p = ch.direct.processFixed (x) * c0;
                }
                p = ch.dcOut.processHighpass (p);

                // Both combs always run, so a Body change crossfades between warm combs (a comb
                // that started empty would put a step into the fade when its first echo arrives).
                const float y = dryGain * dry[c][i] + wetGain * p;
                const float box = combOutput (ch, 0, y);
                const float can = combOutput (ch, 1, y);
                const float to = bodyPick (m_s.bodyTo, y, box, can);
                if (bodyFading)
                {
                    const float from = bodyPick (m_s.bodyFrom, y, box, can);
                    io[c][i] = from + wb * (to - from);
                }
                else
                {
                    io[c][i] = to;
                }
            }

            if (bodyFading)
                m_s.bodyMix = std::min (1.0f, m_s.bodyMix + m_bodyStep);
        }

        m_fade.apply (l, r, n, stereo);
    }

    void RingModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<RingModule> (other);
        for (size_t c = 0; c < m_channels.size(); ++c)
        {
            auto& ch = m_channels[c];
            const auto& oc = o.m_channels[c];
            ch.os.copyStateFrom (oc.os);
            ch.direct.copyStateFrom (oc.direct);
            ch.dcIn.copyStateFrom (oc.dcIn);
            ch.dcOut.copyStateFrom (oc.dcOut);
            for (size_t b = 0; b < ch.combs.size(); ++b)
            {
                ch.combs[b].line.copyStateFrom (oc.combs[b].line);
                ch.combs[b].lowpass.copyStateFrom (oc.combs[b].lowpass);
            }
        }
        m_settings = o.m_settings;
        m_s = o.m_s;
        m_pitchGate = o.m_pitchGate;
        m_fade.copyStateFrom (o.m_fade);
    }

    void RingModule::copyLeftStateToRight()
    {
        auto& left = m_channels[0];
        auto& right = m_channels[1];
        right.os.copyStateFrom (left.os);
        right.direct.copyStateFrom (left.direct);
        right.dcIn.copyStateFrom (left.dcIn);
        right.dcOut.copyStateFrom (left.dcOut);
        for (size_t b = 0; b < right.combs.size(); ++b)
        {
            right.combs[b].line.copyStateFrom (left.combs[b].line);
            right.combs[b].lowpass.copyStateFrom (left.combs[b].lowpass);
        }
        m_fade.copyLeftStateToRight();
    }
}

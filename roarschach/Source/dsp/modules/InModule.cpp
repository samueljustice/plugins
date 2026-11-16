#include "InModule.h"

#include "../MonsterFeatures.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_trimSeconds = 0.020 / 4.6;          // settles within 20 ms
        constexpr double c_blendSeconds = 0.020;
        constexpr double c_inputClamp = 32.0;                    // +30 dBFS: no legitimate sample reaches it

        // Loudness weighting (K-style): the level every decision below is made on.
        constexpr double c_weightHpHz = 80.0;
        constexpr double c_weightShelfHz = 1500.0, c_weightShelfDb = 4.0;

        // Detectors.
        constexpr double c_fastLevelSeconds = 0.020;           // F
        constexpr double c_sustainSeconds = 0.100;             // sus: the minimum of F over this
        constexpr double c_s100Seconds = 0.100;                // the 100 ms mean square (temporary fall)
        constexpr double c_spreadSeconds = 0.400;              // the three range windows
        constexpr double c_silentDb = -85.0;                   // digital silence: the floor is frozen
        constexpr double c_floorStartDb = -40.0, c_floorMinDb = -90.0, c_floorMaxDb = -40.0;
        constexpr double c_floorBelowTopDb = 20.0;             // the floor stays this far under the pass's loudest sustained level
        constexpr double c_floorFallSeconds = 0.100;
        constexpr double c_floorRiseDbPerSecond = 1.0, c_floorSteadyRiseDbPerSecond = 30.0;
        constexpr double c_steadySpreadDb = 4.0;
        constexpr double c_aboveFloorDb = 10.0;
        constexpr double c_settlePitchedSeconds = 0.160, c_settleUnpitchedSeconds = 0.060;
        constexpr double c_pitchedSpreadDb = 4.0;
        constexpr double c_unpitchedSpreadDb = 6.0, c_unpitchedAboveFloorDb = 15.0;
        constexpr double c_presenceDb = -25.0;
        constexpr double c_safetyDb = -50.0;
        constexpr float c_voicedConfidence = 0.6f;

        // The learn.
        constexpr double c_firstPitchedSeconds = 0.150, c_firstUnpitchedSeconds = 0.600;
        constexpr double c_pitchedEvidenceSeconds = 0.050;     // pitched sound that shows the take is a voice
        constexpr double c_unpitchedMaxGainDb = 12.0;          // the most gain a take heard only as unpitched sound gets
        constexpr double c_expireSeconds = 2.0;
        constexpr double c_supportSeconds = 0.5;
        constexpr double c_completeSeconds = 4.0;
        constexpr double c_matchBelowDb = 10.0, c_matchAboveDb = 3.0, c_onTargetDb = 1.0;

        // The temporary fall.
        constexpr double c_overDb = 12.0, c_overSeconds = 0.150;
        constexpr double c_fallMarginDb = 6.0, c_fallDbPerSecond = 80.0, c_recoverDbPerSecond = 6.0;

        // The gain glide.
        constexpr double c_firstGlideSeconds = 0.080, c_fallGlideSeconds = 0.030, c_riseGlideSeconds = 0.300;

        constexpr double c_jawAttackSeconds = 0.005, c_jawReleaseSeconds = 0.120, c_jawFastReleaseSeconds = 0.030;
        constexpr float c_jawFloorDb = -48.0f, c_jawFullDb = -6.0f;
        constexpr double c_meterReleaseSeconds = 0.300;

        constexpr double c_dbToLog = 0.11512925464970228;     // ln (10) / 20

        double powerDb (double ms) { return 10.0 * std::log10 (ms + 1.0e-30); }
    }

    // ---------------------------------------------------------------------------------------------
    // WindowRange

    void InModule::WindowRange::prepare (int maxFrames)
    {
        m_maxWindow = std::max (1, maxFrames);
        const int capacity = dsp::nextPowerOfTwo (m_maxWindow + 1);
        for (auto* d : { &m_max, &m_min })
        {
            d->values.assign (static_cast<size_t> (capacity), 0.0);
            d->stamps.assign (static_cast<size_t> (capacity), 0u);
        }
        m_mask = static_cast<std::uint32_t> (capacity - 1);
        m_window = std::clamp (m_window, 1, m_maxWindow);
        reset();
    }

    void InModule::WindowRange::setWindow (int frames)
    {
        m_window = std::clamp (frames, 1, m_maxWindow);
    }

    void InModule::WindowRange::reset()
    {
        m_max.head = m_max.tail = 0;
        m_min.head = m_min.tail = 0;
        m_clock = 0;
    }

    void InModule::WindowRange::expire (Deque& d) const
    {
        const auto window = static_cast<std::uint32_t> (m_window);
        while (d.head != d.tail && m_clock - d.stamps[static_cast<size_t> (d.head & m_mask)] >= window)
            ++d.head;
    }

    void InModule::WindowRange::insert (Deque& d, double x, bool isMax) const
    {
        while (d.tail != d.head)
        {
            const double back = d.values[static_cast<size_t> ((d.tail - 1u) & m_mask)];
            if (isMax ? (back > x) : (back < x))
                break;
            --d.tail;
        }
        d.values[static_cast<size_t> (d.tail & m_mask)] = x;
        d.stamps[static_cast<size_t> (d.tail & m_mask)] = m_clock;
        ++d.tail;
    }

    double InModule::WindowRange::push (double x, bool valid)
    {
        // The frame is stamped with the clock; entries m_window or more frames old leave.
        ++m_clock;
        expire (m_max);
        expire (m_min);
        if (valid && std::isfinite (x))
        {
            insert (m_max, x, true);
            insert (m_min, x, false);
        }
        if (m_max.head == m_max.tail)
            return 0.0;
        return m_max.values[static_cast<size_t> (m_max.head & m_mask)] - m_min.values[static_cast<size_t> (m_min.head & m_mask)];
    }

    void InModule::WindowRange::copyStateFrom (const WindowRange& o)
    {
        for (auto [dst, src] : { std::pair<Deque*, const Deque*> { &m_max, &o.m_max }, std::pair<Deque*, const Deque*> { &m_min, &o.m_min } })
        {
            const size_t n = std::min (dst->values.size(), src->values.size());
            std::copy (src->values.begin(), src->values.begin() + static_cast<std::ptrdiff_t> (n), dst->values.begin());
            std::copy (src->stamps.begin(), src->stamps.begin() + static_cast<std::ptrdiff_t> (n), dst->stamps.begin());
            dst->head = src->head;
            dst->tail = src->tail;
        }
        m_clock = o.m_clock;
        m_window = o.m_window;
    }

    // ---------------------------------------------------------------------------------------------
    // InModule

    void InModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        const double frameRate = m_fs / static_cast<double> (c_voiceFrameSize);
        m_dt = 1.0 / frameRate;

        m_trimCoeff = dsp::onePoleCoeff (c_trimSeconds, m_fs);
        m_blendStep = 1.0 / std::max (1.0, c_blendSeconds * frameRate);
        m_fastLevelCoeff = dsp::onePoleCoeff (c_fastLevelSeconds, m_fs);
        m_s100Coeff = dsp::onePoleCoeff (c_s100Seconds, frameRate);
        m_floorFallCoeff = dsp::onePoleCoeff (c_floorFallSeconds, frameRate);
        m_firstCoeff = dsp::onePoleCoeff (c_firstGlideSeconds, frameRate);
        m_fallCoeff = dsp::onePoleCoeff (c_fallGlideSeconds, frameRate);
        m_riseCoeff = dsp::onePoleCoeff (c_riseGlideSeconds, frameRate);
        m_jawAttack = static_cast<float> (dsp::onePoleCoeff (c_jawAttackSeconds, m_fs));
        m_jawRelease = static_cast<float> (dsp::onePoleCoeff (c_jawReleaseSeconds, m_fs));
        m_jawFastRelease = static_cast<float> (dsp::onePoleCoeff (c_jawFastReleaseSeconds, m_fs));
        m_meterRelease = static_cast<float> (dsp::poleFromTau (c_meterReleaseSeconds, m_fs));

        m_weightHp.setCoeffs (Biquad::highpass (m_fs, c_weightHpHz, Biquad::c_butterworthQ));
        m_weightShelf.setCoeffs (Biquad::highShelf (m_fs, c_weightShelfHz, Biquad::c_butterworthQ, c_weightShelfDb));

        const int susFrames = std::max (1, static_cast<int> (std::lround (c_sustainSeconds * frameRate)));
        m_sus.prepare (susFrames);
        m_sus.setWindow (susFrames);
        const int spreadFrames = std::max (1, static_cast<int> (std::lround (c_spreadSeconds * frameRate)));
        for (auto* w : { &m_spreadRaw, &m_spreadU, &m_spreadV })
        {
            w->prepare (spreadFrames);
            w->setWindow (spreadFrames);
        }

        const auto traceSize = static_cast<size_t> (std::max (1, maxBlock));
        m_gainTrace.assign (traceSize, 1.0f);
        m_activeTrace.assign (traceSize, 0.0f);

        m_detector.prepare (m_fs);
        // The detector answers for the stretch it has just analysed, about 40 ms back: the stream
        // says so, and the delays behind a module with latency (PITCH's 30 ms) use that up
        // instead of adding to it.
        m_stream.setLateness (m_detector.getReactionSamples());
        m_stream.prepare (maxBlock);
        reset();
    }

    void InModule::clearPass()
    {
        auto& s = m_s;
        s.hist.fill (0.0);
        s.confidentSeconds = s.pitchedSeconds = s.unpitchedSeconds = s.idleSeconds = 0.0;
        s.floorDb = c_floorStartDb;
        s.topDb = -150.0;
        s.runSeconds = 0.0;
        s.s100 = 0.0;
        s.overSeconds = 0.0;
        s.tempLoudEndDb = -150.0;
        m_sus.reset();
        m_spreadRaw.reset();
        m_spreadU.reset();
        m_spreadV.reset();
    }

    double InModule::targetDb() const
    {
        if (! m_s.learnt)
            return 0.0;
        const double le = std::max (m_s.loudEndDb, m_s.tempLoudEndDb);
        // Unpitched sound alone (a rasp, or wind and breathy noise before a line) is lifted by at
        // most 12 dB until pitched sound has been heard.
        const double maxGain = m_s.pitchedHeard ? c_maxGainDb : std::min (c_maxGainDb, c_unpitchedMaxGainDb);
        return std::clamp (c_targetDb - le, c_minGainDb, maxGain);
    }

    void InModule::setGainAtOnce()
    {
        auto& s = m_s;
        s.autoGainDb = targetDb();
        s.first = false;
        const double autoDb = s.autoGainDb + laws::inAutoTargetOffsetDb (s.trimDb) - s.trimDb;
        s.gainPrev = s.gainNext = std::exp (dsp::smoothstep01 (s.autoBlend) * autoDb * c_dbToLog);
    }

    void InModule::reset()
    {
        m_weightHp.reset();
        m_weightShelf.reset();
        m_detector.reset();
        m_stream.reset();

        const State keep = m_s;
        m_s = State {};
        m_s.trimTargetDb = m_s.trimDb = keep.trimTargetDb;
        m_s.autoOn = keep.autoOn;
        m_s.autoBlend = keep.autoOn ? 1.0 : 0.0;
        m_s.snap = true;
        m_s.learnt = keep.learnt;
        m_s.loudEndDb = keep.loudEndDb;
        m_s.evidence = keep.evidence;
        m_s.pitchedHeard = keep.pitchedHeard;
        clearPass();
        setGainAtOnce();
        std::fill (m_gainTrace.begin(), m_gainTrace.end(), getCurrentTotalGain());
        std::fill (m_activeTrace.begin(), m_activeTrace.end(), 0.0f);
    }

    void InModule::beginPass()
    {
        clearPass();
    }

    void InModule::clearLearning()
    {
        m_s.learnt = false;
        m_s.loudEndDb = -150.0;
        m_s.evidence = 0.0;
        m_s.pitchedHeard = false;
        m_s.autoGainDb = 0.0;
        m_s.first = false;
        m_s.matchActive = false;
        clearPass();
    }

    void InModule::restoreLearning (bool learnt, double levelDb, double evidenceSeconds, bool pitchedHeard)
    {
        const bool ok = learnt && std::isfinite (levelDb) && std::isfinite (evidenceSeconds);
        m_s.learnt = ok;
        m_s.pitchedHeard = ok && pitchedHeard;
        m_s.loudEndDb = ok ? std::clamp (levelDb, -70.0, 12.0) : -150.0;
        m_s.evidence = ok ? std::clamp (evidenceSeconds, 0.0, c_completeSeconds) : 0.0;
        m_s.matchActive = false;
        clearPass();
        setGainAtOnce();
    }

    void InModule::setParams (const InSettings& s)
    {
        m_s.trimTargetDb = std::clamp (static_cast<double> (dsp::finiteOrZero (s.trimDb)), -24.0, 24.0);
        // Off to On: "learn this take again".
        if (s.autoLevel && ! m_s.autoOn)
            clearLearning();
        m_s.autoOn = s.autoLevel;
        if (m_s.snap)
        {
            m_s.trimDb = m_s.trimTargetDb;
            m_s.autoBlend = m_s.autoOn ? 1.0 : 0.0;
            m_s.snap = false;
            setGainAtOnce();
        }
    }

    float InModule::getCurrentTotalGain() const
    {
        return static_cast<float> (std::exp (m_s.trimDb * c_dbToLog) * m_s.gainNext);
    }

    float InModule::getJaw() const
    {
        return jawOf (m_s.jawEnv);
    }

    float InModule::getJawFast() const
    {
        return jawOf (m_s.jawFastEnv);
    }

    float InModule::jawOf (float env)
    {
        const float db = 20.0f * std::log10 (std::max (env, 1.0e-6f));
        return std::clamp ((db - c_jawFloorDb) / (c_jawFullDb - c_jawFloorDb), 0.0f, 1.0f);
    }

    double InModule::loudEndStat (double need) const
    {
        // The level above which `need` seconds of this pass's confident sound lie, interpolated
        // within its 1 dB bin.
        double cum = 0.0;
        for (int b = c_histBins - 1; b >= 0; --b)
        {
            const double h = m_s.hist[static_cast<size_t> (b)];
            if (h > 0.0 && cum + h >= need)
                return c_histLowDb + b + 1.0 - (need - cum) / h;
            cum += h;
        }
        return c_histLowDb;
    }

    void InModule::endFrame()
    {
        auto& s = m_s;
        const double dt = m_dt;
        const double frameMs = s.frameSum / static_cast<double> (c_voiceFrameSize);
        s.frameSum = 0.0;
        s.framePhase = 0;

        // Detectors (N still holds the previous frame's floor here).
        const double f = 10.0 * std::log10 (s.fastMs + 1.0e-20);
        s.s100 = dsp::flushTiny (s.s100 + m_s100Coeff * (frameMs - s.s100));
        const double sus = static_cast<double> (m_sus.process (static_cast<float> (f)));
        const bool silent = f < c_silentDb;
        const bool above = ! silent && f > s.floorDb + c_aboveFloorDb;
        s.runSeconds = above ? s.runSeconds + dt : 0.0;

        const double spreadRaw = m_spreadRaw.push (f, ! silent);
        const double spreadU = m_spreadU.push (f, s.runSeconds >= c_settleUnpitchedSeconds);
        const double spreadV = m_spreadV.push (sus, s.runSeconds >= c_settlePitchedSeconds);

        const VoiceInfo& vi = m_detector.getVoiceInfo();
        const bool v = vi.voiced && vi.confidence >= c_voicedConfidence && above && f > c_safetyDb
                    && (spreadV >= c_pitchedSpreadDb || f > c_presenceDb);
        const bool steady = spreadRaw < c_steadySpreadDb && ! v;

        if (! silent)
        {
            if (f < s.floorDb)
                s.floorDb += m_floorFallCoeff * (f - s.floorDb);
            else
                s.floorDb = std::min (f, s.floorDb + (steady ? c_floorSteadyRiseDbPerSecond : c_floorRiseDbPerSecond) * dt);
            s.topDb = std::max (s.topDb, sus);
            s.floorDb = std::min (std::max (s.floorDb, c_floorMinDb), std::min (c_floorMaxDb, s.topDb - c_floorBelowTopDb));
        }

        const bool u = ! v && ((above && spreadU >= c_unpitchedSpreadDb && sus > s.floorDb + c_unpitchedAboveFloorDb && sus > c_safetyDb)
                               || sus > c_presenceDb);
        const bool confident = v || u;
        const double lvl = v ? f : sus;

        // The loud end: a supported level of this pass.
        if (confident)
        {
            s.idleSeconds = 0.0;
            s.confidentSeconds += dt;
            const int bin = std::clamp (static_cast<int> (std::floor (lvl - c_histLowDb)), 0, c_histBins - 1);
            s.hist[static_cast<size_t> (bin)] += dt;
            (v ? s.pitchedSeconds : s.unpitchedSeconds) += dt;
            if (s.pitchedSeconds >= c_pitchedEvidenceSeconds)
                s.pitchedHeard = true;

            if (s.learnt || s.pitchedSeconds / c_firstPitchedSeconds + s.unpitchedSeconds / c_firstUnpitchedSeconds >= 1.0)
            {
                const double a = s.confidentSeconds;
                const double need = std::min (c_supportSeconds, std::max (a, s.evidence) * (c_supportSeconds / c_completeSeconds));
                if (a >= need)
                {
                    const double c = loudEndStat (need);
                    if (! s.learnt)
                    {
                        s.learnt = true;
                        s.loudEndDb = c;
                        s.first = true;
                    }
                    else if (a >= s.evidence && a < c_completeSeconds)
                        s.loudEndDb = c;                              // provisional: either way
                    else
                        s.loudEndDb = std::max (s.loudEndDb, c);      // complete: up only
                }
                if (a > s.evidence)
                    s.evidence = std::min (a, c_completeSeconds);
            }
        }
        else
        {
            s.idleSeconds += dt;
            if (! s.learnt && s.idleSeconds > c_expireSeconds)
            {
                s.confidentSeconds = s.pitchedSeconds = s.unpitchedSeconds = 0.0;
                s.hist.fill (0.0);
            }
        }

        // The temporary fall for a sound far over the loud end.
        if (s.learnt)
        {
            const double le = std::max (s.loudEndDb, s.tempLoudEndDb);
            const double s100d = powerDb (s.s100);
            s.overSeconds = (s100d > le + c_overDb && ! silent) ? std::min (s.overSeconds + dt, 2.0 * c_overSeconds)
                                                                : std::max (0.0, s.overSeconds - dt);
            if (s.overSeconds >= c_overSeconds && s100d - c_fallMarginDb > le)
                s.tempLoudEndDb = std::min (s100d - c_fallMarginDb, le + c_fallDbPerSecond * dt);
            else if (s100d - c_fallMarginDb < s.tempLoudEndDb)
                s.tempLoudEndDb -= c_recoverDbPerSecond * dt;
        }

        // The gain.
        const double target = targetDb();
        if (s.first)
        {
            s.autoGainDb += m_firstCoeff * (target - s.autoGainDb);
            if (std::abs (target - s.autoGainDb) < c_onTargetDb)
                s.first = false;
        }
        else
        {
            s.autoGainDb += (target < s.autoGainDb ? m_fallCoeff : m_riseCoeff) * (target - s.autoGainDb);
        }

        const double le = std::max (s.loudEndDb, s.tempLoudEndDb);
        s.matchActive = s.learnt && confident && lvl > le - c_matchBelowDb && lvl < le + c_matchAboveDb
                     && std::abs (target - s.autoGainDb) < c_onTargetDb;

        const double blendTarget = s.autoOn ? 1.0 : 0.0;
        s.autoBlend = blendTarget > s.autoBlend ? std::min (blendTarget, s.autoBlend + m_blendStep)
                                                : std::max (blendTarget, s.autoBlend - m_blendStep);

        s.gainPrev = s.gainNext;
        // With Auto Level on, Input is replaced by its target offset (the level was learnt before Input).
        const double autoDb = s.autoGainDb + laws::inAutoTargetOffsetDb (s.trimDb) - s.trimDb;
        s.gainNext = std::exp (dsp::smoothstep01 (s.autoBlend) * autoDb * c_dbToLog);
    }

    void InModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& /*voice*/)
    {
        auto& s = m_s;
        n = std::clamp (n, 0, static_cast<int> (m_gainTrace.size()));
        m_stream.beginChunk (n);

        const double trimDiff = s.trimTargetDb - s.trimDb;
        const bool trimMoving = std::abs (trimDiff) > 1.0e-7;
        if (! trimMoving)
            s.trimDb = s.trimTargetDb;
        double trimGain = std::exp (s.trimDb * c_dbToLog);

        constexpr double invFrame = 1.0 / static_cast<double> (c_voiceFrameSize);
        float jaw = s.jawEnv;
        float jawFast = s.jawFastEnv;
        float meter = s.inputLevel;

        for (int i = 0; i < n; ++i)
        {
            double x = static_cast<double> (dsp::finiteOrZero (l[i]));
            if (stereo)
                x = 0.5 * (x + static_cast<double> (dsp::finiteOrZero (r[i])));
            // One absurd sample cannot reach the modules (several have no clamp of their own).
            x = std::clamp (x, -c_inputClamp, c_inputClamp);

            if (trimMoving)
            {
                s.trimDb += m_trimCoeff * (s.trimTargetDb - s.trimDb);
                trimGain = std::exp (s.trimDb * c_dbToLog);
            }
            const double y = x * trimGain;
            meter = std::max (static_cast<float> (std::abs (y)), meter * m_meterRelease);

            const double w = m_weightShelf.process (m_weightHp.process (x));   // measured before Input
            const double w2 = w * w;
            s.frameSum += w2;
            s.fastMs = dsp::flushTiny (s.fastMs + m_fastLevelCoeff * (w2 - s.fastMs));

            const double t = static_cast<double> (s.framePhase + 1) * invFrame;
            const double g = s.gainPrev + (s.gainNext - s.gainPrev) * t;
            const auto out = static_cast<float> (y * g);
            l[i] = out;
            m_gainTrace[static_cast<size_t> (i)] = static_cast<float> (trimGain * g);
            m_activeTrace[static_cast<size_t> (i)] = s.matchActive ? 1.0f : 0.0f;

            const float a = std::abs (out);
            jaw += (a > jaw ? m_jawAttack : m_jawRelease) * (a - jaw);
            jawFast += (a > jawFast ? m_jawAttack : m_jawFastRelease) * (a - jawFast);

            m_detector.pushSample (out);
            // The frame is written once, at its first sample on the global grid (a frame that
            // began in the previous chunk keeps its value: beginChunk copies it), so where the
            // host's blocks end cannot change what the stream says.
            if (m_stream.isFrameStart (i))
            {
                const VoiceInfo& v = m_detector.getVoiceInfo();
                m_stream.frameForSample (i) = VoiceFrame { v.f0Hz, v.confidence, v.voiced };
            }

            if (++s.framePhase >= c_voiceFrameSize)
                endFrame();
        }

        s.jawEnv = std::isfinite (jaw) ? dsp::flushTiny (jaw) : 0.0f;
        s.jawFastEnv = std::isfinite (jawFast) ? dsp::flushTiny (jawFast) : 0.0f;
        s.inputLevel = std::isfinite (meter) ? dsp::flushTiny (meter) : 0.0f;
    }

    void InModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<InModule> (other);
        m_weightHp.copyStateFrom (o.m_weightHp);
        m_weightShelf.copyStateFrom (o.m_weightShelf);
        m_detector = o.m_detector;   // same sizes: the vectors copy in place
        m_stream = o.m_stream;
        m_sus.copyStateFrom (o.m_sus);
        m_spreadRaw.copyStateFrom (o.m_spreadRaw);
        m_spreadU.copyStateFrom (o.m_spreadU);
        m_spreadV.copyStateFrom (o.m_spreadV);
        std::copy (o.m_gainTrace.begin(), o.m_gainTrace.begin() + static_cast<std::ptrdiff_t> (std::min (m_gainTrace.size(), o.m_gainTrace.size())), m_gainTrace.begin());
        std::copy (o.m_activeTrace.begin(), o.m_activeTrace.begin() + static_cast<std::ptrdiff_t> (std::min (m_activeTrace.size(), o.m_activeTrace.size())), m_activeTrace.begin());
        m_s = o.m_s;
    }

    void InModule::publishFeatures (MonsterFeatures& features) const
    {
        const VoiceInfo& v = m_detector.getVoiceInfo();
        features.inputLevel.store (std::min (m_s.inputLevel, 1.0f), std::memory_order_relaxed);
        features.jaw.store (getJaw(), std::memory_order_relaxed);
        features.jawFast.store (getJawFast(), std::memory_order_relaxed);
        features.f0Hz.store (v.f0Hz, std::memory_order_relaxed);
        features.voiced.store (v.voiced ? 1.0f : 0.0f, std::memory_order_relaxed);
    }
}

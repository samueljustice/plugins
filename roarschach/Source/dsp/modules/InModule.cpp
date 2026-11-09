#include "InModule.h"

#include "../MonsterFeatures.h"

#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_trimSeconds = 0.020 / 4.6;          // settles within 20 ms
        constexpr double c_blendSeconds = 0.020;

        // Loudness weighting (K-style) and its calibration: the reference voice reads its plain RMS.
        constexpr double c_weightHpHz = 80.0;
        constexpr double c_weightShelfHz = 1500.0, c_weightShelfDb = 4.0;
        constexpr double c_weightCalibrationDb = 0.0;

        constexpr double c_meterSeconds = 0.400;
        constexpr double c_riseSeconds = 1.5, c_fallSeconds = 4.0;
        constexpr double c_quickSeconds = 0.25;                // glide while first learning
        constexpr double c_quickLearnSeconds = 1.0;            // ... for this much voicing
        constexpr double c_meanLearnSeconds = 0.400;           // running mean until the meter has filled

        constexpr double c_fastLevelSeconds = 0.020;
        constexpr double c_floorFallSeconds = 0.100;
        constexpr double c_floorRiseDbPerSecond = 3.0;
        constexpr double c_floorMinDb = -100.0, c_floorMaxDb = -20.0;
        constexpr double c_learnAboveFloorDb = 12.0;
        constexpr double c_unvoicedWithinDb = 6.0;             // unvoiced frames teach only near the voice level
        constexpr double c_silenceDb = -100.0;                 // a frame this quiet never teaches

        constexpr double c_jawAttackSeconds = 0.005, c_jawReleaseSeconds = 0.120, c_jawFastReleaseSeconds = 0.030;
        constexpr float c_jawFloorDb = -48.0f, c_jawFullDb = -6.0f;
        constexpr double c_meterReleaseSeconds = 0.300;

        constexpr double c_dbToLog = 0.11512925464970228;     // ln (10) / 20
    }

    void InModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        const double frameRate = m_fs / static_cast<double> (c_voiceFrameSize);

        m_trimCoeff = dsp::onePoleCoeff (c_trimSeconds, m_fs);
        m_blendStep = 1.0 / std::max (1.0, c_blendSeconds * frameRate);
        m_meterCoeff = dsp::onePoleCoeff (c_meterSeconds, frameRate);
        m_riseCoeff = dsp::onePoleCoeff (c_riseSeconds, frameRate);
        m_fallCoeff = dsp::onePoleCoeff (c_fallSeconds, frameRate);
        m_quickCoeff = dsp::onePoleCoeff (c_quickSeconds, frameRate);
        m_quickFrames = std::max (1, static_cast<int> (std::lround (c_quickLearnSeconds * frameRate)));
        m_meanFrames = std::max (1, static_cast<int> (std::lround (c_meanLearnSeconds * frameRate)));
        m_fastLevelCoeff = dsp::onePoleCoeff (c_fastLevelSeconds, m_fs);
        m_floorFallCoeff = dsp::onePoleCoeff (c_floorFallSeconds, frameRate);
        m_floorRiseDb = c_floorRiseDbPerSecond / frameRate;
        m_jawAttack = static_cast<float> (dsp::onePoleCoeff (c_jawAttackSeconds, m_fs));
        m_jawRelease = static_cast<float> (dsp::onePoleCoeff (c_jawReleaseSeconds, m_fs));
        m_jawFastRelease = static_cast<float> (dsp::onePoleCoeff (c_jawFastReleaseSeconds, m_fs));
        m_meterRelease = static_cast<float> (dsp::poleFromTau (c_meterReleaseSeconds, m_fs));

        m_weightHp.setCoeffs (Biquad::highpass (m_fs, c_weightHpHz, Biquad::c_butterworthQ));
        m_weightShelf.setCoeffs (Biquad::highShelf (m_fs, c_weightShelfHz, Biquad::c_butterworthQ, c_weightShelfDb));

        m_detector.prepare (m_fs);
        m_stream.prepare (maxBlock);
        reset();
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
    }

    void InModule::setParams (const InSettings& s)
    {
        m_s.trimTargetDb = std::clamp (static_cast<double> (dsp::finiteOrZero (s.trimDb)), -24.0, 24.0);
        m_s.autoOn = s.autoLevel;
        if (m_s.snap)
        {
            m_s.trimDb = m_s.trimTargetDb;
            m_s.autoBlend = m_s.autoOn ? 1.0 : 0.0;
            m_s.snap = false;
        }
    }

    float InModule::getLearnedLevelDb() const
    {
        if (m_s.learnedFrames == 0)
            return -150.0f;
        return static_cast<float> (10.0 * std::log10 (m_s.meterMs + 1.0e-30) + c_weightCalibrationDb);
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

    void InModule::endFrame()
    {
        auto& s = m_s;
        const double frameMs = s.frameSum / static_cast<double> (c_voiceFrameSize);
        s.frameSum = 0.0;
        s.framePhase = 0;

        // Floor: falls quickly to quiet passages, rises slowly through speech.
        const double fastDb = 10.0 * std::log10 (s.fastMs + 1.0e-20);
        if (fastDb < s.floorDb)
            s.floorDb += m_floorFallCoeff * (fastDb - s.floorDb);
        else
            s.floorDb += m_floorRiseDb;
        s.floorDb = std::clamp (s.floorDb, c_floorMinDb, c_floorMaxDb);

        // Learn while voiced, or on loud unvoiced material the detector misses (a shout, a growl):
        // well above the floor and near the voice level learnt so far. Consonants, breaths and
        // room tone never teach, so they do not drag the voice level down.
        const double frameDb = 10.0 * std::log10 (frameMs + 1.0e-30);
        const bool voiced = m_detector.getVoiceInfo().voiced;
        const double learnedDb = 10.0 * std::log10 (s.meterMs + 1.0e-30);
        const bool loud = fastDb > s.floorDb + c_learnAboveFloorDb
                       && (s.learnedFrames == 0 || fastDb > learnedDb - c_unvoicedWithinDb);
        const bool learn = frameDb > c_silenceDb && (voiced || loud);

        if (learn)
        {
            ++s.learnedFrames;
            const double k = s.learnedFrames < m_meanFrames ? 1.0 / static_cast<double> (s.learnedFrames) : m_meterCoeff;
            s.meterMs += k * (frameMs - s.meterMs);

            const double levelDb = 10.0 * std::log10 (s.meterMs + 1.0e-30) + c_weightCalibrationDb;
            const double target = std::clamp (c_referenceDb - levelDb, c_minGainDb, c_maxGainDb);
            const double coeff = s.learnedFrames < m_quickFrames ? m_quickCoeff
                               : (target > s.autoGainDb ? m_riseCoeff : m_fallCoeff);
            s.autoGainDb += coeff * (target - s.autoGainDb);
        }

        const double blendTarget = s.autoOn ? 1.0 : 0.0;
        s.autoBlend = blendTarget > s.autoBlend ? std::min (blendTarget, s.autoBlend + m_blendStep)
                                                : std::max (blendTarget, s.autoBlend - m_blendStep);

        s.gainPrev = s.gainNext;
        s.gainNext = std::exp (dsp::smoothstep01 (s.autoBlend) * s.autoGainDb * c_dbToLog);
    }

    void InModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& /*voice*/)
    {
        auto& s = m_s;
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

            if (trimMoving)
            {
                s.trimDb += m_trimCoeff * (s.trimTargetDb - s.trimDb);
                trimGain = std::exp (s.trimDb * c_dbToLog);
            }
            const double y = x * trimGain;
            meter = std::max (static_cast<float> (std::abs (y)), meter * m_meterRelease);

            const double w = m_weightShelf.process (m_weightHp.process (y));
            const double w2 = w * w;
            s.frameSum += w2;
            s.fastMs = dsp::flushTiny (s.fastMs + m_fastLevelCoeff * (w2 - s.fastMs));

            const double t = static_cast<double> (s.framePhase + 1) * invFrame;
            const double g = s.gainPrev + (s.gainNext - s.gainPrev) * t;
            const auto out = static_cast<float> (y * g);
            l[i] = out;

            const float a = std::abs (out);
            jaw += (a > jaw ? m_jawAttack : m_jawRelease) * (a - jaw);
            jawFast += (a > jawFast ? m_jawAttack : m_jawFastRelease) * (a - jawFast);

            m_detector.pushSample (out);
            const VoiceInfo& v = m_detector.getVoiceInfo();
            m_stream.frameForSample (i) = VoiceFrame { v.f0Hz, v.confidence, v.voiced };

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

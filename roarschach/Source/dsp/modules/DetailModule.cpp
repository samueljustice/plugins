#include "DetailModule.h"

#include "../MonsterFeatures.h"

#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_lookaheadSeconds = 0.001333;
        constexpr double c_fastReleaseSeconds = 0.0007;
        constexpr double c_slowChargeSeconds = 0.095;
        constexpr double c_slowReleaseNaturalSeconds = 0.500;
        constexpr double c_slowReleaseSnappySeconds = 0.080;   // a breath right after a syllable comes up fully
        constexpr double c_slowFloorDb = -120.0;
        constexpr double c_highChargeSeconds = 0.060;
        constexpr double c_highReleaseSeconds = 0.150;
        constexpr double c_gainRiseSeconds = 0.0015;
        constexpr double c_paramSeconds = 0.030;

        // Static curve (fits of the measured upward compressor, peak domain).
        constexpr double c_sineToPeakDb = 3.01;
        constexpr double c_highKneeDb = 9.0;
        constexpr double c_ceilingRefDb = 0.0;
        constexpr double c_highFadeDb = 3.0;            // the high section fades in over M = 0..3 dB
        constexpr double c_trimPeakDb = -12.0;          // reference syllable peak (spec §4.1)

        // Floor guard.
        constexpr double c_levelSeconds = 0.010;        // RMS of the guard's level
        constexpr double c_levelHoldSeconds = 0.060;
        constexpr double c_levelReleaseSeconds = 0.080;
        constexpr double c_runBandDb = 3.0;             // a floor stays within this of where it started
        constexpr double c_runSeconds = 0.400;          // ... for this long
        constexpr double c_floorFallSeconds = 0.050;
        constexpr double c_floorRiseDbPerSecond = 3.0;
        constexpr double c_floorMinDb = -96.0, c_floorMaxDb = -40.0;
        constexpr double c_unknownFloorDb = -70.0;      // assumed until a floor has been heard
        constexpr double c_activityDb = -40.0;          // louder than any floor
        constexpr double c_noBoostAboveFloorDb = 2.0;
        constexpr double c_fullBoostAboveFloorDb = 8.0;
        constexpr double c_maxExpansionDb = 6.0;

        constexpr double c_dbToLog = 0.11512925464970228;   // ln (10) / 20
    }

    double DetailModule::depthForKnob (double u)
    {
        u = std::clamp (u, 0.0, 1.0);
        return 48.0 * std::pow (u, 1.68);
    }

    double DetailModule::softMin (double a, double b)
    {
        return std::min (a, b) - c_highKneeDb * std::log1p (std::exp (-std::abs (a - b) / c_highKneeDb));
    }

    double DetailModule::lowGain (double level) const
    {
        if (m_curve.L <= 0.0)
            return 0.0;
        const double arg = std::min ((level - m_curve.mid) * m_curve.invW, 700.0);
        return m_curve.L / (1.0 + std::exp (arg));
    }

    double DetailModule::highGainStatic (double xp) const
    {
        if (m_curve.highScale <= 0.0)
            return 0.0;
        return m_curve.highScale * (softMin (xp + m_curve.M, m_curve.C) - xp);
    }

    double DetailModule::staticGainDb (double peakDb, bool withTrim) const
    {
        const double gl = lowGain (peakDb);
        const double gh = highGainStatic (peakDb + gl);
        return gl + gh - (withTrim ? m_curve.trim : 0.0);
    }

    void DetailModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        m_lookahead = std::max (1, static_cast<int> (std::lround (c_lookaheadSeconds * m_fs)));

        m_delayL.prepare (m_lookahead);
        m_delayR.prepare (m_lookahead);
        m_delayL.setFixedDelay (m_lookahead);
        m_delayR.setFixedDelay (m_lookahead);
        // The window holds the sample leaving the delay too, so its peak is always covered.
        m_peak.prepare (m_lookahead + 1);
        m_peak.setWindow (m_lookahead + 1);
        m_fade.prepare (m_fs, maxBlock, m_lookahead);

        m_fastRelease = dsp::poleFromTau (c_fastReleaseSeconds, m_fs);
        m_slowCharge = dsp::onePoleCoeff (c_slowChargeSeconds, m_fs);
        m_slowReleaseNatural = dsp::onePoleCoeff (c_slowReleaseNaturalSeconds, m_fs);
        m_slowReleaseSnappy = dsp::onePoleCoeff (c_slowReleaseSnappySeconds, m_fs);
        m_highCharge = dsp::onePoleCoeff (c_highChargeSeconds, m_fs);
        m_highRelease = dsp::onePoleCoeff (c_highReleaseSeconds, m_fs);
        m_gainRise = dsp::onePoleCoeff (c_gainRiseSeconds, m_fs);
        m_msCoeff = dsp::onePoleCoeff (c_levelSeconds, m_fs);

        const double blockRate = m_fs / static_cast<double> (c_controlBlock);
        m_paramCoeff = dsp::onePoleCoeff (c_paramSeconds, blockRate);
        m_holdBlocks = std::max (1, static_cast<int> (std::lround (c_levelHoldSeconds * blockRate)));
        m_levReleaseCoeff = dsp::onePoleCoeff (c_levelReleaseSeconds, blockRate);
        m_qualifyBlocks = std::max (1, static_cast<int> (std::lround (c_runSeconds * blockRate)));
        m_floorFallCoeff = dsp::onePoleCoeff (c_floorFallSeconds, blockRate);
        m_floorRiseDb = c_floorRiseDbPerSecond / blockRate;

        reset();
    }

    void DetailModule::reset()
    {
        m_delayL.reset();
        m_delayR.reset();
        m_peak.reset();
        m_fade.reset();

        const State keep = m_s;
        m_s = State {};
        m_s.lowTarget = keep.lowTarget;
        m_s.highTarget = keep.highTarget;
        m_s.lowU = keep.lowTarget;
        m_s.highU = keep.highTarget;
        m_s.snappy = keep.snappy;
        m_s.trimOn = keep.trimOn;
        m_s.floorOn = keep.floorOn;
        m_s.snapParams = true;          // the first settings after a reset apply at once
        updateCurve();
    }

    void DetailModule::resetWet()
    {
        // Fresh detectors when the module comes back on; the delay lines kept running.
        m_s.fast = 0.0;
        m_s.slowDb = c_slowFloorDb;
        m_s.highSlow = 0.0;
        m_s.gainDb = 0.0;
        m_s.levHeldDb = -150.0;
        m_s.holdLeft = 0;
        m_s.runRefDb = m_s.runSumDb = -150.0;
        m_s.runBlocks = 0;
        m_s.floorDb = c_unknownFloorDb;
        m_s.floorKnown = m_s.activitySeen = false;
        m_s.floorWeight = m_s.floorCutDb = 0.0;
    }

    void DetailModule::setParams (const DetailSettings& s)
    {
        m_s.lowTarget = std::clamp (static_cast<double> (dsp::finiteOrZero (s.low)), 0.0, 1.0);
        m_s.highTarget = std::clamp (static_cast<double> (dsp::finiteOrZero (s.high)), 0.0, 1.0);
        m_s.snappy = s.speed == Params::DetailSpeed::snappy;
        m_fade.setOn (s.on);
        if (m_s.snapParams)
        {
            m_s.lowU = m_s.lowTarget;
            m_s.highU = m_s.highTarget;
            m_s.snapParams = false;
            updateCurve();
        }
    }

    void DetailModule::setTestOptions (bool trim, bool floorGuard)
    {
        m_s.trimOn = trim;
        m_s.floorOn = floorGuard;
        if (! floorGuard)
            m_s.floorWeight = m_s.floorCutDb = 0.0;
        updateCurve();
    }

    void DetailModule::updateCurve()
    {
        auto& c = m_curve;
        c.L = depthForKnob (m_s.lowU);
        c.M = depthForKnob (m_s.highU);
        c.mid = -34.0 + 0.52 * c.L + c_sineToPeakDb;
        c.invW = 1.0 / (8.5 + 0.08 * c.L);
        c.C = c_ceilingRefDb + std::max (0.0, 6.0 - 0.28 * c.M);
        c.highScale = std::clamp (c.M / c_highFadeDb, 0.0, 1.0);
        c.trim = 0.0;
        if (m_s.trimOn)
            c.trim = staticGainDb (c_trimPeakDb, false);
    }

    void DetailModule::controlBlock()
    {
        auto& s = m_s;

        // Knob smoothing (30 ms) and the curve that follows it.
        const double dLow = s.lowTarget - s.lowU, dHigh = s.highTarget - s.highU;
        if (dLow != 0.0 || dHigh != 0.0)
        {
            s.lowU = std::abs (dLow) < 1.0e-6 ? s.lowTarget : s.lowU + m_paramCoeff * dLow;
            s.highU = std::abs (dHigh) < 1.0e-6 ? s.highTarget : s.highU + m_paramCoeff * dHigh;
            updateCurve();
        }

        if (! s.floorOn)
            return;

        const double levDb = std::max (-150.0, 10.0 * std::log10 (s.ms + 1.0e-15));

        // The guard's level: 10 ms RMS with a 60 ms hold and an 80 ms release.
        if (levDb >= s.levHeldDb)
        {
            s.levHeldDb = levDb;
            s.holdLeft = m_holdBlocks;
        }
        else if (s.holdLeft > 0)
            --s.holdLeft;
        else
            s.levHeldDb += m_levReleaseCoeff * (levDb - s.levHeldDb);

        // A floor is stationary: the level stays within a few dB of where the run started. Breath
        // gaps between syllables are too short to qualify; pauses between phrases do.
        if (std::abs (levDb - s.runRefDb) > c_runBandDb)
        {
            s.runRefDb = levDb;
            s.runSumDb = levDb;
            s.runBlocks = 1;
        }
        else
        {
            s.runSumDb += levDb;
            ++s.runBlocks;
        }
        const double runMean = s.runSumDb / static_cast<double> (std::max (1, s.runBlocks));

        if (levDb > c_activityDb)
            s.activitySeen = true;

        if (s.runBlocks >= m_qualifyBlocks)
        {
            const double target = std::clamp (runMean, c_floorMinDb, c_floorMaxDb);
            if (! s.floorKnown)
                s.floorDb = target;   // the first floor heard: take it as it is (a -45 dBFS room tone
                                      // must not wait for the 3 dB/s rise from the assumed floor)
            else if (target < s.floorDb)
                s.floorDb += m_floorFallCoeff * (target - s.floorDb);
            else
                s.floorDb = std::min (target, s.floorDb + m_floorRiseDb);
            s.floorKnown = true;
        }

        // Until anything louder has been heard, quiet material from the start is taken as floor,
        // so a take that opens on room tone never swells while the first run qualifies.
        double floorDb = s.floorDb;
        if (! s.floorKnown && ! s.activitySeen && runMean <= c_floorMaxDb)
            floorDb = std::clamp (runMean, c_floorMinDb, c_floorMaxDb);

        const double aboveFloor = s.levHeldDb - floorDb;
        s.floorWeight = dsp::smoothstep01 ((c_fullBoostAboveFloorDb - aboveFloor)
                                           / (c_fullBoostAboveFloorDb - c_noBoostAboveFloorDb));
        const double depthLimit = std::min (c_maxExpansionDb, 0.5 * (m_curve.L + m_curve.M));
        s.floorCutDb = std::clamp (-aboveFloor, 0.0, depthLimit);
    }

    void DetailModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& /*voice*/)
    {
        m_fade.captureDry (l, r, n, stereo);

        auto& s = m_s;

        if (! m_fade.isActive())
        {
            // Keep the look-ahead running so the wet is continuous when the module comes back.
            for (int i = 0; i < n; ++i)
            {
                const float xl = dsp::finiteOrZero (l[i]);
                const float xr = stereo ? dsp::finiteOrZero (r[i]) : 0.0f;
                m_peak.process (stereo ? std::max (std::abs (xl), std::abs (xr)) : std::abs (xl));
                m_delayL.push (xl);
                if (stereo)
                    m_delayR.push (xr);
            }
            m_fade.outputDry (l, r, n, stereo);
            s.boostDb = s.cutDb = s.gainReadDb = 0.0f;
            return;
        }

        if (m_fade.consumeRestart())
            resetWet();

        const double slowRelease = s.snappy ? m_slowReleaseSnappy : m_slowReleaseNatural;
        double gMax = -1.0e9, gMin = 1.0e9;

        for (int i = 0; i < n; ++i)
        {
            const float xl = dsp::finiteOrZero (l[i]);
            const float xr = stereo ? dsp::finiteOrZero (r[i]) : 0.0f;
            const float af = stereo ? std::max (std::abs (xl), std::abs (xr)) : std::abs (xl);
            const auto a = static_cast<double> (af);

            const auto pk = static_cast<double> (m_peak.process (af));
            const float dl = m_delayL.processFixed (xl);
            const float dr = stereo ? m_delayR.processFixed (xr) : 0.0f;

            // Detector: instant-attack peak with a 0.7 ms release, then the program-dependent
            // slow stage in dB.
            s.fast = std::max (pk, s.fast * m_fastRelease);
            const double fastDb = 20.0 * std::log10 (s.fast + 1.0e-9);
            const double toSlow = std::max (fastDb, c_slowFloorDb);
            s.slowDb += (toSlow > s.slowDb ? m_slowCharge : slowRelease) * (toSlow - s.slowDb);
            const double level = std::max (fastDb, s.slowDb);

            const double gLow = lowGain (level);
            double gHigh = 0.0;
            if (m_curve.highScale > 0.0)
            {
                const double xp = level + gLow;
                const double pushed = xp + m_curve.M;
                const double reduction = pushed - softMin (pushed, m_curve.C);   // >= 0
                s.highSlow += (reduction > s.highSlow ? m_highCharge : m_highRelease) * (reduction - s.highSlow);
                s.highSlow = dsp::flushTiny (s.highSlow);
                gHigh = m_curve.highScale * (m_curve.M - std::max (reduction, s.highSlow));
            }

            const double net = gLow + gHigh - m_curve.trim;
            const double target = net - s.floorWeight * std::max (net, 0.0) - s.floorCutDb;

            s.gainDb = target < s.gainDb ? target : s.gainDb + m_gainRise * (target - s.gainDb);
            s.gainDb = dsp::flushTiny (s.gainDb);
            gMax = std::max (gMax, s.gainDb);
            gMin = std::min (gMin, s.gainDb);

            const float g = static_cast<float> (std::exp (s.gainDb * c_dbToLog));
            l[i] = dl * g;
            if (stereo)
                r[i] = dr * g;

            s.ms += m_msCoeff * (a * a - s.ms);
            s.ms = dsp::flushTiny (s.ms);
            if (++s.blockPhase >= c_controlBlock)
            {
                s.blockPhase = 0;
                controlBlock();
            }
        }

        if (n > 0)
        {
            s.boostDb = static_cast<float> (std::max (0.0, gMax));
            s.cutDb = static_cast<float> (std::min (0.0, gMin));
            s.gainReadDb = static_cast<float> (s.gainDb);
        }

        m_fade.apply (l, r, n, stereo);
    }

    void DetailModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<DetailModule> (other);
        m_fade.copyStateFrom (o.m_fade);
        m_delayL.copyStateFrom (o.m_delayL);
        m_delayR.copyStateFrom (o.m_delayR);
        m_peak.copyStateFrom (o.m_peak);
        m_curve = o.m_curve;
        m_s = o.m_s;
    }

    void DetailModule::copyLeftStateToRight()
    {
        // The detector is already linked; only the audio path has a right channel.
        m_delayR.copyStateFrom (m_delayL);
        m_fade.copyLeftStateToRight();
    }

    void DetailModule::publishFeatures (MonsterFeatures& features) const
    {
        features.detailBoostDb.store (getBoostDb(), std::memory_order_relaxed);
        features.detailCutDb.store (getCutDb(), std::memory_order_relaxed);
    }
}

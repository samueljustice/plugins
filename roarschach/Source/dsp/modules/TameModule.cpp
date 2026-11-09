#include "TameModule.h"

#include "../MonsterFeatures.h"
#include "../common/GainComputer.h"

#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_peakReleaseSeconds = 0.060;
        constexpr double c_rmsSeconds = 0.025;
        constexpr double c_crestLowDb = 6.0, c_crestHighDb = 14.0;
        constexpr double c_slowFactor = 6.0;
        constexpr double c_holdSeconds = 0.100;
        constexpr double c_heldGrDb = -0.5;
        constexpr double c_heldToleranceDb = 0.5;
        constexpr double c_smoothSeconds = 0.020;
        constexpr double c_lnTenOver20 = 0.11512925464970228;
        constexpr double c_tinyPower = 1.0e-20;

        // The release values are the gain smoother's time constants. The peak detector's own
        // 60 ms decay adds to them, so they are set below the release the band shows from the
        // end of a burst to 37 % of its reduction (HM 140 ms, H 90 ms).
        struct BandSpec
        {
            double baseThreshold, thresholdPerT, depthPerT, slope, attack, release;
        };

        constexpr std::array<BandSpec, 2> c_bandSpecs { {
            { -24.0, -12.0, 12.0, 0.45, 0.004, 0.097 },   // HM: measured release 140 ms
            { -30.0, -12.0, 16.0, 0.55, 0.002, 0.049 },   // H: measured release 90 ms
        } };

        double ratioFor (double s) { return 1.5 * std::pow (20.0 / 1.5, s); }

        double curve (double levelDb, double threshold, double depth, double s)
        {
            if (depth <= 0.0)
                return 0.0;
            const double knee = (1.0 - s) * depth * 0.5;
            const double g = GainComputer::compress (levelDb, threshold, 1.0 / ratioFor (s) - 1.0, knee);
            return GainComputer::softDepth (g, depth);
        }
    }

    double TameModule::staticCurveDb (int band, double t, double levelDb)
    {
        const auto& b = c_bandSpecs[static_cast<size_t> (std::clamp (band, 0, 1))];
        t = std::clamp (t, 0.0, 1.0);
        return curve (levelDb, b.baseThreshold + b.thresholdPerT * t, b.depthPerT * t, b.slope);
    }

    void TameModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        m_maxBlock = std::max (1, maxBlock);
        m_bandBuffer.assign (static_cast<size_t> (4 * m_maxBlock), 0.0f);

        for (auto& c : m_ch)
        {
            c.splitA.prepare (m_fs, c_crossoverLowHz);
            c.splitB.prepare (m_fs, c_crossoverHighHz);
            c.lowAllpass.prepare (m_fs, c_crossoverHighHz);
        }

        for (size_t k = 0; k < 2; ++k)
        {
            auto& b = m_bands[k];
            const auto& s = c_bandSpecs[k];
            b.baseThreshold = s.baseThreshold;
            b.thresholdPerT = s.thresholdPerT;
            b.depthPerT = s.depthPerT;
            b.slopeS = s.slope;
            b.attackCoeff = dsp::onePoleCoeff (s.attack, m_fs);
            b.releaseCoeff = dsp::onePoleCoeff (s.release, m_fs);
            b.slowReleaseCoeff = dsp::onePoleCoeff (s.release * c_slowFactor, m_fs);
            b.peakRelease = dsp::poleFromTau (c_peakReleaseSeconds, m_fs);
            b.msCoeff = dsp::onePoleCoeff (c_rmsSeconds, m_fs);
        }

        m_amountCoeff = dsp::onePoleCoeff (c_smoothSeconds, m_fs);
        m_holdSamples = dsp::samplesFor (c_holdSeconds, m_fs, 1);

        m_fade.prepare (m_fs, m_maxBlock, 0);
        reset();
    }

    void TameModule::resetWet()
    {
        for (auto& b : m_bands)
            b.resetState();
        m_amount = m_amountTarget;
    }

    void TameModule::reset()
    {
        m_fade.reset();
        for (auto& c : m_ch)
        {
            c.splitA.reset();
            c.splitB.reset();
            c.lowAllpass.reset();
        }
        resetWet();
    }

    void TameModule::setParams (const TameSettings& s)
    {
        m_fade.setOn (s.on);
        m_amountTarget = std::clamp (static_cast<double> (s.amount), 0.0, 1.0);
    }

    void TameModule::setForcedGainsForTest (bool forced, double highMidDb, double highDb)
    {
        m_forced = forced;
        m_forcedHm = std::min (0.0, highMidDb);
        m_forcedH = std::min (0.0, highDb);
    }

    double TameModule::runBand (BandDynamics& b, double power, double absPeak)
    {
        // Smart detector.
        b.peak = std::max (absPeak, b.peak * b.peakRelease);
        b.ms += b.msCoeff * (power - b.ms);
        b.peak = dsp::flushTiny (b.peak);
        b.ms = dsp::flushTiny (b.ms);
        const double pk = 20.0 * std::log10 (b.peak + 1.0e-10);
        const double rm = 10.0 * std::log10 (b.ms + c_tinyPower);
        const double w = dsp::smoothstep01 ((pk - rm - c_crestLowDb) / (c_crestHighDb - c_crestLowDb));
        const double level = rm + w * (pk - rm);

        // Static curve.
        const double target = curve (level, b.baseThreshold + b.thresholdPerT * m_amount, b.depthPerT * m_amount, b.slopeS);

        // Attack + dual release.
        b.fast += (target < b.fast ? b.attackCoeff : b.releaseCoeff) * (target - b.fast);
        b.fast = dsp::flushTiny (b.fast);
        // "Held": a reduction that is not releasing (the target is at least as deep as the fast
        // stage). A short burst releases before 100 ms and never charges the slow stage.
        if (b.fast < c_heldGrDb && target <= b.fast + c_heldToleranceDb)
            b.heldSamples = std::min (b.heldSamples + 1, m_holdSamples);
        else
            b.heldSamples = 0;
        if (target > b.slow)
            b.slow += b.slowReleaseCoeff * (target - b.slow);
        if (b.heldSamples >= m_holdSamples)
            b.slow = std::min (b.slow, b.fast);
        b.slow = dsp::flushTiny (b.slow);

        b.grDb = std::min (0.0, std::min (b.fast, b.slow));
        return b.grDb;
    }

    void TameModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView&)
    {
        n = std::min (n, m_maxBlock);
        const auto mb = static_cast<size_t> (m_maxBlock);
        float* hmL = m_bandBuffer.data();
        float* hL = hmL + mb;
        float* hmR = hL + mb;
        float* hR = hmR + mb;

        // The split and the allpass sum always run (Off is this allpass, never a phase jump).
        const int numCh = stereo ? 2 : 1;
        for (int c = 0; c < numCh; ++c)
        {
            float* io = c == 0 ? l : r;
            float* hm = c == 0 ? hmL : hmR;
            float* h = c == 0 ? hL : hR;
            auto& ch = m_ch[static_cast<size_t> (c)];
            for (int i = 0; i < n; ++i)
            {
                const double x = dsp::finiteOrZero (static_cast<double> (io[i]));
                double low, upper, highMid, high;
                ch.splitA.process (x, low, upper);
                ch.splitB.process (upper, highMid, high);
                low = ch.lowAllpass.process (low);
                hm[i] = static_cast<float> (highMid);
                h[i] = static_cast<float> (high);
                io[i] = static_cast<float> (low + highMid + high);
            }
        }

        m_fade.captureDry (l, r, n, stereo);
        if (! m_fade.isActive())
        {
            m_fade.outputDry (l, r, n, stereo);
            return;
        }
        if (m_fade.consumeRestart())
            resetWet();

        for (int i = 0; i < n; ++i)
        {
            m_amount += m_amountCoeff * (m_amountTarget - m_amount);
            m_amount = dsp::flushTiny (m_amount);

            double grHm, grH;
            if (m_forced)
            {
                grHm = m_bands[0].grDb = m_forcedHm;
                grH = m_bands[1].grDb = m_forcedH;
            }
            else
            {
                double pHm = static_cast<double> (hmL[i]), pH = static_cast<double> (hL[i]);
                double aHm = std::abs (pHm), aH = std::abs (pH);
                pHm *= pHm;
                pH *= pH;
                if (stereo)
                {
                    const double rHm = static_cast<double> (hmR[i]), rH = static_cast<double> (hR[i]);
                    pHm = std::max (pHm, rHm * rHm);
                    pH = std::max (pH, rH * rH);
                    aHm = std::max (aHm, std::abs (rHm));
                    aH = std::max (aH, std::abs (rH));
                }
                grHm = runBand (m_bands[0], pHm, aHm);
                grH = runBand (m_bands[1], pH, aH);
            }

            const double kHm = grHm == 0.0 ? 0.0 : std::exp (grHm * c_lnTenOver20) - 1.0;
            const double kH = grH == 0.0 ? 0.0 : std::exp (grH * c_lnTenOver20) - 1.0;
            l[i] = static_cast<float> (static_cast<double> (l[i]) + kHm * static_cast<double> (hmL[i]) + kH * static_cast<double> (hL[i]));
            if (stereo)
                r[i] = static_cast<float> (static_cast<double> (r[i]) + kHm * static_cast<double> (hmR[i]) + kH * static_cast<double> (hR[i]));
        }

        m_fade.apply (l, r, n, stereo);
    }

    void TameModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<TameModule> (other);
        m_fade.copyStateFrom (o.m_fade);
        for (size_t c = 0; c < m_ch.size(); ++c)
            m_ch[c].copyStateFrom (o.m_ch[c]);
        for (size_t k = 0; k < m_bands.size(); ++k)
            m_bands[k].copyStateFrom (o.m_bands[k]);
        m_amountTarget = o.m_amountTarget;
        m_amount = o.m_amount;
        m_forced = o.m_forced;
        m_forcedHm = o.m_forcedHm;
        m_forcedH = o.m_forcedH;
    }

    void TameModule::copyLeftStateToRight()
    {
        m_fade.copyLeftStateToRight();
        m_ch[1].copyStateFrom (m_ch[0]);
    }

    void TameModule::publishFeatures (MonsterFeatures& features) const
    {
        features.tameGrDb.store (getGrDb() * m_fade.getValue(), std::memory_order_relaxed);
    }
}

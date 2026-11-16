#include "DeessModule.h"

#include "../KnobLaws.h"
#include "../MonsterFeatures.h"

#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_envAttackSeconds = 0.0005;
        constexpr double c_envReleaseSeconds = 0.020;
        constexpr double c_grAttackSeconds = 0.0005;
        constexpr double c_grReleaseHotSeconds = 0.050;     // while the band is still hot
        constexpr double c_grReleaseFastSeconds = 0.006;    // once the target has let go
        constexpr double c_letGoDb = -0.5;
        constexpr double c_fixedThresholdDb = -16.0;
        constexpr double c_guardMarginDb = 5.0;             // above the voice's own balance
        constexpr double c_guardShiftPerS = 2.5;            // the guard falls 2.5 S over the top of the travel
        constexpr double c_ceilingDb = -6.0;                // a band this close to the whole is sibilance
        constexpr double c_relSlope = 1.5;
        constexpr double c_bandFloorDb = -55.0;
        constexpr double c_refMemorySeconds = 2.5;
        constexpr double c_refStartWeightSeconds = 0.02;
        constexpr double c_refStartDb = -21.0;              // where the guard equals the fixed threshold
        constexpr double c_refHeldMemorySeconds = 0.15;
        constexpr double c_heldGrDb = -1.0;
        constexpr double c_holdSeconds = 0.3;               // longer than an ess
        constexpr double c_wideRelaxSeconds = 0.15;
        constexpr double c_wideDepth = 0.8;
        constexpr double c_activeWindowDb = 30.0, c_activeFloorDb = -60.0, c_topFallDbPerSecond = 3.0;
        constexpr double c_smoothSeconds = 0.020;
        constexpr double c_lnTenOver20 = 0.11512925464970228;
        constexpr double c_svfK = 1.4142135623730951;
        constexpr double c_pi = 3.14159265358979323846;
    }

    double DeessModule::rangeDbFor (const DeessSettings& s)
    {
        return laws::deessRangeDb (static_cast<double> (s.amount));
    }

    void DeessModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        m_dt = 1.0 / m_fs;
        {
            const double fc = std::min (c_shelfHz, 0.45 * m_fs);
            const double g = std::tan (c_pi * fc / m_fs);
            m_svfA1 = 1.0 / (1.0 + g * (g + c_svfK));
            m_svfA2 = g * m_svfA1;
            m_svfA3 = g * m_svfA2;
        }
        for (auto& d : m_detector)
        {
            d[0].setCoeffs (Biquad::highpass (m_fs, c_detectorLowHz, Biquad::c_butterworthQ));
            d[1].setCoeffs (Biquad::lowpass (m_fs, c_detectorHighHz, Biquad::c_butterworthQ));
        }

        m_rangeCoeff = dsp::onePoleCoeff (c_smoothSeconds, m_fs);
        m_wideCoeff = dsp::onePoleCoeff (c_smoothSeconds, m_fs);
        m_envAttack = dsp::onePoleCoeff (c_envAttackSeconds, m_fs);
        m_envRelease = dsp::onePoleCoeff (c_envReleaseSeconds, m_fs);
        m_grAttack = dsp::onePoleCoeff (c_grAttackSeconds, m_fs);
        m_grReleaseHot = dsp::onePoleCoeff (c_grReleaseHotSeconds, m_fs);
        m_grReleaseFast = dsp::onePoleCoeff (c_grReleaseFastSeconds, m_fs);
        m_topFallPerSample = c_topFallDbPerSecond / m_fs;

        m_fade.prepare (m_fs, maxBlock, 0);
        reset();
    }

    void DeessModule::resetWet()
    {
        for (auto& c : m_shelf)
            for (auto& sec : c)
                sec = Svf {};
        for (auto& d : m_detector)
            for (auto& f : d)
                f.reset();
        m_range = m_rangeTarget;
        m_shift = m_shiftTarget;
        m_wide = m_wideTarget;
        m_envBand = m_envFull = 0.0;
        m_grDb = 0.0;
        m_refRel = c_refStartDb;
        m_refSeconds = 0.0;
        m_topDb = -120.0;
        m_heldSeconds = 0.0;
    }

    void DeessModule::reset()
    {
        m_fade.reset();
        resetWet();
    }

    void DeessModule::setParams (const DeessSettings& s)
    {
        m_fade.setOn (s.on);
        m_rangeTarget = rangeDbFor (s);
        m_shiftTarget = laws::deessThresholdShiftDb (static_cast<double> (s.amount));
        m_wideTarget = s.mode == Params::DeessMode::wide ? 1.0 : 0.0;
    }

    void DeessModule::setForcedGainForTest (bool forced, double grDb)
    {
        m_forced = forced;
        m_forcedGrDb = std::min (0.0, grDb);
    }

    void DeessModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView&)
    {
        m_fade.captureDry (l, r, n, stereo);
        if (! m_fade.isActive())
        {
            m_fade.outputDry (l, r, n, stereo);
            return;
        }
        if (m_fade.consumeRestart())
            resetWet();

        const int numCh = stereo ? 2 : 1;
        float* io[2] = { l, r };

        for (int i = 0; i < n; ++i)
        {
            m_range += m_rangeCoeff * (m_rangeTarget - m_range);
            m_wide += m_wideCoeff * (m_wideTarget - m_wide);
            m_shift += m_rangeCoeff * (m_shiftTarget - m_shift);
            m_range = dsp::flushTiny (m_range);
            m_shift = dsp::flushTiny (m_shift);
            m_wide = dsp::flushTiny (m_wide);

            double x[2];
            x[0] = dsp::finiteOrZero (static_cast<double> (l[i]));
            x[1] = stereo ? dsp::finiteOrZero (static_cast<double> (r[i])) : 0.0;

            if (m_forced)
            {
                m_grDb = m_forcedGrDb;
            }
            else
            {
                // ---- Detector (linked)
                double band = std::abs (m_detector[0][1].process (m_detector[0][0].process (x[0]))), full = std::abs (x[0]);
                if (stereo)
                {
                    band = std::max (band, std::abs (m_detector[1][1].process (m_detector[1][0].process (x[1]))));
                    full = std::max (full, std::abs (x[1]));
                }
                m_envBand += (band > m_envBand ? m_envAttack : m_envRelease) * (band - m_envBand);
                m_envFull += (full > m_envFull ? m_envAttack : m_envRelease) * (full - m_envFull);
                m_envBand = dsp::flushTiny (m_envBand);
                m_envFull = dsp::flushTiny (m_envFull);

                const double bandDb = dsp::gainToDb (m_envBand);
                const double fullDb = dsp::gainToDb (m_envFull);
                const double rel = bandDb - fullDb;

                // ---- The voice's own balance: the mean of rel (dB) over the sounding part of the
                // last 2.5 s; 0.15 s while a reduction has been held longer than an ess lasts.
                m_topDb = std::max (fullDb, m_topDb - m_topFallPerSample);
                if (fullDb > m_topDb - c_activeWindowDb && fullDb > c_activeFloorDb)
                {
                    double memory = std::min (c_refStartWeightSeconds + m_refSeconds, c_refMemorySeconds);
                    if (m_heldSeconds > c_holdSeconds)
                        memory = std::min (memory, c_refHeldMemorySeconds);
                    m_refRel += (std::clamp (rel, -40.0, 0.0) - m_refRel) * m_dt / (memory + m_dt);
                    m_refSeconds = std::min (m_refSeconds + m_dt, c_refMemorySeconds);
                }

                // ---- Gain computer
                const double guard = m_refRel + c_guardMarginDb - c_guardShiftPerS * m_shift;
                const double threshold = std::min (std::max (c_fixedThresholdDb - m_shift, guard), c_ceilingDb - m_shift);
                const double limit = std::min (m_range, 0.0);
                double target = bandDb >= c_bandFloorDb ? std::clamp (-c_relSlope * (rel - threshold), limit, 0.0) : 0.0;

                // ---- How long the reduction has been held; Wide relaxes after the hold.
                if (m_grDb < c_heldGrDb)
                    m_heldSeconds += m_dt;
                else if (target > c_letGoDb)
                    m_heldSeconds = 0.0;
                if (m_wide > 0.0 && m_heldSeconds > c_holdSeconds)
                    target = std::max (target, limit * (1.0 - m_wide * (1.0 - std::exp (-(m_heldSeconds - c_holdSeconds) / c_wideRelaxSeconds))));

                // ---- Ballistics
                if (target < m_grDb)
                    m_grDb += m_grAttack * (target - m_grDb);
                else
                    m_grDb += (target > c_letGoDb ? m_grReleaseFast : m_grReleaseHot) * (target - m_grDb);
                m_grDb = std::min (0.0, dsp::flushTiny (m_grDb));
                if (m_grDb > -1.0e-4)
                    m_grDb = 0.0;
            }

            // ---- The shelf (always run, so its state stays warm) and Wide
            const bool active = m_grDb < 0.0;
            const double g1 = active ? std::exp (0.5 * m_grDb * c_lnTenOver20) : 1.0;   // per section: sqrt (G)
            const double mBp = (std::sqrt (g1) - 1.0) * c_svfK;
            const double mHp = g1 - 1.0;
            const double wide = active ? std::exp (c_wideDepth * m_grDb * c_lnTenOver20) : 1.0;
            for (int c = 0; c < numCh; ++c)
            {
                double u = x[c];
                for (auto& sec : m_shelf[static_cast<size_t> (c)])
                {
                    const double v3 = u - sec.ic2;
                    const double v1 = m_svfA1 * sec.ic1 + m_svfA2 * v3;
                    const double v2 = sec.ic2 + m_svfA2 * sec.ic1 + m_svfA3 * v3;
                    sec.ic1 = dsp::flushTiny (2.0 * v1 - sec.ic1);
                    sec.ic2 = dsp::flushTiny (2.0 * v2 - sec.ic2);
                    const double hp = u - c_svfK * v1 - v2;
                    u += mBp * v1 + mHp * hp;
                }
                io[c][i] = static_cast<float> (active ? u + m_wide * (wide * x[c] - u) : x[c]);
            }
        }

        m_fade.apply (l, r, n, stereo);
    }

    void DeessModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<DeessModule> (other);
        m_fade.copyStateFrom (o.m_fade);
        m_shelf = o.m_shelf;
        for (size_t c = 0; c < 2; ++c)
            for (size_t k = 0; k < 2; ++k)
                m_detector[c][k].copyStateFrom (o.m_detector[c][k]);
        m_rangeTarget = o.m_rangeTarget;
        m_range = o.m_range;
        m_shiftTarget = o.m_shiftTarget;
        m_shift = o.m_shift;
        m_wideTarget = o.m_wideTarget;
        m_wide = o.m_wide;
        m_envBand = o.m_envBand;
        m_envFull = o.m_envFull;
        m_grDb = o.m_grDb;
        m_refRel = o.m_refRel;
        m_refSeconds = o.m_refSeconds;
        m_topDb = o.m_topDb;
        m_heldSeconds = o.m_heldSeconds;
        m_forced = o.m_forced;
        m_forcedGrDb = o.m_forcedGrDb;
    }

    void DeessModule::copyLeftStateToRight()
    {
        m_fade.copyLeftStateToRight();
        m_shelf[1] = m_shelf[0];
        for (size_t k = 0; k < 2; ++k)
            m_detector[1][k].copyStateFrom (m_detector[0][k]);
    }

    void DeessModule::publishFeatures (MonsterFeatures& features) const
    {
        // Wide applies 0.8 of the reduction: the readout is what the signal gets.
        const double shown = m_grDb * (1.0 - (1.0 - c_wideDepth) * m_wide);
        features.deessGrDb.store (static_cast<float> (shown) * m_fade.getValue(), std::memory_order_relaxed);
    }
}

#include "DeessModule.h"

#include "../KnobLaws.h"
#include "../MonsterFeatures.h"

#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_envAttackSeconds = 0.0005;
        constexpr double c_envReleaseSeconds = 0.040;
        constexpr double c_grAttackSeconds = 0.0005;
        constexpr double c_grReleaseSeconds = 0.050;
        constexpr double c_relOffsetDb = 16.0;
        constexpr double c_relSlope = 1.5;
        constexpr double c_bandFloorDb = -55.0;
        constexpr double c_smoothSeconds = 0.020;
        constexpr double c_lnTenOver20 = 0.11512925464970228;
    }

    double DeessModule::rangeDbFor (const DeessSettings& s)
    {
        return laws::deessRangeDb (static_cast<double> (s.amount));
    }

    void DeessModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        for (auto& f : m_split)
            f.setCoeffs (Biquad::lowpass (m_fs, c_splitHz, Biquad::c_butterworthQ));
        for (auto& d : m_detector)
        {
            d[0].setCoeffs (Biquad::highpass (m_fs, c_detectorLowHz, Biquad::c_butterworthQ));
            d[1].setCoeffs (Biquad::lowpass (m_fs, c_detectorHighHz, Biquad::c_butterworthQ));
        }

        m_rangeCoeff = dsp::onePoleCoeff (c_smoothSeconds, m_fs);
        m_wideCoeff = dsp::onePoleCoeff (c_smoothSeconds, m_fs);
        m_envAttack = dsp::onePoleCoeff (c_envAttackSeconds, m_fs);
        m_envRelease = dsp::onePoleCoeff (c_envReleaseSeconds, m_fs);
        m_grBallistics.prepare (m_fs);
        m_grBallistics.setTimes (c_grAttackSeconds, c_grReleaseSeconds);

        m_fade.prepare (m_fs, maxBlock, 0);
        reset();
    }

    void DeessModule::resetWet()
    {
        for (auto& f : m_split)
            f.reset();
        for (auto& d : m_detector)
            for (auto& f : d)
                f.reset();
        m_range = m_rangeTarget;
        m_shift = m_shiftTarget;
        m_wide = m_wideTarget;
        m_envBand = m_envFull = 0.0;
        m_grBallistics.reset();
        m_grDb = 0.0;
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

        for (int i = 0; i < n; ++i)
        {
            m_range += m_rangeCoeff * (m_rangeTarget - m_range);
            m_wide += m_wideCoeff * (m_wideTarget - m_wide);
            m_shift += m_rangeCoeff * (m_shiftTarget - m_shift);
            m_range = dsp::flushTiny (m_range);
            m_shift = dsp::flushTiny (m_shift);
            m_wide = dsp::flushTiny (m_wide);

            const double xl = dsp::finiteOrZero (static_cast<double> (l[i]));
            const double xr = stereo ? dsp::finiteOrZero (static_cast<double> (r[i])) : 0.0;

            // ---- Detector (linked)
            const double bl = m_detector[0][1].process (m_detector[0][0].process (xl));
            double band = std::abs (bl), full = std::abs (xl);
            if (stereo)
            {
                band = std::max (band, std::abs (m_detector[1][1].process (m_detector[1][0].process (xr))));
                full = std::max (full, std::abs (xr));
            }
            m_envBand += (band > m_envBand ? m_envAttack : m_envRelease) * (band - m_envBand);
            m_envFull += (full > m_envFull ? m_envAttack : m_envRelease) * (full - m_envFull);
            m_envBand = dsp::flushTiny (m_envBand);
            m_envFull = dsp::flushTiny (m_envFull);

            const double bandDb = dsp::gainToDb (m_envBand);
            double target = 0.0;
            if (bandDb >= c_bandFloorDb)
            {
                const double rel = bandDb - dsp::gainToDb (m_envFull);
                target = std::clamp (-c_relSlope * (rel + c_relOffsetDb + m_shift), std::min (m_range, 0.0), 0.0);
            }
            m_grDb = std::min (0.0, m_grBallistics.process (target));

            // ---- Split / Wide: y = x - (1 - g) ((1 - wide) high + wide x)
            const double k = (m_grDb == 0.0) ? 0.0 : 1.0 - std::exp (m_grDb * c_lnTenOver20);
            const double highL = xl - m_split[0].process (xl);
            l[i] = static_cast<float> (xl - k * (highL + m_wide * (xl - highL)));
            if (stereo)
            {
                const double highR = xr - m_split[1].process (xr);
                r[i] = static_cast<float> (xr - k * (highR + m_wide * (xr - highR)));
            }
        }

        m_fade.apply (l, r, n, stereo);
    }

    void DeessModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<DeessModule> (other);
        m_fade.copyStateFrom (o.m_fade);
        for (size_t c = 0; c < 2; ++c)
        {
            m_split[c].copyStateFrom (o.m_split[c]);
            for (size_t k = 0; k < 2; ++k)
                m_detector[c][k].copyStateFrom (o.m_detector[c][k]);
        }
        m_rangeTarget = o.m_rangeTarget;
        m_range = o.m_range;
        m_shiftTarget = o.m_shiftTarget;
        m_shift = o.m_shift;
        m_wideTarget = o.m_wideTarget;
        m_wide = o.m_wide;
        m_envBand = o.m_envBand;
        m_envFull = o.m_envFull;
        m_grBallistics.copyStateFrom (o.m_grBallistics);
        m_grDb = o.m_grDb;
    }

    void DeessModule::copyLeftStateToRight()
    {
        m_fade.copyLeftStateToRight();
        m_split[1].copyStateFrom (m_split[0]);
        for (size_t k = 0; k < 2; ++k)
            m_detector[1][k].copyStateFrom (m_detector[0][k]);
    }

    void DeessModule::publishFeatures (MonsterFeatures& features) const
    {
        features.deessGrDb.store (static_cast<float> (m_grDb) * m_fade.getValue(), std::memory_order_relaxed);
    }
}

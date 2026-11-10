#include "RoarModule.h"

#include "../KnobLaws.h"

#include "../MonsterFeatures.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        // Triode curve (normalised, Vb = 1).
        constexpr double c_gridMax = 1.5;        // grid-conduction softness
        constexpr double c_cutoff = 2.6;         // cutoff at gh = -c_cutoff
        constexpr double c_cutoffRound = 0.8;    // smooth max(z, 0): larger = rounder cutoff
        // Wide enough that the linear extension beyond it stays close to the curve (with the soft grid
        // conduction the positive side is still bending at +6).
        constexpr double c_tableLow = -16.0, c_tableHigh = 16.0;
        constexpr int c_tablePoints = 8192;

        // Dynamic bias.
        constexpr double c_kb1Low = 0.6, c_kb1High = 0.4, c_kb2 = 0.3;
        // Stage 1's bias shift saturates softly at this many Vb, so its rest point never reaches
        // cutoff (2.6): a long loud passage compresses and turns more even, but never gates.
        constexpr double c_shift1Max = 1.0;
        constexpr double c_env1AttackSeconds = 0.002, c_env1ReleaseSeconds = 0.150;
        constexpr double c_env2AttackSeconds = 0.003, c_env2ReleaseSeconds = 0.250;
        constexpr double c_gridCurrentMaxG = 6.0;   // grid swing above which the current proxy stops rising
        // Beyond the table the curve continues as a line of under 1 % slope; limiting the grid to
        // +-64 (a 0 dBFS input at full drive reaches about 11) keeps absurd inputs bounded.
        constexpr double c_gridLimit = 64.0;
        // Input safety bound (+30 dBFS): far above any real signal, it only stops an absurd sample
        // (1e30, FLT_MAX) from loading the linear filters ahead of the first grid with a state that
        // would take seconds to drain and hold the stage blocked meanwhile.
        constexpr double c_inputLimit = 32.0;

        // Drives.
        constexpr double c_drive1Cal = 1.41;   // Drive1 law: laws::roarDrive1Db
        constexpr double c_drive2Cal = 0.5, c_drive2RangeDb = 8.0;

        // Filters.
        // Tone stack and Miller corners, tuned on the reference take (the tube-stage fingerprint:
        // 125-250 Hz -2 dB, 8-16 kHz +2 dB, relative to the input at the default Drive). The bass
        // boost sits mostly at 80 Hz so it does not undo the scoop; the treble boost leans on the top
        // shelf; the Miller corners sit high enough that the default Drive keeps the air.
        constexpr double c_miller1Hz = 20000.0, c_miller2Hz = 16000.0;
        constexpr double c_bypass2PerDrive = 0.7;   // stage-2 Miller bypass = 1 - this * a
        constexpr double c_couplingHz = 12.0;
        constexpr double c_bassLowHz = 80.0, c_bassHighHz = 300.0, c_bassLowShare = 0.85;
        constexpr double c_bassMaxDb = 6.0;
        constexpr double c_scoopHz = 200.0, c_scoopQ = 0.8;   // depth: laws::roarScoopDb
        constexpr double c_trebleLowHz = 2500.0, c_trebleHighHz = 10000.0, c_trebleLowShare = 0.35;
        constexpr double c_trebleMaxDb = 7.0;
        constexpr double c_fixedTopHz = 13000.0, c_fixedTopDb = 1.5;
        constexpr double c_outLpHz = 18000.0, c_outLpPerDrive = 0.75;
        constexpr double c_outHpHz = 25.0, c_outHpQ = 0.9;
        constexpr double c_micBellHz = 70.0, c_micBellQ = 0.35, c_micBellDb = -3.8;
        constexpr double c_micShelfHz = 15000.0, c_micShelfDb = -2.0;

        constexpr double c_smoothSeconds = 0.030;
        constexpr double c_vuSeconds = 0.065;   // two cascaded poles: about 300 ms to settle

        // Static makeup in dB: rows Tone -1, -0.5, 0, +0.5, +1; columns Drive 0, 0.1, ..., 1.
        // Calibrated at 48 kHz at the reference level (-20 dBFS RMS): the mean of voice-shaped pink
        // noise and the reference voice signal, K-ish weighted RMS (80 Hz HPF, +4 dB at 1.5 kHz).
        // The top columns absorb the extra drive of the steeper law above 50 % (the stage
        // compresses harder there), and the outer rows the stronger tilt at low Drive.
        constexpr int c_makeupTones = 5, c_makeupDrives = 11;
        constexpr double c_makeupTable[c_makeupTones][c_makeupDrives] = {
            { 0.22, 0.32, 0.42, 0.49, 0.57, 0.40, 0.43, 0.67, 1.57, 3.43, 6.44 },
            { 0.31, 0.43, 0.52, 0.64, 0.74, 0.61, 0.70, 1.03, 1.99, 3.88, 6.86 },
            { 0.36, 0.47, 0.60, 0.70, 0.81, 0.75, 0.86, 1.21, 2.18, 4.03, 6.83 },
            { 0.36, 0.47, 0.60, 0.71, 0.84, 0.79, 0.89, 1.22, 2.14, 3.86, 6.39 },
            { 0.32, 0.44, 0.54, 0.68, 0.80, 0.76, 0.81, 1.08, 1.89, 3.39, 5.55 },
        };

        double plateCurrent (double g)
        {
            const double gh = g <= 0.0 ? g : g / (1.0 + g / c_gridMax);
            const double z = 1.0 + gh / c_cutoff;
            const double zs = 0.5 * (z + std::sqrt (z * z + c_cutoffRound * c_cutoffRound));
            return zs * std::sqrt (zs);
        }

        double restSlope()
        {
            constexpr double h = 1.0e-5;
            return (plateCurrent (-1.0 + h) - plateCurrent (-1.0 - h)) / (2.0 * h);
        }

        // Product of two first-order sections (b2 = a2 = 0) as one second-order section.
        Biquad::Coeffs cascade1 (const Biquad::Coeffs& p, const Biquad::Coeffs& q)
        {
            Biquad::Coeffs c;
            c.b0 = p.b0 * q.b0;
            c.b1 = p.b0 * q.b1 + p.b1 * q.b0;
            c.b2 = p.b1 * q.b1;
            c.a1 = p.a1 + q.a1;
            c.a2 = p.a1 * q.a1;
            return c;
        }

        double finiteOr (double x, double fallback) { return std::isfinite (x) ? x : fallback; }
    }

    RoarModule::RoarModule() = default;

    double RoarModule::triode (double g)
    {
        static const double slope = restSlope();
        static const double rest = plateCurrent (-1.0);
        return (plateCurrent (g) - rest) / slope;
    }

    const AdaaTable& RoarModule::table()
    {
        static const AdaaTable t = []
        {
            AdaaTable built;
            const double slope = restSlope();
            built.build ([slope] (double g) { return plateCurrent (g) / slope; }, c_tableLow, c_tableHigh, c_tablePoints);
            return built;
        }();
        return t;
    }

    double RoarModule::drive1For (double a) { return c_drive1Cal * dsp::dbToGain (laws::roarDrive1Db (a)); }
    double RoarModule::drive2For (double a) { return c_drive2Cal * dsp::dbToGain (c_drive2RangeDb * a); }

    double RoarModule::makeupDb (double a, double t)
    {
        const double x = std::clamp (finiteOr (a, 0.0), 0.0, 1.0) * static_cast<double> (c_makeupDrives - 1);
        const double y = (std::clamp (finiteOr (t, 0.0), -1.0, 1.0) + 1.0) * 0.5 * static_cast<double> (c_makeupTones - 1);
        const int i = std::min (static_cast<int> (x), c_makeupDrives - 2);
        const int j = std::min (static_cast<int> (y), c_makeupTones - 2);
        const double fx = x - static_cast<double> (i), fy = y - static_cast<double> (j);
        const double lo = c_makeupTable[j][i] + fx * (c_makeupTable[j][i + 1] - c_makeupTable[j][i]);
        const double hi = c_makeupTable[j + 1][i] + fx * (c_makeupTable[j + 1][i + 1] - c_makeupTable[j + 1][i]);
        return lo + fy * (hi - lo);
    }

    void RoarModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        m_fs2 = 2.0 * m_fs;
        (void) table();   // build the shared curve table here, never on the audio thread

        m_fade.prepare (m_fs, maxBlock, getLatencySamples());

        m_env1Attack = dsp::onePoleCoeff (c_env1AttackSeconds, m_fs2);
        m_env1Release = dsp::onePoleCoeff (c_env1ReleaseSeconds, m_fs2);
        m_env2Attack = dsp::onePoleCoeff (c_env2AttackSeconds, m_fs2);
        m_env2Release = dsp::onePoleCoeff (c_env2ReleaseSeconds, m_fs2);

        const double controlRate = m_fs / static_cast<double> (c_controlBlock);
        m_smoothCoeff = dsp::onePoleCoeff (c_smoothSeconds, controlRate);
        m_vuCoeff = dsp::onePoleCoeff (c_vuSeconds, controlRate);

        for (auto& c : m_ch)
        {
            c.miller1.prepare (m_fs2);
            c.miller2.prepare (m_fs2);
            c.miller2.setCutoff (c_miller2Hz);
            c.outLp.prepare (m_fs);
            c.fixedTop.setCoeffs (Biquad::highShelf (m_fs2, c_fixedTopHz, Biquad::c_butterworthQ, c_fixedTopDb));
            c.outHp.setCoeffs (Biquad::highpass (m_fs, c_outHpHz, c_outHpQ));
        }

        reset();
    }

    void RoarModule::reset()
    {
        m_fade.reset();
        resetWet();
        m_snap = true;
        m_designedA = -1.0;
        m_designedTone = -9.0;
        m_designedMic = -1.0;
        m_ctlPos = 0;
    }

    void RoarModule::resetWet()
    {
        const auto& t = table();
        for (auto& c : m_ch)
        {
            c.micBell.reset();
            c.micShelf.reset();
            c.os.reset();
            c.miller1.reset();
            c.miller2.reset();
            t.resetState (c.adaa1, -1.0);
            t.resetState (c.adaa2, -1.0);
            c.couplingTreble.reset();
            c.bass.reset();
            c.scoop.reset();
            c.trebleTop.reset();
            c.fixedTop.reset();
            c.outLp.reset();
            c.outHp.reset();
            c.bypass1 = c.bypass2 = 0.0;
        }
        m_env1 = m_env2 = 0.0;
        m_swingPeak = 0.0;
        m_vu1 = m_vu2 = 0.0;
        m_heat = 0.0f;
        m_blockingDb = 0.0f;
    }

    void RoarModule::setParams (const RoarSettings& s)
    {
        m_fade.setOn (s.on);
        m_targetA = std::clamp (static_cast<double> (dsp::finiteOrZero (s.drive)), 0.0, 1.0);
        m_targetTone = std::clamp (static_cast<double> (dsp::finiteOrZero (s.tone)), -1.0, 1.0);
        m_targetMic = s.voicing == Params::RoarVoicing::mic ? 1.0 : 0.0;
    }

    void RoarModule::designFilters()
    {
        const double a = m_a, t = m_tone;

        if (a != m_designedA || t != m_designedTone)
        {
            const double tilt = laws::roarToneStrength (a) * t;
            const double bassDb = c_bassMaxDb * (a - tilt);
            const double trebleDb = c_trebleMaxDb * (a + tilt);
            const double scoopDb = laws::roarScoopDb (a);

            const auto couplingTreble = cascade1 (Biquad::highpass1 (m_fs2, c_couplingHz),
                                                  Biquad::highShelf1 (m_fs2, c_trebleLowHz, c_trebleLowShare * trebleDb));
            const auto bass = cascade1 (Biquad::lowShelf1 (m_fs2, c_bassLowHz, c_bassLowShare * bassDb),
                                        Biquad::lowShelf1 (m_fs2, c_bassHighHz, (1.0 - c_bassLowShare) * bassDb));
            const auto scoop = Biquad::peaking (m_fs2, c_scoopHz, c_scoopQ, scoopDb);
            const auto trebleTop = Biquad::highShelf (m_fs2, c_trebleHighHz, Biquad::c_butterworthQ, (1.0 - c_trebleLowShare) * trebleDb);
            const double miller1Hz = c_miller1Hz * std::pow (0.5, a);
            const double outLpHz = c_outLpHz * std::pow (c_outLpPerDrive, a);

            for (auto& c : m_ch)
            {
                c.couplingTreble.setCoeffs (couplingTreble);
                c.bass.setCoeffs (bass);
                c.scoop.setCoeffs (scoop);
                c.trebleTop.setCoeffs (trebleTop);
                c.miller1.setCutoff (miller1Hz);
                c.outLp.setCutoff (outLpHz);
            }
            m_designedA = a;
            m_designedTone = t;
        }

        if (m_mic != m_designedMic)
        {
            const auto bell = Biquad::peaking (m_fs, c_micBellHz, c_micBellQ, c_micBellDb * m_mic);
            const auto shelf = Biquad::highShelf (m_fs, c_micShelfHz, Biquad::c_butterworthQ, c_micShelfDb * m_mic);
            for (auto& c : m_ch)
            {
                c.micBell.setCoeffs (bell);
                c.micShelf.setCoeffs (shelf);
            }
            m_designedMic = m_mic;
        }
    }

    void RoarModule::updateControl()
    {
        if (m_snap)
        {
            m_a = m_targetA;
            m_tone = m_targetTone;
            m_mic = m_targetMic;
        }
        else
        {
            const auto glide = [this] (double& v, double target)
            {
                v += m_smoothCoeff * (target - v);
                if (std::abs (target - v) < 1.0e-6)
                    v = target;
            };
            glide (m_a, m_targetA);
            glide (m_tone, m_targetTone);
            glide (m_mic, m_targetMic);
        }

        designFilters();
        m_kb1 = c_kb1Low + (c_kb1High - c_kb1Low) * m_a;

        Control next;
        next.drive1 = drive1For (m_a);
        next.drive2 = drive2For (m_a);
        next.makeup = dsp::dbToGain (makeupDb (m_a, m_tone));
        next.bypass2 = 1.0 - c_bypass2PerDrive * m_a;
        m_from = m_snap ? next : m_to;
        m_to = next;
        m_snap = false;
    }

    void RoarModule::updateHeat()
    {
        // Stage-1 small-signal gain at the shifted bias, relative to rest: the blocking GR.
        const auto& t = table();
        const double b = bias1();
        constexpr double h = 1.0e-3;
        const double slope = (t.curve (-b + h) - t.curve (-b - h)) / (2.0 * h);
        const double blockDb = std::min (0.0, dsp::gainToDb (std::max (slope, 1.0e-6)));

        // How close the grid swings to conduction, the grid-current envelope, and the blocking.
        const double raw = 0.5 * std::min (m_swingPeak, 1.5) + 1.5 * m_env1 - blockDb / 6.0;
        m_swingPeak = 0.0;
        m_vu1 += m_vuCoeff * (raw - m_vu1);
        m_vu2 += m_vuCoeff * (m_vu1 - m_vu2);
        m_vu1 = dsp::flushTiny (m_vu1);
        m_vu2 = dsp::flushTiny (m_vu2);
        m_heat = static_cast<float> (1.0 - std::exp (-1.6 * std::max (0.0, m_vu2)));
        m_blockingDb = static_cast<float> (blockDb);
    }

    double RoarModule::bias1() const
    {
        const double shift = m_kb1 * m_env1;
        return 1.0 + shift / (1.0 + shift / c_shift1Max);
    }

    void RoarModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& /*voice*/)
    {
        m_fade.captureDry (l, r, n, stereo);
        if (! m_fade.isActive())
        {
            m_fade.outputDry (l, r, n, stereo);
            m_heat = 0.0f;
            m_blockingDb = 0.0f;
            return;
        }
        if (m_fade.consumeRestart())
        {
            // The wet starts again from clean state, on a fresh control grid with the settings
            // snapped, so the result never depends on where the host's blocks fell while it was off.
            resetWet();
            m_snap = true;
            m_ctlPos = 0;
        }

        const auto& tab = table();
        const int numCh = stereo ? 2 : 1;
        float* io[2] = { l, r };
        constexpr double invBlock = 1.0 / static_cast<double> (c_controlBlock);
        bool broken = false;

        for (int i = 0; i < n; ++i)
        {
            if (m_ctlPos == 0)
                updateControl();

            const double w = static_cast<double> (m_ctlPos + 1) * invBlock;
            const double d1 = m_from.drive1 + w * (m_to.drive1 - m_from.drive1);
            const double d2 = m_from.drive2 + w * (m_to.drive2 - m_from.drive2);
            const double makeup = m_from.makeup + w * (m_to.makeup - m_from.makeup);
            const double bypass2 = m_from.bypass2 + w * (m_to.bypass2 - m_from.bypass2);
            const double inv1 = 1.0 / d1, inv2 = 1.0 / d2;
            const auto fd1 = static_cast<float> (d1);
            const auto fd2 = static_cast<float> (d2);

            float up[2][2];
            for (int c = 0; c < numCh; ++c)
            {
                auto& ch = m_ch[static_cast<size_t> (c)];
                double x = std::clamp (dsp::finiteOrZero (static_cast<double> (io[c][i])), -c_inputLimit, c_inputLimit);
                x = ch.micShelf.process (ch.micBell.process (x));
                ch.os.upsample (static_cast<float> (x), up[c][0], up[c][1]);
            }

            for (int k = 0; k < 2; ++k)
            {
                // ---- Triode 1 (shared bias envelope)
                const double b1 = bias1();
                const double rest1 = tab.curve (-b1);
                double grid1 = 0.0;
                double v[2];
                for (int c = 0; c < numCh; ++c)
                {
                    auto& ch = m_ch[static_cast<size_t> (c)];
                    const float driven = up[c][k] * fd1;
                    const double s = static_cast<double> (ch.miller1.processLowpass (driven));
                    m_swingPeak = std::max (m_swingPeak, std::abs (s));
                    const double g = std::clamp (s - b1, -c_gridLimit, c_gridLimit);
                    grid1 = std::max (grid1, g);
                    // Miller bypass, through the 2-tap average ADAA applies to the curve path.
                    const double highs = std::clamp (static_cast<double> (driven) - s, -c_gridLimit, c_gridLimit);
                    const double aligned = 0.5 * (highs + ch.bypass1);
                    ch.bypass1 = highs;
                    v[c] = (tab.process (ch.adaa1, g) - rest1 + aligned) * inv1;
                }
                {
                    const double gc = std::min (grid1, c_gridCurrentMaxG);
                    const double ig = gc > 0.0 ? gc * std::sqrt (gc) : 0.0;
                    m_env1 += (ig > m_env1 ? m_env1Attack : m_env1Release) * (ig - m_env1);
                    m_env1 = dsp::flushTiny (m_env1);
                }

                // ---- Tone stack, triode 2 (shared bias envelope)
                const double b2 = 1.0 + c_kb2 * m_env2;
                const double rest2 = tab.curve (-b2);
                double grid2 = 0.0;
                for (int c = 0; c < numCh; ++c)
                {
                    auto& ch = m_ch[static_cast<size_t> (c)];
                    double e = ch.couplingTreble.process (v[c]);
                    e = ch.bass.process (e);
                    e = ch.scoop.process (e);
                    e = ch.trebleTop.process (e);
                    e = ch.fixedTop.process (e);
                    const float driven = static_cast<float> (e) * fd2;
                    const double s = static_cast<double> (ch.miller2.processLowpass (driven));
                    const double g = std::clamp (s - b2, -c_gridLimit, c_gridLimit);
                    grid2 = std::max (grid2, g);
                    const double highs = std::clamp (static_cast<double> (driven) - s, -c_gridLimit, c_gridLimit);
                    const double aligned = 0.5 * (highs + ch.bypass2);
                    ch.bypass2 = highs;
                    up[c][k] = static_cast<float> ((tab.process (ch.adaa2, g) - rest2 + bypass2 * aligned) * inv2);
                }
                {
                    const double gc = std::min (grid2, c_gridCurrentMaxG);
                    const double ig = gc > 0.0 ? gc * std::sqrt (gc) : 0.0;
                    m_env2 += (ig > m_env2 ? m_env2Attack : m_env2Release) * (ig - m_env2);
                    m_env2 = dsp::flushTiny (m_env2);
                }
            }

            for (int c = 0; c < numCh; ++c)
            {
                auto& ch = m_ch[static_cast<size_t> (c)];
                float y = ch.os.downsample (up[c][0], up[c][1]);
                y = ch.outLp.processLowpass (y);
                const double out = ch.outHp.process (static_cast<double> (y)) * makeup;
                if (! std::isfinite (out))
                    broken = true;
                io[c][i] = static_cast<float> (out);
            }

            if (++m_ctlPos == c_controlBlock)
            {
                m_ctlPos = 0;
                updateHeat();
            }
        }

        if (broken || ! std::isfinite (m_env1) || ! std::isfinite (m_env2))
        {
            // Never let a non-finite state leak: restart the wet path, output the silence it gives.
            resetWet();
            for (int c = 0; c < numCh; ++c)
                std::fill (io[c], io[c] + n, 0.0f);
        }

        m_fade.apply (l, r, n, stereo);
    }

    void RoarModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<RoarModule> (other);
        m_fade.copyStateFrom (o.m_fade);
        m_ch = o.m_ch;   // plain value types: filters with their coefficients, oversamplers, ADAA states

        m_targetA = o.m_targetA;
        m_targetTone = o.m_targetTone;
        m_targetMic = o.m_targetMic;
        m_a = o.m_a;
        m_tone = o.m_tone;
        m_mic = o.m_mic;
        m_designedA = o.m_designedA;
        m_designedTone = o.m_designedTone;
        m_designedMic = o.m_designedMic;
        m_snap = o.m_snap;
        m_from = o.m_from;
        m_to = o.m_to;
        m_ctlPos = o.m_ctlPos;
        m_kb1 = o.m_kb1;
        m_env1 = o.m_env1;
        m_env2 = o.m_env2;
        m_swingPeak = o.m_swingPeak;
        m_vu1 = o.m_vu1;
        m_vu2 = o.m_vu2;
        m_heat = o.m_heat;
        m_blockingDb = o.m_blockingDb;
    }

    void RoarModule::copyLeftStateToRight()
    {
        m_ch[1] = m_ch[0];
        m_fade.copyLeftStateToRight();
    }

    void RoarModule::publishFeatures (MonsterFeatures& features) const
    {
        features.roarHeat.store (getHeat(), std::memory_order_relaxed);
    }
}

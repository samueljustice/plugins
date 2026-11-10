#pragma once

#include "../Params.h"
#include "common/Biquad.h"
#include "common/DspMath.h"
#include "common/LR8Split.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace roar
{
    /**
     * The dry for a partial Mix, given the phase of the wet chain's fixed filters, so a blend of
     * the two does not comb. The dry keeps its full band and level; only its phase follows the
     * wet's fixed-phase filters that are in use:
     *
     *   always    OUTPUT HPF (Butterworth-4 at 30 Hz)       ~ 2nd-order allpass, Q 0.841 (within 2.7 deg)
     *             OUTPUT LPF (Butterworth-2 at the LPF)     ~ 1st-order allpass (within 10 deg)
     *             TAME's split (AP4 at 2.5 kHz x AP4 at 6.5 kHz): exact, TAME runs its split even
     *             when Off, so its phase never jumps
     *   CLEAN On  the tone's high-pass: 2nd order (Flat 40, Voice 70, Heavy 90 Hz)  ~ -(1st-order allpass);
     *             Radio (300 Hz and 3.5 kHz, both Butterworth-4)                    ~ 2nd-order allpasses, Q 0.841
     *   OVERTONE  On in Lean or Replace: its LR4 split at fx sums to the Butterworth-2 allpass at fx (exact)
     *
     * The fits: a Butterworth-N corner has phase N 90 deg - arg B(jw); an allpass of order N/2
     * with denominator D has -2 arg D(jw), so D is chosen with arg D ~ arg B / 2 (a high-pass of
     * order 2 needs the sign flip). Every variant runs all the time and each optional section is
     * blended by its own 20 ms smoothstep weight, so turning CLEAN or OVERTONE on or off, or a
     * tone change, glides from one dry variant to the other.
     *
     * Not matched: CHAMBER's Boom, the RBJ low shelf at 110 Hz (Q 0.7, +3/+6/+9 dB) on the wet
     * after the last module. A minimum-phase shelf's phase is a lag bump that returns to 0 deg
     * above the shelf (-13.8/-27.3/-40.3 deg at 110 Hz, about -22 deg at 250 Hz on Heavy), and
     * no allpass can follow it: an allpass's phase only ever falls (to -180 or -360 deg at the
     * top), so a fit near 110 Hz would leave the dry 90-180 deg off through the rest of the
     * voice band, a far worse comb. Left alone the cost is small, because the shelf also lifts
     * the wet where it lags: at 50 % Mix the worst loss against an in-phase blend is about
     * 0.5 dB on Heavy, and the [mix] test bounds it.
     *
     * One instance per chain (it depends on settings, never on the order). No allocation, NaN-safe.
     */
    class DryPhaseMatch
    {
    public:
        static constexpr double c_fadeSeconds = 0.020;
        static constexpr double c_bw4AllpassQ = 0.841;
        static constexpr double c_outputHighPassHz = 30.0;
        static constexpr double c_tameLowHz = 2500.0, c_tameHighHz = 6500.0;
        static constexpr int c_numTones = 4;

        struct Settings
        {
            bool cleanOn = true;
            Params::CleanTone tone = Params::CleanTone::voice;
            bool overtoneSplit = false;   // OVERTONE On in Lean or Replace
            double overtoneFxHz = 120.0;
        };

        void prepare (double fs, double outputLowPassHz)
        {
            m_fs = dsp::safeSampleRate (fs);
            m_fadeStep = 1.0f / static_cast<float> (std::max (1, dsp::samplesFor (c_fadeSeconds, m_fs)));
            for (auto& c : m_ch)
            {
                c.outHp.setCoeffs (Biquad::allpass (m_fs, c_outputHighPassHz, c_bw4AllpassQ));
                c.outLp.setCoeffs (Biquad::allpass1 (m_fs, std::min (outputLowPassHz, 0.45 * m_fs)));
                c.tameLow.prepare (m_fs, c_tameLowHz);
                c.tameHigh.prepare (m_fs, c_tameHighHz);
                c.cleanAp[0].setCoeffs (Biquad::allpass1 (m_fs, 40.0));
                c.cleanAp[1].setCoeffs (Biquad::allpass1 (m_fs, 70.0));
                c.cleanAp[2].setCoeffs (Biquad::allpass1 (m_fs, 90.0));
                c.radioHp.setCoeffs (Biquad::allpass (m_fs, 300.0, c_bw4AllpassQ));
                c.radioLp.setCoeffs (Biquad::allpass (m_fs, 3500.0, c_bw4AllpassQ));
            }
            m_fxHz = -1.0;
            setOvertoneFx (120.0);
            reset (m_settings);
        }

        /** Clears the filters and snaps the variant weights to the settings. */
        void reset (const Settings& s)
        {
            for (auto& c : m_ch)
                resetChannel (c);
            setSettings (s);
            m_cleanWeights = m_cleanTargets;
            m_overtoneWeight = m_overtoneTarget;
        }

        /** Per chunk. */
        void setSettings (const Settings& s)
        {
            m_settings = s;
            m_cleanTargets.fill (0.0f);
            if (s.cleanOn)
                m_cleanTargets[static_cast<size_t> (std::clamp (static_cast<int> (s.tone), 0, c_numTones - 1))] = 1.0f;
            m_overtoneTarget = s.overtoneSplit ? 1.0f : 0.0f;
            setOvertoneFx (s.overtoneFxHz);
        }

        /** In place on l (and r when stereo); r is ignored otherwise. */
        void process (float* l, float* r, int n, bool stereo)
        {
            for (int i = 0; i < n; ++i)
            {
                // Linear weight glides, shaped by a smoothstep per variant.
                float none = 1.0f;
                std::array<float, c_numTones> w {};
                for (size_t t = 0; t < w.size(); ++t)
                {
                    m_cleanWeights[t] = glide (m_cleanWeights[t], m_cleanTargets[t]);
                    w[t] = dsp::smoothstep01 (m_cleanWeights[t]);
                    none -= w[t];
                }
                none = std::max (0.0f, none);
                m_overtoneWeight = glide (m_overtoneWeight, m_overtoneTarget);
                const float wo = dsp::smoothstep01 (m_overtoneWeight);

                l[i] = processSample (m_ch[0], l[i], none, w, wo);
                if (stereo)
                    r[i] = processSample (m_ch[1], r[i], none, w, wo);
            }
        }

        /** Complex response of the current (settled) variant at hz, for tests. */
        std::complex<double> response (double hz) const
        {
            const auto& c = m_ch[0];
            std::complex<double> h = c.outHp.getCoeffs().response (hz, m_fs) * c.outLp.getCoeffs().response (hz, m_fs)
                                   * c.tameLow.response (hz, m_fs) * c.tameHigh.response (hz, m_fs);
            if (m_settings.cleanOn)
            {
                const auto t = static_cast<int> (m_settings.tone);
                if (t == static_cast<int> (Params::CleanTone::radio))
                    h *= c.radioHp.getCoeffs().response (hz, m_fs) * c.radioLp.getCoeffs().response (hz, m_fs);
                else
                    h *= -c.cleanAp[static_cast<size_t> (std::clamp (t, 0, 2))].getCoeffs().response (hz, m_fs);
            }
            if (m_settings.overtoneSplit)
                h *= c.overtoneAp.getCoeffs().response (hz, m_fs);
            return h;
        }

    private:
        struct Channel
        {
            Biquad outHp, outLp;
            Allpass4 tameLow, tameHigh;
            std::array<Biquad, 3> cleanAp;   // Flat, Voice, Heavy
            Biquad radioHp, radioLp;
            Biquad overtoneAp;
        };

        static void resetChannel (Channel& c)
        {
            c.outHp.reset();
            c.outLp.reset();
            c.tameLow.reset();
            c.tameHigh.reset();
            for (auto& f : c.cleanAp)
                f.reset();
            c.radioHp.reset();
            c.radioLp.reset();
            c.overtoneAp.reset();
        }

        float glide (float v, float target) const
        {
            return target > v ? std::min (target, v + m_fadeStep) : std::max (target, v - m_fadeStep);
        }

        static float processSample (Channel& c, float in, float none, const std::array<float, c_numTones>& w, float wo)
        {
            double x = static_cast<double> (dsp::finiteOrZero (in));
            x = c.outHp.process (x);
            x = c.outLp.process (x);
            x = c.tameHigh.process (c.tameLow.process (x));

            // CLEAN variants (every one runs, so a change never starts from cold state).
            const double flat = -c.cleanAp[0].process (x);
            const double voice = -c.cleanAp[1].process (x);
            const double heavy = -c.cleanAp[2].process (x);
            const double radio = c.radioLp.process (c.radioHp.process (x));
            x = static_cast<double> (none) * x + static_cast<double> (w[0]) * flat + static_cast<double> (w[1]) * voice
              + static_cast<double> (w[2]) * heavy + static_cast<double> (w[3]) * radio;

            const double ov = c.overtoneAp.process (x);
            x += static_cast<double> (wo) * (ov - x);

            if (! std::isfinite (x))
            {
                // Should never happen (the input is sanitised); restart the filters rather than poison them.
                resetChannel (c);
                return 0.0f;
            }
            return static_cast<float> (x);
        }

        void setOvertoneFx (double hz)
        {
            hz = std::clamp (std::isfinite (hz) ? hz : 120.0, 20.0, 0.45 * m_fs);
            if (std::abs (hz - m_fxHz) < 1.0e-6 * hz)
                return;
            m_fxHz = hz;
            for (auto& c : m_ch)
                c.overtoneAp.setCoeffs (Biquad::allpass (m_fs, hz, Biquad::c_butterworthQ));
        }

        double m_fs = 48000.0;
        float m_fadeStep = 1.0f / 960.0f;
        double m_fxHz = -1.0;
        Settings m_settings;
        std::array<Channel, 2> m_ch;
        std::array<float, c_numTones> m_cleanWeights {}, m_cleanTargets {};
        float m_overtoneWeight = 0.0f, m_overtoneTarget = 0.0f;
    };
}

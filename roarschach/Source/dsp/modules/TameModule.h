#pragma once

#include "../common/LR8Split.h"
#include "../common/Module.h"

#include <array>
#include <vector>

namespace roar
{
    struct TameSettings
    {
        bool on = true;
        float amount = 0.50f;   // t, 0..1
    };

    /**
     * TAME (spec §5.12): top-band-only multiband containment.
     *
     *   x -> LR8 @ 2.5 kHz -> L, U;  U -> LR8 @ 6.5 kHz -> HM, H;  L -> Allpass4 @ 6.5 kHz
     *   y = L + g_HM HM + g_H H          (g = 1: y = AP_A AP_B x, flat, zero latency)
     *
     * Per band (HM and H only):
     *   - Smart detector: peak (instant attack, 60 ms release) and RMS (25 ms) in dB, blended by
     *     w = smoothstep (Pk - Rm; 6, 14 dB).
     *   - Slope s: ratio R = 1.5 (20 / 1.5)^s, knee W = (1 - s) |D| / 2, depth ceiling
     *     GR = -|D| tanh (-g / |D|). No makeup.
     *   - Attack, plus a dual release: the fast one, and a slow one at 6x that charges once the
     *     reduction has been held (not releasing) for 100 ms; the deeper of the two is applied.
     *   - Amount t = 5/3 u (laws::tameT, u = the knob): HM T = -24 - 12t dBFS, |D| = 12t dB, s 0.45,
     *               4 / 140 ms (measured release); H  T = -30 - 12t dBFS, |D| = 16t dB, s 0.55, 2 / 90 ms.
     *     So at 100 %: HM -44 dBFS, 20 dB; H -50 dBFS, 26.7 dB. t = 0 gives no reduction (the
     *     module is the allpass).
     *
     * Off still runs the split and the sum, so the phase (AP_A AP_B, matched by DryPhaseMatch)
     * never jumps; only the band gains fade to 1. Linked detection (max of L and R).
     */
    class TameModule final : public Module
    {
    public:
        static constexpr double c_crossoverLowHz = 2500.0;    // AP_A
        static constexpr double c_crossoverHighHz = 6500.0;   // AP_B

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return 0; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const TameSettings& s);

        /** Current band gain reductions in dB (<= 0); getGrDb() is the deeper of the two. */
        float getHighMidGrDb() const { return static_cast<float> (m_bands[0].grDb); }
        float getHighGrDb() const { return static_cast<float> (m_bands[1].grDb); }
        float getGrDb() const { return static_cast<float> (std::min (m_bands[0].grDb, m_bands[1].grDb)); }

        /** Test hook: force the band gains (dB) instead of the dynamics; NaN-free values only. */
        void setForcedGainsForTest (bool forced, double highMidDb = 0.0, double highDb = 0.0);

        /** Static gain reduction (dB) of a band for detector level x (dB) at curve position t (0..5/3). band 0 = HM, 1 = H. */
        static double staticCurveDb (int band, double t, double levelDb);

    private:
        struct BandDynamics
        {
            // fixed per band
            double baseThreshold = -24.0, thresholdPerT = -12.0, depthPerT = 12.0, slopeS = 0.45;
            double attackCoeff = 0.0, releaseCoeff = 0.0, slowReleaseCoeff = 0.0;
            // detector
            double peak = 0.0, peakRelease = 0.0;
            double ms = 0.0, msCoeff = 0.0;
            // ballistics
            double fast = 0.0, slow = 0.0;
            int heldSamples = 0;
            double grDb = 0.0;

            void resetState()
            {
                peak = ms = 0.0;
                fast = slow = 0.0;
                heldSamples = 0;
                grDb = 0.0;
            }

            void copyStateFrom (const BandDynamics& o)
            {
                peak = o.peak;
                ms = o.ms;
                fast = o.fast;
                slow = o.slow;
                heldSamples = o.heldSamples;
                grDb = o.grDb;
            }
        };

        struct Channel
        {
            LR8Split splitA, splitB;
            Allpass4 lowAllpass;

            void copyStateFrom (const Channel& o)
            {
                splitA.copyStateFrom (o.splitA);
                splitB.copyStateFrom (o.splitB);
                lowAllpass.copyStateFrom (o.lowAllpass);
            }
        };

        void resetWet();
        double runBand (BandDynamics& b, double power, double absPeak);

        double m_fs = 48000.0;
        OnFade m_fade;
        std::array<Channel, 2> m_ch;
        std::array<BandDynamics, 2> m_bands;   // 0 = HM, 1 = H

        double m_amountTarget = 0.5, m_amount = 0.5, m_amountCoeff = 0.0;
        int m_holdSamples = 4800;

        bool m_forced = false;
        double m_forcedHm = 0.0, m_forcedH = 0.0;

        // Band signals of the current chunk: [channel][band][sample].
        std::vector<float> m_bandBuffer;
        int m_maxBlock = 0;
    };
}

#pragma once

#include "../common/DelayLine.h"
#include "../common/Module.h"
#include "../common/SlidingExtreme.h"
#include "../../Params.h"

namespace roar
{
    struct DetailSettings
    {
        bool on = true;
        float low = 0.60f;                                       // u, 0..1
        float high = 0.30f;                                      // u, 0..1
        Params::DetailSpeed speed = Params::DetailSpeed::natural;
    };

    /**
     * DETAIL (spec §5.5): an upward + push-into-ceiling level compressor. Breaths, consonants
     * and tails come up a long way while the loud syllables stay where they are.
     *
     *   x --> look-ahead delay D --------------------------------------------> x dB(G) --> out
     *     \-> a = max |L|,|R| --> sliding max over D+1 --> fast env (instant attack, 0.7 ms)
     *         --> dB --> slow env (charge 95 ms, release 500 ms Natural / 80 ms Snappy)
     *         --> level = max (fast, slow)
     *
     *   Low (upward)    G_low  = L / (1 + exp ((level - (c + 3.01)) / w)),  L = 48 uLow^1.68,
     *                   c = -34 + 0.52 L, w = 8.5 + 0.08 L
     *   High (push)     xp = level + G_low, y = softmin (xp + M, C) with a 9 dB knee,
     *                   M = 48 uHigh^1.68, C = max (0, 6 - 0.28 M); G_high = y - xp, whose
     *                   reduction releases through its own slow stage (60 ms charge, 150 ms release)
     *   Trim            T = the static G_low + G_high at a -12 dBFS peak, so syllables at the
     *                   reference level pass at unity and only the quiet material rises
     *   Floor guard     a stationary section (the 10 ms RMS stays within 3 dB for 0.4 s) is taken
     *                   as the floor F: it falls to a quieter one quickly and rises at 3 dB/s. Breath
     *                   gaps between syllables are too short to count. Material within 2 dB of F gets
     *                   no net boost, the boost comes in fully 8 dB above F, and below F the expander
     *                   cuts 1:2 (at most 6 dB). Until anything louder than -40 dBFS has been heard,
     *                   quiet material from the start counts as floor at once.
     *   Gain smoother   instant when falling (the look-ahead covers it), 1.5 ms when rising.
     *
     * Latency round (1.333 ms fs): 64 samples at 48 kHz. Stereo-linked (one gain from the max of
     * both channels). Low 0 and High 0 is an exact delay. No allocation after prepare(), NaN-safe.
     */
    class DetailModule : public Module
    {
    public:
        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return m_lookahead; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const DetailSettings& s);

        /**
         * Test hooks: the trim and the floor guard can be switched off so the curves compare
         * directly with a plain upward compressor measurement. Both are on in the plug-in.
         */
        void setTestOptions (bool trim, bool floorGuard);

        // Readouts (end of the last chunk, scaled by the On fade).
        float getBoostDb() const { return m_s.boostDb * m_fade.getValue(); }   // >= 0
        float getCutDb() const { return m_s.cutDb * m_fade.getValue(); }       // <= 0
        float getGainDb() const { return m_s.gainReadDb * m_fade.getValue(); }
        float getFloorDb() const { return static_cast<float> (m_s.floorDb); }
        double getTrimDb() const { return m_curve.trim; }

        /** The low and high section depths in dB for knob positions u (the 48 u^1.68 taper). */
        static double depthForKnob (double u);

        /** The static gain (no dynamics, no floor guard) for a steady peak level in dBFS. */
        double staticGainDb (double peakDb, bool withTrim) const;

    private:
        void updateCurve();
        void controlBlock();
        void resetWet();
        static double softMin (double a, double b);
        double lowGain (double level) const;
        double highGainStatic (double xp) const;

        // Constants of prepare() (not part of the cloned state: both instances are prepared alike).
        double m_fs = 48000.0;
        int m_lookahead = 64;
        double m_fastRelease = 0.0;                              // pole
        double m_slowCharge = 0.0, m_slowReleaseNatural = 0.0, m_slowReleaseSnappy = 0.0;
        double m_highCharge = 0.0, m_highRelease = 0.0;
        double m_gainRise = 0.0;
        double m_msCoeff = 0.0;
        double m_paramCoeff = 0.0;                               // per control block
        double m_levReleaseCoeff = 0.0, m_floorFallCoeff = 0.0, m_floorRiseDb = 0.0;
        int m_holdBlocks = 1, m_qualifyBlocks = 1;

        static constexpr int c_controlBlock = 32;

        OnFade m_fade;
        DelayLine m_delayL, m_delayR;
        SlidingExtreme m_peak { SlidingExtreme::Kind::max };

        // The static curve for the current (smoothed) knob positions.
        struct Curve
        {
            double L = 0.0, M = 0.0;           // low and high depths, dB
            double mid = 0.0, invW = 1.0;      // logistic midpoint (peak dB) and 1 / width
            double C = 6.0;                    // ceiling of the high section, dB
            double highScale = 0.0;            // fades the high section out below M = 3 dB
            double trim = 0.0;                 // T, dB
        };

        // Every piece of running state (plain values: copied by assignment in copyStateFrom).
        struct State
        {
            double lowTarget = 0.60, highTarget = 0.30;
            double lowU = 0.60, highU = 0.30;
            bool snappy = false;
            bool trimOn = true, floorOn = true;
            bool snapParams = true;

            double fast = 0.0;                 // linear peak envelope
            double slowDb = -120.0;
            double highSlow = 0.0;             // slow stage of the high section's reduction, dB
            double gainDb = 0.0;               // smoothed total gain

            // Floor guard: 10 ms mean square, evaluated per control block on the sample clock.
            double ms = 0.0;
            int blockPhase = 0;
            double levHeldDb = -150.0;
            int holdLeft = 0;
            double runRefDb = -150.0, runSumDb = -150.0;
            int runBlocks = 0;
            double floorDb = -70.0;            // c_unknownFloorDb until a floor has been heard
            bool floorKnown = false, activitySeen = false;
            double floorWeight = 0.0;          // share of the net boost the guard removes, 0..1
            double floorCutDb = 0.0;           // 1:2 expansion below the floor, dB >= 0

            float boostDb = 0.0f, cutDb = 0.0f, gainReadDb = 0.0f;
        };

        Curve m_curve;
        State m_s;
    };
}

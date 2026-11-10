#pragma once

#include <vector>

#include "../../Params.h"
#include "../common/Biquad.h"
#include "../common/LogBallistics.h"
#include "../common/Module.h"
#include "../common/SlidingExtreme.h"

#include <array>

namespace roar
{
    struct CleanSettings
    {
        bool on = true;
        float contain = 0.50f;                                // u, 0..1
        float gate = 0.45f;                                   // u, 0..1
        Params::CleanTone tone = Params::CleanTone::voice;
    };

    /**
     * CLEAN (spec §5.3): the head of a channel strip.
     *
     *   HPF -> Tone EQ -> expander (gate) -> leveller (Avg) + static makeup -> peak catcher (Peak + 10 ms hold)
     *
     *   - Tone (chip) picks the HPF and EQ: Flat / Voice / Heavy / Radio. A tone change crossfades
     *     the previous and the new filter bank over 20 ms.
     *   - Gate: depth -30 u dB, ratio 1:(1.5 + 3.5 u) (so the depth is reached), knee 6 dB, hysteresis 4 dB (opens above T + 4, closes
     *     below T after a 60 ms hold), open 1 ms, close 150 ms. T = floor + O dB in [-72 + O, -50 + O] dBFS,
     *     O = 6 + 12 u (laws::gateThresholdOffsetDb): higher settings also close on the shorter, louder gaps;
     *     the floor (the mean room level) is learnt only while the gate is closed. A voiced frame
     *     with confidence > 0.6 holds the gate open. Closing moves at |depth| per 150 ms. A gate
     *     held open by a steady room (its quietest level of the last 3 s above T) keeps learning
     *     the floor while open, so it cannot stay stuck open.
     *   - Contain: c = u^0.7 (KnobLaws). Leveller T = -20 - 16 c dBFS, ratio 1.4 + 1.2 c, knee 10 dB, 15 / 250 ms, with a
     *     static makeup equal to its gain reduction at the reference level (-20 dBFS). It starts at
     *     that reduction after a reset, holds it while the gate is closed, and releases 20x slower
     *     on unvoiced sound, so a line's onsets are not lifted by the makeup. Peak catcher
     *     T = -6 - 12 c dBFS, 6:1, knee 4 dB, 0.3 / 60 ms on a 10 ms peak hold. Below c = 10 % the
     *     leveller and the catcher fade out, so Contain 0 % is exactly unity (neutral null).
     *
     * Detection is linked (one gain for both channels). Zero latency. Knobs are smoothed (20 ms)
     * and turned into curves every 32 samples on a sample clock, so the output does not depend
     * on the block size.
     */
    class CleanModule final : public Module
    {
    public:
        static constexpr double c_referenceDb = -20.0;
        static constexpr int c_maxSections = 6;
        static constexpr int c_roomSegments = 12;   // gate stuck-open detection: 12 x 250 ms

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return 0; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const CleanSettings& s);

        /**
         * The fixed-phase high-pass of a tone, for DryPhaseMatch (§3.7): writes its sections to
         * out and returns how many (1 for 12 dB/oct, 2 for Radio's 24 dB/oct).
         */
        static int highPassSections (double fs, Params::CleanTone tone, std::array<Biquad::Coeffs, 2>& out);

        /** Static makeup (dB) for a Contain value: minus the leveller's gain reduction at -20 dBFS. */
        static double staticMakeupDb (double contain);   // contain: the curve value c (laws::containCurve)

        // Readouts (audio thread values of the last processed sample).
        float getGateOpen() const { return m_gateOpen; }               // 0 closed .. 1 open
        /** The gate's gain (dB, <= 0, scaled by the On fade) per sample of the last process() call:
            n values, valid until the next call. DETAIL reads it to keep from lifting gated room tone. */
        const float* getGateTrace() const { return m_gateTrace.data(); }
        float getGateGainDb() const { return static_cast<float> (m_gateGainDb); }
        float getLevellerGainDb() const { return static_cast<float> (m_levGainDb); }   // GR + makeup
        float getPeakGainDb() const { return static_cast<float> (m_peakGainDb); }
        float getGateThresholdDb() const { return static_cast<float> (m_gateThresholdDb); }
        float getTotalGainDb() const { return static_cast<float> (m_totalGainDb); }
        /** Contain's gain reduction: the leveller's (before its static makeup) plus the catcher's, dB (<= 0). */
        float getContainGrDb() const { return static_cast<float> (std::min (0.0, m_levY + m_peakGainDb)); }

    private:
        struct ToneBank
        {
            std::array<Biquad, c_maxSections> left, right;
            int tone = 1;
        };

        void designTones();
        void loadBank (ToneBank& bank, int tone);
        void updateControl();
        void resetWet();
        static int toneIndex (Params::CleanTone t) { return std::clamp (static_cast<int> (t), 0, 3); }

        double m_fs = 48000.0;
        OnFade m_fade;

        // Tone EQ: designs per tone, two banks for the 20 ms tone crossfade.
        std::array<std::array<Biquad::Coeffs, c_maxSections>, 4> m_toneCoeffs {};
        std::array<int, 4> m_toneSections {};
        std::array<ToneBank, 2> m_banks;
        int m_activeBank = 0;
        int m_requestedTone = 1;
        int m_xfadePos = 0;          // > 0 while crossfading from the other bank to the active one
        int m_xfadeLength = 960;

        // Knobs: targets, smoothed values, derived curves.
        double m_containTarget = 0.5, m_gateTarget = 0.45;
        double m_contain = 0.5, m_gateU = 0.45;
        double m_knobCoeff = 0.0;    // one-pole per control block
        int m_ctrlCountdown = 0;
        double m_gateDepthDb = -18.0, m_gateRatio = 2.5, m_gateOffsetDb = 10.0;
        double m_levThreshold = -28.0, m_levSlope = -0.5;
        double m_peakThreshold = -12.0;
        double m_makeupDb = 0.0;
        double m_engage = 1.0;

        // Gate.
        double m_gateMs = 0.0, m_gateMsCoeff = 0.0;
        double m_floorPower = 1.0e-8, m_floorDb = -80.0, m_floorUpCoeff = 0.0, m_floorDownCoeff = 0.0;
        double m_gateThresholdDb = -62.0;
        bool m_gateIsOpen = false;
        int m_holdCount = 0, m_holdSamples = 2880;
        std::array<double, c_roomSegments> m_roomSegments {};   // detector minima (dB) per segment
        int m_roomSegmentPos = 0, m_roomSegmentLength = 12000, m_roomSegmentsFilled = 0;
        double m_roomSegmentMin = 0.0, m_roomMinDb = 0.0, m_stuckFloorCapPower = 1.0e-5;
        bool m_floorStuck = false;
        double m_gateGainDb = 0.0, m_gateOpenCoeff = 0.0, m_gateCloseRate = 0.0;   // close: |depth| per c_gateCloseSeconds
        float m_gateOpen = 0.0f;
        std::vector<float> m_gateTrace;   // maxBlock

        // Leveller.
        double m_levMs = 0.0, m_levMsCoeff = 0.0;
        double m_levG1 = 0.0, m_levY = 0.0;                    // decoupled-peak ballistics state (dB)
        double m_levAttack = 0.0, m_levRelease = 0.0, m_levReleaseUnvoiced = 0.0;
        double m_levGainDb = 0.0;

        // Peak catcher.
        SlidingExtreme m_peakHold { SlidingExtreme::Kind::max };
        LogBallistics m_peakBallistics;
        double m_peakGainDb = 0.0;

        double m_totalGainDb = 0.0;
    };
}

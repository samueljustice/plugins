#pragma once

#include "../../Params.h"
#include "../common/Biquad.h"
#include "../common/LogBallistics.h"
#include "../common/Module.h"

#include <array>

namespace roar
{
    struct DeessSettings
    {
        bool on = true;
        float amount = 0.50f;                                 // u, 0..1: Range = -24 u dB
        Params::DeessMode mode = Params::DeessMode::split;
        bool roarOn = true;                                   // drive link (filled by the chain)
        float roarDrive = 0.35f;                              // ROAR Drive u, 0..1
    };

    /**
     * DE-ESS (spec §5.4): split-band sibilance control.
     *
     *   Split: low = LP2 (x) (Butterworth, c_splitHz), high = x - low, y = low + g high
     *          = x - (1 - g) high: complementary, so it nulls exactly at 0 dB of gain reduction,
     *          and |y| <= |x| at every frequency (it never boosts).
     *   Wide:  y = g x.
     *
     *   Detector: a 12 dB/oct band-pass centred near 7 kHz (Butterworth HP2 at 4 kHz into LP2 at
     *   12 kHz), so vowel formants below 3 kHz stay out of it. rel = L_band - L_full from peak envelopes
     *   (0.5 ms / 40 ms). GR = clamp (-1.5 (rel + 16), Range, 0) dB, smoothed 0.5 ms / 50 ms;
     *   no action while L_band < -55 dBFS.
     *   Range = -24 u dB; with ROAR On, Range_eff = Range (1 + 0.5 uRoar).
     *
     * The split is defined by the low-pass (not by a high-pass subtracted from the input): a
     * 2nd-order high-pass and its complement are 90 degrees apart around the corner, so reducing
     * that high band barely lowers the sibilance (and lifts 3-5 kHz). With the low-pass defining
     * the split, a -12 dB gain gives -10.5 dB or more from 5 kHz up.
     *
     * Linked detection, zero latency, Split/Wide changes glide over 20 ms.
     */
    class DeessModule final : public Module
    {
    public:
        static constexpr double c_splitHz = 3500.0;
        static constexpr double c_detectorLowHz = 4000.0;    // detector band edges (12 dB/oct each side)
        static constexpr double c_detectorHighHz = 12000.0;

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return 0; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const DeessSettings& s);

        /** Current gain reduction in dB (<= 0). */
        float getGrDb() const { return static_cast<float> (m_grDb); }

        /** Effective range in dB (<= 0) for given settings. */
        static double rangeDbFor (const DeessSettings& s);

    private:
        void resetWet();

        double m_fs = 48000.0;
        OnFade m_fade;

        std::array<Biquad, 2> m_split;      // LP2 per channel
        std::array<std::array<Biquad, 2>, 2> m_detector;   // per channel: HP2, LP2

        double m_rangeTarget = -12.0, m_range = -12.0, m_rangeCoeff = 0.0;
        double m_wideTarget = 0.0, m_wide = 0.0, m_wideCoeff = 0.0;

        double m_envBand = 0.0, m_envFull = 0.0;
        double m_envAttack = 0.0, m_envRelease = 0.0;
        LogBallistics m_grBallistics;
        double m_grDb = 0.0;
    };
}

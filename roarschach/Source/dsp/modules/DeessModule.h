#pragma once

#include "../../Params.h"
#include "../common/Biquad.h"
#include "../common/Module.h"

#include <array>

namespace roar
{
    struct DeessSettings
    {
        bool on = true;
        float amount = 0.50f;                                 // u, 0..1 (laws::deessRangeDb, laws::deessThresholdShiftDb)
        Params::DeessMode mode = Params::DeessMode::split;
    };

    /**
     * DE-ESS (spec §5.4): sibilance control by a dynamic high shelf, acting on sibilant events
     * measured against the voice's own top-end balance.
     *
     *   Split: two cascaded 2nd-order TPT state-variable high shelves at 2.8 kHz (k = sqrt 2), each
     *          of gain sqrt (G): a 4th-order shelf, unity below the corner, G above it. The filter
     *          states never depend on G; at 0 dB the output is the input sample for sample, and the
     *          gain is <= 1 at every frequency (it never boosts).
     *   Wide:  y = G^0.8 x (0.8 of the reduction on the whole signal). After 0.3 s of continuous
     *          reduction the depth relaxes (0.15 s), so a long bright sound is not ducked throughout.
     *
     *   Detector: a 12 dB/oct band-pass (Butterworth HP2 at 4 kHz into LP2 at 12 kHz), so vowel
     *   formants stay out of it. rel = L_band - L_full from peak envelopes (0.5 ms / 20 ms).
     *   The voice's balance ref: the mean of rel (dB, clamped -40..0) over the sounding part of the
     *   last 2.5 s (a cumulative mean for the first 2.5 s after a reset, starting at -21 dB); while a
     *   reduction of more than 1 dB has been held for over 0.3 s (longer than an ess) it learns
     *   with a 0.15 s memory. Sounding: within 30 dB of a loud end falling 3 dB/s, above -60 dBFS.
     *   Threshold T = min (max (-16 - S, ref + 5 - 2.5 S), -6 - S): the fixed threshold for speech,
     *   lifted for a voice whose top end is normally high (a rasp keeps its top), but never above
     *   -6 - S, so a band within 6 dB of the whole signal is always sibilance.
     *   GR = clamp (-1.5 (rel - T), Range, 0) dB (no action while L_band < -55 dBFS), attack
     *   0.5 ms, release 50 ms while the band is hot and 6 ms once the target has let go.
     *   Range = -20 u - 4 x dB, S = 4 x dB, x = ((u - 0.4) / 0.6)^1.5 above 40 % (KnobLaws).
     *
     * Linked detection, zero latency, Split/Wide changes glide over 20 ms.
     */
    class DeessModule final : public Module
    {
    public:
        static constexpr double c_shelfHz = 2800.0;
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

        /** Current gain reduction in dB (<= 0): the shelf's depth (Wide applies 0.8 of it). */
        float getGrDb() const { return static_cast<float> (m_grDb); }

        /** Test hook: force the reduction (dB) instead of the detector, reference and ballistics. */
        void setForcedGainForTest (bool forced, double grDb = 0.0);

        /** Effective range in dB (<= 0) for given settings. */
        static double rangeDbFor (const DeessSettings& s);

    private:
        void resetWet();

        double m_fs = 48000.0;
        OnFade m_fade;

        // The shelf: two TPT state-variable sections per channel, fixed coefficients.
        struct Svf
        {
            double ic1 = 0.0, ic2 = 0.0;
        };
        std::array<std::array<Svf, 2>, 2> m_shelf;          // [channel][section]
        double m_svfA1 = 0.0, m_svfA2 = 0.0, m_svfA3 = 0.0;
        std::array<std::array<Biquad, 2>, 2> m_detector;   // per channel: HP2, LP2

        double m_rangeTarget = -12.0, m_range = -12.0, m_rangeCoeff = 0.0;
        double m_shiftTarget = 0.0, m_shift = 0.0;   // detector threshold shift (dB, laws::deessThresholdShiftDb)
        double m_wideTarget = 0.0, m_wide = 0.0, m_wideCoeff = 0.0;

        double m_envBand = 0.0, m_envFull = 0.0;
        double m_envAttack = 0.0, m_envRelease = 0.0;
        double m_grAttack = 0.0, m_grReleaseHot = 0.0, m_grReleaseFast = 0.0;
        double m_grDb = 0.0;

        // The voice's own top-end balance (dB) and how long it has been heard; the loud end.
        double m_refRel = -21.0, m_refSeconds = 0.0;
        double m_topDb = -120.0, m_topFallPerSample = 0.0;
        double m_dt = 1.0 / 48000.0;
        double m_heldSeconds = 0.0;   // how long a reduction of more than 1 dB has been held

        bool m_forced = false;
        double m_forcedGrDb = 0.0;
    };
}

#pragma once

#include "../../Params.h"
#include "../common/DelayLine.h"
#include "../common/Module.h"
#include "../common/SmoothRandom.h"

#include <array>
#include <cmath>

namespace roar
{
    struct EnsembleSettings
    {
        bool on = Params::defaultOf (Params::ensembleOn) > 0.5f;
        float amount = Params::defaultOf (Params::ensemble) * 0.01f;        // m, 0..1
        float width = Params::defaultOf (Params::ensembleWidth) * 0.01f;    // W, 0..1.5
        Params::EnsembleVoices voices = static_cast<Params::EnsembleVoices> (static_cast<int> (Params::defaultOf (Params::ensembleVoices)));
        Params::EnsembleMotion motion = static_cast<Params::EnsembleMotion> (static_cast<int> (Params::defaultOf (Params::ensembleMotion)));
    };

    /**
     * ENSEMBLE: a 3- or 6-voice modulated chorus (spec §5.11), mono in, stereo out. It always
     * creates stereo when On.
     *
     *   tau_v = D (1 + 0.07 v) + Dep LFO_v + S RND_v               (clamped to >= 3 ms)
     *   tau_v += 0.18 ms sin (2 pi 6.2 t + 1.7 . 2 pi v / N)        (the same depth for every motion)
     *   LFO_v = 0.8 sin (2 pi R t + 2 pi v / N)
     *   RND_v = smooth random (continuous slope), 0.7 .. 3 Hz, its own rate and seed per voice
     *   tap_v = cubic Hermite read of one shared line at tau_v
     *   Lw, Rw = equal-power pans per voice (v0 left, v1 right, v2 centre; 6 voices repeat it)
     *   mid_w = (Lw + Rw) / 2,  side_w = (Lw - Rw) / 2 . k_N w(W),  w(W) = 0.859 (W / 1.5)^1.3
     *   out_L,R = c(m) ((1 - m) x_L,R + m g_N (mid_w +- side_w)),  m = 0.7 Amount
     *   (laws::ensembleAmount, laws::ensembleSideGain: past m = 0.7 and w = 0.859 the chorus turns
     *   seasick and out of phase, so the travel ends there)
     *
     *   - The input of the voices is the mid of the module's input; the dry is the input itself,
     *     undelayed, so it keeps any stereo it already has.
     *   - The wet is never high-passed: the slow comb movement in the low end is the point.
     *   - Width law: the voices share most of their low end, so (Lw - Rw) is small and needs the
     *     scale k_N (2.08 for 3 voices, 2.55 for 6).
     *   - g_N is calibrated per voice count and motion so the wet (W = 0) of pink noise has the
     *     dry's RMS; a motion change glides it over 50 ms. c(m) =
     *     1 / sqrt ((1 - m)^2 + m^2) is the static mix-law makeup (the dry and a voice 7-25 ms
     *     later are incoherent over most of the band); it is 1 at m = 0, so Amount 0 is an exact
     *     pass-through.
     *   - Motion: Slow (R 0.25 Hz, Dep 2.5 ms, D 15 ms, S 1.0 ms), Medium (0.45, 1.8, 12, 0.5),
     *     Fast (1.2, 1.0, 9, 0.4), Vibrato (5.5, 0.8, 7, 0.2), whose wet share is sqrt (m), so a
     *     vibrato is heard early in the travel and still grows with it (Amount 0 stays neutral).
     *     Motion changes glide over 150 ms; a voice-count change glides the voice phases over
     *     300 ms and fades the extra voices over 50 ms.
     *
     * The delay times are computed on a fixed 32-sample control grid counted from reset and
     * interpolated linearly per sample, so the output does not depend on the host block size.
     * Zero latency. No allocation after prepare().
     */
    class EnsembleModule final : public Module
    {
    public:
        static constexpr int c_maxVoices = 6;

        /** Test-only overrides (the spec §5.11 pitch-modulation test solos a voice, no random). */
        struct Diagnostics
        {
            int soloVoice = -1;     // >= 0: only that voice is heard, at unity gain
            bool random = true;     // false: S = 0
        };

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return 0; }
        bool makesStereo() const override { return m_settings.on; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const EnsembleSettings& s);
        void setDiagnostics (const Diagnostics& d) { m_diagnostics = d; }

        /** Readouts: 0..1 width from the output correlation (1 - corr), and the correlation itself. */
        float getWidthReadout() const;
        float getCorrelation() const;
        float getOnValue() const { return m_fade.getValue(); }

        /** Current delay of voice v in samples (diagnostics). */
        float getVoiceDelaySamples (int v) const;

        struct MotionSpec
        {
            float rateHz, depthMs, delayMs, scatterMs;
        };
        static MotionSpec motionSpec (Params::EnsembleMotion motion);

        /** The width knob (0..1.5) to the side gain actually applied. */
        static float sideGainForWidth (float w);
        static float sideScaleFor (int numVoices);
        static float makeupForAmount (float m);
        static float voiceGainFor (int numVoices, Params::EnsembleMotion motion = Params::EnsembleMotion::medium);

    private:
        static constexpr int c_controlBlock = 32;
        /**
         * Two cascaded one-poles (each with half the smoothing time): the value and its slope
         * are both continuous, so a knob or chip change starts without a kink. Lands exactly on
         * the target (no subnormal tail). Plain copyable state.
         */
        struct Glide
        {
            float stage = 0.0f, value = 0.0f;

            void snap (float target) { stage = value = target; }

            float step (float target, float coeff)
            {
                stage = towards (stage, target, coeff);
                value = towards (value, stage, coeff);
                return value;
            }

            static float towards (float v, float target, float coeff)
            {
                v += coeff * (target - v);
                return std::abs (target - v) < 1.0e-7f ? target : v;
            }
        };

        static constexpr double c_fastRateHz = 6.2;
        static constexpr double c_minDelayMs = 3.0;
        static constexpr double c_maxDelayMs = 30.0;

        struct Voice
        {
            float tau = 0.0f;       // delay now, samples
            float tauStep = 0.0f;   // per-sample increment in this control block
            float phase = 0.0f;     // smoothed v / N, cycles
            float phaseStage = 0.0f;
            float gain = 0.0f;      // 0 / 1 (voice count), ramped per sample
            float gainStep = 0.0f;
            float gainEnd = 0.0f;   // control-rate smoothed value at the end of the block
            float gainStage = 0.0f;
        };

        struct State
        {
            double slowPhase = 0.0, fastPhase = 0.0;   // cycles, [0, 1)
            double rateHz = 0.45, depth = 0.0, delay = 0.0, scatter = 0.0;   // smoothed; samples
            double rateStage = 0.45, depthStage = 0.0, delayStage = 0.0, scatterStage = 0.0;
            float voiceGain = 1.0f, voiceGainStep = 0.0f, voiceGainEnd = 1.0f;   // g_N, ramped
            float sideScale = 2.08f, sideScaleStep = 0.0f, sideScaleEnd = 2.08f;   // k_N, ramped
            float voiceGainStage = 1.0f, sideScaleStage = 2.08f;
            Glide amount, width;                       // smoothed per sample
            double midPower = 0.0, sidePower = 0.0;    // readout trackers
            double midAccum = 0.0, sideAccum = 0.0;
            int controlLeft = 0;
            std::array<Voice, c_maxVoices> voices {};
        };

        void snapSmoothers();
        void startControlBlock (bool snap);
        float effectiveAmount() const;
        int activeVoices() const;
        float voiceDelayTarget (int v);   // tau_v at the current phases, samples; advances RND_v

        double m_fs = 48000.0;
        double m_fastDepth = 0.0;      // the 6.2 Hz component's depth, samples
        int m_maxBlock = 512;
        double m_motionCoeff = 0.0;    // control rate, per stage of a two-stage glide (150 ms in all)
        double m_phaseCoeff = 0.0;     // control rate, per stage (300 ms in all)
        double m_gainCoeff = 0.0;      // control rate, per stage (50 ms in all)
        double m_readoutCoeff = 0.0;   // control rate, 300 ms
        float m_amountCoeff = 0.0f;    // per sample, per Glide stage (20 ms in all)
        float m_minDelay = 0.0f, m_maxDelay = 0.0f;   // samples

        EnsembleSettings m_settings;
        Diagnostics m_diagnostics;
        State m_state;
        std::array<SmoothRandom, c_maxVoices> m_random;
        DelayLine m_line;
        OnFade m_fade;
    };
}

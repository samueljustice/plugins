#pragma once

#include "../../Params.h"
#include "../common/HilbertPair.h"
#include "../common/Module.h"
#include "../common/SmoothRandom.h"
#include "../common/TptOnePole.h"

#include <array>
#include <cmath>

namespace roar
{
    struct ShimmerSettings
    {
        bool on = Params::defaultOf (Params::shimmerOn) > 0.5f;
        float amount = Params::defaultOf (Params::shimmer) * 0.01f;    // u, 0..1
        float rateHz = Params::defaultOf (Params::shimmerRate);        // 0.5..40 Hz
        Params::ShimmerMode mode = static_cast<Params::ShimmerMode> (static_cast<int> (Params::defaultOf (Params::shimmerMode)));
    };

    /**
     * SHIMMER: a single-sideband frequency shifter mixed with the dry (spec §5.7). Every partial
     * moves by the same few Hz and beats against its dry copy at the shift rate: a combined
     * amplitude and pitch wobble that is much deeper in cents on the low partials than on the
     * highs. Feedback around the shifter adds sidebands at 2, 3, ... times the shift, which makes
     * the wobble asymmetric and sharper.
     *
     *   u      = x + g LP16k (HP30 (y_fb[n-1]))
     *   (I, Q) = HilbertPair (u)
     *   theta += 2 pi D / fs                   (double; D = Rate (1 + 0.12 drift), drift at 0.2 Hz)
     *   up     = I cos(theta) - Q sin(theta),  down = I cos(theta) + Q sin(theta)
     *   wet    = Up: up | Down: down | Up+Down: (up + down) / sqrt 2 | Stereo: L = down, R = up
     *   out    = c(m) ((1 - m) x + m sqrt (1 - q g^2) wet)
     *
     *   - Amount m = u. Feedback g = clamp (0.30 + 0.53 max (0, m - 0.25), 0, 0.75), internal.
     *   - c(m) = ((1 - m)^2 + m^2)^-0.42 is the static mix-law makeup: the dry and the shifted
     *     copy are incoherent over a beat period, so a linear mix alone would lose up to 3 dB.
     *     The exponent is a little under full power compensation (0.5), so the level stays
     *     within 0.5 dB of unity and the beat crest within +2.5 dB at the default. It is 1 at
     *     m = 0 and m = 1, so Amount 0 is an exact pass-through.
     *   - The loop filters keep the loop gain below 1 where the Hilbert pair leaves quadrature
     *     (below about 20 Hz and in the top octave): there the shifter's gain can reach sqrt 2.
     *     HP30 also stops DC from building up.
     *   - The feedback path of Up+Down carries (up + down) / 2 (bounded by the input), so the
     *     loop stays stable for any g < 1. That is half the power of its output, so q = 1/2 in
     *     Up+Down and 1 otherwise: sqrt (1 - q g^2) keeps the wet's RMS at the input's in every
     *     mode (the incoherent-sum estimate behind sqrt (1 - g^2)).
     *   - Mode changes crossfade the up / down weights over 20 ms; Rate is smoothed over 50 ms.
     *
     * Mono in, mono out, except Stereo mode, which creates stereo (left down, right up). In the
     * stereo part every other mode runs on both channels with one shared oscillator. Zero latency.
     * The oscillator and the drift run on a fixed 32-sample control grid counted from reset, so
     * the output does not depend on the host block size. No allocation after prepare().
     */
    class ShimmerModule final : public Module
    {
    public:
        /** Test-only overrides (the tests in spec §5.7 need fixed feedback and no drift). */
        struct Diagnostics
        {
            float feedback = -1.0f;   // < 0: the Amount macro; otherwise g, clamped to 0..0.95
            bool drift = true;
        };

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return 0; }
        bool makesStereo() const override;
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const ShimmerSettings& s);
        void setDiagnostics (const Diagnostics& d) { m_diagnostics = d; }

        /** Readouts: the shift oscillator phase 0..1, and the current shift in Hz (with drift). */
        float getPhase() const { return static_cast<float> (std::fmod (m_state.theta, dsp::c_twoPi) / dsp::c_twoPi); }
        float getShiftHz() const { return static_cast<float> (m_state.shiftHz); }
        float getFeedback() const { return m_state.g; }
        float getOnValue() const { return m_fade.getValue(); }

        static float feedbackForAmount (float m);
        static float makeupForAmount (float m);

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

        struct Channel
        {
            HilbertPair hilbert;
            TptOnePole loopHighpass;
            TptOnePole loopLowpass;
            float feedback = 0.0f;   // y_fb[n-1]
            Glide wUp, wDown;        // smoothed sideband weights
        };

        // Every scalar piece of running state (plain, copied by assignment).
        struct State
        {
            double theta = 0.0;      // oscillator phase, radians, wrapped to [0, 2 pi)
            double delta = 0.0;      // phase increment per sample in this control block
            double rateSmoothed = 12.0;
            double shiftHz = 12.0;
            double cosTheta = 1.0, sinTheta = 0.0;   // rotator, re-seeded from theta every block
            double cosDelta = 1.0, sinDelta = 0.0;
            int controlLeft = 0;     // samples left in the current control block
            Glide amount;            // smoothed m
            Glide loopPower;         // smoothed q: the fed-back share of the wet's power
            float g = 0.30f;         // feedback used this sample
        };

        void resetWet();
        void snapSmoothers();
        void startControlBlock();
        void targetWeights (int channel, float& up, float& down) const;

        double m_fs = 48000.0;
        int m_maxBlock = 512;
        float m_amountCoeff = 0.0f;   // per-sample, per Glide stage (20 ms in all)
        float m_weightCoeff = 0.0f;   // per-sample, per Glide stage (20 ms in all; mode crossfade)
        double m_rateCoeff = 0.0;     // per-control-block one-pole, 50 ms

        struct Gains
        {
            float m = -1.0f, g = -1.0f, q = -1.0f;   // the (m, g, q) these were computed for
            float dry = 1.0f, wet = 0.0f;            // c(m) (1 - m), c(m) m sqrt (1 - q g^2)
        };

        ShimmerSettings m_settings;
        Diagnostics m_diagnostics;
        State m_state;
        Gains m_gains;
        std::array<Channel, 2> m_channels;
        SmoothRandom m_drift;
        OnFade m_fade;
    };
}

#pragma once

#include "../../Params.h"
#include "../common/CharacterSaturator.h"
#include "../common/LampLfo.h"
#include "../common/Module.h"
#include "../common/Oversampler2x.h"

#include <array>
#include <cstdint>

namespace roar
{
    struct SwirlSettings
    {
        bool on = Params::defaultOf (Params::swirlOn) > 0.5f;
        float amount = Params::defaultOf (Params::swirl) * 0.01f;          // u, 0..1 (the mix)
        float rateHz = Params::defaultOf (Params::swirlRate);              // 0.05..10 Hz
        Params::SwirlCircuit circuit = static_cast<Params::SwirlCircuit> (static_cast<int> (Params::defaultOf (Params::swirlCircuit)));
        Params::SwirlMotion motion = static_cast<Params::SwirlMotion> (static_cast<int> (Params::defaultOf (Params::swirlMotion)));
    };

    /**
     * SWIRL: an analog-modelled phaser with resonance (spec §5.9).
     *
     *   x_in = Warm (x dB(drive))                    2x oversampled; the dry is untouched
     *   f_i  = Freq 2^M 10^(Spread (i / (N - 1) - 1/2)),   clamped to [20 Hz, 0.45 fs]
     *   stage i: TPT first-order allpass, g = tan (pi f_i / fs) per sample (half-angle Pade tan)
     *
     *   Each stage is affine in its input, ap_i = a_i x_i + b_i with a_i = (g - 1) / (g + 1) and
     *   b_i = 2 s_i / (1 + g), so the chain is A u = Gc u + Sc and the feedback is solved without
     *   a unit delay (zero-delay feedback, so the tuning holds at any frequency):
     *     Classic   y = (Gc x + Sc) / (1 - k Gc),          u = x + k y;  states soft-limited 2 tanh (s / 2)
     *     Modern    y = (Gc x + Sc - k x) / (1 - k Gc),    u = x + k y;  |y| = |x| (allpass lattice), so
     *               the notches keep their full depth while the peaks sharpen
     *   The stages then run on u. Res polarity - uses -k.
     *
     *   P   = 1/2 (x_in + pol y) makeup,   makeup = (1/2 (1 + 1 / (1 - |k|)))^-1/2 for Classic, with
     *         |k| capped at 0.6 (at most -2.4 dB); Modern needs none (|P| <= |x_in|)
     *   P   -> soft safety knee: linear up to R + 14 dB (-6 dBFS), then tanh towards -3 dBFS
     *   out = (1 - a) dry + a P
     *
     * Circuits (chip): stages, spread (decades), Freq, sweep (octaves), k, colour, polarity,
     * res polarity, drive:
     *   Liquid 4  4  1.2  700 Hz  2.5  0.45  Classic  +  +  +3 dB
     *   Smooth 4  4  0    900 Hz  3    0     -        +  -  +2 dB
     *   Talky 4   4  0    800 Hz  2.5  0.65  Classic  +  +  +3 dB
     *   Deep 6    6  0.3  600 Hz  3    0.55  Modern   +  +  +3 dB
     *   Deep 12   12 0.4  500 Hz  3.5  0.5   Modern   +  -  +2 dB
     *   Dark 5    5  0.6  650 Hz  2.5  0.5   Classic  +  +  +4 dB
     * A circuit change crossfades two stage engines over 20 ms; the drive glides.
     *
     * Motion (chip), M in octaves:
     *   LFO        M = Sweep (s - 1/2), s = light^0.7 of a lamp / light-cell model at Rate
     *              (30 ms on, 180 ms off, both scaled by min (1, 1 Hz / Rate))
     *   LFO+Level  the LFO plus 1.5 oct m_env (peak follower 20 / 250 ms;
     *              m_env = clamp ((e_dB - (R - 18)) / 30, 0, 1), R = -20 dBFS): loud lines open the notches up
     *   Random     M = Sweep (r - 1/2), r a sample-and-hold at Rate through a 2-pole smoother
     *              (0.25 / Rate per pole): a drunken walk
     * A motion change crossfades the modulation over 50 ms.
     *
     * Amount sets the mix a = 1 - (1 - u)^1.5 (laws::swirlMix: the effect arrives early in the
     * travel and the top is a gentle approach to full wet). It also scales k by max (0.94, 0.6 + 0.4 a / 0.35), capped at
     * the absolute 0.95 only (1.74 k0 at full wet). Amount 0 outputs the latency-aligned dry exactly.
     *
     * Latency: Oversampler2x::c_latency (23 samples) at every rate and setting (the dry is the
     * aligned dry of the On fade). In the stereo part both channels use the same stage
     * frequencies (one detector on max (|L|, |R|)). Every control value runs per sample (the
     * lamp and smoother coefficients on a 32-sample grid counted from reset), so the output does
     * not depend on the host block size. No allocation after prepare(); NaN-safe
     * (non-finite input, or |x| >= 1e4, is treated as silence).
     */
    class SwirlModule final : public Module
    {
    public:
        static constexpr int c_maxStages = 12;
        static constexpr double c_referenceDb = -20.0;
        static constexpr double c_envThresholdDb = c_referenceDb - 18.0;
        static constexpr double c_envRangeDb = 30.0;
        static constexpr double c_envOctaves = 1.5;
        static constexpr double c_envAttack = 0.020, c_envRelease = 0.250;
        static constexpr double c_lampTauOn = 0.030, c_lampTauOff = 0.180, c_lampReferenceHz = 1.0;
        static constexpr double c_lampExponent = 0.7;
        static constexpr double c_randomSmooth = 0.25;
        static constexpr double c_defaultAmount = 0.35;
        static constexpr double c_maxK = 0.95;
        static constexpr double c_makeupKCap = 0.6;
        static constexpr double c_safetyKneeDb = c_referenceDb + 14.0;
        static constexpr double c_safetyCeilingDb = -3.0;
        static constexpr double c_minStageHz = 20.0;
        static constexpr double c_maxStageRatio = 0.45;

        struct Circuit
        {
            int stages = 4;
            double spreadDecades = 1.2;
            double freqHz = 700.0;
            double sweepOctaves = 2.5;
            double k = 0.45;
            bool modern = false;
            double polarity = 1.0;
            double resPolarity = 1.0;
            double driveDb = 3.0;
        };

        static Circuit circuitFor (Params::SwirlCircuit c);

        /** Per-sample stage coefficients, shared by both channels (linked). */
        struct Coeffs
        {
            int stages = 4;
            std::array<float, c_maxStages> G {};    // g / (1 + g)
            std::array<float, c_maxStages> a {};    // (g - 1) / (g + 1)
            std::array<float, c_maxStages> b2 {};   // 2 / (1 + g)

            void setStage (int i, double g);
        };

        /** One channel's stage chain with zero-delay feedback (public for the tests). */
        class Chain
        {
        public:
            void reset() { m_s.fill (0.0f); }

            /** One sample: returns y (Classic: A u; Modern: A u - k x), k already signed. */
            float process (float x, const Coeffs& c, float k, bool modern);

            void copyStateFrom (const Chain& other) { m_s = other.m_s; }

        private:
            std::array<float, c_maxStages> m_s {};
        };

        /** g = tan (pi f / fs) by the half-angle Pade form (relative error < 3e-8 up to 0.45 fs). */
        static double fastTan (double x);

        /** Test-only overrides. */
        struct Diagnostics
        {
            bool useCircuit = false;   // true: run `circuit` instead of the chip's
            Circuit circuit {};
            bool motion = true;        // false: M = 0 (the notches sit at Freq)
            bool safety = true;        // false: no output knee
            bool makeup = true;
            double k = -1.0;           // >= 0: this |k| (signed by the circuit), ignoring Amount
        };

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return Oversampler2x::c_latency; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const SwirlSettings& s);
        void setDiagnostics (const Diagnostics& d);

        /** Readouts: the first notch in Hz and as a 0..1 log position over 20 Hz..20 kHz. */
        float getNotchHz() const { return m_state.notchHz; }
        float getNotchPosition() const;
        float getModulationOctaves() const { return m_state.modOctaves; }
        float getK() const { return m_state.kNow; }
        float getOnValue() const { return m_fade.getValue(); }

        /** k for a circuit's k0 and Amount a (before the resonance polarity). */
        static double kFor (double k0, double amount);

        /** Classic resonance makeup for |k|. */
        static double makeupFor (double k);

    private:
        struct Engine
        {
            Circuit circuit {};
            int id = 0;                                    // circuit index, -1 for a test circuit
            std::array<double, c_maxStages> ratio {};      // 10^(Spread (i / (N - 1) - 1/2))
            Coeffs coeffs {};
            std::array<Chain, 2> chains {};

            void setCircuit (const Circuit& c, int circuitId);
        };

        struct Channel
        {
            Oversampler2x os;
            CharacterSaturator drive;   // Warm, at 2 fs
        };

        struct State
        {
            double phase = 0.0;          // LFO phase, 0..1
            double logRate = 0.0;        // smoothed log (Rate)
            double lampRate = 0.35;      // rate the lamp times were last set for
            float amount = 0.35f;        // smoothed a
            float drive = 1.0f;          // smoothed dB(drive)
            float env = 0.0f;
            float randomTarget = 0.5f, random1 = 0.5f, random2 = 0.5f;
            float randomCoeff = 0.0f;
            float motionMix = 1.0f;      // 0..1: motionFrom -> motionTo
            int motionFrom = 1, motionTo = 1;
            float engineMix = 1.0f;      // 0..1: the other engine -> engineTo
            int engineTo = 0;
            float notchHz = 0.0f, modOctaves = 0.0f, kNow = 0.0f;
            std::uint32_t controlCount = 0;
            std::uint32_t random = 0x6b43a9b5u;
        };

        void resetWet();
        void resetMotion();
        void runLatencyOnly (const float* l, const float* r, int n, bool stereo);
        void snapToTargets();
        int requestedCircuitId() const;
        Circuit requestedCircuit() const;
        void updateCoeffs (Engine& e, double q, double envOct);
        float kForEngine (const Engine& e, float amount) const;
        float nextUnit();
        float safety (float p) const;

        double m_fs = 48000.0;
        int m_maxBlock = 512;
        float m_paramCoeff = 0.0f;           // 20 ms
        float m_envAttack = 0.0f, m_envRelease = 0.0f;
        float m_engineStep = 1.0f / 960.0f;  // 20 ms
        float m_motionStep = 1.0f / 2400.0f; // 50 ms
        float m_kneeLevel = 0.5f, m_kneeRange = 0.2f;

        SwirlSettings m_settings;
        Diagnostics m_diagnostics;
        State m_state;
        std::array<Engine, 2> m_engines;
        std::array<Channel, 2> m_channels;
        LampLfo m_lamp;
        OnFade m_fade;
    };
}

#pragma once

#include "../../Params.h"
#include "../common/LampLfo.h"
#include "../common/Module.h"
#include "../common/Oversampler2x.h"
#include "../common/SmoothRandom.h"
#include "../common/TptOnePole.h"

#include <array>
#include <cstdint>

namespace roar
{
    struct GrowlSettings
    {
        bool on = Params::defaultOf (Params::growlOn) > 0.5f;
        float depth = Params::defaultOf (Params::growl) * 0.01f;          // u, 0..1
        float rateHz = Params::defaultOf (Params::growlRate);             // 1..150 Hz
        Params::GrowlShape shape = static_cast<Params::GrowlShape> (static_cast<int> (Params::defaultOf (Params::growlShape)));
        Params::GrowlCharacter character = static_cast<Params::GrowlCharacter> (static_cast<int> (Params::defaultOf (Params::growlCharacter)));
    };

    /**
     * GROWL: an analog-style amplitude tremolo, the fast "chop" of a creature voice (spec §5.8).
     *
     *   s      = shape (phase) in 0..1, 1 = fully open
     *   D      = 0.85 u                                 (u = Depth; default 45 % -> D = 0.38)
     *   D_eff  = D (1 + 0.3 m_env) (1 + 0.08 h) gate    (<= 0.95; h: per-cycle depth jitter in -1..1)
     *   g      = 1 - D_eff (1 - s)                      (g <= 1: the stage never boosts by itself)
     *   gm     = g makeup,   makeup = 1 / rms (g), capped at +3 dB: in closed form from D_eff for
     *            the sine shapes (Sine, Throat, Rattle), so it follows the voicing gate and the level
     *            push without lag; measured over each LFO cycle (then smoothed, 50 ms) for Opto and
     *            Square. Derived from the modulation law only, never from the output level.
     *
     *   The line depth of a sine at depth D is m = (D / 2) / (1 - D / 2): 0.236 at the default,
     *   0.74 at 100 %.
     *
     * Shapes:
     *   Sine     s = 0.5 + 0.5 cos (2 pi phase)
     *   Opto     lamp / light-cell model (LampLfo: 3 ms on, 25 ms off, both scaled by min (1, 8 Hz / f)),
     *            s = 1 - light^0.8: a fast dip and a slow recovery
     *   Square   s = (cos (2 pi phase) >= 0), through a critically damped 2-pole low-pass whose
     *            two time constants add up to 0.35 * 0.25 / f (at least 0.3 ms)
     *   Throat   Sine at f0 / 2 of the voice stream: alternate glottal periods get different gains.
     *            Depth fades in and out with voicing over 15 ms, so noise and unvoiced parts get no AM.
     *   Rattle   Sine at f 2^(1.5 m_env): loud lines rattle faster
     *
     *   m_env = clamp ((e_dB - (R + 6)) / 24, 0, 1), e_dB from a 15 / 180 ms peak follower,
     *   R = -20 dBFS. The rate drifts by at most +-0.5 % (a slow smooth random), so the line stays a
     *   line. Shape changes crossfade the shape value over 20 ms.
     *
     * Character ("bias tremolo": the distortion moves with the gain), 2x oversampled:
     *   v = x gm dB(drive);  y = Sat (v + b (1 - g)) - Sat (b (1 - g));  out = y / dB(drive)
     *   Clean  drive 0 dB, no curve
     *   Warm   drive +3 dB, b -0.15, split at 300 Hz: the low band through tanh (1.5 lo + 0.1) / 1.5,
     *          the high band linear. The bias is negative so the closed part of the cycle sits near
     *          the curve's point of symmetry (lo = -0.067): the even harmonics of the curve's offset
     *          sound in the open part and vanish in the trough (crest THD about 13 dB above the trough)
     *   Grind  drive +8 dB, b +0.15, asymmetric: 1.2 tanh (1.8 v) / 1.8 above 0, 0.6 tanh (v / 0.6) below
     *   Only the curve's deviation from linear is DC-blocked (10 Hz), so the linear part keeps its
     *   phase. The character fades in over the first 10 % of Depth (Depth 0 is an exact delay),
     *   and character changes crossfade over 20 ms.
     *
     * Clean takes a pure 23-sample delay of x gm instead of the oversampler, so it keeps the full
     * top octave at every rate. The oversampler always runs (with a linear middle while Clean or
     * fully Off), so a switch to Warm or Grind, or back On, starts from the live signal.
     *
     * Latency: Oversampler2x::c_latency (23 samples) at every rate and setting. In the stereo part
     * both channels get the same modulation (one detector on max (|L|, |R|)). Every control value
     * runs per sample, so the output does not depend on the host block size. No allocation after
     * prepare(); NaN-safe (non-finite input, or |x| >= 1e4, is treated as silence).
     */
    class GrowlModule final : public Module
    {
    public:
        /** Test-only overrides. */
        struct Diagnostics
        {
            bool makeup = true;      // false: no 1 / rms (g) makeup
            bool humanize = true;    // false: no per-cycle depth jitter
            bool drift = true;       // false: the rate is exact
            bool envelope = true;    // false: m_env = 0 (no depth or rate push from the level)
        };

        static constexpr double c_depthScale = 0.85;
        static constexpr double c_maxDepth = 0.95;
        static constexpr double c_referenceDb = -20.0;          // R (spec §4)
        static constexpr double c_envThresholdDb = c_referenceDb + 6.0;
        static constexpr double c_envRangeDb = 24.0;
        static constexpr double c_envAttack = 0.015, c_envRelease = 0.180;
        static constexpr double c_envDepthPush = 0.3;
        static constexpr double c_rattleOctaves = 1.5;
        static constexpr double c_humanizeDepth = 0.08;
        static constexpr double c_driftAmount = 0.005;
        static constexpr double c_driftRateHz = 0.2;
        static constexpr double c_makeupCapDb = 3.0;
        static constexpr double c_warmBias = -0.15;
        static constexpr double c_grindBias = 0.15;
        static constexpr double c_warmDriveDb = 3.0;
        static constexpr double c_grindDriveDb = 8.0;
        static constexpr double c_dcBlockHz = 10.0;
        static constexpr double c_optoTauOn = 0.003, c_optoTauOff = 0.025, c_optoReferenceHz = 8.0;
        static constexpr double c_squareSmooth = 0.35 * 0.25, c_squareMinTau = 0.0003;
        static constexpr double c_voicingTau = 0.015;
        static constexpr double c_fadeSeconds = 0.020;

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return Oversampler2x::c_latency; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const GrowlSettings& s);
        void setDiagnostics (const Diagnostics& d) { m_diagnostics = d; }

        /** Readouts (audio thread values; read them on the audio thread or via publishFeatures). */
        float getModulation() const { return m_state.modulation; }   // gm - 1, about -0.95..+0.41
        float getRateHz() const { return static_cast<float> (m_state.rateNow); }
        float getEffectiveDepth() const { return m_state.depthEff; }
        float getMakeupDb() const { return static_cast<float> (dsp::gainToDb (static_cast<double> (m_state.makeup))); }
        float getOnValue() const { return m_fade.getValue(); }

        /** D for a Depth u (0..1), and the line depth m of a sine at depth D. */
        static double depthForAmount (double u) { return c_depthScale * std::clamp (u, 0.0, 1.0); }
        static double lineDepthFor (double depth) { return (0.5 * depth) / (1.0 - 0.5 * depth); }

        /** 1 / rms (g) of a sine at depth D, capped at +3 dB. */
        static double sineMakeupFor (double depth);
        static float sineMakeupFor (float depth);

    private:
        struct Channel
        {
            Oversampler2x os;
            TptOnePole split;      // Warm: 300 Hz split, at 2 fs
            TptOnePole dcBlock;    // 10 Hz, on the curve's residual, at 2 fs
            DelayLine linear;      // Clean path: x gm delayed by the latency
        };

        static constexpr int c_numShapes = 5;
        static constexpr int c_gRingSize = 32;

        struct State
        {
            double phase = 0.0;          // main LFO phase, 0..1
            double throatPhase = 0.0;    // f0 / 2 phase, 0..1
            double logRate = 0.0;        // smoothed log (rateHz)
            double rateNow = 27.0;       // rate used this sample (readout)
            double f0 = 110.0;           // smoothed voice f0
            double gSquareSum = 0.0;     // makeup: sum of g^2 over the current cycle
            int gSquareCount = 0;
            float depth = 0.38f;         // smoothed D
            float depthEff = 0.38f;      // smoothed D_eff
            float humanize = 0.0f;       // h for this cycle
            float env = 0.0f;            // peak follower
            float voicing = 0.0f;        // Throat voicing gate, 0..1
            float makeup = 1.0f;         // applied
            float makeupTarget = 1.0f;   // Opto / Square: from the last cycle
            float makeupMeasured = 1.0f; // Opto / Square: smoothed
            float square1 = 1.0f, square2 = 1.0f;   // Square 2-pole state
            float squareCoeff = 1.0f;    // per-pole one-pole coefficient (control rate)
            double lampRate = 27.0;      // rate the lamp times were last set for (control rate)
            std::uint32_t controlCount = 0;
            float shapeMix = 1.0f;       // 0..1: from -> to
            int shapeFrom = 0, shapeTo = 0;
            float charMix = 1.0f;        // 0..1: charFrom -> charTo (warm / grind)
            int charFrom = 1, charTo = 1;
            float osWeight = 1.0f;       // 0: the Clean delay path, 1: the oversampled curve
            float modulation = 0.0f;     // readout
            std::array<float, c_gRingSize> gRing {};
            int gPos = 0;
            std::uint32_t random = 0x2545f491u;
        };

        void resetWet();
        void resetModulation();
        void runLatencyOnly (const float* l, const float* r, int n, bool stereo);
        void snapToTargets();
        void startShape (int shape);
        void startCharacter (int character);
        float shapeValue (int shape, double phase, double throatPhase);
        float characterSample (Channel& ch, float w, float lift);
        float nextUniform();

        double m_fs = 48000.0;
        int m_maxBlock = 512;
        float m_paramCoeff = 0.0f;      // 20 ms one-pole
        float m_depthEffCoeff = 0.0f;   // 2 ms one-pole (humanize steps)
        float m_makeupCoeff = 0.0f;     // 50 ms one-pole
        float m_envAttack = 0.0f, m_envRelease = 0.0f;
        float m_voicingCoeff = 0.0f;
        float m_f0Coeff = 0.0f;         // 10 ms
        float m_fadeStep = 1.0f / 960.0f;
        int m_maxCycleSamples = 48000;
        std::array<float, 2> m_drive {};    // dB(drive) for warm, grind
        std::array<float, 2> m_invDrive {};

        GrowlSettings m_settings;
        Diagnostics m_diagnostics;
        State m_state;
        std::array<Channel, 2> m_channels;
        LampLfo m_lamp;
        SmoothRandom m_drift;
        OnFade m_fade;
    };
}

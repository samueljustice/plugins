#pragma once

#include "../../Params.h"
#include "../common/AdaaTable.h"
#include "../common/Biquad.h"
#include "../common/Module.h"
#include "../common/Oversampler2x.h"
#include "../common/TptOnePole.h"

#include <array>

namespace roar
{
    struct RoarSettings
    {
        bool on = true;
        float drive = 0.35f;                                             // u, 0..1 (Roar Drive / 100)
        float tone = 0.0f;                                               // -1 (bass) .. +1 (treble) (Roar Tone / 100)
        Params::RoarVoicing voicing = Params::RoarVoicing::line;
    };

    /**
     * ROAR: a two-stage triode saturator with the tone stack between the stages (spec §5.10).
     *
     *   in -> voicing (Line: flat; Mic: -3.8 dB wide bell @ 70 Hz, -2 dB top shelf)
     *      -> 2x up -> x Drive1 (18 a + 48 max (0, a - 0.5)^2 dB, laws::roarDrive1Db) -> Miller LPF 1 (20 kHz * 0.5^a) -> TRIODE 1 (dynamic bias)
     *      -> / Drive1 -> coupling HPF 12 Hz
     *      -> tone stack: bass boost (1st-order low shelves 80 + 300 Hz, split 85/15),
     *                     plate scoop (bell 200 Hz, Q 0.8: -14 a dB to a = 0.4, a smooth turn to
     *                     -7 dB at 0.6, flat above; laws::roarScoopDb),
     *                     treble boost (1st-order high shelf 2.5 kHz + 2nd-order 10 kHz shelf, 35/65),
     *                     bass = 6 (a - s t) dB, treble = 7 (a + s t) dB, Tone t, tilt strength
     *                     s = 1.5 max (a, 0.5) (laws::roarToneStrength: Tone works at low Drive too),
     *                     fixed +1.5 dB top lift
     *      -> x Drive2 -> Miller LPF 2 (16 kHz) -> TRIODE 2 (lighter bias shift) -> / Drive2
     *      -> 2x down -> output transformer (1-pole LPF 18 kHz * 0.75^a, HPF 25 Hz Q 0.9)
     *      -> static makeup (table over Drive and Tone, calibrated at the reference level)
     *
     * Frequency-dependent drive: each Miller LPF sets what reaches its grid, and the part it removes
     * (driven input minus its low-pass) passes around the triode linearly. The highs hit the curves
     * less (less fizz and IMD) without the top being lost. The bypass runs through the same 2-tap
     * average that first-order ADAA applies to a linear signal, so both paths stay aligned and the
     * small-signal response is flat (a misaligned bypass notches the band around the crossover).
     * Stage 1 bypasses fully; stage 2 bypasses (1 - 0.7 a) of it, so at high Drive more of the
     * top runs through Miller LPF 2 and the near-Nyquist products stay below the aliasing ceiling.
     *
     * The triode (normalised grid volts, static bias Vb = 1):
     *   g = x - b(t);   gh = g <= 0 ? g : g / (1 + g / 1.5)          soft grid conduction
     *   z = 1 + gh / 2.6;   zs = (z + sqrt (z^2 + 0.8^2)) / 2          well-rounded cutoff
     *   I = zs^1.5                                                     3/2-power plate current
     *   y = (I(g) - I(-b(t))) / I'(-1)                                 unity slope, zero at rest
     *   b(t) = 1 + Kb * env (max (0, g)^1.5)                           grid-current blocking
     *          (stage 1: the shift saturates softly at 1 Vb, so the rest point stays above cutoff)
     * The 3/2 law makes it 2nd-harmonic dominant below the knee; the positive side compresses
     * softly first and the negative side fades into cutoff later, so it never squares up. The
     * cutoff rounding and the grid-conduction softness are set so THD rises by at most about 6 %
     * per 3 dB of level at every Drive (no audible knee) while heavy drive stays even-leaning.
     * The curve is tabulated (8192 points over [-16, 16]) and run with first-order ADAA on the
     * combined input g, at the 2x rate. Its output is bounded for any input (it flattens on both sides).
     *
     * Dynamic bias: stage 1 Kb = 0.6 -> 0.4 as Drive rises, envelope 2 ms / 150 ms; stage 2
     * Kb = 0.3, 3 ms / 250 ms. In stereo each channel runs its own curve and both share one
     * envelope per stage (the max), so the image never wanders.
     *
     * Level: there is no output-measuring level follower. A static makeup gain (a function of
     * Drive and Tone only) keeps the loudness of a voice at the reference level within +-1.5 dB
     * across Drive, so louder input still comes out louder and denser.
     *
     * Latency: Oversampler2x::c_latency (23) at every rate and every setting. The two ADAA steps
     * add half a 2x-rate sample each (half a base-rate sample in all), which sits inside the group
     * delay of the minimum-phase stage filters anyway. Off fades to the latency-aligned dry.
     *
     * Parameters are smoothed (30 ms) and applied on a 32-sample control grid counted from reset,
     * so the output does not depend on how the host splits its blocks. No allocation after
     * prepare(); the curve table is built once and shared read-only by every instance.
     */
    class RoarModule : public Module
    {
    public:
        static constexpr int c_controlBlock = 32;

        RoarModule();

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return Oversampler2x::c_latency; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        /** Called once per chunk. No allocation; smoothed internally. */
        void setParams (const RoarSettings& s);

        /** 0..1, VU-ballistic heat: grid swing, grid current and blocking of stage 1, times the On fade. */
        float getHeat() const { return m_heat * m_fade.getValue(); }

        /** Current small-signal gain drop of stage 1 from the bias shift, dB (<= 0). */
        float getBlockingDb() const { return m_blockingDb * m_fade.getValue(); }

        // ---- Exposed for tests and the calibration scripts
        /** The normalised triode transfer y(g) at the static bias: 0 with unity slope at the rest point g = -1. */
        static double triode (double g);

        /** Static makeup in dB for Drive a (0..1) and Tone t (-1..1). */
        static double makeupDb (double a, double t);

        /** The internal drives into stage 1 and stage 2 (linear gains) for Drive a. */
        static double drive1For (double a);
        static double drive2For (double a);

    private:
        struct Channel
        {
            Biquad micBell, micShelf;
            Oversampler2x os;
            TptOnePole miller1, miller2;
            AdaaTable::State adaa1, adaa2;
            Biquad couplingTreble;   // coupling HPF 12 Hz x 1st-order treble shelf 2.5 kHz
            Biquad bass;             // 1st-order low shelves 80 Hz x 300 Hz
            Biquad scoop;            // plate scoop bell 200 Hz
            Biquad trebleTop;        // 2nd-order treble shelf 12 kHz
            Biquad fixedTop;         // fixed top lift
            TptOnePole outLp;
            Biquad outHp;
            double bypass1 = 0.0, bypass2 = 0.0;   // previous Miller bypass samples (2-tap average)
        };

        // Smoothed control values at a control-block boundary.
        struct Control
        {
            double drive1 = 1.0, drive2 = 1.0, makeup = 1.0, bypass2 = 1.0;
        };

        void resetWet();
        void updateControl();
        void designFilters();
        void updateHeat();
        double bias1() const;

        static const AdaaTable& table();

        double m_fs = 48000.0, m_fs2 = 96000.0;
        OnFade m_fade;
        std::array<Channel, 2> m_ch;

        // Targets from setParams, and their smoothed values (per control block).
        double m_targetA = 0.35, m_targetTone = 0.0, m_targetMic = 0.0;
        double m_a = 0.35, m_tone = 0.0, m_mic = 0.0;
        double m_designedA = -1.0, m_designedTone = -9.0, m_designedMic = -1.0;
        double m_smoothCoeff = 1.0;
        bool m_snap = true;

        Control m_from, m_to;
        int m_ctlPos = 0;
        double m_kb1 = 0.6;

        // Shared dynamic-bias envelopes (2x rate).
        double m_env1 = 0.0, m_env2 = 0.0;
        double m_env1Attack = 0.0, m_env1Release = 0.0, m_env2Attack = 0.0, m_env2Release = 0.0;

        // Heat meter (per control block).
        double m_swingPeak = 0.0;
        double m_vu1 = 0.0, m_vu2 = 0.0, m_vuCoeff = 0.0;
        float m_heat = 0.0f;
        float m_blockingDb = 0.0f;
    };
}

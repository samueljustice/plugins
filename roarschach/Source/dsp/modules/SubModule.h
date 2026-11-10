#pragma once

#include "../../Params.h"
#include "../common/Module.h"
#include "../common/TptSvf.h"

#include <array>
#include <cstdint>

namespace roar
{
    /** SUB settings, filled by the processor once per chunk (spec §5.13, §7). */
    struct SubSettings
    {
        bool on = Params::defaultOf (Params::subOn) > 0.5f;
        float level = Params::defaultOf (Params::sub) / 100.0f;        // u, 0..1
        float drive = Params::defaultOf (Params::subDrive) / 100.0f;   // u, 0..1
        Params::SubRegister reg = static_cast<Params::SubRegister> (static_cast<int> (Params::defaultOf (Params::subRegister)));
        Params::SubCharacter character = static_cast<Params::SubCharacter> (static_cast<int> (Params::defaultOf (Params::subCharacter)));
    };

    /**
     * SUB (spec §5.13): a tracked divider octave-down with a tight band-pass and a little drive,
     * added to the voice. Mono in (the mid when stereo), the same layer added to both channels;
     * the dry is never attenuated.
     *
     *   x --> band 1: 2 x TPT SVF band-pass (Q 2, 0 dB at fIn) --> b1 --> divider: u1 = q1 b1
     *     \-> band 2 (Thick only): the same at 1.5 fIn ----------> b2 --> divider: u2 = q2 b2
     *   s = u1 (+ 0.5 u2) -----------------------------------------------------------------------+
     *   u1 --> SVF at fsub: rotated (lp^2 - bp^2, 2 lp bp) / normaliser (2 fsub) x even gain ---------+
     *     (the divided layer s, plus the even harmonic, before the drive)
     *     --> HP 2nd order at 0.6 fsub --> LP 4th-order Butterworth at 1.5 fsub (the clean sub)
     *     --> / level normaliser --> ADAA-1 biased tanh (Drive) --> x normaliser
     *     --> LP 2nd-order Butterworth at 4 fsub (keeps the drive's 2nd and 3rd harmonics)
     *     --> x level law (re band 1's amplitude, i.e. H1) x floor expander --> added to l (and r)
     *
     * Band centre fIn: the VoiceStream f0 when its confidence is >= 0.4, otherwise held (the
     * dividers keep running on whatever is in the band); 140 Hz before any pitch. An octave jump
     * in f0 must persist for 40 ms. The Register chip picks the harmonic that is divided:
     *   Follow  f0 (sub = f0 / 2); 2 f0 when f0 / 2 < 42 Hz (sub = f0)
     *   Tight   the lowest of f0 / 2, f0, 2 f0 that is 55 Hz or above (divides f0, 2 f0 or 4 f0)
     *   Deep    f0 (sub = f0 / 2) down to a 32 Hz sub; below that 2 f0 (sub = f0)
     * Register switches have 3 % hysteresis. fIn and fsub glide in log2 over 30 ms. When the
     * stream is not confident, fsub (the output filters) follows the dividers' own toggle rate.
     *
     * Divider: envelope e = |b| (1 ms attack, 30 ms release); armed when b < -0.15 e; an armed
     * upward zero crossing toggles q. u = q b has f / 2 at 0.849 |b| and nothing at f.
     *
     * Drive: -12 + 24 uDrive dB into f(v) = (tanh (v + beta) - tanh beta) / sech^2 beta, beta 0.1
     * (Clean) or 0.25 (Resonant, Thick), ADAA-1 at the base rate, on the band-passed sub (a near
     * sine), so its 2nd and 3rd harmonics are what Drive adds and the 4 fsub low-pass keeps them.
     * The normaliser is the level of the band-passed sub with an instant attack and a 300 ms
     * release, read as the RMS of a sine (peak / sqrt 2), floored at -60 dBFS: Drive means the same
     * at any level, and an onset is never squashed by a slow level estimate. Up to about 20 % the
     * products stay more than 30 dB under the sub (inaudible).
     *
     * Even harmonic: u = q b, so u^2 = b^2 and no curve of u can make the sub's 2nd harmonic (it
     * sits at f, and only odd multiples of f / 2, DC and multiples of f come out). It is added
     * explicitly: an SVF at fsub gives a low-pass and a band-pass of u1 that are equal and 90
     * degrees apart at fsub, so lp^2 - bp^2 and 2 lp bp are the doubled frequency in quadrature,
     * with no DC and no envelope step. Their mix is rotated so the 2nd harmonic sits in phase
     * with the divided input partial (it adds to it, never thins it). Its level re the sub at the
     * output is evenDbFor (Drive): -30 dB at -6 dB, -25 dB at 0 dB, -20 dB at +6 dB (the
     * default), capped at -12 dB.
     *
     * Level law: the sub's f / 2 component = H1 (the band 1 amplitude) x
     * 10^((-14 + 24 uLevel^0.5) / 20) in the steady state (laws::subLevelDb; 50 % -> +3.0 dB), faded linearly to
     * nothing below u = 0.02 (0 is an exact null). A static table (built in prepare()) removes
     * the drive's fundamental gain and the output filters' gain at fsub, so the law holds at every
     * Drive and character. Floor expander: -60 dBFS on band 1's envelope, 1:2, 1 ms / 50 ms.
     *
     * Latency 0. Control updates run every 32 samples on the VoiceStream frame grid, so the output
     * does not depend on the block size. No allocation after prepare(). NaN-safe.
     */
    class SubModule final : public Module
    {
    public:
        static constexpr double c_initialInputHz = 140.0;
        static constexpr int c_driveTableSize = 97;   // -6 .. +18 dB in 0.25 dB steps

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return 0; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const SubSettings& s);

        // Readouts.
        /** Smoothed peak level of the added layer, linear (scaled by the On fade). */
        float getLevel() const { return m_levelEnv * m_fade.getValue(); }
        /** The level as 0..1 for the views: -60..-12 dBFS (scaled by the On fade). */
        float getLevelNormalised() const;
        /** Frequency of the octave layer (the output band centre), Hz. */
        float getSubHz() const;
        /** The band-1 (divided harmonic) centre, Hz. */
        float getInputHz() const;

        // Static parts of the design, for tests.
        /** Sub level re H1 for a Level knob value u, in dB (-inf below the fade -> -200). */
        static double levelLawDb (double u);
        static double levelLawGain (double u);
        /** Drive in dB for the Drive knob value u. */
        static double driveDbFor (double u) { return -6.0 + 24.0 * u; }
        /** The sub's 2nd harmonic re its fundamental at the output, in dB, for a drive in dB. */
        static double evenDbFor (double driveDb);

    private:
        struct Divider
        {
            float env = 0.0f;
            float prev = 0.0f;
            float q = 1.0f;
            bool armed = false;
            int sinceToggle = 0;
        };

        void resetWet();
        void updateControls (const VoiceFrame& frame);
        void updateTracker (const VoiceFrame& frame);
        void updateFilters();
        float processDivider (Divider& d, float b, bool estimate);
        double curve (double v) const;
        double antiderivative (double v) const;
        double driveMakeup (double driveDb, int betaIndex) const;
        float gainTarget() const;   // level law x drive makeup x filter makeup
        float evenGainTarget() const;
        int betaIndex() const { return m_settings.character == Params::SubCharacter::clean ? 0 : 1; }

        SubSettings m_settings;
        OnFade m_fade;
        double m_fs = 48000.0;

        // Filters.
        TptSvf m_band1a, m_band1b, m_band2a, m_band2b;
        TptSvf m_hp, m_lp1, m_lp2, m_lpPost;
        Divider m_div1, m_div2;

        // Drive.
        double m_norm = 0.0;           // instant-attack, 300 ms release level of the band-passed sub (the drive's normaliser)
        double m_evenNorm = 0.0;       // the same for s (scales the even harmonic)
        double m_normRelease = 0.0;
        double m_adaaX1 = 0.0, m_adaaF1 = 0.0;
        double m_beta = 0.1, m_tanhBeta = 0.0, m_coshBeta2 = 1.0;
        std::array<std::array<float, c_driveTableSize>, 2> m_driveMakeup {};   // per beta: 1 / the drive's fundamental gain on a sine
        float m_dividedFundamental = 0.849f;   // the divided waveform's f / 2 component per unit band amplitude
        std::array<float, 3> m_filterMakeup {};                                // per character
        TptSvf m_even;                 // at fsub: lp and bp of u1 for the 2nd harmonic
        std::array<float, 3> m_evenScale {};   // per character: output-filter tilt / unit H2 of the doubler
        float m_evenGain = 0.0f;

        // Gains.
        float m_gain = 0.0f;           // smoothed: level law x drive makeup x filter makeup
        float m_gainTarget = 0.0f;
        float m_gainCoeff = 0.0f;
        float m_driveDb = 6.0f;        // smoothed per control frame
        float m_driveCoeff = 0.0f;     // per control frame
        float m_driveGain = 2.0f;
        float m_expGain = 0.0f;
        float m_expAttack = 0.0f, m_expRelease = 0.0f;
        float m_envAttack = 0.0f, m_envRelease = 0.0f;

        // Tracking (per control frame).
        double m_f0Log = 0.0;          // log2 of the followed f0
        bool m_hasPitch = false;
        double m_candidateLog = 0.0;
        int m_candidateSamples = 0;
        int m_octaveConfirmSamples = 1920;
        bool m_tracked = false;
        int m_multiplier = 1;          // which harmonic band 1 sits on (1 = f0, 2 = 2 f0, 4 = 4 f0)
        double m_inLog = 0.0;          // glided log2 fIn
        double m_subLog = 0.0;         // glided log2 fsub (output filters)
        double m_estimateLog = 0.0;    // log2 of the band-1 frequency from the divider toggles
        double m_glideCoeff = 0.0;     // per control frame
        bool m_controlsValid = false;

        // Readouts.
        float m_levelEnv = 0.0f;
        float m_levelCoeff = 0.0f;
    };
}

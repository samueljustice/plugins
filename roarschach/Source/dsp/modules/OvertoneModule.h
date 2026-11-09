#pragma once

#include "../common/Biquad.h"
#include "../common/HilbertPair.h"
#include "../common/Module.h"
#include "../common/TptSvf.h"

#include <array>

namespace roar
{
    /** OVERTONE Pattern chip, in the order of the parameter's choices. */
    enum class OvertonePattern
    {
        chest = 0,
        throat,
        beast,
        speaker,
        broad,
        tracked
    };

    /** OVERTONE Mode chip, in the order of the parameter's choices. */
    enum class OvertoneMode
    {
        add = 0,
        lean,
        replace
    };

    /** OVERTONE settings, filled by the processor once per chunk. */
    struct OvertoneSettings
    {
        bool on = true;
        float amount = 0.30f;                              // u, 0..1
        float freqOct = 0.0f;                              // -1..+1 octaves
        OvertonePattern pattern = OvertonePattern::chest;
        OvertoneMode mode = OvertoneMode::add;
    };

    /** One row of the pattern table (centres at Frequency = 0 oct). */
    struct OvertonePatternSpec
    {
        struct BandSpec
        {
            double hz;                       // centre (Tracked: multiple of f0, 0.5 or 1)
            double q;
            double levelDb;
            std::array<double, 5> weightsDb; // H2..H6 re the band's fundamental; <= c_offDb is off
            double hpRatio, lpRatio;         // output HP / LP as multiples of the centre
        };

        BandSpec a, b;
        bool hasB;
        double crossoverHz;                  // Lean / Replace split (Tracked: fixed, x r)
        bool tracked;
    };

    /**
     * OVERTONE: harmonics of the low band, so the weight of a voice (and of SUB's octave) is
     * heard on speakers that cannot play it.
     *
     *   x (mid when stereo) --> band A generator --+
     *                       \-> band B generator --+--> bell 280 Hz -4 dB --> x gain law --> h
     *   dry per channel: Add = untouched | Lean / Replace = LR4 split at fx, low x {0.5, 0} + high
     *   out = dry + h   (h is added equally to both channels)
     *
     * Band generator: two cascaded TPT SVF band-passes (0 dB peak) at fb and a 2nd-order
     * low-pass at 3 fb (gain at fb compensated) that keeps the midrange out, a Hilbert pair whose
     * analytic output z is rotated so Re(z) is in phase with the band at fb, the unit phasor
     * u = z / |z| raised to the powers 2..6, and
     *   g = A * f(A_env) * sum_k w_k Re(u^k),  A = |z|
     * so a steady sine of amplitude A gives exactly A w_k cos(k (wt + phi)) per harmonic, phase
     * locked to the band's own waveform, with no DC, no frames and zero latency. The output is
     * high-passed (4th-order Butterworth at hpRatio fb) and low-passed (2nd-order at lpRatio fb)
     * and scaled by the band level.
     *
     * Dynamics (per 32-sample control frame, interpolated per sample):
     *   f = clamp ((A_env / A_R)^(gamma - 1), -12 dB, +6 dB) x voicing x floor expander
     *   A_env: 3 ms attack / 40 ms release of A; A_R = -20 dBFS (0.1); gamma = 1.15
     *   voicing = 0.25 + 0.75 v, v = smoothstep (0.2 .. 0.8) of the VoiceStream confidence,
     *             smoothed over 20 ms (breaths and consonants generate >= 9 dB less)
     *   floor expander: 1:2 below A_env = -60 dBFS, 1 ms / 50 ms
     *
     * Gain law: L(u) = -20 + 26 u^0.6 dB (30 % -> -7.4 dB, 100 % -> +6 dB), faded linearly to
     * nothing below u = 0.02 (0 is a bit-exact null in Add), plus +1.5 dB (Lean) or +3 dB
     * (Replace). Smoothed over 20 ms.
     *
     * Tracked: A at f0 / 2, B at f0 from the VoiceStream (confidence >= 0.4, otherwise held;
     * 140 Hz before any pitch; octave jumps must persist 40 ms; 30 ms log glide). Frequency then
     * tilts the weights, w_k += 6 oct log2 (k / 3) dB, instead of moving the centres. Fixed
     * patterns: every centre (and fx) x 2^oct, centres clamped to 32..500 Hz.
     *
     * Pattern change: the generator ramps to 0 over 10 ms, the pattern is swapped (filter states
     * kept, centres glide), and it ramps back over 10 ms. Mode change: the dry variants crossfade
     * over 20 ms (smoothstep); the split idles in Add and restarts from clean state when a fade
     * away from Add begins. Amount 0 skips the generator (its state restarts when it returns).
     *
     * Latency 0. Control updates run on the VoiceStream frame grid, so the output does not depend
     * on the block size. No allocation after prepare(). NaN-safe.
     */
    class OvertoneModule final : public Module
    {
    public:
        static constexpr int c_numBands = 2;
        static constexpr int c_numHarmonics = 5;            // H2..H6
        static constexpr double c_offDb = -200.0;
        static constexpr double c_gamma = 1.15;
        static constexpr double c_referenceAmplitude = 0.1; // A_R, -20 dBFS
        static constexpr double c_minCentreHz = 32.0, c_maxCentreHz = 500.0;
        static constexpr double c_bellHz = 280.0, c_bellQ = 0.9, c_bellDb = -4.0;
        static constexpr double c_initialF0Hz = 140.0;

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return 0; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;

        void setParams (const OvertoneSettings& s);

        // ---- Readouts (all scaled by the On fade where they are levels)
        /** Generated layer re the module input, 0..1 over -36..0 dB. */
        float getLevelNormalised() const;
        /** Drive of harmonic H(k + 2), k = 0..4, 0..1, for the visuals. */
        float getHarmonicDrive (int k) const;
        /** Band b's generated level re its own input, 0..1 over -24..+6 dB (the footer bar). */
        float getBandLevelNormalised (int band) const;
        /** Band b's current centre, Hz (0 when the band is off in the active pattern). */
        float getBandHz (int band) const;
        /** The mode chip asks for Replace (the footer's "R" glyph, the hollow monster read). */
        bool isReplace() const { return m_settings.mode == OvertoneMode::replace; }
        /** The pattern currently sounding (it lags a pattern change by up to 10 ms). */
        OvertonePattern getActivePattern() const { return m_s.activePattern; }

        // ---- Static parts of the design (tests, editor)
        static const OvertonePatternSpec& patternSpec (OvertonePattern p);
        /** L(u) in dB; -200 at u = 0. */
        static double amountLawDb (double u);
        static double amountLawGain (double u);
        static double modeMakeupDb (OvertoneMode m);

    private:
        struct Band
        {
            TptSvf bp1, bp2, iso, hp1, hp2, lp;
            HilbertPair hilbert;
            float isoGain = 1.0f;                // 1 / |isolation LP| at the centre
            double rhoRe = 1.0, rhoIm = 0.0;     // rotation that aligns Re(z) with the band at fb
            double env = 0.0;                    // A_env
            float factor = 0.0f, factorStep = 0.0f;
            float expGain = 0.0f;
            std::array<float, c_numHarmonics> weights {};
            float level = 1.0f;
            float outEnv = 0.0f;                 // readout
            double logHz = 0.0;                  // glided log2 centre
            double appliedLogHz = 0.0;           // the centre the coefficients were set for
        };

        struct Crossover
        {
            TptSvf split, low, high;
        };

        // Everything that is state, as one copyable value (copyStateFrom = assignment).
        struct State
        {
            std::array<Band, c_numBands> bands;
            Biquad bell;
            Crossover xoL, xoR;
            double logFx = 0.0, appliedLogFx = 0.0;
            OvertonePattern appliedPattern = OvertonePattern::chest;   // coefficients
            OvertonePattern weightsPattern = OvertonePattern::chest;   // weights
            double weightsOct = 0.0;

            OvertonePattern activePattern = OvertonePattern::chest;
            bool bActive = true;
            bool pendingSwap = false;
            float swapGain = 1.0f;

            float gain = 0.0f, gainTarget = 0.0f;
            bool genRunning = false;

            // Dry variants: weights of raw, low, high, crossfaded.
            std::array<float, 3> modeFrom { 1.0f, 0.0f, 0.0f }, modeTo { 1.0f, 0.0f, 0.0f };
            OvertoneMode modeTarget = OvertoneMode::add;
            int modePos = 0;

            double oct = 0.0;                    // smoothed Frequency
            double confidence = 0.0;             // smoothed voicedness v
            double f0Log = 0.0;
            bool hasPitch = false;
            double candidateLog = 0.0;
            int candidateSamples = 0;
            int interpLeft = 0;
            bool controlsValid = false;

            float inEnv = 0.0f, genEnv = 0.0f;
            std::array<float, c_numHarmonics> harmShape {};
        };

        void resetGenerator();
        void restartFromOff();
        void runBand (Band& band, const float* x, float* sum, int count, int interpCount);
        void updateControls (const VoiceFrame& frame);
        void updateTracker (const VoiceFrame& frame);
        void applyPatternStatics();
        void setFilters (bool force);
        void computeWeights (bool force);
        float gainTargetFor() const;
        double targetLogHz (int band) const;
        double targetLogFx() const;

        OvertoneSettings m_settings;
        OnFade m_fade;
        State m_s;

        double m_fs = 48000.0;
        double m_envAttackD = 0.0, m_envReleaseD = 0.0;  // per sample
        double m_expAttack = 0.0, m_expRelease = 0.0;    // per frame
        double m_glideCoeff = 0.0, m_octCoeff = 0.0, m_confCoeff = 0.0;   // per frame
        float m_gainCoeff = 0.0f, m_readoutCoeff = 0.0f; // per sample
        float m_swapStep = 0.0f;
        int m_modeFadeLength = 960;
        int m_octaveConfirmSamples = 1920;
    };
}

#pragma once

#include "../../Params.h"
#include "../PitchDetector.h"
#include "../common/Biquad.h"
#include "../common/DelayLine.h"
#include "../common/Module.h"
#include "../common/Oversampler2x.h"
#include "../common/VoiceStream.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace roar
{
    /** PITCH settings, filled once per chunk by the processor (spec §5.2, §7). */
    struct PitchSettings
    {
        bool on = true;
        float pitchSemitones = -2.0f;   // -24..+12 st
        float size = 0.0f;              // u, 0..1
        float drive = 0.25f;            // u, 0..1
        Params::PitchMode mode = Params::PitchMode::shift;
        Params::PitchFormant formant = Params::PitchFormant::follow;
    };

    /**
     * PITCH: formant-following TD-PSOLA pitch shift with a 2x oversampled tube drive (spec §5.2).
     *
     *   in --> ring buffers (L, R) ------------------------------------------------+--> aligned dry
     *     \--> mid --> PitchDetector (YIN) --> period, voicing                     |    (OnFade)
     *     \--> mid --> band-limited copy --> pitch marks on the waveform peaks     |
     *                                                                              v
     *   synthesis marks every period / ratio --> nearest analysis mark --> 2-period Hann grain read
     *   at the formant rate phi --> overlap-add (L and R on the same marks) --> Size body --> drive
     *
     * Where no mark is convincing (consonants, breaths, an irregular or creaky voice), unvoiced
     * grains crossfade to a resampling path instead: two read heads sweeping 20 ms read the input
     * continuously at phi (readTape). Long continuous reads keep the fine structure of the input;
     * short grains cut at a foreign rate smear it. In Follow (phi = ratio) this path is itself a
     * correct pitch and formant shift, so marks must correlate >= 0.85 there; where phi and the
     * ratio differ (Keep, Size) only the grains can shift the pitch and 0.5 is enough.
     *
     * Pitch ratio p = 2^(P/12). Formant semitones F = (Follow ? applied shift : 0) - 6 Size; the
     * grain read rate is phi = 2^(F/12), clamped to [0.25, 1.414]. Follow with Size 0 reads every
     * grain at the pitch ratio, so the voiced output is the input resampled period by period: the
     * formants move exactly with the pitch and the harmonic ladder stays clean. Voiced gain is
     * sqrt (phi / ratio), exactly 1 in Follow.
     *
     * Native rate, full band: the audio is never resampled. Grains and the resampling path read
     * the input rings at the host rate through a 32-zero-crossing Kaiser-windowed sinc (table
     * driven, normalised to unity DC), so at 176.4 / 192 kHz a -12 / -24 st Follow shift brings
     * ultrasonic content (e.g. 40 kHz -> 10 kHz) down into the audible band without imaging.
     * Reading faster than the input (phi > 1) widens the kernel by phi, so the band that would
     * fold back is removed instead of aliasing.
     *
     * Modes:
     *   Shift  ratio = p (20 ms knob smoothing, 5 ms slew).
     *   Snap   per voiced grain, the local input period gives m_in = 69 + 12 log2 (f / 440); the
     *          output note is round (m_in + P), kept while it stays within 0.5 + 0.3 st (hysteresis);
     *          the ratio slews to it with tau 5 ms and is held through unvoiced passages.
     *   Mono   output f_t = 130.81 * 2^(P/12) Hz (centred on C3); f_t / f_in is clamped to
     *          [0.25, 4], outside that it falls back to Shift. Follow then uses P (the knob), so
     *          the formants do not follow the speaker's intonation.
     *
     * Size: formants down by up to 6 st, body peaks (+2 dB at 250 and 500 Hz at full Size) and a
     * 1..4 ms throat comb of gain 0.15 Size. All of it is inactive at Size 0.
     *
     * Drive (wet path, after the shifter; 2x oversampled, 23 samples):
     *   g = dB (24 d^1.5);  x1 = HighShelf (+3 d dB, 2.5 kHz) (x);  env = follower (|x1|, 5 / 80 ms)
     *   bias = (0.18 - 0.25 env) d^0.6;  y = S (g x1 + bias) - S (bias)
     *   S (u) = u >= 0 ? tanh (1.4 u) / 1.4 : tanh (0.8 u) / 0.8      (plate side limits sooner)
     *   out = DcBlock5Hz (LP1 (16 -> 7.5 kHz) (HighShelf (-3 d dB) (y))) * g^-0.75 * makeup (d)
     * The curve is smooth through its second derivative at 0, so the biased operating point gives
     * H2 first and H3 only once it is pushed. It runs at 2x with first-order antiderivative
     * anti-aliasing (closed form, log cosh), which adds half a 2x-rate sample (a quarter of a base
     * sample) of group delay to the drive path only. At Drive 0 the drive is a pure 23-sample delay.
     *
     * Latency: ceil (0.030 fs) + 23, constant (1463 at 48 kHz) over the whole -24..+12 st range. The
     * longest grain (-24 st Follow, phi 0.25, 50 Hz voice: half-width 4 periods) would read up to
     * 3 periods ahead of its mark at its start; such a grain takes an earlier mark (up to 3 periods
     * back, the same borrowing the low-pitch case already uses), so no read ever passes the newest
     * input (kernel reach included) and the latency does not grow. At Pitch 0 / Size 0 / Drive 0 in
     * Shift the module is an exact delay (the shifter crossfades to its aligned input over 20 ms).
     *
     * Stereo: the mid is analysed; L and R are synthesised from their own rings with the same
     * marks and grains, then through per-channel body and drive (one shared drive envelope).
     *
     * Off: the input rings, the detector and the mark finder keep running (cheap), so turning the
     * module back on picks up the voice at once; grains, body and drive stop.
     *
     * No allocation after prepare(), no locks, NaN-safe, block-size independent.
     */
    class PitchModule final : public Module, public VoiceRewriter
    {
    public:
        PitchModule();

        // Module
        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return m_latency; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        VoiceRewriter* getVoiceRewriter() override { return this; }

        // VoiceRewriter: f0 of the frames that line up with the chunk just output.
        void rewriteVoice (VoiceFrame* frames, int numFrames) override;

        void setParams (const PitchSettings& s);

        /** ceil (0.030 fs) + Oversampler2x::c_latency. */
        static int latencyFor (double fs);

        // Readouts (any thread, relaxed). Scaled by the On fade: an Off module reads 0 / unvoiced.
        float getAppliedSemitones() const { return m_readShiftSt.load (std::memory_order_relaxed); }
        float getFormantSemitones() const { return m_readFormantSt.load (std::memory_order_relaxed); }
        float getDriveHeat() const { return m_readHeat.load (std::memory_order_relaxed); }   // 0..1
        float getOutputF0() const { return m_readF0.load (std::memory_order_relaxed); }      // Hz, 0 = none
        bool isVoiced() const { return m_readVoiced.load (std::memory_order_relaxed); }

        // Diagnostics for tests.
        std::uint64_t getVoicedGrainCount() const { return m_core.voicedGrains; }
        std::uint64_t getUnvoicedGrainCount() const { return m_core.unvoicedGrains; }
        float getPitchRatio() const { return static_cast<float> (m_core.ratio); }

    private:
        struct Grain
        {
            double synCentre = 0.0;   // output-time centre
            double anaCentre = 0.0;   // input-time centre
            double halfWidth = 1.0;   // output samples
            double invHalfWidth = 1.0;
            double rate = 1.0;        // input samples read per output sample
            float gain = 1.0f;
            bool voiced = true;
        };

        struct Mark
        {
            std::int64_t pos = 0;
            float period = 0.0f;
        };

        static constexpr int c_maxMarks = 256;
        static constexpr int c_maxGrains = 48;
        static constexpr int c_windowSize = 1024;
        static constexpr int c_makeupPoints = 11;

        struct DriveChannel
        {
            Biquad pre, post, lowpass, dcBlock;
            Oversampler2x os;
            double previous = 0.0;         // last 2x-rate curve input (ADAA)
        };

        /** Every piece of plain, trivially copyable state (copyStateFrom copies it in one go). */
        struct Core
        {
            std::int64_t writePos = 0;     // next input sample index

            // Band-limited copy for the peak search.
            float bandHpX1 = 0.0f, bandHpY1 = 0.0f;
            Biquad bandLp;

            // Pitch marks.
            std::array<Mark, c_maxMarks> marks {};
            int markHead = 0;
            int markCount = 0;
            std::int64_t lastMark = -1;
            bool haveLastMark = false;
            double markPeriod = 0.0;       // spacing of the last two marks of the current run
            std::int64_t shortTriedFor = -1;
            std::int64_t startRetry = 0;
            float positivePeaks = 0.0f, negativePeaks = 0.0f, polarity = 1.0f;

            // Grains.
            std::array<Grain, c_maxGrains> grains {};
            int grainCount = 0;
            double nextSyn = 0.0, prevSyn = 0.0, prevHalfWidth = 0.0;
            bool prevVoiced = false;
            double tapePhase = 0.0;        // read-head phase of the resampling path (0..1)
            double tapeXY = 0.0, tapeXX = 0.0, tapeYY = 0.0;   // running head correlation
            double voicedShare = 0.0;      // smoothed share of the output from voiced grains

            // Parameter targets.
            float pitchTarget = -2.0f, sizeTarget = 0.0f, driveTarget = 0.25f;
            Params::PitchMode mode = Params::PitchMode::shift;
            Params::PitchFormant formant = Params::PitchFormant::follow;

            // Smoothed values.
            double knobSt = -2.0;          // 20 ms smoothed pitch knob, semitones
            double logRatio = 0.0;         // applied pitch ratio, natural log
            double ratio = 1.0;
            double modeTarget = 0.0;       // Snap / Mono: target log ratio from the last voiced grain
            std::int64_t modeUpdatePos = 0;
            int snapNote = 0;
            bool haveSnapNote = false;
            bool monoInRange = false;
            double size = 0.0;
            double rate = 1.0;             // grain read rate phi
            float wet = 0.0f;              // shifter vs its aligned input (neutral crossfade)

            // Size body.
            bool bodyActive = false;
            float bodyTrim = 1.0f;
            int throatWrite = 0;

            // Drive.
            double drive = 0.25;           // smoothed u
            double driveCoeffFor = -1.0;   // the drive value the coefficients were made for
            float driveMix = 0.0f;         // 0 delay only .. 1 drive
            int driveFadePos = 0;
            bool driveRunning = false;
            float driveEnv = 0.0f;
            float driveGain = 1.0f, driveOutGain = 1.0f, driveBiasScale = 0.0f;
            float driveHeat = 0.0f;

            std::uint64_t voicedGrains = 0, unvoicedGrains = 0;
        };

        // Per-sample pieces.
        void analyse (float xl, float xr, bool stereo);
        void smoothParams();
        float synthesise (float& right, bool stereo);
        void applyBody (float& yl, float& yr, bool stereo);
        void applyDrive (float& yl, float& yr, bool stereo);
        void restartWet();

        // Shifter (TD-PSOLA).
        void findMarks();
        bool searchPeak (std::int64_t from, std::int64_t to, double predicted, double tolerance, std::int64_t& best, float& bestValue);
        double markCorrelation (std::int64_t mark, std::int64_t previous) const;
        void pushMark (std::int64_t pos, float period);
        void pruneMarks (std::int64_t outPos);
        void scheduleGrains (std::int64_t outPos);
        void createGrain (std::int64_t outPos);
        double localPeriod (int markIndex) const;
        void updateModeRatio (double periodSamples, std::int64_t outPos);
        void updateRate();
        float renderGrains (std::int64_t outPos, float& right, bool stereo);
        /** Band-limited read of both rings at input position pos for read rate `rate`; returns L. */
        float readRings (double pos, double rate, float& right, bool stereo) const;
        float window (double u) const;
        double markThreshold() const;
        void readTape (double now, bool needed, float& left, float& right, bool stereo);

        void updateBodyCoeffs();
        void updateDriveCoeffs();
        void resetDriveState();
        float makeup (double d) const;

        void clearAfterNonFinite();

        // Sample-rate constants (prepare).
        double m_fs = 48000.0;
        int m_maxBlock = 512;
        int m_psolaLatency = 1440;
        int m_latency = 1463;
        int m_minPeriod = 48;
        int m_maxPeriod = 960;
        double m_unvoicedHalfWidth = 480.0;
        std::int64_t m_mask = 0;
        double m_knobCoeff = 0.001;        // 20 ms
        double m_slewCoeff = 0.004;        // 5 ms
        double m_slewTauSamples = 240.0;
        float m_wetStep = 0.001f;
        int m_driveFadeLength = 960;
        float m_envAttack = 0.004f, m_envRelease = 0.0003f;
        double m_shareCoeff = 0.002;
        double m_tapeCorrCoeff = 0.002;
        float m_bandHpCoeff = 0.992f;
        int m_throatSize = 220;
        std::array<float, c_windowSize + 2> m_cosTable {};

        // State.
        Core m_core;
        PitchDetector m_detector;
        std::vector<float> m_inL, m_inR, m_band;
        std::array<std::array<Biquad, 2>, 2> m_body {};   // [channel][peak]
        std::vector<float> m_throatL, m_throatR;
        std::array<DriveChannel, 2> m_driveCh {};
        DelayLine m_driveDelayL, m_driveDelayR;
        OnFade m_fade;

        // Per-frame record of the chunk just output, for rewriteVoice().
        std::vector<float> m_frameLogRatio, m_frameTargetHz;
        int m_frameCount = 0;

        // Readouts.
        std::atomic<float> m_readShiftSt { 0.0f };
        std::atomic<float> m_readFormantSt { 0.0f };
        std::atomic<float> m_readHeat { 0.0f };
        std::atomic<float> m_readF0 { 0.0f };
        std::atomic<bool> m_readVoiced { false };
    };
}

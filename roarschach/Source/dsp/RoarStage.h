#pragma once

#include "VoiceInfo.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace roar
{
    /**
     * ROAR: tube-style saturation driven through bass and treble, then an automatic top-band tame.
     *
     *   in --> pre-emphasis (low shelf @ 180 Hz, high shelf @ 3 kHz, +0..9 dB) --> 2x up
     *      --> [ dry (half-sample aligned) + wet: drive --> ADAA asymmetric tube curve --> DC block
     *            --> level match ] --> 2x down
     *      --> de-emphasis (exact inverse shelves) --> post LPF (18 -> 9 kHz, faded in at high drive)
     *      --> tame: dynamic high shelf @ 3.5 kHz, 4:1 on the level above 3.5 kHz --> out
     *
     *   - Emphasis before the curve and its exact inverse after it means the dry part of the
     *     signal comes out flat, while the lows and highs are the parts that hit the curve hardest:
     *     a chesty, raspy distortion instead of a mid-range fuzz.
     *   - The curve is tanh with a bias (the positive side clips first, like a single-ended tube),
     *     normalised to unity small-signal slope, with first-order antiderivative anti-aliasing.
     *     Together with 2x oversampling that keeps the aliasing of a hard-driven voice low.
     *     The blend with the dry signal happens inside the oversampled domain, so both paths go
     *     through the same filters and stay phase-aligned (no comb filtering at any Amount).
     *   - The shaped signal is level-matched to the dry (~200 ms, stereo-linked, held in pauses),
     *     so Amount changes the colour, not the loudness, at any input level.
     *   - Saturation piles energy into the presence band; the tame takes it back. Its threshold
     *     sits 3 dB above the band's own running level (rises in 250 ms, falls in 1.5 s), so it
     *     only catches what sticks out: about 1 dB on a speech-like line, at most 6 dB. It is a
     *     shelf, exactly flat at 0 dB, so the lows are never touched and there is no crossover
     *     phase shift when it is idle.
     *
     * Amount: wet = Amount (35 % at the default), drive 4..30 dB, emphasis 0..9 dB, bias rises.
     * At Amount 0 the stage is the oversampling filters only (flat, a fixed delay).
     *
     * Latency: the oversampler's (equiripple FIR half-band, up and down): 49 samples at every
     * sample rate, an integer, so no fractional-delay compensation, and constant for every
     * Amount. The FIR is linear phase, so the stage is a pure delay at Amount 0 and a partial
     * MIX against the delayed dry does not comb. The ADAA half-sample at 2x adds 0.25 (not
     * reported).
     * Stereo when `stereo` (stereo-linked), otherwise left only. No allocation after prepare().
     */
    class RoarStage
    {
    public:
        RoarStage();
        ~RoarStage();

        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        int getLatencySamples() const { return m_latency; }

        // amount01 0..1, smoothed internally (30 ms).
        void setParams (float amount01);

        void process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo& voice);

        // Effective drive 0..1 (Amount x how hard the signal hits the curve), smoothed, for the visualiser.
        float getDrive() const { return m_driveDisplay; }

        // Current top-band gain reduction in dB (>= 0).
        float getTameDb() const { return m_tameDisplayDb; }

    private:
        static constexpr int c_subBlock = 32;

        struct Biquad
        {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
            double z1 = 0.0, z2 = 0.0;

            float process (float x)
            {
                const double y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return static_cast<float> (y);
            }

            void clear() { z1 = z2 = 0.0; }
            bool isFinite() const { return std::isfinite (z1) && std::isfinite (z2); }
        };

        struct Channel
        {
            Biquad preLow, preHigh, deLow, deHigh, post;
            Biquad detectA, detectB;          // 4th-order high-pass: the tame detector's top band
            Biquad tame;                      // dynamic high shelf (0 dB = exactly flat)
            double prevV = 0.0;               // ADAA history (curve input at 2x)
            float prevU = 0.0f;               // dry history at 2x
            float dcX = 0.0f, dcY = 0.0f;
            float designedEmphasisDb = -1.0f;  // last designs, so unchanged sub-blocks skip the maths
            float designedPostHz = -1.0f;
        };

        // Control values at sub-block boundaries (base rate).
        struct Control
        {
            float amount = 0.0f;
            float drive = 1.0f;       // linear gain into the curve
            float trim = 1.0f;        // static first guess of the wet trim
            float bias = 0.0f;
            float emphasisDb = 0.0f;
            float postHz = 18000.0f;
            float postBlend = 0.0f;
        };

        // A run of samples inside one control sub-block. Sub-blocks run on a phase kept across
        // calls, so the output does not depend on how the host splits its blocks.
        struct Segment
        {
            int start = 0, end = 0;   // samples [start, end) of this call
            int phase = 0;            // position of `start` inside its sub-block
            Control from, to;         // control at the sub-block's start and end
        };

        void processChunk (float* left, float* right, int numSamples);
        Control controlFor (float amount) const;
        void designEmphasis (Channel& c, float emphasisDb) const;  // no-op when unchanged
        bool stateIsFinite() const;

        double m_sampleRate = 48000.0;
        int m_maxBlock = 512;
        int m_latency = 0;

        std::unique_ptr<juce::dsp::Oversampling<float>> m_oversampling;
        std::array<Channel, 2> m_channels;
        std::array<std::vector<float>, 2> m_pre;   // pre-emphasised base-rate input
        std::vector<float> m_monoRight;            // scratch so the oversampler always runs 2 channels
        std::vector<Segment> m_segments;           // this call's sub-block runs

        Control m_ctlFrom, m_ctlTo;
        int m_ctlPos = 0;                          // samples into the current sub-block
        bool m_ctlValid = false;

        float m_target = 0.0f, m_amount = 0.0f;
        float m_subBlockCoeff = 0.0f;
        bool m_snap = true;

        float m_dcR = 0.999f;

        // Tame
        float m_tameEnv = 0.0f;
        float m_tameAttack = 0.0f, m_tameRelease = 0.0f;
        float m_tameAverageDb = -30.0f;
        float m_tameAverageCoeffBlock = 0.0f, m_tameAverageRiseBlock = 0.0f;
        float m_tameDb = 0.0f;
        int m_tameCounter = 0;                     // shelf redesign phase, kept across calls
        float m_tameDisplayDb = 0.0f;

        // Wet level match (2x rate, stereo-linked)
        float m_dryMs = 0.0f, m_shapedMs = 0.0f;
        float m_trimCoeff = 0.0f;
        float m_trim = -1.0f;

        float m_driveDisplay = 0.0f;
    };
}

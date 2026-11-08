#pragma once

#include "VoiceInfo.h"

#include <array>
#include <cstdint>
#include <vector>

namespace roar
{
    /**
     * LIQUID: the stage that makes the chain stereo. Mono in (left), stereo out (left + right).
     *
     *   in --> + SSB frequency shift (allpass Hilbert pair, shift wanders +-2..18 Hz, feedback) --> s
     *      s --> 4-tap modulated ensemble (8-22 ms) --> mid + side (side high-passed at 120 Hz)
     *      --> L / R --> 6-stage phaser per channel (LFOs offset by width) --> out L, R
     *
     *   - The frequency shifter moves every partial by the same few Hz, so harmonics stop lining
     *     up: the "fluid" shimmer. The shift follows a smoothed random LFO (0.3-2 Hz) through zero,
     *     so it reads as a wet wobble rather than a constant detune.
     *   - The ensemble taps use decorrelated LFOs; taps 0 and 2 feed the left side, 1 and 3 the
     *     right, which is what creates the width. Lows (< 120 Hz) are modulated too but kept in
     *     the mid only, so the low end stays mono-compatible.
     *   - The phaser adds the slow liquid sweep (0.15-0.6 Hz, feedback 0.4).
     *
     * Amount sets the wet of each effect (shifter and phaser at most 35 %, 28 % at Amount 30 %),
     * the shift range, feedback, LFO rates and the width (0 = mono). At Amount 0 the stage
     * outputs the input unchanged on both channels.
     *
     * Zero latency (the dry path is never delayed). No allocation after prepare(), NaN-safe.
     * The Hilbert pair is a fixed design (90 degrees +- 1 degree from about 20 Hz at 44.1/48 kHz; the
     * lower edge scales with the sample rate), which is plenty for a shimmer effect.
     */
    class LiquidStage
    {
    public:
        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        int getLatencySamples() const { return 0; }

        // amount01 0..1, smoothed internally (30 ms).
        void setParams (float amount01);

        // Reads left only (the chain is mono up to here) and writes both left and right.
        // right must be a valid buffer of numSamples; `stereo` is ignored.
        void process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo& voice);

        // Phaser LFO phase 0..1 for the visualiser.
        float getPhase() const { return static_cast<float> (m_phaserPhase); }

        // Current frequency shift in Hz (signed), for diagnostics.
        float getShiftHz() const { return m_shiftHz; }

    private:
        static constexpr int c_numHilbert = 4;
        static constexpr int c_numTaps = 4;
        static constexpr int c_numPhaserStages = 6;
        static constexpr int c_coefficientInterval = 16;

        // Second-order allpass in z^-2: y = c * (x + y[n-2]) - x[n-2]
        struct AllpassZ2
        {
            float c = 0.0f;
            float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

            float process (float x)
            {
                const float y = c * (x + y2) - x2;
                x2 = x1;
                x1 = x;
                y2 = y1;
                y1 = y;
                return y;
            }
        };

        struct Phaser
        {
            std::array<float, c_numPhaserStages> state {};
            float feedback = 0.0f;
            float coeff = 0.0f, coeffStep = 0.0f;
        };

        struct SmoothRandom
        {
            float from = 0.0f, to = 0.0f;
            float phase = 0.0f;
        };

        float nextRandom();  // uniform -1..1
        float advance (SmoothRandom& r, float rateHz);
        float readTap (float delaySamples) const;
        float phaserCoefficient (double phase) const;
        float runPhaser (Phaser& p, float x, float feedback);
        bool stateIsFinite() const;

        double m_sampleRate = 48000.0;
        float m_invSampleRate = 1.0f / 48000.0f;

        float m_target = 0.0f, m_amount = 0.0f, m_paramCoeff = 0.0f;
        bool m_snap = true;

        // Frequency shifter
        std::array<AllpassZ2, c_numHilbert> m_hilbertA {}, m_hilbertB {};
        float m_hilbertDelay = 0.0f;  // the A path is delayed one sample to line up with B
        double m_shiftPhase = 0.0;
        float m_shiftOut = 0.0f;      // last shifter output (feedback)
        float m_shiftHz = 0.0f;
        SmoothRandom m_shiftLfo;

        // Ensemble
        std::vector<float> m_line;
        int m_mask = 0;
        int m_write = 0;
        std::array<double, c_numTaps> m_tapPhase {};
        std::array<SmoothRandom, c_numTaps> m_tapWander {};
        float m_sideZ1 = 0.0f, m_sideZ2 = 0.0f;  // side high-pass (2nd order TDF2)
        float m_sideB0 = 1.0f, m_sideB1 = 0.0f, m_sideB2 = 0.0f, m_sideA1 = 0.0f, m_sideA2 = 0.0f;

        // Phaser
        Phaser m_phaserL, m_phaserR;
        double m_phaserPhase = 0.0;
        int m_coefficientCounter = 0;

        std::uint32_t m_random = 0x2545f491u;
    };
}

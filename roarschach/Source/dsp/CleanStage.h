#pragma once

#include "VoiceInfo.h"

#include <array>

namespace roar
{
    /**
     * CLEAN: the always-on "containment" at the head of the chain. No user controls.
     *
     *   in --> HPF 60 Hz (12 dB/oct) --> low shelf -4 dB @ 150 Hz --> downward expander
     *      --> 2.5:1 soft-knee compressor (auto make-up) --> split-band de-esser --> out
     *
     * Why each piece is there:
     *   - the later stages add a lot of low end (size resonators, saturation emphasis, sub), so
     *     the bass is pre-cut here and rumble below 60 Hz never reaches them;
     *   - the expander keeps room tone and breaths from being lifted by DETAIL and the
     *     saturation. Its threshold follows the noise floor (about -55 dBFS on a clean
     *     recording, kept within -62..-48 dBFS), with 4 dB hysteresis so it does not chatter;
     *   - the compressor evens the performance so every later stage sees a steady level;
     *   - the de-esser only acts when the 5.5-9 kHz band dominates the signal (an "s", not a
     *     bright vowel) and only removes that band (complementary split, so nothing else moves).
     *
     * Mono before LIQUID: processes left only when !stereo; when stereo both channels get the same
     * (linked) gains. Zero latency, no allocation, NaN-safe (non-finite input is treated as silence).
     */
    class CleanStage
    {
    public:
        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        int getLatencySamples() const { return 0; }

        // The fixed 12 dB/oct high-pass at the input (the processor phase-matches the dry to it).
        static constexpr double c_highPassHz = 60.0;

        // No user controls: present so the chain can treat every stage alike.
        void setParams() {}

        void process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo& voice);

        // Diagnostics / display, updated per call (dB, <= 0 for the gains, >= 0 for the de-esser).
        float getGateGainDb() const { return m_gateGainDb; }
        float getCompGainDb() const { return m_compGainDb; }
        float getDeEssDb() const { return m_deEssDb; }
        float getGateThresholdDb() const { return m_thresholdDb; }

    private:
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
        };

        // TPT state-variable band-pass for the de-esser split: x = lp + k * bp + hp exactly,
        // so removing part of k * bp leaves the rest of the spectrum untouched.
        struct Svf
        {
            float g = 0.0f, k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
            float ic1 = 0.0f, ic2 = 0.0f;

            float bandPass (float x)  // returns k * bp (unity gain at the centre)
            {
                const float v3 = x - ic2;
                const float v1 = a1 * ic1 + a2 * v3;
                const float v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0f * v1 - ic1;
                ic2 = 2.0f * v2 - ic2;
                return k * v1;
            }

            void clear() { ic1 = ic2 = 0.0f; }
        };

        struct Channel
        {
            Biquad hpf, shelf;
            Svf sibilance;
        };

        bool stateIsFinite() const;

        double m_sampleRate = 48000.0;
        std::array<Channel, 2> m_channels;

        // Expander
        float m_levelEnv = 0.0f;           // peak envelope (linear)
        float m_levelAttack = 0.0f, m_levelRelease = 0.0f;
        float m_floorDb = -65.0f;          // noise-floor estimate
        float m_floorFall = 0.0f;          // coefficient towards a lower level
        float m_floorRisePerSample = 0.0f; // dB per sample upward drift
        float m_thresholdDb = -55.0f;
        bool m_gateOpen = false;
        float m_gateGain = 1.0f;
        float m_gateOpenCoeff = 0.0f, m_gateCloseCoeff = 0.0f;
        float m_gateGainDb = 0.0f;

        // Compressor
        float m_compMs = 0.0f;             // mean square detector
        float m_compAttack = 0.0f, m_compRelease = 0.0f;
        float m_compGainDb = 0.0f;

        // De-esser
        float m_bandEnv = 0.0f, m_fullEnv = 0.0f;
        float m_sibAttack = 0.0f, m_sibRelease = 0.0f;
        float m_deEss = 0.0f;              // smoothed reduction, dB >= 0
        float m_deEssAttack = 0.0f, m_deEssRelease = 0.0f;
        float m_deEssDb = 0.0f;
    };
}

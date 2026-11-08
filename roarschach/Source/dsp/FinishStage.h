#pragma once

#include "VoiceInfo.h"

#include <array>
#include <cmath>
#include <vector>

namespace roar
{
    /**
     * FINISH: the always-on end of the wet chain. AUTO GAIN LIVES HERE.
     *
     *   chain input (mono, before CLEAN) --> measureInput() --> weighted loudness (reference)
     *
     *   wet in --> HPF 35 Hz (12 dB/oct) --> LPF 15 kHz (12 dB/oct) --> auto gain --> peak limiter
     *          --> soft-clip safety (0 dBFS) --> out (the processor then mixes this with the dry)
     *
     *   - Auto gain: a slow loudness match of the wet to the chain's input, so turning knobs
     *     changes the character, not the level. Both sides are measured with the same rough
     *     loudness weighting (high-pass 80 Hz + presence shelf) over ~400 ms; the correction is
     *     limited to +-12 dB, glides over ~400 ms and only learns while the input is above
     *     -50 dBFS, so it holds through pauses instead of pumping room tone up.
     *     The chain must call measureInput() with each chunk's input before process().
     *   - The limiter has no look-ahead (it adds no latency): 0.5 ms attack, 80 ms release, a
     *     soft knee from -4 dBFS towards a -1 dBFS ceiling. It hardly works on normal lines; the
     *     soft clip after it catches what the attack lets through, and nothing leaves above 0 dBFS.
     *
     * Stereo-linked when `stereo`, otherwise left only. Zero latency, no allocation, NaN-safe
     * (non-finite samples become silence).
     */
    class FinishStage
    {
    public:
        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        int getLatencySamples() const { return 0; }

        // The fixed 12 dB/oct cuts (the processor phase-matches the dry to them). The low-pass
        // is held below 0.45 x the sample rate.
        static constexpr double c_highPassHz = 35.0, c_lowPassHz = 15000.0;
        static double lowPassHzFor (double sampleRate) { return c_lowPassHz < 0.45 * sampleRate ? c_lowPassHz : 0.45 * sampleRate; }

        // autoGain: enable the loudness match (on by default). When switched off the gain glides
        // back to 0 dB.
        void setParams (bool autoGain);

        // The chain's (mono) input for this chunk: the loudness reference for auto gain.
        void measureInput (const float* input, int numSamples);

        void process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo& voice);

        float getAutoGainDb() const { return m_autoGainDb; }
        float getLimiterDb() const { return m_limiterDb; }   // current reduction, dB >= 0

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
            bool isFinite() const { return std::isfinite (z1) && std::isfinite (z2); }
        };

        // Rough loudness weighting (a K-weighting stand-in).
        struct Weighting
        {
            Biquad highPass, shelf;

            float process (float x) { return shelf.process (highPass.process (x)); }
            void clear() { highPass.clear(); shelf.clear(); }
        };

        struct Channel
        {
            Biquad hpf, lpf;
            Weighting weighting;
        };

        bool stateIsFinite() const;

        double m_sampleRate = 48000.0;
        std::array<Channel, 2> m_channels;

        // Auto gain
        Weighting m_inputWeighting;
        double m_inputMs = 0.0, m_wetMs = 0.0;
        double m_msCoeff = 0.0;
        float m_gainDb = 0.0f;
        float m_gainCoeff = 0.0f;
        bool m_autoGain = true;

        // First learn after a reset: the slow loudness meters and glide would leave the first
        // words several dB hot (the chain's fixed make-up), so for the first c_quickLearnSeconds
        // of signal the gain follows the plain average loudness ratio since the wet arrived,
        // with a fast glide. Both sums cover the same samples.
        bool m_quickLearn = true;       // until enough signal has been heard since the reset
        bool m_wetArrived = false;      // the wet has come through the chain's latency
        std::vector<float> m_inputPower; // this chunk's weighted input power, per sample
        int m_inputPowerCount = 0;
        double m_quickIn = 0.0, m_quickWet = 0.0;
        int m_quickSamples = 0, m_quickLength = 0;
        float m_quickGainCoeff = 0.0f;
        float m_autoGainDb = 0.0f;

        // Limiter
        float m_limGain = 1.0f;
        float m_limAttack = 0.0f, m_limRelease = 0.0f;
        float m_limiterDb = 0.0f;
    };
}

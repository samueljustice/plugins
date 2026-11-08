#pragma once

#include "VoiceInfo.h"

#include <cstdint>
#include <vector>

namespace roar
{
    /**
     * GROWL: pitch-synchronous period doubling + analog-style AM "chop" + light jitter.
     *
     *   in --> jitter delay (0.35 ms +- up to 0.25 ms, smoothed random) --> x period-doubling AM
     *      --> x tremolo chop --> level compensation --> out
     *
     *   - Period doubling: gain 1 + d1 * sin(phase / 2), where the phase runs at the tracked f0, so
     *     alternate glottal cycles get different gains. That splits each harmonic into f0/2
     *     sidebands: the rough, subharmonic "growl" of a strained throat. d1 up to 0.6, faded in
     *     and out with voicing (15 ms) because there is no period to double in a consonant.
     *   - Chop: a 25-45 Hz tremolo (rate rises with Amount) whose shape morphs from sine to
     *     squarish, with the depth re-drawn every cycle (shimmer, 0-15 %) so it never sounds like a
     *     steady LFO. Depth up to 0.5.
     *   - Jitter: the short delay is modulated by smoothed random noise, which wobbles the pitch
     *     by a few cents like an unsteady voice. The mean delay is fixed, so the stage has a
     *     constant latency of round(0.35 ms * sampleRate) samples whatever Amount is.
     *   - Intensity follows the input envelope: loud lines roar harder, quiet lines keep 35 % of
     *     the effect so they stay intelligible.
     *
     * Amount scales the depths linearly, so 30 % is a light flavour (the depths are 30 % of
     * their maxima) and 100 % is extreme. At Amount 0 the stage is an exact integer delay.
     *
     * Mono before LIQUID (left only when !stereo; stereo applies the same modulation to both).
     * No allocation after prepare(), NaN-safe.
     */
    class GrowlStage
    {
    public:
        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        int getLatencySamples() const { return m_baseDelay; }

        // amount01 0..1, smoothed internally (20 ms).
        void setParams (float amount01);

        // voice: f0 after the pitch ratio (the pitch of the signal this stage sees).
        void process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo& voice);

        // Current modulation (total gain - 1) low-passed to ~30 Hz, -1..1, for the visualiser.
        float getModulation() const { return m_modDisplay; }

    private:
        float nextRandom();  // uniform -1..1
        float readDelay (const std::vector<float>& line, float delaySamples) const;

        double m_sampleRate = 48000.0;
        float m_invSampleRate = 1.0f / 48000.0f;

        // Delay lines (one per channel), power-of-two length
        std::vector<float> m_lineL, m_lineR;
        int m_mask = 0;
        int m_write = 0;
        int m_baseDelay = 17;
        float m_maxJitterSamples = 12.0f;

        // Parameters
        float m_target = 0.0f;
        float m_amount = 0.0f;
        float m_paramCoeff = 0.0f;
        bool m_snap = true;

        // f0 tracking
        float m_f0 = 110.0f;
        float m_f0Coeff = 0.0f;
        float m_voicedGate = 0.0f;
        float m_voicedCoeff = 0.0f;
        double m_halfPhase = 0.0;

        // Tremolo
        double m_tremPhase = 0.0;
        float m_shimmerFrom = 1.0f, m_shimmerTo = 1.0f;

        // Jitter (smoothed random: new target every m_jitterPeriod samples, smoothstep between)
        float m_jitterFrom = 0.0f, m_jitterTo = 0.0f;
        int m_jitterCount = 0;
        int m_jitterPeriod = 600;

        // Envelope-dependent intensity
        float m_env = 0.0f;
        float m_envAttack = 0.0f, m_envRelease = 0.0f;

        float m_modLowpass = 0.0f, m_modCoeff = 0.0f;
        float m_modDisplay = 0.0f;

        std::uint32_t m_random = 0x9e3779b9u;
    };
}

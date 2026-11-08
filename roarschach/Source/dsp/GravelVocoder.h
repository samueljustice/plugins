#pragma once

#include "VoiceInfo.h"

#include <array>
#include <cstdint>

namespace roar
{
    /**
     * GRAVEL: a 32-band channel vocoder tuned for a deep, weighty, gravelly sound: fast
     * followers, steep 8-pole bands and a sub-octave pulse carrier. Movable: it can sit in any
     * chain slot.
     *
     *   modulator (mono sum) --> 32 x 8-pole Butterworth band-pass --> |x| --> fast attack/release
     *        followers --> ripple boost --> formant shift (band-mapping shift) --+
     *                                                                           v
     *   pulse stack (16 PolyBLEP pulses, f0 x pitchRatio / 2 or / 4) --+--> 32 x 8-pole band-pass
     *   white noise (unvoiced and breath bands) -------------------------+     --> carrier whitening
     *        --> x envelope --> soft per-band VCA saturation --> sum --> blend with dry
     *
     * Why it sounds like it does:
     *   - the followers are fast enough that ripple at the voice's glottal rate survives as
     *     audio-rate AM on the carrier, which prints the voice's own roughness onto the buzz;
     *   - the steep bands (24 dB/oct skirts, about 24 dB rejection at the neighbouring centre)
     *     keep the partials separated, so the result is weighty rather than smeared;
     *   - the carrier sits one or two octaves below the tracked voice, and consonants are rebuilt
     *     from noise in the upper bands so the words stay readable.
     *
     * Every band is a contiguous float lane: the per-sample inner loops run over the 32 bands
     * and auto-vectorise. All filters are IIR (TPT state-variable sections), so the latency is 0.
     *
     * Gravel 0 bypasses the module completely (no CPU); moving away from 0 fades the wet in over
     * 20 ms from cleared state, and back to 0 fades it out before the module goes idle.
     */
    class GravelVocoder
    {
    public:
        static constexpr int c_numBands = 32;
        static constexpr int c_numSections = 4;      // 8-pole band-pass = 4 two-pole sections
        static constexpr int c_numOscillators = 16;

        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        int getLatencySamples() const { return 0; }

        // gravel01 0..1 (0 = bypass), octaveIndex 0 = -1 oct, 1 = -2 oct, formantShiftBands
        // 0..-4 (negative = formants move down), pitchRatio = pitch shift applied upstream of
        // this slot (1 when the module sits before PITCH). Smoothed internally.
        void setParams (float gravel01, int octaveIndex, float formantShiftBands, float pitchRatio);

        // voice is the pitch of the CLEAN input. Mono when !stereo (right ignored).
        void process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo& voice);

        bool isActive() const { return m_active; }

        // Per-band modulator energy for the visualiser, 0..1 (a -60..0 dB scale), updated per call.
        const std::array<float, c_numBands>& getBandLevels() const { return m_bandLevels; }

        float getCarrierHz() const { return m_carrierHz; }

    private:
        using Lanes = std::array<float, c_numBands>;

        struct Bank
        {
            alignas (16) float ic1[c_numSections][c_numBands];
            alignas (16) float ic2[c_numSections][c_numBands];
            void clear();
        };

        void clearState();
        void updateChunkControls (const VoiceInfo& voice);
        void updateSubBlock();
        void updateCharacter();
        void runBank (Bank& bank, float* lanes) const;
        float renderBands (Bank& bank, Lanes& whitenMs, const float* carrierIn);
        bool stateIsFinite() const;
        float nextNoise();

        double m_sampleRate = 48000.0;
        bool m_prepared = false;
        bool m_active = false;
        bool m_wasStereo = false;

        // Parameters (targets) and their smoothed values.
        float m_targetGravel = 0.0f;
        float m_characterTarget = 0.0f;   // follows the knob, but holds its value while fading out
        float m_gravel = 0.0f;            // smoothed character amount (follower speed, ripple, saturation)
        float m_characterCoeff = 1.0f;
        int m_octaveIndex = 0;
        float m_targetShift = 0.0f;   // bands, >= 0 (formants down)
        float m_shift = 0.0f;
        float m_pitchRatio = 1.0f;

        float m_wet = 0.0f;
        float m_wetStep = 0.0f;

        // Filter coefficients (shared by the analysis and synthesis banks).
        alignas (16) float m_a1[c_numSections][c_numBands] {};
        alignas (16) float m_a2[c_numSections][c_numBands] {};
        alignas (16) float m_a3[c_numSections][c_numBands] {};
        Lanes m_centreHz {};
        Lanes m_bankGain {};       // makes each 8-pole band unity gain at its centre
        Lanes m_pulseGainBase {};  // pulse into band ~ unit RMS (divide by sqrt (carrier Hz))
        Lanes m_noiseGainBase {};  // noise into band ~ unit RMS
        Lanes m_breathNoise {};       // constant noise share of the top (breath) bands
        Lanes m_uvRamp {};         // how far unvoiced frames switch a band to noise

        Bank m_analysis {};
        Bank m_synthL {};
        Bank m_synthR {};

        alignas (16) Lanes m_env {};
        alignas (16) Lanes m_envLow {};
        alignas (16) Lanes m_whitenL {};
        alignas (16) Lanes m_whitenR {};
        alignas (16) Lanes m_noiseMix {};
        alignas (16) Lanes m_pulseGain {};
        alignas (16) Lanes m_noiseGain {};
        alignas (16) Lanes m_lanes {};
        alignas (16) Lanes m_lanesR {};
        alignas (16) std::array<float, c_numBands + 8> m_envPad {};
        Lanes m_bandLevels {};

        // Follower / character coefficients, per chunk.
        float m_attackCoeff = 0.0f;
        float m_releaseCoeff = 0.0f;
        float m_envLowCoeff = 0.0f;
        float m_rippleBoost = 0.0f;
        float m_satAmount = 1.0f;
        float m_whitenCoeff = 0.0f;
        float m_makeup = 1.0f;

        // Formant shift split into integer + fraction, per sub-block.
        int m_shiftInt = 0;
        float m_shiftFrac = 0.0f;

        // U/V detector.
        bool m_unvoiced = false;
        float m_uv = 0.0f;
        bool m_detectorVoiced = false;
        float m_detectorConfidence = 0.0f;

        // Carrier.
        float m_heldF0 = 110.0f;
        float m_carrierHz = 55.0f;
        float m_carrierTargetHz = 55.0f;
        float m_glideCoeff = 0.0f;
        float m_vibratoPhase = 0.0f;
        std::array<float, c_numOscillators> m_phase {};
        std::array<float, c_numOscillators> m_inc {};
        std::array<float, c_numOscillators> m_duty {};
        std::array<float, c_numOscillators> m_detuneCents {};
        std::array<float, c_numOscillators> m_driftPhase {};
        std::array<float, c_numOscillators> m_driftRate {};
        uint32_t m_noiseState = 0x9e3779b9u;
        uint32_t m_noiseStateR = 0x7f4a7c15u;

        int m_subCounter = 0;
    };
}

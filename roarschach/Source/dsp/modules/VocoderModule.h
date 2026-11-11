#pragma once

#include "../common/CarrierPitchGate.h"
#include "../common/Module.h"
#include "../../Params.h"

#include <array>
#include <cstdint>
#include <type_traits>

namespace roar
{
    /** VOCODER settings, filled by the processor once per chunk (spec §5.6, §7). */
    struct VocoderSettings
    {
        bool on = Params::defaultOf (Params::vocoderOn) > 0.5f;
        float gravel = Params::defaultOf (Params::gravel) / 100.0f;   // u, 0..1 (0 = dry)
        Params::VocoderOctave octave = static_cast<Params::VocoderOctave> (static_cast<int> (Params::defaultOf (Params::vocoderOctave)));
        // Formant offset in bands, 0..-4 (negative = formants move down). The chain fills it from
        // PITCH Size when PITCH precedes this module, else 0.
        float formantShiftBands = 0.0f;
    };

    /**
     * VOCODER: a 32-band channel vocoder tuned for a deep, weighty, gravelly sound: fast
     * followers, steep 8-pole bands and a sub-octave pulse carrier (spec §5.6, kept as v1).
     *
     *   modulator (mono, or the mid when stereo) --> 32 x 8-pole Butterworth band-pass --> |x|
     *        --> fast attack/release followers --> ripple boost --> formant shift (band mapping) --+
     *                                                                                            v
     *   pulse stack (16 PolyBLEP pulses at f0 / 2 or f0 / 4) --+--> 32 x 8-pole band-pass --> carrier
     *   white noise (unvoiced and breath bands) ----------------+     whitening --> x envelope
     *        --> soft per-band VCA saturation --> sum (stereo: bands panned alternately +-30 %)
     *        --> power-normalised blend with the dry (wet = 0.85 g; g = 0.15 + 0.85 Gravel, faded in
     *            over the first 1 % of Gravel: laws::gravelAmount)
     *
     * Why it sounds like it does:
     *   - the followers are fast enough that ripple at the voice's glottal rate survives as
     *     audio-rate AM on the carrier, which prints the voice's own roughness onto the buzz;
     *   - the steep bands (24 dB/oct skirts, about 24 dB rejection at the neighbouring centre)
     *     keep the partials separated, so the result is weighty rather than smeared;
     *   - the carrier sits one or two octaves below the voice, and consonants are rebuilt from
     *     noise in the upper bands so the words stay readable.
     *
     * The analysis bands sit on the synthesis centres but are never narrower than 1.45 ERB (and
     * never reach below 70 Hz), so the low bands answer within about 20 ms: the weight of a
     * syllable lands with its consonant. The synthesis bands stay constant-Q.
     *
     * The carrier follows the VoiceStream at this module's position (read every 16 samples), so a
     * VOCODER after PITCH follows the shifted voice. Only voiced, confident frames at a real level
     * move it (CarrierPitchGate); the first pitch of a new phrase is taken at once, later ones
     * glide over 25 ms. Latency 0 (all filters IIR TPT sections).
     *
     * Stereo (§3.3): one analysis of the mid and one synthesis bank; band b goes to the output
     * with gains (1 + p_b, 1 - p_b), p_b = +0.3 for even bands and -0.3 for odd ones, so the mid
     * of the stereo output is exactly the mono output. The mono part runs the same synthesis
     * with no pan.
     *
     * Gravel 0 leaves the signal dry and idles the engine; moving away from 0 fades the wet in
     * over 20 ms from cleared state. On/Off is the module's own 20 ms OnFade on top of that.
     * Every (re)start also restarts the free-running carrier motion (vibrato, drift, noise), and
     * the held pitch follows the stream while idle, so the output never depends on how the host
     * split the audio into blocks.
     *
     * No allocation after prepare(). copyStateFrom() is a plain copy of the engine (a trivially
     * copyable block of fixed arrays) plus the OnFade.
     */
    class VocoderModule final : public Module
    {
    public:
        /** The character amount g for a Gravel position u (laws::gravelAmount). */
        static float gravelFor (float u);

        static constexpr int c_numBands = 32;

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return 0; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const VocoderSettings& s);

        // Readouts (audio thread values; the editor reads them through MonsterFeatures).
        /** Per-band modulator energy, 0..1 (a -60..0 dB scale), scaled by the On fade. */
        float getBandLevel (int band) const;
        /** The carrier fundamental in Hz. */
        float getCarrierHz() const { return m_engine.getCarrierHz(); }
        /** 0..1: the wet is running (Gravel above 0 and the module On), scaled by the On fade. */
        float getActivity() const;

    private:
        /** The vocoder DSP (v1 engine, unchanged apart from the stream input and the stereo pan). */
        class Engine
        {
        public:
            static constexpr int c_numSections = 4;      // 8-pole band-pass = 4 two-pole sections
            static constexpr int c_numOscillators = 16;

            void prepare (double sampleRate);
            void clearState();
            /** Forgets the level history of the carrier-pitch gate (a transport restart). */
            void resetPitchGate() { m_pitchGate.reset(); m_gatePeak = 0.0f; }
            void setParams (float gravel01, int octaveIndex, float formantShiftBands);   // gravel01: g (after the law)

            /** Restart from cleared state with the wet already at its target (the OnFade fades it). */
            void restart (const VoiceFrame& voice);

            void process (float* left, float* right, int numSamples, bool stereo, const VoiceFrameView& voice);

            /** While idle: keep the held pitch current, so the carrier starts on the latest note. */
            void holdPitch (const float* left, const float* right, int numSamples, const VoiceFrameView& voice);

            bool isActive() const { return m_active; }
            float getWet() const { return m_wet; }
            float getCarrierHz() const { return m_carrierHz; }
            const std::array<float, c_numBands>& getBandLevels() const { return m_bandLevels; }

        private:
            using Lanes = std::array<float, c_numBands>;

            struct Bank
            {
                alignas (16) float ic1[c_numSections][c_numBands];
                alignas (16) float ic2[c_numSections][c_numBands];
                void clear();
            };

            void activate (const VoiceFrame& voice);
            bool followPitch (const VoiceFrame& voice);   // true at a phrase start
            void updateVoice (const VoiceFrame& voice);
            void updateSubBlock();
            void updateCharacter();
            void runBank (Bank& bank, float* lanes, bool analysis) const;
            void synthesise (const float* carrierIn);
            float renderMono (const float* carrierIn);
            void renderStereo (const float* carrierIn, float& outL, float& outR);
            bool stateIsFinite() const;
            float nextNoise();

            double m_sampleRate = 48000.0;
            bool m_prepared = false;
            bool m_active = false;

            // Parameters (targets) and their smoothed values.
            float m_targetGravel = 0.0f;
            float m_characterTarget = 0.0f;   // follows the knob, but holds its value while fading out
            float m_gravel = 0.0f;            // smoothed character amount (follower speed, ripple, saturation)
            float m_characterCoeff = 1.0f;
            int m_octaveIndex = 0;
            float m_targetShift = 0.0f;       // bands, >= 0 (formants down)
            float m_shift = 0.0f;

            float m_wet = 0.0f;
            float m_wetStep = 0.0f;

            // Filter coefficients: the synthesis bank (constant-Q), and the analysis bank on the
            // same centres with a minimum bandwidth at the low end.
            alignas (16) float m_a1[c_numSections][c_numBands] {};
            alignas (16) float m_a2[c_numSections][c_numBands] {};
            alignas (16) float m_a3[c_numSections][c_numBands] {};
            alignas (16) float m_an1[c_numSections][c_numBands] {};
            alignas (16) float m_an2[c_numSections][c_numBands] {};
            alignas (16) float m_an3[c_numSections][c_numBands] {};
            Lanes m_centreHz {};
            Lanes m_analysisGain {};   // makes each 8-pole analysis band unity gain at its centre, times its level trim
            Lanes m_pulseGainBase {};  // pulse into band ~ unit RMS (divide by sqrt (carrier Hz))
            Lanes m_noiseGainBase {};  // noise into band ~ unit RMS
            Lanes m_breathNoise {};    // constant noise share of the top (breath) bands
            Lanes m_uvRamp {};         // how far unvoiced frames switch a band to noise
            Lanes m_panLeft {};        // stereo band gains: 1 + p_b
            Lanes m_panRight {};       //                    1 - p_b

            Bank m_analysis {};
            Bank m_synth {};

            alignas (16) Lanes m_env {};
            alignas (16) Lanes m_envLow {};
            alignas (16) Lanes m_whiten {};
            alignas (16) Lanes m_noiseMix {};
            alignas (16) Lanes m_pulseGain {};
            alignas (16) Lanes m_noiseGain {};
            alignas (16) Lanes m_lanes {};
            alignas (16) Lanes m_lanesSynth {};
            alignas (16) std::array<float, c_numBands + 8> m_envPad {};
            Lanes m_bandLevels {};

            // Follower / character coefficients.
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
            CarrierPitchGate m_pitchGate;
            float m_gatePeak = 0.0f;   // peak |modulator| since the last control tick
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
            std::array<float, c_numOscillators> m_driftPhaseStart {};
            uint32_t m_noiseState = 0x9e3779b9u;

            int m_subCounter = 0;
        };

        static_assert (std::is_trivially_copyable_v<Engine>, "the clone is a plain copy");

        Engine m_engine;
        OnFade m_fade;
        VocoderSettings m_settings;
    };
}

#pragma once

#include "VoiceInfo.h"

namespace roar
{
    /**
     * SUB: a tight, tracked octave below the voice, mixed into the main channels.
     *
     *   mono sum --> TPT state-variable band-pass, Q 6, centred on the tracked f0 (20 ms glide)
     *            --> band-pass and low-pass outputs form a quadrature pair: amplitude + phase of
     *                the voice's fundamental
     *   phase-locked oscillator at f0 / 2 (its doubled phase follows the fundamental's phase)
     *            --> sine + a little 2nd / 3rd harmonic ("drive", heard as size, not distortion)
     *            x fundamental envelope x amount x voicing gate (15 ms fades) --> added to both channels
     *
     * Locking the oscillator's doubled phase to the voice's fundamental keeps the octave attached to
     * the voice (it moves with every inflection) and puts the 2nd harmonic of the sub exactly in phase
     * with the voice's own fundamental, so the drive reinforces the voice instead of cancelling it.
     *
     * The f0 comes from the chain's VoiceInfo (already multiplied by the pitch ratio). An octave jump
     * in that f0 while the sub is sounding must persist for 40 ms before the sub follows it (the classic
     * tracker error); every other change glides over 20 ms. While the voice is unvoiced the gate closes
     * and the oscillator keeps running at the held pitch, so the next note fades in without a click.
     *
     * Zero latency, no allocation, NaN-safe (non-finite input is treated as silence).
     */
    class SubStage
    {
    public:
        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        int getLatencySamples() const { return 0; }

        /** amount 0..1. Smoothed internally. */
        void setParams (float amount);

        void process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo& voice);

        /** Smoothed sub level for the display, 0..1. */
        float getLevel() const { return m_level; }

        /** Frequency the sub oscillator is following (Hz), and the voicing gate (0..1). */
        float getSubFrequency() const;
        float getGate() const { return m_gate; }

    private:
        void updateTarget (const VoiceInfo& voice, int numSamples);
        void updateFilter();

        double m_sampleRate = 48000.0;
        float m_fs = 48000.0f;

        // Band-pass (TPT SVF).
        float m_g = 0.0f, m_a1 = 1.0f, m_a2 = 0.0f, m_a3 = 0.0f;
        float m_ic1 = 0.0f, m_ic2 = 0.0f;
        int m_filterCounter = 0;

        // Pitch.
        float m_targetLogHz = 0.0f;   // log2 of the voice f0 being followed
        float m_logHz = 0.0f;         // glided
        float m_glideCoeff = 0.0f;
        bool m_hasPitch = false;
        float m_candidateLogHz = 0.0f;
        int m_candidateSamples = 0;
        int m_octaveConfirmSamples = 0;

        // Oscillator.
        double m_phase = 0.0;         // sub phase, radians, kept in [0, 2 pi)
        float m_lockCoeff = 0.0f;

        // Envelope, gate, level.
        float m_env = 0.0f;
        float m_envAttack = 0.0f, m_envRelease = 0.0f;
        float m_gate = 0.0f;
        float m_gateStep = 0.0f;
        bool m_gateOpen = false;
        float m_amountTarget = 0.0f;
        float m_amount = 0.0f;
        float m_amountCoeff = 0.0f;
        float m_level = 0.0f;
        float m_levelCoeff = 0.0f;
    };
}

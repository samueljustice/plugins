#pragma once

#include "VoiceInfo.h"

#include <array>
#include <vector>

namespace roar
{
    /**
     * Streaming YIN pitch detector for voices (50..1000 Hz), with a small tracker on top that
     * turns the raw per-window estimates into a VoiceInfo.
     *
     *   x --> DC/rumble high-pass (40 Hz) --> 4th-order low-pass (1.2 kHz) --> decimate to ~11 kHz
     *     --> YIN every ~5 ms on a 20 ms window --> median of 3 --> tracker --> VoiceInfo
     *
     * Feed it raw full-rate samples: it band-limits them itself (the low-pass also keeps strong
     * upper formants from pulling YIN onto a harmonic). Analysis cadence is independent of the host
     * block size, and YIN runs at the decimated rate, so the cost is small and bounded.
     *
     * The tracker:
     *   - goes voiced after two consecutive confident windows that agree within a semitone,
     *     and unvoiced after three windows (~15 ms) with no confident periodicity;
     *   - follows changes up to 4 semitones per window at once; bigger jumps must repeat for two
     *     windows, octave jumps (the classic YIN error) for four;
     *   - holds the last pitch through unvoiced passages and silence (f0Hz stays at the last value,
     *     voiced = false), as the VoiceInfo contract asks.
     *
     * The estimate describes the signal around getAnalysisDelaySamples() before the newest sample.
     *
     * No allocation after prepare(), no locks: safe for the audio thread. Non-finite input is
     * treated as silence.
     */
    class PitchDetector
    {
    public:
        void prepare (double sampleRate);
        void reset();

        /** Push one raw full-rate sample. Returns true when a new estimate is ready. */
        bool pushSample (float x);

        /** Push a block; returns true if at least one new estimate became ready. */
        bool process (const float* x, int numSamples);

        /** Current tracked voice. f0Hz is 0 only until the first pitch has been found. */
        const VoiceInfo& getVoiceInfo() const { return m_voice; }

        /** Tracked period in full-rate samples (from the held f0; 0 before the first pitch). */
        float getPeriodSamples() const { return m_voice.f0Hz > 0.0f ? static_cast<float> (m_sampleRate) / m_voice.f0Hz : 0.0f; }

        /** Raw output of the last window (median-filtered), 0 when that window was not periodic. */
        float getRawFrequency() const { return m_rawFrequency; }
        float getRawConfidence() const { return m_rawConfidence; }

        /** Full-rate samples between two estimates (~5 ms). */
        int getHopSize() const { return m_hop * m_decimation; }

        /** How far back (full-rate samples) the centre of the analysed window lies. */
        int getAnalysisDelaySamples() const { return m_analysisDelay; }

        static constexpr float c_minFrequency = 50.0f;
        static constexpr float c_maxFrequency = 1000.0f;

    private:
        struct Biquad
        {
            float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
            float z1 = 0.0f, z2 = 0.0f;

            void setLowpass (double fs, double fc, double q);
            void clear() { z1 = z2 = 0.0f; }

            float process (float x)
            {
                const float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
        };

        void analyse();
        static float dipDepth (const float* d, int tau);
        void track (float hz, float confidence);

        double m_sampleRate = 48000.0;
        double m_decimatedRate = 12000.0;
        int m_decimation = 4;
        int m_decimationCounter = 0;

        // Input conditioning (full rate).
        float m_hpCoeff = 0.0f, m_hpX1 = 0.0f, m_hpY1 = 0.0f;
        Biquad m_lp1, m_lp2;

        int m_tauMin = 2;
        int m_tauMax = 240;
        int m_window = 240;
        int m_hop = 60;
        int m_hopCounter = 0;
        int m_analysisDelay = 0;

        std::vector<float> m_ring;
        int m_ringWrite = 0;
        int m_ringFilled = 0;
        std::vector<float> m_frame;
        std::vector<float> m_diff;

        // Median-of-three on periodic estimates rejects isolated octave errors.
        std::array<float, 3> m_history { 0.0f, 0.0f, 0.0f };
        int m_historyCount = 0;
        float m_rawFrequency = 0.0f;
        float m_rawConfidence = 0.0f;

        // Tracker state (semitones relative to 1 Hz: 12 * log2 (f)).
        VoiceInfo m_voice;
        bool m_hasTrack = false;
        float m_trackSt = 0.0f;
        float m_onsetSt = 0.0f;
        int m_onsetHops = 0;
        int m_weakHops = 0;
        float m_candidateSt = 0.0f;
        int m_candidateHops = 0;
        float m_smoothedConfidence = 0.0f;

        static constexpr float c_yinThreshold = 0.2f;
        static constexpr float c_yinFallbackThreshold = 0.45f;
        static constexpr float c_strongConfidence = 0.75f;   // 1 - YIN dip
        static constexpr float c_keepConfidence = 0.55f;
        static constexpr float c_followSemitones = 4.0f;
        static constexpr int c_onHops = 2;
        static constexpr int c_offHops = 3;
        static constexpr int c_jumpHops = 2;
        static constexpr int c_octaveHops = 4;
    };
}

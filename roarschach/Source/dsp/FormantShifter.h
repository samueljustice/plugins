#pragma once

#include "PitchDetector.h"
#include "VoiceInfo.h"

#include <array>
#include <cstdint>
#include <vector>

namespace roar
{
    /**
     * PITCH + SIZE: formant-preserving TD-PSOLA pitch shifter with a grain-resampling formant scale,
     * followed by a small body resonator bank and a throat comb.
     *
     *   in --> ring buffer ----------------------------------------------+--> delayed dry (L samples)
     *     \--> PitchDetector (YIN) --> period, voicing                   |
     *     \--> band-limited copy --> pitch marks on the waveform peaks   |
     *                                                                    v
     *   synthesis marks every period / ratio --> nearest analysis mark --> windowed grain,
     *   read at rate r = 1 / (1 + 0.6 * size) --> overlap-add --> crossfade with dry --> body --> throat --> out
     *
     * Voiced grains are two analysis periods long (Hann), centred on a pitch mark that sits on the
     * waveform peak of its period (so every grain holds one whole glottal pulse and overlapping
     * grains add in phase), and placed at synthesis marks spaced by period / ratio. The formant
     * envelope lives inside each grain, so it stays where it was while the pitch moves.
     *
     * SIZE reads each grain slower than real time (r < 1): its content is stretched, which moves
     * the formants down by r (bigger creature) without touching the pitch, since grain spacing is
     * set by the synthesis marks alone. The body bank (three resonant peaks near 120 / 250 / 500 Hz,
     * moving down and rising to +5 dB with size) and a 1..4 ms throat comb at low feedback add the
     * chest and throat a larger body would have.
     *
     * A pitch mark only counts when the period it closes correlates with the one before it, so a
     * run of marks ends as soon as the voice turns to noise (the detector's verdict lags by ~20 ms
     * and would otherwise carry the old pitch into the start of a fricative as a buzz).
     *
     * Unvoiced input (no confident pitch, or no mark near a synthesis point) uses a fixed 10 ms
     * pseudo-period at 50 % overlap that keeps time (no grain is repeated, so the pitch change cannot
     * impose a period on noise). At Size 0 these grains reconstruct the input exactly; with Size the
     * stretched grains overlap incoherently, so the window turns power-complementary (sine) and the
     * read positions are randomised, so consonants come out as noise rather than a grain-rate buzz.
     * The two grain kinds hand over through ordinary overlap-add, so voicing changes do not click.
     *
     * Latency is constant: ceil(0.030 * sampleRate) samples, whatever the settings. At Pitch 0 and
     * Size 0 the stage crossfades (20 ms) to the plain delayed input, so it is then an exact delay.
     * Pitch marks need about 1.5 periods of look-ahead plus the grain's half-width, so below about
     * 60 Hz (or lower pitches with a large Size) grains may borrow a mark up to a period early: the
     * pitch stays right, timing gets a little looser.
     *
     * Mono: processes the left channel (if called stereo, the mono sum goes to both).
     * No allocation after prepare(), no locks, NaN-safe.
     */
    class FormantShifter
    {
    public:
        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        int getLatencySamples() const { return m_latency; }

        /** pitchSemitones -12..+12, size 0..1. Smoothed internally (20 ms). */
        void setParams (float pitchSemitones, float size);

        /** voice is not used: the stage tracks its own input (it needs marks on its own signal). */
        void process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo& voice);

        /** Voice detected on this stage's input. */
        const VoiceInfo& getInputVoice() const { return m_detector.getVoiceInfo(); }

        /** The same voice after the shift (f0 times the current pitch ratio), for the later stages. */
        VoiceInfo getOutputVoice() const;

        /** Current (smoothed) pitch ratio, 2^(st / 12). */
        float getPitchRatio() const { return static_cast<float> (m_ratio); }

        // Diagnostics for tests.
        std::uint64_t getVoicedGrainCount() const { return m_voicedGrains; }
        std::uint64_t getUnvoicedGrainCount() const { return m_unvoicedGrains; }

    private:
        struct Grain
        {
            double synCentre = 0.0;   // output-time centre
            double anaCentre = 0.0;   // input-time centre
            double halfWidth = 1.0;   // in output samples
            double invHalfWidth = 1.0;
            double rate = 1.0;        // input samples read per output sample
            float gain = 1.0f;
            float hann = 1.0f;        // window shape: 1 Hann, 0 sine
            bool voiced = true;
        };

        struct Mark
        {
            std::int64_t pos = 0;
            float period = 0.0f;
        };

        struct Biquad
        {
            float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
            float z1 = 0.0f, z2 = 0.0f;

            void setPeaking (double fs, double fc, double q, double gainDb);
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

        float processSample (float x);
        void findMarks();
        bool searchPeak (std::int64_t from, std::int64_t to, double predicted, double tolerance,
                         std::int64_t& best, float& bestValue);
        double markCorrelation (std::int64_t mark, std::int64_t previous) const;
        void pushMark (std::int64_t pos, float period);
        void scheduleGrains (std::int64_t outPos);
        void createGrain (std::int64_t outPos);
        double localPeriod (int markIndex) const;
        float renderGrains (std::int64_t outPos);
        float readInput (double pos) const;
        float window (double u, float hann) const;
        float voicedGain (double ratio, double rate) const;
        void updateBody();
        float nextRandom();

        double m_sampleRate = 48000.0;
        int m_latency = 0;
        int m_minPeriod = 48;
        int m_maxPeriod = 960;
        double m_unvoicedHalfWidth = 480.0;

        // Input history (absolute sample index & mask).
        std::vector<float> m_input;
        std::vector<float> m_band;   // band-limited copy for peak picking
        std::int64_t m_mask = 0;
        std::int64_t m_writePos = 0; // index of the next sample to write

        PitchDetector m_detector;
        float m_bandHpCoeff = 0.0f, m_bandHpX1 = 0.0f, m_bandHpY1 = 0.0f;
        Biquad m_bandLp;

        // Pitch marks.
        static constexpr int c_maxMarks = 256;
        std::array<Mark, c_maxMarks> m_marks {};
        int m_markHead = 0;          // index of the oldest mark
        int m_markCount = 0;
        std::int64_t m_lastMark = -1;
        bool m_haveLastMark = false;
        std::int64_t m_startRetry = 0;
        float m_positivePeaks = 0.0f;
        float m_negativePeaks = 0.0f;
        float m_polarity = 1.0f;

        // Grains.
        static constexpr int c_maxGrains = 32;
        std::array<Grain, c_maxGrains> m_grains {};
        int m_grainCount = 0;
        double m_nextSyn = 0.0;
        double m_prevSyn = 0.0;
        double m_prevHalfWidth = 0.0;
        bool m_prevVoiced = false;
        double m_unvoicedOffset = 0.0;

        // Parameters (targets and smoothed values).
        float m_pitchTarget = 0.0f;
        float m_sizeTarget = 0.0f;
        double m_logRatio = 0.0;
        double m_ratio = 1.0;
        double m_size = 0.0;
        double m_rate = 1.0;
        double m_smoothCoeff = 0.001;
        float m_wet = 0.0f;
        float m_wetStep = 0.001f;

        // Body resonators and throat comb.
        std::array<Biquad, 3> m_body {};
        int m_bodyCounter = 0;
        bool m_bodyActive = false;
        float m_bodyTrim = 1.0f;
        std::vector<float> m_throat;
        int m_throatWrite = 0;

        // Hann / sine window table over u in [0, 1]: cos (pi u / 2).
        static constexpr int c_windowSize = 1024;
        std::array<float, c_windowSize + 2> m_cosTable {};

        std::uint32_t m_random = 0x12345678u;
        std::uint64_t m_voicedGrains = 0;
        std::uint64_t m_unvoicedGrains = 0;
    };
}

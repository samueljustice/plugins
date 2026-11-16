#pragma once

#include "../PitchDetector.h"
#include "../common/Biquad.h"
#include "../common/Module.h"
#include "../common/SlidingExtreme.h"
#include "../common/VoiceStream.h"
#include "../KnobLaws.h"

#include <array>
#include <cstdint>
#include <vector>

namespace roar
{
    struct InSettings
    {
        float trimDb = 0.0f;        // Input, -24..+24 dB
        bool autoLevel = true;      // Auto Level
    };

    /**
     * IN (fixed): the head of the chain.
     *
     *   host L/R --> mono sum (L+R)/2 --> Input trim (20 ms) --> take gain --> out (left only)
     *                                                                     \-> YIN --> VoiceStream
     *
     * Auto Level is a per-take gain match: it learns one gain that puts the loud end of the take at
     * -16 dBFS, so the modules see the voice at the level they are calibrated for, and holds it.
     * OUTPUT takes the same gain back (getGainTrace(), sample-aligned), so the level at the output
     * follows the input: a whisper stays a whisper.
     *
     * Everything is measured on the weighted raw input (80 Hz high-pass, +4 dB shelf at 1.5 kHz),
     * before Input, in 32-sample frames on the sample clock:
     *   - F: the 20 ms level; sus: its minimum over 100 ms; a floor N that tracks room tone (frozen
     *     in digital silence, under -85 dBFS; held to 20 dB under the loudest sustained level of
     *     the pass and to -40 dBFS at most);
     *   - confident sound: pitched (detector voiced with confidence 0.6, 10 dB over the floor,
     *     above -50 dBFS, and modulated by 4 dB on the 100 ms minimum inside the sound, or above
     *     -25 dBFS), or unpitched (modulated by 6 dB on the 20 ms level inside the sound and 15 dB
     *     over the floor, above -50 dBFS), or anything whose 100 ms minimum is above -25 dBFS.
     *     Steady tones, hum, room tone and bleed never count;
     *   - the loud end L: the level the confident sound of one pass reaches for 0.5 s in total
     *     (a 1 dB histogram per pass). Until the first learn (150 ms pitched or 600 ms unpitched
     *     confident sound) the gain stays at 0 dB. Until a pass has held 4 s of confident sound L
     *     is the loudest eighth so far and may move either way; after that it only rises;
     *   - a sound more than 12 dB over the loud end for 150 ms gets a temporary fall (to within
     *     6 dB of it at 80 dB/s) that lets go at 6 dB/s.
     *
     *   gain = clamp (-16 - max (L, Lt), -18, +24) dB (at most +12 dB until 50 ms of pitched
     *   confident sound have been heard in one pass: wind or breathy noise before a line reads like
     *   an unpitched performance), glided: the first learn with tau 80 ms, then
     *   falling with tau 30 ms and rising with tau 300 ms. With Auto Level on, Input offsets the
     *   working level by half its value; switching Auto Level from Off to On learns the take again.
     *
     * The learnt level (learnt, L, the evidence E) is kept across passes and saved with the session;
     * the pass state (histogram, floor, windows) starts again at every beginPass() and reset().
     *
     * The detector runs on IN's output and fills the VoiceStream (one frame per 32 samples on the
     * sample clock); getVoiceView() hands it to the first module after process(). Its answers
     * come once the detector has seen the whole stretch it analyses (about 40 ms), and the stream
     * says that much skew, so a module behind PITCH reads the pitch at most about 10 ms late.
     *
     * process(): `stereo` says whether the host input is stereo (r is read, never written); the
     * mono result is written to l. Latency 0. No allocation after prepare(), NaN-safe.
     */
    class InModule : public Module
    {
    public:
        static constexpr double c_targetDb = -16.0;      // where the loud end of the take is put
        static constexpr double c_minGainDb = laws::inAutoMinGainDb, c_maxGainDb = laws::inAutoMaxGainDb;
        static constexpr int c_histBins = 72;            // 1 dB bins from -60 dBFS
        static constexpr double c_histLowDb = -60.0;

        void prepare (double fs, int maxBlock) override;
        /** Clears the filters, the detector and the pass state; the learnt level is kept and the
            gain is set to it at once. */
        void reset() override;
        int getLatencySamples() const override { return 0; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override {}
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const InSettings& s);

        /** A new pass (playback started, the playhead jumped): the pass state starts again. Nothing
            that is heard changes (filters, gain and the learnt level are kept). */
        void beginPass();
        /** Forget the learnt level: the next confident sound is learnt as a new take. */
        void clearLearning();
        /** Restore a learnt level (a session load): the gain is set to it at once. */
        void restoreLearning (bool learnt, double levelDb, double evidenceSeconds, bool pitchedHeard = true);

        /** The stream for the chunk just processed (valid until the next process()). */
        VoiceFrameView getVoiceView() const { return m_stream.view(); }

        /** Per sample of the chunk just processed: the total linear gain applied (Input times the
            take gain as blended), and 1 where the frame is a matched frame for OUTPUT, else 0. */
        const float* getGainTrace() const { return m_gainTrace.data(); }
        const float* getMatchActiveTrace() const { return m_activeTrace.data(); }
        /** IN's total gain now (linear), for refilling OUTPUT's delayed copy after a reset. */
        float getCurrentTotalGain() const;

        // Readouts
        float getAutoGainDb() const { return static_cast<float> (m_s.autoGainDb * m_s.autoBlend); }
        float getLearnedLevelDb() const { return m_s.learnt ? static_cast<float> (m_s.loudEndDb) : -150.0f; }
        bool isLearnt() const { return m_s.learnt; }
        /** Pitched sound has been heard in this take (until then the gain is at most +12 dB). */
        bool hasHeardPitched() const { return m_s.pitchedHeard; }
        float getEvidenceSeconds() const { return static_cast<float> (m_s.evidence); }
        float getFloorDb() const { return static_cast<float> (m_s.floorDb); }
        float getInputLevel() const { return m_s.inputLevel; } // 0..1 linear, smoothed peak (after Input)
        float getJaw() const;                                  // 0..1
        float getJawFast() const;                              // 0..1, 5 ms / 30 ms
        static float jawOf (float envelope);                   // envelope to 0..1 (-48..-6 dB)
        const VoiceInfo& getVoiceInfo() const { return m_detector.getVoiceInfo(); }

    private:
        /**
         * The range (max - min) over the last n frames of the values pushed as valid; a frame
         * pushed as not valid advances the window without entering it. Fixed capacity.
         */
        class WindowRange
        {
        public:
            void prepare (int maxFrames);
            void setWindow (int frames);
            void reset();
            double push (double x, bool valid);
            void copyStateFrom (const WindowRange& o);

        private:
            struct Deque
            {
                std::vector<double> values;
                std::vector<std::uint32_t> stamps;
                std::uint32_t head = 0, tail = 0;
            };
            void expire (Deque& d) const;
            void insert (Deque& d, double x, bool isMax) const;

            Deque m_max, m_min;
            std::uint32_t m_mask = 0;
            std::uint32_t m_clock = 0;
            int m_window = 1, m_maxWindow = 1;
        };

        void endFrame();
        double loudEndStat (double need) const;
        double targetDb() const;
        void clearPass();
        void setGainAtOnce();

        // Constants of prepare().
        double m_fs = 48000.0;
        double m_dt = 32.0 / 48000.0;                      // seconds per frame
        double m_trimCoeff = 0.0, m_blendStep = 0.0;
        double m_fastLevelCoeff = 0.0;                     // per sample
        double m_s100Coeff = 0.0, m_floorFallCoeff = 0.0;  // per frame
        double m_firstCoeff = 0.0, m_fallCoeff = 0.0, m_riseCoeff = 0.0;
        float m_jawAttack = 0.0f, m_jawRelease = 0.0f, m_jawFastRelease = 0.0f, m_meterRelease = 0.0f;

        Biquad m_weightHp, m_weightShelf;
        PitchDetector m_detector;
        VoiceStream m_stream;
        SlidingExtreme m_sus { SlidingExtreme::Kind::min };   // 100 ms minimum of F
        WindowRange m_spreadRaw, m_spreadU, m_spreadV;         // 400 ms
        std::vector<float> m_gainTrace, m_activeTrace;

        struct State
        {
            double trimTargetDb = 0.0, trimDb = 0.0;
            bool autoOn = true;
            bool snap = true;
            double autoBlend = 1.0;          // 0 = Off .. 1 = On, 20 ms ramp

            double frameSum = 0.0;           // weighted power of the current frame
            int framePhase = 0;
            double fastMs = 0.0;             // 20 ms weighted mean square

            // Kept across passes (and saved): the learnt level.
            bool learnt = false;
            double loudEndDb = -150.0;       // L
            double evidence = 0.0;           // E: the most confident seconds one pass has had (<= 4)
            bool pitchedHeard = false;       // 50 ms of pitched confident sound in one pass

            // Pass state.
            std::array<double, c_histBins> hist {};
            double confidentSeconds = 0.0;   // A
            double pitchedSeconds = 0.0, unpitchedSeconds = 0.0;   // a_v, a_u
            double idleSeconds = 0.0;
            double floorDb = -40.0;          // N
            double topDb = -150.0;           // loudest sustained level of the pass
            double runSeconds = 0.0;         // time above the floor
            double s100 = 0.0;               // 100 ms mean square (per frame)
            double overSeconds = 0.0;
            double tempLoudEndDb = -150.0;   // Lt

            // The gain.
            double autoGainDb = 0.0;         // G
            bool first = false;              // the first learn's glide
            double gainPrev = 1.0, gainNext = 1.0;   // linear, interpolated across a frame
            bool matchActive = false;

            float jawEnv = 0.0f;
            float jawFastEnv = 0.0f;
            float inputLevel = 0.0f;
        };

        State m_s;
    };
}

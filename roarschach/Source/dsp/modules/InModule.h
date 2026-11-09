#pragma once

#include "../PitchDetector.h"
#include "../common/Biquad.h"
#include "../common/Module.h"
#include "../common/VoiceStream.h"

namespace roar
{
    struct InSettings
    {
        float trimDb = 0.0f;        // Input, -24..+24 dB
        bool autoLevel = true;      // Auto Level
    };

    /**
     * IN (fixed, spec §5.1): the head of the chain.
     *
     *   host L/R --> mono sum (L+R)/2 --> Input trim (20 ms) --> auto level --> out (left only)
     *                                                                       \-> YIN --> VoiceStream
     *
     * Auto level puts voiced material at the reference level R = -20 dBFS RMS, whatever the source
     * level, so every level-dependent module downstream sees the same voice:
     *   - loudness: a K-style weighting (80 Hz high-pass, +4 dB shelf at 1.5 kHz, calibrated so a
     *     voice reads its plain RMS), one-pole mean square with tau 400 ms;
     *   - it learns only while the detector reports voiced, or on loud unvoiced material (more than
     *     12 dB above the tracked floor and within 6 dB of the voice level learnt so far); pauses,
     *     consonants, breaths, silence and room tone hold everything;
     *   - gain = clamp (R - L_voice, -12, +20) dB, glided with tau 1.5 s rising and 4 s falling.
     *     For the first second of voicing after a reset it learns fast (running mean and a
     *     0.25 s glide), so a take settles within about 2 s; after that it is slow, so a yell
     *     stays louder than a mutter within a line;
     *   - switching Auto Level off fades to unity (after Input) over 20 ms.
     *
     * The detector runs on IN's output and fills the VoiceStream (one frame per 32 samples on the
     * sample clock); getVoiceView() hands it to the first module after process(). Its estimate
     * lags the audio by the detector's analysis delay (about 10 ms).
     *
     * process(): `stereo` says whether the host input is stereo (r is read, never written); the
     * mono result is written to l. Latency 0. No allocation after prepare(), NaN-safe.
     */
    class InModule : public Module
    {
    public:
        static constexpr double c_referenceDb = -20.0;   // R
        static constexpr double c_minGainDb = -12.0, c_maxGainDb = 20.0;

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return 0; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override {}
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const InSettings& s);

        /** The stream for the chunk just processed (valid until the next process()). */
        VoiceFrameView getVoiceView() const { return m_stream.view(); }

        // Readouts
        float getAutoGainDb() const { return static_cast<float> (m_s.autoGainDb * m_s.autoBlend); }
        float getLearnedLevelDb() const;                       // L_voice, dBFS RMS (-150 before learning)
        float getFloorDb() const { return static_cast<float> (m_s.floorDb); }
        float getInputLevel() const { return m_s.inputLevel; } // 0..1 linear, smoothed peak (after Input)
        float getJaw() const;                                  // 0..1
        float getJawFast() const;                              // 0..1, 5 ms / 30 ms
        static float jawOf (float envelope);                   // envelope to 0..1 (-48..-6 dB)
        const VoiceInfo& getVoiceInfo() const { return m_detector.getVoiceInfo(); }

    private:
        void endFrame();

        // Constants of prepare().
        double m_fs = 48000.0;
        double m_trimCoeff = 0.0, m_blendStep = 0.0;
        double m_meterCoeff = 0.0;                         // per frame
        double m_riseCoeff = 0.0, m_fallCoeff = 0.0, m_quickCoeff = 0.0;
        double m_fastLevelCoeff = 0.0;                     // per sample
        double m_floorFallCoeff = 0.0, m_floorRiseDb = 0.0;
        int m_quickFrames = 1, m_meanFrames = 1;
        float m_jawAttack = 0.0f, m_jawRelease = 0.0f, m_jawFastRelease = 0.0f, m_meterRelease = 0.0f;

        Biquad m_weightHp, m_weightShelf;
        PitchDetector m_detector;
        VoiceStream m_stream;

        struct State
        {
            double trimTargetDb = 0.0, trimDb = 0.0;
            bool autoOn = true;
            bool snap = true;
            double autoBlend = 1.0;          // 0 = Off .. 1 = On, 20 ms ramp

            double frameSum = 0.0;           // weighted power of the current frame
            int framePhase = 0;
            double fastMs = 0.0;             // 20 ms weighted mean square (floor tracker input)
            double floorDb = -100.0;
            double meterMs = 0.0;            // learned voice loudness (mean square)
            int learnedFrames = 0;
            double autoGainDb = 0.0;         // glided gain
            double gainPrev = 1.0, gainNext = 1.0;   // linear, interpolated across a frame

            float jawEnv = 0.0f;
            float jawFastEnv = 0.0f;
            float inputLevel = 0.0f;
        };

        State m_s;
    };
}

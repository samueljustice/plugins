#pragma once

#include "CleanStage.h"
#include "DetailCompressor.h"
#include "FinishStage.h"
#include "FormantShifter.h"
#include "GravelVocoder.h"
#include "GrowlStage.h"
#include "LiquidStage.h"
#include "MonsterFeatures.h"
#include "PitchDetector.h"
#include "RoarStage.h"
#include "SubStage.h"
#include "VoiceInfo.h"

#include <vector>

namespace roar
{
    /**
     * The whole wet path: owns every stage, the two movable modules and their slots, the voice
     * plumbing between the stages and the visualiser feed.
     *
     *   IN (mono sum) --> CLEAN --> [0] --> PITCH + SIZE --> [1] --> GROWL --> [2] --> LIQUID (makes stereo)
     *      --> [3] --> ROAR --> [4] --> SUB --> [5] --> FINISH --> OUT (wet, stereo)
     *
     * DETAIL and VOCODER each sit in one slot [n]; when both choose the same one, DETAIL runs first.
     * Slots 0..2 are mono (left only), 3..5 stereo.
     *
     * Voices: the vocoder's carrier follows a pitch detector on the CLEAN output (times the shifter's
     * ratio once the vocoder sits after PITCH); GROWL and SUB follow the shifter's output voice.
     *
     * Slot moves are click-free: the module fades out where it is (its output blends back to its
     * input over 20 ms, so it acts as a bypass), moves at the end of that chunk, then fades in at
     * its new slot. Both movable modules have zero latency, so a move never changes the latency,
     * which is the sum of the PITCH, GROWL and ROAR latencies whatever the settings.
     *
     * Process at most maxBlock samples per call. No allocation after prepare(), no locks.
     */
    class CreatureChain
    {
    public:
        static constexpr int numSlots = 6;

        struct Settings
        {
            float pitchSt = -2.0f;   // semitones
            float size = 0.35f;      // the rest 0..1
            float growl = 0.30f;
            float liquid = 0.30f;
            float roar = 0.35f;
            float sub = 0.25f;
            float detailLow = 0.30f;
            float detailHigh = 0.20f;
            float gravel = 0.0f;
            int detailSlot = 0;      // 0..5
            int vocoderSlot = 0;     // 0..5
            int vocoderOctave = 0;   // 0 = -1 oct, 1 = -2 oct
        };

        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        int getLatencySamples() const { return m_latency; }

        /** l, r in: the input (r may be a copy of l, the chain sums them to mono).
            Out: the wet stereo signal. n <= maxBlockSize. Publishes the features once per call. */
        void process (float* l, float* r, int n, const Settings& settings, MonsterFeatures& features);

        /** Slot each movable module is running in right now (differs from the setting during a move). */
        int getDetailSlotInUse() const { return m_detail.slot; }
        int getVocoderSlotInUse() const { return m_vocoder.slot; }
        bool isMoving() const { return m_detail.slot != m_detail.target || m_detail.gain < 1.0f
                                    || m_vocoder.slot != m_vocoder.target || m_vocoder.gain < 1.0f; }

    private:
        // Slot state of one movable module.
        struct Movable
        {
            int slot = 0;        // where it runs
            int target = 0;      // where it should be
            float gain = 1.0f;   // 0 = bypassed (faded out), 1 = fully in
        };

        void runSlot (int slot, float* l, float* r, int n);
        void runMovable (Movable& m, bool isVocoder, float* l, float* r, int n, bool stereo);
        void finishMoves();
        void publish (const float* input, const float* l, const float* r, int n, MonsterFeatures& features);

        double m_sampleRate = 48000.0;
        int m_maxBlock = 512;
        int m_latency = 0;
        float m_fadeStep = 0.001f;
        bool m_snap = true;

        CleanStage m_clean;
        PitchDetector m_cleanDetector;
        FormantShifter m_shifter;
        GrowlStage m_growl;
        LiquidStage m_liquid;
        RoarStage m_roar;
        SubStage m_sub;
        FinishStage m_finish;
        DetailCompressor m_detailStage;
        GravelVocoder m_vocoderStage;

        Movable m_detail, m_vocoder;
        Settings m_settings;
        VoiceInfo m_cleanVoice;

        std::vector<float> m_input;      // mono input of this chunk
        std::vector<float> m_dryL, m_dryR; // a module's input while it fades

        // Visualiser state.
        float m_jawEnv = 0.0f, m_jawAttack = 0.0f, m_jawRelease = 0.0f;
        float m_inputLevel = 0.0f, m_outputLevel = 0.0f;
    };
}

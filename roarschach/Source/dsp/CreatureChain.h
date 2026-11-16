#pragma once

#include "../Params.h"
#include "DryPhaseMatch.h"
#include "HitDetector.h"
#include "MonsterFeatures.h"
#include "common/DelayLine.h"
#include "common/Module.h"
#include "common/VoiceStream.h"
#include "modules/ChamberModule.h"
#include "modules/CleanModule.h"
#include "modules/DeessModule.h"
#include "modules/DetailModule.h"
#include "modules/EnsembleModule.h"
#include "modules/GrowlModule.h"
#include "modules/InModule.h"
#include "modules/OutputModule.h"
#include "modules/OvertoneModule.h"
#include "modules/PitchModule.h"
#include "modules/RingModule.h"
#include "modules/RoarModule.h"
#include "modules/ShimmerModule.h"
#include "modules/SubModule.h"
#include "modules/SwirlModule.h"
#include "modules/TameModule.h"
#include "modules/VocoderModule.h"

#include <array>
#include <cstdint>
#include <vector>

namespace roar
{
    /** Every setting of the chain for one chunk, filled by the processor from the parameters. */
    struct ChainSettings
    {
        InSettings in;
        PitchSettings pitch;
        CleanSettings clean;
        DeessSettings deess;
        DetailSettings detail;
        VocoderSettings vocoder;
        RingSettings ring;
        ShimmerSettings shimmer;
        GrowlSettings growl;
        SwirlSettings swirl;
        RoarSettings roar;
        EnsembleSettings ensemble;
        TameSettings tame;
        SubSettings sub;
        OvertoneSettings overtone;
        ChamberSettings chamber;
        OutputSettings output;
        Params::Order positions = Params::defaultPositions();

        bool isOn (int module) const;
    };

    /** The learnt levels of IN and OUTPUT, as a session saves them. */
    struct LevelState
    {
        bool inLearnt = false;
        double inDb = -150.0;          // the take's loud end L
        double inEvidence = 0.0;       // E, seconds
        bool inPitched = true;         // pitched sound heard in the take (else IN's gain is capped)
        OutputModule::MatchState out;
        /** What a restore does with `out`: learnt restores it as it is; hint takes out.matchDb as a
            hint (a state saved before the match heard anything); keep leaves the match as it is (a
            state that carries no match: the preset's hint already in force stays). */
        enum class OutKind { learnt, hint, keep };
        OutKind outKind = OutKind::learnt;
        /** ROAR's learnt makeup. Without it (a state that does not carry one) a restore starts a new learn. */
        bool haveRoar = false;
        RoarModule::LevelLearning roar;
    };

    /** The resolved order and the stereo split slot (numModules: the whole chain is mono). */
    struct Topology
    {
        Params::Order order {};
        int split = Params::numModules;

        bool operator== (const Topology& o) const { return order == o.order && split == o.split; }
        bool operator!= (const Topology& o) const { return ! (*this == o); }
    };

    /**
     * The whole wet path:
     *
     *   host in --> IN (mono sum, trim, auto level, YIN --> VoiceStream)
     *           --> chain A (or A and B during a reorder fade): the 15 movable modules in the resolved
     *               order, mono up to the first stereo-creating module, stereo from there, then
     *               the post-chain hook (CHAMBER's Boom shelf)
     *           --> OUTPUT (filters, comp and level match, Ride, Push, Mix with the phase-matched dry,
     *               maximizer, clipper, safety) --> out
     *
     *   host in --> delay (sum of the module latencies) --> DryPhaseMatch --> OUTPUT's dry
     *   IN's gain and matched-flag traces --> delay (the same) --> OUTPUT (comp = 1 / IN's gain)
     *
     * Levels: IN learns the take's working level and OUTPUT the chain's gain at it (both kept
     * across resets and passes, saved with the session: restoreLevels, publish). Every chunk the
     * settings key (an FNV-1a hash of IN's settings and of each On module's, in order) goes to OUTPUT; a change
     * starts its probe.
     *
     * Order (spec §3.1): the modules sorted by (position, defaultIndex), read once per chunk. The
     * stereo split is the slot of the first module that creates stereo (ENSEMBLE when On, SHIMMER
     * On in Stereo, CHAMBER On with Width above 0); before it the modules run mono (left only), from it on stereo (R starts as L).
     *
     * Reorder (spec §3.5): a different order or split clones the running chain into the idle one
     * (copyStateFrom, no allocation), gives it the new topology (modules that enter the stereo part
     * copy their left state to the right), runs both and crossfades over 30 ms (power preserving: the gains follow the running correlation of the two renders, so renders that drift apart in phase do not cancel). A
     * module whose upstream changes (another module or another latency before it) would see a splice
     * at its input: its history is the previous order's signal, its next sample the new order's. So for
     * the first 10 ms the incoming chain feeds each such module a smoothstep blend from the input the
     * outgoing chain gives that module to its own; no splice ever enters a module's state (a
     * PSOLA grain may read its history a long way back). The crossfade starts once that blended
     * stretch has left the incoming chain (the latency from the first blended module to the end,
     * plus the blend: at most about 45 ms), so the two renders it mixes have the same timing. A
     * change that arrives during a fade waits; when the fade ends the latest request is compared
     * with what runs, so at most one reorder runs at a time (30 to 75 ms) and the last request
     * always wins. A new request starts a fade only after it has stood for 5 ms (and at least one
     * further chunk), so positions written one parameter at a time never start a fade into an
     * order in between. After prepare() or reset() the first chunk snaps to the requested topology and
     * settings.
     *
     * Voice (spec §3.6): IN's stream passes through one VoicePass per module (its latency, and
     * PITCH's rewrite), in the order the audio takes.
     *
     * Latency: constant, the sum of every module's latency plus OUTPUT's, for any order and setting.
     * Process at most maxBlock samples per call. No allocation after prepare(), no locks.
     */
    class CreatureChain
    {
    public:
        static constexpr double c_reorderSeconds = 0.030;
        static constexpr double c_inputBlendSeconds = 0.010;
        /** The reorder fade's correlation estimate: time constant, floor and the gain cap it allows. */
        /** A requested topology must stay the same this long (and over two chunks) before a fade starts. */
        static constexpr double c_reorderSettleSeconds = 0.005;
        static constexpr double c_fadeCorrelationSeconds = 0.004;
        static constexpr double c_minFadeCorrelation = -0.6;
        static constexpr double c_maxFadeGain = 2.0;
        /** After a match hint, OUTPUT learns nothing for the modules' latency plus this long. */
        static constexpr double c_hintSettleSeconds = 0.100;

        CreatureChain();

        void prepare (double sampleRate, int maxBlock);
        /** Audio thread safe (no allocation): clears every state; the next chunk snaps. */
        void reset();

        int getLatencySamples() const { return m_latency; }
        /** The part of the latency before OUTPUT (the delay of OUTPUT's dry). */
        int getModuleLatencySamples() const { return m_moduleLatency; }

        /**
         * inL / inR: the host input (inR == inL or inStereo false for a mono input). outL / outR:
         * the processed stereo output (may alias the input). n <= maxBlock.
         */
        void process (const float* inL, const float* inR, bool inStereo, float* outL, float* outR, int n,
                      const ChainSettings& settings, MonsterFeatures& features);

        /** The topology running now (the incoming one during a fade). */
        const Topology& getTopology() const { return m_sets[static_cast<size_t> (frontSet())].topology(); }
        bool isReordering() const { return m_fadePos >= 0; }
        int getReorderCount() const { return m_reorderCount; }
        std::uint64_t getClampCount() const { return m_output.getClampCount(); }
        const OutputModule& getOutput() const { return m_output; }

        const InModule& getIn() const { return m_in; }

        /** Test hooks: OUTPUT's filters, comp and level match, and Ride (all on in the plug-in). */
        void setOutputTestOptions (bool filters, bool autoGain, bool ride) { m_output.setTestOptions (filters, autoGain, ride); }
        void setOutputTestInvertWet (bool inverted) { m_output.setTestInvertWet (inverted); }
        /** Test hooks: forget what IN / OUTPUT learnt. */
        void clearLevelsForTests() { m_in.clearLearning(); m_output.clearLearning(); }

        /** A new pass (playback started or the playhead jumped): IN's pass state starts again.
            Nothing that is heard changes. Audio thread. */
        void beginPass();
        /** A session restore (audio thread, at the start of a block): the settings' Off to On
            transitions are consumed first, so they cannot clear what is restored. */
        void restoreLevels (const LevelState& levels, const ChainSettings& settings);
        /** A preset's match hint (audio thread), for the settings whose key is `hintKey` (those of
            the preset as loaded). Ignored when the match already holds data for the settings in
            force (re-selecting the current preset). If the settings in force are no longer the
            preset's, the hint is a starting point without weight. Returns false when ignored. */
        bool applyMatchHint (double matchDb, std::uint64_t hintKey, const ChainSettings& settings);
        /** The learnt levels now (audio thread, or while not processing). */
        LevelState getLevels() const;

        /** The settings key: 64-bit FNV-1a over IN's settings and, in the order the audio takes them,
         *  the On modules and their settings (Off modules and OUTPUT's own are left out). */
        static std::uint64_t settingsKey (const ChainSettings& settings);

        /** The requested topology for these settings (pure function, also used by tests). */
        static Topology topologyFor (const ChainSettings& settings);

    private:
        /** One complete chain of the 15 movable modules (the twin chains A and B). */
        class ModuleSet
        {
        public:
            ModuleSet();
            ModuleSet (const ModuleSet&) = delete;
            ModuleSet& operator= (const ModuleSet&) = delete;

            void prepare (double fs, int maxBlock);
            void reset();
            int getLatencySamples() const;

            void setParams (const ChainSettings& s);
            void setTopology (const Topology& t);
            /** The modules that are stereo in `next` but were mono in the current topology copy L to R. */
            void enterTopology (const Topology& next);
            const Topology& topology() const { return m_topology; }

            /**
             * l holds the mono input; l and r receive the stereo output. recordTaps keeps every
             * module's input of this chunk (the outgoing chain during a fade); blendSource, while
             * an input blend runs, is the outgoing chain whose taps the inputs glide from.
             */
            void process (float* l, float* r, int n, const VoiceFrameView& voice, bool recordTaps, const ModuleSet* blendSource);

            /**
             * After a clone: the modules whose upstream changes glide their input over `length`
             * samples. Returns how long the output crossfade must wait (0 when nothing is blended).
             */
            int beginInputBlend (const Topology& from, const Topology& to, int length);

            void copyStateFrom (const ModuleSet& other);
            void publishFeatures (const ChainSettings& s, MonsterFeatures& features) const;

            Module& module (int m) { return *m_modules[static_cast<size_t> (m)]; }
            const Module& module (int m) const { return *m_modules[static_cast<size_t> (m)]; }
            RoarModule& roar() { return m_roar; }
            const RoarModule& roar() const { return m_roar; }

        private:
            /** Settings that depend on the order (VOCODER's formant offset follows PITCH Size only after PITCH). */
            void applyOrderDependent();
            int slotOf (int module) const;

            PitchModule m_pitch;
            CleanModule m_clean;
            DeessModule m_deess;
            DetailModule m_detail;
            VocoderModule m_vocoder;
            RingModule m_ring;
            ShimmerModule m_shimmer;
            GrowlModule m_growl;
            SwirlModule m_swirl;
            RoarModule m_roar;
            EnsembleModule m_ensemble;
            TameModule m_tame;
            SubModule m_sub;
            OvertoneModule m_overtone;
            ChamberModule m_chamber;

            std::array<Module*, Params::numModules> m_modules {};
            std::array<VoicePass, Params::numModules> m_passes;
            Topology m_topology;
            VocoderSettings m_vocoderSettings;

            // Input blend after a clone, and the module inputs of the last chunk (taps).
            std::array<std::vector<float>, Params::numModules> m_tapL, m_tapR;
            std::array<bool, Params::numModules> m_tapStereo {};
            int m_blendFromSlot = Params::numModules;
            int m_blendPos = 0, m_blendLength = 1;
            float m_pitchSizeShift = 0.0f;   // -4 Size bands when PITCH is On
        };

        int frontSet() const { return m_fadePos >= 0 ? 1 - m_active : m_active; }
        void startReorder (const Topology& next);
        void publish (const ChainSettings& settings, const float* outL, const float* outR, int n, MonsterFeatures& features);
        void refillLevelDelays();
        int hintSettleSamples() const;
        static DryPhaseMatch::Settings dryMatchSettings (const ChainSettings& s);

        double m_fs = 48000.0;
        int m_maxBlock = 512;
        int m_latency = 0;
        int m_moduleLatency = 0;
        int m_fadeLength = 1440;

        InModule m_in;
        std::array<ModuleSet, 2> m_sets;
        OutputModule m_output;

        int m_active = 0;
        int m_fadePos = -1;        // >= 0 while crossfading from m_active to the other set
        int m_inputBlendLength = 480;
        int m_waitLeft = 0;        // samples the incoming chain runs unheard before the crossfade starts
        Topology m_pendingTopology;  // the latest requested topology and how long it has stood
        int m_pendingSamples = 0;
        int m_settleSamples = 240;
        double m_corrPole = 0.995;  // running power and cross power of A and B during the fade
        double m_corrAA = 0.0, m_corrBB = 0.0, m_corrAB = 0.0;
        int m_reorderCount = 0;
        bool m_snap = true;

        // Dry for OUTPUT's Mix: the host input delayed by the module latency, phase-matched.
        std::array<DelayLine, 2> m_dryDelay;
        DryPhaseMatch m_dryMatch;
        // IN's gain and matched-flag traces, delayed by the module latency (OUTPUT's comp).
        DelayLine m_gainDelay, m_activeDelay;
        std::vector<float> m_gainD, m_activeD;
        bool m_adoptKeyNext = false;   // a hint was applied: its chunk's key is the one in force

        // What feeds ROAR (its learnt makeup holds for that): a key over IN's settings and the On
        // modules before ROAR, as last seen; a new key makes ROAR learn again. IN's gain goes to
        // ROAR as a trace each chunk (ROAR follows it on its own sample clock).
        /** settingsKey over IN and the On modules before stopModule in the order (-1: all of them). */
        static std::uint64_t settingsKeyBefore (const ChainSettings& settings, int stopModule);
        void followRoarFeed (const ChainSettings& settings, const float* inGain, int n);
        std::uint64_t m_roarFeedKey = 0;
        bool m_roarFeedSeen = false;   // false: the next chunk's key is taken as the reference (after a restore)

        std::vector<float> m_dryL, m_dryR, m_monoIn, m_aL, m_aR, m_bL, m_bR, m_scratchR;

        float m_outputLevel = 0.0f;
        HitDetector m_hits;          // hits in the output, for the picture's ripples
        int m_hitsPublished = 0;     // the detector's count as last published
        float m_levelRelease = 0.999f;
        int m_generation = 0;
    };
}

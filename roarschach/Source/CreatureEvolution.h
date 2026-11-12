#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace roar
{
    /**
     * How the creature grows over a session. Pure: no JUCE, no threads, no clock of its own, so it
     * can be stepped in tests at any speed.
     *
     * The audio (never the knob positions alone) gives an intensity 0..1. Three integrators turn it
     * into a slow position on a stage spine: a phrase average (seconds), a session heat (tens of
     * seconds up, minutes down, with a dead band so small wobble never moves it) and an experience
     * (minutes to fill, tens of minutes to forget). The stage follows a slew-limited, critically
     * damped path, so nothing can change form in under about twenty seconds and every visible
     * quantity moves continuously. Four character accumulators (fluid, beast, eldritch, deep) bend
     * the anatomy by which modules have been doing the work.
     *
     * evaluate() blends six anchor forms along the spine and the character offsets on top, and
     * returns every shader parameter: the GL side never branches on a stage.
     *
     * Rarely, after a long stay at the top of the spine, a fresh shout lets the ink flow into the
     * maker's logo for a few seconds and back (the egg state machine below).
     */
    class CreatureEvolution
    {
    public:
        enum class Mode { live, frozen };

        /** One reading of the audio features (and the one knob it needs). */
        struct Inputs
        {
            float inputLevel = 0.0f;     // linear
            float outputLevel = 0.0f;    // linear
            float jaw = 0.0f;            // 0..1
            float roarHeat = 0.0f;       // 0..1
            float growlMod = 0.0f;       // -1..1
            float subLevel = 0.0f;       // 0..1
            float limiterGrDb = 0.0f;    // <= 0
            float clipDb = 0.0f;         // >= 0
            float gravelActive = 0.0f;   // 0 or 1
            float meanBand = 0.0f;       // mean of the vocoder bands, 0..1
            float ringAmount = 0.0f;     // 0..1
            float overtoneLevel = 0.0f;  // 0..1
            float ensembleWidth = 0.0f;  // 0..1
            float pitchSt = 0.0f;        // applied pitch shift, semitones
            float shimmerOn = 0.0f;      // 0 or 1
            float swirlOn = 0.0f;        // 0 or 1
            float liquidKnob = 0.0f;     // SHIMMER amount, 0..1 (0 while Off)
            bool stale = false;          // the audio thread stopped publishing
            bool eggVisible = true;      // a view is open that can draw the logo
        };

        /** Every parameter the shader is given, blended on the CPU. */
        enum Param : int
        {
            cranScale, snout, jawMass, brow,
            wingSpan, wingLift, eyespots, fingers,
            hornLength, hornCurl, hornTine, crown,
            browEyes, cheekEyes, centreEye, eyeSize,
            mawWidth, teethLength, thirdRow, cheekSpikes,
            solidity, bleed, dryBrush, gloss,
            dripRate, extraDrips, extraTendrils, tendrilReach,
            pastel, redBlack, glow, bioLum,
            antennae, scales, centralFigure, satellites,
            inkiness, fogFront, formFloor, formCeiling,
            coffeeRing, eyeSpacing, crownSpread, spare0,
            cloudR, cloudG, cloudB,
            cloudLitR, cloudLitG, cloudLitB,
            underglowR, underglowG, underglowB,
            redInkR, redInkG, redInkB,
            washAR, washAG, washAB,
            washBR, washBG, washBB,
            // derived
            formBase,          // formation the stage and heat allow (before the GL side's fast roar term)
            egg,               // 0..1 logo amount
            seedAX, seedAY,    // mirrored-noise offset of the current blot
            seedBX, seedBY,    // ... of the blot being glided to
            seedGlide,         // 0..1 between them
            cloudX, cloudY,    // offset of the (unmirrored) cloud field
            stageOut, energyOut,
            charFluid, charBeast, charEldritch, charDeep,   // how far each character leans, -1/3..1 (0 neutral)
            // the seed the ink creature's anatomy is grown from (the session's seed and the preset's
            // hash), as two 16-bit halves each (exact in a float): the current blot, the one glided to
            anatomyALo, anatomyAHi, anatomyBLo, anatomyBHi,
            numParams
        };

        static constexpr int numStageParams = washBB + 1;   // the rows of the stage table
        using Params = std::array<float, numParams>;

        /** The slowly-changing state that is saved with a session. */
        struct Persisted
        {
            float energy = 0.0f, experience = 0.0f, stage = 0.0f;
            std::array<float, 4> family {};
        };

        enum class EggState { idle, forming, holding, dissolving };

        explicit CreatureEvolution (std::uint32_t seed = 0x1234567u);

        /** dt in seconds of wall clock (clamped to 0..0.5 s, then multiplied by the test time scale). */
        void step (float dt, const Inputs& in);

        Params evaluate() const;

        /** The blend alone, at any stage and character (tests, harnesses, snapshot pins). */
        static Params evaluateAt (float stage, const std::array<float, 4>& characterWeights, float energy,
                                  std::uint32_t seed);

        // Mode, reset, seed
        void setMode (Mode m) { m_mode = m; }
        Mode getMode() const { return m_mode; }
        /** Eases stage, heat, experience and character back to 0 over 8 s and glides to a new blot. */
        void reset (std::uint32_t newSeed);
        bool isResetting() const { return m_resetSeconds >= 0.0f; }
        std::uint32_t getSeed() const { return m_hasQueuedSeed ? m_queuedSeed : m_seedTo; }
        float getSeedGlide() const { return m_seedGlide; }
        /** Instant (a session load): no glide. */
        void setSeed (std::uint32_t seed);
        /** The preset's hash (CreatureAnatomy::hashName; 0 none): each preset has its own blot on an
            instance's seed. glide: over the seed glide's 8 s (a preset change); false: at once (a load). */
        void setPresetHash (std::uint32_t hash, bool glide);
        std::uint32_t getPresetHash() const { return m_hasQueuedSeed ? m_queuedHash : m_hashTo; }
        /** The seed the blot's anatomy is grown from (the one being glided to, during a glide). */
        std::uint32_t getAnatomySeed() const;

        /** What a save stores: the current growth, or the dormant end state while a reset runs. */
        Persisted getPersisted() const;
        void setPersisted (const Persisted&);

        // Introspection
        float getIntensity() const { return m_intensity; }
        float getBurst() const { return m_burst; }
        float getEnergy() const { return m_energy; }
        float getExperience() const { return m_experience; }
        float getStage() const { return m_stage; }
        const std::array<float, 4>& getFamily() const { return m_family; }
        std::array<float, 4> getCharacterWeights() const;
        EggState getEggState() const { return m_eggState; }
        float getEggAmount() const;
        bool isEggArmed() const { return m_apexSeconds >= c_apexArmSeconds; }
        float getApexSeconds() const { return m_apexSeconds; }
        float getEggCooldown() const { return m_eggCooldown; }
        bool isAsleep() const { return m_quietSeconds >= c_sleepAfterSeconds; }

        /** Tests: simulated time runs this many times faster than the wall clock. */
        void setTimeScaleForTests (float scale) { m_timeScale = scale > 0.0f ? scale : 1.0f; }
        /** Tests and snapshots: evaluate() reports this stage with a neutral character (< 0 = live). */
        void setPinnedStageForTests (float stage) { m_pinnedStage = stage; }
        /** Tests: start the egg now (as if armed and shouted at). */
        void triggerEggForTests();

        /** The intensity formula on its own (0..1). */
        static float intensityOf (const Inputs& in);

        static constexpr float c_apexStage = 0.92f;
        static constexpr float c_apexArmSeconds = 90.0f;
        static constexpr float c_eggCooldownSeconds = 1200.0f;
        static constexpr float c_eggFormSeconds = 6.5f, c_eggHoldSeconds = 12.0f, c_eggDissolveSeconds = 5.0f;
        static constexpr float c_eggAbortSeconds = 3.0f;
        static constexpr float c_resetSeconds = 8.0f;
        static constexpr float c_seedGlideSeconds = 8.0f;
        static constexpr float c_sleepAfterSeconds = 1.5f;
        static constexpr float c_stageRiseRate = 1.0f / 90.0f, c_stageFallRate = 1.0f / 240.0f;

    private:
        Persisted currentState() const;
        void substep (float dt, const Inputs& in, float intensity, float loud);
        void stepEgg (float dt, const Inputs& in);

        Mode m_mode = Mode::live;
        float m_timeScale = 1.0f;
        float m_pinnedStage = -1.0f;

        float m_intensity = 0.0f;
        float m_burst = 0.0f, m_energy = 0.0f, m_experience = 0.0f;
        float m_stageTarget = 0.0f, m_slewed = 0.0f, m_stage = 0.0f, m_stageVel = 0.0f;
        std::array<float, 4> m_family {};
        float m_quietSeconds = 0.0f;

        // Reset path (seconds into it, < 0 = not resetting) and where it started
        float m_resetSeconds = -1.0f;
        Persisted m_resetFrom;

        // Seed glide
        std::uint32_t m_seedFrom, m_seedTo;
        float m_seedGlide = 1.0f;
        std::uint32_t m_queuedSeed = 0;
        bool m_hasQueuedSeed = false;
        std::uint32_t m_hashFrom = 0, m_hashTo = 0, m_queuedHash = 0;   // the preset's hash, glided with the seed

        // Egg
        EggState m_eggState = EggState::idle;
        float m_eggPhase = 0.0f;       // seconds into the current egg state
        float m_eggLevel = 0.0f;       // 0..1 raw envelope
        float m_apexSeconds = 0.0f;
        float m_eggCooldown = 0.0f;
        float m_jawLowSeconds = 0.0f;
        bool m_eggAborting = false;
    };

    /** The evaluated parameters published for the GL thread: relaxed atomics behind a sequence
        counter, so a read never mixes two publishes (the blot offsets are not smoothed). */
    struct CreatureShared
    {
        std::array<std::atomic<float>, CreatureEvolution::numParams> values;
        std::atomic<std::uint32_t> sequence { 0 };
        /** Written by the view (GL thread): a context is up with the logo's texture, so the egg can be seen. */
        mutable std::atomic<bool> eggVisible { false };

        CreatureShared();
        void publish (const CreatureEvolution::Params& p);
        CreatureEvolution::Params read() const;
    };

    /** The fast, per-frame part of the formation (GL thread): a roar pulls the face together now,
        sleep lets it disperse back into cloud. Gathers in ~3 s, disperses in ~4 s. */
    struct CreatureFormation
    {
        float fast = 0.0f;        // 0..1, follows the loudest of jaw, level and heat (0.25 s up, 2 s down)
        float formation = 0.0f;   // 0..1, the F the shader draws (1.2 s up, 4 s down)

        void step (float dt, float jaw, float loud, float heat, float sleep, float formBase, float egg);
        /** Front fog opacity: the stage's veil, pushed back by loud moments, closing in when quiet. */
        static float fogOpacity (float stageFog, float fast, float sleep);
        /** The ink creature's gathering: 0 asleep (only cloud) .. 1 awake, at any stage (the stage
            adds structure, never opacity). It follows waking in about a second and sleep in a few. */
        float ink = 0.0f;
        /** The ink creature's formation, 0 cloud .. 1 formed, eased at both ends. */
        float inkFormation() const;
    };

    /** The shader's uniform block, packed from the parameters (shared by the view and the harness). */
    struct CreatureUniforms
    {
        using Vec4 = std::array<float, 4>;
        using Vec3 = std::array<float, 3>;
        Vec4 head, wing, horn, eyes, maw, ink, flow, pal, misc, form, cloud, seed, seedB;
        Vec4 character;   // fluid, beast, eldritch, deep leans (the medium's look and flow); seedB.w is the stage
        Vec3 colCloud, colCloudLit, colUnderglow, colRedInk, colWashA, colWashB;
        float egg = 0.0f;

        /** formation, fogA, fast formation and the slow bloom phase come from the GL side. */
        static CreatureUniforms pack (const CreatureEvolution::Params& p, float formation, float fogA,
                                      float formationFast, float bloomPhase);
    };
}

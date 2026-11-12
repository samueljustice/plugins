#pragma once

#include <JuceHeader.h>

#include "CreatureAnatomy.h"
#include "CreatureFlow.h"
#include "CreatureMotion.h"
#include "MonsterShaders.h"

#include <array>
#include <cstdint>
#include <vector>

namespace roar
{
    // The per-frame uniforms every program may read: name, components (1..4), array length.
    // The creature's own uniforms first (as the single-pass shader always had them), then the head
    // and jaw motion, then the medium's forces, light and post (filled by MonsterRenderer::prepare),
    // then the ink creature's: its anatomy, flow, carry, cloud, breath and the pigment rule's rates
    // (prepare fills those too).
    #define ROAR_FRAME_UNIFORMS(X) \
        X (uRes, 2, 1) X (uPxScale, 1, 1) X (uRadius, 1, 1) X (uTime, 1, 1) X (uJaw, 1, 1) X (uGrowl, 1, 1) \
        X (uGrowlMod, 1, 1) X (uLiquid, 1, 1) X (uLiquidPhase, 1, 1) X (uRoar, 1, 1) X (uRoarDrive, 1, 1) \
        X (uSub, 1, 1) X (uSubLevel, 1, 1) X (uSubPhase, 1, 1) X (uPitch, 1, 1) X (uSize, 1, 1) X (uGravelOn, 1, 1) \
        X (uDetailLow, 1, 1) X (uDetailHigh, 1, 1) X (uBoost, 1, 1) X (uCut, 1, 1) X (uSleep, 1, 1) X (uBlink, 1, 1) \
        X (uLevel, 1, 1) X (uBands, 1, 32) X (uSwirlNotch, 1, 1) X (uSwirlAmt, 1, 1) X (uEnsWidth, 1, 1) \
        X (uDeess, 1, 1) X (uTame, 1, 1) X (uGateClosed, 1, 1) X (uOvLevel, 1, 1) X (uOvHarm, 1, 5) \
        X (uOvReplace, 1, 1) X (uRing, 1, 1) X (uRingFlicker, 1, 1) X (uRingBody, 1, 1) X (uStrain, 1, 1) \
        X (uStrainPulse, 1, 1) X (uClip, 1, 1) \
        X (uColScope, 3, 1) X (uColGrid, 3, 1) X (uColOuter, 3, 1) X (uColPitch, 3, 1) X (uColGrowl, 3, 1) \
        X (uColLiquid, 3, 1) X (uColRoar, 3, 1) X (uColSub, 3, 1) X (uColDetail, 3, 1) X (uColVocoder, 3, 1) \
        X (uColOutput, 3, 1) X (uColText, 3, 1) X (uColSwirl, 3, 1) X (uColOvertone, 3, 1) X (uColRing, 3, 1) \
        X (uColDeess, 3, 1) X (uColEnsemble, 3, 1) \
        X (uEvoHead, 4, 1) X (uEvoWing, 4, 1) X (uEvoHorn, 4, 1) X (uEvoEyes, 4, 1) X (uEvoMaw, 4, 1) \
        X (uEvoInk, 4, 1) X (uEvoFlow, 4, 1) X (uEvoPal, 4, 1) X (uEvoMisc, 4, 1) X (uForm, 4, 1) X (uCloud, 4, 1) \
        X (uSeed, 4, 1) X (uSeedB, 4, 1) X (uEvoChar, 4, 1) X (uDrift, 4, 1) X (uFlow, 4, 1) X (uFlow2, 4, 1) \
        X (uColCloud, 3, 1) X (uColCloudLit, 3, 1) X (uColUnderglow, 3, 1) X (uColRedInk, 3, 1) \
        X (uColWashA, 3, 1) X (uColWashB, 3, 1) X (uEgg, 1, 1) X (uLogoBox, 4, 1) \
        X (uHead, 4, 1) X (uLip, 1, 1) X (uJawVel, 1, 1) X (uRoarHold, 1, 1) X (uAspect, 1, 1) X (uVort, 4, 8) \
        X (uAmb, 4, 1) X (uSimK, 4, 1) X (uBuoy, 4, 1) X (uGather, 4, 1) X (uSplat, 4, 8) X (uSplatDir, 4, 8) \
        X (uFrill, 4, 1) X (uEns, 4, 1) X (uDiv, 4, 3) X (uGatherD, 4, 1) X (uInk, 4, 1) X (uFogD, 4, 1) \
        X (uEyeP, 4, 1) X (uLightDir, 4, 1) X (uEmit, 4, 4) X (uThroat, 4, 1) X (uEyeCol, 3, 1) X (uThroatCol, 3, 1) \
        X (uEmitCol, 3, 1) X (uBloomK, 4, 1) X (uComp, 4, 1) X (uGrain, 4, 1) X (uSpark, 4, 1) X (uFormK, 1, 1) \
        X (uAtmo, 4, 1) X (uAtmoTint, 3, 1) X (uMood, 4, 1) \
        X (uBlob, 4, 8) X (uBlobR, 4, 8) X (uLimbA, 4, 6) X (uLimbB, 4, 6) X (uMaw, 4, 1) X (uBay, 4, 2) \
        X (uFlowK, 4, 1) X (uFlowO, 4, 1) X (uFlowC, 4, 1) X (uCarry, 4, 1) X (uInkF, 4, 1) X (uInkO, 4, 1) \
        X (uCloudK, 4, 1) X (uCloudO, 4, 1) X (uBreath, 4, 1) X (uHold, 4, 1) X (uPig, 4, 1)

    enum class U : int
    {
       #define ROAR_X(n, c, k) n,
        ROAR_FRAME_UNIFORMS (ROAR_X)
       #undef ROAR_X
        numUniforms
    };

    struct UniformSpec { const char* name; int comps; int count; };

    inline constexpr UniformSpec c_uniformSpecs[] =
    {
       #define ROAR_X(n, c, k) { #n, c, k },
        ROAR_FRAME_UNIFORMS (ROAR_X)
       #undef ROAR_X
    };

    inline constexpr int c_numFrameUniforms = static_cast<int> (U::numUniforms);

    constexpr std::array<int, c_numFrameUniforms + 1> computeUniformOffsets()
    {
        std::array<int, c_numFrameUniforms + 1> o {};
        for (int i = 0; i < c_numFrameUniforms; ++i)
            o[(size_t) i + 1] = o[(size_t) i] + c_uniformSpecs[i].comps * c_uniformSpecs[i].count;
        return o;
    }

    inline constexpr auto c_uniformOffsets = computeUniformOffsets();

    /** One frame's inputs to the renderer: plain data, filled on the GL thread from atomics. */
    struct MonsterFrame
    {
        std::array<float, (size_t) c_uniformOffsets[(size_t) c_numFrameUniforms]> values {};

        float* at (U u) { return values.data() + c_uniformOffsets[(size_t) u]; }
        const float* at (U u) const { return values.data() + c_uniformOffsets[(size_t) u]; }
        float get (U u, int i = 0) const { return at (u)[i]; }
        void set (U u, float a) { at (u)[0] = a; }
        void set (U u, float a, float b) { auto* p = at (u); p[0] = a; p[1] = b; }
        void set (U u, float a, float b, float c) { auto* p = at (u); p[0] = a; p[1] = b; p[2] = c; }
        void set (U u, float a, float b, float c, float d) { auto* p = at (u); p[0] = a; p[1] = b; p[2] = c; p[3] = d; }
        void setAt (U u, int index, float a, float b, float c, float d) { auto* p = at (u) + 4 * index; p[0] = a; p[1] = b; p[2] = c; p[3] = d; }
        void setArray (U u, const float* data, int n);

        // CPU-side inputs (not uniforms)
        float frameDt = 1.0f / 30.0f;       // wall time since the last frame (clamped by the view)
        float interval = 1.0f / 30.0f;      // the measured frame interval (governor)
        float targetInterval = 1.0f / 30.0f;
        bool dtClamped = false;             // the frame time was clamped (a stall): not a measurement
        bool asleep = false;                // asleep and settled (the pressure solve may run every other frame)
        double clock = 0.0;                 // seconds since the view started (slow paths)
        float formationRate = 0.0f;         // dF/dt, 1/s
        float roarKnob = 0.35f;
        float swirlKnob = 0.0f;
        AudioForces forces;                 // the force envelopes (stepped by the view)
        // the ink creature: the blot's seed (and the one it is gliding to), and its own formation
        std::uint32_t seedFrom = 1, seedTo = 1;
        float seedGlide = 1.0f;             // 0..1 between them
        float inkFormation = 1.0f;          // 0 cloud .. 1 formed (InkFormation, from the view)
        float inputLoud = 0.0f;             // how loud the voice coming in is, 0..1
    };

    /**
     * The creature view's renderer: the creature as a field, a small fluid sim carrying the cloud,
     * the fog, heat and red ink, one light pass, bloom and a full-resolution composite; or, on GPUs
     * that can't (or are too slow), the single-pass stateless shader. No Component dependency, so the
     * offline harness drives it directly.
     *
     * GL thread only. create() in newOpenGLContextCreated (programs, fixed-size targets, probe),
     * render() per frame (window-sized targets on the first frame and after a size change), release()
     * in openGLContextClosing (every object deleted, every id zeroed). Nothing is allocated on the
     * heap after create() except the window-sized textures when the window changes size.
     */
    class MonsterRenderer
    {
    public:
        enum class Target { creatureField, emitter, velocity, pressure, dye, light, bloom, ink, numTargets };

        struct PassTimer
        {
            virtual ~PassTimer() = default;
            virtual void begin (const char* pass) = 0;
            virtual void end() = 0;
        };

        struct Stats
        {
            float gpuMs = -1.0f;        // the latest finished timer query (-1 = none yet)
            float cpuMs = 0.0f;         // render-thread time in render()
            int tier = 0, startTier = 0;
            int draws = 0;
            double compileMs = 0.0;
            bool probeOk = false, timers = false;
            int maxUnits = 0;
        };

        MonsterRenderer() = default;
        ~MonsterRenderer() = default;

        /** aspect = width / height of the view (taken once). logo may be empty. */
        bool create (bool coreProfile, float aspect, const std::vector<std::uint8_t>& logoRgba, int logoSize,
                     const std::vector<std::uint8_t>& noiseRgba, int noiseSize);
        void release();
        bool isCreated() const { return m_created; }
        bool hasLogo() const { return m_logoTex != 0 && ! m_ink; }

        /** Which creature is drawn: the ink creature (the build's default, ROARSCHACH_INK_CREATURE)
            or the one before it. Takes effect at the next create(). */
        void setInkCreature (bool ink) { m_ink = ink; }
        /** The ink creature's single pass at the view's own size (1) or at half size and upscaled
            (0); -1: by the GPU (full size on one that is known to be fast). */
        void setInkFullSize (int full) { m_inkFullForced = full; }
        /** Tests: the cloud in front of the ink at this share of its opacity (0: the ink bare). */
        void setInkFrontCloud (float share) { m_inkFrontCloud = share; }
        bool isInkFullSize() const { return m_inkFullForced >= 0 ? m_inkFullForced != 0 : m_inkFull; }
        bool isInkCreature() const { return m_ink; }
        /** The ink creature as the last prepare() posed it (tests: the shader's inputs, for the C++ twins). */
        const CreatureAnatomy::Pose& getInkPose() const { return m_inkPose; }
        const CreatureFlow::Params& getInkFlow() const { return m_inkFlow; }
        const CreatureAnatomy::Genome& getInkGenome() const { return m_genomes[(size_t) m_genomeNow]; }
        /** What drove the ink creature's last frame (tests and the screenshot tool's log). */
        struct InkState { float formation = 0.0f, opening = 0.0f, heat = 0.0f, level = 0.0f, light = 0.0f, carry = 0.0f, drift = 0.0f, kick = 0.0f; };
        const InkState& getInkState() const { return m_inkState; }
        /** Tests: the ink buffer as bytes even where half floats work. Takes effect at the next create(). */
        void setInkBytes (bool bytes) { m_inkBytesForced = bytes; }
        bool isInkBytes() const { return m_inkBytes; }
        /** The ink buffer's size on the tier being drawn (0 on the single pass). */
        int inkColumns() const { return m_tier >= 1 ? m_inkBuf[0].w : 0; }
        int inkRows() const { return m_tier >= 1 ? m_inkBuf[0].h : 0; }
        /** All the pigment in the buffer against the cloud's own at rest, as last read back (1 at rest). */
        float getInkBudget() const { return m_inkTotal; }
        float getInkBudgetGain() const { return m_inkBudget; }
        /** The cloud's own pigment, at rest. */
        static constexpr float c_inkAmbient = 0.15f;
        /** How much held ink one part of free pigment condenses into. */
        static constexpr float c_inkYield = 3.0f;

        /** Fills the medium's uniforms (forces, light, emitters) from the creature's ones and the forces. */
        void prepare (MonsterFrame&);

        /** Draws into hostFbo (bound by the caller) with the host viewport. */
        void render (const MonsterFrame&, unsigned int hostFbo, const int hostViewport[4]);

        int getTier() const { return m_tier; }
        /** Harness and tests: hold a tier (-1 = the governor decides). */
        void setForcedTier (int tier) { m_forcedTier = tier; }
        /** Harness and tests: a fixed 1/30 s step and a longer warm-up on the first frame. */
        void setFixedStep (bool fixed, int warmupSteps = 120) { m_fixedStep = fixed; m_warmupSteps = warmupSteps; }
        void requestReseed() { m_needsSeed = true; }
        void setPassTimer (PassTimer* t) { m_passTimer = t; }
        const Stats& getStats() const { return m_stats; }
        const QualityGovernor& getGovernor() const { return m_governor; }
        int simRows() const { return m_simH; }

        /** Tests (allocates): every GL object this renderer holds. */
        void collectObjectIds (std::vector<unsigned int>& textures, std::vector<unsigned int>& framebuffers,
                               std::vector<unsigned int>& programs, std::vector<unsigned int>& buffers) const;

        /** Harness only (allocates): the active rectangle of a target as RGBA floats, rows bottom up. */
        bool readTarget (Target, std::vector<float>& rgba, int& w, int& h);

    private:
        struct Tex
        {
            unsigned int tex = 0, fbo = 0;
            int w = 0, h = 0;
        };

        struct TierSpec
        {
            int simW, dyeMul, cfDiv, sunSteps, emitTaps, bloomLevels, jacobi;
            bool cubic, corrected, halfComposite;
        };

        enum class R : int
        {
            uOutRes, uSimTexel, uSimRes, uSimS, uDt, uFirst, uDiss, uSeedMode, uWarm, uDyeS, uCFS, uLightS,
            uBloomS, uSrcS, uSrcTexel, uDyeTexel, uDyeRes, uCFTexel, uDyeOn, uSteps, uUpGain, uDivK, uDither,
            uInkRes, uInkStep, uInkRule,
            numR
        };

        enum class S : int { dye, v, e, cf, light, noise, bloom, logo, wc, p, dv, src, ink, numS };

        struct Prog
        {
            unsigned int id = 0;
            std::array<int, (size_t) c_numFrameUniforms> frameLoc {};
            std::array<int, (size_t) R::numR> loc {};
            std::array<int, (size_t) S::numS> unit {};
            std::uint32_t uploaded = 0xffffffffu;
        };

        static const TierSpec& spec (int tier);
        bool buildProgram (shaders::Program, Prog&);
        void prepareInk (MonsterFrame&);
        const CreatureAnatomy::Genome& genomeFor (std::uint32_t seed, std::uint32_t keep);
        bool makeTex (Tex&, int w, int h, bool halfFloat, bool repeat = false, const void* rgba8 = nullptr);
        void freeTex (Tex&);
        bool probeFloat();
        void ensureWindowTargets (int w, int h, float dt);
        void use (shaders::Program, const MonsterFrame&);
        void bind (S, const Tex&);
        void bindRaw (S, unsigned int tex);
        void setR (R, float a);
        void setR (R, float a, float b);
        void setR (R, float a, float b, float c, float d);
        void setScale (R, const Tex&, int activeW, int activeH);
        void draw (const Tex* dst, int w, int h, const char* passName);
        /** One sim step of dt. emitDt: the time since the emitter last ran (its obstacle speed is the
            distance change over that), or <= 0 to keep the last emitter output (later substeps).
            divDt: the time the divergence sources stand for (all the time since the last pressure solve). */
        void simStep (const MonsterFrame&, float dt, bool solvePressure, float emitDt, float divDt);
        bool allocSim();
        void freeFluid();
        void rememberTier();
        void seed (const MonsterFrame&);
        void resampleTier (int from, int to);
        void drawSinglePass (const MonsterFrame&, unsigned int hostFbo, const int vp[4], float alpha);
        // the ink creature's buffer (tiers 1 to 3)
        static int inkWidthFor (int tier) { return tier >= 3 ? 512 : (tier == 2 ? 384 : 256); }
        bool allocInk (int tier);
        bool makeInkTex (int index, int w, int h);
        void freeInk();
        void inkUniforms (float dt, bool fresh);
        void inkStep (const MonsterFrame&, float dt, bool fresh);
        void inkResample (int toTier);
        void inkBudgetRead();
        void ensureInkWindowTargets (int w, int h, float dt);
        void renderInkTier (const MonsterFrame&, int tier, bool fading, unsigned int hostFbo, const int vp[4]);
        bool ensureQ0Target (int w, int h);
        void readTimers();
        void sizesFor (int tier, int& simW, int& simH, int& dyeW, int& dyeH) const;

        bool m_created = false, m_core = true, m_simOk = false;
       #if defined (ROARSCHACH_INK_CREATURE) && ! ROARSCHACH_INK_CREATURE
        bool m_ink = false;
       #else
        bool m_ink = true;
       #endif
        bool m_textureLod = true;    // the noise can be read at an explicit level of detail
        bool m_inkFull = false;      // this GPU draws the ink creature's single pass at full size
        int m_inkFullForced = -1;
        float m_inkFrontCloud = 1.0f;
        // the ink creature (GL thread; fixed storage: a new seed never allocates)
        std::array<CreatureAnatomy::Genome, 2> m_genomes {};
        std::array<bool, 2> m_genomeValid {};
        int m_genomeNow = 0;
        CreatureAnatomy::Pose m_inkPose;
        CreatureFlow::Params m_inkFlow;
        InkState m_inkState;
        double m_inkPhase[10] {};    // the eddies', the clouds' and the breath's slow scrolls (wrapped)
        double m_inkClock = 0.0;
        Tex m_inkBuf[2];             // the ink buffer's ping-pong pair (half the view, with its mips)
        unsigned int m_inkTopFbo[2] {};   // ... a small mip of each, to read all its pigment at once
        unsigned int m_inkPbo = 0;   // the budget's read comes back through this, frames later
        int m_inkCur = 0, m_inkTopLevel = 0, m_inkTopW = 1, m_inkTopH = 1;
        int m_inkReadAge = -1;       // frames since the budget's read was asked for (-1: none pending)
        bool m_inkBytes = false, m_inkBytesForced = false;
        bool m_inkSimToggle = false;
        float m_inkSimDt = 0.0f;     // time the buffer has not been stepped for (asleep: every other frame)
        float m_inkTotal = 1.0f, m_inkBudget = 1.0f;
        float m_inkFPrev = -1.0f, m_inkFRate = 0.0f, m_inkKick = 0.0f, m_inkFalling = 0.0f, m_inkFloor = 0.0f, m_inkSmoke = 0.33f, m_inkGather = 0.0f;
        float m_inkJet = 0.0f;       // the breath's jet, smoothed
        float m_inkLevel = 0.0f;     // the level the eddies follow, smoothed
        float m_aspect = 3.27f;
        int m_simH = 78;       // rows of the Q3 sim
        std::array<Prog, (size_t) shaders::Program::numPrograms> m_progs {};
        Prog* m_cur = nullptr;
        unsigned int m_vbo = 0, m_vao = 0, m_logoTex = 0, m_noiseTex = 0;
        Tex m_E[2], m_V[2], m_P[2], m_dye[2], m_Wc, m_Dv, m_light;
        Tex m_cf, m_bloom[4], m_half, m_fade;
        Tex m_q0;                    // the single pass at half resolution (grown, never shrunk; RGBA8)
        bool m_q0HalfFailed = false; // no half-resolution target here: the single pass draws at full size
        int m_e = 0, m_v = 0, m_p = 0, m_d = 0;
        int m_winW = 0, m_winH = 0, m_pendW = 0, m_pendH = 0;
        float m_pendSeconds = 0.0f;
        bool m_needsSeed = true, m_firstEmitter = true;
        int m_tier = 0, m_prevTier = -1, m_forcedTier = -1, m_capTier = 3;
        bool m_fixedStep = false;
        int m_warmupSteps = 30;
        float m_fadeQ0 = -1.0f;      // >= 0: crossfading into the single pass (seconds into it)
        bool m_tierCapture = false;  // the old tier drew once more and was kept, for the crossfade
        float m_tierFade = -1.0f;    // >= 0: crossfading from the kept frame (seconds into it)
        bool m_fadeUnsupported = false;  // the crossfade blit failed once: tier changes switch outright
        bool m_fluidFreed = false;   // settled on the single pass: the fluid's textures are given back
        float m_divDt = 0.0f;        // time since the last pressure solve (the divergence sources' share)
        char m_rendererKey[128] {};  // GL_RENDERER + GL_VENDOR (the settled tier is remembered per GPU)
        float m_freshGpuMs = -1.0f;  // a timer reading that arrived this frame (-1 = none)
        std::uint32_t m_frame = 0;
        bool m_pressureToggle = false;
        double m_amb[4] {};
        PassTimer* m_passTimer = nullptr;
        QualityGovernor m_governor;
        MistMotion m_mist;           // the top mist layer (kept across a context loss: no pop)
        Stats m_stats;
        // the one-query-per-frame GPU timer ring
        std::array<unsigned int, 4> m_queries {};
        std::array<bool, 4> m_queryPending {};
        int m_queryIndex = 0;
        bool m_timers = false;
        // host state seen at the start of a frame
        int m_hostVp[4] {};
        unsigned int m_hostFbo = 0;
    };
}

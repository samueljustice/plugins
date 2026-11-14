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
    // The per-frame uniforms every program may read: name, components (1..4), array length. The
    // view's readings first (the jaw, the modules, the colours), then the creature's own (form, seed,
    // stage, characters, mood, the mark), then what prepare() fills: the anatomy, the flow, the
    // carry, the cloud, the breath and the pigment rule's rates. A few are only read on the CPU
    // (prepare() turns them into the ones the shaders read).
    #define ROAR_FRAME_UNIFORMS(X) \
        X (uRes, 2, 1) X (uPxScale, 1, 1) X (uRadius, 1, 1) X (uJaw, 1, 1) X (uGrowl, 1, 1) \
        X (uGrowlMod, 1, 1) X (uLiquid, 1, 1) X (uRoar, 1, 1) X (uRoarDrive, 1, 1) \
        X (uSub, 1, 1) X (uSubLevel, 1, 1) X (uSubPhase, 1, 1) X (uPitch, 1, 1) X (uSize, 1, 1) X (uGravelOn, 1, 1) \
        X (uDetailLow, 1, 1) X (uDetailHigh, 1, 1) X (uSleep, 1, 1) \
        X (uLevel, 1, 1) X (uBands, 1, 32) X (uSwirlNotch, 1, 1) X (uSwirlAmt, 1, 1) X (uEnsWidth, 1, 1) \
        X (uDeess, 1, 1) X (uTame, 1, 1) X (uGateClosed, 1, 1) X (uOvLevel, 1, 1) X (uOvHarm, 1, 5) \
        X (uOvReplace, 1, 1) X (uRing, 1, 1) X (uRingFlicker, 1, 1) X (uRingBody, 1, 1) X (uStrain, 1, 1) \
        X (uColOuter, 3, 1) X (uColCloudLit, 3, 1) \
        X (uStage, 1, 1) X (uEvoChar, 4, 1) X (uEgg, 1, 1) X (uLogoBox, 4, 1) \
        X (uHead, 4, 1) X (uLip, 1, 1) X (uJawVel, 1, 1) X (uAspect, 1, 1) \
        X (uAtmoTint, 3, 1) X (uMood, 4, 1) \
        X (uBlob, 4, 8) X (uBlobR, 4, 8) X (uLimbA, 4, 6) X (uLimbB, 4, 6) X (uMaw, 4, 1) X (uBay, 4, 2) \
        X (uFlowK, 4, 1) X (uFlowO, 4, 1) X (uFlowC, 4, 1) X (uCarry, 4, 1) X (uInkF, 4, 1) X (uInkO, 4, 1) \
        X (uCloudK, 4, 1) X (uCloudO, 4, 1) X (uBreath, 4, 1) X (uHold, 4, 1) X (uPig, 4, 1) \
        X (uCloudT, 4, 1) X (uSubPulse, 4, 1) X (uSparkP, 4, 3) \
        X (uModA, 4, 1) X (uModB, 4, 1) X (uModC, 4, 1) X (uModD, 4, 1) X (uModE, 4, 1) X (uClean, 1, 1) X (uChamber, 4, 1) X (uSpine, 4, 5) \
        X (uRippleR, 4, 1) X (uRippleA, 4, 1) X (uRippleY, 4, 1) X (uRippleT, 4, 1)

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
        bool asleep = false;                // asleep and settled (the ink buffer may step every other frame)
        AudioForces forces;                 // the force envelopes (stepped by the view)
        // the ink creature: the blot's seed (and the one it is gliding to), and its own formation
        std::uint32_t seedFrom = 1, seedTo = 1;
        float seedGlide = 1.0f;             // 0..1 between them
        float inkFormation = 1.0f;          // 0 cloud .. 1 formed (InkFormation, from the view)
        float inputLoud = 0.0f;             // how loud the voice coming in is, 0..1
        float overtoneAmount = 1.0f;        // OVERTONE's Amount as set, 0..1 (its wisps are drawn only once it is turned up)
    };

    /**
     * The creature view's renderer: the ink creature. On tiers 1 to 3 a half-domain ink buffer
     * (held ink, free pigment, marbling, breath) is stepped by the pigment rule and drawn with the
     * lit cloud round it; on tier 0 (software and virtual GPUs, and any context without what the
     * buffer needs: measured cost never sends a GPU there) one stateless pass draws the same
     * creature from the same shared shader block. No Component
     * dependency, so the offline harness drives it directly.
     *
     * GL thread only. create() in newOpenGLContextCreated (programs, the buffer, probe), render() per
     * frame (window-sized targets on the first frame and after a size change), release() in
     * openGLContextClosing (every object deleted, every id zeroed). Nothing is allocated on the heap
     * after create() except the window-sized textures when the window changes size, and the buffer
     * when the tier changes.
     */
    class MonsterRenderer
    {
    public:
        enum class Target { ink };

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
        bool hasLogo() const { return m_logoTex != 0; }

        /** The ink creature's single pass at the view's own size (1) or at half size and upscaled
            (0); -1: by the GPU (full size on one that is known to be fast). */
        void setInkFullSize (int full) { m_inkFullForced = full; }
        /** Tests: the cloud in front of the ink at this share of its opacity (0: the ink bare). */
        void setInkFrontCloud (float share) { m_inkFrontCloud = share; }
        /** Tests: the rift's light held at this share of the blot's own (0 a dark moment, 1 a lit one; < 0: live). */
        void setInkMoment (float moment) { m_inkMoment = moment; }
        /** Tests: the rift's light itself (0 a dark tear .. 1 lit breath), whatever the blot's own is (< 0: the blot's). */
        void setInkLight (float light) { m_inkLight = light; }
        /** Tests: the modules' traces left out of the picture (what each module does to the voice
            still reaches it), so a trace can be told from the voice it changed. */
        void setInkTracesOff (bool off) { m_inkTracesOff = off; }
        bool isInkFullSize() const { return m_inkFullForced >= 0 ? m_inkFullForced != 0 : m_inkFull; }
        /** The ink creature as the last prepare() posed it (tests: the shader's inputs, for the C++ twins). */
        const CreatureAnatomy::Pose& getInkPose() const { return m_inkPose; }
        const CreatureFlow::Params& getInkFlow() const { return m_inkFlow; }
        const CreatureAnatomy::Genome& getInkGenome() const { return m_genomes[(size_t) m_genomeNow]; }
        /** What drove the ink creature's last frame (tests and the screenshot tool's log). */
        struct InkState { float formation = 0.0f, opening = 0.0f, heat = 0.0f, level = 0.0f, light = 0.0f, carry = 0.0f, drift = 0.0f, kick = 0.0f, push = 0.0f, beads = 0.0f; };
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
        /** Harness and tests: a fixed 1/30 s step whatever the wall clock says. */
        void setFixedStep (bool fixed) { m_fixedStep = fixed; }
        void requestReseed() { m_needsSeed = true; }
        void setPassTimer (PassTimer* t) { m_passTimer = t; }
        const Stats& getStats() const { return m_stats; }
        const QualityGovernor& getGovernor() const { return m_governor; }
        /** Tests: the governor itself (to script its readings). */
        QualityGovernor& governorForTests() { return m_governor; }
        /** Relief on the lowest tier (see QualityGovernor): none while a test holds a tier, unless a
            test holds the relief too (-1: the governor's). */
        void setForcedRelief (int relief) { m_forcedRelief = relief; }
        int reliefNow() const { return m_forcedRelief >= 0 ? m_forcedRelief : (m_forcedTier >= 0 ? 0 : m_governor.relief); }
        /** The lowest tier's draw is at 1 / this of the view's size. */
        int lowDivisor() const { return reliefNow() >= 1 ? 3 : 2; }
        /** Frames a second the view should ask for awake (30, or fewer in relief). */
        int awakeHz() const { const int r = reliefNow(); return r >= 3 ? 15 : r == 2 ? 20 : 30; }
        /** Tests: what this process remembers of each GPU's settled tier is forgotten. */
        static void forgetTiersForTests();

        /** Tests: the longest of OVERTONE's wisps as last posed, view heights (0: none). */
        float inkWispLengthForTests() const
        {
            float longest = 0.0f;
            for (const auto& s : m_inkSpine)
                longest = std::max (longest, s[1]);
            return longest;
        }
        /** Tests: the slow scrolls moved on as this many seconds would move them (at rest). */
        void advanceInkScrollsForTests (double seconds);
        /** Tests: seconds (at rest) until this slow scroll next wraps; inkScrollCount of them. */
        double inkScrollWrapInForTests (int index) const;
        static constexpr int inkScrollCount = 13;

        /** Tests (allocates): every GL object this renderer holds, each kind in its own list. */
        struct ObjectIds
        {
            std::vector<unsigned int> textures, framebuffers, programs, buffers, queries, vertexArrays;
            bool timers = false;   // it has timer queries (then queries is not empty)
        };
        void collectObjectIds (ObjectIds&) const;

        /** Harness only (allocates): the active rectangle of a target as RGBA floats, rows bottom up. */
        bool readTarget (Target, std::vector<float>& rgba, int& w, int& h);

    private:
        struct Tex
        {
            unsigned int tex = 0, fbo = 0;
            int w = 0, h = 0;
        };

        enum class R : int { uSrcS, uDither, uInkRes, uInkStep, uInkRule, uInkTier, numR };

        enum class S : int { noise, logo, src, ink, numS };

        struct Prog
        {
            unsigned int id = 0;
            std::array<int, (size_t) c_numFrameUniforms> frameLoc {};
            std::array<int, (size_t) R::numR> loc {};
            std::array<int, (size_t) S::numS> unit {};
            std::uint32_t uploaded = 0xffffffffu;
        };

        static double inkScrollRate (int index, double eddyPace, double jet);
        static double inkScrollPeriod (int index);
        void stepInkScrolls (double dt, double eddyPace, double jet);
        bool buildProgram (shaders::Program, Prog&);
        void prepareInk (MonsterFrame&);
        const CreatureAnatomy::Genome& genomeFor (std::uint32_t seed, std::uint32_t keep);
        bool makeTex (Tex&, int w, int h, bool halfFloat, bool repeat = false, const void* rgba8 = nullptr);
        void freeTex (Tex&);
        bool probeFloat();
        void use (shaders::Program, const MonsterFrame&);
        void bind (S, const Tex&);
        void bindRaw (S, unsigned int tex);
        void setR (R, float a);
        void setR (R, float a, float b);
        void setR (R, float a, float b, float c, float d);
        void setScale (R, const Tex&, int activeW, int activeH);
        void draw (const Tex* dst, int w, int h, const char* passName);
        void rememberTier();
        void drawSinglePass (const MonsterFrame&, unsigned int hostFbo, const int vp[4]);
        // the ink creature's buffer (tiers 1 to 3)
        static int inkWidthFor (int tier) { return tier >= 3 ? 512 : (tier == 2 ? 384 : 256); }
        bool allocInk (int tier);
        bool makeInkTex (int index, int w, int h);
        void freeInk();
        void inkUniforms (float dt, bool fresh);
        void inkStep (const MonsterFrame&, float dt, bool fresh);
        void inkResample (int toTier);
        void inkBudgetRead();
        void ensureInkWindowTargets (int w, int h, float dt, int div);
        void renderInkTier (const MonsterFrame&, int tier, unsigned int hostFbo, const int vp[4]);
        bool ensureQ0Target (int w, int h, int div);
        void readTimers();

        bool m_created = false, m_core = true, m_simOk = false;   // (m_simOk: the buffer's tiers can run here)
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
        static constexpr int c_inkScrolls = 13;
        double m_inkPhase[c_inkScrolls] {};   // the slow scrolls (see stepInkScrolls): each wrapped at its own period
        Tex m_inkBuf[2];             // the ink buffer's ping-pong pair (half the view, with its mips)
        unsigned int m_inkPbo = 0;   // the budget's read comes back through this, frames later
        int m_inkCur = 0, m_inkTopLevel = 0, m_inkTopW = 1, m_inkTopH = 1;
        int m_inkReadAge = -1;       // frames since the budget's read was asked for (-1: none pending)
        bool m_inkBytes = false, m_inkBytesForced = false;
        bool m_inkSimToggle = false;
        float m_inkSimDt = 0.0f;     // time the buffer has not been stepped for (asleep: every other frame)
        float m_inkTotal = 1.0f, m_inkBudget = 1.0f;
        float m_inkFPrev = -1.0f, m_inkFRate = 0.0f, m_inkKick = 0.0f, m_inkFalling = 0.0f, m_inkFloor = 0.0f, m_inkSmoke = 1.0f, m_inkGather = 0.0f;
        float m_inkLetGo = 0.0f;     // how lately the body was let go: up as the hold gives, down as it gathers again
        float m_inkQuiet = 0.0f, m_inkHush = 0.0f;   // how long the voice has been still, and the hush that lets the rim go
        float m_inkUnSingle = 0.0f;  // the single pass's own unformedness: it lets go no faster than the buffer's smoke clears
        float m_inkSwellHold = 0.0f; // seconds the swell is held for yet
        float m_inkFitK = 1.0f, m_inkFitY = 0.0f;   // the pose kept inside the view: its scale and shift, eased
        float m_inkNotch = 0.0f;     // SWIRL's notch as the bound pair's place follows it (eased)
        float m_inkBands[32] {};     // VOCODER's band levels as the strata follow them (eased)
        float m_inkSpine[5][2] {};   // OVERTONE's spines as they follow the body's top: root height, length (eased)
        float m_inkGate = 0.0f, m_inkTame = 0.0f;   // CLEAN's gate and TAME as the limbs follow them (eased)
        float m_inkTidy = 0.0f;      // CLEAN's tidiness, as the edge follows it (eased)
        float m_inkTraceK = 1.0f;    // the trace budget's scale (eased)
        double m_inkModPhase[4] {};  // the modules' own clocks: RING's ripples, GROWL's ruffle, SHIMMER's beads, CHAMBER's bands
        float m_inkHeat = 0.0f;      // the roar's heat as the eddies follow it (eased)
        float m_inkSwirl = 0.0f;     // SWIRL's amount as the bound pair follows it (eased)
        float m_inkJet = 0.0f;       // the breath's jet, smoothed
        float m_inkLevel = 0.0f;     // the level the eddies follow, smoothed
        float m_inkSwell = 0.0f;     // the roar's heat as the body follows it: up at once, down over seconds
        double m_inkCloudPhase = 0.0;   // the far cloud's carry, a cycle of nine seconds
        int m_inkSparkSeed = -1;     // the spark the beads were last thrown for
        float m_inkSparkAge = 9.0f;
        std::array<std::array<float, 4>, 3> m_inkBead {};   // each bead: where it left the rim xy, its way xy
        float m_inkLight = -1.0f;    // tests: the rift's light, whatever the blot's own (< 0: the blot's)
        float m_inkMoment = -1.0f;   // tests: the rift's light held at this moment's share (< 0: live)
        bool m_inkTracesOff = false; // tests: no module traces
        float m_aspect = 3.27f;
        std::array<Prog, (size_t) shaders::Program::numPrograms> m_progs {};
        Prog* m_cur = nullptr;
        unsigned int m_vbo = 0, m_vao = 0, m_logoTex = 0, m_noiseTex = 0;
        Tex m_half, m_fade;          // the half-size composite (tier 1; a third in relief) and the tier crossfade's kept frame
        Tex m_q0;                    // the single pass at half resolution, or a third (grown, never shrunk; RGBA8)
        bool m_q0HalfFailed = false; // no half-resolution target here: the single pass draws at full size
        int m_winW = 0, m_winH = 0, m_pendW = 0, m_pendH = 0, m_halfDiv = 2;
        int m_forcedRelief = -1;
        float m_pendSeconds = 0.0f;
        bool m_needsSeed = true;
        int m_tier = 0, m_prevTier = -1, m_forcedTier = -1, m_capTier = 3;
        bool m_fixedStep = false;
        bool m_tierCapture = false;  // the tier being left drew once more and was kept, for the crossfade
        float m_tierFade = -1.0f;    // >= 0: crossfading from the kept frame (seconds into it)
        bool m_fadeUnsupported = false;  // the crossfade blit failed once: tier changes switch outright
        char m_rendererKey[128] {};  // GL_RENDERER + GL_VENDOR (the settled tier is remembered per GPU)
        float m_freshGpuMs = -1.0f;  // a timer reading that arrived this frame (-1 = none)
        std::uint32_t m_frame = 0;
        PassTimer* m_passTimer = nullptr;
        QualityGovernor m_governor;
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

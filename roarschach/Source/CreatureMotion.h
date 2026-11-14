#pragma once

#include <array>
#include <cstdint>

namespace roar
{
    /**
     * The creature view's motion models. Pure: no JUCE, no threads, no clock, no allocation, so they
     * run on the GL thread and can be stepped in tests at any frame rate.
     */

    /**
     * The jaw: a heavy, damped spring that follows the phrase, not the syllables.
     *
     * x is the input envelope (5 ms / 120 ms), xf the fast one (5 ms / 30 ms), heat the roar's heat.
     * A phrase level P (rises in 0.03 s, holds 0.2 s, falls in 0.9 s, lets go after 0.15 s of real
     * quiet) decides how far the mouth opens, spread so soft phrases open little and shouts wide; a
     * two-pole syllable filter at 2.5 Hz shapes it within the phrase; a hysteresis switch (opens
     * above 0.32, closes below 0.18) keeps it from chattering at the threshold; a roar hold keeps it
     * open through short dips. A spring moves the jaw to that target: stiff (5.3 Hz at sizeJ 0.5)
     * for an onset, so the mouth opens with the sound, then heavy (1.3 Hz, critically damped) inside
     * the phrase, and 1.9 Hz at damping 0.72 to close; the teeth meet with a small inelastic
     * rebound. Fast detail goes only to the lips (lip, -1..1).
     */
    struct JawMotion
    {
        // outputs
        float y = 0.0f;          // opening 0..1.05 (the shader opens by y^1.6)
        float v = 0.0f;          // opening speed, 1/s
        float lip = 0.0f;        // lip-only syllable detail, -1..1
        float roarHold = 0.0f;   // seconds of roar hold left (0..0.45)

        // state (public for tests)
        float P = 0.0f, hold = 0.0f, quiet = 0.0f;
        float S1 = 0.0f, S2 = 0.0f, Lf = 0.0f, roarT = 0.0f;
        bool engaged = false;
        bool justEngaged = false;  // this step turned the switch on
        float dPdt = 0.0f;         // phrase level slope, 1/s
        float attack = 0.0f;       // seconds left of a fast onset opening

        void reset() { *this = JawMotion(); }

        /** sizeJ: the creature's size (SIZE knob plus the stage's jaw mass); 0.5 gives 1.3 / 1.9 Hz. */
        void step (float dt, float x, float xf, float heat, float sizeJ = 0.5f);

        static constexpr float c_openThreshold = 0.32f, c_closeThreshold = 0.18f;
        static constexpr float c_vMin = -3.5f, c_vMax = 2.4f, c_vMaxAttack = 8.0f;
    };

    /**
     * Secondary motion of the head: lag behind the jaw (a 0.8 Hz spring), an onset lunge, a slow
     * breath (0.20 Hz awake, 0.11 Hz asleep, nearly stopped while the phrase is loud, one deep breath
     * after a roar), a slow sway, and a lagged copy for the wings and tendrils. Mirror-safe: every
     * output moves the head up and down or scales it, never sideways.
     */
    struct HeadMotion
    {
        // outputs
        float offY = 0.0f;       // head offset, view heights (lag, lunge, sway)
        float scaleAdd = 0.0f;   // added head scale (sway)
        float follow = 0.0f;     // offY through a 0.35 s one-pole (wings, tendrils)
        float breath = 0.0f;     // breathing shape, about -1.25..1.25 times the depth (1, 2 after a roar)

        // state
        float lag = 0.0f, lagV = 0.0f, phase = 0.0f, deep = 0.0f, clock = 0.0f;
        float prevRoarHold = 0.0f;

        void reset() { *this = HeadMotion(); }

        /** jawY / phrase / justEngaged / dPdt / roarHold come from JawMotion; growl is the rectified growl
            envelope (0..1); sleep and awake are 0..1. */
        void step (float dt, float jawY, float phrase, bool justEngaged, float dPdt, float roarHold,
                   float growl, float sleep);

        float breathHz (float sleep, float phrase) const;
    };

    /**
     * The audio as forces on the ink: followers (every one at least 40 ms up and 150 ms down), a
     * sub onset detector (120 ms refractory), the exhale puff, the clip sparks and a flash limiter
     * (at most 3 visible onsets a second).
     */
    struct AudioForces
    {
        struct Inputs
        {
            float roarHeat = 0.0f;     // 0..1
            float growlMod = 0.0f;     // -1..1 (rectified here)
            float subLevel = 0.0f;     // 0..1
            float swirlOn = 0.0f;      // the swirl feature in effect, 0..1 (amount times activity)
            float ensemble = 0.0f;     // 0..1
            float clip = 0.0f;         // 0..1
            float strain = 0.0f;       // 0..1
            float jawVel = 0.0f;       // JawMotion::v
            float awake = 1.0f;
        };

        // outputs (followers; the view reads growl, swirl, subPulse and the sparks, the rest are read by the tests)
        float roar = 0.0f, growl = 0.0f, sub = 0.0f, swirl = 0.0f, ensemble = 0.0f, clip = 0.0f, strain = 0.0f;
        float puff = 0.0f;          // exhale, decays in 0.4 s
        float subPulse = 0.0f;      // the sub's push right after an onset (decays in 0.12 s)
        float clipSparks = 0.0f;    // 0..1 spark strength this frame (limited)
        int sparkSeed = 0;          // changes each spark frame (the shader hashes it)

        // state
        float sinceOnset = 1.0f, prevSubRaw = 0.0f, slowSub = 0.0f, sparkClock = 0.0f;
        std::array<float, 4> flashTimes { -9.0f, -9.0f, -9.0f, -9.0f };
        float clock = 0.0f;
        int onsets = 0, flashesAllowed = 0;   // counters (tests)

        void reset() { *this = AudioForces(); }
        void step (float dt, const Inputs& in);

        /** A visible onset may happen now (at most 3 in any second); records it if so. */
        bool allowFlash();
        /** A visible onset was allowed this recently (seconds): one event seen two ways is one flash. */
        bool flashedWithin (float seconds) const;

        static constexpr float c_attack = 0.040f, c_release = 0.150f;
        static constexpr float c_subRefractory = 0.120f, c_subOnsetJump = 0.15f;
        static constexpr int c_maxFlashesPerSecond = 3;
    };

    /**
     * Hits ripple the picture like a struck water surface. Each hit the processor counts
     * (HitDetector) starts a ring from the middle of the body, a little above or below it on the
     * fold, that grows at a fixed speed and dies away as it spreads; at most four are in flight (a
     * fifth replaces the earliest), and as their middles differ a little they cross. Each ring's
     * strength is the hit's; a weak one starts no ring at all. Rings count against the flash limit
     * (AudioForces::allowFlash), unless an onset was just allowed for the same moment. A sustained
     * low end trembles the surface finely instead (tremble, with its own clock).
     *
     * The shader reads the picture a little along the radius from each ring's middle: a surface
     * wave, never a drawn line (MonsterShaders).
     */
    struct SurfaceRipples
    {
        static constexpr int c_maxRings = 4;
        struct Ring
        {
            float age = -1.0f;       // seconds since the hit (< 0: none)
            float strength = 0.0f;   // the hit's, 0..1
            float offsetY = 0.0f;    // its middle above (+) or below the body's, view heights
        };

        std::array<Ring, c_maxRings> rings {};
        float tremble = 0.0f;        // the low end's fine tremble, 0..1 (followed)
        float trembleClock = 0.0f;   // its own clock, wrapped
        int lastHits = 0;
        bool seen = false;           // a count has been read (the first is taken as it is)
        int started = 0, refused = 0;   // counters (tests): rings started, hits the flash limit held back

        void reset() { *this = SurfaceRipples(); }

        /** hits: the detector's count; strength: the latest hit's; rumble: the low end, 0..1;
            awake: 0 asleep .. 1; forces: the flash limit. */
        void step (float dt, int hits, float strength, float rumble, float awake, AudioForces& forces);

        /** A ring's radius and its displacement's amplitude, view heights (0 for none). */
        float radius (int i) const;
        float amplitude (int i) const;
        int live() const;

        static constexpr float c_speed = 0.75f;        // view heights a second
        static constexpr float c_decaySeconds = 1.3f;  // the wave's own damping
        static constexpr float c_lifeSeconds = 2.6f;
        static constexpr float c_peak = 0.04f;         // the displacement of the strongest ring at its start, view heights
        static constexpr float c_minStrength = 0.12f;  // a hit weaker than this starts no ring
    };

    /**
     * The renderer's quality tier, chosen from the GPU and stepped by measured cost.
     *
     * Tiers: 3 the largest ink buffer, 2 a smaller one, 1 the smallest with a half-resolution
     * composite, 0 the single-pass stateless shader (no buffer). Slow and fast are judged from the GPU timer's EMA when there is one (so a busy
     * host message thread, which only stretches the frame interval, never costs quality), and from
     * the frame interval (over 1.4 times the target) only without timers. Down one tier after 20 slow
     * frames in a row (10 when the EMA is far over budget, and then two tiers at once), then 3 s before the next drop; up one
     * after 300 fast frames (EMA under 1.8 ms) and 30 s without a change, never above the starting
     * tier; an upgrade that turns slow within 5 s is reverted and upgrades stop for the context.
     * Measurement never goes below tier 1: the single pass (tier 0) costs more per pixel than tier 1,
     * so it is only for a GPU without the buffer's capabilities (a start tier of 0, final).
     *
     * Relief at the floor: a GPU that is still slow on its lowest tier (tier 1, or the single pass
     * where that is all it has, judged there from the frame interval: the single pass has no timer)
     * is given relief in three steps, each after 60 slow frames: the composite (or the single pass)
     * drawn at a third of the view's size instead of half, then 20 frames a second awake instead of
     * 30, then 15 where a frame still costs over 8 ms (or still overruns). Relief is taken back a
     * step at a time by the same rule that raises a tier, before any tier is raised.
     *
     * A lock on upgrades belongs to the load it was measured at: it is dropped when the view has
     * fewer than 0.7 times the pixels it had when the upgrade failed, and a settled tier remembered
     * from a view that much larger is not resumed at all (the smaller view starts from the top).
     * The first 2 s and stalled frames (a gap of a quarter second or more: hidden, host busy) are
     * ignored. GPU readings outside (0, 250] ms are ignored.
     */
    struct QualityGovernor
    {
        int startTier = 3, tier = 3;
        float ema = 0.0f;
        bool emaValid = false;
        int slowFrames = 0, fastFrames = 0;
        float elapsed = 0.0f, cooldown = 0.0f, sinceChange = 1.0e9f, sinceUpgrade = 1.0e9f;
        bool upgradesLocked = false;
        int changes = 0;
        int relief = 0;                 // 0 none; 1 a third-size draw; 2 and 20 Hz; 3 and 15 Hz (only on the lowest tier)
        double pixels = 0.0;            // the view's pixels now (0: not told)
        double lockPixels = 0.0;        // ... when the upgrade lock was set, or when the resumed tier was settled on
        bool resumed = false;           // started from a remembered tier, its load not yet compared with this view's

        void reset (int start);
        /** The view's size in pixels, each frame before step(): a lock or a remembered tier measured
            on a much larger view does not hold a smaller one down. */
        void noteLoad (double viewPixels);
        /** The lowest tier's draw is at 1 / this of the view's size (2, or 3 in relief). */
        int lowDivisor() const { return relief >= 1 ? 3 : 2; }
        /** Frames a second awake (30, or 20 or 15 in relief). */
        int awakeHz() const { return relief >= 3 ? 15 : relief == 2 ? 20 : 30; }
        /** The lowest tier measurement can reach: 1 (0 only when 0 is where it starts). */
        int floorTier() const { return startTier >= 1 ? 1 : 0; }
        /** Starts at the tier this GPU settled on before (capped by start), keeping its upgrade lock
            and its relief; settledPixels: the view's size it settled at (0: not known). */
        void resume (int start, int settled, bool locked, double settledPixels = 0.0, int settledRelief = 0);
        /** One frame. interval and target in seconds; gpuMs < 0 when there is no reading; stalled: the
            frame came after a long gap (not a measurement). Returns the tier. */
        int step (float interval, float target, float gpuMs, bool stalled);

        static constexpr float c_warmupSeconds = 2.0f, c_cooldownSeconds = 30.0f, c_dropCooldownSeconds = 3.0f;
        static constexpr float c_failedUpgradeSeconds = 5.0f;
        static constexpr float c_slowMs = 4.0f, c_fastMs = 1.8f, c_slowIntervalRatio = 1.4f, c_farOverRatio = 2.5f;
        static constexpr int c_slowFramesToDrop = 20, c_fastFramesToRise = 300, c_slowFramesToRelieve = 60;
        static constexpr float c_reliefFarMs = 8.0f;     // the last step of relief: a frame still costs over this
        static constexpr double c_lighterLoad = 0.7;      // a view with fewer than this share of the pixels is another load

        /** The starting tier for a GL_RENDERER / GL_VENDOR string (0 for software renderers). */
        static int startTierFor (const char* renderer, const char* vendor);
    };
}

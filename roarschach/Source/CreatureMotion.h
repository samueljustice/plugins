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

        // outputs (followers)
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

        static constexpr float c_attack = 0.040f, c_release = 0.150f;
        static constexpr float c_subRefractory = 0.120f, c_subOnsetJump = 0.15f;
        static constexpr int c_maxFlashesPerSecond = 3;
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

        void reset (int start);
        /** The lowest tier measurement can reach: 1 (0 only when 0 is where it starts). */
        int floorTier() const { return startTier >= 1 ? 1 : 0; }
        /** Starts at the tier this GPU settled on before (capped by start), keeping its upgrade lock. */
        void resume (int start, int settled, bool locked);
        /** One frame. interval and target in seconds; gpuMs < 0 when there is no reading; stalled: the
            frame came after a long gap (not a measurement). Returns the tier. */
        int step (float interval, float target, float gpuMs, bool stalled);

        static constexpr float c_warmupSeconds = 2.0f, c_cooldownSeconds = 30.0f, c_dropCooldownSeconds = 3.0f;
        static constexpr float c_failedUpgradeSeconds = 5.0f;
        static constexpr float c_slowMs = 4.0f, c_fastMs = 1.8f, c_slowIntervalRatio = 1.4f, c_farOverRatio = 2.5f;
        static constexpr int c_slowFramesToDrop = 20, c_fastFramesToRise = 300;

        /** The starting tier for a GL_RENDERER / GL_VENDOR string (0 for software renderers). */
        static int startTierFor (const char* renderer, const char* vendor);
    };
}

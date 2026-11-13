#include "CreatureEvolution.h"

#include "CreatureAnatomy.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        using P = CreatureEvolution::Param;

        float clamp01 (float x) { return std::min (1.0f, std::max (0.0f, x)); }

        float smoothstep (float a, float b, float x)
        {
            const float t = clamp01 ((x - a) / (b - a));
            return t * t * (3.0f - 2.0f * t);
        }

        float finite (float x, float lo, float hi)
        {
            return std::isfinite (x) ? std::min (hi, std::max (lo, x)) : 0.0f;
        }

        void follow (float& state, float target, float dt, float up, float down)
        {
            const float tau = target > state ? up : down;
            state += (target - state) * (1.0f - std::exp (-dt / tau));
        }

        float toDb (float gain) { return 20.0f * std::log10 (std::max (gain, 1.0e-6f)); }

        //======================================================================
        // The stage spine: six anchors at s = 0, 0.2, ..., 1. Every row is one parameter. (What the
        // stage means for the blot's shape is CreatureAnatomy's.) Colours are sRGB 0..1 and blended
        // in linear light.
        struct Row { P param; float v[6]; };

        constexpr Row c_table[] = {
            { P::jawMass,       { 0.0f,  0.0f,  0.0f,  0.15f, 0.25f, 0.30f } },
            { P::formFloor,     { 0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.30f } },
            { P::formCeiling,   { 0.55f, 0.70f, 0.85f, 0.90f, 0.95f, 1.00f } },
        };

        // The cloud's light per anchor: cool blue-grey; with a pastel blush; cooler; slate; warm
        // slate; violet slate (only its hue reaches the picture, a small lean within the palette).
        struct Rgb { float r, g, b; };
        constexpr Rgb c_cloudLit[6]  = { { 0.400f, 0.440f, 0.520f }, { 0.470f, 0.430f, 0.500f }, { 0.390f, 0.420f, 0.480f },
                                         { 0.330f, 0.340f, 0.390f }, { 0.380f, 0.370f, 0.390f }, { 0.400f, 0.380f, 0.530f } };

        float toLinear (float c) { return std::pow (std::max (c, 0.0f), 2.2f); }
        float toDisplay (float c) { return std::pow (std::max (c, 0.0f), 1.0f / 2.2f); }

        Rgb mixLinear (const Rgb& a, const Rgb& b, float t)
        {
            return { toDisplay (toLinear (a.r) + (toLinear (b.r) - toLinear (a.r)) * t),
                     toDisplay (toLinear (a.g) + (toLinear (b.g) - toLinear (a.g)) * t),
                     toDisplay (toLinear (a.b) + (toLinear (b.b) - toLinear (a.b)) * t) };
        }

        // Rotates the hue by turns (a YIQ rotation: cheap, keeps luminance).
        Rgb hueShift (const Rgb& c, float turns)
        {
            const float y = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
            const float i = 0.596f * c.r - 0.274f * c.g - 0.322f * c.b;
            const float q = 0.211f * c.r - 0.523f * c.g + 0.312f * c.b;
            const float a = turns * 6.2831853f;
            const float i2 = i * std::cos (a) - q * std::sin (a);
            const float q2 = i * std::sin (a) + q * std::cos (a);
            return { std::max (0.0f, y + 0.956f * i2 + 0.621f * q2),
                     std::max (0.0f, y - 0.272f * i2 - 0.647f * q2),
                     std::max (0.0f, y - 1.106f * i2 + 1.703f * q2) };
        }

        void setRgb (CreatureEvolution::Params& p, int first, const Rgb& c)
        {
            p[(size_t) first] = c.r;
            p[(size_t) first + 1] = c.g;
            p[(size_t) first + 2] = c.b;
        }

        Rgb getRgb (const CreatureEvolution::Params& p, int first)
        {
            return { p[(size_t) first], p[(size_t) first + 1], p[(size_t) first + 2] };
        }

        //======================================================================
        // What one seed means for the picture beyond its blot: a small hue nudge of the cloud's light.
        std::uint32_t splitmix (std::uint32_t& state)
        {
            std::uint32_t z = (state += 0x9e3779b9u);
            z = (z ^ (z >> 16)) * 0x85ebca6bu;
            z = (z ^ (z >> 13)) * 0xc2b2ae35u;
            return z ^ (z >> 16);
        }

        float hueOf (std::uint32_t seed)
        {
            std::uint32_t s = seed;
            const auto uni = [&s] { return (float) (splitmix (s) >> 8) / 16777216.0f; };
            for (int i = 0; i < 8; ++i)   // (the draws before it, so each seed keeps the hue it had)
                uni();
            return 0.04f * (2.0f * uni() - 1.0f);
        }

        // Character offsets (fluid, beast, eldritch, deep), applied relative to a neutral mix (the
        // anatomy bends the blot itself by the same leans): deep weighs the jaw.
        struct Offset { P param; float d[4]; };
        constexpr Offset c_offsets[] = {
            { P::jawMass,       { 0.0f,  0.0f,  0.0f,  0.45f } },
        };

        // Each corner's light: the lit cloud leans toward these (linear mix, up to c_cornerTint of
        // the way at a pure corner). Fluid: cold sea-green; beast: ember; eldritch: violet; deep:
        // abyssal blue.
        constexpr Rgb c_cornerLit[4]  = { { 0.330f, 0.470f, 0.480f }, { 0.470f, 0.380f, 0.330f },
                                          { 0.430f, 0.360f, 0.540f }, { 0.250f, 0.320f, 0.450f } };
        constexpr float c_cornerTint = 0.45f;

        CreatureEvolution::Params blend (float s, const std::array<float, 4>& weights)
        {
            CreatureEvolution::Params p {};
            s = clamp01 (std::isfinite (s) ? s : 0.0f);
            const int k = std::min (4, (int) (s * 5.0f));
            const float a = (float) k * 0.2f;
            // Smoothstep between neighbours: C1 in s, lingering a little near each anchor.
            const float w = smoothstep (a, a + 0.2f, s);

            for (const auto& row : c_table)
                p[(size_t) row.param] = row.v[k] + (row.v[k + 1] - row.v[k]) * w;

            setRgb (p, P::cloudLitR, mixLinear (c_cloudLit[k], c_cloudLit[k + 1], w));

            // Character shows only once there is a creature to show it.
            const float charScale = smoothstep (0.15f, 0.6f, s);
            for (const auto& o : c_offsets)
            {
                float d = 0.0f;
                for (size_t f = 0; f < 4; ++f)
                    d += (weights[f] - 0.25f) * (4.0f / 3.0f) * o.d[f];
                p[(size_t) o.param] += d * charScale;
            }
            for (size_t f = 0; f < 4; ++f)
                p[(size_t) P::charFluid + f] = (weights[f] - 0.25f) * (4.0f / 3.0f) * charScale;
            for (size_t f = 0; f < 4; ++f)
            {
                const float t = clamp01 ((weights[f] - 0.25f) * (4.0f / 3.0f)) * charScale * c_cornerTint;
                if (t > 0.0f)
                    setRgb (p, P::cloudLitR, mixLinear (getRgb (p, P::cloudLitR), c_cornerLit[f], t));
            }

            p[P::jawMass] = std::max (0.0f, p[P::jawMass]);
            p[P::stageOut] = s;
            return p;
        }

        void applySeed (CreatureEvolution::Params& p, std::uint32_t from, std::uint32_t to, float glide,
                        std::uint32_t hashFrom = 0, std::uint32_t hashTo = 0)
        {
            const std::uint32_t anatomyA = CreatureAnatomy::combine (from, hashFrom), anatomyB = CreatureAnatomy::combine (to, hashTo);
            p[P::anatomyALo] = (float) (anatomyA & 0xffffu);
            p[P::anatomyAHi] = (float) (anatomyA >> 16);
            p[P::anatomyBLo] = (float) (anatomyB & 0xffffu);
            p[P::anatomyBHi] = (float) (anatomyB >> 16);
            const float g = smoothstep (0.0f, 1.0f, glide);
            p[P::seedGlide] = g;
            const float hue = hueOf (from) + (hueOf (to) - hueOf (from)) * g;
            setRgb (p, P::cloudLitR, hueShift (getRgb (p, P::cloudLitR), hue));
        }
    }

    //==========================================================================
    CreatureEvolution::CreatureEvolution (std::uint32_t seed)
        : m_seedFrom (seed), m_seedTo (seed)
    {
    }

    float CreatureEvolution::intensityOf (const Inputs& in)
    {
        const float inDb = toDb (finite (in.inputLevel, 0.0f, 8.0f));
        const float outDb = toDb (finite (in.outputLevel, 0.0f, 8.0f));
        const float gate = smoothstep (-55.0f, -35.0f, inDb) * (in.stale ? 0.0f : 1.0f);
        const float loud = smoothstep (-36.0f, -10.0f, outDb);
        const float heat = finite (in.roarHeat, 0.0f, 1.0f);
        const float growl = clamp01 (std::abs (finite (in.growlMod, -1.0f, 1.0f)) * 1.5f);
        const float sub = finite (in.subLevel, 0.0f, 1.0f);
        const float strain = clamp01 (std::abs (finite (in.limiterGrDb, -60.0f, 60.0f)) / 9.0f);
        const float clip = clamp01 (std::abs (finite (in.clipDb, -60.0f, 60.0f)) / 4.0f);
        const float gravel = finite (in.gravelActive, 0.0f, 1.0f) * finite (in.meanBand, 0.0f, 1.0f);

        // A probabilistic OR: any one strong source counts, several compound, the result stays <= 1.
        // (The heat weighs most: a loud ROAR on its own, I ~0.78, reaches the top in about ten minutes.)
        const auto product = [] (float h, float g, float s, float st, float c, float gr, float l)
        {
            return 1.0f - (1.0f - 0.60f * h) * (1.0f - 0.25f * g) * (1.0f - 0.30f * s) * (1.0f - 0.30f * st)
                             * (1.0f - 0.35f * c) * (1.0f - 0.20f * gr) * (1.0f - 0.35f * l);
        };
        static const float maxRaw = product (1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        return clamp01 (gate * product (heat, growl, sub, strain, clip, gravel, loud) / maxRaw);
    }

    void CreatureEvolution::step (float dt, const Inputs& rawIn)
    {
        if (! std::isfinite (dt))
            dt = 0.0f;

        float simulated = std::min (0.5f, std::max (0.0f, dt)) * m_timeScale;

        Inputs in = rawIn;
        in.inputLevel = finite (in.inputLevel, 0.0f, 8.0f);
        in.outputLevel = finite (in.outputLevel, 0.0f, 8.0f);
        in.jaw = finite (in.jaw, 0.0f, 1.0f);

        const float intensity = intensityOf (in);
        const float loud = in.stale ? 0.0f : smoothstep (-36.0f, -10.0f, toDb (in.outputLevel));
        m_intensity = intensity;

        // Substeps keep the integrators exact and the second-order smoother stable at any speed.
        while (simulated > 0.0f)
        {
            const float h = std::min (simulated, 0.05f);
            substep (h, in, intensity, loud);
            simulated -= h;
        }
    }

    void CreatureEvolution::substep (float dt, const Inputs& in, float intensity, float loud)
    {
        const bool quiet = in.stale || in.inputLevel < 0.001f;
        m_quietSeconds = quiet ? std::min (m_quietSeconds + dt, 3600.0f) : 0.0f;

        follow (m_burst, intensity, dt, 1.5f, 6.0f);

        // Seed glide (and a seed queued behind it).
        if (m_seedGlide < 1.0f)
            m_seedGlide = std::min (1.0f, m_seedGlide + dt / c_seedGlideSeconds);
        if (m_seedGlide >= 1.0f && m_hasQueuedSeed)
        {
            m_seedFrom = m_seedTo;
            m_hashFrom = m_hashTo;
            m_seedTo = m_queuedSeed;
            m_hashTo = m_queuedHash;
            m_seedGlide = 0.0f;
            m_hasQueuedSeed = false;
        }

        m_eggCooldown = std::max (0.0f, m_eggCooldown - dt);
        stepEgg (dt, in);

        if (m_resetSeconds >= 0.0f)
        {
            // A fixed, continuous path home: slew limits bypassed, never a cut.
            m_resetSeconds += dt;
            const float k = 1.0f - smoothstep (0.0f, 1.0f, m_resetSeconds / c_resetSeconds);
            m_energy = m_resetFrom.energy * k;
            m_experience = m_resetFrom.experience * k;
            m_stage = m_slewed = m_stageTarget = m_resetFrom.stage * k;
            m_stageVel = 0.0f;
            for (size_t f = 0; f < 4; ++f)
                m_family[f] = m_resetFrom.family[f] * k;

            if (m_resetSeconds >= c_resetSeconds)
                m_resetSeconds = -1.0f;
            return;
        }

        // Frozen, or the logo showing: the creature holds its form.
        if (m_mode == Mode::frozen || m_eggState != EggState::idle)
            return;

        // Session heat: a dead band, then rise 20 s, fall 150 s. Real silence lets it go all the way.
        if (std::abs (m_burst - m_energy) > 0.03f || (m_burst < 0.005f && m_energy > 0.0f))
            follow (m_energy, m_burst, dt, 20.0f, 150.0f);

        // Experience: fills over ~10 min of heat, forgets over ~40 min of quiet.
        if (m_energy > 0.35f)
            m_experience += smoothstep (0.35f, 0.85f, m_energy) / 600.0f * dt;
        else if (m_energy < 0.2f)
            m_experience -= m_experience / 2400.0f * dt;
        m_experience = clamp01 (m_experience);

        m_stageTarget = clamp01 (m_energy * (0.7f + 0.3f * m_experience) + 0.25f * m_experience);

        const float up = c_stageRiseRate * dt, down = c_stageFallRate * dt;
        m_slewed += std::min (up, std::max (-down, m_stageTarget - m_slewed));

        // Critically damped, 10-90 % in about 6 s (omega = 3.36 / 6).
        constexpr float omega = 0.56f;
        const float acc = omega * omega * (m_slewed - m_stage) - 2.0f * omega * m_stageVel;
        m_stageVel += acc * dt;
        m_stage = clamp01 (m_stage + m_stageVel * dt);

        // Character: which modules have been doing the work (silence feeds nothing).
        const float heat = finite (in.roarHeat, 0.0f, 1.0f);
        const float growl = clamp01 (std::abs (finite (in.growlMod, -1.0f, 1.0f)) * 1.5f);
        const float clip = clamp01 (std::abs (finite (in.clipDb, -60.0f, 60.0f)) / 4.0f);
        const float strain = clamp01 (std::abs (finite (in.limiterGrDb, -60.0f, 60.0f)) / 9.0f);
        const std::array<float, 4> drive {
            clamp01 (0.4f * finite (in.shimmerOn, 0.0f, 1.0f) + 0.3f * finite (in.swirlOn, 0.0f, 1.0f)
                     + 0.4f * finite (in.ensembleWidth, 0.0f, 1.0f) + 0.3f * finite (in.liquidKnob, 0.0f, 1.0f)),
            clamp01 (0.6f * heat + 0.5f * growl + 0.4f * clip),
            clamp01 (2.0f * finite (in.gravelActive, 0.0f, 1.0f) * finite (in.meanBand, 0.0f, 1.0f)
                     + 0.6f * finite (in.ringAmount, 0.0f, 1.0f) + 0.6f * finite (in.overtoneLevel, 0.0f, 1.0f)),
            clamp01 (0.8f * finite (in.subLevel, 0.0f, 1.0f) + 0.4f * std::max (0.0f, -finite (in.pitchSt, -48.0f, 48.0f) / 12.0f)
                     + 0.4f * strain),
        };
        for (size_t f = 0; f < 4; ++f)
            follow (m_family[f], drive[f] * loud, dt, 45.0f, 180.0f);
    }

    void CreatureEvolution::stepEgg (float dt, const Inputs& in)
    {
        const bool asleep = m_quietSeconds >= c_sleepAfterSeconds;
        const bool live = m_mode == Mode::live && m_resetSeconds < 0.0f;

        // A fresh shout: the jaw past 0.8 after at least half a second below 0.3 (a breath, then a roar).
        bool shout = false;
        // (one unbroken low stretch: short dips between syllables never add up to a breath)
        if (in.jaw < 0.3f)
            m_jawLowSeconds = std::min (m_jawLowSeconds + dt, 60.0f);
        else if (in.jaw > 0.8f)
        {
            shout = m_jawLowSeconds >= 0.5f;
            m_jawLowSeconds = 0.0f;
        }
        else
        {
            m_jawLowSeconds = 0.0f;
        }

        if (m_eggState == EggState::idle)
        {
            const bool atApex = m_stage >= c_apexStage && ! asleep && ! in.stale && m_burst >= 0.5f && live
                             && m_eggCooldown <= 0.0f;
            m_apexSeconds = atApex ? m_apexSeconds + dt : std::max (0.0f, m_apexSeconds - 0.25f * dt);

            // Only while a view can draw the logo: an egg no one can see would hold the evolution and
            // spend the cooldown for nothing (it stays armed for the next shout instead).
            if (isEggArmed() && shout && ! asleep && ! in.stale && live && m_eggCooldown <= 0.0f && in.eggVisible)
            {
                m_eggState = EggState::forming;
                m_eggPhase = 0.0f;
                m_eggAborting = false;
                m_apexSeconds = 0.0f;
                m_eggCooldown = c_eggCooldownSeconds;
            }
            return;
        }

        // Silence (or a stop, or Frozen, or a reset, or the view closing) during the egg: dissolve early,
        // before sleep's fade completes.
        if (m_quietSeconds > 1.0f || in.stale || ! live || ! in.eggVisible)
            m_eggAborting = true;

        if (m_eggAborting)
        {
            m_eggLevel = std::max (0.0f, m_eggLevel - dt / c_eggAbortSeconds);
            if (m_eggLevel <= 0.0f)
            {
                m_eggState = EggState::idle;
                m_eggAborting = false;
            }
            return;
        }

        m_eggPhase += dt;
        switch (m_eggState)
        {
            case EggState::forming:
                m_eggLevel = clamp01 (m_eggPhase / c_eggFormSeconds);
                if (m_eggPhase >= c_eggFormSeconds)
                {
                    m_eggState = EggState::holding;
                    m_eggPhase = 0.0f;
                }
                break;
            case EggState::holding:
                m_eggLevel = 1.0f;
                if (m_eggPhase >= c_eggHoldSeconds)
                {
                    m_eggState = EggState::dissolving;
                    m_eggPhase = 0.0f;
                }
                break;
            case EggState::dissolving:
                m_eggLevel = clamp01 (1.0f - m_eggPhase / c_eggDissolveSeconds);
                if (m_eggPhase >= c_eggDissolveSeconds)
                {
                    m_eggState = EggState::idle;
                    m_eggLevel = 0.0f;
                }
                break;
            case EggState::idle:
                break;
        }
    }

    float CreatureEvolution::getEggAmount() const
    {
        return smoothstep (0.0f, 1.0f, m_eggLevel);
    }

    void CreatureEvolution::triggerEggForTests()
    {
        m_eggState = EggState::forming;
        m_eggPhase = 0.0f;
        m_eggAborting = false;
        m_eggCooldown = c_eggCooldownSeconds;
    }

    std::array<float, 4> CreatureEvolution::getCharacterWeights() const
    {
        float sum = 0.0f, sumSq = 0.0f;
        for (float f : m_family)
        {
            sum += f;
            sumSq += f * f;
        }
        std::array<float, 4> w { 0.25f, 0.25f, 0.25f, 0.25f };
        const float towards = smoothstep (0.05f, 0.3f, sum);
        if (sumSq > 1.0e-9f)
            for (size_t f = 0; f < 4; ++f)
                w[f] = 0.25f + (m_family[f] * m_family[f] / sumSq - 0.25f) * towards;
        return w;
    }

    CreatureEvolution::Params CreatureEvolution::evaluateAt (float stage, const std::array<float, 4>& weights,
                                                             float energy, std::uint32_t seed)
    {
        auto p = blend (stage, weights);
        applySeed (p, seed, seed, 1.0f);
        p[P::formBase] = p[P::formFloor] + (p[P::formCeiling] - p[P::formFloor]) * smoothstep (0.0f, 1.0f, clamp01 (energy));
        p[P::energyOut] = clamp01 (energy);
        p[P::egg] = 0.0f;
        return p;
    }

    CreatureEvolution::Params CreatureEvolution::evaluate() const
    {
        const bool pinned = m_pinnedStage >= 0.0f;
        auto p = blend (pinned ? m_pinnedStage : m_stage, pinned ? m_pinnedWeights : getCharacterWeights());
        applySeed (p, m_seedFrom, m_seedTo, m_seedGlide, m_hashFrom, m_hashTo);

        const float energy = pinned ? 1.0f : m_energy;
        p[P::formBase] = p[P::formFloor] + (p[P::formCeiling] - p[P::formFloor]) * smoothstep (0.0f, 1.0f, energy);
        p[P::energyOut] = energy;

        p[P::egg] = getEggAmount();
        return p;
    }

    //==========================================================================
    void CreatureEvolution::reset (std::uint32_t newSeed)
    {
        m_resetFrom = currentState();   // (a reset during a reset starts from where the first one is)
        m_resetSeconds = 0.0f;
        m_apexSeconds = 0.0f;

        // A glide already running finishes first; the new blot follows on from where it ends.
        if (m_seedGlide < 1.0f)
        {
            if (! m_hasQueuedSeed)
                m_queuedHash = m_hashTo;
            m_queuedSeed = newSeed;
            m_hasQueuedSeed = true;
        }
        else
        {
            m_seedFrom = m_seedTo;
            m_hashFrom = m_hashTo;
            m_seedTo = newSeed;
            m_seedGlide = 0.0f;
        }
    }

    void CreatureEvolution::setPresetHash (std::uint32_t hash, bool glide)
    {
        if (! glide)
        {
            // at once: the blot on screen is this preset's (a glide under way keeps its seeds)
            m_hashFrom = m_hashTo = m_queuedHash = hash;
            return;
        }
        if (hash == getPresetHash())
            return;
        if (m_seedGlide < 1.0f)
        {
            // a glide already running finishes first; this preset's blot follows on
            if (! m_hasQueuedSeed)
                m_queuedSeed = m_seedTo;
            m_queuedHash = hash;
            m_hasQueuedSeed = true;
        }
        else
        {
            m_seedFrom = m_seedTo;
            m_hashFrom = m_hashTo;
            m_hashTo = hash;
            m_seedGlide = 0.0f;
        }
    }

    std::uint32_t CreatureEvolution::getAnatomySeed() const
    {
        return CreatureAnatomy::combine (getSeed(), getPresetHash());
    }

    void CreatureEvolution::setSeed (std::uint32_t seed)
    {
        m_seedFrom = m_seedTo = seed;
        m_hashFrom = m_hashTo;
        m_seedGlide = 1.0f;
        m_hasQueuedSeed = false;
    }

    CreatureEvolution::Persisted CreatureEvolution::getPersisted() const
    {
        // A reset is a decision, not a path: a save made while it eases home stores where it ends,
        // so reloading never cancels it half way.
        if (isResetting())
            return {};
        return currentState();
    }

    CreatureEvolution::Persisted CreatureEvolution::currentState() const
    {
        Persisted s;
        s.energy = m_energy;
        s.experience = m_experience;
        s.stage = m_stage;
        s.family = m_family;
        return s;
    }

    void CreatureEvolution::setPersisted (const Persisted& s)
    {
        m_energy = finite (s.energy, 0.0f, 1.0f);
        m_experience = finite (s.experience, 0.0f, 1.0f);
        m_stage = m_slewed = m_stageTarget = finite (s.stage, 0.0f, 1.0f);
        m_stageVel = 0.0f;
        for (size_t f = 0; f < 4; ++f)
            m_family[f] = finite (s.family[f], 0.0f, 1.0f);
        // The phrase average starts where the heat is, so a loaded creature doesn't cool at once.
        m_burst = m_energy;
        m_resetSeconds = -1.0f;
    }

    //==========================================================================
    CreatureShared::CreatureShared()
    {
        publish (CreatureEvolution::evaluateAt (0.4f, { 0.25f, 0.25f, 0.25f, 0.25f }, 1.0f, 0x1234567u));
    }

    void CreatureShared::publish (const CreatureEvolution::Params& p)
    {
        // A sequence lock (one writer at a time: every caller holds the processor's creature lock).
        const auto s = sequence.load (std::memory_order_relaxed);
        sequence.store (s + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        for (size_t i = 0; i < p.size(); ++i)
            values[i].store (p[i], std::memory_order_relaxed);
        sequence.store (s + 2u, std::memory_order_release);
    }

    CreatureEvolution::Params CreatureShared::read() const
    {
        CreatureEvolution::Params p {};
        // Retries while a publish is half written; after a few tries (a writer stalled mid-way) the
        // last copy is used as it is, which only the smoothed values could notice.
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const auto before = sequence.load (std::memory_order_acquire);
            for (size_t i = 0; i < p.size(); ++i)
                p[i] = values[i].load (std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_acquire);
            if ((before & 1u) == 0u && sequence.load (std::memory_order_relaxed) == before)
                break;
        }
        for (auto& v : p)
            v = std::isfinite (v) ? v : 0.0f;
        return p;
    }

    void CreatureFormation::step (float dt, float jaw, float loud, float heat, float sleep, float formBase, float egg)
    {
        dt = std::isfinite (dt) ? std::min (0.5f, std::max (0.0f, dt)) : 0.0f;
        const float drive = std::max (finite (jaw, 0.0f, 1.0f), std::max (0.8f * finite (loud, 0.0f, 1.0f), finite (heat, 0.0f, 1.0f)));
        follow (fast, drive, dt, 0.25f, 2.0f);
        const float base = finite (formBase, 0.0f, 1.0f);
        const float s = finite (sleep, 0.0f, 1.0f);
        float target = clamp01 (base + 0.35f * fast * (1.0f - base)) * (1.0f - 0.85f * s);
        // the logo is formed of the same cloud, held condensed while it shows
        target = std::max (target, 0.85f * finite (egg, 0.0f, 1.0f) * (1.0f - s));
        follow (formation, target, dt, 1.2f, 4.0f);
        follow (ink, 1.0f - s, dt, 0.9f, 2.0f);
        ink = clamp01 (ink);
        fast = clamp01 (fast);
        formation = clamp01 (formation);
    }

    float CreatureFormation::inkFormation() const
    {
        return smoothstep (0.03f, 0.97f, finite (ink, 0.0f, 1.0f));
    }
}

#include "CreatureEvolution.h"

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
        // The stage spine: six anchor forms at s = 0, 0.2, ..., 1. Every row is one parameter.
        // (Internal names: dormant blot, moth spirit, horned beast, liquid horror, horned wyrm,
        // eldritch crown.) Colours are sRGB 0..1 and blended in linear light.
        struct Row { P param; float v[6]; };

        constexpr Row c_table[] = {
            { P::cranScale,     { 0.85f, 0.92f, 1.00f, 1.04f, 1.06f, 1.10f } },
            { P::snout,         { 0.0f,  0.0f,  0.0f,  0.1f,  0.8f,  0.5f } },
            { P::jawMass,       { 0.0f,  0.0f,  0.0f,  0.15f, 0.25f, 0.30f } },
            { P::brow,          { 0.2f,  0.3f,  1.0f,  0.8f,  1.2f,  1.0f } },
            { P::wingSpan,      { 1.10f, 1.25f, 1.00f, 0.95f, 1.15f, 1.20f } },
            { P::wingLift,      { 0.0f,  0.18f, 0.0f, -0.05f, 0.12f, 0.10f } },
            { P::eyespots,      { 0.0f,  1.0f,  0.2f,  0.0f,  0.0f,  0.4f } },
            { P::fingers,       { 0.0f,  0.0f,  0.3f,  0.0f,  1.0f,  0.8f } },
            { P::hornLength,    { 0.0f,  0.15f, 1.0f,  0.8f,  1.3f,  1.1f } },
            { P::hornCurl,      { 0.0f,  0.0f,  0.0f,  0.3f,  0.6f,  0.4f } },
            { P::hornTine,      { 0.0f,  0.0f,  0.0f,  0.0f,  1.0f,  0.6f } },
            { P::crown,         { 0.0f,  0.0f,  0.0f,  0.1f,  0.35f, 1.0f } },
            { P::browEyes,      { 0.0f,  0.0f,  0.0f,  0.2f,  0.5f,  1.0f } },
            { P::cheekEyes,     { 0.0f,  0.0f,  0.0f,  0.0f,  0.2f,  1.0f } },
            { P::centreEye,     { 0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  1.0f } },
            { P::eyeSize,       { 0.7f,  1.1f,  1.0f,  0.9f,  0.85f, 0.9f } },
            { P::mawWidth,      { 0.80f, 0.85f, 1.00f, 1.20f, 1.12f, 1.15f } },   // never wider than the cheeks
            { P::teethLength,   { 0.5f,  0.6f,  1.0f,  1.2f,  1.3f,  1.1f } },
            { P::thirdRow,      { 0.0f,  0.0f,  0.0f,  1.0f,  0.6f,  0.8f } },
            { P::cheekSpikes,   { 0.0f,  0.0f,  1.0f,  0.5f,  1.0f,  1.0f } },
            { P::solidity,      { 0.45f, 0.55f, 0.75f, 0.85f, 0.85f, 0.80f } },
            { P::bleed,         { 1.6f,  1.3f,  1.0f,  1.4f,  0.9f,  1.1f } },
            { P::dryBrush,      { 0.8f,  0.5f,  0.3f,  0.0f,  0.4f,  0.2f } },
            { P::gloss,         { 0.2f,  0.3f,  0.5f,  1.6f,  0.8f,  1.0f } },
            { P::dripRate,      { 0.5f,  0.6f,  1.0f,  2.5f,  1.0f,  1.4f } },
            { P::extraDrips,    { 0.0f,  0.0f,  0.0f,  1.0f,  0.3f,  0.5f } },
            { P::extraTendrils, { 0.0f,  0.0f,  0.0f,  1.0f,  0.3f,  1.0f } },
            { P::tendrilReach,  { 0.0f,  0.1f,  0.2f,  0.7f,  0.4f,  0.8f } },
            { P::pastel,        { 0.15f, 0.8f,  0.1f,  0.0f,  0.0f,  0.6f } },
            { P::redBlack,      { 0.0f,  0.0f,  0.15f, 0.8f,  0.6f,  0.4f } },
            { P::glow,          { 0.35f, 0.6f,  1.0f,  1.1f,  1.3f,  1.6f } },
            { P::bioLum,        { 0.0f,  0.3f,  0.0f,  0.2f,  0.0f,  1.0f } },
            { P::antennae,      { 0.0f,  1.0f,  0.0f,  0.0f,  0.0f,  0.0f } },
            { P::scales,        { 0.0f,  0.0f,  0.2f,  0.0f,  1.0f,  0.5f } },
            { P::centralFigure, { 1.0f,  0.6f,  0.0f,  0.0f,  0.0f,  0.3f } },
            { P::satellites,    { 1.0f,  0.8f,  0.6f,  1.0f,  0.5f,  0.7f } },
            { P::inkiness,      { 0.35f, 0.30f, 0.25f, 0.60f, 0.30f, 0.40f } },
            { P::fogFront,      { 0.35f, 0.30f, 0.25f, 0.22f, 0.20f, 0.18f } },
            { P::formFloor,     { 0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.30f } },
            { P::formCeiling,   { 0.55f, 0.70f, 0.85f, 0.90f, 0.95f, 1.00f } },
            { P::coffeeRing,    { 0.6f,  0.5f,  0.4f,  0.8f,  0.3f,  0.5f } },
            { P::eyeSpacing,    { 1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f } },
            { P::crownSpread,   { 1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f } },
            { P::spare0,        { 0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f } },
        };

        // Cloud colours per anchor: cool blue-grey; with a pastel blush; with a faint ember
        // underglow; slate with a red underglow; slate with a warm storm underglow; violet slate
        // with a cyan scatter.
        struct Rgb { float r, g, b; };
        constexpr Rgb c_cloud[6]     = { { 0.120f, 0.140f, 0.180f }, { 0.150f, 0.140f, 0.180f }, { 0.125f, 0.140f, 0.175f },
                                         { 0.110f, 0.115f, 0.140f }, { 0.125f, 0.120f, 0.130f }, { 0.130f, 0.115f, 0.185f } };
        constexpr Rgb c_cloudLit[6]  = { { 0.400f, 0.440f, 0.520f }, { 0.470f, 0.430f, 0.500f }, { 0.390f, 0.420f, 0.480f },
                                         { 0.330f, 0.340f, 0.390f }, { 0.380f, 0.370f, 0.390f }, { 0.400f, 0.380f, 0.530f } };
        constexpr Rgb c_underglow[6] = { { 0.120f, 0.140f, 0.190f }, { 0.320f, 0.170f, 0.230f }, { 0.340f, 0.130f, 0.050f },
                                         { 0.440f, 0.050f, 0.060f }, { 0.460f, 0.220f, 0.060f }, { 0.100f, 0.380f, 0.450f } };
        constexpr Rgb c_redInk = { 0.700f, 0.150f, 0.180f };   // crimson ink
        constexpr Rgb c_logoRed = { 0.812f, 0.114f, 0.208f };  // the logo's cherry
        // Pastel washes (two picked per creature)
        constexpr Rgb c_washes[5] = { { 0.910f, 0.627f, 0.659f }, { 0.941f, 0.706f, 0.549f }, { 0.612f, 0.769f, 0.894f },
                                      { 0.659f, 0.784f, 0.627f }, { 0.910f, 0.863f, 0.565f } };

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
        // What one seed means: a blot shape (an offset into the mirrored noise), a cloud offset,
        // a few percent of jitter on proportions, a hue nudge and a pair of washes.
        struct SeedLook
        {
            float ax, ay, cx, cy;
            float span, curl, spacing, spread, hue;
            int washA, washB;
        };

        std::uint32_t splitmix (std::uint32_t& state)
        {
            std::uint32_t z = (state += 0x9e3779b9u);
            z = (z ^ (z >> 16)) * 0x85ebca6bu;
            z = (z ^ (z >> 13)) * 0xc2b2ae35u;
            return z ^ (z >> 16);
        }

        SeedLook lookOf (std::uint32_t seed)
        {
            std::uint32_t s = seed;
            const auto uni = [&s] { return (float) (splitmix (s) >> 8) / 16777216.0f; };
            SeedLook l {};
            l.ax = uni() * 80.0f - 40.0f;
            l.ay = uni() * 80.0f - 40.0f;
            l.cx = uni() * 6.0f - 3.0f;
            l.cy = uni() * 6.0f - 3.0f;
            l.span = 1.0f + 0.08f * (2.0f * uni() - 1.0f);
            l.curl = 1.0f + 0.08f * (2.0f * uni() - 1.0f);
            l.spacing = 1.0f + 0.08f * (2.0f * uni() - 1.0f);
            l.spread = 1.0f + 0.08f * (2.0f * uni() - 1.0f);
            l.hue = 0.04f * (2.0f * uni() - 1.0f);
            l.washA = std::min (4, (int) (uni() * 5.0f));
            l.washB = (l.washA + 1 + std::min (3, (int) (uni() * 4.0f))) % 5;
            return l;
        }

        // Character offsets (fluid, beast, eldritch, deep), applied relative to a neutral mix. Each
        // corner has its own silhouette cue (fluid: drips and hanging arms; beast: horns, spikes and
        // scales; eldritch: eyes, crown and feelers; deep: wide wings, a heavy jaw and satellites)
        // and its own light (below), so two creatures that worked differently look different by s 0.6.
        struct Offset { P param; float d[4]; };
        constexpr Offset c_offsets[] = {
            { P::extraTendrils, { 1.0f,  -0.3f, 0.0f,  0.0f } },
            { P::tendrilReach,  { 0.7f,  0.0f,  0.0f,  0.0f } },
            { P::dripRate,      { 2.0f,  0.0f,  0.0f,  0.0f } },
            { P::extraDrips,    { 0.9f,  0.0f,  0.0f,  0.0f } },
            { P::bleed,         { 0.5f,  -0.2f, 0.0f,  0.0f } },
            { P::gloss,         { 0.15f, 0.0f,  0.0f,  0.0f } },
            { P::pastel,        { 0.4f,  0.0f,  0.0f,  0.0f } },
            { P::hornLength,    { -0.5f, 0.7f,  0.0f,  0.0f } },
            { P::hornTine,      { 0.0f,  0.6f,  0.0f,  0.0f } },
            { P::hornCurl,      { 0.4f,  0.0f,  0.0f,  0.0f } },
            { P::inkiness,      { 0.25f, 0.0f,  0.0f,  0.0f } },
            { P::teethLength,   { 0.0f,  0.4f,  0.0f,  0.0f } },
            { P::cheekSpikes,   { 0.0f,  0.9f,  0.0f,  0.0f } },
            { P::scales,        { 0.0f,  0.7f,  0.0f,  0.0f } },
            { P::brow,          { 0.0f,  0.4f,  0.0f,  0.0f } },
            { P::snout,         { 0.0f,  0.4f,  0.0f,  0.0f } },
            { P::glow,          { 0.0f,  0.2f,  0.0f,  0.0f } },
            { P::browEyes,      { 0.0f,  0.0f,  0.5f,  0.0f } },   // extra eyes open earlier
            { P::cheekEyes,     { 0.0f,  0.0f,  0.5f,  0.0f } },
            { P::centreEye,     { 0.0f,  0.0f,  0.45f, 0.0f } },
            { P::crown,         { 0.0f,  0.0f,  0.7f,  0.0f } },
            { P::antennae,      { 0.0f,  0.0f,  0.6f,  0.0f } },
            { P::bioLum,        { 0.0f,  0.0f,  0.8f,  0.0f } },
            { P::eyespots,      { 0.0f,  0.0f,  0.4f,  0.0f } },
            { P::wingSpan,      { 0.0f,  -0.08f, 0.0f, 0.35f } },
            { P::jawMass,       { 0.0f,  0.0f,  0.0f,  0.45f } },
            { P::fingers,       { 0.0f,  0.0f,  0.0f,  0.7f } },
            { P::satellites,    { 0.0f,  0.0f,  0.0f,  0.7f } },
            { P::centralFigure, { 0.0f,  0.0f,  0.0f,  0.5f } },
            { P::redBlack,      { 0.0f,  0.0f,  0.0f,  0.3f } },
            { P::fogFront,      { 0.0f,  0.0f,  0.0f,  0.12f } },
        };

        // Each corner's light: the lit cloud and the underglow lean toward these (linear mix, up to
        // c_cornerTint of the way at a pure corner). Fluid: cold sea-green; beast: ember; eldritch:
        // violet; deep: abyssal blue.
        constexpr Rgb c_cornerLit[4]  = { { 0.330f, 0.470f, 0.480f }, { 0.470f, 0.380f, 0.330f },
                                          { 0.430f, 0.360f, 0.540f }, { 0.250f, 0.320f, 0.450f } };
        constexpr Rgb c_cornerGlow[4] = { { 0.080f, 0.400f, 0.400f }, { 0.520f, 0.170f, 0.040f },
                                          { 0.360f, 0.120f, 0.520f }, { 0.050f, 0.150f, 0.420f } };
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

            setRgb (p, P::cloudR, mixLinear (c_cloud[k], c_cloud[k + 1], w));
            setRgb (p, P::cloudLitR, mixLinear (c_cloudLit[k], c_cloudLit[k + 1], w));
            setRgb (p, P::underglowR, mixLinear (c_underglow[k], c_underglow[k + 1], w));
            setRgb (p, P::redInkR, c_redInk);

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
            {
                const float t = clamp01 ((weights[f] - 0.25f) * (4.0f / 3.0f)) * charScale * c_cornerTint;
                if (t > 0.0f)
                {
                    setRgb (p, P::cloudLitR, mixLinear (getRgb (p, P::cloudLitR), c_cornerLit[f], t));
                    setRgb (p, P::underglowR, mixLinear (getRgb (p, P::underglowR), c_cornerGlow[f], t));
                    setRgb (p, P::cloudR, mixLinear (getRgb (p, P::cloudR), c_cornerLit[f], t * 0.12f));
                }
            }

            // Keep everything in range.
            for (int i : { (int) P::snout, (int) P::jawMass, (int) P::brow, (int) P::eyespots, (int) P::fingers,
                           (int) P::hornLength, (int) P::hornCurl, (int) P::hornTine, (int) P::crown,
                           (int) P::teethLength, (int) P::thirdRow, (int) P::cheekSpikes, (int) P::dryBrush,
                           (int) P::gloss, (int) P::dripRate, (int) P::extraDrips, (int) P::extraTendrils,
                           (int) P::tendrilReach, (int) P::pastel, (int) P::redBlack, (int) P::glow, (int) P::bioLum,
                           (int) P::antennae, (int) P::scales, (int) P::centralFigure, (int) P::satellites,
                           (int) P::inkiness, (int) P::coffeeRing })
                p[(size_t) i] = std::max (0.0f, p[(size_t) i]);

            for (int i : { (int) P::browEyes, (int) P::cheekEyes, (int) P::centreEye, (int) P::extraDrips,
                           (int) P::extraTendrils, (int) P::pastel, (int) P::redBlack, (int) P::bioLum, (int) P::crown,
                           (int) P::inkiness })
                p[(size_t) i] = std::min (1.0f, p[(size_t) i]);

            p[P::fogFront] = std::min (0.45f, std::max (0.12f, p[P::fogFront]));
            p[P::stageOut] = s;
            return p;
        }

        void applySeed (CreatureEvolution::Params& p, std::uint32_t from, std::uint32_t to, float glide)
        {
            const auto a = lookOf (from), b = lookOf (to);
            const float g = smoothstep (0.0f, 1.0f, glide);
            const auto lerp = [g] (float x, float y) { return x + (y - x) * g; };

            p[P::seedAX] = a.ax;
            p[P::seedAY] = a.ay;
            p[P::seedBX] = b.ax;
            p[P::seedBY] = b.ay;
            p[P::seedGlide] = g;
            p[P::cloudX] = lerp (a.cx, b.cx);
            p[P::cloudY] = lerp (a.cy, b.cy);
            p[P::wingSpan] *= lerp (a.span, b.span);
            p[P::hornCurl] *= lerp (a.curl, b.curl);
            p[P::eyeSpacing] *= lerp (a.spacing, b.spacing);
            p[P::crownSpread] *= lerp (a.spread, b.spread);

            const float hue = lerp (a.hue, b.hue);
            for (int c : { (int) P::cloudR, (int) P::cloudLitR, (int) P::underglowR })
                setRgb (p, c, hueShift (getRgb (p, c), hue));

            setRgb (p, P::washAR, mixLinear (c_washes[a.washA], c_washes[b.washA], g));
            setRgb (p, P::washBR, mixLinear (c_washes[a.washB], c_washes[b.washB], g));
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
            m_seedTo = m_queuedSeed;
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
        const std::array<float, 4> neutral { 0.25f, 0.25f, 0.25f, 0.25f };
        auto p = blend (pinned ? m_pinnedStage : m_stage, pinned ? neutral : getCharacterWeights());
        applySeed (p, m_seedFrom, m_seedTo, m_seedGlide);

        const float energy = pinned ? 1.0f : m_energy;
        p[P::formBase] = p[P::formFloor] + (p[P::formCeiling] - p[P::formFloor]) * smoothstep (0.0f, 1.0f, energy);
        p[P::energyOut] = energy;

        const float g = getEggAmount();
        p[P::egg] = g;
        // The logo is drawn in the same inks: the red pulls toward the cherry's crimson.
        setRgb (p, P::redInkR, mixLinear (getRgb (p, P::redInkR), c_logoRed, g));
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
            m_queuedSeed = newSeed;
            m_hasQueuedSeed = true;
        }
        else
        {
            m_seedFrom = m_seedTo;
            m_seedTo = newSeed;
            m_seedGlide = 0.0f;
        }
    }

    void CreatureEvolution::setSeed (std::uint32_t seed)
    {
        m_seedFrom = m_seedTo = seed;
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
        fast = clamp01 (fast);
        formation = clamp01 (formation);
    }

    float CreatureFormation::fogOpacity (float stageFog, float fast, float sleep)
    {
        const float f = finite (stageFog, 0.0f, 1.0f) * (1.0f - 0.5f * finite (fast, 0.0f, 1.0f))
                      * (0.7f + 0.3f * finite (sleep, 0.0f, 1.0f) * 1.6f);
        return std::min (0.45f, std::max (0.12f, f));
    }

    CreatureUniforms CreatureUniforms::pack (const CreatureEvolution::Params& p, float formation, float fogA,
                                             float formationFast, float bloomPhase)
    {
        CreatureUniforms u;
        const auto v4 = [&p] (P a, P b, P c, P d) { return Vec4 { p[(size_t) a], p[(size_t) b], p[(size_t) c], p[(size_t) d] }; };
        const auto v3 = [&p] (P a) { return Vec3 { p[(size_t) a], p[(size_t) a + 1], p[(size_t) a + 2] }; };
        u.head = v4 (P::cranScale, P::snout, P::jawMass, P::brow);
        u.wing = v4 (P::wingSpan, P::wingLift, P::eyespots, P::fingers);
        u.horn = v4 (P::hornLength, P::hornCurl, P::hornTine, P::crown);
        u.eyes = v4 (P::browEyes, P::cheekEyes, P::centreEye, P::eyeSize);
        u.maw = v4 (P::mawWidth, P::teethLength, P::thirdRow, P::cheekSpikes);
        u.ink = v4 (P::solidity, P::bleed, P::dryBrush, P::gloss);
        u.flow = v4 (P::dripRate, P::extraDrips, P::extraTendrils, P::tendrilReach);
        u.pal = v4 (P::pastel, P::redBlack, P::glow, P::bioLum);
        u.misc = v4 (P::antennae, P::scales, P::centralFigure, P::satellites);
        u.form = { formation, fogA, formationFast, p[P::coffeeRing] };
        u.cloud = { p[P::inkiness], p[P::eyeSpacing], p[P::crownSpread], bloomPhase };
        u.seed = { p[P::seedAX], p[P::seedAY], p[P::cloudX], p[P::cloudY] };
        u.seedB = { p[P::seedBX], p[P::seedBY], p[P::seedGlide], 0.0f };
        u.colCloud = v3 (P::cloudR);
        u.colCloudLit = v3 (P::cloudLitR);
        u.colUnderglow = v3 (P::underglowR);
        u.colRedInk = v3 (P::redInkR);
        u.colWashA = v3 (P::washAR);
        u.colWashB = v3 (P::washBR);
        u.egg = p[P::egg];
        return u;
    }
}

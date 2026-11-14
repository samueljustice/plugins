#include "CreatureAnatomy.h"

#include "CreatureFlow.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        using A = CreatureAnatomy;

        constexpr float c_pi = 3.14159265f;
        constexpr float c_far = 5.0f;              // where an unused primitive is parked
        constexpr float c_maxY = 0.42f, c_maxX = 1.25f;
        constexpr int c_tries = 192;
        // a blot is a sprawl, not a head: the round mass on the fold holds no more than this share
        // of the ink, and the ink lies at least this far from the fold on the whole
        constexpr float c_maxDome = 0.15f, c_minSpread = 0.26f;

        float clamp01 (float v) { return std::isfinite (v) ? std::min (1.0f, std::max (0.0f, v)) : 0.0f; }
        float clampf (float v, float lo, float hi) { return std::min (hi, std::max (lo, v)); }
        float smooth (float a, float b, float x) { const float t = clamp01 ((x - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
        float lerp (float a, float b, float t) { return a + (b - a) * t; }

        std::uint32_t splitmix (std::uint32_t& state)
        {
            std::uint32_t z = (state += 0x9e3779b9u);
            z = (z ^ (z >> 16)) * 0x85ebca6bu;
            z = (z ^ (z >> 13)) * 0xc2b2ae35u;
            return z ^ (z >> 16);
        }

        struct Rng
        {
            std::uint32_t s;
            float uni() { return (float) (splitmix (s) >> 8) / 16777216.0f; }
            float rr (float a, float b) { return a + (b - a) * uni(); }
            bool chance (float p) { return uni() < p; }
            int below (int n) { return std::min (n - 1, (int) (uni() * (float) n)); }
            float sign() { return uni() < 0.5f ? -1.0f : 1.0f; }
        };

        float smin (float a, float b, float k) { const float h = std::max (k - std::abs (a - b), 0.0f) / k; return std::min (a, b) - h * h * k * 0.25f; }
        float smax (float a, float b, float k) { return -smin (-a, -b, k); }

        float sdSeg (float px, float py, float ax, float ay, float bx, float by, float ra, float rb)
        {
            const float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
            const float h = clamp01 ((pax * bax + pay * bay) / std::max (bax * bax + bay * bay, 1.0e-6f));
            const float dx = pax - bax * h, dy = pay - bay * h;
            return std::sqrt (dx * dx + dy * dy) - lerp (ra, rb, h);
        }

        //==========================================================================================
        // One candidate for a seed. Every draw comes from the one generator, in a fixed order.
        A::Genome candidate (std::uint32_t seed, std::uint32_t state)
        {
            A::Genome g;
            g.seed = seed;
            Rng r { state };
            for (int i = 0; i < 4; ++i)
                r.uni();

            const auto addLobe = [&g] (float x, float y, float rx, float ry, float rot, float gate, bool core) -> int
            {
                if (g.numLobes >= A::maxLobes)
                    return -1;
                auto& l = g.lobes[(size_t) g.numLobes];
                l.x = std::max (0.0f, x);
                l.y = y;
                l.rx = rx;
                l.ry = ry;
                l.rot = rot;
                l.gate = gate;
                l.core = core;
                return g.numLobes++;
            };

            g.look.ox = r.rr (-0.15f, 0.15f);
            g.look.oy = r.rr (-0.15f, 0.15f);
            const float pick = r.uni();
            g.build = pick < 0.30f ? A::Build::single : pick < 0.52f ? A::Build::split : pick < 0.78f ? A::Build::stacked : A::Build::hollow;
            const float cy = r.rr (-0.04f, 0.06f);
            g.centreY = cy;

            // ---- the trunk
            int anchor = 0;
            switch (g.build)
            {
                case A::Build::single:
                {
                    // (a modest mass: the blot's weight is in what is chained out from it. A large round
                    // trunk with the rest hung on it is a head.)
                    const float rx = r.rr (0.11f, 0.17f), ry = r.rr (0.10f, 0.16f);
                    addLobe (0.0f, cy, rx, ry, 0.0f, 0.0f, true);
                    g.mawY = cy + r.rr (-0.35f, 0.10f) * ry;
                    break;
                }
                case A::Build::split:
                {
                    // two lobes side by side that meet on the fold, with a bridge on it above them. (They
                    // overlap there: a pale seam left down the fold between two dark masses is a nose
                    // between two sockets.)
                    const float ry = r.rr (0.18f, 0.28f), x0 = r.rr (0.13f, 0.19f), rx = x0 + r.rr (0.012f, 0.030f);
                    addLobe (x0, cy - 0.03f, rx, ry, r.rr (-0.25f, 0.25f), 0.0f, true);
                    const float br = r.rr (0.07f, 0.10f), by = cy - 0.03f + ry * r.rr (0.45f, 0.75f);
                    addLobe (0.0f, by, br * 1.15f, br, 0.0f, 0.0f, true);
                    g.mawY = by + r.rr (-0.3f, 0.2f) * br;
                    break;
                }
                case A::Build::stacked:
                {
                    const float r1x = r.rr (0.14f, 0.24f), r1y = r.rr (0.11f, 0.17f), y1 = cy + r.rr (0.04f, 0.10f);
                    const float r2x = r.rr (0.13f, 0.24f), r2y = r.rr (0.11f, 0.18f), y2 = y1 - (r1y + r2y) * r.rr (0.55f, 0.75f);
                    addLobe (0.0f, y1, r1x, r1y, 0.0f, 0.0f, true);
                    addLobe (0.0f, y2, r2x, r2y, 0.0f, 0.0f, true);
                    const float r3 = r.rr (0.06f, 0.10f), y3 = y1 + (r1y + r3) * r.rr (0.6f, 0.8f);
                    if (r.chance (0.5f) && y3 + r3 < 0.38f)
                        addLobe (0.0f, y3, r3 * r.rr (0.9f, 1.4f), r3, 0.0f, 0.0f, true);
                    g.mawY = y2 + r.rr (-0.2f, 0.5f) * r2y;
                    anchor = r1x > r2x ? 0 : 1;
                    break;
                }
                case A::Build::hollow:
                {
                    // a ring of lobes round a bay on the fold; the bay is open at one end, so the
                    // cloud it lets in is never an enclosed hole
                    const float R = r.rr (0.16f, 0.21f), lr = R * r.rr (0.44f, 0.54f);
                    // (always open downwards, an arch: open upwards, the bay is a pale column on the fold
                    // between two risen masses and over the rift, which is a brow, a nose and a mouth.
                    // The draw is still made, so a seed's later draws stay where they were.)
                    r.chance (0.5f);
                    const bool up = false;
                    const float sgn = up ? -1.0f : 1.0f;   // the closed end is below (the ring opens upwards) or above
                    for (const float deg : { 0.0f, 65.0f, 125.0f })
                    {
                        const float a = deg * c_pi / 180.0f;
                        const float x = R * std::sin (a), y = cy + sgn * R * std::cos (a);
                        // along the ring: the long radius lies on the tangent
                        const float tangent = std::atan2 (-sgn * std::sin (a), std::cos (a));
                        // (the mass at the closed end is the heaviest: the rift is in it, and with the jaw
                        // hung open a slight one is torn through, the blot left in two halves)
                        const float heavy = deg == 0.0f ? 1.35f : 1.0f;
                        addLobe (x, y, R * 0.62f * heavy, lr * heavy, tangent, 0.0f, true);
                    }
                    auto& b = g.bays[(size_t) g.numBays++];
                    b.x = 0.0f;
                    b.y = cy;
                    b.r = R * 0.27f;
                    b.angle = up ? 0.5f * c_pi : -0.5f * c_pi;
                    b.gate = 0.0f;
                    g.mawY = cy + sgn * R + r.rr (-0.2f, 0.2f) * lr;
                    anchor = 1;
                    break;
                }
            }
            const int numCore = g.numLobes;

            // ---- lobes chained out from the trunk, each overlapping the one before
            float lastAngle = 0.0f;
            int outer = anchor;
            {
                const int n = std::min (A::maxLobes - 1 - g.numLobes, 3 + r.below (2));
                const auto& an = g.lobes[(size_t) anchor];
                float a = r.rr (-0.6f, 0.7f), px = an.x, py = an.y, pr = 0.5f * (an.rx + an.ry);
                const float gates[4] = { 0.0f, 0.20f, 0.46f, 0.72f };
                for (int i = 0; i < n; ++i)
                {
                    const float rn = r.rr (0.09f, 0.16f) * (1.0f - 0.13f * (float) i);
                    const bool branch = i == 2 && r.chance (0.45f);
                    if (branch)   // a second arm from the trunk, the other way
                    {
                        px = an.x;
                        py = an.y;
                        pr = 0.5f * (an.rx + an.ry);
                        a = -a + r.rr (-0.3f, 0.3f);
                    }
                    const float step = (pr + rn) * r.rr (0.62f, 0.82f);
                    const float rx = rn * r.rr (1.1f, 1.9f), ry = rn * r.rr (0.65f, 1.0f);
                    const float x = clampf (px + std::cos (a) * step, rn * 0.7f, 1.05f);
                    float y = clampf (py + std::sin (a) * step, -0.38f + rn * 1.3f, 0.38f - rn * 1.3f);
                    // (a mass close beside the fold above the rift, mirrored, is half of a face layout:
                    // it is grown below the rift's level instead where there is room)
                    if (x < 0.27f && y > g.mawY + 0.03f)
                    {
                        const float lower = clampf (py - std::abs (std::sin (a)) * step, -0.38f + rn * 1.3f, 0.38f - rn * 1.3f);
                        if (lower <= g.mawY + 0.03f)
                        {
                            y = lower;
                            a = -std::abs (a);
                        }
                    }
                    // (the last of the chain always arrives late: every blot still gains a mass past stage 0.7)
                    const int idx = addLobe (x, y, rx, ry, a + r.rr (-0.5f, 0.5f), i == n - 1 ? gates[3] : gates[i], false);
                    if (idx >= 0 && ! branch)
                    {
                        outer = idx;
                        lastAngle = a;
                    }
                    px = x;
                    py = y;
                    pr = rn;
                    a += r.rr (-0.6f, 0.6f);
                }
            }

            // ---- limbs: two tapered segments, always bent, rooted inside a lobe
            const auto addLimb = [&g] () -> A::Limb*
            {
                return g.numLimbs < A::maxLimbs ? &g.limbs[(size_t) g.numLimbs++] : nullptr;
            };
            const bool satellite = r.chance (0.4f);
            const int numPrimary = 3 + (r.chance (0.5f) ? 1 : 0);
            {
                const float gates[4] = { 0.0f, 0.08f, 0.20f, 0.30f };
                for (int i = 0; i < numPrimary; ++i)
                {
                    // rooted in a lobe that is there by then; the outer lobes are likelier
                    int root = 0;
                    for (int k = 0; k < 6; ++k)
                    {
                        root = r.below (g.numLobes);
                        if (g.lobes[(size_t) root].gate <= gates[i] + 0.01f && (root >= numCore || k >= 2))
                            break;
                        root = anchor;
                    }
                    const auto& lb = g.lobes[(size_t) root];
                    float base = lb.x > 0.05f ? std::atan2 (lb.y - cy, lb.x) : r.rr (0.35f, 1.2f) * r.sign();
                    const float ang = clampf (base + r.rr (-0.9f, 0.9f), -1.45f, 1.45f);
                    const float len = r.rr (0.30f, 0.62f), bend = r.rr (0.4f, 1.1f) * r.sign();
                    if (auto* l = addLimb())
                    {
                        l->rootX = lb.x + 0.3f * lb.rx * std::cos (ang) * (lb.x > 0.05f ? 1.0f : 0.5f);
                        l->rootY = lb.y + 0.3f * lb.ry * std::sin (ang);
                        l->d1x = 0.5f * len * std::cos (ang);
                        l->d1y = 0.5f * len * std::sin (ang);
                        l->d2x = 0.6f * len * std::cos (ang + bend);
                        l->d2y = 0.6f * len * std::sin (ang + bend);
                        l->r0 = r.rr (0.040f, 0.075f);
                        l->r1 = r.rr (0.006f, 0.016f);
                        l->gate = gates[i];
                    }
                }
                // later: limbs that branch from a limb's bend
                const int room = A::maxLimbs - g.numLimbs - (satellite ? 1 : 0);
                const int numSecondary = std::min (room, 2 + r.below (2));
                const float gates2[3] = { 0.42f, 0.88f, 0.64f };
                for (int i = 0; i < numSecondary; ++i)
                {
                    const int parent = r.below (numPrimary);
                    const auto& pl = g.limbs[(size_t) parent];
                    const float ang = std::atan2 (pl.d1y, pl.d1x) + r.rr (0.6f, 1.3f) * r.sign();
                    const float len = r.rr (0.15f, 0.30f), bend = r.rr (0.4f, 1.0f) * r.sign();
                    if (auto* l = addLimb())
                    {
                        l->parent = parent;
                        l->d1x = 0.5f * len * std::cos (ang);
                        l->d1y = 0.5f * len * std::sin (ang);
                        l->d2x = 0.6f * len * std::cos (ang + bend);
                        l->d2y = 0.6f * len * std::sin (ang + bend);
                        l->r0 = r.rr (0.020f, 0.035f);
                        l->r1 = r.rr (0.004f, 0.010f);
                        l->gate = gates2[i];
                    }
                }
            }

            // ---- a satellite, joined to the outermost lobe by a thread
            if (satellite && g.numLobes < A::maxLobes && g.numLimbs < A::maxLimbs)
            {
                const auto& o = g.lobes[(size_t) outer];
                const float a = lastAngle + r.rr (-0.7f, 0.7f), sr = r.rr (0.04f, 0.07f);
                const float dist = 0.5f * (o.rx + o.ry) + sr + r.rr (0.10f, 0.20f);
                const float sx = clampf (o.x + std::cos (a) * dist, sr + 0.05f, 1.1f), sy = clampf (o.y + std::sin (a) * dist, -0.34f, 0.34f);
                addLobe (sx, sy, sr * r.rr (1.0f, 1.5f), sr, a, 0.72f, false);
                if (auto* l = addLimb())
                {
                    const float bow = r.rr (-0.04f, 0.04f);
                    l->rootX = o.x;
                    l->rootY = o.y;
                    l->d1x = 0.5f * (sx - o.x) - bow * std::sin (a);
                    l->d1y = 0.5f * (sy - o.y) + bow * std::cos (a);
                    l->d2x = (sx - o.x) - l->d1x;
                    l->d2y = (sy - o.y) - l->d1y;
                    l->r0 = l->r1 = r.rr (0.010f, 0.016f);
                    l->gate = 0.70f;
                    l->thread = true;
                }
            }

            // ---- the rift and the bays
            g.mawHalf = r.rr (0.05f, 0.085f);
            g.strands = r.uni();
            g.look.strandGap = r.rr (0.45f, 0.8f);
            {
                const int want = r.below (3);
                const float gates[2] = { 0.20f, 0.60f };
                for (int i = 0; i < want && g.numBays < A::maxBays && g.numLobes > numCore; ++i)
                {
                    const int li = numCore + r.below (g.numLobes - numCore);
                    const auto& lb = g.lobes[(size_t) li];
                    const float out = std::atan2 (lb.y - cy, lb.x);
                    const float x = lb.x + r.rr (-0.3f, 0.3f) * lb.rx, y = lb.y + r.rr (-0.3f, 0.3f) * lb.ry;
                    const float angle = out + r.rr (-0.8f, 0.8f), rad = r.rr (0.02f, 0.04f);
                    // never a pair of bays above the rift near the fold: mirrored, they would be two eyes
                    if (y > g.mawY - 0.10f && x < 0.60f)
                        continue;
                    auto& b = g.bays[(size_t) g.numBays++];
                    b.x = x;
                    b.y = y;
                    b.r = rad;
                    b.angle = angle;
                    b.gate = gates[i];
                }
            }

            // ---- the temperament
            {
                const float t = r.uni();
                g.look.carry = 0.7f + 1.7f * t * t;           // most are compact; some are carried far, into ribbons
                g.look.feather = r.rr (0.65f, 1.45f);
                g.look.ribbon = r.uni();
                g.look.density = r.rr (0.85f, 1.25f);
                g.look.bank = r.rr (0.55f, 1.35f);
                // some rifts are a dark tear and never lit (about one in five); the others' light is strong
                // enough to be seen (the draws as they always were, so a blot's shape does not change)
                if (r.chance (0.3f))
                {
                    std::uint32_t h = seed * 0x2c1b3c6du + 0x297a2d39u;
                    g.look.glow = (float) (splitmix (h) >> 8) / 16777216.0f < 0.27f ? 0.6f : 0.0f;
                }
                else
                    g.look.glow = 0.55f + 0.45f * (r.rr (0.3f, 1.0f) - 0.3f) / 0.7f;
                g.look.flow = r.rr (0.8f, 1.3f);
            }
            return g;
        }

        // the lobe weight for a gate at a stage
        float gateWeight (float gate, float stage)
        {
            return gate <= 0.001f ? 1.0f : smooth (gate - 0.06f, gate + 0.06f, stage);
        }

        //==========================================================================================
        // The mask on a fixed grid of the half plane, and what the rules read from it.
        constexpr int c_nx = 80, c_ny = 60;
        constexpr float c_gridW = 1.36f;

        struct Grid
        {
            std::array<std::uint8_t, (size_t) (c_nx * c_ny)> ink {};
            static float x (int i) { return ((float) i + 0.5f) * c_gridW / (float) c_nx; }
            static float y (int j) { return ((float) j + 0.5f) / (float) c_ny - 0.5f; }
            std::uint8_t& at (int i, int j) { return ink[(size_t) (j * c_nx + i)]; }
            std::uint8_t at (int i, int j) const { return ink[(size_t) (j * c_nx + i)]; }
        };

        bool faceLayout (const A::Pose& p)
        {
            // a head: a pair of limbs close beside the fold that rise from the body to above the top
            // of the mass on the fold, ears or horns over a single round mass
            {
                float coreTop = -9.0f;
                for (int i = 0; i < A::maxLobes; ++i)
                {
                    const auto& b = p.blob[(size_t) i];
                    if (p.blobR[(size_t) i][2] >= 0.5f && b[0] <= 0.06f)
                        coreTop = std::max (coreTop, b[1] + std::max (b[2], b[3]) * 0.8f);
                }
                for (int i = 0; i < A::maxLimbs; ++i)
                {
                    const auto& a = p.limbA[(size_t) i];
                    const auto& b = p.limbB[(size_t) i];
                    if (b[2] <= 0.004f)
                        continue;
                    if (b[0] < 0.32f && b[1] > coreTop - 0.02f && b[1] - a[1] > 0.07f && b[1] > p.maw[0] + 0.10f)
                        return true;
                }
            }
            // the largest lobe above the rift is one of a mirrored pair near the fold, with a lobe on
            // the fold between the two
            int big = -1;
            float bigArea = 0.0f;
            for (int i = 0; i < A::maxLobes; ++i)
            {
                const auto& b = p.blob[(size_t) i];
                const float w = p.blobR[(size_t) i][2];
                if (w < 0.5f || b[1] < p.maw[0] + 0.03f)
                    continue;
                const float area = b[2] * b[3] * w * w * (b[0] > 0.04f ? 1.0f : 0.5f);   // (a lobe on the fold is one lobe, not a pair)
                if (area > bigArea)
                {
                    bigArea = area;
                    big = i;
                }
            }
            if (big < 0)
                return false;
            const auto& L = p.blob[(size_t) big];
            if (L[0] <= 0.04f || L[0] >= 0.25f)
                return false;
            for (int i = 0; i < A::maxLobes; ++i)
            {
                const auto& b = p.blob[(size_t) i];
                if (i == big || p.blobR[(size_t) i][2] < 0.5f || b[0] > 0.04f)
                    continue;
                if (std::abs (b[1] - L[1]) < std::max (L[3], b[3]) * 0.8f)
                    return true;
            }
            return false;
        }
    }

    //==============================================================================================
    float CreatureAnatomy::opening (float jaw, float stage)
    {
        return std::pow (clamp01 (jaw), 1.4f) * (0.13f + 0.04f * clamp01 (stage));
    }

    float CreatureAnatomy::sdf (const Pose& P, float x, float y, bool ragged)
    {
        const float open = P.maw[2];
        // the weighted jaw: everything under the rift hangs lower (the ink between stretches), and
        // what is above it lifts by a third as much
        const float below = 1.0f - smooth (P.maw[0] - 0.14f, P.maw[0] + 0.03f, y);
        const float px = x, py = y + open * (below - 0.35f);
        float d = 9.0f;
        for (int i = 0; i < maxLobes; ++i)
        {
            const auto& b = P.blob[(size_t) i];
            const auto& r = P.blobR[(size_t) i];
            const float lx0 = px - b[0], ly0 = py - b[1];
            const float lx = r[0] * lx0 + r[1] * ly0, ly = -r[1] * lx0 + r[0] * ly0;
            const float rx = b[2] * r[2] + 1.0e-4f, ry = b[3] * r[2] + 1.0e-4f;
            const float e = (std::sqrt ((lx / rx) * (lx / rx) + (ly / ry) * (ly / ry)) - 1.0f) * std::min (rx, ry) + (1.0f - r[2]) * 0.6f;
            d = smin (d, e, 0.055f);
        }
        for (int i = 0; i < maxLimbs; ++i)
        {
            const auto& a = P.limbA[(size_t) i];
            const auto& b = P.limbB[(size_t) i];
            const float rm = lerp (b[2], b[3], 0.45f);
            const float e = std::min (sdSeg (px, py, a[0], a[1], a[2], a[3], b[2], rm), sdSeg (px, py, a[2], a[3], b[0], b[1], rm, b[3]));
            d = smin (d, e, 0.045f);
        }
        // the rift: a tear on the fold, taller than wide when open, shut when the jaw is
        {
            const float mcx = x, mcy = y - (P.maw[0] - open * 0.15f);
            const float mrx = P.maw[1] * (0.8f + 1.6f * smooth (0.0f, 0.12f, open)), mry = 0.005f + open * 1.25f;
            // (a crack, not an oval: pointed at both ends, its width uneven along its length)
            const float ty = std::min (1.0f, std::max (-1.0f, mcy / mry));
            float uneven = 0.5f;
            if (ragged)
                uneven = clamp01 ((NoiseSampler::shared().sample (0.61f + P.look.ox, y * 1.3f + P.look.oy, 1.0f)[0] - 0.5f) * 1.45f + 0.5f);
            float torn = 0.5f;
            if (ragged)
                torn = clamp01 ((NoiseSampler::shared().sample (x * 4.5f + P.look.oy, y * 4.5f + P.look.ox, 0.5f)[0] - 0.5f) * 1.45f + 0.5f);
            const float rw = mrx * std::pow (1.0f - ty * ty, 1.1f) * (0.35f + 1.5f * uneven) * (0.55f + 0.9f * torn);
            const float rift = std::max (std::abs (mcx) - rw, std::abs (mcy) - mry) + (1.0f - smooth (0.0f, 0.02f, open)) * 0.25f;
            // (one ragged opening with nothing left across it: strands across a tear are a row of teeth)
            d = smax (d, -rift, 0.03f);
        }
        // the bays: tapered cuts that widen towards the outside
        for (int i = 0; i < maxBays; ++i)
        {
            const auto& b = P.bay[(size_t) i];
            const float e = sdSeg (px, py, b[0], b[1], b[0] + 1.5f * std::cos (b[3]), b[1] + 1.5f * std::sin (b[3]), b[2], b[2] * 3.0f);
            d = smax (d, -e, 0.03f);
        }
        return d;
    }

    void CreatureAnatomy::rasterise (const Pose& p, int w, int h, float aspect, std::uint8_t* mask)
    {
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
            {
                const float x = (((float) i + 0.5f) / (float) w - 0.5f) * aspect, y = ((float) j + 0.5f) / (float) h - 0.5f;
                mask[(size_t) j * (size_t) w + (size_t) i] = sdf (p, std::sqrt (x * x + 0.0004f), y) < 0.0f ? 1 : 0;
            }
    }

    CreatureAnatomy::Report CreatureAnatomy::inspect (const Pose& p)
    {
        Report rep;
        Grid g;
        int count = 0;
        for (int j = 0; j < c_ny; ++j)
            for (int i = 0; i < c_nx; ++i)
            {
                // (through the soft fold, as the view draws it: nothing is read closer to the fold than 0.02)
                const bool in = sdf (p, std::sqrt (Grid::x (i) * Grid::x (i) + 0.0004f), Grid::y (j)) < 0.0f;
                g.at (i, j) = in ? 1 : 0;
                count += in ? 1 : 0;
            }
        const float cell = (c_gridW / (float) c_nx) * (1.0f / (float) c_ny);
        rep.area = 2.0f * (float) count * cell;

        for (int i = 0; i < maxLimbs; ++i)
            if (p.limbB[(size_t) i][2] > 0.004f
                && std::hypot (p.limbB[(size_t) i][0] - p.limbA[(size_t) i][0], p.limbB[(size_t) i][1] - p.limbA[(size_t) i][1]) > 0.06f)
                ++rep.limbs;
        rep.faceLayout = faceLayout (p);
        if (count == 0)
            return rep;

        // connected parts (8-connected), the cloud from outside (4-connected), the outline's length
        std::array<int, (size_t) (c_nx * c_ny)> stack {};
        std::array<std::uint8_t, (size_t) (c_nx * c_ny)> seen {};
        int largest = 0;
        for (int start = 0; start < c_nx * c_ny; ++start)
        {
            if (g.ink[(size_t) start] == 0 || seen[(size_t) start] != 0)
                continue;
            int top = 0, n = 0;
            stack[(size_t) top++] = start;
            seen[(size_t) start] = 1;
            while (top > 0)
            {
                const int c = stack[(size_t) --top];
                ++n;
                const int ci = c % c_nx, cj = c / c_nx;
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di)
                    {
                        const int ni = ci + di, nj = cj + dj;
                        if (ni < 0 || nj < 0 || ni >= c_nx || nj >= c_ny)
                            continue;
                        const int k = nj * c_nx + ni;
                        if (g.ink[(size_t) k] != 0 && seen[(size_t) k] == 0)
                        {
                            seen[(size_t) k] = 1;
                            stack[(size_t) top++] = k;
                        }
                    }
            }
            largest = std::max (largest, n);
        }
        rep.largestShare = (float) largest / (float) count;

        {
            seen.fill (0);
            int top = 0;
            const auto push = [&] (int i, int j)
            {
                const int k = j * c_nx + i;
                if (g.ink[(size_t) k] == 0 && seen[(size_t) k] == 0)
                {
                    seen[(size_t) k] = 1;
                    stack[(size_t) top++] = k;
                }
            };
            for (int i = 0; i < c_nx; ++i)
            {
                push (i, 0);
                push (i, c_ny - 1);
            }
            for (int j = 0; j < c_ny; ++j)
                push (c_nx - 1, j);
            while (top > 0)
            {
                const int c = stack[(size_t) --top];
                const int ci = c % c_nx, cj = c / c_nx;
                if (ci > 0) push (ci - 1, cj);
                if (ci < c_nx - 1) push (ci + 1, cj);
                if (cj > 0) push (ci, cj - 1);
                if (cj < c_ny - 1) push (ci, cj + 1);
            }
            // (the rift is the one enclosed opening there may be: its own span is not counted)
            const float riftX = p.maw[1] * 3.0f + 0.03f, riftTop = p.maw[0] + 1.10f * p.maw[2] + 0.03f, riftBottom = p.maw[0] - 1.40f * p.maw[2] - 0.03f;
            for (int k = 0; k < c_nx * c_ny; ++k)
                if (g.ink[(size_t) k] == 0 && seen[(size_t) k] == 0)
                {
                    const float x = Grid::x (k % c_nx), y = Grid::y (k / c_nx);
                    if (! (p.maw[2] > 0.0f && x < riftX && y < riftTop && y > riftBottom))
                        ++rep.enclosed;
                }
        }

        // the largest round mass centred on the fold (how far inside the ink the fold's points are),
        // how far the ink lies from the fold, and what rises beside that mass above its top
        {
            float R = 0.0f, at = 0.0f;
            for (int j = 0; j < c_ny; ++j)
            {
                const float d = -sdf (p, 0.02f, Grid::y (j));
                if (d > R)
                {
                    R = d;
                    at = Grid::y (j);
                }
            }
            rep.dome = rep.area > 1.0e-4f ? c_pi * R * R / rep.area : 0.0f;
            rep.domeY = at;
            double sx = 0.0;
            float lump = 0.0f;
            for (int j = 0; j < c_ny; ++j)
                for (int i = 0; i < c_nx; ++i)
                    if (g.at (i, j) != 0)
                    {
                        sx += (double) Grid::x (i);
                        // (beside the mass, within twice its radius of the fold, above its top)
                        if (Grid::x (i) > 0.6f * R && Grid::x (i) < 2.2f * R + 0.05f)
                            lump = std::max (lump, Grid::y (j) - (at + R));
                    }
            rep.spread = (float) (sx / (double) count);
            rep.topLumps = lump;
        }
        // the thick mass on the fold: the mask worn away by 0.06 (limbs and necks go), what is left
        // that reaches the fold, grown back by as much
        {
            constexpr int r = 4;   // (cells: about 0.065)
            std::array<std::uint8_t, (size_t) (c_nx * c_ny)> worn {}, mark {};
            const auto inkAt = [&g] (int i, int j)
            {
                if (j < 0 || j >= c_ny || i >= c_nx)
                    return false;
                return g.at (i < 0 ? -1 - i : i, j) != 0;   // (the other half is the mirror image)
            };
            for (int j = 0; j < c_ny; ++j)
                for (int i = 0; i < c_nx; ++i)
                {
                    bool all = g.at (i, j) != 0;
                    for (int dj = -r; dj <= r && all; ++dj)
                        for (int di = -r; di <= r && all; ++di)
                            if (di * di + dj * dj <= r * r)
                                all = inkAt (i + di, j + dj);
                    worn[(size_t) (j * c_nx + i)] = all ? 1 : 0;
                }
            int top = 0;
            for (int j = 0; j < c_ny; ++j)
                for (int i = 0; i < 5; ++i)
                    if (worn[(size_t) (j * c_nx + i)] != 0 && mark[(size_t) (j * c_nx + i)] == 0)
                    {
                        mark[(size_t) (j * c_nx + i)] = 1;
                        stack[(size_t) top++] = j * c_nx + i;
                    }
            while (top > 0)
            {
                const int c = stack[(size_t) --top], ci = c % c_nx, cj = c / c_nx;
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di)
                    {
                        const int ni = ci + di, nj = cj + dj;
                        if (ni < 0 || nj < 0 || ni >= c_nx || nj >= c_ny)
                            continue;
                        const int k = nj * c_nx + ni;
                        if (worn[(size_t) k] != 0 && mark[(size_t) k] == 0)
                        {
                            mark[(size_t) k] = 1;
                            stack[(size_t) top++] = k;
                        }
                    }
            }
            int n = 0, iMax = 0, jMin = c_ny, jMax = -1;
            for (int j = 0; j < c_ny; ++j)
                for (int i = 0; i < c_nx; ++i)
                {
                    if (g.at (i, j) == 0)
                        continue;
                    bool near = false;
                    for (int dj = -r; dj <= r && ! near; ++dj)
                        for (int di = -r; di <= r && ! near; ++di)
                        {
                            const int ni = i + di, nj = j + dj;
                            if (di * di + dj * dj <= r * r && ni >= 0 && nj >= 0 && ni < c_nx && nj < c_ny)
                                near = mark[(size_t) (nj * c_nx + ni)] != 0;
                        }
                    if (near)
                    {
                        ++n;
                        iMax = std::max (iMax, i);
                        jMin = std::min (jMin, j);
                        jMax = std::max (jMax, j);
                    }
                }
            rep.core = (float) n / (float) count;
            rep.coreRound = n > 0 ? (float) n / (float) ((iMax + 1) * (jMax - jMin + 1)) : 0.0f;
        }
        // cloud the ink all but encloses, off the fold and above the rift's level: of eight ways
        // out of it, at most two are open within a sixth of a view height (mirrored, a pair of sockets)
        {
            const float reach = 0.16f;
            for (int j = 0; j < c_ny; ++j)
                for (int i = 0; i < c_nx; ++i)
                {
                    if (g.at (i, j) != 0 || Grid::x (i) < 0.06f || Grid::x (i) > 0.75f || Grid::y (j) < p.maw[0] - 0.02f)
                        continue;
                    int shut = 0;
                    for (int k = 0; k < 8; ++k)
                    {
                        const float a = (float) k * 0.785398f, cx = std::cos (a), sy = std::sin (a);
                        bool hit = false;
                        for (float t = 0.02f; t <= reach && ! hit; t += 0.02f)
                        {
                            const float x = std::abs (Grid::x (i) + cx * t), y = Grid::y (j) + sy * t;
                            const int ii = (int) (x / c_gridW * (float) c_nx), jj = (int) ((y + 0.5f) * (float) c_ny);
                            hit = ii >= 0 && jj >= 0 && ii < c_nx && jj < c_ny && g.at (ii, jj) != 0;
                        }
                        shut += hit ? 1 : 0;
                    }
                    rep.sockets += shut >= 6 ? 1 : 0;
                }
        }

        // extent, outline, and the convex hull of both halves (the concave majorant of the row extents)
        std::array<float, (size_t) c_ny> ext {};
        int edges = 0;
        for (int j = 0; j < c_ny; ++j)
            for (int i = 0; i < c_nx; ++i)
            {
                if (g.at (i, j) == 0)
                    continue;
                rep.maxX = std::max (rep.maxX, Grid::x (i) + 0.5f * c_gridW / (float) c_nx);
                rep.maxY = std::max (rep.maxY, std::abs (Grid::y (j)) + 0.5f / (float) c_ny);
                ext[(size_t) j] = Grid::x (i) + 0.5f * c_gridW / (float) c_nx;
                if (i == c_nx - 1 || g.at (i + 1, j) == 0) ++edges;
                if (i > 0 && g.at (i - 1, j) == 0) ++edges;
                if (j == c_ny - 1 || g.at (i, j + 1) == 0) ++edges;
                if (j == 0 || g.at (i, j - 1) == 0) ++edges;
            }
        rep.perimeter = 2.0f * (float) edges * 0.5f * (c_gridW / (float) c_nx + 1.0f / (float) c_ny);
        {
            std::array<int, (size_t) c_ny> hull {};
            int n = 0;
            for (int j = 0; j < c_ny; ++j)
            {
                if (ext[(size_t) j] <= 0.0f)
                    continue;
                while (n >= 2)
                {
                    const int a = hull[(size_t) (n - 2)], b = hull[(size_t) (n - 1)];
                    // b is under the chord from a to j: not on the hull
                    const float cross = (float) (b - a) * (ext[(size_t) j] - ext[(size_t) a]) - (float) (j - a) * (ext[(size_t) b] - ext[(size_t) a]);
                    if (cross >= 0.0f)
                        --n;
                    else
                        break;
                }
                hull[(size_t) n++] = j;
            }
            float half = 0.0f;
            for (int k = 0; k + 1 < n; ++k)
                half += 0.5f * (ext[(size_t) hull[(size_t) k]] + ext[(size_t) hull[(size_t) (k + 1)]]) * (float) (hull[(size_t) (k + 1)] - hull[(size_t) k]) / (float) c_ny;
            if (n == 1)
                half = ext[(size_t) hull[0]] / (float) c_ny;
            rep.negative = half > 1.0e-6f ? clamp01 (1.0f - 0.5f * rep.area / half) : 0.0f;
        }
        return rep;
    }

    //==============================================================================================
    CreatureAnatomy::Pose CreatureAnatomy::evaluate (const Genome& g, float stageIn, const std::array<float, 4>& chars,
                                                     float pitchSt, float size)
    {
        Pose p;
        const float fluid = clamp01 (chars[0]), beast = clamp01 (chars[1]), eld = clamp01 (chars[2]), deep = clamp01 (chars[3]);
        const float stage = clamp01 (stageIn);
        const float gs = stage + 0.25f * eld;                    // eldritch: structure arrives earlier
        const float lenK = (0.62f + 0.38f * stage) * (1.0f + 0.30f * fluid + 0.12f * eld);
        const float heft = 0.75f + 0.5f * stage;                 // the limbs thicken as it grows
        const float pk = clampf (std::isfinite (pitchSt) ? -pitchSt / 12.0f : 0.0f, -1.0f, 2.0f);
        const float wide = 1.0f + 0.05f * pk, tall = 1.0f - 0.04f * pk;
        const float drop = -0.012f * pk - 0.04f * deep;          // shifted down, and deep: a lower, sagging mass
        const float sizeK = 0.95f + 0.12f * clamp01 (size);
        const float cy = g.centreY;
        const auto place = [&] (float x, float y, float& ox, float& oy)
        {
            ox = x * wide;
            oy = cy + (y - cy) * tall + drop;
        };

        for (int i = 0; i < maxLobes; ++i)
        {
            auto& b = p.blob[(size_t) i];
            auto& r = p.blobR[(size_t) i];
            if (i >= g.numLobes)
            {
                b = { c_far, c_far, 0.01f, 0.01f };
                r = { 1.0f, 0.0f, 0.0f, 0.0f };
                continue;
            }
            const auto& l = g.lobes[(size_t) i];
            place (l.x, l.y, b[0], b[1]);
            const float heavy = l.core ? 1.0f + 0.30f * beast : 1.0f;
            const float sag = l.y < cy ? 1.0f + 0.12f * deep : 1.0f;
            // (the masses away from the trunk draw out as it grows: longer, a little leaner)
            const float draw = l.core ? 0.0f : stage - 0.5f;
            b[2] = l.rx * wide * sizeK * heavy * sag * (1.0f + 0.30f * draw);
            b[3] = l.ry * tall * sizeK * heavy * (1.0f - 0.16f * draw);
            r = { std::cos (l.rot), std::sin (l.rot), gateWeight (l.gate, gs), l.core ? 1.0f : 0.0f };
        }

        for (int i = 0; i < maxLimbs; ++i)
        {
            auto& a = p.limbA[(size_t) i];
            auto& b = p.limbB[(size_t) i];
            if (i >= g.numLimbs)
            {
                a = { c_far, c_far, c_far, c_far };
                b = { c_far, c_far, 0.0f, 0.0f };
                continue;
            }
            const auto& l = g.limbs[(size_t) i];
            const float w = gateWeight (l.gate, gs);
            // (a branch keeps reaching out long after it arrives: the late stages' new structure)
            const float reachOn = l.parent >= 0 ? 1.0f + 1.0f * smooth (l.gate + 0.15f, l.gate + 0.45f, gs) : 1.0f;
            const float k = (l.thread ? 1.0f : lenK * reachOn * (1.0f - 0.12f * beast)) * w;
            float rx, ry;
            if (l.parent >= 0 && l.parent < i)
            {
                rx = p.limbA[(size_t) l.parent][2];
                ry = p.limbA[(size_t) l.parent][3];
            }
            else
                place (l.rootX, l.rootY, rx, ry);
            const float len = std::hypot (l.d1x + l.d2x, l.d1y + l.d2y);
            const float droop = l.thread ? 0.0f : (0.30f * fluid + 0.15f * deep) * len;
            const auto keepIn = [] (float& x, float& y)
            {
                x = clampf (x, 0.03f, c_maxX - 0.03f);
                y = clampf (y, -(c_maxY - 0.02f), c_maxY - 0.02f);
            };
            float bx = rx + l.d1x * wide * k, by = ry + (l.d1y * tall - 0.3f * droop) * k;
            keepIn (bx, by);
            float tx = bx + l.d2x * wide * k, ty = by + (l.d2y * tall - 0.7f * droop) * k;
            keepIn (tx, ty);
            const float rw = std::min (1.0f, 1.5f * w);
            a = { rx, ry, bx, by };
            b = { tx, ty, l.r0 * sizeK * rw * (l.thread ? 1.0f : heft), l.r1 * (1.0f - 0.3f * fluid) * rw * (l.thread ? 1.0f : heft) };
        }

        {
            float mx, my;
            place (0.0f, g.mawY, mx, my);
            p.maw = { my, g.mawHalf * (1.0f + 0.45f * beast) * sizeK, 0.0f, g.strands };
        }

        // no head: a pair of limbs close beside the fold, rising over the mass on the fold, reads as
        // ears or horns on a head. Such a limb is swung outwards and down, the more the closer it
        // comes to that (smoothly, so a change of stage or a glide never jumps).
        {
            float coreTop = -9.0f;
            for (int i = 0; i < maxLobes; ++i)
            {
                const auto& b = p.blob[(size_t) i];
                if (p.blobR[(size_t) i][2] >= 0.5f && b[0] <= 0.06f)
                    coreTop = std::max (coreTop, b[1] + std::max (b[2], b[3]) * 0.8f);
            }
            for (int i = 0; i < maxLimbs; ++i)
            {
                auto& a = p.limbA[(size_t) i];
                auto& b = p.limbB[(size_t) i];
                if (b[2] <= 0.004f)
                    continue;
                // (far out too: a pair of limbs raised over the middle mass from its shoulders is a pair of horns)
                const float w = (1.0f - smooth (0.55f, 0.85f, b[0])) * smooth (coreTop - 0.10f, coreTop + 0.02f, b[1])
                              * smooth (0.03f, 0.09f, b[1] - a[1]) * smooth (p.maw[0] + 0.04f, p.maw[0] + 0.12f, b[1]);
                if (w <= 0.0f)
                    continue;
                const float lowTip = std::min (b[1], coreTop - 0.10f), lowBend = std::min (a[3], 0.5f * (a[1] + lowTip));
                b[0] = std::min (c_maxX - 0.03f, b[0] + w * 0.26f);
                b[1] += w * (lowTip - b[1]);
                a[2] = std::min (c_maxX - 0.03f, a[2] + w * 0.13f);
                a[3] += w * (lowBend - a[3]);
            }
        }

        for (int i = 0; i < maxBays; ++i)
        {
            auto& b = p.bay[(size_t) i];
            b = { c_far, c_far, 0.0f, 0.0f };
            if (i >= g.numBays)
                continue;
            const auto& src = g.bays[(size_t) i];
            const float w = gateWeight (src.gate, gs);
            if (w <= 0.001f)
                continue;
            place (src.x, src.y, b[0], b[1]);
            b[2] = src.r * w * (0.7f + 0.5f * eld + 0.9f * stage);   // deeper with the stage
            b[3] = src.angle;
        }

        p.look = g.look;
        p.look.flow *= 1.0f + 0.25f * stage;
        // (late, it is torn further and its edge frays more: the top stages are told from the middle
        // ones at a glance, by ribbons and bays, not by a few more curls)
        p.look.carry *= (1.0f + 0.25f * fluid) * (1.0f + 0.60f * smooth (0.50f, 1.0f, stage));
        p.look.feather *= 1.0f + 0.45f * smooth (0.50f, 1.0f, stage);
        // young, the blot is a dense mass the eddies barely tear; grown, they tear it into ribbons
        p.look.ribbon = std::min (1.0f, p.look.ribbon * (0.55f + 0.45f * smooth (0.0f, 0.45f, stage)) + 0.40f * smooth (0.55f, 1.0f, stage));
        p.centreY = cy + drop;
        return p;
    }

    CreatureAnatomy::Pose CreatureAnatomy::mix (const Pose& a, const Pose& b, float tIn)
    {
        const float t = clamp01 (tIn);
        if (t <= 0.0f) return a;
        if (t >= 1.0f) return b;
        Pose p;
        for (size_t i = 0; i < (size_t) maxLobes; ++i)
        {
            // a lobe that one blot does not have grows in place, where the other has it
            const bool hasA = a.blobR[i][2] > 0.0f, hasB = b.blobR[i][2] > 0.0f;
            const auto& pa = hasA || ! hasB ? a.blob[i] : b.blob[i];
            const auto& pb = hasB || ! hasA ? b.blob[i] : a.blob[i];
            const auto& ra = hasA || ! hasB ? a.blobR[i] : b.blobR[i];
            const auto& rb = hasB || ! hasA ? b.blobR[i] : a.blobR[i];
            for (size_t c = 0; c < 4; ++c)
                p.blob[i][c] = lerp (pa[c], pb[c], t);
            // the turn at an even rate, the short way round (an ellipse is the same turned half round)
            const float turnA = std::atan2 (ra[1], ra[0]);
            float turn = 2.0f * (std::atan2 (rb[1], rb[0]) - turnA);
            while (turn > c_pi) turn -= 2.0f * c_pi;
            while (turn < -c_pi) turn += 2.0f * c_pi;
            const float cs = std::cos (turnA + 0.5f * turn * t), sn = std::sin (turnA + 0.5f * turn * t);
            p.blobR[i] = { cs, sn, lerp (a.blobR[i][2], b.blobR[i][2], t), lerp (a.blobR[i][3], b.blobR[i][3], t) };
        }
        for (size_t i = 0; i < (size_t) maxLimbs; ++i)
        {
            const bool hasA = a.limbB[i][2] > 0.0f, hasB = b.limbB[i][2] > 0.0f;
            auto la = a.limbA[i], lb = b.limbA[i];
            auto ta = a.limbB[i], tb = b.limbB[i];
            // a limb that one blot does not have grows out of the other's root
            if (! hasA && hasB) { la = { lb[0], lb[1], lb[0], lb[1] }; ta = { lb[0], lb[1], 0.0f, 0.0f }; }
            if (! hasB && hasA) { lb = { la[0], la[1], la[0], la[1] }; tb = { la[0], la[1], 0.0f, 0.0f }; }
            for (size_t c = 0; c < 4; ++c)
            {
                p.limbA[i][c] = lerp (la[c], lb[c], t);
                p.limbB[i][c] = lerp (ta[c], tb[c], t);
            }
        }
        for (size_t c = 0; c < 4; ++c)
            p.maw[c] = lerp (a.maw[c], b.maw[c], t);
        for (size_t i = 0; i < (size_t) maxBays; ++i)
        {
            const bool hasA = a.bay[i][2] > 0.0f, hasB = b.bay[i][2] > 0.0f;
            const auto& pa = hasA || ! hasB ? a.bay[i] : b.bay[i];
            const auto& pb = hasB || ! hasA ? b.bay[i] : a.bay[i];
            p.bay[i][0] = lerp (pa[0], pb[0], t);
            p.bay[i][1] = lerp (pa[1], pb[1], t);
            p.bay[i][2] = lerp (a.bay[i][2], b.bay[i][2], t);
            float da = pb[3] - pa[3];
            while (da > c_pi) da -= 2.0f * c_pi;
            while (da < -c_pi) da += 2.0f * c_pi;
            p.bay[i][3] = pa[3] + da * t;
        }
        const auto& la = a.look;
        const auto& lb = b.look;
        p.look.carry = lerp (la.carry, lb.carry, t);
        p.look.feather = lerp (la.feather, lb.feather, t);
        p.look.ribbon = lerp (la.ribbon, lb.ribbon, t);
        p.look.density = lerp (la.density, lb.density, t);
        p.look.bank = lerp (la.bank, lb.bank, t);
        p.look.glow = lerp (la.glow, lb.glow, t);
        p.look.flow = lerp (la.flow, lb.flow, t);
        p.look.strandGap = lerp (la.strandGap, lb.strandGap, t);
        p.look.ox = lerp (la.ox, lb.ox, t);
        p.look.oy = lerp (la.oy, lb.oy, t);
        p.centreY = lerp (a.centreY, b.centreY, t);
        // the glide keeps the blot's weight: half way, where the two blots' masses pass each other
        // and each is still moving to its place, the union of them is far smaller than either blot.
        // The masses and limbs swell by as much as the ink fell short (one measured step, on the
        // coarse grid; the swelling is smooth in t, so the glide never jumps)
        {
            const float want = lerp (inspect (a).area, inspect (b).area, t), now = inspect (p).area;
            if (now > 1.0e-4f && now < want)
            {
                const float k = std::min (1.6f, std::sqrt (want / now));
                for (auto& bl : p.blob)
                {
                    bl[2] *= k;
                    bl[3] *= k;
                }
                for (auto& lb : p.limbB)
                {
                    lb[2] *= k;
                    lb[3] *= k;
                }
            }
        }
        return p;
    }

    //==============================================================================================
    CreatureAnatomy::Genome CreatureAnatomy::generate (std::uint32_t seed)
    {
        Genome g;
        int face = 0, sprawl = 0;
        for (int attempt = 0; attempt < c_tries; ++attempt)
        {
            std::uint32_t state = seed ^ (0x7f4a7c15u * (std::uint32_t) attempt);
            state = splitmix (state);
            g = candidate (seed, state);
            bool ok = true, faceFail = false, sprawlFail = false;
            for (const float stage : { 0.3f, 0.6f, 1.0f })
            {
                auto pose = evaluate (g, stage);
                const auto rep = inspect (pose);
                const float atRift = sdf (pose, 0.02f, pose.maw[0]);
                // (with the jaw wide open the hanging mass must not close a gap into a hole, or tear off)
                pose.maw[2] = opening (1.0f, stage);
                const auto open = inspect (pose);
                if (open.enclosed > 2 || open.largestShare < 0.9f)
                    ok = false;
                // one body, no enclosed hole, inside the view, air in the hull, limbs, ink round the rift
                if (rep.largestShare < 0.97f || rep.enclosed > 2 || rep.maxX > c_maxX || rep.maxY > c_maxY
                    || rep.negative < 0.35f || rep.limbs < 3 || atRift > -0.03f)
                    ok = false;
                if (rep.faceLayout)
                    ok = false, faceFail = true;
                // (grown, it sprawls: one round mass on the fold with the rest hung on it is a head)
                if (stage >= 0.5f && (rep.dome > c_maxDome || rep.spread < c_minSpread))
                    ok = false, sprawlFail = true;
                if (! ok)
                    break;
            }
            g.rerolls = attempt;
            g.faceRerolls = face;
            g.sprawlRerolls = sprawl;
            g.valid = ok;
            if (ok)
                break;
            face += faceFail ? 1 : 0;
            sprawl += sprawlFail ? 1 : 0;
        }
        return g;
    }

    std::uint32_t CreatureAnatomy::combine (std::uint32_t sessionSeed, std::uint32_t presetHash)
    {
        if (presetHash == 0)
            return sessionSeed;
        std::uint32_t s = sessionSeed ^ (presetHash * 0x9e3779b1u);
        s = splitmix (s) ^ presetHash;
        return splitmix (s);
    }

    std::uint32_t CreatureAnatomy::hashName (const char* utf8)
    {
        if (utf8 == nullptr || *utf8 == 0)
            return 0;
        std::uint32_t h = 2166136261u;
        for (const char* c = utf8; *c != 0; ++c)
            h = (h ^ (std::uint32_t) (unsigned char) *c) * 16777619u;
        return h == 0 ? 1u : h;
    }
}

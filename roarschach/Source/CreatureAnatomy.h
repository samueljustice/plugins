#pragma once

#include <array>
#include <cstdint>

namespace roar
{
    /**
     * The creature's anatomy: an ink blot grown from a seed. Pure (no GL, no JUCE, no allocation),
     * so it is stepped on the GL thread, tested on its own and rasterised on the CPU.
     *
     * A seed gives a Genome: a trunk on the fold (one of four builds), lobes chained out from it,
     * bent tapered limbs, a rift on the fold, bays that let the cloud in from outside, and a
     * temperament (how far the flow carries it, how ragged, how dense, how much cloud stands in
     * front, whether the rift is lit). Nothing in it is an eye, a tooth or a face part; a candidate
     * that would read as a face layout, break into pieces, enclose a hole or leave too little
     * negative space is re-rolled (deterministically), so the same seed is the same blot everywhere.
     *
     * evaluate() poses a genome for a stage and the four character leans: the stage switches later
     * lobes and limbs in one at a time (structure, not scale); the characters bend the build. mix()
     * glides between two poses primitive by primitive. sdf() and rasterise() are the C++ twin of
     * the shaders' anatomySdf().
     */
    struct CreatureAnatomy
    {
        static constexpr int maxLobes = 8, maxLimbs = 6, maxBays = 2;

        enum class Build { single, split, stacked, hollow };

        struct Lobe { float x = 5.0f, y = 5.0f, rx = 0.01f, ry = 0.01f, rot = 0.0f, gate = 2.0f; bool core = false; };
        struct Limb
        {
            float rootX = 5.0f, rootY = 5.0f;   // inside a lobe (or, with a parent, taken from the parent's bend)
            float d1x = 0.0f, d1y = 0.0f;       // root to bend
            float d2x = 0.0f, d2y = 0.0f;       // bend to tip
            float r0 = 0.0f, r1 = 0.0f, gate = 2.0f;
            int parent = -1;
            bool thread = false;                // joins a satellite: never lengthened or drooped
        };
        struct Bay { float x = 5.0f, y = 5.0f, r = 0.0f, angle = 0.0f, gate = 2.0f; };

        /** The temperament: how the same anatomy is drawn (each seed and preset has its own). */
        struct Look
        {
            float carry = 1.0f;     // how long the flow carries the shape (short: a dense mass; long: ribbons)
            float feather = 1.0f;   // raggedness of the edge
            float ribbon = 0.5f;    // patchiness of the carry (0 even, 1 torn by eddies)
            float density = 1.0f;   // absorbance of the held ink
            float bank = 1.0f;      // how heavy the cloud in front stands
            float glow = 0.0f;      // the rift's light: 0 a dark tear, 1 lit breath
            float flow = 1.0f;      // the eddies' gain
            float strandGap = 0.6f; // spacing of the strands across the rift, of its half width
            float ox = 0.0f, oy = 0.0f;   // where this blot reads its noise
        };

        struct Genome
        {
            std::uint32_t seed = 0;
            Build build = Build::single;
            int numLobes = 0, numLimbs = 0, numBays = 0;
            std::array<Lobe, maxLobes> lobes {};
            std::array<Limb, maxLimbs> limbs {};
            std::array<Bay, maxBays> bays {};
            float centreY = 0.0f;
            float mawY = 0.0f, mawHalf = 0.06f, strands = 0.5f;
            Look look;
            int rerolls = 0;        // candidates thrown away before this one
            int faceRerolls = 0;    // ... of them for the face rules
            bool valid = false;     // false: no candidate passed (the last one is kept)
        };

        /** The uniforms (see the shader): everything in view heights, the half plane x >= 0. */
        struct Pose
        {
            std::array<std::array<float, 4>, maxLobes> blob {};    // centre xy, radii
            std::array<std::array<float, 4>, maxLobes> blobR {};   // cos, sin, weight, core
            std::array<std::array<float, 4>, maxLimbs> limbA {};   // root xy, bend xy
            std::array<std::array<float, 4>, maxLimbs> limbB {};   // tip xy, root radius, tip radius
            std::array<float, 4> maw {};                           // height on the fold, half width, opening, strand amount
            std::array<std::array<float, 4>, maxBays> bay {};      // centre xy, radius, direction (radians)
            Look look;
            float centreY = 0.0f;
        };

        /** What a seed is (the validated genome). A few milliseconds: call it when the seed changes. */
        static Genome generate (std::uint32_t seed);

        /** characters: the fluid, beast, eldritch and deep leans (0 neutral, up to 1). */
        static Pose evaluate (const Genome&, float stage, const std::array<float, 4>& characters = {},
                              float pitchSt = 0.0f, float size = 0.35f);

        /** The glide between two blots, primitive by primitive. */
        static Pose mix (const Pose& a, const Pose& b, float t);

        /** The opening of the rift for a jaw position (0..1) at a stage: jaw^1.4, taller with the stage. */
        static float opening (float jaw, float stage);

        /** Signed distance to the anatomy at a point of the half plane (x >= 0); negative inside.
            ragged: the rift's outline is eroded by the noise, as the shader does. */
        static float sdf (const Pose&, float x, float y, bool ragged = false);

        /** The ink mask (1 inside) over a whole view of w x h cells, aspect = width / height, rows
            bottom up. mask must hold w * h bytes. */
        static void rasterise (const Pose&, int w, int h, float aspect, std::uint8_t* mask);

        /** What the rules measure on a pose (the mask on a fixed grid of the half plane). */
        struct Report
        {
            float area = 0.0f;            // ink, both halves, view heights squared
            float largestShare = 1.0f;    // share of the ink in the largest connected part
            int enclosed = 0;             // cells of cloud the ink encloses
            float maxX = 0.0f, maxY = 0.0f;   // extent from the centre
            float negative = 0.0f;        // share of the convex hull that is not ink
            int limbs = 0;                // limbs switched in
            bool faceLayout = false;      // two large lobes side by side above the rift, one between them
            float perimeter = 0.0f;       // both halves, view heights
        };
        static Report inspect (const Pose&);

        /** The seed a blot is grown from: the session's seed and the preset's hash (0: none). */
        static std::uint32_t combine (std::uint32_t sessionSeed, std::uint32_t presetHash);

        /** A stable hash of a preset's name (0 for an empty name). */
        static std::uint32_t hashName (const char* utf8);
    };
}

#pragma once

#include <array>
#include <vector>

namespace roar
{
    /**
     * The baked noise texture as the GPU reads it: its mip chain and a trilinear, wrapping lookup
     * (value, d/du, d/dv, ridged; the gradient per unit of texture coordinate). Pure: no GL. Built
     * once per process from NoiseTexture::shared() (call shared() first on the message thread).
     */
    class NoiseSampler
    {
    public:
        static const NoiseSampler& shared();

        /** value 0..1, gradient x and y (per unit uv), ridged 0..1, at an explicit level of detail. */
        std::array<float, 4> sample (float u, float v, float lod) const;

    private:
        NoiseSampler();
        std::array<float, 4> level (int l, float u, float v) const;

        struct Level { int size = 0; std::vector<float> rgba; };
        std::vector<Level> m_levels;
    };

    /**
     * The creature's flow: the C++ twin of the shaders' flow(). A stream function that is odd across
     * the fold (x = 0), so the fold is a streamline and nothing crosses it: large eddies from the
     * noise and its mirror image, smaller eddies that fade out at the fold, and one mirrored vortex
     * pair bound to the body. View heights per second, never faster than c_ceiling.
     */
    struct CreatureFlow
    {
        using Vec2 = std::array<float, 2>;

        struct Params
        {
            float gain = 0.8f;            // 0.8 at rest, up to 3.6 in a roar
            float smallGain = 1.0f;       // the smaller eddies' share
            float vortex = 0.0f;          // the bound pair's peak speed, view heights per second
            float vortexRadius = 0.22f;
            float vortexX = 0.32f, vortexY = 0.0f;
            float o1x = 0.0f, o1y = 0.0f; // where the large eddies are read (drifts with time)
            float o2x = 0.0f, o2y = 0.0f; // ... the small ones
        };

        static constexpr float c_ceiling = 0.6f;
        static constexpr float c_scale = 0.060f;      // noise gradient to view heights per second at gain 1
        static constexpr float c_restGain = 0.8f;

        /** The gain for a level and a roar heat (each 0..1). */
        static float gainFor (float level, float roarHeat);

        /** The velocity at a point of the whole view (x either side of the fold). */
        static Vec2 velocity (const Params&, float x, float y);

        /** Where the ink at (x, y), x >= 0, was tau seconds ago: four backward steps, as the shader takes. */
        static Vec2 backtrace (const Params&, float x, float y, float tau);
    };
}

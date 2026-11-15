#pragma once

#include <cstdint>
#include <vector>

namespace roar
{
    /**
     * A tileable noise texture for the creature view's flow-moved detail, baked once per process
     * (pure: no JUCE, no GL). R = 4-octave value-noise fbm on a periodic lattice, G and B = its
     * analytic gradient (x, y) mapped to 0..1, A = a ridged sum of the two finest octaves.
     * Rows run bottom to top (GL order), and every channel wraps seamlessly at the edges.
     */
    struct NoiseTexture
    {
        int size = 0;
        std::vector<float> value, gradX, gradY, ridged;   // size * size each, floats before packing
        std::vector<std::uint8_t> rgba;                   // size * size * 4, packed

        /** Gradient encoding: G = 0.5 + gradX * c_gradScale, with the gradient in R per texel. */
        static constexpr float c_gradScale = 8.0f;

        static NoiseTexture bake (int size = 256, std::uint32_t seed = 0x5ea1u);

        /** The one shared by every view, baked on first use (call it first on the message thread). */
        static const NoiseTexture& shared();
    };
}

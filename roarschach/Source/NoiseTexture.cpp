#include "NoiseTexture.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        float latticeHash (int x, int y, int octave, std::uint32_t seed)
        {
            std::uint32_t h = seed ^ (std::uint32_t) (x * 374761393) ^ (std::uint32_t) (y * 668265263) ^ (std::uint32_t) (octave * 2246822519u);
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= h >> 16;
            return (float) (h & 0xffffffu) / (float) 0xffffff;
        }

        // Value noise on a lattice of `period` cells that wraps; returns value and d/du, d/dv (per cell).
        void periodicNoise (float u, float v, int period, int octave, std::uint32_t seed, float& n, float& du, float& dv)
        {
            const int i0 = (int) std::floor (u), j0 = (int) std::floor (v);
            const float fx = u - (float) i0, fy = v - (float) j0;
            const auto wrap = [period] (int i) { return ((i % period) + period) % period; };
            const float a = latticeHash (wrap (i0), wrap (j0), octave, seed);
            const float b = latticeHash (wrap (i0 + 1), wrap (j0), octave, seed);
            const float c = latticeHash (wrap (i0), wrap (j0 + 1), octave, seed);
            const float d = latticeHash (wrap (i0 + 1), wrap (j0 + 1), octave, seed);
            const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
            const float dsx = 6.0f * fx * (1.0f - fx), dsy = 6.0f * fy * (1.0f - fy);
            const float k1 = b - a, k2 = c - a, k4 = a - b - c + d;
            n = a + k1 * sx + k2 * sy + k4 * sx * sy;
            du = dsx * (k1 + k4 * sy);
            dv = dsy * (k2 + k4 * sx);
        }
    }

    NoiseTexture NoiseTexture::bake (int size, std::uint32_t seed)
    {
        NoiseTexture t;
        t.size = std::max (8, size);
        const size_t n = (size_t) t.size * (size_t) t.size;
        t.value.assign (n, 0.0f);
        t.gradX.assign (n, 0.0f);
        t.gradY.assign (n, 0.0f);
        t.ridged.assign (n, 0.0f);
        t.rgba.assign (n * 4, 0);

        constexpr int octaves = 4;
        constexpr int basePeriod = 4;   // cells across the tile at the coarsest octave
        float norm = 0.0f;
        for (int o = 0; o < octaves; ++o)
            norm += std::pow (0.5f, (float) o);

        for (int y = 0; y < t.size; ++y)
            for (int x = 0; x < t.size; ++x)
            {
                float v = 0.0f, gx = 0.0f, gy = 0.0f, r = 0.0f, amp = 1.0f;
                for (int o = 0; o < octaves; ++o)
                {
                    const int period = basePeriod << o;
                    const float cellsPerTexel = (float) period / (float) t.size;
                    // a per-octave offset (whole cells, so the tiling holds) decorrelates the octaves
                    const float u = ((float) x + 0.5f) * cellsPerTexel + (float) (o * 3);
                    const float w = ((float) y + 0.5f) * cellsPerTexel + (float) (o * 5);
                    float nv, du, dv;
                    periodicNoise (u, w, period, o, seed, nv, du, dv);
                    v += amp * nv;
                    gx += amp * du * cellsPerTexel;
                    gy += amp * dv * cellsPerTexel;
                    if (o >= 2)
                        r += amp * (1.0f - std::abs (2.0f * nv - 1.0f));
                    amp *= 0.5f;
                }
                const size_t i = (size_t) y * (size_t) t.size + (size_t) x;
                t.value[i] = v / norm;
                t.gradX[i] = gx / norm;
                t.gradY[i] = gy / norm;
                t.ridged[i] = r / (0.25f + 0.125f);
            }

        const auto pack = [] (float f) { return (std::uint8_t) std::lround (std::min (1.0f, std::max (0.0f, f)) * 255.0f); };
        for (size_t i = 0; i < n; ++i)
        {
            t.rgba[i * 4 + 0] = pack (t.value[i]);
            t.rgba[i * 4 + 1] = pack (0.5f + t.gradX[i] * c_gradScale);
            t.rgba[i * 4 + 2] = pack (0.5f + t.gradY[i] * c_gradScale);
            t.rgba[i * 4 + 3] = pack (t.ridged[i]);
        }
        return t;
    }

    const NoiseTexture& NoiseTexture::shared()
    {
        static const NoiseTexture t = bake();
        return t;
    }
}

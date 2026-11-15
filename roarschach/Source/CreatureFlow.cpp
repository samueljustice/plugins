#include "CreatureFlow.h"

#include "NoiseTexture.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr float c_gradToUv = 32.0f;   // (G - 0.5) / c_gradScale is per texel; x 256 texels per unit uv

        float clamp01 (float v) { return std::min (1.0f, std::max (0.0f, v)); }
    }

    NoiseSampler::NoiseSampler()
    {
        const auto& t = NoiseTexture::shared();
        Level base;
        base.size = t.size;
        base.rgba.resize (t.rgba.size());
        for (size_t i = 0; i < t.rgba.size(); ++i)
            base.rgba[i] = (float) t.rgba[i] / 255.0f;
        m_levels.push_back (std::move (base));
        // the mip chain, as a 2 x 2 box filter makes it
        while (m_levels.back().size > 1)
        {
            const auto& src = m_levels.back();
            Level dst;
            dst.size = src.size / 2;
            dst.rgba.resize ((size_t) dst.size * (size_t) dst.size * 4);
            for (int y = 0; y < dst.size; ++y)
                for (int x = 0; x < dst.size; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        const auto at = [&src, c] (int sx, int sy) { return src.rgba[((size_t) sy * (size_t) src.size + (size_t) sx) * 4 + (size_t) c]; };
                        dst.rgba[((size_t) y * (size_t) dst.size + (size_t) x) * 4 + (size_t) c]
                            = 0.25f * (at (2 * x, 2 * y) + at (2 * x + 1, 2 * y) + at (2 * x, 2 * y + 1) + at (2 * x + 1, 2 * y + 1));
                    }
            m_levels.push_back (std::move (dst));
        }
    }

    const NoiseSampler& NoiseSampler::shared()
    {
        static const NoiseSampler s;
        return s;
    }

    std::array<float, 4> NoiseSampler::level (int l, float u, float v) const
    {
        const auto& lv = m_levels[(size_t) l];
        const int n = lv.size;
        const float fx = u * (float) n - 0.5f, fy = v * (float) n - 0.5f;
        const float x0f = std::floor (fx), y0f = std::floor (fy);
        const float tx = fx - x0f, ty = fy - y0f;
        const auto wrap = [n] (float f) { const int i = (int) f % n; return i < 0 ? i + n : i; };
        const int x0 = wrap (x0f), y0 = wrap (y0f), x1 = (x0 + 1) % n, y1 = (y0 + 1) % n;
        std::array<float, 4> out {};
        for (int c = 0; c < 4; ++c)
        {
            const auto at = [&lv, n, c] (int x, int y) { return lv.rgba[((size_t) y * (size_t) n + (size_t) x) * 4 + (size_t) c]; };
            const float a = at (x0, y0) + (at (x1, y0) - at (x0, y0)) * tx;
            const float b = at (x0, y1) + (at (x1, y1) - at (x0, y1)) * tx;
            out[(size_t) c] = a + (b - a) * ty;
        }
        return out;
    }

    std::array<float, 4> NoiseSampler::sample (float u, float v, float lod) const
    {
        if (! (std::isfinite (u) && std::isfinite (v)))
            return { 0.5f, 0.0f, 0.0f, 0.5f };
        u -= std::floor (u);
        v -= std::floor (v);
        const float top = (float) ((int) m_levels.size() - 1);
        lod = std::min (top, std::max (0.0f, lod));
        const int l0 = (int) std::floor (lod), l1 = std::min ((int) top, l0 + 1);
        const float t = lod - (float) l0;
        auto a = level (l0, u, v);
        if (t > 0.0f)
        {
            const auto b = level (l1, u, v);
            for (size_t c = 0; c < 4; ++c)
                a[c] += (b[c] - a[c]) * t;
        }
        a[1] = (a[1] - 0.5f) * c_gradToUv;
        a[2] = (a[2] - 0.5f) * c_gradToUv;
        return a;
    }

    //==============================================================================================
    float CreatureFlow::gainFor (float level, float roarHeat)
    {
        return c_restGain + 1.2f * clamp01 (level) + 1.6f * clamp01 (roarHeat);
    }

    namespace
    {
        // The half plane's velocity (x >= 0), exactly as the shader's flow() computes it.
        CreatureFlow::Vec2 halfPlane (const CreatureFlow::Params& k, float x, float y)
        {
            const auto& n = NoiseSampler::shared();
            // the large eddies: psi = n (x, y) - n (-x, y), odd across the fold
            const auto a = n.sample (x * 0.19f + k.o1x, y * 0.19f + k.o1y, 2.5f);
            const auto a2 = n.sample (-x * 0.19f + k.o1x, y * 0.19f + k.o1y, 2.5f);
            float gx = 0.19f * (a[1] + a2[1]), gy = 0.19f * (a[2] - a2[2]);
            // the smaller eddies, faded out at the fold
            const auto b = n.sample (x * 0.47f + k.o2x, y * 0.47f + k.o2y, 1.5f);
            const float s = clamp01 (x / 0.25f);
            const float r = s * s * (3.0f - 2.0f * s), dr = 6.0f * s * (1.0f - s) / 0.25f;
            gx += k.smallGain * 0.55f * (r * b[1] * 0.47f + dr * (b[0] - 0.5f));
            gy += k.smallGain * 0.55f * (r * b[2] * 0.47f);
            float vx = gy * k.gain * CreatureFlow::c_scale, vy = -gx * k.gain * CreatureFlow::c_scale;
            // the bound pair: psi = G (exp (-|p - c|^2 / R^2) - exp (-|p - c'|^2 / R^2)), c' the mirror image
            const float r2 = std::max (1.0e-4f, k.vortexRadius * k.vortexRadius);
            const float d1x = x - k.vortexX, d2x = x + k.vortexX, dy = y - k.vortexY;
            const float e1 = std::exp (-(d1x * d1x + dy * dy) / r2), e2 = std::exp (-(d2x * d2x + dy * dy) / r2);
            const float g = k.vortex * 2.3316f / std::max (1.0e-2f, k.vortexRadius);   // (the peak speed, at R / sqrt 2, is k.vortex)
            vx += g * (-dy * e1 + dy * e2);
            vy += g * (d1x * e1 - d2x * e2);
            // the ceiling: a soft limit, so no term can carry ink faster than this
            const float lim = 1.0f / std::sqrt (1.0f + (vx * vx + vy * vy) / (CreatureFlow::c_ceiling * CreatureFlow::c_ceiling));
            return { vx * lim, vy * lim };
        }
    }

    CreatureFlow::Vec2 CreatureFlow::velocity (const Params& k, float x, float y)
    {
        const auto v = halfPlane (k, std::abs (x), y);
        return { x < 0.0f ? -v[0] : v[0], v[1] };
    }

    CreatureFlow::Vec2 CreatureFlow::backtrace (const Params& k, float x, float y, float tau)
    {
        float qx = std::abs (x), qy = y;
        const float h = tau * 0.25f;
        for (int i = 0; i < 4; ++i)
        {
            const auto v = halfPlane (k, qx, qy);
            qx = std::abs (qx - v[0] * h);
            qy -= v[1] * h;
        }
        return { qx, qy };
    }
}

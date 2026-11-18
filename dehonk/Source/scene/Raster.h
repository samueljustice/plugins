// A small software rasteriser for De-Honk's scene: low-polygon shapes with smooth (per-vertex)
// shading, depth fog, and a depth buffer. The picture is drawn at the size of its card in the
// window (see DisplayComponent). Plain C++, no JUCE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// The inner loops (a row of a triangle, the copies to and from the picture) use the processor's
// vector unit where there is one: NEON on arm64, SSE2 on x86. Each gives exactly the pixels the
// plain loops give, bit for bit (see spanSolid).
#if DEHONK_RASTER_PLAIN
 // the plain loops only (for checks)
#elif defined(__ARM_NEON) || defined(_M_ARM64)
 #include <arm_neon.h>
 #define DEHONK_RASTER_NEON 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
 #include <emmintrin.h>
 #define DEHONK_RASTER_SSE2 1
#endif

namespace dehonk::scene
{

struct Vec3
{
    float x = 0, y = 0, z = 0;
    Vec3 operator+ (Vec3 o) const noexcept { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator- (Vec3 o) const noexcept { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator* (float s) const noexcept { return { x * s, y * s, z * s }; }
};
inline float dot (Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross (Vec3 a, Vec3 b) noexcept { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline Vec3 normalised (Vec3 v) noexcept
{
    const float l = std::sqrt (dot (v, v));
    return l > 1.0e-9f ? v * (1.0f / l) : Vec3 { 0, 1, 0 };
}

/** A rotation and scale (3 x 3, rows) followed by a move. */
struct Xform
{
    float m[9] { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    Vec3 t;

    Vec3 apply (Vec3 v) const noexcept
    {
        return { m[0] * v.x + m[1] * v.y + m[2] * v.z + t.x,
                 m[3] * v.x + m[4] * v.y + m[5] * v.z + t.y,
                 m[6] * v.x + m[7] * v.y + m[8] * v.z + t.z };
    }
    /** Direction only (no move). Good enough for normals while scales are near uniform. */
    Vec3 turn (Vec3 v) const noexcept
    {
        return { m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z };
    }

    /** this, then `outer`. */
    Xform then (const Xform& outer) const noexcept
    {
        Xform r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i * 3 + j] = outer.m[i * 3] * m[j] + outer.m[i * 3 + 1] * m[3 + j] + outer.m[i * 3 + 2] * m[6 + j];
        r.t = outer.apply (t);
        return r;
    }

    static Xform move (Vec3 v) noexcept { Xform r; r.t = v; return r; }
    static Xform scale (float s) noexcept { Xform r; r.m[0] = r.m[4] = r.m[8] = s; return r; }
    static Xform rotX (float a) noexcept { Xform r; const float c = std::cos (a), s = std::sin (a); r.m[4] = c; r.m[5] = -s; r.m[7] = s; r.m[8] = c; return r; }
    static Xform rotY (float a) noexcept { Xform r; const float c = std::cos (a), s = std::sin (a); r.m[0] = c; r.m[2] = s; r.m[6] = -s; r.m[8] = c; return r; }
    static Xform rotZ (float a) noexcept { Xform r; const float c = std::cos (a), s = std::sin (a); r.m[0] = c; r.m[1] = -s; r.m[3] = s; r.m[4] = c; return r; }
};

struct Rgb
{
    float r = 0, g = 0, b = 0;      // 0..255
    Rgb operator* (float s) const noexcept { return { r * s, g * s, b * s }; }
    Rgb operator+ (Rgb o) const noexcept { return { r + o.r, g + o.g, b + o.b }; }
    Rgb mix (Rgb o, float t) const noexcept { return { r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t }; }
};
inline constexpr Rgb rgb (std::uint32_t hex) noexcept
{
    return { (float) ((hex >> 16) & 255u), (float) ((hex >> 8) & 255u), (float) (hex & 255u) };
}

class Raster
{
public:
    void setSize (int w, int h)
    {
        width = w; height = h;
        colour.assign ((size_t) (w * h) * kBytes, 255);
        depth.assign ((size_t) (w * h), 0.0f);
    }

    /** Camera at `eye`, looking along +z tilted down by `pitch` radians. `focal` is in pixels. */
    void setCamera (Vec3 eye, float pitch, float focal) noexcept
    {
        cam = eye; camSin = std::sin (pitch); camCos = std::cos (pitch); focalPx = focal;
    }

    /** One light, a sky colour that fills in the shadows, and fog that things fade into. */
    void setLight (Vec3 towardsLight, Rgb lightColour, Rgb skyFill, Rgb fogColour, float fogStart, float fogEnd) noexcept
    {
        light = normalised (towardsLight); lightCol = lightColour * (1.0f / 255.0f); fill = skyFill * (1.0f / 255.0f);
        fog = fogColour; fogA = fogStart; fogB = std::max (fogStart + 0.01f, fogEnd);
    }

    /** Everything drawn from now on is mixed this far towards the colour given (0 = not at all): for
        fading something back without darkening it. */
    void setVeil (Rgb towards, float amount) noexcept { veil = towards; veilAmount = std::clamp (amount, 0.0f, 1.0f); }

    void clearDepth() noexcept { std::fill (depth.begin(), depth.end(), 1.0e9f); }

    /** Keeps a copy of the picture and its depths as they are now, and puts them back. */
    void keep() { keptColour = colour; keptDepth = depth; }
    void restore() noexcept
    {
        if (keptColour.size() == colour.size() && keptDepth.size() == depth.size())
        {
            std::copy (keptColour.begin(), keptColour.end(), colour.begin());
            std::copy (keptDepth.begin(), keptDepth.end(), depth.begin());
        }
    }

    //============================================================================== the backdrop
    // The camera never rolls, so the sky, the water and the ground are horizontal bands on the
    // screen: they are filled a row at a time, which costs next to nothing.

    /** A row in one colour, written straight in. `zv` is the view depth recorded for the row. */
    void fillRow (int y, Rgb c, float zv) noexcept
    {
        if (y < 0 || y >= height)
            return;
        std::uint32_t* p = reinterpret_cast<std::uint32_t*> (&colour[(size_t) (y * width) * kBytes]);
        const std::uint32_t v = 0xff000000u | ((std::uint32_t) toByte (c.r) << 16) | ((std::uint32_t) toByte (c.g) << 8) | (std::uint32_t) toByte (c.b);
        std::fill_n (p, (size_t) width, v);
        std::fill_n (&depth[(size_t) (y * width)], (size_t) width, zv);
    }

    /** The screen row a point on the ground plane (y = 0) at world depth `z` lands on. */
    float rowOfGroundZ (float z) const noexcept
    {
        const Vec3 d { 0.0f, -cam.y, z - cam.z };
        const float zv = d.z * camCos - d.y * camSin, yv = d.y * camCos + d.z * camSin;
        return (float) height * 0.5f - yv * focalPx / std::max (0.01f, zv);
    }
    /** The world depth and view depth of the ground under screen row `y` (below the horizon). */
    bool groundAtRow (float y, float& worldZ, float& viewZ) const noexcept
    {
        // yv / zv = (h/2 - y) / focal, with yv = -cy*c + dz*s and zv = dz*c + cy*s.
        const float k = ((float) height * 0.5f - y) / focalPx;
        const float den = camSin - k * camCos;
        if (den <= 1.0e-5f)
            return false;                         // at or above the horizon
        const float dz = cam.y * (camCos + k * camSin) / den;
        worldZ = cam.z + dz;
        viewZ = dz * camCos + cam.y * camSin;
        return true;
    }

    /** The world point that lands at (sx, sy), each 0..1 across and down the picture, at a given
        distance along the view. For placing things in the sky by where they should appear. */
    Vec3 atScreen (float sx, float sy, float viewZ) const noexcept
    {
        const float dx = (sx - 0.5f) * (float) width * viewZ / focalPx;
        const float yv = (0.5f - sy) * (float) height * viewZ / focalPx;
        return { cam.x + dx, cam.y + yv * camCos - viewZ * camSin, cam.z + viewZ * camCos + yv * camSin };
    }

    /** The height a point at world depth `z` must have to land on screen row `row`. */
    float heightAtRow (float row, float z) const noexcept
    {
        const float k = ((float) height * 0.5f - row) / focalPx;
        return cam.y + (z - cam.z) * (k * camCos - camSin) / std::max (1.0e-4f, camCos + k * camSin);
    }

    /** How many pixels one world unit covers at a distance along the view. */
    float pixelsPerUnitAt (float viewZ) const noexcept { return focalPx / std::max (0.01f, viewZ); }

    float fogAt (float viewZ) const noexcept { return std::clamp ((viewZ - fogA) / (fogB - fogA), 0.0f, 1.0f); }
    Rgb fogColour() const noexcept { return fog; }

    //============================================================================== shapes
    struct Vertex { Vec3 p; Vec3 n; };

    /** A lit colour for a surface of `base` colour with normal `n`. `glow` 1 = ignore the light. */
    Rgb lit (Rgb base, Vec3 n, float glow = 0.0f) const noexcept
    {
        const float d = std::max (0.0f, dot (n, light));
        const float up = 0.5f + 0.5f * n.y;                       // sky from above, a little less beneath
        Rgb c { base.r * (lightCol.r * d + fill.r * (0.72f + 0.28f * up)),
                base.g * (lightCol.g * d + fill.g * (0.72f + 0.28f * up)),
                base.b * (lightCol.b * d + fill.b * (0.72f + 0.28f * up)) };
        c = { std::min (255.0f, c.r), std::min (255.0f, c.g), std::min (255.0f, c.b) };
        return c.mix (base, glow);
    }

    /** A corner of a triangle as it lands on the picture: where, how deep, and its colour with the
        fog already mixed in. */
    struct SV { float x, y, z; Rgb c; bool ok; };

    SV shade (Vec3 p, Rgb base) const noexcept
    {
        const P q = project (p);
        return { q.x, q.y, q.z, inRange (base.mix (fog, fogAt (q.z)).mix (veil, veilAmount)), q.ok };
    }

    /** The colour a surface of `base` colour with normal `n` at world point `p` is drawn in. */
    Rgb colourAt (Vec3 p, Rgb base, Vec3 n, float glow) const noexcept { return shade (p, lit (base, n, glow)).c; }

    /** World-space triangle with a colour at each corner, shaded smoothly between them.
        alpha < 1 blends over what is there and leaves the depth alone (for rings and splashes). */
    void triangle (Vec3 a, Vec3 b, Vec3 c, Rgb ca, Rgb cb, Rgb cc, float alpha = 1.0f) noexcept
    {
        paintTri (shade (a, ca), shade (b, cb), shade (c, cc), alpha, 0);
    }

    /** Fills a triangle whose corners are already on the picture. `cull` +1 or -1 drops triangles
        wound the other way (the far side of a closed shape); 0 draws either.
        Filled a row at a time between its two edges, with the depth and the colour stepped along
        each row; a pixel belongs to the triangle when its centre is inside, so triangles that
        share an edge never draw a pixel twice. */
    void paintTri (const SV& pa, const SV& pb, const SV& pc, float alpha, int cull) noexcept
    {
        if (! (pa.ok && pb.ok && pc.ok))
            return;
        const float area = (pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x);
        if (std::abs (area) < 1.0e-4f || (cull != 0 && (area > 0.0f) != (cull > 0)))
            return;
        const float minXf = std::min ({ pa.x, pb.x, pc.x }), maxXf = std::max ({ pa.x, pb.x, pc.x });
        const float minYf = std::min ({ pa.y, pb.y, pc.y }), maxYf = std::max ({ pa.y, pb.y, pc.y });
        if (maxXf < 0.0f || minXf > (float) width || maxYf < 0.0f || minYf > (float) height)
            return;
        const int y0 = std::max (0, (int) std::ceil (minYf - 0.5f)), y1 = std::min (height - 1, (int) std::ceil (maxYf - 0.5f) - 1);
        if (y0 > y1)
            return;

        const Rgb ca = pa.c, cb = pb.c, cc = pc.c;

        // How each value changes per pixel across and down the picture.
        const float inv = 1.0f / area;
        const float bx = pb.x - pa.x, by = pb.y - pa.y, cx = pc.x - pa.x, cy = pc.y - pa.y;
        auto slopes = [&] (float va, float vb, float vc, float& ddx, float& ddy) noexcept
        {
            ddx = ((vb - va) * cy - (vc - va) * by) * inv;
            ddy = ((vc - va) * bx - (vb - va) * cx) * inv;
        };
        float zx, zy, rx, ry, gx, gy, blx, bly;
        slopes (pa.z, pb.z, pc.z, zx, zy);
        slopes (ca.r, cb.r, cc.r, rx, ry);
        slopes (ca.g, cb.g, cc.g, gx, gy);
        slopes (ca.b, cb.b, cc.b, blx, bly);

        const SV* e0[3] = { &pa, &pb, &pc };
        const SV* e1[3] = { &pb, &pc, &pa };
        float slope[3], lo[3], hi[3];
        for (int i = 0; i < 3; ++i)
        {
            lo[i] = std::min (e0[i]->y, e1[i]->y); hi[i] = std::max (e0[i]->y, e1[i]->y);
            slope[i] = hi[i] > lo[i] ? (e1[i]->x - e0[i]->x) / (e1[i]->y - e0[i]->y) : 0.0f;
        }

        for (int y = y0; y <= y1; ++y)
        {
            const float py = (float) y + 0.5f;
            float xl = 1.0e9f, xr = -1.0e9f;
            for (int i = 0; i < 3; ++i)
                if (py >= lo[i] && py < hi[i])
                {
                    const float x = e0[i]->x + (py - e0[i]->y) * slope[i];
                    xl = std::min (xl, x); xr = std::max (xr, x);
                }
            const int xa = std::max (0, (int) std::ceil (xl - 0.5f)), xb = std::min (width - 1, (int) std::ceil (xr - 0.5f) - 1);
            if (xa > xb)
                continue;
            const float sx = (float) xa + 0.5f - pa.x, sy = py - pa.y;
            float z = pa.z + sx * zx + sy * zy;
            float r = ca.r + sx * rx + sy * ry + 0.5f, g = ca.g + sx * gx + sy * gy + 0.5f, bl = ca.b + sx * blx + sy * bly + 0.5f;
            float* zb = &depth[(size_t) (y * width + xa)];
            std::uint8_t* p = &colour[(size_t) (y * width + xa) * kBytes];
            if (alpha >= 1.0f)
                spanSolid (p, zb, xb - xa + 1, bl, g, r, z, blx, gx, rx, zx);
            else
                spanBlend (p, zb, xb - xa + 1, bl, g, r, z, blx, gx, rx, zx, alpha);
        }
    }

    /** One row of a triangle: `n` pixels from `p` (and their depths from `zb`), the colour (blue,
        green, red, already with a half added for rounding) and the depth stepped along by the
        given amounts. A pixel is written where it is nearer than what is there.
        The vector versions keep the four values in the four lanes of one register and step them
        all with one add, so each value takes exactly the same additions, in the same order, as it
        would one at a time: the pictures are the same bit for bit. Stepping can overshoot a
        corner's colour by a hair at the very edge, hence the clamp. */
    static void spanSolid (std::uint8_t* p, float* zb, int n, float b, float g, float r, float z,
                           float bx, float gx, float rx, float zx) noexcept
    {
       #if DEHONK_RASTER_NEON
        const float lanes[4] { b, g, r, z }, steps[4] { bx, gx, rx, zx };
        float32x4_t v = vld1q_f32 (lanes);
        const float32x4_t d = vld1q_f32 (steps), lo = vdupq_n_f32 (0.0f), hi = vdupq_n_f32 (255.0f);
        const uint32x4_t opaque = vdupq_n_u32 (0xff000000u);
        int i = 0;
        // Four pixels at a time: their four sets of values (each stepped exactly as one at a time),
        // one depth test for the four, and the colours and depths written where they pass.
        for (; i + 4 <= n; i += 4, p += 4 * kBytes)
        {
            const float32x4_t v0 = v, v1 = vaddq_f32 (v0, d), v2 = vaddq_f32 (v1, d), v3 = vaddq_f32 (v2, d);
            v = vaddq_f32 (v3, d);
            const float32x4_t z4 = vcombine_f32 (vget_high_f32 (vzip2q_f32 (v0, v1)), vget_high_f32 (vzip2q_f32 (v2, v3)));
            const float32x4_t was = vld1q_f32 (zb + i);
            const uint32x4_t in = vcltq_f32 (z4, was);
            if (vmaxvq_u32 (in) == 0)
                continue;
            vst1q_f32 (zb + i, vbslq_f32 (in, z4, was));
            auto bytes = [&] (float32x4_t c) { return vmovn_u32 (vreinterpretq_u32_s32 (vcvtq_s32_f32 (vminq_f32 (vmaxq_f32 (c, lo), hi)))); };
            const uint8x16_t px = vcombine_u8 (vmovn_u16 (vcombine_u16 (bytes (v0), bytes (v1))), vmovn_u16 (vcombine_u16 (bytes (v2), bytes (v3))));
            const uint32x4_t now = vorrq_u32 (vreinterpretq_u32_u8 (px), opaque);
            vst1q_u8 (p, vreinterpretq_u8_u32 (vbslq_u32 (in, now, vreinterpretq_u32_u8 (vld1q_u8 (p)))));
        }
        zb += i;
        n -= i;
        for (i = 0; i < n; ++i, p += kBytes, v = vaddq_f32 (v, d))
        {
            const float zz = vgetq_lane_f32 (v, 3);
            if (zz < zb[i])
            {
                zb[i] = zz;
                storePixel (p, vcvtq_s32_f32 (vminq_f32 (vmaxq_f32 (v, lo), hi)));
            }
        }
       #elif DEHONK_RASTER_SSE2
        __m128 v = _mm_setr_ps (b, g, r, z);
        const __m128 d = _mm_setr_ps (bx, gx, rx, zx), lo = _mm_setzero_ps(), hi = _mm_set1_ps (255.0f);
        const __m128i opaque = _mm_set1_epi32 ((int) 0xff000000u);
        int i = 0;
        for (; i + 4 <= n; i += 4, p += 4 * kBytes)
        {
            const __m128 v0 = v, v1 = _mm_add_ps (v0, d), v2 = _mm_add_ps (v1, d), v3 = _mm_add_ps (v2, d);
            v = _mm_add_ps (v3, d);
            const __m128 z4 = _mm_movehl_ps (_mm_unpackhi_ps (v2, v3), _mm_unpackhi_ps (v0, v1));
            const __m128 was = _mm_loadu_ps (zb + i);
            const __m128 in = _mm_cmplt_ps (z4, was);
            if (_mm_movemask_ps (in) == 0)
                continue;
            _mm_storeu_ps (zb + i, _mm_or_ps (_mm_and_ps (in, z4), _mm_andnot_ps (in, was)));
            auto ints = [&] (__m128 c) { return _mm_cvttps_epi32 (_mm_min_ps (_mm_max_ps (c, lo), hi)); };
            const __m128i now = _mm_or_si128 (_mm_packus_epi16 (_mm_packs_epi32 (ints (v0), ints (v1)), _mm_packs_epi32 (ints (v2), ints (v3))), opaque);
            const __m128i mask = _mm_castps_si128 (in);
            const __m128i there = _mm_loadu_si128 (reinterpret_cast<const __m128i*> (p));
            _mm_storeu_si128 (reinterpret_cast<__m128i*> (p), _mm_or_si128 (_mm_and_si128 (mask, now), _mm_andnot_si128 (mask, there)));
        }
        zb += i;
        n -= i;
        for (i = 0; i < n; ++i, p += kBytes, v = _mm_add_ps (v, d))
        {
            const float zz = _mm_cvtss_f32 (_mm_shuffle_ps (v, v, _MM_SHUFFLE (3, 3, 3, 3)));
            if (zz < zb[i])
            {
                zb[i] = zz;
                storePixel (p, _mm_cvttps_epi32 (_mm_min_ps (_mm_max_ps (v, lo), hi)));
            }
        }
       #else
        for (int i = 0; i < n; ++i, p += kBytes, z += zx, r += rx, g += gx, b += bx)
            if (z < zb[i])
            {
                zb[i] = z;
                p[2] = byteOf (r); p[1] = byteOf (g); p[0] = byteOf (b);
            }
       #endif
    }

    /** As spanSolid, but mixed `alpha` of the way over what is there, and the depths left alone. */
    static void spanBlend (std::uint8_t* p, const float* zb, int n, float b, float g, float r, float z,
                           float bx, float gx, float rx, float zx, float alpha) noexcept
    {
       #if DEHONK_RASTER_NEON
        const float lanes[4] { b, g, r, z }, steps[4] { bx, gx, rx, zx };
        float32x4_t v = vld1q_f32 (lanes);
        const float32x4_t d = vld1q_f32 (steps), lo = vdupq_n_f32 (0.0f), hi = vdupq_n_f32 (255.0f), a = vdupq_n_f32 (alpha);
        for (int i = 0; i < n; ++i, p += kBytes, v = vaddq_f32 (v, d))
            if (vgetq_lane_f32 (v, 3) < zb[i])
            {
                // What is there, as floats; then there + (new - there) * alpha, in one rounding
                // (as the plain loop is compiled for arm64, where the multiply and add are fused).
                std::uint32_t px;
                std::memcpy (&px, p, 4);
                const float32x4_t there = vcvtq_f32_u32 (vmovl_u16 (vget_low_u16 (vmovl_u8 (vcreate_u8 (px)))));
                storePixel (p, vcvtq_s32_f32 (vminq_f32 (vmaxq_f32 (vfmaq_f32 (there, vsubq_f32 (v, there), a), lo), hi)));
            }
       #elif DEHONK_RASTER_SSE2
        __m128 v = _mm_setr_ps (b, g, r, z);
        const __m128 d = _mm_setr_ps (bx, gx, rx, zx), lo = _mm_setzero_ps(), hi = _mm_set1_ps (255.0f), a = _mm_set1_ps (alpha);
        const __m128i zero = _mm_setzero_si128();
        for (int i = 0; i < n; ++i, p += kBytes, v = _mm_add_ps (v, d))
            if (_mm_cvtss_f32 (_mm_shuffle_ps (v, v, _MM_SHUFFLE (3, 3, 3, 3))) < zb[i])
            {
                std::int32_t px;
                std::memcpy (&px, p, 4);
                const __m128 there = _mm_cvtepi32_ps (_mm_unpacklo_epi16 (_mm_unpacklo_epi8 (_mm_cvtsi32_si128 (px), zero), zero));
                storePixel (p, _mm_cvttps_epi32 (_mm_min_ps (_mm_max_ps (_mm_add_ps (there, _mm_mul_ps (_mm_sub_ps (v, there), a)), lo), hi)));
            }
       #else
        for (int i = 0; i < n; ++i, p += kBytes, z += zx, r += rx, g += gx, b += bx)
            if (z < zb[i])
            {
                p[2] = byteOf ((float) p[2] + (r - (float) p[2]) * alpha);
                p[1] = byteOf ((float) p[1] + (g - (float) p[1]) * alpha);
                p[0] = byteOf ((float) p[0] + (b - (float) p[0]) * alpha);
            }
       #endif
    }

    /** A flat-coloured quad (lit by its own face). */
    void quad (Vec3 a, Vec3 b, Vec3 c, Vec3 d, Rgb base, float glow = 0.0f, float alpha = 1.0f) noexcept
    {
        const Rgb col = lit (base, facing (normalised (cross (b - a, c - a)), a), glow);
        triangle (a, b, c, col, col, col, alpha);
        triangle (a, c, d, col, col, col, alpha);
    }

    /** A box of half-sizes (hx, hy, hz) about the origin, placed by `x`. Flat faces. */
    void box (const Xform& x, float hx, float hy, float hz, Rgb base, float glow = 0.0f) noexcept
    {
        const Vec3 v[8] = { x.apply ({ -hx, -hy, -hz }), x.apply ({ hx, -hy, -hz }), x.apply ({ hx, hy, -hz }), x.apply ({ -hx, hy, -hz }),
                            x.apply ({ -hx, -hy, hz }),  x.apply ({ hx, -hy, hz }),  x.apply ({ hx, hy, hz }),  x.apply ({ -hx, hy, hz }) };
        quad (v[0], v[3], v[2], v[1], base, glow);
        quad (v[4], v[5], v[6], v[7], base, glow);
        quad (v[0], v[4], v[7], v[3], base, glow);
        quad (v[1], v[2], v[6], v[5], base, glow);
        quad (v[3], v[7], v[6], v[2], base, glow);
        quad (v[0], v[1], v[5], v[4], base, glow);
    }

    /** A rounded blob: an ellipsoid of radii (rx, ry, rz) about the origin, placed by `x`, shaded
        smoothly. `around` and `up` set how many faces it has. Each corner is lit and placed on the
        picture once, and the faces on the far side are not drawn at all. */
    void blob (const Xform& x, float rx, float ry, float rz, Rgb base, int around = 8, int up = 5, float glow = 0.0f) noexcept
    {
        around = std::clamp (around, 3, kMaxRing); up = std::clamp (up, 2, kMaxRing);
        const Circle& ca = circles().full[around];
        const Circle& cu = circles().half[up];
        SV v[kMaxRing + 1][kMaxRing + 1];
        const float irx = 1.0f / rx, iry = 1.0f / ry, irz = 1.0f / rz;
        for (int j = 0; j <= up; ++j)
        {
            const float sv = cu.s[j], cv = cu.c[j];
            for (int i = 0; i <= around; ++i)
            {
                const Vec3 n { sv * ca.c[i], cv, sv * ca.s[i] };
                v[j][i] = shade (x.apply ({ n.x * rx, n.y * ry, n.z * rz }), lit (base, normalised (x.turn ({ n.x * irx, n.y * iry, n.z * irz })), glow));
            }
        }
        // Which way round the near faces go on the picture (a mirrored placement turns them over).
        const float* m = x.m;
        const float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
        const int cull = det < 0.0f ? -kNearWinding : kNearWinding;
        for (int j = 0; j < up; ++j)
            for (int i = 0; i < around; ++i)
            {
                paintTri (v[j][i], v[j + 1][i], v[j + 1][i + 1], 1.0f, cull);
                paintTri (v[j][i], v[j + 1][i + 1], v[j][i + 1], 1.0f, cull);
            }
    }

    /** A limb: a round tube from `a` to `b` (world space), radius r0 at a and r1 at b, shaded
        smoothly round its length. r1 = 0 makes a cone. */
    void limb (Vec3 a, Vec3 b, float r0, float r1, Rgb base, int around = 6, float glow = 0.0f) noexcept
    {
        around = std::clamp (around, 3, kMaxRing);
        const Circle& ca = circles().full[around];
        const Vec3 axis = normalised (b - a);
        const Vec3 side = normalised (cross (axis, std::abs (axis.y) < 0.9f ? Vec3 { 0, 1, 0 } : Vec3 { 1, 0, 0 }));
        const Vec3 other = cross (axis, side);
        SV p0[kMaxRing + 1], p1[kMaxRing + 1];
        for (int i = 0; i <= around; ++i)
        {
            const Vec3 n = side * ca.c[i] + other * ca.s[i];
            const Rgb c = lit (base, n, glow);
            p0[i] = shade (a + n * r0, c);
            p1[i] = shade (b + n * r1, c);
        }
        for (int i = 0; i < around; ++i)
        {
            paintTri (p0[i], p1[i], p1[i + 1], 1.0f, kLimbCull);
            paintTri (p0[i], p1[i + 1], p0[i + 1], 1.0f, kLimbCull);
        }
    }

    /** A thin line straight onto the picture (screen coordinates), blended over what is there. */
    void streak (float x0, float y0, float x1, float y1, Rgb col, float alpha) noexcept
    {
        const int steps = std::max (1, (int) std::ceil (std::max (std::abs (x1 - x0), std::abs (y1 - y0))));
        for (int i = 0; i <= steps; ++i)
        {
            const float t = (float) i / (float) steps;
            const int x = (int) std::lround (x0 + (x1 - x0) * t), y = (int) std::lround (y0 + (y1 - y0) * t);
            if (x < 0 || y < 0 || x >= width || y >= height)
                continue;
            std::uint8_t* p = &colour[(size_t) (y * width + x) * kBytes];
            p[2] = toByte ((float) p[2] + (col.r - (float) p[2]) * alpha);
            p[1] = toByte ((float) p[1] + (col.g - (float) p[1]) * alpha);
            p[0] = toByte ((float) p[0] + (col.b - (float) p[0]) * alpha);
        }
    }

    int width = 0, height = 0;
    static constexpr int kBytes = 4;     // per pixel: blue, green, red, 255 (the layout of an opaque ARGB image)
    std::vector<std::uint8_t> colour;    // row by row

    struct P { float x, y, z; bool ok; };

    /** Where the camera is. */
    Vec3 eye() const noexcept { return cam; }

    P project (Vec3 p) const noexcept
    {
        const Vec3 d = p - cam;
        const float zv = d.z * camCos - d.y * camSin;          // along the view direction
        const float yv = d.y * camCos + d.z * camSin;          // up the screen
        if (zv < 0.5f)
            return { 0, 0, 0, false };
        const float s = focalPx / zv;
        return { (float) width * 0.5f + d.x * s, (float) height * 0.5f - yv * s, zv, true };
    }

private:
    static constexpr int kMaxRing = 12;
    static constexpr int kNearWinding = -1;  // the sign of the area of a blob's near faces on the picture
    static constexpr int kLimbCull = -1;      // a tube is wound the same way: its far side is not drawn either

    /** Sines and cosines round a circle (and half way round), for every number of faces used. */
    struct Circle { float s[kMaxRing + 1], c[kMaxRing + 1]; };
    struct Circles
    {
        Circle full[kMaxRing + 1], half[kMaxRing + 1];
        Circles() noexcept
        {
            for (int n = 1; n <= kMaxRing; ++n)
                for (int i = 0; i <= kMaxRing; ++i)
                {
                    full[n].s[i] = std::sin (6.2831853f * (float) i / (float) n); full[n].c[i] = std::cos (6.2831853f * (float) i / (float) n);
                    half[n].s[i] = std::sin (3.14159265f * (float) i / (float) n); half[n].c[i] = std::cos (3.14159265f * (float) i / (float) n);
                }
            for (int i = 0; i <= kMaxRing; ++i)
                full[0].s[i] = full[0].c[i] = half[0].s[i] = half[0].c[i] = 0.0f;
        }
    };
    static const Circles& circles() noexcept { static const Circles c; return c; }

    /** Writes a pixel from four whole numbers 0..255 (blue, green, red, anything): the last byte is
        always 255, as every pixel of the picture is opaque. */
   #if DEHONK_RASTER_NEON
    static void storePixel (std::uint8_t* p, int32x4_t c) noexcept
    {
        const uint16x4_t h = vmovn_u32 (vreinterpretq_u32_s32 (c));
        const std::uint32_t px = vget_lane_u32 (vreinterpret_u32_u8 (vmovn_u16 (vcombine_u16 (h, h))), 0) | 0xff000000u;
        std::memcpy (p, &px, 4);
    }
   #elif DEHONK_RASTER_SSE2
    static void storePixel (std::uint8_t* p, __m128i c) noexcept
    {
        const __m128i w = _mm_packs_epi32 (c, c);
        const std::uint32_t px = (std::uint32_t) _mm_cvtsi128_si32 (_mm_packus_epi16 (w, w)) | 0xff000000u;
        std::memcpy (p, &px, 4);
    }
   #endif

    static std::uint8_t toByte (float v) noexcept { return (std::uint8_t) std::clamp ((int) (v + 0.5f), 0, 255); }
    /** Truncates a value that already has its half added for rounding, kept inside 0..255. */
    static std::uint8_t byteOf (float v) noexcept { return (std::uint8_t) (int) std::min (255.0f, std::max (0.0f, v)); }
    static Rgb inRange (Rgb c) noexcept { return { std::clamp (c.r, 0.0f, 255.0f), std::clamp (c.g, 0.0f, 255.0f), std::clamp (c.b, 0.0f, 255.0f) }; }

    /** A face normal turned to face the camera, so that either winding lights the same. */
    Vec3 facing (Vec3 n, Vec3 at) const noexcept { return dot (n, cam - at) < 0.0f ? n * -1.0f : n; }

    std::vector<float> depth, keptDepth;
    std::vector<std::uint8_t> keptColour;
    Vec3 cam, light { 0, 1, 0 };
    Rgb lightCol { 1, 1, 1 }, fill { 0.4f, 0.4f, 0.4f }, fog, veil;
    float veilAmount = 0.0f;
    float camSin = 0, camCos = 1, focalPx = 200, fogA = 20, fogB = 60;
};

} // namespace dehonk::scene

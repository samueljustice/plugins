// A small software rasteriser for De-Honk's scene: low-polygon shapes with smooth (per-vertex)
// shading, depth fog, and a depth buffer. The picture is drawn at a modest size and scaled up
// smoothly by the editor. Plain C++, no JUCE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

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
        colour.assign ((size_t) (w * h) * 3, 0);
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
        std::uint8_t* p = &colour[(size_t) (y * width) * 3];
        const std::uint8_t r = toByte (c.r), g = toByte (c.g), b = toByte (c.b);
        for (int x = 0; x < width; ++x, p += 3)
        {
            p[0] = r; p[1] = g; p[2] = b;
        }
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

    /** World-space triangle with a colour at each corner, shaded smoothly between them.
        alpha < 1 blends over what is there and leaves the depth alone (for rings and splashes).
        Filled a row at a time between its two edges, with the depth and the colour stepped along
        each row; a pixel belongs to the triangle when its centre is inside, so triangles that
        share an edge never draw a pixel twice. */
    void triangle (Vec3 a, Vec3 b, Vec3 c, Rgb ca, Rgb cb, Rgb cc, float alpha = 1.0f) noexcept
    {
        const P pa = project (a), pb = project (b), pc = project (c);
        if (! (pa.ok && pb.ok && pc.ok))
            return;
        const float area = (pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x);
        if (std::abs (area) < 1.0e-4f)
            return;
        const float minXf = std::min ({ pa.x, pb.x, pc.x }), maxXf = std::max ({ pa.x, pb.x, pc.x });
        const float minYf = std::min ({ pa.y, pb.y, pc.y }), maxYf = std::max ({ pa.y, pb.y, pc.y });
        if (maxXf < 0.0f || minXf > (float) width || maxYf < 0.0f || minYf > (float) height)
            return;
        const int y0 = std::max (0, (int) std::ceil (minYf - 0.5f)), y1 = std::min (height - 1, (int) std::ceil (maxYf - 0.5f) - 1);
        if (y0 > y1)
            return;

        ca = inRange (ca.mix (fog, fogAt (pa.z))); cb = inRange (cb.mix (fog, fogAt (pb.z))); cc = inRange (cc.mix (fog, fogAt (pc.z)));

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

        const P* e0[3] = { &pa, &pb, &pc };
        const P* e1[3] = { &pb, &pc, &pa };
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
            std::uint8_t* p = &colour[(size_t) (y * width + xa) * 3];
            if (alpha >= 1.0f)
            {
                for (int x = xa; x <= xb; ++x, ++zb, p += 3, z += zx, r += rx, g += gx, bl += blx)
                    if (z < *zb)
                    {
                        *zb = z;
                        // Stepping can overshoot a corner's colour by a hair at the very edge.
                        p[0] = byteOf (r); p[1] = byteOf (g); p[2] = byteOf (bl);
                    }
            }
            else
            {
                for (int x = xa; x <= xb; ++x, ++zb, p += 3, z += zx, r += rx, g += gx, bl += blx)
                    if (z < *zb)
                    {
                        p[0] = byteOf ((float) p[0] + (r - (float) p[0]) * alpha);
                        p[1] = byteOf ((float) p[1] + (g - (float) p[1]) * alpha);
                        p[2] = byteOf ((float) p[2] + (bl - (float) p[2]) * alpha);
                    }
            }
        }
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
        smoothly. `around` and `up` set how many faces it has. */
    void blob (const Xform& x, float rx, float ry, float rz, Rgb base, int around = 8, int up = 5, float glow = 0.0f) noexcept
    {
        around = std::clamp (around, 3, kMaxRing); up = std::clamp (up, 2, kMaxRing);
        Vec3 pos[kMaxRing + 1][kMaxRing + 1];
        Rgb col[kMaxRing + 1][kMaxRing + 1];
        for (int j = 0; j <= up; ++j)
        {
            const float v = 3.14159265f * (float) j / (float) up, sv = std::sin (v), cv = std::cos (v);
            for (int i = 0; i <= around; ++i)
            {
                const float u = 6.2831853f * (float) i / (float) around;
                const Vec3 n { sv * std::cos (u), cv, sv * std::sin (u) };
                pos[j][i] = x.apply ({ n.x * rx, n.y * ry, n.z * rz });
                col[j][i] = lit (base, normalised (x.turn ({ n.x / rx, n.y / ry, n.z / rz })), glow);
            }
        }
        for (int j = 0; j < up; ++j)
            for (int i = 0; i < around; ++i)
            {
                triangle (pos[j][i], pos[j + 1][i], pos[j + 1][i + 1], col[j][i], col[j + 1][i], col[j + 1][i + 1]);
                triangle (pos[j][i], pos[j + 1][i + 1], pos[j][i + 1], col[j][i], col[j + 1][i + 1], col[j][i + 1]);
            }
    }

    /** A limb: a round tube from `a` to `b` (world space), radius r0 at a and r1 at b, shaded
        smoothly round its length. r1 = 0 makes a cone. */
    void limb (Vec3 a, Vec3 b, float r0, float r1, Rgb base, int around = 6, float glow = 0.0f) noexcept
    {
        around = std::clamp (around, 3, kMaxRing);
        const Vec3 axis = normalised (b - a);
        const Vec3 side = normalised (cross (axis, std::abs (axis.y) < 0.9f ? Vec3 { 0, 1, 0 } : Vec3 { 1, 0, 0 }));
        const Vec3 other = cross (axis, side);
        Vec3 p0[kMaxRing + 1], p1[kMaxRing + 1];
        Rgb c[kMaxRing + 1];
        for (int i = 0; i <= around; ++i)
        {
            const float u = 6.2831853f * (float) i / (float) around;
            const Vec3 n = side * std::cos (u) + other * std::sin (u);
            p0[i] = a + n * r0;
            p1[i] = b + n * r1;
            c[i] = lit (base, n, glow);
        }
        for (int i = 0; i < around; ++i)
        {
            triangle (p0[i], p1[i], p1[i + 1], c[i], c[i], c[i + 1]);
            triangle (p0[i], p1[i + 1], p0[i + 1], c[i], c[i + 1], c[i + 1]);
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
            std::uint8_t* p = &colour[(size_t) (y * width + x) * 3];
            p[0] = toByte ((float) p[0] + (col.r - (float) p[0]) * alpha);
            p[1] = toByte ((float) p[1] + (col.g - (float) p[1]) * alpha);
            p[2] = toByte ((float) p[2] + (col.b - (float) p[2]) * alpha);
        }
    }

    int width = 0, height = 0;
    std::vector<std::uint8_t> colour;    // RGB, row by row

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

    static std::uint8_t toByte (float v) noexcept { return (std::uint8_t) std::clamp ((int) (v + 0.5f), 0, 255); }
    /** Truncates a value that already has its half added for rounding, kept inside 0..255. */
    static std::uint8_t byteOf (float v) noexcept { return (std::uint8_t) (int) std::min (255.0f, std::max (0.0f, v)); }
    static Rgb inRange (Rgb c) noexcept { return { std::clamp (c.r, 0.0f, 255.0f), std::clamp (c.g, 0.0f, 255.0f), std::clamp (c.b, 0.0f, 255.0f) }; }

    /** A face normal turned to face the camera, so that either winding lights the same. */
    Vec3 facing (Vec3 n, Vec3 at) const noexcept { return dot (n, cam - at) < 0.0f ? n * -1.0f : n; }

    std::vector<float> depth, keptDepth;
    std::vector<std::uint8_t> keptColour;
    Vec3 cam, light { 0, 1, 0 };
    Rgb lightCol { 1, 1, 1 }, fill { 0.4f, 0.4f, 0.4f }, fog;
    float camSin = 0, camCos = 1, focalPx = 200, fogA = 20, fogB = 60;
};

} // namespace dehonk::scene

#include "FlightView.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace flight
{
namespace
{
    constexpr Rgb rgb (std::uint32_t hex) noexcept
    {
        return { (float) ((hex >> 16) & 0xff) / 255.0f, (float) ((hex >> 8) & 0xff) / 255.0f, (float) (hex & 0xff) / 255.0f };
    }
    Rgb mix (Rgb a, Rgb b, float t) noexcept { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
    Rgb scaled (Rgb a, float s) noexcept { return { a.r * s, a.g * s, a.b * s }; }

    // Theme's palette, as plain numbers (this layer has no JUCE).
    constexpr Rgb c_void = rgb (0x05070d), c_panel = rgb (0x0b1220), c_edge = rgb (0x1b2a44);
    constexpr Rgb c_cyan = rgb (0x2ee6ff), c_lime = rgb (0xc6ff3d), c_text = rgb (0xe8f1ff), c_dim = rgb (0x6f86a8);

    // The world: the track on the left, the lane on the right, the camera between and behind.
    constexpr float c_trackLeft = -2.1f, c_trackRight = -0.35f;
    constexpr float c_laneLeft = 0.35f, c_laneRight = 1.65f;
    constexpr float c_camX = -0.2f, c_camBack = 4.4f, c_camLift = 1.5f;
    constexpr float c_horizon = 0.22f;          // the horizon's height in the frame, from the top
    constexpr float c_viewDistance = 50.0f, c_fogStart = 10.0f;
    constexpr float c_skirt = 0.35f;            // how deep the ribbons' side walls go
    constexpr float c_checkLength = 1.4f;       // world length of one checker square
    constexpr float c_gateHeight = 1.0f;

    float fogAt (float depth) noexcept
    {
        const float f = std::clamp ((depth - c_fogStart) / (c_viewDistance - c_fogStart), 0.0f, 1.0f);
        return std::sqrt (f);   // the middle distance already hazes; the far end melts into the void
    }

    // 3 x 5 digits, the gate numbers: one bit per pixel, rows top to bottom.
    constexpr std::uint16_t c_digits[10] = {
        0b111101101101111, 0b010110010010111, 0b111001111100111, 0b111001111001111, 0b101101111001001,
        0b111100111001111, 0b111100111101111, 0b111001001010010, 0b111101111101111, 0b111101111001111
    };

    void drawNumber (Raster& r, int value, int cx, int top, int scale, Rgb colour)
    {
        const auto digits = std::to_string (value);
        const int w = (int) digits.size() * 4 * scale - scale;
        int x = cx - w / 2;
        for (const char ch : digits)
        {
            const auto bits = c_digits[ch - '0'];
            for (int row = 0; row < 5; ++row)
                for (int col = 0; col < 3; ++col)
                    if ((bits >> (14 - (row * 3 + col))) & 1)
                        for (int sy = 0; sy < scale; ++sy)
                            for (int sx = 0; sx < scale; ++sx)
                                r.set (x + col * scale + sx, top + row * scale + sy, colour);
            x += 4 * scale;
        }
    }

    std::uint32_t hash (std::uint32_t x) noexcept
    {
        x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
        return x;
    }
}

//==============================================================================
Projected project (const Camera& cam, Vec3 p) noexcept
{
    Projected out;
    out.depth = p.z - cam.z;
    out.inFront = out.depth >= cam.nearZ;
    const float d = std::max (out.depth, cam.nearZ);
    out.x = (int) std::lround (cam.cx + cam.focal * (p.x - cam.x) / d);
    out.y = (int) std::lround (cam.cy - cam.focal * (p.y - cam.y) / d);
    return out;
}

double unitsPerSemitone (double lowMidi, double highMidi) noexcept
{
    return kCourseSpan / std::max (kMinSemitones, highMidi - lowMidi);
}

float Course::at (const std::vector<float>& h, double stepSeconds, double seconds) noexcept
{
    if (h.empty())
        return 0.0f;
    const double pos = std::clamp (seconds / stepSeconds, 0.0, (double) h.size() - 1.0);
    const auto i = (std::size_t) pos;
    const auto j = std::min (i + 1, h.size() - 1);
    const float t = (float) (pos - (double) i);
    return h[i] + (h[j] - h[i]) * t;
}

Course buildCourse (const pitchflattener::core::PitchLine& line, bool flattened, double targetMidi, double amount, double stepSeconds)
{
    Course c;
    const auto& midi = line.midi;
    if (midi.empty() || line.hopSeconds <= 0.0 || stepSeconds <= 0.0)
        return c;

    // The range to fit: where the line sits (its 5th to 95th percentile, so a stray frame cannot
    // squash the rest), and the target, which must always be on screen.
    double low = line.lowMidi, high = line.highMidi;
    if (! (high > low))
    {
        const auto [mn, mx] = std::minmax_element (midi.begin(), midi.end());
        low = *mn;
        high = *mx;
    }
    if (flattened)
    {
        low = std::min (low, targetMidi);
        high = std::max (high, targetMidi);
    }
    const double ups = unitsPerSemitone (low, high);
    const double centre = 0.5 * (low + high);
    const auto heightOf = [&] (double m) { return (float) std::clamp ((m - centre) * ups, (double) -kCourseSpan, (double) kCourseSpan); };

    c.valid = true;
    c.hasLane = flattened;
    c.stepSeconds = stepSeconds;
    c.seconds = (double) midi.size() * line.hopSeconds;
    const auto n = (std::size_t) std::ceil (c.seconds / stepSeconds) + 1;
    c.track.resize (n);
    if (flattened)
        c.lane.resize (n);

    const double frames = (double) midi.size();
    c.lowest = 1.0e9f;
    c.highest = -1.0e9f;
    for (std::size_t i = 0; i < n; ++i)
    {
        // Frame f is the sound around (f + 0.5) * hop.
        const double fp = std::clamp ((double) i * stepSeconds / line.hopSeconds - 0.5, 0.0, frames - 1.0);
        const auto f0 = (std::size_t) fp;
        const auto f1 = std::min (f0 + 1, midi.size() - 1);
        const double m = midi[f0] + (midi[f1] - midi[f0]) * (fp - (double) f0);
        c.track[i] = heightOf (m);
        c.lowest = std::min (c.lowest, c.track[i]);
        c.highest = std::max (c.highest, c.track[i]);
        if (flattened)
        {
            c.lane[i] = heightOf (targetMidi + (1.0 - amount) * (m - targetMidi));
            c.lowest = std::min (c.lowest, c.lane[i]);
            c.highest = std::max (c.highest, c.lane[i]);
        }
    }
    return c;
}

Course demoCourse()
{
    Course c;
    c.valid = true;
    c.demo = true;
    c.hasLane = true;
    c.stepSeconds = kStepSeconds;
    c.seconds = 64.0;
    const auto n = (std::size_t) std::ceil (c.seconds / c.stepSeconds) + 1;
    c.track.resize (n);
    c.lane.assign (n, 0.55f);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = (double) i * c.stepSeconds;
        c.track[i] = (float) (0.6 * std::sin (t * 0.85) + 0.28 * std::sin (t * 2.1 + 1.0) + 0.1 * std::sin (t * 4.7 + 2.0) - 0.1);
    }
    const auto [mn, mx] = std::minmax_element (c.track.begin(), c.track.end());
    c.lowest = std::min (*mn, 0.55f);
    c.highest = std::max (*mx, 0.55f);
    return c;
}

int dither5 (int value, int x, int y) noexcept
{
    // The classic 4 x 4 Bayer matrix: thresholds spread evenly over a cell, so a flat colour between
    // two 5-bit levels becomes a fixed, fine pattern of both (and its average is the colour).
    static constexpr int bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    const int v = std::clamp (value, 0, 255);
    const double level = (double) v * 31.0 / 255.0;
    const double threshold = ((double) bayer[y & 3][x & 3] + 0.5) / 16.0;
    return std::clamp ((int) std::floor (level + threshold), 0, 31);
}

std::uint8_t expand5 (int value5) noexcept
{
    const int v = std::clamp (value5, 0, 31);
    return (std::uint8_t) ((v << 3) | (v >> 2));
}

//==============================================================================
void Raster::setSize (int width, int height)
{
    m_width = std::max (1, width);
    m_height = std::max (1, height);
    m_pixels.assign ((std::size_t) (m_width * m_height), c_void);
}

void Raster::set (int x, int y, Rgb c) noexcept
{
    if (x >= 0 && y >= 0 && x < m_width && y < m_height)
        m_pixels[(std::size_t) (y * m_width + x)] = c;
}

void Raster::triangle (const Vertex& a, const Vertex& b0, const Vertex& c0, const Material& m) noexcept
{
    const auto edge = [] (const Vertex& p, const Vertex& q, int x, int y) noexcept
    {
        return (std::int64_t) (q.x - p.x) * (y - p.y) - (std::int64_t) (q.y - p.y) * (x - p.x);
    };

    const Vertex* b = &b0;
    const Vertex* c = &c0;
    auto area = edge (a, *b, c->x, c->y);
    if (area == 0)
        return;
    if (area < 0)
    {
        std::swap (b, c);
        area = -area;
    }

    const int minX = std::max (0, std::min ({ a.x, b->x, c->x })), maxX = std::min (m_width - 1, std::max ({ a.x, b->x, c->x }));
    const int minY = std::max (0, std::min ({ a.y, b->y, c->y })), maxY = std::min (m_height - 1, std::max ({ a.y, b->y, c->y }));
    if (minX > maxX || minY > maxY)
        return;

    const float inv = 1.0f / (float) area;
    const Rgb a0 = scaled (m.a, m.shade), a1 = scaled (m.b, m.shade);
    for (int y = minY; y <= maxY; ++y)
    {
        for (int x = minX; x <= maxX; ++x)
        {
            const auto w0 = edge (*b, *c, x, y), w1 = edge (*c, a, x, y), w2 = edge (a, *b, x, y);
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            // Screen-space (affine) weights: exactly what makes the checker swim as it nears.
            const float l0 = (float) w0 * inv, l1 = (float) w1 * inv, l2 = (float) w2 * inv;
            const float fog = l0 * a.fog + l1 * b->fog + l2 * c->fog;
            Rgb colour = a0;
            if (m.checker)
            {
                const float u = l0 * a.u + l1 * b->u + l2 * c->u;
                const float v = l0 * a.v + l1 * b->v + l2 * c->v;
                if ((((int) std::floor (u) + (int) std::floor (v)) & 1) != 0)
                    colour = a1;
            }
            m_pixels[(std::size_t) (y * m_width + x)] = mix (colour, c_void, fog);
        }
    }
}

void Raster::line (int x0, int y0, float fog0, int x1, int y1, float fog1, Rgb colour) noexcept
{
    const int dx = std::abs (x1 - x0), dy = -std::abs (y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    const int steps = std::max (dx, -dy);
    int err = dx + dy, i = 0;
    for (;;)
    {
        const float t = steps > 0 ? (float) i / (float) steps : 0.0f;
        set (x0, y0, mix (colour, c_void, fog0 + (fog1 - fog0) * t));
        if (x0 == x1 && y0 == y1)
            break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        ++i;
    }
}

//==============================================================================
Camera cameraFor (const Raster& r, const Scene& scene) noexcept
{
    Camera cam;
    cam.cx = (float) r.width() * 0.5f;
    cam.cy = std::round ((float) r.height() * c_horizon);
    cam.focal = (float) r.width() * 0.5f;
    cam.x = c_camX;
    cam.z = (float) scene.focusSeconds * kUnitsPerSecond - c_camBack;
    // The camera rides the lane: the flattened sound stays level on screen while the heard pitch
    // climbs and falls beside it, which is the whole idea in one picture.
    float base = 0.0f;
    if (scene.course != nullptr && scene.course->valid)
        base = scene.course->hasLane ? Course::at (scene.course->lane, scene.course->stepSeconds, scene.focusSeconds)
                                     : 0.5f * (scene.course->lowest + scene.course->highest);
    cam.y = base + c_camLift;
    return cam;
}

void render (Raster& r, const Scene& scene)
{
    const int W = r.width(), H = r.height();
    const Camera cam = cameraFor (r, scene);
    const Course* course = scene.course != nullptr && scene.course->valid ? scene.course : nullptr;
    const float groundY = (course != nullptr ? course->lowest : -1.0f) - 1.2f;
    const int horizon = (int) cam.cy;

    // Sky: the void, lifting a little towards the horizon in dithered steps; then the ground plane
    // with its perspective grid, worked out per pixel (a row's depth is fixed on a level plane).
    const Rgb sky = mix (c_void, c_panel, 0.9f);
    const Rgb groundBase = mix (c_void, c_panel, 0.5f);
    const Rgb gridLine = mix (c_edge, c_dim, 0.25f);
    const float camHeight = cam.y - groundY;
    for (int y = 0; y < H; ++y)
    {
        if (y <= horizon)
        {
            const float t = (float) y / (float) std::max (1, horizon);
            const Rgb c = mix (c_void, sky, t * t);
            for (int x = 0; x < W; ++x)
                r.set (x, y, c);
            continue;
        }
        const float depth = cam.focal * camHeight / ((float) y - cam.cy);
        const float footprint = depth / cam.focal;      // world size of one pixel at this row
        const float fog = fogAt (depth);
        const float wz = cam.z + depth;
        const float fz = wz / 2.0f - std::floor (wz / 2.0f);
        const bool zLine = std::min (fz, 1.0f - fz) * 2.0f < footprint * 0.9f;
        for (int x = 0; x < W; ++x)
        {
            const float wx = cam.x + ((float) x + 0.5f - cam.cx) * footprint;
            const float fx = wx / 2.0f - std::floor (wx / 2.0f);
            const bool xLine = std::min (fx, 1.0f - fx) * 2.0f < footprint * 0.9f;
            r.set (x, y, mix (zLine || xLine ? gridLine : groundBase, c_void, fog));
        }
    }

    // A few star specks, drifting slowly as the camera travels.
    for (std::uint32_t i = 0; i < 26; ++i)
    {
        const auto h = hash (i * 7919u + 13u);
        const float drift = (float) (scene.clockSeconds * 1.5 + (double) cam.z * 0.4);
        const int x = (int) std::fmod ((float) (h % 1000u) / 1000.0f * (float) W + drift * (0.3f + (float) ((h >> 10) % 7u) * 0.1f), (float) W);
        const int y = (int) ((float) ((h >> 20) % 1000u) / 1000.0f * (float) (horizon - 3));
        r.set (x, y, ((h >> 4) & 3u) == 0u ? c_text : mix (c_dim, c_void, 0.3f));
    }

    // The horizon line.
    for (int x = 0; x < W; ++x)
        r.set (x, horizon, mix (c_edge, c_dim, 0.35f));

    if (course == nullptr)
        return;

    // Slices from the far end to the camera (painter's order: nearer draws over farther).
    const double step = course->stepSeconds;
    const float zps = kUnitsPerSecond;
    const double firstSeconds = std::max (0.0, (double) ((cam.z + 0.6f) / zps));
    const double lastSeconds = std::min (course->seconds, (double) ((cam.z + c_viewDistance) / zps));
    const auto first = (std::ptrdiff_t) std::floor (firstSeconds / step);
    const auto last = (std::ptrdiff_t) std::ceil (lastSeconds / step);
    const double dartSeconds = std::clamp (scene.focusSeconds, 0.0, course->seconds);
    bool dartDrawn = false;

    using V = Raster::Vertex;
    const auto vertex = [&] (Vec3 p, float u, float v) -> V
    {
        const auto s = project (cam, p);
        return { s.x, s.y, u, v, fogAt (s.depth) };
    };
    const auto quad = [&] (Vec3 p00, Vec3 p10, Vec3 p11, Vec3 p01, float u0, float u1, float v0, float v1, const Raster::Material& m)
    {
        const V a = vertex (p00, u0, v0), b = vertex (p10, u1, v0), c = vertex (p11, u1, v1), d = vertex (p01, u0, v1);
        r.triangle (a, b, c, m);
        r.triangle (a, c, d, m);
    };
    const auto edgeLine = [&] (Vec3 p, Vec3 q, Rgb colour)
    {
        const auto a = project (cam, p), b = project (cam, q);
        if (a.inFront && b.inFront)
            r.line (a.x, a.y, fogAt (a.depth), b.x, b.y, fogAt (b.depth), colour);
    };

    const Raster::Material trackWall { mix (c_edge, c_void, 0.25f), {}, false, 1.0f };
    const Raster::Material laneWall { mix (c_void, c_lime, 0.22f), {}, false, 1.0f };

    const auto drawDart = [&]
    {
        // An original wedge: a nose, two tail tips and a fin, a few flat-shaded faces.
        const float z = (float) dartSeconds * zps;
        const float cx = 0.5f * (c_laneLeft + c_laneRight);
        const float y = (course->hasLane ? Course::at (course->lane, step, dartSeconds) : Course::at (course->track, step, dartSeconds)) + 0.07f;
        const Vec3 nose { cx, y + 0.03f, z + 0.4f }, tailL { cx - 0.26f, y, z - 0.22f }, tailR { cx + 0.26f, y, z - 0.22f };
        const Vec3 fin { cx, y + 0.18f, z - 0.17f }, belly { cx, y - 0.02f, z - 0.22f };
        // Its shadow on the lane, so it reads as riding it.
        const float laneY = y - 0.06f;
        quad ({ cx - 0.22f, laneY, z - 0.24f }, { cx + 0.22f, laneY, z - 0.24f }, { cx + 0.06f, laneY, z + 0.36f }, { cx - 0.06f, laneY, z + 0.36f },
              0, 0, 0, 0, { mix (c_void, c_lime, 0.05f), {}, false, 1.0f });
        r.triangle (vertex (nose, 0, 0), vertex (tailL, 0, 0), vertex (fin, 0, 0), { c_lime, {}, false, 0.95f });
        r.triangle (vertex (nose, 0, 0), vertex (fin, 0, 0), vertex (tailR, 0, 0), { c_lime, {}, false, 0.62f });
        r.triangle (vertex (tailL, 0, 0), vertex (belly, 0, 0), vertex (fin, 0, 0), { mix (c_lime, c_void, 0.55f), {}, false, 1.0f });
        r.triangle (vertex (belly, 0, 0), vertex (tailR, 0, 0), vertex (fin, 0, 0), { mix (c_lime, c_void, 0.7f), {}, false, 1.0f });
        // The exhaust: one hot pixel at the back.
        const auto e = project (cam, { cx, y + 0.05f, z - 0.23f });
        r.set (e.x, e.y, c_text);
        r.set (e.x + 1, e.y, c_text);
    };

    for (auto i = last - 1; i >= first; --i)
    {
        const double t0 = (double) i * step, t1 = (double) (i + 1) * step;
        if (t0 > course->seconds)
            continue;
        const float z0 = (float) t0 * zps, z1 = (float) t1 * zps;
        const float h0 = Course::at (course->track, step, t0), h1 = Course::at (course->track, step, t1);

        // The dart sits in this slice: draw it before the nearer slices.
        if (! dartDrawn && dartSeconds >= t1)
        {
            drawDart();
            dartDrawn = true;
        }

        // Track: side wall facing the camera, then the checkered top, flat-shaded by its slope
        // (a climb faces the camera and catches the light, a fall turns away from it).
        const float slope = (h1 - h0) / (z1 - z0);
        const float shade = std::clamp (1.0f + slope * 2.2f, 0.55f, 1.5f);
        quad ({ c_trackRight, h0, z0 }, { c_trackRight, h0 - c_skirt, z0 }, { c_trackRight, h1 - c_skirt, z1 }, { c_trackRight, h1, z1 }, 0, 0, 0, 0, trackWall);
        quad ({ c_trackLeft, h0, z0 }, { c_trackRight, h0, z0 }, { c_trackRight, h1, z1 }, { c_trackLeft, h1, z1 },
              0.0f, 3.0f, z0 / c_checkLength, z1 / c_checkLength,
              { mix (c_edge, c_dim, 0.18f), mix (c_void, c_panel, 0.8f), true, shade });
        edgeLine ({ c_trackLeft, h0, z0 }, { c_trackLeft, h1, z1 }, c_cyan);
        edgeLine ({ c_trackRight, h0, z0 }, { c_trackRight, h1, z1 }, c_cyan);

        if (course->hasLane)
        {
            const float l0 = Course::at (course->lane, step, t0), l1 = Course::at (course->lane, step, t1);
            const float laneShade = std::clamp (1.0f + (l1 - l0) / (z1 - z0) * 2.2f, 0.55f, 1.5f);
            quad ({ c_laneLeft, l0, z0 }, { c_laneLeft, l0 - c_skirt, z0 }, { c_laneLeft, l1 - c_skirt, z1 }, { c_laneLeft, l1, z1 }, 0, 0, 0, 0, laneWall);
            quad ({ c_laneLeft, l0, z0 }, { c_laneRight, l0, z0 }, { c_laneRight, l1, z1 }, { c_laneLeft, l1, z1 },
                  0.0f, 2.0f, z0 / c_checkLength, z1 / c_checkLength,
                  { mix (c_void, c_lime, 0.24f), mix (c_void, c_lime, 0.1f), true, laneShade });
            edgeLine ({ c_laneLeft, l0, z0 }, { c_laneLeft, l1, z1 }, c_lime);
            edgeLine ({ c_laneRight, l0, z0 }, { c_laneRight, l1, z1 }, c_lime);
        }

        // A gate across the track at each whole second.
        const int second = (int) std::floor (t1);
        if (second >= 1 && (double) second > t0 && (double) second <= t1 && (double) second < course->seconds)
        {
            const float zg = (float) second * zps;
            const float hg = Course::at (course->track, step, (double) second);
            const Raster::Material post { mix (c_dim, c_edge, 0.2f), {}, false, 1.0f };
            const float l = c_trackLeft - 0.06f, rr = c_trackRight + 0.06f, top = hg + c_gateHeight;
            quad ({ l, hg, zg }, { l + 0.1f, hg, zg }, { l + 0.1f, top, zg }, { l, top, zg }, 0, 0, 0, 0, post);
            quad ({ rr - 0.1f, hg, zg }, { rr, hg, zg }, { rr, top, zg }, { rr - 0.1f, top, zg }, 0, 0, 0, 0, post);
            quad ({ l, top - 0.1f, zg }, { rr, top - 0.1f, zg }, { rr, top, zg }, { l, top, zg }, 0, 0, 0, 0, post);
            const auto label = project (cam, { 0.5f * (l + rr), top, zg });
            if (label.inFront && label.depth < 16.0f)
                drawNumber (r, second, label.x, label.y - (label.depth < 7.0f ? 12 : 7), label.depth < 7.0f ? 2 : 1,
                            mix (c_text, c_void, fogAt (label.depth)));
        }
    }
    if (! dartDrawn)
        drawDart();
}
} // namespace flight

//==============================================================================
FlightView::FlightView (const Hud::Clock& clock)
    : m_clock (clock)
    , m_demo (flight::demoCourse())
{
    setOpaque (false);   // drawn through the editor's framebuffer, which must repaint under it
}

void FlightView::setView (std::shared_ptr<const pitchflattener::Result> result, double playSeconds)
{
    if (result != m_result)
    {
        m_result = std::move (result);
        m_course = {};
        if (m_result != nullptr && m_result->learned != nullptr)
        {
            const auto& analysis = m_result->learned->analysis;
            const auto* line = pitchflattener::core::lineFor (analysis, m_result->options);
            const bool flattened = m_result->playable() && line != nullptr;
            if (line == nullptr)
                for (const auto& l : analysis.lines)   // nothing usable: fly whatever was read
                    if (! l.midi.empty())
                    {
                        line = &l;
                        break;
                    }
            if (line != nullptr)
                m_course = flight::buildCourse (*line, flattened, m_result->flattened.targetMidi, m_result->options.amount);
        }
    }
    m_playSeconds = playSeconds;
}

void FlightView::resized()
{
    const int w = (getWidth() + kPixelScale - 1) / kPixelScale, h = (getHeight() + kPixelScale - 1) / kPixelScale;
    m_raster.setSize (w, h);
    m_image = juce::Image (juce::Image::RGB, std::max (1, w), std::max (1, h), true);
}

void FlightView::renderFrame()
{
    if (m_image.isNull())
        return;

    const auto start = std::chrono::steady_clock::now();
    const double now = m_clock.now();
    const auto& course = m_course.valid ? m_course : m_demo;

    // Playing: follow the play head. Otherwise cruise along at half speed and loop (the demo
    // course at full speed, short of its end).
    if (m_course.valid && m_playSeconds >= 0.0)
        m_focus = m_playSeconds;
    else if (m_course.valid)
        m_focus = std::fmod (now * 0.5, std::max (0.5, course.seconds));
    else
        m_focus = std::fmod (now, course.seconds - 6.0);

    flight::Scene scene;
    scene.course = &course;
    scene.focusSeconds = m_focus;
    scene.clockSeconds = now;
    flight::render (m_raster, scene);

    // Down to 5 bits a channel through the ordered dither, into the image.
    {
        juce::Image::BitmapData data (m_image, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < m_raster.height() && y < data.height; ++y)
            for (int x = 0; x < m_raster.width() && x < data.width; ++x)
            {
                const auto& c = m_raster.at (x, y);
                const auto r = flight::expand5 (flight::dither5 ((int) std::lround (c.r * 255.0f), x, y));
                const auto g = flight::expand5 (flight::dither5 ((int) std::lround (c.g * 255.0f), x, y));
                const auto b = flight::expand5 (flight::dither5 ((int) std::lround (c.b * 255.0f), x, y));
                data.setPixelColour (x, y, juce::Colour (r, g, b));
            }
    }

    m_lastFrameMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
    repaint();
}

void FlightView::paint (juce::Graphics& g)
{
    using namespace Theme::Colours;
    if (Hud::skipOnScreen (*this))
        return;
    g.fillAll (space);
    // Its own pixels, already dithered on their coarser grid: the framebuffer passes them as they are.
    Hud::markRaw (g, getLocalBounds().toFloat());

    if (m_image.isValid() && ! Hud::blitRaw (g, m_image, kPixelScale))
    {
        // Nearest neighbour: the pixels are the point.
        juce::Graphics::ScopedSaveState save (g);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImageTransformed (m_image, juce::AffineTransform::scale ((float) kPixelScale));
    }

    // Crisp read-outs over the pixels, on a dark strip: the key, and where the flight is in the sound.
    g.setColour (space.withAlpha (0.78f));
    g.fillRect (getLocalBounds().toFloat().removeFromBottom (28.0f));
    g.setColour (panelEdge);
    g.fillRect (getLocalBounds().toFloat().removeFromBottom (28.0f).withHeight (1.0f));
    const auto bounds = getLocalBounds().toFloat().reduced (12.0f, 6.0f);
    const auto small = m_fonts->label (10.5f);
    auto key = bounds.withTrimmedTop (bounds.getHeight() - 16.0f);
    const auto swatch = [&] (juce::Colour c, const juce::String& words)
    {
        g.setColour (c);
        g.fillRect (key.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 4.0f));
        key.removeFromLeft (6.0f);
        const float w = Hud::textWidth (small, words) + 2.0f;
        Hud::drawText (g, words, key.removeFromLeft (w), small, c);
        key.removeFromLeft (18.0f);
    };
    const bool demo = ! m_course.valid;
    swatch (cyan, "PITCH AS HEARD");
    if (demo || m_course.hasLane)
        swatch (lime, "FLATTENED");

    juce::String where;
    if (demo)
        where = "DEMO RUN";
    else
        where = "T " + juce::String (m_focus, 2) + " / " + juce::String (m_course.seconds, 2) + " S";
    const float w = Hud::textWidth (small, where) + 2.0f;
    Hud::drawText (g, where, bounds.withTrimmedTop (bounds.getHeight() - 16.0f).withTrimmedLeft (bounds.getWidth() - w), small,
                   demo ? textDim : text.withAlpha (0.85f), juce::Justification::centredRight);
}

juce::String FlightView::getTooltip()
{
    return "Flight: the learned sound as a course. The camera travels along the sound; the track on the left rises and "
           "falls with its pitch as heard (cyan), and the lane on the right is the flattened sound (lime), level at the "
           "target. The dart rides it at the play head; a gate marks each second.";
}

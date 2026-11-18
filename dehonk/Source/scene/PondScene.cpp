// The JUCE side of the scene: PondWorld does the drawing into its own buffer, and this copies the
// result into the image the editor shows.
#include "PondScene.h"
#include "PondWorld.h"
#include "ToyPalette.h"

#include <utility>
#include <vector>

static_assert (! (dehonk::scene::PondScene::kAxisY < dehonk::scene::PondWorld::kBankRow) && ! (dehonk::scene::PondScene::kAxisY > dehonk::scene::PondWorld::kBankRow),
               "the header and the world must agree where the bank is");

namespace dehonk::scene
{

struct PondScene::Impl
{
    PondWorld world;
    double renderMs = 0.0;
    float pixelsPerPoint = 1.0f;
    std::vector<std::uint8_t> rows[3];     // three rows of the picture, each doubled across (see doubleInto)

    Impl()
    {
        world.setSize (PondScene::kWidth, PondScene::kHeight);
        // The ranges in the picture take the colours of their tabs in the window.
        auto of = [] (juce::Colour c) { return Rgb { (float) c.getRed(), (float) c.getGreen(), (float) c.getBlue() }; };
        const Rgb five[kNumModules + 1] = { of (toy::module[0]), of (toy::module[1]), of (toy::module[2]), of (toy::module[3]), of (toy::click) };
        world.setModuleColours (five);
    }

    /** One row of the picture at twice the width: each pixel becomes two, each three parts itself
        and one part its neighbour on that side. */
    static void doubleRow (const std::uint8_t* src, int w, std::uint8_t* out) noexcept
    {
        for (int c = 0; c < 4; ++c)
        {
            out[c] = src[c];
            out[(size_t) (8 * w - 4 + c)] = src[(size_t) (4 * w - 4 + c)];
        }
        std::uint8_t* o = out + 4;
        int x = 0;
        // Four pixels at a time (each with the one after it), the same sums in 16 bits.
       #if DEHONK_RASTER_NEON
        for (; x + 5 <= w; x += 4, src += 16, o += 32)
        {
            const uint8x16_t a = vld1q_u8 (src), b = vld1q_u8 (src + 4);
            auto half = [] (uint8x8_t p, uint8x8_t q)
            {
                const uint16x8_t pp = vmovl_u8 (p), qq = vmovl_u8 (q);
                return std::pair { vmovn_u16 (vrshrq_n_u16 (vmlaq_n_u16 (qq, pp, 3), 2)), vmovn_u16 (vrshrq_n_u16 (vmlaq_n_u16 (pp, qq, 3), 2)) };
            };
            const auto [nearLo, farLo] = half (vget_low_u8 (a), vget_low_u8 (b));
            const auto [nearHi, farHi] = half (vget_high_u8 (a), vget_high_u8 (b));
            const uint32x4x2_t z = vzipq_u32 (vreinterpretq_u32_u8 (vcombine_u8 (nearLo, nearHi)), vreinterpretq_u32_u8 (vcombine_u8 (farLo, farHi)));
            vst1q_u8 (o, vreinterpretq_u8_u32 (z.val[0]));
            vst1q_u8 (o + 16, vreinterpretq_u8_u32 (z.val[1]));
        }
       #elif DEHONK_RASTER_SSE2
        const __m128i zero = _mm_setzero_si128(), two = _mm_set1_epi16 (2);
        for (; x + 5 <= w; x += 4, src += 16, o += 32)
        {
            const __m128i a = _mm_loadu_si128 (reinterpret_cast<const __m128i*> (src)), b = _mm_loadu_si128 (reinterpret_cast<const __m128i*> (src + 4));
            auto mix = [&] (__m128i p, __m128i q) { return _mm_srli_epi16 (_mm_add_epi16 (_mm_add_epi16 (_mm_add_epi16 (p, _mm_slli_epi16 (p, 1)), q), two), 2); };
            const __m128i aLo = _mm_unpacklo_epi8 (a, zero), aHi = _mm_unpackhi_epi8 (a, zero), bLo = _mm_unpacklo_epi8 (b, zero), bHi = _mm_unpackhi_epi8 (b, zero);
            const __m128i nearPx = _mm_packus_epi16 (mix (aLo, bLo), mix (aHi, bHi)), farPx = _mm_packus_epi16 (mix (bLo, aLo), mix (bHi, aHi));
            _mm_storeu_si128 (reinterpret_cast<__m128i*> (o), _mm_unpacklo_epi32 (nearPx, farPx));
            _mm_storeu_si128 (reinterpret_cast<__m128i*> (o + 16), _mm_unpackhi_epi32 (nearPx, farPx));
        }
       #endif
        for (; x + 1 < w; ++x, src += 4, o += 8)
            for (int c = 0; c < 4; ++c)
            {
                const unsigned a = src[c], b = src[c + 4];
                o[c] = (std::uint8_t) ((3u * a + b + 2u) >> 2);
                o[c + 4] = (std::uint8_t) ((a + 3u * b + 2u) >> 2);
            }
    }

    /** `near` three parts, `far` one part. */
    static void blendRow (const std::uint8_t* near, const std::uint8_t* far, std::uint8_t* out, int bytes) noexcept
    {
        int i = 0;
       #if DEHONK_RASTER_NEON
        for (; i + 16 <= bytes; i += 16)
        {
            const uint8x16_t n = vld1q_u8 (near + i), f = vld1q_u8 (far + i);
            const uint16x8_t lo = vmlaq_n_u16 (vmovl_u8 (vget_low_u8 (f)), vmovl_u8 (vget_low_u8 (n)), 3);
            const uint16x8_t hi = vmlaq_n_u16 (vmovl_u8 (vget_high_u8 (f)), vmovl_u8 (vget_high_u8 (n)), 3);
            vst1q_u8 (out + i, vcombine_u8 (vmovn_u16 (vrshrq_n_u16 (lo, 2)), vmovn_u16 (vrshrq_n_u16 (hi, 2))));
        }
       #elif DEHONK_RASTER_SSE2
        const __m128i zero = _mm_setzero_si128(), two = _mm_set1_epi16 (2);
        for (; i + 16 <= bytes; i += 16)
        {
            const __m128i n = _mm_loadu_si128 (reinterpret_cast<const __m128i*> (near + i)), f = _mm_loadu_si128 (reinterpret_cast<const __m128i*> (far + i));
            auto mix = [&] (__m128i p, __m128i q) { return _mm_srli_epi16 (_mm_add_epi16 (_mm_add_epi16 (_mm_add_epi16 (p, _mm_slli_epi16 (p, 1)), q), two), 2); };
            _mm_storeu_si128 (reinterpret_cast<__m128i*> (out + i), _mm_packus_epi16 (mix (_mm_unpacklo_epi8 (n, zero), _mm_unpacklo_epi8 (f, zero)),
                                                                                       mix (_mm_unpackhi_epi8 (n, zero), _mm_unpackhi_epi8 (f, zero))));
        }
       #endif
        for (; i < bytes; ++i)
            out[i] = (std::uint8_t) ((3u * near[i] + far[i] + 2u) >> 2);
    }

    /** The world's picture at twice the size each way, smoothly, straight into the image. */
    void doubleInto (juce::Image::BitmapData& data)
    {
        const int w = world.width(), h = world.height(), bytes = 8 * w;
        for (auto& r : rows)
            if ((int) r.size() != bytes)
                r.assign ((size_t) bytes, 255);
        const std::uint8_t* src = world.pixels();
        std::uint8_t* prev = rows[0].data(), * here = rows[1].data(), * next = rows[2].data();
        doubleRow (src, w, here);
        std::copy_n (here, bytes, prev);
        for (int y = 0; y < h; ++y)
        {
            if (y + 1 < h)
                doubleRow (src + (size_t) (y + 1) * (size_t) w * 4, w, next);
            else
                std::copy_n (here, bytes, next);
            blendRow (here, prev, data.getLinePointer (2 * y), bytes);
            blendRow (here, next, data.getLinePointer (2 * y + 1), bytes);
            std::uint8_t* t = prev; prev = here; here = next; next = t;
        }
    }
};

PondScene::PondScene() : impl (std::make_unique<Impl>()) {}
PondScene::~PondScene() = default;

void PondScene::advance (const SceneInput& in, double seconds)
{
    // Nothing the core sends should ever be out of range, but one bad number must not stick in
    // the picture: anything that is not finite becomes 0 (or silence for a level), and the rest
    // is kept to sensible ranges.
    auto clean = [] (float v, float lo, float hi, float bad) { return std::isfinite (v) ? juce::jlimit (lo, hi, v) : bad; };
    WorldInput w;
    w.frame = in.frame;
    auto& f = w.frame;
    f.numBands = juce::jlimit (0, kMaxBands, f.numBands);
    for (int k = 0; k < f.numBands; ++k)
    {
        f.bandHz[k] = clean (f.bandHz[k], 0.0f, 48000.0f, 0.0f);
        f.levelDb[k] = clean (f.levelDb[k], -120.0f, 24.0f, -120.0f);
        f.cutDb[k] = clean (f.cutDb[k], 0.0f, 24.0f, 0.0f);
    }
    f.totalCutDb = clean (f.totalCutDb, 0.0f, 24.0f, 0.0f);
    for (auto& c : f.moduleCutDb)
        c = clean (c, 0.0f, 24.0f, 0.0f);
    f.snapCutDb = clean (f.snapCutDb, 0.0f, 48.0f, 0.0f);
    f.keyDb = clean (f.keyDb, -120.0f, 24.0f, -120.0f);
    f.programDb = clean (f.programDb, -120.0f, 24.0f, -120.0f);
    f.wash = clean (f.wash, 0.0f, 1.0f, 0.0f);
    f.activity = clean (f.activity, 0.0f, 1.0f, 0.0f);
    f.guard = clean (f.guard, 0.0f, 1.0f, 0.0f);
    for (int m = 0; m < kNumModules; ++m)
        w.moduleOn[m] = in.moduleOn[m];
    for (int e = 0; e < kNumEdges; ++e)
        w.edgeHz[e] = clean (in.edgeHz[e], 0.0f, 48000.0f, 1000.0f);
    w.snapOn = in.snapOn;
    w.maxCutDb = clean (in.maxCutDb, 0.0f, 48.0f, 6.0f);
    w.guardAmount = clean (in.guardAmount, 0.0f, 1.0f, 1.0f);
    w.soloHonk = in.soloHonk;
    w.diff = in.diff;
    w.bypass = in.bypass;
    w.calm = in.calm;
    w.focus = in.focusModule >= 0 && in.focusModule <= kNumModules ? in.focusModule : -1;
    w.axisLo = clean (in.axisLeft, 0.0f, 0.4f, 0.0f);
    w.axisHi = clean (in.axisRight, 0.6f, 1.0f, 1.0f);
    impl->world.advance (w, (float) juce::jlimit (0.0, 0.25, seconds));
}

void PondScene::render (juce::Image& target, Fit fit, float pixelsPerPoint)
{
    const int tw = target.getWidth(), th = target.getHeight();
    const int w = fit == Fit::doubled ? tw / 2 : fit == Fit::halved ? tw * 2 : tw;
    const int h = fit == Fit::doubled ? th / 2 : fit == Fit::halved ? th * 2 : th;
    if (w < 160 || h < 90 || target.getFormat() != juce::Image::ARGB || (fit == Fit::doubled && (w * 2 != tw || h * 2 != th)))
    {
        jassertfalse;      // the host must hand in an ARGB image the scene comes to at least 160 x 90 for (an even size, to be doubled)
        return;
    }
    if (impl->world.width() != w || impl->world.height() != h || std::abs (impl->pixelsPerPoint - pixelsPerPoint) > 1.0e-3f)
    {
        impl->pixelsPerPoint = pixelsPerPoint;
        impl->world.setSize (w, h, pixelsPerPoint);
    }

    const double start = juce::Time::getMillisecondCounterHiRes();
    impl->world.render();

    // The world's pixels are already laid out as an opaque ARGB image's are.
    juce::Image::BitmapData data (target, juce::Image::BitmapData::writeOnly);
    if (data.pixelStride == 4)
    {
        const std::uint8_t* src = impl->world.pixels();
        if (fit == Fit::doubled)
            impl->doubleInto (data);
        else if (fit == Fit::halved)
        {
            // Each pixel of the image is the mean of four of the scene's: its edges come out smooth.
            for (int y = 0; y < th; ++y)
            {
                const std::uint8_t* a = src + (size_t) (2 * y) * (size_t) w * 4;
                const std::uint8_t* b = a + (size_t) w * 4;
                std::uint8_t* o = data.getLinePointer (y);
                int x = 0;
               #if DEHONK_RASTER_NEON
                // Four pixels of the image at a time, from eight of the scene's in each of two rows.
                for (; x + 4 <= tw; x += 4, a += 32, b += 32, o += 16)
                {
                    const uint8x16_t a0 = vld1q_u8 (a), a1 = vld1q_u8 (a + 16), b0 = vld1q_u8 (b), b1 = vld1q_u8 (b + 16);
                    auto two = [] (uint8x16_t p, uint8x16_t q)
                    {
                        const uint16x8_t lo = vaddl_u8 (vget_low_u8 (p), vget_low_u8 (q)), hi = vaddl_u8 (vget_high_u8 (p), vget_high_u8 (q));
                        return vcombine_u16 (vadd_u16 (vget_low_u16 (lo), vget_high_u16 (lo)), vadd_u16 (vget_low_u16 (hi), vget_high_u16 (hi)));
                    };
                    const uint8x16_t px = vcombine_u8 (vmovn_u16 (vrshrq_n_u16 (two (a0, b0), 2)), vmovn_u16 (vrshrq_n_u16 (two (a1, b1), 2)));
                    vst1q_u8 (o, vreinterpretq_u8_u32 (vorrq_u32 (vreinterpretq_u32_u8 (px), vdupq_n_u32 (0xff000000u))));
                }
               #elif DEHONK_RASTER_SSE2
                const __m128i zero = _mm_setzero_si128(), two = _mm_set1_epi16 (2), opaque = _mm_set1_epi32 ((int) 0xff000000u);
                for (; x + 4 <= tw; x += 4, a += 32, b += 32, o += 16)
                {
                    auto pair = [&] (const std::uint8_t* p, const std::uint8_t* q)
                    {
                        const __m128i pv = _mm_loadu_si128 (reinterpret_cast<const __m128i*> (p)), qv = _mm_loadu_si128 (reinterpret_cast<const __m128i*> (q));
                        const __m128i lo = _mm_add_epi16 (_mm_unpacklo_epi8 (pv, zero), _mm_unpacklo_epi8 (qv, zero));   // pixels 0, 1 (rows summed)
                        const __m128i hi = _mm_add_epi16 (_mm_unpackhi_epi8 (pv, zero), _mm_unpackhi_epi8 (qv, zero));   // pixels 2, 3
                        const __m128i sum = _mm_add_epi16 (_mm_unpacklo_epi64 (lo, hi), _mm_unpackhi_epi64 (lo, hi));   // 0+1, 2+3
                        return _mm_srli_epi16 (_mm_add_epi16 (sum, two), 2);
                    };
                    const __m128i px = _mm_packus_epi16 (pair (a, b), pair (a + 16, b + 16));
                    _mm_storeu_si128 (reinterpret_cast<__m128i*> (o), _mm_or_si128 (px, opaque));
                }
               #endif
                for (; x < tw; ++x, a += 8, b += 8, o += 4)
                {
                    o[0] = (std::uint8_t) (((unsigned) a[0] + a[4] + b[0] + b[4] + 2u) >> 2);
                    o[1] = (std::uint8_t) (((unsigned) a[1] + a[5] + b[1] + b[5] + 2u) >> 2);
                    o[2] = (std::uint8_t) (((unsigned) a[2] + a[6] + b[2] + b[6] + 2u) >> 2);
                    o[3] = 255;
                }
            }
        }
        else
            for (int y = 0; y < h; ++y, src += (size_t) w * 4)
                std::copy_n (src, (size_t) w * 4, data.getLinePointer (y));
    }
    impl->renderMs = juce::Time::getMillisecondCounterHiRes() - start;
}

bool PondScene::somethingAt (float x01, float y01) const noexcept
{
    return impl->world.hitTest (x01 * (float) impl->world.width(), y01 * (float) impl->world.height()) != PondWorld::Hot::none;
}

PondScene::Hits PondScene::hits() const noexcept
{
    Hits h;
    const auto& w = impl->world;
    h.count = std::min ((int) h.spots.size(), w.hotSpotCount());
    for (int i = 0; i < h.count; ++i)
    {
        auto& s = h.spots[(size_t) i];
        w.hotSpot (i, s.x, s.y, s.rx, s.ry, s.kind, s.index);
    }
    h.width = w.width();
    h.height = w.height();
    return h;
}

bool PondScene::Hits::somethingAt (float x01, float y01) const noexcept
{
    return PondWorld::pick (spots.data(), count, x01 * (float) width, y01 * (float) height) != PondWorld::Hot::none;
}

bool PondScene::click (float x01, float y01) noexcept
{
    return impl->world.click (x01 * (float) impl->world.width(), y01 * (float) impl->world.height());
}

std::array<PondScene::TreeTop, kNumEdges> PondScene::treeTops() const noexcept
{
    std::array<TreeTop, kNumEdges> out;
    const float w = (float) juce::jmax (1, impl->world.width());
    const auto& tops = impl->world.treeTops();
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = { tops[i].x / w, tops[i].y / w, tops[i].radius / w, juce::Colour (tops[i].r, tops[i].g, tops[i].b) };
    return out;
}

bool PondScene::isAnimating() const noexcept { return impl->world.isMoving(); }
bool PondScene::isRaining() const noexcept { return impl->world.isRaining(); }
float PondScene::xForHz (float hz) noexcept { return PondWorld::axis01ForHz (hz); }
float PondScene::hzForX (float x01) noexcept { return PondWorld::hzForAxis01 (x01); }
double PondScene::lastRenderMs() const noexcept { return impl->renderMs; }

} // namespace dehonk::scene

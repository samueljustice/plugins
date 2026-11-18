// The JUCE side of the scene: PondWorld does the drawing into its own buffer, and this copies the
// result into the image the editor shows.
#include "PondScene.h"
#include "PondWorld.h"

static_assert (! (dehonk::scene::PondScene::kAxisY < dehonk::scene::PondWorld::kBankRow) && ! (dehonk::scene::PondScene::kAxisY > dehonk::scene::PondWorld::kBankRow),
               "the header and the world must agree where the bank is");

namespace dehonk::scene
{

struct PondScene::Impl
{
    PondWorld world;
    double renderMs = 0.0;
    Impl() { world.setSize (PondScene::kWidth, PondScene::kHeight); }
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
    impl->world.advance (w, (float) juce::jlimit (0.0, 0.25, seconds));
}

void PondScene::render (juce::Image& target)
{
    const int w = target.getWidth(), h = target.getHeight();
    if (w < 160 || h < 90 || target.getFormat() != juce::Image::RGB)
    {
        jassertfalse;      // the host must hand in an RGB software image of at least 160 x 90
        return;
    }
    if (impl->world.width() != w || impl->world.height() != h)
        impl->world.setSize (w, h);

    const double start = juce::Time::getMillisecondCounterHiRes();
    impl->world.render();

    const std::uint8_t* src = impl->world.pixels();
    juce::Image::BitmapData data (target, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < h; ++y)
    {
        auto* line = data.getLinePointer (y);
        for (int x = 0; x < w; ++x)
        {
            ((juce::PixelRGB*) (line + x * data.pixelStride))->setARGB (255, src[0], src[1], src[2]);
            src += 3;
        }
    }
    impl->renderMs = juce::Time::getMillisecondCounterHiRes() - start;
}

bool PondScene::isAnimating() const noexcept { return impl->world.isMoving(); }
float PondScene::xForHz (float hz) noexcept { return PondWorld::x01ForHz (hz); }
float PondScene::hzForX (float x01) noexcept { return PondWorld::hzForX01 (x01); }
double PondScene::lastRenderMs() const noexcept { return impl->renderMs; }

} // namespace dehonk::scene

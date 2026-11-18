#include "Render.h"

#include <algorithm>
#include <chrono>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
 #include <xmmintrin.h>
#endif

namespace dh
{

namespace
{
const dehonk::Tuning* gTuningOverride = nullptr;
}

void setTuningOverride (const dehonk::Tuning* tuning) { gTuningOverride = tuning; }
const dehonk::Tuning* tuningOverride() { return gTuningOverride; }

void enableFlushDenormals()
{
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    std::uint64_t fpcr;
    asm volatile ("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ull << 24); // FZ
    asm volatile ("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    _mm_setcsr (_mm_getcsr() | 0x8040); // FTZ | DAZ
#endif
}

const char* moduleName (int module)
{
    static const char* const names[dehonk::kNumModules] = { "Woof", "Tub", "Fog", "Bark" };
    return module >= 0 && module < dehonk::kNumModules ? names[module] : "-";
}

std::array<float, dehonk::kNumEdges> clampedEdges (const dehonk::Params& p)
{
    std::array<float, dehonk::kNumEdges> e {};
    for (int i = 0; i < dehonk::kNumEdges; ++i)
        e[(std::size_t) i] = std::clamp (p.edgeHz[i], dehonk::kEdgeMinHz[i], dehonk::kEdgeMaxHz[i]);
    return e;
}

int moduleAt (double hz, const dehonk::Params& p)
{
    const auto e = clampedEdges (p);
    for (int m = 0; m < dehonk::kNumModules; ++m)
        if (hz >= e[(std::size_t) m] && hz < e[(std::size_t) m + 1])
            return m;
    return -1;
}

RenderResult render (const Audio& in, const RenderSpec& spec)
{
    dehonk::Core core;
    return render (core, in, spec);
}

RenderResult render (dehonk::Core& core, const Audio& in, const RenderSpec& spec)
{
    using Clock = std::chrono::steady_clock;
    const int nch = in.numChannels();
    const std::size_t n = in.length();
    const int maxBlock = std::max (1, spec.maxBlockSize > 0 ? spec.maxBlockSize : spec.blockSize);

    if (spec.useLayout)
        core.prepare (in.sampleRate, maxBlock, nch, spec.layout);
    else
        core.prepare (in.sampleRate, maxBlock, nch);
    if (spec.useTuning)
        core.setTuning (spec.tuning);
    else if (gTuningOverride != nullptr)
        core.setTuning (*gTuningOverride);
    dehonk::Params p = spec.params;
    core.setParams (p);
    core.reset();
    if (! spec.staticCuts.empty())
        core.debugSetStaticCuts (spec.staticCuts.data());

    RenderResult r;
    r.latency = core.latencySamples();
    r.numBands = core.numBands();
    for (int b = 0; b < r.numBands; ++b)
        r.bandHz.push_back (core.bandHz (b));
    r.out = Audio::make (in.sampleRate, nch, n);

    const std::size_t L = spec.compensateLatency ? (std::size_t) std::max (0, r.latency) : 0;
    const std::size_t total = n + L;

    std::vector<std::vector<float>> work ((std::size_t) nch, std::vector<float> ((std::size_t) maxBlock, 0.0f));
    std::vector<float*> ptrs ((std::size_t) nch), sub ((std::size_t) nch);
    for (int c = 0; c < nch; ++c)
        ptrs[(std::size_t) c] = work[(std::size_t) c].data();

    if (spec.timeBlocks)
    {
        const std::size_t est = total / (std::size_t) std::max (1, spec.blockSize) + 16;
        r.blockSeconds.reserve (est);
        r.blockSizes.reserve (est);
        r.blockStarts.reserve (est);
    }

    std::uint32_t lastCounter = core.frameCounter();
    std::size_t pos = 0;
    while (pos < total)
    {
        int b = spec.nextBlockSize ? spec.nextBlockSize() : spec.blockSize;
        b = std::clamp (b, 1, maxBlock);
        b = (int) std::min<std::size_t> ((std::size_t) b, total - pos);

        if (spec.automate)
        {
            spec.automate ((std::int64_t) pos, p);
            core.setParams (p);
        }

        for (int c = 0; c < nch; ++c)
        {
            float* w = work[(std::size_t) c].data();
            const auto& src = in.ch[(std::size_t) c];
            for (int i = 0; i < b; ++i)
            {
                const std::size_t idx = pos + (std::size_t) i;
                w[i] = idx < n ? src[idx] : 0.0f;
            }
        }

        auto readFrame = [&] (std::size_t endPos) {
            if (! spec.collectFrames || core.frameCounter() == lastCounter)
                return;
            lastCounter = core.frameCounter();
            const auto& df = core.displayFrame();
            Frame f;
            f.timeSec = (double) std::min (endPos, n) / in.sampleRate;
            f.counter = lastCounter;
            f.totalCutDb = df.totalCutDb;
            f.programDb = df.programDb;
            f.activity = df.activity;
            f.snapCutDb = df.snapCutDb;
            f.diffOffsetDb = df.diffOffsetDb;
            f.guard = df.guard;
            for (int k = 0; k < 12; ++k)
                f.pitchClass[(std::size_t) k] = df.pitchClass[k];
            const int nb = std::clamp (df.numBands, 0, dehonk::kMaxBands);
            for (int k = 0; k < nb; ++k)
            {
                f.cutDb[(std::size_t) k] = df.cutDb[k];
                f.levelDb[(std::size_t) k] = df.levelDb[k];
            }
            for (int m = 0; m < dehonk::kNumModules; ++m)
                f.moduleCutDb[(std::size_t) m] = df.moduleCutDb[m];
            r.frames.push_back (f);
        };

        if (spec.timeBlocks)
        {
            const auto t0 = Clock::now();
            core.process (ptrs.data(), nch, b);
            const auto t1 = Clock::now();
            r.blockSeconds.push_back (std::chrono::duration<double> (t1 - t0).count());
            r.blockSizes.push_back (b);
            r.blockStarts.push_back ((std::int64_t) pos);
        }
        else if (spec.frameEveryTick && spec.collectFrames)
        {
            for (int done = 0; done < b;)
            {
                const int m = std::min (kFrameSubBlock, b - done);
                for (int c = 0; c < nch; ++c)
                    sub[(std::size_t) c] = ptrs[(std::size_t) c] + done;
                core.process (sub.data(), nch, m);
                done += m;
                readFrame (pos + (std::size_t) done);
            }
        }
        else
        {
            core.process (ptrs.data(), nch, b);
        }

        for (int c = 0; c < nch; ++c)
        {
            const float* w = work[(std::size_t) c].data();
            auto& dst = r.out.ch[(std::size_t) c];
            for (int i = 0; i < b; ++i)
            {
                const std::size_t idx = pos + (std::size_t) i;
                if (idx >= L && idx - L < n)
                    dst[idx - L] = w[i];
            }
        }
        pos += (std::size_t) b;
        readFrame (pos);
    }
    return r;
}

Audio streamThrough (dehonk::Core& core, const Audio& in, int blockSize)
{
    Audio out = in;
    const int nch = in.numChannels();
    std::vector<float*> ptrs ((std::size_t) nch);
    for (std::size_t pos = 0; pos < in.length(); pos += (std::size_t) blockSize)
    {
        const int b = (int) std::min<std::size_t> ((std::size_t) blockSize, in.length() - pos);
        for (int c = 0; c < nch; ++c)
            ptrs[(std::size_t) c] = out.ch[(std::size_t) c].data() + pos;
        core.process (ptrs.data(), nch, b);
    }
    return out;
}

} // namespace dh

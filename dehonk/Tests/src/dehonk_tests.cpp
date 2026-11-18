// dehonk_tests: self-contained tests for the De-Honk DSP core (dehonk/SPEC.md section 7).
//   dehonk_tests [--list] [--only NAME[,NAME...]] [--report out.md] [--strict-cpu] [--no-ftz]
//                [--write-signals DIR]
// Every signal is generated here from fixed seeds. Exit code is 1 if any test fails.

#include "Analysis.h"
#include "Audio.h"
#include "Fft.h"
#include "GoldenCases.h"
#include "Measures.h"
#include "Render.h"
#include "Signals.h"
#include "TuningFields.h"
#include "Wav.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <new>
#include <numbers>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace dh;

//==============================================================================
// Counting replacements for the global allocation functions, so no_allocation_in_process can
// see whether the core allocates (or frees) inside process(). Every form the core could reach is
// replaced: plain, array, aligned and nothrow. Allocations through malloc directly are not seen.

namespace
{
std::atomic<long long> gAllocations { 0 }, gDeallocations { 0 };

void* countedAlloc (std::size_t size, std::size_t align, bool nothrow)
{
    gAllocations.fetch_add (1, std::memory_order_relaxed);
    if (size == 0)
        size = 1;
    void* p = nullptr;
    if (align <= alignof (std::max_align_t))
        p = std::malloc (size);
    else
    {
       #if defined (_WIN32)
        p = _aligned_malloc (size, align);    // Windows has no posix_memalign; freed with _aligned_free
       #else
        if (posix_memalign (&p, align, size) != 0)
            p = nullptr;
       #endif
    }
    if (p == nullptr && ! nothrow)
        throw std::bad_alloc();
    return p;
}

void countedFree (void* p, bool aligned = false) noexcept
{
    if (p == nullptr)
        return;
    gDeallocations.fetch_add (1, std::memory_order_relaxed);
   #if defined (_WIN32)
    if (aligned)
    {
        _aligned_free (p);
        return;
    }
   #else
    (void) aligned;
   #endif
    std::free (p);
}
} // namespace

void* operator new (std::size_t n) { return countedAlloc (n, 0, false); }
void* operator new[] (std::size_t n) { return countedAlloc (n, 0, false); }
void* operator new (std::size_t n, std::align_val_t a) { return countedAlloc (n, (std::size_t) a, false); }
void* operator new[] (std::size_t n, std::align_val_t a) { return countedAlloc (n, (std::size_t) a, false); }
void* operator new (std::size_t n, const std::nothrow_t&) noexcept { return countedAlloc (n, 0, true); }
void* operator new[] (std::size_t n, const std::nothrow_t&) noexcept { return countedAlloc (n, 0, true); }
void* operator new (std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return countedAlloc (n, (std::size_t) a, true); }
void* operator new[] (std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return countedAlloc (n, (std::size_t) a, true); }
void operator delete (void* p) noexcept { countedFree (p); }
void operator delete[] (void* p) noexcept { countedFree (p); }
void operator delete (void* p, std::size_t) noexcept { countedFree (p); }
void operator delete[] (void* p, std::size_t) noexcept { countedFree (p); }
void operator delete (void* p, std::align_val_t) noexcept { countedFree (p, true); }
void operator delete[] (void* p, std::align_val_t) noexcept { countedFree (p, true); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { countedFree (p, true); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { countedFree (p, true); }
void operator delete (void* p, const std::nothrow_t&) noexcept { countedFree (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { countedFree (p); }
void operator delete (void* p, std::align_val_t, const std::nothrow_t&) noexcept { countedFree (p, true); }
void operator delete[] (void* p, std::align_val_t, const std::nothrow_t&) noexcept { countedFree (p, true); }

namespace
{
constexpr double kSr = 48000.0;
constexpr double kPlusTwelveDb = 3.9810717; // +12 dBFS
// Threshold setting at which De-Honk always acts. The default threshold (-16 dBFS programme level)
// would gate most test signals off and make the tests trivially pass, so tests that judge the
// detector or the filters run with this; the threshold itself is tested in threshold,
// transparency_pink, automation_clicks and stability.
constexpr float kAlwaysDb = -60.0f;

// Where tests may write scratch files: the folder holding the executable (the build folder).
std::filesystem::path gScratchDir = ".";

std::size_t S (double seconds, double sr = kSr) { return (std::size_t) std::llround (seconds * sr); }

//==============================================================================
// Runner

struct Ctx
{
    std::string name;
    bool failed = false;
    std::string headline;
    std::vector<std::string> lines;
    bool strictCpu = false;

    void check (bool ok, const std::string& what)
    {
        lines.push_back (std::string (ok ? "  ok    " : "  FAIL  ") + what);
        std::printf ("%s\n", lines.back().c_str());
        std::fflush (stdout);
        if (! ok)
            failed = true;
    }
    void info (const std::string& what)
    {
        lines.push_back ("  info  " + what);
        std::printf ("%s\n", lines.back().c_str());
        std::fflush (stdout);
    }
};

struct TestDef
{
    const char* name;
    const char* what;
    void (*fn) (Ctx&);
};

//==============================================================================
// Helpers

/** The core's default tuning, or the one given with --set (see setTuningOverride). */
dehonk::Tuning defaultTuning()
{
    if (const auto* o = tuningOverride())
        return *o;
    dehonk::Core c;
    return c.tuning();
}

Audio fromChannels (double sr, std::vector<std::vector<float>> chans)
{
    Audio a;
    a.sampleRate = sr;
    a.ch = std::move (chans);
    return a;
}

Audio pinkAudio (double sr, std::size_t n, int nch, double rmsDbFs, std::uint64_t seed)
{
    Audio a = Audio::make (sr, nch, n);
    for (int c = 0; c < nch; ++c)
    {
        a.ch[(std::size_t) c] = pinkNoise (sr, n, seed + 101ull * (std::uint64_t) c);
        normaliseRms (a.ch[(std::size_t) c], rmsDbFs);
    }
    return a;
}

Audio musicAudio (double sr, std::size_t n, int nch, std::uint64_t seed)
{
    Audio a = Audio::make (sr, nch, n);
    for (int c = 0; c < nch; ++c)
        a.ch[(std::size_t) c] = musicLike (sr, n, seed + 13ull * (std::uint64_t) c);
    return a;
}

struct CutStats
{
    std::vector<double> mean, max;
    double meanTotal = 0.0, maxTotal = 0.0, p99Total = 0.0;
    std::size_t frames = 0;
};

CutStats cutStats (const RenderResult& r, double fromSec)
{
    CutStats s;
    const auto nb = (std::size_t) std::clamp (r.numBands, 0, dehonk::kMaxBands);
    s.mean.assign (nb, 0.0);
    s.max.assign (nb, 0.0);
    std::vector<double> totals;
    for (const auto& f : r.frames)
    {
        if (f.timeSec < fromSec)
            continue;
        totals.push_back (f.totalCutDb);
        for (std::size_t k = 0; k < nb; ++k)
        {
            s.mean[k] += f.cutDb[k];
            s.max[k] = std::max (s.max[k], (double) f.cutDb[k]);
        }
    }
    s.frames = totals.size();
    if (s.frames > 0)
    {
        for (auto& m : s.mean)
            m /= (double) s.frames;
        s.meanTotal = std::accumulate (totals.begin(), totals.end(), 0.0) / (double) s.frames;
        s.maxTotal = *std::max_element (totals.begin(), totals.end());
        s.p99Total = percentile (totals, 0.99);
    }
    return s;
}

int nearestBand (const std::vector<float>& bandHz, double hz)
{
    int best = 0;
    double bestD = 1e9;
    for (std::size_t k = 0; k < bandHz.size(); ++k)
    {
        const double d = std::abs (std::log2 (bandHz[k] / hz));
        if (d < bestD)
        {
            bestD = d;
            best = (int) k;
        }
    }
    return best;
}

/** Per core band: output level minus input level, own FFT analysis, a 1/12-octave band around
    each band centre (the core's own band spacing). */
std::vector<double> bandChanges (const Spectrum& in, const Spectrum& out, const std::vector<float>& bandHz)
{
    std::vector<double> d;
    for (float hz : bandHz)
        d.push_back (out.centredDb (hz, 1.0 / 24.0) - in.centredDb (hz, 1.0 / 24.0));
    return d;
}

std::string bandList (const std::vector<float>& bandHz, const std::vector<double>& v, double loHz = 0.0, double hiHz = 1e9)
{
    std::string s;
    for (std::size_t k = 0; k < bandHz.size() && k < v.size(); ++k)
        if (bandHz[k] >= loHz && bandHz[k] <= hiHz)
            s += fmt ("%s%.0f:%+.2f", s.empty() ? "" : " ", (double) bandHz[k], v[k]);
    return s;
}

//==============================================================================
// Helpers for the Snap tests. The harness measures with its own filters (Measures.h), not the core's.

constexpr double kClickLoHz = 1800.0, kClickHiHz = 4000.0;   // the band Snap works in (SPEC 12.8)

/** Params with the four notch modules off, so only Snap acts. */
dehonk::Params clickOnly (float depth = dehonk::Params {}.depth)
{
    dehonk::Params p;
    for (auto& on : p.moduleOn)
        on = false;
    p.snapOn = true;
    p.depth = depth;
    return p;
}

enum class BurstKind
{
    tone,        // Hann-windowed sine at hz, durMs long
    broadband,   // white noise, 1.5 ms exponential decay, durMs long
    snare,       // white noise, 30 ms exponential decay, durMs long
    kick         // 60 Hz sine falling from 150 Hz (15 ms), 150 ms decay, plus a 1 ms broadband tick
};

/** One burst, peak |x| = peak. */
std::vector<float> makeBurst (double sr, BurstKind kind, double hz, double durMs, double peak, Rng& rng)
{
    const std::size_t len = std::max<std::size_t> (1, S (durMs * 0.001, sr));
    std::vector<double> b (len, 0.0);
    const double ph = rng.uniform (0.0, 2.0 * std::numbers::pi);
    double phase = ph;
    for (std::size_t i = 0; i < len; ++i)
    {
        const double t = (double) i / sr;
        switch (kind)
        {
            case BurstKind::tone:
                b[i] = 0.5 * (1.0 - std::cos (2.0 * std::numbers::pi * ((double) i + 0.5) / (double) len)) * std::sin (2.0 * std::numbers::pi * hz * t + ph);
                break;
            case BurstKind::broadband: b[i] = rng.gauss() * std::exp (-t / 0.0015); break;
            case BurstKind::snare: b[i] = rng.gauss() * std::exp (-t / 0.030); break;
            case BurstKind::kick:
            {
                const double f = 60.0 + 90.0 * std::exp (-t / 0.015);
                phase += 2.0 * std::numbers::pi * f / sr;
                b[i] = std::sin (phase) * std::exp (-t / 0.150) * std::min (1.0, t / 0.0005);
                if (t < 0.001)
                    b[i] += 0.5 * rng.gauss() * (1.0 - t / 0.001);
                break;
            }
        }
    }
    double mx = 0.0;
    for (double v : b)
        mx = std::max (mx, std::abs (v));
    std::vector<float> out (len);
    for (std::size_t i = 0; i < len; ++i)
        out[i] = (float) (b[i] * peak / std::max (1e-30, mx));
    return out;
}

/** Bursts over a pink-noise bed. starts are where each burst begins, len its length. */
struct BurstSignal
{
    Audio in;
    std::vector<std::size_t> starts;
    std::size_t len = 0;
};

/** Pink bed at bedDb RMS (independent noise per channel) plus the same burst in every channel at
    each of the given times (or every periodSec from firstSec when times is empty). */
BurstSignal burstSignal (double sr, double seconds, int nch, BurstKind kind, double hz, double durMs, double peak, double bedDb,
                         std::uint64_t seed, double firstSec = 0.5, double periodSec = 0.5, std::vector<double> times = {})
{
    BurstSignal s;
    const std::size_t n = S (seconds, sr);
    s.in = pinkAudio (sr, n, nch, bedDb, seed);
    if (times.empty())
        for (double t = firstSec; t + 0.5 <= seconds; t += periodSec)
            times.push_back (t);
    Rng rng (seed + 7);
    for (double t : times)
    {
        const auto b = makeBurst (sr, kind, hz, durMs, peak, rng);
        const std::size_t at = S (t, sr);
        if (at + b.size() > n)
            continue;
        s.starts.push_back (at);
        s.len = b.size();
        for (auto& ch : s.in.ch)
            for (std::size_t i = 0; i < b.size(); ++i)
                ch[at + i] += b[i];
    }
    return s;
}

/** The click measures, as change from input to output in dB (negative = reduced): the peak of
    the 1.8 to 4 kHz band (harness's own band-pass) and of the whole signal within 8 ms of each
    burst, mean and extremes over the bursts; and the bed's level from 250 to 450 ms after each
    burst, over the whole signal and in the 1.8 to 4 kHz band alone. Channel 0. */
struct BurstChange
{
    double bandMean = 0.0, bandMin = 0.0, bandMax = 0.0;   // band peak change
    double fullMean = 0.0, fullMin = 0.0, fullMax = 0.0;   // whole-signal peak change
    double bedDb = 0.0;                                    // bed level change between bursts, whole signal
    double bedBandDb = 0.0;                                // the same in the 1.8 to 4 kHz band (the harness's band-pass)
    int bursts = 0;
};

BurstChange burstChange (const BurstSignal& s, const Audio& out, double fromSec = 0.4)
{
    const double sr = s.in.sampleRate;
    const auto bi = bandpass8 (s.in.ch[0], sr, kClickLoHz, kClickHiHz);
    const auto bo = bandpass8 (out.ch[0], sr, kClickLoHz, kClickHiHz);
    const auto& xi = s.in.ch[0];
    const auto& xo = out.ch[0];
    const std::size_t n = xi.size(), w = S (0.008, sr);
    BurstChange c;
    c.bandMin = c.fullMin = 1e9;
    c.bandMax = c.fullMax = -1e9;
    double bedIn = 0.0, bedOut = 0.0, bedBandIn = 0.0, bedBandOut = 0.0;
    for (std::size_t st : s.starts)
    {
        if (st < S (fromSec, sr) || st < w)
            continue;
        const std::size_t a = st - w, b = std::min (n, st + s.len + w);
        double pbi = 0.0, pbo = 0.0, pfi = 0.0, pfo = 0.0;
        for (std::size_t i = a; i < b; ++i)
        {
            pbi = std::max (pbi, std::abs (bi[i]));
            pbo = std::max (pbo, std::abs (bo[i]));
            pfi = std::max (pfi, (double) std::abs (xi[i]));
            pfo = std::max (pfo, (double) std::abs (xo[i]));
        }
        const double db = ampToDb (pbo / pbi), df = ampToDb (pfo / pfi);
        c.bandMean += db;
        c.bandMin = std::min (c.bandMin, db);
        c.bandMax = std::max (c.bandMax, db);
        c.fullMean += df;
        c.fullMin = std::min (c.fullMin, df);
        c.fullMax = std::max (c.fullMax, df);
        ++c.bursts;
        for (std::size_t i = st + S (0.25, sr); i < std::min (n, st + S (0.45, sr)); ++i)
        {
            bedIn += (double) xi[i] * xi[i];
            bedOut += (double) xo[i] * xo[i];
            bedBandIn += bi[i] * bi[i];
            bedBandOut += bo[i] * bo[i];
        }
    }
    if (c.bursts > 0)
    {
        c.bandMean /= c.bursts;
        c.fullMean /= c.bursts;
    }
    c.bedDb = powerToDb (bedOut / std::max (1e-300, bedIn));
    c.bedBandDb = powerToDb (bedBandOut / std::max (1e-300, bedBandIn));
    return c;
}

struct ClickCase
{
    const char* name;
    double hz, durMs, minDb;   // minDb: the least reduction wanted at default Depth
};
const ClickCase kClickCases[] = {
    { "2.5 kHz, 3 ms", 2500.0, 3.0, 4.0 },
    { "2.8 kHz, 1.2 ms", 2800.0, 1.2, 2.5 },
    { "3.5 kHz, 3 ms", 3500.0, 3.0, 2.5 },
};

/** The click signal: pink bed at -22 dBFS RMS, Hann-windowed bursts at peak 0.5 every half second. */
BurstSignal clickSignal (double sr, double hz, double durMs, int nch = 1, double seconds = 10.0, std::uint64_t seed = 401)
{
    return burstSignal (sr, seconds, nch, BurstKind::tone, hz, durMs, 0.5, -22.0, seed);
}

/** Level change in the click band (own band-pass), channel 0, over [from, to). */
double clickBandChangeDb (const Audio& in, const Audio& out, std::size_t from, std::size_t to)
{
    const auto bi = bandpass8 (in.ch[0], in.sampleRate, kClickLoHz, kClickHiHz);
    const auto bo = bandpass8 (out.ch[0], in.sampleRate, kClickLoHz, kClickHiHz);
    double ei = 0.0, eo = 0.0;
    for (std::size_t i = from; i < std::min (to, bi.size()); ++i)
    {
        ei += bi[i] * bi[i];
        eo += bo[i] * bo[i];
    }
    return powerToDb (eo / std::max (1e-300, ei));
}

/** Frame statistics for DisplayFrame::snapCutDb (render with frameEveryTick). */
struct ClickFrames
{
    double mean = 0.0, max = 0.0, pctOver = 0.0;
    int events = 0;   // runs of frames above 1 dB
};

ClickFrames clickFrames (const RenderResult& r, double overDb, double fromSec = 0.0)
{
    ClickFrames c;
    std::size_t nf = 0, over = 0;
    bool inEvent = false;
    for (const auto& f : r.frames)
    {
        if (f.timeSec < fromSec)
            continue;
        ++nf;
        c.mean += f.snapCutDb;
        c.max = std::max (c.max, (double) f.snapCutDb);
        over += f.snapCutDb > overDb ? 1 : 0;
        c.events += (f.snapCutDb > 1.0f && ! inEvent) ? 1 : 0;
        inEvent = f.snapCutDb > 1.0f;
    }
    if (nf > 0)
    {
        c.mean /= (double) nf;
        c.pctOver = 100.0 * (double) over / (double) nf;
    }
    return c;
}

//==============================================================================
// null_depth_zero

void testNullDepthZero (Ctx& t)
{
    const std::size_t n = S (4.0);
    double worst = -400.0;
    for (int sig = 0; sig < 2; ++sig)
        for (int nch = 1; nch <= 2; ++nch)
            for (float la : { 0.0f, 10.0f })
            {
                const Audio in = sig == 0 ? pinkAudio (kSr, n, nch, -18.0, 11) : musicAudio (kSr, n, nch, 12);
                RenderSpec s;
                s.params.thresholdDb = kAlwaysDb;   // at the default threshold the pink cases would never act
                s.params.depth = 0.0f;
                s.params.lookaheadMs = la;
                const auto r = render (in, s);
                const double res = residualDb (r.out, in, 1.0, 0, n);
                const double mad = maxAbsDiff (r.out, in, 1.0, 0, n);
                worst = std::max (worst, res);
                t.check (res < -100.0, fmt ("%-5s %s lookahead %2.0f ms (latency %d): residual %s dB, max |diff| %.3g (limit -100 dB)",
                                            sig == 0 ? "pink" : "music", nch == 1 ? "mono  " : "stereo", (double) la, r.latency,
                                            dbStr (res).c_str(), mad));
                // The same render at default Depth must change the signal, or Depth 0 proves nothing.
                RenderSpec d = s;
                d.params.depth = dehonk::Params {}.depth;
                d.collectFrames = true;
                const auto rd = render (in, d);
                const double resD = residualDb (rd.out, in, 1.0, 0, n);
                t.check (resD > -60.0 && cutStats (rd, 0.0).maxTotal > 0.5,
                         fmt ("%-5s %s lookahead %2.0f ms at default Depth: residual %s dB, largest cut %.2f dB (need above -60 dB and 0.5 dB: the signal is being treated)",
                              sig == 0 ? "pink" : "music", nch == 1 ? "mono  " : "stereo", (double) la, dbStr (resD).c_str(),
                              cutStats (rd, 0.0).maxTotal));
            }
    t.headline = "worst residual " + dbStr (worst) + " dB (limit -100 dB)";
}

//==============================================================================
// Helpers for the module and notch tests

dehonk::Params alwaysOn()
{
    dehonk::Params p;
    p.thresholdDb = kAlwaysDb;
    return p;
}

std::string moduleList (const dehonk::Params& p)
{
    std::string s;
    for (int m = 0; m < dehonk::kNumModules; ++m)
        if (p.moduleOn[m])
            s += std::string (s.empty() ? "" : "+") + moduleName (m);
    return s.empty() ? "no module" : s;
}

/** The module a band's notch is counted against: the module whose range holds the band if that
    module is on, otherwise the nearest module that is on (the top of a run of modules reaches a
    little above its edge, Tuning::highSlopeOctaves). -1 when no module is on. */
int countingModule (double hz, const dehonk::Params& p)
{
    const int m = moduleAt (hz, p);
    if (m >= 0 && p.moduleOn[m])
        return m;
    const auto e = clampedEdges (p);
    int best = -1;
    double bestD = 1e9;
    for (int k = 0; k < dehonk::kNumModules; ++k)
        if (p.moduleOn[k])
        {
            const double lo = e[(std::size_t) k], hi = e[(std::size_t) k + 1];
            const double d = hz < lo ? std::log2 (lo / hz) : hz >= hi ? std::log2 (hz / hi) : 0.0;
            if (d < bestD)
            {
                bestD = d;
                best = k;
            }
        }
    return best;
}

/** Which bands hold an open notch in each frame, as opposed to a notch that has been let go and
    is releasing. The core smooths every band's cut with one release constant, so a band whose
    notch has been dropped falls by the same factor at every detector tick and nothing can fall
    faster; a notch that is still held (wanted cut above zero) falls more slowly or not at all.
    The fastest per-tick fall seen anywhere in the render is taken as free release, and a band
    falling at that rate is counted as releasing. This needs one frame per detector tick: render
    with a block size that divides the tick period (8 samples at 48 kHz). held[f][k] is set when
    band k holds a notch deeper than minDb in frame f. */
struct HeldNotches
{
    std::vector<std::vector<char>> held;
    double releaseRatio = 1.0;
    bool everyTick = true;
    double framePeriodSec = 0.0;
};

HeldNotches heldNotches (const RenderResult& r, double minDb)
{
    HeldNotches h;
    const auto nb = (std::size_t) r.numBands;
    const std::size_t nf = r.frames.size();
    h.held.assign (nf, std::vector<char> (nb, 0));
    double rmin = 1.0;
    for (std::size_t f = 1; f < nf; ++f)
    {
        if (r.frames[f].counter != r.frames[f - 1].counter + 1)
            h.everyTick = false;
        for (std::size_t k = 0; k < nb; ++k)
        {
            const double a = r.frames[f - 1].cutDb[k], b = r.frames[f].cutDb[k];
            if (a > 0.05 && b > 0.0 && b < a)
                rmin = std::min (rmin, b / a);
        }
    }
    h.releaseRatio = rmin;
    for (std::size_t f = 0; f < nf; ++f)
        for (std::size_t k = 0; k < nb; ++k)
        {
            const double c = r.frames[f].cutDb[k];
            if (c <= minDb)
                continue;
            const double prev = f > 0 ? (double) r.frames[f - 1].cutDb[k] : 0.0;
            const bool releasing = f > 0 && c < prev && c <= prev * rmin * (1.0 + 2e-5);
            h.held[f][k] = releasing ? 0 : 1;
        }
    if (nf > 1)
        h.framePeriodSec = (r.frames.back().timeSec - r.frames.front().timeSec) / (double) (nf - 1);
    return h;
}

/** Level of the bed in a 1/12-octave band around hz, mean square in dB. */
double bandLevelDb (const Spectrum& s, double hz) { return s.centredDb (hz, 1.0 / 24.0); }

/** Peak amplitude of a sine whose mean square is levelDb. */
double sineAmpFor (double levelDb) { return std::sqrt (2.0 * std::pow (10.0, levelDb / 10.0)); }

/** The signal for effect_on_peaks and several later tests: the bass-heavy bed (pink, +12 dB below
    100 Hz) with narrow noise bands (raised cosine, 1/6 octave at half height, so a third of an
    octave in all) peaking 8 dB above it at each of peakHz, normalised to rmsDbFs. */
Audio peaksSignal (double sr, double seconds, int nch, const std::vector<double>& peakHz, double rmsDbFs, std::uint64_t seed,
                   Audio* bedOut = nullptr, double peakDb = 8.0)
{
    HonkShape shape;
    shape.bed = bassHeavyBedDb;
    shape.resonanceHz = peakHz;
    shape.resonanceDb = peakDb;
    shape.resonanceFwhmOct = 1.0 / 6.0;
    return honkSignal (sr, seconds, nch, shape, rmsDbFs, seed, bedOut);
}

/** The bass-heavy bed plus sustained wavering tones. levelsDb[i] (one value per sample, or a
    single value) is tone i's level above the bed's own level in the 1/12-octave band around it.
    The sum is normalised to rmsDbFs. Mono. */
Audio tonesOverBed (double sr, std::size_t n, const std::vector<double>& toneHz, const std::vector<std::vector<float>>& levelsDb,
                    double rmsDbFs, std::uint64_t seed)
{
    auto bed = shapedNoise (sr, n, seed, bassHeavyBedDb);
    normaliseRms (bed, -20.0);
    const Spectrum sb = powerSpectrum (fromChannels (sr, { bed }), 0, n);
    std::vector<double> acc (bed.begin(), bed.end());
    for (std::size_t i = 0; i < toneHz.size(); ++i)
    {
        const double base = bandLevelDb (sb, toneHz[i]);
        std::vector<float> env (levelsDb[i].size());
        for (std::size_t j = 0; j < env.size(); ++j)
            env[j] = (float) sineAmpFor (base + levelsDb[i][j]);
        const auto tone = waveringTone (sr, n, toneHz[i], 0.003, env, seed * 31 + i);
        for (std::size_t j = 0; j < n; ++j)
            acc[j] += tone[j];
    }
    std::vector<float> x (n);
    for (std::size_t j = 0; j < n; ++j)
        x[j] = (float) acc[j];
    normaliseRms (x, rmsDbFs);
    return fromChannels (sr, { x });
}

/** Per-band mean of the cut over frames from fromSec on, and the mean of each module's deepest notch. */
std::string moduleCutSummary (const RenderResult& r, double fromSec)
{
    double mean[dehonk::kNumModules] {}, mx[dehonk::kNumModules] {};
    std::size_t n = 0;
    for (const auto& f : r.frames)
        if (f.timeSec >= fromSec)
        {
            ++n;
            for (int m = 0; m < dehonk::kNumModules; ++m)
            {
                mean[m] += f.moduleCutDb[(std::size_t) m];
                mx[m] = std::max (mx[m], (double) f.moduleCutDb[(std::size_t) m]);
            }
        }
    std::string s;
    for (int m = 0; m < dehonk::kNumModules; ++m)
        s += fmt ("%s%s mean %.2f max %.2f", m == 0 ? "" : ", ", moduleName (m), n > 0 ? mean[m] / (double) n : 0.0, mx[m]);
    return s;
}

//==============================================================================
// transparency_pink

void testTransparencyPink (Ctx& t)
{
    // 1. Always acting (threshold -60), default modules.
    {
        const Audio in = pinkAudio (kSr, S (15.0), 2, -18.0, 21);
        RenderSpec s;
        s.params = alwaysOn();
        s.collectFrames = true;
        const auto r = render (in, s);
        const auto cs = cutStats (r, 1.0);
        const Spectrum si = powerSpectrum (in, S (1.0), in.length());
        const Spectrum so = powerSpectrum (r.out, S (1.0), in.length());
        const auto d = bandChanges (si, so, r.bandHz);
        double worst = 0.0, worstHz = 0.0;
        for (std::size_t k = 0; k < d.size(); ++k)
            if (r.bandHz[k] < kSr * 0.45 && std::abs (d[k]) >= std::abs (worst))
            {
                worst = d[k];
                worstHz = r.bandHz[k];
            }
        t.check (cs.frames > 0, fmt ("pink -18 dBFS RMS stereo, threshold -60: %zu display frames after 1 s", cs.frames));
        t.check (cs.meanTotal < 1.5, fmt ("deepest notch: mean %.3f dB (limit 1.5), 99th percentile %.3f dB, largest %.3f dB", cs.meanTotal,
                                          cs.p99Total, cs.maxTotal));
        t.check (std::abs (worst) < 0.5, fmt ("largest long-term level change in any 1/12-octave band %+.3f dB at %.0f Hz (limit +-0.5)", worst, worstHz));
        t.info ("per module: " + moduleCutSummary (r, 1.0));
        t.info ("level change per band (dB): " + bandList (r.bandHz, d));
        t.headline = fmt ("always acting: deepest notch mean %.2f dB (limit 1.5), max %.2f dB, worst band %+.2f dB (limit 0.5)", cs.meanTotal,
                          cs.maxTotal, worst);
    }
    // 2. Default threshold (-16 dBFS) with pink noise at -30 dBFS RMS: below it, so no cut at all.
    {
        const Audio in = pinkAudio (kSr, S (10.0), 2, -30.0, 22);
        RenderSpec s;
        s.collectFrames = true;
        const auto r = render (in, s);
        const auto cs = cutStats (r, 0.0);
        double prog = -400.0, act = 0.0;
        for (const auto& f : r.frames)
        {
            prog = std::max (prog, (double) f.programDb);
            act = std::max (act, (double) f.activity);
        }
        t.check (cs.maxTotal <= 0.1, fmt ("pink -30 dBFS RMS at the default threshold (%.0f dBFS): largest cut in any frame %.3f dB (limit 0.1); programme level up to %.1f dBFS, activity up to %.2f",
                                          (double) dehonk::Params {}.thresholdDb, cs.maxTotal, prog, act));
        t.headline += fmt ("; at -30 dBFS and default threshold max cut %.2f dB (limit 0.1)", cs.maxTotal);
    }
}

//==============================================================================
// transparency_sweep

void testTransparencySweep (Ctx& t)
{
    // A slow log sweep is a lone sine: nothing sits at a subharmonic below it, so the parent rule
    // should keep every notch off it. Measured directly: in 20 ms windows the output's level
    // against the input's is the cut at the frequency the sweep is at.
    const double f0 = 20.0, f1 = 5000.0, T = 30.0;
    const std::size_t n = S (T);
    const Audio in = fromChannels (kSr, { logSweep (kSr, n, f0, f1, dbToAmp (-12.0)) });
    RenderSpec s;
    s.params = alwaysOn();
    s.collectFrames = true;
    const auto r = render (in, s);
    auto sweepHz = [&] (double tSec) { return f0 * std::exp (tSec / T * std::log (f1 / f0)); };

    const std::size_t W = S (0.020), hop = S (0.010);
    // Largest cut per third of an octave, from 25 Hz.
    std::vector<double> binMax (30, 0.0);
    double worst = 0.0, worstT = 0.0;
    for (std::size_t w0 = S (0.1); w0 + W + S (0.1) < n; w0 += hop)
    {
        double pi = 0.0, po = 0.0;
        for (std::size_t i = w0; i < w0 + W; ++i)
        {
            pi += (double) in.ch[0][i] * in.ch[0][i];
            po += (double) r.out.ch[0][i] * r.out.ch[0][i];
        }
        const double tc = (double) (w0 + W / 2) / kSr;
        const double cut = -powerToDb (po / pi);
        const double hz = sweepHz (tc);
        const int bin = (int) std::floor (3.0 * std::log2 (hz / 25.0));
        if (bin >= 0 && bin < (int) binMax.size())
            binMax[(std::size_t) bin] = std::max (binMax[(std::size_t) bin], cut);
        if (cut > worst)
        {
            worst = cut;
            worstT = tc;
        }
    }
    std::string curve;
    for (std::size_t b = 0; b < binMax.size(); ++b)
    {
        const double lo = 25.0 * std::exp2 ((double) b / 3.0);
        if (lo < f1)
            curve += fmt ("%s%.0f:%.2f", curve.empty() ? "" : " ", lo, binMax[b]);
    }
    t.check (worst < 1.0, fmt ("largest cut at the sweep's own frequency %.2f dB at %.2f s (%.0f Hz) (limit 1)", worst, worstT, sweepHz (worstT)));
    t.info ("largest cut at the sweep's frequency by third of an octave (from Hz : dB): " + curve);
    const auto cs = cutStats (r, 0.0);
    t.info (fmt ("display: deepest notch anywhere mean %.2f dB, max %.2f dB; %s", cs.meanTotal, cs.maxTotal, moduleCutSummary (r, 0.0).c_str()));

    // Where the display reports cuts above 1 dB: which bands, when, and the band levels then,
    // including the bands at the parent rule's subharmonics (an octave, a twelfth, two octaves and
    // two octaves and a major third below).
    int shown = 0;
    double lastShown = -1.0;
    for (const auto& f : r.frames)
    {
        if (f.totalCutDb <= 1.0 || f.timeSec - lastShown < 0.5 || shown >= 8)
            continue;
        lastShown = f.timeSec;
        ++shown;
        const double hz = sweepHz (f.timeSec);
        std::string bands;
        for (int k = 0; k < r.numBands; ++k)
            if (f.cutDb[(std::size_t) k] > 0.5f)
            {
                std::string parents;
                for (int b : { 12, 19, 24, 28 })
                    if (k - b >= 0)
                        parents += fmt ("%s%.1f", parents.empty() ? "" : "/", (double) f.levelDb[(std::size_t) (k - b)]);
                bands += fmt (" [%.0f Hz cut %.2f dB, level %.1f dBFS, parents %s dBFS]", (double) r.bandHz[(std::size_t) k],
                              (double) f.cutDb[(std::size_t) k], (double) f.levelDb[(std::size_t) k], parents.c_str());
            }
        const int sb = nearestBand (r.bandHz, hz);
        t.info (fmt ("cut above 1 dB at %.2f s, sweep at %.0f Hz (band %.0f Hz at %.1f dBFS):%s", f.timeSec, hz, (double) r.bandHz[(std::size_t) sb],
                     (double) f.levelDb[(std::size_t) sb], bands.c_str()));
    }
    t.headline = fmt ("largest cut at the sweep's frequency %.2f dB at %.0f Hz (limit 1)", worst, sweepHz (worstT));
}

//==============================================================================
// below_modules_untouched

void belowModulesCase (Ctx& t, const char* label, const Audio& in, const dehonk::Params& p, double limitHz,
                       const std::vector<double>& toneHz, double& worstOut)
{
    RenderSpec s;
    s.params = p;
    s.collectFrames = true;
    const auto r = render (in, s);
    const auto cs = cutStats (r, 1.0);
    const Spectrum si = powerSpectrum (in, S (1.0), in.length());
    const Spectrum so = powerSpectrum (r.out, S (1.0), in.length());
    const std::string head = fmt ("%s, %s", label, moduleList (p).c_str());

    // The signal must be one the core acts on, or nothing below says anything.
    t.check (cs.meanTotal >= 1.0, fmt ("%s: deepest notch mean %.2f dB, max %.2f dB after 1 s (need a mean of at least 1 dB: the signal is being treated)",
                                       head.c_str(), cs.meanTotal, cs.maxTotal));

    // Every 1/12-octave band whose centre is at or below the limit, so a notch just under the
    // edge shows even where the broadband level is dominated by the bass.
    // Bands that hold nothing (the steady tones have no bed: between harmonics the input is FFT
    // leakage 100 dB down) are skipped: a change there is meaningless and inaudible. A band is
    // judged when its input level is within 60 dB of the loudest band.
    const auto d = bandChanges (si, so, r.bandHz);
    double loudest = -400.0;
    for (float hz : r.bandHz)
        loudest = std::max (loudest, si.centredDb (hz, 1.0 / 24.0));
    double worst = 0.0, worstHz = 0.0;
    int judged = 0, skipped = 0;
    for (std::size_t k = 0; k < d.size(); ++k)
    {
        if (r.bandHz[k] < 20.0 || r.bandHz[k] > limitHz + 1e-3)
            continue;
        if (si.centredDb (r.bandHz[k], 1.0 / 24.0) < loudest - 60.0)
        {
            ++skipped;
            continue;
        }
        ++judged;
        if (std::abs (d[k]) >= std::abs (worst))
        {
            worst = d[k];
            worstHz = r.bandHz[k];
        }
    }
    worstOut = std::max (worstOut, std::abs (worst));
    t.check (std::abs (worst) < 0.5 && judged > 0,
             fmt ("%s: largest level change in a 1/12-octave band centred 20-%.0f Hz %+.3f dB at %.0f Hz (limit +-0.5; %d bands judged, %d skipped as more than 60 dB below the loudest band)",
                  head.c_str(), limitHz, worst, worstHz, judged, skipped));
    t.info (fmt ("%s: per band up to %.0f Hz (Hz:dB): %s", head.c_str(), limitHz, bandList (r.bandHz, d, 20.0, limitHz + 1e-3).c_str()));
    t.info (fmt ("%s: mean cut per band from half an octave below to half an octave above %.0f Hz (Hz:dB): %s", head.c_str(), limitHz,
                 bandList (r.bandHz, cs.mean, limitHz * std::sqrt (0.5), limitHz * std::sqrt (2.0)).c_str()));

    const double below = so.bandDb (20.0, limitHz) - si.bandDb (20.0, limitHz);
    worstOut = std::max (worstOut, std::abs (below));
    t.check (std::abs (below) < 0.5, fmt ("%s: level 20-%.0f Hz changed %+.3f dB (limit +-0.5)", head.c_str(), limitHz, below));
    for (double hz : toneHz)
    {
        const double dc = so.centredDb (hz, 1.0 / 24.0) - si.centredDb (hz, 1.0 / 24.0);
        worstOut = std::max (worstOut, std::abs (dc));
        t.check (std::abs (dc) < 0.5, fmt ("%s: %.0f Hz component changed %+.3f dB (limit +-0.5)", head.c_str(), hz, dc));
    }
    const double top = clampedEdges (p)[dehonk::kNumModules];
    t.info (fmt ("%s: %.0f-%.0f Hz changed %+.2f dB", head.c_str(), limitHz, top, so.bandDb (limitHz, top) - si.bandDb (limitHz, top)));
}

void testBelowModulesUntouched (Ctx& t)
{
    const std::size_t n = S (8.0);
    const std::vector<double> rbass = { 0.0, -17.0, -22.0, -40.0, -42.0, -60.0, -55.0, -65.0 };
    std::vector<double> raised = rbass;
    for (std::size_t h = 3; h < raised.size(); ++h)
        raised[h] = -10.0;

    // A bass line (41 to 73 Hz) with harmonics 4..8 raised, over a quiet pink bed, stereo.
    Audio line = Audio::make (kSr, 2, n);
    const double notes[] = { 41.2, 55.0, 61.74, 73.42, 49.0, 65.41, 46.25, 55.0 };
    for (int c = 0; c < 2; ++c)
    {
        auto bed = pinkNoise (kSr, n, 33 + (std::uint64_t) c);
        normaliseRms (bed, -40.0);
        for (std::size_t i = 0; i < n; ++i)
            line.ch[(std::size_t) c][i] = bed[i];
        for (std::size_t note = 0; note < 8; ++note)
        {
            const std::size_t start = note * S (1.0), len = S (1.0);
            auto tone = harmonicTone (kSr, len, notes[note], raised, 0.25, 40 + note);
            for (std::size_t i = 0; i < len && start + i < n; ++i)
            {
                const double fade = std::min ({ 1.0, (double) i / 480.0, (double) (len - i) / 480.0 });
                line.ch[(std::size_t) c][start + i] += (float) (fade * tone[i]);
            }
        }
    }
    const Audio tone1 = fromChannels (kSr, { harmonicTone (kSr, n, 50.0, rbass, 0.25, 31) });
    const Audio tone2 = fromChannels (kSr, { harmonicTone (kSr, n, 50.0, raised, 0.25, 32) });
    const Audio peaks = peaksSignal (kSr, 8.0, 2, { 151.0, 247.0 }, -12.0, 34);

    double worst = 0.0;
    dehonk::Params boxMud = alwaysOn();
    belowModulesCase (t, "50 Hz, RBass-like harmonics", tone1, boxMud, 100.0, { 50.0, 100.0 }, worst);
    belowModulesCase (t, "50 Hz, harmonics 4-8 at -10 dB", tone2, boxMud, 100.0, { 50.0, 100.0 }, worst);
    belowModulesCase (t, "bass line 41-73 Hz, harmonics 4-8 at -10 dB, stereo", line, boxMud, 100.0, {}, worst);
    belowModulesCase (t, "bass-heavy bed with peaks at 151 and 247 Hz, stereo", peaks, boxMud, 100.0, {}, worst);

    // Only Fog: the RBass-like tone has nothing in Fog's range for it to act on, so it is left out.
    dehonk::Params mudOnly = alwaysOn();
    for (int m = 0; m < dehonk::kNumModules; ++m)
        mudOnly.moduleOn[m] = m == dehonk::Fog;
    belowModulesCase (t, "50 Hz, harmonics 4-8 at -10 dB", tone2, mudOnly, 180.0, { 50.0, 100.0, 150.0 }, worst);
    belowModulesCase (t, "bass line 41-73 Hz, harmonics 4-8 at -10 dB, stereo", line, mudOnly, 180.0, {}, worst);
    belowModulesCase (t, "bass-heavy bed with peaks at 151 and 247 Hz, stereo", peaks, mudOnly, 180.0, { 151.0 }, worst);
    t.headline = fmt ("largest change below the lowest module that is on %.3f dB (limit 0.5)", worst);
}

//==============================================================================
// effect_on_peaks

struct PeakResult
{
    double meanCut = 0.0;      // mean over settled frames of the largest cut within 1/12 octave of the peak
    double centreOct = 0.0;    // cut-weighted centre of the time-mean cut within 1/3 octave, octaves from the peak
    double realised = 0.0;     // own analysis: level change at the peak (1/12-octave band)
    double sideLo = 0.0, sideHi = 0.0;   // own analysis: level change 1/3 octave below and above
    double maxNear = 0.0;      // largest cut within 1/12 octave of the peak in any settled frame
    double maxNearHz = 0.0;    // the band it was in
};

PeakResult measurePeak (const RenderResult& r, const Spectrum& si, const Spectrum& so, double peakHz, double settle)
{
    PeakResult p;
    const auto cs = cutStats (r, settle);
    std::size_t nf = 0;
    for (const auto& f : r.frames)
    {
        if (f.timeSec < settle)
            continue;
        double m = 0.0, mHz = 0.0;
        for (int k = 0; k < r.numBands; ++k)
            if (std::abs (std::log2 (r.bandHz[(std::size_t) k] / peakHz)) <= 1.0 / 12.0 + 1e-6 && f.cutDb[(std::size_t) k] > m)
            {
                m = f.cutDb[(std::size_t) k];
                mHz = r.bandHz[(std::size_t) k];
            }
        p.meanCut += m;
        if (m > p.maxNear)
        {
            p.maxNear = m;
            p.maxNearHz = mHz;
        }
        ++nf;
    }
    p.meanCut /= std::max<double> (1.0, (double) nf);
    double wsum = 0.0, xsum = 0.0;
    for (int k = 0; k < r.numBands; ++k)
    {
        const double x = std::log2 (r.bandHz[(std::size_t) k] / peakHz);
        if (std::abs (x) <= 1.0 / 3.0 + 1e-6 && ! cs.mean.empty())
        {
            wsum += cs.mean[(std::size_t) k];
            xsum += cs.mean[(std::size_t) k] * x;
        }
    }
    p.centreOct = wsum > 0.0 ? xsum / wsum : 99.0;
    auto change = [&] (double hz) { return so.centredDb (hz, 1.0 / 24.0) - si.centredDb (hz, 1.0 / 24.0); };
    p.realised = change (peakHz);
    p.sideLo = change (peakHz * std::exp2 (-1.0 / 3.0));
    p.sideHi = change (peakHz * std::exp2 (1.0 / 3.0));
    return p;
}

void testEffectOnPeaks (Ctx& t)
{
    const double seconds = 10.0, settle = 2.0;

    // 1. Peaks at 151 and 247 Hz (Tub and Fog), default modules: each must be notched.
    {
        const Audio in = peaksSignal (kSr, seconds, 2, { 151.0, 247.0 }, -12.0, 51);
        RenderSpec s;
        s.params = alwaysOn();
        s.collectFrames = true;
        const auto r = render (in, s);
        const Spectrum si = powerSpectrum (in, S (settle), in.length());
        const Spectrum so = powerSpectrum (r.out, S (settle), in.length());
        std::string head;
        for (double hz : { 151.0, 247.0 })
        {
            const auto p = measurePeak (r, si, so, hz, settle);
            t.check (p.meanCut >= 3.0, fmt ("peak %.0f Hz: mean cut within 1/12 octave of it %.2f dB once settled (need at least 3); largest %.2f dB",
                                            hz, p.meanCut, p.maxNear));
            t.check (std::abs (p.centreOct) <= 1.0 / 12.0, fmt ("peak %.0f Hz: the long-term cut is centred %+.3f octave from it (limit 1/12 = 0.083)", hz, p.centreOct));
            t.check (std::abs (p.sideLo) < 1.5 && std::abs (p.sideHi) < 1.5,
                     fmt ("peak %.0f Hz: level a third of an octave below changed %+.2f dB, above %+.2f dB (limit +-1.5); at the peak %+.2f dB",
                          hz, p.sideLo, p.sideHi, p.realised));
            head += fmt ("%s%.0f Hz cut %.1f dB", head.empty() ? "" : ", ", hz, p.meanCut);
        }
        const auto cs = cutStats (r, settle);
        t.info ("mean cut per band (Hz:dB): " + bandList (r.bandHz, cs.mean, 50.0, 1000.0));
        t.info ("per module: " + moduleCutSummary (r, settle));
        t.headline = head;
    }

    // 2. Peaks at 500 and 900 Hz, in Bark's range. Tub and Fog only: left alone. Bark on as well: notched.
    {
        const Audio in = peaksSignal (kSr, seconds, 2, { 500.0, 900.0 }, -12.0, 52);
        const Spectrum si = powerSpectrum (in, S (settle), in.length());
        for (bool honk : { false, true })
        {
            RenderSpec s;
            s.params = alwaysOn();
            s.params.moduleOn[dehonk::Bark] = honk;
            s.collectFrames = true;
            const auto r = render (in, s);
            const Spectrum so = powerSpectrum (r.out, S (settle), in.length());
            for (double hz : { 500.0, 900.0 })
            {
                const auto p = measurePeak (r, si, so, hz, settle);
                if (! honk)
                    t.check (p.maxNear < 0.5 && std::abs (p.realised) < 0.5,
                             fmt ("peak %.0f Hz, %s: largest cut within 1/12 octave %.2f dB (band %.0f Hz), mean %.2f dB; level change at the peak %+.2f dB (limits 0.5: left alone)",
                                  hz, moduleList (s.params).c_str(), p.maxNear, p.maxNearHz, p.meanCut, p.realised));
                else if (hz > 600.0)
                {
                    // The 900 Hz peak has nothing at any subharmonic (450, 300, 225, 180 Hz are all
                    // plain bed), so it reads as the fundamental of something: a lead line, a siren,
                    // a sung note. On a full mix that is wanted material, and Bark leaves it alone.
                    t.check (p.meanCut < 1.5, fmt ("peak %.0f Hz with nothing below it, %s: mean cut within 1/12 octave %.2f dB (limit 1.5: taken for a fundamental and left alone), largest %.2f dB",
                                                   hz, moduleList (s.params).c_str(), p.meanCut, p.maxNear));
                }
                else
                {
                    t.check (p.meanCut >= 1.0, fmt ("peak %.0f Hz, %s: mean cut within 1/12 octave %.2f dB once settled (need at least 1), largest %.2f dB, level change at the peak %+.2f dB",
                                                    hz, moduleList (s.params).c_str(), p.meanCut, p.maxNear, p.realised));
                    t.info (fmt ("peak %.0f Hz, %s: centred %+.3f octave from it, a third of an octave below %+.2f dB, above %+.2f dB", hz,
                                 moduleList (s.params).c_str(), p.centreOct, p.sideLo, p.sideHi));
                }
                t.headline += fmt ("; %.0f Hz %s %.1f dB", hz, honk ? "Bark on" : "Bark off", honk ? p.meanCut : p.maxNear);
            }
        }
    }
}

//==============================================================================
// parent_rule_protects_fundamental

void testParentRule (Ctx& t)
{
    const std::size_t n = S (8.0);
    std::vector<double> levels (12);
    for (std::size_t h = 0; h < levels.size(); ++h)
        levels[h] = -20.0 * std::log10 ((double) h + 1.0); // 1/n, like a sawtooth
    const Audio in = fromChannels (kSr, { harmonicTone (kSr, n, 160.0, levels, 0.25, 61) });
    const Spectrum si = powerSpectrum (in, S (1.0), n);

    double cutOn = 0.0, cutOff = 0.0;
    for (float pr : { 1.0f, 0.0f })
    {
        RenderSpec s;
        s.params = alwaysOn();
        s.collectFrames = true;
        s.useTuning = true;
        s.tuning = defaultTuning();
        s.tuning.parentRule = pr;
        const auto r = render (in, s);
        const auto cs = cutStats (r, 1.0);
        const Spectrum so = powerSpectrum (r.out, S (1.0), n);
        const int b = nearestBand (r.bandHz, 160.0);
        const double frameCut = cs.mean.empty() ? 0.0 : cs.mean[(std::size_t) b];
        const double frameMax = cs.max.empty() ? 0.0 : cs.max[(std::size_t) b];
        const double realised = si.centredDb (160.0, 1.0 / 24.0) - so.centredDb (160.0, 1.0 / 24.0);
        const double worst = std::max (frameCut, realised);
        (pr > 0.5f ? cutOn : cutOff) = worst;
        std::string harm;
        for (double hz : { 320.0, 480.0, 640.0 })
            harm += fmt (" %.0f Hz %.2f", hz, si.centredDb (hz, 1.0 / 24.0) - so.centredDb (hz, 1.0 / 24.0));
        if (pr > 0.5f)
            t.check (worst <= 3.0, fmt ("parentRule=1: cut on the 160 Hz fundamental (band %.0f Hz) mean %.2f dB, max %.2f dB, realised %.2f dB (fail above 3; expected under 2)",
                                        (double) r.bandHz[(std::size_t) b], frameCut, frameMax, realised));
        else
            t.info (fmt ("parentRule=0: cut on the 160 Hz fundamental mean %.2f dB, max %.2f dB, realised %.2f dB", frameCut, frameMax, realised));
        t.info (fmt ("parentRule=%g: realised cut on harmonics:%s dB", (double) pr, harm.c_str()));
    }
    // Without the rule the detector must cut this fundamental, or the parentRule=1 result above says
    // nothing about the rule: a detector that ignores the signal would pass it too.
    t.check (cutOff >= 3.0, fmt ("parentRule=0: cut on the fundamental %.2f dB (need at least 3), so the signal does trigger the detector", cutOff));
    t.check (cutOff - cutOn >= 2.0, fmt ("the parent rule spares the fundamental %.2f dB of cut (need at least 2)", cutOff - cutOn));
    t.info (fmt ("parentRule=1 %s the 2 dB expectation", cutOn < 2.0 ? "meets" : "misses"));
    t.headline = fmt ("fundamental cut %.2f dB with parentRule=1 (limit 3), %.2f dB with 0 (need 3 and 2 more)", cutOn, cutOff);
}

//==============================================================================
// notch_shape

/** The core's magnitude response with fixed cuts set through debugSetStaticCuts, from its impulse
    response. The notches are left 0.2 s to settle on silence before the impulse. */
struct StaticResponse
{
    double binHz = 0.0;
    std::vector<double> db;
    double at (double hz) const
    {
        const double x = hz / binHz;
        const auto i = (std::size_t) std::floor (x);
        if (i + 1 >= db.size())
            return db.empty() ? 0.0 : db.back();
        return db[i] + (db[i + 1] - db[i]) * (x - (double) i);
    }
};

StaticResponse staticResponse (const std::vector<float>& cuts)
{
    const std::size_t pre = S (0.2), N = 1u << 18;   // 5.5 s of impulse response at 48 kHz
    Audio in = Audio::make (kSr, 1, pre + N);
    in.ch[0][pre] = 1.0f;
    RenderSpec s;
    s.params = alwaysOn();
    s.params.maxCutDb = 12.0f;   // the shape is measured up to 8 dB; Max Cut has its own test
    s.staticCuts = cuts;
    const auto r = render (in, s);
    std::vector<std::complex<double>> buf (N);
    for (std::size_t i = 0; i < N; ++i)
        buf[i] = { (double) r.out.ch[0][pre + i], 0.0 };
    fft (buf, false);
    StaticResponse resp;
    resp.binHz = kSr / (double) N;
    resp.db.resize (N / 2);
    for (std::size_t k = 0; k < N / 2; ++k)
        resp.db[k] = powerToDb (std::norm (buf[k]));
    return resp;
}

void testNotchShape (Ctx& t)
{
    std::vector<float> bandHz;
    {
        dehonk::Core probe;
        probe.prepare (kSr, 512, 1);
        for (int k = 0; k < probe.numBands(); ++k)
            bandHz.push_back (probe.bandHz (k));
    }
    const std::size_t nb = bandHz.size();

    double worstDepth = 0.0, minW = 9.0, maxW = 0.0, worstFlat = 0.0;
    for (double centre : { 80.0, 151.0, 247.0, 620.0 })
    {
        const int b = nearestBand (bandHz, centre);
        const double fc = bandHz[(std::size_t) b];
        std::string widths;
        for (double depth : { 3.0, 6.0, 8.0 })
        {
            std::vector<float> cuts (nb, 0.0f);
            cuts[(std::size_t) b] = (float) depth;
            const auto resp = staticResponse (cuts);
            const double got = -resp.at (fc);
            worstDepth = std::max (worstDepth, std::abs (got - depth));

            // Width at half the depth (in dB), searched outwards in steps of 1/2000 octave.
            auto edge = [&] (double dir) {
                double prevX = 0.0, prevC = got;
                for (double x = 0.0005; x < 1.5; x += 0.0005)
                {
                    const double c = -resp.at (fc * std::exp2 (dir * x));
                    if (c <= 0.5 * depth)
                        return prevX + (x - prevX) * (prevC - 0.5 * depth) / std::max (1e-12, prevC - c);
                    prevX = x;
                    prevC = c;
                }
                return 1.5;
            };
            const double width = edge (-1.0) + edge (1.0);
            minW = std::min (minW, width);
            maxW = std::max (maxW, width);

            double flat = 0.0, flatHz = 0.0;
            for (double f = 20.0; f < 0.45 * kSr; f *= std::exp2 (1.0 / 48.0))
                if (std::abs (std::log2 (f / fc)) > 1.0 && std::abs (resp.at (f)) >= std::abs (flat))
                {
                    flat = resp.at (f);
                    flatHz = f;
                }
            worstFlat = std::max (worstFlat, std::abs (flat));

            t.check (std::abs (got - depth) <= 0.5, fmt ("band %.1f Hz, %.0f dB requested: depth at the centre %.2f dB (limit +-0.5)", fc, depth, got));
            t.check (width >= 0.10 && width <= 0.25, fmt ("band %.1f Hz, %.0f dB: width at half depth %.3f octave (limits 0.10 to 0.25)", fc, depth, width));
            t.check (std::abs (flat) <= 0.5, fmt ("band %.1f Hz, %.0f dB: largest deviation more than an octave away %+.3f dB at %.0f Hz (limit +-0.5)",
                                                  fc, depth, flat, flatHz));
            widths += fmt (" %.0f dB: %.3f", depth, width);
        }
        t.info (fmt ("band %.1f Hz: width at half depth (octave) by depth:%s", fc, widths.c_str()));
    }

    // Two notches a quarter of an octave apart (three bands). The core does not correct for their
    // overlap, so each comes out a little deeper than asked; both must still be within 1 dB.
    double worstPair = 0.0;
    for (double centre : { 151.0, 247.0 })
        for (double depth : { 6.0, 8.0 })
        {
            const int b = nearestBand (bandHz, centre);
            const int b2 = b + dehonk::kBandsPerOctave / 4;
            std::vector<float> cuts (nb, 0.0f);
            cuts[(std::size_t) b] = (float) depth;
            cuts[(std::size_t) b2] = (float) depth;
            const auto resp = staticResponse (cuts);
            const double g1 = -resp.at (bandHz[(std::size_t) b]), g2 = -resp.at (bandHz[(std::size_t) b2]);
            const double mid = -resp.at (std::sqrt ((double) bandHz[(std::size_t) b] * bandHz[(std::size_t) b2]));
            worstPair = std::max ({ worstPair, std::abs (g1 - depth), std::abs (g2 - depth) });
            t.check (std::abs (g1 - depth) <= 1.0 && std::abs (g2 - depth) <= 1.0,
                     fmt ("two %.0f dB notches at %.1f and %.1f Hz: %.2f and %.2f dB at their centres (limit +-1); %.2f dB halfway between",
                          depth, (double) bandHz[(std::size_t) b], (double) bandHz[(std::size_t) b2], g1, g2, mid));
        }
    t.headline = fmt ("depth error %.2f dB (limit 0.5), width %.3f-%.3f octave (limits 0.10-0.25), flat beyond an octave %.2f dB (limit 0.5), pair %.2f dB (limit 1)",
                      worstDepth, minW, maxW, worstFlat, worstPair);
}

//==============================================================================
// Diff (SPEC 13.6): the output is the delayed input times 10^(diffOffsetDb / 20).

/** How well a Diff render matches "delayed input times the gain the core reports". Each 10 ms
    window of the output is fitted from the input and the input times time (a gain and its slope,
    all channels together); the window's residual is what that fit cannot explain, relative to the
    output, and the window's gain is compared with DisplayFrame::diffOffsetDb read at the same
    moment (the frame is looked up at the window's centre plus the latency, since the core applied
    the gain that much later than the input time). The whole stretch is also compared with the
    input times one fixed gain (the mean of the reported offsets), for information. */
struct DiffMatch
{
    double worstResidDb = -400.0, worstResidAt = 0.0;   // per window, gain removed
    double worstGainErrDb = 0.0, worstGainErrAt = 0.0;  // |window gain - reported offset|
    double meanOffsetDb = 0.0, minOffsetDb = 0.0, maxOffsetDb = -100.0;
    double fixedGainResidDb = -400.0;                    // one gain for the whole stretch
    int windows = 0;
    int negativeGainWindows = 0;                         // fitted gain 0 or below: inverted (or silent) output
    double offsetOutOfRangeDb = 0.0;                     // worst distance of diffOffsetDb outside -9..0 dB
    double offsetOutOfRangeAt = 0.0;
};

DiffMatch diffMatch (const RenderResult& r, const Audio& in, std::size_t from, std::size_t to)
{
    DiffMatch d;
    const double sr = in.sampleRate;
    const std::size_t W = S (0.010, sr);
    to = std::min (to, in.length());
    std::size_t fi = 0;
    double sumOff = 0.0;
    d.minOffsetDb = 100.0;
    for (std::size_t w0 = from; w0 + W <= to; w0 += W)
    {
        double a00 = 0, a01 = 0, a11 = 0, b0 = 0, b1 = 0, yy = 0;
        for (int ch = 0; ch < in.numChannels(); ++ch)
            for (std::size_t i = 0; i < W; ++i)
            {
                const double tau = ((double) i - 0.5 * (double) W) / (double) W;
                const double x = in.ch[(std::size_t) ch][w0 + i], y = r.out.ch[(std::size_t) ch][w0 + i];
                a00 += x * x;
                a01 += x * x * tau;
                a11 += x * x * tau * tau;
                b0 += x * y;
                b1 += x * y * tau;
                yy += y * y;
            }
        const double det = a00 * a11 - a01 * a01;
        if (det <= 0.0 || yy <= 0.0)
            continue;
        const double g0 = (b0 * a11 - b1 * a01) / det, g1 = (b1 * a00 - b0 * a01) / det;
        const double explained = g0 * b0 + g1 * b1;
        const double resid = powerToDb (std::max (0.0, yy - explained) / yy);
        // Residual can only be computed to about -140 dB in floats; clamp the reading there.
        const double residDb = std::max (resid, -160.0);
        const double t = ((double) w0 + 0.5 * (double) W + (double) r.latency) / sr;
        while (fi + 1 < r.frames.size() && r.frames[fi].timeSec < t)
            ++fi;
        const double off = r.frames.empty() ? 0.0 : (double) r.frames[fi].diffOffsetDb;
        // The gain must be positive: the delayed input times 10^(offset/20), not its inverse. A
        // negative fitted gain is an inverted output and counts as an infinite gain error.
        const bool positive = g0 > 0.0;
        const double gDb = positive ? ampToDb (g0) : std::numeric_limits<double>::infinity();
        d.negativeGainWindows += positive ? 0 : 1;
        const double outside = off > 0.0 ? off : (off < -9.0 ? -9.0 - off : 0.0);
        if (outside > d.offsetOutOfRangeDb)
        {
            d.offsetOutOfRangeDb = outside;
            d.offsetOutOfRangeAt = t;
        }
        ++d.windows;
        sumOff += off;
        d.minOffsetDb = std::min (d.minOffsetDb, off);
        d.maxOffsetDb = std::max (d.maxOffsetDb, off);
        if (residDb > d.worstResidDb)
        {
            d.worstResidDb = residDb;
            d.worstResidAt = (double) w0 / sr;
        }
        if (std::abs (gDb - off) > d.worstGainErrDb)
        {
            d.worstGainErrDb = std::abs (gDb - off);
            d.worstGainErrAt = (double) w0 / sr;
        }
    }
    if (d.windows > 0)
    {
        d.meanOffsetDb = sumOff / d.windows;
        d.fixedGainResidDb = residualDb (r.out, in, dbToAmp (d.meanOffsetDb), from, to);
    }
    return d;
}

/** A signal De-Honk cuts by well over 1 dB overall at Depth 100%: the bass-heavy bed with steady
    tones at 151 and 247 Hz standing 25 dB above it, stereo (independent beds), -18 dBFS RMS. */
Audio diffSignal (double seconds, std::uint64_t seed)
{
    const std::size_t n = S (seconds);
    Audio a = Audio::make (kSr, 2, n);
    for (int c = 0; c < 2; ++c)
    {
        const std::vector<float> lv (1, 25.0f);
        const Audio m = tonesOverBed (kSr, n, { 151.0, 247.0 }, { lv, lv }, -18.0, seed + (std::uint64_t) c);
        a.ch[(std::size_t) c] = m.ch[0];
    }
    return a;
}

//==============================================================================
// solo_honk_identity

void testSoloHonkIdentity (Ctx& t)
{
    HonkShape shape;
    shape.bump = true;
    const Audio in = honkSignal (kSr, 6.0, 2, shape, -18.0, 81);
    const std::size_t n = in.length(), from = S (0.5);
    double worst = -400.0;

    struct Case
    {
        float mix, trim, la;
    };
    for (const Case c : { Case { 1.0f, -3.0f, 0.0f }, Case { 0.6f, 2.0f, 5.0f } })
    {
        RenderSpec s;
        s.params.thresholdDb = kAlwaysDb;
        s.params.mix = c.mix;
        s.params.trimDb = c.trim;
        s.params.lookaheadMs = c.la;
        const auto normal = render (in, s);
        s.params.soloHonk = true;
        const auto solo = render (in, s);
        Audio sum = normal.out;
        for (int ch = 0; ch < sum.numChannels(); ++ch)
            for (std::size_t i = 0; i < n; ++i)
                sum.ch[(std::size_t) ch][i] += solo.out.ch[(std::size_t) ch][i];
        const double g = dbToAmp (c.trim);
        const double res = residualDb (sum, in, g, from, n);
        worst = std::max (worst, res);
        t.check (res < -80.0, fmt ("mix %.1f trim %+.0f dB lookahead %.0f ms: normal + solo vs trim x input: residual %s dB (limit -80)",
                                   (double) c.mix, (double) c.trim, (double) c.la, dbStr (res).c_str()));
        t.info (fmt ("mix %.1f trim %+.0f dB: solo output level %s dB relative to the input",
                     (double) c.mix, (double) c.trim, dbStr (powerToDb (meanSquare (solo.out, from, n) / meanSquare (in, from, n))).c_str()));
    }

    // Bypass: exactly the delayed input.
    for (float la : { 0.0f, 10.0f })
    {
        RenderSpec s;
        s.params.thresholdDb = kAlwaysDb;
        s.params.depth = 1.0f;
        s.params.lookaheadMs = la;
        const std::int64_t at = (std::int64_t) S (1.5);
        s.automate = [at] (std::int64_t pos, dehonk::Params& p) { p.bypass = pos >= at; };
        const auto r = render (in, s);
        const double res = residualDb (r.out, in, 1.0, S (1.6), n);
        const double mad = maxAbsDiff (r.out, in, 1.0, S (1.6), n);
        const double before = residualDb (r.out, in, 1.0, S (0.5), S (1.5));
        worst = std::max (worst, res);
        t.check (res < -120.0, fmt ("bypass switched on at 1.5 s, lookahead %2.0f ms: output vs input from 1.6 s: residual %s dB, max |diff| %.3g%s (limit -120)",
                                    (double) la, dbStr (res).c_str(), mad, mad == 0.0 ? ", bit-identical" : ""));
        t.info (fmt ("bypass lookahead %.0f ms: before switching, processed output differed from input by %s dB", (double) la, dbStr (before).c_str()));
    }

    // Diff: the delayed input turned down to the treated signal's level (SPEC 13.6). On a signal
    // De-Honk cuts by more than 1 dB overall, switched on at 1.5 s and left on; settled after
    // four times Tuning::diffMatchSeconds.
    const dehonk::Tuning tune = defaultTuning();
    const double settleSec = 4.0 * (double) tune.diffMatchSeconds;
    const Audio dsig = diffSignal (settleSec + 12.0, 83);
    const std::size_t dn = dsig.length(), settled = S (settleSec), last3 = dn - S (3.0);
    double worstShape = -400.0, worstGain = 0.0, worstLevel = 0.0;
    for (float la : { 0.0f, 10.0f })
    {
        RenderSpec s;
        s.params.thresholdDb = kAlwaysDb;
        s.params.depth = 1.0f;
        s.params.lookaheadMs = la;
        s.collectFrames = true;
        const auto treated = render (dsig, s);
        const double overall = powerToDb (meanSquare (treated.out, last3, dn) / meanSquare (dsig, last3, dn));
        const std::int64_t at = (std::int64_t) S (1.5);
        s.automate = [at] (std::int64_t pos, dehonk::Params& p) { p.diff = pos >= at; };
        const auto r = render (dsig, s);
        const DiffMatch early = diffMatch (r, dsig, S (1.6), settled);
        const DiffMatch late = diffMatch (r, dsig, settled, dn);
        worstShape = std::max ({ worstShape, early.worstResidDb, late.worstResidDb });
        worstGain = std::max (worstGain, late.worstGainErrDb);
        t.check (overall <= -1.0, fmt ("lookahead %2.0f ms: the treated signal is %+.2f dB against the input over the last 3 s (need -1 dB or less, or the level match proves little)",
                                       (double) la, overall));
        t.check (late.worstResidDb < -80.0 && early.worstResidDb < -80.0,
                 fmt ("diff on at 1.5 s, lookahead %2.0f ms: output against the input times a gain fitted per 10 ms window: worst residual %s dB at %.2f s from 1.6 s to %.0f s, %s dB at %.2f s once settled (limit -80); with one fixed gain over the settled stretch %s dB",
                      (double) la, dbStr (early.worstResidDb).c_str(), early.worstResidAt, settleSec, dbStr (late.worstResidDb).c_str(),
                      late.worstResidAt, dbStr (late.fixedGainResidDb).c_str()));
        t.check (late.worstGainErrDb <= 0.05,
                 fmt ("diff lookahead %2.0f ms, settled (from %.0f s): the gain applied matches diffOffsetDb (%.2f to %.2f dB) within %.3f dB, worst at %.2f s (limit 0.05)",
                      (double) la, settleSec, late.minOffsetDb, late.maxOffsetDb, late.worstGainErrDb, late.worstGainErrAt));
        t.info (fmt ("diff lookahead %2.0f ms, before settling (1.6 s to %.0f s): offset %.2f to %.2f dB, applied gain within %.3f dB of it (worst at %.2f s)",
                     (double) la, settleSec, early.minOffsetDb, early.maxOffsetDb, early.worstGainErrDb, early.worstGainErrAt));
        t.check (early.negativeGainWindows == 0 && late.negativeGainWindows == 0,
                 fmt ("diff lookahead %2.0f ms: the fitted gain is positive (not an inverted output) in every 10 ms window: %d of %d negative",
                      (double) la, early.negativeGainWindows + late.negativeGainWindows, early.windows + late.windows));
        {
            // diffOffsetDb in every frame once settled, not only at the fitted windows.
            double worstOut = 0.0, worstOutAt = 0.0, lo = 0.0, hi = -100.0;
            for (const auto& f : r.frames)
                if (f.timeSec >= settleSec)
                {
                    const double off = f.diffOffsetDb;
                    lo = std::min (lo, off);
                    hi = std::max (hi, off);
                    const double outside = off > 0.0 ? off : (off < -9.0 ? -9.0 - off : 0.0);
                    if (outside > worstOut)
                    {
                        worstOut = outside;
                        worstOutAt = f.timeSec;
                    }
                }
            t.check (worstOut == 0.0, fmt ("diff lookahead %2.0f ms: diffOffsetDb in every settled frame is between -9 and 0 dB: it ranged %.3f to %.3f dB%s",
                                           (double) la, lo, hi, worstOut > 0.0 ? fmt (", %.3f dB outside at %.2f s", worstOut, worstOutAt).c_str() : ""));
        }
        const double levelDiff = powerToDb (meanSquare (r.out, last3, dn) / meanSquare (treated.out, last3, dn));
        {
            // How the treated level moves over the settled stretch, in 1 s steps, against the offset.
            std::string steps;
            for (std::size_t a = settled; a + S (1.0) <= dn; a += S (1.0))
                steps += fmt (" %+.2f", powerToDb (meanSquare (treated.out, a, a + S (1.0)) / meanSquare (dsig, a, a + S (1.0))));
            t.info (fmt ("diff lookahead %2.0f ms: treated against input over the settled stretch %+.3f dB, by second:%s; Diff's offset averages %.3f dB there",
                         (double) la, powerToDb (meanSquare (treated.out, settled, dn) / meanSquare (dsig, settled, dn)), steps.c_str(), late.meanOffsetDb));
        }
        worstLevel = std::max (worstLevel, std::abs (levelDiff));
        t.check (std::abs (levelDiff) <= 0.1, fmt ("diff lookahead %2.0f ms: level over the last 3 s %+.3f dB against the treated output's (limit 0.1); treated is %+.2f dB against the input, Diff's offset there %.2f dB",
                                                   (double) la, levelDiff, overall, late.meanOffsetDb));
    }
    // Depth 0: nothing is taken out, so there is nothing to match.
    {
        RenderSpec s;
        s.params.thresholdDb = kAlwaysDb;
        s.params.depth = 0.0f;
        s.params.diff = true;
        s.collectFrames = true;
        const auto r = render (dsig, s);
        double worstOff = 0.0;
        for (const auto& f : r.frames)
            if (f.timeSec >= settleSec)
                worstOff = std::max (worstOff, (double) std::abs (f.diffOffsetDb));
        const double res = residualDb (r.out, dsig, 1.0, settled, dn);
        t.check (worstOff <= 0.02, fmt ("diff at Depth 0: largest |diffOffsetDb| once settled %.4f dB (limit 0.02); output against the input %s dB",
                                        worstOff, dbStr (res).c_str()));
    }
    t.headline = fmt ("Solo and Bypass worst residual %s dB; Diff: shape residual %s dB (limit -80), gain against offset %.3f dB (limit 0.05), level against treated %.3f dB (limit 0.1)",
                      dbStr (worst).c_str(), dbStr (worstShape).c_str(), worstGain, worstLevel);
}

//==============================================================================
// latency

void testLatency (Ctx& t)
{
    int failures = 0;
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (float la : { 0.0f, 5.0f, 10.0f, 20.0f })
        {
            const std::size_t at = 1000;
            const std::size_t n = at + S (0.1, sr) + 4096;
            Audio in = Audio::make (sr, 1, n);
            in.ch[0][at] = 1.0f;
            RenderSpec s;
            s.params.depth = 0.0f;
            s.params.lookaheadMs = la;
            s.compensateLatency = false;
            dehonk::Core core;
            const auto r = render (core, in, s);
            std::size_t peakAt = 0;
            double peak = 0.0, energy = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const double v = r.out.ch[0][i];
                energy += v * v;
                if (std::abs (v) > std::abs (peak))
                {
                    peak = v;
                    peakAt = i;
                }
            }
            const double rest = powerToDb ((energy - peak * peak) / std::max (1e-300, peak * peak));
            const long measured = (long) peakAt - (long) at;
            const double expect = (double) la * sr / 1000.0;
            const bool ok = measured == r.latency && r.latency == core.latencySamplesFor (la) && std::abs ((double) r.latency - expect) <= 1.0
                            && std::abs (peak - 1.0) < 1e-4 && rest < -100.0;
            failures += ok ? 0 : 1;
            t.check (ok, fmt ("%6.0f Hz lookahead %2.0f ms: reported %d (latencySamplesFor %d, %.1f expected), impulse at +%ld, peak %.5f, energy elsewhere %s dB",
                              sr, (double) la, r.latency, core.latencySamplesFor (la), expect, measured, peak, dbStr (rest).c_str()));
        }
    t.headline = fmt ("%d of 12 cases wrong", failures);
}

//==============================================================================
// block_size_invariance

void testBlockSizeInvariance (Ctx& t)
{
    const std::size_t n = S (4.0);
    HonkShape shape;
    shape.bump = true;
    shape.resonanceHz = { 330.0 };
    Audio in = honkSignal (kSr, 4.0, 2, shape, -20.0, 91);
    const Audio music = musicAudio (kSr, n, 2, 92);
    for (int c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < n; ++i)
            in.ch[(std::size_t) c][i] += 0.7f * music.ch[(std::size_t) c][i];

    // The same signal with 2.5 kHz clicks added, rendered with Snap on as well.
    Audio clicky = in;
    {
        const auto bursts = clickSignal (kSr, 2500.0, 3.0, 2, 4.0, 94);
        for (int c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < n; ++i)
                clicky.ch[(std::size_t) c][i] += bursts.in.ch[(std::size_t) c][i];
    }

    double worst = -400.0;
    for (int click = 0; click < 2; ++click)
    for (float la : { 0.0f, 10.0f })
    {
        const Audio& sig = click ? clicky : in;
        dehonk::Params p;
        p.snapOn = click == 1;
        // Threshold 3 dB under the signal's RMS, so the level follower and the threshold's knee
        // are in play and their state has to be block-size independent too.
        p.thresholdDb = (float) (powerToDb (meanSquare (sig, 0, n)) - 3.0);
        p.depth = 0.8f;
        p.speed = 0.8f;
        p.link = 0.5f;
        p.lookaheadMs = la;
        RenderSpec ref;
        ref.params = p;
        ref.blockSize = 512;
        ref.collectFrames = click == 1;
        ref.frameEveryTick = true;
        const auto r0 = render (sig, ref);
        if (click)
        {
            const auto cf = clickFrames (r0, 3.0);
            t.check (cf.max > 3.0, fmt ("Snap on, lookahead %2.0f ms: Snap cuts up to %.2f dB, %d events (need above 3 dB, or the comparison proves nothing for Snap)",
                                        (double) la, cf.max, cf.events));
        }

        for (int bs : { 1, 7, 32, 4096, -1 })
        {
            RenderSpec s;
            s.params = p;
            Rng rng (93);
            if (bs > 0)
                s.blockSize = bs;
            else
            {
                s.blockSize = 4096;
                s.maxBlockSize = 4096;
                s.nextBlockSize = [&rng] { return (int) std::lround (std::exp2 (rng.uniform (0.0, 12.0))); };
            }
            const auto r = render (sig, s);
            const double res = residualDb (r.out, r0.out, 1.0, 0, n);
            worst = std::max (worst, res);
            t.check (res < -90.0, fmt ("%slookahead %2.0f ms, block %s vs 512: residual %s dB (limit -90)", click ? "with clicks, Snap on, " : "", (double) la,
                                       bs > 0 ? std::to_string (bs).c_str() : "random 1-4096", dbStr (res).c_str()));
        }
    }
    t.headline = "worst residual " + dbStr (worst) + " dB (limit -90)";
}

//==============================================================================
// stability

struct StabilityResult
{
    bool finite = true;
    double peak = 0.0, tail = 0.0;
};

StabilityResult judge (const Audio& out, std::size_t tailFrom)
{
    StabilityResult s;
    s.finite = allFinite (out);
    s.peak = peakAbs (out, 0, out.length());
    s.tail = peakAbs (out, tailFrom, out.length());
    return s;
}

dehonk::Params randomParams (Rng& rng, const dehonk::Params& prev, bool changeLookahead)
{
    dehonk::Params p;
    p.depth = (float) rng.uniform();
    p.sharpness = (float) rng.uniform();
    p.speed = (float) rng.uniform();
    p.releaseMs = (float) (50.0 * std::pow (30.0, rng.uniform()));
    p.maxCutDb = (float) rng.uniform (3.0, 12.0);
    p.impactGuard = (float) rng.uniform();
    // Module switches flip now and then; edges are usually inside their limits but are sometimes
    // anywhere from 10 Hz to 5 kHz, in any order, so they cross and leave their limits.
    for (int m = 0; m < dehonk::kNumModules; ++m)
        p.moduleOn[m] = rng.uniform() < 0.1 ? ! prev.moduleOn[m] : prev.moduleOn[m];
    const double wild = rng.uniform();
    for (int e = 0; e < dehonk::kNumEdges; ++e)
        p.edgeHz[e] = wild < 0.2 ? (float) (10.0 * std::pow (500.0, rng.uniform()))
                    : wild < 0.3 ? prev.edgeHz[e]
                                 : (float) rng.uniform (dehonk::kEdgeMinHz[e], dehonk::kEdgeMaxHz[e]);
    p.link = (float) rng.uniform();
    // The mode stays for a while (as a user would leave it); the in-between Link values above
    // are only reached in Linked and Grouped.
    p.linkMode = rng.uniform() < 0.1 ? (dehonk::LinkMode) rng.uniformInt (0, 2) : prev.linkMode;
    p.dialogueOn = rng.uniform() < 0.1 ? ! prev.dialogueOn : prev.dialogueOn;
    p.dialogueAmount = (float) rng.uniform();
    p.detection = rng.uniform() < 0.1 ? (prev.detection == dehonk::Detection::fast ? dehonk::Detection::steady : dehonk::Detection::fast) : prev.detection;
    for (int m = 0; m < dehonk::kNumModules; ++m)
    {
        p.rangeTimingLocked[m] = rng.uniform() < 0.1 ? ! prev.rangeTimingLocked[m] : prev.rangeTimingLocked[m];
        p.rangeAttackMs[m] = (float) (2.0 * std::pow (50.0, rng.uniform()));
        p.rangeReleaseMs[m] = (float) (50.0 * std::pow (30.0, rng.uniform()));
    }
    p.mix = (float) rng.uniform();
    p.trimDb = (float) rng.uniform (-12.0, 12.0);
    p.thresholdDb = (float) rng.uniform (-60.0, 0.0);
    p.snapOn = rng.uniform() < 0.1 ? ! prev.snapOn : prev.snapOn;
    p.soloHonk = rng.uniform() < 0.1 ? ! prev.soloHonk : prev.soloHonk;
    p.diff = rng.uniform() < 0.05 ? ! prev.diff : prev.diff;
    p.bypass = rng.uniform() < 0.05 ? ! prev.bypass : prev.bypass;
    const float las[] = { 0.0f, 5.0f, 10.0f, 20.0f };
    p.lookaheadMs = changeLookahead ? las[rng.uniformInt (0, 3)] : prev.lookaheadMs;
    return p;
}

void testStability (Ctx& t)
{
    struct Case
    {
        const char* name;
        std::function<std::vector<float> (double, std::size_t, std::uint64_t)> gen;
        bool automate;
    };
    const std::vector<Case> cases = {
        { "silence", [] (double, std::size_t n, std::uint64_t) { return std::vector<float> (n, 0.0f); }, false },
        { "DC 0.5", [] (double, std::size_t n, std::uint64_t) { return std::vector<float> (n, 0.5f); }, false },
        { "full-scale square 110 Hz", [] (double sr, std::size_t n, std::uint64_t) { return square (sr, n, 110.0, 1.0); }, false },
        { "denormal-level noise 1e-30",
          [] (double, std::size_t n, std::uint64_t seed) {
              Rng r (seed);
              std::vector<float> x (n);
              for (auto& v : x)
                  v = (float) (1e-30 * r.gauss());
              return x;
          },
          false },
        { "full-scale noise",
          [] (double, std::size_t n, std::uint64_t seed) {
              Rng r (seed);
              std::vector<float> x (n);
              for (auto& v : x)
                  v = (float) r.uniform (-1.0, 1.0);
              return x;
          },
          false },
        { "0 dBFS sine 150 Hz", [] (double sr, std::size_t n, std::uint64_t) { return sine (sr, n, 150.0, 1.0); }, false },
        { "honk bed, every parameter random every block",
          [] (double sr, std::size_t n, std::uint64_t seed) {
              HonkShape shape;
              shape.bump = true;
              auto a = honkSignal (sr, (double) n / sr, 1, shape, -18.0, seed);
              for (auto& v : a.ch[0])
                  v = std::clamp (v, -1.0f, 1.0f);
              return a.ch[0];
          },
          true },
    };

    int runs = 0, bad = 0;
    for (const auto& c : cases)
    for (int click = 0; click < (c.automate ? 1 : 2); ++click)   // the random run switches Snap itself
    {
        StabilityResult worst;
        std::string failures;
        for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
            for (int nch = 1; nch <= 2; ++nch)
            {
                const std::size_t sig = S (1.0, sr), n = S (3.0, sr);
                Audio in = Audio::make (sr, nch, n);
                for (int ch = 0; ch < nch; ++ch)
                {
                    auto x = c.gen (sr, sig, 100 + (std::uint64_t) ch);
                    std::copy (x.begin(), x.end(), in.ch[(std::size_t) ch].begin());
                }
                RenderSpec s;
                s.params.thresholdDb = kAlwaysDb;
                s.params.snapOn = click == 1;
                s.compensateLatency = false;
                Rng rng (200 + (std::uint64_t) nch);
                std::int64_t nextLa = 0;   // lives until render() returns, which calls s.automate
                if (c.automate)
                {
                    s.blockSize = 1024;
                    s.maxBlockSize = 1024;
                    s.nextBlockSize = [&rng] { return rng.uniformInt (1, 1024); };
                    s.automate = [&rng, &nextLa, sig, sr] (std::int64_t pos, dehonk::Params& p) {
                        if (pos >= (std::int64_t) sig)
                        {
                            p = dehonk::Params {};
                            return;
                        }
                        const bool la = pos >= nextLa;
                        if (la)
                            nextLa = pos + (std::int64_t) (0.25 * sr);
                        p = randomParams (rng, p, la);
                    };
                }
                const auto r = render (in, s);
                const auto j = judge (r.out, n - S (0.5, sr));
                ++runs;
                const bool ok = j.finite && j.peak <= kPlusTwelveDb && j.tail <= 1e-5;
                if (! ok)
                {
                    ++bad;
                    failures += fmt (" [%.0f Hz %s: finite %s, peak %s dBFS, tail %s dBFS]", sr, nch == 1 ? "mono" : "stereo",
                                     j.finite ? "yes" : "NO", dbStr (ampToDb (j.peak)).c_str(), dbStr (ampToDb (j.tail)).c_str());
                }
                worst.finite = worst.finite && j.finite;
                worst.peak = std::max (worst.peak, j.finite ? j.peak : 1e30);
                worst.tail = std::max (worst.tail, j.finite ? j.tail : 1e30);
            }
        t.check (failures.empty(), fmt ("%s%s, 5 rates x mono/stereo: all finite %s, highest peak %s dBFS (limit +12), highest peak in the last 0.5 s of silence %s dBFS (limit -100)%s",
                                        c.name, c.automate ? " (Snap switched at random too)" : click ? ", Snap on" : "", worst.finite ? "yes" : "NO", dbStr (ampToDb (worst.peak)).c_str(),
                                        dbStr (ampToDb (worst.tail)).c_str(), failures.c_str()));
    }

    // prepare() again mid-stream with another rate and channel count, and reset() mid-stream.
    HonkShape shape;
    shape.bump = true;
    auto segment = [&] (double sr, int nch, double sigSec, double silenceSec) {
        Audio a = honkSignal (sr, sigSec + silenceSec, nch, shape, -18.0, 300);
        for (auto& ch : a.ch)
            std::fill (ch.begin() + (std::ptrdiff_t) S (sigSec, sr), ch.end(), 0.0f);
        return a;
    };
    {
        dehonk::Core core;
        dehonk::Params p;
        p.thresholdDb = kAlwaysDb;
        p.depth = 1.0f;
        p.lookaheadMs = 10.0f;
        core.prepare (48000.0, 512, 2);
        core.setParams (p);
        const auto a = streamThrough (core, segment (48000.0, 2, 1.0, 0.0), 512);
        core.prepare (96000.0, 512, 2);
        core.setParams (p);
        const auto b = streamThrough (core, segment (96000.0, 2, 1.0, 1.0), 512);
        core.prepare (44100.0, 256, 1);
        core.setParams (p);
        const auto c = streamThrough (core, segment (44100.0, 1, 1.0, 1.0), 256);
        const auto ja = judge (a, a.length()), jb = judge (b, b.length() - S (0.5, 96000.0)), jc = judge (c, c.length() - S (0.5, 44100.0));
        const bool ok = ja.finite && jb.finite && jc.finite && std::max ({ ja.peak, jb.peak, jc.peak }) <= kPlusTwelveDb
                        && jb.tail <= 1e-5 && jc.tail <= 1e-5;
        ++runs;
        bad += ok ? 0 : 1;
        t.check (ok, fmt ("prepare() again mid-stream (48 kHz stereo -> 96 kHz stereo -> 44.1 kHz mono): finite %s, peak %s dBFS, tails %s / %s dBFS",
                          ja.finite && jb.finite && jc.finite ? "yes" : "NO", dbStr (ampToDb (std::max ({ ja.peak, jb.peak, jc.peak }))).c_str(),
                          dbStr (ampToDb (jb.tail)).c_str(), dbStr (ampToDb (jc.tail)).c_str()));
    }
    {
        dehonk::Core core;
        dehonk::Params p;
        p.thresholdDb = kAlwaysDb;
        p.depth = 1.0f;
        p.lookaheadMs = 5.0f;
        core.prepare (48000.0, 512, 2);
        core.setParams (p);
        const auto a = streamThrough (core, segment (48000.0, 2, 1.0, 0.0), 512);
        core.reset();
        const auto b = streamThrough (core, segment (48000.0, 2, 1.0, 1.0), 512);
        const auto ja = judge (a, a.length()), jb = judge (b, b.length() - S (0.5));
        const bool ok = ja.finite && jb.finite && std::max (ja.peak, jb.peak) <= kPlusTwelveDb && jb.tail <= 1e-5;
        ++runs;
        bad += ok ? 0 : 1;
        t.check (ok, fmt ("reset() mid-stream: finite %s, peak %s dBFS, tail %s dBFS", ja.finite && jb.finite ? "yes" : "NO",
                          dbStr (ampToDb (std::max (ja.peak, jb.peak))).c_str(), dbStr (ampToDb (jb.tail)).c_str()));
    }
    t.headline = fmt ("%d of %d runs failed", bad, runs);
}

//==============================================================================
// automation_clicks

/** automation_clicks' first metric: each 10 ms window (W samples, hopping by W/2) of the output
    above 6 kHz is fitted by least squares from the input, the input times time, and the input's
    derivative and derivative times time; returns the largest share of the window's energy the
    fit cannot explain, in dB, and where (seconds). */
std::pair<double, double> hfUnexplained (const std::vector<double>& hpIn, const std::vector<double>& hpOut, std::size_t start, std::size_t W)
{
    const std::size_t len = std::min (hpIn.size(), hpOut.size());
    double hfMax = -400.0, hfAt = 0.0;
    for (std::size_t w0 = start; w0 + W < len; w0 += W / 2)
    {
        double A[4][4] = {}, B[4] = {}, yy = 0.0;
        for (std::size_t i = 0; i < W; ++i)
        {
            const std::size_t j = w0 + i;
            const double tau = ((double) i - 0.5 * (double) W) / (double) W;
            const double a = hpIn[j], da = 0.5 * (hpIn[j + 1] - hpIn[j - 1]);
            const double reg[4] = { a, a * tau, da, da * tau };
            const double yv = hpOut[j];
            for (int p = 0; p < 4; ++p)
            {
                B[p] += reg[p] * yv;
                for (int q = 0; q < 4; ++q)
                    A[p][q] += reg[p] * reg[q];
            }
            yy += yv * yv;
        }
        if (yy <= 0.0)
            continue;
        // Solve A g = B by Gaussian elimination with partial pivoting.
        double M[4][5];
        for (int p = 0; p < 4; ++p)
        {
            for (int q = 0; q < 4; ++q)
                M[p][q] = A[p][q];
            M[p][4] = B[p];
        }
        bool singular = false;
        for (int c = 0; c < 4 && ! singular; ++c)
        {
            int piv = c;
            for (int p = c + 1; p < 4; ++p)
                if (std::abs (M[p][c]) > std::abs (M[piv][c]))
                    piv = p;
            if (std::abs (M[piv][c]) < 1e-300)
            {
                singular = true;
                break;
            }
            for (int q = 0; q < 5; ++q)
                std::swap (M[c][q], M[piv][q]);
            for (int p = 0; p < 4; ++p)
                if (p != c)
                {
                    const double f = M[p][c] / M[c][c];
                    for (int q = c; q < 5; ++q)
                        M[p][q] -= f * M[c][q];
                }
        }
        if (singular)
            continue;
        double explained = 0.0;
        for (int p = 0; p < 4; ++p)
            explained += (M[p][4] / M[p][p]) * B[p];
        const double resid = std::max (0.0, yy - explained);
        const double db = powerToDb (resid / yy);
        if (db > hfMax)
        {
            hfMax = db;
            hfAt = (double) w0 / kSr;
        }
    }

    return { hfMax, hfAt };
}


void testAutomationClicks (Ctx& t)
{
    // The signal: stereo, a steady 200 Hz sine at -12 dBFS peak in both channels plus independent
    // pink noise at -30 dBFS RMS in each (so Link has something to do). 200 Hz sits on the default
    // Tub/Fog edge, so sweeping that edge and switching modules moves the notch from one module to
    // the other, or takes it away. parentRule is set to 0 so
    // the detector cuts the sine (with no subharmonic below it, the default rule may leave it
    // alone, and then nothing would move and the test would prove nothing). Lookahead stays off:
    // changing it changes the latency, which is not smooth by design.
    const std::size_t n = S (8.0);
    Audio in = Audio::make (kSr, 2, n);
    for (int c = 0; c < 2; ++c)
    {
        auto& x = in.ch[(std::size_t) c];
        x = sine (kSr, n, 200.0, dbToAmp (-12.0));
        auto pink = pinkNoise (kSr, n, 101 + (std::uint64_t) c);
        normaliseRms (pink, -30.0);
        for (std::size_t i = 0; i < n; ++i)
            x[i] += pink[i];
    }

    struct Run
    {
        const char* name;
        std::function<void (double, dehonk::Params&)> apply;
        std::function<double (double)> trimDb;
        double hfLimitDb = -40.0;   // tighter where -40 would not notice the smoothing being taken out
    };
    auto sweep = [] (double tSec, double hz, double ph) { return 0.5 + 0.5 * std::sin (2.0 * std::numbers::pi * hz * tSec + ph); };
    auto noTrim = [] (double) { return 0.0; };
    auto trimOf = [sweep] (double tSec) { return -12.0 + 24.0 * sweep (tSec, 0.29, 0.0); };
    // On for 0.7 s, off for 0.7 s. SPEC section 4: Diff crossfades so the switch does not click,
    // and Bypass is click-free.
    auto toggled = [] (double tSec) { return std::fmod (tSec, 1.4) >= 0.7; };
    // Each edge moves between its limits, log scale, at its own rate.
    auto sweepEdges = [sweep] (double s, dehonk::Params& p) {
        for (int e = 0; e < dehonk::kNumEdges; ++e)
            p.edgeHz[e] = (float) (dehonk::kEdgeMinHz[e] * std::pow (dehonk::kEdgeMaxHz[e] / dehonk::kEdgeMinHz[e], sweep (s, 0.19 + 0.07 * e, e)));
    };
    // Each module switches on and off at its own rate, so every combination comes up.
    auto toggleModules = [] (double s, dehonk::Params& p) {
        const double period[dehonk::kNumModules] = { 0.9, 1.3, 1.1, 0.7 };
        for (int m = 0; m < dehonk::kNumModules; ++m)
            p.moduleOn[m] = std::fmod (s + 0.2 * m, period[m]) < 0.5 * period[m];
    };
    const std::vector<Run> runs = {
        { "no automation", [] (double, dehonk::Params&) {}, noTrim },
        { "depth 0-100%", [sweep] (double s, dehonk::Params& p) { p.depth = (float) sweep (s, 0.3, 0.0); }, noTrim },
        { "every edge swept across its range", sweepEdges, noTrim },
        { "module switches toggled", toggleModules, noTrim },
        { "mix 0-100%", [sweep] (double s, dehonk::Params& p) { p.mix = (float) sweep (s, 0.37, 0.0); }, noTrim },
        { "trim -12 to +12 dB", [trimOf] (double s, dehonk::Params& p) { p.trimDb = (float) trimOf (s); }, trimOf },
        { "threshold -60 to 0 dBFS", [sweep] (double s, dehonk::Params& p) { p.thresholdDb = (float) (-60.0 + 60.0 * sweep (s, 0.27, 0.0)); }, noTrim },
        { "link 0-100%", [sweep] (double s, dehonk::Params& p) { p.link = (float) sweep (s, 0.43, 0.0); }, noTrim },
        // -50 for Max Cut: stepped without its 15 ms smoothing it measures -43 dB, which -40 would pass.
        { "max cut 3-12 dB", [sweep] (double s, dehonk::Params& p) { p.maxCutDb = (float) (3.0 + 9.0 * sweep (s, 0.61, 0.0)); }, noTrim, -50.0 },
        { "max cut 12 dB / 3 dB every 0.7 s", [toggled] (double s, dehonk::Params& p) { p.maxCutDb = toggled (s) ? 3.0f : 12.0f; }, noTrim, -50.0 },
        { "release 50-1500 ms", [sweep] (double s, dehonk::Params& p) { p.releaseMs = (float) (50.0 * std::pow (30.0, sweep (s, 0.47, 0.0))); }, noTrim },
        { "impact guard 0-100%", [sweep] (double s, dehonk::Params& p) { p.impactGuard = (float) sweep (s, 0.83, 0.0); }, noTrim },
        { "diff on/off every 0.7 s", [toggled] (double s, dehonk::Params& p) { p.diff = toggled (s); }, noTrim },
        { "diff on/off every 60 ms (switched again inside its cross-fade)", [] (double s, dehonk::Params& p) { p.diff = std::fmod (s, 0.12) >= 0.06; }, noTrim },
        // (Not with Trim moving: Diff plays the untreated signal without Trim, so switching it then
        // is a change of level by design, which this measure would call a click.)
        { "diff on/off every 0.7 s while mix and depth move",
          [toggled, sweep] (double s, dehonk::Params& p) { p.diff = toggled (s); p.mix = (float) sweep (s, 0.37, 0.0); p.depth = (float) sweep (s, 0.3, 1.0); }, noTrim },
        { "bypass on/off every 0.7 s", [toggled] (double s, dehonk::Params& p) { p.bypass = toggled (s); }, noTrim },
        { "detection Fast / Steady every 0.7 s", [toggled] (double s, dehonk::Params& p) { p.detection = toggled (s) ? dehonk::Detection::steady : dehonk::Detection::fast; }, noTrim },
        { "detection Fast / Steady every 60 ms", [] (double s, dehonk::Params& p) { p.detection = std::fmod (s, 0.12) >= 0.06 ? dehonk::Detection::steady : dehonk::Detection::fast; }, noTrim },
        { "link mode Linked / Grouped / Unlinked every 0.35 s", [] (double s, dehonk::Params& p) { p.linkMode = (dehonk::LinkMode) (((int) (s / 0.35)) % 3); }, noTrim },
        { "range timing: locks switched, attack and release swept",
          [sweep] (double s, dehonk::Params& p) {
              for (int m = 0; m < dehonk::kNumModules; ++m)
              {
                  p.rangeTimingLocked[m] = std::fmod (s + 0.13 * m, 0.5 + 0.1 * m) < 0.25;
                  p.rangeAttackMs[m] = (float) (2.0 * std::pow (50.0, sweep (s, 0.7 + 0.2 * m, 0.0)));
                  p.rangeReleaseMs[m] = (float) (50.0 * std::pow (30.0, sweep (s, 0.9 + 0.3 * m, 1.0)));
              }
          },
          noTrim },
        { "everything at once",
          [sweep, trimOf, sweepEdges, toggleModules] (double s, dehonk::Params& p) {
              p.depth = (float) sweep (s, 0.3, 0.0);
              sweepEdges (s, p);
              toggleModules (s, p);
              p.mix = (float) sweep (s, 0.37, 0.5);
              p.trimDb = (float) trimOf (s);
              p.sharpness = (float) sweep (s, 0.41, 2.0);
              p.speed = (float) sweep (s, 0.53, 3.0);
              p.link = (float) sweep (s, 0.43, 4.0);
          },
          trimOf },
    };

    const std::size_t W = S (0.010), start = S (0.5);
    double anyCut = 0.0, worstHf = -400.0, worstJump = -400.0;
    struct Measured
    {
        double hfMax, hfAt, jumpMax, jumpAt;
    };
    // Renders each run on sig and measures it. checkHf false: the residual above 6 kHz is only
    // returned, for a signal on which Snap's own action leaves some there (judged by the caller).
    auto runAll = [&] (const Audio& sig, const std::vector<Run>& runList, const dehonk::Params& base, const char* label, bool checkHf) {
        const std::size_t len = sig.length();
        std::vector<Measured> measured;
        std::vector<double> hpIns[2], dIns[2];
        for (int c = 0; c < 2; ++c)
        {
            const auto& x = sig.ch[(std::size_t) c];
            hpIns[c] = highpass4 (x, kSr, 6000.0);
            dIns[c].assign (len, 0.0);
            for (std::size_t i = 1; i < len; ++i)
                dIns[c][i] = std::abs ((double) x[i] - x[i - 1]);
        }

        for (const auto& run : runList)
        {
            RenderSpec s;
            s.params = base;
            s.blockSize = 32;
            s.collectFrames = true;
            s.frameEveryTick = true;
            s.useTuning = true;
            s.tuning = defaultTuning();
            s.tuning.parentRule = 0.0f;
            const auto apply = run.apply;
            s.automate = [apply] (std::int64_t pos, dehonk::Params& p) { apply ((double) pos / kSr, p); };
            const auto r = render (sig, s);
            const auto cs = cutStats (r, 0.5);
            anyCut = std::max (anyCut, cs.maxTotal);

            double hfMax = -400.0, hfAt = 0.0, jumpMax = -400.0, jumpAt = 0.0;
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto& y = r.out.ch[(std::size_t) ch];
                const auto hpOut = highpass4 (y, kSr, 6000.0);
                const auto& hpIn = hpIns[ch];
                const auto& dIn = dIns[ch];

                // Metric 1, high-frequency residual. De-Honk only acts below about 2 kHz, so above 6 kHz
                // the output must be the input times a slowly changing gain (trim), with at most a tiny
                // phase shift from the skirts of the bells. Each 10 ms window of the output is fitted by
                // least squares from four regressors: the input, the input times time (gain slope), and
                // the input's derivative and derivative times time (small phase shift). Whatever the fit
                // cannot explain is a click or zipper noise. Reported relative to the output's own energy
                // above 6 kHz in that window.
                {
                    const auto [m, at] = hfUnexplained (hpIn, hpOut, start, W);
                    if (m > hfMax)
                    {
                        hfMax = m;
                        hfAt = at;
                    }
                }

                // Metric 2, sample-to-sample jump. In each 10 ms window the largest |y[n]-y[n-1]| is compared
                // with the input's largest jump (window widened by 1 ms) times the largest trim gain
                // requested within 50 ms. Mix and the cuts cannot make jumps bigger than the input's
                // except through phase changes, so 1.5 dB of margin is allowed for those.
                const std::size_t widen = S (0.001);
                for (std::size_t w0 = start; w0 + W <= len; w0 += W)
                {
                    double dy = 0.0, dx = 0.0;
                    for (std::size_t i = w0; i < w0 + W; ++i)
                        dy = std::max (dy, (double) std::abs (y[i] - y[i - 1]));
                    for (std::size_t i = w0 - widen; i < std::min (len, w0 + W + widen); ++i)
                        dx = std::max (dx, dIn[i]);
                    double g = 0.0;
                    const double tc = (double) w0 / kSr;
                    for (int k = -5; k <= 5; ++k)
                        g = std::max (g, dbToAmp (run.trimDb (tc + 0.01 * k)));
                    const double db = ampToDb (dy / std::max (1e-30, g * dx));
                    if (db > jumpMax)
                    {
                        jumpMax = db;
                        jumpAt = tc;
                    }
                }
            }
            worstJump = std::max (worstJump, jumpMax);
            if (checkHf)
                worstHf = std::max (worstHf, hfMax);
            if (checkHf)
                t.check (hfMax < run.hfLimitDb, fmt ("%s%s: unexplained energy above 6 kHz, worst 10 ms window %s dB at %.2f s (limit %.0f)", label, run.name,
                                                     dbStr (hfMax).c_str(), hfAt, run.hfLimitDb));
            const double clickMax = clickFrames (r, 1.0, 0.5).max;
            t.check (jumpMax < 1.5, fmt ("%s%s: largest sample jump vs input's x trim gain %s dB at %.2f s (limit +1.5); max cut %.1f dB%s",
                                         label, run.name, dbStr (jumpMax).c_str(), jumpAt, cs.maxTotal,
                                         clickMax > 0.0 ? fmt (", Snap's largest %.1f dB", clickMax).c_str() : ""));
            measured.push_back ({ hfMax, hfAt, jumpMax, jumpAt });
        }
        return measured;
    };

    dehonk::Params steady;
    steady.thresholdDb = kAlwaysDb;
    steady.depth = 1.0f;
    runAll (in, runs, steady, "", true);

    // Diff is level matched (SPEC 13.6), so switching it changes the level by the offset, up to
    // 9 dB. The metric above allows a gain that moves in a straight line within each 10 ms window,
    // which a fast fade of several dB is not. For comparison, the same metric on the input above
    // 6 kHz times an ideal gain that steps by the offset the core reported at each switch and is
    // smoothed by a one-pole of 5 ms (the core's output-mode smoothing) and of 50 ms. Information
    // only: if the 5 ms figure is close to the measured one, what the metric sees is the fade
    // itself, not a discontinuity.
    {
        RenderSpec s;
        s.params = steady;
        s.blockSize = 32;
        s.collectFrames = true;
        s.useTuning = true;
        s.tuning = defaultTuning();
        s.tuning.parentRule = 0.0f;
        s.automate = [toggled] (std::int64_t pos, dehonk::Params& p) { p.diff = toggled ((double) pos / kSr); };
        const auto r = render (in, s);
        double offMin = 0.0, offMax = -100.0;
        for (const auto& f : r.frames)
            if (f.timeSec > 0.5)
            {
                offMin = std::min (offMin, (double) f.diffOffsetDb);
                offMax = std::max (offMax, (double) f.diffOffsetDb);
            }
        const auto hpIn = highpass4 (in.ch[0], kSr, 6000.0);
        const auto hpOut = highpass4 (r.out.ch[0], kSr, 6000.0);
        const auto measured = hfUnexplained (hpIn, hpOut, start, W);
        std::string ideal;
        for (double tauMs : { 5.0, 50.0 })
        {
            const double c = 1.0 - std::exp (-1.0 / (tauMs * 0.001 * kSr));
            std::vector<double> y (hpIn.size());
            double g = 1.0;
            std::size_t fi = 0;
            for (std::size_t i = 0; i < y.size(); ++i)
            {
                const double tSec = (double) i / kSr;
                while (fi + 1 < r.frames.size() && r.frames[fi].timeSec < tSec)
                    ++fi;
                const double target = toggled (tSec) ? dbToAmp (r.frames.empty() ? 0.0 : r.frames[fi].diffOffsetDb) : 1.0;
                g += c * (target - g);
                y[i] = hpIn[i] * g;
            }
            const auto m = hfUnexplained (hpIn, y, start, W);
            ideal += fmt ("%s%.0f ms fade %s dB", ideal.empty() ? "" : ", ", tauMs, dbStr (m.first).c_str());
        }
        t.info (fmt ("diff on/off every 0.7 s, channel 0: Diff's offset %.2f to %.2f dB; unexplained above 6 kHz %s dB at %.2f s; the same metric on an ideal gain change of that size: %s",
                     offMin, offMax, dbStr (measured.first).c_str(), measured.second, ideal.c_str()));
    }

    // Snap acts above 6 kHz a little (its band's upper skirt), and it acts on the noise in this
    // signal, so with Snap on the output above 6 kHz is no longer the input times a slow gain.
    // Switching it is judged against leaving it on: the switch must add nothing of its own.
    const std::vector<Run> switchRuns = {
        { "Snap always on", [] (double, dehonk::Params& p) { p.snapOn = true; }, noTrim },
        { "Snap on/off every 0.7 s", [toggled] (double s, dehonk::Params& p) { p.snapOn = toggled (s); }, noTrim },
    };
    const auto sm = runAll (in, switchRuns, steady, "steady signal, Tub+Fog on: ", false);
    t.check (sm[1].hfMax <= std::max (-40.0, sm[0].hfMax + 3.0),
             fmt ("steady signal: switching Snap every 0.7 s leaves %s dB unexplained above 6 kHz (at %.2f s), against %s dB with it always on (at %.2f s) (limit: -40 dB, or 3 dB more than always on)",
                  dbStr (sm[1].hfMax).c_str(), sm[1].hfAt, dbStr (sm[0].hfMax).c_str(), sm[0].hfAt));

    // The same switch on a signal with clicks in it, with only Snap on: bursts 1.5 ms before every
    // switch (so it is switched off, and on, in the middle of a click) and halfway between switches.
    std::vector<double> times;
    for (int k = 1; k <= 11; ++k)
    {
        times.push_back (0.7 * k - 0.0015);
        times.push_back (0.7 * k + 0.35);
    }
    const auto clicky = burstSignal (kSr, 8.0, 2, BurstKind::tone, 2500.0, 3.0, 0.5, -22.0, 171, 0.0, 0.0, times);
    dehonk::Params clickBase = clickOnly (1.0f);
    clickBase.thresholdDb = kAlwaysDb;
    const auto cm = runAll (clicky.in, switchRuns, clickBase, "clicks over noise, only Snap on: ", false);
    t.check (cm[1].hfMax <= std::max (-40.0, cm[0].hfMax + 3.0),
             fmt ("clicks over noise: switching Snap every 0.7 s leaves %s dB unexplained above 6 kHz (at %.2f s), against %s dB with it always on (at %.2f s) (limit: -40 dB, or 3 dB more than always on)",
                  dbStr (cm[1].hfMax).c_str(), cm[1].hfAt, dbStr (cm[0].hfMax).c_str(), cm[0].hfAt));
    t.check (anyCut >= 1.0, fmt ("the detector cut at least 1 dB somewhere during the runs (largest %.2f dB); without a cut nothing moved and the test is empty",
                                 anyCut));
    t.headline = fmt ("worst HF residual %s dB (limit -40), worst jump %s dB (limit +1.5), largest cut %.1f dB; switching Snap adds %+.1f dB (steady) and %+.1f dB (clicks) above 6 kHz to leaving it on (limit +3)",
                      dbStr (worstHf).c_str(), dbStr (worstJump).c_str(), anyCut, sm[1].hfMax - sm[0].hfMax, cm[1].hfMax - cm[0].hfMax);
}

//==============================================================================
// sample_rate_consistency

void testSampleRateConsistency (Ctx& t)
{
    // The effect_on_peaks signal. Made at each rate from the same seed: at 48, 96 and 192 kHz the
    // synthesis uses the same frequency grid and the same random draws, so below 24 kHz it is the
    // same signal; at 44.1 kHz it is another draw of the same noise.
    const double settle = 2.0;

    struct Result
    {
        double sr;
        std::vector<float> bandHz;
        std::vector<double> frameCut, realised;
    };
    std::vector<Result> results;
    for (double sr : { 48000.0, 44100.0, 96000.0, 192000.0 })
    {
        const Audio in = peaksSignal (sr, 8.0, 1, { 151.0, 247.0 }, -12.0, 111);
        RenderSpec s;
        s.params = alwaysOn();
        s.collectFrames = true;
        const auto r = render (in, s);
        const auto cs = cutStats (r, settle);
        const Spectrum si = powerSpectrum (in, S (settle, sr), in.length());
        const Spectrum so = powerSpectrum (r.out, S (settle, sr), in.length());
        Result res { sr, r.bandHz, cs.mean, {} };
        for (float hz : r.bandHz)
            res.realised.push_back (si.centredDb (hz, 1.0 / 24.0) - so.centredDb (hz, 1.0 / 24.0));
        if (res.frameCut.size() < r.bandHz.size())
            res.frameCut.resize (r.bandHz.size(), 0.0);
        results.push_back (std::move (res));
    }

    const auto& ref = results[0];
    const double refMax = ref.frameCut.empty() ? 0.0 : *std::max_element (ref.frameCut.begin(), ref.frameCut.end());
    t.info (fmt ("48 kHz mean cut per band: %s", bandList (ref.bandHz, ref.frameCut, 100.0, 1000.0).c_str()));
    t.check (refMax >= 1.0, fmt ("48 kHz: largest mean cut %.2f dB; the comparison needs at least 1 dB of cut to mean anything", refMax));
    double worst = 0.0;
    for (std::size_t i = 1; i < results.size(); ++i)
    {
        const auto& r = results[i];
        const bool sameBands = r.bandHz == ref.bandHz;
        double dFrame = 0.0, dReal = 0.0, fHz = 0.0, rHz = 0.0;
        for (std::size_t k = 0; k < std::min (r.bandHz.size(), ref.bandHz.size()); ++k)
        {
            if (std::abs (r.frameCut[k] - ref.frameCut[k]) >= std::abs (dFrame))
            {
                dFrame = r.frameCut[k] - ref.frameCut[k];
                fHz = r.bandHz[k];
            }
            if (ref.bandHz[k] < 0.45 * std::min (r.sr, ref.sr) && std::abs (r.realised[k] - ref.realised[k]) >= std::abs (dReal))
            {
                dReal = r.realised[k] - ref.realised[k];
                rHz = r.bandHz[k];
            }
        }
        worst = std::max ({ worst, std::abs (dFrame), std::abs (dReal) });
        t.check (sameBands, fmt ("%.1f kHz: band centres %s those at 48 kHz", r.sr / 1000.0, sameBands ? "identical to" : "DIFFER from"));
        t.check (std::abs (dFrame) < 1.5, fmt ("%.1f kHz vs 48 kHz: largest difference in mean cut (display) %+.2f dB at %.0f Hz (limit 1.5)",
                                               r.sr / 1000.0, dFrame, fHz));
        t.check (std::abs (dReal) < 1.5, fmt ("%.1f kHz vs 48 kHz: largest difference in realised cut (own analysis) %+.2f dB at %.0f Hz (limit 1.5)",
                                              r.sr / 1000.0, dReal, rHz));
    }

    // Snap: the snap_reduces_clicks signal made at each rate, reduction of the band peak.
    double clickRef = 0.0, clickWorst = 0.0;
    std::string clickList;
    for (double sr : { 48000.0, 44100.0, 96000.0, 192000.0 })
    {
        const auto sig = clickSignal (sr, 2500.0, 3.0, 1, 10.0, 401);
        RenderSpec s;
        s.params = clickOnly();
        const double red = -burstChange (sig, render (sig.in, s).out).bandMean;
        clickList += fmt ("%s%.1f kHz %.2f dB", clickList.empty() ? "" : ", ", sr / 1000.0, red);
        if (sr == 48000.0)
        {
            clickRef = red;
            t.check (red >= 1.0, fmt ("Snap at 48 kHz: 2.5 kHz clicks lose %.2f dB of band peak (need at least 1 for the comparison to mean anything)", red));
            continue;
        }
        clickWorst = std::max (clickWorst, std::abs (red - clickRef));
        t.check (std::abs (red - clickRef) < 2.0, fmt ("Snap at %.1f kHz: 2.5 kHz clicks lose %.2f dB of band peak, %+.2f dB from 48 kHz (limit 2)", sr / 1000.0, red,
                                                       red - clickRef));
    }
    t.headline = fmt ("worst difference %.2f dB (limit 1.5), 48 kHz largest cut %.2f dB; Snap %s, worst difference %.2f dB (limit 2)", worst, refMax,
                      clickList.c_str(), clickWorst);
}

//==============================================================================
// notch_count

/** Twelve wavering tones between 110 and 400 Hz over the bass-heavy bed, each 2 to 12 dB above the
    bed in its own 1/12-octave band, the levels wandering independently over a second or two, so
    which peaks are the most prominent keeps changing. Mono, -12 dBFS RMS. */
Audio denseSignal (double seconds, std::uint64_t seed)
{
    const std::size_t n = S (seconds);
    Rng rng (seed);
    std::vector<double> hz;
    std::vector<std::vector<float>> levels;
    for (int i = 0; i < 12; ++i)
    {
        hz.push_back (110.0 * std::pow (400.0 / 110.0, ((double) i + rng.uniform (0.2, 0.8)) / 12.0));
        levels.push_back (slowLevelDb (kSr, n, 2.0, 12.0, rng.uniform (1.0, 2.5), seed * 7 + (std::uint64_t) i));
    }
    return tonesOverBed (kSr, n, hz, levels, -12.0, seed);
}

/** The brief's counting rule: band k holds a notch in frame f when its cut is deeper than minDb
    and within tolDb of its own largest cut over the previous windowSec (current frame included).
    A notch that is being let go drops out of the count once it has fallen tolDb below where it
    was, whatever the core's selection loop wants this tick, so a notch that hops between
    neighbouring bands is counted on both bands while the old one is still near full depth. */
std::vector<std::vector<char>> recentNotches (const RenderResult& r, double minDb, double windowSec, double tolDb)
{
    const auto nb = (std::size_t) r.numBands;
    const std::size_t nf = r.frames.size();
    std::vector<std::vector<char>> on (nf, std::vector<char> (nb, 0));
    std::vector<std::size_t> dq (nf);   // monotonic deque of frame indices, one band at a time
    for (std::size_t k = 0; k < nb; ++k)
    {
        std::size_t head = 0, tail = 0;
        for (std::size_t f = 0; f < nf; ++f)
        {
            const double c = r.frames[f].cutDb[k];
            while (tail > head && (double) r.frames[dq[tail - 1]].cutDb[k] <= c)
                --tail;
            dq[tail++] = f;
            while (r.frames[dq[head]].timeSec < r.frames[f].timeSec - windowSec - 1e-9)
                ++head;
            const double mx = r.frames[dq[head]].cutDb[k];
            on[f][k] = (c > minDb && c >= mx - tolDb) ? 1 : 0;
        }
    }
    return on;
}

void testNotchCount (Ctx& t)
{
    const Audio in = denseSignal (12.0, 141);
    const dehonk::Tuning tune = defaultTuning();
    const int perModule = (int) std::lround (tune.notchesPerModule);
    const double spacingOct = tune.notchSpacingOctaves;

    std::string head;
    for (int config = 0; config < 2; ++config)
    {
        RenderSpec s;
        s.params = alwaysOn();
        if (config == 1)
            for (auto& on : s.params.moduleOn)
                on = true;
        s.blockSize = 8;   // a frame at every detector tick
        s.collectFrames = true;
        const auto r = render (in, s);
        const auto h = heldNotches (r, 1.0);
        const auto a = recentNotches (r, 1.0, 0.100, 1.0);
        const std::string label = moduleList (s.params);
        t.check (h.everyTick, fmt ("%s: display frames are consecutive detector ticks (%zu frames, %.2f ms apart); the counting below needs that",
                                   label.c_str(), r.frames.size(), 1000.0 * h.framePeriodSec));

        std::vector<int> owner ((std::size_t) r.numBands);
        for (int k = 0; k < r.numBands; ++k)
            owner[(std::size_t) k] = countingModule (r.bandHz[(std::size_t) k], s.params);

        // Counted per frame three ways: the brief's rule (pass/fail), notches the core still holds
        // (wanted cut above zero, told apart by the release rate), and every notch deeper than 1 dB.
        int maxAct[dehonk::kNumModules] {}, maxHeld[dehonk::kNumModules] {}, maxRaw[dehonk::kNumModules] {};
        std::size_t over = 0, overHeld = 0, overRaw = 0, frames = 0;
        double closest = 99.0, closestAt = 0.0, closestHeld = 99.0, closestAny = 99.0;
        std::size_t closeFrames = 0, deepFrames = 0;
        double worstPair = 0.0, worstPairAt = 0.0;
        std::string firstOver, closestDesc, worstPairDesc;
        const int spacingBands = std::max (1, (int) std::lround (spacingOct * dehonk::kBandsPerOctave));
        const double capDb = (double) s.params.maxCutDb;
        struct Moment
        {
            std::size_t firstFrame, lastFrame;
            int frames;
            double startSec, endSec;
            std::string kinds, notchesAtStart;
        };
        std::vector<Moment> moments;
        auto pairMin = [] (const std::vector<int>& b) {
            double d = 99.0;
            for (std::size_t i = 1; i < b.size(); ++i)
                d = std::min (d, (double) (b[i] - b[i - 1]) / dehonk::kBandsPerOctave);
            return d;
        };
        for (std::size_t f = 0; f < r.frames.size(); ++f)
        {
            if (r.frames[f].timeSec < 1.0)
                continue;
            ++frames;
            int act[dehonk::kNumModules] {}, held[dehonk::kNumModules] {}, raw[dehonk::kNumModules] {};
            std::vector<int> actBands, heldBands, rawBands;
            for (int k = 0; k < r.numBands; ++k)
            {
                const int m = owner[(std::size_t) k];
                if (m < 0)
                    continue;
                if (r.frames[f].cutDb[(std::size_t) k] > 1.0f)
                {
                    ++raw[m];
                    rawBands.push_back (k);
                }
                if (h.held[f][(std::size_t) k])
                {
                    ++held[m];
                    heldBands.push_back (k);
                }
                if (a[f][(std::size_t) k])
                    actBands.push_back (k);
            }
            // A peak that moves to a band nearby is handed from one notch to the next: the old one
            // lets go as the new one grows, and together they are one notch on the move. So open
            // bands (any cut at all) closer than the spacing are chained into one group, the new
            // notch included while it is still shallow, and a group counts once, for the module
            // of its deepest band, if any band in it counts by the rule above.
            {
                std::vector<int> open;
                for (int k = 0; k < r.numBands; ++k)
                    if (owner[(std::size_t) k] >= 0 && r.frames[f].cutDb[(std::size_t) k] > 0.01f)
                        open.push_back (k);
                for (std::size_t i = 0; i < open.size();)
                {
                    std::size_t j = i, deepest = i;
                    bool counts = a[f][(std::size_t) open[i]] != 0;
                    while (j + 1 < open.size() && open[j + 1] - open[j] < spacingBands)
                    {
                        ++j;
                        counts = counts || a[f][(std::size_t) open[j]] != 0;
                        if (r.frames[f].cutDb[(std::size_t) open[j]] > r.frames[f].cutDb[(std::size_t) open[deepest]])
                            deepest = j;
                    }
                    if (counts)
                        ++act[owner[(std::size_t) open[deepest]]];
                    i = j + 1;
                }
            }
            // ... and what matters about two notches that close is what they add up to: never more
            // than one notch may cut (Max Cut), within a quarter of a dB.
            double pairSum = 0.0;
            int pairA = -1, pairB = -1;
            for (int i = 0; i < r.numBands; ++i)
                for (int j = i + 1; j < std::min (r.numBands, i + spacingBands); ++j)
                {
                    const double sum = (double) r.frames[f].cutDb[(std::size_t) i] + (double) r.frames[f].cutDb[(std::size_t) j];
                    if (r.frames[f].cutDb[(std::size_t) i] > 0.0f && r.frames[f].cutDb[(std::size_t) j] > 0.0f && sum > pairSum)
                    {
                        pairSum = sum;
                        pairA = i;
                        pairB = j;
                    }
                }
            if (pairSum > worstPair)
            {
                worstPair = pairSum;
                worstPairAt = r.frames[f].timeSec;
                worstPairDesc = fmt ("%.0f Hz %.2f dB + %.0f Hz %.2f dB", (double) r.bandHz[(std::size_t) pairA], (double) r.frames[f].cutDb[(std::size_t) pairA],
                                     (double) r.bandHz[(std::size_t) pairB], (double) r.frames[f].cutDb[(std::size_t) pairB]);
            }
            const bool tooDeep = pairSum > capDb + 0.25;
            bool bad = false, badHeld = false, badRaw = false;
            for (int m = 0; m < dehonk::kNumModules; ++m)
            {
                maxAct[m] = std::max (maxAct[m], act[m]);
                maxHeld[m] = std::max (maxHeld[m], held[m]);
                maxRaw[m] = std::max (maxRaw[m], raw[m]);
                bad = bad || act[m] > perModule;
                badHeld = badHeld || held[m] > perModule;
                badRaw = badRaw || raw[m] > perModule;
            }
            over += bad ? 1 : 0;
            overHeld += badHeld ? 1 : 0;
            overRaw += badRaw ? 1 : 0;
            if (bad && firstOver.empty())
            {
                firstOver = fmt ("first at %.3f s:", r.frames[f].timeSec);
                for (int k : actBands)
                    firstOver += fmt (" %.0f Hz %.2f dB (%s)", (double) r.bandHz[(std::size_t) k], (double) r.frames[f].cutDb[(std::size_t) k],
                                      moduleName (owner[(std::size_t) k]));
            }
            const double d = pairMin (actBands);
            if (d < spacingOct - 1e-9)
                ++closeFrames;
            deepFrames += tooDeep ? 1 : 0;
            // Every offending frame, for the report: grouped into moments of consecutive frames.
            if (bad || tooDeep)
            {
                std::string desc;
                for (int k : actBands)
                    desc += fmt (" %.1f Hz %.2f dB (%s, %s)", (double) r.bandHz[(std::size_t) k], (double) r.frames[f].cutDb[(std::size_t) k],
                                 moduleName (owner[(std::size_t) k]), h.held[f][(std::size_t) k] ? "held" : "releasing");
                const std::string kind = std::string (bad ? "over count" : "") + (bad && tooDeep ? " + " : "")
                                         + (tooDeep ? fmt ("two within the spacing add to %.2f dB", pairSum) : "");
                if (! moments.empty() && moments.back().lastFrame + 1 == f)
                {
                    moments.back().lastFrame = f;
                    moments.back().frames++;
                    moments.back().endSec = r.frames[f].timeSec;
                    if (moments.back().kinds.find (kind) == std::string::npos)
                        moments.back().kinds += "; " + kind;
                }
                else
                    moments.push_back ({ f, f, 1, r.frames[f].timeSec, r.frames[f].timeSec, kind, desc });
            }
            if (d < closest)
            {
                closest = d;
                closestAt = r.frames[f].timeSec;
                closestDesc.clear();
                for (int k : actBands)
                    closestDesc += fmt (" %.0f Hz %.2f dB", (double) r.bandHz[(std::size_t) k], (double) r.frames[f].cutDb[(std::size_t) k]);
            }
            closestHeld = std::min (closestHeld, pairMin (heldBands));
            closestAny = std::min (closestAny, pairMin (rawBands));
        }
        std::string perMod;
        int reached = 0;
        for (int m = 0; m < dehonk::kNumModules; ++m)
            if (s.params.moduleOn[m])
            {
                perMod += fmt ("%s%s %d (held %d, all %d)", perMod.empty() ? "" : ", ", moduleName (m), maxAct[m], maxHeld[m], maxRaw[m]);
                reached = std::max (reached, maxAct[m]);
            }
        // A notch over the number is let go over a few milliseconds, not instantly (that would
        // click), so a handful of frames are allowed: at most one frame in a thousand.
        const std::size_t allowed = frames / 1000;
        t.check (over <= allowed, fmt ("%s: frames with more than %d notches in one module (deeper than 1 dB and within 1 dB of their own maximum over the previous 100 ms; bands closer than %.2f octave count as one notch being handed over): %zu of %zu (%.2f%%, limit 0.1%%)%s%s",
                                 label.c_str(), perModule, spacingOct, over, frames, 100.0 * (double) over / (double) std::max<std::size_t> (1, frames),
                                 firstOver.empty() ? "" : "; ", firstOver.c_str()));
        t.check (deepFrames <= allowed, fmt ("%s: two notches closer than %.2f octave add up to %.2f dB at most (%s at %.3f s; limit Max Cut %.0f dB + 0.25); %zu frames over (allowed: one in a thousand)",
                                             label.c_str(), spacingOct, worstPair, worstPairDesc.c_str(), worstPairAt, capDb, deepFrames));
        t.info (fmt ("%s: closest two notches %.3f octave apart at %.3f s; %zu of %zu frames (%.1f%%) have two closer than %.2f octave, a peak being handed from one band to the next; notches then:%s",
                     label.c_str(), closest > 9.0 ? 0.0 : closest, closestAt, closeFrames, frames, 100.0 * (double) closeFrames / (double) std::max<std::size_t> (1, frames),
                     spacingOct, closestDesc.c_str()));
        t.check (reached >= perModule, fmt ("%s: most notches in one module at once %d (need %d, or the limit was never tested)", label.c_str(),
                                            reached, perModule));
        t.info (fmt ("%s: most notches at once per module (held = wanted cut above zero, told apart by the release rate; all = every band deeper than 1 dB): %s",
                     label.c_str(), perMod.c_str()));
        t.info (fmt ("%s: frames over %d counting held notches only: %zu of %zu, closest two held %.3f octave; counting every band deeper than 1 dB: %zu of %zu, closest %.3f octave",
                     label.c_str(), perModule, overHeld, frames, closestHeld > 9.0 ? 0.0 : closestHeld, overRaw, frames,
                     closestAny > 9.0 ? 0.0 : closestAny));
        for (const auto& mo : moments)
            t.info (fmt ("%s: offending moment %.3f-%.3f s, %d frame%s (%s); notches in its first frame:%s", label.c_str(), mo.startSec, mo.endSec,
                         mo.frames, mo.frames == 1 ? "" : "s", mo.kinds.c_str(), mo.notchesAtStart.c_str()));
        t.info (fmt ("%s: free release measured at %.5f per tick, a time constant of %.0f ms", label.c_str(), h.releaseRatio,
                     1000.0 * h.framePeriodSec / std::max (1e-9, 1.0 - h.releaseRatio)));
        // However the notches are counted above, each band that is cut at all is a filter that
        // runs: bound how many there can be at once, so that a change to how a notch lets go
        // cannot grow the number (and the cost) unseen.
        int modulesOn = 0;
        for (bool on : s.params.moduleOn)
            modulesOn += on ? 1 : 0;
        int mostFilters = 0;
        double mostFiltersAt = 0.0;
        for (const auto& f : r.frames)
        {
            if (f.timeSec < 1.0)
                continue;
            int open = 0;
            for (int k = 0; k < r.numBands; ++k)
                open += f.cutDb[(std::size_t) k] >= 0.1f ? 1 : 0;
            if (open > mostFilters)
            {
                mostFilters = open;
                mostFiltersAt = f.timeSec;
            }
        }
        const int filterLimit = perModule * modulesOn + 2;
        t.check (mostFilters <= filterLimit, fmt ("%s: most bands cut by 0.1 dB or more at once (each is a filter running): %d at %.3f s (limit %d: %d notches in each of %d modules, and 2 letting go)",
                                                  label.c_str(), mostFilters, mostFiltersAt, filterLimit, perModule, modulesOn));
        head += fmt ("%s%s: %zu of %zu frames over %d, two within the spacing add to %.2f dB at most (cap %.0f), %d filters at once at most (limit %d)", head.empty() ? "" : "; ", label.c_str(), over,
                     frames, perModule, worstPair, capDb, mostFilters, filterLimit);
    }
    t.headline = head;
}

//==============================================================================
// modules_independent

void testModulesIndependent (Ctx& t)
{
    const double seconds = 10.0, settle = 1.5;
    const dehonk::Params defaults = alwaysOn();
    const auto edges = clampedEdges (defaults);

    struct Sig
    {
        const char* name;
        std::vector<double> peaks;
    };
    // Peaks in every module: clear of the edges, and close to them (125 Hz is in the bottom of
    // Tub, 440 Hz just above the top of Fog, in Bark's range).
    const Sig sigs[] = { { "peaks at 75, 151, 247 and 700 Hz", { 75.0, 151.0, 247.0, 700.0 } },
                         { "peaks at 75, 125, 247, 440 and 700 Hz", { 75.0, 125.0, 247.0, 440.0, 700.0 } } };
    double worstDiff = 0.0, worstOff = 0.0;
    std::uint64_t seed = 151;
    for (const auto& sig : sigs)
    {
        const Audio in = peaksSignal (kSr, seconds, 1, sig.peaks, -12.0, seed++);
        RenderSpec s;
        s.collectFrames = true;
        s.params = defaults;   // Tub and Fog
        const auto a = render (in, s);
        for (auto& on : s.params.moduleOn)
            on = true;
        const auto b = render (in, s);
        dehonk::Params outer = defaults;
        outer.moduleOn[dehonk::Tub] = outer.moduleOn[dehonk::Fog] = false;
        outer.moduleOn[dehonk::Woof] = outer.moduleOn[dehonk::Bark] = true;
        s.params = outer;
        const auto c = render (in, s);

        // 1. Inside Tub and Fog the cut is the same frame by frame with Woof and Bark on or off.
        double diff = 0.0, diffHz = 0.0, diffAt = 0.0, modDiff = 0.0;
        const std::size_t nf = std::min (a.frames.size(), b.frames.size());
        for (std::size_t f = 0; f < nf; ++f)
        {
            if (a.frames[f].timeSec < settle)
                continue;
            for (int k = 0; k < a.numBands; ++k)
            {
                const double hz = a.bandHz[(std::size_t) k];
                if (hz < edges[dehonk::Tub] || hz >= edges[dehonk::Fog + 1])
                    continue;
                const double d = (double) b.frames[f].cutDb[(std::size_t) k] - a.frames[f].cutDb[(std::size_t) k];
                if (std::abs (d) > std::abs (diff))
                {
                    diff = d;
                    diffHz = hz;
                    diffAt = a.frames[f].timeSec;
                }
            }
            for (int m : { (int) dehonk::Tub, (int) dehonk::Fog })
                modDiff = std::max (modDiff, (double) std::abs (b.frames[f].moduleCutDb[(std::size_t) m] - a.frames[f].moduleCutDb[(std::size_t) m]));
        }
        worstDiff = std::max ({ worstDiff, std::abs (diff), modDiff });
        t.check (std::abs (diff) <= 0.5 && modDiff <= 0.5,
                 fmt ("%s: cut inside Tub and Fog (%.0f-%.0f Hz) with all four on minus Tub+Fog only: largest difference %+.2f dB at %.0f Hz, %.2f s; deepest notch of Tub or Fog differs by up to %.2f dB (limit 0.5)",
                      sig.name, (double) edges[dehonk::Tub], (double) edges[dehonk::Fog + 1], diff, diffHz, diffAt, modDiff));

        // 2. A module that is off has no cut inside its range.
        auto offCheck = [&] (const RenderResult& r, const dehonk::Params& p) {
            double worst = 0.0, worstHz = 0.0, worstAt = 0.0;
            for (const auto& f : r.frames)
                for (int k = 0; k < r.numBands; ++k)
                {
                    const int m = moduleAt (r.bandHz[(std::size_t) k], p);
                    if (m >= 0 && ! p.moduleOn[m] && f.cutDb[(std::size_t) k] > worst)
                    {
                        worst = f.cutDb[(std::size_t) k];
                        worstHz = r.bandHz[(std::size_t) k];
                        worstAt = f.timeSec;
                    }
                }
            worstOff = std::max (worstOff, worst);
            std::string where = worst > 0.0 ? fmt (" at %.0f Hz (in %s's range), %.2f s", worstHz, moduleName (moduleAt (worstHz, p)), worstAt) : "";
            t.check (worst <= 0.05, fmt ("%s, %s on: largest cut inside the range of a module that is off %.3f dB%s (limit 0.05)", sig.name,
                                         moduleList (p).c_str(), worst, where.c_str()));
        };
        offCheck (a, defaults);
        offCheck (c, outer);
        t.info (fmt ("%s, Tub+Fog: %s", sig.name, moduleCutSummary (a, settle).c_str()));
        t.info (fmt ("%s, all four: %s", sig.name, moduleCutSummary (b, settle).c_str()));
    }

    // 3. Moving an edge moves the peak to the other module. One peak at 180 Hz: with the Tub/Fog edge
    // at 200 Hz it is Tub's, with the edge at 165 Hz it is Fog's.
    {
        const Audio in = peaksSignal (kSr, seconds, 1, { 180.0 }, -12.0, 159, nullptr, 10.0);
        std::string head;
        for (float edge : { 200.0f, 165.0f })
        {
            RenderSpec s;
            s.params = alwaysOn();
            s.params.edgeHz[dehonk::Fog] = edge;
            s.collectFrames = true;
            const auto r = render (in, s);
            const int expect = edge > 180.0f ? dehonk::Tub : dehonk::Fog, other = edge > 180.0f ? dehonk::Fog : dehonk::Tub;
            std::size_t cutFrames = 0, ownedByExpect = 0, ownedByOther = 0;
            for (const auto& f : r.frames)
            {
                if (f.timeSec < settle)
                    continue;
                double c = 0.0;
                for (int k = 0; k < r.numBands; ++k)
                    if (std::abs (std::log2 (r.bandHz[(std::size_t) k] / 180.0)) <= 1.0 / 12.0 + 1e-6)
                        c = std::max (c, (double) f.cutDb[(std::size_t) k]);
                if (c <= 1.0)
                    continue;
                ++cutFrames;
                ownedByExpect += f.moduleCutDb[(std::size_t) expect] >= c - 0.01 ? 1 : 0;
                ownedByOther += f.moduleCutDb[(std::size_t) other] >= c - 0.01 ? 1 : 0;
            }
            const double pe = cutFrames > 0 ? 100.0 * (double) ownedByExpect / (double) cutFrames : 0.0;
            const double po = cutFrames > 0 ? 100.0 * (double) ownedByOther / (double) cutFrames : 0.0;
            t.check (cutFrames > 0 && pe >= 95.0 && po <= 5.0,
                     fmt ("peak at 180 Hz, Tub/Fog edge at %.0f Hz: notched (over 1 dB) in %zu frames; %s's deepest notch is that notch in %.0f%% of them (need 95), %s's in %.0f%% (limit 5)",
                          (double) edge, cutFrames, moduleName (expect), pe, moduleName (other), po));
            head += fmt ("%s%.0f Hz edge: %s %.0f%%", head.empty() ? "" : ", ", (double) edge, moduleName (expect), pe);
        }
        t.headline = fmt ("Tub/Fog difference %.2f dB (limit 0.5), cut in an off module %.2f dB (limit 0.05); 180 Hz peak owner: %s", worstDiff,
                          worstOff, head.c_str());
    }
}

//==============================================================================
// threshold

void testThreshold (Ctx& t)
{
    // The signal's level as the core compares it with the threshold: RMS below 2.8 kHz, louder channel.
    Audio base = peaksSignal (kSr, 8.0, 2, { 151.0, 247.0 }, -12.0, 161);
    double level = -400.0;
    for (int c = 0; c < 2; ++c)
    {
        const Spectrum sp = powerSpectrum (fromChannels (kSr, { base.ch[(std::size_t) c] }), 0, base.length());
        level = std::max (level, sp.bandDb (0.0, 2800.0));
    }
    const float thr = -20.0f;
    const double settle = 1.0;

    struct Point
    {
        double over, meanCut, meanActivity, maxCut;
    };
    std::vector<Point> pts;
    double worstFull = 0.0;
    for (double over : { -6.0, -3.0, -1.5, 0.0, 1.5, 3.0, 4.5, 6.0, 9.0 })
    {
        Audio in = base;
        const auto g = (float) dbToAmp ((double) thr + over - level);
        for (auto& ch : in.ch)
            for (auto& v : ch)
                v *= g;
        RenderSpec s;
        s.params.thresholdDb = thr;
        s.collectFrames = true;
        const auto r = render (in, s);
        const auto cs = cutStats (r, settle);
        const auto csAll = cutStats (r, 0.0);
        double act = 0.0;
        std::size_t na = 0;
        for (const auto& f : r.frames)
            if (f.timeSec >= settle)
            {
                act += f.activity;
                ++na;
            }
        act /= std::max<double> (1.0, (double) na);
        pts.push_back ({ over, cs.meanTotal, act, csAll.maxTotal });

        if (over <= -3.0)
            t.check (csAll.maxTotal <= 0.1, fmt ("%+.1f dB against the threshold: largest cut in any frame %.3f dB (limit 0.1)", over, csAll.maxTotal));
        if (over >= 6.0)
        {
            RenderSpec ref = s;
            ref.params.thresholdDb = kAlwaysDb;
            const auto rr = render (in, ref);
            const auto cr = cutStats (rr, settle);
            double band = 0.0, bandHz = 0.0;
            for (std::size_t k = 0; k < cr.mean.size(); ++k)
                if (std::abs (cs.mean[k] - cr.mean[k]) > std::abs (band))
                {
                    band = cs.mean[k] - cr.mean[k];
                    bandHz = r.bandHz[k];
                }
            worstFull = std::max ({ worstFull, std::abs (cs.meanTotal - cr.meanTotal), std::abs (band) });
            t.check (std::abs (cs.meanTotal - cr.meanTotal) <= 0.5 && std::abs (band) <= 0.5,
                     fmt ("%+.1f dB against the threshold: deepest notch mean %.2f dB against %.2f dB with threshold -60; largest difference in any band's mean cut %+.2f dB at %.0f Hz (limit 0.5)",
                          over, cs.meanTotal, cr.meanTotal, band, bandHz));
        }
    }
    bool mono = true;
    std::string curve;
    for (std::size_t i = 0; i < pts.size(); ++i)
    {
        curve += fmt ("%s%+.1f dB: %.2f dB (activity %.2f)", i == 0 ? "" : ", ", pts[i].over, pts[i].meanCut, pts[i].meanActivity);
        if (i > 0 && pts[i].meanCut < pts[i - 1].meanCut - 0.1)
            mono = false;
    }
    t.check (mono, "mean deepest notch against level over the threshold rises steadily (each step at least the one before less 0.1 dB): " + curve);
    const auto mid = std::find_if (pts.begin(), pts.end(), [] (const Point& p) { return p.over == 3.0; });
    const auto full = std::find_if (pts.begin(), pts.end(), [] (const Point& p) { return p.over == 6.0; });
    if (mid != pts.end() && full != pts.end())
        t.check (mid->meanCut > 0.1 && mid->meanCut <= full->meanCut + 0.1,
                 fmt ("+3 dB, inside the knee: deepest notch mean %.2f dB, between none and the full %.2f dB", mid->meanCut, full->meanCut));
    t.info (fmt ("signal level as compared with the threshold (RMS below 2.8 kHz, louder channel) %.2f dBFS before scaling; threshold %.0f dBFS", level,
                 (double) thr));
    t.headline = fmt ("below: max cut %.2f dB (limit 0.1); full effect from +6 dB within %.2f dB (limit 0.5); %s", std::max (pts[0].maxCut, pts[1].maxCut),
                      worstFull, mono ? "monotonic" : "NOT monotonic");
}

//==============================================================================
// notch_dwell (informational)

/** How long each notch stays open: from the cut rising above 1 dB until it is under 0.5 dB for
    100 ms (notchEvents, as the render tool logs them). Notches still open at the end are not
    counted. A notch handing a peak to the band next door lets go as the other grows, so "held"
    cannot be read off the rate the cut moves at; open and closed can. */
void dwellReport (Ctx& t, const char* label, const RenderResult& r, double fromSec)
{
    std::vector<double> runs;
    const double end = r.frames.empty() ? 0.0 : r.frames.back().timeSec;
    for (const auto& e : notchEvents (r))
        if (e.openSec >= fromSec && e.closeSec < end - 0.15)
            runs.push_back (e.closeSec - e.openSec);
    if (runs.empty())
    {
        t.info (fmt ("%s: no notch opened and closed", label));
        return;
    }
    t.info (fmt ("%s: %zu notches opened (above 1 dB) and closed (under 0.5 dB for 100 ms); time open: median %.0f ms, quartiles %.0f and %.0f ms, 90th percentile %.0f ms",
                 label, runs.size(), 1000.0 * percentile (runs, 0.5), 1000.0 * percentile (runs, 0.25), 1000.0 * percentile (runs, 0.75),
                 1000.0 * percentile (runs, 0.9)));
    t.headline += fmt ("%s%s median %.0f ms", t.headline.empty() ? "" : "; ", label, 1000.0 * percentile (runs, 0.5));
}

void testNotchDwell (Ctx& t)
{
    // 1. The effect_on_peaks signal: two steady peaks.
    {
        const Audio in = peaksSignal (kSr, 10.0, 1, { 151.0, 247.0 }, -12.0, 171);
        RenderSpec s;
        s.params = alwaysOn();
        s.blockSize = 8;
        s.collectFrames = true;
        const auto r = render (in, s);
        const auto h = heldNotches (r, 1.0);
        dwellReport (t, "steady peaks at 151 and 247 Hz", r, 1.0);

        // For each peak: which band within 1/12 octave has its deepest notch (over 1 dB), for what
        // share of the time, and how often that changes band.
        for (double hz : { 151.0, 247.0 })
        {
            std::vector<int> near;
            for (int k = 0; k < r.numBands; ++k)
                if (std::abs (std::log2 (r.bandHz[(std::size_t) k] / hz)) <= 1.0 / 12.0 + 1e-6)
                    near.push_back (k);
            std::vector<double> share (near.size(), 0.0);
            int last = -1, changes = 0;
            std::size_t frames = 0, none = 0;
            for (std::size_t f = 0; f < r.frames.size(); ++f)
            {
                if (r.frames[f].timeSec < 1.0)
                    continue;
                ++frames;
                int which = -1;
                float deepest = 1.0f;
                for (std::size_t i = 0; i < near.size(); ++i)
                    if (r.frames[f].cutDb[(std::size_t) near[i]] > deepest)
                    {
                        which = (int) i;
                        deepest = r.frames[f].cutDb[(std::size_t) near[i]];
                    }
                if (which >= 0)
                    share[(std::size_t) which] += 1.0;
                none += which < 0 ? 1 : 0;
                if (which >= 0 && last >= 0 && which != last)
                    ++changes;
                if (which >= 0)
                    last = which;
            }
            std::string bands;
            for (std::size_t i = 0; i < near.size(); ++i)
                bands += fmt (" %.0f Hz %.0f%%", (double) r.bandHz[(std::size_t) near[i]], 100.0 * share[i] / std::max<double> (1.0, (double) frames));
            const double secs = (double) frames * h.framePeriodSec;
            t.info (fmt ("peak %.0f Hz: deepest notch on%s, none over 1 dB %.0f%% of the time; it changed band %d times in %.1f s (%.1f a second)", hz, bands.c_str(),
                         100.0 * (double) none / std::max<double> (1.0, (double) frames), changes, secs, (double) changes / std::max (1e-9, secs)));
        }
    }
    // 2. Two wavering tones 10 dB above the bed, one in Tub and one in Fog, each jumping to a new
    // frequency every 2 s.
    {
        const double seg = 2.0;
        const int segs = 6;
        const std::size_t n = S (seg * segs), fade = S (0.01);
        Rng rng (172);
        auto bed = shapedNoise (kSr, n, 173, bassHeavyBedDb);
        normaliseRms (bed, -20.0);
        const Spectrum sb = powerSpectrum (fromChannels (kSr, { bed }), 0, n);
        std::vector<double> acc (bed.begin(), bed.end());
        std::vector<std::vector<double>> where (2);
        for (int v = 0; v < 2; ++v)
            for (int j = 0; j < segs; ++j)
            {
                const double hz = v == 0 ? rng.uniform (125.0, 185.0) : rng.uniform (215.0, 370.0);
                where[(std::size_t) v].push_back (hz);
                const std::size_t a = S (seg * j), len = S (seg);
                std::vector<float> env (len);
                for (std::size_t i = 0; i < len; ++i)
                    env[i] = (float) (sineAmpFor (bandLevelDb (sb, hz) + 10.0) * std::min ({ 1.0, (double) i / (double) fade, (double) (len - i) / (double) fade }));
                const auto tone = waveringTone (kSr, len, hz, 0.003, env, 174 + (std::uint64_t) (v * 100 + j));
                for (std::size_t i = 0; i < len && a + i < n; ++i)
                    acc[a + i] += tone[i];
            }
        std::vector<float> x (n);
        for (std::size_t i = 0; i < n; ++i)
            x[i] = (float) acc[i];
        normaliseRms (x, -12.0);
        RenderSpec s;
        s.params = alwaysOn();
        s.blockSize = 8;
        s.collectFrames = true;
        const auto r = render (fromChannels (kSr, { x }), s);
        dwellReport (t, "tones that move every 2 s", r, 1.0);

        // How long after each move a notch deeper than 1 dB sits within 1/12 octave of the tone's
        // new place, and how long the notch at the old place takes to fall below 1 dB.
        std::vector<double> follow, letGo;
        int never = 0;
        for (int v = 0; v < 2; ++v)
            for (int j = 1; j < segs; ++j)
            {
                const double t0 = seg * j, hz = where[(std::size_t) v][(std::size_t) j], old = where[(std::size_t) v][(std::size_t) j - 1];
                double got = -1.0, gone = -1.0;
                for (std::size_t f = 0; f < r.frames.size(); ++f)
                {
                    const double tf = r.frames[f].timeSec;
                    if (tf < t0 || tf >= t0 + seg)
                        continue;
                    bool near = false, oldOn = false;
                    for (int k = 0; k < r.numBands; ++k)
                    {
                        const double bk = r.bandHz[(std::size_t) k];
                        near = near || (r.frames[f].cutDb[(std::size_t) k] > 1.0f && std::abs (std::log2 (bk / hz)) <= 1.0 / 12.0 + 1e-6);
                        oldOn = oldOn || (r.frames[f].cutDb[(std::size_t) k] > 1.0f && std::abs (std::log2 (bk / old)) <= 1.0 / 12.0 + 1e-6
                                          && std::abs (std::log2 (bk / hz)) > 1.0 / 12.0 + 1e-6);
                    }
                    if (near && got < 0.0)
                        got = tf - t0;
                    if (! oldOn && gone < 0.0)
                        gone = tf - t0;
                }
                if (got >= 0.0)
                    follow.push_back (got);
                else
                    ++never;
                if (gone >= 0.0)
                    letGo.push_back (gone);
            }
        if (! follow.empty())
            t.info (fmt ("after a tone moves, a notch deeper than 1 dB is within 1/12 octave of its new place after: median %.0f ms, quartiles %.0f and %.0f ms, longest %.0f ms; %d of %d moves never followed within 2 s",
                         1000.0 * percentile (follow, 0.5), 1000.0 * percentile (follow, 0.25), 1000.0 * percentile (follow, 0.75),
                         1000.0 * *std::max_element (follow.begin(), follow.end()), never, (int) follow.size() + never));
        else
            t.info (fmt ("no move was followed by a notch within 2 s (%d moves)", never));
        if (! letGo.empty())
            t.info (fmt ("the notch at the old place falls below 1 dB after: median %.0f ms, longest %.0f ms", 1000.0 * percentile (letGo, 0.5),
                         1000.0 * *std::max_element (letGo.begin(), letGo.end())));
        if (! follow.empty())
            t.headline += fmt ("; follows a move in %.0f ms (median)", 1000.0 * percentile (follow, 0.5));
    }
    t.info ("informational: this test never fails");
}

//==============================================================================
// Snap module (SPEC 12.8). In these tests the four notch modules are off unless stated, so only
// Snap acts.

void testClickReducesClicks (Ctx& t)
{
    std::string head;
    for (const auto& cc : kClickCases)
    {
        const auto sig = clickSignal (kSr, cc.hz, cc.durMs);
        BurstChange at[3];
        const float depths[3] = { 0.0f, dehonk::Params {}.depth, 1.0f };
        for (int d = 0; d < 3; ++d)
        {
            RenderSpec s;
            s.params = clickOnly (depths[d]);
            at[d] = burstChange (sig, render (sig.in, s).out);
        }
        t.check (-at[1].bandMean >= cc.minDb,
                 fmt ("%s bursts, %d of them, default Depth: band peak within 8 ms falls %.2f dB on average (each %.2f to %.2f; need at least %.1f)",
                      cc.name, at[1].bursts, -at[1].bandMean, -at[1].bandMax, -at[1].bandMin, cc.minDb));
        t.check (-at[2].bandMean > -at[1].bandMean,
                 fmt ("%s: Depth 100%% takes %.2f dB, more than %.2f dB at 50%%", cc.name, -at[2].bandMean, -at[1].bandMean));
        t.check (std::abs (at[0].bandMean) < 0.1 && std::abs (at[0].bandMin) < 0.1,
                 fmt ("%s: Depth 0 changes the band peak by %+.3f dB on average, %+.3f dB at most (limit 0.1)", cc.name, at[0].bandMean,
                      std::abs (at[0].bandMin) > std::abs (at[0].bandMax) ? at[0].bandMin : at[0].bandMax));
        // The bed is judged in Snap's band: the band holds about a tenth of the pink bed's power,
        // so a whole-signal figure would let a standing cut of several dB in the band through.
        for (int d = 1; d < 3; ++d)
        {
            t.check (std::abs (at[d].bedBandDb) < 0.1, fmt ("%s, Depth %.0f%%: bed from 250 to 450 ms after each burst, 1.8 to 4 kHz band, changed %+.3f dB (limit 0.1)",
                                                            cc.name, 100.0 * depths[d], at[d].bedBandDb));
            t.info (fmt ("%s, Depth %.0f%%: the same bed over the whole signal changed %+.3f dB", cc.name, 100.0 * depths[d], at[d].bedDb));
        }
        t.info (fmt ("%s: whole-signal peak near the bursts %+.2f dB at default Depth, %+.2f dB at 100%%", cc.name, at[1].fullMean, at[2].fullMean));
        head += fmt ("%s%s %.1f dB (100%%: %.1f)", head.empty() ? "" : ", ", cc.name, -at[1].bandMean, -at[2].bandMean);
    }
    t.headline = "band peak falls: " + head;
}

void testClickLeavesHitsAlone (Ctx& t)
{
    struct Hit
    {
        const char* name;
        BurstKind kind;
        double durMs, peak;
    };
    const Hit hits[] = {
        { "broadband bursts (white noise, 1.5 ms decay, 6 ms)", BurstKind::broadband, 6.0, 0.5 },
        { "snare-like bursts (white noise, 30 ms decay, 150 ms)", BurstKind::snare, 150.0, 0.5 },
        { "kick-like bursts (60 Hz with a pitch drop, 1 ms tick)", BurstKind::kick, 300.0, 0.7 },
    };
    double worstBand = 0.0, worstFull = 0.0;
    for (const auto& h : hits)
    {
        const auto sig = burstSignal (kSr, 10.0, 1, h.kind, 0.0, h.durMs, h.peak, -22.0, 411);
        RenderSpec s;
        s.params = clickOnly();
        s.collectFrames = true;
        s.frameEveryTick = true;
        const auto r = render (sig.in, s);
        const auto c = burstChange (sig, r.out);
        const auto cf = clickFrames (r, 3.0, 0.4);
        worstBand = std::max (worstBand, std::abs (c.bandMean));
        worstFull = std::max (worstFull, std::abs (c.fullMean));
        t.check (std::abs (c.bandMean) < 1.0, fmt ("%s, %d of them: band peak changes %+.2f dB on average (each %+.2f to %+.2f; limit 1)", h.name,
                                                   c.bursts, c.bandMean, c.bandMin, c.bandMax));
        t.check (std::abs (c.fullMean) < 0.3, fmt ("%s: whole-signal peak changes %+.3f dB on average (each %+.3f to %+.3f; limit 0.3)", h.name,
                                                   c.fullMean, c.fullMin, c.fullMax));
        t.info (fmt ("%s: Snap's cut in the frames: largest %.2f dB, %d events above 1 dB", h.name, cf.max, cf.events));
    }
    t.headline = fmt ("worst band peak change %.2f dB (limit 1), worst whole-signal peak change %.3f dB (limit 0.3)", worstBand, worstFull);
}

void testClickLeavesSteadyAlone (Ctx& t)
{
    std::string head;
    // A sustained tone that starts abruptly over the bed.
    {
        const double start = 1.0, seconds = 6.0;
        Audio in = pinkAudio (kSr, S (seconds), 1, -22.0, 421);
        const auto tone = sine (kSr, S (seconds - start), 2500.0, dbToAmp (-14.0));
        for (std::size_t i = 0; i < tone.size(); ++i)
            in.ch[0][S (start) + i] += tone[i];
        RenderSpec s;
        s.params = clickOnly();
        s.collectFrames = true;
        s.frameEveryTick = true;
        const auto r = render (in, s);
        const std::size_t a = S (start + 2.0), b = in.length();
        const double ai = fitSineAmplitude (in.ch[0].data() + a, b - a, 2500.0, kSr);
        const double ao = fitSineAmplitude (r.out.ch[0].data() + a, b - a, 2500.0, kSr);
        const double dLong = ampToDb (ao / ai);
        t.check (std::abs (dLong) < 0.1, fmt ("2.5 kHz tone at -14 dBFS starting abruptly at %.0f s: its level from 2 s after the start changed %+.3f dB (limit 0.1)",
                                              start, dLong));
        // Informational: what happens to the onset.
        double worst = 0.0, worstAt = 0.0;
        const std::size_t w = S (0.005);
        for (std::size_t p = S (start); p + w <= S (start + 0.05); p += w)
        {
            const double d = ampToDb (fitSineAmplitude (r.out.ch[0].data() + p, w, 2500.0, kSr) / fitSineAmplitude (in.ch[0].data() + p, w, 2500.0, kSr));
            if (d < worst)
            {
                worst = d;
                worstAt = (double) (p - S (start)) / kSr;
            }
        }
        const double first50 = ampToDb (fitSineAmplitude (r.out.ch[0].data() + S (start), S (0.05), 2500.0, kSr)
                                        / fitSineAmplitude (in.ch[0].data() + S (start), S (0.05), 2500.0, kSr));
        const auto cf = clickFrames (r, 1.0, start - 0.01);
        t.info (fmt ("onset (informational): the tone's level over its first 50 ms changed %+.2f dB; the 5 ms window changed most was %+.2f dB at %.0f ms; Snap's largest cut %.2f dB",
                     first50, worst, worstAt * 1000.0, cf.max));
        head += fmt ("tone %+.3f dB", dLong);
    }
    // Pink noise alone.
    for (double level : { -18.0, -3.0 })
    {
        const Audio in = pinkAudio (kSr, S (20.0), 1, level, 431);
        RenderSpec s;
        s.params = clickOnly();
        s.collectFrames = true;
        s.frameEveryTick = true;
        const auto r = render (in, s);
        const std::size_t a = S (0.5), b = in.length();
        const double band = clickBandChangeDb (in, r.out, a, b);
        const double full = powerToDb (meanSquare (r.out, a, b) / meanSquare (in, a, b));
        const auto cf = clickFrames (r, 6.0, 0.5);
        t.check (std::abs (full) < 0.05, fmt ("pink noise at %.0f dBFS RMS: level changed %+.4f dB (limit 0.05)", level, full));
        t.info (fmt ("pink noise at %.0f dBFS RMS: level in the 1.8 to 4 kHz band alone changed %+.3f dB; Snap's mean cut over all frames %.3f dB",
                     level, band, cf.mean));
        t.check (cf.pctOver <= 0.5, fmt ("pink noise at %.0f dBFS RMS: frames with Snap cutting more than 6 dB %.3f%% (limit 0.5%%); largest %.2f dB, %d events above 1 dB in 19.5 s",
                                         level, cf.pctOver, cf.max, cf.events));
        head += fmt (", pink %.0f dBFS %+.3f dB (band %+.2f dB, %d events)", level, full, band, cf.events);
    }
    // A tone with slow tremolo.
    {
        const double seconds = 8.0;
        Audio in = pinkAudio (kSr, S (seconds), 1, -22.0, 441);
        const auto tone = sine (kSr, S (seconds), 2500.0, dbToAmp (-14.0));
        for (std::size_t i = 0; i < tone.size(); ++i)
        {
            const double tdb = -3.0 - 3.0 * std::cos (2.0 * std::numbers::pi * 4.0 * (double) i / kSr);   // 0 to -6 dB at 4 Hz
            in.ch[0][i] += (float) (tone[i] * dbToAmp (tdb));
        }
        RenderSpec s;
        s.params = clickOnly();
        const auto r = render (in, s);
        const double band = clickBandChangeDb (in, r.out, S (1.0), in.length());
        t.check (std::abs (band) < 0.1, fmt ("2.5 kHz tone at -14 dBFS with a 4 Hz tremolo 6 dB deep: level in the band changed %+.4f dB (limit 0.1)", band));
        head += fmt (", tremolo %+.3f dB", band);
    }
    t.headline = head;
}

void testClickLevelIndependent (Ctx& t)
{
    const auto loud = clickSignal (kSr, 2500.0, 3.0);
    auto quiet = loud;
    for (auto& ch : quiet.in.ch)
        applyGain (ch, dbToAmp (-20.0));
    RenderSpec s;
    s.params = clickOnly();   // default threshold: Snap is not tied to it
    const auto cl = burstChange (loud, render (loud.in, s).out);
    const auto cq = burstChange (quiet, render (quiet.in, s).out);
    const double d = cq.bandMean - cl.bandMean;
    t.check (std::abs (d) < 1.0, fmt ("2.5 kHz clicks at the default threshold (%.0f dBFS): band peak falls %.2f dB as made, %.2f dB 20 dB quieter (difference %+.2f dB, limit 1)",
                                      (double) s.params.thresholdDb, -cl.bandMean, -cq.bandMean, d));
    t.check (-cl.bandMean >= 1.0, fmt ("the louder signal is reduced by %.2f dB (need at least 1, or the comparison is empty)", -cl.bandMean));
    t.headline = fmt ("%.2f dB as made, %.2f dB at -20 dB (difference %+.2f dB, limit 1)", -cl.bandMean, -cq.bandMean, d);
}

void testClickIsBandLimited (Ctx& t)
{
    const auto sig = clickSignal (kSr, 2500.0, 3.0, 1, 20.0, 451);
    std::string head;
    for (float depth : { dehonk::Params {}.depth, 1.0f })
    {
        RenderSpec s;
        s.params = clickOnly (depth);
        const auto r = render (sig.in, s);
        Audio diff = r.out;
        for (std::size_t i = 0; i < diff.length(); ++i)
            diff.ch[0][i] -= sig.in.ch[0][i];
        const Spectrum sp = powerSpectrum (diff, S (0.5), diff.length());
        // Density (power per Hz) in third-octave bands, so bands of different widths compare.
        auto density = [&] (double centre) {
            const double lo = centre * std::exp2 (-1.0 / 6.0), hi = centre * std::exp2 (1.0 / 6.0);
            return powerToDb (sp.bandPower (lo, hi) / (hi - lo));
        };
        const double ref = density (2500.0);
        double worstLow = -400.0, worstLowHz = 0.0, worstHigh = -400.0, worstHighHz = 0.0;
        for (double c = 20.0; c * std::exp2 (1.0 / 6.0) <= 600.0; c *= std::exp2 (1.0 / 12.0))
            if (density (c) - ref > worstLow)
            {
                worstLow = density (c) - ref;
                worstLowHz = c;
            }
        for (double c = 10000.0 * std::exp2 (1.0 / 6.0); c * std::exp2 (1.0 / 6.0) <= 0.45 * kSr; c *= std::exp2 (1.0 / 12.0))
            if (density (c) - ref > worstHigh)
            {
                worstHigh = density (c) - ref;
                worstHighHz = c;
            }
        const double removed = powerToDb (meanSquare (diff, S (0.5), diff.length()) / meanSquare (sig.in, S (0.5), diff.length()));
        t.check (removed > -60.0, fmt ("Depth %.0f%%: what Snap takes out is %s dB relative to the input (need above -60, or there is nothing to judge)",
                                       100.0 * depth, dbStr (removed).c_str()));
        t.check (worstLow <= -20.0, fmt ("Depth %.0f%%: output minus input, highest third-octave below 600 Hz %s dB at %.0f Hz relative to 2.5 kHz (limit -20, per Hz)",
                                         100.0 * depth, dbStr (worstLow).c_str(), worstLowHz));
        t.check (worstHigh <= -20.0, fmt ("Depth %.0f%%: highest third-octave above 10 kHz %s dB at %.0f Hz relative to 2.5 kHz (limit -20, per Hz)",
                                          100.0 * depth, dbStr (worstHigh).c_str(), worstHighHz));
        head += fmt ("%sDepth %.0f%%: below 600 Hz %s dB, above 10 kHz %s dB", head.empty() ? "" : "; ", 100.0 * depth, dbStr (worstLow, 1).c_str(),
                     dbStr (worstHigh, 1).c_str());
    }
    t.headline = head + " (limit -20)";
}

void testClickNoLatencyAndIdentities (Ctx& t)
{
    int bad = 0;
    // An impulse with Snap on comes out at latencySamples(), which is what it is with Snap off.
    for (float la : { 0.0f, 5.0f, 10.0f, 20.0f })
    {
        const std::size_t at = 4800, n = at + S (0.1) + 4096;
        Audio in = pinkAudio (kSr, n, 1, -60.0, 461);
        in.ch[0][at] += 1.0f;
        RenderSpec s;
        s.params = clickOnly (1.0f);
        s.params.lookaheadMs = la;
        s.compensateLatency = false;
        dehonk::Core core;
        const auto r = render (core, in, s);
        std::size_t peakAt = 0;
        double peak = 0.0;
        for (std::size_t i = 0; i < n; ++i)
            if (std::abs (r.out.ch[0][i]) > std::abs (peak))
            {
                peak = r.out.ch[0][i];
                peakAt = i;
            }
        dehonk::Params off = s.params;
        off.snapOn = false;
        core.setParams (off);
        const int latOff = core.latencySamples();
        const long measured = (long) peakAt - (long) at;
        const bool ok = measured == r.latency && r.latency == latOff;
        bad += ok ? 0 : 1;
        t.check (ok, fmt ("impulse, Snap on, lookahead %2.0f ms: comes out at +%ld, latencySamples() %d with Snap on and %d with it off; height %.4f",
                          (double) la, measured, r.latency, latOff, peak));
    }

    // Parameters change at block starts, so Snap is switched off on a block boundary, and the
    // burst that would start at 3 s is moved to 1.5 ms before it so the switch falls mid-click.
    static constexpr std::int64_t offAt = 512 * 281;   // 2.997 s
    const double clickBeforeOff = (double) offAt / kSr - 0.0015;
    const auto sig = burstSignal (kSr, 6.0, 2, BurstKind::tone, 2500.0, 3.0, 0.5, -22.0, 462, 0.5, 0.5,
                                  { 0.5, 1.0, 1.5, 2.0, 2.5, clickBeforeOff, 3.5, 4.0, 4.5, 5.0, 5.5 });
    const std::size_t n = sig.in.length();
    // Snap off after being on is bit-identical to a core where it was never on, with the notch
    // modules off and with Tub and Fog on, once 100 ms have passed: the first moments after the
    // switch are left to automation_clicks, so a cut that releases instead of stopping dead passes.
    const std::size_t settled = (std::size_t) offAt + S (0.1);
    for (bool modules : { false, true })
    {
        RenderSpec s;
        s.blockSize = 512;
        s.params = clickOnly (1.0f);
        s.params.thresholdDb = kAlwaysDb;
        if (modules)
            s.params.moduleOn[dehonk::Tub] = s.params.moduleOn[dehonk::Fog] = true;
        s.automate = [] (std::int64_t pos, dehonk::Params& p) { p.snapOn = pos < offAt; };
        const auto toggled = render (sig.in, s);
        RenderSpec never = s;
        never.automate = nullptr;
        never.params.snapOn = false;
        const auto ref = render (sig.in, never);
        const double before = residualDb (toggled.out, ref.out, 1.0, S (0.5), (std::size_t) offAt);
        const double after = maxAbsDiff (toggled.out, ref.out, 1.0, settled, n);
        const double during = maxAbsDiff (toggled.out, ref.out, 1.0, (std::size_t) offAt, settled);
        const bool ok = after == 0.0 && before > -60.0;
        bad += ok ? 0 : 1;
        t.check (ok, fmt ("%s: Snap on until %.4f s (%.1f ms into a 3 ms click) then off, against never on: from 100 ms later max |diff| %.3g (need 0, bit-identical); before the switch they differ by %s dB (need above -60)",
                          modules ? "Tub+Fog on" : "notch modules off", (double) offAt / kSr, 1000.0 * ((double) offAt / kSr - clickBeforeOff), after,
                          dbStr (before).c_str()));
        t.info (fmt ("%s: in the 100 ms after the switch, max |diff| %.3g", modules ? "Tub+Fog on" : "notch modules off", during));
    }
    // Normal + Solo Honk with Snap on.
    struct Case
    {
        float mix, trim, la;
    };
    for (const Case c : { Case { 1.0f, -3.0f, 0.0f }, Case { 0.6f, 2.0f, 5.0f } })
    {
        RenderSpec s;
        s.params = clickOnly (1.0f);
        s.params.thresholdDb = kAlwaysDb;
        s.params.moduleOn[dehonk::Tub] = s.params.moduleOn[dehonk::Fog] = true;
        s.params.mix = c.mix;
        s.params.trimDb = c.trim;
        s.params.lookaheadMs = c.la;
        const auto normal = render (sig.in, s);
        s.params.soloHonk = true;
        const auto solo = render (sig.in, s);
        Audio sum = normal.out;
        for (int ch = 0; ch < sum.numChannels(); ++ch)
            for (std::size_t i = 0; i < n; ++i)
                sum.ch[(std::size_t) ch][i] += solo.out.ch[(std::size_t) ch][i];
        const double res = residualDb (sum, sig.in, dbToAmp (c.trim), S (0.5), n);
        bad += res < -80.0 ? 0 : 1;
        t.check (res < -80.0, fmt ("Snap and Tub+Fog on, mix %.1f trim %+.0f dB lookahead %.0f ms: normal + solo vs trim x input: residual %s dB (limit -80)",
                                   (double) c.mix, (double) c.trim, (double) c.la, dbStr (res).c_str()));
    }
    // Diff and Bypass with Snap on.
    for (int mode = 0; mode < 2; ++mode)
        for (float la : { 0.0f, 10.0f })
        {
            RenderSpec s;
            s.params = clickOnly (1.0f);
            s.params.lookaheadMs = la;
            const std::int64_t at = (std::int64_t) S (1.5);
            s.automate = [mode, at] (std::int64_t pos, dehonk::Params& p) { (mode == 0 ? p.diff : p.bypass) = pos >= at; };
            s.collectFrames = true;
            const auto r = render (sig.in, s);
            const double before = residualDb (r.out, sig.in, 1.0, S (0.5), S (1.5));
            if (mode == 1)
            {
                const double res = residualDb (r.out, sig.in, 1.0, S (1.6), n);
                bad += res < -120.0 ? 0 : 1;
                t.check (res < -120.0, fmt ("Snap on, Bypass switched on at 1.5 s, lookahead %2.0f ms: output vs input from 1.6 s %s dB (limit -120); before switching %s dB",
                                            (double) la, dbStr (res).c_str(), dbStr (before).c_str()));
                continue;
            }
            // Diff: the delayed input times the gain the core reports (SPEC 13.6). Snap alone takes
            // out very little, so the offset here is small; the level match itself is judged in
            // solo_honk_identity. The core updates its running levels every millisecond, so each
            // click moves the offset in a small step, which a gain-and-slope fit over 10 ms cannot
            // follow exactly: here the shape limit is -60 dB (a gain wobble of about 0.01 dB), not
            // the -80 dB used on the steady signal in solo_honk_identity, and the size of the
            // steps is reported.
            const DiffMatch d = diffMatch (r, sig.in, S (1.6), n);
            double step = 0.0, stepAt = 0.0;
            for (std::size_t f = 1; f < r.frames.size(); ++f)
                if (r.frames[f].timeSec > 1.6 && std::abs (r.frames[f].diffOffsetDb - r.frames[f - 1].diffOffsetDb) > step)
                {
                    step = std::abs (r.frames[f].diffOffsetDb - r.frames[f - 1].diffOffsetDb);
                    stepAt = r.frames[f].timeSec;
                }
            const bool ok = d.worstResidDb < -60.0 && d.worstGainErrDb <= 0.05 && d.negativeGainWindows == 0 && d.offsetOutOfRangeDb == 0.0;
            bad += ok ? 0 : 1;
            t.check (d.negativeGainWindows == 0 && d.offsetOutOfRangeDb == 0.0,
                     fmt ("Snap on, Diff lookahead %2.0f ms: fitted gain positive in every 10 ms window (%d of %d negative); diffOffsetDb within -9..0 dB (worst %.3f dB outside)",
                          (double) la, d.negativeGainWindows, d.windows, d.offsetOutOfRangeDb));
            t.check (ok, fmt ("Snap on, Diff switched on at 1.5 s, lookahead %2.0f ms: from 1.6 s the output is the input times a gain fitted per 10 ms window to %s dB at %.2f s (limit -60), that gain within %.3f dB of diffOffsetDb (%.3f to %.3f dB) (limit 0.05); before switching %s dB",
                              (double) la, dbStr (d.worstResidDb).c_str(), d.worstResidAt, d.worstGainErrDb, d.minOffsetDb, d.maxOffsetDb,
                              dbStr (before).c_str()));
            t.info (fmt ("Snap on, Diff lookahead %2.0f ms: largest change of diffOffsetDb from one frame (%d samples) to the next %.4f dB at %.2f s",
                         (double) la, s.blockSize, step, stepAt));
        }
    t.headline = fmt ("%d checks wrong", bad);
}

//==============================================================================
// SPEC section 13: tests from the mix review notes.

/** Mean, over frames from fromSec, of the cut at band k. */
double meanBandCut (const RenderResult& r, int k, double fromSec)
{
    double s = 0.0;
    std::size_t n = 0;
    for (const auto& f : r.frames)
        if (f.timeSec >= fromSec)
        {
            s += f.cutDb[(std::size_t) k];
            ++n;
        }
    return n > 0 ? s / (double) n : 0.0;
}

/** The two bands either side of hz (or the one it sits on and its nearer neighbour). */
std::pair<int, int> bandsAround (const std::vector<float>& bandHz, double hz)
{
    const int k = nearestBand (bandHz, hz);
    const int other = hz >= bandHz[(std::size_t) k] ? std::min (k + 1, (int) bandHz.size() - 1) : std::max (k - 1, 0);
    return { std::min (k, other), std::max (k, other) };
}

std::string noteLabel (double hz)
{
    const Note n = noteOf (hz);
    return fmt ("%s%+.0fc", n.name().c_str(), n.cents);
}

//==============================================================================
// noise_is_left_alone (SPEC 13.3)

/** Dense noise from 80 to 600 Hz, equal power per octave (pink slope), skirts of 48 dB per
    octave either side; stereo, independent channels. extraDb(hz) is added to its shape. */
Audio bandNoise (double seconds, std::uint64_t seed, const std::function<double (double)>& extraDb = {})
{
    const std::size_t n = S (seconds);
    Audio a = Audio::make (kSr, 2, n);
    for (int c = 0; c < 2; ++c)
        a.ch[(std::size_t) c] = shapedNoise (kSr, n, seed + 977ull * (std::uint64_t) c, [&] (double hz) {
            if (hz <= 1.0)
                return -400.0;
            const double pink = -10.0 * std::log10 (hz / 1000.0);
            const double hp = -10.0 * std::log10 (1.0 + std::pow (80.0 / hz, 8.0));
            const double lp = -10.0 * std::log10 (1.0 + std::pow (hz / 600.0, 8.0));
            return pink + hp + lp + (extraDb ? extraDb (hz) : 0.0);
        });
    return a;
}

void scaleTo (Audio& a, double rmsDbFs)
{
    // Programme level as the core reads it: RMS below 2.8 kHz in the louder channel. These signals
    // have nothing above 2.8 kHz, so it is the louder channel's RMS.
    double loud = -400.0;
    for (const auto& c : a.ch)
        loud = std::max (loud, rmsDb (c));
    for (auto& c : a.ch)
        applyGain (c, dbToAmp (rmsDbFs - loud));
}

/** The same checks at either Detection. noise_is_left_alone runs it at Fast (the default) and
    steady_noise_is_left_alone at Steady, whose steady level needs longer to settle on a new
    signal, so its figures are taken from 4 s on rather than 2. */
void noiseIsLeftAlone (Ctx& t, dehonk::Detection detection)
{
    const bool steady = detection == dehonk::Detection::steady;
    const double seconds = 20.0, settle = steady ? 4.0 : 2.0;
    const dehonk::Params defaults;
    const double programDb = (double) defaults.thresholdDb + 8.0;   // 8 dB above the default threshold
    Audio noise = bandNoise (seconds, 1301);
    scaleTo (noise, programDb);
    const Spectrum sIn = powerSpectrum (noise, S (settle), noise.length());

    std::string head;
    for (int config = 0; config < 2; ++config)
    {
        RenderSpec s;
        s.params.detection = detection;
        if (config == 1)
            for (auto& on : s.params.moduleOn)
                on = true;
        s.collectFrames = true;
        const auto r = render (noise, s);
        std::vector<double> deepest;
        for (const auto& f : r.frames)
            if (f.timeSec >= settle)
                deepest.push_back (f.totalCutDb);
        const double under = 100.0 * (double) std::count_if (deepest.begin(), deepest.end(), [] (double v) { return v < 0.5; })
                             / (double) std::max<std::size_t> (1, deepest.size());
        const double mean = deepest.empty() ? 0.0 : std::accumulate (deepest.begin(), deepest.end(), 0.0) / (double) deepest.size();
        const std::string label = moduleList (s.params);
        double programSeen = 0.0;
        for (const auto& f : r.frames)
            programSeen = std::max (programSeen, (double) f.activity);
        t.check (under >= 99.5, fmt ("%s, noise 80-600 Hz at %.0f dBFS RMS (threshold %.0f): deepest notch under 0.5 dB in %.2f%% of frames after %.0f s (need 99.5%%)",
                                     label.c_str(), programDb, (double) defaults.thresholdDb, under, settle));
        t.check (mean < 0.15, fmt ("%s: mean deepest notch %.3f dB (limit 0.15)", label.c_str(), mean));
        // A ceiling that must not rise, until the detector work of SPEC 13.15 brings these inside
        // the limits above: what the core gave before Release was a control (release 122 ms, Max
        // Cut 8 dB), measured on this signal on 17 November 2026. A longer release on plain noise
        // would show here first.
        {
            const double wasUnder = config == 0 ? 69.25 : 33.18, wasMean = config == 0 ? 0.390 : 1.209;
            t.check (under >= wasUnder && mean <= wasMean,
                     fmt ("%s: no worse than before Release was a control: under 0.5 dB in %.2f%% of frames (was %.2f%%, must not fall), mean deepest notch %.3f dB (was %.3f, must not rise)",
                          label.c_str(), under, wasUnder, mean, wasMean));
        }
        t.info (fmt ("%s: deepest notch percentiles: 50%% %.2f, 90%% %.2f, 95%% %.2f, 99%% %.2f, 99.5%% %.2f, 99.9%% %.2f dB, max %.2f dB; activity reached %.2f",
                     label.c_str(), percentile (deepest, 0.5), percentile (deepest, 0.9), percentile (deepest, 0.95), percentile (deepest, 0.99),
                     percentile (deepest, 0.995), percentile (deepest, 0.999), deepest.empty() ? 0.0 : *std::max_element (deepest.begin(), deepest.end()),
                     programSeen));
        // Where the cut lands and what it does to the level, own analysis.
        const Spectrum sOut = powerSpectrum (r.out, S (settle), r.out.length());
        const auto change = bandChanges (sIn, sOut, r.bandHz);
        double worst = 0.0, worstHz = 0.0;
        for (std::size_t k = 0; k < change.size(); ++k)
            if (r.bandHz[k] >= 80.0 && r.bandHz[k] <= 600.0 && std::abs (change[k]) > std::abs (worst))
            {
                worst = change[k];
                worstHz = r.bandHz[k];
            }
        const auto cs = cutStats (r, settle);
        int busiest = 0;
        for (std::size_t k = 0; k < cs.max.size(); ++k)
            if (cs.mean[k] > cs.mean[(std::size_t) busiest])
                busiest = (int) k;
        t.info (fmt ("%s: largest long-term level change 80-600 Hz %+.2f dB at %.0f Hz; band cut most on average %.0f Hz (mean %.3f dB, max %.2f dB)",
                     label.c_str(), worst, worstHz, (double) r.bandHz[(std::size_t) busiest], cs.mean[(std::size_t) busiest], cs.max[(std::size_t) busiest]));
        head += fmt ("%s%s: under 0.5 dB %.2f%% (need 99.5), mean %.3f dB (limit 0.15), p99 %.2f dB", head.empty() ? "" : "; ", label.c_str(), under, mean,
                     percentile (deepest, 0.99));
    }

    // A steady resonance 6 dB proud of the same noise, default modules: a sine, and a band of noise
    // 1/12 octave wide, at 160 and at 250 Hz. "Proud" is against the noise's own power in the
    // 1/12-octave band around the frequency.
    const Spectrum sNoise = powerSpectrum (noise, 0, noise.length());
    for (double hz : { 160.0, 250.0 })
        for (int kind = 0; kind < 2; ++kind)
        {
            Audio in;
            if (kind == 0)
            {
                in = noise;
                const double amp = sineAmpFor (bandLevelDb (sNoise, hz) + 6.0);
                for (int c = 0; c < 2; ++c)
                {
                    const auto tone = sine (kSr, in.length(), hz, amp, 0.3 + c);
                    for (std::size_t i = 0; i < in.length(); ++i)
                        in.ch[(std::size_t) c][i] += tone[i];
                }
            }
            else
            {
                // The same random draws with the band raised so it carries 6 dB more than the noise
                // alone does there, then scaled by the gain the noise alone was given.
                const double lo = hz * std::exp2 (-1.0 / 24.0), hi = hz * std::exp2 (1.0 / 24.0);
                in = bandNoise (seconds, 1301, [lo, hi] (double f) { return f >= lo && f < hi ? 6.0 : 0.0; });
                const Audio plain = bandNoise (seconds, 1301);
                for (int c = 0; c < 2; ++c)
                    applyGain (in.ch[(std::size_t) c], dbToAmp (rmsDb (noise.ch[(std::size_t) c]) - rmsDb (plain.ch[(std::size_t) c])));
            }
            RenderSpec s;
            s.params.detection = detection;
            s.collectFrames = true;
            const auto r = render (in, s);
            const auto [k0, k1] = bandsAround (r.bandHz, hz);
            const double m0 = meanBandCut (r, k0, settle), m1 = meanBandCut (r, k1, settle);
            const Spectrum si = powerSpectrum (in, S (settle), in.length()), so = powerSpectrum (r.out, S (settle), r.out.length());
            const double realised = bandLevelDb (so, hz) - bandLevelDb (si, hz);
            const double sideLo = so.centredDb (hz * std::exp2 (-1.0 / 3.0), 1.0 / 24.0) - si.centredDb (hz * std::exp2 (-1.0 / 3.0), 1.0 / 24.0);
            const double sideHi = so.centredDb (hz * std::exp2 (1.0 / 3.0), 1.0 / 24.0) - si.centredDb (hz * std::exp2 (1.0 / 3.0), 1.0 / 24.0);
            const double best = std::max (m0, m1);
            t.check (best >= 3.0, fmt ("%s at %.0f Hz, 6 dB proud of the noise: mean cut %.2f dB at %.1f Hz and %.2f dB at %.1f Hz (need 3 dB in one of them); level at %.0f Hz %+.2f dB, a third of an octave either side %+.2f / %+.2f dB",
                                       kind == 0 ? "sine" : "1/12-octave band", hz, m0, (double) r.bandHz[(std::size_t) k0], m1,
                                       (double) r.bandHz[(std::size_t) k1], hz, realised, sideLo, sideHi));
            head += fmt ("; %s %.0f Hz %.2f dB", kind == 0 ? "sine" : "band", hz, best);
            if (kind == 0)
            {
                // For the record: the same with the parent rule off (a resonance in this noise has
                // nothing an octave below it, so the rule takes it for a fundamental).
                RenderSpec off = s;
                off.useTuning = true;
                off.tuning = defaultTuning();
                off.tuning.parentRule = 0.0f;
                const auto ro = render (in, off);
                t.info (fmt ("sine at %.0f Hz with the parent rule off: mean cut %.2f / %.2f dB at %.1f / %.1f Hz",
                             hz, meanBandCut (ro, k0, settle), meanBandCut (ro, k1, settle), (double) ro.bandHz[(std::size_t) k0], (double) ro.bandHz[(std::size_t) k1]));
            }
        }

    // For the record: a broad lift, +6 dB over half an octave and over an octave (raised cosine in
    // log frequency, full width at half height) centred on 200 Hz: the deepest notch it gets. A lift
    // that wide is not a build-up to be notched.
    for (double widthOct : { 0.5, 1.0 })
    {
        Audio in = bandNoise (seconds, 1301, [widthOct] (double f) {
            const double x = std::log2 (f / 200.0) / widthOct;   // half height at x = +-0.5
            return std::abs (x) >= 1.0 ? 0.0 : 6.0 * 0.5 * (1.0 + std::cos (std::numbers::pi * x));
        });
        scaleTo (in, programDb);
        RenderSpec s;
        s.params.detection = detection;
        s.collectFrames = true;
        const auto r = render (in, s);
        std::vector<double> deepest;
        for (const auto& f : r.frames)
            if (f.timeSec >= settle)
                deepest.push_back (f.totalCutDb);
        const double mean = deepest.empty() ? 0.0 : std::accumulate (deepest.begin(), deepest.end(), 0.0) / (double) deepest.size();
        const Spectrum si = powerSpectrum (in, S (settle), in.length()), so = powerSpectrum (r.out, S (settle), r.out.length());
        t.info (fmt ("+6 dB lift %.1f octave wide at 200 Hz: deepest notch mean %.2f dB, 99%% %.2f dB; level at 200 Hz %+.2f dB",
                     widthOct, mean, percentile (deepest, 0.99), bandLevelDb (so, 200.0) - bandLevelDb (si, 200.0)));
    }
    t.headline = head;
}

void testNoiseIsLeftAlone (Ctx& t)       { noiseIsLeftAlone (t, dehonk::Detection::fast); }
void testSteadyNoiseIsLeftAlone (Ctx& t) { noiseIsLeftAlone (t, dehonk::Detection::steady); }

//==============================================================================
// impacts_keep_body_tail_front (SPEC 13.2)

struct ImpactSignal
{
    Audio in;
    Audio hits;   // the impacts alone, as mixed into in
    std::vector<std::size_t> onsets;
    std::vector<std::string> desc;
};

/** Ten big impacts over a music-like bed in D minor, mono. Each impact is a low thump (50 to
    70 Hz, gliding down from half as high again, decaying to -60 dB in 0.6 to 1.2 s), four to six
    body modes between 110 and 400 Hz (one or two of them a near-harmonic of the thump, so they
    have a parent below them; the rest anywhere), each ringing to -60 dB in 0.3 to 1.5 s, and a
    broadband noise burst with a 20 to 80 ms time constant. Onsets every 2.5 s from 2 s, peaks
    from -9 to -3 dBFS. The bed changes chord every 5 s: a bass note with its fifth and octave and
    a triad two octaves up, sample peak 12 dB below the loudest impact. */
ImpactSignal impactSignal (std::uint64_t seed)
{
    ImpactSignal s;
    const double seconds = 27.5;
    const std::size_t n = S (seconds);
    Rng rng (seed);
    constexpr double twoPi = 2.0 * std::numbers::pi;

    // The bed.
    std::vector<double> bed (n, 0.0);
    struct Chord
    {
        double root;
        bool minor;
    };
    const Chord chords[] = { { 73.42, true }, { 58.27, false }, { 87.31, false }, { 65.41, false }, { 49.00, true }, { 55.00, false } };
    const double seg = 5.0, fadeIn = 0.08, fadeOut = 0.15;
    for (int c = 0; c * seg < seconds; ++c)
    {
        const Chord& ch = chords[c % 6];
        const double fifth = std::exp2 (7.0 / 12.0), third = std::exp2 ((ch.minor ? 3.0 : 4.0) / 12.0);
        struct Voice
        {
            double hz, amp;
        };
        const Voice voices[] = {
            { ch.root, 1.0 },             { ch.root * 2.0, 0.2 },        { ch.root * 3.0, 0.08 },   // bass note and its harmonics
            { ch.root * fifth, 0.35 },    { ch.root * 2.0, 0.2 },                                     // fifth and octave
            { ch.root * 4.0, 0.1 },       { ch.root * 4.0 * third, 0.1 }, { ch.root * 4.0 * fifth, 0.1 },   // triad two octaves up
        };
        const std::size_t a = S (c * seg), len = S (seg + fadeOut);
        for (const auto& v : voices)
        {
            const double ph = rng.uniform (0.0, twoPi), w = twoPi * v.hz / kSr;
            for (std::size_t i = 0; i < len && a + i < n; ++i)
            {
                const double tt = (double) i / kSr;
                const double env = std::min (1.0, tt / fadeIn) * std::clamp ((seg + fadeOut - tt) / fadeOut, 0.0, 1.0);
                bed[a + i] += v.amp * env * std::sin (w * (double) i + ph);
            }
        }
    }

    // The impacts.
    std::vector<double> hits (n, 0.0);
    double loudest = 0.0;
    for (int h = 0; h < 10; ++h)
    {
        const std::size_t at = S (2.0 + 2.5 * h);
        const double peakDb = rng.uniform (-9.0, -3.0);
        const double f0 = rng.uniform (50.0, 70.0), thumpT60 = rng.uniform (0.6, 1.2);
        const int modes = rng.uniformInt (4, 6), harmonic = rng.uniformInt (1, 2);
        const double noiseTau = rng.uniform (0.020, 0.080);
        const std::size_t len = std::min (n - at, S (2.4));
        std::vector<double> x (len, 0.0);
        // Thump.
        double ph = 0.0;
        for (std::size_t i = 0; i < len; ++i)
        {
            const double tt = (double) i / kSr;
            ph += twoPi * f0 * (1.0 + 0.5 * std::exp (-tt / 0.03)) / kSr;
            x[i] += std::min (1.0, tt / 0.003) * std::exp (-tt * 6.9078 / thumpT60) * std::sin (ph);
        }
        std::string d = fmt ("thump %.0f Hz T60 %.2f s; modes", f0, thumpT60);
        for (int m = 0; m < modes; ++m)
        {
            double f;
            if (m < harmonic)
                f = f0 * (double) (m + 2) * rng.uniform (0.985, 1.015);    // near 2 x and 3 x the thump
            else
                f = 110.0 * std::pow (400.0 / 110.0, rng.uniform());
            f = std::clamp (f, 110.0, 400.0);
            const double t60 = rng.uniform (0.3, 1.5), amp = rng.uniform (0.25, 0.6), mph = rng.uniform (0.0, twoPi);
            for (std::size_t i = 0; i < len; ++i)
            {
                const double tt = (double) i / kSr;
                x[i] += amp * std::min (1.0, tt / 0.002) * std::exp (-tt * 6.9078 / t60) * std::sin (twoPi * f * tt + mph);
            }
            d += fmt (" %.0f Hz/%.1f s%s", f, t60, m < harmonic ? "*" : "");
        }
        for (std::size_t i = 0; i < len; ++i)
            x[i] += 0.5 * rng.gauss() * std::exp (-((double) i / kSr) / noiseTau);
        d += fmt ("; burst %.0f ms; peak %.1f dBFS", 1000.0 * noiseTau, peakDb);
        double mx = 0.0;
        for (double v : x)
            mx = std::max (mx, std::abs (v));
        const double g = dbToAmp (peakDb) / mx;
        for (std::size_t i = 0; i < len; ++i)
            hits[at + i] += g * x[i];
        loudest = std::max (loudest, dbToAmp (peakDb));
        s.onsets.push_back (at);
        s.desc.push_back (d);
    }
    double bedPeak = 0.0;
    for (double v : bed)
        bedPeak = std::max (bedPeak, std::abs (v));
    const double gb = loudest * dbToAmp (-12.0) / bedPeak;
    std::vector<float> out (n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = (float) (hits[i] + gb * bed[i]);
    s.in = fromChannels (kSr, { out });
    std::vector<float> hf (n);
    for (std::size_t i = 0; i < n; ++i)
        hf[i] = (float) hits[i];
    s.hits = fromChannels (kSr, { hf });
    return s;
}

/** Bands with a cut above 1 dB between from and to (seconds), and their deepest cut. */
std::string notchesBetween (const RenderResult& r, double from, double to)
{
    std::vector<double> mx ((std::size_t) r.numBands, 0.0);
    for (const auto& f : r.frames)
        if (f.timeSec >= from && f.timeSec < to)
            for (int k = 0; k < r.numBands; ++k)
                mx[(std::size_t) k] = std::max (mx[(std::size_t) k], (double) f.cutDb[(std::size_t) k]);
    std::string s;
    for (int k = 0; k < r.numBands; ++k)
        if (mx[(std::size_t) k] > 1.0)
            s += fmt (" %.0f Hz (%s) %.1f dB", (double) r.bandHz[(std::size_t) k], noteLabel (r.bandHz[(std::size_t) k]).c_str(), mx[(std::size_t) k]);
    return s.empty() ? " none" : s;
}

/** Scales an impact signal (the mix and the impacts alone together). */
ImpactSignal scaled (const ImpactSignal& sig, double gainDb)
{
    ImpactSignal r = sig;
    for (auto& c : r.in.ch)
        applyGain (c, dbToAmp (gainDb));
    for (auto& c : r.hits.ch)
        applyGain (c, dbToAmp (gainDb));
    return r;
}

struct ImpactWorst
{
    double body = 0.0, tail = 0.0, front = 0.0;
    int bodyAt = -1, tailAt = -1, frontAt = -1, tailMeasurable = 0;
    double aloneBody = 0.0, aloneFront = 0.0;   // the impacts alone through the filters the mix got
    int aloneBodyAt = -1, aloneFrontAt = -1;
    const char* frontSpeed = "";
    int gateShut = 0;   // impacts during whose 1.5 s the gate never opened
};

/** Body, Tail and Front for every impact of sig with params (Speed 0 and 100% for Front too),
    logged under label. Body and Front are on the mix; Tail on the impacts alone through the
    filters the mix got (see below). */
ImpactWorst impactCase (Ctx& t, const char* label, const ImpactSignal& sig, const dehonk::Params& params)
{
    const std::size_t no = sig.onsets.size();
    RenderSpec s;
    s.params = params;
    s.collectFrames = true;
    const auto r = render (sig.in, s);
    const BandPair bp = bandPair (sig.in, r.out, 110.0, 400.0);

    // The impacts alone through the very filters the mix got. With Link at 100% both channels get
    // the same cuts, and the filters are linear for a given set of cuts, so with the mix in one
    // channel and the mix minus a small fraction d of the impacts in the other, (out0 - out1) / d
    // is the impacts alone through those filters. d is small enough that the detector hears the
    // same thing in both channels (the bands' powers differ by under 0.15 dB at most).
    constexpr double d = 1.0 / 64.0;
    Audio pair = Audio::make (kSr, 2, sig.in.length());
    for (std::size_t i = 0; i < sig.in.length(); ++i)
    {
        pair.ch[0][i] = sig.in.ch[0][i];
        pair.ch[1][i] = (float) ((double) sig.in.ch[0][i] - d * (double) sig.hits.ch[0][i]);
    }
    RenderSpec sd = s;
    sd.params.link = 1.0f;
    sd.collectFrames = false;
    const auto rd = render (pair, sd);
    Audio hitOut = Audio::make (kSr, 1, sig.in.length());
    for (std::size_t i = 0; i < sig.in.length(); ++i)
        hitOut.ch[0][i] = (float) (((double) rd.out.ch[0][i] - (double) rd.out.ch[1][i]) / d);
    const BandPair bpHit = bandPair (sig.hits, hitOut, 110.0, 400.0);

    std::vector<double> front0, front100;
    for (float speed : { 0.0f, 1.0f })
    {
        RenderSpec sp = s;
        sp.params.speed = speed;
        sp.collectFrames = false;
        const auto rs = render (sig.in, sp);
        const BandPair b = bandPair (sig.in, rs.out, 110.0, 400.0);
        for (std::size_t i = 0; i < no; ++i)
            (speed == 0.0f ? front0 : front100).push_back (frontChangeDb (b, sig.onsets[i]));
    }

    ImpactWorst w;
    double mixTail = 0.0;
    int mixMeasurable = 0;
    t.info (fmt ("[%s] threshold %.0f dBFS, Speed %.0f%%. Per impact: Body (dB, 110-400 Hz energy over 1.5 s) | Tail of the mix in -> out (ms for the fitted decay to fall 20 dB, change) | Front at Speed %.0f%% / 0%% / 100%% (dB) | gate | what it is",
                 label, (double) params.thresholdDb, 100.0 * (double) params.speed, 100.0 * (double) params.speed));
    for (std::size_t i = 0; i < no; ++i)
    {
        const ImpactFigures f = impactFigures (bp, sig.onsets[i], i + 1 < no ? sig.onsets[i + 1] : 0);
        const double onsetSec = (double) sig.onsets[i] / kSr;
        // The gate during the 1.5 s after the onset, from the display frames.
        std::size_t inWin = 0, openFrames = 0;
        double maxAct = 0.0, maxProg = -400.0, actAtOnset = 0.0;
        bool onsetSeen = false;
        for (const auto& fr : r.frames)
            if (fr.timeSec >= onsetSec && fr.timeSec < onsetSec + 1.5)
            {
                ++inWin;
                openFrames += fr.activity > 0.0f ? 1 : 0;
                maxAct = std::max (maxAct, (double) fr.activity);
                maxProg = std::max (maxProg, (double) fr.programDb);
                if (! onsetSeen)
                {
                    actAtOnset = fr.activity;
                    onsetSeen = true;
                }
            }
        w.gateShut += maxAct > 0.0 ? 0 : 1;
        t.info (fmt ("%2zu at %5.2f s: Body %+.2f | Tail %s | Front %+.2f / %+.2f / %+.2f | gate open %.0f%% of the 1.5 s, activity at onset %.2f, max %.2f, programme max %.1f dBFS | %s",
                     i + 1, onsetSec, f.bodyDb,
                     f.tailMeasurable ? fmt ("%.0f -> %.0f ms (%+.1f%%, fitted over %.0f dB)", f.tailInMs, f.tailOutMs, f.tailChangePct, f.fallDb).c_str()
                                      : fmt ("not measurable (%.1f dB of decay above the bed%s)", f.fallDb,
                                             f.fallDb >= ImpactSpec().minFallDb ? ", but the curve does not fall: the bed changes under the tail" : "").c_str(),
                     f.frontDb, front0[i], front100[i], 100.0 * (double) openFrames / (double) std::max<std::size_t> (1, inWin), actAtOnset, maxAct,
                     maxProg, sig.desc[i].c_str()));
        const ImpactFigures h = impactFigures (bpHit, sig.onsets[i], i + 1 < no ? sig.onsets[i + 1] : 0);
        t.info (fmt ("%2zu the impact alone through the same filters: Body %+.2f | Tail %s | Front %+.2f", i + 1, h.bodyDb,
                     h.tailMeasurable ? fmt ("%.0f -> %.0f ms (%+.1f%%, fitted over %.0f dB)", h.tailInMs, h.tailOutMs, h.tailChangePct, h.fallDb).c_str()
                                      : "not measurable",
                     h.frontDb));
        t.info (fmt ("%2zu notches during the 1.5 s after the onset:%s", i + 1, notchesBetween (r, onsetSec, onsetSec + 1.5).c_str()));
        t.info (fmt ("%2zu notches already open at the onset:%s", i + 1, notchesBetween (r, onsetSec - 0.05, onsetSec).c_str()));
        if (std::abs (h.bodyDb) > std::abs (w.aloneBody))
        {
            w.aloneBody = h.bodyDb;
            w.aloneBodyAt = (int) i;
        }
        if (std::abs (h.frontDb) > std::abs (w.aloneFront))
        {
            w.aloneFront = h.frontDb;
            w.aloneFrontAt = (int) i;
        }
        if (h.tailMeasurable)
        {
            ++w.tailMeasurable;
            if (std::abs (h.tailChangePct) > std::abs (w.tail))
            {
                w.tail = h.tailChangePct;
                w.tailAt = (int) i;
            }
        }
        if (f.tailMeasurable)
        {
            ++mixMeasurable;
            mixTail = std::abs (f.tailChangePct) > std::abs (mixTail) ? f.tailChangePct : mixTail;
        }
        if (std::abs (f.bodyDb) > std::abs (w.body))
        {
            w.body = f.bodyDb;
            w.bodyAt = (int) i;
        }
        const std::pair<double, const char*> fronts[] = { { f.frontDb, "default" }, { front0[i], "0%" }, { front100[i], "100%" } };
        for (const auto& [v, name] : fronts)
            if (std::abs (v) > std::abs (w.front))
            {
                w.front = v;
                w.frontAt = (int) i;
                w.frontSpeed = name;
            }
    }
    t.info (fmt ("[%s] Tail of the mix, over the decay the bed leaves above it: measurable on %d of %zu impacts, worst change %+.1f%%", label,
                 mixMeasurable, no, mixTail));
    return w;
}

void testImpactsKeepBodyTailFront (Ctx& t)
{
    const ImpactSignal raw = impactSignal (1313);
    const std::size_t no = raw.onsets.size();
    const dehonk::Params defaults;

    // The programme level the core measures (it does not depend on the threshold): the loudest it
    // gets in the 300 ms after each onset, and on the bed alone (the 400 ms before each onset).
    std::vector<double> hitLevels, bedLevels;
    {
        RenderSpec s;
        s.params = defaults;
        s.collectFrames = true;
        const auto r = render (raw.in, s);
        for (std::size_t i = 0; i < no; ++i)
        {
            const double at = (double) raw.onsets[i] / kSr;
            double hit = -400.0, bed = -400.0;
            for (const auto& f : r.frames)
            {
                if (f.timeSec >= at && f.timeSec < at + 0.3)
                    hit = std::max (hit, (double) f.programDb);
                if (f.timeSec >= at - 0.4 && f.timeSec < at - 0.02)
                    bed = std::max (bed, (double) f.programDb);
            }
            hitLevels.push_back (hit);
            bedLevels.push_back (bed);
        }
    }
    // Scale so the bed alone sits just under the default threshold (its loudest 0.5 dB under), so
    // De-Honk is not already at work on the bed when an impact arrives, and the impacts take the
    // programme over it. The bed is dense and the impacts are short, so the core's programme level
    // (RMS) rises only about 2 to 8 dB over the bed on an impact, although the impacts' sample
    // peaks are 12 dB over the bed's: 8 dB over the threshold for the median impact (as asked)
    // would put the bed itself well over the threshold. The level each impact reaches is logged.
    const double bedRaw = *std::max_element (bedLevels.begin(), bedLevels.end());
    const double gainDb = (double) defaults.thresholdDb - 0.5 - bedRaw;
    const ImpactSignal sig = scaled (raw, gainDb);
    const double hitLo = *std::min_element (hitLevels.begin(), hitLevels.end()) + gainDb, hitHi = *std::max_element (hitLevels.begin(), hitLevels.end()) + gainDb;
    const double hitMed = percentile (hitLevels, 0.5) + gainDb, bedHi = bedRaw + gainDb;
    t.info (fmt ("signal scaled by %+.1f dB: the programme level peaks %.1f to %.1f dBFS on the impacts (median %.1f, %.1f dB over the threshold), the bed alone reaches %.1f dBFS; default threshold %.0f dBFS, full effect %.0f dB over it",
                 gainDb, hitLo, hitHi, hitMed, hitMed - (double) defaults.thresholdDb, bedHi, (double) defaults.thresholdDb, (double) defaultTuning().thresholdKneeDb));
    t.check (hitLo > (double) defaults.thresholdDb,
             fmt ("every impact takes the programme over the threshold: the quietest reaches %.1f dBFS against %.0f", hitLo, (double) defaults.thresholdDb));

    // Harness self-check: the input's own Tail must not depend on the measure's settings. The
    // impacts alone, each against itself, with the fitted span 19 and 21 dB in place of 20, the fit
    // starting 4 or 6 dB down in place of 5, and the bed guard's envelope 10 or 40 ms in place of 20.
    {
        const BandPair self = bandPair (sig.hits, sig.hits, 110.0, 400.0);
        double worstDev = 0.0;
        int worstAt = -1;
        const char* worstWhat = "";
        std::string per;
        for (std::size_t i = 0; i < no; ++i)
        {
            const std::size_t next = i + 1 < no ? sig.onsets[i + 1] : 0;
            const double ref = impactFigures (self, sig.onsets[i], next).tailInMs;
            per += fmt (" %.0f", ref);
            struct Variant
            {
                const char* what;
                ImpactSpec spec;
            };
            std::vector<Variant> vs;
            for (double fall : { 19.0, 21.0 })
            {
                ImpactSpec v;
                v.tailFallDb = fall;
                vs.push_back ({ fall < 20.0 ? "fall 19 dB" : "fall 21 dB", v });
            }
            for (double st : { 4.0, 6.0 })
            {
                ImpactSpec v;
                v.fitStartDb = st;
                vs.push_back ({ st < 5.0 ? "start 4 dB" : "start 6 dB", v });
            }
            for (double ms : { 10.0, 40.0 })
            {
                ImpactSpec v;
                v.envelopeMs = ms;
                vs.push_back ({ ms < 20.0 ? "envelope 10 ms" : "envelope 40 ms", v });
            }
            for (const auto& v : vs)
            {
                const double x = impactFigures (self, sig.onsets[i], next, v.spec).tailInMs;
                // Tail is the time to fall tailFallDb; compare the decay rate, i.e. per 20 dB.
                const double per20 = x > 0.0 ? x * 20.0 / v.spec.tailFallDb : -1.0;
                const double dev = ref > 0.0 && per20 > 0.0 ? 100.0 * (per20 - ref) / ref : 1000.0;
                if (std::abs (dev) > std::abs (worstDev))
                {
                    worstDev = dev;
                    worstAt = (int) i;
                    worstWhat = v.what;
                }
            }
        }
        t.info ("Tail of each impact alone (ms to fall 20 dB, fitted):" + per);
        t.check (std::abs (worstDev) < 3.0, fmt ("harness: the input's own Tail moves %+.2f%% at most when the measure's settings change (impact %d, %s) (limit 3%%, so a 10%% limit on the core means something)",
                                                worstDev, worstAt + 1, worstWhat));
    }

    // At the plug-in's defaults, threshold included: what SPEC 13.2 asks.
    const ImpactWorst w = impactCase (t, "defaults", sig, defaults);
    t.check (w.gateShut == 0, fmt ("defaults: the gate opened during the 1.5 s after every impact: shut throughout on %d of %zu (the figures mean nothing for those)",
                                   w.gateShut, no));
    t.check (std::abs (w.body) < 1.0, fmt ("defaults: Body worst change %+.2f dB (impact %d) (limit 1 dB, SPEC 13.2, to measure)", w.body, w.bodyAt + 1));
    t.check (w.tailMeasurable == (int) no, fmt ("defaults: Tail of the impacts alone measurable (20 dB fitted) on %d of %zu", w.tailMeasurable, no));
    t.check (std::abs (w.tail) < 10.0, fmt ("defaults: Tail worst change %+.1f%% (impact %d), the impact alone through the filters the mix got (limit 10%%, to measure)",
                                            w.tail, w.tailAt + 1));
    t.check (std::abs (w.front) < 1.0, fmt ("defaults: Front worst change %+.2f dB (impact %d, Speed %s) (limit 1 dB at every Speed, to measure)", w.front,
                                            w.frontAt + 1, w.frontSpeed));

    // The same signal at its original level with the threshold at -60 dBFS, so De-Honk is always
    // acting and is already cutting the bed's chord tones when each impact arrives. Here the
    // question is what the impact itself loses, so Body is taken on the impacts alone through the
    // filters the mix got, like Tail (on the mix, Body over 1.5 s is mostly the bed's chord tones
    // being cut once the impact has died away, which is the plug-in's job; it is logged). Front is
    // on the mix, as above. Body and Front have the limits above. Tail has a wider one: once an
    // impact has fallen under what was there before it, the notches come back, which this
    // measure counts (it follows the impact alone 20 dB down, well under the bed) although under
    // the bed it cannot be heard. Keeping them lifted longer was measured on the reference mixes
    // and gives up too much of the cutting (SPEC 13.16).
    dehonk::Params always = defaults;
    always.thresholdDb = kAlwaysDb;
    const ImpactWorst a = impactCase (t, "always acting, threshold -60", raw, always);
    t.info (fmt ("[always acting, threshold -60] on the mix: worst Body %+.2f dB (impact %d), Front %+.2f dB (impact %d, Speed %s); the impacts alone: Body %+.2f dB (impact %d), Tail %+.1f%% (impact %d), Front %+.2f dB (impact %d)",
                 a.body, a.bodyAt + 1, a.front, a.frontAt + 1, a.frontSpeed, a.aloneBody, a.aloneBodyAt + 1, a.tail, a.tailAt + 1, a.aloneFront, a.aloneFrontAt + 1));
    t.check (std::abs (a.aloneBody) < 1.0, fmt ("always acting: Body of the impact alone, worst change %+.2f dB (impact %d) (limit 1 dB)", a.aloneBody, a.aloneBodyAt + 1));
    t.check (std::abs (a.front) < 1.0, fmt ("always acting: Front worst change %+.2f dB (impact %d, Speed %s) (limit 1 dB at every Speed)", a.front, a.frontAt + 1, a.frontSpeed));
    t.check (std::abs (a.tail) < 25.0, fmt ("always acting: Tail worst change %+.1f%% (impact %d), the impact alone through the filters the mix got (limit 25%%; 10%% at the default threshold)",
                                            a.tail, a.tailAt + 1));

    // The guard must be what does it: with Impact Guard off the same figures are well outside.
    dehonk::Params unguarded = always;
    unguarded.impactGuard = 0.0f;
    const ImpactWorst u = impactCase (t, "always acting, Impact Guard off", raw, unguarded);
    t.info (fmt ("[always acting, Impact Guard off] on the mix: worst Body %+.2f dB, Front %+.2f dB; the impacts alone: Body %+.2f dB, Tail %+.1f%%, Front %+.2f dB", u.body, u.front, u.aloneBody,
                 u.tail, u.aloneFront));
    t.check (std::abs (u.front) > 2.0 && std::abs (u.aloneBody) > std::abs (a.aloneBody) + 0.5,
             fmt ("always acting with Impact Guard off: Front %+.2f dB, Body of the impact alone %+.2f dB (the guard must be what keeps them: Front over 2 dB, Body at least 0.5 dB worse than with it)",
                  u.front, u.aloneBody));

    t.headline = fmt ("defaults (threshold %.0f): worst Body %+.2f dB (limit 1), Tail %+.1f%% (limit 10), Front %+.2f dB (limit 1), gate shut on %d; always acting (threshold -60): Body of the impact alone %+.2f (limit 1), Front %+.2f (limit 1), Tail %+.1f%% (limit 25), Body on the mix %+.2f; Impact Guard off: Front %+.2f, Body alone %+.2f",
                      (double) defaults.thresholdDb, w.body, w.tail, w.front, w.gateShut, a.aloneBody, a.front, a.tail, a.body, u.front, u.aloneBody);
}

//==============================================================================
// headroom (SPEC 13.6, informational)

void headroomReport (Ctx& t, const char* name, const Audio& sig, const dehonk::Params& params, std::string& head)
{
    // Raise the input so the untreated signal needs 4.5 dB of limiting at its loudest.
    const double ceilingDb = -1.0;
    const double peak = peakAbs (sig, 0, sig.length());
    Audio in = sig;
    for (auto& c : in.ch)
        applyGain (c, dbToAmp (ceilingDb + 4.5) / peak);
    RenderSpec s;
    s.params = params;
    s.collectFrames = true;
    const auto r = render (in, s);
    const auto lu = referenceLimiter (in, ceilingDb), lt = referenceLimiter (r.out, ceilingDb);

    // Where the gate was open, from the display frames (activity above zero).
    std::vector<char> open (in.length(), 0);
    {
        std::size_t from = 0;
        for (const auto& f : r.frames)
        {
            const std::size_t to = std::min (in.length(), S (f.timeSec));
            if (f.activity > 0.0f)
                std::fill (open.begin() + (std::ptrdiff_t) from, open.begin() + (std::ptrdiff_t) to, 1);
            from = to;
        }
    }
    double pIn = 0.0, pOut = 0.0;
    std::size_t openCount = 0;
    for (std::size_t i = 0; i < in.length(); ++i)
        if (open[i])
        {
            ++openCount;
            for (int c = 0; c < in.numChannels(); ++c)
            {
                pIn = std::max (pIn, (double) std::abs (in.ch[(std::size_t) c][i]));
                pOut = std::max (pOut, (double) std::abs (r.out.ch[(std::size_t) c][i]));
            }
        }
    const std::size_t win = S (0.4), hop = S (0.1);
    std::vector<double> stDiff;
    double stMaxIn = -400.0, stMaxOut = -400.0;
    for (std::size_t a = 0; a + win <= in.length(); a += hop)
    {
        if (std::find (open.begin() + (std::ptrdiff_t) a, open.begin() + (std::ptrdiff_t) (a + win), 0) != open.begin() + (std::ptrdiff_t) (a + win))
            continue;
        const double li = powerToDb (meanSquare (in, a, a + win)), lo = powerToDb (meanSquare (r.out, a, a + win));
        stDiff.push_back (lo - li);
        stMaxIn = std::max (stMaxIn, li);
        stMaxOut = std::max (stMaxOut, lo);
    }
    const double stMean = stDiff.empty() ? 0.0 : std::accumulate (stDiff.begin(), stDiff.end(), 0.0) / (double) stDiff.size();
    t.info (fmt ("%s, raised so the untreated peak is %+.1f dBFS; limiter ceiling %.0f dBFS, 5 ms look-ahead, 100 ms release:", name,
                 ceilingDb + 4.5, ceilingDb));
    t.info (fmt ("  untreated: gain reduction mean %.3f dB (%.3f dB while limiting, %.1f%% of the time), max %.2f dB", lu.meanGrDb,
                 lu.meanGrWhileLimitingDb, lu.pctLimiting, lu.maxGrDb));
    t.info (fmt ("  De-Honk:   gain reduction mean %.3f dB (%.3f dB while limiting, %.1f%% of the time), max %.2f dB", lt.meanGrDb,
                 lt.meanGrWhileLimitingDb, lt.pctLimiting, lt.maxGrDb));
    t.info (fmt ("  difference: mean %+.3f dB, max %+.2f dB (negative = De-Honk leaves the limiter less to do)", lt.meanGrDb - lu.meanGrDb,
                 lt.maxGrDb - lu.maxGrDb));
    t.info (fmt ("  while the gate is open (%.0f%% of the time): sample peak %+.2f dB; short-term (400 ms) level %+.2f dB on average over %zu windows, loudest window %+.2f dB",
                 100.0 * (double) openCount / (double) std::max<std::size_t> (1, in.length()), ampToDb (pOut / std::max (1e-30, pIn)), stMean,
                 stDiff.size(), stMaxOut - stMaxIn));
    head += fmt ("%s%s: limiter mean %+.3f dB, max %+.2f dB, peak %+.2f dB", head.empty() ? "" : "; ", name, lt.meanGrDb - lu.meanGrDb,
                 lt.maxGrDb - lu.maxGrDb, ampToDb (pOut / std::max (1e-30, pIn)));
}

void testHeadroom (Ctx& t)
{
    std::string head;
    // At the plug-in's defaults, threshold included: the raised signals are well above it.
    // Then the same with the threshold at -60 dBFS, where De-Honk always acts.
    const dehonk::Params defaults;
    const dehonk::Params always = alwaysOn();
    const Audio impacts = impactSignal (1313).in;
    const Audio peaks = peaksSignal (kSr, 15.0, 2, { 151.0, 247.0 }, -12.0, 51);
    headroomReport (t, "impacts over a bed, default threshold", impacts, defaults, head);
    headroomReport (t, "impacts over a bed, threshold -60", impacts, always, head);
    headroomReport (t, "effect_on_peaks signal (151 and 247 Hz), default threshold", peaks, defaults, head);
    headroomReport (t, "effect_on_peaks signal, threshold -60", peaks, always, head);
    t.headline = head;
}

//==============================================================================
// key_profile (SPEC 13.4 step 1, informational except the profile itself)

void testKeyProfile (Ctx& t)
{
    // C minor over two octaves (C3, Eb3, G3, C4, Eb4, G4), each 15 dB above the bass-heavy bed in
    // its own 1/12-octave band, and an out-of-key steady resonance a quarter-tone sharp of F#3,
    // 12 dB above the bed. Mono, -14 dBFS RMS, 24 s.
    const dehonk::Tuning tune = defaultTuning();
    const double seconds = std::max (24.0, 3.0 * (double) tune.keySeconds);
    const std::size_t n = S (seconds);
    const double quarterSharpFs = 184.9972 * std::exp2 (1.0 / 24.0);
    const std::vector<double> hz = { 130.8128, 155.5635, 195.9977, 261.6256, 311.1270, 391.9954, quarterSharpFs };
    std::vector<std::vector<float>> levels;
    for (std::size_t i = 0; i < hz.size(); ++i)
        levels.push_back (std::vector<float> (1, i + 1 < hz.size() ? 15.0f : 12.0f));
    const Audio in = tonesOverBed (kSr, n, hz, levels, -14.0, 1401);
    RenderSpec s;
    s.params.thresholdDb = kAlwaysDb;
    s.collectFrames = true;
    s.frameEveryTick = true;   // notch open and close times to the tick
    const auto r = render (in, s);

    const double settle = 2.0 * (double) tune.keySeconds;
    std::size_t frames = 0, right = 0;
    std::array<float, 12> last {};
    for (const auto& f : r.frames)
        if (f.timeSec >= settle)
        {
            ++frames;
            const auto rank = pitchClassRanks (f.pitchClass);
            right += (rank[0] <= 3 && rank[3] <= 3 && rank[7] <= 3) ? 1 : 0;
            last = f.pitchClass;
        }
    const auto rank = pitchClassRanks (last);
    std::string prof;
    double top = 1e-30;
    for (float v : last)
        top = std::max (top, (double) v);
    for (int pc = 0; pc < 12; ++pc)
        prof += fmt (" %s %.1f dB (#%d)", pitchClassName (pc), powerToDb ((double) last[(std::size_t) pc] / top), rank[(std::size_t) pc]);
    t.check (frames > 0 && right == frames,
             fmt ("C, D# (Eb) and G are the three strongest pitch classes in %zu of %zu frames after %.0f s (need all)", right, frames, settle));
    t.info ("pitch class profile at the end, against the strongest:" + prof);

    // Every notch that opens, against the profile at that moment. Notches that open before the
    // profile has had Tuning::keySeconds to fill get no rank and are left out of the shares.
    NotchRule rule;
    rule.rankFromSec = (double) tune.keySeconds;
    const auto events = notchEvents (r, rule);
    int onTop3 = 0, rank4to12 = 0, unranked = 0, laterTop3 = 0, laterOther = 0;
    // For a notch that opened before the profile filled and was still open when it had, the
    // rank of its note at that moment (for information only, not in the shares).
    std::array<float, 12> profileFilled {};
    for (const auto& f : r.frames)
        if (f.timeSec >= rule.rankFromSec)
        {
            profileFilled = f.pitchClass;
            break;
        }
    const auto ranksFilled = pitchClassRanks (profileFilled);
    t.info (fmt ("%zu notches opened (cut above 1 dB, closed once under 0.5 dB for 100 ms); open time, band, note, rank of its pitch class (- = before %.0f s, profile still filling), in the top three, deepest cut:",
                 events.size(), rule.rankFromSec));
    for (std::size_t i = 0; i < events.size(); ++i)
    {
        const auto& e = events[i];
        const bool ranked = e.pcRankAtOpen > 0, top3 = ranked && e.pcRankAtOpen <= 3;
        unranked += ranked ? 0 : 1;
        onTop3 += top3 ? 1 : 0;
        rank4to12 += ranked && ! top3 ? 1 : 0;
        std::string later;
        if (! ranked && e.closeSec > rule.rankFromSec)
        {
            const int rk = ranksFilled[(std::size_t) noteOf (e.hz).pitchClass];
            later = fmt ("  (still open at %.0f s: rank %d then)", rule.rankFromSec, rk);
            laterTop3 += rk <= 3 ? 1 : 0;
            laterOther += rk > 3 ? 1 : 0;
        }
        if (i < 40)
            t.info (fmt ("  %6.2f-%6.2f s  %6.1f Hz  %-9s rank %2s  %s  %.2f dB%s", e.openSec, e.closeSec, e.hz, noteLabel (e.hz).c_str(),
                         ranked ? fmt ("%d", e.pcRankAtOpen).c_str() : "-", top3 ? "top 3" : "     ", e.deepestDb, later.c_str()));
    }
    if (events.size() > 40)
        t.info (fmt ("  ... %zu more", events.size() - 40));
    const Note res = noteOf (quarterSharpFs);
    t.info (fmt ("of those still open when the profile had filled: %d on a top-three pitch class then, %d on ranks 4 to 12", laterTop3, laterOther));
    t.info (fmt ("notches on a top-three pitch class %d, on ranks 4 to 12 %d, opened before %.0f s (no rank) %d; the resonance is at %.1f Hz (%s%+.0f cents), between the F#3 and G3 bands",
                 onTop3, rank4to12, rule.rankFromSec, unranked, quarterSharpFs, res.name().c_str(), res.cents));
    t.headline = fmt ("C, Eb, G strongest in %zu of %zu settled frames; %zu notches, %d on a top-three pitch class, %d on ranks 4-12, %d before the profile filled",
                      right, frames, events.size(), onTop3, rank4to12, unranked);
}

//==============================================================================
// cpu_benchmark

void testCpuBenchmark (Ctx& t)
{
    const double warm = 1.0, timed = 20.0;
    bool allOk = true;
    std::string head;
    // Peaks in all four modules, so every module has notches open when it is on, and 2.5 kHz clicks
    // every half second, so Snap has work when it is on. Each module set is timed with Snap off
    // and on, on the same signal.
    std::string clickAdds;
    for (double sr : { 48000.0, 96000.0 })
    {
        Audio in = peaksSignal (sr, warm + timed, 2, { 75.0, 151.0, 247.0, 700.0 }, -12.0, 121);
        {
            const auto bursts = burstSignal (sr, warm + timed, 2, BurstKind::tone, 2500.0, 3.0, 0.5, -200.0, 124, 0.25, 0.5);
            for (int c = 0; c < 2; ++c)
                for (std::size_t i = 0; i < in.length(); ++i)
                    in.ch[(std::size_t) c][i] += bursts.in.ch[(std::size_t) c][i];
        }
        const double target = sr < 60000.0 ? 1.0 : 2.0;
        double cfgPct[4][4] = {};   // [all + 2 * click][block size]
        for (int cfgIdx = 0; cfgIdx < 4; ++cfgIdx)
        {
            const bool all = (cfgIdx & 1) != 0, click = (cfgIdx & 2) != 0;
            double worstPct = 0.0;
            const char* cfgNames[4] = { "Tub+Fog", "all four modules", "Tub+Fog+Snap", "all four+Snap" };
            const char* cfg = cfgNames[cfgIdx];
            const int sizes[4] = { 32, 128, 1024, 2048 };
            for (int bi = 0; bi < 4; ++bi)
            {
                const int bs = sizes[bi];
                RenderSpec s;
                s.params = alwaysOn();
                s.params.snapOn = click;
                if (all)
                    for (auto& on : s.params.moduleOn)
                        on = true;
                s.blockSize = bs;
                s.timeBlocks = true;
                s.compensateLatency = false;
                const auto r = render (in, s);
                std::vector<double> times;
                double sum = 0.0;
                std::size_t samples = 0;
                for (std::size_t i = 0; i < r.blockSeconds.size(); ++i)
                    if (r.blockStarts[i] >= (std::int64_t) S (warm, sr) && r.blockSizes[i] == bs)
                    {
                        times.push_back (r.blockSeconds[i]);
                        sum += r.blockSeconds[i];
                        samples += (std::size_t) bs;
                    }
                if (times.empty())
                    continue;
                const double avg = sum / (double) times.size();
                const double worst = *std::max_element (times.begin(), times.end());
                const double p999 = percentile (times, 0.999);
                const double pct = 100.0 * sum / ((double) samples / sr);
                cfgPct[cfgIdx][bi] = pct;
                worstPct = std::max (worstPct, pct);
                const bool pctOk = pct < target, worstOk = worst <= 2.0 * avg;
                allOk = allOk && pctOk && (worstOk || ! t.strictCpu);
                t.check (pctOk, fmt ("%.0f kHz stereo, %s, block %4d: %.2f%% of one core (limit %.0f%%)", sr / 1000.0, cfg, bs, pct, target));
                const std::string line = fmt ("%.0f kHz stereo, %s, block %4d: block avg %.2f us, worst %.2f us (%.1fx avg; limit 2x), 99.9th pct %.2f us (%.1fx avg)",
                                              sr / 1000.0, cfg, bs, avg * 1e6, worst * 1e6, worst / std::max (1e-12, avg), p999 * 1e6,
                                              p999 / std::max (1e-12, avg));
                if (t.strictCpu)
                    t.check (worstOk, line);
                else
                    t.info (line + (worstOk ? " - meets the target" : " - misses the target (checked only with --strict-cpu)"));
            }
            head += fmt ("%s%.0f kHz %s worst %.2f%%", head.empty() ? "" : ", ", sr / 1000.0, cfg, worstPct);
        }
        for (int all = 0; all < 2; ++all)
        {
            std::string per;
            double mean = 0.0;
            for (int bi = 0; bi < 4; ++bi)
            {
                const double d = cfgPct[all + 2][bi] - cfgPct[all][bi];
                per += fmt ("%s%+.3f", per.empty() ? "" : ", ", d);
                mean += 0.25 * d;
            }
            t.info (fmt ("%.0f kHz stereo, %s: Snap adds %s percentage points at blocks 32, 128, 1024, 2048", sr / 1000.0,
                         all ? "all four modules" : "Tub+Fog", per.c_str()));
            clickAdds += fmt ("%s%.0f kHz %s %+.2f", clickAdds.empty() ? "" : ", ", sr / 1000.0, all ? "all four" : "Tub+Fog", mean);
        }
    }

    // Silence and denormal-level input should cost no more than signal (SPEC section 6).
    {
        const double sr = 48000.0;
        const std::size_t n = S (warm + timed, sr);
        const Audio sig = peaksSignal (sr, warm + timed, 2, { 151.0, 247.0 }, -12.0, 122);
        Audio silence = Audio::make (sr, 2, n);
        Audio denorm = Audio::make (sr, 2, n);
        Rng rng (123);
        for (auto& ch : denorm.ch)
            for (auto& v : ch)
                v = (float) (1e-30 * rng.gauss());
        double pct[3] = {};
        const Audio* ins[3] = { &sig, &silence, &denorm };
        for (int k = 0; k < 3; ++k)
        {
            RenderSpec s;
            s.params.thresholdDb = kAlwaysDb;
            s.blockSize = 128;
            s.timeBlocks = true;
            s.compensateLatency = false;
            const auto r = render (*ins[k], s);
            double sum = 0.0;
            for (std::size_t i = 0; i < r.blockSeconds.size(); ++i)
                if (r.blockStarts[i] >= (std::int64_t) S (warm, sr))
                    sum += r.blockSeconds[i];
            pct[k] = 100.0 * sum / timed;
        }
        const bool ok = pct[1] <= 1.25 * pct[0] + 0.01 && pct[2] <= 1.25 * pct[0] + 0.01;
        allOk = allOk && ok;
        t.check (ok, fmt ("48 kHz stereo block 128: signal %.3f%%, silence %.3f%%, denormal-level noise %.3f%% (limit: silence and denormals no more than signal + 25%% timing margin + 0.01%%)",
                          pct[0], pct[1], pct[2]));
    }
    t.info ("timed with std::chrono::steady_clock on this machine, single thread, editor-free core; SPEC targets are for an M1");
    if (! t.strictCpu)
        t.info ("the worst-block limit (2x average) is reported but checked only with --strict-cpu, because one interruption by the operating system breaks it");
    t.headline = head + "; Snap adds on average (points): " + clickAdds + (allOk ? " - meets the checked targets" : " - misses a target")
                 + (t.strictCpu ? " (strict)" : " (worst block not checked)");
}

//==============================================================================
// no_allocation_in_process

void testNoAllocation (Ctx& t)
{
    // Self-check: the counters do see an allocation made here.
    {
        const long long before = gAllocations.load();
        auto* probe = new std::vector<float> (16);
        const long long seen = gAllocations.load() - before;
        delete probe;
        t.check (seen >= 1, fmt ("counter self-check: a new std::vector<float> (16) counted %lld allocations (need at least 1)", seen));
    }

    long long totalAlloc = 0, totalFree = 0, calls = 0;
    for (double sr : { 48000.0, 96000.0 })
        for (int nch : { 1, 2, 6, 12 })
        {
            const int maxBlock = 1024;
            HonkShape shape;
            shape.bump = true;
            const Audio sig = honkSignal (sr, 3.0, nch, shape, -18.0, 131);
            std::vector<std::vector<float>> buf ((std::size_t) nch, std::vector<float> ((std::size_t) maxBlock));
            float* ptrs[dehonk::kMaxChannels] = {};
            for (int c = 0; c < nch; ++c)
                ptrs[c] = buf[(std::size_t) c].data();

            dehonk::Core core;
            if (nch > 2)
                core.prepare (sr, maxBlock, nch, dehonk::ChannelLayout::withLfe (3));   // 5.1 and 7.1.4: LFE at 3
            else
                core.prepare (sr, maxBlock, nch);
            dehonk::Tuning tuning = core.tuning();
            Rng rng (132 + (std::uint64_t) nch);
            dehonk::Params p;
            p.depth = 1.0f;
            const int sizes[] = { 1, 7, 64, 512, maxBlock };
            long long alloc = 0, freed = 0, n = 0;
            std::size_t pos = 0, block = 0;
            const float las[] = { 0.0f, 5.0f, 10.0f, 20.0f };
            while (pos < sig.length())
            {
                const int bs = (int) std::min<std::size_t> ((std::size_t) sizes[block % 5], sig.length() - pos);
                for (int c = 0; c < nch; ++c)
                    std::copy_n (sig.ch[(std::size_t) c].begin() + (std::ptrdiff_t) pos, bs, buf[(std::size_t) c].begin());
                // Parameter changes every block (lookahead every 50 blocks), a tuning change every
                // 100 and a reset() every 400, all counted along with process().
                p = randomParams (rng, p, block % 50 == 0);
                p.lookaheadMs = las[(block / 50) % 4];
                if (block % 100 == 0)
                {
                    tuning.parentRule = tuning.parentRule > 0.5f ? 0.0f : 1.0f;
                    tuning.clickLowHz = tuning.clickLowHz > 1900.0f ? 1800.0f : 2000.0f;   // Snap's filters are rebuilt
                }
                const long long a0 = gAllocations.load(), f0 = gDeallocations.load();
                core.setParams (p);
                if (block % 100 == 0)
                    core.setTuning (tuning);
                if (block % 400 == 399)
                    core.reset();
                // Every other stretch of 64 blocks with a sidechain (the first channel of the signal,
                // later in time), so the dialogue's analysis starts, runs and stops inside the count.
                const float* keyPtr = sig.ch[0].data() + (pos + 4096 < sig.length() ? pos + 4096 - (std::size_t) bs : 0);
                if ((block / 64) % 2 == 0)
                    core.processWithKey (ptrs, nch, &keyPtr, 1, bs);
                else
                    core.process (ptrs, nch, bs);
                (void) core.displayFrame();
                (void) core.latencySamples();
                alloc += gAllocations.load() - a0;
                freed += gDeallocations.load() - f0;
                ++n;
                pos += (std::size_t) bs;
                ++block;
            }
            totalAlloc += alloc;
            totalFree += freed;
            calls += n;
            t.check (alloc == 0 && freed == 0, fmt ("%.0f kHz %s, %lld blocks of 1 to %d samples, every parameter changing (Snap, the link mode, the dialogue sidechain and each range's timing included), with and without a key, lookahead, tuning and reset() included: %lld allocations, %lld frees (limit 0)",
                                                    sr / 1000.0, nch == 1 ? "mono" : nch == 2 ? "stereo" : nch == 6 ? "6 channels (LFE at 3)" : "12 channels (LFE at 3)",
                                                    n, maxBlock, alloc, freed));
        }
    t.info ("counts operator new and delete in every form; an allocation made with malloc directly would not be seen");
    t.headline = fmt ("%lld allocations, %lld frees in %lld audio-thread calls", totalAlloc, totalFree, calls);
}

//==============================================================================
// Surround: layouts up to 7.1.4 (12 channels), an LFE passed through, Link across N channels,
// and the programme level taken on a stereo fold-down.

/** A channel layout from speaker names, in the order given: L and R are the fronts, C the centre,
    LFE the LFE, any other name starting with L or R a speaker on that side, anything else on the
    centre line. */
dehonk::ChannelLayout layoutOf (const std::vector<std::string>& names)
{
    dehonk::ChannelLayout l;
    for (std::size_t i = 0; i < names.size() && i < (std::size_t) dehonk::kMaxChannels; ++i)
    {
        const auto& s = names[i];
        auto& p = l.position[i];
        if (s == "L") p = dehonk::Position::leftFront;
        else if (s == "R") p = dehonk::Position::rightFront;
        else if (s == "C") p = dehonk::Position::centre;
        else if (s == "LFE") p = dehonk::Position::lfe;
        else if (s[0] == 'L') p = dehonk::Position::leftOther;
        else if (s[0] == 'R') p = dehonk::Position::rightOther;
        else p = dehonk::Position::middleOther;
        // Overhead speakers (Lts, Ltf, Rtr, ...) are the heights, for Link's Grouped mode.
        if (s.size() > 2 && (s[0] == 'L' || s[0] == 'R') && s[1] == 't')
            l.group[i] = dehonk::Group::height;
    }
    return l;
}

struct SurroundLayout
{
    const char* name;
    std::vector<std::string> speakers;
    int lfe() const
    {
        for (std::size_t i = 0; i < speakers.size(); ++i)
            if (speakers[i] == "LFE")
                return (int) i;
        return -1;
    }
    int size() const { return (int) speakers.size(); }
    dehonk::ChannelLayout layout() const { return layoutOf (speakers); }
};

/** One layout for every channel count from 1 to 12, in the order JUCE hands them to a plug-in. */
const std::vector<SurroundLayout>& surroundLayouts()
{
    static const std::vector<SurroundLayout> l = {
        { "mono", { "C" } },
        { "stereo", { "L", "R" } },
        { "LCR", { "L", "R", "C" } },
        { "quad", { "L", "R", "Ls", "Rs" } },
        { "5.0", { "L", "R", "C", "Ls", "Rs" } },
        { "5.1", { "L", "R", "C", "LFE", "Ls", "Rs" } },
        { "7.0", { "L", "R", "C", "Lss", "Rss", "Lrs", "Rrs" } },
        { "7.1", { "L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs" } },
        { "7.0.2", { "L", "R", "C", "Lss", "Rss", "Lts", "Rts", "Lrs", "Rrs" } },
        { "7.1.2", { "L", "R", "C", "LFE", "Lss", "Rss", "Lts", "Rts", "Lrs", "Rrs" } },
        { "7.0.4", { "L", "R", "C", "Lss", "Rss", "Ltf", "Rtf", "Ltr", "Rtr", "Lrs", "Rrs" } },
        { "7.1.4", { "L", "R", "C", "LFE", "Lss", "Rss", "Ltf", "Rtf", "Ltr", "Rtr", "Lrs", "Rrs" } },
    };
    return l;
}

const SurroundLayout& layoutNamed (const char* name)
{
    for (const auto& l : surroundLayouts())
        if (std::string (l.name) == name)
            return l;
    return surroundLayouts().back();
}

/** The same one channel copied to n channels. */
Audio copies (const std::vector<float>& x, double sr, int n)
{
    Audio a;
    a.sampleRate = sr;
    a.ch.assign ((std::size_t) n, x);
    return a;
}

Audio channelOf (const Audio& a, int c) { return fromChannels (a.sampleRate, { a.ch[(std::size_t) c] }); }

/** Samples that differ, bit for bit, between channel ca of a and channel cb of b. */
std::size_t samplesDiffering (const Audio& a, int ca, const Audio& b, int cb)
{
    const auto& x = a.ch[(std::size_t) ca];
    const auto& y = b.ch[(std::size_t) cb];
    std::size_t d = x.size() == y.size() ? 0 : std::max (x.size(), y.size());
    for (std::size_t i = 0; i < std::min (x.size(), y.size()); ++i)
        d += std::memcmp (&x[i], &y[i], sizeof (float)) != 0 ? 1 : 0;
    return d;
}

/** The test material for most surround tests: peaks at 151 and 247 Hz (Tub and Fog) over the
    bass-heavy bed, different noise on every channel, plus 2.5 kHz clicks for Snap. */
Audio surroundSignal (double sr, double seconds, int nch, std::uint64_t seed, double rmsDbFs = -14.0)
{
    Audio a = peaksSignal (sr, seconds, nch, { 151.0, 247.0 }, rmsDbFs, seed);
    const auto bursts = clickSignal (sr, 2500.0, 3.0, nch, seconds, seed + 1);
    for (int c = 0; c < nch; ++c)
        for (std::size_t i = 0; i < a.length(); ++i)
            a.ch[(std::size_t) c][i] += 0.5f * bursts.in.ch[(std::size_t) c][i];
    return a;
}

RenderSpec surroundSpec (const dehonk::Params& p, const SurroundLayout& l)
{
    RenderSpec s;
    s.params = p;
    s.useLayout = true;
    s.layout = l.layout();
    return s;
}

//==============================================================================
// Max Cut, Release and Impact Guard

/** The layout a test uses for n channels: mono, stereo, 5.1 or 7.1.4. */
RenderSpec specForChannels (const dehonk::Params& p, int nch)
{
    if (nch <= 2)
    {
        RenderSpec s;
        s.params = p;
        return s;
    }
    return surroundSpec (p, layoutNamed (nch == 6 ? "5.1" : "7.1.4"));
}

const char* channelsName (int nch) { return nch == 1 ? "mono" : nch == 2 ? "stereo" : nch == 6 ? "5.1" : "7.1.4"; }

/** A tone over quiet pink noise, the same in every channel: something for one notch to sit on. */
Audio toneOverNoise (double seconds, int nch, double hz, double toneDb, double noiseDb, std::uint64_t seed, double onSec = 0.0, double offSec = 1.0e9)
{
    const std::size_t n = S (seconds);
    auto x = pinkNoise (kSr, n, seed);
    normaliseRms (x, noiseDb);
    const auto tone = sine (kSr, n, hz, dbToAmp (toneDb));
    const double fade = 0.010;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / kSr;
        const double g = std::clamp ((t - onSec) / fade, 0.0, 1.0) * std::clamp ((offSec - t) / fade, 0.0, 1.0);
        x[i] += (float) (g * (double) tone[i]);
    }
    return copies (x, kSr, nch);
}

/** The tuning the three tests below use: the parent rule off, so that a lone tone gets its notch. */
dehonk::Tuning noParentTuning()
{
    dehonk::Tuning t = defaultTuning();
    t.parentRule = 0.0f;
    return t;
}

void testMaxCut (Ctx& t)
{
    // 1. A dense signal that asks for far more than any ceiling, Depth 100%, every module on,
    // always acting. No band of any frame may be over Max Cut, the ceiling must be reached (or it
    // was never tested), and two notches closer than the spacing must not add up to more.
    const Audio dense = denseSignal (8.0, 611);
    const int spacingBands = std::max (1, (int) std::lround (defaultTuning().notchSpacingOctaves * dehonk::kBandsPerOctave));
    double worstOver = -99.0, worstPairOver = -99.0;
    std::string head;
    for (int nch : { 1, 2, 6, 12 })
        for (float cap : { 3.0f, 6.0f, 9.0f, 12.0f })
            for (float link : { 1.0f, 0.0f })
            {
                if (link < 0.5f && (nch == 1 || cap != 6.0f))
                    continue;
                dehonk::Params p = alwaysOn();
                p.depth = 1.0f;
                p.maxCutDb = cap;
                p.link = link;
                for (auto& on : p.moduleOn)
                    on = true;
                RenderSpec s = specForChannels (p, nch);
                s.collectFrames = true;
                s.frameEveryTick = true;
                Audio in = copies (dense.ch[0], kSr, nch);
                if (link < 0.5f)   // unlinked: give every channel its own level, so the channels differ
                    for (int c = 0; c < nch; ++c)
                        applyGain (in.ch[(std::size_t) c], dbToAmp (-1.5 * c));
                const auto r = render (in, s);
                double deepest = 0.0, pair = 0.0;
                for (const auto& f : r.frames)
                    for (int k = 0; k < r.numBands; ++k)
                    {
                        deepest = std::max (deepest, (double) f.cutDb[(std::size_t) k]);
                        for (int j = k + 1; j < std::min (r.numBands, k + spacingBands); ++j)
                            pair = std::max (pair, (double) f.cutDb[(std::size_t) k] + (double) f.cutDb[(std::size_t) j]);
                    }
                worstOver = std::max (worstOver, deepest - (double) cap);
                worstPairOver = std::max (worstPairOver, pair - (double) cap);
                // Unlinked, the frame shows the deeper channel band by band, so a pair may be from two
                // channels: only the single-band ceiling is checked there.
                t.check (deepest <= (double) cap + 1.0e-3 && deepest >= (double) cap - 0.3 && (link < 0.5f || pair <= (double) cap + 0.25),
                         fmt ("%s, Link %.0f%%, Max Cut %.0f dB: deepest band of any frame %.3f dB (never over, and within 0.3 dB of it); two notches closer than the spacing add to %.2f dB at most (limit Max Cut + 0.25%s)",
                              channelsName (nch), 100.0 * (double) link, (double) cap, deepest, pair, link < 0.5f ? ", not checked unlinked" : ""));
            }

    // 2. What it does to the sound: a 247 Hz tone (the centre of a band) 20 dB over quiet noise,
    // Depth 100%. Its level must fall by Max Cut, no more.
    for (float cap : { 3.0f, 6.0f, 12.0f })
    {
        dehonk::Params p = alwaysOn();
        p.depth = 1.0f;
        p.maxCutDb = cap;
        RenderSpec s;
        s.params = p;
        s.useTuning = true;
        s.tuning = noParentTuning();
        const Audio in = toneOverNoise (8.0, 1, 246.94, -12.0, -36.0, 612);
        const auto r = render (in, s);
        const double change = powerSpectrum (r.out, S (4.0), S (8.0)).centredDb (246.94, 1.0 / 24.0) - powerSpectrum (in, S (4.0), S (8.0)).centredDb (246.94, 1.0 / 24.0);
        t.check (change >= -(double) cap - 0.3 && change <= -(double) cap + 1.0,
                 fmt ("a steady 247 Hz tone, Depth 100%%, Max Cut %.0f dB: its level changes by %+.2f dB (must be between %.1f and %.1f)", (double) cap, change,
                      -(double) cap - 0.3, -(double) cap + 1.0));
        head += fmt ("%stone at Max Cut %.0f: %+.2f dB", head.empty() ? "" : ", ", (double) cap, change);
    }

    // 3. Turned down while a notch is open: the notch follows within 200 ms and is never deeper
    // than the higher of the two settings. And values that are not a setting.
    {
        dehonk::Params p = alwaysOn();
        p.depth = 1.0f;
        p.maxCutDb = 12.0f;
        RenderSpec s;
        s.params = p;
        s.useTuning = true;
        s.tuning = noParentTuning();
        s.collectFrames = true;
        s.frameEveryTick = true;
        s.blockSize = 32;
        s.automate = [] (std::int64_t pos, dehonk::Params& q) { q.maxCutDb = (double) pos / kSr >= 4.0 ? 3.0f : 12.0f; };
        const auto r = render (toneOverNoise (8.0, 2, 246.94, -12.0, -36.0, 613), s);
        double before = 0.0, after = 0.0, ever = 0.0;
        for (const auto& f : r.frames)
        {
            ever = std::max (ever, (double) f.totalCutDb);
            if (f.timeSec >= 3.0 && f.timeSec < 4.0)
                before = std::max (before, (double) f.totalCutDb);
            if (f.timeSec >= 4.2)
                after = std::max (after, (double) f.totalCutDb);
        }
        t.check (before > 11.0 && after <= 3.0 + 1.0e-3 && ever <= 12.0 + 1.0e-3,
                 fmt ("Max Cut turned from 12 to 3 dB at 4 s: deepest cut %.2f dB before, %.3f dB from 200 ms after (limit 3), %.3f dB ever (limit 12)", before, after, ever));

        struct Odd
        {
            const char* name;
            float value;
            double limit;
        };
        const Odd odd[] = { { "0 dB", 0.0f, 0.0 },
                            { "not a number (taken as the default)", std::numeric_limits<float>::quiet_NaN(), (double) dehonk::Params {}.maxCutDb },
                            { "100 dB (held to 18)", 100.0f, 18.0 } };
        for (const auto& o : odd)
        {
            RenderSpec so;
            so.params = p;
            so.params.maxCutDb = o.value;
            so.useTuning = true;
            so.tuning = noParentTuning();
            so.collectFrames = true;
            const auto ro = render (toneOverNoise (4.0, 1, 246.94, -12.0, -36.0, 614), so);
            const double deepest = cutStats (ro, 0.0).maxTotal;
            bool finite = true;
            for (float v : ro.out.ch[0])
                finite = finite && std::isfinite (v);
            t.check (deepest <= o.limit + 1.0e-3 && finite, fmt ("Max Cut set to %s: deepest cut %.3f dB (limit %.1f), output finite: %s", o.name, deepest, o.limit, finite ? "yes" : "no"));
        }
    }
    t.headline = fmt ("deepest band over the ceiling by %+.3f dB at most (limit +0.001), two within the spacing by %+.2f dB (limit +0.25); %s", worstOver, worstPairOver, head.c_str());
}

void testReleaseTime (Ctx& t)
{
    // A 247 Hz tone, 20 dB over quiet noise, on from the start to 4 s. When it stops, the notch on
    // it lets go. The rate the cut falls at, once nothing holds it, is the Release: the steepest
    // fall from one detector tick to the next gives the time constant, exactly. (The number of
    // notches a module may have is raised for this test: a notch that is letting go is otherwise
    // hurried out when the noise left behind earns notches of its own and the module is full,
    // which is the count rule, not the release.) The time the notch takes to come in when the
    // tone starts must not depend on Release, and must still follow Speed.
    // Impact Guard is off for these: everything starting from nothing at once is an entry to the
    // guard, which holds new depth back for a moment, and that is not what is measured here (it
    // is measured at the end).
    const double onSec = 0.0, offSec = 4.0;
    struct Got
    {
        double tauMs = 0.0, letGoMs = 0.0, attackMs = 0.0, settled = 0.0;
        double halfMs = -1.0;   // from the tone stopping to the notch at three quarters of its depth
    };
    auto measure = [&] (float releaseMs, float speed, int nch, float guard = 0.0f) {
        dehonk::Params p = alwaysOn();
        p.depth = 1.0f;
        p.releaseMs = releaseMs;
        p.speed = speed;
        p.impactGuard = guard;
        RenderSpec s = specForChannels (p, nch);
        s.useTuning = true;
        s.tuning = noParentTuning();
        s.tuning.notchesPerModule = 8.0f;
        s.collectFrames = true;
        s.frameEveryTick = true;
        s.blockSize = 64;
        const auto r = render (toneOverNoise (10.0, nch, 246.94, -12.0, -36.0, 621, onSec, offSec), s);
        const auto band = (std::size_t) nearestBand (r.bandHz, 246.94);
        Got g;
        double ratio = 1.0, period = 0.0;
        std::size_t counted = 0;
        for (std::size_t f = 1; f < r.frames.size(); ++f)
        {
            const double a = r.frames[f - 1].cutDb[band], b = r.frames[f].cutDb[band], tf = r.frames[f].timeSec;
            if (r.frames[f].counter == r.frames[f - 1].counter + 1)
            {
                period += tf - r.frames[f - 1].timeSec;
                ++counted;
                if (tf > offSec && a > 0.2 && b > 0.0 && b < a)
                    ratio = std::min (ratio, b / a);
            }
            if (tf >= offSec - 0.6 && tf < offSec - 0.1)
                g.settled = std::max (g.settled, b);
        }
        period /= (double) std::max<std::size_t> (1, counted);
        g.tauMs = ratio < 1.0 ? -1000.0 * period / std::log (ratio) : 0.0;
        for (const auto& f : r.frames)
        {
            if (g.attackMs == 0.0 && f.timeSec > onSec && (double) f.cutDb[band] >= 0.5 * g.settled && g.settled > 0.0)
                g.attackMs = 1000.0 * (f.timeSec - onSec);
            if (f.timeSec > offSec && (double) f.cutDb[band] >= 0.5)
                g.letGoMs = 1000.0 * (f.timeSec - offSec);
            if (g.halfMs < 0.0 && f.timeSec > offSec && (double) f.cutDb[band] <= 0.75 * g.settled)
                g.halfMs = 1000.0 * (f.timeSec - offSec);
        }
        return g;
    };

    std::string head;
    double worstErr = 0.0, lastLetGo = 0.0;
    std::vector<double> attacks;
    for (float rel : { 50.0f, 150.0f, 400.0f, 1000.0f, 1500.0f })
        for (int nch : { 1, 2, 6, 12 })
        {
            if (nch != 1 && rel != 400.0f && rel != 1500.0f)
                continue;
            const Got g = measure (rel, dehonk::Params {}.speed, nch);
            const double err = 100.0 * (g.tauMs - (double) rel) / (double) rel;
            worstErr = std::abs (err) > std::abs (worstErr) ? err : worstErr;
            t.check (std::abs (err) < 2.0 && g.settled > 3.0,
                     fmt ("%s, Release %.0f ms: the cut falls with a time constant of %.1f ms (%+.2f%%, limit 2%%); a %.1f dB notch is under 0.5 dB %.0f ms after the tone stops; it came in (half depth) %.0f ms after the tone started",
                          channelsName (nch), (double) rel, g.tauMs, err, g.settled, g.letGoMs, g.attackMs));
            // The whole time, not only the rate. The release starts once the tone has gone from
            // its analysis band, which is narrow and rings on: a tone 24 dB over the noise takes
            // 100 to 200 ms to fall to where it no longer asks for the full cut. From there the
            // notch is at three quarters of its depth 0.29 of the Release later. (Three quarters
            // and not half: by half depth, at this Depth, the noise left behind asks for cuts of
            // its own now and then, and that is not the release.)
            const double expect = 0.2877 * (double) rel;
            t.check (g.halfMs >= expect && g.halfMs <= expect + 200.0 + 0.05 * (double) rel,
                     fmt ("%s, Release %.0f ms: the notch is down to three quarters of its depth %.0f ms after the tone stops (0.29 x Release = %.0f ms, plus up to 200 ms for the tone to leave its band, plus 5%%)", channelsName (nch),
                          (double) rel, g.halfMs, expect));
            if (nch == 1)
            {
                t.check (g.letGoMs > lastLetGo, fmt ("Release %.0f ms: the notch takes longer to let go than at the setting before (%.0f ms against %.0f)", (double) rel, g.letGoMs, lastLetGo));
                lastLetGo = g.letGoMs;
                attacks.push_back (g.attackMs);
                head += fmt ("%s%.0f: %.1f", head.empty() ? "" : ", ", (double) rel, g.tauMs);
            }
        }
    const double aLo = *std::min_element (attacks.begin(), attacks.end()), aHi = *std::max_element (attacks.begin(), attacks.end());
    t.check (aHi - aLo <= 0.05 * aHi + 2.0, fmt ("the time the notch takes to come in does not depend on Release: %.0f to %.0f ms across Release 50 to 1500 ms (limit 5%% + 2 ms)", aLo, aHi));

    // Speed sets how fast it comes in, and nothing about how it lets go.
    const Got slow = measure (400.0f, 0.0f, 1), fast = measure (400.0f, 1.0f, 1);
    // Part of that time is what the detector needs to see that the tone stands out at all; Speed
    // sets the attack that follows, 40 ms at 0% and 5 ms at 100%.
    t.check (fast.attackMs < slow.attackMs - 10.0, fmt ("Speed still sets the attack: half depth after %.0f ms at Speed 0%%, %.0f ms at 100%% (need at least 10 ms quicker)", slow.attackMs, fast.attackMs));
    t.check (std::abs (slow.tauMs - 400.0) < 8.0 && std::abs (fast.tauMs - 400.0) < 8.0,
             fmt ("Speed does not touch the release: Release 400 ms measures %.1f ms at Speed 0%% and %.1f ms at Speed 100%% (limit 2%%)", slow.tauMs, fast.tauMs));
    // What the guard costs at a start from silence, where everything comes up at once: it takes
    // that for an entry and holds new depth back until it has seen that nothing is dying away.
    const Got guarded = measure (400.0f, dehonk::Params {}.speed, 1, 1.0f), unguarded = measure (400.0f, dehonk::Params {}.speed, 1, 0.0f);
    t.check (guarded.attackMs <= unguarded.attackMs + 400.0,
             fmt ("starting from silence with Impact Guard at 100%%, the notch is at half depth after %.0f ms, against %.0f ms with it off (the start is an entry to the guard: it waits guardConfirmMs to see whether it dies away, then lets go; limit 400 ms later)",
                  guarded.attackMs, unguarded.attackMs));

    // Under the Threshold the cut closes by itself, whatever Release is. The same gated tone with
    // the Threshold 9 dB under the programme level with the tone on; the noise left behind is
    // 12 dB under it. Every notch must be shut (under 0.5 dB) in the same, short time at Release
    // 50, 400 and 1500 ms.
    std::vector<double> shut;
    for (float rel : { 50.0f, 400.0f, 1500.0f })
        for (int nch : { 1, 2, 6, 12 })
        {
            if (nch != 1 && rel != 1500.0f)
                continue;
            dehonk::Params p;
            p.depth = 1.0f;
            p.releaseMs = rel;
            RenderSpec s = specForChannels (p, nch);
            s.useTuning = true;
            s.tuning = noParentTuning();
            s.collectFrames = true;
            s.frameEveryTick = true;
            s.blockSize = 64;
            const Audio in = toneOverNoise (10.0, nch, 246.94, -12.0, -36.0, 621, onSec, offSec);
            // The Threshold 9 dB under the programme level with the tone on, as this layout reads
            // it (the same signal in every channel adds up in the fold-down).
            {
                const auto level = render (in, s);
                double prog = -400.0;
                for (const auto& f : level.frames)
                    if (f.timeSec >= offSec - 0.6 && f.timeSec < offSec - 0.1)
                        prog = std::max (prog, (double) f.programDb);
                s.params.thresholdDb = (float) (prog - 9.0);
            }
            const auto r = render (in, s);
            double open = 0.0, lastOpen = offSec, actAtOff = 0.0;
            for (const auto& f : r.frames)
            {
                if (f.timeSec >= offSec - 0.6 && f.timeSec < offSec - 0.1)
                    open = std::max (open, (double) f.totalCutDb);
                if (f.timeSec < offSec)
                    actAtOff = f.activity;
                if (f.timeSec > offSec && f.totalCutDb >= 0.5f)
                    lastOpen = f.timeSec;
            }
            const double ms = 1000.0 * (lastOpen - offSec);
            if (nch == 1)
                shut.push_back (ms);
            t.check (open > 3.0 && actAtOff > 0.99 && ms < 500.0,
                     fmt ("%s, Release %.0f ms, Threshold %.1f dBFS (9 dB under the tone, 12 dB over the noise): a %.2f dB notch (activity %.2f) is under 0.5 dB for good %.0f ms after the tone stops (limit 500, whatever the Release)",
                          channelsName (nch), (double) rel, (double) s.params.thresholdDb, open, actAtOff, ms));
        }
    const double sLo = *std::min_element (shut.begin(), shut.end()), sHi = *std::max_element (shut.begin(), shut.end());
    t.check (sHi - sLo <= 120.0, fmt ("that time hardly depends on Release: %.0f to %.0f ms across Release 50, 400 and 1500 ms (limit 120 ms apart; at 50 ms the notch is also letting go by itself)", sLo, sHi));

    t.headline = fmt ("measured time constant by setting (ms): %s; worst error %+.2f%% (limit 2); attack %.0f-%.0f ms whatever the Release, %.0f ms at Speed 0%%, %.0f ms at 100%%; under the Threshold shut in %.0f-%.0f ms whatever the Release", head.c_str(),
                      worstErr, aLo, aHi, slow.attackMs, fast.attackMs, sLo, sHi);
}

/** What happens at eventSec, over the bed of toneOverNoise (a 247 Hz tone with a notch on it). */
enum class GuardEvent
{
    none,
    burst,        // noise, flat to 1 kHz and falling 12 dB an octave above, 150 ms decay, -9 dBFS RMS at its start: an impact
    burstAndTone, // the burst, and a 330 Hz tone at -10 dBFS that starts with it and stays: something new to cut, under an impact
    toneOnly,     // that 330 Hz tone without the burst
    tone,         // a 500 Hz tone at -6 dBFS switched on (2 ms) and left on: a narrow-band onset
    narrowBurst,  // a third-octave band of noise around 500 Hz with the burst's envelope and level
    wall,         // the same noise at -12 dBFS RMS switched on (2 ms) and left on: an entry, not an impact
    drums,        // a burst every 250 ms from eventSec on, over pink noise at -18 dBFS: a busy passage
    slowHits,     // a burst every 1.1 s from eventSec on
    spikeThenBurst // one sample of 1e6, 1.5 s before the burst
};

Audio guardSignal (double seconds, int nch, GuardEvent ev, double eventSec, std::uint64_t seed)
{
    Audio a = toneOverNoise (seconds, 1, 246.94, -14.0, -36.0, seed);
    auto& x = a.ch[0];
    const std::size_t n = x.size(), at = S (eventSec);
    Rng rng (seed + 5);
    // Noise with its energy where an impact has it: flat to 1 kHz, falling 12 dB an octave above. RMS 1.
    auto body = shapedNoise (kSr, n, seed + 6, [] (double hz) { return hz <= 1000.0 ? 0.0 : -12.0 * std::log2 (hz / 1000.0); });
    normaliseRms (body, 0.0);
    auto addBurst = [&] (std::size_t start, double rmsDb) {
        for (std::size_t i = start; i < std::min (n, start + S (1.0)); ++i)
        {
            const double tt = (double) (i - start) / kSr;
            x[i] += (float) (dbToAmp (rmsDb) * (double) body[i] * std::min (1.0, tt / 0.001) * std::exp (-tt / 0.150));
        }
    };
    auto addTone = [&] (double hz, double db) {
        const auto tone = sine (kSr, n, hz, dbToAmp (db));
        for (std::size_t i = at; i < n; ++i)
            x[i] += (float) (std::min (1.0, (double) (i - at) / (0.002 * kSr)) * (double) tone[i]);
    };
    switch (ev)
    {
        case GuardEvent::none: break;
        case GuardEvent::burst: addBurst (at, -9.0); break;
        case GuardEvent::burstAndTone: addBurst (at, -9.0); addTone (329.63, -10.0); break;
        case GuardEvent::toneOnly: addTone (329.63, -10.0); break;
        case GuardEvent::spikeThenBurst:
            addBurst (at, -9.0);
            x[at - S (1.5)] = 1.0e6f;
            break;
        case GuardEvent::tone: addTone (500.0, -6.0); break;
        case GuardEvent::narrowBurst:
        {
            std::vector<float> noise (n);
            for (auto& v : noise)
                v = (float) rng.gauss();
            const auto band = bandpass8 (noise, kSr, 500.0 / std::exp2 (1.0 / 6.0), 500.0 * std::exp2 (1.0 / 6.0));
            double mx = 0.0;
            for (double v : band)
                mx = std::max (mx, std::abs (v));
            for (std::size_t i = at; i < std::min (n, at + S (1.0)); ++i)
            {
                const double tt = (double) (i - at) / kSr;
                x[i] += (float) (dbToAmp (-3.0) * band[i] / mx * std::min (1.0, tt / 0.001) * std::exp (-tt / 0.150));   // peak -3 dBFS
            }
            break;
        }
        case GuardEvent::wall:
            for (std::size_t i = at; i < n; ++i)
                x[i] += (float) (dbToAmp (-12.0) * (double) body[i] * std::min (1.0, (double) (i - at) / (0.002 * kSr)));
            break;
        case GuardEvent::drums:
        case GuardEvent::slowHits:
        {
            if (ev == GuardEvent::drums)
            {
                auto bed = pinkNoise (kSr, n, seed + 9);
                normaliseRms (bed, -18.0);
                for (std::size_t i = at; i < n; ++i)
                    x[i] += bed[i];
            }
            const double every = ev == GuardEvent::drums ? 0.25 : 1.1;
            for (double tt = eventSec; tt < seconds - 0.1; tt += every)
                addBurst (S (tt), ev == GuardEvent::drums ? -12.0 : -9.0);
            break;
        }
    }
    return copies (x, kSr, nch);
}

void testImpactGuard (Ctx& t)
{
    const double eventSec = 3.0;
    enum class Tune { asIs, neverFire, holdLong };
    auto specFor = [&] (int nch, float amount, Tune tune) {
        dehonk::Params p = alwaysOn();
        p.depth = 1.0f;
        p.impactGuard = amount;
        RenderSpec s = specForChannels (p, nch);
        s.useTuning = true;
        s.tuning = noParentTuning();
        if (tune == Tune::neverFire)
            s.tuning.guardShare = 2.0f;   // more than every band: the detector can never fire
        if (tune == Tune::holdLong)
        {
            // Every landing made to hold for a second whatever follows it: only the limit on the
            // guard's share of the time can then bring it down.
            s.tuning.guardBackDb = 0.0f;
            s.tuning.guardDecayDb = 0.0f;
            s.tuning.guardHoldMs = 1000.0f;
        }
        s.collectFrames = true;
        s.frameEveryTick = true;
        s.blockSize = 64;
        return s;
    };
    auto run = [&] (GuardEvent ev, int nch, float amount, Tune tune = Tune::asIs, double len = 9.0) {
        return render (guardSignal (len, nch, ev, eventSec, 631), specFor (nch, amount, tune));
    };
    struct Seen
    {
        double guardBefore = 0.0, guardMax = 0.0, firstFullMs = -1.0, firstHalfMs = -1.0, upSec = 0.0;   // upSec: time with the guard over 0.5, from the event on
        double cutBefore = 0.0, cutEarly = 0.0, cutAfter = 0.0;   // deepest cut 2.5-3 s; highest 20-60 ms after; deepest 1.5-2.5 s after
        double upShareLate = 0.0;                                 // share of the time from 2 s after the event with the guard over 0.5
    };
    auto see = [&] (const RenderResult& r) {
        Seen v;
        std::size_t late = 0, lateUp = 0;
        double last = eventSec;
        for (const auto& f : r.frames)
        {
            const double tt = f.timeSec;
            if (tt >= 2.0 && tt < eventSec - 0.001)
                v.guardBefore = std::max (v.guardBefore, (double) f.guard);
            if (tt >= eventSec)
            {
                v.guardMax = std::max (v.guardMax, (double) f.guard);
                if (v.firstFullMs < 0.0 && f.guard >= 0.99f)
                    v.firstFullMs = 1000.0 * (tt - eventSec);
                if (v.firstHalfMs < 0.0 && f.guard >= 0.5f)
                    v.firstHalfMs = 1000.0 * (tt - eventSec);
                if (f.guard > 0.5f)
                    v.upSec += tt - last;
                last = tt;
            }
            if (tt >= eventSec - 0.5 && tt < eventSec)
                v.cutBefore = std::max (v.cutBefore, (double) f.totalCutDb);
            if (tt >= eventSec + 0.02 && tt < eventSec + 0.06)
                v.cutEarly = std::max (v.cutEarly, (double) f.totalCutDb);
            if (tt >= eventSec + 1.5 && tt < eventSec + 2.5)
                v.cutAfter = std::max (v.cutAfter, (double) f.totalCutDb);
            if (tt >= eventSec + 2.0)
            {
                ++late;
                lateUp += f.guard > 0.5f ? 1 : 0;
            }
        }
        v.upShareLate = late > 0 ? (double) lateUp / (double) late : 0.0;
        return v;
    };

    std::string head;
    for (int nch : { 1, 2, 6, 12 })
    {
        const char* cn = channelsName (nch);
        // A broadband burst: the guard comes up at once, the notch on the tone is eased off while
        // the burst stands over the tone, and it comes back as the burst dies away.
        const auto rb = run (GuardEvent::burst, nch, 1.0f);
        const Seen b = see (rb);
        t.check (b.guardBefore == 0.0 && b.cutBefore > 3.0,
                 fmt ("%s: before the burst the guard is down (%.3f) and a notch is open on the tone (%.2f dB, need 3)", cn, b.guardBefore, b.cutBefore));
        t.check (b.firstHalfMs >= 0.0 && b.firstHalfMs <= 15.0 && b.firstFullMs >= 0.0 && b.firstFullMs <= 25.0,
                 fmt ("%s, broadband burst: the guard is half up %.0f ms after it lands (limit 15) and fully up after %.0f ms (limit 25)", cn, b.firstHalfMs, b.firstFullMs));
        t.check (b.cutEarly < 0.5, fmt ("%s, broadband burst, Impact Guard 100%%: deepest cut from 20 to 60 ms after it lands %.2f dB (limit 0.5; %.2f dB before)", cn, b.cutEarly, b.cutBefore));
        t.check (b.upSec > 0.05 && b.upSec < 0.75, fmt ("%s, broadband burst: the guard is up for %.2f s (0.05 to 0.75: until the burst is back under the tone, at most the hold and the return)", cn, b.upSec));
        t.check (std::abs (b.cutAfter - b.cutBefore) < 1.0 && b.upShareLate == 0.0,
                 fmt ("%s, broadband burst: 1.5 s later the guard is down and the notch is back, %.2f dB against %.2f dB before (within 1 dB)", cn, b.cutAfter, b.cutBefore));
        if (nch == 1)
            head += fmt ("burst: half up in %.0f ms, fully in %.0f ms, cut %.2f -> %.2f dB, up %.2f s", b.firstHalfMs, b.firstFullMs, b.cutBefore, b.cutEarly, b.upSec);

        // At 50% the notch is eased by half.
        const Seen h = see (run (GuardEvent::burst, nch, 0.5f));
        t.check (h.guardMax > 0.49 && h.guardMax < 0.51 && h.cutEarly > 0.35 * b.cutBefore && h.cutEarly < 0.65 * b.cutBefore,
                 fmt ("%s, broadband burst, Impact Guard 50%%: the guard reads %.2f and the deepest cut 20 to 60 ms after it lands is %.2f dB, against %.2f dB before (35 to 65%%)", cn,
                      h.guardMax, h.cutEarly, b.cutBefore));

        // At 0% the guard does nothing at all: bit for bit what a core whose detector can never fire gives.
        const auto r0 = run (GuardEvent::burst, nch, 0.0f);
        const auto rn = run (GuardEvent::burst, nch, 1.0f, Tune::neverFire);
        std::size_t differ = 0;
        for (int c = 0; c < nch; ++c)
            differ += samplesDiffering (r0.out, c, rn.out, c);
        bool framesSame = r0.frames.size() == rn.frames.size();
        for (std::size_t f = 0; f < r0.frames.size() && framesSame; ++f)
            framesSame = r0.frames[f].cutDb == rn.frames[f].cutDb && r0.frames[f].guard == 0.0f && rn.frames[f].guard == 0.0f;
        t.check (differ == 0 && framesSame, fmt ("%s, Impact Guard 0%%: %zu output samples differ from a core whose guard can never fire, display frames %s", cn, differ,
                                                 framesSame ? "the same, guard at 0 throughout" : "differ"));

        // No new depth under an impact: a tone that starts with the burst gets no notch while
        // the guard is up. (On its own the same tone has one well before then, or this says nothing.)
        {
            const auto rg = run (GuardEvent::burstAndTone, nch, 1.0f), rt = run (GuardEvent::toneOnly, nch, 1.0f), ru = run (GuardEvent::burstAndTone, nch, 0.0f);
            const auto band = (std::size_t) nearestBand (rg.bandHz, 329.63);
            double downAt = -1.0, whileUp = 0.0, alone = 0.0, unguarded = 0.0, later = 0.0;
            for (const auto& f : rg.frames)
            {
                if (f.timeSec > eventSec + 0.03 && downAt < 0.0 && f.guard < 0.5f)
                    downAt = f.timeSec;
                if (f.timeSec >= eventSec && downAt < 0.0)
                    for (int d = -1; d <= 1; ++d)
                        whileUp = std::max (whileUp, (double) f.cutDb[band + (std::size_t) d]);
                if (f.timeSec >= eventSec + 2.0)
                    later = std::max (later, (double) f.cutDb[band]);
            }
            for (const auto& f : rt.frames)
                if (f.timeSec >= eventSec && f.timeSec < downAt)
                    alone = std::max (alone, (double) f.cutDb[band]);
            for (const auto& f : ru.frames)
                if (f.timeSec >= eventSec && f.timeSec < downAt)
                    for (int d = -1; d <= 1; ++d)
                        unguarded = std::max (unguarded, (double) f.cutDb[band + (std::size_t) d]);
            t.check (downAt > eventSec && whileUp < 0.3 && later > 3.0,
                     fmt ("%s, a 330 Hz tone starting with the burst: while the guard is over 0.5 (%.0f ms) the deepest cut within a band of it is %.2f dB (limit 0.3); 2 s later it has its notch (%.2f dB, need 3). With Impact Guard off the cut there reaches %.2f dB in that time, and the tone alone gets %.2f dB",
                          cn, 1000.0 * (downAt - eventSec), whileUp, later, unguarded, alone));
            if (nch == 1)
                head += fmt ("; new tone under the burst: %.2f dB while the guard is up (%.2f dB with it off)", whileUp, unguarded);
        }

        // What is not an impact.
        struct Not
        {
            GuardEvent ev;
            const char* name;
            double maxUpSec;   // the guard may be over 0.5 for at most this long after the event
        };
        const Not nots[] = {
            { GuardEvent::tone, "a 500 Hz tone switched on", 0.0 },
            { GuardEvent::narrowBurst, "a third-octave burst at 500 Hz", 0.0 },
            { GuardEvent::wall, "broadband noise switched on and left on (an entry)", 0.45 },
        };
        for (const auto& no : nots)
        {
            const Seen v = see (run (no.ev, nch, 1.0f));
            t.check (v.upSec <= no.maxUpSec + 1.0e-9 && v.upShareLate == 0.0 && (no.maxUpSec > 0.0 || v.guardMax == 0.0),
                     fmt ("%s, %s: the guard is over 0.5 for %.2f s (limit %.2f), highest reading %.2f, and down from 2 s after", cn, no.name, v.upSec, no.maxUpSec, v.guardMax));
            if (nch == 1)
                head += fmt ("; %s: %.2f s", no.ev == GuardEvent::tone ? "tone" : no.ev == GuardEvent::narrowBurst ? "narrow burst" : "entry", v.upSec);
        }

        // A busy passage: a burst every 250 ms over a noise bed, for 12 s. The guard may go up
        // on the hits, but never for more of the time than its limit, and the plug-in must go
        // on doing its job: the tone under the hits is still cut.
        {
            const double len = 15.0;
            const Audio in = guardSignal (len, nch, GuardEvent::drums, eventSec, 631);
            const auto rd = render (in, specFor (nch, 1.0f, Tune::asIs));
            const Seen v = see (rd);
            const double limit = (double) defaultTuning().guardDutyLimit;
            const double tone = powerSpectrum (rd.out, S (eventSec + 2.0), S (len)).centredDb (246.94, 1.0 / 24.0) - powerSpectrum (in, S (eventSec + 2.0), S (len)).centredDb (246.94, 1.0 / 24.0);
            t.check (v.upShareLate <= limit + 0.02 && tone < -4.0,
                     fmt ("%s, a burst every 250 ms over a bed (a busy passage): from 2 s in, the guard is over 0.5 for %.1f%% of the time (limit %.0f%%), and the 247 Hz tone under the hits is %+.2f dB (must still be cut by 4 dB or more)",
                          cn, 100.0 * v.upShareLate, 100.0 * limit, tone));
            if (nch == 1)
                head += fmt ("; busy passage: up %.1f%%, tone %+.1f dB", 100.0 * v.upShareLate, tone);
        }
        // Heavy hits that keep coming, 1.1 s apart. As it is, each is over when it has died
        // away (reported). Made to hold for a second after every landing, it would be up nearly
        // all the time: then the limit on the guard's share of the time (Tuning::guardDutyLimit)
        // must hold it back.
        {
            const Seen asIs = see (run (GuardEvent::slowHits, nch, 1.0f, Tune::asIs, 23.0));
            const Seen v = see (run (GuardEvent::slowHits, nch, 1.0f, Tune::holdLong, 23.0));
            const double limit = (double) defaultTuning().guardDutyLimit;
            t.info (fmt ("%s, a heavy hit every 1.1 s for 20 s: from 2 s in, the guard is over 0.5 for %.0f%% of the time", cn, 100.0 * asIs.upShareLate));
            t.check (v.upShareLate <= limit + 0.02 && v.upShareLate > limit - 0.12,
                     fmt ("%s, the same with every landing made to hold for a second: the guard is over 0.5 for %.0f%% of the time (limit %.0f%%, and within 12 points of it, or the limit was never reached)", cn,
                          100.0 * v.upShareLate, 100.0 * limit));
            if (nch == 1)
                head += fmt ("; a hit every 1.1 s: up %.0f%%, %.0f%% when made to hold a second each (limit %.0f)", 100.0 * asIs.upShareLate, 100.0 * v.upShareLate, 100.0 * limit);
        }
        // One absurd sample must not blind it for long: a sample of 1e6, 1.5 s before the burst.
        // (With no ceiling on what a band is taken to be, --set guardCeilingDb=200, this burst is missed.)
        {
            const Seen v = see (run (GuardEvent::spikeThenBurst, nch, 1.0f));
            t.check (v.guardMax >= 0.99 && v.firstHalfMs >= 0.0 && v.firstHalfMs <= 15.0,
                     fmt ("%s, one sample of 1e6 1.5 s before the burst: the guard still comes up on the burst (highest reading %.2f, half up after %.0f ms, limit 15)", cn, v.guardMax, v.firstHalfMs));
        }
    }

    // What it does to the sound, measured on the audio and not on the display. Stereo, linked:
    // the left channel is the signal, the right the signal less a 64th of the 247 Hz tone. Both
    // get the same notches, and the filters are linear for a given set of notches, so
    // (left - right) x 64 is the tone alone through the filters the signal got. Its level
    // against the tone's own says how deep the notch on it is in the audio, moment by moment.
    {
        constexpr double d = 1.0 / 64.0;
        const double len = 9.0;
        const Audio whole = guardSignal (len, 1, GuardEvent::burst, eventSec, 631);
        Audio toneOnly = toneOverNoise (len, 1, 246.94, -14.0, -36.0, 631);
        const Audio noiseOnly = toneOverNoise (len, 1, 246.94, -200.0, -36.0, 631);
        for (std::size_t i = 0; i < toneOnly.length(); ++i)
            toneOnly.ch[0][i] -= noiseOnly.ch[0][i];
        Audio pair = Audio::make (kSr, 2, whole.length());
        for (std::size_t i = 0; i < whole.length(); ++i)
        {
            pair.ch[0][i] = whole.ch[0][i];
            pair.ch[1][i] = (float) ((double) whole.ch[0][i] - d * (double) toneOnly.ch[0][i]);
        }
        auto toneGain = [&] (float amount, double from, double to) {
            RenderSpec s = specFor (2, amount, Tune::asIs);
            s.collectFrames = false;
            const auto r = render (pair, s);
            double in = 0.0, out = 0.0;
            for (std::size_t i = S (from); i < S (to); ++i)
            {
                const double o = ((double) r.out.ch[0][i] - (double) r.out.ch[1][i]) / d;
                in += (double) toneOnly.ch[0][i] * (double) toneOnly.ch[0][i];
                out += o * o;
            }
            return powerToDb (out / std::max (1e-300, in));
        };
        const double before = toneGain (1.0f, eventSec - 0.5, eventSec - 0.05);
        const double full = toneGain (1.0f, eventSec + 0.02, eventSec + 0.06), half = toneGain (0.5f, eventSec + 0.02, eventSec + 0.06);
        const double off = toneGain (0.0f, eventSec + 0.02, eventSec + 0.06), after = toneGain (1.0f, eventSec + 1.5, eventSec + 2.5);
        t.check (before < -5.0, fmt ("in the audio: before the burst the 247 Hz tone is %+.2f dB through the notch on it (need -5 or lower)", before));
        t.check (full > -0.5, fmt ("in the audio, Impact Guard 100%%: 20 to 60 ms after the burst lands the tone is %+.2f dB (limit -0.5: the notch is out of the impact)", full));
        t.check (half < 0.35 * before && half > 0.65 * before, fmt ("in the audio, Impact Guard 50%%: %+.2f dB (35 to 65%% of the %+.2f dB before)", half, before));
        t.check (off < 0.8 * before, fmt ("in the audio, Impact Guard off: %+.2f dB (the notch stays: at least 80%% of the %+.2f dB before)", off, before));
        t.check (std::abs (after - before) < 1.0, fmt ("in the audio: 1.5 s later the tone is %+.2f dB again (within 1 dB of before)", after));
        head += fmt ("; in the audio the tone is %+.2f dB before, %+.2f dB under the burst at 100%%, %+.2f at 50%%, %+.2f with the guard off, %+.2f after", before, full, half, off, after);
    }

    // Switching the guard while it is holding must not click: Impact Guard stepped between 0 and
    // 100% every 70 ms across a burst every 1.5 s, judged above 6 kHz, where the notches do nothing.
    {
        const std::size_t n = S (8.0);
        Audio in = guardSignal (8.0, 2, GuardEvent::none, 0.0, 641);
        for (double at = 1.0; at < 7.0; at += 1.5)
        {
            const Audio one = guardSignal (8.0, 1, GuardEvent::burst, at, 642);
            const Audio bedOnly = guardSignal (8.0, 1, GuardEvent::none, at, 642);
            for (std::size_t i = 0; i < n; ++i)
            {
                const float v = one.ch[0][i] - bedOnly.ch[0][i];
                in.ch[0][i] += v;
                in.ch[1][i] += v;
            }
        }
        double worst = -400.0, worstAt = 0.0, ownWorst = -400.0;
        bool wentUp = false;
        for (int mode = 0; mode < 2; ++mode)
        {
            dehonk::Params p = alwaysOn();
            p.depth = 1.0f;
            RenderSpec s;
            s.params = p;
            s.useTuning = true;
            s.tuning = noParentTuning();
            s.blockSize = 32;
            if (mode == 1)
                s.automate = [] (std::int64_t pos, dehonk::Params& q) { q.impactGuard = std::fmod ((double) pos / kSr, 0.14) < 0.07 ? 1.0f : 0.0f; };
            s.collectFrames = true;
            const auto r = render (in, s);
            for (const auto& f : r.frames)
                wentUp = wentUp || (mode == 0 && f.timeSec > 2.0 && f.guard > 0.99f);
            for (int c = 0; c < 2; ++c)
            {
                const auto [m, at] = hfUnexplained (highpass4 (in.ch[(std::size_t) c], kSr, 6000.0), highpass4 (r.out.ch[(std::size_t) c], kSr, 6000.0), S (0.5), S (0.010));
                if (mode == 1 && m > worst)
                {
                    worst = m;
                    worstAt = at;
                }
                if (mode == 0)
                    ownWorst = std::max (ownWorst, m);
                if (mode == 0)
                    t.check (m < -50.0, fmt ("the guard easing and returning the notch on its own, channel %d: unexplained energy above 6 kHz, worst 10 ms window %s dB at %.2f s (limit -50)", c,
                                             dbStr (m).c_str(), at));
            }
        }
        t.check (wentUp, "the guard did go up on those bursts (or switching it proved nothing)");
        // -50, not the -40 of automation_clicks: with the 15 ms smoothing of the amount taken out
        // this measures -40.7 dB, so -40 would not notice.
        t.check (worst < -50.0, fmt ("Impact Guard switched between 0 and 100%% every 70 ms across four bursts: unexplained energy above 6 kHz, worst 10 ms window %s dB at %.2f s (limit -50)",
                                     dbStr (worst).c_str(), worstAt));
        head += "; above 6 kHz, easing on its own " + dbStr (ownWorst) + " dB, switched every 70 ms " + dbStr (worst) + " dB (limit -50)";
    }
    t.headline = head;
}

//==============================================================================
// golden_mono_stereo

const char* goldenArch()
{
#if defined (__aarch64__) || defined (_M_ARM64)
    return "arm64";
#elif defined (__x86_64__) || defined (_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

using GoldenRef = std::vector<std::pair<std::string, std::string>>;

/** Tests/golden/<stem>_<arch>.txt, kept with the sources so a clean build checks it too. A
    missing file or a missing case fails: an unguarded bit-identity rule is worse than a red one. */
bool readGolden (Ctx& t, const char* stem, const char* whatItHolds, GoldenRef& ref, std::string& fileName)
{
    const auto path = std::filesystem::path (DEHONK_TESTS_SOURCE_DIR) / "golden" / (std::string (stem) + "_" + goldenArch() + ".txt");
    fileName = path.filename().string();
    std::ifstream f (path);
    if (! f)
    {
        t.check (false, "no " + path.string() + ": " + whatItHolds + " are missing for this architecture (Tests/CMakeLists.txt says how to record them)");
        return false;
    }
    for (std::string line; std::getline (f, line);)
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls (line);
        std::string name, hash;
        if (ls >> name >> hash)
            ref.emplace_back (name, hash);
    }
    t.info (fmt ("reference: %s, %d cases", path.string().c_str(), (int) ref.size()));
    return true;
}

struct GoldenTally
{
    int same = 0, differ = 0, missing = 0;
};

/** Every result must be in the reference with the same hash, and the reference must hold nothing else. */
GoldenTally compareGolden (Ctx& t, const std::vector<golden::Result>& now, const GoldenRef& ref, const std::string& fileName)
{
    GoldenTally g;
    for (const auto& r : now)
    {
        char hex[32];
        std::snprintf (hex, sizeof hex, "%016llx", (unsigned long long) r.hash);
        const auto it = std::find_if (ref.begin(), ref.end(), [&] (const auto& e) { return e.first == r.name; });
        if (it == ref.end())
        {
            ++g.missing;
            t.check (false, r.name + ": not in " + fileName);
        }
        else if (it->second == hex)
            ++g.same;
        else
        {
            ++g.differ;
            t.check (false, r.name + ": output or display frames differ from the reference (" + hex + " against " + it->second + ")");
        }
    }
    return g;
}

/** The plug-in always passes a layout: a mono track is a centre speaker, stereo is left and right
    fronts. That must give exactly what the three-argument prepare() gives. */
const auto pluginPrepare = [] (dehonk::Core& core, double sr, int maxBlock, int nch)
{
    dehonk::ChannelLayout l;
    if (nch == 1)
        l.position[0] = dehonk::Position::centre;
    else
    {
        l.position[0] = dehonk::Position::leftFront;
        l.position[1] = dehonk::Position::rightFront;
    }
    core.prepare (sr, maxBlock, nch, l);
};

void testGoldenMonoStereo (Ctx& t)
{
    // Max Cut, Release and Impact Guard used to be fixed or tied to Speed. At the settings that
    // stand for how it was (golden::compatParams: Max Cut 8 dB, the release Speed used to give,
    // Impact Guard 0; golden::compatTuning: the old notch hand-over) the core must give exactly
    // what the older cores gave: mono and stereo against the core before surround, 5.1 and 7.1.4
    // against the core before the three controls.
    GoldenRef ref, refSurround;
    std::string file, fileSurround;
    const bool haveRef = readGolden (t, "golden_ref", "the reference hashes of the core before surround support", ref, file);
    const bool haveSurround = readGolden (t, "golden_surround", "the reference hashes of the core before Max Cut, Release and Impact Guard", refSurround, fileSurround);
    if (! haveRef || ! haveSurround)
    {
        t.headline = std::string ("no reference for ") + goldenArch();
        return;
    }

    const auto now = golden::runAll (golden::PlainPrepare {}, true);
    const auto viaLayout = golden::runAll (pluginPrepare, true);
    const GoldenTally g = compareGolden (t, now, ref, file);
    int layoutDiffer = 0;
    for (std::size_t i = 0; i < now.size(); ++i)
        if (viaLayout[i].hash != now[i].hash)
        {
            ++layoutDiffer;
            t.check (false, now[i].name + ": prepared with the plug-in's layout, output or display frames differ from the three-argument prepare()");
        }
    t.check (g.differ == 0 && g.missing == 0 && g.same == (int) ref.size() && ! ref.empty(),
             fmt ("%d of %d mono and stereo renders (12 settings x 44.1/48/96 kHz, every output sample and display frame) at Max Cut 8 dB, the old release and Impact Guard 0 bit-identical to the core before surround",
                  g.same, (int) ref.size()));
    t.check (layoutDiffer == 0, fmt ("%d of %d renders differ when prepared with the plug-in's mono (centre) and stereo (left, right) layouts instead of the three-argument prepare()",
                                     layoutDiffer, (int) now.size()));

    const auto surroundNow = golden::runAllSurround (golden::SurroundPrepare {}, "layout", true);
    const GoldenTally gs = compareGolden (t, surroundNow, refSurround, fileSurround);
    // The surround file also holds the mono and stereo cases of the same core; only the surround ones are needed here.
    int surroundInRef = 0;
    for (const auto& e : refSurround)
        surroundInRef += e.first.find ("ch_layout_") != std::string::npos ? 1 : 0;
    t.check (gs.differ == 0 && gs.missing == 0 && gs.same == surroundInRef && surroundInRef > 0,
             fmt ("%d of %d 5.1 and 7.1.4 renders (12 settings at 48 kHz, 3 at 44.1 and 96 kHz) at the same settings bit-identical to the core before the three controls",
                  gs.same, surroundInRef));
    t.headline = fmt ("at Max Cut 8 dB, the old release, Impact Guard 0: %d of %d mono and stereo bit-identical to the core before surround, %d differ, %d missing, %d differ with the plug-in's layouts; %d of %d surround bit-identical to the core before the three controls",
                      g.same, (int) ref.size(), g.differ, g.missing, layoutDiffer, gs.same, surroundInRef);
}

void testGoldenDefaults (Ctx& t)
{
    // The plug-in's defaults (Max Cut 6 dB, Release 400 ms, Impact Guard 100%) against hashes
    // recorded from this core on purpose. It says nothing about how it should sound, only that
    // it has not changed since. The cases are those of golden_mono_stereo and three more that
    // make the three controls work; the guard's reading is in the hash.
    GoldenRef ref;
    std::string file;
    if (! readGolden (t, "golden_defaults", "the hashes of the core at its defaults", ref, file))
    {
        t.headline = std::string ("no reference for ") + goldenArch();
        return;
    }

    // Self-check first: a change detector is only worth having if the renders do something. In
    // every "defaults" and "key" case the cut must open, 96 kHz included; in the cases made for
    // it the cut must reach Max Cut and the guard must come up.
    {
        int idle = 0, total = 0;
        std::string which;
        float capReached = 0.0f, guardReached = 1.0f;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (int nch : { 1, 2 })
                for (const auto& cs : golden::cases (true, true))
                {
                    const std::string name = cs.name;
                    const bool plain = name == "defaults" || name == "key" || name == "defaults_threshold-34";
                    if (! plain && ! cs.impacts)
                        continue;
                    golden::Reached got;
                    golden::run (cs, sr, nch, 4.0, golden::PlainPrepare {}, false, true, &got, true);
                    ++total;
                    if (got.deepestCutDb < 0.5f)
                    {
                        ++idle;
                        which += fmt (" %s_%s_%.0f (%.2f dB)", cs.name, nch == 1 ? "mono" : "stereo", sr, (double) got.deepestCutDb);
                    }
                    if (cs.impacts)
                    {
                        capReached = std::max (capReached, got.deepestCutDb);
                        guardReached = std::min (guardReached, got.guardMax);
                    }
                }
        t.check (idle == 0, fmt ("the cases do something: %d of %d \"defaults\", \"key\" and extra renders never cut half a dB%s%s", idle, total, which.empty() ? "" : ":", which.c_str()));
        t.check (capReached >= dehonk::Params {}.maxCutDb - 0.01f && guardReached >= 0.99f,
                 fmt ("the cases made for it reach Max Cut (deepest cut %.2f dB of %.0f) and bring the guard up (lowest of their highest readings %.2f, need 0.99)", (double) capReached,
                      (double) dehonk::Params {}.maxCutDb, (double) guardReached));
    }

    auto now = golden::runAll (golden::PlainPrepare {}, false, true, true);
    const auto viaLayout = golden::runAll (pluginPrepare, false, true, true);
    int layoutDiffer = 0;
    for (std::size_t i = 0; i < now.size(); ++i)
        layoutDiffer += viaLayout[i].hash != now[i].hash ? 1 : 0;
    const auto surroundNow = golden::runAllSurround (golden::SurroundPrepare {}, "layout", false, true, true);
    now.insert (now.end(), surroundNow.begin(), surroundNow.end());
    const GoldenTally g = compareGolden (t, now, ref, file);
    t.check (g.differ == 0 && g.missing == 0 && g.same == (int) ref.size() && ! ref.empty(),
             fmt ("%d of %d renders at the defaults (mono, stereo, 5.1, 7.1.4; every output sample and display frame) bit-identical to the recorded hashes", g.same, (int) ref.size()));
    t.check (layoutDiffer == 0, fmt ("%d mono and stereo renders differ when prepared with the plug-in's layouts", layoutDiffer));
    t.headline = fmt ("%d of %d bit-identical to the recorded defaults, %d differ, %d missing; %d differ with the plug-in's layouts", g.same, (int) ref.size(), g.differ,
                      g.missing, layoutDiffer);
}

//==============================================================================
// surround_linked_matches_stereo

void testSurroundLinkedMatchesStereo (Ctx& t)
{
    // The same signal on every channel, Link 100%: every channel but the LFE must come out as the
    // stereo render of that signal does, display frames included. Threshold at Always: on a
    // surround master the Threshold reads the fold-down, which is louder when every speaker
    // carries the same thing, so with a threshold the two would rightly differ.
    const double sr = kSr;
    const Audio mono = surroundSignal (sr, 6.0, 1, 501);
    const auto& l = layoutNamed ("7.1.4");
    struct Setting { const char* name; dehonk::Params p; };
    std::vector<Setting> settings;
    { dehonk::Params p = alwaysOn(); settings.push_back ({ "defaults at Always", p }); }
    { dehonk::Params p = alwaysOn(); p.snapOn = true; p.lookaheadMs = 10.0f; settings.push_back ({ "Snap on, lookahead 10 ms", p }); }
    { dehonk::Params p = alwaysOn(); for (auto& m : p.moduleOn) m = true; p.depth = 1.0f; p.speed = 1.0f; p.sharpness = 0.8f; settings.push_back ({ "all modules, Depth 100%, fast, narrow", p }); }
    { dehonk::Params p = alwaysOn(); p.soloHonk = true; p.snapOn = true; settings.push_back ({ "Solo Honk, Snap on", p }); }
    { dehonk::Params p = alwaysOn(); p.diff = true; settings.push_back ({ "Diff", p }); }
    { dehonk::Params p = alwaysOn(); p.mix = 0.6f; p.trimDb = -4.0f; settings.push_back ({ "Mix 60%, Trim -4 dB", p }); }

    double worst = -400.0, worstFrame = 0.0;
    for (const auto& st : settings)
    {
        RenderSpec s2;
        s2.params = st.p;
        s2.collectFrames = true;
        const auto r2 = render (copies (mono.ch[0], sr, 2), s2);
        RenderSpec s12 = surroundSpec (st.p, l);
        s12.collectFrames = true;
        const auto r12 = render (copies (mono.ch[0], sr, 12), s12);

        double res = -400.0;
        std::size_t differing = 0;
        for (int c = 0; c < 12; ++c)
        {
            if (c == l.lfe())
                continue;
            res = std::max (res, residualDb (channelOf (r12.out, c), channelOf (r2.out, 0), 1.0, 0, mono.length()));
            differing += samplesDiffering (r12.out, c, r2.out, 0);
        }
        double frameDiff = 0.0;
        const bool sameFrames = r2.frames.size() == r12.frames.size();
        for (std::size_t i = 0; sameFrames && i < r2.frames.size(); ++i)
            for (int k = 0; k < r2.numBands; ++k)
            {
                frameDiff = std::max (frameDiff, (double) std::abs (r2.frames[i].cutDb[(std::size_t) k] - r12.frames[i].cutDb[(std::size_t) k]));
                frameDiff = std::max (frameDiff, (double) std::abs (r2.frames[i].levelDb[(std::size_t) k] - r12.frames[i].levelDb[(std::size_t) k]));
            }
        CutStats cs = cutStats (r2, 1.0);
        worst = std::max (worst, res);
        worstFrame = std::max (worstFrame, frameDiff);
        t.check (res < -120.0 && sameFrames && frameDiff < 1e-4,
                 fmt ("%s: 11 channels of 7.1.4 against stereo: residual %s dB (limit -120), %zu samples not bit-identical; display cut and level differ by up to %.6f dB (limit 0.0001) over %zu frames; stereo's deepest cut %.2f dB",
                      st.name, dbStr (res).c_str(), differing, frameDiff, r2.frames.size(), cs.maxTotal));
    }
    t.headline = fmt ("worst residual %s dB (limit -120), display frames within %.6f dB", dbStr (worst).c_str(), worstFrame);
}

//==============================================================================
// surround_lfe

void testSurroundLfe (Ctx& t)
{
    int wrong = 0, checks = 0;
    const double sr = kSr;
    struct L { const char* name; SurroundLayout layout; };
    const std::vector<L> layouts = {
        { "5.1", layoutNamed ("5.1") },
        { "7.1.4", layoutNamed ("7.1.4") },
        { "5.1, LFE last", { "5.1 LFE last", { "L", "R", "C", "Ls", "Rs", "LFE" } } },
    };
    for (const auto& lay : layouts)
    {
        const int n = lay.layout.size(), lfe = lay.layout.lfe();
        Audio in = surroundSignal (sr, 3.0, n, 600 + (std::uint64_t) n, -12.0);
        // The LFE: loud low noise with a full-scale square burst, nothing like the rest.
        {
            auto sub = pinkNoise (sr, in.length(), 611);
            normaliseRms (sub, -10.0);
            const auto sq = square (sr, in.length(), 40.0, 1.0);
            for (std::size_t i = 0; i < in.length(); ++i)
                in.ch[(std::size_t) lfe][i] = i > S (1.0) && i < S (1.5) ? sq[i] : sub[i];
        }
        struct Mode { const char* name; dehonk::Params p; int kind; };   // kind 0: delayed x gain, 1: silent
        std::vector<Mode> modes;
        for (float la : { 0.0f, 5.0f, 10.0f, 20.0f })
        {
            dehonk::Params p;
            p.lookaheadMs = la;
            p.snapOn = true;
            p.thresholdDb = -40.0f;
            modes.push_back ({ la == 0.0f ? "default, lookahead 0" : la == 5.0f ? "lookahead 5 ms" : la == 10.0f ? "lookahead 10 ms" : "lookahead 20 ms", p, 0 });
        }
        { dehonk::Params p; p.trimDb = 6.0f; p.mix = 0.3f; p.lookaheadMs = 5.0f; modes.push_back ({ "Trim +6 dB, Mix 30%", p, 0 }); }
        { dehonk::Params p; p.soloHonk = true; p.thresholdDb = -60.0f; p.lookaheadMs = 10.0f; modes.push_back ({ "Solo Honk", p, 1 }); }
        { dehonk::Params p; p.diff = true; p.lookaheadMs = 10.0f; modes.push_back ({ "Diff (plays as it is)", p, 0 }); }
        { dehonk::Params p; p.bypass = true; p.lookaheadMs = 20.0f; modes.push_back ({ "Bypass", p, 0 }); }
        { dehonk::Params p; p.link = 0.0f; p.thresholdDb = -60.0f; p.depth = 1.0f; modes.push_back ({ "Link 0%", p, 0 }); }

        for (const auto& m : modes)
        {
            RenderSpec s = surroundSpec (m.p, lay.layout);
            s.compensateLatency = false;
            s.blockSize = 333;
            dehonk::Core core;
            const auto r = render (core, in, s);
            const int lat = r.latency;
            const float trim = (m.p.bypass || m.p.diff) ? 1.0f : std::pow (10.0f, std::clamp (m.p.trimDb, -24.0f, 24.0f) / 20.0f);
            std::size_t bad = 0;
            double worstErr = 0.0;
            const auto& x = in.ch[(std::size_t) lfe];
            const auto& y = r.out.ch[(std::size_t) lfe];
            for (std::size_t i = 0; i < y.size(); ++i)
            {
                const float src = i >= (std::size_t) lat ? x[i - (std::size_t) lat] : 0.0f;
                const float want = m.kind == 1 ? 0.0f : trim * src;
                if (! (want == y[i]))
                {
                    ++bad;
                    worstErr = std::max (worstErr, (double) std::abs (want - y[i]));
                }
            }
            const bool latOk = lat == core.latencySamplesFor (m.p.lookaheadMs);
            ++checks;
            wrong += bad == 0 && latOk ? 0 : 1;
            t.check (bad == 0 && latOk, fmt ("%s, LFE at %d, %s: LFE out %s the LFE in delayed by the reported %d samples%s: %zu samples differ (largest by %.3g)",
                                             lay.name, lfe, m.name, m.kind == 1 ? "is silent, not" : "equals", lat,
                                             m.kind == 1 ? "" : (trim != 1.0f ? " times Trim" : ""), bad, worstErr));
        }

        // The LFE does not take part in anything: the other channels come out the same whatever it carries.
        {
            dehonk::Params p;
            p.snapOn = true;
            p.link = 0.6f;
            p.thresholdDb = -30.0f;
            Audio quiet = in;
            std::fill (quiet.ch[(std::size_t) lfe].begin(), quiet.ch[(std::size_t) lfe].end(), 0.0f);
            const auto a = render (in, surroundSpec (p, lay.layout));
            const auto b = render (quiet, surroundSpec (p, lay.layout));
            std::size_t differ = 0;
            for (int c = 0; c < n; ++c)
                if (c != lfe)
                    differ += samplesDiffering (a.out, c, b.out, c);
            ++checks;
            wrong += differ == 0 ? 0 : 1;
            t.check (differ == 0, fmt ("%s: with the LFE loud or silent, the other channels are bit-identical (Link 60%%, Snap on, threshold -30 dBFS): %zu samples differ",
                                       lay.name, differ));
        }
    }
    t.headline = fmt ("%d of %d checks wrong", wrong, checks);
}

//==============================================================================
// surround_unlinked

void testSurroundUnlinked (Ctx& t)
{
    // At Link 0% each channel is detected on its own: it comes out exactly as it would on its own
    // in mono. Threshold at Always and Snap off, the two things that are shared by design (the
    // programme level and the click detector).
    const double sr = kSr;
    const auto& l = layoutNamed ("5.1");
    const int n = l.size(), silent = 5;   // Rs
    Audio in = surroundSignal (sr, 4.0, n, 700, -14.0);
    std::fill (in.ch[(std::size_t) silent].begin(), in.ch[(std::size_t) silent].end(), 0.0f);
    dehonk::Params p = alwaysOn();
    p.link = 0.0f;
    p.depth = 0.8f;
    const auto r = render (in, surroundSpec (p, l));

    int wrong = 0;
    double deepest = 0.0;
    for (int c = 0; c < n; ++c)
    {
        if (c == l.lfe())
            continue;
        RenderSpec s;
        s.params = p;
        s.collectFrames = true;
        const auto alone = render (channelOf (in, c), s);
        const auto d = samplesDiffering (r.out, c, alone.out, 0);
        deepest = std::max (deepest, cutStats (alone, 0.5).maxTotal);
        wrong += d == 0 ? 0 : 1;
        t.check (d == 0, fmt ("channel %d (%s)%s: same as on its own in mono, %zu samples differ; on its own its deepest cut is %.2f dB", c, l.speakers[(std::size_t) c].c_str(),
                              c == silent ? ", silent" : "", d, cutStats (alone, 0.5).maxTotal));
    }
    {
        const double pk = peakAbs (channelOf (r.out, silent), 0, r.out.length());
        wrong += pk == 0.0 ? 0 : 1;
        t.check (pk == 0.0, fmt ("the silent channel stays exactly silent: peak %s dBFS", dbStr (ampToDb (pk)).c_str()));
    }
    // The silent channel does not affect the others: give it a loud honk and they do not change.
    {
        Audio loud = in;
        const Audio honk = surroundSignal (sr, 4.0, 1, 777, -6.0);
        loud.ch[(std::size_t) silent] = honk.ch[0];
        const auto r2 = render (loud, surroundSpec (p, l));
        std::size_t d = 0;
        for (int c = 0; c < n; ++c)
            if (c != silent)
                d += samplesDiffering (r.out, c, r2.out, c);
        wrong += d == 0 ? 0 : 1;
        t.check (d == 0, fmt ("with that channel silent or carrying a loud honk at -6 dBFS, the others are bit-identical: %zu samples differ", d));
    }
    // And at Link 100% the same signal is cut everywhere alike, so the comparison above means something.
    {
        dehonk::Params q = p;
        q.link = 1.0f;
        const auto r3 = render (in, surroundSpec (q, l));
        const double diff = residualDb (channelOf (r3.out, 0), channelOf (r.out, 0), 1.0, S (0.5), in.length());
        t.check (diff > -60.0, fmt ("control: at Link 100%% channel 0 comes out differently from Link 0%% (residual %s dB, need above -60)", dbStr (diff).c_str()));
        wrong += diff > -60.0 ? 0 : 1;
    }
    t.headline = fmt ("%d checks wrong; per-channel detection exact against mono, deepest cut %.2f dB", wrong, deepest);
}

//==============================================================================
// surround_threshold_level

void testSurroundThresholdLevel (Ctx& t)
{
    // The Threshold compares the louder side of a stereo fold-down: L and R at 1, the centre and
    // everything to one side at 0.7071, LFE left out.
    const double sr = kSr;
    const std::size_t n = S (4.0);
    // The programme level the threshold compares, averaged in dB over the frames after the first second.
    auto lastProgram = [] (const RenderResult& r) {
        double sum = 0.0;
        int count = 0;
        for (const auto& f : r.frames)
            if (f.timeSec >= 1.0)
            {
                sum += f.programDb;
                ++count;
            }
        return count > 0 ? sum / count : -200.0;
    };
    auto run = [&] (const Audio& a, const SurroundLayout* l) {
        dehonk::Params p;
        p.thresholdDb = -60.0f;   // keeps programDb from being held up near the threshold
        RenderSpec s = l != nullptr ? surroundSpec (p, *l) : RenderSpec {};
        s.params = p;
        s.collectFrames = true;
        return render (a, s);
    };
    int wrong = 0;
    const Audio st = pinkAudio (sr, n, 2, -20.0, 801);

    // 1. A 5.1 holding a stereo mix in L and R alone reads exactly what stereo reads.
    {
        Audio s51 = Audio::make (sr, 6, n);
        s51.ch[0] = st.ch[0];
        s51.ch[1] = st.ch[1];
        s51.ch[3] = pinkNoise (sr, n, 802);   // LFE: ignored
        const double a = lastProgram (run (st, nullptr)), b = lastProgram (run (s51, &layoutNamed ("5.1")));
        const bool ok = std::abs (a - b) < 1e-4;
        wrong += ok ? 0 : 1;
        t.check (ok, fmt ("stereo in L and R of a 5.1 (and noise in the LFE): %.3f dBFS against stereo's %.3f (limit 0.0001 dB)", b, a));
    }
    // 2. A centre-only 5.1 reads as the same signal panned to the phantom centre of a stereo mix (-3 dB each side).
    {
        Audio c51 = Audio::make (sr, 6, n);
        c51.ch[2] = st.ch[0];
        Audio phantom = Audio::make (sr, 2, n);
        for (int c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < n; ++i)
                phantom.ch[(std::size_t) c][i] = 0.70710678f * st.ch[0][i];
        const double a = lastProgram (run (phantom, nullptr)), b = lastProgram (run (c51, &layoutNamed ("5.1")));
        const bool ok = std::abs (a - b) < 0.01;
        wrong += ok ? 0 : 1;
        t.check (ok, fmt ("centre only, 5.1: %.3f dBFS against %.3f for the same signal at -3 dB in both channels of stereo (limit 0.01 dB)", b, a));
    }
    // 3. Equal, independent noise in all eleven speakers of 7.1.4: the left side folds L at 1 and
    //    C, Lss, Lrs, Ltf, Ltr at 0.7071, so it reads 1 + 5 x 0.5 = 3.5 times one speaker's power (+5.44 dB).
    {
        const auto& l = layoutNamed ("7.1.4");
        Audio a = Audio::make (sr, 12, n);
        Audio one = Audio::make (sr, 1, n);
        for (int c = 0; c < 12; ++c)
        {
            a.ch[(std::size_t) c] = pinkNoise (sr, n, 810 + (std::uint64_t) c);
            normaliseRms (a.ch[(std::size_t) c], -26.0);
        }
        one.ch[0] = a.ch[0];
        const double single = lastProgram (run (one, nullptr));
        const double all = lastProgram (run (a, &l));
        const double expect = single + powerToDb (3.5);
        const bool ok = std::abs (all - expect) < 1.0;
        wrong += ok ? 0 : 1;
        t.check (ok, fmt ("7.1.4, equal independent pink in every speaker: %.2f dBFS, one speaker alone %.2f; the fold-down predicts %.2f (limit 1 dB, the reading is a 50 ms window of noise)",
                          all, single, expect));
    }
    t.headline = fmt ("%d of 3 checks wrong", wrong);
}

//==============================================================================
// surround_block_size_invariance

void testSurroundBlockSizeInvariance (Ctx& t)
{
    const auto& l = layoutNamed ("7.1.4");
    const Audio in = surroundSignal (kSr, 3.0, 12, 900, -16.0);
    double worst = -400.0;
    for (float link : { 1.0f, 0.5f })
        for (float la : { 0.0f, 10.0f })
        {
            dehonk::Params p;
            p.snapOn = true;
            p.link = link;
            p.lookaheadMs = la;
            p.depth = 0.8f;
            p.thresholdDb = (float) (powerToDb (meanSquare (in, 0, in.length())) - 3.0);
            RenderSpec ref = surroundSpec (p, l);
            ref.blockSize = 512;
            const auto r0 = render (in, ref);
            for (int bs : { 1, 7, 32, 4096, -1 })
            {
                RenderSpec s = surroundSpec (p, l);
                Rng rng (93);
                if (bs > 0)
                    s.blockSize = bs;
                else
                {
                    s.blockSize = 4096;
                    s.maxBlockSize = 4096;
                    s.nextBlockSize = [&rng] { return (int) std::lround (std::exp2 (rng.uniform (0.0, 12.0))); };
                }
                const auto r = render (in, s);
                const double res = residualDb (r.out, r0.out, 1.0, 0, in.length());
                worst = std::max (worst, res);
                t.check (res < -90.0, fmt ("7.1.4, Link %.0f%%, Snap on, lookahead %2.0f ms, block %s vs 512: residual %s dB (limit -90)", 100.0 * link, (double) la,
                                           bs > 0 ? std::to_string (bs).c_str() : "random 1-4096", dbStr (res).c_str()));
            }
        }
    t.headline = "worst residual " + dbStr (worst) + " dB (limit -90)";
}

//==============================================================================
// surround_latency

void testSurroundLatency (Ctx& t)
{
    int wrong = 0;
    const auto& l = layoutNamed ("7.1.4");
    for (double sr : { 48000.0, 96000.0 })
        for (float la : { 0.0f, 5.0f, 10.0f, 20.0f })
        {
            const std::size_t n = S (0.2, sr) + 4096;
            Audio in = Audio::make (sr, 12, n);
            for (int c = 0; c < 12; ++c)
                in.ch[(std::size_t) c][1000 + 37 * (std::size_t) c] = 1.0f;
            dehonk::Params p;
            p.depth = 0.0f;
            p.lookaheadMs = la;
            RenderSpec s = surroundSpec (p, l);
            s.compensateLatency = false;
            dehonk::Core core;
            const auto r = render (core, in, s);
            int badChannels = 0;
            for (int c = 0; c < 12; ++c)
            {
                const auto& y = r.out.ch[(std::size_t) c];
                const std::size_t at = 1000 + 37 * (std::size_t) c + (std::size_t) r.latency;
                double rest = 0.0;
                for (std::size_t i = 0; i < y.size(); ++i)
                    if (i != at)
                        rest = std::max (rest, (double) std::abs (y[i]));
                badChannels += (std::abs (y[at] - 1.0f) < 1e-4f && rest < 1e-5) ? 0 : 1;
            }
            const bool ok = badChannels == 0 && r.latency == core.latencySamplesFor (la);
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%6.0f Hz, lookahead %2.0f ms, 7.1.4: reported %d; %d of 12 channels put the impulse elsewhere", sr, (double) la, r.latency, badChannels));
        }
    t.headline = fmt ("%d of 8 cases wrong", wrong);
}

//==============================================================================
// surround_stability

void testSurroundStability (Ctx& t)
{
    int runs = 0, bad = 0;
    std::string failures;
    for (const char* name : { "5.1", "7.1.4" })
    {
        const auto& l = layoutNamed (name);
        const int nch = l.size();
        for (double sr : { 48000.0, 96000.0 })
            for (int kind = 0; kind < 5; ++kind)
            {
                const std::size_t sig = S (1.0, sr), n = S (3.0, sr);
                Audio in = Audio::make (sr, nch, n);
                const char* kinds[] = { "silence", "DC 0.5", "full-scale noise, NaN and infinity in channel 0", "denormal-level noise", "honk bed, every parameter random every block" };
                Rng rng (1000 + (std::uint64_t) kind);
                for (int c = 0; c < nch; ++c)
                {
                    auto& x = in.ch[(std::size_t) c];
                    for (std::size_t i = 0; i < sig; ++i)
                        switch (kind)
                        {
                            case 1: x[i] = 0.5f; break;
                            case 2: x[i] = (float) rng.uniform (-1.0, 1.0); break;
                            case 3: x[i] = (float) (1e-30 * rng.gauss()); break;
                            default: break;
                        }
                    if (kind == 2 && c == 0)
                        for (std::size_t i = 1000; i < sig; i += 7919)
                            x[i] = (i / 7919) % 2 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
                }
                if (kind == 4)
                {
                    const Audio h = surroundSignal (sr, 1.0, nch, 1100, -12.0);
                    for (int c = 0; c < nch; ++c)
                        for (std::size_t i = 0; i < sig; ++i)
                            in.ch[(std::size_t) c][i] = std::clamp (h.ch[(std::size_t) c][i], -1.0f, 1.0f);
                }
                RenderSpec s = surroundSpec (alwaysOn(), l);
                s.params.snapOn = true;
                s.compensateLatency = false;
                std::int64_t nextLa = 0;
                if (kind == 4)
                {
                    s.blockSize = 1024;
                    s.maxBlockSize = 1024;
                    s.nextBlockSize = [&rng] { return rng.uniformInt (1, 1024); };
                    s.automate = [&rng, &nextLa, sig, sr] (std::int64_t pos, dehonk::Params& p) {
                        if (pos >= (std::int64_t) sig)
                        {
                            p = dehonk::Params {};
                            return;
                        }
                        const bool la = pos >= nextLa;
                        if (la)
                            nextLa = pos + (std::int64_t) (0.25 * sr);
                        p = randomParams (rng, p, la);
                    };
                }
                const auto r = render (in, s);
                // Every channel is judged, the one fed NaN and infinity included: with more than
                // two channels a non-finite sample becomes silence before it reaches the notches.
                const auto j = judge (r.out, r.out.length() - S (0.5, sr));
                ++runs;
                const bool ok = j.finite && j.peak <= kPlusTwelveDb && j.tail <= 1e-5;
                if (! ok)
                {
                    ++bad;
                    failures += fmt (" [%s %.0f Hz %s: finite %s, peak %s dBFS, tail %s dBFS]", name, sr, kinds[kind], j.finite ? "yes" : "NO",
                                     dbStr (ampToDb (j.peak)).c_str(), dbStr (ampToDb (j.tail)).c_str());
                }
            }
    }
    t.check (failures.empty(), fmt ("silence, DC, full-scale noise (NaN and infinity in channel 0, every channel judged), denormals, random automation; 5.1 and 7.1.4 at 48 and 96 kHz, Snap on: %d of %d runs fail (finite, peak under +12 dBFS, last 0.5 s of silence under -100 dBFS)%s",
                                    bad, runs, failures.c_str()));

    // prepare() again mid-stream with another channel count and layout, and reset().
    {
        dehonk::Core core;
        dehonk::Params p = alwaysOn();
        p.depth = 1.0f;
        p.lookaheadMs = 10.0f;
        p.snapOn = true;
        bool ok = true;
        double peak = 0.0, tail = 0.0;
        for (const char* name : { "7.1.4", "stereo", "5.1", "7.1.4", "mono" })
        {
            const auto& l = layoutNamed (name);
            core.prepare (48000.0, 512, l.size(), l.layout());
            core.setParams (p);
            Audio a = surroundSignal (48000.0, 2.0, l.size(), 1200, -12.0);
            for (auto& ch : a.ch)
                std::fill (ch.begin() + (std::ptrdiff_t) S (1.0), ch.end(), 0.0f);
            const auto out = streamThrough (core, a, 512);
            const auto j = judge (out, out.length() - S (0.5));
            ok = ok && j.finite && j.peak <= kPlusTwelveDb && j.tail <= 1e-5;
            peak = std::max (peak, j.peak);
            tail = std::max (tail, j.tail);
            core.reset();
        }
        ++runs;
        bad += ok ? 0 : 1;
        t.check (ok, fmt ("prepare() again mid-stream 7.1.4 -> stereo -> 5.1 -> 7.1.4 -> mono, reset() between: peak %s dBFS, tails %s dBFS",
                          dbStr (ampToDb (peak)).c_str(), dbStr (ampToDb (tail)).c_str()));
    }
    t.headline = fmt ("%d of %d runs failed", bad, runs);
}

//==============================================================================
// surround_recovery

namespace
{
/** Streams `a` through `core` in two halves and returns Diff's level-match offset after each. */
std::pair<float, float> diffOffsets (dehonk::Core& core, const Audio& a, bool& finite)
{
    const std::size_t half = a.length() / 2;
    Audio first = Audio::make (a.sampleRate, a.numChannels(), half);
    Audio second = Audio::make (a.sampleRate, a.numChannels(), a.length() - half);
    for (int c = 0; c < a.numChannels(); ++c)
    {
        std::copy_n (a.ch[(std::size_t) c].begin(), half, first.ch[(std::size_t) c].begin());
        std::copy (a.ch[(std::size_t) c].begin() + (std::ptrdiff_t) half, a.ch[(std::size_t) c].end(), second.ch[(std::size_t) c].begin());
    }
    const auto o1 = streamThrough (core, first, 512);
    const float d1 = core.displayFrame().diffOffsetDb;
    const auto o2 = streamThrough (core, second, 512);
    const float d2 = core.displayFrame().diffOffsetDb;
    finite = allFinite (o1) && allFinite (o2);
    return { d1, d2 };
}
} // namespace

void testSurroundRecovery (Ctx& t)
{
    // 1. One NaN and one infinity in a treated channel of 5.1 and 7.1.4: every channel stays
    //    finite and Diff's level match keeps working (within 0.05 dB of the clean run).
    int wrong = 0;
    for (const char* name : { "5.1", "7.1.4" })
    {
        const auto& l = layoutNamed (name);
        const int nch = l.size();
        dehonk::Params p = alwaysOn();
        for (auto& on : p.moduleOn)
            on = true;
        p.snapOn = true;
        const Audio clean = surroundSignal (kSr, 20.0, nch, 1500, -12.0);
        Audio hostile = clean;
        hostile.ch[(std::size_t) nch - 1][S (2.0)] = std::numeric_limits<float>::quiet_NaN();
        hostile.ch[0][S (3.0)] = std::numeric_limits<float>::infinity();
        hostile.ch[0][S (3.5)] = 1.0e20f;

        dehonk::Core a, b;
        a.prepare (kSr, 512, nch, l.layout());
        b.prepare (kSr, 512, nch, l.layout());
        a.setParams (p);
        b.setParams (p);
        bool fa = false, fb = false;
        const auto da = diffOffsets (a, clean, fa);
        const auto db = diffOffsets (b, hostile, fb);
        const bool ok = fa && fb && da.second < -0.01f && std::abs (da.first - db.first) < 0.05f && std::abs (da.second - db.second) < 0.05f;
        wrong += ok ? 0 : 1;
        t.check (ok, fmt ("%s, NaN at 2 s in the last channel, infinity and 1e20 in channel 0: every channel finite %s; Diff's offset at 10 s %.3f dB (clean %.3f), at 20 s %.3f dB (clean %.3f), need within 0.05 dB",
                          name, fb ? "yes" : "NO", db.first, da.first, db.second, da.second));
    }

    // 2. A call with fewer channels than prepared, all of them LFE: those channels are treated
    //    as ordinary ones, nothing goes NaN, and the level match is not left dead afterwards.
    {
        const int nch = 6;
        const auto layout = dehonk::ChannelLayout::withLfe (0);
        dehonk::Params p = alwaysOn();
        p.diff = true;
        const Audio prog = surroundSignal (kSr, 10.0, nch, 1600, -12.0);
        const Audio one = surroundSignal (kSr, 0.25, 1, 1601, -12.0);

        dehonk::Core fresh, used;
        fresh.prepare (kSr, 512, nch, layout);
        used.prepare (kSr, 512, nch, layout);
        fresh.setParams (p);
        used.setParams (p);
        const Audio shortOut = streamThrough (used, one, 512);    // about 20 calls of one channel
        used.reset();
        bool ff = false, fu = false;
        const auto df = diffOffsets (fresh, prog, ff);
        const auto du = diffOffsets (used, prog, fu);
        const bool ok = allFinite (shortOut) && ff && fu && df.second < -0.01f && std::abs (df.second - du.second) < 0.05f;
        // reset() clears the level match too, so also run without it in between.
        dehonk::Core noReset;
        noReset.prepare (kSr, 512, nch, layout);
        noReset.setParams (p);
        const Audio shortOut2 = streamThrough (noReset, one, 512);
        bool fn = false;
        const auto dn = diffOffsets (noReset, prog, fn);
        const bool ok2 = allFinite (shortOut2) && fn && std::abs (df.second - dn.second) < 0.05f;
        wrong += (ok ? 0 : 1) + (ok2 ? 0 : 1);
        t.check (ok && ok2, fmt ("prepared for 6 channels with the LFE first, then calls with that one channel only: finite %s; Diff's offset afterwards %.3f dB (with reset() between %.3f dB), on a fresh core %.3f dB, need within 0.05 dB",
                                 allFinite (shortOut2) && fn ? "yes" : "NO", dn.second, du.second, df.second));
    }
    t.headline = fmt ("%d of 3 checks wrong", wrong);
}

//==============================================================================
// surround_channel_counts

void testSurroundChannelCounts (Ctx& t)
{
    // Every channel count from 1 to 12, with its layout and without one (the three-argument
    // prepare: no LFE). Finite output, notches on every channel but the LFE, the LFE exact.
    int wrong = 0;
    std::string summary;
    for (const auto& l : surroundLayouts())
        for (int withLayout = 1; withLayout >= 0; --withLayout)
        {
            if (! withLayout && l.size() <= 2)
                continue;
            const int n = l.size(), lfe = withLayout ? l.lfe() : -1;
            const Audio in = surroundSignal (kSr, 3.0, n, 1300 + (std::uint64_t) n, -14.0);
            dehonk::Params p = alwaysOn();
            p.snapOn = true;
            p.lookaheadMs = 5.0f;
            RenderSpec s = withLayout ? surroundSpec (p, l) : RenderSpec {};
            s.params = p;
            s.collectFrames = true;
            const auto r = render (in, s);
            const bool finite = allFinite (r.out);
            double leastChange = 1e9;
            for (int c = 0; c < n; ++c)
                if (c != lfe)
                    leastChange = std::min (leastChange, residualDb (channelOf (r.out, c), channelOf (in, c), 1.0, S (0.5), in.length()));
            const std::size_t lfeDiff = lfe >= 0 ? samplesDiffering (r.out, lfe, in, lfe) : 0;
            const double deepest = cutStats (r, 0.5).maxTotal;
            const bool ok = finite && leastChange > -60.0 && deepest > 1.0 && lfeDiff == 0;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%2d channels (%s%s): finite %s; every channel but the LFE treated (smallest change %s dB, need above -60); deepest cut %.2f dB; %s",
                              n, l.name, withLayout ? "" : ", no layout given", finite ? "yes" : "NO", dbStr (leastChange).c_str(), deepest,
                              lfe >= 0 ? fmt ("LFE at %d: %zu samples differ from its input", lfe, lfeDiff).c_str() : "no LFE"));
        }
    t.headline = fmt ("%d of 22 layouts wrong (1 to 12 channels, with and without a layout)", wrong);
}

//==============================================================================
// surround_cpu

void testSurroundCpu (Ctx& t)
{
    // Default settings (Tub and Fog, threshold -16 dBFS, Link 100%, Snap off) on programme loud
    // enough to be over the threshold, then all four modules and Snap, always acting, at Link
    // 100%, 50% and 0%, per layout. Link 100% is the cheap case of the three: one detector, and
    // the notches run on four channels side by side. Below it every treated channel runs its own
    // detector and its notches one channel at a time, so Link 0% is the heaviest setting.
    // Percent of one core, average over 20 s after 1 s of warm-up.
    const double warm = 1.0, timed = 20.0;
    std::string table;
    // Then the three additions of SPEC 13.17: Grouped (one detector per group), every range's
    // timing unlocked, and the dialogue sidechain (the key's own analysis and the comparison).
    const char* settingNames[] = { "defaults", "all four+Snap, always, Link 100%", "all four+Snap, always, Link 50%", "all four+Snap, always, Link 0% (heaviest)",
                                   "all four+Snap, always, Grouped", "defaults, every range's timing unlocked", "defaults, dialogue keyed", "all four+Snap, always, Unlinked, dialogue keyed",
                                   "defaults, Detection Steady", "all four+Snap, always, Link 100%, Detection Steady", "all four+Snap, always, Unlinked, Detection Steady" };
    // And Detection Steady (SPEC 13.19): at the defaults, and all four + Snap linked and unlinked.
    constexpr int numSettings = (int) std::size (settingNames);
    for (double sr : { 48000.0, 96000.0 })
        for (int heavy = 0; heavy < numSettings; ++heavy)
        {
            std::string row = fmt ("%.0f kHz %s:", sr / 1000.0, settingNames[heavy]);
            for (const char* name : { "stereo", "5.1", "7.1", "7.1.4" })
            {
                const auto& l = layoutNamed (name);
                const Audio in = surroundSignal (sr, warm + timed, l.size(), 1400, -10.0);
                dehonk::Params p;
                if ((heavy >= 1 && heavy <= 4) || heavy == 7 || heavy == 9 || heavy == 10)
                {
                    p = alwaysOn();
                    for (auto& on : p.moduleOn)
                        on = true;
                    p.snapOn = true;
                    p.link = heavy == 1 ? 1.0f : (heavy == 2 ? 0.5f : (heavy == 3 ? 0.0f : 1.0f));
                    if (heavy == 4) p.linkMode = dehonk::LinkMode::grouped;
                    if (heavy == 7 || heavy == 10) p.linkMode = dehonk::LinkMode::unlinked;
                }
                if (heavy >= 8)
                    p.detection = dehonk::Detection::steady;
                if (heavy == 5)
                    for (int m = 0; m < dehonk::kNumModules; ++m)
                    {
                        p.rangeTimingLocked[m] = false;
                        p.rangeAttackMs[m] = 5.0f + 10.0f * (float) m;
                        p.rangeReleaseMs[m] = 200.0f + 100.0f * (float) m;
                    }
                p.dialogueOn = heavy == 6 || heavy == 7;
                // The key: the first channel's own signal, a quarter of a second on, so it is loud and unrelated in time.
                Audio key = Audio::make (sr, 1, in.length());
                for (std::size_t i = S (0.25, sr); i < in.length(); ++i)
                    key.ch[0][i] = in.ch[0][i - S (0.25, sr)];
                double best = 1e9;
                for (int bs : { 128, 1024 })
                {
                    RenderSpec s = surroundSpec (p, l);
                    s.blockSize = bs;
                    s.timeBlocks = true;
                    s.compensateLatency = false;
                    s.collectFrames = false;
                    if (heavy == 6 || heavy == 7)
                        s.key = &key;
                    const auto r = render (in, s);
                    double sum = 0.0;
                    std::size_t samples = 0;
                    for (std::size_t i = 0; i < r.blockSeconds.size(); ++i)
                        if (r.blockStarts[i] >= (std::int64_t) S (warm, sr) && r.blockSizes[i] == bs)
                        {
                            sum += r.blockSeconds[i];
                            samples += (std::size_t) bs;
                        }
                    const double pct = 100.0 * sum / ((double) samples / sr);
                    best = std::min (best, pct);
                    t.info (fmt ("%.0f kHz, %s, %s, block %d: %.2f%% of one core", sr / 1000.0, settingNames[heavy], name, bs, pct));
                }
                row += fmt (" %s %.2f%%", name, best);
            }
            t.info (row + " (the better of blocks 128 and 1024)");
            table += (table.empty() ? "" : "; ") + row;
        }
    t.info ("timed with std::chrono::steady_clock, single thread, core only; informational, nothing is checked");
    t.headline = table;
}

//==============================================================================
// SPEC 13.17: link modes, each range's own timing, the dialogue sidechain

/** A group's speakers of a layout, in order, by name: the fronts, the centre, the surrounds (any
    other L.. or R.. that is not overhead) and the heights (Lt.., Rt..). */
std::vector<int> groupMembers (const SurroundLayout& l, dehonk::Group g)
{
    std::vector<int> m;
    for (int c = 0; c < l.size(); ++c)
    {
        const auto& s = l.speakers[(std::size_t) c];
        dehonk::Group is = dehonk::Group::surround;
        if (s == "LFE") continue;
        if (s == "L" || s == "R") is = dehonk::Group::front;
        else if (s == "C") is = dehonk::Group::centre;
        else if (s.size() > 1 && s[1] == 't') is = dehonk::Group::height;
        if (is == g)
            m.push_back (c);
    }
    return m;
}

/** The worst the click metric of automation_clicks reads on any channel: energy above 6 kHz in
    a 10 ms window that the input times a slowly moving gain does not explain. */
double worstUnexplained (const Audio& in, const Audio& out, const std::vector<int>& skip = {})
{
    double worst = -400.0;
    for (int c = 0; c < in.numChannels(); ++c)
    {
        if (std::find (skip.begin(), skip.end(), c) != skip.end())
            continue;
        const auto [m, at] = hfUnexplained (highpass4 (in.ch[(std::size_t) c], kSr, 6000.0), highpass4 (out.ch[(std::size_t) c], kSr, 6000.0), S (0.5), S (0.010));
        (void) at;
        worst = std::max (worst, m);
    }
    return worst;
}

void testLinkModes (Ctx& t)
{
    // Threshold at Always and Snap off throughout: the programme level and Snap's detector are
    // shared by design, whatever Link is.
    int wrong = 0;

    // 1. Unlinked is what Link 0% was, Linked what Link 100% was, and Grouped is Linked with one
    //    or two channels. Bit for bit, output and frames.
    for (int nch : { 1, 2, 6, 12 })
    {
        const Audio in = surroundSignal (kSr, 3.0, nch, 2100 + (std::uint64_t) nch, -14.0);
        auto run = [&] (dehonk::LinkMode mode, float link) {
            dehonk::Params p = alwaysOn();
            p.depth = 0.8f;
            p.linkMode = mode;
            p.link = link;
            RenderSpec s = specForChannels (p, nch);
            s.collectFrames = true;
            return render (in, s);
        };
        auto same = [&] (const RenderResult& a, const RenderResult& b) {
            std::size_t d = 0;
            for (int c = 0; c < nch; ++c)
                d += samplesDiffering (a.out, c, b.out, c);
            for (std::size_t f = 0; f < std::min (a.frames.size(), b.frames.size()); ++f)
                d += std::memcmp (a.frames[f].cutDb.data(), b.frames[f].cutDb.data(), sizeof (float) * dehonk::kMaxBands) != 0 ? 1 : 0;
            return d + (a.frames.size() != b.frames.size() ? 1 : 0);
        };
        const auto linked = run (dehonk::LinkMode::linked, 1.0f);
        const auto oldUnlinked = run (dehonk::LinkMode::linked, 0.0f);
        const auto unlinked = run (dehonk::LinkMode::unlinked, 1.0f);
        const auto grouped = run (dehonk::LinkMode::grouped, 1.0f);
        const auto d1 = same (unlinked, oldUnlinked);
        wrong += d1 == 0 ? 0 : 1;
        t.check (d1 == 0, fmt ("%s: Unlinked is bit-identical to what Link 0%% was: %zu samples or frames differ; deepest cut %.2f dB", channelsName (nch), d1, cutStats (unlinked, 0.5).maxTotal));
        if (nch <= 2)
        {
            const auto d2 = same (grouped, linked);
            wrong += d2 == 0 ? 0 : 1;
            t.check (d2 == 0, fmt ("%s: Grouped is bit-identical to Linked: %zu samples or frames differ", channelsName (nch), d2));
        }
        if (nch >= 2)
        {
            const double apart = residualDb (channelOf (unlinked.out, 0), channelOf (linked.out, 0), 1.0, S (0.5), in.length());
            wrong += apart > -60.0 ? 0 : 1;
            t.check (apart > -60.0, fmt ("%s, control: Unlinked comes out differently from Linked on channel 0 (residual %s dB, need above -60)", channelsName (nch), dbStr (apart).c_str()));
        }
    }

    // 2. Grouped with more than two channels: every group comes out exactly as its channels do
    //    on their own, linked (the front pair as a stereo instance, the centre as a mono one,
    //    the surrounds and the heights as instances of their own).
    for (const char* name : { "5.1", "7.1.4", "7.0.2", "LCR" })
    {
        const auto& l = layoutNamed (name);
        Audio in = surroundSignal (kSr, 3.0, l.size(), 2200, -14.0);
        // A different peak in every group, so the groups want different notches.
        const double extra[] = { 0.0, 180.0, 300.0, 131.0, 220.0 };
        for (auto g : { dehonk::Group::front, dehonk::Group::centre, dehonk::Group::surround, dehonk::Group::height })
            for (int c : groupMembers (l, g))
            {
                const auto tone = sine (kSr, in.length(), extra[(int) g], dbToAmp (-16.0));
                for (std::size_t i = 0; i < in.length(); ++i)
                    in.ch[(std::size_t) c][i] += tone[i];
            }
        dehonk::Params p = alwaysOn();
        p.depth = 0.8f;
        p.linkMode = dehonk::LinkMode::grouped;
        RenderSpec s = surroundSpec (p, l);
        s.useTuning = true;
        s.tuning = noParentTuning();
        const auto r = render (in, s);
        dehonk::Params q = p;
        q.linkMode = dehonk::LinkMode::linked;
        std::string sizes;
        for (auto g : { dehonk::Group::front, dehonk::Group::centre, dehonk::Group::surround, dehonk::Group::height })
        {
            const auto members = groupMembers (l, g);
            if (members.empty())
                continue;
            std::vector<std::vector<float>> chans;
            for (int c : members)
                chans.push_back (in.ch[(std::size_t) c]);
            RenderSpec gs;
            gs.params = q;
            gs.useTuning = true;
            gs.tuning = noParentTuning();
            gs.collectFrames = true;
            if (members.size() > 2)
            {
                gs.useLayout = true;
                for (std::size_t i = 0; i < members.size(); ++i)
                    gs.layout.position[i] = i % 2 == 0 ? dehonk::Position::leftOther : dehonk::Position::rightOther;
            }
            const auto alone = render (fromChannels (kSr, chans), gs);
            std::size_t d = 0;
            for (std::size_t i = 0; i < members.size(); ++i)
                d += samplesDiffering (r.out, members[i], alone.out, (int) i);
            wrong += d == 0 ? 0 : 1;
            const char* gname = g == dehonk::Group::front ? "front pair" : g == dehonk::Group::centre ? "centre" : g == dehonk::Group::surround ? "surrounds" : "heights";
            t.check (d == 0, fmt ("%s Grouped, %s (%zu channels): bit-identical to those channels on their own, linked: %zu samples differ; their deepest cut %.2f dB",
                                  name, gname, members.size(), d, cutStats (alone, 0.5).maxTotal));
            sizes += fmt ("%s%zu", sizes.empty() ? "" : "+", members.size());
        }
        // And it is neither Linked nor Unlinked.
        for (auto other : { dehonk::LinkMode::linked, dehonk::LinkMode::unlinked })
        {
            dehonk::Params o = p;
            o.linkMode = other;
            RenderSpec os = s;
            os.params = o;
            const auto ro = render (in, os);
            double apart = -400.0;
            for (int c = 0; c < l.size(); ++c)
                if (c != l.lfe())
                    apart = std::max (apart, residualDb (channelOf (ro.out, c), channelOf (r.out, c), 1.0, S (0.5), in.length()));
            wrong += apart > -60.0 ? 0 : 1;
            t.check (apart > -60.0, fmt ("%s (%s), control: Grouped comes out differently from %s (largest residual on a channel %s dB, need above -60)", name, sizes.c_str(),
                                         other == dehonk::LinkMode::linked ? "Linked" : "Unlinked", dbStr (apart).c_str()));
        }
        if (l.lfe() >= 0)
        {
            const auto d = samplesDiffering (r.out, l.lfe(), in, l.lfe());
            wrong += d == 0 ? 0 : 1;
            t.check (d == 0, fmt ("%s Grouped: the LFE is untouched (%zu samples differ)", name, d));
        }
    }

    // 3. Switching the mode while it plays makes no click, and neither does every block size.
    for (int nch : { 2, 6, 12 })
    {
        Audio in = surroundSignal (kSr, 6.0, nch, 2300, -14.0);
        for (int c = 0; c < nch; ++c)
        {
            const auto tone = sine (kSr, in.length(), 180.0 + 23.0 * c, dbToAmp (-16.0));
            for (std::size_t i = 0; i < in.length(); ++i)
                in.ch[(std::size_t) c][i] += tone[i];
        }
        dehonk::Params p = alwaysOn();
        p.depth = 1.0f;
        p.snapOn = false;
        RenderSpec s = specForChannels (p, nch);
        s.useTuning = true;
        s.tuning = noParentTuning();
        s.blockSize = 32;
        s.automate = [] (std::int64_t pos, dehonk::Params& q) { q.linkMode = (dehonk::LinkMode) (((int) ((double) pos / kSr / 0.35)) % 3); };
        const auto r = render (in, s);
        const std::vector<int> skip = nch > 2 ? std::vector<int> { 3 } : std::vector<int> {};
        const double hf = worstUnexplained (in, r.out, skip);
        wrong += hf < -40.0 ? 0 : 1;
        t.check (hf < -40.0, fmt ("%s: Linked, Grouped, Unlinked switched every 0.35 s: unexplained energy above 6 kHz, worst 10 ms window on any channel %s dB (limit -40)", channelsName (nch), dbStr (hf).c_str()));
        if (nch > 2)
        {
            Rng rng (2301);
            RenderSpec v = s;
            v.automate = nullptr;
            v.params.linkMode = dehonk::LinkMode::grouped;
            v.blockSize = 512;
            const auto a = render (in, v);
            v.maxBlockSize = 1024;
            v.nextBlockSize = [&rng] { return rng.uniformInt (1, 1024); };
            const auto b = render (in, v);
            std::size_t d = 0;
            for (int c = 0; c < nch; ++c)
                d += samplesDiffering (a.out, c, b.out, c);
            wrong += d == 0 ? 0 : 1;
            t.check (d == 0, fmt ("%s Grouped: blocks of 512 and random blocks of 1 to 1024 give the same output: %zu samples differ", channelsName (nch), d));
        }
    }
    t.headline = fmt ("%d checks wrong; Unlinked = old Link 0%% and Grouped = Linked in mono and stereo, bit for bit; each group of 5.1, 7.1.4, 7.0.2 and LCR as its channels on their own", wrong);
}

void testRangeTiming (Ctx& t)
{
    // Two tones 20 dB over quiet noise, 151 Hz in Tub and 247 Hz in Fog, on until 4 s. Fog's
    // timing is unlocked; Tub's stays locked to Speed and Release.
    const double offSec = 4.0;
    auto signal = [&] (int nch, double onSec = 0.0) {
        Audio a = toneOverNoise (9.0, nch, 246.94, -12.0, -36.0, 2401, onSec, offSec);
        const Audio b = toneOverNoise (9.0, nch, 155.56, -12.0, -90.0, 2402, onSec, offSec);
        for (int c = 0; c < nch; ++c)
            for (std::size_t i = 0; i < a.length(); ++i)
                a.ch[(std::size_t) c][i] += b.ch[(std::size_t) c][i];
        return a;
    };
    struct Got
    {
        double tauMs = 0.0, attackMs = 0.0, settled = 0.0;
    };
    auto measure = [&] (const RenderResult& r, double hz, double onSec = 0.0) {
        const auto band = (std::size_t) nearestBand (r.bandHz, hz);
        Got g;
        double ratio = 1.0, period = 0.0;
        std::size_t counted = 0;
        for (std::size_t f = 1; f < r.frames.size(); ++f)
        {
            const double a = r.frames[f - 1].cutDb[band], b = r.frames[f].cutDb[band], tf = r.frames[f].timeSec;
            if (r.frames[f].counter == r.frames[f - 1].counter + 1)
            {
                period += tf - r.frames[f - 1].timeSec;
                ++counted;
                if (tf > offSec && a > 0.2 && b > 0.0 && b < a)
                    ratio = std::min (ratio, b / a);
            }
            if (tf >= offSec - 0.6 && tf < offSec - 0.1)
                g.settled = std::max (g.settled, b);
        }
        period /= (double) std::max<std::size_t> (1, counted);
        g.tauMs = ratio < 1.0 ? -1000.0 * period / std::log (ratio) : 0.0;
        for (const auto& f : r.frames)
            if (g.attackMs == 0.0 && f.timeSec > onSec && (double) f.cutDb[band] >= 0.5 * g.settled && g.settled > 0.0)
                g.attackMs = 1000.0 * (f.timeSec - onSec);
        return g;
    };
    auto spec = [&] (const dehonk::Params& p, int nch) {
        RenderSpec s = specForChannels (p, nch);
        s.useTuning = true;
        s.tuning = noParentTuning();
        s.tuning.notchesPerModule = 8.0f;
        s.collectFrames = true;
        s.frameEveryTick = true;
        s.blockSize = 64;
        return s;
    };
    dehonk::Params base = alwaysOn();
    base.depth = 1.0f;
    base.releaseMs = 150.0f;
    base.impactGuard = 0.0f;

    int wrong = 0;
    double worstErr = 0.0;
    for (int nch : { 1, 2, 6, 12 })
    {
        const Audio in = signal (nch);
        const auto plain = render (in, spec (base, nch));

        // Locked: the range's own values do nothing, whatever they are.
        {
            dehonk::Params p = base;
            for (int m = 0; m < dehonk::kNumModules; ++m)
            {
                p.rangeAttackMs[m] = 2.0f + 30.0f * (float) m;
                p.rangeReleaseMs[m] = 1500.0f - 400.0f * (float) m;
            }
            const auto r = render (in, spec (p, nch));
            std::size_t d = 0;
            for (int c = 0; c < nch; ++c)
                d += samplesDiffering (r.out, c, plain.out, c);
            wrong += d == 0 ? 0 : 1;
            t.check (d == 0, fmt ("%s: every range locked, with other values of its own set: bit-identical to the defaults (%zu samples differ)", channelsName (nch), d));
        }
        // Fog unlocked with a release of 1000 ms: Fog's notch lets go at 1000 ms, Tub's still at Release (150 ms).
        {
            dehonk::Params p = base;
            p.rangeTimingLocked[dehonk::Fog] = false;
            p.rangeReleaseMs[dehonk::Fog] = 1000.0f;
            p.rangeAttackMs[dehonk::Fog] = 14.0f;
            const auto r = render (in, spec (p, nch));
            const Got fog = measure (r, 246.94), tub = measure (r, 155.56), tubPlain = measure (plain, 155.56), fogPlain = measure (plain, 246.94);
            const double eFog = 100.0 * (fog.tauMs - 1000.0) / 1000.0, eTub = 100.0 * (tub.tauMs - 150.0) / 150.0, ePlain = 100.0 * (fogPlain.tauMs - 150.0) / 150.0;
            worstErr = std::max ({ worstErr, std::abs (eFog), std::abs (eTub) });
            const bool ok = std::abs (eFog) < 2.0 && std::abs (eTub) < 2.0 && std::abs (ePlain) < 2.0 && fog.settled > 3.0 && tub.settled > 3.0;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%s, Release 150 ms, Fog unlocked at 1000 ms: Fog's notch (%.1f dB) falls with a time constant of %.1f ms (%+.2f%%), Tub's (%.1f dB) with %.1f ms (%+.2f%%); locked, Fog's fell with %.1f ms (limit 2%% each)",
                              channelsName (nch), fog.settled, fog.tauMs, eFog, tub.settled, tub.tauMs, eTub, fogPlain.tauMs));
            (void) tubPlain;
        }
        // Fog unlocked with an attack of 100 ms and of 2 ms: it comes in later and sooner; Tub does
        // not move. The tones start at 1 s, once the Threshold's own opening (which follows Speed)
        // is over, and at a Depth and Max Cut at which the notch does not reach the ceiling, so
        // that how fast it comes in is the attack and not how far over the ceiling it is asked.
        {
            const double onSec = 1.0;
            const Audio late = signal (nch, onSec);
            dehonk::Params locked = base;
            locked.depth = 0.2f;
            locked.maxCutDb = 12.0f;
            dehonk::Params slow = locked, fast = locked;
            slow.rangeTimingLocked[dehonk::Fog] = fast.rangeTimingLocked[dehonk::Fog] = false;
            slow.rangeReleaseMs[dehonk::Fog] = fast.rangeReleaseMs[dehonk::Fog] = 150.0f;
            slow.rangeAttackMs[dehonk::Fog] = 100.0f;
            fast.rangeAttackMs[dehonk::Fog] = 2.0f;
            const auto rl = render (late, spec (locked, nch)), rs = render (late, spec (slow, nch)), rf = render (late, spec (fast, nch));
            const Got fs = measure (rs, 246.94, onSec), ff = measure (rf, 246.94, onSec), fl = measure (rl, 246.94, onSec);
            const Got ts = measure (rs, 155.56, onSec), tf = measure (rf, 155.56, onSec), tl = measure (rl, 155.56, onSec);
            const bool ok = fs.attackMs > fl.attackMs + 30.0 && ff.attackMs < fl.attackMs - 3.0 && std::abs (ts.attackMs - tl.attackMs) < 1.5 && std::abs (tf.attackMs - tl.attackMs) < 1.5
                         && fl.settled < 11.9 && fl.settled > 3.0;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%s: Fog's notch (%.1f dB, under a 12 dB ceiling) is at half depth %.0f ms after its tone starts with its attack at 100 ms, %.0f ms at 2 ms, %.0f ms locked (Speed 50, about 14 ms): 30 ms later and 3 ms sooner at least; Tub's at %.0f, %.0f and %.0f ms (must not move)",
                              channelsName (nch), fl.settled, fs.attackMs, ff.attackMs, fl.attackMs, ts.attackMs, tf.attackMs, tl.attackMs));
        }
    }

    // Snap keeps its own timing: with only Snap on, unlocking every range changes nothing.
    {
        const auto clicky = clickSignal (kSr, 2500.0, 3.0, 2, 4.0, 2403);
        dehonk::Params p = clickOnly();
        RenderSpec s;
        s.params = p;
        const auto a = render (clicky.in, s);
        for (int m = 0; m < dehonk::kNumModules; ++m)
        {
            s.params.rangeTimingLocked[m] = false;
            s.params.rangeAttackMs[m] = 100.0f;
            s.params.rangeReleaseMs[m] = 50.0f;
        }
        const auto b = render (clicky.in, s);
        const auto d = samplesDiffering (a.out, 0, b.out, 0) + samplesDiffering (a.out, 1, b.out, 1);
        const double took = residualDb (a.out, clicky.in, 1.0, S (0.5), clicky.in.length());
        wrong += d == 0 && took > -80.0 ? 0 : 1;
        t.check (d == 0 && took > -80.0, fmt ("only Snap on: every range unlocked at 100 ms and 50 ms changes nothing (%zu samples differ); Snap itself takes out %s dB", d, dbStr (took).c_str()));
    }

    // Switching the locks and sweeping the times while it plays makes no click.
    for (int nch : { 1, 2, 6, 12 })
    {
        Audio in = surroundSignal (kSr, 6.0, nch, 2404, -14.0);
        for (int c = 0; c < nch; ++c)
        {
            const auto tone = sine (kSr, in.length(), 200.0, dbToAmp (-14.0));
            for (std::size_t i = 0; i < in.length(); ++i)
                in.ch[(std::size_t) c][i] += tone[i];
        }
        dehonk::Params p = alwaysOn();
        p.depth = 1.0f;
        for (auto& on : p.moduleOn)
            on = true;
        RenderSpec s = specForChannels (p, nch);
        s.useTuning = true;
        s.tuning = noParentTuning();
        s.blockSize = 32;
        s.automate = [] (std::int64_t pos, dehonk::Params& q) {
            const double sec = (double) pos / kSr;
            for (int m = 0; m < dehonk::kNumModules; ++m)
            {
                q.rangeTimingLocked[m] = std::fmod (sec + 0.13 * m, 0.5 + 0.1 * m) < 0.25;
                q.rangeAttackMs[m] = (float) (2.0 * std::pow (50.0, 0.5 + 0.5 * std::sin (6.283 * (0.7 + 0.2 * m) * sec)));
                q.rangeReleaseMs[m] = (float) (50.0 * std::pow (30.0, 0.5 + 0.5 * std::sin (6.283 * (0.9 + 0.3 * m) * sec)));
            }
        };
        const auto r = render (in, s);
        const std::vector<int> skip = nch > 2 ? std::vector<int> { 3 } : std::vector<int> {};
        const double hf = worstUnexplained (in, r.out, skip);
        wrong += hf < -40.0 ? 0 : 1;
        t.check (hf < -40.0, fmt ("%s: every range's lock switched and its attack and release swept: unexplained energy above 6 kHz, worst 10 ms window on any channel %s dB (limit -40)",
                                  channelsName (nch), dbStr (hf).c_str()));
    }
    t.headline = fmt ("%d checks wrong; an unlocked range's release within %.2f%% of its own setting (limit 2%%), the locked ranges unmoved", wrong, worstErr);
}

void testDialogueSidechain (Ctx& t)
{
    // The mix: quiet pink noise, a 155.6 Hz tone (in Tub) that is never in the key, a quiet tone
    // at 244 Hz (in Fog) that is the mix's own, and "dialogue": a tone at 250 Hz, in the same
    // analysis band as the 244 Hz one and 12 dB over it, from 3 s to 6 s. The key is the dialogue
    // alone, with a little of the 155.6 Hz band in it, 15 dB under the mix's tone there (a voice
    // has something everywhere).
    //   before 3 s  nothing in the key: both tones are cut as without a key
    //   3 s to 6 s  the 247 Hz band is the dialogue's: its cut goes, the 155.6 Hz cut stays
    //   after 6 s   the dialogue has left: the cut on the mix's own 244 Hz tone comes back
    const double from = 3.0, to = 6.0, len = 9.0;
    const std::size_t n = S (len);
    auto gate = [&] (std::vector<float> x) {
        for (std::size_t i = 0; i < n; ++i)
        {
            const double sec = (double) i / kSr;
            x[i] *= (float) (std::clamp ((sec - from) / 0.01, 0.0, 1.0) * std::clamp ((to - sec) / 0.01, 0.0, 1.0));
        }
        return x;
    };
    auto pink = pinkNoise (kSr, n, 2501);
    normaliseRms (pink, -40.0);
    const auto low = sine (kSr, n, 155.56, dbToAmp (-12.0));
    const auto own = sine (kSr, n, 244.0, dbToAmp (-20.0));
    const auto voice = gate (sine (kSr, n, 250.0, dbToAmp (-8.0)));
    const auto voiceLow = gate (sine (kSr, n, 155.56, dbToAmp (-27.0)));
    std::vector<float> mixCh (n), keyCh (n);
    for (std::size_t i = 0; i < n; ++i)
    {
        keyCh[i] = voice[i] + voiceLow[i];
        mixCh[i] = pink[i] + low[i] + own[i] + voice[i];
    }

    dehonk::Params base = alwaysOn();
    base.depth = 1.0f;
    base.impactGuard = 0.0f;       // the voice coming in is not what is being measured
    auto spec = [&] (const dehonk::Params& p, int nch) {
        RenderSpec s = specForChannels (p, nch);
        s.useTuning = true;
        s.tuning = noParentTuning();
        s.collectFrames = true;
        s.blockSize = 64;
        return s;
    };
    auto meanCut = [] (const RenderResult& r, double hz, double a, double b) {
        const auto band = (std::size_t) nearestBand (r.bandHz, hz);
        double s = 0.0;
        std::size_t c = 0;
        for (const auto& f : r.frames)
            if (f.timeSec >= a && f.timeSec < b)
            {
                s += f.cutDb[band];
                ++c;
            }
        return c > 0 ? s / (double) c : 0.0;
    };

    int wrong = 0;
    double worstLeft = 0.0;
    for (int nch : { 1, 2, 6, 12 })
    {
        const Audio in = copies (mixCh, kSr, nch);
        const Audio key = fromChannels (kSr, { keyCh });
        const auto plain = render (in, spec (base, nch));

        dehonk::Params on = base;
        on.dialogueOn = true;
        RenderSpec ks = spec (on, nch);
        ks.key = &key;
        const auto keyed = render (in, ks);

        const double voiceBefore = meanCut (plain, 246.94, from + 0.5, to), voiceKeyed = meanCut (keyed, 246.94, from + 0.5, to);
        const double lowBefore = meanCut (plain, 155.56, from + 0.5, to), lowKeyed = meanCut (keyed, 155.56, from + 0.5, to);
        const double earlyBefore = meanCut (plain, 246.94, 1.5, from), earlyKeyed = meanCut (keyed, 246.94, 1.5, from);
        const double lateBefore = meanCut (plain, 246.94, to + 1.5, len), lateKeyed = meanCut (keyed, 246.94, to + 1.5, len);
        worstLeft = std::max (worstLeft, voiceKeyed);
        bool ok = voiceBefore > 3.0 && voiceKeyed < 0.1;
        wrong += ok ? 0 : 1;
        t.check (ok, fmt ("%s: while the dialogue is in the 247 Hz band, the cut there is %.2f dB without the key (need over 3) and %.3f dB with it (limit 0.1)", channelsName (nch), voiceBefore, voiceKeyed));
        ok = lowBefore > 3.0 && lowKeyed > 0.9 * lowBefore;
        wrong += ok ? 0 : 1;
        t.check (ok, fmt ("%s: the 155.6 Hz tone, 15 dB over the dialogue in its band, is cut %.2f dB without the key and %.2f dB with it (need 90%% of it)", channelsName (nch), lowBefore, lowKeyed));
        ok = earlyBefore > 1.0 && std::abs (earlyKeyed - earlyBefore) < 0.02 && lateBefore > 1.0 && lateKeyed > 0.9 * lateBefore;
        wrong += ok ? 0 : 1;
        t.check (ok, fmt ("%s: the mix's own 244 Hz tone is cut %.2f dB before the dialogue comes in (%.2f without the key) and %.2f dB from 1.5 s after it has left (%.2f without the key, need 90%% of it)",
                          channelsName (nch), earlyKeyed, earlyBefore, lateKeyed, lateBefore));
        // In the audio as well: the level at 250 Hz while the dialogue speaks.
        {
            const double a = bandLevelDb (powerSpectrum (channelOf (in, 0), S (from + 0.5), S (to)), 250.0);
            const double p = bandLevelDb (powerSpectrum (channelOf (plain.out, 0), S (from + 0.5), S (to)), 250.0);
            const double k = bandLevelDb (powerSpectrum (channelOf (keyed.out, 0), S (from + 0.5), S (to)), 250.0);
            ok = p - a < -3.0 && std::abs (k - a) < 0.1;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%s, in the output: the dialogue's 250 Hz is %+.2f dB without the key and %+.3f dB with it (limit 0.1)", channelsName (nch), p - a, k - a));
        }
        // How fast: the cut is under half a dB within 100 ms of the dialogue arriving.
        {
            const auto band = (std::size_t) nearestBand (keyed.bandHz, 246.94);
            double goneAt = -1.0, shownMax = 0.0;
            bool keyedFlag = true;
            for (const auto& f : keyed.frames)
            {
                if (goneAt < 0.0 && f.timeSec > from && f.cutDb[band] < 0.5f)
                    goneAt = f.timeSec - from;
                if (f.timeSec > from + 0.5 && f.timeSec < to)
                    shownMax = std::max (shownMax, (double) f.dialogue[band]);
                keyedFlag = keyedFlag && f.keyed;
            }
            bool plainFlag = false;
            for (const auto& f : plain.frames)
                plainFlag = plainFlag || f.keyed;
            ok = goneAt >= 0.0 && goneAt < 0.1 && shownMax > 0.95 && keyedFlag && ! plainFlag;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%s: the cut is under 0.5 dB %.0f ms after the dialogue arrives (limit 100); the frame shows the band as the dialogue's (%.2f, need over 0.95) and keyed in every frame (%s), never without a key (%s)",
                              channelsName (nch), 1000.0 * goneAt, shownMax, keyedFlag ? "yes" : "no", plainFlag ? "no" : "yes"));
        }
        // Amount 50%: half of the cut is left.
        {
            dehonk::Params half = on;
            half.dialogueAmount = 0.5f;
            RenderSpec hs = spec (half, nch);
            hs.key = &key;
            const auto r = render (in, hs);
            const double c = meanCut (r, 246.94, from + 1.0, to);
            ok = c > 0.45 * voiceBefore && c < 0.55 * voiceBefore;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%s, Dialogue Amount 50%%: the cut on the dialogue's band is %.2f dB, of %.2f without the key (between 45%% and 55%% of it)", channelsName (nch), c, voiceBefore));
        }
        // Dialogue off with a key, or on with a silent key: exactly as with no key at all.
        {
            RenderSpec offSpec = spec (base, nch);
            offSpec.key = &key;
            const auto offKeyed = render (in, offSpec);
            const Audio silent = Audio::make (kSr, 1, n);
            RenderSpec silentSpec = spec (on, nch);
            silentSpec.key = &silent;
            const auto onSilent = render (in, silentSpec);
            std::size_t d = 0;
            for (int c = 0; c < nch; ++c)
                d += samplesDiffering (offKeyed.out, c, plain.out, c) + samplesDiffering (onSilent.out, c, plain.out, c);
            wrong += d == 0 ? 0 : 1;
            t.check (d == 0, fmt ("%s: Dialogue off with the key connected, and Dialogue on with a silent key: both bit-identical to no key (%zu samples differ)", channelsName (nch), d));
        }
        // A key that arrives 6 dB hot (a stereo stem summed to mono) is turned down to fit.
        {
            Audio hot = key;
            for (auto& v : hot.ch[0])
                v *= 2.0f;
            RenderSpec hs = spec (on, nch);
            hs.key = &hot;
            const auto r = render (in, hs);
            double trim = 0.0;
            for (const auto& f : r.frames)
                if (f.timeSec > to - 1.0 && f.timeSec < to - 0.1)
                    trim = f.dialogueTrimDb;
            const double lowHot = meanCut (r, 155.56, from + 1.5, to), voiceHot = meanCut (r, 246.94, from + 1.5, to);
            ok = trim < -4.5 && trim > -7.5 && lowHot > 0.9 * lowBefore && voiceHot < 0.1;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%s, key 6 dB hot: the gain it is taken at reads %.2f dB after 2 s (need -4.5 to -7.5); the 155.6 Hz tone is still cut %.2f dB (%.2f without the key, need 90%%) and the dialogue's band %.3f dB (limit 0.1)",
                              channelsName (nch), trim, lowHot, lowBefore, voiceHot));
        }
        // A key that arrives 12 dB quiet, or 24 dB hot, is found in the mix and taken at the level
        // the dialogue has there.
        for (double offDb : { -12.0, 24.0 })
        {
            Audio off = key;
            for (auto& v : off.ch[0])
                v *= (float) dbToAmp (offDb);
            RenderSpec hs = spec (on, nch);
            hs.key = &off;
            const auto r = render (in, hs);
            double gain = 0.0, found = 0.0;
            bool flagged = false;
            for (const auto& f : r.frames)
                if (f.timeSec > to - 1.0 && f.timeSec < to - 0.1)
                {
                    gain = f.dialogueTrimDb;
                    found = f.keyMatch;
                    flagged = flagged || f.keyOver;
                }
            const double lowOff = meanCut (r, 155.56, from + 2.0, to), voiceOff = meanCut (r, 246.94, from + 2.0, to);
            ok = std::abs (gain + offDb) < 1.5 && found > 0.9 && ! flagged && lowOff > 0.9 * lowBefore && voiceOff < 0.1;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%s, key %+.0f dB: taken at %+.2f dB (need within 1.5 of %+.0f), found in the mix %.2f (need over 0.9), not flagged (%s); from 2 s after the dialogue arrives the 155.6 Hz tone is cut %.2f dB (%.2f without the key, need 90%%) and the dialogue's band %.3f dB (limit 0.1)",
                              channelsName (nch), offDb, gain, -offDb, found, flagged ? "no" : "yes", lowOff, lowBefore, voiceOff));
        }
        // A key that has nothing to do with the mix is not found in it; the mix itself as the key is flagged.
        {
            auto other = pinkNoise (kSr, n, 2504);
            normaliseRms (other, -30.0);
            const Audio unrelated = fromChannels (kSr, { other });
            RenderSpec us = spec (on, nch);
            us.key = &unrelated;
            const auto ru = render (in, us);
            const Audio whole = fromChannels (kSr, { mixCh });
            RenderSpec ws = spec (on, nch);
            ws.key = &whole;
            const auto rw = render (in, ws);
            double foundMost = 0.0;
            for (const auto& f : ru.frames)
                if (f.timeSec > 2.0)
                    foundMost = std::max (foundMost, (double) f.keyMatch);
            std::size_t over = 0, all = 0;
            for (const auto& f : rw.frames)
                if (f.timeSec > 4.0)
                {
                    ++all;
                    over += f.keyOver ? 1 : 0;
                }
            const double cutWhole = meanCut (rw, 155.56, 4.0, len);
            ok = foundMost < 0.2 && all > 0 && 10 * over >= 8 * all && cutWhole < 0.1;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%s: unrelated noise on the key is found in the mix %.2f at most (limit 0.2); with the mix itself as the key nothing is cut (%.3f dB) and the frame says the key is most of the mix in %zu of %zu frames after 4 s (need 80%%)",
                              channelsName (nch), foundMost, cutWhole, over, all));
        }
        // Snap: a click that is in the key (a consonant) keeps its peak; the same click when it is
        // not in the key is still taken down.
        {
            const auto sig = clickSignal (kSr, 2500.0, 3.0, nch);
            const auto bare = burstSignal (kSr, 10.0, 1, BurstKind::tone, 2500.0, 3.0, 0.5, -200.0, 401);
            Audio snapKey = Audio::make (kSr, 1, sig.in.length());
            BurstSignal inKey = sig, notInKey = sig;
            inKey.starts.clear();
            notInKey.starts.clear();
            for (std::size_t b = 0; b < sig.starts.size(); ++b)
            {
                const std::size_t st = sig.starts[b];
                if (b % 2 == 0)
                {
                    inKey.starts.push_back (st);
                    for (std::size_t i = st > S (0.002) ? st - S (0.002) : 0; i < std::min (st + S (0.05), sig.in.length()); ++i)
                        snapKey.ch[0][i] = bare.in.ch[0][i];
                }
                else
                    notInKey.starts.push_back (st);
            }
            dehonk::Params snap = clickOnly();
            RenderSpec ps;
            ps.params = snap;
            if (nch > 2)
                ps = surroundSpec (snap, layoutNamed (nch == 6 ? "5.1" : "7.1.4"));
            const auto plainSnap = render (sig.in, ps);
            RenderSpec kp = ps;
            kp.params.dialogueOn = true;
            kp.key = &snapKey;
            const auto keyedSnap = render (sig.in, kp);
            const auto keptPlain = burstChange (inKey, plainSnap.out), kept = burstChange (inKey, keyedSnap.out);
            const auto cutPlain = burstChange (notInKey, plainSnap.out), cut = burstChange (notInKey, keyedSnap.out);
            ok = -keptPlain.bandMean > 3.0 && std::abs (kept.bandMean) < 0.5 && std::abs (kept.bandMin) < 1.0 && -cut.bandMean > 0.9 * -cutPlain.bandMean;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%s, Snap: 2.5 kHz bursts that are in the key change by %+.2f dB of band peak (%+.2f at most; limits 0.5 and 1; %.2f dB taken off without the key); those that are not lose %.2f dB (%.2f without the key, need 90%%)",
                              channelsName (nch), kept.bandMean, kept.bandMin, -keptPlain.bandMean, -cut.bandMean, -cutPlain.bandMean));
        }
        // A stereo key with the dialogue on one side only counts just the same.
        if (nch >= 2)
        {
            const Audio oneSide = fromChannels (kSr, { std::vector<float> (n, 0.0f), keyCh });
            RenderSpec ss = spec (on, nch);
            ss.key = &oneSide;
            const auto r = render (in, ss);
            const double c = meanCut (r, 246.94, from + 0.5, to);
            ok = c < 0.1;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%s, stereo key with the dialogue in its second channel only: the cut on the dialogue's band is %.3f dB (limit 0.1)", channelsName (nch), c));
        }
        // Block sizes, and a hostile key.
        {
            Rng rng (2502);
            RenderSpec a = spec (on, nch), b = spec (on, nch);
            a.key = b.key = &key;
            a.blockSize = 512;
            b.maxBlockSize = 1024;
            b.nextBlockSize = [&rng] { return rng.uniformInt (1, 1024); };
            const auto ra = render (in, a), rb = render (in, b);
            std::size_t d = 0;
            for (int c = 0; c < nch; ++c)
                d += samplesDiffering (ra.out, c, rb.out, c);
            Audio bad = key;
            bad.ch[0][n / 3] = std::numeric_limits<float>::quiet_NaN();
            bad.ch[0][n / 2] = std::numeric_limits<float>::infinity();
            bad.ch[0][n / 2 + 7] = -1.0e30f;
            RenderSpec hs = spec (on, nch);
            hs.key = &bad;
            const auto rh = render (in, hs);
            bool finite = true;
            for (const auto& ch : rh.out.ch)
                for (float v : ch)
                    finite = finite && std::isfinite (v);
            const double after = meanCut (rh, 155.56, to + 1.0, len);
            ok = d == 0 && finite && after > 3.0;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%s: blocks of 512 and random blocks of 1 to 1024 give the same output (%zu samples differ); with NaN, infinity and 1e30 in the key the output is finite (%s) and the cut carries on afterwards (%.2f dB)",
                              channelsName (nch), d, finite ? "yes" : "no", after));
        }
        // Switching Dialogue and sweeping its Amount while the dialogue speaks makes no click.
        {
            std::vector<float> longMix (S (8.0)), longKey (S (8.0));
            auto pk = pinkNoise (kSr, longMix.size(), 2503);
            normaliseRms (pk, -30.0);
            const auto tone = sine (kSr, longMix.size(), 250.0, dbToAmp (-10.0));
            for (std::size_t i = 0; i < longMix.size(); ++i)
            {
                longKey[i] = tone[i];
                longMix[i] = tone[i] + pk[i];
            }
            const Audio lin = copies (longMix, kSr, nch), lkey = fromChannels (kSr, { longKey });
            for (int mode = 0; mode < 2; ++mode)
            {
                RenderSpec cs = spec (on, nch);
                cs.key = &lkey;
                cs.blockSize = 32;
                cs.collectFrames = true;
                if (mode == 0)
                    cs.automate = [] (std::int64_t pos, dehonk::Params& q) { q.dialogueOn = std::fmod ((double) pos / kSr, 1.4) >= 0.7; };
                else
                    cs.automate = [] (std::int64_t pos, dehonk::Params& q) { q.dialogueAmount = (float) (0.5 + 0.5 * std::sin (6.283 * 0.9 * (double) pos / kSr)); };
                const auto r = render (lin, cs);
                const std::vector<int> skip = nch > 2 ? std::vector<int> { 3 } : std::vector<int> {};
                const double hf = worstUnexplained (lin, r.out, skip);
                const double moved = cutStats (r, 0.5).maxTotal;
                ok = hf < -40.0 && moved > 3.0;
                wrong += ok ? 0 : 1;
                t.check (ok, fmt ("%s: %s: unexplained energy above 6 kHz, worst 10 ms window on any channel %s dB (limit -40); the cut it moves reaches %.1f dB (need over 3)",
                                  channelsName (nch), mode == 0 ? "Dialogue switched every 0.7 s" : "Dialogue Amount swept 0 to 100%", dbStr (hf).c_str(), moved));
            }
        }
    }

    // Unlinked and Grouped in 5.1 with the dialogue only in the centre, and the key the dialogue:
    // the centre's band is left alone, and a channel that has the mix's own tone there and no
    // dialogue keeps its cut while the dialogue speaks (the key is not found in that channel).
    for (auto mode : { dehonk::LinkMode::unlinked, dehonk::LinkMode::grouped })
    {
        const auto& l = layoutNamed ("5.1");
        Audio in = Audio::make (kSr, l.size(), n);
        for (int c = 0; c < l.size(); ++c)
            for (std::size_t i = 0; i < n; ++i)
                in.ch[(std::size_t) c][i] = c == l.lfe() ? 0.0f : pink[i] + low[i] + (c == 2 ? voice[i] : own[i] * 4.0f);
        const Audio key = fromChannels (kSr, { keyCh });
        dehonk::Params on = base;
        on.dialogueOn = true;
        on.linkMode = mode;
        RenderSpec ks = surroundSpec (on, l);
        ks.useTuning = true;
        ks.tuning = noParentTuning();
        ks.key = &key;
        const auto keyed = render (in, ks);
        RenderSpec ps = ks;
        ps.key = nullptr;
        ps.params.dialogueOn = false;
        const auto plain = render (in, ps);
        auto level = [&] (const Audio& a, int c, double hz, double a0, double a1) { return bandLevelDb (powerSpectrum (channelOf (a, c), S (a0), S (a1)), hz); };
        const double cPlain = level (plain.out, 2, 250.0, from + 0.5, to) - level (in, 2, 250.0, from + 0.5, to);
        const double cKeyed = level (keyed.out, 2, 250.0, from + 0.5, to) - level (in, 2, 250.0, from + 0.5, to);
        const double lLate = level (keyed.out, 0, 244.0, to + 1.5, len) - level (in, 0, 244.0, to + 1.5, len);
        const double lLatePlain = level (plain.out, 0, 244.0, to + 1.5, len) - level (in, 0, 244.0, to + 1.5, len);
        // While the dialogue speaks, from 1.5 s in (the key has been found by then): the left and
        // the left surround keep the cut on their own tone.
        const double lDuring = level (keyed.out, 0, 244.0, from + 1.5, to) - level (in, 0, 244.0, from + 1.5, to);
        const double lDuringPlain = level (plain.out, 0, 244.0, from + 1.5, to) - level (in, 0, 244.0, from + 1.5, to);
        const double sDuring = level (keyed.out, 4, 244.0, from + 1.5, to) - level (in, 4, 244.0, from + 1.5, to);
        const double sDuringPlain = level (plain.out, 4, 244.0, from + 1.5, to) - level (in, 4, 244.0, from + 1.5, to);
        const bool ok = cPlain < -3.0 && std::abs (cKeyed) < 0.1 && lLatePlain < -3.0 && lLate < 0.9 * lLatePlain
                        && lDuringPlain < -3.0 && lDuring < 0.9 * lDuringPlain && sDuringPlain < -3.0 && sDuring < 0.9 * sDuringPlain;
        wrong += ok ? 0 : 1;
        t.check (ok, fmt ("5.1 %s, dialogue in the centre: its 250 Hz is %+.2f dB without the key and %+.3f dB with it (limit 0.1); while it speaks the left channel's own 244 Hz tone is %+.2f dB (%+.2f without the key, need 90%%) and the left surround's %+.2f dB (%+.2f); once the dialogue has left the left's is %+.2f dB (%+.2f)",
                          mode == dehonk::LinkMode::unlinked ? "Unlinked" : "Grouped", cPlain, cKeyed, lDuring, lDuringPlain, sDuring, sDuringPlain, lLate, lLatePlain));
    }
    t.headline = fmt ("%d checks wrong; a band that is the dialogue's keeps at most %.3f dB of cut (limit 0.1), a tone 15 dB over the dialogue keeps its cut, mono to 7.1.4", wrong, worstLeft);
}

//==============================================================================
// wav_io (harness self-check)

void testWavIo (Ctx& t)
{
    namespace fs = std::filesystem;
    const fs::path dir = gScratchDir / "dehonk_tests_wav_io";
    std::error_code ec;
    fs::create_directories (dir, ec);
    const std::size_t n = 4801;
    Audio a = Audio::make (44100.0, 2, n);
    a.ch[0] = sine (44100.0, n, 441.0, 0.7);
    a.ch[1] = sine (44100.0, n, 1000.0, -0.5);
    a.ch[1][10] = 1.0f;
    a.ch[1][11] = -1.0f;

    struct Fmt
    {
        WavFormat f;
        double tol;
        const char* name;
    };
    for (const auto& c : { Fmt { { 16, false, false }, 1.0 / 32768.0, "16-bit PCM" }, Fmt { { 24, false, false }, 1.0 / 8388608.0, "24-bit PCM" },
                           Fmt { { 32, true, false }, 0.0, "32-bit float" }, Fmt { { 24, false, true }, 1.0 / 8388608.0, "24-bit PCM extensible" },
                           Fmt { { 32, true, true }, 0.0, "32-bit float extensible" } })
    {
        const std::string path = (dir / fmt ("rt_%d_%d_%d.wav", c.f.bitsPerSample, (int) c.f.isFloat, (int) c.f.extensible)).string();
        std::string err;
        Audio b;
        WavFormat got;
        const bool w = writeWav (path, a, c.f, err);
        const bool r = w && readWav (path, b, err, &got);
        double maxErr = 1e9;
        if (r && b.numChannels() == 2 && b.length() == n)
        {
            Audio clipped = a;
            clipped.ch[1][10] = (float) (c.f.isFloat ? 1.0 : (c.f.bitsPerSample == 16 ? 32767.0 / 32768.0 : 8388607.0 / 8388608.0));
            maxErr = maxAbsDiff (b, clipped, 1.0, 0, n);
        }
        const bool ok = r && b.sampleRate == 44100.0 && got.bitsPerSample == c.f.bitsPerSample && got.isFloat == c.f.isFloat
                        && got.extensible == c.f.extensible && maxErr <= c.tol;
        t.check (ok, fmt ("%s: write %s, read %s, %d ch, %zu frames, %.0f Hz, max error %.3g (limit %.3g)%s%s", c.name, w ? "ok" : "FAILED",
                          r ? "ok" : "FAILED", b.numChannels(), b.length(), b.sampleRate, maxErr, c.tol, err.empty() ? "" : " - ",
                          err.c_str()));
        fs::remove (path, ec);
    }
    fs::remove_all (dir, ec);
    t.headline = "WAV write/read round trip";
}

void testTuningFields (Ctx& t)
{
    const bool ok = tuningFieldsComplete();
    t.check (ok, fmt ("dehonk::Tuning has %zu fields; Tests/src/TuningFields.h lists %zu%s", kTuningFieldCount, kListedTuningFieldCount,
                      ok ? "" : ": a field was added to Tuning, add it to the lists so --set can reach it"));
    // Self-check of the counter, so a counter that always agrees cannot pass the test above.
    struct Probe
    {
        float a = 0.0f;
        bool b = false, c = false;
    };
    t.check (detail::aggregateFieldCount<Probe>() == 3,
             fmt ("field counter self-check: a struct of one float and two bools counts %zu fields (expect 3; sizeof alone could not tell)",
                  detail::aggregateFieldCount<Probe>()));
    // The Snap module's eight fields are listed and each one reaches its own field.
    {
        const char* names[] = { "clickLowHz", "clickHighHz", "clickMarginDb", "clickNeighbourDb", "clickSlope", "clickMaxCutDb", "clickBodyMs", "clickReleaseMs" };
        const dehonk::Tuning d = defaultTuning();
        int reached = 0;
        std::string bad;
        for (const char* name : names)
        {
            dehonk::Tuning tu = d;
            std::string err;
            const bool set = setTuningField (tu, name, "1234.5", err);
            const float got[] = { tu.clickLowHz, tu.clickHighHz, tu.clickMarginDb, tu.clickNeighbourDb, tu.clickSlope, tu.clickMaxCutDb, tu.clickBodyMs, tu.clickReleaseMs };
            const float was[] = { d.clickLowHz, d.clickHighHz, d.clickMarginDb, d.clickNeighbourDb, d.clickSlope, d.clickMaxCutDb, d.clickBodyMs, d.clickReleaseMs };
            int changed = 0, which = -1;
            for (int k = 0; k < 8; ++k)
                if (got[k] != was[k])
                {
                    ++changed;
                    which = k;
                }
            const bool good = set && changed == 1 && std::string (names[which < 0 ? 0 : which]) == name && got[which] == 1234.5f;
            reached += good ? 1 : 0;
            if (! good)
                bad += std::string (" ") + name;
        }
        t.check (reached == 8, fmt ("Snap's tuning fields: %d of 8 set by name to their own field%s%s", reached, bad.empty() ? "" : "; wrong:", bad.c_str()));
    }
    t.headline = ok ? "every Tuning field reachable with --set, Snap's eight included" : "TuningFields.h is missing a field";
}

//==============================================================================
// steady_detection (SPEC 13.19)

void testSteadyDetection (Ctx& t)
{
    // Threshold at Always throughout, so the programme level (a fold-down with more than two
    // channels) does not come into it.
    int wrong = 0;
    std::string head;
    const double seconds = 10.0, settle = 4.0;
    auto steadyParams = [] () {
        dehonk::Params p = alwaysOn();
        p.detection = dehonk::Detection::steady;
        return p;
    };
    for (int nch : { 1, 2, 6, 12 })
    {
        const Audio in = surroundSignal (kSr, seconds, nch, 3100 + (std::uint64_t) nch, -14.0);
        const int lfe = nch > 2 ? layoutNamed (nch == 6 ? "5.1" : "7.1.4").lfe() : -1;

        // 1. The peaks at 151 and 247 Hz are cut by Steady as by Fast, once it has settled.
        RenderSpec s = specForChannels (steadyParams(), nch);
        s.collectFrames = true;
        const auto r = render (in, s);
        RenderSpec fs = s;
        fs.params.detection = dehonk::Detection::fast;
        const auto rf = render (in, fs);
        bool finite = true;
        for (const auto& c : r.out.ch)
            for (float v : c)
                finite = finite && std::isfinite (v);
        double cutS[2], cutF[2];
        int i = 0;
        for (double hz : { 151.0, 247.0 })
        {
            const auto [k0, k1] = bandsAround (r.bandHz, hz);
            cutS[i] = std::max (meanBandCut (r, k0, settle), meanBandCut (r, k1, settle));
            cutF[i] = std::max (meanBandCut (rf, k0, settle), meanBandCut (rf, k1, settle));
            ++i;
        }
        // Steady cuts less often than Fast, so less on average; it must still find both.
        const bool cuts = cutS[0] >= 1.0 && cutS[1] >= 1.0 && cutS[0] >= 0.4 * cutF[0] && cutS[1] >= 0.4 * cutF[1];
        wrong += cuts && finite ? 0 : 1;
        t.check (finite, fmt ("%s, Steady: every sample finite", channelsName (nch)));
        t.check (cuts, fmt ("%s, Steady, linked: peaks 8 dB proud at 151 and 247 Hz cut %.2f and %.2f dB on average from %.0f s; Fast cuts them %.2f and %.2f dB (need 1 dB each and 40%% of Fast's)",
                            channelsName (nch), cutS[0], cutS[1], settle, cutF[0], cutF[1]));

        // 2. Unlinked, every channel is exactly that channel on its own (mono, Steady).
        {
            dehonk::Params p = steadyParams();
            p.linkMode = dehonk::LinkMode::unlinked;
            RenderSpec us = specForChannels (p, nch);
            const auto ru = render (in, us);
            std::size_t d = 0;
            for (int c = 0; c < nch; ++c)
            {
                if (c == lfe)
                    continue;
                RenderSpec ms;
                ms.params = p;
                d += samplesDiffering (ru.out, c, render (channelOf (in, c), ms).out, 0);
            }
            wrong += d == 0 ? 0 : 1;
            t.check (d == 0, fmt ("%s, Steady, Unlinked: each channel bit-identical to it alone in mono: %zu samples differ", channelsName (nch), d));
        }

        // 3. The output does not depend on the block size.
        {
            std::size_t d = 0;
            for (int bs : { 1, 7, 4096 })
            {
                RenderSpec bsSpec = specForChannels (steadyParams(), nch);
                bsSpec.blockSize = bs;
                bsSpec.maxBlockSize = 4096;
                const auto rb = render (in, bsSpec);
                for (int c = 0; c < nch; ++c)
                    d += samplesDiffering (rb.out, c, r.out, c);
            }
            wrong += d == 0 ? 0 : 1;
            t.check (d == 0, fmt ("%s, Steady: blocks of 1, 7 and 4096 bit-identical to blocks of 512: %zu samples differ", channelsName (nch), d));
        }

        // 4. Switching between Fast and Steady every 0.7 s, and every 60 ms, in blocks of 32: no
        //    click. Measured as automation_clicks does: above 6 kHz each channel must be the input
        //    times a slowly changing gain; whatever a fit of that cannot explain, against the
        //    output's own energy there, worst 10 ms window, limit -40 dB. (Each notch moves at its
        //    own attack and release whichever mode asks for it.)
        for (double period : { 0.7, 0.06 })
        {
            RenderSpec ws = s;
            ws.blockSize = 32;
            ws.automate = [period] (std::int64_t pos, dehonk::Params& p) {
                p.detection = std::fmod ((double) pos / kSr, 2.0 * period) >= period ? dehonk::Detection::steady : dehonk::Detection::fast;
            };
            const auto rw = render (in, ws);
            double worst = -400.0;
            for (int c = 0; c < nch; ++c)
            {
                if (c == lfe)
                    continue;
                const auto [m, at] = hfUnexplained (highpass4 (in.ch[(std::size_t) c], kSr, 6000.0), highpass4 (rw.out.ch[(std::size_t) c], kSr, 6000.0), S (0.5), S (0.010));
                (void) at;
                worst = std::max (worst, m);
            }
            const bool ok = worst < -40.0;
            wrong += ok ? 0 : 1;
            t.check (ok, fmt ("%s, Detection switched every %.2f s: unexplained energy above 6 kHz, worst 10 ms window %s dB (limit -40)",
                              channelsName (nch), period, dbStr (worst).c_str()));
        }
        head += fmt ("%s%s %.1f/%.1f dB", head.empty() ? "" : "; ", channelsName (nch), cutS[0], cutS[1]);
    }

    // 5. Grouped: in 5.1 and 7.1.4 the front pair comes out exactly as a stereo instance of it.
    for (const char* name : { "5.1", "7.1.4" })
    {
        const auto& l = layoutNamed (name);
        const Audio in = surroundSignal (kSr, 6.0, l.size(), 3200, -14.0);
        dehonk::Params p = steadyParams();
        p.linkMode = dehonk::LinkMode::grouped;
        const auto r = render (in, surroundSpec (p, l));
        const auto front = groupMembers (l, dehonk::Group::front);
        std::vector<std::vector<float>> chans;
        for (int c : front)
            chans.push_back (in.ch[(std::size_t) c]);
        RenderSpec gs;
        gs.params = p;
        gs.params.linkMode = dehonk::LinkMode::linked;
        const auto alone = render (fromChannels (kSr, chans), gs);
        std::size_t d = 0;
        for (std::size_t i = 0; i < front.size(); ++i)
            d += samplesDiffering (r.out, front[i], alone.out, (int) i);
        wrong += d == 0 ? 0 : 1;
        t.check (d == 0, fmt ("%s, Steady, Grouped: the front pair bit-identical to a stereo instance of it: %zu samples differ", name, d));
    }

    // 6. Fast is the core as it was: covered bit for bit by golden_defaults and golden_mono_stereo,
    //    and against the core from before Detection by dehonk_golden_ref4 (Tests/README.md).
    t.headline = fmt ("%d checks wrong; Steady cuts the 151 / 247 Hz peaks %s; Unlinked exact against mono, Grouped against stereo, block-size invariant, switching makes no click", wrong, head.c_str());
}

const std::vector<TestDef>& allTests()
{
    static const std::vector<TestDef> tests = {
        { "null_depth_zero", "Depth 0 output equals the latency-delayed input (pink and music, mono and stereo, lookahead off and 10 ms)", testNullDepthZero },
        { "notch_shape", "One fixed notch at a time (debugSetStaticCuts): depth at the centre, width at half depth, flat beyond an octave; two notches a quarter octave apart", testNotchShape },
        { "transparency_pink", "Pink noise: shallow notches and no long-term level change per band when always acting; no cut at all below the default threshold", testTransparencyPink },
        { "transparency_sweep", "A slow lone sine sweep (no subharmonic parent): the cut at the sweep's own frequency stays under 1 dB", testTransparencySweep },
        { "below_modules_untouched", "Bass with harmonics above: level below the lowest module that is on unchanged (Tub+Fog: below 100 Hz; Fog only: below 180 Hz)", testBelowModulesUntouched },
        { "effect_on_peaks", "Sustained narrow peaks over a bass-heavy bed are notched, centred on the peak, with the surroundings left alone; peaks in Bark's range only with Bark on", testEffectOnPeaks },
        { "parent_rule_protects_fundamental", "A harmonic tone's fundamental inside a module is cut with the parent rule off and spared with it on", testParentRule },
        { "notch_count", "Many peaks: never more than notchesPerModule notches per module (deeper than 1 dB, within 1 dB of their 100 ms maximum; bands within the spacing count as one notch being handed over); two within the spacing add to no more than Max Cut + 0.25 dB; never more filters running than notches per module x modules + 2", testNotchCount },
        { "modules_independent", "Tub and Fog cut the same with Woof and Bark on or off; an off module cuts nothing in its range; moving an edge moves a peak to the other module", testModulesIndependent },
        { "threshold", "No cut 3 dB below the threshold, the full cut from 6 dB above it, rising steadily between", testThreshold },
        { "notch_dwell", "Informational: how long a notch stays on one band, and how quickly it follows a peak that moves", testNotchDwell },
        { "noise_is_left_alone", "SPEC 13.3: dense noise 80-600 Hz, 8 dB above the threshold, is barely cut; a resonance 6 dB proud of it is cut at least 3 dB; and the noise is cut no more than before Release was a control", testNoiseIsLeftAlone },
        { "steady_noise_is_left_alone", "SPEC 13.3 and 13.19: the same at Detection Steady, from 4 s in: dense noise barely cut, a resonance 6 dB proud of it cut at least 3 dB, and no more cut than the ceiling Fast is held to", testSteadyNoiseIsLeftAlone },
        { "impacts_keep_body_tail_front", "SPEC 13.2: ten impacts over a bed keep their 110-400 Hz Body (1 dB), Tail (10%) and Front (1 dB, every Speed); always acting, the impact itself keeps its Body and Front (1 dB) and its Tail within 25%, and loses them with Impact Guard off", testImpactsKeepBodyTailFront },
        { "headroom", "Informational, SPEC 13.6: a reference limiter after De-Honk and after the untreated signal; gain reduction, peak and short-term level", testHeadroom },
        { "key_profile", "SPEC 13.4 step 1: the note profile finds C minor; every notch logged against it (the log is informational)", testKeyProfile },
        { "solo_honk_identity", "Normal + Solo Honk = trimmed input; Diff and Bypass output the delayed input exactly", testSoloHonkIdentity },
        { "latency", "An impulse comes out at latencySamples() for each lookahead and sample rate", testLatency },
        { "block_size_invariance", "Output does not depend on block size (1, 7, 32, 512, 4096 and random)", testBlockSizeInvariance },
        { "stability", "Hostile inputs, random automation (modules and edges included, out of range and crossing), all rates, mono and stereo, prepare/reset mid-stream", testStability },
        { "automation_clicks", "Smooth automation of depth, edges, threshold, mix, trim and link, module switches, the link mode, Detection and each range's timing, and switching Diff (slowly, inside its own cross-fade, with Mix moving) and Bypass, makes no clicks", testAutomationClicks },
        { "sample_rate_consistency", "The cut for one signal agrees across 44.1, 48, 96 and 192 kHz", testSampleRateConsistency },
        { "snap_reduces_clicks", "Snap: short 2.5, 2.8 and 3.5 kHz bursts over noise lose at least 4, 2.5 and 2.5 dB of band peak at default Depth, more at 100%, none at 0; the bed between them untouched", testClickReducesClicks },
        { "snap_leaves_hits_alone", "Snap: broadband, snare-like and kick-like hits keep their band peak (within 1 dB) and whole-signal peak (within 0.3 dB)", testClickLeavesHitsAlone },
        { "snap_leaves_steady_alone", "Snap: a sustained or tremolo 2.5 kHz tone and pink noise at -18 and -3 dBFS keep their level", testClickLeavesSteadyAlone },
        { "snap_level_independent", "Snap: the same clicks 20 dB quieter are reduced the same (within 1 dB) at the default threshold", testClickLevelIndependent },
        { "snap_is_band_limited", "Snap: what it takes out is at least 20 dB down below 600 Hz and above 10 kHz relative to 2.5 kHz", testClickIsBandLimited },
        { "snap_no_latency_and_identities", "Snap: no added latency; off is bit-identical to never on; Solo Honk, Diff and Bypass identities hold with it on", testClickNoLatencyAndIdentities },
        { "cpu_benchmark", "CPU percentage against SPEC section 6 with Tub+Fog and with all four modules, and silence/denormal cost; worst block time checked only with --strict-cpu", testCpuBenchmark },
        { "no_allocation_in_process", "No operator new or delete inside process(), setParams(), setTuning() or reset(), at any block size or setting", testNoAllocation },
        { "golden_mono_stereo", "At Max Cut 8 dB, the old release, Impact Guard 0 and the old notch movement (Tuning::handOver off, which no control reaches) the core is bit-identical to the cores before (Tests/golden/): mono and stereo, through the plain prepare() and the plug-in's layouts, 11 settings at 44.1/48/96 kHz; 5.1 and 7.1.4; output and display frames (nothing with Diff engaged, and not the offset Diff reports: those changed on purpose, SPEC 13.17)", testGoldenMonoStereo },
        { "golden_defaults", "At the plug-in's defaults the core is bit-identical to the hashes recorded from it (Tests/golden/): mono, stereo, 5.1 and 7.1.4, output and display frames with the guard's reading; every plain case cuts, and three more reach Max Cut and bring the guard up", testGoldenDefaults },
        { "surround_linked_matches_stereo", "7.1.4, the same signal everywhere, Link 100%: every channel but the LFE comes out as stereo does, display frames included", testSurroundLinkedMatchesStereo },
        { "surround_lfe", "The LFE is the input delayed by the latency (times Trim; silent in Solo Honk; as it is in Diff and Bypass) at every lookahead, and the others do not depend on it", testSurroundLfe },
        { "surround_unlinked", "Link 0%: each channel exactly as on its own in mono; a silent channel stays silent and does not affect the others", testSurroundUnlinked },
        { "surround_threshold_level", "The Threshold reads the stereo fold-down: L/R-only 5.1 reads as stereo, a centre as a phantom centre, 7.1.4 noise as the fold-down predicts", testSurroundThresholdLevel },
        { "surround_block_size_invariance", "7.1.4 output does not depend on block size (1, 7, 32, 4096, random), Link 100% and 50%, Snap on", testSurroundBlockSizeInvariance },
        { "surround_latency", "7.1.4: an impulse in every channel, LFE included, comes out at latencySamples() for each lookahead", testSurroundLatency },
        { "surround_stability", "5.1 and 7.1.4: hostile input, random automation, prepare() with another layout and reset() mid-stream", testSurroundStability },
        { "surround_recovery", "5.1 and 7.1.4: one NaN, infinity or huge sample leaves every channel finite and Diff's level match working; a short call of LFE channels only does not break the level match", testSurroundRecovery },
        { "surround_channel_counts", "Every channel count from 1 to 12, with its layout and without: finite, every channel but the LFE treated, the LFE exact", testSurroundChannelCounts },
        { "max_cut_is_a_ceiling", "Max Cut: no band of any frame is deeper, at 3, 6, 9 and 12 dB, mono, stereo, 5.1 and 7.1.4, linked and not; two notches within the spacing never add to more; a tone falls by Max Cut and no more; turning it down is followed within 200 ms", testMaxCut },
        { "release_time", "Release: the rate a notch lets go at on a gated tone matches the setting within 2% from 50 to 1500 ms, and the whole time from the tone stopping fits it, mono, stereo, 5.1 and 7.1.4; the attack does not depend on it and still follows Speed; under the Threshold every notch is shut within 500 ms whatever the Release", testReleaseTime },
        { "impact_guard", "Impact Guard: up within 15 ms of a broadband burst, the open notch eased off (in the audio too) and back afterwards, half at 50%, bit-identical to no guard at 0%; no new notch while it is up; not for a tone or a narrow burst, briefly for an entry; in a busy passage never over its limit and the tone still cut; not blinded by one absurd sample; no clicks when easing or switched; mono, stereo, 5.1 and 7.1.4", testImpactGuard },
        { "surround_cpu", "CPU percentage for stereo, 5.1, 7.1 and 7.1.4 at 48 and 96 kHz: defaults, and all four modules + Snap always acting at Link 100%, 50% and 0% (informational)", testSurroundCpu },
        { "link_modes", "Link: Unlinked is bit-identical to what Link 0% was and Grouped to Linked in mono and stereo; with more channels each group (front pair, centre, surrounds, heights) comes out as its channels on their own, linked; the LFE untouched; switching modes makes no click; 1, 2, 6 and 12 channels", testLinkModes },
        { "range_timing", "Each range's own timing: locked it changes nothing (bit-identical); unlocked, that range's notch lets go at its own release within 2% and comes in at its own attack while the other ranges still follow Speed and Release; Snap is not affected; switching and sweeping makes no click; 1, 2, 6 and 12 channels", testRangeTiming },
        { "dialogue_sidechain", "Dialogue sidechain: a band the key accounts for loses its cut (0.1 dB left, within 100 ms) and a tone 15 dB over the key keeps it; the mix's own tone is cut again once the dialogue has left; off or with a silent key it is bit-identical to no key; a key 6 dB hot, 24 dB hot or 12 dB quiet is taken at the level the dialogue has in the mix; an unrelated key is not found and the mix itself as the key is flagged; Snap leaves a click that is in the key alone and still cuts one that is not; in 5.1 Unlinked and Grouped a channel without the dialogue keeps its cut; Amount, a stereo key, block sizes, a hostile key, no clicks when switched; 1, 2, 6 and 12 channels", testDialogueSidechain },
        { "steady_detection", "Detection Steady (SPEC 13.19): peaks still cut at 1, 2, 6 and 12 channels; Unlinked exact against mono and Grouped against stereo; block-size invariant; switching Fast and Steady makes no click", testSteadyDetection },
        { "wav_io", "Harness self-check: WAV writer and reader round trip (16/24-bit, float, extensible)", testWavIo },
        { "tuning_fields", "Harness self-check: every dehonk::Tuning field can be set by name with --set", testTuningFields },
    };
    return tests;
}

int writeSignals (const std::string& dirPath)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories (dirPath, ec);
    struct Item
    {
        const char* file;
        Audio audio;
        WavFormat f;
    };
    Audio bed;
    const Audio peaks = peaksSignal (kSr, 10.0, 2, { 151.0, 247.0 }, -12.0, 51, &bed);
    std::vector<double> rbass = { 0.0, -17.0, -22.0, -40.0, -42.0, -60.0, -55.0, -65.0 };
    std::vector<double> saw (12);
    for (std::size_t h = 0; h < saw.size(); ++h)
        saw[h] = -20.0 * std::log10 ((double) h + 1.0);
    std::vector<Item> items;
    items.push_back ({ "peaks_151_247hz_stereo_24bit.wav", peaks, { 24, false, false } });
    items.push_back ({ "peaks_bed_only_stereo_24bit.wav", bed, { 24, false, false } });
    items.push_back ({ "peaks_500_900hz_stereo_float.wav", peaksSignal (kSr, 10.0, 2, { 500.0, 900.0 }, -12.0, 52), { 32, true, false } });
    items.push_back ({ "dense_12_peaks_mono_float.wav", denseSignal (12.0, 141), { 32, true, false } });
    items.push_back ({ "pink_-18dBFS_stereo_16bit.wav", pinkAudio (kSr, S (15.0), 2, -18.0, 21), { 16, false, false } });
    items.push_back ({ "sweep_20-5000hz_mono_float_extensible.wav", fromChannels (kSr, { logSweep (kSr, S (30.0), 20.0, 5000.0, dbToAmp (-12.0)) }), { 32, true, true } });
    items.push_back ({ "bass_50hz_rbass_mono_24bit_extensible.wav", fromChannels (kSr, { harmonicTone (kSr, S (8.0), 50.0, rbass, 0.25, 31) }), { 24, false, true } });
    items.push_back ({ "tone_160hz_mono_24bit.wav", fromChannels (kSr, { harmonicTone (kSr, S (8.0), 160.0, saw, 0.25, 61) }), { 24, false, false } });
    items.push_back ({ "music_like_stereo_24bit.wav", musicAudio (kSr, S (10.0), 2, 12), { 24, false, false } });
    items.push_back ({ "clicks_2500hz_3ms_over_pink_mono_float.wav", clickSignal (kSr, 2500.0, 3.0).in, { 32, true, false } });
    items.push_back ({ "hits_broadband_6ms_over_pink_mono_float.wav", burstSignal (kSr, 10.0, 1, BurstKind::broadband, 0.0, 6.0, 0.5, -22.0, 411).in,
                       { 32, true, false } });
    items.push_back ({ "impacts_over_bed_mono_float.wav", impactSignal (1313).in, { 32, true, false } });
    {
        Audio noise = bandNoise (20.0, 1301);
        scaleTo (noise, (double) dehonk::Params {}.thresholdDb + 8.0);
        items.push_back ({ "noise_80-600hz_8db_over_threshold_stereo_float.wav", noise, { 32, true, false } });
    }
    for (const auto& it : items)
    {
        std::string err;
        const std::string path = (fs::path (dirPath) / it.file).string();
        if (! writeWav (path, it.audio, it.f, err))
        {
            std::fprintf (stderr, "dehonk_tests: %s\n", err.c_str());
            return 1;
        }
        std::printf ("wrote %s\n", path.c_str());
    }
    return 0;
}

void usage()
{
    std::printf ("Usage: dehonk_tests [--list] [--only NAME[,NAME...]] [--report out.md] [--strict-cpu] [--no-ftz]\n"
                 "                    [--write-signals DIR] [--set NAME=VALUE ...]\n"
                 "--set changes a dehonk::Tuning constant for every render that does not set its own tuning,\n"
                 "to check that a test fails when the core is set up wrongly.\n");
}
} // namespace

int main (int argc, char** argv)
{
    std::vector<std::string> args (argv + 1, argv + argc);
    {
        std::error_code ec;
        const auto exe = std::filesystem::absolute (argv[0], ec);
        if (! ec && exe.has_parent_path() && std::filesystem::is_directory (exe.parent_path(), ec))
            gScratchDir = exe.parent_path();
    }
    std::set<std::string> only;
    std::string reportPath, signalsDir;
    dehonk::Tuning overrideTuning = defaultTuning();
    std::string overrideText;
    bool strictCpu = false, ftz = true, list = false;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const auto& a = args[i];
        auto value = [&] () -> std::string {
            if (i + 1 >= args.size())
            {
                std::fprintf (stderr, "dehonk_tests: %s needs a value\n", a.c_str());
                std::exit (2);
            }
            return args[++i];
        };
        if (a == "--list") list = true;
        else if (a == "--only")
        {
            std::string v = value();
            std::size_t p = 0;
            while (p <= v.size())
            {
                const auto c = v.find (',', p);
                const auto name = v.substr (p, c == std::string::npos ? std::string::npos : c - p);
                if (! name.empty())
                    only.insert (name);
                if (c == std::string::npos)
                    break;
                p = c + 1;
            }
        }
        else if (a == "--report") reportPath = value();
        else if (a == "--strict-cpu") strictCpu = true;
        else if (a == "--no-ftz") ftz = false;
        else if (a == "--write-signals") signalsDir = value();
        else if (a == "--set")
        {
            const std::string kv = value();
            const auto eq = kv.find ('=');
            std::string err;
            if (eq == std::string::npos || ! setTuningField (overrideTuning, kv.substr (0, eq), kv.substr (eq + 1), err))
            {
                std::fprintf (stderr, "dehonk_tests: --set %s: %s\nTuning fields: %s\n", kv.c_str(), eq == std::string::npos ? "expected NAME=VALUE" : err.c_str(),
                              tuningFieldNames().c_str());
                return 2;
            }
            overrideText += (overrideText.empty() ? "" : " ") + kv;
            setTuningOverride (&overrideTuning);
        }
        else if (a == "-h" || a == "--help")
        {
            usage();
            return 0;
        }
        else
        {
            std::fprintf (stderr, "dehonk_tests: unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    if (list)
    {
        for (const auto& t : allTests())
            std::printf ("%-34s %s\n", t.name, t.what);
        return 0;
    }
    if (ftz)
        enableFlushDenormals();
    if (! signalsDir.empty())
        return writeSignals (signalsDir);

    for (const auto& name : only)
        if (std::none_of (allTests().begin(), allTests().end(), [&] (const TestDef& t) { return name == t.name; }))
        {
            std::fprintf (stderr, "dehonk_tests: no test named %s (see --list)\n", name.c_str());
            return 2;
        }

    std::vector<Ctx> results;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& def : allTests())
    {
        if (! only.empty() && ! only.count (def.name))
            continue;
        Ctx c;
        c.name = def.name;
        c.strictCpu = strictCpu;
        std::printf ("\n[ RUN  ] %s - %s\n", def.name, def.what);
        std::fflush (stdout);
        const auto t0 = std::chrono::steady_clock::now();
        def.fn (c);
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("[ %s ] %s: %s (%.1f s)\n", c.failed ? "FAIL" : "PASS", def.name, c.headline.c_str(), secs);
        results.push_back (std::move (c));
    }
    const double total = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();

    int failed = 0;
    std::printf ("\n==== Summary ====\n");
    for (const auto& r : results)
    {
        failed += r.failed ? 1 : 0;
        std::printf ("%s  %-34s %s\n", r.failed ? "FAIL" : "PASS", r.name.c_str(), r.headline.c_str());
    }
    std::printf ("%d passed, %d failed, %.1f s%s%s\n", (int) results.size() - failed, failed, total, ftz ? "" : " (flush-to-zero off)",
                 overrideText.empty() ? "" : (" (tuning changed: " + overrideText + ")").c_str());

    if (! reportPath.empty())
    {
        std::ofstream f (reportPath);
        if (! f)
        {
            std::fprintf (stderr, "dehonk_tests: cannot write %s\n", reportPath.c_str());
            return 1;
        }
        const std::time_t now = std::time (nullptr);
        char when[64];
        std::strftime (when, sizeof (when), "%Y-%m-%d %H:%M", std::localtime (&now));
        f << "# De-Honk test report\n\n";
        f << fmt ("Run %s. %d passed, %d failed. Flush-to-zero %s. CPU checks %s.%s\n\n", when, (int) results.size() - failed, failed,
                  ftz ? "on" : "off", strictCpu ? "strict" : "informational",
                  overrideText.empty() ? "" : (" Tuning changed with --set: " + overrideText + ".").c_str());
        f << "| Test | Result | Measured |\n|---|---|---|\n";
        for (const auto& r : results)
            f << "| " << r.name << " | " << (r.failed ? "FAIL" : "PASS") << " | " << r.headline << " |\n";
        f << "\n";
        for (const auto& r : results)
        {
            f << "## " << r.name << " - " << (r.failed ? "FAIL" : "PASS") << "\n\n```\n";
            for (const auto& l : r.lines)
                f << l << "\n";
            f << "```\n\n";
        }
        std::printf ("report written to %s\n", reportPath.c_str());
    }
    return failed == 0 ? 0 : 1;
}

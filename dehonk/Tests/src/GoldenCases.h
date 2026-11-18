// A fixed set of mono and stereo renders, each reduced to one 64-bit hash of every output sample
// and every display frame, bit for bit. Used to prove that a change to the core leaves mono and
// stereo exactly as they were.
//
// Header-only and using nothing but the public API of DeHonkCore.h that has not changed (the
// three-argument prepare()), so the same cases can be built against an older copy of the core:
// the dehonk_golden_ref tool does that with a copy of dsp/ placed in the build folder as ref-dsp/
// and writes golden_ref.txt. The hashes of the core before surround support are kept in
// Tests/golden/, one file per architecture; the golden_mono_stereo test runs the same cases on the
// current core and compares, both through the three-argument prepare() and (by passing its own
// `prepare` to runAll) through the layouts the plug-in really passes. Signals are made here from
// a fixed seed, independent of the rest of the harness.
#pragma once

#include "DeHonkCore.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace golden
{

struct Result
{
    std::string name;
    std::uint64_t hash = 0;
};

struct Hasher
{
    std::uint64_t h = 1469598103934665603ull;
    void bytes (const void* p, std::size_t n)
    {
        const auto* b = static_cast<const unsigned char*> (p);
        for (std::size_t i = 0; i < n; ++i)
        {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    void f (float v) { bytes (&v, sizeof v); }
    void i (std::int64_t v) { bytes (&v, sizeof v); }
};

/** Deterministic noise. */
struct Noise
{
    std::uint64_t s;
    explicit Noise (std::uint64_t seed) : s (seed * 0x9E3779B97F4A7C15ull + 1) {}
    double uniform()   // -1 .. 1
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (double) (s >> 11) * (2.0 / 9007199254740992.0) - 1.0;
    }
};

/** Programme-like test material for one channel: pinkish noise, a few honky tones with slow
    vibrato, and short 2.5 kHz bursts for Snap, under an envelope that climbs from about -40 to
    about -4 dBFS and back, so the threshold is crossed both ways. `variant` changes the noise
    and the balance of the tones per channel. With `hostile`, NaN, infinity, huge values, a DC
    step and a stretch of silence are written in. With `impacts` (the cases of the current
    defaults only), the 302 Hz tone is six times as strong, so that its notch reaches Max Cut,
    and a broadband burst with a 150 ms decay lands every 0.9 s, so that the impact guard comes up. */
inline std::vector<float> signal (double sr, double seconds, int variant, bool hostile, bool impacts = false)
{
    const std::size_t n = (std::size_t) std::llround (seconds * sr);
    std::vector<float> x (n);
    Noise rng (1000u + (std::uint64_t) variant);
    double b0 = 0, b1 = 0, b2 = 0;
    const double tones[] = { 151.0, 247.0, 302.0, 700.0, 95.0 };
    const double amps[] = { 0.30, 0.22 + 0.05 * variant, 0.12, 0.10 - 0.03 * variant, 0.20 };
    const double twoPi = 6.283185307179586;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        const double w = rng.uniform();
        b0 = 0.99765 * b0 + w * 0.0990460;
        b1 = 0.96300 * b1 + w * 0.2965164;
        b2 = 0.57000 * b2 + w * 1.0526913;
        double v = 0.18 * (b0 + b1 + b2 + w * 0.1848);
        for (int k = 0; k < 5; ++k)
            v += amps[k] * std::sin (twoPi * tones[k] * t * (1.0 + 0.004 * std::sin (twoPi * 0.3 * t + k)) + 0.7 * k + variant);
        const double ph = std::fmod (t, 0.37);
        if (ph < 0.003)
            v += 0.8 * std::sin (twoPi * 2500.0 * ph) * std::sin (3.14159265 * ph / 0.003);
        if (impacts)
        {
            v += 5.0 * amps[2] * std::sin (twoPi * tones[2] * t * (1.0 + 0.004 * std::sin (twoPi * 0.3 * t + 2)) + 1.4 + variant);
            const double since = std::fmod (t + 0.45, 0.9);
            v += 1.2 * rng.uniform() * std::min (1.0, since / 0.001) * std::exp (-since / 0.150);
        }
        const double env = std::pow (10.0, (-40.0 + 36.0 * std::sin (3.14159265 * std::min (1.0, t / seconds))) / 20.0);
        x[i] = (float) (v * env * 1.6);
    }
    if (hostile && n > 10)
    {
        x[n / 7] = std::numeric_limits<float>::quiet_NaN();
        x[n / 5] = std::numeric_limits<float>::infinity();
        x[n / 5 + 3] = -std::numeric_limits<float>::infinity();
        x[n / 4] = 1.0e20f;
        for (std::size_t i = n / 3; i < n / 3 + n / 20; ++i)
            x[i] += 0.5f;    // DC step
        for (std::size_t i = n / 2; i < n / 2 + n / 10; ++i)
            x[i] = 0.0f;     // silence
    }
    return x;
}

/** The settings at which the core must still give exactly what it gave before Max Cut, Release
    and Impact Guard became controls: the ceiling that used to be fixed (8 dB), the release the
    Speed control used to set along with the attack, no guard, and the notch hand-over as it was.
    On a core from before those controls (which has none of the fields) these do nothing. */
inline constexpr float kCompatMaxCutDb = 8.0f;

/** The release the old Speed mapping gave (300 ms at Speed 0 down to 50 ms at Speed 1), as the
    float that gives the very same smoothing coefficient at this sample rate. The old code worked
    the time out in double from Speed; a release in float milliseconds can be one rounding step
    away, so the neighbours are tried too. */
inline float compatReleaseMs (float speed, double sr)
{
    const float s = std::clamp (speed, 0.0f, 1.0f);
    const double tickRate = sr / (double) (std::max (1, (int) std::floor (sr / 8000.0)) * 8);
    auto coef = [tickRate] (double seconds) { return (float) (1.0 - std::exp (-1.0 / std::max (1.0e-6, seconds * tickRate))); };
    const float old = coef (0.001 * 300.0f * std::pow (50.0f / 300.0f, s));
    float r = 300.0f * std::pow (50.0f / 300.0f, s);
    float lo = r, hi = r;
    for (int i = 0; i < 16; ++i)
    {
        if (coef (0.001 * (double) lo) == old) return lo;
        if (coef (0.001 * (double) hi) == old) return hi;
        lo = std::nextafter (lo, 0.0f);
        hi = std::nextafter (hi, 1.0e9f);
    }
    return r;
}

template <typename P>
inline void compatParams (P& p, double sr)
{
    if constexpr (requires { p.maxCutDb; p.releaseMs; p.impactGuard; })
    {
        p.maxCutDb = kCompatMaxCutDb;
        p.releaseMs = compatReleaseMs (p.speed, sr);
        p.impactGuard = 0.0f;
    }
}

template <typename T>
inline void compatTuning (T& t)
{
    if constexpr (requires { t.handOver; })
        t.handOver = false;
}

struct Case
{
    const char* name;
    dehonk::Params params;
    bool useTuning = false;
    dehonk::Tuning tuning;
    bool varyBlocks = false;
    bool automate = false;
    bool key = false;
    bool hostile = false;
    bool staticCuts = false;
    bool impacts = false;
};

/** With `extended`, the cases that only make sense for the current core at its defaults are
    added: they need Max Cut and the impact guard to be there. The reference cores are run
    without them. */
inline std::vector<Case> cases (bool extended = false)
{
    std::vector<Case> v;
    auto add = [&] (const char* name) -> Case& { v.push_back ({}); v.back().name = name; return v.back(); };
    add ("defaults");
    { auto& c = add ("always_all_modules_snap_depth1"); c.params.thresholdDb = -60.0f; for (auto& m : c.params.moduleOn) m = true; c.params.snapOn = true; c.params.depth = 1.0f; c.varyBlocks = true; }
    { auto& c = add ("link50_sharp_fast_la5"); c.params.thresholdDb = -60.0f; c.params.link = 0.5f; c.params.sharpness = 0.9f; c.params.speed = 1.0f; c.params.lookaheadMs = 5.0f; }
    { auto& c = add ("link0_snap_la20"); c.params.thresholdDb = -60.0f; c.params.link = 0.0f; c.params.snapOn = true; c.params.lookaheadMs = 20.0f; c.varyBlocks = true; }
    { auto& c = add ("solo_mix70_trim-3"); c.params.thresholdDb = -60.0f; c.params.soloHonk = true; c.params.mix = 0.7f; c.params.trimDb = -3.0f; }
    { auto& c = add ("diff"); c.params.thresholdDb = -60.0f; c.params.diff = true; c.params.depth = 0.9f; }
    { auto& c = add ("bypass"); c.params.bypass = true; c.params.lookaheadMs = 10.0f; }
    { auto& c = add ("mix40_trim6_detrend"); c.params.thresholdDb = -60.0f; c.params.mix = 0.4f; c.params.trimDb = 6.0f; c.useTuning = true; c.tuning.detrend = true; c.tuning.referenceExponent = 0.7f; }
    { auto& c = add ("automation"); c.automate = true; c.varyBlocks = true; c.key = true; }
    { auto& c = add ("key"); c.key = true; c.params.keyOn = true; c.params.thresholdDb = -20.0f; c.params.snapOn = true; }
    { auto& c = add ("hostile"); c.hostile = true; c.params.thresholdDb = -60.0f; c.params.snapOn = true; c.params.link = 0.3f; c.varyBlocks = true; }
    { auto& c = add ("static_cuts"); c.staticCuts = true; c.params.lookaheadMs = 5.0f; }
    if (extended)
    {
        // The plain "defaults" case only just crosses the Threshold. These two are at the
        // defaults of the three controls and make them work: a lower Threshold, so the cut is
        // deep for most of the render; and a tone strong enough to reach Max Cut with a burst
        // every 0.9 s, so the ceiling holds and the guard comes up, unlinked as well as linked.
        { auto& c = add ("defaults_threshold-34"); c.params.thresholdDb = -34.0f; }
        { auto& c = add ("impacts_and_ceiling"); c.params.thresholdDb = -40.0f; c.params.depth = 1.0f; c.impacts = true; }
        { auto& c = add ("impacts_and_ceiling_link0"); c.params.thresholdDb = -40.0f; c.params.depth = 1.0f; c.params.link = 0.0f; c.impacts = true; c.varyBlocks = true; }
    }
    return v;
}

/** The three-argument prepare(), which every version of the core has. */
struct PlainPrepare
{
    void operator() (dehonk::Core& core, double sr, int maxBlock, int nch) const { core.prepare (sr, maxBlock, nch); }
};

/** Renders one case and hashes every output sample and every display frame. */
template <typename F>
inline void hashGuard (Hasher& h, const F& f)
{
    if constexpr (requires { f.guard; })
        h.f (f.guard);
}

/** What a render reached, for the self-check that the extended cases do exercise the controls. */
struct Reached
{
    float deepestCutDb = 0.0f, guardMax = 0.0f;
};

template <typename F>
inline void noteGuard (Reached& r, const F& f)
{
    if constexpr (requires { f.guard; })
        r.guardMax = std::max (r.guardMax, f.guard);
}

template <typename Prepare = PlainPrepare>
inline std::uint64_t run (const Case& cs, double sr, int nch, double seconds, const Prepare& prepare = {}, bool compat = false, bool extended = false,
                          Reached* reached = nullptr)
{
    const int maxBlock = 1024;
    dehonk::Core core;
    prepare (core, sr, maxBlock, nch);
    if (cs.useTuning || compat)
    {
        dehonk::Tuning t = core.tuning();
        if (cs.useTuning)
        {
            t.detrend = cs.tuning.detrend;
            t.referenceExponent = cs.tuning.referenceExponent;
        }
        if (compat)
            compatTuning (t);
        core.setTuning (t);
    }
    dehonk::Params p = cs.params;
    if (compat)
        compatParams (p, sr);
    core.setParams (p);
    core.reset();
    float staticCut[dehonk::kMaxBands] {};
    if (cs.staticCuts)
    {
        for (int k = 0; k < core.numBands(); ++k)
            staticCut[k] = (k % 9 == 4) ? 3.0f + (float) (k % 5) : 0.0f;
        core.debugSetStaticCuts (staticCut);
    }

    std::vector<std::vector<float>> in;
    for (int c = 0; c < nch; ++c)
        in.push_back (signal (sr, seconds, c, cs.hostile, cs.impacts));
    std::vector<std::vector<float>> keySig;
    if (cs.key)
        for (int c = 0; c < nch; ++c)
        {
            keySig.push_back (signal (sr, seconds, 7 + c, false));
            for (auto& s : keySig.back())
                s *= 0.5f;
        }

    const std::size_t n = in[0].size();
    std::vector<std::vector<float>> buf ((std::size_t) nch, std::vector<float> ((std::size_t) maxBlock));
    std::vector<float*> ptrs ((std::size_t) nch);
    std::vector<const float*> keyPtrs ((std::size_t) nch);
    Hasher h;
    h.i (core.latencySamples());
    std::size_t pos = 0;
    std::uint64_t blockRng = 77;
    int block = 0;
    while (pos < n)
    {
        int bs = 512;
        if (cs.varyBlocks)
        {
            blockRng = blockRng * 6364136223846793005ull + 1442695040888963407ull;
            bs = 1 + (int) ((blockRng >> 33) % (std::uint64_t) maxBlock);
        }
        bs = (int) std::min<std::size_t> ((std::size_t) bs, n - pos);
        if (cs.automate)
        {
            const double t = (double) pos / sr;
            p.depth = (float) (0.5 + 0.5 * std::sin (t * 1.3));
            p.link = (float) std::clamp (1.2 * std::cos (t * 0.9), 0.0, 1.0);
            p.lookaheadMs = ((block / 40) % 3 == 0) ? 0.0f : ((block / 40) % 3 == 1 ? 10.0f : 5.0f);
            p.soloHonk = (block / 23) % 4 == 1;
            p.diff = (block / 31) % 5 == 2;
            p.snapOn = (block / 17) % 2 == 0;
            p.keyOn = (block / 50) % 2 == 1;
            p.mix = (float) (0.75 + 0.25 * std::sin (t * 2.1));
            p.thresholdDb = (float) (-30.0 + 10.0 * std::sin (t * 0.7));
            p.moduleOn[0] = (block / 60) % 2 == 1;
            p.edgeHz[2] = (float) (200.0 + 40.0 * std::sin (t));
            core.setParams (p);
        }
        for (int c = 0; c < nch; ++c)
        {
            std::copy_n (in[(std::size_t) c].begin() + (std::ptrdiff_t) pos, bs, buf[(std::size_t) c].begin());
            ptrs[(std::size_t) c] = buf[(std::size_t) c].data();
            if (cs.key)
                keyPtrs[(std::size_t) c] = keySig[(std::size_t) c].data() + pos;
        }
        if (cs.key)
            core.processWithKey (ptrs.data(), nch, keyPtrs.data(), nch, bs);
        else
            core.process (ptrs.data(), nch, bs);
        for (int c = 0; c < nch; ++c)
            h.bytes (buf[(std::size_t) c].data(), sizeof (float) * (std::size_t) bs);

        const auto& f = core.displayFrame();
        h.i (core.frameCounter());
        h.i (f.numBands);
        h.bytes (f.levelDb, sizeof f.levelDb);
        h.bytes (f.cutDb, sizeof f.cutDb);
        h.f (f.totalCutDb);
        h.bytes (f.moduleCutDb, sizeof f.moduleCutDb);
        h.f (f.snapCutDb);
        h.f (f.diffOffsetDb);
        h.f (f.keyDb);
        h.i (f.keyed ? 1 : 0);
        h.f (f.wash);
        h.bytes (f.pitchClass, sizeof f.pitchClass);
        h.f (f.programDb);
        h.f (f.activity);
        if (extended)
            hashGuard (h, f);       // the reference cores have no such field, and their hashes stand
        if (reached != nullptr)
        {
            reached->deepestCutDb = std::max (reached->deepestCutDb, f.totalCutDb);
            noteGuard (*reached, f);
        }
        h.i (core.latencySamples());
        pos += (std::size_t) bs;
        ++block;
    }
    return h.h;
}

/** Every case, mono and stereo, at 44.1, 48 and 96 kHz. With `compat`, at the settings of
    compatParams() and compatTuning(). With `extended` (the current core at its defaults, never
    with compat): the extra cases, DisplayFrame::guard in the hash, and 4 s at 96 kHz as at the
    other rates (the references were recorded with 3 s there, in which "defaults" and "key"
    never get over the Threshold long enough to cut). */
template <typename Prepare = PlainPrepare>
inline std::vector<Result> runAll (const Prepare& prepare = {}, bool compat = false, bool extended = false)
{
    std::vector<Result> out;
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (int nch = 1; nch <= 2; ++nch)
            for (const auto& cs : cases (extended))
            {
                char name[160];
                std::snprintf (name, sizeof name, "%s_%s_%.0f", cs.name, nch == 1 ? "mono" : "stereo", sr);
                out.push_back ({ name, run (cs, sr, nch, sr > 50000.0 && ! extended ? 3.0 : 4.0, prepare, compat, extended) });
            }
    return out;
}

#if DEHONK_GOLDEN_SURROUND
/** 5.1 for six channels (L R C LFE Ls Rs) and 7.1.4 for twelve (L R C LFE Lss Rss Ltf Rtf Ltr Rtr
    Lrs Rrs), as a host hands them over; anything else has no positions. Needs a core with
    layouts, so it is only compiled where DEHONK_GOLDEN_SURROUND is set. */
struct SurroundPrepare
{
    void operator() (dehonk::Core& core, double sr, int maxBlock, int nch) const
    {
        using P = dehonk::Position;
        dehonk::ChannelLayout l;
        if (nch == 6)
        {
            const P pos[] = { P::leftFront, P::rightFront, P::centre, P::lfe, P::leftOther, P::rightOther };
            std::copy (std::begin (pos), std::end (pos), l.position);
        }
        else if (nch == 12)
        {
            const P pos[] = { P::leftFront, P::rightFront, P::centre, P::lfe, P::leftOther, P::rightOther,
                              P::leftOther, P::rightOther, P::leftOther, P::rightOther, P::leftOther, P::rightOther };
            std::copy (std::begin (pos), std::end (pos), l.position);
        }
        core.prepare (sr, maxBlock, nch, l);
    }
};
#endif

/** Every case with 6 and 12 channels at 48 kHz (and the first three at 44.1 and 96 kHz), prepared
    by `prepare`, which is given the channel count and sets the layout. Shorter than the mono and
    stereo runs. */
template <typename Prepare>
inline std::vector<Result> runAllSurround (const Prepare& prepare, const char* layoutName, bool compat = false, bool extended = false)
{
    std::vector<Result> out;
    for (double sr : { 48000.0, 44100.0, 96000.0 })
        for (int nch : { 6, 12 })
        {
            int index = 0;
            for (const auto& cs : cases (extended))
            {
                if (sr != 48000.0 && index++ >= 3)
                    break;
                char name[200];
                std::snprintf (name, sizeof name, "%s_%dch_%s_%.0f", cs.name, nch, layoutName, sr);
                out.push_back ({ name, run (cs, sr, nch, extended ? 3.0 : 2.0, prepare, compat, extended) });
            }
        }
    return out;
}

} // namespace golden

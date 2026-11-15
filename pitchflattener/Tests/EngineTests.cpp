// The Locator and the Engine without a host: synthetic sounds with known answers, driven through
// real process() calls in random block sizes.
//
// The output of the engine for input sample t comes out latency samples later. While nothing is
// recognised it must be the input exactly (bit for bit); once the learned sound is recognised at
// input sample N as learned sample S, it must be the flattened sound from outputSampleFor(S),
// one sample per sample, exactly (after the 5 ms fade).

#include "Check.h"
#include "Engine.h"
#include "Locator.h"
#include "core/Flatten.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

//==============================================================================
// Allocation watch: every allocation or free made while the audio thread is inside process().
namespace
{
thread_local bool t_insideProcess = false;
std::atomic<int> g_audioAllocations { 0 };
std::atomic<int> g_audioFrees { 0 };

void* allocate(std::size_t n)
{
    if (t_insideProcess)
        g_audioAllocations.fetch_add(1);
    if (void* p = std::malloc(n > 0 ? n : 1))
        return p;
    throw std::bad_alloc();
}

void release(void* p) noexcept
{
    if (p != nullptr && t_insideProcess)
        g_audioFrees.fetch_add(1);
    std::free(p);
}
} // namespace

void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }

namespace pftest
{
namespace
{
using pitchflattener::Engine;
using pitchflattener::Locator;
using pitchflattener::Result;
namespace core = pitchflattener::core;
using Sound = std::vector<float>;

constexpr double kTwoPi = 6.283185307179586;
const std::vector<double> kRich { 1.0, 0.6, 0.45, 0.3, 0.25, 0.2, 0.15, 0.12 };

std::string num(double v, int decimals = 2)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals, v);
    return text;
}

double msSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// A tone whose fundamental follows hzAt(seconds), with harmonics, plus noise (as in CoreTests).
Sound tone(double seconds, double rate, const std::function<double(double)>& hzAt, const std::vector<double>& harmonics,
           double noise = 0.0, std::uint32_t seed = 2463534242u)
{
    const auto n = static_cast<std::size_t>(seconds * rate);
    Sound out(n);
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / rate;
        phase += kTwoPi * hzAt(t) / rate;
        double v = 0.0;
        for (std::size_t h = 0; h < harmonics.size(); ++h)
            v += harmonics[h] * std::sin(phase * static_cast<double>(h + 1));
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        v += noise * (static_cast<double>(seed) / 2147483648.0 - 1.0);
        out[i] = static_cast<float>(0.3 * v);
    }
    return out;
}

// The pass-by of CoreTests, shortened: a rich tone falling 7 semitones through the middle.
// roadNoise 0.5 adds noise 11 dB under the tone, as a real pass-by has.
Sound passBy(double rate, double seconds = 2.5, double transpose = 0.0, double roadNoise = 0.02)
{
    const double middle = seconds * 0.5;
    return tone(seconds, rate, [=](double t) { return 220.0 * std::exp2((transpose + 3.5 - 7.0 / (1.0 + std::exp(-(t - middle) * 5.0))) / 12.0); },
                kRich, roadNoise);
}

Sound noise(double seconds, double rate, float level, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-level, level);
    Sound out(static_cast<std::size_t>(seconds * rate));
    for (auto& v : out)
        v = d(rng);
    return out;
}

Sound silence(double seconds, double rate) { return Sound(static_cast<std::size_t>(seconds * rate), 0.0f); }

Sound join(std::initializer_list<Sound> parts)
{
    Sound out;
    for (const auto& p : parts)
        out.insert(out.end(), p.begin(), p.end());
    return out;
}

Sound slice(const Sound& x, double fromSeconds, double toSeconds, double rate)
{
    const auto a = static_cast<std::size_t>(fromSeconds * rate);
    const auto b = std::min(x.size(), static_cast<std::size_t>(toSeconds * rate));
    return Sound(x.begin() + static_cast<std::ptrdiff_t>(a), x.begin() + static_cast<std::ptrdiff_t>(b));
}

std::int64_t firstSound(const Sound& x)
{
    for (std::size_t i = 0; i < x.size(); ++i)
        if (std::abs(x[i]) >= Locator::kSilence)
            return static_cast<std::int64_t>(i);
    return -1;
}

std::int64_t firstNonZero(const float* x, std::int64_t n)
{
    for (std::int64_t i = 0; i < n; ++i)
        if (! identical(x[i], 0.0f))
            return i;
    return -1;
}

unsigned g_seed = 1;

// One engine, driven like a host drives a plug-in.
struct Rig
{
    Engine engine;
    double rate;
    std::mt19937 rng;
    std::int64_t played = 0;   // input samples so far (the engine's own count since prepare)
    int maxBlock;

    explicit Rig(double sampleRate = 48000.0, int maxBlockSize = 1024)
        : rate(sampleRate), rng(g_seed++), maxBlock(maxBlockSize)
    {
        engine.prepare(rate, maxBlock);
        engine.setNonRealtime(true);
    }

    int nextBlock(std::size_t left)
    {
        // Mostly ordinary sizes, now and then a tiny one (some hosts split blocks at automation).
        const int size = std::uniform_int_distribution<int>(0, 9)(rng) == 0 ? std::uniform_int_distribution<int>(1, 16)(rng)
                                                                            : std::uniform_int_distribution<int>(17, maxBlock)(rng);
        return static_cast<int>(std::min<std::size_t>(left, static_cast<std::size_t>(size)));
    }

    core::Channels play(const core::Channels& in, bool bypass = false, double realtimePacing = 0.0)
    {
        core::Channels out = in;
        const std::size_t n = out[0].size();
        const int channels = static_cast<int>(out.size());
        std::vector<float*> ptrs(out.size());
        std::size_t pos = 0;
        auto clock = std::chrono::steady_clock::now();
        while (pos < n)
        {
            const int block = realtimePacing > 0.0 ? static_cast<int>(std::min<std::size_t>(n - pos, 480)) : nextBlock(n - pos);
            for (std::size_t c = 0; c < out.size(); ++c)
                ptrs[c] = out[c].data() + pos;
            t_insideProcess = true;
            if (bypass)
                engine.processBypassed(ptrs.data(), channels, block);
            else
                engine.process(ptrs.data(), channels, block);
            t_insideProcess = false;
            pos += static_cast<std::size_t>(block);
            if (realtimePacing > 0.0)
            {
                clock += std::chrono::microseconds(static_cast<std::int64_t>(1.0e6 * block / rate));
                std::this_thread::sleep_until(clock);
            }
        }
        played += static_cast<std::int64_t>(n);
        return out;
    }

    Sound play(const Sound& in, bool bypass = false) { return play(core::Channels { in }, bypass)[0]; }

    // Learn a sound: silence, the sound, then the second of silence that ends the capture.
    bool learn(const core::Channels& sound)
    {
        engine.armLearn();
        core::Channels in;
        for (const auto& ch : sound)
            in.push_back(join({ silence(0.2, rate), ch, silence(1.3, rate) }));
        play(in);
        return engine.waitUntilIdle(60.0);
    }
    bool learn(const Sound& sound) { return learn(core::Channels { sound }); }
};

// Where the learned sound (as played, from its first sample) starts in what was captured.
std::int64_t soundStartInLearned(const Engine& engine)
{
    const auto r = engine.result();
    return r != nullptr ? firstNonZero(r->learned->locator.samples(), r->learned->locator.length()) : -1;
}

// out (from a play that began at engine input `base`) against the input delayed by the latency,
// over input samples [from, to). Returns the number that differ.
int countDryMismatches(const Sound& in, const Sound& out, int latency, std::int64_t from, std::int64_t to)
{
    int bad = 0;
    for (std::int64_t t = std::max<std::int64_t>(from, 0); t < to && t + latency < static_cast<std::int64_t>(out.size()); ++t)
        if (! identical(out[static_cast<std::size_t>(t + latency)], in[static_cast<std::size_t>(t)]))
            ++bad;
    return bad;
}

// The output after the match against the flattened sound from outputSampleFor(S), one sample per
// sample. matchInput is relative to the play. Checks [matchInput + fade, until).
struct FlatCheck
{
    int compared = 0, bad = 0;
};

FlatCheck compareFlat(const core::Channels& out, const Result& r, int latency, std::int64_t matchInput, std::int64_t source,
                      std::int64_t until, double rate, int hostChannels)
{
    FlatCheck fc;
    const auto& flat = r.flattened.channels;
    const auto F0 = static_cast<std::int64_t>(std::llround(r.flattened.outputSampleFor(static_cast<double>(source))));
    const auto fade = static_cast<std::int64_t>(std::llround(Engine::kFadeSeconds * rate)) + 1;
    const auto flatLength = static_cast<std::int64_t>(flat[0].size());
    for (std::int64_t t = matchInput + fade; t < until; ++t)
    {
        const std::int64_t o = t + latency, f = F0 + (t - matchInput);
        if (o >= static_cast<std::int64_t>(out[0].size()) || f >= flatLength)
            break;
        for (int c = 0; c < hostChannels; ++c)
        {
            float want;
            if (flat.size() == 1)
                want = flat[0][static_cast<std::size_t>(f)];
            else if (hostChannels == 1)
                want = 0.5f * (flat[0][static_cast<std::size_t>(f)] + flat[1][static_cast<std::size_t>(f)]);
            else
                want = flat[static_cast<std::size_t>(c)][static_cast<std::size_t>(f)];
            if (! identical(out[static_cast<std::size_t>(c)][static_cast<std::size_t>(o)], want))
                ++fc.bad;
        }
        ++fc.compared;
    }
    return fc;
}

//==============================================================================
// Locator
//==============================================================================
void locatorBasics()
{
    beginCase("locator: exact from the start, from anywhere, at any gain, after silence");
    const double rate = 48000.0;
    const Sound x = passBy(rate, 5.0);
    Locator loc;
    loc.prepare(x.data(), static_cast<std::int64_t>(x.size()), rate);
    const int P = loc.probeLength();
    check(P == 4096, "probe of 4096 samples at 48 kHz");

    auto m = loc.locate(x.data());
    check(m.found && m.sourceSample == 0 && m.correlation > 0.999999, "from the start: " + std::to_string(m.sourceSample));

    std::mt19937 rng(7);
    int exact = 0;
    double worstMs = 0.0;
    for (int k = 0; k < 20; ++k)
    {
        const auto pos = std::uniform_int_distribution<std::int64_t>(0, static_cast<std::int64_t>(x.size()) - P)(rng);
        const auto start = std::chrono::steady_clock::now();
        const auto mk = loc.locate(x.data() + pos);
        worstMs = std::max(worstMs, msSince(start));
        exact += mk.found && mk.sourceSample == pos ? 1 : 0;
    }
    check(exact == 20, "20 random places found to the sample: " + std::to_string(exact));
    std::printf("    -> 5 s sound at 48 kHz: slowest search %.1f ms\n", worstMs);

    for (const float gain : { 0.25f, 3.0f })
    {
        const std::int64_t pos = 123457;
        Sound probe(x.begin() + pos, x.begin() + pos + P);
        for (auto& v : probe)
            v *= gain;
        const auto mg = loc.locate(probe.data());
        check(mg.found && mg.sourceSample == pos && std::abs(mg.gain - gain) < 1.0e-3,
              "at gain " + num(gain) + ": found " + std::to_string(mg.sourceSample) + ", gain " + num(mg.gain, 4));
    }

    // A sound that begins after silence, the probe starting in that silence.
    const Sound y = join({ silence(0.3, rate), x });
    Locator loc2;
    loc2.prepare(y.data(), static_cast<std::int64_t>(y.size()), rate);
    const std::int64_t pos = static_cast<std::int64_t>(0.25 * rate);
    const auto ms = loc2.locate(y.data() + pos);
    check(ms.found && ms.sourceSample == pos, "probe beginning in the silence before the sound: " + std::to_string(ms.sourceSample));

    // Shorter than a probe.
    const Sound shortSound = slice(x, 1.0, 1.06, rate);
    Locator loc3;
    loc3.prepare(shortSound.data(), static_cast<std::int64_t>(shortSound.size()), rate);
    const auto mshort = loc3.locate(shortSound.data());
    check(loc3.probeLength() == static_cast<int>(shortSound.size()) && mshort.found && mshort.sourceSample == 0, "a 60 ms sound");
}

void locatorRejects()
{
    beginCase("locator: rejects unrelated audio, and the same sound at another speed or pitch");
    // With road noise in it, as a recorded pass-by has. (A perfectly clean tone of constant timbre
    // that falls through a pitch really is the same waveform, shifted, at another point of its
    // fall: nothing in the audio tells them apart. The engine's checks while playing catch that.)
    const double rate = 48000.0;
    const Sound x = passBy(rate, 5.0, 0.0, 0.5);
    Locator loc;
    loc.prepare(x.data(), static_cast<std::int64_t>(x.size()), rate);
    const int P = loc.probeLength();

    const Sound n = noise(0.2, rate, 0.2f, 99);
    auto m = loc.locate(n.data());
    check(! m.found, "noise rejected (best " + num(m.correlation, 3) + ")");

    const Sound other = tone(0.2, rate, [](double) { return 330.0; }, kRich);
    m = loc.locate(other.data());
    check(! m.found, "a steady tone rejected (best " + num(m.correlation, 3) + ")");

    // The same sound resampled 2% faster (higher and shorter): linear interpolation.
    Sound faster(static_cast<std::size_t>(static_cast<double>(x.size()) / 1.02) - 2);
    for (std::size_t i = 0; i < faster.size(); ++i)
    {
        const double s = static_cast<double>(i) * 1.02;
        const auto j = static_cast<std::size_t>(s);
        const double f = s - static_cast<double>(j);
        faster[i] = static_cast<float>(x[j] * (1.0 - f) + x[j + 1] * f);
    }
    double worst = 0.0;
    bool anyFound = false;
    for (const double at : { 0.3, 1.0, 2.4, 4.0 })
    {
        const auto mr = loc.locate(faster.data() + static_cast<std::size_t>(at * rate));
        anyFound = anyFound || mr.found;
        worst = std::max(worst, mr.correlation);
    }
    check(! anyFound, "resampled by 2% rejected everywhere (best " + num(worst, 3) + ")");

    const Sound higher = passBy(rate, 5.0, 1.0, 0.5);
    const auto same = loc.locate(x.data() + static_cast<std::size_t>(2.4 * rate));
    check(same.found && same.correlation > 0.999999, "while the same noisy sound is found exactly");
    anyFound = false;
    worst = 0.0;
    for (const double at : { 0.3, 2.4, 4.0 })
    {
        const auto mp = loc.locate(higher.data() + static_cast<std::size_t>(at * rate));
        anyFound = anyFound || mp.found;
        worst = std::max(worst, mp.correlation);
    }
    check(! anyFound, "a semitone higher rejected (best " + num(worst, 3) + ")");
    (void) P;
}

void locatorRepeats()
{
    beginCase("locator: a steady tone matches everywhere; the earliest consistent place is taken");
    const double rate = 48000.0;
    const Sound x = tone(3.0, rate, [](double) { return 200.0; }, kRich);
    Locator loc;
    loc.prepare(x.data(), static_cast<std::int64_t>(x.size()), rate);
    const auto period = static_cast<std::int64_t>(rate / 200.0);
    const auto m = loc.locate(x.data() + static_cast<std::size_t>(1.5 * rate));
    check(m.found && m.sourceSample < period + 1, "earliest: " + std::to_string(m.sourceSample));
    const auto prefer = static_cast<std::int64_t>(rate);
    const auto m2 = loc.locate(x.data() + static_cast<std::size_t>(1.5 * rate), prefer);
    check(m2.found && m2.sourceSample >= prefer && m2.sourceSample < prefer + period + 1,
          "earliest after what has been heard: " + std::to_string(m2.sourceSample));
}

void locatorTiming()
{
    beginCase("locator: a 120 s sound at 96 kHz");
    const double rate = 96000.0;
    const auto n = static_cast<std::size_t>(120.0 * rate);
    Sound x(n);
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.0f, 0.02f);
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / rate;
        phase += kTwoPi * (300.0 + 100.0 * std::sin(t * 0.5)) / rate;
        x[i] = static_cast<float>(0.3 * std::sin(phase)) + nd(rng);
    }
    auto start = std::chrono::steady_clock::now();
    Locator loc;
    loc.prepare(x.data(), static_cast<std::int64_t>(n), rate);
    const double prepareMs = msSince(start);
    const int P = loc.probeLength();

    const std::int64_t places[] = { 0, static_cast<std::int64_t>(60.0 * rate), static_cast<std::int64_t>(n) - P - 1 };
    for (const auto pos : places)
    {
        start = std::chrono::steady_clock::now();
        const auto m = loc.locate(x.data() + pos);
        const double ms = msSince(start);
        std::printf("    -> at %.1f s: %.1f ms\n", static_cast<double>(pos) / rate, ms);
        check(m.found && m.sourceSample == pos, "found at " + num(static_cast<double>(pos) / rate, 1) + " s");
        check(ms < 500.0, "searched in under 0.5 s: " + num(ms, 1) + " ms");
    }
    const Sound other = noise(0.1, rate, 0.2f, 5);
    start = std::chrono::steady_clock::now();
    const auto m = loc.locate(other.data());
    const double ms = msSince(start);
    std::printf("    -> unrelated (whole sound searched): %.1f ms; prepare %.1f ms; probe %d samples\n", ms, prepareMs, P);
    check(! m.found && ms < 500.0, "unrelated audio rejected in under 0.5 s");
}

//==============================================================================
// Engine
//==============================================================================
void engineLearnAndPlay()
{
    beginCase("engine: learn a falling tone, play it again from the start and from the middle");
    Rig rig;
    const double rate = rig.rate;
    const int L = rig.engine.latencySamples();
    check(L == static_cast<int>(std::llround(Engine::kLatencySeconds * rate)), "latency " + std::to_string(L));
    check(rig.engine.state() == Engine::State::Idle, "idle before learning");

    const Sound sound = passBy(rate);
    check(rig.learn(sound), "learned and rendered");
    check(rig.engine.state() == Engine::State::Ready, "ready");
    const auto r = rig.engine.result();
    if (r == nullptr || ! r->playable())
        return;
    const auto learnedStart = soundStartInLearned(rig.engine);
    std::printf("    -> learned %.3f s (sound %.3f s), flattened %.3f s, %s line\n", static_cast<double>(r->learned->locator.length()) / rate,
                static_cast<double>(sound.size()) / rate, static_cast<double>(r->flattened.channels[0].size()) / rate,
                r->options.line >= 0 ? pitchflattener::core::trackerName((pitchflattener::core::Tracker) r->options.line) : "no");
    // The capture starts 50 ms before the first block with sound in it (the block itself may begin
    // before the sound does).
    const auto before = learnedStart - static_cast<std::int64_t>(0.05 * rate);
    check(before >= 0 && before < rig.maxBlock && r->learned->locator.length() <= static_cast<std::int64_t>(sound.size()) + learnedStart + static_cast<std::int64_t>(0.011 * rate),
          "the capture is the sound with 50 ms before its first block: starts at " + std::to_string(learnedStart));

    // From the start.
    {
        const Sound in = join({ silence(0.5, rate), sound, silence(1.0, rate) });
        const auto base = rig.played;
        const Sound out = rig.play(in);
        const auto N = rig.engine.lastMatchInput() - base;
        const auto S = rig.engine.lastMatchSource();
        check(N == firstSound(in), "matched at the input's first sound: " + std::to_string(N) + " vs " + std::to_string(firstSound(in)));
        check(S == learnedStart + (N - static_cast<std::int64_t>(0.5 * rate)), "at the right learned sample: " + std::to_string(S));
        check(countDryMismatches(in, out, L, 0, N) == 0, "the input, delayed exactly, before the match");
        const auto fc = compareFlat(core::Channels { out }, *r, L, N, S, static_cast<std::int64_t>(in.size()), rate, 1);
        check(fc.compared > static_cast<int>(1.5 * rate) && fc.bad == 0,
              "then the flattened sound sample for sample: " + std::to_string(fc.bad) + " of " + std::to_string(fc.compared) + " differ");
        // After the flattened sound ends: silence until the original ends, then the input.
        const auto F0 = std::llround(r->flattened.outputSampleFor(static_cast<double>(S)));
        const auto flatEnd = N + static_cast<std::int64_t>(r->flattened.channels[0].size()) - F0;
        int loud = 0;
        for (auto t = flatEnd + 10; t + L < static_cast<std::int64_t>(out.size()); ++t)
            loud += std::abs(out[static_cast<std::size_t>(t + L)]) > 0.0f ? 1 : 0;
        check(loud == 0, "silence after the flattened sound ends");
        check(! rig.engine.isLocated() && rig.engine.playPosition() < 0, "waiting again afterwards");
    }

    // From the middle, cut in abruptly.
    {
        const Sound in = join({ silence(0.5, rate), slice(sound, 1.0, 2.5, rate), silence(1.0, rate) });
        const auto base = rig.played;
        const Sound out = rig.play(in);
        const auto N = rig.engine.lastMatchInput() - base;
        const auto S = rig.engine.lastMatchSource();
        const auto want = learnedStart + static_cast<std::int64_t>(1.0 * rate) + (N - static_cast<std::int64_t>(0.5 * rate));
        check(N == firstSound(in) && S == want, "from the middle: matched " + std::to_string(N) + " -> " + std::to_string(S) + " (want " + std::to_string(want) + ")");
        check(countDryMismatches(in, out, L, 0, N) == 0, "dry before");
        const auto fc = compareFlat(core::Channels { out }, *r, L, N, S, static_cast<std::int64_t>(in.size()), rate, 1);
        check(fc.compared > static_cast<int>(0.5 * rate) && fc.bad == 0, "flattened from the matching point: " + std::to_string(fc.bad) + " differ");
    }

    // Unrelated audio passes untouched.
    {
        const Sound in = join({ tone(1.0, rate, [](double) { return 330.0; }, kRich, 0.01), noise(1.0, rate, 0.1f, 11), silence(0.5, rate) });
        const Sound out = rig.play(in);
        check(countDryMismatches(in, out, L, 0, static_cast<std::int64_t>(in.size())) == 0, "unrelated audio: the input delayed by exactly the latency, bit for bit");
    }

    // A jump: the first second, then straight on from 1.8 s.
    {
        const Sound in = join({ silence(0.3, rate), slice(sound, 0.0, 1.0, rate), slice(sound, 1.8, 2.5, rate), silence(1.0, rate) });
        const auto jump = static_cast<std::int64_t>(1.3 * rate);
        const auto base = rig.played;
        const Sound out = rig.play(in);
        const auto N2 = rig.engine.lastMatchInput() - base;
        const auto S2 = rig.engine.lastMatchSource();
        const auto want = learnedStart + static_cast<std::int64_t>(1.8 * rate) + (N2 - jump);
        check(N2 >= jump && N2 - jump < static_cast<std::int64_t>(0.15 * rate) && S2 == want,
              "relocated " + num(static_cast<double>(N2 - jump) / rate * 1000.0, 1) + " ms after the jump, at the right place");
        const auto fc = compareFlat(core::Channels { out }, *r, L, N2, S2, static_cast<std::int64_t>(in.size()), rate, 1);
        check(fc.compared > static_cast<int>(0.3 * rate) && fc.bad == 0, "flattened from the new place: " + std::to_string(fc.bad) + " differ");
        // Before the jump: the first match, flattened.
        const auto N1 = firstSound(in);
        const auto S1 = learnedStart + (N1 - static_cast<std::int64_t>(0.3 * rate));
        const auto fc1 = compareFlat(core::Channels { out }, *r, L, N1, S1, jump - 2 * 1024, rate, 1);
        check(fc1.compared > static_cast<int>(0.8 * rate) && fc1.bad == 0, "and from the start before it: " + std::to_string(fc1.bad) + " differ");
    }

    // A different target renders again, and playback uses the new render.
    {
        auto options = rig.engine.options();
        options.target = core::Target::LowPoint;
        rig.engine.setOptions(options);
        check(rig.engine.waitUntilIdle(30.0), "re-rendered");
        const auto r2 = rig.engine.result();
        check(r2 != nullptr && r2 != r && r2->generation > r->generation && r2->learned == r->learned, "a new render of the same reading");
        if (r2 != nullptr && r2->playable())
        {
            check(r2->flattened.channels[0].size() > r->flattened.channels[0].size(), "to the low point it is longer");
            const Sound in = join({ silence(0.5, rate), sound, silence(1.5, rate) });
            const auto base = rig.played;
            const Sound out = rig.play(in);
            const auto N = rig.engine.lastMatchInput() - base;
            const auto S = rig.engine.lastMatchSource();
            const auto fc = compareFlat(core::Channels { out }, *r2, L, N, S, static_cast<std::int64_t>(in.size()), rate, 1);
            check(fc.compared > static_cast<int>(2.0 * rate) && fc.bad == 0, "playback uses the new render: " + std::to_string(fc.bad) + " differ");
        }
    }

    // Bypassed: the delayed input.
    {
        const Sound in = join({ silence(0.3, rate), sound, silence(0.6, rate) });
        const Sound out = rig.play(in, true);
        check(countDryMismatches(in, out, L, 0, static_cast<std::int64_t>(in.size())) == 0, "bypassed: the input delayed");
    }

    // Clear: back to the input.
    {
        rig.engine.clear();
        check(rig.engine.state() == Engine::State::Idle && rig.engine.result() == nullptr, "cleared");
        const Sound in = join({ silence(0.3, rate), sound, silence(0.6, rate) });
        const Sound out = rig.play(in);
        check(countDryMismatches(in, out, L, 0, static_cast<std::int64_t>(in.size())) == 0, "after Clear the sound passes as it is");
    }
}

void engineChangeWhilePlaying()
{
    beginCase("engine: a new render arriving mid-sound takes over from the same moment");
    Rig rig;
    const double rate = rig.rate;
    const int L = rig.engine.latencySamples();
    const Sound sound = passBy(rate, 3.0);
    check(rig.learn(sound), "learned");
    const auto r1 = rig.engine.result();
    if (r1 == nullptr || ! r1->playable())
        return;
    const Sound first = join({ silence(0.3, rate), slice(sound, 0.0, 1.0, rate) });
    const auto base = rig.played;
    core::Channels out1 = rig.play(core::Channels { first });
    auto options = rig.engine.options();
    options.target = core::Target::Centre;
    rig.engine.setOptions(options);
    check(rig.engine.waitUntilIdle(30.0), "re-rendered while paused mid-sound");
    const auto r2 = rig.engine.result();
    const Sound rest = join({ slice(sound, 1.0, 3.0, rate), silence(1.0, rate) });
    core::Channels out2 = rig.play(core::Channels { rest });
    const auto N = rig.engine.lastMatchInput() - base;
    const auto S = rig.engine.lastMatchSource();
    check(N == firstSound(first), "the match from the start still holds");
    // After the switch (at the next output sample), the new render from the same moment of the sound.
    const auto at = static_cast<std::int64_t>(first.size()) - L;   // first output sample of the second play, as input position
    const auto sourceAt = S + (at - N);
    Sound joined = out1[0];
    joined.insert(joined.end(), out2[0].begin(), out2[0].end());
    const auto fc = compareFlat(core::Channels { joined }, *r2, L, at, sourceAt, static_cast<std::int64_t>(joined.size()), rate, 1);
    check(fc.compared > static_cast<int>(1.0 * rate) && fc.bad == 0, "the new render from the same moment: " + std::to_string(fc.bad) + " differ");
}

void engineNothingToFlatten()
{
    beginCase("engine: noise has no pitch: nothing to flatten, it passes as it is");
    Rig rig;
    const double rate = rig.rate;
    const Sound n = noise(1.5, rate, 0.1f, 21);
    check(rig.learn(n), "learned");
    check(rig.engine.state() == Engine::State::NothingToFlatten, "nothing to flatten");
    const Sound in = join({ silence(0.3, rate), n, silence(0.5, rate) });
    const Sound out = rig.play(in);
    check(countDryMismatches(in, out, rig.engine.latencySamples(), 0, static_cast<std::int64_t>(in.size())) == 0, "passes dry");
}

void engineInstances()
{
    beginCase("engine: two instances are independent");
    Rig a, b;
    const double rate = a.rate;
    const Sound sound = passBy(rate);
    check(a.learn(sound), "the first learned");
    check(b.engine.state() == Engine::State::Idle && b.engine.result() == nullptr, "the second has learned nothing");
    const Sound in = join({ silence(0.3, rate), sound, silence(0.6, rate) });
    const Sound out = b.play(in);
    check(countDryMismatches(in, out, b.engine.latencySamples(), 0, static_cast<std::int64_t>(in.size())) == 0, "the second passes it dry");
    b.engine.armLearn();
    check(a.engine.state() == Engine::State::Ready && ! a.engine.isLearnArmed(), "arming the second leaves the first ready");
    b.engine.disarmLearn();
}

void engineCaptureEnds()
{
    beginCase("engine: a capture ends on silence, when the audio stops, on disarm, at two minutes; clicks are ignored");
    const double rate = 48000.0;
    const Sound sound = passBy(rate, 1.5);
    const auto soundLength = static_cast<std::int64_t>(sound.size());
    const auto slack = static_cast<std::int64_t>(0.061 * rate) + 1024;   // pre-roll, part of a block, the tail kept

    {
        Rig rig;
        rig.engine.armLearn();
        check(rig.engine.state() == Engine::State::Armed, "armed");
        rig.play(join({ silence(0.2, rate), sound, silence(0.5, rate) }));
        check(rig.engine.state() == Engine::State::Listening, "still listening after half a second of silence");
        check(std::abs(rig.engine.captureSeconds() - 2.05) < 0.06, "capture time so far " + num(rig.engine.captureSeconds()));
        rig.play(silence(0.6, rate));
        check(rig.engine.state() != Engine::State::Listening, "ended by a second of silence");
        check(rig.engine.waitUntilIdle(30.0), "analysed");
        const auto r = rig.engine.result();
        check(r != nullptr && r->learned->locator.length() >= soundLength && r->learned->locator.length() <= soundLength + slack,
              "the silence at the end trimmed");
    }
    {
        Rig rig;
        rig.engine.armLearn();
        rig.play(join({ silence(0.2, rate), sound }));
        check(rig.engine.state() == Engine::State::Listening, "listening while the sound plays");
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        check(rig.engine.state() != Engine::State::Listening, "ended when the audio stopped arriving");
        check(rig.engine.waitUntilIdle(30.0), "analysed");
        const auto r = rig.engine.result();
        check(r != nullptr && r->learned->locator.length() >= soundLength - 2 && r->learned->locator.length() <= soundLength + slack,
              "kept all of it");
    }
    {
        Rig rig;
        rig.engine.armLearn();
        rig.play(join({ silence(0.2, rate), slice(sound, 0.0, 1.0, rate) }));
        rig.engine.disarmLearn();
        check(! rig.engine.isLearnArmed() && rig.engine.state() == Engine::State::Analysing, "disarmed: analysing what was heard");
        check(rig.engine.waitUntilIdle(30.0), "analysed");
        const auto r = rig.engine.result();
        const auto heard = static_cast<std::int64_t>(1.0 * rate);
        check(r != nullptr && r->learned->locator.length() >= heard - 2 && r->learned->locator.length() <= heard + slack, "kept what was heard");
    }
    {
        Rig rig;
        rig.engine.armLearn();
        rig.play(join({ silence(0.2, rate), slice(sound, 0.0, 0.02, rate), silence(1.2, rate) }));
        for (int i = 0; i < 200 && rig.engine.state() != Engine::State::Armed; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(rig.engine.state() == Engine::State::Armed && rig.engine.result() == nullptr, "a 20 ms click is forgotten and Learn stays armed");
        rig.engine.disarmLearn();
        check(rig.engine.state() == Engine::State::Idle, "disarmed");
    }
    {
        Rig rig(8000.0);
        rig.engine.armLearn();
        rig.play(tone(121.0, 8000.0, [](double) { return 200.0; }, { 1.0 }));
        check(rig.engine.state() != Engine::State::Listening && std::abs(rig.engine.captureSeconds() - 120.0) < 1.0e-9,
              "stopped at two minutes: " + num(rig.engine.captureSeconds(), 3) + " s");
        rig.engine.clear();
        check(rig.engine.waitUntilIdle(30.0) && rig.engine.state() == Engine::State::Idle, "and Clear stops its analysis");
    }
}

void engineChannels()
{
    beginCase("engine: stereo sound on stereo and mono tracks, mono sound on a stereo track");
    const double rate = 48000.0;
    const Sound left = passBy(rate);
    Sound right = left;
    for (std::size_t i = 0; i < right.size(); ++i)
        right[i] = 0.6f * left[i] + 0.05f * left[(i + 37) % left.size()];

    Rig rig;
    const int L = rig.engine.latencySamples();
    check(rig.learn(core::Channels { left, right }), "learned in stereo");
    const auto r = rig.engine.result();
    if (r == nullptr || ! r->playable())
        return;
    check(r->learned->channels.size() == 2 && r->flattened.channels.size() == 2, "kept and flattened both channels");
    const auto learnedStart = soundStartInLearned(rig.engine);
    {
        const core::Channels in { join({ silence(0.4, rate), left, silence(0.8, rate) }), join({ silence(0.4, rate), right, silence(0.8, rate) }) };
        const auto base = rig.played;
        const auto out = rig.play(in);
        const auto N = rig.engine.lastMatchInput() - base;
        const auto S = rig.engine.lastMatchSource();
        check(S == learnedStart + (N - static_cast<std::int64_t>(0.4 * rate)), "stereo: right place");
        const auto fc = compareFlat(out, *r, L, N, S, static_cast<std::int64_t>(in[0].size()), rate, 2);
        check(fc.compared > static_cast<int>(1.5 * rate) && fc.bad == 0, "stereo on stereo: each channel its own: " + std::to_string(fc.bad) + " differ");
    }
    {
        Sound mono(left.size());
        for (std::size_t i = 0; i < mono.size(); ++i)
            mono[i] = 0.5f * (left[i] + right[i]);
        const Sound in = join({ silence(0.4, rate), mono, silence(0.8, rate) });
        const auto base = rig.played;
        const Sound out = rig.play(in);
        const auto N = rig.engine.lastMatchInput() - base;
        const auto S = rig.engine.lastMatchSource();
        check(S == learnedStart + (N - static_cast<std::int64_t>(0.4 * rate)), "on a mono track: right place");
        const auto fc = compareFlat(core::Channels { out }, *r, L, N, S, static_cast<std::int64_t>(in.size()), rate, 1);
        check(fc.compared > static_cast<int>(1.5 * rate) && fc.bad == 0, "stereo on mono: the halves summed: " + std::to_string(fc.bad) + " differ");
    }

    Rig monoRig;
    check(monoRig.learn(left), "learned in mono");
    const auto rm = monoRig.engine.result();
    if (rm == nullptr || ! rm->playable())
        return;
    const auto monoStart = soundStartInLearned(monoRig.engine);
    const Sound padded = join({ silence(0.4, rate), left, silence(0.8, rate) });
    const auto base = monoRig.played;
    const auto out = monoRig.play(core::Channels { padded, padded });
    const auto N = monoRig.engine.lastMatchInput() - base;
    const auto S = monoRig.engine.lastMatchSource();
    check(S == monoStart + (N - static_cast<std::int64_t>(0.4 * rate)), "mono on stereo: right place");
    const auto fc = compareFlat(out, *rm, L, N, S, static_cast<std::int64_t>(padded.size()), rate, 2);
    check(fc.compared > static_cast<int>(1.5 * rate) && fc.bad == 0, "mono on stereo: the same on both: " + std::to_string(fc.bad) + " differ");
}

void engineRateAndRealtime()
{
    beginCase("engine: another sample rate is not played; in real time the latency covers the search");
    Rig rig;
    const double rate = rig.rate;
    const Sound sound = passBy(rate);
    check(rig.learn(sound), "learned at 48 kHz");
    const auto r = rig.engine.result();
    if (r == nullptr || ! r->playable())
        return;

    rig.engine.prepare(96000.0, 512);
    rig.played = 0;
    const int L96 = rig.engine.latencySamples();
    const Sound in96 = join({ silence(0.3, 96000.0), sound, silence(0.5, 96000.0) });
    rig.rate = 96000.0;
    const Sound out96 = rig.play(in96);
    check(rig.engine.sampleRateMismatch(), "the mismatch is flagged");
    check(countDryMismatches(in96, out96, L96, 0, static_cast<std::int64_t>(in96.size())) == 0, "and the audio passes as it is");

    rig.engine.prepare(48000.0, 512);
    rig.rate = 48000.0;
    rig.played = 0;
    rig.engine.setNonRealtime(false);
    const int L = rig.engine.latencySamples();
    const Sound in = join({ silence(0.3, rate), sound, silence(0.6, rate) });
    const auto out = rig.play(core::Channels { in }, false, 1.0);
    check(! rig.engine.sampleRateMismatch(), "back at 48 kHz: no mismatch");
    const auto N = rig.engine.lastMatchInput();
    const auto S = rig.engine.lastMatchSource();
    const auto learnedStart = soundStartInLearned(rig.engine);
    check(N == firstSound(in) && S == learnedStart + (N - static_cast<std::int64_t>(0.3 * rate)), "real time: matched at the start");
    const auto fc = compareFlat(out, *r, L, N, S, static_cast<std::int64_t>(in.size()), rate, 1);
    check(fc.compared > static_cast<int>(1.5 * rate) && fc.bad == 0,
          "real time: flattened from the very first sample of the sound (the answer beat the latency): " + std::to_string(fc.bad) + " differ");
    std::printf("    -> search took %.1f ms\n", rig.engine.lastSearchMilliseconds());
}

} // namespace

void runLocatorTests()
{
    locatorBasics();
    locatorRejects();
    locatorRepeats();
    locatorTiming();
}

void runEngineTests()
{
    g_seed = static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xffff) + 1u;
    std::printf("(block sizes from seed %u)\n", g_seed);
    g_audioAllocations.store(0);
    g_audioFrees.store(0);
    engineLearnAndPlay();
    engineChangeWhilePlaying();
    engineNothingToFlatten();
    engineInstances();
    engineCaptureEnds();
    engineChannels();
    engineRateAndRealtime();
    beginCase("engine: the audio thread never allocated or freed");
    check(g_audioAllocations.load() == 0 && g_audioFrees.load() == 0,
          "allocations " + std::to_string(g_audioAllocations.load()) + ", frees " + std::to_string(g_audioFrees.load()));
}

} // namespace pftest

#ifdef PF_ENGINE_TESTS_MAIN
int main()
{
    pftest::runLocatorTests();
    pftest::runEngineTests();
    std::printf("\n%d checks, %d failed\n", pftest::g_checks, pftest::g_failures);
    return pftest::g_failures == 0 ? 0 : 1;
}
#endif

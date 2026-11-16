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
#include "SoundRegistry.h"
#include "core/Flatten.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
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

// Every rig's engine logs into this (as the plug-in's does into its lock-free queue), so the
// allocation watch covers the audio thread's logging too.
std::atomic<int> g_logged { 0 };
const bool g_printEvents = std::getenv("PITCHFLATTENER_PRINT_EVENTS") != nullptr;   // to see what an engine did (debugging)
void countingSink(void*, const pitchflattener::EngineEvent& e) noexcept
{
    g_logged.fetch_add(1);
    if (g_printEvents)
        std::printf("      [event %d: %lld %lld %lld %lld %lld | %.2f %.2f]\n", static_cast<int>(e.kind), static_cast<long long>(e.i[0]), static_cast<long long>(e.i[1]),
                    static_cast<long long>(e.i[2]), static_cast<long long>(e.i[3]), static_cast<long long>(e.i[4]), e.d[0], e.d[1]);
}
unsigned g_seedChosen = 0;

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
        engine.setLogSink(&countingSink, nullptr);
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

    std::function<void()> onBlock;   // called after every block (readouts)
    int fixedBlock = 0;              // > 0: every block this size

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
            const int block = fixedBlock > 0 ? static_cast<int>(std::min<std::size_t>(n - pos, static_cast<std::size_t>(fixedBlock)))
                            : realtimePacing > 0.0 ? static_cast<int>(std::min<std::size_t>(n - pos, 480)) : nextBlock(n - pos);
            for (std::size_t c = 0; c < out.size(); ++c)
                ptrs[c] = out[c].data() + pos;
            t_insideProcess = true;
            if (bypass)
                engine.processBypassed(ptrs.data(), channels, block);
            else
                engine.process(ptrs.data(), channels, block);
            t_insideProcess = false;
            pos += static_cast<std::size_t>(block);
            if (onBlock)
                onBlock();
            if (realtimePacing > 0.0)
            {
                // Paced like an audio device: one block per block's worth of time. A device never
                // sends blocks early to make up for a late one, so neither does this: after an
                // oversleep (a busy machine) the next block is due a block's time from now. Catching
                // up instead would send bursts faster than real time, which the engine rightly
                // takes for a render.
                clock += std::chrono::microseconds(static_cast<std::int64_t>(1.0e6 * block / rate));
                std::this_thread::sleep_until(clock);
                clock = std::max(clock, std::chrono::steady_clock::now());
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

FlatCheck compareFlatFrom(const core::Channels& out, const Result& r, int latency, std::int64_t matchInput, std::int64_t F0,
                          std::int64_t until, double rate, int hostChannels)
{
    FlatCheck fc;
    const auto& flat = r.flattened.channels;
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

FlatCheck compareFlat(const core::Channels& out, const Result& r, int latency, std::int64_t matchInput, std::int64_t source,
                      std::int64_t until, double rate, int hostChannels)
{
    const auto F0 = static_cast<std::int64_t>(std::llround(r.flattened.outputSampleFor(static_cast<double>(source))));
    return compareFlatFrom(out, r, latency, matchInput, F0, until, rate, hostChannels);
}

double rms(const Sound& x, std::int64_t a, std::int64_t b)
{
    double s = 0.0;
    a = std::max<std::int64_t>(a, 0);
    b = std::min<std::int64_t>(b, static_cast<std::int64_t>(x.size()));
    if (b <= a)
        return 0.0;
    for (auto i = a; i < b; ++i)
        s += static_cast<double>(x[static_cast<std::size_t>(i)]) * x[static_cast<std::size_t>(i)];
    return std::sqrt(s / static_cast<double>(b - a));
}

double db(double v) { return 20.0 * std::log10(std::max(v, 1.0e-12)); }

Sound mixIn(const Sound& a, const Sound& b, float gain)
{
    Sound out(std::max(a.size(), b.size()), 0.0f);
    for (std::size_t i = 0; i < a.size(); ++i)
        out[i] += a[i];
    for (std::size_t i = 0; i < b.size(); ++i)
        out[i] += gain * b[i];
    return out;
}

void setTarget(Rig& rig, core::Target target, bool keepLength = false)
{
    auto o = rig.engine.options();
    o.target = target;
    o.keepLength = keepLength;
    rig.engine.setOptions(o);
    check(rig.engine.waitUntilIdle(60.0), "rendered again");
}

// Waits (up to timeout) for a condition, polling: robust on a loaded machine.
bool eventually(const std::function<bool()>& condition, double timeoutSeconds)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(timeoutSeconds * 1000.0));
    while (std::chrono::steady_clock::now() < end)
    {
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
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
    // With road noise in it, as a recorded pass-by has, so the near misses are near misses of a
    // real recording. (A perfectly clean tone of constant timbre falling through a pitch is,
    // shifted, nearly the same waveform at another point of its fall; with the residual test the
    // search refuses that too, and the checks while playing would let go of it at once.)
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
    beginCase("locator: a 120 s sound at 96 kHz and at 192 kHz, against the latency's search budget");
    const double budgetMs = Engine::kSearchBudgetSeconds * 1000.0;
    for (const double rate : { 96000.0, 192000.0 })
    {
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

        // The quickest of three runs: what the machine can do, not what a busy moment allowed.
        const auto timed = [&](const float* probe, Locator::Match& m)
        {
            double best = 1.0e9;
            for (int k = 0; k < 3; ++k)
            {
                start = std::chrono::steady_clock::now();
                m = loc.locate(probe);
                best = std::min(best, msSince(start));
            }
            return best;
        };
        const std::int64_t places[] = { 0, static_cast<std::int64_t>(60.0 * rate), static_cast<std::int64_t>(n) - P - 1 };
        for (const auto pos : places)
        {
            Locator::Match m;
            const double ms = timed(x.data() + pos, m);
            std::printf("    -> %.0f kHz, at %.1f s: %.1f ms\n", rate / 1000.0, static_cast<double>(pos) / rate, ms);
            check(m.found && m.sourceSample == pos, "found at " + num(static_cast<double>(pos) / rate, 1) + " s, " + num(rate / 1000.0, 0) + " kHz: " + std::to_string(m.sourceSample));
            check(ms < 1.5 * budgetMs, "searched within the budget (" + num(budgetMs, 0) + " ms, half again allowed for a busy machine): " + num(ms, 1) + " ms");
        }
        const Sound other = noise(0.1, rate, 0.2f, 5);
        Locator::Match m;
        const double ms = timed(other.data(), m);
        std::printf("    -> unrelated (whole sound searched): %.1f ms; prepare %.1f ms; probe %d samples\n", ms, prepareMs, P);
        check(! m.found && ms < 1.5 * budgetMs, "unrelated audio rejected within the budget");
    }
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
    beginCase("engine: a new render arriving mid-sound carries on from the moment being heard");
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
    // After the switch (at the next output sample): the moment of the sound the old render was
    // playing there, found in the new render. (In Varispeed that is not the input's own moment:
    // continuing from the input's moment replayed or skipped a stretch of the sound.)
    const auto at = static_cast<std::int64_t>(first.size()) - L;   // first output sample of the second play, as input position
    const auto oldOffset = static_cast<std::int64_t>(std::llround(r1->flattened.outputSampleFor(static_cast<double>(S)))) - N;
    const double heard = r1->flattened.sourceSampleFor(static_cast<double>(at + oldOffset));
    const auto F0 = static_cast<std::int64_t>(std::llround(r2->flattened.outputSampleFor(heard)));
    Sound joined = out1[0];
    joined.insert(joined.end(), out2[0].begin(), out2[0].end());
    const auto fc = compareFlatFrom(core::Channels { joined }, *r2, L, at, F0, static_cast<std::int64_t>(joined.size()), rate, 1);
    check(fc.compared > static_cast<int>(0.5 * rate) && fc.bad == 0, "the new render from the moment being heard: " + std::to_string(fc.bad) + " differ");
    std::printf("    -> heard %.3f s of the sound at the switch (the input was at %.3f s)\n", heard / rate, static_cast<double>(S + (at - N)) / rate);
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
        check(eventually([&] { return rig.engine.state() != Engine::State::Listening; }, 10.0), "ended when the audio stopped arriving");
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
    // Where the flattened sound took over: from the very first sample when the answer beat the
    // latency (it does by far on an idle machine); on a loaded one it may start later, but always
    // at the right place for that moment.
    std::int64_t start = N;
    while (start < static_cast<std::int64_t>(in.size()) - L && identical(out[0][static_cast<std::size_t>(start + L)], in[static_cast<std::size_t>(start)]))
        ++start;
    const auto fc = compareFlat(out, *r, L, N, S, static_cast<std::int64_t>(in.size()), rate, 1);
    FlatCheck after;
    {
        const auto F0 = static_cast<std::int64_t>(std::llround(r->flattened.outputSampleFor(static_cast<double>(S))));
        after = compareFlatFrom(out, *r, L, N, F0, static_cast<std::int64_t>(in.size()), rate, 1);
    }
    if (start > N + static_cast<std::int64_t>(Engine::kFadeSeconds * rate) + 1)
    {
        const auto F0 = static_cast<std::int64_t>(std::llround(r->flattened.outputSampleFor(static_cast<double>(S)))) + (start - N);
        after = compareFlatFrom(out, *r, L, start, F0, static_cast<std::int64_t>(in.size()), rate, 1);
    }
    check(after.compared > static_cast<int>(1.0 * rate) && after.bad == 0, "real time: the flattened sound at the right place: " + std::to_string(after.bad) + " differ");
    std::printf("    -> search took %.1f ms; the flattened sound took over %s (%.1f ms after the sound began)\n", rig.engine.lastSearchMilliseconds(),
                fc.bad == 0 ? "from its first sample" : "late", static_cast<double>(start - N) / rate * 1000.0);
}

//==============================================================================
// The audit's faults, one test each: the plug-in must never replace audio it should leave alone.
//==============================================================================
void locatorResidual()
{
    beginCase("locator: the same recording, not a resemblance (residual under -30 dB)");
    const double rate = 48000.0;
    const Sound x = passBy(rate, 4.0);
    Locator loc;
    loc.prepare(x.data(), static_cast<std::int64_t>(x.size()), rate);
    const auto pos = static_cast<std::size_t>(1.7 * rate);
    const auto same = loc.locate(x.data() + pos);
    check(same.found && same.residual < 1.0e-9, "the same file: residual " + num(db(std::sqrt(same.residual)) * 2.0, 0) + " dB");
    Sound inverted(x.begin() + static_cast<std::ptrdiff_t>(pos), x.begin() + static_cast<std::ptrdiff_t>(pos) + loc.probeLength());
    for (auto& v : inverted)
        v = -v;
    check(! loc.locate(inverted.data()).found, "a polarity-inverted copy is not the learned recording");
    Sound offset(x.begin() + static_cast<std::ptrdiff_t>(pos), x.begin() + static_cast<std::ptrdiff_t>(pos) + loc.probeLength());
    for (auto& v : offset)
        v += 0.05f;
    check(! loc.locate(offset.data()).found, "nor one with a DC offset");
    // A fade on the player across the probe (40 dB a second) still fits.
    Sound faded(x.begin() + static_cast<std::ptrdiff_t>(pos), x.begin() + static_cast<std::ptrdiff_t>(pos) + loc.probeLength());
    for (std::size_t i = 0; i < faded.size(); ++i)
        faded[i] *= static_cast<float>(std::pow(10.0, (-20.0 + 40.0 * static_cast<double>(i) / rate) / 20.0));
    const auto mf = loc.locate(faded.data());
    check(mf.found && mf.sourceSample == static_cast<std::int64_t>(pos), "a fade across the probe still matches, to the sample");
    // Quiet material that matches everywhere (a steady tone) is no evidence; loud, it is.
    const Sound held = tone(3.0, rate, [](double) { return 200.0; }, kRich);
    Locator heldLoc;
    heldLoc.prepare(held.data(), static_cast<std::int64_t>(held.size()), rate);
    Sound quiet(held.begin() + 48000, held.begin() + 48000 + heldLoc.probeLength());
    for (auto& v : quiet)
        v *= 0.1f;   // -20 dB
    const auto mq = heldLoc.locate(quiet.data());
    check(! mq.found && mq.ambiguous && mq.tooQuiet, "an ambiguous match 20 dB under the learned sound is refused");
    const auto ml = heldLoc.locate(held.data() + 48000);
    check(ml.found && ml.sourceSample <= static_cast<std::int64_t>(rate / 200.0), "the same at full level is taken (the earliest place): " + std::to_string(ml.sourceSample));
}

void enginePendingAndLayers()
{
    beginCase("engine: a match is heard only once it has held; layers, fades, swaps, hum and tones are left alone");
    Rig rig;
    const double rate = rig.rate;
    const int L = rig.engine.latencySamples();
    const Sound A = passBy(rate, 4.0);
    check(rig.learn(A), "learned");
    const auto r = rig.engine.result();
    if (r == nullptr || ! r->playable())
        return;

    // 0.2 s of the sound, then other audio: matched, but it stops holding before the output gets
    // there, so nothing is heard.
    {
        const auto cancelledBefore = rig.engine.cancelledStarts();
        const Sound in = join({ silence(0.3, rate), slice(A, 0.0, 0.2, rate), tone(1.0, rate, [](double) { return 330.0; }, kRich, 0.02, 9u), silence(0.6, rate) });
        const Sound out = rig.play(in);
        check(countDryMismatches(in, out, L, 0, static_cast<std::int64_t>(in.size())) == 0, "a match that did not hold is never heard: the input passes bit for bit");
        check(rig.engine.cancelledStarts() > cancelledBefore, "the start was cancelled");
    }
    // Another sound layered 14 dB under the learned one: the layer would be lost, so the input passes.
    {
        const Sound other = tone(4.0, rate, [](double) { return 330.0; }, kRich, 0.05, 777u);
        const float g = static_cast<float>(rms(A, 0, static_cast<std::int64_t>(A.size())) / rms(other, 0, static_cast<std::int64_t>(other.size())) * std::pow(10.0, -14.0 / 20.0));
        const Sound in = join({ silence(0.5, rate), mixIn(A, other, g), silence(1.0, rate) });
        const Sound out = rig.play(in);
        check(countDryMismatches(in, out, L, 0, static_cast<std::int64_t>(in.size())) == 0, "a layer 14 dB under the learned sound: the input passes");
    }
    // A fade-out on the player: the flattened sound fades with it.
    {
        Sound f = A;
        const double T = static_cast<double>(A.size()) / rate;
        for (std::size_t i = 0; i < f.size(); ++i)
        {
            const double t = static_cast<double>(i) / rate;
            if (t > T - 1.5)
                f[i] *= static_cast<float>(std::pow(10.0, (-60.0 * (t - (T - 1.5)) / 1.5) / 20.0));
        }
        const Sound in = join({ silence(0.5, rate), f, silence(1.0, rate) });
        const Sound out = rig.play(in);
        double worst = 0.0;
        for (const double t0 : { T - 1.4, T - 1.15, T - 0.9 })   // while the (shorter) render still plays
        {
            const auto a = static_cast<std::int64_t>((0.5 + t0) * rate), b = a + static_cast<std::int64_t>(0.05 * rate);
            // The flattened sound at the learned level over the same output span, faded as the input is.
            const double inputDb = db(rms(in, a, b)) - db(rms(join({ silence(0.5, rate), A }), a, b));
            const double outputDb = db(rms(out, a + L, b + L));
            const auto N = rig.engine.lastMatchInput() - (rig.played - static_cast<std::int64_t>(in.size()));
            const auto offset = static_cast<std::int64_t>(std::llround(r->flattened.outputSampleFor(static_cast<double>(rig.engine.lastMatchSource())))) - N;
            Sound flatSpan(static_cast<std::size_t>(b - a));
            for (auto t = a; t < b; ++t)
            {
                const auto k = t + offset;
                flatSpan[static_cast<std::size_t>(t - a)] = k >= 0 && k < static_cast<std::int64_t>(r->flattened.channels[0].size()) ? r->flattened.channels[0][static_cast<std::size_t>(k)] : 0.0f;
            }
            worst = std::max(worst, std::abs(outputDb - db(rms(flatSpan, 0, b - a)) - inputDb));
        }
        check(worst < 1.0, "a fade on the player comes out as a fade (within 1 dB): " + num(worst, 2) + " dB");
    }
    // Twice as loud (another, normalised copy): the flattened sound comes out twice as loud.
    {
        Sound loud = A;
        for (auto& v : loud)
            v *= 2.0f;
        const Sound in = join({ silence(0.5, rate), loud, silence(1.0, rate) });
        const Sound out = rig.play(in);
        const auto N = rig.engine.lastMatchInput() - (rig.played - static_cast<std::int64_t>(in.size()));
        const auto S = rig.engine.lastMatchSource();
        const auto offset = static_cast<std::int64_t>(std::llround(r->flattened.outputSampleFor(static_cast<double>(S)))) - N;
        double worst = 0.0;
        for (auto t = N + static_cast<std::int64_t>(0.2 * rate); t < N + static_cast<std::int64_t>(2.0 * rate); ++t)
            worst = std::max(worst, static_cast<double>(std::abs(out[static_cast<std::size_t>(t + L)] - 2.0f * r->flattened.channels[0][static_cast<std::size_t>(t + offset)])));
        check(worst < 1.0e-5, "played 6 dB up: the render, 6 dB up: " + num(worst, 7));
    }
    // A reset mid-sound, then silence: nothing of the old sound comes back.
    {
        rig.play(join({ silence(0.5, rate), slice(A, 0.0, 2.0, rate) }));
        rig.engine.requestReset();
        const Sound out = rig.play(silence(1.0, rate));
        int loud = 0;
        for (const float v : out)
            loud += identical(v, 0.0f) ? 0 : 1;
        check(loud == 0, "after reset() with silent input the output is silent: " + std::to_string(loud) + " samples were not");
    }
    // Unrelated noise for 5 s: the search backs off instead of running all the time.
    {
        const auto before = rig.engine.searchesDone();
        const auto started = std::chrono::steady_clock::now();
        rig.play(noise(5.0, rate, 0.2f, 9));
        const auto searches = rig.engine.searchesDone() - before;
        std::printf("    -> 5 s of unrelated noise: %llu searches, %.0f ms offline\n", static_cast<unsigned long long>(searches), msSince(started));
        // The back-off, plus up to 8 quick retries at the start of the sound (20 ms apart).
        check(searches <= 20, "unrelated audio is searched a few times, not continuously: " + std::to_string(searches));
    }
}

void engineAmbiguousAndStereo()
{
    beginCase("engine: hum, a held tone and swapped channels are not the learned sound");
    const double rate = 48000.0;
    {
        Rig rig;
        const auto humAt = [rate](double seconds, double phase, std::uint32_t seed)
        {
            Sound h = tone(seconds, rate, [](double) { return 50.0; }, { 1.0, 0.5, 0.3, 0.2 }, 0.002, seed);
            const auto shift = static_cast<std::size_t>(phase / (kTwoPi * 50.0) * rate);
            std::rotate(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(shift % h.size()), h.end());
            for (auto& v : h)
                v *= 0.02f;   // -46 dBFS
            return h;
        };
        Sound A = humAt(5.0, 0.0, 1u);
        const Sound pb = passBy(rate, 4.0);
        for (std::size_t i = 0; i < pb.size(); ++i)
            A[i + static_cast<std::size_t>(rate)] += pb[i];
        check(rig.learn(A), "learned a sound with a second of hum before it");
        const Sound in = join({ silence(0.5, rate), humAt(3.0, 2.0, 77u), silence(1.0, rate) });
        const Sound out = rig.play(in);
        check(countDryMismatches(in, out, rig.engine.latencySamples(), 0, static_cast<std::int64_t>(in.size())) == 0, "another file of the same hum passes untouched");
    }
    {
        Rig rig;
        const Sound A = tone(4.0, rate, [](double t) { return 200.0 * std::exp2(0.3 * std::sin(kTwoPi * t / 4.0) / 12.0); }, kRich, 0.01, 11u);
        check(rig.learn(A), "learned a held note");
        const Sound in = join({ silence(0.5, rate), tone(3.0, rate, [](double) { return 201.0; }, kRich, 0.01, 12345u), silence(1.0, rate) });
        const Sound out = rig.play(in);
        check(countDryMismatches(in, out, rig.engine.latencySamples(), 0, static_cast<std::int64_t>(in.size())) == 0, "a different steady tone at that pitch passes untouched");
    }
    {
        Rig rig;
        const Sound left = passBy(rate, 3.0);
        const Sound hiss = noise(3.0, rate, 0.05f, 4);
        Sound right(left.size());
        for (std::size_t i = 0; i < right.size(); ++i)
            right[i] = 0.3f * left[i] + hiss[i];
        check(rig.learn(core::Channels { left, right }), "learned in stereo");
        const core::Channels in { join({ silence(0.5, rate), right, silence(1.0, rate) }), join({ silence(0.5, rate), left, silence(1.0, rate) }) };
        const auto out = rig.play(in);
        const int L = rig.engine.latencySamples();
        check(countDryMismatches(in[0], out[0], L, 0, static_cast<std::int64_t>(in[0].size())) == 0
                  && countDryMismatches(in[1], out[1], L, 0, static_cast<std::int64_t>(in[1].size())) == 0,
              "the same file with its channels swapped passes untouched");
    }
}

void engineStopsAndTails()
{
    beginCase("engine: stopping in a quiet gap, the tail over the next file, the two timelines");
    const double rate = 48000.0;
    {
        Rig rig;
        const Sound s1 = passBy(rate, 1.5), s2 = passBy(rate, 1.5, 5.0, 0.02);
        const Sound gap = noise(0.6, rate, 0.0015f, 3);   // room tone, about -60 dBFS
        check(rig.learn(join({ s1, gap, s2 })), "learned two sounds with room tone between");
        const int L = rig.engine.latencySamples();
        const Sound in = join({ silence(0.5, rate), s1, slice(gap, 0.0, 0.1, rate), silence(2.5, rate) });
        const Sound out = rig.play(in);
        const auto stop = static_cast<std::int64_t>(0.5 * rate) + static_cast<std::int64_t>(s1.size() + static_cast<std::size_t>(0.1 * rate));
        check(countDryMismatches(in, out, L, stop, static_cast<std::int64_t>(in.size())) == 0, "stopped in the gap: the output stops too");
    }
    {
        Rig rig;
        const Sound A = passBy(rate, 4.0);
        check(rig.learn(A), "learned");
        setTarget(rig, core::Target::LowPoint);
        const auto r = rig.engine.result();
        check(r != nullptr && static_cast<std::int64_t>(r->flattened.channels[0].size()) > r->learned->locator.length(), "a longer render");
        const int L = rig.engine.latencySamples();
        const int fade = static_cast<int>(std::llround(Engine::kFadeSeconds * rate)) + 2;
        const Sound B = tone(2.0, rate, [](double) { return 523.0; }, kRich, 0.05, 99u);
        for (const double gapSeconds : { 0.0, 0.3 })
        {
            const Sound in = join({ silence(0.5, rate), A, silence(gapSeconds, rate), B, silence(1.0, rate) });
            const Sound out = rig.play(in);
            const auto b0 = static_cast<std::int64_t>(0.5 * rate) + static_cast<std::int64_t>(A.size()) + static_cast<std::int64_t>(gapSeconds * rate);
            check(countDryMismatches(in, out, L, b0 + fade, static_cast<std::int64_t>(in.size())) == 0,
                  "the next file " + num(gapSeconds, 1) + " s after the sound comes through at once, not under the tail");
        }
        // With silence after the sound, the tail plays out.
        const Sound in = join({ silence(0.5, rate), A, silence(2.0, rate) });
        bool sawTail = false;
        rig.onBlock = [&] { sawTail = sawTail || rig.engine.playback() == Engine::Playback::Tail; };
        const Sound out = rig.play(in);
        rig.onBlock = {};
        const auto N = rig.engine.lastMatchInput() - (rig.played - static_cast<std::int64_t>(in.size()));
        const auto fc = compareFlat(core::Channels { out }, *r, L, N, rig.engine.lastMatchSource(), static_cast<std::int64_t>(in.size()), rate, 1);
        check(sawTail && fc.compared > static_cast<int>(4.5 * rate) && fc.bad == 0, "into silence the whole longer render plays, tail and all");
    }
    for (const bool keepLength : { false, true })
    {
        // The input is the first 2 s of the learned sound, then silence.
        Rig rig;
        const Sound A = passBy(rate, 4.0);
        check(rig.learn(A), "learned");
        setTarget(rig, core::Target::HighPoint, keepLength);
        const auto r = rig.engine.result();
        if (r == nullptr || ! r->playable())
            continue;
        const int L = rig.engine.latencySamples();
        const auto cut = static_cast<std::int64_t>(0.5 * rate) + static_cast<std::int64_t>(2.0 * rate);
        const Sound in = join({ silence(0.5, rate), slice(A, 0.0, 2.0, rate), silence(1.5, rate) });
        const Sound out = rig.play(in);
        const auto N = rig.engine.lastMatchInput() - (rig.played - static_cast<std::int64_t>(in.size()));
        const auto S = rig.engine.lastMatchSource();
        std::int64_t last = -1;
        for (std::int64_t t = 0; t + L < static_cast<std::int64_t>(out.size()); ++t)
            if (! identical(out[static_cast<std::size_t>(t + L)], in[static_cast<std::size_t>(t)]))
                last = t;
        const auto offset = static_cast<std::int64_t>(std::llround(r->flattened.outputSampleFor(static_cast<double>(S)))) - N;
        const double heard = (r->flattened.sourceSampleFor(static_cast<double>(last + offset)) - static_cast<double>(S - N)) / rate - 0.5;
        if (! keepLength)
        {
            // Varispeed (High Point: a shorter render, which runs ahead of the input). When the
            // input stops being the sound, the output returns to the input at that input position.
            // The last moment of the sound heard is then a little past what the input had.
            check(last >= 0 && last < cut, "Varispeed: the flattened sound stops at the input's cut (" + num(static_cast<double>(last - static_cast<std::int64_t>(0.5 * rate)) / rate, 3) + " s), then the input");
            std::printf("    -> Varispeed: the last flattened sample heard is the sound's moment %.3f s; the input had 0-2.000 s\n", heard);
        }
        else
        {
            check(last >= 0 && last < cut && heard <= 2.0 + 1.0e-3, "Keep Length: nothing past the checked input is played (last moment " + num(heard, 3) + " s)");
        }
    }
}

void engineRenderSwitchAndRelearn()
{
    beginCase("engine: offline right after a change uses the new render; a new capture plays nothing until analysed");
    const double rate = 48000.0;
    Rig rig;
    const Sound A = passBy(rate, 3.0);
    check(rig.learn(A), "learned");
    const int L = rig.engine.latencySamples();
    const auto before = rig.engine.result();
    auto o = rig.engine.options();
    o.target = core::Target::LowPoint;
    rig.engine.setOptions(o);   // and bounce at once, without waiting
    std::vector<std::uint64_t> heard;
    rig.onBlock = [&]
    {
        if (rig.engine.playPosition() >= 0 && (heard.empty() || heard.back() != rig.engine.playGeneration()))
            heard.push_back(rig.engine.playGeneration());
    };
    const Sound in = join({ silence(0.5, rate), A, silence(2.0, rate) });
    const Sound out = rig.play(in);
    rig.onBlock = {};
    const auto after = rig.engine.result();
    check(after != nullptr && after != before && heard.size() == 1 && heard[0] == after->generation && ! rig.engine.offlineWaitGaveUp(),
          "the bounce waited for the new render and heard only it");
    if (after != nullptr && after->playable())
    {
        const auto N = rig.engine.lastMatchInput() - (rig.played - static_cast<std::int64_t>(in.size()));
        const auto fc = compareFlat(core::Channels { out }, *after, L, N, rig.engine.lastMatchSource(), static_cast<std::int64_t>(in.size()), rate, 1);
        check(fc.compared > static_cast<int>(2.5 * rate) && fc.bad == 0, "and played it exactly");
    }

    // Learn another sound: while it is analysed, the old one is not played.
    rig.engine.armLearn();
    rig.play(join({ silence(0.2, rate), tone(3.0, rate, [](double t) { return 400.0 + 100.0 * t; }, kRich, 0.02, 8u), silence(1.3, rate) }));
    check(eventually([&] { return rig.engine.state() == Engine::State::Analysing; }, 2.0), "analysing the new sound");
    const Sound again = join({ silence(0.3, rate), A, silence(0.5, rate) });
    const Sound out2 = rig.play(again);
    const bool wasAnalysing = rig.engine.state() == Engine::State::Analysing;
    check(! wasAnalysing || countDryMismatches(again, out2, L, 0, static_cast<std::int64_t>(again.size())) == 0, "the old sound passes dry while the new one is analysed");
    rig.engine.waitUntilIdle(60.0);
}

void engineCancelLearn()
{
    beginCase("engine: cancelLearn() keeps the sound learned before, from armed, listening and analysing");
    const double rate = 48000.0;
    const Sound A = passBy(rate, 2.5);
    const Sound B = tone(3.0, rate, [](double t) { return 400.0 + 100.0 * t; }, kRich, 0.02, 8u);
    const auto playsA = [&](Rig& rig)
    {
        const auto r = rig.engine.result();
        if (r == nullptr || ! r->playable())
            return false;
        const Sound in = join({ silence(0.4, rate), A, silence(0.8, rate) });
        const auto base = rig.played;
        const Sound out = rig.play(in);
        const auto N = rig.engine.lastMatchInput() - base;
        const auto fc = compareFlat(core::Channels { out }, *r, rig.engine.latencySamples(), N, rig.engine.lastMatchSource(), static_cast<std::int64_t>(in.size()), rate, 1);
        return N >= 0 && fc.compared > static_cast<int>(1.5 * rate) && fc.bad == 0;
    };
    Rig rig;
    check(rig.learn(A), "learned");
    const auto learnedBefore = rig.engine.result();
    for (int phase = 0; phase < 3; ++phase)
    {
        rig.engine.armLearn();
        const char* what = phase == 0 ? "armed" : phase == 1 ? "listening" : "analysing";
        if (phase == 1)
            rig.play(join({ silence(0.2, rate), slice(B, 0.0, 1.0, rate) }));
        if (phase == 2)
        {
            rig.play(join({ silence(0.2, rate), B, silence(1.3, rate) }));
            check(eventually([&] { return rig.engine.state() == Engine::State::Analysing; }, 2.0), "analysing B");
        }
        rig.engine.cancelLearn();
        check(rig.engine.state() == Engine::State::Ready && ! rig.engine.isLearnArmed() && rig.engine.result() == learnedBefore,
              std::string("cancelled while ") + what + ": back to Ready with the earlier sound");
        check(playsA(rig), std::string("and it plays flattened again (cancelled while ") + what + ")");
    }
    check(rig.engine.waitUntilIdle(30.0) && rig.engine.result() == learnedBefore, "the abandoned analysis never replaced it");
    for (int phase = 0; phase < 3; ++phase)
    {
        Rig fresh;
        fresh.engine.armLearn();
        if (phase == 1)
            fresh.play(join({ silence(0.2, rate), slice(B, 0.0, 1.0, rate) }));
        if (phase == 2)
        {
            fresh.play(join({ silence(0.2, rate), B, silence(1.3, rate) }));
            eventually([&] { return fresh.engine.state() == Engine::State::Analysing; }, 2.0);
        }
        fresh.engine.cancelLearn();
        check(fresh.engine.state() == Engine::State::Idle && fresh.engine.result() == nullptr, "with nothing learned before, cancelling returns to Idle (" + std::to_string(phase) + ")");
        check(fresh.engine.waitUntilIdle(30.0) && fresh.engine.result() == nullptr, "and stays there");
    }
}

void engineTelemetry()
{
    beginCase("engine: what the output is doing, for the window");
    const double rate = 48000.0;
    Rig rig;
    const Sound A = passBy(rate, 4.0);
    check(rig.learn(A), "learned");
    std::vector<Engine::Playback> seen;
    bool currentWhileFlat = true;
    rig.onBlock = [&]
    {
        const auto p = rig.engine.playback();
        if (seen.empty() || seen.back() != p)
            seen.push_back(p);
        if (p == Engine::Playback::Flattened)
            currentWhileFlat = currentWhileFlat && rig.engine.playingCurrentRender();
    };
    rig.play(join({ silence(0.5, rate), A, silence(1.0, rate) }));
    const std::vector<Engine::Playback> highPoint { Engine::Playback::Searching, Engine::Playback::Flattened, Engine::Playback::FlattenedEnded, Engine::Playback::Searching };
    check(seen == highPoint, "High Point (shorter): searching, flattened, flattened-ended, searching");
    check(currentWhileFlat, "and it was the current render");
    setTarget(rig, core::Target::LowPoint);
    seen.clear();
    rig.play(join({ silence(0.5, rate), A, silence(2.0, rate) }));
    const std::vector<Engine::Playback> lowPoint { Engine::Playback::Searching, Engine::Playback::Flattened, Engine::Playback::Tail, Engine::Playback::Searching };
    check(seen == lowPoint, "Low Point (longer): searching, flattened, tail, searching");
    rig.onBlock = {};
    rig.engine.clear();
    rig.play(silence(0.1, rate));
    check(rig.engine.playback() == Engine::Playback::Idle, "idle with nothing learned");
}

void engineCaptureFaults()
{
    beginCase("engine: Learn ends over a noise floor, survives a bad sample, captures while bypassed; Clear lets go");
    const double rate = 48000.0;
    Sound A = passBy(rate, 2.0);
    float peak = 0.0f;
    for (const float v : A)
        peak = std::max(peak, std::abs(v));
    for (auto& v : A)
        v *= 0.891f / peak;   // -1 dBFS
    for (const float floorPeak : { 0.00063f, 0.002f })   // -64 and -54 dBFS
    {
        Rig rig;
        const Sound bed = noise(12.0, rate, floorPeak, 4);
        Sound in = bed;
        for (std::size_t i = 0; i < A.size(); ++i)
            in[i + static_cast<std::size_t>(0.5 * rate)] += A[i];
        rig.engine.armLearn();
        rig.play(in);
        check(rig.engine.state() != Engine::State::Listening, "a noise floor at " + num(db(floorPeak), 0) + " dBFS ends Learn");
        check(rig.engine.waitUntilIdle(60.0), "analysed");
        const auto r = rig.engine.result();
        check(r != nullptr && r->learned->locator.length() < static_cast<std::int64_t>(A.size()) + static_cast<std::int64_t>(0.6 * rate) + 1024,
              "and the floor after the sound is trimmed");
    }
    {
        Rig rig;
        Sound bad = passBy(rate, 3.0);
        bad[static_cast<std::size_t>(1.0 * rate)] = std::numeric_limits<float>::quiet_NaN();
        check(rig.learn(bad), "learned a sound with a NaN in it");
        const auto r = rig.engine.result();
        bool finite = r != nullptr && r->playable();
        if (finite)
            for (const float v : r->flattened.channels[0])
                finite = finite && std::isfinite(v);
        check(finite, "the render is all finite");
        bad[static_cast<std::size_t>(1.0 * rate)] = 0.0f;
        const auto base = rig.played;
        const Sound out = rig.play(join({ silence(0.5, rate), slice(bad, 2.0, 3.0, rate), silence(1.0, rate) }));
        bool allFinite = true;
        for (const float v : out)
            allFinite = allFinite && std::isfinite(v);
        check(rig.engine.lastMatchInput() >= base && allFinite, "and it is still found, after the bad sample");
    }
    {
        Rig rig;
        rig.engine.armLearn();
        rig.play(join({ silence(0.2, rate), passBy(rate, 2.0), silence(1.3, rate) }), true);
        check(rig.engine.waitUntilIdle(60.0) && rig.engine.state() == Engine::State::Ready, "Learn captured the sound while bypassed");
    }
    {
        Rig rig;
        check(rig.learn(passBy(rate, 2.0)), "learned");
        std::weak_ptr<const Result> render = rig.engine.result();
        std::weak_ptr<const pitchflattener::Learned> learned = rig.engine.result()->learned;
        rig.play(join({ silence(0.5, rate), passBy(rate, 2.0), silence(0.5, rate) }));
        rig.engine.clear();
        // The registry keeps a sound an instance lets go of for a while (for a render instance
        // made just after); without that, the instance itself must hold nothing.
        pitchflattener::SoundRegistry::instance().clearForTesting();
        check(eventually([&] { rig.play(silence(0.02, rate)); return render.expired() && learned.expired(); }, 5.0), "Clear gives the learned sound's memory back");
    }
}

void engineLargeBlocks()
{
    beginCase("engine: the latency covers the prepared block size (a 32768-sample block in real time)");
    const double rate = 48000.0;
    const int block = 32768;
    check(Engine::latencyFor(rate, 512) == 19200 && Engine::latencyFor(rate, 2048) == 19200 && Engine::latencyFor(44100.0, 2048) == 17640
              && Engine::latencyFor(96000.0, 4096) == 38400 && Engine::latencyFor(192000.0, 4096) == 76800,
          "0.4 s for ordinary blocks (up to 2048 at 44.1/48 kHz, 4096 at 96/192 kHz)");
    const int L = Engine::latencyFor(rate, block);
    check(L >= Locator::nominalProbeLength(rate) + block + static_cast<int>(Engine::kSearchBudgetSeconds * rate), "probe + block + search budget for a big one: " + num(L / rate, 3) + " s");
    Rig rig(rate, block);
    const Sound A = passBy(rate, 4.0);
    check(rig.learn(A), "learned");
    check(rig.engine.latencySamples() == L, "reported");
    rig.engine.setNonRealtime(false);
    rig.fixedBlock = block;
    const auto r = rig.engine.result();
    const Sound in = join({ silence(0.75, rate), A, silence(1.5, rate) });
    // Paced in real time, one 683 ms block at a time.
    core::Channels io { in };
    std::vector<float*> ptrs(1);
    auto clock = std::chrono::steady_clock::now();
    for (std::size_t pos = 0; pos < in.size(); pos += static_cast<std::size_t>(block))
    {
        const int n = static_cast<int>(std::min<std::size_t>(in.size() - pos, static_cast<std::size_t>(block)));
        ptrs[0] = io[0].data() + pos;
        rig.engine.process(ptrs.data(), 1, n);
        clock += std::chrono::microseconds(static_cast<std::int64_t>(1.0e6 * n / rate));
        std::this_thread::sleep_until(clock);
    }
    const auto N = rig.engine.lastMatchInput() - rig.played;
    std::int64_t dry = 0;
    for (std::int64_t t = static_cast<std::int64_t>(0.75 * rate); t < static_cast<std::int64_t>(0.75 * rate + static_cast<double>(A.size())); ++t)
        dry += identical(io[0][static_cast<std::size_t>(t + L)], in[static_cast<std::size_t>(t)]) && ! identical(in[static_cast<std::size_t>(t)], 0.0f) ? 1 : 0;
    std::printf("    -> %lld samples of the sound passed unflattened (matched at %lld)\n", static_cast<long long>(dry), static_cast<long long>(N));
    check(r != nullptr && dry == 0, "none of the sound's start leaks through unflattened");
}

//==============================================================================
// The learned sound travelling with the state, and renders the host does not announce.
//==============================================================================

// A codec that keeps the floats as they are (the plug-in's own is FLAC: PitchFlattenerTests).
Engine::Hooks plainCodec(std::function<void()> whileDecoding = {})
{
    Engine::Hooks hooks;
    hooks.encode = [](const core::Channels& sound, double rate)
    {
        std::vector<char> bytes(sizeof(double) + sizeof(std::uint32_t) * 2);
        const auto channels = static_cast<std::uint32_t>(sound.size()), frames = static_cast<std::uint32_t>(sound[0].size());
        std::memcpy(bytes.data(), &rate, sizeof(double));
        std::memcpy(bytes.data() + sizeof(double), &channels, sizeof(channels));
        std::memcpy(bytes.data() + sizeof(double) + sizeof(channels), &frames, sizeof(frames));
        for (const auto& c : sound)
        {
            const auto* p = reinterpret_cast<const char*>(c.data());
            bytes.insert(bytes.end(), p, p + c.size() * sizeof(float));
        }
        return bytes;
    };
    hooks.decode = [whileDecoding](const std::vector<char>& bytes, core::Channels& sound, double& rate)
    {
        if (whileDecoding)
            whileDecoding();
        std::uint32_t channels = 0, frames = 0;
        std::memcpy(&rate, bytes.data(), sizeof(double));
        std::memcpy(&channels, bytes.data() + sizeof(double), sizeof(channels));
        std::memcpy(&frames, bytes.data() + sizeof(double) + sizeof(channels), sizeof(frames));
        sound.assign(channels, std::vector<float>(frames));
        const char* p = bytes.data() + sizeof(double) + 2 * sizeof(std::uint32_t);
        for (auto& c : sound)
        {
            std::memcpy(c.data(), p, frames * sizeof(float));
            p += frames * sizeof(float);
        }
        return true;
    };
    return hooks;
}

// Plays `in` in a rig and checks the output is the render from the match on, to the end of the input.
bool playsFlattened(Rig& rig, const Sound& in, const Result& r, std::int64_t soundStart, std::int64_t learnedSoundStart, std::string& why)
{
    const auto base = rig.played;
    const Sound out = rig.play(in);
    const auto N = rig.engine.lastMatchInput() - base;
    const auto S = rig.engine.lastMatchSource();
    const auto fc = compareFlat(core::Channels { out }, r, rig.engine.latencySamples(), N, S, static_cast<std::int64_t>(in.size()), rig.rate, 1);
    const bool fromStart = N == soundStart && S == learnedSoundStart + (N - soundStart);
    why = "matched at " + std::to_string(N) + " (sound at " + std::to_string(soundStart) + "), " + std::to_string(fc.bad) + " of " + std::to_string(fc.compared) + " differ";
    return fromStart && fc.compared > static_cast<int>(1.0 * rig.rate) && fc.bad == 0;
}

void engineRegistry()
{
    beginCase("engine: another instance takes the learned sound by its id (the registry)");
    const double rate = 48000.0;
    const Sound A = passBy(rate, 2.5);
    const Sound in = join({ silence(0.5, rate), A, silence(1.0, rate) });
    const auto soundStart = static_cast<std::int64_t>(0.5 * rate);
    std::uint64_t id = 0;
    std::shared_ptr<const Result> renderA;
    {
        Rig a;
        check(a.learn(A), "the first instance learned");
        renderA = a.engine.result();
        id = a.engine.learnedId();
        check(id != 0 && renderA != nullptr && renderA->learned->id == id, "the sound has an id");

        Rig b;
        check(! b.engine.adoptLearned(id ^ 0x5555, b.engine.options()), "an unknown id: nothing");
        check(b.engine.adoptLearned(id, a.engine.options()) && b.engine.state() == Engine::State::Ready && b.engine.result() == renderA,
              "a second instance takes it at once, render and all, when the options are the same");
        std::string why;
        check(playsFlattened(b, in, *renderA, soundStart, soundStartInLearned(a.engine), why), "and plays it exactly as the first does: " + why);

        auto low = b.engine.options();
        low.target = core::Target::LowPoint;
        check(b.engine.adoptLearned(id, low), "taken again with other options");
        check(b.engine.waitUntilIdle(60.0), "renders for them");
        const auto renderB = b.engine.result();
        check(renderB != nullptr && renderB != renderA && renderB->learned == renderA->learned && renderB->flattened.channels[0].size() > renderA->flattened.channels[0].size(),
              "its own render, from the same reading (no analysis)");

        b.engine.clear();
        check(b.engine.result() == nullptr && a.engine.result() == renderA, "Clear in the second leaves the first as it was");
        check(playsFlattened(a, in, *renderA, soundStart, soundStartInLearned(a.engine), why), "and the first still plays it: " + why);
    }
    // The first instance is gone; the registry keeps its sound a while for a render instance.
    Rig c;
    check(c.engine.adoptLearned(id, c.engine.options()) && c.engine.result() == renderA, "after the first instance is deleted, a new one still finds it");
    renderA.reset();
}

void engineFastWithoutFlag()
{
    beginCase("engine: a render the host does not announce (audio flat out, setNonRealtime(false)) is flattened from the right sample");
    const double rate = 48000.0;
    const Sound A = passBy(rate, 2.5);
    Rig rig;
    check(rig.learn(A), "learned");
    const auto r = rig.engine.result();
    if (r == nullptr || ! r->playable())
        return;
    const auto learnedStart = soundStartInLearned(rig.engine);
    rig.engine.setNonRealtime(false);
    rig.fixedBlock = 512;
    bool fastSeen = false;
    rig.onBlock = [&] { fastSeen = fastSeen || rig.engine.runningFasterThanRealTime(); };
    std::string why;
    for (const double lead : { 0.3, 0.0 })   // after a little silence, and from the file's very first sample
    {
        rig.engine.prepare(rate, 512);   // a new render pass
        rig.played = 0;
        const Sound in = join({ silence(lead, rate), A, silence(1.0, rate) });
        check(playsFlattened(rig, in, *r, firstSound(in), learnedStart, why), "flat out, sound " + num(lead, 1) + " s in: the whole sound flattened: " + why);
    }
    rig.onBlock = {};
    check(fastSeen, "the pace was seen to be faster than real time");
    check(! rig.engine.offlineWaitGaveUp(), "no wait gave up");

    // Paced in real time it is real time.
    rig.engine.prepare(rate, 512);
    rig.played = 0;
    rig.fixedBlock = 0;
    bool fastPaced = false;
    rig.onBlock = [&] { fastPaced = fastPaced || rig.engine.runningFasterThanRealTime(); };
    rig.play(core::Channels { join({ silence(0.3, rate), slice(A, 0.0, 1.0, rate) }) }, false, 1.0);
    rig.onBlock = {};
    check(! fastPaced, "a real-time paced run is never taken for a render");
}

void engineRestoredRenderInstance()
{
    beginCase("engine: a render instance given the sound (no registry) waits for the analysis and flattens the whole file");
    const double rate = 48000.0;
    const Sound A = passBy(rate, 2.5);
    std::uint64_t id = 0;
    std::shared_ptr<const std::vector<char>> stored;
    std::int64_t learnedStart = 0;
    {
        Rig a;
        a.engine.setHooks(plainCodec());
        check(a.learn(A), "learned");
        id = a.engine.learnedId();
        stored = a.engine.storedSound();
        learnedStart = soundStartInLearned(a.engine);
        check(stored != nullptr && ! stored->empty(), "compressed for the state once, when learned");
    }
    pitchflattener::SoundRegistry::instance().clearForTesting();   // another process: nothing to take

    Rig b;
    b.engine.setHooks(plainCodec());
    b.engine.setNonRealtime(false);   // and the host says nothing about rendering
    b.fixedBlock = 512;
    check(! b.engine.adoptLearned(id, b.engine.options()), "not in the registry");
    b.engine.restoreLearned(id, stored);
    check(b.engine.state() == Engine::State::Analysing && b.engine.learnedId() == id, "analysing the sound from the state");
    const Sound in = join({ A, silence(1.0, rate) });   // the file from its first sample, flat out at once
    const Sound out = b.play(in);
    const auto r = b.engine.result();
    check(r != nullptr && r->playable() && r->learned->id == id && r->learned->stored == stored, "restored: the same id, and the same bytes for its own state");
    if (r != nullptr && r->playable())
    {
        const auto N = b.engine.lastMatchInput();
        const auto fc = compareFlat(core::Channels { out }, *r, b.engine.latencySamples(), N, b.engine.lastMatchSource(), static_cast<std::int64_t>(in.size()), rate, 1);
        check(N == firstSound(in) && b.engine.lastMatchSource() == learnedStart + N && fc.compared > static_cast<int>(2.0 * rate) && fc.bad == 0,
              "the whole file flattened from its first sound: " + std::to_string(fc.bad) + " differ");
    }

    // The waits end: an analysis that takes longer than the limit is given up on, and the audio goes on.
    pitchflattener::SoundRegistry::instance().clearForTesting();
    Rig slow;
    slow.engine.setHooks(plainCodec([] { std::this_thread::sleep_for(std::chrono::milliseconds(1500)); }));
    slow.engine.setNonRealtime(true);
    slow.engine.setWaitLimitsForTesting(0.3, 0.3, 0.3);
    slow.engine.restoreLearned(id, stored);
    const auto started = std::chrono::steady_clock::now();
    const Sound quick = silence(0.1, rate);
    const Sound passed = slow.play(quick);
    const double took = msSince(started);
    check(slow.engine.offlineWaitGaveUp() && took < 1200.0, "a wait gives up at its limit (" + num(took, 0) + " ms), so a bounce can never hang");
    slow.engine.waitUntilIdle(30.0);
}

//==============================================================================
// The head of a sound, as a host's transfer path may treat it differently from its audition path.
//==============================================================================
// Plays `in` through a fresh instance that takes the learned sound by id (as a render instance
// given the state does), flat out or paced, and counts the audible samples of the head
// [soundStart + allowance, soundStart + headSeconds) that come out dry.
struct HeadRun
{
    std::int64_t dry = 0, audible = 0;
};

HeadRun headRun(std::uint64_t id, const core::Options& options, const Sound& in, std::int64_t soundStart, std::int64_t allowance,
                double headSeconds, bool paced, bool resetsAfterFirstBlock)
{
    HeadRun h;
    Rig b;
    b.engine.adoptLearned(id, options);
    b.engine.setNonRealtime(false);   // the host says nothing (as the log showed)
    b.fixedBlock = 512;
    int blocks = 0;
    if (resetsAfterFirstBlock)
        b.onBlock = [&] { if (++blocks == 1) for (int k = 0; k < 3; ++k) b.engine.requestReset(); };
    const auto out = b.play(core::Channels { in }, false, paced ? 1.0 : 0.0)[0];
    const int L = b.engine.latencySamples();
    const auto end = std::min<std::int64_t>(soundStart + static_cast<std::int64_t>(headSeconds * b.rate), static_cast<std::int64_t>(in.size()) - L);
    for (auto t = soundStart + allowance; t < end; ++t)
    {
        const auto x = in[static_cast<std::size_t>(t)];
        if (std::abs(x) < Locator::kSilence)
            continue;
        ++h.audible;
        h.dry += identical(out[static_cast<std::size_t>(t + L)], x) ? 1 : 0;
    }
    return h;
}

void engineHeadOfSound()
{
    beginCase("engine: the head of the sound is flattened in a render instance (a de-click fade, a late first sample, resets after the first block)");
    const double rate = 48000.0;
    const Sound A = passBy(rate, 2.5);
    // Learned through a real-time-paced run, as an audition is.
    Rig a;
    a.engine.armLearn();
    a.play(core::Channels { join({ silence(0.2, rate), A, silence(1.3, rate) }) }, false, 1.0);
    check(a.engine.waitUntilIdle(60.0) && a.engine.state() == Engine::State::Ready, "learned in real time");
    const auto id = a.engine.learnedId();
    const auto options = a.engine.options();
    const auto lead = static_cast<std::int64_t>(0.5 * rate);
    const auto ms = [rate](double m) { return static_cast<std::int64_t>(m * rate / 1000.0); };

    for (const bool paced : { false, true })
    {
        const std::string how = paced ? " (paced in real time)" : " (flat out)";
        {
            Sound faded = A;
            for (std::int64_t i = 0; i < ms(5.0); ++i)
                faded[static_cast<std::size_t>(i)] *= static_cast<float>(i) / static_cast<float>(ms(5.0));
            const auto h = headRun(id, options, join({ silence(0.5, rate), faded, silence(1.0, rate) }), lead, ms(5.0), 0.3, paced, false);
            std::printf("    -> 5 ms fade-in%s: %lld of %lld audible head samples dry\n", how.c_str(), static_cast<long long>(h.dry), static_cast<long long>(h.audible));
            check(h.audible > 0 && h.dry == 0, "a 5 ms fade-in on the head: flattened from the end of the fade" + how);
        }
        {
            const Sound late(A.begin() + ms(3.0), A.end());
            const auto h = headRun(id, options, join({ silence(0.5, rate), late, silence(1.0, rate) }), lead, 0, 0.3, paced, false);
            std::printf("    -> first 3 ms missing%s: %lld of %lld audible head samples dry\n", how.c_str(), static_cast<long long>(h.dry), static_cast<long long>(h.audible));
            check(h.audible > 0 && h.dry == 0, "a head starting 3 ms late: flattened from its first sample" + how);
        }
        {
            // The sound from the file's first sample; three resets after the first block (that
            // block is cleared from the delay line by the reset, so it is silent either way).
            const auto h = headRun(id, options, join({ A, silence(1.0, rate) }), 0, 512, 0.3, paced, true);
            std::printf("    -> three resets after the first block%s: %lld of %lld audible head samples dry\n", how.c_str(), static_cast<long long>(h.dry), static_cast<long long>(h.audible));
            check(h.audible > 0 && h.dry == 0, "three resets after the first block: flattened from the first sample after them" + how);
        }
    }
    {
        // A different sound whose later part is the learned one: its own first 0.2 s stays as it is.
        const Sound other = tone(0.2, rate, [](double) { return 330.0; }, kRich, 0.02, 99u);
        const Sound in = join({ silence(0.5, rate), other, slice(A, 0.2, 2.5, rate), silence(1.0, rate) });
        Rig b;
        b.engine.adoptLearned(id, options);
        b.engine.setNonRealtime(false);
        b.fixedBlock = 512;
        const Sound out = b.play(in);
        check(countDryMismatches(in, out, b.engine.latencySamples(), lead, lead + static_cast<std::int64_t>(other.size())) == 0,
              "other material before the matching part is not replaced");
        check(b.engine.lastMatchInput() >= lead + static_cast<std::int64_t>(other.size()) - ms(1.0), "the match does not reach back into it");
    }
}

//==============================================================================
// A host that plays the file through a short filter on its transfer path (interpolation, a
// fraction of a sample of offset), and the comparison it logs.
//==============================================================================
Sound fractionalDelay(const Sound& x, double delay)
{
    // A windowed sinc: the output is the input `delay` samples later.
    constexpr int taps = 24;
    constexpr double pi = 3.14159265358979323846;
    Sound y(x.size(), 0.0f);
    for (std::size_t t = 0; t < x.size(); ++t)
    {
        double sum = 0.0;
        for (int j = -taps; j <= taps; ++j)
        {
            const auto k = static_cast<std::int64_t>(t) - j;
            if (k < 0 || k >= static_cast<std::int64_t>(x.size()))
                continue;
            const double u = j - delay + std::floor(delay);
            const double d = static_cast<double>(j) - (delay - std::floor(delay));
            const double sinc = std::abs(d) < 1.0e-12 ? 1.0 : std::sin(pi * d) / (pi * d);
            const double window = std::abs(d) < taps ? 0.5 + 0.5 * std::cos(pi * d / taps) : 0.0;
            (void) u;
            sum += x[static_cast<std::size_t>(k)] * sinc * window;
        }
        y[t] = static_cast<float>(sum);
    }
    const auto whole = static_cast<std::size_t>(std::floor(delay));
    Sound out(x.size(), 0.0f);
    for (std::size_t t = whole; t < x.size(); ++t)
        out[t] = y[t - whole];
    return out;
}

Sound linearPhase(const Sound& x, double phase)
{
    Sound y(x.size(), 0.0f);
    for (std::size_t t = 0; t + 1 < x.size(); ++t)
        y[t] = static_cast<float>((1.0 - phase) * x[t] + phase * x[t + 1]);
    return y;
}

Sound lowPass(const Sound& x, double cutoff, double rate)
{
    const double a = std::exp(-kTwoPi * cutoff / rate);
    Sound y(x.size());
    double state = 0.0;
    for (std::size_t t = 0; t < x.size(); ++t)
    {
        state = (1.0 - a) * x[t] + a * state;
        y[t] = static_cast<float>(state);
    }
    return y;
}

// Spot-like: a new instance given the sound by id, three resets before prepare, the sound 1.0 s
// in, flat out without the host saying so. Returns the audible samples of the sound that came out dry.
std::int64_t spotDry(std::uint64_t id, const core::Options& options, const core::Channels& sound, double rate, std::int64_t& matchedAt)
{
    Rig b;
    for (int k = 0; k < 3; ++k)
        b.engine.requestReset();
    b.engine.prepare(rate, 512);
    b.engine.adoptLearned(id, options);
    b.engine.setNonRealtime(false);
    b.fixedBlock = 512;
    core::Channels in;
    for (const auto& ch : sound)
        in.push_back(join({ silence(1.0, rate), ch, silence(1.0, rate) }));
    const auto out = b.play(in);
    matchedAt = b.engine.lastMatchInput() - static_cast<std::int64_t>(1.0 * rate);
    const int L = b.engine.latencySamples();
    std::int64_t dry = 0;
    const auto start = static_cast<std::int64_t>(1.0 * rate);
    for (auto t = start; t < start + static_cast<std::int64_t>(sound[0].size()) && t + L < static_cast<std::int64_t>(in[0].size()); ++t)
        for (std::size_t c = 0; c < in.size(); ++c)
            if (std::abs(in[c][static_cast<std::size_t>(t)]) >= Locator::kSilence && identical(out[c][static_cast<std::size_t>(t + L)], in[c][static_cast<std::size_t>(t)]))
            {
                ++dry;
                break;
            }
    return dry;
}

void engineFilteredPaths()
{
    beginCase("engine: the learned recording through a short filter (a host's interpolation) still matches from its first sample");
    const double rate = 48000.0;
    const Sound A = passBy(rate, 2.5, 0.0, 0.3);   // bright: road noise up to the top of the band
    Rig a;
    check(a.learn(A), "learned");
    const auto id = a.engine.learnedId();
    const auto options = a.engine.options();
    struct Case { const char* name; Sound sound; int whole; };
    const std::vector<Case> cases {
        { "0.3-sample delay", fractionalDelay(A, 0.3), 0 },
        { "0.5-sample delay", fractionalDelay(A, 0.5), 0 },
        { "linear interpolation, unity ratio, 0.37 phase", linearPhase(A, 0.37), 0 },
        { "first-order low-pass at 12 kHz", lowPass(A, 12000.0, rate), 0 },
        { "4.25-sample delay", fractionalDelay(A, 4.25), 4 },
    };
    for (const auto& c : cases)
    {
        // The residual at the sound's start before (a plain level) and after (a short filter).
        const int n = 4096;
        const auto fit = pitchflattener::matching::fitRecording(
            [&](int t) { return static_cast<double>(c.sound[static_cast<std::size_t>(t + c.whole)]); },
            [&](int t) { return t >= 0 && t < static_cast<int>(A.size()) ? static_cast<double>(A[static_cast<std::size_t>(t)]) : 0.0; }, n);
        std::int64_t matchedAt = -1;
        const auto dry = spotDry(id, options, core::Channels { c.sound }, rate, matchedAt);
        std::printf("    -> %s: residual %.1f dB with a plain level, %.1f dB through the fit (%.0f%% in 3 taps); matched %lld samples in; %lld samples dry\n", c.name,
                    10.0 * std::log10(std::max(fit.plainResidual, 1.0e-15)), 10.0 * std::log10(std::max(fit.filterResidual, 1.0e-15)), 100.0 * fit.delayShare,
                    static_cast<long long>(matchedAt), static_cast<long long>(dry));
        check(fit.filtered && fit.residual < pitchflattener::matching::kAcceptResidual, std::string(c.name) + ": the fit brings it under the accept figure");
        check(dry == 0 && matchedAt >= 0 && matchedAt <= c.whole + 2, std::string(c.name) + ": flattened from its first sample, all through");
    }

    // A dual-mono copy of a stereo learned sound has the same mix but no side: refused.
    Rig s;
    const Sound right = tone(2.5, rate, [](double t) { return 330.0 + 20.0 * t; }, kRich, 0.05, 31u);
    Sound mixR(A.size());
    for (std::size_t i = 0; i < A.size(); ++i)
        mixR[i] = 0.2f * A[i] + 0.4f * right[i];
    check(s.learn(core::Channels { A, mixR }), "learned in stereo");
    Sound mono(A.size());
    for (std::size_t i = 0; i < A.size(); ++i)
        mono[i] = 0.5f * (A[i] + mixR[i]);
    std::int64_t matchedAt = -1;
    const auto audible = static_cast<std::int64_t>(A.size());
    const auto dry = spotDry(s.engine.learnedId(), s.engine.options(), core::Channels { mono, mono }, rate, matchedAt);
    check(dry >= audible - 512, "a dual-mono copy of a stereo sound passes untouched (" + std::to_string(audible - dry) + " samples not dry)");
}

// The comparison lines the engine logs (house thread), kept for the tests.
std::mutex g_diagnosedMutex;
std::vector<pitchflattener::EngineEvent> g_diagnosed;
void diagnosingSink(void*, const pitchflattener::EngineEvent& e) noexcept
{
    g_logged.fetch_add(1);
    if (e.kind != pitchflattener::EngineEvent::Kind::Diagnosed)
        return;   // (the audio thread's events: nothing locked or allocated for them)
    const std::lock_guard<std::mutex> lock(g_diagnosedMutex);
    if (g_diagnosed.size() < 64)
        g_diagnosed.push_back(e);
}

void engineDiagnostics()
{
    beginCase("engine: a near miss logs what differs (one channel on both, a 0.3-sample delay, a balance)");
    const double rate = 48000.0;
    const Sound L = passBy(rate, 2.5, 0.0, 0.3);
    const Sound other = tone(2.5, rate, [](double t) { return 330.0 + 20.0 * t; }, kRich, 0.05, 31u);
    Sound R(L.size());
    for (std::size_t i = 0; i < L.size(); ++i)
        R[i] = 0.2f * L[i] + 0.4f * other[i];
    Rig a;
    check(a.learn(core::Channels { L, R }), "learned in stereo");
    const auto id = a.engine.learnedId();
    const auto options = a.engine.options();
    const auto run = [&](const core::Channels& in)
    {
        {
            const std::lock_guard<std::mutex> lock(g_diagnosedMutex);
            g_diagnosed.clear();
        }
        Rig b;
        b.engine.setLogSink(&diagnosingSink, nullptr);
        b.engine.adoptLearned(id, options);
        b.engine.setNonRealtime(false);
        b.fixedBlock = 512;
        core::Channels padded;
        for (const auto& ch : in)
            padded.push_back(join({ silence(0.5, rate), ch, silence(1.0, rate) }));
        b.play(padded);
        b.engine.waitUntilIdle(5.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::lock_guard<std::mutex> lock(g_diagnosedMutex);
        return g_diagnosed.empty() ? pitchflattener::EngineEvent {} : g_diagnosed.front();
    };
    const auto show = [](const char* what, const pitchflattener::EngineEvent& e)
    {
        std::printf("    -> %s: L/L %.1f, R/R %.1f, L/R %.1f, R/L %.1f, L/mix %.1f, R/mix %.1f, sum %.1f, diff %.1f dB; L-R level in %+.2f, learned %+.2f dB; "
                    "L-R correlation %.3f; best offset %+.3f (%.1f dB there, %.1f at 0); filter %.1f dB (%.0f%%)\n",
                    what, static_cast<double>(e.i[5]) / 100.0, static_cast<double>(e.i[6]) / 100.0, static_cast<double>(e.i[7]) / 100.0,
                    static_cast<double>(e.i[8]) / 100.0, static_cast<double>(e.i[9]) / 100.0, static_cast<double>(e.i[10]) / 100.0,
                    static_cast<double>(e.i[11]) / 100.0, static_cast<double>(e.i[12]) / 100.0, static_cast<double>(e.i[13]) / 100.0,
                    static_cast<double>(e.i[14]) / 100.0, static_cast<double>(e.i[15]) / 1000.0, e.d[1], e.d[2], e.d[3], e.d[4], 100.0 * e.d[5]);
    };
    {
        const auto e = run(core::Channels { L, L });
        show("left channel on both", e);
        check(e.kind == pitchflattener::EngineEvent::Kind::Diagnosed && e.i[5] < -10000 && e.i[8] < -10000 && e.i[15] > 990,
              "one channel on both: L/L and R/L exact, input L-R correlation 1");
    }
    {
        const auto e = run(core::Channels { fractionalDelay(L, 0.3), fractionalDelay(R, 0.3) });
        show("0.3-sample delay", e);
        check(e.kind == pitchflattener::EngineEvent::Kind::Diagnosed && std::abs(e.d[1] - 0.3) < 0.07 && e.d[2] < e.d[3] - 10.0,
              "a 0.3-sample delay: the best offset found, much better there than at 0");
    }
    {
        Sound quieter = R;
        for (auto& v : quieter)
            v *= 0.7f;
        const auto e = run(core::Channels { L, quieter });
        show("L/R gains 1.0/0.7", e);
        check(e.kind == pitchflattener::EngineEvent::Kind::Diagnosed && e.i[5] < -10000 && e.i[6] < -10000 && std::abs(static_cast<double>(e.i[13] - e.i[14]) / 100.0 - 3.10) < 0.1,
              "L/R gains 1.0/0.7: each channel exact, the balance 3.1 dB off the learned");
    }
}

void engineFitCost()
{
    beginCase("engine: the cost of the short-filter fit (the per-window check on the audio thread)");
    const double rate = 48000.0;
    const Sound x = passBy(rate, 1.0, 0.0, 0.3);
    const Sound y = fractionalDelay(x, 0.3);
    double fastest = 1.0e9;
    double sink = 0.0;
    for (int k = 0; k < 200; ++k)
    {
        const auto started = std::chrono::steady_clock::now();
        const auto fit = pitchflattener::matching::fitRecording([&](int t) { return static_cast<double>(y[static_cast<std::size_t>(t + 8000)]); },
                                                                [&](int t) { return static_cast<double>(x[static_cast<std::size_t>(t + 8000)]); }, 1024);
        fastest = std::min(fastest, msSince(started) * 1000.0);
        sink += fit.residual;
    }
    std::printf("    -> a 1024-sample window: %.1f microseconds (%.2f%% of the window's own 21.3 ms)\n", fastest, fastest / 21333.0 * 100.0);
    check(fastest < 200.0 && sink >= 0.0, "a window's check costs well under a millisecond");
}

} // namespace

void runLocatorTests()
{
    locatorBasics();
    locatorRejects();
    locatorRepeats();
    locatorResidual();
    locatorTiming();
}

void setEngineTestSeed(unsigned seed) { g_seedChosen = seed; }

void runEngineTests()
{
    // Random block sizes, from a seed printed here and accepted back (--seed) to replay a run.
    if (const char* fromEnvironment = std::getenv("PITCHFLATTENER_SEED"); g_seedChosen == 0 && fromEnvironment != nullptr)
        g_seedChosen = static_cast<unsigned>(std::strtoul(fromEnvironment, nullptr, 10));
    g_seed = g_seedChosen != 0 ? g_seedChosen : static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xffff) + 1u;
    std::printf("(block sizes from seed %u: --seed %u replays them)\n", g_seed, g_seed);
    g_audioAllocations.store(0);
    g_audioFrees.store(0);
    engineLearnAndPlay();
    engineChangeWhilePlaying();
    engineNothingToFlatten();
    engineInstances();
    engineCaptureEnds();
    engineChannels();
    engineRateAndRealtime();
    enginePendingAndLayers();
    engineAmbiguousAndStereo();
    engineStopsAndTails();
    engineRenderSwitchAndRelearn();
    engineCancelLearn();
    engineTelemetry();
    engineCaptureFaults();
    engineLargeBlocks();
    engineRegistry();
    engineFastWithoutFlag();
    engineRestoredRenderInstance();
    engineHeadOfSound();
    engineFilteredPaths();
    engineDiagnostics();
    engineFitCost();
    beginCase("engine: the audio thread never allocated or freed");
    check(g_logged.load() > 0, "the engines logged (" + std::to_string(g_logged.load()) + " events)");
    check(g_audioAllocations.load() == 0 && g_audioFrees.load() == 0,
          "allocations " + std::to_string(g_audioAllocations.load()) + ", frees " + std::to_string(g_audioFrees.load()));
}

} // namespace pftest

#ifdef PF_ENGINE_TESTS_MAIN
int main(int argc, char* argv[])
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--seed")
            pftest::setEngineTestSeed(static_cast<unsigned>(std::strtoul(argv[i + 1], nullptr, 10)));
    pftest::runLocatorTests();
    pftest::runEngineTests();
    std::printf("\n%d checks, %d failed\n", pftest::g_checks, pftest::g_failures);
    return pftest::g_failures == 0 ? 0 : 1;
}
#endif

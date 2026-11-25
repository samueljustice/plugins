// Swirl's Tape Flanger and Doubler test: the delay over time (read from the phase of a sine, not from the class),
// the null at zero, Varispeed's pitch and held offset, feedback that never runs away, the Doubler's delay law,
// Thicken's gain and its mono sum, that nothing allocates while it runs, and what each type costs.
// Plain C++, no JUCE: SwirlFx is free of it.
//
//   SwooshbucklerSwirlFx       prints what it measures and ends with "ALL PASSED" (exit code 0) or "n FAILED".

#include "SwirlFx.h"
#include "BusView.h"
#include "TestClock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

// Every allocation in the program is counted, so a stretch of processing can be shown to make none.
namespace { std::atomic<long> g_allocations { 0 }; }
void* operator new(std::size_t size)
{
    ++g_allocations;
    if (void* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace
{
    constexpr double c_pi = 3.14159265358979323846;
    int g_failures = 0;

    void check(bool ok, const std::string& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }

    std::string num(double v, int places = 3)
    {
        std::ostringstream s;
        s << std::fixed << std::setprecision(places) << v;
        return s.str();
    }

    double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }
    double cents(double ratio) { return 1200.0 * std::log2(ratio); }

    double rms(const std::vector<float>& x, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
        return std::sqrt(sum / static_cast<double>(std::max<size_t>(1, to - from)));
    }

    std::vector<float> sine(double hz, double fs, double seconds, float level)
    {
        std::vector<float> x(static_cast<size_t>(seconds * fs));
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = level * static_cast<float>(std::sin(2.0 * c_pi * hz * static_cast<double>(i) / fs));
        return x;
    }

    std::vector<float> noise(double fs, double seconds, float level, std::uint32_t seed)
    {
        sj::Random r;
        r.seed(seed);
        std::vector<float> x(static_cast<size_t>(seconds * fs));
        for (auto& v : x)
            v = level * r.bipolar();
        return x;
    }

    //==============================================================================
    /** What one run gives back: each lane's sound, and at the end of every chunk where the class says each lane's
        copy is and where the held offset is. */
    struct Run
    {
        std::array<std::vector<float>, 3> out;
        std::array<std::vector<float>, 3> offset;
        std::vector<float> hand;
        int chunk = 32;
        double fs = 48000.0;

        double timeOfChunk(size_t c) const { return static_cast<double>((c + 1) * static_cast<size_t>(chunk)) / fs; }
        size_t chunkAt(double seconds) const { return static_cast<size_t>(seconds * fs / chunk); }
    };

    struct Step
    {
        // (Invert on unless a part says otherwise: most of these are about the cancel. The default is off.)
        Step() { tape.invert = true; }
        SwirlFx::Tape tape;
        SwirlFx::Doubler doubler;
        float knob = 1.0f;
        std::array<float, 3> passNear {};
        float filterOpen = 0.0f;
    };
    using Control = std::function<void(double seconds, Step&, SwirlFx&)>;

    /** Runs the same input through `lanes` lanes, chunk by chunk; `control` sets each chunk's settings (it is given
        the time at the chunk's end). */
    Run run(SwirlFx& fx, bool tape, const std::vector<float>& in, double fs, int lanes, const Control& control, int chunk = 32)
    {
        Run r;
        r.chunk = chunk;
        r.fs = fs;
        for (int l = 0; l < lanes; ++l)
            r.out[static_cast<size_t>(l)] = in;
        Step step;
        std::array<float, 3> last {};
        for (size_t at = 0; at + static_cast<size_t>(chunk) <= in.size(); at += static_cast<size_t>(chunk))
        {
            const double t = static_cast<double>(at + static_cast<size_t>(chunk)) / fs;
            if (control)
                control(t, step, fx);
            SwirlFx::Lanes ln;
            ln.count = lanes;
            for (int l = 0; l < lanes; ++l)
                ln.lane[static_cast<size_t>(l)] = r.out[static_cast<size_t>(l)].data() + at;
            ln.passNearFrom = last;
            ln.passNearTo = step.passNear;
            ln.filterOpen = step.filterOpen;
            last = step.passNear;
            if (tape)
                fx.processTape(step.tape, step.knob, ln, chunk);
            else
                fx.processDoubler(step.doubler, step.knob, ln, chunk);
            for (size_t l = 0; l < 3; ++l)
                r.offset[l].push_back(fx.getLaneOffsetMs(static_cast<int>(l)));
            r.hand.push_back(fx.getOffsetMs());
        }
        return r;
    }

    Run runFresh(bool tape, const std::vector<float>& in, double fs, int lanes, const Control& control, int chunk = 32, std::uint32_t seed = 77u)
    {
        SwirlFx fx;
        fx.prepare(fs);
        fx.setSeed(seed);
        return run(fx, tape, in, fs, lanes, control, chunk);
    }

    //==============================================================================
    /** A sine's size and phase over time: the sound against a cosine and a sine at `hz`, averaged over whole cycles
        (fs / hz must be whole). The value at i covers the cycles centred on sample i. */
    struct Phasor
    {
        std::vector<double> re, im;
    };

    Phasor demod(const std::vector<double>& x, double hz, double fs, int cycles = 2)
    {
        const int window = static_cast<int>(std::lround(fs / hz)) * cycles;
        const size_t n = x.size();
        std::vector<double> sumRe(n + 1, 0.0), sumIm(n + 1, 0.0);
        for (size_t i = 0; i < n; ++i)
        {
            const double ph = 2.0 * c_pi * hz * static_cast<double>(i) / fs;
            sumRe[i + 1] = sumRe[i] + x[i] * std::cos(ph);
            sumIm[i + 1] = sumIm[i] - x[i] * std::sin(ph);
        }
        Phasor p;
        p.re.assign(n, 0.0);
        p.im.assign(n, 0.0);
        const size_t half = static_cast<size_t>(window / 2);
        for (size_t i = half; i + half < n; ++i)
        {
            p.re[i] = (sumRe[i + half] - sumRe[i - half]) * 2.0 / window;
            p.im[i] = (sumIm[i + half] - sumIm[i - half]) * 2.0 / window;
        }
        return p;
    }

    /** How late `wet` is against `dry` over time, in ms, from the phases of a sine at `hz` in both. The whole
        cycles are settled at `from` (seconds) by `anchorMs`, the nearest answer to it being taken; after that the
        phase is followed sample by sample. Samples before `from` are left at the anchor. */
    std::vector<double> delayFromPhase(const std::vector<double>& wet, const std::vector<double>& dry, double hz, double fs,
                                       double from, double to, double anchorMs)
    {
        const auto w = demod(wet, hz, fs), d = demod(dry, hz, fs);
        std::vector<double> out(wet.size(), anchorMs);
        const size_t a = static_cast<size_t>(from * fs), b = std::min(wet.size(), static_cast<size_t>(to * fs));
        const double periodMs = 1000.0 / hz;
        double previous = 0.0, turns = 0.0;
        for (size_t i = a; i < b; ++i)
        {
            // wet x conj(dry): the angle is how far the wet's phase is ahead (negative when it is late).
            const double re = w.re[i] * d.re[i] + w.im[i] * d.im[i];
            const double im = w.im[i] * d.re[i] - w.re[i] * d.im[i];
            const double late = -std::atan2(im, re) / (2.0 * c_pi); // cycles, -0.5 .. 0.5
            if (i == a)
                turns = std::round(anchorMs / periodMs - late);
            else if (late - previous > 0.5)
                turns -= 1.0;
            else if (late - previous < -0.5)
                turns += 1.0;
            previous = late;
            out[i] = (late + turns) * periodMs;
        }
        for (size_t i = b; i < out.size(); ++i)
            out[i] = out[b > a ? b - 1 : a];
        return out;
    }

    /** Least-squares slope of y over [a, b), per sample. */
    double slope(const std::vector<double>& y, size_t a, size_t b)
    {
        const double n = static_cast<double>(b - a);
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        for (size_t i = a; i < b; ++i)
        {
            const double x = static_cast<double>(i - a);
            sx += x; sy += y[i]; sxx += x * x; sxy += x * y[i];
        }
        return (n * sxy - sx * sy) / (n * sxx - sx * sx);
    }

    /** The Tape Flanger's copy and its dry, separated: the same run with Invert on and off (no feedback, so the
        two differ only in the copy's sign). */
    struct Split
    {
        std::vector<double> wet, dry;
        Run run;
    };

    Split splitTape(const std::vector<float>& in, double fs, int lane, int lanes, const Control& control, int chunk = 32)
    {
        auto with = [&](bool invert)
        {
            return runFresh(true, in, fs, lanes, [&](double t, Step& s, SwirlFx& fx)
            {
                if (control)
                    control(t, s, fx);
                s.tape.invert = invert;
                s.knob = 1.0f;
            }, chunk);
        };
        const Run plus = with(false), minus = with(true);
        Split s;
        s.run = plus;
        const double g = std::sqrt(0.5); // knob 1: both shares are cos(pi / 4)
        const auto& a = plus.out[static_cast<size_t>(lane)];
        const auto& b = minus.out[static_cast<size_t>(lane)];
        s.wet.resize(a.size());
        s.dry.resize(a.size());
        for (size_t i = 0; i < a.size(); ++i)
        {
            s.wet[i] = (static_cast<double>(a[i]) - static_cast<double>(b[i])) / (2.0 * g);
            s.dry[i] = (static_cast<double>(a[i]) + static_cast<double>(b[i])) / (2.0 * g);
        }
        return s;
    }

    /** The Doubler's copy on its own: what is left of a lane when the dry's share is taken away (knob 1). */
    std::vector<double> doublerWet(const Run& r, const std::vector<float>& in, int lane)
    {
        const double g = std::sqrt(0.5);
        const auto& o = r.out[static_cast<size_t>(lane)];
        std::vector<double> wet(o.size());
        for (size_t i = 0; i < o.size(); ++i)
            wet[i] = (static_cast<double>(o[i]) - g * static_cast<double>(in[i])) / g;
        return wet;
    }

    std::vector<double> asDouble(const std::vector<float>& x) { return std::vector<double>(x.begin(), x.end()); }

    /** The sum of x over [centre - reach, centre + reach], and where its weight lies (samples). */
    double sumAround(const std::vector<double>& x, double centre, int reach, double* centroid = nullptr)
    {
        const long c = std::lround(centre);
        double sum = 0.0, moment = 0.0;
        for (long i = c - reach; i <= c + reach; ++i)
        {
            if (i < 0 || i >= static_cast<long>(x.size()))
                continue;
            sum += x[static_cast<size_t>(i)];
            moment += x[static_cast<size_t>(i)] * static_cast<double>(i);
        }
        if (centroid != nullptr)
            *centroid = std::abs(sum) > 1.0e-12 ? moment / sum : 0.0;
        return sum;
    }

    double correlation(const std::vector<float>& a, const std::vector<float>& b, size_t from)
    {
        double ma = 0.0, mb = 0.0;
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = from; i < n; ++i) { ma += a[i]; mb += b[i]; }
        ma /= static_cast<double>(n - from);
        mb /= static_cast<double>(n - from);
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (size_t i = from; i < n; ++i)
        {
            ab += (a[i] - ma) * (b[i] - mb);
            aa += (a[i] - ma) * (a[i] - ma);
            bb += (b[i] - mb) * (b[i] - mb);
        }
        return ab / std::sqrt(std::max(1.0e-30, aa * bb));
    }

    //==============================================================================
    /** Today's Swirl at its fullest, one lane (six all-passes with a tanh in their loop, then the short flanger),
        as RigEngine::renderBus runs it: what the two types' cost is held against. */
    struct ClassicLane
    {
        std::array<sj::Allpass1, 6> stages;
        sj::DelayLine line;
        float loop = 0.0f, comb = 0.0f;

        void run(float* x, int frames, float a0, float a1, float ms0, float ms1, float msToSamples)
        {
            const float step = 1.0f / static_cast<float>(frames);
            for (int i = 0; i < frames; ++i)
            {
                const float t = static_cast<float>(i + 1) * step;
                const float a = a0 + (a1 - a0) * t;
                float y = x[i] + 0.55f * loop;
                for (auto& s : stages)
                    y = s.process(y, a);
                loop = sj::fastTanh(y);
                float o = x[i] + 0.5f * (y - x[i]);
                line.push(o + 0.6f * comb);
                comb = line.read(static_cast<double>((ms0 + (ms1 - ms0) * t) * msToSamples));
                o += 0.5f * (comb - o);
                x[i] = o;
            }
        }
    };

    double secondsFor(const std::function<void()>& work, int tries = 5)
    {
        double best = 1.0e9;
        for (int i = 0; i < tries; ++i)
        {
            const auto t0 = std::chrono::steady_clock::now();
            work();
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        return best;
    }
}

int main()
{
    const double fs = 48000.0;

    std::cout << "Swirl types: the mix and the rings" << std::endl;
    {
        // The front wheel's mix law (equal power, half wet at the top of the wheel).
        check(SwirlFx::mixFor(0.0f) == 0.0f && std::abs(SwirlFx::mixFor(1.0f) - 0.5f) < 1.0e-6f, "mix: none at 0, half at the top of the wheel");
        SwirlFx fx;
        fx.prepare(48000.0);
        check(fx.getOffsetMs() == 0.0f, "varispeed: the held offset starts in the middle");
        check(fx.dryDelaySamples() == 384, "tape flanger: the dry ring is 8 ms in whole samples (384 at 48 kHz)");
        fx.prepare(44100.0);
        check(fx.dryDelaySamples() == 353, "tape flanger: 353 samples at 44.1 kHz");

        // With the wheel at 0 the Tape Flanger is its dry ring and nothing else; the Doubler is the input.
        const auto in = noise(fs, 0.5, 0.5f, 5u);
        const Run tape = runFresh(true, in, fs, 2, [](double, Step& s, SwirlFx&) { s.knob = 0.0f; s.tape.motion = SwirlFx::ByHand; s.tape.depthMs = 0.0f; });
        bool exact = true;
        for (size_t i = 384; i < tape.out[0].size(); ++i)
            exact = exact && tape.out[0][i] == in[i - 384] && tape.out[1][i] == in[i - 384];
        for (size_t i = 0; i < 384; ++i)
            exact = exact && tape.out[0][i] == 0.0f;
        check(exact, "tape flanger: at wheel 0 each lane is its input 384 samples late, bit for bit");
        const Run dbl = runFresh(false, in, fs, 2, [](double, Step& s, SwirlFx&) { s.knob = 0.0f; });
        check(dbl.out[0] == in && dbl.out[1] == in, "doubler: at wheel 0 each lane is its input, bit for bit (no dry delay)");

        // The stereo bed's sides take the same ring.
        SwirlFx sides;
        sides.prepare(fs);
        auto l = in, r = in;
        for (auto& v : r) v = -v;
        for (size_t at = 0; at + 32 <= l.size(); at += 32)
            sides.delaySides(l.data() + at, r.data() + at, 32);
        bool sidesExact = true;
        for (size_t i = 384; i + 32 < l.size(); ++i)
            sidesExact = sidesExact && l[i] == in[i - 384] && r[i] == -in[i - 384];
        check(sidesExact, "tape flanger: delaySides() is the same 384 samples on both sides");
    }

    std::cout << "Tape Flanger: the null at zero" << std::endl;
    {
        // By Hand with Depth at 0 and the offset in the middle: the copy lies on the dry, sample for sample (nothing
        // is between the two but the move). Inverted, at the top of the wheel, the two cancel, alike at every
        // frequency, level and sample rate; not inverted they add to 3 dB more.
        auto nullAt = [](double hz, double rate, bool invert, float level)
        {
            const auto in = sine(hz, rate, 0.5, level);
            const Run r = runFresh(true, in, rate, 2, [invert](double, Step& s, SwirlFx&) { s.tape.motion = SwirlFx::ByHand; s.tape.depthMs = 0.0f; s.tape.invert = invert; s.knob = 1.0f; });
            const size_t a = in.size() / 2, b = in.size() - 64;
            return db(rms(r.out[0], a, b) / rms(in, a, b));
        };
        std::string row;
        for (double hz : { 100.0, 300.0, 1000.0, 3000.0, 6000.0, 10000.0 })
            row += "  " + num(hz, 0) + " Hz " + num(nullAt(hz, fs, true, 0.1f), 1) + " dB";
        std::cout << "         inverted, d = 0, -20 dBFS sine, 48 kHz:" << row << std::endl;
        const double at1k = nullAt(1000.0, fs, true, 0.1f);
        check(at1k <= -25.0, "null: 1 kHz falls by at least 25 dB at d = 0 with Invert (" + num(at1k, 1) + " dB)");
        for (double rate : { 44100.0, 96000.0, 192000.0 })
        {
            const double v = nullAt(1000.0, rate, true, 0.1f);
            check(v <= -25.0, "null: the same at " + num(rate, 0) + " Hz (" + num(v, 1) + " dB)");
        }
        // The top of the band, loud and quiet, at every rate: the same cancel (the measure's floor is float's).
        double shallowest = -1000.0;
        std::string everywhere;
        for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            for (double hz : { 100.0, 5000.0, 10000.0, 15000.0, 18000.0 })
                for (float level : { 0.05f, 1.0f, 2.0f })
                    shallowest = std::max(shallowest, nullAt(hz, rate, true, level));
        std::cout << "         100 Hz to 18 kHz, sine peaks 0.05 to 2.0, 44.1 to 192 kHz: the shallowest is " << num(shallowest, 1) << " dB" << std::endl;
        check(shallowest <= -100.0, "null: as deep at the top of the band, at any level and at every sample rate (" + num(shallowest, 1) + " dB at the least)");
        check(! SwirlFx::Tape().invert, "Invert is off unless asked for (the copy and the dry add where they meet)");
        {
            // By Hand, untouched: the copy waits Depth behind the dry (it is not left sitting in the cancel).
            SwirlFx fx;
            fx.prepare(fs);
            SwirlFx::Tape t;
            t.motion = SwirlFx::ByHand;
            std::vector<float> a(32, 0.0f), b(32, 0.0f);
            SwirlFx::Lanes lanes;
            lanes.lane = { a.data(), b.data(), nullptr };
            for (int i = 0; i < 400; ++i)
                fx.processTape(t, 1.0f, lanes, 32);
            check(std::abs(fx.getLaneOffsetMs(0) - t.depthMs) < 1.0e-3f && fx.getOffsetMs() == 0.0f,
                  "By Hand at rest: the copy waits Depth behind (" + num(fx.getLaneOffsetMs(0), 3) + " ms), the held offset in the middle");
        }
        {
            // The comb the plate over a bus draws (busview::swirlTransfer, from the gains and the place the SwirlFx
            // reports) against the Tape Flanger driven with sine tones: the copy held 2 ms behind (By Hand) and
            // 1.2 ms ahead (With the Pass at the pass's closest point), Feedback 50%, 100% and -100%, inverted and
            // not, from 180 Hz to 15 kHz, at 48 kHz (the copy read with the sinc) and 192 kHz (with the cubic).
            double worst = 0.0;
            int points = 0;
            std::string worstAt;
            for (const double rate : { 48000.0, 192000.0 })
                for (const bool ahead : { false, true })
                    for (const float feedback : { 0.5f, 1.0f, -1.0f })
                        for (const bool invert : { false, true })
                            for (const double hz : { 180.0, 610.0, 2400.0, 6100.0, 11000.0, 15000.0 })
                            {
                                SwirlFx fx;
                                fx.prepare(rate);
                                SwirlFx::Tape t;
                                t.motion = ahead ? SwirlFx::WithPass : SwirlFx::ByHand;
                                t.depthMs = ahead ? 4.0f : 2.0f;
                                t.wobble = 0.0f;
                                t.feedback = feedback;
                                t.invert = invert;
                                std::vector<float> a(32, 0.0f);
                                SwirlFx::Lanes lanes;
                                lanes.lane = { a.data(), nullptr, nullptr };
                                lanes.count = 1;
                                if (ahead)
                                    lanes.passNearFrom = lanes.passNearTo = { 1.0f, 1.0f, 1.0f };
                                const int settle = static_cast<int>(rate * 0.6), count = static_cast<int>(rate / 4.0);
                                double re = 0.0, im = 0.0;
                                for (int n = 0; n < settle + count; n += 32)
                                {
                                    for (int i = 0; i < 32; ++i)
                                        a[static_cast<size_t>(i)] = 0.02f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * hz * (n + i) / rate));
                                    fx.processTape(t, 1.0f, lanes, 32);
                                    if (n >= settle)
                                        for (int i = 0; i < 32; ++i)
                                        {
                                            const double ph = 2.0 * 3.14159265358979323846 * hz * (n + i) / rate;
                                            re += a[static_cast<size_t>(i)] * std::cos(ph);
                                            im += a[static_cast<size_t>(i)] * std::sin(ph);
                                        }
                                }
                                const double gain = 2.0 * std::sqrt(re * re + im * im) / (count / 32 * 32) / 0.02;
                                busview::SwirlLook look;
                                look.type = busview::SwirlTape;
                                look.offsetMs = fx.getLaneOffsetMs(0);
                                look.dryGain = fx.getDryGain();
                                look.wetGain = fx.getWetGain();
                                look.loopGain = fx.getLoopGain(0);
                                look.loopMs = static_cast<float>(fx.loopFloorMs());
                                const double drawn = std::abs(busview::swirlTransfer(look, hz, rate));
                                if (drawn < 0.1 || gain < 0.1) // a notch: under -20 dB the sum is all rounding
                                    continue;
                                const double err = std::abs(20.0 * std::log10(gain / drawn));
                                if (err > worst)
                                {
                                    worst = err;
                                    worstAt = num(rate / 1000.0, 0) + " kHz, " + num(look.offsetMs, 2) + " ms, Feedback "
                                              + std::to_string(static_cast<int>(std::lround(feedback * 100.0f))) + "%, " + num(hz, 0) + " Hz";
                                }
                                ++points;
                            }
            check(worst < 0.5 && points >= 100, "the comb the plate draws is the Tape Flanger's measured gain within " + num(worst, 3) + " dB at "
                                                    + std::to_string(points) + " points (2 ms behind and 1.2 ms ahead, Feedback 50%, 100%, -100%, "
                                                    "inverted and not, 180 Hz to 15 kHz, 48 and 192 kHz; worst at " + worstAt + ")");
        }
        const double sum = nullAt(1000.0, fs, false, 0.1f);
        check(std::abs(sum - 3.01) < 0.1, "not inverted, the copy and the dry add to +3 dB at d = 0 (" + num(sum, 2) + " dB)");

        // The copy's top end against the dry: the same (the low-pass and the clip are the loop's).
        auto copyLevel = [](double hz, double rate)
        {
            const auto tone = sine(hz, rate, 0.25, 0.1f);
            const Split s = splitTape(tone, rate, 0, 1, [](double, Step& st, SwirlFx&) { st.tape.motion = SwirlFx::ByHand; st.tape.depthMs = 0.0f; });
            double w = 0.0, d = 0.0;
            for (size_t i = s.wet.size() / 2; i < s.wet.size() - 64; ++i)
            {
                w += s.wet[i] * s.wet[i];
                d += s.dry[i] * s.dry[i];
            }
            return db(std::sqrt(w / d));
        };
        std::string top;
        for (double rate : { 48000.0, 192000.0 })
            for (double hz : { 1000.0, 5000.0, 10000.0, 15000.0, 18000.0 })
                top += std::string(hz == 1000.0 ? (rate == 48000.0 ? "  at 48 kHz:" : "  at 192 kHz:") : "") + " " + num(hz / 1000.0, 0) + " kHz " + num(copyLevel(hz, rate), 2) + " dB";
        std::cout << "         the copy against the dry" << top << std::endl;
        check(std::abs(copyLevel(1000.0, fs)) < 0.05 && std::abs(copyLevel(15000.0, fs)) < 0.05 && std::abs(copyLevel(15000.0, 192000.0)) < 0.05,
              "the copy is at unity at 1 kHz and at 15 kHz, at 48 and at 192 kHz");

        // Noise, as a whoosh is: how far each band falls.
        const auto in = noise(fs, 4.0, 0.25f, 9u);
        const Run r = runFresh(true, in, fs, 2, [](double, Step& s, SwirlFx&) { s.tape.motion = SwirlFx::ByHand; s.tape.depthMs = 0.0f; s.knob = 1.0f; });
        std::string bands;
        for (double hz : { 30.0, 300.0, 1000.0, 3000.0, 6000.0, 10000.0 })
        {
            auto bandRms = [hz, fs](const std::vector<float>& x)
            {
                sj::Svf f;
                f.set(static_cast<float>(hz), 3.0f, static_cast<float>(fs));
                double s = 0.0;
                for (size_t i = 0; i < x.size(); ++i)
                {
                    const double y = f.bandpass(x[i]);
                    if (i > x.size() / 4)
                        s += y * y;
                }
                return std::sqrt(s);
            };
            bands += "  " + num(hz, 0) + " Hz " + num(db(bandRms(r.out[0]) / bandRms(in)), 1) + " dB";
        }
        std::cout << "         white noise at -12 dBFS peak, by band:" << bands << std::endl;
    }

    std::cout << "Tape Flanger: On Its Own (the delay over time, from the phase of a 1 kHz sine)" << std::endl;
    {
        const auto in = sine(1000.0, fs, 7.0, 0.1f);
        auto sweep = [&](float rate, float depth, float wobble, int chunk)
        {
            return splitTape(in, fs, 0, 2, [=](double, Step& s, SwirlFx&)
            {
                s.tape.motion = SwirlFx::OnItsOwn;
                s.tape.rateHz = rate;
                s.tape.depthMs = depth;
                s.tape.wobble = wobble;
            }, chunk);
        };
        struct Shape { double lo, hi, lateShare, period, swingUp, swingDown, worst; };
        auto measure = [&](const Split& s, double from, double to)
        {
            const auto d = delayFromPhase(s.wet, s.dry, 1000.0, fs, from, to, s.run.offset[0][s.run.chunkAt(from)]);
            Shape m { 1.0e9, -1.0e9, 0.0, 0.0, 0.0, 0.0, 0.0 };
            const size_t a = static_cast<size_t>(from * fs), b = static_cast<size_t>(to * fs);
            // Whole cycles: from the first upward crossing of zero to the last.
            std::vector<size_t> ups;
            for (size_t i = a + 1; i < b; ++i)
                if (d[i - 1] < 0.0 && d[i] >= 0.0)
                    ups.push_back(i);
            if (ups.size() < 2)
                return m;
            size_t late = 0;
            for (size_t i = ups.front(); i < ups.back(); ++i)
            {
                m.lo = std::min(m.lo, d[i]);
                m.hi = std::max(m.hi, d[i]);
                late += d[i] > 0.0 ? 1u : 0u;
            }
            m.lateShare = static_cast<double>(late) / static_cast<double>(ups.back() - ups.front());
            m.period = static_cast<double>(ups.back() - ups.front()) / static_cast<double>(ups.size() - 1) / fs;
            // The pitch of the copy: 1 - the delay's rate of change, over 5 ms at a time.
            const size_t span = static_cast<size_t>(0.005 * fs);
            for (size_t i = ups.front(); i + span < ups.back(); i += span / 2)
            {
                const double ratio = 1.0 - slope(d, i, i + span) * fs * 0.001;
                m.swingUp = std::max(m.swingUp, ratio - 1.0);
                m.swingDown = std::min(m.swingDown, ratio - 1.0);
            }
            // Against what the class reports at each chunk's end.
            for (size_t c = s.run.chunkAt(from) + 2; c + 2 < s.run.chunkAt(to); ++c)
                m.worst = std::max(m.worst, std::abs(d[(c + 1) * static_cast<size_t>(s.run.chunk) - 1] - s.run.offset[0][c]));
            return m;
        };

        const auto slow = sweep(0.5f, 4.0f, 0.0f, 32);
        const Shape a = measure(slow, 0.3, 6.9);
        std::cout << "         0.5 Hz, Depth 4 ms: " << num(a.lo) << " .. " << num(a.hi) << " ms, period " << num(a.period) << " s, late for "
                  << num(100.0 * a.lateShare, 1) << "%, pitch " << num(cents(1.0 + a.swingDown), 1) << " .. +" << num(cents(1.0 + a.swingUp), 1)
                  << " cents; phase against the class's own figure: " << num(a.worst * 1000.0, 2) << " us at most" << std::endl;
        check(std::abs(a.hi - 4.0) < 0.05 && std::abs(a.lo + 4.0) < 0.05, "on its own: the copy goes Depth late and Depth early, through zero");
        check(std::abs(a.period - 2.0) < 0.005, "on its own: one sweep per 1 / Rate");
        check(std::abs(a.lateShare - 0.6) < 0.01, "on its own: the late side takes 60% of the cycle");
        check(a.worst < 0.005, "the phase of the sine and the class agree on the delay within 5 us");
        check(a.swingUp > -a.swingDown * 1.1, "on its own: the dive through zero is the faster one (the quick turn is on the early side)");

        // The cap: the swing is held as Rate rises.
        std::string caps;
        double worstSwing = 0.0;
        for (float rate : { 1.0f, 2.0f, 4.0f })
        {
            const auto s = sweep(rate, 6.0f, 0.0f, 32);
            const Shape m = measure(s, 0.3, 6.9);
            caps += "  " + num(rate, 0) + " Hz " + num(m.lo, 2) + " .. " + num(m.hi, 2) + " ms, " + num(100.0 * m.swingDown, 1) + "% .. +" + num(100.0 * m.swingUp, 1) + "%";
            worstSwing = std::max({ worstSwing, -m.swingDown, m.swingUp });
            if (rate == 4.0f)
                check(m.hi < 2.05 && m.hi > 1.5, "on its own: at 4 Hz a Depth of 6 ms is held to about 2 ms (" + num(m.hi, 2) + " ms)");
        }
        std::cout << "         Depth 6 ms:" << caps << std::endl;
        check(worstSwing < 0.065 && worstSwing > 0.03, "on its own: the pitch swing stays near 5% at any Rate (" + num(100.0 * worstSwing, 1) + "% at most)");

        // The same sweep in chunks of 7: the motion does not depend on how the engine cuts the block.
        const auto odd = sweep(0.5f, 4.0f, 0.25f, 7);
        const auto even = sweep(0.5f, 4.0f, 0.25f, 32);
        double apart = 0.0;
        for (double t = 0.5; t < 6.5; t += 0.01)
            apart = std::max(apart, std::abs(static_cast<double>(odd.run.offset[0][odd.run.chunkAt(t)]) - static_cast<double>(even.run.offset[0][even.run.chunkAt(t)])));
        // The two are read up to 32 samples apart in time, so the sweep's own speed is allowed for.
        check(apart < 0.02, "the motion is the same in chunks of 7 as in chunks of 32 (" + num(apart * 1000.0, 1) + " us apart at most)");

        // Wobble at a sweep rate on the resonance: the ring adds to the sweep but is held.
        const auto ring = sweep(0.6f, 3.0f, 1.0f, 32);
        const Shape w = measure(ring, 3.0, 6.9);
        const auto plain = sweep(0.6f, 3.0f, 0.0f, 32);
        const Shape p = measure(plain, 3.0, 6.9);
        std::cout << "         0.6 Hz (on the Wobble's resonance), Depth 3 ms: Wobble 0% " << num(p.lo, 2) << " .. " << num(p.hi, 2) << " ms, Wobble 100% "
                  << num(w.lo, 2) << " .. " << num(w.hi, 2) << " ms" << std::endl;
        check(w.hi - p.hi < 0.35 * 4.0 + 0.05 && w.hi > p.hi, "wobble: a sweep on the resonance grows by no more than 1.4 ms at full Wobble");
    }

    std::cout << "Tape Flanger: With the Pass" << std::endl;
    {
        // One pass of 3 s, closest at 1.5 s, twice over.
        const auto in = sine(1000.0, fs, 6.5, 0.1f);
        auto nearOf = [](double t)
        {
            const double s = std::sin(c_pi * std::fmod(t, 3.0) / 3.0);
            return static_cast<float>(s * s);
        };
        auto pass = [&](int lane, float wobble)
        {
            return splitTape(in, fs, lane, 3, [=](double t, Step& s, SwirlFx&)
            {
                s.tape.motion = SwirlFx::WithPass;
                s.tape.depthMs = 4.0f;
                s.tape.wobble = wobble;
                s.passNear = { nearOf(t), nearOf(t), nearOf(t) };
            });
        };
        auto crossings = [&](const std::vector<double>& d, double from, double to)
        {
            std::vector<double> at;
            for (size_t i = static_cast<size_t>(from * fs) + 1; i < static_cast<size_t>(to * fs); ++i)
                if ((d[i - 1] < 0.0) != (d[i] < 0.0))
                    at.push_back(static_cast<double>(i) / fs);
            return at;
        };
        const auto first = pass(0, 0.0f);
        const auto d0 = delayFromPhase(first.wet, first.dry, 1000.0, fs, 0.05, 6.4, first.run.offset[0][first.run.chunkAt(0.05)]);
        const auto x0 = crossings(d0, 3.1, 5.9);
        const double closest = d0[static_cast<size_t>((4.5 + 0.03) * fs)]; // the settle is 30 ms behind
        const double farMs = d0[static_cast<size_t>(3.03 * fs)];
        std::cout << "         Depth 4 ms: far " << num(farMs, 2) << " ms, at the closest point " << num(closest, 2) << " ms; through zero";
        for (double t : x0) std::cout << " " << num(t - 4.5, 3) << " s";
        std::cout << " from the closest point" << std::endl;
        check(x0.size() == 2 && x0[0] < 4.5 && x0[1] > 4.5, "with the pass: the copy goes through zero once before the closest point and once after");
        check(std::abs(farMs - 4.0) < 0.05, "with the pass: Depth late while the pass is far");
        check(std::abs(closest + 4.0 * (SwirlFx::c_passLead - 1.0)) < 0.05, "with the pass: (lead - 1) x Depth early at the closest point (" + num(closest, 2) + " ms)");
        // The law itself, 30 ms late: d = Depth (1 - lead x near).
        double worst = 0.0;
        for (double t = 3.2; t < 5.8; t += 0.01)
            worst = std::max(worst, std::abs(d0[static_cast<size_t>(t * fs)] - 4.0 * (1.0 - SwirlFx::c_passLead * static_cast<double>(nearOf(t - 0.03)))));
        check(worst < 0.02, "with the pass: d = Depth (1 - " + num(SwirlFx::c_passLead, 2) + " near), 30 ms behind (" + num(worst * 1000.0, 1) + " us off at most)");

        // The other lanes: the same zero, a different sweep either side.
        const auto second = pass(1, 0.0f);
        const auto d1 = delayFromPhase(second.wet, second.dry, 1000.0, fs, 0.05, 6.4, second.run.offset[1][second.run.chunkAt(0.05)]);
        const auto third = pass(2, 0.0f);
        const auto d2 = delayFromPhase(third.wet, third.dry, 1000.0, fs, 0.05, 6.4, third.run.offset[2][third.run.chunkAt(0.05)]);
        const auto x1 = crossings(d1, 3.1, 5.9), x2 = crossings(d2, 3.1, 5.9);
        const size_t farAt = static_cast<size_t>(3.03 * fs), closeAt = static_cast<size_t>(4.53 * fs);
        std::cout << "         second lane: far " << num(d1[farAt], 2) << " ms, closest " << num(d1[closeAt], 3) << " ms; third lane: far " << num(d2[farAt], 2)
                  << " ms, closest " << num(d2[closeAt], 3) << " ms" << std::endl;
        check(x1.size() == 2 && x2.size() == 2 && std::abs(x1[0] - x0[0]) < 0.001 && std::abs(x1[1] - x0[1]) < 0.001
                  && std::abs(x2[0] - x0[0]) < 0.001 && std::abs(x2[1] - x0[1]) < 0.001,
              "lanes: all three go through zero within 1 ms of each other");
        check(std::abs(d1[farAt] - 4.0 * (1.0 - 0.15 * 4.0 / 6.0)) < 0.05 && std::abs(d2[farAt] - 4.0 * (1.0 + 0.15 * 4.0 / 6.0)) < 0.05,
              "lanes: the second is 3.6 ms and the third 4.4 ms where the first is 4 ms (d (1 -+ 0.15 d / 6 ms))");

        // Wobble at its default rides on the pass without moving the zero off the closest point's two sides.
        const auto wob = pass(0, 0.25f);
        const auto dw = delayFromPhase(wob.wet, wob.dry, 1000.0, fs, 0.05, 6.4, wob.run.offset[0][wob.run.chunkAt(0.05)]);
        const auto xw = crossings(dw, 3.1, 5.9);
        double ride = 0.0;
        for (double t = 3.2; t < 5.8; t += 0.01)
            ride = std::max(ride, std::abs(dw[static_cast<size_t>(t * fs)] - d0[static_cast<size_t>(t * fs)]));
        std::cout << "         Wobble 25% moves the copy by " << num(ride, 2) << " ms at most on a 3 s pass" << std::endl;
        check(xw.size() == 2 && xw[0] < 4.5 && xw[1] > 4.5, "with the pass: the zero still brackets the closest point with Wobble at its default");

        // The first call starts where its settings say, with no glide from nowhere.
        SwirlFx fx;
        fx.prepare(fs);
        std::vector<float> buf(32, 0.0f);
        SwirlFx::Lanes ln;
        ln.lane[0] = buf.data();
        ln.count = 1;
        SwirlFx::Tape t;
        t.depthMs = 5.0f;
        fx.processTape(t, 1.0f, ln, 32);
        check(std::abs(fx.getLaneOffsetMs(0) - 5.0f) < 1.0e-4f, "the first call after reset() starts at its own settings (5 ms, not a glide from 0)");
    }

    std::cout << "Tape Flanger: Varispeed" << std::endl;
    {
        const auto in = sine(1000.0, fs, 9.0, 0.1f);
        // A lean of `v` from 2 s on, starting at rest at the far stop; the copy's pitch over a window where the speed
        // has arrived (it follows the wheel over 0.1 s, so a full lean only gets there in the last of the travel)
        // and the offset is clear of the last 10%.
        auto leanPitch = [&](float v, double from, double to, double* drift)
        {
            const auto s = splitTape(in, fs, 0, 2, [=](double t, Step& st, SwirlFx&)
            {
                st.tape.motion = SwirlFx::ByHand; st.tape.depthMs = 0.0f;
                st.tape.wobble = 0.0f;
                st.tape.varispeed = t < 1.0 ? (v > 0.0f ? -1.0f : 1.0f) : (t < 2.0 ? 0.0f : v); // to the far stop, let go, then lean
            });
            const auto d = delayFromPhase(s.wet, s.dry, 1000.0, fs, 0.05, 8.9, 0.0);
            const size_t a = static_cast<size_t>((2.0 + from) * fs), b = static_cast<size_t>((2.0 + to) * fs);
            const double perSecond = slope(d, a, b) * fs; // ms/s
            if (drift != nullptr)
                *drift = perSecond;
            // Inside the window the offset must be in the free part of its range.
            const bool free = std::abs(d[a]) < 5.4 && std::abs(d[b]) < 5.4;
            return free ? cents(1.0 - perSecond * 0.001) : 1.0e9;
        };
        struct Lean { float v; double from, to; };
        for (const Lean& l : { Lean { 0.5f, 0.7, 1.1 }, Lean { -0.5f, 0.7, 1.1 }, Lean { 0.75f, 0.45, 0.6 }, Lean { 1.0f, 0.455, 0.478 }, Lean { -1.0f, 0.455, 0.478 } })
        {
            double drift = 0.0;
            const double got = leanPitch(l.v, l.from, l.to, &drift);
            const double want = cents(1.0 + SwirlFx::leanSpeed(l.v));
            std::cout << "         Varispeed " << num(100.0 * l.v, 0) << "%: the offset drifts " << num(drift, 3) << " ms/s, the copy is " << num(got, 2)
                      << " cents off (the law: " << num(want, 2) << ")" << std::endl;
            check(std::abs(got - want) < 1.0, "varispeed " + num(100.0 * l.v, 0) + "%: the copy's pitch is the lean's speed within 1 cent (" + num(got - want, 2) + ")");
        }
        check(std::abs(cents(1.0 + SwirlFx::leanSpeed(1.0)) - 51.18) < 0.05 && SwirlFx::leanSpeed(0.05) == 0.0 && SwirlFx::leanSpeed(-0.04) == 0.0,
              "varispeed: 3% (51 cents) at the end of the wheel, nothing inside the 5% dead zone");

        // Dead zone, hold after release, the stops, and the slide home.
        struct Trace { std::vector<float> hand; std::vector<double> d; Run run; };
        auto trace = [&](const std::function<float(double)>& lean, double homeAt)
        {
            const auto s = splitTape(in, fs, 0, 2, [=](double t, Step& st, SwirlFx& fx)
            {
                st.tape.motion = SwirlFx::ByHand; st.tape.depthMs = 0.0f;
                st.tape.wobble = 0.0f;
                st.tape.varispeed = lean(t);
                if (t >= homeAt && t < homeAt + 0.02) // the engine holds the request for a few chunks
                    fx.recentre();
            });
            Trace tr;
            tr.d = delayFromPhase(s.wet, s.dry, 1000.0, fs, 0.05, 8.9, 0.0);
            tr.hand = s.run.hand;
            tr.run = s.run;
            return tr;
        };
        {
            const Trace t = trace([](double) { return 0.049f; }, 1.0e9);
            check(t.hand.back() == 0.0f, "varispeed: inside the dead zone the offset does not move");
        }
        {
            // Lean +60% for 0.3 s, let go, and watch for 6 s.
            const Trace t = trace([](double s) { return s > 1.0 && s < 1.3 ? 0.6f : 0.0f; }, 1.0e9);
            const float atRelease = t.hand[t.run.chunkAt(1.3)], settled = t.hand[t.run.chunkAt(3.0)], later = t.hand[t.run.chunkAt(8.8)];
            double lo = 1.0e9, hi = -1.0e9;
            for (size_t i = static_cast<size_t>(3.0 * fs); i < static_cast<size_t>(8.8 * fs); ++i)
            {
                lo = std::min(lo, t.d[i]);
                hi = std::max(hi, t.d[i]);
            }
            std::cout << "         +60% for 0.3 s: " << num(atRelease, 3) << " ms at release, coasting to " << num(settled, 3) << " ms; from 1.7 s to 7.5 s after it the copy stays within "
                      << num((hi - lo) * 1000.0, 2) << " us (by the sine's phase)" << std::endl;
            check(settled < -1.0f && settled == later, "varispeed: the offset holds after release (not a sample of drift in 5.8 s)");
            check(hi - lo < 0.002 && std::abs(0.5 * (hi + lo) - static_cast<double>(settled)) < 0.005, "varispeed: and the copy really stays there (within 2 us)");
            check(std::abs(settled - atRelease) < static_cast<float>(SwirlFx::leanSpeed(0.6) * 1000.0 * 0.1) + 0.02f && std::abs(settled - atRelease) > 0.5f,
                  "varispeed: it coasts by its speed x 0.1 s after release (" + num(std::abs(settled - atRelease), 2) + " ms from a 60% lean)");
        }
        {
            // Full lean each way: it eases into the stop, and comes home on request.
            const Trace t = trace([](double s) { return s < 2.0 ? 1.0f : 0.0f; }, 3.0);
            const float stop = t.hand[t.run.chunkAt(1.99)];
            bool monotonic = true, within = true;
            for (size_t c = 1; c < t.run.chunkAt(2.9); ++c)
            {
                monotonic = monotonic && t.hand[c] <= t.hand[c - 1] + 1.0e-6f;
                within = within && std::abs(t.hand[c]) <= 6.0f;
            }
            const double nearStop = t.d[static_cast<size_t>(2.9 * fs)];
            std::cout << "         +100% held: the offset stops at " << num(stop, 4) << " ms; the copy sits at " << num(nearStop, 3) << " ms (the end stop's ease)" << std::endl;
            check(monotonic && within && stop < -5.98f, "varispeed: a held lean eases into the stop at -6 ms, with no overshoot");
            // The speed eases off over the last 10%: the pitch falls, it does not hit a wall.
            std::string fall;
            double previous = 1.0e9;
            bool easing = true;
            for (double at = 0.36; at < 0.50; at += 0.02)
            {
                const double c = cents(1.0 - slope(t.d, static_cast<size_t>(at * fs), static_cast<size_t>((at + 0.02) * fs)) * fs * 0.001);
                fall += " " + num(c, 1);
                easing = easing && c <= previous + 0.5 && c > -0.5;
                previous = c;
            }
            std::cout << "         the copy's pitch through the last of the travel, 20 ms at a time:" << fall << " cents" << std::endl;
            check(easing, "varispeed: the pitch eases down to nothing at the stop");

            const size_t homeFrom = t.run.chunkAt(3.0);
            size_t homeAt = homeFrom;
            bool inward = true;
            double fastest = 0.0;
            for (size_t c = homeFrom + 1; c < t.hand.size(); ++c)
            {
                inward = inward && t.hand[c] >= t.hand[c - 1] - 1.0e-6f && t.hand[c] <= 0.0f;
                fastest = std::max(fastest, static_cast<double>(t.hand[c] - t.hand[c - 1]) / (32.0 / fs));
                if (homeAt == homeFrom && t.hand[c] == 0.0f)
                    homeAt = c;
            }
            const double took = static_cast<double>(homeAt - homeFrom) * 32.0 / fs;
            std::cout << "         recentre() from the stop: home in " << num(took, 3) << " s, " << num(fastest, 1) << " ms/s at most ("
                      << num(cents(1.0 + fastest * 0.001), 1) << " cents)" << std::endl;
            check(inward && homeAt > homeFrom && t.hand.back() == 0.0f, "varispeed: the offset slides home on request, one way, and ends at exactly 0");
            check(took > 0.2 && took < 1.0 && fastest <= 30.0 + 0.01, "varispeed: the slide takes under a second and is no faster than a full lean");
            const double homeD = t.d[static_cast<size_t>(8.5 * fs)];
            check(std::abs(homeD) < 0.002, "varispeed: the copy is back on the dry (" + num(homeD * 1000.0, 2) + " us)");
        }
        {
            // Varispeed adds to the other motions, and reset() puts it back in the middle.
            SwirlFx fx;
            fx.prepare(fs);
            std::vector<float> buf(32, 0.0f);
            SwirlFx::Lanes ln;
            ln.lane[0] = buf.data();
            ln.count = 1;
            SwirlFx::Tape t;
            t.depthMs = 2.0f;
            t.wobble = 0.0f;
            t.varispeed = -1.0f;
            for (int i = 0; i < 150; ++i) // 0.1 s
                fx.processTape(t, 1.0f, ln, 32);
            t.varispeed = 0.0f;
            for (int i = 0; i < 3000; ++i)
                fx.processTape(t, 1.0f, ln, 32);
            const float hand = fx.getOffsetMs();
            check(hand > 1.0f && std::abs(fx.getLaneOffsetMs(0) - (2.0f + hand)) < 0.01f, "varispeed: the offset adds to With the Pass (2 ms + " + num(hand, 2) + " ms)");
            // Straight to the Doubler and back: the offset is still there.
            SwirlFx::Doubler d;
            fx.processDoubler(d, 1.0f, ln, 32);
            fx.processTape(t, 1.0f, ln, 32);
            check(fx.getOffsetMs() == hand, "varispeed: a change of type keeps the held offset");
            fx.recentre();
            fx.reset();
            fx.processTape(t, 1.0f, ln, 32);
            check(fx.getOffsetMs() == 0.0f && std::abs(fx.getLaneOffsetMs(0) - 2.0f) < 1.0e-4f, "varispeed: reset() puts the offset back in the middle");
        }
    }

    {
        // A steady slow lean under 120 kHz: the copy is read with the sinc, so a tone at the top of the band stays
        // the same level as the read point moves between samples (the four-point read lets it rise and fall by a
        // decibel or more, which is heard as sidebands). The copy's level over every ten cycles of the tone.
        std::string rows;
        double worst = 0.0;
        for (double rate : { 44100.0, 48000.0, 96000.0 })
            for (double hz : { 5000.0, 10000.0, 15000.0 })
            {
                const auto tone = sine(hz, rate, 1.2, 0.25f);
                const auto s = splitTape(tone, rate, 0, 1, [](double, Step& st, SwirlFx&)
                {
                    st.tape.motion = SwirlFx::ByHand; st.tape.depthMs = 0.0f;
                    st.tape.wobble = 0.0f;
                    st.tape.varispeed = -0.3f;
                });
                const size_t window = static_cast<size_t>(std::lround(40.0 * rate / hz));
                double low = 1.0e30, high = 0.0;
                for (size_t a = static_cast<size_t>(0.6 * rate); a + window < static_cast<size_t>(1.1 * rate); a += window / 4)
                {
                    double sq = 0.0;
                    for (size_t i = a; i < a + window; ++i)
                        sq += s.wet[i] * s.wet[i];
                    low = std::min(low, sq);
                    high = std::max(high, sq);
                }
                const double ripple = 10.0 * std::log10(high / low);
                worst = std::max(worst, ripple);
                rows += "  " + num(rate / 1000.0, 1) + " kHz, " + num(hz / 1000.0, 0) + " kHz tone " + num(ripple, 3) + " dB";
            }
        std::cout << "         a slow lean: the copy's level moves by" << rows << std::endl;
        check(worst < 0.25, "under 120 kHz the moving copy keeps its level at the top of the band (" + num(worst, 3) + " dB at the most)");
    }

    std::cout << "Tape Flanger: Wobble and Feedback" << std::endl;
    {
        // Wobble: a step of the copy (the pass jumps) arrives as fast at any setting, then rings.
        const auto in = sine(1000.0, fs, 9.0, 0.1f);
        std::string rows;
        double ringAtFull = 0.0, quick = 0.0;
        for (float wobble : { 0.0f, 0.25f, 1.0f })
        {
            const auto s = splitTape(in, fs, 0, 1, [=](double t, Step& st, SwirlFx&)
            {
                st.tape.motion = SwirlFx::WithPass;
                st.tape.depthMs = 3.0f;
                st.tape.wobble = wobble;
                st.passNear[0] = t < 1.0 ? 0.0f : 1.0f / static_cast<float>(SwirlFx::c_passLead); // 3 ms, then 0
            });
            const auto d = delayFromPhase(s.wet, s.dry, 1000.0, fs, 0.05, 8.9, 3.0);
            double over = 0.0;
            for (size_t i = static_cast<size_t>(1.15 * fs); i < static_cast<size_t>(8.0 * fs); ++i)
                over = std::max(over, std::abs(d[i]));
            const double at50 = 1.0 - d[static_cast<size_t>(1.05 * fs)] / 3.0;
            rows += "  Wobble " + num(100.0 * wobble, 0) + "%: " + num(100.0 * at50, 0) + "% there in 50 ms, rings by " + num(100.0 * over / 3.0, 1) + "% of the move";
            if (wobble == 1.0f) ringAtFull = over / 3.0;
            if (wobble == 0.0f) quick = at50;
        }
        std::cout << "         a 3 ms move:" << rows << std::endl;
        check(quick > 0.75 && quick < 0.9, "settle: about 80% of a move is made in 50 ms (two 15 ms one-poles)");
        check(ringAtFull > 0.2 && ringAtFull < 0.36, "wobble: at 100% a move rings by between a fifth and a third of its size");
    }
    {
        // A click with Feedback at either end: the repeats are 4 ms apart (the copy is 4 ms from the dry), each
        // 0.9 of the last, and they die away.
        auto click = [&](float feedback, float nearNow, double* ratio, double* spacingMs)
        {
            std::vector<float> in(static_cast<size_t>(3.0 * fs), 0.0f);
            const size_t at = static_cast<size_t>(0.1 * fs);
            in[at] = 0.05f;
            const Run r = runFresh(true, in, fs, 1, [=](double, Step& s, SwirlFx&)
            {
                s.tape.motion = SwirlFx::WithPass;
                s.tape.depthMs = 4.0f;
                s.tape.wobble = 0.0f;
                s.tape.feedback = feedback;
                s.tape.invert = false;
                s.passNear[0] = nearNow;
            });
            const auto o = asDouble(r.out[0]);
            const double offset = 4.0 * (1.0 - SwirlFx::c_passLead * static_cast<double>(nearNow));
            const double gap = std::max(0.25, std::abs(offset)) * 0.001 * fs;
            const double first = static_cast<double>(at) + 384.0 + offset * 0.001 * fs;
            double c0 = 0.0, c1 = 0.0;
            const int reach = static_cast<int>(std::min(40.0, gap * 0.45));
            const double s0 = sumAround(o, first, reach, &c0), s1 = sumAround(o, first + gap, reach, &c1), s2 = sumAround(o, first + 2.0 * gap, reach);
            if (ratio != nullptr) *ratio = 0.5 * (s1 / s0 + s2 / s1);
            if (spacingMs != nullptr) *spacingMs = (c1 - c0) / fs * 1000.0;
            double peak = 0.0, tail = 0.0;
            for (size_t i = 0; i < o.size(); ++i)
            {
                peak = std::max(peak, std::abs(o[i]));
                if (i > o.size() - static_cast<size_t>(0.5 * fs))
                    tail = std::max(tail, std::abs(o[i]));
            }
            return db(tail / peak);
        };
        for (float feedback : { 1.0f, -1.0f })
        {
            double ratio = 0.0, spacing = 0.0;
            const double tail = click(feedback, 0.0f, &ratio, &spacing);
            std::cout << "         Feedback " << num(100.0 * feedback, 0) << "%, the copy 4 ms late: repeats " << num(spacing, 3) << " ms apart, each " << num(ratio, 3)
                      << " of the last; 2.4 s on the tail is " << num(tail, 0) << " dB under the click" << std::endl;
            check(std::abs(ratio - 0.9 * static_cast<double>(feedback)) < 0.01 && std::abs(spacing - 4.0) < 0.01, "feedback " + num(100.0 * feedback, 0) + "%: a recirculating comb as long as the copy's offset, loop gain 0.9");
            check(tail < -120.0, "feedback " + num(100.0 * feedback, 0) + "%: a click decays (below -120 dB in 2.4 s)");
            const double early = click(feedback, 1.0f, nullptr, nullptr); // 1.2 ms early: the comb is |d| long
            const double zero = click(feedback, static_cast<float>(1.0 / SwirlFx::c_passLead), nullptr, nullptr); // at zero: the 0.25 ms floor
            check(early < -120.0 && zero < -120.0, "feedback " + num(100.0 * feedback, 0) + "%: and with the copy early or at zero (the comb never shorter than 0.25 ms)");
        }
        // The worst case: full-scale noise, every motion at once, Feedback at both ends. It stays bounded, and
        // dies away when the sound stops.
        for (float feedback : { 1.0f, -1.0f })
        {
            auto in = noise(fs, 12.0, 1.0f, 21u);
            for (size_t i = static_cast<size_t>(8.0 * fs); i < in.size(); ++i)
                in[i] = 0.0f;
            const Run r = runFresh(true, in, fs, 3, [=](double t, Step& s, SwirlFx&)
            {
                s.tape.motion = SwirlFx::OnItsOwn;
                s.tape.rateHz = 4.0f;
                s.tape.depthMs = 6.0f;
                s.tape.wobble = 1.0f;
                s.tape.feedback = feedback;
                s.tape.varispeed = static_cast<float>(std::sin(2.0 * c_pi * 0.7 * t));
            });
            double peak = 0.0, tail = 0.0;
            for (int l = 0; l < 3; ++l)
                for (size_t i = 0; i < in.size(); ++i)
                {
                    const double v = std::abs(static_cast<double>(r.out[static_cast<size_t>(l)][i]));
                    peak = std::max(peak, v);
                    if (i > static_cast<size_t>(11.0 * fs))
                        tail = std::max(tail, v);
                }
            std::cout << "         Feedback " << num(100.0 * feedback, 0) << "%, full-scale noise, a 4 Hz sweep and a moving lean: peak " << num(peak, 2)
                      << ", 3 s after the sound stops " << num(db(tail), 0) << " dBFS" << std::endl;
            check(std::isfinite(peak) && peak < 3.0 && tail < 1.0e-6, "feedback " + num(100.0 * feedback, 0) + "%: it never runs away");
        }
    }

    std::cout << "Doubler: the delay" << std::endl;
    {
        // No wander: a click comes back Delay later (the low-pass adds 4 us).
        auto clickDelay = [&](float delayMs, bool spread, int lane)
        {
            std::vector<float> in(static_cast<size_t>(0.4 * fs), 0.0f);
            const size_t at = static_cast<size_t>(0.05 * fs);
            in[at] = 0.05f;
            const Run r = runFresh(false, in, fs, 3, [=](double, Step& s, SwirlFx&)
            {
                s.doubler.delayMs = delayMs;
                s.doubler.wander = 0.0f;
                s.doubler.thicken = false;
                s.doubler.spread = spread;
            });
            const auto wet = doublerWet(r, in, lane);
            double c = 0.0;
            const double wide = spread ? (lane == 1 ? 1.3 : (lane == 2 ? 1.15 : 1.0)) : 1.0;
            sumAround(wet, static_cast<double>(at) + static_cast<double>(delayMs) * wide * 0.001 * fs, 30, &c);
            return (c - static_cast<double>(at)) / fs * 1000.0;
        };
        std::string row;
        bool ok = true;
        for (float ms : { 3.0f, 6.0f, 20.0f, 45.0f, 75.0f })
        {
            const double got = clickDelay(ms, false, 0);
            row += "  " + num(ms, 0) + " -> " + num(got, 3);
            ok = ok && std::abs(got - static_cast<double>(ms)) < 0.02;
        }
        std::cout << "         Wander 0%, Delay in ms -> where the click lands:" << row << std::endl;
        check(ok, "doubler: with no wander the copy is Delay late (3 to 75 ms, within 20 us)");
        const double second = clickDelay(20.0f, true, 1), third = clickDelay(20.0f, true, 2), same = clickDelay(20.0f, false, 1);
        check(std::abs(second - 26.0) < 0.02 && std::abs(third - 23.0) < 0.02 && std::abs(same - 20.0) < 0.02,
              "doubler: Spread puts the second lane at 1.3 x Delay and the third at 1.15 x (" + num(second, 2) + ", " + num(third, 2) + " ms); off, every lane is the first");

        // The wander: d = (1 - Wander) Delay + Wander (3 ms + P u), from the phase of a 1 kHz sine.
        const auto in = sine(1000.0, fs, 8.0, 0.1f);
        auto wanderOf = [&](int wave, float rate, float wander, int lane, Run* keep)
        {
            const Run r = runFresh(false, in, fs, 3, [=](double, Step& s, SwirlFx&)
            {
                s.doubler.wave = wave;
                s.doubler.rateHz = rate;
                s.doubler.wander = wander;
                s.doubler.thicken = false;
            });
            if (keep != nullptr)
                *keep = r;
            return delayFromPhase(doublerWet(r, in, lane), asDouble(in), 1000.0, fs, 0.2, 7.9, r.offset[static_cast<size_t>(lane)][r.chunkAt(0.2)]);
        };
        auto range = [&](const std::vector<double>& d, double from, double to, double* lo, double* hi)
        {
            *lo = 1.0e9; *hi = -1.0e9;
            for (size_t i = static_cast<size_t>(from * fs); i < static_cast<size_t>(to * fs); ++i)
            {
                *lo = std::min(*lo, d[i]);
                *hi = std::max(*hi, d[i]);
            }
        };
        auto pitchSwing = [&](const std::vector<double>& d, double from, double to, double* median)
        {
            std::vector<double> c;
            const size_t span = static_cast<size_t>(0.01 * fs);
            for (size_t i = static_cast<size_t>(from * fs); i + span < static_cast<size_t>(to * fs); i += span)
                c.push_back(std::abs(cents(1.0 - slope(d, i, i + span) * fs * 0.001)));
            std::sort(c.begin(), c.end());
            if (median != nullptr)
                *median = c[c.size() / 2];
            return c.back();
        };
        Run sineRun;
        const auto s0 = wanderOf(SwirlFx::WaveSine, 1.0f, 0.15f, 0, &sineRun);
        const auto s1 = wanderOf(SwirlFx::WaveSine, 1.0f, 0.15f, 1, nullptr);
        double lo = 0.0, hi = 0.0, lo1 = 0.0, hi1 = 0.0;
        range(s0, 1.0, 7.0, &lo, &hi);
        range(s1, 1.0, 7.0, &lo1, &hi1);
        const double sinePeak = pitchSwing(s0, 1.0, 7.0, nullptr);
        std::cout << "         Sine, 1 Hz, Wander 15%, Delay 20 ms: " << num(lo, 2) << " .. " << num(hi, 2) << " ms, pitch +-" << num(sinePeak, 1)
                  << " cents; second lane " << num(lo1, 2) << " .. " << num(hi1, 2) << " ms" << std::endl;
        check(std::abs(lo - 17.45) < 0.05 && std::abs(hi - 24.95) < 0.05, "doubler: the wander runs from 0.85 x 20 + 0.15 x 3 ms up by 0.15 x 50 ms (17.45 to 24.95 ms)");
        check(std::abs(lo1 - 22.55) < 0.05 && std::abs(hi1 - 30.05) < 0.05, "doubler: the second lane's from 22.55 to 30.05 ms");
        double worst = 0.0;
        for (size_t c = sineRun.chunkAt(0.3); c < sineRun.chunkAt(7.8); ++c)
            worst = std::max(worst, std::abs(s0[(c + 1) * 32 - 1] - static_cast<double>(sineRun.offset[0][c])));
        check(worst < 0.01, "the phase of the sine and the class agree on the Doubler's delay within 10 us (" + num(worst * 1000.0, 1) + ")");
        {
            // The second lane moves the other way.
            std::vector<float> a, b;
            for (size_t i = static_cast<size_t>(1.0 * fs); i < static_cast<size_t>(7.0 * fs); i += 48)
            {
                a.push_back(static_cast<float>(s0[i]));
                b.push_back(static_cast<float>(s1[i]));
            }
            const double c = correlation(a, b, 0);
            check(c < -0.999, "doubler: with Spread the second lane's wander is the first's upside down (correlation " + num(c, 3) + ")");
        }
        const auto tri = wanderOf(SwirlFx::WaveTriangle, 1.0f, 0.15f, 0, nullptr);
        double triMedian = 0.0;
        range(tri, 1.0, 7.0, &lo, &hi);
        pitchSwing(tri, 1.0, 7.0, &triMedian);
        std::cout << "         Triangle, the same: " << num(lo, 2) << " .. " << num(hi, 2) << " ms, a steady " << num(triMedian, 1) << " cents either way" << std::endl;
        check(std::abs(triMedian - std::abs(cents(1.0 - 0.015))) < 1.0, "doubler: a Triangle is a steady detune that flips (15 ms/s: 26 cents)");
        check(lo > 17.45 - 0.01 && hi < 24.95 + 0.01 && hi - lo > 7.3, "doubler: inside the same range, its corners just rounded");

        // The range falls with Rate above 0.71 Hz: P = min(70 ms, 50 ms Hz / Rate).
        check(SwirlFx::wanderRangeMs(0.3) == 70.0 && std::abs(SwirlFx::wanderRangeMs(1.0) - 50.0) < 1.0e-9 && std::abs(SwirlFx::wanderRangeMs(4.0) - 12.5) < 1.0e-9,
              "doubler: the widest wander is 70 ms up to 0.71 Hz, then 50 ms Hz / Rate");
        std::string rates;
        for (float rate : { 0.3f, 1.0f, 2.0f, 4.0f, 6.0f })
        {
            const auto d = wanderOf(SwirlFx::WaveSine, rate, 1.0f, 0, nullptr);
            range(d, 3.5, 7.5, &lo, &hi);
            const double swing = pitchSwing(d, 3.5, 7.5, nullptr);
            rates += "  " + num(rate, 1) + " Hz " + num(hi - lo, 1) + " ms (+-" + num(swing, 0) + " cents)";
            if (rate == 0.3f)
                check(std::abs(lo - 3.0) < 0.1 && std::abs(hi - 73.0) < 0.1, "doubler: at Wander 100% and 0.3 Hz the copy runs from 3 to 73 ms");
            if (rate == 4.0f)
                check(std::abs((hi - lo) - 12.5) < 0.6, "doubler: at 4 Hz the range is 12.5 ms (" + num(hi - lo, 2) + " after the 5 ms smoothing)");
        }
        std::cout << "         Sine at Wander 100%, range by Rate:" << rates << std::endl;
    }
    {
        // Random: a smooth wander inside the same range, the lanes each on their own.
        const double seconds = 240.0;
        const std::vector<float> silent(static_cast<size_t>(seconds * fs), 0.0f);
        auto wander = [&](float rate, bool spread, int chunk, std::uint32_t seed)
        {
            return runFresh(false, silent, fs, 3, [=](double, Step& s, SwirlFx&)
            {
                s.doubler.wave = SwirlFx::WaveRandom;
                s.doubler.rateHz = rate;
                s.doubler.wander = 0.15f;
                s.doubler.spread = spread;
            }, chunk, seed);
        };
        auto stats = [&](const Run& r, int lane, double base, double* sd, double* lo, double* hi, double* pitchRms)
        {
            const auto& d = r.offset[static_cast<size_t>(lane)];
            double sum = 0.0, sq = 0.0, speed = 0.0;
            *lo = 1.0e9; *hi = -1.0e9;
            const double dt = static_cast<double>(r.chunk) / fs;
            for (size_t i = 1; i < d.size(); ++i)
            {
                const double v = static_cast<double>(d[i]) - base;
                sum += v; sq += v * v;
                *lo = std::min(*lo, static_cast<double>(d[i]));
                *hi = std::max(*hi, static_cast<double>(d[i]));
                const double s = (static_cast<double>(d[i]) - static_cast<double>(d[i - 1])) * 0.001 / dt;
                speed += s * s;
            }
            const double n = static_cast<double>(d.size() - 1);
            *sd = std::sqrt(sq / n - (sum / n) * (sum / n));
            *pitchRms = std::sqrt(speed / n);
            return sum / n + base;
        };
        const Run r = wander(1.0f, true, 32, 77u);
        double sd = 0.0, lo = 0.0, hi = 0.0, pitch = 0.0;
        const double mean = stats(r, 0, 21.2, &sd, &lo, &hi, &pitch);
        std::cout << "         Random, 1 Hz, Wander 15%, Delay 20 ms, 240 s: mean " << num(mean, 2) << " ms, spread (one sigma) " << num(sd, 2) << " ms, " << num(lo, 2) << " .. "
                  << num(hi, 2) << " ms, pitch wander " << num(100.0 * pitch, 2) << "% rms (" << num(cents(1.0 + pitch), 1) << " cents)" << std::endl;
        check(lo >= 17.45 && hi <= 24.95, "doubler: Random stays inside the range the other waves use (17.45 to 24.95 ms)");
        check(sd > 1.5 && sd < 2.25, "doubler: Random's spread is about a quarter of the range (" + num(sd, 2) + " ms of 7.5)");
        check(std::abs(mean - 21.2) < 0.6, "doubler: and it is centred in it");
        const double c01 = correlation(r.offset[0], r.offset[1], 100), c02 = correlation(r.offset[0], r.offset[2], 100);
        check(std::abs(c01) < 0.4 && std::abs(c02) < 0.4, "doubler: with Spread each lane has its own Random (correlations " + num(c01, 2) + ", " + num(c02, 2) + ")");
        const Run together = wander(1.0f, false, 32, 77u);
        check(together.offset[0] == together.offset[1] && together.offset[0] == together.offset[2], "doubler: without Spread every lane is the same copy");
        const Run again = wander(1.0f, true, 32, 77u), other = wander(1.0f, true, 32, 78u);
        check(again.offset[0] == r.offset[0] && other.offset[0] != r.offset[0], "doubler: the same seed gives the same wander, another seed another");
        // The length of the engine's chunks does not change its size or its speed.
        const Run fine = wander(1.0f, true, 8, 77u);
        double sdFine = 0.0, pitchFine = 0.0;
        stats(fine, 0, 21.2, &sdFine, &lo, &hi, &pitchFine);
        check(std::abs(sdFine / sd - 1.0) < 0.2, "doubler: Random is as wide in chunks of 8 as in chunks of 32 (" + num(sdFine, 2) + " ms against " + num(sd, 2) + ")");
        // At its defaults (0.6 Hz, 12%).
        const Run def = runFresh(false, silent, fs, 3, [](double, Step&, SwirlFx&) {});
        double sdDef = 0.0, pitchDef = 0.0;
        const double meanDef = stats(def, 0, 20.0, &sdDef, &lo, &hi, &pitchDef);
        std::cout << "         at its defaults (0.6 Hz, 12%, 20 ms): mean " << num(meanDef, 2) << " ms, " << num(lo, 2) << " .. " << num(hi, 2) << " ms, sigma " << num(sdDef, 2)
                  << " ms, pitch wander " << num(cents(1.0 + pitchDef), 1) << " cents rms" << std::endl;
    }

    std::cout << "Doubler: Thicken and Feedback" << std::endl;
    {
        // The loop gain, from a click: each repeat's sum against the one before.
        auto loopGain = [&](float delayMs, bool thicken, float feedback)
        {
            std::vector<float> in(static_cast<size_t>(0.6 * fs), 0.0f);
            const size_t at = static_cast<size_t>(0.05 * fs);
            in[at] = 0.05f;
            const Run r = runFresh(false, in, fs, 1, [=](double, Step& s, SwirlFx&)
            {
                s.doubler.delayMs = delayMs;
                s.doubler.wander = 0.0f;
                s.doubler.thicken = thicken;
                s.doubler.feedback = feedback;
                s.doubler.spread = false;
            });
            const auto wet = doublerWet(r, in, 0);
            const double gap = static_cast<double>(delayMs) * 0.001 * fs;
            const double s0 = sumAround(wet, static_cast<double>(at) + gap, 40), s1 = sumAround(wet, static_cast<double>(at) + 2.0 * gap, 40);
            return s1 / s0;
        };
        std::string row;
        bool ok = true;
        for (float ms : { 3.0f, 6.0f, 8.5f, 14.0f, 20.0f, 26.0f, 34.0f, 46.0f, 60.0f })
        {
            const double got = loopGain(ms, true, 0.9f), want = SwirlFx::thickenGain(ms);
            row += "  " + num(ms, 1) + " ms " + num(got, 3);
            ok = ok && std::abs(got - want) < 0.005;
        }
        std::cout << "         Thicken's loop gain by Delay:" << row << std::endl;
        check(ok, "thicken: the loop gain is -0.65 (1 - d / 45 ms)^1.2, nothing above 45 ms, whatever Feedback says");
        check(std::abs(SwirlFx::thickenGain(6.0) + 0.5475) < 0.001 && SwirlFx::thickenGain(45.0) == 0.0 && SwirlFx::thickenGain(0.0) == -0.65,
              "thicken: -0.547 at 6 ms, 0 from 45 ms");
        const double up = loopGain(20.0f, false, 1.0f), down = loopGain(20.0f, false, -1.0f), half = loopGain(20.0f, false, 0.5f), none = loopGain(20.0f, false, 0.0f);
        check(std::abs(up - 0.68) < 0.005 && std::abs(down + 0.68) < 0.005 && std::abs(half - 0.34) < 0.005 && std::abs(none) < 1.0e-6,
              "feedback: with Thicken off the loop gain is 0.68 x Feedback (" + num(up, 3) + ", " + num(down, 3) + ", " + num(half, 3) + ")");

        // Thicken follows the moving delay (Sine, slow and wide): the gain at the long end is the law's there.
        // And a click at full Feedback decays.
        std::vector<float> in(static_cast<size_t>(4.0 * fs), 0.0f);
        in[4800] = 0.5f;
        for (float feedback : { 1.0f, -1.0f })
        {
            const Run r = runFresh(false, in, fs, 3, [=](double, Step& s, SwirlFx&)
            {
                s.doubler.thicken = false;
                s.doubler.feedback = feedback;
                s.doubler.delayMs = 3.0f;
                s.doubler.wander = 0.0f;
            });
            double tail = 0.0;
            for (size_t i = static_cast<size_t>(3.0 * fs); i < in.size(); ++i)
                tail = std::max(tail, std::abs(static_cast<double>(r.out[0][i])));
            check(tail < 1.0e-7, "feedback " + num(100.0 * feedback, 0) + "%: a click through the Doubler decays (" + num(db(tail / 0.5), 0) + " dB after 2.9 s)");
        }
    }

    std::cout << "Doubler: the mono sum" << std::endl;
    {
        const auto in = noise(fs, 20.0, 0.25f, 33u);
        auto sumLoss = [&](bool tape, const Control& control, double* corr, double* level)
        {
            const Run r = runFresh(tape, in, fs, 2, control);
            std::vector<float> mono(in.size());
            for (size_t i = 0; i < in.size(); ++i)
                mono[i] = 0.5f * (r.out[0][i] + r.out[1][i]);
            const size_t a = static_cast<size_t>(1.0 * fs), b = in.size() - 64;
            const double l = rms(r.out[0], a, b), rr = rms(r.out[1], a, b);
            if (corr != nullptr) *corr = correlation(r.out[0], r.out[1], a);
            if (level != nullptr) *level = db(0.5 * (l + rr) / rms(in, a, b));
            return db(rms(mono, a, b) / (0.5 * (l + rr)));
        };
        double corr = 0.0, level = 0.0;
        const double def = sumLoss(false, [](double, Step&, SwirlFx&) {}, &corr, &level);
        std::cout << "         at its defaults, wheel at the top, white noise: left and right " << num(level, 2) << " dB against the input, correlation " << num(corr, 2)
                  << ", the mono sum " << num(def, 2) << " dB under either" << std::endl;
        check(def > -2.0 && def < 0.0, "doubler: the mono sum loses under 2 dB (" + num(def, 2) + " dB)");
        const double plain = sumLoss(false, [](double, Step& s, SwirlFx&) { s.doubler.thicken = false; s.doubler.wave = SwirlFx::WaveSine; }, &corr, nullptr);
        const double shortest = sumLoss(false, [](double, Step& s, SwirlFx&) { s.doubler.delayMs = 3.0f; }, nullptr, nullptr);
        const double longest = sumLoss(false, [](double, Step& s, SwirlFx&) { s.doubler.delayMs = 75.0f; s.doubler.wander = 1.0f; }, nullptr, nullptr);
        std::cout << "         Thicken off with a Sine " << num(plain, 2) << " dB, Delay 3 ms " << num(shortest, 2) << " dB, Delay 75 ms at Wander 100% " << num(longest, 2) << " dB" << std::endl;
        check(plain > -2.0 && shortest > -2.0 && longest > -2.0, "doubler: and under 2 dB at the short end, the long end and with Thicken off");
        const double narrow = sumLoss(false, [](double, Step& s, SwirlFx&) { s.doubler.spread = false; }, &corr, nullptr);
        check(std::abs(narrow) < 0.01 && corr > 0.9999, "doubler: without Spread left and right are the same (nothing to lose)");
        const double tape = sumLoss(true, [](double t, Step& s, SwirlFx&) { const double v = std::sin(c_pi * std::fmod(t, 3.0) / 3.0); s.passNear = { static_cast<float>(v * v), static_cast<float>(v * v), 0.0f }; }, &corr, &level);
        std::cout << "         (Tape Flanger at its defaults over 3 s passes: " << num(level, 2) << " dB against the input, correlation " << num(corr, 2) << ", mono sum " << num(tape, 2) << " dB)" << std::endl;
    }

    std::cout << "Tape Flanger: With the Pass as the engine drives it, and With the Filter" << std::endl;
    {
        // passNearFor: what the engine hands over for a nearness and the time to the pass's closest point.
        const double lead = SwirlFx::c_passLead, hold = SwirlFx::c_passHold, window = SwirlFx::c_passWindowSeconds;
        double worstFar = 0.0;
        for (const double near : { 0.0, 0.2, 0.5, 0.77, 1.0 })
            for (const double s : { -3.0, -window, window, 0.5 })
                worstFar = std::max(worstFar, std::abs(SwirlFx::passNearFor(near, s) - (1.0 - hold) * near / lead));
        check(worstFar < 1.0e-12, "passNearFor: outside its window it is (1 - hold) of the nearness over the lead, so the copy is never nearer than hold x Depth out there");
        check(std::abs(SwirlFx::passNearFor(1.0, 0.0) - 1.0) < 1.0e-12 && std::abs(SwirlFx::passNearFor(0.0, 0.0) - 1.0) < 1.0e-12,
              "passNearFor: 1 at the closest point (the copy (lead - 1) x Depth early), however near the pass is said to be");
        // Where it crosses 1 / lead (the copy level with the dry): for a pass that is near all the time, for one that
        // is near for an instant only, and for one in between. Always inside the window, and the same either side.
        auto zeroAt = [lead](double sharpness)
        {
            // near = 1 / (1 + (s / sharpness)^2): 1 at the closest point, half at `sharpness` seconds.
            for (double s = 0.0; s < 1.0; s += 1.0e-5)
                if (SwirlFx::passNearFor(1.0 / (1.0 + (s / sharpness) * (s / sharpness)), s) < 1.0 / lead)
                    return s;
            return 1.0;
        };
        const double wide = zeroAt(1000.0), middling = zeroAt(0.1), sharp = zeroAt(0.001);
        std::cout << "         the copy is level with the dry " << num(1000.0 * wide, 1) << " ms either side of the closest point for a pass that stays near, " << num(1000.0 * middling, 1)
                  << " ms for one half as near 100 ms away, " << num(1000.0 * sharp, 1) << " ms for one near for a millisecond (the window is " << num(1000.0 * window, 0) << " ms)" << std::endl;
        check(wide < window && wide > sharp && sharp > 0.2 * window && middling <= wide && middling >= sharp,
              "passNearFor: the zero is inside the window whatever the pass, between " + num(1000.0 * sharp, 1) + " and " + num(1000.0 * wide, 1) + " ms from the closest point");
        bool even = true;
        for (double s = 0.0; s < 0.2; s += 0.001)
            even = even && SwirlFx::passNearFor(0.6, s) == SwirlFx::passNearFor(0.6, -s);
        check(even, "passNearFor: the same before the closest point as after it");

        // With the Filter: d = Depth (1 - lead x open), 30 ms behind; Depth late with the filter closed.
        const auto in = sine(1000.0, fs, 6.5, 0.1f);
        auto openOf = [](double t) { return static_cast<float>(0.5 - 0.5 * std::cos(2.0 * c_pi * t / 2.0)); }; // 0 .. 1, a sweep every 2 s
        const auto swept = splitTape(in, fs, 0, 1, [=](double t, Step& s, SwirlFx&)
        {
            s.tape.motion = SwirlFx::WithFilter;
            s.tape.depthMs = 4.0f;
            s.tape.wobble = 0.0f;
            s.filterOpen = openOf(t);
            s.passNear = { 1.0f, 1.0f, 1.0f }; // (not what it goes by)
        });
        const auto d = delayFromPhase(swept.wet, swept.dry, 1000.0, fs, 0.05, 6.4, swept.run.offset[0][swept.run.chunkAt(0.05)]);
        double worst = 0.0;
        for (double t = 2.2; t < 6.2; t += 0.01)
            worst = std::max(worst, std::abs(d[static_cast<size_t>(t * fs)] - 4.0 * (1.0 - SwirlFx::c_filterLead * static_cast<double>(openOf(t - 0.03)))));
        const double closed = d[static_cast<size_t>(4.03 * fs)], open = d[static_cast<size_t>(5.03 * fs)];
        std::cout << "         With the Filter, Depth 4 ms: " << num(closed, 2) << " ms with the filter closed, " << num(open, 2) << " ms at its most open; off the law by " << num(worst * 1000.0, 1) << " us at most" << std::endl;
        check(worst < 0.02, "with the filter: d = Depth (1 - " + num(SwirlFx::c_filterLead, 2) + " open), 30 ms behind");
        check(std::abs(closed - 4.0) < 0.05 && std::abs(open + 4.0 * (SwirlFx::c_filterLead - 1.0)) < 0.05,
              "with the filter: Depth late while it is closed, (lead - 1) x Depth early at its most open");
        // Rate rests in it as it does With the Pass: nothing of On Its Own's sweep is in the move.
        const auto fast = splitTape(in, fs, 0, 1, [=](double t, Step& s, SwirlFx&)
        {
            s.tape.motion = SwirlFx::WithFilter;
            s.tape.depthMs = 4.0f;
            s.tape.wobble = 0.0f;
            s.tape.rateHz = 4.0f;
            s.filterOpen = openOf(t);
        });
        check(fast.run.offset[0] == swept.run.offset[0], "with the filter: Rate changes nothing");
    }

    std::cout << "No allocation, and a clean start" << std::endl;
    {
        SwirlFx fx;
        fx.prepare(192000.0);
        fx.setSeed(5u);
        auto in = noise(192000.0, 0.5, 0.5f, 3u);
        std::array<std::vector<float>, 3> lane { in, in, in };
        std::vector<float> sideL = in, sideR = in;
        SwirlFx::Tape tape;
        tape.feedback = 0.7f;
        SwirlFx::Doubler doubler;
        const long before = g_allocations.load();
        int calls = 0;
        for (size_t at = 0; at + 32 <= in.size(); at += 32, ++calls)
        {
            SwirlFx::Lanes ln;
            ln.count = 3;
            for (size_t l = 0; l < 3; ++l)
                ln.lane[l] = lane[l].data() + at;
            if (calls % 97 == 13)
                ln.lane[2] = nullptr; // no throw in flight
            ln.passNearTo = { 0.3f, 0.6f, static_cast<float>(SwirlFx::passNearFor(0.9, 0.001 * (calls % 300 - 150))) };
            ln.filterOpen = static_cast<float>(calls % 200) / 200.0f;
            const int phase = (calls / 400) % 5;
            tape.motion = calls / 150 % 4;
            tape.varispeed = phase == 1 ? 1.0f : (phase == 3 ? -1.0f : 0.0f);
            doubler.wave = calls / 150 % 3;
            doubler.thicken = calls / 300 % 2 == 0;
            if (phase == 2)
                fx.processDoubler(doubler, 0.8f, ln, calls % 7 == 0 ? 17 : 32);
            else
            {
                fx.processTape(tape, 0.8f, ln, calls % 7 == 0 ? 17 : 32);
                fx.delaySides(sideL.data() + at, sideR.data() + at, 32);
            }
            if (calls == 1000) fx.recentre();
            if (calls == 2200) { fx.reset(); fx.setSeed(9u); fx.scatter(0.4f); }
        }
        const long made = g_allocations.load() - before;
        check(made == 0, "nothing allocates in " + std::to_string(calls) + " calls: both types, every Motion and Wave, changes of type, reset(), scatter(), recentre(), delaySides() (" + std::to_string(made) + ")");

        // reset() after sound: silence in gives silence out. And straight from one type to the other.
        std::vector<float> quiet(4096, 0.0f);
        fx.reset();
        SwirlFx::Lanes ln;
        ln.count = 1;
        bool silent = true;
        for (size_t at = 0; at + 32 <= quiet.size(); at += 32)
        {
            ln.lane[0] = quiet.data() + at;
            fx.processDoubler(doubler, 1.0f, ln, 32);
        }
        for (float v : quiet) silent = silent && v == 0.0f;
        check(silent, "reset() leaves nothing in the lines");
        auto loud = noise(192000.0, 0.2, 0.5f, 4u);
        tape.feedback = 0.9f;
        for (size_t at = 0; at + 32 <= loud.size(); at += 32)
        {
            ln.lane[0] = loud.data() + at;
            fx.processTape(tape, 1.0f, ln, 32);
        }
        std::fill(quiet.begin(), quiet.end(), 0.0f);
        for (size_t at = 0; at + 32 <= quiet.size(); at += 32)
        {
            ln.lane[0] = quiet.data() + at;
            fx.processDoubler(doubler, 1.0f, ln, 32);
        }
        silent = true;
        for (float v : quiet) silent = silent && v == 0.0f;
        check(silent, "going straight from the Tape Flanger to the Doubler leaves none of the first in the second");

        // A lane that is not there for a while (a room's throws) comes back with nothing of what it held.
        for (int type = 0; type < 2; ++type)
        {
            fx.reset();
            const auto burst = noise(192000.0, 0.2, 0.5f, 6u);
            std::vector<float> first(burst.size() * 3, 0.0f);
            SwirlFx::Lanes three;
            three.count = 3;
            bool clean = true;
            for (size_t at = 0; at + 32 <= first.size(); at += 32)
            {
                three.lane[0] = first.data() + at;
                const bool there = at + 32 <= burst.size() || at >= 2 * burst.size(); // gone for 0.2 s in the middle
                std::vector<float> third(32, 0.0f);
                if (at + 32 <= burst.size())
                    std::copy(burst.begin() + static_cast<long>(at), burst.begin() + static_cast<long>(at) + 32, third.begin());
                three.lane[2] = there ? third.data() : nullptr;
                if (type == 0) fx.processTape(tape, 1.0f, three, 32); else fx.processDoubler(doubler, 1.0f, three, 32);
                if (at >= 2 * burst.size())
                    for (float v : third) clean = clean && v == 0.0f;
            }
            check(clean, std::string(type == 0 ? "tape flanger" : "doubler") + ": a lane that was away for 0.2 s comes back clean");
        }

        // reset() while nothing has run costs nothing: the engine may call it every chunk.
        fx.reset();
        const double idle = secondsFor([&] { for (int i = 0; i < 1000000; ++i) fx.reset(); }, 3);
        std::cout << "         a million reset() calls with nothing processed in between: " << num(idle * 1000.0, 2) << " ms" << std::endl;
        check(idle < 0.05, "reset() is free when nothing has been processed since the last one");

        // scatter() sets where the sweeps start.
        fx.reset();
        fx.scatter(0.25f);
        std::vector<float> one(32, 0.0f);
        ln.lane[0] = one.data();
        SwirlFx::Tape own;
        own.motion = SwirlFx::OnItsOwn;
        own.wobble = 0.0f;
        fx.processTape(own, 1.0f, ln, 32);
        check(std::abs(static_cast<double>(fx.getLaneOffsetMs(0)) - 4.0 * SwirlFx::ownShape(0.25)) < 1.0e-3, "scatter(0.25) starts On Its Own a quarter of the way round (" + num(fx.getLaneOffsetMs(0), 2) + " ms)");
    }

    std::cout << "Cost at 192 kHz, three lanes" << std::endl;
    {
        const double rate = 192000.0, seconds = 10.0;
        const auto in = noise(rate, seconds, 0.25f, 11u);
        const size_t chunks = in.size() / 32;
        std::array<std::vector<float>, 3> lane { in, in, in };
        auto time = [&](const std::function<void(SwirlFx&, SwirlFx::Lanes&, size_t)>& call)
        {
            return secondsFor([&]
            {
                SwirlFx fx;
                fx.prepare(rate);
                for (auto& l : lane) l = in;
                for (size_t c = 0; c < chunks; ++c)
                {
                    SwirlFx::Lanes ln;
                    ln.count = 3;
                    for (size_t l = 0; l < 3; ++l)
                        ln.lane[l] = lane[l].data() + c * 32;
                    const float nearNow = 0.5f - 0.5f * std::cos(static_cast<float>(c) * 0.0003f);
                    ln.passNearTo = { nearNow, nearNow, nearNow };
                    call(fx, ln, c);
                }
            });
        };
        SwirlFx::Tape tape;
        SwirlFx::Doubler doubler;
        const double tapeDefault = time([&](SwirlFx& fx, SwirlFx::Lanes& ln, size_t) { fx.processTape(tape, 1.0f, ln, 32); });
        SwirlFx::Tape full = tape;
        full.feedback = 0.8f;
        full.motion = SwirlFx::OnItsOwn;
        full.rateHz = 2.0f;
        full.wobble = 1.0f;
        const double tapeFull = time([&](SwirlFx& fx, SwirlFx::Lanes& ln, size_t c) { full.varispeed = c % 4000 < 2000 ? 0.8f : -0.8f; fx.processTape(full, 1.0f, ln, 32); });
        const double dbl = time([&](SwirlFx& fx, SwirlFx::Lanes& ln, size_t) { fx.processDoubler(doubler, 1.0f, ln, 32); });
        SwirlFx::Doubler sineWave = doubler;
        sineWave.wave = SwirlFx::WaveSine;
        sineWave.thicken = false;
        sineWave.feedback = 0.8f;
        const double dblSine = time([&](SwirlFx& fx, SwirlFx::Lanes& ln, size_t) { fx.processDoubler(sineWave, 1.0f, ln, 32); });

        // Today's Swirl at the top of the wheel, the same three lanes.
        const double classic = secondsFor([&]
        {
            std::array<ClassicLane, 3> lanes;
            for (auto& l : lanes) l.line.prepare(static_cast<int>(0.03 * rate));
            for (auto& l : lane) l = in;
            float a0 = -0.5f, ms0 = 3.0f;
            for (size_t c = 0; c < chunks; ++c)
            {
                const float lfo = std::sin(static_cast<float>(c) * 0.002f);
                const float t = std::tan(sj::c_pi * sj::clamp(260.0f * std::exp2(3.6f * (0.5f + 0.5f * lfo)), 20.0f, static_cast<float>(rate) * 0.45f) / static_cast<float>(rate));
                const float a1 = (t - 1.0f) / (t + 1.0f), ms1 = 0.3f + 6.5f * (0.5f + 0.4f * lfo);
                for (size_t l = 0; l < 3; ++l)
                    lanes[l].run(lane[l].data() + c * 32, 32, a0, a1, ms0, ms1, 0.001f * static_cast<float>(rate));
                a0 = a1;
                ms0 = ms1;
            }
        });
        auto line = [&](const char* name, double s)
        {
            std::cout << "         " << name << ": " << num(1.0e9 * s / (seconds * rate * 3.0), 1) << " ns per lane sample, " << num(100.0 * s / seconds, 2) << "% of a core" << std::endl;
        };
        line("Tape Flanger at its defaults (no feedback)      ", tapeDefault);
        line("Tape Flanger, Feedback, On Its Own, Wobble, lean", tapeFull);
        line("Doubler at its defaults (Random, Thicken)       ", dbl);
        line("Doubler, Sine, Feedback                         ", dblSine);
        line("today's Swirl's inner loop (phaser + flanger)   ", classic);
        // (With everything in it runs level with the phaser and flanger pair: 102% of it on one build machine, so a tenth is allowed for.)
        check((tapeDefault <= classic && tapeFull <= 1.1 * classic) || !TestClock::holdsCostLimits(), "the Tape Flanger costs about what today's Swirl does or less (" + num(100.0 * tapeFull / classic, 0) + "% of it with everything in)");
        check((dbl <= classic && dblSine <= classic) || !TestClock::holdsCostLimits(), "the Doubler costs no more than today's Swirl (" + num(100.0 * std::max(dbl, dblSine) / classic, 0) + "% of it)");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : std::to_string(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

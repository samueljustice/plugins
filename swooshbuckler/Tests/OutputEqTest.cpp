// Output EQ test. Plain C++, no JUCE: OutputEq is free of it.
//
//   - Each band against its analytic response: the measured response (an impulse through process(), then a Fourier
//     sum) within 0.1 dB of the textbook analogue shape of that band, warped to the sample rate, wherever that
//     shape is above -60 dB; at 44.1, 48, 96 and 192 kHz, at the ends and the middle of every control. The shapes
//     here are written out on their own (as ratios of polynomials in s), not taken from OutputEq, and
//     responseDb() (what the graph draws) is held against the same shapes.
//   - Flat is bit-exact: at its defaults, switched off, with every gain back at 0 dB and with the cuts back at
//     Off, every sample comes out as it went in.
//   - No click: a steady tone through every kind of move, against the same change spliced in with no glide. What a
//     move puts more than 1 kHz away from the tone, and the largest step between two samples.
//   - Every channel alike: the same curve on each, so the mains and the LFE summed through a 24 dB an octave cut at
//     80 Hz are the sum put through the cut.
//   - No delay: the first sample of an impulse comes out where it went in.
//   - The Low Cut's noise floor at 20 Hz and 192 kHz.
//   - Block sizes: the same samples however the stream is cut up.
//   - Speed: 7.1.4 at 192 kHz with every band in.
//
//   SwooshbucklerOutputEq      prints what it measures and ends with "ALL PASSED" (exit code 0) or "n FAILED".

#include "OutputEq.h"
#include "TestClock.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    using Complex = std::complex<double>;
    constexpr double c_pi = 3.14159265358979323846;
    int g_failures = 0;

    void check(bool ok, const std::string& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }

    std::string num(double v, int places = 2)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "%.*f", places, v);
        return text;
    }

    double db(double gain) { return 20.0 * std::log10(std::max(gain, 1.0e-30)); }

    struct Noise
    {
        std::uint32_t state = 12345u;
        float next()
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>((state >> 8) & 0xffffff) / 8388608.0f - 1.0f;
        }
    };

    //==========================================================================
    // The textbook shapes, in s (1 at the band's own frequency). Written from the analogue prototypes; nothing here
    // is OutputEq's.
    Complex highPass(Complex s, double q) { return s * s / (s * s + s / q + 1.0); }
    Complex lowPass(Complex s, double q) { return 1.0 / (s * s + s / q + 1.0); }
    Complex bell(Complex s, double gainDb, double q)
    {
        const double a = std::pow(10.0, gainDb / 40.0);
        return (s * s + s * (a / q) + 1.0) / (s * s + s / (a * q) + 1.0);
    }
    Complex lowShelf(Complex s, double gainDb)
    {
        const double a = std::pow(10.0, gainDb / 40.0), b = std::sqrt(a) / std::sqrt(0.5);
        return a * (s * s + b * s + a) / (a * s * s + b * s + 1.0);
    }
    Complex highShelf(Complex s, double gainDb)
    {
        const double a = std::pow(10.0, gainDb / 40.0), b = std::sqrt(a) / std::sqrt(0.5);
        return a * (a * s * s + b * s + 1.0) / (s * s + b * s + a);
    }

    /** s for a band at f0, heard at hz, in a filter made by the bilinear transform at sampleRate. */
    Complex warped(double hz, double f0, double sampleRate)
    {
        return Complex(0.0, std::tan(c_pi * hz / sampleRate) / std::tan(c_pi * f0 / sampleRate));
    }

    /** The whole curve of these settings, or one band's. */
    Complex analytic(const OutputEq::Params& p, double hz, double fs, int band = -1)
    {
        Complex h = 1.0;
        if (! p.on)
            return h;
        auto wants = [band](int b) { return band < 0 || band == b; };
        if (wants(OutputEq::LowCut) && p.lowCutHz > 0.0f)
        {
            const auto s = warped(hz, p.lowCutHz, fs);
            h *= p.lowCutSlope >= 24 ? highPass(s, 1.0 / (2.0 * std::cos(c_pi / 8.0))) * highPass(s, 1.0 / (2.0 * std::cos(3.0 * c_pi / 8.0)))
                                     : highPass(s, std::sqrt(0.5));
        }
        if (wants(OutputEq::LowShelf))
            h *= lowShelf(warped(hz, p.lowShelfHz, fs), p.lowShelfDb);
        if (wants(OutputEq::LowMid))
            h *= bell(warped(hz, p.lowMidHz, fs), p.lowMidDb, p.lowMidQ);
        if (wants(OutputEq::HighMid))
            h *= bell(warped(hz, p.highMidHz, fs), p.highMidDb, p.highMidQ);
        if (wants(OutputEq::HighShelf))
            h *= highShelf(warped(hz, p.highShelfHz, fs), p.highShelfDb);
        if (wants(OutputEq::HighCut) && p.highCutHz > 0.0f)
            h *= lowPass(warped(hz, p.highCutHz, fs), std::sqrt(0.5));
        return h;
    }

    /** The unwarped analogue curve (what an EQ with no top to its band would do), for the note on squeezing. */
    double analogueBellDb(double hz, double f0, double gainDb, double q) { return db(std::abs(bell(Complex(0.0, hz / f0), gainDb, q))); }

    //==========================================================================
    /** Runs `seconds` of one channel through an EQ set to p from the start, in blocks of 512. */
    std::vector<float> through(const OutputEq::Params& p, double fs, std::vector<float> x)
    {
        OutputEq eq;
        eq.setParams(p);
        eq.prepare(fs, 1);
        for (size_t pos = 0; pos < x.size(); pos += 512)
        {
            float* chans[1] = { x.data() + pos };
            eq.setParams(p);
            eq.process(chans, 1, static_cast<int>(std::min<size_t>(512, x.size() - pos)));
        }
        return x;
    }

    Complex fourier(const std::vector<float>& x, double hz, double fs, size_t from = 0)
    {
        const Complex turn = std::polar(1.0, -2.0 * c_pi * hz / fs);
        Complex w = 1.0, sum = 0.0;
        for (size_t i = from; i < x.size(); ++i)
        {
            sum += static_cast<double>(x[i]) * w;
            w *= turn;
            if ((i & 1023) == 1023)
                w /= std::abs(w);
        }
        return sum;
    }

    std::vector<double> testFrequencies(double fs)
    {
        std::vector<double> f;
        const double top = std::min(20000.0, 0.45 * fs);
        for (int i = 0; i <= 40; ++i)
            f.push_back(20.0 * std::pow(top / 20.0, i / 40.0));
        return f;
    }

    struct Worst
    {
        double measured = 0.0, drawn = 0.0;
        std::string where;
    };

    /** One setting at one rate: the measured response and responseDb() against the analytic one. */
    void measure(const OutputEq::Params& p, double fs, int band, const std::string& name, Worst& worst)
    {
        std::vector<float> x(static_cast<size_t>(fs * 1.5), 0.0f);
        x[0] = 1.0f;
        const auto y = through(p, fs, x);
        for (double hz : testFrequencies(fs))
        {
            const double want = db(std::abs(analytic(p, hz, fs, band)));
            const double drawn = std::abs(OutputEq::responseDb(p, hz, fs, band) - want);
            if (drawn > worst.drawn)
                worst.drawn = drawn;
            if (want < -60.0)
                continue;
            const double off = std::abs(db(std::abs(fourier(y, hz, fs))) - want);
            if (off > worst.measured)
            {
                worst.measured = off;
                worst.where = name + " at " + num(hz, 0) + " Hz, " + num(fs / 1000.0, 1) + " kHz";
            }
        }
    }

    //==========================================================================
    // The click test: a steady tone, one move, and what the move adds.
    constexpr double c_clickFs = 48000.0;

    struct ClickResult
    {
        double sidebandDb = -300.0; // the energy more than 1 kHz from the tone, against all of it
        double step = 0.0;          // the largest step between samples through the move, against the tone's own
    };

    /** sum |X(f)|^2 of a Hann-windowed stretch, more than 1 kHz from the tone, against the whole. */
    double sidebands(const std::vector<float>& y, size_t centre, double toneHz)
    {
        const size_t n = 8192, from = centre - n / 2;
        std::vector<double> w(n);
        double total = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            w[i] = y[from + i] * (0.5 - 0.5 * std::cos(2.0 * c_pi * (static_cast<double>(i) + 0.5) / static_cast<double>(n)));
            total += w[i] * w[i];
        }
        // What is near the tone, taken out by fitting: every bin within 1 kHz.
        double near = 0.0;
        const double binHz = c_clickFs / static_cast<double>(n);
        for (int k = 0; k <= static_cast<int>(n / 2); ++k)
        {
            const double hz = k * binHz;
            if (std::abs(hz - toneHz) > 1000.0)
                continue;
            Complex sum = 0.0;
            const Complex turn = std::polar(1.0, -2.0 * c_pi * k / static_cast<double>(n));
            Complex ph = 1.0;
            for (size_t i = 0; i < n; ++i)
            {
                sum += w[i] * ph;
                ph *= turn;
            }
            near += (k == 0 || k == static_cast<int>(n / 2) ? 1.0 : 2.0) * std::norm(sum) / static_cast<double>(n);
        }
        return 10.0 * std::log10(std::max(1.0e-30, (total - near) / total));
    }

    /** Plays a tone through settings that change from `before` to `after` half-way (given to the EQ every 512
        samples, as the plug-in does), or step by step with `drag`. */
    ClickResult click(double toneHz, const OutputEq::Params& before, const OutputEq::Params& after,
                      const std::function<OutputEq::Params(double)>& drag = nullptr)
    {
        const size_t n = static_cast<size_t>(c_clickFs * 2.0), half = n / 2;
        std::vector<float> x(n);
        for (size_t i = 0; i < n; ++i)
            x[i] = static_cast<float>(0.25 * std::sin(1.0 + 2.0 * c_pi * toneHz * static_cast<double>(i) / c_clickFs));
        OutputEq eq;
        eq.setParams(before);
        eq.prepare(c_clickFs, 1);
        for (size_t pos = 0; pos < n; pos += 512)
        {
            const double sinceMove = (static_cast<double>(pos) - static_cast<double>(half)) / c_clickFs;
            eq.setParams(pos < half ? before : drag != nullptr ? drag(std::min(1.0, sinceMove / 0.5)) : after);
            float* chans[1] = { x.data() + pos };
            eq.process(chans, 1, static_cast<int>(std::min<size_t>(512, n - pos)));
        }
        ClickResult r;
        // The move and everything it sets going lie between the half-way point and 0.6 s after it.
        for (size_t centre = half - 2048; centre + 4096 < n - 8192; centre += 2048)
            r.sidebandDb = std::max(r.sidebandDb, sidebands(x, centre, toneHz));
        auto largestStep = [&x](size_t from, size_t to) {
            double m = 0.0;
            for (size_t i = from + 1; i < to; ++i)
                m = std::max(m, static_cast<double>(std::abs(x[i] - x[i - 1])));
            return m;
        };
        const double settledBefore = largestStep(half - 4800, half), settledAfter = largestStep(n - 4800, n);
        double own = std::max(settledBefore, settledAfter);
        if (drag != nullptr) // (a drag passes through louder settings than either end)
            own = std::max(own, 4.0 * 0.25 * 2.0 * c_pi * toneHz / c_clickFs);
        r.step = largestStep(half - 64, n - 4800) / std::max(own, 1.0e-9);
        return r;
    }

    /** What a click reads as: the same two settings with no glide between them (two EQs, spliced half-way). */
    ClickResult splice(double toneHz, const OutputEq::Params& before, const OutputEq::Params& after)
    {
        const size_t n = static_cast<size_t>(c_clickFs * 2.0), half = n / 2;
        std::vector<float> x(n);
        for (size_t i = 0; i < n; ++i)
            x[i] = static_cast<float>(0.25 * std::sin(1.0 + 2.0 * c_pi * toneHz * static_cast<double>(i) / c_clickFs));
        auto y = through(before, c_clickFs, x);
        const auto late = through(after, c_clickFs, x);
        for (size_t i = half; i < n; ++i)
            y[i] = late[i];
        ClickResult r;
        for (size_t centre = half - 2048; centre <= half + 2048; centre += 2048)
            r.sidebandDb = std::max(r.sidebandDb, sidebands(y, centre, toneHz));
        double own = 0.0;
        for (size_t i = half - 4800; i < half; ++i)
            own = std::max(own, static_cast<double>(std::abs(y[i] - y[i - 1])));
        for (size_t i = n - 4800; i < n; ++i)
            own = std::max(own, static_cast<double>(std::abs(y[i] - y[i - 1])));
        r.step = static_cast<double>(std::abs(y[half] - y[half - 1])) / std::max(own, 1.0e-9);
        return r;
    }

    OutputEq::Params everyBandIn()
    {
        OutputEq::Params p;
        p.lowCutHz = 80.0f;
        p.lowCutSlope = 24;
        p.lowShelfDb = 6.0f;
        p.lowMidDb = -9.0f;
        p.lowMidQ = 2.0f;
        p.highMidDb = 12.0f;
        p.highMidQ = 0.7f;
        p.highShelfDb = -6.0f;
        p.highCutHz = 12000.0f;
        return p;
    }
}

int main()
{
    //==========================================================================
    std::cout << "Each band against its analytic response" << std::endl;
    {
        const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
        Worst worst[OutputEq::c_bands + 1];
        const char* names[OutputEq::c_bands + 1] = { "Low Cut", "Low Shelf", "Low Mid", "High Mid", "High Shelf", "High Cut", "every band together" };
        int settings = 0;
        for (double fs : rates)
        {
            for (float hz : { 20.0f, 80.0f, 1000.0f })
                for (int slope : { 12, 24 })
                {
                    OutputEq::Params p;
                    p.lowCutHz = hz;
                    p.lowCutSlope = slope;
                    measure(p, fs, OutputEq::LowCut, "Low Cut " + num(hz, 0) + " Hz " + std::to_string(slope) + " dB", worst[OutputEq::LowCut]);
                    ++settings;
                }
            for (float hz : { 30.0f, 100.0f, 600.0f })
                for (float gain : { -12.0f, -3.0f, 12.0f })
                {
                    OutputEq::Params p;
                    p.lowShelfHz = hz;
                    p.lowShelfDb = gain;
                    measure(p, fs, OutputEq::LowShelf, "Low Shelf " + num(hz, 0) + " Hz " + num(gain, 0) + " dB", worst[OutputEq::LowShelf]);
                    ++settings;
                }
            for (int which = 0; which < 2; ++which)
                for (float hz : { 40.0f, 300.0f, 3000.0f, 16000.0f })
                    for (float gain : { -12.0f, 12.0f })
                        for (float q : { 0.3f, 1.0f, 8.0f })
                        {
                            OutputEq::Params p;
                            (which == 0 ? p.lowMidHz : p.highMidHz) = hz;
                            (which == 0 ? p.lowMidDb : p.highMidDb) = gain;
                            (which == 0 ? p.lowMidQ : p.highMidQ) = q;
                            const int band = which == 0 ? OutputEq::LowMid : OutputEq::HighMid;
                            measure(p, fs, band, std::string(names[band]) + " " + num(hz, 0) + " Hz " + num(gain, 0) + " dB Q " + num(q, 1), worst[band]);
                            ++settings;
                        }
            for (float hz : { 1000.0f, 8000.0f, 18000.0f })
                for (float gain : { -12.0f, -3.0f, 12.0f })
                {
                    OutputEq::Params p;
                    p.highShelfHz = hz;
                    p.highShelfDb = gain;
                    measure(p, fs, OutputEq::HighShelf, "High Shelf " + num(hz, 0) + " Hz " + num(gain, 0) + " dB", worst[OutputEq::HighShelf]);
                    ++settings;
                }
            for (float hz : { 1000.0f, 8000.0f, 20000.0f })
            {
                OutputEq::Params p;
                p.highCutHz = hz;
                measure(p, fs, OutputEq::HighCut, "High Cut " + num(hz, 0) + " Hz", worst[OutputEq::HighCut]);
                ++settings;
            }
            measure(everyBandIn(), fs, -1, "every band", worst[OutputEq::c_bands]);
            ++settings;
        }
        std::cout << "         " << settings << " settings at 44.1, 48, 96 and 192 kHz, 41 frequencies each from 20 Hz to 20 kHz (or 0.45 of the rate)" << std::endl;
        double drawn = 0.0;
        for (int b = 0; b <= OutputEq::c_bands; ++b)
        {
            std::cout << "         " << names[b] << ": measured within " << num(worst[b].measured, 5) << " dB (worst: " << worst[b].where
                      << "), drawn within " << num(worst[b].drawn, 9) << " dB" << std::endl;
            check(worst[b].measured < 0.1, std::string(names[b]) + ": the sound is within 0.1 dB of the analytic response");
            drawn = std::max(drawn, worst[b].drawn);
        }
        check(drawn < 1.0e-6, "responseDb() is the analytic response (the graph draws what is heard)");

        // One band of responseDb() at a time adds up to the whole.
        const auto all = everyBandIn();
        double apart = 0.0;
        for (double hz : testFrequencies(48000.0))
        {
            double sum = 0.0;
            for (int b = 0; b < OutputEq::c_bands; ++b)
                sum += OutputEq::responseDb(all, hz, 48000.0, b);
            apart = std::max(apart, std::abs(sum - OutputEq::responseDb(all, hz, 48000.0)));
        }
        check(apart < 1.0e-9, "responseDb(): the bands one at a time add up to the whole curve");
        auto off = all;
        off.on = false;
        check(OutputEq::responseDb(off, 1000.0, 48000.0) == 0.0 && OutputEq::isFlat(off) && ! OutputEq::isFlat(all),
              "switched off, the curve is flat and isFlat() says so");

        // Glided to, not started at: the same filter.
        {
            const double fs = 48000.0;
            OutputEq eq;
            eq.prepare(fs, 1);
            std::vector<float> x(static_cast<size_t>(fs * 1.5), 0.0f);
            const size_t at = 9600; // 200 ms in: every glide is over
            x[at] = 1.0f;
            for (size_t pos = 0; pos < x.size(); pos += 512)
            {
                float* chans[1] = { x.data() + pos };
                eq.setParams(all);
                eq.process(chans, 1, static_cast<int>(std::min<size_t>(512, x.size() - pos)));
            }
            double worstGlided = 0.0;
            for (double hz : testFrequencies(fs))
            {
                const double want = db(std::abs(analytic(all, hz, fs)));
                if (want > -60.0)
                    worstGlided = std::max(worstGlided, std::abs(db(std::abs(fourier(x, hz, fs, at))) - want));
            }
            std::cout << "         every band glided in from flat: within " << num(worstGlided, 5) << " dB" << std::endl;
            check(worstGlided < 0.1, "settings glided to give the same response as settings started at");
        }

        // How far the top bell is squeezed at 48 kHz (accepted and drawn): against a bell with no top to its band.
        OutputEq::Params top;
        top.highMidHz = 10000.0f;
        top.highMidDb = 12.0f;
        std::cout << "         a +12 dB bell at 10 kHz, Q 1, heard at 16 kHz: " << num(OutputEq::responseDb(top, 16000.0, 48000.0), 2) << " dB at 48 kHz, "
                  << num(OutputEq::responseDb(top, 16000.0, 192000.0), 2) << " dB at 192 kHz, " << num(analogueBellDb(16000.0, 10000.0, 12.0, 1.0), 2)
                  << " dB with no top to the band" << std::endl;
    }

    //==========================================================================
    std::cout << "Flat is bit-exact" << std::endl;
    {
        const double fs = 48000.0;
        const int n = 48000;
        auto noise = [n](int channels) {
            std::vector<std::vector<float>> x(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(n)));
            Noise r;
            for (auto& ch : x)
                for (auto& v : ch)
                    v = r.next();
            return x;
        };
        // Runs the stages (settings, seconds) one after another; returns the samples from the last one that differ
        // from what went in, and how long after the last change the EQ let go.
        struct Stage
        {
            OutputEq::Params p;
            double seconds;
        };
        auto run = [&](const std::vector<Stage>& stages, int& changedInLast, double& letGoMs, bool& bypassed, bool& changedBefore) {
            auto x = noise(2);
            const auto in = x;
            OutputEq eq;
            eq.setParams(stages.front().p);
            eq.prepare(fs, 2);
            size_t pos = 0;
            changedInLast = 0;
            letGoMs = -1.0;
            changedBefore = false;
            for (size_t s = 0; s < stages.size(); ++s)
            {
                const size_t end = std::min<size_t>(static_cast<size_t>(n), pos + static_cast<size_t>(stages[s].seconds * fs));
                const size_t stageStart = pos;
                size_t lastChanged = stageStart;
                bool any = false;
                while (pos < end)
                {
                    const int len = static_cast<int>(std::min<size_t>(512, end - pos));
                    float* chans[2] = { x[0].data() + pos, x[1].data() + pos };
                    eq.setParams(stages[s].p);
                    eq.process(chans, 2, len);
                    for (size_t i = pos; i < pos + static_cast<size_t>(len); ++i)
                        if (x[0][i] != in[0][i] || x[1][i] != in[1][i])
                        {
                            lastChanged = i + 1;
                            any = true;
                        }
                    pos += static_cast<size_t>(len);
                }
                if (s + 1 == stages.size())
                {
                    letGoMs = 1000.0 * static_cast<double>(lastChanged - stageStart) / fs;
                    // "Changed" in the last stage: anything after the first 60 ms of it.
                    for (size_t i = stageStart + static_cast<size_t>(0.06 * fs); i < end; ++i)
                        changedInLast += (x[0][i] != in[0][i] || x[1][i] != in[1][i]) ? 1 : 0;
                }
                else
                    changedBefore = changedBefore || any;
            }
            bypassed = eq.isBypassed();
        };

        int changed = 0;
        double letGo = 0.0;
        bool bypassed = false, before = false;
        const OutputEq::Params defaults;
        check(OutputEq::isFlat(defaults), "defaults: the EQ opens flat");
        run({ { defaults, 0.5 } }, changed, letGo, bypassed, before);
        check(changed == 0 && letGo == 0.0 && bypassed, "at its defaults every sample comes out exactly as it went in");

        auto off = everyBandIn();
        off.on = false;
        run({ { off, 0.5 } }, changed, letGo, bypassed, before);
        check(changed == 0 && letGo == 0.0 && bypassed, "switched off with every band set: exactly as it went in, from the first sample");

        auto gainsHome = everyBandIn();
        gainsHome.lowShelfDb = gainsHome.lowMidDb = gainsHome.highMidDb = gainsHome.highShelfDb = 0.0f;
        gainsHome.lowCutHz = gainsHome.highCutHz = 0.0f;
        run({ { everyBandIn(), 0.3 }, { gainsHome, 0.5 } }, changed, letGo, bypassed, before);
        std::cout << "         every band in, then every gain to 0 dB and both cuts Off: the last changed sample is " << num(letGo, 1) << " ms after" << std::endl;
        check(before, "(with every band in the sound is changed)");
        check(changed == 0 && bypassed && letGo < 60.0, "gains back at 0 dB and cuts back at Off: bit-exact again within 60 ms, and the EQ is bypassed");

        run({ { everyBandIn(), 0.3 }, { off, 0.5 } }, changed, letGo, bypassed, before);
        std::cout << "         every band in, then switched off: the last changed sample is " << num(letGo, 1) << " ms after" << std::endl;
        check(changed == 0 && bypassed && letGo < 60.0, "switched off: bit-exact again within 60 ms");

        OutputEq::Params cut;
        cut.lowCutHz = 1000.0f;
        cut.highCutHz = 1000.0f;
        run({ { defaults, 0.1 }, { cut, 0.3 }, { defaults, 0.5 } }, changed, letGo, bypassed, before);
        std::cout << "         both cuts at 1 kHz, then Off: the last changed sample is " << num(letGo, 1) << " ms after" << std::endl;
        check(before && changed == 0 && bypassed && letGo < 60.0, "a cut going Off: bit-exact again within 60 ms");

        // One band in leaves the others out: the EQ is not bypassed, and comes back.
        OutputEq::Params one;
        one.lowMidDb = 3.0f;
        run({ { defaults, 0.1 }, { one, 0.3 }, { defaults, 0.2 }, { one, 0.2 }, { defaults, 0.2 } }, changed, letGo, bypassed, before);
        check(before && changed == 0 && bypassed, "a band taken in and out twice ends bit-exact");
    }

    //==========================================================================
    std::cout << "No click on a move (a 0.25 tone at 48 kHz; settings given every 512 samples)" << std::endl;
    {
        struct Move
        {
            std::string name;
            double tone;
            OutputEq::Params before, after;
        };
        std::vector<Move> moves;
        const OutputEq::Params flat;
        auto with = [&flat](const std::function<void(OutputEq::Params&)>& f) {
            auto p = flat;
            f(p);
            return p;
        };
        const auto bellUp = with([](auto& p) { p.lowMidHz = 100.0f; p.lowMidDb = 12.0f; });
        const auto bellDown = with([](auto& p) { p.lowMidHz = 100.0f; p.lowMidDb = -12.0f; });
        moves.push_back({ "a bell at the tone, 0 to +12 dB", 100.0, flat, bellUp });
        moves.push_back({ "a bell at the tone, +12 to -12 dB", 100.0, bellUp, bellDown });
        moves.push_back({ "a bell at the tone, -12 dB to flat", 100.0, bellDown, flat });
        moves.push_back({ "a +12 dB bell jumping from 100 Hz to 1 kHz", 100.0, bellUp, with([](auto& p) { p.lowMidHz = 1000.0f; p.lowMidDb = 12.0f; }) });
        moves.push_back({ "a +12 dB bell, Q 0.3 to 8", 100.0, with([](auto& p) { p.lowMidHz = 130.0f; p.lowMidDb = 12.0f; p.lowMidQ = 0.3f; }),
                          with([](auto& p) { p.lowMidHz = 130.0f; p.lowMidDb = 12.0f; p.lowMidQ = 8.0f; }) });
        moves.push_back({ "the Low Shelf, 0 to +12 dB", 100.0, flat, with([](auto& p) { p.lowShelfDb = 12.0f; }) });
        moves.push_back({ "the Low Shelf, +12 to -12 dB", 100.0, with([](auto& p) { p.lowShelfDb = 12.0f; }), with([](auto& p) { p.lowShelfDb = -12.0f; }) });
        const auto cut80 = with([](auto& p) { p.lowCutHz = 80.0f; });
        const auto cut200 = with([](auto& p) { p.lowCutHz = 200.0f; });
        moves.push_back({ "the Low Cut coming on at 80 Hz, 24 dB", 100.0, flat, cut80 });
        moves.push_back({ "the Low Cut at 80 Hz going Off", 100.0, cut80, flat });
        moves.push_back({ "the Low Cut coming on at 200 Hz, 24 dB", 100.0, flat, cut200 });
        moves.push_back({ "the Low Cut at 200 Hz going Off", 100.0, cut200, flat });
        moves.push_back({ "the Low Cut at 80 Hz, 24 to 12 dB", 100.0, cut80, with([](auto& p) { p.lowCutHz = 80.0f; p.lowCutSlope = 12; }) });
        moves.push_back({ "the Low Cut at 80 Hz, 12 to 24 dB", 100.0, with([](auto& p) { p.lowCutHz = 80.0f; p.lowCutSlope = 12; }), cut80 });
        moves.push_back({ "the Low Cut jumping from 20 Hz to 1 kHz", 100.0, with([](auto& p) { p.lowCutHz = 20.0f; }), with([](auto& p) { p.lowCutHz = 1000.0f; }) });
        auto lowIn = with([](auto& p) { p.lowCutHz = 60.0f; p.lowShelfDb = 9.0f; p.lowMidHz = 100.0f; p.lowMidDb = -6.0f; p.highMidDb = 12.0f; p.highShelfDb = 6.0f; p.highCutHz = 9000.0f; });
        auto lowOut = lowIn;
        lowOut.on = false;
        moves.push_back({ "every band in, switched off", 100.0, lowIn, lowOut });
        moves.push_back({ "every band in, switched on", 100.0, lowOut, lowIn });
        const auto shelfHigh = with([](auto& p) { p.highShelfHz = 5000.0f; p.highShelfDb = 12.0f; });
        moves.push_back({ "the High Shelf at the tone, 0 to +12 dB", 5000.0, flat, shelfHigh });
        moves.push_back({ "the High Shelf at the tone, +12 to -12 dB", 5000.0, shelfHigh, with([](auto& p) { p.highShelfHz = 5000.0f; p.highShelfDb = -12.0f; }) });
        const auto highCut = with([](auto& p) { p.highCutHz = 4000.0f; });
        moves.push_back({ "the High Cut coming on at 4 kHz", 5000.0, flat, highCut });
        moves.push_back({ "the High Cut at 4 kHz going Off", 5000.0, highCut, flat });
        moves.push_back({ "the High Cut jumping from 20 kHz to 1 kHz", 5000.0, with([](auto& p) { p.highCutHz = 20000.0f; }), with([](auto& p) { p.highCutHz = 1000.0f; }) });
        moves.push_back({ "the Low Cut coming on at 60 Hz, far under the tone", 5000.0, flat, with([](auto& p) { p.lowCutHz = 60.0f; }) });
        moves.push_back({ "the Low Cut at 60 Hz going Off, far under the tone", 5000.0, with([](auto& p) { p.lowCutHz = 60.0f; }), flat });
        moves.push_back({ "a bell at the tone, 0 to -12 dB", 5000.0, flat, with([](auto& p) { p.highMidHz = 5000.0f; p.highMidDb = -12.0f; }) });
        moves.push_back({ "every band in, switched off", 5000.0, lowIn, lowOut });
        moves.push_back({ "every band in, switched on", 5000.0, lowOut, lowIn });

        const double still = click(100.0, bellUp, bellUp).sidebandDb, stillHigh = click(5000.0, shelfHigh, shelfHigh).sidebandDb;
        std::cout << "         nothing moving: " << num(still, 1) << " dB more than 1 kHz from a 100 Hz tone, " << num(stillHigh, 1) << " dB from a 5 kHz tone" << std::endl;
        // Each move against the same two settings spliced with no glide (what a click would read as).
        double worstSide = -300.0, worstStep = 0.0, leastUnder = 300.0, worstQuiet = -300.0;
        for (const auto& m : moves)
        {
            const auto r = click(m.tone, m.before, m.after);
            const auto cut = splice(m.tone, m.before, m.after);
            std::cout << "         " << num(m.tone, 0) << " Hz, " << m.name << ": " << num(r.sidebandDb, 1) << " dB (spliced " << num(cut.sidebandDb, 1)
                      << " dB), largest step " << num(r.step, 3) << "x the tone's own (spliced " << num(cut.step, 1) << "x)" << std::endl;
            worstSide = std::max(worstSide, r.sidebandDb);
            worstStep = std::max(worstStep, r.step);
            leastUnder = std::min(leastUnder, cut.sidebandDb - r.sidebandDb);
            if (cut.sidebandDb - r.sidebandDb < 20.0)
                worstQuiet = std::max(worstQuiet, r.sidebandDb);
        }
        // A handle dragged for half a second: a bell from 60 Hz to 2 kHz and 0 to +12 dB and back, across the tone.
        const auto dragged = click(100.0, flat, flat, [&flat](double t) {
            auto p = flat;
            p.lowMidHz = static_cast<float>(60.0 * std::pow(2000.0 / 60.0, t));
            p.lowMidDb = static_cast<float>(12.0 * std::sin(c_pi * t));
            p.lowMidQ = 2.0f;
            return p;
        });
        std::cout << "         100 Hz, a bell dragged across the tone for half a second: " << num(dragged.sidebandDb, 1) << " dB, largest step " << num(dragged.step, 3) << "x" << std::endl;
        std::cout << "         worst of the moves: " << num(worstSide, 1) << " dB; the least any move is under its splice: " << num(leastUnder, 1)
                  << " dB; largest step " << num(worstStep, 3) << "x" << std::endl;
        check(worstSide < -55.0, "no move puts more than -55 dB of its tone further than 1 kHz from it");
        check(worstQuiet < -70.0, "every move is 20 dB or more under the same change made with no glide (or under -70 dB: a cut coming on far under the tone)");
        check(dragged.sidebandDb < -80.0, "a handle dragged across the tone stays under -80 dB (no zipper)");
        check(worstStep < 1.5, "no move makes a step between samples more than 1.5x the tone's own at either end (bands that leave one after another pass through louder settings; a splice reads 6x to 80x)");
    }

    //==========================================================================
    std::cout << "Every channel alike, and no delay" << std::endl;
    {
        // The mains and the LFE through a 24 dB an octave cut at 80 Hz: the LFE is a low-passed copy of the mains
        // (two one-poles at 120 Hz here). Their sum through the cut is the sum of each through the cut.
        const double fs = 48000.0;
        const int n = 96000;
        OutputEq::Params p;
        p.lowCutHz = 80.0f;
        p.lowCutSlope = 24;
        std::vector<float> mains(static_cast<size_t>(n)), lfe(static_cast<size_t>(n)), sum(static_cast<size_t>(n));
        Noise r;
        double lp1 = 0.0, lp2 = 0.0;
        const double c = 1.0 - std::exp(-2.0 * c_pi * 120.0 / fs);
        for (int i = 0; i < n; ++i)
        {
            const size_t k = static_cast<size_t>(i);
            mains[k] = 0.5f * r.next();
            lp1 += c * (mains[k] - lp1);
            lp2 += c * (lp1 - lp2);
            lfe[k] = static_cast<float>(lp2);
            sum[k] = mains[k] + lfe[k];
        }
        const auto sumIn = sum, lfeIn = lfe;
        OutputEq eq;
        eq.setParams(p);
        eq.prepare(fs, 3);
        for (int pos = 0; pos < n; pos += 512)
        {
            float* chans[3] = { mains.data() + pos, lfe.data() + pos, sum.data() + pos };
            eq.process(chans, 3, std::min(512, n - pos));
        }
        double err = 0.0, power = 0.0;
        for (size_t i = 0; i < static_cast<size_t>(n); ++i)
        {
            const double d = static_cast<double>(mains[i]) + lfe[i] - sum[i];
            err += d * d;
            power += static_cast<double>(sum[i]) * sum[i];
        }
        const double apart = 10.0 * std::log10(std::max(err, 1.0e-300) / power);
        std::cout << "         mains + LFE through a 24 dB cut at 80 Hz, against their sum through it: " << num(apart, 1) << " dB" << std::endl;
        check(apart < -120.0, "mains + LFE: the two cut and summed are the sum cut (below -120 dB apart)");
        // And the low end the two share is still there in step: at 160 Hz (an octave above the cut) the summed
        // level is what the curve says, against the sum that went in.
        const double got = db(std::abs(fourier(sum, 160.0, fs, 24000)) / std::abs(fourier(sumIn, 160.0, fs, 24000)));
        const double want = OutputEq::responseDb(p, 160.0, fs);
        std::cout << "         the sum at 160 Hz: " << num(got, 2) << " dB against the sum that went in; the curve says " << num(want, 2) << " dB" << std::endl;
        check(std::abs(got - want) < 0.3, "mains + LFE: the sum follows the curve (no cancelling between them)");

        // The same signal on 1, 2, 3, 6, 8, 10, 12 and 16 channels: every channel the same, to the bit, at every width.
        std::vector<float> reference;
        bool same = true;
        double widest = 0.0;
        for (int channels : { 1, 2, 3, 6, 8, 10, 12, 16 })
        {
            std::vector<std::vector<float>> x(static_cast<size_t>(channels), std::vector<float>(24000));
            Noise source;
            for (size_t i = 0; i < 24000; ++i)
            {
                const float v = source.next();
                for (auto& ch : x)
                    ch[i] = v;
            }
            OutputEq many;
            many.setParams(everyBandIn());
            many.prepare(fs, channels);
            for (size_t pos = 0; pos < 24000; pos += 512)
            {
                float* chans[OutputEq::c_maxChannels];
                for (int ch = 0; ch < channels; ++ch)
                    chans[ch] = x[static_cast<size_t>(ch)].data() + pos;
                many.process(chans, channels, static_cast<int>(std::min<size_t>(512, 24000 - pos)));
            }
            for (auto& ch : x)
                same = same && ch == x[0];
            if (reference.empty())
                reference = x[0];
            for (size_t i = 0; i < 24000; ++i)
                widest = std::max(widest, static_cast<double>(std::abs(x[0][i] - reference[i])));
        }
        check(same, "the same sound on every channel comes out the same on every channel, to the bit (1 to 16 channels)");
        std::cout << "         a channel at 1 width against the same at the others: at most " << num(db(widest), 1) << " dB apart" << std::endl;
        check(widest < 1.0e-6, "a channel does not depend on how many others there are (below -120 dB)");

        // No delay: an impulse comes out starting on the sample it went in on, in every band.
        std::vector<float> x(4800, 0.0f);
        x[100] = 1.0f;
        const auto y = through(everyBandIn(), fs, x);
        bool before = false;
        for (size_t i = 0; i < 100; ++i)
            before = before || y[i] != 0.0f;
        std::cout << "         an impulse at sample 100 with every band in: sample 100 comes out at " << num(y[100], 4) << std::endl;
        check(! before && std::abs(y[100]) > 0.1f, "no delay: the impulse comes out on the sample it went in on");
    }

    //==========================================================================
    std::cout << "The Low Cut's noise floor at 20 Hz and 192 kHz" << std::endl;
    {
        const double fs = 192000.0;
        OutputEq::Params p;
        p.lowCutHz = 20.0f;
        double worst = -300.0;
        for (int slope : { 12, 24 })
        {
            p.lowCutSlope = slope;
            for (double tone : { 30.0, 1000.0 })
            {
                const size_t n = static_cast<size_t>(fs * 6.0), from = static_cast<size_t>(fs * 4.0);
                std::vector<float> x(n);
                for (size_t i = 0; i < n; ++i)
                    x[i] = static_cast<float>(0.5 * std::sin(2.0 * c_pi * tone * static_cast<double>(i) / fs));
                // What should come out: the tone as the samples really hold it (its own rounding to 24 bits of
                // mantissa is not the filter's), scaled and turned by the analytic response.
                const Complex in = fourier(x, tone, fs, from) * 2.0 / static_cast<double>(n - from);
                const Complex h = analytic(p, tone, fs, OutputEq::LowCut);
                const auto y = through(p, fs, x);
                const Complex want = in * h;
                double err = 0.0, power = 0.0;
                for (size_t i = from; i < n; ++i)
                {
                    const double ideal = (want * std::polar(1.0, 2.0 * c_pi * tone * static_cast<double>(i - from) / fs)).real();
                    err += (y[i] - ideal) * (y[i] - ideal);
                    power += ideal * ideal;
                }
                const double floorDb = 10.0 * std::log10(std::max(err, 1.0e-300) / power) + db(std::abs(h)) + db(0.5 * std::sqrt(0.5));
                std::cout << "         " << slope << " dB an octave, a 0.5 tone at " << num(tone, 0) << " Hz: everything that is not the tone is at "
                          << num(floorDb, 1) << " dBFS (the tone comes out at " << num(db(std::abs(h)) + db(0.5 * std::sqrt(0.5)), 1) << " dBFS)" << std::endl;
                worst = std::max(worst, floorDb);
            }
        }
        check(worst < -120.0, "the Low Cut at 20 Hz and 192 kHz adds nothing above -120 dBFS");

        // And into silence: after a loud burst the filter dies away to nothing (no hum, no stuck value).
        std::vector<float> x(static_cast<size_t>(fs * 4.0), 0.0f);
        Noise r;
        for (size_t i = 0; i < 19200; ++i)
            x[i] = r.next();
        const auto y = through(p, fs, x);
        double tail = 0.0;
        for (size_t i = x.size() - 19200; i < x.size(); ++i)
            tail = std::max(tail, static_cast<double>(std::abs(y[i])));
        std::cout << "         3.9 s after a burst of noise: " << (tail > 0.0 ? num(db(tail), 1) + " dBFS" : std::string("exactly silent")) << std::endl;
        check(tail < 1.0e-9, "after the sound stops the Low Cut dies away below -180 dBFS");
    }

    //==========================================================================
    std::cout << "Block sizes: the sound does not depend on how the stream is cut up" << std::endl;
    {
        const double fs = 48000.0;
        const size_t n = 48000;
        // Settings that change every 512 samples (as the plug-in gives them), cuts coming and going.
        auto settingsAt = [](size_t tick) {
            auto p = everyBandIn();
            const double t = static_cast<double>(tick) / 93.75;
            p.lowMidHz = static_cast<float>(100.0 * std::pow(40.0, 0.5 + 0.5 * std::sin(7.0 * t)));
            p.highMidDb = static_cast<float>(12.0 * std::sin(11.0 * t));
            p.lowCutHz = (tick / 12) % 2 == 0 ? 0.0f : 150.0f;
            p.lowCutSlope = (tick / 20) % 2 == 0 ? 12 : 24;
            p.highCutHz = (tick / 17) % 2 == 0 ? 6000.0f : 0.0f;
            p.on = (tick / 31) % 3 != 2;
            return p;
        };
        auto render = [&](const std::function<int(size_t)>& blockAt) {
            std::vector<float> l(n), r(n);
            Noise source;
            for (size_t i = 0; i < n; ++i)
            {
                l[i] = source.next();
                r[i] = 0.3f * source.next();
            }
            OutputEq eq;
            eq.setParams(settingsAt(0));
            eq.prepare(fs, 2);
            size_t pos = 0, call = 0;
            while (pos < n)
            {
                // A block never crosses a 512-sample tick (the plug-in cuts its work there too).
                size_t len = static_cast<size_t>(std::max(1, blockAt(call++)));
                len = std::min(len, std::min(n - pos, 512 - pos % 512));
                if (pos % 512 == 0)
                    eq.setParams(settingsAt(pos / 512));
                float* chans[2] = { l.data() + pos, r.data() + pos };
                eq.process(chans, 2, static_cast<int>(len));
                pos += len;
            }
            l.insert(l.end(), r.begin(), r.end());
            return l;
        };
        const auto reference = render([](size_t) { return 512; });
        bool same = true;
        for (int size : { 1, 7, 32, 37, 64, 441 })
            same = same && render([size](size_t) { return size; }) == reference;
        std::uint32_t seed = 99u;
        same = same && render([&seed](size_t) {
                           seed = seed * 1664525u + 1013904223u;
                           return static_cast<int>(1 + (seed >> 16) % 300);
                       }) == reference;
        check(same, "blocks of 1, 7, 32, 37, 64, 441 and random sizes give the same samples as blocks of 512, to the bit");
    }

    //==========================================================================
    std::cout << "Speed" << std::endl;
    {
        auto percent = [](double fs, int channels, const OutputEq::Params& p, double seconds) {
            const int block = 512;
            std::vector<std::vector<float>> x(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(block)));
            std::vector<std::vector<float>> source = x;
            Noise r;
            for (auto& ch : source)
                for (auto& v : ch)
                    v = 0.25f * r.next();
            OutputEq eq;
            eq.setParams(p);
            eq.prepare(fs, channels);
            float* chans[OutputEq::c_maxChannels];
            for (int ch = 0; ch < channels; ++ch)
                chans[ch] = x[static_cast<size_t>(ch)].data();
            const int blocks = static_cast<int>(seconds * fs / block);
            double best = 1.0e9;
            for (int pass = 0; pass < 3; ++pass)
            {
                double used = 0.0;
                for (int b = 0; b < blocks; ++b)
                {
                    x = source; // (not timed: fresh sound each block)
                    const double t0 = TestClock::cpuMs();
                    eq.setParams(p);
                    eq.process(chans, channels, block);
                    used += TestClock::cpuMs() - t0;
                }
                best = std::min(best, used);
            }
            return 100.0 * best / (seconds * 1000.0);
        };
        const auto all = everyBandIn();
        const double wide192 = percent(192000.0, 12, all, 4.0), wide48 = percent(48000.0, 12, all, 4.0);
        const double stereo192 = percent(192000.0, 2, all, 4.0), stereo48 = percent(48000.0, 2, all, 4.0);
        OutputEq::Params one;
        one.lowMidDb = 3.0f;
        const double oneBand = percent(192000.0, 12, one, 4.0), flat = percent(192000.0, 12, OutputEq::Params(), 4.0);
        std::cout << "         every band in (seven sections): 7.1.4 at 192 kHz " << num(wide192, 2) << "% of a core, at 48 kHz " << num(wide48, 2)
                  << "%; Stereo at 192 kHz " << num(stereo192, 2) << "%, at 48 kHz " << num(stereo48, 2) << "%" << std::endl;
        std::cout << "         one bell in, 7.1.4 at 192 kHz: " << num(oneBand, 2) << "%; flat: " << num(flat, 3) << "% (the stopwatch itself included)" << std::endl;
        // (Under 0.5% on a desk machine; a shared build machine a third as fast measured 1.29%.)
        check(wide192 <= 2.0 || !TestClock::holdsCostLimits(), "7.1.4 at 192 kHz with every band in costs no more than 2% of a core");
        // (Flat it is only the stopwatch: held to a tenth of every band in, since a slower machine pays more for both.)
        check(flat < 0.1 * wide192 || !TestClock::holdsCostLimits(), "flat costs nothing to speak of (under a tenth of every band in)");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : std::to_string(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

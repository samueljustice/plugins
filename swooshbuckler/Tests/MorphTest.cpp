// Offline test for the spectral morph (SpectralMorph) and the way it is switched in between the buses (MorphInsert).
//
//   SwooshbucklerMorph
//
// Checks, at 48 kHz: the level holds across X in every mode (two noises of the same level); with inputs of
// different levels the output follows the mix of their powers, and a swell in one input comes through as a swell;
// Glide puts the partials of two sawtooths at the in-between frequencies; a hit in Glide has no pre-echo;
// switching the morph in and out fades against a sample-aligned copy of the dry sound (a null test against the
// delayed input), leaves the buses untouched while it is out, and leaves no old sound inside the morph.
// Prints how fast the morph runs at 48 and 192 kHz. Writes no files.

#include <juce_dsp/juce_dsp.h>
#include "TestClock.h"

#include "SpectralMorph.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
    int g_failures = 0;
    constexpr double c_twoPi = 6.283185307179586;
    constexpr double c_rate = 48000.0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }

    using Signal = std::vector<float>;

    size_t at(double seconds) { return static_cast<size_t>(c_rate * seconds); }

    Signal noise(double seconds, int seed, float rmsLevel)
    {
        juce::Random r(seed);
        Signal s(at(seconds));
        const float amp = rmsLevel * std::sqrt(3.0f); // uniform noise: RMS = amp / sqrt(3)
        for (auto& v : s)
            v = amp * (r.nextFloat() * 2.0f - 1.0f);
        return s;
    }

    Signal saw(double f0, double seconds, float amp)
    {
        Signal s(at(seconds), 0.0f);
        for (int h = 1; f0 * h <= 5000.0; ++h)
            for (size_t i = 0; i < s.size(); ++i)
                s[i] += amp * static_cast<float>(std::sin(c_twoPi * f0 * h * static_cast<double>(i) / c_rate) / h);
        return s;
    }

    float rms(const Signal& s, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += static_cast<double>(s[i]) * s[i];
        return static_cast<float>(std::sqrt(sum / static_cast<double>(std::max<size_t>(1, to - from))));
    }

    float db(float gain) { return juce::Decibels::gainToDecibels(gain, -200.0f); }

    // Amplitude of the component at hz over [from, to), Hann weighted.
    double component(const Signal& s, size_t from, size_t to, double hz)
    {
        const size_t len = to - from;
        const double step = c_twoPi * hz / c_rate;
        double c = 0.0, q = 0.0, sumW = 0.0;
        for (size_t i = 0; i < len; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(c_twoPi * static_cast<double>(i) / static_cast<double>(len));
            c += w * s[from + i] * std::cos(step * static_cast<double>(i));
            q += w * s[from + i] * std::sin(step * static_cast<double>(i));
            sumW += w;
        }
        return 2.0 * std::sqrt(c * c + q * q) / sumW;
    }

    Signal morph(const Signal& a, const Signal& b, int mode, float x, float y = 0.5f)
    {
        SpectralMorph m;
        m.prepare(c_rate, 32);
        SpectralMorph::Params p;
        p.mode = mode;
        p.x = x;
        p.y = y;
        m.setParams(p);
        m.reset();
        Signal out(a.size());
        for (size_t i = 0; i < a.size(); i += 32)
            m.process(a.data() + i, b.data() + i, out.data() + i, static_cast<int>(std::min<size_t>(32, a.size() - i)));
        return out;
    }

    //==============================================================================
    void testLevels()
    {
        std::cout << "\n== level across X ==" << std::endl;
        const Signal a = noise(5.0, 1, 0.1f), b = noise(5.0, 2, 0.1f);
        const Signal quiet = noise(5.0, 3, 0.01f); // 20 dB under a
        const size_t from = at(2.0), to = at(4.8);

        for (int mode = 0; mode < SpectralMorph::NumModes; ++mode)
        {
            const juce::String name = SpectralMorph::modeName(mode);
            float worst = 0.0f;
            juce::String line;
            for (float x : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                const float d = db(rms(morph(a, b, mode, x), from, to) / 0.1f);
                line << " " << juce::String(d, 2);
                worst = std::max(worst, std::abs(d));
            }
            std::cout << "  " << name << ", two noises at the same level, X 0 / 25 / 50 / 75 / 100 %:" << line << " dB" << std::endl;
            check(worst < 1.0f, name + ": the level holds within 1 dB across X");

            // Different levels: the output power is the mix of the two powers.
            float worstMix = 0.0f;
            juce::String line2;
            for (float x : { 0.25f, 0.5f, 0.75f })
            {
                const float want = 10.0f * std::log10((1.0f - x) * 0.01f + x * 0.0001f);
                const float got = db(rms(morph(a, quiet, mode, x), from, to));
                line2 << " " << juce::String(got, 1) << " (" << juce::String(want, 1) << ")";
                worstMix = std::max(worstMix, std::abs(got - want));
            }
            std::cout << "  " << name << ", A -20 dBFS and B -40 dBFS, X 25 / 50 / 75 %:" << line2 << " dBFS (the mix of their powers)" << std::endl;
            check(worstMix < 1.5f, name + ": inputs 20 dB apart come out at the mix of their powers (within 1.5 dB)");
        }

        // A swell in A over two quiet beds stays a swell at X 50 %.
        {
            Signal sa = noise(4.0, 4, 0.02f);
            const Signal sb = noise(4.0, 5, 0.02f);
            const Signal loud = noise(4.0, 6, 0.4f);
            for (size_t i = at(2.0); i < at(2.5); ++i)
                sa[i] = loud[i];
            const float inDb = db(rms(sa, at(2.1), at(2.4)));
            const size_t lat = 2048;
            for (int mode = 0; mode < SpectralMorph::NumModes; ++mode)
            {
                const Signal out = morph(sa, sb, mode, 0.5f);
                const float outDb = db(rms(out, at(2.1) + lat, at(2.4) + lat));
                std::cout << "  " << SpectralMorph::modeName(mode) << ": a swell of " << inDb << " dBFS in A (beds at -34 dBFS) comes out at "
                          << outDb << " dBFS at X 50 %" << std::endl;
                check(outDb > inDb - 4.5f && outDb < inDb, juce::String(SpectralMorph::modeName(mode)) + ": a swell in one input keeps its size (3 dB down at X 50 %, within 1.5 dB)");
            }
        }
    }

    //==============================================================================
    void testGlide()
    {
        std::cout << "\n== Glide ==" << std::endl;

        // Partials move: saw 110 Hz to saw 165 Hz at X 50 % (the glide is eased: a quarter of the way).
        {
            const Signal a = saw(110.0, 4.0, 0.2f), b = saw(165.0, 4.0, 0.2f);
            const Signal out = morph(a, b, SpectralMorph::Glide, 0.5f, 0.0f);
            const size_t from = at(1.5), to = at(3.5);
            for (int partial = 1; partial <= 3; ++partial)
            {
                const double want = partial * (110.0 + 0.25 * 55.0);
                double best = 0.0, bestHz = 0.0;
                for (double hz = partial * 100.0; hz <= partial * 175.0; hz += 0.25)
                {
                    const double c = component(out, from, to, hz);
                    if (c > best) { best = c; bestHz = hz; }
                }
                const double ends = std::max(component(out, from, to, partial * 110.0), component(out, from, to, partial * 165.0));
                std::cout << "  partial " << partial << " at " << bestHz << " Hz (want " << want << "), the inputs' own partials are "
                          << 20.0 * std::log10(std::max(1.0e-12, ends / best)) << " dB under it" << std::endl;
                check(std::abs(bestHz - want) < 2.0, "Glide: partial " + juce::String(partial) + " sits at the in-between frequency (within 2 Hz)");
                if (partial == 1) // higher up, one input's partial can be the other's too (330 Hz is both 3 x 110 and 2 x 165)
                    check(ends < 0.18 * best, "Glide: the inputs' own first partials are at least 15 dB under the moved one");
            }
        }

        // A hit in A over two beds: nothing of it ahead of the hit.
        {
            Signal a = noise(3.0, 7, 0.02f);
            const Signal b = noise(3.0, 8, 0.02f);
            const size_t onset = at(1.2345);
            juce::Random r(9);
            for (size_t i = 0; i < at(0.03); ++i)
                a[onset + i] += 0.8f * (r.nextFloat() * 2.0f - 1.0f) * static_cast<float>(std::exp(-static_cast<double>(i) / (c_rate * 0.008)));
            const size_t lat = 2048, t = onset + lat;
            // Near A the hit must be clean. Mid-X the frames that hold the hit also hold B's share of the morph,
            // brought up to the hit's level, so some of it is heard just ahead of the hit: bounded here.
            float near20 = -100.0f, near5 = -100.0f, worst20 = -100.0f, worst5 = -100.0f;
            for (float x : { 0.02f, 0.25f, 0.5f })
            {
                const Signal out = morph(a, b, SpectralMorph::Glide, x, 0.0f);
                const float bed = rms(out, t - at(0.6), t - at(0.1));
                const float pre20 = db(rms(out, t - at(0.02), t - at(0.001)) / bed);
                const float pre5 = db(rms(out, t - at(0.005), t - at(0.001)) / bed);
                const float hit = db(rms(out, t, t + at(0.005)) / rms(a, onset, onset + at(0.005)));
                std::cout << "  X " << 100.0f * x << " %: 20 ms before the hit " << pre20 << " dB over the bed, last 5 ms " << pre5
                          << " dB, first 5 ms of the hit " << hit << " dB of the source" << std::endl;
                worst20 = std::max(worst20, pre20);
                worst5 = std::max(worst5, pre5);
                if (x < 0.1f) { near20 = pre20; near5 = pre5; }
            }
            check(near20 < 2.0f && near5 < 2.0f, "Glide near A: the 20 ms and the last 5 ms before a hit stay within 2 dB of the bed");
            check(worst20 < 8.0f, "Glide up to X 50 %: the 20 ms before a hit stay within 8 dB of the bed");
            check(worst5 < 12.5f, "Glide up to X 50 %: the last 5 ms before it stay within 12.5 dB of the bed");
        }
    }

    //==============================================================================
    void testSwitch()
    {
        std::cout << "\n== switching the morph in and out ==" << std::endl;

        // Bus A: a tone plus noise (a tone shows a comb at once). Bus B: another noise. X 0 in Blend: the morph gives
        // bus A back as it went in, one latency late, so with an aligned dry side the switch is inaudible on bus A.
        const double seconds = 5.0;
        Signal a = noise(seconds, 11, 0.05f);
        const Signal b = noise(seconds, 12, 0.05f);
        for (size_t i = 0; i < a.size(); ++i)
            a[i] += 0.2f * static_cast<float>(std::sin(c_twoPi * 311.0 * static_cast<double>(i) / c_rate));

        MorphInsert insert;
        insert.prepare(c_rate, 256);
        SpectralMorph::Params p;
        p.mode = SpectralMorph::Blend;
        p.x = 0.0f;
        insert.setParams(p);
        insert.reset(false);
        const auto lat = static_cast<size_t>(insert.getLatencySamples());
        check(lat == 2048, "the insert reports the morph's latency (2048 at 48 kHz)");

        // The mix: out until 1 s, in over 0.3 s, held, out again from 3 s over 0.3 s.
        auto mixAt = [](double t)
        {
            if (t < 1.0) return 0.0f;
            if (t < 1.3) return static_cast<float>(0.5 - 0.5 * std::cos(juce::MathConstants<double>::pi * (t - 1.0) / 0.3));
            if (t < 3.0) return 1.0f;
            if (t < 3.3) return static_cast<float>(0.5 + 0.5 * std::cos(juce::MathConstants<double>::pi * (t - 3.0) / 0.3));
            return 0.0f;
        };

        Signal outA = a, outB = b, oldA(a.size(), 0.0f);
        SpectralMorph plain; // what a fade against the live sound gives (for comparison)
        plain.prepare(c_rate, 256);
        plain.setParams(p);
        plain.reset();
        Signal wet(256);
        bool runningWhileOut = false;
        for (size_t i = 0; i < a.size(); i += 256)
        {
            const int n = static_cast<int>(std::min<size_t>(256, a.size() - i));
            const float mix = mixAt(static_cast<double>(i + static_cast<size_t>(n)) / c_rate);
            insert.process(outA.data() + i, outB.data() + i, n, mix);
            if (static_cast<double>(i) / c_rate > 3.5 && insert.isRunning())
                runningWhileOut = true;

            plain.process(a.data() + i, b.data() + i, wet.data(), n);
            for (int k = 0; k < n; ++k)
                oldA[i + static_cast<size_t>(k)] = a[i + static_cast<size_t>(k)] + mix * (wet[static_cast<size_t>(k)] - a[i + static_cast<size_t>(k)]);
        }

        auto residual = [&](const Signal& out, size_t from, size_t to, size_t delay)
        {
            double err = 0.0, ref = 0.0;
            for (size_t i = from; i < to; ++i)
            {
                const double want = a[i - delay];
                err += (out[i] - want) * (out[i] - want);
                ref += want * want;
            }
            return static_cast<float>(10.0 * std::log10(std::max(1.0e-20, err / ref)));
        };

        float liveErr = 0.0f;
        for (size_t i = 0; i < at(1.0) - 256; ++i)
            liveErr = std::max(liveErr, std::abs(outA[i] - a[i]) + std::abs(outB[i] - b[i]));
        for (size_t i = at(3.5); i < a.size(); ++i)
            liveErr = std::max(liveErr, std::abs(outA[i] - a[i]) + std::abs(outB[i] - b[i]));
        check(liveErr == 0.0f, "while the morph is out, both buses pass untouched (bit exact, no delay)");
        check(! runningWhileOut, "the morph is not run while it is out");

        // From the end of the 10 ms splice to the start of the splice back, bus A is the input one latency late.
        const float fadeIn = residual(outA, at(1.02), at(1.3), lat);
        const float held = residual(outA, at(1.3), at(3.0), lat);
        const float fadeOut = residual(outA, at(3.0), at(3.28), lat);
        const float oldIn = residual(oldA, at(1.02), at(1.3), lat);
        const float oldInLive = residual(oldA, at(1.02), at(1.3), 0);
        std::cout << "  bus A against its input one latency late: " << fadeIn << " dB while fading in, " << held << " dB held, "
                  << fadeOut << " dB while fading out" << std::endl;
        std::cout << "  (a fade against the live sound instead: " << oldIn << " dB against the late input, " << oldInLive
                  << " dB against the live one: two copies 43 ms apart)" << std::endl;
        check(fadeIn < -25.0f && fadeOut < -25.0f, "the fades null against the delayed input (under -25 dB): no doubled sound");
        check(held < -25.0f, "fully in at X 0 in Blend, bus A is its input one latency late");
        check(db(rms(outB, at(1.5), at(2.9)) / 0.05f) < -60.0f, "fully in at X 0, bus B gets nothing");

        // The splices are short and hold the level (two moments of the same sound, equal power).
        const float spliceIn = db(rms(outA, at(1.0), at(1.01)) / rms(a, at(1.0), at(1.01)));
        const float spliceOut = db(rms(outA, at(3.3), at(3.32)) / rms(a, at(3.3), at(3.32)));
        std::cout << "  level through the splices: " << spliceIn << " dB going in, " << spliceOut << " dB coming out" << std::endl;
        check(std::abs(spliceIn) < 3.5f && std::abs(spliceOut) < 3.5f, "the 10 ms splices onto and off the delayed copy hold the level (within 3.5 dB)");
        float step = 0.0f;
        for (size_t i = at(0.9); i < at(3.5); ++i)
            step = std::max(step, std::abs(outA[i] - outA[i - 1]));
        float inStep = 0.0f;
        for (size_t i = 1; i < a.size(); ++i)
            inStep = std::max(inStep, std::abs(a[i] - a[i - 1]));
        check(step < 1.5f * inStep, "no click anywhere in the switch (largest sample step " + juce::String(step, 3) + ", the input's own " + juce::String(inStep, 3) + ")");

        // Switched in again while the splice back to the live sound is still under way (5 ms after the mix reached
        // 0): the morph must still be lined up with the delayed copy, not a few ms behind it.
        {
            MorphInsert quick;
            quick.prepare(c_rate, 256);
            quick.setParams(p);
            quick.reset(true);
            Signal qa = a, qb = b;
            const size_t offAt = at(1.0) / 256 * 256, onAt = offAt + 512;
            for (size_t i = 0; i + 256 <= at(2.0); i += 256)
                quick.process(qa.data() + i, qb.data() + i, 256, (i >= offAt && i < onAt) ? 0.0f : 1.0f);
            const float again = residual(qa, onAt + at(0.012), onAt + lat, lat);
            std::cout << "  switched in again 5 ms after switching out: bus A against its input one latency late " << again << " dB" << std::endl;
            check(again < -25.0f, "switched in again during the splice back, the morph is still lined up (under -25 dB)");
        }

        // Nothing old comes back: loud sound while the morph is in, then out, silence, and in again.
        {
            MorphInsert again;
            again.prepare(c_rate, 256);
            p.x = 0.5f;
            again.setParams(p);
            again.reset(true);
            const Signal loudA = noise(1.0, 13, 0.3f), loudB = noise(1.0, 14, 0.3f);
            Signal bufA(256), bufB(256);
            auto runFor = [&](double secs, const Signal* srcA, const Signal* srcB, float mix, float& peak)
            {
                for (size_t i = 0; i + 256 <= at(secs); i += 256)
                {
                    for (size_t k = 0; k < 256; ++k)
                    {
                        bufA[k] = srcA != nullptr ? (*srcA)[i + k] : 0.0f;
                        bufB[k] = srcB != nullptr ? (*srcB)[i + k] : 0.0f;
                    }
                    again.process(bufA.data(), bufB.data(), 256, mix);
                    for (size_t k = 0; k < 256; ++k)
                        peak = std::max({ peak, std::abs(bufA[k]), std::abs(bufB[k]) });
                }
            };
            float ignore = 0.0f, peak = 0.0f;
            runFor(1.0, &loudA, &loudB, 1.0f, ignore);  // in, loud
            runFor(0.2, &loudA, &loudB, 0.0f, ignore);  // switched out while still loud
            runFor(0.5, nullptr, nullptr, 0.0f, ignore); // silence
            runFor(0.5, nullptr, nullptr, 1.0f, peak);  // in again, on silence
            std::cout << "  switched in again on silence: peak " << peak << std::endl;
            check(peak < 1.0e-6f, "switching the morph in again brings back nothing from the last time it was in");
        }
    }

    //==============================================================================
    void testSpeed()
    {
        std::cout << "\n== speed ==" << std::endl;
        for (double rate : { 48000.0, 192000.0 })
        {
            const double seconds = 10.0;
            juce::Random r(21);
            Signal a(static_cast<size_t>(rate * seconds)), b(a.size());
            for (size_t i = 0; i < a.size(); ++i)
            {
                a[i] = 0.2f * (r.nextFloat() * 2.0f - 1.0f) + 0.2f * static_cast<float>(std::sin(c_twoPi * 110.0 * static_cast<double>(i) / rate));
                b[i] = 0.2f * (r.nextFloat() * 2.0f - 1.0f);
            }
            for (int mode = 0; mode < SpectralMorph::NumModes; ++mode)
            {
                MorphInsert m;
                m.prepare(rate, 64);
                SpectralMorph::Params p;
                p.mode = mode;
                p.x = 0.4f;
                p.y = 0.6f;
                m.setParams(p);
                m.reset(true);
                Signal wa(64), wb(64);
                const double t0 = TestClock::cpuMs();
                for (size_t i = 0; i + 64 <= a.size(); i += 64)
                {
                    std::copy(a.begin() + static_cast<long>(i), a.begin() + static_cast<long>(i + 64), wa.begin());
                    std::copy(b.begin() + static_cast<long>(i), b.begin() + static_cast<long>(i + 64), wb.begin());
                    m.process(wa.data(), wb.data(), 64, 1.0f);
                }
                const double secs = (TestClock::cpuMs() - t0) * 0.001;
                const double factor = seconds / secs;
                std::cout << "  " << SpectralMorph::modeName(mode) << " at " << rate / 1000.0 << " kHz (frame " << m.getLatencySamples()
                          << "): " << factor << "x real time (" << 100.0 / factor << " % of one core)" << std::endl;
                check(factor > 10.0, juce::String(SpectralMorph::modeName(mode)) + " at " + juce::String(rate / 1000.0, 0) + " kHz runs faster than 10x real time");
            }
        }
    }
}

int main()
{
    testLevels();
    testGlide();
    testSwitch();
    testSpeed();

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

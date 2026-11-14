// Classifier test for Swooshbuckler.
//
//   SwooshbucklerClassify
//
// Synthesises one clip per role (noise bed, pitched chirps with silences, short noise hit, single noise swell,
// sparse clicks, 40 Hz rumble), runs CargoData::detectRegions + SampleClassifier::analyse, prints the features,
// and checks: >= 5 of 6 roles right, impact focus on the transient, file-name labels, file-name prior,
// and the speed budget (< 150 ms for 10 s mono 48 kHz). Returns non-zero on failure.

#include <JuceHeader.h>
#include "TestClock.h"
#include "Cargo.h"
#include "SampleClassifier.h"

#include <cstdio>
#include <functional>
#include <iostream>

namespace
{
    constexpr double c_rate = 48000.0;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }

    struct Clip
    {
        juce::String name;
        CargoRole expected;
        std::vector<float> audio;
    };

    std::vector<float> silence(double seconds) { return std::vector<float>(static_cast<size_t>(seconds * c_rate), 0.0f); }

    // Simple one-pole low-pass for colouring noise.
    struct OnePole
    {
        float a = 0.0f, z = 0.0f;
        explicit OnePole(double cutoff) : a(static_cast<float>(std::exp(-juce::MathConstants<double>::twoPi * cutoff / c_rate))) {}
        float operator()(float x) { z = x + a * (z - x); return z; }
    };

    Clip makeBed(juce::Random& rng)
    {
        // 8 s of steady, slightly coloured noise (wind / room tone), 50 ms fades.
        Clip c { "noise bed", CargoRole::Bed, silence(8.0) };
        OnePole lp(3000.0);
        const int n = static_cast<int>(c.audio.size()), fade = static_cast<int>(0.05 * c_rate);
        for (int i = 0; i < n; ++i)
        {
            const float g = juce::jmin(1.0f, static_cast<float>(juce::jmin(i, n - 1 - i)) / static_cast<float>(fade));
            const float wobble = 1.0f + 0.1f * std::sin(static_cast<float>(i / c_rate) * 1.3f);
            c.audio[static_cast<size_t>(i)] = 0.3f * g * wobble * lp(rng.nextFloat() * 2.0f - 1.0f);
        }
        return c;
    }

    Clip makeTonal()
    {
        // Four pitched chirps (450 -> 900 Hz glide, a few harmonics), 0.45 s each, 0.5 s silence between.
        Clip c { "pitched chirps", CargoRole::Tonal, silence(0.3) };
        for (int k = 0; k < 4; ++k)
        {
            const int len = static_cast<int>(0.45 * c_rate);
            double phase = 0.0;
            for (int i = 0; i < len; ++i)
            {
                const double t = i / static_cast<double>(len);
                const double f = 450.0 * std::pow(2.0, t) * (1.0 + 0.1 * k);
                phase += juce::MathConstants<double>::twoPi * f / c_rate;
                const float env = static_cast<float>(std::sin(juce::MathConstants<double>::pi * t));
                const float v = static_cast<float>(std::sin(phase) + 0.5 * std::sin(2 * phase) + 0.25 * std::sin(3 * phase));
                c.audio.push_back(0.4f * env * v);
            }
            auto gap = silence(0.5);
            c.audio.insert(c.audio.end(), gap.begin(), gap.end());
        }
        return c;
    }

    Clip makeImpact(juce::Random& rng)
    {
        // 0.3 s pre-silence, then a noise burst with a 1 ms attack and an 80 ms exponential decay.
        Clip c { "noise hit", CargoRole::Impact, silence(0.3) };
        const int len = static_cast<int>(0.9 * c_rate);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / c_rate;
            const float env = static_cast<float>(juce::jmin(1.0, t / 0.001) * std::exp(-t / 0.08));
            c.audio.push_back(0.9f * env * (rng.nextFloat() * 2.0f - 1.0f));
        }
        auto tail = silence(0.8);
        c.audio.insert(c.audio.end(), tail.begin(), tail.end());
        return c;
    }

    Clip makeAir(juce::Random& rng)
    {
        // One smooth 1.4 s noise swell (raised-sine envelope, peak at 60 %), silence around it.
        Clip c { "noise swell", CargoRole::Air, silence(0.4) };
        const int len = static_cast<int>(1.4 * c_rate);
        OnePole lp(6000.0);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / static_cast<double>(len);
            const double shaped = t < 0.6 ? t / 0.6 * 0.5 : 0.5 + (t - 0.6) / 0.4 * 0.5; // peak at 60 %
            const float env = static_cast<float>(std::pow(std::sin(juce::MathConstants<double>::pi * shaped), 2.0));
            c.audio.push_back(0.6f * env * lp(rng.nextFloat() * 2.0f - 1.0f));
        }
        auto tail = silence(0.6);
        c.audio.insert(c.audio.end(), tail.begin(), tail.end());
        return c;
    }

    Clip makeCrackle(juce::Random& rng)
    {
        // 4 s of sparse clicks: ~25 per second, random level, 3 ms decaying noise grains.
        Clip c { "sparse clicks", CargoRole::Crackle, silence(4.0) };
        const int n = static_cast<int>(c.audio.size());
        double t = 0.05;
        while (t < 3.9)
        {
            const int start = static_cast<int>(t * c_rate);
            const float amp = 0.2f + 0.7f * rng.nextFloat();
            for (int i = 0; i < static_cast<int>(0.012 * c_rate) && start + i < n; ++i)
                c.audio[static_cast<size_t>(start + i)] += amp * static_cast<float>(std::exp(-i / (0.003 * c_rate)))
                                                           * (rng.nextFloat() * 2.0f - 1.0f);
            t += -std::log(juce::jmax(1.0e-4f, rng.nextFloat())) / 25.0;
        }
        return c;
    }

    Clip makeRumble(juce::Random& rng)
    {
        // 6 s of 40 Hz + 55 Hz with low-passed noise, slow fades, slow level motion.
        Clip c { "40 Hz rumble", CargoRole::Rumble, silence(6.0) };
        OnePole lp1(90.0), lp2(90.0);
        const int n = static_cast<int>(c.audio.size());
        for (int i = 0; i < n; ++i)
        {
            const double t = i / c_rate;
            const float env = static_cast<float>(juce::jmin(1.0, juce::jmin(t, 6.0 - t) / 0.5) * (0.8 + 0.2 * std::sin(t * 2.1)));
            const float tone = static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 40.0 * t)
                                                  + 0.4 * std::sin(juce::MathConstants<double>::twoPi * 55.0 * t));
            const float noise = 3.0f * lp2(lp1(rng.nextFloat() * 2.0f - 1.0f));
            c.audio[static_cast<size_t>(i)] = 0.4f * env * (tone + noise);
        }
        return c;
    }

    const char* fmt(float v, const char* f)
    {
        static char buf[16][32];
        static int slot = 0;
        slot = (slot + 1) % 16;
        std::snprintf(buf[slot], sizeof(buf[slot]), f, static_cast<double>(v));
        return buf[slot];
    }
}

int main(int, char*[])
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::Random rng(1234);

    std::vector<Clip> clips;
    clips.push_back(makeBed(rng));
    clips.push_back(makeTonal());
    clips.push_back(makeImpact(rng));
    clips.push_back(makeAir(rng));
    clips.push_back(makeCrackle(rng));
    clips.push_back(makeRumble(rng));

    std::cout << "Role detection (no file names)\n";
    std::printf("  %-15s %-8s %-8s %5s %5s %6s %6s %7s %5s %5s %6s %6s %4s %s\n",
                "clip", "expect", "got", "sound", "sust", "flat", "centr", "low", "clar", "onset", "crest", "attk", "conf", "label");

    int correct = 0;
    for (auto& c : clips)
    {
        const auto regions = CargoData::detectRegions(c.audio.data(), static_cast<int>(c.audio.size()), c_rate);
        const auto a = SampleClassifier::analyse(c.audio.data(), static_cast<int>(c.audio.size()), c_rate, regions, {});
        const bool ok = a.role == c.expected;
        if (ok) ++correct;
        std::printf("  %-15s %-8s %-8s %5s %5s %6s %6s %7s %5s %5s %6s %6s %4s %s%s (%d regions)\n",
                    c.name.toRawUTF8(), SampleClassifier::roleName(c.expected).toRawUTF8(),
                    SampleClassifier::roleName(a.role).toRawUTF8(),
                    fmt(a.soundSeconds, "%.2f"), fmt(a.sustainRatio, "%.2f"), fmt(a.flatness, "%.3f"),
                    fmt(a.centroidHz, "%.0f"), fmt(a.lowRatio, "%.3f"), fmt(a.pitchClarity, "%.2f"),
                    fmt(a.onsetRate, "%.1f"), fmt(a.crest, "%.1f"), fmt(a.attackMs, "%.1f"),
                    fmt(a.confidence, "%.2f"), a.label.toRawUTF8(), ok ? "" : "  <-- wrong", static_cast<int>(regions.size()));
    }
    check(correct >= 5, "roles correct: " + juce::String(correct) + " / 6 (need >= 5)");

    // Focus point: the impact's focus is its transient (the burst starts at 0.3 s).
    {
        auto& c = clips[2];
        auto regions = CargoData::detectRegions(c.audio.data(), static_cast<int>(c.audio.size()), c_rate);
        SampleClassifier::findFocusPoints(c.audio.data(), static_cast<int>(c.audio.size()), c_rate, regions);
        const int onset = static_cast<int>(0.3 * c_rate);
        const bool ok = ! regions.empty() && std::abs(regions[0].focus - onset) < static_cast<int>(0.005 * c_rate);
        check(ok, "impact focus on the transient (" + (regions.empty() ? juce::String("no region")
                  : juce::String((regions[0].focus - onset) * 1000.0 / c_rate, 2) + " ms from onset") + ")");
    }
    // Focus point: the swell's focus is near its loudest moment (0.4 + 0.84 s).
    {
        auto& c = clips[3];
        auto regions = CargoData::detectRegions(c.audio.data(), static_cast<int>(c.audio.size()), c_rate);
        SampleClassifier::findFocusPoints(c.audio.data(), static_cast<int>(c.audio.size()), c_rate, regions);
        const double expected = 0.4 + 0.6 * 1.4;
        const bool ok = ! regions.empty() && std::abs(regions[0].focus / c_rate - expected) < 0.12;
        check(ok, "swell focus near its peak (" + (regions.empty() ? juce::String("no region")
                  : juce::String(regions[0].focus / c_rate - expected, 3) + " s off)"));
        for (const auto& r : regions)
            check(r.focus >= r.start && r.focus < r.end, "focus inside its region");
    }

    // File-name labels
    std::cout << "File-name labels\n";
    const std::pair<const char*, const char*> labels[] = {
        { "ANMLFarm_PIG_Squeal_03_v2", "Pig Squeal" },
        { "PIG_Squeal_03_v2", "Pig Squeal" },
        { "WHSHSwish_Cloth Flap Fast_SJ_SBK", "Cloth Flap Fast" },
        { "MetalHit_take3", "Metal Hit" },
        { "wind-howl-01", "Wind Howl" },
        { "fire crackle 48k 24b", "Fire Crackle" },
        { "IMPCT_Big_Boom_v12", "Big Boom" },
        { "Rumble.wav", "Rumble" },
        { "", "" },
        { "01", "" },
    };
    for (const auto& [in, want] : labels)
    {
        const auto got = SampleClassifier::labelFromFileName(in);
        check(got == want, "\"" + juce::String(in) + "\" -> \"" + got + "\" (want \"" + juce::String(want) + "\")");
    }

    // analyse().label: file name if meaningful, else a role default
    {
        auto& c = clips[0];
        const auto regions = CargoData::detectRegions(c.audio.data(), static_cast<int>(c.audio.size()), c_rate);
        const auto named = SampleClassifier::analyse(c.audio.data(), static_cast<int>(c.audio.size()), c_rate, regions, "AMB_Forest_Night_01.wav");
        check(named.label == "Forest Night", "named bed label \"" + named.label + "\"");
        const auto unnamed = SampleClassifier::analyse(c.audio.data(), static_cast<int>(c.audio.size()), c_rate, regions, "Audio 1");
        check(unnamed.label == "Wind Bed", "unnamed bed label \"" + unnamed.label + "\"");
    }

    // File-name prior decides ambiguous audio (quiet, short, slightly tonal noise), but doesn't override clear audio.
    {
        std::vector<float> amb(static_cast<size_t>(0.25 * c_rate));
        for (size_t i = 0; i < amb.size(); ++i)
            amb[i] = 0.2f * (rng.nextFloat() * 2.0f - 1.0f) * static_cast<float>(std::sin(juce::MathConstants<double>::pi * static_cast<double>(i) / static_cast<double>(amb.size())));
        const auto regions = CargoData::detectRegions(amb.data(), static_cast<int>(amb.size()), c_rate);
        const auto a = SampleClassifier::analyse(amb.data(), static_cast<int>(amb.size()), c_rate, regions, "ANMLFarm_Pig Squeal Close_SJ");
        check(a.role == CargoRole::Tonal, "ambiguous audio + pig file name -> " + SampleClassifier::roleName(a.role)
                                          + ", label \"" + a.label + "\"");

        auto& bed = clips[0];
        const auto bedRegions = CargoData::detectRegions(bed.audio.data(), static_cast<int>(bed.audio.size()), c_rate);
        const auto b = SampleClassifier::analyse(bed.audio.data(), static_cast<int>(bed.audio.size()), c_rate, bedRegions, "Whoosh_01");
        check(b.role == CargoRole::Bed, "clear bed audio named \"Whoosh\" stays " + SampleClassifier::roleName(b.role));
    }

    // Speed: 10 s mono 48 kHz noise with gaps
    {
        std::vector<float> ten(static_cast<size_t>(10.0 * c_rate));
        for (size_t i = 0; i < ten.size(); ++i)
            ten[i] = ((i / 24000) % 3 == 2) ? 0.0f : 0.3f * (rng.nextFloat() * 2.0f - 1.0f);
        const auto t0 = TestClock::cpuMs();
        auto regions = CargoData::detectRegions(ten.data(), static_cast<int>(ten.size()), c_rate);
        const auto a = SampleClassifier::analyse(ten.data(), static_cast<int>(ten.size()), c_rate, regions, "test");
        SampleClassifier::findFocusPoints(ten.data(), static_cast<int>(ten.size()), c_rate, regions);
        const double ms = TestClock::cpuMs() - t0;
        juce::ignoreUnused(a);
       #if JUCE_DEBUG
        check(true, "10 s analysis took " + juce::String(ms, 1) + " ms (debug build, budget 150 ms not enforced)");
       #else
        check(ms < 150.0, "10 s analysis took " + juce::String(ms, 1) + " ms (budget 150 ms)");
       #endif
    }

    // Edge cases: nothing, silence, shorter than one analysis frame, unusual sample rates. No crash, a valid role.
    {
        auto valid = [](const SampleAnalysis& a)
        {
            const int r = static_cast<int>(a.role);
            return r >= 0 && r < c_numCargoRoles && std::isfinite(a.confidence) && a.confidence >= 0.0f && a.confidence <= 1.0f;
        };
        bool ok = true;
        ok = ok && valid(SampleClassifier::analyse(nullptr, 0, c_rate, {}, {}));
        std::vector<float> silence(static_cast<size_t>(c_rate), 0.0f);
        ok = ok && valid(SampleClassifier::analyse(silence.data(), static_cast<int>(silence.size()), c_rate,
                                                   CargoData::detectRegions(silence.data(), static_cast<int>(silence.size()), c_rate), {}));
        std::vector<float> tiny(500);
        for (size_t i = 0; i < tiny.size(); ++i)
            tiny[i] = 0.5f * static_cast<float>(std::sin(0.3 * static_cast<double>(i)));
        ok = ok && valid(SampleClassifier::analyse(tiny.data(), static_cast<int>(tiny.size()), c_rate,
                                                   CargoData::detectRegions(tiny.data(), static_cast<int>(tiny.size()), c_rate), {}));
        for (double rate : { 8000.0, 384000.0 })
        {
            std::vector<float> clip(static_cast<size_t>(rate * 0.8));
            juce::Random r(9);
            for (size_t i = 0; i < clip.size(); ++i)
                clip[i] = 0.4f * (r.nextFloat() * 2.0f - 1.0f) * std::sin(juce::MathConstants<float>::pi * static_cast<float>(i) / static_cast<float>(clip.size()));
            auto regions = CargoData::detectRegions(clip.data(), static_cast<int>(clip.size()), rate);
            SampleClassifier::findFocusPoints(clip.data(), static_cast<int>(clip.size()), rate, regions);
            ok = ok && valid(SampleClassifier::analyse(clip.data(), static_cast<int>(clip.size()), rate, regions, {}));
        }
        check(ok, "edge cases: empty, silent, tiny, 8 kHz and 384 kHz clips classify safely");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILURE(S)") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

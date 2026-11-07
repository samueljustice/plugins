// Offline test for SpectralMorph: renders test pairs through every mode, writes WAVs and checks
//  - finite output
//  - x = 0 ~ A and x = 1 ~ B (linear long-term spectral correlation > 0.9)
//  - level at x = 0.25 / 0.5 / 0.75 and along a slow sweep within 3 dB of the endpoints
//  - Glide puts the energy between 300 and 600 Hz at x = 0.5 (a glide, not a two-tone crossfade)
//  - no clicks while x / y / mode jump around (2nd-difference peak vs the static renders)
//  - silence in either input gives the other input
//  - 44.1 / 96 / 192 kHz, realtime factor
//
// Usage: MorphTest [outputFolder]

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "DspUtils.h"
#include "SpectralMorph.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <vector>

namespace
{
    using Signal = std::vector<float>;

    int g_failures = 0;
    juce::File g_folder;

    void check(bool ok, const juce::String& what)
    {
        std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what.toRawUTF8());
        if (! ok)
            ++g_failures;
    }

    void normalise(Signal& s, float rmsDb)
    {
        double sum = 0.0;
        for (float v : s) sum += static_cast<double>(v) * v;
        const float rms = static_cast<float>(std::sqrt(sum / static_cast<double>(s.size())));
        const float g = rms > 0.0f ? sj::dbToGain(rmsDb) / rms : 0.0f;
        for (float& v : s) v *= g;
    }

    //==========================================================================
    // Test signals
    Signal saw(double fs, double secs, float freq)
    {
        Signal s(static_cast<size_t>(fs * secs));
        sj::Saw osc;
        for (auto& v : s) v = osc.next(freq, static_cast<float>(fs));
        normalise(s, -20.0f);
        return s;
    }

    Signal filteredNoise(double fs, double secs)
    {
        Signal s(static_cast<size_t>(fs * secs));
        sj::Random rng;
        rng.seed(1234);
        sj::Svf bp, lp;
        bp.set(1500.0f, 0.8f, static_cast<float>(fs));
        lp.set(5000.0f, 0.7f, static_cast<float>(fs));
        for (auto& v : s) v = lp.lowpass(bp.bandpass(rng.bipolar()));
        normalise(s, -20.0f);
        return s;
    }

    Signal sine(double fs, double secs, float freq)
    {
        Signal s(static_cast<size_t>(fs * secs));
        for (size_t i = 0; i < s.size(); ++i)
            s[i] = std::sin(sj::c_twoPi * freq * static_cast<float>(static_cast<double>(i) / fs));
        normalise(s, -20.0f);
        return s;
    }

    // Additive glottal-ish pulse train at 140 Hz (light vibrato) with three formants.
    Signal voice(double fs, double secs)
    {
        Signal s(static_cast<size_t>(fs * secs));
        const float formants[3] = { 700.0f, 1220.0f, 2600.0f }, widths[3] = { 130.0f, 170.0f, 250.0f }, gains[3] = { 1.0f, 0.6f, 0.25f };
        double phase = 0.0;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double t = static_cast<double>(i) / fs;
            const double f0 = 140.0 * (1.0 + 0.01 * std::sin(sj::c_twoPi * 5.0 * t));
            phase += f0 / fs;
            float v = 0.0f;
            for (int h = 1; h * f0 < std::min(8000.0, fs * 0.45); ++h)
            {
                const float fh = static_cast<float>(h * f0);
                float a = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float d = (fh - formants[k]) / widths[k];
                    a += gains[k] * std::exp(-0.5f * d * d);
                }
                v += (a + 0.01f) / static_cast<float>(h) * std::sin(static_cast<float>(sj::c_twoPi * std::fmod(phase * h, 1.0)));
            }
            s[i] = v;
        }
        normalise(s, -20.0f);
        return s;
    }

    Signal chord(double fs, double secs)
    {
        Signal s(static_cast<size_t>(fs * secs));
        sj::Saw o1, o2, o3;
        sj::Svf lp;
        lp.set(3000.0f, 0.7f, static_cast<float>(fs));
        const auto f = static_cast<float>(fs);
        for (auto& v : s) v = lp.lowpass(o1.next(261.63f, f) + o2.next(329.63f, f) + o3.next(392.0f, f));
        normalise(s, -20.0f);
        return s;
    }

    struct Pair
    {
        const char* name;
        Signal a, b;
    };

    //==========================================================================
    // Rendering
    using Curve = std::function<float(double /*seconds*/)>;

    Signal render(double fs, const Signal* a, const Signal* b, size_t length, int mode, const Curve& x, const Curve& y,
                  const std::function<int(double)>& modeCurve = {})
    {
        SpectralMorph m;
        constexpr int block = 256;
        m.prepare(fs, block);
        Signal out(length, 0.0f);
        for (size_t pos = 0; pos < length; pos += block)
        {
            const int n = static_cast<int>(std::min<size_t>(block, length - pos));
            const double t = static_cast<double>(pos) / fs;
            SpectralMorph::Params p;
            p.x = x(t);
            p.y = y(t);
            p.mode = modeCurve ? modeCurve(t) : mode;
            m.setParams(p);
            m.process(a != nullptr ? a->data() + pos : nullptr, b != nullptr ? b->data() + pos : nullptr, out.data() + pos, n);
        }
        return out;
    }

    Curve constant(float v) { return [v](double) { return v; }; }

    void writeWav(const juce::String& name, const Signal& s, double fs)
    {
        auto file = g_folder.getChildFile(name + ".wav");
        file.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(file);
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream, fs, 1, 24, {}, 0));
        if (writer == nullptr) { delete stream; return; }
        const float* chans[1] = { s.data() };
        writer->writeFromFloatArrays(chans, 1, static_cast<int>(s.size()));
    }

    //==========================================================================
    // Analysis
    bool allFinite(const Signal& s)
    {
        for (float v : s) if (! std::isfinite(v)) return false;
        return true;
    }

    float rmsDb(const Signal& s, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i) sum += static_cast<double>(s[i]) * s[i];
        return static_cast<float>(10.0 * std::log10(sum / static_cast<double>(std::max<size_t>(1, to - from)) + 1.0e-20));
    }

    // Welch average magnitude spectrum, 4096-point Hann (at the test rate).
    std::vector<float> spectrum(const Signal& s, size_t from, size_t to)
    {
        constexpr int order = 12, n = 1 << order;
        juce::dsp::FFT fft(order);
        std::vector<float> acc(n / 2 + 1, 0.0f), buf(2 * n);
        int frames = 0;
        for (size_t p = from; p + n <= to; p += n / 4, ++frames)
        {
            for (int i = 0; i < n; ++i)
                buf[static_cast<size_t>(i)] = s[p + static_cast<size_t>(i)] * (0.5f - 0.5f * std::cos(sj::c_twoPi * static_cast<float>(i) / n));
            std::fill(buf.begin() + n, buf.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform(buf.data(), true);
            for (int k = 0; k <= n / 2; ++k) acc[static_cast<size_t>(k)] += buf[static_cast<size_t>(k)];
        }
        for (auto& v : acc) v /= static_cast<float>(std::max(1, frames));
        return acc;
    }

    float binHz(double fs) { return static_cast<float>(fs / 4096.0); }

    float correlation(const std::vector<float>& p, const std::vector<float>& q, double fs, float lo = 40.0f, float hi = 12000.0f)
    {
        const int k0 = static_cast<int>(lo / binHz(fs)), k1 = std::min(static_cast<int>(p.size()) - 1, static_cast<int>(hi / binHz(fs)));
        double mp = 0, mq = 0;
        const int cnt = k1 - k0 + 1;
        for (int k = k0; k <= k1; ++k) { mp += p[static_cast<size_t>(k)]; mq += q[static_cast<size_t>(k)]; }
        mp /= cnt; mq /= cnt;
        double sxy = 0, sxx = 0, syy = 0;
        for (int k = k0; k <= k1; ++k)
        {
            const double dx = p[static_cast<size_t>(k)] - mp, dy = q[static_cast<size_t>(k)] - mq;
            sxy += dx * dy; sxx += dx * dx; syy += dy * dy;
        }
        return static_cast<float>(sxy / std::sqrt(sxx * syy + 1.0e-30));
    }

    float bandPower(const std::vector<float>& p, double fs, float lo, float hi)
    {
        double sum = 0;
        for (int k = static_cast<int>(lo / binHz(fs)); k <= static_cast<int>(hi / binHz(fs)); ++k)
            sum += static_cast<double>(p[static_cast<size_t>(k)]) * p[static_cast<size_t>(k)];
        return static_cast<float>(sum);
    }

    float maxSecondDiff(const Signal& s, size_t from, size_t to)
    {
        float mx = 0.0f;
        for (size_t i = std::max<size_t>(from, 2); i < to; ++i)
            mx = std::max(mx, std::abs(s[i] - 2.0f * s[i - 1] + s[i - 2]));
        return mx;
    }
}

//==============================================================================
int main(int argc, char* argv[])
{
    g_folder = argc > 1 ? juce::File(juce::String(argv[1]))
                        : juce::File("/private/tmp/claude-501/-Users-samueljustice--claude-spares-spare-ZpAbzR/dfdcec56-f40c-4196-ada3-668f67b9775a/scratchpad/morph");
    g_folder.createDirectory();

    constexpr double fs = 48000.0;
    constexpr double secs = 4.0;
    const auto len = static_cast<size_t>(fs * secs);
    const size_t from = static_cast<size_t>(fs * 0.5), to = len;

    std::vector<Pair> pairs;
    pairs.push_back({ "saw_vs_noise", saw(fs, secs, 220.0f), filteredNoise(fs, secs) });
    pairs.push_back({ "sine300_vs_sine600", sine(fs, secs, 300.0f), sine(fs, secs, 600.0f) });
    pairs.push_back({ "voice_vs_chord", voice(fs, secs), chord(fs, secs) });

    for (auto& p : pairs)
    {
        writeWav(juce::String(p.name) + "_A", p.a, fs);
        writeWav(juce::String(p.name) + "_B", p.b, fs);
    }

    std::vector<float> staticSecondDiffAtHalf(3 * SpectralMorph::NumModes, 0.0f);

    for (int mode = 0; mode < SpectralMorph::NumModes; ++mode)
    {
        const juce::String modeName = SpectralMorph::modeName(mode);
        std::printf("\n=== %s ===\n", modeName.toRawUTF8());

        for (size_t pi = 0; pi < pairs.size(); ++pi)
        {
            const auto& pr = pairs[pi];
            const juce::String tag = modeName + "_" + pr.name;
            std::printf(" %s\n", pr.name);

            const float xs[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
            Signal outs[5];
            float levels[5], staticD2 = 0.0f;
            bool finite = true;
            for (int i = 0; i < 5; ++i)
            {
                outs[i] = render(fs, &pr.a, &pr.b, len, mode, constant(xs[i]), constant(0.5f));
                finite = finite && allFinite(outs[i]);
                levels[i] = rmsDb(outs[i], from, to);
                staticD2 = std::max(staticD2, maxSecondDiff(outs[i], from, to));
                if (i == 0 || i == 2 || i == 4)
                    writeWav(tag + "_x" + juce::String(xs[i], 2), outs[i], fs);
            }
            staticSecondDiffAtHalf[static_cast<size_t>(mode * 3) + pi] = maxSecondDiff(outs[2], from, to);
            check(finite, "finite output");

            const auto specA = spectrum(pr.a, from, to), specB = spectrum(pr.b, from, to);
            const auto spec0 = spectrum(outs[0], from, to), spec1 = spectrum(outs[4], from, to), specH = spectrum(outs[2], from, to);
            const float c0 = correlation(spec0, specA, fs), c1 = correlation(spec1, specB, fs);
            check(c0 > 0.9f, juce::String::formatted("x=0 ~ A: corr %.3f", c0));
            check(c1 > 0.9f, juce::String::formatted("x=1 ~ B: corr %.3f", c1));

            const float lo = std::min(levels[0], levels[4]) - 3.0f, hi = std::max(levels[0], levels[4]) + 3.0f;
            bool levelOk = true;
            for (int i = 1; i < 4; ++i) levelOk = levelOk && levels[i] >= lo && levels[i] <= hi;
            check(levelOk, juce::String::formatted("level dB x=0 %.1f, .25 %.1f, .5 %.1f, .75 %.1f, 1 %.1f",
                                                   levels[0], levels[1], levels[2], levels[3], levels[4]));

            if (pi == 1)
            {
                const float mid = bandPower(specH, fs, 350.0f, 550.0f);
                const float ends = bandPower(specH, fs, 250.0f, 349.0f) + bandPower(specH, fs, 551.0f, 650.0f);
                const float frac = mid / (mid + ends + 1.0e-20f);
                if (mode == SpectralMorph::Glide)
                    check(frac > 0.5f, juce::String::formatted("x=0.5 energy between 350 and 550 Hz: %.0f %%", 100.0f * frac));
                else
                    std::printf("  [info] x=0.5 energy between 350 and 550 Hz: %.0f %% (crossfade-like, expected low)\n", 100.0f * frac);
            }

            // Slow sweep 0 -> 1 over the render (WAV + level along the way).
            const auto sweep = render(fs, &pr.a, &pr.b, len, mode, [&](double t) { return static_cast<float>(juce::jlimit(0.0, 1.0, (t - 0.25) / (secs - 0.5))); }, constant(0.5f));
            writeWav(tag + "_sweep", sweep, fs);
            float minWin = 1000.0f, maxWin = -1000.0f;
            for (size_t w = from; w + 9600 <= to; w += 4800)
            {
                const float l = rmsDb(sweep, w, w + 9600);
                minWin = std::min(minWin, l);
                maxWin = std::max(maxWin, l);
            }
            check(allFinite(sweep) && minWin >= lo - 1.0f && maxWin <= hi + 1.0f,
                  juce::String::formatted("slow sweep level (200 ms windows) %.1f .. %.1f dB", minWin, maxWin));

            if (pi == 1)
            {
                // Peak frequency along the sweep: Glide should pass through the frequencies in between.
                juce::String track;
                int between = 0;
                for (double t = 0.6; t < secs - 0.2; t += 0.3)
                {
                    const auto w0 = static_cast<size_t>(t * fs);
                    const auto sp = spectrum(sweep, w0, w0 + 8192);
                    const auto peak = std::max_element(sp.begin(), sp.end()) - sp.begin();
                    const float hz = static_cast<float>(peak) * binHz(fs);
                    between += (hz > 330.0f && hz < 570.0f) ? 1 : 0;
                    track << juce::String(juce::roundToInt(hz)) << " ";
                }
                if (mode == SpectralMorph::Glide)
                    check(between >= 4, "sweep peak track (Hz): " + track);
                else
                    std::printf("  [info] sweep peak track (Hz): %s\n", track.toRawUTF8());
            }

            // Fast moves: x jumps 0 <-> 1 every 80 ms, y jumps every 130 ms.
            const auto fast = render(fs, &pr.a, &pr.b, len, mode,
                                     [](double t) { return std::fmod(t, 0.16) < 0.08 ? 0.0f : 1.0f; },
                                     [](double t) { return std::fmod(t, 0.26) < 0.13 ? 0.1f : 0.9f; });
            writeWav(tag + "_fast", fast, fs);
            const float fastD2 = maxSecondDiff(fast, from, to);
            check(allFinite(fast) && fastD2 <= 2.0f * staticD2,
                  juce::String::formatted("fast x/y jumps: 2nd-diff peak %.4f vs static %.4f (ratio %.2f)", fastD2, staticD2, fastD2 / (staticD2 + 1e-12f)));
        }
    }

    //--------------------------------------------------------------------------
    std::printf("\n=== mode switching ===\n");
    for (size_t pi = 0; pi < pairs.size(); ++pi)
    {
        const auto& pr = pairs[pi];
        const auto sw = render(fs, &pr.a, &pr.b, len, 0, constant(0.5f), constant(0.5f),
                               [](double t) { return static_cast<int>(t / 0.15) % SpectralMorph::NumModes; });
        writeWav(juce::String("modeswitch_") + pr.name, sw, fs);
        float ref = 0.0f;
        for (int m = 0; m < SpectralMorph::NumModes; ++m)
            ref = std::max(ref, staticSecondDiffAtHalf[static_cast<size_t>(m * 3) + pi]);
        const float d2 = maxSecondDiff(sw, from, to);
        check(allFinite(sw) && d2 <= 2.0f * ref, juce::String::formatted("%s: 2nd-diff peak %.4f vs static %.4f", pr.name, d2, ref));
    }

    //--------------------------------------------------------------------------
    std::printf("\n=== silence handling (x = 0.5) ===\n");
    {
        const auto& pr = pairs[0];
        const auto specA = spectrum(pr.a, from, to), specB = spectrum(pr.b, from, to);
        const float la = rmsDb(pr.a, from, to), lb = rmsDb(pr.b, from, to);
        Signal gappy = pr.a;
        for (size_t i = len / 2; i < len; ++i) gappy[i] = 0.0f;

        for (int mode = 0; mode < SpectralMorph::NumModes; ++mode)
        {
            const auto onlyB = render(fs, nullptr, &pr.b, len, mode, constant(0.5f), constant(0.5f));
            const auto onlyA = render(fs, &pr.a, nullptr, len, mode, constant(0.5f), constant(0.5f));
            const float cb = correlation(spectrum(onlyB, from, to), specB, fs), ca = correlation(spectrum(onlyA, from, to), specA, fs);
            const float lB = rmsDb(onlyB, from, to), lA = rmsDb(onlyA, from, to);
            check(cb > 0.9f && std::abs(lB - lb) < 1.5f, juce::String::formatted("%s, A silent -> B: corr %.3f, level %+.1f dB", SpectralMorph::modeName(mode), cb, lB - lb));
            check(ca > 0.9f && std::abs(lA - la) < 1.5f, juce::String::formatted("%s, B silent -> A: corr %.3f, level %+.1f dB", SpectralMorph::modeName(mode), ca, lA - la));

            const auto stop = render(fs, &gappy, &pr.b, len, mode, constant(0.5f), constant(0.5f));
            writeWav(juce::String("silence_Astops_") + SpectralMorph::modeName(mode), stop, fs);
            const size_t tail = len * 5 / 8;
            const float cs = correlation(spectrum(stop, tail, len), specB, fs);
            const float ls = rmsDb(stop, tail, len);
            check(cs > 0.9f && std::abs(ls - lb) < 1.5f, juce::String::formatted("%s, A stops mid-render -> B after: corr %.3f, level %+.1f dB", SpectralMorph::modeName(mode), cs, ls - lb));
        }
    }

    //--------------------------------------------------------------------------
    std::printf("\n=== sample rates ===\n");
    for (double rate : { 44100.0, 96000.0, 192000.0 })
    {
        const auto n = static_cast<size_t>(rate * 3.0);
        const auto a = sine(rate, 3.0, 300.0f), b = sine(rate, 3.0, 600.0f);
        for (int mode = 0; mode < SpectralMorph::NumModes; ++mode)
        {
            const auto out = render(rate, &a, &b, n, mode, [](double t) { return static_cast<float>(juce::jlimit(0.0, 1.0, t / 3.0)); }, constant(0.5f));
            SpectralMorph probe;
            probe.prepare(rate, 256);
            const float l = rmsDb(out, static_cast<size_t>(rate * 0.3), n);
            check(allFinite(out) && std::abs(l + 20.0f) < 3.0f,
                  juce::String::formatted("%.1f kHz %s: FFT %d, latency %d samples (%.1f ms), sweep level %.1f dB",
                                          rate / 1000.0, SpectralMorph::modeName(mode), probe.getFftSize(), probe.getLatencySamples(),
                                          1000.0 * probe.getLatencySamples() / rate, l));
        }
    }

    //--------------------------------------------------------------------------
    std::printf("\n=== realtime factor (20 s, 256-sample blocks, x sweeping) ===\n");
    for (double rate : { 48000.0, 192000.0 })
    {
        const double dur = 20.0;
        const auto n = static_cast<size_t>(rate * dur);
        const auto a = saw(rate, dur, 220.0f), b = filteredNoise(rate, dur);
        for (int mode = 0; mode < SpectralMorph::NumModes; ++mode)
        {
            const auto t0 = std::chrono::steady_clock::now();
            const auto out = render(rate, &a, &b, n, mode, [](double t) { return static_cast<float>(0.5 + 0.5 * std::sin(t)); }, constant(0.5f));
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double cpu = 100.0 * el / dur;
            check(allFinite(out) && (rate > 50000.0 || cpu < 5.0),
                  juce::String::formatted("%.0f kHz %s: %.3f s for %.0f s -> %.2f %% of one core (%.0fx realtime)",
                                          rate / 1000.0, SpectralMorph::modeName(mode), el, dur, cpu, dur / el));
        }
    }

    std::printf("\n%s (%d failure%s). WAVs in %s\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s", g_folder.getFullPathName().toRawUTF8());
    return g_failures == 0 ? 0 : 1;
}

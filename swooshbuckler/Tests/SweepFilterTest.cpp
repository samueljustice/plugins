// Bus filter test: the laws, the response of each type and slope against the analytic one, a held Low Pass against
// sj::Svf to the bit, spurs under a 20 Hz sweep, stability at full Resonance, the rhythm's shapes, a change of Type,
// and the curve the plate over a bus draws (busview::filterGain) against the filter's gain measured with sine tones.
// Plain C++, no JUCE: SweepFilter is free of it.
//
//   SwooshbucklerSweepFilter   prints what it measures and ends with "ALL PASSED" (exit code 0) or "n FAILED".

#include "SweepFilter.h"
#include "BusView.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void check(bool ok, const std::string& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }

    constexpr double c_pi = 3.14159265358979323846;
    constexpr float c_fs = 48000.0f;
    const char* const c_typeNames[SweepFilter::c_types] = { "low pass", "high pass", "band pass", "notch" };

    std::string fmt(const char* format, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0, double e = 0.0)
    {
        char text[256];
        std::snprintf(text, sizeof(text), format, a, b, c, d, e);
        return text;
    }
    double db(double x) { return 20.0 * std::log10(std::max(x, 1.0e-30)); }

    /** One chunk, the way the engine drives it, with the cutoff given directly. */
    void chunk(SweepFilter& filter, const SweepFilter::Settings& s, float cutoff, float q, const float* x, float* out, int frames)
    {
        filter.advance(s, frames);
        filter.setTarget(s, cutoff, q);
        filter.process(0, x, out, frames);
        filter.endChunk();
    }

    /** The response the class comment promises, at `hz`, for a filter held at `cutoff` and `q`. */
    double analytic(int type, bool steep, double cutoff, double q, double hz, double fs)
    {
        const double w = std::tan(c_pi * hz / fs) / std::tan(c_pi * cutoff / fs);
        const std::complex<double> s(0.0, w);
        auto stage = [&](double stageQ)
        {
            const std::complex<double> den = s * s + s / stageQ + 1.0;
            switch (type)
            {
                case SweepFilter::HighPass: return s * s / den;
                case SweepFilter::BandPass: return s / den / 0.7;
                case SweepFilter::Notch:    return (s * s + 1.0) / den;
                default:                    return 1.0 / den;
            }
        };
        if (! steep)
            return std::abs(stage(q));
        if (type == SweepFilter::BandPass)
            return std::abs(stage(0.7) * stage(q));
        if (type == SweepFilter::Notch)
            return std::abs(stage(q) * stage(q));
        return std::abs(stage(0.5412) * stage(1.8478 * q));
    }

    /** The gain of a held filter at `hz`, from a sine. */
    double measured(int type, bool steep, float cutoff, float q, double hz)
    {
        SweepFilter filter;
        filter.prepare(c_fs);
        SweepFilter::Settings s;
        s.type = type;
        s.steep = steep;
        const int settle = 48000, count = 48000;
        double re = 0.0, im = 0.0;
        float x[32], y[32];
        for (int n = 0; n < settle + count; n += 32)
        {
            for (int i = 0; i < 32; ++i)
                x[i] = static_cast<float>(std::sin(2.0 * c_pi * hz * (n + i) / c_fs));
            chunk(filter, s, cutoff, q, x, y, 32);
            if (n >= settle)
                for (int i = 0; i < 32; ++i)
                {
                    const double ph = 2.0 * c_pi * hz * (n + i) / c_fs;
                    re += y[i] * std::cos(ph);
                    im += y[i] * std::sin(ph);
                }
        }
        return 2.0 * std::sqrt(re * re + im * im) / count;
    }

    /** The parts of a signal that repeats every 2400 samples (20 Hz at 48 kHz), on the 20 Hz grid: bin k is 20 k Hz. */
    std::vector<double> spectrum20(const std::vector<float>& x, size_t from)
    {
        const size_t period = 2400;
        std::vector<double> fold(period, 0.0);
        const size_t periods = (x.size() - from) / period;
        for (size_t n = 0; n < periods * period; ++n)
            fold[n % period] += x[from + n] / static_cast<double>(periods);
        std::vector<double> cosTable(period), sinTable(period);
        for (size_t n = 0; n < period; ++n)
        {
            cosTable[n] = std::cos(2.0 * c_pi * n / period);
            sinTable[n] = std::sin(2.0 * c_pi * n / period);
        }
        std::vector<double> mag(period / 2, 0.0);
        for (size_t k = 1; k < period / 2; ++k)
        {
            double re = 0.0, im = 0.0;
            for (size_t n = 0; n < period; ++n)
            {
                re += fold[n] * cosTable[(k * n) % period];
                im += fold[n] * sinTable[(k * n) % period];
            }
            mag[k] = 2.0 * std::sqrt(re * re + im * im) / period;
        }
        return mag;
    }

    struct Spurs { double spurDb = -400.0, residualDb = -400.0; int bins = 0; };

    /** A 1 kHz sine through a filter swept at 20 Hz around 1 kHz, in chunks of `frames`, against the same filter
        driven a sample at a time (its place is then right at every sample, with nothing at the chunk rate): the
        largest part of the output where that one has nothing over -120 dB, against the output's largest part. */
    Spurs sweepSpurs(int type, bool steep, float sweep, float resonance, int frames)
    {
        SweepFilter::Settings s;
        s.type = type;
        s.steep = steep;
        s.follow = 0.0f;
        s.sweep = sweep;
        s.resonance = resonance;
        s.rateHz = 20.0f;
        const float f = 0.36f; // q() stays at its lowest; Resonance adds to it
        // Centred on 1 kHz in every type: Closed At from the law, turned back through the type's mirror.
        s.closedAtHz = 1000.0f;
        SweepFilter::Settings probe = s;
        probe.type = SweepFilter::LowPass;
        const float centre = SweepFilter::cutoffHz(probe, f, 0.0f, 0.0f, c_fs);
        s.closedAtHz = type == SweepFilter::HighPass ? 1000.0f * 440.0f / centre : 1000.0f * 1000.0f / centre;

        const int total = 48000 * 3;
        std::vector<float> in(static_cast<size_t>(total)), out(in.size()), ref(in.size());
        for (int n = 0; n < total; ++n)
            in[static_cast<size_t>(n)] = 0.5f * static_cast<float>(std::sin(2.0 * c_pi * 1000.0 * n / c_fs));
        auto run = [&](int step, std::vector<float>& to)
        {
            SweepFilter filter;
            filter.prepare(c_fs);
            for (int n = 0; n < total; n += step)
            {
                const float lfo = filter.advance(s, step);
                filter.setTarget(s, SweepFilter::cutoffHz(s, f, 0.0f, lfo, c_fs), SweepFilter::q(s, f));
                filter.process(0, in.data() + n, to.data() + n, step);
                filter.endChunk();
            }
        };
        run(frames, out);
        run(1, ref);
        const auto a = spectrum20(out, 48000), b = spectrum20(ref, 48000);
        const double topA = *std::max_element(a.begin(), a.end()), topB = *std::max_element(b.begin(), b.end());
        Spurs result;
        for (size_t k = 1; k < a.size(); ++k)
            if (db(b[k] / topB) < -120.0)
            {
                result.spurDb = std::max(result.spurDb, db(a[k] / topA));
                ++result.bins;
            }
        double diff = 0.0, level = 0.0;
        for (size_t n = 48000; n < out.size(); ++n)
        {
            diff += static_cast<double>(out[n] - ref[n]) * (out[n] - ref[n]);
            level += static_cast<double>(ref[n]) * ref[n];
        }
        result.residualDb = 10.0 * std::log10(std::max(diff, 1.0e-30) / std::max(level, 1.0e-30));
        return result;
    }
}

int main()
{
    std::cout << "Filter laws" << std::endl;
    {
        // At its defaults the filter's cutoff and Q are the bus's earlier law.
        const SweepFilter::Settings defaults;
        float worst = 0.0f;
        for (float f = 0.0f; f <= 1.0f; f += 0.05f)
            for (float peak = 0.0f; peak <= 1.0f; peak += 0.1f)
            {
                const float before = std::max(170.0f, 170.0f * std::exp2(7.0f * std::sqrt(peak) * (1.0f - 0.9f * std::sqrt(f))));
                const float now = SweepFilter::cutoffHz(defaults, f, peak, 0.0f, 48000.0f);
                worst = std::max(worst, std::abs(now - before) / before);
                const float r = std::clamp((f - 0.6f) / 0.4f, 0.0f, 1.0f);
                worst = std::max(worst, std::abs(SweepFilter::q(defaults, f) - (0.7f + 1.5f * r * r)));
            }
        check(worst < 1.0e-5f, "defaults: the cutoff and Q are the bus's earlier law");

        // Each type's place on the axis: High Pass mirrors, Band Pass and Notch stop at 12 kHz.
        SweepFilter::Settings s;
        s.type = SweepFilter::HighPass;
        const float closedHigh = SweepFilter::cutoffHz(s, 1.0f, 0.0f, 0.0f, 48000.0f); // closed: Low Pass would sit at 170 Hz
        const float openHigh = SweepFilter::cutoffHz(s, 0.0f, 1.0f, 0.0f, 48000.0f);   // open: Low Pass would sit at 21.6 kHz
        std::cout << "         high pass: closed " << closedHigh << " Hz, open " << openHigh << " Hz" << std::endl;
        check(std::abs(closedHigh - 440000.0f / 170.0f) < 1.0f && std::abs(openHigh - 440000.0f / 21600.0f) < 0.1f,
              "high pass: mirrored on the log axis (closed at 170 Hz it sits at 2.6 kHz and opens downward)");
        s.type = SweepFilter::BandPass;
        const float band = SweepFilter::cutoffHz(s, 0.0f, 1.0f, 0.0f, 48000.0f);
        s.type = SweepFilter::Notch;
        const float notch = SweepFilter::cutoffHz(s, 0.0f, 1.0f, 0.0f, 48000.0f);
        s.type = SweepFilter::BandPass;
        const float bandClosed = SweepFilter::cutoffHz(s, 1.0f, 0.0f, 0.0f, 48000.0f);
        check(band == 12000.0f && notch == 12000.0f && std::abs(bandClosed - 170.0f) < 0.01f, "band pass and notch: centred on the cutoff, 12 kHz at the most");
        // Follow at 0: the wheel is a plain cutoff (the level does not move it); Sweep moves it three octaves each way.
        SweepFilter::Settings plain;
        plain.follow = 0.0f;
        const bool still = SweepFilter::cutoffHz(plain, 0.5f, 0.0f, 0.0f, 48000.0f) == SweepFilter::cutoffHz(plain, 0.5f, 1.0f, 0.0f, 48000.0f);
        plain.sweep = 1.0f;
        const float up = SweepFilter::octaves(plain, 1.0f, 0.0f, 1.0f) - SweepFilter::octaves(plain, 1.0f, 0.0f, 0.0f);
        check(still && std::abs(up - 3.0f) < 1.0e-5f, "follow 0%: the level does not move it; sweep 100%: three octaves either way");
    }

    std::cout << "How open it is: on the whole of its travel, and in its present travel" << std::endl;
    {
        const float fs = 48000.0f;
        SweepFilter::Settings s;
        // The whole travel with the layer at its defaults: Closed At (170 Hz) to seven octaves over it (0.45 fs comes first).
        const float top = 0.45f * fs;
        check(SweepFilter::openness(s, 170.0f, fs) == 0.0f && SweepFilter::openness(s, top, fs) == 1.0f && SweepFilter::openness(s, 20.0f, fs) == 0.0f,
              "openness: 0 at Closed At, 1 at the top of the travel");
        const float half = SweepFilter::openness(s, std::sqrt(170.0f * top), fs);
        check(std::abs(half - 0.5f) < 1.0e-5f, "openness: a straight line on the log-frequency axis (half way at the geometric middle)");
        // The front wheel moves the answer as it moves the filter: at the whoosh's peak the wheel at 0 is fully
        // open and at 100% it is 0.7 octaves over Closed At, a tenth of the way.
        const float atZero = SweepFilter::openness(s, SweepFilter::openHz(s, 0.0f, 1.0f, 0.0f, fs), fs), atFull = SweepFilter::openness(s, SweepFilter::openHz(s, 1.0f, 1.0f, 0.0f, fs), fs);
        std::cout << "         at the whoosh's peak: the wheel at 0% reads " << atZero << ", at 100% " << atFull << std::endl;
        check(atZero == 1.0f && std::abs(atFull - 0.7f / std::log2(top / 170.0f)) < 1.0e-4f, "openness: the front wheel is in it");
        // Sweep widens the travel by its three octaves either way; the place is the same for every Type.
        SweepFilter::Settings wide = s;
        wide.sweep = 1.0f;
        wide.follow = 0.0f;
        SweepFilter::Settings following = wide;
        following.follow = 1.0f; // (the whoosh gone and the rhythm at its bottom: three octaves under Closed At)
        const float lowest = SweepFilter::openness(following, SweepFilter::openHz(following, 1.0f, 0.0f, -1.0f, fs), fs);
        const float closedAt = SweepFilter::openness(wide, 170.0f, fs);
        check(lowest < 1.0e-5f && std::abs(closedAt - 3.0f / std::log2(top / (170.0f / 8.0f))) < 1.0e-4f,
              "openness: Sweep's octaves under Closed At are part of the travel (Closed At itself is then " + fmt("%.3f of the way up)", static_cast<double>(closedAt)));
        for (const int type : { SweepFilter::HighPass, SweepFilter::BandPass, SweepFilter::Notch })
        {
            SweepFilter::Settings typed = wide;
            typed.type = type;
            check(SweepFilter::openHz(typed, 0.4f, 0.3f, 0.5f, fs) == SweepFilter::openHz(wide, 0.4f, 0.3f, 0.5f, fs)
                      && SweepFilter::cutoffHz(typed, 0.4f, 0.3f, 0.5f, fs) == SweepFilter::placeHz(typed, SweepFilter::openHz(typed, 0.4f, 0.3f, 0.5f, fs), fs),
                  std::string(c_typeNames[type]) + ": how open it is does not depend on the Type; cutoffHz is that, placed for the Type");
        }
        // The present travel: whatever the wheel, the whoosh at its peak with the rhythm at its top is 1 and the
        // whoosh gone with the rhythm at its bottom is 0; nothing moving is 0.
        bool ends = true;
        for (const float f : { 0.05f, 0.3f, 0.6f, 1.0f })
            for (const float follow : { 0.0f, 0.5f, 1.0f })
                for (const float sweep : { 0.0f, 0.3f, 1.0f })
                {
                    SweepFilter::Settings t;
                    t.follow = follow;
                    t.sweep = sweep;
                    const float hi = SweepFilter::travel(t, f, SweepFilter::openHz(t, f, 1.0f, 1.0f, fs), fs), lo = SweepFilter::travel(t, f, SweepFilter::openHz(t, f, 0.0f, -1.0f, fs), fs);
                    const bool moves = follow > 0.0f || sweep > 0.0f;
                    ends = ends && lo == 0.0f && (moves ? hi == 1.0f : hi == 0.0f);
                }
        check(ends, "travel: 0 at the bottom and 1 at the top of what Follow and Sweep give it wherever the wheel is; 0 when neither moves it");
        // The rhythm read ahead: what advance() gives that much later, for the shapes that have no edge.
        bool aheadOk = true;
        float worstAhead = 0.0f;
        for (const int shape : { SweepFilter::Sine, SweepFilter::Triangle })
        {
            SweepFilter f;
            f.prepare(fs);
            SweepFilter::Settings t;
            t.shape = shape;
            t.rateHz = 1.7f;
            std::vector<float> told;
            for (int n = 0; n < 6000; ++n)
            {
                const float now = f.advance(t, 32);
                told.push_back(f.ahead(t, 45.0f * 32.0f / fs)); // 45 chunks on
                if (n >= 45)
                    worstAhead = std::max(worstAhead, std::abs(told[static_cast<size_t>(n - 45)] - now));
            }
            aheadOk = aheadOk && worstAhead < 2.0e-3f;
        }
        check(aheadOk, fmt("ahead: a Sine or Triangle rhythm 30 ms on is what advance() gives 30 ms later (%.5f off at most)", static_cast<double>(worstAhead)));
    }

    std::cout << "Response of each type and slope (held, against the analytic response)" << std::endl;
    {
        const double probes[] = { 30.0, 60.0, 125.0, 250.0, 500.0, 707.0, 1000.0, 1414.0, 2000.0, 4000.0, 8000.0, 16000.0 };
        const float cutoffs[] = { 170.0f, 1000.0f, 6000.0f };
        const float qs[] = { 0.7f, 2.2f, 10.2f };
        for (int type = 0; type < SweepFilter::c_types; ++type)
            for (int steep = 0; steep < 2; ++steep)
            {
                double worst = 0.0;
                int points = 0;
                for (float cutoff : cutoffs)
                    for (float q : qs)
                        for (double hz : probes)
                        {
                            const double want = analytic(type, steep != 0, cutoff, q, hz, c_fs);
                            if (want < 1.0e-4) // under -80 dB: single precision's floor, not the curve
                                continue;
                            worst = std::max(worst, std::abs(db(measured(type, steep != 0, cutoff, q, hz) / want)));
                            ++points;
                        }
                check(worst < 0.02, std::string(c_typeNames[type]) + (steep ? ", 24 dB" : ", 12 dB") + fmt(": within %.4f dB of analytic at %.0f points", worst, points));
            }

        // The slopes and the corner, low enough that the warp of the top of the band does not show.
        const double lp12 = db(measured(SweepFilter::LowPass, false, 50.0f, 0.7f, 400.0) / measured(SweepFilter::LowPass, false, 50.0f, 0.7f, 800.0));
        const double lp24 = db(measured(SweepFilter::LowPass, true, 50.0f, 0.7f, 400.0) / measured(SweepFilter::LowPass, true, 50.0f, 0.7f, 800.0));
        const double hp12 = db(measured(SweepFilter::HighPass, false, 3200.0f, 0.7f, 400.0) / measured(SweepFilter::HighPass, false, 3200.0f, 0.7f, 200.0));
        const double hp24 = db(measured(SweepFilter::HighPass, true, 3200.0f, 0.7f, 400.0) / measured(SweepFilter::HighPass, true, 3200.0f, 0.7f, 200.0));
        const double bp12 = db(measured(SweepFilter::BandPass, false, 1000.0f, 0.7f, 62.5) / measured(SweepFilter::BandPass, false, 1000.0f, 0.7f, 31.25));
        const double bp24 = db(measured(SweepFilter::BandPass, true, 1000.0f, 0.7f, 62.5) / measured(SweepFilter::BandPass, true, 1000.0f, 0.7f, 31.25));
        std::cout << fmt("         dB an octave: low pass %.2f and %.2f, high pass %.2f and %.2f", lp12, lp24, hp12, hp24)
                  << fmt(", band pass skirt %.2f and %.2f", bp12, bp24) << std::endl;
        check(std::abs(lp12 - 12.0) < 0.3 && std::abs(lp24 - 24.0) < 0.3 && std::abs(hp12 - 12.0) < 0.3 && std::abs(hp24 - 24.0) < 0.3,
              "low pass and high pass fall 12 dB an octave, 24 dB with the key");
        check(std::abs(bp12 - 6.0) < 0.2 && std::abs(bp24 - 12.0) < 0.3, "band pass falls 6 dB an octave either side, 12 dB with the key");

        const double corner12 = db(measured(SweepFilter::LowPass, false, 1000.0f, 0.7f, 1000.0));
        const double corner24 = db(measured(SweepFilter::LowPass, true, 1000.0f, 0.7f, 1000.0));
        const double peak12 = db(measured(SweepFilter::LowPass, false, 1000.0f, 10.2f, 1000.0));
        const double peak24 = db(measured(SweepFilter::LowPass, true, 1000.0f, 10.2f, 1000.0));
        const double bandPeak12 = db(measured(SweepFilter::BandPass, false, 1000.0f, 0.7f, 1000.0));
        const double bandPeak24 = db(measured(SweepFilter::BandPass, true, 1000.0f, 0.7f, 1000.0));
        const double bandRes12 = db(measured(SweepFilter::BandPass, false, 1000.0f, 10.2f, 1000.0));
        const double bandRes24 = db(measured(SweepFilter::BandPass, true, 1000.0f, 10.2f, 1000.0));
        const double notch12 = db(measured(SweepFilter::Notch, false, 1000.0f, 0.7f, 1000.0));
        const double notch24 = db(measured(SweepFilter::Notch, true, 1000.0f, 0.7f, 1000.0));
        const double notchSide = db(measured(SweepFilter::Notch, false, 1000.0f, 0.7f, 100.0));
        std::cout << fmt("         low pass at its cutoff, Q 0.7: %.2f dB (12 dB), %.2f dB (24 dB); at Q 10.2: %+.2f dB, %+.2f dB", corner12, corner24, peak12, peak24) << std::endl;
        std::cout << fmt("         band pass at its centre, Q 0.7: %+.2f dB, %+.2f dB; at Q 10.2: %+.2f dB, %+.2f dB", bandPeak12, bandPeak24, bandRes12, bandRes24) << std::endl;
        std::cout << fmt("         notch at its centre: %.1f dB (12 dB), %.1f dB (24 dB); a decade under it %.2f dB", notch12, notch24, notchSide) << std::endl;
        check(std::abs(corner12 + 3.1) < 0.1 && std::abs(corner24 + 3.1) < 0.2, "low pass: 3 dB down at its cutoff at the lowest Q, at either slope");
        check(std::abs(peak12 - peak24) < 0.5 && std::abs(peak12 - db(10.2)) < 0.1, "low pass: the resonant peak is Q, the same at either slope");
        check(std::abs(bandPeak12) < 0.01 && std::abs(bandPeak24) < 0.01 && std::abs(bandRes12 - bandRes24) < 0.01, "band pass: level at the lowest Q, the same peak at either slope");
        check(notch12 < -60.0 && notch24 < -60.0 && std::abs(notchSide) < 0.2, "notch: more than 60 dB deep at its centre, level away from it");
    }

    std::cout << "The curve the plate over a bus draws (busview::filterGain) against the filter, with sine tones" << std::endl;
    {
        // The plate draws busview::filterGain at the render rate for the filter's place and Q: driven with a sine at
        // each of these frequencies, the filter's gain is that curve's, for every type and slope, at a low, a middle
        // and a resonant Q, wherever the filter is.
        const double tones[] = { 40.0, 110.0, 315.0, 700.0, 1000.0, 1900.0, 4400.0, 9000.0, 15000.0 };
        const float places[] = { 120.0f, 900.0f, 5000.0f };
        const float qs[] = { 0.7f, 1.9f, 8.0f };
        double worst = 0.0;
        int points = 0;
        for (int type = 0; type < SweepFilter::c_types; ++type)
            for (int steep = 0; steep < 2; ++steep)
                for (const float place : places)
                    for (const float q : qs)
                    {
                        busview::FilterLook look;
                        look.type = type;
                        look.steep = steep != 0;
                        look.hz = place;
                        look.q = q;
                        for (const double hz : tones)
                        {
                            const double drawn = busview::filterGain(look, hz, c_fs);
                            if (drawn < 1.0e-3) // under -60 dB: off the foot of the plate's graph (-48 dB)
                                continue;
                            worst = std::max(worst, std::abs(db(measured(type, steep != 0, place, q, hz) / drawn)));
                            ++points;
                        }
                    }
        check(worst < 0.05, fmt("the drawn curve is the filter's measured gain within %.4f dB at %.0f points (every type, both slopes)", worst, points));
        // What is heard: the dry and the filter mixed by the wheel's law, both ends.
        busview::FilterLook look;
        look.hz = 1000.0f;
        look.mix = 0.0f;
        const bool dryAtNothing = std::abs(busview::heardGain(look, 5000.0, c_fs) - 1.0) < 1.0e-9;
        look.mix = 1.0f;
        look.trim = 0.8f;
        const bool allAtFull = std::abs(busview::heardGain(look, 300.0, c_fs) - static_cast<double>(look.trim) * busview::filterGain(look, 300.0, c_fs)) < 1.0e-9;
        check(dryAtNothing && allAtFull && std::abs(busview::filterMix(0.0f)) < 1.0e-7f && std::abs(busview::filterMix(0.8f) - 1.0f) < 1.0e-5f && busview::filterMix(1.0f) == 1.0f,
              "the heard line: the dry alone with Filter at 0%, the trimmed filter alone from 80% up");
    }

    std::cout << "Defaults against sj::Svf (the filter a bus runs when nothing behind the wheel is touched)" << std::endl;
    {
        const SweepFilter::Settings defaults;
        sj::Random noise;
        noise.seed(0x1234567u);
        // Held. Given the same cutoff and Q, the same samples to the bit. Given the plain law written out the way
        // the bus had it (its terms multiply in another order, so the cutoff can differ in its last bit), the
        // difference is that last bit's worth.
        float worstHeld = 0.0f, worstWritten = 0.0f;
        for (float f : { 0.1f, 0.5f, 0.8f, 1.0f })
            for (float peak : { 0.0f, 0.3f, 1.0f })
            {
                SweepFilter filter;
                filter.prepare(c_fs);
                sj::Svf svf, written;
                float x[32], y[32];
                for (int n = 0; n < 48000; n += 32)
                {
                    for (auto& v : x)
                        v = 0.9f * noise.bipolar();
                    const float cutoff = SweepFilter::cutoffHz(defaults, f, peak, filter.advance(defaults, 32), c_fs);
                    const float q = SweepFilter::q(defaults, f);
                    filter.setTarget(defaults, cutoff, q);
                    filter.process(0, x, y, 32);
                    filter.endChunk();
                    svf.set(cutoff, q, c_fs);
                    const float r = std::clamp((f - 0.6f) / 0.4f, 0.0f, 1.0f);
                    written.set(std::max(170.0f, 170.0f * std::exp2(7.0f * std::sqrt(peak) * (1.0f - 0.9f * std::sqrt(f)))), 0.7f + 1.5f * r * r, c_fs);
                    for (int i = 0; i < 32; ++i)
                    {
                        worstHeld = std::max(worstHeld, std::abs(svf.lowpass(x[i]) - y[i]));
                        worstWritten = std::max(worstWritten, std::abs(written.lowpass(x[i]) - y[i]));
                    }
                }
            }
        std::cout << "         held: largest difference " << worstHeld << " given the same cutoff and Q; " << worstWritten
                  << fmt(" (%.1f dB) against the law written out", db(worstWritten)) << std::endl;
        check(worstHeld == 0.0f, "defaults, held: sj::Svf's low pass to the bit (12 settings, a second of noise each)");
        check(worstWritten < 1.0e-5f, "defaults, held, the law written out: under -100 dB");

        // Moving: sj::Svf stepped every 32 samples against the glide. They are not the same filter while it moves;
        // these are the numbers for whoever nulls the engine. Then held again: how long until it is exact.
        for (float passHz : { 0.25f, 1.0f, 4.0f })
        {
            SweepFilter filter;
            filter.prepare(c_fs);
            sj::Svf svf;
            double diff = 0.0, level = 0.0;
            float x[32], y[32];
            const float f = 0.7f;
            const int moving = 48000 * 4, total = moving + 4800;
            int lastDifferent = -1;
            for (int n = 0; n < total; n += 32)
            {
                for (auto& v : x)
                    v = 0.5f * noise.bipolar();
                const float t = static_cast<float>(std::min(n + 32, moving)) / c_fs;
                const float peak = 0.5f - 0.5f * std::cos(sj::c_twoPi * passHz * t);
                const float cutoff = SweepFilter::cutoffHz(defaults, f, peak, 0.0f, c_fs);
                chunk(filter, defaults, cutoff, SweepFilter::q(defaults, f), x, y, 32);
                svf.set(cutoff, SweepFilter::q(defaults, f), c_fs);
                for (int i = 0; i < 32; ++i)
                {
                    const float want = svf.lowpass(x[i]);
                    if (n < moving)
                    {
                        diff += static_cast<double>(want - y[i]) * (want - y[i]);
                        level += static_cast<double>(want) * want;
                    }
                    else if (want != y[i])
                        lastDifferent = n + i - moving;
                }
            }
            std::cout << fmt("         a pass every %.2f s (the level swelling 0..1, Filter 70%%): the glide is %.1f dB under the stepped filter's output;", 1.0 / passHz, -10.0 * std::log10(diff / level))
                      << " held after it, the last sample that differs is number " << lastDifferent << std::endl;
            check(lastDifferent < 4800 && diff < level, "moving, then held: the glide stays close and ends on sj::Svf's low pass to the bit");
        }
    }

    std::cout << "Spurs under a 20 Hz sweep (a 1 kHz sine, the filter swept around 1 kHz, 32-sample chunks)" << std::endl;
    {
        double worstAll = -400.0;
        for (float sweep : { 1.0f / 3.0f, 1.0f })
            for (float resonance : { 0.0f, 0.5f })
                for (int type = 0; type < SweepFilter::c_types; ++type)
                    for (int steep = 0; steep < 2; ++steep)
                    {
                        const Spurs r = sweepSpurs(type, steep != 0, sweep, resonance, 32);
                        worstAll = std::max(worstAll, r.spurDb);
                        std::cout << fmt("         +-%.0f octaves, Resonance %.0f%%, ", 3.0 * sweep, 100.0 * resonance) << c_typeNames[type] << (steep ? " 24 dB" : " 12 dB")
                                  << fmt(": largest spur %.1f dB (%.0f of 1199 parts looked at), residual against a sample-at-a-time filter %.1f dB", r.spurDb, r.bins, r.residualDb) << std::endl;
                        check(r.spurDb < -80.0 && r.bins > 200, "spurs under -80 dB");
                    }
        std::cout << fmt("         worst of all: %.1f dB", worstAll) << std::endl;

        // What stepping the coefficients every 32 samples leaves, for comparison: sj::Svf with the same sweep.
        std::vector<float> stepped(48000 * 3), smooth(stepped.size());
        sj::Svf a, b;
        for (size_t n = 0; n < stepped.size(); ++n)
        {
            const float x = 0.5f * static_cast<float>(std::sin(2.0 * c_pi * 1000.0 * static_cast<double>(n) / c_fs));
            auto hz = [](size_t at) { return 1000.0f * std::exp2(std::sin(sj::c_twoPi * 20.0f * static_cast<float>(at % 2400) / c_fs)); };
            if (n % 32 == 0)
                a.set(hz(n + 32), 0.7f, c_fs);
            b.set(hz(n + 1), 0.7f, c_fs);
            stepped[n] = a.lowpass(x);
            smooth[n] = b.lowpass(x);
        }
        const auto sa = spectrum20(stepped, 48000), sb = spectrum20(smooth, 48000);
        const double topA = *std::max_element(sa.begin(), sa.end()), topB = *std::max_element(sb.begin(), sb.end());
        double spur = -400.0;
        for (size_t k = 1; k < sa.size(); ++k)
            if (db(sb[k] / topB) < -120.0)
                spur = std::max(spur, db(sa[k] / topA));
        std::cout << fmt("         for comparison, coefficients stepped every 32 samples (+-1 octave, low pass 12 dB): largest spur %.1f dB", spur) << std::endl;
    }

    std::cout << "Full Resonance, hot input" << std::endl;
    {
        sj::Random noise;
        noise.seed(0xBEEF01u);
        bool finite = true, decays = true;
        double biggest = 0.0, slowestTail = 0.0;
        for (int type = 0; type < SweepFilter::c_types; ++type)
            for (int steep = 0; steep < 2; ++steep)
                for (int shape : { static_cast<int>(SweepFilter::Sine), static_cast<int>(SweepFilter::Square), static_cast<int>(SweepFilter::Random) })
                {
                    SweepFilter::Settings s;
                    s.type = type;
                    s.steep = steep != 0;
                    s.shape = shape;
                    s.resonance = 1.0f;
                    s.sweep = 1.0f;
                    s.rateHz = 20.0f;
                    SweepFilter filter;
                    filter.prepare(c_fs);
                    filter.setSeed(0x51u + static_cast<std::uint32_t>(type));
                    float x[32], y[32];
                    double top = 0.0, tail = 0.0;
                    const int loud = 48000 * 6, total = loud + 48000 * 2;
                    for (int n = 0; n < total; n += 32)
                    {
                        // +12 dB over full scale: noise for two seconds, then a square that sits on the filter.
                        for (int i = 0; i < 32; ++i)
                        {
                            const int at = n + i;
                            x[i] = at >= loud ? 0.0f : (at < 96000 ? 4.0f * noise.bipolar() : ((at / 24) % 2 == 0 ? 4.0f : -4.0f));
                        }
                        const float f = 1.0f; // the knob's own resonance on top: Q 10.2
                        const float peak = 0.5f + 0.5f * std::sin(sj::c_twoPi * 3.0f * static_cast<float>(n) / c_fs);
                        const float lfo = filter.advance(s, 32);
                        filter.setTarget(s, SweepFilter::cutoffHz(s, f, peak, lfo, c_fs), SweepFilter::q(s, f));
                        filter.process(0, x, y, 32);
                        filter.endChunk();
                        for (int i = 0; i < 32; ++i)
                        {
                            finite = finite && std::isfinite(y[i]);
                            top = std::max(top, static_cast<double>(std::abs(y[i])));
                            if (n >= loud + 48000)
                                tail = std::max(tail, static_cast<double>(std::abs(y[i])));
                        }
                    }
                    biggest = std::max(biggest, top);
                    slowestTail = std::max(slowestTail, tail);
                    decays = decays && tail < 1.0e-6;
                }
        std::cout << fmt("         24 runs (4 types, 2 slopes, 3 rhythms at 20 Hz and +-3 octaves), input at +12 dB: largest output %.1f (%.1f dB over the input),", biggest, db(biggest / 4.0))
                  << fmt(" largest sample a second after the input stops %.2e", slowestTail) << std::endl;
        check(finite, "full Resonance: every sample finite");
        check(biggest < 4.0 * 10.2 * 4.0, "full Resonance: the output stays within a few times Q over the input");
        check(decays, "full Resonance: silent a second after the input stops");
    }

    std::cout << "The rhythm" << std::endl;
    {
        const char* const names[SweepFilter::c_shapes] = { "sine", "triangle", "ramp up", "ramp down", "square", "random" };
        for (int shape = 0; shape < SweepFilter::c_shapes; ++shape)
        {
            SweepFilter::Settings s;
            s.shape = shape;
            s.rateHz = 2.0f;
            SweepFilter filter;
            filter.prepare(c_fs);
            filter.setSeed(77u);
            float low = 2.0f, high = -2.0f, quarter = 0.0f, biggestStep = 0.0f, last = 0.0f;
            int rises = 0;
            for (int n = 0; n < 48000 * 4; n += 32)
            {
                const float v = filter.advance(s, 32);
                if (n > 0)
                {
                    biggestStep = std::max(biggestStep, std::abs(v - last));
                    if (last <= 0.0f && v > 0.0f)
                        ++rises;
                }
                if (n + 32 == 6016)
                    quarter = v; // a quarter of the first cycle (16 samples past it)
                low = std::min(low, v);
                high = std::max(high, v);
                last = v;
            }
            std::cout << "         " << names[shape] << fmt(": %.3f .. %.3f, at a quarter of its cycle %.3f, largest move in a chunk %.4f, upward crossings in 4 s: %.0f", low, high, quarter, biggestStep, rises) << std::endl;
            bool ok = low >= -1.0f && high <= 1.0f && biggestStep <= SweepFilter::c_slewPerSecond * 32.0f / c_fs + 1.0e-6f;
            if (shape == SweepFilter::Sine || shape == SweepFilter::Triangle)
                ok = ok && std::abs(quarter - 1.0f) < 0.01f && low < -0.99f && high > 0.99f && rises >= 7 && rises <= 8;
            if (shape == SweepFilter::RampUp)
                ok = ok && std::abs(quarter + 0.5f) < 0.01f && high > 0.99f;
            if (shape == SweepFilter::RampDown)
                ok = ok && std::abs(quarter - 0.5f) < 0.01f && low < -0.99f;
            if (shape == SweepFilter::Square)
                ok = ok && quarter == 1.0f && low == -1.0f && high == 1.0f && rises >= 7 && rises <= 8;
            check(ok, std::string(names[shape]) + ": in range, the right shape, never faster than its whole swing in 4 ms");
        }

        // Random: a fresh level each cycle, the same from the same seed whichever of reset() and setSeed() came
        // first, different from another; scatter() starts the cycle elsewhere.
        auto levels = [](std::uint32_t seed, bool seedFirst)
        {
            SweepFilter::Settings s;
            s.shape = SweepFilter::Random;
            s.rateHz = 10.0f;
            SweepFilter filter;
            filter.prepare(c_fs);
            if (seedFirst) { filter.setSeed(seed); filter.reset(); }
            else           { filter.reset(); filter.setSeed(seed); }
            std::vector<float> v;
            for (int n = 0; n < 48000; n += 32)
            {
                const float value = filter.advance(s, 32);
                if ((n / 32) % 150 == 140) // late in each tenth of a second, once it has arrived
                    v.push_back(value);
            }
            return v;
        };
        const auto a = levels(1234u, false), b = levels(1234u, true), c = levels(99u, false);
        int distinct = 0;
        for (size_t i = 1; i < a.size(); ++i)
            distinct += a[i] != a[i - 1] ? 1 : 0;
        check(a == b && a != c && distinct == static_cast<int>(a.size()) - 1, "random: a fresh level each cycle, repeatable from its seed in either order of reset() and setSeed()");

        SweepFilter::Settings s;
        s.rateHz = 1.0f;
        SweepFilter filter;
        filter.prepare(c_fs);
        filter.scatter(0.25f);
        const float scattered = filter.advance(s, 32);
        filter.reset();
        const float top = filter.advance(s, 32);
        check(std::abs(scattered - 1.0f) < 0.001f && std::abs(top) < 0.01f && filter.getRhythm() == top, "scatter() starts the cycle elsewhere; reset() puts it back at the top");
    }

    std::cout << "A change of Type or slope, and lanes" << std::endl;
    {
        // A 100 Hz sine at 0.5 through a filter at 1 kHz: its steepest step is 0.0065 a sample. Switching must not
        // add a jump on top of that.
        double worstJump = 0.0;
        for (int from = 0; from < SweepFilter::c_types; ++from)
            for (int to = 0; to < SweepFilter::c_types; ++to)
                for (int steepTo = 0; steepTo < 2; ++steepTo)
                {
                    SweepFilter filter;
                    filter.prepare(c_fs);
                    SweepFilter::Settings s;
                    s.type = from;
                    float x[32], y[32];
                    float last = 0.0f;
                    for (int n = 0; n < 48000; n += 32)
                    {
                        if (n >= 24000)
                        {
                            s.type = to;
                            s.steep = steepTo != 0;
                        }
                        for (int i = 0; i < 32; ++i)
                            x[i] = 0.5f * static_cast<float>(std::sin(2.0 * c_pi * 100.0 * (n + i) / c_fs));
                        chunk(filter, s, 1000.0f, 0.7f, x, y, 32);
                        for (int i = 0; i < 32; ++i)
                        {
                            if (n >= 23000)
                                worstJump = std::max(worstJump, static_cast<double>(std::abs(y[i] - last)));
                            last = y[i];
                        }
                    }
                }
        std::cout << fmt("         32 switches at a steady cutoff: the largest step between two samples is %.4f (the sine's own is 0.0065)", worstJump) << std::endl;
        check(worstJump < 0.02, "a change of Type or slope crosses over without a jump");

        // Each lane has its own state, a lane left out keeps it, out may be x, and chunks shorter than 32 work.
        SweepFilter three, one;
        three.prepare(c_fs);
        one.prepare(c_fs);
        SweepFilter::Settings s;
        s.steep = true;
        sj::Random noise;
        noise.seed(5u);
        float worst = 0.0f;
        for (int n = 0, frames = 32; n < 48000; n += frames)
        {
            frames = 1 + static_cast<int>(noise.nextInt() % 32u);
            float x[32], lane0[32], lane2[32], alone[32];
            for (int i = 0; i < frames; ++i)
                x[i] = noise.bipolar();
            const float cutoff = 300.0f + 200.0f * std::sin(static_cast<float>(n) * 0.001f);
            three.advance(s, frames);
            three.setTarget(s, cutoff, 2.0f);
            three.process(0, x, lane0, frames);
            if ((n / 7) % 3 != 0) // lane 1 drops out now and then, with something else in it
            {
                float other[32];
                for (int i = 0; i < frames; ++i)
                    other[i] = 1.0f;
                three.process(1, other, other, frames);
            }
            for (int i = 0; i < frames; ++i)
                lane2[i] = x[i];
            three.process(2, lane2, lane2, frames); // in place
            three.endChunk();
            chunk(one, s, cutoff, 2.0f, x, alone, frames);
            for (int i = 0; i < frames; ++i)
                worst = std::max({ worst, std::abs(lane0[i] - alone[i]), std::abs(lane2[i] - alone[i]) });
        }
        check(worst == 0.0f, "lanes do not touch each other; in place and in chunks of any length up to 32");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : std::to_string(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

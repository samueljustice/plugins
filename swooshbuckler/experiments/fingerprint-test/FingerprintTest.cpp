// Fingerprint test: synthesises signals with known pitch / loudness and checks the analysis,
// the control curves, similarity, DTW alignment, region matching and speed. Exit code 0 = pass.

#include "Fingerprint.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
    constexpr double c_pi = 3.14159265358979323846;
    int g_failures = 0;

    void check(bool ok, const char* what)
    {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (! ok)
            ++g_failures;
    }

    /** Swell: sin^2 rise to the peak at peakFrac, cos^2 fall. u = 0..1. */
    double swell(double u, double peakFrac = 0.6)
    {
        if (u <= 0.0 || u >= 1.0)
            return 0.0;
        if (u < peakFrac)
            return std::pow(std::sin(0.5 * c_pi * u / peakFrac), 2.0);
        return std::pow(std::cos(0.5 * c_pi * (u - peakFrac) / (1.0 - peakFrac)), 2.0);
    }

    struct Gesture
    {
        double lead = 0.2, dur = 2.0, tail = 0.2;
        double f0 = 300.0, f1 = 900.0; // exponential chirp
        double hz(double t) const { return f0 * std::pow(f1 / f0, juce::jlimit(0.0, 1.0, (t - lead) / dur)); }
        double env(double t) const { return swell((t - lead) / dur); }
        double total() const { return lead + dur + tail; }
    };

    /** Harmonic chirp (6 partials, 1/k) under the swell. */
    std::vector<float> chirp(const Gesture& g, double sr)
    {
        const int n = static_cast<int>(g.total() * sr);
        std::vector<float> x(static_cast<size_t>(n));
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            phase += 2.0 * c_pi * g.hz(t) / sr;
            double s = 0.0;
            for (int k = 1; k <= 6; ++k)
                s += std::sin(k * phase) / k;
            x[static_cast<size_t>(i)] = static_cast<float>(0.4 * g.env(t) * s);
        }
        return x;
    }

    /** Band-passed noise (2-pole resonator at 1.5 kHz, Q 0.8) under the swell. */
    std::vector<float> noiseSwell(const Gesture& g, double sr, unsigned seed = 1)
    {
        const int n = static_cast<int>(g.total() * sr);
        std::vector<float> x(static_cast<size_t>(n));
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        const double w = 2.0 * c_pi * 1500.0 / sr, q = 0.8;
        const double alpha = std::sin(w) / (2.0 * q);
        const double b0 = alpha, a0 = 1 + alpha, a1 = -2 * std::cos(w), a2 = 1 - alpha;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (int i = 0; i < n; ++i)
        {
            const double in = u(rng);
            const double y = (b0 * in - b0 * x2 - a1 * y1 - a2 * y2) / a0;
            x2 = x1; x1 = in; y2 = y1; y1 = y;
            x[static_cast<size_t>(i)] = static_cast<float>(0.8 * g.env(i / sr) * y);
        }
        return x;
    }

    /** Click train: 2 ms decaying noise bursts every `period` seconds. */
    std::vector<float> clicks(double sr, double lead, int count, double period, std::vector<double>& times)
    {
        const int n = static_cast<int>((lead + count * period + 0.2) * sr);
        std::vector<float> x(static_cast<size_t>(n), 0.0f);
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        for (int c = 0; c < count; ++c)
        {
            const double t = lead + c * period;
            times.push_back(t);
            const int s = static_cast<int>(t * sr);
            for (int i = 0; i < static_cast<int>(0.004 * sr) && s + i < n; ++i)
                x[static_cast<size_t>(s + i)] = 0.7f * u(rng) * static_cast<float>(std::exp(-i / (0.0007 * sr)));
        }
        return x;
    }

    double pearson(const std::vector<double>& a, const std::vector<double>& b)
    {
        double ma = 0, mb = 0;
        for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
        ma /= static_cast<double>(a.size());
        mb /= static_cast<double>(a.size());
        double sab = 0, saa = 0, sbb = 0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            sab += (a[i] - ma) * (b[i] - mb);
            saa += (a[i] - ma) * (a[i] - ma);
            sbb += (b[i] - mb) * (b[i] - mb);
        }
        return sab / std::sqrt(saa * sbb + 1e-30);
    }

    /** Median relative pitch error and voiced coverage over frames where the truth envelope > 0.1. */
    void pitchError(const fp::Fingerprint& f, const Gesture& g, double& medianErr, double& coverage)
    {
        std::vector<double> errs;
        int total = 0;
        for (int i = 0; i < f.numFrames(); ++i)
        {
            const double t = i * f.hopSeconds;
            if (g.env(t) < 0.1)
                continue;
            ++total;
            if (f.pitchHz[static_cast<size_t>(i)] > 0.0f)
                errs.push_back(std::abs(f.pitchHz[static_cast<size_t>(i)] / g.hz(t) - 1.0));
        }
        std::sort(errs.begin(), errs.end());
        medianErr = errs.empty() ? 1.0 : errs[errs.size() / 2];
        coverage = total > 0 ? static_cast<double>(errs.size()) / total : 0.0;
    }

    /** Truth envelope (linear) at control-curve point p of a fingerprint's active range. */
    double truthAtPoint(const fp::Fingerprint& f, const Gesture& g, int p)
    {
        const auto& s = f.summary;
        const double frame = s.activeStart + static_cast<double>(p) / (fp::ControlCurves::c_points - 1) * (s.activeEnd - 1 - s.activeStart);
        return g.env(frame * f.hopSeconds);
    }
}

int main()
{
    const double sr = 48000.0;
    Gesture master;                         // 2 s, 300 -> 900 Hz, peak at 60%
    Gesture stretched = master;             // 1.5x longer, +3.86 st (x1.25)
    stretched.lead = 0.3; stretched.dur = 3.0; stretched.tail = 0.3;
    stretched.f0 = 375.0; stretched.f1 = 1125.0;

    const auto xMaster = chirp(master, sr);
    const auto xStretch = chirp(stretched, sr);
    const auto xNoise = noiseSwell(master, sr);
    std::vector<double> clickTimes;
    const auto xClicks = clicks(sr, 0.2, 16, 0.125, clickTimes);

    const auto fMaster = fp::analyse(xMaster.data(), static_cast<int>(xMaster.size()), sr);
    const auto fStretch = fp::analyse(xStretch.data(), static_cast<int>(xStretch.size()), sr);
    const auto fNoise = fp::analyse(xNoise.data(), static_cast<int>(xNoise.size()), sr);
    const auto fClicks = fp::analyse(xClicks.data(), static_cast<int>(xClicks.size()), sr);

    std::printf("Summaries (voiced, clarity, flatness, centroid, median f0, onset rate):\n");
    auto pr = [](const char* name, const fp::Fingerprint& f)
    {
        const auto& s = f.summary;
        std::printf("  %-10s %.2f  %.2f  %.4f  %6.0f Hz  %6.1f Hz  %5.1f/s  active %.2f..%.2f s\n", name, s.voicedRatio, s.meanClarity,
                    s.meanFlatness, s.meanCentroidHz, s.medianPitchHz, s.onsetRate, s.activeStart * f.hopSeconds, s.activeEnd * f.hopSeconds);
    };
    pr("chirp", fMaster);
    pr("stretched", fStretch);
    pr("noise", fNoise);
    pr("clicks", fClicks);

    // 1. Pitch contour --------------------------------------------------------
    std::printf("\n1. Pitch contour\n");
    {
        double e1, c1, e2, c2;
        pitchError(fMaster, master, e1, c1);
        pitchError(fStretch, stretched, e2, c2);
        std::printf("  chirp: median error %.3f%%, voiced coverage %.1f%%\n", 100 * e1, 100 * c1);
        std::printf("  stretched: median error %.3f%%, voiced coverage %.1f%%\n", 100 * e2, 100 * c2);
        check(e1 < 0.03 && e2 < 0.03, "median pitch error < 3%");
        check(c1 > 0.9 && c2 > 0.9, "voiced coverage > 90% where the swell is above -20 dB");
        check(fNoise.summary.voicedRatio < 0.1, "noise swell is (almost) unvoiced");

        const double sr44 = 44100.0;
        const auto x44 = chirp(master, sr44);
        const auto f44 = fp::analyse(x44.data(), static_cast<int>(x44.size()), sr44);
        double e3, c3;
        pitchError(f44, master, e3, c3);
        std::printf("  chirp @44.1k: median error %.3f%%, coverage %.1f%%\n", 100 * e3, 100 * c3);
        check(e3 < 0.03 && c3 > 0.9, "pitch also right at 44.1 kHz");
    }

    // 2. Envelope / control curves ---------------------------------------------
    std::printf("\n2. Control curves\n");
    {
        const auto cm = fp::makeControlCurves(fMaster, 1.0f);
        const auto cs = fp::makeControlCurves(fStretch, 1.0f);
        std::vector<double> est, truth, estS;
        for (int p = 0; p < fp::ControlCurves::c_points; ++p)
        {
            est.push_back(cm.envelope[static_cast<size_t>(p)]);
            truth.push_back(truthAtPoint(fMaster, master, p));
            estS.push_back(cs.envelope[static_cast<size_t>(p)]);
        }
        const double r = pearson(est, truth), rs = pearson(est, estS);
        double maxSemi = 0;
        for (int p = 0; p < fp::ControlCurves::c_points; ++p)
            if (cm.pitchConf[static_cast<size_t>(p)] > 0.5f && cs.pitchConf[static_cast<size_t>(p)] > 0.5f)
                maxSemi = std::max(maxSemi, static_cast<double>(std::abs(cm.pitchSemis[static_cast<size_t>(p)] - cs.pitchSemis[static_cast<size_t>(p)])));
        std::printf("  envelope vs truth r = %.4f, master vs stretched r = %.4f, max pitch-curve diff %.2f st\n", r, rs, maxSemi);
        std::printf("  peak phase %.3f (truth ~0.6), suggested shape %.2f, master %.2f s, pitch amount %.2f\n", cm.peakPhase,
                    cm.suggestedShape, cm.masterSeconds, cm.pitchAmount);
        std::printf("  pitch curve: start %.2f st, end %.2f st (truth span 19.02 st), brightness %.2f -> %.2f\n",
                    cm.pitchSemis[20], cm.pitchSemis[235], cm.brightness[20], cm.brightness[235]);
        check(r > 0.98, "envelope correlation with the true swell > 0.98");
        check(rs > 0.97, "stretched + transposed copy gives the same envelope curve (r > 0.97)");
        check(maxSemi < 1.0, "and the same relative pitch curve (< 1 st apart)");
        check(std::abs(cm.peakPhase - 0.6f) < 0.05f, "peak phase within 0.05 of the truth");
        check(cm.brightness[235] > cm.brightness[20], "brightness rises with the chirp");
        check(fp::ControlCurves::at(cm.envelope, 0.0f) == 0.0f && std::abs(fp::ControlCurves::at(cm.envelope, cm.peakPhase) - 1.0f) < 0.02f,
              "envelope 0 at the ends, 1 at the peak");
    }

    // 3. Similarity --------------------------------------------------------------
    std::printf("\n3. Similarity\n");
    {
        const fp::Fingerprint* f[] = { &fMaster, &fStretch, &fNoise, &fClicks };
        const char* names[] = { "chirp", "stretched", "noise", "clicks" };
        std::printf("  %-10s", "");
        for (auto* n : names) std::printf(" %9s", n);
        std::printf("\n");
        for (int i = 0; i < 4; ++i)
        {
            std::printf("  %-10s", names[i]);
            for (int j = 0; j < 4; ++j)
                std::printf(" %9.3f", fp::similarity(*f[i], *f[j]));
            std::printf("\n");
        }
        const float sStretch = fp::similarity(fMaster, fStretch);
        const float sNoise = fp::similarity(fMaster, fNoise);
        const float sClicks = fp::similarity(fMaster, fClicks);
        const float sSelf = fp::similarity(fMaster, fMaster);
        check(sSelf > 0.99f, "self-similarity ~1");
        check(sStretch > 0.7f, "stretched + transposed chirp > 0.7");
        check(sStretch - std::max(sNoise, sClicks) > 0.3f, "margin over noise / clicks > 0.3");
        check(std::abs(fp::similarity(fMaster, fNoise) - fp::similarity(fNoise, fMaster)) < 0.02f, "roughly symmetric");
    }

    // 4. DTW alignment -----------------------------------------------------------
    std::printf("\n4. DTW alignment (chirp -> stretched)\n");
    {
        const auto al = fp::align(fMaster, fStretch);
        double sum = 0, worst = 0;
        int n = 0;
        for (int i = 0; i < fMaster.numFrames(); ++i)
        {
            const double t = i * fMaster.hopSeconds;
            if (master.env(t) < 0.05)
                continue;
            const double expected = stretched.lead + (t - master.lead) * 1.5;
            const double got = al.bFrameForA[static_cast<size_t>(i)] * fStretch.hopSeconds;
            sum += std::abs(got - expected);
            worst = std::max(worst, std::abs(got - expected));
            ++n;
        }
        std::printf("  mean error %.1f ms, worst %.1f ms, stretch %.3f, cost %.3f\n", 1000 * sum / n, 1000 * worst, al.stretch, al.cost);
        check(sum / n < 0.03, "mean alignment error < 30 ms");
        check(worst < 0.10, "worst alignment error < 100 ms");
        check(std::abs(al.stretch - 1.5f) < 0.08f, "stretch ~1.5");
    }

    // 5. Region matching ---------------------------------------------------------
    std::printf("\n5. Region matching\n");
    {
        // Track: [noise swell] gap [stretched chirp] gap [clicks]
        std::vector<float> track;
        std::vector<fp::Span> spans;
        auto add = [&](const std::vector<float>& x)
        {
            fp::Span s;
            s.start = static_cast<int>(track.size());
            track.insert(track.end(), x.begin(), x.end());
            s.end = static_cast<int>(track.size());
            spans.push_back(s);
            track.insert(track.end(), static_cast<size_t>(0.5 * sr), 0.0f);
        };
        add(xNoise);
        add(xStretch);
        add(xClicks);
        const auto ft = fp::analyse(track.data(), static_cast<int>(track.size()), sr);
        const auto m = fp::matchRegions(fMaster, ft, spans);
        for (const auto& r : m)
            std::printf("  region %d score %.3f  start %.2f s  peak %.2f s  end %.2f s  stretch %.2f\n", r.region, r.score,
                        r.startSample / sr, r.peakSample / sr, r.endSample / sr, r.stretch);
        const double truePeak = spans[1].start / sr + stretched.lead + 0.6 * stretched.dur;
        check(! m.empty() && m[0].region == 1, "the stretched chirp region ranks first");
        check(! m.empty() && std::abs(m[0].peakSample / sr - truePeak) < 0.06, "its peak sample within 60 ms of the true peak");
        check(! m.empty() && std::abs(m[0].stretch - 1.5f) < 0.1f, "its stretch ~1.5");

        // One long continuous region: [noise][chirp][clicks] with no gaps -> subsequence search.
        std::vector<float> bed;
        bed.insert(bed.end(), xNoise.begin(), xNoise.end());
        const int chirpAt = static_cast<int>(bed.size());
        bed.insert(bed.end(), xMaster.begin(), xMaster.end());
        bed.insert(bed.end(), xClicks.begin(), xClicks.end());
        const auto fb = fp::analyse(bed.data(), static_cast<int>(bed.size()), sr);
        const auto mb = fp::matchRegions(fMaster, fb, { fp::Span { 0, static_cast<int>(bed.size()) } });
        const double bedPeak = chirpAt / sr + master.lead + 0.6 * master.dur;
        if (! mb.empty())
            std::printf("  long region: score %.3f  window %.2f..%.2f s  peak %.2f s (truth %.2f)  stretch %.2f\n", mb[0].score,
                        mb[0].startSample / sr, mb[0].endSample / sr, mb[0].peakSample / sr, bedPeak, mb[0].stretch);
        check(! mb.empty() && std::abs(mb[0].peakSample / sr - bedPeak) < 0.08, "subsequence DTW finds the chirp inside a long region (peak within 80 ms)");
    }

    // 6. Speed ---------------------------------------------------------------------
    std::printf("\n6. Speed\n");
    {
        Gesture ten;
        ten.lead = 0.0; ten.dur = 10.0; ten.tail = 0.0; ten.f0 = 120.0; ten.f1 = 1500.0;
        auto x = chirp(ten, sr);
        const auto nz = noiseSwell(ten, sr, 3);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] += 0.3f * nz[i];
        double best = 1e9;
        for (int k = 0; k < 5; ++k)
        {
            const auto t0 = std::chrono::steady_clock::now();
            const auto f = fp::analyse(x.data(), static_cast<int>(x.size()), sr);
            const auto t1 = std::chrono::steady_clock::now();
            best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
            juce::ignoreUnused(f);
        }
        const auto fx = fp::analyse(x.data(), static_cast<int>(x.size()), sr);
        const auto t0 = std::chrono::steady_clock::now();
        const float s = fp::similarity(fx, fMaster);
        const auto c = fp::makeControlCurves(fx, 1.6f);
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("  analyse 10 s @ 48 kHz: %.2f ms (best of 5); similarity + curves: %.2f ms (sim %.2f, valid %d)\n", best,
                    std::chrono::duration<double, std::milli>(t1 - t0).count(), s, c.valid ? 1 : 0);
        check(best < 50.0, "analysis < 50 ms per 10 s at 48 kHz");
    }

    // 7. Onsets ----------------------------------------------------------------------
    std::printf("\n7. Onsets (click train, 16 clicks)\n");
    {
        int hits = 0;
        double worst = 0;
        for (double t : clickTimes)
        {
            double bestD = 1e9;
            for (float o : fClicks.onsetsSeconds)
                bestD = std::min(bestD, std::abs(o - t));
            if (bestD < 0.015)
            {
                ++hits;
                worst = std::max(worst, bestD);
            }
        }
        std::printf("  detected %zu onsets, %d of 16 within 15 ms (worst %.1f ms); chirp onsets %zu, noise onsets %zu\n",
                    fClicks.onsetsSeconds.size(), hits, 1000 * worst, fMaster.onsetsSeconds.size(), fNoise.onsetsSeconds.size());
        check(hits >= 15 && fClicks.onsetsSeconds.size() <= 17, "click onsets found (>= 15 of 16, no extras)");
        check(fMaster.onsetsSeconds.size() <= 2, "a smooth swell has (almost) no onsets");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

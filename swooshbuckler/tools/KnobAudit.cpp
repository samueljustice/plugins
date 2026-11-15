// Knob audit: sweeps one parameter across its travel through the real plug-in (offline) and measures
// what each step does to the sound, so you can see which part of a knob's travel is useful and which is fluff.
//
//   SwooshbucklerAudit --param aDoppler [--steps 11 | --norms 0,0.25,1 | --plains -12,0,12]
//                      [--set id=plain ...] [--setn id=normalised ...] [--wire source:paramId:depth ...]
//                      [--src growl,wind,hits] [--seconds 20] [--reps 3] [--swell] [--wav dir] [--json file]
//                      [--record-rate 96000] [--bands]
//
// --record-rate renders with the Record Rate setting at that rate (the device stays at 48 kHz), so what is
// measured is the monitor / host output: the high-rate render low-passed, downsampled and held to the Ceiling.
// The reported latency is printed with the table.
//
// Sources (synthetic, mono, routed by their type): growl = gappy tonal growls (Bus A), wind = an energy bed
// (Bus B), hits = impacts (the hit gate), tone = a steady 330 Hz sine on Bus A (for pitch and distortion),
// air = a hissy bed on Bus B (for filters and width), rumble = a wideband rumble (heavy under 100 Hz, with hiss
// on top), which Stowaway takes by itself while it is on and its Mode is Sub; drone = a steady low drone (45 and
// 72 Hz with a little hiss) of the Rumble type too, which has no movement of its own, so what moves it is the plug-in.
// A source goes somewhere else with @: name@a (Bus A), name@b (Bus B), name@h (the hits), name@s (Stowaway),
// e.g. --src growl,wind,air@s.
// Stowaway starts switched off, as in a new session. A rumble, a drone or anything sent @s switches it on, so the
// source is heard where it is meant to be; --set stowOn=0 or 1 says otherwise (it is applied afterwards).
// --bands also prints each value's octave-band levels (dB against the whole), for filters.
//
// Each value is rendered `reps` times after different pre-rolls (so a different stretch of the rig's random
// passes is measured each time); the table shows the mean, and the step-to-step changes that are both larger
// than a just-noticeable difference and larger than the spread between the reps are marked with *.

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Parameters.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <vector>

namespace
{
    constexpr double c_fs = 48000.0;
    constexpr int c_block = 512;
    constexpr double c_twoPi = 6.283185307179586;

    // ---- sources ---------------------------------------------------------------------------------------
    juce::AudioBuffer<float> growl()
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 6.0));
        b.clear();
        double ph = 0.0;
        juce::Random r(3);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = i / c_fs;
            const double lt = std::fmod(t, 1.5);
            if (lt < 0.6)
            {
                ph += (110.0 + 40.0 * std::sin(6.0 * t)) / c_fs;
                const float env = static_cast<float>(std::sin(juce::MathConstants<double>::pi * lt / 0.6));
                float v = 0.0f;
                for (int h = 1; h < 8; ++h)
                    v += static_cast<float>(std::sin(c_twoPi * ph * h)) / static_cast<float>(h);
                b.setSample(0, i, 0.4f * env * std::tanh(2.0f * v) + 0.02f * env * (r.nextFloat() * 2.0f - 1.0f));
            }
        }
        return b;
    }

    juce::AudioBuffer<float> wind()
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 8.0));
        juce::Random r(5);
        float y = 0.0f;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            y += 0.06f * ((r.nextFloat() * 2.0f - 1.0f) - y);
            const double t = i / c_fs;
            b.setSample(0, i, 2.5f * y * static_cast<float>(0.75 + 0.25 * std::sin(1.3 * t)));
        }
        return b;
    }

    juce::AudioBuffer<float> air()
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 8.0));
        juce::Random r(11);
        float prev = 0.0f;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float w = r.nextFloat() * 2.0f - 1.0f;
            const double t = i / c_fs;
            b.setSample(0, i, 0.35f * (w - 0.7f * prev) * static_cast<float>(0.8 + 0.2 * std::sin(0.9 * t)));
            prev = w;
        }
        return b;
    }

    juce::AudioBuffer<float> rumble()
    {
        // Brown-ish noise (a corner near 50 Hz) that heaves slowly, with white hiss on top: mostly low end, but
        // sound all the way up, so what a filter lets through shows.
        juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 8.0));
        juce::Random r(17);
        float y = 0.0f;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float w = r.nextFloat() * 2.0f - 1.0f;
            y += 0.0065f * (w - y);
            const double t = i / c_fs;
            b.setSample(0, i, (7.0f * y + 0.03f * w) * static_cast<float>(0.7 + 0.3 * std::sin(0.8 * t)));
        }
        return b;
    }

    juce::AudioBuffer<float> drone()
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 8.0)); // whole cycles of both tones: it loops cleanly
        juce::Random r(19);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = i / c_fs;
            b.setSample(0, i, static_cast<float>(0.25 * std::sin(c_twoPi * 45.0 * t) + 0.18 * std::sin(c_twoPi * 72.0 * t))
                                  + 0.01f * (r.nextFloat() * 2.0f - 1.0f));
        }
        return b;
    }

    juce::AudioBuffer<float> hits()
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 2.0));
        b.clear();
        juce::Random r(9);
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < static_cast<int>(c_fs * 0.3); ++i)
            {
                const double t = i / c_fs;
                const int idx = static_cast<int>(k * 0.5 * c_fs) + i;
                b.setSample(0, idx, static_cast<float>(0.8 * std::exp(-t / 0.04) * (r.nextFloat() * 2.0f - 1.0f)
                                                     + 0.7 * std::exp(-t / 0.2) * std::sin(c_twoPi * 55.0 * t)));
            }
        return b;
    }

    juce::AudioBuffer<float> tone()
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 6.0));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample(0, i, 0.4f * static_cast<float>(std::sin(c_twoPi * 330.0 * i / c_fs)));
        return b;
    }

    juce::File writeWav(const juce::File& f, const juce::AudioBuffer<float>& b, double rate = c_fs)
    {
        // Written to a temporary file and renamed into place, so runs started in parallel never read a half-written source.
        const auto tmp = f.getSiblingFile(f.getFileNameWithoutExtension() + "." + juce::String(juce::Random::getSystemRandom().nextInt64()) + ".tmp");
        {
            juce::WavAudioFormat wav;
            auto* stream = new juce::FileOutputStream(tmp);
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream, rate, static_cast<unsigned int>(b.getNumChannels()), 24, {}, 0));
            if (w != nullptr) w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()); else delete stream;
        }
        tmp.moveFileTo(f);
        return f;
    }

    // ---- analysis --------------------------------------------------------------------------------------
    struct Biquad
    {
        double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
        double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    };

    double percentile(std::vector<double> v, double p)
    {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        const double idx = p * static_cast<double>(v.size() - 1);
        const auto lo = static_cast<size_t>(std::floor(idx));
        const auto hi = std::min(lo + 1, v.size() - 1);
        return v[lo] + (v[hi] - v[lo]) * (idx - static_cast<double>(lo));
    }

    double toDb(double power) { return 10.0 * std::log10(std::max(power, 1.0e-20)); }

    constexpr int c_numBands = 10; // octaves 31.5 Hz .. 16 kHz
    const double c_bandCentres[c_numBands] = { 31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

    // The low end in third-octaves (for --bands): fine enough to see a crossover move.
    constexpr int c_numThirds = 16;
    const double c_thirdCentres[c_numThirds] = { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630 };

    struct Metrics
    {
        // name, value
        std::map<std::string, double> v;
        double band[c_numBands] {};
        double third[c_numThirds] {}; // third-octave levels of the low end (dBFS), for --bands
    };

    // Metric names in print order, with the just-noticeable difference used to mark a step as audible.
    struct MetricInfo { const char* name; double jnd; const char* what; };
    const MetricInfo c_metrics[] = {
        { "lufs",     0.7,  "integrated loudness (LUFS)" },
        { "peak",     0.7,  "sample peak (dBFS)" },
        { "mov",      1.0,  "movement: momentary loudness P95-P10 (LU)" },
        { "gaps",     0.03, "fraction of 400 ms windows 20 LU under the integrated loudness" },
        { "cent",     60.0, "spectral centroid (Hz; step change judged in cents)" },
        { "lowR",     0.8,  "level of 31-125 Hz relative to the whole (dB)" },
        { "highR",    0.8,  "level of 4-16 kHz relative to the whole (dB)" },
        { "spec",     0.8,  "spectral shape change from the previous value (mean |dB| over octave bands)" },
        { "sm",       1.0,  "side / mid energy (dB)" },
        { "corr",     0.05, "left-right correlation" },
        { "pan",      0.03, "pan movement (std of left-right balance)" },
        { "am",       0.5,  "fast level wobble (dB std, faster than ~3 Hz)" },
        { "pRange",   15.0, "pitch range P5-P95 (cents; tone source)" },
        { "pMed",     10.0, "median pitch offset from 330 Hz (cents; tone source)" },
        { "nonFund",  1.0,  "energy outside the fundamental (dB re fundamental; tone source)" },
        { "gate",     0.03, "hit gate open fraction" },
        { "opens",    4.0,  "hit gate openings per minute" },
        { "onsets",   4.0,  "transients per minute" },
        { "grAvg",    0.7,  "limiter gain reduction, mean (dB)" },
        { "grP95",    0.7,  "limiter gain reduction, P95 (dB)" },
        { "stow",     0.7,  "Stowaway's own level, mean over its loud moments (dB)" },
        { "stowMov",  1.0,  "Stowaway's own movement: its level P95-P10 (dB)" },
    };

    struct Probe
    {
        std::vector<float> gate, gr; // per block
        std::vector<float> stow;     // per block: Stowaway's level (its envelope, linear)
    };

    Metrics analyse(const juce::AudioBuffer<float>& out, const Probe& probe, bool toneSource)
    {
        Metrics m;
        const int n = out.getNumSamples();
        const float* L = out.getReadPointer(0);
        const float* R = out.getReadPointer(1);

        // Loudness (BS.1770 K-weighting at 48 kHz).
        std::vector<double> sq(static_cast<size_t>(n));
        {
            Biquad s1[2], s2[2];
            for (int c = 0; c < 2; ++c)
            {
                s1[c] = { 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585 };
                s2[c] = { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 };
            }
            for (int i = 0; i < n; ++i)
            {
                const double l = s2[0].run(s1[0].run(L[i])), r = s2[1].run(s1[1].run(R[i]));
                sq[static_cast<size_t>(i)] = l * l + r * r;
            }
        }
        const int win = static_cast<int>(0.4 * c_fs), hop = static_cast<int>(0.1 * c_fs);
        std::vector<double> momentary, momentaryPower;
        for (int s = 0; s + win <= n; s += hop)
        {
            double acc = 0.0;
            for (int i = s; i < s + win; ++i) acc += sq[static_cast<size_t>(i)];
            const double pw = acc / win;
            momentaryPower.push_back(pw);
            momentary.push_back(-0.691 + toDb(pw));
        }
        double integrated = -70.0;
        {
            double acc = 0.0; int cnt = 0;
            for (size_t i = 0; i < momentary.size(); ++i) if (momentary[i] > -70.0) { acc += momentaryPower[i]; ++cnt; }
            if (cnt > 0)
            {
                const double rel = -0.691 + toDb(acc / cnt) - 10.0;
                acc = 0.0; cnt = 0;
                for (size_t i = 0; i < momentary.size(); ++i) if (momentary[i] > rel) { acc += momentaryPower[i]; ++cnt; }
                if (cnt > 0) integrated = -0.691 + toDb(acc / cnt);
            }
        }
        m.v["lufs"] = integrated;
        {
            std::vector<double> loud;
            for (double x : momentary) if (x > -70.0) loud.push_back(x);
            m.v["mov"] = loud.empty() ? 0.0 : percentile(loud, 0.95) - percentile(loud, 0.10);
            int g = 0;
            for (double x : momentary) if (x < integrated - 20.0) ++g;
            m.v["gaps"] = momentary.empty() ? 0.0 : static_cast<double>(g) / static_cast<double>(momentary.size());
        }

        // Peak.
        float pk = 0.0f;
        for (int i = 0; i < n; ++i) pk = std::max({ pk, std::abs(L[i]), std::abs(R[i]) });
        m.v["peak"] = 20.0 * std::log10(std::max(1.0e-9, static_cast<double>(pk)));

        // Average spectrum (L and R power), 4096 Hann.
        constexpr int order = 12, fftSize = 1 << order;
        juce::dsp::FFT fft(order);
        std::vector<double> spec(fftSize / 2 + 1, 0.0);
        std::vector<float> buf(2 * fftSize), window(fftSize);
        for (int i = 0; i < fftSize; ++i) window[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(c_twoPi * i / fftSize));
        for (int s = 0; s + fftSize <= n; s += fftSize / 2)
        {
            for (const float* x : { L, R })
            {
                std::fill(buf.begin(), buf.end(), 0.0f);
                for (int i = 0; i < fftSize; ++i) buf[static_cast<size_t>(i)] = x[s + i] * window[static_cast<size_t>(i)];
                fft.performFrequencyOnlyForwardTransform(buf.data(), true);
                for (int k = 0; k <= fftSize / 2; ++k) spec[static_cast<size_t>(k)] += static_cast<double>(buf[static_cast<size_t>(k)]) * buf[static_cast<size_t>(k)];
            }
        }
        const double binHz = c_fs / fftSize;
        double total = 0.0, weighted = 0.0;
        for (int k = 1; k <= fftSize / 2; ++k)
        {
            const double f = k * binHz;
            if (f < 20.0 || f > 20000.0) continue;
            total += spec[static_cast<size_t>(k)];
            weighted += f * spec[static_cast<size_t>(k)];
        }
        m.v["cent"] = total > 0.0 ? weighted / total : 0.0;
        for (int b = 0; b < c_numBands; ++b)
        {
            double e = 0.0;
            for (int k = 1; k <= fftSize / 2; ++k)
            {
                const double f = k * binHz;
                if (f >= c_bandCentres[b] / std::sqrt(2.0) && f < c_bandCentres[b] * std::sqrt(2.0)) e += spec[static_cast<size_t>(k)];
            }
            m.band[b] = toDb(e) - toDb(total);
        }
        m.v["lowR"] = toDb(std::pow(10.0, m.band[0] / 10) + std::pow(10.0, m.band[1] / 10) + std::pow(10.0, m.band[2] / 10));
        m.v["highR"] = toDb(std::pow(10.0, m.band[7] / 10) + std::pow(10.0, m.band[8] / 10) + std::pow(10.0, m.band[9] / 10));

        // The low end in third-octaves, from a longer transform (2.9 Hz bins).
        {
            constexpr int lowOrder = 14, lowSize = 1 << lowOrder;
            juce::dsp::FFT lowFft(lowOrder);
            std::vector<double> lowSpec(lowSize / 2 + 1, 0.0);
            std::vector<float> lowBuf(2 * lowSize);
            double windowPower = 0.0;
            int frames = 0;
            for (int s = 0; s + lowSize <= n; s += lowSize / 2)
            {
                std::fill(lowBuf.begin(), lowBuf.end(), 0.0f);
                windowPower = 0.0;
                for (int i = 0; i < lowSize; ++i)
                {
                    const double w = 0.5 - 0.5 * std::cos(c_twoPi * i / lowSize);
                    lowBuf[static_cast<size_t>(i)] = 0.5f * (L[s + i] + R[s + i]) * static_cast<float>(w);
                    windowPower += w * w;
                }
                lowFft.performFrequencyOnlyForwardTransform(lowBuf.data(), true);
                for (int k = 0; k <= lowSize / 2; ++k) lowSpec[static_cast<size_t>(k)] += static_cast<double>(lowBuf[static_cast<size_t>(k)]) * lowBuf[static_cast<size_t>(k)];
                ++frames;
            }
            for (int b = 0; b < c_numThirds; ++b)
            {
                double e = 0.0;
                for (int k = 1; k <= lowSize / 2; ++k)
                {
                    const double f = k * c_fs / lowSize;
                    if (f >= c_thirdCentres[b] * std::pow(2.0, -1.0 / 6.0) && f < c_thirdCentres[b] * std::pow(2.0, 1.0 / 6.0)) e += lowSpec[static_cast<size_t>(k)];
                }
                // Mean square of the band (the mid signal): a full-scale sine in the band reads -3 dB.
                m.third[b] = frames > 0 ? toDb(2.0 * e / (std::max(1, frames) * windowPower * lowSize)) : -200.0;
            }
        }

        if (toneSource)
        {
            int peakBin = 1;
            for (int k = static_cast<int>(100 / binHz); k < static_cast<int>(2000 / binHz); ++k)
                if (spec[static_cast<size_t>(k)] > spec[static_cast<size_t>(peakBin)]) peakBin = k;
            const double f0 = peakBin * binHz;
            double fund = 0.0;
            for (int k = 1; k <= fftSize / 2; ++k)
            {
                const double f = k * binHz;
                if (f > f0 * std::pow(2.0, -1.0 / 6.0) && f < f0 * std::pow(2.0, 1.0 / 6.0)) fund += spec[static_cast<size_t>(k)];
            }
            m.v["nonFund"] = toDb(std::max(total - fund, 1.0e-20)) - toDb(fund);
        }
        else
            m.v["nonFund"] = 0.0;

        // Stereo.
        double mm = 0, ss = 0, ll = 0, rr = 0, lr = 0;
        for (int i = 0; i < n; ++i)
        {
            const double l = L[i], r = R[i], mid = 0.5 * (l + r), side = 0.5 * (l - r);
            mm += mid * mid; ss += side * side; ll += l * l; rr += r * r; lr += l * r;
        }
        m.v["sm"] = toDb(ss) - toDb(mm);
        m.v["corr"] = (ll > 0 && rr > 0) ? lr / std::sqrt(ll * rr) : 1.0;

        // Pan movement and fast level wobble (5 ms frames).
        {
            const int f5 = static_cast<int>(0.005 * c_fs);
            std::vector<double> envDb, bal, balW;
            double maxEnv = -200.0;
            for (int s = 0; s + f5 <= n; s += f5)
            {
                double el = 0, er = 0;
                for (int i = s; i < s + f5; ++i)
                {
                    el += static_cast<double>(L[i]) * L[i];
                    er += static_cast<double>(R[i]) * R[i];
                }
                envDb.push_back(toDb((el + er) / (2.0 * f5)));
                maxEnv = std::max(maxEnv, envDb.back());
                bal.push_back((el + er) > 1.0e-12 ? (el - er) / (el + er) : 0.0);
                balW.push_back(el + er);
            }
            // Pan: energy-weighted std of the balance over 50 ms (10 frames) groups, loud groups only.
            {
                std::vector<double> gb, gw;
                for (size_t s = 0; s + 10 <= bal.size(); s += 10)
                {
                    double w = 0, b = 0;
                    for (size_t i = s; i < s + 10; ++i) { w += balW[i]; b += bal[i] * balW[i]; }
                    if (w > 0 && toDb(w / (20.0 * f5)) > maxEnv - 30.0) { gb.push_back(b / w); gw.push_back(w); }
                }
                double sw = 0, mean = 0, var = 0;
                for (size_t i = 0; i < gb.size(); ++i) { sw += gw[i]; mean += gb[i] * gw[i]; }
                if (sw > 0) { mean /= sw; for (size_t i = 0; i < gb.size(); ++i) var += gw[i] * (gb[i] - mean) * (gb[i] - mean); var /= sw; }
                m.v["pan"] = std::sqrt(var);
            }
            // Fast wobble: level minus its 0.3 s moving average, over loud frames.
            {
                const int half = 30;
                std::vector<double> dev;
                for (size_t i = half; i + half < envDb.size(); ++i)
                {
                    double avg = 0;
                    for (size_t j = i - half; j <= i + half; ++j) avg += envDb[j];
                    avg /= (2 * half + 1);
                    if (avg > maxEnv - 30.0) dev.push_back(envDb[i] - avg);
                }
                double mean = 0, var = 0;
                for (double d : dev) mean += d;
                if (! dev.empty()) { mean /= static_cast<double>(dev.size()); for (double d : dev) var += (d - mean) * (d - mean); var /= static_cast<double>(dev.size()); }
                m.v["am"] = std::sqrt(var);
            }
            // Onsets: a rise of 9 dB over the last 50 ms, near the top of the level, 80 ms apart.
            {
                int onsets = 0, lastOnset = -1000;
                for (size_t i = 10; i < envDb.size(); ++i)
                {
                    double mn = envDb[i];
                    for (size_t j = i - 10; j < i; ++j) mn = std::min(mn, envDb[j]);
                    if (envDb[i] - mn > 9.0 && envDb[i] > maxEnv - 30.0 && static_cast<int>(i) - lastOnset > 16)
                    {
                        ++onsets;
                        lastOnset = static_cast<int>(i);
                    }
                }
                m.v["onsets"] = onsets * 60.0 / (n / c_fs);
            }
        }

        // Pitch (tone source): rising zero crossings of the mid signal, 20 ms frames.
        if (toneSource)
        {
            std::vector<double> cents;
            const int fr = static_cast<int>(0.02 * c_fs);
            std::vector<double> mid(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) mid[static_cast<size_t>(i)] = 0.5 * (L[i] + R[i]);
            double maxRms = 0;
            std::vector<double> rmsF;
            for (int s = 0; s + fr <= n; s += fr)
            {
                double e = 0; for (int i = s; i < s + fr; ++i) e += mid[static_cast<size_t>(i)] * mid[static_cast<size_t>(i)];
                rmsF.push_back(std::sqrt(e / fr)); maxRms = std::max(maxRms, rmsF.back());
            }
            for (size_t f = 0; f < rmsF.size(); ++f)
            {
                if (rmsF[f] < maxRms * 0.03) continue;
                const int s = static_cast<int>(f) * fr;
                std::vector<double> crossings;
                for (int i = s + 1; i < s + fr; ++i)
                    if (mid[static_cast<size_t>(i - 1)] < 0.0 && mid[static_cast<size_t>(i)] >= 0.0)
                        crossings.push_back((i - 1) + mid[static_cast<size_t>(i - 1)] / (mid[static_cast<size_t>(i - 1)] - mid[static_cast<size_t>(i)]));
                if (crossings.size() < 3) continue;
                std::vector<double> periods;
                for (size_t k = 1; k < crossings.size(); ++k) periods.push_back(crossings[k] - crossings[k - 1]);
                double mean = std::accumulate(periods.begin(), periods.end(), 0.0) / static_cast<double>(periods.size());
                double var = 0; for (double p : periods) var += (p - mean) * (p - mean);
                var /= static_cast<double>(periods.size());
                if (std::sqrt(var) > 0.08 * mean) continue; // not a clean single tone in this frame
                cents.push_back(1200.0 * std::log2((c_fs / mean) / 330.0));
            }
            m.v["pRange"] = cents.size() > 10 ? percentile(cents, 0.95) - percentile(cents, 0.05) : 0.0;
            m.v["pMed"] = cents.size() > 10 ? percentile(cents, 0.5) : 0.0;
        }
        else
        {
            m.v["pRange"] = 0.0;
            m.v["pMed"] = 0.0;
        }

        // Probes.
        {
            int open = 0, opens = 0; bool isOpen = false;
            for (float g : probe.gate)
            {
                if (g > 0.5f) ++open;
                if (! isOpen && g > 0.5f) { isOpen = true; ++opens; }
                else if (isOpen && g < 0.3f) isOpen = false;
            }
            m.v["gate"] = probe.gate.empty() ? 0.0 : static_cast<double>(open) / static_cast<double>(probe.gate.size());
            m.v["opens"] = opens * 60.0 / (n / c_fs);
            std::vector<double> gr;
            for (float g : probe.gr) gr.push_back(std::abs(static_cast<double>(g)));
            m.v["grAvg"] = gr.empty() ? 0.0 : std::accumulate(gr.begin(), gr.end(), 0.0) / static_cast<double>(gr.size());
            m.v["grP95"] = percentile(gr, 0.95);
            // Stowaway, from the engine's own follower: the mean of the blocks within 40 dB of its loudest, and
            // how far it moves.
            double top = 0.0;
            for (float e : probe.stow) top = std::max(top, static_cast<double>(e));
            std::vector<double> stowDb;
            double power = 0.0;
            for (float e : probe.stow)
                if (top > 1.0e-6 && e > top * 0.01) { stowDb.push_back(20.0 * std::log10(static_cast<double>(e))); power += static_cast<double>(e) * e; }
            m.v["stow"] = stowDb.empty() ? -120.0 : 10.0 * std::log10(power / static_cast<double>(stowDb.size()));
            m.v["stowMov"] = stowDb.empty() ? 0.0 : percentile(stowDb, 0.95) - percentile(stowDb, 0.10);
        }
        return m;
    }

    // ---- rendering -------------------------------------------------------------------------------------
    struct Setup
    {
        std::vector<std::pair<juce::String, float>> plains, norms; // fixed settings
        struct Wire { int source; juce::String param; float depth; };
        std::vector<Wire> wires;
        std::vector<juce::String> sources { "growl", "wind", "hits" };
        double seconds = 20.0;
        int reps = 3;
        bool swell = false;
        int recordRate = 0; // 0 = the device rate
        juce::File work;
    };

    juce::RangedAudioParameter* findParam(juce::AudioProcessor& p, const juce::String& id)
    {
        for (auto* prm : p.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*>(prm))
                if (r->getParameterID() == id)
                    return r;
        return nullptr;
    }

    bool loadSources(SwooshbucklerAudioProcessor& proc, const Setup& setup)
    {
        int track = 0;
        for (const auto& s : setup.sources)
        {
            juce::String err;
            const auto name = s.upToFirstOccurrenceOf("@", false, false);
            const auto place = s.fromFirstOccurrenceOf("@", false, false);
            const auto f = setup.work.getChildFile("src_" + name + ".wav");
            if (! proc.getCargo().loadFile(track, f, err))
            {
                std::cerr << "can't load " << s << ": " << err << std::endl;
                return false;
            }
            CargoRole role = CargoRole::Tonal;
            if (name == "wind" || name == "air") role = CargoRole::Bed;
            else if (name == "hits") role = CargoRole::Impact;
            else if (name == "rumble" || name == "drone") role = CargoRole::Rumble;
            proc.getCargo().setRoleOverride(track, static_cast<int>(role));
            if (role == CargoRole::Rumble || place == "s")
                if (auto* on = proc.getState().getParameter(ParamIDs::stowOn))
                    on->setValueNotifyingHost(1.0f);
            if (place.isNotEmpty())
            {
                const int bus = place == "a" ? 0 : place == "b" ? 1 : place == "h" ? CargoHold::c_hitsBus : place == "s" ? CargoHold::c_stowBus : -1;
                if (bus < 0)
                {
                    std::cerr << "unknown place @" << place << " (a, b, h or s)" << std::endl;
                    return false;
                }
                proc.getCargo().setBusOverride(track, bus);
            }
            ++track;
        }
        return true;
    }

    Metrics render(const Setup& setup, const juce::String& paramId, float norm, int rep, const juce::File& wavOut)
    {
        std::unique_ptr<juce::AudioProcessor> base(createPluginFilter());
        auto& proc = *dynamic_cast<SwooshbucklerAudioProcessor*>(base.get());
        proc.setPlayConfigDetails(0, 2, c_fs, c_block);
        proc.overrideRecordRate(setup.recordRate); // never the saved setting: the measurements must not depend on it
        if (! loadSources(proc, setup))
            std::exit(2);

        for (const auto& [id, v] : setup.plains)
            if (auto* prm = findParam(proc, id)) prm->setValueNotifyingHost(prm->convertTo0to1(v));
            else std::cerr << "unknown parameter " << id << std::endl;
        for (const auto& [id, v] : setup.norms)
            if (auto* prm = findParam(proc, id)) prm->setValueNotifyingHost(v);
            else std::cerr << "unknown parameter " << id << std::endl;
        if (paramId.isNotEmpty())
        {
            if (auto* prm = findParam(proc, paramId)) prm->setValueNotifyingHost(norm);
            else { std::cerr << "unknown parameter " << paramId << std::endl; std::exit(2); }
        }
        for (const auto& w : setup.wires)
            if (proc.getMods().connect(w.source, w.param, w.depth) < 0)
                std::cerr << "can't wire " << w.param << std::endl;

        proc.prepareToPlay(c_fs, c_block);
        proc.setSwell(setup.swell);
        proc.setPlaying(true);

        juce::AudioBuffer<float> block(2, c_block);
        juce::MidiBuffer midi;
        // Pre-roll: a different stretch of the rig for each rep (the rig's randomness is seeded, so this is what
        // makes the reps independent).
        const double prerolls[] = { 3.0, 11.0, 23.0, 37.0, 53.0 };
        const int pre = static_cast<int>(prerolls[rep % 5] * c_fs) / c_block;
        for (int b = 0; b < pre; ++b) { block.clear(); proc.processBlock(block, midi); }

        const int total = static_cast<int>(setup.seconds * c_fs);
        juce::AudioBuffer<float> out(2, total);
        Probe probe;
        for (int pos = 0; pos < total; pos += c_block)
        {
            const int len = std::min(c_block, total - pos);
            juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, pos, len);
            view.clear();
            proc.processBlock(view, midi);
            probe.gate.push_back(proc.getEngine().getHitGate());
            probe.gr.push_back(proc.getGainReductionDb());
            probe.stow.push_back(proc.getEngine().getStowEnv());
        }
        proc.setPlaying(false);
        proc.releaseResources();
        if (wavOut != juce::File())
            writeWav(wavOut, out);
        bool toneSource = false;
        for (const auto& s : setup.sources)
            toneSource = toneSource || s.upToFirstOccurrenceOf("@", false, false) == "tone";
        return analyse(out, probe, toneSource);
    }

    std::vector<float> parseList(const juce::String& s)
    {
        std::vector<float> v;
        for (auto& t : juce::StringArray::fromTokens(s, ",", ""))
            v.push_back(t.trim().getFloatValue());
        return v;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    Setup setup;
    setup.work = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SwooshbucklerAudit");
    juce::String paramId;
    std::vector<float> norms, plains;
    int steps = 11;
    juce::File wavDir, jsonFile;
    bool printBands = false;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a(argv[i]);
        auto next = [&]() { return i + 1 < argc ? juce::String(argv[++i]) : juce::String(); };
        if (a == "--param") paramId = next();
        else if (a == "--steps") steps = next().getIntValue();
        else if (a == "--norms") norms = parseList(next());
        else if (a == "--plains") plains = parseList(next());
        else if (a == "--set" || a == "--setn")
        {
            const auto kv = next();
            const auto id = kv.upToFirstOccurrenceOf("=", false, false);
            const float v = kv.fromFirstOccurrenceOf("=", false, false).getFloatValue();
            (a == "--set" ? setup.plains : setup.norms).push_back({ id, v });
        }
        else if (a == "--wire")
        {
            auto parts = juce::StringArray::fromTokens(next(), ":", "");
            if (parts.size() == 3)
                setup.wires.push_back({ parts[0].getIntValue() - 1, parts[1], parts[2].getFloatValue() });
        }
        else if (a == "--src") { setup.sources.clear(); for (auto& s : juce::StringArray::fromTokens(next(), ",", "")) setup.sources.push_back(s.trim()); }
        else if (a == "--seconds") setup.seconds = next().getDoubleValue();
        else if (a == "--reps") setup.reps = std::max(1, next().getIntValue());
        else if (a == "--swell") setup.swell = true;
        else if (a == "--bands") printBands = true;
        else if (a == "--record-rate") setup.recordRate = next().getIntValue();
        else if (a == "--wav") wavDir = juce::File(next());
        else if (a == "--json") jsonFile = juce::File(next());
        else if (a == "--work") setup.work = juce::File(next());
        else { std::cerr << "unknown option " << a << std::endl; return 2; }
    }

    // Sources.
    setup.work.createDirectory();
    writeWav(setup.work.getChildFile("src_growl.wav"), growl());
    writeWav(setup.work.getChildFile("src_wind.wav"), wind());
    writeWav(setup.work.getChildFile("src_air.wav"), air());
    writeWav(setup.work.getChildFile("src_hits.wav"), hits());
    writeWav(setup.work.getChildFile("src_tone.wav"), tone());
    writeWav(setup.work.getChildFile("src_rumble.wav"), rumble());
    writeWav(setup.work.getChildFile("src_drone.wav"), drone());
    if (wavDir != juce::File()) wavDir.createDirectory();

    // The values to sweep, as knob positions (normalised).
    std::unique_ptr<juce::AudioProcessor> probeProc(createPluginFilter());
    {
        auto& probe = *dynamic_cast<SwooshbucklerAudioProcessor*>(probeProc.get());
        probe.setPlayConfigDetails(0, 2, c_fs, c_block);
        probe.overrideRecordRate(setup.recordRate);
        probe.prepareToPlay(c_fs, c_block);
        std::cout << "render rate " << probe.getRenderRate() << " Hz, device rate " << c_fs << " Hz, reported latency "
                  << probe.getLatencySamples() << " samples" << std::endl;
        probe.releaseResources();
    }
    juce::RangedAudioParameter* prm = paramId.isNotEmpty() ? findParam(*probeProc, paramId) : nullptr;
    if (paramId.isNotEmpty() && prm == nullptr) { std::cerr << "unknown parameter " << paramId << std::endl; return 2; }
    if (prm != nullptr && ! plains.empty())
        for (float p : plains) norms.push_back(prm->convertTo0to1(p));
    if (norms.empty())
    {
        if (prm == nullptr) norms.push_back(0.0f);
        else for (int s = 0; s < steps; ++s) norms.push_back(static_cast<float>(s) / static_cast<float>(std::max(1, steps - 1)));
    }

    std::cout << "param " << (paramId.isEmpty() ? juce::String("(none)") : paramId)
              << "  sources " << juce::StringArray([&] { juce::StringArray s; for (auto& x : setup.sources) s.add(x); return s; }()).joinIntoString(",")
              << "  " << setup.seconds << " s x " << setup.reps << " reps";
    for (auto& [id, v] : setup.plains) std::cout << "  " << id << "=" << v;
    for (auto& [id, v] : setup.norms) std::cout << "  " << id << "=n" << v;
    for (auto& w : setup.wires) std::cout << "  wire mod" << (w.source + 1) << "->" << w.param << "@" << w.depth;
    std::cout << std::endl;

    struct Row { float norm; juce::String text; std::map<std::string, double> mean, sd; double band[c_numBands] {}; double third[c_numThirds] {}; };
    std::vector<Row> rows;
    for (float nv : norms)
    {
        Row row { nv, prm != nullptr ? prm->getText(nv, 32) : juce::String("-"), {}, {} };
        std::vector<Metrics> ms;
        for (int r = 0; r < setup.reps; ++r)
        {
            juce::File wav;
            if (wavDir != juce::File() && r == 0)
                wav = wavDir.getChildFile(paramId + "_" + juce::String(juce::roundToInt(nv * 1000.0f)).paddedLeft('0', 4) + ".wav");
            ms.push_back(render(setup, paramId, nv, r, wav));
        }
        for (const auto& info : c_metrics)
        {
            double mean = 0; for (auto& m : ms) mean += m.v[info.name];
            mean /= static_cast<double>(ms.size());
            double var = 0; for (auto& m : ms) var += (m.v[info.name] - mean) * (m.v[info.name] - mean);
            row.mean[info.name] = mean;
            row.sd[info.name] = ms.size() > 1 ? std::sqrt(var / static_cast<double>(ms.size() - 1)) : 0.0;
        }
        for (int b = 0; b < c_numBands; ++b)
        {
            double mean = 0; for (auto& m : ms) mean += m.band[b];
            row.band[b] = mean / static_cast<double>(ms.size());
        }
        for (int b = 0; b < c_numThirds; ++b)
        {
            double mean = 0; for (auto& m : ms) mean += m.third[b];
            row.third[b] = mean / static_cast<double>(ms.size());
        }
        rows.push_back(row);
    }
    // Spectral shape change from the previous row.
    for (size_t i = 0; i < rows.size(); ++i)
    {
        double d = 0;
        if (i > 0) for (int b = 0; b < c_numBands; ++b) d += std::abs(rows[i].band[b] - rows[i - 1].band[b]);
        rows[i].mean["spec"] = d / c_numBands;
        rows[i].sd["spec"] = 0.0;
    }

    // Table.
    std::cout << std::fixed;
    std::cout << std::setw(6) << "knob" << std::setw(12) << "value";
    for (const auto& info : c_metrics) std::cout << std::setw(9) << info.name;
    std::cout << "\n";
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto& r = rows[i];
        std::cout << std::setw(5) << juce::roundToInt(r.norm * 100.0f) << "%" << std::setw(12) << r.text.substring(0, 11);
        for (const auto& info : c_metrics)
        {
            const double v = r.mean.at(info.name);
            bool audible = false;
            if (i > 0)
            {
                const auto& p = rows[i - 1];
                double d = v - p.mean.at(info.name);
                double jnd = info.jnd;
                if (std::string(info.name) == "cent") { d = (v > 0 && p.mean.at("cent") > 0) ? 1200.0 * std::log2(v / p.mean.at("cent")) : 0.0; }
                if (std::string(info.name) == "spec") d = v;
                const double noise = 2.0 * std::sqrt((r.sd.at(info.name) * r.sd.at(info.name) + p.sd.at(info.name) * p.sd.at(info.name)) / setup.reps);
                audible = std::abs(d) > jnd && std::abs(d) > noise;
            }
            const int prec = (std::string(info.name) == "gaps" || std::string(info.name) == "corr" || std::string(info.name) == "pan" || std::string(info.name) == "gate") ? 3
                           : (std::string(info.name) == "cent" || std::string(info.name) == "pRange" || std::string(info.name) == "pMed"
                              || std::string(info.name) == "opens" || std::string(info.name) == "onsets") ? 0 : 1;
            std::ostringstream cell;
            cell << std::fixed << std::setprecision(prec) << v << (audible ? "*" : " ");
            std::cout << std::setw(9) << cell.str();
        }
        std::cout << "\n";
    }
    std::cout << "rep spread (sd) at each value, worst metrics:";
    for (const auto& info : c_metrics)
    {
        double worst = 0; for (auto& r : rows) worst = std::max(worst, r.sd.at(info.name));
        if (worst > info.jnd * 0.5) std::cout << "  " << info.name << "=" << std::setprecision(2) << worst;
    }
    std::cout << std::endl;

    if (printBands)
    {
        std::cout << "octave bands (dB against the whole)\n" << std::setw(6) << "knob";
        for (double c : c_bandCentres) std::cout << std::setw(8) << std::setprecision(c < 100 ? 1 : 0) << c;
        std::cout << "\n";
        for (auto& r : rows)
        {
            std::cout << std::setw(5) << juce::roundToInt(r.norm * 100.0f) << "%";
            for (double b : r.band) std::cout << std::setw(8) << std::setprecision(1) << b;
            std::cout << "\n";
        }
        std::cout << "the low end in third-octaves (dBFS)\n" << std::setw(6) << "knob";
        for (double c : c_thirdCentres) std::cout << std::setw(8) << std::setprecision(c < 100 && c != std::floor(c) ? 1 : 0) << c;
        std::cout << "\n";
        for (auto& r : rows)
        {
            std::cout << std::setw(5) << juce::roundToInt(r.norm * 100.0f) << "%";
            for (double b : r.third) std::cout << std::setw(8) << std::setprecision(1) << b;
            std::cout << "\n";
        }
        std::cout << std::endl;
    }

    if (jsonFile != juce::File())
    {
        juce::Array<juce::var> arr;
        for (auto& r : rows)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("norm", r.norm);
            o->setProperty("text", r.text);
            auto* mean = new juce::DynamicObject();
            auto* sd = new juce::DynamicObject();
            for (auto& [k, v] : r.mean) mean->setProperty(juce::Identifier(k), v);
            for (auto& [k, v] : r.sd) sd->setProperty(juce::Identifier(k), v);
            juce::Array<juce::var> bands;
            for (double b : r.band) bands.add(b);
            o->setProperty("mean", juce::var(mean));
            o->setProperty("sd", juce::var(sd));
            o->setProperty("bands", bands);
            arr.add(juce::var(o));
        }
        jsonFile.replaceWithText(juce::JSON::toString(juce::var(arr)));
    }
    return 0;
}

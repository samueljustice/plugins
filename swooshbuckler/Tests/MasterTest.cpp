// Output stage test: the master (Inflate -> Push -> true-peak limiter with a programme-dependent release) and the
// band-limited downsampler of the monitor path. Plain C++, no JUCE: both classes are free of it.
//
//   SwooshbucklerMaster        prints what it measures and ends with "ALL PASSED" (exit code 0) or "n FAILED".

#include "Downsampler.h"
#include "MasterProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    constexpr double c_pi = 3.141592653589793;
    int g_failures = 0;

    void check(bool ok, const std::string& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }

    double toDb(double gain) { return 20.0 * std::log10(std::max(gain, 1.0e-12)); }

    // Small deterministic noise source (the same on every platform).
    struct Noise
    {
        std::uint32_t state;
        explicit Noise(std::uint32_t seed) : state(seed) {}
        float next()
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>((state >> 8) & 0xffffff) / 8388608.0f - 1.0f;
        }
    };

    double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 60; ++k)
        {
            term *= (0.5 * x / k) * (0.5 * x / k);
            sum += term;
        }
        return sum;
    }

    // Reference true peak: the samples and `oversample - 1` points between each pair, each read from 64 samples
    // with a Kaiser-windowed sinc (a longer, steeper reader than the limiter's own detector).
    double truePeak(const std::vector<float>& x, int oversample)
    {
        constexpr int zeros = 32;
        double peak = 0.0;
        for (const float v : x)
            peak = std::max(peak, static_cast<double>(std::abs(v)));
        for (int p = 1; p < oversample; ++p)
        {
            double h[2 * zeros];
            double total = 0.0;
            for (int j = 0; j < 2 * zeros; ++j)
            {
                const double d = (j - zeros + 1) - static_cast<double>(p) / oversample;
                const double r = d / zeros;
                h[j] = std::sin(c_pi * d) / (c_pi * d) * besselI0(9.0 * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(9.0);
                total += h[j];
            }
            for (size_t i = zeros; i + zeros + 1 < x.size(); ++i)
            {
                double acc = 0.0;
                for (int j = 0; j < 2 * zeros; ++j)
                    acc += h[j] * x[i - zeros + 1 + static_cast<size_t>(j)];
                peak = std::max(peak, std::abs(acc / total));
            }
        }
        return peak;
    }

    double samplePeak(const std::vector<float>& x)
    {
        double peak = 0.0;
        for (const float v : x)
            peak = std::max(peak, static_cast<double>(std::abs(v)));
        return peak;
    }

    // Level of one frequency in x[from, to) (Hann window; a full-scale sine reads 1).
    double levelAt(const std::vector<float>& x, int from, int to, double cyclesPerSample)
    {
        double re = 0.0, im = 0.0, norm = 0.0;
        for (int i = from; i < to; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2.0 * c_pi * (i - from) / (to - from));
            re += w * x[static_cast<size_t>(i)] * std::cos(2.0 * c_pi * cyclesPerSample * i);
            im += w * x[static_cast<size_t>(i)] * std::sin(2.0 * c_pi * cyclesPerSample * i);
            norm += w;
        }
        return 2.0 * std::sqrt(re * re + im * im) / norm;
    }

    MasterProcessor::Parameters limiterOnly(float ceilingDb = -0.3f)
    {
        MasterProcessor::Parameters p;
        p.limiterOn = true;
        p.ceilingDb = ceilingDb;
        return p;
    }

    // The plug-in's own release settings (see SwooshbucklerAudioProcessor::updateEngineParams).
    MasterProcessor::Parameters pluginRelease(float pushDb)
    {
        auto p = limiterOnly();
        p.releaseMs = 20.0f;
        p.sustainReleaseMs = 1000.0f;
        p.pushDb = pushDb;
        return p;
    }

    void run(MasterProcessor& m, std::vector<float>& x, int block = 512)
    {
        for (size_t pos = 0; pos < x.size(); pos += static_cast<size_t>(block))
        {
            float* ch[1] = { x.data() + pos };
            m.process(ch, 1, static_cast<int>(std::min(static_cast<size_t>(block), x.size() - pos)));
        }
    }

    std::vector<float> runFresh(double rate, const MasterProcessor::Parameters& p, std::vector<float> x, int block = 512)
    {
        MasterProcessor m;
        m.prepare(rate, 1);
        m.setParameters(p);
        run(m, x, block);
        return x;
    }

    // Worst-case material for peaks between the samples: full-band noise bursts (hits) over a noise bed, then a
    // sine sweep whose peaks fall between the samples.
    std::vector<float> hitsAndSweep(double rate)
    {
        const int n = static_cast<int>(rate * 8.0);
        std::vector<float> x(static_cast<size_t>(n));
        Noise noise(5);
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            if (i < n / 2)
            {
                const double t = std::fmod(i / rate, 0.5);
                const double env = t < 0.15 ? std::exp(-t * 25.0) : 0.0;
                x[static_cast<size_t>(i)] = static_cast<float>(env) * noise.next() + 0.1f * noise.next();
            }
            else
            {
                const double t = (i - n / 2) / rate;
                phase += 0.05 + 0.44 * t / 4.0;
                x[static_cast<size_t>(i)] = static_cast<float>(0.9 * std::sin(2.0 * c_pi * phase + 0.7));
            }
        }
        return x;
    }

    double thdPercent(double rate, const MasterProcessor::Parameters& p, double hz)
    {
        const int n = static_cast<int>(rate * 4.0);
        std::vector<float> x(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            x[static_cast<size_t>(i)] = static_cast<float>(std::sin(2.0 * c_pi * hz * i / rate));
        x = runFresh(rate, p, x);
        const double fundamental = levelAt(x, n / 2, n, hz / rate);
        double harmonics = 0.0;
        for (int k = 2; k <= 9; ++k)
        {
            const double v = levelAt(x, n / 2, n, k * hz / rate);
            harmonics += v * v;
        }
        return 100.0 * std::sqrt(harmonics) / fundamental;
    }
}

int main()
{
    const double rate = 48000.0;
    const double ceiling = std::pow(10.0, -0.3 / 20.0);

    //==============================================================================
    std::cout << "true-peak limiting" << std::endl;
    for (const double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const auto out = runFresh(fs, pluginRelease(6.0f), hitsAndSweep(fs));
        const double sample = toDb(samplePeak(out)), tp4 = toDb(truePeak(out, 4)), tp8 = toDb(truePeak(out, 8));
        std::cout << "  " << fs << " Hz, hits and a sweep pushed 6 dB into a -0.3 dB ceiling: sample peak " << sample
                  << " dBFS, true peak " << tp4 << " dBTP (4x), " << tp8 << " dBTP (8x)" << std::endl;
        check(samplePeak(out) <= ceiling, "no sample over the ceiling");
        check(tp4 <= -0.3 + 0.1, "the peaks between the samples (4x) stay within 0.1 dB of the ceiling");
        check(tp8 <= -0.3 + 0.35, "and within 0.35 dB read at 8x");
    }
    {
        // The detector on its own: a sine at a quarter of the sample rate whose samples all miss the crest by 3 dB
        // (the crest sits half way between the sample the reading belongs to and the next one).
        float window[MasterProcessor::c_detectorTaps];
        const double crest = MasterProcessor::c_detectorTaps / 2 - 0.5;
        for (int j = 0; j < MasterProcessor::c_detectorTaps; ++j)
            window[j] = static_cast<float>(std::cos(0.5 * c_pi * (j - crest)));
        const float peak = MasterProcessor::detectPeak(window);
        std::cout << "  a sine sampled 3 dB under its crest reads " << toDb(peak) << " dB" << std::endl;
        check(std::abs(toDb(peak)) < 0.05, "the detector finds a crest that falls between the samples");
    }

    //==============================================================================
    std::cout << "latency" << std::endl;
    for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        MasterProcessor m;
        m.prepare(fs, 1);
        const int latency = m.getLatencySamples();
        const int expected = static_cast<int>(std::lround(0.0015 * fs)) + MasterProcessor::c_detectorTaps / 2;

        // With the limiter resting, the output is the input, bit for bit, exactly `latency` samples later, for
        // quiet and for limited material alike (the timing never moves with the gain).
        bool exact = true;
        Noise noise(11);
        std::vector<float> in(static_cast<size_t>(fs)), out;
        for (auto& v : in)
            v = 0.25f * noise.next();
        out = runFresh(fs, pluginRelease(0.0f), in, 97);
        for (size_t i = static_cast<size_t>(latency); i < out.size(); ++i)
            exact = exact && out[i] == in[i - static_cast<size_t>(latency)];

        // Driven hard, an isolated click still comes out at the same place.
        std::vector<float> click(static_cast<size_t>(fs / 4), 0.0f);
        const size_t at = click.size() / 2;
        click[at] = 4.0f;
        const auto limited = runFresh(fs, pluginRelease(12.0f), click, 61);
        size_t loudest = 0;
        for (size_t i = 0; i < limited.size(); ++i)
            if (std::abs(limited[i]) > std::abs(limited[loudest]))
                loudest = i;

        std::cout << "  " << fs << " Hz: " << latency << " samples" << std::endl;
        check(latency == expected, "latency is the look-ahead plus the detector's half length");
        check(exact, "under the ceiling the output is the input, exactly that much later");
        check(loudest == at + static_cast<size_t>(latency), "a limited click comes out at the same delay");
    }

    //==============================================================================
    std::cout << "inflate, then push, then the limiter" << std::endl;
    {
        // A quiet sine, full Inflate, Push +6 dB: the result must be the inflated sine times two (Inflate works on
        // the signal at its own level, Push drives what comes out of it), not the inflated doubled sine.
        MasterProcessor::Parameters p = limiterOnly(0.0f);
        p.inflateEffect = 1.0f;
        p.pushDb = 6.0f;
        const float push = std::exp(6.0f * 0.115129255f);
        MasterProcessor m;
        m.prepare(rate, 1);
        const int latency = m.getLatencySamples();
        std::vector<float> in(4800);
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = 0.25f * static_cast<float>(std::sin(2.0 * c_pi * 440.0 * static_cast<double>(i) / rate));
        const auto out = runFresh(rate, p, in);
        double worstNew = 0.0, worstOld = 0.0;
        for (size_t i = static_cast<size_t>(latency); i < out.size(); ++i)
        {
            const float x = in[i - static_cast<size_t>(latency)];
            worstNew = std::max(worstNew, static_cast<double>(std::abs(out[i] - push * MasterProcessor::inflateTransfer(x, 0.0f))));
            worstOld = std::max(worstOld, static_cast<double>(std::abs(out[i] - MasterProcessor::inflateTransfer(push * x, 0.0f))));
        }
        std::cout << "  furthest from push x inflate(x): " << worstNew << "; from inflate(push x x): " << worstOld << std::endl;
        check(worstNew < 1.0e-5 && worstOld > 0.02, "Push comes after Inflate");

        // So Inflate lifts a quiet signal by the same amount wherever Push is (measured under the ceiling).
        auto liftDb = [&](float pushDb)
        {
            MasterProcessor::Parameters q = limiterOnly(0.0f);
            q.pushDb = pushDb;
            std::vector<float> quiet(in);
            for (auto& v : quiet)
                v *= 0.2f;
            const double dry = levelAt(runFresh(rate, q, quiet), 1200, 4800, 440.0 / rate);
            q.inflateEffect = 1.0f;
            return toDb(levelAt(runFresh(rate, q, quiet), 1200, 4800, 440.0 / rate) / dry);
        };
        const double lift0 = liftDb(0.0f), lift12 = liftDb(12.0f);
        std::cout << "  Inflate lifts a quiet sine by " << lift0 << " dB at Push 0 and " << lift12 << " dB at Push +12" << std::endl;
        check(std::abs(lift0 - lift12) < 0.01 && lift0 > 3.0, "Inflate does the same at any Push");
    }

    //==============================================================================
    std::cout << "programme-dependent release" << std::endl;
    {
        // A hit over a bed: a 1 kHz tone at -14 dBFS with a 10 ms noise burst 12 dB over full scale on top.
        const int n = static_cast<int>(rate * 2.0);
        std::vector<float> x(static_cast<size_t>(n));
        Noise noise(3);
        for (int i = 0; i < n; ++i)
        {
            x[static_cast<size_t>(i)] = 0.2f * static_cast<float>(std::sin(2.0 * c_pi * 1000.0 * i / rate));
            const double t = i / rate - 1.0;
            if (t >= 0.0 && t < 0.010)
                x[static_cast<size_t>(i)] += 4.0f * noise.next() * static_cast<float>(std::exp(-t * 200.0));
        }
        MasterProcessor probe;
        probe.prepare(rate, 1);
        const int latency = probe.getLatencySamples();
        auto bedAfter = [&](const MasterProcessor::Parameters& p, double seconds)
        {
            const auto out = runFresh(rate, p, x);
            const double before = levelAt(out, static_cast<int>(0.5 * rate), static_cast<int>(0.9 * rate), 1000.0 / rate);
            const int from = latency + static_cast<int>((1.0 + seconds) * rate);
            return toDb(levelAt(out, from, from + 480, 1000.0 / rate) / before);
        };
        auto single = limiterOnly();
        single.releaseMs = 80.0f;
        const double at40 = bedAfter(pluginRelease(0.0f), 0.040), at100 = bedAfter(pluginRelease(0.0f), 0.100), at400 = bedAfter(pluginRelease(0.0f), 0.400);
        std::cout << "  the bed after a hit 12 dB over: " << at40 << " dB at 40 ms, " << at100 << " dB at 100 ms, " << at400
                  << " dB at 400 ms (one 80 ms release: " << bedAfter(single, 0.040) << " / " << bedAfter(single, 0.100) << " / "
                  << bedAfter(single, 0.400) << " dB)" << std::endl;
        check(at40 > -1.5 && at100 > -0.3 && at400 > -0.1, "a short over leaves no hole: the bed is back within 1.5 dB after 40 ms and stays back");

        // Sustained bass, 12 dB over: the slow stage holds it, so the gain is not let go and caught every cycle.
        auto fastOnly = pluginRelease(12.0f);
        fastOnly.sustainReleaseMs = 0.0f;
        single.pushDb = 12.0f;
        const double bass = thdPercent(rate, pluginRelease(12.0f), 40.0), bassFast = thdPercent(rate, fastOnly, 40.0),
                     bassSingle = thdPercent(rate, single, 40.0), mid = thdPercent(rate, pluginRelease(12.0f), 330.0);
        std::cout << "  distortion of a sine 12 dB over: 40 Hz " << bass << " % (fast stage alone " << bassFast << " %, one 80 ms release "
                  << bassSingle << " %), 330 Hz " << mid << " %" << std::endl;
        check(bass < 2.5 && bass < 0.5 * bassFast, "sustained bass is held by the slow stage (under 2.5 % distortion at 40 Hz)");
        check(mid < 0.01, "a steady tone over the ceiling is limited without distortion");

        // Once the loud part ends, lasting reduction lets go slowly (not at the speed of the fast stage).
        const int half = static_cast<int>(rate * 2.0);
        std::vector<float> swell(static_cast<size_t>(2 * half));
        for (int i = 0; i < 2 * half; ++i)
            swell[static_cast<size_t>(i)] = (i < half ? 1.0f : 0.1f) * static_cast<float>(std::sin(2.0 * c_pi * 500.0 * i / rate));
        const auto out = runFresh(rate, pluginRelease(12.0f), swell);
        const int after = latency + half;
        auto tailDb = [&](double seconds)
        {
            const int from = after + static_cast<int>(seconds * rate);
            return toDb(levelAt(out, from, from + 960, 500.0 / rate) / (0.1 * std::pow(10.0, 12.0 / 20.0)));
        };
        std::cout << "  after two seconds 12 dB over, the gain is at " << tailDb(0.1) << " dB after 100 ms, " << tailDb(1.0) << " dB after 1 s, "
                  << tailDb(1.9) << " dB after 1.9 s" << std::endl;
        check(tailDb(0.1) < -6.0 && tailDb(1.0) > -4.5 && tailDb(1.9) > -1.5, "reduction that lasted lets go over about a second");
    }

    //==============================================================================
    std::cout << "gain reduction meter" << std::endl;
    {
        MasterProcessor m;
        m.prepare(rate, 1);
        m.setParameters(pluginRelease(0.0f));
        std::vector<float> x(4800, 0.0f);
        x[100] = 2.0f; // 6.3 dB over, early in the first of many blocks
        float deepestBlock = 0.0f;
        for (size_t pos = 0; pos < x.size(); pos += 64)
        {
            float* ch[1] = { x.data() + pos };
            m.process(ch, 1, 64);
            deepestBlock = std::min(deepestBlock, m.getGainReductionDb());
        }
        const float lastBlock = m.getGainReductionDb();
        const float held = m.takeGainReductionDb();
        const float again = m.takeGainReductionDb();
        std::cout << "  a click 6.3 dB over, 75 blocks ago: the last block reads " << lastBlock << " dB, the hold " << held
                  << " dB, and after reading it " << again << " dB" << std::endl;
        check(lastBlock > -0.1f && held < -6.0f && std::abs(held - deepestBlock) < 1.0e-6f, "the hold keeps the deepest reduction until it is read");
        check(again > -1.0e-6f, "reading the hold starts it again");
    }

    //==============================================================================
    std::cout << "downsampler (monitor path when the rig renders above the device rate)" << std::endl;
    struct Case { double inRate, outRate; };
    for (const Case c : { Case { 96000.0, 48000.0 }, Case { 192000.0, 48000.0 }, Case { 96000.0, 44100.0 }, Case { 88200.0, 48000.0 } })
    {
        const double ratio = c.inRate / c.outRate;
        const int numOut = static_cast<int>(c.outRate);

        auto downsample = [&](const std::vector<float>& in, int block, long long* consumed = nullptr)
        {
            Downsampler d;
            d.prepare(ratio, 1);
            std::vector<float> out(static_cast<size_t>(numOut));
            size_t inPos = 0;
            long long firstBlockInputs = -1;
            for (int pos = 0; pos < numOut; pos += block)
            {
                const int len = std::min(block, numOut - pos);
                const int needed = d.inputsNeeded(len);
                if (firstBlockInputs < 0)
                    firstBlockInputs = needed;
                const float* inCh[1] = { in.data() + inPos };
                float* outCh[1] = { out.data() + pos };
                d.process(inCh, outCh, len);
                inPos += static_cast<size_t>(needed);
            }
            if (consumed != nullptr)
                *consumed = static_cast<long long>(inPos);
            return out;
        };
        auto tone = [&](double hz)
        {
            std::vector<float> in(static_cast<size_t>(numOut * ratio) + 4096);
            for (size_t i = 0; i < in.size(); ++i)
                in[i] = static_cast<float>(std::sin(2.0 * c_pi * hz * static_cast<double>(i) / c.inRate));
            return in;
        };

        // In band: level and timing (output k is input k x ratio).
        const double passHz = 0.3 * c.outRate;
        long long consumed = 0;
        const auto pass = downsample(tone(passHz), 480, &consumed);
        double worst = 0.0;
        for (int k = 2000; k < numOut; ++k)
            worst = std::max(worst, std::abs(pass[static_cast<size_t>(k)] - std::sin(2.0 * c_pi * passHz * k / c.outRate)));

        // Above the output's Nyquist frequency: what is left of it after downsampling, wherever it lands.
        double leak = 0.0;
        for (const double f : { 0.56, 0.62, 0.75, 0.9 })
            if (f * c.outRate < 0.49 * c.inRate)
                leak = std::max(leak, std::sqrt(2.0) * [&] {
                    const auto out = downsample(tone(f * c.outRate), 512);
                    double sum = 0.0;
                    for (int k = 2000; k < numOut; ++k)
                        sum += static_cast<double>(out[static_cast<size_t>(k)]) * out[static_cast<size_t>(k)];
                    return std::sqrt(sum / (numOut - 2000));
                }());

        // Different block sizes give the same samples, and the input used matches the reported latency.
        const auto other = downsample(tone(passHz), 37);
        bool same = true;
        for (int k = 0; k < numOut; ++k)
            same = same && pass[static_cast<size_t>(k)] == other[static_cast<size_t>(k)];
        Downsampler d;
        d.prepare(ratio, 1);
        const double ahead = static_cast<double>(consumed) / ratio - (numOut - 1) - 1.0; // input used beyond the last output's own sample

        std::cout << "  " << c.inRate << " -> " << c.outRate << " Hz: a tone at 0.3 x the output rate is off by at most " << worst
                  << "; tones above the output's Nyquist frequency come out at " << toDb(leak) << " dB; latency "
                  << d.getLatencySamples() << " output samples" << std::endl;
        check(worst < 2.0e-3, "in-band audio passes at its level and in time");
        check(toDb(leak) < -70.0, "what is above the output's Nyquist frequency is removed (under -70 dB), not folded back");
        check(same, "the result does not depend on the block size");
        check(std::abs(ahead - d.getLatencySamples()) < 1.0, "the input it runs ahead by matches the reported latency");
        if (std::abs(ratio - std::round(ratio)) < 1.0e-9)
            check(std::abs(d.getLatencySamples() - std::round(d.getLatencySamples())) < 1.0e-9 && std::abs(ahead - d.getLatencySamples()) < 1.0e-9,
                  "a whole-number ratio has a whole-number latency, exactly");
    }
    {
        // The whole high-rate path: limit at 96 kHz, downsample to 48 kHz, hold the ceiling again.
        const auto limited = runFresh(96000.0, pluginRelease(9.0f), hitsAndSweep(96000.0));
        Downsampler d;
        d.prepare(2.0, 1);
        std::vector<float> down(limited.size() / 2 - 512);
        size_t inPos = 0;
        for (size_t pos = 0; pos < down.size(); pos += 256)
        {
            const int len = static_cast<int>(std::min<size_t>(256, down.size() - pos));
            const int needed = d.inputsNeeded(len);
            const float* inCh[1] = { limited.data() + inPos };
            float* outCh[1] = { down.data() + pos };
            d.process(inCh, outCh, len);
            inPos += static_cast<size_t>(needed);
        }
        auto monitor = limiterOnly();
        monitor.releaseMs = 50.0f;
        const auto out = runFresh(48000.0, monitor, down);
        std::cout << "  limited at 96 kHz then downsampled to 48 kHz: peak " << toDb(samplePeak(down)) << " dBFS; after the monitor limiter "
                  << toDb(samplePeak(out)) << " dBFS (true peak " << toDb(truePeak(out, 4)) << " dBTP)" << std::endl;
        check(samplePeak(out) <= ceiling && toDb(truePeak(out, 4)) <= -0.3 + 0.1, "the device output keeps the ceiling when the rig renders above the device rate");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : std::to_string(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

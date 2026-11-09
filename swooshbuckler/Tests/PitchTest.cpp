// Offline test for the track pitch shifter (PitchShifter: pitch changes, duration stays).
//
//   SwooshbucklerPitch
//
// Runs at 48 kHz and 96 kHz and checks: ratio 1 is a pure delay of the reported latency,
// sines land on the right pitch (octave up, octave down, a fifth up), a tone burst keeps
// its length, the level holds, clicks stay sharp when shifted, a ratio sweep stays finite
// and bounded, the output does not depend on how the audio is chunked, leaving a shift
// gets back to the exact bypass, and ten instances run far faster than real time.
// Writes a few WAVs to the current directory for listening (pitch_up12.wav etc.).

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "PitchShifter.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
    int g_failures = 0;
    constexpr double c_twoPi = 6.283185307179586;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }

    using Signal = std::vector<float>;

    void writeWav(const juce::String& name, const Signal& s, double rate)
    {
        auto f = juce::File::getCurrentWorkingDirectory().getChildFile(name + ".wav");
        f.deleteFile();
        juce::AudioBuffer<float> b(1, static_cast<int>(s.size()));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample(0, i, s[static_cast<size_t>(i)]);
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(f);
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream, rate, 1, 24, {}, 0));
        if (w != nullptr) w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()); else delete stream;
    }

    // Runs a fresh shifter over the whole signal in `chunk`-sample calls at a fixed ratio.
    Signal run(double rate, const Signal& in, float ratio, int chunk = 32)
    {
        PitchShifter ps;
        ps.prepare(rate);
        Signal out(in.size());
        const int total = static_cast<int>(in.size());
        for (int i = 0; i < total; i += chunk)
        {
            const int n = std::min(chunk, total - i);
            ps.process(in.data() + i, out.data() + i, n, ratio);
        }
        return out;
    }

    Signal sine(double rate, double hz, double seconds, float amp = 0.5f, double start = 0.0, double length = -1.0)
    {
        Signal s(static_cast<size_t>(rate * seconds), 0.0f);
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double t = static_cast<double>(i) / rate;
            if (t >= start && (length < 0.0 || t < start + length))
                s[i] = amp * static_cast<float>(std::sin(c_twoPi * hz * (t - start)));
        }
        return s;
    }

    Signal noise(double rate, double seconds, int seed, float amp = 0.3f)
    {
        juce::Random r(seed);
        Signal s(static_cast<size_t>(rate * seconds));
        for (auto& v : s)
            v = amp * (r.nextFloat() * 2.0f - 1.0f);
        return s;
    }

    float rms(const Signal& s, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += static_cast<double>(s[i]) * s[i];
        return static_cast<float>(std::sqrt(sum / static_cast<double>(std::max<size_t>(1, to - from))));
    }

    // Frequency from upward zero crossings (linearly interpolated) over [from, to).
    double zeroCrossingHz(const Signal& s, size_t from, size_t to, double rate)
    {
        double first = -1.0, last = -1.0;
        int count = 0;
        for (size_t i = from + 1; i < to; ++i)
        {
            if (s[i - 1] < 0.0f && s[i] >= 0.0f)
            {
                const double t = static_cast<double>(i - 1) + s[i - 1] / static_cast<double>(s[i - 1] - s[i]);
                if (first < 0.0) first = t;
                last = t;
                ++count;
            }
        }
        if (count < 2) return 0.0;
        return (count - 1) * rate / (last - first);
    }

    // First / last sample where a 5 ms RMS envelope is above `level`.
    std::pair<int, int> span(const Signal& s, float level, double rate)
    {
        const int w = static_cast<int>(rate * 0.005);
        int first = -1, last = -1;
        for (int i = 0; i + w <= static_cast<int>(s.size()); i += w / 4)
        {
            if (rms(s, static_cast<size_t>(i), static_cast<size_t>(i + w)) > level)
            {
                if (first < 0) first = i;
                last = i + w;
            }
        }
        return { first, last };
    }

    void testRate(double rate)
    {
        const juce::String tag = " @ " + juce::String(rate / 1000.0, 0) + " kHz";
        std::cout << "\n== " << rate << " Hz ==" << std::endl;

        PitchShifter probe;
        probe.prepare(rate);
        const int lat = probe.getLatencySamples();
        std::cout << "  latency " << lat << " samples (" << 1000.0 * lat / rate << " ms)" << std::endl;
        check(lat == (rate > 60000.0 ? 4096 : 2048), "latency is the frame size" + tag);

        // 1. Ratio 1 is a pure delay of exactly the latency.
        {
            const Signal in = noise(rate, 2.0, 1);
            const Signal out = run(rate, in, 1.0f);
            const Signal outNear = run(rate, in, 1.00005f);
            float err = 0.0f, errNear = 0.0f;
            for (size_t i = 0; i < in.size(); ++i)
            {
                const float want = i >= static_cast<size_t>(lat) ? in[i - static_cast<size_t>(lat)] : 0.0f;
                err = std::max(err, std::abs(out[i] - want));
                errNear = std::max(errNear, std::abs(outNear[i] - want));
            }
            std::cout << "  ratio 1 max error " << err << ", ratio 1.00005 max error " << errNear << std::endl;
            check(err < 1.0e-4f, "ratio 1: output is the input delayed by the latency" + tag);
            check(errNear < 1.0e-4f, "ratio within 1e-4 of 1 is also the pure delay" + tag);
        }

        // 2. Pitch and level of a shifted 440 Hz sine.
        {
            const Signal in = sine(rate, 440.0, 3.0, 0.5f);
            const float inRms = rms(in, static_cast<size_t>(rate * 0.5), static_cast<size_t>(rate * 2.5));
            struct Case { float ratio; double want; const char* name; };
            const Case cases[] = { { 2.0f, 880.0, "octave up" },
                                   { 0.5f, 220.0, "octave down" },
                                   { std::pow(2.0f, 7.0f / 12.0f), 440.0 * std::pow(2.0, 7.0 / 12.0), "fifth up" } };
            for (const auto& c : cases)
            {
                const Signal out = run(rate, in, c.ratio);
                const size_t from = static_cast<size_t>(lat) + static_cast<size_t>(rate * 0.5);
                const size_t to = static_cast<size_t>(rate * 2.8);
                const double hz = zeroCrossingHz(out, from, to, rate);
                const float db = juce::Decibels::gainToDecibels(rms(out, from, to) / inRms);
                std::cout << "  " << c.name << ": " << hz << " Hz (want " << c.want << "), level " << db << " dB" << std::endl;
                check(std::abs(hz / c.want - 1.0) < 0.01, juce::String(c.name) + ": 440 Hz lands within 1 %" + tag);
                check(std::abs(db) < 2.0f, juce::String(c.name) + ": level within 2 dB" + tag);
            }
        }

        // 2b. The ends of the range: 0.25 (two octaves down) and 4 (two octaves up) land on pitch,
        // hold their level, stay finite and bounded on dense material, and ratios past the ends clamp.
        {
            const Signal in = sine(rate, 440.0, 3.0, 0.5f);
            const float inRms = rms(in, static_cast<size_t>(rate * 0.5), static_cast<size_t>(rate * 2.5));
            const size_t from = static_cast<size_t>(lat) + static_cast<size_t>(rate * 0.5);
            const size_t to = static_cast<size_t>(rate * 2.8);
            for (float ratio : { 0.25f, 4.0f })
            {
                const Signal out = run(rate, in, ratio);
                const double want = 440.0 * ratio;
                const double hz = zeroCrossingHz(out, from, to, rate);
                const float db = juce::Decibels::gainToDecibels(rms(out, from, to) / inRms);
                std::cout << "  ratio " << ratio << ": " << hz << " Hz (want " << want << "), level " << db << " dB" << std::endl;
                check(std::abs(hz / want - 1.0) < 0.01, "ratio " + juce::String(ratio) + ": 440 Hz lands within 1 %" + tag);
                check(std::abs(db) < 2.0f, "ratio " + juce::String(ratio) + ": level within 2 dB" + tag);

                const Signal past = run(rate, in, ratio < 1.0f ? 0.1f : 8.0f);
                float diff = 0.0f;
                for (size_t i = 0; i < in.size(); ++i)
                    diff = std::max(diff, std::abs(past[i] - out[i]));
                check(diff == 0.0f, "ratio " + juce::String(ratio < 1.0f ? 0.1f : 8.0f) + " clamps to " + juce::String(ratio) + tag);

                Signal dense = noise(rate, 3.0, 5, 0.3f);
                for (size_t i = 0; i < dense.size(); ++i)
                    if (i % static_cast<size_t>(rate / 4.0) == 0) dense[i] = 0.95f;
                const Signal d = run(rate, dense, ratio);
                bool finite = true;
                float peak = 0.0f;
                for (float v : d)
                {
                    finite = finite && std::isfinite(v);
                    peak = std::max(peak, std::abs(v));
                }
                const float dDb = juce::Decibels::gainToDecibels(rms(d, from, to) / rms(dense, from - static_cast<size_t>(lat), to - static_cast<size_t>(lat)));
                // Shifting white noise up by r pushes all but 1/r of its power past Nyquist, so expect
                // 10 log10(1/r) dB from that alone; allow 4 dB on top for the phase vocoder's partly
                // incoherent overlap-add on noise (about -3 dB at worst).
                const float expectDb = 10.0f * std::log10(std::min(1.0f, 1.0f / ratio));
                std::cout << "  ratio " << ratio << " on noise + clicks: peak " << peak << ", level " << dDb
                          << " dB (band-limit alone gives " << expectDb << " dB)" << std::endl;
                check(finite && peak <= 4.0f, "ratio " + juce::String(ratio) + ": noise + clicks stay finite and under 4.0" + tag);
                check(std::abs(dDb - expectDb) < 4.0f, "ratio " + juce::String(ratio) + ": noise level within 4 dB of expected" + tag);
            }
        }

        // 3. Duration: a 1 s burst stays 1 s long.
        {
            const double start = 0.5;
            const Signal in = sine(rate, 440.0, 2.5, 0.5f, start, 1.0);
            const float level = 0.5f * 0.7071f * 0.25f; // -12 dB of the burst's RMS
            const auto inSpan = span(in, level, rate);
            for (float ratio : { 2.0f, 0.5f, 1.5f })
            {
                const Signal out = run(rate, in, ratio);
                const auto outSpan = span(out, level, rate);
                const int onErr = outSpan.first - (inSpan.first + lat);
                const int offErr = outSpan.second - (inSpan.second + lat);
                const double length = (outSpan.second - outSpan.first) / rate;
                std::cout << "  ratio " << ratio << ": burst " << length << " s, onset " << onErr << ", offset " << offErr
                          << " samples from input + latency" << std::endl;
                check(std::abs(onErr) <= 2 * lat && std::abs(offErr) <= 2 * lat,
                      "ratio " + juce::String(ratio) + ": a 1 s burst keeps its length (within 2 frames)" + tag);
            }
        }

        // 3b. Staggered frames (reset with a hop phase, as the engine does per track) keep the same latency:
        //     ratio 1 is still the exact delay, and a shifted burst starts and ends where the unstaggered one does.
        {
            const Signal in = sine(rate, 440.0, 2.5, 0.5f, 0.5, 1.0);
            const float level = 0.5f * 0.7071f * 0.25f;
            auto runPhased = [&](float ratio, float phase)
            {
                PitchShifter ps;
                ps.prepare(rate);
                ps.reset(ratio, phase);
                Signal out(in.size(), 0.0f);
                for (size_t pos = 0; pos < in.size(); pos += 32)
                {
                    const int n = static_cast<int>(std::min<size_t>(32, in.size() - pos));
                    ps.process(in.data() + pos, out.data() + pos, n, ratio);
                }
                return out;
            };
            float err = 0.0f;
            const Signal flat = runPhased(1.0f, 0.37f);
            for (size_t i = static_cast<size_t>(lat); i < in.size(); ++i)
                err = std::max(err, std::abs(flat[i] - in[i - static_cast<size_t>(lat)]));
            check(err < 1.0e-4f, "staggered reset: ratio 1 is still the exact delay" + tag);
            const auto base = span(runPhased(2.0f, 0.0f), level, rate);
            int worst = 0;
            for (float phase : { 0.25f, 0.37f, 0.77f })
            {
                const auto sp = span(runPhased(2.0f, phase), level, rate);
                worst = std::max({ worst, std::abs(sp.first - base.first), std::abs(sp.second - base.second) });
            }
            std::cout << "  staggered reset: ratio-1 error " << err << ", burst edges move at most " << worst << " samples" << std::endl;
            check(worst <= lat / 4, "staggered reset: a shifted burst starts and ends within a hop of the unstaggered one" + tag);
        }

        // 4. Transients: a 4 Hz click train shifted by 1.5 keeps sharp clicks.
        {
            const int period = static_cast<int>(rate / 4.0);
            Signal in(static_cast<size_t>(rate * 3.0), 0.0f);
            for (int p = period / 2; p < static_cast<int>(in.size()); p += period)
                in[static_cast<size_t>(p)] = 0.9f;
            const Signal dry = run(rate, in, 1.0f);
            const Signal up = run(rate, in, 1.5f);
            float worst = 1.0e9f;
            for (int p = period / 2; p + lat + lat < static_cast<int>(in.size()); p += period)
            {
                float a = 0.0f, b = 0.0f;
                for (int i = p + lat - lat / 2; i < p + lat + lat / 2; ++i)
                {
                    a = std::max(a, std::abs(dry[static_cast<size_t>(i)]));
                    b = std::max(b, std::abs(up[static_cast<size_t>(i)]));
                }
                worst = std::min(worst, b / a);
            }
            std::cout << "  clicks at ratio 1.5: weakest click peak " << 100.0f * worst << " % of the unshifted peak" << std::endl;
            check(worst >= 0.5f, "clicks at ratio 1.5 keep >= 50 % of their peak" + tag);
            if (rate == 48000.0)
                writeWav("pitch_clicks_x1_5", up, rate);
        }

        // 5. A fast ratio sweep 0.5 -> 2 over 2 s: finite and bounded.
        {
            Signal in = sine(rate, 330.0, 3.0, 0.4f);
            const Signal n = noise(rate, 3.0, 7, 0.15f);
            for (size_t i = 0; i < in.size(); ++i)
            {
                in[i] += n[i];
                if (i % static_cast<size_t>(rate / 3.0) == 0) in[i] = 0.95f;
            }
            PitchShifter ps;
            ps.prepare(rate);
            Signal out(in.size());
            bool finite = true;
            float peak = 0.0f;
            const int sweep = static_cast<int>(rate * 2.0);
            for (int i = 0; i < static_cast<int>(in.size()); i += 32)
            {
                const float t = juce::jlimit(0.0f, 1.0f, static_cast<float>(i) / static_cast<float>(sweep));
                const float ratio = 0.5f * std::pow(4.0f, t);
                const int len = std::min(32, static_cast<int>(in.size()) - i);
                ps.process(in.data() + i, out.data() + i, len, ratio);
            }
            for (float v : out)
            {
                finite = finite && std::isfinite(v);
                peak = std::max(peak, std::abs(v));
            }
            std::cout << "  sweep 0.5 -> 2: peak " << peak << std::endl;
            check(finite, "sweep: no NaN / inf" + tag);
            check(peak <= 4.0f, "sweep: nothing over 4.0" + tag);
            if (rate == 48000.0)
                writeWav("pitch_sweep", out, rate);
        }

        // 6. Chunking does not change the output.
        {
            Signal in = noise(rate, 2.0, 3, 0.2f);
            const Signal s = sine(rate, 523.0, 2.0, 0.3f);
            for (size_t i = 0; i < in.size(); ++i)
                in[i] += s[i] + (i % 9000 == 100 ? 0.8f : 0.0f);
            const Signal whole = run(rate, in, 1.5f, static_cast<int>(in.size()));
            float worst = 0.0f;
            for (int chunk : { 32, 1, 512 })
            {
                const Signal c = run(rate, in, 1.5f, chunk);
                for (size_t i = 0; i < in.size(); ++i)
                    worst = std::max(worst, std::abs(c[i] - whole[i]));
            }
            {
                // Mixed: 32, 1, 0, 512, ... (and an in-place call).
                PitchShifter ps;
                ps.prepare(rate);
                Signal c = in;
                const int sizes[] = { 32, 1, 0, 512 };
                int i = 0, k = 0;
                while (i < static_cast<int>(c.size()))
                {
                    const int len = std::min(sizes[k++ % 4], static_cast<int>(c.size()) - i);
                    ps.process(c.data() + i, c.data() + i, len, 1.5f);
                    i += len;
                }
                for (size_t j = 0; j < in.size(); ++j)
                    worst = std::max(worst, std::abs(c[j] - whole[j]));
            }
            std::cout << "  chunking: max difference " << worst << std::endl;
            check(worst < 1.0e-5f, "chunk sizes 32, 1, 0, 512 and in-place give the same output as one call" + tag);
        }

        // 7. Leaving a shift: back at ratio 1 the bypass is exact again, and the hand-over is smooth.
        {
            const Signal in = sine(rate, 440.0, 4.0, 0.5f);
            PitchShifter ps;
            ps.prepare(rate);
            Signal out(in.size());
            for (int i = 0; i < static_cast<int>(in.size()); i += 32)
            {
                const double t = i / rate;
                const float ratio = (t >= 1.0 && t < 2.0) ? 1.5f : 1.0f;
                ps.process(in.data() + i, out.data() + i, std::min(32, static_cast<int>(in.size()) - i), ratio);
            }
            float err = 0.0f, jump = 0.0f;
            for (size_t i = static_cast<size_t>(rate * 3.0); i < in.size(); ++i)
                err = std::max(err, std::abs(out[i] - in[i - static_cast<size_t>(lat)]));
            for (size_t i = 1; i < out.size(); ++i)
                jump = std::max(jump, std::abs(out[i] - out[i - 1]));
            const float sineStep = 0.5f * static_cast<float>(c_twoPi * 660.0 / rate);
            std::cout << "  1 -> 1.5 -> 1: error 1 s after returning " << err << ", largest sample step " << jump
                      << " (a 660 Hz sine at this level steps " << sineStep << ")" << std::endl;
            check(err < 1.0e-4f, "back at ratio 1 the output is the exact delay again" + tag);
            check(jump < 2.0f * sineStep, "switching in and out of the shift does not click" + tag);
            if (rate == 48000.0)
                writeWav("pitch_in_and_out", out, rate);
        }
    }

    // A richer source for listening: a buzzy tone with vibrato plus drum-like hits.
    Signal listeningSource(double rate, double seconds)
    {
        Signal s(static_cast<size_t>(rate * seconds), 0.0f);
        double ph = 0.0;
        juce::Random r(11);
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double t = static_cast<double>(i) / rate;
            ph += (196.0 * (1.0 + 0.01 * std::sin(c_twoPi * 5.0 * t))) / rate;
            float v = 0.0f;
            for (int h = 1; h <= 12; ++h)
                v += static_cast<float>(std::sin(c_twoPi * ph * h)) / static_cast<float>(h);
            const double beat = std::fmod(t, 0.5);
            const float hit = static_cast<float>(std::exp(-beat * 40.0)) * (r.nextFloat() * 2.0f - 1.0f)
                            + static_cast<float>(std::exp(-beat * 25.0) * std::sin(c_twoPi * 70.0 * beat));
            s[i] = 0.18f * v + 0.4f * hit;
        }
        return s;
    }
}

int main()
{
    // Frame size (= latency) follows the rate, ~43 ms everywhere.
    {
        std::cout << "\n== frame sizes ==" << std::endl;
        const std::pair<double, int> want[] = { { 44100.0, 2048 }, { 48000.0, 2048 }, { 88200.0, 4096 },
                                                { 96000.0, 4096 }, { 176400.0, 8192 }, { 192000.0, 8192 } };
        for (const auto& [rate, size] : want)
        {
            PitchShifter ps;
            ps.prepare(rate);
            check(ps.getLatencySamples() == size,
                  juce::String(rate, 0) + " Hz: latency " + juce::String(ps.getLatencySamples()) + " (want " + juce::String(size) + ")");
        }
    }

    testRate(48000.0);
    testRate(96000.0);

    // Listening files.
    {
        const double rate = 48000.0;
        const Signal src = listeningSource(rate, 4.0);
        writeWav("pitch_source", src, rate);
        writeWav("pitch_up12", run(rate, src, 2.0f), rate);
        writeWav("pitch_down12", run(rate, src, 0.5f), rate);
        writeWav("pitch_up7", run(rate, src, std::pow(2.0f, 7.0f / 12.0f)), rate);
        writeWav("pitch_down5_cents30", run(rate, src, std::pow(2.0f, (-5.0f + 0.3f) / 12.0f)), rate);
        std::cout << "\n  wrote pitch_*.wav to " << juce::File::getCurrentWorkingDirectory().getFullPathName() << std::endl;
    }

    // Speed: ten instances, 10 s at 48 kHz, 32-sample calls, all shifting.
    {
        const double rate = 48000.0;
        const Signal src = listeningSource(rate, 10.0);
        std::vector<PitchShifter> shifters(10);
        for (auto& s : shifters)
            s.prepare(rate);
        Signal out(32);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i + 32 <= static_cast<int>(src.size()); i += 32)
            for (size_t k = 0; k < shifters.size(); ++k)
                shifters[k].process(src.data() + i, out.data(), 32, std::pow(2.0f, (static_cast<float>(k) - 4.5f) / 6.0f));
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double factor = 10.0 / secs;
        std::cout << "\n  speed: 10 instances x 10 s took " << secs << " s (" << factor << "x real time)" << std::endl;
        check(factor > 20.0, "ten shifting instances run faster than 20x real time");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

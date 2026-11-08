// Offline tests for Roarschach: drive the real AudioProcessor at several sample rates and block
// sizes and check what the spec promises (finite, bounded output; silence stays silent; constant
// latency; exact dry/bypass nulls; state round-trip; click-free slot moves; every preset; CPU).
//
//   RoarschachTests                        run every test, non-zero exit on failure
//   RoarschachTests --render in.wav outdir [preset ...]
//                                          render a file through factory presets (default: a few)

#include <JuceHeader.h>

#include "Params.h"
#include "PluginProcessor.h"
#include "PresetManager.h"
#include "PluginEditor.h"
#include "dsp/GravelVocoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void check(bool ok, const juce::String& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("  FAIL: %s\n", what.toRawUTF8());
        }
    }

    using Signal = std::vector<float>;

    struct Stereo
    {
        Signal l, r;
        int size() const { return static_cast<int>(l.size()); }
    };

    constexpr double c_pi = 3.14159265358979323846;

    //==========================================================================
    // Test signals (mono).

    Signal makeSweep(double sr, double seconds)
    {
        const int n = static_cast<int>(sr * seconds);
        Signal s(static_cast<size_t>(n));
        const double f0 = 60.0, f1 = std::min(8000.0, 0.4 * sr);
        const double k = std::log(f1 / f0) / seconds;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const double phase = 2.0 * c_pi * f0 * (std::exp(k * t) - 1.0) / k;
            s[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(phase));
        }
        return s;
    }

    // A voiced vowel: a glottal pulse train (f0 gliding 80 -> 220 -> 120 Hz) through three formant
    // resonators, with a 3 Hz syllable envelope, normalised to a -6 dBFS peak.
    Signal makeVowel(double sr, double seconds, double f0Lo = 80.0, double f0Hi = 220.0)
    {
        const int n = static_cast<int>(sr * seconds);
        Signal s(static_cast<size_t>(n), 0.0f);

        struct Reson { double b0 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; };
        const auto makeReson = [sr](double fc, double bw)
        {
            Reson r;
            const double rad = std::exp(-c_pi * bw / sr);
            r.a1 = -2.0 * rad * std::cos(2.0 * c_pi * fc / sr);
            r.a2 = rad * rad;
            r.b0 = 1.0 - rad;
            return r;
        };
        Reson formants[3] { makeReson(700.0, 110.0), makeReson(1220.0, 120.0), makeReson(2600.0, 160.0) };
        const double gains[3] { 1.0, 0.5, 0.25 };

        double phase = 0.0;
        double prevPulse = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const double u = t / seconds;
            const double f0 = u < 0.5 ? f0Lo + (f0Hi - f0Lo) * (u / 0.5) : f0Hi + (120.0 - f0Hi) * ((u - 0.5) / 0.5);
            phase += f0 / sr;
            if (phase >= 1.0)
                phase -= 1.0;

            // Rosenberg-like pulse: rising 40 %, closing 16 %, closed after. Differentiated.
            double g = 0.0;
            if (phase < 0.4)
                g = 0.5 * (1.0 - std::cos(c_pi * phase / 0.4));
            else if (phase < 0.56)
                g = std::cos(0.5 * c_pi * (phase - 0.4) / 0.16);
            const double excitation = g - prevPulse;
            prevPulse = g;

            double y = 0.0;
            for (int f = 0; f < 3; ++f)
            {
                auto& r = formants[f];
                const double out = r.b0 * excitation - r.a1 * r.z1 - r.a2 * r.z2;
                r.z2 = r.z1;
                r.z1 = out;
                y += gains[f] * out;
            }

            const double syllable = 0.55 + 0.45 * std::sin(2.0 * c_pi * 3.0 * t);
            s[static_cast<size_t>(i)] = static_cast<float>(y * syllable);
        }

        float peak = 0.0f;
        for (float x : s)
            peak = std::max(peak, std::abs(x));
        if (peak > 0.0f)
            for (auto& x : s)
                x *= 0.5f / peak;
        return s;
    }

    Signal makeNoiseBursts(double sr, double seconds)
    {
        const int n = static_cast<int>(sr * seconds);
        Signal s(static_cast<size_t>(n), 0.0f);
        std::mt19937 rng(1234);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        const int period = static_cast<int>(0.3 * sr), burst = static_cast<int>(0.1 * sr);
        for (int i = 0; i < n; ++i)
            if (i % period < burst)
                s[static_cast<size_t>(i)] = 0.1f * dist(rng);
        return s;
    }

    // Steady band-limited noise at about -20 dBFS RMS.
    Signal makeBandNoise(double sr, double seconds, double loHz, double hiHz)
    {
        const int n = static_cast<int>(sr * seconds);
        Signal s(static_cast<size_t>(n));
        std::mt19937 rng(99);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        juce::IIRFilter hp1, hp2, lp1, lp2;
        hp1.setCoefficients(juce::IIRCoefficients::makeHighPass(sr, loHz));
        hp2.setCoefficients(juce::IIRCoefficients::makeHighPass(sr, loHz));
        lp1.setCoefficients(juce::IIRCoefficients::makeLowPass(sr, hiHz));
        lp2.setCoefficients(juce::IIRCoefficients::makeLowPass(sr, hiHz));
        for (auto& x : s)
            x = lp2.processSingleSampleRaw(lp1.processSingleSampleRaw(hp2.processSingleSampleRaw(hp1.processSingleSampleRaw(dist(rng)))));
        double sq = 0.0;
        for (float x : s)
            sq += static_cast<double>(x) * x;
        const float gain = static_cast<float>(0.1 / std::sqrt(sq / std::max(1, n)));
        for (auto& x : s)
            x *= gain;
        return s;
    }

    Signal makeSilence(double sr, double seconds)
    {
        return Signal(static_cast<size_t>(sr * seconds), 0.0f);
    }

    Signal makeDC(double sr, double seconds)
    {
        return Signal(static_cast<size_t>(sr * seconds), 0.5f);
    }

    Signal makeSquare(double sr, double seconds)
    {
        const int n = static_cast<int>(sr * seconds);
        Signal s(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            s[static_cast<size_t>(i)] = std::fmod(i * 110.0 / sr, 1.0) < 0.5 ? 1.0f : -1.0f;
        return s;
    }

    //==========================================================================
    // Processor driving.

    using Values = std::vector<std::pair<const char*, float>>;

    void setParam(RoarschachAudioProcessor& p, const char* id, float value)
    {
        auto* param = p.getValueTreeState().getParameter(id);
        if (param == nullptr)
        {
            check(false, juce::String("missing parameter ") + id);
            return;
        }
        param->setValueNotifyingHost(param->convertTo0to1(value));
    }

    void setValues(RoarschachAudioProcessor& p, const Values& values)
    {
        for (const auto& [id, value] : values)
            setParam(p, id, value);
    }

    float getParam(RoarschachAudioProcessor& p, const char* id)
    {
        return p.getValueTreeState().getRawParameterValue(id)->load();
    }

    std::unique_ptr<RoarschachAudioProcessor> makeProcessor(double sr, int block, int numIns = 2, int numOuts = 2)
    {
        auto p = std::make_unique<RoarschachAudioProcessor>();
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add(numIns == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
        layout.outputBuses.add(numOuts == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
        check(p->setBusesLayout(layout), "bus layout accepted");
        p->setRateAndBufferSizeDetails(sr, block);
        p->prepareToPlay(sr, block);
        return p;
    }

    // Renders in.l / in.r (in.r ignored for a mono input) in blocks of `block`. onBlock(startSample)
    // is called before each block (for mid-stream parameter changes).
    Stereo render(RoarschachAudioProcessor& p, const Stereo& in, int block, bool bypassed = false,
                  const std::function<void(int)>& onBlock = {})
    {
        const int numIns = p.getTotalNumInputChannels();
        const int numOuts = p.getTotalNumOutputChannels();
        const int numCh = std::max(numIns, numOuts);
        const int n = in.size();

        Stereo out;
        out.l.assign(static_cast<size_t>(n), 0.0f);
        out.r.assign(static_cast<size_t>(n), 0.0f);

        juce::AudioBuffer<float> buffer(numCh, block);
        juce::MidiBuffer midi;

        for (int start = 0; start < n; start += block)
        {
            const int len = std::min(block, n - start);
            if (onBlock)
                onBlock(start);

            buffer.setSize(numCh, len, false, false, true);
            buffer.clear();
            for (int i = 0; i < len; ++i)
            {
                buffer.setSample(0, i, in.l[static_cast<size_t>(start + i)]);
                if (numIns > 1)
                    buffer.setSample(1, i, in.r[static_cast<size_t>(start + i)]);
            }

            if (bypassed)
                p.processBlockBypassed(buffer, midi);
            else
                p.processBlock(buffer, midi);

            for (int i = 0; i < len; ++i)
            {
                out.l[static_cast<size_t>(start + i)] = buffer.getSample(0, i);
                out.r[static_cast<size_t>(start + i)] = numOuts > 1 ? buffer.getSample(1, i) : buffer.getSample(0, i);
            }
        }

        return out;
    }

    Stereo dual(const Signal& s)
    {
        return { s, s };
    }

    struct Stats
    {
        bool finite = true;
        bool subnormal = false;
        float peak = 0.0f;
        double rms = 0.0;
        double mean = 0.0;
    };

    Stats stats(const Stereo& s, int from = 0)
    {
        Stats st;
        double sum = 0.0, sumSq = 0.0;
        int count = 0;
        for (const auto* ch : { &s.l, &s.r })
            for (size_t i = static_cast<size_t>(from); i < ch->size(); ++i)
            {
                const float x = (*ch)[i];
                if (!std::isfinite(x))
                {
                    st.finite = false;
                    continue;
                }
                if (std::fpclassify(x) == FP_SUBNORMAL)
                    st.subnormal = true;
                st.peak = std::max(st.peak, std::abs(x));
                sum += x;
                sumSq += static_cast<double>(x) * x;
                ++count;
            }
        if (count > 0)
        {
            st.mean = sum / count;
            st.rms = std::sqrt(sumSq / count);
        }
        return st;
    }

    double toDb(double x)
    {
        return 20.0 * std::log10(std::max(x, 1.0e-12));
    }

    // Settings that push every stage hard.
    const Values c_extreme {
        { Params::pitch, -12.0f }, { Params::size, 100.0f }, { Params::growl, 100.0f }, { Params::liquid, 100.0f },
        { Params::roar, 100.0f }, { Params::sub, 100.0f }, { Params::detailLow, 100.0f }, { Params::detailHigh, 100.0f },
        { Params::gravel, 100.0f }, { Params::mix, 100.0f }, { Params::detailSlot, 5.0f }, { Params::vocoderSlot, 3.0f },
        { Params::vocoderOctave, 1.0f },
    };

    // Every stage at its neutral point: the chain is then CLEAN + FINISH and pure delays.
    const Values c_neutral {
        { Params::pitch, 0.0f }, { Params::size, 0.0f }, { Params::growl, 0.0f }, { Params::liquid, 0.0f },
        { Params::roar, 0.0f }, { Params::sub, 0.0f }, { Params::detailLow, 0.0f }, { Params::detailHigh, 0.0f },
        { Params::gravel, 0.0f }, { Params::mix, 100.0f },
    };

    //==========================================================================
    // Tests.

    void testGrid()
    {
        std::printf("[grid] rates x block sizes x signals (defaults and extreme settings)\n");

        const double rates[] { 44100.0, 48000.0, 96000.0 };
        const int blocks[] { 1, 64, 480, 512, 2049 };

        for (double sr : rates)
        {
            struct Named { const char* name; Signal s; };
            const double len = 0.8;
            std::vector<Named> signals;
            signals.push_back({ "sweep", makeSweep(sr, len) });
            signals.push_back({ "vowel", makeVowel(sr, len) });
            signals.push_back({ "noise", makeNoiseBursts(sr, len) });
            signals.push_back({ "silence", makeSilence(sr, len) });
            signals.push_back({ "dc", makeDC(sr, len) });
            signals.push_back({ "square", makeSquare(sr, len) });

            for (int block : blocks)
            {
                for (int setting = 0; setting < 2; ++setting)
                {
                    auto p = makeProcessor(sr, block);
                    if (setting == 1)
                        setValues(*p, c_extreme);
                    const int latency = p->getLatencySamples();

                    float worstPeak = 0.0f;
                    for (const auto& sig : signals)
                    {
                        // Block size 1 is slow; a shorter run covers it.
                        Stereo in = dual(sig.s);
                        if (block == 1)
                        {
                            in.l.resize(static_cast<size_t>(0.25 * sr));
                            in.r = in.l;
                        }

                        p->reset();
                        const auto out = render(*p, in, block);
                        const auto st = stats(out);
                        const juce::String where = juce::String(sr / 1000.0, 1) + "k block " + juce::String(block)
                                                 + (setting == 0 ? " defaults " : " extreme ") + sig.name;
                        check(st.finite, where + ": non-finite output");
                        check(!st.subnormal, where + ": subnormal output");
                        check(st.peak <= 1.0001f, where + ": peak " + juce::String(st.peak) + " above 0 dBFS");
                        worstPeak = std::max(worstPeak, st.peak);

                        if (juce::String(sig.name) == "silence")
                            check(st.peak < 1.0e-4f, where + ": silence in gave " + juce::String(toDb(st.peak), 1) + " dBFS out");

                        // DC is not passed on (high-passes in CLEAN and FINISH).
                        if (juce::String(sig.name) == "dc")
                        {
                            const auto tail = stats(out, out.size() / 2);
                            check(std::abs(tail.mean) < 0.01, where + ": DC passes, mean " + juce::String(tail.mean));
                        }
                    }

                    check(p->getLatencySamples() == latency, "latency changed while rendering");
                    if (block == 512)
                        std::printf("  %5.1fk %s: latency %d, worst peak %.2f dBFS\n", sr / 1000.0,
                                    setting == 0 ? "defaults" : "extreme ", latency, toDb(worstPeak));
                }
            }
        }
    }

    void testLatency()
    {
        std::printf("[latency] constant over settings and slots, matches the measured alignment\n");

        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            auto p = makeProcessor(sr, 512);
            const int latency = p->getLatencySamples();
            const int expected = static_cast<int>(std::ceil(0.030 * sr)) + juce::roundToInt(0.00035 * sr)
                               + 49; // ROAR's linear-phase 2x oversampler (FIR half-band, both ways)
            check(latency == expected, "latency " + juce::String(latency) + " != expected " + juce::String(expected)
                                           + " at " + juce::String(sr));

            // Re-prepare with every slot pair and with extreme settings: the latency never moves.
            bool constant = true;
            for (int d = 0; d < Params::numSlots; ++d)
                for (int v = 0; v < Params::numSlots; ++v)
                {
                    setParam(*p, Params::detailSlot, static_cast<float>(d));
                    setParam(*p, Params::vocoderSlot, static_cast<float>(v));
                    setValues(*p, (d + v) % 2 == 0 ? c_extreme : c_neutral);
                    setParam(*p, Params::detailSlot, static_cast<float>(d));
                    setParam(*p, Params::vocoderSlot, static_cast<float>(v));
                    p->prepareToPlay(sr, 512);
                    constant = constant && p->getLatencySamples() == latency;
                }
            check(constant, "latency changes with settings / slots at " + juce::String(sr));

            // Measured: with every stage neutral, the output lines up with the input delayed by
            // the reported latency (cross-correlation peak). The test signal is 1-4 kHz noise: the
            // chain's minimum-phase high-passes (CLEAN, FINISH) lead low frequencies by a few
            // samples, which is phase, not latency; the 15 kHz low-pass lags the band by a sample or
            // two.
            setValues(*p, c_neutral);
            setParam(*p, Params::detailSlot, 0.0f);
            setParam(*p, Params::vocoderSlot, 0.0f);
            p->prepareToPlay(sr, 512);
            const auto vowel = makeBandNoise(sr, 1.5, 1000.0, 4000.0);
            const auto out = render(*p, dual(vowel), 512);

            const int from = static_cast<int>(0.5 * sr), to = static_cast<int>(1.4 * sr);
            int bestLag = 0;
            double best = -1.0e30;
            for (int lag = 0; lag < latency * 2 + 64; ++lag)
            {
                double c = 0.0;
                for (int i = from; i < to; ++i)
                    c += static_cast<double>(out.l[static_cast<size_t>(i)]) * vowel[static_cast<size_t>(i - lag)];
                if (c > best)
                {
                    best = c;
                    bestLag = lag;
                }
            }
            std::printf("  %5.1fk: reported %d, measured %d samples\n", sr / 1000.0, latency, bestLag);
            check(std::abs(bestLag - latency) <= 2, "measured alignment " + juce::String(bestLag) + " vs reported "
                                                         + juce::String(latency) + " at " + juce::String(sr));
        }
    }

    // Output must equal the input delayed by the latency, sample for sample.
    double nullError(const Stereo& in, const Stereo& out, int latency, int skip)
    {
        double worst = 0.0;
        for (int i = std::max(skip, latency); i < out.size(); ++i)
        {
            worst = std::max(worst, static_cast<double>(std::abs(out.l[static_cast<size_t>(i)] - in.l[static_cast<size_t>(i - latency)])));
            worst = std::max(worst, static_cast<double>(std::abs(out.r[static_cast<size_t>(i)] - in.r[static_cast<size_t>(i - latency)])));
        }
        // The first `latency` samples must be the (silent) delay start.
        for (int i = 0; i < std::min(latency, out.size()); ++i)
            worst = std::max(worst, static_cast<double>(std::abs(out.l[static_cast<size_t>(i)]) + std::abs(out.r[static_cast<size_t>(i)])));
        return worst;
    }

    void testNulls()
    {
        std::printf("[null] Mix 0 and host bypass give the input delayed by the latency\n");

        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (int block : { 1, 64, 480, 512, 2049 })
            {
                // Different L / R content, to see the channels are kept apart.
                Stereo in { makeVowel(sr, 0.5), makeSweep(sr, 0.5) };

                auto p = makeProcessor(sr, block);
                setValues(*p, c_extreme);
                setParam(*p, Params::mix, 0.0f);
                p->prepareToPlay(sr, block);
                const auto mixOut = render(*p, in, block);
                const double mixErr = nullError(in, mixOut, p->getLatencySamples(), 0);
                check(mixErr < 1.0e-7, "Mix 0 null error " + juce::String(mixErr) + " at " + juce::String(sr) + " block " + juce::String(block));

                auto q = makeProcessor(sr, block);
                setValues(*q, c_extreme);
                const auto bypassOut = render(*q, in, block, true);
                const double bypassErr = nullError(in, bypassOut, q->getLatencySamples(), 0);
                check(bypassErr < 1.0e-7, "bypass null error " + juce::String(bypassErr) + " at " + juce::String(sr) + " block " + juce::String(block));
            }

        // Bypass engaged mid-stream: after the 15 ms crossfade it is the delayed input again, and
        // the AU wrapper's reset on entering bypass does not break the dry path.
        {
            const double sr = 48000.0;
            const int block = 480;
            auto p = makeProcessor(sr, block);
            const Stereo in = dual(makeVowel(sr, 1.0));
            const int n = in.size();
            const int switchAt = n / 2;
            Stereo out;
            out.l.assign(static_cast<size_t>(n), 0.0f);
            out.r.assign(static_cast<size_t>(n), 0.0f);
            juce::AudioBuffer<float> buffer(2, block);
            juce::MidiBuffer midi;
            for (int start = 0; start < n; start += block)
            {
                const int len = std::min(block, n - start);
                buffer.setSize(2, len, false, false, true);
                for (int ch = 0; ch < 2; ++ch)
                    buffer.copyFrom(ch, 0, (ch == 0 ? in.l : in.r).data() + start, len);
                if (start >= switchAt)
                {
                    if (start < switchAt + block)
                        p->reset();  // as the AU wrapper does when bypass is switched on
                    p->processBlockBypassed(buffer, midi);
                }
                else
                {
                    p->processBlock(buffer, midi);
                }
                for (int i = 0; i < len; ++i)
                {
                    out.l[static_cast<size_t>(start + i)] = buffer.getSample(0, i);
                    out.r[static_cast<size_t>(start + i)] = buffer.getSample(1, i);
                }
            }
            const int latency = p->getLatencySamples();
            const int settled = switchAt + static_cast<int>(0.02 * sr) + block;
            double worst = 0.0;
            for (int i = settled; i < n; ++i)
                worst = std::max(worst, static_cast<double>(std::abs(out.l[static_cast<size_t>(i)] - in.l[static_cast<size_t>(i - latency)])));
            check(worst < 1.0e-7, "bypass engaged mid-stream: null error " + juce::String(worst));
            check(stats(out).finite, "bypass engaged mid-stream: non-finite");
        }

        // Mono layouts.
        for (auto [ins, outs] : { std::pair { 1, 1 }, std::pair { 1, 2 }, std::pair { 2, 1 } })
        {
            const double sr = 48000.0;
            auto p = makeProcessor(sr, 512, ins, outs);
            Stereo in { makeVowel(sr, 0.5), makeSweep(sr, 0.5) };
            if (ins == 1)
                in.r = in.l;
            const auto wet = render(*p, in, 512);
            const auto st = stats(wet);
            const juce::String where = juce::String(ins) + " in / " + juce::String(outs) + " out";
            check(st.finite && st.peak <= 1.0001f && st.rms > 1.0e-3, where + ": wet output broken");

            setParam(*p, Params::mix, 0.0f);
            p->prepareToPlay(sr, 512);
            const auto dry = render(*p, in, 512);
            Stereo expected = in;
            if (outs == 1 && ins == 2)
                for (size_t i = 0; i < expected.l.size(); ++i)
                    expected.l[i] = expected.r[i] = 0.5f * (in.l[i] + in.r[i]);
            if (outs == 1)
                expected.r = expected.l;
            const double err = nullError(expected, dry, p->getLatencySamples(), 0);
            check(err < 1.0e-6, where + ": Mix 0 null error " + juce::String(err));
        }
    }


    // Magnitude of `s` at `hz` over [from, to) (single-bin DFT), as an amplitude.
    double toneAmplitude(const Signal& s, double sr, double hz, int from, int to)
    {
        double re = 0.0, im = 0.0;
        for (int i = from; i < to; ++i)
        {
            const double ph = 2.0 * c_pi * hz * i / sr;
            re += s[static_cast<size_t>(i)] * std::cos(ph);
            im += s[static_cast<size_t>(i)] * std::sin(ph);
        }
        return 2.0 * std::sqrt(re * re + im * im) / std::max(1, to - from);
    }

    void testMixResponse()
    {
        std::printf("[mix] a partial Mix does not comb: Mix 50 matches the in-phase sum of dry and wet\n");

        const double freqs[] { 50.0, 80.0, 100.0, 150.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 6000.0, 8000.0,
                               10000.0, 12000.0, 13000.0, 14000.0, 15000.0, 16000.0, 17000.0, 18000.0 };

        for (double sr : { 44100.0, 48000.0 })
        {
            double worst = 0.0, worstHz = 0.0;
            for (double hz : freqs)
            {
                const int n = static_cast<int>(1.2 * sr);
                Signal tone(static_cast<size_t>(n));
                for (int i = 0; i < n; ++i)
                    tone[static_cast<size_t>(i)] = static_cast<float>(0.1 * std::sin(2.0 * c_pi * hz * i / sr));

                double level[2] {};
                for (int k = 0; k < 2; ++k)
                {
                    auto p = makeProcessor(sr, 512);
                    setValues(*p, c_neutral);
                    setParam(*p, Params::mix, k == 0 ? 100.0f : 50.0f);
                    p->prepareToPlay(sr, 512);
                    const auto out = render(*p, dual(tone), 512);
                    level[k] = toneAmplitude(out.l, sr, hz, static_cast<int>(0.8 * sr), n) / 0.1;
                }

                // In phase, Mix 50 is the average of the dry (unity) and the wet magnitudes.
                const double expected = 0.5 * (1.0 + level[0]);
                const double errDb = std::abs(toDb(level[1]) - toDb(expected));
                if (errDb > worst)
                {
                    worst = errDb;
                    worstHz = hz;
                }
            }
            std::printf("  %5.1fk: worst Mix 50 deviation %.2f dB at %.0f Hz\n", sr / 1000.0, worst, worstHz);
            check(worst < 1.5, "Mix 50 combs: " + juce::String(worst, 2) + " dB at " + juce::String(worstHz)
                                   + " Hz, " + juce::String(sr));
        }
    }

    void testNonFiniteInput()
    {
        std::printf("[nan] NaN / Inf in the input never reach the output at Mix 100\n");

        const double sr = 48000.0;
        auto p = makeProcessor(sr, 512);
        Stereo in = dual(makeVowel(sr, 0.6));
        const int at = static_cast<int>(0.2 * sr);
        for (int i = 0; i < 10; ++i)
            in.l[static_cast<size_t>(at + i)] = in.r[static_cast<size_t>(at + i)] = std::numeric_limits<float>::quiet_NaN();
        in.l[static_cast<size_t>(at + 10)] = std::numeric_limits<float>::infinity();
        in.r[static_cast<size_t>(at + 10)] = -std::numeric_limits<float>::infinity();
        const auto out = render(*p, in, 512);
        int bad = 0;
        for (int i = 0; i < out.size(); ++i)
            bad += (std::isfinite(out.l[static_cast<size_t>(i)]) ? 0 : 1) + (std::isfinite(out.r[static_cast<size_t>(i)]) ? 0 : 1);
        check(bad == 0, "Mix 100: " + juce::String(bad) + " non-finite output samples");

        // A partial Mix keeps the (sanitised) phase-matched dry: still finite.
        auto q = makeProcessor(sr, 512);
        setParam(*q, Params::mix, 50.0f);
        q->prepareToPlay(sr, 512);
        const auto half = render(*q, in, 512);
        int badHalf = 0;
        for (int i = 0; i < half.size(); ++i)
            badHalf += std::isfinite(half.l[static_cast<size_t>(i)]) ? 0 : 1;
        check(badHalf == 0, "Mix 50: " + juce::String(badHalf) + " non-finite output samples");
    }

    void testVocoderLowRates()
    {
        std::printf("[vocoder] bands above 0.45 fs stay silent at low sample rates (no duplicated bands)\n");

        for (double sr : { 22050.0, 24000.0, 44100.0 })
        {
            roar::GravelVocoder v;
            v.prepare(sr, 512);
            v.setParams(1.0f, 0, 0.0f, 1.0f);
            auto noise = makeBandNoise(sr, 1.0, 100.0, 0.4 * sr);
            roar::VoiceInfo voice;
            voice.f0Hz = 110.0f;
            voice.voiced = true;
            voice.confidence = 0.9f;
            for (size_t start = 0; start + 512 <= noise.size(); start += 512)
                v.process(noise.data() + start, nullptr, 512, false, voice);

            const auto& levels = v.getBandLevels();
            int lit = 0;
            for (float l : levels)
                lit += l > 0.01f ? 1 : 0;
            const bool topDark = levels[30] <= 0.0f && levels[31] <= 0.0f;
            std::printf("  %5.2fk: %d bands lit\n", sr / 1000.0, lit);
            if (sr < 30000.0)
                check(topDark, "top bands active at " + juce::String(sr));
            else
                check(levels[31] > 0.01f, "top band dark at " + juce::String(sr));
        }
    }

    void testEditorText()
    {
        std::printf("[editor] tooltips and titles are plain text (no code identifiers)\n");

        auto p = makeProcessor(48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());
        int count = 0, bad = 0;
        std::function<void(juce::Component&)> walk = [&](juce::Component& c)
        {
            if (auto* t = dynamic_cast<juce::TooltipClient*>(&c))
            {
                const auto tip = t->getTooltip();
                if (tip.isNotEmpty())
                {
                    ++count;
                    if (tip.contains("::") || tip.contains("Palette") || tip.contains("m_"))
                    {
                        ++bad;
                        std::printf("    bad tooltip: %s\n", tip.toRawUTF8());
                    }
                }
            }
            for (auto* child : c.getChildren())
                walk(*child);
        };
        walk(*editor);
        std::printf("  %d tooltips\n", count);
        check(count > 10, "editor has tooltips");
        check(bad == 0, juce::String(bad) + " tooltips contain code identifiers");
        editor.reset();
    }


    void testAutoGainStart()
    {
        std::printf("[autogain] the first words after a reset are not louder than once settled\n");

        const auto& presets = PresetManager::getFactoryPresets();
        const double sr = 48000.0;
        const auto phrase = makeVowel(sr, 1.0, 110.0, 160.0);
        Signal twice = phrase;
        twice.insert(twice.end(), phrase.begin(), phrase.end());
        double worst = 0.0;
        juce::String worstName;

        // Output level per 125 ms window of the phrase, from a fresh start and with the phrase
        // heard once before (settled auto gain): any difference is the start-up.
        for (int i = 0; i < static_cast<int>(presets.size()); ++i)
        {
            const auto levels = [&](const Signal& in, int offset)
            {
                auto p = makeProcessor(sr, 512);
                p->setCurrentProgram(i);
                p->prepareToPlay(sr, 512);
                const int latency = p->getLatencySamples();
                Signal padded = in;
                padded.resize(in.size() + static_cast<size_t>(latency), 0.0f);
                const auto out = render(*p, dual(padded), 512);
                std::vector<double> db;
                const int window = static_cast<int>(0.125 * sr);
                for (int from = 0; from + window <= static_cast<int>(phrase.size()); from += window)
                {
                    double sq = 0.0;
                    for (int k = from; k < from + window; ++k)
                    {
                        const double y = out.l[static_cast<size_t>(offset + k + latency)];
                        sq += y * y;
                    }
                    db.push_back(10.0 * std::log10(std::max(sq, 1.0e-20)));
                }
                return db;
            };
            const auto fresh = levels(phrase, 0);
            const auto settled = levels(twice, static_cast<int>(phrase.size()));
            // The first 250 ms (two windows). Later windows also move with the slower stages'
            // own warm-up (ROAR's tame average, DETAIL's slow detector), which is not auto gain.
            const double start = 0.5 * ((fresh[0] - settled[0]) + (fresh[1] - settled[1]));
            if (start > worst)
            {
                worst = start;
                worstName = presets[static_cast<size_t>(i)].name;
            }
        }
        std::printf("  worst start overshoot %.1f dB (%s)\n", worst, worstName.toRawUTF8());
        check(worst < 2.5, "auto gain starts " + juce::String(worst, 1) + " dB hot (" + worstName + ")");
    }

    void testState()
    {
        std::printf("[state] get/setStateInformation restores every parameter and slot\n");

        std::mt19937 rng(77);
        for (int round = 0; round < 8; ++round)
        {
            auto a = makeProcessor(48000.0, 512);
            for (auto* param : a->getParameters())
            {
                if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
                {
                    const float u = std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
                    ranged->setValueNotifyingHost(ranged->convertTo0to1(ranged->convertFrom0to1(u)));
                }
            }
            a->setViewValue("viewEditorScale", 1.25);

            juce::MemoryBlock state;
            a->getStateInformation(state);

            auto b = makeProcessor(48000.0, 512);
            b->setStateInformation(state.getData(), static_cast<int>(state.getSize()));

            bool same = true;
            for (const char* id : { Params::pitch, Params::size, Params::growl, Params::liquid, Params::roar, Params::sub,
                                    Params::detailLow, Params::detailHigh, Params::gravel, Params::mix,
                                    Params::detailSlot, Params::vocoderSlot, Params::vocoderOctave })
            {
                const float va = getParam(*a, id), vb = getParam(*b, id);
                if (std::abs(va - vb) > 1.0e-4f)
                {
                    same = false;
                    std::printf("    %s: %f vs %f\n", id, va, vb);
                }
            }
            check(same, "state round-trip, round " + juce::String(round));
            check(std::abs(static_cast<double>(b->getViewValue("viewEditorScale", 1.0)) - 1.25) < 1.0e-6, "view option round-trip");
        }

        // The slots really move the modules after a state load.
        auto c = makeProcessor(48000.0, 512);
        setParam(*c, Params::detailSlot, 4.0f);
        setParam(*c, Params::vocoderSlot, 2.0f);
        juce::MemoryBlock state;
        c->getStateInformation(state);
        auto d = makeProcessor(48000.0, 512);
        d->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        d->reset();
        render(*d, dual(makeVowel(48000.0, 0.1)), 512);
        check(d->getDetailSlotInUse() == 4 && d->getVocoderSlotInUse() == 2, "slots in use after a state load");
    }

    void testSlotMoves()
    {
        std::printf("[slots] moves mid-stream are click-free\n");

        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (int block : { 64, 512, 2049 })
            {
                for (int pattern = 0; pattern < 2; ++pattern)
                {
                    auto p = makeProcessor(sr, block);
                    setParam(*p, Params::gravel, 70.0f);
                    setParam(*p, Params::detailLow, 80.0f);
                    setParam(*p, Params::detailHigh, 60.0f);
                    p->prepareToPlay(sr, block);

                    const Stereo in = dual(makeVowel(sr, 3.0));
                    std::vector<int> moveAt;
                    // Pattern 0: a move every 250 ms. Pattern 1: every ~10 ms (reverses mid-fade).
                    const int interval = pattern == 0 ? static_cast<int>(0.25 * sr) : std::max(block, static_cast<int>(0.01 * sr));
                    int nextMove = static_cast<int>(0.3 * sr);
                    std::mt19937 rng(static_cast<unsigned>(block + pattern));

                    const auto out = render(*p, in, block, false, [&](int start)
                    {
                        if (start >= nextMove && start < static_cast<int>(2.7 * sr))
                        {
                            setParam(*p, Params::detailSlot, static_cast<float>(rng() % 6));
                            setParam(*p, Params::vocoderSlot, static_cast<float>(rng() % 6));
                            moveAt.push_back(start);
                            nextMove = start + interval;
                        }
                    });

                    // Step size near the moves vs. everywhere else (the signal's own steps).
                    std::vector<bool> near(out.l.size(), false);
                    const int window = static_cast<int>(0.06 * sr) + block;
                    for (int m : moveAt)
                        for (int i = m; i < std::min(out.size(), m + window); ++i)
                            near[static_cast<size_t>(i)] = true;

                    float nearMax = 0.0f, otherMax = 0.0f;
                    for (int i = static_cast<int>(0.2 * sr); i < out.size(); ++i)
                    {
                        const float step = std::max(std::abs(out.l[static_cast<size_t>(i)] - out.l[static_cast<size_t>(i - 1)]),
                                                    std::abs(out.r[static_cast<size_t>(i)] - out.r[static_cast<size_t>(i - 1)]));
                        float& worst = near[static_cast<size_t>(i)] ? nearMax : otherMax;
                        worst = std::max(worst, step);
                    }

                    const auto st = stats(out);
                    const juce::String where = juce::String(sr / 1000.0, 1) + "k block " + juce::String(block)
                                             + (pattern == 0 ? " slow moves" : " fast moves");
                    check(st.finite && st.peak <= 1.0001f, where + ": broken output");
                    if (pattern == 0)
                    {
                        // Slow moves leave quiet stretches to compare against.
                        check(nearMax <= 1.5f * otherMax + 0.02f, where + ": step " + juce::String(nearMax)
                                                                       + " near moves vs " + juce::String(otherMax) + " elsewhere");
                        if (block == 512)
                            std::printf("  %5.1fk: max step near moves %.4f, elsewhere %.4f (%d moves)\n", sr / 1000.0,
                                        nearMax, otherMax, static_cast<int>(moveAt.size()));
                    }
                    else
                    {
                        // Constant moving: no stretch is free of moves; compare to a run without moves.
                        auto ref = makeProcessor(sr, block);
                        setParam(*ref, Params::gravel, 70.0f);
                        setParam(*ref, Params::detailLow, 80.0f);
                        setParam(*ref, Params::detailHigh, 60.0f);
                        ref->prepareToPlay(sr, block);
                        const auto refOut = render(*ref, in, block);
                        float refMax = 0.0f;
                        for (int i = static_cast<int>(0.2 * sr); i < refOut.size(); ++i)
                            refMax = std::max(refMax, std::abs(refOut.l[static_cast<size_t>(i)] - refOut.l[static_cast<size_t>(i - 1)]));
                        const float allMax = std::max(nearMax, otherMax);
                        check(allMax <= 2.0f * refMax + 0.02f, where + ": step " + juce::String(allMax) + " vs " + juce::String(refMax) + " without moves");
                    }
                }
            }
    }

    void testPresets()
    {
        std::printf("[presets] every factory preset renders finite, bounded audio\n");

        const auto& presets = PresetManager::getFactoryPresets();
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            const auto vowel = makeVowel(sr, 1.5);
            const auto square = makeSquare(sr, 0.5);
            const auto silence = makeSilence(sr, 0.5);

            // At a partial Mix the dry share is the input with the wet chain's phase (see
            // DryPhaseMatch): a full-scale square crests above 0 dBFS once its phase is turned,
            // so that share may too. The wet share stays under the limiter's ceiling.
            float matchedSquarePeak = 0.0f;
            {
                roar::DryPhaseMatch match;
                match.prepare(sr);
                for (float x : square)
                    matchedSquarePeak = std::max(matchedSquarePeak, std::abs(match.process(x)));
            }

            for (int i = 0; i < static_cast<int>(presets.size()); ++i)
            {
                auto p = makeProcessor(sr, 512);
                p->setCurrentProgram(i);
                check(p->getCurrentProgram() == i, juce::String("program ") + presets[static_cast<size_t>(i)].name + " selected");
                p->reset();

                const juce::String where = juce::String(presets[static_cast<size_t>(i)].name) + " @ " + juce::String(sr / 1000.0, 1) + "k";
                const auto a = stats(render(*p, dual(vowel), 512));
                check(a.finite && !a.subnormal && a.peak <= 1.0001f, where + ": vowel output broken (peak " + juce::String(a.peak) + ")");
                check(a.rms > 1.0e-3, where + ": vowel output nearly silent (" + juce::String(toDb(a.rms), 1) + " dBFS)");

                const float mix = getParam(*p, Params::mix) * 0.01f;
                const float squareLimit = 1.0001f * mix + matchedSquarePeak * (1.0f - mix);
                const auto b = stats(render(*p, dual(square), 512));
                check(b.finite && b.peak <= std::max(1.0001f, squareLimit), where + ": square output broken (peak " + juce::String(b.peak) + ")");

                p->reset();
                const auto c = stats(render(*p, dual(silence), 512));
                check(c.finite && c.peak < 1.0e-4f, where + ": silence in gave " + juce::String(toDb(c.peak), 1) + " dBFS");

                if (juce::exactlyEqual(sr, 48000.0))
                    std::printf("  %-18s vowel rms %6.1f dBFS peak %6.1f dBFS\n", presets[static_cast<size_t>(i)].name, toDb(a.rms), toDb(a.peak));
            }
        }
    }

    void testCpu()
    {
        std::printf("[cpu] real-time factor at 48 kHz, block 512\n");

        const double sr = 48000.0;
        const auto in = dual(makeVowel(sr, 10.0));

        for (int config = 0; config < 3; ++config)
        {
            auto p = makeProcessor(sr, 512);
            const char* name = "defaults";
            if (config == 1)
            {
                setParam(*p, Params::gravel, 100.0f);
                name = "defaults + vocoder 100 %";
            }
            else if (config == 2)
            {
                setValues(*p, c_extreme);
                name = "extreme";
            }
            p->prepareToPlay(sr, 512);

            const auto t0 = std::chrono::steady_clock::now();
            render(*p, in, 512);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double rtf = seconds / 10.0;
            std::printf("  %-26s %.1f %% of one core\n", name, rtf * 100.0);
            if (config == 1)
            {
                if (rtf > 0.15)
                    std::printf("  WARNING: above the 15 %% target\n");
                check(rtf < 1.0, "not real-time");
            }
        }
    }

    //==========================================================================
    // Offline render mode.

    double spectralCentroid(const Signal& s, double sr)
    {
        constexpr int order = 12, size = 1 << order;
        juce::dsp::FFT fft(order);
        std::vector<float> data(static_cast<size_t>(2 * size));
        std::vector<double> power(static_cast<size_t>(size / 2 + 1), 0.0);
        for (size_t start = 0; start + size <= s.size(); start += size / 2)
        {
            std::fill(data.begin(), data.end(), 0.0f);
            for (int i = 0; i < size; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(c_pi) * static_cast<float>(i) / static_cast<float>(size));
                data[static_cast<size_t>(i)] = s[start + static_cast<size_t>(i)] * w;
            }
            fft.performFrequencyOnlyForwardTransform(data.data());
            for (int k = 0; k <= size / 2; ++k)
                power[static_cast<size_t>(k)] += static_cast<double>(data[static_cast<size_t>(k)]) * data[static_cast<size_t>(k)];
        }
        double num = 0.0, den = 0.0;
        for (int k = 1; k <= size / 2; ++k)
        {
            num += power[static_cast<size_t>(k)] * k * sr / size;
            den += power[static_cast<size_t>(k)];
        }
        return den > 0.0 ? num / den : 0.0;
    }

    int renderFiles(const juce::File& input, const juce::File& outDir, juce::StringArray presetNames)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(input));
        if (reader == nullptr)
        {
            std::printf("cannot read %s\n", input.getFullPathName().toRawUTF8());
            return 1;
        }

        const double sr = reader->sampleRate;
        const int length = static_cast<int>(reader->lengthInSamples);
        juce::AudioBuffer<float> file(static_cast<int>(reader->numChannels), length);
        reader->read(&file, 0, length, 0, true, true);

        Stereo in;
        in.l.assign(file.getReadPointer(0), file.getReadPointer(0) + length);
        in.r = file.getNumChannels() > 1 ? Signal(file.getReadPointer(1), file.getReadPointer(1) + length) : in.l;

        if (presetNames.isEmpty())
            presetNames = { "Init", "Liquid Beast", "Kaiju", "Pulse Choir", "Detail Only" };

        outDir.createDirectory();

        const auto inStats = stats(in);
        std::printf("input %s: %.2f s @ %.0f Hz, rms %.1f dBFS, peak %.1f dBFS, centroid %.0f Hz\n",
                    input.getFileName().toRawUTF8(), length / sr, sr, toDb(inStats.rms), toDb(inStats.peak),
                    spectralCentroid(in.l, sr));

        const auto& presets = PresetManager::getFactoryPresets();
        int failures = 0;
        for (const auto& name : presetNames)
        {
            int index = -1;
            for (size_t i = 0; i < presets.size(); ++i)
                if (name == presets[i].name)
                    index = static_cast<int>(i);
            if (index < 0)
            {
                std::printf("no factory preset \"%s\"\n", name.toRawUTF8());
                ++failures;
                continue;
            }

            auto p = makeProcessor(sr, 512);
            p->setCurrentProgram(index);
            p->reset();
            const int latency = p->getLatencySamples();

            // Pad with the latency plus a tail, then drop the latency so the file lines up with the input.
            Stereo padded = in;
            const int extra = latency + static_cast<int>(0.3 * sr);
            padded.l.resize(static_cast<size_t>(length + extra), 0.0f);
            padded.r.resize(static_cast<size_t>(length + extra), 0.0f);
            auto out = render(*p, padded, 512);
            out.l.erase(out.l.begin(), out.l.begin() + latency);
            out.r.erase(out.r.begin(), out.r.begin() + latency);

            const auto st = stats(out);
            Signal mono(out.l.size());
            for (size_t i = 0; i < mono.size(); ++i)
                mono[i] = 0.5f * (out.l[i] + out.r[i]);
            double sideSq = 0.0, midSq = 0.0;
            for (size_t i = 0; i < mono.size(); ++i)
            {
                const double side = 0.5 * (out.l[i] - out.r[i]);
                sideSq += side * side;
                midSq += static_cast<double>(mono[i]) * mono[i];
            }
            const double centroid = spectralCentroid(mono, sr);

            const juce::File target = outDir.getChildFile(name.replaceCharacter(' ', '-').toLowerCase() + ".wav");
            target.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream(target.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> writer(stream != nullptr
                ? wav.createWriterFor(stream.get(), sr, 2, 24, {}, 0) : nullptr);
            if (writer != nullptr)
            {
                stream.release();
                const float* channels[] { out.l.data(), out.r.data() };
                writer->writeFromFloatArrays(channels, 2, static_cast<int>(out.l.size()));
            }

            const bool broken = !st.finite || st.rms < 1.0e-3 || st.peak > 1.0001f || std::abs(st.mean) > 0.01;
            failures += broken ? 1 : 0;
            std::printf("%-14s rms %6.1f dBFS  peak %6.1f dBFS  DC %+.5f  centroid %5.0f Hz  side/mid %5.1f dB  %s -> %s\n",
                        name.toRawUTF8(), toDb(st.rms), toDb(st.peak), st.mean, centroid,
                        toDb(std::sqrt(sideSq / std::max(midSq, 1.0e-20))), broken ? "BROKEN" : "ok",
                        target.getFullPathName().toRawUTF8());
        }
        return failures == 0 ? 0 : 1;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc >= 4 && std::string(argv[1]) == "--render")
    {
        juce::StringArray names;
        for (int i = 4; i < argc; ++i)
            names.add(argv[i]);
        return renderFiles(juce::File(argv[2]), juce::File(argv[3]), names);
    }

    const auto t0 = std::chrono::steady_clock::now();

    testLatency();
    testNulls();
    testMixResponse();
    testNonFiniteInput();
    testVocoderLowRates();
    testEditorText();
    testAutoGainStart();
    testState();
    testSlotMoves();
    testPresets();
    testGrid();
    testCpu();

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%d checks, %d failed (%.1f s)\n", g_checks, g_failures, seconds);
    return g_failures == 0 ? 0 : 1;
}

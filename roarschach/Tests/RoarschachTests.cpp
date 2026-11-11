// Offline tests for Roarschach: drive the real AudioProcessor at several sample rates and block
// sizes and check what the spec promises: the parameter table, constant latency for any order,
// exact nulls, click-free reorders, a maximizer that never passes its ceiling, the state round
// trip, every preset in every order, the native-rate pitch path, and the CPU budget.
//
//   RoarschachTests [--quick] [test ...]   run every test (or the named ones), non-zero exit on failure
//   RoarschachTests --snapshot <out.png>   a picture of the editor
//   RoarschachTests --snapshots <dir>      the editor on screen, playing: the whole editor (with the
//                                          creature), presets, menus, a tooltip, a drag, minimum size
//   RoarschachTests --render <in.wav> <outdir> [--preset NAME]... [--only XX]... [--without XX]...
//                   [--order "PI CL ..."]
//                                          render a file at its own sample rate (no resampling),
//                                          latency-trimmed, 24-bit, through factory presets
//                   [--set id=value]...    after the preset: a real value, a choice name, On/Off,
//                                          or @0.75 for a knob position (0..1)
//                   [--metrics out.json]   measurements per render (Tests/RenderMetrics.h, and
//                                          grP95Db: OUTPUT's gain reduction, 95th percentile)
//                   [--sweep id from to steps [--norm]] [--no-wav] [--raw-out]
//                                          the same input at each value of one parameter (real
//                                          values, or knob positions with --norm); metrics go to
//                                          --metrics or <outdir>/sweep-<id>.json; --raw-out turns
//                                          OUTPUT's filters, auto gain and Ride off
//                   [--roll from to]       one render per seed from..to: the preset (and sets),
//                                          then ROLL ALL with that seed (the dice's batch check)

#if defined(__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>   // the snapshot harness runs the run loop (before JUCE: it declares Point)
#endif

#include <JuceHeader.h>

#if defined(__APPLE__)
 #define GL_SILENCE_DEPRECATION 1
 #include <OpenGL/OpenGL.h>   // an offscreen context for the renderer test (after JUCE: it declares the GL types)
#endif

#include "CreatureEvolution.h"
#include "CreatureMood.h"
#include "CreatureMotion.h"
#include "MonsterRenderer.h"
#include "NoiseTexture.h"
#include "LogoSdf.h"
#include "Params.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "PresetManager.h"
#include "dsp/common/ChaCha20Poly1305.h"
#include "dsp/common/IrPack.h"
#include "dsp/common/ZeroLatencyConvolver.h"
#include "dsp/modules/ChamberModule.h"
#include "dsp/modules/CleanModule.h"
#include "dsp/modules/InModule.h"
#include "dsp/modules/TameModule.h"
#include "dsp/KnobLaws.h"
#include "RenderMetrics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

// The editor's parts, for the editor test and the snapshot harness (a friend of the editor).
struct RoarschachEditorInspector
{
    static SignalFlowStrip& strip(RoarschachAudioProcessorEditor& e) { return e.m_strip; }
    static MonsterView& monster(RoarschachAudioProcessorEditor& e) { return e.m_monsterView; }
    static juce::Component& content(RoarschachAudioProcessorEditor& e) { return e.m_content; }
    static juce::LookAndFeel& lookAndFeel(RoarschachAudioProcessorEditor& e) { return e.m_lookAndFeel; }
    static DiceButton& presetDice(RoarschachAudioProcessorEditor& e) { return e.m_presetDice; }
};

namespace
{
    int g_failures = 0;
    int g_checks = 0;
    bool g_quick = false;

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
    constexpr std::array<double, 4> c_rates { 44100.0, 48000.0, 96000.0, 192000.0 };

    double toDb(double x)
    {
        return 20.0 * std::log10(std::max(x, 1.0e-15));
    }

    //==========================================================================
    // Test signals (mono).

    void normalisePeak(Signal& s, float peak)
    {
        float p = 0.0f;
        for (float x : s)
            p = std::max(p, std::abs(x));
        if (p > 0.0f)
            for (auto& x : s)
                x *= peak / p;
    }

    /**
     * The reference voice (spec §11.1): an /a/ vowel (F1 700, F2 1220, F3 2600 Hz) on a glottal pulse
     * at f0 (1 % jitter), in syllables of 260 ms alternating with 4-12 kHz noise consonants and
     * -45 dBFS breaths. Voiced RMS -20 dBFS, peaks about -9 dBFS.
     */
    Signal makeVoice(double sr, double seconds, double f0 = 110.0, unsigned seed = 7)
    {
        const int n = static_cast<int>(sr * seconds);
        Signal s(static_cast<size_t>(n), 0.0f);
        std::mt19937 rng(seed);
        std::normal_distribution<double> gauss(0.0, 1.0);

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
        juce::IIRFilter hp, lp;
        hp.setCoefficients(juce::IIRCoefficients::makeHighPass(sr, 4000.0));
        lp.setCoefficients(juce::IIRCoefficients::makeLowPass(sr, std::min(12000.0, 0.45 * sr)));

        const int syllable = static_cast<int>(0.26 * sr), consonant = static_cast<int>(0.07 * sr), breath = static_cast<int>(0.12 * sr);
        const int cycle = syllable + consonant + breath;

        Signal voiced(static_cast<size_t>(n), 0.0f);
        std::vector<int> kind(static_cast<size_t>(n), 0);
        double phase = 0.0, prevPulse = 0.0, period = 1.0 / f0, jitter = 1.0;
        for (int i = 0; i < n; ++i)
        {
            const int pos = i % cycle;
            phase += jitter / (period * sr);
            if (phase >= 1.0)
            {
                phase -= 1.0;
                jitter = 1.0 + 0.01 * gauss(rng);
            }
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
            if (pos < syllable)
            {
                const double t = static_cast<double>(pos) / syllable;
                const double env = std::sin(c_pi * t);
                voiced[static_cast<size_t>(i)] = static_cast<float>(y * (0.35 + 0.65 * env));
                kind[static_cast<size_t>(i)] = 1;
            }
            else
            {
                kind[static_cast<size_t>(i)] = pos < syllable + consonant ? 2 : 3;
            }
        }

        // Voiced RMS to -20 dBFS.
        double sq = 0.0;
        int count = 0;
        for (int i = 0; i < n; ++i)
            if (kind[static_cast<size_t>(i)] == 1)
            {
                sq += static_cast<double>(voiced[static_cast<size_t>(i)]) * voiced[static_cast<size_t>(i)];
                ++count;
            }
        const double gain = count > 0 && sq > 0.0 ? 0.1 / std::sqrt(sq / count) : 1.0;

        for (int i = 0; i < n; ++i)
        {
            const int pos = i % cycle;
            const auto k = kind[static_cast<size_t>(i)];
            if (k == 1)
                s[static_cast<size_t>(i)] = static_cast<float>(voiced[static_cast<size_t>(i)] * gain);
            else
            {
                const float noise = lp.processSingleSampleRaw(hp.processSingleSampleRaw(static_cast<float>(gauss(rng))));
                if (k == 2)
                {
                    const double t = static_cast<double>(pos - syllable) / consonant;
                    s[static_cast<size_t>(i)] = static_cast<float>(0.06 * std::sin(c_pi * t)) * noise;
                }
                else
                {
                    s[static_cast<size_t>(i)] = static_cast<float>(0.0056 * gauss(rng) * 0.5);   // about -45 dBFS
                }
            }
        }
        return s;
    }

    Signal makeSine(double sr, double seconds, double hz, double amplitude)
    {
        const int n = static_cast<int>(sr * seconds);
        Signal s(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            s[static_cast<size_t>(i)] = static_cast<float>(amplitude * std::sin(2.0 * c_pi * hz * i / sr));
        return s;
    }

    Signal makeWhite(double sr, double seconds, float amplitude, unsigned seed = 3)
    {
        const int n = static_cast<int>(sr * seconds);
        Signal s(static_cast<size_t>(n));
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        for (auto& x : s)
            x = amplitude * dist(rng);
        return s;
    }

    Signal makePink(double sr, double seconds, float peak, unsigned seed = 5)
    {
        const int n = static_cast<int>(sr * seconds);
        Signal s(static_cast<size_t>(n));
        std::mt19937 rng(seed);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (auto& x : s)
        {
            const float w = dist(rng);
            b0 = 0.99886f * b0 + w * 0.0555179f;
            b1 = 0.99332f * b1 + w * 0.0750759f;
            b2 = 0.96900f * b2 + w * 0.1538520f;
            b3 = 0.86650f * b3 + w * 0.3104856f;
            b4 = 0.55000f * b4 + w * 0.5329522f;
            b5 = -0.7616f * b5 - w * 0.0168980f;
            x = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
            b6 = w * 0.115926f;
        }
        normalisePeak(s, peak);
        return s;
    }

    Signal makeSquare(double sr, double seconds, double hz, float amplitude)
    {
        const int n = static_cast<int>(sr * seconds);
        Signal s(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            s[static_cast<size_t>(i)] = std::fmod(i * hz / sr, 1.0) < 0.5 ? amplitude : -amplitude;
        return s;
    }

    Signal makeSweep(double sr, double seconds, float amplitude)
    {
        const int n = static_cast<int>(sr * seconds);
        Signal s(static_cast<size_t>(n));
        const double f0 = 30.0, f1 = std::min(18000.0, 0.45 * sr);
        const double k = std::log(f1 / f0) / seconds;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            s[static_cast<size_t>(i)] = static_cast<float>(amplitude * std::sin(2.0 * c_pi * f0 * (std::exp(k * t) - 1.0) / k));
        }
        return s;
    }

    Signal makeClicks(double sr, double seconds, float amplitude)
    {
        const int n = static_cast<int>(sr * seconds);
        Signal s(static_cast<size_t>(n), 0.0f);
        const int period = static_cast<int>(0.05 * sr);
        for (int i = 0; i < n; i += period)
        {
            s[static_cast<size_t>(i)] = amplitude;
            if (i + 1 < n)
                s[static_cast<size_t>(i + 1)] = -amplitude;
        }
        return s;
    }

    Stereo dual(const Signal& s)
    {
        return { s, s };
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

    void setOrder(RoarschachAudioProcessor& p, const Params::Order& order)
    {
        // 1.0's modules: their place among those 14; CHAMBER: its insertion slot.
        const auto positions = Params::positionsFor(order);
        for (int m = 0; m < Params::numModules; ++m)
            setParam(p, Params::modules[static_cast<size_t>(m)].posId, static_cast<float>(positions[static_cast<size_t>(m)]));
    }

    int slotOf(const Params::Order& order, Params::ModuleId id)
    {
        return static_cast<int>(std::find(order.begin(), order.end(), static_cast<int>(id)) - order.begin());
    }

    /** 1.0's 14 positions are a permutation of 0..13 and CHAMBER's insertion slot is in 0..14. */
    bool positionsAreValid(const Params::Order& positions)
    {
        std::array<bool, Params::numLegacyModules> seen {};
        for (int m = 0; m < Params::numLegacyModules; ++m)
        {
            const int pos = positions[static_cast<size_t>(m)];
            if (!juce::isPositiveAndBelow(pos, Params::numLegacyModules) || seen[static_cast<size_t>(pos)])
                return false;
            seen[static_cast<size_t>(pos)] = true;
        }
        const int slot = positions[static_cast<size_t>(Params::ModuleId::chamber)];
        return slot >= 0 && slot <= Params::numLegacyModules;
    }

    Params::Order orderFromCodes(const juce::String& codes)
    {
        Params::Order order {};
        std::array<bool, Params::numModules> used {};
        int slot = 0;
        for (const auto& token : juce::StringArray::fromTokens(codes, " ,", ""))
        {
            const int m = Params::moduleIndexForCode(token.trim().toUpperCase().toStdString());
            if (m >= 0 && !used[static_cast<size_t>(m)] && slot < Params::numModules)
            {
                used[static_cast<size_t>(m)] = true;
                order[static_cast<size_t>(slot++)] = m;
            }
        }
        // CHAMBER not named: its default slot, right after the first module (as the presets do).
        constexpr int chamber = static_cast<int>(Params::ModuleId::chamber);
        if (!used[static_cast<size_t>(chamber)] && slot >= Params::numLegacyModules)
        {
            const int at = static_cast<int>(Params::defaultOf(Params::chamberPos));
            std::copy_backward(order.begin() + at, order.begin() + slot, order.begin() + slot + 1);
            order[static_cast<size_t>(at)] = chamber;
            used[static_cast<size_t>(chamber)] = true;
            ++slot;
        }
        // Modules not named keep their reference order after the named ones.
        for (int m = 0; m < Params::numModules; ++m)
            if (!used[static_cast<size_t>(m)])
                order[static_cast<size_t>(slot++)] = m;
        return order;
    }

    Params::Order randomOrder(std::mt19937& rng)
    {
        Params::Order order {};
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), rng);
        return order;
    }

    juce::String orderCodes(const Params::Order& order)
    {
        juce::StringArray codes;
        for (int m : order)
            codes.add(Params::modules[static_cast<size_t>(m)].code);
        return codes.joinIntoString(" ");
    }

    void setAllModules(RoarschachAudioProcessor& p, bool on)
    {
        for (const auto& m : Params::modules)
            setParam(p, m.onId, on ? 1.0f : 0.0f);
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

    /** Re-prepares after parameter changes, so the first chunk snaps to them (no fades). */
    void restart(RoarschachAudioProcessor& p, double sr, int block)
    {
        p.prepareToPlay(sr, block);
    }

    int presetIndex(const juce::String& name)
    {
        const auto& presets = PresetManager::getFactoryPresets();
        for (size_t i = 0; i < presets.size(); ++i)
            if (name.equalsIgnoreCase(presets[i].name))
                return static_cast<int>(i);
        return -1;
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
            for (size_t i = static_cast<size_t>(std::max(0, from)); i < ch->size(); ++i)
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

    /** Peak of the difference of a and b (b shifted by `lag`), dB re a's RMS, from `from`. */
    double nullDb(const Signal& a, const Signal& b, int lag, int from)
    {
        double err = 0.0, ref = 0.0;
        for (int i = from; i < static_cast<int>(a.size()); ++i)
        {
            const int j = i - lag;
            const double y = j >= 0 && j < static_cast<int>(b.size()) ? b[static_cast<size_t>(j)] : 0.0;
            err = std::max(err, std::abs(static_cast<double>(a[static_cast<size_t>(i)]) - y));
            ref += y * y;
        }
        ref = std::sqrt(ref / std::max(1, static_cast<int>(a.size()) - from));
        return toDb(err) - toDb(ref);
    }

    /**
     * True peak with an 8x windowed-sinc interpolator. The default is the meter the OUTPUT plan's
     * targets were set with (a polyphase resampler: 10 zero crossings a side, Kaiser beta 5);
     * strict = 16 zero crossings, beta 8, closer to ideal reconstruction near Nyquist.
     */
    double truePeak(const Signal& s, bool strict = false)
    {
        constexpr int factor = 8;
        struct Kernel { int half; std::vector<double> taps; };
        const auto make = [](int half, double beta)
        {
            Kernel k { half, std::vector<double>(static_cast<size_t>(2 * half * factor + 1)) };
            const auto bessel = [](double x)
            {
                double sum = 1.0, term = 1.0;
                for (int i = 1; i < 30; ++i)
                {
                    term *= (x / (2.0 * i)) * (x / (2.0 * i));
                    sum += term;
                }
                return sum;
            };
            for (int i = 0; i < static_cast<int>(k.taps.size()); ++i)
            {
                const double t = static_cast<double>(i - half * factor) / factor;
                const double sinc = std::abs(t) < 1e-12 ? 1.0 : std::sin(c_pi * t) / (c_pi * t);
                const double r = static_cast<double>(i - half * factor) / (half * factor);
                k.taps[static_cast<size_t>(i)] = sinc * bessel(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel(beta);
            }
            return k;
        };
        static const Kernel planMeter = make(10, 5.0), strictMeter = make(16, 8.0);
        const Kernel& kernel = strict ? strictMeter : planMeter;
        const int half = kernel.half;

        // Samples count everywhere; between samples only where the whole kernel lies inside the
        // signal (at the very ends a truncated kernel rings: a render's last sample is not a stop).
        double peak = 0.0;
        const int n = static_cast<int>(s.size());
        for (int i = 0; i < n; ++i)
        {
            peak = std::max(peak, static_cast<double>(std::abs(s[static_cast<size_t>(i)])));
            if (i < half || i + half >= n)
                continue;
            for (int p = 1; p < factor; ++p)
            {
                double acc = 0.0;
                for (int k = -half + 1; k <= half; ++k)
                    acc += s[static_cast<size_t>(i + k)] * kernel.taps[static_cast<size_t>((half - k) * factor + p)];
                peak = std::max(peak, std::abs(acc));
            }
        }
        return peak;
    }

    /** Power spectrum (Blackman-Harris, one frame of 2^order from `start`), bin k at k sr / 2^order. */
    std::vector<double> spectrum(const Signal& s, int start, int order)
    {
        const int size = 1 << order;
        juce::dsp::FFT fft(order);
        std::vector<float> data(static_cast<size_t>(2 * size), 0.0f);
        for (int i = 0; i < size && start + i < static_cast<int>(s.size()); ++i)
        {
            const double t = 2.0 * c_pi * i / size;
            const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
            data[static_cast<size_t>(i)] = static_cast<float>(s[static_cast<size_t>(start + i)] * w);
        }
        fft.performFrequencyOnlyForwardTransform(data.data());
        std::vector<double> power(static_cast<size_t>(size / 2 + 1));
        for (int k = 0; k <= size / 2; ++k)
            power[static_cast<size_t>(k)] = static_cast<double>(data[static_cast<size_t>(k)]) * data[static_cast<size_t>(k)];
        return power;
    }

    double bandPower(const std::vector<double>& power, double sr, int order, double lo, double hi)
    {
        const double binHz = sr / (1 << order);
        double sum = 0.0;
        for (size_t k = 0; k < power.size(); ++k)
        {
            const double f = static_cast<double>(k) * binHz;
            if (f >= lo && f < hi)
                sum += power[k];
        }
        return sum;
    }

    /** Amplitude of the hz component of s[from..to) (a correlation, so other components cancel). */
    double toneAmplitude(const Signal& s, double sr, double hz, int from, int to)
    {
        double c = 0.0, q = 0.0;
        for (int i = from; i < to; ++i)
        {
            const double ph = 2.0 * c_pi * hz * i / sr;
            c += s[static_cast<size_t>(i)] * std::cos(ph);
            q += s[static_cast<size_t>(i)] * std::sin(ph);
        }
        return 2.0 * std::sqrt(c * c + q * q) / std::max(1, to - from);
    }

    float ceilingGain(float ceilingDb)
    {
        return std::pow(10.0f, ceilingDb / 20.0f);
    }

    // The neutral setting of every module (spec §11.3). ROAR is never neutral (it is skipped).
    struct Neutral
    {
        Params::ModuleId module;
        Values values;
    };

    const std::vector<Neutral>& neutralSettings()
    {
        using namespace Params;
        static const std::vector<Neutral> table {
            { ModuleId::pitch, { { pitch, 0.0f }, { pitchSize, 0.0f }, { pitchDrive, 0.0f }, { pitchMode, 0.0f }, { pitchFormant, 0.0f } } },
            { ModuleId::clean, { { cleanContain, 0.0f }, { cleanGate, 0.0f }, { cleanTone, 0.0f } } },
            { ModuleId::deess, { { deess, 0.0f } } },
            { ModuleId::detail, { { detailLow, 0.0f }, { detailHigh, 0.0f } } },
            { ModuleId::vocoder, { { gravel, 0.0f } } },
            { ModuleId::ring, { { ring, 0.0f }, { ringBody, 0.0f } } },
            { ModuleId::shimmer, { { shimmer, 0.0f } } },
            { ModuleId::growl, { { growl, 0.0f } } },
            { ModuleId::swirl, { { swirl, 0.0f } } },
            { ModuleId::ensemble, { { ensemble, 0.0f } } },
            { ModuleId::tame, { { tame, 0.0f } } },
            { ModuleId::sub, { { sub, 0.0f } } },
            { ModuleId::overtone, { { overtone, 0.0f }, { overtoneMode, 0.0f } } },
        };
        return table;
    }

    int expectedLatency(double fs)
    {
        const int pitch = static_cast<int>(std::ceil(0.030 * fs)) + 23;
        const int detail = static_cast<int>(std::lround(1.333e-3 * fs));
        const int output = static_cast<int>(std::lround(0.010 * fs)) + 10 + 49 + static_cast<int>(std::lround(0.0005 * fs)) + 10;
        return pitch + 4 * 23 + detail + output;   // GROWL, SWIRL, ROAR, RING: 23 each
    }

    /** The phase TAME's split gives every signal (it runs even when TAME is Off). */
    Signal tameAllpass(const Signal& in, double sr)
    {
        roar::Allpass4 a, b;
        a.prepare(sr, roar::TameModule::c_crossoverLowHz);
        b.prepare(sr, roar::TameModule::c_crossoverHighHz);
        Signal out(in.size());
        for (size_t i = 0; i < in.size(); ++i)
            out[i] = static_cast<float>(b.process(a.process(static_cast<double>(in[i]))));
        return out;
    }

    //==========================================================================
    // Tests.

    void testParameters()
    {
        std::printf("[params] table, AU order, ranges, defaults, text round trip\n");
        auto p = makeProcessor(48000.0, 512);
        const auto& params = p->getParameters();
        check(params.size() == Params::numParameters, "parameter count " + juce::String(params.size()) + " (87)");

        int textFailures = 0;
        for (int i = 0; i < std::min(params.size(), Params::numParameters); ++i)
        {
            const auto& spec = Params::all[static_cast<size_t>(i)];
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(params[i]);
            if (ranged == nullptr)
            {
                check(false, "not ranged: " + juce::String(spec.id));
                continue;
            }
            check(ranged->getParameterID() == spec.id, "AU index " + juce::String(i) + " is " + spec.id);
            const auto& range = ranged->getNormalisableRange();
            check(std::abs(range.start - spec.minValue) < 1e-6f && std::abs(range.end - spec.maxValue) < 1e-6f,
                  juce::String(spec.id) + " range");
            const float def = ranged->convertFrom0to1(ranged->getDefaultValue());
            check(std::abs(def - spec.defaultValue) < 1e-4f * std::max(1.0f, std::abs(spec.defaultValue)),
                  juce::String(spec.id) + " default " + juce::String(def));

            if (spec.kind == Params::Kind::choice || spec.kind == Params::Kind::toggle)
                continue;
            for (float u : { 0.0f, 0.13f, 0.5f, 0.77f, 1.0f })
            {
                const float plain = ranged->convertFrom0to1(u);
                const auto text = ranged->getText(ranged->convertTo0to1(plain), 0);
                const float back = ranged->convertFrom0to1(ranged->getValueForText(text));
                const bool wholePercent = spec.kind == Params::Kind::percent || spec.kind == Params::Kind::bipolar
                                          || spec.kind == Params::Kind::position;   // displayed as whole numbers
                // Milliseconds from 10 ms up display whole milliseconds (the log taper's mid point,
                // 31.6 ms, reads "32 ms").
                const bool wholeMs = spec.kind == Params::Kind::milliseconds && plain >= 10.0f && plain < 1000.0f;
                const float tolerance = std::max({ wholePercent || wholeMs ? 0.51f : 0.051f, std::abs(plain) * 0.01f, spec.step });
                if (std::abs(back - plain) > tolerance)
                {
                    ++textFailures;
                    std::printf("    %s: %.4f -> \"%s\" -> %.4f\n", spec.id, plain, text.toRawUTF8(), back);
                }
            }
        }
        check(textFailures == 0, juce::String(textFailures) + " text round trips off");

        // Only the log kinds (hertz, ratio, milliseconds) and the decibel knobs with a centre (Push,
        // Clip) have a non-linear taper: the rest are linear, so Pitch's -24..-12 st octave gets a
        // third of the travel. (Push and Clip were linear in the first release; their skew puts
        // the useful range, 10 dB and 5 dB, at mid travel.)
        for (const auto& spec : Params::all)
        {
            if (spec.kind == Params::Kind::choice || spec.kind == Params::Kind::toggle || spec.kind == Params::Kind::position)
                continue;
            const bool logTaper = spec.kind == Params::Kind::hertz || spec.kind == Params::Kind::ratio || spec.kind == Params::Kind::milliseconds;
            if (logTaper || (spec.kind == Params::Kind::decibels && spec.centre != 0.0f))
                continue;
            auto* param = p->getValueTreeState().getParameter(spec.id);
            const float mid = 0.5f * (spec.minValue + spec.maxValue);
            check(std::abs(param->convertTo0to1(mid) - 0.5f) < 1e-4f, juce::String(spec.id) + " is linear (mid value at "
                                                                         + juce::String(param->convertTo0to1(mid), 3) + ")");
        }
        {
            auto* pitch = p->getValueTreeState().getParameter(Params::pitch);
            check(std::abs(pitch->convertTo0to1(-6.0f) - 0.5f) < 1e-4f && std::abs(pitch->convertTo0to1(-12.0f) - 1.0f / 3.0f) < 1e-4f,
                  "Pitch is linear: -6 st at the middle, -12 st at a third");
        }

        // A few display formats from the spec.
        const auto text = [&p](const char* id, float v)
        {
            auto* param = p->getValueTreeState().getParameter(id);
            return param->getText(param->convertTo0to1(v), 0);
        };
        check(text(Params::outPush, 3.0f) == "+3.0 dB", "Push text " + text(Params::outPush, 3.0f));
        check(text(Params::pitch, -2.0f) == "-2.00 st", "Pitch text " + text(Params::pitch, -2.0f));
        check(text(Params::outClip, 0.0f) == "Off", "Clip text at 0");
        check(text(Params::outClipShape, 0.0f) == "Soft" && text(Params::outClipShape, 100.0f) == "Hard", "Clip Shape ends");
        check(text(Params::outRelease, 60.0f) == "60 ms" && text(Params::outRelease, 1000.0f) == "1.00 s", "Release text");
        // (Attack now starts at 0.2 ms: below that it was dead.)
        check(text(Params::outAttack, 2.0f) == "2.0 ms" && text(Params::outAttack, 0.2f) == "0.20 ms", "Attack text");
        check(text(Params::ringFreq, 1200.0f) == "1.20 kHz" && text(Params::ringFreq, 150.0f) == "150.0 Hz", "Ring Freq text");
        check(text(Params::ringRatio, 1.5f) == "x1.50", "Ring Ratio text");
        check(text(Params::overtoneFreq, 0.25f) == "+0.25 oct", "Overtone Frequency text");
        check(text(Params::roarTone, -40.0f) == "Bass 40%" && text(Params::roarTone, 0.0f) == "Even", "Roar Tone text");
        check(text(Params::cleanPos, 13.0f) == "14th", "position text");
        check(text(Params::pitch, -24.0f) == "-24.00 st", "Pitch reaches -24 st");

        // Skewed knobs put the named value at mid travel.
        for (const char* id : { Params::outAttack, Params::outRelease, Params::ringFreq, Params::ringRatio, Params::growlRate })
        {
            auto* param = p->getValueTreeState().getParameter(id);
            const float mid = param->convertFrom0to1(0.5f);
            check(std::abs(mid - Params::spec(id).centre) < 0.02f * Params::spec(id).centre, juce::String(id) + " mid travel " + juce::String(mid));
        }
    }

    void testLatency()
    {
        std::printf("[latency] constant for every rate, order, block size and setting\n");
        std::mt19937 rng(11);
        const std::array<int, 5> blocks { 1, 64, 480, 512, 2049 };

        for (double sr : c_rates)
        {
            auto p = makeProcessor(sr, 512);
            const int latency = p->getLatencySamples();
            check(latency == expectedLatency(sr), juce::String(sr) + " Hz: latency " + juce::String(latency)
                                                      + " (formula " + juce::String(expectedLatency(sr)) + ")");
            std::printf("  %6.0f Hz: %d samples (%.2f ms)\n", sr, latency, 1000.0 * latency / sr);

            const int perms = g_quick ? 3 : 10;
            int wrong = 0;
            for (int t = 0; t < perms; ++t)
            {
                for (int block : blocks)
                {
                    auto q = makeProcessor(sr, block);
                    q->getChainForTests().setOutputTestOptions(false, false, false);
                    setAllModules(*q, false);
                    setParam(*q, Params::outPush, 0.0f);
                    setOrder(*q, randomOrder(rng));
                    // Every On and choice toggled once (Off modules still delay by their latency).
                    for (const auto& spec : Params::all)
                        if (spec.kind == Params::Kind::choice)
                            setParam(*q, spec.id, static_cast<float>(rng() % static_cast<unsigned>(spec.numChoices)));
                    restart(*q, sr, block);
                    check(q->getLatencySamples() == latency, "latency changed with the settings");

                    const int at = static_cast<int>(0.05 * sr);
                    Stereo in = dual(Signal(static_cast<size_t>(at + latency + 2048), 0.0f));
                    in.l[static_cast<size_t>(at)] = in.r[static_cast<size_t>(at)] = 0.5f;
                    const auto out = render(*q, in, block);
                    // Every module Off is a pure delay except TAME's band split, which always runs
                    // (an exact allpass, AP4 at 2.5 kHz x AP4 at 6.5 kHz): the reference gets it too.
                    const auto reference = tameAllpass(in.l, sr);
                    const double db = nullDb(out.l, reference, latency, 0);
                    if (db > -100.0)
                    {
                        ++wrong;
                        std::printf("    %.0f Hz block %d: impulse vs delayed allpass %.1f dB\n", sr, block, db);
                    }
                }
            }
            check(wrong == 0, juce::String(sr) + " Hz: impulse delay != latency in " + juce::String(wrong) + " renders");
        }
    }

    void testNulls()
    {
        std::printf("[null] bypass, every module alone at its neutral setting, order independence\n");
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            const auto voice = makeVoice(sr, 1.6);
            const auto in = dual(voice);
            const int skip = static_cast<int>(0.4 * sr);

            // Host bypass: the input delayed by the latency, bit for bit.
            {
                auto p = makeProcessor(sr, 512);
                const auto out = render(*p, in, 512, true);
                check(nullDb(out.l, voice, p->getLatencySamples(), 0) < -200.0, juce::String(sr) + " Hz bypass is not a pure delay");
            }

            // Reference: every module Off (OUTPUT linear: no auto gain, no Ride, no filters, Push 0).
            const auto prepareLinear = [](RoarschachAudioProcessor& p)
            {
                p.getChainForTests().setOutputTestOptions(false, false, false);
                setAllModules(p, false);
                setParam(p, Params::outPush, 0.0f);
                setParam(p, Params::inAuto, 0.0f);
            };
            auto ref = makeProcessor(sr, 512);
            prepareLinear(*ref);
            restart(*ref, sr, 512);
            const auto allOff = render(*ref, in, 512);
            const int latency = ref->getLatencySamples();
            {
                // All Off is the input through TAME's allpass split (it runs even when Off), delayed.
                const double db = nullDb(allOff.l, tameAllpass(voice, sr), latency, skip);
                std::printf("  %6.0f Hz all Off vs the delayed, allpassed input: %.1f dB\n", sr, db);
                check(db < -100.0, "all Off is not a delayed allpass");
            }

            for (const auto& neutral : neutralSettings())
            {
                const auto& spec = Params::module(neutral.module);
                auto p = makeProcessor(sr, 512);
                prepareLinear(*p);
                setParam(*p, spec.onId, 1.0f);
                setValues(*p, neutral.values);
                restart(*p, sr, 512);
                const auto out = render(*p, in, 512);

                Signal reference = allOff.l;
                if (neutral.module == Params::ModuleId::clean)
                {
                    // CLEAN Flat keeps its 40 Hz high-pass: the reference gets the same filter.
                    std::array<roar::Biquad::Coeffs, 2> hp;
                    const int k = roar::CleanModule::highPassSections(sr, Params::CleanTone::flat, hp);
                    for (int s = 0; s < k; ++s)
                    {
                        roar::Biquad f;
                        f.setCoeffs(hp[static_cast<size_t>(s)]);
                        for (auto& x : reference)
                            x = f.process(x);
                    }
                }
                const double db = nullDb(out.l, reference, 0, skip);
                const double dbR = nullDb(out.r, reference, 0, skip);
                check(std::max(db, dbR) <= -90.0, juce::String(sr) + " Hz " + spec.uiName + " neutral null " + juce::String(std::max(db, dbR), 1) + " dB");
                if (g_quick || sr == 48000.0)
                    std::printf("  %6.0f Hz %-9s neutral null %7.1f dB\n", sr, spec.uiName, std::max(db, dbR));
            }

            // Order independence: with every module Off, any order renders the same, bit for bit.
            std::mt19937 rng(static_cast<unsigned>(sr));
            for (int t = 0; t < 3; ++t)
            {
                auto p = makeProcessor(sr, 512);
                prepareLinear(*p);
                setOrder(*p, randomOrder(rng));
                restart(*p, sr, 512);
                const auto out = render(*p, in, 512);
                check(nullDb(out.l, allOff.l, 0, 0) < -200.0, "all Off depends on the order");
            }
        }
    }

    void testMixPhase()
    {
        std::printf("[mix] a partial Mix does not comb (dry follows the wet's fixed filters)\n");
        const double sr = 48000.0;
        const auto configure = [](RoarschachAudioProcessor& p, const char* tone)
        {
            if (juce::String(tone) == "Boom")
            {
                // CHAMBER at Chamber 0 % (the direct path only) with Boom Heavy: the shelf's phase is
                // not matched on the dry (DryPhaseMatch says why); its worst blend loss is bounded.
                setValues(p, { { Params::chamberOn, 1.0f }, { Params::chamber, 0.0f }, { Params::chamberBoom, 3.0f },
                               { Params::chamberDrive, 0.0f } });
            }
            else if (juce::String(tone) != "Off")
            {
                // CLEAN at no dynamics, only its tone; the dry gets the tone's high-pass phase.
                setValues(p, { { Params::cleanOn, 1.0f }, { Params::cleanContain, 0.0f }, { Params::cleanGate, 0.0f },
                               { Params::cleanTone, juce::String(tone) == "Voice" ? 1.0f : 3.0f } });
            }
        };
        for (const char* tone : { "Off", "Voice", "Radio", "Boom" })
        {
            double worst = 0.0;
            for (double hz : { 50.0, 100.0, 300.0, 1000.0, 3000.0, 6000.0, 12000.0 })
            {
                double level[2] {};
                for (int k = 0; k < 2; ++k)
                {
                    auto p = makeProcessor(sr, 512);
                    p->getChainForTests().setOutputTestOptions(true, false, false);
                    setAllModules(*p, false);
                    setParam(*p, Params::outPush, 0.0f);
                    setParam(*p, Params::inAuto, 0.0f);
                    configure(*p, tone);
                    setParam(*p, Params::mix, k == 0 ? 100.0f : 0.0f);
                    restart(*p, sr, 512);
                    const auto sine = makeSine(sr, 0.6, hz, 0.1);
                    const auto out = render(*p, dual(sine), 512);
                    level[k] = toneAmplitude(out.l, sr, hz, static_cast<int>(0.3 * sr), out.size());
                }
                // The mix at 50 % of wet and dry: |0.5 W + 0.5 D| vs (|W| + |D|) / 2.
                auto p = makeProcessor(sr, 512);
                p->getChainForTests().setOutputTestOptions(true, false, false);
                setAllModules(*p, false);
                setParam(*p, Params::outPush, 0.0f);
                setParam(*p, Params::inAuto, 0.0f);
                configure(*p, tone);
                setParam(*p, Params::mix, 50.0f);
                restart(*p, sr, 512);
                const auto sine = makeSine(sr, 0.6, hz, 0.1);
                const auto out = render(*p, dual(sine), 512);
                const double half = toneAmplitude(out.l, sr, hz, static_cast<int>(0.3 * sr), out.size());
                const double ideal = 0.5 * (level[0] + level[1]);
                const double lossDb = toDb(half) - toDb(ideal);
                worst = std::min(worst, lossDb);
            }
            std::printf("  %-11s: worst 50 %% mix loss %.2f dB (phase mismatch)\n", (juce::String(juce::String(tone) == "Boom" ? "Boom Heavy" : "CLEAN ") + (juce::String(tone) == "Boom" ? "" : tone)).toRawUTF8(), worst);
            // Radio's band-pass EQ is far from flat, its phase is matched only at the corners.
            check(worst > (juce::String(tone) == "Radio" ? -3.0 : juce::String(tone) == "Boom" ? -0.65 : -0.5), juce::String("Mix combs with ") + tone);
        }
    }

    void testStereoSplit()
    {
        std::printf("[stereo] mono core up to the first stereo-creating module\n");
        const double sr = 48000.0;
        const auto in = dual(makeVoice(sr, 1.0));
        const auto side = [](const Stereo& s)
        {
            double sq = 0.0, mid = 0.0;
            for (int i = 0; i < s.size(); ++i)
            {
                const double a = s.l[static_cast<size_t>(i)], b = s.r[static_cast<size_t>(i)];
                sq += 0.25 * (a - b) * (a - b);
                mid += 0.25 * (a + b) * (a + b);
            }
            return toDb(std::sqrt(sq / std::max(mid, 1e-30)));
        };

        {
            auto p = makeProcessor(sr, 512);
            setParam(*p, Params::ensembleOn, 0.0f);
            restart(*p, sr, 512);
            const auto out = render(*p, in, 512);
            check(nullDb(out.l, out.r, 0, 0) < -200.0, "no stereo module: L != R");
            check(p->getChain().getTopology().split == Params::numModules, "split with no stereo module");
        }
        {
            auto p = makeProcessor(sr, 512);
            auto order = Params::resolveOrder(Params::defaultPositions());
            const int at = slotOf(order, Params::ModuleId::ensemble);
            std::rotate(order.begin(), order.begin() + at, order.begin() + at + 1);
            setOrder(*p, order);
            setParam(*p, Params::ensembleWidth, 100.0f);
            restart(*p, sr, 512);
            const auto out = render(*p, in, 512);
            check(p->getChain().getTopology().split == 0, "ENSEMBLE first: split at 0");
            const double s = side(out);
            std::printf("  ENSEMBLE first, width 100: side/mid %.1f dB\n", s);
            check(s > -30.0, "ENSEMBLE makes stereo");
        }
        {
            auto p = makeProcessor(sr, 512);
            setParam(*p, Params::ensembleOn, 0.0f);
            setParam(*p, Params::shimmerMode, 3.0f);
            restart(*p, sr, 512);
            const auto out = render(*p, in, 512);
            check(p->getChain().getTopology().split == slotOf(Params::referenceOrder(), Params::ModuleId::shimmer), "SHIMMER Stereo: split at its slot");
            std::printf("  SHIMMER Stereo: side/mid %.1f dB\n", side(out));
            check(side(out) > -40.0, "SHIMMER Stereo makes stereo");
        }
        {
            // Init: ENSEMBLE near mono (width 15 %).
            auto p = makeProcessor(sr, 512);
            const auto out = render(*p, in, 512);
            std::printf("  Init side/mid %.1f dB (target <= -30)\n", side(out));
        }
    }

    /** Every parameter's plain value (a snapshot of the whole state). */
    std::vector<float> snapshot(RoarschachAudioProcessor& p)
    {
        std::vector<float> v;
        for (const auto& spec : Params::all)
            v.push_back(getParam(p, spec.id));
        return v;
    }

    void applySnapshot(RoarschachAudioProcessor& p, const std::vector<float>& v)
    {
        for (size_t i = 0; i < Params::all.size(); ++i)
            setParam(p, Params::all[i].id, v[i]);
    }

    /** 5 ms RMS frames (hop 2.5 ms) of x over [a, b), dB (or frames of `seconds`). */
    std::vector<double> shortRmsDb(const Signal& x, int a, int b, double sr, double seconds = 0.005)
    {
        const int win = static_cast<int>(seconds * sr);
        std::vector<double> out;
        for (int s = std::max(0, a); s + win <= std::min(static_cast<int>(x.size()), b); s += win / 2)
        {
            double t = 0.0;
            for (int i = s; i < s + win; ++i)
                t += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
            out.push_back(10.0 * std::log10(t / win + 1e-30));
        }
        return out;
    }

    double maxSecondDifference(const Signal& x, int a, int b)
    {
        double m = 0.0;
        for (int i = std::max(2, a); i < std::min(static_cast<int>(x.size()), b); ++i)
            m = std::max(m, std::abs(static_cast<double>(x[static_cast<size_t>(i)]) - 2.0 * x[static_cast<size_t>(i - 1)] + x[static_cast<size_t>(i - 2)]));
        return m;
    }

    struct TransitionResult
    {
        double worstSecondDiffRatio = 0.0;   // in a transition: peak |2nd difference| / the larger of the two steady renders'
        double percentileRatio = 0.0;        // spec §11.4: peak in the windows / 99.9th percentile outside them
        double worstDipDb = 0.0;             // 5 ms RMS under the quieter of the two steady renders
        float peak = 0.0f;
        int transitions = 0;
    };

    /**
     * Renders `in` while `onBlock` changes parameters, and compares every transition window with
     * steady renders of the settings before and after it (each from a fresh processor), so a
     * transition is judged against the sounds it goes between, not against an unrelated level.
     * `windowFor` gives the output window [start, end) of a change made before block `start`.
     */
    TransitionResult transitionTest(RoarschachAudioProcessor& p, const Stereo& in, double sr, int block,
                                    const std::function<bool(int)>& change,
                                    const std::function<std::pair<int, int>(int)>& windowFor, bool linearOutput)
    {
        // linearOutput: OUTPUT without its slow auto gain and Ride, in the render and the
        // references alike, so a level change upstream is not followed by a 400 ms glide that
        // the settled references do not have.
        if (linearOutput)
            p.getChainForTests().setOutputTestOptions(true, false, false);
        struct Event { int start; std::vector<float> before, after; };
        std::vector<Event> events;
        const auto out = render(p, in, block, false, [&](int start)
        {
            const auto before = snapshot(p);
            if (change(start))
                events.push_back({ start, before, snapshot(p) });
        });

        std::map<std::vector<float>, Signal> steady;
        const auto steadyFor = [&](const std::vector<float>& state) -> const Signal&
        {
            auto it = steady.find(state);
            if (it == steady.end())
            {
                auto q = makeProcessor(sr, block);
                if (linearOutput)
                    q->getChainForTests().setOutputTestOptions(true, false, false);
                applySnapshot(*q, state);
                restart(*q, sr, block);
                it = steady.emplace(state, render(*q, in, block).l).first;
            }
            return it->second;
        };

        TransitionResult r;
        for (const auto& e : events)
        {
            const auto [a, b] = windowFor(e.start);
            const auto& before = steadyFor(e.before);
            const auto& after = steadyFor(e.after);
            const double refD2 = std::max(maxSecondDifference(before, a, b), maxSecondDifference(after, a, b));
            r.worstSecondDiffRatio = std::max(r.worstSecondDiffRatio, maxSecondDifference(out.l, a, b) / std::max(refD2, 1e-9));
            const auto t = shortRmsDb(out.l, a, b, sr), x = shortRmsDb(before, a, b, sr), y = shortRmsDb(after, a, b, sr);
            for (size_t i = 0; i < t.size() && i < x.size() && i < y.size(); ++i)
                if (std::max(x[i], y[i]) > -60.0)
                    r.worstDipDb = std::min(r.worstDipDb, t[i] - std::min(x[i], y[i]));
            ++r.transitions;
        }
        for (float v : out.l)
            r.peak = std::max(r.peak, std::abs(v));

        // The spec's form of the click metric, over the whole render (after the first 100 ms).
        std::vector<bool> inWindow(out.l.size(), false);
        for (const auto& e : events)
        {
            const auto [a, b] = windowFor(e.start);
            for (int i = std::max(0, a); i < std::min(out.size(), b); ++i)
                inWindow[static_cast<size_t>(i)] = true;
        }
        std::vector<double> outside;
        double inside = 0.0;
        for (int i = std::max(2, static_cast<int>(0.1 * sr)); i < out.size(); ++i)
        {
            const double d2 = std::abs(static_cast<double>(out.l[static_cast<size_t>(i)]) - 2.0 * out.l[static_cast<size_t>(i - 1)] + out.l[static_cast<size_t>(i - 2)]);
            if (inWindow[static_cast<size_t>(i)])
                inside = std::max(inside, d2);
            else
                outside.push_back(d2);
        }
        if (!outside.empty())
        {
            std::sort(outside.begin(), outside.end());
            r.percentileRatio = inside / std::max(outside[static_cast<size_t>(0.999 * static_cast<double>(outside.size() - 1))], 1e-12);
        }
        return r;
    }

    void testReorderClicks()
    {
        std::printf("[reorder] twin-chain crossfade: no clicks, no dips, queued orders all applied\n");
        const double sr = 48000.0;
        const int block = 480;   // 10 ms
        const double seconds = g_quick ? 2.5 : 4.0;

        // The click detector is a 220 Hz sine. On a chain of time-invariant modules (no level
        // modulators, no PITCH grains, no SUB divider, no ENSEMBLE delays, no OVERTONE) the two
        // renders a reorder crossfades between are the same sound in the same phase, so any step
        // or dip is the mechanism's own. With those modules a moved module legitimately changes the
        // phase of a pure tone, and the 5 ms frames of these runs compare against fresh renders
        // that are not phase-locked to the render: those runs are reported, not gated. The fade's
        // level on them is gated below, against the exact continuation of the previous order.
        const Values steady { { Params::growlOn, 0.0f }, { Params::shimmerOn, 0.0f }, { Params::swirlOn, 0.0f },
                              { Params::pitchOn, 0.0f }, { Params::ensembleOn, 0.0f }, { Params::subOn, 0.0f },
                              { Params::overtoneOn, 0.0f } };

        struct Run { const char* name; bool sine; bool gated; Values values; };
        const std::vector<Run> runs {
            { "sine, time-invariant chain", true, true, steady },
            { "sine, Init", true, false, {} },
            { "voice, Init", false, false, {} },
        };

        for (size_t run = 0; run < runs.size(); ++run)
        {
            const auto& cfg = runs[run];
            const auto in = dual(cfg.sine ? makeSine(sr, seconds, 220.0, 0.25) : makeVoice(sr, seconds));

            // Reorders every 200 ms, plus a burst of 5 within 50 ms at 1.5 s.
            std::mt19937 rng(42 + static_cast<unsigned>(run));
            std::vector<std::pair<int, Params::Order>> changes;
            for (int t = 1; t * 0.2 < seconds - 0.4; ++t)
                if (std::abs(t * 0.2 - 1.5) > 0.05)
                    changes.push_back({ static_cast<int>(t * 0.2 * sr) / block * block, randomOrder(rng) });
            for (int k = 0; k < 5; ++k)
                changes.push_back({ (static_cast<int>(1.5 * sr) / block + k) * block, randomOrder(rng) });
            std::sort(changes.begin(), changes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

            auto p = makeProcessor(sr, block);
            setValues(*p, cfg.values);
            if (cfg.gated)
                p->getChainForTests().setOutputTestOptions(true, false, false);
            restart(*p, sr, block);
            const int outLatency = p->getChain().getOutput().getLatencySamples();
            const int fade = static_cast<int>(std::lround(0.030 * sr));
            // The crossfade waits (at most) until the input blend has left the incoming chain.
            const int wait = p->getChain().getModuleLatencySamples() + static_cast<int>(std::lround(0.010 * sr));
            Params::Order lastRequested = Params::resolveOrder(Params::defaultPositions());
            size_t next = 0;
            const auto result = transitionTest(*p, in, sr, block,
                [&](int start)
                {
                    bool changed = false;
                    while (next < changes.size() && changes[next].first <= start)
                    {
                        setOrder(*p, changes[next].second);
                        lastRequested = changes[next].second;
                        ++next;
                        changed = true;
                    }
                    return changed;
                },
                [&](int start)
                {
                    // The fade starts at this chunk (or, queued, when the running one ends) and
                    // reaches the output OUTPUT's latency later.
                    return std::pair<int, int> { start + outLatency, start + outLatency + wait + fade + 2 * block };
                }, cfg.gated);
            render(*p, dual(Signal(static_cast<size_t>(0.2 * sr), 0.0f)), block);   // let the last fade finish
            check(p->getChain().getTopology().order == lastRequested, juce::String(cfg.name) + ": the last requested order is running ("
                                                                          + orderCodes(p->getChain().getTopology().order) + ")");
            const int requested = static_cast<int>(changes.size());
            const int fades = p->getChain().getReorderCount();
            std::printf("  %-27s %d requests -> %d fades; 2nd difference %.2fx the steady renders' (%.2fx the 99.9th pct outside); worst 5 ms dip %.2f dB; peak %.2f dBFS\n",
                        cfg.name, requested, fades, result.worstSecondDiffRatio, result.percentileRatio, result.worstDipDb, toDb(result.peak));
            check(fades <= requested && fades >= requested - 4, "reorder count");
            if (cfg.gated)
            {
                check(result.worstSecondDiffRatio <= 2.0, "reorder click (2nd difference " + juce::String(result.worstSecondDiffRatio, 2) + "x the steady renders')");
                check(result.worstDipDb >= -3.0, "reorder level dip " + juce::String(result.worstDipDb, 2) + " dB");
            }
            check(result.peak <= ceilingGain(-1.0f), "over the ceiling during reorders");
        }

        // Single reorders against the exact continuation of the previous order (the same processor's
        // history, never reordered) and a fresh render of the new order: the level during the fade
        // must not drop under the quieter of the two. On Init, PITCH's grains, SUB's divider and
        // ENSEMBLE's delays leave the two renders out of phase, so this is where a plain linear
        // crossfade would cancel.
        {
            struct Case { const char* name; bool sine; double limitDb; };
            for (const Case c : { Case { "voice, Init", false, -3.0 }, Case { "sine, Init", true, -3.0 } })
            {
                const auto in = dual(c.sine ? makeSine(sr, 1.4, 220.0, 0.25) : makeVoice(sr, 1.4));
                std::mt19937 rng(99);
                std::vector<double> dips;
                const int trials = g_quick ? 6 : 12;
                for (int trial = 0; trial < trials; ++trial)
                {
                    const auto order = randomOrder(rng);
                    const int at = static_cast<int>(1.0 * sr) / block * block;
                    const auto make = [&](bool reordered)
                    {
                        auto q = makeProcessor(sr, block);
                        if (reordered)
                            setOrder(*q, order);
                        q->getChainForTests().setOutputTestOptions(true, false, false);
                        restart(*q, sr, block);
                        return q;
                    };
                    auto p = make(false), a = make(false), b = make(true);
                    const auto moved = render(*p, in, block, false, [&](int start) { if (start == at) setOrder(*p, order); }).l;
                    const auto before = render(*a, in, block).l;
                    const auto after = render(*b, in, block).l;
                    const int from = at, to = at + static_cast<int>(0.15 * sr);
                    // 20 ms frames: the 5 ms frames above swing by several dB with the phase of a
                    // 100 Hz voice and its 50 Hz octave, and the references here are not in phase.
                    const double frame = 0.020;
                    const auto t = shortRmsDb(moved, from, to, sr, frame), x = shortRmsDb(before, from, to, sr, frame), y = shortRmsDb(after, from, to, sr, frame);
                    double worst = 0.0;
                    for (size_t i = 0; i < t.size() && i < x.size() && i < y.size(); ++i)
                        if (std::max(x[i], y[i]) > -60.0)
                            worst = std::min(worst, t[i] - std::min(x[i], y[i]));
                    dips.push_back(worst);
                }
                std::sort(dips.begin(), dips.end());
                std::printf("  %-27s %d single reorders against the previous order's continuation: worst 20 ms dip %.2f dB, median %.2f dB\n",
                            c.name, trials, dips.front(), dips[dips.size() / 2]);
                check(dips.front() >= c.limitDb, juce::String(c.name) + ": reorder fade dip " + juce::String(dips.front(), 2) + " dB");
            }
        }

        // Module On/Off: each module fades itself (20 ms), the same metrics on the sine, one module
        // at a time from the time-invariant chain (DETAIL off too: its 500 ms stage would still be
        // settling on a level change, which the settled references do not show).
        {
            const auto in = dual(makeSine(sr, 1.2, 220.0, 0.25));
            double worstRatio = 0.0, worstDip = 0.0;
            juce::String worstName;
            for (const auto& spec : Params::modules)
            {
                const std::string_view code(spec.code);
                if (code == "GR" || code == "SH" || code == "SW")
                    continue;   // the level modulators themselves
                for (int dir = 0; dir < 2; ++dir)
                {
                    auto p = makeProcessor(sr, block);
                    setValues(*p, steady);
                    setParam(*p, Params::detailOn, 0.0f);
                    const bool initiallyOn = getParam(*p, spec.onId) > 0.5f;
                    if (dir == 1)
                        setParam(*p, spec.onId, initiallyOn ? 0.0f : 1.0f);
                    restart(*p, sr, block);
                    const int latency = p->getLatencySamples();
                    const auto r = transitionTest(*p, in, sr, block,
                        [&](int start)
                        {
                            if (start != static_cast<int>(0.6 * sr) / block * block)
                                return false;
                            setParam(*p, spec.onId, getParam(*p, spec.onId) > 0.5f ? 0.0f : 1.0f);
                            return true;
                        },
                        [&](int start) { return std::pair<int, int> { start, start + latency + static_cast<int>(0.04 * sr) }; }, true);
                    // A switch that brings in or removes other tones (pitch shift, chorus, ring, vocoder,
                    // the octave, the overtones) beats inside 5 ms frames; those are reported, the rest gated.
                    const bool changesTone = code == "PI" || code == "EN" || code == "RM" || code == "VO" || code == "SU" || code == "OV";
                    const juce::String what = juce::String(spec.uiName) + (getParam(*p, spec.onId) > 0.5f ? " on" : " off");
                    if (!changesTone)
                    {
                        check(r.worstSecondDiffRatio <= 2.0, what + " click (2nd difference " + juce::String(r.worstSecondDiffRatio, 2) + "x)");
                        check(r.worstDipDb >= -3.0, what + " level dip " + juce::String(r.worstDipDb, 2) + " dB");
                        if (r.worstSecondDiffRatio > worstRatio)
                        {
                            worstRatio = r.worstSecondDiffRatio;
                            worstName = what;
                        }
                        worstDip = std::min(worstDip, r.worstDipDb);
                    }
                    else
                    {
                        std::printf("    %-13s (tone change, not gated): 2nd difference %.2fx, dip %.2f dB\n", what.toRawUTF8(), r.worstSecondDiffRatio, r.worstDipDb);
                    }
                    check(r.peak <= ceilingGain(-1.0f), what + ": over the ceiling");
                }
            }
            std::printf("  On/Off (gated modules): worst 2nd difference %.2fx (%s), worst 5 ms dip %.2f dB\n",
                        worstRatio, worstName.toRawUTF8(), worstDip);
        }
    }

    struct NoOverResult
    {
        double worstSampleDb = -200.0, worstTruePeakDb = -200.0, worstStrictDb = -200.0;
        std::uint64_t clamps = 0;
        int overs = 0;
        int renders = 0;
        juce::String worstTruePeakCase;
        int truePeakOvers = 0;
    };

    void noOverRender(RoarschachAudioProcessor& p, const Stereo& in, float ceilingDb, NoOverResult& r, bool measureTruePeak,
                      const juce::String& label)
    {
        const auto out = render(p, in, 512);
        const float limit = ceilingGain(ceilingDb);
        const auto st = stats(out);
        ++r.renders;
        if (!st.finite || st.peak > limit)
            ++r.overs;
        r.worstSampleDb = std::max(r.worstSampleDb, toDb(st.peak) - ceilingDb);
        if (measureTruePeak)
        {
            const double tp = toDb(std::max(truePeak(out.l), truePeak(out.r))) - ceilingDb;
            if (tp > r.worstTruePeakDb)
            {
                r.worstTruePeakDb = tp;
                r.worstTruePeakCase = label;
            }
            if (tp > 0.2)
                ++r.truePeakOvers;
            r.worstStrictDb = std::max(r.worstStrictDb, toDb(std::max(truePeak(out.l, true), truePeak(out.r, true))) - ceilingDb);
        }
        r.clamps += p.getChain().getClampCount();
    }

    void testMaximizer()
    {
        std::printf("[maximizer] no overs: presets x Push x Ceiling x signals, and the clip/crunch grid\n");
        const double sr = 48000.0;
        const double len = g_quick ? 0.3 : 0.5;
        // Signals: voice, white noise, pink noise, 3 kHz square, sweep, clicks (all hot).
        std::vector<std::pair<const char*, Stereo>> signals {
            { "voice", dual(makeVoice(sr, len)) },
            { "white", dual(makeWhite(sr, len, 0.9f)) },
            { "pink", dual(makePink(sr, len, 0.9f)) },
            { "square", dual(makeSquare(sr, len, 3000.0, 0.7f)) },
            { "sweep", dual(makeSweep(sr, len, 0.8f)) },
            { "clicks", dual(makeClicks(sr, len, 0.99f)) },
        };
        for (auto& [name, s] : signals)
            for (auto* ch : { &s.l, &s.r })
                for (auto& x : *ch)
                    x = std::clamp(x, -1.0f, 1.0f);

        // Push 24 is reported apart (the most demanding case for the true-peak detectors).
        NoOverResult result, extreme;
        const auto pick = [&](float push) -> NoOverResult& { return push > 12.0f ? extreme : result; };
        const auto& presets = PresetManager::getFactoryPresets();
        const std::vector<float> pushes = g_quick ? std::vector<float> { 0.0f, 24.0f } : std::vector<float> { 0.0f, 6.0f, 12.0f, 24.0f };
        for (size_t pi = 0; pi < presets.size(); ++pi)
        {
            auto p = makeProcessor(sr, 512);
            p->setCurrentProgram(static_cast<int>(pi));
            for (float push : pushes)
                for (float ceiling : { -1.0f, -6.0f })
                {
                    setParam(*p, Params::outPush, push);
                    setParam(*p, Params::outCeiling, ceiling);
                    for (const auto& [name, s] : signals)
                    {
                        restart(*p, sr, 512);
                        noOverRender(*p, s, ceiling, pick(push), push >= 12.0f,
                                     juce::String(presets[pi].name) + ", " + name + ", Push " + juce::String(push) + ", Ceiling " + juce::String(ceiling));
                    }
                }
        }

        // SUB 100 % before ROAR 100 % (the worst order-specific case).
        {
            auto p = makeProcessor(sr, 512);
            setValues(*p, { { Params::sub, 100.0f }, { Params::subDrive, 100.0f }, { Params::roar, 100.0f } });
            setOrder(*p, orderFromCodes("PI CL DS DT VO RM SH GR SW SU RO EN TA OV"));
            for (float push : { 0.0f, 12.0f, 24.0f })
                for (float ceiling : { -1.0f, -6.0f })
                {
                    setParam(*p, Params::outPush, push);
                    setParam(*p, Params::outCeiling, ceiling);
                    restart(*p, sr, 512);
                    noOverRender(*p, signals[0].second, ceiling, pick(push), true, "SUB 100 before ROAR 100, Push " + juce::String(push));
                }
        }

        // The clip / crunch grid on Init (output-clipper-crunch.md 6.7).
        for (float push : { 0.0f, 12.0f, 24.0f })
            for (float clip : { 0.0f, 6.0f, 18.0f })
                for (float shape : { 0.0f, 100.0f })
                    for (float attack : { 0.05f, 10.0f })
                        for (int release = 0; release < 2; ++release)
                        {
                            if (g_quick && (attack > 1.0f || shape > 0.0f))
                                continue;
                            auto p = makeProcessor(sr, 512);
                            setValues(*p, { { Params::outPush, push }, { Params::outClip, clip }, { Params::outClipShape, shape },
                                            { Params::outAttack, attack }, { Params::outRelease, 1.0f },
                                            { Params::outAuto, release == 0 ? 0.0f : 1.0f } });
                            restart(*p, sr, 512);
                            for (size_t s = 0; s < 3; ++s)
                                noOverRender(*p, signals[s].second, -1.0f, pick(push), true,
                                             juce::String(signals[s].first) + ", Push " + juce::String(push) + " Clip " + juce::String(clip)
                                                 + " Shape " + juce::String(shape) + " Attack " + juce::String(attack) + (release == 0 ? " Release 1 ms" : " Auto"));
                        }

        for (const auto* r : { &result, &extreme })
            std::printf("  %s: %d renders, worst sample peak %+.2f dB re ceiling, worst true peak (8x) %+.2f dB (%s), %d over +0.2 dB TP, "
                        "near-ideal meter %+.2f dB, %d sample overs, %llu clamps\n",
                        r == &result ? "Push <= 12" : "Push 24   ", r->renders, r->worstSampleDb, r->worstTruePeakDb,
                        r->worstTruePeakCase.toRawUTF8(), r->truePeakOvers, r->worstStrictDb, r->overs,
                        static_cast<unsigned long long>(r->clamps));
        check(result.overs == 0 && extreme.overs == 0, juce::String(result.overs + extreme.overs) + " renders went over the ceiling");
        check(std::max(result.worstTruePeakDb, extreme.worstTruePeakDb) <= 0.2,
              "true peak " + juce::String(std::max(result.worstTruePeakDb, extreme.worstTruePeakDb), 2) + " dB over the ceiling");
        check(result.clamps == 0 && extreme.clamps == 0, "safety clamp triggered");

        // Transparency: Push 0, Clip 0, quiet signal: the maximizer is a pure delay (linear OUTPUT).
        {
            auto p = makeProcessor(sr, 512);
            p->getChainForTests().setOutputTestOptions(false, false, false);
            setAllModules(*p, false);
            setValues(*p, { { Params::outPush, 0.0f }, { Params::inAuto, 0.0f } });
            restart(*p, sr, 512);
            auto quiet = makeVoice(sr, 1.0);
            normalisePeak(quiet, 0.7f);
            const auto out = render(*p, dual(quiet), 512);
            // TAME's split is the only thing left: compare against an all-Off render at a lower level.
            auto q = makeProcessor(sr, 512);
            q->getChainForTests().setOutputTestOptions(false, false, false);
            setAllModules(*q, false);
            setValues(*q, { { Params::outPush, 0.0f }, { Params::inAuto, 0.0f } });
            restart(*q, sr, 512);
            Signal scaled = quiet;
            for (auto& x : scaled)
                x *= 0.5f;
            const auto outScaled = render(*q, dual(scaled), 512);
            Signal doubled = outScaled.l;
            for (auto& x : doubled)
                x *= 2.0f;
            const double db = nullDb(out.l, doubled, 0, static_cast<int>(0.1 * sr));
            std::printf("  Push 0 transparency (-3 dBFS peaks vs -9 dBFS x2): %.1f dB\n", db);
            check(db < -120.0, "the maximizer is not transparent below the ceiling");
        }
    }

    void testState()
    {
        std::printf("[state] all 87 parameters round trip, duplicate positions, one fade per preset load\n");
        std::mt19937 rng(77);
        auto p = makeProcessor(48000.0, 512);
        std::vector<float> written;
        for (const auto& spec : Params::all)
        {
            auto* param = p->getValueTreeState().getParameter(spec.id);
            const float u = static_cast<float>(rng() % 1000) / 999.0f;
            param->setValueNotifyingHost(u);
            written.push_back(param->convertFrom0to1(param->getValue()));
        }
        juce::MemoryBlock block;
        p->getStateInformation(block);

        auto q = makeProcessor(48000.0, 512);
        q->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        int mismatches = 0;
        for (size_t i = 0; i < Params::all.size(); ++i)
        {
            const float v = getParam(*q, Params::all[i].id);
            if (std::abs(v - written[i]) > 1e-4f * std::max(1.0f, std::abs(written[i])))
            {
                ++mismatches;
                std::printf("    %s: %f != %f\n", Params::all[i].id, v, written[i]);
            }
        }
        check(mismatches == 0, juce::String(mismatches) + " parameters did not round trip");

        // A state that lacks some parameters (an older or partial state) sets those to their
        // defaults, whatever the instance held before.
        {
            auto src = makeProcessor(48000.0, 512);
            juce::MemoryBlock full;
            src->getStateInformation(full);
            auto xml = juce::AudioProcessor::getXmlFromBinary(full.getData(), static_cast<int>(full.getSize()));
            const std::vector<const char*> dropped { Params::ringOn, Params::ring, Params::overtoneFreq, Params::ringPos,
                                                     Params::pitch, Params::outClip };
            for (const char* id : dropped)
                for (auto* e = xml->getFirstChildElement(); e != nullptr; e = e->getNextElement())
                    if (e->getStringAttribute("id") == id)
                    {
                        xml->removeChildElement(e, true);
                        break;
                    }
            juce::MemoryBlock partial;
            juce::AudioProcessor::copyXmlToBinary(*xml, partial);

            auto dst = makeProcessor(48000.0, 512);
            for (const char* id : dropped)
            {
                auto* param = dst->getValueTreeState().getParameter(id);
                param->setValueNotifyingHost(param->getDefaultValue() < 0.5f ? 0.9f : 0.1f);
            }
            dst->setStateInformation(partial.getData(), static_cast<int>(partial.getSize()));
            int wrong = 0;
            for (const char* id : dropped)
            {
                const float v = getParam(*dst, id), d = Params::defaultOf(id);
                if (std::abs(v - d) > 1e-4f * std::max(1.0f, std::abs(d)))
                {
                    ++wrong;
                    std::printf("    %s: %f, default %f\n", id, v, d);
                }
            }
            check(wrong == 0, juce::String(wrong) + " parameters a state lacks did not take their defaults");
        }

        // Positions written one parameter at a time never fade into an order in between: a writer
        // holding PositionWrites, or (host automation) a write that is split over two blocks.
        {
            const double sr = 48000.0;
            const int block = 128;
            const auto in = dual(makeVoice(sr, 0.6));
            const auto renderFor = [&](RoarschachAudioProcessor& q, double seconds)
            {
                const int n = static_cast<int>(seconds * sr);
                Stereo part { Signal(in.l.begin(), in.l.begin() + n), Signal(in.r.begin(), in.r.begin() + n) };
                render(q, part, block);
            };
            auto target = Params::resolveOrder(Params::defaultPositions());
            std::reverse(target.begin(), target.end());
            const auto targetPositions = Params::positionsFor(target);
            const auto writeHalf = [&](RoarschachAudioProcessor& q, bool first)
            {
                for (int slot = 0; slot < Params::numModules; ++slot)
                    if ((slot < Params::numModules / 2) == first)
                    {
                        const int m = target[static_cast<size_t>(slot)];
                        setParam(q, Params::modules[static_cast<size_t>(m)].posId, static_cast<float>(targetPositions[static_cast<size_t>(m)]));
                    }
            };

            for (int scoped = 0; scoped < 2; ++scoped)
            {
                auto q = makeProcessor(sr, block);
                renderFor(*q, 0.1);
                const int before = q->getChain().getReorderCount();
                {
                    std::optional<roar::PositionWrites::Scope> scope;
                    if (scoped == 1)
                        scope.emplace(&q->getPositionWrites());
                    writeHalf(*q, true);
                    // Scoped: a slow writer (40 ms between the halves). Unscoped: one 2.7 ms block.
                    renderFor(*q, scoped == 1 ? 0.04 : static_cast<double>(block) / sr);
                    writeHalf(*q, false);
                }
                renderFor(*q, 0.2);
                const int fades = q->getChain().getReorderCount() - before;
                check(fades == 1, juce::String(scoped == 1 ? "a scoped" : "a split") + " position write caused " + juce::String(fades) + " reorder fades");
                check(q->getChain().getTopology().order == target, "the written order is running");
            }
        }

        // Duplicate positions resolve by the reference index, always the same way.
        {
            Params::Order positions {};
            positions.fill(3);
            const auto order = Params::resolveOrder(positions);
            // 1.0's 14 in their reference order, CHAMBER inserted at its slot (3).
            bool reference = order[3] == static_cast<int>(Params::ModuleId::chamber);
            for (int k = 0, m = 0; k < Params::numModules; ++k)
                if (k != 3)
                    reference = reference && order[static_cast<size_t>(k)] == m++;
            check(reference, "all-equal positions resolve to the reference order");
            positions = Params::defaultPositions();
            positions[static_cast<size_t>(Params::ModuleId::sub)] = 0;   // SUB and PITCH both ask for 0
            const auto o2 = Params::resolveOrder(positions);
            check(o2[0] == static_cast<int>(Params::ModuleId::pitch) && o2[1] == static_cast<int>(Params::ModuleId::chamber)
                      && o2[2] == static_cast<int>(Params::ModuleId::sub), "tie-break by the reference index");
        }

        // Loading a reordered preset during playback: exactly one reorder fade.
        {
            const double sr = 48000.0;
            auto r = makeProcessor(sr, 512);
            const auto in = dual(makeVoice(sr, 0.5));
            render(*r, in, 512);
            const int before = r->getChain().getReorderCount();
            r->setCurrentProgram(presetIndex("Scorched Swirl"));
            render(*r, in, 512);
            const int fades = r->getChain().getReorderCount() - before;
            check(fades == 1, "preset load caused " + juce::String(fades) + " reorder fades");
            check(r->getChain().getTopology().order == orderFromCodes("PI CL DS RO SH GR SW DT VO RM EN TA SU OV"), "preset order running");
        }
    }

    void testPresets()
    {
        std::printf("[presets] complete tables; every preset x 20 orders: finite, no denormals, under the ceiling\n");
        const auto& presets = PresetManager::getFactoryPresets();
        std::printf("  %d factory presets\n", static_cast<int>(presets.size()));
        juce::StringArray names;
        for (const auto& preset : presets)
        {
            check(!names.contains(preset.name), juce::String("duplicate preset ") + preset.name);
            names.add(preset.name);
            check(static_cast<int>(preset.values.size()) == Params::numParameters, juce::String(preset.name) + " spells out "
                                                                                   + juce::String(static_cast<int>(preset.values.size())) + " values");
            Params::Order positions {};
            for (int m = 0; m < Params::numModules; ++m)
            {
                const auto it = preset.values.find(Params::modules[static_cast<size_t>(m)].posId);
                positions[static_cast<size_t>(m)] = it != preset.values.end() ? juce::roundToInt(it->second) : -1;
            }
            check(positionsAreValid(positions), juce::String(preset.name) + " positions are a permutation");
            for (const auto& [id, value] : preset.values)
            {
                const int i = Params::indexOf(id.toStdString());
                check(i >= 0, juce::String(preset.name) + " has unknown parameter " + id);
                if (i >= 0)
                {
                    const auto& spec = Params::all[static_cast<size_t>(i)];
                    check(value >= spec.minValue && value <= spec.maxValue, juce::String(preset.name) + " " + id + " out of range");
                }
            }
        }
        check(presetIndex("Init") == 0, "Init is the first preset");
        for (const char* required : { "Phone Monster", "Throat Singer", "Ribcage", "Cyber Commander", "Respirator Rasp", "Rasp Machine",
                                      "Simple Size", "Iron Tyrant", "Tin Can Drone", "Upside Down", "Spore Throat", "Hoarse Hoarder",
                                      "Angry Pepper Pot", "Ball Bearing", "Use Your Words", "Thing Next Door", "Stairwell Stalker" })
            check(presetIndex(required) >= 0, juce::String("missing preset ") + required);
        // The presets that use CHAMBER, and its slot in each (an insertion slot among the 14).
        {
            int chamberPresets = 0;
            for (const auto& preset : presets)
                chamberPresets += preset.values.at(Params::chamberOn) > 0.5f ? 1 : 0;
            check(chamberPresets == 13, juce::String(chamberPresets) + " presets use CHAMBER (expected 13)");
            const auto slot = [&](const char* name)
            {
                const int i = presetIndex(name);
                return i < 0 ? -1 : juce::roundToInt(presets[static_cast<size_t>(i)].values.at(Params::chamberPos));
            };
            check(slot("Simple Size") == 1 && slot("Tin Can Drone") == 1, "Simple Size and Tin Can Drone: CHAMBER right after PITCH");
            check(slot("Iron Tyrant") == 5, "Iron Tyrant: CHAMBER right after ROAR");
            check(slot("Cave Troll") == 14 && slot("Gargoyle") == 14 && slot("Demon Whisper") == 14, "the creature in the room: CHAMBER last");
            check(slot("Upside Down") == 14 && slot("Hoarse Hoarder") == 14, "Upside Down and Hoarse Hoarder: the octave and rumble in the room, CHAMBER last");
            check(slot("Thing Next Door") == 14 && slot("Stairwell Stalker") == 14, "Thing Next Door and Stairwell Stalker: the whole creature in the room, CHAMBER last");
            // The two presets that show off the recorded rooms use them.
            const auto roomOf = [&](const char* name)
            {
                const int i = presetIndex(name);
                return i < 0 ? -1 : static_cast<int>(presets[static_cast<size_t>(i)].values.at(Params::chamberRoom));
            };
            check(roomOf("Thing Next Door") == static_cast<int>(Params::ChamberRoom::partyWall)
                      && roomOf("Stairwell Stalker") == static_cast<int>(Params::ChamberRoom::towerBlock),
                  "Thing Next Door plays in Party Wall, Stairwell Stalker in Tower Block");
            check(slot("Use Your Words") == 1, "Use Your Words: the pitched voice played into the room, CHAMBER right after PITCH");
            check(slot("Ball Bearing") == 8, "Ball Bearing: CHAMBER after TAME, the tiny speaker at the end of the sounding chain");
            // Spore Throat and Angry Pepper Pot are dry: CHAMBER off in its default slot.
            check(presetIndex("Spore Throat") >= 0 && presets[static_cast<size_t>(presetIndex("Spore Throat"))].values.at(Params::chamberOn) < 0.5f
                  && presetIndex("Angry Pepper Pot") >= 0 && presets[static_cast<size_t>(presetIndex("Angry Pepper Pot"))].values.at(Params::chamberOn) < 0.5f,
                  "Spore Throat and Angry Pepper Pot: no CHAMBER");
        }

        // Every preset loaded through the host program interface sets exactly its values.
        {
            auto p = makeProcessor(48000.0, 512);
            int wrong = 0;
            for (size_t i = 0; i < presets.size(); ++i)
            {
                p->setCurrentProgram(static_cast<int>(i));
                for (const auto& [id, value] : presets[i].values)
                    if (std::abs(getParam(*p, id.toRawUTF8()) - value) > 1e-3f * std::max(1.0f, std::abs(value)))
                        ++wrong;
            }
            check(wrong == 0, juce::String(wrong) + " preset values not applied");
        }

        // Every preset in 20 random orders (one long render per preset, the order changing every
        // segment, so the reorder fades are exercised too).
        const double sr = 48000.0;
        const int perms = g_quick ? 4 : 20;
        const double segment = 0.4;
        auto voice = makeVoice(sr, segment * perms + 0.3);
        auto hot = voice;
        normalisePeak(hot, 0.98f);   // a hot take too: the ceiling must still hold
        int bad = 0;
        for (size_t i = 0; i < presets.size(); ++i)
        {
            for (int level = 0; level < 2; ++level)
            {
                auto p = makeProcessor(sr, 512);
                p->setCurrentProgram(static_cast<int>(i));
                restart(*p, sr, 512);
                std::mt19937 rng(static_cast<unsigned>(i * 31 + static_cast<size_t>(level)));
                const int segmentSamples = static_cast<int>(segment * sr) / 512 * 512;
                const float ceiling = getParam(*p, Params::outCeiling);
                const auto out = render(*p, dual(level == 0 ? voice : hot), 512, false, [&](int start)
                {
                    if (start > 0 && start % segmentSamples == 0)
                        setOrder(*p, randomOrder(rng));
                });
                const auto st = stats(out);
                const bool ok = st.finite && !st.subnormal && st.peak <= ceilingGain(ceiling) && st.rms > 1e-4
                                && p->getChain().getClampCount() == 0;
                if (!ok)
                {
                    ++bad;
                    std::printf("    %s (%s): finite %d subnormal %d peak %.2f dBFS rms %.1f dBFS clamps %llu\n",
                                presets[i].name, level == 0 ? "voice" : "hot", st.finite, st.subnormal, toDb(st.peak),
                                toDb(st.rms), static_cast<unsigned long long>(p->getChain().getClampCount()));
                }
                if (level == 0)
                    std::printf("  %-18s rms %6.1f dBFS  peak %6.2f dBFS\n", presets[i].name, toDb(st.rms), toDb(st.peak));
            }
        }
        check(bad == 0, juce::String(bad) + " preset renders broken");
    }

    void testNonFiniteInput()
    {
        std::printf("[robust] NaN / inf / huge bursts: finite output, recovers\n");
        const double sr = 48000.0;
        auto in = makeVoice(sr, 2.0);
        for (int i = static_cast<int>(0.5 * sr); i < static_cast<int>(0.5 * sr) + 64; ++i)
            in[static_cast<size_t>(i)] = std::numeric_limits<float>::quiet_NaN();
        for (int i = static_cast<int>(0.7 * sr); i < static_cast<int>(0.7 * sr) + 64; ++i)
            in[static_cast<size_t>(i)] = (i & 1) ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
        for (int i = static_cast<int>(0.9 * sr); i < static_cast<int>(0.9 * sr) + 64; ++i)
            in[static_cast<size_t>(i)] = (i & 1) ? 3.0e38f : -3.0e38f;
        for (const char* name : { "Init", "Kaiju", "Rasp Machine", "Phone Monster" })
        {
            auto p = makeProcessor(sr, 512);
            p->setCurrentProgram(presetIndex(name));
            restart(*p, sr, 512);
            const auto out = render(*p, dual(in), 512);
            const auto st = stats(out);
            Stereo tail { Signal(out.l.begin() + static_cast<long>(1.6 * sr), out.l.end()), Signal(out.r.begin() + static_cast<long>(1.6 * sr), out.r.end()) };
            const auto ts = stats(tail);
            check(st.finite && st.peak <= ceilingGain(-1.0f), juce::String(name) + ": non-finite or over after bad input");
            check(ts.rms > 1e-4, juce::String(name) + ": did not recover (tail rms " + juce::String(toDb(ts.rms), 1) + " dBFS)");
        }
    }

    void testBlockSizes()
    {
        std::printf("[blocks] Init renders the same at any host block size\n");
        const double sr = 48000.0;
        const auto in = dual(makeVoice(sr, 1.0));
        auto ref = makeProcessor(sr, 512);
        const auto a = render(*ref, in, 512);
        for (int block : { 1, 64, 480, 2049 })
        {
            auto p = makeProcessor(sr, block);
            const auto b = render(*p, in, block);
            double err = 0.0;
            for (int i = 0; i < a.size(); ++i)
                err = std::max({ err, std::abs(static_cast<double>(a.l[static_cast<size_t>(i)]) - b.l[static_cast<size_t>(i)]),
                                 std::abs(static_cast<double>(a.r[static_cast<size_t>(i)]) - b.r[static_cast<size_t>(i)]) });
            std::printf("  block %4d: max difference %.3g (%.1f dBFS)\n", block, err, toDb(err));
            check(err < 1.0e-5, "block size " + juce::String(block) + " changes the output by " + juce::String(err));
        }
    }

    void testNativeRatePitch()
    {
        std::printf("[pitch] native rate: 40 kHz at 192 kHz, -24 st Follow, through the whole plug-in\n");
        const double sr = 192000.0;
        auto p = makeProcessor(sr, 512);
        p->getChainForTests().setOutputTestOptions(true, false, false);   // no level matching: the level is checked too
        setAllModules(*p, false);
        setValues(*p, { { Params::inAuto, 0.0f }, { Params::pitchOn, 1.0f }, { Params::pitch, -24.0f }, { Params::pitchDrive, 0.0f }, { Params::pitchSize, 0.0f },
                        { Params::pitchFormant, 0.0f }, { Params::pitchMode, 0.0f }, { Params::outPush, 0.0f } });
        restart(*p, sr, 512);
        const auto out = render(*p, dual(makeSine(sr, 1.2, 40000.0, 0.25)), 512);
        constexpr int order = 16;
        const auto power = spectrum(out.l, static_cast<int>(0.6 * sr), order);
        const double binHz = sr / (1 << order);
        const int target = static_cast<int>(std::lround(10000.0 / binHz));
        double tone = 0.0, spur = 0.0;
        double spurHz = 0.0;
        for (int k = 1; k < static_cast<int>(power.size()); ++k)
        {
            const double v = power[static_cast<size_t>(k)];
            if (std::abs(k - target) <= 8)
                tone += v;
            else if (v > spur)
            {
                spur = v;
                spurHz = k * binHz;
            }
        }
        const double spurDb = 10.0 * std::log10(spur / std::max(tone, 1e-30));
        const double level = toneAmplitude(out.l, sr, 10000.0, static_cast<int>(0.6 * sr), out.size());
        std::printf("  output: 10 kHz at %.1f dB re the 40 kHz input; largest other component %.1f dB re the tone (at %.0f Hz)\n",
                    toDb(level / 0.25), spurDb, spurHz);
        check(std::abs(toDb(level / 0.25)) < 1.5, "the 40 kHz tone does not land at 10 kHz at its level");
        check(spurDb <= -60.0, "spurious components " + juce::String(spurDb, 1) + " dB (<= -60)");

        // Nothing ahead of PITCH in the default order cuts the ultrasonic band: IN and Init's CLEAN at 192 kHz.
        {
            roar::InModule inModule;
            roar::CleanModule clean;
            inModule.prepare(sr, 512);
            clean.prepare(sr, 512);
            roar::InSettings is;
            is.autoLevel = false;
            inModule.setParams(is);
            clean.setParams(roar::CleanSettings {});
            inModule.reset();
            clean.reset();
            auto noise = makeWhite(sr, 1.0, 0.1f, 9);
            auto x = noise;
            Signal r = x;
            for (int start = 0; start < static_cast<int>(x.size()); start += 512)
            {
                const int n = std::min(512, static_cast<int>(x.size()) - start);
                inModule.process(x.data() + start, r.data() + start, n, false, roar::VoiceFrameView {});
                clean.process(x.data() + start, r.data() + start, n, false, inModule.getVoiceView());
            }
            const auto pin = spectrum(noise, static_cast<int>(0.5 * sr), 14);
            const auto pout = spectrum(x, static_cast<int>(0.5 * sr), 14);
            double worst = 1.0e9;
            for (double lo : { 20000.0, 40000.0, 60000.0, 80000.0 })
            {
                const double db = 10.0 * std::log10(bandPower(pout, sr, 14, lo, lo + 10000.0) / bandPower(pin, sr, 14, lo, lo + 10000.0));
                worst = std::min(worst, db);
            }
            const double mid = 10.0 * std::log10(bandPower(pout, sr, 14, 1000.0, 5000.0) / bandPower(pin, sr, 14, 1000.0, 5000.0));
            std::printf("  IN + CLEAN (Init) at 192 kHz: 1-5 kHz %+.2f dB, worst 20-90 kHz band %+.2f dB re input\n", mid, worst);
            check(worst > -1.0, "ultrasonic content is cut before PITCH");
        }
    }

    void testRingAndOvertone()
    {
        std::printf("[ring / overtone] basic behaviour through the plug-in\n");
        const double sr = 48000.0;
        {
            auto p = makeProcessor(sr, 512);
            p->getChainForTests().setOutputTestOptions(false, false, false);
            setAllModules(*p, false);
            setValues(*p, { { Params::ringOn, 1.0f }, { Params::ring, 100.0f }, { Params::ringFreq, 100.0f }, { Params::ringCarrier, 0.0f },
                            { Params::ringTrack, 0.0f }, { Params::ringMode, 0.0f }, { Params::ringBody, 0.0f },
                            { Params::outPush, 0.0f }, { Params::inAuto, 0.0f } });
            restart(*p, sr, 512);
            const auto out = render(*p, dual(makeSine(sr, 1.0, 1000.0, 0.1)), 512);
            const int from = static_cast<int>(0.4 * sr), to = out.size();
            const double lo = toneAmplitude(out.l, sr, 900.0, from, to), hi = toneAmplitude(out.l, sr, 1100.0, from, to);
            const double carrierIn = toneAmplitude(out.l, sr, 1000.0, from, to);
            std::printf("  RING 100 Hz sine on 1 kHz: sidebands %.2f / %.2f dB re input, input line %.1f dB re sideband\n",
                        toDb(lo / 0.1), toDb(hi / 0.1), toDb(carrierIn) - toDb(lo));
            check(std::abs(toDb(lo / 0.1) + 3.01) < 0.3 && std::abs(toDb(hi / 0.1) + 3.01) < 0.3, "RING sideband level (-3.01 dB)");
            check(toDb(carrierIn) - toDb(lo) < -40.0, "RING suppresses the input");
        }
        {
            // OVERTONE on a 100 Hz low voice-like tone: H2..H4 rise against Off; Replace removes the fundamental.
            double harm[2] {};
            double fund[2] {};
            for (int k = 0; k < 2; ++k)
            {
                auto p = makeProcessor(sr, 512);
                p->getChainForTests().setOutputTestOptions(false, false, false);
                setAllModules(*p, false);
                setValues(*p, { { Params::overtoneOn, k == 0 ? 0.0f : 1.0f }, { Params::overtone, 100.0f }, { Params::overtonePattern, 4.0f },
                                { Params::overtoneMode, 0.0f }, { Params::outPush, 0.0f }, { Params::inAuto, 0.0f } });
                restart(*p, sr, 512);
                const auto out = render(*p, dual(makeSine(sr, 1.0, 100.0, 0.1)), 512);
                const int from = static_cast<int>(0.4 * sr);
                harm[k] = toneAmplitude(out.l, sr, 200.0, from, out.size()) + toneAmplitude(out.l, sr, 300.0, from, out.size());
                fund[k] = toneAmplitude(out.l, sr, 100.0, from, out.size());
            }
            std::printf("  OVERTONE Broad 100 %%: H2+H3 %.1f dB re fundamental (Off: %.1f dB)\n", toDb(harm[1] / fund[1]), toDb(harm[0] / fund[0]));
            check(toDb(harm[1] / fund[1]) > -30.0 && toDb(harm[0] / fund[0]) < -80.0, "OVERTONE generates harmonics");
        }
    }


    /** Records host notifications in order: 'b' gesture begin, 's' value, 'e' gesture end. */
    struct GestureLog final : public juce::AudioProcessorListener
    {
        std::vector<std::pair<char, int>> events;
        void audioProcessorParameterChanged(juce::AudioProcessor*, int index, float) override { events.emplace_back('s', index); }
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
        void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int index) override { events.emplace_back('b', index); }
        void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int index) override { events.emplace_back('e', index); }
    };

    /** True when the log is one gesture over position parameters only: every parameter that is set
        is begun before any value is set and ended after all of them, exactly once each. */
    bool isOneGesture(const GestureLog& log, juce::String& why)
    {
        std::map<int, std::array<int, 3>> counts;
        int phase = 0;   // 0 begins, 1 sets, 2 ends
        for (const auto& [kind, index] : log.events)
        {
            const int k = kind == 'b' ? 0 : kind == 's' ? 1 : 2;
            if (k < phase)
            {
                why = "events out of order";
                return false;
            }
            phase = k;
            ++counts[index][static_cast<size_t>(k)];
            if (Params::all[static_cast<size_t>(index)].kind != Params::Kind::position)
            {
                why = juce::String("touched ") + Params::all[static_cast<size_t>(index)].id;
                return false;
            }
        }
        for (const auto& [index, c] : counts)
            if (c[0] != 1 || c[1] != 1 || c[2] != 1)
            {
                why = juce::String(Params::all[static_cast<size_t>(index)].id) + " not begun, set and ended once";
                return false;
            }
        return true;
    }

    bool isPermutationOfPositions(RoarschachAudioProcessor& p)
    {
        Params::Order positions {};
        for (int m = 0; m < Params::numModules; ++m)
            positions[static_cast<size_t>(m)] = juce::roundToInt(getParam(p, Params::modules[static_cast<size_t>(m)].posId));
        return positionsAreValid(positions);
    }

    juce::MouseEvent mouseEventFor(juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, bool dragged)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos,
                                juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                &c, &c, now, downPos, now, 1, dragged);
    }

    /** Drags `tile` by its header through `path` (strip coordinates of the grab point), releasing at the
        end when `release` is set. Positions are converted to the tile as it is at each event (it moves). */
    void dragTile(SignalFlowStrip& strip, juce::Component& tile, const std::vector<juce::Point<float>>& path, bool release)
    {
        const juce::Point<float> grab { 50.0f, 12.0f };
        const auto downInStrip = tile.getPosition().toFloat() + grab;
        tile.mouseDown(mouseEventFor(tile, grab, grab, false));
        for (const auto& p : path)
        {
            const auto local = p - tile.getPosition().toFloat();
            const auto down = downInStrip - tile.getPosition().toFloat();
            tile.mouseDrag(mouseEventFor(tile, local, down, true));
        }
        if (release)
        {
            const auto local = path.back() - tile.getPosition().toFloat();
            tile.mouseUp(mouseEventFor(tile, local, downInStrip - tile.getPosition().toFloat(), true));
        }
        juce::ignoreUnused(strip);
    }

    juce::StringArray forbiddenWords()
    {
        // The names UI text must never contain live in a gitignored research file, so they are not in
        // the repository either. Without it, only the code-identifier checks run.
        juce::StringArray words;
        const auto file = juce::File(__FILE__).getParentDirectory().getParentDirectory().getChildFile("docs/research/forbidden-words.txt");
        if (file.existsAsFile())
            for (const auto& line : juce::StringArray::fromLines(file.loadFileAsString()))
                if (line.trim().isNotEmpty() && !line.trim().startsWithChar('#'))
                    words.add(line.trim());
        return words;
    }

    void collectMenuTexts(const juce::PopupMenu& menu, juce::StringArray& out)
    {
        for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
        {
            const auto& item = it.getItem();
            out.add(item.text);
            if (item.subMenu != nullptr)
                collectMenuTexts(*item.subMenu, out);
        }
    }

    //==========================================================================
    // The dice: the preset die's bag, ROLL ALL's ranges and loudness, a module's die, undo, gestures,
    // and a roll mid-stream.

    struct GestureCounter : juce::AudioProcessorListener
    {
        std::map<int, int> begins, ends;
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
        void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int i) override { ++begins[i]; }
        void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int i) override { ++ends[i]; }
    };

    std::vector<float> normalisedState(RoarschachAudioProcessor& p)
    {
        std::vector<float> v;
        for (auto* param : p.getParameters())
            v.push_back(param->getValue());
        return v;
    }

    bool isModuleControl(int module, std::string_view id)
    {
        const auto& m = Params::modules[static_cast<size_t>(module)];
        for (const char* k : m.knobIds)
            if (k != nullptr && id == k)
                return true;
        for (const char* c : m.chipIds)
            if (c != nullptr && id == c)
                return true;
        return false;
    }

    double integratedLufs(const Stereo& s, double sr, int from)
    {
        const RenderMetrics::Sig l(s.l.begin() + from, s.l.end()), r(s.r.begin() + from, s.r.end());
        return RenderMetrics::loudness(l, r, sr).integrated;
    }

    void testDice()
    {
        std::printf("[dice] preset bag, ROLL ALL ranges and loudness, module dice, undo, gestures, a roll mid-stream\n");
        const int numFactory = PresetManager::getNumFactory();

        // 1. The preset die: never the preset showing, never the same twice in a row, every factory
        //    preset once per round.
        {
            auto p = makeProcessor(48000.0, 512);
            auto& dice = p->getDice();
            auto& pm = p->getPresetManager();
            dice.setSeed(11);
            std::vector<int> seen(static_cast<size_t>(numFactory), 0);
            int repeats = 0, wrong = 0, modified = 0, firstRoundMissing = 0;
            int previous = pm.getCurrentFactoryIndex();
            const int rounds = 3;
            for (int k = 0; k < rounds * numFactory; ++k)
            {
                const int pick = dice.rollPreset();
                repeats += pick == previous ? 1 : 0;
                wrong += pick < 0 || pm.getCurrentFactoryIndex() != pick || pm.getCurrentName() != PresetManager::getFactoryPresetName(pick) ? 1 : 0;
                modified += pm.isModified() ? 1 : 0;
                if (pick >= 0)
                    ++seen[static_cast<size_t>(pick)];
                previous = pick;
                if (k == numFactory - 1)
                    for (int c : seen)
                        firstRoundMissing += c == 0 ? 1 : 0;
            }
            const auto [lo, hi] = std::minmax_element(seen.begin(), seen.end());
            std::printf("  preset die: %d rolls over %d presets, each came up %d-%d times, %d back-to-back repeats\n",
                        rounds * numFactory, numFactory, *lo, *hi, repeats);
            check(repeats == 0, "the preset die repeated a preset back to back");
            check(wrong == 0, "the preset die loads the preset it picked");
            check(modified == 0, "a rolled preset shows as the preset, not edited");
            check(firstRoundMissing == 0, juce::String(firstRoundMissing) + " presets missing from the first round of the preset die");
            check(*lo == rounds && *hi == rounds, "every factory preset comes up once per round");

            // The preset showing is never picked, even when it was chosen by hand mid-round.
            int hits = 0;
            for (int k = 0; k < 4 * numFactory; ++k)
            {
                const int current = (k * 7) % numFactory;
                p->setCurrentProgram(current);
                hits += dice.rollPreset() == current ? 1 : 0;
            }
            check(hits == 0, "the preset die picked the preset showing");

            // One undo step puts the previous preset back, unedited.
            p->setCurrentProgram(presetIndex("Kaiju"));
            const auto before = normalisedState(*p);
            dice.rollPreset();
            check(dice.undo(), "a preset roll can be undone");
            check(normalisedState(*p) == before && pm.getCurrentName() == "Kaiju" && !pm.isModified(), "undoing a preset roll restores Kaiju exactly");
        }

        // 2. ROLL ALL: every control inside its range, 3-7 modules on, the order, IN and OUTPUT kept,
        //    the preset edited, one undo step restoring everything exactly, the same seed rolling the same.
        {
            auto p = makeProcessor(48000.0, 512);
            auto& dice = p->getDice();
            auto& pm = p->getPresetManager();
            const int seeds = g_quick ? 60 : 200;
            int outOfRange = 0, badCount = 0, fewCharacter = 0, orderMoved = 0, keptChanged = 0, notModified = 0, undoWrong = 0, redoWrong = 0, narrowed = 0;
            std::array<int, Params::numModules> onCount {};
            std::array<int, Params::Random::maxModulesOn + 1> countHist {};
            for (int seed = 1; seed <= seeds; ++seed)
            {
                p->setCurrentProgram(seed % numFactory);   // from many starting points
                const auto before = normalisedState(*p);
                const auto plainBefore = snapshot(*p);
                const auto orderBefore = Params::resolveOrder(p->readSettings().positions);
                const auto nameBefore = pm.getCurrentName();
                dice.setSeed(static_cast<std::uint32_t>(seed));
                dice.rollAll();
                const auto plain = snapshot(*p);

                int on = 0, character = 0;
                for (int m = 0; m < Params::numModules; ++m)
                {
                    const auto& mod = Params::modules[static_cast<size_t>(m)];
                    if (getParam(*p, mod.onId) > 0.5f)
                    {
                        ++on;
                        ++onCount[static_cast<size_t>(m)];
                        character += Params::Random::isUtility(m) ? 0 : 1;
                    }
                }
                badCount += on < Params::Random::minModulesOn || on > Params::Random::maxModulesOn ? 1 : 0;
                fewCharacter += character < Params::Random::minCharacterModules ? 1 : 0;
                ++countHist[static_cast<size_t>(std::clamp(on, 0, Params::Random::maxModulesOn))];

                for (size_t i = 0; i < Params::all.size(); ++i)
                {
                    const auto& s = Params::all[i];
                    if (const auto* r = Params::Random::find(s.id))
                    {
                        const float tol = std::max(s.step * 0.5f, 1.0e-4f * (s.maxValue - s.minValue));
                        if (!Params::Random::allows(s.id, plain[i], tol))
                        {
                            ++outOfRange;
                            if (outOfRange <= 5)
                                std::printf("    out of range: %s = %.4f (%.4f..%.4f)\n", s.id, plain[i], r->lo, r->hi);
                        }
                    }
                    else
                    {
                        // Not a module control: only the On switches may change.
                        bool isOn = false;
                        for (const auto& mod : Params::modules)
                            isOn = isOn || std::string_view(mod.onId) == s.id;
                        if (!isOn && !juce::exactlyEqual(plain[i], plainBefore[i]))
                        {
                            ++keptChanged;
                            std::printf("    changed: %s\n", s.id);
                        }
                    }
                }
                orderMoved += Params::resolveOrder(p->readSettings().positions) != orderBefore ? 1 : 0;
                notModified += pm.isModified() ? 0 : 1;

                // Where a mode changes what a knob does: SWIRL Deep 6 to 30 %, RING Follow from x1,
                // PITCH Mono on a whole note.
                const auto at = [&](const char* id) { return plain[static_cast<size_t>(Params::indexOf(id))]; };
                narrowed += static_cast<int>(at(Params::swirlCircuit) + 0.5f) == Params::Random::swirlDeep6 && at(Params::swirl) > Params::Random::swirlDeep6Max + 0.01f ? 1 : 0;
                narrowed += static_cast<int>(at(Params::ringTrack) + 0.5f) == Params::Random::ringFollow && at(Params::ringRatio) < Params::Random::ringFollowMinRatio - 0.01f ? 1 : 0;
                narrowed += static_cast<int>(at(Params::pitchMode) + 0.5f) == Params::Random::pitchMono && std::abs(at(Params::pitch) - std::round(at(Params::pitch))) > 0.006f ? 1 : 0;

                const auto after = normalisedState(*p);
                dice.undo();
                undoWrong += normalisedState(*p) != before || pm.getCurrentName() != nameBefore || pm.isModified() ? 1 : 0;
                dice.redo();
                redoWrong += normalisedState(*p) != after || !pm.isModified() ? 1 : 0;
                dice.undo();
            }
            juce::String hist, perModule;
            for (int c = Params::Random::minModulesOn; c <= Params::Random::maxModulesOn; ++c)
                hist << c << ":" << countHist[static_cast<size_t>(c)] << " ";
            for (int m = 0; m < Params::numModules; ++m)
                perModule << Params::modules[static_cast<size_t>(m)].code << " " << onCount[static_cast<size_t>(m)] << "  ";
            std::printf("  ROLL ALL x%d: modules on %s\n    on per module: %s\n", seeds, hist.toRawUTF8(), perModule.toRawUTF8());
            check(outOfRange == 0, juce::String(outOfRange) + " rolled controls outside their random range");
            check(badCount == 0, "ROLL ALL turns 3 to 7 modules on");
            check(fewCharacter == 0, "ROLL ALL turns on at least two modules that change the voice");
            check(keptChanged == 0, "ROLL ALL leaves IN, OUTPUT and the positions alone");
            check(orderMoved == 0, "ROLL ALL keeps the module order");
            check(narrowed == 0, juce::String(narrowed) + " rolls outside the narrowed ranges (SWIRL Deep 6, RING Follow, PITCH Mono)");
            check(notModified == 0, "a roll shows the preset as edited");
            check(undoWrong == 0, juce::String(undoWrong) + " ROLL ALL undos did not restore the exact state and preset");
            check(redoWrong == 0, juce::String(redoWrong) + " ROLL ALL redos did not restore the roll");

            // The same seed rolls the same values (the batch renders rely on it).
            p->setCurrentProgram(0);
            dice.setSeed(1234);
            dice.rollAll();
            const auto a = normalisedState(*p);
            p->setCurrentProgram(0);
            dice.setSeed(1234);
            dice.rollAll();
            check(normalisedState(*p) == a, "a seed rolls the same values");

            // One undo step per roll, however many rolls.
            p->setCurrentProgram(0);
            const auto init = normalisedState(*p);
            dice.rollAll();
            const auto first = normalisedState(*p);
            dice.rollModule(static_cast<int>(Params::ModuleId::growl));
            dice.rollAll();
            dice.undo();
            dice.undo();
            check(normalisedState(*p) == first, "two undos take back exactly two rolls");
            dice.undo();
            check(normalisedState(*p) == init && !pm.isModified() && pm.getCurrentName() == "Init", "three undos are back at Init");

            // An edit by hand after a roll stays through the roll's undo, and keeps the preset edited.
            dice.rollModule(static_cast<int>(Params::ModuleId::growl));
            setParam(*p, Params::outPush, 12.0f);
            dice.undo();
            check(std::abs(getParam(*p, Params::outPush) - 12.0f) < 0.05f && juce::exactlyEqual(getParam(*p, Params::growl), Params::defaultOf(Params::growl))
                      && pm.isModified(), "undo keeps a later edit by hand and the edited mark");

            // Loading another preset ends the history: undo must not mix the roll into it.
            p->setCurrentProgram(0);
            dice.rollAll();
            p->setCurrentProgram(presetIndex("Kaiju"));
            const auto kaiju = normalisedState(*p);
            check(!dice.canUndo() && !dice.undo() && normalisedState(*p) == kaiju, "a preset loaded after a roll is not undone into");

            // An edit by hand to a knob the roll changed also stays through undo and redo; the
            // roll's other knobs go back.
            {
                p->setCurrentProgram(0);
                dice.setSeed(5);
                dice.rollModule(static_cast<int>(Params::ModuleId::growl));
                const float rolledRate = getParam(*p, Params::growlRate);
                setParam(*p, Params::growl, 33.0f);
                dice.undo();
                const bool undoKept = std::abs(getParam(*p, Params::growl) - 33.0f) < 0.05f
                                   && juce::exactlyEqual(getParam(*p, Params::growlRate), Params::defaultOf(Params::growlRate)) && pm.isModified();
                dice.redo();
                const bool redoKept = std::abs(getParam(*p, Params::growl) - 33.0f) < 0.05f && std::abs(getParam(*p, Params::growlRate) - rolledRate) < 1.0e-3f;
                check(undoKept && redoKept, "undo and redo keep a hand edit to a knob the roll changed");
            }

            // Loading the same preset again, or a session with the same preset, also ends the history.
            {
                p->setCurrentProgram(0);
                dice.rollAll();
                dice.rollAll();
                p->setCurrentProgram(0);
                const auto reloaded = normalisedState(*p);
                check(!dice.canUndo() && !dice.undo() && normalisedState(*p) == reloaded, "the same preset loaded again ends the roll history");

                dice.rollAll();
                pm.loadPreset(0);
                const auto menuLoaded = normalisedState(*p);
                check(!dice.canUndo() && !dice.undo() && normalisedState(*p) == menuLoaded, "the same preset picked from the menu ends the roll history");

                p->setCurrentProgram(presetIndex("Kaiju"));
                dice.rollAll();
                p->setCurrentProgram(presetIndex("Init"));
                p->setCurrentProgram(presetIndex("Kaiju"));
                const auto backAgain = normalisedState(*p);
                check(!dice.canUndo() && !dice.undo() && normalisedState(*p) == backAgain, "A, roll, B, A ends the roll history");

                juce::MemoryBlock session;
                p->getStateInformation(session);
                dice.rollModule(static_cast<int>(Params::ModuleId::ring));
                p->setStateInformation(session.getData(), static_cast<int>(session.getSize()));
                const auto recalled = normalisedState(*p);
                check(!dice.canUndo() && !dice.undo() && normalisedState(*p) == recalled, "a session with the same preset name ends the roll history");

                // A roll after a load starts a new history that works.
                dice.rollAll();
                dice.undo();
                check(normalisedState(*p) == recalled, "a roll after a load undoes");
            }

            // The host hears of a program change only when the program changed.
            {
                std::vector<juce::AudioProcessorListener::ChangeDetails> sent;
                DiceRoller roller(p->getValueTreeState(), pm, [&sent](const juce::AudioProcessorListener::ChangeDetails& d) { sent.push_back(d); });
                roller.setSeed(9);
                p->setCurrentProgram(presetIndex("Kaiju"));
                roller.rollAll();
                roller.undo();
                roller.redo();
                const bool allQuiet = sent.size() == 2 && !sent[0].programChanged && !sent[1].programChanged && sent[0].nonParameterStateChanged;
                p->setCurrentProgram(0);
                sent.clear();
                roller.rollPreset();
                roller.undo();
                check(allQuiet && sent.size() == 1 && sent[0].programChanged, "a dice undo reports a program change only when the program changed");
            }
        }

        // 3. A module's die: that module's knobs and choices only, inside their ranges.
        {
            auto p = makeProcessor(48000.0, 512);
            auto& dice = p->getDice();
            int stray = 0, outOfRange = 0, untouched = 0, undoWrong = 0;
            for (int m = 0; m < Params::numModules; ++m)
                for (int seed = 1; seed <= (g_quick ? 4 : 12); ++seed)
                {
                    p->setCurrentProgram((m * 5 + seed) % numFactory);
                    const auto before = normalisedState(*p);
                    const auto plainBefore = snapshot(*p);
                    dice.setSeed(static_cast<std::uint32_t>(1000 * m + seed));
                    dice.rollModule(m);
                    const auto plain = snapshot(*p);
                    bool changed = false;
                    for (size_t i = 0; i < Params::all.size(); ++i)
                    {
                        const auto* id = Params::all[i].id;
                        if (isModuleControl(m, id))
                        {
                            changed = changed || !juce::exactlyEqual(plain[i], plainBefore[i]);
                            const float tol = std::max(Params::all[i].step * 0.5f, 1.0e-4f * (Params::all[i].maxValue - Params::all[i].minValue));
                            outOfRange += Params::Random::allows(id, plain[i], tol) ? 0 : 1;
                        }
                        else if (!juce::exactlyEqual(plain[i], plainBefore[i]))
                        {
                            ++stray;
                            std::printf("    %s's die changed %s\n", Params::modules[static_cast<size_t>(m)].uiName, id);
                        }
                    }
                    untouched += changed ? 0 : 1;
                    dice.undo();
                    undoWrong += normalisedState(*p) != before ? 1 : 0;
                }
            check(stray == 0, juce::String(stray) + " parameters outside the module changed by its die");
            check(outOfRange == 0, juce::String(outOfRange) + " module-die values outside their range");
            check(untouched == 0, "a module's die changes something");
            check(undoWrong == 0, "a module roll undoes exactly");
        }

        // 4. Gestures: every parameter a roll changes gets one begin and one end, and no other does.
        {
            auto p = makeProcessor(48000.0, 512);
            GestureCounter counter;
            p->addListener(&counter);
            const auto before = normalisedState(*p);
            p->getDice().setSeed(77);
            p->getDice().rollAll();
            const auto after = normalisedState(*p);
            int bad = 0, changedCount = 0;
            for (size_t i = 0; i < before.size(); ++i)
            {
                const bool changed = !juce::exactlyEqual(before[i], after[i]);
                changedCount += changed ? 1 : 0;
                const int b = counter.begins[static_cast<int>(i)], e = counter.ends[static_cast<int>(i)];
                bad += changed ? (b != 1 || e != 1) : (b != 0 || e != 0);
            }
            counter.begins.clear();
            counter.ends.clear();
            p->getDice().undo();
            for (size_t i = 0; i < before.size(); ++i)
            {
                const bool changed = !juce::exactlyEqual(before[i], after[i]);
                const int b = counter.begins[static_cast<int>(i)], e = counter.ends[static_cast<int>(i)];
                bad += changed ? (b != 1 || e != 1) : (b != 0 || e != 0);
            }
            p->removeListener(&counter);
            std::printf("  gestures: a roll changed %d parameters, one begin/end each (and the same for its undo)\n", changedCount);
            check(bad == 0, juce::String(bad) + " parameters with the wrong gestures for a roll or its undo");
        }

        // 5. Loudness after a roll, against Init (the reference voice; the real takes are in the batch
        //    renders, see docs/parameter-audit.md), and no runaway: finite, under the ceiling, no clamps.
        {
            const double sr = 48000.0;
            const int block = 512;
            const auto in = dual(makeVoice(sr, g_quick ? 3.0 : 5.0));
            const int skip = static_cast<int>(1.0 * sr);
            auto ref = makeProcessor(sr, block);
            const double initLufs = integratedLufs(render(*ref, in, block), sr, skip);
            const int rolls = g_quick ? 8 : 40;
            std::vector<double> deltas;
            int broken = 0;
            for (int seed = 1; seed <= rolls; ++seed)
            {
                auto p = makeProcessor(sr, block);
                p->getDice().setSeed(static_cast<std::uint32_t>(seed));
                p->getDice().rollAll();
                restart(*p, sr, block);
                const auto out = render(*p, in, block);
                const auto st = stats(out);
                broken += !st.finite || st.peak > ceilingGain(-1.0f) || p->getChain().getClampCount() > 0 ? 1 : 0;
                deltas.push_back(integratedLufs(out, sr, skip) - initLufs);
            }
            std::sort(deltas.begin(), deltas.end());
            std::printf("  ROLL ALL x%d on the reference voice: loudness against Init %.2f / %.2f / %.2f LU (min / median / max), %d broken\n",
                        rolls, deltas.front(), deltas[deltas.size() / 2], deltas.back(), broken);
            check(broken == 0, "a roll ran away (non-finite, over the ceiling or clamped)");
            check(deltas.front() >= -3.0 && deltas.back() <= 3.0, "ROLL ALL stays within 3 LU of Init");
        }

        // 6. A roll mid-stream: the settings glide (no clicks or zips), at most one chain crossfade, and
        //    none when the stereo split stays where it is. The click measure is gated. The 5 ms level
        //    dips against the two settled renders are only reported: a roll changes PITCH, GROWL,
        //    SHIMMER and ENSEMBLE, whose grains and LFOs are not in phase with fresh renders, so those
        //    frames differ by several dB without any zip (turning PITCH off by hand reads -8 dB too).
        {
            const double sr = 48000.0;
            const int block = 480;
            const auto in = dual(makeVoice(sr, 2.0));
            const int at = static_cast<int>(1.0 * sr) / block * block;
            double worstRatio = 0.0, worstPercentile = 0.0, worstDip = 0.0;
            int extraFades = 0, fadesWithoutSplitChange = 0;
            const int rolls = g_quick ? 3 : 10;
            for (int seed = 1; seed <= rolls; ++seed)
            {
                auto p = makeProcessor(sr, block);
                p->getDice().setSeed(static_cast<std::uint32_t>(500 + seed));
                const auto topologyBefore = roar::CreatureChain::topologyFor(p->readSettings());
                roar::Topology topologyAfter;
                const int latency = p->getLatencySamples();
                const auto r = transitionTest(*p, in, sr, block,
                    [&](int start)
                    {
                        if (start != at)
                            return false;
                        p->getDice().rollAll();
                        topologyAfter = roar::CreatureChain::topologyFor(p->readSettings());
                        return true;
                    },
                    [&](int start) { return std::pair<int, int> { start, start + latency + static_cast<int>(0.12 * sr) }; }, true);
                const int fades = p->getChain().getReorderCount();
                extraFades += fades > 1 ? 1 : 0;
                fadesWithoutSplitChange += topologyAfter == topologyBefore && fades > 0 ? 1 : 0;
                worstRatio = std::max(worstRatio, r.worstSecondDiffRatio);
                worstPercentile = std::max(worstPercentile, r.percentileRatio);
                std::printf("    roll %d: 2nd difference %.2fx, dip %.2f dB, %s, %d crossfade(s)\n", 500 + seed, r.worstSecondDiffRatio, r.worstDipDb,
                            topologyAfter == topologyBefore ? "same split" : "split moved", fades);
                worstDip = std::min(worstDip, r.worstDipDb);
                check(r.peak <= ceilingGain(-1.0f), "a roll mid-stream went over the ceiling");
            }
            std::printf("  %d rolls mid-stream: worst 2nd difference %.2fx the steady renders', %.2fx the 99.9th percentile outside; worst 5 ms dip %.2f dB\n",
                        rolls, worstRatio, worstPercentile, worstDip);
            check(extraFades == 0, "a roll started more than one chain crossfade");
            check(fadesWithoutSplitChange == 0, "a roll that keeps the stereo split started a chain crossfade");
            check(worstRatio <= 2.0, "a roll mid-stream clicks (2nd difference " + juce::String(worstRatio, 2) + "x the steady renders')");
        }

        // 7. The editor: every die is there, fits its tile or slot, rolls through the processor, and
        //    Cmd+Z / Ctrl+Z takes the roll back.
        {
            auto p = makeProcessor(48000.0, 512);
            std::unique_ptr<juce::AudioProcessorEditor> editorBase(p->createEditor());
            auto& editor = *dynamic_cast<RoarschachAudioProcessorEditor*>(editorBase.get());
            editor.setSize(RoarschachAudioProcessorEditor::c_baseWidth, RoarschachAudioProcessorEditor::c_baseHeight);
            auto& strip = RoarschachEditorInspector::strip(editor);
            strip.refresh();
            int overlaps = 0, outside = 0, tooSmall = 0, tooClose = 0;
            for (int m = 0; m < Params::numModules; ++m)
            {
                auto* die = strip.getModuleDice(m);
                auto* tile = strip.getModuleTile(m);
                if (die == nullptr || tile == nullptr)
                {
                    ++outside;
                    continue;
                }
                const auto d = die->getBounds();
                outside += tile->getLocalBounds().contains(d) ? 0 : 1;
                tooSmall += d.getWidth() < 22 || d.getHeight() < 22 || die->getPillSize() < 18 ? 1 : 0;
                for (auto* child : tile->getChildren())
                    if (child != die && child->isVisible() && child->getBounds().intersects(d))
                        ++overlaps;
                // The mode chip: at least 6 px from the die's pill, and not touching its clickable area.
                if (auto* chip = strip.getModeChip(m))
                {
                    const float pillLeft = static_cast<float>(d.getX()) + die->getPillBounds().getX() - 0.5f;
                    tooClose += pillLeft - static_cast<float>(chip->getRight()) < 6.0f || chip->getRight() >= d.getX() ? 1 : 0;
                }
            }
            auto* all = strip.getRollAllDice();
            const auto slot = strip.getSlotBounds(Params::numModules);
            outside += all != nullptr && slot.contains(all->getBounds()) ? 0 : 1;
            for (int m = 0; m < Params::numModules; ++m)
                overlaps += strip.getModuleTile(m)->getBounds().intersects(all->getBounds()) ? 1 : 0;
            check(outside == 0, "every die sits inside its tile, and ROLL ALL in the empty slot");
            check(overlaps == 0, juce::String(overlaps) + " dice overlap another control");
            check(tooSmall == 0, "the module dice click over at least 22 px, with an 18 px pill");
            check(tooClose == 0, juce::String(tooClose) + " module dice too close to their mode chip");

            // The drawn die at the editor's opening scale on a 1x display: large enough to read, and a
            // small die shows at most four pips.
            {
                const float defaultScale = 0.8f;
                const float moduleDie = DiceButton::dieSideFor(18.0f) * defaultScale;
                const float minScaleDie = DiceButton::dieSideFor(18.0f) * 0.5f;
                check(moduleDie >= 10.0f, "a module die is drawn " + juce::String(moduleDie, 1) + " px at the default scale on a 1x display (10 px or more)");
                bool fewPips = true;
                for (int face = 1; face <= 6; ++face)
                    fewPips = fewPips && DiceButton::drawnFace(face, minScaleDie) <= 4 && DiceButton::drawnFace(face, moduleDie) <= 4
                                      && DiceButton::drawnFace(face, moduleDie * 2.0f) == face;
                check(fewPips, "small dice show at most four pips, a 2x die every face");
            }

            // RANDOMISE ALL's caption is part of its button: a click under the die still rolls.
            {
                const auto b = all->getLocalBounds();
                check(all->getHeight() > all->getPillSize() + 10 && all->getWidth() >= 60
                          && all->hitTest(b.getCentreX(), b.getBottom() - 4), "RANDOMISE ALL's caption is part of its button");
            }

            // Every die has a "show menu" action for screen readers, and Shift+F10 opens the menu.
            {
                int menus = 0;
                std::vector<DiceButton*> dice { all, &RoarschachEditorInspector::presetDice(editor) };
                for (int m = 0; m < Params::numModules; ++m)
                    dice.push_back(strip.getModuleDice(m));
                for (auto* die : dice)
                {
                    // (getAccessibilityHandler needs a window on screen: the handler the die makes for it.)
                    const auto handler = die->createAccessibilityHandler();
                    const bool hasMenu = handler != nullptr && handler->getActions().contains(juce::AccessibilityActionType::showMenu);
                    int opened = 0;
                    auto keep = die->onSecondaryClick;
                    die->onSecondaryClick = [&opened] { ++opened; };
                    die->keyPressed(juce::KeyPress(juce::KeyPress::F10Key, juce::ModifierKeys::shiftModifier, 0));
                    if (hasMenu)
                        handler->getActions().invoke(juce::AccessibilityActionType::showMenu);
                    die->onSecondaryClick = keep;
                    menus += hasMenu && opened == 2 ? 1 : 0;
                }
                check(menus == static_cast<int>(dice.size()), "every die opens its undo menu from the keyboard and for screen readers");
            }

            // Off modules dim their die with the rest of the tile.
            {
                const auto ring = static_cast<int>(Params::ModuleId::ring);
                setParam(*p, Params::modules[static_cast<size_t>(ring)].onId, 0.0f);
                strip.refresh();
                const float offAlpha = strip.getModuleDice(ring)->getAlpha();
                setParam(*p, Params::modules[static_cast<size_t>(ring)].onId, 1.0f);
                strip.refresh();
                check(offAlpha < 0.6f && strip.getModuleDice(ring)->getAlpha() > 0.99f, "an Off module's die is dimmed");
            }

            // Cmd+Z / Shift+Cmd+Z on the die that has the focus; anywhere else they are the host's.
            const auto undoKey = juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0);
            const auto redoKey = juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0);
            p->setCurrentProgram(0);
            const auto init = normalisedState(*p);
            all->onClick();
            check(normalisedState(*p) != init && p->getPresetManager().isModified(), "ROLL ALL in the editor rolls");
            check(!editor.keyPressed(undoKey) && !strip.keyPressed(undoKey) && normalisedState(*p) != init,
                  "Cmd+Z away from a die is left to the host");
            all->keyPressed(undoKey);
            check(normalisedState(*p) == init && !p->getPresetManager().isModified(), "Cmd+Z on the die undoes ROLL ALL");
            all->keyPressed(redoKey);
            check(normalisedState(*p) != init, "Shift+Cmd+Z redoes it");
            auto* growlDie = strip.getModuleDice(static_cast<int>(Params::ModuleId::growl));
            growlDie->onClick();
            growlDie->keyPressed(undoKey);
            growlDie->keyPressed(undoKey);
            check(normalisedState(*p) == init, "Cmd+Z twice undoes a module roll and ROLL ALL");
            check(!growlDie->keyPressed(undoKey), "Cmd+Z with nothing to undo is left to the host");
        }
    }

    void testEditor()
    {
        std::printf("[editor] tooltips, menus, moves, drag, chips, value boxes, host moves\n");
        auto p = makeProcessor(48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editorBase(p->createEditor());
        auto& editor = *dynamic_cast<RoarschachAudioProcessorEditor*>(editorBase.get());
        editor.setSize(RoarschachAudioProcessorEditor::c_baseWidth, RoarschachAudioProcessorEditor::c_baseHeight);
        auto& strip = RoarschachEditorInspector::strip(editor);
        strip.refresh();
        const auto forbidden = forbiddenWords();
        std::printf("  %d forbidden names checked%s\n", forbidden.size(), forbidden.isEmpty() ? " (word list not found)" : "");

        const auto badText = [&forbidden](const juce::String& t) -> juce::String
        {
            for (const char* code : { "::", "m_", "Params", "nullptr", "TODO" })
                if (t.contains(code))
                    return code;
            for (const auto& w : forbidden)
                if (t.contains(w))
                    return w;
            return {};
        };

        // 1. Every control has a tooltip in plain words.
        {
            int tips = 0, controls = 0, missing = 0, bad = 0;
            std::function<void(juce::Component&)> walk = [&](juce::Component& c)
            {
                const bool isControl = dynamic_cast<juce::Slider*>(&c) != nullptr || dynamic_cast<juce::Button*>(&c) != nullptr;
                auto* client = dynamic_cast<juce::TooltipClient*>(&c);
                const auto tip = client != nullptr ? client->getTooltip() : juce::String();
                if (isControl)
                {
                    ++controls;
                    if (tip.isEmpty())
                    {
                        ++missing;
                        std::printf("    no tooltip: %s\n", c.getTitle().toRawUTF8());
                    }
                }
                if (tip.isNotEmpty())
                {
                    ++tips;
                    if (const auto why = badText(tip); why.isNotEmpty())
                    {
                        ++bad;
                        std::printf("    bad tooltip (%s): %s\n", why.toRawUTF8(), tip.toRawUTF8());
                    }
                }
                for (auto* child : c.getChildren())
                    walk(*child);
            };
            walk(editor);
            std::printf("  %d controls, %d tooltips\n", controls, tips);
            check(controls > 80, "editor has its controls (" + juce::String(controls) + ")");
            check(missing == 0, juce::String(missing) + " controls without a tooltip");
            check(bad == 0, juce::String(bad) + " tooltips with code identifiers or forbidden names");
        }

        // 2. Menu texts, chip texts and captions are clean too.
        {
            juce::StringArray texts;
            for (int m = 0; m < Params::numModules; ++m)
            {
                collectMenuTexts(strip.buildMoveMenu(m), texts);
                std::vector<std::pair<const char*, int>> map;
                collectMenuTexts(strip.buildModeMenu(m, map), texts);
                texts.add(strip.modeChipText(m, 1000.0f));
            }
            int bad = 0;
            for (const auto& t : texts)
                if (const auto why = badText(t); why.isNotEmpty())
                {
                    ++bad;
                    std::printf("    bad menu text (%s): %s\n", why.toRawUTF8(), t.toRawUTF8());
                }
            check(bad == 0, juce::String(bad) + " menu texts with code identifiers or forbidden names");
        }

        // 3. Every move menu action gives a valid permutation, with the module where the item says,
        //    written as one gesture.
        {
            std::mt19937 rng(11);
            int failures = 0, actions = 0;
            GestureLog log;
            p->addListener(&log);
            for (int m = 0; m < Params::numModules; ++m)
            {
                std::vector<int> items { SignalFlowStrip::moveToStart, SignalFlowStrip::moveEarlier, SignalFlowStrip::moveLater,
                                         SignalFlowStrip::moveToEnd, SignalFlowStrip::resetOrder };
                for (int other = 0; other < Params::numModules; ++other)
                    if (other != m)
                        items.push_back(SignalFlowStrip::moveAfterBase + other);

                for (const int item : items)
                {
                    const auto start = randomOrder(rng);
                    setOrder(*p, start);
                    strip.refresh();
                    log.events.clear();
                    const auto before = strip.getOrder();
                    const int slotBefore = static_cast<int>(std::find(before.begin(), before.end(), m) - before.begin());
                    strip.applyMove(m, item);
                    const auto after = strip.getOrder();
                    const int slot = static_cast<int>(std::find(after.begin(), after.end(), m) - after.begin());
                    ++actions;

                    bool ok = isPermutationOfPositions(*p) && after == SignalFlowStrip::orderAfterMove(before, m, item);
                    if (item == SignalFlowStrip::moveToStart)
                        ok = ok && slot == 0;
                    else if (item == SignalFlowStrip::moveEarlier)
                        ok = ok && slot == std::max(0, slotBefore - 1);
                    else if (item == SignalFlowStrip::moveLater)
                        ok = ok && slot == std::min(Params::numModules - 1, slotBefore + 1);
                    else if (item == SignalFlowStrip::moveToEnd)
                        ok = ok && slot == Params::numModules - 1;
                    else if (item == SignalFlowStrip::resetOrder)
                        ok = ok && after == Params::resolveOrder(Params::defaultPositions());
                    else
                    {
                        const int target = item - SignalFlowStrip::moveAfterBase;
                        const int targetSlot = static_cast<int>(std::find(after.begin(), after.end(), target) - after.begin());
                        ok = ok && slot == targetSlot + 1;
                    }
                    // The rest keep their relative order.
                    if (item != SignalFlowStrip::resetOrder)
                    {
                        std::vector<int> a, b;
                        for (int x : before) if (x != m) a.push_back(x);
                        for (int x : after) if (x != m) b.push_back(x);
                        ok = ok && a == b;
                    }
                    juce::String why;
                    ok = ok && isOneGesture(log, why);
                    if (!ok && failures++ < 5)
                        std::printf("    move %s item %d: %s -> %s %s\n", Params::modules[static_cast<size_t>(m)].code, item,
                                    orderCodes(before).toRawUTF8(), orderCodes(after).toRawUTF8(), why.toRawUTF8());
                }
            }
            check(failures == 0, juce::String(failures) + " of " + juce::String(actions) + " move menu actions wrong");

            // Turn Off / On flips the module's switch and nothing else.
            log.events.clear();
            const bool wasOn = getParam(*p, Params::growlOn) > 0.5f;
            strip.applyMove(static_cast<int>(Params::ModuleId::growl), SignalFlowStrip::toggleOnOff);
            check((getParam(*p, Params::growlOn) > 0.5f) != wasOn, "Turn Off/On flips the switch");
            check(log.events.size() == 3 && log.events[1].second == Params::indexOf(Params::growlOn), "Turn Off/On is one gesture on the switch");
            strip.applyMove(static_cast<int>(Params::ModuleId::growl), SignalFlowStrip::toggleOnOff);
            p->removeListener(&log);
        }

        // 4. A drag across both rows commits one permutation as one gesture; nothing is written while
        //    the tile is held; Escape cancels.
        {
            setOrder(*p, Params::resolveOrder(Params::defaultPositions()));
            strip.refresh();
            strip.finishAnimations();
            GestureLog log;
            p->addListener(&log);

            const int growl = static_cast<int>(Params::ModuleId::growl);   // slot 7 (row 2, first)
            auto& tile = *strip.getModuleTile(growl);
            const auto grab = juce::Point<float>(50.0f, 12.0f);
            const auto from = tile.getPosition().toFloat() + grab;
            const auto targetSlot = strip.getSlotBounds(2);           // row 1, third
            const auto to = targetSlot.getPosition().toFloat() + grab;
            std::vector<juce::Point<float>> path;
            for (int k = 1; k <= 12; ++k)
                path.push_back(from + (to - from) * (static_cast<float>(k) / 12.0f));

            dragTile(strip, tile, path, false);
            check(strip.isDragging(), "the drag started");
            check(log.events.empty(), "nothing written while the tile is held");
            dragTile(strip, tile, {}, false);   // a press on the moving tile mid-drag does nothing harmful
            tile.mouseUp(mouseEventFor(tile, to - tile.getPosition().toFloat(), grab, true));
            check(!strip.isDragging(), "the drop ended the drag");

            auto expected = Params::resolveOrder(Params::defaultPositions());
            {
                std::vector<int> list;
                for (int x : expected) if (x != growl) list.push_back(x);
                list.insert(list.begin() + 2, growl);
                for (int k = 0; k < Params::numModules; ++k)
                    expected[static_cast<size_t>(k)] = list[static_cast<size_t>(k)];
            }
            juce::String why;
            check(strip.getOrder() == expected, "drop gives " + orderCodes(strip.getOrder()) + ", expected " + orderCodes(expected));
            check(isOneGesture(log, why), "the drop is one gesture: " + why);
            check(isPermutationOfPositions(*p), "the drop writes a permutation");
            strip.finishAnimations();
            check(tile.getBounds() == strip.getSlotBounds(2), "the dropped tile settles in its slot");

            // Escape
            log.events.clear();
            const auto before = strip.getOrder();
            auto& other = *strip.getModuleTile(static_cast<int>(Params::ModuleId::sub));
            const auto start = other.getPosition().toFloat() + grab;
            dragTile(strip, other, { start + juce::Point<float>(-200.0f, -60.0f), start + juce::Point<float>(-500.0f, -170.0f) }, false);
            check(strip.isDragging(), "second drag started");
            strip.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
            check(!strip.isDragging(), "Escape ends the drag");
            {
                // The button is still down: further moves must not pick the tile up again, and the
                // release must not commit the cancelled order.
                const auto down = start - other.getPosition().toFloat();
                for (const auto pt : { start + juce::Point<float>(-520.0f, -170.0f), start + juce::Point<float>(-560.0f, -175.0f) })
                    other.mouseDrag(mouseEventFor(other, pt - other.getPosition().toFloat(), down, true));
                check(!strip.isDragging(), "a move after Escape does not restart the drag");
                other.mouseUp(mouseEventFor(other, start + juce::Point<float>(-560.0f, -175.0f) - other.getPosition().toFloat(), down, true));
            }
            check(strip.getOrder() == before && log.events.empty(), "Escape cancels: nothing written");
            strip.finishAnimations();
            check(other.getBounds() == strip.getSlotBounds(static_cast<int>(std::find(before.begin(), before.end(),
                                                                                       static_cast<int>(Params::ModuleId::sub)) - before.begin())),
                  "a cancelled tile goes back to its slot");

            // The host moves the modules mid-drag: the drag ends, and neither the moves that follow
            // nor the release write anything over the host's order.
            {
                strip.finishAnimations();
                log.events.clear();
                auto& held = *strip.getModuleTile(static_cast<int>(Params::ModuleId::roar));
                const auto at = held.getPosition().toFloat() + grab;
                dragTile(strip, held, { at + juce::Point<float>(120.0f, 20.0f), at + juce::Point<float>(260.0f, 40.0f) }, false);
                check(strip.isDragging(), "third drag started");
                auto hostOrder = strip.getOrder();
                std::reverse(hostOrder.begin(), hostOrder.end());
                p->removeListener(&log);
                setOrder(*p, hostOrder);
                p->addListener(&log);
                strip.refresh();
                check(!strip.isDragging(), "a host reorder ends the drag");
                const auto down = at - held.getPosition().toFloat();
                for (const auto pt : { at + juce::Point<float>(280.0f, 40.0f), at + juce::Point<float>(-300.0f, 30.0f) })
                    held.mouseDrag(mouseEventFor(held, pt - held.getPosition().toFloat(), down, true));
                check(!strip.isDragging(), "a move after a host reorder does not restart the drag");
                held.mouseUp(mouseEventFor(held, at + juce::Point<float>(-300.0f, 30.0f) - held.getPosition().toFloat(), down, true));
                check(strip.getOrder() == hostOrder && log.events.empty(), "the release after a host reorder writes nothing");
                p->removeListener(&log);
                setOrder(*p, before);
                p->addListener(&log);
                strip.refresh();
                strip.finishAnimations();
            }

            // Alt+Left on a focused control moves the module one step earlier.
            log.events.clear();
            const int ring = static_cast<int>(Params::ModuleId::ring);
            const auto o0 = strip.getOrder();
            const int s0 = static_cast<int>(std::find(o0.begin(), o0.end(), ring) - o0.begin());
            strip.getModuleTile(ring)->keyPressed(juce::KeyPress(juce::KeyPress::leftKey, juce::ModifierKeys::altModifier, 0));
            const auto o1 = strip.getOrder();
            const int s1 = static_cast<int>(std::find(o1.begin(), o1.end(), ring) - o1.begin());
            check(s1 == s0 - 1 && isOneGesture(log, why), "Alt+Left moves the module one step earlier");
            p->removeListener(&log);
        }

        // 5. The host or a preset moves modules: the strip follows (animated) and writes nothing back.
        {
            GestureLog log;
            p->addListener(&log);
            std::mt19937 rng(5);
            const auto order = randomOrder(rng);
            p->removeListener(&log);
            setOrder(*p, order);
            p->addListener(&log);
            strip.refresh();
            bool moving = false;
            for (int k = 0; k < Params::numModules; ++k)
                moving = moving || strip.getModuleTile(order[static_cast<size_t>(k)])->getBounds() != strip.getSlotBounds(k);
            check(moving, "a host move animates (the tiles are not there yet)");
            strip.finishAnimations();
            bool placed = true;
            for (int k = 0; k < Params::numModules; ++k)
                placed = placed && strip.getModuleTile(order[static_cast<size_t>(k)])->getBounds() == strip.getSlotBounds(k);
            check(placed, "after the animation every tile is in its slot");
            strip.refresh();
            check(log.events.empty(), "the strip never writes positions in response to the host");
            p->removeListener(&log);
            setOrder(*p, Params::resolveOrder(Params::defaultPositions()));
            strip.refresh();
            strip.finishAnimations();
        }

        // 6. Every mode chip maps to its module's choice parameters, every choice once, the current
        //    one ticked; its text follows the parameters and fits.
        {
            int failures = 0;
            for (int m = 0; m < Params::numModules; ++m)
            {
                const auto& spec = Params::modules[static_cast<size_t>(m)];
                auto* chip = strip.getModeChip(m);
                if (spec.chipIds[0] == nullptr)
                {
                    failures += chip != nullptr ? 1 : 0;
                    continue;
                }
                if (chip == nullptr || chip->getTooltip().isEmpty())
                {
                    ++failures;
                    continue;
                }
                std::vector<std::pair<const char*, int>> map;
                const auto menu = strip.buildModeMenu(m, map);
                std::map<std::string, std::vector<int>> seen;
                for (size_t i = 1; i < map.size(); ++i)
                    seen[map[i].first].push_back(map[i].second);
                int expectedParams = 0;
                for (const char* id : spec.chipIds)
                {
                    if (id == nullptr)
                        continue;
                    ++expectedParams;
                    const auto& s = Params::spec(id);
                    auto& choices = seen[id];
                    std::sort(choices.begin(), choices.end());
                    std::vector<int> all(static_cast<size_t>(s.numChoices));
                    std::iota(all.begin(), all.end(), 0);
                    if (choices != all)
                    {
                        ++failures;
                        std::printf("    chip %s: %s choices not mapped one to one\n", spec.code, id);
                    }
                }
                if (static_cast<int>(seen.size()) != expectedParams)
                    ++failures;

                // Ticks: exactly the current choice of each parameter.
                std::function<void(const juce::PopupMenu&)> ticks = [&](const juce::PopupMenu& menuToCheck)
                {
                    for (juce::PopupMenu::MenuItemIterator it(menuToCheck); it.next();)
                    {
                        const auto& item = it.getItem();
                        if (item.subMenu != nullptr)
                            ticks(*item.subMenu);
                        else if (item.itemID > 0 && item.itemID < static_cast<int>(map.size()))
                        {
                            const auto& [id, c] = map[static_cast<size_t>(item.itemID)];
                            const bool current = juce::roundToInt(getParam(*p, id)) == c;
                            if (item.isTicked != current)
                            {
                                ++failures;
                                std::printf("    chip %s: %s tick wrong\n", spec.code, item.text.toRawUTF8());
                            }
                        }
                    }
                };
                ticks(menu);

                // Every combination of choices: the chip text names them and fits the chip.
                const float room = static_cast<float>(chip->getWidth() - 16);
                const auto font = Theme::Fonts().label(9.0f);
                std::vector<const char*> ids;
                for (const char* id : spec.chipIds)
                    if (id != nullptr)
                        ids.push_back(id);
                std::function<void(size_t)> combos = [&](size_t k)
                {
                    if (k == ids.size())
                    {
                        const auto t = strip.modeChipText(m, room);
                        const float w = juce::GlyphArrangement::getStringWidth(font, t);
                        if (w > static_cast<float>(chip->getWidth()) - 10.0f)
                        {
                            ++failures;
                            std::printf("    chip %s: \"%s\" is %.0f px in a %d px chip\n", spec.code, t.toRawUTF8(), w, chip->getWidth());
                        }
                        return;
                    }
                    const auto& s = Params::spec(ids[k]);
                    for (int c = 0; c < s.numChoices; ++c)
                    {
                        setParam(*p, ids[k], static_cast<float>(c));
                        combos(k + 1);
                    }
                    setParam(*p, ids[k], Params::defaultOf(ids[k]));
                };
                combos(0);
            }
            check(failures == 0, juce::String(failures) + " mode chip problems");
        }

        // 7. Knob value boxes and captions never clip, at the extremes and the default.
        {
            int failures = 0;
            std::vector<juce::Component*> tiles { strip.getInTile(), strip.getOutputTile() };
            for (int m = 0; m < Params::numModules; ++m)
                tiles.push_back(strip.getModuleTile(m));
            for (auto* tile : tiles)
            {
                // CHAMBER's tile holds four knobs side by side in the same width as the others'
                // two or three, so its knobs are smaller (still at least 28 px).
                const float minimumKnob = strip.getKnobs(tile).size() >= 4 ? 28.0f : 36.0f;
                for (const auto& [slider, id] : strip.getKnobs(tile))
                {
                    juce::Label* box = nullptr;
                    for (auto* c : slider->getChildren())
                        if (auto* l = dynamic_cast<juce::Label*>(c))
                            box = l;
                    if (box == nullptr)
                    {
                        ++failures;
                        continue;
                    }
                    const auto font = box->getLookAndFeel().getLabelFont(*box);
                    for (const double v : { slider->getMinimum(), slider->getMaximum(), static_cast<double>(Params::defaultOf(id)),
                                            slider->getMinimum() + 0.37 * (slider->getMaximum() - slider->getMinimum()) })
                    {
                        const auto t = slider->getTextFromValue(v);
                        const float w = juce::GlyphArrangement::getStringWidth(font, t);
                        if (w > static_cast<float>(box->getWidth()) - 2.0f)
                        {
                            ++failures;
                            std::printf("    value \"%s\" of %s is %.0f px in a %d px box\n", t.toRawUTF8(), id, w, box->getWidth());
                        }
                    }
                    const float knob = static_cast<float>(std::min(slider->getWidth(), slider->getHeight() - 20)) - 8.0f;
                    if (knob < minimumKnob)
                    {
                        ++failures;
                        std::printf("    knob %s is only %.0f px\n", id, knob);
                    }
                }
                // The captions above the knobs show their whole word (no "AMO...").
                for (auto* c : tile->getChildren())
                    if (auto* caption = dynamic_cast<juce::Label*>(c); caption != nullptr && caption->isVisible() && caption->getText().isNotEmpty())
                    {
                        const auto area = caption->getBorderSize().subtractedFrom(caption->getLocalBounds());
                        const float w = juce::GlyphArrangement::getStringWidth(caption->getFont(), caption->getText());
                        if (w > static_cast<float>(area.getWidth()))
                        {
                            ++failures;
                            std::printf("    caption \"%s\" is %.0f px in %d px\n", caption->getText().toRawUTF8(), w, area.getWidth());
                        }
                    }
            }
            check(failures == 0, juce::String(failures) + " value boxes or knobs too small");
        }

        // 8. RING's second knob swaps with Track; Off tiles dim but stay usable.
        {
            const int ring = static_cast<int>(Params::ModuleId::ring);
            const auto knobs = strip.getKnobs(strip.getModuleTile(ring));
            setParam(*p, Params::ringTrack, 1.0f);
            strip.refresh();
            const bool ratioShown = knobs[2].first->isVisible() && !knobs[1].first->isVisible();
            setParam(*p, Params::ringTrack, 0.0f);
            strip.refresh();
            const bool freqShown = knobs[1].first->isVisible() && !knobs[2].first->isVisible();
            check(ratioShown && freqShown, "RING shows Ratio in Follow and Freq in Fixed");
            check(knobs[1].first->getBounds() == knobs[2].first->getBounds(), "Freq and Ratio share one place");

            setParam(*p, Params::pitchOn, 0.0f);
            strip.refresh();
            const auto pitch = strip.getKnobs(strip.getModuleTile(static_cast<int>(Params::ModuleId::pitch)));
            check(std::abs(pitch[0].first->getAlpha() - 0.45f) < 0.01f && pitch[0].first->isEnabled(), "an Off tile is dimmed and its knobs still work");
            setParam(*p, Params::pitchOn, 1.0f);
        }

        editorBase.reset();
    }

    //==========================================================================
    // The editor's usability: type size and contrast, the window size, undo for preset loads, the
    // first-run hint, the quick start, knobs the current settings do not use, the latency note.

    /** WCAG contrast ratio of two opaque colours. */
    double contrastRatio(juce::Colour a, juce::Colour b)
    {
        const auto luminance = [](juce::Colour c)
        {
            const auto channel = [](float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
            return 0.2126 * channel(c.getFloatRed()) + 0.7152 * channel(c.getFloatGreen()) + 0.0722 * channel(c.getFloatBlue());
        };
        const double la = luminance(a), lb = luminance(b);
        return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
    }

    void writePng(const juce::Image& image, const juce::File& file)
    {
        file.deleteFile();
        juce::FileOutputStream stream(file);
        juce::PNGImageFormat png;
        if (stream.openedOk() && png.writeImageToStream(image, stream))
            std::printf("  wrote %s\n", file.getFullPathName().toRawUTF8());
    }

    /** Pictures of the editor at several window scales, the creature view's hint and the About
        window's content into the folder ROARSCHACH_UI_SHOTS names (for looking at the layout;
        nothing is written without it). */
    void writeUiShots()
    {
        const auto dirName = juce::SystemStats::getEnvironmentVariable("ROARSCHACH_UI_SHOTS", {});
        if (dirName.isEmpty())
            return;
        const juce::File dir(dirName);
        dir.createDirectory();
        for (const float scale : { 0.65f, 0.8f, 1.0f })
        {
            auto p = makeProcessor(48000.0, 512);
            std::unique_ptr<juce::AudioProcessorEditor> editorBase(p->createEditor());
            auto& editor = *dynamic_cast<RoarschachAudioProcessorEditor*>(editorBase.get());
            editor.setSize(juce::roundToInt(RoarschachAudioProcessorEditor::c_baseWidth * scale),
                           juce::roundToInt(RoarschachAudioProcessorEditor::c_baseHeight * scale));
            // The second module on with its room blended in, so its four narrow knobs show lit.
            setParam(*p, Params::chamberOn, 1.0f);
            RoarschachEditorInspector::strip(editor).refresh();
            RoarschachEditorInspector::monster(editor).refreshOverlay(2.0f);
            const auto name = "editor-" + juce::String(juce::roundToInt(scale * 100.0f));
            writePng(editor.createComponentSnapshot(editor.getLocalBounds(), true, 1.0f), dir.getChildFile(name + ".png"));
            writePng(editor.createComponentSnapshot(editor.getLocalBounds(), true, 2.0f), dir.getChildFile(name + "@2x.png"));

            if (scale == 1.0f)
            {
                auto& look = dynamic_cast<RoarschachLookAndFeel&>(RoarschachEditorInspector::lookAndFeel(editor));
                const auto note = RoarschachAudioProcessorEditor::latencyNote(p->getLatencySamples(), p->getSampleRate());
                auto about = AboutWindow::createContent(look, note);
                writePng(about->createComponentSnapshot(about->getLocalBounds(), true, 2.0f), dir.getChildFile("about@2x.png"));
            }
        }
    }

    void testUi()
    {
        std::printf("[ui] window size, type and contrast, preset undo, hint, quick start, unused knobs, latency note\n");
        using Editor = RoarschachAudioProcessorEditor;
        const int baseW = Editor::c_baseWidth, baseH = Editor::c_baseHeight;
        const auto near = [](float a, float b) { return std::abs(a - b) < 0.011f; };

        // 1. The size a first window opens at: the largest of 80-100 % that leaves room on the display.
        {
            struct Case { int w, h; float want; };
            int wrong = 0;
            for (const auto& c : { Case { 1280, 720, 0.8f }, Case { 1440, 847, 0.8f }, Case { 1728, 1079, 0.9f }, Case { 1920, 1040, 1.0f },
                                   Case { 2560, 1400, 1.0f }, Case { 5120, 2800, 1.0f }, Case { 0, 0, 0.8f }, Case { 3000, 100, 0.8f } })
            {
                const float got = Editor::defaultScaleForDisplay(c.w, c.h);
                if (!near(got, c.want))
                {
                    ++wrong;
                    std::printf("    a %d x %d display opens at %.2f, expected %.2f\n", c.w, c.h, got, c.want);
                }
                // The window it gives fits the display, unless the display is smaller than the smallest default.
                if (got > Editor::c_smallestDefaultScale + 0.001f && (static_cast<float>(baseW) * got > static_cast<float>(c.w) || static_cast<float>(baseH) * got > static_cast<float>(c.h)))
                    ++wrong;
            }
            float previous = 0.0f;
            bool monotone = true, inRange = true;
            for (int w = 800; w <= 4000; w += 40)
            {
                const float s = Editor::defaultScaleForDisplay(w, w * 10 / 16);
                monotone = monotone && s >= previous;
                inRange = inRange && s >= Editor::c_smallestDefaultScale && s <= Editor::c_largestDefaultScale;
                previous = s;
            }
            check(wrong == 0, "the default window scale is the largest of 80-100 % that fits the display");
            check(monotone && inRange, "the default window scale grows with the display and stays within 80-100 %");
            check(near(Editor::c_minScale, 0.65f), "the smallest window scale is 65 %");
        }

        // 2. The size last chosen is kept in a settings file and read back (clamped; a damaged or
        //    missing file gives none).
        {
            const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile("roarschach-ui-test-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt()) + ".settings");
            file.deleteFile();
            check(Editor::readRememberedScale(file) == 0.0f, "no remembered size without a settings file");
            check(Editor::rememberScale(file, 0.9f) && near(Editor::readRememberedScale(file), 0.9f), "the chosen size is remembered");
            check(Editor::rememberScale(file, 3.0f) && near(Editor::readRememberedScale(file), Editor::c_maxScale), "a remembered size above the largest is clamped");
            check(Editor::rememberScale(file, 0.1f) && near(Editor::readRememberedScale(file), Editor::c_minScale), "a remembered size below the smallest is clamped");
            check(!Editor::rememberScale(file, std::numeric_limits<float>::quiet_NaN()) && near(Editor::readRememberedScale(file), Editor::c_minScale),
                  "a non-finite size is not remembered");
            file.replaceWithText("not a settings file");
            check(Editor::readRememberedScale(file) == 0.0f, "a damaged settings file gives no remembered size");
            file.deleteFile();
            check(Editor::getSettingsFile().getFileName() == "Roarschach.settings"
                      && Editor::getSettingsFile().getParentDirectory().getFileName() == "Sammy J",
                  "the settings file is Roarschach's own, beside the update check's");
        }

        // 3. A new editor opens within the default range (or as large as the display allows); a size
        //    saved with the instance wins; the window cannot go under 65 %.
        {
            auto p = makeProcessor(48000.0, 512);
            {
                std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());
                const float scale = static_cast<float>(editor->getWidth()) / static_cast<float>(baseW);
                std::printf("  a new editor opens at %.0f %% here (%d x %d)\n", scale * 100.0f, editor->getWidth(), editor->getHeight());
                check(scale >= Editor::c_minScale - 0.001f && scale <= Editor::c_largestDefaultScale + 0.001f, "a new editor opens at 100 % or less");
                check(std::abs(static_cast<float>(editor->getHeight()) / static_cast<float>(baseH) - scale) < 0.002f, "the editor keeps its shape");
                auto* limits = editor->getConstrainer();
                check(limits != nullptr && limits->getMinimumWidth() == juce::roundToInt(baseW * Editor::c_minScale)
                          && limits->getMinimumHeight() == juce::roundToInt(baseH * Editor::c_minScale),
                      "the window's smallest size is 65 %");
                editor->setSize(juce::roundToInt(baseW * 0.7f), juce::roundToInt(baseH * 0.7f));
            }
            std::unique_ptr<juce::AudioProcessorEditor> again(p->createEditor());
            check(again->getWidth() == juce::roundToInt(baseW * 0.7f), "the size saved with the instance is the size its editor opens at");
        }

        // 4. Contrast: every rank of text reads on every panel colour (WCAG AA is 4.5 to 1).
        {
            namespace C = Theme::Colours;
            const double faintPanel = contrastRatio(C::textFaint, C::panel), faintBack = contrastRatio(C::textFaint, C::background),
                         faintRaised = contrastRatio(C::textFaint, C::panelRaised), dimRaised = contrastRatio(C::textDim, C::panelRaised);
            std::printf("  contrast: faint text %.2f on a panel, %.2f on the background, %.2f on a raised panel; dim text %.2f on a raised panel\n",
                        faintPanel, faintBack, faintRaised, dimRaised);
            check(faintPanel >= 4.5 && faintBack >= 4.5, "the faintest text is at least 4.5 to 1 on the panel and the background");
            check(faintRaised >= 4.0, "the faintest text is at least 4 to 1 on a raised panel");
            check(dimRaised >= 4.5 && contrastRatio(C::textDim, C::panel) > faintPanel + 1.5
                      && contrastRatio(C::text, C::panel) > contrastRatio(C::textDim, C::panel) + 3.0,
                  "the three ranks of text stay apart");
        }

        // 5. Captions: 11 design px wherever the tile has the room (8.8 px at the smallest default
        //    window), never under 9, each whole word shown, clear of the captions beside it and of the sides of its tile.
        {
            auto p = makeProcessor(48000.0, 512);
            std::unique_ptr<juce::AudioProcessorEditor> editorBase(p->createEditor());
            auto& editor = *dynamic_cast<Editor*>(editorBase.get());
            editor.setSize(baseW, baseH);
            auto& strip = RoarschachEditorInspector::strip(editor);
            strip.refresh();
            int captions = 0, small = 0, tiny = 0, cut = 0, outside = 0, touching = 0;
            float smallest = 100.0f;
            std::vector<juce::Component*> tiles { strip.getInTile(), strip.getOutputTile() };
            for (int m = 0; m < Params::numModules; ++m)
                tiles.push_back(strip.getModuleTile(m));
            const auto* chamberTile = strip.getModuleTile(static_cast<int>(Params::ModuleId::chamber));
            for (auto* tile : tiles)
            {
                std::vector<juce::Label*> labels;
                for (auto* c : tile->getChildren())
                    if (auto* label = dynamic_cast<juce::Label*>(c); label != nullptr && label->isVisible() && label->getText().isNotEmpty())
                        labels.push_back(label);
                for (auto* label : labels)
                {
                    ++captions;
                    const float height = label->getFont().getHeight();
                    smallest = std::min(smallest, height);
                    tiny += height < 9.0f - 0.01f ? 1 : 0;
                    small += tile != chamberTile && height < 11.0f - 0.01f ? 1 : 0;
                    cut += juce::GlyphArrangement::getStringWidth(label->getFont(), label->getText()) > static_cast<float>(label->getWidth()) ? 1 : 0;
                    cut += height > static_cast<float>(label->getHeight()) ? 1 : 0;
                    outside += tile->getLocalBounds().contains(label->getBounds()) ? 0 : 1;
                    for (auto* other : labels)
                        touching += other != label && other->getBounds().intersects(label->getBounds()) ? 1 : 0;
                }
            }
            std::printf("  %d knob captions, the smallest %.2f design px (%.1f px in an 80 %% window)\n", captions, smallest, smallest * 0.8f);
            check(captions >= 30, "the knob captions were found");
            check(small == 0, juce::String(small) + " captions under 11 design px outside CHAMBER");
            check(tiny == 0, juce::String(tiny) + " captions under 9 design px");
            check(cut == 0, juce::String(cut) + " captions cut short");
            check(outside == 0 && touching == 0, "captions overlap each other or leave their tile");
        }

        // 6. A preset load from the preset bar is one step in the dice's history: undo brings back the
        //    sound that was showing (edits and all), redo loads the preset again; a hand edit made
        //    after the load survives the undo; a load by the host ends the history.
        {
            auto p = makeProcessor(48000.0, 512);
            auto& dice = p->getDice();
            auto& pm = p->getPresetManager();
            dice.setSeed(5);
            const int face = dice.getFace();
            check(!dice.canUndo() && !dice.undo(), "nothing to undo before anything is loaded");

            setParam(*p, Params::pitch, -7.0f);
            setParam(*p, Params::roar, 83.0f);
            const auto edited = normalisedState(*p);
            const auto editedName = pm.getCurrentName();
            check(pm.isModified(), "an edit marks the preset as edited");

            check(dice.loadNext(), "the next preset loads");
            const auto loaded = normalisedState(*p);
            const auto loadedName = pm.getCurrentName();
            check(loadedName != editedName && !pm.isModified() && loaded != edited, "the load changed the sound and the name");
            check(dice.canUndo() && dice.getUndoManager().getUndoDescription() == "Load", "the load is a step called Load");
            check(dice.getFace() == face, "a load leaves the dice's faces alone");

            check(dice.undo(), "the load is undone");
            check(normalisedState(*p) == edited && pm.getCurrentName() == editedName && pm.isModified(), "undo brings back the edited sound, its name and its edited mark");
            check(dice.canRedo() && dice.getUndoManager().getRedoDescription() == "Load" && dice.redo(), "the load is redone");
            check(normalisedState(*p) == loaded && pm.getCurrentName() == loadedName && !pm.isModified(), "redo loads the preset again, unedited");

            // Menu loads, the arrows and the preset die share one history, newest first.
            const int third = std::min(7, PresetManager::getNumFactory() - 1);
            check(dice.loadPreset(third), "a preset loads by its place in the list");
            const auto thirdState = normalisedState(*p);
            check(dice.loadPrevious(), "the previous preset loads");
            check(dice.rollPreset() >= 0, "the preset die rolls");
            check(dice.undo() && dice.undo() && normalisedState(*p) == thirdState && pm.getCurrentIndex() == third, "two undos go back past the die and the arrow");
            check(dice.undo() && normalisedState(*p) == loaded && dice.undo() && normalisedState(*p) == edited && !dice.canUndo(), "and on back to the first sound");
            check(dice.redo() && dice.redo() && normalisedState(*p) == thirdState, "redo goes forward again");

            // A knob turned after the load keeps its value through the undo; the rest goes back.
            const float handPitch = getParam(*p, Params::pitch) > -15.0f ? -20.0f : -3.0f;
            setParam(*p, Params::pitch, handPitch);
            check(dice.undo(), "undo after a hand edit");
            auto expected = loaded;
            const auto afterHand = normalisedState(*p);
            int differing = 0;
            for (size_t i = 0; i < expected.size() && i < afterHand.size(); ++i)
                differing += juce::exactlyEqual(expected[i], afterHand[i]) ? 0 : 1;
            check(std::abs(getParam(*p, Params::pitch) - handPitch) < 0.01f && differing <= 1 && pm.isModified(),
                  "the hand edit survives the undo and the preset shows as edited");

            // A load that does not come from the preset bar (the host's program list) ends the history.
            check(dice.loadNext() && dice.canUndo(), "a load to undo");
            check(pm.loadPreset(0), "the host loads a preset");
            const auto hostState = normalisedState(*p);
            check(!dice.canUndo() && !dice.undo() && normalisedState(*p) == hostState, "a load from outside the preset bar ends the history");
            check(dice.loadNext() && dice.undo() && normalisedState(*p) == hostState && !dice.canUndo(), "the next load starts a new history on that sound");
            check(!dice.loadPreset(100000) && !dice.canUndo(), "a load that fails adds no step");
        }

        // 7. The editor: Cmd+Z / Ctrl+Z takes a load back and Shift redoes it; other keys, and Cmd+Z
        //    with nothing to take back, are left for the host.
        {
            auto p = makeProcessor(48000.0, 512);
            std::unique_ptr<juce::AudioProcessorEditor> editorBase(p->createEditor());
            auto& editor = *dynamic_cast<Editor*>(editorBase.get());
            const juce::KeyPress undoKey('z', juce::ModifierKeys::commandModifier, 0);
            const juce::KeyPress redoKey('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0);
            check(!editor.undoKeyPressed(undoKey) && !editor.undoKeyPressed(redoKey), "with nothing to undo the key is not taken");
            setParam(*p, Params::growl, 61.0f);
            const auto before = normalisedState(*p);
            check(p->getDice().loadNext(), "a preset loads");
            const auto after = normalisedState(*p);
            check(!editor.undoKeyPressed(juce::KeyPress('z')) && !editor.undoKeyPressed(juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0))
                      && normalisedState(*p) == after, "other keys are not taken");
            check(editor.undoKeyPressed(undoKey) && normalisedState(*p) == before, "Cmd+Z takes the load back");
            check(editor.undoKeyPressed(redoKey) && normalisedState(*p) == after, "Shift+Cmd+Z loads it again");
            // Without the keyboard focus on the preset bar (nothing has it here) the editor leaves the key alone.
            check(!editor.keyPressed(undoKey) && normalisedState(*p) == after, "the key is only taken while the preset bar has the focus");
        }

        // 8. The hint: up after 1.5 s of silence in a new instance, gone for good with the first sound.
        {
            const double sr = 48000.0;
            auto p = makeProcessor(sr, 512);
            {
                std::unique_ptr<juce::AudioProcessorEditor> editorBase(p->createEditor());
                auto& view = RoarschachEditorInspector::monster(*dynamic_cast<Editor*>(editorBase.get()));
                view.refreshOverlay(0.5f);
                check(!view.isHintShowing(), "no hint in the first moments");
                view.refreshOverlay(1.2f);
                check(view.isHintShowing(), "the hint shows once the creature has slept through 1.5 s of silence");

                Stereo silence;
                silence.l.assign(4096, 0.0f);
                silence.r = silence.l;
                render(*p, silence, 512);
                view.refreshOverlay(0.1f);
                check(view.isHintShowing(), "silence through the plug-in keeps the hint");

                Stereo voice;
                voice.l = makeVoice(sr, 0.5);
                voice.r = voice.l;
                render(*p, voice, 512);
                view.refreshOverlay(0.1f);
                check(!view.isHintShowing(), "the first sound takes the hint away");
                render(*p, silence, 512);
                render(*p, silence, 512);
                view.refreshOverlay(30.0f);
                check(!view.isHintShowing(), "it stays away when the sound stops");
            }
            std::unique_ptr<juce::AudioProcessorEditor> again(p->createEditor());
            auto& view = RoarschachEditorInspector::monster(*dynamic_cast<Editor*>(again.get()));
            view.refreshOverlay(30.0f);
            check(!view.isHintShowing(), "and when the editor is opened again");

            // The same through a saved session.
            juce::MemoryBlock state;
            p->getStateInformation(state);
            auto q = makeProcessor(sr, 512);
            q->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            std::unique_ptr<juce::AudioProcessorEditor> recalled(q->createEditor());
            auto& recalledView = RoarschachEditorInspector::monster(*dynamic_cast<Editor*>(recalled.get()));
            recalledView.refreshOverlay(30.0f);
            check(!recalledView.isHintShowing(), "and in the session when it is opened again");
        }

        // 9. The quick start (five short lines) and the latency note, which states what the processor reports.
        {
            const auto forbidden = forbiddenWords();
            const auto lines = AboutWindow::quickStartLines();
            int bad = 0, longLines = 0;
            for (const auto& line : lines)
            {
                longLines += line.length() > 110 ? 1 : 0;
                for (const auto& w : forbidden)
                    bad += line.contains(w) ? 1 : 0;
            }
            check(lines.size() == 5 && longLines == 0, "the quick start is five short lines");
            check(bad == 0, "the quick start names nothing it must not");

            int wrong = 0;
            for (const double sr : c_rates)
            {
                auto p = makeProcessor(sr, 512);
                const int latency = p->getLatencySamples();
                const auto note = Editor::latencyNote(latency, p->getSampleRate());
                const auto ms = juce::String(1000.0 * latency / sr, 1) + " ms";
                const bool ok = latency > 0 && note.startsWith(ms) && note.contains(juce::String(latency) + " samples")
                             && AboutWindow::latencyLine(note).contains(note);
                wrong += ok ? 0 : 1;
                std::printf("  latency note at %.0f Hz: %s\n", sr, note.toRawUTF8());
            }
            check(wrong == 0, "the latency note states the latency the processor reports, in ms and samples");
            check(Editor::latencyNote(0, 48000.0).isEmpty() && Editor::latencyNote(2192, 0.0).isEmpty(), "no figure before the host has prepared the plug-in");
            check(AboutWindow::latencyLine({}).isNotEmpty() && !AboutWindow::latencyLine({}).containsAnyOf("0123456789"), "and then the line gives no number");

            // The About window's content lays out inside its window.
            auto p = makeProcessor(48000.0, 512);
            std::unique_ptr<juce::AudioProcessorEditor> editorBase(p->createEditor());
            auto& look = dynamic_cast<RoarschachLookAndFeel&>(RoarschachEditorInspector::lookAndFeel(*dynamic_cast<Editor*>(editorBase.get())));
            auto about = AboutWindow::createContent(look, Editor::latencyNote(p->getLatencySamples(), p->getSampleRate()));
            int outside = 0, children = 0, smallLicence = 0;
            for (auto* c : about->getChildren())
            {
                ++children;
                outside += about->getLocalBounds().contains(c->getBounds()) ? 0 : 1;
                if (dynamic_cast<juce::TextEditor*>(c) != nullptr)
                    smallLicence += c->getHeight() < 120 ? 1 : 0;
            }
            check(children >= 5 && outside == 0 && smallLicence == 0, "the About window's parts fit inside it with the quick start added");
            about.reset();
        }

        // 10. Knobs the current settings do not use are dimmed and still turn; each "not used" case is
        //     an exact null in the audio, and the knob is lit again when it is.
        {
            const double sr = 48000.0;
            auto p = makeProcessor(sr, 512);
            std::unique_ptr<juce::AudioProcessorEditor> editorBase(p->createEditor());
            auto& editor = *dynamic_cast<Editor*>(editorBase.get());
            auto& strip = RoarschachEditorInspector::strip(editor);

            const auto knobFor = [&](const char* id) -> juce::Slider*
            {
                std::vector<juce::Component*> tiles { strip.getInTile(), strip.getOutputTile() };
                for (int m = 0; m < Params::numModules; ++m)
                    tiles.push_back(strip.getModuleTile(m));
                for (auto* tile : tiles)
                    for (const auto& [slider, knobId] : strip.getKnobs(tile))
                        if (std::string_view(knobId) == id)
                            return slider;
                return nullptr;
            };
            const auto alphaOf = [&](const char* id)
            {
                strip.refresh();
                auto* knob = knobFor(id);
                return knob != nullptr && knob->isEnabled() ? knob->getAlpha() : -1.0f;
            };
            const auto is = [](float alpha, float want) { return std::abs(alpha - want) < 0.01f; };
            const float lit = 1.0f, weak = 0.7f, unused = 0.45f;

            // Every module On, so only the knob's own state dims it.
            const auto allOn = [](RoarschachAudioProcessor& processor)
            {
                for (const auto& m : Params::modules)
                    setParam(processor, m.onId, 1.0f);
            };
            allOn(*p);

            struct Case { const char* knob; const char* condition; float deadValue, liveValue; const char* what; };
            const Case cases[] {
                { Params::outClipShape, Params::outClip, 0.0f, 6.0f, "OUTPUT Shape at Clip 0" },
                { Params::growlRate, Params::growl, 0.0f, 25.0f, "GROWL Rate at Depth 0" },
                { Params::growlRate, Params::growlShape, static_cast<float>(Params::GrowlShape::throat), 0.0f, "GROWL Rate with Throat" },
                { Params::shimmerRate, Params::shimmer, 0.0f, 18.0f, "SHIMMER Rate at Amount 0" },
                { Params::swirlRate, Params::swirl, 0.0f, 21.0f, "SWIRL Rate at Amount 0" },
                { Params::ensembleWidth, Params::ensemble, 0.0f, 10.0f, "ENSEMBLE Width at Amount 0" },
                { Params::ringFreq, Params::ring, 0.0f, 40.0f, "RING Freq at Amount 0" },
                { Params::subDrive, Params::sub, 0.0f, 49.0f, "SUB Drive at Level 0" },
                { Params::overtoneFreq, Params::overtone, 0.0f, 4.0f, "OVERTONE Freq at Amount 0" },
                { Params::chamberResonance, Params::chamber, 0.0f, 50.0f, "CHAMBER Reson at Amount 0" },
                { Params::chamberDrive, Params::chamber, 0.0f, 80.0f, "CHAMBER Edge at Amount 0" },
            };

            Stereo voice;
            voice.l = makeVoice(sr, g_quick ? 0.8 : 1.5);
            voice.r = voice.l;
            // (a new processor per render: nothing is left fading from the settings before)
            const auto renderAt = [&](const char* condition, float conditionValue, const char* id, float normalised)
            {
                auto q = makeProcessor(sr, 512);
                allOn(*q);
                setParam(*q, condition, conditionValue);
                q->getValueTreeState().getParameter(id)->setValueNotifyingHost(normalised);
                restart(*q, sr, 512);
                return render(*q, voice, 512);
            };
            const auto differ = [](const Stereo& a, const Stereo& b)
            {
                double worst = 0.0;
                for (size_t i = 0; i < a.l.size(); ++i)
                    worst = std::max({ worst, static_cast<double>(std::abs(a.l[i] - b.l[i])), static_cast<double>(std::abs(a.r[i] - b.r[i])) });
                return worst;
            };

            int wrongDim = 0, notNull = 0, notLive = 0;
            for (const auto& c : cases)
            {
                setParam(*p, c.condition, c.deadValue);
                const bool dimmed = is(alphaOf(c.knob), unused);
                const double deadDiff = differ(renderAt(c.condition, c.deadValue, c.knob, 0.0f), renderAt(c.condition, c.deadValue, c.knob, 1.0f));
                setParam(*p, c.condition, c.liveValue);
                const bool relit = is(alphaOf(c.knob), lit);
                const double liveDiff = differ(renderAt(c.condition, c.liveValue, c.knob, 0.0f), renderAt(c.condition, c.liveValue, c.knob, 1.0f));
                wrongDim += dimmed && relit ? 0 : 1;
                notNull += deadDiff == 0.0 ? 0 : 1;
                notLive += liveDiff > 1.0e-4 ? 0 : 1;
                if (!(dimmed && relit) || deadDiff != 0.0 || liveDiff <= 1.0e-4)
                    std::printf("    %s: dimmed %d, lit again %d, difference %.2e dimmed and %.2e lit\n", c.what, dimmed, relit, deadDiff, liveDiff);
            }
            check(wrongDim == 0, juce::String(wrongDim) + " unused knobs not dimmed, or not lit again when used");
            check(notNull == 0, juce::String(notNull) + " dimmed knobs still change the sound");
            check(notLive == 0, juce::String(notLive) + " lit knobs do not change the sound");

            // The two half-dimmed knobs do a little, not nothing: CHAMBER Edge under 60 % Amount, and
            // OUTPUT Attack while Auto is on.
            setParam(*p, Params::chamber, 50.0f);
            check(is(alphaOf(Params::chamberDrive), weak), "CHAMBER Edge is half-dimmed at Amount 50");
            setParam(*p, Params::chamber, 60.0f);
            check(is(alphaOf(Params::chamberDrive), lit), "CHAMBER Edge is lit from Amount 60");
            setParam(*p, Params::outAuto, 1.0f);
            check(is(alphaOf(Params::outAttack), weak), "OUTPUT Attack is half-dimmed with Auto on");
            setParam(*p, Params::outAuto, 0.0f);
            check(is(alphaOf(Params::outAttack), lit), "OUTPUT Attack is lit with Auto off");
            setParam(*p, Params::outAuto, 1.0f);

            // CHAMBER Width stays lit at Amount 0: it still decides where the chain turns stereo.
            setParam(*p, Params::chamber, 0.0f);
            check(is(alphaOf(Params::chamberWidth), lit), "CHAMBER Width stays lit at Amount 0");
            setParam(*p, Params::chamber, 50.0f);

            // A dimmed knob on an Off tile is no dimmer than the tile, and every knob still takes a value.
            setParam(*p, Params::growl, 0.0f);
            setParam(*p, Params::growlOn, 0.0f);
            check(is(alphaOf(Params::growlRate), unused) && is(alphaOf(Params::growl), unused), "an Off tile dims all its knobs alike");
            setParam(*p, Params::growlOn, 1.0f);
            auto* rate = knobFor(Params::growlRate);
            rate->setValue(40.0, juce::sendNotificationSync);
            check(std::abs(getParam(*p, Params::growlRate) - 40.0f) < 0.5f, "a dimmed knob still turns");

            // Dimming a knob and its caption together.
            int captionsApart = 0;
            for (auto* c : strip.getOutputTile()->getChildren())
                if (auto* label = dynamic_cast<juce::Label*>(c); label != nullptr && label->getText() == "SHAPE")
                    captionsApart += is(label->getAlpha(), alphaOf(Params::outClipShape)) ? 0 : 1;
            check(captionsApart == 0, "a dimmed knob's caption dims with it");
        }

        writeUiShots();
    }

    //==========================================================================
    // Snapshot harness: the editor on screen (so the creature's OpenGL view renders), fed with the
    // reference voice, then pictures of the whole editor, the strip, menus, a tooltip and a drag.

    void runLoop(int ms)
    {
        const auto end = juce::Time::getMillisecondCounterHiRes() + ms;
        while (juce::Time::getMillisecondCounterHiRes() < end)
        {
            juce::Timer::callPendingTimersSynchronously();
           #if JUCE_MAC
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.004, true);
           #else
            juce::Thread::sleep(4);
           #endif
        }
    }

    bool savePng(const juce::Image& image, const juce::File& file)
    {
        file.deleteFile();
        juce::FileOutputStream stream(file);
        const bool ok = stream.openedOk() && juce::PNGImageFormat().writeImageToStream(image, stream);
        std::printf("%s %s (%dx%d)\n", ok ? "wrote" : "could not write", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
        return ok;
    }

    void drawPointer(juce::Graphics& g, juce::Point<float> tip)
    {
        juce::Path arrow;
        arrow.startNewSubPath(0, 0);
        arrow.lineTo(0, 17); arrow.lineTo(4.5f, 13); arrow.lineTo(7.5f, 19.5f); arrow.lineTo(10, 18.5f);
        arrow.lineTo(7, 12); arrow.lineTo(12.5f, 12); arrow.closeSubPath();
        arrow.applyTransform(juce::AffineTransform::translation(tip));
        g.setColour(juce::Colours::black);
        g.strokePath(arrow, juce::PathStrokeType(2.0f));
        g.setColour(juce::Colours::white);
        g.fillPath(arrow);
    }

    int runSnapshots(const juce::File& dir)
    {
        dir.createDirectory();
        const double sr = 48000.0;
        const int block = 512;
        auto p = makeProcessor(sr, block);
        p->setViewValue("viewEditorScale", 1.0);
        // The pictures show the horned form (the middle of the stage spine), whatever the seed's growth.
        p->withCreatureForTests([](roar::CreatureEvolution& e) { e.setPinnedStageForTests(0.4f); });
        const auto voice = dual(makeVoice(sr, 4.0, 98.0));
        int position = 0;
        juce::AudioBuffer<float> buffer(2, block);
        juce::MidiBuffer midi;

        // Plays the voice in real time while the editor's timers and the GL thread run.
        const auto play = [&](double seconds)
        {
            const int blocks = static_cast<int>(seconds * sr / block);
            for (int b = 0; b < blocks; ++b)
            {
                if (position + block > voice.size())
                    position = 0;
                buffer.copyFrom(0, 0, voice.l.data() + position, block);
                buffer.copyFrom(1, 0, voice.r.data() + position, block);
                position += block;
                p->processBlock(buffer, midi);
                runLoop(static_cast<int>(1000.0 * block / sr));
            }
        };

        std::unique_ptr<juce::AudioProcessorEditor> editorBase(p->createEditor());
        auto& editor = *dynamic_cast<RoarschachAudioProcessorEditor*>(editorBase.get());
        auto& strip = RoarschachEditorInspector::strip(editor);
        auto& monster = RoarschachEditorInspector::monster(editor);

        // A stage window holds the editor (and the menus and tooltips, so they are in the pictures).
        juce::Component stage;
        stage.setOpaque(true);
        const int baseW = RoarschachAudioProcessorEditor::c_baseWidth, baseH = RoarschachAudioProcessorEditor::c_baseHeight;
        stage.setSize(baseW, baseH + 100);
        stage.addAndMakeVisible(editor);
        editor.setBounds(0, 0, baseW, baseH);
        stage.addToDesktop(juce::ComponentPeer::windowHasDropShadow);
        stage.setTopLeftPosition(40, 40);
        stage.setVisible(true);
        strip.setMenuParentForSnapshots(&stage);
        juce::Process::makeForegroundProcess();
        runLoop(600);

        // The editor with the creature's GL frame put in where the GL view covers it.
        const auto editorPicture = [&](float scale)
        {
            monster.requestCapture();
            juce::Image frame;
            for (int k = 0; k < 100 && !frame.isValid(); ++k)
            {
                play(0.02);
                frame = monster.takeCapture();
            }
            auto image = stage.createComponentSnapshot(editor.getBounds(), true, scale);
            if (frame.isValid())
            {
                juce::Graphics g(image);
                const auto area = stage.getLocalArea(&monster, monster.getLocalBounds()).toFloat() * scale;
                g.drawImage(frame, area);
                // The overlay (readouts, outline) is painted over the GL frame by the component.
                g.addTransform(juce::AffineTransform::scale(area.getWidth() / static_cast<float>(monster.getWidth())).translated(area.getX(), area.getY()));
                monster.paintEntireComponent(g, false);
            }
            else
                std::printf("  (no GL frame: the creature view is blank in this picture)\n");
            return image;
        };

        const auto stripArea = [&] { return stage.getLocalArea(&strip, strip.getLocalBounds()).expanded(8); };

        int failures = 0;
        const auto save = [&](const juce::Image& image, const juce::String& name) { failures += savePng(image, dir.getChildFile(name)) ? 0 : 1; };

        // 1. Init, playing.
        play(2.5);
        save(editorPicture(1.0f), "01-editor.png");
        {
            const auto picture = editorPicture(2.0f);
            save(picture, "01-editor@2x.png");
            save(picture.getClippedImage(stage.getLocalArea(&monster, monster.getLocalBounds()).withSizeKeepingCentre(560, 400) * 2), "01-face@2x.png");
        }
        save(stage.createComponentSnapshot(stripArea(), true, 2.0f), "02-strip@2x.png");
        {
            const auto a = stripArea();
            save(stage.createComponentSnapshot(a.withWidth(a.getWidth() / 2 + 40), true, 2.0f), "02a-strip-left@2x.png");
            save(stage.createComponentSnapshot(a.withTrimmedLeft(a.getWidth() / 2 - 40), true, 2.0f), "02b-strip-right@2x.png");
        }

        // 2. Presets that light the other parts of the creature.
        for (const char* name : { "Cyber Commander", "Ribcage", "Smashed", "Pulse Choir", "Kaiju" })
        {
            const int index = presetIndex(name);
            if (index < 0)
                continue;
            p->setCurrentProgram(index);
            play(2.0);
            strip.finishAnimations();
            const auto picture = editorPicture(2.0f);
            {
                const auto& f = p->getMonsterFeatures();
                std::printf("  %s: limiter GR %.1f dB, clip %.1f dB, overtone %.2f, ring %.2f\n", name, f.limiterGrDb.load(), f.clipDb.load(),
                            f.overtoneLevel.load(), f.ringAmount.load());
            }
            const auto slug = juce::String(name).replace(" ", "-").toLowerCase();
            save(picture.rescaled(picture.getWidth() / 2, picture.getHeight() / 2, juce::Graphics::highResamplingQuality), "03-preset-" + slug + ".png");
            // The face, close up.
            const auto face = stage.getLocalArea(&monster, monster.getLocalBounds()).withSizeKeepingCentre(560, 400) * 2;
            save(picture.getClippedImage(face), "03-face-" + slug + "@2x.png");
        }
        save(stage.createComponentSnapshot(stripArea(), true, 2.0f), "04-strip-last-preset@2x.png");
        // The preset bar with its die (Kaiju showing), and ROLL ALL in the slot after the last module.
        save(stage.createComponentSnapshot({ baseW - 500, 0, 500, 56 }, true, 2.0f), "header@2x.png");
        save(stage.createComponentSnapshot(stage.getLocalArea(&strip, strip.getSlotBounds(Params::numModules)), true, 2.0f), "panel-roll-all@2x.png");

        // The README's close-ups, one per tile: Kaiju (the last preset above) for most, RING and
        // OUTPUT from Cyber Commander, CHAMBER from Iron Tyrant. The values shown are printed.
        {
            const auto tilePicture = [&](juce::Component* tile, const juce::String& name)
            {
                if (tile == nullptr)
                    return;
                save(stage.createComponentSnapshot(stage.getLocalArea(tile, tile->getLocalBounds()), true, 2.0f), name);
                juce::String values;
                for (const auto& [knob, id] : strip.getKnobs(tile))
                    if (knob->isVisible())
                        values << "  " << id << "=" << p->getValueTreeState().getParameter(id)->getCurrentValueAsText();
                std::printf("  %s:%s\n", name.toRawUTF8(), values.toRawUTF8());
            };
            const auto choices = [&](std::initializer_list<const char*> ids)
            {
                juce::String values;
                for (const char* id : ids)
                    values << "  " << id << "=" << p->getValueTreeState().getParameter(id)->getCurrentValueAsText();
                std::printf("   %s\n", values.toRawUTF8());
            };
            tilePicture(strip.getInTile(), "panel-in@2x.png");
            for (int m = 0; m < Params::numModules; ++m)
                if (m != static_cast<int>(Params::ModuleId::ring) && m != static_cast<int>(Params::ModuleId::chamber))
                    tilePicture(strip.getModuleTile(m), juce::String("panel-") + juce::String(Params::modules[static_cast<size_t>(m)].uiName).toLowerCase().removeCharacters("- ") + "@2x.png");
            p->setCurrentProgram(presetIndex("Cyber Commander"));
            play(1.5);
            strip.finishAnimations();
            tilePicture(strip.getModuleTile(static_cast<int>(Params::ModuleId::ring)), "panel-ring@2x.png");
            choices({ Params::ringCarrier, Params::ringTrack, Params::ringMode, Params::ringBody });
            tilePicture(strip.getOutputTile(), "panel-out@2x.png");
            choices({ Params::outAuto });
            p->setCurrentProgram(presetIndex("Iron Tyrant"));
            play(1.5);
            strip.finishAnimations();
            tilePicture(strip.getModuleTile(static_cast<int>(Params::ModuleId::chamber)), "panel-chamber@2x.png");
            choices({ Params::chamberRoom, Params::chamberSpeaker, Params::chamberBoom });
        }
        p->setCurrentProgram(presetIndex("Cyber Commander"));
        play(0.5);
        strip.finishAnimations();

        // 3. Menus (opened inside the stage).
        const auto menuPicture = [&](juce::Component& target, const juce::String& name, int w, int h)
        {
            runLoop(30);   // (a menu closes itself when the process is not in front: keep this short)
            const auto t = stage.getLocalArea(&target, target.getLocalBounds());
            // Around the chip and the menu (which opens below or above it, wherever it fits).
            auto area = t;
            for (auto* c : stage.getChildren())
                if (c != &editor && c->isVisible())
                    area = area.getUnion(c->getBounds());
            area = area.expanded(24).withWidth(std::max(area.getWidth() + 48, w)).withHeight(std::max(area.getHeight() + 48, h))
                       .constrainedWithin(stage.getLocalBounds());
            auto image = stage.createComponentSnapshot(area, true, 2.0f);
            {
                juce::Graphics g(image);
                g.addTransform(juce::AffineTransform::scale(2.0f));
                drawPointer(g, (t.getCentre() - area.getPosition()).toFloat());
            }
            save(image, name);
            juce::PopupMenu::dismissAllActiveMenus();
            runLoop(200);
        };
        const int ring = static_cast<int>(Params::ModuleId::ring);
        strip.showMoveMenu(ring);
        menuPicture(*strip.getMoveChip(ring), "05-move-menu@2x.png", 420, 420);
        {
            // The Move After submenu on its own, from the same chip.
            const auto menu = strip.buildMoveMenu(ring);
            for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
                if (it.getItem().subMenu != nullptr)
                {
                    auto sub = *it.getItem().subMenu;
                    sub.setLookAndFeel(&RoarschachEditorInspector::lookAndFeel(editor));
                    sub.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(strip.getMoveChip(ring)).withParentComponent(&stage)
                                          .withStandardItemHeight(24));
                }
            menuPicture(*strip.getMoveChip(ring), "06-move-after-menu@2x.png", 420, 480);
        }
        strip.showModeMenu(ring);
        menuPicture(*strip.getModeChip(ring), "07-ring-mode-menu@2x.png", 420, 300);
        strip.showModeMenu(static_cast<int>(Params::ModuleId::clean));
        menuPicture(*strip.getModeChip(static_cast<int>(Params::ModuleId::clean)), "08-clean-mode-menu@2x.png", 360, 260);

        // 4. A tooltip over RING's second knob.
        {
            juce::TooltipWindow tip(&stage, 0);
            tip.setLookAndFeel(&RoarschachEditorInspector::lookAndFeel(editor));
            tip.setOpaque(false);
            auto* knob = strip.getKnobs(strip.getModuleTile(ring))[1].first;
            if (!knob->isVisible())
                knob = strip.getKnobs(strip.getModuleTile(ring))[2].first;
            const auto k = stage.getLocalArea(knob, knob->getLocalBounds());
            const auto at = k.getCentre().translated(6, -10);
            tip.displayTip(stage.localPointToGlobal(at), knob->getTooltip());   // (no run loop: its timer would hide it)
            auto area = k.getUnion(tip.getBounds()).expanded(40, 30).constrainedWithin(stage.getLocalBounds());
            auto image = stage.createComponentSnapshot(area, true, 2.0f);
            {
                juce::Graphics g(image);
                g.addTransform(juce::AffineTransform::scale(2.0f));
                drawPointer(g, (at - area.getPosition()).toFloat());
            }
            save(image, "09-tooltip@2x.png");
            tip.hideTip();
            tip.setLookAndFeel(nullptr);
        }

        // 5. Mid-drag: GROWL lifted from row 2 and carried into row 1, the others making room.
        {
            const int growl = static_cast<int>(Params::ModuleId::growl);
            auto& tile = *strip.getModuleTile(growl);
            const juce::Point<float> grab { 50.0f, 12.0f };
            const auto from = tile.getPosition().toFloat() + grab;
            const auto to = strip.getSlotBounds(2).getPosition().toFloat() + grab + juce::Point<float>(30.0f, 40.0f);
            std::vector<juce::Point<float>> path;
            for (int k = 1; k <= 16; ++k)
                path.push_back(from + (to - from) * (static_cast<float>(k) / 16.0f));
            tile.mouseDown(mouseEventFor(tile, grab, grab, false));
            for (const auto& pt : path)
            {
                tile.mouseDrag(mouseEventFor(tile, pt - tile.getPosition().toFloat(), from - tile.getPosition().toFloat(), true));
                play(0.016);
            }
            play(0.3);
            save(stage.createComponentSnapshot(stripArea(), true, 2.0f), "10-dragging@2x.png");
            tile.mouseUp(mouseEventFor(tile, to - tile.getPosition().toFloat(), from - tile.getPosition().toFloat(), true));
            play(0.4);
            save(stage.createComponentSnapshot(stripArea(), true, 2.0f), "11-after-drop@2x.png");
        }

        // 6. A small editor (the minimum scale) for legibility.
        {
            editor.setSize(800, 450);
            stage.setSize(800, 450);
            play(0.5);
            save(editorPicture(2.0f), "12-editor-min-scale@2x.png");
        }

        strip.setMenuParentForSnapshots(nullptr);
        stage.removeFromDesktop();
        stage.removeChildComponent(&editor);
        editorBase.reset();
        return failures == 0 ? 0 : 1;
    }

    //==========================================================================
    // CHAMBER: the crypto that protects its room data, the convolver, the rooms and the module.

    std::vector<std::uint8_t> hexBytes(const char* hex)
    {
        std::vector<std::uint8_t> out;
        int nibble = -1;
        for (const char* c = hex; *c != 0; ++c)
        {
            int v = -1;
            if (*c >= '0' && *c <= '9') v = *c - '0';
            else if (*c >= 'a' && *c <= 'f') v = *c - 'a' + 10;
            else if (*c >= 'A' && *c <= 'F') v = *c - 'A' + 10;
            if (v < 0)
                continue;
            if (nibble < 0)
                nibble = v;
            else
            {
                out.push_back(static_cast<std::uint8_t>(nibble * 16 + v));
                nibble = -1;
            }
        }
        return out;
    }

    template <typename A>
    bool sameBytes(const A& a, const std::vector<std::uint8_t>& b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
    }

    /** Runs a ChamberModule over a stereo signal in host blocks of `block` (or a block pattern). */
    Stereo runChamber(roar::ChamberModule& m, const Stereo& in, const std::vector<int>& blocks, bool stereo,
                      const std::function<void(int)>& onBlock = {})
    {
        Stereo out = in;
        size_t b = 0;
        for (int start = 0; start < in.size();)
        {
            if (onBlock)
                onBlock(start);
            const int n = std::min(blocks[b++ % blocks.size()], in.size() - start);
            m.process(out.l.data() + start, out.r.data() + start, n, stereo, roar::VoiceFrameView {});
            start += n;
        }
        return out;
    }

    double rmsOf(const Signal& s, size_t from = 0, size_t to = std::numeric_limits<size_t>::max())
    {
        to = std::min(to, s.size());
        double acc = 0.0;
        for (size_t i = from; i < to; ++i)
            acc += static_cast<double>(s[i]) * s[i];
        return to > from ? std::sqrt(acc / static_cast<double>(to - from)) : 0.0;
    }

    void testChamber()
    {
        std::printf("[chamber] crypto test vectors, sealed rooms, convolver, modelled rooms, module, chain\n");
        using namespace roar;

        // --- RFC 8439 test vectors (ChaCha20 block 2.3.2, encryption 2.4.2, Poly1305 2.5.2, AEAD 2.8.2).
        {
            crypto::Key key {};
            for (size_t i = 0; i < key.size(); ++i)
                key[i] = static_cast<std::uint8_t>(i);
            crypto::Nonce nonce {};
            const auto n = hexBytes("000000090000004a00000000");
            std::copy(n.begin(), n.end(), nonce.begin());
            std::array<std::uint8_t, 64> block {};
            crypto::chacha20Block(key, 1, nonce, block.data());
            check(sameBytes(block, hexBytes("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
                                            "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e")),
                  "ChaCha20 block function (RFC 8439 2.3.2)");

            const char* sunscreen = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
            std::vector<std::uint8_t> text(sunscreen, sunscreen + std::strlen(sunscreen));
            const auto n2 = hexBytes("000000000000004a00000000");
            std::copy(n2.begin(), n2.end(), nonce.begin());
            auto enc = text;
            crypto::chacha20Xor(key, 1, nonce, enc.data(), enc.size());
            check(sameBytes(std::vector<std::uint8_t>(enc.begin(), enc.begin() + 16), hexBytes("6e2e359a2568f98041ba0728dd0d6981"))
                      && sameBytes(std::vector<std::uint8_t>(enc.end() - 2, enc.end()), hexBytes("874d")),
                  "ChaCha20 encryption (RFC 8439 2.4.2)");
            crypto::chacha20Xor(key, 1, nonce, enc.data(), enc.size());
            check(enc == text, "ChaCha20 decrypts what it encrypted");

            const auto polyKey = hexBytes("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
            const char* msg = "Cryptographic Forum Research Group";
            const auto tag = crypto::poly1305(polyKey.data(), reinterpret_cast<const std::uint8_t*>(msg), std::strlen(msg));
            check(sameBytes(tag, hexBytes("a8061dc1305136c6c22b8baf0c0127a9")), "Poly1305 (RFC 8439 2.5.2)");
            // The same, fed in uneven pieces.
            crypto::Poly1305 pieces(polyKey.data());
            for (size_t at = 0, step = 1; at < std::strlen(msg); at += step, step = step % 7 + 3)
                pieces.update(reinterpret_cast<const std::uint8_t*>(msg) + at, std::min(step, std::strlen(msg) - at));
            check(sameBytes(pieces.finish(), hexBytes("a8061dc1305136c6c22b8baf0c0127a9")), "Poly1305 streamed in pieces");

            crypto::Key aeadKey {};
            for (size_t i = 0; i < aeadKey.size(); ++i)
                aeadKey[i] = static_cast<std::uint8_t>(0x80 + i);
            const auto n3 = hexBytes("070000004041424344454647");
            std::copy(n3.begin(), n3.end(), nonce.begin());
            const auto aad = hexBytes("50515253c0c1c2c3c4c5c6c7");
            auto sealed = text;
            const auto aeadTag = crypto::aeadEncrypt(aeadKey, nonce, aad.data(), aad.size(), sealed.data(), sealed.size());
            check(sameBytes(sealed, hexBytes("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
                                             "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
                                             "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
                                             "3ff4def08e4b7a9de576d26586cec64b6116")),
                  "AEAD ciphertext (RFC 8439 2.8.2)");
            check(sameBytes(aeadTag, hexBytes("1ae10b594f09e26a7e902ecbd0600691")), "AEAD tag (RFC 8439 2.8.2)");
            auto opened = sealed;
            check(crypto::aeadDecrypt(aeadKey, nonce, aad.data(), aad.size(), opened.data(), opened.size(), aeadTag) && opened == text,
                  "AEAD opens its own message");
            opened = sealed;
            opened[40] ^= 1;
            const auto before = opened;
            check(!crypto::aeadDecrypt(aeadKey, nonce, aad.data(), aad.size(), opened.data(), opened.size(), aeadTag) && opened == before,
                  "AEAD rejects a changed message and leaves it untouched");
        }

        // --- The sealed room container: round trip with a test key; any change or another key is rejected.
        {
            const auto rooms = ChamberBank::modelledRooms();
            crypto::Key key {};
            for (size_t i = 0; i < key.size(); ++i)
                key[i] = static_cast<std::uint8_t>(0xa5 ^ (i * 29));
            crypto::Nonce nonce {};
            for (size_t i = 0; i < nonce.size(); ++i)
                nonce[i] = static_cast<std::uint8_t>(i * 7 + 1);
            const auto plain = irpack::serialise(rooms);
            const auto blob = irpack::seal(plain, key, nonce);
            size_t same = 0;
            for (size_t i = 0; i < plain.size(); ++i)
                same += blob[irpack::c_headerSize + i] == plain[i] ? 1 : 0;
            check(blob.size() == plain.size() + irpack::c_headerSize + 16 && same < plain.size() / 100, "the sealed rooms are not stored in the clear");

            std::vector<std::uint8_t> payload;
            check(irpack::open(blob.data(), blob.size(), key, payload) && payload == plain, "sealed rooms open with their key");
            std::vector<irpack::Ir> back;
            const bool parsed = irpack::parse(payload.data(), payload.size(), back);
            check(parsed && back.size() == rooms.size(), "the opened rooms parse");
            double worst = 0.0;
            for (size_t r = 0; parsed && r < std::min(back.size(), rooms.size()); ++r)
            {
                float peak = 0.0f;
                for (float x : rooms[r].left) peak = std::max(peak, std::abs(x));
                for (float x : rooms[r].right) peak = std::max(peak, std::abs(x));
                for (size_t i = 0; i < rooms[r].left.size(); ++i)
                    worst = std::max(worst, std::max(std::abs(static_cast<double>(back[r].left[i]) - rooms[r].left[i]),
                                                     std::abs(static_cast<double>(back[r].right[i]) - rooms[r].right[i])) / peak);
            }
            check(worst <= 0.6 / 32767.0, "rooms survive the 16-bit container (worst error " + juce::String(worst * 32767.0, 2) + " LSB)");

            auto other = key;
            other[5] ^= 0x10;
            check(!irpack::open(blob.data(), blob.size(), other, payload) && payload.empty(), "another key is rejected");
            for (const size_t at : { size_t { 2 }, size_t { 10 }, size_t { 21 }, irpack::c_headerSize + 100, blob.size() - 3 })
            {
                auto damaged = blob;
                damaged[at] ^= 0x01;
                check(!irpack::open(damaged.data(), damaged.size(), key, payload), "a changed byte at " + juce::String(static_cast<int>(at)) + " is rejected");
            }
            check(!irpack::open(blob.data(), blob.size() - 1, key, payload), "a truncated blob is rejected");
            crypto::Key parsedKey {};
            check(irpack::parseHexKey("000102030405060708090a0b0c0d0e0f101112131415161718191A1B1C1D1E1F", parsedKey) && parsedKey[31] == 0x1f
                      && !irpack::parseHexKey("0011", parsedKey) && !irpack::parseHexKey(std::string(64, 'g'), parsedKey),
                  "hex keys");
        }

        // --- The convolver: exact against direct convolution, at every partition boundary, at any block size.
        {
            std::mt19937 rng(11);
            std::uniform_real_distribution<float> u(-1.0f, 1.0f);
            std::uniform_int_distribution<int> blockDist(1, 300);
            const std::vector<double> rates = g_quick ? std::vector<double> { 48000.0 } : std::vector<double> { 48000.0, 96000.0, 192000.0 };
            for (double fs : rates)
                for (int len : { 1, 63, 64, 65, 511, 512, 513, 1023, 8192, 36000 })
                {
                    std::vector<float> h0(static_cast<size_t>(len)), h1(static_cast<size_t>(len));
                    for (int i = 0; i < len; ++i)
                    {
                        const float env = std::exp(-3.0f * static_cast<float>(i) / static_cast<float>(len));
                        h0[static_cast<size_t>(i)] = u(rng) * env;
                        h1[static_cast<size_t>(i)] = u(rng) * env;
                    }
                    const auto layout = ConvolverLayout::forRate(fs, len);
                    ConvolverIr ir;
                    ir.build(layout, { h0.data(), h1.data() }, len);
                    const int n = std::min(len, 9000) + 3000;
                    std::vector<float> x(static_cast<size_t>(n));
                    for (auto& v : x)
                        v = u(rng);

                    ZeroLatencyConvolver c;
                    c.prepare(layout);
                    c.bind(0, &ir, 2);
                    std::vector<float> y0(static_cast<size_t>(n)), y1(static_cast<size_t>(n));
                    for (int at = 0; at < n;)
                    {
                        int want = std::min(blockDist(rng), n - at);
                        while (want > 0)
                        {
                            const int m = std::min(want, c.samplesToGrid());
                            float* const outs[2][2] { { y0.data() + at, y1.data() + at }, { nullptr, nullptr } };
                            c.process(x.data() + at, m, outs);
                            at += m;
                            want -= m;
                        }
                    }

                    double err = 0.0, ref = 0.0;
                    for (int o = 0; o < 2; ++o)
                    {
                        const auto& h = o == 0 ? h0 : h1;
                        const auto& y = o == 0 ? y0 : y1;
                        for (int t = 0; t < n; ++t)
                        {
                            double acc = 0.0;
                            for (int k = 0; k < len && k <= t; ++k)
                                acc += static_cast<double>(h[static_cast<size_t>(k)]) * x[static_cast<size_t>(t - k)];
                            const double e = y[static_cast<size_t>(t)] - acc;
                            err += e * e;
                            ref += acc * acc;
                        }
                    }
                    const double db = 10.0 * std::log10(std::max(err, 1.0e-300) / std::max(ref, 1.0e-300));
                    check(db < -100.0, "convolver at " + juce::String(fs / 1000.0, 1) + " kHz, " + juce::String(len) + " taps: error "
                                           + juce::String(db, 1) + " dB re direct convolution (<= -100)");
                }
        }

        const double sr = 48000.0;
        const int seconds = g_quick ? 1 : 2;
        const auto voice = makeVoice(sr, seconds);
        const Stereo voiceStereo = dual(voice);
        const auto prepared = [sr](const ChamberSettings& s)
        {
            auto m = std::make_unique<ChamberModule>();
            m->setParams(s);
            m->prepare(sr, 512);
            return m;
        };
        ChamberSettings on;
        on.on = true;

        // --- The modelled rooms (what a build without the bundled rooms uses): ten rooms close to
        //     the measured decay of the captures they stand in for, and all at the same level.
        {
            ChamberBank::setForceModelled(false);
            std::printf("  this build %s\n", ChamberBank::hasBundledRooms() ? "carries the bundled rooms (encrypted)" : "has no bundled rooms: modelled rooms");
           #if ROARSCHACH_EXPECT_BUNDLED_ROOMS
            check(ChamberBank::hasBundledRooms(), "the embedded rooms decrypt and parse (a build with the captures must not fall back to the modelled rooms)");
           #endif
            const auto modelled = ChamberBank::build(sr, ChamberBank::Source::modelled);
            check(modelled->getSource() == ChamberBank::Source::modelled, "the modelled bank");
            // The captures' RT60 (Party Wall and Tower Block: after the packer steepens their 2-3 s ring to fit 0.75 s).
            const std::array<double, ChamberBank::numRooms> measured { 0.42, 0.71, 0.61, 0.55, 0.37, 0.58, 0.54, 0.20, 0.78, 0.79 };
            const auto bundled = ChamberBank::hasBundledRooms() ? ChamberBank::build(sr, ChamberBank::Source::bundled) : nullptr;
            for (int r = 0; r < ChamberBank::numRooms; ++r)
            {
                const double rt = modelled->room(r).rt60;
                std::printf("  %-17s modelled RT60 %.2f s%s", Params::chamberRoomNames[static_cast<size_t>(r)], rt, bundled != nullptr ? "" : "\n");
                check(rt > 0.55 * measured[static_cast<size_t>(r)] && rt < 1.6 * measured[static_cast<size_t>(r)],
                      juce::String(Params::chamberRoomNames[static_cast<size_t>(r)]) + " modelled RT60 near the capture's");
                if (bundled != nullptr)
                {
                    const double brt = bundled->room(r).rt60;
                    std::printf(", bundled %.2f s (capture %.2f s), %.1f ms\n", brt, measured[static_cast<size_t>(r)],
                                1000.0 * bundled->room(r).ir.getLength() / sr);
                    check(bundled->room(r).ir.getLength() <= static_cast<int>(irpack::c_maxSeconds * sr) + 1, "bundled rooms end by 0.75 s");
                }
            }

            // The recorded rooms start from (near) silence and keep their high-pass: a room that
            // starts mid-waveform (an onset trim that cut a slow arrival) is a step at sample 0,
            // which lets back the low end the packer's high-pass removed.
            if (bundled != nullptr)
            {
                const auto rooms = ChamberBank::bundledRooms();
                for (size_t r = 0; r < rooms.size(); ++r)
                {
                    const auto& ir = rooms[r];
                    double peak = 0.0, dc = 0.0, re1k = 0.0, im1k = 0.0;
                    for (size_t i = 0; i < ir.left.size(); ++i)
                    {
                        const double m = 0.5 * (ir.left[i] + ir.right[i]);
                        peak = std::max(peak, std::abs(m));
                        dc += m;
                        const double ph = 2.0 * juce::MathConstants<double>::pi * 1000.0 * static_cast<double>(i) / ir.rate;
                        re1k += m * std::cos(ph);
                        im1k -= m * std::sin(ph);
                    }
                    const double m0 = ir.left.empty() ? 0.0 : 0.5 * (ir.left[0] + ir.right[0]);
                    const double startDb = toDb(std::abs(m0) / std::max(peak, 1e-12));
                    const double dcDb = toDb(std::abs(dc) / std::max(std::hypot(re1k, im1k), 1e-12));
                    std::printf("  %-17s bundled: first sample %6.1f dB re peak, DC %6.1f dB re 1 kHz\n",
                                Params::chamberRoomNames[r], startDb, dcDb);
                    // (The first eight are designed responses whose first sample and low end are
                    // part of their sound; only the recordings, high-passed, are held to this.)
                    if (irpack::c_captures[r].keepArrival)
                    {
                        check(startDb < -30.0, juce::String(Params::chamberRoomNames[r]) + " starts from near silence (no step at sample 0)");
                        check(dcDb < -40.0, juce::String(Params::chamberRoomNames[r]) + " keeps its high-pass (no DC let back by the trim)");
                    }
                }
            }

            // Every room at the same level: loudness-matched on a voice-band pink spectrum (what the
            // packer and the modelled rooms match on), so switching rooms keeps the level. A single
            // sung pitch lands on or between a room's modes, so it varies more (printed).
            auto pinkVoiceBand = makePink(sr, seconds, 0.3f);
            for (const auto& c : { Biquad::highpass(sr, 150.0, Biquad::c_bw4Q1), Biquad::highpass(sr, 150.0, Biquad::c_bw4Q2),
                                   Biquad::lowpass(sr, 5000.0, Biquad::c_bw4Q1), Biquad::lowpass(sr, 5000.0, Biquad::c_bw4Q2) })
            {
                Biquad f;
                f.setCoeffs(c);
                f.process(pinkVoiceBand.data(), static_cast<int>(pinkVoiceBand.size()));
            }
            const auto pink = dual(pinkVoiceBand);
            for (int signal = 0; signal < 2; ++signal)
            {
                std::vector<double> levels;
                for (int r = 0; r < ChamberBank::numRooms; ++r)
                {
                    auto s = on;
                    s.amount = 1.0f;
                    s.edge = 0.0f;
                    s.width = 0.0f;
                    s.room = r;
                    auto m = prepared(s);
                    const auto& x = signal == 0 ? pink : voiceStereo;
                    auto out = runChamber(*m, x, { 512 }, false);
                    // The wet alone (the dry is the input times its gain, exactly).
                    double gd = 1.0, gw = 0.0;
                    ChamberModule::blendGains(1.0, gd, gw);
                    for (size_t i = 0; i < out.l.size(); ++i)
                        out.l[i] = static_cast<float>((out.l[i] - gd * x.l[i]) / gw);
                    levels.push_back(toDb(rmsOf(out.l, out.l.size() / 4) / rmsOf(x.l, x.l.size() / 4)));
                }
                const auto [lo, hi] = std::minmax_element(levels.begin(), levels.end());
                std::printf("  room levels (Chamber 100 %%, %s): %.1f .. %.1f dB re the input\n", signal == 0 ? "pink, 150 Hz - 5 kHz" : "voice at 110 Hz", *lo, *hi);
                if (signal == 0)
                    check(*hi - *lo < 2.0, "the rooms are loudness-matched (spread " + juce::String(*hi - *lo, 1) + " dB, < 2)");
            }

            // The wet alone at Chamber 100 % for a setting.
            const auto wetOf = [&](const ChamberSettings& s, const Stereo& x)
            {
                auto m = prepared(s);
                auto out = runChamber(*m, x, { 512 }, false);
                double gd = 1.0, gw = 0.0;
                ChamberModule::blendGains(1.0, gd, gw);
                for (size_t i = 0; i < out.l.size(); ++i)
                    out.l[i] = static_cast<float>((out.l[i] - gd * x.l[i]) / gw);
                return out.l;
            };

            // Each speaker has its own response (audit: their filters once kept the identity
            // coefficients, so all three sounded alike): the Small box keeps the lows the Tiny
            // speaker and the horn cut.
            {
                std::array<double, 3> lowShare {};
                for (int t = 0; t < 3; ++t)
                {
                    auto s = on;
                    s.amount = 1.0f;
                    s.edge = 0.0f;
                    s.width = 0.0f;
                    s.speaker = t;
                    auto wet = wetOf(s, pink);
                    auto low = wet;
                    for (const auto& c : { Biquad::lowpass(sr, 220.0, Biquad::c_bw4Q1), Biquad::lowpass(sr, 220.0, Biquad::c_bw4Q2) })
                    {
                        Biquad f;
                        f.setCoeffs(c);
                        f.process(low.data(), static_cast<int>(low.size()));
                    }
                    lowShare[static_cast<size_t>(t)] = toDb(rmsOf(low, low.size() / 4) / rmsOf(wet, wet.size() / 4));
                }
                std::printf("  energy under 220 Hz: Tiny %.1f, Small %.1f, Horn %.1f dB\n", lowShare[0], lowShare[1], lowShare[2]);
                check(lowShare[1] - lowShare[0] > 6.0 && lowShare[1] - lowShare[2] > 6.0, "each speaker has its own response");
            }

            // Resonance holds the wet's level in every room (audit: a room that swells in late once
            // jumped 9 dB at 100 %), and a room's longest decay still fits its IR.
            for (int r = 0; r < ChamberBank::numRooms; ++r)
            {
                const auto bank = ChamberBank::get(sr);
                const auto& room = bank->room(r);
                check(room.maxDecayScale >= 1.0 && room.maxDecayScale <= ChamberBank::maxResonanceScale + 1.0e-9
                          && room.decayStart >= 0.0 && room.decayStart < 0.25,
                      juce::String(Params::chamberRoomNames[static_cast<size_t>(r)]) + ": decay start and longest decay in range");
                std::vector<double> levels;
                for (const float u : { 0.0f, 0.4f, 1.0f })
                {
                    auto s = on;
                    s.amount = 1.0f;
                    s.edge = 0.0f;
                    s.width = 0.0f;
                    s.room = r;
                    s.resonance = u;
                    const auto wet = wetOf(s, pink);
                    levels.push_back(toDb(rmsOf(wet, wet.size() / 4)));
                }
                const auto [lo, hi] = std::minmax_element(levels.begin(), levels.end());
                check(*hi - *lo < 1.5, juce::String(Params::chamberRoomNames[static_cast<size_t>(r)]) + ": Resonance holds the level (spread "
                                           + juce::String(*hi - *lo, 1) + " dB, < 1.5)");
                std::printf("  %-17s decay starts %3.0f ms, longest x%.2f, Resonance 0/40/100 %%: %+.1f / %+.1f / %+.1f dB\n",
                            Params::chamberRoomNames[static_cast<size_t>(r)], 1000.0 * room.decayStart, room.maxDecayScale,
                            levels[0] - levels[1], 0.0, levels[2] - levels[1]);
            }
            // Peaks (audit: Party Wall and Tower Block are recordings of a loud impulsive sound, not
            // designed impulse responses, with a low-mid body 6-9 dB over the other rooms'): neither
            // may reach higher than the loudest of the first eight, on a click (the wet's transient
            // peak) or on a slow 50 Hz - 2 kHz sweep (a sustained sung pitch on every mode), at
            // Chamber 100 %, Resonance 100 %, each speaker; and on a dense, low, shouted vowel at
            // Resonance 0 and 100 % (the clip audit: Party Wall's closest case is Resonance 0 %
            // with the Tiny speaker on a 0 dBFS shout). A pitched voice's harmonics land on or
            // between a room's modes, so its peak varies with the pitch by more than the click's
            // or the sweep's (as the room levels at 110 Hz above): it is allowed 1.5 dB, not 0.5.
            // The sweep is allowed 1 dB: the full run's slower sweep (8 s) holds each mode long
            // enough for the longest rooms to build up almost fully (Party Wall, the longest ring,
            // reads +0.6 dB over the first eight there and -2 dB on the quick 4 s sweep).
            // Whether a peak clips is checked through the whole plug-in in testChamberClip.
            {
                Signal click(static_cast<size_t>(0.8 * sr), 0.0f);
                click[1000] = 0.5f;
                Signal sweep(static_cast<size_t>((g_quick ? 4.0 : 8.0) * sr));
                double phase = 0.0;
                for (size_t i = 0; i < sweep.size(); ++i)
                {
                    const double t = static_cast<double>(i) / static_cast<double>(sweep.size());
                    phase += roar::dsp::c_twoPi * 50.0 * std::pow(40.0, t) / sr;
                    sweep[i] = static_cast<float>(0.1 * std::sqrt(2.0) * std::sin(phase));
                }
                auto shout = makeVoice(sr, g_quick ? 1.5 : 3.0, 82.0, 11);
                for (auto& v : shout)
                    v = std::tanh(4.0f * v);
                normalisePeak(shout, 0.5f);
                struct PeakCase { const char* name; const Signal* x; float resonance; double inputPeak, allowDb; };
                const std::array<PeakCase, 4> cases { { { "click", &click, 1.0f, 0.5, 0.5 },
                                                        { "sweep 50 Hz - 2 kHz", &sweep, 1.0f, 0.1 * std::sqrt(2.0), 1.0 },
                                                        { "shout, Resonance 0 %", &shout, 0.0f, 0.5, 1.5 },
                                                        { "shout, Resonance 100 %", &shout, 1.0f, 0.5, 1.5 } } };
                std::array<std::array<double, cases.size()>, ChamberBank::numRooms> peaks {};
                for (int r = 0; r < ChamberBank::numRooms; ++r)
                    for (size_t k = 0; k < cases.size(); ++k)
                    {
                        double worst = -200.0;
                        for (int speaker = 0; speaker < 3; ++speaker)
                        {
                            auto s = on;
                            s.amount = 1.0f;
                            s.resonance = cases[k].resonance;
                            s.edge = 0.0f;
                            s.width = 0.0f;
                            s.room = r;
                            s.speaker = speaker;
                            const auto wet = wetOf(s, dual(*cases[k].x));
                            float top = 0.0f;
                            for (const float v : wet)
                                top = std::max(top, std::abs(v));
                            worst = std::max(worst, toDb(top / cases[k].inputPeak));
                        }
                        peaks[static_cast<size_t>(r)][k] = worst;
                    }
                for (size_t k = 0; k < cases.size(); ++k)
                {
                    double designed = -200.0;
                    for (int r = 0; r < 8; ++r)
                        designed = std::max(designed, peaks[static_cast<size_t>(r)][k]);
                    std::printf("  wet peak re the input (%s): first eight up to %+.1f dB; Party Wall %+.1f, Tower Block %+.1f dB\n",
                                cases[k].name, designed, peaks[8][k], peaks[9][k]);
                    for (int r = 8; r < ChamberBank::numRooms; ++r)
                        check(peaks[static_cast<size_t>(r)][k] <= designed + cases[k].allowDb,
                              juce::String(Params::chamberRoomNames[static_cast<size_t>(r)]) + " peaks no higher than the other rooms ("
                                  + cases[k].name + ")");
                }
            }

            // Resonance's level hold stays clear of its limit in every room (the clip audit: Tower
            // Block at Resonance 100 % asks for the most, about 11 of the 12 dB). A room that needed
            // more would change level at the ends of the knob instead of holding it.
            {
                double worstHold = 0.0;
                int worstRoom = 0;
                for (int r = 0; r < ChamberBank::numRooms; ++r)
                    for (const float u : { 0.0f, 1.0f })
                    {
                        auto s = on;
                        s.amount = 1.0f;
                        s.room = r;
                        s.resonance = u;
                        auto m = prepared(s);
                        runChamber(*m, dual(Signal(2048, 0.0f)), { 512 }, false);
                        if (std::abs(m->getLevelHoldDb()) > std::abs(worstHold))
                        {
                            worstHold = m->getLevelHoldDb();
                            worstRoom = r;
                        }
                    }
                std::printf("  Resonance's level hold: at most %+.1f dB (%s), limit +-%.0f dB\n", worstHold,
                            Params::chamberRoomNames[static_cast<size_t>(worstRoom)], ChamberModule::c_maxLevelHoldDb);
                check(std::abs(worstHold) <= ChamberModule::c_maxLevelHoldDb - 0.25,
                      "Resonance's level hold stays 0.25 dB clear of its limit (" + juce::String(worstHold, 2) + " dB)");
            }
        }

        // --- Off: the dry, bit for bit, and the Boom hook passes untouched.
        {
            ChamberSettings off;
            auto m = prepared(off);
            const auto out = runChamber(*m, voiceStereo, { 480 }, true);
            check(out.l == voiceStereo.l && out.r == voiceStereo.r, "Off is a bit-exact pass");
            auto l = voiceStereo.l, r = voiceStereo.r;
            m->processReturn(l.data(), r.data(), static_cast<int>(l.size()));
            check(l == voiceStereo.l && r == voiceStereo.r, "the Boom hook is a bit-exact pass while Off");
            check(m->getLatencySamples() == 0, "CHAMBER has no latency");
        }

        // --- Block-size independence (with changes of every control on the way) and clone exactness.
        {
            auto s = on;
            s.width = 0.6f;
            s.edge = 0.5f;
            const auto changes = [&](ChamberModule& m)
            {
                return [&m, s](int start) mutable
                {
                    // Every change lands on a boundary every block pattern below shares.
                    if (start == 7680)  { s.room = 0; s.resonance = 0.9f; m.setParams(s); }
                    if (start == 15360) { s.speaker = 2; s.amount = 0.8f; s.width = 0.0f; m.setParams(s); }
                    if (start == 23040) { s.boom = 3; s.edge = 0.0f; s.width = 1.0f; s.room = 6; m.setParams(s); }
                };
            };
            Stereo reference;
            bool same = true;
            for (const auto& blocks : std::vector<std::vector<int>> { { 512 }, { 1 }, { 64 }, { 480 }, { 7680 }, { 32, 160, 96, 192 } })
            {
                auto m = prepared(s);
                const auto out = runChamber(*m, voiceStereo, blocks, true, changes(*m));
                if (reference.l.empty())
                    reference = out;
                else
                    same = same && out.l == reference.l && out.r == reference.r;
            }
            check(same, "bit-identical at host blocks of 1, 64, 480, 512, 7680 and mixed");

            // A clone mid-ring, during a room change and a Resonance glide, continues bit for bit.
            auto a = prepared(s);
            const int cut = 7680 + 700;
            const Stereo first { Signal(voiceStereo.l.begin(), voiceStereo.l.begin() + cut), Signal(voiceStereo.r.begin(), voiceStereo.r.begin() + cut) };
            const Stereo rest { Signal(voiceStereo.l.begin() + cut, voiceStereo.l.end()), Signal(voiceStereo.r.begin() + cut, voiceStereo.r.end()) };
            runChamber(*a, first, { 128 }, true, changes(*a));
            auto b = prepared(on);   // other settings: the clone takes the source's
            b->copyStateFrom(*a);
            const auto ya = runChamber(*a, rest, { 77 }, true);
            const auto yb = runChamber(*b, rest, { 77 }, true);
            check(ya.l == yb.l && ya.r == yb.r, "copyStateFrom mid-ring (room change and Resonance glide running) is bit-exact");
            std::printf("  clone state: %.0f KB at 48 kHz\n", static_cast<double>(a->getConvolver().getStateBytes()) / 1024.0);
        }

        // --- Width: the mono fold-down is the same at every Width; the room's own stereo at 100 %.
        {
            std::array<Stereo, 2> outs;
            for (int k = 0; k < 2; ++k)
            {
                auto s = on;
                s.amount = 1.0f;
                s.width = k == 0 ? 0.0f : 1.0f;
                auto m = prepared(s);
                outs[static_cast<size_t>(k)] = runChamber(*m, voiceStereo, { 512 }, true);
            }
            const auto mono = [](const Stereo& s)
            {
                Signal m(s.l.size());
                for (size_t i = 0; i < m.size(); ++i)
                    m[i] = 0.5f * (s.l[i] + s.r[i]);
                return m;
            };
            const double m0 = toDb(rmsOf(mono(outs[0]))), m1 = toDb(rmsOf(mono(outs[1])));
            double ll = 0.0, rr = 0.0, lr = 0.0;
            for (size_t i = 0; i < outs[1].l.size(); ++i)
            {
                ll += static_cast<double>(outs[1].l[i]) * outs[1].l[i];
                rr += static_cast<double>(outs[1].r[i]) * outs[1].r[i];
                lr += static_cast<double>(outs[1].l[i]) * outs[1].r[i];
            }
            const double corr = lr / std::sqrt(std::max(ll * rr, 1.0e-30));
            std::printf("  Width: mono fold-down %.2f dB (0 %%) vs %.2f dB (100 %%); L/R correlation at 100 %%: %.2f (dry at -6 dB included)\n",
                        m0, m1, corr);
            check(std::abs(m1 - m0) < 1.0, "Width's mono fold-down within 1 dB of Width 0");
            check(outs[0].l == outs[0].r, "Width 0 is mono");
            check(corr < 0.95, "Width 100 % is stereo");
        }

        // --- Resonance changes the ring, not the level; a longer ring after the voice stops.
        {
            const Stereo burst { Signal(voice.begin(), voice.begin() + voice.size() / 2), {} };
            Stereo padded = burst;
            padded.l.resize(voice.size(), 0.0f);
            padded.r = padded.l;
            std::array<double, 3> level {}, tail {};
            const std::array<float, 3> settings { 0.0f, 0.4f, 1.0f };
            for (size_t k = 0; k < settings.size(); ++k)
            {
                auto s = on;
                s.amount = 1.0f;
                s.width = 0.0f;
                s.room = 1;
                s.resonance = settings[k];
                auto m = prepared(s);
                const auto out = runChamber(*m, padded, { 512 }, false);
                level[k] = toDb(rmsOf(out.l, voice.size() / 8, voice.size() / 2));
                const size_t stop = voice.size() / 2;
                tail[k] = toDb(rmsOf(out.l, stop + static_cast<size_t>(0.15 * sr), stop + static_cast<size_t>(0.45 * sr)));
            }
            std::printf("  Resonance 0 / 40 / 100 %%: level %.1f / %.1f / %.1f dB, tail 150-450 ms %.1f / %.1f / %.1f dB\n",
                        level[0], level[1], level[2], tail[0], tail[1], tail[2]);
            // One sung pitch (110 Hz harmonics) can sit on a room's modes, which ring louder as the
            // decay lengthens; on broadband voice material the level holds within 1.5 dB (checked per
            // room above, and measured on real takes in the parameter audit).
            check(std::abs(level[0] - level[1]) < 2.0 && std::abs(level[2] - level[1]) < 2.0, "Resonance holds the level on one pitch (+-2 dB)");
            check(tail[2] > tail[0] + 6.0, "Resonance lengthens the ring");
        }

        // --- Boom: the shelf after the whole chain, as designed, once it has glided in.
        {
            for (int boom = 1; boom <= 3; ++boom)
            {
                auto s = on;
                s.boom = boom;
                auto m = prepared(s);
                for (const double hz : { 40.0, 1000.0 })
                {
                    auto l = makeSine(sr, 1.0, hz, 0.1);
                    auto r = l;
                    m->processReturn(l.data(), r.data(), static_cast<int>(l.size()));
                    const auto ref = makeSine(sr, 1.0, hz, 0.1);
                    const double got = toDb(rmsOf(l, l.size() / 2) / rmsOf(ref, ref.size() / 2));
                    const double want = Biquad::lowShelf(sr, ChamberModule::c_boomHz, ChamberModule::c_boomQ,
                                                         ChamberModule::c_boomDb[static_cast<size_t>(boom)]).magnitudeDb(hz, sr);
                    check(std::abs(got - want) < 0.2, "Boom " + juce::String(Params::chamberBoomNames[static_cast<size_t>(boom)]) + " at "
                                                          + juce::String(hz, 0) + " Hz: " + juce::String(got, 2) + " dB (design "
                                                          + juce::String(want, 2) + ")");
                    check(r == l, "Boom is the same on both channels");
                }
            }
        }

        // --- Through the plug-in: latency unchanged with CHAMBER On anywhere; Off anywhere renders the
        //     same bit for bit; a 1.0 state (no CHAMBER rows) loads with CHAMBER Off after PITCH.
        {
            auto p = makeProcessor(sr, 512);
            const int base = p->getLatencySamples();
            bool constant = true;
            for (int slot : { 0, 1, 7, 14 })
            {
                setParam(*p, Params::chamberOn, 1.0f);
                setParam(*p, Params::chamberPos, static_cast<float>(slot));
                setParam(*p, Params::chamberWidth, 80.0f);
                render(*p, dual(makeVoice(sr, 0.1)), 512);
                constant = constant && p->getLatencySamples() == base;
            }
            check(constant, "plug-in latency is the same with CHAMBER On in any slot");

            const auto renderAt = [&](int slot)
            {
                auto q = makeProcessor(sr, 512);
                setParam(*q, Params::chamberPos, static_cast<float>(slot));
                restart(*q, sr, 512);
                return render(*q, voiceStereo, 512);
            };
            const auto a = renderAt(1), b = renderAt(14), c = renderAt(0);
            check(a.l == b.l && a.r == b.r && a.l == c.l && a.r == c.r, "CHAMBER Off: the same output in every slot, bit for bit");

            // A 1.0 state: no CHAMBER rows, a reordered chain.
            auto src = makeProcessor(sr, 512);
            setOrder(*src, orderFromCodes("PI CL DS RO SH GR SW DT VO RM EN TA SU OV"));
            juce::MemoryBlock full;
            src->getStateInformation(full);
            auto xml = juce::AudioProcessor::getXmlFromBinary(full.getData(), static_cast<int>(full.getSize()));
            for (auto* e = xml->getFirstChildElement(); e != nullptr;)
            {
                auto* next = e->getNextElement();
                if (e->getStringAttribute("id").startsWith("chamber"))
                    xml->removeChildElement(e, true);
                e = next;
            }
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary(*xml, old);
            auto dst = makeProcessor(sr, 512);
            setParam(*dst, Params::chamberOn, 1.0f);
            setParam(*dst, Params::chamberPos, 9.0f);
            dst->setStateInformation(old.getData(), static_cast<int>(old.getSize()));
            check(getParam(*dst, Params::chamberOn) < 0.5f && juce::roundToInt(getParam(*dst, Params::chamberPos)) == 1,
                  "a 1.0 state loads with CHAMBER Off, right after PITCH");
            render(*dst, dual(makeVoice(sr, 0.2)), 512);
            check(dst->getChain().getTopology().order == orderFromCodes("PI CL DS RO SH GR SW DT VO RM EN TA SU OV"), "a 1.0 state keeps its order");
            check(dst->getLatencySamples() == base, "a 1.0 state keeps its latency");
        }

        // --- CPU (one instance, steady state, block 512).
        {
            for (double fs : { 48000.0, 192000.0 })
            {
                const double secs = g_quick ? 2.0 : 5.0;
                const auto x = dual(makeVoice(fs, secs));
                for (const float width : { 0.0f, 1.0f })
                {
                    auto s = on;
                    s.width = width;
                    ChamberModule m;
                    m.setParams(s);
                    m.prepare(fs, 512);
                    runChamber(m, dual(makeVoice(fs, 0.3)), { 512 }, width > 0.0f);
                    const auto t0 = std::chrono::steady_clock::now();
                    runChamber(m, x, { 512 }, width > 0.0f);
                    const double pct = 100.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / secs;
                    std::printf("  CPU at %.0f kHz, Width %.0f %%: %.2f %% of a core\n", fs / 1000.0, 100.0 * width, pct);
                    static const bool onSharedRunner = std::getenv("CI") != nullptr;
                    if (!onSharedRunner)
                        check(pct < (fs > 100000.0 ? 12.0 : 3.0), "CHAMBER CPU " + juce::String(pct, 2) + " %");
                }
            }
        }

        // --- The worst block: a Room change (or Width turning on) primes the new room's current
        //     partition block at once, on the audio thread, and the 50 ms crossfade runs both
        //     rooms. 192 kHz, block 256, Width 0.7, a room change every 0.25 s.
        {
            const double fs = 192000.0;
            const int block = 256;
            const double secs = g_quick ? 2.0 : 4.0;
            const auto x = dual(makeVoice(fs, secs));
            auto s = on;
            s.width = 0.7f;
            ChamberModule m;
            m.setParams(s);
            m.prepare(fs, block);
            runChamber(m, dual(makeVoice(fs, 0.3)), { block }, true);
            const int changeEvery = static_cast<int>(0.25 * fs) / block * block;
            std::vector<double> times;
            std::vector<bool> isChange;
            auto last = std::chrono::steady_clock::now();
            int sinceChange = 1 << 20;   // blocks since the last change (the change block and the next count)
            int room = s.room;
            runChamber(m, x, { block }, true, [&](int start)
            {
                const auto now = std::chrono::steady_clock::now();
                if (start > 0)
                {
                    times.push_back(std::chrono::duration<double>(now - last).count());
                    isChange.push_back(sinceChange <= 1);
                }
                ++sinceChange;
                if (start > 0 && start % changeEvery == 0)
                {
                    sinceChange = 0;
                    room = (room + 3) % ChamberBank::numRooms;
                    auto t = s;
                    t.room = room;
                    m.setParams(t);
                }
                last = std::chrono::steady_clock::now();
            });
            double steady = 0.0, worstChange = 0.0;
            int steadyCount = 0;
            for (size_t i = 0; i < times.size(); ++i)
            {
                if (isChange[i])
                    worstChange = std::max(worstChange, times[i]);
                else
                {
                    steady += times[i];
                    ++steadyCount;
                }
            }
            steady /= std::max(1, steadyCount);
            const double blockSecs = block / fs;
            std::printf("  worst block at 192 kHz, block 256, Width 70 %%: other blocks (crossfades included) mean %.0f us, on a room change %.0f us (%.1f %% of the block, %.1fx)\n",
                        1.0e6 * steady, 1.0e6 * worstChange, 100.0 * worstChange / blockSecs, worstChange / std::max(1.0e-9, steady));
            static const bool onSharedRunner = std::getenv("CI") != nullptr;
            if (!onSharedRunner)
                check(worstChange < 0.5 * blockSecs, "CHAMBER worst block on a room change " + juce::String(100.0 * worstChange / blockSecs, 1) + " % of the block");
        }
    }

    // The audit's limits for CHAMBER's cost to OUTPUT's maximizer (gain reduction it adds, 95th
    // percentile, against the same chain with CHAMBER off): any room at the worst corner of its
    // legal settings, and a preset or a roll as it plays. These are not what keeps the output from
    // clipping (that is the clamp and true-peak pass mark); they catch a change that makes a room
    // cost the limiter much more than it did when audited. Each is the baseline measured here on
    // these test signals plus a stated margin, so ordinary changes (Resonance's level hold, the
    // speaker makeup, the limiter, another compiler) do not trip them; a change that does is a
    // deliberate re-baseline, recorded in docs/parameter-audit.md ("Clipping and headroom").
    //   worst corner: +5.8 dB measured (Resonant Chamber, bundled rooms; modelled rooms +4.0), + 1 dB;
    //   a preset as shipped or a roll: +2.2 dB measured (Iron Tyrant; rolls +1.5), + 1.8 dB.
    constexpr double c_chamberCornerBaselineDb = 5.8;
    constexpr double c_chamberMaxAddedGrDb = c_chamberCornerBaselineDb + 1.0;
    constexpr double c_chamberPresetMaxAddedGrDb = 4.0;

    // CHAMBER through the whole plug-in (docs/parameter-audit.md, "Clipping and headroom"): every
    // room at the worst corners of its legal settings, every preset that uses CHAMBER (with every
    // room swapped in) and the dice rolls that land on CHAMBER. The pass mark is the plug-in's own:
    // finite, under the ceiling (true peak included), OUTPUT's safety clamp never engaged, and
    // CHAMBER costs the maximizer no more gain reduction than the audit allows. Run once with the
    // bundled rooms (when the build has them) and once with the modelled rooms.
    struct ChamberClipRender
    {
        bool ok = true;
        double truePeakDb = -200.0;   // re the ceiling
        double grP95 = 0.0;           // the maximizer's gain reduction, 95th percentile over active blocks, dB
        std::uint64_t clamps = 0;
    };

    ChamberClipRender chamberClipRender(RoarschachAudioProcessor& p, const Stereo& in, double sr, int block)
    {
        restart(p, sr, block);
        std::vector<double> gr;
        const auto out = render(p, in, block, false, [&](int start)
        {
            // (the reduction of the block before this one; the first blocks are the limiter settling)
            if (start >= 4 * block)
                gr.push_back(-static_cast<double>(p.getChain().getOutput().getReductionDb()));
        });
        ChamberClipRender r;
        const float ceiling = getParam(p, Params::outCeiling);
        const auto st = stats(out);
        r.clamps = p.getChain().getClampCount();
        r.truePeakDb = toDb(std::max(truePeak(out.l), truePeak(out.r))) - ceiling;
        r.ok = st.finite && st.peak <= ceilingGain(ceiling) && r.truePeakDb <= 0.2 && r.clamps == 0;
        std::sort(gr.begin(), gr.end());
        r.grP95 = gr.empty() ? 0.0 : gr[std::min(gr.size() - 1, static_cast<size_t>(0.95 * static_cast<double>(gr.size())))];
        return r;
    }

    void testChamberClip()
    {
        using roar::ChamberBank;
        std::printf("[chamberclip] CHAMBER through the plug-in: every room at its worst legal corners, the presets that use it, the dice\n");
        const double sr = 48000.0;
        const int block = 512;
        const double seconds = g_quick ? 1.5 : 2.5;
        // The reference voice at its own level (IN levels it), the same voice at 0 dBFS, and a
        // dense, low, 0 dBFS voice (a shouted, sustained vowel on a low pitch: every room's low-mid
        // modes at once).
        auto voice = makeVoice(sr, seconds);
        auto hot = voice;
        normalisePeak(hot, 0.98f);
        auto dense = makeVoice(sr, seconds, 82.0, 11);
        for (size_t i = 0; i < dense.size(); ++i)
            dense[i] = std::tanh(4.0f * dense[i]);
        normalisePeak(dense, 0.98f);
        const std::vector<std::pair<const char*, Stereo>> signals { { "voice", dual(voice) }, { "hot", dual(hot) }, { "dense", dual(dense) } };
        const auto roomName = [](int r) { return juce::String(Params::chamberRoomNames[static_cast<size_t>(r)]); };
        constexpr int numFirstRooms = 8;   // the designed rooms; the rest are recordings (Party Wall, Tower Block)

        std::vector<ChamberBank::Source> sources { ChamberBank::Source::modelled };
        if (ChamberBank::hasBundledRooms())
            sources.insert(sources.begin(), ChamberBank::Source::bundled);
        for (const auto source : sources)
        {
            ChamberBank::setForceModelled(source == ChamberBank::Source::modelled);
            const char* sourceName = source == ChamberBank::Source::bundled ? "bundled" : "modelled";

            // 1. Every room at the corners of its legal settings: Chamber 100 %, Edge 100 %, Width
            //    100 %, Boom Heavy, Resonance 0 and 100 %, the Tiny and Horn speakers (the audit's
            //    worst), CHAMBER last in Init (the most gain reduction) and right after PITCH (the
            //    hottest CHAMBER output). Against the same chain with CHAMBER off.
            {
                std::array<double, ChamberBank::numRooms> addedGr {}, worstTp {};
                worstTp.fill(-200.0);
                int bad = 0, renders = 0;
                const std::vector<const char*> orders = g_quick ? std::vector<const char*> { "PI CL DS DT VO RM SH GR SW RO EN TA SU OV CH" }
                                                                : std::vector<const char*> { "PI CL DS DT VO RM SH GR SW RO EN TA SU OV CH",
                                                                                             "PI CH CL DS DT VO RM SH GR SW RO EN TA SU OV" };
                const std::vector<int> speakers = g_quick ? std::vector<int> { 0 } : std::vector<int> { 0, 2 };
                for (const char* order : orders)
                    for (const auto& [name, x] : signals)
                    {
                        if (g_quick && juce::String(name) == "voice")
                            continue;
                        auto ref = makeProcessor(sr, block);
                        setOrder(*ref, orderFromCodes(order));
                        setParam(*ref, Params::chamberOn, 0.0f);
                        const auto off = chamberClipRender(*ref, x, sr, block);
                        bad += off.ok ? 0 : 1;
                        for (int r = 0; r < ChamberBank::numRooms; ++r)
                            for (const float resonance : { 0.0f, 100.0f })
                                for (const int speaker : speakers)
                                {
                                    auto p = makeProcessor(sr, block);
                                    setOrder(*p, orderFromCodes(order));
                                    setValues(*p, { { Params::chamberOn, 1.0f }, { Params::chamber, 100.0f }, { Params::chamberResonance, resonance },
                                                    { Params::chamberDrive, 100.0f }, { Params::chamberWidth, 100.0f },
                                                    { Params::chamberRoom, static_cast<float>(r) }, { Params::chamberSpeaker, static_cast<float>(speaker) },
                                                    { Params::chamberBoom, 3.0f } });
                                    const auto res = chamberClipRender(*p, x, sr, block);
                                    ++renders;
                                    if (!res.ok)
                                    {
                                        ++bad;
                                        std::printf("    BROKEN %s, %s, %s, Resonance %.0f, speaker %d: true peak %+.2f dB re ceiling, %llu clamps\n",
                                                    roomName(r).toRawUTF8(), name, order, resonance, speaker, res.truePeakDb,
                                                    static_cast<unsigned long long>(res.clamps));
                                    }
                                    addedGr[static_cast<size_t>(r)] = std::max(addedGr[static_cast<size_t>(r)], res.grP95 - off.grP95);
                                    worstTp[static_cast<size_t>(r)] = std::max(worstTp[static_cast<size_t>(r)], res.truePeakDb);
                                }
                    }
                double firstEight = 0.0;
                for (int r = 0; r < numFirstRooms; ++r)
                    firstEight = std::max(firstEight, addedGr[static_cast<size_t>(r)]);
                std::printf("  %s rooms, worst corners (%d renders): GR CHAMBER adds (p95)", sourceName, renders);
                for (int r = 0; r < ChamberBank::numRooms; ++r)
                    std::printf("%s %s %+.1f", r == 0 ? "" : ",", roomName(r).toRawUTF8(), addedGr[static_cast<size_t>(r)]);
                std::printf(" dB; worst true peak %+.2f dB re ceiling\n", *std::max_element(worstTp.begin(), worstTp.end()));
                check(bad == 0, juce::String(sourceName) + " rooms at their worst corners: " + juce::String(bad) + " renders over the ceiling or clamped");
                for (int r = 0; r < ChamberBank::numRooms; ++r)
                    check(addedGr[static_cast<size_t>(r)] <= c_chamberMaxAddedGrDb,
                          juce::String(sourceName) + " " + roomName(r) + ": CHAMBER at its worst corner adds "
                              + juce::String(addedGr[static_cast<size_t>(r)], 1) + " dB of limiting (<= " + juce::String(c_chamberMaxAddedGrDb, 1) + ")");
                for (int r = numFirstRooms; r < ChamberBank::numRooms; ++r)
                    check(addedGr[static_cast<size_t>(r)] <= firstEight + 0.5,
                          juce::String(sourceName) + " " + roomName(r) + " costs the limiter no more than the first eight rooms");
            }

            // 2. Every preset that uses CHAMBER, as shipped and with every room swapped in (the
            //    shipped room only in --quick), on the voice and the hot voice.
            {
                const auto& presets = PresetManager::getFactoryPresets();
                int bad = 0, count = 0;
                double worstShipped = 0.0, worstSwapped = 0.0;
                juce::String worstShippedName, worstSwappedName;
                for (size_t i = 0; i < presets.size(); ++i)
                {
                    if (presets[i].values.at(Params::chamberOn) < 0.5f)
                        continue;
                    ++count;
                    const int shippedRoom = static_cast<int>(presets[i].values.at(Params::chamberRoom));
                    for (size_t s = 0; s < 2; ++s)
                    {
                        const auto& x = signals[s].second;
                        auto ref = makeProcessor(sr, block);
                        ref->setCurrentProgram(static_cast<int>(i));
                        setParam(*ref, Params::chamberOn, 0.0f);
                        const auto off = chamberClipRender(*ref, x, sr, block);
                        for (int r = 0; r < ChamberBank::numRooms; ++r)
                        {
                            if (g_quick && r != shippedRoom)
                                continue;
                            auto p = makeProcessor(sr, block);
                            p->setCurrentProgram(static_cast<int>(i));
                            setParam(*p, Params::chamberRoom, static_cast<float>(r));
                            const auto res = chamberClipRender(*p, x, sr, block);
                            if (!res.ok)
                            {
                                ++bad;
                                std::printf("    BROKEN %s in %s (%s): true peak %+.2f dB re ceiling, %llu clamps\n", presets[i].name,
                                            roomName(r).toRawUTF8(), signals[s].first, res.truePeakDb, static_cast<unsigned long long>(res.clamps));
                            }
                            const double added = res.grP95 - off.grP95;
                            if (r == shippedRoom && added > worstShipped)
                            {
                                worstShipped = added;
                                worstShippedName = juce::String(presets[i].name) + " (" + signals[s].first + ")";
                            }
                            if (added > worstSwapped)
                            {
                                worstSwapped = added;
                                worstSwappedName = juce::String(presets[i].name) + " in " + roomName(r) + " (" + signals[s].first + ")";
                            }
                        }
                    }
                }
                std::printf("  %s rooms, %d presets with CHAMBER: GR CHAMBER adds (p95) at most %+.1f dB as shipped (%s), %+.1f dB with any room (%s)\n",
                            sourceName, count, worstShipped, worstShippedName.toRawUTF8(), worstSwapped, worstSwappedName.toRawUTF8());
                check(bad == 0, juce::String(sourceName) + " rooms: " + juce::String(bad) + " CHAMBER preset renders over the ceiling or clamped");
                check(worstShipped <= c_chamberPresetMaxAddedGrDb, juce::String(sourceName) + " rooms: a preset's CHAMBER adds "
                          + juce::String(worstShipped, 1) + " dB of limiting (<= " + juce::String(c_chamberPresetMaxAddedGrDb, 1) + ")");
                check(worstSwapped <= c_chamberMaxAddedGrDb, juce::String(sourceName) + " rooms: a preset with another room adds "
                          + juce::String(worstSwapped, 1) + " dB of limiting (<= " + juce::String(c_chamberMaxAddedGrDb, 1) + ")");
            }

            // 3. ROLL ALL: the rolls that turn CHAMBER on, against the same roll with CHAMBER off.
            {
                const int wanted = g_quick ? 4 : 16;
                int found = 0, bad = 0;
                double worst = 0.0;
                std::array<int, ChamberBank::numRooms> rooms {};
                for (std::uint32_t seed = 1; found < wanted && seed < 400; ++seed)
                {
                    auto p = makeProcessor(sr, block);
                    p->getDice().setSeed(seed);
                    p->getDice().rollAll();
                    if (getParam(*p, Params::chamberOn) < 0.5f)
                        continue;
                    ++found;
                    ++rooms[static_cast<size_t>(std::lround(getParam(*p, Params::chamberRoom)))];
                    const auto res = chamberClipRender(*p, signals[1].second, sr, block);
                    setParam(*p, Params::chamberOn, 0.0f);
                    const auto off = chamberClipRender(*p, signals[1].second, sr, block);
                    bad += res.ok ? 0 : 1;
                    worst = std::max(worst, res.grP95 - off.grP95);
                }
                std::printf("  %s rooms, %d rolls with CHAMBER on (hot voice): GR CHAMBER adds (p95) at most %+.1f dB\n", sourceName, found, worst);
                check(found == wanted && bad == 0, juce::String(sourceName) + " rooms: " + juce::String(bad) + " rolls with CHAMBER over the ceiling or clamped");
                check(worst <= c_chamberPresetMaxAddedGrDb, juce::String(sourceName) + " rooms: a roll's CHAMBER adds "
                          + juce::String(worst, 1) + " dB of limiting (<= " + juce::String(c_chamberPresetMaxAddedGrDb, 1) + ")");
            }
        }
        ChamberBank::setForceModelled(false);
    }

    void testCpu()
    {
        std::printf("[cpu] percent of one core, block 512 (steady state)\n");
        const auto& presets = PresetManager::getFactoryPresets();
        for (double sr : { 48000.0, 192000.0 })
        {
            const double seconds = g_quick ? 2.0 : 6.0;
            const auto in = dual(makeVoice(sr, seconds));
            const auto measure = [&](int preset, bool reorderEveryBlock)
            {
                auto p = makeProcessor(sr, 512);
                p->setCurrentProgram(preset);
                restart(*p, sr, 512);
                std::mt19937 rng(1);
                render(*p, dual(makeVoice(sr, 0.5)), 512);   // warm up
                const auto t0 = std::chrono::steady_clock::now();
                render(*p, in, 512, false, [&](int start)
                {
                    if (reorderEveryBlock && start % (512 * 4) == 0)
                        setOrder(*p, randomOrder(rng));
                });
                const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                return 100.0 * elapsed / seconds;
            };

            const double init = measure(0, false);
            int worstIndex = 0;
            double worst = 0.0;
            for (size_t i = 0; i < presets.size(); ++i)
            {
                if (g_quick && i % 4 != 0)
                    continue;
                const double c = sr > 100000.0 && !g_quick ? measure(static_cast<int>(i), false) : measure(static_cast<int>(i), false);
                if (c > worst)
                {
                    worst = c;
                    worstIndex = static_cast<int>(i);
                }
            }
            const double fading = measure(0, true);
            std::printf("  %6.0f Hz: Init %.1f %%, worst preset %s %.1f %%, Init reordering continuously (both chains) %.1f %%\n",
                        sr, init, presets[static_cast<size_t>(worstIndex)].name, worst, fading);
            // The budgets are wall-clock times, so they are only enforced on a quiet machine: shared CI
            // runners (CI set) vary by several points from run to run, so there they are reported only.
            // Real time (below) is checked everywhere.
            static const bool onSharedRunner = std::getenv("CI") != nullptr;
            if (sr == 48000.0 && !onSharedRunner)
            {
                check(init <= 12.0, "Init CPU " + juce::String(init, 1) + " % (<= 12 %)");
                check(worst <= 25.0, "worst preset CPU " + juce::String(worst, 1) + " % (<= 25 %)");
            }
            else if (sr == 48000.0)
            {
                std::printf("  CPU budgets reported only on a shared runner (Init <= 12 %%, worst <= 25 %%)\n");
            }
            check(worst < 100.0, "not real time at " + juce::String(sr));
        }
    }

    //==========================================================================
    // The creature's evolution (CreatureEvolution directly, then through the processor).

    using Evolution = roar::CreatureEvolution;

    /** Features for a sustained, hard roar (intensity about 0.86). */
    Evolution::Inputs roarInputs()
    {
        Evolution::Inputs in;
        in.inputLevel = 0.1f;
        in.outputLevel = 0.5f;
        in.roarHeat = 0.95f;
        in.growlMod = 0.5f;
        in.subLevel = 0.6f;
        in.limiterGrDb = -4.0f;
        in.clipDb = 1.0f;
        in.jaw = 0.6f;
        return in;
    }

    /** Talking at a moderate level (intensity about 0.36). */
    Evolution::Inputs talkInputs()
    {
        Evolution::Inputs in;
        in.inputLevel = 0.05f;
        in.outputLevel = 0.1f;
        in.roarHeat = 0.3f;
        in.jaw = 0.35f;
        return in;
    }

    void run(Evolution& e, double seconds, const Evolution::Inputs& in, float dt = 0.1f)
    {
        const int steps = static_cast<int>(seconds / dt + 0.5);
        for (int i = 0; i < steps; ++i)
            e.step(dt, in);
    }

    bool finiteParams(const Evolution::Params& p)
    {
        return std::all_of(p.begin(), p.end(), [](float v) { return std::isfinite(v); });
    }

    void testCreature()
    {
        std::printf("[creature] growth, decay, hysteresis, rate, robustness, continuity, egg, reset, state\n");

        check(std::abs(Evolution::intensityOf(roarInputs()) - 0.90f) < 0.03f, "roar intensity ~0.90");
        check(std::abs(Evolution::intensityOf(talkInputs()) - 0.39f) < 0.03f, "talking intensity ~0.39");
        // A loud ROAR on its own (no other module working) still reaches the top in a long session.
        {
            Evolution::Inputs roarOnly;
            roarOnly.inputLevel = 0.1f;
            roarOnly.outputLevel = 0.5f;   // -6 dB
            roarOnly.roarHeat = 1.0f;
            roarOnly.jaw = 0.6f;
            Evolution e;
            run(e, 120.0, roarOnly);
            const float at2 = e.getStage();
            double reached = -1.0;
            for (int i = 0; i < 18 * 600 && reached < 0.0; ++i)
            {
                e.step(0.1f, roarOnly);
                if (e.getStage() >= Evolution::c_apexStage)
                    reached = 120.0 + i * 0.1;
            }
            std::printf("  ROAR alone: I %.2f, stage %.2f at 2 min, 0.92 after %.0f s\n", Evolution::intensityOf(roarOnly), at2, reached);
            check(at2 > 0.5f, "ROAR alone: the liquid band within 2 min (" + juce::String(at2, 3) + ")");
            check(reached > 0.0 && reached <= 15.0 * 60.0, "ROAR alone reaches the top within 15 min");
        }
        {
            auto silent = roarInputs();
            silent.inputLevel = 0.0f;   // knobs and heat alone, no input: nothing grows
            check(Evolution::intensityOf(silent) < 1.0e-9f, "no input, no intensity");
        }

        // Step responses and the long session.
        {
            Evolution e;
            check(e.getStage() < 1.0e-9f && e.getEnergy() < 1.0e-9f, "a new creature starts dormant");
            run(e, 60.0, roarInputs());
            check(e.getEnergy() >= 0.80f && e.getEnergy() <= 0.90f, "60 s roar: heat " + juce::String(e.getEnergy(), 3) + " in [0.80, 0.90]");
            run(e, 60.0, roarInputs());
            check(e.getStage() >= 0.5f && e.getStage() <= 0.72f, "2 min roar: stage " + juce::String(e.getStage(), 3) + " in the liquid band");
            double reached = -1.0;
            for (int i = 0; i < 6000 && reached < 0.0; ++i)
            {
                e.step(0.1f, roarInputs());
                if (e.getStage() >= Evolution::c_apexStage)
                    reached = 120.0 + i * 0.1;
            }
            std::printf("  hard roar: stage 0.92 after %.0f s\n", reached);
            check(reached > 0.0 && reached <= 12.0 * 60.0, "a 12 min hard roar reaches the top");
            run(e, std::max(0.0, 12.0 * 60.0 - reached), roarInputs());
            run(e, 300.0, {});
            std::printf("  then 5 min quiet: heat %.3f, experience %.3f, stage %.3f\n", e.getEnergy(), e.getExperience(), e.getStage());
            check(e.getEnergy() < 0.15f, "5 min quiet: heat cools below 0.15");
            check(std::abs(e.getStage() - 0.31f) <= 0.06f, "5 min quiet: a residue stage near 0.31 (" + juce::String(e.getStage(), 3) + ")");
            run(e, 40.0 * 60.0, {});
            check(e.getStage() < 0.1f, "40 min quiet: home again (" + juce::String(e.getStage(), 3) + ")");
        }
        {
            Evolution e;
            run(e, 600.0, talkInputs());
            check(e.getStage() > 0.12f && e.getStage() < 0.3f, "10 min talking: the moth form (" + juce::String(e.getStage(), 3) + ")");
        }

        // Hysteresis: a 1 Hz wobble of +-0.05 never moves the heat, so it never moves the stage.
        // (Around 0.3 the experience holds still, so the stage must be flat; around 0.5 the
        // experience keeps growing slowly, so only the heat is checked there.)
        for (const float centre : { 0.3f, 0.5f })
        {
            Evolution e;
            auto lo = talkInputs(), hi = talkInputs();
            const auto solveHeat = [](Evolution::Inputs in, float target)
            {
                float a = 0.0f, b = 1.0f;
                for (int i = 0; i < 40; ++i)
                {
                    in.roarHeat = 0.5f * (a + b);
                    (Evolution::intensityOf(in) < target ? a : b) = in.roarHeat;
                }
                in.roarHeat = 0.5f * (a + b);
                return in;
            };
            lo = solveHeat(lo, centre - 0.05f);
            hi = solveHeat(hi, centre + 0.05f);
            float minS = 1.0f, maxS = 0.0f, minE = 1.0f, maxE = 0.0f;
            for (int i = 0; i < 20 * 60 * 10; ++i)
            {
                e.step(0.1f, (i / 5) % 2 == 0 ? lo : hi);
                if (i >= 10 * 60 * 10)
                {
                    minS = std::min(minS, e.getStage());
                    maxS = std::max(maxS, e.getStage());
                    minE = std::min(minE, e.getEnergy());
                    maxE = std::max(maxE, e.getEnergy());
                }
            }
            std::printf("  1 Hz wobble %.2f +- 0.05: heat %.4f..%.4f, stage %.4f..%.4f\n", centre, minE, maxE, minS, maxS);
            check(maxE - minE < 0.005f, "hysteresis: the wobble never moves the heat (" + juce::String(maxE - minE, 4) + ")");
            if (centre < 0.4f)
                check(maxS - minS < 0.01f, "hysteresis: stage peak-to-peak < 0.01 over 10 min (" + juce::String(maxS - minS, 4) + ")");
        }

        // Rate limit: no input sequence moves the stage faster than 1/90 per second.
        {
            std::mt19937 rng(11);
            std::uniform_real_distribution<float> u(0.0f, 1.0f);
            Evolution e;
            e.setPersisted({ 0.5f, 0.6f, 0.5f, {} });
            float maxRate = 0.0f;
            for (int i = 0; i < 40000; ++i)
            {
                Evolution::Inputs in;
                if (u(rng) < 0.5f)
                    in = roarInputs();
                in.roarHeat *= u(rng);
                const float before = e.getStage();
                e.step(0.1f, in);
                maxRate = std::max(maxRate, std::abs(e.getStage() - before) / 0.1f);
            }
            std::printf("  fastest stage motion %.5f /s (limit %.5f)\n", maxRate, 1.0f / 90.0f);
            check(maxRate <= 1.01f / 90.0f, "stage rate <= 1/90 per s");
        }

        // Robustness: non-finite features, odd time steps, a stopped host.
        {
            Evolution e;
            run(e, 30.0, roarInputs());
            Evolution::Inputs bad;
            bad.inputLevel = std::numeric_limits<float>::quiet_NaN();
            bad.outputLevel = std::numeric_limits<float>::infinity();
            bad.roarHeat = -std::numeric_limits<float>::infinity();
            bad.growlMod = std::numeric_limits<float>::quiet_NaN();
            bad.jaw = std::numeric_limits<float>::quiet_NaN();
            for (float dt : { 0.0f, 0.1f, 10.0f, std::numeric_limits<float>::quiet_NaN(), -1.0f })
                e.step(dt, bad);
            check(finiteParams(e.evaluate()) && std::isfinite(e.getStage()), "non-finite input and odd dt: finite");
            const float before = e.getEnergy();
            e.step(10.0f, roarInputs());   // clamped to half a second
            check(std::abs(e.getEnergy() - before) < 0.05f, "a 10 s step is clamped");
            auto stale = roarInputs();
            stale.stale = true;
            const float heat = e.getEnergy();
            run(e, 30.0, stale);
            check(e.getEnergy() < heat && e.getEnergy() > 0.5f * heat, "a stopped host cools slowly, nothing resets");
        }

        // Continuity of every parameter along the stage spine, at every character corner.
        {
            float worst = 0.0f;
            int worstParam = -1, pops = 0;
            for (int c = -1; c < 4; ++c)
            {
                std::array<float, 4> w { 0.25f, 0.25f, 0.25f, 0.25f };
                if (c >= 0)
                    w = { 0.0f, 0.0f, 0.0f, 0.0f }, w[static_cast<size_t>(c)] = 1.0f;
                auto prev = Evolution::evaluateAt(0.0f, w, 0.7f, 5u);
                for (int i = 1; i <= 1000; ++i)
                {
                    const float s = static_cast<float>(i) * 0.001f;
                    const auto cur = Evolution::evaluateAt(s, w, 0.7f, 5u);
                    const auto half = Evolution::evaluateAt(s - 0.0005f, w, 0.7f, 5u);
                    for (int k = 0; k < Evolution::numStageParams; ++k)
                    {
                        const auto kk = static_cast<size_t>(k);
                        const float d = std::abs(cur[kk] - prev[kk]);
                        if (d > worst)
                        {
                            worst = d;
                            worstParam = k;
                        }
                        // a jump doesn't halve with the step; a continuous change does
                        if (d > 0.002f && std::abs(cur[kk] - half[kk]) > 0.9f * d)
                            ++pops;
                    }
                    prev = cur;
                }
            }
            std::printf("  largest parameter change per 0.001 of stage: %.4f (param %d), discontinuities %d\n", worst, worstParam, pops);
            check(worst <= 0.02f, "every parameter moves <= 0.02 per 0.001 of stage");
            check(pops == 0, "no parameter jumps along the stage");
            check(finiteParams(Evolution::evaluateAt(0.37f, { 0.1f, 0.2f, 0.3f, 0.4f }, 0.5f, 99u)), "evaluateAt finite");
        }

        // Character: the modules doing the work bend the form.
        {
            Evolution e;
            auto fluid = talkInputs();
            fluid.outputLevel = 0.4f;
            fluid.shimmerOn = 1.0f;
            fluid.swirlOn = 1.0f;
            fluid.liquidKnob = 0.8f;
            fluid.ensembleWidth = 0.7f;
            fluid.roarHeat = 0.0f;
            run(e, 300.0, fluid);
            const auto w = e.getCharacterWeights();
            check(w[0] > 0.6f, "fluid modules make a fluid creature (" + juce::String(w[0], 2) + ")");
            Evolution quiet;
            auto onButSilent = fluid;
            onButSilent.outputLevel = 0.0f;
            onButSilent.inputLevel = 0.0f;
            run(quiet, 300.0, onButSilent);
            check(quiet.getFamily()[0] < 1.0e-3f, "modules switched On in silence feed nothing");
        }

        // The egg: armed only after 90 s at the top, fired by a fresh shout, never asleep or
        // stopped, a cooldown after, dissolving early on silence, never while Frozen.
        {
            const auto atTop = [] { Evolution e; e.setPersisted({ 0.95f, 1.0f, 1.0f, {} }); return e; };
            auto hold = roarInputs();
            hold.jaw = 0.5f;
            auto breath = roarInputs();
            breath.jaw = 0.1f;
            auto shout = roarInputs();
            shout.jaw = 0.95f;
            const auto shoutAfterBreath = [&](Evolution& e) { run(e, 0.8, breath); run(e, 0.2, shout); };

            Evolution e = atTop();
            run(e, 60.0, hold);
            shoutAfterBreath(e);
            check(e.getEggState() == Evolution::EggState::idle, "egg: not armed after 60 s at the top");
            run(e, 40.0, hold);
            check(e.isEggArmed(), "egg: armed after 90 s at the top");
            run(e, 2.0, shout);
            check(e.getEggState() == Evolution::EggState::idle, "egg: a held roar is not a fresh shout");
            shoutAfterBreath(e);
            check(e.getEggState() == Evolution::EggState::forming, "egg: a fresh shout fires it");
            const float stageAtEgg = e.getStage();
            float peak = 0.0f, maxStep = 0.0f, prev = e.getEggAmount();
            double held = 0.0;
            for (int i = 0; i < 300 && e.getEggState() != Evolution::EggState::idle; ++i)
            {
                e.step(0.1f, hold);
                peak = std::max(peak, e.getEggAmount());
                maxStep = std::max(maxStep, std::abs(e.getEggAmount() - prev));
                prev = e.getEggAmount();
                held += e.getEggAmount() > 0.999f ? 0.1 : 0.0;
            }
            std::printf("  egg: peak %.2f, held %.1f s, largest step %.3f per 0.1 s\n", peak, held, maxStep);
            check(peak > 0.999f && held >= 11.0 && held <= 13.5, "egg: forms, holds ~12 s, dissolves");
            check(maxStep < 0.05f, "egg: no jumps");
            check(std::abs(e.getStage() - stageAtEgg) < 1.0e-6f, "egg: the evolution holds while it shows");
            run(e, 120.0, hold);
            shoutAfterBreath(e);
            check(e.getEggState() == Evolution::EggState::idle, "egg: cooldown honoured");
            check(e.getEggCooldown() > 1000.0f, "egg: a 20 min cooldown");

            Evolution s = atTop();
            run(s, 100.0, hold);
            auto silent = breath;
            silent.inputLevel = 0.0f;
            run(s, 2.0, silent);   // asleep
            auto loudInSleep = shout;
            loudInSleep.inputLevel = 0.0f;
            run(s, 0.3, loudInSleep);
            check(s.getEggState() == Evolution::EggState::idle, "egg: never fires asleep");
            Evolution st = atTop();
            run(st, 100.0, hold);
            auto staleShout = shout;
            staleShout.stale = true;
            run(st, 0.8, breath);
            run(st, 0.2, staleShout);
            check(st.getEggState() == Evolution::EggState::idle, "egg: never fires with the host stopped");

            Evolution a = atTop();
            a.triggerEggForTests();
            run(a, Evolution::c_eggFormSeconds + 1.5, hold);
            check(a.getEggAmount() > 0.999f, "egg: showing");
            double gone = -1.0;
            for (int i = 0; i < 100 && gone < 0.0; ++i)
            {
                a.step(0.1f, silent);
                if (a.getEggState() == Evolution::EggState::idle)
                    gone = (i + 1) * 0.1;
            }
            std::printf("  egg: silence dissolves it in %.1f s (before sleep's fade completes)\n", gone);
            check(gone > 0.0 && gone <= 4.2, "egg: dissolves early on silence");

            // Dips below 0.3 between syllables don't add up to a breath: only an unbroken 0.5 s does.
            Evolution d = atTop();
            run(d, 100.0, hold);
            for (int i = 0; i < 8; ++i)
            {
                run(d, 0.2, breath);
                run(d, 0.3, hold);
            }
            run(d, 0.2, shout);
            check(d.getEggState() == Evolution::EggState::idle, "egg: broken dips are not a breath");

            // No view to draw it: it stays armed, the evolution keeps going, no cooldown is spent.
            Evolution nv = atTop();
            run(nv, 100.0, hold);
            auto breathNoView = breath, shoutNoView = shout;
            breathNoView.eggVisible = shoutNoView.eggVisible = false;
            run(nv, 0.8, breathNoView);
            run(nv, 0.2, shoutNoView);
            check(nv.getEggState() == Evolution::EggState::idle && nv.isEggArmed() && nv.getEggCooldown() <= 0.0f,
                  "egg: never fires with no view to show it");
            Evolution closing = atTop();
            closing.triggerEggForTests();
            run(closing, 3.0, hold);
            auto holdNoView = hold;
            holdNoView.eggVisible = false;
            run(closing, 3.5, holdNoView);
            check(closing.getEggState() == Evolution::EggState::idle, "egg: the view closing dissolves it");

            Evolution f = atTop();
            f.setMode(Evolution::Mode::frozen);
            run(f, 100.0, hold);
            shoutAfterBreath(f);
            check(f.getEggState() == Evolution::EggState::idle && !f.isEggArmed(), "egg: Frozen blocks it");
        }

        // Frozen holds the form; Reset eases home over 8 s and glides to a new blot.
        {
            Evolution e;
            e.setPersisted({ 0.8f, 0.6f, 0.7f, { 0.2f, 0.4f, 0.0f, 0.1f } });
            e.setMode(Evolution::Mode::frozen);
            run(e, 120.0, {});
            check(std::abs(e.getStage() - 0.7f) < 1.0e-6f && std::abs(e.getEnergy() - 0.8f) < 1.0e-6f, "frozen: the form holds");
            e.setMode(Evolution::Mode::live);

            const auto seedBefore = e.getSeed();
            e.reset(seedBefore + 1u);
            e.step(0.5f, {});
            {
                const auto mid = e.getPersisted();
                check(e.isResetting() && mid.stage < 1.0e-9f && mid.energy < 1.0e-9f && mid.experience < 1.0e-9f,
                      "a save during a reset stores where it ends");
            }
            float prev = e.getStage(), maxStep = 0.0f;
            double done = -1.0;
            for (int i = 0; i < 200 && done < 0.0; ++i)
            {
                e.step(0.05f, {});
                maxStep = std::max(maxStep, std::abs(e.getStage() - prev));
                prev = e.getStage();
                if (!e.isResetting())
                    done = (i + 1) * 0.05;
            }
            std::printf("  reset: home in %.2f s, largest step %.4f per 50 ms\n", done, maxStep);
            check(done >= 7.5 && done <= 8.5, "reset takes 8 +- 0.5 s");
            check(e.getStage() < 1.0e-4f && e.getEnergy() < 1.0e-4f, "reset ends at the dormant form");
            check(maxStep < 0.012f, "reset path is continuous");
            check(e.getSeed() == seedBefore + 1u, "reset grows a new creature");
            // the blot glides (no cut): the glide amount moves smoothly 0 -> 1
            check(e.getSeedGlide() > 0.99f, "the new blot has flowed in");
        }

        // Test time scale: a 12-minute session in a minute of wall clock.
        {
            Evolution fast, slow;
            fast.setTimeScaleForTests(12.0f);
            run(fast, 60.0, roarInputs());
            run(slow, 720.0, roarInputs());
            check(std::abs(fast.getStage() - slow.getStage()) < 0.01f, "time scale: 12x runs 12 minutes in one");
        }

        // Through the processor: persisted with the session, without dirtying it.
        {
            struct Changes final : public juce::AudioProcessorListener
            {
                int nonParameter = 0;
                void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
                void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& d) override { nonParameter += d.nonParameterStateChanged ? 1 : 0; }
            };

            auto p = makeProcessor(48000.0, 512);
            Changes changes;
            p->addListener(&changes);
            runLoop(50);
            changes.nonParameter = 0;
            p->withCreatureForTests([](Evolution& e) { e.setPersisted({ 0.61234f, 0.4321f, 0.555f, { 0.1f, 0.2f, 0.3f, 0.05f } }); });
            const auto voice = dual(makeVoice(48000.0, 2.0));
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            for (int pos = 0; pos + 512 <= voice.size(); pos += 512)
            {
                buffer.copyFrom(0, 0, voice.l.data() + pos, 512);
                buffer.copyFrom(1, 0, voice.r.data() + pos, 512);
                p->processBlock(buffer, midi);
                p->stepCreatureForTests(0.1f);
            }
            juce::MemoryBlock state;
            p->getStateInformation(state);
            runLoop(100);
            check(changes.nonParameter == 0, "creature growth never marks the session changed (" + juce::String(changes.nonParameter) + ")");

            Evolution::Persisted saved;
            std::uint32_t seed = 0;
            p->withCreatureForTests([&](Evolution& e) { saved = e.getPersisted(); seed = e.getSeed(); });
            p->setCreatureFrozen(true);
            runLoop(50);
            check(changes.nonParameter >= 1, "Frozen is a session change");
            p->getStateInformation(state);
            p->removeListener(&changes);

            auto q = makeProcessor(48000.0, 512);
            q->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            Evolution::Persisted loaded;
            std::uint32_t loadedSeed = 0;
            q->withCreatureForTests([&](Evolution& e) { loaded = e.getPersisted(); loadedSeed = e.getSeed(); });
            check(loadedSeed == seed, "state round trip: same seed");
            check(std::abs(loaded.energy - saved.energy) < 1.0e-4f && std::abs(loaded.experience - saved.experience) < 1.0e-4f
                      && std::abs(loaded.stage - saved.stage) < 1.0e-4f && std::abs(loaded.family[2] - saved.family[2]) < 1.0e-4f,
                  "state round trip: same growth");
            check(q->isCreatureFrozen(), "state round trip: same mode");
            juce::MemoryBlock again;
            q->getStateInformation(again);
            check(again == state, "state round trip is stable");

            // Frozen keeps its colour with the session (the colour mood's axes), and a reload brings it back.
            {
                check(!q->getCreatureColour().has_value(), "no frozen colour is kept until one is set");
                q->setCreatureColour(std::array<float, 4> { 0.8f, 0.25f, 0.0f, 0.5f });
                juce::MemoryBlock withColour;
                q->getStateInformation(withColour);
                auto c = makeProcessor(48000.0, 512);
                c->setStateInformation(withColour.getData(), static_cast<int>(withColour.getSize()));
                const auto kept = c->getCreatureColour();
                check(kept.has_value() && std::abs((*kept)[0] - 0.8f) < 1.0e-3f && std::abs((*kept)[1] - 0.25f) < 1.0e-3f
                          && std::abs((*kept)[2]) < 1.0e-3f && std::abs((*kept)[3] - 0.5f) < 1.0e-3f && c->isCreatureFrozen(),
                      "state round trip: the frozen colour comes back with the session");
                q->setCreatureColour(std::nullopt);
                check(!q->getCreatureColour().has_value(), "Live clears the kept colour");
            }

            // An older session without a creature keeps this instance's one (and saves it from now on).
            auto r = makeProcessor(48000.0, 512);
            std::unique_ptr<juce::XmlElement> xml(juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize())));
            for (const auto* name : { "viewCreatureSeed", "viewCreatureMode", "viewCreatureEnergy", "viewCreatureExperience", "viewCreatureStage",
                                      "viewCreatureFluid", "viewCreatureBeast", "viewCreatureEldritch", "viewCreatureDeep" })
                xml->removeAttribute(name);
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary(*xml, old);
            std::uint32_t rSeed = 0;
            r->withCreatureForTests([&](Evolution& e) { rSeed = e.getSeed(); });
            r->setStateInformation(old.getData(), static_cast<int>(old.getSize()));
            juce::MemoryBlock rState;
            r->getStateInformation(rState);
            std::unique_ptr<juce::XmlElement> rXml(juce::AudioProcessor::getXmlFromBinary(rState.getData(), static_cast<int>(rState.getSize())));
            check(static_cast<std::uint32_t>(rXml->getStringAttribute("viewCreatureSeed").getLargeIntValue()) == rSeed,
                  "a session without a creature keeps (and saves) this one");

            // The editor wires the view's right-click menu to the processor.
            {
                std::unique_ptr<juce::AudioProcessorEditor> editorBase(q->createEditor());
                auto& view = RoarschachEditorInspector::monster(*dynamic_cast<RoarschachAudioProcessorEditor*>(editorBase.get()));
                check(view.isCreatureFrozen && view.setCreatureFrozen && view.resetCreature && view.keepCreatureColour, "the creature menu is wired");
                view.keepCreatureColour(std::array<float, 4> { 0.5f, 0.0f, 0.0f, 0.0f });
                check(q->getCreatureColour().has_value(), "the menu keeps the frozen colour with the session");
                view.keepCreatureColour(std::nullopt);
                check(view.isCreatureFrozen(), "the menu reads the mode");
                view.setCreatureFrozen(false);
                check(!q->isCreatureFrozen(), "the menu sets the mode");
                check(view.getMouseCursor() == juce::MouseCursor::NormalCursor, "the view keeps the normal cursor");
            }

            // Reset through the processor: a new seed, saved.
            q->resetCreature();
            std::uint32_t newSeed = 0;
            q->withCreatureForTests([&](Evolution& e) { newSeed = e.getSeed(); });
            check(newSeed != seed || seed == 0u, "Reset grows a new creature");
        }
    }

    //==========================================================================
    // The creature view's motion (the jaw spring, the head, the audio forces), the quality
    // governor, the baked noise, and the renderer's GL lifetime.

    // Reversals of y whose swing is at least `prominence` (a peak or trough counts only once the
    // signal has moved that far back from it).
    int countReversals(const std::vector<float>& y, float prominence)
    {
        int reversals = 0, dir = 0;
        float ext = y.empty() ? 0.0f : y.front();
        for (float v : y)
        {
            if (dir >= 0)
            {
                if (v > ext) ext = v;
                else if (ext - v >= prominence) { if (dir > 0) ++reversals; dir = -1; ext = v; }
                else if (dir == 0 && v < ext) {}
            }
            if (dir <= 0)
            {
                if (v < ext && dir < 0) ext = v;
                else if (v - ext >= prominence) { if (dir < 0) ++reversals; dir = 1; ext = v; }
            }
        }
        return reversals;
    }

    // Steps a jaw at `fps` through an input function of time; returns y per frame.
    std::vector<float> runJaw(roar::JawMotion& jaw, double fps, double seconds,
                              const std::function<void(double, float&, float&, float&)>& input)
    {
        std::vector<float> ys;
        const int n = static_cast<int>(std::lround(seconds * fps));
        for (int i = 0; i < n; ++i)
        {
            float x = 0, xf = 0, heat = 0;
            input(i / fps, x, xf, heat);
            jaw.step(static_cast<float>(1.0 / fps), x, xf, heat, 0.5f);
            ys.push_back(jaw.y);
        }
        return ys;
    }

    // (y[i] is the state after step i, at time (i + 1) / fps)
    float sampleAt(const std::vector<float>& y, double fps, double t)
    {
        const double i = std::clamp(t * fps - 1.0, 0.0, static_cast<double>(y.size() - 1));
        const auto i0 = static_cast<size_t>(i);
        const auto i1 = std::min(i0 + 1, y.size() - 1);
        return static_cast<float>(y[i0] + (y[i1] - y[i0]) * (i - static_cast<double>(i0)));
    }

    void testMotion()
    {
        std::printf("[motion] jaw spring, head, audio forces, quality governor, noise\n");
        using roar::JawMotion;

        const auto flap = [](double t, float& x, float& xf, float&)
        {
            const bool hi = std::fmod(t * 4.5, 1.0) < 0.5;
            x = hi ? 0.85f : 0.45f;
            xf = hi ? 0.85f : 0.2f;
        };
        const auto stepOpen = [](double, float& x, float& xf, float&) { x = xf = 0.8f; };

        std::map<double, std::vector<float>> flapAt, openAt;
        for (const double fps : { 30.0, 60.0, 144.0 })
        {
            const auto tag = " (" + juce::String(fps, 0) + " fps)";

            // 1. No flap through 4.5 Hz syllables.
            {
                JawMotion j;
                const auto y = runJaw(j, fps, 3.0, flap);
                flapAt[fps] = y;
                const double mean = std::accumulate(y.begin(), y.end(), 0.0) / static_cast<double>(y.size());
                const double rate = countReversals(y, 0.08f) / 3.0;
                if (fps == 60.0)
                    std::printf("  syllables at 4.5 Hz: %.2f reversals/s, mean opening %.2f\n", rate, mean);
                check(rate <= 1.0, "jaw: no flap" + tag + ", " + juce::String(rate, 2) + " reversals/s");
                check(mean >= 0.55, "jaw: mean opening through syllables" + tag + " " + juce::String(mean, 2));
            }
            // 2. Step open: heavy, no overshoot.
            JawMotion j;
            const auto up = runJaw(j, fps, 3.0, stepOpen);
            openAt[fps] = up;
            {
                const float fin = up.back();
                float peak = 0.0f;
                double t10 = -1, t90 = -1;
                for (size_t i = 0; i < up.size(); ++i)
                {
                    peak = std::max(peak, up[i]);
                    if (t10 < 0 && up[i] >= 0.1f * fin) t10 = static_cast<double>(i + 1) / fps;
                    if (t90 < 0 && up[i] >= 0.9f * fin) t90 = static_cast<double>(i + 1) / fps;
                }
                const double rise = t90 - t10;
                if (fps == 60.0)
                    std::printf("  step open: 10-90 %% rise %.2f s, overshoot %.1f %%\n", rise, 100.0 * (peak / fin - 1.0));
                check(rise >= 0.12 && rise <= 0.45, "jaw: 10-90 % rise 0.12-0.45 s" + tag + " (" + juce::String(rise, 2) + ")");
                check(peak <= fin * 1.02f, "jaw: overshoot <= 2 %" + tag);
            }
            // 2b. The onset keeps up with the sound: a full-scale step reaches half its visible
            // opening (the shader opens by y^1.6) within 150 ms at every jaw mass.
            for (const float sizeJ : { 0.0f, 0.5f, 1.0f })
            {
                JawMotion o;
                const int n = static_cast<int>(std::lround(fps));
                std::vector<float> vis;
                for (int i = 0; i < n; ++i)
                {
                    o.step(static_cast<float>(1.0 / fps), 0.95f, 0.95f, 0.0f, sizeJ);
                    vis.push_back(std::pow(o.y, 1.6f));
                }
                const float plateau = vis.back();
                double t50 = -1.0;
                for (size_t i = 0; i < vis.size() && t50 < 0; ++i)
                    if (vis[i] >= 0.5f * plateau) t50 = static_cast<double>(i + 1) / fps;
                check(t50 > 0.0 && t50 <= 0.150, "jaw: half the visible opening within 150 ms at sizeJ " + juce::String(sizeJ, 1) + tag
                                                     + " (" + juce::String(1000.0 * t50, 0) + " ms)");
            }
            // 3. Step close.
            {
                const float peak = j.y;
                double below = -1.0;
                float lowest = 1.0f, rebound = 0.0f;
                bool touched = false;
                const auto down = runJaw(j, fps, 2.0, [](double, float& x, float& xf, float&) { x = xf = 0.0f; });
                for (size_t i = 0; i < down.size(); ++i)
                {
                    lowest = std::min(lowest, down[i]);
                    if (below < 0 && down[i] < 0.1f * peak) below = static_cast<double>(i + 1) / fps;
                    if (down[i] <= 1.0e-4f) touched = true;
                    else if (touched) rebound = std::max(rebound, down[i]);
                }
                if (fps == 60.0)
                    std::printf("  step close: under 10 %% after %.2f s, rebound %.3f\n", below, rebound);
                check(below > 0.0 && below <= 0.8, "jaw: closes to 10 % within 0.8 s" + tag + " (" + juce::String(below, 2) + ")");
                check(lowest >= 0.0f, "jaw: never below shut" + tag);
                check(rebound <= 0.02f, "jaw: rebound <= 0.02" + tag + " (" + juce::String(rebound, 3) + ")");
            }
            // 4. A roar holds through a 150 ms dip.
            {
                JawMotion r;
                const auto y = runJaw(r, fps, 2.2, [](double t, float& x, float& xf, float&)
                {
                    const bool dip = t >= 1.5 && t < 1.65;
                    x = dip ? 0.6f : 1.0f;
                    xf = dip ? 0.4f : 1.0f;
                });
                const float peak = sampleAt(y, fps, 1.49);
                float through = 1.0f;
                for (double t = 1.5; t < 1.9; t += 1.0 / fps)
                    through = std::min(through, sampleAt(y, fps, t));
                check(through >= 0.85f * peak, "jaw: roar holds through a dip" + tag + " (" + juce::String(through / peak, 2) + " of peak)");
            }
            // 5. A bark (90 ms of voice, through the plug-in's envelopes) opens part way and shuts.
            {
                JawMotion b;
                double env = 0.0, envFast = 0.0;
                const auto y = runJaw(b, fps, 1.3, [&env, &envFast, fps](double t, float& x, float& xf, float&)
                {
                    // the 5 ms / 120 ms and 5 ms / 30 ms envelopes of a full-scale 90 ms burst, per frame
                    const double a = t < 0.09 ? 1.0 : 0.0, dt = 1.0 / fps;
                    env += (a > env ? 1.0 - std::exp(-dt / 0.005) : 1.0 - std::exp(-dt / 0.120)) * (a - env);
                    envFast += (a > envFast ? 1.0 - std::exp(-dt / 0.005) : 1.0 - std::exp(-dt / 0.030)) * (a - envFast);
                    x = roar::InModule::jawOf(static_cast<float>(env));
                    xf = roar::InModule::jawOf(static_cast<float>(envFast));
                });
                const float peak = *std::max_element(y.begin(), y.end());
                const float after = sampleAt(y, fps, 1.09);
                if (fps == 60.0)
                    std::printf("  90 ms bark: peak %.2f, %.3f a second later\n", peak, after);
                // (the onset is fast now: a bark snaps the mouth open while it sounds, then it shuts)
                check(peak >= 0.5f && peak <= 1.05f, "jaw: bark peak 0.5-1.05" + tag + " (" + juce::String(peak, 2) + ")");
                check(after < 0.05f, "jaw: shut within 1 s of a bark" + tag);
            }
            // 8. Silence from any state.
            {
                JawMotion s;
                runJaw(s, fps, 1.0, [](double, float& x, float& xf, float& h) { x = xf = 1.0f; h = 1.0f; });
                runJaw(s, fps, 1.2, [](double, float& x, float& xf, float&) { x = xf = 0.0f; });
                check(s.y < 0.02f && std::abs(s.v) < 0.05f, "jaw: shut and still after 1.2 s of silence" + tag + " (" + juce::String(s.y, 3) + ")");
            }
        }
        // 5b. Speech: phrases of different loudness with 4 Hz syllables. The jaw follows the phrase,
        // not the syllables (few reversals), but loud and soft phrases open differently.
        for (const double fps : { 30.0, 60.0, 144.0 })
        {
            JawMotion sp;
            std::mt19937 rng(7);
            std::uniform_real_distribution<float> uni(0.0f, 1.0f);
            double env = 0.0, envFast = 0.0, phraseEnd = 0.0, nextPhrase = 0.0;
            bool inPhrase = false;
            float loud = 0.7f;
            std::vector<float> y;
            for (int i = 0; i < static_cast<int>(60.0 * fps); ++i)
            {
                const double t = i / fps, dt = 1.0 / fps;
                if (inPhrase && t >= phraseEnd) { inPhrase = false; nextPhrase = t + 0.3 + 0.6 * uni(rng); }
                if (! inPhrase && t >= nextPhrase) { inPhrase = true; phraseEnd = t + 1.0 + 2.5 * uni(rng); loud = 0.4f + 0.55f * uni(rng); }
                const double a = inPhrase ? loud * (1.0 - 0.85 * (0.5 + 0.5 * std::cos(6.283185307 * 4.0 * t))) : 0.0;
                env += (a > env ? 1.0 - std::exp(-dt / 0.005) : 1.0 - std::exp(-dt / 0.120)) * (a - env);
                envFast += (a > envFast ? 1.0 - std::exp(-dt / 0.005) : 1.0 - std::exp(-dt / 0.030)) * (a - envFast);
                sp.step(static_cast<float>(dt), static_cast<float>(env), static_cast<float>(envFast), 0.0f, 0.5f);
                y.push_back(sp.y);
            }
            double m = 0.0, m2 = 0.0;
            int c = 0;
            for (const float v : y)
                if (v > 0.3f) { m += v; m2 += v * v; ++c; }
            m /= std::max(c, 1);
            const double sd = std::sqrt(std::max(0.0, m2 / std::max(c, 1) - m * m));
            const double rate = countReversals(y, 0.08f) / 60.0;
            if (fps == 60.0)
                std::printf("  speech: %.2f reversals/s, opening sd %.3f while open\n", rate, sd);
            const auto tag = " (" + juce::String(fps, 0) + " fps)";
            check(rate <= 1.0, "jaw: speech reversals <= 1/s" + tag + " (" + juce::String(rate, 2) + ")");
            check(sd >= 0.15, "jaw: speech opening varies with the phrase, sd >= 0.15" + tag + " (" + juce::String(sd, 3) + ")");
        }
        // 6. Frame-rate independence (30 vs 144 fps, resampled).
        {
            float worstFlap = 0.0f, worstOpen = 0.0f;
            for (double t = 0.0; t < 2.95; t += 0.01)
            {
                worstFlap = std::max(worstFlap, std::abs(sampleAt(flapAt[30.0], 30.0, t) - sampleAt(flapAt[144.0], 144.0, t)));
                worstOpen = std::max(worstOpen, std::abs(sampleAt(openAt[30.0], 30.0, t) - sampleAt(openAt[144.0], 144.0, t)));
            }
            const float worst = std::max(worstFlap, worstOpen);
            std::printf("  30 vs 144 fps: worst difference %.3f through syllables, %.3f on a step\n", worstFlap, worstOpen);
            // (the fast onset is two or three frames long at 30 fps: a little more room than the slow spring needed)
            check(worst <= 0.13f, "jaw: 30 and 144 fps agree within 0.13 (" + juce::String(worst, 3) + ")");
        }
        // 7. Robustness.
        {
            JawMotion r;
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            const float inputs[] = { nan, inf, -inf, -1.0f, 2.0f, 0.7f };
            const float dts[] = { 0.0f, 1.0e-6f, 0.5f, 1.0f / 60.0f, nan, -1.0f };
            bool ok = true;
            for (float x : inputs)
                for (float dt : dts)
                {
                    r.step(dt, x, x, x, x);
                    ok = ok && std::isfinite(r.y) && std::isfinite(r.v) && std::isfinite(r.lip) && r.y >= 0.0f && r.y <= 1.05f;
                }
            check(ok, "jaw: finite and in range for NaN, inf, negative inputs and odd dt");
        }

        // The head.
        {
            using roar::HeadMotion;
            const auto rate = [](float sleep, float phrase)
            {
                HeadMotion h;
                int wraps = 0;
                float last = 0.0f;
                for (int i = 0; i < 60 * 60; ++i)
                {
                    h.step(1.0f / 60.0f, 0.0f, phrase, false, 0.0f, 0.0f, 0.0f, sleep);
                    if (h.phase < last) ++wraps;
                    last = h.phase;
                }
                return static_cast<float>(wraps) / 60.0f;
            };
            const float awake = rate(0.0f, 0.0f), asleep = rate(1.0f, 0.0f), loud = rate(0.0f, 1.0f);
            std::printf("  breath: %.3f Hz awake, %.3f Hz asleep, %.3f Hz through a loud phrase\n", awake, asleep, loud);
            check(std::abs(awake - 0.20f) < 0.02f && std::abs(asleep - 0.11f) < 0.02f, "head: breath 0.20 Hz awake, 0.11 Hz asleep");
            check(loud <= 0.07f, "head: breathing nearly stops while the phrase is loud");

            HeadMotion h;
            float best = 0.0f;
            double at = 0.0;
            for (int i = 0; i < 120; ++i)
            {
                h.step(1.0f / 120.0f, 0.0f, 0.5f, i == 0, i == 0 ? 5.0f : 0.0f, 0.0f, 0.0f, 1.0f);
                if (h.offY < best) { best = h.offY; at = (i + 1) / 120.0; }
            }
            std::printf("  onset lunge: deepest %.4f at %.0f ms\n", best, 1000.0 * at);
            check(at >= 0.15 && at <= 0.25, "head: the lag peaks 150-250 ms after an onset (" + juce::String(1000.0 * at, 0) + " ms)");

            HeadMotion bad;
            const float nan = std::numeric_limits<float>::quiet_NaN();
            for (int i = 0; i < 10; ++i)
                bad.step(i % 2 ? nan : 0.5f, nan, nan, true, nan, nan, nan, nan);
            check(std::isfinite(bad.offY) && std::isfinite(bad.scaleAdd) && std::isfinite(bad.breath) && std::isfinite(bad.follow), "head: finite under NaN input");
        }

        // The audio forces.
        {
            using roar::AudioForces;
            AudioForces f;
            AudioForces::Inputs in;
            std::vector<double> onsetTimes;
            int lastOnsets = 0;
            for (int i = 0; i < 600; ++i)   // a sub level jumping every 50 ms for 10 s at 60 fps
            {
                in.subLevel = (i / 3) % 2 == 0 ? 1.0f : 0.0f;
                f.step(1.0f / 60.0f, in);
                if (f.onsets != lastOnsets) { onsetTimes.push_back(i / 60.0); lastOnsets = f.onsets; }
            }
            double minGap = 1e9;
            int worstSecond = 0;
            for (size_t k = 1; k < onsetTimes.size(); ++k)
                minGap = std::min(minGap, onsetTimes[k] - onsetTimes[k - 1]);
            for (size_t k = 0; k < onsetTimes.size(); ++k)
            {
                int n = 0;
                for (size_t m = k; m < onsetTimes.size() && onsetTimes[m] < onsetTimes[k] + 1.0 - 1e-9; ++m) ++n;
                worstSecond = std::max(worstSecond, n);
            }
            std::printf("  sub onsets: %zu in 10 s, shortest gap %.0f ms, at most %d in a second\n", onsetTimes.size(), 1000.0 * minGap, worstSecond);
            check(onsetTimes.size() >= 10 && minGap >= 0.12 - 1e-6, "forces: sub onsets keep their 120 ms refractory");
            check(worstSecond <= 3, "forces: at most 3 flashes a second");

            // Followers: at least 40 ms up and 150 ms down (63 % step response).
            const auto t63 = [](bool up, const std::function<void(AudioForces::Inputs&, float)>& setIn,
                                const std::function<float(const AudioForces&)>& out)
            {
                AudioForces a;
                AudioForces::Inputs x;
                setIn(x, up ? 0.0f : 1.0f);
                for (int i = 0; i < 2000; ++i) a.step(0.001f, x);
                setIn(x, up ? 1.0f : 0.0f);
                for (int i = 1; i < 5000; ++i)
                {
                    a.step(0.001f, x);
                    const float v = out(a);
                    if (up ? v >= 0.632f : v <= 0.368f)
                        return i / 1000.0;
                }
                return 99.0;
            };
            struct Fl { const char* n; std::function<void(AudioForces::Inputs&, float)> in; std::function<float(const AudioForces&)> out; };
            const std::vector<Fl> fl {
                { "roar", [](auto& x, float v) { x.roarHeat = v; }, [](const auto& a) { return a.roar; } },
                { "growl", [](auto& x, float v) { x.growlMod = v; }, [](const auto& a) { return a.growl; } },
                { "sub", [](auto& x, float v) { x.subLevel = v; }, [](const auto& a) { return a.sub; } },
                { "swirl", [](auto& x, float v) { x.swirlOn = v; }, [](const auto& a) { return a.swirl; } },
                { "ensemble", [](auto& x, float v) { x.ensemble = v; }, [](const auto& a) { return a.ensemble; } },
                { "clip", [](auto& x, float v) { x.clip = v; }, [](const auto& a) { return a.clip; } },
                { "strain", [](auto& x, float v) { x.strain = v; }, [](const auto& a) { return a.strain; } },
            };
            for (const auto& e : fl)
            {
                const double a = t63(true, e.in, e.out), r = t63(false, e.in, e.out);
                check(a >= 0.039 && r >= 0.149, juce::String("forces: ") + e.n + " follower attack " + juce::String(1000.0 * a, 0)
                                                     + " ms, release " + juce::String(1000.0 * r, 0) + " ms");
            }
        }

        // The quality governor (scripted timings).
        {
            using roar::QualityGovernor;
            const float T = 1.0f / 30.0f;
            const auto warm = [T](QualityGovernor& g) { for (int i = 0; i < 70; ++i) g.step(T, T, -1.0f, false); };

            // (slow by the frame interval, which needs no EMA to build up)
            QualityGovernor g;
            g.reset(3);
            warm(g);
            int at = -1;
            for (int i = 0; i < 25 && at < 0; ++i)
                if (g.step(2.0f * T, T, -1.0f, false) == 2) at = i + 1;
            check(at == 20, "governor: down one tier after 20 slow frames (" + juce::String(at) + ")");
            int frames = 0;
            while (g.step(2.0f * T, T, -1.0f, false) == 2 && frames < 5000) ++frames;
            const float waited = static_cast<float>(frames + 1) * 2.0f * T;
            check(waited >= 2.9f && waited <= 4.5f, "governor: a further drop waits out the 3 s drop cooldown (" + juce::String(waited, 1) + " s)");
            QualityGovernor gg;
            gg.reset(3);
            warm(gg);
            int gpuAt = -1;
            for (int i = 0; i < 60 && gpuAt < 0; ++i)
                if (gg.step(T, T, 6.0f, false) == 2) gpuAt = i + 1;
            check(gpuAt == 20, "governor: a slow GPU (EMA over 4 ms) steps down (" + juce::String(gpuAt) + " frames)");

            // A weak GPU far over budget, at the intervals the view really reports: 70 ms frames are
            // measurements (the view flags only gaps of 250 ms or more as stalls).
            {
                QualityGovernor weak;
                weak.reset(3);
                warm(weak);
                const float raw = 0.070f;
                const bool stalled = raw > 0.25f;   // (MonsterView: m_lastClamped = raw > c_stallSeconds)
                float tDrop = -1.0f, tAt = 0.0f;
                for (int i = 0; i < 200 && tDrop < 0.0f; ++i)
                {
                    tAt += raw;
                    if (weak.step(raw, T, 60.0f, stalled) < 3) tDrop = tAt;
                }
                check(tDrop > 0.0f && tDrop <= 1.0f, "governor: 70 ms frames with 60 ms GPU drop within 1 s (" + juce::String(tDrop, 2) + " s)");
                check(weak.tier == 1, "governor: far over budget drops two tiers at once (" + juce::String(weak.tier) + ")");
                // (the single pass costs more per pixel than tier 1: measurement never goes below tier 1)
                for (int i = 0; i < 400; ++i) weak.step(raw, T, 60.0f, stalled);
                check(weak.tier == 1, "governor: measurement never drops to the single pass (" + juce::String(weak.tier) + ")");
                QualityGovernor r0;
                r0.resume(3, 0, true);
                check(r0.tier == 1, "governor: a remembered tier 0 resumes at tier 1 (" + juce::String(r0.tier) + ")");
            }
            // A busy host message thread (21 fps frames, GPU well under budget) costs no quality.
            {
                QualityGovernor busy;
                busy.reset(3);
                warm(busy);
                for (int i = 0; i < 600; ++i) busy.step(0.048f, T, 1.0f, false);
                check(busy.tier == 3, "governor: slow intervals with a fast GPU timer never drop (" + juce::String(busy.tier) + ")");
            }
            // A start tier of 0 (no capability) is final.
            {
                QualityGovernor none;
                none.reset(0);
                for (int i = 0; i < 3000; ++i) none.step(T, T, 0.2f, false);
                check(none.tier == 0, "governor: a start tier of 0 stays 0");
            }
            // Resuming where this GPU settled (a reopened editor).
            {
                QualityGovernor r;
                r.resume(3, 1, true);
                check(r.tier == 1 && r.startTier == 3 && r.upgradesLocked, "governor: resumes at the settled tier with its lock");
                QualityGovernor r2;
                r2.resume(2, 3, false);
                check(r2.tier == 2, "governor: a resumed tier is capped by the start tier");
            }

            QualityGovernor u;
            u.reset(3);
            warm(u);
            for (int i = 0; i < 20; ++i) u.step(2.0f * T, T, -1.0f, false);   // to 2
            for (int i = 0; i < 1300; ++i) u.step(T, T, 1.0f, false);           // the cooldown passes, then 300 fast frames
            check(u.tier == 3, "governor: back up after 300 fast frames");
            for (int i = 0; i < 2000; ++i) u.step(T, T, 0.5f, false);
            check(u.tier == 3, "governor: never above the starting tier");
            for (int i = 0; i < 60 && u.tier == 3; ++i) u.step(T, T, 8.0f, false);   // to 2 again
            for (int i = 0; i < 3000 && u.tier < 3; ++i) u.step(T, T, 1.0f, false);   // up to 3
            const int before = u.tier;
            for (int i = 0; i < 60 && u.tier == 3; ++i) u.step(T, T, 8.0f, false);   // slow right after the upgrade
            check(before == 3 && u.tier == 2 && u.upgradesLocked, "governor: a failed upgrade reverts and locks");
            for (int i = 0; i < 3000; ++i) u.step(T, T, 0.5f, false);
            check(u.tier == 2, "governor: no upgrade once locked");

            QualityGovernor z;
            z.reset(3);
            warm(z);
            for (int i = 0; i < 100; ++i) { z.step(T, T, 0.0f, false); z.step(T, T, 300.0f, false); }
            check(z.tier == 3 && ! z.emaValid, "governor: ignores readings of 0 and over 250 ms");

            QualityGovernor n;
            n.reset(2);
            warm(n);
            for (int i = 0; i < 20; ++i) n.step(2.0f * T, T, -1.0f, false);
            check(n.tier == 1, "governor: works from the frame interval alone");
            QualityGovernor c;
            c.reset(2);
            warm(c);
            for (int i = 0; i < 40; ++i) c.step(2.0f * T, T, -1.0f, true);
            check(c.tier == 2, "governor: ignores stalled frames");
            QualityGovernor w;
            w.reset(1);
            for (int i = 0; i < 40; ++i) w.step(2.0f * T, T, 9.0f, false);
            check(w.tier == 1, "governor: ignores the first 2 s");

            check(QualityGovernor::startTierFor("AMD Radeon(TM) Graphics", "ATI Technologies Inc.") == 1
                      && QualityGovernor::startTierFor("AMD Radeon(TM) Vega 8 Graphics", "ATI Technologies Inc.") == 1
                      && QualityGovernor::startTierFor("AMD Radeon Pro 560 OpenGL Engine", "ATI Technologies Inc.") == 2
                      && QualityGovernor::startTierFor("AMD Radeon Pro 5500M OpenGL Engine", "ATI Technologies Inc.") == 3
                      && QualityGovernor::startTierFor("AMD Radeon RX 6800 XT", "ATI Technologies Inc.") == 3,
                  "governor: integrated and entry-level AMD parts start lower");
            check(QualityGovernor::startTierFor("Apple M1", "Apple") == 3 && QualityGovernor::startTierFor("NVIDIA GeForce RTX 3060", "NVIDIA Corporation") == 3
                      && QualityGovernor::startTierFor("Intel(R) Iris(R) Xe Graphics", "Intel") == 2
                      && QualityGovernor::startTierFor("Intel(R) UHD Graphics 620", "Intel") == 1
                      && QualityGovernor::startTierFor("llvmpipe (LLVM 15.0.7, 256 bits)", "Mesa") == 0
                      && QualityGovernor::startTierFor("GDI Generic", "Microsoft Corporation") == 0,
                  "governor: starting tier from the renderer name");
        }

        // The top mist's opacity: ~0.88 awake, ~0.95 asleep, thins towards 0.70 on a roar, never
        // clears, eases without pops at any frame rate, and survives garbage input.
        {
            using roar::MistMotion;
            const float awakeT = MistMotion::target(0.0f, 0.0f, 0.2f);
            const float asleepT = MistMotion::target(1.0f, 0.0f, 0.0f);
            const float roarT = MistMotion::target(0.0f, 1.0f, 1.0f);
            check(awakeT >= 0.85f && awakeT <= 0.90f, "mist: awake and quiet ~85-90 % (" + juce::String(awakeT, 3) + ")");
            check(std::abs(asleepT - 0.95f) < 0.01f, "mist: asleep ~95 % (" + juce::String(asleepT, 3) + ")");
            check(roarT >= MistMotion::c_min && roarT <= 0.72f, "mist: a loud roar thins it to ~70 %, never clear (" + juce::String(roarT, 3) + ")");
            bool monotone = true, bounded = true;
            for (int i = 0; i <= 20; ++i)
            {
                const float h = static_cast<float>(i) / 20.0f;
                const float a = MistMotion::target(0.0f, h, 0.5f), b = MistMotion::target(0.0f, h + 0.05f, 0.5f);
                monotone = monotone && b <= a + 1.0e-6f;
                for (const float sl : { 0.0f, 0.5f, 1.0f })
                {
                    const float x = MistMotion::target(sl, h, h);
                    bounded = bounded && x >= MistMotion::c_min && x <= 0.96f;
                }
            }
            check(monotone && bounded, "mist: thins monotonically with heat and stays within its range");

            std::map<double, float> at1s;
            float worstStep = 0.0f;
            for (const double fps : { 30.0, 60.0, 144.0 })
            {
                MistMotion m;
                const float dt = static_cast<float>(1.0 / fps);
                float prev = m.step(dt, 0.0f, 0.0f, 0.2f, 0.0);
                const int n = static_cast<int>(3.0 * fps);
                for (int i = 1; i < n; ++i)
                {
                    const double t = i / fps;
                    const bool roar = t >= 0.5 && t < 2.0;   // a step in, a step out
                    const float a = m.step(dt, 0.0f, roar ? 1.0f : 0.0f, roar ? 1.0f : 0.2f, t);
                    worstStep = std::max(worstStep, std::abs(a - prev) * static_cast<float>(fps / 30.0));
                    if (i == static_cast<int>(1.5 * fps)) at1s[fps] = a;
                    prev = a;
                }
            }
            check(worstStep < 0.03f, "mist: no pops (largest change per 1/30 s " + juce::String(worstStep, 4) + ")");
            check(std::abs(at1s[30.0] - at1s[144.0]) < 0.01f && at1s[30.0] < 0.75f,
                  "mist: thins within a second of a roar, the same at 30 and 144 fps (" + juce::String(at1s[30.0], 3) + ", " + juce::String(at1s[144.0], 3) + ")");
            MistMotion bad;
            const float g = bad.step(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                                     std::numeric_limits<float>::infinity(), -5.0f, std::numeric_limits<double>::quiet_NaN());
            const float g2 = bad.step(1.0e9f, 0.0f, 0.0f, 0.0f, 1.0);
            check(std::isfinite(g) && std::isfinite(g2) && g >= MistMotion::c_min && g2 <= 0.96f
                  && std::isfinite(bad.drift[0]) && std::isfinite(bad.drift[1]), "mist: garbage in stays finite and in range");
        }

        // The baked noise.
        {
            const auto t = roar::NoiseTexture::bake(256);
            const int n = t.size;
            const auto v = [&](int x, int y) { return t.value[static_cast<size_t>(((y % n + n) % n) * n + ((x % n + n) % n))]; };
            float interior = 0.0f, seam = 0.0f;
            for (int y = 0; y < n; ++y)
                for (int x = 0; x + 1 < n; ++x)
                    interior = std::max(interior, std::abs(v(x + 1, y) - v(x, y)));
            for (int i = 0; i < n; ++i)
                seam = std::max({ seam, std::abs(v(0, i) - v(n - 1, i)), std::abs(v(i, 0) - v(i, n - 1)) });
            check(seam <= interior, "noise: tiles seamlessly (seam " + juce::String(seam, 4) + ", interior step " + juce::String(interior, 4) + ")");
            float worst = 0.0f;
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                {
                    const float fx = 0.5f * (v(x + 1, y) - v(x - 1, y)), fy = 0.5f * (v(x, y + 1) - v(x, y - 1));
                    const size_t i = static_cast<size_t>(y * n + x);
                    worst = std::max({ worst, std::abs(t.gradX[i] - fx) * roar::NoiseTexture::c_gradScale, std::abs(t.gradY[i] - fy) * roar::NoiseTexture::c_gradScale });
                }
            check(worst <= 3.0f / 255.0f, "noise: gradient matches finite differences within 3/255 (" + juce::String(worst * 255.0f, 2) + "/255)");
            check(t.rgba.size() == static_cast<size_t>(n * n * 4), "noise: packed");
        }

        // The uniform table: unique names, one per shader uniform.
        {
            std::set<std::string> names;
            for (const auto& s : roar::c_uniformSpecs)
                names.insert(s.name);
            check(static_cast<int>(names.size()) == roar::c_numFrameUniforms, "renderer: uniform names are unique");
        }
    }

    // The clouds' colour mood: read from the settings, 0 at the defaults, rising across each
    // module's knob; its colour per axis; the slow spring that carries the view to it.
    void testMood()
    {
        std::printf("[mood] colour mood from the settings, its colours, the spring\n");
        using M = roar::CreatureMood;
        const auto hsv = [](const std::array<float, 3>& c, float& h, float& s)
        {
            const float mx = std::max(c[0], std::max(c[1], c[2])), mn = std::min(c[0], std::min(c[1], c[2])), d = mx - mn;
            s = mx > 1.0e-6f ? d / mx : 0.0f;
            h = 0.0f;
            if (d > 1.0e-6f)
            {
                const int top = c[0] >= c[1] && c[0] >= c[2] ? 0 : (c[1] >= c[2] ? 1 : 2);
                h = 60.0f * (top == 0 ? std::fmod((c[1] - c[2]) / d + 6.0f, 6.0f) : top == 1 ? (c[2] - c[0]) / d + 2.0f : (c[0] - c[1]) / d + 4.0f);
            }
        };

        // the defaults are neutral, and so is the reference colour for them
        {
            const auto m = M::evaluate(M::Inputs::defaults());
            check(m[0] == 0.0f && m[1] == 0.0f && m[2] == 0.0f && m[3] == 0.0f, "mood: the defaults are neutral");
            const auto k = M::shaderMood(m);
            check(std::abs(k[0]) < 1.0e-4f && std::abs(k[1]) < 1.0e-4f, "mood: neutral leaves the palette as it is");
        }
        // the preset table (and the defaults' preset, Init, is neutral)
        for (const auto& fp : PresetManager::getFactoryPresets())
        {
            const auto in = M::Inputs::read([&fp](const char* id)
            {
                const auto it = fp.values.find(id);
                return it != fp.values.end() ? it->second : Params::defaultOf(id);
            });
            const auto m = M::evaluate(in);
            const auto c = M::referenceColour(m);
            float h = 0.0f, s = 0.0f;
            hsv(c, h, s);
            std::printf("  %-18s aggr %.2f  mach %.2f  eerie %.2f  room %.2f  -> hue %5.1f sat %.2f\n", fp.name, m[0], m[1], m[2], m[3], h, s);
            if (juce::String(fp.name) == "Init")
                check(m[0] + m[1] + m[2] + m[3] < 1.0e-4f, "mood: Init is neutral");
        }

        // each axis rises across its modules' knobs (from the default up), each step a little
        struct Sweep { const char* name; int axis; std::function<void(M::Inputs&, float)> set; };
        const std::vector<Sweep> sweeps {
            { "ROAR drive", M::aggression, [](M::Inputs& in, float x) { in.roar = 45.0f + 55.0f * x; } },
            { "GROWL depth", M::aggression, [](M::Inputs& in, float x) { in.growl = 25.0f + 75.0f * x; } },
            { "PITCH drive", M::aggression, [](M::Inputs& in, float x) { in.pitchDrive = 1.0f + 99.0f * x; } },
            { "PITCH down", M::aggression, [](M::Inputs& in, float x) { in.pitch = -2.0f - 22.0f * x; } },
            { "SUB drive", M::aggression, [](M::Inputs& in, float x) { in.subDrive = 20.0f + 80.0f * x; } },
            { "OUTPUT clip", M::aggression, [](M::Inputs& in, float x) { in.outClip = 18.0f * x; } },
            { "VOCODER gravel", M::machine, [](M::Inputs& in, float x) { in.vocoderOn = 1.0f; in.gravel = 100.0f * x; } },
            { "RING amount", M::machine, [](M::Inputs& in, float x) { in.ringOn = 1.0f; in.ring = 100.0f * x; } },
            { "SHIMMER", M::machine, [](M::Inputs& in, float x) { in.shimmer = 18.0f + 82.0f * x; } },
            { "SWIRL", M::eerie, [](M::Inputs& in, float x) { in.swirl = 21.0f + 79.0f * x; } },
            { "ENSEMBLE", M::eerie, [](M::Inputs& in, float x) { in.ensemble = 10.0f + 90.0f * x; } },
            { "OVERTONE", M::eerie, [](M::Inputs& in, float x) { in.overtone = 4.0f + 96.0f * x; } },
            { "CHAMBER", M::room, [](M::Inputs& in, float x) { in.chamberOn = 1.0f; in.chamber = 100.0f * x; in.chamberResonance = 40.0f; } },
        };
        for (const auto& sw : sweeps)
        {
            float prev = -1.0f, top = 0.0f;
            int rises = 0;
            const int steps = 10;
            bool monotonic = true;
            for (int i = 0; i <= steps; ++i)
            {
                auto in = M::Inputs::defaults();
                sw.set(in, static_cast<float>(i) / steps);
                const float v = M::evaluate(in)[static_cast<size_t>(sw.axis)];
                monotonic = monotonic && v >= prev - 1.0e-6f;
                rises += i > 0 && v > prev + 0.01f ? 1 : 0;
                prev = v;
                top = v;
            }
            // (a single knob reaches a clear share of its axis on its own; the strong moods take several)
            check(monotonic && rises >= steps / 2 && top > 0.25f,
                  juce::String("mood: ") + sw.name + " raises its axis across its travel (to " + juce::String(top, 2) + ", " + juce::String(rises) + " steps)");
        }
        // Mix: a dry signal is neutral
        {
            auto in = M::Inputs::defaults();
            in.roar = 100.0f;
            in.vocoderOn = 1.0f;
            const float wet = M::evaluate(in)[0];
            in.mix = 0.0f;
            const auto dry = M::evaluate(in);
            check(wet > 0.3f && dry[0] == 0.0f && dry[1] == 0.0f, "mood: Mix at 0 (dry) is neutral");
        }

        // the colours: full aggression is ember red, full machine an electric blue clearly bluer than
        // the reference, eerie violet, room green-grey; aggression with machine is a rust (never the
        // eerie violet); all keep the reference's value
        const auto okHueGap = [](float a, float b) { const float d = std::fmod(std::abs(a - b), 360.0f); return std::min(d, 360.0f - d); };
        const M::Tone ref = M::reference();
        const float eerieHue = M::tone({ 0, 0, 1, 0 }).hue;
        {
            const auto lumaOf = [](const std::array<float, 3>& c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; };
            const float l0 = lumaOf({ 0.580f, 0.690f, 0.757f });
            float h0 = 0.0f, s0 = 0.0f;
            hsv({ 0.580f, 0.690f, 0.757f }, h0, s0);
            struct Case { const char* name; M::Axes m; float hLo, hHi, sMin; };
            const std::vector<Case> cases {
                { "aggression: ember red", { 1, 0, 0, 0 }, -15.0f, 20.0f, 0.30f },
                { "machine: electric blue", { 0, 1, 0, 0 }, h0 + 10.0f, 235.0f, 0.32f },
                { "eerie: violet", { 0, 0, 1, 0 }, 260.0f, 300.0f, 0.20f },
                { "room: green-grey", { 0, 0, 0, 1 }, 110.0f, 175.0f, 0.12f },
                { "aggression + machine: rust", { 1, 1, 0, 0 }, -10.0f, 30.0f, 0.25f },
                { "aggression + room: warm stone", { 0.6f, 0, 0, 1 }, -20.0f, 45.0f, 0.15f },
                { "machine + room: teal", { 0, 1, 0, 1 }, 175.0f, 200.0f, 0.20f },
            };
            for (const auto& cs : cases)
            {
                const auto c = M::referenceColour(cs.m);
                float h = 0.0f, s = 0.0f;
                hsv(c, h, s);
                const float hs = h > 300.0f ? h - 360.0f : h;   // (reds either side of 0)
                check(hs >= cs.hLo && hs <= cs.hHi && s >= cs.sMin && s <= 0.45f && std::abs(lumaOf(c) - l0) < 0.03f,
                      juce::String("mood: ") + cs.name + " (hue " + juce::String(h, 1) + ", saturation " + juce::String(s, 2) + ", luma " + juce::String(lumaOf(c), 3) + ")");
            }
        }
        // aggression with machine, at any balance, stays well away from the eerie violet (they sit
        // across the wheel from each other: the stronger keeps its hue, the weaker mutes it)
        {
            float nearest = 360.0f, lowest = 1.0f;
            for (int i = 1; i <= 10; ++i)
                for (int j = 1; j <= 10; ++j)
                {
                    const auto t = M::tone({ i / 10.0f, j / 10.0f, 0, 0 });
                    if (i >= 4 && j >= 4)   // (both clearly there: lighter moods are on their way from the reference)
                        nearest = std::min(nearest, okHueGap(t.hue, eerieHue));
                    lowest = std::min(lowest, t.chroma / ref.chroma);
                }
            check(nearest >= 40.0f && lowest >= 0.99f, "mood: aggression with machine stays at least 40 degrees from the eerie violet (nearest "
                      + juce::String(nearest, 1) + "), and never greyer than the reference (" + juce::String(lowest, 2) + ")");
            check(okHueGap(M::tone({ 1.0f, 0.79f, 0, 0 }).hue, M::tone({ 1, 0, 0, 0 }).hue) < 15.0f, "mood: strong aggression with some machine stays ember");
        }
        // each mood, from none to full: the chroma never drops below the reference's (no grey on the
        // way) and the hue moves steadily from the reference's to the mood's
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                float lowest = 10.0f, prevGap = 0.0f;
                bool steady = true;
                const float full = M::tone([axis] { M::Axes m {}; m[static_cast<size_t>(axis)] = 1.0f; return m; }()).hue;
                for (int i = 0; i <= 40; ++i)
                {
                    M::Axes m {};
                    m[static_cast<size_t>(axis)] = i / 40.0f;
                    const auto t = M::tone(m);
                    lowest = std::min(lowest, t.chroma / ref.chroma);
                    // (the turn's distance from the reference along its way, which never goes back)
                    const float left = okHueGap(t.hue, full);
                    const float gap = 1000.0f - left;
                    steady = steady && gap >= prevGap - 0.01f;
                    prevGap = gap;
                }
                static const char* names[] = { "aggression", "machine", "eerie" };
                check(lowest >= 0.99f && steady, juce::String("mood: ") + names[axis] + " from none to full keeps the reference's chroma (lowest "
                          + juce::String(lowest, 2) + " of it) and turns steadily");
            }
            // a light room keeps the reference; a strong one leans it
            check(okHueGap(M::tone({ 0, 0, 0, 0.4f }).hue, ref.hue) < 0.5f && okHueGap(M::tone({ 0, 0, 0, 1.0f }).hue, ref.hue) > 50.0f,
                  "mood: a light room keeps the reference blue-grey, a strong one leans it");
            // a half mood is part of the way: the hue mostly turned, the chroma between
            const auto half = M::tone({ 0.5f, 0, 0, 0 }), full = M::tone({ 1, 0, 0, 0 });
            check(okHueGap(half.hue, full.hue) < 40.0f && half.chroma > ref.chroma && half.chroma < full.chroma,
                  "mood: half aggression is a partial lean (hue " + juce::String(half.hue, 1) + ", chroma " + juce::String(half.chroma, 3) + ")");
            // the muddy hues (khaki, olive) are never on the way: aggression with room, any mix
            bool clean = true;
            for (int i = 0; i <= 10; ++i)
                for (int j = 0; j <= 10; ++j)
                    for (int k = 0; k <= 4; ++k)
                    {
                        const auto t = M::tone({ i / 10.0f, k / 8.0f, j % 3 == 0 ? 0.0f : 0.3f, j / 10.0f });
                        clean = clean && (t.hue < 75.0f || t.hue > 135.0f || t.chroma < 0.05f);
                    }
            check(clean, "mood: no blend lands on a muddy khaki or olive");
        }
        // the factory presets: a clear mood is never greyer than the reference, and none is muddy
        {
            int grey = 0, muddy = 0;
            juce::String which;
            for (const auto& fp : PresetManager::getFactoryPresets())
            {
                const auto in = M::Inputs::read([&fp](const char* id)
                {
                    const auto it = fp.values.find(id);
                    return it != fp.values.end() ? it->second : Params::defaultOf(id);
                });
                const auto m = M::evaluate(in);
                const auto t = M::tone(m);
                const float strength = 1.0f - (1.0f - m[0]) * (1.0f - m[1]) * (1.0f - m[2]) * (1.0f - m[3]);
                if (strength > 0.3f && t.chroma < 0.99f * ref.chroma)
                {
                    ++grey;
                    which << fp.name << " ";
                }
                if (t.hue > 75.0f && t.hue < 135.0f)
                {
                    ++muddy;
                    which << fp.name << " ";
                }
            }
            check(grey == 0 && muddy == 0, "mood: no factory preset settles greyer than the reference or muddy " + which);
        }
        // the shader turn: none for the reference, warm only for warm tones
        {
            const auto k0 = M::shaderMood(ref);
            const auto ka = M::shaderMood(M::tone({ 1, 0, 0, 0 })), km = M::shaderMood(M::tone({ 0, 1, 0, 0 })), ke = M::shaderMood(M::tone({ 0, 0, 1, 0 }));
            check(k0[0] == 0.0f && k0[1] == 0.0f && k0[2] == 0.0f && ka[2] > 0.9f && km[2] < 0.05f && ke[2] < 0.15f,
                  "mood: the accents' warmth is the mood's (" + juce::String(ka[2], 2) + ", " + juce::String(km[2], 2) + ", " + juce::String(ke[2], 2) + ")");
        }

        // the spring: slow (no snap), settles, the same at any frame rate, gentle overshoot, frozen holds
        {
            using S = roar::MoodSpring;
            const M::Axes zero {}, one { 1.0f, 0.5f, 0.0f, 0.25f };
            const auto run = [&](float fps, float seconds, S sp)
            {
                const int n = static_cast<int>(std::lround(seconds * fps));
                for (int i = 0; i < n; ++i)
                    sp.step(1.0f / fps, one, false, false);
                return sp;
            };
            S sp;
            sp.snap(zero);
            float maxStep = 0.0f, peak = 0.0f, prev = 0.0f;
            float at1 = 0.0f, at3 = 0.0f, at12 = 0.0f;
            for (int i = 1; i <= 30 * 20; ++i)
            {
                sp.step(1.0f / 30.0f, one, false, false);
                maxStep = std::max(maxStep, std::abs(sp.value[0] - prev));
                prev = sp.value[0];
                peak = std::max(peak, sp.value[0]);
                if (i == 30) at1 = sp.value[0];
                if (i == 90) at3 = sp.value[0];
                if (i == 360) at12 = sp.value[0];
            }
            std::printf("  spring 0 -> 1: %.2f after 1 s, %.2f after 3 s, %.3f after 12 s, peak %.3f, largest frame step %.4f\n", at1, at3, at12, peak, maxStep);
            check(at1 < 0.25f && at3 > 0.3f && at3 < 0.85f, "mood spring: a slow skew, not a snap (" + juce::String(at1, 2) + " after 1 s, " + juce::String(at3, 2) + " after 3 s)");
            check(std::abs(at12 - 1.0f) < 0.02f && std::abs(sp.value[0] - 1.0f) < 1.0e-3f, "mood spring: settles within ~10 s");
            check(peak < 1.04f, "mood spring: a gentle overshoot at most (" + juce::String(peak, 3) + ")");
            check(maxStep < 0.02f, "mood spring: no frame jumps (largest step " + juce::String(maxStep, 4) + " at 30 fps)");
            S a, b, c;
            a.snap(zero);
            b.snap(zero);
            c.snap(zero);
            const auto a2 = run(24.0f, 2.5f, a), b2 = run(60.0f, 2.5f, b), c2 = run(144.0f, 2.5f, c);
            float diff = 0.0f;
            for (size_t i = 0; i < 4; ++i)
                diff = std::max(diff, std::max(std::abs(a2.value[i] - b2.value[i]), std::abs(c2.value[i] - b2.value[i])));
            check(diff < 1.0e-4f, "mood spring: the same at 24, 60 and 144 fps (" + juce::String(diff, 6) + ")");
            // frozen: eases to a stop where it is and holds while the target moves
            S f;
            f.snap(zero);
            for (int i = 0; i < 45; ++i)
                f.step(1.0f / 30.0f, one, false, false);
            const float atFreeze = f.value[0];
            for (int i = 0; i < 30 * 15; ++i)
                f.step(1.0f / 30.0f, one, true, false);
            check(std::abs(f.value[0] - atFreeze) < 0.12f && f.value[0] < 0.9f, "mood spring: frozen holds the colour (" + juce::String(atFreeze, 3) + " -> " + juce::String(f.value[0], 3) + ")");
            // a reset while frozen takes the settings' colour once, then holds it
            f.step(1.0f / 30.0f, one, true, true);
            for (int i = 0; i < 30 * 15; ++i)
                f.step(1.0f / 30.0f, zero, true, false);
            check(std::abs(f.value[0] - 1.0f) < 0.02f, "mood spring: a reset while frozen takes the current colour and holds it (" + juce::String(f.value[0], 3) + ")");
            // live again: follows
            for (int i = 0; i < 30 * 15; ++i)
                f.step(1.0f / 30.0f, zero, false, false);
            check(std::abs(f.value[0]) < 0.02f, "mood spring: live again, it follows the settings");
            // a kept colour (saved with the session): a frozen view opens on it and holds it
            {
                S k;
                const M::Axes kept { 0.0f, 0.9f, 0.0f, 0.0f };
                k.snap(one, true, &kept);
                for (int i = 0; i < 30 * 5; ++i)
                    k.step(1.0f / 30.0f, one, true, false, &kept);
                check(std::abs(k.value[1] - 0.9f) < 1.0e-3f && std::abs(k.value[0]) < 1.0e-3f, "mood spring: a frozen view opens on the kept colour and holds it");
                S live;
                live.snap(one, false, &kept);
                check(std::abs(live.value[0] - 1.0f) < 1.0e-6f, "mood spring: a live view ignores a kept colour");
            }
            // nonsense in, nothing out
            S n;
            n.snap(zero);
            n.step(std::numeric_limits<float>::quiet_NaN(), one, false, false);
            M::Axes bad { std::numeric_limits<float>::quiet_NaN(), 0, 0, 0 };
            n.step(1.0f / 30.0f, bad, false, false);
            check(std::isfinite(n.value[0]) && std::isfinite(n.velocity[0]), "mood spring: non-finite input is ignored");
        }

        // the colour on screen: from one preset's mood to another's, the hue turns at a bounded speed,
        // never through grey, and arrives; the same at any frame rate
        {
            struct Change { const char* name; M::Axes from, to; };
            const std::vector<Change> changes {
                { "none to aggression", { 0, 0, 0, 0 }, { 1, 0, 0, 0 } },
                { "aggression to machine", { 1, 0, 0, 0 }, { 0, 1, 0, 0 } },
                { "machine to eerie", { 0, 1, 0, 0 }, { 0.04f, 0.26f, 0.95f, 0 } },
                { "aggression to room", { 1, 0, 0, 0 }, { 0, 0, 0, 1 } },
            };
            for (const auto& ch : changes)
            {
                const auto run = [&](float fps, float& fastest, float& lowest, float& at)
                {
                    roar::MoodSpring sp;
                    roar::ToneFollower f;
                    sp.snap(ch.from);
                    f.snap(M::tone(ch.from));
                    fastest = 0.0f;
                    lowest = 10.0f;
                    const float dt = 1.0f / fps;
                    const int n = static_cast<int>(std::lround(16.0f * fps));
                    for (int i = 0; i < n; ++i)
                    {
                        const float before = f.value.hue;
                        sp.step(dt, ch.to, false, false);
                        f.step(dt, M::tone(sp.value));
                        fastest = std::max(fastest, okHueGap(f.value.hue, before) / dt);
                        lowest = std::min(lowest, f.value.chroma / ref.chroma);
                        if (i == static_cast<int>(std::lround(4.0f * fps)))
                            at = f.value.hue;
                    }
                    return f.value;
                };
                float fast60 = 0, low60 = 0, at60 = 0, fast24 = 0, low24 = 0, at24 = 0, fast144 = 0, low144 = 0, at144 = 0;
                const auto end60 = run(60.0f, fast60, low60, at60);
                run(24.0f, fast24, low24, at24);
                run(144.0f, fast144, low144, at144);
                const auto goal = M::tone(ch.to);
                std::printf("  colour %-22s fastest turn %.1f deg/s, lowest chroma %.2f of the reference, ends at hue %.1f (goal %.1f)\n",
                            ch.name, fast60, low60, end60.hue, goal.hue);
                check(std::max(fast60, std::max(fast24, fast144)) <= roar::ToneFollower::c_maxTurn + 0.5f,
                      juce::String("mood colour: ") + ch.name + " turns at most " + juce::String(roar::ToneFollower::c_maxTurn, 0) + " degrees a second ("
                          + juce::String(std::max(fast60, std::max(fast24, fast144)), 1) + ")");
                check(std::min(low60, std::min(low24, low144)) >= 0.8f, juce::String("mood colour: ") + ch.name + " never passes through grey (lowest chroma "
                          + juce::String(std::min(low60, std::min(low24, low144)), 2) + " of the reference)");
                check(okHueGap(end60.hue, goal.hue) < 1.0f && std::abs(end60.chroma - goal.chroma) < 0.002f, juce::String("mood colour: ") + ch.name + " arrives");
                check(okHueGap(at24, at60) < 3.0f && okHueGap(at144, at60) < 3.0f, juce::String("mood colour: ") + ch.name + " is the same at 24, 60 and 144 fps ("
                          + juce::String(at24, 1) + ", " + juce::String(at60, 1) + ", " + juce::String(at144, 1) + ")");
            }
            roar::ToneFollower f;
            f.snap(M::tone({ 1, 0, 0, 0 }));
            f.step(std::numeric_limits<float>::quiet_NaN(), ref);
            f.step(1.0f / 30.0f, M::Tone { std::numeric_limits<float>::quiet_NaN(), 0.05f });
            check(std::isfinite(f.value.hue) && std::isfinite(f.value.chroma), "mood colour: non-finite input is ignored");
        }
    }

   #if defined(__APPLE__)
    // The renderer in a real (offscreen) GL context: every program builds, frames draw on every
    // tier, a tier change and a context loss leave nothing behind and nothing empty.
    void testRenderer()
    {
        std::printf("[renderer] GL lifetime, tiers, first frame\n");
        for (const bool core : { true, false })
        {
            CGLPixelFormatAttribute attrs[] = { kCGLPFAOpenGLProfile, static_cast<CGLPixelFormatAttribute>(core ? kCGLOGLPVersion_3_2_Core : kCGLOGLPVersion_Legacy),
                                                kCGLPFAColorSize, static_cast<CGLPixelFormatAttribute>(24), static_cast<CGLPixelFormatAttribute>(0) };
            CGLPixelFormatObj pf = nullptr;
            GLint npf = 0;
            if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || pf == nullptr)
            {
                std::printf("  (no %s GL pixel format: skipped)\n", core ? "core" : "legacy");
                continue;
            }
            CGLContextObj ctx = nullptr;
            CGLCreateContext(pf, nullptr, &ctx);
            CGLDestroyPixelFormat(pf);
            CGLSetCurrentContext(ctx);
            juce::gl::loadFunctions();
            using namespace juce::gl;
            const juce::String tag = core ? " (core)" : " (legacy)";

            const int W = 784, H = 240;
            GLuint fbo = 0, rb = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glGenRenderbuffers(1, &rb);
            glBindRenderbuffer(GL_RENDERBUFFER, rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);

            const auto& noise = roar::NoiseTexture::shared();
            const auto& logo = LogoSdf::shared();
            roar::MonsterFrame frame;
            const auto p = roar::CreatureEvolution::evaluateAt(0.4f, { 0.25f, 0.25f, 0.25f, 0.25f }, 1.0f, 1u);
            const auto u = roar::CreatureUniforms::pack(p, 1.0f, 0.25f, 0.3f, 0.7f);
            using roar::U;
            frame.set(U::uRes, static_cast<float>(W), static_cast<float>(H));
            frame.set(U::uPxScale, 0.5f);
            frame.set(U::uJaw, 0.4f);
            frame.set(U::uRoar, 0.35f);
            frame.set(U::uSize, 0.35f);
            frame.set(U::uEvoHead, u.head[0], u.head[1], u.head[2], u.head[3]);
            frame.set(U::uEvoWing, u.wing[0], u.wing[1], u.wing[2], u.wing[3]);
            frame.set(U::uEvoHorn, u.horn[0], u.horn[1], u.horn[2], u.horn[3]);
            frame.set(U::uEvoEyes, u.eyes[0], u.eyes[1], u.eyes[2], u.eyes[3]);
            frame.set(U::uEvoMaw, u.maw[0], u.maw[1], u.maw[2], u.maw[3]);
            frame.set(U::uEvoInk, u.ink[0], u.ink[1], u.ink[2], u.ink[3]);
            frame.set(U::uEvoFlow, u.flow[0], u.flow[1], u.flow[2], u.flow[3]);
            frame.set(U::uEvoPal, u.pal[0], u.pal[1], u.pal[2], u.pal[3]);
            frame.set(U::uEvoMisc, u.misc[0], u.misc[1], u.misc[2], u.misc[3]);
            frame.set(U::uForm, u.form[0], u.form[1], u.form[2], u.form[3]);
            frame.set(U::uCloud, u.cloud[0], u.cloud[1], u.cloud[2], u.cloud[3]);
            frame.set(U::uSeed, u.seed[0], u.seed[1], u.seed[2], u.seed[3]);
            frame.set(U::uColCloud, u.colCloud[0], u.colCloud[1], u.colCloud[2]);
            frame.set(U::uColCloudLit, u.colCloudLit[0], u.colCloudLit[1], u.colCloudLit[2]);
            frame.set(U::uColScope, 0.035f, 0.043f, 0.06f);
            frame.set(U::uColOutput, 1.0f, 0.71f, 0.3f);
            frame.set(U::uColRoar, 1.0f, 0.478f, 0.271f);      // (Theme::Colours::roar, as the view sets it: a roar's glow takes its hue)
            frame.set(U::uColVocoder, 0.776f, 0.910f, 0.420f);  // (Theme::Colours::vocoder)

            const auto meanLuma = [&]
            {
                std::vector<unsigned char> px(static_cast<size_t>(W * H * 4));
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                double s = 0.0;
                for (size_t i = 0; i < px.size(); i += 4)
                    s += 0.3 * px[i] + 0.59 * px[i + 1] + 0.11 * px[i + 2];
                return s / (W * H) / 255.0;
            };
            const int vp[4] = { 0, 0, W, H };
            const auto draw = [&](roar::MonsterRenderer& r, int frames)
            {
                for (int i = 0; i < frames; ++i)
                {
                    frame.clock += 1.0 / 30.0;
                    r.prepare(frame);
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    glClearColor(0, 0, 0, 1);
                    glClear(GL_COLOR_BUFFER_BIT);
                    r.render(frame, fbo, vp);
                }
            };

            roar::MonsterRenderer r;
            r.setFixedStep(true, 30);
            const bool created = r.create(core, static_cast<float>(W) / H, logo.rgba, LogoSdf::c_size, noise.rgba, noise.size);
            check(created, "renderer: created" + tag);
            if (created)
            {
                const auto& st = r.getStats();
                std::printf("  %s: start tier %d, float probe %s, timers %s, %d texture units, programs built in %.0f ms\n", core ? "core" : "legacy",
                            st.startTier, st.probeOk ? "ok" : "failed", st.timers ? "yes" : "no", st.maxUnits, st.compileMs);
                // A shared CI runner's virtual GPU may only run tier 0 (the plug-in falls back to it the same way),
                // so the fluid-tier checks are reported, not gated, there. On a real machine they gate.
                const bool fluidTiers = st.probeOk && st.startTier >= 1;
                const bool fluidReportOnly = ! fluidTiers && std::getenv("CI") != nullptr;
                const auto fluidCheck = [&](bool ok, const juce::String& what)
                {
                    if (fluidReportOnly)
                        std::printf("  (reported only: this GPU runs tier 0) %s: %s\n", what.toRawUTF8(), ok ? "ok" : "no");
                    else
                        check(ok, what);
                };
                fluidCheck(fluidTiers, "renderer: half floats work and the fluid tiers are available" + tag);
                for (const int tier : { 3, 2, 1, 0 })
                {
                    r.setForcedTier(tier);
                    draw(r, 4);
                    const double l = meanLuma();
                    const juce::String what = "renderer: tier " + juce::String(tier) + " draws (mean " + juce::String(l, 3) + ")" + tag;
                    if (tier == 0)
                        check(r.getTier() == tier && l > 0.02 && l < 0.9, what);
                    else
                        fluidCheck(r.getTier() == tier && l > 0.02 && l < 0.9, what);
                }
                // The atmosphere: prepare() fills the mist (eased, 85-90 % awake) and a near-white hue lean,
                // and the drawn frame is high-key and cool (the palette's luma and saturation, loosely:
                // this test scene is small and not the reference state).
                {
                    const float a = frame.get(U::uAtmo, 0);
                    const float* tint = frame.at(U::uAtmoTint);
                    check(a >= 0.84f && a <= 0.90f && frame.get(U::uAtmo, 3) > 0.5f, "renderer: the top mist is on at ~85-90 % (" + juce::String(a, 3) + ")" + tag);
                    check(std::abs(tint[0] - 1.0f) < 0.25f && std::abs(tint[1] - 1.0f) < 0.25f && std::abs(tint[2] - 1.0f) < 0.25f,
                          "renderer: the stage's hue lean is subtle" + tag);
                    frame.set(U::uRadius, 4.0f);   // (as the view sets it: with no radius the corner mask halves every pixel)
                    for (const int tier : { 3, 0 })
                    {
                        r.setForcedTier(tier);
                        draw(r, 8);
                        std::vector<unsigned char> px(static_cast<size_t>(W * H * 4));
                        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                        std::vector<float> lum, sat, hues;
                        double rs = 0.0, bs = 0.0;
                        for (int y = H / 10; y < H - H / 10; ++y)
                            for (int x = W / 20; x < W - W / 20; ++x)
                            {
                                const unsigned char* q = &px[(static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)) * 4];
                                const float rr = q[0] / 255.0f, gg = q[1] / 255.0f, bb = q[2] / 255.0f;
                                const float mx = std::max(rr, std::max(gg, bb)), mn = std::min(rr, std::min(gg, bb));
                                lum.push_back(0.3f * rr + 0.59f * gg + 0.11f * bb);
                                sat.push_back(mx > 1.0e-4f ? (mx - mn) / mx : 0.0f);
                                if (mx > 1.0e-4f && (mx - mn) / mx > 0.05f)
                                {
                                    const float d = mx - mn;
                                    const int top = rr >= gg && rr >= bb ? 0 : (gg >= bb ? 1 : 2);
                                    float hh = top == 0 ? std::fmod((gg - bb) / d + 6.0f, 6.0f) : top == 1 ? (bb - rr) / d + 2.0f : (rr - gg) / d + 4.0f;
                                    hues.push_back(hh * 60.0f);
                                }
                                rs += rr;
                                bs += bb;
                            }
                        std::sort(lum.begin(), lum.end());
                        std::sort(sat.begin(), sat.end());
                        const float l50 = lum[lum.size() / 2], s50 = sat[sat.size() / 2];
                        check(l50 > 0.52f && l50 < 0.80f && s50 > 0.12f && s50 < 0.36f && bs > rs,
                              "renderer: tier " + juce::String(tier) + " is high-key and cool (luma median " + juce::String(l50, 3) + ", saturation median " + juce::String(s50, 3) + ")" + tag);
                        std::sort(hues.begin(), hues.end());
                        const float h50 = hues.empty() ? 0.0f : hues[hues.size() / 2];
                        check(h50 > 196.0f && h50 < 212.0f, "renderer: tier " + juce::String(tier) + " keeps the reference's blue-grey hue (median " + juce::String(h50, 1) + " degrees)" + tag);
                    }
                    // The colour mood: full aggression leans the drawn clouds and mist to ember red, full
                    // machine to a clearly bluer, stronger blue, each keeping the high-key values and a bounded
                    // saturation (and no mood leaves the frame as above).
                    {
                        const auto measure = [&](float& l50, float& s50, float& h50, float& redShare)
                        {
                            std::vector<unsigned char> px(static_cast<size_t>(W * H * 4));
                            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                            std::vector<float> lum, sat;
                            double hx = 0.0, hy = 0.0;
                            int red = 0, n = 0;
                            for (int y = H / 10; y < H - H / 10; ++y)
                                for (int x = W / 20; x < W - W / 20; ++x)
                                {
                                    const unsigned char* q = &px[(static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)) * 4];
                                    const float rr = q[0] / 255.0f, gg = q[1] / 255.0f, bb = q[2] / 255.0f;
                                    const float mx = std::max(rr, std::max(gg, bb)), mn = std::min(rr, std::min(gg, bb));
                                    lum.push_back(0.3f * rr + 0.59f * gg + 0.11f * bb);
                                    sat.push_back(mx > 1.0e-4f ? (mx - mn) / mx : 0.0f);
                                    ++n;
                                    red += rr > bb ? 1 : 0;
                                    if (mx > 1.0e-4f && (mx - mn) / mx > 0.05f)
                                    {
                                        const float d = mx - mn;
                                        const int top = rr >= gg && rr >= bb ? 0 : (gg >= bb ? 1 : 2);
                                        const float hh = 60.0f * (top == 0 ? std::fmod((gg - bb) / d + 6.0f, 6.0f) : top == 1 ? (bb - rr) / d + 2.0f : (rr - gg) / d + 4.0f);
                                        hx += std::cos(hh * 0.0174533f);
                                        hy += std::sin(hh * 0.0174533f);
                                    }
                                }
                            std::sort(lum.begin(), lum.end());
                            std::sort(sat.begin(), sat.end());
                            l50 = lum[lum.size() / 2];
                            s50 = sat[sat.size() / 2];
                            h50 = static_cast<float>(std::fmod(std::atan2(hy, hx) * 57.29578 + 360.0, 360.0));   // (the circular mean)
                            redShare = static_cast<float>(red) / static_cast<float>(std::max(1, n));
                        };
                        using M = roar::CreatureMood;
                        frame.set(U::uRadius, 4.0f);
                        for (const int tier : { 3, 0 })
                        {
                            r.setForcedTier(tier);
                            float l0 = 0, s0 = 0, h0 = 0, r0 = 0;
                            frame.set(U::uMood, 0.0f, 0.0f, 0.0f, 0.0f);
                            draw(r, 6);
                            measure(l0, s0, h0, r0);
                            const auto k = M::shaderMood({ 1.0f, 0.0f, 0.0f, 0.0f });
                            frame.set(U::uMood, k[0], k[1], k[2], 0.0f);
                            draw(r, 6);
                            float la = 0, sa = 0, ha = 0, ra = 0;
                            measure(la, sa, ha, ra);
                            const auto km = M::shaderMood({ 0.0f, 1.0f, 0.0f, 0.0f });
                            frame.set(U::uMood, km[0], km[1], km[2], 0.0f);
                            draw(r, 6);
                            float lm = 0, sm = 0, hm = 0, rm = 0;
                            measure(lm, sm, hm, rm);
                            frame.set(U::uMood, 0.0f, 0.0f, 0.0f, 0.0f);
                            const juce::String t = "renderer: tier " + juce::String(tier) + " ";
                            std::printf("  tier %d mood: none hue %.0f sat %.2f luma %.2f | ember hue %.0f sat %.2f luma %.2f red %.2f | electric hue %.0f sat %.2f luma %.2f\n",
                                        tier, h0, s0, l0, ha, sa, la, ra, hm, sm, lm);
                            check((ha < 40.0f || ha > 340.0f) && ra > 0.6f && r0 < 0.1f,
                                  t + "full aggression leans the clouds ember red (hue " + juce::String(ha, 0) + ", " + juce::String(ra * 100.0f, 0) + " % warm)" + tag);
                            check(hm > h0 + 8.0f && hm < 240.0f && sm > s0 + 0.06f,
                                  t + "full machine is a clearly bluer, stronger blue (hue " + juce::String(h0, 0) + " -> " + juce::String(hm, 0) + ", saturation " + juce::String(s0, 2) + " -> " + juce::String(sm, 2) + ")" + tag);
                            check(la > 0.55f && la < 0.80f && lm > 0.55f && lm < 0.80f && std::abs(la - l0) < 0.04f && std::abs(lm - l0) < 0.04f,
                                  t + "the mood keeps the high-key values (luma " + juce::String(l0, 3) + ", " + juce::String(la, 3) + ", " + juce::String(lm, 3) + ")" + tag);
                            check(sa <= 0.45f && sm <= 0.45f, t + "the mood's saturation stays bounded (" + juce::String(sa, 2) + ", " + juce::String(sm, 2) + ")" + tag);
                        }
                    }
                    // Roaring: the warm accents are veiled, not removed (some pixels stay warm), and the
                    // throat's glow sits in a dark cavity (the warmest pixel is below the frame's median value).
                    frame.set(U::uRoarDrive, 1.0f);
                    frame.set(U::uRoar, 1.0f);
                    frame.set(U::uJaw, 1.0f);
                    frame.set(U::uLevel, 0.7f);
                    for (const int tier : { 3, 0 })
                    {
                        r.setForcedTier(tier);
                        draw(r, 12);
                        std::vector<unsigned char> px(static_cast<size_t>(W * H * 4));
                        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                        int warm = 0;
                        float bestRB = -1.0f, bestL = 0.0f;
                        std::vector<float> lum;
                        for (size_t i = 0; i < px.size(); i += 4)
                        {
                            const float rr = px[i] / 255.0f, gg = px[i + 1] / 255.0f, bb = px[i + 2] / 255.0f;
                            const float l = 0.3f * rr + 0.59f * gg + 0.11f * bb;
                            lum.push_back(l);
                            warm += rr - bb > 0.04f ? 1 : 0;
                            // (the throat: the warmest pixel below the centre line, under the eyes; GL rows run bottom up)
                            if (static_cast<int>(i / 4) / W < H / 2 && rr - bb > bestRB) { bestRB = rr - bb; bestL = l; }
                        }
                        std::sort(lum.begin(), lum.end());
                        const float l50 = lum[lum.size() / 2];
                        check(warm >= 40 && bestRB < 0.25f, "renderer: tier " + juce::String(tier) + " roaring keeps veiled warm accents (" + juce::String(warm) + " pixels, R-B max " + juce::String(bestRB, 3) + ")" + tag);
                        check(bestL < l50, "renderer: tier " + juce::String(tier) + " the throat glows in a dark cavity, not a pale fill (" + juce::String(bestL, 3) + " vs median " + juce::String(l50, 3) + ")" + tag);
                    }
                    frame.set(U::uRoarDrive, 0.0f);
                    frame.set(U::uRoar, 0.35f);
                    frame.set(U::uJaw, 0.4f);
                    frame.set(U::uLevel, 0.0f);
                }
                // RING's muzzle blends into the face: no step in value across where the old
                // rectangular plate's sides were (a feathered oval now). The luma step across that
                // line, against the step just beside it on either side; a hard edge measured 3-18x.
                // (Twice the size, so the muzzle's feather spans enough pixels to tell from an edge.)
                {
                    const int W2 = W * 2, H2 = H * 2;
                    GLuint fbo2 = 0, rb2 = 0;
                    glGenFramebuffers(1, &fbo2);
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
                    glGenRenderbuffers(1, &rb2);
                    glBindRenderbuffer(GL_RENDERBUFFER, rb2);
                    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W2, H2);
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb2);
                    const int vp2[4] = { 0, 0, W2, H2 };
                    frame.set(U::uRes, static_cast<float>(W2), static_cast<float>(H2));
                    frame.set(U::uPxScale, 1.0f);
                    frame.set(U::uRadius, 8.0f);
                    frame.set(U::uRing, 1.0f);
                    frame.set(U::uRingBody, 0.6f);
                    frame.set(U::uRingFlicker, 1.0f);
                    {
                        const juce::Colour accent(Params::module(Params::ModuleId::ring).accent);   // (as the view sets it)
                        frame.set(U::uColRing, accent.getFloatRed(), accent.getFloatGreen(), accent.getFloatBlue());
                    }
                    for (const int tier : { 3, 0 })
                        for (const float jaw : { 0.35f, 1.0f })
                        {
                            r.setForcedTier(tier);
                            frame.set(U::uJaw, jaw);
                            for (int i = 0; i < 8; ++i)
                            {
                                frame.clock += 1.0 / 30.0;
                                r.prepare(frame);
                                glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
                                glClearColor(0, 0, 0, 1);
                                glClear(GL_COLOR_BUFFER_BIT);
                                r.render(frame, fbo2, vp2);
                            }
                            std::vector<unsigned char> px(static_cast<size_t>(W2 * H2 * 4));
                            glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
                            glReadPixels(0, 0, W2, H2, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                            const auto L = [&](double x, double yUp)
                            {
                                const int xi = juce::jlimit(0, W2 - 1, static_cast<int>(std::lround(x)));
                                const int yi = juce::jlimit(0, H2 - 1, static_cast<int>(std::lround(yUp)));
                                const unsigned char* q = &px[(static_cast<size_t>(yi) * static_cast<size_t>(W2) + static_cast<size_t>(xi)) * 4];
                                return (0.3 * q[0] + 0.59 * q[1] + 0.11 * q[2]) / 255.0;
                            };
                            // the head transform and the mouth, as the shader has them (GL rows run bottom up)
                            const float* hd = frame.at(U::uHead);
                            const float* cl = frame.at(U::uCloud);
                            const float sizeV = frame.get(U::uSize);
                            const float scale = 1.0f + juce::jlimit(-1.0f, 2.0f, -frame.get(U::uPitch) / 12.0f) * 0.065f + 0.014f * hd[3] * 0.35f + jaw * 0.025f + hd[1];
                            const float offY = 0.025f + hd[0];
                            const float eyeOuter = 0.135f * cl[1] + 0.056f * (1.0f + (frame.get(U::uEvoEyes, 3) - 1.0f) * 0.5f);
                            const float mw = std::min((0.15f + 0.08f * sizeV) * frame.get(U::uEvoMaw, 0), eyeOuter * 0.92f) * (1.0f - 0.10f * jaw);
                            const float open = 0.006f + std::pow(jaw, 1.6f) * (0.12f + 0.05f * (sizeV + frame.get(U::uEvoHead, 2)));
                            const float mY = -0.075f, pw = mw * (1.12f + 0.08f * 0.6f), top = mY + 0.032f + 0.22f * open;
                            double stepSum = 0.0, baseSum = 0.0;
                            for (int k = 0; k < 30; ++k)
                            {
                                const float hy = (mY - 0.5f * open) + (top - (mY - 0.5f * open)) * (0.1f + 0.8f * k / 30.0f);
                                const double y = H2 * 0.5 + (hy * scale + offY) * H2;
                                for (const int sgn : { -1, 1 })
                                {
                                    const double x = W2 * 0.5 + sgn * pw * scale * H2;
                                    const auto band = [&](double o0, double o1)
                                    {
                                        double s = 0.0;
                                        for (double o = o0; o <= o1; o += 1.0)
                                            s += L(x + sgn * o, y);
                                        return s / (o1 - o0 + 1.0);
                                    };
                                    stepSum += std::abs(band(1, 3) - band(-3, -1));
                                    baseSum += 0.5 * (std::abs(band(9, 11) - band(5, 7)) + std::abs(band(-7, -5) - band(-11, -9)));
                                }
                            }
                            const double ratio = stepSum / std::max(baseSum, 1.0e-6);
                            std::printf("  tier %d, jaw %.2f: RING muzzle, step across the old plate's side %.2fx the step beside it%s\n", tier, jaw, ratio, tag.toRawUTF8());
                            check(ratio < 2.0, "renderer: tier " + juce::String(tier) + " RING's muzzle has no straight edge at jaw " + juce::String(jaw, 2)
                                                   + " (step across the old plate's side " + juce::String(ratio, 2) + "x the step beside it)" + tag);
                        }
                    frame.set(U::uRing, 0.0f);
                    frame.set(U::uRingBody, 0.0f);
                    frame.set(U::uJaw, 0.4f);
                    frame.set(U::uRes, static_cast<float>(W), static_cast<float>(H));
                    frame.set(U::uPxScale, 0.5f);
                    frame.set(U::uRadius, 4.0f);
                    glDeleteRenderbuffers(1, &rb2);
                    glDeleteFramebuffers(1, &fbo2);
                }
                r.setForcedTier(3);
                draw(r, 6);
                std::vector<float> v;
                int vw = 0, vh = 0;
                bool finite = r.readTarget(roar::MonsterRenderer::Target::velocity, v, vw, vh);
                for (float x : v) finite = finite && std::isfinite(x);
                fluidCheck(finite, "renderer: the fluid stays finite across tier changes" + tag);
                const double before = meanLuma();

                std::vector<unsigned int> tex, fbos, progs, bufs;
                r.collectObjectIds(tex, fbos, progs, bufs);
                r.release();
                int alive = 0;
                for (auto id : tex) alive += glIsTexture(id) ? 1 : 0;
                for (auto id : fbos) alive += glIsFramebuffer(id) ? 1 : 0;
                for (auto id : progs) alive += glIsProgram(id) ? 1 : 0;
                for (auto id : bufs) alive += glIsBuffer(id) ? 1 : 0;
                std::vector<unsigned int> after;
                r.collectObjectIds(after, after, after, after);
                check(! tex.empty() && alive == 0 && after.empty(), "renderer: release deletes every GL object and zeroes the ids (" + juce::String(alive) + " left)" + tag);

                // A context loss and back: the first frame is a full one, not an empty grid.
                r.create(core, static_cast<float>(W) / H, logo.rgba, LogoSdf::c_size, noise.rgba, noise.size);
                draw(r, 1);
                const double first = meanLuma();
                check(std::abs(first - before) <= 0.15 * before, "renderer: first frame after recreation matches (" + juce::String(first, 3) + " vs " + juce::String(before, 3) + ")" + tag);
                r.release();
            }
            glDeleteRenderbuffers(1, &rb);
            glDeleteFramebuffers(1, &fbo);
            CGLSetCurrentContext(nullptr);
            CGLDestroyContext(ctx);
        }
    }
   #endif

    void testLogoSdf()
    {
        std::printf("[logo] distance field of the maker's logo\n");
        const LogoSdf sdf;
        std::printf("  built in %.1f ms, black %.1f %%, red %.1f %% of the box\n", sdf.buildMilliseconds, 100.0 * sdf.grenadeArea, 100.0 * sdf.cherryArea);
        check(sdf.rgba.size() == static_cast<size_t>(LogoSdf::c_size * LogoSdf::c_size * 4), "logo field built");
        if (sdf.rgba.empty())
            return;
        check(sdf.buildMilliseconds < 400.0, "logo field builds quickly");
        // Sampled as the shader does (bilinear), four times finer than the texels.
        const auto sample = [&sdf](float u, float v, int channel)
        {
            const float x = u * LogoSdf::c_size - 0.5f, y = v * LogoSdf::c_size - 0.5f;
            const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
            const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
            const auto at = [&](int xx, int yy) { return sdf.distanceAt(xx, yy, channel); };
            return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
        };
        int insideG = 0, insideC = 0;
        bool finite = true;
        constexpr int fine = LogoSdf::c_size * 4;
        for (int y = 0; y < fine; ++y)
            for (int x = 0; x < fine; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) / fine, v = (static_cast<float>(y) + 0.5f) / fine;
                const float g = sample(u, v, 0), c = sample(u, v, 1);
                finite = finite && std::isfinite(g) && std::isfinite(c);
                insideG += g < 0.0f ? 1 : 0;
                insideC += c < 0.0f ? 1 : 0;
            }
        const float n = static_cast<float>(fine * fine);
        const float areaG = static_cast<float>(insideG) / n, areaC = static_cast<float>(insideC) / n;
        std::printf("  zero-contour areas: black %.2f %%, red %.2f %%\n", 100.0 * areaG, 100.0 * areaC);
        check(finite, "logo field finite");
        check(std::abs(areaG - sdf.grenadeArea) <= 0.02f * sdf.grenadeArea, "black area matches the filled path within 2 %");
        check(std::abs(areaC - sdf.cherryArea) <= 0.02f * sdf.cherryArea, "red area matches the filled path within 2 %");
        check(sdf.distanceAt(0, 0, 0) > 0.0f && sdf.distanceAt(0, 0, 1) > 0.0f, "the corners are outside");
        check(sdf.distanceAt(LogoSdf::c_size - 1, LogoSdf::c_size - 1, 0) > 0.0f, "the far corner is outside");
        check(sdf.grenadeArea > 0.05f && sdf.cherryArea > 0.005f, "both groups found");

        // For the offline shader harness: ROARSCHACH_DUMP_LOGO_SDF=<file> writes the raw RGBA8 texels.
        if (const auto* path = std::getenv("ROARSCHACH_DUMP_LOGO_SDF"))
        {
            juce::File out(path);
            out.replaceWithData(sdf.rgba.data(), sdf.rgba.size());
            std::printf("  wrote %s\n", out.getFullPathName().toRawUTF8());
        }
    }

    //==========================================================================
    // Offline render mode.

    struct RenderOptions
    {
        juce::StringArray presets, only, without;
        juce::String order;
        juce::StringArray sets;    // "id=value" (real value or choice name) or "id=@0.75" (knob position 0..1)
        juce::File metrics;        // --metrics: a JSON file of metrics per render
        juce::String sweepId;      // --sweep id from to steps
        double sweepFrom = 0.0, sweepTo = 0.0;
        int sweepSteps = 0;
        bool sweepNorm = false;    // --norm: the sweep's from/to are knob positions 0..1
        bool writeWav = true;      // --no-wav
        bool rawOutput = false;    // --raw-out: OUTPUT's filters, auto gain and Ride off (module-level measurements)
        int rollFrom = 0, rollTo = -1;   // --roll from to: one render per seed, ROLL ALL (seeded) after the sets
    };

    /** Sets id from "value", "@knob" (0..1) or a choice name; false (and a message) when it cannot. */
    bool setFromText(RoarschachAudioProcessor& p, const juce::String& id, const juce::String& text)
    {
        auto* param = p.getValueTreeState().getParameter(id);
        if (param == nullptr)
        {
            std::printf("no parameter \"%s\"\n", id.toRawUTF8());
            return false;
        }
        if (text.startsWithChar('@'))
        {
            param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, text.substring(1).getFloatValue()));
            return true;
        }
        if (!text.containsOnly("+-.0123456789eE"))
        {
            for (const auto& spec : Params::all)
                if (id == spec.id && spec.choices != nullptr)
                    for (int c = 0; c < spec.numChoices; ++c)
                        if (text.equalsIgnoreCase(spec.choices[c]))
                        {
                            param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(c)));
                            return true;
                        }
            if (text.equalsIgnoreCase("on") || text.equalsIgnoreCase("off"))
            {
                param->setValueNotifyingHost(text.equalsIgnoreCase("on") ? 1.0f : 0.0f);
                return true;
            }
            std::printf("cannot read \"%s\" as a value of %s\n", text.toRawUTF8(), id.toRawUTF8());
            return false;
        }
        param->setValueNotifyingHost(param->convertTo0to1(text.getFloatValue()));
        return true;
    }

    int renderFiles(const juce::File& input, const juce::File& outDir, RenderOptions opt)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(input));
        if (reader == nullptr)
        {
            std::printf("cannot read %s\n", input.getFullPathName().toRawUTF8());
            return 1;
        }

        // The file's own rate: the render never resamples.
        const double sr = reader->sampleRate;
        const int length = static_cast<int>(reader->lengthInSamples);
        juce::AudioBuffer<float> file(static_cast<int>(reader->numChannels), length);
        reader->read(&file, 0, length, 0, true, true);

        Stereo in;
        in.l.assign(file.getReadPointer(0), file.getReadPointer(0) + length);
        in.r = file.getNumChannels() > 1 ? Signal(file.getReadPointer(1), file.getReadPointer(1) + length) : in.l;
        const bool monoIn = file.getNumChannels() < 2;

        if (opt.presets.isEmpty())
            opt.presets = { "Init" };

        if (opt.writeWav && !outDir.createDirectory())
        {
            std::printf("cannot create %s\n", outDir.getFullPathName().toRawUTF8());
            return 1;
        }

        const auto inStats = stats(in);
        std::printf("input %s: %.2f s @ %.0f Hz (rendered at this rate), %d ch, rms %.1f dBFS, peak %.1f dBFS\n",
                    input.getFileName().toRawUTF8(), length / sr, sr, file.getNumChannels(), toDb(inStats.rms), toDb(inStats.peak));

        // The sweep's values (or one pass without a sweep), or the seeds of --roll.
        const Params::Spec* sweepSpec = nullptr;
        std::vector<double> sweepValues { std::numeric_limits<double>::quiet_NaN() };
        const bool rolling = opt.rollTo >= opt.rollFrom;
        if (rolling)
        {
            if (opt.sweepId.isNotEmpty())
            {
                std::printf("--roll and --sweep cannot be combined\n");
                return 1;
            }
            sweepValues.clear();
            for (int seed = opt.rollFrom; seed <= opt.rollTo; ++seed)
                sweepValues.push_back(seed);
        }
        if (opt.sweepId.isNotEmpty())
        {
            for (const auto& spec : Params::all)
                if (opt.sweepId == spec.id)
                    sweepSpec = &spec;
            if (sweepSpec == nullptr || opt.sweepSteps < 1)
            {
                std::printf("bad sweep: parameter \"%s\", %d steps\n", opt.sweepId.toRawUTF8(), opt.sweepSteps);
                return 1;
            }
            sweepValues.clear();
            for (int i = 0; i < opt.sweepSteps; ++i)
                sweepValues.push_back(opt.sweepSteps == 1 ? opt.sweepFrom
                                      : opt.sweepFrom + (opt.sweepTo - opt.sweepFrom) * i / (opt.sweepSteps - 1));
        }

        const bool wantMetrics = opt.metrics != juce::File();
        std::unique_ptr<RenderMetrics::Reference> reference;
        juce::Array<juce::var> records;
        auto* root = new juce::DynamicObject();
        juce::var rootVar(root);
        if (wantMetrics)
        {
            reference = std::make_unique<RenderMetrics::Reference>(RenderMetrics::makeReference(in.l, in.r, sr));
            auto* inRec = new juce::DynamicObject();
            inRec->setProperty("file", input.getFullPathName());
            inRec->setProperty("sampleRate", sr);
            inRec->setProperty("seconds", length / sr);
            inRec->setProperty("channels", file.getNumChannels());
            inRec->setProperty("sineHz", RenderMetrics::num(reference->sineHz));
            inRec->setProperty("metrics", RenderMetrics::measure(in.l, in.r, *reference, sr,
                                                                 toDb(std::max(truePeak(in.l), truePeak(in.r))), 0));
            root->setProperty("input", juce::var(inRec));
            root->setProperty("thirdOctaveCentresHz", RenderMetrics::numArray(RenderMetrics::thirdOctaveCentres()));
            root->setProperty("amBandEdgesHz", RenderMetrics::numArray(RenderMetrics::modBandEdges()));
            juce::Array<juce::var> setList;
            for (const auto& s : opt.sets)
                setList.add(s);
            root->setProperty("set", setList);
            if (sweepSpec != nullptr)
            {
                auto* sw = new juce::DynamicObject();
                sw->setProperty("param", opt.sweepId);
                sw->setProperty("name", juce::String(sweepSpec->name));
                sw->setProperty("min", sweepSpec->minValue);
                sw->setProperty("max", sweepSpec->maxValue);
                sw->setProperty("from", opt.sweepFrom);
                sw->setProperty("to", opt.sweepTo);
                sw->setProperty("steps", opt.sweepSteps);
                sw->setProperty("norm", opt.sweepNorm);
                root->setProperty("sweep", juce::var(sw));
            }
        }

        int failures = 0;
        for (const auto& name : opt.presets)
        {
            const int index = presetIndex(name);
            if (index < 0)
            {
                std::printf("no factory preset \"%s\"\n", name.toRawUTF8());
                ++failures;
                continue;
            }

            for (const double sweepValue : sweepValues)
            {
                auto p = makeProcessor(sr, 512, monoIn ? 1 : 2, 2);
                p->setCurrentProgram(index);
                juce::String suffix;
                if (!opt.only.isEmpty())
                {
                    for (const auto& m : Params::modules)
                        setParam(*p, m.onId, opt.only.contains(m.code) ? 1.0f : 0.0f);
                    suffix << "-only-" << opt.only.joinIntoString("-").toLowerCase();
                }
                for (const auto& code : opt.without)
                {
                    const int m = Params::moduleIndexForCode(code.toStdString());
                    if (m >= 0)
                        setParam(*p, Params::modules[static_cast<size_t>(m)].onId, 0.0f);
                }
                if (!opt.without.isEmpty())
                    suffix << "-without-" << opt.without.joinIntoString("-").toLowerCase();
                if (opt.order.isNotEmpty())
                {
                    setOrder(*p, orderFromCodes(opt.order));
                    suffix << "-order-" << opt.order.removeCharacters(" ,").toLowerCase();
                }
                for (const auto& s : opt.sets)
                {
                    const auto id = s.upToFirstOccurrenceOf("=", false, false).trim();
                    const auto value = s.fromFirstOccurrenceOf("=", false, false).trim();
                    if (!s.contains("=") || !setFromText(*p, id, value))
                        return 1;
                    suffix << "-" << id << "-" << value.removeCharacters("@");
                }

                juce::RangedAudioParameter* swept = nullptr;
                if (sweepSpec != nullptr)
                {
                    swept = p->getValueTreeState().getParameter(opt.sweepId);
                    if (opt.sweepNorm)
                        swept->setValueNotifyingHost(static_cast<float>(juce::jlimit(0.0, 1.0, sweepValue)));
                    else
                        swept->setValueNotifyingHost(swept->convertTo0to1(static_cast<float>(sweepValue)));
                    suffix << "-" << opt.sweepId << "-" << juce::String(getParam(*p, opt.sweepId.toRawUTF8()), 3);
                }

                int rollSeed = -1;
                if (rolling)
                {
                    rollSeed = static_cast<int>(sweepValue);
                    p->getDice().setSeed(static_cast<std::uint32_t>(rollSeed));
                    p->getDice().rollAll();
                    suffix << "-roll-" << rollSeed;
                }

                if (opt.rawOutput)
                {
                    p->getChainForTests().setOutputTestOptions(false, false, false);
                    suffix << "-raw";
                }
                restart(*p, sr, 512);
                const int latency = p->getLatencySamples();

                // Pad with the latency plus a tail, then drop the latency so the file lines up with the input.
                Stereo padded = in;
                const int extra = latency + static_cast<int>(0.5 * sr);
                padded.l.resize(static_cast<size_t>(length + extra), 0.0f);
                padded.r.resize(static_cast<size_t>(length + extra), 0.0f);
                // OUTPUT's maximizer gain reduction per block (95th percentile over the input's
                // length, after the first blocks of the limiter settling): what the render costs it.
                std::vector<double> gr;
                auto out = render(*p, padded, 512, false, [&](int start)
                {
                    if (start >= 4 * 512 && start < length + latency)
                        gr.push_back(-static_cast<double>(p->getChain().getOutput().getReductionDb()));
                });
                std::sort(gr.begin(), gr.end());
                const double grP95 = gr.empty() ? 0.0 : gr[std::min(gr.size() - 1, static_cast<size_t>(0.95 * static_cast<double>(gr.size())))];
                out.l.erase(out.l.begin(), out.l.begin() + latency);
                out.r.erase(out.r.begin(), out.r.begin() + latency);

                const auto st = stats(out);
                double sideSq = 0.0, midSq = 0.0;
                for (size_t i = 0; i < out.l.size(); ++i)
                {
                    const double mid = 0.5 * (out.l[i] + out.r[i]), side = 0.5 * (out.l[i] - out.r[i]);
                    sideSq += side * side;
                    midSq += mid * mid;
                }
                const double tpDb = toDb(std::max(truePeak(out.l), truePeak(out.r)));
                const auto clamps = p->getChain().getClampCount();

                const juce::File target = outDir.getChildFile((name + suffix).replaceCharacter(' ', '-').replaceCharacter('&', 'n').toLowerCase() + ".wav");
                if (opt.writeWav)
                {
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
                    else
                    {
                        std::printf("cannot write %s\n", target.getFullPathName().toRawUTF8());
                        ++failures;
                    }
                }

                const float ceiling = ceilingGain(getParam(*p, Params::outCeiling));
                const bool broken = !st.finite || st.peak > ceiling || std::abs(st.mean) > 0.01;
                failures += broken ? 1 : 0;

                juce::String metricLine;
                if (wantMetrics)
                {
                    // Measured over the input's length (aligned with it); the tail is in the file only.
                    const Signal l(out.l.begin(), out.l.begin() + length), r(out.r.begin(), out.r.begin() + length);
                    auto m = RenderMetrics::measure(l, r, *reference, sr, tpDb, clamps);
                    auto* rec = new juce::DynamicObject();
                    rec->setProperty("preset", name);
                    rec->setProperty("suffix", suffix);
                    if (swept != nullptr)
                    {
                        rec->setProperty("param", opt.sweepId);
                        rec->setProperty("value", getParam(*p, opt.sweepId.toRawUTF8()));   // the real value after snapping
                        rec->setProperty("norm", swept->getValue());
                        rec->setProperty("text", swept->getCurrentValueAsText());
                    }
                    if (rollSeed >= 0)
                    {
                        rec->setProperty("roll", rollSeed);
                        juce::String on;
                        for (const auto& mod : Params::modules)
                            if (getParam(*p, mod.onId) > 0.5f)
                                on << (on.isEmpty() ? "" : " ") << mod.code;
                        rec->setProperty("on", on);
                        auto* rolled = new juce::DynamicObject();
                        for (const auto& r : Params::Random::ranges)
                            rolled->setProperty(r.id, p->getValueTreeState().getParameter(r.id)->getCurrentValueAsText());
                        rec->setProperty("rolled", juce::var(rolled));
                    }
                    rec->setProperty("latency", latency);
                    rec->setProperty("wav", opt.writeWav ? juce::var(target.getFullPathName()) : juce::var());
                    rec->setProperty("broken", broken);
                    rec->setProperty("metrics", m);
                    rec->setProperty("grP95Db", grP95);
                    records.add(juce::var(rec));
                    metricLine << juce::String::formatted("  %6.1f LUFS  HNR %5.1f dB  env r %.2f",
                                                          static_cast<double>(m["lufs"]), static_cast<double>(m["hnrDb"]),
                                                          static_cast<double>(m["envCorr1to4k"]));
                }

                std::printf("%-18s%s rms %6.1f dBFS  peak %6.2f dBFS  true peak %6.2f dBTP  side/mid %5.1f dB  GR p95 %4.1f dB  clamps %llu  latency %d  %s%s%s%s\n",
                            name.toRawUTF8(), suffix.toRawUTF8(), toDb(st.rms), toDb(st.peak), tpDb,
                            toDb(std::sqrt(sideSq / std::max(midSq, 1.0e-20))), grP95, static_cast<unsigned long long>(clamps),
                            latency, broken ? "BROKEN" : "ok", metricLine.toRawUTF8(),
                            opt.writeWav ? " -> " : "", opt.writeWav ? target.getFullPathName().toRawUTF8() : "");
            }
        }

        if (wantMetrics)
        {
            root->setProperty("records", records);
            opt.metrics.getParentDirectory().createDirectory();
            if (!opt.metrics.replaceWithText(juce::JSON::toString(rootVar)))
            {
                std::printf("cannot write %s\n", opt.metrics.getFullPathName().toRawUTF8());
                ++failures;
            }
            else
            {
                std::printf("metrics -> %s\n", opt.metrics.getFullPathName().toRawUTF8());
            }
        }
        return failures == 0 ? 0 : 1;
    }

}

namespace
{
    // ---------------------------------------------------------------------------------------------
    // Knob laws (dsp/KnobLaws.h): tapers, laws, conversions, state migration, preset conversion.

    /** n -> value of a parameter through the processor's own range (the knob's taper). */
    float valueAtKnob(RoarschachAudioProcessor& p, const char* id, float n)
    {
        return p.getValueTreeState().getParameter(id)->convertFrom0to1(n);
    }

    // The first release's factory preset values for the remapped knobs (the conversion source).
    struct V1Preset
    {
        const char* name;
        bool roarOn;
        // contain, gate, deess, low, high, gravel, shimmer, swirl, ensemble, width, growl, roar,
        // roarTone, tame, sub, subDrive, overtone, push, ceiling, clip, clipShape, attack
        std::array<float, 22> v;
    };

    // PITCH Drive per factory preset in the first release (its knob was the drive amount itself).
    const std::map<std::string, float>& v1PitchDrive()
    {
        static const std::map<std::string, float> t {
            { "Init", 5.0f },
            { "Symbiote", 20.0f },
            { "Liquid Beast", 25.0f },
            { "Gravel Throat", 45.0f },
            { "Cave Troll", 25.0f },
            { "Kaiju", 50.0f },
            { "Gargoyle", 25.0f },
            { "Hellhound", 60.0f },
            { "Golem", 25.0f },
            { "Kraken", 25.0f },
            { "Throat Singer", 25.0f },
            { "Big Bad Narrator", 30.0f },
            { "Demon Whisper", 25.0f },
            { "Giant", 25.0f },
            { "Possessed", 25.0f },
            { "Ogre Radio", 25.0f },
            { "Cyber Commander", 40.0f },
            { "Respirator Rasp", 30.0f },
            { "Rasp Machine", 55.0f },
            { "Robot Ogre", 25.0f },
            { "Scorched Swirl", 5.0f },
            { "Molten Chorus", 5.0f },
            { "Sub Furnace", 5.0f },
            { "Ribcage", 5.0f },
            { "Tar Pit", 25.0f },
            { "Breathing Walls", 5.0f },
            { "Chopped Space", 5.0f },
            { "Vocoded Fire", 5.0f },
            { "Clanking Choir", 5.0f },
            { "Pulse Choir", 5.0f },
            { "Two Octaves Down", 5.0f },
            { "Gravel Pit", 5.0f },
            { "Iron Lung", 5.0f },
            { "Detail Only", 25.0f },
            { "Subtle Size", 15.0f },
            { "Clean Lift", 25.0f },
            { "Loud & Contained", 5.0f },
            { "Crunch Lift", 5.0f },
            { "Smashed", 5.0f },
            { "Phone Monster", 5.0f }
        };
        return t;
    }

    const std::vector<V1Preset>& v1Presets()
    {
        static const std::vector<V1Preset> t {
            { "Init", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 18.0f, 12.0f, 48.0f, 45.0f, 10.0f, 30.0f, 45.0f, 60.0f, 10.0f, 8.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Symbiote", true, { 40.0f, 45.0f, 40.0f, 30.0f, 25.0f, 50.0f, 25.0f, 32.0f, 30.0f, 15.0f, 55.0f, 50.0f, 0.0f, 35.0f, 55.0f, 40.0f, 10.0f, 8.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Liquid Beast", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 40.0f, 50.0f, 60.0f, 70.0f, 40.0f, 40.0f, 0.0f, 30.0f, 55.0f, 50.0f, 10.0f, 7.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Gravel Throat", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 45.0f, 12.0f, 30.0f, 18.0f, 12.0f, 55.0f, 50.0f, 0.0f, 30.0f, 50.0f, 50.0f, 35.0f, 7.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Cave Troll", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 15.0f, 30.0f, 30.0f, 10.0f, 50.0f, 45.0f, -30.0f, 30.0f, 70.0f, 50.0f, 40.0f, 7.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Kaiju", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 40.0f, 12.0f, 30.0f, 18.0f, 12.0f, 60.0f, 65.0f, -20.0f, 70.0f, 80.0f, 65.0f, 40.0f, 7.0f, -1.0f, 4.0f, 0.0f, 2.0f } },
            { "Gargoyle", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 50.0f, 18.0f, 12.0f, 35.0f, 60.0f, 40.0f, 30.0f, 50.0f, 50.0f, 10.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Hellhound", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 18.0f, 12.0f, 70.0f, 70.0f, 20.0f, 30.0f, 45.0f, 70.0f, 35.0f, 8.0f, -2.0f, 3.0f, 60.0f, 0.5f } },
            { "Golem", true, { 35.0f, 35.0f, 30.0f, 60.0f, 35.0f, 50.0f, 12.0f, 30.0f, 18.0f, 12.0f, 30.0f, 40.0f, 0.0f, 30.0f, 60.0f, 50.0f, 10.0f, 6.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Kraken", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 60.0f, 55.0f, 70.0f, 90.0f, 20.0f, 45.0f, 10.0f, 30.0f, 50.0f, 50.0f, 10.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Throat Singer", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 18.0f, 12.0f, 40.0f, 45.0f, 10.0f, 30.0f, 60.0f, 50.0f, 55.0f, 7.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Big Bad Narrator", true, { 35.0f, 35.0f, 30.0f, 70.0f, 40.0f, 50.0f, 12.0f, 20.0f, 18.0f, 12.0f, 15.0f, 30.0f, 0.0f, 30.0f, 40.0f, 50.0f, 10.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Demon Whisper", true, { 50.0f, 15.0f, 70.0f, 60.0f, 40.0f, 50.0f, 35.0f, 30.0f, 35.0f, 60.0f, 48.0f, 45.0f, 40.0f, 30.0f, 25.0f, 50.0f, 10.0f, 8.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Giant", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 35.0f, 25.0f, 0.0f, 45.0f, 30.0f, -30.0f, 30.0f, 65.0f, 50.0f, 10.0f, 6.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Possessed", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 35.0f, 30.0f, 30.0f, 18.0f, 12.0f, 40.0f, 40.0f, 0.0f, 30.0f, 50.0f, 50.0f, 10.0f, 7.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Ogre Radio", true, { 50.0f, 45.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 35.0f, 15.0f, 48.0f, 70.0f, 50.0f, 30.0f, 20.0f, 50.0f, 10.0f, 8.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Cyber Commander", true, { 55.0f, 40.0f, 60.0f, 65.0f, 35.0f, 50.0f, 25.0f, 35.0f, 35.0f, 15.0f, 20.0f, 45.0f, 30.0f, 55.0f, 50.0f, 50.0f, 30.0f, 9.0f, -2.5f, 3.0f, 40.0f, 2.0f } },
            { "Respirator Rasp", true, { 50.0f, 20.0f, 35.0f, 85.0f, 40.0f, 50.0f, 15.0f, 35.0f, 35.0f, 15.0f, 35.0f, 35.0f, 10.0f, 30.0f, 50.0f, 50.0f, 30.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Rasp Machine", true, { 60.0f, 45.0f, 55.0f, 55.0f, 45.0f, 50.0f, 25.0f, 35.0f, 35.0f, 15.0f, 25.0f, 65.0f, 40.0f, 60.0f, 50.0f, 50.0f, 30.0f, 9.0f, -3.0f, 6.0f, 70.0f, 0.5f } },
            { "Robot Ogre", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 80.0f, 12.0f, 30.0f, 18.0f, 12.0f, 40.0f, 45.0f, 10.0f, 30.0f, 50.0f, 50.0f, 10.0f, 7.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Scorched Swirl", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 55.0f, 18.0f, 12.0f, 48.0f, 50.0f, 0.0f, 30.0f, 50.0f, 50.0f, 10.0f, 7.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Molten Chorus", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 55.0f, 80.0f, 48.0f, 55.0f, 0.0f, 30.0f, 50.0f, 50.0f, 10.0f, 8.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Sub Furnace", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 18.0f, 12.0f, 48.0f, 60.0f, -30.0f, 30.0f, 65.0f, 75.0f, 10.0f, 5.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Ribcage", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 18.0f, 12.0f, 48.0f, 45.0f, -10.0f, 30.0f, 60.0f, 50.0f, 60.0f, 6.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Tar Pit", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 18.0f, 12.0f, 50.0f, 50.0f, 0.0f, 30.0f, 50.0f, 50.0f, 10.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Breathing Walls", true, { 35.0f, 35.0f, 30.0f, 80.0f, 30.0f, 50.0f, 12.0f, 30.0f, 18.0f, 12.0f, 48.0f, 45.0f, 10.0f, 30.0f, 50.0f, 50.0f, 10.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Chopped Space", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 35.0f, 30.0f, 18.0f, 12.0f, 50.0f, 45.0f, 10.0f, 30.0f, 50.0f, 50.0f, 10.0f, 8.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Vocoded Fire", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 55.0f, 12.0f, 30.0f, 18.0f, 12.0f, 48.0f, 55.0f, 0.0f, 30.0f, 50.0f, 50.0f, 10.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Clanking Choir", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 60.0f, 90.0f, 48.0f, 45.0f, 10.0f, 30.0f, 50.0f, 50.0f, 10.0f, 11.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Pulse Choir", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 70.0f, 12.0f, 30.0f, 60.0f, 100.0f, 45.0f, 20.0f, 0.0f, 30.0f, 30.0f, 50.0f, 10.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Two Octaves Down", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 75.0f, 12.0f, 30.0f, 18.0f, 12.0f, 48.0f, 45.0f, 10.0f, 30.0f, 60.0f, 50.0f, 10.0f, 7.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Gravel Pit", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 90.0f, 12.0f, 30.0f, 18.0f, 12.0f, 50.0f, 55.0f, 0.0f, 30.0f, 50.0f, 50.0f, 10.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Iron Lung", true, { 35.0f, 35.0f, 30.0f, 80.0f, 35.0f, 60.0f, 50.0f, 50.0f, 18.0f, 12.0f, 48.0f, 45.0f, 10.0f, 30.0f, 50.0f, 50.0f, 10.0f, 7.0f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Detail Only", false, { 50.0f, 45.0f, 50.0f, 70.0f, 35.0f, 50.0f, 25.0f, 35.0f, 35.0f, 15.0f, 45.0f, 35.0f, 0.0f, 50.0f, 50.0f, 50.0f, 30.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Subtle Size", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 15.0f, 20.0f, 20.0f, 15.0f, 25.0f, 20.0f, 0.0f, 30.0f, 35.0f, 50.0f, 20.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Clean Lift", false, { 35.0f, 35.0f, 30.0f, 55.0f, 30.0f, 50.0f, 25.0f, 35.0f, 35.0f, 15.0f, 45.0f, 35.0f, 0.0f, 30.0f, 50.0f, 50.0f, 30.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
            { "Loud & Contained", true, { 70.0f, 45.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 18.0f, 12.0f, 48.0f, 45.0f, 10.0f, 70.0f, 50.0f, 50.0f, 10.0f, 10.5f, -1.0f, 4.0f, 0.0f, 2.0f } },
            { "Crunch Lift", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 18.0f, 12.0f, 48.0f, 45.0f, 10.0f, 30.0f, 50.0f, 50.0f, 10.0f, 10.0f, -1.0f, 3.0f, 60.0f, 0.5f } },
            { "Smashed", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 18.0f, 12.0f, 48.0f, 45.0f, 10.0f, 30.0f, 50.0f, 50.0f, 10.0f, 13.0f, -1.0f, 9.0f, 100.0f, 0.05f } },
            { "Phone Monster", true, { 35.0f, 35.0f, 30.0f, 30.0f, 25.0f, 50.0f, 12.0f, 30.0f, 18.0f, 12.0f, 48.0f, 45.0f, 10.0f, 30.0f, 55.0f, 50.0f, 60.0f, 7.5f, -1.0f, 0.0f, 0.0f, 2.0f } },
        };
        return t;
    }

    /** True when f is non-decreasing over u in 0..1, with the given end values and real movement
        in the first and last 10 % of the travel (no dead bottom or top). */
    bool lawShape(const std::function<double(double)>& f, double at0, double at1, double tol, juce::String& why)
    {
        double prev = f(0.0);
        for (int i = 1; i <= 1000; ++i)
        {
            const double y = f(i / 1000.0);
            if (y < prev - 1e-12)
            {
                why = "not monotonic at " + juce::String(i / 1000.0);
                return false;
            }
            prev = y;
        }
        if (std::abs(f(0.0) - at0) > tol || std::abs(f(1.0) - at1) > tol)
        {
            why = "ends " + juce::String(f(0.0)) + " .. " + juce::String(f(1.0));
            return false;
        }
        const double span = std::abs(at1 - at0);
        const double bottom = std::abs(f(0.1) - f(0.0)) / span, top = std::abs(f(1.0) - f(0.9)) / span;
        if (bottom < 0.02 || top < 0.02)
        {
            why = "first / last 10 % move " + juce::String(bottom * 100.0, 1) + " % / " + juce::String(top * 100.0, 1) + " % of the span";
            return false;
        }
        return true;
    }

    void testKnobLaws()
    {
        std::printf("[laws] log tapers, knob laws, conversions, state migration, preset conversion, DC, Vibrato, Rattle\n");
        namespace laws = roar::laws;
        auto p = makeProcessor(48000.0, 512);

        // Log tapers: the geometric centre at mid travel, equal travel per octave, n -> v -> n.
        for (const auto& spec : Params::all)
        {
            if (!Params::hasLogTaper(spec))
                continue;
            auto* param = p->getValueTreeState().getParameter(spec.id);
            const float mid = param->convertFrom0to1(0.5f);
            const float centre = std::sqrt(spec.minValue * spec.maxValue);
            check(std::abs(mid - centre) <= 0.005f * centre + spec.step, juce::String(spec.id) + " mid travel " + juce::String(mid) + " (sqrt (min max) " + juce::String(centre) + ")");
            int worst = 0;
            for (int i = 0; i <= 20; ++i)
            {
                const float n = static_cast<float>(i) / 20.0f;
                const float v = param->convertFrom0to1(n);
                const float back = param->convertTo0to1(v);
                const float stepN = spec.step > 0.0f ? param->convertTo0to1(std::min(spec.maxValue, v + spec.step)) - back : 0.0f;
                if (std::abs(back - n) > 1e-4f + std::abs(stepN))
                    ++worst;
            }
            check(worst == 0, juce::String(spec.id) + ": " + juce::String(worst) + " knob positions do not round trip");
            // Equal travel per octave: the first 20 % of the travel covers 20 % of the octaves.
            const double octaves = std::log2(static_cast<double>(spec.maxValue / spec.minValue));
            const double first = std::log2(static_cast<double>(param->convertFrom0to1(0.2f) / spec.minValue));
            check(std::abs(first / octaves - 0.2) < 0.01, juce::String(spec.id) + ": the first 20 % covers " + juce::String(100.0 * first / octaves, 1) + " % of the octaves");
        }
        check(std::abs(valueAtKnob(*p, Params::shimmerRate, 1.0f) - 25.0f) < 1e-3f && std::abs(valueAtKnob(*p, Params::growlRate, 0.0f) - 2.0f) < 1e-3f
                  && std::abs(valueAtKnob(*p, Params::outAttack, 0.0f) - 0.2f) < 1e-4f,
              "Shimmer Rate tops at 25 Hz, Growl Rate starts at 2 Hz, Attack at 0.2 ms");
        // Push and Clip: skewed, 10 dB and 5 dB at mid travel.
        check(std::abs(valueAtKnob(*p, Params::outPush, 0.5f) - 10.0f) < 0.06f, "Push: 10 dB at mid travel");
        check(std::abs(valueAtKnob(*p, Params::outClip, 0.5f) - 5.0f) < 0.06f, "Clip: 5 dB at mid travel");

        // Each forward law: monotonic, the documented ends, and no dead bottom or top.
        struct Law { const char* name; std::function<double(double)> f; double at0, at1; };
        const std::vector<Law> forward {
            { "contain", laws::containCurve, 0.0, 1.0 },
            { "gate depth", [](double u) { return -laws::gateDepthDb(u); }, 0.0, 30.0 },
            { "gate ratio", laws::gateRatio, 1.5, 5.0 },
            { "de-ess range", [](double u) { return -laws::deessRangeDb(u); }, 0.0, 24.0 },
            { "de-ess threshold shift", laws::deessThresholdShiftDb, 0.0, 12.0 },
            { "detail depth", laws::detailDepthDb, 0.0, 34.0 },
            { "gravel", laws::gravelAmount, 0.0, 1.0 },
            { "ring AM", laws::ringAmAmount, 0.0, 1.0 },
            { "shimmer", laws::shimmerAmount, 0.0, 0.65 },
            { "growl depth", laws::growlDepth, 0.0, 1.0 - std::pow(10.0, -18.0 / 20.0) },
            { "swirl", laws::swirlMix, 0.0, 1.0 },
            { "roar scoop", [](double a) { return -laws::roarScoopDb(a); }, 0.0, 7.0 },
            { "roar drive", laws::roarDrive1Db, 0.0, 30.0 },
            { "ensemble", laws::ensembleAmount, 0.0, 0.7 },
            { "vibrato", [](double u) { return laws::ensembleVibratoAmount(laws::ensembleAmount(u)); }, 0.0, std::sqrt(0.7) },
            { "width", [](double u) { return laws::ensembleSideGain(1.5 * u); }, 0.0, 0.85914057 },
            { "tame", laws::tameT, 0.0, 2.2 },
            { "pitch drive (gain dB)", [](double u) { const double d = laws::pitchDriveAmount(u); return 24.0 * d * std::sqrt(d); }, 0.0, 24.0 },
            { "gate threshold offset", laws::gateThresholdOffsetDb, 6.0, 18.0 },
            { "overtone add (gain)", laws::overtoneAddGain, 0.0, std::pow(10.0, 14.8 / 20.0) },
            { "sub level", laws::subLevelDb, -14.0, 10.0 },
            { "overtone", laws::overtoneLevelDb, -10.0, 10.0 },
            { "overtone low cut", laws::overtoneLowCut, 0.0, 1.0 },
            { "clip knee", [](double s) { return -laws::clipKnee(s, 0.2); }, -0.2, 0.0 },
            { "auto fast release (log)", [](double u) { return std::log(laws::autoFastReleaseMs(std::pow(1000.0, u))); }, std::log(3.0), std::log(250.0) },
            { "mix wet", laws::mixWetGain, 0.0, 1.0 },
        };
        for (const auto& law : forward)
        {
            juce::String why;
            // The roar scoop and the overtone low cut reach their end early on purpose (flat from
            // 60 % and 25 %), the equal-power mix flattens at 100 % by design (sin), and the de-ess
            // threshold shift starts at 40 % (the range alone does the first part): only their
            // monotonic ends are checked.
            const juce::String n(law.name);
            const bool flatTop = n == "roar scoop" || n == "overtone low cut" || n == "mix wet" || n == "de-ess threshold shift";
            bool ok = true;
            if (flatTop)
            {
                double prev = law.f(0.0);
                for (int i = 1; i <= 1000 && ok; ++i)
                {
                    const double y = law.f(i / 1000.0);
                    ok = y >= prev - 1e-12;
                    prev = y;
                }
                ok = ok && std::abs(law.f(0.0) - law.at0) < 1e-6 && std::abs(law.f(1.0) - law.at1) < 1e-6;
            }
            else
                ok = lawShape(law.f, law.at0, law.at1, 1e-6, why);
            check(ok, juce::String("law ") + law.name + " " + why);
        }
        // The scoop is C1 (no kink at 40 % or 60 %) and matches the previous -14 a up to 40 %.
        {
            const double h = 1e-6;
            const auto slope = [h](double a) { return (laws::roarScoopDb(a + h) - laws::roarScoopDb(a - h)) / (2.0 * h); };
            check(std::abs(slope(0.4) - (-14.0)) < 1e-3 && std::abs(slope(0.6)) < 1e-3 && std::abs(laws::roarScoopDb(0.3) + 4.2) < 1e-9,
                  "roar scoop: -14 a to 40 %, smooth turn, flat -7 dB from 60 %");
        }
        // Clip Shape acts over its whole travel at any Clip (no dead span below 1 - 2 cap).
        for (const double clipDb : { 1.0, 3.0, 6.0 })
        {
            const double cap = 1.0 - std::pow(10.0, -clipDb / 20.0);
            check(laws::clipKnee(0.0, cap) - laws::clipKnee(0.25, cap) > 0.1 * laws::clipKnee(0.0, cap),
                  "Clip Shape moves the knee from 0 % at Clip " + juce::String(clipDb));
            check(laws::clipKnee(0.75, cap) - laws::clipKnee(1.0, cap) > 0.1 * laws::clipKnee(0.0, cap),
                  "Clip Shape still moves the knee in its last 25 % at Clip " + juce::String(clipDb));
        }
        // Auto release's fast stage: 3 ms at 1 ms, 15 ms at 60 ms, R / 4 above, continuous.
        check(std::abs(laws::autoFastReleaseMs(1.0) - 3.0) < 1e-9 && std::abs(laws::autoFastReleaseMs(60.0) - 15.0) < 1e-9
                  && std::abs(laws::autoFastReleaseMs(400.0) - 100.0) < 1e-9 && laws::autoFastReleaseMs(3.0) > laws::autoFastReleaseMs(2.0) + 0.3,
              "Auto fast release: 3 ms at 1 ms, 15 ms at 60 ms, a step for every Release");

        // Conversions: the new law at the converted value gives the previous law's value.
        {
            int bad = 0;
            const auto near = [&bad](double a, double b, double tol, const char* what, double old)
            {
                if (std::abs(a - b) > tol)
                {
                    ++bad;
                    std::printf("    %s at %.1f: %.5f vs %.5f\n", what, old, a, b);
                }
            };
            namespace cv = laws::convert;
            for (int i = 0; i <= 100; ++i)
            {
                const double old = i, u = old / 100.0;
                near(laws::containCurve(cv::contain(old) / 100.0), laws::v1::containCurve(u), 1e-6, "contain", old);
                near(laws::detailDepthDb(cv::detail(old) / 100.0), std::min(laws::v1::detailDepthDb(u), 34.0), 1e-6, "detail", old);
                near(laws::gravelAmount(cv::gravel(old) / 100.0), laws::v1::gravelAmount(u), 1e-6, "gravel", old);
                near(laws::ringAmAmount(cv::ringAm(old) / 100.0), laws::v1::ringAmAmount(u), 1e-6, "ring AM", old);
                near(laws::shimmerAmount(cv::shimmer(old) / 100.0), std::min(laws::v1::shimmerAmount(u), 0.65), 1e-6, "shimmer", old);
                near(laws::growlDepth(cv::growl(old) / 100.0), std::min(laws::v1::growlDepth(u), laws::growlDepth(1.0)), 1e-6, "growl", old);
                near(laws::swirlMix(cv::swirl(old) / 100.0), laws::v1::swirlMix(u), 1e-6, "swirl", old);
                near(laws::ensembleAmount(cv::ensemble(old, false) / 100.0), std::min(laws::v1::ensembleAmount(u), 0.7), 1e-6, "ensemble", old);
                near(laws::tameT(cv::tame(old) / 100.0), laws::v1::tameT(u), 1e-6, "tame", old);
                near(laws::pitchDriveAmount(cv::pitchDrive(old) / 100.0), u, 1e-6, "pitch drive", old);
                if (i > 0)
                    near(laws::subLevelDb(cv::sub(old) / 100.0), std::min(laws::v1::subLevelDb(u), 10.0), 1e-6, "sub", old);
                // Overtone (with the 2 % fade-ins), as linear gains.
                {
                    const auto gainFor = [](double (*law)(double), double x)
                    {
                        if (x <= 0.0)
                            return 0.0;
                        return x < 0.02 ? std::pow(10.0, law(0.02) / 20.0) * x / 0.02 : std::pow(10.0, law(x) / 20.0);
                    };
                    near(gainFor(laws::overtoneLevelDb, cv::overtone(old, false) / 100.0), gainFor(laws::v1::overtoneLevelDb, u), 1e-6, "overtone", old);
                    near(laws::overtoneAddGain(cv::overtone(old, true) / 100.0), gainFor(laws::v1::overtoneLevelDb, u), 1e-6, "overtone (add)", old);
                }
                // Gate: the room-tone cut matches up to the 15 dB the previous law could reach.
                near(laws::gateDepthDb(cv::gate(old) / 100.0), -std::min(-laws::v1::gateDepthDb(u), 15.0), 1e-6, "gate", old);
                // De-ess: the previous range (with the ROAR link) where the laws agree (up to 6 dB, 40 %);
                // beyond, a measured map that never goes past the previous reduction's ceiling.
                for (const double drive : { 0.0, 50.0, 100.0 })
                {
                    const double r = laws::v1::deessRangeDb(u, true, drive / 100.0), v = cv::deess(old, true, drive);
                    if (r >= -6.0)
                        near(laws::deessRangeDb(v / 100.0), r, 1e-6, "de-ess", old);
                    else if (! (v > 40.0 && v <= 55.61))
                        near(v, 48.0, 7.6, "de-ess (measured map)", old);
                }
                // Roar: the first stage's drive matches (up to the previous top).
                near(laws::roarDrive1Db(cv::roarDrive(old) / 100.0), laws::v1::roarDrive1Db(u), 1e-6, "roar drive", old);
                // Width (up to the new 150 %, which is the previous 105 %).
                if (old <= 105.0)
                    near(laws::ensembleSideGain(cv::width(old) / 100.0), laws::v1::ensembleSideGain(u), 1e-6, "width", old);
            }
            for (int t = -100; t <= 100; t += 10)
                for (const double drive : { 10.0, 45.0, 70.0 })
                {
                    const double nd = laws::convert::roarDrive(drive);
                    near(laws::roarToneStrength(nd / 100.0) * laws::convert::roarTone(t, drive, nd) / 100.0,
                         laws::v1::roarToneStrength(drive / 100.0) * t / 100.0, 1e-6, "roar tone", t);
                }
            for (int s = 0; s <= 100; s += 10)
                for (const double clipDb : { 1.0, 3.0, 9.0 })
                {
                    const double cap = 1.0 - std::pow(10.0, -clipDb / 20.0);
                    near(laws::clipKnee(laws::convert::clipShape(s, clipDb) / 100.0, cap), laws::v1::clipKnee(s / 100.0, cap), 1e-6, "clip shape", s);
                }
            near(laws::pushGainDb(laws::convert::push(8.0, -2.0), -2.0), 8.0, 1e-9, "push", 8.0);
            check(bad == 0, juce::String(bad) + " conversions do not reproduce the previous law");
        }

        // State migration: a first-release state (stateVersion 2) loads converted; a current one is untouched.
        {
            auto src = makeProcessor(48000.0, 512);
            setValues(*src, { { Params::cleanContain, 35.0f }, { Params::growl, 48.0f }, { Params::shimmer, 12.0f }, { Params::outPush, 8.0f },
                              { Params::outCeiling, -3.0f }, { Params::subDrive, 60.0f }, { Params::roar, 70.0f }, { Params::roarTone, 20.0f },
                              { Params::ringMode, 1.0f }, { Params::ring, 50.0f }, { Params::ensembleMotion, 3.0f }, { Params::ensemble, 18.0f },
                              { Params::inAuto, 1.0f }, { Params::inTrim, 6.0f } });
            juce::MemoryBlock current;
            src->getStateInformation(current);
            auto xml = juce::AudioProcessor::getXmlFromBinary(current.getData(), static_cast<int>(current.getSize()));
            check(xml != nullptr && xml->getIntAttribute("stateVersion") == 3, "states are written as version 3");

            auto same = makeProcessor(48000.0, 512);
            same->setStateInformation(current.getData(), static_cast<int>(current.getSize()));
            check(std::abs(getParam(*same, Params::growl) - 48.0f) < 1e-3f && std::abs(getParam(*same, Params::outPush) - 8.0f) < 1e-3f,
                  "a current state is not converted");

            xml->setAttribute("stateVersion", 2);
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary(*xml, old);
            auto q = makeProcessor(48000.0, 512);
            q->setStateInformation(old.getData(), static_cast<int>(old.getSize()));
            const auto expect = [&q](const char* id, double value, double tol)
            {
                const float v = getParam(*q, id);
                check(std::abs(static_cast<double>(v) - value) <= tol, juce::String("v2 state: ") + id + " " + juce::String(v) + " (expected " + juce::String(value) + ")");
            };
            namespace cv = laws::convert;
            expect(Params::cleanContain, cv::contain(35.0), 0.06);
            expect(Params::growl, cv::growl(48.0), 0.06);
            expect(Params::shimmer, cv::shimmer(12.0), 0.06);
            expect(Params::outPush, 10.0, 0.06);                          // 8 dB at a -3 dB ceiling
            expect(Params::subDrive, laws::subDriveNeutral, 0.06);
            expect(Params::roar, cv::roarDrive(70.0), 0.06);
            expect(Params::roarTone, cv::roarTone(20.0, 70.0, cv::roarDrive(70.0)), 0.06);
            expect(Params::ring, 25.0, 0.06);                             // AM: old^2
            expect(Params::ensemble, cv::ensemble(18.0, true), 0.06);     // Vibrato: the previous 70 % wet
            expect(Params::inTrim, 0.0, 0.06);                            // inside Auto Level's old dead window
            expect(Params::cleanGate, cv::gate(Params::defaultOf(Params::cleanGate)), 0.06);   // a default that was written too
        }

        // The converter user presets and states share: values it has no rule for, and missing
        // values, are left alone; Ring Amount converts only in AM mode.
        {
            std::map<std::string, double> v { { "growl", 48.0 }, { "tame", 50.0 }, { "pitch", -7.0 }, { "ring", 50.0 } };
            laws::migrateFromV1(v);
            check(std::abs(v["growl"] - laws::convert::growl(48.0)) < 1e-9 && std::abs(v["tame"] - laws::convert::tame(50.0)) < 1e-9 && std::abs(v["pitch"] + 7.0) < 1e-9
                      && v["ring"] == 50.0 && v.count("subDrive") == 0,
                  "migrateFromV1 converts only what it has rules for");
        }

        // Factory presets: 1.1 redesigns them on the new laws (see docs/parameter-audit.md), so only
        // Init (whose values are the parameter defaults) is still the conversion of the first
        // release's. Every first-release preset keeps its name.
        {
            const auto& presets = PresetManager::getFactoryPresets();
            int bad = 0;
            namespace cv = laws::convert;
            for (const auto& old : v1Presets())
            {
                const int i = presetIndex(old.name);
                check(i >= 0, juce::String("a first-release preset is missing: ") + old.name);
                if (i < 0 || juce::String(old.name) != "Init")
                    continue;
                const auto& values = presets[static_cast<size_t>(i)].values;
                const auto get = [&values](const char* id) { return static_cast<double>(values.at(id)); };
                const auto& o = old.v;
                const auto expect = [&](const char* id, double value)
                {
                    if (std::abs(get(id) - value) > 0.51)
                    {
                        ++bad;
                        std::printf("    %s %s: %.2f, expected %.2f\n", old.name, id, get(id), value);
                    }
                };
                expect(Params::cleanContain, cv::contain(o[0]));
                expect(Params::cleanGate, cv::gate(o[1]));
                expect(Params::deess, cv::deess(o[2], old.roarOn, o[11]));
                expect(Params::detailLow, cv::detail(o[3]));
                expect(Params::detailHigh, cv::detail(o[4]));
                expect(Params::gravel, cv::gravel(o[5]));
                expect(Params::shimmer, cv::shimmer(o[6]));
                expect(Params::swirl, cv::swirl(o[7]));
                expect(Params::ensemble, cv::ensemble(o[8], false));
                expect(Params::ensembleWidth, cv::width(o[9]));
                expect(Params::growl, cv::growl(o[10]));
                const double drive = cv::roarDrive(o[11]);
                expect(Params::roar, drive);
                expect(Params::roarTone, cv::roarTone(o[12], o[11], std::round(drive)));
                expect(Params::tame, cv::tame(o[13]));
                expect(Params::sub, cv::sub(o[14]));
                expect(Params::subDrive, laws::subDriveNeutral);
                // The previous Overtone value sat in the dead zone: it became 10 on the Lean / Replace
                // law (-5 dB), the same level at about 2-4 % on Add's law.
                expect(Params::overtone, cv::overtoneForGain(std::pow(10.0, laws::overtoneLevelDb(0.1) / 20.0), get(Params::overtoneMode) < 0.5));
                {
                    const auto it = v1PitchDrive().find(old.name);
                    check(it != v1PitchDrive().end(), juce::String("v1 pitch drive table: no ") + old.name);
                    if (it != v1PitchDrive().end())
                        expect(Params::pitchDrive, cv::pitchDrive(it->second));
                }
                expect(Params::outPush, cv::push(o[17], o[18]));
                expect(Params::outClipShape, cv::clipShape(o[20], o[19]));
                expect(Params::outAttack, std::max(0.2, static_cast<double>(o[21])));
            }
            check(bad == 0, juce::String(bad) + " Init values are not the conversion of the first release's");
            check(v1Presets().size() == 40 && presets.size() == 51, "40 first-release presets plus Simple Size, Iron Tyrant, Tin Can Drone, "
                                                                   "Upside Down, Spore Throat, Hoarse Hoarder, Angry Pepper Pot, Ball Bearing, Use Your Words, "
                                                                   "Thing Next Door and Stairwell Stalker");
            // The parameter defaults are Init's values.
            int defaults = 0;
            for (const auto& [id, value] : presets[0].values)
                if (std::abs(Params::defaultOf(id.toStdString()) - value) > 1e-4f && !id.endsWith("Pos"))
                {
                    ++defaults;
                    std::printf("    default of %s is %.2f, Init has %.2f\n", id.toRawUTF8(), Params::defaultOf(id.toStdString()), value);
                }
            check(defaults == 0, juce::String(defaults) + " defaults differ from Init");
        }

        const double sr = 48000.0;
        // DC: a heavy, asymmetric preset pushed to the limit leaves no DC after the clipper.
        {
            auto q = makeProcessor(sr, 512);
            q->setCurrentProgram(presetIndex("Smashed"));
            setParam(*q, Params::outPush, 24.0f);
            const auto out = render(*q, dual(makeVoice(sr, 5.0)), 512);
            // The mean over whole syllable cycles of the test voice (0.45 s each), after the first second.
            const int from = static_cast<int>(sr), count = static_cast<int>(8 * 0.45 * sr);
            double mean = 0.0;
            for (int i = from; i < from + count; ++i)
                mean += static_cast<double>(out.l[static_cast<size_t>(i)]);
            mean /= static_cast<double>(count);
            // (1.0.0 left -0.004 to -0.009 on real recordings here. What remains comes from the
            // safety stage, which must stay exact so the ceiling holds.)
            check(std::abs(mean) < 1e-3, "Smashed at Push 24: DC " + juce::String(mean, 6));
        }

        const auto renderWith = [sr](const Values& values, int seconds10 = 15)
        {
            auto q = makeProcessor(sr, 512);
            setValues(*q, values);
            q->reset();
            return render(*q, dual(makeVoice(sr, seconds10 / 10.0)), 512);
        };
        const auto differenceDb = [](const Stereo& a, const Stereo& b)
        {
            double d = 0.0, e = 0.0;
            for (int i = a.size() / 3; i < a.size(); ++i)
            {
                const double x = a.l[static_cast<size_t>(i)], y = b.l[static_cast<size_t>(i)];
                d += (x - y) * (x - y);
                e += x * x;
            }
            return 10.0 * std::log10(d / std::max(e, 1e-30) + 1e-30);
        };
        // Ensemble Vibrato grows with Amount (it was stuck at 70 % wet from 10 % up).
        {
            const Values base { { Params::ensembleMotion, 3.0f } };
            auto a = base, b = base;
            a.push_back({ Params::ensemble, 20.0f });
            b.push_back({ Params::ensemble, 60.0f });
            const double d = differenceDb(renderWith(a), renderWith(b));
            check(d > -30.0, "Vibrato at 20 % and 60 % differ by " + juce::String(d, 1) + " dB");
        }
        // The top of De-ess keeps taking more (it stopped at the detector's slope near 70 %).
        {
            const double d = differenceDb(renderWith({ { Params::deess, 75.0f } }), renderWith({ { Params::deess, 100.0f } }));
            check(d > -40.0, "De-ess at 75 % and 100 % differ by " + juce::String(d, 1) + " dB");
        }
        // Rattle rattles at the reference level (it was identical to Sine).
        {
            const Values base { { Params::growl, 60.0f }, { Params::growlRate, 20.0f } };
            auto a = base, b = base;
            a.push_back({ Params::growlShape, 0.0f });
            b.push_back({ Params::growlShape, 4.0f });
            const double d = differenceDb(renderWith(a), renderWith(b));
            check(d > -40.0, "Rattle differs from Sine by " + juce::String(d, 1) + " dB at the reference level");
        }
        // Sub Drive 20 is clean; 100 is audibly driven (it was inaudible at every setting).
        {
            const Values base { { Params::sub, 60.0f } };
            auto a = base, b = base, c = base;
            a.push_back({ Params::subDrive, 0.0f });
            b.push_back({ Params::subDrive, 20.0f });
            c.push_back({ Params::subDrive, 100.0f });
            const auto ra = renderWith(a);
            const double clean = differenceDb(ra, renderWith(b)), driven = differenceDb(ra, renderWith(c));
            check(driven > clean + 10.0 && driven > -30.0, "Sub Drive: 20 % vs 0 % " + juce::String(clean, 1) + " dB, 100 % vs 0 % " + juce::String(driven, 1) + " dB");
        }
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc >= 2 && std::string(argv[1]) == "--motion")
    {
        // (just the creature's motion models and the quality governor: quick)
        testMotion();
        std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    if (argc >= 3 && std::string(argv[1]) == "--snapshot")
    {
        // A PNG of the editor at its design size (for a quick look at the layout).
        auto p = makeProcessor(48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());
        editor->setSize(RoarschachAudioProcessorEditor::c_baseWidth, RoarschachAudioProcessorEditor::c_baseHeight);
        const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
        juce::File file(argv[2]);
        file.deleteFile();
        juce::FileOutputStream stream(file);
        juce::PNGImageFormat png;
        const bool ok = stream.openedOk() && png.writeImageToStream(image, stream);
        editor.reset();
        std::printf("%s %s\n", ok ? "wrote" : "could not write", file.getFullPathName().toRawUTF8());
        return ok ? 0 : 1;
    }

    if (argc >= 3 && std::string(argv[1]) == "--snapshots")
        return runSnapshots(juce::File(argv[2]));

    if (argc >= 4 && std::string(argv[1]) == "--render")
    {
        RenderOptions opt;
        for (int i = 4; i < argc; ++i)
        {
            const std::string a(argv[i]);
            const bool hasValue = i + 1 < argc;
            if (a == "--preset" && hasValue)
                opt.presets.add(argv[++i]);
            else if (a == "--only" && hasValue)
                opt.only.add(juce::String(argv[++i]).toUpperCase());
            else if (a == "--without" && hasValue)
                opt.without.add(juce::String(argv[++i]).toUpperCase());
            else if (a == "--order" && hasValue)
                opt.order = argv[++i];
            else if (a == "--set" && hasValue)
                opt.sets.add(argv[++i]);
            else if (a == "--metrics" && hasValue)
                opt.metrics = juce::File::getCurrentWorkingDirectory().getChildFile(argv[++i]);
            else if (a == "--sweep" && i + 4 < argc)
            {
                opt.sweepId = argv[++i];
                opt.sweepFrom = std::atof(argv[++i]);
                opt.sweepTo = std::atof(argv[++i]);
                opt.sweepSteps = std::atoi(argv[++i]);
            }
            else if (a == "--norm")
                opt.sweepNorm = true;
            else if (a == "--roll" && i + 2 < argc)
            {
                opt.rollFrom = std::atoi(argv[++i]);
                opt.rollTo = std::atoi(argv[++i]);
            }
            else if (a == "--no-wav")
                opt.writeWav = false;
            else if (a == "--raw-out")
                opt.rawOutput = true;
            else if (a.rfind("--", 0) == 0)
            {
                std::printf("unknown or incomplete option %s\n", a.c_str());
                return 1;
            }
            else
                opt.presets.add(argv[i]);   // a bare name is a preset
        }
        for (const auto* codes : { &opt.only, &opt.without })
            for (const auto& code : *codes)
                if (Params::moduleIndexForCode(code.toStdString()) < 0)
                {
                    std::printf("unknown module code %s (one of PI CL DS DT VO RM SH GR SW RO EN TA SU OV CH)\n", code.toRawUTF8());
                    return 1;
                }
        if (opt.sweepId.isNotEmpty() && opt.metrics == juce::File())
            opt.metrics = juce::File::getCurrentWorkingDirectory().getChildFile(argv[3]).getChildFile("sweep-" + opt.sweepId + ".json");
        return renderFiles(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]),
                           juce::File::getCurrentWorkingDirectory().getChildFile(argv[3]), opt);
    }

    juce::StringArray selected;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--quick")
            g_quick = true;
        else
            selected.add(argv[i]);
    }

    const std::vector<std::pair<const char*, void (*)()>> tests {
        { "params", testParameters },
        { "laws", testKnobLaws },
        { "latency", testLatency },
        { "null", testNulls },
        { "mix", testMixPhase },
        { "stereo", testStereoSplit },
        { "reorder", testReorderClicks },
        { "maximizer", testMaximizer },
        { "state", testState },
        { "presets", testPresets },
        { "robust", testNonFiniteInput },
        { "blocks", testBlockSizes },
        { "pitch", testNativeRatePitch },
        { "ring", testRingAndOvertone },
        { "chamber", testChamber },
        { "chamberclip", testChamberClip },
        { "editor", testEditor },
        { "ui", testUi },
        { "dice", testDice },
        { "creature", testCreature },
        { "logo", testLogoSdf },
        { "motion", testMotion },
        { "mood", testMood },
       #if defined(__APPLE__)
        { "renderer", testRenderer },
       #endif
        { "cpu", testCpu },
    };

    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& [name, fn] : tests)
        if (selected.isEmpty() || selected.contains(name))
            fn();

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%d checks, %d failed (%.1f s)\n", g_checks, g_failures, seconds);
    return g_failures == 0 ? 0 : 1;
}

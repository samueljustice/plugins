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

#if defined(__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>   // the snapshot harness runs the run loop (before JUCE: it declares Point)
#endif

#include <JuceHeader.h>

#include "CreatureEvolution.h"
#include "LogoSdf.h"
#include "Params.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "PresetManager.h"
#include "dsp/modules/CleanModule.h"
#include "dsp/modules/InModule.h"
#include "dsp/modules/TameModule.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <vector>

// The editor's parts, for the editor test and the snapshot harness (a friend of the editor).
struct RoarschachEditorInspector
{
    static SignalFlowStrip& strip(RoarschachAudioProcessorEditor& e) { return e.m_strip; }
    static MonsterView& monster(RoarschachAudioProcessorEditor& e) { return e.m_monsterView; }
    static juce::Component& content(RoarschachAudioProcessorEditor& e) { return e.m_content; }
    static juce::LookAndFeel& lookAndFeel(RoarschachAudioProcessorEditor& e) { return e.m_lookAndFeel; }
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
        for (int slot = 0; slot < Params::numModules; ++slot)
            setParam(p, Params::modules[static_cast<size_t>(order[static_cast<size_t>(slot)])].posId, static_cast<float>(slot));
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
                const float tolerance = std::max({ wholePercent ? 0.51f : 0.051f, std::abs(plain) * 0.01f, spec.step });
                if (std::abs(back - plain) > tolerance)
                {
                    ++textFailures;
                    std::printf("    %s: %.4f -> \"%s\" -> %.4f\n", spec.id, plain, text.toRawUTF8(), back);
                }
            }
        }
        check(textFailures == 0, juce::String(textFailures) + " text round trips off");

        // Only the log kinds (hertz, ratio, milliseconds) have a skewed taper: the rest are linear,
        // so Pitch's -24..-12 st octave gets a third of the travel.
        for (const auto& spec : Params::all)
        {
            if (spec.kind == Params::Kind::choice || spec.kind == Params::Kind::toggle || spec.kind == Params::Kind::position)
                continue;
            const bool logTaper = spec.kind == Params::Kind::hertz || spec.kind == Params::Kind::ratio || spec.kind == Params::Kind::milliseconds;
            if (logTaper)
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
        check(text(Params::outAttack, 2.0f) == "2.0 ms" && text(Params::outAttack, 0.05f) == "0.05 ms", "Attack text");
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
        for (const char* tone : { "Off", "Voice", "Radio" })
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
                    if (juce::String(tone) != "Off")
                    {
                        // CLEAN at no dynamics, only its tone; the dry gets the tone's high-pass phase.
                        setValues(*p, { { Params::cleanOn, 1.0f }, { Params::cleanContain, 0.0f }, { Params::cleanGate, 0.0f },
                                        { Params::cleanTone, juce::String(tone) == "Voice" ? 1.0f : 3.0f } });
                    }
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
                if (juce::String(tone) != "Off")
                    setValues(*p, { { Params::cleanOn, 1.0f }, { Params::cleanContain, 0.0f }, { Params::cleanGate, 0.0f },
                                    { Params::cleanTone, juce::String(tone) == "Voice" ? 1.0f : 3.0f } });
                setParam(*p, Params::mix, 50.0f);
                restart(*p, sr, 512);
                const auto sine = makeSine(sr, 0.6, hz, 0.1);
                const auto out = render(*p, dual(sine), 512);
                const double half = toneAmplitude(out.l, sr, hz, static_cast<int>(0.3 * sr), out.size());
                const double ideal = 0.5 * (level[0] + level[1]);
                const double lossDb = toDb(half) - toDb(ideal);
                worst = std::min(worst, lossDb);
            }
            std::printf("  CLEAN %-5s: worst 50 %% mix loss %.2f dB (phase mismatch)\n", tone, worst);
            // Radio's band-pass EQ is far from flat, its phase is matched only at the corners.
            check(worst > (juce::String(tone) == "Radio" ? -3.0 : -0.5), juce::String("Mix combs with CLEAN ") + tone);
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
            std::rotate(order.begin(), order.begin() + static_cast<int>(Params::ModuleId::ensemble), order.begin() + static_cast<int>(Params::ModuleId::ensemble) + 1);
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
            check(p->getChain().getTopology().split == static_cast<int>(Params::ModuleId::shimmer), "SHIMMER Stereo: split at its slot");
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
        // level on them is gated below, against the exact continuation of the old order.
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

        // Single reorders against the exact continuation of the old order (the same processor's
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
                std::printf("  %-27s %d single reorders against the old order's continuation: worst 20 ms dip %.2f dB, median %.2f dB\n",
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
            const auto writeHalf = [&](RoarschachAudioProcessor& q, bool first)
            {
                for (int slot = 0; slot < Params::numModules; ++slot)
                    if ((slot < Params::numModules / 2) == first)
                        setParam(q, Params::modules[static_cast<size_t>(target[static_cast<size_t>(slot)])].posId, static_cast<float>(slot));
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
            bool reference = true;
            for (int k = 0; k < Params::numModules; ++k)
                reference = reference && order[static_cast<size_t>(k)] == k;
            check(reference, "all-equal positions resolve to the reference order");
            positions = Params::defaultPositions();
            positions[static_cast<size_t>(Params::ModuleId::sub)] = 0;   // SUB and PITCH both ask for 0
            const auto o2 = Params::resolveOrder(positions);
            check(o2[0] == static_cast<int>(Params::ModuleId::pitch) && o2[1] == static_cast<int>(Params::ModuleId::sub), "tie-break by the reference index");
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
            std::array<bool, Params::numModules> slots {};
            bool permutation = true;
            for (int m = 0; m < Params::numModules; ++m)
            {
                const auto it = preset.values.find(Params::modules[static_cast<size_t>(m)].posId);
                const int pos = it != preset.values.end() ? juce::roundToInt(it->second) : -1;
                if (pos < 0 || pos >= Params::numModules || slots[static_cast<size_t>(pos)])
                    permutation = false;
                else
                    slots[static_cast<size_t>(pos)] = true;
                positions[static_cast<size_t>(m)] = pos;
            }
            check(permutation, juce::String(preset.name) + " positions are a permutation");
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
        for (const char* required : { "Phone Monster", "Throat Singer", "Ribcage", "Cyborg Commander", "Respirator Rasp", "Rasp Machine" })
            check(presetIndex(required) >= 0, juce::String("missing preset ") + required);

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
        std::array<bool, Params::numModules> seen {};
        for (const auto& m : Params::modules)
        {
            const int pos = juce::roundToInt(getParam(p, m.posId));
            if (!juce::isPositiveAndBelow(pos, Params::numModules) || seen[static_cast<size_t>(pos)])
                return false;
            seen[static_cast<size_t>(pos)] = true;
        }
        return true;
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
                    if (knob < 36.0f)
                    {
                        ++failures;
                        std::printf("    knob %s is only %.0f px\n", id, knob);
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
        stage.setSize(1600, 1000);
        stage.addAndMakeVisible(editor);
        editor.setBounds(0, 0, 1600, 900);
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
        for (const char* name : { "Cyborg Commander", "Ribcage", "Smashed", "Pulse Choir", "Kaiju" })
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
        p->setCurrentProgram(presetIndex("Cyborg Commander"));
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
            if (sr == 48000.0)
            {
                check(init <= 12.0, "Init CPU " + juce::String(init, 1) + " % (<= 12 %)");
                check(worst <= 25.0, "worst preset CPU " + juce::String(worst, 1) + " % (<= 25 %)");
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
                check(view.isCreatureFrozen && view.setCreatureFrozen && view.resetCreature, "the creature menu is wired");
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

    int renderFiles(const juce::File& input, const juce::File& outDir, juce::StringArray presetNames,
                    const juce::StringArray& only, const juce::StringArray& without, const juce::String& order)
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

        if (presetNames.isEmpty())
            presetNames = { "Init" };

        if (!outDir.createDirectory())
        {
            std::printf("cannot create %s\n", outDir.getFullPathName().toRawUTF8());
            return 1;
        }

        const auto inStats = stats(in);
        std::printf("input %s: %.2f s @ %.0f Hz (rendered at this rate), %d ch, rms %.1f dBFS, peak %.1f dBFS\n",
                    input.getFileName().toRawUTF8(), length / sr, sr, file.getNumChannels(), toDb(inStats.rms), toDb(inStats.peak));

        int failures = 0;
        for (const auto& name : presetNames)
        {
            const int index = presetIndex(name);
            if (index < 0)
            {
                std::printf("no factory preset \"%s\"\n", name.toRawUTF8());
                ++failures;
                continue;
            }

            auto p = makeProcessor(sr, 512, monoIn ? 1 : 2, 2);
            p->setCurrentProgram(index);
            juce::String suffix;
            if (!only.isEmpty())
            {
                for (const auto& m : Params::modules)
                    setParam(*p, m.onId, only.contains(m.code) ? 1.0f : 0.0f);
                suffix << "-only-" << only.joinIntoString("-").toLowerCase();
            }
            for (const auto& code : without)
            {
                const int m = Params::moduleIndexForCode(code.toStdString());
                if (m >= 0)
                    setParam(*p, Params::modules[static_cast<size_t>(m)].onId, 0.0f);
            }
            if (!without.isEmpty())
                suffix << "-without-" << without.joinIntoString("-").toLowerCase();
            if (order.isNotEmpty())
            {
                setOrder(*p, orderFromCodes(order));
                suffix << "-order-" << order.removeCharacters(" ,").toLowerCase();
            }
            restart(*p, sr, 512);
            const int latency = p->getLatencySamples();

            // Pad with the latency plus a tail, then drop the latency so the file lines up with the input.
            Stereo padded = in;
            const int extra = latency + static_cast<int>(0.5 * sr);
            padded.l.resize(static_cast<size_t>(length + extra), 0.0f);
            padded.r.resize(static_cast<size_t>(length + extra), 0.0f);
            auto out = render(*p, padded, 512);
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

            const juce::File target = outDir.getChildFile(name.replaceCharacter(' ', '-').replaceCharacter('&', 'n').toLowerCase() + suffix + ".wav");
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

            const float ceiling = ceilingGain(getParam(*p, Params::outCeiling));
            const bool broken = !st.finite || st.peak > ceiling || std::abs(st.mean) > 0.01;
            failures += broken ? 1 : 0;
            std::printf("%-18s%s rms %6.1f dBFS  peak %6.2f dBFS  true peak %6.2f dBTP  side/mid %5.1f dB  clamps %llu  latency %d  %s -> %s\n",
                        name.toRawUTF8(), suffix.toRawUTF8(), toDb(st.rms), toDb(st.peak),
                        toDb(std::max(truePeak(out.l, true), truePeak(out.r, true))),
                        toDb(std::sqrt(sideSq / std::max(midSq, 1.0e-20))), static_cast<unsigned long long>(p->getChain().getClampCount()),
                        latency, broken ? "BROKEN" : "ok", target.getFullPathName().toRawUTF8());
        }
        return failures == 0 ? 0 : 1;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

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
        juce::StringArray presets, only, without;
        juce::String order;
        for (int i = 4; i < argc; ++i)
        {
            const std::string a(argv[i]);
            const bool hasValue = i + 1 < argc;
            if (a == "--preset" && hasValue)
                presets.add(argv[++i]);
            else if (a == "--only" && hasValue)
                only.add(juce::String(argv[++i]).toUpperCase());
            else if (a == "--without" && hasValue)
                without.add(juce::String(argv[++i]).toUpperCase());
            else if (a == "--order" && hasValue)
                order = argv[++i];
            else
                presets.add(argv[i]);   // a bare name is a preset
        }
        return renderFiles(juce::File(argv[2]), juce::File(argv[3]), presets, only, without, order);
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
        { "editor", testEditor },
        { "creature", testCreature },
        { "logo", testLogoSdf },
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

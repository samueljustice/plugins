// Output stage test: Inflate, Push, the limiter, the trims and the fold-downs behave the same at every room width.
//
//   SwooshbucklerOutput [scratch folder]
//
//   the maths      two channels worked by side are the plain Stereo stage; one sound shared between a room's
//                  speakers (any width, depth, height, Spread) comes out, side by side, as the Stereo render of the
//                  same sound: the same harmonics from Inflate, the same gain reduction; speaker by speaker (the
//                  stage without sides) it would be shaped and limited less, and that is caught
//   in the rig     the same pass (Across, spread wide, Overhead) at Stereo, 5.1 and 7.1.4 with Inflate and Push up:
//                  harmonics added and gain reduction within a stated tolerance of the Stereo render
//   the Ceiling    held on every channel at every width, at the samples and between them, with every speaker
//                  sounding; the LFE on its own limiter; no Inflate on the LFE; one latency
//   loudness       the same settings read about the same BS.1770 loudness at every width
//   trims          Master Trim moves every channel alike, the LFE too; the Everything tremolo shakes every speaker
//                  and the LFE in phase
//   fold-downs     a worst-case 7.1.4 take auditioned on a narrower output stays under the Ceiling; a quiet one is
//                  the plain fold; takes that are not folded play untouched
//   LFE Level      typed values ("10" is -10 dB)
//   speed          the output stage at Stereo and 7.1.4, 48 / 96 / 192 kHz, by side and speaker by speaker

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "Cargo.h"
#include "RigEngine.h"
#include "Room.h"
#include "RoomMaster.h"
#include "MasterProcessor.h"
#include "TakeAudition.h"
#include "TestClock.h"

#include <cmath>
#include <functional>
#include <iostream>

namespace
{
    constexpr double c_fs = 48000.0;
    constexpr int c_block = 256;
    constexpr int c_wide = room::c_maxChannels;
    constexpr double c_twoPi = 6.283185307179586;
    constexpr float c_ceilingDb = -0.3f;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok)
            ++g_failures;
    }

    double db(double power) { return 10.0 * std::log10(std::max(power, 1.0e-30)); }
    juce::String num(double v, int places = 2) { return juce::String(v, places); }
    juce::String nameOf(room::Width w) { return room::layout(w).name; }

    const room::Width c_widths[] = { room::Width::Mono, room::Width::Stereo, room::Width::Quad, room::Width::Surround51,
                                     room::Width::Surround71, room::Width::Surround712, room::Width::Surround714 };
    const room::Width c_rooms[] = { room::Width::Quad, room::Width::Surround51, room::Width::Surround71,
                                    room::Width::Surround712, room::Width::Surround714 };

    using Signal = std::vector<float>;
    struct Take
    {
        std::array<Signal, c_wide> ch;
        size_t samples() const { return ch[0].size(); }
        void resize(size_t n) { for (auto& c : ch) c.assign(n, 0.0f); }
    };

    double energy(const Signal& x, size_t from = 0)
    {
        double e = 0.0;
        for (size_t i = from; i < x.size(); ++i)
            e += static_cast<double>(x[i]) * x[i];
        return e;
    }
    float peakOf(const Signal& x, size_t from = 0)
    {
        float p = 0.0f;
        for (size_t i = from; i < x.size(); ++i) p = std::max(p, std::abs(x[i]));
        return p;
    }

    /** The peak between the samples as well: four points a sample from a long windowed sinc (64 samples; the
        limiter's own detector reads 48). */
    float truePeakOf(const Signal& x)
    {
        constexpr int half = 32, over = 4;
        static std::vector<float> table;
        if (table.empty())
        {
            table.resize(static_cast<size_t>(over * 2 * half));
            for (int p = 0; p < over; ++p)
                for (int j = 0; j < 2 * half; ++j)
                {
                    const double d = static_cast<double>(j - half + 1) - static_cast<double>(p) / over;
                    const double w = 0.5 + 0.5 * std::cos(c_twoPi * 0.5 * d / half);
                    table[static_cast<size_t>(p * 2 * half + j)] = static_cast<float>(std::abs(d) < 1.0e-9 ? 1.0 : std::sin(0.5 * c_twoPi * d) / (0.5 * c_twoPi * d) * w);
                }
        }
        float peak = 0.0f;
        for (size_t n = half; n + half < x.size(); ++n)
        {
            if (std::abs(x[n]) < 0.5f * peak && std::abs(x[n + 1]) < 0.5f * peak)
                continue; // a peak between two samples is never more than a few dB over them
            for (int p = 0; p < over; ++p)
            {
                float sum = 0.0f;
                const float* h = table.data() + p * 2 * half;
                for (int j = 0; j < 2 * half; ++j)
                    sum += h[j] * x[n - half + 1 + static_cast<size_t>(j)];
                peak = std::max(peak, std::abs(sum));
            }
        }
        return peak;
    }

    Signal noise(int samples, int seed, float gain = 1.0f)
    {
        juce::Random r(seed);
        Signal x(static_cast<size_t>(samples));
        for (auto& v : x) v = gain * (r.nextFloat() * 2.0f - 1.0f);
        return x;
    }

    /** Power of x at one frequency (a Hann-windowed correlation), as the power of a sine of that size. */
    double toneIn(const Signal& x, double hz, size_t from = 0)
    {
        double re = 0.0, im = 0.0, wsum = 0.0;
        const size_t n = x.size() - from;
        for (size_t i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(c_twoPi * static_cast<double>(i) / static_cast<double>(n));
            const double ph = c_twoPi * hz * static_cast<double>(i) / c_fs;
            re += w * x[from + i] * std::cos(ph);
            im += w * x[from + i] * std::sin(ph);
            wsum += w;
        }
        return 2.0 * (re * re + im * im) / (wsum * wsum);
    }

    //==================================================================================================================
    // Spectra: Welch's average of Hann-windowed power spectra.
    constexpr int c_fftOrder = 13, c_fftSize = 1 << c_fftOrder;
    std::vector<double> spectrum(const Signal& x, size_t from = 0)
    {
        juce::dsp::FFT fft(c_fftOrder);
        std::vector<double> psd(c_fftSize / 2 + 1, 0.0);
        std::vector<float> buf(2 * c_fftSize);
        for (size_t start = from; start + c_fftSize <= x.size(); start += c_fftSize / 2)
        {
            std::fill(buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < c_fftSize; ++i)
                buf[static_cast<size_t>(i)] = x[start + static_cast<size_t>(i)] * static_cast<float>(0.5 - 0.5 * std::cos(c_twoPi * i / c_fftSize));
            fft.performFrequencyOnlyForwardTransform(buf.data());
            for (size_t k = 0; k < psd.size(); ++k)
                psd[k] += static_cast<double>(buf[k]) * buf[k];
        }
        return psd;
    }
    double band(const std::vector<double>& psd, double f1, double f2)
    {
        double sum = 0.0;
        const double binHz = c_fs / c_fftSize;
        for (size_t k = 1; k < psd.size(); ++k)
            if (const double f = static_cast<double>(k) * binHz; f >= f1 && f < f2)
                sum += psd[k];
        return sum;
    }

    //==================================================================================================================
    // Loudness, ITU-R BS.1770: each speaker through the K weighting (48 kHz coefficients from the standard), its mean
    // square weighted by where the speaker stands (room::loudnessWeight: the LFE left out), summed. Not gated: the
    // takes here sound all the way through.
    double kWeightedPower(const Signal& x, size_t from)
    {
        const double b1[3] = { 1.53512485958697, -2.69169618940638, 1.19839281085285 }, a1[2] = { -1.69065929318241, 0.73248077421585 };
        const double b2[3] = { 1.0, -2.0, 1.0 }, a2[2] = { -1.99004745483398, 0.99007225036621 };
        double s1[2] {}, s2[2] {}, sum = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double in = x[i];
            const double y1 = b1[0] * in + s1[0];
            s1[0] = b1[1] * in - a1[0] * y1 + s1[1];
            s1[1] = b1[2] * in - a1[1] * y1;
            const double y2 = b2[0] * y1 + s2[0];
            s2[0] = b2[1] * y1 - a2[0] * y2 + s2[1];
            s2[1] = b2[2] * y1 - a2[1] * y2;
            if (i >= from)
                sum += y2 * y2;
        }
        return sum / static_cast<double>(std::max<size_t>(1, x.size() - from));
    }
    double loudness(const juce::AudioBuffer<float>& audio, room::Width width, size_t from)
    {
        const auto& lay = room::layout(width);
        double total = 0.0;
        for (int c = 0; c < audio.getNumChannels() && c < lay.numChannels; ++c)
        {
            const float weight = width == room::Width::Mono ? 1.0f : room::loudnessWeight(lay.speakers[static_cast<size_t>(c)]);
            if (weight > 0.0f)
                total += weight * kWeightedPower(Signal(audio.getReadPointer(c), audio.getReadPointer(c) + audio.getNumSamples()), from);
        }
        return -0.691 + db(total);
    }

    //==================================================================================================================
    void writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(file);
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream, c_fs, static_cast<unsigned int>(audio.getNumChannels()), 24, {}, 0));
        if (w != nullptr) w->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples()); else delete stream;
    }

    struct Sources
    {
        juce::File tone, hissA, hissB, rumble;
    };
    constexpr double c_toneHz = 400.0;

    Sources makeSources(const juce::File& folder)
    {
        Sources s;
        auto hiss = [&](const juce::String& name, int seed)
        {
            juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 8.0));
            juce::Random r(seed);
            float y = 0.0f, y2 = 0.0f;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                y += 0.25f * ((r.nextFloat() * 2.0f - 1.0f) - y);
                y2 += 0.5f * (y - y2);
                b.setSample(0, i, 0.9f * y2);
            }
            auto f = folder.getChildFile(name + ".wav");
            writeWav(f, b);
            return f;
        };
        s.hissA = hiss("hissA", 11);
        s.hissB = hiss("hissB", 29);
        {
            juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 8.0));
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample(0, i, 0.9f * static_cast<float>(std::sin(c_twoPi * c_toneHz * i / c_fs)));
            s.tone = folder.getChildFile("tone.wav");
            writeWav(s.tone, b);
        }
        {
            juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 8.0));
            juce::Random r(5);
            sj::Svf lp;
            lp.set(180.0f, 0.7f, static_cast<float>(c_fs));
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample(0, i, 2.2f * lp.lowpass(r.nextFloat() * 2.0f - 1.0f));
            s.rumble = folder.getChildFile("rumble.wav");
            writeWav(s.rumble, b);
        }
        return s;
    }

    //==================================================================================================================
    // The output stage on its own.
    MasterProcessor::Parameters stageParams(float pushDb, float inflate, float curve = 1.0f)
    {
        MasterProcessor::Parameters p;
        p.limiterOn = true;
        p.ceilingDb = c_ceilingDb;
        p.pushDb = pushDb;
        p.inflateEffect = inflate;
        p.inflateCurve = curve;
        p.releaseMs = 20.0f;
        p.sustainReleaseMs = 1000.0f;
        return p;
    }

    struct Reduction
    {
        double meanDb = 0.0, deepestDb = 0.0;
        std::uint64_t clamps = 0; // how often the safety clamp after the limiter had to act (it never should)
    };

    /** The stage a width has (RoomMaster), in place. */
    Reduction runStage(Take& take, room::Width width, const MasterProcessor::Parameters& p, double rate = c_fs)
    {
        RoomMaster m;
        m.prepare(rate, width);
        m.setParameters(p);
        const int channels = RoomMaster::pipeChannels(width);
        Reduction r;
        int blocks = 0;
        float* ptrs[c_wide];
        for (size_t pos = 0; pos < take.samples(); pos += c_block)
        {
            const int n = static_cast<int>(std::min<size_t>(c_block, take.samples() - pos));
            for (int c = 0; c < c_wide; ++c)
                ptrs[c] = take.ch[static_cast<size_t>(c)].data() + pos;
            m.process(ptrs, channels, n);
            r.meanDb += m.getGainReductionDb();
            r.deepestDb = std::min(r.deepestDb, static_cast<double>(m.getGainReductionDb()));
            ++blocks;
        }
        r.meanDb /= std::max(1, blocks);
        r.clamps = m.getSafetyClampCount();
        return r;
    }

    /** The stage as it was before sides: every main speaker shaped by its own level and the limiter reading each
        speaker's own peak (one gain for all), the LFE on its own without Inflate. */
    Reduction runSpeakerBySpeaker(Take& take, room::Width width, const MasterProcessor::Parameters& p, double rate = c_fs)
    {
        const auto& lay = room::layout(RoomMaster::pipeWidth(width));
        MasterProcessor mains, lfe;
        mains.prepare(rate, lay.numChannels - (lay.lfe >= 0 ? 1 : 0));
        lfe.prepare(rate, 1);
        mains.setParameters(p);
        auto q = p;
        q.inflateEffect = 0.0f;
        lfe.setParameters(q);
        Reduction r;
        int blocks = 0;
        for (size_t pos = 0; pos < take.samples(); pos += c_block)
        {
            const int n = static_cast<int>(std::min<size_t>(c_block, take.samples() - pos));
            float* ptrs[c_wide];
            int count = 0;
            for (int c = 0; c < lay.numChannels; ++c)
                if (c != lay.lfe)
                    ptrs[count++] = take.ch[static_cast<size_t>(c)].data() + pos;
            mains.process(ptrs, count, n);
            if (lay.lfe >= 0)
            {
                float* l = take.ch[static_cast<size_t>(lay.lfe)].data() + pos;
                lfe.process(&l, 1, n);
            }
            r.meanDb += mains.getGainReductionDb();
            r.deepestDb = std::min(r.deepestDb, static_cast<double>(mains.getGainReductionDb()));
            ++blocks;
        }
        r.meanDb /= std::max(1, blocks);
        return r;
    }

    /** One sound placed in a room by the panner. */
    Take placed(const Signal& sound, room::Width width, room::Position at, const room::PanSettings& settings)
    {
        room::Gains g {};
        room::pan(width, at, settings, g);
        Take take;
        take.resize(sound.size());
        for (size_t c = 0; c < c_wide; ++c)
            if (g[c] != 0.0f)
                for (size_t i = 0; i < sound.size(); ++i)
                    take.ch[c][i] = g[c] * sound[i];
        return take;
    }

    /** The speakers of one side of a room summed by power, sample by sample (side -1 left, +1 right; a centre
        speaker counts half to each): the Stereo channel that side stands in for, without its sign. */
    Signal sideLevel(const Take& take, room::Width width, int side)
    {
        const auto& lay = room::layout(width);
        Signal level(take.samples(), 0.0f);
        for (int c = 0; c < lay.numChannels; ++c)
        {
            if (c == lay.lfe)
                continue;
            const float x = room::info(lay.speakers[static_cast<size_t>(c)]).x;
            const float w = x * static_cast<float>(side) > 0.0f ? 1.0f : (juce::exactlyEqual(x, 0.0f) ? 0.5f : 0.0f);
            if (w > 0.0f)
                for (size_t i = 0; i < level.size(); ++i)
                    level[i] += w * take.ch[static_cast<size_t>(c)][i] * take.ch[static_cast<size_t>(c)][i];
        }
        for (auto& v : level)
            v = std::sqrt(v);
        return level;
    }

    /** Every main speaker's power at one frequency, added up. */
    double toneInMains(const Take& take, room::Width width, double hz, size_t from)
    {
        const auto& lay = room::layout(RoomMaster::pipeWidth(width));
        double p = 0.0;
        for (int c = 0; c < lay.numChannels; ++c)
            if (c != lay.lfe)
                p += toneIn(take.ch[static_cast<size_t>(c)], hz, from);
        return p;
    }

    //==================================================================================================================
    // The engine on its own, every channel of the room.
    struct Scene
    {
        CargoHold hold;
        RigEngine rig;
        RigEngine::Params p;

        Scene()
        {
            p.rideOn = false;
            p.space = 0.0f;
            p.fill = 0.0f;
            p.hitAim = false;
            for (auto& b : p.buses)
            {
                b.low = b.swirl = b.filter = b.drive = b.duck = b.key = 0.0f;
                b.width = 1.0f;
            }
        }
        void load(int track, const juce::File& file, int bus)
        {
            juce::String error;
            if (! hold.loadFile(track, file, error))
                check(false, "loads " + file.getFileName() + " " + error);
            hold.setBusOverride(track, bus);
        }
        Take run(room::Width width, double seconds, const std::function<void(int)>& before = {})
        {
            rig.setCargoHold(&hold);
            rig.setWidth(RoomMaster::pipeWidth(width));
            rig.setParams(p);
            rig.prepare(c_fs, c_block);
            rig.setParams(p);
            Take take;
            const int blocks = static_cast<int>(seconds * c_fs / c_block);
            take.resize(static_cast<size_t>(blocks * c_block));
            for (int b = 0; b < blocks; ++b)
            {
                if (before)
                {
                    before(b);
                    rig.setParams(p);
                }
                float* out[c_wide];
                for (int c = 0; c < c_wide; ++c)
                    out[c] = take.ch[static_cast<size_t>(c)].data() + b * c_block;
                rig.process(nullptr, nullptr, out, c_wide, c_block);
            }
            return take;
        }
    };

    /** How much of a take's sound, every main speaker together, is above the tone's own band (dB re the whole). */
    double harmonicsDb(const Take& take, room::Width width, size_t from)
    {
        const auto& lay = room::layout(RoomMaster::pipeWidth(width));
        double above = 0.0, all = 0.0;
        for (int c = 0; c < lay.numChannels; ++c)
        {
            if (c == lay.lfe)
                continue;
            const auto psd = spectrum(take.ch[static_cast<size_t>(c)], from);
            above += band(psd, 1.6 * c_toneHz, 20000.0);
            all += band(psd, 20.0, 20000.0);
        }
        return db(above / std::max(all, 1.0e-30));
    }

    //==================================================================================================================
    // The plug-in.
    using Proc = SwooshbucklerAudioProcessor;

    struct Plug
    {
        std::unique_ptr<juce::AudioProcessor> base { createPluginFilter() };
        Proc& proc { *dynamic_cast<Proc*>(base.get()) };

        Plug(const Sources& src, room::Width width, const std::function<void(Plug&)>& setUp, double rate = c_fs)
        {
            juce::AudioProcessor::BusesLayout layout;
            layout.outputBuses.add(room::channelSet(width));
            if (! proc.setBusesLayout(layout))
                check(false, "the bus takes " + nameOf(width));
            juce::String error;
            if (! proc.getCargo().loadFile(0, src.hissA, error))
                check(false, "loads the hiss: " + error);
            proc.getCargo().setBusOverride(0, 0);
            if (! proc.getCargo().loadFile(1, src.hissB, error))
                check(false, "loads the hiss: " + error);
            proc.getCargo().setBusOverride(1, 1);
            if (! proc.getCargo().loadFile(2, src.rumble, error))
                check(false, "loads the rumble: " + error);
            proc.getCargo().setBusOverride(2, CargoHold::c_stowBus);
            if (setUp)
                setUp(*this);
            proc.prepareToPlay(rate, 512);
        }
        void setPlain(const char* id, float plain)
        {
            auto* param = proc.getState().getParameter(id);
            param->setValueNotifyingHost(param->convertTo0to1(plain));
        }
        juce::AudioBuffer<float> run(double seconds, float* deepest = nullptr, double rate = c_fs)
        {
            proc.setPlaying(true);
            const int channels = proc.getTotalNumOutputChannels();
            const int blocks = static_cast<int>(seconds * rate / 512);
            juce::AudioBuffer<float> out(channels, blocks * 512), block(channels, 512);
            juce::MidiBuffer midi;
            for (int b = 0; b < blocks; ++b)
            {
                block.clear();
                proc.processBlock(block, midi);
                if (deepest != nullptr)
                    *deepest = std::min(*deepest, proc.getGainReductionDb());
                for (int ch = 0; ch < channels; ++ch)
                    out.copyFrom(ch, b * 512, block, ch, 0, 512);
            }
            return out;
        }
    };

    Signal channelOf(const juce::AudioBuffer<float>& a, int ch) { return Signal(a.getReadPointer(ch), a.getReadPointer(ch) + a.getNumSamples()); }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("SwooshbucklerOutputTest");
    folder.createDirectory();
    const auto src = makeSources(folder);

    juce::PropertiesFile::Options options;
    options.applicationName = "SwooshbucklerOutputTest";
    options.filenameSuffix = ".settings";
    const auto settingsFile = folder.getChildFile("test.settings");
    settingsFile.deleteFile();
    juce::PropertiesFile settings(settingsFile, options);
    settings.setValue("recordFolder", folder.getChildFile("tape").getFullPathName());
    Proc::useSettings(&settings);
    // A second argument runs only the parts whose key it names (maths rig ceiling plugin loudness trims folds typed speed).
    const juce::String only = argc > 2 ? juce::String(argv[2]) : juce::String();
    auto wanted = [&](const char* key) { return only.isEmpty() || only.contains(key); };

    //==================================================================================================================
    if (wanted("maths"))
    {
        std::cout << "The maths: a room's speakers worked by side" << std::endl;
        // Two channels, one a side, are the plain stage (the Stereo path itself does not use sides).
        {
            const int n = static_cast<int>(c_fs * 2.0);
            Signal l = noise(n, 1, 0.9f), r = noise(n, 2, 0.6f);
            for (int i = 0; i < n; ++i) // some low end too, so the even part's coupling is worked
                l[static_cast<size_t>(i)] = 0.5f * l[static_cast<size_t>(i)] + 0.45f * static_cast<float>(std::sin(c_twoPi * 90.0 * i / c_fs));
            double worst = 0.0;
            for (float push : { 0.0f, 9.0f })
            {
                MasterProcessor plain, sided;
                plain.prepare(c_fs, 2);
                sided.prepare(c_fs, 2);
                const float sides[2] = { -1.0f, 1.0f };
                sided.setSides(sides, 2);
                plain.setParameters(stageParams(push, 1.0f));
                sided.setParameters(stageParams(push, 1.0f));
                Signal a = l, b = r, c = l, d = r;
                for (int pos = 0; pos < n; pos += c_block)
                {
                    float* p1[2] = { a.data() + pos, b.data() + pos };
                    float* p2[2] = { c.data() + pos, d.data() + pos };
                    plain.process(p1, 2, std::min(c_block, n - pos));
                    sided.process(p2, 2, std::min(c_block, n - pos));
                }
                for (int i = 0; i < n; ++i)
                    worst = std::max({ worst, static_cast<double>(std::abs(a[static_cast<size_t>(i)] - c[static_cast<size_t>(i)])),
                                       static_cast<double>(std::abs(b[static_cast<size_t>(i)] - d[static_cast<size_t>(i)])) });
            }
            std::cout << "         two channels by side against the plain stage: apart by " << num(worst * 1.0e6, 2) << " millionths" << std::endl;
            check(worst < 2.0e-5, "two channels, one a side, come out as the plain Stereo stage does (Inflate at its hardest, with and without Push)");
        }

        // One sound shared between a room's speakers: side by side the Stereo render of the same sound.
        const int n = static_cast<int>(c_fs * 1.5);
        const size_t settle = static_cast<size_t>(c_fs * 0.5);
        Signal tone(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            tone[static_cast<size_t>(i)] = 0.8f * static_cast<float>(std::sin(c_twoPi * 1000.0 * i / c_fs)) * std::min(1.0f, static_cast<float>(i) / 2400.0f);
        struct Place
        {
            const char* name;
            room::Position at;
            float spread;
        };
        const Place places[] = { { "front left", { -1.0f, 1.0f, 0.0f }, 0.0f },      { "front, a third left", { -0.33f, 1.0f, 0.0f }, 0.0f },
                                 { "middle of the room", { 0.0f, 0.0f, 0.0f }, 0.0f }, { "overhead right", { 0.6f, 0.0f, 1.0f }, 0.0f },
                                 { "back left, half up", { -0.8f, -0.7f, 0.5f }, 0.3f }, { "left, spread wide", { -1.0f, 0.3f, 0.0f }, 1.0f },
                                 { "right rear, spread half", { 0.7f, -1.0f, 0.2f }, 0.5f } };
        double worstSide = 0.0, worstHarm = 0.0, worstGr = 0.0;
        double oldHarmLeast = 0.0, oldGrMost = 0.0;
        juce::String oldWhere;
        for (float push : { 0.0f, 9.0f })
            for (const auto& place : places)
            {
                room::PanSettings pan;
                pan.spread = place.spread;
                Take stereo = placed(tone, room::Width::Stereo, place.at, pan);
                const auto stereoGr = runStage(stereo, room::Width::Stereo, stageParams(push, 1.0f));
                const double stereoThird = toneInMains(stereo, room::Width::Stereo, 3000.0, settle);
                for (auto w : c_rooms)
                {
                    Take roomTake = placed(tone, w, place.at, pan);
                    Take before = roomTake;
                    const auto gr = runStage(roomTake, w, stageParams(push, 1.0f));
                    const auto grBefore = runSpeakerBySpeaker(before, w, stageParams(push, 1.0f));
                    for (int side : { -1, 1 })
                    {
                        const auto level = sideLevel(roomTake, w, side);
                        const auto& ref = stereo.ch[side < 0 ? 0 : 1];
                        for (size_t i = 0; i < level.size(); ++i)
                            worstSide = std::max(worstSide, static_cast<double>(std::abs(level[i] - std::abs(ref[i]))));
                    }
                    const double third = db(toneInMains(roomTake, w, 3000.0, settle) / stereoThird);
                    const double thirdBefore = db(toneInMains(before, w, 3000.0, settle) / stereoThird);
                    worstHarm = std::max(worstHarm, std::abs(third));
                    worstGr = std::max(worstGr, std::abs(gr.meanDb - stereoGr.meanDb));
                    if (thirdBefore < oldHarmLeast)
                    {
                        oldHarmLeast = thirdBefore;
                        oldWhere = juce::String(place.name) + " at " + nameOf(w);
                    }
                    oldGrMost = std::max(oldGrMost, grBefore.meanDb - stereoGr.meanDb);
                }
            }
        std::cout << "         a 1 kHz tone at -1.9 dB, seven places, five rooms, Push 0 and +9 dB, against its Stereo render:" << std::endl
                  << "           by side: each side's level apart by at most " << num(worstSide * 1.0e6, 1) << " millionths; third harmonic within "
                  << num(worstHarm, 3) << " dB; gain reduction within " << num(worstGr, 3) << " dB" << std::endl
                  << "           speaker by speaker (as it was): third harmonic down to " << num(oldHarmLeast, 1) << " dB of Stereo's (" << oldWhere
                  << "); up to " << num(oldGrMost, 1) << " dB less gain reduction" << std::endl;
        check(worstSide < 1.0e-4, "one sound in a room, each side's speakers together, is the Stereo channel of the same sound, sample for sample");
        check(worstHarm < 0.05, "Inflate adds the same harmonics wherever the sound is and however many speakers share it (within 0.05 dB)");
        check(worstGr < 0.05, "the limiter takes off the same whatever the width and the Spread (within 0.05 dB)");
        check(oldHarmLeast < -6.0 && oldGrMost > 3.0, "the stage speaker by speaker does less in a room (the measure tells the two apart)");

        // The centre speaker: half to each side.
        {
            room::PanSettings pan;
            pan.centre = 1.0f;
            double worst = 0.0, worstGrC = 0.0;
            for (float x : { 0.0f, -0.5f, 0.8f })
            {
                Take stereo = placed(tone, room::Width::Stereo, { x, 1.0f, 0.0f }, pan);
                const auto sg = runStage(stereo, room::Width::Stereo, stageParams(6.0f, 1.0f));
                Take wide = placed(tone, room::Width::Surround51, { x, 1.0f, 0.0f }, pan);
                const auto wg = runStage(wide, room::Width::Surround51, stageParams(6.0f, 1.0f));
                worst = std::max(worst, std::abs(db(toneInMains(wide, room::Width::Surround51, 3000.0, settle) / toneInMains(stereo, room::Width::Stereo, 3000.0, settle))));
                worstGrC = std::max(worstGrC, std::abs(wg.meanDb - sg.meanDb));
            }
            std::cout << "         with the centre speaker in use (front wall, three places): third harmonic within " << num(worst, 2)
                      << " dB of Stereo's, gain reduction within " << num(worstGrC, 2) << " dB" << std::endl;
            check(worst < 3.2 && worstGrC < 3.2, "a sound in or near the centre speaker is shaped about as its phantom would be (within 3 dB; limited up to 3 dB sooner)");

            // Below the limiter the centre speaker's sound and its phantom are the same sound to Inflate: the speaker
            // counts half to each side, which is what each Stereo channel carries of a centred sound. (The check
            // above cannot tell: its limiter is working, and that alone moves the harmonics by up to 3 dB.)
            Signal quiet = tone;
            for (auto& v : quiet)
                v *= 0.5f;
            Take stereo = placed(quiet, room::Width::Stereo, { 0.0f, 1.0f, 0.0f }, pan);
            const auto sg = runStage(stereo, room::Width::Stereo, stageParams(0.0f, 1.0f));
            double apart = 0.0, deepest = sg.deepestDb;
            for (auto w : { room::Width::Surround51, room::Width::Surround71, room::Width::Surround714 })
            {
                Take wide = placed(quiet, w, { 0.0f, 1.0f, 0.0f }, pan);
                const auto wg = runStage(wide, w, stageParams(0.0f, 1.0f));
                deepest = std::min(deepest, wg.deepestDb);
                for (double hz : { 1000.0, 3000.0, 5000.0 })
                    apart = std::max(apart, std::abs(db(toneInMains(wide, w, hz, settle) / toneInMains(stereo, room::Width::Stereo, hz, settle))));
            }
            std::cout << "         a quieter sound in the centre speaker alone against its phantom in Stereo: the tone and its third and fifth harmonics within "
                      << num(apart, 3) << " dB (no limiting: " << num(deepest, 2) << " dB)" << std::endl;
            check(apart < 0.05 && deepest > -0.01, "below the limiter a sound in the centre speaker gets exactly the Inflate its phantom gets (within 0.05 dB)");
        }
    }

    //==================================================================================================================
    if (wanted("rig"))
    {
        std::cout << "In the rig: the same pass at Stereo, 5.1 and 7.1.4 with Inflate and Push up" << std::endl;
        // The Stereo render a room is held against is the pass in the same place left to right: a pass that
        // crosses the room against one that crosses the stereo field, and a pass that stays in the middle of the
        // room (spread everywhere, or flying overhead) against one kept in the middle of the stereo field (Width
        // 0). Stereo itself shapes a centred sound less than one at the side (each channel carries 3 dB less of
        // it), so the two Stereo renders differ too: that difference is printed, and is not the room's doing.
        struct Case
        {
            const char* name;
            room::Path path;
            float spread;
            float stereoWidth;
            bool between; // a little to one side: held between the two Stereo renders
        };
        constexpr float c_rigPush = 15.0f;
        const Case cases[] = { { "Across", room::Path::Across, 0.0f, 1.0f, false }, { "Across, Spread 100%", room::Path::Across, 1.0f, 0.0f, false },
                               { "Overhead", room::Path::Overhead, 0.0f, 0.0f, true } };
        const size_t settle = static_cast<size_t>(c_fs * 1.0);
        double worstHarm = 0.0, worstGr = 0.0, worstOldHarm = 0.0, worstOldGr = 0.0, crossingHarm = 0.0, crossingGr = 0.0, worstFromCrossing = 0.0, worstGrFromCrossing = 0.0;
        bool overheadBetween = true;
        for (const auto& c : cases)
        {
            double stereoHarm = 0.0;
            Reduction stereoGr;
            for (auto w : { room::Width::Stereo, room::Width::Surround51, room::Width::Surround714 })
            {
                Scene scene;
                scene.load(0, src.tone, 0);
                scene.p.buses[0].path = static_cast<int>(c.path);
                scene.p.buses[0].bend = 0.0f;
                scene.p.roomSpread = c.spread;
                if (w == room::Width::Stereo)
                    scene.p.buses[0].width = c.stereoWidth;
                Take take = scene.run(w, 12.0);
                Take before = take;
                const auto gr = runStage(take, w, stageParams(c_rigPush, 1.0f));
                const auto grBefore = runSpeakerBySpeaker(before, w, stageParams(c_rigPush, 1.0f));
                const double harm = harmonicsDb(take, w, settle), harmBefore = harmonicsDb(before, w, settle);
                if (w == room::Width::Stereo)
                {
                    stereoHarm = harm;
                    stereoGr = gr;
                    if (c.stereoWidth > 0.5f)
                    {
                        crossingHarm = harm;
                        crossingGr = gr.meanDb;
                    }
                }
                std::cout << "         " << juce::String(c.name).paddedRight(' ', 20) << (w == room::Width::Stereo ? juce::String(c.stereoWidth > 0.5f ? "Stereo" : "Stereo, in the middle") : nameOf(w)).paddedRight(' ', 22)
                          << " harmonics " << num(harm, 2) << " dB of the sound, gain reduction mean " << num(gr.meanDb, 2) << " dB, deepest " << num(gr.deepestDb, 2) << " dB";
                if (w != room::Width::Stereo)
                {
                    std::cout << "   (speaker by speaker: " << num(harmBefore, 2) << " dB, " << num(grBefore.meanDb, 2) << " dB)";
                    if (c.between)
                    {
                        // Overhead flies a little to one side of the middle: between the two Stereo renders.
                        auto inside = [](double v, double a, double b) { return v > std::min(a, b) - 0.5 && v < std::max(a, b) + 0.5; };
                        overheadBetween = overheadBetween && inside(harm, stereoHarm, crossingHarm) && inside(gr.meanDb, stereoGr.meanDb, crossingGr);
                    }
                    else
                    {
                        worstHarm = std::max(worstHarm, std::abs(harm - stereoHarm));
                        worstGr = std::max(worstGr, std::abs(gr.meanDb - stereoGr.meanDb));
                    }
                    worstOldHarm = std::max(worstOldHarm, std::abs(harmBefore - stereoHarm));
                    worstOldGr = std::max(worstOldGr, std::abs(grBefore.meanDb - stereoGr.meanDb));
                    worstFromCrossing = std::max(worstFromCrossing, std::abs(harm - crossingHarm));
                    worstGrFromCrossing = std::max(worstGrFromCrossing, std::abs(gr.meanDb - crossingGr));
                }
                std::cout << std::endl;
            }
        }
        std::cout << "         Across and Spread 100% against the Stereo render in the same place: harmonics within " << num(worstHarm, 2) << " dB, gain reduction within " << num(worstGr, 2)
                  << " dB (speaker by speaker: " << num(worstOldHarm, 2) << " dB and " << num(worstOldGr, 2) << " dB)" << std::endl
                  << "         against the Stereo pass that crosses the field, whatever the path and Spread: harmonics within " << num(worstFromCrossing, 2)
                  << " dB, gain reduction within " << num(worstGrFromCrossing, 2) << " dB" << std::endl;
        check(worstHarm < 0.5, "the harmonics Inflate adds to a pass are within 0.5 dB of the Stereo render's at 5.1 and 7.1.4 (Across, and spread over every speaker)");
        check(worstGr < 0.5, "the limiter's gain reduction on a pass is within 0.5 dB of the Stereo render's at 5.1 and 7.1.4");
        check(overheadBetween, "an Overhead pass, a little to one side of the middle, gets between what Stereo gives a pass in the middle and one that crosses");
        check(worstFromCrossing < 2.0 && worstGrFromCrossing < 2.0, "and every pass is within 2 dB of a Stereo pass crossing the field, even one that stays in the middle of the room");
        check(worstOldHarm > 3.0 && worstOldGr > 2.0 * worstGr + 1.0, "speaker by speaker, a pass in a room falls well short of both (the measure tells the two apart)");
    }

    //==================================================================================================================
    if (wanted("ceiling"))
    {
        std::cout << "The Ceiling on every channel, the LFE's own limiter, one latency" << std::endl;
        const float ceiling = juce::Decibels::decibelsToGain(c_ceilingDb);
        const int n = static_cast<int>(c_fs * 3.0);
        float worstSample = 0.0f, worstTrue = 0.0f;
        std::uint64_t clamps = 0;
        bool latencySame = true;
        int latency = -1;
        for (auto w : c_widths)
        {
            const auto& lay = room::layout(RoomMaster::pipeWidth(w));
            // Every speaker its own loud noise, a loud low tone as well in the surrounds, pushed hard.
            for (int kind = 0; kind < 3; ++kind)
            {
                Take take;
                take.resize(static_cast<size_t>(n));
                for (int c = 0; c < lay.numChannels; ++c)
                {
                    if (kind == 0)
                        take.ch[static_cast<size_t>(c)] = noise(n, 40 + c, 2.0f);
                    else if (kind == 1) // the same sound in every speaker, in phase
                        take.ch[static_cast<size_t>(c)] = noise(n, 7, 1.5f);
                    else // one speaker at a time, loud, the others quiet
                        for (int i = 0; i < n; ++i)
                            take.ch[static_cast<size_t>(c)][static_cast<size_t>(i)] = ((i * lay.numChannels / n) == c ? 3.0f : 0.05f)
                                * static_cast<float>(std::sin(c_twoPi * (997.0 + 31.0 * c) * i / c_fs));
                }
                clamps += runStage(take, w, stageParams(kind == 0 ? 12.0f : 6.0f, 1.0f)).clamps;
                for (int c = 0; c < lay.numChannels; ++c)
                {
                    worstSample = std::max(worstSample, peakOf(take.ch[static_cast<size_t>(c)]));
                    worstTrue = std::max(worstTrue, truePeakOf(take.ch[static_cast<size_t>(c)]));
                }
            }
            RoomMaster m;
            m.prepare(c_fs, w);
            if (latency < 0)
                latency = m.getLatencySamples();
            latencySame = latencySame && m.getLatencySamples() == latency;
            // An impulse on each channel comes out the same number of samples later, Inflate and Push or not.
            for (int c = 0; c < lay.numChannels; ++c)
            {
                Take take;
                take.resize(2048);
                take.ch[static_cast<size_t>(c)][100] = 0.25f;
                runStage(take, w, stageParams(3.0f, 1.0f));
                size_t at = 0;
                for (size_t i = 0; i < 2048; ++i)
                    if (std::abs(take.ch[static_cast<size_t>(c)][i]) > std::abs(take.ch[static_cast<size_t>(c)][at]))
                        at = i;
                latencySame = latencySame && static_cast<int>(at) - 100 == latency;
            }
        }
        std::cout << "         loudest sample " << num(juce::Decibels::gainToDecibels(worstSample), 3) << " dB, loudest point between samples "
                  << num(juce::Decibels::gainToDecibels(worstTrue), 3) << " dB (Ceiling " << num(c_ceilingDb, 1) << " dB); latency " << latency << " samples" << std::endl;
        check(worstSample <= ceiling, "no sample of any channel at any width is over the Ceiling (every speaker loud, in phase, and one at a time)");
        check(juce::Decibels::gainToDecibels(worstTrue) <= c_ceilingDb + 0.25f, "nor is the waveform between the samples (within 0.25 dB: full-scale white noise pushed 12 dB, the hardest there is)");
        check(clamps == 0, "the limiter's gain did that on its own: the safety clamp after it never acted");
        check(latencySame, "every channel of every width comes out the same number of samples later");

        // A spread sound reaches the Ceiling as its sum: the side's level sits at the Ceiling, each speaker below it.
        {
            Signal loud(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i)
                loud[static_cast<size_t>(i)] = 2.0f * static_cast<float>(std::sin(c_twoPi * 500.0 * i / c_fs));
            room::PanSettings pan;
            pan.spread = 1.0f;
            Take take = placed(loud, room::Width::Surround714, { -1.0f, 0.0f, 0.0f }, pan);
            runStage(take, room::Width::Surround714, stageParams(0.0f, 0.0f));
            const auto left = sideLevel(take, room::Width::Surround714, -1), right = sideLevel(take, room::Width::Surround714, 1);
            const float sidePeak = std::max(peakOf(left, 4800), peakOf(right, 4800));
            float speakerPeak = 0.0f;
            for (const auto& c : take.ch)
                speakerPeak = std::max(speakerPeak, peakOf(c, 4800));
            std::cout << "         a loud sound spread over all of 7.1.4: each side's level peaks at " << num(juce::Decibels::gainToDecibels(sidePeak), 2)
                      << " dB, the loudest speaker at " << num(juce::Decibels::gainToDecibels(speakerPeak), 2) << " dB" << std::endl;
            check(std::abs(juce::Decibels::gainToDecibels(sidePeak) - c_ceilingDb) < 0.1f && speakerPeak < 0.6f * sidePeak,
                  "a spread sound is held at the Ceiling as its sum, as Stereo would hold it (each speaker then sits below)");
        }

        // The LFE: its own limiter, no Inflate, Push as the mains.
        {
            const auto w = room::Width::Surround714;
            const auto& lay = room::layout(w);
            auto make = [&](float lfeGain, float mainGain)
            {
                Take take;
                take.resize(static_cast<size_t>(n));
                for (int c = 0; c < lay.numChannels; ++c)
                    for (int i = 0; i < n; ++i)
                        take.ch[static_cast<size_t>(c)][static_cast<size_t>(i)] = c == lay.lfe ? lfeGain * static_cast<float>(std::sin(c_twoPi * 50.0 * i / c_fs))
                                                                                            : mainGain * static_cast<float>(std::sin(c_twoPi * (300.0 + 40.0 * c) * i / c_fs));
                return take;
            };
            Take quietLfe = make(0.1f, 0.2f), loudLfe = make(3.0f, 0.2f), loudMains = make(0.1f, 3.0f);
            Take quietLfeFlat = quietLfe;
            runStage(quietLfe, w, stageParams(0.0f, 1.0f));
            runStage(quietLfeFlat, w, stageParams(0.0f, 0.0f));
            runStage(loudLfe, w, stageParams(0.0f, 1.0f));
            runStage(loudMains, w, stageParams(0.0f, 1.0f));
            float mainsMoved = 0.0f, lfeMoved = 0.0f, lfeInflated = 0.0f;
            for (int c = 0; c < lay.numChannels; ++c)
                for (size_t i = 0; i < quietLfe.samples(); ++i)
                {
                    if (c != lay.lfe)
                        mainsMoved = std::max(mainsMoved, std::abs(loudLfe.ch[static_cast<size_t>(c)][i] - quietLfe.ch[static_cast<size_t>(c)][i]));
                    else
                    {
                        lfeMoved = std::max(lfeMoved, std::abs(loudMains.ch[static_cast<size_t>(c)][i] - quietLfe.ch[static_cast<size_t>(c)][i]));
                        lfeInflated = std::max(lfeInflated, std::abs(quietLfeFlat.ch[static_cast<size_t>(c)][i] - quietLfe.ch[static_cast<size_t>(c)][i]));
                    }
                }
            check(juce::exactlyEqual(mainsMoved, 0.0f), "a loud LFE held by its limiter moves no main speaker");
            check(juce::exactlyEqual(lfeMoved, 0.0f), "loud mains held by their limiter do not move the LFE");
            check(juce::exactlyEqual(lfeInflated, 0.0f), "Inflate does not touch the LFE");
            Take pushed = make(0.1f, 0.2f);
            runStage(pushed, w, stageParams(6.0f, 0.0f));
            const double lfeUp = db(energy(pushed.ch[static_cast<size_t>(lay.lfe)], 4800) / energy(quietLfeFlat.ch[static_cast<size_t>(lay.lfe)], 4800));
            const double mainUp = db(energy(pushed.ch[0], 4800) / energy(quietLfeFlat.ch[0], 4800));
            check(std::abs(lfeUp - 6.0) < 0.01 && std::abs(mainUp - 6.0) < 0.01, "Push drives the LFE as it drives the mains (" + num(lfeUp, 2) + " and " + num(mainUp, 2) + " dB)");
        }
    }

    //==================================================================================================================
    if (wanted("plugin"))
    {
        std::cout << "The plug-in: the Ceiling with every speaker sounding, rendering at the device rate and above it" << std::endl;
        const float ceiling = juce::Decibels::decibelsToGain(c_ceilingDb);
        float worst = 0.0f, quietest = 1.0f;
        for (int high = 0; high < 2; ++high)
            for (auto w : c_rooms)
            {
                Plug plug(src, w, [&](Plug& p)
                {
                    if (high != 0)
                        p.proc.setRecordRate(96000);
                    p.setPlain(ParamIDs::push, 12.0f);
                    p.setPlain(ParamIDs::inflate, 1.0f);
                    p.setPlain(ParamIDs::ceiling, c_ceilingDb);
                    p.setPlain(ParamIDs::roomSpread, 0.6f);
                    p.setPlain(ParamIDs::roomCentre, 1.0f);
                    p.setPlain(ParamIDs::space, 0.5f);
                    p.setPlain(ParamIDs::flightPath[0], static_cast<float>(room::Path::Around));
                    p.setPlain(ParamIDs::flightPath[1], static_cast<float>(room::Path::Overhead));
                    p.setPlain(ParamIDs::stowOn, 1.0f);
                    p.setPlain(ParamIDs::stowMode, static_cast<float>(ParamIDs::StowSub));
                    p.setPlain(ParamIDs::roomLfeMode, 2.0f);
                    p.setPlain(ParamIDs::roomLfeLevel, 0.0f);
                });
                const auto out = plug.run(6.0);
                for (int c = 0; c < out.getNumChannels(); ++c)
                {
                    const float peak = out.getMagnitude(c, 0, out.getNumSamples());
                    worst = std::max(worst, peak);
                    quietest = std::min(quietest, peak);
                }
            }
        std::cout << "         loudest sample of any channel " << num(juce::Decibels::gainToDecibels(worst), 3) << " dB, the quietest channel's peak "
                  << num(juce::Decibels::gainToDecibels(quietest), 1) << " dB" << std::endl;
        check(worst <= ceiling, "no channel of any room is over the Ceiling, pushed 12 dB with every speaker and the LFE sounding");
        check(quietest > 0.01f, "every channel sounded (the check is not of silent channels)");
    }

    //==================================================================================================================
    if (wanted("loudness"))
    {
        std::cout << "Loudness: the same settings at every width (BS.1770 weights, against Stereo)" << std::endl;
        struct Setting
        {
            const char* name;
            std::function<void(Plug&)> setUp;
            bool held = true; // false: printed for the record, not held to the tolerance
        };
        const Setting cases[] = {
            { "as it opens (Across)", [](Plug&) {} },
            { "Through", [](Plug& p) { p.setPlain(ParamIDs::flightPath[0], 1.0f); p.setPlain(ParamIDs::flightPath[1], 1.0f); } },
            { "Overhead", [](Plug& p) { p.setPlain(ParamIDs::flightPath[0], 2.0f); p.setPlain(ParamIDs::flightPath[1], 2.0f); } },
            { "Around", [](Plug& p) { p.setPlain(ParamIDs::flightPath[0], 3.0f); p.setPlain(ParamIDs::flightPath[1], 3.0f); } },
            { "Around, Doppler 10%", [](Plug& p) { p.setPlain(ParamIDs::flightPath[0], 3.0f); p.setPlain(ParamIDs::flightPath[1], 3.0f); p.setPlain("aDoppler", 0.1f); p.setPlain("bDoppler", 0.1f); } },
            { "Around, Doppler 100%", [](Plug& p) { p.setPlain(ParamIDs::flightPath[0], 3.0f); p.setPlain(ParamIDs::flightPath[1], 3.0f); p.setPlain("aDoppler", 1.0f); p.setPlain("bDoppler", 1.0f); }, false },
            { "Spread 100%", [](Plug& p) { p.setPlain(ParamIDs::roomSpread, 1.0f); } },
            { "Centre Speaker on", [](Plug& p) { p.setPlain(ParamIDs::roomCentre, 1.0f); } },
            { "Space 70%", [](Plug& p) { p.setPlain(ParamIDs::space, 0.7f); } },
            { "Inflate 100%, Push +9", [](Plug& p) { p.setPlain(ParamIDs::inflate, 1.0f); p.setPlain(ParamIDs::push, 9.0f); } },
            { "Around, Spread 50%, Inflate 100%, Push +9", [](Plug& p) { p.setPlain(ParamIDs::flightPath[0], 3.0f); p.setPlain(ParamIDs::flightPath[1], 3.0f);
                                                                         p.setPlain(ParamIDs::roomSpread, 0.5f); p.setPlain(ParamIDs::inflate, 1.0f); p.setPlain(ParamIDs::push, 9.0f); } },
        };
        const size_t settle = static_cast<size_t>(c_fs * 2.0);
        double worst = 0.0, worstPlain = 0.0;
        juce::String worstWhere;
        std::cout << "         " << juce::String("setting").paddedRight(' ', 44) << "Stereo LUFS   Mono   Quad    5.1    7.1  7.1.2  7.1.4   (dB from Stereo; in brackets without the surround weights)" << std::endl;
        for (const auto& c : cases)
        {
            double stereo = 0.0, stereoPlain = 0.0;
            juce::String line = "         " + juce::String(c.name).paddedRight(' ', 44);
            juce::String plainLine;
            for (auto w : { room::Width::Stereo, room::Width::Mono, room::Width::Quad, room::Width::Surround51, room::Width::Surround71, room::Width::Surround712, room::Width::Surround714 })
            {
                Plug plug(src, w, c.setUp);
                const auto out = plug.run(20.0);
                const double l = loudness(out, w, settle);
                double plain = 0.0; // every main speaker counted alike: the power in the room
                {
                    const auto& lay = room::layout(w);
                    for (int ch = 0; ch < out.getNumChannels(); ++ch)
                        if (ch != lay.lfe || w == room::Width::Mono)
                            plain += kWeightedPower(channelOf(out, ch), settle);
                    plain = -0.691 + db(plain);
                }
                if (w == room::Width::Stereo)
                {
                    stereo = l;
                    stereoPlain = plain;
                    line << num(l, 2).paddedLeft(' ', 9) << "   ";
                    continue;
                }
                line << num(l - stereo, 2).paddedLeft(' ', 6) << " ";
                plainLine << num(plain - stereoPlain, 2).paddedLeft(' ', 6) << " ";
                if (w != room::Width::Mono && c.held)
                {
                    if (std::abs(l - stereo) > worst)
                    {
                        worst = std::abs(l - stereo);
                        worstWhere = juce::String(c.name) + " at " + nameOf(w);
                    }
                    worstPlain = std::max(worstPlain, std::abs(plain - stereoPlain));
                }
            }
            std::cout << line << (c.held ? "" : "  (not held: see below)") << std::endl << "         " << juce::String().paddedRight(' ', 56) << "(" << plainLine.trimEnd() << ")" << std::endl;
        }
        std::cout << "         furthest from Stereo: " << num(worst, 2) << " dB (" << worstWhere << "); as plain power " << num(worstPlain, 2) << " dB" << std::endl;
        // The plain power is the room's doing; the weighted reading is higher wherever sound is in the surrounds,
        // which BS.1770 counts 1.5 dB up (Ls / Rs, the sides). Around with Doppler at 100% is not held: Doppler
        // brings a pass nearer, and a pass going Around stays that near (as loud, as bright) all the way round,
        // where a crossing pass is only there for a moment.
        check(worstPlain < 0.5, "every room carries the Stereo render's power at the same settings (within 0.5 dB)");
        check(worst < 1.5, "and reads within 1.5 dB of its loudness (the surrounds count 1.5 dB up in a loudness reading)");
    }

    //==================================================================================================================
    if (wanted("trims"))
    {
        std::cout << "Trims: every channel alike" << std::endl;
        // Master Trim: the same gain on every channel, the LFE too.
        auto setUp = [](Plug& p)
        {
            p.setPlain(ParamIDs::roomSpread, 0.7f);
            p.setPlain(ParamIDs::roomCentre, 1.0f);
            p.setPlain(ParamIDs::stowOn, 1.0f);
            p.setPlain(ParamIDs::stowMode, static_cast<float>(ParamIDs::StowSub));
            p.setPlain(ParamIDs::roomLfeMode, 1.0f);
        };
        Plug full(src, room::Width::Surround714, [&](Plug& p) { setUp(p); p.setPlain(ParamIDs::masterTrim, 0.0f); });
        Plug trimmed(src, room::Width::Surround714, [&](Plug& p) { setUp(p); p.setPlain(ParamIDs::masterTrim, -6.0f); });
        const auto a = full.run(4.0), b = trimmed.run(4.0);
        const float g = juce::Decibels::decibelsToGain(-6.0f);
        float worst = 0.0f, quietest = 1.0f;
        for (int c = 0; c < a.getNumChannels(); ++c)
        {
            quietest = std::min(quietest, a.getMagnitude(c, 0, a.getNumSamples()));
            for (int i = 0; i < a.getNumSamples(); ++i)
                worst = std::max(worst, std::abs(b.getSample(c, i) - g * a.getSample(c, i)));
        }
        check(worst < 1.0e-6f && quietest > 0.003f, "Master Trim at -6 dB is every one of 7.1.4's twelve channels at exactly half (apart by " + num(worst * 1.0e6, 2) + " millionths)");

        // The Everything tremolo: one gain, the same in every speaker at every moment.
        auto shaken = [&](bool on)
        {
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.p.roomSpread = 1.0f; // the one sound in every speaker, so each speaker's dip can be read exactly
            scene.p.roomCentre = true;
            scene.p.lfeMode = RigEngine::LfeMixdown;
            scene.p.lfeLevelDb = 0.0f;
            scene.p.tremOn = on;
            scene.p.tremTarget = 0;
            scene.p.tremDepth = 0.5f;
            scene.p.tremThreshold = 0.0f;
            scene.p.tremRate = 0.0f; // its slowest, so a short stretch of sound has one gain
            return scene.run(room::Width::Surround71, 6.0);
        };
        const Take with = shaken(true), without = shaken(false);
        // Per 5 ms, each speaker's level with the tremolo over its level without: the same in every speaker. (The
        // LFE is the mains' low end through its low-pass, which smears a moving gain a little: held loosely.)
        const auto& lay = room::layout(room::Width::Surround71);
        double worstMain = 0.0, deepest = 1.0;
        std::vector<double> mainDips, lfeDips; // dB, per 5 ms: the first main speaker's and the LFE's
        const size_t blockLen = 240;
        for (size_t start = static_cast<size_t>(c_fs); start + blockLen <= with.samples(); start += blockLen)
        {
            double ratio0 = -1.0, lfeRatio = -1.0;
            for (int c = 0; c < lay.numChannels; ++c)
            {
                double ew = 0.0, eo = 0.0;
                for (size_t i = start; i < start + blockLen; ++i)
                {
                    ew += static_cast<double>(with.ch[static_cast<size_t>(c)][i]) * with.ch[static_cast<size_t>(c)][i];
                    eo += static_cast<double>(without.ch[static_cast<size_t>(c)][i]) * without.ch[static_cast<size_t>(c)][i];
                }
                if (eo < 1.0e-7 * static_cast<double>(blockLen))
                    continue;
                const double ratio = std::sqrt(ew / eo);
                if (c == lay.lfe)
                {
                    lfeRatio = ratio;
                    continue;
                }
                if (ratio0 < 0.0)
                    ratio0 = ratio;
                deepest = std::min(deepest, ratio);
                worstMain = std::max(worstMain, std::abs(20.0 * std::log10(ratio / ratio0)));
            }
            if (ratio0 > 0.0 && lfeRatio > 0.0)
            {
                mainDips.push_back(20.0 * std::log10(ratio0));
                lfeDips.push_back(20.0 * std::log10(lfeRatio));
            }
        }
        // The LFE is the mains' low end through its low-pass, which smears a moving gain over a few milliseconds:
        // its dips are held to the mains' in time (they rise and fall together) and in depth on average.
        double meanApart = 0.0, corr = 0.0;
        {
            double ma = 0.0, mb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;
            const double count = static_cast<double>(std::max<size_t>(1, mainDips.size()));
            for (size_t i = 0; i < mainDips.size(); ++i)
            {
                ma += mainDips[i] / count;
                mb += lfeDips[i] / count;
                meanApart += std::abs(mainDips[i] - lfeDips[i]) / count;
            }
            for (size_t i = 0; i < mainDips.size(); ++i)
            {
                saa += (mainDips[i] - ma) * (mainDips[i] - ma);
                sbb += (lfeDips[i] - mb) * (lfeDips[i] - mb);
                sab += (mainDips[i] - ma) * (lfeDips[i] - mb);
            }
            corr = sab / std::sqrt(std::max(saa * sbb, 1.0e-30));
        }
        std::cout << "         the Everything tremolo at 7.1 (deepest dip " << num(20.0 * std::log10(deepest), 1) << " dB): speakers apart by at most "
                  << num(worstMain, 3) << " dB in any 5 ms; the LFE's dips follow theirs (correlation " << num(corr, 3) << ", " << num(meanApart, 2) << " dB apart on average)" << std::endl;
        check(deepest < 0.7, "the tremolo shook the room (the check is not of a still sound)");
        check(worstMain < 0.05, "the Everything tremolo dips every main speaker together (within 0.05 dB in any 5 ms)");
        check(corr > 0.9 && meanApart < 0.75 && mainDips.size() > 200, "and the LFE with them (its dips rise and fall with theirs)");
    }

    //==================================================================================================================
    if (wanted("folds"))
    {
        std::cout << "Fold-downs that are heard: a take auditioned on a narrower output" << std::endl;
        const int n = static_cast<int>(c_fs * 2.0);
        auto takeFile = [&](const juce::String& name, int channels, float gain)
        {
            // Worst case: the same full-scale sound in every channel, in phase.
            juce::AudioBuffer<float> b(channels, n);
            juce::Random r(77);
            for (int i = 0; i < n; ++i)
            {
                const float v = gain * (i % 2000 < 1000 ? 0.999f * static_cast<float>(std::sin(c_twoPi * 220.0 * i / c_fs)) : 0.999f * (r.nextFloat() * 2.0f - 1.0f));
                for (int c = 0; c < channels; ++c)
                    b.setSample(c, i, v);
            }
            auto f = folder.getChildFile(name + ".wav");
            writeWav(f, b);
            return f;
        };
        auto audition = [&](const juce::File& file, room::Width output, float ceilingDb)
        {
            TakeAudition player;
            player.prepare(c_fs, output);
            player.setCeilingDb(ceilingDb);
            juce::String error;
            if (! player.play(file, error))
                check(false, "plays " + file.getFileName() + ": " + error);
            const int channels = room::layout(output).numChannels;
            juce::AudioBuffer<float> out(channels, n + 4800), block(channels, 512);
            out.clear();
            for (int pos = 0; pos + 512 <= out.getNumSamples(); pos += 512)
            {
                player.topUp();
                block.clear();
                player.process(block, 0, 512);
                for (int c = 0; c < channels; ++c)
                    out.copyFrom(c, pos, block, c, 0, 512);
            }
            return out;
        };
        const auto loud = takeFile("worst714", 12, 1.0f), quiet = takeFile("quiet714", 12, 0.02f), loud51 = takeFile("worst51", 6, 1.0f);
        struct Fold
        {
            const juce::File* file;
            int channels;
            room::Width to;
        };
        const Fold folds[] = { { &loud, 12, room::Width::Stereo }, { &loud, 12, room::Width::Mono }, { &loud, 12, room::Width::Surround51 }, { &loud, 12, room::Width::Surround71 },
                               { &loud51, 6, room::Width::Stereo }, { &loud51, 6, room::Width::Quad } };
        float worstOver = -100.0f, mostUnlimited = 0.0f;
        bool sounded = true;
        for (const auto& f : folds)
        {
            const float ceilingDb = -1.0f;
            const auto out = audition(*f.file, f.to, ceilingDb);
            float peak = 0.0f;
            for (int c = 0; c < out.getNumChannels(); ++c)
                peak = std::max(peak, truePeakOf(channelOf(out, c)));
            const auto mix = TakeAudition::mixFor(f.channels, f.to);
            float rowSum = 0.0f;
            for (int o = 0; o < mix.numOut; ++o)
            {
                float sum = 0.0f;
                for (int i = 0; i < mix.numIn; ++i)
                    sum += mix.gain[static_cast<size_t>(o)][static_cast<size_t>(i)];
                rowSum = std::max(rowSum, sum);
            }
            mostUnlimited = std::max(mostUnlimited, juce::Decibels::gainToDecibels(rowSum));
            worstOver = std::max(worstOver, juce::Decibels::gainToDecibels(peak) - ceilingDb);
            sounded = sounded && peak > 0.5f;
            std::cout << "         " << f.channels << " channels on " << nameOf(f.to).paddedRight(' ', 7) << ": unlimited it would reach "
                      << num(juce::Decibels::gainToDecibels(rowSum), 1) << " dB, heard at " << num(juce::Decibels::gainToDecibels(peak), 2) << " dB (Ceiling -1.0)" << std::endl;
        }
        check(worstOver <= 0.25f && sounded, "a worst-case take folded for auditioning stays at the Ceiling, within 0.25 dB between the samples (it would reach +" + num(mostUnlimited, 1) + " dB)");

        // A quiet take: the plain fold, only later by the limiter's look-ahead.
        {
            const auto out = audition(quiet, room::Width::Stereo, -1.0f);
            const auto mix = TakeAudition::mixFor(12, room::Width::Stereo);
            float row = 0.0f;
            for (int i = 0; i < mix.numIn; ++i)
                row += mix.gain[0][static_cast<size_t>(i)];
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(quiet));
            juce::AudioBuffer<float> in(12, n);
            reader->read(&in, 0, n, 0, true, true);
            MasterProcessor probe;
            probe.prepare(c_fs, 2);
            const int delay = probe.getLatencySamples();
            // (The player's rate converter delays by a few samples of its own, the same for every take: found here.)
            float worst = 1.0f, last = 0.0f;
            int converter = 0;
            for (int d = 0; d < 16; ++d)
            {
                float apart = 0.0f;
                for (int i = 1000; i < n - 16; ++i) // past the fade in
                    apart = std::max(apart, std::abs(out.getSample(0, i + delay + d) - row * in.getSample(0, i)));
                if (apart < worst)
                {
                    worst = apart;
                    converter = d;
                }
            }
            for (int i = n - 64; i < n; ++i)
                last = std::max(last, std::abs(out.getSample(0, i + delay + converter)));
            check(worst < 1.0e-6f, "a quiet take is folded as the room folds it and no more (apart by " + num(worst * 1.0e6, 2) + " millionths), "
                                   + juce::String(delay) + " samples later");
            check(last > 0.001f, "and its last samples are heard (the limiter's look-ahead plays out)");
        }
        // Not folded: untouched, not delayed.
        {
            const auto out = audition(loud51, room::Width::Surround51, -6.0f);
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(loud51));
            juce::AudioBuffer<float> in(6, n);
            reader->read(&in, 0, n, 0, true, true);
            float worst = 1.0f;
            for (int d = 0; d < 16; ++d)
            {
                float apart = 0.0f;
                for (int c = 0; c < 6; ++c)
                    for (int i = 1000; i < n - 16; ++i)
                        apart = std::max(apart, std::abs(out.getSample(c, i + d) - in.getSample(c, i)));
                worst = std::min(worst, apart);
            }
            check(worst < 1.0e-6f, "a take as wide as the output plays as it is, over the Ceiling or not: no limiter");
            check(! TakeAudition::needsLimiter(TakeAudition::mixFor(6, room::Width::Surround714)) && ! TakeAudition::needsLimiter(TakeAudition::mixFor(1, room::Width::Stereo))
                  && TakeAudition::needsLimiter(TakeAudition::mixFor(12, room::Width::Stereo)),
                  "only a mix that can come out louder than the take is limited (a narrower take on a wider output is not)");
        }
    }

    //==================================================================================================================
    if (wanted("typed"))
    {
        std::cout << "LFE Level as typed" << std::endl;
        auto typed = [](const char* t) { return Params::lfeLevelFromText(t); };
        check(std::abs(typed("10") + 10.0f) < 1.0e-6f && std::abs(typed("-10") + 10.0f) < 1.0e-6f && std::abs(typed("18.5 dB") + 18.5f) < 1.0e-6f,
              "a level typed without its minus sign is that many dB down (\"10\" is -10 dB, as \"-10\" is)");
        check(juce::exactlyEqual(typed("0"), 0.0f) && juce::exactlyEqual(typed("-0.0 dB"), 0.0f) && juce::exactlyEqual(typed("90"), ParamIDs::lfeLevelMinDb)
              && juce::exactlyEqual(typed("unity"), ParamIDs::lfeLevelUnityDb) && juce::exactlyEqual(typed("-10.0 dB (Unity)"), ParamIDs::lfeLevelUnityDb),
              "0 is 0 dB, past the bottom stops at the bottom, Unity is found");
        Plug plug(src, room::Width::Surround51, {});
        auto* param = plug.proc.getState().getParameter(ParamIDs::roomLfeLevel);
        check(std::abs(param->convertFrom0to1(param->getValueForText("12")) + 12.0f) < 0.05f, "the parameter reads typed text the same way");
    }

    //==================================================================================================================
    if (wanted("speed"))
    {
        std::cout << "Speed: the output stage (Inflate on, the limiter working, 10 s of noise)" << std::endl;
        for (double rate : { 48000.0, 96000.0, 192000.0 })
            for (auto w : { room::Width::Stereo, room::Width::Surround714 })
            {
                const int n = static_cast<int>(rate * 10.0);
                const auto& lay = room::layout(w);
                Take take;
                take.resize(static_cast<size_t>(n));
                for (int c = 0; c < lay.numChannels; ++c)
                    take.ch[static_cast<size_t>(c)] = noise(n, 60 + c, 0.7f);
                Take copy = take;
                double t0 = TestClock::cpuMs();
                runStage(take, w, stageParams(6.0f, 1.0f), rate);
                const double bySide = (TestClock::cpuMs() - t0) / 100.0; // % of a core: ms per 10 s
                t0 = TestClock::cpuMs();
                runSpeakerBySpeaker(copy, w, stageParams(6.0f, 1.0f), rate);
                const double before = (TestClock::cpuMs() - t0) / 100.0;
                std::cout << "         " << juce::String(rate / 1000.0, 0).paddedLeft(' ', 3) << " kHz " << nameOf(w).paddedRight(' ', 7) << num(bySide, 2) << "% of a core"
                          << (w == room::Width::Stereo ? juce::String() : "   (speaker by speaker, as it was: " + num(before, 2) + "%)") << std::endl;
                if (rate > 190000.0 && w == room::Width::Surround714)
                    check(bySide < 25.0, "the output stage at 7.1.4 and 192 kHz takes under a quarter of a core");
            }
        // The whole plug-in at 7.1.4 rendering at 192 kHz.
        for (auto w : { room::Width::Stereo, room::Width::Surround714 })
        {
            Plug plug(src, w, [](Plug& p)
            {
                p.proc.setRecordRate(192000);
                p.setPlain(ParamIDs::inflate, 1.0f);
                p.setPlain(ParamIDs::push, 6.0f);
                p.setPlain(ParamIDs::roomLfeMode, 2.0f);
            });
            const double t0 = TestClock::cpuMs();
            plug.run(10.0);
            std::cout << "         the whole plug-in rendering at 192 kHz, " << nameOf(w) << ": " << num((TestClock::cpuMs() - t0) / 100.0, 1) << "% of a core" << std::endl;
        }
    }

    Proc::useSettings(nullptr);
    std::cout << std::endl << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

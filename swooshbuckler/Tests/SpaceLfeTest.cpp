// Space and LFE test: the reverb in a room, and what feeds the LFE.
//
//   SwooshbucklerSpaceLfe [scratch folder]
//
//   the reverb   RoomReverb against SpaceReverb fed the same sound: as much reverb, all speakers together, as the
//                stereo one gives in L and R; every return uncorrelated with every other at every delay up to 50 ms
//                (so a fold-down adds power and makes no comb); a fold to Stereo as smooth in frequency as one
//                speaker (and a deliberately combed fold is caught)
//   in the rig   reverb in every main speaker and none in the LFE; the same Space as loud as in Stereo (within
//                1 dB); Height takes it into the tops; it leans toward where the passes are and still fills the
//                room; the centre speaker stays silent with Centre Speaker off; a fold to Stereo keeps the reverb's
//                share; Space coming on from 0 makes no click an echo later
//   the LFE      Off silent; Discrete is exactly Stowaway's Sub through the LFE's low-pass, and only when Stowaway
//                sounds in Sub; nothing above the low-pass beyond its slope; Mixdown follows the mains' low end;
//                the mains are the same in every mode; bass management (mains + LFE 10 dB hotter) adds 3.5 dB and
//                never cancels, where the same copies without the phase match would; LFE Level and Mode glide
//   the plug-in  the LFE's limiter does not move the mains; no Inflate on the LFE; LFE Mode and Level as
//                parameters (Off, the Unity mark); which widths have an LFE; a fold drops the LFE (the mains
//                already carry it)
//   speed        the engine at 7.1.4 with Space and the LFE against Stereo, at 48, 96 and 192 kHz

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "Presets.h"
#include "Cargo.h"
#include "RigEngine.h"
#include "Room.h"
#include "SpaceReverb.h"
#include "LfeFilter.h"
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
    // The engine's Space: dry eased by this much at full Space (RigEngine.cpp c_spaceDryTrim). Used to take the
    // dry out of a render with Space, leaving what Space added.
    constexpr float c_spaceDryTrim = 0.45f;
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

    using Signal = std::vector<float>;

    double energy(const Signal& x, size_t from = 0)
    {
        double e = 0.0;
        for (size_t i = from; i < x.size(); ++i)
            e += static_cast<double>(x[i]) * x[i];
        return e;
    }
    float peakOf(const Signal& x)
    {
        float p = 0.0f;
        for (float v : x) p = std::max(p, std::abs(v));
        return p;
    }

    //==================================================================================================================
    // Spectra: Welch's average of Hann-windowed power spectra, 2^14 points (2.9 Hz a bin at 48 kHz).
    constexpr int c_fftOrder = 14, c_fftSize = 1 << c_fftOrder;
    std::vector<double> spectrum(const Signal& x, size_t from = 0)
    {
        juce::dsp::FFT fft(c_fftOrder);
        std::vector<double> psd(c_fftSize / 2 + 1, 0.0);
        std::vector<float> buf(2 * c_fftSize);
        int frames = 0;
        for (size_t start = from; start + c_fftSize <= x.size(); start += c_fftSize / 2)
        {
            std::fill(buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < c_fftSize; ++i)
                buf[static_cast<size_t>(i)] = x[start + static_cast<size_t>(i)] * static_cast<float>(0.5 - 0.5 * std::cos(c_twoPi * i / c_fftSize));
            fft.performFrequencyOnlyForwardTransform(buf.data());
            for (size_t k = 0; k < psd.size(); ++k)
                psd[k] += static_cast<double>(buf[k]) * buf[k];
            ++frames;
        }
        for (auto& v : psd) v /= std::max(1, frames);
        return psd;
    }
    double band(const std::vector<double>& psd, double f1, double f2)
    {
        double sum = 0.0;
        const double binHz = c_fs / c_fftSize;
        for (size_t k = 1; k < psd.size(); ++k)
        {
            const double f = static_cast<double>(k) * binHz;
            if (f >= f1 && f < f2)
                sum += psd[k];
        }
        return sum;
    }
    double third(const std::vector<double>& psd, double centre) { return band(psd, centre / std::pow(2.0, 1.0 / 6.0), centre * std::pow(2.0, 1.0 / 6.0)); }

    /** How rough a spectrum is in fine detail: the spread (dB, standard deviation) of each bin from the mean of
        its neighbours within a sixth of an octave, 300 Hz to 8 kHz. A comb (a sound added to a delayed copy of
        itself) makes regular peaks and dips and shows here; a reverb's own fine detail is averaged away by Welch. */
    double ripple(const std::vector<double>& psd)
    {
        const double binHz = c_fs / c_fftSize;
        double sum = 0.0, squares = 0.0;
        int n = 0;
        for (size_t k = 1; k < psd.size(); ++k)
        {
            const double f = static_cast<double>(k) * binHz;
            if (f < 300.0 || f > 8000.0)
                continue;
            const auto lo = static_cast<size_t>(f / std::pow(2.0, 1.0 / 12.0) / binHz), hi = static_cast<size_t>(f * std::pow(2.0, 1.0 / 12.0) / binHz);
            double mean = 0.0;
            for (size_t j = lo; j <= hi; ++j)
                mean += psd[j];
            mean /= static_cast<double>(hi - lo + 1);
            const double d = db(psd[k]) - db(mean);
            sum += d;
            squares += d * d;
            ++n;
        }
        const double m = sum / n;
        return std::sqrt(std::max(0.0, squares / n - m * m));
    }

    /** Cross-correlation by FFT: each signal's spectrum once, then any pair. */
    struct Correlator
    {
        static constexpr int c_order = 18, c_size = 1 << c_order; // 5.5 s: the signals (up to half of it) and their delays
        juce::dsp::FFT fft { c_order };
        std::vector<std::vector<juce::dsp::Complex<float>>> spectra;
        std::vector<double> energies;
        double scale = 1.0; // what the inverse transform leaves on top of a plain sum (measured once)

        void add(const Signal& x)
        {
            std::vector<juce::dsp::Complex<float>> in(static_cast<size_t>(c_size)), out(static_cast<size_t>(c_size));
            const size_t n = std::min(x.size(), static_cast<size_t>(c_size / 2));
            double e = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                in[i] = { x[i], 0.0f };
                e += static_cast<double>(x[i]) * x[i];
            }
            fft.perform(in.data(), out.data(), false);
            spectra.push_back(std::move(out));
            energies.push_back(e);
            if (spectra.size() == 1)
                scale = raw(0, 0)[0] / e; // a signal against itself, no delay: its energy
        }
        std::vector<double> raw(size_t a, size_t b)
        {
            std::vector<juce::dsp::Complex<float>> prod(static_cast<size_t>(c_size)), out(static_cast<size_t>(c_size));
            for (size_t k = 0; k < prod.size(); ++k)
                prod[k] = spectra[a][k] * std::conj(spectra[b][k]);
            fft.perform(prod.data(), out.data(), true);
            std::vector<double> r(out.size());
            for (size_t k = 0; k < r.size(); ++k)
                r[k] = out[k].real();
            return r;
        }
        /** The largest |correlation| of a and b at any delay of either up to maxLag samples; atZero: with no delay. */
        double largest(size_t a, size_t b, int maxLag, double* atZero = nullptr)
        {
            const auto r = raw(a, b);
            const double norm = scale * std::sqrt(energies[a] * energies[b]);
            double worst = 0.0;
            for (int lag = -maxLag; lag <= maxLag; ++lag)
                worst = std::max(worst, std::abs(r[static_cast<size_t>((lag + c_size) % c_size)]) / norm);
            if (atZero != nullptr)
                *atZero = std::abs(r[0]) / norm;
            return worst;
        }
    };

    //==================================================================================================================
    void writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(file);
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream, c_fs, static_cast<unsigned int>(audio.getNumChannels()), 24, {}, 0));
        if (w != nullptr) w->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples()); else delete stream;
    }

    Signal noise(int samples, int seed)
    {
        juce::Random r(seed);
        Signal x(static_cast<size_t>(samples));
        for (auto& v : x) v = r.nextFloat() * 2.0f - 1.0f;
        return x;
    }

    struct Sources
    {
        juce::File hissA, hissB, tone60, rumble;
    };

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
                b.setSample(0, i, 0.5f * static_cast<float>(std::sin(c_twoPi * 60.0 * i / c_fs)));
            s.tone60 = folder.getChildFile("tone60.wav");
            writeWav(s.tone60, b);
        }
        {
            // A steady rumble: noise, mostly under 200 Hz, so every band of the LFE has something in it.
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
    // The engine on its own, every channel of the room.
    struct Take
    {
        std::array<Signal, c_wide> ch;
        Signal stem;
        size_t samples() const { return ch[0].size(); }
    };

    struct Scene
    {
        CargoHold hold;
        RigEngine rig;
        RigEngine::Params p;
        double rate = c_fs;

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
        void stowSub()
        {
            p.stow.on = true;
            p.stow.mode = RigEngine::StowSub;
            p.stow.follow = 0.0f;
        }
        void start(room::Width width, double sampleRate = c_fs)
        {
            rate = sampleRate;
            rig.setCargoHold(&hold);
            rig.setWidth(width);
            rig.setParams(p);
            rig.prepare(rate, c_block);
            rig.setParams(p);
        }
        Take run(double seconds, const std::function<void(int)>& before = {})
        {
            Take take;
            const int blocks = static_cast<int>(seconds * rate / c_block);
            for (auto& c : take.ch)
                c.assign(static_cast<size_t>(blocks * c_block), 0.0f);
            take.stem.assign(static_cast<size_t>(blocks * c_block), 0.0f);
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
                std::copy(rig.getSubStem(), rig.getSubStem() + c_block, take.stem.begin() + b * c_block);
            }
            return take;
        }
    };

    /** A bed of two hisses on bus A and bus B flying `path`, as a room of `width` or Stereo, with Space at `space`. */
    Take bed(const Sources& src, room::Width width, float space, room::Path path, double seconds, const std::function<void(RigEngine::Params&)>& tweak = {})
    {
        Scene scene;
        scene.load(0, src.hissA, 0);
        scene.load(1, src.hissB, 1);
        scene.p.space = space;
        for (auto& b : scene.p.buses)
            b.path = static_cast<int>(path);
        if (tweak)
            tweak(scene.p);
        scene.start(width);
        return scene.run(seconds);
    }

    /** What Space added: a render with Space less the same render without it, its dry eased as Space eases it. */
    Take wetOf(const Take& with, const Take& without, float space)
    {
        Take wet;
        const float dry = 1.0f - c_spaceDryTrim * space * space;
        for (size_t c = 0; c < wet.ch.size(); ++c)
        {
            wet.ch[c].resize(with.samples());
            for (size_t i = 0; i < with.samples(); ++i)
                wet.ch[c][i] = with.ch[c][i] - dry * without.ch[c][i];
        }
        return wet;
    }

    Signal sumOf(const Take& take, const std::vector<int>& channels)
    {
        Signal s(take.samples(), 0.0f);
        for (int c : channels)
            for (size_t i = 0; i < s.size(); ++i)
                s[i] += take.ch[static_cast<size_t>(c)][i];
        return s;
    }

    std::vector<int> mainsOf(room::Width w)
    {
        std::vector<int> m;
        const auto& lay = room::layout(w);
        for (int c = 0; c < lay.numChannels; ++c)
            if (c != lay.lfe)
                m.push_back(c);
        return m;
    }

    /** A take folded to Stereo with the room's own matrix (the LFE dropped). */
    std::array<Signal, 2> foldToStereo(const Take& take, room::Width w)
    {
        const auto& m = room::fold(w, room::Width::Stereo);
        std::array<Signal, 2> out { Signal(take.samples(), 0.0f), Signal(take.samples(), 0.0f) };
        for (int o = 0; o < 2; ++o)
            for (int c = 0; c < m.numIn; ++c)
                if (const float g = m.gain[static_cast<size_t>(o)][static_cast<size_t>(c)]; g != 0.0f)
                    for (size_t i = 0; i < take.samples(); ++i)
                        out[static_cast<size_t>(o)][i] += g * take.ch[static_cast<size_t>(c)][i];
        return out;
    }

    //==================================================================================================================
    // The plug-in, for the output stage.
    using Proc = SwooshbucklerAudioProcessor;

    struct Plug
    {
        std::unique_ptr<juce::AudioProcessor> base { createPluginFilter() };
        Proc& proc { *dynamic_cast<Proc*>(base.get()) };

        Plug(const Sources& src, room::Width width, const std::function<void(Plug&)>& setUp)
        {
            juce::AudioProcessor::BusesLayout layout;
            layout.outputBuses.add(room::channelSet(width));
            if (! proc.setBusesLayout(layout))
                check(false, "the bus takes " + nameOf(width));
            juce::String error;
            if (! proc.getCargo().loadFile(0, src.rumble, error))
                check(false, "loads the rumble: " + error);
            proc.getCargo().setBusOverride(0, CargoHold::c_stowBus);
            if (! proc.getCargo().loadFile(1, src.hissA, error))
                check(false, "loads the hiss: " + error);
            proc.getCargo().setBusOverride(1, 0);
            setPlain(ParamIDs::stowOn, 1.0f);
            setPlain(ParamIDs::stowMode, static_cast<float>(ParamIDs::StowSub));
            setPlain(ParamIDs::space, 0.0f);
            if (setUp)
                setUp(*this);
            proc.prepareToPlay(c_fs, 512);
        }
        void setPlain(const char* id, float plain)
        {
            auto* param = proc.getState().getParameter(id);
            param->setValueNotifyingHost(param->convertTo0to1(plain));
        }
        juce::AudioBuffer<float> run(double seconds, float* lfeGr = nullptr)
        {
            proc.setPlaying(true);
            const int channels = proc.getTotalNumOutputChannels();
            const int blocks = static_cast<int>(seconds * c_fs / 512);
            juce::AudioBuffer<float> out(channels, blocks * 512), block(channels, 512);
            juce::MidiBuffer midi;
            for (int b = 0; b < blocks; ++b)
            {
                block.clear();
                proc.processBlock(block, midi);
                if (lfeGr != nullptr)
                    *lfeGr = std::min(*lfeGr, proc.getLfeGainReductionDb());
                for (int ch = 0; ch < channels; ++ch)
                    out.copyFrom(ch, b * 512, block, ch, 0, 512);
            }
            return out;
        }
    };

    float maxDiff(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int ch)
    {
        float m = 0.0f;
        for (int i = 0; i < a.getNumSamples(); ++i)
            m = std::max(m, std::abs(a.getSample(ch, i) - b.getSample(ch, i)));
        return m;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("SwooshbucklerSpaceLfeTest");
    folder.createDirectory();
    const auto src = makeSources(folder);

    juce::PropertiesFile::Options options;
    options.applicationName = "SwooshbucklerSpaceLfeTest";
    options.filenameSuffix = ".settings";
    const auto settingsFile = folder.getChildFile("test.settings");
    settingsFile.deleteFile();
    juce::PropertiesFile settings(settingsFile, options);
    settings.setValue("recordFolder", folder.getChildFile("tape").getFullPathName());
    Proc::useSettings(&settings);

    //==================================================================================================================
    std::cout << "The room's reverb on its own, against the stereo one" << std::endl;
    {
        const int n = static_cast<int>(c_fs * 6.0);
        const size_t settle = static_cast<size_t>(c_fs * 1.0);
        for (float space : { 0.3f, 1.0f })
        {
            // Stereo: two unrelated noises in L and R, wet only.
            SpaceReverb stereo;
            stereo.prepare(c_fs);
            stereo.setDecay(SpaceReverb::decaySecondsFor(space));
            Signal l = noise(n, 1), r = noise(n, 2);
            const double inPower = energy(l) + energy(r);
            stereo.process(l.data(), r.data(), n, 0.0f, 0.0f, 1.0f, 1.0f);
            const double stereoWet = (energy(l, settle) + energy(r, settle)) / inPower;
            const auto stereoL = spectrum(l, settle);
            // The stereo reverb's own L and R, for comparison: correlated 0.22 by design (mid + 0.8 side, mid - 0.8 side).
            double stereoZero = 0.0, stereoAny = 0.0;
            {
                Correlator corr;
                corr.add(Signal(l.begin() + static_cast<long>(settle), l.end()));
                corr.add(Signal(r.begin() + static_cast<long>(settle), r.end()));
                stereoAny = corr.largest(0, 1, 2400, &stereoZero);
            }

            for (auto w : { room::Width::Quad, room::Width::Surround51, room::Width::Surround71, room::Width::Surround714 })
            {
                const auto& lay = room::layout(w);
                const auto mains = mainsOf(w);
                // The same power, shared by every main speaker, each its own noise; the taps even and adding up
                // to the stereo reverb's.
                std::array<Signal, c_wide> ch;
                double roomIn = 0.0;
                for (int c = 0; c < c_wide; ++c)
                {
                    ch[static_cast<size_t>(c)] = Signal(static_cast<size_t>(n), 0.0f);
                    if (std::find(mains.begin(), mains.end(), c) != mains.end())
                    {
                        auto x = noise(n, 100 + c);
                        const float g = std::sqrt(2.0f / static_cast<float>(mains.size()));
                        for (int i = 0; i < n; ++i)
                            ch[static_cast<size_t>(c)][static_cast<size_t>(i)] = g * x[static_cast<size_t>(i)];
                        roomIn += energy(ch[static_cast<size_t>(c)]);
                    }
                }
                // Taps as the engine sets them with nothing heard yet: L and R a mid row and a side row, the rest a
                // row each (see RigEngine::roomReverbTaps), evenly.
                RoomReverb::Taps taps {};
                const float g = std::sqrt(RoomReverb::c_stereoTapPower / static_cast<float>(mains.size()));
                int row = 3;
                for (int c : mains)
                {
                    const auto s = lay.speakers[static_cast<size_t>(c)];
                    if (s == room::Speaker::L || s == room::Speaker::R)
                    {
                        taps[static_cast<size_t>(c)][1] = g * 0.78086881f;
                        taps[static_cast<size_t>(c)][2] = (s == room::Speaker::L ? 0.8f : -0.8f) * g * 0.78086881f;
                    }
                    else
                        taps[static_cast<size_t>(c)][static_cast<size_t>(row++)] = g;
                }
                RoomReverb reverb;
                reverb.prepare(c_fs);
                reverb.setDecay(SpaceReverb::decaySecondsFor(space));
                reverb.snapTaps(taps);
                float* ptr[c_wide];
                for (int c = 0; c < c_wide; ++c)
                    ptr[c] = ch[static_cast<size_t>(c)].data();
                reverb.process(ptr, lay.numChannels, n, 0.0f, 0.0f, 1.0f, 1.0f, taps);
                double roomWet = 0.0;
                for (int c : mains)
                    roomWet += energy(ch[static_cast<size_t>(c)], settle);
                roomWet /= roomIn;
                const double apart = db(roomWet) - db(stereoWet);

                // Every pair of returns: uncorrelated at every delay up to 50 ms (L and R excepted at no delay:
                // they are the stereo pair's mid and side, 0.22 as in stereo).
                double worst = 0.0, worstZero = 0.0;
                juce::String worstPair;
                Signal lfeOut = ch[static_cast<size_t>(std::max(0, lay.lfe))];
                Correlator corr;
                for (int c : mains)
                    corr.add(Signal(ch[static_cast<size_t>(c)].begin() + static_cast<long>(settle), ch[static_cast<size_t>(c)].end()));
                for (size_t i = 0; i < mains.size(); ++i)
                    for (size_t j = i + 1; j < mains.size(); ++j)
                    {
                        const auto a = lay.speakers[static_cast<size_t>(mains[i])], b = lay.speakers[static_cast<size_t>(mains[j])];
                        if ((a == room::Speaker::L && b == room::Speaker::R) || (a == room::Speaker::R && b == room::Speaker::L))
                            continue;
                        double zero = 0.0;
                        if (const double c = corr.largest(i, j, 2400, &zero); c > worst)
                        {
                            worst = c;
                            worstPair = juce::String(room::info(a).name) + "/" + room::info(b).name;
                        }
                        worstZero = std::max(worstZero, zero);
                    }

                // The fold to Stereo: as smooth in frequency as one speaker's return, and as loud as the fold of
                // the room's own power would be (each step at -3 dB adds power, not amplitude).
                Take t;
                for (int c = 0; c < c_wide; ++c)
                    t.ch[static_cast<size_t>(c)] = ch[static_cast<size_t>(c)];
                const auto folded = foldToStereo(t, w);
                const auto foldL = spectrum(folded[0], settle);
                const double foldRipple = ripple(foldL), oneRipple = ripple(spectrum(ch[0], settle));
                // ...and a fold that adds a speaker to a 10 ms late copy of another's reverb (what reading the
                // lines a little earlier for the tops would do) has a comb the ripple measure sees.
                Signal combed = folded[0];
                for (size_t i = 480; i < combed.size(); ++i)
                    combed[i] += 0.7071f * ch[0][i - 480];
                const double combRipple = ripple(spectrum(combed, settle));
                // Third-octave bands 200 Hz .. 8 kHz of the fold's L against the stereo reverb's L.
                double lo = 1.0e9, hi = -1.0e9;
                juce::String bands;
                for (double f = 200.0; f <= 8000.0; f *= std::pow(2.0, 1.0 / 3.0))
                {
                    const double d = db(third(foldL, f)) - db(third(stereoL, f));
                    lo = std::min(lo, d);
                    hi = std::max(hi, d);
                    bands << num(d, 1) << " ";
                }
                if (w == room::Width::Surround51)
                    std::cout << "         (5.1 fold's L against stereo's L, 200 Hz .. 8 kHz by third octave: " << bands << "dB)" << std::endl;
                std::cout << "         Space " << num(space * 100.0, 0) << "%, " << nameOf(w) << ": all speakers " << num(apart, 2)
                          << " dB against stereo's L + R; correlation of any two speakers at no delay up to " << num(worstZero, 3) << ", at any delay up to "
                          << num(worst, 3) << " (" << worstPair << "; stereo's L and R " << num(stereoZero, 3) << " / " << num(stereoAny, 3) << "); fold ripple "
                          << num(foldRipple, 2) << " dB (one speaker " << num(oneRipple, 2) << ", a combed fold " << num(combRipple, 2)
                          << "); fold's L against stereo's L by third octave " << num(lo, 1) << " .. " << num(hi, 1) << " dB" << std::endl;
                check(std::abs(apart) < 0.5, "Space " + num(space * 100.0, 0) + "%, " + nameOf(w) + ": the room's reverb, all speakers together, within 0.5 dB of the stereo reverb's L + R");
                check(worstZero < 0.03 && worst < 0.16 && worst < stereoAny,
                      "Space " + num(space * 100.0, 0) + "%, " + nameOf(w) + ": every two speakers' reverb uncorrelated (under 0.03), and under 0.16 at any delay up to 50 ms (less than stereo's L and R)");
                check(foldRipple < oneRipple + 0.3 && combRipple > foldRipple + 0.4,
                      "Space " + num(space * 100.0, 0) + "%, " + nameOf(w) + ": the fold to Stereo has no comb (as smooth as one speaker; a combed fold is caught)");
                // (Two different reverbs, eight lines and sixteen: their bands differ by chance by a dB or so.)
                check(hi - lo < 2.5, "Space " + num(space * 100.0, 0) + "%, " + nameOf(w) + ": the fold sounds like the stereo reverb (its third-octave balance within 2.5 dB)");
                check(lay.lfe < 0 || peakOf(lfeOut) == 0.0f, nameOf(w) + ": nothing in the LFE");
            }
        }
        // The taps glide: from one set to another across the call, never in a step.
        {
            RoomReverb reverb;
            reverb.prepare(c_fs);
            RoomReverb::Taps a {}, b {};
            a[0][1] = 2.0f;
            b[0][4] = 2.0f;
            Signal x = noise(48000, 7), y(48000, 0.0f);
            float* ptr[1] = { x.data() };
            reverb.snapTaps(a);
            reverb.process(ptr, 1, 24000, 0.0f, 0.0f, 1.0f, 1.0f, a);
            float* ptr2[1] = { x.data() + 24000 };
            reverb.process(ptr2, 1, 2400, 0.0f, 0.0f, 1.0f, 1.0f, b);
            check(reverb.taps()[0][4] == 2.0f && reverb.taps()[0][1] == 0.0f, "the taps reach the new set at the end of the call (and glide across it)");
        }
    }

    //==================================================================================================================
    std::cout << "Space in the rig, in a room" << std::endl;
    {
        constexpr double seconds = 12.0;
        constexpr float space = 0.5f;
        const Take stereoWith = bed(src, room::Width::Stereo, space, room::Path::Across, seconds);
        const Take stereoWithout = bed(src, room::Width::Stereo, 0.0f, room::Path::Across, seconds);
        const Take stereoWet = wetOf(stereoWith, stereoWithout, space);
        const double stereoWetE = energy(stereoWet.ch[0]) + energy(stereoWet.ch[1]);
        const double stereoDryE = energy(stereoWithout.ch[0]) + energy(stereoWithout.ch[1]);
        for (auto w : { room::Width::Quad, room::Width::Surround51, room::Width::Surround71, room::Width::Surround712, room::Width::Surround714 })
        {
            const auto& lay = room::layout(w);
            const auto mains = mainsOf(w);
            auto centreOn = [](RigEngine::Params& p) { p.roomCentre = true; };
            const Take with = bed(src, w, space, room::Path::Across, seconds, centreOn);
            const Take without = bed(src, w, 0.0f, room::Path::Across, seconds, centreOn);
            const Take wet = wetOf(with, without, space);
            double wetE = 0.0, dryE = 0.0, quietest = 1.0e30;
            juce::String quietestName;
            for (int c : mains)
            {
                const double e = energy(wet.ch[static_cast<size_t>(c)]);
                wetE += e;
                dryE += energy(without.ch[static_cast<size_t>(c)]);
                if (e < quietest)
                {
                    quietest = e;
                    quietestName = room::info(lay.speakers[static_cast<size_t>(c)]).name;
                }
            }
            const double apart = db(wetE) - db(stereoWetE);
            const double share = db(quietest / (wetE / static_cast<double>(mains.size())));
            const float lfePeak = lay.lfe >= 0 ? std::max(peakOf(with.ch[static_cast<size_t>(lay.lfe)]), peakOf(without.ch[static_cast<size_t>(lay.lfe)])) : 0.0f;
            std::cout << "         " << nameOf(w) << " (Across, Centre Speaker on): Space's sound " << num(apart, 2) << " dB against Stereo's (the dry "
                      << num(db(dryE) - db(stereoDryE), 2) << " dB); quietest speaker " << quietestName << " " << num(share, 1)
                      << " dB against an even share; LFE peak " << lfePeak << std::endl;
            check(std::abs(apart) < 1.0, nameOf(w) + ": the same Space is as loud as in Stereo (all speakers together within 1 dB)");
            check(share > -15.0, nameOf(w) + ": reverb in every main speaker (the quietest within 15 dB of an even share)");
            check(lfePeak == 0.0f, nameOf(w) + ": none in the LFE");

            if (w == room::Width::Surround51 || w == room::Width::Surround714)
            {
                // Folded to Stereo: the reverb keeps its share of the sound (its level against the dry's, as the
                // fold changes both). Centre Speaker off: a centre speaker folds into L and R in step with them,
                // which raises the dry alone.
                const Take with2 = bed(src, w, space, room::Path::Across, seconds);
                const Take without2 = bed(src, w, 0.0f, room::Path::Across, seconds);
                const auto foldWet = foldToStereo(wetOf(with2, without2, space), w), foldDry = foldToStereo(without2, w);
                const double ratio = (db(energy(foldWet[0]) + energy(foldWet[1])) - db(energy(foldDry[0]) + energy(foldDry[1])))
                                   - (db(stereoWetE) - db(stereoDryE));
                const double level = db(energy(foldWet[0]) + energy(foldWet[1])) - db(stereoWetE);
                std::cout << "         " << nameOf(w) << " folded to Stereo: the reverb " << num(level, 2) << " dB against Stereo's, its share of the sound "
                          << num(ratio, 2) << " dB against Stereo's" << std::endl;
                // The fold takes each step at -3 dB, which keeps the level of a sound between two speakers but
                // halves the power of what is different in them, as the reverbs are: the wider the room, the more
                // of its reverb is in speakers that fold in two or three steps down.
                check(std::abs(level) < 2.5 && ratio > -3.5, nameOf(w) + " folded to Stereo: the reverb within 2.5 dB of Stereo's, its share of the sound within 3.5 dB");
            }
        }

        // Height: the tops' share of the reverb (Across flies at ear level, so the tops have nothing else).
        {
            const auto& lay = room::layout(room::Width::Surround714);
            const int tops[] = { lay.indexOf(room::Speaker::Ltf), lay.indexOf(room::Speaker::Rtf), lay.indexOf(room::Speaker::Ltr), lay.indexOf(room::Speaker::Rtr) };
            double share[2] {};
            float peak[2] {};
            for (int h = 0; h < 2; ++h)
            {
                const Take take = bed(src, room::Width::Surround714, space, room::Path::Across, 8.0, [h](RigEngine::Params& p) { p.roomHeight = h == 0 ? 0.0f : 1.0f; });
                double all = 0.0, top = 0.0;
                for (int c : mainsOf(room::Width::Surround714))
                    all += energy(take.ch[static_cast<size_t>(c)]);
                for (int c : tops)
                {
                    top += energy(take.ch[static_cast<size_t>(c)]);
                    peak[h] = std::max(peak[h], peakOf(take.ch[static_cast<size_t>(c)]));
                }
                share[h] = top / all;
            }
            std::cout << "         7.1.4, Across: the tops carry " << num(100.0 * share[0], 2) << "% of the sound at Height 0 (peak " << peak[0]
                      << "), " << num(100.0 * share[1], 1) << "% at Height 100%" << std::endl;
            check(peak[0] == 0.0f && share[1] > 0.04, "Height moves reverb into the tops (none at 0, more than 4% of all the sound at 100%)");
        }

        // The lean: a room's reverb is louder near where the passes fly, and still fills the room.
        {
            const auto& lay = room::layout(room::Width::Surround51);
            const int front[] = { lay.indexOf(room::Speaker::L), lay.indexOf(room::Speaker::R) };
            const int back[] = { lay.indexOf(room::Speaker::Ls), lay.indexOf(room::Speaker::Rs) };
            double frontShare[2] {};
            const room::Path paths[] = { room::Path::Across, room::Path::Through };
            for (int k = 0; k < 2; ++k)
            {
                const Take with = bed(src, room::Width::Surround51, space, paths[k], seconds);
                const Take without = bed(src, room::Width::Surround51, 0.0f, paths[k], seconds);
                const Take wet = wetOf(with, without, space);
                double f = 0.0, b = 0.0;
                for (int c : front) f += energy(wet.ch[static_cast<size_t>(c)]);
                for (int c : back) b += energy(wet.ch[static_cast<size_t>(c)]);
                frontShare[k] = f / (f + b);
            }
            std::cout << "         5.1: the front pair's share of the reverb, Across " << num(100.0 * frontShare[0], 1) << "%, Through "
                      << num(100.0 * frontShare[1], 1) << "% (even: 50%)" << std::endl;
            check(frontShare[0] > 0.62 && frontShare[0] < 0.9 && frontShare[1] < frontShare[0] - 0.08,
                  "the reverb leans toward where the passes fly (Across: the front pair over 62%) and fills the room (the back over 10%)");
        }

        // Centre Speaker off: the centre speaker carries nothing, the reverb included.
        {
            const Take take = bed(src, room::Width::Surround51, space, room::Path::Across, 4.0);
            check(peakOf(take.ch[static_cast<size_t>(room::layout(room::Width::Surround51).centre)]) == 0.0f,
                  "Centre Speaker off: the centre speaker stays silent, Space's reverb too");
        }

        // Space coming on from 0 under a steady low tone: the echoes start without a click (an echo later).
        for (auto w : { room::Width::Stereo, room::Width::Surround51 })
        {
            Scene scene;
            scene.load(0, src.tone60, 0);
            scene.p.buses[0].width = 0.0f; // the pass stays put: only Space changes
            scene.p.buses[0].doppler = 0.0f;
            scene.p.buses[0].bend = 0.0f;
            scene.start(w);
            const int at = static_cast<int>(2.0 * c_fs / c_block);
            const Take take = scene.run(4.0, [&](int b) { if (b == at) scene.p.space = 0.6f; });
            auto changeOfStep = [&](size_t from, size_t to)
            {
                float worst = 0.0f;
                for (int c = 0; c < 2; ++c)
                {
                    const auto& x = take.ch[static_cast<size_t>(c)];
                    for (size_t i = std::max<size_t>(from, 2); i < to; ++i)
                        worst = std::max(worst, std::abs(x[i] - 2.0f * x[i - 1] + x[i - 2]));
                }
                return worst;
            };
            const float peak = std::max(peakOf(take.ch[0]), peakOf(take.ch[1]));
            const size_t on = static_cast<size_t>(at * c_block);
            const float steady = changeOfStep(on - static_cast<size_t>(c_fs), on);
            const float after = changeOfStep(on, on + static_cast<size_t>(1.5 * c_fs));
            std::cout << "         " << nameOf(w) << ": Space 0 -> 60% under a 60 Hz tone: largest change of step " << num(100.0 * after / peak, 3)
                      << "% of the peak in the next 1.5 s (steady " << num(100.0 * steady / peak, 3) << "%)" << std::endl;
            check(after < 0.01f * peak, nameOf(w) + ": Space coming on from 0 makes no click (the echoes' send fades in)");
        }
    }

    //==================================================================================================================
    std::cout << "The LFE" << std::endl;
    {
        const double rumbleSeconds = 8.0;
        auto subScene = [&](room::Width w, int mode, float levelDb, const std::function<void(RigEngine::Params&)>& tweak = {})
        {
            Scene scene;
            scene.load(0, src.rumble, CargoHold::c_stowBus);
            scene.stowSub();
            scene.p.lfeMode = mode;
            scene.p.lfeLevelDb = levelDb;
            if (tweak)
                tweak(scene.p);
            scene.start(w);
            return scene.run(rumbleSeconds);
        };
        const size_t settle = static_cast<size_t>(c_fs * 0.5);

        for (auto w : { room::Width::Surround51, room::Width::Surround714 })
        {
            const auto& lay = room::layout(w);
            const auto lfe = static_cast<size_t>(lay.lfe);
            const Take off = subScene(w, RigEngine::LfeOff, -10.0f);
            const Take discrete = subScene(w, RigEngine::LfeDiscrete, -10.0f);
            const Take mixdown = subScene(w, RigEngine::LfeMixdown, -10.0f);
            check(peakOf(off.ch[lfe]) == 0.0f && peakOf(off.ch[0]) > 0.05f, nameOf(w) + " Off: the LFE is silent (with the Sub sounding in L and R)");

            // The mains are the same whatever the mode.
            bool same = true;
            for (int c : mainsOf(w))
                same = same && off.ch[static_cast<size_t>(c)] == discrete.ch[static_cast<size_t>(c)] && off.ch[static_cast<size_t>(c)] == mixdown.ch[static_cast<size_t>(c)];
            check(same, nameOf(w) + ": the main speakers are exactly the same in Off, Discrete and Mixdown (nothing is taken from them)");

            // Discrete: exactly the Sub stem through the LFE's low-pass, at LFE Level, then the last DC blocker.
            {
                lfe::LowPass lp;
                lp.prepare(c_fs);
                sj::DcBlock dc;
                dc.set(8.0f, static_cast<float>(c_fs));
                const float level = juce::Decibels::decibelsToGain(-10.0f);
                float worst = 0.0f;
                for (size_t i = 0; i < discrete.samples(); ++i)
                    worst = std::max(worst, std::abs(dc.process(lp.process(level * discrete.stem[i])) - discrete.ch[lfe][i]));
                std::cout << "         " << nameOf(w) << " Discrete: LFE peak " << num(peakOf(discrete.ch[lfe]), 3) << ", against the Sub stem low-passed "
                          << worst << std::endl;
                check(peakOf(discrete.ch[lfe]) > 0.02f && worst < 1.0e-5f, nameOf(w) + " Discrete: the LFE is Stowaway's Sub through the LFE's low-pass at LFE Level");
            }
            // ...and only from the Sub: nothing with Stowaway off, or in another mode, though the rig sounds.
            {
                Scene scene;
                scene.load(0, src.rumble, CargoHold::c_stowBus);
                scene.load(1, src.hissA, 0);
                scene.p.stow.on = true;
                scene.p.stow.mode = RigEngine::StowDry;
                scene.p.lfeMode = RigEngine::LfeDiscrete;
                scene.start(w);
                const Take dry = scene.run(3.0);
                Scene scene2;
                scene2.load(0, src.rumble, CargoHold::c_stowBus);
                scene2.load(1, src.hissA, 0);
                scene2.p.stow.on = false;
                scene2.p.lfeMode = RigEngine::LfeDiscrete;
                scene2.start(w);
                const Take offStow = scene2.run(3.0);
                check(peakOf(dry.ch[lfe]) == 0.0f && peakOf(offStow.ch[lfe]) == 0.0f && peakOf(dry.ch[0]) > 0.05f,
                      nameOf(w) + " Discrete: the LFE is silent with Stowaway in Dry, or off");
            }

            // Mixdown: the low end of half the mains' sum, at LFE Level.
            {
                Signal half = sumOf(mixdown, mainsOf(w));
                for (auto& v : half) v *= 0.5f * juce::Decibels::decibelsToGain(-10.0f);
                const auto pm = spectrum(half, settle), pl = spectrum(mixdown.ch[lfe], settle);
                double worst = 0.0;
                for (double f : { 25.0, 31.5, 40.0, 50.0, 63.0 })
                    worst = std::max(worst, std::abs(db(third(pl, f)) - db(third(pm, f))));
                const double at120 = db(band(pl, 115.0, 125.0)) - db(band(pm, 115.0, 125.0));
                std::cout << "         " << nameOf(w) << " Mixdown: LFE against half the mains' sum, 25 - 63 Hz within " << num(worst, 2) << " dB, at 120 Hz "
                          << num(at120, 1) << " dB" << std::endl;
                check(worst < 0.5 && std::abs(at120 + 6.0) < 1.0, nameOf(w) + " Mixdown: the LFE follows the mains' low end (within 0.5 dB under 63 Hz, -6 dB at 120 Hz)");
            }

            // Nothing above the LFE's low-pass beyond its slope, though the Sub's Drive and Low End add overtones.
            {
                // (The crossover at its top, 250 Hz, so the overtones reach well past the LFE's 120 Hz.)
                const Take driven = subScene(w, RigEngine::LfeDiscrete, 0.0f, [](RigEngine::Params& p) { p.stow.drive = 0.9f; p.stow.low = 0.8f; p.stow.filter = 1.0f; });
                const auto ps = spectrum(driven.stem, settle), pl = spectrum(driven.ch[lfe], settle);
                const double stemAbove = db(band(ps, 300.0, 20000.0)) - db(band(ps, 20.0, 100.0));
                const double lfeAbove = db(band(pl, 300.0, 20000.0)) - db(band(pl, 20.0, 100.0));
                const double lfe240 = db(third(pl, 250.0)) - db(third(ps, 250.0));
                std::cout << "         " << nameOf(w) << " Discrete, Sub driven: above 300 Hz against under 100 Hz, the stem " << num(stemAbove, 1)
                          << " dB, the LFE " << num(lfeAbove, 1) << " dB; the LFE at 250 Hz against the stem " << num(lfe240, 1) << " dB" << std::endl;
                check(stemAbove > -30.0 && lfeAbove < -60.0 && lfe240 < -40.0,
                      nameOf(w) + ": nothing in the LFE above its low-pass beyond its slope (above 300 Hz under -60 dB; the stem has overtones there)");
            }

            // Bass management: the mains and the LFE (10 dB hotter) summed in one subwoofer. In step, never cancelled.
            {
                const auto mains = mainsOf(w);
                const float levels[] = { -10.0f, -20.0f, 0.0f };
                for (int k = 0; k < 3; ++k)
                {
                    const float levelDb = levels[k];
                    const bool unity = k == 0;
                    const Take take = unity ? discrete : subScene(w, RigEngine::LfeDiscrete, levelDb);
                    const Signal m = sumOf(take, mains);
                    Signal managed = m, naive(m.size(), 0.0f);
                    sj::DcBlock dc;
                    dc.set(8.0f, static_cast<float>(c_fs));
                    for (size_t i = 0; i < m.size(); ++i)
                    {
                        const float lfeOut = 3.1622777f * take.ch[lfe][i];
                        managed[i] += lfeOut;
                        naive[i] = 2.0f * dc.process(take.stem[i]) + lfeOut; // the mains' copy without the phase match
                    }
                    const auto pm = spectrum(m, settle), ps = spectrum(managed, settle), pn = spectrum(naive, settle);
                    double lo = 1.0e9, hi = -1.0e9, naiveLo = 1.0e9, low = 0.0;
                    juce::String line;
                    for (double f : { 31.5, 40.0, 50.0, 63.0, 80.0, 100.0, 125.0 })
                    {
                        const double d = db(third(ps, f)) - db(third(pm, f));
                        lo = std::min(lo, d);
                        hi = std::max(hi, d);
                        naiveLo = std::min(naiveLo, db(third(pn, f)) - db(third(pm, f)));
                        if (f <= 50.0)
                            low = std::max(low, std::abs(d - 20.0 * std::log10(1.0 + 0.5 * std::pow(10.0, (levelDb + 10.0) / 20.0))));
                        line << num(d, 1) << " ";
                    }
                    const double inPhase = 20.0 * std::log10(1.0 + 0.5 * std::pow(10.0, (levelDb + 10.0) / 20.0));
                    std::cout << "         " << nameOf(w) << " Discrete at " << num(levelDb, 0) << " dB, bass managed against the mains alone, 31.5 - 125 Hz: "
                              << line << "dB (in step: +" << num(inPhase, 1) << " under 80 Hz; the same copies without the phase match dip to "
                              << num(naiveLo, 1) << " dB)" << std::endl;
                    check(lo > -0.1 && hi < inPhase + 0.2 && low < 0.3,
                          nameOf(w) + " Discrete at " + num(levelDb, 0) + " dB: bass management adds the LFE in step and never cancels (31.5 - 125 Hz between 0 and +"
                          + num(inPhase, 1) + " dB)");
                    if (unity)
                    {
                        check(hi < 3.7, nameOf(w) + " Discrete at Unity: the Sub is weighted, not doubled (+3.5 dB, not +6)");
                        check(naiveLo < -1.0, nameOf(w) + ": without the phase match the same copies would cancel in part (the test can see it)");
                    }
                }
            }
        }

        // LFE Mode and Level glide: a change mid-way never steps the LFE.
        {
            Scene scene;
            scene.load(0, src.rumble, CargoHold::c_stowBus);
            scene.stowSub();
            scene.p.lfeMode = RigEngine::LfeDiscrete;
            scene.start(room::Width::Surround51);
            const Take take = scene.run(6.0, [&](int b)
            {
                if (b % 150 == 75)
                    scene.p.lfeMode = (scene.p.lfeMode + 1) % RigEngine::c_lfeModes;
                if (b % 211 == 100)
                    scene.p.lfeLevelDb = scene.p.lfeLevelDb < -20.0f ? 0.0f : -40.0f;
            });
            const auto& x = take.ch[3];
            float worst = 0.0f;
            for (size_t i = 2; i < x.size(); ++i)
                worst = std::max(worst, std::abs(x[i] - 2.0f * x[i - 1] + x[i - 2]));
            std::cout << "         5.1, LFE Mode and Level changing: largest change of step " << num(100.0 * worst / std::max(1.0e-9f, peakOf(x)), 3) << "% of the peak" << std::endl;
            check(peakOf(x) > 0.02f && worst < 0.002f * peakOf(x), "LFE Mode and LFE Level glide (no step in the LFE)");
        }
        // Quad has no LFE: the mode does nothing there.
        {
            Scene scene;
            scene.load(0, src.rumble, CargoHold::c_stowBus);
            scene.stowSub();
            scene.p.lfeMode = RigEngine::LfeMixdown;
            scene.start(room::Width::Quad);
            const Take mixdown = scene.run(3.0);
            Scene scene2;
            scene2.load(0, src.rumble, CargoHold::c_stowBus);
            scene2.stowSub();
            scene2.start(room::Width::Quad);
            const Take off = scene2.run(3.0);
            bool same = true;
            for (int c = 0; c < c_wide; ++c)
                same = same && mixdown.ch[static_cast<size_t>(c)] == off.ch[static_cast<size_t>(c)];
            check(same && peakOf(off.ch[0]) > 0.05f, "Quad (no LFE): LFE Mode changes nothing");
        }
    }

    //==================================================================================================================
    std::cout << "The plug-in: the LFE's output stage, the parameters, the folds" << std::endl;
    {
        // A loud LFE (Discrete at 0 dB, pushed): its own limiter holds it; the mains are untouched by it.
        for (auto w : { room::Width::Surround51, room::Width::Surround714 })
        {
            const int lfe = room::layout(w).lfe;
            auto loud = [&](int mode) { return [mode](Plug& p)
            {
                p.setPlain(ParamIDs::roomLfeMode, static_cast<float>(mode));
                p.setPlain(ParamIDs::roomLfeLevel, 0.0f);
                p.setPlain(ParamIDs::push, 12.0f);
            }; };
            Plug offPlug(src, w, loud(RigEngine::LfeOff));
            const auto off = offPlug.run(4.0);
            float gr = 0.0f;
            Plug onPlug(src, w, loud(RigEngine::LfeDiscrete));
            const auto on = onPlug.run(4.0, &gr);
            float mainsApart = 0.0f;
            for (int c : mainsOf(w))
                mainsApart = std::max(mainsApart, maxDiff(off, on, c));
            const float lfePeak = on.getMagnitude(lfe, 0, on.getNumSamples());
            const float ceiling = juce::Decibels::decibelsToGain(onPlug.proc.getState().getParameter(ParamIDs::ceiling)->convertFrom0to1(
                                                                     onPlug.proc.getState().getParameter(ParamIDs::ceiling)->getValue()));
            std::cout << "         " << nameOf(w) << ", LFE Discrete at 0 dB, Push +12 dB: the LFE limiter's deepest reduction " << num(gr, 1)
                      << " dB, LFE peak " << num(db(static_cast<double>(lfePeak) * lfePeak), 2) << " dB (Ceiling " << num(db(static_cast<double>(ceiling) * ceiling), 2)
                      << "), the mains against LFE Off " << mainsApart << std::endl;
            check(gr < -1.0f && lfePeak <= ceiling * 1.001f, nameOf(w) + ": a loud LFE is held at the Ceiling by its own limiter");
            check(mainsApart == 0.0f, nameOf(w) + ": the LFE's limiter does not duck the mains (exactly the same as with the LFE off)");
        }
        // No Inflate on the LFE: Inflate full changes the mains and leaves the LFE as it was.
        {
            auto withInflate = [](float inflate) { return [inflate](Plug& p)
            {
                p.setPlain(ParamIDs::roomLfeMode, static_cast<float>(RigEngine::LfeDiscrete));
                p.setPlain(ParamIDs::roomLfeLevel, -20.0f);
                p.setPlain(ParamIDs::inflate, inflate);
            }; };
            Plug a(src, room::Width::Surround51, withInflate(0.0f)), b(src, room::Width::Surround51, withInflate(1.0f));
            const auto x = a.run(3.0), y = b.run(3.0);
            const float lfeApart = maxDiff(x, y, 3), mainsApart = maxDiff(x, y, 0);
            std::cout << "         5.1, Inflate 0 against 100%: the LFE apart by " << lfeApart << ", L by " << mainsApart << std::endl;
            check(lfeApart == 0.0f && mainsApart > 1.0e-3f && x.getMagnitude(3, 0, x.getNumSamples()) > 0.01f, "no Inflate on the LFE (the mains get it)");
        }
        // The parameters.
        {
            Plug p(src, room::Width::Surround51, {});
            auto* mode = p.proc.getState().getParameter(ParamIDs::roomLfeMode);
            auto* level = p.proc.getState().getParameter(ParamIDs::roomLfeLevel);
            const auto range = p.proc.getState().getParameterRange(ParamIDs::roomLfeLevel);
            const bool modeOk = mode != nullptr && mode->getCurrentValueAsText() == "Off" && mode->getNumSteps() == RigEngine::c_lfeModes
                             && mode->getName(64) == "LFE Mode";
            const bool levelOk = level != nullptr && level->getCurrentValueAsText() == "-10.0 dB (Unity)"
                              && level->getText(level->convertTo0to1(-18.5f), 64) == "-18.5 dB"
                              && juce::approximatelyEqual(level->convertFrom0to1(level->getValueForText("Unity")), RigEngine::c_lfeReferenceDb)
                              && juce::approximatelyEqual(range.start, RigEngine::c_lfeMinDb) && juce::approximatelyEqual(range.end, RigEngine::c_lfeMaxDb)
                              && juce::approximatelyEqual(ParamIDs::lfeLevelUnityDb, RigEngine::c_lfeReferenceDb);
            std::cout << "         LFE Mode reads " << mode->getCurrentValueAsText() << ", LFE Level reads " << level->getCurrentValueAsText() << std::endl;
            check(modeOk, "LFE Mode: Off, Discrete, Mixdown; Off to start with");
            check(levelOk, "LFE Level: -40 .. 0 dB, starts at -10 dB and reads \"(Unity)\" there; \"Unity\" typed in finds it");
            check(Presets::isSessionParameter(ParamIDs::roomLfeMode) && Presets::isSessionParameter(ParamIDs::roomLfeLevel),
                  "LFE Mode and Level belong to the session (no preset or Randomise moves them)");
        }
        // Which widths have an LFE (the room strip dims the LFE controls where there is none).
        {
            bool ok = true;
            for (auto w : { room::Width::Mono, room::Width::Stereo, room::Width::Quad, room::Width::Surround51, room::Width::Surround71,
                            room::Width::Surround712, room::Width::Surround714 })
            {
                Plug p(src, w, {});
                const bool want = w != room::Width::Mono && w != room::Width::Stereo && w != room::Width::Quad;
                ok = ok && p.proc.roomHasLfe() == want && room::hasLfe(w) == want;
            }
            check(ok, "has an LFE: 5.1, 7.1, 7.1.2 and 7.1.4; not Mono, Stereo or Quad");
        }
        // Folds drop the LFE: in Discrete and Mixdown the mains carry that low end already, so a fold of either
        // is the fold of the same render with the LFE off; folding the LFE in as well would count it twice.
        {
            Scene a, b;
            for (auto* s : { &a, &b })
            {
                s->load(0, src.rumble, CargoHold::c_stowBus);
                s->stowSub();
            }
            b.p.lfeMode = RigEngine::LfeDiscrete;
            a.start(room::Width::Surround51);
            b.start(room::Width::Surround51);
            const Take off = a.run(3.0), on = b.run(3.0);
            const auto f0 = foldToStereo(off, room::Width::Surround51), f1 = foldToStereo(on, room::Width::Surround51);
            const auto& m = room::fold(room::Width::Surround51, room::Width::Stereo, room::LfeFold::IntoFronts);
            Signal doubled(on.samples(), 0.0f);
            for (int c = 0; c < m.numIn; ++c)
                for (size_t i = 0; i < doubled.size(); ++i)
                    doubled[i] += m.gain[0][static_cast<size_t>(c)] * on.ch[static_cast<size_t>(c)][i];
            const double extra = db(band(spectrum(doubled), 30.0, 60.0)) - db(band(spectrum(f1[0]), 30.0, 60.0));
            std::cout << "         5.1 Discrete folded to Stereo: the same as with the LFE off; with the LFE folded in too, +" << num(extra, 1) << " dB at 30 - 60 Hz" << std::endl;
            check(f0[0] == f1[0] && f0[1] == f1[1] && extra > 1.0, "a fold drops the LFE: a Discrete render folds to exactly what LFE Off does (folding it in would add its low end twice)");
        }
    }

    //==================================================================================================================
    std::cout << "Speed: the engine with Space and the LFE" << std::endl;
    {
        for (double rate : { 48000.0, 96000.0, 192000.0 })
        {
            double percent[3] {};
            const char* names[] = { "Stereo, Space 50%", "7.1.4, Space 0", "7.1.4, Space 50% + LFE Mixdown" };
            for (int which = 0; which < 3; ++which)
            {
                Scene scene;
                scene.load(0, src.hissA, 0);
                scene.load(1, src.hissB, 1);
                scene.load(2, src.rumble, CargoHold::c_stowBus);
                scene.stowSub();
                scene.p.space = which == 1 ? 0.0f : 0.5f;
                scene.p.lfeMode = which == 2 ? RigEngine::LfeMixdown : RigEngine::LfeOff;
                scene.start(which == 0 ? room::Width::Stereo : room::Width::Surround714, rate);
                std::array<Signal, c_wide> out;
                for (auto& c : out) c.assign(c_block, 0.0f);
                float* ptr[c_wide];
                for (int c = 0; c < c_wide; ++c) ptr[c] = out[static_cast<size_t>(c)].data();
                const int blocks = static_cast<int>(10.0 * rate / c_block);
                for (int b = 0; b < blocks / 10; ++b)
                    scene.rig.process(nullptr, nullptr, ptr, c_wide, c_block);
                const double t0 = TestClock::cpuMs();
                for (int b = 0; b < blocks; ++b)
                    scene.rig.process(nullptr, nullptr, ptr, c_wide, c_block);
                percent[which] = 100.0 * (TestClock::cpuMs() - t0) / (1000.0 * blocks * c_block / rate);
            }
            std::cout << "         " << num(rate / 1000.0, 0) << " kHz: " << names[0] << " " << num(percent[0], 1) << "% of a core, " << names[1] << " "
                      << num(percent[1], 1) << "%, " << names[2] << " " << num(percent[2], 1) << "%" << std::endl;
            check(percent[2] < 50.0, num(rate / 1000.0, 0) + " kHz: the engine at 7.1.4 with Space and the LFE well under half a core");
        }
    }

    std::cout << std::endl << (g_failures == 0 ? juce::String("ALL PASSED") : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

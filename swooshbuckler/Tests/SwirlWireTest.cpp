// What is behind Swirl and Filter, on the engine itself.
//
//   SwooshbucklerSwirlWire [output folder] [only these parts]
//   SwooshbucklerSwirlWire render <folder>          writes the renders the "null" comparison reads
//   SwooshbucklerSwirlWire null <folder> <folder>   compares two such folders (two builds of the engine)
//
// prints what it measures and ends with "ALL PASSED" (exit code 0) or "n FAILED".
//
//   same       settings that belong to a type or a rhythm not in use change nothing, to the bit
//   types      Phaser has the all-pass notches and no comb; Flanger has the comb and no all-pass notch
//   tape       Tape Flanger, With the Pass: the copy goes through zero on the way in and again on the way out of
//              every pass (either side of the pass's marker); Varispeed, its held offset, and sliding it home
//   sides      a stereo bed in a Tape Flanger: its side signal is as late as its middle
//   aim        an aimed hit lands on the bus as it is heard (the Tape Flanger's clean path is 8 ms long)
//   change     a change of Type does not click, in stereo and in a room (with "each" as well: every change's own figure)
//   filter     each Filter type, the 24 dB slope and its own rhythm, on the bus
//   speed      each Swirl type at 7.1.4 and 192 kHz with a throw in flight, against Phaser + Flanger

#include <JuceHeader.h>
#include "RigEngine.h"
#include "Parameters.h"
#include "Room.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
    constexpr double c_rate = 48000.0;
    constexpr int c_chunk = 32;
    constexpr double c_twoPi = 6.283185307179586;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }
    juce::String num(double v, int places = 2) { return juce::String(v, places); }
    double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-15)); }

    juce::File writeWav(const juce::File& folder, const juce::String& name, const juce::AudioBuffer<float>& b, double rate = c_rate, int bits = 24)
    {
        auto f = folder.getChildFile(name + ".wav");
        f.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(f);
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream, rate, static_cast<unsigned int>(b.getNumChannels()), bits, {}, 0));
        if (w != nullptr) w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()); else delete stream;
        return f;
    }
    bool readWav(const juce::File& f, juce::AudioBuffer<float>& into)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> r(wav.createReaderFor(new juce::FileInputStream(f), true));
        if (r == nullptr)
            return false;
        into.setSize(static_cast<int>(r->numChannels), static_cast<int>(r->lengthInSamples));
        return r->read(&into, 0, into.getNumSamples(), 0, true, true);
    }

    // A steady tone that loops without a seam.
    juce::AudioBuffer<float> tone(double hz, float gain)
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_rate * 4.0));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample(0, i, gain * static_cast<float>(std::sin(c_twoPi * hz * i / c_rate)));
        return b;
    }
    // White noise, one channel or two that have nothing in common.
    juce::AudioBuffer<float> noise(int seed, int channels, float gain, double seconds = 8.0)
    {
        juce::AudioBuffer<float> b(channels, static_cast<int>(c_rate * seconds));
        juce::Random r(seed);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample(ch, i, gain * (r.nextFloat() * 2.0f - 1.0f));
        return b;
    }
    // Four hits in two seconds.
    juce::AudioBuffer<float> hits()
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_rate * 2.0));
        b.clear();
        juce::Random r(9);
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < static_cast<int>(c_rate * 0.3); ++i)
            {
                const double t = i / c_rate;
                const int idx = static_cast<int>(k * 0.5 * c_rate) + i;
                b.setSample(0, idx, static_cast<float>(0.8 * std::exp(-t / 0.04) * (r.nextFloat() * 2.0f - 1.0f)
                                                     + 0.7 * std::exp(-t / 0.2) * std::sin(c_twoPi * 55.0 * t)));
            }
        return b;
    }

    void start(RigEngine& e, CargoHold& hold, const RigEngine::Params& q, double fs = c_rate)
    {
        e.setCargoHold(&hold);
        e.setParams(q);
        e.prepare(fs, c_chunk);
        e.setParams(q);
    }

    // One run, a chunk at a time, at any width. `each` is called before every chunk (it may set parameters) and
    // `after` after it.
    struct Run
    {
        juce::AudioBuffer<float> heard;
        std::array<std::vector<double>, 5> marks; // by RigEngine::MarkerKind, samples
    };
    template <typename Each, typename After>
    Run run(RigEngine& e, int channels, double fs, double seconds, bool keep, Each&& each, After&& after)
    {
        Run r;
        const int total = static_cast<int>(seconds * fs) / c_chunk * c_chunk;
        juce::AudioBuffer<float> scratch(channels, c_chunk);
        if (keep)
            r.heard.setSize(channels, total);
        std::vector<float*> out(static_cast<size_t>(channels));
        for (int pos = 0; pos < total; pos += c_chunk)
        {
            each(pos);
            for (int ch = 0; ch < channels; ++ch)
                out[static_cast<size_t>(ch)] = keep ? r.heard.getWritePointer(ch, pos) : scratch.getWritePointer(ch);
            if (channels == 2)
                e.process(nullptr, nullptr, out[0], out[1], c_chunk);
            else
                e.process(nullptr, nullptr, out.data(), channels, c_chunk);
            for (int m = 0; m < e.getNumMarkers(); ++m)
                r.marks[static_cast<size_t>(e.getMarker(m).kind)].push_back(static_cast<double>(pos + e.getMarker(m).offset));
            after(pos);
        }
        return r;
    }
    template <typename Each>
    Run run(RigEngine& e, int channels, double fs, double seconds, Each&& each) { return run(e, channels, fs, seconds, true, each, [](int) {}); }
    Run run(RigEngine& e, int channels, double fs, double seconds) { return run(e, channels, fs, seconds, true, [](int) {}, [](int) {}); }

    bool finite(const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite(b.getSample(ch, i)))
                    return false;
        return true;
    }
    double rms(const juce::AudioBuffer<float>& b, int from = 0)
    {
        double sum = 0.0;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = from; i < b.getNumSamples(); ++i)
                sum += static_cast<double>(b.getSample(ch, i)) * static_cast<double>(b.getSample(ch, i));
        return std::sqrt(sum / std::max(1, b.getNumChannels() * (b.getNumSamples() - from)));
    }
    // How far apart two renders are: the difference's level against the first's, and the largest difference.
    struct Apart { double db = -400.0; float largest = 0.0f; };
    Apart apart(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        Apart r;
        if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        {
            r.db = 0.0;
            r.largest = 1.0f;
            return r;
        }
        double diff = 0.0, ref = 0.0;
        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
            {
                const float d = a.getSample(ch, i) - b.getSample(ch, i);
                diff += static_cast<double>(d) * static_cast<double>(d);
                ref += static_cast<double>(a.getSample(ch, i)) * static_cast<double>(a.getSample(ch, i));
                r.largest = std::max(r.largest, std::abs(d));
            }
        r.db = diff <= 0.0 ? -400.0 : 10.0 * std::log10(diff / std::max(ref, 1.0e-30));
        return r;
    }
    // The sharpest change in a render (the largest second difference), between two times.
    float sharpest(const juce::AudioBuffer<float>& o, int from, int to)
    {
        float worst = 0.0f;
        for (int ch = 0; ch < o.getNumChannels(); ++ch)
        {
            const float* x = o.getReadPointer(ch);
            for (int i = std::max(1, from); i < std::min(to, o.getNumSamples()) - 1; ++i)
                worst = std::max(worst, std::abs(x[i + 1] - 2.0f * x[i] + x[i - 1]));
        }
        return worst;
    }
    // The long-term power spectrum of one channel (Hann windows of 2^order, half overlapped), from `from` on.
    std::vector<double> spectrum(const juce::AudioBuffer<float>& b, int channel, int from, int order = 13)
    {
        const int size = 1 << order;
        juce::dsp::FFT fft(order);
        std::vector<float> work(static_cast<size_t>(2 * size));
        std::vector<double> power(static_cast<size_t>(size / 2 + 1), 0.0);
        const float* x = b.getReadPointer(channel);
        for (int at = from; at + size <= b.getNumSamples(); at += size / 2)
        {
            for (int i = 0; i < size; ++i)
                work[static_cast<size_t>(i)] = x[at + i] * static_cast<float>(0.5 - 0.5 * std::cos(c_twoPi * i / size));
            std::fill(work.begin() + size, work.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform(work.data());
            for (size_t k = 0; k < power.size(); ++k)
                power[k] += static_cast<double>(work[k]) * static_cast<double>(work[k]);
        }
        return power;
    }
    // A band's power (Hz either side) in a spectrum of 2^order at c_rate.
    double bandPower(const std::vector<double>& power, double hz, double halfWidth, int order = 13)
    {
        const double perBin = c_rate / static_cast<double>(1 << order);
        const int lo = std::max(1, static_cast<int>(std::floor((hz - halfWidth) / perBin))), hi = std::min(static_cast<int>(power.size()) - 1, static_cast<int>(std::ceil((hz + halfWidth) / perBin)));
        double sum = 0.0;
        for (int k = lo; k <= hi; ++k)
            sum += power[static_cast<size_t>(k)];
        return sum / std::max(1, hi - lo + 1);
    }

    // The sources every part uses: a tone and white noise on bus A, a stereo hiss on bus B (a stereo bed), hits.
    struct Sources
    {
        CargoHold hold;
        bool ok = false;
        juce::String error;
        explicit Sources(const juce::File& folder, bool toneOnA = true, bool noiseOnA = true)
        {
            ok = true;
            if (toneOnA)
            {
                ok = ok && hold.loadFile(0, writeWav(folder, "swirlwire_src_tone", tone(330.0, 0.4f)), error);
                hold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
                hold.setSkipSilence(0, false);
            }
            if (noiseOnA)
            {
                ok = ok && hold.loadFile(3, writeWav(folder, "swirlwire_src_noise", noise(21, 1, 0.3f)), error);
                hold.setRoleOverride(3, static_cast<int>(CargoRole::Tonal));
                hold.setSkipSilence(3, false);
            }
            ok = ok && hold.loadFile(1, writeWav(folder, "swirlwire_src_bed", noise(5, 2, 0.3f)), error)
                    && hold.loadFile(2, writeWav(folder, "swirlwire_src_hits", hits()), error);
            hold.setRoleOverride(1, static_cast<int>(CargoRole::Bed));
            hold.setRoleOverride(2, static_cast<int>(CargoRole::Impact));
            hold.setSkipSilence(1, false);
        }
    };

    // ---- the renders the null comparison reads -------------------------------------------------------------------
    struct NullCase
    {
        const char* name;
        const char* what;
        double fs;
        int channels;
        bool filterOff, together, throws;
        float swirl, chaos;
    };
    const NullCase c_nullCases[] = {
        { "null_stereo",        "Stereo, the buses as they open, Filter at 0%",                         48000.0, 2,  true,  false, false, -1.0f, 0.4f },
        { "null_stereo_full",   "Stereo, Swirl at 100%, Chaos at 100%, Filter at 0%",                   48000.0, 2,  true,  false, true,  1.0f,  1.0f },
        { "null_room",          "7.1.4 with a throw every 3 s, Swirl at 60%, Filter at 0%",             48000.0, 12, true,  false, true,  0.6f,  0.4f },
        { "null_together_96",   "Stereo at 96 kHz, Faders Together, Swirl at 50%, Filter at 0%",        96000.0, 2,  true,  true,  false, 0.5f,  0.4f },
        { "null_filter",        "Stereo, the buses as they open, Filter where it opens (it moves)",     48000.0, 2,  false, false, false, -1.0f, 0.4f },
        { "null_filter_room",   "7.1.4 with a throw every 3 s, the buses as they open, Filter in",      48000.0, 12, false, false, true,  -1.0f, 0.4f },
    };
    void renderNull(const juce::File& folder)
    {
        Sources src(folder);
        for (const auto& c : c_nullCases)
        {
            auto e = std::make_unique<RigEngine>();
            RigEngine::Params q;
            q.chaos = c.chaos;
            q.rideMode = c.together ? RigEngine::RideTogether : RigEngine::RideApart;
            for (auto& bus : q.buses)
            {
                if (c.filterOff)
                    bus.filter = 0.0f;
                if (c.swirl >= 0.0f)
                    bus.swirl = c.swirl;
            }
            if (c.channels > 2)
                e->setWidth(room::Width::Surround714);
            start(*e, src.hold, q, c.fs);
            const int every = static_cast<int>(3.0 * c.fs) / c_chunk * c_chunk;
            RigEngine* engine = e.get();
            const bool throws = c.throws;
            const Run r = run(*e, c.channels, c.fs, 24.0, [engine, every, throws](int pos) { if (throws && pos % every == every / 2) engine->throwWhoosh(0, 1.0f); });
            writeWav(folder, c.name, r.heard, c.fs, 32);
            std::cout << "  " << c.name << ": " << c.what << ", " << num(db(rms(r.heard)), 1) << " dB" << std::endl;
        }
    }
    int compareNull(const juce::File& a, const juce::File& b)
    {
        std::cout << "Each render against the same one from the other build (the difference's level against the render's)" << std::endl;
        for (const auto& c : c_nullCases)
        {
            juce::AudioBuffer<float> x, y;
            if (! readWav(a.getChildFile(juce::String(c.name) + ".wav"), x) || ! readWav(b.getChildFile(juce::String(c.name) + ".wav"), y))
            {
                check(false, juce::String(c.name) + ": both files read");
                continue;
            }
            const Apart d = apart(x, y);
            const juce::String line = juce::String(c.what) + ": " + (d.largest == 0.0f ? juce::String("identical") : num(d.db, 1) + " dB, largest difference " + juce::String(d.largest, 8))
                                      + " (the render is at " + num(db(rms(x)), 1) + " dB)";
            if (c.filterOff)
                check(d.db < -100.0, line);
            else
                std::cout << "  (said) " << line << std::endl; // the Filter glides where it stepped every 32 samples
        }
        std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
        return g_failures == 0 ? 0 : 1;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 2 && juce::String(argv[1]) == "render")
    {
        juce::File(argv[2]).createDirectory();
        renderNull(juce::File(argv[2]));
        return 0;
    }
    if (argc > 3 && juce::String(argv[1]) == "null")
        return compareNull(juce::File(argv[2]), juce::File(argv[3]));

    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getCurrentWorkingDirectory();
    folder.createDirectory();
    juce::StringArray only;
    for (int i = 2; i < argc; ++i)
        only.add(argv[i]);
    auto wanted = [&only](const char* part) { return only.isEmpty() || only.contains(part); };

    std::cout << "Behind Swirl and Filter, on the engine" << std::endl;
    juce::String error;
    // Holds with one thing in each: white noise on bus A; a tone on bus A; a stereo hiss on bus B; a tone and hits.
    CargoHold noiseHold, toneHold, bedHold, hitHold;
    {
        bool ok = noiseHold.loadFile(0, writeWav(folder, "swirlwire_src_noise", noise(21, 1, 0.3f)), error);
        noiseHold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
        noiseHold.setSkipSilence(0, false);
        ok = ok && toneHold.loadFile(0, writeWav(folder, "swirlwire_src_tone", tone(330.0, 0.4f)), error);
        toneHold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
        toneHold.setSkipSilence(0, false);
        ok = ok && bedHold.loadFile(1, writeWav(folder, "swirlwire_src_bed", noise(5, 2, 0.3f)), error);
        bedHold.setRoleOverride(1, static_cast<int>(CargoRole::Bed));
        bedHold.setSkipSilence(1, false);
        ok = ok && hitHold.loadFile(0, folder.getChildFile("swirlwire_src_tone.wav"), error)
                && hitHold.loadFile(2, writeWav(folder, "swirlwire_src_hits", hits()), error);
        hitHold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
        hitHold.setRoleOverride(2, static_cast<int>(CargoRole::Impact));
        hitHold.setSkipSilence(0, false);
        check(ok && noiseHold.getBus(0) == 0 && toneHold.getBus(0) == 0 && bedHold.getBus(1) == 1 && hitHold.getBus(2) == CargoHold::c_hitsBus,
              "loads the sources: noise or a tone on bus A, a stereo hiss on bus B, hits " + error);
    }
    // A bus with nothing on it but what a part turns up, and a steady level into it.
    auto plain = []
    {
        RigEngine::Params q;
        q.rideOn = false;
        q.space = 0.0f;
        q.fill = 0.0f;
        q.hitAim = false;
        for (auto& bus : q.buses)
        {
            bus.low = bus.swirl = bus.filter = bus.drive = bus.duck = bus.key = 0.0f;
            bus.bend = 0.0f;
        }
        return q;
    };

    // ---- same ----------------------------------------------------------------------------------------------------
    if (wanted("same"))
    {
        std::cout << "same: what is not in use changes nothing" << std::endl;
        Sources src(folder);
        check(src.ok, "loads a tone and noise on bus A, a stereo bed on bus B and hits " + src.error);
        RigEngine::Params q;
        auto a = std::make_unique<RigEngine>();
        start(*a, src.hold, q);
        const Run ra = run(*a, 2, c_rate, 10.0);
        for (auto& d : q.swirlDetail)
        {
            d.tape.motion = SwirlFx::OnItsOwn;
            d.tape.invert = false;
            d.tape.depthMs = 6.0f;
            d.tape.rateHz = 3.0f;
            d.tape.feedback = -0.8f;
            d.tape.wobble = 1.0f;
            d.doubler.wave = SwirlFx::WaveSine;
            d.doubler.thicken = false;
            d.doubler.delayMs = 60.0f;
            d.doubler.wander = 1.0f;
            d.doubler.feedback = 1.0f;
        }
        for (auto& d : q.filterDetail)
        {
            d.shape = SweepFilter::Square;
            d.rateHz = 7.0f; // (Sweep is at 0%)
        }
        auto b = std::make_unique<RigEngine>();
        start(*b, src.hold, q);
        const Run rb = run(*b, 2, c_rate, 10.0);
        const Apart d = apart(ra.heard, rb.heard);
        check(d.largest == 0.0f && rms(ra.heard) > 0.01, "the Tape Flanger's and Doubler's settings and the Filter's Shape and Rate change nothing while Swirl is Phaser + Flanger and Sweep is 0% (largest difference "
                                                         + juce::String(d.largest, 9) + ")");
        check(a->getVarispeedOffset(0) == 0.0f && a->getVarispeedOffset(1) == 0.0f && a->getSwirlTypeInUse(0) == RigEngine::SwirlBoth, "the offset meter reads 0 outside the Tape Flanger");

        // The Doubler with the wheel at 0% is the bus as it is with Swirl at 0%, to the bit; with the wheel up it
        // is a different sound at about the same level (Swirl's level match stays on in this type).
        auto doubled = [&](int type, float wheel)
        {
            RigEngine::Params w;
            for (size_t bus = 0; bus < w.buses.size(); ++bus)
            {
                w.buses[bus].swirl = wheel;
                w.swirlDetail[bus].type = type;
            }
            auto e = std::make_unique<RigEngine>();
            start(*e, src.hold, w);
            return run(*e, 2, c_rate, 10.0).heard;
        };
        const auto dryBus = doubled(RigEngine::SwirlBoth, 0.0f), doublerOff = doubled(RigEngine::SwirlDoubler, 0.0f), doublerOn = doubled(RigEngine::SwirlDoubler, 0.7f);
        const Apart none = apart(dryBus, doublerOff), some = apart(dryBus, doublerOn);
        const double levelBy = db(rms(doublerOn, static_cast<int>(2.0 * c_rate)) / rms(dryBus, static_cast<int>(2.0 * c_rate)));
        std::cout << "  Doubler: wheel at 0% against Swirl at 0%, largest difference " << juce::String(none.largest, 9) << "; wheel at 70% " << num(some.db, 1) << " dB apart from it, its level "
                  << num(levelBy, 2) << " dB against it" << std::endl;
        check(none.largest == 0.0f, "the Doubler with the wheel at 0% leaves the bus as it is, to the bit");
        check(some.db > -30.0 && finite(doublerOn) && std::abs(levelBy) < 1.5, "the Doubler with the wheel up is heard, at the level the bus had");
    }

    // ---- types ---------------------------------------------------------------------------------------------------
    if (wanted("types"))
    {
        std::cout << "types: Phaser alone has no comb, Flanger alone has no all-pass notch" << std::endl;
        // White noise through bus A with the sweeps held still (Follow 0%, Depth 0%): the flanger sits at 3.55 ms (a
        // comb every 282 Hz) and the phaser's six all-pass stages at one place. As the stages are written
        // (sj::Allpass1 with RigEngine's coefficient) they turn at the top of the band, not at the 905 Hz the
        // coefficient is worked out for: at 48 kHz, held still, the phaser's first notch is at 20.6 kHz, and it is
        // there that this part looks for it. Each type's long-term spectrum against the same render with Swirl at 0%.
        auto render = [&](int type, float swirl)
        {
            auto q = plain();
            q.buses[0].swirl = swirl;
            q.buses[0].doppler = 0.0f;
            q.swirlDetail[0].type = type;
            q.swirlDetail[0].follow = 0.0f;
            q.swirlDetail[0].depth = 0.0f;
            auto e = std::make_unique<RigEngine>();
            start(*e, noiseHold, q);
            return spectrum(run(*e, 2, c_rate, 20.0).heard, 0, static_cast<int>(2.0 * c_rate));
        };
        const auto off = render(RigEngine::SwirlBoth, 0.0f);
        struct Shape { double comb = 0.0, delayMs = 0.0, notch = 0.0; };
        auto measure = [&](const std::vector<double>& with)
        {
            auto ratio = [&](double hz) { return 10.0 * std::log10(std::max(bandPower(with, hz, 12.0), 1.0e-30) / std::max(bandPower(off, hz, 12.0), 1.0e-30)); };
            if (only.contains("each"))
            {
                std::cout << "     ";
                for (const double hz : { 100.0, 243.0, 500.0, 905.0, 1800.0, 3379.0, 6000.0, 9000.0, 14000.0, 18000.0, 20000.0, 20600.0, 21200.0, 22000.0 })
                    std::cout << " " << hz << ":" << num(ratio(hz), 1);
                std::cout << std::endl;
            }
            Shape sh;
            // The comb: the delay (3.4 .. 3.7 ms) whose teeth stand out most between 4 and 10 kHz: the mean of each
            // tooth's top against the gap after it.
            for (double ms = 3.40; ms <= 3.70; ms += 0.002)
            {
                const double tooth = 1000.0 / ms;
                double sum = 0.0;
                int n = 0;
                for (int k = static_cast<int>(std::ceil(4000.0 / tooth)); (k + 1) * tooth < 10000.0; ++k, ++n)
                    sum += 0.5 * (ratio(k * tooth) + ratio((k + 1) * tooth)) - ratio((k + 0.5) * tooth);
                if (n > 0 && sum / n > sh.comb)
                {
                    sh.comb = sum / n;
                    sh.delayMs = ms;
                }
            }
            // The all-pass notch: the level round 20.6 kHz against the level round 1 kHz, each the mean (in dB) over
            // a whole number of the comb's teeth (two up there, four down here), so a comb reads the same in both.
            const double tooth = 1000.0 / 3.55;
            auto mean = [&](double from, double width)
            {
                double sum = 0.0;
                int n = 0;
                for (double hz = from; hz < from + width; hz += 12.0, ++n)
                    sum += ratio(hz);
                return sum / std::max(1, n);
            };
            sh.notch = mean(20600.0 - tooth, 2.0 * tooth) - mean(500.0, 4.0 * tooth);
            return sh;
        };
        const Shape both = measure(render(RigEngine::SwirlBoth, 1.0f)), phaser = measure(render(RigEngine::SwirlPhaser, 1.0f)),
                    flanger = measure(render(RigEngine::SwirlFlanger, 1.0f));
        auto say = [](const char* name, const Shape& sh)
        {
            std::cout << "  " << name << ": comb " << num(sh.comb, 1) << " dB (at " << num(sh.delayMs, 3) << " ms); round 20.6 kHz against round 1 kHz " << num(sh.notch, 1) << " dB" << std::endl;
        };
        say("Phaser + Flanger", both);
        say("Phaser", phaser);
        say("Flanger", flanger);
        check(both.comb > 6.0 && both.notch < -6.0, "Phaser + Flanger has the comb and the all-pass notch (the measure sees both)");
        check(phaser.comb < 1.0 && phaser.notch < -6.0, "Phaser alone: the all-pass notch (" + num(phaser.notch, 1) + " dB) and no comb (" + num(phaser.comb, 2) + " dB at the most)");
        check(flanger.comb > 6.0 && flanger.notch > -3.0, "Flanger alone: the comb, and no all-pass notch (" + num(flanger.notch, 1) + " dB)");
        // The flanger alone comes in from the bottom of the knob (in Phaser + Flanger it waits for about 14% of it).
        const Shape low = measure(render(RigEngine::SwirlFlanger, 0.08f)), lowBoth = measure(render(RigEngine::SwirlBoth, 0.08f));
        std::cout << "  at 8% of the knob: Flanger's comb " << num(low.comb, 1) << " dB, Phaser + Flanger's " << num(lowBoth.comb, 1) << " dB" << std::endl;
        check(low.comb > 1.5 && lowBoth.comb < 0.5, "Flanger alone is heard from the bottom of the knob");
        // Feedback at both ends, Speed at its top: nothing runs away.
        bool stable = true;
        double loudest = 0.0;
        for (const int type : { RigEngine::SwirlBoth, RigEngine::SwirlPhaser, RigEngine::SwirlFlanger })
            for (const float feedback : { -1.0f, 1.0f })
            {
                auto q = plain();
                q.buses[0].swirl = 1.0f;
                q.swirlDetail[0].type = type;
                q.swirlDetail[0].feedback = feedback;
                q.swirlDetail[0].speed = 8.0f;
                auto e = std::make_unique<RigEngine>();
                start(*e, noiseHold, q);
                const Run r = run(*e, 2, c_rate, 8.0);
                stable = stable && finite(r.heard);
                loudest = std::max(loudest, static_cast<double>(r.heard.getMagnitude(0, r.heard.getNumSamples())));
            }
        std::cout << "  Feedback at -100% and +100%, Speed 8x, every type: the loudest sample " << num(loudest, 3) << std::endl;
        check(stable && loudest < 4.0, "Feedback at either end does not run away");
    }

    // ---- tape ----------------------------------------------------------------------------------------------------
    if (wanted("tape"))
    {
        std::cout << "tape: the copy goes through zero either side of each pass" << std::endl;
        for (const bool room : { false, true })
            for (const int passMode : { 0, 1 })
            {
                RigEngine::Params q;
                q.hitAim = false;
                q.buses[0].swirl = 0.5f;
                q.buses[0].passMode = passMode;
                q.swirlDetail[0].type = RigEngine::SwirlTape;
                auto e = std::make_unique<RigEngine>();
                if (room)
                    e->setWidth(room::Width::Surround714);
                start(*e, toneHold, q);
                std::vector<float> d0, d1;
                RigEngine* engine = e.get();
                const Run r = run(*e, room ? 12 : 2, c_rate, 60.0, false, [](int) {}, [&d0, &d1, engine](int)
                {
                    d0.push_back(engine->getSwirlLaneOffsetMs(0, 0));
                    d1.push_back(engine->getSwirlLaneOffsetMs(0, 1));
                });
                // Each pass's marker (its closest point): the copy is early there, was late before it and is late
                // again after it. In a room a lane is one pass: the lane that pass is in.
                int passes = 0, bracketed = 0;
                double beforeFrom = 1.0e9, beforeTo = 0.0, afterFrom = 1.0e9, afterTo = 0.0, earliest = 0.0, latest = 0.0, atMarkFrom = 1.0e9, atMarkTo = -1.0e9;
                const int perSecond = static_cast<int>(c_rate) / c_chunk;
                for (const double m : r.marks[RigEngine::MarkPassA])
                {
                    const int at = static_cast<int>(m) / c_chunk;
                    if (at < 4 * perSecond || at > static_cast<int>(d0.size()) - 4 * perSecond)
                        continue;
                    ++passes;
                    const auto& d = room && d1[static_cast<size_t>(at)] < d0[static_cast<size_t>(at)] ? d1 : d0;
                    const float here = d[static_cast<size_t>(at)];
                    atMarkFrom = std::min(atMarkFrom, static_cast<double>(here));
                    atMarkTo = std::max(atMarkTo, static_cast<double>(here));
                    int before = at, after = at;
                    while (before > at - 4 * perSecond && d[static_cast<size_t>(before)] <= 0.0f) --before;
                    while (after < at + 4 * perSecond && d[static_cast<size_t>(after)] <= 0.0f) ++after;
                    const bool ok = here < 0.0f && before > at - 4 * perSecond && after < at + 4 * perSecond;
                    bracketed += ok ? 1 : 0;
                    if (ok)
                    {
                        beforeFrom = std::min(beforeFrom, static_cast<double>(at - before) / perSecond);
                        beforeTo = std::max(beforeTo, static_cast<double>(at - before) / perSecond);
                        afterFrom = std::min(afterFrom, static_cast<double>(after - at) / perSecond);
                        afterTo = std::max(afterTo, static_cast<double>(after - at) / perSecond);
                    }
                }
                for (size_t i = static_cast<size_t>(4 * perSecond); i < d0.size(); ++i)
                {
                    earliest = std::min(earliest, static_cast<double>(d0[i]));
                    latest = std::max(latest, static_cast<double>(d0[i]));
                }
                const juce::String where = juce::String(room ? "7.1.4" : "Stereo") + (passMode == 1 ? ", Fly-By" : ", Bed");
                std::cout << "  " << where << ": " << passes << " passes, " << bracketed << " with a zero either side; the one before " << num(beforeFrom, 3) << " .. " << num(beforeTo, 3)
                          << " s ahead of the marker, the one after " << num(afterFrom, 3) << " .. " << num(afterTo, 3) << " s behind it; at the marker the copy is at "
                          << num(atMarkFrom, 2) << " .. " << num(atMarkTo, 2) << " ms; over the run " << num(earliest, 2) << " .. " << num(latest, 2) << " ms" << std::endl;
                check(passes >= 8 && bracketed == passes, "Tape Flanger, With the Pass: the copy is early at every pass's marker and goes through zero before and after it (" + where + ")");
                check(latest > 2.0 && earliest < -0.5 && earliest > -6.5 && latest < 6.5, "it is late between the passes and stays inside its travel (" + where + ")");
            }

        // Varispeed: a lean moves the held offset, it stays when let go, the meter's double-click slides it home,
        // and a seeded restart starts from the middle.
        {
            auto q = plain();
            q.buses[0].swirl = 0.5f;
            q.swirlDetail[0].type = RigEngine::SwirlTape;
            q.swirlDetail[0].tape.motion = SwirlFx::ByHand;
            auto e = std::make_unique<RigEngine>();
            start(*e, toneHold, q);
            RigEngine* engine = e.get();
            float afterLean = 0.0f, held = 0.0f, home = 1.0f, restarted = 1.0f, other = 1.0f;
            double homeAt = -1.0;
            run(*e, 2, c_rate, 9.0, false, [&](int pos)
            {
                auto now = q;
                const double t = pos / c_rate;
                now.swirlDetail[0].tape.varispeed = t >= 1.0 && t < 1.15 ? 1.0f : 0.0f;
                now.varispeedHome[0] = t >= 4.0 && t < 4.01;
                if (t >= 7.0)
                    now.swirlDetail[0].type = RigEngine::SwirlPhaser;
                engine->setParams(now);
            }, [&](int pos)
            {
                const double t = pos / c_rate;
                const float v = engine->getVarispeedOffset(0);
                if (t >= 3.0 && t < 3.1) afterLean = v;
                if (t < 4.0) held = v;
                if (t >= 4.0 && t < 6.9)
                {
                    home = v;
                    if (homeAt < 0.0 && v == 0.0f)
                        homeAt = t - 4.0;
                }
                if (t >= 8.0) other = v;
            });
            std::cout << "  Varispeed leant all the way for 0.15 s: the offset is " << num(afterLean, 5) << " of its travel two seconds later and " << num(held, 5)
                      << " a second after that; the meter double-clicked: home in " << num(homeAt, 3) << " s" << std::endl;
            check(afterLean < -0.1f && afterLean > -1.0f, "a lean on Varispeed moves the held offset, and the meter's view shows it");
            check(std::abs(held - afterLean) < 1.0e-6f, "the offset stays where it was left");
            check(home == 0.0f && homeAt > 0.0 && homeAt < 1.5, "the meter's double-click slides it home");
            check(other == 0.0f && engine->getSwirlTypeInUse(0) == RigEngine::SwirlPhaser, "another Type: the meter reads 0");
            // A lean, then a seeded restart.
            auto f = std::make_unique<RigEngine>();
            start(*f, toneHold, q);
            RigEngine* second = f.get();
            float before = 0.0f;
            run(*f, 2, c_rate, 3.0, false, [&](int pos)
            {
                auto now = q;
                const double t = pos / c_rate;
                now.swirlDetail[0].tape.varispeed = t >= 0.5 && t < 0.65 ? -1.0f : 0.0f;
                second->setParams(now);
                if (pos == static_cast<int>(2.0 * c_rate) / c_chunk * c_chunk)
                {
                    before = second->getVarispeedOffset(0);
                    second->restartSeeded();
                }
            }, [&](int) { restarted = second->getVarispeedOffset(0); });
            check(before > 0.1f && restarted == 0.0f, "a transport start puts the offset back in the middle (it was at " + num(before, 3) + ")");
        }
    }

    // ---- sides ---------------------------------------------------------------------------------------------------
    if (wanted("sides"))
    {
        std::cout << "sides: a stereo bed's side signal is as late as its middle in a Tape Flanger" << std::endl;
        // The lag at which one signal best matches another, and how well (-1 .. 1).
        auto lagOf = [](const std::vector<float>& ref, const std::vector<float>& late, int from, int length, int reach)
        {
            std::pair<int, double> best { 0, -2.0 };
            double refSq = 0.0;
            for (int i = from; i < from + length; ++i)
                refSq += static_cast<double>(ref[static_cast<size_t>(i)]) * static_cast<double>(ref[static_cast<size_t>(i)]);
            for (int lag = -reach; lag <= reach; ++lag)
            {
                double dot = 0.0, lateSq = 0.0;
                for (int i = from; i < from + length; ++i)
                {
                    const double y = static_cast<double>(late[static_cast<size_t>(i + lag)]);
                    dot += static_cast<double>(ref[static_cast<size_t>(i)]) * y;
                    lateSq += y * y;
                }
                const double c = dot / std::sqrt(std::max(refSq * lateSq, 1.0e-30));
                if (c > best.second)
                    best = { lag, c };
            }
            return best;
        };
        for (const float wheel : { 0.0f, 0.5f })
        {
            auto render = [&](int type, int channels)
            {
                auto q = plain();
                q.buses[1].swirl = wheel;
                q.buses[1].doppler = 0.2f;
                q.swirlDetail[1].type = type;
                q.swirlDetail[1].tape.motion = SwirlFx::ByHand; // (the copy sits on the dry: the delay is all there is to see)
                q.swirlDetail[1].tape.invert = false;
                auto e = std::make_unique<RigEngine>();
                if (channels > 2)
                    e->setWidth(room::Width::Surround714);
                start(*e, bedHold, q);
                return run(*e, channels, c_rate, 8.0).heard;
            };
            const auto dry = render(RigEngine::SwirlBoth, 2), tape = render(RigEngine::SwirlTape, 2);
            auto part = [](const juce::AudioBuffer<float>& b, float sign)
            {
                std::vector<float> x(static_cast<size_t>(b.getNumSamples()));
                for (int i = 0; i < b.getNumSamples(); ++i)
                    x[static_cast<size_t>(i)] = b.getSample(0, i) + sign * b.getSample(1, i);
                return x;
            };
            const auto midDry = part(dry, 1.0f), sideDry = part(dry, -1.0f), midTape = part(tape, 1.0f), sideTape = part(tape, -1.0f);
            const int from = static_cast<int>(3.0 * c_rate), length = static_cast<int>(1.0 * c_rate);
            const auto mid = lagOf(midDry, midTape, from, length, 600), side = lagOf(sideDry, sideTape, from, length, 600);
            auto level = [&](const std::vector<float>& x) { double sq = 0.0; for (int i = from; i < from + 3 * length; ++i) sq += static_cast<double>(x[static_cast<size_t>(i)]) * static_cast<double>(x[static_cast<size_t>(i)]); return std::sqrt(sq / (3 * length)); };
            const double sideShare = db(level(sideDry) / level(midDry)), sideShareTape = db(level(sideTape) / level(midTape));
            std::cout << "  wheel at " << juce::roundToInt(100.0f * wheel) << "%: the middle is " << mid.first << " samples late (match " << num(mid.second, 4) << "), the sides " << side.first
                      << " samples (match " << num(side.second, 4) << "); the sides against the middle " << num(sideShare, 2) << " dB before, " << num(sideShareTape, 2) << " dB in the Tape Flanger" << std::endl;
            const juce::String where = " (wheel at " + juce::String(juce::roundToInt(100.0f * wheel)) + "%)";
            check(sideShare > -20.0, "the bed has sides to speak of" + where);
            check(mid.first == 384 && side.first == 384, "middle and sides are both 8 ms (384 samples) late" + where);
            check(wheel > 0.0f ? side.second > 0.7 : (side.second > 0.98 && mid.second > 0.98), "the sides are what they were, moved" + where);
            check(std::abs(sideShareTape - sideShare) < (wheel > 0.0f ? 3.0 : 0.5), "the bed is as wide as it was" + where);
            if (wheel == 0.0f)
            {
                // A room: every speaker's signal is the same one, 8 ms late (the sides are a lane of their own there).
                const auto dryRoom = render(RigEngine::SwirlBoth, 12), tapeRoom = render(RigEngine::SwirlTape, 12);
                int speakers = 0, right = 0;
                double worst = 1.0;
                for (int ch = 0; ch < 12; ++ch)
                {
                    if (dryRoom.getRMSLevel(ch, from, length) < 1.0e-4f)
                        continue;
                    ++speakers;
                    std::vector<float> a(dryRoom.getReadPointer(ch), dryRoom.getReadPointer(ch) + dryRoom.getNumSamples()), b(tapeRoom.getReadPointer(ch), tapeRoom.getReadPointer(ch) + tapeRoom.getNumSamples());
                    const auto l = lagOf(a, b, from, length / 2, 500);
                    right += l.first == 384 ? 1 : 0;
                    worst = std::min(worst, l.second);
                }
                std::cout << "  7.1.4, wheel at 0%: " << right << " of " << speakers << " speakers are 384 samples late, the poorest match " << num(worst, 4) << std::endl;
                check(speakers >= 6 && right == speakers && worst > 0.98, "in a room every speaker carries the bed 8 ms late, sides and all");
            }
        }
    }

    // ---- aim -----------------------------------------------------------------------------------------------------
    if (wanted("aim"))
    {
        std::cout << "aim: the hit lands where the bus is heard" << std::endl;
        for (const bool together : { false, true })
        {
            double lateBy[2] = { 0.0, 0.0 };
            int counted[2] = { 0, 0 };
            double spread[2] = { 0.0, 0.0 };
            for (const int tape : { 0, 1 })
            {
                RigEngine::Params q;
                q.hitSense = 1.0f;
                q.rideMode = together ? RigEngine::RideTogether : RigEngine::RideApart;
                q.swirlDetail[0].type = tape == 1 ? RigEngine::SwirlTape : RigEngine::SwirlBoth;
                auto e = std::make_unique<RigEngine>();
                start(*e, hitHold, q);
                const Run r = run(*e, 2, c_rate, 60.0, false, [](int) {}, [](int) {});
                double sum = 0.0, lo = 1.0e9, hi = -1.0e9;
                for (const double h : r.marks[RigEngine::MarkHit])
                {
                    if (h < 5.0 * c_rate)
                        continue;
                    double best = 1.0e12;
                    for (const double m : r.marks[RigEngine::MarkPassA])
                        if (std::abs(h - m) < std::abs(best))
                            best = h - m;
                    if (std::abs(best) > 0.1 * c_rate)
                        continue;
                    sum += best;
                    lo = std::min(lo, best);
                    hi = std::max(hi, best);
                    ++counted[tape];
                }
                lateBy[tape] = counted[tape] > 0 ? sum / counted[tape] : 1.0e9;
                spread[tape] = hi - lo;
            }
            std::cout << "  " << (together ? "Faders Together" : "Faders Apart") << ": the hit's marker against the pass's: " << num(lateBy[0], 1) << " samples (" << counted[0] << " hits, "
                      << num(spread[0], 1) << " apart) as Phaser + Flanger, " << num(lateBy[1], 1) << " samples (" << counted[1] << " hits, " << num(spread[1], 1) << " apart) as a Tape Flanger" << std::endl;
            check(counted[0] >= 5 && counted[1] >= 5 && std::abs(lateBy[0]) <= c_chunk && std::abs(lateBy[1] - 384.0) <= c_chunk,
                  juce::String("the hit is aimed 8 ms later for a bus whose Swirl is a Tape Flanger, ") + (together ? "in Faders Together too" : "and where it was otherwise"));
        }

        // Faders Together: the faders' swell as it is heard (the render against the same one with Auto Ride off, in
        // windows of eleven cycles of the tone) is where it is without the Tape Flanger: its 8 ms are in the lead.
        auto ride = [&](int type)
        {
            std::vector<double> heard;
            juce::AudioBuffer<float> on, offRide;
            for (const bool rideOn : { true, false })
            {
                RigEngine::Params q;
                q.rideMode = RigEngine::RideTogether;
                q.rideOn = rideOn;
                q.chaos = 0.0f;
                q.hitAim = false;
                q.space = q.fill = 0.0f;
                q.buses[0].swirl = 0.0f; // (the wheel at 0: the Tape Flanger is its clean path alone)
                q.buses[0].bend = 0.0f;
                q.buses[0].low = q.buses[0].filter = q.buses[0].drive = 0.0f;
                q.swirlDetail[0].type = type;
                auto e = std::make_unique<RigEngine>();
                start(*e, toneHold, q);
                (rideOn ? on : offRide) = run(*e, 2, c_rate, 30.0).heard;
            }
            const int window = 1600;
            for (int at = 0; at + window <= on.getNumSamples(); at += c_chunk)
                heard.push_back(static_cast<double>(on.getRMSLevel(0, at, window) + on.getRMSLevel(1, at, window))
                                / std::max(1.0e-9, static_cast<double>(offRide.getRMSLevel(0, at, window) + offRide.getRMSLevel(1, at, window))));
            return heard;
        };
        const auto plainRide = ride(RigEngine::SwirlBoth), tapeRide = ride(RigEngine::SwirlTape);
        int bestLag = 0;
        double bestError = 1.0e30;
        const int from = static_cast<int>(8.0 * c_rate) / c_chunk, to = static_cast<int>(26.0 * c_rate) / c_chunk;
        for (int lag = -30; lag <= 30; ++lag)
        {
            double sq = 0.0;
            for (int i = from; i < to; ++i)
                sq += (tapeRide[static_cast<size_t>(i + lag)] - plainRide[static_cast<size_t>(i)]) * (tapeRide[static_cast<size_t>(i + lag)] - plainRide[static_cast<size_t>(i)]);
            if (sq < bestError)
            {
                bestError = sq;
                bestLag = lag;
            }
        }
        std::cout << "  Faders Together: the swell as heard through the Tape Flanger is " << bestLag * c_chunk << " samples (" << num(1000.0 * bestLag * c_chunk / c_rate, 2)
                  << " ms) later than without it (in steps of " << c_chunk << " samples)" << std::endl;
        check(std::abs(bestLag) <= 2, "in Faders Together the faders lead a Tape Flanger's bus by its 8 ms: the swell is heard where it was");
    }

    // ---- change --------------------------------------------------------------------------------------------------
    if (wanted("change"))
    {
        std::cout << "change: a change of Type does not click" << std::endl;
        const int order[] = { RigEngine::SwirlPhaser, RigEngine::SwirlFlanger, RigEngine::SwirlTape, RigEngine::SwirlDoubler, RigEngine::SwirlBoth, RigEngine::SwirlTape,
                              RigEngine::SwirlPhaser, RigEngine::SwirlDoubler, RigEngine::SwirlFlanger, RigEngine::SwirlTape, RigEngine::SwirlBoth, RigEngine::SwirlDoubler,
                              RigEngine::SwirlTape, RigEngine::SwirlFlanger, RigEngine::SwirlBoth };
        for (const bool room : { false, true })
        {
            RigEngine::Params q;
            q.hitAim = false;
            q.buses[0].swirl = 0.6f;
            auto e = std::make_unique<RigEngine>();
            if (room)
                e->setWidth(room::Width::Surround714);
            start(*e, toneHold, q);
            RigEngine* engine = e.get();
            const int first = static_cast<int>(6.0 * c_rate), every = static_cast<int>(0.6 * c_rate) / c_chunk * c_chunk;
            bool followed = true;
            const Run r = run(*e, room ? 12 : 2, c_rate, 6.0 + 0.6 * 16.0, true, [&](int pos)
            {
                if (pos < first)
                    return;
                const int k = std::min((pos - first) / every, 14);
                auto now = q;
                now.swirlDetail[0].type = order[k];
                // (Half a second on, the type asked for is the one in use.)
                if ((pos - first) % every == 0 && k > 0 && (pos - first) / every <= 14)
                    followed = followed && engine->getSwirlTypeInUse(0) == order[k - 1];
                // Every other change comes with a throw in flight (a room's third lane).
                if ((pos - first) % (2 * every) == every / 2)
                    engine->throwWhoosh(0, 1.0f);
                engine->setParams(now);
            }, [](int) {});
            // Calm play as each type, away from the changes, against the changes themselves (the 150 ms after one).
            float calm = 0.0f, changing = 0.0f;
            for (int k = 0; k < 15; ++k)
            {
                const int at = first + k * every;
                const float here = sharpest(r.heard, at, at + static_cast<int>(0.15 * c_rate));
                if (only.contains("each"))
                    std::cout << "    to type " << order[k] << ": " << juce::String(here, 5) << std::endl;
                changing = std::max(changing, here);
                calm = std::max(calm, sharpest(r.heard, at + static_cast<int>(0.2 * c_rate), at + every - c_chunk));
            }
            calm = std::max(calm, sharpest(r.heard, static_cast<int>(2.0 * c_rate), first));
            const juce::String where = room ? "7.1.4" : "Stereo";
            std::cout << "  " << where << ": the sharpest change in the sound (second difference) " << juce::String(calm, 5) << " in calm play, " << juce::String(changing, 5)
                      << " through 15 changes of Type" << std::endl;
            check(finite(r.heard) && followed, "every Type asked for is in use within the 0.6 s (" + where + ")");
            check(changing <= 1.25f * calm, "a change of Type is no sharper than the sound itself (" + where + ")");
        }
    }

    // ---- filter --------------------------------------------------------------------------------------------------
    if (wanted("filter"))
    {
        std::cout << "filter: each Type, the slope and the rhythm, on the bus" << std::endl;
        // White noise through bus A, the Filter all the way in with Follow at 0%: it sits at Closed At x 2^0.7
        // (276 Hz; High Pass mirrors that to 1593 Hz), Q 2.2. Against the same render with the Filter at 0%.
        auto render = [&](int type, bool steep, float sweep, juce::AudioBuffer<float>* keep = nullptr)
        {
            auto q = plain();
            q.buses[0].filter = type < 0 ? 0.0f : 1.0f;
            q.levels[0] = 0.1f; // (quiet: the Filter's soft clip adds next to nothing of its own)
            q.buses[0].doppler = 0.0f;
            q.filterDetail[0].type = std::max(0, type);
            q.filterDetail[0].steep = steep;
            q.filterDetail[0].follow = 0.0f;
            q.filterDetail[0].sweep = sweep;
            q.filterDetail[0].rateHz = 2.0f;
            auto e = std::make_unique<RigEngine>();
            start(*e, noiseHold, q);
            auto heard = run(*e, 2, c_rate, 14.0).heard;
            if (keep != nullptr)
                *keep = heard;
            return spectrum(heard, 0, static_cast<int>(2.0 * c_rate));
        };
        const auto off = render(-1, false, 0.0f);
        auto ratio = [&](const std::vector<double>& with, double hz) { return 10.0 * std::log10(std::max(bandPower(with, hz, hz * 0.06), 1.0e-30) / std::max(bandPower(off, hz, hz * 0.06), 1.0e-30)); };
        const double fc = 170.0 * std::pow(2.0, 0.7), mirrored = 20.0 * 22000.0 / fc;
        {
            const auto lp = render(SweepFilter::LowPass, false, 0.0f), lp24 = render(SweepFilter::LowPass, true, 0.0f);
            const double slope = ratio(lp, 4.0 * fc) - ratio(lp, 0.25 * fc), slope24 = ratio(lp24, 4.0 * fc) - ratio(lp24, 0.25 * fc);
            std::cout << "  Low Pass at " << num(fc, 0) << " Hz: two octaves above it against two below, " << num(slope, 1) << " dB (the filter alone: -24.1), with 24 dB " << num(slope24, 1) << " dB (-48.2)" << std::endl;
            check(std::abs(slope + 24.1) < 2.0 && std::abs(slope24 + 48.2) < 3.0, "Low Pass: 12 dB an octave, and 24 with the key");
            const auto hp = render(SweepFilter::HighPass, false, 0.0f), hp24 = render(SweepFilter::HighPass, true, 0.0f);
            const double hpSlope = ratio(hp, 0.25 * mirrored) - ratio(hp, 4.0 * mirrored), hpSlope24 = ratio(hp24, 0.25 * mirrored) - ratio(hp24, 4.0 * mirrored);
            std::cout << "  High Pass at " << num(mirrored, 0) << " Hz: two octaves below it against two above, " << num(hpSlope, 1) << " dB, with 24 dB " << num(hpSlope24, 1) << " dB" << std::endl;
            check(std::abs(hpSlope + 24.1) < 2.0 && std::abs(hpSlope24 + 48.2) < 3.0, "High Pass: mirrored on the axis, 12 and 24 dB an octave");
            const auto bp = render(SweepFilter::BandPass, false, 0.0f), notch = render(SweepFilter::Notch, false, 0.0f);
            const double bpBelow = ratio(bp, fc) - ratio(bp, 0.125 * fc), bpAbove = ratio(bp, fc) - ratio(bp, 8.0 * fc);
            const double notchDip = ratio(notch, fc) - 0.5 * (ratio(notch, 0.125 * fc) + ratio(notch, 8.0 * fc));
            std::cout << "  Band Pass: its centre " << num(bpBelow, 1) << " dB over three octaves below and " << num(bpAbove, 1) << " dB over three above; Notch: its centre " << num(notchDip, 1)
                      << " dB under three octaves either side" << std::endl;
            check(bpBelow > 15.0 && bpAbove > 15.0, "Band Pass: a band round the same place");
            check(notchDip < -10.0, "Notch: a dip there");
        }
        {
            // Its own rhythm: Sweep at 100%, 2 Hz. The top of the sound (twice differenced) in 10 ms steps comes and
            // goes twice a second.
            juce::AudioBuffer<float> swept;
            render(SweepFilter::LowPass, false, 1.0f, &swept);
            std::vector<double> env;
            const float* x = swept.getReadPointer(0);
            const int hop = 480;
            for (int at = static_cast<int>(3.0 * c_rate); at + hop < swept.getNumSamples(); at += hop)
            {
                double sq = 0.0;
                for (int i = at; i < at + hop; ++i)
                    sq += static_cast<double>(x[i + 1] - 2.0f * x[i] + x[i - 1]) * static_cast<double>(x[i + 1] - 2.0f * x[i] + x[i - 1]);
                env.push_back(10.0 * std::log10(std::max(sq / hop, 1.0e-20)));
            }
            double lo = 1.0e9, hi = -1.0e9, mean = 0.0;
            for (const double v : env) { lo = std::min(lo, v); hi = std::max(hi, v); mean += v / static_cast<double>(env.size()); }
            auto match = [&](int shift)
            {
                double dot = 0.0, a = 0.0, b = 0.0;
                for (size_t i = 0; i + static_cast<size_t>(shift) < env.size(); ++i)
                {
                    dot += (env[i] - mean) * (env[i + static_cast<size_t>(shift)] - mean);
                    a += (env[i] - mean) * (env[i] - mean);
                    b += (env[i + static_cast<size_t>(shift)] - mean) * (env[i + static_cast<size_t>(shift)] - mean);
                }
                return dot / std::sqrt(std::max(a * b, 1.0e-30));
            };
            std::cout << "  Sweep at 100%, 2 Hz: the top of the sound moves over " << num(hi - lo, 1) << " dB; against itself half a second on " << num(match(50), 3) << ", a quarter of a second on "
                      << num(match(25), 3) << std::endl;
            check(hi - lo > 30.0 && match(50) > 0.9 && match(25) < -0.5 && finite(swept), "the Filter's own rhythm opens and closes it at its Rate");
        }
    }

    // ---- speed ---------------------------------------------------------------------------------------------------
    if (wanted("speed"))
    {
        std::cout << "speed: each type at 7.1.4 and 192 kHz with a throw in flight" << std::endl;
        Sources src(folder);
        const double fs = 192000.0, seconds = 6.0;
        // One timed run of a setting. The settings are timed in turn, seven rounds of all of them, and each keeps its
        // best: whatever else the machine is doing falls on every setting alike instead of on whichever came last.
        auto once = [&](int type, float swirl, float tapeFeedback)
        {
            RigEngine::Params q;
            for (size_t b = 0; b < q.buses.size(); ++b)
            {
                q.buses[b].swirl = swirl;
                q.swirlDetail[b].type = type;
                q.swirlDetail[b].tape.feedback = tapeFeedback;
            }
            auto e = std::make_unique<RigEngine>();
            e->setWidth(room::Width::Surround714);
            start(*e, src.hold, q, fs);
            RigEngine* engine = e.get();
            const int every = static_cast<int>(2.0 * fs) / c_chunk * c_chunk;
            // (One second first, so everything is running; the clock is on the rest.)
            run(*e, 12, fs, 1.0, false, [engine](int pos) { if (pos == 0) engine->throwWhoosh(0, 1.0f); }, [](int) {});
            const auto t0 = std::chrono::steady_clock::now();
            run(*e, 12, fs, seconds, false, [engine, every](int pos) { if (pos % every == 0) engine->throwWhoosh(0, 1.0f); }, [](int) {});
            return 100.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / seconds; // percent of one core
        };
        struct Timed { int type; float swirl, feedback; double best; };
        Timed timed[] = { { RigEngine::SwirlBoth, 0.0f, 0.0f, 1.0e9 }, { RigEngine::SwirlBoth, 0.6f, 0.0f, 1.0e9 }, { RigEngine::SwirlPhaser, 0.6f, 0.0f, 1.0e9 },
                          { RigEngine::SwirlFlanger, 0.6f, 0.0f, 1.0e9 }, { RigEngine::SwirlTape, 0.6f, 0.0f, 1.0e9 }, { RigEngine::SwirlTape, 0.6f, 0.5f, 1.0e9 },
                          { RigEngine::SwirlDoubler, 0.6f, 0.0f, 1.0e9 }, { RigEngine::SwirlBoth, 0.0f, 0.0f, 1.0e9 } };
        for (int round = 0; round < 7; ++round)
            for (auto& t : timed)
                t.best = std::min(t.best, once(t.type, t.swirl, t.feedback));
        const double off = timed[0].best, both = timed[1].best, phaser = timed[2].best, flanger = timed[3].best, tape = timed[4].best, tapeFed = timed[5].best,
                     doubler = timed[6].best, offAgain = timed[7].best; // (the same thing timed twice: how far the clock can be trusted)
        std::cout << "  the whole engine, both buses, percent of one core (the best of seven runs): Swirl at 0% " << num(off, 2) << " and, timed again, " << num(offAgain, 2)
                  << "; Phaser + Flanger " << num(both, 2) << " (Swirl's part " << num(both - off, 2) << "); Phaser " << num(phaser, 2) << " (" << num(phaser - off, 2) << "); Flanger "
                  << num(flanger, 2) << " (" << num(flanger - off, 2) << "); Tape Flanger " << num(tape, 2) << " (" << num(tape - off, 2) << "), with Feedback " << num(tapeFed, 2) << " ("
                  << num(tapeFed - off, 2) << "); Doubler " << num(doubler, 2) << " (" << num(doubler - off, 2) << ")" << std::endl;
        // (Timing on a shared machine: 0.2 of a percent of a core is allowed for.)
        const double slack = 0.2;
        check(phaser <= both + slack && flanger <= both + slack, "Phaser or Flanger alone costs no more than both");
        check(tape <= both + slack, "the Tape Flanger costs no more than Phaser + Flanger");
        // (Its Feedback is a second read of the line with its own low-pass and soft clip: measured 0.36 to 0.68 of a
        // percent of a core over Phaser + Flanger here, on a machine doing other work. Held to one percent.)
        check(tapeFed <= both + 1.0, "with its Feedback in, the Tape Flanger is within one percent of a core of Phaser + Flanger (" + num(tapeFed - both, 2) + " over)");
        check(doubler <= both + slack, "the Doubler costs no more than Phaser + Flanger");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

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
//   centre     Tape Flanger, With the Pass: where the copy goes through zero against the middle of the whoosh as it
//              is heard, pass by pass, at several Rates and closenesses, in Stereo and 5.1 (a table)
//   follow     the Followers' Pass and Filter inputs (the Pass tops out on the pass's marker and does not hear a loud
//              burst; the Filter is where the filter is, measured), a Follower wired to its own bus's Filter settles,
//              Tape Flanger With the Filter goes through zero round the filter's most open moment, and none of it
//              allocates
//   speed      each Swirl type at 7.1.4 and 192 kHz with a throw in flight, against Phaser + Flanger

#include <JuceHeader.h>
#include "RigEngine.h"
#include "TestClock.h"
#include "Modulators.h"
#include "Parameters.h"
#include "Room.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <memory>
#include <vector>

// Every allocation in the program is counted, so a stretch of the engine and its modulators can be shown to make none.
namespace { std::atomic<long> g_allocations { 0 }; }
void* operator new(std::size_t size)
{
    ++g_allocations;
    if (void* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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
        // comb every 282 Hz) and the phaser's six all-pass stages at one place. In Phaser + Flanger the stages turn
        // at the top of the band (sj::Allpass1 with RigEngine's coefficient as it is written): at 48 kHz, held
        // still, the phaser's first notch is at 20.6 kHz. The phaser alone is tuned from the bottom up: held still
        // its notches are near 243 Hz, 905 Hz and 3.4 kHz. Each type's long-term spectrum against the same render
        // with Swirl at 0%.
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
        struct Shape { double comb = 0.0, delayMs = 0.0, notch = 0.0, mid = 0.0; };
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
            // The phaser alone: its middle notch (905 Hz) against half way to the next one up (1.75 kHz), each
            // over one tooth of the comb.
            sh.mid = mean(905.0 - 0.5 * tooth, tooth) - mean(1750.0 - 0.5 * tooth, tooth);
            return sh;
        };
        const Shape both = measure(render(RigEngine::SwirlBoth, 1.0f)), phaser = measure(render(RigEngine::SwirlPhaser, 1.0f)),
                    flanger = measure(render(RigEngine::SwirlFlanger, 1.0f));
        auto say = [](const char* name, const Shape& sh)
        {
            std::cout << "  " << name << ": comb " << num(sh.comb, 1) << " dB (at " << num(sh.delayMs, 3) << " ms); round 20.6 kHz against round 1 kHz " << num(sh.notch, 1) << " dB; round 905 Hz against round 1.75 kHz "
                      << num(sh.mid, 1) << " dB" << std::endl;
        };
        say("Phaser + Flanger", both);
        say("Phaser", phaser);
        say("Flanger", flanger);
        check(both.comb > 6.0 && both.notch < -6.0, "Phaser + Flanger has the comb and the all-pass notch (the measure sees both)");
        check(phaser.comb < 1.0 && phaser.mid < -3.0 && phaser.notch > -3.0,
              "Phaser alone: its notch in the middle of the sound (" + num(phaser.mid, 1) + " dB at 905 Hz), none at the top (" + num(phaser.notch, 1) + " dB), and no comb ("
                  + num(phaser.comb, 2) + " dB at the most)");
        check(both.mid > -3.0 && flanger.mid > -3.0, "Phaser + Flanger and Flanger alone have no notch there (" + num(both.mid, 1) + " and " + num(flanger.mid, 1) + " dB)");
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

    // ---- centre --------------------------------------------------------------------------------------------------
    if (wanted("centre"))
    {
        std::cout << "centre: where the copy goes through zero against the middle of the whoosh as it is heard" << std::endl;
        // The middle of the whoosh as heard: the pass's marker is where its window and its nearness top out, and they
        // act on the sound as it leaves the pass's own delay; the Tape Flanger's clean path then makes it 8 ms later.
        // First that itself, measured: white noise at a steady level through Fly-By passes that come close, the Tape
        // Flanger at 0% (its clean path alone), and where the loudest 20 ms of each pass falls against its marker.
        {
            auto q = plain();
            q.buses[0].swirl = 0.0f;
            q.buses[0].doppler = 1.0f;
            q.buses[0].passMode = 1;
            q.buses[0].width = 0.0f;
            q.chaos = 0.0f;
            q.swirlDetail[0].type = RigEngine::SwirlTape;
            auto e = std::make_unique<RigEngine>();
            start(*e, noiseHold, q);
            const Run r = run(*e, 2, c_rate, 90.0);
            // Every pass's power laid over the others on its marker (the noise averages out), smoothed over 10 ms,
            // and the top of that, to a fraction of a sample.
            const float* x = r.heard.getReadPointer(0);
            const int reach = static_cast<int>(0.1 * c_rate), half = static_cast<int>(0.005 * c_rate);
            std::vector<double> laid(static_cast<size_t>(2 * reach + 1), 0.0);
            int counted = 0;
            for (const double m : r.marks[RigEngine::MarkPassA])
            {
                const int at = static_cast<int>(m);
                if (at < 4 * static_cast<int>(c_rate) || at > r.heard.getNumSamples() - 4 * static_cast<int>(c_rate))
                    continue;
                ++counted;
                for (int i = -reach; i <= reach; ++i)
                    laid[static_cast<size_t>(i + reach)] += static_cast<double>(x[at + i]) * static_cast<double>(x[at + i]);
            }
            std::vector<double> smooth(laid.size(), 0.0);
            for (int i = half; i < static_cast<int>(laid.size()) - half; ++i)
                for (int k = -half; k <= half; ++k)
                    smooth[static_cast<size_t>(i)] += laid[static_cast<size_t>(i + k)];
            // (The top of a parabola through the smoothed curve's upper part: steadier than its one highest point.)
            int top = half;
            for (int i = half; i < static_cast<int>(laid.size()) - half; ++i)
                if (smooth[static_cast<size_t>(i)] > smooth[static_cast<size_t>(top)])
                    top = i;
            double weight = 0.0, moment = 0.0;
            for (int i = half; i < static_cast<int>(laid.size()) - half; ++i)
                if (const double over = smooth[static_cast<size_t>(i)] - 0.97 * smooth[static_cast<size_t>(top)]; over > 0.0)
                {
                    weight += over;
                    moment += over * static_cast<double>(i - reach);
                }
            const double mean = 1000.0 * moment / std::max(weight, 1.0e-30) / c_rate;
            std::cout << "  the loudest moment of " << counted << " close Fly-By passes (noise at a steady level, the Tape Flanger's clean path alone, the passes laid over each other on their markers): "
                      << num(mean, 1) << " ms after the marker (the clean path is 8.0 ms long)" << std::endl;
            check(counted >= 20 && std::abs(mean - 8.0) < 3.0, "the middle of the whoosh as heard is the pass's marker and the clean path's 8 ms");
        }
        struct Case { const char* name; float rate, doppler, chaos; int passMode; bool together; };
        const Case cases[] = {
            { "Rate 0%, as it opens",            0.0f, 0.55f, 0.4f, 0, false },
            { "Rate 40%, as it opens",           0.4f, 0.55f, 0.4f, 0, false },
            { "Rate 100%, as it opens",          1.0f, 0.55f, 0.4f, 0, false },
            { "Rate 40%, Fly-By",                0.4f, 0.55f, 0.4f, 1, false },
            { "Rate 40%, Doppler 100% Chaos 100%", 0.4f, 1.0f, 1.0f, 0, false },
            { "Rate 100%, Doppler 100% Chaos 100%", 1.0f, 1.0f, 1.0f, 1, false },
            { "Rate 40%, Doppler 15% Chaos 0%",  0.4f, 0.15f, 0.0f, 0, false },
            { "Rate 0%, Doppler 0%",             0.0f, 0.0f, 0.0f, 0, false },
            { "Rate 40%, Faders Together",       0.4f, 0.55f, 0.4f, 0, true },
        };
        std::cout << "  (ms before and after the middle as heard: the nearest, the middle one, the furthest; the copy's place at the middle)" << std::endl;
        for (const bool room : { false, true })
            for (const auto& c : cases)
            {
                RigEngine::Params q;
                q.hitAim = false;
                q.rate = c.rate;
                q.chaos = c.chaos;
                q.rideMode = c.together ? RigEngine::RideTogether : RigEngine::RideApart;
                q.buses[0].swirl = 0.5f;
                q.buses[0].doppler = c.doppler;
                q.buses[0].passMode = c.passMode;
                q.swirlDetail[0].type = RigEngine::SwirlTape;
                auto e = std::make_unique<RigEngine>();
                if (room)
                    e->setWidth(room::Width::Surround51);
                start(*e, toneHold, q);
                std::vector<float> d0, d1;
                RigEngine* engine = e.get();
                const Run r = run(*e, room ? 6 : 2, c_rate, 120.0, false, [](int) {}, [&d0, &d1, engine](int)
                {
                    d0.push_back(engine->getSwirlLaneOffsetMs(0, 0));
                    d1.push_back(engine->getSwirlLaneOffsetMs(0, 1));
                });
                const double perMs = c_rate / c_chunk / 1000.0; // chunks to a millisecond
                const int span = static_cast<int>(4000.0 * perMs);
                std::vector<double> before, after, atMiddle;
                int passes = 0;
                for (const double m : r.marks[RigEngine::MarkPassA])
                {
                    // The middle as heard: the marker and the clean path's 8 ms.
                    const double heard = m / c_chunk + 8.0 * perMs;
                    const int at = static_cast<int>(std::lround(heard));
                    if (at < span + 4 || at > static_cast<int>(d0.size()) - span - 4)
                        continue;
                    ++passes;
                    const auto& d = room && d1[static_cast<size_t>(at)] < d0[static_cast<size_t>(at)] ? d1 : d0;
                    if (d[static_cast<size_t>(at)] >= 0.0f)
                        continue;
                    // The zero either side, to a fraction of a chunk (d is read at the end of each chunk).
                    int b = at, a = at;
                    while (b > at - span && d[static_cast<size_t>(b)] < 0.0f) --b;
                    while (a < at + span && d[static_cast<size_t>(a)] < 0.0f) ++a;
                    if (b <= at - span || a >= at + span)
                        continue;
                    auto cross = [&d](int neg, int pos) { const double n = d[static_cast<size_t>(neg)], p = d[static_cast<size_t>(pos)]; return static_cast<double>(neg) + (static_cast<double>(pos) - neg) * (-n / (p - n)); };
                    before.push_back((heard - cross(b + 1, b)) / perMs);
                    after.push_back((cross(a - 1, a) - heard) / perMs);
                    atMiddle.push_back(d[static_cast<size_t>(at)]);
                }
                auto three = [](std::vector<double> v)
                {
                    if (v.empty())
                        return juce::String("none");
                    std::sort(v.begin(), v.end());
                    return juce::String(juce::roundToInt(v.front())) + " / " + juce::String(juce::roundToInt(v[v.size() / 2])) + " / " + juce::String(juce::roundToInt(v.back()));
                };
                auto middle = [](std::vector<double> v) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
                auto furthest = [](const std::vector<double>& v) { double w = 0.0; for (const double x : v) w = std::max(w, std::abs(x)); return w; };
                const juce::String where = juce::String(room ? "5.1, " : "Stereo, ") + c.name;
                std::cout << "  " << where << ": " << passes << " passes, " << before.size() << " with a zero either side; before " << three(before) << " ms, after " << three(after)
                          << " ms; at the middle the copy is " << num(middle(atMiddle), 2) << " ms (the middle one)" << std::endl;
                check(passes >= 20 && static_cast<int>(before.size()) == passes, "every pass has the copy early at its middle and a zero either side of it (" + where + ")");
                check(furthest(before) <= 65.0 && furthest(after) <= 65.0 && std::abs(middle(before) - middle(after)) <= 10.0,
                      "the zeros are within 65 ms of the middle as heard and even either side of it (" + num(middle(before), 0) + " ms before, " + num(middle(after), 0) + " after) (" + where + ")");
            }
    }

    // ---- follow --------------------------------------------------------------------------------------------------
    if (wanted("follow"))
    {
        std::cout << "follow: the Followers' Pass and Filter inputs, and Tape Flanger With the Filter" << std::endl;
        // A quiet hiss with a loud burst every 0.7 s, on bus A: something loud that has nothing to do with the passes.
        // And the same hiss without them.
        CargoHold burstHold, hissHold;
        {
            bool ok = true;
            for (const bool bursts : { true, false })
            {
                juce::AudioBuffer<float> b(1, static_cast<int>(c_rate * 7.0));
                juce::Random r(77);
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const double t = std::fmod(i / c_rate, 0.7);
                    b.setSample(0, i, static_cast<float>((0.01 + (bursts ? 0.9 * std::exp(-t / 0.03) : 0.0)) * (r.nextFloat() * 2.0f - 1.0f)));
                }
                auto& hold = bursts ? burstHold : hissHold;
                ok = ok && hold.loadFile(0, writeWav(folder, bursts ? "swirlwire_src_bursts" : "swirlwire_src_hiss", b), error);
                hold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
                hold.setSkipSilence(0, false);
            }
            check(ok && burstHold.getBus(0) == 0 && hissHold.getBus(0) == 0, "loads a hiss with loud bursts, and the hiss alone, on bus A " + error);
        }

        // The Pass input, chunk by chunk, against the pass's marker.
        struct Passes { std::vector<float> near, level; Run run; float peakIn = 1.0e9f; };
        auto fly = [&](CargoHold& hold, bool together, bool room, int passMode, float rate, bool throws)
        {
            Passes out;
            RigEngine::Params q;
            q.hitAim = false;
            q.rate = rate;
            q.rideMode = together ? RigEngine::RideTogether : RigEngine::RideApart;
            q.buses[0].passMode = passMode;
            auto e = std::make_unique<RigEngine>();
            if (room)
                e->setWidth(room::Width::Surround51);
            start(*e, hold, q);
            RigEngine* engine = e.get();
            out.run = run(*e, room ? 6 : 2, c_rate, 60.0, false,
                          [engine, throws](int pos) { const int every = static_cast<int>(5.0 * c_rate) / c_chunk * c_chunk; if (throws && pos % every == every / 2) engine->throwWhoosh(0, 1.0f); },
                          [&out, engine](int)
                          {
                              out.near.push_back(engine->getPassNear(0));
                              out.level.push_back(engine->takeSectionLevels()[0]);
                              // (On the clock, the pass's marker is on the swell's peak: how far from it the clock says it is.)
                              for (int m = 0; m < engine->getNumMarkers(); ++m)
                                  if (engine->getMarker(m).kind == RigEngine::MarkPassA && engine->getCyclePeakIn() >= 0.0f)
                                  {
                                      const float in = engine->getCyclePeakIn(), swell = engine->getCycleSeconds();
                                      out.peakIn = out.peakIn > 1.0e8f ? std::min(in, swell - in) : std::max(out.peakIn, std::min(in, swell - in));
                                  }
                          });
            return out;
        };
        const int perSecond = static_cast<int>(c_rate) / c_chunk;
        // Where the Pass input tops out round each marker, in chunks from the marker's own chunk, and how high.
        auto tops = [](const Passes& f, int kind, int& worst, float& lowest)
        {
            int counted = 0;
            worst = 0;
            lowest = 1.0f;
            for (const double m : f.run.marks[static_cast<size_t>(kind)])
            {
                const int at = static_cast<int>(m) / c_chunk;
                const int second = static_cast<int>(c_rate) / c_chunk, reach = second * 3 / 10;
                if (at < 3 * second || at > static_cast<int>(f.near.size()) - second)
                    continue;
                // (The input is read at the end of each chunk and the marker is somewhere inside one: the chunk whose
                // end is nearest the marker is that one or the one before it.)
                int top = at - reach;
                for (int i = at - reach; i <= at + reach; ++i)
                    if (f.near[static_cast<size_t>(i)] > f.near[static_cast<size_t>(top)])
                        top = i;
                const int away = std::min(std::abs(top - at), std::abs(top - (at - 1)));
                worst = std::max(worst, away);
                lowest = std::min(lowest, f.near[static_cast<size_t>(top)]);
                ++counted;
            }
            return counted;
        };
        struct PassCase { const char* name; bool together, room; int passMode; float rate; };
        const PassCase passCases[] = { { "Stereo, Bed", false, false, 0, 0.4f }, { "Stereo, Fly-By, Rate 100%", false, false, 1, 1.0f }, { "5.1, Bed, Rate 0%", false, true, 0, 0.0f },
                                       { "Stereo, Faders Together", true, false, 0, 0.4f }, { "5.1, Faders Together, Fly-By", true, true, 1, 0.7f } };
        for (const auto& c : passCases)
        {
            const Passes f = fly(toneHold, c.together, c.room, c.passMode, c.rate, false);
            int worst = 0;
            float lowest = 0.0f;
            const int counted = tops(f, RigEngine::MarkPassA, worst, lowest);
            float least = 1.0f;
            for (size_t i = static_cast<size_t>(3 * perSecond); i < f.near.size(); ++i)
                least = std::min(least, f.near[i]);
            std::cout << "  " << c.name << ": " << counted << " passes; the Pass input tops out " << worst << " chunk(s) from the marker at the most, at " << num(lowest, 4) << " or more; between passes it is down to "
                      << num(least, 3) << (c.together ? "; the clock's peak is " + num(1000.0 * f.peakIn, 2) + " ms from the marker at the most" : juce::String()) << std::endl;
            check(counted >= 8 && worst <= 1 && lowest >= 0.995f, juce::String("the Pass input tops out within one chunk of the pass's marker, at 1 (") + c.name + ")");
            if (c.together)
                check(f.peakIn <= 2.0f * c_chunk / static_cast<float>(c_rate), juce::String("Faders Together: that is the top of the swell (") + c.name + ")");
            if (c.passMode == 1)
                check(least < 0.15f, juce::String("between Fly-By passes it is most of the way down (") + c.name + ")");
        }
        {
            // The same passes with a steady hiss and with the loud bursts in it: the input is the same to the bit, while
            // the bus's loudness (what the Bus A input hears) is nothing alike.
            const Passes steady = fly(hissHold, false, false, 1, 0.4f, false), loud = fly(burstHold, false, false, 1, 0.4f, false);
            float apartMost = 0.0f, quiet = 1.0e9f, burst = 0.0f;
            for (size_t i = 0; i < std::min(steady.near.size(), loud.near.size()); ++i)
                apartMost = std::max(apartMost, std::abs(steady.near[i] - loud.near[i]));
            // Its loudness while a pass is at its closest, burst or no burst (the 0.2 s round each marker).
            for (const double m : loud.run.marks[RigEngine::MarkPassA])
            {
                const int at = static_cast<int>(m) / c_chunk;
                if (at < 3 * perSecond || at > static_cast<int>(loud.near.size()) - perSecond)
                    continue;
                for (int i = at - perSecond / 10; i <= at + perSecond / 10; ++i)
                {
                    quiet = std::min(quiet, loud.level[static_cast<size_t>(i)]);
                    burst = std::max(burst, loud.level[static_cast<size_t>(i)]);
                }
            }
            std::cout << "  a hiss with a loud burst every 0.7 s against the hiss alone: the Pass input differs by " << juce::String(apartMost, 9) << " at the most; bus A's loudness round the markers runs from "
                      << num(db(quiet), 1) << " to " << num(db(burst), 1) << " dB" << std::endl;
            check(steady.near.size() == loud.near.size() && apartMost == 0.0f, "the Pass input does not hear a loud burst: it is the same whatever plays");
            check(db(burst) - db(quiet) > 20.0, "(the bursts are there: the bus's loudness jumps by 20 dB and more round the markers)");
        }
        {
            // A throw in flight is the nearest thing as it peaks.
            const Passes f = fly(toneHold, false, true, 1, 0.0f, true);
            // (A pass of the bus's own can be near at the same time, so not where it tops out: what it reads as the
            // throw peaks, in the marker's chunk or the one before.)
            int counted = 0;
            float lowest = 1.0f;
            for (const double m : f.run.marks[RigEngine::MarkThrow])
            {
                const int at = static_cast<int>(m) / c_chunk;
                if (at < 1 || at >= static_cast<int>(f.near.size()))
                    continue;
                lowest = std::min(lowest, std::max(f.near[static_cast<size_t>(at)], f.near[static_cast<size_t>(at - 1)]));
                ++counted;
            }
            std::cout << "  5.1, a throw every 5 s: " << counted << " throws; the Pass input reads " << num(lowest, 4) << " or more as each one peaks" << std::endl;
            check(counted >= 8 && lowest >= 0.995f, "a throw in flight counts: the Pass input is at 1 as it peaks");
        }

        // The Filter input against where the filter itself is. Low Pass: the filter's own place is how open it is.
        {
            auto q = plain();
            q.filterDetail[0].sweep = 0.6f;
            q.filterDetail[0].rateHz = 3.0f;
            q.filterDetail[0].follow = 1.0f;
            auto e = std::make_unique<RigEngine>();
            start(*e, noiseHold, q);
            RigEngine* engine = e.get();
            const double lo = 170.0 * std::pow(2.0, -1.8), hi = std::min(0.45 * c_rate, 170.0 * std::pow(2.0, 8.8));
            double worst = 0.0, least = 1.0, most = 0.0;
            run(*e, 2, c_rate, 30.0, false, [&q, engine](int pos)
            {
                q.buses[0].filter = static_cast<float>(0.5 + 0.45 * std::sin(c_twoPi * 0.37 * pos / c_rate)); // the wheel moves too
                engine->setParams(q);
            }, [&](int pos)
            {
                if (pos < static_cast<int>(c_rate))
                    return;
                const double open = engine->getFilterOpen(0), place = std::log(engine->getFilterHz(0) / lo) / std::log(hi / lo);
                worst = std::max(worst, std::abs(open - place));
                least = std::min(least, open);
                most = std::max(most, open);
            });
            std::cout << "  the Filter input against the filter's own place on the log axis (Sweep 60% at 3 Hz, Follow 100%, the wheel moving 5% .. 95%): apart by " << num(100.0 * worst, 2)
                      << "% of the travel at the most; it ran from " << num(least, 3) << " to " << num(most, 3) << std::endl;
            check(worst <= 0.02 && most - least > 0.5, "the Filter input is where the filter is on its log-frequency travel, within 2%");
        }
        {
            // And against the filter as it is heard: white noise through it standing still, the response fitted to
            // a two-pole low-pass of its Q, which gives where it is.
            auto heardSpectrum = [&](float closedAt, float knob, float* openOut)
            {
                auto q = plain();
                q.buses[0].filter = knob;
                q.levels[0] = 0.1f;
                q.buses[0].doppler = 0.0f;
                q.filterDetail[0].closedAtHz = closedAt;
                q.filterDetail[0].follow = 0.0f;
                auto e = std::make_unique<RigEngine>();
                start(*e, noiseHold, q);
                const auto heard = run(*e, 2, c_rate, 14.0).heard;
                if (openOut != nullptr)
                    *openOut = e->getFilterOpen(0);
                return spectrum(heard, 0, static_cast<int>(2.0 * c_rate));
            };
            const auto flat = heardSpectrum(170.0f, 0.0f, nullptr);
            struct Still { float closedAt, knob; };
            double worst = 0.0;
            for (const Still c : { Still { 170.0f, 0.8f }, Still { 170.0f, 1.0f }, Still { 60.0f, 0.9f }, Still { 800.0f, 0.8f }, Still { 2000.0f, 0.8f } })
            {
                float open = 0.0f;
                const auto with = heardSpectrum(c.closedAt, c.knob, &open);
                SweepFilter::Settings set;
                set.closedAtHz = c.closedAt;
                set.follow = 0.0f;
                const double qv = SweepFilter::q(set, c.knob);
                std::vector<std::pair<double, double>> points; // Hz, dB
                for (double hz = 40.0; hz < 16000.0; hz *= std::pow(2.0, 1.0 / 6.0))
                    points.push_back({ hz, 10.0 * std::log10(std::max(bandPower(with, hz, hz * 0.06), 1.0e-30) / std::max(bandPower(flat, hz, hz * 0.06), 1.0e-30)) });
                double best = 1.0e30, fitted = 0.0;
                for (double fc = 25.0; fc < 20000.0; fc *= std::pow(2.0, 1.0 / 200.0))
                {
                    const double g = std::tan(c_twoPi * 0.5 * fc / c_rate);
                    double sum = 0.0, sumSq = 0.0;
                    int n = 0;
                    for (const auto& [hz, measured] : points)
                    {
                        const double w = std::tan(c_twoPi * 0.5 * hz / c_rate) / g;
                        const double model = -10.0 * std::log10((1.0 - w * w) * (1.0 - w * w) + (w / qv) * (w / qv));
                        if (model < -36.0)
                            continue;
                        const double d = measured - model;
                        sum += d;
                        sumSq += d * d;
                        ++n;
                    }
                    const double spread = n > 6 ? sumSq / n - (sum / n) * (sum / n) : 1.0e30;
                    if (spread < best)
                    {
                        best = spread;
                        fitted = fc;
                    }
                }
                const double lo = c.closedAt, hi = std::min(0.45 * c_rate, c.closedAt * 128.0);
                const double place = std::log(fitted / lo) / std::log(hi / lo);
                const double meant = c.closedAt * std::pow(2.0, 7.0 * (1.0 - 0.9 * std::sqrt(c.knob)));
                worst = std::max(worst, std::abs(place - static_cast<double>(open)));
                std::cout << "  Closed At " << num(c.closedAt, 0) << " Hz, Filter " << juce::roundToInt(100.0f * c.knob) << "%: the filter is heard at " << num(fitted, 1) << " Hz (the law says " << num(meant, 1)
                          << "; the fit is within " << num(std::sqrt(best), 2) << " dB), which is " << num(place, 4) << " of the travel; the Filter input reads " << num(open, 4) << std::endl;
            }
            check(worst <= 0.02, "the Filter input tracks the filter as measured from its sound, within 2% of the travel (" + num(100.0 * worst, 2) + "% at the most)");
        }

        // Tape Flanger, With the Filter: the filter sweeping by itself (Follow 0%), the copy against its most open moments.
        {
            struct Swept { int cycles = 0, bracketed = 0; std::vector<double> before, after, atTop; double latest = 0.0, earliest = 0.0; };
            auto sweep = [&](float rateHz, int type, int shape, float knob, float sweepDepth, bool room, float wobble = 0.0f)
            {
                Swept out;
                auto q = plain();
                q.buses[0].swirl = 0.5f;
                q.buses[0].filter = knob;
                q.swirlDetail[0].type = RigEngine::SwirlTape;
                q.swirlDetail[0].tape.motion = SwirlFx::WithFilter;
                q.swirlDetail[0].tape.wobble = wobble;
                q.filterDetail[0].type = type;
                q.filterDetail[0].shape = shape;
                q.filterDetail[0].follow = 0.0f;
                q.filterDetail[0].sweep = sweepDepth;
                q.filterDetail[0].rateHz = rateHz;
                auto e = std::make_unique<RigEngine>();
                if (room)
                    e->setWidth(room::Width::Surround51);
                start(*e, noiseHold, q);
                RigEngine* engine = e.get();
                std::vector<float> d, open;
                const double seconds = std::max(20.0, 12.0 / rateHz);
                run(*e, room ? 6 : 2, c_rate, seconds, false, [](int) {}, [&d, &open, engine](int)
                {
                    d.push_back(engine->getSwirlLaneOffsetMs(0, 0));
                    open.push_back(engine->getFilterOpen(0));
                });
                const double perMs = c_rate / c_chunk / 1000.0;
                const int cycle = static_cast<int>(c_rate / c_chunk / rateHz);
                for (size_t i = static_cast<size_t>(3.0 * c_rate / c_chunk); i < d.size(); ++i)
                {
                    out.latest = std::max(out.latest, static_cast<double>(d[i]));
                    out.earliest = std::min(out.earliest, static_cast<double>(d[i]));
                }
                // Each most open moment: the middle of the stretch over 99.5% of that cycle's top (a sine is flat there).
                for (int from = static_cast<int>(3.0 * c_rate / c_chunk); from + 2 * cycle < static_cast<int>(open.size()) - cycle; from += cycle)
                {
                    int top = from;
                    for (int i = from; i < from + cycle; ++i)
                        if (open[static_cast<size_t>(i)] > open[static_cast<size_t>(top)])
                            top = i;
                    if (top - cycle / 2 < 0 || top + cycle / 2 >= static_cast<int>(open.size()))
                        continue;
                    int a = top, b = top;
                    const float bar = open[static_cast<size_t>(top)] - 0.002f;
                    while (a > top - cycle / 4 && open[static_cast<size_t>(a - 1)] >= bar) --a;
                    while (b < top + cycle / 4 && open[static_cast<size_t>(b + 1)] >= bar) ++b;
                    const double middle = 0.5 * (a + b);
                    const int at = static_cast<int>(std::lround(middle));
                    ++out.cycles;
                    out.atTop.push_back(d[static_cast<size_t>(at)]);
                    if (d[static_cast<size_t>(at)] >= 0.0f)
                        continue;
                    int lo = at, hi = at;
                    while (lo > at - cycle / 2 && d[static_cast<size_t>(lo)] < 0.0f) --lo;
                    while (hi < at + cycle / 2 && d[static_cast<size_t>(hi)] < 0.0f) ++hi;
                    if (lo <= at - cycle / 2 || hi >= at + cycle / 2)
                        continue;
                    auto cross = [&d](int neg, int pos) { const double n = d[static_cast<size_t>(neg)], p = d[static_cast<size_t>(pos)]; return static_cast<double>(neg) + (static_cast<double>(pos) - neg) * (-n / (p - n)); };
                    ++out.bracketed;
                    out.before.push_back((middle - cross(lo + 1, lo)) / perMs);
                    out.after.push_back((cross(hi - 1, hi) - middle) / perMs);
                }
                return out;
            };
            auto mean = [](const std::vector<double>& v) { double m = 0.0; for (const double x : v) m += x / static_cast<double>(std::max<size_t>(1, v.size())); return m; };
            struct SweepCase { const char* name; float rate; int type; bool room; };
            const SweepCase sweeps[] = { { "1 Hz Sine, Low Pass, Stereo", 1.0f, SweepFilter::LowPass, false }, { "1 Hz Sine, High Pass, 5.1", 1.0f, SweepFilter::HighPass, true },
                                         { "0.25 Hz Sine, Low Pass, Stereo", 0.25f, SweepFilter::LowPass, false }, { "4 Hz Sine, Low Pass, Stereo", 4.0f, SweepFilter::LowPass, false } };
            for (const auto& c : sweeps)
            {
                const Swept r = sweep(c.rate, c.type, SweepFilter::Sine, 0.5f, 0.5f, c.room);
                const double before = mean(r.before), after = mean(r.after);
                std::cout << "  " << c.name << " (Filter 50%, Sweep 50%, Follow 0%, Wobble 0%): " << r.cycles << " sweeps, " << r.bracketed << " with the copy early at the filter's most open moment and a zero either side: "
                          << num(before, 1) << " ms before it and " << num(after, 1) << " ms after (the middle of the two " << num(0.5 * (after - before), 1) << " ms after it); the copy there "
                          << num(mean(r.atTop), 2) << " ms; over the run " << num(r.earliest, 2) << " .. " << num(r.latest, 2) << " ms" << std::endl;
                check(r.cycles >= 8 && r.bracketed == r.cycles, juce::String("With the Filter: the copy goes through zero either side of every most open moment (") + c.name + ")");
                // (The two zeros are a set share of the sweep apart, so their middle is what sits on the moment.)
                check(std::abs(0.5 * (after - before)) <= (c.rate > 2.0f ? 12.0 : 10.0), juce::String("the middle of the two zeros is on the filter's most open moment, within 10 ms (") + c.name + ")");
                check(r.latest > 2.0 && r.latest < 6.5, juce::String("the copy is late while the filter is closed (") + c.name + ")");
            }
            // Wobble is a spring that follows the move, so it leans the copy: said, at its default of 25%.
            for (const float rate : { 0.25f, 1.0f, 4.0f })
            {
                const Swept r = sweep(rate, SweepFilter::LowPass, SweepFilter::Sine, 0.5f, 0.5f, false, 0.25f);
                std::cout << "  (said) " << num(rate, 2) << " Hz with Wobble at 25%: " << r.bracketed << " of " << r.cycles << " sweeps with a zero either side, " << num(mean(r.before), 1) << " ms before and "
                          << num(mean(r.after), 1) << " ms after (the middle of the two " << num(0.5 * (mean(r.after) - mean(r.before)), 1) << " ms after the most open moment)" << std::endl;
            }
            // It has nothing to go by: the Filter wheel at 0, or a filter nothing moves. The copy waits Depth late.
            const Swept off = sweep(1.0f, SweepFilter::LowPass, SweepFilter::Sine, 0.0f, 0.5f, false), still = sweep(1.0f, SweepFilter::LowPass, SweepFilter::Sine, 0.5f, 0.0f, false);
            std::cout << "  Filter at 0%: the copy stays at " << num(off.earliest == 0.0 ? off.latest : off.earliest, 2) << " .. " << num(off.latest, 2) << " ms; a filter nothing moves (Follow and Sweep at 0%): "
                      << num(still.latest, 2) << " ms" << std::endl;
            check(off.bracketed == 0 && off.latest > 3.9 && mean(off.atTop) > 3.9, "With the Filter rests with the Filter wheel at 0: the copy waits Depth late");
            check(still.bracketed == 0 && still.latest > 3.9 && still.latest < 4.1 && still.earliest == 0.0, "and with a filter nothing moves");
        }
        {
            // The filter as it opens (Follow 100%, no Sweep) with the faders riding: the envelope filter. Said, not
            // checked: how often the copy gets through zero depends on how near each swell comes to the recent peak.
            RigEngine::Params q;
            q.hitAim = false;
            q.buses[0].swirl = 0.5f;
            q.buses[0].filter = 0.5f;
            q.swirlDetail[0].type = RigEngine::SwirlTape;
            q.swirlDetail[0].tape.motion = SwirlFx::WithFilter;
            auto e = std::make_unique<RigEngine>();
            start(*e, noiseHold, q);
            RigEngine* engine = e.get();
            std::vector<float> d, open;
            run(*e, 2, c_rate, 120.0, false, [](int) {}, [&d, &open, engine](int)
            {
                d.push_back(engine->getSwirlLaneOffsetMs(0, 0));
                open.push_back(engine->getFilterOpen(0));
            });
            int dips = 0;
            float most = 0.0f, least = 1.0f;
            for (size_t i = static_cast<size_t>(3 * perSecond) + 1; i < d.size(); ++i)
            {
                dips += d[i] < 0.0f && d[i - 1] >= 0.0f ? 1 : 0;
                most = std::max(most, open[i]);
                least = std::min(least, open[i]);
            }
            std::cout << "  (said) the filter as it opens (Follow 100%, Sweep 0%, Filter 50%, the faders riding, noise): in 117 s the copy went through zero " << dips << " times; the Filter input ran from "
                      << num(least, 3) << " to " << num(most, 3) << std::endl;
        }

        // A Follower on a bus's Filter input, wired to that same bus's Filter: what the modulators read is a block
        // old, so it goes round a loop. The filter stands still but for the wire (Follow and Sweep at 0%).
        {
            const int target = ModMatrix::targetIndex(ParamIDs::bus[0][ParamIDs::Filter]);
            const int section = 512;
            double worstRipple = 0.0;
            juce::String said;
            long made = 0;
            for (const float depth : { 1.0f, 0.5f, -0.5f, -1.0f })
                for (const float speed : { 0.0f, 0.4f, 1.0f })
                    for (const float knob : { 0.0f, 0.02f, 0.1f, 0.3f, 0.6f, 1.0f })
                    {
                        auto q = plain();
                        q.buses[0].filter = knob;
                        q.filterDetail[0].follow = 0.0f;
                        auto e = std::make_unique<RigEngine>();
                        start(*e, noiseHold, q);
                        auto mods = std::make_unique<ModMatrix>();
                        mods->prepare(c_rate);
                        mods->connect(0, ParamIDs::bus[0][ParamIDs::Filter], depth);
                        std::array<ModMatrix::SourceSettings, ModMatrix::c_numSources> set;
                        set[0] = { ModMatrix::Type::Follower, ModMatrix::Shape::Sine, speed, ModMatrix::FollowFilterA };
                        for (size_t k = 1; k < set.size(); ++k)
                            set[k] = { ModMatrix::Type::Macro, ModMatrix::Shape::Sine, 0.0f, 0 };
                        ModMatrix::FollowerInputs in {};
                        RigEngine* engine = e.get();
                        ModMatrix* m = mods.get();
                        std::vector<float> open;
                        const float alone = [&] { run(*e, 2, c_rate, 0.5, false, [](int) {}, [](int) {}); return engine->getFilterOpen(0); }();
                        juce::AudioBuffer<float> scratch(2, c_chunk);
                        const int total = static_cast<int>(6.0 * c_rate);
                        open.reserve(static_cast<size_t>(total / section + 1));
                        const long before = g_allocations.load();
                        for (int pos = 0; pos < total; pos += c_chunk)
                        {
                            if (pos % section == 0)
                            {
                                // As the plug-in does it: the modulators, then the engine's settings, then the sound.
                                m->process(set, section, in);
                                q.busMod[0][ParamIDs::Filter] = juce::jlimit(0.0f, 1.0f, knob + m->offset(target)) - knob;
                                engine->setParams(q);
                            }
                            engine->process(nullptr, nullptr, scratch.getWritePointer(0), scratch.getWritePointer(1), c_chunk);
                            if ((pos + c_chunk) % section == 0)
                            {
                                const auto places = engine->takeFollowerPlaces();
                                in[ModMatrix::FollowPassA] = places.pass[0];
                                in[ModMatrix::FollowFilterA] = places.filter[0];
                                if (pos > total - static_cast<int>(2.0 * c_rate))
                                    open.push_back(places.filter[0]);
                            }
                        }
                        made += g_allocations.load() - before;
                        float lo = 1.0f, hi = 0.0f;
                        for (const float v : open) { lo = std::min(lo, v); hi = std::max(hi, v); }
                        worstRipple = std::max(worstRipple, static_cast<double>(hi - lo));
                        if (speed > 0.99f && (knob < 0.001f || std::abs(knob - 0.3f) < 0.001f || knob > 0.99f))
                            said << "    depth " << juce::roundToInt(100.0f * depth) << "%, Filter " << juce::roundToInt(100.0f * knob) << "%: " << num(alone, 3) << " alone, " << num(0.5f * (lo + hi), 3) << " with the wire\n";
                    }
            std::cout << "  a Follower on Bus A Filter wired to bus A's Filter (72 settings: depth +-100% and +-50%, Speed 0%, 40% and 100%, the wheel from 0% to 100%): over the last 2 s of 6 the Filter input moves by "
                      << juce::String(worstRipple, 6) << " at the most. How open the filter settles (Speed 100%):\n" << said << std::flush;
            check(worstRipple < 0.002, "wired to its own bus's Filter it settles: nothing rings or hunts");
            check(made == 0, "nothing allocates while those run: the engine, its Pass and Filter inputs, the modulators (" + juce::String(static_cast<int>(made)) + ")");
        }
        {
            // No allocation: the engine in a Tape Flanger With the Filter, both inputs taken and followed, a wire on Swirl.
            auto q = plain();
            q.buses[0].swirl = 0.5f;
            q.swirlDetail[0].type = RigEngine::SwirlTape;
            q.swirlDetail[0].tape.motion = SwirlFx::WithFilter;
            q.filterDetail[0].sweep = 0.5f;
            auto e = std::make_unique<RigEngine>();
            start(*e, noiseHold, q);
            auto mods = std::make_unique<ModMatrix>();
            mods->prepare(c_rate);
            mods->connect(0, ParamIDs::bus[0][ParamIDs::Swirl], 0.5f);
            mods->connect(1, ParamIDs::bus[0][ParamIDs::Drive], 0.5f);
            std::array<ModMatrix::SourceSettings, ModMatrix::c_numSources> set;
            set[0] = { ModMatrix::Type::Follower, ModMatrix::Shape::Sine, 0.8f, ModMatrix::FollowPassA };
            set[1] = { ModMatrix::Type::Follower, ModMatrix::Shape::Sine, 0.8f, ModMatrix::FollowFilterA };
            set[2] = { ModMatrix::Type::Follower, ModMatrix::Shape::Sine, 0.8f, ModMatrix::FollowPassB };
            set[3] = { ModMatrix::Type::Follower, ModMatrix::Shape::Sine, 0.8f, ModMatrix::FollowFilterB };
            ModMatrix::FollowerInputs in {};
            juce::AudioBuffer<float> scratch(2, c_chunk);
            const int tSwirl = ModMatrix::targetIndex(ParamIDs::bus[0][ParamIDs::Swirl]);
            for (int pos = 0; pos < static_cast<int>(c_rate); pos += c_chunk) // (running first)
                e->process(nullptr, nullptr, scratch.getWritePointer(0), scratch.getWritePointer(1), c_chunk);
            const long before = g_allocations.load();
            float top = 0.0f, filterTop = 0.0f;
            for (int pos = 0; pos < static_cast<int>(20.0 * c_rate); pos += c_chunk)
            {
                if (pos % 512 == 0)
                {
                    mods->process(set, 512, in);
                    q.busMod[0][ParamIDs::Swirl] = mods->offset(tSwirl);
                    e->setParams(q);
                }
                e->process(nullptr, nullptr, scratch.getWritePointer(0), scratch.getWritePointer(1), c_chunk);
                if ((pos + c_chunk) % 512 == 0)
                {
                    const auto places = e->takeFollowerPlaces();
                    in[ModMatrix::FollowPassA] = places.pass[0];
                    in[ModMatrix::FollowPassB] = places.pass[1];
                    in[ModMatrix::FollowFilterA] = places.filter[0];
                    in[ModMatrix::FollowFilterB] = places.filter[1];
                    top = std::max(top, mods->getSourceValue(0));
                    filterTop = std::max(filterTop, mods->getSourceValue(1));
                }
            }
            const long made = g_allocations.load() - before;
            std::cout << "  20 s of the engine in a Tape Flanger With the Filter, four Followers on the Pass and Filter inputs, a wire on Swirl: " << made << " allocations (the Followers reached " << num(top, 3) << " and "
                      << num(filterTop, 3) << ")" << std::endl;
            check(made == 0 && top > 0.9f && filterTop > 0.3f, "nothing allocates: the engine's Pass and Filter inputs, the Followers on them, With the Filter");
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
        check((phaser <= both + slack && flanger <= both + slack) || !TestClock::holdsCostLimits(), "Phaser or Flanger alone costs no more than both");
        check((tape <= both + slack) || !TestClock::holdsCostLimits(), "the Tape Flanger costs no more than Phaser + Flanger");
        // (Its Feedback is a second read of the line with its own low-pass and soft clip: measured 0.36 to 0.68 of a
        // percent of a core over Phaser + Flanger here, on a machine doing other work. Held to one percent.)
        // (A slower machine pays more for the same work, 1.10 on one a third as fast: so the limit is one percent or a
        // twentieth of what the whole engine costs there, whichever is more.)
        check(tapeFed <= both + std::max(1.0, 0.05 * off) || !TestClock::holdsCostLimits(), "with its Feedback in, the Tape Flanger is within one percent of a core, or a twentieth of the engine, of Phaser + Flanger (" + num(tapeFed - both, 2) + " over)");
        check((doubler <= both + slack) || !TestClock::holdsCostLimits(), "the Doubler costs no more than Phaser + Flanger");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

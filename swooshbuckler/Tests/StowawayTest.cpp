// Stowaway test: the extra bus's modes, through the real engine (no window).
//
//   Sub
//   - It filters: white noise sent to it comes out at least 45 dB down one octave above the crossover and 80 dB
//     down from two octaves above, with the crossover at 40, 120 and 250 Hz, at 48 and at 192 kHz. (The same
//     measurement in Dry, which passes the noise on, reads about 0 dB: the control.)
//   - With Low End and Drive at 100% nothing from 2.5 times the crossover upwards is within 40 dB of the passband.
//   - A 30 Hz tone passes within 1 dB; left and right are the same signal; Width does nothing.
//   - The filter's delay at each crossover, against the same design at 24 dB per octave (printed).
//   - Follow: 0% is a steady sub; the more Follow, the further it rises and falls with bus A (evenly, in dB); a
//     sub's own tracks are not ridden while it has a bus to follow; with no track on bus A it follows bus B; with
//     none on either its tracks are ridden instead, so it is never stuck silent.
//   - The hits do not duck it; moving the crossover does not click; its filters ring out and then it leaves the
//     mix.
//   - How its knobs read and which are in use (Width reads Mono and is out of use).
//
// Usage: SwooshbucklerStowaway [scratch folder] (a folder in the temp directory if none is given).

#include <JuceHeader.h>
#include "Parameters.h"
#include "RigEngine.h"
#include "SubFilter.h"

#include <cmath>
#include <complex>
#include <iostream>

namespace
{
    constexpr int c_block = 256;
    constexpr double c_twoPi = juce::MathConstants<double>::twoPi;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok)
            ++g_failures;
    }

    juce::File writeWav(const juce::File& folder, const juce::String& name, const juce::AudioBuffer<float>& audio, double rate)
    {
        const auto file = folder.getChildFile(name + ".wav");
        file.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(file);
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream, rate, 1, 24, {}, 0));
        writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
        return file;
    }

    juce::AudioBuffer<float> whiteNoise(double seconds, double rate, int seed)
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(seconds * rate));
        juce::Random r(seed);
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample(0, i, 0.5f * (r.nextFloat() * 2.0f - 1.0f));
        return b;
    }

    juce::AudioBuffer<float> tone(double hz, double seconds, double rate, float gain = 0.4f)
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(seconds * rate));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample(0, i, gain * static_cast<float>(std::sin(c_twoPi * hz * i / rate)));
        return b;
    }

    /** A tone that swells: a raised cosine every 2 s, 30 dB from trough to peak (whole cycles: it loops cleanly). */
    juce::AudioBuffer<float> swellingTone(double hz, double seconds, double rate)
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(seconds * rate));
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = i / rate;
            const double u = 0.5 - 0.5 * std::cos(c_twoPi * t / 2.0);
            b.setSample(0, i, static_cast<float>(0.5 * std::pow(10.0, (-30.0 + 30.0 * u) / 20.0) * std::sin(c_twoPi * hz * t)));
        }
        return b;
    }

    /** A plain rig: steady faders, no colour, no echoes, no hits, Stowaway a steady Sub. */
    RigEngine::Params plainParams()
    {
        RigEngine::Params p;
        p.rideOn = false;
        p.fill = 0.0f;
        p.space = 0.0f;
        p.chaos = 0.0f;
        p.hitAim = false;
        p.gateMode = 3; // Open
        p.hitBoost = 0.0f;
        p.hitDuck = 0.0f;
        for (auto& b : p.buses)
        {
            b.doppler = 0.0f;
            b.bend = 0.0f;
            b.width = 0.0f;
            b.low = b.swirl = b.filter = b.drive = b.duck = b.key = 0.0f;
        }
        p.stow.mode = RigEngine::StowSub;
        p.stow.follow = 0.0f;
        p.stow.low = 0.0f;
        p.stow.drive = 0.0f;
        return p;
    }

    juce::AudioBuffer<float> render(RigEngine& rig, double seconds, double rate, const std::function<void(int)>& afterBlock = {})
    {
        juce::AudioBuffer<float> out(2, static_cast<int>(seconds * rate));
        for (int pos = 0; pos < out.getNumSamples(); pos += c_block)
        {
            rig.process(nullptr, nullptr, out.getWritePointer(0, pos), out.getWritePointer(1, pos), std::min(c_block, out.getNumSamples() - pos));
            if (afterBlock)
                afterBlock(pos);
        }
        return out;
    }

    // ---- spectrum ----------------------------------------------------------------------------------------
    void fft(std::vector<std::complex<double>>& a)
    {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const double angle = -c_twoPi / static_cast<double>(len);
            const std::complex<double> step(std::cos(angle), std::sin(angle));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<double> w(1.0, 0.0);
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= step;
                }
            }
        }
    }

    double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 80; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    /** The power in each bin (averaged over half-overlapped frames, then over `smooth` neighbouring bins), in dB.
        A Kaiser window with side lobes far under what is being measured (beta 20: about 150 dB), so a passband
        cannot leak into a stopband 100 dB below it. */
    struct Spectrum
    {
        std::vector<double> db;
        double binHz = 1.0;

        /** The loudest bin from `from` Hz to `to` Hz. */
        double peak(double from, double to) const
        {
            double m = -400.0;
            for (size_t k = static_cast<size_t>(std::ceil(from / binHz)); k < db.size() && static_cast<double>(k) * binHz <= to; ++k)
                m = std::max(m, db[k]);
            return m;
        }
    };

    Spectrum spectrum(const float* x, int n, double rate, int order, int smooth = 8)
    {
        const size_t size = static_cast<size_t>(1) << order;
        std::vector<double> window(size);
        for (size_t i = 0; i < size; ++i)
        {
            const double r = 2.0 * static_cast<double>(i) / static_cast<double>(size - 1) - 1.0;
            window[i] = besselI0(20.0 * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(20.0);
        }
        std::vector<double> power(size / 2 + 1, 0.0);
        std::vector<std::complex<double>> buf(size);
        int frames = 0;
        for (size_t s = 0; s + size <= static_cast<size_t>(n); s += size / 2)
        {
            for (size_t i = 0; i < size; ++i)
                buf[i] = { static_cast<double>(x[s + i]) * window[i], 0.0 };
            fft(buf);
            for (size_t k = 0; k <= size / 2; ++k)
                power[k] += std::norm(buf[k]);
            ++frames;
        }
        Spectrum sp;
        sp.binHz = rate / static_cast<double>(size);
        sp.db.resize(power.size());
        for (size_t k = 0; k < power.size(); ++k)
        {
            double sum = 0.0;
            int count = 0;
            for (size_t j = k >= static_cast<size_t>(smooth / 2) ? k - static_cast<size_t>(smooth / 2) : 0; j < std::min(power.size(), k + static_cast<size_t>(smooth / 2) + 1); ++j)
            {
                sum += power[j];
                ++count;
            }
            sp.db[k] = 10.0 * std::log10(std::max(sum / (std::max(1, count) * std::max(1, frames)), 1.0e-300));
        }
        return sp;
    }

    double rmsDb(const juce::AudioBuffer<float>& a, int channel, int from, int to)
    {
        double sum = 0.0;
        for (int i = from; i < to; ++i)
            sum += static_cast<double>(a.getSample(channel, i)) * a.getSample(channel, i);
        return 10.0 * std::log10(std::max(sum / std::max(1, to - from), 1.0e-30));
    }

    /** The delay of a filter (seconds) at a frequency, from its impulse response: minus the slope of its phase. */
    template <typename Filter>
    double groupDelay(Filter&& run, double hz, double rate)
    {
        const int n = static_cast<int>(rate * 2.0);
        std::vector<double> h(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            h[static_cast<size_t>(i)] = run(i == 0 ? 1.0 : 0.0);
        auto phaseAt = [&](double f)
        {
            std::complex<double> sum(0.0, 0.0);
            for (int i = 0; i < n; ++i)
                sum += h[static_cast<size_t>(i)] * std::polar(1.0, -c_twoPi * f * i / rate);
            return std::arg(sum);
        };
        const double df = 0.05;
        double d = phaseAt(hz + df) - phaseAt(hz - df);
        while (d > juce::MathConstants<double>::pi) d -= c_twoPi;
        while (d < -juce::MathConstants<double>::pi) d += c_twoPi;
        return -d / (c_twoPi * 2.0 * df);
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto folder = argc > 1 ? juce::File(juce::String(argv[1]))
                                 : juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SwooshbucklerStowawayTest");
    folder.createDirectory();
    juce::String error;

    /** One engine with its own sounds, at a sample rate. */
    struct Rig
    {
        CargoHold hold;
        RigEngine engine;
        double rate = 48000.0;

        void start(const RigEngine::Params& p, double sampleRate)
        {
            rate = sampleRate;
            engine.setCargoHold(&hold);
            engine.setParams(p);
            engine.prepare(rate, c_block);
            engine.setParams(p);
        }
    };

    //==========================================================================
    std::cout << "Sub: only the low end comes through" << std::endl;
    {
        struct Measured { double oneOctave = 0.0, twoOctaves = 0.0, passbandRipple = 0.0; };
        // White noise (flat to half the sample rate) on Stowaway, nothing else playing. The reference is the
        // loudest part of what comes out (the passband); "down" is how far under it the loudest bin is.
        auto measure = [&](double rate, float crossoverHz, int mode)
        {
            Rig rig;
            rig.hold.loadFile(0, writeWav(folder, "stow_noise_" + juce::String(juce::roundToInt(rate)), whiteNoise(8.0, rate, 7), rate), error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plainParams();
            p.stow.mode = mode;
            p.stow.filter = Params::stowCrossoverKnob(crossoverHz);
            rig.start(p, rate);
            render(rig.engine, 1.0, rate);
            const auto out = render(rig.engine, 24.0, rate);
            const auto sp = spectrum(out.getReadPointer(0), out.getNumSamples(), rate, rate > 100000.0 ? 18 : 16);
            const double fc = crossoverHz;
            const double ref = sp.peak(20.0, fc);
            Measured m;
            m.oneOctave = ref - sp.peak(2.0 * fc, 2.5 * fc);
            m.twoOctaves = ref - sp.peak(4.0 * fc, 0.5 * rate);
            return m;
        };
        bool steep = true;
        for (const double rate : { 48000.0, 192000.0 })
            for (const float hz : { 40.0f, 120.0f, 250.0f })
            {
                const auto m = measure(rate, hz, RigEngine::StowSub);
                std::cout << "  " << juce::roundToInt(rate / 1000.0) << " kHz, crossover " << hz << " Hz: " << juce::String(m.oneOctave, 1) << " dB down one octave above, "
                          << juce::String(m.twoOctaves, 1) << " dB down from two octaves above" << std::endl;
                const bool ok = m.oneOctave >= 45.0 && m.twoOctaves >= 80.0;
                steep = steep && ok;
                check(ok, "sub: white noise is at least 45 dB down one octave above the crossover and 80 dB down from two octaves above ("
                              + juce::String(hz, 0) + " Hz at " + juce::String(juce::roundToInt(rate / 1000.0)) + " kHz)");
            }
        const auto dry = measure(48000.0, 120.0f, RigEngine::StowDry);
        std::cout << "  the same measurement with Stowaway in Dry (no filter): " << juce::String(dry.oneOctave, 1) << " dB and " << juce::String(dry.twoOctaves, 1) << " dB" << std::endl;
        check(std::abs(dry.oneOctave) < 6.0 && std::abs(dry.twoOctaves) < 6.0, "sub: the measurement reads nothing where there is no filter (the control)");
    }

    //==========================================================================
    std::cout << "Sub: Low End and Drive stay under the safety low-pass" << std::endl;
    {
        auto measure = [&](double rate, float crossoverHz, bool noise, float low, float drive)
        {
            Rig rig;
            const auto file = noise ? writeWav(folder, "stow_noise_" + juce::String(juce::roundToInt(rate)), whiteNoise(8.0, rate, 7), rate)
                                    : writeWav(folder, "stow_lowtone", tone(0.55 * crossoverHz, 8.0, rate), rate);
            rig.hold.loadFile(0, file, error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plainParams();
            p.stow.filter = Params::stowCrossoverKnob(crossoverHz);
            p.stow.low = low;
            p.stow.drive = drive;
            rig.start(p, rate);
            render(rig.engine, 2.0, rate);
            const auto out = render(rig.engine, 24.0, rate);
            const auto sp = spectrum(out.getReadPointer(0), out.getNumSamples(), rate, rate > 100000.0 ? 18 : 16);
            const double fc = crossoverHz;
            return std::pair<double, double> { sp.peak(20.0, fc) - sp.peak(2.5 * fc, 0.5 * rate), sp.peak(20.0, fc) - sp.peak(1.2 * fc, 2.0 * fc) };
        };
        for (const float hz : { 40.0f, 120.0f, 250.0f })
        {
            const auto noisy = measure(48000.0, hz, true, 1.0f, 1.0f);
            const auto toned = measure(48000.0, hz, false, 1.0f, 1.0f);
            const auto clean = measure(48000.0, hz, false, 0.0f, 0.0f);
            std::cout << "  crossover " << hz << " Hz, Low End and Drive at 100%: noise " << juce::String(noisy.first, 1) << " dB down from 2.5 x the crossover, a tone at 0.55 x the crossover "
                      << juce::String(toned.first, 1) << " dB down (its overtones between 1.2 x and 2 x the crossover: " << juce::String(toned.second, 1)
                      << " dB under it, " << juce::String(clean.second, 1) << " dB with both at 0%)" << std::endl;
            check(noisy.first >= 40.0 && toned.first >= 40.0,
                  "sub: with Low End and Drive at 100% nothing from 2.5 x the crossover up is within 40 dB of the passband (" + juce::String(hz, 0) + " Hz)");
            check(toned.second < clean.second - 20.0, "sub: Low End and Drive do add overtones under the safety low-pass (" + juce::String(hz, 0) + " Hz)");
        }
        const auto fast = measure(192000.0, 120.0f, true, 1.0f, 1.0f);
        std::cout << "  at 192 kHz, 120 Hz: " << juce::String(fast.first, 1) << " dB down" << std::endl;
        check(fast.first >= 40.0, "sub: and the same at 192 kHz");
    }

    //==========================================================================
    std::cout << "Sub: what passes" << std::endl;
    {
        // Against the same track through Stowaway in Dry, which passes it on as it is.
        auto run = [&](double hz, float crossoverHz, float width, int mode = RigEngine::StowSub)
        {
            Rig rig;
            rig.hold.loadFile(0, writeWav(folder, "stow_tone_" + juce::String(juce::roundToInt(hz)), tone(hz, 4.0, 48000.0), 48000.0), error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plainParams();
            p.stow.mode = mode;
            p.stow.filter = mode == RigEngine::StowSub ? Params::stowCrossoverKnob(crossoverHz) : 0.5f;
            p.stow.width = width;
            rig.start(p, 48000.0);
            render(rig.engine, 1.0, 48000.0);
            return render(rig.engine, 2.0, 48000.0);
        };
        auto level = [](const juce::AudioBuffer<float>& a) { return rmsDb(a, 0, 0, a.getNumSamples()); };
        const double in = level(run(30.0, 120.0f, 1.0f, RigEngine::StowDry));
        const double d120 = level(run(30.0, 120.0f, 1.0f)) - in, d250 = level(run(30.0, 250.0f, 1.0f)) - in, d40 = level(run(30.0, 40.0f, 1.0f)) - in;
        std::cout << "  a 30 Hz tone: " << juce::String(d120, 2) << " dB with the crossover at 120 Hz, " << juce::String(d250, 2) << " dB at 250 Hz, " << juce::String(d40, 2)
                  << " dB at 40 Hz" << std::endl;
        check(std::abs(d120) < 1.0 && std::abs(d250) < 1.0, "sub: a 30 Hz tone passes within 1 dB (crossover at 120 Hz and at 250 Hz)");
        check(d40 > -3.0 && d40 < 0.0, "sub: with the crossover right down at 40 Hz a 30 Hz tone is still within 3 dB");
        const double high = level(run(1000.0, 120.0f, 1.0f)) - level(run(1000.0, 120.0f, 1.0f, RigEngine::StowDry));
        std::cout << "  a 1 kHz tone: " << juce::String(high, 1) << " dB" << std::endl;
        check(high < -100.0, "sub: a 1 kHz tone does not pass");

        const auto narrow = run(50.0, 120.0f, 0.0f), wide = run(50.0, 120.0f, 1.0f);
        float leftRight = 0.0f, widthMoves = 0.0f, peak = 0.0f;
        for (int i = 0; i < wide.getNumSamples(); ++i)
        {
            leftRight = std::max(leftRight, std::abs(wide.getSample(0, i) - wide.getSample(1, i)));
            widthMoves = std::max(widthMoves, std::abs(wide.getSample(0, i) - narrow.getSample(0, i)));
            peak = std::max(peak, std::abs(wide.getSample(0, i)));
        }
        std::cout << "  a 50 Hz tone: peak " << peak << ", left - right " << leftRight << ", Width 0% against 100% " << widthMoves << std::endl;
        check(peak > 0.1f && leftRight == 0.0f, "sub: left and right are the same signal");
        check(widthMoves == 0.0f, "sub: Width does nothing");
        check(Params::stowText(ParamIDs::StowSub, ParamIDs::StowWidth, 1.0f) == "Mono" && ! Params::stowControlInUse(ParamIDs::StowSub, ParamIDs::StowWidth)
                  && Params::stowControlInUse(ParamIDs::StowWake, ParamIDs::StowWidth),
              "sub: Width reads Mono and is out of use (a panel dims it); in the other modes it is a width again");
        bool othersInUse = true;
        for (const int k : { ParamIDs::StowFollow, ParamIDs::StowLow, ParamIDs::StowFilter, ParamIDs::StowDrive })
            othersInUse = othersInUse && Params::stowControlInUse(ParamIDs::StowSub, k);
        check(othersInUse && Params::stowText(ParamIDs::StowSub, ParamIDs::StowFilter, 0.5f) == "120 Hz", "sub: Follow, Low End, Filter and Drive are in use, and Filter starts at 120 Hz");
    }

    //==========================================================================
    std::cout << "Sub: the filter's delay" << std::endl;
    {
        // The filter as built (48 dB per octave, both the crossover and the safety low-pass), and the same design
        // at 24 dB per octave for comparison: a 20 Hz high-pass, one fourth-order Linkwitz-Riley at the crossover
        // and one at the safety low-pass.
        struct Gentle
        {
            sj::Svf64 hp, a[2], b[2];
            void set(double hz, double rate)
            {
                hp.set(SubFilter::c_highpassHz, 0.70710678, rate);
                for (auto& f : a) f.set(hz, 0.70710678, rate);
                for (auto& f : b) f.set(SubFilter::c_safetyRatio * hz, 0.70710678, rate);
            }
            double run(double x)
            {
                x = hp.highpass(x);
                for (auto& f : a) x = f.lowpass(x);
                for (auto& f : b) x = f.lowpass(x);
                return x;
            }
        };
        double worst = 0.0;
        for (const double hz : { 40.0, 120.0, 250.0 })
        {
            double steep[2] {}, gentle[2] {};
            const double at[2] = { 0.75 * hz, hz };
            for (int i = 0; i < 2; ++i)
            {
                SubFilter f;
                f.set(hz, 48000.0);
                steep[i] = 1000.0 * groupDelay([&](double x) { return f.safety(f.band(x)); }, at[i], 48000.0);
                Gentle g;
                g.set(hz, 48000.0);
                gentle[i] = 1000.0 * groupDelay([&](double x) { return g.run(x); }, at[i], 48000.0);
            }
            worst = std::max(worst, steep[1] * hz / 1000.0);
            std::cout << "  crossover " << hz << " Hz: " << juce::String(steep[0], 1) << " ms at " << at[0] << " Hz and " << juce::String(steep[1], 1) << " ms at the crossover ("
                      << juce::String(steep[1] * hz / 1000.0, 2) << " cycles); at 24 dB per octave " << juce::String(gentle[0], 1) << " ms and " << juce::String(gentle[1], 1) << " ms" << std::endl;
        }
        check(worst < 2.5, "sub: the filter is never more than two and a half cycles late at its crossover");
    }

    //==========================================================================
    std::cout << "Sub: Follow" << std::endl;
    {
        // A steady 50 Hz tone on Stowaway; on bus A (or B) a tone that swells 30 dB every 2 s. The sub's own level
        // is read from its follower after every block.
        const auto subFile = writeWav(folder, "stow_sub50", tone(50.0, 4.0, 48000.0), 48000.0);
        const auto swellFile = writeWav(folder, "stow_swell", swellingTone(440.0, 4.0, 48000.0), 48000.0);
        const auto steadyFile = writeWav(folder, "stow_steady", tone(440.0, 4.0, 48000.0), 48000.0);
        // What Follow does is read against the same run with Follow at 0% and Auto Ride off (the rig repeats itself
        // exactly, so the difference in the sub's level, block by block, is the gain Follow and the ride put on it).
        struct Seen { std::vector<double> level, bus; float rideLow = 1.0f; };
        // other: -1 nothing else, 0 / 1 the swelling tone on bus A / B, 2 a steady tone on bus A, 3 the swelling tone
        // on bus A with its fader down.
        auto run = [&](float follow, int other, bool ride)
        {
            Rig rig;
            rig.hold.loadFile(0, subFile, error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            if (other >= 0)
            {
                rig.hold.loadFile(1, other == 2 ? steadyFile : swellFile, error);
                rig.hold.setSkipSilence(1, false);
                rig.hold.setBusOverride(1, other == 1 ? 1 : 0);
            }
            auto p = plainParams();
            p.stow.follow = follow;
            p.rideOn = ride;
            p.energy = 0.9f;
            p.rate = 0.8f;
            if (other == 3)
                p.levels[1] = 0.0f;
            rig.start(p, 48000.0);
            render(rig.engine, 4.0, 48000.0);
            Seen seen;
            render(rig.engine, 12.0, 48000.0, [&](int)
            {
                seen.level.push_back(20.0 * std::log10(std::max(rig.engine.getStowEnv(), 1.0e-6f)));
                seen.bus.push_back(static_cast<double>(rig.engine.getBusLevel(other == 1 ? 1 : 0)));
                seen.rideLow = std::min(seen.rideLow, rig.engine.getRide(0));
            });
            return seen;
        };
        struct Gain { double range = 0.0, mean = 0.0, top = 0.0, correlation = 0.0; };
        auto against = [](const Seen& seen, const Seen& base)
        {
            std::vector<double> gain(seen.level.size());
            for (size_t i = 0; i < gain.size(); ++i)
                gain[i] = seen.level[i] - base.level[i];
            auto sorted = gain;
            std::sort(sorted.begin(), sorted.end());
            Gain g;
            g.top = sorted[sorted.size() * 98 / 100];
            g.range = g.top - sorted[sorted.size() * 2 / 100];
            double mb = 0.0;
            for (size_t i = 0; i < gain.size(); ++i) { g.mean += gain[i]; mb += seen.bus[i]; }
            g.mean /= static_cast<double>(gain.size());
            mb /= static_cast<double>(gain.size());
            double sgg = 0.0, sbb = 0.0, sgb = 0.0;
            for (size_t i = 0; i < gain.size(); ++i) { sgg += (gain[i] - g.mean) * (gain[i] - g.mean); sbb += (seen.bus[i] - mb) * (seen.bus[i] - mb); sgb += (gain[i] - g.mean) * (seen.bus[i] - mb); }
            g.correlation = sgg > 0.0 && sbb > 0.0 ? sgb / std::sqrt(sgg * sbb) : 0.0;
            return g;
        };
        const auto baseA = run(0.0f, 0, false);
        Gain byA[11];
        for (int i = 0; i <= 10; ++i)
            byA[i] = against(run(static_cast<float>(i) / 10.0f, 0, false), baseA);
        std::cout << "  the sub's rise and fall with a 30 dB swell on bus A, Follow 0% .. 100%:";
        bool even = true;
        for (int i = 0; i <= 10; ++i)
        {
            std::cout << " " << juce::String(byA[i].range, 1);
            if (i > 0)
                even = even && byA[i].range - byA[i - 1].range > 2.0 && byA[i].range - byA[i - 1].range < 4.0;
        }
        std::cout << " dB (it moves with bus A: " << juce::String(byA[10].correlation, 2) << "; its loudest moments " << juce::String(byA[10].top, 1) << " dB up)" << std::endl;
        check(byA[0].range == 0.0, "follow: at 0% the sub is steady");
        check(byA[10].range > 26.0 && byA[10].correlation > 0.8, "follow: at 100% it rises and falls as far as bus A does, with it");
        check(even, "follow: every 10% takes on about as much more of the swell (2 to 4 dB of a 30 dB swell)");
        check(std::abs(byA[10].top - RigEngine::c_subFollowMakeupDb) < 1.0, "follow: its loudest moments rise by the make-up only");

        const auto baseSteady = run(0.0f, 2, false), baseAlone = run(0.0f, -1, false);
        const auto steadySeen = run(1.0f, 2, true), riddenSeen = run(1.0f, -1, true);
        const auto steadyA = against(steadySeen, baseSteady), unfollowed = against(run(0.0f, 2, true), baseSteady), noRide = against(run(0.0f, -1, true), baseAlone), ridden = against(riddenSeen, baseAlone),
                   half = against(run(0.5f, -1, true), baseAlone);
        std::cout << "  with Auto Ride deep: a tone on bus A (ridden), Follow 100%: the sub moves " << juce::String(steadyA.range, 1) << " dB with it (" << juce::String(steadyA.correlation, 2)
                  << "), its own fader's lowest " << steadySeen.rideLow << "; at Follow 0% it moves " << juce::String(unfollowed.range, 1) << " dB; nothing on the buses: " << juce::String(noRide.range, 1) << " dB at 0%, " << juce::String(half.range, 1) << " dB at 50%, " << juce::String(ridden.range, 1)
                  << " dB at 100% (fader's lowest " << riddenSeen.rideLow << ")" << std::endl;
        check(steadySeen.rideLow > 0.99f && unfollowed.range < 0.5 && steadyA.range > 12.0 && steadyA.correlation > 0.8,
              "follow: with a bus to follow, the sub's own tracks are not ridden: it moves with the bus, and at 0% not at all");
        check(noRide.range < 0.5 && ridden.range > 12.0 && half.range > 0.4 * ridden.range && half.range < 0.6 * ridden.range,
              "follow: with no track on bus A or bus B its tracks are ridden instead, as deep as Follow says");
        const auto byB = against(run(1.0f, 1, false), run(0.0f, 1, false)), faderDown = against(run(1.0f, 3, false), run(0.0f, 3, false)), alone = against(run(1.0f, -1, false), baseAlone);
        std::cout << "  following bus B: " << juce::String(byB.range, 1) << " dB (" << juce::String(byB.correlation, 2) << "); bus A's only track faded right down: " << juce::String(faderDown.mean, 1)
                  << " dB against Follow 0%; nothing to follow at all: " << juce::String(alone.mean, 1) << " dB" << std::endl;
        check(byB.range > 26.0 && byB.correlation > 0.8, "follow: with no track on bus A it follows bus B");
        check(faderDown.mean > -1.0 && faderDown.range < 0.5 && alone.mean > -1.0 && alone.range < 0.5,
              "follow: with nothing to follow (no tracks on the buses, or their faders right down) it plays on: never stuck silent");
    }

    //==========================================================================
    std::cout << "Sub: with the rest of the rig" << std::endl;
    {
        // The hits do not duck a sub: a 300 Hz tone through the open hits at a full Hit Duck.
        const auto subFile = writeWav(folder, "stow_sub50", tone(50.0, 4.0, 48000.0), 48000.0);
        auto level = [&](bool hits, int mode)
        {
            Rig rig;
            rig.hold.loadFile(0, subFile, error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            if (hits)
            {
                rig.hold.loadFile(1, writeWav(folder, "stow_hit300", tone(300.0, 4.0, 48000.0), 48000.0), error);
                rig.hold.setSkipSilence(1, false);
                rig.hold.setBusOverride(1, CargoHold::c_hitsBus);
            }
            auto p = plainParams();
            p.hitDuck = 1.0f;
            p.stow.mode = mode;
            rig.start(p, 48000.0);
            render(rig.engine, 1.0, 48000.0);
            rig.engine.takeSectionLevels();
            render(rig.engine, 2.0, 48000.0);
            return rig.engine.takeSectionLevels()[RigEngine::SectionStow];
        };
        const float alone = level(false, RigEngine::StowSub), underHits = level(true, RigEngine::StowSub);
        const float dryAlone = level(false, RigEngine::StowDry), dryUnderHits = level(true, RigEngine::StowDry);
        std::cout << "  under a full Hit Duck: Sub " << underHits << " (" << alone << " with no hits), Dry " << dryUnderHits << " (" << dryAlone << ")" << std::endl;
        check(alone > 0.1f && std::abs(underHits / alone - 1.0f) < 0.01f, "sub: the hits do not duck it");
        check(dryUnderHits < 0.5f * dryAlone, "sub: (they do duck the same track in Dry)");

        // Moving the crossover: a 60 Hz tone while Filter sweeps its whole travel in a second and back. The engine
        // glides the knob; the filters are retuned as it goes. Nothing may jump: the steepest step between two
        // samples stays what a full-level 250 Hz tone could make.
        {
            Rig rig;
            rig.hold.loadFile(0, writeWav(folder, "stow_tone_60", tone(60.0, 4.0, 48000.0), 48000.0), error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plainParams();
            rig.start(p, 48000.0);
            render(rig.engine, 1.0, 48000.0);
            const auto out = render(rig.engine, 4.0, 48000.0, [&](int pos)
            {
                const double t = std::fmod(pos / 48000.0, 2.0);
                p.stow.filter = static_cast<float>(t < 1.0 ? t : 2.0 - t);
                rig.engine.setParams(p);
            });
            float step = 0.0f, peak = 0.0f;
            for (int i = 1; i < out.getNumSamples(); ++i)
            {
                step = std::max(step, std::abs(out.getSample(0, i) - out.getSample(0, i - 1)));
                peak = std::max(peak, std::abs(out.getSample(0, i)));
            }
            const float allowed = 0.6f * static_cast<float>(c_twoPi * 250.0 / 48000.0);
            std::cout << "  sweeping the crossover under a 60 Hz tone: peak " << peak << ", steepest step " << step << " (a 250 Hz tone at that level: " << allowed << ")" << std::endl;
            check(peak > 0.2f && peak < 0.6f && step < allowed, "sub: moving the crossover does not click or ring up");
        }

        // When its tracks stop, the filters ring out and then it leaves the mix (and comes back clean).
        {
            Rig rig;
            rig.hold.loadFile(0, subFile, error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            const auto p = plainParams();
            rig.start(p, 48000.0);
            render(rig.engine, 1.0, 48000.0);
            rig.hold.clear(0);
            const auto after = render(rig.engine, 2.0, 48000.0);
            float tail = 0.0f, late = 0.0f, step = 0.0f;
            for (int i = 1; i < after.getNumSamples(); ++i)
            {
                const float v = std::abs(after.getSample(0, i));
                if (i < 4800) tail = std::max(tail, v);
                if (i > 48000) late = std::max(late, v);
                step = std::max(step, std::abs(after.getSample(0, i) - after.getSample(0, i - 1)));
            }
            std::cout << "  after its track is taken away: " << tail << " in the first 100 ms, " << late << " a second later, steepest step " << step
                      << ", level " << rig.engine.getStowEnv() << std::endl;
            check(tail > 0.01f && late < 1.0e-9f && rig.engine.getStowEnv() == 0.0f && step < 0.05f, "sub: it fades with its track, rings out and leaves the mix without a click");
        }
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : (juce::String(g_failures) + " FAILED")) << std::endl;
    return g_failures == 0 ? 0 : 1;
}

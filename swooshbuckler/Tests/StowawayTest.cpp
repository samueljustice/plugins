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
        check(byA[10].top > 3.0 && byA[10].top < RigEngine::c_subFollowMakeupDb + 0.5, "follow: its loudest moments rise by the make-up only (never more than 6 dB)");

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

    //==========================================================================
    // Wake, Dry and what all the modes share. A 1 kHz tone on Stowaway is the layer; Stowaway's level (the RMS of
    // each block of what it adds to the mix) and what bus A does (its level, and where its two passes are) are read
    // from the engine after every block. The layer is longer than any of the runs (70 s): a sound that loops is
    // crossfaded into its own start, and where the two copies of a steady tone meet in step its level rises 3 dB,
    // which is the file and not the mode.
    const auto layerFile = writeWav(folder, "stow_layer1k", tone(1000.0, 70.0, 48000.0, 0.3f), 48000.0);
    const auto lowLayerFile = writeWav(folder, "stow_layer200", tone(200.0, 4.0, 48000.0, 0.3f), 48000.0);
    const auto busToneFile = writeWav(folder, "stow_bus440", tone(440.0, 4.0, 48000.0, 0.1f), 48000.0);
    struct Seen
    {
        std::vector<float> amount, pan, stowDb, busDb, out, dir; // per block; out / dir: bus A's outgoing pass (how far out, which way)
        float rideLow = 1.0f;
        juce::AudioBuffer<float> audio;
    };
    auto watch = [](Rig& rig, double seconds)
    {
        Seen seen;
        seen.audio = render(rig.engine, seconds, rig.rate, [&](int)
        {
            seen.amount.push_back(rig.engine.getWakeAmount());
            seen.pan.push_back(rig.engine.getStowPan());
            seen.stowDb.push_back(20.0f * std::log10(std::max(rig.engine.takeSectionLevels()[RigEngine::SectionStow], 1.0e-6f)));
            seen.busDb.push_back(20.0f * std::log10(std::max(rig.engine.getBusLevel(0) / 3.0f, 1.0e-6f)));
            const auto a = rig.engine.getFlightView(0), b = rig.engine.getFlightView(1);
            const auto& leaving = a.age > 0.5f ? a : b;
            seen.out.push_back(2.0f * leaving.age - 1.0f);
            seen.dir.push_back(leaving.x >= 0.0f ? 1.0f : -1.0f);
            seen.rideLow = std::min(seen.rideLow, rig.engine.getRide(0));
        });
        return seen;
    };
    auto percentile = [](std::vector<float> v, double p)
    {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0f : v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
    };
    /** The steepest step between two samples of a channel. */
    auto steepest = [](const juce::AudioBuffer<float>& a, int channel = 0)
    {
        float step = 0.0f;
        for (int i = 1; i < a.getNumSamples(); ++i)
            step = std::max(step, std::abs(a.getSample(channel, i) - a.getSample(channel, i - 1)));
        return step;
    };
    auto peakOf = [](const juce::AudioBuffer<float>& a, int channel = 0)
    {
        float peak = 0.0f;
        for (int i = 0; i < a.getNumSamples(); ++i)
            peak = std::max(peak, std::abs(a.getSample(channel, i)));
        return peak;
    };

    std::cout << "Wake: it opens as bus A falls away" << std::endl;
    {
        // onA: a steady tone on bus A. ride: Auto Ride deep (what then makes bus A rise and fall is its fader).
        auto run = [&](float follow, bool onA, bool ride, int passMode, float width, double seconds = 30.0)
        {
            Rig rig;
            rig.hold.loadFile(0, layerFile, error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            if (onA)
            {
                rig.hold.loadFile(1, busToneFile, error);
                rig.hold.setSkipSilence(1, false);
                rig.hold.setBusOverride(1, 0);
            }
            auto p = plainParams();
            p.stow.mode = RigEngine::StowWake;
            p.stow.follow = follow;
            p.stow.width = width;
            p.rideOn = ride;
            p.energy = 0.9f;
            p.rate = 0.8f;
            p.buses[0].passMode = passMode;
            rig.start(p, 48000.0);
            render(rig.engine, 4.0, 48000.0);
            return watch(rig, seconds);
        };

        // A ridden bed: bus A's level is its ride. The trail is open while A is falling and shut while it rises.
        {
            const auto seen = run(1.0f, true, true, 0, 1.0f, 60.0);
            const size_t lag = 19; // about 100 ms of blocks
            double falling = 0.0, rising = 0.0;
            int nFalling = 0, nRising = 0;
            for (size_t i = lag; i < seen.amount.size(); ++i)
            {
                if (seen.busDb[i - lag] < -55.0f)
                    continue;
                const float d = seen.busDb[i] - seen.busDb[i - lag];
                if (d < -1.5f) { falling += seen.amount[i]; ++nFalling; }
                if (d > 1.5f) { rising += seen.amount[i]; ++nRising; }
            }
            falling /= std::max(1, nFalling);
            rising /= std::max(1, nRising);
            std::cout << "  a ridden bed on bus A: the trail is " << juce::String(100.0 * falling, 0) << "% open on average while A falls (" << nFalling << " blocks) and "
                      << juce::String(100.0 * rising, 0) << "% while it rises (" << nRising << "); its own fader's lowest " << seen.rideLow << std::endl;
            check(nFalling > 500 && nRising > 500 && falling > 0.6 && rising < 0.25, "wake: with a ridden bed on bus A it is open while A falls away and shut while A rises");
            check(seen.rideLow > 0.99f, "wake: its own tracks are not ridden (the trail is what moves them)");
        }

        // Nothing on bus A at all: the passes are all there is to go by, and the trail blooms behind each.
        {
            const auto seen = run(1.0f, false, false, 0, 1.0f);
            double sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;
            const double n = static_cast<double>(seen.amount.size());
            for (size_t i = 0; i < seen.amount.size(); ++i)
            {
                const double a = seen.amount[i], b = RigEngine::wakeBloom(seen.out[i]);
                sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
            }
            const double corr = (sab - sa * sb / n) / std::sqrt((saa - sa * sa / n) * (sbb - sb * sb / n));
            const float top = percentile(seen.amount, 0.99), bottom = percentile(seen.amount, 0.01);
            std::cout << "  nothing on bus A: the trail goes from " << bottom << " to " << top << ", with how far out A's outgoing pass is (" << juce::String(corr, 3)
                      << "); Stowaway's level " << juce::String(percentile(seen.stowDb, 0.02), 1) << " .. " << juce::String(percentile(seen.stowDb, 0.98), 1) << " dB" << std::endl;
            check(top > 0.95f && bottom < 0.05f && corr > 0.95, "wake: with no sound on bus A it blooms behind each of A's passes all the same (never stuck silent)");
        }

        // Fly-By: a steady tone on bus A, faders still. Just behind each pass the trail is open; as the next pass
        // comes in it is shut.
        {
            const auto seen = run(1.0f, true, false, 1, 1.0f);
            double behind = 0.0, ahead = 0.0, atPeak = 0.0;
            int nBehind = 0, nAhead = 0, nPeak = 0;
            for (size_t i = 0; i < seen.amount.size(); ++i)
            {
                if (seen.out[i] > 0.1f && seen.out[i] < 0.5f) { behind += seen.amount[i]; ++nBehind; }
                if (seen.out[i] > 0.85f) { ahead += seen.amount[i]; ++nAhead; }
                if (seen.out[i] < 0.02f) { atPeak += seen.amount[i]; ++nPeak; }
            }
            behind /= std::max(1, nBehind);
            ahead /= std::max(1, nAhead);
            atPeak /= std::max(1, nPeak);
            std::cout << "  Fly-By on bus A: the trail is " << juce::String(100.0 * atPeak, 0) << "% open as a pass goes by, " << juce::String(100.0 * behind, 0)
                      << "% over the first half of its way out, " << juce::String(100.0 * ahead, 0) << "% as the next pass arrives" << std::endl;
            check(behind > 0.7 && ahead < 0.2 && atPeak < 0.2, "wake: in Fly-By each pass leaves a trail: shut as the pass goes by, open behind it, shut again before the next");
        }

        // Where it sits: behind A's outgoing pass (the same side of the middle, not as far out), by Width.
        {
            const auto wide = run(1.0f, false, false, 0, 1.0f), half = run(1.0f, false, false, 0, 0.5f), narrow = run(1.0f, false, false, 0, 0.0f);
            int open = 0, sameSide = 0, behind = 0;
            float furthest = 0.0f, furthestHalf = 0.0f, furthestNarrow = 0.0f;
            double leftWhenRight = 0.0, rightWhenRight = 0.0;
            for (size_t i = 0; i < wide.amount.size(); ++i)
            {
                furthest = std::max(furthest, std::abs(wide.pan[i]));
                furthestHalf = std::max(furthestHalf, std::abs(half.pan[i]));
                furthestNarrow = std::max(furthestNarrow, std::abs(narrow.pan[i]));
                if (wide.amount[i] < 0.5f || wide.out[i] < 0.1f)
                    continue;
                ++open;
                sameSide += wide.pan[i] * wide.dir[i] > 0.0f ? 1 : 0;
                behind += std::abs(wide.pan[i]) < std::pow(wide.out[i], 0.6f) ? 1 : 0;
                if (wide.pan[i] > 0.2f)
                    for (int s = static_cast<int>(i) * c_block; s < static_cast<int>(i + 1) * c_block; ++s)
                    {
                        leftWhenRight += static_cast<double>(wide.audio.getSample(0, s)) * wide.audio.getSample(0, s);
                        rightWhenRight += static_cast<double>(wide.audio.getSample(1, s)) * wide.audio.getSample(1, s);
                    }
            }
            float narrowDiff = 0.0f;
            for (int s = 0; s < narrow.audio.getNumSamples(); ++s)
                narrowDiff = std::max(narrowDiff, std::abs(narrow.audio.getSample(0, s) - narrow.audio.getSample(1, s)));
            const double tilt = 10.0 * std::log10(std::max(rightWhenRight, 1.0e-20) / std::max(leftWhenRight, 1.0e-20));
            std::cout << "  its place while open (" << open << " blocks): on the side A's outgoing pass is heading " << sameSide << ", nearer the middle than the pass " << behind
                      << "; furthest out " << furthest << " at Width 100%, " << furthestHalf << " at 50%, " << furthestNarrow << " at 0% (left - right " << narrowDiff
                      << "); right over left while it sits to the right: " << juce::String(tilt, 1) << " dB" << std::endl;
            check(open > 500 && sameSide > open * 95 / 100 && behind > open * 95 / 100, "wake: it sits behind bus A's outgoing pass (the same side, nearer the middle)");
            check(furthest > 0.4f && furthest < 0.75f && furthestHalf > 0.45f * furthest && furthestHalf < 0.7f * furthest && furthestNarrow == 0.0f && narrowDiff == 0.0f && tilt > 2.0,
                  "wake: Width is how far out it goes (0% is the middle: left and right the same)");
        }

        // Follow: how far it shuts between trails, evenly in dB, and about as loud on average wherever it is.
        {
            auto meanDb = [](const Seen& seen)
            {
                double sum = 0.0;
                for (const float db : seen.stowDb)
                    sum += std::pow(10.0, db / 10.0);
                return 10.0 * std::log10(sum / static_cast<double>(seen.stowDb.size()));
            };
            // Nothing on bus A (the trail is the passes' alone, so each run moves the same), then a ridden bed.
            for (const bool bed : { false, true })
            {
                const auto base = run(0.0f, bed, bed, 0, 1.0f, 40.0);
                const float steady = percentile(base.stowDb, 0.98) - percentile(base.stowDb, 0.02);
                std::cout << "  " << (bed ? "a ridden bed on bus A" : "nothing on bus A") << ", Follow 0%: its level moves " << juce::String(steady, 2) << " dB. Follow 10% .. 100%, how far it shuts between trails:";
                bool even = true, level = true;
                float last = 0.0f, top = 0.0f;
                juce::String means;
                for (int i = 1; i <= 10; ++i)
                {
                    const auto seen = run(static_cast<float>(i) / 10.0f, bed, bed, 0, 1.0f, 40.0);
                    std::vector<float> gain(seen.stowDb.size());
                    for (size_t k = 0; k < gain.size(); ++k)
                        gain[k] = seen.stowDb[k] - base.stowDb[k];
                    const float range = percentile(gain, 0.98) - percentile(gain, 0.05);
                    top = std::max(top, percentile(gain, 0.995));
                    std::cout << " " << juce::String(range, 1);
                    even = even && range - last > 2.0f && range - last < 7.5f;
                    last = range;
                    const double mean = meanDb(seen) - meanDb(base);
                    means << " " << juce::String(mean, 1);
                    level = level && std::abs(mean) < 1.5;
                }
                std::cout << " dB; its average level against 0%:" << means << " dB; its loudest moments " << juce::String(top, 1) << " dB up" << std::endl;
                check(steady < 0.5f, juce::String("wake follow: at 0% it is a steady layer") + (bed ? " (a ridden bed on bus A)" : ""));
                check(even && last > 35.0f, juce::String("wake follow: every 10% shuts it about as much further between trails (2 to 7.5 dB), down to silence at 100%") + (bed ? " (a ridden bed on bus A)" : ""));
                check(level && top < RigEngine::c_wakeFollowMakeupDb + 0.5f, juce::String("wake follow: on average it is as loud wherever the knob is (within 1.5 dB), and never more than 12 dB up") + (bed ? " (a ridden bed on bus A)" : ""));
            }
        }
    }

    //==========================================================================
    std::cout << "Wake and Dry: Low End, Filter and Drive" << std::endl;
    {
        const auto noiseFile = writeWav(folder, "stow_noise_48000", whiteNoise(8.0, 48000.0, 7), 48000.0);
        auto run = [&](int mode, const juce::File& file, float low, float filter, float drive)
        {
            Rig rig;
            rig.hold.loadFile(0, file, error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plainParams();
            p.stow.mode = mode;
            p.stow.low = low;
            p.stow.filter = filter;
            p.stow.drive = drive;
            rig.start(p, 48000.0);
            render(rig.engine, 2.0, 48000.0);
            return render(rig.engine, 12.0, 48000.0);
        };
        auto spec = [&](const juce::AudioBuffer<float>& a) { return spectrum(a.getReadPointer(0), a.getNumSamples(), 48000.0, 14); };
        for (const int mode : { RigEngine::StowWake, RigEngine::StowDry })
        {
            const juce::String name = ParamIDs::stowModeNames[mode];
            const auto open = spec(run(mode, noiseFile, 0.0f, 0.5f, 0.0f)), dark = spec(run(mode, noiseFile, 0.0f, 0.15f, 0.0f)), thin = spec(run(mode, noiseFile, 0.0f, 0.85f, 0.0f));
            const double lpHz = Params::stowLowpassHz(0.15f), hpHz = Params::stowHighpassHz(0.85f);
            const double flat = open.peak(100.0, 200.0) - open.peak(8000.0, 12000.0);
            const double darkTop = open.peak(4.0 * lpHz, 6.0 * lpHz) - dark.peak(4.0 * lpHz, 6.0 * lpHz), darkLow = open.peak(0.2 * lpHz, 0.4 * lpHz) - dark.peak(0.2 * lpHz, 0.4 * lpHz);
            const double thinLow = open.peak(hpHz / 6.0, hpHz / 4.0) - thin.peak(hpHz / 6.0, hpHz / 4.0), thinTop = open.peak(3.0 * hpHz, 5.0 * hpHz) - thin.peak(3.0 * hpHz, 5.0 * hpHz);
            std::cout << "  " << name << ", white noise: Filter in the middle, 150 Hz against 10 kHz " << juce::String(flat, 1) << " dB; at 15% (" << Params::stowText(mode, ParamIDs::StowFilter, 0.15f) << ") "
                      << juce::String(darkTop, 1) << " dB off two octaves above and " << juce::String(darkLow, 1) << " dB off under it; at 85% (" << Params::stowText(mode, ParamIDs::StowFilter, 0.85f) << ") "
                      << juce::String(thinLow, 1) << " dB off two octaves under and " << juce::String(thinTop, 1) << " dB off above it" << std::endl;
            check(std::abs(flat) < 3.0, name + ": Filter in the middle is open");
            check(darkTop > 18.0 && std::abs(darkLow) < 2.0, name + ": below the middle Filter is a low-pass (12 dB per octave)");
            check(thinLow > 18.0 && std::abs(thinTop) < 2.0, name + ": above the middle it is a high-pass");
            check(Params::stowText(mode, ParamIDs::StowFilter, 0.5f) == "Open" && Params::stowText(mode, ParamIDs::StowFilter, 0.15f).startsWith("Under ")
                      && Params::stowText(mode, ParamIDs::StowFilter, 0.85f).startsWith("Over ") && Params::stowControlInUse(mode, ParamIDs::StowFilter),
                  name + ": Filter reads Open in the middle, and what it lets through either side");

            // Sweeping Filter over its whole travel (through the middle, where each side comes in and goes out) under
            // a 200 Hz tone: nothing steps.
            Rig rig;
            rig.hold.loadFile(0, lowLayerFile, error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plainParams();
            p.stow.mode = mode;
            rig.start(p, 48000.0);
            render(rig.engine, 1.0, 48000.0);
            const auto swept = render(rig.engine, 4.0, 48000.0, [&](int pos)
            {
                const double t = std::fmod(pos / 48000.0, 2.0);
                p.stow.filter = static_cast<float>(t < 1.0 ? t : 2.0 - t);
                rig.engine.setParams(p);
            });
            const float allowed = 1.5f * 0.3f * static_cast<float>(c_twoPi * 200.0 / 48000.0);
            std::cout << "  " << name << ", sweeping Filter under a 200 Hz tone: steepest step " << steepest(swept) << " (the tone's own: " << allowed / 1.5f << ")" << std::endl;
            check(steepest(swept) < allowed, name + ": moving Filter through its whole travel does not click");
        }

        // Low End and Drive: in Wake they colour the trail; Dry skips them.
        const auto lowTone = writeWav(folder, "stow_tone_100", tone(100.0, 4.0, 48000.0, 0.3f), 48000.0);
        for (const int mode : { RigEngine::StowWake, RigEngine::StowDry })
        {
            const juce::String name = ParamIDs::stowModeNames[mode];
            const auto plainOut = run(mode, lowTone, 0.0f, 0.5f, 0.0f), lowOut = run(mode, lowTone, 1.0f, 0.5f, 0.0f), drivenOut = run(mode, lowTone, 0.0f, 0.5f, 1.0f);
            const auto plainSpec = spec(plainOut), low = spec(lowOut), driven = spec(drivenOut);
            const double octaveDown = low.peak(40.0, 60.0) - plainSpec.peak(40.0, 60.0), third = driven.peak(280.0, 320.0) - plainSpec.peak(280.0, 320.0);
            const double level = rmsDb(drivenOut, 0, 0, drivenOut.getNumSamples()) - rmsDb(plainOut, 0, 0, plainOut.getNumSamples());
            std::cout << "  " << name << ", a 100 Hz tone: Low End at 100% adds " << juce::String(octaveDown, 1) << " dB at 50 Hz; Drive at 100% adds " << juce::String(third, 1)
                      << " dB at 300 Hz and changes the level by " << juce::String(level, 2) << " dB" << std::endl;
            if (mode == RigEngine::StowWake)
            {
                check(octaveDown > 30.0, "Wake: Low End adds the octave under the low band");
                check(third > 30.0 && std::abs(level) < 3.0, "Wake: Drive adds overtones and holds its level (within 3 dB: a squared-off tone at the same peak)");
                check(Params::stowControlInUse(mode, ParamIDs::StowLow) && Params::stowControlInUse(mode, ParamIDs::StowDrive) && Params::stowControlInUse(mode, ParamIDs::StowWidth)
                          && Params::stowControlInUse(mode, ParamIDs::StowFollow),
                      "Wake: all five knobs are in use");
            }
            else
            {
                check(std::abs(octaveDown) < 0.5 && std::abs(third) < 0.5 && std::abs(level) < 0.01, "Dry: Low End and Drive do nothing");
                check(! Params::stowControlInUse(mode, ParamIDs::StowLow) && ! Params::stowControlInUse(mode, ParamIDs::StowDrive) && Params::stowControlInUse(mode, ParamIDs::StowWidth)
                          && Params::stowControlInUse(mode, ParamIDs::StowFollow),
                      "Dry: Low End and Drive are out of use (a panel dims them); Follow, Filter and Width are in use");
            }
        }
    }

    //==========================================================================
    std::cout << "Dry: riding only" << std::endl;
    {
        // A short burst every 4 s on Stowaway, Fill at 100%: in Dry there are no echoes after it (Fly, which passes
        // the tracks on with their echoes, is the control).
        juce::AudioBuffer<float> burst(1, 4 * 48000);
        burst.clear();
        for (int i = 0; i < 4800; ++i)
            burst.setSample(0, i, 0.4f * static_cast<float>(std::sin(c_twoPi * 300.0 * i / 48000.0) * std::sin(juce::MathConstants<double>::pi * i / 4800.0)));
        const auto burstFile = writeWav(folder, "stow_burst", burst, 48000.0);
        auto echoes = [&](int mode)
        {
            Rig rig;
            rig.hold.loadFile(0, burstFile, error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plainParams();
            p.stow.mode = mode;
            p.fill = 1.0f;
            rig.start(p, 48000.0);
            const auto out = render(rig.engine, 4.0, 48000.0);
            // (Up to 3 s: after that the file is on its way round to the burst again.)
            return std::pair<double, double> { rmsDb(out, 0, 0, 4800), rmsDb(out, 0, 19200, 3 * 48000) };
        };
        const auto dryEchoes = echoes(RigEngine::StowDry), flyEchoes = echoes(RigEngine::StowFly);
        std::cout << "  a burst with Fill at 100%: after it, Dry " << juce::String(dryEchoes.second, 1) << " dB (the burst: " << juce::String(dryEchoes.first, 1) << " dB), Fly "
                  << juce::String(flyEchoes.second, 1) << " dB" << std::endl;
        check(dryEchoes.first > -30.0 && dryEchoes.second < -120.0 && flyEchoes.second > -60.0, "dry: no Fill echoes");

        // Space and the Everything tremolo do not touch it: the output is the same with them as without, sample
        // for sample (in Fly they change it: the control).
        auto through = [&](int mode, float space, bool tremolo)
        {
            Rig rig;
            rig.hold.loadFile(0, layerFile, error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plainParams();
            p.stow.mode = mode;
            p.space = space;
            p.tremOn = tremolo;
            p.tremDepth = 1.0f;
            p.tremThreshold = 0.0f;
            p.tremTarget = 0;
            rig.start(p, 48000.0);
            return render(rig.engine, 3.0, 48000.0);
        };
        auto differs = [](const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            float d = 0.0f;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    d = std::max(d, std::abs(a.getSample(c, i) - b.getSample(c, i)));
            return d;
        };
        const auto dryPlain = through(RigEngine::StowDry, 0.0f, false);
        const float drySpace = differs(through(RigEngine::StowDry, 1.0f, false), dryPlain), dryTrem = differs(through(RigEngine::StowDry, 0.0f, true), dryPlain);
        const auto flyPlain = through(RigEngine::StowFly, 0.0f, false);
        const float flySpace = differs(through(RigEngine::StowFly, 1.0f, false), flyPlain), flyTrem = differs(through(RigEngine::StowFly, 0.0f, true), flyPlain);
        std::cout << "  Space at 100% changes the output by " << drySpace << " in Dry (" << flySpace << " in Fly); the Everything tremolo by " << dryTrem << " (" << flyTrem << " in Fly); peak "
                  << peakOf(dryPlain) << std::endl;
        check(peakOf(dryPlain) > 0.1f && drySpace == 0.0f && flySpace > 0.01f, "dry: it skips Space");
        check(dryTrem == 0.0f && flyTrem > 0.01f, "dry: it skips the Everything tremolo");

        // Follow: how much of the ride its tracks keep.
        auto ridden = [&](float follow, bool ride)
        {
            Rig rig;
            rig.hold.loadFile(0, layerFile, error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plainParams();
            p.stow.mode = RigEngine::StowDry;
            p.stow.follow = follow;
            p.rideOn = ride;
            p.energy = 0.9f;
            p.rate = 0.8f;
            rig.start(p, 48000.0);
            render(rig.engine, 4.0, 48000.0);
            return watch(rig, 30.0);
        };
        auto moves = [&](const Seen& seen) { return percentile(seen.stowDb, 0.98) - percentile(seen.stowDb, 0.02); };
        auto mean = [](const Seen& seen)
        {
            double sum = 0.0;
            for (const float db : seen.stowDb)
                sum += std::pow(10.0, db / 10.0);
            return 10.0 * std::log10(sum / static_cast<double>(seen.stowDb.size()));
        };
        const auto still = ridden(1.0f, false), none = ridden(0.0f, true), halfRide = ridden(0.5f, true), full = ridden(1.0f, true);
        std::cout << "  with Auto Ride deep its level moves " << juce::String(moves(none), 1) << " dB at Follow 0%, " << juce::String(moves(halfRide), 1) << " dB at 50%, " << juce::String(moves(full), 1)
                  << " dB at 100% (faders still: " << juce::String(moves(still), 1) << " dB); on average " << juce::String(mean(none) - mean(still), 1) << ", " << juce::String(mean(halfRide) - mean(still), 1) << ", "
                  << juce::String(mean(full) - mean(still), 1) << " dB against still faders" << std::endl;
        check(moves(none) < 0.5f && moves(full) > 12.0f && moves(halfRide) > 0.4f * moves(full) && moves(halfRide) < 0.6f * moves(full), "dry follow: how much of the ride its tracks keep (0% none, 100% all)");
        check(std::abs(mean(none) - mean(still)) < 0.5 && std::abs(mean(halfRide) - mean(still)) < 2.0 && std::abs(mean(full) - mean(still)) < 2.0,
              "dry follow: on average it stays about as loud wherever the knob is");

        // A stereo sound keeps its sides, by Width: 300 Hz on the left only, 500 Hz on the right only.
        {
            juce::AudioBuffer<float> stereo(2, 4 * 48000);
            for (int i = 0; i < stereo.getNumSamples(); ++i)
            {
                stereo.setSample(0, i, 0.3f * static_cast<float>(std::sin(c_twoPi * 300.0 * i / 48000.0)));
                stereo.setSample(1, i, 0.3f * static_cast<float>(std::sin(c_twoPi * 500.0 * i / 48000.0)));
            }
            const auto file = folder.getChildFile("stow_stereo.wav");
            file.deleteFile();
            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(new juce::FileOutputStream(file), 48000.0, 2, 24, {}, 0));
                writer->writeFromAudioSampleBuffer(stereo, 0, stereo.getNumSamples());
            }
            // How much of the right-hand tone (500 Hz) is in the left channel, against the left-hand tone there (dB).
            auto crosstalk = [&](int mode, float width, bool hits = false)
            {
                Rig rig;
                rig.hold.loadFile(0, file, error);
                rig.hold.setSkipSilence(0, false);
                rig.hold.setBusOverride(0, CargoHold::c_stowBus);
                if (hits)
                {
                    rig.hold.loadFile(1, writeWav(folder, "stow_hit300", tone(300.0, 4.0, 48000.0), 48000.0), error);
                    rig.hold.setSkipSilence(1, false);
                    rig.hold.setBusOverride(1, CargoHold::c_hitsBus);
                    rig.hold.setBusOverride(0, CargoHold::c_stowBus);
                }
                auto p = plainParams();
                p.stow.mode = mode;
                p.stow.width = width;
                p.stow.filter = mode == RigEngine::StowSub ? 1.0f : 0.5f;
                rig.start(p, 48000.0);
                render(rig.engine, 1.0, 48000.0);
                const auto out = render(rig.engine, 4.0, 48000.0);
                const auto left = spectrum(out.getReadPointer(0), out.getNumSamples(), 48000.0, 14);
                return left.peak(480.0, 520.0) - left.peak(280.0, 320.0);
            };
            const double wide = crosstalk(RigEngine::StowDry, 1.0f), half = crosstalk(RigEngine::StowDry, 0.5f), mono = crosstalk(RigEngine::StowDry, 0.0f), wake = crosstalk(RigEngine::StowWake, 1.0f);
            std::cout << "  a stereo sound (300 Hz left, 500 Hz right): the right-hand tone in the left channel, against the left-hand one: Width 100% " << juce::String(wide, 1) << " dB, 50% "
                      << juce::String(half, 1) << " dB, 0% " << juce::String(mono, 1) << " dB; in Wake " << juce::String(wake, 1) << " dB" << std::endl;
            check(wide < -60.0, "dry: at Width 100% a stereo sound is as wide as its file");
            check(std::abs(half - 20.0 * std::log10(1.0 / 3.0)) < 1.0 && std::abs(mono) < 0.5, "dry: Width narrows it evenly, down to mono at 0%");
            check(std::abs(wake) < 0.5, "dry: (the other modes play the mono sum)");

            // The stereo sound stops being stereo on Stowaway while the host asks for blocks of every odd length:
            // it leaves for bus A (where it plays its mono sum in the middle), or its VARI is turned off (the mono
            // sum again). Once that has faded across, nothing of its sides is left behind, there or on bus B (what is left 40 ms on is
            // the output stage's filters settling, far under a thousandth).
            auto leftBehind = [&](bool byVari, float& with, float& without)
            {
                Rig rig;
                rig.hold.loadFile(0, file, error);
                rig.hold.setSkipSilence(0, false);
                rig.hold.setBusOverride(0, CargoHold::c_stowBus);
                // (A mono sound stays on Stowaway throughout, so Dry goes on playing.)
                rig.hold.loadFile(1, writeWav(folder, "stow_mono200", tone(200.0, 30.0, 48000.0, 0.2f), 48000.0), error);
                rig.hold.setSkipSilence(1, false);
                rig.hold.setBusOverride(1, CargoHold::c_stowBus);
                auto p = plainParams();
                p.stow.mode = RigEngine::StowDry;
                p.stow.filter = 0.5f;
                p.buses[1].width = 1.0f;
                rig.start(p, 48000.0);
                const int blocks[] = { 48, 37, 53, 40, 19, 61, 45 };
                with = without = 0.0f;
                int at = 0;
                for (int round = 0; round < 12; ++round)
                {
                    const bool stereo = round % 2 == 0;
                    if (byVari)
                    {
                        p.trackVari[0] = stereo;
                        rig.engine.setParams(p);
                    }
                    else
                        rig.hold.setBusOverride(0, stereo ? CargoHold::c_stowBus : 0);
                    juce::AudioBuffer<float> out(2, 19200);
                    for (int pos = 0; pos < out.getNumSamples();)
                    {
                        const int n = std::min(blocks[at++ % 7], out.getNumSamples() - pos);
                        rig.engine.process(nullptr, nullptr, out.getWritePointer(0, pos), out.getWritePointer(1, pos), n);
                        pos += n;
                    }
                    // From 40 ms after the change to the end of the round.
                    auto& worst = stereo ? with : without;
                    for (int i = 1920; i < out.getNumSamples(); ++i)
                        worst = std::max(worst, std::abs(out.getSample(0, i) - out.getSample(1, i)));
                }
            };
            float movedWith = 0.0f, movedWithout = 0.0f, variWith = 0.0f, variWithout = 0.0f;
            leftBehind(false, movedWith, movedWithout);
            leftBehind(true, variWith, variWithout);
            std::cout << "  a stereo sound in odd block lengths, left against right: " << movedWith << " on Stowaway and " << movedWithout << " once it has left for bus A; " << variWith << " with VARI on and "
                      << variWithout << " with it off" << std::endl;
            check(movedWith > 0.05f && movedWithout < 1.0e-3f, "dry: nothing of a stereo sound's sides is left behind when it leaves for another bus");
            check(variWith > 0.05f && variWithout < 1.0e-3f, "dry: nothing of them is left behind when its VARI is turned off");
        }
    }

    //==========================================================================
    std::cout << "Every mode: hits, throws, changes of mode and of place" << std::endl;
    {
        // The hits duck Wake, Fly and Dry (not Sub: see above). A full Hit Duck under a steady hit.
        for (const int mode : { RigEngine::StowWake, RigEngine::StowFly, RigEngine::StowDry })
        {
            auto level = [&](bool hits)
            {
                Rig rig;
                rig.hold.loadFile(0, layerFile, error);
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
            const float alone = level(false), under = level(true);
            std::cout << "  " << ParamIDs::stowModeNames[mode] << " under a full Hit Duck: " << under << " (" << alone << " with no hits)" << std::endl;
            check(alone > 0.1f && under < 0.5f * alone, juce::String(ParamIDs::stowModeNames[mode]) + ": the hits duck it");
        }

        // A throw includes Stowaway in every mode, from rest too: the rig is stopped, a tone on bus A and the layer
        // on Stowaway, one throw of 2 s. And with nothing on bus A at all.
        for (const bool onA : { true, false })
            for (int mode = 0; mode < RigEngine::c_stowModes; ++mode)
            {
                Rig rig;
                rig.hold.loadFile(0, mode == RigEngine::StowSub ? writeWav(folder, "stow_sub50", tone(50.0, 4.0, 48000.0), 48000.0) : layerFile, error);
                rig.hold.setSkipSilence(0, false);
                rig.hold.setBusOverride(0, CargoHold::c_stowBus);
                if (onA)
                {
                    rig.hold.loadFile(1, busToneFile, error);
                    rig.hold.setSkipSilence(1, false);
                    rig.hold.setBusOverride(1, 0);
                }
                auto p = plainParams();
                p.stow.mode = mode;
                p.stow.follow = 0.7f;
                p.throwSeconds = 2.0f;
                rig.start(p, 48000.0);
                rig.engine.setReleased(true);
                render(rig.engine, 1.0, 48000.0);
                rig.engine.takeSectionLevels();
                render(rig.engine, 0.5, 48000.0);
                const float before = rig.engine.takeSectionLevels()[RigEngine::SectionStow];
                rig.engine.throwWhoosh(0, 1.0f);
                float loudest = 0.0f;
                render(rig.engine, 2.2, 48000.0, [&](int) { loudest = std::max(loudest, rig.engine.getStowEnv()); });
                rig.engine.takeSectionLevels();
                render(rig.engine, 1.5, 48000.0);
                const float after = rig.engine.takeSectionLevels()[RigEngine::SectionStow];
                std::cout << "  a throw from rest, " << ParamIDs::stowModeNames[mode] << (onA ? " (a tone on bus A)" : " (nothing on bus A)") << ": Stowaway " << before << " before, up to " << loudest
                          << " during, " << after << " after" << std::endl;
                check(before < 1.0e-6f && loudest > 0.05f && after < 1.0e-3f,
                      juce::String(ParamIDs::stowModeNames[mode]) + ": a throw from rest is heard on Stowaway, and gone with its tail" + (onA ? "" : " (with nothing on bus A too)"));
            }

        // Changing the Mode: a 60 Hz tone (which every mode passes with Filter in the middle: under the sub's
        // crossover, and open otherwise) at steady level, every change of mode there is, one every half second. The sound fades out, the mode swaps, and it fades back in: about 30 ms each way.
        {
            const auto file = writeWav(folder, "stow_tone_60b", tone(60.0, 20.0, 48000.0, 0.3f), 48000.0); // (longer than the run: see the layer)
            Rig rig;
            rig.hold.loadFile(0, file, error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plainParams();
            rig.start(p, 48000.0);
            render(rig.engine, 1.0, 48000.0);
            const float allowed = 2.0f * 0.3f * static_cast<float>(c_twoPi * 60.0 / 48000.0);
            // The tone's peak in the output with nothing changing (every mode passes it alike, with Follow at 0%).
            const float steadyPeak = peakOf(render(rig.engine, 0.5, 48000.0));
            float worst = 0.0f, slowest = 0.0f, fastest = 1.0f, deepest = 1.0f, back = 0.0f;
            int changes = 0;
            for (int from = 0; from < RigEngine::c_stowModes; ++from)
                for (int to = 0; to < RigEngine::c_stowModes; ++to)
                {
                    if (from == to)
                        continue;
                    p.stow.mode = from;
                    rig.engine.setParams(p);
                    render(rig.engine, 0.5, 48000.0);
                    p.stow.mode = to;
                    rig.engine.setParams(p);
                    float swappedAt = -1.0f, low = 1.0f;
                    const auto out = render(rig.engine, 0.5, 48000.0, [&](int pos)
                    {
                        if (swappedAt < 0.0f && rig.engine.getStowNow().mode == to)
                            swappedAt = static_cast<float>(pos + c_block) / 48000.0f;
                    });
                    for (int i = 480; i + 480 < 4800; i += 48) // the quietest 17 ms (one cycle of the tone) of the first 100 ms
                    {
                        float m = 0.0f;
                        for (int k = i - 400; k < i + 400; ++k)
                            m = std::max(m, std::abs(out.getSample(0, k)));
                        low = std::min(low, m);
                    }
                    // (Both channels: Wake sits to one side or the other.)
                    juce::AudioBuffer<float> tail(1, 9600);
                    for (int k = 0; k < 9600; ++k)
                        tail.setSample(0, k, std::sqrt(0.5f * (out.getSample(0, 14400 + k) * out.getSample(0, 14400 + k) + out.getSample(1, 14400 + k) * out.getSample(1, 14400 + k))));
                    worst = std::max(worst, steepest(out));
                    slowest = std::max(slowest, swappedAt);
                    fastest = std::min(fastest, swappedAt);
                    deepest = std::min(deepest, peakOf(tail));
                    back = std::max(back, low);
                    ++changes;
                }
            std::cout << "  " << changes << " changes of mode under a 60 Hz tone: steepest step " << worst << " (twice the tone's own: " << allowed << "); the new mode is in use "
                      << juce::String(1000.0f * fastest, 0) << " to " << juce::String(1000.0f * slowest, 0) << " ms after it is asked for; the loudest the gap between gets: " << back
                      << "; 300 ms later the quietest any mode is: " << deepest << " (the tone with nothing changing: " << steadyPeak << ")" << std::endl;
            check(changes == 12 && worst < allowed, "modes: changing the Mode does not click (any mode to any other)");
            check(fastest > 0.02f && slowest < 0.045f && back < 0.3f * steadyPeak && deepest > 0.9f * steadyPeak, "modes: the sound fades out in about 30 ms, the mode swaps at silence, and it fades back in");
        }
        // Under automation: the Mode moved every block (5 ms), round and round, then left alone.
        {
            Rig rig;
            rig.hold.loadFile(0, writeWav(folder, "stow_tone_60b", tone(60.0, 20.0, 48000.0, 0.3f), 48000.0), error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plainParams();
            rig.start(p, 48000.0);
            render(rig.engine, 1.0, 48000.0);
            const float steadyPeak = peakOf(render(rig.engine, 0.5, 48000.0)); // the tone before anything changes
            int n = 0;
            const auto busy = render(rig.engine, 2.0, 48000.0, [&](int)
            {
                p.stow.mode = (++n / 3) % RigEngine::c_stowModes;
                rig.engine.setParams(p);
            });
            p.stow.mode = RigEngine::StowDry;
            rig.engine.setParams(p);
            const auto settled = render(rig.engine, 0.5, 48000.0);
            juce::AudioBuffer<float> end(1, 4800);
            end.copyFrom(0, 0, settled, 0, settled.getNumSamples() - 4800, 4800);
            const float allowed = 2.0f * 0.3f * static_cast<float>(c_twoPi * 60.0 / 48000.0);
            std::cout << "  the Mode automated (a new one every 16 ms for 2 s): steepest step " << std::max(steepest(busy), steepest(settled)) << " (allowed " << allowed << "); then left in Dry: peak "
                      << peakOf(end) << " (before: " << steadyPeak << ")" << std::endl;
            check(std::max(steepest(busy), steepest(settled)) < allowed && rig.engine.getStowNow().mode == RigEngine::StowDry && steadyPeak > 0.1f && std::abs(peakOf(end) - steadyPeak) < 0.05f * steadyPeak, "modes: automating the Mode does not click either, and it settles in the last one asked for");
        }

        // A track changing place: by hand (Stowaway to bus A and back), and a rumble as Stowaway stops being a sub.
        {
            Rig rig;
            rig.hold.loadFile(0, writeWav(folder, "stow_tone_100", tone(100.0, 4.0, 48000.0, 0.3f), 48000.0), error);
            rig.hold.setSkipSilence(0, false);
            rig.hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plainParams();
            p.stow.mode = RigEngine::StowDry;
            rig.start(p, 48000.0);
            render(rig.engine, 1.0, 48000.0);
            float worst = 0.0f;
            for (const int place : { 0, CargoHold::c_stowBus, 1, CargoHold::c_stowBus, CargoHold::c_hitsBus, CargoHold::c_stowBus })
            {
                rig.hold.setBusOverride(0, place);
                worst = std::max(worst, steepest(render(rig.engine, 0.5, 48000.0)));
            }
            rig.engine.takeSectionLevels();
            render(rig.engine, 0.5, 48000.0);
            const auto there = rig.engine.takeSectionLevels();
            // A rumble on Auto: on Stowaway while it is a sub, on bus A otherwise (the processor tells the hold as
            // the Mode moves).
            rig.hold.setBusOverride(0, -1);
            rig.hold.setRoleOverride(0, static_cast<int>(CargoRole::Rumble));
            p.stow.mode = RigEngine::StowSub;
            p.stow.filter = 1.0f;
            rig.hold.setRumbleToStowaway(true);
            rig.engine.setParams(p);
            render(rig.engine, 0.5, 48000.0);
            rig.engine.takeSectionLevels();
            float rumbleWorst = steepest(render(rig.engine, 0.5, 48000.0));
            const auto asSub = rig.engine.takeSectionLevels();
            p.stow.mode = RigEngine::StowWake;
            rig.hold.setRumbleToStowaway(false);
            rig.engine.setParams(p);
            rumbleWorst = std::max(rumbleWorst, steepest(render(rig.engine, 0.5, 48000.0)));
            rig.engine.takeSectionLevels();
            render(rig.engine, 0.5, 48000.0);
            const auto asWake = rig.engine.takeSectionLevels();
            p.stow.mode = RigEngine::StowSub;
            rig.hold.setRumbleToStowaway(true);
            rig.engine.setParams(p);
            rumbleWorst = std::max(rumbleWorst, steepest(render(rig.engine, 0.5, 48000.0)));
            const float allowed = 2.0f * 0.3f * static_cast<float>(c_twoPi * 100.0 / 48000.0);
            std::cout << "  a track moved by hand between Stowaway, bus A, bus B and the hits: steepest step " << worst << " (allowed " << allowed << "); it ends on Stowaway (" << there[RigEngine::SectionStow]
                      << "). A rumble on Auto: Stowaway / bus A " << asSub[RigEngine::SectionStow] << " / " << asSub[RigEngine::SectionBusA] << " in Sub, " << asWake[RigEngine::SectionStow] << " / "
                      << asWake[RigEngine::SectionBusA] << " in Wake; steepest step through the changes " << rumbleWorst << std::endl;
            check(worst < allowed && there[RigEngine::SectionStow] > 0.1f, "places: a track sent somewhere else is faded across (no click)");
            check(asSub[RigEngine::SectionStow] > 0.1f && asSub[RigEngine::SectionBusA] < 1.0e-5f && asWake[RigEngine::SectionStow] < 1.0e-5f && asWake[RigEngine::SectionBusA] > 0.1f && rumbleWorst < allowed,
                  "places: a rumble leaves Stowaway for bus A as the Mode leaves Sub, and comes back, without a click");
        }

        // The modes are about as loud as each other: the same low drone (45 and 72 Hz, under the sub's crossover) on
        // Stowaway, gappy bursts on bus A, everything else as the plug-in starts.
        {
            juce::AudioBuffer<float> drone(1, 8 * 48000), gappy(1, 8 * 48000);
            juce::Random r(11);
            for (int i = 0; i < drone.getNumSamples(); ++i)
            {
                const double t = i / 48000.0;
                drone.setSample(0, i, static_cast<float>(0.2 * std::sin(c_twoPi * 45.0 * t) + 0.15 * std::sin(c_twoPi * 72.0 * t)));
                const double u = std::fmod(t, 1.6) / 1.6; // a burst every 1.6 s, 60% of it sounding
                gappy.setSample(0, i, u < 0.6 ? static_cast<float>(0.3 * std::sin(juce::MathConstants<double>::pi * u / 0.6) * (std::sin(c_twoPi * 220.0 * t) + 0.3 * (r.nextFloat() * 2.0f - 1.0f))) : 0.0f);
            }
            const auto droneFile = writeWav(folder, "stow_drone", drone, 48000.0), gappyFile = writeWav(folder, "stow_gappy", gappy, 48000.0);
            auto level = [&](int mode)
            {
                Rig rig;
                rig.hold.loadFile(0, droneFile, error);
                rig.hold.setSkipSilence(0, false);
                rig.hold.setBusOverride(0, CargoHold::c_stowBus);
                rig.hold.loadFile(1, gappyFile, error);
                rig.hold.setBusOverride(1, 0);
                RigEngine::Params p; // as the plug-in starts
                p.stow.mode = mode;
                rig.start(p, 48000.0);
                render(rig.engine, 4.0, 48000.0);
                rig.engine.takeSectionLevels();
                render(rig.engine, 90.0, 48000.0);
                return 20.0 * std::log10(static_cast<double>(rig.engine.takeSectionLevels()[RigEngine::SectionStow]));
            };
            const double sub = level(RigEngine::StowSub), wake = level(RigEngine::StowWake), fly = level(RigEngine::StowFly), dry = level(RigEngine::StowDry);
            std::cout << "  the same drone on Stowaway over 90 s, at the starting settings: Sub " << juce::String(sub, 1) << " dB, Wake " << juce::String(wake, 1) << " dB, Dry " << juce::String(dry, 1)
                      << " dB (Fly, which passes it on for now: " << juce::String(fly, 1) << " dB)" << std::endl;
            check(std::abs(wake - sub) < 1.0 && std::abs(dry - sub) < 1.0 && std::abs(wake - dry) < 1.0, "modes: Sub, Wake and Dry are within 1 dB of each other on the same sound");
        }
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : (juce::String(g_failures) + " FAILED")) << std::endl;
    return g_failures == 0 ? 0 : 1;
}

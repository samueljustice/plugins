// Faders Together and Auto Swell, on the engine itself.
//
//   SwooshbucklerTogether [output folder] [only these parts]
//
// prints what it measures and ends with "ALL PASSED" (exit code 0) or "n FAILED".
//
//   clock      every peak of the clock has bus A's pass at its closest, bus B's, and the aimed hit, each within one
//              chunk (32 samples) of it, over a minute, at Chaos 0 and 100%, at 48 and 192 kHz; no pass is at its
//              closest anywhere else
//   every      Hit Every 1, 2, 3 and 4: which peaks get a hit
//   rides      at Stagger 0 every fader shows the same swell; Stagger fans them in track order; Peak At moves the
//              trough and leaves the top on the peak; Chaos never moves a peak
//   heard      the top of the swell is heard as the pass goes by: within 10 ms of the pass's marker, for the
//              closest passes and the furthest (their sound takes 6 to about 150 ms to come out of the pass)
//   level      the loudest moments are no more than 1 dB over what they are with Auto Ride off
//   switch     a change of Ride Mode, and of Rate while together, does not click; the passes join the clock and
//              leave it; a linked bus B stays on bus A's passes
//   render     the clock starts where it is told: a file opens on a trough whatever Peak At is; two seeded
//              restarts are the same
//   auto       Auto Swell: Swell held from outside, On a Clock and On Peaks each raise the faders; Chance; Loose;
//              Hold; where it rests

#include <JuceHeader.h>
#include "RigEngine.h"
#include "Parameters.h"
#include "Room.h"

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

    juce::File writeWav(const juce::File& folder, const juce::String& name, const juce::AudioBuffer<float>& b)
    {
        auto f = folder.getChildFile(name + ".wav");
        f.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(f);
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream, c_rate, static_cast<unsigned int>(b.getNumChannels()), 24, {}, 0));
        if (w != nullptr) w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()); else delete stream;
        return f;
    }

    // A steady tone that loops without a seam.
    juce::AudioBuffer<float> tone(double hz, float gain)
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_rate * 4.0));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample(0, i, gain * static_cast<float>(std::sin(c_twoPi * hz * i / c_rate)));
        return b;
    }
    // A steady hiss.
    juce::AudioBuffer<float> hiss(int seed)
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_rate * 8.0));
        juce::Random r(seed);
        float y = 0.0f;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            y += 0.2f * ((r.nextFloat() * 2.0f - 1.0f) - y);
            b.setSample(0, i, 0.8f * y);
        }
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

    // One run, a chunk at a time: where the clock's peaks fall (from the "peak in" view: it counts down and jumps
    // back up as a peak goes by) and every marker.
    struct Run
    {
        std::vector<double> peaks;                 // samples
        std::array<std::vector<double>, 5> marks;  // by RigEngine::MarkerKind
        int total = 0;
        double fs = c_rate;
    };
    template <typename Each>
    Run run(RigEngine& e, double fs, double seconds, juce::AudioBuffer<float>* keep, Each&& each)
    {
        Run r;
        r.fs = fs;
        r.total = static_cast<int>(seconds * fs) / c_chunk * c_chunk;
        juce::AudioBuffer<float> scratch(2, c_chunk);
        if (keep != nullptr)
            keep->setSize(2, r.total);
        float lastIn = -1.0f;
        float lastSeconds = 0.0f;
        for (int pos = 0; pos < r.total; pos += c_chunk)
        {
            const float swell = each(pos); // (may set parameters; returns how long a swell is now)
            float* l = keep != nullptr ? keep->getWritePointer(0, pos) : scratch.getWritePointer(0);
            float* rr = keep != nullptr ? keep->getWritePointer(1, pos) : scratch.getWritePointer(1);
            e.process(nullptr, nullptr, l, rr, c_chunk);
            const float in = e.getCyclePeakIn();
            if (lastIn >= 0.0f && in > lastIn + 0.5f * swell) // (a Rate that jumps moves it too, by less)
                r.peaks.push_back(static_cast<double>(pos + c_chunk) - (static_cast<double>(swell) - static_cast<double>(in)) * fs);
            lastIn = in;
            lastSeconds = swell;
            for (int m = 0; m < e.getNumMarkers(); ++m)
                r.marks[static_cast<size_t>(e.getMarker(m).kind)].push_back(static_cast<double>(pos + e.getMarker(m).offset));
        }
        juce::ignoreUnused(lastSeconds);
        return r;
    }

    // How far the nearest of `marks` is from `at` (samples; a large number with none).
    double nearest(const std::vector<double>& marks, double at)
    {
        double best = 1.0e12;
        for (const double m : marks)
            if (std::abs(m - at) < std::abs(best))
                best = m - at;
        return best;
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
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getCurrentWorkingDirectory();
    folder.createDirectory();
    juce::StringArray only;
    for (int i = 2; i < argc; ++i)
        only.add(argv[i]);
    auto wanted = [&only](const char* part) { return only.isEmpty() || only.contains(part); };

    std::cout << "Faders Together" << std::endl;
    {
        const RigEngine::Params p;
        check(p.rideMode == RigEngine::RideApart && p.autoSwell.mode == RigEngine::AutoSwellOff
                  && p.swirlDetail[0].type == RigEngine::SwirlBoth && p.swirlDetail[1].type == RigEngine::SwirlBoth,
              "defaults: Faders Apart, no Auto Swell, Swirl as it was");
    }

    juce::String error;
    // A tone on bus A, a hiss on bus B, hits on the hit gate.
    CargoHold hold;
    check(hold.loadFile(0, writeWav(folder, "together_src_tone", tone(330.0, 0.4f)), error)
              && hold.loadFile(1, writeWav(folder, "together_src_hiss", hiss(5)), error)
              && hold.loadFile(2, writeWav(folder, "together_src_hits", hits()), error), "loads the sources " + error);
    hold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
    hold.setRoleOverride(1, static_cast<int>(CargoRole::Bed));
    hold.setRoleOverride(2, static_cast<int>(CargoRole::Impact));
    hold.setSkipSilence(0, false);
    hold.setSkipSilence(1, false);
    check(hold.getBus(0) == 0 && hold.getBus(1) == 1 && hold.getBus(2) == CargoHold::c_hitsBus, "a track on bus A, one on bus B, one on the hits");

    RigEngine::Params together;
    together.rideMode = RigEngine::RideTogether;

    // ---- clock ---------------------------------------------------------------------------------------------------
    if (wanted("clock"))
    {
        std::cout << "clock: the passes and the hits on its peaks" << std::endl;
        for (const double fs : { 48000.0, 192000.0 })
            for (const float chaos : { 0.0f, 1.0f })
            {
                auto e = std::make_unique<RigEngine>();
                auto q = together;
                q.chaos = chaos;
                start(*e, hold, q, fs);
                const float swell = ::Params::passSeconds(q.rate);
                juce::AudioBuffer<float> heard;
                const Run r = run(*e, fs, 60.0, &heard, [swell](int) { return swell; });
                // The hit as it is heard: the sharpest change within 50 ms of each peak (the hit's first transient;
                // the tone and the hiss have nothing as sharp).
                double hitFrom = 1.0e9, hitTo = -1.0e9;
                for (const double at : r.peaks)
                {
                    if (at < 1.5 * swell * fs || at > static_cast<double>(r.total) - fs)
                        continue;
                    const int mid = static_cast<int>(at), reach = static_cast<int>(0.05 * fs);
                    float top = 0.0f;
                    const float* x = heard.getReadPointer(0);
                    for (int i = mid - reach; i < mid + reach; ++i)
                        top = std::max(top, std::abs(x[i + 1] - 2.0f * x[i] + x[i - 1]));
                    int first = mid - reach;
                    while (first < mid + reach && std::abs(x[first + 1] - 2.0f * x[first] + x[first - 1]) < 0.1f * top)
                        ++first;
                    hitFrom = std::min(hitFrom, static_cast<double>(first) - at);
                    hitTo = std::max(hitTo, static_cast<double>(first) - at);
                }
                std::cout << "    (as heard, the hit's first sharp change, a tenth of its sharpest, is " << num(1000.0 * hitFrom / fs, 2) << " .. " << num(1000.0 * hitTo / fs, 2)
                          << " ms from the peak)" << std::endl;
                int peaks = 0, onA = 0, onB = 0, onHit = 0, stray = 0;
                double worstA = 0.0, worstB = 0.0, worstHit = 0.0, worstGap = 0.0;
                const double from = 1.5 * swell * fs, to = static_cast<double>(r.total) - fs;
                for (size_t k = 0; k < r.peaks.size(); ++k)
                {
                    const double at = r.peaks[k];
                    if (k > 0)
                        worstGap = std::max(worstGap, std::abs(at - r.peaks[k - 1] - static_cast<double>(swell) * fs));
                    if (at < from || at > to)
                        continue;
                    ++peaks;
                    const double a = nearest(r.marks[RigEngine::MarkPassA], at), b = nearest(r.marks[RigEngine::MarkPassB], at),
                                 h = nearest(r.marks[RigEngine::MarkHit], at);
                    if (std::abs(a) <= c_chunk) { ++onA; worstA = std::max(worstA, std::abs(a)); }
                    if (std::abs(b) <= c_chunk) { ++onB; worstB = std::max(worstB, std::abs(b)); }
                    if (std::abs(h) <= c_chunk) { ++onHit; worstHit = std::max(worstHit, std::abs(h)); }
                }
                for (const int kind : { RigEngine::MarkPassA, RigEngine::MarkPassB, RigEngine::MarkHit })
                    for (const double m : r.marks[static_cast<size_t>(kind)])
                        if (m >= from && m <= to && std::abs(nearest(r.peaks, m)) > c_chunk)
                            ++stray;
                std::cout << "  " << fs << " Hz, Chaos " << 100.0f * chaos << "%: " << peaks << " peaks, a swell every " << swell << " s (furthest from that "
                          << num(worstGap, 1) << " samples); bus A's pass on " << onA << " (furthest " << num(worstA, 1) << " samples), bus B's on " << onB
                          << " (" << num(worstB, 1) << "), the hit on " << onHit << " (" << num(worstHit, 1) << "); markers away from a peak " << stray << std::endl;
                const juce::String where = " at " + juce::String(fs / 1000.0, 0) + " kHz, Chaos " + juce::String(juce::roundToInt(100.0f * chaos)) + "%";
                check(peaks >= 14 && worstGap <= c_chunk, "the clock's peaks are a swell apart" + where);
                check(onA == peaks && onB == peaks, "a pass of each bus is at its closest within a chunk of every peak" + where);
                check(onHit == peaks, "the aimed hit lands within a chunk of every peak" + where);
                check(stray == 0, "no pass is at its closest, and no hit lands, away from a peak" + where);
            }
    }

    if (wanted("clock"))
    {
        // A room: the same clock, each pass in its own lane.
        auto e = std::make_unique<RigEngine>();
        e->setWidth(room::Width::Surround714);
        start(*e, hold, together);
        const float swell = ::Params::passSeconds(together.rate);
        juce::AudioBuffer<float> o;
        const Run r = run(*e, c_rate, 30.0, &o, [swell](int) { return swell; });
        int peaks = 0, on = 0;
        for (size_t k = 1; k < r.peaks.size(); ++k)
        {
            ++peaks;
            on += std::abs(nearest(r.marks[RigEngine::MarkPassA], r.peaks[k])) <= c_chunk && std::abs(nearest(r.marks[RigEngine::MarkPassB], r.peaks[k])) <= c_chunk
                      && std::abs(nearest(r.marks[RigEngine::MarkHit], r.peaks[k])) <= c_chunk ? 1 : 0;
        }
        bool finite = true;
        for (int i = 0; i < o.getNumSamples(); ++i)
            finite = finite && std::isfinite(o.getSample(0, i)) && std::isfinite(o.getSample(1, i));
        std::cout << "  7.1.4: " << on << " of " << peaks << " peaks have both buses' passes and the hit; level " << juce::Decibels::gainToDecibels(o.getRMSLevel(0, 0, o.getNumSamples())) << " dB" << std::endl;
        check(finite && peaks >= 7 && on == peaks && o.getRMSLevel(0, 0, o.getNumSamples()) > 0.001f, "in a room the passes and the hit are on the clock's peaks too");
    }

    // ---- every ---------------------------------------------------------------------------------------------------
    if (wanted("every"))
    {
        std::cout << "every: Hit Every" << std::endl;
        for (int every = 1; every <= 4; ++every)
        {
            auto e = std::make_unique<RigEngine>();
            auto q = together;
            q.rate = 1.0f; // a swell every 1.6 s
            q.together.hitEvery = every;
            q.hitSense = 0.0f; // (Sensitivity has no say here)
            start(*e, hold, q);
            const float swell = ::Params::passSeconds(q.rate);
            const Run r = run(*e, c_rate, 40.0, nullptr, [swell](int) { return swell; });
            int wantedHits = 0, got = 0, wrong = 0;
            for (size_t k = 1; k < r.peaks.size(); ++k) // (the first peak comes before anything has sounded)
            {
                const bool has = std::abs(nearest(r.marks[RigEngine::MarkHit], r.peaks[k])) <= c_chunk;
                const bool should = k % static_cast<size_t>(every) == 0;
                wantedHits += should ? 1 : 0;
                got += has ? 1 : 0;
                wrong += has != should ? 1 : 0;
            }
            std::cout << "  Hit Every " << every << ": " << r.peaks.size() - 1 << " peaks, " << got << " with a hit (" << wantedHits << " wanted), "
                      << wrong << " peaks wrong, " << r.marks[RigEngine::MarkHit].size() << " hits in all" << std::endl;
            check(wrong == 0 && got == wantedHits && got >= 5, "Hit Every " + juce::String(every) + ": a hit on every " + juce::String(every) + (every == 1 ? " peak" : " peaks") + " and on no other");
        }
    }

    // Four tones, two on each bus (no hits), for what the faders do.
    CargoHold four;
    for (int t = 0; t < 4; ++t)
    {
        four.loadFile(t, writeWav(folder, "together_src_tone" + juce::String(t), tone(220.0 + 110.0 * t, 0.25f)), error);
        four.setRoleOverride(t, static_cast<int>(t < 2 ? CargoRole::Tonal : CargoRole::Bed));
        four.setSkipSilence(t, false);
    }

    // ---- rides ---------------------------------------------------------------------------------------------------
    if (wanted("rides"))
    {
        std::cout << "rides: the faders on the one swell" << std::endl;
        check(four.getBus(0) == 0 && four.getBus(1) == 0 && four.getBus(2) == 1 && four.getBus(3) == 1, "two tracks on bus A and two on bus B");
        // Each fader over each swell: where its top falls (seconds from the swell's peak on the clock), and how
        // much of the swell's width at half height is before the top (the build's share: Peak At).
        struct Ride { float apart = 0.0f; std::array<double, 4> top {}, build {}; double range = 0.0; int swells = 0; double peakJitter = 0.0; };
        auto ride = [&](float stagger, float peakAt, float chaos)
        {
            auto e = std::make_unique<RigEngine>();
            auto q = together;
            q.rate = 0.6f;
            q.energy = 0.8f;
            q.chaos = chaos;
            q.together.stagger = stagger;
            q.together.peakAt = peakAt;
            start(*e, four, q);
            const float swell = ::Params::passSeconds(q.rate);
            Ride out;
            std::array<std::vector<float>, 4> shown;
            const Run r = run(*e, c_rate, 40.0, nullptr, [&](int)
            {
                float lo = 2.0f, hi = -1.0f;
                for (size_t t = 0; t < 4; ++t)
                {
                    const float v = e->getRide(static_cast<int>(t));
                    shown[t].push_back(v);
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                if (shown[0].size() > 4)
                    out.apart = std::max(out.apart, hi - lo);
                return swell;
            });
            // (shown[t][n] is the fader after chunk n - 1, so at the time n chunks in.)
            const double chunkSeconds = c_chunk / c_rate;
            float lowest = 2.0f, highest = -1.0f;
            for (size_t t = 0; t < 4; ++t)
                for (size_t n = 8; n < shown[t].size(); ++n)
                    lowest = std::min(lowest, shown[t][n]);
            for (size_t k = 1; k + 2 < r.peaks.size(); ++k)
            {
                const double mid = r.peaks[k] / c_rate;
                for (size_t t = 0; t < 4; ++t)
                {
                    const auto from = static_cast<size_t>((mid - 0.45 * swell) / chunkSeconds), to = static_cast<size_t>((mid + 0.45 * swell) / chunkSeconds);
                    size_t topAt = from;
                    for (size_t n = from; n < std::min(to, shown[t].size()); ++n)
                        if (shown[t][n] > shown[t][topAt])
                            topAt = n;
                    const float half = 0.5f * (shown[t][topAt] + lowest);
                    size_t up = topAt, down = topAt;
                    while (up > 0 && shown[t][up] > half) --up;
                    while (down + 1 < shown[t].size() && shown[t][down] > half) ++down;
                    out.top[t] += static_cast<double>(topAt) * chunkSeconds - mid;
                    out.build[t] += static_cast<double>(topAt - up) / static_cast<double>(std::max<size_t>(1, down - up));
                    highest = std::max(highest, shown[t][topAt]);
                    if (t == 0)
                        out.peakJitter = std::max(out.peakJitter, std::abs(static_cast<double>(topAt) * chunkSeconds - mid));
                }
                ++out.swells;
            }
            for (size_t t = 0; t < 4; ++t)
            {
                out.top[t] /= std::max(1, out.swells);
                out.build[t] /= std::max(1, out.swells);
            }
            out.range = static_cast<double>(highest - lowest);
            return out;
        };
        const float swell = ::Params::passSeconds(0.6f);
        const Ride even = ride(0.0f, 0.5f, 0.0f);
        std::cout << "  Stagger 0%: the four faders are at most " << even.apart << " apart; their top is " << num(1000.0 * even.top[0], 1)
                  << " ms from the clock's peak, with " << num(even.build[0], 3) << " of the swell's width before it; they move over " << num(even.range, 3) << std::endl;
        check(even.apart == 0.0f, "Stagger 0%: every fader shows the same swell");
        check(std::abs(even.top[0]) < 0.002 && std::abs(even.build[0] - 0.5) < 0.01 && even.range > 0.5, "the swell's top is on the clock's peak, with as much build as tail");
        const Ride fanned = ride(1.0f, 0.5f, 0.0f);
        std::cout << "  Stagger 100%: the tops fall " << num(fanned.top[0] / swell, 3) << " / " << num(fanned.top[1] / swell, 3) << " / " << num(fanned.top[2] / swell, 3)
                  << " / " << num(fanned.top[3] / swell, 3) << " of a swell from the peak" << std::endl;
        check(std::abs(fanned.top[0] / swell + 0.1) < 0.005 && std::abs(fanned.top[3] / swell - 0.1) < 0.005
                  && fanned.top[0] < fanned.top[1] && fanned.top[1] < fanned.top[2] && fanned.top[2] < fanned.top[3],
              "Stagger 100%: the tracks fan in, in track order, from a tenth of a swell early to a tenth late");
        const Ride half = ride(0.5f, 0.5f, 0.0f);
        check(std::abs(half.top[0] / swell + 0.05) < 0.005 && std::abs(half.top[3] / swell - 0.05) < 0.005, "Stagger 50%: half as far");
        for (const float peakAt : { 0.2f, 0.7f, 0.8f })
        {
            const Ride skew = ride(0.0f, peakAt, 0.0f);
            std::cout << "  Peak At " << 100.0f * peakAt << "%: top " << num(1000.0 * skew.top[0], 1) << " ms from the clock's peak, "
                      << num(skew.build[0], 3) << " of the swell's width before it" << std::endl;
            check(std::abs(skew.top[0]) < 0.002 && std::abs(skew.build[0] - static_cast<double>(peakAt)) < 0.01,
                  "Peak At " + juce::String(juce::roundToInt(100.0f * peakAt)) + "%: the top stays on the peak, the build is that share of the swell");
        }
        const Ride wild = ride(0.0f, 0.5f, 1.0f);
        std::cout << "  Chaos 100%: the four faders are at most " << wild.apart << " apart; the top is never more than " << num(1000.0 * wild.peakJitter, 1) << " ms from the peak" << std::endl;
        check(wild.apart == 0.0f && wild.peakJitter < 0.002, "Chaos 100%: the faders are still one swell, its top on the peak");
    }

    // ---- heard ---------------------------------------------------------------------------------------------------
    if (wanted("heard"))
    {
        std::cout << "heard: the top of the swell as the pass goes by" << std::endl;
        // One tone on bus A, Fly-By passes (one heard at a time), no colours. The same render with Auto Ride off
        // has the same passes and the same carrier, so the ratio of the two is the swell alone, as it is heard.
        CargoHold one;
        one.loadFile(0, writeWav(folder, "together_src_one", tone(330.0, 0.4f)), error);
        one.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
        one.setSkipSilence(0, false);
        for (const float bend : { 0.0f, 1.0f })
        {
            auto q = together;
            q.rate = 1.0f;
            q.energy = 0.9f;
            q.chaos = 1.0f;
            q.fill = 0.0f;
            q.space = 0.0f;
            q.hitAim = false;
            for (auto& b : q.buses) { b.doppler = 1.0f; b.bend = bend; b.width = 0.6f; b.swirl = 0.0f; b.drive = 0.0f; b.low = 0.0f; b.filter = 0.0f; b.duck = 0.0f; b.key = 0.0f; b.passMode = 1; }
            const float swell = ::Params::passSeconds(q.rate);
            juce::AudioBuffer<float> ridden, still;
            std::vector<std::pair<double, float>> passes; // each marker, and how close that pass is
            {
                auto e = std::make_unique<RigEngine>();
                start(*e, one, q);
                size_t seen = 0;
                Run r;
                r = run(*e, c_rate, 90.0, &ridden, [&](int) { return swell; });
                juce::ignoreUnused(seen);
                // The same again for how close each pass was: the flight views say, chunk by chunk.
                auto f = std::make_unique<RigEngine>();
                start(*f, one, q);
                juce::AudioBuffer<float> scratch(2, c_chunk);
                for (int pos = 0; pos < r.total; pos += c_chunk)
                {
                    f->process(nullptr, nullptr, scratch.getWritePointer(0), scratch.getWritePointer(1), c_chunk);
                    for (int m = 0; m < f->getNumMarkers(); ++m)
                        if (f->getMarker(m).kind == RigEngine::MarkPassA)
                        {
                            const auto a = f->getFlightView(0), b = f->getFlightView(1);
                            passes.push_back({ static_cast<double>(pos + f->getMarker(m).offset), std::abs(a.age - 0.5f) < std::abs(b.age - 0.5f) ? a.closeness : b.closeness });
                        }
                }
            }
            {
                auto e = std::make_unique<RigEngine>();
                auto off = q;
                off.rideOn = false;
                start(*e, one, off);
                run(*e, c_rate, 90.0, &still, [&](int) { return swell; });
            }
            auto power = [](const juce::AudioBuffer<float>& o, int from, int n)
            {
                double sum = 0.0;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                        sum += static_cast<double>(o.getSample(ch, from + i)) * o.getSample(ch, from + i);
                return sum;
            };
            const int window = 144, hop = 12, reach = static_cast<int>(0.06 * c_rate);
            double worst = 0.0, worstNear = 0.0, worstFar = 0.0, sum = 0.0;
            int counted = 0, nearOnes = 0, farOnes = 0;
            float closest = 0.0f, furthest = 1.0f;
            for (const auto& [at, closeness] : passes)
            {
                const int mid = static_cast<int>(at);
                if (mid < static_cast<int>(3.0 * swell * c_rate) || mid + reach + window >= ridden.getNumSamples())
                    continue;
                double best = -1.0;
                int bestAt = 0;
                for (int off = -reach; off <= reach; off += hop)
                {
                    const double below = power(still, mid + off - window / 2, window);
                    if (below < 1.0e-9)
                        continue;
                    const double ratio = power(ridden, mid + off - window / 2, window) / below;
                    if (ratio > best) { best = ratio; bestAt = off; }
                }
                if (best < 0.0)
                    continue;
                const double ms = 1000.0 * bestAt / c_rate;
                ++counted;
                sum += ms;
                worst = std::max(worst, std::abs(ms));
                closest = std::max(closest, closeness);
                furthest = std::min(furthest, closeness);
                if (closeness > 0.9f) { ++nearOnes; worstNear = std::max(worstNear, std::abs(ms)); }
                if (closeness < 0.1f) { ++farOnes; worstFar = std::max(worstFar, std::abs(ms)); }
            }
            std::cout << "  Bend " << 100.0f * bend << "%: " << counted << " passes (closeness " << furthest << " .. " << closest << "); the heard top is on average "
                      << num(sum / std::max(1, counted), 2) << " ms from the marker, at most " << num(worst, 2) << " ms; the " << nearOnes << " closest passes at most "
                      << num(worstNear, 2) << " ms, the " << farOnes << " furthest at most " << num(worstFar, 2) << " ms" << std::endl;
            const juce::String where = " (Bend " + juce::String(juce::roundToInt(100.0f * bend)) + "%)";
            check(counted >= 30 && nearOnes >= 3 && farOnes >= 3, "passes at both ends of how close a pass can be" + where);
            check(worst <= 10.0, "the top of the swell is heard within 10 ms of the pass's marker, the closest passes and the furthest" + where);
        }
    }

    // Two beds and a tonal sound, no hits: what the level does.
    CargoHold beds;
    beds.loadFile(0, writeWav(folder, "together_src_bed0", tone(180.0, 0.35f)), error);
    beds.loadFile(1, writeWav(folder, "together_src_bed1", hiss(7)), error);
    beds.loadFile(2, writeWav(folder, "together_src_bed2", hiss(11)), error);
    beds.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
    beds.setRoleOverride(1, static_cast<int>(CargoRole::Bed));
    beds.setRoleOverride(2, static_cast<int>(CargoRole::Tonal));
    for (int t = 0; t < 3; ++t)
        beds.setSkipSilence(t, false);

    // ---- level ---------------------------------------------------------------------------------------------------
    if (wanted("level"))
    {
        std::cout << "level: the loudest moments against Auto Ride off" << std::endl;
        auto loudest = [&](RigEngine::Params q, float& samplePeak, float& average)
        {
            auto e = std::make_unique<RigEngine>();
            start(*e, beds, q);
            juce::AudioBuffer<float> o;
            const float swell = ::Params::passSeconds(q.rate);
            run(*e, c_rate, 60.0, &o, [swell](int) { return swell; });
            const int from = static_cast<int>(4.0 * c_rate), w = static_cast<int>(0.05 * c_rate);
            float top = 0.0f;
            for (int at = from; at + w <= o.getNumSamples(); at += w / 2)
                top = std::max(top, std::sqrt(0.5f * (o.getRMSLevel(0, at, w) * o.getRMSLevel(0, at, w) + o.getRMSLevel(1, at, w) * o.getRMSLevel(1, at, w))));
            samplePeak = juce::Decibels::gainToDecibels(o.getMagnitude(from, o.getNumSamples() - from));
            average = juce::Decibels::gainToDecibels(std::sqrt(0.5f * (o.getRMSLevel(0, from, o.getNumSamples() - from) * o.getRMSLevel(0, from, o.getNumSamples() - from)
                                                                       + o.getRMSLevel(1, from, o.getNumSamples() - from) * o.getRMSLevel(1, from, o.getNumSamples() - from))));
            return juce::Decibels::gainToDecibels(top);
        };
        for (const float chaos : { 0.4f, 0.0f, 1.0f })
            for (const float energy : { 0.6f, 1.0f })
            {
                auto q = together;
                q.chaos = chaos;
                q.energy = energy;
                auto off = q;
                off.rideOn = false;
                auto apart = q;
                apart.rideMode = RigEngine::RideApart;
                float peakT = 0.0f, peakOff = 0.0f, peakApart = 0.0f, avgT = 0.0f, avgOff = 0.0f, avgApart = 0.0f;
                const float t = loudest(q, peakT, avgT), o = loudest(off, peakOff, avgOff), a = loudest(apart, peakApart, avgApart);
                std::cout << "  Chaos " << 100.0f * chaos << "%, Energy " << 100.0f * energy << "%: loudest 50 ms together " << num(t) << " dB, Auto Ride off " << num(o)
                          << " dB (" << num(t - o) << " dB over), apart " << num(a) << " dB; sample peaks " << num(peakT) << " / " << num(peakOff) << " / " << num(peakApart)
                          << " dB; average " << num(avgT) << " / " << num(avgOff) << " / " << num(avgApart) << " dB" << std::endl;
                check(t - o <= 1.0f && peakT - peakOff <= 1.0f, "together, the loudest moments are no more than 1 dB over Auto Ride off (Chaos "
                                                               + juce::String(juce::roundToInt(100.0f * chaos)) + "%, Energy " + juce::String(juce::roundToInt(100.0f * energy)) + "%)");
            }
        std::cout << "  (the make-up with the faders still: " << num(juce::Decibels::gainToDecibels(RigEngine::rideMakeup(0.0f, 0.4f))) << " dB at Chaos 40%)" << std::endl;
    }

    // ---- switch --------------------------------------------------------------------------------------------------
    if (wanted("switch"))
    {
        std::cout << "switch: a change of Ride Mode and of Rate" << std::endl;
        // Tones only (any step in a delay or a gain shows in the second difference), the passes bending.
        auto base = [&]()
        {
            RigEngine::Params q;
            q.fill = 0.0f;
            q.space = 0.0f;
            q.chaos = 0.5f;
            q.energy = 0.7f;
            for (auto& b : q.buses) { b.swirl = 0.0f; b.drive = 0.0f; b.low = 0.0f; b.filter = 0.0f; b.duck = 0.0f; b.key = 0.0f; b.doppler = 0.8f; b.bend = 0.8f; }
            return q;
        };
        for (const bool linked : { false, true })
        {
            auto e = std::make_unique<RigEngine>();
            auto q = base();
            q.linkBuses = linked;
            start(*e, four, q);
            juce::AudioBuffer<float> o;
            // 0 .. 20 s apart; 20 .. 60 together (at 28 s the tracks are started again); at 40 s Rate goes from 40% to 70%, at 50 s back, in one step
            // each; 60 .. 90 apart again; 90 .. 110 together with Peak At and Stagger moving.
            float worstGapApart = 0.0f, worstGapLinked = 0.0f;
            float shownApart = 0.0f;
            const Run r = run(*e, c_rate, 110.0, &o, [&](int pos)
            {
                const double t = pos / c_rate;
                if (pos == static_cast<int>(28.0 * c_rate))
                    e->restart(); // Play pressed again while it plays: the tracks start again, the clock and the passes carry on
                auto now = q;
                now.rideMode = (t >= 20.0 && t < 60.0) || t >= 90.0 ? RigEngine::RideTogether : RigEngine::RideApart;
                now.rate = t >= 40.0 && t < 50.0 ? 0.7f : 0.4f;
                if (t >= 90.0)
                {
                    now.together.peakAt = 0.5f + 0.3f * static_cast<float>(std::sin(0.9 * t));
                    now.together.stagger = 0.5f + 0.5f * static_cast<float>(std::sin(1.3 * t));
                }
                e->setParams(now);
                // Apart again (well after the passes have left the clock): the two passes of a bus half a pass apart.
                if (t > 75.0 && t < 90.0)
                    for (int b = 0; b < 2; ++b)
                    {
                        float gap = e->getFlightView(b * 2 + 1).age - e->getFlightView(b * 2).age;
                        gap -= std::floor(gap);
                        worstGapApart = std::max(worstGapApart, std::abs(gap - 0.5f));
                    }
                if (t > 1.0)
                    for (int k = 0; k < 2; ++k)
                        worstGapLinked = std::max(worstGapLinked, std::abs(e->getFlightView(k).age - e->getFlightView(2 + k).age));
                if (t > 21.0 && t < 60.0)
                    shownApart = std::max(shownApart, std::abs(e->getRide(0) - e->getRide(3)));
                return ::Params::passSeconds(now.rate);
            });
            const auto at = [](double seconds) { return static_cast<int>(seconds * c_rate); };
            const float calm = std::max(sharpest(o, at(5.0), at(20.0)), sharpest(o, at(34.0), at(40.0)));
            const float intoTogether = sharpest(o, at(20.0), at(34.0)), rateUp = sharpest(o, at(40.0), at(50.0)), rateDown = sharpest(o, at(50.0), at(60.0)),
                        outAgain = sharpest(o, at(60.0), at(90.0)), moving = sharpest(o, at(90.0), at(110.0));
            // The passes on the clock: from 34 s (they have joined by then) to 60 s every peak has bus A's pass.
            int peaks = 0, onA = 0;
            for (const double p : r.peaks)
                if (p > 34.0 * c_rate && p < 59.0 * c_rate)
                {
                    ++peaks;
                    onA += std::abs(nearest(r.marks[RigEngine::MarkPassA], p)) <= c_chunk ? 1 : 0;
                }
            std::cout << "  " << (linked ? "linked" : "unlinked") << ": sharpest change in calm play " << calm << "; into Faders Together " << intoTogether << ", Rate up "
                      << rateUp << ", Rate down " << rateDown << ", apart again " << outAgain << ", Peak At and Stagger moving " << moving << std::endl;
            std::cout << "    half a second and more after the change the faders of bus A and bus B show at most " << shownApart << " apart; from 14 s after it " << onA << " of "
                      << peaks << " peaks have bus A's pass (through both changes of Rate); apart again, the two passes of a bus are within " << worstGapApart
                      << " of half a pass apart" << (linked ? "; bus B's passes are never more than " + juce::String(worstGapLinked) + " of a pass from bus A's" : juce::String()) << std::endl;
            const juce::String which = linked ? " (linked)" : " (unlinked)";
            check(intoTogether < 2.0f * calm && outAgain < 2.0f * calm, "a change of Ride Mode, and Play pressed again while together, do not click" + which);
            check(rateUp < 2.5f * calm && rateDown < 2.5f * calm, "Rate jumping while the faders are together does not click" + which);
            check(moving < 2.0f * calm, "Peak At and Stagger moving do not click" + which);
            check(shownApart == 0.0f, "half a second after the change every fader is on the one swell" + which);
            check(peaks >= 6 && onA == peaks, "the passes join the clock and stay on it through a change of Rate" + which);
            check(worstGapApart < 1.0e-3f, "back apart, the two passes of a bus are half a pass apart again" + which);
            if (linked)
                check(worstGapLinked < 1.0e-4f, "a linked bus B flies bus A's passes, on the clock and off it");
        }
    }

    // ---- render --------------------------------------------------------------------------------------------------
    if (wanted("render"))
    {
        std::cout << "render: where the clock starts" << std::endl;
        for (const float peakAt : { 0.5f, 0.3f, 0.8f })
        {
            auto e = std::make_unique<RigEngine>();
            auto q = together;
            q.energy = 0.8f;
            q.together.peakAt = peakAt;
            start(*e, four, q);
            const float swell = ::Params::passSeconds(q.rate);
            const double preRoll = 2.0;
            e->setCycleStart(-preRoll / static_cast<double>(swell));
            std::vector<float> shown;
            const Run r = run(*e, c_rate, 12.0, nullptr, [&](int) { shown.push_back(e->getRide(0)); return swell; });
            // The fader after the pre-roll against the lowest it ever is (shown[n]: n chunks in).
            float lowest = 2.0f;
            for (size_t n = 8; n < shown.size(); ++n)
                lowest = std::min(lowest, shown[n]);
            const float atStart = shown[static_cast<size_t>(preRoll * c_rate / c_chunk)] - lowest;
            const double firstPeak = r.peaks.empty() ? -1.0 : r.peaks[0] / c_rate;
            // (With the trough after the pre-roll the first peak may fall inside it: the one after the trough counts.)
            double peakAfter = -1.0;
            for (const double p : r.peaks)
                if (peakAfter < 0.0 && p / c_rate > preRoll - 0.01)
                    peakAfter = p / c_rate;
            std::cout << "  Peak At " << 100.0f * peakAt << "%, a 2 s pre-roll: after it the fader is " << atStart << " over its lowest, the next peak at " << num(peakAfter, 3)
                      << " s (" << num(preRoll + peakAt * swell, 3) << " s wanted); the first peak of all at " << num(firstPeak, 3) << " s" << std::endl;
            check(atStart < 1.0e-4f && std::abs(peakAfter - (preRoll + peakAt * swell)) < 0.002,
                  "the render opens on a trough after its pre-roll, Peak At " + juce::String(juce::roundToInt(100.0f * peakAt)) + "%");
        }
        // Two seeded restarts: the same sound from each. (Faders Apart for what the engine did before: its restarts
        // are not the same to the last bit either.)
        for (int which = 0; which < 3; ++which)
        {
            auto e = std::make_unique<RigEngine>();
            auto q = together;
            q.chaos = 1.0f;
            if (which == 0)
                q.rideMode = RigEngine::RideApart;
            if (which == 2)
            {
                q.autoSwell.mode = RigEngine::AutoSwellClock;
                q.autoSwell.every = 1.0f;
                q.autoSwell.hold = 0.3f;
                q.autoSwell.chance = 0.5f;
                q.autoSwell.loose = 0.5f;
            }
            start(*e, hold, q);
            juce::AudioBuffer<float> o(2, static_cast<int>(12.0 * c_rate) / c_chunk * c_chunk), again(2, o.getNumSamples()), lead(2, static_cast<int>(3.3 * c_rate) / c_chunk * c_chunk);
            std::vector<int> gates, gatesAgain;
            auto render = [&](juce::AudioBuffer<float>& into, std::vector<int>* swelling)
            {
                for (int pos = 0; pos < into.getNumSamples(); pos += c_chunk)
                {
                    e->process(nullptr, nullptr, into.getWritePointer(0, pos), into.getWritePointer(1, pos), c_chunk);
                    if (swelling != nullptr)
                        swelling->push_back((e->isAutoSwelling() ? 1 : 0) + 2 * static_cast<int>(1000.0f * e->getCyclePeakIn()));
                }
            };
            // (After a restart is asked for the old sound fades out first: the take starts when the new run does.)
            auto restart = [&]()
            {
                juce::AudioBuffer<float> fade(2, c_chunk);
                e->restartSeeded();
                int n = 0;
                do
                    e->process(nullptr, nullptr, fade.getWritePointer(0), fade.getWritePointer(1), c_chunk);
                while (e->isRestarting() && ++n < 4000);
            };
            render(lead, nullptr);
            restart();
            render(o, &gates);
            render(lead, nullptr);
            restart();
            render(again, &gatesAgain);
            float diff = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < o.getNumSamples(); ++i)
                    diff = std::max(diff, std::abs(o.getSample(ch, i) - again.getSample(ch, i)));
            const char* const name = which == 0 ? "apart" : which == 1 ? "together" : "together, Auto Swell on a loose clock with Chance 50%";
            std::cout << "  two seeded restarts (" << name << ", Chaos 100%): they differ by at most "
                      << juce::Decibels::gainToDecibels(diff, -200.0f) << " dB (level " << juce::Decibels::gainToDecibels(o.getMagnitude(0, o.getNumSamples())) << " dB); the clock and Auto Swell's gate are "
                      << (gates == gatesAgain ? "the same" : "NOT the same") << " chunk by chunk" << std::endl;
            check(gates == gatesAgain, juce::String("a seeded restart starts the clock and Auto Swell from the same place (") + name + ")");
            check(diff <= 1.0e-6f && o.getMagnitude(0, o.getNumSamples()) > 0.01f, juce::String("two seeded restarts sound the same (") + name + ")");
        }
    }

    // ---- auto ----------------------------------------------------------------------------------------------------
    if (wanted("auto"))
    {
        std::cout << "Auto Swell" << std::endl;
        // How far Swell is in (the engine's spring, 0 .. 1): a fader with it against the same fader in a twin
        // engine without it, wherever the twin's is far enough down to tell.
        struct Swells
        {
            std::vector<double> starts, ends; // seconds: Auto Swell's gate opening and closing
            float most = 0.0f;                // the furthest in the spring got
            std::vector<float> atMarkers;     // the spring as each of bus A's passes is at its closest
            int markers = 0;
        };
        auto swells = [&](RigEngine::Params q, double seconds, bool playing, std::function<void(RigEngine::Params&, double)> change = nullptr)
        {
            auto e = std::make_unique<RigEngine>(), twin = std::make_unique<RigEngine>();
            auto plain = q;
            plain.autoSwell.mode = RigEngine::AutoSwellOff;
            plain.swell = false;
            e->setReleased(! playing);
            twin->setReleased(! playing);
            start(*e, four, q);
            start(*twin, four, plain);
            Swells out;
            juce::AudioBuffer<float> scratch(2, c_chunk);
            bool was = false;
            for (int pos = 0; pos < static_cast<int>(seconds * c_rate); pos += c_chunk)
            {
                const double t = pos / c_rate;
                if (change != nullptr)
                {
                    auto now = q;
                    change(now, t);
                    e->setParams(now);
                }
                e->process(nullptr, nullptr, scratch.getWritePointer(0), scratch.getWritePointer(1), c_chunk);
                bool marker = false;
                for (int m = 0; m < e->getNumMarkers(); ++m)
                    marker = marker || e->getMarker(m).kind == RigEngine::MarkPassA;
                twin->process(nullptr, nullptr, scratch.getWritePointer(0), scratch.getWritePointer(1), c_chunk);
                const bool is = e->isAutoSwelling();
                if (is && ! was) out.starts.push_back(t);
                if (! is && was) out.ends.push_back(t);
                was = is;
                float spring = -1.0f;
                for (int track = 0; track < 4; ++track)
                    if (const float under = twin->getRide(track); under < 0.8f)
                        spring = std::max(spring, (e->getRide(track) - under) / (1.0f - under));
                if (spring >= 0.0f)
                    out.most = std::max(out.most, spring);
                if (marker && t > 3.0)
                {
                    ++out.markers;
                    if (spring >= 0.0f)
                        out.atMarkers.push_back(spring);
                }
            }
            return out;
        };
        RigEngine::Params q;
        q.energy = 0.8f;

        {
            const auto none = swells(q, 20.0, true);
            check(none.starts.empty() && none.most < 1.0e-6f, "Auto Swell off: nothing swells");
            // Held from outside the engine (the parameter, the button, MIDI and the wires all arrive as this).
            const auto held = swells(q, 12.0, true, [](RigEngine::Params& now, double t) { now.swell = t >= 4.0 && t < 6.0; });
            std::cout << "  Swell held from outside for 2 s: the spring gets to " << held.most << std::endl;
            check(held.most > 0.95f && held.starts.empty(), "Swell held from outside the engine raises the faders (and is not Auto Swell)");
        }
        {
            auto c = q;
            c.autoSwell.mode = RigEngine::AutoSwellClock;
            c.autoSwell.every = 2.0f;
            c.autoSwell.hold = 0.6f;
            const auto clock = swells(c, 31.0, true);
            double worstStart = 0.0, worstHold = 0.0;
            for (size_t k = 0; k < clock.starts.size(); ++k)
                worstStart = std::max(worstStart, std::abs(clock.starts[k] - 2.0 * static_cast<double>(k + 1)));
            for (size_t k = 0; k < clock.ends.size(); ++k)
                worstHold = std::max(worstHold, std::abs(clock.ends[k] - clock.starts[k] - 0.6));
            std::cout << "  On a Clock, Every 2 s, Hold 0.6 s: " << clock.starts.size() << " swells in 31 s, the first at " << (clock.starts.empty() ? -1.0 : clock.starts[0])
                      << " s; furthest from the clock " << num(1000.0 * worstStart, 2) << " ms, from the hold " << num(1000.0 * worstHold, 2) << " ms; the spring gets to "
                      << clock.most << std::endl;
            check(clock.starts.size() == 15 && worstStart < 0.002 && worstHold < 0.002, "On a Clock: one swell every Every, held for Hold, the first a whole Every after the start");
            check(clock.most >= 0.9f, "On a Clock raises the faders");

            auto half = c;
            half.autoSwell.every = 0.5f;
            half.autoSwell.hold = 0.25f;
            half.autoSwell.chance = 0.5f;
            const auto chance = swells(half, 200.5, true);
            // 400 periods at one half: the standard deviation is 10.
            std::cout << "  Chance 50%, Every 0.5 s: " << chance.starts.size() << " swells in 400 periods (170 .. 230 is three standard deviations)" << std::endl;
            check(chance.starts.size() >= 170 && chance.starts.size() <= 230, "Chance 50%: about half of them fire");
            half.autoSwell.chance = 0.0f;
            check(swells(half, 20.0, true).starts.empty(), "Chance 0%: none fires");

            auto loose = c;
            loose.autoSwell.every = 1.0f;
            loose.autoSwell.hold = 0.25f;
            loose.autoSwell.loose = 0.5f;
            const auto wander = swells(loose, 120.5, true);
            double least = 1.0, mostLate = 0.0, mean = 0.0;
            for (const double s : wander.starts)
            {
                const double late = s - std::floor(s + 1.0e-4);
                least = std::min(least, late);
                mostLate = std::max(mostLate, late);
                mean += late / static_cast<double>(wander.starts.size());
            }
            std::cout << "  Loose 50%, Every 1 s: " << wander.starts.size() << " swells, each " << num(least, 3) << " .. " << num(mostLate, 3) << " s into its period (mean "
                      << num(mean, 3) << " s)" << std::endl;
            check(wander.starts.size() >= 118 && least >= 0.0 && least < 0.03 && mostLate > 0.47 && mostLate <= 0.502 && std::abs(mean - 0.25) < 0.05,
                  "Loose 50%: each swell starts up to half an Every into its period");

            auto still = c;
            still.rideOn = false;
            check(swells(still, 12.0, true).starts.empty(), "it rests while Auto Ride is off");
            still = c;
            still.energy = 0.0f;
            check(swells(still, 12.0, true).starts.empty(), "it rests while Energy is at nothing");
            check(swells(c, 12.0, false).starts.empty(), "it rests while the rig is stopped");
            auto both = c;
            both.rideMode = RigEngine::RideTogether;
            check(swells(both, 12.0, true).starts.size() == 5, "On a Clock runs in Faders Together too");
        }
        {
            auto pk = q;
            pk.autoSwell.mode = RigEngine::AutoSwellPeaks;
            const auto peaks = swells(pk, 60.0, true);
            float least = 2.0f;
            for (const float s : peaks.atMarkers)
                least = std::min(least, s);
            double worstHold = 0.0;
            for (size_t k = 0; k < peaks.ends.size(); ++k)
                worstHold = std::max(worstHold, std::abs(peaks.ends[k] - peaks.starts[k] - 0.6));
            std::cout << "  On Peaks: " << peaks.markers << " passes of bus A at their closest, " << peaks.starts.size() << " swells; the spring could be read at "
                      << peaks.atMarkers.size() << " of them and is at least " << least << " there; held " << num(1000.0 * worstHold, 1) << " ms from Hold at most" << std::endl;
            check(peaks.markers >= 20 && peaks.atMarkers.size() >= 5 && least >= 0.9f, "On Peaks: Swell is at 0.9 or more as each pass is at its closest");
            auto shortHold = pk;
            shortHold.autoSwell.hold = 0.25f;
            const auto brief = swells(shortHold, 60.0, true);
            least = 2.0f;
            for (const float s : brief.atMarkers)
                least = std::min(least, s);
            std::cout << "  On Peaks, Hold 0.25 s: the spring is at least " << least << " as a pass is at its closest (" << brief.atMarkers.size() << " read)" << std::endl;
            check(brief.atMarkers.size() >= 5 && least >= 0.9f, "On Peaks with the shortest Hold still has Swell in as the pass goes by");
            auto halfPk = pk;
            halfPk.autoSwell.chance = 0.5f;
            halfPk.autoSwell.hold = 0.3f;
            halfPk.rate = 1.0f;
            const auto some = swells(halfPk, 120.0, true);
            std::cout << "  On Peaks, Chance 50%: " << some.starts.size() << " swells for " << some.markers << " passes (after the first 3 s)" << std::endl;
            check(some.markers >= 100 && some.starts.size() > static_cast<size_t>(some.markers) / 3 && some.starts.size() < static_cast<size_t>(2 * some.markers) / 3,
                  "On Peaks, Chance 50%: about half the passes get one");
            auto tog = pk;
            tog.rideMode = RigEngine::RideTogether;
            check(swells(tog, 20.0, true).starts.empty(), "On Peaks rests in Faders Together");
        }
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

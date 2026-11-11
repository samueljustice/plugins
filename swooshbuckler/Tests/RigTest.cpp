// Offline test for the whoosh rig engine.
//
//   SwooshbucklerRig <output folder>
//
// Loads synthetic tracks into a CargoHold (a gappy growl on bus A, a wind bed on
// bus B, a hit track), runs the rig for 60 s with auto ride, and checks:
// finite output under the ceiling, a long continuous energy file (no long
// silences), real movement (swells), the hit gate opening at the loudest
// moments only, A ducking B, and speed (faster than real time).

#include <JuceHeader.h>
#include "Cargo.h"
#include "MasterProcessor.h"
#include "RigEngine.h"
#include "DspUtils.h"
#include "Modulators.h"
#include "Parameters.h"

namespace
{
    constexpr double c_rate = 48000.0;
    constexpr int c_block = 256;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }

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

    juce::AudioBuffer<float> growl()
    {
        // Gappy tonal growls: 0.6 s sounds with 0.9 s gaps.
        juce::AudioBuffer<float> b(1, static_cast<int>(c_rate * 6.0));
        b.clear();
        double ph = 0.0;
        juce::Random r(3);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = i / c_rate;
            const double lt = std::fmod(t, 1.5);
            if (lt < 0.6)
            {
                ph += (110.0 + 40.0 * std::sin(6.0 * t)) / c_rate;
                const float env = static_cast<float>(std::sin(juce::MathConstants<double>::pi * lt / 0.6));
                float v = 0.0f;
                for (int h = 1; h < 8; ++h)
                    v += static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * ph * h)) / static_cast<float>(h);
                b.setSample(0, i, 0.4f * env * std::tanh(2.0f * v) + 0.02f * env * (r.nextFloat() * 2.0f - 1.0f));
            }
        }
        return b;
    }

    juce::AudioBuffer<float> windBed()
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_rate * 8.0));
        juce::Random r(5);
        float y = 0.0f;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            y += 0.06f * ((r.nextFloat() * 2.0f - 1.0f) - y);
            const double t = i / c_rate;
            b.setSample(0, i, 2.5f * y * static_cast<float>(0.75 + 0.25 * std::sin(1.3 * t)));
        }
        return b;
    }

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
                                                     + 0.7 * std::exp(-t / 0.2) * std::sin(juce::MathConstants<double>::twoPi * 55.0 * t)));
            }
        return b;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getCurrentWorkingDirectory();
    folder.createDirectory();

    CargoHold hold;
    juce::String error;
    check(hold.loadFile(0, writeWav(folder, "rig_src_growl", growl()), error), "loads the growl track " + error);
    check(hold.loadFile(1, writeWav(folder, "rig_src_wind", windBed()), error), "loads the wind bed " + error);
    check(hold.loadFile(2, writeWav(folder, "rig_src_hits", hits()), error), "loads the hit track " + error);
    // Routing is by type; correct the type and the routing follows.
    hold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
    hold.setRoleOverride(1, static_cast<int>(CargoRole::Bed));
    hold.setRoleOverride(2, static_cast<int>(CargoRole::Impact));
    check(hold.getBus(0) == 0 && hold.getBus(1) == 1 && hold.getBus(2) == CargoHold::c_hitsBus, "routing follows the type");
    std::cout << "  types: " << hold.getDisplayName(0) << " / " << hold.getDisplayName(1) << " / " << hold.getDisplayName(2) << std::endl;

    RigEngine rig;
    rig.setCargoHold(&hold);
    RigEngine::Params p;
    p.hitAim = false; // the gate and sync tests below describe the classic hit gate; AIM has its own test
    rig.setParams(p);
    rig.prepare(c_rate, c_block);
    rig.setParams(p);

    MasterProcessor master;
    master.prepare(c_rate, 2);
    MasterProcessor::Parameters mp;
    mp.inflateEffect = 0.3f;
    mp.limiterOn = true;
    mp.ceilingDb = -0.3f;
    master.setParameters(mp);

    const double seconds = 60.0;
    const int total = static_cast<int>(seconds * c_rate);
    juce::AudioBuffer<float> out(2, total);
    int gateBlocks = 0, blocks = 0;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    for (int pos = 0; pos < total; pos += c_block)
    {
        const int n = juce::jmin(c_block, total - pos);
        rig.setParams(p);
        float* ch[2] = { out.getWritePointer(0, pos), out.getWritePointer(1, pos) };
        rig.process(nullptr, nullptr, ch[0], ch[1], n);
        for (int i = 0; i < n; ++i) { ch[0][i] *= 1.41f; ch[1][i] *= 1.41f; }
        master.process(ch, 2, n);
        ++blocks;
        if (rig.getHitGate() > 0.5f) ++gateBlocks;
    }
    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
    writeWav(folder, "rig_energy_60s", out);

    // Analyse in 100 ms windows (skip the first 2 s)
    const int win = static_cast<int>(0.1 * c_rate);
    std::vector<float> rmsDb;
    bool finite = true;
    float peak = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < total; ++i)
        {
            const float v = out.getSample(ch, i);
            finite = finite && std::isfinite(v);
            peak = juce::jmax(peak, std::abs(v));
        }
    for (int w = static_cast<int>(2.0 * c_rate); w + win <= total; w += win)
    {
        double s = 0.0;
        for (int i = w; i < w + win; ++i)
            s += 0.5 * (out.getSample(0, i) * out.getSample(0, i) + out.getSample(1, i) * out.getSample(1, i));
        rmsDb.push_back(juce::Decibels::gainToDecibels(static_cast<float>(std::sqrt(s / win)), -120.0f));
    }
    int silentRun = 0, longestSilence = 0;
    for (auto v : rmsDb)
    {
        silentRun = v < -60.0f ? silentRun + 1 : 0;
        longestSilence = juce::jmax(longestSilence, silentRun);
    }
    auto sorted = rmsDb;
    std::sort(sorted.begin(), sorted.end());
    const float p10 = sorted[sorted.size() / 10], p90 = sorted[sorted.size() * 9 / 10];
    const double realtime = seconds * 1000.0 / juce::jmax(1.0, ms);

    std::cout << "rig: peak " << juce::Decibels::gainToDecibels(peak) << " dBFS, 10th/90th percentile "
              << p10 << " / " << p90 << " dB, longest silence " << longestSilence * 0.1 << " s, gate open "
              << (100.0 * gateBlocks / blocks) << "% of the time, " << realtime << "x realtime" << std::endl;

    check(finite, "output is finite");
    check(peak <= juce::Decibels::decibelsToGain(-0.29f), "peak under the -0.3 dB ceiling");
    check(longestSilence * 0.1 < 1.0, "a continuous energy file (no silence longer than 1 s)");
    check(p90 - p10 > 8.0f, "real movement: at least 8 dB between quiet and loud moments (auto ride swells)");
    check(gateBlocks > 0 && gateBlocks < blocks / 3, "hit gate opens, but only at the loudest moments");
    check(realtime > 8.0, "renders faster than 8x real time");

    // Without auto ride the movement should be much smaller.
    {
        RigEngine flat;
        flat.setCargoHold(&hold);
        auto q = p;
        q.rideOn = false;
        q.buses[0].doppler = q.buses[1].doppler = 0.0f;
        flat.setParams(q);
        flat.prepare(c_rate, c_block);
        flat.setParams(q);
        juce::AudioBuffer<float> o(2, static_cast<int>(20.0 * c_rate));
        for (int pos = 0; pos < o.getNumSamples(); pos += c_block)
        {
            const int n = juce::jmin(c_block, o.getNumSamples() - pos);
            flat.process(nullptr, nullptr, o.getWritePointer(0, pos), o.getWritePointer(1, pos), n);
        }
        std::vector<float> db;
        for (int w = static_cast<int>(2.0 * c_rate); w + win <= o.getNumSamples(); w += win)
            db.push_back(juce::Decibels::gainToDecibels(o.getRMSLevel(0, w, win), -120.0f));
        std::sort(db.begin(), db.end());
        const float spread = db[db.size() * 9 / 10] - db[db.size() / 10];
        std::cout << "  ride off spread " << spread << " dB vs ride on " << (p90 - p10) << " dB" << std::endl;
        check(spread < p90 - p10, "auto ride adds movement");
    }

    // Hit sync: three hit tracks with different timing, one shared cue: their loudest moments line up.
    {
        CargoHold hits3;
        const double offsets[3] = { 0.05, 0.42, 0.17 };
        for (int t = 0; t < 3; ++t)
        {
            juce::AudioBuffer<float> b(1, static_cast<int>(c_rate * 3.0));
            b.clear();
            juce::Random r(11 + t);
            for (int k = 0; k < 3; ++k)
            {
                const int start = static_cast<int>((offsets[t] + k * 0.9) * c_rate);
                for (int i = 0; i < static_cast<int>(0.25 * c_rate) && start + i < b.getNumSamples(); ++i)
                {
                    const double tt = i / c_rate;
                    // a short rise (different per track) into a sharp transient
                    const double rise = 0.02 + 0.03 * t;
                    const double env = tt < rise ? tt / rise : std::exp(-(tt - rise) / 0.05);
                    b.setSample(0, start + i, static_cast<float>(0.8 * env * (r.nextFloat() * 2.0f - 1.0f)));
                }
            }
            check(hits3.loadFile(t, writeWav(folder, "rig_src_hit" + juce::String(t), b), error), "loads hit track " + juce::String(t + 1));
            hits3.setRoleOverride(t, static_cast<int>(CargoRole::Impact));
        }

        double maxPre = 0.0;
        for (int t = 0; t < 3; ++t)
            for (const auto& r : hits3.get(t)->regions)
                maxPre = std::max(maxPre, (r.focus - r.start) / c_rate);

        std::array<CargoPlayer, 3> players;
        std::array<std::vector<float>, 3> outs;
        sj::Random rng;
        const int total = static_cast<int>(1.5 * c_rate), chunk = 32;
        for (int t = 0; t < 3; ++t)
        {
            players[static_cast<size_t>(t)].prepare(c_rate);
            outs[static_cast<size_t>(t)].assign(static_cast<size_t>(total), 0.0f);
        }
        for (int pos = 0; pos < total; pos += chunk)
        {
            CargoCue cue;
            cue.active = pos == 0;
            cue.secondsToPeak = maxPre;
            cue.flightSeconds = 1.0;
            for (int t = 0; t < 3; ++t)
                players[static_cast<size_t>(t)].render(hits3.getLive(t), CargoRole::Impact, true, 1.0f, 0.0f, rng, 0.15f, cue,
                                                       outs[static_cast<size_t>(t)].data() + pos, juce::jmin(chunk, total - pos));
        }
        std::array<double, 3> peakAt {};
        for (int t = 0; t < 3; ++t)
        {
            const auto& o = outs[static_cast<size_t>(t)];
            size_t best = 0;
            float bestV = 0.0f, env = 0.0f;
            for (size_t i = 0; i < o.size(); ++i)
            {
                env = std::max(std::abs(o[i]), env * 0.999f);
                if (env > bestV) { bestV = env; best = i; }
            }
            peakAt[static_cast<size_t>(t)] = static_cast<double>(best) / c_rate;
        }
        const double spread = *std::max_element(peakAt.begin(), peakAt.end()) - *std::min_element(peakAt.begin(), peakAt.end());
        std::cout << "  synced hit peaks at " << peakAt[0] << " / " << peakAt[1] << " / " << peakAt[2] << " s (spread " << spread * 1000.0 << " ms)" << std::endl;
        check(spread < 0.025, "hit sync: three hit tracks peak within 25 ms of each other");

        // Skip silence OFF on one hit track: the whole file is one hit, lined up on its first transient; the shared
        // beat is still sized by sounds, not by the whole 3 s file.
        {
            hits3.setSkipSilence(1, false);
            HitWindow win;
            for (int t = 0; t < 3; ++t)
                win.extend(*hits3.get(t), hits3.getSkipSilence(t), hits3.getStart(t));
            std::cout << "  hit window with skip off on track 2: pre " << win.pre << " s, post " << win.post << " s" << std::endl;
            check(win.post < 0.6, "hit sync: skip silence off does not stretch the shared beat to the whole file");

            std::array<CargoPlayer, 3> ps;
            std::array<std::vector<float>, 3> os;
            for (int t = 0; t < 3; ++t)
            {
                ps[static_cast<size_t>(t)].prepare(c_rate);
                os[static_cast<size_t>(t)].assign(static_cast<size_t>(total), 0.0f);
            }
            for (int pos = 0; pos < total; pos += chunk)
            {
                CargoCue cue;
                cue.active = pos == 0;
                cue.secondsToPeak = win.pre;
                cue.flightSeconds = 1.0;
                for (int t = 0; t < 3; ++t)
                    ps[static_cast<size_t>(t)].render(hits3.getLive(t), CargoRole::Impact, hits3.getSkipSilence(t), 1.0f, 0.0f, rng, 0.15f,
                                                      cue, os[static_cast<size_t>(t)].data() + pos, juce::jmin(chunk, total - pos));
            }
            // Only the first hit counts (the whole-file track carries on into its later sounds).
            const size_t searchEnd = static_cast<size_t>((win.pre + 0.2) * c_rate);
            std::array<double, 3> at {};
            for (int t = 0; t < 3; ++t)
            {
                const auto& o = os[static_cast<size_t>(t)];
                size_t best = 0;
                float bestV = 0.0f, env = 0.0f;
                for (size_t i = 0; i < std::min(o.size(), searchEnd); ++i)
                {
                    env = std::max(std::abs(o[i]), env * 0.999f);
                    if (env > bestV) { bestV = env; best = i; }
                }
                at[static_cast<size_t>(t)] = static_cast<double>(best) / c_rate;
            }
            const double sp = *std::max_element(at.begin(), at.end()) - *std::min_element(at.begin(), at.end());
            std::cout << "  skip-off synced peaks at " << at[0] << " / " << at[1] << " / " << at[2] << " s (spread " << sp * 1000.0 << " ms)" << std::endl;
            check(sp < 0.025, "hit sync with skip silence off: still peaks within 25 ms");
            hits3.setSkipSilence(1, true);
        }

        // And through the whole rig with sync on: still finite and audible.
        RigEngine rs;
        rs.setCargoHold(&hits3);
        auto q = p;
        q.hitSync = true;
        q.hitSense = 1.0f;
        rs.setParams(q);
        rs.prepare(c_rate, c_block);
        juce::AudioBuffer<float> o(2, static_cast<int>(6.0 * c_rate));
        for (int pos = 0; pos < o.getNumSamples(); pos += c_block)
            rs.process(nullptr, nullptr, o.getWritePointer(0, pos), o.getWritePointer(1, pos), juce::jmin(c_block, o.getNumSamples() - pos));
        bool finite = true;
        for (int i = 0; i < o.getNumSamples(); ++i)
            finite = finite && std::isfinite(o.getSample(0, i));
        check(finite, "hit sync through the rig: finite output");
    }

    // AIM: the hits land on pass peaks. Every time a cued hit is heard, some bus pass should be at its middle (its
    // loudest, closest moment), and Sensitivity should set how many peaks get a hit.
    {
        auto aimRun = [&](float sense, int& beats, int& onPeak)
        {
            RigEngine e;
            e.setCargoHold(&hold);
            auto q = p;
            q.hitAim = true;
            q.hitSense = sense;
            e.setParams(q);
            e.prepare(c_rate, c_block);
            e.setParams(q);
            juce::AudioBuffer<float> o(2, c_block);
            beats = onPeak = 0;
            int last = e.getHitBeats();
            for (int blk = 0; blk < static_cast<int>(40.0 * c_rate) / c_block; ++blk)
            {
                e.process(nullptr, nullptr, o.getWritePointer(0), o.getWritePointer(1), c_block);
                if (e.getHitBeats() == last)
                    continue;
                last = e.getHitBeats();
                ++beats;
                for (int f = 0; f < RigEngine::c_maxFlights; ++f)
                    if (std::abs(e.getFlightView(f).age - 0.5f) < 0.02f)
                    {
                        ++onPeak;
                        break;
                    }
            }
        };
        int beatsHi = 0, peakHi = 0, beatsLo = 0, peakLo = 0;
        aimRun(1.0f, beatsHi, peakHi);
        aimRun(0.2f, beatsLo, peakLo);
        std::cout << "  aim: " << beatsHi << " hits at sensitivity 100% (" << peakHi << " on a pass peak), "
                  << beatsLo << " at 20% (" << peakLo << " on a peak)" << std::endl;
        check(beatsHi >= 8 && peakHi >= beatsHi * 9 / 10, "aim: hits land on pass peaks");
        check(beatsLo < beatsHi, "aim: lower Sensitivity keeps only the bigger peaks");
    }

    // Peak tremolo: with Depth up the level flutters at the peaks; at 0 it's off.
    {
        auto flutter = [&](float depth, int target)
        {
            RigEngine e;
            e.setCargoHold(&hold);
            auto q = p;
            q.tremOn = depth > 0.0f;
            q.tremDepth = depth;
            q.tremThreshold = 0.0f;
            q.tremShape = 1; // square: easy to see
            q.tremRate = 0.4f;
            q.tremTarget = target;
            q.space = 0.0f;
            e.setParams(q);
            e.prepare(c_rate, c_block);
            e.setParams(q);
            juce::AudioBuffer<float> o(2, static_cast<int>(10.0 * c_rate));
            for (int pos = 0; pos < o.getNumSamples(); pos += c_block)
                e.process(nullptr, nullptr, o.getWritePointer(0, pos), o.getWritePointer(1, pos), juce::jmin(c_block, o.getNumSamples() - pos));
            // Within each tremolo cycle: the range (dB) between the loudest and quietest 5 ms window. The median over
            // all cycles: steady material changes a little, a deep tremolo dips hard every cycle.
            const double hz = 1.5 * std::pow(20.0, 0.4);
            const int cycle = static_cast<int>(c_rate / hz), w = static_cast<int>(0.005 * c_rate);
            std::vector<float> ranges;
            for (int c0 = static_cast<int>(2.0 * c_rate); c0 + cycle < o.getNumSamples(); c0 += cycle)
            {
                float lo = 1000.0f, hi = -1000.0f;
                for (int i = c0; i + w <= c0 + cycle; i += w)
                {
                    const float db = juce::Decibels::gainToDecibels(o.getRMSLevel(0, i, w), -80.0f);
                    lo = std::min(lo, db);
                    hi = std::max(hi, db);
                }
                if (hi > -70.0f)
                    ranges.push_back(hi - lo);
            }
            std::sort(ranges.begin(), ranges.end());
            return ranges.empty() ? 0.0 : static_cast<double>(ranges[ranges.size() / 2]);
        };
        const double off = flutter(0.0f, 0), everything = flutter(1.0f, 0), buses = flutter(1.0f, 1);
        std::cout << "  tremolo: level range within a cycle " << off << " dB off, " << everything << " dB on everything, "
                  << buses << " dB on the buses" << std::endl;
        check(everything > off + 10.0 && buses > off + 4.0, // the hits stay steady with only the buses shaking
               "peak tremolo shakes the level, on everything or the buses");
    }

    // Linked buses: bus B flies bus A's passes (same moment, position, closeness, direction); unlinked they differ.
    {
        auto passGap = [&](bool link)
        {
            RigEngine e;
            e.setCargoHold(&hold);
            auto q = p;
            q.linkBuses = link;
            q.chaos = 0.8f; // plenty of variety between passes
            e.setParams(q);
            e.prepare(c_rate, c_block);
            e.setParams(q);
            juce::AudioBuffer<float> o(2, c_block);
            float worst = 0.0f;
            for (int blk = 0; blk < static_cast<int>(20.0 * c_rate) / c_block; ++blk)
            {
                e.process(nullptr, nullptr, o.getWritePointer(0), o.getWritePointer(1), c_block);
                for (int k = 0; k < 2; ++k)
                {
                    const auto a = e.getFlightView(k), b = e.getFlightView(2 + k);
                    worst = std::max({ worst, std::abs(a.x - b.x), std::abs(a.closeness - b.closeness), std::abs(a.age - b.age) });
                }
            }
            return worst;
        };
        const float linked = passGap(true), unlinked = passGap(false);
        std::cout << "  bus passes A vs B: linked gap " << linked << ", unlinked gap " << unlinked << std::endl;
        check(linked < 1.0e-4f, "linked buses: B's passes match A's over 20 s (position, closeness, timing)");
        check(unlinked > 0.05f, "unlinked buses: each bus does its own passes");
    }

    // Turning LINK on and off mid-pass doesn't click: B fades out, lines up with A, and fades back in.
    // Two steady sine tones (one per bus, no hits) so any jump in the delay shows as a spike in the
    // waveform's second difference, far above anything a smoothly moving sine makes.
    {
        auto tone = [](double hz)
        {
            juce::AudioBuffer<float> b(1, static_cast<int>(4.0 * c_rate));
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample(0, i, 0.5f * std::sin(static_cast<float>(juce::MathConstants<double>::twoPi * hz * i / c_rate)));
            return b;
        };
        CargoHold tones;
        juce::String err;
        tones.loadFile(0, writeWav(folder, "rig_src_toneA", tone(220.0)), err);
        tones.loadFile(1, writeWav(folder, "rig_src_toneB", tone(330.0)), err);
        tones.setRoleOverride(0, static_cast<int>(CargoRole::Tonal)); // bus A
        tones.setRoleOverride(1, static_cast<int>(CargoRole::Bed));   // bus B
        tones.setSkipSilence(0, false);
        tones.setSkipSilence(1, false);

        RigEngine e;
        e.setCargoHold(&tones);
        auto q = p;
        q.chaos = 0.8f;
        q.rideOn = false;   // steady levels
        q.fill = 0.0f;
        q.space = 0.0f;
        for (auto& b : q.buses) { b.swirl = 0.0f; b.drive = 0.0f; b.low = 0.0f; b.filter = 0.0f; }
        e.setParams(q);
        e.prepare(c_rate, c_block);
        e.setParams(q);
        juce::AudioBuffer<float> o(2, static_cast<int>(9.0 * c_rate));
        const int switchOn = static_cast<int>(3.0 * c_rate) / c_block * c_block;
        const int switchOff = static_cast<int>(6.0 * c_rate) / c_block * c_block;
        for (int pos = 0; pos < o.getNumSamples(); pos += c_block)
        {
            if (pos == switchOn) { q.linkBuses = true; e.setParams(q); }
            if (pos == switchOff) { q.linkBuses = false; e.setParams(q); }
            e.process(nullptr, nullptr, o.getWritePointer(0, pos), o.getWritePointer(1, pos), juce::jmin(c_block, o.getNumSamples() - pos));
        }
        auto maxCurve = [&](int from, int to)
        {
            float m = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = from + 2; i < to; ++i)
                    m = std::max(m, std::abs(o.getSample(ch, i) - 2.0f * o.getSample(ch, i - 1) + o.getSample(ch, i - 2)));
            return m;
        };
        const float normal = maxCurve(static_cast<int>(1.0 * c_rate), switchOn);
        const float around = std::max(maxCurve(switchOn, switchOn + static_cast<int>(0.2 * c_rate)),
                                      maxCurve(switchOff, switchOff + static_cast<int>(0.2 * c_rate)));
        std::cout << "  link switch: sharpest change " << around << " vs " << normal << " in normal play" << std::endl;
        check(around <= 2.0f * normal, "turning LINK on and off mid-pass doesn't click");
    }

    // Per-track pitch: VARI on changes the track's speed, VARI off keeps it (through the pitch shifter).
    {
        auto run = [&](float semis, bool vari, float& speed, float& rms, bool& finite)
        {
            RigEngine e;
            e.setCargoHold(&hold);
            auto q = p;
            q.trackPitch[1] = semis;
            q.trackVari[1] = vari;
            e.setParams(q);
            e.prepare(c_rate, c_block);
            e.setParams(q);
            juce::AudioBuffer<float> o(2, static_cast<int>(4.0 * c_rate));
            for (int pos = 0; pos < o.getNumSamples(); pos += c_block)
                e.process(nullptr, nullptr, o.getWritePointer(0, pos), o.getWritePointer(1, pos), juce::jmin(c_block, o.getNumSamples() - pos));
            speed = e.getTrackSpeed(1);
            rms = o.getRMSLevel(0, static_cast<int>(1.0 * c_rate), static_cast<int>(3.0 * c_rate));
            finite = true;
            for (int i = 0; i < o.getNumSamples(); ++i)
                finite = finite && std::isfinite(o.getSample(0, i)) && std::isfinite(o.getSample(1, i));
        };
        float sUp = 0, rUp = 0, sKeep = 0, rKeep = 0, sFlat = 0, rFlat = 0;
        bool fUp = false, fKeep = false, fFlat = false;
        run(12.0f, true, sUp, rUp, fUp);
        run(7.25f, false, sKeep, rKeep, fKeep);
        run(0.0f, true, sFlat, rFlat, fFlat);
        std::cout << "  track pitch: VARI +12 speed " << sUp << ", VARI off +7.25 speed " << sKeep
                  << ", rms " << rFlat << " / " << rUp << " / " << rKeep << std::endl;
        check(std::abs(sUp - 2.0f) < 0.01f && std::abs(sKeep - 1.0f) < 1.0e-6f && std::abs(sFlat - 1.0f) < 1.0e-6f,
              "track pitch: VARI changes the track's speed (+12 st = twice as fast), VARI off keeps it");
        check(fUp && fKeep && fFlat && rUp > 1.0e-4f && rKeep > 1.0e-4f, "track pitch: both modes play, finite");
    }

    // Morph between the buses: finite, audible, and the wander-style X sweep doesn't blow up.
    {
        RigEngine m;
        m.setCargoHold(&hold);
        auto q = p;
        q.morphOn = true;
        m.setParams(q);
        m.prepare(c_rate, c_block);
        juce::AudioBuffer<float> o(2, static_cast<int>(10.0 * c_rate));
        bool ok = true;
        for (int pos = 0, k = 0; pos < o.getNumSamples(); pos += c_block, ++k)
        {
            q.morphX = 0.5f + 0.5f * std::sin(0.01f * static_cast<float>(k));
            q.morphMode = (k / 400) % 3;
            m.setParams(q);
            const int n = juce::jmin(c_block, o.getNumSamples() - pos);
            m.process(nullptr, nullptr, o.getWritePointer(0, pos), o.getWritePointer(1, pos), n);
        }
        for (int i = 0; i < o.getNumSamples(); ++i)
            ok = ok && std::isfinite(o.getSample(0, i)) && std::isfinite(o.getSample(1, i));
        const float rms = o.getRMSLevel(0, static_cast<int>(2.0 * c_rate), static_cast<int>(8.0 * c_rate));
        std::cout << "  morph rms " << juce::Decibels::gainToDecibels(rms) << " dBFS" << std::endl;
        check(ok && rms > juce::Decibels::decibelsToGain(-50.0f), "morph between the buses: finite and audible through all three modes");
    }

    // Gate and duck options: every combination renders finite and audible; Open really opens; Floor lets hits through.
    {
        bool ok = true;
        float openGate = 0.0f;
        for (int gm = 0; gm < 4; ++gm)
            for (int dm = 0; dm < 3; ++dm)
            {
                RigEngine e;
                e.setCargoHold(&hold);
                auto q = p;
                q.gateMode = gm;
                q.duckMode = dm;
                q.duckReturn = dm == 1 ? 0.6f : 0.0f;
                q.takeTurns = dm == 2;
                q.hitFloor = gm == 1 ? 0.5f : 0.0f;
                e.setParams(q);
                e.prepare(c_rate, c_block);
                juce::AudioBuffer<float> o(2, static_cast<int>(4.0 * c_rate));
                for (int pos = 0; pos < o.getNumSamples(); pos += c_block)
                {
                    e.process(nullptr, nullptr, o.getWritePointer(0, pos), o.getWritePointer(1, pos), juce::jmin(c_block, o.getNumSamples() - pos));
                    if (gm == 3) openGate = e.getHitGate();
                }
                bool finite = true;
                for (int i = 0; i < o.getNumSamples(); ++i)
                    finite = finite && std::isfinite(o.getSample(0, i)) && std::isfinite(o.getSample(1, i));
                const float rms = o.getRMSLevel(0, static_cast<int>(1.0 * c_rate), static_cast<int>(3.0 * c_rate));
                if (! finite || rms < juce::Decibels::decibelsToGain(-60.0f))
                {
                    std::cout << "  gate mode " << gm << " duck mode " << dm << ": finite " << finite << " rms " << rms << std::endl;
                    ok = false;
                }
            }
        check(ok, "gate modes x duck modes (+ return, take turns, floor): finite and audible");
        check(openGate > 0.95f, "gate mode Open: the gate stays open");
    }

    // Modulation wires: connect, depth, macro maths, LFO range, save / restore, disconnect.
    {
        ModMatrix mods;
        mods.prepare(c_rate);
        check(ModMatrix::targetIndex(ParamIDs::morphX) < 0, "mods: the morph pad is not a wire target");
        check(ModMatrix::targetIndex(ParamIDs::cargoTrack[0]) < 0, "mods: track faders are not wire targets (the ride owns them)");
        const int tFill = ModMatrix::targetIndex(ParamIDs::fill), tDop = ModMatrix::targetIndex(ParamIDs::bus[0][ParamIDs::Doppler]);
        check(tFill >= 0 && tDop >= 0, "mods: knobs are targets");
        const int macroWire = mods.connect(3, ParamIDs::fill, 0.5f);
        const int lfoWire = mods.connect(0, ParamIDs::bus[0][ParamIDs::Doppler], 1.0f);
        check(macroWire >= 0 && lfoWire >= 0 && mods.connect(3, ParamIDs::fill, 0.2f) == macroWire, "mods: wiring the same pair twice keeps one wire");
        check(mods.connect(0, "nonsense", 0.5f) < 0, "mods: unknown parameters are refused");

        std::array<ModMatrix::SourceSettings, ModMatrix::c_numSources> set;
        set[0] = { ModMatrix::Type::Lfo, ModMatrix::Shape::Sine, 0.6f };
        set[3] = { ModMatrix::Type::Macro, ModMatrix::Shape::Sine, 0.8f };
        float lo = 1.0f, hi = -1.0f;
        for (int k = 0; k < 2000; ++k)
        {
            mods.process(set, 256, ModMatrix::FollowerInputs { 0.1f, 0.1f, 0.1f, 0.1f });
            lo = std::min(lo, mods.offset(tDop));
            hi = std::max(hi, mods.offset(tDop));
        }
        std::cout << "  macro offset " << mods.offset(tFill) << ", LFO offset " << lo << " .. " << hi << std::endl;
        check(std::abs(mods.offset(tFill) - 0.4f) < 0.01f, "mods: macro 80% x depth 50% = +0.4");
        check(lo < -0.45f && hi > 0.45f && lo >= -0.5f && hi <= 0.5f, "mods: a 100% LFO wire swings +-0.5 around the knob");

        const auto saved = mods.toValueTree();
        ModMatrix restored;
        restored.fromValueTree(saved);
        check(restored.getRoutes().size() == 2, "mods: wires survive save / restore");
        mods.disconnect(macroWire);
        for (int k = 0; k < 200; ++k)
            mods.process(set, 256, ModMatrix::FollowerInputs { 0.1f, 0.1f, 0.1f, 0.1f });
        check(std::abs(mods.offset(tFill)) < 1.0e-4f, "mods: a pulled wire glides back to zero");

        // Followers listen to the input they're set to: bus B loud, the output silent.
        set[1] = { ModMatrix::Type::Follower, ModMatrix::Shape::Sine, 0.8f, ModMatrix::FollowBusB };
        set[2] = { ModMatrix::Type::Follower, ModMatrix::Shape::Sine, 0.8f, ModMatrix::FollowOutput };
        for (int k = 0; k < 400; ++k)
            mods.process(set, 256, ModMatrix::FollowerInputs { 0.0f, 0.5f, 0.0f, 0.0f } /* bus A, bus B, hits, output */);
        std::cout << "  follower on bus B " << mods.getSourceValue(1) << ", on the output " << mods.getSourceValue(2) << std::endl;
        check(mods.getSourceValue(1) > 0.8f && mods.getSourceValue(2) < 0.05f, "mods: a follower follows the input it's set to");
    }

    // ---- The bus engine: clicks, timing and small DSP fixes -------------------------------------------------------
    {
        const double twoPi = juce::MathConstants<double>::twoPi;
        auto makeTone = [&](double hz, double length, float gain)
        {
            juce::AudioBuffer<float> b(1, static_cast<int>(length * c_rate));
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample(0, i, gain * static_cast<float>(std::sin(twoPi * hz * i / c_rate)));
            return b;
        };
        auto renderRig = [&](RigEngine& e, juce::AudioBuffer<float>& o, int from, int to, int block)
        {
            for (int pos = from; pos < to; pos += block)
                e.process(nullptr, nullptr, o.getWritePointer(0, pos), o.getWritePointer(1, pos), juce::jmin(block, to - pos));
        };
        // A bus on its own: nothing but the doppler passes unless a test turns something on.
        auto bare = [&]()
        {
            auto q = p;
            q.rideOn = false;
            q.fill = 0.0f;
            q.space = 0.0f;
            q.chaos = 0.0f;
            for (auto& b : q.buses) { b.doppler = 0.0f; b.bend = 0.0f; b.width = 0.0f; b.swirl = 0.0f; b.drive = 0.0f; b.low = 0.0f; b.filter = 0.0f; b.duck = 0.0f; b.key = 0.0f; }
            return q;
        };
        auto start = [&](RigEngine& e, CargoHold& h, const RigEngine::Params& q)
        {
            e.setCargoHold(&h);
            e.setParams(q);
            e.prepare(c_rate, c_block);
            e.setParams(q);
        };
        // Power of one frequency (Hann-windowed frames, summed), for telling which pitches a render holds.
        auto powerAt = [&](const juce::AudioBuffer<float>& o, double hz, int from, int to)
        {
            const int n = 8192;
            double sum = 0.0;
            for (int at = from; at + n <= to; at += n / 2)
            {
                double re = 0.0, im = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    const double w = 0.5 - 0.5 * std::cos(twoPi * i / n);
                    const double x = w * 0.5 * (o.getSample(0, at + i) + o.getSample(1, at + i));
                    re += x * std::cos(twoPi * hz * i / c_rate);
                    im += x * std::sin(twoPi * hz * i / c_rate);
                }
                sum += re * re + im * im;
            }
            return sum;
        };

        CargoHold toneHold;
        juce::String err;
        toneHold.loadFile(0, writeWav(folder, "rig_src_tone1320", makeTone(1320.0, 4.0, 0.5f)), err);
        toneHold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal)); // bus A
        toneHold.setSkipSilence(0, false);

        // Swirl doesn't click at the engine's 32-sample steps. A steady sine has a second difference that an
        // "annihilating" filter removes exactly; what is left are the moments the sound changes. With the flanger
        // delay and the phaser tuning ramped, those are spread evenly; stepped, they pile up on the step edges.
        for (float swirl : { 0.3f, 1.0f })
        {
            RigEngine e;
            auto q = bare();
            q.rideOn = true; // the level has to move for Swirl to sweep
            q.energy = 0.7f;
            q.rate = 0.8f;
            q.chaos = 0.4f;
            q.buses[0].swirl = swirl;
            start(e, toneHold, q);
            juce::AudioBuffer<float> o(2, static_cast<int>(12.0 * c_rate) / c_block * c_block);
            renderRig(e, o, 0, o.getNumSamples(), c_block);
            const float c = 2.0f * std::cos(static_cast<float>(twoPi * 1320.0 / c_rate));
            const int from = static_cast<int>(2.0 * c_rate);
            std::vector<std::pair<float, int>> spikes;
            spikes.reserve(static_cast<size_t>(o.getNumSamples()));
            const float* x = o.getReadPointer(0);
            for (int i = from; i < o.getNumSamples(); ++i)
                spikes.push_back({ std::abs(x[i] - c * x[i - 1] + x[i - 2]), i });
            std::partial_sort(spikes.begin(), spikes.begin() + 400, spikes.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::array<int, 32> hist {};
            for (int k = 0; k < 400; ++k)
                ++hist[static_cast<size_t>(spikes[static_cast<size_t>(k)].second % 32)];
            int most = 0;
            for (int k = 0; k < 32; ++k)
                most = std::max(most, hist[static_cast<size_t>(k)] + hist[static_cast<size_t>((k + 1) % 32)]);
            std::cout << "  swirl " << swirl << ": " << most << " of the 400 sharpest changes fall on the same two samples of the 32-sample step" << std::endl;
            check(most < 80, "swirl " + juce::String(static_cast<int>(swirl * 100.0f)) + "%: no click on every 32-sample step (the sweep is ramped)");
        }

        // The doppler pitch is a real pass-by: up by 1 / (1 - speed) on the way in and down by 1 / (1 + speed) on the
        // way out (the rise is the bigger of the two). At Doppler 100%, Bend 100% the speed is 0.42 of sound, so a
        // 330 Hz tone is heard at 569 Hz coming and 232 Hz going, not at 469 Hz and 191 Hz.
        {
            CargoHold h330;
            h330.loadFile(0, writeWav(folder, "rig_src_tone330", makeTone(330.0, 4.0, 0.5f)), err);
            h330.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
            h330.setSkipSilence(0, false);
            RigEngine e;
            auto q = bare();
            q.buses[0].doppler = 1.0f;
            q.buses[0].bend = 1.0f;
            start(e, h330, q);
            juce::AudioBuffer<float> o(2, static_cast<int>(20.0 * c_rate) / c_block * c_block);
            renderRig(e, o, 0, o.getNumSamples(), c_block);
            const int from = static_cast<int>(1.0 * c_rate), to = o.getNumSamples();
            const double rising = powerAt(o, 330.0 / (1.0 - 0.42), from, to), falling = powerAt(o, 330.0 / (1.0 + 0.42), from, to);
            const double wrongIn = powerAt(o, 330.0 * 1.42, from, to), wrongOut = powerAt(o, 330.0 * 0.58, from, to);
            std::cout << "  doppler pitch: power at 569 / 232 Hz " << juce::Decibels::gainToDecibels(std::sqrt(rising)) << " / " << juce::Decibels::gainToDecibels(std::sqrt(falling))
                      << " dB, at 469 / 191 Hz " << juce::Decibels::gainToDecibels(std::sqrt(wrongIn)) << " / " << juce::Decibels::gainToDecibels(std::sqrt(wrongOut)) << " dB" << std::endl;
            check(rising > 30.0 * wrongIn && falling > 30.0 * wrongOut, "doppler: the pitch rises by 1/(1 - speed) coming in and falls by 1/(1 + speed) going out");

            // And the passes hand over without a step in the delay (the second head is at its loudest when the
            // first one starts a new pass), at any Chaos. A smoothly bent sine never curves this sharply.
            RigEngine wild;
            q.chaos = 1.0f;
            start(wild, h330, q);
            renderRig(wild, o, 0, o.getNumSamples(), c_block);
            float sharpest = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = from + 2; i < to; ++i)
                    sharpest = std::max(sharpest, std::abs(o.getSample(ch, i) - 2.0f * o.getSample(ch, i - 1) + o.getSample(ch, i - 2)));
            std::cout << "  doppler passes at Chaos 100%: sharpest change " << sharpest << std::endl;
            check(sharpest < 0.012f, "doppler: passes start and hand over without a click");
        }

        // The two passes of a bus stay exactly half a pass apart at any Chaos (so their power adds up to a steady level).
        {
            RigEngine e;
            auto q = p;
            q.chaos = 1.0f;
            start(e, hold, q);
            juce::AudioBuffer<float> o(2, c_block);
            float worst = 0.0f;
            for (int blk = 0; blk < static_cast<int>(40.0 * c_rate) / c_block; ++blk)
            {
                e.process(nullptr, nullptr, o.getWritePointer(0), o.getWritePointer(1), c_block);
                for (int b = 0; b < 2; ++b)
                {
                    float gap = e.getFlightView(b * 2 + 1).age - e.getFlightView(b * 2).age;
                    gap -= std::floor(gap);
                    worst = std::max(worst, std::abs(gap - 0.5f));
                }
            }
            std::cout << "  two passes at Chaos 100%: furthest from half a pass apart " << worst << std::endl;
            check(worst < 1.0e-3f, "the two passes of a bus stay half a pass apart at any Chaos");
        }

        // Bend changes the pitch only: the swell (level over time) of a hiss is the same at Bend 0% and 100%.
        {
            juce::AudioBuffer<float> hiss(1, static_cast<int>(5.0 * c_rate));
            juce::Random r(17);
            for (int i = 0; i < hiss.getNumSamples(); ++i)
                hiss.setSample(0, i, 0.3f * (r.nextFloat() * 2.0f - 1.0f));
            CargoHold hissHold;
            hissHold.loadFile(0, writeWav(folder, "rig_src_hiss", hiss), err);
            hissHold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
            hissHold.setSkipSilence(0, false);
            auto envelope = [&](float bend)
            {
                RigEngine e;
                auto q = bare();
                q.buses[0].doppler = 1.0f;
                q.buses[0].bend = bend;
                q.chaos = 0.4f;
                start(e, hissHold, q);
                juce::AudioBuffer<float> o(2, static_cast<int>(24.0 * c_rate) / c_block * c_block);
                renderRig(e, o, 0, o.getNumSamples(), c_block);
                std::vector<float> db;
                const int w = static_cast<int>(0.1 * c_rate);
                for (int at = static_cast<int>(2.0 * c_rate); at + w <= o.getNumSamples(); at += w)
                    db.push_back(juce::Decibels::gainToDecibels(0.5f * (o.getRMSLevel(0, at, w) + o.getRMSLevel(1, at, w)), -100.0f));
                return db;
            };
            const auto flat = envelope(0.0f), bent = envelope(1.0f);
            double diff = 0.0;
            for (size_t i = 0; i < flat.size(); ++i)
                diff += std::abs(flat[i] - bent[i]);
            diff /= static_cast<double>(flat.size());
            std::cout << "  bend 0% vs 100%: the level over time differs by " << diff << " dB on average" << std::endl;
            check(diff < 2.0, "bend changes the pitch, not the swell");
        }

        // Low End's sub octave doesn't rumble between sounds. Bursts of an 80 Hz tone over a quiet noise floor: when a
        // burst stops, the noise alone must not flip the sub octave at random while its level is still falling (a
        // trigger that needs a real swing each way holds still; one that flips on every zero crossing rumbles).
        {
            const double period = 0.75, on = 0.45, edge = 0.01;
            juce::AudioBuffer<float> b(1, static_cast<int>(8.0 * period * c_rate));
            juce::Random r(11);
            float y = 0.0f;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const double lt = std::fmod(i / c_rate, period);
                const double env = lt < edge ? lt / edge : (lt < on - edge ? 1.0 : (lt < on ? (on - lt) / edge : 0.0));
                y += 0.04f * ((r.nextFloat() * 2.0f - 1.0f) - y); // noise, most of it below about 300 Hz
                b.setSample(0, i, 0.35f * static_cast<float>(env * std::sin(twoPi * 80.0 * i / c_rate)) + 0.1f * y);
            }
            CargoHold lowHold;
            lowHold.loadFile(0, writeWav(folder, "rig_src_low80", b), err);
            lowHold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
            lowHold.setSkipSilence(0, false);
            RigEngine e;
            auto q = bare();
            q.buses[0].low = 1.0f;
            start(e, lowHold, q);
            juce::AudioBuffer<float> o(2, static_cast<int>(8.0 * period * c_rate) / c_block * c_block);
            renderRig(e, o, 0, o.getNumSamples(), c_block);
            // The level while each burst plays, and from 50 ms to 200 ms after it has stopped.
            double note = 0.0, rumble = 0.0;
            const double latency = 0.021; // the bus's pass delay at these settings
            const int span = static_cast<int>(0.15 * c_rate);
            for (int k = 1; k < 7; ++k) // the file's first time through (8 bursts)
            {
                const float playing = o.getRMSLevel(0, static_cast<int>((k * period + latency + 0.15) * c_rate), span);
                const float after = o.getRMSLevel(0, static_cast<int>((k * period + latency + on + 0.05) * c_rate), span);
                note += static_cast<double>(playing) * playing;
                rumble += static_cast<double>(after) * after;
            }
            const double ratio = 10.0 * std::log10(std::max(rumble, 1.0e-30) / std::max(note, 1.0e-30));
            std::cout << "  low end: after a sound stops the bus is at " << ratio << " dB against while it plays" << std::endl;
            check(note > 0.0 && ratio < -23.0, "low end: the sub octave doesn't rumble on the noise between sounds");
        }

        // Fill: every track echoes at its own times. The same click on track 1 and on track 4: the first echo comes
        // back after a different time on each.
        {
            juce::AudioBuffer<float> click(1, static_cast<int>(3.0 * c_rate));
            click.clear();
            for (int i = 0; i < 96; ++i)
                click.setSample(0, static_cast<int>(0.1 * c_rate) + i, 0.8f * std::sin(static_cast<float>(twoPi * 2000.0 * i / c_rate)) * std::sin(juce::MathConstants<float>::pi * static_cast<float>(i) / 96.0f));
            const auto file = writeWav(folder, "rig_src_click", click);
            auto firstEcho = [&](int track)
            {
                CargoHold h;
                h.loadFile(track, file, err);
                h.setRoleOverride(track, static_cast<int>(CargoRole::Tonal));
                h.setSkipSilence(track, false);
                RigEngine e;
                auto q = bare();
                q.fill = 1.0f;
                start(e, h, q);
                juce::AudioBuffer<float> o(2, static_cast<int>(1.2 * c_rate) / c_block * c_block);
                renderRig(e, o, 0, o.getNumSamples(), c_block);
                auto loudest = [&](double fromS, double toS)
                {
                    int at = 0;
                    float best = 0.0f;
                    for (int i = static_cast<int>(fromS * c_rate); i < static_cast<int>(toS * c_rate); ++i)
                        if (std::abs(o.getSample(0, i)) > best) { best = std::abs(o.getSample(0, i)); at = i; }
                    return std::pair<double, float>(at / c_rate, best);
                };
                const auto dry = loudest(0.0, 0.3);
                const auto echo = loudest(dry.first + 0.3, dry.first + 0.8);
                return std::pair<double, float>(echo.first - dry.first, echo.second / std::max(dry.second, 1.0e-9f));
            };
            const auto a = firstEcho(0), b = firstEcho(3);
            std::cout << "  fill: first echo after " << a.first << " s on track 1 and " << b.first << " s on track 4" << std::endl;
            bool sized = true, apart = true;
            for (int t = 0; t < RigEngine::c_tracks; ++t)
            {
                sized = sized && RigEngine::fillTapSeconds(t, 2) < 2.2f && RigEngine::fillTapSeconds(t, 0) > 0.4f;
                for (int u = 0; u < t; ++u)
                    apart = apart && std::abs(RigEngine::fillTapSeconds(t, 0) - RigEngine::fillTapSeconds(u, 0)) > 0.005f;
            }
            check(a.second > 0.05f && b.second > 0.05f && std::abs(a.first - RigEngine::fillTapSeconds(0, 0)) < 0.003 && std::abs(b.first - RigEngine::fillTapSeconds(3, 0)) < 0.003
                      && std::abs(a.first - b.first) > 0.03,
                  "fill: each track echoes at its own times");
            check(sized && apart, "fill: every track's echo times differ and fit the echo lines");
        }

        // Space at 0% empties the echoes and the reverb: turning it up again later doesn't play back old sound.
        {
            RigEngine e;
            auto q = bare();
            q.space = 0.8f;
            start(e, toneHold, q);
            const int sec = static_cast<int>(c_rate) / c_block * c_block;
            juce::AudioBuffer<float> o(2, 8 * sec);
            renderRig(e, o, 0, 2 * sec, c_block);       // sound into the echoes and the reverb
            q.space = 0.0f;
            e.setParams(q);
            renderRig(e, o, 2 * sec, 4 * sec, c_block); // Space off
            q.levels.fill(0.0f);
            e.setParams(q);
            renderRig(e, o, 4 * sec, 6 * sec, c_block); // the track fades to silence
            q.space = 0.8f;
            e.setParams(q);
            renderRig(e, o, 6 * sec, 8 * sec, c_block); // Space back up: nothing should come out
            const float stale = std::max(o.getMagnitude(0, 6 * sec, 2 * sec), o.getMagnitude(1, 6 * sec, 2 * sec));
            std::cout << "  space off, then on again in silence: peak " << juce::Decibels::gainToDecibels(stale, -200.0f) << " dBFS" << std::endl;
            check(stale < juce::Decibels::decibelsToGain(-90.0f), "space at 0% empties the echoes and reverb (no old sound when it's turned up again)");
        }

        // The glides take the same time whatever the host's block size: a fader pulled down fades out as fast with
        // 7-sample blocks (every chunk a short one) as with 256-sample blocks.
        {
            auto fadeSeconds = [&](int block)
            {
                RigEngine e;
                auto q = bare();
                start(e, toneHold, q);
                const int pre = static_cast<int>(1.0 * c_rate) / block * block;
                juce::AudioBuffer<float> o(2, pre + static_cast<int>(1.0 * c_rate));
                renderRig(e, o, 0, pre, block);
                q.levels.fill(0.0f);
                e.setParams(q);
                renderRig(e, o, pre, o.getNumSamples(), block);
                const float before = o.getMagnitude(0, pre - 4800, 4800);
                int last = pre;
                for (int i = pre; i < o.getNumSamples(); ++i)
                    if (std::abs(o.getSample(0, i)) > 0.1f * before)
                        last = i;
                return (last - pre) / c_rate;
            };
            const double big = fadeSeconds(256), small = fadeSeconds(7);
            std::cout << "  fader glide: " << big << " s with 256-sample blocks, " << small << " s with 7-sample blocks" << std::endl;
            check(big > 0.02 && std::abs(small - big) < 0.15 * big, "glides take the same time at any block size");
        }
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

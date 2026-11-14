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
#include "TestClock.h"
#include "Cargo.h"
#include "MasterProcessor.h"
#include "RigEngine.h"
#include "SpaceReverb.h"
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
    const auto t0 = TestClock::cpuMs();
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
    const double ms = TestClock::cpuMs() - t0;
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

        // The doppler read is band-limited. On the way in the bus's delay line is read faster than it was written
        // (up to twice as fast), so the top of the sound would land above half the sample rate; a plain
        // interpolator folds it back as whistles that sweep downwards. First the read on its own.
        {
            const auto& kernel = sj::SincKernel::get();
            auto sweepRead = [&](double hz, double speed, bool sinc)
            {
                sj::DelayLine line;
                line.prepare(8192);
                const int n = 6000;
                double sum = 0.0;
                double delay = 7000.0;
                for (int i = 0; i < 7200 + n; ++i)
                {
                    line.push(static_cast<float>(std::sin(twoPi * hz * i / c_rate)));
                    if (i < 7200)
                        continue;
                    delay -= speed - 1.0; // the read point moves `speed` samples per sample
                    const float y = sinc ? line.readSinc(delay, sj::SincKernel::stretchFor(speed), kernel) : line.read(delay);
                    sum += static_cast<double>(y) * y;
                }
                return juce::Decibels::gainToDecibels(static_cast<float>(std::sqrt(2.0 * sum / n)), -150.0f);
            };
            const float foldSinc = sweepRead(15000.0, 2.0, true), foldPlain = sweepRead(15000.0, 2.0, false);
            const float foldMid = sweepRead(17000.0, 1.5, true), foldMidPlain = sweepRead(17000.0, 1.5, false);
            const float passFast = sweepRead(3000.0, 2.0, true), passSlow = sweepRead(10000.0, 0.7, true), passStill = sweepRead(12000.0, 1.0, true);
            std::cout << "  doppler read: 15 kHz at 2x speed (30 kHz, cannot be played) comes out at " << foldSinc << " dB (a 4-point read: " << foldPlain
                      << " dB); 17 kHz at 1.5x " << foldMid << " dB (" << foldMidPlain << " dB); kept: 3 kHz at 2x " << passFast
                      << " dB, 10 kHz at 0.7x " << passSlow << " dB, 12 kHz at 1x " << passStill << " dB" << std::endl;
            check(foldSinc < -55.0f && foldMid < -40.0f, "doppler read: what a fast read pushes above half the sample rate is filtered out, not folded back");
            check(std::abs(passFast) < 0.3f && std::abs(passSlow) < 0.3f && std::abs(passStill) < 0.3f, "doppler read: what fits is kept at full level");
            sj::DelayLine exact;
            exact.prepare(1024);
            for (int i = 0; i < 600; ++i)
                exact.push(static_cast<float>(i));
            check(exact.readSinc(100.0, 1.0, kernel) == 499.0f && std::abs(exact.readSinc(100.5, 1.0, kernel) - 498.5f) < 1.0e-2f,
                  "doppler read: a whole-sample delay returns the sample itself, and a half-way one sits between its neighbours");
        }
        // And in the engine: a 20 kHz tone through the fastest pass (Doppler and Bend 100%). Going away it falls to
        // 14 kHz; coming in it rises out of hearing. Nothing real is below 10 kHz: anything there is fold-back.
        {
            CargoHold h20k;
            h20k.loadFile(0, writeWav(folder, "rig_src_tone20k", makeTone(20000.0, 4.0, 0.5f)), err);
            h20k.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
            h20k.setSkipSilence(0, false);
            RigEngine e;
            auto q = bare();
            q.buses[0].doppler = 1.0f;
            q.buses[0].bend = 1.0f;
            start(e, h20k, q);
            juce::AudioBuffer<float> o(2, static_cast<int>(20.0 * c_rate) / c_block * c_block);
            renderRig(e, o, 0, o.getNumSamples(), c_block);
            std::array<sj::Svf, 6> lp;
            for (auto& f : lp)
                f.set(7000.0f, 0.7071f, static_cast<float>(c_rate));
            double low = 0.0, all = 0.0;
            for (int i = static_cast<int>(1.0 * c_rate); i < o.getNumSamples(); ++i)
            {
                float x = o.getSample(0, i);
                all += static_cast<double>(x) * x;
                for (auto& f : lp)
                    x = f.lowpass(x);
                low += static_cast<double>(x) * x;
            }
            const float ratio = juce::Decibels::gainToDecibels(static_cast<float>(std::sqrt(low / std::max(all, 1.0e-30))), -150.0f);
            std::cout << "  doppler fold-back: a 20 kHz tone through the fastest pass leaves " << ratio << " dB below 7 kHz (against everything)" << std::endl;
            check(ratio < -45.0f, "doppler: a fast pass doesn't fold the top of the sound back down as whistles");
        }

        // A pitched track plays about as loud with VARI on (faster / slower) as with VARI off (the shifter). The hard
        // case is a bright hiss pitched up: most of it sits at the very top, where the faster read has to filter, so
        // VARI on carries a small make-up there. (The tone is printed only: its loop point adds a level of its own.)
        {
            juce::AudioBuffer<float> bright(1, static_cast<int>(6.0 * c_rate));
            juce::Random r(11);
            float prev = 0.0f;
            for (int i = 0; i < bright.getNumSamples(); ++i)
            {
                const float w = r.nextFloat() * 2.0f - 1.0f;
                bright.setSample(0, i, 0.35f * (w - 0.7f * prev));
                prev = w;
            }
            CargoHold hHiss, hTone;
            hHiss.loadFile(0, writeWav(folder, "rig_src_bright", bright), err);
            hTone.loadFile(0, writeWav(folder, "rig_src_tone660", makeTone(660.0, 4.0, 0.5f)), err);
            float worst = 0.0f;
            for (auto* h : { &hHiss, &hTone })
            {
                h->setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
                h->setSkipSilence(0, false);
                for (float semis : { -12.0f, 7.0f, 12.0f })
                {
                    float db[2] {};
                    for (int vari = 0; vari < 2; ++vari)
                    {
                        RigEngine e;
                        auto q = bare();
                        q.trackPitch[0] = semis;
                        q.trackVari[0] = vari == 1;
                        start(e, *h, q);
                        juce::AudioBuffer<float> o(2, static_cast<int>(6.0 * c_rate) / c_block * c_block);
                        renderRig(e, o, 0, o.getNumSamples(), c_block);
                        const int from = static_cast<int>(2.0 * c_rate);
                        db[vari] = juce::Decibels::gainToDecibels(o.getRMSLevel(0, from, o.getNumSamples() - from));
                    }
                    std::cout << "  track pitch " << semis << " st on " << (h == &hHiss ? "a bright hiss" : "a tone") << ": VARI on " << db[1] << " dB, VARI off " << db[0] << " dB" << std::endl;
                    if (h == &hHiss)
                        worst = std::max(worst, std::abs(db[1] - db[0]));
                }
            }
            check(worst < 1.0f, "track pitch: a hiss is within 1 dB with VARI on and off, an octave down, a fifth up and an octave up (worst " + juce::String(worst, 2) + " dB)");
        }

        // Stop, then Play again before the sources have let go (60 ms): the restart waits for a short fade, so the
        // sound is never cut mid-wave. Also a restart while playing. A smooth 330 Hz tone never curves this sharply.
        {
            CargoHold h330;
            h330.loadFile(0, writeWav(folder, "rig_src_tone330b", makeTone(330.0, 4.0, 0.5f)), err);
            h330.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
            h330.setSkipSilence(0, false);
            auto sharpestAfter = [&](int stopSamples)
            {
                RigEngine e;
                start(e, h330, bare());
                juce::AudioBuffer<float> o(2, static_cast<int>(2.0 * c_rate) / 64 * 64);
                const int stopAt = static_cast<int>(1.0 * c_rate) / 64 * 64;
                renderRig(e, o, 0, stopAt, 64);
                if (stopSamples > 0)
                {
                    e.setReleased(true);
                    renderRig(e, o, stopAt, stopAt + stopSamples, 64);
                    e.setReleased(false);
                }
                e.restart();
                renderRig(e, o, stopAt + stopSamples, o.getNumSamples(), 64);
                float sharpest = 0.0f;
                for (int i = stopAt - 4800; i < o.getNumSamples(); ++i)
                    sharpest = std::max(sharpest, std::abs(o.getSample(0, i) - 2.0f * o.getSample(0, i - 1) + o.getSample(0, i - 2)));
                const float after = o.getRMSLevel(0, o.getNumSamples() - 9600, 9600);
                return std::pair<float, float> { sharpest, after };
            };
            const auto quick = sharpestAfter(960), live = sharpestAfter(0), late = sharpestAfter(9600);
            std::cout << "  restart: sharpest change, Play 20 ms after Stop " << quick.first << ", restart while playing " << live.first
                      << ", Play 200 ms after Stop " << late.first << " (level afterwards " << quick.second << " / " << live.second << " / " << late.second << ")" << std::endl;
            check(quick.first < 0.01f && live.first < 0.01f && late.first < 0.01f, "restart: Play just after Stop (or a restart while playing) doesn't click");
            check(quick.second > 0.1f && live.second > 0.1f && late.second > 0.1f, "restart: the sources come back at full level");
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

        // Space's reverb on its own, at 44.1 to 192 kHz: an impulse into the longest tail (Space 100%) comes out
        // late (pre-delay), dense, wide but mono-safe, and it dies away without ever growing.
        {
            // How many samples of a stretch lie outside one standard deviation, against what dense (gaussian)
            // reverb gives: about 1 is a smooth tail, well under 1 is separate echoes (heard as a metallic ring).
            auto density = [](const std::vector<float>& x, int from, int to)
            {
                double sq = 0.0;
                for (int i = from; i < to; ++i) sq += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
                const double sd = std::sqrt(sq / juce::jmax(1, to - from));
                int outside = 0;
                for (int i = from; i < to; ++i) outside += std::abs(x[static_cast<size_t>(i)]) > sd ? 1 : 0;
                return static_cast<double>(outside) / juce::jmax(1, to - from) / 0.3173;
            };
            for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                SpaceReverb rev;
                rev.prepare(rate);
                rev.setDecay(SpaceReverb::decaySecondsFor(1.0f));
                const int n = static_cast<int>(rate * 9.0);
                std::vector<float> l(static_cast<size_t>(n), 0.0f), r(static_cast<size_t>(n), 0.0f);
                l[0] = 1.0f; r[0] = 1.0f;
                for (int pos = 0; pos < n; pos += 32)
                    rev.process(l.data() + pos, r.data() + pos, juce::jmin(32, n - pos), 0.0f, 0.0f, 1.0f, 1.0f);

                bool finite = true;
                for (int i = 0; i < n; ++i) finite = finite && std::isfinite(l[static_cast<size_t>(i)]) && std::isfinite(r[static_cast<size_t>(i)]);
                int first = n;
                for (int i = 0; i < n && first == n; ++i)
                    if (std::abs(l[static_cast<size_t>(i)]) > 1.0e-6f || std::abs(r[static_cast<size_t>(i)]) > 1.0e-6f) first = i;
                // Level in 250 ms steps: the loudest step, whether any later step is louder than the one before, and
                // when the level has fallen 60 dB.
                const int win = static_cast<int>(rate * 0.25);
                std::vector<double> lev;
                for (int at = 0; at + win <= n; at += win)
                {
                    double sq = 0.0;
                    for (int i = at; i < at + win; ++i) sq += static_cast<double>(l[static_cast<size_t>(i)]) * l[static_cast<size_t>(i)] + static_cast<double>(r[static_cast<size_t>(i)]) * r[static_cast<size_t>(i)];
                    lev.push_back(std::sqrt(sq / (2.0 * win)));
                }
                const double top = *std::max_element(lev.begin(), lev.end());
                double worstRise = 0.0, t60 = -1.0;
                for (size_t k = 1; k < lev.size(); ++k)
                {
                    if (lev[k] > top * 1.0e-4)
                        worstRise = juce::jmax(worstRise, 20.0 * std::log10(lev[k] / juce::jmax(1.0e-30, lev[k - 1])));
                    if (t60 < 0.0 && lev[k] < top * 1.0e-3)
                        t60 = 0.25 * static_cast<double>(k);
                }
                const double dens = density(l, first + static_cast<int>(rate * 0.06), first + static_cast<int>(rate * 0.16));
                std::cout << "  space reverb at " << rate << " Hz: first sound at " << 1000.0 * first / rate << " ms, density " << dens
                          << ", -60 dB after " << t60 << " s, worst rise " << worstRise << " dB" << std::endl;
                const juce::String at = " at " + juce::String(rate / 1000.0, 1) + " kHz";
                check(finite, "space reverb: output is finite" + at);
                check(1000.0 * first / rate >= 15.0 && 1000.0 * first / rate <= 60.0, "space reverb: the tail starts after a pre-delay" + at);
                check(dens > 0.85, "space reverb: the tail is dense 60 ms in (no separate, ringing echoes)" + at);
                check(worstRise < 0.5, "space reverb: the tail never grows" + at);
                check(t60 > 1.5 && t60 <= 6.5, "space reverb: the longest tail is down 60 dB within 6.5 s" + at);
            }

            // What the reverb it replaced did on the same measure, for the record (not a check).
            {
                juce::Reverb old;
                old.setSampleRate(c_rate);
                juce::Reverb::Parameters rp;
                rp.roomSize = 0.95f; rp.damping = 0.35f; rp.wetLevel = 0.2f; rp.dryLevel = 0.0f; rp.width = 1.0f;
                old.setParameters(rp);
                const int n = static_cast<int>(c_rate);
                std::vector<float> l(static_cast<size_t>(n), 0.0f), r(static_cast<size_t>(n), 0.0f);
                for (int i = 0; i < 2000; ++i) old.processStereo(l.data(), r.data(), 32); // let its parameter glide finish
                l[0] = 1.0f; r[0] = 1.0f;
                old.processStereo(l.data(), r.data(), n);
                int first = 0;
                while (first < n - 1 && std::abs(l[static_cast<size_t>(first)]) < 1.0e-6f) ++first;
                std::cout << "  (a comb-filter reverb on the same measure: first sound at " << 1000.0 * first / c_rate << " ms, density "
                          << density(l, first + static_cast<int>(c_rate * 0.06), first + static_cast<int>(c_rate * 0.16)) << ")" << std::endl;
            }

            // A moving Space knob: the dry and wet gains glide across the chunk, they don't step at its start.
            {
                SpaceReverb rev;
                rev.prepare(c_rate);
                std::vector<float> l(64, 1.0f), r(64, 1.0f);
                rev.process(l.data(), r.data(), 64, 1.0f, 0.5f, 0.0f, 0.0f);
                float worstStep = std::abs(l[0] - 1.0f);
                for (size_t i = 1; i < l.size(); ++i) worstStep = juce::jmax(worstStep, std::abs(l[i] - l[i - 1]));
                check(worstStep < 0.01f && std::abs(l[63] - 0.5f) < 1.0e-6f && std::abs(r[63] - 0.5f) < 1.0e-6f,
                      "space reverb: a change of level is spread over the chunk (no step)");
            }

            // Noise through it, the same on both sides (the worst case for width): left and right come out
            // different (wide) but not opposed, so a mono fold-down keeps the reverb.
            {
                SpaceReverb rev;
                rev.prepare(c_rate);
                rev.setDecay(SpaceReverb::decaySecondsFor(1.0f));
                const int n = static_cast<int>(c_rate * 6.0);
                std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
                juce::Random rnd(11);
                for (int i = 0; i < n; ++i) l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = rnd.nextFloat() * 2.0f - 1.0f;
                rev.process(l.data(), r.data(), n, 0.0f, 0.0f, 1.0f, 1.0f);
                double ll = 0.0, rr = 0.0, lr = 0.0;
                for (int i = n / 3; i < n; ++i)
                {
                    ll += static_cast<double>(l[static_cast<size_t>(i)]) * l[static_cast<size_t>(i)];
                    rr += static_cast<double>(r[static_cast<size_t>(i)]) * r[static_cast<size_t>(i)];
                    lr += static_cast<double>(l[static_cast<size_t>(i)]) * r[static_cast<size_t>(i)];
                }
                const double corr = lr / std::sqrt(juce::jmax(1.0e-30, ll * rr));
                std::cout << "  space reverb, mono noise in: left/right correlation of the wet " << corr << std::endl;
                check(corr > -0.1 && corr < 0.6, "space reverb: wide, and the mono sum doesn't cancel");
            }
        }

        // Space in the rig, at 44.1 to 192 kHz: a held hiss at Space 100% stays finite, more Space doesn't mean much
        // more level (the dry is eased down as the wet comes up), and once the sound stops the echoes and the
        // reverb die away steadily.
        juce::AudioBuffer<float> spaceHiss(1, static_cast<int>(5.0 * c_rate));
        {
            juce::Random r(23);
            for (int i = 0; i < spaceHiss.getNumSamples(); ++i)
                spaceHiss.setSample(0, i, 0.3f * (r.nextFloat() * 2.0f - 1.0f));
        }
        CargoHold spaceHold;
        spaceHold.loadFile(0, writeWav(folder, "rig_src_space_hiss", spaceHiss), err);
        spaceHold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
        spaceHold.setSkipSilence(0, false);
        for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            auto render = [&](float space, juce::AudioBuffer<float>& o)
            {
                RigEngine e;
                auto q = bare();
                q.space = space;
                e.setCargoHold(&spaceHold);
                e.setParams(q);
                e.prepare(rate, c_block);
                e.setParams(q);
                const int sec = static_cast<int>(rate) / c_block * c_block;
                o.setSize(2, 16 * sec);
                o.clear();
                renderRig(e, o, 0, 5 * sec, c_block);
                q.levels.fill(0.0f);
                e.setParams(q);
                renderRig(e, o, 5 * sec, 16 * sec, c_block);
                return sec;
            };
            juce::AudioBuffer<float> dry, wet;
            render(0.0f, dry);
            const int sec = render(1.0f, wet);
            bool finite = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < wet.getNumSamples(); ++i)
                    finite = finite && std::isfinite(wet.getSample(ch, i));
            auto level = [&](const juce::AudioBuffer<float>& o, int from, int len) { return std::sqrt(0.5 * (std::pow(o.getRMSLevel(0, from, len), 2.0) + std::pow(o.getRMSLevel(1, from, len), 2.0))); };
            const double held = level(wet, 3 * sec, 2 * sec), heldDry = level(dry, 3 * sec, 2 * sec);
            const double lift = 20.0 * std::log10(held / juce::jmax(1.0e-30, heldDry));
            double worstRise = 0.0, t60 = -1.0, last = level(wet, 5 * sec, sec);
            for (int k = 6; k < 16; ++k)
            {
                const double now = level(wet, k * sec, sec);
                if (now > held * 1.0e-4)
                    worstRise = juce::jmax(worstRise, 20.0 * std::log10(now / juce::jmax(1.0e-30, last)));
                if (t60 < 0.0 && now < held * 1.0e-3)
                    t60 = static_cast<double>(k - 5);
                last = now;
            }
            std::cout << "  rig at " << rate << " Hz, held hiss, Space 100% against 0%: level " << lift << " dB, after the sound stops: -60 dB within "
                      << t60 << " s, worst rise " << worstRise << " dB" << std::endl;
            const juce::String at = " at " + juce::String(rate / 1000.0, 1) + " kHz";
            check(finite, "space 100%: the rig's output is finite" + at);
            check(lift < 3.0, "space 100%: a held sound is not much louder than with Space off" + at);
            check(worstRise < 0.5 && t60 > 0.0 && t60 <= 10.0, "space 100%: echoes and reverb die away steadily once the sound stops" + at);
        }

        // A reset puts Space back where it started (the echo drift too), so the same render twice is the same sound.
        // (The rest of the rig differs by a few thousandths between two runs; echoes at another point of their drift
        // differ by a few tenths.)
        {
            RigEngine e;
            auto q = bare();
            q.space = 1.0f;
            e.setCargoHold(&spaceHold);
            e.setParams(q);
            e.prepare(c_rate, c_block);
            e.setParams(q);
            const int len = static_cast<int>(c_rate) / c_block * c_block * 3;
            juce::AudioBuffer<float> first(2, len), second(2, len);
            first.clear(); second.clear();
            renderRig(e, first, 0, len, c_block);
            e.prepare(c_rate, c_block);
            e.setParams(q);
            renderRig(e, second, 0, len, c_block);
            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < len; ++i)
                    worst = juce::jmax(worst, std::abs(first.getSample(ch, i) - second.getSample(ch, i)));
            std::cout << "  space 100%, the same render twice: largest difference " << worst << std::endl;
            check(worst < 0.03f, "space: the same render after a reset is the same sound (echo drift starts from the same place)");
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

        // ---- Pass Mode, the passes moving the colours, ride make-up and head shadow ---------------------------------
        {
            juce::AudioBuffer<float> hissB(1, static_cast<int>(5.0 * c_rate));
            {
                juce::Random r(23);
                for (int i = 0; i < hissB.getNumSamples(); ++i)
                    hissB.setSample(0, i, 0.3f * (r.nextFloat() * 2.0f - 1.0f));
            }
            const auto hissFile = writeWav(folder, "rig_src_hiss2", hissB);
            CargoHold hissA, hissOnB, tone330;
            hissA.loadFile(0, hissFile, err);
            hissA.setRoleOverride(0, static_cast<int>(CargoRole::Tonal)); // bus A
            hissA.setSkipSilence(0, false);
            hissOnB.loadFile(0, hissFile, err);
            hissOnB.setRoleOverride(0, static_cast<int>(CargoRole::Bed)); // bus B
            hissOnB.setSkipSilence(0, false);
            tone330.loadFile(0, writeWav(folder, "rig_src_tone330b", makeTone(330.0, 4.0, 0.5f)), err);
            tone330.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
            tone330.setSkipSilence(0, false);

            auto render = [&](CargoHold& h, const RigEngine::Params& q, double seconds)
            {
                RigEngine e;
                start(e, h, q);
                juce::AudioBuffer<float> o(2, static_cast<int>(seconds * c_rate) / c_block * c_block);
                renderRig(e, o, 0, o.getNumSamples(), c_block);
                return o;
            };
            // The level over time, in dB, in windows of `window` seconds from `from` seconds on.
            auto levels = [&](const juce::AudioBuffer<float>& o, double from, double window)
            {
                std::vector<float> db;
                const int w = static_cast<int>(window * c_rate);
                for (int at = static_cast<int>(from * c_rate); at + w <= o.getNumSamples(); at += w)
                    db.push_back(juce::Decibels::gainToDecibels(std::sqrt(0.5f * (o.getRMSLevel(0, at, w) * o.getRMSLevel(0, at, w)
                                                                                  + o.getRMSLevel(1, at, w) * o.getRMSLevel(1, at, w))), -120.0f));
                return db;
            };
            auto spread = [](std::vector<float> v, double lo, double hi)
            {
                std::sort(v.begin(), v.end());
                const auto at = [&v](double q) { return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))]; };
                return at(hi) - at(lo);
            };
            auto meanDb = [&](const juce::AudioBuffer<float>& o, double from, double to)
            {
                const int a = static_cast<int>(from * c_rate), n = static_cast<int>((to - from) * c_rate);
                return juce::Decibels::gainToDecibels(std::sqrt(0.5f * (o.getRMSLevel(0, a, n) * o.getRMSLevel(0, a, n) + o.getRMSLevel(1, a, n) * o.getRMSLevel(1, a, n))), -120.0f);
            };

            // Fly-By plays one pass at a time. A 330 Hz tone at Doppler 100%, Bend 100% is heard at 569 Hz coming in
            // and 232 Hz going out. In Bed both pitches sound together much of the time (the two passes overlap); in
            // Fly-By the pass coming in is still far off while the one going out is heard.
            {
                auto together = [&](int mode)
                {
                    auto q = bare();
                    q.buses[0].doppler = 1.0f;
                    q.buses[0].bend = 1.0f;
                    q.buses[0].passMode = mode;
                    const auto o = render(tone330, q, 24.0);
                    const int n = 4096;
                    std::vector<std::pair<double, double>> frames;
                    double loudest = 0.0;
                    for (int at = static_cast<int>(1.0 * c_rate); at + n <= o.getNumSamples(); at += n / 2)
                    {
                        double p2[2] {};
                        const double hz[2] = { 330.0 / (1.0 - 0.42), 330.0 / (1.0 + 0.42) };
                        for (int k = 0; k < 2; ++k)
                        {
                            double re = 0.0, im = 0.0;
                            for (int i = 0; i < n; ++i)
                            {
                                const double w = 0.5 - 0.5 * std::cos(twoPi * i / n);
                                const double x = w * 0.5 * (o.getSample(0, at + i) + o.getSample(1, at + i));
                                re += x * std::cos(twoPi * hz[k] * i / c_rate);
                                im += x * std::sin(twoPi * hz[k] * i / c_rate);
                            }
                            p2[k] = re * re + im * im;
                        }
                        frames.push_back({ p2[0], p2[1] });
                        loudest = std::max({ loudest, p2[0], p2[1] });
                    }
                    int both = 0;
                    for (const auto& f : frames) // both pitches within 6 dB of each other, and not in the quiet between
                        if (std::max(f.first, f.second) > 1.0e-3 * loudest && std::min(f.first, f.second) > 0.25 * std::max(f.first, f.second))
                            ++both;
                    return static_cast<double>(both) / static_cast<double>(frames.size());
                };
                const double bed = together(0), fly = together(1);
                std::cout << "  pass mode: the rising and the falling pitch sound together " << 100.0 * bed << "% of the time in Bed, "
                          << 100.0 * fly << "% in Fly-By" << std::endl;
                check(bed > 0.2 && fly < 0.4 * bed && fly < 0.12, "pass mode Fly-By: one pass (one pitch) at a time, where Bed overlaps two");
            }

            // Fly-By's shape on a steady hiss (a slow, far pass, so the level is the window's): it dips between passes
            // but never goes silent, and Bed stays steady. The approach is longer than the tail.
            auto slowPass = [&]()
            {
                auto q = bare();
                for (auto& b : q.buses) { b.doppler = 0.2f; b.bend = 1.0f; } // just enough speed that the two passes don't read the same sample
                return q;
            };
            {
                auto q = slowPass();
                q.buses[0].passMode = 1;
                const auto fly = render(hissA, q, 26.0);
                q.buses[0].passMode = 0;
                const auto bed = render(hissA, q, 26.0);
                const float flyRange = spread(levels(fly, 2.0, 0.05), 0.02, 0.98), bedRange = spread(levels(bed, 2.0, 0.05), 0.02, 0.98);
                // How much of the way in, and of the way out, a pass is within 9 dB of its peak.
                int in = 0, out = 0;
                for (int i = 0; i < 1000; ++i)
                    if (RigEngine::passWindow(static_cast<float>(i) / 1000.0f, 1.0f) > 0.355f)
                        (i < 500 ? in : out) += 1;
                std::cout << "  pass mode on a hiss: Fly-By's level spans " << flyRange << " dB (Bed " << bedRange << " dB); a Fly-By is within 9 dB of its peak for "
                          << in / 5 << "% of the way in and " << out / 5 << "% of the way out" << std::endl;
                check(bedRange < 3.0f, "pass mode Bed: the two passes add up to a steady level");
                check(flyRange > 7.0f && flyRange < 16.0f, "pass mode Fly-By: the level dips between passes without going silent");
                check(in > out * 13 / 10 && out > 100, "pass mode Fly-By: a long approach and a shorter tail");
                // And the two modes are about as loud as each other on the same sound (Fly-By's passes don't overlap,
                // so it carries a make-up).
                const int skip = static_cast<int>(2.0 * c_rate);
                const float flyDb = juce::Decibels::gainToDecibels(fly.getRMSLevel(0, skip, fly.getNumSamples() - skip));
                const float bedDb = juce::Decibels::gainToDecibels(bed.getRMSLevel(0, skip, bed.getNumSamples() - skip));
                std::cout << "  pass mode level on a hiss: Fly-By " << flyDb << " dB, Bed " << bedDb << " dB (average power)" << std::endl;
                check(std::abs(flyDb - bedDb) < 1.5f, "pass mode: Fly-By is about as loud as Bed on the same sound");
            }

            // Fly-By's loudest moment is the pass's closest point (which AIM aims at): the hits still land on it.
            {
                RigEngine e;
                auto q = p;
                q.hitAim = true;
                q.hitSense = 1.0f;
                q.buses[0].passMode = q.buses[1].passMode = 1;
                start(e, hold, q);
                juce::AudioBuffer<float> o(2, c_block);
                int beats = 0, onPeak = 0, last = e.getHitBeats();
                for (int blk = 0; blk < static_cast<int>(40.0 * c_rate) / c_block; ++blk)
                {
                    e.process(nullptr, nullptr, o.getWritePointer(0), o.getWritePointer(1), c_block);
                    if (e.getHitBeats() == last)
                        continue;
                    last = e.getHitBeats();
                    ++beats;
                    for (int fl = 0; fl < RigEngine::c_maxFlights; ++fl)
                        if (std::abs(e.getFlightView(fl).age - 0.5f) < 0.02f)
                        {
                            ++onPeak;
                            break;
                        }
                }
                // And the window itself peaks there, in both modes and in between.
                bool peakAtClosest = true;
                for (float shape : { 0.0f, 0.5f, 1.0f })
                    for (float u = 0.0f; u <= 1.0f; u += 0.01f)
                        peakAtClosest = peakAtClosest && RigEngine::passWindow(u, shape) <= RigEngine::passWindow(0.5f, shape) + 1.0e-6f;
                std::cout << "  aim in Fly-By: " << beats << " hits, " << onPeak << " on a pass's closest point" << std::endl;
                check(beats >= 8 && onPeak >= beats * 9 / 10, "pass mode Fly-By: AIM's hits land on the passes' closest points");
                check(peakAtClosest && std::abs(RigEngine::passWindow(0.5f, 1.0f) - 1.0f) < 1.0e-6f && RigEngine::passWindow(0.0f, 1.0f) < 1.0e-6f
                          && RigEngine::passWindow(1.0f, 1.0f) < 1.0e-6f,
                      "pass mode: the window is loudest at the closest point and silent at both ends");
            }

            // Linked, bus B flies in bus A's Pass Mode (its own menu rests).
            {
                auto range = [&](bool link)
                {
                    auto q = slowPass();
                    q.linkBuses = link;
                    q.buses[0].passMode = 1;
                    q.buses[1].passMode = 0;
                    return spread(levels(render(hissOnB, q, 20.0), 2.0, 0.05), 0.02, 0.98);
                };
                const float linked = range(true), own = range(false);
                std::cout << "  pass mode and LINK: bus B's level spans " << linked << " dB linked to a Fly-By bus A, " << own << " dB on its own (Bed)" << std::endl;
                check(linked > 7.0f && own < 3.0f, "pass mode: a linked bus B uses bus A's Pass Mode");
            }

            // Changing Pass Mode mid-pass doesn't click: the window glides from one shape to the other.
            {
                RigEngine e;
                auto q = bare();
                start(e, toneHold, q);
                juce::AudioBuffer<float> o(2, static_cast<int>(8.0 * c_rate) / c_block * c_block);
                int mode = 0;
                const int every = static_cast<int>(0.37 * c_rate) / c_block * c_block;
                for (int pos = 0; pos < o.getNumSamples(); pos += c_block)
                {
                    if (pos > 0 && pos % every == 0)
                    {
                        mode = 1 - mode;
                        q.buses[0].passMode = mode;
                        e.setParams(q);
                    }
                    e.process(nullptr, nullptr, o.getWritePointer(0, pos), o.getWritePointer(1, pos), c_block);
                }
                // A steady sine is removed exactly by this filter; what is left is how sharply the level changes.
                const float c = 2.0f * std::cos(static_cast<float>(twoPi * 1320.0 / c_rate));
                float sharpest = 0.0f;
                const float* x = o.getReadPointer(0);
                for (int i = static_cast<int>(1.0 * c_rate); i < o.getNumSamples(); ++i)
                    sharpest = std::max(sharpest, std::abs(x[i] - c * x[i - 1] + x[i - 2]));
                std::cout << "  pass mode switched every 0.37 s: sharpest change " << sharpest << std::endl;
                check(sharpest < 2.0e-4f, "pass mode: switching mid-pass doesn't click");
            }

            // With the faders still (Auto Ride off) the passes move the colours. Two steady tones (100 Hz and 1200 Hz,
            // no pitch bend): the Filter lets more of the high one through as a pass comes close and less as it leaves,
            // at least as much as when the ride moves the level.
            {
                juce::AudioBuffer<float> pair(1, static_cast<int>(4.0 * c_rate));
                for (int i = 0; i < pair.getNumSamples(); ++i)
                    pair.setSample(0, i, 0.2f * static_cast<float>(std::sin(twoPi * 100.0 * i / c_rate) + std::sin(twoPi * 1200.0 * i / c_rate)));
                CargoHold pairHold;
                pairHold.loadFile(0, writeWav(folder, "rig_src_pair", pair), err);
                pairHold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
                pairHold.setSkipSilence(0, false);
                auto filterMove = [&](bool ride)
                {
                    auto q = bare();
                    q.rideOn = ride;
                    q.energy = 0.35f;
                    q.buses[0].doppler = 0.55f;
                    q.buses[0].filter = 0.5f;
                    const auto o = render(pairHold, q, 32.0);
                    // The high tone against the low one, in 100 ms windows.
                    std::vector<float> ratio;
                    const int w = static_cast<int>(0.1 * c_rate);
                    for (int at = static_cast<int>(2.0 * c_rate); at + w <= o.getNumSamples(); at += w)
                    {
                        double power[2] {};
                        const double hz[2] = { 100.0, 1200.0 };
                        for (int k = 0; k < 2; ++k)
                        {
                            double re = 0.0, im = 0.0;
                            for (int i = 0; i < w; ++i)
                            {
                                const double x = (0.5 - 0.5 * std::cos(twoPi * i / w)) * 0.5 * (o.getSample(0, at + i) + o.getSample(1, at + i));
                                re += x * std::cos(twoPi * hz[k] * (at + i) / c_rate);
                                im += x * std::sin(twoPi * hz[k] * (at + i) / c_rate);
                            }
                            power[k] = re * re + im * im;
                        }
                        if (power[0] > 1.0e-3 && power[1] > 1.0e-9) // skip the ride's silences
                            ratio.push_back(static_cast<float>(10.0 * std::log10(power[1] / power[0])));
                    }
                    return spread(ratio, 0.1, 0.9);
                };
                const float still = filterMove(false), ridden = filterMove(true);
                std::cout << "  the Filter over time on two tones (P10 to P90): " << still << " dB with Auto Ride off, " << ridden << " dB with it on" << std::endl;
                check(still > 0.8f * ridden && still > 3.0f, "with the faders still, the passes move the Filter");
            }

            // Ride make-up: the rig is about as loud into the output stage with Auto Ride off as with the default ride;
            // a Swell is a push at first and settles to that level when it is held.
            {
                const float atDefault = RigEngine::rideMakeup(::Params::energyDepth(0.35f), 0.4f), deeper = RigEngine::rideMakeup(1.0f, 0.4f);
                const float still = juce::Decibels::gainToDecibels(RigEngine::rideMakeup(0.0f, 0.4f));
                bool falls = true;
                for (float k = 0.0f; k < 0.35f; k += 0.05f)
                    falls = falls && RigEngine::rideMakeup(::Params::energyDepth(k), 0.4f) <= RigEngine::rideMakeup(::Params::energyDepth(k + 0.05f), 0.4f) + 1.0e-6f;
                check(atDefault == 1.0f && deeper == 1.0f && still < -3.5f && still > -6.5f && falls,
                      "ride make-up: nothing at the default ride or deeper, easing down to about -5 dB with the faders still");

                auto q = p;
                q.energy = 0.35f; // the default Energy
                const auto ridden = render(hold, q, 40.0);
                q.rideOn = false;
                const auto steady = render(hold, q, 40.0);
                const float on = meanDb(ridden, 4.0, 40.0), off = meanDb(steady, 4.0, 40.0);

                RigEngine e;
                q.rideOn = true;
                start(e, hold, q);
                juce::AudioBuffer<float> o(2, static_cast<int>(34.0 * c_rate) / c_block * c_block);
                const int press = static_cast<int>(4.0 * c_rate) / c_block * c_block;
                renderRig(e, o, 0, press, c_block);
                q.swell = true;
                e.setParams(q);
                renderRig(e, o, press, o.getNumSamples(), c_block);
                const float pushed = meanDb(o, 4.3, 5.3), held = meanDb(o, 14.0, 34.0);
                std::cout << "  ride make-up: level with Auto Ride on " << on << " dB, off " << off << " dB; Swell " << pushed << " dB in its first second, "
                          << held << " dB held" << std::endl;
                check(std::abs(off - on) < 2.0f, "ride make-up: Auto Ride off is about as loud as the default ride");
                check(pushed > held + 1.5f && std::abs(held - on) < 2.0f, "ride make-up: a Swell pushes at first, and settles when held");
            }

            // Head shadow: when a pass is off to one side, the far ear hears it duller than the near ear.
            {
                auto q = bare();
                q.buses[0].width = 1.0f;
                q.buses[0].passMode = 1; // one pass at a time, so each moment has one place
                const auto o = render(hissA, q, 24.0);
                float x1[2] {}, x2[2] {};
                double all[2] {}, top[2] {};
                double duller = 0.0;
                int sides = 0;
                const int w = static_cast<int>(0.05 * c_rate);
                for (int i = 0; i < o.getNumSamples(); ++i)
                {
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        // The top: what changes fastest from sample to sample (mostly above 12 kHz).
                        const float x = o.getSample(ch, i), d = x - 2.0f * x1[ch] + x2[ch];
                        x2[ch] = x1[ch];
                        x1[ch] = x;
                        all[ch] += x * x;
                        top[ch] += d * d;
                    }
                    if ((i + 1) % w == 0)
                    {
                        const int nearEar = all[0] > all[1] ? 0 : 1, farEar = 1 - nearEar;
                        // Clearly to one side (the far ear 6 to 20 dB down), and loud enough to measure.
                        if (i > static_cast<int>(2.0 * c_rate) && all[farEar] > 1.0e-5 * w && all[nearEar] > 4.0 * all[farEar] && all[nearEar] < 100.0 * all[farEar])
                        {
                            duller += 10.0 * std::log10((top[nearEar] / all[nearEar]) / (top[farEar] / all[farEar]));
                            ++sides;
                        }
                        all[0] = all[1] = top[0] = top[1] = 0.0;
                    }
                }
                duller /= std::max(1, sides);
                std::cout << "  head shadow: off to a side, the far ear has " << duller << " dB less top (above 12 kHz) than the near ear (" << sides << " moments)" << std::endl;
                check(sides > 40 && duller > 1.4, "head shadow: the ear turned away from a pass hears it duller");
            }
        }
    }

    // Throw: one whoosh on demand. From silence (the rig stopped) a throw makes one swell whose peak is heard half a
    // Throw Length after it was thrown, then it is silent again; AIM lands the hits on that peak.
    {
        auto steadyTone = [](double hz)
        {
            // Whole cycles, so the loop is seamless: a steady level to measure a swell on.
            juce::AudioBuffer<float> b(1, static_cast<int>(c_rate * 2.0));
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample(0, i, 0.5f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * hz * i / c_rate)));
            return b;
        };
        CargoHold tones, tonesAndHits;
        for (auto* h : { &tones, &tonesAndHits })
        {
            h->loadFile(0, writeWav(folder, "rig_src_tone1000", steadyTone(1000.0)), error);
            h->loadFile(1, writeWav(folder, "rig_src_tone1500", steadyTone(1500.0)), error);
            h->setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
            h->setRoleOverride(1, static_cast<int>(CargoRole::Bed));
            h->setSkipSilence(0, false);
            h->setSkipSilence(1, false);
        }
        tonesAndHits.loadFile(2, writeWav(folder, "rig_src_hits", hits()), error);
        tonesAndHits.setRoleOverride(2, static_cast<int>(CargoRole::Impact));

        // Thrown after 6 s of rest (an engine that starts stopped lets its sources go over its first 60 ms, and the
        // passes carry that for a few seconds), 1000 samples into a block.
        constexpr int throwAt = static_cast<int>(6.0 * c_rate) + 1000;
        bool stillFaders = false; // Energy and Chaos at 0: the ride leaves every fader up, the same every run
        auto throwRun = [&](CargoHold& from, float length, float velocity, bool playing, int& throwsSeen, bool& stillThrowing)
        {
            RigEngine e;
            e.setCargoHold(&from);
            RigEngine::Params q;
            q.space = 0.0f;
            q.fill = 0.0f;
            q.hitDuck = 0.0f;
            q.throwSeconds = length;
            if (stillFaders)
                q.energy = q.chaos = 0.0f;
            for (auto& b : q.buses)
                b.low = b.swirl = b.filter = b.drive = b.duck = b.key = 0.0f; // the swell alone
            e.setParams(q);
            e.prepare(c_rate, c_block);
            e.setParams(q);
            e.setReleased(! playing);
            juce::AudioBuffer<float> o(2, static_cast<int>(11.0 * c_rate));
            for (int pos = 0; pos < o.getNumSamples(); pos += c_block)
            {
                if (throwAt >= pos && throwAt < pos + c_block)
                    e.throwWhoosh(throwAt - pos, velocity);
                e.process(nullptr, nullptr, o.getWritePointer(0, pos), o.getWritePointer(1, pos), juce::jmin(c_block, o.getNumSamples() - pos));
            }
            throwsSeen = e.getThrowCount();
            stillThrowing = e.isThrowing();
            return o;
        };
        // The level over 20 ms around a sample (both channels).
        auto levelAt = [](const juce::AudioBuffer<float>& o, int centre)
        {
            const int w = static_cast<int>(0.02 * c_rate), start = juce::jlimit(0, o.getNumSamples() - w, centre - w / 2);
            return std::sqrt(0.5f * (o.getRMSLevel(0, start, w) * o.getRMSLevel(0, start, w) + o.getRMSLevel(1, start, w) * o.getRMSLevel(1, start, w)));
        };
        // Markers: the engine says where a throw peaks, where the passes go by and where the hits land (for the
        // cue points of a recording).
        {
            struct Mark { int at = 0, kind = 0; };
            auto markRun = [&](CargoHold& from, bool playing, bool link, bool aim, bool throwOne, double length, juce::AudioBuffer<float>& into)
            {
                std::vector<Mark> marks;
                RigEngine e;
                e.setCargoHold(&from);
                RigEngine::Params q;
                q.space = 0.0f;
                q.fill = 0.0f;
                q.hitDuck = 0.0f;
                q.throwSeconds = 2.0f;
                q.energy = q.chaos = 0.0f;
                q.linkBuses = link;
                q.hitAim = aim;
                for (auto& b : q.buses)
                {
                    b.low = b.swirl = b.filter = b.drive = b.duck = b.key = 0.0f;
                    b.passMode = 1; // Fly-By: one pass at a time, so each peak stands out
                }
                e.setParams(q);
                e.prepare(c_rate, c_block);
                e.setParams(q);
                e.setReleased(! playing);
                into.setSize(2, static_cast<int>(length * c_rate));
                for (int pos = 0; pos < into.getNumSamples(); pos += c_block)
                {
                    if (throwOne && throwAt >= pos && throwAt < pos + c_block)
                        e.throwWhoosh(throwAt - pos, 0.8f);
                    e.process(nullptr, nullptr, into.getWritePointer(0, pos), into.getWritePointer(1, pos), juce::jmin(c_block, into.getNumSamples() - pos));
                    for (int i = 0; i < e.getNumMarkers(); ++i)
                        marks.push_back({ pos + e.getMarker(i).offset, e.getMarker(i).kind });
                }
                return marks;
            };
            auto count = [](const std::vector<Mark>& marks, int kind)
            {
                return static_cast<int>(std::count_if(marks.begin(), marks.end(), [kind](const Mark& m) { return m.kind == kind; }));
            };
            juce::AudioBuffer<float> o;
            // A throw with the rig stopped: one Throw marker, where the whoosh is loudest; no pass markers.
            {
                const auto marks = markRun(tones, false, false, true, true, 11.0, o);
                int peakAt = 0;
                float best = 0.0f;
                for (int at = throwAt; at < throwAt + static_cast<int>(2.0 * c_rate); at += 64)
                    if (const float v = levelAt(o, at); v > best)
                    {
                        best = v;
                        peakAt = at;
                    }
                const int expected = throwAt + static_cast<int>(c_rate);
                check(marks.size() == 1 && marks[0].kind == RigEngine::MarkThrow && std::abs(marks[0].at - expected) < 64
                          && std::abs(marks[0].at - peakAt) < static_cast<int>(0.06 * c_rate),
                      "markers: a throw is marked once, at its peak (marker " + juce::String(marks.empty() ? -1.0 : (marks[0].at - throwAt) / c_rate, 3)
                          + " s after the throw, loudest at " + juce::String((peakAt - throwAt) / c_rate, 3) + " s; " + juce::String(static_cast<int>(marks.size())) + " markers)");
            }
            // With hit tracks and AIM, the hit that lands on the throw's peak is marked too, at the same moment.
            {
                const auto marks = markRun(tonesAndHits, false, false, true, true, 11.0, o);
                int throwMark = -1, hitMark = -1;
                for (const auto& m : marks)
                    (m.kind == RigEngine::MarkThrow ? throwMark : hitMark) = m.at;
                check(count(marks, RigEngine::MarkThrow) == 1 && count(marks, RigEngine::MarkHit) == 1 && std::abs(throwMark - hitMark) < static_cast<int>(0.02 * c_rate),
                      "markers: the aimed hit is marked where the throw peaks (" + juce::String((hitMark - throwMark) * 1000.0 / c_rate, 1) + " ms apart)");
            }
            // The rig playing, buses linked: bus A's passes are marked (bus B flies the same ones), each where the level stands out.
            {
                const auto marks = markRun(tones, true, true, true, false, 30.0, o);
                const int passes = count(marks, RigEngine::MarkPassA);
                double atMarks = 0.0;
                int used = 0;
                for (const auto& m : marks)
                    if (m.kind == RigEngine::MarkPassA && m.at > static_cast<int>(3.0 * c_rate))
                    {
                        atMarks += levelAt(o, m.at);
                        ++used;
                    }
                atMarks /= juce::jmax(1, used);
                const double overall = std::sqrt(0.5 * (static_cast<double>(o.getRMSLevel(0, 0, o.getNumSamples())) * o.getRMSLevel(0, 0, o.getNumSamples())
                                                        + static_cast<double>(o.getRMSLevel(1, 0, o.getNumSamples())) * o.getRMSLevel(1, 0, o.getNumSamples())));
                check(passes >= 6 && passes <= 40 && count(marks, RigEngine::MarkPassB) == 0 && count(marks, RigEngine::MarkThrow) == 0,
                      "markers: linked, every pass is marked once (" + juce::String(passes) + " in 30 s)");
                check(atMarks > 1.3 * overall, "markers: the pass markers sit on the loud moments (" + juce::String(20.0 * std::log10(atMarks / juce::jmax(1.0e-9, overall)), 1)
                                                   + " dB over the take's level)");
                // Unlinked, bus B flies its own passes and they get their own markers.
                const auto apart = markRun(tones, true, false, true, false, 30.0, o);
                check(count(apart, RigEngine::MarkPassA) >= 6 && count(apart, RigEngine::MarkPassB) >= 6,
                      "markers: unlinked, each bus's passes are marked (" + juce::String(count(apart, RigEngine::MarkPassA)) + " on A, "
                          + juce::String(count(apart, RigEngine::MarkPassB)) + " on B)");
            }
            // The hit gate (AIM off): every opening of the gate is a Hit marker.
            {
                const auto marks = markRun(tonesAndHits, true, true, false, false, 30.0, o);
                check(count(marks, RigEngine::MarkHit) >= 1, "markers: the hit gate opening is marked (" + juce::String(count(marks, RigEngine::MarkHit)) + " in 30 s)");
            }
        }

        for (const float length : { 2.0f, 0.6f })
        {
            int count = 0;
            bool still = true;
            const auto o = throwRun(tones, length, 0.8f, false, count, still);
            const int expected = throwAt + static_cast<int>(0.5 * length * c_rate);
            int peakAt = 0;
            float top = 0.0f;
            for (int i = 0; i < o.getNumSamples(); i += 24)
                if (const float l = levelAt(o, i); l > top) { top = l; peakAt = i; }
            const float before = o.getMagnitude(throwAt - static_cast<int>(c_rate), static_cast<int>(c_rate)), after = o.getMagnitude(static_cast<int>(throwAt + (length + 0.5) * c_rate), static_cast<int>(1.0 * c_rate));
            const double offMs = 1000.0 * (peakAt - expected) / c_rate;
            std::cout << "  throw " << length << " s from rest: peak " << juce::Decibels::gainToDecibels(top) << " dB, " << offMs
                      << " ms from the predicted time; before " << juce::Decibels::gainToDecibels(before, -200.0f) << " dB, after "
                      << juce::Decibels::gainToDecibels(after, -200.0f) << " dB" << std::endl;
            check(count == 1 && top > 0.05f, "throw: one whoosh is heard from rest");
            check(std::abs(offMs) <= 5.0, "throw: the peak lands half a Throw Length after the throw (within 5 ms)");
            check(before < 1.0e-6f, "throw: silent until it is thrown");
            check(after < 1.0e-4f && ! still, "throw: back to silence after its tail");
            // One swell: up to the peak and down again, nothing else.
            bool oneSwell = true;
            const int step = static_cast<int>(0.05 * c_rate);
            for (int i = throwAt + step; i < peakAt - step; i += step)
                oneSwell = oneSwell && levelAt(o, i) >= 0.7f * levelAt(o, i - step);
            for (int i = peakAt + 2 * step; i < o.getNumSamples() - step; i += step)
                oneSwell = oneSwell && levelAt(o, i) <= 1.4f * levelAt(o, i - step) + 1.0e-5f;
            check(oneSwell, "throw: a single swell (approach, peak, tail)");
        }
        {
            // Velocity: a soft throw is quieter than a hard one. With the rig playing, a throw goes on top.
            int count = 0;
            bool still = false;
            const auto hard = throwRun(tones, 2.0f, 1.0f, false, count, still), soft = throwRun(tones, 2.0f, 0.1f, false, count, still);
            const int at = throwAt + static_cast<int>(c_rate), span = static_cast<int>(0.2 * c_rate);
            const float diff = juce::Decibels::gainToDecibels(soft.getRMSLevel(0, at - span / 2, span) + soft.getRMSLevel(1, at - span / 2, span))
                             - juce::Decibels::gainToDecibels(hard.getRMSLevel(0, at - span / 2, span) + hard.getRMSLevel(1, at - span / 2, span));
            std::cout << "  throw velocity: 10% is " << diff << " dB against 100% at the peak" << std::endl;
            check(diff < -6.0f, "throw: velocity sets how close and loud the pass is");
            const auto riding = throwRun(tones, 2.0f, 1.0f, true, count, still);
            check(count == 1 && riding.getRMSLevel(0, at - span / 2, span) + riding.getRMSLevel(1, at - span / 2, span)
                                    > 0.7f * (hard.getRMSLevel(0, at - span / 2, span) + hard.getRMSLevel(1, at - span / 2, span)),
                  "throw: with the rig playing, the throw goes on top of the ride");
        }
        {
            // The hits: the same throw with and without the hit track; what differs is the hit, and it starts on the peak.
            int count = 0;
            bool still = false;
            const auto with = throwRun(tonesAndHits, 2.0f, 0.8f, false, count, still), without = throwRun(tones, 2.0f, 0.8f, false, count, still);
            const int expected = throwAt + static_cast<int>(c_rate);
            float top = 0.0f;
            for (int i = 0; i < with.getNumSamples(); ++i)
                top = std::max(top, std::abs(with.getSample(0, i) - without.getSample(0, i)));
            int onset = -1, last = -1;
            for (int i = 0; i < with.getNumSamples(); ++i)
                if (std::abs(with.getSample(0, i) - without.getSample(0, i)) > 0.2f * top)
                {
                    if (onset < 0) onset = i;
                    last = i;
                }
            const double offMs = 1000.0 * (onset - expected) / c_rate;
            std::cout << "  throw + hits: the hit (" << juce::Decibels::gainToDecibels(top, -200.0f) << " dB) starts " << offMs
                      << " ms from the peak and is over " << 1000.0 * (last - expected) / c_rate << " ms after it" << std::endl;
            check(top > 0.05f, "throw: AIM fires the hits for a throw from rest");
            check(std::abs(offMs) <= 10.0, "throw: the hits land on the throw's peak (within 10 ms)");
            check(last - expected < static_cast<int>(1.0 * c_rate), "throw: the hits fire once");
        }
        {
            // The throw's hit is as loud with the rig playing as from rest. The faders are still and all the way
            // up here, so a playing rig already has the hit track at its fader level: the throw must not add the
            // same hit a second time (it would come out 6 dB hotter).
            // One hit in the file, so every cue plays the same sound.
            juce::AudioBuffer<float> oneHit(1, static_cast<int>(c_rate * 2.0));
            oneHit.clear();
            for (int i = 0; i < static_cast<int>(c_rate * 0.3); ++i)
                oneHit.setSample(0, i, static_cast<float>(0.8 * std::exp(-(i / c_rate) / 0.08) * std::sin(juce::MathConstants<double>::twoPi * 55.0 * i / c_rate)));
            CargoHold tonesAndOneHit;
            tonesAndOneHit.loadFile(0, writeWav(folder, "rig_src_tone1000", steadyTone(1000.0)), error);
            tonesAndOneHit.loadFile(1, writeWav(folder, "rig_src_tone1500", steadyTone(1500.0)), error);
            tonesAndOneHit.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
            tonesAndOneHit.setRoleOverride(1, static_cast<int>(CargoRole::Bed));
            tonesAndOneHit.setSkipSilence(0, false);
            tonesAndOneHit.setSkipSilence(1, false);
            tonesAndOneHit.loadFile(2, writeWav(folder, "rig_src_onehit", oneHit), error);
            tonesAndOneHit.setRoleOverride(2, static_cast<int>(CargoRole::Impact));
            stillFaders = true;
            int count = 0;
            bool still = false;
            const int expected = throwAt + static_cast<int>(c_rate), span = static_cast<int>(0.25 * c_rate);
            auto hitPeak = [&](bool playing)
            {
                const auto with = throwRun(tonesAndOneHit, 2.0f, 0.8f, playing, count, still), without = throwRun(tones, 2.0f, 0.8f, playing, count, still);
                float top = 0.0f;
                for (int i = expected - span / 5; i < expected + span; ++i)
                    top = std::max(top, std::abs(with.getSample(0, i) - without.getSample(0, i)));
                return top;
            };
            const float rest = hitPeak(false), riding = hitPeak(true);
            stillFaders = false;
            const float diff = juce::Decibels::gainToDecibels(riding / std::max(rest, 1.0e-9f));
            std::cout << "  throw + hits: the hit peaks at " << juce::Decibels::gainToDecibels(rest, -200.0f) << " dB from rest and "
                      << juce::Decibels::gainToDecibels(riding, -200.0f) << " dB with the rig playing (" << diff << " dB apart)" << std::endl;
            check(rest > 0.05f && std::abs(diff) < 1.5f, "throw: its hit is as loud with the rig playing as from rest");
        }

        // Keys: held keys add their distance from middle C to the tracks' pitches, dealt out over the loaded tracks.
        {
            CargoHold three;
            for (int t = 0; t < 3; ++t)
            {
                three.loadFile(t * 2, writeWav(folder, "rig_src_tone400", steadyTone(400.0)), error); // tracks 1, 3 and 5: the empty ones are skipped
                three.setRoleOverride(t * 2, static_cast<int>(CargoRole::Tonal));
                three.setSkipSilence(t * 2, false);
            }
            RigEngine e;
            e.setCargoHold(&three);
            RigEngine::Params q;
            q.rideOn = false;
            q.space = 0.0f;
            q.fill = 0.0f;
            q.trackVari[4] = false; // the third tone goes through the pitch shifter
            for (auto& b : q.buses)
                b.bend = b.low = b.swirl = b.filter = b.drive = 0.0f; // no doppler bend: the pitches as played
            e.setParams(q);
            e.prepare(c_rate, c_block);
            e.setParams(q);
            auto run = [&](double length)
            {
                juce::AudioBuffer<float> o(2, static_cast<int>(length * c_rate));
                for (int pos = 0; pos < o.getNumSamples(); pos += c_block)
                    e.process(nullptr, nullptr, o.getWritePointer(0, pos), o.getWritePointer(1, pos), juce::jmin(c_block, o.getNumSamples() - pos));
                return o;
            };
            // The level of one frequency over the last second of a render (dB).
            auto tone = [](const juce::AudioBuffer<float>& o, double hz)
            {
                const int n = static_cast<int>(c_rate), start = o.getNumSamples() - n;
                double re = 0.0, im = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    const double hann = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * i / n);
                    const double x = hann * (o.getSample(0, start + i) + o.getSample(1, start + i));
                    re += x * std::cos(juce::MathConstants<double>::twoPi * hz * i / c_rate);
                    im += x * std::sin(juce::MathConstants<double>::twoPi * hz * i / c_rate);
                }
                return 10.0 * std::log10((re * re + im * im) / (static_cast<double>(n) * n) + 1.0e-20);
            };
            const double f0 = 400.0, f4 = 400.0 * std::pow(2.0, 4.0 / 12.0), f7 = 400.0 * std::pow(2.0, 7.0 / 12.0);
            const auto none = run(3.0);
            q.keys[0] = 0.0f; q.keys[1] = 4.0f; q.keys[2] = 7.0f; // C, E and G above middle C
            q.numKeys = 3;
            e.setParams(q);
            const auto chord = run(3.0);
            // The shifted track: where its tone is (the strongest frequency within a semitone of +7).
            double shiftedHz = 0.0, shiftedDb = -200.0;
            for (double hz = f7 / 1.06; hz < f7 * 1.06; hz += 0.5)
                if (const double level = tone(chord, hz); level > shiftedDb) { shiftedDb = level; shiftedHz = hz; }
            std::cout << "  keys: the pitch-shifted track is at " << shiftedHz << " Hz (" << 1200.0 * std::log2(shiftedHz / f7) << " cents from +7 st), " << shiftedDb << " dB" << std::endl;
            std::cout << "  keys: no keys " << tone(none, f0) << " / " << tone(none, f4) << " / " << tone(none, f7) << " dB at 0 / +4 / +7 st; C E G held "
                      << tone(chord, f0) << " / " << tone(chord, f4) << " / " << tone(chord, f7) << " dB" << std::endl;
            check(tone(none, f0) > tone(none, f4) + 30.0 && tone(none, f0) > tone(none, f7) + 30.0, "keys: no keys held, every track at its own pitch");
            check(std::abs(tone(chord, f4) - tone(chord, f0)) < 8.0 && std::abs(shiftedDb - tone(chord, f0)) < 12.0
                      && std::abs(1200.0 * std::log2(shiftedHz / f7)) < 15.0
                      && tone(chord, f4) > tone(none, f4) + 30.0 && tone(chord, f7) > tone(none, f7) + 30.0,
                  "keys: C, E and G held: the three tracks play 0, +4 and +7 semitones (the third through the pitch shifter)");
            check(e.getKeyPitch(0) == 0.0f && e.getKeyPitch(2) == 4.0f && e.getKeyPitch(4) == 7.0f && e.getKeyPitch(1) == 0.0f,
                  "keys: dealt out over the loaded tracks in order");
            // Released: glides back. A step in pitch would click; the level may not jump while it glides.
            q.numKeys = 0;
            e.setParams(q);
            const auto glideBack = run(0.25);
            float jump = 0.0f;
            for (int i = 1; i < glideBack.getNumSamples(); ++i)
                jump = std::max(jump, std::abs(glideBack.getSample(0, i) - glideBack.getSample(0, i - 1)));
            const auto back = run(2.0);
            std::cout << "  keys released: " << tone(back, f0) << " / " << tone(back, f4) << " / " << tone(back, f7) << " dB; largest step while gliding " << jump << std::endl;
            check(tone(back, f0) > tone(back, f4) + 30.0 && tone(back, f0) > tone(back, f7) + 30.0, "keys: releasing the keys returns to the knobs");
            check(jump < 0.25f, "keys: the pitch glides (no click)");
            // One key moves every track; the limits hold.
            q.keys[0] = 30.0f;
            q.numKeys = 1;
            e.setParams(q);
            run(1.0);
            check(std::abs(e.getTrackSpeed(0) - 4.0f) < 0.01f && std::abs(e.getTrackSpeed(2) - 4.0f) < 0.01f && e.getTrackSpeed(4) == 1.0f,
                  "keys: one key moves every track, held to two octaves (VARI off keeps its speed)");
        }

        // Seeded restart: the same settings give the same ride again.
        {
            RigEngine e;
            e.setCargoHold(&hold);
            RigEngine::Params q;
            e.setParams(q);
            e.prepare(c_rate, c_block);
            e.setParams(q);
            auto take = [&](double playFirst)
            {
                juce::AudioBuffer<float> skip(2, c_block), o(2, static_cast<int>(4.0 * c_rate));
                // (the first 32 samples are rendered again and again below until the new run starts)
                for (int n = 0; n < static_cast<int>(playFirst * c_rate) / c_block; ++n)
                    e.process(nullptr, nullptr, skip.getWritePointer(0), skip.getWritePointer(1), c_block);
                e.restartSeeded();
                int pos = 0;
                do // 32 samples at a time until the old sound has faded and the new run has started
                    e.process(nullptr, nullptr, o.getWritePointer(0), o.getWritePointer(1), 32);
                while (e.isRestarting() && ++pos < 4000);
                for (pos = 32; pos < o.getNumSamples(); pos += c_block)
                    e.process(nullptr, nullptr, o.getWritePointer(0, pos), o.getWritePointer(1, pos), juce::jmin(c_block, o.getNumSamples() - pos));
                return o;
            };
            const auto first = take(1.3), second = take(2.9);
            float diff = 0.0f;
            for (int i = 0; i < first.getNumSamples(); ++i)
                diff = std::max(diff, std::abs(first.getSample(0, i) - second.getSample(0, i)));
            std::cout << "  seeded restart: two starts differ by at most " << juce::Decibels::gainToDecibels(diff, -200.0f) << " dB (level "
                      << juce::Decibels::gainToDecibels(first.getRMSLevel(0, 0, first.getNumSamples()), -200.0f) << " dB)" << std::endl;
            check(first.getRMSLevel(0, 0, first.getNumSamples()) > 0.01f && diff < 1.0e-5f, "seeded restart: the ride repeats from the transport start");
        }
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

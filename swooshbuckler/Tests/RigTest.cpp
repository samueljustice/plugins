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
            mods.process(set, 256, 0.1f);
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
            mods.process(set, 256, 0.1f);
        check(std::abs(mods.offset(tFill)) < 1.0e-4f, "mods: a pulled wire glides back to zero");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

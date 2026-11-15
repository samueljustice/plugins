// Offline test for where a hit lands: Hit Position, Hit Spread, and stereo sounds that are brought to a point.
//
//   SwooshbucklerHits [output folder] [only these parts]
//
// What is measured is the sound on each speaker (its power, block by block, and the energy centroid: every speaker's
// place in the room weighted by its power), against where the engine says the hits and the passes are.
//
//   pass       With the Pass, at 5.1, 7.1 and 7.1.4, on every Flight Path both ways: a hit's energy is where its
//              pass's is, block by block, and as high; it moves with the pass
//   which      which pass, when several fly; the front when none does; a thrown whoosh's hit; a linked bus
//   front      Front: the front speakers only
//   random     Random: thirty hits land in front, beside and behind, the same again from the same seed, other places
//              from another; never one speaker alone
//   spread     Hit Spread 0, 50 and 100%: the share of a hit's power outside the speakers nearest it, at one power
//   loudness   the same hit at Hit Spread 0, 25, 50 and 100% reads the same (plain power and BS.1770) whether its file
//              is mono, stereo with two sides alike, stereo with two unrelated sides, stereo with opposed sides, or
//              stereo with sides half opposed: at Mono, Stereo, 5.1, 7.1 and 7.1.4
//   clicks     nothing steps when Hit Position or Hit Spread change during a hit
//   stereo     at Stereo a hit pans with its pass; at Mono neither control does anything; Front with a mono hit is
//              what the hits were before they had a place
//   beds       a stereo bed brought to a point (Width, Doppler, Spread) keeps its level, for the same kinds of file
//   plugin     through the whole plug-in: the limiter does no more for a hit brought to a point than for one spread
//              out, and no peak passes the Ceiling

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "Cargo.h"
#include "RigEngine.h"
#include "Room.h"

#include <cmath>
#include <functional>
#include <iostream>

namespace
{
    constexpr double c_fs = 48000.0;
    constexpr int c_block = 256;
    constexpr int c_wide = room::c_maxChannels;
    constexpr double c_twoPi = 6.283185307179586;
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

    const room::Width c_rooms[] = { room::Width::Surround51, room::Width::Surround71, room::Width::Surround714 };

    juce::File writeWav(const juce::File& folder, const juce::String& name, const juce::AudioBuffer<float>& b)
    {
        auto f = folder.getChildFile(name + ".wav");
        f.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(f);
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream, c_fs, static_cast<unsigned int>(b.getNumChannels()), 24, {}, 0));
        if (w != nullptr) w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()); else delete stream;
        return f;
    }

    // A steady hiss: what the passes are made of.
    juce::AudioBuffer<float> hiss(int seed)
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
        return b;
    }

    // One long hit: a burst and a tail that rings for over a second, so it is heard to travel.
    juce::AudioBuffer<float> longHit()
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 1.6));
        juce::Random r(9);
        float y = 0.0f;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = i / c_fs;
            y += 0.4f * ((r.nextFloat() * 2.0f - 1.0f) - y);
            b.setSample(0, i, static_cast<float>((0.7 * std::exp(-t / 0.03) + 0.5 * std::exp(-t / 0.5)) * y * std::min(1.0, (1.6 - t) / 0.1)));
        }
        return b;
    }

    // Bursts every quarter second, as a file of each kind: 0 mono, 1 stereo with two sides alike, 2 two unrelated
    // sides, 3 opposed sides, 4 sides half opposed. steady: a hiss with no bursts (for the beds).
    constexpr int c_kinds = 5;
    const char* const c_kindNames[c_kinds] = { "mono", "stereo alike", "stereo unrelated", "stereo opposed", "stereo half opposed" };
    juce::AudioBuffer<float> kindOf(int kind, bool steady)
    {
        const int n = static_cast<int>(c_fs * 4.0);
        juce::AudioBuffer<float> b(kind == 0 ? 1 : 2, n);
        juce::Random r1(21), r2(22);
        float a = 0.0f, c = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const double t = std::fmod(i / c_fs, 0.25);
            const float env = steady ? 0.5f : static_cast<float>(0.8 * std::exp(-t / 0.04) * std::min(1.0, (0.25 - t) / 0.01));
            a += 0.35f * ((r1.nextFloat() * 2.0f - 1.0f) - a);
            c += 0.35f * ((r2.nextFloat() * 2.0f - 1.0f) - c);
            const float one = env * a, other = env * c;
            b.setSample(0, i, one);
            if (kind == 1) b.setSample(1, i, 0.97f * one + 0.03f * other);
            if (kind == 2) b.setSample(1, i, other);
            if (kind == 3) b.setSample(1, i, -one);
            if (kind == 4) b.setSample(1, i, -0.4f * one + 0.917f * other);
        }
        return b;
    }

    // A low tone that loops without a seam, its right side a quarter cycle behind its left: any step stands out.
    juce::AudioBuffer<float> stereoTone()
    {
        juce::AudioBuffer<float> b(2, static_cast<int>(c_fs * 4.0));
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            b.setSample(0, i, 0.5f * static_cast<float>(std::sin(c_twoPi * 60.0 * i / c_fs)));
            b.setSample(1, i, 0.5f * static_cast<float>(std::cos(c_twoPi * 60.0 * i / c_fs)));
        }
        return b;
    }

    struct Sources
    {
        juce::File hissA, hissB, longHit, tone;
        juce::File burst[c_kinds], steady[c_kinds];
    };

    struct Block
    {
        std::array<double, c_wide> power {};
        double total = 0.0;
        std::array<RigEngine::ObjectView, RigEngine::c_objects> object {};
        RigEngine::HitView hit;
    };
    struct Take
    {
        std::array<std::vector<float>, c_wide> ch;
        std::vector<Block> blocks;
        int samples() const { return static_cast<int>(ch[0].size()); }
    };

    struct Scene
    {
        CargoHold hold;
        RigEngine rig;
        RigEngine::Params p;

        Scene()
        {
            // The plainest rig: faders still, no colours, no Space, so what is heard is the passes and the hits.
            p.rideOn = false;
            p.space = 0.0f;
            p.fill = 0.0f;
            p.hitDuck = 0.0f;
            p.hitBoost = 0.0f;
            p.hitSpread = 0.0f;
            for (auto& b : p.buses)
            {
                b.low = b.swirl = b.filter = b.drive = b.duck = b.key = 0.0f;
                b.width = 1.0f;
                b.passMode = 1; // Fly-By: one pass at a time
            }
        }
        void load(int track, const juce::File& file, int bus)
        {
            juce::String error;
            if (! hold.loadFile(track, file, error))
                check(false, "loads " + file.getFileName() + " " + error);
            hold.setBusOverride(track, bus);
        }
        void start(room::Width width, std::uint32_t seed = 0)
        {
            rig.setCargoHold(&hold);
            rig.setWidth(width);
            if (seed != 0)
                rig.setSeed(seed);
            rig.setParams(p);
            rig.prepare(c_fs, c_block);
            rig.setParams(p);
        }
        void run(Take& take, int blocks, const std::function<void(int)>& before = {})
        {
            const int from = take.samples();
            for (auto& c : take.ch)
                c.resize(static_cast<size_t>(from + blocks * c_block), 0.0f);
            for (int b = 0; b < blocks; ++b)
            {
                if (before)
                    before(b);
                float* out[c_wide];
                for (int c = 0; c < c_wide; ++c)
                    out[c] = take.ch[static_cast<size_t>(c)].data() + from + b * c_block;
                rig.process(nullptr, nullptr, out, c_wide, c_block);
                Block info;
                for (int c = 0; c < c_wide; ++c)
                {
                    double sum = 0.0;
                    for (int i = 0; i < c_block; ++i)
                        sum += static_cast<double>(out[c][i]) * out[c][i];
                    info.power[static_cast<size_t>(c)] = sum / c_block;
                    info.total += sum / c_block;
                }
                for (int o = 0; o < RigEngine::c_objects; ++o)
                    info.object[static_cast<size_t>(o)] = rig.getObject(o);
                info.hit = rig.getHitView();
                take.blocks.push_back(info);
            }
        }
    };

    using Info = room::SpeakerInfo;
    struct Speakers
    {
        const room::Layout& layout;
        explicit Speakers(room::Width w) : layout(room::layout(w)) {}
        const Info& info(int ch) const { return room::info(layout.speakers[static_cast<size_t>(ch)]); }
        double share(const Block& b, const std::function<bool(const Info&)>& pick) const
        {
            double sum = 0.0;
            for (int ch = 0; ch < layout.numChannels; ++ch)
                if (pick(info(ch)))
                    sum += b.power[static_cast<size_t>(ch)];
            return b.total > 0.0 ? sum / b.total : 0.0;
        }
        room::Position centroid(const Block& b) const
        {
            room::Position c { 0.0f, 0.0f, 0.0f };
            for (int ch = 0; ch < layout.numChannels && b.total > 0.0; ++ch)
            {
                const double w = b.power[static_cast<size_t>(ch)] / b.total;
                c.x += static_cast<float>(w * info(ch).x);
                c.y += static_cast<float>(w * info(ch).y);
                c.z += static_cast<float>(w * info(ch).z);
            }
            return c;
        }
        double most(const Block& b) const
        {
            double top = 0.0;
            for (int ch = 0; ch < layout.numChannels; ++ch)
                top = std::max(top, b.power[static_cast<size_t>(ch)]);
            return b.total > 0.0 ? top / b.total : 0.0;
        }
    };
    bool isTop(const Info& s) { return s.role == room::Role::Height; }
    float apart(const room::Position& a, const room::Position& b)
    {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    }
    float apart(const RigEngine::ObjectView& a, const RigEngine::ObjectView& b) { return apart(room::Position { a.x, a.y, a.z }, room::Position { b.x, b.y, b.z }); }

    /** The largest change of step between samples on any channel (a click is a step its neighbours do not share). */
    float secondStep(const Take& take, int from, int to)
    {
        float worst = 0.0f;
        for (const auto& c : take.ch)
            for (int i = std::max(2, from); i < std::min(to, static_cast<int>(c.size())); ++i)
                worst = std::max(worst, std::abs(c[static_cast<size_t>(i)] - 2.0f * c[static_cast<size_t>(i - 1)] + c[static_cast<size_t>(i - 2)]));
        return worst;
    }

    // BS.1770: each speaker through the K weighting (48 kHz), its mean square weighted by where it stands, summed.
    double kWeightedPower(const std::vector<float>& x, size_t from)
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
    struct Level { double power = 0.0, loudness = 0.0; };
    /** A take's plain power and BS.1770 loudness at a width (Mono: the two channels folded to one). */
    Level levelOf(const Take& take, room::Width width, size_t from)
    {
        Level l;
        if (width == room::Width::Mono)
        {
            std::vector<float> mono(take.ch[0].size());
            for (size_t i = 0; i < mono.size(); ++i)
                mono[i] = 0.5f * (take.ch[0][i] + take.ch[1][i]);
            for (size_t i = from; i < mono.size(); ++i)
                l.power += static_cast<double>(mono[i]) * mono[i];
            l.power = db(l.power / static_cast<double>(mono.size() - from));
            l.loudness = -0.691 + db(kWeightedPower(mono, from));
            return l;
        }
        const auto& lay = room::layout(width);
        double weighted = 0.0;
        for (int c = 0; c < lay.numChannels; ++c)
        {
            const auto& x = take.ch[static_cast<size_t>(c)];
            for (size_t i = from; i < x.size(); ++i)
                l.power += static_cast<double>(x[i]) * x[i];
            weighted += room::loudnessWeight(lay.speakers[static_cast<size_t>(c)]) * kWeightedPower(x, from);
        }
        l.power = db(l.power / static_cast<double>(take.ch[0].size() - from));
        l.loudness = -0.691 + db(weighted);
        return l;
    }

    Sources makeSources(const juce::File& folder)
    {
        Sources s;
        s.hissA = writeWav(folder, "hissA", hiss(1));
        s.hissB = writeWav(folder, "hissB", hiss(2));
        s.longHit = writeWav(folder, "longHit", longHit());
        s.tone = writeWav(folder, "tone", stereoTone());
        for (int k = 0; k < c_kinds; ++k)
        {
            s.burst[k] = writeWav(folder, "burst" + juce::String(k), kindOf(k, false));
            s.steady[k] = writeWav(folder, "steady" + juce::String(k), kindOf(k, true));
        }
        return s;
    }

    using Proc = SwooshbucklerAudioProcessor;
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("SwooshbucklerHitsTest");
    folder.createDirectory();
    const auto src = makeSources(folder);
    const juce::String only = argc > 2 ? juce::String(argv[2]) : juce::String();
    auto wanted = [&](const char* key) { return only.isEmpty() || only.contains(key); };

    juce::PropertiesFile::Options options;
    options.applicationName = "SwooshbucklerHitsTest";
    options.filenameSuffix = ".settings";
    const auto settingsFile = folder.getChildFile("test.settings");
    settingsFile.deleteFile();
    juce::PropertiesFile settings(settingsFile, options);
    settings.setValue("recordFolder", folder.getChildFile("tape").getFullPathName());
    Proc::useSettings(&settings);

    //==================================================================================================================
    // With the Pass: two runs of the same rig from the same seed. In one the bus is heard and the hits are silent;
    // in the other the hits are heard and the bus all but silent. Block by block the hits' energy is where the
    // pass's is.
    if (wanted("pass"))
    {
        std::cout << "With the Pass: a hit's energy is where its pass's is" << std::endl;
        for (const auto width : c_rooms)
        {
            const Speakers spk(width);
            float worstApart = 0.0f, worstTop = 0.0f, leastTravel = 10.0f;
            int hitsHeard = 0;
            bool samePasses = true;
            for (int path = 0; path < room::c_numPaths; ++path)
                for (int direction = 1; direction <= 2; ++direction)
                {
                    auto play = [&](bool hearHits, Take& take)
                    {
                        Scene scene;
                        scene.load(0, src.hissA, 0);
                        scene.load(2, src.longHit, CargoHold::c_hitsBus);
                        scene.p.levels[0] = hearHits ? 0.01f : 0.9f;
                        scene.p.levels[2] = hearHits ? 0.9f : 0.0f;
                        scene.p.buses[0].path = path;
                        scene.p.buses[0].direction = direction;
                        scene.p.hitAim = true;
                        scene.p.hitSense = 1.0f;
                        scene.p.hitPosition = RigEngine::HitWithPass;
                        scene.p.rate = 0.9f;
                        scene.start(width);
                        scene.run(take, static_cast<int>(24.0 * c_fs / c_block));
                    };
                    Take bus, hit;
                    play(false, bus);
                    play(true, hit);
                    // One hit at a time: from where it is loud to where it has died away 20 dB.
                    double top = 0.0;
                    for (const auto& b : hit.blocks)
                        top = std::max(top, b.total);
                    size_t b = 200;
                    while (b < hit.blocks.size())
                    {
                        if (hit.blocks[b].total < 0.25 * top) { ++b; continue; }
                        const size_t first = b;
                        double peak = 0.0;
                        while (b < hit.blocks.size() && hit.blocks[b].total > 0.01 * std::max(peak, hit.blocks[b].total))
                        {
                            peak = std::max(peak, hit.blocks[b].total);
                            ++b;
                        }
                        if (b - first < 20)
                            continue;
                        // Compared where the pass the hit goes with is what the bus sounds like: 12 dB over the
                        // bus's other pass (a Fly-By's next pass is on its way in while this one's tail goes out).
                        double sumApart = 0.0, sumTop = 0.0, weight = 0.0;
                        int compared = 0;
                        size_t firstCompared = 0, lastCompared = 0;
                        for (size_t k = first; k < b; ++k)
                        {
                            samePasses = samePasses && apart(hit.blocks[k].object[RigEngine::ObjPassA1], bus.blocks[k].object[RigEngine::ObjPassA1]) < 1.0e-4f;
                            const auto& objects = hit.blocks[k].object;
                            const float to1 = apart(objects[RigEngine::ObjHits], objects[RigEngine::ObjPassA1]), to2 = apart(objects[RigEngine::ObjHits], objects[RigEngine::ObjPassA2]);
                            const int with = to1 <= to2 ? RigEngine::ObjPassA1 : RigEngine::ObjPassA2, other = to1 <= to2 ? RigEngine::ObjPassA2 : RigEngine::ObjPassA1;
                            if (std::min(to1, to2) > 0.02f || bus.blocks[k].object[static_cast<size_t>(with)].level < 4.0f * bus.blocks[k].object[static_cast<size_t>(other)].level)
                                continue;
                            if (compared++ == 0)
                                firstCompared = k;
                            lastCompared = k;
                            const double w = hit.blocks[k].total;
                            sumApart += w * apart(spk.centroid(hit.blocks[k]), spk.centroid(bus.blocks[k]));
                            sumTop += w * std::abs(spk.share(hit.blocks[k], isTop) - spk.share(bus.blocks[k], isTop));
                            weight += w;
                        }
                        if (compared < 20)
                            continue;
                        ++hitsHeard;
                        worstApart = std::max(worstApart, static_cast<float>(sumApart / weight));
                        worstTop = std::max(worstTop, static_cast<float>(sumTop / weight));
                        leastTravel = std::min(leastTravel, apart(spk.centroid(hit.blocks[firstCompared]), spk.centroid(hit.blocks[lastCompared])));
                    }
                }
            std::cout << "         " << nameOf(width) << ": " << hitsHeard << " hits on four paths both ways; a hit's energy is at most " << num(worstApart)
                      << " of the room from its pass's (mean over the hit), its share overhead within " << num(worstTop)
                      << "; over its length a hit travels at least " << num(leastTravel) << std::endl;
            check(samePasses, nameOf(width) + ": the two runs flew the same passes");
            check(hitsHeard >= 16 && worstApart < 0.15f, nameOf(width) + ": With the Pass, a hit's energy is where its pass's is on every path, both ways");
            check(worstTop < 0.1f, nameOf(width) + ": and as high (its share in the speakers overhead is its pass's)");
            check(leastTravel > 0.25f, nameOf(width) + ": and it travels with its pass for its length");
        }
    }

    //==================================================================================================================
    if (wanted("which"))
    {
        std::cout << "Which pass a hit goes with" << std::endl;
        const auto width = room::Width::Surround51;
        const Speakers spk(width);
        auto nearestObject = [](const Block& b, int from, int to)
        {
            float best = 10.0f;
            for (int o = from; o <= to; ++o)
                best = std::min(best, apart(b.object[RigEngine::ObjHits], b.object[static_cast<size_t>(o)]));
            return best;
        };
        // Aimed, two buses on different paths: every hit is at one of the passes, the one it was aimed at.
        for (const bool link : { false, true })
        {
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.load(1, src.hissB, 1);
            scene.load(2, src.longHit, CargoHold::c_hitsBus);
            scene.p.buses[0].path = static_cast<int>(room::Path::Through);
            scene.p.buses[1].path = static_cast<int>(room::Path::Across);
            scene.p.linkBuses = link;
            scene.p.hitAim = true;
            scene.p.hitSense = 1.0f;
            scene.start(width);
            Take take;
            scene.run(take, static_cast<int>(20.0 * c_fs / c_block));
            int heard = 0, with = 0, withA = 0;
            for (size_t b = 200; b < take.blocks.size(); ++b)
                if (take.blocks[b].hit.level > 0.3f)
                {
                    ++heard;
                    with += nearestObject(take.blocks[b], RigEngine::ObjPassA1, RigEngine::ObjPassB2) < 0.02f ? 1 : 0;
                    withA += nearestObject(take.blocks[b], RigEngine::ObjPassA1, RigEngine::ObjPassA2) < 0.02f ? 1 : 0;
                }
            std::cout << "         aimed" << (link ? ", bus B linked" : "") << ": of " << heard << " blocks with a hit sounding, " << with << " are at a pass ("
                      << withA << " at one of bus A's)" << std::endl;
            check(heard > 100 && with > heard * 95 / 100, juce::String("aimed") + (link ? ", linked" : "") + ": a hit is where the pass it was aimed at is");
            if (link)
                check(withA == with, "linked: bus B's passes are bus A's, so every hit is where one of A's is");
            else
                check(withA > 0 && withA < with, "two buses: some hits go with bus A's passes, some with bus B's");
        }
        // Not aimed (the gate held open): the hits go with the loudest pass in flight, and move on when it has gone.
        {
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.load(1, src.hissB, 1);
            scene.load(2, src.steady[0], CargoHold::c_hitsBus);
            scene.p.buses[0].path = static_cast<int>(room::Path::Through);
            scene.p.buses[1].path = static_cast<int>(room::Path::Around);
            scene.p.levels[1] = 0.3f;
            scene.p.hitAim = false;
            scene.p.gateMode = 3;
            scene.start(width);
            Take take;
            scene.run(take, static_cast<int>(20.0 * c_fs / c_block));
            int with = 0, picks = 0, loudestPicked = 0, all = 0;
            int was = -1;
            for (size_t b = 400; b < take.blocks.size(); ++b)
            {
                const auto& block = take.blocks[b];
                int at = -1, loudest = RigEngine::ObjPassA1;
                for (int o = RigEngine::ObjPassA1; o <= RigEngine::ObjPassB2; ++o)
                {
                    if (apart(block.object[RigEngine::ObjHits], block.object[static_cast<size_t>(o)]) < 0.02f)
                        at = o;
                    if (block.object[static_cast<size_t>(o)].level > block.object[static_cast<size_t>(loudest)].level)
                        loudest = o;
                }
                ++all;
                with += at >= 0 ? 1 : 0;
                if (at >= 0 && at != was && was >= 0)
                {
                    ++picks;
                    // (The loudest by the engine's own reckoning: the pass's window times its bus's level. Bus A is
                    // three times bus B here, so one of A's.)
                    loudestPicked += (at == loudest || at <= RigEngine::ObjPassA2) ? 1 : 0;
                }
                if (at >= 0)
                    was = at;
            }
            std::cout << "         not aimed: " << with << " of " << all << " blocks at a pass; it moved on " << picks << " times, " << loudestPicked << " of them to the loudest bus's pass" << std::endl;
            check(with > all * 9 / 10 && picks >= 3, "not aimed: the hits go with a pass, and move on to another when it has gone");
            check(loudestPicked == picks, "not aimed: each time, the loudest pass in flight");
        }
        // Nothing in flight: the front.
        {
            Scene scene;
            scene.load(2, src.steady[0], CargoHold::c_hitsBus);
            scene.p.hitAim = false;
            scene.p.gateMode = 3;
            scene.start(width);
            Take take;
            scene.run(take, 400);
            const auto& last = take.blocks.back();
            const double front = spk.share(last, [](const Info& s) { return s.y > 0.5f; });
            check(last.total > 0.0 && front > 0.99 && std::abs(last.hit.x) < 1.0e-3f && last.hit.y > 0.999f, "With the Pass and nothing in flight: the hit is at the front");
        }
        // A thrown whoosh from a stopped rig: its hit is where the whoosh is, all the way.
        for (const auto w : c_rooms)
        {
            const Speakers s(w);
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.load(2, src.longHit, CargoHold::c_hitsBus);
            scene.p.buses[0].path = static_cast<int>(room::Path::Through);
            scene.p.hitAim = true;
            scene.p.throwSeconds = 2.0f;
            scene.start(w);
            scene.rig.setReleased(true);
            Take take;
            scene.run(take, 100);
            scene.rig.throwWhoosh(0, 0.8f);
            scene.run(take, static_cast<int>(4.0 * c_fs / c_block));
            float worst = 0.0f;
            int heard = 0;
            for (size_t b = 100; b < take.blocks.size(); ++b)
                if (take.blocks[b].hit.level > 0.3f && take.blocks[b].object[RigEngine::ObjThrow1].active)
                {
                    ++heard;
                    worst = std::max(worst, apart(take.blocks[b].object[RigEngine::ObjHits], take.blocks[b].object[RigEngine::ObjThrow1]));
                }
            std::cout << "         " << nameOf(w) << ", a throw: its hit is within " << num(worst, 3) << " of the whoosh over " << heard << " blocks" << std::endl;
            check(heard > 20 && worst < 0.05f, nameOf(w) + ": a thrown whoosh's hit is where the whoosh is");
        }
    }

    //==================================================================================================================
    if (wanted("front"))
    {
        std::cout << "Front: the front speakers only" << std::endl;
        for (const auto width : c_rooms)
            for (const bool centre : { false, true })
            {
                const Speakers spk(width);
                Scene scene;
                scene.load(0, src.hissA, 0);
                scene.load(2, src.longHit, CargoHold::c_hitsBus);
                scene.p.levels[0] = 0.01f;
                scene.p.buses[0].path = static_cast<int>(room::Path::Through);
                scene.p.hitAim = true;
                scene.p.hitSense = 1.0f;
                scene.p.hitPosition = RigEngine::HitFront;
                scene.p.roomCentre = centre;
                scene.start(width);
                Take take;
                scene.run(take, static_cast<int>(12.0 * c_fs / c_block));
                double front = 0.0, mid = 0.0, total = 0.0;
                for (size_t b = 200; b < take.blocks.size(); ++b)
                    if (take.blocks[b].hit.level > 0.3f)
                    {
                        front += take.blocks[b].total * spk.share(take.blocks[b], [](const Info& s) { return s.y > 0.5f && s.z < 0.5f && s.role == room::Role::Main; });
                        mid += take.blocks[b].total * spk.share(take.blocks[b], [](const Info& s) { return s.x == 0.0f && s.role == room::Role::Main; });
                        total += take.blocks[b].total;
                    }
                std::cout << "         " << nameOf(width) << (centre ? ", Centre Speaker on: " : ": ") << num(front / total, 4) << " of the hits' power in the front speakers, "
                          << num(mid / total, 3) << " in the centre speaker" << std::endl;
                check(total > 0.0 && front / total > 0.99, nameOf(width) + (centre ? ", Centre Speaker on" : "") + ": Front is the front speakers only, whatever the passes do");
                check(centre ? mid / total > 0.85 : mid / total < 1.0e-6, centre ? "and with Centre Speaker on, nearly all the centre speaker" : "and with it off the centre speaker is silent");
            }
    }

    //==================================================================================================================
    if (wanted("random"))
    {
        std::cout << "Random: each hit at a place of its own" << std::endl;
        for (const auto width : c_rooms)
        {
            const Speakers spk(width);
            auto play = [&](std::uint32_t seed, float spread, Take& take)
            {
                Scene scene;
                scene.load(2, src.longHit, CargoHold::c_hitsBus);
                scene.p.hitAim = false;
                scene.p.hitSync = true;
                scene.p.gateMode = 3;
                scene.p.hitPosition = RigEngine::HitRandom;
                scene.p.hitSpread = spread;
                scene.p.roomHeight = 1.0f;
                scene.start(width, seed);
                scene.run(take, static_cast<int>(56.0 * c_fs / c_block));
            };
            Take one, again, other, bled;
            play(0, 0.0f, one);
            play(0, 0.0f, again);
            play(77, 0.0f, other);
            play(0, RigEngine::c_hitSpreadDefault, bled);
            // Each hit at its loudest block.
            auto places = [&](const Take& take, std::vector<double>* mostShare)
            {
                std::vector<room::Position> at;
                double top = 0.0;
                for (const auto& b : take.blocks)
                    top = std::max(top, b.total);
                for (size_t b = 1; b + 1 < take.blocks.size(); ++b)
                    if (take.blocks[b].total > 0.3 * top && take.blocks[b].total >= take.blocks[b - 1].total && take.blocks[b].total > take.blocks[b + 1].total)
                    {
                        // (the loudest block within a quarter second either side)
                        bool loudest = true;
                        for (size_t k = b > 47 ? b - 47 : 0; k < std::min(take.blocks.size(), b + 47); ++k)
                            loudest = loudest && take.blocks[k].total <= take.blocks[b].total;
                        if (! loudest)
                            continue;
                        at.push_back(spk.centroid(take.blocks[b]));
                        if (mostShare != nullptr)
                            mostShare->push_back(spk.most(take.blocks[b]));
                    }
                return at;
            };
            const auto at = places(one, nullptr);
            std::vector<double> most;
            places(bled, &most);
            int front = 0, beside = 0, behind = 0, left = 0, right = 0, high = 0;
            for (const auto& p : at)
            {
                front += p.y > 0.4f ? 1 : 0;
                behind += p.y < -0.4f ? 1 : 0;
                beside += std::abs(p.y) <= 0.4f ? 1 : 0;
                left += p.x < -0.2f ? 1 : 0;
                right += p.x > 0.2f ? 1 : 0;
                high += p.z > 0.3f ? 1 : 0;
            }
            bool same = one.samples() == again.samples(), differs = false;
            for (int c = 0; c < c_wide && same; ++c)
                same = one.ch[static_cast<size_t>(c)] == again.ch[static_cast<size_t>(c)];
            for (int c = 0; c < c_wide; ++c)
                differs = differs || one.ch[static_cast<size_t>(c)] != other.ch[static_cast<size_t>(c)];
            double mostOne = 0.0;
            for (const double m : most)
                mostOne = std::max(mostOne, m);
            std::cout << "         " << nameOf(width) << ": " << at.size() << " hits: " << front << " in front, " << beside << " beside, " << behind << " behind; "
                      << left << " left, " << right << " right; " << high << " high. At Hit Spread " << juce::roundToInt(100.0f * RigEngine::c_hitSpreadDefault)
                      << "% the most one speaker has of a hit is " << num(mostOne) << std::endl;
            check(at.size() >= 30, nameOf(width) + ": thirty hits and more");
            check(front >= 3 && beside >= 3 && behind >= 3 && left >= 5 && right >= 5, nameOf(width) + ": Random hits land in front, beside and behind, left and right");
            check(width != room::Width::Surround714 || high >= 3, nameOf(width) + ": and some of them overhead");
            check(same, nameOf(width) + ": the same seed gives the same hits, sample for sample");
            check(differs, nameOf(width) + ": another seed gives others");
            check(! most.empty() && mostOne < 0.9, nameOf(width) + ": as a hit opens (Hit Spread " + juce::String(juce::roundToInt(100.0f * RigEngine::c_hitSpreadDefault)) + "%) it is never in one speaker alone");
        }
    }

    //==================================================================================================================
    if (wanted("spread"))
    {
        std::cout << "Hit Spread: how far a hit bleeds, at one power" << std::endl;
        for (const auto width : c_rooms)
        {
            const Speakers spk(width);
            double power[4] {}, outside[4] {};
            const float spreads[4] = { 0.0f, 0.5f, 1.0f, RigEngine::c_hitSpreadDefault };
            for (int k = 0; k < 4; ++k)
            {
                Scene scene;
                scene.load(2, src.steady[0], CargoHold::c_hitsBus);
                scene.p.hitAim = false;
                scene.p.gateMode = 3;
                scene.p.hitPosition = RigEngine::HitFront;
                scene.p.hitSpread = spreads[k];
                scene.start(width);
                Take take;
                scene.run(take, 800);
                double total = 0.0, out = 0.0;
                for (size_t b = 300; b < take.blocks.size(); ++b)
                {
                    total += take.blocks[b].total;
                    out += take.blocks[b].total * (1.0 - spk.share(take.blocks[b], [](const Info& s) { return s.y > 0.5f && s.z < 0.5f && s.x != 0.0f; }));
                }
                power[k] = db(total);
                outside[k] = out / total;
            }
            // Every main speaker alike at 100%: all but the two in front.
            int mains = 0;
            for (int ch = 0; ch < spk.layout.numChannels; ++ch)
                mains += spk.info(ch).role != room::Role::Lfe && spk.info(ch).x != 0.0f ? 1 : 0;
            const double evenOutside = 1.0 - 2.0 / mains;
            std::cout << "         " << nameOf(width) << ", a hit at the front: outside L and R " << num(100.0 * outside[0], 1) << "% / " << num(100.0 * outside[1], 1) << "% / "
                      << num(100.0 * outside[2], 1) << "% of its power at Hit Spread 0 / 50 / 100% (every speaker alike: " << num(100.0 * evenOutside, 1)
                      << "%); power " << num(power[1] - power[0], 3) << " and " << num(power[2] - power[0], 3) << " dB against 0%. As it opens ("
                      << juce::roundToInt(100.0f * spreads[3]) << "%): " << num(100.0 * outside[3], 1) << "% outside, each of those speakers "
                      << num(db(outside[3] / (mains - 2)), 1) << " dB under the whole hit" << std::endl;
            check(outside[3] > 0.02 && outside[3] < 0.15, nameOf(width) + ": as it opens a hit bleeds a little (2 to 15% of its power outside the speakers nearest it)");
            check(outside[0] < 0.005, nameOf(width) + ": Hit Spread 0% is a point (nothing outside the speakers nearest it)");
            check(std::abs(outside[2] - evenOutside) < 0.01, nameOf(width) + ": 100% is every main speaker alike");
            // (A mono hit's two objects are one sound, so where both play they add as one: the share outside the
            // nearest speakers grows slowly at first. Half way round the wheel it is a third to a half of the whole way.)
            check(outside[1] > 0.3 * evenOutside && outside[1] < 0.5 * evenOutside, nameOf(width) + ": 50% is part of the way");
            check(std::abs(power[1] - power[0]) < 0.1 && std::abs(power[2] - power[0]) < 0.1, nameOf(width) + ": the power is the same within 0.1 dB");
        }
        // The LFE never, the speakers overhead as far as Height lets them, the centre speaker with Centre Speaker on.
        {
            const auto width = room::Width::Surround714;
            const Speakers spk(width);
            auto shares = [&](float height, bool centre)
            {
                Scene scene;
                scene.load(2, src.steady[0], CargoHold::c_hitsBus);
                scene.p.hitAim = false;
                scene.p.gateMode = 3;
                scene.p.hitPosition = RigEngine::HitFront;
                scene.p.hitSpread = 1.0f;
                scene.p.roomHeight = height;
                scene.p.roomCentre = centre;
                scene.start(width);
                Take take;
                scene.run(take, 500);
                const auto& b = take.blocks.back();
                struct R { double top, lfe, centre; };
                return R { spk.share(b, isTop), spk.share(b, [](const Info& s) { return s.role == room::Role::Lfe; }),
                           spk.share(b, [](const Info& s) { return s.x == 0.0f && s.role == room::Role::Main; }) };
            };
            const auto full = shares(1.0f, false), none = shares(0.0f, false), withCentre = shares(1.0f, true);
            std::cout << "         7.1.4 at 100%: overhead " << num(full.top, 3) << " of the power at Height 100%, " << num(none.top, 3) << " at 0%; LFE " << num(full.lfe, 6)
                      << "; centre speaker " << num(full.centre, 4) << " off, " << num(withCentre.centre, 3) << " on" << std::endl;
            check(std::abs(full.top - 4.0 / 10.0) < 0.01 && none.top < 1.0e-6, "Hit Spread reaches the speakers overhead as far as Height lets it");
            check(full.lfe < 1.0e-9, "and never the LFE");
            check(full.centre < 1.0e-9 && std::abs(withCentre.centre - 1.0 / 11.0) < 0.01, "and the centre speaker only with Centre Speaker on");
        }
    }

    //==================================================================================================================
    if (wanted("loudness"))
    {
        std::cout << "The same hit from a point to everywhere: each kind of file at Hit Spread 0 / 25 / 50 / 100%" << std::endl;
        const room::Width widths[] = { room::Width::Mono, room::Width::Stereo, room::Width::Surround51, room::Width::Surround71, room::Width::Surround714 };
        const float spreads[4] = { 0.0f, 0.25f, 0.5f, 1.0f };
        double worstPower = 0.0, worstLoudness = 0.0;
        for (const auto width : widths)
            for (const int position : { static_cast<int>(RigEngine::HitFront), static_cast<int>(RigEngine::HitRandom) })
            {
                if (position == RigEngine::HitRandom && (width == room::Width::Mono || width == room::Width::Surround71))
                    continue;
                for (int kind = 0; kind < c_kinds; ++kind)
                {
                    Level level[4];
                    for (int k = 0; k < 4; ++k)
                    {
                        Scene scene;
                        scene.load(2, src.burst[kind], CargoHold::c_hitsBus);
                        scene.p.hitAim = false;
                        scene.p.gateMode = 3;
                        scene.p.hitPosition = position;
                        scene.p.hitSpread = spreads[k];
                        scene.start(width);
                        Take take;
                        scene.run(take, static_cast<int>(9.0 * c_fs / c_block));
                        level[k] = levelOf(take, width, static_cast<size_t>(c_fs));
                    }
                    double powerRange = 0.0, loudRange = 0.0;
                    for (int a = 0; a < 4; ++a)
                        for (int b2 = 0; b2 < 4; ++b2)
                        {
                            powerRange = std::max(powerRange, std::abs(level[a].power - level[b2].power));
                            loudRange = std::max(loudRange, std::abs(level[a].loudness - level[b2].loudness));
                        }
                    worstPower = std::max(worstPower, powerRange);
                    worstLoudness = std::max(worstLoudness, loudRange);
                    std::cout << "         " << nameOf(width).paddedRight(' ', 7) << (position == RigEngine::HitFront ? "Front  " : "Random ") << juce::String(c_kindNames[kind]).paddedRight(' ', 20)
                              << "power " << num(level[0].power, 2) << " " << num(level[1].power, 2) << " " << num(level[2].power, 2) << " " << num(level[3].power, 2)
                              << "  loudness " << num(level[0].loudness, 2) << " " << num(level[1].loudness, 2) << " " << num(level[2].loudness, 2) << " " << num(level[3].loudness, 2) << std::endl;
                }
            }
        std::cout << "         the most a hit's level moves with Hit Spread: " << num(worstPower) << " dB of plain power, " << num(worstLoudness) << " dB of BS.1770 loudness" << std::endl;
        check(worstPower < 0.5, "every kind of file, every width: the power stays within 0.5 dB from a point to everywhere");
        check(worstLoudness < 1.0, "and the BS.1770 loudness within 1 dB (the speakers beside you count 1.5 dB up in that reading)");
    }

    //==================================================================================================================
    if (wanted("clicks"))
    {
        std::cout << "Nothing steps when Hit Position or Hit Spread change during a hit" << std::endl;
        for (const auto width : { room::Width::Stereo, room::Width::Surround51, room::Width::Surround714 })
        {
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.load(2, src.tone, CargoHold::c_hitsBus);
            scene.hold.setSkipSilence(2, false);
            scene.p.levels[0] = 0.002f;
            scene.p.chaos = 0.0f;
            scene.p.buses[0].path = static_cast<int>(room::Path::Through);
            scene.p.hitAim = false;
            scene.p.gateMode = 3;
            scene.p.hitPosition = RigEngine::HitFront;
            scene.p.hitSpread = 0.0f;
            scene.start(width);
            Take take;
            scene.run(take, 400);
            const float steady = secondStep(take, 200 * c_block, 400 * c_block);
            int n = 0;
            scene.run(take, 1200, [&](int b)
            {
                if (b % 60 != 30)
                    return;
                // Every pair of positions, and Hit Spread from end to end, in turn.
                const int positions[6] = { RigEngine::HitRandom, RigEngine::HitFront, RigEngine::HitWithPass, RigEngine::HitRandom, RigEngine::HitWithPass, RigEngine::HitFront };
                if (n % 2 == 0)
                    scene.p.hitPosition = positions[(n / 2) % 6];
                else
                    scene.p.hitSpread = scene.p.hitSpread > 0.5f ? 0.0f : 1.0f;
                ++n;
                scene.rig.setParams(scene.p);
            });
            const float moving = secondStep(take, 400 * c_block, take.samples());
            std::cout << "         " << nameOf(width) << ": the largest change of step, steady " << num(steady, 5) << ", with " << n << " changes " << num(moving, 5) << std::endl;
            check(steady > 0.0f && moving < 2.0f * steady + 2.0e-4f, nameOf(width) + ": no click when Hit Position or Hit Spread change mid-hit");
        }
    }

    //==================================================================================================================
    if (wanted("stereo"))
    {
        std::cout << "At Stereo and Mono" << std::endl;
        // With the Pass at Stereo: the hit's tail goes the way its pass goes.
        auto balance = [&](int position, int direction, float* spreadOut = nullptr)
        {
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.load(2, src.longHit, CargoHold::c_hitsBus);
            scene.p.levels[0] = 0.01f;
            scene.p.buses[0].direction = direction;
            scene.p.hitAim = position != RigEngine::HitRandom;
            scene.p.hitSync = position == RigEngine::HitRandom;
            scene.p.gateMode = 3;
            scene.p.hitSense = 1.0f;
            scene.p.hitPosition = position;
            scene.p.rate = 0.9f;
            scene.start(room::Width::Stereo);
            Take take;
            scene.run(take, static_cast<int>(30.0 * c_fs / c_block));
            // Power to the right less power to the left, over everything; and hit by hit (each second), how far
            // that differs.
            double l = 0.0, r = 0.0, least = 1.0, most = -1.0;
            for (size_t b = 200; b + 188 <= take.blocks.size(); b += 188)
            {
                double bl = 0.0, br = 0.0;
                for (size_t k = 0; k < 188; ++k)
                {
                    bl += take.blocks[b + k].power[0];
                    br += take.blocks[b + k].power[1];
                }
                l += bl;
                r += br;
                if (bl + br > 0.0)
                {
                    least = std::min(least, (br - bl) / (br + bl));
                    most = std::max(most, (br - bl) / (br + bl));
                }
            }
            if (spreadOut != nullptr)
                *spreadOut = static_cast<float>(most - least);
            return (r - l) / (r + l);
        };
        const double toRight = balance(RigEngine::HitWithPass, 1), toLeft = balance(RigEngine::HitWithPass, 2), front = balance(RigEngine::HitFront, 1);
        float randomRange = 0.0f;
        const double random = balance(RigEngine::HitRandom, 1, &randomRange);
        std::cout << "         Stereo, the hits' power right less left: with passes that fly left to right " << num(toRight) << ", right to left " << num(toLeft)
                  << ", Front " << num(front, 4) << "; Random " << num(random) << " over all, from second to second it differs by " << num(randomRange) << std::endl;
        check(toRight > 0.1 && toLeft < -0.1, "Stereo, With the Pass: a hit's tail goes the way its pass goes");
        check(std::abs(front) < 1.0e-4, "Stereo, Front: the middle");
        check(randomRange > 0.8f, "Stereo, Random: each hit somewhere between left and right");

        // Mono: neither control does anything. And the hits there are what they were before they had a place (their
        // mid, alike in left and right): Front at Stereo with a mono file is that too.
        auto render = [&](room::Width width, int position, float spread, const juce::File& file)
        {
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.load(2, file, CargoHold::c_hitsBus);
            scene.p.hitAim = true;
            scene.p.hitSense = 1.0f;
            scene.p.hitPosition = position;
            scene.p.hitSpread = spread;
            scene.start(width);
            Take take;
            scene.run(take, static_cast<int>(10.0 * c_fs / c_block));
            return take;
        };
        auto difference = [](const Take& a, const Take& b)
        {
            double diff = 0.0, all = 0.0;
            for (int c = 0; c < 2; ++c)
                for (size_t i = 0; i < a.ch[static_cast<size_t>(c)].size(); ++i)
                {
                    const double d = static_cast<double>(a.ch[static_cast<size_t>(c)][i]) - b.ch[static_cast<size_t>(c)][i];
                    diff += d * d;
                    all += static_cast<double>(a.ch[static_cast<size_t>(c)][i]) * a.ch[static_cast<size_t>(c)][i];
                }
            return db(diff / std::max(all, 1.0e-30));
        };
        const Take monoA = render(room::Width::Mono, RigEngine::HitWithPass, 0.0f, src.burst[2]);
        const Take monoB = render(room::Width::Mono, RigEngine::HitRandom, 1.0f, src.burst[2]);
        check(monoA.ch[0] == monoB.ch[0] && monoA.ch[1] == monoB.ch[1], "Mono: Hit Position and Hit Spread change nothing, sample for sample");
        const Take before = render(room::Width::Mono, RigEngine::HitFront, 0.0f, src.longHit);
        double worst = -400.0;
        for (const float spread : { 0.0f, RigEngine::c_hitSpreadDefault, 1.0f })
            worst = std::max(worst, difference(before, render(room::Width::Stereo, RigEngine::HitFront, spread, src.longHit)));
        std::cout << "         Stereo, Front, a mono hit, at any Hit Spread: " << num(worst, 1) << " dB from the hits in the middle as they were" << std::endl;
        check(worst < -120.0, "Stereo, Front, a mono hit: the same sound as before the hits had a place, wherever Hit Spread is");
        const double moved = difference(before, render(room::Width::Stereo, RigEngine::HitWithPass, RigEngine::c_hitSpreadDefault, src.longHit));
        std::cout << "         Stereo as it opens (With the Pass) against that: " << num(moved, 1) << " dB" << std::endl;
    }

    //==================================================================================================================
    if (wanted("beds"))
    {
        std::cout << "A stereo bed brought to a point keeps its level" << std::endl;
        // Bus B in Bed mode, each kind of file: wide (Width 100%, Doppler 0%) against narrow (Width 0%), and a
        // room's Spread from 0 to 100%.
        // (A pass's own Width changes its level a little whatever it carries, most of all folded to Mono: so each
        // kind of file is read against the mono file at the same settings.)
        double worst = 0.0, worstSpread = 0.0, worstDoppler = 0.0;
        for (const auto width : { room::Width::Mono, room::Width::Stereo, room::Width::Surround51, room::Width::Surround714 })
        {
            Level monoFile[5];
            for (int kind = 0; kind < c_kinds; ++kind)
            {
                auto level = [&](float busWidth, float doppler, float spread)
                {
                    Scene scene;
                    scene.load(1, src.steady[kind], 1);
                    scene.p.buses[1].passMode = 0;
                    scene.p.buses[1].width = busWidth;
                    scene.p.buses[1].doppler = doppler;
                    scene.p.buses[1].bend = 0.0f;
                    scene.p.roomSpread = spread;
                    scene.start(width);
                    Take take;
                    scene.run(take, static_cast<int>(16.0 * c_fs / c_block));
                    return levelOf(take, width, static_cast<size_t>(2.0 * c_fs));
                };
                const Level at[5] = { level(1.0f, 0.0f, 0.0f), level(0.5f, 0.0f, 0.0f), level(0.0f, 0.0f, 0.0f), level(1.0f, 0.5f, 0.0f), level(1.0f, 0.0f, 1.0f) };
                if (kind == 0)
                    for (int k = 0; k < 5; ++k)
                        monoFile[k] = at[k];
                double against[5];
                for (int k = 0; k < 5; ++k)
                    against[k] = at[k].power - monoFile[k].power;
                worst = std::max({ worst, std::abs(against[0] - against[2]), std::abs(against[1] - against[2]) });
                worstDoppler = std::max(worstDoppler, std::abs(against[3] - against[2]));
                worstSpread = std::max(worstSpread, std::abs(against[4] - against[0]));
                std::cout << "         " << nameOf(width).paddedRight(' ', 7) << juce::String(c_kindNames[kind]).paddedRight(' ', 20) << "power at Width 100 / 50 / 0%: " << num(at[0].power, 2) << " "
                          << num(at[1].power, 2) << " " << num(at[2].power, 2) << "; Doppler 50%: " << num(at[3].power, 2) << "; Spread 100%: " << num(at[4].power, 2)
                          << "   against the mono file: " << num(against[0], 2) << " " << num(against[1], 2) << " " << num(against[2], 2) << "; " << num(against[3], 2) << "; " << num(against[4], 2) << std::endl;
            }
        }
        std::cout << "         against a mono file at the same settings, the most a stereo bed's power moves with its Width: " << num(worst) << " dB; with Doppler: " << num(worstDoppler)
                  << " dB; with a room's Spread: " << num(worstSpread) << " dB" << std::endl;
        check(worst < 1.0, "a stereo bed is as loud narrow as wide, within 1 dB, for every kind of file at every width");
        check(worstDoppler < 1.0, "and as loud flown (Doppler 50%) as still");
        check(worstSpread < 1.0, "and as loud with a room's Spread at 100% as at 0%");

        // Stowaway in Dry keeps a stereo sound's sides too, by its Width.
        double worstDry = 0.0;
        for (const auto width : { room::Width::Mono, room::Width::Stereo, room::Width::Surround51 })
            for (int kind = 0; kind < c_kinds; ++kind)
            {
                auto level = [&](float stowWidth)
                {
                    Scene scene;
                    scene.load(3, src.steady[kind], CargoHold::c_stowBus);
                    scene.p.stow.on = true;
                    scene.p.stow.mode = RigEngine::StowDry;
                    scene.p.stow.follow = 0.0f;
                    scene.p.stow.width = stowWidth;
                    scene.start(width);
                    Take take;
                    scene.run(take, static_cast<int>(8.0 * c_fs / c_block));
                    return levelOf(take, width, static_cast<size_t>(2.0 * c_fs)).power;
                };
                const double wide = level(1.0f), half = level(0.5f), narrow = level(0.0f);
                worstDry = std::max({ worstDry, std::abs(wide - narrow), std::abs(half - narrow) });
                std::cout << "         Stowaway Dry, " << nameOf(width).paddedRight(' ', 7) << juce::String(c_kindNames[kind]).paddedRight(' ', 20) << "power at Width 100 / 50 / 0%: " << num(wide, 2) << " "
                          << num(half, 2) << " " << num(narrow, 2) << std::endl;
            }
        check(worstDry < 1.0, "Stowaway in Dry: a stereo sound is as loud narrow as wide, within 1 dB");
    }

    //==================================================================================================================
    if (wanted("plugin"))
    {
        std::cout << "Through the whole plug-in" << std::endl;
        double worstExtra = 0.0, worstPeak = -100.0;
        for (const auto width : { room::Width::Stereo, room::Width::Surround51, room::Width::Surround714 })
            for (int kind = 0; kind < c_kinds; ++kind)
            {
                auto play = [&](float spread, float boostDb, double& meanGr, double& peak, double& loud)
                {
                    std::unique_ptr<juce::AudioProcessor> base { createPluginFilter() };
                    auto& proc = *dynamic_cast<Proc*>(base.get());
                    juce::AudioProcessor::BusesLayout layout;
                    layout.outputBuses.add(room::channelSet(width));
                    if (! proc.setBusesLayout(layout))
                        check(false, "the bus takes " + nameOf(width));
                    juce::String error;
                    if (! proc.getCargo().loadFile(2, src.burst[kind], error))
                        check(false, "loads the hit: " + error);
                    proc.getCargo().setBusOverride(2, CargoHold::c_hitsBus);
                    auto set = [&](const char* id, float plain)
                    {
                        auto* param = proc.getState().getParameter(id);
                        param->setValueNotifyingHost(param->convertTo0to1(plain));
                    };
                    set(ParamIDs::hitAim, 0.0f);
                    set(ParamIDs::gateMode, 3.0f);
                    set(ParamIDs::hitPosition, static_cast<float>(ParamIDs::HitFront));
                    set(ParamIDs::hitSpread, spread);
                    set(ParamIDs::hitBoost, boostDb);
                    set(ParamIDs::space, 0.0f);
                    set(ParamIDs::inflate, 0.0f);
                    set(ParamIDs::push, 0.0f);
                    set(ParamIDs::rideOn, 0.0f);
                    proc.prepareToPlay(c_fs, 512);
                    proc.setPlaying(true);
                    const int channels = proc.getTotalNumOutputChannels();
                    juce::AudioBuffer<float> block(channels, 512);
                    juce::MidiBuffer midi;
                    meanGr = 0.0;
                    peak = 0.0;
                    Take take;
                    const int blocks = static_cast<int>(8.0 * c_fs / 512);
                    for (int b = 0; b < blocks; ++b)
                    {
                        block.clear();
                        proc.processBlock(block, midi);
                        meanGr += proc.getGainReductionDb() / blocks;
                        for (int c = 0; c < channels; ++c)
                        {
                            peak = std::max(peak, static_cast<double>(block.getMagnitude(c, 0, 512)));
                            take.ch[static_cast<size_t>(c)].insert(take.ch[static_cast<size_t>(c)].end(), block.getReadPointer(c), block.getReadPointer(c) + 512);
                        }
                    }
                    for (auto& c : take.ch)
                        c.resize(take.ch[0].size(), 0.0f);
                    loud = levelOf(take, width, static_cast<size_t>(c_fs)).loudness;
                    peak = 20.0 * std::log10(std::max(peak, 1.0e-9));
                };
                // Loud enough that the limiter is at work on a hit that is spread out, then the same hit as a point.
                double grWide = 0.0, grPoint = 0.0, peakWide = 0.0, peakPoint = 0.0, loudWide = 0.0, loudPoint = 0.0;
                play(1.0f, 9.0f, grWide, peakWide, loudWide);
                play(0.0f, 9.0f, grPoint, peakPoint, loudPoint);
                worstExtra = std::max(worstExtra, grWide - grPoint); // (gain reduction is negative: more of it is lower)
                worstPeak = std::max({ worstPeak, peakWide, peakPoint });
                std::cout << "         " << nameOf(width).paddedRight(' ', 7) << juce::String(c_kindNames[kind]).paddedRight(' ', 20) << "mean gain reduction, everywhere " << num(grWide, 2)
                          << " dB, a point " << num(grPoint, 2) << " dB; peak " << num(peakWide, 2) << " / " << num(peakPoint, 2) << " dBFS; loudness " << num(loudWide, 2) << " / " << num(loudPoint, 2) << std::endl;
            }
        std::cout << "         the most extra gain reduction a point costs: " << num(worstExtra) << " dB (mean); the highest sample " << num(worstPeak, 2) << " dBFS" << std::endl;
        check(worstExtra < 0.5, "bringing a hit to a point costs under 0.5 dB more limiting than spreading it out, for every kind of file");
        check(worstPeak <= -0.29, "and no sample passes the Ceiling (-0.3 dBFS)");
    }

    Proc::useSettings(nullptr);
    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

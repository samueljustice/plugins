// Offline test for the rig in a room: every pass, throw and hit as an object on the speakers (Quad to 7.1.4).
//
//   SwooshbucklerSurround [output folder]
//
// What is measured is the sound on each speaker: how its power is shared between them over time (the energy
// centroid: every speaker's place in the room weighted by its power), against where the engine says each object is
// (RigEngine::getObject) and against what each Flight Path is meant to do.
//
//   paths      at 5.1, 7.1 and 7.1.4, for Across, Through, Overhead and Around: a pass's energy goes through the
//              speakers in the path's order; the LFE is silent; the tops sound only on Overhead; channels a width
//              does not have are silent
//   power      one thrown object keeps exactly the power it has when it is in every speaker at once, block by
//              block, wherever it is; the same passes are as loud at every width
//   clicks     no step in any speaker when a pass starts again, or when the Flight Path, the room's width, Spread,
//              Height or Centre Speaker change mid-pass
//   the rig    LINK, Take Turns, the morph, AIM, the tremolo, throws and Swell in a room
//   Stowaway   Sub in L and R (and its stem for an LFE), Wake behind bus A's pass, Fly on its own path, Dry in L and R
//   beds       a stereo bed's sides stay a left / right pair, and Spread takes them round the room
//   stereo     at Stereo the twelve-channel call is the two-channel one
//   speed      the engine at 7.1.4 against Stereo at 48, 96 and 192 kHz

#include <JuceHeader.h>
#include "TestClock.h"
#include "Cargo.h"
#include "RigEngine.h"
#include "Room.h"
#include "DspUtils.h"

#include <functional>

namespace
{
    constexpr double c_fs = 48000.0;
    constexpr int c_block = 256;
    constexpr int c_wide = room::c_maxChannels;
    constexpr double c_pi = juce::MathConstants<double>::pi;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }

    float db(double power) { return static_cast<float>(10.0 * std::log10(std::max(power, 1.0e-30))); }
    juce::String num(double v, int places = 2) { return juce::String(v, places); }
    juce::String nameOf(room::Width w) { return room::layout(w).name; }

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

    // A steady hiss (no gaps, no swells of its own): what moves it is the rig.
    juce::AudioBuffer<float> hiss(int channels, int seed, bool sameBothSides = false)
    {
        juce::AudioBuffer<float> b(channels, static_cast<int>(c_fs * 8.0));
        for (int ch = 0; ch < channels; ++ch)
        {
            juce::Random r(seed + (sameBothSides ? 0 : 17 * ch));
            float y = 0.0f, y2 = 0.0f;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                y += 0.25f * ((r.nextFloat() * 2.0f - 1.0f) - y);
                y2 += 0.5f * (y - y2);
                b.setSample(ch, i, 0.9f * y2);
            }
        }
        return b;
    }

    // A steady low tone, whole cycles in the file (so its loop has no seam): any step in a speaker stands out.
    juce::AudioBuffer<float> tone(double hz)
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 8.0));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample(0, i, 0.5f * static_cast<float>(std::sin(2.0 * c_pi * hz * i / c_fs)));
        return b;
    }

    juce::AudioBuffer<float> hits()
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(c_fs * 2.0));
        b.clear();
        juce::Random r(9);
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < static_cast<int>(c_fs * 0.3); ++i)
            {
                const double t = i / c_fs;
                b.setSample(0, static_cast<int>(k * 0.5 * c_fs) + i,
                            static_cast<float>(0.8 * std::exp(-t / 0.04) * (r.nextFloat() * 2.0f - 1.0f) + 0.7 * std::exp(-t / 0.2) * std::sin(2.0 * c_pi * 55.0 * t)));
            }
        return b;
    }

    struct Sources
    {
        juce::File hissA, hissB, hissC, tone60, tone1k, hits, stereoBed, monoBed, drone;
    };

    // What one block sounded like and where the engine said everything was.
    struct Block
    {
        std::array<double, c_wide> power {};
        double total = 0.0;
        std::array<RigEngine::ObjectView, RigEngine::c_objects> object {};
        std::array<RigEngine::FlightView, 2> flightA {}, flightStow {};
        std::array<float, RigEngine::c_sections> section {};
    };

    struct Take
    {
        std::array<std::vector<float>, c_wide> ch;
        std::vector<float> stem;
        std::vector<Block> blocks;
        int samples() const { return static_cast<int>(ch[0].size()); }
    };

    // One rig with its own tracks.
    struct Scene
    {
        CargoHold hold;
        RigEngine rig;
        RigEngine::Params p;
        double rate = c_fs;

        Scene()
        {
            // The plainest rig: faders still, no colours, no Space, so what is heard is the passes themselves.
            p.rideOn = false;
            p.space = 0.0f;
            p.fill = 0.0f;
            p.hitAim = false;
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
        void start(room::Width width, double sampleRate = c_fs)
        {
            rate = sampleRate;
            rig.setCargoHold(&hold);
            rig.setWidth(width);
            rig.setParams(p);
            rig.prepare(rate, c_block);
            rig.setParams(p);
        }
        /** Plays `blocks` more blocks onto the end of `take` (twelve channels, whatever the width). before: called
            ahead of each block with its number. */
        void run(Take& take, int blocks, const std::function<void(int)>& before = {})
        {
            const int from = take.samples();
            for (auto& c : take.ch)
                c.resize(static_cast<size_t>(from + blocks * c_block), 0.0f);
            take.stem.resize(static_cast<size_t>(from + blocks * c_block), 0.0f);
            for (int b = 0; b < blocks; ++b)
            {
                if (before)
                    before(b);
                float* out[c_wide];
                for (int c = 0; c < c_wide; ++c)
                    out[c] = take.ch[static_cast<size_t>(c)].data() + from + b * c_block;
                rig.process(nullptr, nullptr, out, c_wide, c_block);
                std::copy(rig.getSubStem(), rig.getSubStem() + c_block, take.stem.begin() + from + b * c_block);
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
                for (int h = 0; h < 2; ++h)
                {
                    info.flightA[static_cast<size_t>(h)] = rig.getFlightView(h);
                    info.flightStow[static_cast<size_t>(h)] = rig.getStowFlight(h);
                }
                info.section = rig.takeSectionLevels();
                take.blocks.push_back(info);
            }
        }
    };

    // The speakers of a width as the test reads them.
    struct Speakers
    {
        const room::Layout& layout;
        explicit Speakers(room::Width w) : layout(room::layout(w)) {}
        const room::SpeakerInfo& info(int ch) const { return room::info(layout.speakers[static_cast<size_t>(ch)]); }
        bool isMain(int ch) const { return info(ch).role != room::Role::Lfe; }

        /** The share of a block's power on the speakers `pick` chooses. */
        double share(const Block& b, const std::function<bool(const room::SpeakerInfo&)>& pick) const
        {
            double sum = 0.0;
            for (int ch = 0; ch < layout.numChannels; ++ch)
                if (isMain(ch) && pick(info(ch)))
                    sum += b.power[static_cast<size_t>(ch)];
            return b.total > 0.0 ? sum / b.total : 0.0;
        }
        /** Where the block's energy is: every speaker's place weighted by its power. */
        room::Position centroid(const Block& b) const
        {
            room::Position c { 0.0f, 0.0f, 0.0f };
            if (b.total <= 0.0)
                return c;
            for (int ch = 0; ch < layout.numChannels; ++ch)
            {
                const double w = b.power[static_cast<size_t>(ch)] / b.total;
                c.x += static_cast<float>(w * info(ch).x);
                c.y += static_cast<float>(w * info(ch).y);
                c.z += static_cast<float>(w * info(ch).z);
            }
            return c;
        }
        int loudest(const Block& b) const
        {
            int best = 0;
            for (int ch = 1; ch < layout.numChannels; ++ch)
                if (b.power[static_cast<size_t>(ch)] > b.power[static_cast<size_t>(best)])
                    best = ch;
            return best;
        }
    };

    double correlation(const std::vector<double>& a, const std::vector<double>& b)
    {
        const size_t n = std::min(a.size(), b.size());
        if (n < 3)
            return 0.0;
        double ma = 0.0, mb = 0.0;
        for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
        ma /= static_cast<double>(n);
        mb /= static_cast<double>(n);
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            ab += (a[i] - ma) * (b[i] - mb);
            aa += (a[i] - ma) * (a[i] - ma);
            bb += (b[i] - mb) * (b[i] - mb);
        }
        return aa > 0.0 && bb > 0.0 ? ab / std::sqrt(aa * bb) : 0.0;
    }

    // One pass of one mover, as the blocks in which it is what is heard.
    using Pass = std::vector<int>;

    /** The passes of a mover (age: how far through its pass, 0..1; level: how loud it is; other: the other mover of
        the pair): the blocks where it is at least 12 dB over the other and within 30 dB of its pass's loudest. Only
        whole passes count: heard from their first third to past their closest point. (A Fly-By is a long approach
        and a short tail, and the next pass takes over soon after the closest point: that is as far as a pass is
        heard by itself.) */
    std::vector<Pass> passesOf(const Take& take, const std::function<float(const Block&)>& age, const std::function<float(const Block&)>& level,
                               const std::function<float(const Block&)>& other)
    {
        std::vector<Pass> passes;
        Pass now;
        float last = -1.0f;
        auto close = [&]
        {
            if (now.size() >= 12)
            {
                double top = 0.0;
                for (int b : now)
                    top = std::max(top, take.blocks[static_cast<size_t>(b)].total);
                Pass heard;
                for (int b : now)
                    if (take.blocks[static_cast<size_t>(b)].total > top * 1.0e-3)
                        heard.push_back(b);
                if (heard.size() >= 12 && age(take.blocks[static_cast<size_t>(heard.front())]) < 0.33f && age(take.blocks[static_cast<size_t>(heard.back())]) > 0.55f)
                    passes.push_back(heard);
            }
            now.clear();
        };
        for (int b = 0; b < static_cast<int>(take.blocks.size()); ++b)
        {
            const auto& block = take.blocks[static_cast<size_t>(b)];
            const float a = age(block);
            if (a < last - 0.2f)
                close();
            last = a;
            if (level(block) > 0.02f && level(block) > 4.0f * other(block))
                now.push_back(b);
        }
        close();
        return passes;
    }

    /** A mean over the first, middle or last third of a pass. */
    double third(const Take& take, const Pass& pass, int which, const std::function<double(const Block&)>& value)
    {
        const size_t n = pass.size() / 3;
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i)
            sum += value(take.blocks[static_cast<size_t>(pass[static_cast<size_t>(which) * n + i])]);
        return sum / static_cast<double>(std::max<size_t>(1, n));
    }

    struct Steps
    {
        float peak = 0.0f, first = 0.0f, second = 0.0f; // the loudest sample, and the largest first and second difference
    };
    /** The largest sample-to-sample step on any channel (first difference), and the largest change of that step
        (second difference: a click is a step that the samples either side of it do not share). */
    Steps stepsOf(const Take& take, int from = 0)
    {
        Steps s;
        for (const auto& c : take.ch)
            for (size_t i = static_cast<size_t>(from) + 2; i < c.size(); ++i)
            {
                s.peak = std::max(s.peak, std::abs(c[i]));
                s.first = std::max(s.first, std::abs(c[i] - c[i - 1]));
                s.second = std::max(s.second, std::abs(c[i] - 2.0f * c[i - 1] + c[i - 2]));
            }
        return s;
    }

    bool allFinite(const Take& take)
    {
        for (const auto& c : take.ch)
            for (float x : c)
                if (! std::isfinite(x))
                    return false;
        return true;
    }
    float peakOf(const std::vector<float>& x)
    {
        float m = 0.0f;
        for (float v : x)
            m = std::max(m, std::abs(v));
        return m;
    }
    double meanPower(const Take& take, int fromBlock = 0)
    {
        double sum = 0.0;
        int n = 0;
        for (size_t b = static_cast<size_t>(fromBlock); b < take.blocks.size(); ++b, ++n)
            sum += take.blocks[b].total;
        return n > 0 ? sum / n : 0.0;
    }

    constexpr room::Width c_rooms[] = { room::Width::Surround51, room::Width::Surround71, room::Width::Surround714 };
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getCurrentWorkingDirectory();
    folder.createDirectory();

    Sources src;
    src.hissA = writeWav(folder, "surround_src_hiss_a", hiss(1, 11));
    src.hissB = writeWav(folder, "surround_src_hiss_b", hiss(1, 23));
    src.hissC = writeWav(folder, "surround_src_hiss_c", hiss(1, 37));
    src.tone60 = writeWav(folder, "surround_src_tone60", tone(60.0));
    src.tone1k = writeWav(folder, "surround_src_tone1k", tone(1000.0));
    src.hits = writeWav(folder, "surround_src_hits", hits());
    src.stereoBed = writeWav(folder, "surround_src_bed_stereo", hiss(2, 51));
    src.monoBed = writeWav(folder, "surround_src_bed_mono", hiss(2, 51, true));
    src.drone = writeWav(folder, "surround_src_drone", tone(50.0));

    using Info = room::SpeakerInfo;
    const auto isLeft = [](const Info& s) { return s.x < 0.0f; };
    const auto isAhead = [](const Info& s) { return s.y > 0.0f; };
    const auto isBehind = [](const Info& s) { return s.y < 0.0f; };
    const auto isSideRow = [](const Info& s) { return s.role == room::Role::Main && s.y == 0.0f; };
    const auto isTop = [](const Info& s) { return s.role == room::Role::Height; };
    const auto ageA = [](int h) { return [h](const Block& b) { return b.flightA[static_cast<size_t>(h)].age; }; };
    const auto levelA = [](int h) { return [h](const Block& b) { return b.flightA[static_cast<size_t>(h)].env; }; };

    //==================================================================================================================
    std::cout << "Flight Paths: a pass's energy goes through the speakers in the path's order" << std::endl;
    for (auto width : c_rooms)
    {
        const Speakers spk(width);
        const int lfe = spk.layout.lfe;
        for (int path = 0; path < room::c_numPaths; ++path)
        {
            const juce::String name = nameOf(width) + ", " + room::pathName(static_cast<room::Path>(path));
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.p.buses[0].path = path;
            scene.start(width);
            Take take;
            scene.run(take, static_cast<int>(60.0 * c_fs / c_block));

            // The passes of bus A's two movers, each while it is the one heard.
            std::vector<Pass> passes;
            std::vector<int> mover;
            for (int h = 0; h < 2; ++h)
                for (auto& pass : passesOf(take, ageA(h), levelA(h), levelA(1 - h)))
                {
                    passes.push_back(pass);
                    mover.push_back(h);
                }

            // The energy against where the engine says the pass is, and the order the speakers are reached in.
            std::vector<double> heardX, heardY, heardZ, saidX, saidY, saidZ;
            int inOrder = 0, viaMiddle = 0, bothWays[2] = { 0, 0 };
            double worstStart = 1.0, worstEnd = 0.0, leastTop = 1.0, mostTop = 0.0;
            std::array<bool, c_wide> wasLoudest {};
            for (size_t n = 0; n < passes.size(); ++n)
            {
                const auto& pass = passes[n];
                const int obj = RigEngine::ObjPassA1 + mover[n];
                for (int b : pass)
                {
                    const auto& block = take.blocks[static_cast<size_t>(b)];
                    const auto c = spk.centroid(block);
                    heardX.push_back(c.x); heardY.push_back(c.y); heardZ.push_back(c.z);
                    saidX.push_back(block.object[static_cast<size_t>(obj)].x);
                    saidY.push_back(block.object[static_cast<size_t>(obj)].y);
                    saidZ.push_back(block.object[static_cast<size_t>(obj)].z);
                    wasLoudest[static_cast<size_t>(spk.loudest(block))] = true;
                }
                auto said = [&](int which, float RigEngine::ObjectView::* axis)
                {
                    return third(take, pass, which, [&](const Block& b) { return static_cast<double>(b.object[static_cast<size_t>(obj)].*axis); });
                };
                if (path == static_cast<int>(room::Path::Across))
                {
                    // Left to right (or right to left): the left speakers first, the right ones last.
                    const bool toRight = said(2, &RigEngine::ObjectView::x) > said(0, &RigEngine::ObjectView::x);
                    ++bothWays[toRight ? 1 : 0];
                    const double start = third(take, pass, 0, [&](const Block& b) { return spk.share(b, isLeft); });
                    const double end = third(take, pass, 2, [&](const Block& b) { return spk.share(b, isLeft); });
                    const double first = toRight ? start : 1.0 - start, last = toRight ? end : 1.0 - end;
                    worstStart = std::min(worstStart, first);
                    worstEnd = std::max(worstEnd, last);
                    if (first > 0.7 && last < 0.45)
                        ++inOrder;
                }
                else if (path == static_cast<int>(room::Path::Through) || path == static_cast<int>(room::Path::Overhead))
                {
                    // Front to back (or back to front): the front speakers first, the rear ones last, and on the
                    // way the side speakers (7.1) or the tops (Overhead at 7.1.4).
                    const bool toBack = said(2, &RigEngine::ObjectView::y) < said(0, &RigEngine::ObjectView::y);
                    ++bothWays[toBack ? 1 : 0];
                    // How much more of the power is in the speakers ahead of the listener than in those behind
                    // (floor and tops alike): well to the side it starts on, and across to the other by the end.
                    // (A Fly-By is heard alone until soon after its closest point: that far, not to the far wall.)
                    const auto ahead = [&](const Block& b) { return spk.share(b, isAhead) - spk.share(b, isBehind); };
                    const double start = third(take, pass, 0, ahead), end = third(take, pass, 2, ahead);
                    const double first = toBack ? start : -start, last = toBack ? end : -end;
                    worstStart = std::min(worstStart, first);
                    worstEnd = std::max(worstEnd, last);
                    if (first > 0.3 && last < first - 0.5)
                        ++inOrder;
                    // On the way: the side speakers (or the tops) hold their largest share while the object is
                    // beside (or over) the listener.
                    const auto middle = path == static_cast<int>(room::Path::Overhead) ? std::function<bool(const Info&)>(isTop)
                                                                                         : std::function<bool(const Info&)>(isSideRow);
                    double most = 0.0;
                    float where = 1.0f;
                    for (int b : pass)
                        if (const double m = spk.share(take.blocks[static_cast<size_t>(b)], middle); m > most)
                        {
                            most = m;
                            where = std::abs(take.blocks[static_cast<size_t>(b)].object[static_cast<size_t>(obj)].y);
                        }
                    if (most > 0.5 && where < 0.35f)
                        ++viaMiddle;
                    leastTop = std::min(leastTop, most);
                }
                for (int b : pass)
                    mostTop = std::max(mostTop, spk.share(take.blocks[static_cast<size_t>(b)], isTop));
            }

            // Silent where it has to be: the LFE, and every channel the width does not have.
            float lfePeak = 0.0f, beyond = 0.0f, topPeak = 0.0f;
            for (int ch = 0; ch < c_wide; ++ch)
            {
                const float peak = peakOf(take.ch[static_cast<size_t>(ch)]);
                if (ch == lfe)
                    lfePeak = peak;
                else if (ch >= spk.layout.numChannels)
                    beyond = std::max(beyond, peak);
                else if (isTop(spk.info(ch)))
                    topPeak = std::max(topPeak, peak);
            }
            const double cx = correlation(heardX, saidX), cy = correlation(heardY, saidY), cz = correlation(heardZ, saidZ);
            std::cout << "         " << name << ": " << passes.size() << " passes; energy against the object: x " << num(cx) << " y " << num(cy)
                      << " z " << num(cz) << "; in order " << inOrder << ", through the middle " << viaMiddle
                      << "; share at the start " << num(worstStart) << " and at the end " << num(worstEnd)
                      << " (worst); tops at most " << num(mostTop) << std::endl;
            check(allFinite(take) && passes.size() >= 8 && meanPower(take) > 1.0e-5, name + ": plays, pass after pass");
            check(lfePeak == 0.0f && beyond == 0.0f, name + ": the LFE is silent, and so is every channel the width does not have");
            const bool hasTops = width == room::Width::Surround714;
            switch (static_cast<room::Path>(path))
            {
                case room::Path::Across:
                    check(inOrder == static_cast<int>(passes.size()) && cx > 0.9, name + ": every pass starts in the speakers of one side and ends in the other's, as the object does");
                    check(bothWays[0] > 0 && bothWays[1] > 0, name + ": some passes go left to right and some right to left");
                    check(topPeak == 0.0f, name + ": nothing in the tops");
                    break;
                case room::Path::Through:
                    check(inOrder == static_cast<int>(passes.size()) && cy > 0.9, name + ": every pass starts in the front speakers and ends in the rear ones (or the other way), as the object does");
                    check(width == room::Width::Surround51 || viaMiddle == static_cast<int>(passes.size()), name + ": by way of the side speakers");
                    check(bothWays[0] > 0 && bothWays[1] > 0, name + ": some passes go front to back and some back to front");
                    check(topPeak == 0.0f, name + ": nothing in the tops");
                    break;
                case room::Path::Overhead:
                    check(inOrder == static_cast<int>(passes.size()) && cy > 0.9, name + ": every pass starts in the front speakers and ends in the rear ones (or the other way), as the object does");
                    if (hasTops)
                        check(viaMiddle == static_cast<int>(passes.size()) && leastTop > 0.5 && cz > 0.9,
                              name + ": by way of the tops, which hold most of it mid-pass (at least " + num(leastTop) + " of the power)");
                    else
                        check(topPeak == 0.0f && beyond == 0.0f, name + ": with no tops it stays on the floor");
                    break;
                case room::Path::Around:
                {
                    bool everySpeaker = true;
                    for (int ch = 0; ch < spk.layout.numChannels; ++ch)
                        if (spk.isMain(ch) && ! isTop(spk.info(ch)) && spk.info(ch).x != 0.0f)
                            everySpeaker = everySpeaker && wasLoudest[static_cast<size_t>(ch)];
                    check(everySpeaker && cx > 0.9 && cy > 0.9, name + ": the fronts, the sides and the rears each take their turn as the loudest speaker, as the object goes round");
                    check(topPeak == 0.0f, name + ": nothing in the tops");
                    break;
                }
            }
        }
    }

    //==================================================================================================================
    std::cout << "Power: an object is as loud wherever it is" << std::endl;
    {
        // One throw from rest, a steady tone: the only thing that changes the power on the speakers is the panner.
        // With Spread at 100% it is in every speaker all the time (nothing moves), so the two must be as loud as
        // each other block by block.
        auto thrown = [&](room::Width width, int path, float spread)
        {
            Scene scene;
            scene.load(0, src.tone1k, 0);
            scene.p.buses[0].path = path;
            scene.p.roomSpread = spread;
            scene.start(width);
            scene.rig.setReleased(true);
            Take take;
            // (Stopped from the start: the tracks let go first, and the throw comes a second later.)
            scene.run(take, static_cast<int>(5.0 * c_fs / c_block), [&](int b)
            {
                scene.rig.setReleased(true);
                if (b == 190)
                    scene.rig.throwWhoosh(0, 1.0f);
            });
            take.blocks.erase(take.blocks.begin(), take.blocks.begin() + 190);
            return take;
        };
        for (auto width : { room::Width::Surround51, room::Width::Surround714 })
            for (int path = 0; path < room::c_numPaths; ++path)
            {
                const auto moving = thrown(width, path, 0.0f), everywhere = thrown(width, path, 1.0f);
                double top = 0.0;
                for (const auto& b : everywhere.blocks)
                    top = std::max(top, b.total);
                float worst = 0.0f, worstStep = 0.0f;
                int counted = 0;
                for (size_t b = 1; b < moving.blocks.size(); ++b)
                {
                    if (everywhere.blocks[b].total < top * 1.0e-3 || everywhere.blocks[b - 1].total < top * 1.0e-3)
                        continue;
                    ++counted;
                    worst = std::max(worst, std::abs(db(moving.blocks[b].total) - db(everywhere.blocks[b].total)));
                    // (From 20 dB under its loudest: lower down is where the thrown sound first arrives.)
                    if (everywhere.blocks[b].total > top * 1.0e-2 && everywhere.blocks[b - 1].total > top * 1.0e-2)
                        worstStep = std::max(worstStep, std::abs(db(moving.blocks[b].total) - db(moving.blocks[b - 1].total)));
                }
                const juce::String name = nameOf(width) + ", " + room::pathName(static_cast<room::Path>(path));
                std::cout << "         " << name << ": a thrown tone against the same in every speaker, at most " << num(worst, 3)
                          << " dB apart over " << counted << " blocks; block to block at most " << num(worstStep, 2) << " dB" << std::endl;
                check(counted > 100 && worst < 0.05f, name + ": the total power on the speakers is the object's, wherever it is (within 0.05 dB)");
                check(worstStep < 1.0f, name + ": and it rises and falls smoothly (under 1 dB from one block to the next)");
            }

        // The same passes at every width (the same seed): as loud, block by block.
        std::vector<Take> takes;
        for (auto width : { room::Width::Quad, room::Width::Surround51, room::Width::Surround71, room::Width::Surround712, room::Width::Surround714 })
        {
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.load(1, src.hissB, 1);
            scene.p.buses[0].path = static_cast<int>(room::Path::Around);
            scene.p.buses[1].path = static_cast<int>(room::Path::Overhead);
            scene.start(width);
            takes.emplace_back();
            scene.run(takes.back(), static_cast<int>(20.0 * c_fs / c_block));
        }
        float worst = 0.0f, mean = 0.0f;
        for (size_t t = 1; t < takes.size(); ++t)
        {
            worst = std::max(worst, std::abs(db(meanPower(takes[t])) - db(meanPower(takes[0]))));
            // Over a tenth of a second at a time (two passes that share speakers add by chance, block by block).
            for (size_t b = 0; b + 20 <= takes[t].blocks.size(); b += 20)
            {
                double p0 = 0.0, p1 = 0.0;
                for (size_t k = 0; k < 20; ++k)
                {
                    p0 += takes[0].blocks[b + k].total;
                    p1 += takes[t].blocks[b + k].total;
                }
                mean = std::max(mean, std::abs(db(p1) - db(p0)));
            }
        }
        std::cout << "         Quad, 5.1, 7.1, 7.1.2, 7.1.4 against each other: whole take within " << num(worst, 2) << " dB, any tenth of a second within "
                  << num(mean, 2) << " dB" << std::endl;
        check(worst < 0.1f && mean < 0.6f, "the same passes are as loud at every width (the take within 0.1 dB, any tenth of a second within 0.6 dB)");

        // And against the stereo engine's render of the same rig: not the same sound (two ears there, speakers
        // here), but about as loud over the take, all channels together.
        {
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.load(1, src.hissB, 1);
            scene.start(room::Width::Stereo);
            Take stereo;
            scene.run(stereo, static_cast<int>(20.0 * c_fs / c_block));
            const float apart = db(meanPower(takes[1])) - db(meanPower(stereo));
            std::cout << "         5.1 against the Stereo render of the same rig: " << num(apart, 2) << " dB over the take" << std::endl;
            check(std::abs(apart) < 1.5f, "a room is about as loud as Stereo (all its speakers together within 1.5 dB over the take)");
        }
    }

    //==================================================================================================================
    std::cout << "Clicks: nothing steps when a pass starts again or a setting changes mid-pass" << std::endl;
    {
        // A 60 Hz tone on bus A, no colours: from one sample to the next it moves by under 1% of its peak, so a
        // step in any speaker's gain shows. Measured: the largest step, and the largest change of step.
        enum Change { Nothing, Paths, RoomKnobs, Widths, StereoAndBack, Everything };
        auto played = [&](int passMode, Change change, float space = 0.0f)
        {
            Scene scene;
            scene.load(0, src.tone60, 0);
            scene.p.space = space;
            scene.p.buses[0].passMode = passMode;
            scene.p.buses[0].path = static_cast<int>(room::Path::Through);
            scene.start(room::Width::Surround714);
            const room::Width widths[] = { room::Width::Surround51, room::Width::Surround71, room::Width::Quad, room::Width::Surround712, room::Width::Surround714 };
            Take take;
            scene.run(take, static_cast<int>(40.0 * c_fs / c_block), [&](int b)
            {
                auto& p = scene.p;
                if ((change == Paths || change == Everything) && b % 97 == 0)
                    p.buses[0].path = (b / 97) % room::c_numPaths;
                if (change == RoomKnobs || change == Everything)
                {
                    if (b % 61 == 0) p.roomSpread = (b / 61) % 2 == 0 ? 0.0f : 0.9f;
                    if (b % 73 == 0) p.roomCentre = (b / 73) % 2 == 1;
                    if (b % 89 == 0) p.roomHeight = (b / 89) % 2 == 0 ? 1.0f : 0.0f;
                }
                if ((change == Widths || change == Everything) && b % 131 == 0)
                    scene.rig.setWidth(widths[(b / 131) % 5]);
                if (change == StereoAndBack && b % 211 == 0)
                    scene.rig.setWidth((b / 211) % 2 == 1 ? room::Width::Stereo : room::Width::Surround714);
                scene.rig.setParams(p);
            });
            return take;
        };
        const char* modes[] = { "Bed", "Fly-By" };
        for (int passMode = 0; passMode < 2; ++passMode)
        {
            const auto steady = stepsOf(played(passMode, Nothing), 4800);
            std::cout << "         " << modes[passMode] << ", nothing changed (passes start again every few seconds): peak " << num(steady.peak, 3)
                      << ", largest step " << num(100.0f * steady.first / steady.peak, 2) << "% of it, largest change of step "
                      << num(100.0f * steady.second / steady.peak, 3) << "%" << std::endl;
            check(steady.peak > 0.2f && steady.first < 0.012f * steady.peak && steady.second < 0.001f * steady.peak,
                  juce::String(modes[passMode]) + ": no click where a pass ends and the next begins at the far wall");
            const std::pair<Change, const char*> changes[] = { { Paths, "the Flight Path changes" }, { RoomKnobs, "Spread, Height and Centre Speaker jump" },
                                                               { Widths, "the room changes width" }, { StereoAndBack, "the width goes to Stereo and back" },
                                                               { Everything, "all of them at once" } };
            for (const auto& [change, what] : changes)
            {
                const auto take = played(passMode, change);
                const auto s = stepsOf(take, 4800);
                std::cout << "         " << modes[passMode] << ", " << what << ": largest step " << num(100.0f * s.first / s.peak, 2)
                          << "% of the peak, largest change of step " << num(100.0f * s.second / s.peak, 3) << "%" << std::endl;
                check(allFinite(take) && s.peak > 0.2f && s.first < 0.015f * s.peak && s.second < 0.005f * s.peak,
                      juce::String(modes[passMode]) + ": no click when " + what + " mid-pass");
            }
            // With Space up (the echoes and the reverb are sent a fold of the room, and a step in what they are
            // sent comes back as a click an echo later).
            for (const auto& [change, what] : { std::pair<Change, const char*> { Widths, "the room changes width" }, { StereoAndBack, "the width goes to Stereo and back" } })
            {
                const auto take = played(passMode, change, 0.5f);
                const auto s = stepsOf(take, 4800);
                std::cout << "         " << modes[passMode] << ", Space at 50%, " << what << ": largest step " << num(100.0f * s.first / s.peak, 2)
                          << "% of the peak, largest change of step " << num(100.0f * s.second / s.peak, 3) << "%" << std::endl;
                check(allFinite(take) && s.peak > 0.2f && s.first < 0.015f * s.peak && s.second < 0.005f * s.peak,
                      juce::String(modes[passMode]) + ": no click when " + what + " mid-pass with Space up");
            }
        }
    }

    //==================================================================================================================
    std::cout << "The rig in a room" << std::endl;
    {
        const auto width = room::Width::Surround51;
        const Speakers spk(width);
        const int blocks = static_cast<int>(20.0 * c_fs / c_block);

        // LINK: bus B flies bus A's passes, on bus A's Flight Path.
        {
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.load(1, src.hissB, 1);
            scene.p.buses[0].path = static_cast<int>(room::Path::Through);
            scene.p.buses[1].path = static_cast<int>(room::Path::Across);
            scene.p.linkBuses = true;
            scene.start(width);
            Take linked;
            scene.run(linked, blocks);
            float apart = 0.0f;
            bool moved = false;
            for (const auto& b : linked.blocks)
                for (int h = 0; h < 2; ++h)
                {
                    const auto& a = b.object[static_cast<size_t>(RigEngine::ObjPassA1 + h)];
                    const auto& o = b.object[static_cast<size_t>(RigEngine::ObjPassB1 + h)];
                    apart = std::max({ apart, std::abs(a.x - o.x), std::abs(a.y - o.y), std::abs(a.z - o.z) });
                    moved = moved || a.y < -0.5f;
                }
            check(apart < 1.0e-6f && moved && allFinite(linked), "LINK: bus B's passes are where bus A's are, on bus A's Flight Path");
        }
        // Take Turns: the quieter bus ducks under the louder one.
        {
            auto ratio = [&](bool takeTurns)
            {
                Scene scene;
                scene.load(0, src.hissA, 0);
                scene.load(1, src.hissB, 1);
                scene.p.takeTurns = takeTurns;
                scene.p.buses[1].duck = 1.0f;
                scene.start(width);
                Take take;
                scene.run(take, blocks);
                double sum = 0.0;
                int n = 0;
                for (size_t b = 200; b < take.blocks.size(); ++b, ++n)
                {
                    const float a = take.blocks[b].section[RigEngine::SectionBusA], o = take.blocks[b].section[RigEngine::SectionBusB];
                    sum += std::min(a, o) / std::max({ a, o, 1.0e-9f });
                }
                return sum / n;
            };
            const double off = ratio(false), on = ratio(true);
            std::cout << "         Take Turns: the quieter bus against the louder, off " << num(off) << ", on " << num(on) << std::endl;
            check(on < 0.75 * off, "Take Turns: the quieter bus goes further under the louder one");
        }
        // The morph: buses A and B are fed the morph of the two, and still fly.
        {
            auto power = [&](bool morph)
            {
                Scene scene;
                scene.load(0, src.hissA, 0);
                scene.load(1, src.tone1k, 1);
                scene.p.morphOn = morph;
                scene.start(width);
                Take take;
                scene.run(take, blocks);
                return std::make_pair(meanPower(take, 400), allFinite(take));
            };
            const auto off = power(false), on = power(true);
            std::cout << "         morph: " << num(db(on.first) - db(off.first), 1) << " dB against off" << std::endl;
            check(on.second && on.first > 0.0 && std::abs(db(on.first) - db(off.first)) < 6.0f, "morph: the room plays with the morph in, about as loud");
        }
        // AIM: the hits land where the pass they were aimed at is at its closest.
        {
            auto heard = [&](bool aim)
            {
                Scene scene;
                scene.load(0, src.hissA, 0);
                scene.load(2, src.hits, CargoHold::c_hitsBus);
                scene.p.levels[0] = 0.01f; // bus A all but silent: what is heard is the hits
                scene.p.buses[0].path = static_cast<int>(room::Path::Through);
                scene.p.hitAim = aim;
                scene.p.hitSense = 1.0f;
                scene.p.hitDuck = 0.0f;
                scene.start(width);
                Take take;
                scene.run(take, blocks);
                double rear = 0.0, total = 0.0, depth = 0.0;
                float nearestFront = 0.0f, furthestFront = 1.0f;
                for (size_t b = 200; b < take.blocks.size(); ++b)
                {
                    const auto& block = take.blocks[b];
                    if (! block.object[RigEngine::ObjHits].active || block.object[RigEngine::ObjHits].level < 0.2f)
                        continue;
                    rear += block.total * (1.0 - spk.share(block, [](const Info& s) { return s.y > 0.0f; }));
                    total += block.total;
                    depth += block.total * std::abs(block.object[RigEngine::ObjHits].y);
                    nearestFront = std::max(nearestFront, block.object[RigEngine::ObjHits].y);
                    furthestFront = std::min(furthestFront, block.object[RigEngine::ObjHits].y);
                }
                struct Result { double rear; float yMost, yLeast; double total, depth; };
                return Result { total > 0.0 ? rear / total : 0.0, nearestFront, furthestFront, total, total > 0.0 ? depth / total : 1.0 };
            };
            const auto aimed = heard(true), plain = heard(false);
            std::cout << "         hits: not aimed, " << num(plain.rear) << " of their power behind (object y " << num(plain.yLeast) << " .. " << num(plain.yMost)
                      << "); aimed at passes that go Through, " << num(aimed.rear) << " (y " << num(aimed.yLeast) << " .. " << num(aimed.yMost) << ")" << std::endl;
            check(plain.total > 0.0 && plain.rear < 0.02 && plain.yLeast > 0.999f, "hits that are not aimed play at the front");
            check(aimed.total > 0.0 && aimed.rear > 0.3 && aimed.rear < 0.7 && aimed.depth < 0.05,
                  "AIM: the hits play where their pass is at its closest (beside the listener: half in front, half behind)");
        }
        // The tremolo on the buses and on everything: the room shakes.
        for (int target : { 0, 1 })
        {
            auto ripple = [&](bool on)
            {
                Scene scene;
                scene.load(0, src.hissA, 0);
                scene.p.buses[0].passMode = 0;
                scene.p.tremOn = on;
                scene.p.tremDepth = 1.0f;
                scene.p.tremThreshold = 0.0f;
                scene.p.tremRate = 0.4f;
                scene.p.tremTarget = target;
                scene.start(width);
                Take take;
                scene.run(take, blocks);
                // How far the power falls within a quarter of a second, on average.
                double sum = 0.0;
                int n = 0;
                for (size_t b = 400; b + 47 <= take.blocks.size(); b += 47, ++n)
                {
                    double lo = 1.0e30, hi = 0.0;
                    for (size_t k = 0; k < 47; ++k)
                    {
                        lo = std::min(lo, take.blocks[b + k].total);
                        hi = std::max(hi, take.blocks[b + k].total);
                    }
                    sum += db(hi) - db(lo);
                }
                return sum / n;
            };
            const double off = ripple(false), on = ripple(true);
            std::cout << "         tremolo on " << (target == 0 ? "everything" : "the buses") << ": the power moves " << num(off, 1) << " dB in a quarter second off, "
                      << num(on, 1) << " dB on" << std::endl;
            check(on > off + 6.0, juce::String("tremolo on ") + (target == 0 ? "everything" : "the buses") + ": every speaker shakes");
        }
        // Throws while the rig plays, and Swell.
        {
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.load(1, src.hissB, 1);
            scene.p.buses[0].path = static_cast<int>(room::Path::Around);
            for (auto& b : scene.p.buses)
                b.low = b.swirl = b.filter = b.drive = 0.4f; // the colours on: the throws' lane goes through them too
            scene.start(width);
            Take take;
            bool seen = false;
            scene.run(take, blocks, [&](int b)
            {
                if (b % 400 == 100)
                    scene.rig.throwWhoosh(17, 0.9f);
                seen = seen || scene.rig.getObject(RigEngine::ObjThrow1).active;
            });
            const auto s = stepsOf(take, 4800);
            check(allFinite(take) && seen && peakOf(take.ch[static_cast<size_t>(spk.layout.lfe)]) == 0.0f, "throws: a throw flies the room with the rig playing (its own object), the LFE stays silent");
            juce::ignoreUnused(s);

            // Swell, pressed four seconds in and held: a push at first, settling as it is held (as in stereo).
            Scene ridden;
            ridden.load(0, src.hissA, 0);
            ridden.load(1, src.hissB, 1);
            ridden.p.rideOn = true;
            ridden.p.energy = 0.35f;
            for (auto& b : ridden.p.buses)
                b.passMode = 0;
            ridden.start(width);
            Take t;
            const int perSecond = static_cast<int>(c_fs / c_block);
            ridden.run(t, 34 * perSecond, [&](int b)
            {
                ridden.p.swell = b >= 4 * perSecond;
                ridden.rig.setParams(ridden.p);
            });
            auto level = [&](double from, double to)
            {
                double sum = 0.0;
                int n = 0;
                for (int b = static_cast<int>(from * perSecond); b < static_cast<int>(to * perSecond); ++b, ++n)
                    sum += t.blocks[static_cast<size_t>(b)].total;
                return db(sum / n);
            };
            const float pushed = level(4.3, 5.3), held = level(14.0, 34.0);
            std::cout << "         Swell: " << num(pushed - held, 1) << " dB over the held level in its first second" << std::endl;
            check(pushed > held + 1.5f, "Swell: a push at first that settles as it is held, in a room too");
        }
    }

    //==================================================================================================================
    std::cout << "Stowaway in a room" << std::endl;
    for (auto width : { room::Width::Surround51, room::Width::Surround714 })
    {
        const Speakers spk(width);
        const int blocks = static_cast<int>(20.0 * c_fs / c_block);
        const int left = spk.layout.indexOf(room::Speaker::L), right = spk.layout.indexOf(room::Speaker::R);
        auto othersPeak = [&](const Take& take)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < c_wide; ++ch)
                if (ch != left && ch != right)
                    peak = std::max(peak, peakOf(take.ch[static_cast<size_t>(ch)]));
            return peak;
        };
        // Sub: planted at the front, the same in L and R and nowhere else; its stem is what each of them got.
        {
            Scene scene;
            scene.load(0, src.drone, CargoHold::c_stowBus);
            scene.p.stow.on = true;
            scene.p.stow.mode = RigEngine::StowSub;
            scene.p.stow.follow = 0.0f;
            scene.start(width);
            Take take;
            scene.run(take, blocks);
            const auto& l = take.ch[static_cast<size_t>(left)];
            const auto& r = take.ch[static_cast<size_t>(right)];
            float apart = 0.0f, stemApart = 0.0f;
            sj::DcBlock dc; // the engine's last stage, which the stem is taken before
            dc.set(8.0f, static_cast<float>(c_fs));
            for (size_t i = 0; i < l.size(); ++i)
            {
                apart = std::max(apart, std::abs(l[i] - r[i]));
                stemApart = std::max(stemApart, std::abs(dc.process(take.stem[i]) - l[i]));
            }
            std::cout << "         " << nameOf(width) << " Sub: L peak " << num(peakOf(l), 3) << ", L against R " << num(apart, 7) << ", the stem against L "
                      << num(stemApart, 7) << ", every other channel " << num(othersPeak(take), 7) << std::endl;
            check(peakOf(l) > 0.05f && apart == 0.0f && othersPeak(take) == 0.0f, nameOf(width) + " Sub: at the front, the same in L and R (a phantom centre), nothing in any other speaker");
            check(peakOf(take.stem) > 0.05f && stemApart < 1.0e-6f, nameOf(width) + " Sub: its stem (for an LFE) is exactly what L and R each carry");
        }
        // Dry: straight to L and R; a stereo sound keeps its sides.
        {
            Scene scene;
            scene.load(0, src.stereoBed, CargoHold::c_stowBus);
            scene.p.stow.on = true;
            scene.p.stow.mode = RigEngine::StowDry;
            scene.start(width);
            Take take;
            scene.run(take, blocks);
            float apart = 0.0f;
            for (size_t i = 0; i < take.ch[0].size(); ++i)
                apart = std::max(apart, std::abs(take.ch[static_cast<size_t>(left)][i] - take.ch[static_cast<size_t>(right)][i]));
            check(peakOf(take.ch[static_cast<size_t>(left)]) > 0.05f && apart > 0.05f && othersPeak(take) == 0.0f && peakOf(take.stem) == 0.0f,
                  nameOf(width) + " Dry: straight to L and R (a stereo sound still wide), nothing anywhere else, and no Sub stem");
        }
        // Fly: its own passes on its own Flight Path (bus A's is another, and bus A is empty).
        {
            Scene scene;
            scene.load(0, src.hissA, CargoHold::c_stowBus);
            scene.p.stow.on = true;
            scene.p.stow.mode = RigEngine::StowFly;
            scene.p.stow.follow = 0.0f;
            scene.p.stow.path = static_cast<int>(room::Path::Through);
            scene.p.buses[0].path = static_cast<int>(room::Path::Across);
            scene.start(width);
            Take take;
            scene.run(take, static_cast<int>(60.0 * c_fs / c_block));
            std::vector<double> heardY, saidY;
            int passes = 0, inOrder = 0;
            for (int h = 0; h < 2; ++h)
                for (const auto& pass : passesOf(take, [h](const Block& b) { return b.flightStow[static_cast<size_t>(h)].age; },
                                                 [h](const Block& b) { return b.flightStow[static_cast<size_t>(h)].env; },
                                                 [h](const Block& b) { return b.flightStow[static_cast<size_t>(1 - h)].env; }))
                {
                    ++passes;
                    const int obj = RigEngine::ObjStow1 + h;
                    for (int b : pass)
                    {
                        heardY.push_back(spk.centroid(take.blocks[static_cast<size_t>(b)]).y);
                        saidY.push_back(take.blocks[static_cast<size_t>(b)].object[static_cast<size_t>(obj)].y);
                    }
                    const bool toBack = third(take, pass, 2, [&](const Block& b) { return static_cast<double>(b.object[static_cast<size_t>(obj)].y); })
                                        < third(take, pass, 0, [&](const Block& b) { return static_cast<double>(b.object[static_cast<size_t>(obj)].y); });
                    const auto ahead = [&](const Block& b) { return spk.share(b, isAhead) - spk.share(b, isBehind); };
                    const double start = third(take, pass, 0, ahead), end = third(take, pass, 2, ahead);
                    const double first = toBack ? start : -start, last = toBack ? end : -end;
                    if (first > 0.3 && last < first - 0.5)
                        ++inOrder;
                }
            std::cout << "         " << nameOf(width) << " Fly: " << passes << " passes, " << inOrder << " front to back (or back to front); energy against its objects, y "
                      << num(correlation(heardY, saidY)) << std::endl;
            check(allFinite(take) && passes >= 6 && inOrder == passes && correlation(heardY, saidY) > 0.9,
                  nameOf(width) + " Fly: its passes fly Stowaway's own Flight Path (Through, while bus A's is Across)");
        }
        // Wake: the trail is on bus A's path, behind bus A's outgoing pass.
        {
            Scene scene;
            scene.load(0, src.hissA, 0);
            scene.load(1, src.hissC, CargoHold::c_stowBus);
            scene.p.levels[0] = 0.0005f; // bus A all but silent (the trail then goes by its passes alone): what is heard is the trail
            scene.p.buses[0].path = static_cast<int>(room::Path::Through);
            scene.p.stow.on = true;
            scene.p.stow.mode = RigEngine::StowWake;
            scene.p.stow.follow = 1.0f;
            scene.start(width);
            Take take;
            scene.run(take, static_cast<int>(60.0 * c_fs / c_block));
            std::vector<double> heardY, saidY;
            int counted = 0, behind = 0;
            double top = 0.0;
            for (const auto& b : take.blocks)
                top = std::max(top, b.total);
            for (size_t b = 400; b < take.blocks.size(); ++b)
            {
                const auto& block = take.blocks[b];
                if (block.total < top * 1.0e-2)
                    continue;
                const auto& wake = block.object[RigEngine::ObjStow1];
                heardY.push_back(spk.centroid(block).y);
                saidY.push_back(wake.y);
                // Bus A's pass on its way out (the one past half way), and the trail: on the same side of the
                // listener (front or back) and no further from them.
                const int out = block.flightA[0].age > 0.5f ? 0 : 1;
                const auto& pass = block.object[static_cast<size_t>(RigEngine::ObjPassA1 + out)];
                if (std::abs(pass.y) < 0.3f)
                    continue;
                ++counted;
                if (wake.y * pass.y > 0.0f && std::abs(wake.y) < std::abs(pass.y))
                    ++behind;
            }
            std::cout << "         " << nameOf(width) << " Wake: behind bus A's outgoing pass in " << behind << " of " << counted
                      << " blocks; energy against its object, y " << num(correlation(heardY, saidY)) << std::endl;
            check(allFinite(take) && counted > 200 && behind > counted * 0.95 && correlation(heardY, saidY) > 0.9,
                  nameOf(width) + " Wake: the trail follows bus A's outgoing pass along its Flight Path, and is heard there");
        }
    }

    //==================================================================================================================
    std::cout << "Stereo beds in a room" << std::endl;
    {
        // The same bed twice on bus B (Pass Mode Bed): once as a stereo file, once with both sides alike. How alike
        // the left and right speakers of a pair are (their correlation) says whether the bed kept its width.
        const auto width = room::Width::Surround51;
        const Speakers spk(width);
        auto alike = [&](const juce::File& file, float spread, room::Speaker a, room::Speaker b)
        {
            Scene scene;
            scene.load(0, file, 1);
            scene.p.buses[1].passMode = 0;
            scene.p.buses[1].doppler = 0.2f;
            scene.p.roomSpread = spread;
            scene.start(width);
            Take take;
            scene.run(take, static_cast<int>(20.0 * c_fs / c_block));
            const auto& l = take.ch[static_cast<size_t>(spk.layout.indexOf(a))];
            const auto& r = take.ch[static_cast<size_t>(spk.layout.indexOf(b))];
            std::vector<double> x(l.begin() + 48000, l.end()), y(r.begin() + 48000, r.end());
            return std::make_pair(correlation(x, y), peakOf(r));
        };
        const auto frontMono = alike(src.monoBed, 0.0f, room::Speaker::L, room::Speaker::R);
        const auto frontStereo = alike(src.stereoBed, 0.0f, room::Speaker::L, room::Speaker::R);
        const auto rearStereoPoint = alike(src.stereoBed, 0.0f, room::Speaker::Ls, room::Speaker::Rs);
        const auto rearMono = alike(src.monoBed, 0.8f, room::Speaker::Ls, room::Speaker::Rs);
        const auto rearStereo = alike(src.stereoBed, 0.8f, room::Speaker::Ls, room::Speaker::Rs);
        std::cout << "         L against R: a bed with both sides alike " << num(frontMono.first) << ", the stereo bed " << num(frontStereo.first)
                  << "; Ls against Rs with Spread at 80%: alike " << num(rearMono.first) << ", stereo " << num(rearStereo.first)
                  << " (Spread 0%: " << num(rearStereoPoint.first) << ")" << std::endl;
        check(frontStereo.first < frontMono.first - 0.2, "a stereo bed is wider across the front pair than the same bed with both sides alike");
        check(rearStereo.first < rearMono.first - 0.2 && rearStereo.first < rearStereoPoint.first - 0.1 && rearStereo.second > 0.01f,
              "Spread takes its sides into the surrounds too: it widens into the room");

        // A throw must not take the bed's sides away. With Spread at 100% every pass and throw is the same in L
        // and R, so L minus R is the bed's sides alone: they carry on, as loud, while a throw flies and after it.
        {
            Scene scene;
            scene.load(0, src.stereoBed, 1);
            scene.p.buses[1].passMode = 0;
            scene.p.buses[1].doppler = 0.2f;
            scene.p.roomSpread = 1.0f;
            scene.start(width);
            Take take;
            const int perSecond = static_cast<int>(c_fs / c_block);
            scene.run(take, 12 * perSecond, [&](int b)
            {
                if (b == 6 * perSecond)
                    scene.rig.throwWhoosh(17, 0.9f);
            });
            const auto& l = take.ch[static_cast<size_t>(spk.layout.indexOf(room::Speaker::L))];
            const auto& r = take.ch[static_cast<size_t>(spk.layout.indexOf(room::Speaker::R))];
            auto sides = [&](int fromBlock, int toBlock, bool least)
            {
                double best = least ? 1.0e30 : 0.0, sum = 0.0;
                for (int b = fromBlock; b < toBlock; ++b)
                {
                    double p = 0.0;
                    for (int i = b * c_block; i < (b + 1) * c_block; ++i)
                        p += static_cast<double>(l[static_cast<size_t>(i)] - r[static_cast<size_t>(i)]) * (l[static_cast<size_t>(i)] - r[static_cast<size_t>(i)]);
                    best = std::min(best, p);
                    sum += p;
                }
                return least ? best : sum / (toBlock - fromBlock);
            };
            const double before = sides(2 * perSecond, 6 * perSecond, false), leastBefore = sides(2 * perSecond, 6 * perSecond, true);
            const double leastDuring = sides(6 * perSecond, 12 * perSecond, true), during = sides(6 * perSecond, 12 * perSecond, false);
            std::cout << "         the bed's sides (L minus R): " << num(db(during) - db(before), 1) << " dB with a throw in flight against before it; quietest block "
                      << num(db(leastDuring) - db(before), 1) << " dB (before the throw " << num(db(leastBefore) - db(before), 1) << " dB)" << std::endl;
            check(before > 1.0e-6 && std::abs(db(during) - db(before)) < 3.0f && db(leastDuring) > db(leastBefore) - 6.0f,
                  "a stereo bed keeps its sides while a throw flies");
        }
    }

    //==================================================================================================================
    std::cout << "Mono and Stereo: the stereo engine, and the objects are a room's" << std::endl;
    {
        auto scene = [&](Scene& s)
        {
            s.load(0, src.hissA, 0);
            s.load(1, src.stereoBed, 1);
            s.load(2, src.hits, CargoHold::c_hitsBus);
            s.p.buses[0].path = static_cast<int>(room::Path::Around); // (not heard at Stereo)
            s.p.roomSpread = 0.7f;
            s.p.roomCentre = true;
            s.p.space = 0.3f;
        };
        Scene twelve, two;
        scene(twelve);
        scene(two);
        twelve.start(room::Width::Stereo);
        two.rig.setCargoHold(&two.hold);
        two.rig.setParams(two.p);
        two.rig.prepare(c_fs, c_block); // never told a width
        two.rig.setParams(two.p);
        Take wide;
        twelve.run(wide, 1000);
        float apart = 0.0f, beyond = 0.0f;
        bool anyObject = false;
        std::vector<float> l(c_block), r(c_block);
        for (int b = 0; b < 1000; ++b)
        {
            two.rig.process(nullptr, nullptr, l.data(), r.data(), c_block);
            for (int i = 0; i < c_block; ++i)
                apart = std::max({ apart, std::abs(l[static_cast<size_t>(i)] - wide.ch[0][static_cast<size_t>(b * c_block + i)]),
                                   std::abs(r[static_cast<size_t>(i)] - wide.ch[1][static_cast<size_t>(b * c_block + i)]) });
            for (const auto& o : wide.blocks[static_cast<size_t>(b)].object)
                anyObject = anyObject || o.active;
        }
        for (int ch = 2; ch < c_wide; ++ch)
            beyond = std::max(beyond, peakOf(wide.ch[static_cast<size_t>(ch)]));
        check(peakOf(wide.ch[0]) > 0.05f && apart == 0.0f && beyond == 0.0f,
              "at Stereo the twelve-channel call is the two-channel one sample for sample (whatever the Flight Path and the room's settings), the rest silent");
        check(! anyObject, "and no object is reported: they belong to a room");

        // To a room and back: the stereo engine carries on, and is the stereo engine again.
        Take back;
        twelve.rig.setWidth(room::Width::Surround714);
        twelve.run(back, 400);
        twelve.rig.setWidth(room::Width::Stereo);
        twelve.run(back, 400);
        float tail = 0.0f;
        for (int ch = 2; ch < c_wide; ++ch)
            for (size_t i = static_cast<size_t>(500 * c_block); i < back.ch[0].size(); ++i)
                tail = std::max(tail, std::abs(back.ch[static_cast<size_t>(ch)][i]));
        check(allFinite(back) && twelve.rig.getWidth() == room::Width::Stereo && tail == 0.0f && back.blocks[200].power[4] > 0.0 && back.blocks.back().total > 0.0,
              "a change to 7.1.4 and back: the room sounds, then only L and R do");
    }

    //==================================================================================================================
    std::cout << "Speed: the engine in a room against Stereo" << std::endl;
    {
        // A full rig: both buses with their colours, the hits, Stowaway flying, Space.
        for (double rate : { 48000.0, 96000.0, 192000.0 })
        {
            double percent[2] = { 0.0, 0.0 }, worstBlock[2] = { 0.0, 0.0 };
            int which = 0;
            for (auto width : { room::Width::Stereo, room::Width::Surround714 })
            {
                Scene scene;
                scene.p = RigEngine::Params {};
                scene.load(0, src.hissA, 0);
                scene.load(1, src.hissB, 1);
                scene.load(2, src.hits, CargoHold::c_hitsBus);
                scene.load(3, src.hissC, CargoHold::c_stowBus);
                scene.p.stow.on = true;
                scene.p.stow.mode = RigEngine::StowFly;
                scene.p.buses[0].path = static_cast<int>(room::Path::Overhead);
                scene.p.buses[1].path = static_cast<int>(room::Path::Around);
                scene.start(width, rate);
                std::array<std::vector<float>, c_wide> buffers;
                float* out[c_wide];
                for (int c = 0; c < c_wide; ++c)
                {
                    buffers[static_cast<size_t>(c)].assign(c_block, 0.0f);
                    out[c] = buffers[static_cast<size_t>(c)].data();
                }
                const int blocks = static_cast<int>(10.0 * rate / c_block);
                for (int b = 0; b < blocks / 10; ++b) // warm up
                    scene.rig.process(nullptr, nullptr, out, c_wide, c_block);
                const double t0 = TestClock::cpuMs();
                for (int b = 0; b < blocks; ++b)
                {
                    const double b0 = TestClock::cpuMs();
                    scene.rig.process(nullptr, nullptr, out, c_wide, c_block);
                    worstBlock[which] = std::max(worstBlock[which], TestClock::cpuMs() - b0);
                }
                percent[which] = 100.0 * (TestClock::cpuMs() - t0) / (1000.0 * blocks * c_block / rate);
                worstBlock[which] = 100.0 * worstBlock[which] / (1000.0 * c_block / rate);
                ++which;
            }
            std::cout << "         " << num(rate / 1000.0, 0) << " kHz: Stereo " << num(percent[0], 1) << "% of a core (worst block " << num(worstBlock[0], 1)
                      << "%), 7.1.4 " << num(percent[1], 1) << "% (worst block " << num(worstBlock[1], 1) << "%): " << num(percent[1] / percent[0], 2) << " times" << std::endl;
            check(percent[1] < 100.0 && percent[1] < 2.0 * percent[0] + 1.0, "the engine at 7.1.4 runs in real time at " + num(rate / 1000.0, 0) + " kHz, and costs under twice what Stereo does");
        }
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

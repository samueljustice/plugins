// Room test: the maths of surround output (Source/Room.h), with no engine.
//
//   layouts      the seven widths against JUCE's own channel sets, channel by channel, and back again
//   panner       constant power everywhere, the LFE never fed, a sound at a speaker plays from that speaker
//                alone, left-right symmetry, the Centre, Height and Spread controls, widths without tops or rears,
//                and no gain jumps along any flight path
//   flight paths the documented shape of each, and where the sound actually goes at each width
//   fold-downs   every coefficient of the documented steps, wider folds as those steps in a row, the LFE, and
//                what a fold does to the level of a sound placed anywhere
//   file orders  every order line, speaker mask and Multi-Mono suffix, and the Pro Tools order against the
//                order of JUCE's AAX wrapper
//
//   SwooshbucklerRoom        prints what it measures and ends with "ALL PASSED" (exit code 0) or "n FAILED".

#include <JuceHeader.h>
#include "Room.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void check(bool ok, const std::string& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }

    std::string num(double v, int places = 3) { return juce::String(v, places).toStdString(); }
    double toDb(double gain) { return 20.0 * std::log10(std::max(gain, 1.0e-12)); }
    double powerDb(double power) { return 10.0 * std::log10(std::max(power, 1.0e-24)); }
    bool near(double a, double b, double tolerance = 1.0e-5) { return std::abs(a - b) <= tolerance; }

    using room::Width;
    using S = room::Speaker;
    using Type = juce::AudioChannelSet::ChannelType;

    const std::array<Width, room::c_numWidths> c_widths { Width::Mono, Width::Stereo, Width::Quad, Width::Surround51,
                                                           Width::Surround71, Width::Surround712, Width::Surround714 };
    const std::array<room::Path, room::c_numPaths> c_paths { room::Path::Across, room::Path::Through,
                                                              room::Path::Overhead, room::Path::Around };

    std::string name(Width w) { return room::layout(w).name; }

    /** The processing order spelt out. */
    std::string processingOrder(Width w)
    {
        const auto& l = room::layout(w);
        std::string s;
        for (int c = 0; c < l.numChannels; ++c)
            s += (c > 0 ? " " : "") + std::string(room::info(l.speakers[static_cast<size_t>(c)]).name);
        return s;
    }

    double power(const room::Gains& g)
    {
        double sum = 0.0;
        for (const float v : g)
            sum += static_cast<double>(v) * v;
        return sum;
    }

    float gainOf(Width w, const room::Gains& g, S speaker)
    {
        const int at = room::layout(w).indexOf(speaker);
        return at < 0 ? 0.0f : g[static_cast<size_t>(at)];
    }

    room::Gains panned(Width w, float x, float y, float z, float spread = 0.0f, float centre = 0.0f, float height = 1.0f)
    {
        room::Gains g;
        g.fill(99.0f); // pan() must write every entry
        room::pan(w, { x, y, z }, { spread, centre, height }, g);
        return g;
    }

    //==================================================================================================================
    void testLayouts()
    {
        std::cout << "Layouts" << std::endl;

        struct Expect { Width width; const char* name; int channels; const char* order; juce::AudioChannelSet set; };
        const std::vector<Expect> expected {
            { Width::Mono,        "Mono",   1,  "C",                                          juce::AudioChannelSet::mono() },
            { Width::Stereo,      "Stereo", 2,  "L R",                                        juce::AudioChannelSet::stereo() },
            { Width::Quad,        "Quad",   4,  "L R Ls Rs",                                  juce::AudioChannelSet::quadraphonic() },
            { Width::Surround51,  "5.1",    6,  "L R C LFE Ls Rs",                            juce::AudioChannelSet::create5point1() },
            { Width::Surround71,  "7.1",    8,  "L R C LFE Lss Rss Lsr Rsr",                  juce::AudioChannelSet::create7point1() },
            { Width::Surround712, "7.1.2",  10, "L R C LFE Lss Rss Lsr Rsr Ltm Rtm",          juce::AudioChannelSet::create7point1point2() },
            { Width::Surround714, "7.1.4",  12, "L R C LFE Lss Rss Ltf Rtf Ltr Rtr Lsr Rsr",  juce::AudioChannelSet::create7point1point4() },
        };

        for (const auto& e : expected)
        {
            const auto& l = room::layout(e.width);
            bool typesMatch = e.set.size() == l.numChannels;
            for (int c = 0; typesMatch && c < l.numChannels; ++c)
                typesMatch = e.set.getTypeOfChannel(c) == room::info(l.speakers[static_cast<size_t>(c)]).type
                             && e.set.getChannelIndexForType(room::info(l.speakers[static_cast<size_t>(c)]).type) == c;
            check(l.width == e.width && std::string(l.name) == e.name && l.numChannels == e.channels
                      && processingOrder(e.width) == e.order,
                  std::string(e.name) + ": " + std::to_string(l.numChannels) + " channels, processing order \"" + processingOrder(e.width) + "\"");
            check(typesMatch && room::channelSet(e.width) == e.set,
                  std::string(e.name) + ": every channel is the same speaker as in JUCE's set (" + e.set.getDescription().toStdString() + ")");
            const auto back = room::widthFor(e.set);
            check(back.has_value() && *back == e.width, std::string(e.name) + ": found again from its channel set");

            int lfe = -1, mains = 0, tops = 0;
            for (int c = 0; c < l.numChannels; ++c)
            {
                const auto role = room::info(l.speakers[static_cast<size_t>(c)]).role;
                if (role == room::Role::Lfe) lfe = c;
                mains += role == room::Role::Main ? 1 : 0;
                tops += role == room::Role::Height ? 1 : 0;
            }
            const int wantTops = e.width == Width::Surround714 ? 4 : (e.width == Width::Surround712 ? 2 : 0);
            const int wantLfe = e.channels >= 6 ? 3 : -1;
            check(l.lfe == lfe && lfe == wantLfe && tops == wantTops && mains == e.channels - wantTops - (wantLfe >= 0 ? 1 : 0),
                  std::string(e.name) + ": " + std::to_string(mains) + " mains, " + std::to_string(tops) + " tops, LFE on channel " + std::to_string(lfe));
        }

        check(room::layout(Width::Surround714).indexOf(S::Lsr) == 10 && room::layout(Width::Surround714).indexOf(S::Rsr) == 11
                  && room::layout(Width::Surround714).indexOf(S::Ltf) == 6,
              "7.1.4: the rear surrounds are the LAST two channels (10, 11), after the tops (6..9)");
        check(room::layout(Width::Quad).indexOf(S::C) < 0 && room::layout(Width::Quad).indexOf(S::Lfe) < 0
                  && room::layout(Width::Quad).centre < 0 && room::layout(Width::Quad).lfe < 0,
              "Quad has no centre and no LFE");
        check(room::layout(Width::Surround51).centre == 2 && room::layout(Width::Mono).centre == 0 && room::layout(Width::Stereo).centre < 0,
              "the centre channel: 2 in 5.1 and wider, Mono's one channel, none in Stereo");

        const std::vector<juce::AudioChannelSet> strangers {
            juce::AudioChannelSet::create5point0(), juce::AudioChannelSet::create7point1SDDS(), juce::AudioChannelSet::create7point0(),
            juce::AudioChannelSet::create5point1point2(), juce::AudioChannelSet::create5point1point4(), juce::AudioChannelSet::createLCR(),
            juce::AudioChannelSet::discreteChannels(12), juce::AudioChannelSet::discreteChannels(2), juce::AudioChannelSet::disabled(),
            juce::AudioChannelSet::ambisonic(1), juce::AudioChannelSet::create7point1point6()
        };
        bool noneFound = true;
        for (const auto& s : strangers)
            noneFound = noneFound && ! room::widthFor(s).has_value();
        check(noneFound, "other layouts (5.0, 7.1 SDDS, 5.1.2, discrete, ambisonic ...) are not mistaken for one of the seven");

        // Where the speakers stand: each side mirrors the other, in the room and by angle; tops are up, the rest are not.
        bool mirrorOk = true, standOk = true;
        for (int i = 0; i < room::c_numSpeakers; ++i)
        {
            const auto s = static_cast<S>(i);
            const auto& a = room::info(s);
            const auto& b = room::info(room::mirrored(s));
            mirrorOk = mirrorOk && room::mirrored(room::mirrored(s)) == s && near(a.x, -b.x) && near(a.y, b.y) && near(a.z, b.z)
                       && near(a.azimuth, -b.azimuth) && near(a.elevation, b.elevation) && a.role == b.role;
            standOk = standOk && (a.role == room::Role::Height ? (near(a.z, 1.0) && near(a.elevation, 45.0)) : (near(a.z, 0.0) && near(a.elevation, 0.0)))
                      && (a.x < 0.0f) == (a.azimuth < 0.0f) && std::abs(a.x) <= 1.0f && std::abs(a.y) <= 1.0f
                      && (std::abs(a.azimuth) < 90.0f ? a.y > 0.0f : (std::abs(a.azimuth) > 90.0f ? a.y < 0.0f : near(a.y, 0.0)));
        }
        check(mirrorOk, "every left speaker has its right twin at the mirrored place");
        check(standOk, "tops are at the ceiling 45 degrees up, the others at ear level; front speakers are in front, rear ones behind");
        check(near(room::info(S::L).azimuth, -30.0) && near(room::info(S::C).azimuth, 0.0) && near(room::info(S::Ls).azimuth, -110.0)
                  && near(room::info(S::Lss).azimuth, -90.0) && near(room::info(S::Lsr).azimuth, -135.0)
                  && near(room::info(S::Ltf).azimuth, -45.0) && near(room::info(S::Ltr).azimuth, -135.0) && near(room::info(S::Ltm).azimuth, -90.0),
              "speaker angles: L 30, C 0, Ls 110, Lss 90, Lsr 135, Ltf 45, Ltr 135, Ltm 90 degrees");
    }

    //==================================================================================================================
    void testPanner()
    {
        std::cout << "Panner" << std::endl;

        const std::vector<float> steps { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

        for (const Width w : c_widths)
        {
            const auto& l = room::layout(w);

            // Constant power, the LFE never fed, nothing written past the last channel, every gain in 0..1.
            double worstDb = 0.0;
            bool lfeSilent = true, restZero = true, inRange = true;
            long points = 0;
            for (const float spread : steps)
                for (const float centre : steps)
                    for (const float height : steps)
                        for (int ix = -20; ix <= 20; ++ix)
                            for (int iy = -20; iy <= 20; ++iy)
                                for (int iz = 0; iz <= 8; ++iz)
                                {
                                    const auto g = panned(w, static_cast<float>(ix) / 20.0f, static_cast<float>(iy) / 20.0f, static_cast<float>(iz) / 8.0f, spread, centre, height);
                                    worstDb = std::max(worstDb, std::abs(powerDb(power(g))));
                                    if (l.lfe >= 0)
                                        lfeSilent = lfeSilent && g[static_cast<size_t>(l.lfe)] == 0.0f;
                                    for (size_t c = 0; c < room::c_maxChannels; ++c)
                                    {
                                        inRange = inRange && std::isfinite(g[c]) && g[c] >= 0.0f && g[c] <= 1.00001f;
                                        if (static_cast<int>(c) >= l.numChannels)
                                            restZero = restZero && g[c] == 0.0f;
                                    }
                                    ++points;
                                }
            check(worstDb < 0.001, name(w) + ": the gains' power is 1 at every position and setting (worst " + num(worstDb, 6) + " dB off over " + std::to_string(points) + " points; must be within 0.1)");
            check(lfeSilent && restZero && inRange, name(w) + ": the LFE gets nothing, unused channels are zero, every gain is between 0 and 1");

            // Out-of-range input is clamped to the walls.
            const auto wild = panned(w, -7.0f, 5.0f, 3.0f, 4.0f, -2.0f, 9.0f);
            const auto wall = panned(w, -1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f);
            check(wild == wall, name(w) + ": positions and settings outside their range are held at the limit");

            // Left-right symmetry: mirroring x swaps each pair of speakers and leaves C alone.
            double worstMirror = 0.0;
            for (const float spread : { 0.0f, 0.3f })
                for (const float centre : { 0.0f, 0.6f, 1.0f })
                    for (int ix = -10; ix <= 10; ++ix)
                        for (int iy = -10; iy <= 10; ++iy)
                            for (int iz = 0; iz <= 4; ++iz)
                            {
                                const float x = static_cast<float>(ix) / 10.0f, y = static_cast<float>(iy) / 10.0f, z = static_cast<float>(iz) / 4.0f;
                                const auto a = panned(w, x, y, z, spread, centre);
                                const auto b = panned(w, -x, y, z, spread, centre);
                                for (int c = 0; c < l.numChannels; ++c)
                                    worstMirror = std::max(worstMirror, static_cast<double>(std::abs(a[static_cast<size_t>(c)]
                                        - gainOf(w, b, room::mirrored(l.speakers[static_cast<size_t>(c)])))));
                            }
            check(worstMirror < 1.0e-6, name(w) + ": left and right are mirror images (worst difference " + num(worstMirror, 8) + ")");
        }

        // A sound at a speaker's place plays from that speaker alone (Centre full, so the centre speaker has a place).
        for (const Width w : { Width::Quad, Width::Surround51, Width::Surround71, Width::Surround712, Width::Surround714 })
        {
            const auto& l = room::layout(w);
            double worstOwn = 0.0, worstOther = -200.0;
            for (int c = 0; c < l.numChannels; ++c)
            {
                const auto& sp = room::info(l.speakers[static_cast<size_t>(c)]);
                if (sp.role == room::Role::Lfe)
                    continue;
                const auto g = panned(w, sp.x, sp.y, sp.z, 0.0f, 1.0f, 1.0f);
                worstOwn = std::max(worstOwn, std::abs(toDb(g[static_cast<size_t>(c)])));
                for (int o = 0; o < l.numChannels; ++o)
                    if (o != c)
                        worstOther = std::max(worstOther, toDb(g[static_cast<size_t>(o)]));
            }
            check(worstOwn < 0.01 && worstOther < -30.0, name(w) + ": a sound at a speaker plays from it alone (own gain within " + num(worstOwn, 4)
                                                             + " dB of full, loudest other speaker " + num(worstOther, 1) + " dB; must be under -30)");
        }

        // Centre: 0 is a phantom centre, 1 is the centre speaker.
        for (const Width w : { Width::Surround51, Width::Surround71, Width::Surround712, Width::Surround714 })
        {
            bool silentAtZero = true;
            for (int ix = -20; ix <= 20; ++ix)
                for (int iy = -20; iy <= 20; ++iy)
                    silentAtZero = silentAtZero && gainOf(w, panned(w, static_cast<float>(ix) / 20.0f, static_cast<float>(iy) / 20.0f, 0.5f, 0.7f, 0.0f), S::C) == 0.0f;
            const auto phantom = panned(w, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
            const auto half = panned(w, 0.0f, 1.0f, 0.0f, 0.0f, 0.5f);
            const auto full = panned(w, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
            check(silentAtZero && near(gainOf(w, phantom, S::L), 0.70710678, 1.0e-5) && near(gainOf(w, phantom, S::R), 0.70710678, 1.0e-5),
                  name(w) + ": Centre 0 leaves the centre speaker silent everywhere, even with Spread; front centre is L and R at -3 dB");
            check(near(gainOf(w, full, S::C), 1.0) && gainOf(w, full, S::L) < 1.0e-4f && gainOf(w, full, S::R) < 1.0e-4f
                      && near(gainOf(w, half, S::C), 0.70710678, 1.0e-5) && near(gainOf(w, half, S::L), 0.5, 1.0e-5),
                  name(w) + ": Centre 1 puts front centre in the centre speaker alone; Centre 0.5 shares the power half and half");
        }
        check(panned(Width::Quad, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f) == panned(Width::Quad, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f)
                  && panned(Width::Stereo, 0.3f, 1.0f, 0.0f, 0.0f, 1.0f) == panned(Width::Stereo, 0.3f, 1.0f, 0.0f, 0.0f, 0.0f),
              "Quad and Stereo have no centre speaker: Centre changes nothing");

        // Height.
        for (const Width w : { Width::Surround712, Width::Surround714 })
        {
            const auto& l = room::layout(w);
            double topsAtZero = 0.0, bedAtTop = 0.0, topsOnFloor = 0.0;
            for (int ix = -10; ix <= 10; ++ix)
                for (int iy = -10; iy <= 10; ++iy)
                {
                    const float x = static_cast<float>(ix) / 10.0f, y = static_cast<float>(iy) / 10.0f;
                    const auto heightOff = panned(w, x, y, 1.0f, 0.0f, 0.5f, 0.0f);
                    const auto ceiling = panned(w, x, y, 1.0f, 0.0f, 0.5f, 1.0f);
                    const auto floor = panned(w, x, y, 0.0f, 0.0f, 0.5f, 1.0f);
                    for (int c = 0; c < l.numChannels; ++c)
                    {
                        const bool isTop = room::info(l.speakers[static_cast<size_t>(c)]).role == room::Role::Height;
                        if (isTop) topsAtZero = std::max(topsAtZero, static_cast<double>(heightOff[static_cast<size_t>(c)]));
                        if (isTop) topsOnFloor = std::max(topsOnFloor, static_cast<double>(floor[static_cast<size_t>(c)]));
                        if (! isTop) bedAtTop = std::max(bedAtTop, static_cast<double>(ceiling[static_cast<size_t>(c)]));
                    }
                }
            // Half way up at full Height, and at the ceiling with Height at a half, are the same split: half the power up.
            const auto halfUp = panned(w, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 1.0f);
            const auto halfHeight = panned(w, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.5f);
            double topPower = 0.0;
            for (int c = 0; c < l.numChannels; ++c)
                if (room::info(l.speakers[static_cast<size_t>(c)]).role == room::Role::Height)
                    topPower += static_cast<double>(halfUp[static_cast<size_t>(c)]) * halfUp[static_cast<size_t>(c)];
            check(topsAtZero == 0.0 && topsOnFloor == 0.0 && bedAtTop < 1.0e-4,
                  name(w) + ": Height 0 keeps everything in the bed; on the floor the tops are silent; at the ceiling the bed is (" + num(bedAtTop, 6) + ")");
            check(near(topPower, 0.5, 1.0e-5) && halfUp == halfHeight, name(w) + ": half way up, the tops carry half the power; Height scales z");
        }
        for (const Width w : { Width::Stereo, Width::Quad, Width::Surround51, Width::Surround71 })
        {
            bool same = true;
            for (int ix = -5; ix <= 5; ++ix)
                for (int iy = -5; iy <= 5; ++iy)
                    same = same && panned(w, static_cast<float>(ix) / 5.0f, static_cast<float>(iy) / 5.0f, 1.0f) == panned(w, static_cast<float>(ix) / 5.0f, static_cast<float>(iy) / 5.0f, 0.0f);
            check(same, name(w) + ": no tops, so a sound at the ceiling plays in the bed at full power (z changes nothing)");
        }
        {
            bool same = true;
            for (int ix = -5; ix <= 5; ++ix)
                same = same && panned(Width::Stereo, static_cast<float>(ix) / 5.0f, -1.0f, 0.0f) == panned(Width::Stereo, static_cast<float>(ix) / 5.0f, 1.0f, 0.0f);
            const auto left = panned(Width::Stereo, -1.0f, 0.0f, 0.0f), mid = panned(Width::Stereo, 0.0f, 0.0f, 0.0f);
            check(same && near(left[0], 1.0) && left[1] < 1.0e-4f && near(mid[0], 0.70710678, 1.0e-5) && near(mid[1], 0.70710678, 1.0e-5),
                  "Stereo: no rears, so depth changes nothing; hard left is L alone, the middle is -3 dB in each");
            const auto mono = panned(Width::Mono, 0.7f, -0.3f, 0.9f, 0.4f, 0.8f, 1.0f);
            check(mono[0] == 1.0f && power(mono) == 1.0, "Mono: always the one channel at full gain");
        }
        {
            // Depth in 5.1 and Quad: a sound behind the listener is in the surrounds; in the middle it is shared.
            const auto back = panned(Width::Surround51, -1.0f, -1.0f, 0.0f);
            const auto mid = panned(Width::Surround51, -1.0f, 0.0f, 0.0f);
            check(near(gainOf(Width::Surround51, back, S::Ls), 1.0) && near(gainOf(Width::Surround51, mid, S::L), 0.70710678, 1.0e-5)
                      && near(gainOf(Width::Surround51, mid, S::Ls), 0.70710678, 1.0e-5),
                  "5.1: the back corner is Ls alone; half way down the left wall is L and Ls at -3 dB each");
            // 7.1: the three rows hand over through the sides.
            const auto side = panned(Width::Surround71, 1.0f, 0.0f, 0.0f);
            const auto between = panned(Width::Surround71, 1.0f, -0.5f, 0.0f);
            check(near(gainOf(Width::Surround71, side, S::Rss), 1.0) && near(gainOf(Width::Surround71, between, S::Rss), 0.70710678, 1.0e-5)
                      && near(gainOf(Width::Surround71, between, S::Rsr), 0.70710678, 1.0e-5) && gainOf(Width::Surround71, between, S::R) == 0.0f,
                  "7.1: half way down the right wall is Rss alone; between sides and rears is -3 dB in each and nothing in front");
            // 7.1.2's one top pair takes the whole ceiling; 7.1.4 shares it front to back.
            const auto top2 = panned(Width::Surround712, -1.0f, 1.0f, 1.0f);
            const auto top4 = panned(Width::Surround714, -1.0f, 0.0f, 1.0f);
            check(near(gainOf(Width::Surround712, top2, S::Ltm), 1.0) && near(gainOf(Width::Surround714, top4, S::Ltf), 0.70710678, 1.0e-5)
                      && near(gainOf(Width::Surround714, top4, S::Ltr), 0.70710678, 1.0e-5),
                  "7.1.2: anywhere on the left of the ceiling is Ltm alone; 7.1.4: the middle of the ceiling's left edge is Ltf and Ltr at -3 dB");
        }

        // Spread.
        for (const Width w : { Width::Stereo, Width::Quad, Width::Surround51, Width::Surround71, Width::Surround712, Width::Surround714 })
        {
            const auto& l = room::layout(w);
            const auto g = panned(w, -1.0f, 1.0f, 0.0f, 1.0f, 0.0f);
            const auto withCentre = panned(w, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f);
            int bed = 0;
            for (int c = 0; c < l.numChannels; ++c)
                bed += room::info(l.speakers[static_cast<size_t>(c)]).role == room::Role::Main && l.speakers[static_cast<size_t>(c)] != S::C ? 1 : 0;
            bool even = true, evenWithCentre = true;
            for (int c = 0; c < l.numChannels; ++c)
            {
                const auto sp = l.speakers[static_cast<size_t>(c)];
                const auto role = room::info(sp).role;
                const double want = role == room::Role::Main && sp != S::C ? std::sqrt(1.0 / bed) : 0.0;
                const double wantC = role == room::Role::Main ? std::sqrt(1.0 / (bed + (l.centre >= 0 ? 1 : 0))) : 0.0;
                even = even && near(g[static_cast<size_t>(c)], want, 1.0e-5);
                evenWithCentre = evenWithCentre && near(withCentre[static_cast<size_t>(c)], wantC, 1.0e-5);
            }
            // More Spread never makes the loudest speaker louder.
            bool settles = true;
            float last = 2.0f;
            for (int i = 0; i <= 20; ++i)
            {
                const auto s = panned(w, -1.0f, 1.0f, 0.0f, static_cast<float>(i) / 20.0f, 0.0f);
                settles = settles && s[0] <= last + 1.0e-6f;
                last = s[0];
            }
            check(even && evenWithCentre && settles, name(w) + ": Spread 1 plays every ear-level speaker equally (" + std::to_string(bed)
                                                         + ", plus the centre speaker at Centre 1); raising Spread only ever takes level from the loudest speaker");
        }
        {
            const auto g = panned(Width::Surround714, 0.2f, -0.4f, 1.0f, 1.0f, 0.0f, 1.0f);
            check(near(gainOf(Width::Surround714, g, S::Ltf), 0.5) && near(gainOf(Width::Surround714, g, S::Rtr), 0.5) && gainOf(Width::Surround714, g, S::L) < 1.0e-4f,
                  "7.1.4: Spread 1 at the ceiling is the four tops equally, and nothing in the bed");
        }

        // No jumps: a fine sweep along every flight path, every width, a range of settings. The largest change of
        // any one gain between neighbouring steps is bounded by how fast the path moves (at most 2 pi per pass).
        const int sweep = 4000;
        double worstStep = 0.0;
        std::string worstWhere;
        for (const Width w : c_widths)
            for (const auto path : c_paths)
                for (const float closeness : { 0.0f, 0.5f, 1.0f })
                    for (const float spread : { 0.0f, 0.4f })
                        for (const float centre : { 0.0f, 0.5f, 1.0f })
                            for (const float direction : { 1.0f, -1.0f })
                            {
                                room::Gains before {};
                                for (int i = 0; i <= sweep; ++i)
                                {
                                    const auto f = room::flight(path, static_cast<float>(i) / static_cast<float>(sweep), closeness, direction, 1.0f);
                                    room::Gains g;
                                    room::pan(w, f.position, { spread, centre, 1.0f }, g);
                                    if (i > 0)
                                        for (size_t c = 0; c < room::c_maxChannels; ++c)
                                            if (const double d = std::abs(g[c] - before[c]); d > worstStep)
                                            {
                                                worstStep = d;
                                                worstWhere = name(w) + " " + room::pathName(path);
                                            }
                                    before = g;
                                }
                            }
        // A gain is at most |d gain / d position| = pi/2 times the position's speed; the fastest path (Around)
        // moves 2 pi per pass: that is under 10 per pass, 0.0025 per step of 1/4000. A hard switch between
        // speakers would be a step of 0.7 or more.
        check(worstStep < 0.0035, "no gain jumps along any flight path at any width: the largest step is " + num(worstStep, 5) + " per 1/4000 of a pass (" + worstWhere
                                      + "; a hard switch would be 0.7)");

        // The same sweep straight across each axis, where the rows hand over.
        double worstAxis = 0.0;
        for (const Width w : c_widths)
            for (int axis = 0; axis < 3; ++axis)
                for (const float other : { -1.0f, -0.3f, 0.0f, 0.6f, 1.0f })
                {
                    room::Gains before {};
                    for (int i = 0; i <= sweep; ++i)
                    {
                        const float t = static_cast<float>(i) / static_cast<float>(sweep);
                        const float v = 2.0f * t - 1.0f;
                        const room::Position p = axis == 0 ? room::Position { v, other, 0.5f } : (axis == 1 ? room::Position { other, v, 0.5f } : room::Position { other, other, t });
                        room::Gains g;
                        room::pan(w, p, { 0.0f, 1.0f, 1.0f }, g);
                        if (i > 0)
                            for (size_t c = 0; c < room::c_maxChannels; ++c)
                                worstAxis = std::max(worstAxis, static_cast<double>(std::abs(g[c] - before[c])));
                        before = g;
                    }
                }
        check(worstAxis < 0.001, "no gain jumps wall to wall or floor to ceiling: the largest step is " + num(worstAxis, 5) + " per 1/4000 of the way");
    }

    //==================================================================================================================
    void testFlightPaths()
    {
        std::cout << "Flight paths" << std::endl;
        using room::Path;

        check(std::string(room::pathName(Path::Across)) == "Across" && std::string(room::pathName(Path::Through)) == "Through"
                  && std::string(room::pathName(Path::Overhead)) == "Overhead" && std::string(room::pathName(Path::Around)) == "Around",
              "the four paths are named Across, Through, Overhead, Around");

        const auto at = [](Path path, float progress, float closeness = 0.0f, float direction = 1.0f, float side = 1.0f)
        {
            return room::flight(path, progress, closeness, direction, side);
        };
        const auto is = [](const room::Flight& f, double x, double y, double z)
        {
            return near(f.position.x, x, 1.0e-5) && near(f.position.y, y, 1.0e-5) && near(f.position.z, z, 1.0e-5);
        };

        // Across.
        check(is(at(Path::Across, 0.0f), -1.0, 1.0, 0.0) && is(at(Path::Across, 0.5f), 0.0, 1.0, 0.0) && is(at(Path::Across, 1.0f), 1.0, 1.0, 0.0)
                  && is(at(Path::Across, 0.0f, 0.0f, -1.0f), 1.0, 1.0, 0.0) && is(at(Path::Across, 1.0f, 0.0f, -1.0f), -1.0, 1.0, 0.0),
              "Across: left to right along the front wall; the other direction runs right to left");
        check(is(at(Path::Across, 0.5f, 1.0f), 0.0, 0.25, 0.0) && is(at(Path::Across, 0.25f, 0.5f), -0.5, 0.625, 0.0)
                  && is(at(Path::Across, 0.3f, 0.4f, 1.0f, -1.0f), at(Path::Across, 0.3f, 0.4f).position.x, at(Path::Across, 0.3f, 0.4f).position.y, 0.0),
              "Across: closeness brings the line from the front wall to a quarter of the way from the listener; side is not used");

        // Through.
        check(is(at(Path::Through, 0.0f), 0.6, 1.0, 0.0) && is(at(Path::Through, 0.5f), 0.6, 0.0, 0.0) && is(at(Path::Through, 1.0f), 0.6, -1.0, 0.0)
                  && is(at(Path::Through, 0.0f, 0.0f, -1.0f), 0.6, -1.0, 0.0) && is(at(Path::Through, 0.5f, 1.0f, 1.0f, -1.0f), -0.12, 0.0, 0.0),
              "Through: front wall to back wall past the head, 0.6 out when far and 0.12 when close, on either side; the other direction runs back to front");

        // Overhead.
        check(is(at(Path::Overhead, 0.0f), 0.3, 1.0, 0.0) && is(at(Path::Overhead, 0.5f), 0.3, 0.0, 1.0) && is(at(Path::Overhead, 1.0f), 0.3, -1.0, 0.0)
                  && is(at(Path::Overhead, 0.25f), 0.3, 0.5, 0.5) && is(at(Path::Overhead, 0.5f, 1.0f, 1.0f, -1.0f), 0.0, 0.0, 1.0)
                  && is(at(Path::Overhead, 1.0f, 0.0f, -1.0f, -1.0f), -0.3, 1.0, 0.0),
              "Overhead: an arch from the front wall over the listener to the back wall, on the floor at the walls and at the ceiling overhead");

        // Around.
        check(is(at(Path::Around, 0.0f), 0.0, 1.0, 0.0) && is(at(Path::Around, 0.25f), 1.0, 0.0, 0.0) && is(at(Path::Around, 0.5f), 0.0, -1.0, 0.0)
                  && is(at(Path::Around, 0.75f), -1.0, 0.0, 0.0) && is(at(Path::Around, 1.0f), 0.0, 1.0, 0.0),
              "Around: one full circle from straight ahead, by the right, behind and the left, back to where it began");
        check(is(at(Path::Around, 0.25f, 0.0f, -1.0f), -1.0, 0.0, 0.0) && is(at(Path::Around, 0.0f, 0.0f, 1.0f, -1.0f), 0.0, -1.0, 0.0)
                  && is(at(Path::Around, 0.25f, 0.0f, 1.0f, -1.0f), -1.0, 0.0, 0.0) && is(at(Path::Around, 0.25f, 1.0f), 0.45, 0.0, 0.0),
              "Around: the other direction goes by the left first; the other side starts behind; the closest circle has radius 0.45");

        // Every path: inside the room, no jumps, clamped ends, mirrored by side, distance as documented.
        bool inside = true, clamped = true, sidesMirror = true, distanceOk = true;
        double worstJump = 0.0;
        for (const auto path : c_paths)
            for (const float closeness : { 0.0f, 0.3f, 1.0f })
                for (const float direction : { 1.0f, -1.0f })
                    for (const float side : { 1.0f, -1.0f })
                    {
                        room::Flight before;
                        for (int i = 0; i <= 2000; ++i)
                        {
                            const float p = static_cast<float>(i) / 2000.0f;
                            const auto f = room::flight(path, p, closeness, direction, side);
                            inside = inside && std::abs(f.position.x) <= 1.00001f && std::abs(f.position.y) <= 1.00001f && f.position.z >= 0.0f && f.position.z <= 1.00001f;
                            const double top = path == Path::Overhead ? 0.8 - 0.55 * closeness : 0.0;
                            const double y2 = static_cast<double>(f.position.y) * f.position.y;
                            const double want = std::sqrt(static_cast<double>(f.position.x) * f.position.x + y2 + top * top * (1.0 - y2));
                            distanceOk = distanceOk && near(f.distance, want, 1.0e-5);
                            if (path != Path::Around)
                            {
                                const auto other = room::flight(path, p, closeness, direction, -side);
                                sidesMirror = sidesMirror && near(other.position.x, path == Path::Across ? f.position.x : -f.position.x, 1.0e-6) && near(other.position.y, f.position.y, 1.0e-6);
                            }
                            if (i > 0)
                                worstJump = std::max({ worstJump, static_cast<double>(std::abs(f.position.x - before.position.x)), static_cast<double>(std::abs(f.position.y - before.position.y)),
                                                       static_cast<double>(std::abs(f.position.z - before.position.z)), static_cast<double>(std::abs(f.distance - before.distance)) });
                            before = f;
                        }
                        const auto under = room::flight(path, -3.0f, closeness, direction, side), start = room::flight(path, 0.0f, closeness, direction, side);
                        const auto over = room::flight(path, 4.0f, closeness, direction, side), end = room::flight(path, 1.0f, closeness, direction, side);
                        clamped = clamped && is(under, start.position.x, start.position.y, start.position.z) && is(over, end.position.x, end.position.y, end.position.z);
                    }
        check(inside && clamped, "every path stays inside the room, and progress outside 0..1 holds at the ends");
        check(sidesMirror, "Through and Overhead on the other side are the mirror image");
        check(worstJump < 0.004, "positions and distances move without jumps (largest step " + num(worstJump, 5) + " per 1/2000 of a pass)");
        check(distanceOk, "distance is the length of (x, y) on the floor, and of (x, y, the arch's real height) for Overhead");

        // Distance: nearest half way through for the three passes, nearer the closer the pass; a circle keeps its distance.
        for (const auto path : { Path::Across, Path::Through, Path::Overhead })
        {
            const double farMid = at(path, 0.5f, 0.0f).distance, closeMid = at(path, 0.5f, 1.0f).distance;
            bool nearestInMiddle = true;
            for (const float closeness : { 0.2f, 0.6f, 1.0f })
                for (int i = 0; i <= 100; ++i)
                    nearestInMiddle = nearestInMiddle && at(path, static_cast<float>(i) / 100.0f, closeness).distance >= at(path, 0.5f, closeness).distance - 1.0e-6f;
            check(nearestInMiddle && closeMid < 0.5 * farMid && at(path, 0.0f, 1.0f).distance > 2.0 * closeMid,
                  std::string(room::pathName(path)) + ": nearest half way through; " + num(farMid) + " away for a far pass, " + num(closeMid) + " for the closest");
        }
        {
            bool constant = true;
            for (int i = 0; i <= 100; ++i)
                constant = constant && near(at(Path::Around, static_cast<float>(i) / 100.0f, 0.4f).distance, 1.0 - 0.55 * 0.4, 1.0e-5);
            check(constant, "Around keeps one distance all the way round (0.78 at closeness 0.4)");
        }

        // Where the sound goes. Power per group of speakers along a pass.
        const auto groupPower = [](Width w, const room::Flight& f, const std::vector<S>& group)
        {
            room::Gains g;
            room::pan(w, f.position, { 0.0f, 0.0f, 1.0f }, g);
            double sum = 0.0;
            for (const S s : group)
                sum += static_cast<double>(gainOf(w, g, s)) * gainOf(w, g, s);
            return sum;
        };
        const std::vector<S> fronts { S::L, S::R, S::C }, rears { S::Ls, S::Rs, S::Lsr, S::Rsr }, sides { S::Lss, S::Rss };
        const std::vector<S> tops { S::Ltf, S::Rtf, S::Ltr, S::Rtr, S::Ltm, S::Rtm }, lefts { S::L, S::Ls, S::Lss, S::Lsr }, rights { S::R, S::Rs, S::Rss, S::Rsr };

        for (const Width w : { Width::Quad, Width::Surround51, Width::Surround71, Width::Surround712, Width::Surround714 })
        {
            // Across (a far pass): all in front, left before right.
            const bool across = near(groupPower(w, at(Path::Across, 0.0f), { S::L }), 1.0, 1.0e-5) && near(groupPower(w, at(Path::Across, 1.0f), { S::R }), 1.0, 1.0e-5)
                                && near(groupPower(w, at(Path::Across, 0.5f), fronts), 1.0, 1.0e-5) && groupPower(w, at(Path::Across, 0.2f), lefts) > groupPower(w, at(Path::Across, 0.2f), rights);
            // Through: fronts at the start, rears at the end, and the reverse the other way; on the side asked for.
            const bool through = near(groupPower(w, at(Path::Through, 0.0f), fronts), 1.0, 1.0e-5) && near(groupPower(w, at(Path::Through, 1.0f), rears), 1.0, 1.0e-5)
                                 && near(groupPower(w, at(Path::Through, 0.0f, 0.0f, -1.0f), rears), 1.0, 1.0e-5)
                                 && groupPower(w, at(Path::Through, 0.3f), rights) > 0.75 && groupPower(w, at(Path::Through, 0.3f, 0.0f, 1.0f, -1.0f), lefts) > 0.75;
            check(across && through, name(w) + ": Across plays L, then the front, then R; Through starts in the front speakers and ends in the rear ones, on its own side");
        }
        check(near(groupPower(Width::Surround71, at(Path::Through, 0.5f), sides), 1.0, 1.0e-5) && near(groupPower(Width::Surround712, at(Path::Through, 0.5f), sides), 1.0, 1.0e-5),
              "7.1: half way Through, the sound is in the side surrounds alone");

        for (const Width w : { Width::Surround712, Width::Surround714 })
        {
            double peak = 0.0;
            int peakAt = 0;
            for (int i = 0; i <= 100; ++i)
                if (const double p = groupPower(w, at(Path::Overhead, static_cast<float>(i) / 100.0f), tops); p > peak)
                {
                    peak = p;
                    peakAt = i;
                }
            check(peakAt == 50 && near(peak, 1.0, 1.0e-5) && groupPower(w, at(Path::Overhead, 0.0f), tops) < 1.0e-12 && groupPower(w, at(Path::Overhead, 1.0f), tops) < 1.0e-12
                      && near(groupPower(w, at(Path::Overhead, 0.0f), fronts), 1.0, 1.0e-5) && near(groupPower(w, at(Path::Overhead, 1.0f), rears), 1.0, 1.0e-5),
                  name(w) + ": Overhead starts in the front speakers, is in the tops alone overhead, and ends in the rear ones");
        }
        check(groupPower(Width::Surround714, at(Path::Overhead, 0.35f), { S::Ltf, S::Rtf }) > groupPower(Width::Surround714, at(Path::Overhead, 0.35f), { S::Ltr, S::Rtr })
                  && groupPower(Width::Surround714, at(Path::Overhead, 0.65f), { S::Ltr, S::Rtr }) > groupPower(Width::Surround714, at(Path::Overhead, 0.65f), { S::Ltf, S::Rtf }),
              "7.1.4: Overhead crosses the front tops before the rear tops");
        check(near(groupPower(Width::Surround51, at(Path::Overhead, 0.5f), tops), 0.0) && near(groupPower(Width::Surround51, at(Path::Overhead, 0.5f), fronts), 0.5, 1.0e-5),
              "5.1: Overhead has no tops to use, so overhead is shared by the front and the surrounds");

        // Around: every ear-level speaker (but the centre, at Centre 0) is the loudest at some point, in clockwise order.
        for (const Width w : { Width::Quad, Width::Surround51, Width::Surround71, Width::Surround714 })
        {
            const auto& l = room::layout(w);
            std::vector<S> visited;
            for (int i = 0; i < 400; ++i) // not the last point: it is the first again, and a tie between L and R
            {
                room::Gains g;
                room::pan(w, at(Path::Around, static_cast<float>(i) / 400.0f).position, {}, g);
                const auto loudest = static_cast<size_t>(std::max_element(g.begin(), g.begin() + l.numChannels) - g.begin());
                // On a tie (straight ahead: L and R) the first is taken; skip repeats.
                if (visited.empty() || visited.back() != l.speakers[loudest])
                    visited.push_back(l.speakers[loudest]);
            }
            std::string route;
            for (const S s : visited)
                route += std::string(room::info(s).name) + " ";
            const std::string want = w == Width::Surround71 || w == Width::Surround714 ? "L R Rss Rsr Lsr Lss L " : "L R Rs Ls L ";
            check(route == want, name(w) + ": Around is loudest in turn in " + route);
        }
    }

    //==================================================================================================================
    void testFolds()
    {
        std::cout << "Fold-downs" << std::endl;
        using room::LfeFold;
        const double h = 0.70710678;

        // A matrix as "out = gain in + gain in ..." lines, to compare with the documented ones.
        const auto describe = [](Width from, Width to, LfeFold lfe = LfeFold::Drop)
        {
            const auto& m = room::fold(from, to, lfe);
            const auto& in = room::layout(from);
            const auto& out = room::layout(to);
            std::string s;
            for (int r = 0; r < m.numOut; ++r)
            {
                s += std::string(room::info(out.speakers[static_cast<size_t>(r)]).name) + " =";
                for (int c = 0; c < m.numIn; ++c)
                    if (const float g = m.gain[static_cast<size_t>(r)][static_cast<size_t>(c)]; g != 0.0f)
                        s += " " + juce::String(g, 3).toStdString() + " " + room::info(in.speakers[static_cast<size_t>(c)]).name;
                s += "; ";
            }
            return s;
        };
        const auto pin = [&](Width from, Width to, const std::string& want, LfeFold lfe = LfeFold::Drop)
        {
            const auto got = describe(from, to, lfe);
            check(got == want, name(from) + " -> " + name(to) + (lfe == LfeFold::IntoFronts ? " (LFE into the fronts)" : "") + ": " + got);
            if (got != want)
                std::cout << "         wanted: " << want << std::endl;
        };

        // The documented steps.
        pin(Width::Stereo, Width::Mono, "C = 0.500 L 0.500 R; ");
        pin(Width::Quad, Width::Stereo, "L = 1.000 L 0.707 Ls; R = 1.000 R 0.707 Rs; ");
        pin(Width::Surround51, Width::Quad, "L = 1.000 L 0.707 C; R = 1.000 R 0.707 C; Ls = 1.000 Ls; Rs = 1.000 Rs; ");
        pin(Width::Surround51, Width::Stereo, "L = 1.000 L 0.707 C 0.707 Ls; R = 1.000 R 0.707 C 0.707 Rs; ");
        pin(Width::Surround71, Width::Surround51,
            "L = 1.000 L; R = 1.000 R; C = 1.000 C; LFE = 1.000 LFE; Ls = 0.707 Lss 0.707 Lsr; Rs = 0.707 Rss 0.707 Rsr; ");
        pin(Width::Surround712, Width::Surround71,
            "L = 1.000 L; R = 1.000 R; C = 1.000 C; LFE = 1.000 LFE; Lss = 1.000 Lss 0.707 Ltm; Rss = 1.000 Rss 0.707 Rtm; Lsr = 1.000 Lsr; Rsr = 1.000 Rsr; ");
        pin(Width::Surround714, Width::Surround712,
            "L = 1.000 L; R = 1.000 R; C = 1.000 C; LFE = 1.000 LFE; Lss = 1.000 Lss; Rss = 1.000 Rss; Lsr = 1.000 Lsr; Rsr = 1.000 Rsr; "
            "Ltm = 0.707 Ltf 0.707 Ltr; Rtm = 0.707 Rtf 0.707 Rtr; ");
        pin(Width::Surround714, Width::Surround71,
            "L = 1.000 L 0.707 Ltf; R = 1.000 R 0.707 Rtf; C = 1.000 C; LFE = 1.000 LFE; Lss = 1.000 Lss; Rss = 1.000 Rss; Lsr = 0.707 Ltr 1.000 Lsr; Rsr = 0.707 Rtr 1.000 Rsr; ");
        // Wider folds: the steps in a row.
        pin(Width::Surround714, Width::Surround51,
            "L = 1.000 L 0.707 Ltf; R = 1.000 R 0.707 Rtf; C = 1.000 C; LFE = 1.000 LFE; Ls = 0.707 Lss 0.500 Ltr 0.707 Lsr; Rs = 0.707 Rss 0.500 Rtr 0.707 Rsr; ");
        pin(Width::Surround714, Width::Stereo,
            "L = 1.000 L 0.707 C 0.500 Lss 0.707 Ltf 0.354 Ltr 0.500 Lsr; R = 1.000 R 0.707 C 0.500 Rss 0.707 Rtf 0.354 Rtr 0.500 Rsr; ");
        pin(Width::Surround712, Width::Stereo,
            "L = 1.000 L 0.707 C 0.500 Lss 0.500 Lsr 0.354 Ltm; R = 1.000 R 0.707 C 0.500 Rss 0.500 Rsr 0.354 Rtm; ");
        pin(Width::Surround51, Width::Mono, "C = 0.500 L 0.500 R 0.707 C 0.354 Ls 0.354 Rs; ");
        pin(Width::Surround51, Width::Stereo, "L = 1.000 L 0.707 C 0.707 LFE 0.707 Ls; R = 1.000 R 0.707 C 0.707 LFE 0.707 Rs; ", LfeFold::IntoFronts);
        pin(Width::Surround71, Width::Mono, "C = 0.500 L 0.500 R 0.707 C 0.707 LFE 0.250 Lss 0.250 Rss 0.250 Lsr 0.250 Rsr; ", LfeFold::IntoFronts);

        // Every pair: sizes, the identity, the LFE, mirror symmetry, and steps in a row give the same matrix.
        bool sizes = true, identity = true, lfeKept = true, lfeDropped = true, lfeFolded = true, mirror = true, noUpmix = true, exact = true, canOk = true;
        for (const Width from : c_widths)
            for (const Width to : c_widths)
            {
                const auto& in = room::layout(from);
                const auto& out = room::layout(to);
                const bool narrower = static_cast<int>(to) <= static_cast<int>(from);
                canOk = canOk && room::canFold(from, to) == narrower;
                const auto& m = room::fold(from, to);
                const auto& mf = room::fold(from, to, LfeFold::IntoFronts);
                if (! narrower)
                {
                    noUpmix = noUpmix && &m == &room::fold(from, from);
                    continue;
                }
                sizes = sizes && m.numIn == in.numChannels && m.numOut == out.numChannels && mf.numIn == m.numIn && mf.numOut == m.numOut;
                for (int r = 0; r < room::c_maxChannels; ++r)
                    for (int c = 0; c < room::c_maxChannels; ++c)
                    {
                        const float g = m.gain[static_cast<size_t>(r)][static_cast<size_t>(c)];
                        const float gf = mf.gain[static_cast<size_t>(r)][static_cast<size_t>(c)];
                        if (r >= m.numOut || c >= m.numIn)
                        {
                            sizes = sizes && g == 0.0f && gf == 0.0f;
                            continue;
                        }
                        if (from == to)
                            identity = identity && g == (r == c ? 1.0f : 0.0f) && gf == g;
                        // Every coefficient is a whole number of -3 dB steps (or the Mono average of two of them).
                        const double steps = std::log(std::max(1.0e-9, static_cast<double>(g))) / std::log(h);
                        exact = exact && (g == 0.0f || near(steps, std::round(steps), 1.0e-4) || to == Width::Mono);
                        const bool lfeIn = c == in.lfe, lfeOut = r == out.lfe;
                        if (lfeIn && lfeOut) lfeKept = lfeKept && g == 1.0f && gf == 1.0f;
                        if (lfeIn != lfeOut && out.lfe >= 0) lfeKept = lfeKept && g == 0.0f && gf == 0.0f; // nothing else reaches the LFE, the LFE reaches nothing else
                        if (lfeIn && out.lfe < 0)
                        {
                            lfeDropped = lfeDropped && g == 0.0f;
                            const bool front = to == Width::Mono || out.speakers[static_cast<size_t>(r)] == S::L || out.speakers[static_cast<size_t>(r)] == S::R;
                            lfeFolded = lfeFolded && near(gf, front ? h : 0.0, 1.0e-6);
                        }
                        if (! lfeIn)
                            lfeFolded = lfeFolded && gf == g; // the option changes nothing but the LFE's own column
                        const int mr = out.indexOf(room::mirrored(out.speakers[static_cast<size_t>(r)]));
                        const int mc = in.indexOf(room::mirrored(in.speakers[static_cast<size_t>(c)]));
                        mirror = mirror && near(g, m.gain[static_cast<size_t>(mr)][static_cast<size_t>(mc)], 1.0e-7);
                    }
            }
        check(canOk && sizes && noUpmix, "every width folds to itself and to every narrower one, with the right channel counts; there is no up-mix");
        check(identity, "a width folded to itself is unchanged");
        check(exact && mirror, "every coefficient is a whole number of -3 dB steps, and left and right fold alike");
        check(lfeKept && lfeDropped, "the LFE stays in the LFE where there is one, takes nothing in, and is dropped where there is none");
        check(lfeFolded, "LFE into the fronts: -3 dB into L and R (0.707 into Mono), and nothing else changes");

        // Steps in a row: folding by way of any width in between gives the same matrix, as long as the tops go
        // straight to the bed (by way of 7.1.2 they would land in the sides instead, so that way is left out).
        double worstChain = 0.0;
        for (const Width from : c_widths)
            for (const Width via : c_widths)
                for (const Width to : c_widths)
                {
                    if (! (static_cast<int>(to) < static_cast<int>(via) && static_cast<int>(via) < static_cast<int>(from)))
                        continue;
                    if (from == Width::Surround714 && via == Width::Surround712)
                        continue;
                    const auto& a = room::fold(from, via);
                    const auto& b = room::fold(via, to);
                    const auto& direct = room::fold(from, to);
                    for (size_t r = 0; r < static_cast<size_t>(direct.numOut); ++r)
                        for (size_t c = 0; c < static_cast<size_t>(direct.numIn); ++c)
                        {
                            double sum = 0.0;
                            for (size_t k = 0; k < static_cast<size_t>(a.numOut); ++k)
                                sum += static_cast<double>(b.gain[r][k]) * a.gain[k][c];
                            worstChain = std::max(worstChain, std::abs(sum - direct.gain[r][c]));
                        }
                }
        check(worstChain < 1.0e-6, "a fold by way of a width in between is the same as the fold straight there (worst difference " + num(worstChain, 8) + ")");

        // applyFold does what the matrix says.
        {
            const auto& m = room::fold(Width::Surround714, Width::Surround51);
            constexpr int n = 64;
            std::array<std::array<float, n>, 12> in {};
            std::array<std::array<float, n>, 6> out {};
            std::array<const float*, 12> inPtr {};
            std::array<float*, 6> outPtr {};
            for (size_t c = 0; c < 12; ++c)
            {
                for (size_t i = 0; i < n; ++i)
                    in[c][i] = std::sin(0.1f * static_cast<float>(i * (c + 1)) + static_cast<float>(c));
                inPtr[c] = in[c].data();
            }
            for (size_t r = 0; r < 6; ++r)
            {
                out[r].fill(123.0f); // must be overwritten, not added to
                outPtr[r] = out[r].data();
            }
            room::applyFold(m, inPtr.data(), outPtr.data(), n);
            double worst = 0.0;
            for (size_t r = 0; r < 6; ++r)
                for (size_t i = 0; i < n; ++i)
                {
                    double want = 0.0;
                    for (size_t c = 0; c < 12; ++c)
                        want += static_cast<double>(m.gain[r][c]) * in[c][i];
                    worst = std::max(worst, std::abs(want - out[r][i]));
                }
            check(worst < 1.0e-5, "applyFold writes matrix times input (7.1.4 to 5.1 on twelve different signals, worst error " + num(worst, 8) + ")");
        }

        // What a fold does to one full-scale sound placed anywhere by the panner: the highest any folded channel
        // goes, and the most power it gains or loses, over every position, Centre and Spread.
        double maxPeakDb = -100.0, maxPowerDb = -100.0, minPowerDb = 100.0, monoMinPowerDb = 100.0, worstRowSum = 0.0;
        std::string peakWhere, lossWhere, monoLossWhere, gainWhere;
        for (const Width from : c_widths)
            for (const Width to : c_widths)
            {
                if (static_cast<int>(to) >= static_cast<int>(from))
                    continue;
                const auto& m = room::fold(from, to);
                worstRowSum = std::max(worstRowSum, static_cast<double>(room::foldWorstCaseGain(m)));
                double pairPeak = -100.0, pairMax = -100.0, pairMin = 100.0;
                for (const float spread : { 0.0f, 0.5f, 1.0f })
                    for (const float centre : { 0.0f, 0.5f, 1.0f })
                        for (int ix = -16; ix <= 16; ++ix)
                            for (int iy = -16; iy <= 16; ++iy)
                                for (int iz = 0; iz <= 8; ++iz)
                                {
                                    const auto g = panned(from, static_cast<float>(ix) / 16.0f, static_cast<float>(iy) / 16.0f, static_cast<float>(iz) / 8.0f, spread, centre);
                                    double pw = 0.0, peak = 0.0;
                                    for (size_t r = 0; r < static_cast<size_t>(m.numOut); ++r)
                                    {
                                        double v = 0.0;
                                        for (size_t c = 0; c < static_cast<size_t>(m.numIn); ++c)
                                            v += static_cast<double>(m.gain[r][c]) * g[c];
                                        pw += v * v;
                                        peak = std::max(peak, std::abs(v));
                                    }
                                    pairPeak = std::max(pairPeak, toDb(peak));
                                    pairMax = std::max(pairMax, powerDb(pw));
                                    pairMin = std::min(pairMin, powerDb(pw));
                                }
                std::cout << "         " << name(from) << " -> " << name(to) << ": peak " << num(pairPeak, 2) << " dB, power " << num(pairMin, 2) << " .. " << num(pairMax, 2)
                          << " dB, unrelated full-scale channels up to " << num(toDb(room::foldWorstCaseGain(m)), 1) << " dB" << std::endl;
                if (pairPeak > maxPeakDb) { maxPeakDb = pairPeak; peakWhere = name(from) + " -> " + name(to); }
                if (pairMax > maxPowerDb) { maxPowerDb = pairMax; gainWhere = name(from) + " -> " + name(to); }
                if (to != Width::Mono && pairMin < minPowerDb) { minPowerDb = pairMin; lossWhere = name(from) + " -> " + name(to); }
                if (to == Width::Mono && pairMin < monoMinPowerDb) { monoMinPowerDb = pairMin; monoLossWhere = name(from) + " -> " + name(to); }
            }
        check(maxPeakDb <= room::c_foldMaxPeakDb && maxPeakDb > room::c_foldMaxPeakDb - 0.5,
              "a sound placed anywhere folds to at most " + num(maxPeakDb, 2) + " dB over full scale in any channel (" + peakWhere + "; documented " + num(room::c_foldMaxPeakDb, 1) + ")");
        check(maxPowerDb <= room::c_foldMaxPowerDb && maxPowerDb > room::c_foldMaxPowerDb - 0.5,
              "... gains at most " + num(maxPowerDb, 2) + " dB of power (" + gainWhere + "; documented " + num(room::c_foldMaxPowerDb, 1) + ")");
        check(minPowerDb >= room::c_foldMinPowerDb && minPowerDb < room::c_foldMinPowerDb + 0.5,
              "... and to Stereo or wider loses at most " + num(-minPowerDb, 2) + " dB (" + lossWhere + "; documented " + num(-room::c_foldMinPowerDb, 1) + ")");
        check(monoMinPowerDb >= room::c_foldMonoMinPowerDb && monoMinPowerDb < room::c_foldMonoMinPowerDb + 0.5,
              "... and to Mono at most " + num(-monoMinPowerDb, 2) + " dB (" + monoLossWhere + "; documented " + num(-room::c_foldMonoMinPowerDb, 1) + ")");
        check(near(room::foldWorstCaseGain(room::fold(Width::Surround51, Width::Stereo)), 1.0 + 2.0 * h, 1.0e-5) && near(room::foldWorstCaseGain(room::fold(Width::Stereo, Width::Mono)), 1.0)
                  && near(room::foldWorstCaseGain(room::fold(Width::Surround71, Width::Surround71)), 1.0),
              "the worst case for unrelated channels is the largest row sum: 2.414 for 5.1 to Stereo, 1 for Stereo to Mono (the most of all: " + num(toDb(worstRowSum), 1) + " dB)");
    }

    //==================================================================================================================
    void testFileOrders()
    {
        std::cout << "File channel orders" << std::endl;
        using room::FileOrder;

        check(std::string(room::fileOrderName(FileOrder::ProTools)) == "Pro Tools (Film)" && std::string(room::fileOrderName(FileOrder::Smpte)) == "SMPTE / WAV",
              "the two orders are named Pro Tools (Film) and SMPTE / WAV");

        struct Expect { Width width; const char* film; const char* smpte; std::uint32_t mask; std::vector<int> filmChannels, smpteChannels; const char* suffixes; };
        const std::vector<Expect> expected {
            { Width::Mono,        "C", "C", 0x4, { 0 }, { 0 }, "" },
            { Width::Stereo,      "L R", "L R", 0x3, { 0, 1 }, { 0, 1 }, ".L .R" },
            { Width::Quad,        "L R Ls Rs", "L R Ls Rs", 0x33, { 0, 1, 2, 3 }, { 0, 1, 2, 3 }, ".L .R .Ls .Rs" },
            { Width::Surround51,  "L C R Ls Rs LFE", "L R C LFE Ls Rs", 0x3F, { 0, 2, 1, 4, 5, 3 }, { 0, 1, 2, 3, 4, 5 }, ".L .R .C .LFE .Ls .Rs" },
            { Width::Surround71,  "L C R Lss Rss Lsr Rsr LFE", "L R C LFE Lsr Rsr Lss Rss", 0x63F,
              { 0, 2, 1, 4, 5, 6, 7, 3 }, { 0, 1, 2, 3, 6, 7, 4, 5 }, ".L .R .C .LFE .Lss .Rss .Lsr .Rsr" },
            { Width::Surround712, "L C R Lss Rss Lsr Rsr LFE Ltm Rtm", "L R C LFE Lsr Rsr Lss Rss Ltm Rtm", 0x563F,
              { 0, 2, 1, 4, 5, 6, 7, 3, 8, 9 }, { 0, 1, 2, 3, 6, 7, 4, 5, 8, 9 }, ".L .R .C .LFE .Lss .Rss .Lsr .Rsr .Ltm .Rtm" },
            { Width::Surround714, "L C R Lss Rss Lsr Rsr LFE Ltf Rtf Ltr Rtr", "L R C LFE Lsr Rsr Lss Rss Ltf Rtf Ltr Rtr", 0x2D63F,
              { 0, 2, 1, 4, 5, 10, 11, 3, 6, 7, 8, 9 }, { 0, 1, 2, 3, 10, 11, 4, 5, 6, 7, 8, 9 }, ".L .R .C .LFE .Lss .Rss .Ltf .Rtf .Ltr .Rtr .Lsr .Rsr" },
        };

        // The order of a Pro Tools track, as JUCE's AAX wrapper has it (juce_audio_plugin_client_AAX.cpp).
        const std::vector<std::vector<Type>> aax {
            { Type::centre },
            { Type::left, Type::right },
            { Type::left, Type::right, Type::leftSurround, Type::rightSurround },
            { Type::left, Type::centre, Type::right, Type::leftSurround, Type::rightSurround, Type::LFE },
            { Type::left, Type::centre, Type::right, Type::leftSurroundSide, Type::rightSurroundSide, Type::leftSurroundRear, Type::rightSurroundRear, Type::LFE },
            { Type::left, Type::centre, Type::right, Type::leftSurroundSide, Type::rightSurroundSide, Type::leftSurroundRear, Type::rightSurroundRear, Type::LFE,
              Type::topSideLeft, Type::topSideRight },
            { Type::left, Type::centre, Type::right, Type::leftSurroundSide, Type::rightSurroundSide, Type::leftSurroundRear, Type::rightSurroundRear, Type::LFE,
              Type::topFrontLeft, Type::topFrontRight, Type::topRearLeft, Type::topRearRight },
        };
        // The bit each speaker has in a WAV speaker mask (written out here on its own, from the WAVEFORMATEXTENSIBLE list).
        const auto wavBit = [](S s) -> std::uint32_t
        {
            switch (s)
            {
                case S::L: return 0x1;       case S::R: return 0x2;       case S::C: return 0x4;      case S::Lfe: return 0x8;
                case S::Ls: case S::Lsr: return 0x10;                     case S::Rs: case S::Rsr: return 0x20;
                case S::Lss: return 0x200;   case S::Rss: return 0x400;
                case S::Ltf: case S::Ltm: return 0x1000;                  case S::Rtf: case S::Rtm: return 0x4000;
                case S::Ltr: return 0x8000;  case S::Rtr: return 0x20000;
            }
            return 0;
        };

        size_t row = 0;
        for (const auto& e : expected)
        {
            const auto& l = room::layout(e.width);
            const std::string film = room::orderLabel(e.width, FileOrder::ProTools), smpte = room::orderLabel(e.width, FileOrder::Smpte);
            check(film == e.film && smpte == e.smpte, name(e.width) + ": Pro Tools (Film) \"" + film + "\", SMPTE / WAV \"" + smpte + "\"");

            const auto& filmCh = room::fileChannels(e.width, FileOrder::ProTools);
            const auto& smpteCh = room::fileChannels(e.width, FileOrder::Smpte);
            bool channelsOk = true, permutation = true, aaxOk = true, ascending = true;
            std::uint32_t seenFilm = 0, seenSmpte = 0, lastBit = 0, maskFromBits = 0;
            const auto set = room::channelSet(e.width);
            for (int i = 0; i < l.numChannels; ++i)
            {
                const auto at = static_cast<size_t>(i);
                channelsOk = channelsOk && filmCh[at] == e.filmChannels[at] && smpteCh[at] == e.smpteChannels[at];
                permutation = permutation && filmCh[at] >= 0 && filmCh[at] < l.numChannels && smpteCh[at] >= 0 && smpteCh[at] < l.numChannels;
                seenFilm |= 1u << filmCh[at];
                seenSmpte |= 1u << smpteCh[at];
                aaxOk = aaxOk && set.getTypeOfChannel(filmCh[at]) == aax[row][at];
                const std::uint32_t bit = wavBit(l.speakers[static_cast<size_t>(smpteCh[at])]);
                ascending = ascending && bit > lastBit && bit == room::info(l.speakers[static_cast<size_t>(smpteCh[at])]).wavBit;
                lastBit = bit;
                maskFromBits |= bit;
            }
            permutation = permutation && seenFilm == (1u << l.numChannels) - 1u && seenSmpte == seenFilm;
            check(channelsOk && permutation, name(e.width) + ": each file channel takes the right processing channel, and every channel is written once");
            check(aaxOk && aax[row].size() == static_cast<size_t>(l.numChannels), name(e.width) + ": the Pro Tools (Film) order is the order of a Pro Tools track");

            const auto mask = room::wavChannelMask(e.width);
            int bits = 0;
            for (std::uint32_t b = mask; b != 0; b >>= 1)
                bits += static_cast<int>(b & 1u);
            check(mask == e.mask && bits == l.numChannels && ascending && maskFromBits == mask,
                  name(e.width) + ": WAV speaker mask 0x" + juce::String::toHexString(static_cast<int>(mask)).toUpperCase().toStdString()
                      + ", one bit per channel, and the SMPTE / WAV order is the order of those bits");

            std::string suffixes;
            for (int c = 0; c < l.numChannels; ++c)
                suffixes += std::string(c > 0 && e.width != Width::Mono ? " " : "") + room::multiMonoSuffix(e.width, c);
            check(suffixes == e.suffixes && std::string(room::multiMonoSuffix(e.width, -1)).empty() && std::string(room::multiMonoSuffix(e.width, l.numChannels)).empty(),
                  name(e.width) + ": Multi-Mono suffixes \"" + suffixes + "\"");
            ++row;
        }

        // JUCE's WAV writer agrees on the masks it can express (its bit is the channel type minus one): a 5.1 set
        // and the "back pair plus side pair" set that stands for film 7.1 in a WAV file.
        const auto juceMask = [](const juce::AudioChannelSet& set)
        {
            std::uint32_t mask = 0;
            for (int c = 0; c < set.size(); ++c)
                mask |= 1u << (static_cast<int>(set.getTypeOfChannel(c)) - 1);
            return mask;
        };
        juce::AudioChannelSet wav71 = juce::AudioChannelSet::create5point1();
        wav71.addChannel(Type::leftSurroundSide);
        wav71.addChannel(Type::rightSurroundSide);
        juce::AudioChannelSet wav714 = wav71;
        for (const auto t : { Type::topFrontLeft, Type::topFrontRight, Type::topRearLeft, Type::topRearRight })
            wav714.addChannel(t);
        check(juceMask(juce::AudioChannelSet::quadraphonic()) == room::wavChannelMask(Width::Quad) && juceMask(juce::AudioChannelSet::create5point1()) == room::wavChannelMask(Width::Surround51)
                  && juceMask(wav71) == room::wavChannelMask(Width::Surround71) && juceMask(wav714) == room::wavChannelMask(Width::Surround714)
                  && juceMask(juce::AudioChannelSet::create7point1SDDS()) != room::wavChannelMask(Width::Surround71),
              "the masks are the ones JUCE's WAV writer gives Quad, 5.1 and the back-plus-side 7.1 and 7.1.4 sets (and 7.1 is not the old five-front 0xFF)");
    }
}

int main()
{
    testLayouts();
    testPanner();
    testFlightPaths();
    testFolds();
    testFileOrders();

    std::cout << (g_failures == 0 ? "ALL PASSED" : std::to_string(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

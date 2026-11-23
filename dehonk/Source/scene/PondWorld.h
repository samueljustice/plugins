// What is in De-Honk's picture and how it behaves. Plain C++ on top of Raster.h, no JUCE, so it
// can be drawn and tested without a window.
//
// The picture: a pond on a strange, mild day, seen from the bank, in flat off-beat pastels: a peach
// sky with a sunburst and a rainbow, mint water, a pale lawn, and a little stream running along the
// front. Everything is made of a few simple rounded shapes. The frequency axis runs along the
// water's edge, low on the left. The reeds at the edge are scenery: they sway a little in the wind
// and part where a goose comes through.
//
// Who is in it, and what each one shows:
//
//   Geese        a notch. One lands on the bank wherever De-Honk has a notch open and honks; the
//                deeper the notch, the bigger the goose. It waddles after its notch and flies off
//                when the notch closes.
//   Recordist    De-Honk at work. He walks along the bank to the worst goose, facing the way he
//                goes, and holds the microphone out over its head, never quite still. Between the
//                two of them there is one bit at a time, with a rest after it that is shorter the
//                busier the mix: he shoos it, swings and misses, it takes hold of the windshield,
//                he lunges and it hops over the pole. How it ends is the plug-in's doing: if the
//                notch closes he sends it off with one great swing; if the cut stays near Max Cut
//                it walks up his pole and sits on his hat. With nothing to do he puts the pole on
//                his shoulder and finds small things to see to. Bypass: he sits down with a
//                flask. Solo Honk: he cups a headphone and listens. Diff: he stops
//                and looks from side to side, comparing.
//   Trees        the edges of the ranges: one stands on the far shore at each edge, in the colour
//                of the range to its right, and moves as the edge is dragged.
//   Fish         Snap: one leaps out of the pond, with a twist, when a click is caught.
//   Beaver       the Wash reading: while the low mids stay clogged it builds a dam across the
//                stream, and the dam comes apart again as they clear. With nothing to build, it
//                floats on its back.
//   Turtles      Impact Guard: they pop into their shells while the guard is holding for a hit, and
//                peek out again one at a time afterwards. With Impact Guard off they doze.
//   Frog         Max Cut: its lily pad drifts along the stream to sit below the deepest goose; the
//                closer that notch comes to the ceiling the tenser the frog, and it leaps into the
//                water whenever a notch reaches it.
//   Dragonflies  the sidechain: they are about only while the key is driving the threshold.
//
// The weather follows how honky the mix has been over the last while: clear and sunny when there
// is little to do, clouding over, then wind and rain when De-Honk has been kept busy. It changes
// over tens of seconds, never from moment to moment.
//
// Calm comes first. Nothing here follows the meters directly: every reading is filtered or turned
// into an event with its own timing, and everything moves by the seconds that have passed, so it
// looks the same at any frame rate. Small business (a stretch, a preen, a pat, a breeze through the
// reeds) is handed out by one director, one piece at a time with seconds between, and never while
// something bigger is going on. With a range in focus, only the creatures of that range move; the
// rest hold still, a little dimmed.
//
// Everyone can be clicked (see hitTest and click). What a click sets off is for fun only and
// changes nothing the mix drives.
#pragma once

#include "Raster.h"
#include "../../dsp/DeHonkCore.h"

#include <array>

namespace dehonk::scene
{

struct WorldInput
{
    DisplayFrame frame;
    bool moduleOn[kNumModules] {};
    bool snapOn = false;
    float edgeHz[kNumEdges] {};
    float maxCutDb = 6.0f;             // the Max Cut control
    float guardAmount = 1.0f;          // the Impact Guard control, 0..1 (0 = off)
    bool soloHonk = false, diff = false, bypass = false, calm = false;
    int focus = -1;                    // the range in focus: 0..3 Woof, Tub, Fog, Bark; 4 Snap; -1 none
    float axisLo = 0.0f, axisHi = 1.0f; // where the frequency axis starts and ends across the picture (0..1): the band
                                       //   view's plot under the pond does not reach its card's edges, and the trees
                                       //   and posts stand over its handles
};

class PondWorld
{
public:
    static constexpr float kLowHz = 40.0f, kOctaves = 7.0f;       // the axis: 40 Hz to 5.12 kHz
    static constexpr float kHalfWidth = 8.0f;                      // world units either side of centre
    static constexpr float kBankRow = 0.74f;                       // the water's edge, as a share of the height

    /** The axis itself, 0..1 from 40 Hz to 5.12 kHz, and back. */
    static float axis01ForHz (float hz) noexcept { return std::log2 (std::clamp (hz, kLowHz, kLowHz * 128.0f) / kLowHz) / kOctaves; }
    static float hzForAxis01 (float x) noexcept { return kLowHz * std::pow (2.0f, kOctaves * std::clamp (x, 0.0f, 1.0f)); }
    /** Where a frequency is across the picture (0..1), with the axis laid where the host says (WorldInput::axisLo, axisHi). */
    float x01ForHz (float hz) const noexcept { return input.axisLo + (input.axisHi - input.axisLo) * axis01ForHz (hz); }
    float worldX (float hz) const noexcept { return (x01ForHz (hz) - 0.5f) * 2.0f * kHalfWidth; }

    /** `pixelsPerPoint`: how many of the picture's pixels make one point of the window it is shown
        in (2 for a picture drawn at the full resolution of a display with two pixels to the
        point). The few things that are sized in pixels, not in the world, are scaled by it. */
    void setSize (int w, int h, float pixelsPerPoint = 1.0f)
    {
        pp = std::clamp (pixelsPerPoint, 0.5f, 4.0f);
        r.setSize (w, h);
        backdropDue = 0.0f;
        // In a very wide picture there is little height above the bank: shrink the cast to fit.
        cast = std::clamp (0.2f * (float) h / ((float) w / (2.0f * kHalfWidth)), 0.45f, 1.0f);
        // The bank's edge (z = 0) must span the picture exactly and sit on kBankRow.
        // With the focal length set from the first condition, the row of the edge depends only on
        // how far the camera is above it: solve for the height that puts it on kBankRow.
        // The camera looks down just enough to put the horizon about three tenths of the way down
        // the picture, whatever its shape, so there is always sky for the sun and the rainbow.
        const float camZ = -11.0f;
        const float pitch = std::atan (0.29f * (float) h / (float) w);
        const float drop = (kBankRow - 0.5f) * (float) h * kHalfWidth / ((float) w * 0.5f);
        const float camY = (drop - camZ * std::sin (pitch)) / std::cos (pitch);
        const float viewDepth = camY * std::sin (pitch) - camZ * std::cos (pitch);
        const float focal = (float) w * 0.5f * viewDepth / kHalfWidth;
        r.setCamera ({ 0.0f, camY, camZ }, pitch, focal);

        // The stream along the bottom of the picture, where the small creatures live. It is placed
        // by rows of the picture, so that it is the same strip whatever the picture's shape.
        float wz = -3.1f, vz = 8.0f;
        if (r.groundAtRow (0.865f * (float) h, wz, vz))
            streamFar = wz;
        if (r.groundAtRow (0.94f * (float) h, wz, vz))
        {
            streamZ = wz;
            const float px = focal / vz;                                   // pixels per unit at the stream
            streamRatio = ((float) w / (2.0f * kHalfWidth)) / px;          // the same place across the picture: x here = x on the bank * this
            fg = 1.35f * std::clamp (0.16f * (float) h / px / 0.62f, 0.55f, 1.1f) * std::clamp (cast * 1.25f, 0.6f, 1.0f);
        }
        const float oldStage = stageHalf;
        stageHalf = kHalfWidth * streamRatio;
        // The turtles' log at the left, the beaver's dam at the right, and the frog's pad free to
        // drift in between, where the geese most often are. Both are placed in from the edges by
        // their own size, so that they stay whole whatever the picture's shape.
        logX = -stageHalf + (kLogHalf + 0.3f) * fg + 0.2f * fg;
        damX = stageHalf - (damHalf (1.0f) + 0.45f) * fg - 0.25f * fg;
        if (logX > -0.25f * stageHalf) logX = -0.25f * stageHalf;
        if (damX < 0.25f * stageHalf) damX = 0.25f * stageHalf;

        // Geese standing behind the turtles' log keep to the back of the bank, and the log rides
        // low enough in the stream that the tops of the log and the shells stay clear below their
        // feet in the picture, with grass between: otherwise they would seem to stand on the shells.
        // Worked out in the picture, for the front one of those two rows.
        {
            const float k = fg;
            const float shellTop = 0.24f * k + 0.42f * 0.682f * k * 1.12f;               // the biggest turtle's shell, unsunk
            const float row = r.project ({ logX, shellTop, streamZ }).y;
            const float pxPerUnit = std::max (1.0f, r.project ({ logX, 0.0f, streamZ }).y - r.project ({ logX, 1.0f, streamZ }).y);
            const float clear = r.rowOfGroundZ (kLogGooseFrontZ) + std::max (5.0f * pp, 0.03f * (float) h);
            logSink = std::clamp ((clear - row) / pxPerUnit, 0.0f, 0.18f * k);
            const float rad = 0.2f * k;
            const Vec3 a { logX - kLogHalf * k, 0.0f, streamZ - 0.05f * k }, b { logX + kLogHalf * k, 0.0f, streamZ + 0.1f * k };
            logLeftPx = r.project (a - Vec3 { rad, 0.0f, 0.0f }).x;
            logRightPx = r.project (b + Vec3 { rad, 0.0f, 0.0f }).x;
        }

        // The recordist has his own strip of bank at the back, by the water's edge, behind the
        // geese: they stand in front of him and he works them from behind with the pole held out
        // over them. Across the picture he keeps to the stretch between the turtles' log and the
        // beaver: never above either of them, standing or running, whatever the geese do. The
        // stretch is worked out in the picture, so it holds at any size and shape.
        {
            const float o = r.project ({ 0.0f, 0.0f, kKeeperZ }).x;
            const float pk = r.project ({ 1.0f, 0.0f, kKeeperZ }).x - o;
            const float ps = r.project ({ 1.0f, 0.0f, streamZ }).x - r.project ({ 0.0f, 0.0f, streamZ }).x;
            keeperToStream = ps > 1.0e-3f ? pk / ps : 0.8f;
            auto keeperXAt = [&] (float px) { return pk > 1.0e-3f ? (px - o) / pk : 0.0f; };
            const float halfBody = 0.85f * cast;                                     // shoulders, arms and a little air
            const float logRight = r.project ({ logX + (kLogHalf + 0.3f) * fg, 0.0f, streamZ }).x;
            const float beaverLeft = r.project ({ damX - (damHalf (1.0f) + 1.45f) * fg, 0.0f, streamZ }).x;
            keeperLo = std::max (keeperXAt (logRight) + halfBody, keeperXAt (0.0f) + 1.0f);
            keeperHi = std::min (keeperXAt (beaverLeft) - halfBody, keeperXAt ((float) w) - 1.0f);
            if (keeperLo > keeperHi) keeperLo = keeperHi = 0.5f * (keeperLo + keeperHi);
            sitLo = keeperLo + 0.3f; sitHi = keeperHi - 0.3f;
            if (sitLo > sitHi) sitLo = sitHi = 0.5f * (sitLo + sitHi);
        }

        if (! placed || time <= 0.0f)
        {
            // Before the scene has run, place everyone afresh for this size.
            placed = true;
            frog.x = std::clamp (-0.3f * stageHalf, logX + (kLogHalf + 0.6f) * fg, std::max (logX + (kLogHalf + 0.6f) * fg, damX - (damHalf (1.0f) + 1.75f) * fg));
            beaver.x = damX - (damHalf (dam) + 1.0f) * fg;
            keeper.x = std::clamp (keeper.x, keeperLo, keeperHi);
            keeper.cableX = keeper.x - keeper.facing * 1.3f;
        }
        else if (oldStage > 0.01f)
        {
            // A new size later on: everyone in the stream keeps their place in the picture.
            const float s = stageHalf / oldStage;
            frog.x *= s; frog.landX *= s; beaver.x *= s;
            keeper.x = std::clamp (keeper.x, keeperLo, keeperHi);
        }
    }

    //==============================================================================
    void advance (const WorldInput& in, float dt)
    {
        dt = std::clamp (dt, 0.0f, 0.25f);
        input = in;
        input.axisLo = std::clamp (in.axisLo, 0.0f, 0.4f);
        input.axisHi = std::clamp (in.axisHi, 0.6f, 1.0f);
        if (input.axisLo < axisWas[0] || input.axisLo > axisWas[0] || input.axisHi < axisWas[1] || input.axisHi > axisWas[1])
        {
            axisWas[0] = input.axisLo; axisWas[1] = input.axisHi;
            backdropDue = 0.0f;
        }

        // How honky has it been? The deepest notch and how many are open, averaged slowly: the
        // sky takes about half a minute to cloud over and rain, and twice that to clear. Ordinary
        // work (a few notches a few dB deep) leaves the sky clear; it takes cuts that stay near
        // the ceiling in several places at once to bring the rain. With nothing playing it clears
        // quickly. (This runs on real time even when the picture is being held still.)
        {
            int open = 0;
            for (int k = 0; k < in.frame.numBands; ++k)
                open += in.frame.cutDb[k] > 1.0f ? 1 : 0;
            backdropDue -= dt;
            const bool quiet = in.frame.activity <= 0.0f && in.frame.totalCutDb <= 0.05f;
            silence = quiet ? silence + dt : 0.0f;
            const float now = std::clamp (0.65f * (in.frame.totalCutDb - 2.0f) / 8.0f, 0.0f, 0.65f) + std::clamp (0.35f * ((float) open - 2.0f) / 8.0f, 0.0f, 0.35f);
            honkiness += (now - honkiness) * std::min (1.0f, dt / (now > honkiness ? 20.0f : silence > 2.0f ? 6.0f : 40.0f));
            if (! std::isfinite (honkiness)) honkiness = 0.0f;          // never let one bad frame stick
            const float target = smooth01 ((honkiness - 0.2f) / 0.6f);
            weather += (target - weather) * std::min (1.0f, dt / (silence > 2.0f ? 3.0f : 6.0f));
            if (! std::isfinite (weather)) weather = 0.0f;
            windPhase += dt * (1.3f + 2.2f * weather);
            rainPhase += dt;
        }

        if (in.calm)
            dt = 0.0f;                         // the picture holds still; positions still follow below

        // Long steps (a slow host, or the idle rate) are taken in pieces, so that springs and
        // short events behave the same at any frame rate.
        // Was the pond still last time? Then small idle business waits for the shared idle timer
        // (see step), so that a still pond is not kept at the full rate by blinks and twitches.
        stillPrev = ! moving;
        moving = fussing = false;
        const int pieces = std::max (1, (int) std::ceil (dt / 0.034f));
        for (int i = 0; i < pieces; ++i)
            step (in, dt / (float) pieces);
    }

    bool isMoving() const noexcept { return ((moving || fussing) && ! input.calm) || clicked != Hot::none; }
    /** Nothing is moving but the rain is falling: a slower rate will do for that. */
    bool isRaining() const noexcept { return weather > 0.57f; }

    //============================================================================== the mouse
    enum class Hot { none, turtle, beaver, frog, goose, keeper, fly, resident };

    /** Who is under a point of the picture (in its own pixels), as last drawn. */
    Hot hitTest (float px, float py, int* index = nullptr) const noexcept { return pick (hots.data(), hotCount, px, py, index); }

    /** The same, among a copy of where everyone was (anything with x, y, rx, ry, kind and index:
        see PondScene::Hits), so that it can be asked on another thread than the one drawing. */
    template <typename Spot>
    static Hot pick (const Spot* spots, int count, float px, float py, int* index = nullptr) noexcept
    {
        // Nearest the front first; a dragonfly that is passing a goose leaves the goose to be clicked.
        static constexpr Hot order[] = { Hot::frog, Hot::turtle, Hot::beaver, Hot::goose, Hot::fly, Hot::keeper, Hot::resident };
        for (const Hot kind : order)
        {
            int best = -1;
            float bestD = 1.0f;
            for (int i = 0; i < count; ++i)
            {
                const auto& h = spots[i];
                if ((int) h.kind != (int) kind)
                    continue;
                const float dx = (px - h.x) / h.rx, dy = (py - h.y) / h.ry, d = dx * dx + dy * dy;
                if (d < bestD) { bestD = d; best = i; }
            }
            if (best >= 0)
            {
                if (index != nullptr)
                    *index = spots[best].index;
                return kind;
            }
        }
        return Hot::none;
    }
    /** Where the mouse can find someone, as last drawn (see hitTest). */
    int hotSpotCount() const noexcept { return hotCount; }
    void hotSpot (int i, float& x, float& y, float& rx, float& ry, int& kind, int& index) const noexcept
    {
        const auto& h = hots[(size_t) i];
        x = h.x; y = h.y; rx = h.rx; ry = h.ry; kind = (int) h.kind; index = h.index;
    }

    /** A click on the picture. Returns true if someone was there to take it. What they do starts at
        the next advance(), is for show only, and changes nothing the mix drives: a turtle topples
        off the log and climbs back, the beaver waves (or slaps its tail), the frog leaps off its
        pad, a goose is knocked along the bank in a puff of feathers and waddles back, the recordist
        tips his hat or waves, and the dragonflies scatter. */
    bool click (float px, float py) noexcept
    {
        int index = 0;
        const Hot kind = hitTest (px, py, &index);
        if (kind == Hot::none)
            return false;
        clicked = kind; clickedIndex = index;
        return true;
    }

    //============================================================================== the trees
    /** The crown of the tree at each range edge, as drawn: its centre and radius in the picture's
        pixels (the centre may be above the top edge, y < 0) and the colour of its top. The host
        draws the part that rises above the picture. */
    struct TreeTop { float x = 0, y = 0, radius = 0; std::uint8_t r = 0, g = 0, b = 0; };
    const std::array<TreeTop, kNumEdges>& treeTops() const noexcept { return tops; }

    /** The colours of the four ranges and of Snap, so that the trees, the strips along the bank and
        the posts match the tabs in the window. */
    void setModuleColours (const Rgb* five) noexcept
    {
        for (int i = 0; i <= kNumModules; ++i)
            moduleCol[i] = five[i];
        backdropDue = 0.0f;
    }

    /** 0 = clear and sunny, 1 = wind and rain. For the tests and for anything that wants to show it. */
    float weatherNow() const noexcept { return weather; }
    void debugSetWeather (float w) noexcept { weather = std::clamp (w, 0.0f, 1.0f); honkiness = 0.2f + 0.6f * weather; backdropDue = 0.0f; }

    /** For the tests: makes one of the rare events happen as soon as it can. */
    enum class Treat { hatTheft, trip, tapHeadphones, adjustHat, wipeBrow, fistPump, swing, stretch, breeze };
    void debugTreat (Treat t) noexcept
    {
        switch (t)
        {
            case Treat::hatTheft:      hat.wait = 0.0f; break;
            case Treat::trip:          keeper.tripWait = 0.0f; break;
            case Treat::tapHeadphones: startGesture (Gesture::tap, 2.6f); break;
            case Treat::adjustHat:     startGesture (Gesture::hat, 1.6f); break;
            case Treat::wipeBrow:      startGesture (Gesture::wipe, 1.5f); break;
            case Treat::fistPump:      startGesture (Gesture::pump, kPumpSeconds); break;
            case Treat::swing:         keeper.bitWait = 1.0f; keeper.sinceBit = 9.0f; break;
            case Treat::stretch:       startGesture (Gesture::stretch, 2.6f); break;
            case Treat::breeze:        gust = 0.0f; break;
        }
    }
    float damNow() const noexcept { return dam; }
    float keeperX() const noexcept { return keeper.x; }

    //==============================================================================
    /** Draws into the raster's own buffer (see pixels()). */
    void render()
    {
        hotCount = 0;
        const bool night = input.soloHonk;         // Solo Honk: the lights go down, the geese stay lit
        const float dim = night ? 0.38f : 1.0f;
        sceneDim = dim;
        setLight (dim, weather);
        // The backdrop moves slowly (drifting clouds and water), so it is redrawn about seven times
        // a second and copied back in between; the characters are drawn fresh every frame. It is
        // redrawn at once when the lights change or a tree has to move.
        const std::uint32_t sig = backdropSignature();
        if (backdropDue <= 0.0f || backdropDim < dim || backdropDim > dim || sig != backdropSig)
        {
            r.clearDepth();
            drawBackdrop (dim);
            drawFarShore (dim);
            r.keep();
            // About seven times a second while anything is going on; in a still pond, where only
            // the clouds and the water drift, once a second is plenty.
            backdropDue = moving || fussing || weather > 0.3f ? 0.14f : 1.0f;
            backdropDim = dim;
            backdropSig = sig;
        }
        else
            r.restore();
        // The weather greys the sky, the water and the far shore. Everyone on the bank and in the
        // stream keeps most of their colour whatever it does (the rain itself is streaks and sky),
        // so the picture stays the picture it is through a long busy session. With a range in
        // focus, its own creatures take no grey at all, and everyone else fades back towards the
        // paper: not darker, paler.
        const float grey = std::min (weather, kCastGrey);
        const Rgb paper = rgb (0xf5eed8) * dim;
        const float asideVeil = kAsideVeil * hush;
        drawZones();
        drawReeds();
        drawPosts();
        setLight (dim, grey);
        r.setVeil (paper, asideVeil);
        drawResident();
        for (int i = 0; i < kMaxGeese; ++i)
            if (geese[(size_t) i].used)
            {
                const float still = geese[(size_t) i].still;
                setLight (dim, grey * (hush > 0.0f ? still : 1.0f));
                // A goose is mostly white, and white faded towards the paper hardly changes: one
                // out of focus goes towards a soft grey instead, so that it plainly steps back.
                r.setVeil (rgb (0xb9b2a2) * dim, kGooseAsideVeil * still);
                drawNotchGoose (i, night ? 0.55f : 0.0f);
            }
        // The recordist keeps his own light: in Solo Honk he is the one still lit, like the geese.
        r.setVeil (paper, 0.0f);
        setLight (1.0f, grey * (1.0f - hush));
        drawKeeper();
        drawHat();
        setLight (dim, grey * (input.focus == kNumModules ? 1.0f - hush : 1.0f));
        r.setVeil (paper, input.focus == kNumModules ? 0.0f : asideVeil);
        drawFish();
        setLight (dim, grey);
        r.setVeil (paper, asideVeil);
        drawBeaver();                                   // first: the dam's new stick starts from its paws
        drawDam();
        drawLogAndTurtles();
        drawFrog();
        drawDragonflies();
        r.setVeil (paper, 0.0f);
        setLight (dim, weather);
        drawBursts();
        drawRings();
        drawRain();
    }

    /** The picture: blue, green, red, 255 for each pixel, row by row (an opaque ARGB image's layout). */
    const std::uint8_t* pixels() const noexcept { return r.colour.data(); }
    int width() const noexcept { return r.width; }
    int height() const noexcept { return r.height; }

private:
    static constexpr int kMaxGeese = 10;
    static constexpr float kGooseZ = -0.85f;       // where a goose stands on the bank on its own
    static constexpr float kLeavingLowFor = 0.5f;  // a goose taking off is near the bank for this much of its way
    static constexpr float kGooseBackZ = -0.45f, kGooseFrontZ = -1.45f;   // two rows, where they crowd
    static constexpr float kLogGooseBackZ = -0.12f, kLogGooseFrontZ = -0.5f;   // the two rows behind the turtles' log
    static constexpr float kKeeperZ = -0.12f;      // the recordist, at the back by the water, behind the geese
    static constexpr float kPi = 3.14159265f;

    static float smooth01 (float t) noexcept { t = std::clamp (t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
    static float bump (float t) noexcept { return t <= 0.0f || t >= 1.0f ? 0.0f : std::sin (t * kPi); }
    static float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }
    static Vec3 lerp (Vec3 a, Vec3 b, float t) noexcept { return a + (b - a) * t; }
    /** 0 -> 1 over the first `in` of t, held, then 1 -> 0 over the last `out`. */
    static float envelope (float t, float in, float out) noexcept { return smooth01 (t / in) * (1.0f - smooth01 ((t - (1.0f - out)) / out)); }
    /** Moves `v` towards `target`, covering most of the gap in `seconds`. */
    static void follow (float& v, float target, float dt, float seconds) noexcept { v += (target - v) * std::min (1.0f, dt / seconds); }
    /** A squash (positive) or stretch (negative) about the feet, keeping the volume about the same. */
    static Xform squash (float s) noexcept
    {
        s = std::clamp (s, -0.35f, 0.35f);
        Xform x;
        x.m[0] = x.m[8] = 1.0f + 0.5f * s;
        x.m[4] = 1.0f - s;
        return x;
    }
    static Xform place (Vec3 at, float yaw, float size) noexcept { return Xform::scale (size).then (Xform::rotY (yaw)).then (Xform::move (at)); }

    static constexpr float kLogHalf = 0.85f;          // half the turtles' log, in stream-creature sizes
    static constexpr float kOfferSeconds = 3.5f;     // the microphone held out to the frog or the turtles, there and back
    static constexpr float kHatSeconds = 0.8f;       // a hand to the brim of his hat, a push, and away
    static constexpr float kPumpSeconds = 0.75f;     // the recordist's fist pump, start to settle
    static constexpr float kTripSeconds = 1.5f;      // caught, two skips on the front foot, settle
    static constexpr float kF = 1.0f / 60.0f;        // the bits between him and his goose are timed in sixtieths of a second
    static constexpr float kKnockSeconds = 0.75f;    // a clicked goose in the air
    static constexpr float kFallSeconds = 3.4f;      // a clicked turtle: off the log, under, and back up
    static constexpr float kCastGrey = 0.3f;         // the most the weather greys the characters (the sky takes all of it)
    static constexpr float kAsideVeil = 0.38f;       // how far those out of focus fade towards the paper
    static constexpr float kGooseAsideVeil = 0.5f;   // ... and the geese, towards a grey (see render)

    // Geese come and go slowly. A notch must have been there a moment before a goose comes for it,
    // a goose that has landed stays a while, and only so many are on the bank at once.
    static constexpr int kMaxCands = 12, kMaxPeaks = 16;
    static constexpr int kLiveGeese = 5, kGeesePerRange = 2;
    static constexpr float kGooseKeepDb = 0.6f;       // a goose keeps a notch that is still this deep
    static constexpr float kGooseComeDb = 1.5f;       // ... and comes for one that has been this deep
    static constexpr float kGooseComeSeconds = 0.5f;  //     for this long (a third of that from 3 dB)
    static constexpr float kGooseStaySeconds = 3.0f;  // once down it stays at least this long
    static constexpr float kGooseGapSeconds = 2.0f;   // between two arrivals
    struct Cand { float x = 0, depth = 0, seen = 0, missed = 0; bool used = false; };

    /** A high sun and a lot of light from the sky: colours stay flat and bright, shadows soft.
        Under cloud (`grey`) the sun goes and everything flattens and cools a little. */
    void setLight (float dim, float grey) noexcept
    {
        r.setLight ({ -0.4f, 0.8f, -0.45f }, rgb (0x5a5448) * (dim * (1.0f - 0.8f * grey)), (rgb (0xe4e0dc).mix (rgb (0xb8bcc4), grey)) * dim,
                    (rgb (0xf6e6d6).mix (rgb (0x9aa0a8), grey)) * dim, 30.0f - 12.0f * grey, 110.0f - 45.0f * grey);
    }

    /** A closed, smiling eye: a little upturned arc, drawn as two short strokes in `at`'s space. */
    void happyEye (const Xform& at, Vec3 c, float half, float lift, float thick, Rgb colour) noexcept
    {
        const Vec3 l { c.x, c.y - lift, c.z - half }, m { c.x + 0.004f, c.y + lift, c.z }, rr { c.x, c.y - lift, c.z + half };
        r.limb (at.apply (l), at.apply (m), thick, thick, colour, 3);
        r.limb (at.apply (m), at.apply (rr), thick, thick, colour, 3);
    }

    /** Random numbers for the timing of things. Always the same sequence, so a session looks the
        same each time and the tests can rely on it. */
    float rnd() noexcept
    {
        seed = seed * 1664525u + 1013904223u;
        return (float) (seed >> 8) * (1.0f / 16777216.0f);
    }
    float rnd (float lo, float hi) noexcept { return lo + (hi - lo) * rnd(); }

    struct Blink
    {
        float wait = 2.0f, t = 1.0f;
        float closed() const noexcept { return bump (t); }
    };
    /** Blinks wait while the pond is still (a blink is shorter than one frame at the idle rate);
        one under way keeps the full rate until it is done. */
    void tick (Blink& b, float dt, float lo = 2.0f, float hi = 6.0f) noexcept
    {
        if (lively && (b.wait -= dt) <= 0.0f) { b.t = 0.0f; b.wait = rnd (lo, hi); }
        if (b.t < 1.0f) { b.t = std::min (1.0f, b.t + dt / 0.18f); fussing = true; }
    }

    /** A weight on a spring: for squash, wobble and settle. */
    struct Spring
    {
        float x = 0.0f, v = 0.0f;
        void tick (float target, float dt, float stiffness, float damping) noexcept
        {
            v += (stiffness * stiffness * (target - x) - 2.0f * damping * stiffness * v) * dt;
            x += v * dt;
        }
        bool busy() const noexcept { return std::abs (x) > 0.01f || std::abs (v) > 0.08f; }
    };

    struct Goose
    {
        bool used = false, leaving = false, thief = false;
        float homeX = 0, x = 0, vx = 0, depth = 0, arrive = 0, away = 0, honkPhase = 0, lastCycle = 0, unseen = 0, seed = 0;
        float walkPhase = 0, walkAmt = 0, yaw = 0, landed = 0, hiss = 0, flap = 1.0f, age = 0, peak = 0, fleeX = 0;
        float wag = 0, wagPhase = 0, temper = 0;                       // temper: seconds it stays cross after the pole came down
        float patience = 2.5f;                                         // seconds it waits for a notch that has closed
        float still = 0;                                               // 1 = out of focus: holding still
        float preen = 1.0f;                                            // a preen under the wing, 0..1 (1 = not preening)
        float reed = 0;                                                // how hard it is pushing the reeds aside (as it comes through them)
        float knock = 1.0f, knockTo = 0, knockX = 0, knockPhase = 0, dizzy = 0;   // clicked: in the air, where to, how far from its place now
        float z = kGooseZ, laneZ = kGooseZ, spreadX = 0, crowd = 0;    // its row on the bank, where it stands to be seen, how crowded
        bool crowded = false, byLog = false;                           // byLog: standing close behind the turtles' log
        bool beaten = false;                                           // its notch closed while he was at it: it goes when he sends it
        float hissClock = 0, alert = 0, skid = 0, depthSlow = 0;       // the hiss comes in bursts; neck up, watching; the slide as it lands; its depth a moment ago
        Spring squash;
        Blink blink;
    };
    enum class Gesture { none, tap, hat, pump, wipe, sip, stretch, wave, tip, shoulder, coil, polish, doze, offer };
    /** What he and the goose he is working get up to between them. Each is a wind-up, the action,
        a follow-through and a held ending, and each starts from and comes back to the pose he holds
        the microphone in (see bitPose and gooseBit). One at a time, with a rest between. */
    enum class Bit { none, shoo, whiff, tug, lunge, win, gooseWins, honk };
    /** A bit can go more than one way (`var`): the lunge ends in his cable or in a skid (1), and
        the swing that misses has three endings (the low two bits) and three lengths of wind-up
        (the next two). */
    static float whiffWind (int var) noexcept { const int w = (var >> 2) & 3; return w == 0 ? 14.0f : w == 1 ? 20.0f : 28.0f; }
    static float bitFrames (Bit b, int var) noexcept
    {
        switch (b)
        {
            case Bit::shoo:      return 58.0f;
            case Bit::whiff:     return ((var & 3) == 0 ? 84.0f : (var & 3) == 1 ? 96.0f : 92.0f) + whiffWind (var) - 20.0f;
            case Bit::tug:       return 128.0f;
            case Bit::lunge:     return var != 0 ? 98.0f : 160.0f;
            case Bit::win:       return 114.0f;
            case Bit::gooseWins: return 192.0f;
            case Bit::honk:      return 56.0f;
            case Bit::none:      break;
        }
        return 0.0f;
    }
    /** How long its action lasts: until then nothing else big in the picture may start. */
    static float bitActionFrames (Bit b, int var) noexcept
    {
        switch (b)
        {
            case Bit::shoo:      return 42.0f;
            case Bit::whiff:     return ((var & 3) == 0 ? 47.0f : (var & 3) == 1 ? 78.0f : 74.0f) + whiffWind (var) - 20.0f;
            case Bit::tug:       return 100.0f;
            case Bit::lunge:     return var != 0 ? 86.0f : 148.0f;
            case Bit::win:       return 76.0f;
            case Bit::gooseWins: return 174.0f;
            case Bit::honk:      return 32.0f;
            case Bit::none:      break;
        }
        return 0.0f;
    }
    static bool bigBit (Bit b) noexcept { return b == Bit::lunge || b == Bit::win || b == Bit::gooseWins; }
    struct Keeper
    {
        float x = -2.6f, speed = 0, phase = 0, facing = 1, yaw = 1.35f, poleYaw = -0.3f, shoo = 0, sit = 0, idle = 0;
        float recoil = 0, trip = 1.0f, tripWait = 9.0f, solo = 0, compare = 0, chase = 0, hand = -1.0f;
        float pumpWait = 0.0f, cableX = -3.9f, quiet = 0;
        // The bit that is playing: which, how far through (seconds), how much of it shows (it
        // fades when it has to end early, so nothing jumps), the goose, which way the goose is,
        // and where he stood when it began.
        Bit bit = Bit::none, lastBit = Bit::none, nextBit = Bit::none;
        float bt = 0, bitW = 0, bitDir = 1.0f, bitX = 0, bitRoom = 2.6f, bitReach = 1.2f, bitScale = 1.0f;   // bitScale: how far its steps take him, against how they are written
        bool bitCut = false;
        int bitVar = 0, lastWhiff = -1;            // which way this one goes (see bitFrames), and how the last miss ended
        int bitGoose = -1;
        float bitWait = 0.3f, gapScale = 1.0f;     // how far the rest between bits has run (1 = over), and this rest's share of scatter
        float sinceBit = 9.0f, bigCool = 6.0f, gooseWinCool = 12.0f, cue = 0;   // cue: counting down the last frames before a bit
        float heat = 0, honkCool = 0;              // how hard the mix is working him, 0..1
        int rung = 0, rungGoose = -1;              // how far things have escalated with this goose
        float ceilingT = 0;                        // seconds his goose's cut has sat near Max Cut
        // Holding the microphone is never quite still: it follows the goose's head a moment late,
        // he shifts his weight, breathes, takes a new grip, looks from the goose to the microphone
        // and listens, crouches as the cut deepens, and flinches at a loud honk.
        Spring micX, micY, tip, hatLag, hatHop, cableWave;
        float aimW = 0, weight = -1.0f, weightTo = -1.0f, weightWait = 3.0f, grip = 1.0f, gripWait = 5.0f;
        float glanceWait = 2.0f, lookGoose = 0, lookMic = 0, lookEar = 0, earT = 0, flinch = 0, crouch = 0, stance = 0;
        // How he stands to a goose goes with its cut: upright for a light one, knees bent and a
        // foot back for a middling one, low and wide for a deep one (0..2, eased). Each honk rocks
        // him back, and every few seconds he takes a shuffle step towards it or away.
        float stanceAt = 0, flinchT = 1.0f, flinchAmt = 0;
        int stanceTo = 0;
        float stepT = 1.0f, stepFrom = 0, stepTo = 0, stepWait = 2.5f;
        float frontW = 1.0f, hatLand = 1.0f;       // which hand leads on the pole, eased; his hat landing back on his head (0..1, 1 = settled)
        float holdArrive = 0;                      // seconds a goose has been kept waiting off the picture for his big moment
        int glance = 0;
        float headWas = 0, noGoose = 0, rest = 0, stillT = 0, fishLook = 0;   // rest: the pole on his shoulder, with nothing to do
        bool shouldered = false;
        int lastPiece = -1;
        float runAmt = 0, yawTo = 1.35f;           // 0 standing .. 1 at a run, eased; the way he is turning to
        float setOff = 1.0f, skidT = 1.0f;         // leaning back before a run, and the skid at the end of one (0..1, 1 = none)
        bool ran = false;
        float switchT = 0, greet = 0, clickWait = 0;   // clickWait: above 0 while a click on him has not been answered yet
        int target = -1, clickTurn = 0;            // the goose he is working
        bool walking = false, going = false;       // going: on his way somewhere more than a shuffle off
        float look = 0, lookTo = 0, lookWait = 1.0f, workSide = -1.0f, eyeLook = 0, torsoLook = 0, extend = 0;
        Gesture gesture = Gesture::none;
        float gt = 0, glen = 1.0f;
        bool adjustNext = false;
        bool idleGesture = false;      // the gesture was started by the shared idle timer
        Blink blink;
    };
    struct Ring { float x = 0, y = 0, z = 0, age = 1.0f, size = 0.5f, rate = 1.9f; bool flat = false; Rgb colour; bool splash = false; };
    struct Burst { float x = 0, z = 0, t = 1.0f, size = 1.0f, seed = 0; float y = 0; bool feathers = false; int kind = 0; float dir = 0; };   // kind: 0 drops (or feathers), 2 dust, 3 speed lines, 4 a hit
    struct Fish { float t = 9.0f, power = 0.5f, dir = 1.0f; bool flopped = true; float size = 1.0f, off = 0.0f; };
    struct HotSpot { float x = 0, y = 0, rx = 1, ry = 1; Hot kind = Hot::none; int index = 0; };
    // 0 on his head, 1 snatched, 2 on the goose, 3 on its way back by itself, 4 he is fetching it with the pole
    struct Hat { int state = 0; float t = 0, wait = 70.0f; int goose = -1; int prefer = -1; bool reach = false; Vec3 from; };
    struct Beaver
    {
        float x = 0, yaw = 0.4f, upright = 0, build = 0, sulk = 0, proud = 0, proudT = 0, pat = 0, slap = 0, earWait = 3.0f, ear = 1.0f, patWait = 5.0f;
        float patLen = 0.8f, carry = 0.0f;
        float wave = 1.0f, flop = 1.0f;     // a wave of the paw; a slap of the tail while floating (0..1, 1 = none)
        float take = 1.0f, hurry = 0, brow = 1.0f;   // a start when the reading jumps, then it hurries; a wipe of its brow (0..1, 1 = none)
        bool wiped = false;
        int pats = 0, patCount = 3;
        bool patting = false, idlePat = false;
        Blink blink;
    };
    struct Turtle
    {
        float out = 1.0f, delay = 0, peekWait = 0, look = 0, lookTo = 0, lookWait = 2.0f, stretch = 1.0f, stretchWait = 9.0f;
        float antic = 1.0f, eyes = 0.0f;   // the start of a hide (0..1, head up, eyes wide); the eyes showing in the opening
        float fall = 1.0f, stretchDelay = -1.0f;   // clicked: toppling off the log and climbing back (0..1, 1 = on the log)
        int stage = 3;                  // 0 right in, 1 an eye, then two, in the opening, 2 head peeking, 3 out
        bool in = false;
        Spring wobble, drop;            // the shell's rock, and its squash as it lands on the log
        Blink blink;
    };
    struct Frog
    {
        int state = 0;                  // 0 sitting, 1 crouching, 2 in the air, 3 under, 4 swimming back, 5 climbing on, 6 shaking itself dry,
                                        // 7 staying in the water, eyes showing, until the cut eases
        float lurk = 0, rest = 0;       // seconds it has stayed in; seconds it sits before it can tense again
        float x = 0, v = 0, t = 0, cool = 1.0f, side = 1.0f, landX = 0, tension = 0, puffWait = 2.0f, puff = 1.0f, look = 0.0f;
        float lookUp = 0.0f, below = 0.0f, quiver = 0.0f, sinceLeap = 99.0f;
        float leapH = 1.0f;             // how high this leap goes: kept low where it would pass in front of him
        float tongue = 1.0f, coolKept = 0.0f;   // a flick of the tongue (0..1, 1 = none)
        bool byClick = false;           // this leap was asked for with the mouse: it leaves the ceiling's own timing alone
        int repeats = 0;
        bool armed = true;              // a leap marks a notch arriving at the ceiling, not one sitting there
        Spring pad;
        Blink blink;
    };
    struct Fly
    {
        int state = 0;                  // 0 away, 1 about, 2 leaving
        int perch = 0;                  // 0 in the air, 1 on the microphone, 2 on a post
        Vec3 pos, from, to;
        float hop = 1.0f, hopLen = 0.2f, wait = 0, yaw = 0;
    };

    //============================================================================== what happens
    /** The range a place on the bank belongs to (0..3), or -1. */
    int moduleAt (float wx) const noexcept
    {
        for (int m = 0; m < kNumModules; ++m)
            if (wx >= worldX (input.edgeHz[m]) && wx < worldX (input.edgeHz[m + 1]))
                return m;
        return -1;
    }
    /** With a range in focus, only its geese are at work. */
    bool gooseActive (const Goose& g) const noexcept { return input.focus < 0 || moduleAt (g.homeX) == input.focus; }

    void step (const WorldInput& in, float dt)
    {
        time += dt;
        const auto& f = in.frame;
        lively = ! stillPrev;
        deepestCut = 0.0f;
        for (int k = 0; k < f.numBands; ++k)
            deepestCut = std::max (deepestCut, f.cutDb[k]);
        const bool focused = in.focus >= 0;
        follow (hush, focused ? 1.0f : 0.0f, dt, 0.35f);
        moving = moving || (hush > 0.02f && hush < 0.98f);
        residentPhase += dt * (1.0f - hush);

        // Where are the notches? Each peak of the cut curve may have a goose. A goose that is here
        // keeps its notch while the notch is at all open, so that a cut wavering about a dB does
        // not send it off and bring it back.
        struct Peak { float x, depth; bool taken; };
        std::array<Peak, kMaxPeaks> peaks {};
        int numPeaks = 0;
        float deepestNow = 0.0f;
        for (int k = 0; k < f.numBands; ++k)
            deepestNow = std::max (deepestNow, f.cutDb[k]);
        for (int k = 1; k + 1 < f.numBands && numPeaks < kMaxPeaks; ++k)
            if (f.cutDb[k] > kGooseKeepDb && f.cutDb[k] >= f.cutDb[k - 1] && f.cutDb[k] > f.cutDb[k + 1])
                peaks[(size_t) numPeaks++] = { worldX (f.bandHz[k]), f.cutDb[k], false };

        // Keep each goose with its notch. With a range in focus, a goose outside it holds as it
        // is: it is not looked for, does not wait, and does not leave.
        int live = 0, shown = 0, inRange[kNumModules + 1] {};
        for (auto& g : geese)
        {
            shown += g.used ? 1 : 0;                                  // on the bank, leaving ones too
            if (! g.used || g.leaving)
                continue;
            if (g.beaten)
            {
                // Its notch has closed and he is seeing it off: it goes when the pole sends it
                // (or at once, if that is called off).
                ++live;
                if (keeper.bit != Bit::win || keeper.bitGoose != (int) (&g - geese.data()) || keeper.bitCut || in.calm)
                {
                    g.beaten = false;
                    g.leaving = true;
                }
                continue;
            }
            int best = -1;
            float bestD = 0.9f;
            for (int i = 0; i < numPeaks; ++i)
                if (! peaks[(size_t) i].taken && std::abs (peaks[(size_t) i].x - g.homeX) < bestD)
                    { bestD = std::abs (peaks[(size_t) i].x - g.homeX); best = i; }
            if (best >= 0)
                peaks[(size_t) best].taken = true;
            const int range = moduleAt (g.homeX);
            ++live;
            ++inRange[range >= 0 ? range : kNumModules];
            if (! gooseActive (g) && ! in.calm)
                continue;
            if (best >= 0)
            {
                g.homeX += (peaks[(size_t) best].x - g.homeX) * std::min (1.0f, dt * 6.0f);
                g.depth += (peaks[(size_t) best].depth - g.depth) * std::min (1.0f, dt * 10.0f);
                g.unseen = 0.0f;
                continue;
            }
            // Its notch has closed. It calms down, waits a little in case the notch comes back,
            // and goes; never before it has been down a couple of seconds.
            g.unseen += dt;
            g.depth += (1.0f - g.depth) * std::min (1.0f, dt * 1.5f);
            const bool busy = g.thief || g.knock < 1.0f || g.knockX < 0.0f || g.knockX > 0.0f;
            if ((g.unseen > g.patience && g.landed >= kGooseStaySeconds && ! busy) || in.calm)
            {
                // The notch has closed: his work is done. If he has been at this goose he sees it
                // off with one great swing (once any bit with it is over, and he has had a moment);
                // otherwise it leaves by itself, with a small fist pump from him if he was near.
                const int index = (int) (&g - geese.data());
                const bool his = ! in.calm && index == keeper.target && keeper.shoo > 0.5f && std::abs (keeper.x - g.x) < 4.4f && gooseActive (g);
                if (! in.calm && keeper.bit != Bit::none && keeper.bitGoose == index && ! keeper.bitCut)
                    continue;
                if (his && keeper.rung >= 1 && keeper.bigCool <= 0.0f && g.unseen < g.patience + 2.5f && canStartBit (in, g))
                {
                    if (keeper.sinceBit < 1.5f)
                        continue;
                    g.beaten = true;
                    startBit (Bit::win, index);
                    continue;
                }
                g.leaving = true;
                g.away = -8.0f * kF * 0.95f;                          // a crouch first, then up
                if (his && std::abs (keeper.x - g.x) < 3.2f && g.age > 1.5f && g.peak > 3.0f && keeper.pumpWait <= 0.0f
                    && keeper.bit == Bit::none && keeper.clickWait <= 0.0f && keeper.gesture == Gesture::none)
                {
                    startGesture (Gesture::pump, kPumpSeconds);
                    keeper.pumpWait = rnd (7.0f, 12.0f);
                }
            }
        }

        // The notches no goose has. One that lasts becomes a candidate, and a candidate that has
        // been deep enough for long enough brings a goose: the deepest first, one at a time with
        // a gap between, and only so many on the bank and in each range. With a range in focus
        // nothing comes for a notch outside it.
        auto minded = [&] (float x) { return in.focus < 0 || moduleAt (x) == in.focus; };
        for (auto& c : cands)
        {
            if (! c.used)
                continue;
            int best = -1;
            float bestD = 0.7f;
            for (int i = 0; i < numPeaks; ++i)
                if (! peaks[(size_t) i].taken && std::abs (peaks[(size_t) i].x - c.x) < bestD)
                    { bestD = std::abs (peaks[(size_t) i].x - c.x); best = i; }
            if (best >= 0)
            {
                auto& pk = peaks[(size_t) best];
                pk.taken = true;
                c.x += (pk.x - c.x) * std::min (1.0f, dt * 10.0f);
                c.depth += (pk.depth - c.depth) * std::min (1.0f, dt * 12.0f);
                if (pk.depth >= kGooseComeDb) { c.seen += dt; c.missed = 0.0f; }
                else                          c.missed += dt;
            }
            else
                c.missed += dt;
            if (c.missed > 0.2f || ! minded (c.x) || in.calm)
                c.used = false;
        }
        for (int i = 0; i < numPeaks; ++i)
            if (! peaks[(size_t) i].taken && peaks[(size_t) i].depth >= kGooseComeDb && minded (peaks[(size_t) i].x) && ! in.calm)
                for (auto& c : cands)
                    if (! c.used)
                    {
                        c = { peaks[(size_t) i].x, peaks[(size_t) i].depth, 0.0f, 0.0f, true };
                        break;
                    }
        spawnCool = std::max (0.0f, spawnCool - dt);
        {
            Cand* ready = nullptr;
            for (auto& c : cands)
                if (c.used && c.seen >= (c.depth >= 3.0f ? kGooseComeSeconds / 3.0f : kGooseComeSeconds) && (ready == nullptr || c.depth > ready->depth))
                    ready = &c;
            if (ready != nullptr && dt > 0.0f)
            {
                const int range = moduleAt (ready->x), slot = range >= 0 ? range : kNumModules;
                // Never more than kLiveGeese in the picture, counting any still on their way off:
                // a new one waits for a leaving one to be gone.
                const bool room = live < kLiveGeese && inRange[slot] < kGeesePerRange;
                if (! room)
                {
                    // No room. If this notch is a good deal deeper than the shallowest goose in its
                    // way (in its range, or on the whole bank), that one makes way for it.
                    Goose* least = nullptr;
                    for (auto& g : geese)
                        if (g.used && ! g.leaving && gooseActive (g) && ! g.thief && g.landed >= kGooseStaySeconds
                            && (inRange[slot] < kGeesePerRange || moduleAt (g.homeX) == range)
                            && (least == nullptr || g.depth < least->depth))
                            least = &g;
                    if (least != nullptr && ready->depth > least->depth + 2.0f && least->knock >= 1.0f && ! (least->knockX < 0.0f || least->knockX > 0.0f)
                        && ! least->beaten && ! (keeper.bit != Bit::none && keeper.bitGoose == (int) (least - geese.data())))
                    {
                        least->leaving = true;
                        least->away = -8.0f * kF * 0.95f;
                    }
                }
                else if (spawnCool <= 0.0f && shown < kLiveGeese && ! holdArrival (ready->x, dt))
                    for (auto& g : geese)
                        if (! g.used)
                        {
                            g = {};
                            g.used = true;
                            g.homeX = g.x = g.spreadX = ready->x;
                            g.depth = ready->depth;
                            g.seed = (float) (++spawned % 7) * 0.9f;
                            g.arrive = 0.0f;
                            g.yaw = keeper.x >= g.x ? -0.45f : kPi + 0.45f;
                            g.blink.wait = rnd (0.5f, 4.0f);
                            g.patience = rnd (2.0f, 3.0f);
                            ready->used = false;
                            spawnCool = kGooseGapSeconds;
                            break;
                        }
            }
        }

        spreadGeese();
        for (auto& g : geese)
            if (g.used && ! g.leaving && g.arrive <= 0.0f && g.landed <= 0.0f && ! g.thief)
                g.x = g.spreadX;                                      // just come: it hops in where it will stand
        takeClick();

        // The geese themselves.
        float worstDepth = 0.0f, worstX = keeper.x;
        deepestGoose = -1;
        bool anyGoose = false;

        for (auto& g : geese)
        {
            if (! g.used)
                continue;
            const int index = (int) (&g - geese.data());
            const bool active = gooseActive (g);
            follow (g.still, active ? 0.0f : 1.0f, dt, active ? 0.3f : 0.8f);
            if (active)
                tick (g.blink, dt);
            if (g.leaving)
            {
                g.away += dt * 0.95f;
                g.reed = 0.0f;
                if (g.away >= 1.0f || in.calm)
                    g.used = false;
                moving = true;
                continue;
            }
            anyGoose = anyGoose || active;
            const bool wasDown = g.arrive >= 1.0f;
            g.arrive = std::min (1.0f, g.arrive + dt * 2.6f);
            if (! wasDown && g.arrive >= 1.0f)
            {
                g.squash.v = 2.6f;                                    // lands with a squash,
                g.wag = 1.0f;                                         // a wiggle of the tail,
                g.skid = 1.0f;                                        // a short slide, and a puff of dust
                addFx (2, g.x - std::cos (g.yaw) * 0.35f * cast, 0.0f, g.z - 0.05f, 0.8f * cast, std::cos (g.yaw) >= 0.0f ? 1.0f : -1.0f);
            }
            g.skid = std::max (0.0f, g.skid - dt / (18.0f * kF));
            if (g.arrive >= 1.0f)
                g.landed += dt;
            // The reeds part as it hops through them, and spring back once it is down.
            g.reed = g.arrive < 1.0f ? 0.9f * std::sin (g.arrive * kPi) : g.landed < 1.5f ? 0.5f * std::exp (-4.0f * g.landed) * std::cos (12.0f * g.landed) : 0.0f;
            g.squash.tick (0.0f, dt, 15.0f, 0.3f);
            if (! active && g.arrive >= 1.0f && ! g.squash.busy() && g.still > 0.98f && g.knock >= 1.0f && ! (g.knockX < 0.0f || g.knockX > 0.0f))
                continue;                                             // out of focus and settled: nothing about it moves
            g.wag = std::max (0.0f, g.wag - dt / 1.1f);
            g.wagPhase += dt * 19.0f;
            g.age += dt;
            g.peak = std::max (g.peak, g.depth);

            // In the air it drifts to where it is to stand (another may have shifted meanwhile).
            if (g.arrive < 1.0f && ! g.thief)
                g.x += (g.spreadX - g.x) * std::min (1.0f, dt * 10.0f);
            // It waddles after its notch when the notch drifts (and runs, with the hat).
            const float goal = g.thief ? g.fleeX : g.spreadX;
            // A goose with far to go (a newcomer has landed beside it) hurries a little.
            const float top = g.thief ? 3.6f : std::abs (goal - g.x) > 0.6f * cast ? 2.4f : 1.5f;
            // (In a bit with him it stays where the bit puts it.)
            const bool inBit = keeper.bit != Bit::none && keeper.bitGoose == index;
            const float wanted = g.arrive < 1.0f || ! active || inBit ? 0.0f : std::clamp ((goal - g.x) * 4.0f, -top, top);
            g.vx += (wanted - g.vx) * std::min (1.0f, dt * 8.0f);
            g.x += g.vx * dt;
            // Into its row: a few steps forward or back.
            const float vz = g.arrive < 1.0f || ! active ? 0.0f : std::clamp ((g.laneZ - g.z) * 3.0f, -0.9f, 0.9f);
            g.z += vz * dt;
            follow (g.crowd, g.crowded ? 1.0f : 0.0f, dt, 0.5f);
            const float stepping = std::max (std::abs (g.vx), 1.4f * std::abs (vz));
            g.walkPhase += stepping * dt * 8.5f / cast;
            follow (g.walkAmt, std::clamp (stepping / 0.5f, 0.0f, 1.0f), dt, 0.12f);
            if (g.walkAmt > 0.3f && g.wag < 0.25f)
                g.wag = 0.25f;                                        // the tail goes as it walks

            // Clicked: through the air, a moment to gather itself, and the waddle back.
            if (g.knock < 1.0f)
            {
                g.knock = std::min (1.0f, g.knock + dt / kKnockSeconds);
                g.knockX = g.knockTo * g.knock;
                if (g.knock >= 1.0f)
                {
                    g.squash.v = 2.6f;
                    g.wag = 1.0f;
                    g.dizzy = 0.8f;
                }
            }
            else if (g.knockX < 0.0f || g.knockX > 0.0f)
            {
                if (g.dizzy > 0.0f)
                    g.dizzy -= dt;
                else
                {
                    const float pace = 1.5f * dt;
                    g.knockPhase += pace * 8.5f / cast;
                    g.knockX = std::abs (g.knockX) <= pace ? 0.0f : g.knockX - std::copysign (pace, g.knockX);
                }
            }
            const bool away = g.knock < 1.0f || g.knockX < 0.0f || g.knockX > 0.0f;

            const float towards = g.walkAmt > 0.45f ? (g.vx > 0.0f ? 1.0f : -1.0f) : (keeper.x >= g.x ? 1.0f : -1.0f);
            // Side on, turned a little away towards him. In a crowd the front row turns to face us
            // a little and the back row away, so that they take less room along the bank.
            const float turnAway = lerp (0.45f, inBackRow (g) ? 0.75f : -0.35f, g.crowd);
            const float yawTo = (towards > 0.0f ? 0.0f : kPi) - turnAway * towards + 0.15f * std::sin (g.seed);
            follow (g.yaw, yawTo, dt, 0.24f);

            // Only the one he is working minds the pole. It hisses at the microphone in short
            // bursts with a rest between (so that a hiss is something that happens, not how it
            // stands), and whenever he winds up for a swing.
            const bool worked = index == keeper.target && active && ! g.thief && ! away;
            g.temper = std::max (0.0f, g.temper - dt);
            const bool poleNear = worked && std::abs (micAt.x - g.x) < 2.0f && keeper.shoo > 0.5f;
            g.hissClock = poleNear ? g.hissClock + dt : 0.0f;
            const float gf = inBit ? keeper.bt / kF : -1.0f;
            const bool windUp = inBit && ((keeper.bit == Bit::whiff && gf > 6.0f && gf < whiffWind (keeper.bitVar) + 2.0f) || (keeper.bit == Bit::win && gf > 6.0f && gf < 33.0f));
            const bool burst = poleNear && ! inBit && keeper.cue <= 0.0f && g.depth > 2.0f && std::fmod (g.hissClock + 1.2f, 3.2f) > 2.0f;
            follow (g.hiss, (burst || windUp) && g.flap >= 1.0f ? 1.0f : 0.0f, dt, 0.1f);
            // The others look over while a bit is on, necks up.
            follow (g.alert, keeper.bit != Bit::none && ! inBit && active && keeper.bt < bitActionFrames (keeper.bit, keeper.bitVar) * kF ? 1.0f : 0.0f, dt, 0.1f);
            g.depthSlow += (g.depth - g.depthSlow) * std::min (1.0f, dt / 0.5f);
            if (g.flap < 1.0f)
            {
                const float before = g.flap;
                g.flap = std::min (1.0f, g.flap + dt / 0.85f);
                if (before < 0.8f && g.flap >= 0.8f)
                {
                    g.squash.v = 2.0f;
                    g.wag = 0.8f;
                }
            }
            if (g.preen < 1.0f)
                g.preen = std::min (1.0f, g.preen + dt / 2.4f);

            // It honks, slower than it used to; out of focus it holds its beak shut.
            if (g.hiss < 0.5f && g.still < 0.5f && ! away && g.preen >= 1.0f && ! inBit)
                g.honkPhase += dt * (0.55f + 0.1f * g.depth);
            if (active && g.depth > worstDepth) { worstDepth = g.depth; worstX = g.x; deepestGoose = index; }

            // A honk leaves the beak as a ring each time the beak opens: only from the one he is
            // working (or the deepest, when he is not), so that there is one set of rings at a time.
            const float cycle = g.honkPhase - std::floor (g.honkPhase);
            const bool ringing = keeper.target >= 0 ? worked : index == ringGoose;
            if (ringing && g.arrive >= 1.0f && cycle < g.lastCycle && dt > 0.0f && g.hiss < 0.5f && g.still < 0.5f)
            {
                addRing (g.x + std::cos (g.yaw) * 0.9f * cast, (2.0f + 0.07f * g.depth) * cast, g.z - 0.3f, (0.4f + 0.05f * g.depth) * cast, false, 0.0f, 1.6f, rgb (0xe5533b));
                if (worked && keeper.shoo > 0.8f)
                {
                    // The microphone jumps a little at each honk, and a loud one makes him flinch.
                    // (All of him rocks back from it, more for a deeper cut.)
                    keeper.micY.v += 1.1f * cast;
                    if (keeper.bit == Bit::none)
                    {
                        keeper.flinchT = 0.0f;
                        keeper.flinchAmt = g.depth > 4.0f ? 1.0f : g.depth > 2.0f ? 0.5f : 0.3f;
                    }
                    if (g.depth > 4.0f)
                        keeper.flinch = 1.0f;
                }
            }
            g.lastCycle = cycle;
            moving = true;
        }
        ringGoose = deepestGoose;
        for (auto& ring : rings)
            if (ring.age < 1.0f)
            {
                ring.age += dt * ring.rate;
                moving = true;
            }
        for (auto& b : bursts)
            if (b.t < 1.0f)
            {
                b.t += dt / (b.feathers ? 1.3f : b.kind == 2 ? 24.0f * kF : b.kind == 3 ? 9.0f * kF : b.kind == 4 ? 10.0f * kF : 0.55f);
                moving = true;
            }

        stepHat (in, dt, anyGoose);
        stepKeeper (in, dt, anyGoose);
        stepFish (in, dt);
        stepBeaver (in, dt);
        stepTurtles (in, dt);
        stepFrog (in, dt, anyGoose && ! focused, worstX, worstDepth, focused ? 0.0f : deepestNow);
        stepFlies (in, dt);
        direct (in, dt, anyGoose);

        // The reeds sway while anything is going on, and come to rest with the pond.
        {
            const bool busy = moving || fussing;
            follow (reedLife, busy ? 1.0f : 0.0f, dt, busy ? 1.2f : 0.25f);
            if (! busy && reedLife > 0.03f)
                fussing = true;                                       // let them settle at the full rate
            else if (! busy)
                reedLife = 0.0f;
        }
    }

    //------------------------------------------------------------------------------ the mouse
    void takeClick()
    {
        const Hot kind = clicked;
        const int i = clickedIndex;
        clicked = Hot::none;
        switch (kind)
        {
            case Hot::turtle:
                if (i >= 0 && i < 3 && turtles[(size_t) i].fall >= 1.0f)
                    turtles[(size_t) i].fall = 0.0f;
                break;
            case Hot::beaver:
                if (beaver.wave >= 1.0f && beaver.flop >= 1.0f)
                    (beaver.upright > 0.6f ? beaver.wave : beaver.flop) = 0.0f;
                break;
            case Hot::frog:
                if (frog.state == 0)
                {
                    frog.state = 1; frog.t = 0.0f;
                    frog.byClick = true;
                    frog.coolKept = frog.cool;
                    chooseLeap (frogLo(), frogHi());
                }
                break;
            case Hot::goose:
                if (i >= 0 && i < kMaxGeese)
                {
                    auto& g = geese[(size_t) i];
                    if (g.used && ! g.leaving && g.arrive >= 1.0f && g.knock >= 1.0f && ! (g.knockX < 0.0f || g.knockX > 0.0f) && ! g.thief && ! g.beaten)
                    {
                        if (keeper.bit != Bit::none && keeper.bitGoose == i)
                            cutBit();
                        // Away from him, unless that is off the end of the bank.
                        float dir = g.x >= keeper.x ? 1.0f : -1.0f;
                        const float far = 2.3f * std::max (cast, 0.7f);
                        if (std::abs (g.x + dir * far) > kHalfWidth - 1.0f)
                            dir = -dir;
                        g.knockTo = dir * far;
                        g.knock = 0.0f;
                        g.flap = 1.0f; g.hiss = 0.0f; g.preen = 1.0f;
                        const float size = (1.0f + 0.07f * std::min (8.0f, g.depth)) * cast;
                        for (auto& b : bursts)
                            if (b.t >= 1.0f)
                            {
                                b = { g.x, g.z, 0.0f, size, rnd (0.0f, 6.0f), 0.7f * size, true };
                                break;
                            }
                    }
                }
                break;
            case Hot::keeper:
                keeper.clickWait = 1.0f;                              // kept until he has answered it, as soon as his hands are free
                if (keeper.bit != Bit::none && keeper.bit != Bit::win)
                    cutBit();
                break;
            case Hot::fly:
                for (auto& d : flies)
                    if (d.state == 1)
                    {
                        d.perch = 0;
                        d.from = d.pos;
                        d.to = { std::clamp (d.pos.x + (rnd() < 0.5f ? -1.0f : 1.0f) * rnd (2.5f, 4.5f), -kHalfWidth + 0.8f, kHalfWidth - 0.8f),
                                 rnd (2.0f, 2.8f) * std::max (cast, 0.75f), rnd (1.0f, 2.6f) };
                        d.hop = 0.0f; d.hopLen = 0.22f;
                        d.wait = rnd (1.0f, 2.2f);
                    }
                break;
            case Hot::resident:
                if (dabble >= 1.0f)
                    dabble = 0.0f;
                break;
            case Hot::none:
                break;
        }
    }

    /** True while a click's reaction is still playing. */
    bool reacting() const noexcept
    {
        for (const auto& t : turtles)
            if (t.fall < 1.0f)
                return true;
        for (const auto& g : geese)
            if (g.used && (g.knock < 1.0f || g.knockX < 0.0f || g.knockX > 0.0f))
                return true;
        return beaver.wave < 1.0f || beaver.flop < 1.0f || dabble < 1.0f || keeper.clickWait > 0.0f || keeper.greet > 0.05f;
    }

    //------------------------------------------------------------------------------ small business
    /** One director for all the small business in the picture: one piece at a time, with seconds
        between, and none while something bigger is going on. */
    void direct (const WorldInput& in, float dt, bool anyGoose)
    {
        if (gust < 1.0f)
        {
            gust = std::min (1.0f, gust + dt / 3.2f);
            fussing = true;
        }
        if (bowT < 1.0f)
        {
            bowT = std::min (1.0f, bowT + dt / 1.2f);
            moving = true;
        }
        if (dabble < 1.0f)
        {
            dabble = std::min (1.0f, dabble + dt / 1.9f);
            moving = true;
        }
        if (idleBusy > 0.0f)
        {
            idleBusy -= dt;
            fussing = true;
            return;
        }
        const bool big = keeper.bit != Bit::none || keeper.cue > 0.0f || keeper.trip < 1.0f || hat.state != 0 || frog.state != 0 || fish.t < kFishLeap + 0.6f
                         || keeper.gesture != Gesture::none || reacting();
        if (dt <= 0.0f || big || (idleWait -= dt) > 0.0f)
            return;
        // With nothing for him to do, small business comes every few seconds for the first half
        // minute, then less often, and once the pond has been left alone for a minute only now
        // and then, so that it comes to rest.
        const float lull = keeper.noGoose;
        idleWait = anyGoose ? rnd (7.0f, 12.0f) : silence > 60.0f ? rnd (30.0f, 60.0f) : lull < 30.0f ? rnd (5.0f, 9.0f) : rnd (12.0f, 20.0f);
        startIdleEvent (in);
    }

    /** Where the geese stand so that each can be seen. On its own a goose stands right over its
        notch. Where notches crowd (several low ones, the usual case on a full mix), the geese
        stand in two rows, back and front by turns along the bank, a little smaller, and are eased
        apart a little along it, never past each other: still in order of frequency, and each
        within reach of its own notch. A goose taking off holds its place until it is well up, and
        the others keep clear of it rather than walk into the room it leaves while it is still
        there; a goose hopping in is placed with the rest, so that it lands clear of them. */
    void spreadGeese() noexcept
    {
        std::array<int, kMaxGeese> order {};
        int n = 0;
        for (int i = 0; i < kMaxGeese; ++i)
            if (geese[(size_t) i].used && (! geese[(size_t) i].leaving || geese[(size_t) i].away < kLeavingLowFor))
                order[(size_t) n++] = i;
        auto key = [&] (int i) { const auto& g = geese[(size_t) i]; return g.leaving ? g.x : g.homeX; };
        for (int i = 1; i < n; ++i)                                  // by frequency, low to high
            for (int j = i; j > 0 && key (order[(size_t) j]) < key (order[(size_t) j - 1]); --j)
                std::swap (order[(size_t) j], order[(size_t) j - 1]);
        auto G = [&] (int j) -> Goose& { return geese[(size_t) order[(size_t) j]]; };
        std::array<bool, kMaxGeese> fixed {};                         // taking off: it stays where it is
        for (int j = 0; j < n; ++j)
            fixed[(size_t) j] = G (j).leaving;
        // Two neighbours too close: each gives way by half, or the one that can move by all of it.
        auto ease = [&] (std::array<float, kMaxGeese>& x, int i, int j, float short_)
        {
            if (fixed[(size_t) i] && fixed[(size_t) j])
                return;
            if (fixed[(size_t) i])
                x[(size_t) j] += short_;
            else if (fixed[(size_t) j])
                x[(size_t) i] -= short_;
            else
            {
                x[(size_t) i] -= 0.5f * short_;
                x[(size_t) j] += 0.5f * short_;
            }
        };

        // Rows: a run of geese closer than about a goose's length takes turns, back and front.
        std::array<int, kMaxGeese> lane {};
        std::array<float, kMaxGeese> x {};
        int runStart = 0;
        for (int j = 0; j < n; ++j)
        {
            const bool closeToLast = j > 0 && G (j).homeX - G (j - 1).homeX < ((G (j).crowded || G (j - 1).crowded) ? 2.9f : 2.5f) * cast;
            if (! closeToLast)
                runStart = j;
            const bool closeToNext = j + 1 < n && G (j + 1).homeX - G (j).homeX < ((G (j).crowded || G (j + 1).crowded) ? 2.9f : 2.5f) * cast;
            lane[(size_t) j] = (closeToLast || closeToNext) ? (((j - runStart) & 1) ? -1 : 1) : 0;
            x[(size_t) j] = G (j).homeX;
            if (fixed[(size_t) j])
            {
                // Its row and place are the ones it took off from.
                const auto& g = G (j);
                const float mid = g.byLog ? 0.5f * (kLogGooseBackZ + kLogGooseFrontZ) : kGooseZ;
                lane[(size_t) j] = std::abs (g.laneZ - mid) < 1.0e-3f ? 0 : g.laneZ > mid ? 1 : -1;
                x[(size_t) j] = g.x;
            }
        }
        // Behind the turtles' log they keep to two rows of their own at the back of the bank (see
        // setSize). A goose counts as behind it while any of it would be in front of the log in the
        // picture, and leaves that row only once it is clear of it by a little.
        for (int j = 0; j < n; ++j)
        {
            auto& g = G (j);
            if (fixed[(size_t) j])
                continue;
            const float half = 1.0f * (1.0f + 0.07f * std::min (8.0f, g.depth)) * cast;
            const float zz = g.byLog ? (lane[(size_t) j] > 0 ? kLogGooseBackZ : kLogGooseFrontZ) : (lane[(size_t) j] > 0 ? kGooseBackZ : lane[(size_t) j] < 0 ? kGooseFrontZ : kGooseZ);
            const float at = g.arrive < 1.0f ? g.homeX : g.x;                  // where it stands (or lands)
            const float lo = r.project ({ at - half, 0.0f, zz }).x, hi = r.project ({ at + half, 0.0f, zz }).x;
            const float margin = g.byLog ? 6.0f : 0.0f;
            g.byLog = hi > logLeftPx - margin && lo < logRightPx + margin;
        }
        // Eased apart: neighbours in different rows need less room than two in the same row. The
        // two rows behind the log are close together, so there two in different rows need nearly
        // as much room as two in the same one.
        auto apart = [&] (int i, int j, float same, float other, float logRows)
        {
            if (lane[(size_t) i] == lane[(size_t) j])
                return same * cast;
            return (G (i).byLog && G (j).byLog ? logRows : other) * cast;
        };
        for (int pass = 0; pass < 4; ++pass)
            for (int j = 1; j < n; ++j)
                for (int back = 1; back <= 2 && j - back >= 0; ++back)
                {
                    const int i = j - back;
                    const bool sameRow = lane[(size_t) i] == lane[(size_t) j];
                    if (back == 2 && ! sameRow)
                        continue;
                    const float need = apart (i, j, 2.3f, 1.3f, 1.9f);
                    const float short_ = need - (x[(size_t) j] - x[(size_t) i]);
                    if (short_ > 0.0f)
                        ease (x, i, j, short_);
                }
        // And none stands in front of him in the picture, where it would hide his legs or his body:
        // it steps aside, the way it stands from him now, by its own length and half his width.
        // Worked out in the picture, since a goose stands nearer the camera than he does.
        std::array<float, kMaxGeese> laneZs {}, reach {}, him {}, clearOf {};
        const float keeperPx = r.project ({ keeper.x, 0.0f, kKeeperZ }).x;
        for (int j = 0; j < n; ++j)
        {
            clearOf[(size_t) j] = 0.0f;
            auto& g = G (j);
            const int l = lane[(size_t) j];
            laneZs[(size_t) j] = g.byLog ? (l > 0 ? kLogGooseBackZ : kLogGooseFrontZ) : l > 0 ? kGooseBackZ : l < 0 ? kGooseFrontZ : kGooseZ;
            reach[(size_t) j] = 1.0f * cast;
            if (fixed[(size_t) j])
            {
                laneZs[(size_t) j] = g.laneZ;
                continue;
            }
            if (hat.state != 0)
                continue;
            const float z = laneZs[(size_t) j];
            const float o = r.project ({ 0.0f, 0.0f, z }).x, per = r.project ({ 1.0f, 0.0f, z }).x - o;
            if (per < 1.0e-3f)
                continue;
            const float himHere = (keeperPx - o) / per;                   // his place, at this goose's row
            const float size = (1.0f + 0.07f * std::min (8.0f, g.depth)) * cast * (l != 0 ? 0.84f : 1.0f);
            const float clear = 0.95f * size + 0.75f * cast;
            him[(size_t) j] = himHere;
            clearOf[(size_t) j] = clear;
            const float d = x[(size_t) j] - himHere;
            if (std::abs (d) < clear)
            {
                // The side it is on now (so that it does not dart from one side to the other as he
                // moves), unless that would take it too far from its notch.
                float side = g.x >= himHere ? 1.0f : -1.0f;
                if (std::abs (himHere + side * clear - g.homeX) > 3.0f * cast)
                    side = -side;
                x[(size_t) j] = himHere + side * clear;
                reach[(size_t) j] = std::max (reach[(size_t) j], std::abs (x[(size_t) j] - g.homeX) + 0.05f);
            }
        }
        // Stepping round him can put a goose on top of its neighbour: ease them apart again, with
        // him standing where he is, so that in a crowd they share out the room on either side of
        // him rather than stand on one another. A crowded goose may go a little further from its
        // notch for it.
        for (int pass = 0; pass < 6; ++pass)
        {
            for (int j = 1; j < n; ++j)
            {
                const int i = j - 1;
                const float need = apart (i, j, 2.1f, 1.2f, 1.8f);
                const float short_ = need - (x[(size_t) j] - x[(size_t) i]);
                if (short_ > 0.0f)
                    ease (x, i, j, short_);
            }
            for (int j = 0; j < n; ++j)
            {
                if (fixed[(size_t) j])
                    continue;
                const float c = clearOf[(size_t) j], d = x[(size_t) j] - him[(size_t) j];
                if (c > 0.0f && std::abs (d) < c)
                    x[(size_t) j] = him[(size_t) j] + (d >= 0.0f ? c : -c);
                const float most = std::max (reach[(size_t) j], (lane[(size_t) j] != 0 ? 1.9f : 1.3f) * cast);
                reach[(size_t) j] = std::max (reach[(size_t) j], std::min (most, std::abs (x[(size_t) j] - G (j).homeX) + 0.05f));
            }
        }
        for (int j = 0; j < n; ++j)
        {
            auto& g = G (j);
            if (fixed[(size_t) j])
                continue;
            const float lo = j > 0 ? x[(size_t) j - 1] : -1.0e9f;      // never past a neighbour
            g.spreadX = std::max (lo, std::clamp (x[(size_t) j], g.homeX - reach[(size_t) j], g.homeX + reach[(size_t) j]));
            x[(size_t) j] = g.spreadX;
            g.crowded = lane[(size_t) j] != 0;
            g.laneZ = laneZs[(size_t) j];
            if (g.arrive < 1.0f && g.landed <= 0.0f)
                g.z = g.laneZ;                                            // it lands in its row
        }
    }

    static bool inBackRow (const Goose& g) noexcept { return g.laneZ > (g.byLog ? 0.5f * (kLogGooseBackZ + kLogGooseFrontZ) : kGooseZ); }

    /** How wide a goose's beak is open, 0..1, at this point in its honking. */
    static float honkOpen (const Goose& g) noexcept
    {
        const float cycle = g.honkPhase - std::floor (g.honkPhase);
        return std::clamp (g.depth / 6.0f, 0.25f, 1.0f) * (cycle < 0.55f ? std::sin (cycle / 0.55f * kPi) : 0.0f);
    }

    void addRing (float x, float y, float z, float size, bool flat, float delay, float rate, Rgb colour, bool splashy = false) noexcept
    {
        for (auto& ring : rings)
            if (ring.age >= 1.0f)
            {
                ring = { x, y, z, -delay * rate, size, rate, flat, colour, splashy };
                return;
            }
    }

    /** A short accent: a puff of dust (2), speed lines (3) or the flash of a hit (4). */
    void addFx (int kind, float x, float y, float z, float size, float dir) noexcept
    {
        for (auto& b : bursts)
            if (b.t >= 1.0f)
            {
                b = { x, z, 0.0f, size, rnd (0.0f, 6.0f), y, false, kind, dir };
                return;
            }
    }
    void addFeathers (float x, float y, float z, float size) noexcept
    {
        for (auto& b : bursts)
            if (b.t >= 1.0f)
            {
                b = { x, z, 0.0f, size, rnd (0.0f, 6.0f), y, true };
                return;
            }
    }

    /** Something has hit the water: rings spreading out and a few drops thrown up. The rings are
        thick foam with a darker edge, so that they show on pale water. */
    void splash (float x, float z, float size, bool drops = true) noexcept
    {
        const Rgb foam = rgb (0xf4fbff);
        addRing (x, 0.03f, z, size * 0.5f, true, 0.0f, 1.5f, foam, true);
        addRing (x, 0.03f, z, size * 0.8f, true, 0.15f, 1.3f, foam, true);
        addRing (x, 0.03f, z, size * 1.1f, true, 0.32f, 1.1f, foam, true);
        if (drops)
            for (auto& b : bursts)
                if (b.t >= 1.0f)
                {
                    b = { x, z, 0.0f, size, rnd (0.0f, 6.0f) };
                    break;
                }
    }

    /** One piece of small business, from whoever is free to do something and did not have the
        last turn. With a range in focus, only its own geese and the recordist are asked. */
    void startIdleEvent (const WorldInput& in)
    {
        const bool focused = in.focus >= 0;
        int who[7] {}, n = 0;
        auto offer = [&] (int actor, bool free) { if (free && actor != lastActor) who[n++] = actor; };
        auto& k = keeper;
        const bool kFree = k.gesture == Gesture::none && std::abs (k.speed) < 0.3f && k.trip >= 1.0f && hat.state == 0 && k.bit == Bit::none
                           && k.cue <= 0.0f && ! in.diff && ! in.soloHonk && k.clickWait <= 0.0f;
        // Working a goose, he has a hand free for a moment only when the next bit is some way off.
        const bool holdFree = k.shoo > 0.9f && k.target >= 0 && (1.0f - k.bitWait) * bitGap() > 4.0f;
        const bool lull = k.noGoose > 4.0f && k.shoo < 0.1f;
        const bool kOffer = kFree && (in.bypass ? k.sit > 0.97f : ((lull || holdFree || weather > 0.6f) && k.sit < 0.03f));
        offer (0, kOffer);
        offer (1, ! focused);                                            // the beaver always has something
        offer (2, ! focused && frog.state == 0 && frog.tension < 0.3f);
        offer (3, ! focused && asleep < 0.5f && ! turtles[0].in && ! turtles[1].in && ! turtles[2].in);
        int preener = -1;
        for (int j = 0; j < kMaxGeese && preener < 0; ++j)
        {
            const int i = (j + spawned + (int) (time * 0.1f)) % kMaxGeese;
            const auto& g = geese[(size_t) i];
            if (g.used && ! g.leaving && g.landed > 2.0f && ! g.thief && i != k.target && g.flap >= 1.0f && gooseActive (g) && g.walkAmt < 0.2f)
                preener = i;
        }
        offer (4, preener >= 0);
        offer (5, ! focused && hush < 0.05f);
        offer (6, weather < 0.5f);                                       // a breeze through the reeds
        if (n == 0)
        {
            lastActor = -1;
            return;
        }
        lastActor = who[std::min (n - 1, (int) (rnd() * (float) n))];
        // With nothing to do he is the one to watch: he takes most of the turns.
        if (kOffer && lull && ! in.bypass && lastActor != 0 && rnd() < 0.7f)
            lastActor = 0;
        switch (lastActor)
        {
            case 0:
            {
                const float pick = rnd();
                auto play = [&] (Gesture g, float seconds, int piece) { startGesture (g, seconds); idleBusy = seconds; k.lastPiece = piece; };
                if (in.bypass)                              play (Gesture::sip, 2.4f, -1);
                else if (! lull)
                {
                    // A hand off the pole for a moment: the headphones, the hat, his brow.
                    if (weather > 0.6f && pick < 0.5f)      play (Gesture::wipe, 1.5f, -1);
                    else if (pick < 0.4f)                   play (Gesture::tap, 1.9f, -1);
                    else if (pick < 0.75f)                  play (Gesture::hat, kHatSeconds, -1);
                    else                                    play (Gesture::wipe, 1.5f, -1);
                }
                else if (! k.shouldered)
                {
                    // First the pole goes up onto his shoulder, and he has a look round. It stays
                    // there until there is work again.
                    play (Gesture::shoulder, 1.6f, 0);
                    k.shouldered = true;
                }
                else
                {
                    // Then whatever he did not do last: coil some cable, see to the windshield, a
                    // yawn and a stretch, nod off on the pole, hold the microphone out to the frog
                    // or a turtle and listen, tap the headphones, set his hat, wipe his brow.
                    const float frogAt = frog.x / std::max (0.1f, keeperToStream), logAt = (logX + 0.3f * fg) / std::max (0.1f, keeperToStream);
                    const bool frogNear = frog.state == 0 && (frogAt - k.x) * k.facing > 0.7f && std::abs (frogAt - k.x) < 4.0f && in.focus < 0;
                    const bool logNear = (logAt - k.x) * k.facing > 0.0f && std::abs (logAt - k.x) < 4.5f && asleep < 0.5f && in.focus < 0;
                    int piece = 1 + std::min (7, (int) (pick * 8.0f));
                    if (piece == k.lastPiece)
                        piece = 1 + piece % 8;
                    if (piece == 5 && ! frogNear && ! logNear)
                        piece = 6;
                    switch (piece)
                    {
                        case 1:  play (Gesture::coil, 2.4f, 1); break;
                        case 2:  play (Gesture::polish, 1.8f, 2); break;
                        case 3:  play (Gesture::stretch, 2.6f, 3); break;
                        case 4:  play (Gesture::doze, 3.0f, 4); break;
                        case 5:
                            play (Gesture::offer, kOfferSeconds, 5);
                            offerFrog = frogNear;
                            offerX = frogNear ? frogAt : logAt;
                            if (! frogNear)
                            {
                                turtles[1].stretchDelay = 0.5f;               // the turtles crane up at it
                                turtles[2].stretchDelay = 0.8f;
                            }
                            break;
                        case 6:  play (Gesture::tap, 2.6f, 6); break;
                        case 7:  play (Gesture::hat, kHatSeconds, 7); break;
                        default: play (Gesture::wipe, 1.5f, 8); break;
                    }
                }
                k.idleGesture = true;
                break;
            }
            case 1:
            {
                const float pick = rnd();
                if (beaver.upright > 0.9f && dam > 0.22f && ! beaver.patting && beaver.proudT <= 0.0f && pick < 0.7f)
                {
                    startPats (in.frame.activity > 0.08f ? 1 + std::min (1, (int) (rnd() * 2.0f)) : 1);      // a pat or two, and a short look of pride
                    beaver.idlePat = true;
                    idleBusy = beaver.patLen * (float) beaver.patCount + 0.9f;
                }
                else if (beaver.upright > 0.9f)
                {
                    beaver.wave = 0.0f;
                    idleBusy = 2.0f;
                }
                else if (beaver.upright < 0.05f && pick < 0.45f)
                {
                    beaver.flop = 0.0f;                                  // floating: a lazy slap of the tail
                    idleBusy = 1.1f;
                }
                else
                {
                    beaver.ear = 0.0f;
                    beaver.blink.t = 0.0f;
                    idleBusy = 0.6f;
                }
                break;
            }
            case 2:
                if (rnd() < 0.55f) { frog.puff = 0.0f; frog.blink.t = 0.0f; idleBusy = 1.1f; }
                else               { frog.tongue = 0.0f; idleBusy = 0.6f; }
                break;
            case 3:
            {
                const float pick = rnd();
                auto& t = turtles[(size_t) std::min (2, (int) (rnd() * 3.0f))];
                if (pick < 0.4f)      { t.stretch = 0.0f; idleBusy = 2.4f; }
                else if (pick < 0.7f) { t.lookTo = t.lookTo > 0.0f ? rnd (-0.7f, -0.2f) : rnd (0.2f, 0.7f); t.blink.t = 0.0f; idleBusy = 1.2f; }
                else
                {
                    // All three, one after the other along the log.
                    for (int i = 0; i < 3; ++i)
                        turtles[(size_t) i].stretchDelay = 0.45f * (float) i;
                    idleBusy = 3.3f;
                }
                break;
            }
            case 4:
                geese[(size_t) preener].preen = 0.0f;
                idleBusy = 2.4f;
                break;
            case 5:
                dabble = 0.0f;
                idleBusy = 1.9f;
                break;
            case 6:
                gust = 0.0f;
                idleBusy = 3.2f;
                break;
            default: break;
        }
    }

    //------------------------------------------------------------------------------ the hat
    // A treat, not a loop: once in a long while a goose he is shooing snatches his hat and makes
    // off along the bank with it on its own head. He gives chase and gets it back.
    void stepHat (const WorldInput& in, float dt, bool anyGoose)
    {
        const bool idleNow = in.bypass || in.diff || in.soloHonk || ! anyGoose;
        if (hat.state == 0)
        {
            if (keeper.shoo > 0.8f && ! idleNow)
                hat.wait -= dt;
            if (hat.wait > 0.0f || idleNow || keeper.trip < 1.0f || keeper.gesture != Gesture::none || keeper.bit != Bit::none || keeper.cue > 0.0f
                || keeper.bigCool > 0.0f || frog.state == 1 || frog.state == 2 || fish.t < kFishLeap)
                return;
            // Only a goose standing on its own will do, so that the hat can be seen on its head,
            // and not in a crowd.
            int onBank = 0;
            for (const auto& g : geese)
                onBank += g.used && ! g.leaving ? 1 : 0;
            if (onBank > 4)
                return;
            int best = -1;
            for (int i = 0; i < kMaxGeese; ++i)
            {
                const auto& g = geese[(size_t) i];
                if (! g.used || g.leaving || g.landed <= 0.8f || std::abs (g.x - keeper.x) >= 2.8f || ! gooseActive (g)
                    || g.knock < 1.0f || g.knockX < 0.0f || g.knockX > 0.0f)
                    continue;
                bool alone = true;
                for (int j = 0; j < kMaxGeese; ++j)
                    if (j != i && geese[(size_t) j].used && ! geese[(size_t) j].leaving && std::abs (geese[(size_t) j].x - g.x) < 1.2f)
                        alone = false;
                if (alone && (best < 0 || i == hat.prefer || (best != hat.prefer && g.depth > geese[(size_t) best].depth)))
                    best = i;
            }
            if (best < 0)
                return;
            // About once in a minute or two of busy work; sooner when a goose he cannot shift
            // takes it as its answer (see chooseBit).
            hat = { 1, 0.0f, rnd (60.0f, 120.0f), best, -1, false, {} };
            keeper.bigCool = 12.0f;
            keeper.rung = std::min (keeper.rung, 1);
            auto& g = geese[(size_t) best];
            g.thief = true;
            g.fleeX = g.x;
            g.flap = 1.0f;
            g.hiss = 0.0f;
            return;
        }

        moving = true;
        auto& g = geese[(size_t) hat.goose];
        const float before = hat.t / kF;
        hat.t += dt;
        const float f = hat.t / kF;
        const float away = g.x >= keeper.x ? 1.0f : -1.0f;
        const bool lost = idleNow || ! g.used || g.leaving;
        auto handBack = [&]
        {
            // Back it comes by itself (Bypass, or the goose has gone).
            hat.state = 3; hat.t = 0.0f;
            g.thief = false;
            g.squash.v = 2.0f;
            g.flap = 0.0f;                                   // put out at losing it
        };
        if (hat.state == 1 && hat.t > 0.5f)
        {
            hat.state = 2; hat.t = 0.0f;
            // Off it goes: to an open stretch of bank where the hat can be seen on its head, away
            // from him if it can, never past him, never in among the others, and never so close
            // that he hides it.
            float bestTo = g.x, bestScore = -1.0e9f;
            for (float dir : { -1.0f, 1.0f })
                for (float far : { 4.5f, 3.4f, 2.4f, 1.6f })
                {
                    const float to = std::clamp (g.x + dir * far, keeperLo - 1.0f, keeperHi + 1.0f);
                    float score = 0.6f * std::min (3.0f, std::abs (to - g.x)) + (dir * away > 0.0f ? 0.8f : 0.0f);
                    if ((keeper.x - g.x) * dir > 0.0f && (keeper.x - to) * dir < 0.0f)
                        score -= 3.0f;
                    if (std::abs (to - keeper.x) < 1.2f)
                        score -= 4.0f;
                    for (const auto& o : geese)
                        if (&o != &g && o.used && ! o.leaving)
                            score -= std::abs (o.x - to) < 0.9f ? 2.5f : (o.x - g.x) * dir > 0.0f && (o.x - to) * dir < 0.0f ? 0.4f : 0.0f;
                    if (score > bestScore) { bestScore = score; bestTo = to; }
                }
            // Has he room to run after it? If not (it has not gone far, or his stretch of bank
            // ends here), he stays where he is and fetches the hat back with the pole.
            const float stand = std::clamp (bestTo + (keeper.x < bestTo ? -0.8f : 0.8f), keeperLo, keeperHi);
            hat.reach = std::abs (stand - keeper.x) < 1.2f || std::abs (bestTo - keeper.x) < 1.5f;
            if (hat.reach)
            {
                const float side = bestTo >= keeper.x ? 1.0f : -1.0f;
                bestTo = keeper.x + side * std::clamp (std::abs (bestTo - keeper.x), 1.7f, 2.2f + 0.8f * cast);
            }
            g.fleeX = bestTo;
            g.wag = 1.0f;
        }
        else if (hat.state == 2 && lost)
            handBack();
        else if (hat.state == 2 && hat.reach)
        {
            // He has had his start; the goose has got where it was going. Out goes the pole.
            if (f > 24.0f && (std::abs (g.x - g.fleeX) < 0.15f || hat.t > 1.6f))
            {
                hat.state = 4; hat.t = 0.0f;
                keeper.tip.v -= 1.0f;
            }
        }
        else if (hat.state == 2 && (hat.t > 3.5f || (hat.t > 1.2f && std::abs (g.x - keeper.x) < 1.8f)))
        {
            // He has caught it up (or run out of patience within a pole's length of it): the pole
            // goes out for his hat. Too far off even for that, it comes back by itself.
            if (std::abs (g.x - keeper.x) < 2.2f + 0.8f * cast)
            {
                hat.state = 4; hat.t = 0.0f;
                keeper.tip.v -= 1.0f;
            }
            else
                handBack();
        }
        else if (hat.state == 4)
        {
            if (lost && f < 10.0f)
                handBack();
            else
            {
                if (before < 10.0f && f >= 10.0f)
                {
                    // Hooked off its head on the end of the pole.
                    g.thief = false;
                    g.squash.v = 2.4f;
                    g.flap = 0.0f;
                    g.wag = 1.0f;
                }
                if (f < 14.0f)
                    hat.from = micAt + Vec3 { 0.0f, 0.2f * cast, 0.0f };          // on the pole's end, until the flick lets it go
                if (f >= 28.0f)
                {
                    hat.state = 0; hat.t = 0.0f;
                    keeper.hatLand = 0.0f;                                         // it lands with a squash, and he sets it straight
                    keeper.hatHop.v = 1.2f;
                    keeper.adjustNext = true;
                }
            }
        }
        else if (hat.state == 3 && hat.t > 0.6f)
        {
            hat.state = 0; hat.t = 0.0f;
            g.thief = false;
            keeper.hatLand = 0.0f;
            keeper.adjustNext = true;
        }
    }

    //------------------------------------------------------------------------------ the bits
    // Each bit is written as curves over its frames (sixtieths of a second), for him and for the
    // goose: changes to the pose each already has, which start and end at nothing. Being functions
    // of the time alone, they look the same at any frame rate and can be faded out at any moment.
    static float ramp (float f, float a, float b) noexcept { return smooth01 ((f - a) / (b - a)); }
    static float pulse (float f, float a, float b) noexcept { return bump ((f - a) / (b - a)); }
    /** 0 until frame a, then up to 1 with one overshoot and a settle, over about n frames. */
    static float sprung (float f, float a, float n) noexcept
    {
        const float u = (f - a) / n;
        return u <= 0.0f ? 0.0f : 1.0f - std::exp (-4.5f * u) * std::cos (7.5f * u);
    }

    /** 0 until frame a, then a wobble that dies away over about n frames (it peaks near 0.55). */
    static float wobble (float f, float a, float n) noexcept
    {
        const float u = (f - a) / n;
        return u <= 0.0f ? 0.0f : std::exp (-4.5f * u) * std::sin (7.5f * u);
    }

    struct BitPose
    {
        float pole = 0;        // added to the pole's angle
        float lean = 0;        // his body, forward (+) or back along the bank
        float sq = 0;          // squash (+) or stretch
        float crouch = 0, hipX = 0, hop = 0, footLift = 0, backLift = 0;
        float spin = 0;        // turned round, towards us
        float back = 0;        // his hands drawn back along the pole
        float level = 0;       // 1 = the pole brought down level, like a lance
        float plant = 0;       // 1 = the pole stood upright beside him
        float aim = 0;         // 1 = the microphone goes to the goose's beak
        float draw = 0, lift = 0;   // the microphone drawn back towards him, and up (in his own sizes)
        float flail = 0;       // 1 = his free hand out for balance
        float loop = 0;        // 1 = the cable round his back boot
        float headYaw = 0, shut = 0;   // his head turned (towards us +), his eyes narrowed
        float hatLift = 0, hatDown = 0, cups = 0, nod = 0, wide = 0, mouth = 0, fan = 0;
        float shift = 0;       // where he has got to along the bank, towards the goose
    };

    static BitPose bitPoseAt (Bit b, float f, int var) noexcept
    {
        BitPose p;
        switch (b)
        {
            case Bit::shoo:
            {
                // A jab at it. He draws the pole back and sinks; steps in and puts the microphone
                // just short of its beak; again, smaller; holds it there; and steps back, the
                // pole's end springing up.
                const float ant = ramp (f, 0.0f, 8.0f) - ramp (f, 8.0f, 11.0f);
                const float jab = ramp (f, 8.0f, 12.0f) - 0.65f * ramp (f, 12.0f, 16.0f) + 0.35f * ramp (f, 16.0f, 20.0f) - 0.7f * ramp (f, 26.0f, 38.0f);
                p.back = 0.2f * ant - 0.12f * jab;
                p.draw = 0.5f * ant;
                p.lift = 0.22f * ant;
                p.aim = 0.92f * jab;
                p.lean = -0.15f * ant + 0.3f * jab;
                p.crouch = 0.06f * ant + 0.04f * jab;
                p.hipX = -0.04f * ant + 0.07f * jab;
                if (var == 0)
                {
                    p.footLift = 0.06f * pulse (f, 2.0f, 12.0f);
                    p.backLift = 0.05f * pulse (f, 28.0f, 40.0f);
                    p.shift = 0.25f * (ramp (f, 8.0f, 12.0f) - ramp (f, 28.0f, 40.0f));
                }
                else
                {
                    // Too close to it for the pole: a hop back first, to make room.
                    p.backLift = 0.07f * pulse (f, 0.0f, 7.0f);
                    p.footLift = 0.06f * pulse (f, 2.0f, 9.0f) + 0.05f * pulse (f, 30.0f, 42.0f);
                    p.hop = 0.05f * pulse (f, 1.0f, 8.0f);
                    p.shift = -0.25f * (ramp (f, 0.0f, 8.0f) - ramp (f, 30.0f, 42.0f));
                }
                p.sq = 0.05f * ant - 0.08f * pulse (f, 8.0f, 14.0f) - 0.05f * pulse (f, 16.0f, 22.0f);
                p.pole = 0.36f * wobble (f, 27.0f, 14.0f);
                p.nod = 0.12f * jab;
                p.mouth = 0.35f * pulse (f, 8.0f, 28.0f);
                break;
            }
            case Bit::whiff:
            {
                // A full wind-up (short, middling or long), a beat at the top, the stroke. Then
                // one of three things: the miss carries him a quarter turn round, and he looks at
                // the pole and at the goose; or it takes him right round and leaves him dizzy; or
                // the pole's end goes into the reeds and he has to pull it out.
                const float wind = whiffWind (var), h = f - (wind - 20.0f);
                const int ending = var & 3;
                const float up = ramp (f, 0.0f, wind), stroke = ramp (h, 24.0f, 29.0f);
                p.sq = 0.08f * up * (1.0f - stroke) - 0.1f * pulse (h, 24.0f, 32.0f);
                p.footLift = 0.07f * up * (1.0f - stroke);
                p.fan = h > 24.5f && h < 28.6f ? 1.0f : 0.0f;
                if (ending == 0)
                {
                    p.pole = 0.8f * up - 1.1f * stroke + 0.3f * sprung (h, 29.0f, 22.0f);
                    p.spin = 1.3f * (ramp (h, 25.0f, 38.0f) - ramp (h, 58.0f, 80.0f));
                    p.lean = -0.22f * up + 0.5f * stroke - 0.28f * ramp (h, 30.0f, 46.0f);
                    p.hipX = -0.08f * up + 0.16f * stroke - 0.08f * ramp (h, 30.0f, 46.0f);
                    p.nod = -0.22f * pulse (h, 44.0f, 60.0f) + 0.16f * pulse (h, 58.0f, 78.0f);
                    p.mouth = 0.5f * pulse (h, 40.0f, 78.0f);
                    p.wide = pulse (h, 28.0f, 56.0f);
                }
                else if (ending == 1)
                {
                    // Right round, once; then his head goes round twice by itself.
                    const float dizzy = ramp (h, 46.0f, 50.0f) - ramp (h, 68.0f, 76.0f), a = (h - 48.0f) * 0.62f;
                    p.pole = 0.8f * up - 1.1f * stroke + 0.3f * sprung (h, 29.0f, 22.0f) - 0.2f * dizzy + 0.16f * std::sin (a + 1.0f) * dizzy;
                    p.spin = 6.2831853f * ramp (h, 25.0f, 47.0f);
                    p.lean = -0.22f * up + 0.5f * stroke - 0.28f * ramp (h, 30.0f, 46.0f) + 0.15f * std::cos (a) * dizzy;
                    p.hipX = -0.08f * up + 0.16f * stroke - 0.08f * ramp (h, 30.0f, 46.0f) + 0.09f * std::sin (a) * dizzy;
                    p.headYaw = 0.55f * std::sin (a) * dizzy + 0.5f * dizzy;
                    p.nod = 0.32f * std::cos (a) * dizzy;
                    p.crouch = 0.05f * dizzy;
                    p.footLift = p.footLift + 0.06f * pulse (h, 52.0f, 60.0f);
                    p.backLift = 0.06f * pulse (h, 62.0f, 70.0f);
                    p.flail = 0.7f * dizzy;
                    p.shut = 0.55f * dizzy;
                    p.mouth = 0.5f * pulse (h, 30.0f, 80.0f);
                    p.wide = pulse (h, 26.0f, 46.0f);
                    p.backLift += 0.06f * pulse (h, 78.0f, 90.0f);
                }
                else
                {
                    // Into the reeds. Two pulls; the second frees it, and he goes back a step.
                    const float t1 = pulse (h, 36.0f, 44.0f), t2 = pulse (h, 46.0f, 54.0f), out = ramp (h, 52.0f, 56.0f);
                    p.pole = 0.8f * up - 1.3f * stroke + 0.5f * sprung (h, 52.0f, 18.0f) + 0.05f * (t1 + t2) * (1.0f - out);
                    p.back = 0.14f * (t1 + t2);
                    p.lean = -0.22f * up + 0.5f * stroke - 0.1f * ramp (h, 30.0f, 36.0f) - 0.18f * (t1 + t2) - 0.18f * out - 0.26f * pulse (h, 52.0f, 76.0f);
                    p.hipX = -0.08f * up + 0.16f * stroke - 0.05f * (t1 + t2) - 0.08f * ramp (h, 52.0f, 60.0f);
                    p.crouch = 0.04f * (t1 + t2);
                    p.shift = -0.22f * ramp (h, 53.0f, 63.0f);
                    p.hop = 0.05f * pulse (h, 53.0f, 63.0f);
                    p.backLift = 0.09f * pulse (h, 52.0f, 62.0f);
                    p.flail = ramp (h, 52.0f, 55.0f) - ramp (h, 64.0f, 72.0f);
                    p.nod = 0.14f * (ramp (h, 30.0f, 36.0f) - ramp (h, 52.0f, 58.0f));
                    p.mouth = 0.5f * pulse (h, 34.0f, 74.0f);
                    p.wide = pulse (h, 29.0f, 40.0f) + pulse (h, 52.0f, 70.0f);
                }
                break;
            }
            case Bit::tug:
            {
                // The goose has the windshield in its beak, and the pole is taut between them.
                // Three tugs: each pulls him forward and drags him a step, and each time he digs
                // his heels in and leans back further. It lets go: he goes back three steps with
                // the pole waving, stands a moment looking at the microphone, and has a closer look.
                const float let = 1.0f - ramp (f, 58.0f, 61.0f);
                float pull = 0.0f, heave = 0.0f, drag = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    const float a = 14.0f + 14.0f * (float) i;
                    pull += pulse (f, a, a + 7.0f);
                    heave += ramp (f, a + 5.0f, a + 12.0f);
                    drag += ramp (f, a, a + 7.0f);
                }
                const float stag = ramp (f, 58.0f, 62.0f) - ramp (f, 80.0f, 88.0f);
                p.aim = ramp (f, 3.0f, 11.0f) * let;
                p.lean = 0.22f * pull * let - 0.14f * heave * let - 0.3f * pulse (f, 58.0f, 90.0f);
                p.hipX = -0.05f * heave * let;
                p.crouch = 0.035f * heave * let;
                p.back = 0.05f * heave * let;
                p.shift = 0.15f * drag - 0.2f * (ramp (f, 60.0f, 68.0f) + ramp (f, 68.0f, 76.0f) + ramp (f, 76.0f, 84.0f));
                p.hop = 0.05f * (pulse (f, 60.0f, 68.0f) + pulse (f, 68.0f, 76.0f) + pulse (f, 76.0f, 84.0f));
                p.backLift = 0.1f * (pulse (f, 60.0f, 68.0f) + pulse (f, 76.0f, 84.0f));
                p.footLift = 0.1f * pulse (f, 68.0f, 76.0f);
                p.flail = stag;
                p.pole = stag * (0.45f + 0.35f * std::sin ((f - 58.0f) * 0.62f)) + 0.4f * (ramp (f, 98.0f, 104.0f) - ramp (f, 116.0f, 126.0f));
                p.nod = -0.2f * (ramp (f, 84.0f, 90.0f) - ramp (f, 116.0f, 126.0f));
                p.mouth = 0.6f * pulse (f, 8.0f, 90.0f);
                p.wide = pulse (f, 6.0f, 70.0f);
                break;
            }
            case Bit::lunge:
            {
                // A deep crouch with the pole drawn back like a lance, and a lunge that goes under
                // the goose as it hops. Then his cable has his boot (see stepKeeper): when he has
                // his feet again he hops twice more on the free one, looks down at it, kicks it
                // off, and gives the goose a look. Or he skids: arms going at the end of it, a
                // rock back on his heels, and a breath out.
                const float wind = ramp (f, 0.0f, 16.0f), go = ramp (f, 16.0f, 24.0f), over = ramp (f, 26.0f, 40.0f);
                p.crouch = 0.16f * wind * (1.0f - go);
                p.back = 0.3f * wind - 0.55f * go + 0.25f * over;
                p.level = wind * (1.0f - over);
                p.lean = -0.1f * wind + 0.45f * go - 0.35f * over;
                p.sq = 0.1f * wind * (1.0f - ramp (f, 16.0f, 18.0f)) - 0.15f * pulse (f, 16.0f, 28.0f);
                p.shift = 1.2f * ramp (f, 16.0f, 27.0f);
                p.wide = pulse (f, 14.0f, 42.0f);
                if (var != 0)
                {
                    const float dig = pulse (f, 25.0f, 50.0f), arms = ramp (f, 36.0f, 40.0f) - ramp (f, 54.0f, 60.0f), puff = pulse (f, 70.0f, 90.0f);
                    p.lean += -0.3f * dig + 0.2f * arms - 0.2f * pulse (f, 56.0f, 72.0f);
                    p.hipX = -0.1f * dig;
                    p.crouch += 0.08f * dig;
                    p.flail = arms;
                    p.pole = 0.5f * std::sin ((f - 38.0f) * 0.7f) * arms;
                    p.footLift = 0.05f * pulse (f, 56.0f, 70.0f);
                    p.sq += 0.07f * puff;
                    p.shut = 0.8f * puff;
                    p.mouth = 0.5f * pulse (f, 26.0f, 60.0f) + 0.35f * puff;
                }
                else
                {
                    const float hops = pulse (f, 78.0f, 88.0f) + pulse (f, 88.0f, 98.0f), hopping = ramp (f, 74.0f, 80.0f) - ramp (f, 96.0f, 102.0f);
                    const float down = ramp (f, 98.0f, 105.0f) - ramp (f, 118.0f, 124.0f), glare = ramp (f, 126.0f, 132.0f) - ramp (f, 146.0f, 158.0f);
                    const float kick = pulse (f, 118.0f, 128.0f);
                    p.loop = ramp (f, 24.0f, 27.0f) - ramp (f, 121.0f, 125.0f);
                    p.hop = 0.12f * hops + 0.05f * kick;
                    p.backLift = 0.2f * hopping + 0.24f * kick;
                    p.flail = hopping;
                    p.pole = 0.35f * std::sin ((f - 78.0f) * 0.63f) * hopping - 0.3f * down + 0.12f * std::sin ((f - 130.0f) * 0.9f) * glare;
                    p.sq += 0.08f * (pulse (f, 86.0f, 90.0f) + pulse (f, 96.0f, 101.0f));
                    p.nod = 0.5f * down + 0.22f * glare;
                    p.lean += -0.12f * hopping + 0.24f * down - 0.16f * kick + 0.2f * glare;
                    p.crouch += 0.07f * down;
                    p.hipX = 0.05f * glare - 0.05f * down;
                    p.shut = 0.5f * glare;
                    p.mouth = 0.4f * pulse (f, 76.0f, 100.0f);
                }
                break;
            }
            case Bit::win:
            {
                // The biggest wind-up, weight on his back foot and the front one off the ground; a
                // beat; the stroke; everything stops for three frames as it lands; then the pole's
                // weight takes him right round and he plants it. (The fist pump follows.)
                const float up = ramp (f, 0.0f, 24.0f), stroke = ramp (f, 30.0f, 34.0f), rec = ramp (f, 37.0f, 50.0f);
                p.pole = 1.0f * up - 1.45f * stroke + 0.45f * rec;
                p.lean = -0.28f * up + 0.62f * stroke - 0.34f * rec;
                p.hipX = -0.1f * up + 0.18f * stroke - 0.08f * rec;
                p.footLift = 0.16f * up * (1.0f - ramp (f, 28.0f, 32.0f));
                p.sq = 0.1f * up * (1.0f - stroke) - 0.12f * pulse (f, 30.0f, 40.0f);
                p.spin = 6.2831853f * ramp (f, 37.0f, 57.0f);
                p.plant = ramp (f, 46.0f, 57.0f) - ramp (f, 100.0f, 114.0f);
                p.fan = f > 30.3f && f < 34.3f ? 1.0f : 0.0f;
                p.wide = pulse (f, 16.0f, 40.0f);
                break;
            }
            case Bit::gooseWins:
            {
                // The goose hops onto the end of the pole (it dips and springs), walks up it, and
                // steps onto his hat, which comes down over his eyes. He freezes, starts, and sags.
                const float on = ramp (f, 24.0f, 30.0f) - ramp (f, 68.0f, 76.0f), sag = ramp (f, 96.0f, 112.0f) - ramp (f, 156.0f, 170.0f);
                p.pole = -0.3f * pulse (f, 22.0f, 34.0f) - 0.14f * on + 0.03f * std::sin (f * 0.48f) * on - 0.14f * sag;
                p.hatDown = ramp (f, 73.0f, 78.0f) - ramp (f, 160.0f, 174.0f);
                p.sq = 0.1f * pulse (f, 73.0f, 82.0f) - 0.12f * pulse (f, 86.0f, 96.0f);
                p.crouch = 0.07f * sag;
                p.lean = 0.08f * sag - 0.12f * pulse (f, 14.0f, 40.0f);
                p.wide = pulse (f, 12.0f, 60.0f);
                p.mouth = 0.6f * pulse (f, 14.0f, 72.0f) + 0.4f * pulse (f, 100.0f, 160.0f);
                break;
            }
            case Bit::honk:
            {
                // It steps in, neck right up, and he bends down to it: the honk catches him leaning
                // in. Hat up, headphones out, and he leans well away.
                const float in = ramp (f, 3.0f, 10.0f) - ramp (f, 11.0f, 14.0f), take = ramp (f, 11.0f, 16.0f) - ramp (f, 28.0f, 46.0f);
                p.lean = 0.16f * in - 0.3f * take;
                p.hipX = 0.03f * in - 0.06f * take;
                p.nod = 0.14f * in - 0.08f * take;
                p.pole = 0.2f * take;
                p.hatLift = 0.11f * pulse (f, 11.0f, 26.0f);
                p.cups = pulse (f, 11.0f, 24.0f);
                p.sq = -0.05f * pulse (f, 11.0f, 19.0f);
                p.wide = pulse (f, 10.0f, 42.0f);
                p.mouth = pulse (f, 11.0f, 48.0f);
                break;
            }
            case Bit::none:
                break;
        }
        return p;
    }

    /** The goose's side of a bit, as changes to how it stands. */
    struct GooseBit
    {
        float dx = 0;          // along the bank, away from him (in units)
        float dy = 0;          // off the ground (in its own heights)
        float sq = 0, honk = 0, flap = 0, tumble = 0;
        float low = 0;         // 1 = flat to the ground, neck down
        float bite = 0;        // 1 = neck out and up, beak shut on the end of the windshield
        float lean = 0;        // its whole body tipped back, away from him
        float tall = 0;        // 1 = neck straight up, as tall as it can make itself
        float ride = 0, u = 0; // on him: how far it has left the ground for the pole; 0 the pole's end .. 1 by his hands .. 2 on his hat
    };

    static GooseBit gooseBitAt (Bit b, float f, float room, int var) noexcept
    {
        GooseBit g;
        switch (b)
        {
            case Bit::shoo:
            {
                // It leans back from the first jab; hops aside at the second, with a squash as it
                // lands; and has one honk at the microphone.
                const float h = pulse (f, 17.0f, 29.0f);
                g.lean = 0.3f * (ramp (f, 9.0f, 13.0f) - ramp (f, 14.0f, 19.0f));
                g.tall = 0.6f * pulse (f, 8.0f, 20.0f);
                g.dx = 0.35f * ramp (f, 17.0f, 29.0f);
                g.dy = 0.26f * h;
                g.flap = 0.5f * h;
                g.sq = 0.12f * pulse (f, 12.0f, 18.0f) - 0.1f * h;
                g.honk = pulse (f, 34.0f, 48.0f);
                break;
            }
            case Bit::whiff:
            {
                // It ducks just before the stroke, and pops up taller than before to honk.
                const float h = f - (whiffWind (var) - 20.0f);
                g.low = ramp (h, 22.0f, 28.0f) - ramp (h, 45.0f, 49.0f);
                g.sq = 0.33f * g.low - 0.1f * pulse (h, 47.0f, 64.0f);
                g.honk = pulse (h, 49.0f, 63.0f);
                break;
            }
            case Bit::tug:
            {
                // A step back to take up the slack; then it throws its whole weight back, three
                // times, feet sliding; lets go; and sits down hard.
                float pull = 0.0f, slide = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    const float a = 14.0f + 14.0f * (float) i;
                    pull += pulse (f, a, a + 12.0f);
                    slide += ramp (f, a, a + 7.0f);
                }
                g.bite = ramp (f, 1.0f, 9.0f) - ramp (f, 58.0f, 61.0f);
                g.lean = 0.25f * pull * (1.0f - ramp (f, 57.0f, 60.0f));
                g.dx = 0.3f * ramp (f, 0.0f, 10.0f) + 0.1f * slide;
                g.flap = 0.35f * pull;
                g.sq = 0.3f * pulse (f, 58.0f, 72.0f);
                g.honk = pulse (f, 100.0f, 116.0f);
                break;
            }
            case Bit::lunge:
            {
                // Straight up, a beat at the top, and down where it was. (When his cable has him
                // it watches, and honks once at the look he gives it.)
                const float up = std::clamp ((f - 14.0f) / 12.0f, 0.0f, 1.0f), down = std::clamp ((f - 30.0f) / 12.0f, 0.0f, 1.0f);
                g.dy = 1.15f * (1.0f - (1.0f - up) * (1.0f - up) - down * down);
                g.flap = 0.9f * (ramp (f, 13.0f, 17.0f) - ramp (f, 40.0f, 44.0f));
                g.sq = 0.16f * pulse (f, 6.0f, 15.0f) - 0.12f * pulse (f, 15.0f, 27.0f);
                if (var == 0)
                {
                    g.tall = 0.7f * (ramp (f, 48.0f, 56.0f) - ramp (f, 124.0f, 132.0f));
                    g.honk = pulse (f, 132.0f, 148.0f);
                }
                break;
            }
            case Bit::win:
            {
                // It cowers; then it is sent along the bank, two bounces, and it is off.
                if (f < 34.0f)
                {
                    g.low = 0.35f * ramp (f, 22.0f, 32.0f);
                    g.sq = 0.1f * g.low;
                    break;
                }
                const float u = std::clamp ((f - 37.0f) / 36.0f, 0.0f, 1.0f);
                g.dx = room * (1.0f - (1.0f - u) * (1.0f - u * 0.5f));
                const float v = u < 0.5f ? u / 0.5f : u < 0.8f ? (u - 0.5f) / 0.3f : (u - 0.8f) / 0.2f;
                g.dy = (u < 0.5f ? 1.0f : u < 0.8f ? 0.45f : 0.2f) * 4.0f * v * (1.0f - v);
                g.tumble = 12.566371f * smooth01 (u);
                g.sq = f < 37.0f ? -0.2f : 0.0f;
                g.flap = 0.75f;
                g.honk = 1.0f;
                break;
            }
            case Bit::gooseWins:
            {
                const float steps[3] = { 28.0f, 41.0f, 54.0f };
                g.sq = 0.2f * pulse (f, 0.0f, 14.0f);
                g.ride = ramp (f, 12.0f, 24.0f) - ramp (f, 156.0f, 172.0f);
                g.dy = 0.9f * pulse (f, 12.0f, 24.0f) + 0.45f * pulse (f, 68.0f, 78.0f) + 0.7f * pulse (f, 156.0f, 172.0f);
                for (float a : steps)
                {
                    g.u += ramp (f, a, a + 9.0f) / 3.0f;
                    g.dy += 0.1f * pulse (f, a, a + 9.0f);
                }
                g.u += ramp (f, 68.0f, 78.0f);
                g.flap = 0.7f * (pulse (f, 12.0f, 26.0f) + pulse (f, 66.0f, 80.0f) + pulse (f, 154.0f, 174.0f));
                g.honk = pulse (f, 114.0f, 134.0f);
                break;
            }
            case Bit::honk:
                // In close, neck straight up, and a hop as it lets go: its beak is at his chin.
                g.dx = -0.3f * (ramp (f, 0.0f, 9.0f) - ramp (f, 34.0f, 50.0f));
                g.tall = ramp (f, 0.0f, 8.0f) - ramp (f, 30.0f, 44.0f);
                g.dy = 0.3f * pulse (f, 9.0f, 22.0f);
                g.honk = pulse (f, 8.0f, 30.0f);
                g.flap = 0.45f * pulse (f, 8.0f, 26.0f);
                g.sq = 0.1f * pulse (f, 3.0f, 10.0f) - 0.1f * pulse (f, 9.0f, 24.0f);
                break;
            case Bit::none:
                break;
        }
        return g;
    }

    /** Where the tip of its beak is when it has hold of the windshield, tipped back by `lean`:
        ahead of its feet and above them, in its own sizes (see drawGoose). */
    static void biteTip (float lean, float& ahead, float& up) noexcept
    {
        const float a = 1.37f, u = 1.34f;
        ahead = a * std::cos (lean) - u * std::sin (lean);
        up = a * std::sin (lean) + u * std::cos (lean);
    }

    static float gooseSizeFor (float depth, float crowd, float cast) noexcept { return (1.0f + 0.07f * std::min (8.0f, depth)) * cast * (1.0f - 0.16f * crowd); }
    float gooseSize (const Goose& g) const noexcept { return gooseSizeFor (g.depth, g.crowd, cast); }

    /** The rest between two bits, in seconds, for how hard the mix is working him. */
    float bitGap() const noexcept { return lerp (7.0f, 1.4f, std::clamp (keeper.heat, 0.0f, 1.0f)); }

    /** True while a bit is in its action: nothing else big may start (a leap of the frog or the
        fish waits its turn, and the hat and the cable leave him alone). */
    bool bitActing() const noexcept
    {
        return keeper.cue > 0.0f || (keeper.bit != Bit::none && ! keeper.bitCut && keeper.bt < bitActionFrames (keeper.bit, keeper.bitVar) * kF);
    }

    /** May a bit with this goose start now? */
    bool canStartBit (const WorldInput& in, const Goose& g) const noexcept
    {
        const auto& k = keeper;
        return k.bit == Bit::none && ! in.calm && ! in.bypass && ! in.diff && ! in.soloHonk && hat.state == 0 && k.trip >= 1.0f && k.clickWait <= 0.0f
               && g.used && ! g.leaving && ! g.thief && g.arrive >= 1.0f && g.landed > 1.2f && g.knock >= 1.0f && ! (g.knockX < 0.0f || g.knockX > 0.0f)
               && g.flap >= 1.0f && g.preen >= 1.0f
               && frog.state != 1 && frog.state != 2 && fish.t >= kFishLeap && fish2.t >= kFishLeap;
    }

    /** How far a lunge takes him: most of the way to the goose, and never right up to it. */
    float lungeReach (const Goose& g) const noexcept { return std::clamp (std::abs (g.x - keeper.x) - 0.8f, 0.7f, 1.2f); }

    Bit chooseBit (const WorldInput& in, const Goose& g) noexcept
    {
        auto& k = keeper;
        const float share = g.depth / std::max (1.0f, in.maxCutDb);
        // The top of the ladder, or a cut that will not come off the ceiling: a result is due.
        // Mostly the goose's; one time in four it settles for his hat (see stepHat).
        if ((k.rung >= 3 || k.ceilingT > 6.0f) && k.bigCool <= 0.0f)
        {
            if (k.rung >= 3 && rnd() < 0.25f)
            {
                hat.wait = 0.0f;
                hat.prefer = k.target;
                k.rung = 1;
                return Bit::none;
            }
            if (k.gooseWinCool <= 0.0f)
                return Bit::gooseWins;
        }
        const float reach = lungeReach (g);
        const bool room = std::abs (g.x - k.x) >= 1.5f && k.x + k.facing * reach > keeperLo && k.x + k.facing * reach < keeperHi && k.bigCool <= 0.0f;
        Bit pool[3] {};
        int n = 0;
        auto add = [&] (Bit b, bool ok) { if (ok && b != k.lastBit && n < 3) pool[n++] = b; };
        if (k.rung <= 0)
            add (Bit::shoo, true);
        else if (k.rung >= 2)
        {
            add (Bit::tug, share > 0.5f && std::abs (g.x - k.x) >= 1.8f && std::abs (g.x - k.x) <= 2.6f);     // with room for the pole to be taut between them
            add (Bit::lunge, room);
        }
        if (n == 0)
            add (Bit::whiff, true);
        if (n == 0)
            add (Bit::shoo, true);
        return pool[std::min (n - 1, (int) (rnd() * (float) n))];
    }

    void startBit (Bit b, int index) noexcept
    {
        auto& k = keeper;
        const auto& g = geese[(size_t) index];
        k.bit = b; k.bt = 0.0f; k.bitW = 1.0f; k.bitCut = false; k.cue = 0.0f;
        k.bitGoose = index;
        k.bitDir = g.x >= k.x ? 1.0f : -1.0f;
        k.bitX = k.x;
        k.bitVar = 0;
        if (b == Bit::lunge)
            k.bitVar = rnd() < 0.34f ? 1 : 0;
        else if (b == Bit::whiff)
        {
            // Never the ending he had last time, and the wind-up short, middling or long, so that
            // the stroke does not always fall on the same beat.
            int ending = std::min (2, (int) (rnd() * 3.0f));
            if (ending == k.lastWhiff)
                ending = (ending + 1 + (rnd() < 0.5f ? 1 : 0)) % 3;
            if (k.lastWhiff < 0)
                ending = 0;
            k.lastWhiff = ending;
            k.bitVar = ending | (std::min (2, (int) (rnd() * 3.0f)) << 2);
        }
        k.bitReach = lungeReach (g);
        k.bitScale = b == Bit::lunge ? k.bitReach / 1.2f : 1.0f;
        if (b == Bit::shoo)
        {
            // He jabs from a pole's length off: a step in if it is far, a hop back if it is close.
            const float step = std::clamp (std::abs (g.x - k.x) - 2.5f * std::max (cast, 0.7f), -0.5f, 0.25f);
            k.bitVar = step < 0.0f ? 1 : 0;
            k.bitScale = std::abs (step) / 0.25f;
        }
        k.bitRoom = std::clamp (k.bitDir > 0.0f ? kHalfWidth - 1.0f - g.x : g.x + kHalfWidth - 1.0f, 0.8f, 2.6f);
        k.speed = 0.0f;
        k.walking = false;
        if (bigBit (b))
            k.bigCool = 12.0f;
    }

    /** A goose due to land beside him in the middle of something big waits off the picture for it
        to be over: a second at most (a little longer while he sends one off and takes his bow),
        the way the fish waits its turn. True while it is to wait. */
    bool holdArrival (float x, float dt) noexcept
    {
        auto& k = keeper;
        const bool winOn = (k.bit == Bit::win && ! k.bitCut) || (k.lastBit == Bit::win && k.sinceBit < 0.5f);
        const bool moment = bitActing() || winOn || hat.state == 1 || hat.state == 4 || (hat.state == 2 && hat.t < 0.6f);
        const bool near = std::abs (x - k.x) < 2.5f || (k.bit != Bit::none && k.bitGoose >= 0 && std::abs (x - geese[(size_t) k.bitGoose].x) < 2.5f);
        if (! moment || ! near || k.holdArrive >= (winOn ? 1.5f : 1.0f))
        {
            if (! moment)
                k.holdArrive = 0.0f;
            return false;
        }
        k.holdArrive += dt;
        return true;
    }

    /** A bit that cannot go on (its goose has gone, Bypass, a click...) eases out from where it is. */
    void cutBit() noexcept
    {
        if (keeper.bit != Bit::none)
            keeper.bitCut = true;
    }

    void endBit() noexcept
    {
        auto& k = keeper;
        if (! k.bitCut)
        {
            auto& g = geese[(size_t) k.bitGoose];
            if ((k.bit == Bit::shoo || k.bit == Bit::tug) && g.used && ! g.leaving)
                g.x += k.bitDir * gooseBitAt (k.bit, bitFrames (k.bit, k.bitVar), k.bitRoom, k.bitVar).dx;      // where it ended up: it walks back
            if (k.bit != Bit::honk)
                k.rung = std::min (3, k.rung + 1);
            if (k.bit == Bit::win)
                k.rung = 0;
            if (k.bit == Bit::gooseWins)
            {
                k.rung = 1;
                k.gooseWinCool = 25.0f;
                k.ceilingT = 0.0f;
            }
            if (k.bit != Bit::honk)
                k.bitWait = 0.0f;
        }
        k.lastBit = k.bit;
        k.bit = Bit::none;
        k.bitW = 0.0f;
        k.bitCut = false;
        k.sinceBit = 0.0f;
        k.gapScale = rnd (0.7f, 1.3f);
    }

    /** The reeds near `x` bow over, the way `dir` (from a stroke, or a skid). */
    void bowReeds (float x, float dir) noexcept { bowX = x; bowDir = dir; bowT = 0.0f; }

    /** What a bit sets off as it passes each of its moments. */
    void bitEvents (const WorldInput&, Goose& g, float before, float f)
    {
        auto& k = keeper;
        auto at = [&] (float frame) { return before < frame && f >= frame; };
        const float size = gooseSize (g), feet = k.x + k.bitDir * 0.3f;
        switch (k.bit)
        {
            case Bit::shoo:
                if (at (12.0f)) { k.tip.v -= 1.2f; k.hatHop.v = 0.8f; addFx (2, feet, 0.0f, kKeeperZ - 0.1f, 0.6f * cast, -k.bitDir); }
                if (at (20.0f)) k.tip.v -= 0.8f;
                if (at (29.0f)) { g.squash.v = 2.2f; g.wag = 0.7f; }
                if (at (40.0f)) k.blink.t = 0.0f;
                break;
            case Bit::whiff:
            {
                const float o = whiffWind (k.bitVar) - 20.0f;
                const int ending = k.bitVar & 3;
                if (at (o + 29.0f)) { k.hatHop.v = 1.4f; bowReeds (g.x, k.bitDir); }
                if (at (o + 48.0f)) g.wag = 0.8f;
                if (at (o + 60.0f)) k.blink.t = 0.0f;
                if (ending == 1 && at (o + 47.0f))
                    addFx (2, k.x, 0.0f, kKeeperZ - 0.1f, 0.8f * cast, -k.bitDir);
                if (ending == 2)
                {
                    if (at (o + 40.0f) || at (o + 50.0f)) { bowReeds (g.x + k.bitDir * 0.4f, -k.bitDir); k.hatHop.v = 0.8f; }
                    if (at (o + 53.0f)) { k.tip.v += 2.4f; k.hatHop.v = 1.6f; }
                    if (at (o + 62.0f)) addFx (2, k.x - k.bitDir * 0.2f, 0.0f, kKeeperZ - 0.1f, 0.7f * cast, -k.bitDir);
                }
                break;
            }
            case Bit::tug:
                for (float a : { 16.0f, 30.0f, 44.0f })
                    if (at (a))
                    {
                        addFx (2, feet + k.bitDir * 0.2f, 0.0f, kKeeperZ - 0.1f, 0.8f * cast, k.bitDir);
                        addFx (2, g.x + k.bitDir * (gooseBitAt (k.bit, f, k.bitRoom, k.bitVar).dx - 0.3f * size), 0.0f, g.z - 0.05f, 0.6f * cast, -k.bitDir);
                        k.hatHop.v = 0.8f;
                    }
                if (at (59.0f)) { k.hatHop.v = 2.0f; k.tip.v += 2.5f; g.wag = 1.0f; }
                if (at (64.0f)) addFx (2, g.x + k.bitDir * 0.6f, 0.0f, g.z - 0.05f, 0.9f * cast, k.bitDir);       // it sits down hard
                if (at (68.0f) || at (76.0f) || at (84.0f)) addFx (2, k.x - k.bitDir * 0.2f, 0.0f, kKeeperZ - 0.1f, 0.6f * cast, -k.bitDir);
                if (at (92.0f) || at (110.0f)) k.blink.t = 0.0f;
                break;
            case Bit::lunge:
                if (at (17.0f)) addFx (3, k.x - k.bitDir * 0.5f, 1.25f * cast, kKeeperZ - 0.3f, 1.3f * cast, k.bitDir);
                if (at (26.0f))
                {
                    if (k.bitVar == 0)
                        k.trip = 0.15f / kTripSeconds;                 // the cable has his boot: straight into the two skips
                    else
                        bowReeds (k.x + k.bitDir * 1.5f, k.bitDir);
                }
                if (k.bitVar != 0)
                {
                    for (float a : { 27.0f, 32.0f, 38.0f })
                        if (at (a))
                            addFx (2, k.x + k.bitDir * 0.45f, 0.0f, kKeeperZ - 0.1f, 0.9f * cast, k.bitDir);
                    if (at (56.0f)) g.preen = 0.0f;                    // and it sees to its feathers, unbothered
                    if (at (72.0f)) k.hatHop.v = 0.8f;
                }
                else
                {
                    if (at (88.0f) || at (98.0f)) { addFx (2, k.x, 0.0f, kKeeperZ - 0.1f, 0.5f * cast, -k.bitDir); k.hatHop.v = 0.9f; }
                    if (at (123.0f)) k.cableWave.v += 3.0f;            // kicked off: a wave runs down it
                    if (at (134.0f)) addRing (g.x - k.bitDir * 0.9f * size, 1.9f * size, g.z - 0.3f, 0.5f * cast, false, 0.0f, 1.9f, rgb (0xe5533b));
                    if (at (150.0f)) k.blink.t = 0.0f;
                }
                if (at (42.0f)) { g.squash.v = 2.4f; g.wag = 0.8f; }
                break;
            case Bit::win:
                if (at (34.0f))
                {
                    // It lands. Everyone in the picture feels it.
                    addFx (4, g.x - k.bitDir * 0.1f, 0.95f * size, g.z - 0.2f, 1.1f * cast, k.bitDir);
                    addFeathers (g.x, 0.7f * size, g.z, size);
                    bowReeds (g.x + k.bitDir * 0.8f, k.bitDir);
                    for (auto& o : geese)
                        if (&o != &g && o.used && ! o.leaving && gooseActive (o))
                            o.squash.v = 1.6f;
                    beaver.ear = 0.0f;
                    beaver.blink.t = 0.0f;
                    if (rnd() < 0.34f && dabble >= 1.0f)
                        dabble = 0.0f;
                }
                if (at (55.0f) || at (66.0f)) addFx (2, g.x + k.bitDir * gooseBitAt (k.bit, f, k.bitRoom, k.bitVar).dx, 0.0f, g.z, 0.7f * cast, k.bitDir);
                if (at (57.0f)) { startGesture (Gesture::pump, kPumpSeconds); k.pumpWait = rnd (7.0f, 12.0f); k.tip.v += 1.5f; }
                if (at (73.0f))
                {
                    g.x += k.bitDir * k.bitRoom;
                    g.beaten = false;
                    g.leaving = true;
                    g.away = 0.0f;
                }
                break;
            case Bit::gooseWins:
                if (at (24.0f)) k.tip.v -= 1.5f;
                if (at (78.0f)) k.blink.t = 0.0f;
                if (at (118.0f)) addRing (rideHat.x + k.bitDir * 0.3f * size, rideHat.y + 1.4f * size, kKeeperZ, 0.5f * cast, false, 0.0f, 1.6f, rgb (0xe5533b));
                if (at (160.0f)) startGesture (Gesture::hat, kHatSeconds);
                if (at (172.0f)) { g.squash.v = 2.4f; g.wag = 1.0f; }
                if (at (180.0f))
                    for (auto& o : geese)
                        if (&o != &g && o.used && ! o.leaving && gooseActive (o))
                        {
                            o.honkPhase = std::floor (o.honkPhase) + 1.0f;     // one of the others has something to say about it
                            break;
                        }
                break;
            case Bit::honk:
                if (at (10.0f)) addRing (g.x - k.bitDir * (0.3f + 0.75f * size), std::min (headAt.y - 0.2f * cast, 2.3f * size), g.z - 0.3f, 0.6f * cast, false, 0.0f, 1.9f, rgb (0xe5533b));
                if (at (11.0f)) { k.hatHop.v = 1.0f; k.tip.v += 1.5f; }
                if (at (44.0f)) k.blink.t = 0.0f;
                break;
            case Bit::none:
                break;
        }
    }

    //------------------------------------------------------------------------------ the recordist
    void startGesture (Gesture g, float seconds) noexcept
    {
        keeper.gesture = g; keeper.gt = 0.0f; keeper.glen = seconds; keeper.idleGesture = false;
        if (keeper.solo < 0.05f && keeper.compare < 0.05f)
            keeper.hand = keeper.facing > 0.0f ? -1.0f : 1.0f;        // the hand on the camera's side
    }

    void stepKeeper (const WorldInput& in, float dt, bool anyGoose)
    {
        auto& k = keeper;
        const bool resting = in.bypass, frozen = in.diff && ! in.bypass, listening = in.soloHonk && ! resting && ! frozen;
        // (With his hat gone he has a moment of disbelief first; and where there is no room to
        // run he gets it back with the pole instead: see stepHat.)
        const bool chasing = hat.state == 2 && ! hat.reach && hat.t > 16.0f * kF;
        const bool hatless = hat.state == 2 || hat.state == 4;
        tick (k.blink, dt, 2.5f, 6.0f);
        if (k.solo < 0.05f && k.compare < 0.05f && k.gesture == Gesture::none && k.sit < 0.05f)
            k.hand = k.facing > 0.0f ? -1.0f : 1.0f;
        follow (k.solo, listening ? 1.0f : 0.0f, dt, 0.3f);
        follow (k.compare, frozen ? 1.0f : 0.0f, dt, 0.3f);
        follow (k.chase, chasing ? 1.0f : 0.0f, dt, 0.15f);
        k.pumpWait = std::max (0.0f, k.pumpWait - dt);

        // Which goose he is working: the deepest in the range he is asked to mind, but he does not
        // change his mind at once, or for a small difference.
        {
            auto ok = [&] (int i) { return i >= 0 && geese[(size_t) i].used && ! geese[(size_t) i].leaving && gooseActive (geese[(size_t) i]); };
            if (k.bit != Bit::none && k.bitGoose >= 0 && geese[(size_t) k.bitGoose].used)
                k.target = k.bitGoose;                                // he sees a bit through with the goose it began with
            else if (! anyGoose || ! ok (k.target))
            {
                k.target = anyGoose && ok (deepestGoose) ? deepestGoose : -1;
                k.switchT = 0.0f;
            }
            else if (ok (deepestGoose) && deepestGoose != k.target && geese[(size_t) deepestGoose].depth > geese[(size_t) k.target].depth + 2.0f)
            {
                if ((k.switchT += dt) > 2.0f && k.cue <= 0.0f)
                {
                    k.target = deepestGoose;
                    k.switchT = 0.0f;
                }
            }
            else
                k.switchT = 0.0f;
        }
        const bool has = k.target >= 0;
        // With Snap in focus and no goose of his own, he holds the microphone out towards the
        // shallows where the fish leaps.
        const bool snapWatch = in.focus == kNumModules && ! has;
        const float tx = has ? geese[(size_t) k.target].x : snapWatch ? std::min (fishX(), keeperHi + 2.6f) : k.x;
        const float tDepth = has ? geese[(size_t) k.target].depth : 0.0f;
        const bool work = (has || snapWatch) && ! resting && ! frozen && hat.state == 0;

        // Where he wants to be: beside it, at the pole's length, never right behind it (it would
        // hide him, and he would have no room to work it), and always on his own stretch of bank
        // between the turtles' log and the beaver. While it is within reach of the pole he stays
        // where he is: he walks only when he has to.
        float want = k.x, top = listening ? 2.2f : 3.4f;
        if (chasing)
        {
            const auto& g = geese[(size_t) hat.goose];
            want = g.x + (k.x < g.x ? -0.8f : 0.8f);
            top = 4.6f;
            k.walking = false;
        }
        else if (work)
        {
            // Which side of it to work from: the nearer, unless another goose is standing there or
            // his stretch of bank ends. He changes his mind only for a clearly better side.
            const float reach = listening ? 2.6f : 2.3f;
            auto spotFor = [&] (float side) { return std::clamp (tx + side * reach, keeperLo, keeperHi); };
            auto cost = [&] (float side)
            {
                const float ideal = tx + side * reach, spot = spotFor (side);
                float c = std::abs (spot - k.x) + 1.5f * std::abs (ideal - spot);
                if (std::abs (spot - tx) < 1.4f)
                    c += 6.0f;                                        // right behind it
                for (const auto& g : geese)
                    if (g.used && ! g.leaving && std::abs (g.x - spot) < 1.1f)
                        c += 2.0f;
                return c;
            };
            if (cost (-k.workSide) + 1.5f < cost (k.workSide))
            {
                k.workSide = -k.workSide;
                k.walking = true;
            }
            const float spot = spotFor (k.workSide), d = std::abs (tx - k.x);
            const float far = 3.2f * cast + 0.9f;                     // as far as the pole reaches, let right out
            if (d < 1.5f || d > std::max (far, 2.4f) || (tx - k.x) * k.workSide > 0.0f)
                k.walking = true;
            if (k.walking && std::abs (spot - k.x) < 0.15f)
                k.walking = false;
            if (k.walking && k.bit == Bit::none)
                want = spot;
        }
        else
            k.walking = false;
        want = std::clamp (want, resting ? sitLo : keeperLo, resting ? sitHi : keeperHi);

        // The boom pole telescopes out for a goose that is further off than it reaches.
        {
            const float need = work ? std::abs (tx - k.x) : 0.0f;
            follow (k.extend, std::clamp ((need - 3.2f * cast) / (1.4f * cast), 0.0f, 0.9f), dt, 0.45f);
        }
        const bool inPlace = std::abs (want - k.x) < 0.2f;
        follow (k.sit, resting && (inPlace || k.sit > 0.5f) ? 1.0f : 0.0f, dt, 0.28f);

        // Now and then, on the move, his cable catches his foot: about once in a minute or so of
        // busy work, and never on top of anything else big.
        if (std::abs (k.speed) > 1.5f && k.trip >= 1.0f && ! chasing && k.sit < 0.05f && k.bit == Bit::none && k.bigCool <= 0.0f
            && frog.state != 1 && frog.state != 2 && fish.t >= kFishLeap)
            if ((k.tripWait -= dt) <= 0.0f)
            {
                k.trip = 0.0f;
                k.tripWait = rnd (3.0f, 6.0f);                         // seconds spent on the move, not seconds
                k.bigCool = 12.0f;
            }
        float held = 1.0f;
        if (k.trip < 1.0f)
        {
            k.trip = std::min (1.0f, k.trip + dt / kTripSeconds);
            const float s = k.trip * kTripSeconds;
            held = s < 0.56f ? 0.15f : smooth01 ((s - 0.56f) / 0.5f);
            moving = true;
        }

        const float gap = want - k.x;
        // He faces the way he goes. Only a shuffle of half a pace from a goose close by is taken
        // facing it; for anything longer he turns first and then sets off, so that he is never
        // seen running sideways or backwards.
        const bool shuffle = work && ! chasing && std::abs (tx - k.x) < 3.2f && std::abs (gap) < 0.5f;
        const bool hatBusy = (k.adjustNext || k.hatLand < 1.0f || (k.gesture == Gesture::hat && ! k.idleGesture)) && ! chasing;     // his hat first
        const bool going = std::abs (gap) >= (k.going ? 0.15f : 0.5f) && ! shuffle && ! resting && ! frozen && ! in.calm && k.bit == Bit::none && ! hatBusy;
        // Setting off on a run: he leans back first, front knee up, and then goes.
        if (going && ! k.going && std::abs (gap) > 1.6f && k.trip >= 1.0f && dt > 0.0f)
            k.setOff = 0.0f;
        k.going = going;
        const float facingWas = k.facing;
        if (k.bit != Bit::none)
            k.facing = k.bitDir;
        else if (hatless && ! chasing && hat.goose >= 0)
            k.facing = geese[(size_t) hat.goose].x >= k.x ? 1.0f : -1.0f;     // he turns to the goose that has it
        else if (going)
            k.facing = gap > 0.0f ? 1.0f : -1.0f;
        else if (std::abs (k.speed) > 0.3f && ! shuffle)
            k.facing = k.speed > 0.0f ? 1.0f : -1.0f;
        else if (work)
            k.facing = tx >= k.x ? 1.0f : -1.0f;
        // Until his body is within sixty degrees of the way he is going, he stays where he is.
        const float along = std::cos (k.yaw) * (gap > 0.0f ? 1.0f : -1.0f);
        const float turned = going ? smooth01 ((along - 0.5f) / 0.25f) : 1.0f;
        const float pace = shuffle ? 0.8f : top;
        if (k.setOff < 1.0f)
        {
            k.setOff = std::min (1.0f, k.setOff + dt / (8.0f * kF));
            moving = true;
        }
        const float speedWanted = std::clamp (gap * 3.0f, -pace, pace) * held * turned * (k.setOff < 1.0f || hatBusy ? 0.0f : 1.0f);
        k.speed += (speedWanted - k.speed) * std::min (1.0f, dt * (speedWanted * k.speed < 0.0f || turned < 1.0f ? 12.0f : 5.0f));
        // Bypass and Diff stop him where he is, once he has stepped clear of the creatures.
        if (((resting || frozen) && (inPlace || k.sit > 0.05f)) || in.calm || k.bit != Bit::none)
            k.speed = 0.0f;
        k.x = std::clamp (k.x + k.speed * dt, keeperLo, keeperHi);
        // Pulling up from a run: a short skid, heels in, two puffs of dust, and the pole's end
        // carries on a little.
        if (k.runAmt > 0.6f)
            k.ran = true;
        else if (k.ran && std::abs (k.speed) < 1.2f)
        {
            k.ran = false;
            if (k.trip >= 1.0f && k.bit == Bit::none && dt > 0.0f)
            {
                k.skidT = 0.0f;
                k.tip.v += 2.2f;
                addFx (2, k.x + k.facing * 0.35f, 0.0f, kKeeperZ - 0.1f, 1.0f * cast, k.facing);
                bowReeds (k.x + k.facing * 1.2f, k.facing);
            }
        }
        if (k.skidT < 1.0f)
        {
            const float before = k.skidT;
            k.skidT = std::min (1.0f, k.skidT + dt / (12.0f * kF));
            if (before < 0.4f && k.skidT >= 0.4f)
                addFx (2, k.x + k.facing * 0.5f, 0.0f, kKeeperZ - 0.15f, 0.8f * cast, k.facing);
            moving = true;
        }
        if (k.facing * facingWas < 0.0f)
            k.cableWave.v += 2.4f;                                     // the cable on the grass takes a wave as he turns
        // One turn of the phase is two steps, and the feet cover the ground he does.
        k.phase += std::abs (k.speed) * dt * 2.74f / cast;
        follow (k.runAmt, std::clamp (std::abs (k.speed) / 2.4f, 0.0f, 1.0f), dt, 0.12f);

        // Side on to us when he goes anywhere; nearly side on, turned to the goose, while he works
        // it (his head looks at it); three-quarters round towards us when there is nothing to do,
        // and squarely only when he compares, listens or greets us. A turn to set off or to a goose
        // is quick; the others take their time, and go round by way of facing us.
        float turn = work || hatless ? 0.3f : 0.78f;
        turn = lerp (turn, 0.3f, going ? 1.0f : k.runAmt);
        turn = lerp (turn, 1.5f, std::max ({ k.compare, 0.6f * k.solo, 0.8f * k.greet, 0.45f * offerTurn() }));
        turn = lerp (turn, 0.75f, k.sit);                    // sitting: three-quarters on, so his legs show
        k.yawTo = k.facing > 0.0f ? turn : kPi - turn;
        follow (k.yaw, k.yawTo, dt, going || std::abs (k.speed) > 0.6f || work || hatless ? 0.12f : 0.26f);
        // Standing, the pole points out to his side and a little away from us, so that the
        // microphone hangs over the geese from behind them and never comes in front of one (a
        // grey microphone over a white goose reads as a hump on its back). Sitting, he lays the
        // pole down along the bank beside him, not out over the stream.
        const float poleOut = lerp (-0.3f, -0.12f, smooth01 (k.sit));
        follow (k.poleYaw, k.facing > 0.0f ? poleOut : -kPi - poleOut, dt, 0.3f);      // turning round, it swings round behind him

        // The microphone held out over it, once he is near and standing.
        const bool close = work && std::abs (tx - k.x) < 4.4f && ! listening && k.trip >= 1.0f && k.runAmt < 0.5f;
        follow (k.shoo, close ? 1.0f : 0.0f, dt, 0.3f);
        // A big goose right in front of him: he leans away and holds the microphone out at arm's length.
        const bool blasted = close && has && tDepth > 5.5f;
        follow (k.recoil, blasted ? 1.0f : 0.0f, dt, blasted ? 0.3f : 0.5f);
        k.idle += dt;

        // The bits between him and his goose. One at a time; the busier the mix, the shorter the
        // rest between them; and each goose is taken up a ladder: a shoo, then a swing that
        // misses, then something bigger, and then a result, which the plug-in decides. If the notch
        // closes he sends the goose off with one great swing (see step); if the cut stays near
        // Max Cut the goose climbs his pole and sits on his hat.
        k.sinceBit += dt;
        k.bigCool = std::max (0.0f, k.bigCool - dt);
        k.gooseWinCool = std::max (0.0f, k.gooseWinCool - dt);
        k.honkCool = std::max (0.0f, k.honkCool - dt);
        {
            int on = 0;
            for (const auto& g : geese)
                on += g.used && ! g.leaving && gooseActive (g) ? 1 : 0;
            const float now = 0.5f * std::min (1.0f, (float) on / 4.0f) + 0.5f * std::min (1.0f, deepestCut / std::max (1.0f, in.maxCutDb));
            follow (k.heat, anyGoose ? now : 0.0f, dt, 2.0f);
        }
        if (k.target != k.rungGoose)
        {
            k.rungGoose = k.target;
            k.rung = 0;
            k.lastBit = Bit::none;
            k.ceilingT = 0.0f;
        }
        if (has)
            k.ceilingT = tDepth >= 0.85f * in.maxCutDb ? k.ceilingT + dt : std::max (0.0f, k.ceilingT - 2.0f * dt);
        if (k.bit != Bit::none)
        {
            auto& g = geese[(size_t) k.bitGoose];
            const float before = k.bt / kF;
            k.bt += dt;
            const float f = k.bt / kF;
            const bool sent = k.bit == Bit::win && f >= 34.0f;        // once the swing has landed it plays out
            const bool gone = ! g.used || ((g.leaving || g.thief || ! gooseActive (g) || g.knock < 1.0f) && ! sent);
            if (! k.bitCut && (gone || resting || frozen || listening || in.calm || chasing))
                cutBit();
            if (k.bitCut)
            {
                // Ended early: it eases out over a fifth of a second, never a jump.
                k.bitW -= dt / (12.0f * kF);
                if (k.bitW <= 0.0f)
                    endBit();
            }
            else
            {
                bitEvents (in, g, before, f);
                k.x = std::clamp (k.bitX + k.bitDir * bitPoseAt (k.bit, f, k.bitVar).shift * k.bitScale, keeperLo, keeperHi);
                if (f >= bitFrames (k.bit, k.bitVar))
                    endBit();
            }
            moving = true;
        }
        else if (dt > 0.0f)
        {
            const bool ready = has && close && k.shoo > 0.9f && std::abs (k.speed) < 0.2f && k.gesture == Gesture::none
                               && canStartBit (in, geese[(size_t) std::max (0, k.target)]) && geese[(size_t) k.target].depth >= 0.7f;
            if (has && work)
                k.bitWait = std::min (1.0f, k.bitWait + dt / (bitGap() * k.gapScale * (tDepth < 1.2f ? 1.4f : 1.0f)));      // a goose that is hardly cut is left alone longer
            if (k.cue > 0.0f)
            {
                // The last few frames before it starts (a dragonfly on the microphone takes off).
                if (! ready)
                    k.cue = 0.0f;
                else if ((k.cue -= dt) <= 0.0f)
                    startBit (k.nextBit, k.target);
            }
            else if (ready && k.sinceBit >= 1.5f)
            {
                const auto& g = geese[(size_t) k.target];
                Bit next = Bit::none;
                if (g.depth > 4.0f && g.depth - g.depthSlow > 1.0f && k.honkCool <= 0.0f && k.sinceBit > 2.5f)
                {
                    next = Bit::honk;                                  // a cut that has just jumped: a honk in his face
                    k.honkCool = 14.0f;
                }
                else if (k.bitWait >= 1.0f)
                {
                    next = chooseBit (in, g);
                    if (next == Bit::none)
                        k.bitWait = 0.6f;
                }
                if (next != Bit::none)
                {
                    k.nextBit = next;
                    k.cue = 10.0f * kF;
                }
            }
        }

        // Holding the microphone. It hangs over the goose's head and follows it a moment late,
        // with a little overshoot; he shifts his weight from foot to foot, takes a new grip now and
        // then, looks from the goose to the microphone and puts a hand to his headphones, crouches
        // as the cut deepens, and jerks his head back at a loud honk. None of it is large.
        {
            const bool holding = close && has && k.bit == Bit::none;
            follow (k.aimW, close && has && k.trip >= 1.0f && hat.state == 0 ? 1.0f : 0.0f, dt, 0.25f);
            follow (k.stance, close ? 1.0f : 0.0f, dt, 0.25f);
            if (has)
            {
                const auto& g = geese[(size_t) k.target];
                const float size = gooseSize (g), toward = k.x >= g.x ? 1.0f : -1.0f;
                float gx = g.x + g.knockX, gy = 0.0f, tipAhead = 0.0f, tipUp = 0.0f;
                biteTip (0.0f, tipAhead, tipUp);
                if (k.bit != Bit::none && k.bitGoose == k.target)
                {
                    const GooseBit b = gooseBitAt (k.bit, k.bt / kF, k.bitRoom, k.bitVar);
                    gx += k.bitDir * b.dx * k.bitW;
                    gy = (b.dy - 0.4f * b.low) * size * k.bitW * (b.ride > 0.0f ? 0.0f : 1.0f);
                    biteTip (b.lean * k.bitW, tipAhead, tipUp);
                }
                k.micX.tick (gx + toward * 0.4f * size, dt, 10.0f, 0.6f);
                k.micY.tick ((1.5f + 0.3f * honkOpen (g) * (1.0f - g.hiss)) * size + 0.5f * cast + gy, dt, 10.0f, 0.6f);
                // Where the microphone goes when it is put to the goose's beak, or the goose has
                // hold of it: its near end at the tip of the beak, wherever the goose has got to.
                // (The pole swings in its own upright plane, a little behind the goose's row, so
                // the goose turns its head to it: see drawNotchGoose.)
                const float across = micAt.z - g.z, out = tipAhead * size + 0.2f * 1.5f * cast;
                if (k.bit == Bit::tug)
                    biteAt = { gx + toward * std::sqrt (std::max (0.3f * out * out, out * out - across * across)), tipUp * size, kKeeperZ };
                else
                    biteAt = { gx + toward * 0.5f * size, 1.6f * size + gy, kKeeperZ };       // a jab: at its head, from in front
            }
            else
            {
                k.micX.x = k.x + k.facing * 3.2f * cast; k.micX.v = 0.0f;
                k.micY.x = 2.1f * cast; k.micY.v = 0.0f;
            }
            // How he stands to it: by how deep its cut is, with a little give so that he does not
            // bob between two. A change takes a third of a second; he keeps it through a bit.
            if (holding)
            {
                const float share = tDepth / std::max (1.0f, in.maxCutDb);
                const float up1 = k.stanceTo >= 1 ? 0.28f : 0.38f, up2 = k.stanceTo >= 2 ? 0.58f : 0.68f;
                k.stanceTo = share > up2 ? 2 : share > up1 ? 1 : 0;
            }
            else if (k.bit == Bit::none && k.cue <= 0.0f)
                k.stanceTo = 0;
            {
                const float to = (float) k.stanceTo, step = dt * 3.0f;
                if (std::abs (to - k.stanceAt) > 1.0e-4f)
                    moving = true;
                k.stanceAt = std::abs (to - k.stanceAt) <= step ? to : k.stanceAt + std::copysign (step, to - k.stanceAt);
            }
            if (k.flinchT < 1.0f)
            {
                k.flinchT = std::min (1.0f, k.flinchT + dt / (20.0f * kF));
                moving = true;
            }
            // Footwork: every two or three seconds a shuffle step towards the goose or away, with
            // a small hop, one foot and then the other. Called away, he brings his feet together.
            if (k.stepT < 1.0f)
            {
                k.stepT = std::min (1.0f, k.stepT + dt / (11.0f * kF));
                moving = true;
            }
            else if (holding && k.cue <= 0.0f && k.gesture == Gesture::none)
            {
                if ((k.stepWait -= dt) <= 0.0f)
                {
                    const float stride = 0.15f * std::max (cast, 0.7f);
                    k.stepFrom = k.stepTo;
                    k.stepTo = std::abs (k.stepTo) > 0.01f ? 0.0f : (rnd() < 0.5f ? stride : -stride);
                    k.stepT = 0.0f;
                    k.stepWait = rnd (2.0f, 3.5f);
                }
            }
            else if (! close && k.bit == Bit::none && std::abs (k.stepTo) > 0.01f)
            {
                k.stepFrom = k.stepTo;
                k.stepTo = 0.0f;
                k.stepT = 0.0f;
            }
            if (k.hatLand < 1.0f)
            {
                k.hatLand = std::min (1.0f, k.hatLand + dt / (10.0f * kF));
                moving = true;
            }
            if (holding && (k.weightWait -= dt) <= 0.0f)
            {
                k.weightTo = -k.weightTo;
                k.weightWait = rnd (2.5f, 4.0f);
            }
            follow (k.weight, k.weightTo, dt, 0.2f);
            if (holding && (k.gripWait -= dt) <= 0.0f)
            {
                k.grip = 0.0f;
                k.gripWait = rnd (4.0f, 7.0f);
                k.tip.v += 1.2f;
            }
            if (k.grip < 1.0f)
                k.grip = std::min (1.0f, k.grip + dt / (18.0f * kF));
            if (holding && (k.glanceWait -= dt) <= 0.0f)
            {
                const float pick = rnd();
                k.glance = k.glance != 0 ? 0 : pick < 0.6f ? 1 : 2;      // the goose, the microphone, the goose, his headphones...
                k.glanceWait = k.glance == 2 ? 40.0f * kF + rnd (0.3f, 0.8f) : rnd (1.5f, 3.0f);
            }
            if (! holding)
                k.glance = 0;
            follow (k.lookGoose, holding && k.glance == 0 ? 1.0f : 0.0f, dt, 0.045f);
            follow (k.lookMic, holding && k.glance == 1 ? 1.0f : 0.0f, dt, 0.045f);
            follow (k.lookEar, holding && k.glance == 2 && k.gesture == Gesture::none ? 1.0f : 0.0f, dt, 0.09f);
            k.flinch = std::max (0.0f, k.flinch - dt / (10.0f * kF));
            k.fishLook = std::max (0.0f, k.fishLook - dt);
            follow (k.crouch, (holding || k.bit != Bit::none) && in.maxCutDb > 2.5f ? std::clamp ((tDepth - 2.0f) / (in.maxCutDb - 2.0f), 0.0f, 1.0f) : 0.0f, dt, 0.4f);
            k.tip.tick (0.0f, dt, 19.0f, 0.22f);                       // the pole's end: about 3 Hz, gone in half a second
            k.hatHop.tick (0.0f, dt, 24.0f, 0.3f);
            k.cableWave.tick (0.0f, dt, 9.0f, 0.22f);
            // His hat follows his head a few frames late and settles after he stops.
            k.hatLag.x = std::clamp (k.hatLag.x - (headAt.x - k.headWas), -0.3f, 0.3f);
            k.hatLag.tick (0.0f, dt, 28.0f, 0.45f);
            k.headWas = headAt.x;
            k.noGoose = has ? 0.0f : k.noGoose + dt;
            if (work || resting || frozen || listening)
                k.shouldered = false;
            // Up onto his shoulder takes half a second (see keeperRig), and down a little less.
            k.rest = std::clamp (k.rest + (k.shouldered ? dt / (32.0f * kF) : -dt / (26.0f * kF)), 0.0f, 1.0f);
            const bool doing = k.bit != Bit::none || k.cue > 0.0f || k.gesture != Gesture::none || std::abs (k.speed) > 0.3f || k.trip < 1.0f;
            k.stillT = doing ? 0.0f : k.stillT + dt;
            moving = moving || std::abs (k.tip.x) > 0.004f || std::abs (k.tip.v) > 0.05f || std::abs (k.hatLag.x) > 0.01f || k.hatHop.busy()
                            || k.cableWave.busy() || k.flinch > 0.0f || (k.rest > 0.02f && k.rest < 0.98f) || k.grip < 1.0f;
        }

        // The cable trails after him along the grass.
        follow (k.cableX, k.x - k.facing * 1.3f, dt, 0.45f);

        // How long has the mix given him nothing to do?
        const bool busy = anyGoose || in.frame.activity > 0.08f;
        k.quiet = busy ? 0.0f : k.quiet + dt;

        // One small piece of business at a time, when he is standing still. (The director hands
        // out the idle ones: see startIdleEvent.)
        const bool standing = std::abs (k.speed) < 0.3f && k.trip >= 1.0f && hat.state == 0;
        const bool greeting = k.gesture == Gesture::wave || k.gesture == Gesture::tip;
        if (k.gesture != Gesture::none)
        {
            const bool lullPiece = k.gesture == Gesture::shoulder || k.gesture == Gesture::coil || k.gesture == Gesture::polish || k.gesture == Gesture::doze
                                   || k.gesture == Gesture::offer || k.gesture == Gesture::stretch;
            const bool fits = k.gesture == Gesture::pump || (standing && ! (lullPiece && work) && (greeting || (k.gesture == Gesture::sip) == (k.sit > 0.9f)));
            if (! fits && k.gesture == Gesture::offer)
                k.gt = std::max (k.gt, 1.0f - k.gt);                   // called away: the way back is the way out, run backwards
            else if (! fits)
                k.gt = std::max (k.gt, 0.85f);                         // cut short: straight to its ending
            k.gt += dt / k.glen;
            if (k.gt >= 1.0f)
                k.gesture = Gesture::none;
            (k.idleGesture ? fussing : moving) = true;
        }
        else if (standing && dt > 0.0f && k.bit == Bit::none)
        {
            if (k.adjustNext)
            {
                k.adjustNext = false;
                startGesture (Gesture::hat, kHatSeconds);
            }
            else if (k.clickWait > 0.0f)
            {
                // Clicked: he tips his hat to us, or waves, by turns.
                if ((k.clickTurn++ & 1) != 0) startGesture (Gesture::wave, 1.5f);
                else                          startGesture (Gesture::tip, 1.4f);
                k.clickWait = 0.0f;
            }
        }
        follow (k.greet, greeting ? 1.0f : 0.0f, dt, 0.2f);

        // Where he is looking. In Diff he looks from one side to the other, comparing; after
        // tapping his headphones he looks about for what has gone quiet.
        if (frozen)
        {
            if ((k.lookWait -= dt) <= 0.0f)
            {
                k.lookTo = k.lookTo > 0.0f ? -0.6f : 0.6f;
                k.lookWait = rnd (0.9f, 1.5f);
            }
        }
        else if (k.gesture == Gesture::tap && k.gt > 0.45f)
            k.lookTo = k.gt < 0.68f ? 0.55f : k.gt < 0.88f ? -0.5f : 0.0f;
        else
            k.lookTo = 0.0f;
        // The eyes lead, the head follows, and the body comes round a little after the head.
        follow (k.eyeLook, k.lookTo, dt, 0.05f);
        follow (k.look, k.eyeLook, dt, 0.13f);
        follow (k.torsoLook, k.look, dt, 0.1f);

        const float yawTo = k.facing > 0.0f ? turn : kPi - turn;
        moving = moving || std::abs (k.speed) > 0.05f || (k.sit > 0.01f && k.sit < 0.99f) || (k.solo > 0.01f && k.solo < 0.99f)
                        || k.compare > 0.01f || k.recoil > 0.02f || std::abs (k.cableX - (k.x - k.facing * 1.3f)) > 0.03f
                        || std::abs (k.yaw - yawTo) > 0.02f || k.clickWait > 0.0f || (k.greet > 0.02f && k.greet < 0.98f)
                        || (k.shoo > 0.02f && k.shoo < 0.98f) || k.runAmt > 0.02f;
        // Looking about is part of his idle business (Diff, which is not idle, counts above).
        fussing = fussing || std::abs (k.look) > 0.02f || std::abs (k.eyeLook) > 0.02f || std::abs (k.torsoLook) > 0.02f;

        {
            const KeeperRig rig = keeperRig();
            const float sc = 1.5f * cast;
            micAt = rig.mic;
            headAt = rig.head.apply ({ 0.04f, rig.hipY + 1.08f, 0.0f });
            // Which hand leads on the pole changes over a tenth of a second, never at once.
            const float to = rig.frontRaw, step = dt / (6.0f * kF);
            k.frontW = std::abs (to - k.frontW) <= step ? to : k.frontW + std::copysign (step, to - k.frontW);
            if (k.frontW > 0.0f && k.frontW < 1.0f)
                moving = true;
            // Where a goose can stand on him: the end of the pole, by his hands, and his hat.
            rideTip = rig.mic + Vec3 { 0.0f, 0.15f * sc, 0.0f };
            rideGrip = rig.upper.apply (rig.grip + rig.dir * 0.55f) + Vec3 { 0.0f, 0.04f * sc, 0.0f };
            rideHat = rig.head.apply ({ 0.01f, rig.hipY + 1.08f + 0.44f - 0.14f * rig.hatDown, 0.0f });
        }
    }

    //------------------------------------------------------------------------------ the fish
    void stepFish (const WorldInput& in, float dt)
    {
        // One leap for a click, and no more than one every few seconds however many clicks there
        // are. With another range in focus the fish stays down. A ring spreads on the water a
        // moment before it comes up. A second click soon after the first brings a second, smaller
        // fish close behind. A leap never starts on top of a bit between him and his goose: it
        // waits a moment for its turn, and is let go if the turn does not come.
        const bool minded = in.snapOn && (in.focus < 0 || in.focus == kNumModules);
        const bool click = minded && in.frame.snapCutDb > 2.0f && snapWas <= 2.0f && dt > 0.0f;
        snapWas = in.frame.snapCutDb;
        if (click || (minded && in.frame.snapCutDb > 2.0f && fish.t >= kFishRest && dt > 0.0f))
        {
            if (fish.t >= kFishRest)
            {
                fishPend = 1.2f;
                fishPower = std::clamp (in.frame.snapCutDb / 12.0f, 0.3f, 1.0f);
            }
            else if (click && fish.t > 0.1f && fish.t < 1.0f && fish2.t >= kFishRest && ! bitActing())
            {
                fish2 = { -kFishLead - 10.0f * kF, 0.6f * fish.power, fish.dir, false, 0.62f, -fish.dir * 0.55f };
                addRing (fishX() + fish2.off - fish2.dir * 0.75f, 0.03f, kFishZ + 0.25f, 0.3f, true, 0.0f, 2.2f, rgb (0xf4fbff), true);
            }
        }
        if (fishPend > 0.0f)
        {
            fishPend -= dt;
            if (! bitActing() && keeper.trip >= 1.0f && hat.state == 0 && fish.t >= kFishRest && dt > 0.0f)
            {
                fishPend = 0.0f;
                fish = { -kFishLead, fishPower, (spawned++ % 2) ? 1.0f : -1.0f, false, 1.0f, 0.0f };
                addRing (fishX() - fish.dir * 0.75f, 0.03f, kFishZ, 0.4f, true, 0.0f, 2.0f, rgb (0xf4fbff), true);
                // A big one turns his head, if he has nothing else on.
                if (fish.power > 0.55f && keeper.bit == Bit::none && keeper.gesture == Gesture::none && keeper.sit < 0.1f
                    && (fishX() - keeper.x) * keeper.facing > 0.0f)
                    keeper.fishLook = 0.9f;
            }
        }
        for (Fish* fi : { &fish, &fish2 })
        {
            auto& fs = *fi;
            const float before = fs.t;
            if (fs.t < kFishRest)
                fs.t += dt;
            if (fs.t < kFishLeap + 0.6f)
            {
                if (before < 0.0f && fs.t >= 0.0f)
                    splash (fishX() + fs.off - fs.dir * 0.75f, kFishZ, 0.55f * fs.size, false);
                if (! fs.flopped && fs.t >= kFishLeap)
                {
                    fs.flopped = true;
                    splash (fishX() + fs.off + fs.dir * 0.75f, kFishZ, (1.6f + 0.6f * fs.power) * fs.size);       // the belly-flop
                }
                moving = true;
            }
        }
    }
    static constexpr float kFishLead = 12.0f / 60.0f;   // the ring on the water this long before it leaps
    float fishPower = 0.5f;
    static constexpr float kFishLeap = 0.95f, kFishZ = 0.95f, kFishRest = 4.5f;
    float fishX() const noexcept { return worldX (2700.0f); }

    //------------------------------------------------------------------------------ the beaver
    void stepBeaver (const WorldInput& in, float dt)
    {
        auto& b = beaver;
        tick (b.blink, dt, 2.0f, 5.0f);
        // The wash reading means something only while there is programme; plain noise reads about
        // 0.8. With no programme the reading is held, and so is the dam.
        if (in.frame.activity > 0.03f)
            follow (washSlow, std::clamp (in.frame.wash / 0.8f, 0.0f, 1.0f), dt, 2.5f);
        // With a range in focus the dam is left as it is.
        // With nothing playing it is left as it is too: the beaver downs tools when the music stops,
        // so that the pond comes to rest soon after.
        const float target = in.focus >= 0 || silence > 1.0f ? dam : smooth01 ((washSlow - 0.3f) / 0.5f);
        damGoal = target;
        dam += std::clamp (target - dam, -dt / 30.0f, dt / 24.0f);

        const bool building = target > dam + 0.02f, shrinking = target < dam - 0.02f && dam > 0.02f;
        // The reading jumps: a start (ears up, eyes wide, quite still), and then it hurries.
        b.hurry = std::max (0.0f, b.hurry - dt);
        if (in.frame.activity > 0.03f && in.focus < 0 && std::clamp (in.frame.wash / 0.8f, 0.0f, 1.0f) - washSlow > 0.3f && b.take >= 1.0f && b.hurry <= 0.0f && dt > 0.0f)
        {
            b.take = 0.0f;
            b.hurry = 6.0f;
        }
        if (b.take < 1.0f) { b.take = std::min (1.0f, b.take + dt / 0.6f); moving = true; }
        // The dam starts to come apart: it wipes its brow, once.
        if (shrinking && ! b.wiped && b.sulk > 0.6f) { b.wiped = true; b.brow = 0.0f; }
        if (! shrinking && b.sulk < 0.1f) b.wiped = false;
        if (b.brow < 1.0f) { b.brow = std::min (1.0f, b.brow + dt / 1.5f); moving = true; }
        follow (b.build, building ? 1.0f : 0.0f, dt, 0.4f);
        follow (b.sulk, shrinking ? 1.0f : 0.0f, dt, 0.5f);
        follow (b.carry, carryingStick() ? 1.0f : 0.0f, dt, 0.12f);
        const bool up = building || shrinking || dam > 0.22f;
        follow (b.upright, up ? 1.0f : 0.0f, dt, 0.7f);

        // Building: two or three pats and a moment to admire it. With the dam standing and the mix
        // playing: one to three pats now and then. With the mix quiet it leaves the dam alone,
        // apart from a single pat when the shared idle timer picks it (see startIdleEvent).
        if (! b.patting && b.proudT <= 0.0f && b.upright > 0.9f && ! shrinking && dt > 0.0f)
        {
            if (building && (b.patWait -= dt) <= 0.0f)
            {
                startPats (2);
                b.patWait = rnd (2.5f, 4.5f);                          // and a breather before the next two
            }
        }
        if (b.patting)
        {
            const float before = b.pat;
            b.pat += dt / b.patLen;
            if (before < 0.62f && b.pat >= 0.62f)
                damSquash.v += 1.6f;                                   // the sticks settle under the paws
            if (b.pat >= 1.0f)
            {
                b.pat = 0.0f;
                b.patLen = rnd (0.65f, 0.95f);                         // no two pats alike
                if (++b.pats >= b.patCount || shrinking)
                {
                    b.patting = false;
                    b.proudT = shrinking ? 0.0f : (b.patCount == 1 ? 0.8f : rnd (1.2f, 1.8f));
                }
            }
            (b.idlePat ? fussing : moving) = true;
        }
        damSquash.tick (0.0f, dt, 14.0f, 0.25f);
        fussing = fussing || damSquash.busy();
        if (b.proudT > 0.0f)
        {
            b.proudT -= dt;
            (b.idlePat ? fussing : moving) = true;
        }
        follow (b.proud, b.proudT > 0.0f ? 1.0f : 0.0f, dt, 0.18f);

        // Coming apart: it turns its back on the thing and slaps its tail on the water.
        if (b.sulk > 0.6f && b.brow >= 1.0f)
        {
            const float before = b.slap;
            b.slap += dt / 1.8f;
            if (before < 0.62f && b.slap >= 0.62f)
                splash (b.x - 0.5f * fg * std::cos (b.yaw), streamZ + 0.5f * fg * std::sin (b.yaw), 0.55f * fg);
            if (b.slap >= 1.0f)
                b.slap -= 1.0f;
        }
        else
            b.slap = 0.0f;

        // An ear flicks when the director says so; so does a wave, or a slap of the tail as it floats.
        if (b.wave < 1.0f) { b.wave = std::min (1.0f, b.wave + dt / 2.0f); moving = true; }
        if (b.flop < 1.0f)
        {
            const float before = b.flop;
            b.flop = std::min (1.0f, b.flop + dt / 1.1f);
            if (before < 0.6f && b.flop >= 0.6f)
                splash (b.x - 0.5f * fg * std::cos (b.yaw), streamZ + 0.5f * fg * std::sin (b.yaw), 0.5f * fg);
            moving = true;
        }
        if (b.ear < 1.0f) { b.ear = std::min (1.0f, b.ear + dt / 0.35f); fussing = true; }

        float home = up ? damX - (damHalf (dam) + 0.42f) * fg : damX - (damHalf (dam) + 1.0f) * fg + 0.15f * fg * std::sin (time * 0.13f);
        // With a stick in its paws, it wades over to where that stick goes (its paws above the
        // place), so that it can set the stick down there without letting go of it.
        int stickIndex = 0;
        const bool placing = placingStick (stickIndex) >= 0.0f && b.carry > 0.5f;
        if (placing)
        {
            Vec3 c;
            float yawS = 0.0f, lenS = 0.0f;
            stickRest (stickIndex, c, yawS, lenS);
            home = c.x - (beaverHoldAt.x - b.x);
        }
        const float wade = (placing ? 1.8f : 0.9f) * (b.hurry > 0.0f && b.take >= 1.0f ? 1.7f : b.take < 1.0f ? 0.0f : 1.0f);
        b.x += std::clamp ((home - b.x) * 3.0f, -wade, wade) * dt;
        const float yawTo = b.sulk > 0.5f ? kPi - 0.15f : b.proud > 0.5f ? kPi * 0.5f - 0.25f : up ? 0.5f : kPi - 0.45f + 0.25f * std::sin (time * 0.07f);
        follow (b.yaw, yawTo, dt, 0.35f);

        moving = moving || b.build > 0.03f || b.sulk > 0.03f || (b.upright > 0.02f && b.upright < 0.98f) || std::abs (home - b.x) > 0.06f
                        || (damSquash.busy() && ! b.idlePat) || (dam > 0.001f && dam < 0.999f && std::abs (target - dam) > 0.02f);
    }
    void startPats (int count) noexcept
    {
        beaver.idlePat = false;
        beaver.patting = true;
        beaver.pat = 0.0f;
        beaver.pats = 0;
        beaver.patCount = count;
        beaver.patLen = rnd (0.65f, 0.95f);
    }

    static constexpr float kCarryShare = 0.45f;      // of each new stick's time, how much it spends in the beaver's paws
    /** True while the beaver has the next stick in its paws: carried, then set down in its place. */
    bool carryingStick() const noexcept
    {
        const float n = dam * 12.0f;
        return damGoal > dam + 0.02f && n - std::floor (n) < 0.97f && dam < 0.999f;
    }
    /** How far the stick being added has got in being set down (0 = still carried across the
        chest, 1 = in place), or -1 when none is being added. */
    float placingStick (int& index) const noexcept
    {
        const float n = dam * 12.0f;
        index = std::min (11, (int) n);
        if (! (damGoal > dam + 0.02f && dam < 0.999f))
            return -1.0f;
        return std::clamp ((n - std::floor (n) - kCarryShare) / (1.0f - kCarryShare), 0.0f, 1.0f);
    }
    static constexpr int kDamSticks = 12;
    /** Where stick `i` of the dam lies, and how it lies. */
    void stickRest (int i, Vec3& c, float& yaw, float& len) const noexcept
    {
        const float half = damHalf (dam) * fg, settle = 1.0f - 0.6f * damSquash.x;
        const int course = i / 3, inCourse = i % 3;
        const float yy = (0.04f + 0.1f * (float) course * settle) * fg;
        const float width = half * (1.0f - 0.18f * (float) course);
        const float xx = damX + width * (0.55f * (float) (inCourse - 1) + 0.12f * std::sin ((float) i * 2.4f));
        yaw = 0.25f * std::sin ((float) i * 1.7f) + (inCourse == 1 ? 0.0f : 0.35f * (float) (inCourse - 1));
        len = (0.42f + 0.12f * std::cos ((float) i * 3.1f)) * fg;
        c = { xx, yy, streamZ - 0.38f * fg + 0.12f * fg * std::cos ((float) i * 0.7f) };
    }

    /** Half the width of the dam along the stream, for how much of it there is. */
    static float damHalf (float amount) noexcept { return 0.22f + 0.33f * amount; }

    //------------------------------------------------------------------------------ the turtles
    void stepTurtles (const WorldInput& in, float dt)
    {
        const bool guardOn = in.guardAmount > 0.001f;
        // The core reports the guard already scaled by its amount: judge it against the control,
        // so that a hit reads the same at 20 % as at 100 %.
        const bool hit = guardOn && in.focus < 0 && in.frame.guard > 0.3f * std::max (in.guardAmount, 0.05f);
        guardHold = hit ? 0.35f : std::max (0.0f, guardHold - dt);
        const bool hide = hit || guardHold > 0.0f;
        follow (asleep, guardOn ? 0.0f : 1.0f, dt, 1.2f);
        // A really big hit rocks their log.
        const bool big = guardOn && in.focus < 0 && in.frame.guard > 0.7f * std::max (in.guardAmount, 0.05f);
        if (big && ! guardBig && dt > 0.0f)
            logRock.v += 0.6f;
        guardBig = big;
        logRock.tick (0.0f, dt, 12.0f, 0.2f);
        moving = moving || logRock.busy();

        for (int i = 0; i < 3; ++i)
        {
            auto& t = turtles[(size_t) i];
            tick (t.blink, dt, 2.5f, 7.0f);
            if (hide)
            {
                // A start (head up, eyes wide), then snap: everything in, the shell drops onto the
                // log with a squash and rocks.
                if (! t.in && t.antic >= 1.0f && (t.delay -= dt) <= 0.0f)
                    t.antic = 0.0f;
                if (! t.in && t.antic < 1.0f)
                {
                    t.antic = std::min (1.0f, t.antic + dt / 0.06f);
                    if (t.antic >= 1.0f)
                    {
                        t.in = true; t.stage = 0;
                        t.wobble.v += (i == 1 ? -6.0f : 6.0f);
                        t.drop.x = 0.17f; t.drop.v = 0.0f;
                    }
                }
                if (t.in)
                {
                    t.stage = 0;
                    t.peekWait = 0.45f + 0.6f * (float) ((i + peekOrder) % 3) + rnd (0.0f, 0.35f);
                }
            }
            else
            {
                t.delay = 0.02f + 0.07f * (float) i;
                if (! t.in)
                    t.antic = 1.0f;
                if (t.in && (t.peekWait -= dt) <= 0.0f)
                {
                    // Coming out: two eyes in the opening first, then the head, just, then all of it.
                    if (t.stage == 0)      { t.stage = 1; t.peekWait = 0.78f; }      // one eye for a third of a second, then both
                    else if (t.stage == 1) { t.stage = 2; t.peekWait = rnd (0.7f, 1.1f); }
                    else                   { t.stage = 3; t.in = false; ++peekOrder; }
                }
            }
            const float to = t.in ? (t.stage == 2 ? 0.42f : 0.0f) : 1.0f;
            follow (t.out, to, dt, to < t.out ? 0.035f : 0.22f);
            follow (t.eyes, t.in && t.stage == 1 ? 1.0f : 0.0f, dt, 0.08f);
            t.wobble.tick (0.0f, dt, 14.0f, 0.25f);                   // about 2 Hz, dying away over half a second
            t.drop.tick (0.0f, dt, 21.0f, 0.24f);                     // two bounces on the log

            // Out and at ease: it looks about, and stretches its neck up into the sun, when the
            // director says so.
            const float lookTarget = t.stage == 2 ? 0.55f * std::sin (time * 3.2f + (float) i * 2.0f) : t.lookTo * (1.0f - asleep);
            follow (t.look, lookTarget, dt, 0.3f);
            if (t.stretchDelay >= 0.0f && (t.stretchDelay -= dt) < 0.0f)
            {
                t.stretchDelay = -1.0f;
                if (asleep < 0.5f && ! t.in)
                    t.stretch = 0.0f;
            }
            if (t.stretch < 1.0f) { t.stretch = std::min (1.0f, t.stretch + dt / 2.4f); fussing = true; }

            // Clicked: off the back of the log it goes, with a splash, and climbs back over it.
            if (t.fall < 1.0f)
            {
                const float before = t.fall * kFallSeconds;
                t.fall = std::min (1.0f, t.fall + dt / kFallSeconds);
                const float now = t.fall * kFallSeconds;
                const Vec3 at = turtleAt (i);
                if (before < 0.75f && now >= 0.75f)
                    splash (at.x, at.z + 0.55f * fg, 0.75f * fg);
                if (before < 1.3f && now >= 1.3f)
                    addRing (at.x, 0.03f, at.z + 0.5f * fg, 0.3f * fg, true, 0.0f, 1.5f, rgb (0xf4fbff), true);
                if (before < 2.7f && now >= 2.7f)
                {
                    t.drop.x = 0.2f; t.drop.v = 0.0f;
                    t.wobble.v += (i == 1 ? -6.0f : 6.0f);
                }
                moving = true;
            }

            moving = moving || std::abs (t.out - to) > 0.02f || t.wobble.busy() || t.drop.busy() || (t.in && ! hide) || t.antic < 1.0f
                            || (t.eyes > 0.02f && t.eyes < 0.98f) || (t.stage == 2 && t.in);
            fussing = fussing || std::abs (t.look - lookTarget) > 0.03f;
        }
        moving = moving || (asleep > 0.02f && asleep < 0.98f);
    }

    //------------------------------------------------------------------------------ the frog
    void stepFrog (const WorldInput& in, float dt, bool anyGoose, float worstX, float worstDepth, float deepestNow)
    {
        auto& fr = frog;
        tick (fr.blink, dt, 1.5f, 5.0f);
        const float ceiling = std::max (1.0f, in.maxCutDb);
        const bool atCeiling = deepestNow >= ceiling - 0.25f && ! in.bypass;
        // Re-armed only once the deepest notch has stayed clear of the ceiling for a second: the
        // leap marks a notch reaching it, and a notch pinned there gets a tense frog, not a loop.
        fr.below = deepestNow < ceiling - 1.0f ? fr.below + dt : 0.0f;
        if (fr.below > 1.0f)
            fr.armed = true;
        const float near = anyGoose && ! in.bypass ? smooth01 ((worstDepth / ceiling - 0.55f) / 0.4f) : 0.0f;
        fr.rest = std::max (0.0f, fr.rest - dt);                          // just back on its pad: it sits a moment before it can tense again
        follow (fr.tension, fr.rest > 0.0f ? 0.0f : atCeiling && ! fr.armed ? 1.0f : near, dt, 0.5f);
        const float high = smooth01 ((fr.tension - 0.6f) / 0.4f);
        if (high > 0.0f)
        {
            fr.quiver += dt * 3.5f * 6.2831853f;                          // a shiver, slow enough to show
            moving = true;
        }

        // The pad drifts along the stream to sit below the deepest goose. Where it cannot follow,
        // the frog turns and looks up at it instead.
        const float lo = frogLo(), hi = frogHi();
        const float gooseHere = worstX * streamRatio;
        if (fr.tongue < 1.0f) { fr.tongue = std::min (1.0f, fr.tongue + dt / 0.5f); fussing = true; }
        if (fr.state == 0)
        {
            const float goal = anyGoose ? std::clamp (gooseHere, lo, hi) : std::clamp (fr.x, lo, hi);
            const float wanted = std::clamp ((goal - fr.x) * 1.3f, -0.8f, 0.8f);
            fr.v += (wanted - fr.v) * std::min (1.0f, dt * 2.5f);
            follow (fr.look, anyGoose && std::abs (gooseHere - fr.x) > 0.3f ? (gooseHere > fr.x ? 1.0f : -1.0f) : 0.0f, dt, 0.4f);
            follow (fr.lookUp, anyGoose && std::abs (gooseHere - fr.x) > 0.6f * fg && std::abs (fr.v) < 0.1f ? 1.0f : 0.0f, dt, 0.5f);
        }
        else
        {
            fr.v += (0.0f - fr.v) * std::min (1.0f, dt * 4.0f);
            follow (fr.lookUp, 0.0f, dt, 0.2f);
        }
        if (keeper.gesture == Gesture::offer && offerFrog && fr.state == 0)
        {
            // The microphone is being held out to it: it looks up at it.
            follow (fr.look, keeper.x * keeperToStream > fr.x ? 1.0f : -1.0f, dt, 0.15f);
            follow (fr.lookUp, 1.0f, dt, 0.2f);
            fussing = true;
        }
        fr.x += fr.v * dt;
        fr.pad.tick (0.0f, dt, 9.0f, 0.22f);

        // Its throat goes, twice, when the director says so. Tense, it goes on its own in quick
        // short gulps, quicker as the ceiling comes near.
        const bool gulping = fr.tension > 0.35f;
        if (gulping && (fr.puffWait -= dt * (1.0f + 5.0f * fr.tension)) <= 0.0f) { fr.puff = 0.0f; fr.puffWait = rnd (2.5f, 5.5f); }
        if (fr.puff < 1.0f) { fr.puff = std::min (1.0f, fr.puff + dt * (1.0f + 1.5f * fr.tension)); (gulping ? moving : fussing) = true; }

        fr.t += dt;
        fr.sinceLeap += dt;
        switch (fr.state)
        {
            case 0:
                fr.cool -= dt;
                // (Never on top of a bit between him and his goose, the cable or the hat: it waits.)
                if (atCeiling && fr.armed && fr.cool <= 0.0f && dt > 0.0f && ! bitActing() && keeper.trip >= 1.0f && hat.state == 0)
                {
                    fr.state = 1; fr.t = 0.0f;
                    fr.armed = false;
                    // Leaps close together come less and less often, so a notch bobbing at the
                    // ceiling does not set the frog off on a loop.
                    fr.repeats = fr.sinceLeap < 20.0f ? fr.repeats + 1 : 0;
                    fr.sinceLeap = 0.0f;
                    chooseLeap (lo, hi);
                    // ...and the stream right under the goose that did it stirs too, so that the two
                    // read together even when the pad could not get there.
                    if (anyGoose && std::abs (gooseHere - fr.landX) > 1.2f * fg)
                    {
                        addRing (gooseHere, 0.03f, streamZ + 0.1f * fg, 0.35f * fg, true, 0.3f, 1.3f, rgb (0xf4fbff), true);
                        addRing (gooseHere, 0.03f, streamZ + 0.1f * fg, 0.6f * fg, true, 0.5f, 1.1f, rgb (0xf4fbff), true);
                    }
                }
                break;
            case 1: if (fr.t >= 0.3f)  { fr.state = 2; fr.t = 0.0f; fr.pad.v = 2.4f; } break;
            case 2: if (fr.t >= 0.7f)  { fr.state = 3; fr.t = 0.0f; splash (fr.landX, streamZ, 0.6f * fg); } break;
            case 3:
                if (fr.t >= 0.5f)
                {
                    // Its third leap in twenty seconds: it has had enough, and stays in the water
                    // with only its eyes showing until the cut eases.
                    fr.state = fr.repeats >= 2 && ! fr.byClick ? 7 : 4;
                    fr.t = 0.0f; fr.lurk = 0.0f;
                    addRing (fr.landX, 0.03f, streamZ, 0.3f * fg, true, 0.0f, 1.6f, rgb (0xf4fbff));
                }
                break;
            case 7:
                fr.lurk += dt;
                if (fr.below > 0.6f || fr.lurk > 20.0f || in.bypass) { fr.state = 4; fr.t = 0.0f; fr.repeats = 0; }
                break;
            case 4: if (fr.t >= 0.8f)  { fr.state = 5; fr.t = 0.0f; } break;
            case 6: if (fr.t >= 20.0f * kF) { fr.state = 0; fr.t = 0.0f; fr.rest = 0.5f; } break;
            case 5:
                if (fr.t >= 0.5f)
                {
                    // Back on the pad: it shakes itself dry.
                    fr.state = 6; fr.t = 0.0f; fr.pad.v = 1.8f;
                    for (auto& b : bursts)
                        if (b.t >= 1.0f)
                        {
                            b = { fr.x, streamZ, 0.0f, 0.4f * fg, rnd (0.0f, 6.0f) };
                            break;
                        }
                    // (A leap asked for with the mouse leaves the ceiling's own timing as it was.)
                    fr.cool = fr.byClick ? fr.coolKept : std::min (30.0f, rnd (2.8f, 5.5f) * (float) (1 << std::min (fr.repeats, 4)));
                    fr.byClick = false;
                }
                break;
            default: break;
        }
        moving = moving || fr.state != 0 || std::abs (fr.v) > 0.04f || fr.pad.busy() || (fr.tension > 0.02f && fr.tension < 0.98f)
                        || (fr.lookUp > 0.02f && fr.lookUp < 0.98f);
    }

    float frogLo() const noexcept { return logX + (kLogHalf + 0.6f) * fg; }
    float frogHi() const noexcept { return damX - (damHalf (1.0f) + 1.75f) * fg; }

    /** Which way the frog leaps, and how high: away from the recordist where it can, so that it
        never passes in front of his boots; where it must leap his way, a low hop that stays below
        them in the picture. */
    void chooseLeap (float lo, float hi) noexcept
    {
        auto& fr = frog;
        const auto kp = r.project ({ keeper.x, 0.0f, kKeeperZ }), kq = r.project ({ keeper.x + 1.0f, 0.0f, kKeeperZ });
        const float halfBody = 0.8f * cast * (kq.x - kp.x);
        const float perStream = r.project ({ 1.0f, 0.0f, streamZ }).x - r.project ({ 0.0f, 0.0f, streamZ }).x;
        auto at = [&] (float sx) { return r.project ({ sx, 0.0f, streamZ }).x; };
        auto fits = [&] (float side) { const float to = fr.x + side * fg; return to <= hi + 0.6f * fg && to >= lo - 0.6f * fg; };
        auto crosses = [&] (float side)
        {
            const float a = at (fr.x), b = at (fr.x + side * fg), half = 0.3f * fg * perStream;
            return std::max (a, b) + half > kp.x - halfBody && std::min (a, b) - half < kp.x + halfBody;
        };
        const float away = at (fr.x) < kp.x ? -1.0f : 1.0f;
        float side = away;
        if (! fits (away) || (crosses (away) && fits (-away) && ! crosses (-away)))
            side = -away;
        fr.side = side;
        fr.landX = fr.x + side * fg;
        fr.leapH = 1.0f;
        if (crosses (side))
        {
            // Its top must stay below his boots: the height at the stream that shows on their row.
            const float top = r.heightAtRow (kp.y - 2.0f, streamZ) - (0.05f + 0.3f) * fg;
            fr.leapH = std::clamp (top / (0.62f * fg), 0.2f, 1.0f);
        }
    }

    //------------------------------------------------------------------------------ the dragonflies
    void stepFlies (const WorldInput& in, float dt)
    {
        keyHold = in.frame.keyed ? std::min (1.0f, keyHold + dt / 0.4f) : std::max (0.0f, keyHold - dt / 1.2f);
        const bool wanted = in.frame.keyed ? keyHold >= 1.0f : keyHold > 0.0f;
        const bool settle = keeper.quiet > 4.0f || in.focus >= 0;   // nothing going on, or a range in focus: they find a post to sit on
        // He is standing still (working a goose from where he stands is fine: it rides the jab).
        // (It lifts off in the last frames before a bit begins: the sign that something is coming.)
        const bool micFree = std::abs (keeper.speed) < 0.3f && keeper.gesture == Gesture::none && keeper.trip >= 1.0f
                             && hat.state == 0 && keeper.sit < 0.1f && keeper.recoil < 0.3f && keeper.bit == Bit::none && keeper.cue <= 0.0f;
        const float keyLevel = std::clamp ((in.frame.keyDb + 45.0f) / 35.0f, 0.0f, 1.0f);
        if (! wanted)
            micVisits = 0;

        for (int i = 0; i < 3; ++i)
        {
            auto& d = flies[(size_t) i];
            const float sideOut = (i == 1 ? -1.0f : 1.0f) * (kHalfWidth + 4.0f);
            if (d.state == 0)
            {
                if (! wanted || dt <= 0.0f)
                    continue;
                d.state = 1; d.perch = 0;
                d.pos = d.from = { sideOut, 3.4f + 0.3f * (float) i, 1.5f };
                d.to = { sideOut * 0.45f + rnd (-1.5f, 1.5f), rnd (1.3f, 2.2f), rnd (0.5f, 2.2f) };
                d.hop = 0.0f; d.hopLen = 0.8f + 0.25f * (float) i; d.wait = rnd (0.3f, 0.8f);
            }
            if (d.state == 1 && ! wanted)
            {
                d.state = 2; d.perch = 0;
                d.from = d.pos; d.to = { d.pos.x >= 0.0f ? kHalfWidth + 4.0f : -kHalfWidth - 4.0f, 3.6f, 1.5f };
                d.hop = 0.0f; d.hopLen = 0.9f;
            }

            // On a post: if the post is dragged along, it hops after it rather than hang in the air.
            if (d.perch == 2 && d.state == 1)
            {
                const Vec3 top = postTop (i);
                if (d.hop >= 1.0f && std::abs (top.x - d.pos.x) > 0.1f)
                {
                    d.from = d.pos; d.hop = 0.0f;
                    d.hopLen = 0.25f + 0.05f * std::abs (top.x - d.pos.x);
                }
                d.to = top;
            }

            if (d.hop < 1.0f)
            {
                d.hop = std::min (1.0f, d.hop + dt / d.hopLen);
                const float e = 1.0f - (1.0f - d.hop) * (1.0f - d.hop) * (1.0f - d.hop);     // away fast, arriving gently
                if (d.perch == 1)
                    d.to = micPerch();
                d.pos = lerp (d.from, d.to, e);
                const Vec3 way = d.to - d.from;
                if (std::abs (way.x) + std::abs (way.z) > 0.2f && d.hop < 0.7f)
                    follow (d.yaw, std::atan2 (-way.z, way.x), dt, 0.06f);
                if (d.hop >= 1.0f && d.state == 2)
                    d.state = 0;
                moving = true;
                continue;
            }
            if (d.state != 1)
                continue;

            if (d.perch == 1)
            {
                d.pos = micPerch();
                if (! micFree)
                    d.wait = 0.0f;
            }
            if (d.perch == 2 && settle)
                continue;                                           // sitting it out
            // In the way of a bit: it darts off at once.
            if (d.perch == 0 && keeper.bit != Bit::none && std::abs (d.pos.x - micAt.x) < 1.6f && d.pos.z < 1.2f)
                d.wait = 0.0f;
            if (d.perch != 2)
                moving = true;
            if ((d.wait -= dt) > 0.0f)
                continue;

            // Somewhere new: a short dart, then a hover.
            d.from = d.pos;
            if (settle)
            {
                d.perch = 2;
                d.to = postTop (i);
                d.wait = 1.0f;
            }
            else if (i == 0 && d.perch == 0 && micFree && (micVisits == 0 || keeper.stillT > 3.0f))
            {
                // The first one lands on the microphone soon after they come (a second or two),
                // and again whenever he has held still for a few seconds. It stays until he moves.
                d.perch = 1;
                d.to = micPerch();
                d.wait = micVisits == 0 ? rnd (2.5f, 5.0f) : 30.0f;
                ++micVisits;
            }
            else
            {
                d.perch = 0;
                const float centre = 0.6f * keeper.x;
                // Short darts over the water, just above the reed tops and below the trees.
                // Short darts over the water and a hover between, higher the louder the dialogue is.
                d.to = { std::clamp (d.pos.x + rnd (-2.6f, 2.6f) + 0.15f * (centre - d.pos.x), -kHalfWidth + 0.8f, kHalfWidth - 0.8f),
                         (1.1f + 0.9f * keyLevel + rnd (0.0f, 0.35f)) * std::max (cast, 0.75f) + 0.2f, rnd (0.5f, 2.4f) };
                // ...and clear of the pole while he is swinging it about.
                if (keeper.bit != Bit::none && std::abs (d.to.x - micAt.x) < 1.8f)
                    d.to.x = std::clamp (micAt.x + (d.pos.x >= micAt.x ? 1.9f : -1.9f), -kHalfWidth + 0.8f, kHalfWidth - 0.8f);
                d.wait = rnd (0.5f, 0.85f);
            }
            const Vec3 way = d.to - d.from;
            d.hop = 0.0f;
            d.hopLen = 0.09f + 0.045f * std::sqrt (dot (way, way));
        }
    }
    Vec3 micPerch() const noexcept { return { micAt.x, micAt.y + 0.3f * cast, micAt.z }; }
    Vec3 postTop (int fly) const noexcept { return { worldX (input.edgeHz[(fly * 2 + 1) % kNumEdges]) + 0.05f, 1.02f, -0.02f }; }

    //============================================================================== backdrop
    void drawBackdrop (float dim)
    {
        const float w = weather;
        const Rgb skyTop = rgb (0xf9c9b4).mix (rgb (0x6a6f7c), w) * dim, skyLow = rgb (0xfdf0d8).mix (rgb (0xa4a8b0), w) * dim;
        const Rgb waterA = rgb (0x8fd6cc).mix (rgb (0x6a8690), w) * dim, waterB = rgb (0xb4e6dc).mix (rgb (0x86a0a8), w) * dim;
        const Rgb grassA = rgb (0xcfe08a).mix (rgb (0xa0b47a), w) * dim, grassB = rgb (0xd9e89c).mix (rgb (0xaabc86), w) * dim;
        const Rgb sand = rgb (0xe6d8a8).mix (rgb (0xb4b090), w) * dim;
        const float horizon = horizonRow();
        for (int y = 0; y < r.height; ++y)
        {
            float wz = 0.0f, vz = 0.0f;
            if (! r.groundAtRow ((float) y + 0.5f, wz, vz))
            {
                const float t = std::clamp (((float) y + 0.5f) / std::max (1.0f, horizon), 0.0f, 1.0f);
                const Rgb c = skyTop.mix (skyLow, t * t);
                r.fillRow (y, c, 1.0e8f);
            }
            else if (wz > 0.0f)
            {
                // Water in two blues: broad bands that drift slowly towards the bank.
                // Calm water moves in broad bands; wind breaks it into narrow, quicker ones.
                const float band = std::sin (wz * (0.9f + 1.6f * weather) + windPhase * 0.7f + 1.5f * std::sin (wz * 0.23f));
                const Rgb c = (band > 0.55f ? waterB : waterA).mix (skyLow, r.fogAt (vz) * 0.8f);
                r.fillRow (y, c, vz);
            }
            else if (wz < streamFar)
            {
                // The stream along the front: narrow quick bands.
                const float band = std::sin ((wz - streamFar) * 7.0f - windPhase * 1.1f);
                r.fillRow (y, (band > 0.6f ? waterB : waterA), vz);
            }
            else if (wz < streamFar + 0.14f)
                r.fillRow (y, sand, vz);                                 // its sandy edge
            else
            {
                // The bank: a lawn mown in stripes.
                const Rgb c = (((int) std::floor (-wz / 0.9f)) & 1) ? grassA : grassB;
                r.fillRow (y, c, vz);
            }
        }
        drawSky (dim);
    }

    /** A round sun and fat clouds that drift. As the weather turns, the sun sinks into them, more
        of them arrive, and they darken. */
    void drawSky (float dim)
    {
        const float far = 150.0f, w = weather;
        const float horizon = horizonRow() / (float) std::max (1, r.height);      // 0..1 down the picture

        if (w < 0.85f)
        {
            // The sun, with rays turning slowly behind it.
            const Vec3 sunAt = r.atScreen (0.80f, horizon * 0.42f, far);
            const float size = 0.055f * skyUnit (far);
            const Rgb ray = rgb (0xfff3c4) * dim;
            for (int i = 0; i < 12; ++i)
            {
                const float a0 = (float) i * 0.5236f + time * 0.05f, a1 = a0 + 0.2618f;
                const float reach = size * 9.0f;
                r.triangle (sunAt, { sunAt.x + reach * std::cos (a0), sunAt.y + reach * std::sin (a0), sunAt.z + 2.0f },
                            { sunAt.x + reach * std::cos (a1), sunAt.y + reach * std::sin (a1), sunAt.z + 2.0f }, ray, ray, ray, 0.5f * (1.0f - w));
            }
            const Rgb sun = rgb (0xfff3c4).mix (rgb (0xa4a8b0), smooth01 (w / 0.7f)) * dim;
            r.blob (Xform::move (sunAt), size, size, size * 0.1f, sun, 14, 6, 1.0f);
        }

        if (w < 0.6f)
        {
            // A rainbow standing on the far side, off to the left.
            static constexpr std::uint32_t bands[] = { 0xe5533b, 0xf2c14e, 0xb7c468, 0x8fd0e2, 0xb9a8e0 };
            const float fade = 0.8f * (1.0f - w / 0.6f);
            const Vec3 foot = r.atScreen (0.27f, horizon, far - 6.0f);
            const float outer = 0.19f * skyUnit (far), thick = outer * 0.075f;
            for (int b = 0; b < 5; ++b)
            {
                const float r0 = outer - (float) b * thick, r1 = r0 - thick;
                const Rgb c = rgb (bands[b]) * dim;
                for (int i = 0; i < 18; ++i)
                {
                    const float a0 = kPi * (float) i / 18.0f, a1 = kPi * (float) (i + 1) / 18.0f;
                    auto at = [&] (float a, float rad) { return Vec3 { foot.x + rad * std::cos (a), foot.y + rad * std::sin (a), foot.z }; };
                    r.triangle (at (a0, r0), at (a0, r1), at (a1, r1), c, c, c, fade);
                    r.triangle (at (a0, r0), at (a1, r1), at (a1, r0), c, c, c, fade);
                }
            }
        }

        // Clouds drift across. As the weather turns more of them arrive, and they darken.
        const Rgb cloud = rgb (0xfffaf0).mix (rgb (0x6c7280), smooth01 (w)) * dim;
        const int count = 4 + (int) (9.0f * w);
        for (int i = 0; i < count; ++i)
        {
            const float arrived = i < 4 ? 1.0f : smooth01 (w * 9.0f - (float) (i - 4));
            const float across = std::fmod (windPhase * 0.004f + (float) i * 0.37f, 1.3f) - 0.15f;
            const float down = horizon * (0.12f + 0.5f * (0.5f + 0.5f * std::sin ((float) i * 2.1f))) - (1.0f - arrived) * 0.4f;
            const Vec3 at = r.atScreen (across, down, far - 10.0f - (float) i);
            const float size = (0.045f + 0.02f * std::cos ((float) i * 1.3f)) * (1.0f + 0.9f * w) * skyUnit (far);
            for (int b = -1; b <= 1; ++b)
                r.blob (Xform::move ({ at.x + (float) b * size * 0.9f, at.y - std::abs ((float) b) * size * 0.2f, at.z }),
                        size, size * (b == 0 ? 0.5f : 0.38f), size * 0.1f, cloud, 10, 5, 1.0f);
        }
    }

    /** The width of the picture, in world units, at a distance along the view. */
    float skyUnit (float viewZ) const noexcept { return r.atScreen (1.0f, 0.5f, viewZ).x - r.atScreen (0.0f, 0.5f, viewZ).x; }

    float horizonRow() const noexcept
    {
        float wz, vz;
        for (int y = 0; y < r.height; ++y)
            if (r.groundAtRow ((float) y + 0.5f, wz, vz))
                return (float) y;
        return (float) r.height;
    }

    /** What the backdrop depends on besides the time: where the trees stand and what colour they are. */
    std::uint32_t backdropSignature() const noexcept
    {
        std::uint32_t h = (std::uint32_t) (input.snapOn ? 1 : 0);
        for (int m = 0; m < kNumModules; ++m)
            h = h * 2u + (input.moduleOn[m] ? 1u : 0u);
        for (int e = 0; e < kNumEdges; ++e)
            h = h * 2053u + (std::uint32_t) std::lround (x01ForHz (input.edgeHz[e]) * (float) r.width * 2.0f);
        return h;
    }

    void drawFarShore (float dim)
    {
        // Round hills on the far side.
        const float z = 62.0f;
        for (int i = -5; i <= 5; ++i)
        {
            const float x = (float) i * 17.0f + 5.0f * std::sin ((float) i * 2.3f);
            const float w = 13.0f + 4.0f * std::cos ((float) i * 1.9f), h = 5.0f + 2.5f * std::sin ((float) i * 1.1f + 1.0f);
            r.blob (Xform::move ({ x, 0.0f, z + (float) (i & 1) * 6.0f }), w, h, 3.0f, ((i & 1) ? rgb (0xc4cf8a) : rgb (0xd6cbe6)) * dim, 12, 6, 0.75f);
        }

        // A lollipop tree at each edge of the ranges, straight above its post on the bank, in the
        // colour of the range to its right (paler when that range is switched off). They are sized
        // and placed in the picture, not in the world, so that they stand the same at any shape:
        // tall enough for the crowns to rise a little above the top edge, each by its own amount.
        // The host draws the part above the edge (see treeTops).
        const float W = (float) r.width, H = (float) r.height;
        const float rad = std::clamp (std::min (0.088f * H, 0.034f * W), 8.0f * pp, 44.0f * pp);
        static constexpr float rise[kNumEdges] = { 0.36f, 0.14f, 0.44f, 0.22f, 0.32f };       // of the radius, above the top edge
        static constexpr float depth[kNumEdges] = { 43.0f, 40.0f, 44.5f, 41.5f, 46.0f };     // staggered, so crowns that meet overlap cleanly
        const Rgb off = rgb (0xd3ccba);
        for (int e = 0; e < kNumEdges; ++e)
        {
            const bool on = e < kNumModules ? input.moduleOn[e] : input.snapOn;
            const Rgb col = (on ? moduleCol[e] : moduleCol[e].mix (off, 0.72f)) * dim;
            const float zz = depth[e], row = rad * (1.0f - rise[e]);
            const float y = r.heightAtRow (row, zz);
            const auto mid = r.project ({ 0.0f, y, zz });
            if (! mid.ok)
                continue;
            const float perPx = 1.0f / r.pixelsPerUnitAt (mid.z);
            const float sx = x01ForHz (input.edgeHz[e]) * W;
            const float x = (sx - 0.5f * W) * perPx, R = rad * perPx;
            r.limb ({ x, 0.0f, zz }, { x, y, zz }, 0.2f * R, 0.16f * R, rgb (0x8a6f5c) * dim, 6, 0.6f);
            r.blob (Xform::move ({ x, y, zz }), R, R, R, col, 12, 7, 0.8f);
            const Rgb top = r.colourAt ({ x, y + R, zz }, col, { 0.0f, 1.0f, 0.0f }, 0.8f);
            tops[(size_t) e] = { sx, row, rad, (std::uint8_t) (top.r + 0.5f), (std::uint8_t) (top.g + 0.5f), (std::uint8_t) (top.b + 0.5f) };
        }
    }

    //============================================================================== the working parts
    /** A coloured strip along the bank for each range that is on, and one in the shallows for Snap.
        With a range in focus, the others' strips fade back. */
    void drawZones()
    {
        for (int m = 0; m < kNumModules; ++m)
        {
            const float x0 = worldX (input.edgeHz[m]), x1 = worldX (input.edgeHz[m + 1]);
            if (x1 <= x0)
                continue;
            const float lit = input.moduleOn[m] ? 0.85f : 0.0f;
            const Rgb c = input.moduleOn[m] ? moduleCol[m] : rgb (0xb8c880);
            const float show = input.focus < 0 || input.focus == m ? 1.0f : 1.0f - 0.55f * hush;
            r.quad ({ x0 + 0.04f, 0.012f, -0.26f }, { x0 + 0.04f, 0.012f, -0.04f }, { x1 - 0.04f, 0.012f, -0.04f }, { x1 - 0.04f, 0.012f, -0.26f }, c, lit, (input.moduleOn[m] ? 0.9f : 0.5f) * show);
        }
        if (input.snapOn)
        {
            const float x0 = worldX (1800.0f), x1 = worldX (4000.0f);
            const float show = input.focus < 0 || input.focus == kNumModules ? 1.0f : 1.0f - 0.55f * hush;
            r.quad ({ x0, 0.012f, 0.06f }, { x0, 0.012f, 0.30f }, { x1, 0.012f, 0.30f }, { x1, 0.012f, 0.06f }, rgb (0xffffff), 0.9f, 0.55f * show);
        }
    }

    static float hash01 (int i, float k) noexcept
    {
        const float v = std::sin ((float) i * k) * 43758.5453f;
        return v - std::floor (v);
    }

    /** Reeds at the water's edge: scenery. They stand where they stand, sway a little while the
        pond is alive (more in bad weather, and when a breeze comes through, left to right), and
        part where a goose hops through them. */
    void drawReeds()
    {
        // In clumps, with open water between them, and none where the fish leaps.
        static constexpr int kClumps = 7, kPerClump = 6, kReeds = kClumps * kPerClump;
        const Rgb stem = rgb (0x6aa07a), head = rgb (0x8a6a50);
        const float amp = (0.035f + 0.16f * weather) * reedLife;
        const float gustAt = lerp (-kHalfWidth - 3.0f, kHalfWidth + 3.0f, gust);
        const float fishAt = fishX();
        for (int i = 0; i < kReeds; ++i)
        {
            const int clumpIndex = i / kPerClump, j = i % kPerClump;
            float centre = (((float) clumpIndex + 0.5f + 0.5f * (hash01 (clumpIndex, 12.9898f) - 0.5f)) / (float) kClumps - 0.5f) * 2.0f * kHalfWidth;
            if (std::abs (centre - fishAt) < 1.5f)
                centre = fishAt + (centre >= fishAt ? 1.5f : -1.5f);
            const float off = ((float) j - 0.5f * (float) (kPerClump - 1)) * 0.17f + 0.1f * (hash01 (i, 12.9898f) - 0.5f);
            const float x = centre + off * std::max (cast, 0.7f);
            const float tall = hash01 (i, 78.233f), clump = 1.0f - std::abs (off) / 0.62f;
            const float h = (0.5f + 1.25f * (0.35f + 0.65f * tall) * (0.3f + 0.7f * std::max (0.0f, clump))) * cast;
            float lean = amp * std::sin (windPhase + (float) i * 0.8f) + 0.1f * weather * reedLife;
            if (gust < 1.0f)
            {
                const float d = (x - gustAt) / 2.4f;
                lean += 0.3f * std::exp (-d * d);
            }
            if (bowT < 1.0f)
            {
                // Bowing away from a stroke or a skid, and back.
                const float d = (x - bowX) / 1.6f;
                lean += 0.55f * bowDir * std::exp (-d * d) * std::exp (-3.0f * bowT) * std::cos (9.0f * bowT) * (bowT < 0.12f ? bowT / 0.12f : 1.0f);
            }
            for (const auto& g : geese)
                if (g.used && (g.reed > 0.004f || g.reed < -0.004f))
                {
                    const float d = x - g.x;
                    if (std::abs (d) < 1.3f)
                        lean += 0.45f * g.reed * (d >= 0.0f ? 1.0f : -1.0f) * (1.0f - std::abs (d) / 1.3f);
                }
            const float zz = 0.2f + 0.1f * hash01 (i, 39.3f);
            const Vec3 base { x, 0.0f, zz };
            const Vec3 tip { x + std::sin (lean) * h, std::cos (lean) * h, zz };
            r.limb (base, tip, 0.045f, 0.03f, stem, 3);
            if (tall > 0.3f)
                r.limb (tip, tip + normalised (tip - base) * 0.28f, 0.07f, 0.06f, head, 4);
        }
    }

    /** A post at each range edge, with a knob in the colour of the range to its right. */
    void drawPosts()
    {
        for (int e = 0; e < kNumEdges; ++e)
        {
            const float x = worldX (input.edgeHz[e]);
            const bool on = e < kNumModules ? input.moduleOn[e] : input.snapOn;
            r.limb ({ x, 0.0f, -0.02f }, { x, 0.8f, -0.02f }, 0.05f, 0.05f, rgb (0xfffdf6), 5);
            r.blob (Xform::move ({ x, 0.9f, -0.02f }), 0.12f, 0.12f, 0.12f, on ? moduleCol[e] : rgb (0x3a2e2a), 6, 4, 0.5f);
        }
    }

    /** Somewhere the mouse can find someone: an ellipse about a point of the world, as drawn. */
    void addHot (Hot kind, int index, Vec3 centre, float rx, float ry, float minPx = 9.0f) noexcept
    {
        if (hotCount >= (int) hots.size())
            return;
        const auto p = r.project (centre);
        if (! p.ok)
            return;
        const float px = r.pixelsPerUnitAt (p.z);
        hots[(size_t) hotCount++] = { p.x, p.y, std::max (minPx * pp, rx * px), std::max (minPx * pp, ry * px), kind, index };
    }

    //============================================================================== a goose
    struct GoosePose
    {
        float walk = 0;          // phase of the legs
        float walkAmt = 0;       // 0 standing .. 1 a full waddle
        float honk = 0;          // 0 = neck at rest, 1 = stretched out, beak wide
        float flap = 0;          // 0 = wings folded, 1 = up
        float swim = 0;          // 1 = sitting in the water (no legs)
        float hiss = 0;          // 1 = neck straight out, low, beak open, brows down
        float wag = 0;           // the tail, side to side, -1..1
        float blink = 0;
        float reach = 0;         // 1 = neck stretched straight up (for the hat)
        float hissLen = 0.62f;   // how far the neck may reach when hissing (kept short of the recordist)
        float aim = 0;           // the head's tilt when hissing, up (+) or down, towards the microphone
        float shade = 1.0f;      // a goose in the back row of a crowd is a touch greyer, so the front ones stand out against it
        float preen = 0;         // 1 = the neck curled right back, beak in the feathers of its back
        float alert = 0;         // 1 = neck up straight, watching something
        float tall = 0;          // 1 = neck straight up and stretched: as tall as it can make itself
        float bite = 0;          // 1 = neck out and up, beak shut on something (see biteTip)
    };

    /** A goose facing +x in its own space, feet at the origin. `hatAt`, if given, receives where a
        hat would sit on its head. */
    void drawGoose (const Xform& at, const GoosePose& p, float glow, Xform* hatAt = nullptr)
    {
        const Rgb white = rgb (0xfffdf6).mix (rgb (0xd8d4dc), 1.0f - p.shade), wing = rgb (0xece6da).mix (rgb (0xc8c2cc), 1.0f - p.shade);
        const Rgb orange = rgb (0xf2946b), dark = rgb (0x3a2e2a);
        const float bob = 0.05f * p.walkAmt * std::abs (std::sin (p.walk));
        const float bodyY = (p.swim > 0.5f ? 0.20f : 0.62f) + bob - 0.06f * p.hiss;
        const float roll = 0.13f * p.walkAmt * std::sin (p.walk);
        const float pitch = -0.2f * p.hiss + 0.18f * p.reach;
        const Xform up = Xform::move ({ 0.0f, -bodyY, 0.0f }).then (Xform::rotZ (pitch)).then (Xform::rotX (roll))
                             .then (Xform::move ({ 0.0f, bodyY, 0.0f })).then (at);

        r.blob (Xform::move ({ 0.0f, bodyY, 0.0f }).then (up), 0.47f, 0.33f, 0.31f, white, 9, 6, glow);
        // The tail: it wags, and fans up when the goose hisses.
        r.limb (up.apply ({ -0.36f, bodyY + 0.08f, 0.0f }), up.apply ({ -0.74f + 0.05f * p.hiss, bodyY + 0.26f + 0.04f * std::abs (p.wag) + 0.14f * p.hiss, 0.16f * p.wag }),
                0.15f + 0.04f * p.hiss, 0.0f, white, 6, glow);

        // Neck and head, the neck in two pieces with a goose's curve. Walking, the head bobs
        // forward with each step. Hissing, the neck goes out low and forward with the head level,
        // like a snake, not straight out like a pole.
        float lean = 0.30f + 0.55f * p.honk * (1.0f - 0.75f * p.tall) + 0.28f * p.walkAmt * (0.5f + 0.5f * std::sin (p.walk * 2.0f)) - 0.2f * p.alert - 0.2f * p.tall;
        float length = 0.46f + 0.30f * p.honk + 0.14f * p.alert + 0.3f * p.tall;
        lean = lerp (lean, -0.12f, p.reach); length = lerp (length, 0.95f, p.reach);
        lean = lerp (lean, 0.785f, p.bite); length = lerp (length, 0.75f, p.bite);
        const Vec3 neck0 { 0.30f, bodyY + 0.16f, 0.0f };
        Vec3 head { neck0.x + std::sin (lean) * length, neck0.y + std::cos (lean) * length, 0.0f };
        Vec3 mid = (neck0 + head) * 0.5f + Vec3 { std::cos (lean), -std::sin (lean), 0.0f } * 0.04f;
        if (p.hiss > 0.0f)
        {
            const float hl = std::clamp (p.hissLen, 0.25f, 0.62f);
            const Vec3 midH = neck0 + Vec3 { 0.46f * hl, -0.18f * hl, 0.0f };
            const Vec3 headH = midH + Vec3 { 0.5f * hl * std::cos (p.aim), 0.06f + 0.5f * hl * std::sin (p.aim), 0.0f };
            mid = lerp (mid, midH, p.hiss);
            head = lerp (head, headH, p.hiss);
        }
        // Preening: the neck arches up and right back, and the beak nibbles at the feathers of its back.
        float fwd = 1.0f;                                               // which way the face points along its body
        if (p.preen > 0.0f)
        {
            const float nibble = 0.025f * std::sin (time * 15.0f) * p.preen;
            mid = lerp (mid, neck0 + Vec3 { -0.06f, 0.3f, 0.0f }, p.preen);
            head = lerp (head, Vec3 { -0.02f + nibble, bodyY + 0.36f - nibble, 0.13f }, p.preen);
            fwd = 1.0f - 2.0f * smooth01 (p.preen * 1.4f);
        }
        r.limb (up.apply (neck0), up.apply (mid), 0.14f, 0.11f, white, 7, glow);
        r.limb (up.apply (mid), up.apply (head), 0.11f, 0.09f, white, 7, glow);
        r.blob (Xform::move ({ head.x + 0.04f * fwd, head.y + 0.03f, head.z }).then (up), 0.2f, 0.18f, 0.17f, white, 8, 5, glow);
        const float open = lerp (0.02f + 0.17f * p.honk + p.hiss * (0.09f + 0.025f * std::sin (time * 9.0f)), 0.05f, p.bite);
        const float dip = 0.16f * p.preen;
        r.limb (up.apply ({ head.x + 0.17f * fwd, head.y + 0.045f, head.z }), up.apply ({ head.x + 0.52f * fwd, head.y + 0.03f + open - dip, head.z }), 0.09f, 0.02f, orange, 5, glow);
        r.limb (up.apply ({ head.x + 0.17f * fwd, head.y - 0.02f, head.z }), up.apply ({ head.x + 0.47f * fwd, head.y - 0.04f - open - dip, head.z }), 0.07f, 0.02f, orange, 5, glow);
        if (p.hiss > 0.3f)
            r.blob (Xform::move ({ head.x + 0.3f, head.y + 0.0f, 0.0f }).then (up), 0.09f, 0.03f, 0.04f, rgb (0xf4b6c6), 5, 3, glow);     // tongue
        const float eyeH = 0.045f * (1.0f - 0.85f * std::max ({ p.blink, 0.0f, p.preen })) * (1.0f - 0.35f * p.hiss);
        for (float side : { -1.0f, 1.0f })
        {
            r.blob (Xform::move ({ head.x + 0.10f * fwd, head.y + 0.08f, head.z + side * 0.15f }).then (up), 0.04f, std::max (0.006f, eyeH), 0.03f, dark, 5, 3);
            if (p.hiss > 0.25f)        // cross brows
                r.limb (up.apply ({ head.x + 0.02f, head.y + 0.18f + 0.03f * p.hiss, side * 0.15f }), up.apply ({ head.x + 0.18f, head.y + 0.11f, side * 0.16f }), 0.022f, 0.018f, dark, 3);
        }
        if (hatAt != nullptr)
            *hatAt = Xform::move ({ head.x + 0.02f, head.y + 0.19f, 0.0f }).then (up);

        // Wings: hissing, they come half away from the body.
        const float flap = std::max (p.flap, 0.25f * p.hiss);
        for (float side : { -1.0f, 1.0f })
        {
            const Xform w = Xform::rotX (side * flap * -1.15f)
                                .then (Xform::move ({ -0.06f, bodyY + 0.07f + 0.22f * flap, side * (0.27f + 0.22f * flap) }))
                                .then (up);
            r.blob (w, 0.36f, 0.19f - 0.08f * flap, 0.07f + 0.10f * flap, wing, 7, 4, glow);
        }

        // Legs and feet: a waddle, and a slow shift from foot to foot while it stands.
        if (p.swim < 0.5f)
            for (float side : { -1.0f, 1.0f })
            {
                const float ph = p.walk + (side > 0.0f ? 0.0f : kPi);
                const float swing = 0.26f * p.walkAmt * std::sin (ph);
                const float lift = std::max (0.0f, 0.10f * p.walkAmt * std::cos (ph));
                const Vec3 hip { 0.02f, bodyY - 0.24f, side * 0.13f }, foot { 0.02f + swing, 0.03f + lift, side * (0.13f + 0.03f * p.walkAmt) };
                r.limb (at.apply (hip), at.apply (foot), 0.035f, 0.03f, orange, 4, glow);
                r.blob (Xform::move ({ foot.x + 0.08f, foot.y, foot.z }).then (at), 0.13f, 0.025f, 0.09f, orange, 5, 2, glow);
            }
    }

    void drawNotchGoose (int index, float glow)
    {
        const auto& g = geese[(size_t) index];
        float size = gooseSize (g);
        GoosePose pose;
        pose.shade = inBackRow (g) ? 1.0f - 0.55f * g.crowd : 1.0f;
        pose.blink = g.blink.closed();
        Vec3 pos { g.x + g.knockX, 0.0f, g.z };
        float yaw = g.yaw, sq = g.squash.x, tumble = 0.0f, tip = 0.0f;
        const float live = 1.0f - g.still;                           // out of focus it stands still, beak shut

        if (g.leaving && g.away < 0.0f)
        {
            // About to go: a crouch first.
            sq = 0.26f * smooth01 (1.0f + g.away / (8.0f * kF * 0.95f));
            pose.flap = 0.25f * sq / 0.26f;
        }
        else if (g.leaving)
        {
            // Off it goes, over the water.
            const float t = g.away;
            // Away from him, so that it never rises up behind his head.
            const float away = g.x >= keeper.x ? 1.0f : -1.0f;
            pos = { g.x + g.knockX + away * (3.5f * t + 0.6f * t * std::abs (std::sin (g.seed * 3.0f))), 0.2f + 4.5f * t * t + 1.2f * t, g.z + 9.0f * t };
            yaw = 1.5708f;
            pose.flap = 0.5f + 0.5f * std::sin (time * 26.0f + g.seed);
            pose.swim = 1.0f;
            sq = 0.0f;
        }
        else if (g.arrive < 1.0f)
        {
            // Hops up out of the water (and lands a little short of its place: see the slide below).
            const float t = g.arrive;
            pos.x -= std::cos (g.yaw) * 0.3f * cast;
            pos.z = g.z + (1.0f - t) * 1.6f;
            pos.y = 0.9f * std::sin (t * kPi);
            pose.flap = 0.4f + 0.6f * std::sin (t * kPi);
            sq = -0.12f * std::sin (t * kPi);                 // stretched in the air
        }
        else
        {
            // The slide as it lands.
            pos.x -= std::cos (g.yaw) * 0.3f * cast * g.skid * g.skid;
            sq += 0.12f * g.skid;
            pose.alert = g.alert * live;
            pose.honk = honkOpen (g) * (1.0f - g.hiss) * live * (index == keeper.target || index == ringGoose ? 1.0f : 0.7f);
            pose.preen = envelope (g.preen, 0.22f, 0.22f) * (g.preen < 1.0f ? 1.0f : 0.0f);
            // Waddling, it keeps its head forward rather than carry the neck out like a lance.
            pose.hiss = g.hiss * (1.0f - 0.6f * g.walkAmt);
            pose.walk = g.walkPhase;
            pose.walkAmt = g.walkAmt;
            pose.wag = g.wag * std::sin (g.wagPhase);
            // The hiss is aimed at the microphone, and the head stops well short of the man.
            {
                const float dx = std::abs (keeper.x - g.x);
                const float room = dx - 0.75f * cast - 0.6f;                             // to his body and arms, less a gap
                pose.hissLen = std::clamp (room / (0.9f * size) - 0.85f, 0.25f, 0.62f);
                const float headY = size * 0.72f;
                pose.aim = std::clamp (std::atan2 (micAt.y - headY, std::max (0.3f, std::abs (micAt.x - g.x) - size)), -0.4f, 0.4f);
            }
            // A deep notch: the one he is working (or the deepest) lifts its wings as it honks.
            if (index == keeper.target || index == ringGoose)
                pose.flap = std::clamp ((g.depth - 5.0f) / 3.0f, 0.0f, 1.0f) * (0.3f + 0.2f * std::sin (time * 7.0f + g.seed)) * (1.0f - g.hiss) * live * pose.honk;
            if (g.walkAmt < 0.2f)
            {
                pose.walk = 0.35f * std::sin (time * 1.7f + g.seed * 2.0f) * live;       // shifting from foot to foot
                pose.walkAmt = 0.25f * live;
            }
            // Jabbed: up off the ground in a temper, wings going, honking.
            if (g.flap < 1.0f)
            {
                const float e = bump (g.flap);
                pos.y += 0.42f * size * bump (std::min (1.0f, g.flap / 0.8f));
                pose.flap = std::max (pose.flap, e * (0.55f + 0.45f * std::sin (time * 22.0f)));
                pose.honk = std::max (pose.honk, e);
                pose.hiss = 0.0f;
            }
            // In a bit with him: see gooseBitAt.
            if (keeper.bit != Bit::none && keeper.bitGoose == index)
            {
                const GooseBit b = gooseBitAt (keeper.bit, keeper.bt / kF, keeper.bitRoom, keeper.bitVar);
                const float w = keeper.bitW, dir = keeper.bitDir;
                pos.x += dir * b.dx * w;
                pos.y += b.dy * size * w;
                sq += b.sq * w;
                tumble = (dir > 0.0f ? -1.0f : 1.0f) * b.tumble * w;
                pose.honk = lerp (pose.honk, b.honk, w);
                pose.flap = std::max (pose.flap, b.flap * w * (0.6f + 0.4f * std::sin (time * 24.0f)));
                pose.hiss = std::max (pose.hiss, b.low * w) * (1.0f - b.bite * w);
                pose.tall = b.tall * w;
                tip = b.lean * w;
                if (b.bite > 0.0f)
                {
                    // It has the windshield: beak shut on it, and turned to where the microphone is.
                    pose.bite = b.bite * w;
                    pose.honk *= 1.0f - pose.bite;
                    float to = std::atan2 (-(micAt.z - pos.z), micAt.x - pos.x);
                    while (to - yaw > kPi) to -= 2.0f * kPi;
                    while (to - yaw < -kPi) to += 2.0f * kPi;
                    yaw = lerp (yaw, to, pose.bite);
                }
                if (b.low > 0.0f)
                    pose.aim = lerp (pose.aim, -0.3f, b.low * w);
                if (b.ride > 0.0f)
                {
                    // On his pole, and then on his hat: it stands on him, a little smaller, facing him.
                    const float ride = b.ride * w;
                    const Vec3 on = b.u <= 1.0f ? lerp (rideTip, rideGrip, b.u) : lerp (rideGrip, rideHat, b.u - 1.0f);
                    const float lift = pos.y;
                    pos = lerp (Vec3 { pos.x, 0.0f, pos.z }, on, ride);
                    pos.y += lift;
                    size *= 1.0f - 0.2f * ride;
                    yaw = lerp (yaw, dir > 0.0f ? kPi : 0.0f, ride);
                    pose.walkAmt = lerp (pose.walkAmt, 0.0f, ride);
                    pose.hiss *= 1.0f - ride;
                }
            }
            // Snatching the hat: a stretch up at his head.
            if (g.thief && hat.state == 1)
                pose.reach = bump (hat.t / 0.5f);
            if (g.thief && (hat.state == 2 || hat.state == 4))
                pose.flap = std::max (pose.flap, 0.35f + 0.25f * std::sin (time * 20.0f));
            // Clicked: head over heels through the air, squawking; then it shakes itself and waddles back.
            if (g.knock < 1.0f)
            {
                const float t = g.knock;
                pos.y += 1.5f * size * std::sin (t * kPi);
                tumble = (g.knockTo > 0.0f ? -1.0f : 1.0f) * 6.2832f * smooth01 (t);
                pose.flap = 0.6f + 0.4f * std::sin (time * 24.0f);
                pose.honk = 1.0f; pose.hiss = 0.0f; pose.preen = 0.0f;
                pose.walkAmt = 0.0f;
            }
            else if (g.knockX < 0.0f || g.knockX > 0.0f)
            {
                pose.honk = 0.0f; pose.hiss = 0.0f; pose.flap = 0.0f;
                if (g.dizzy > 0.0f)
                    pose.wag = std::sin (time * 22.0f);                           // a shake of the tail
                else
                {
                    pose.walk = g.knockPhase;
                    pose.walkAmt = 1.0f;
                    yaw = g.knockX > 0.0f ? kPi - 0.3f : 0.3f;                    // back the way it came
                }
            }
        }
        addHot (Hot::goose, index, { pos.x, pos.y + 0.62f * size, pos.z }, 0.56f * size, 0.62f * size);
        Xform hatX;
        const Xform roll = Xform::move ({ 0.0f, -0.62f, 0.0f }).then (Xform::rotZ (tumble)).then (Xform::move ({ 0.0f, 0.62f, 0.0f }));
        drawGoose (squash (sq).then (roll).then (Xform::rotZ (tip)).then (Xform::scale (size)).then (Xform::rotY (yaw)).then (Xform::move (pos)), pose, glow,
                   index == hat.goose ? &hatX : nullptr);
        if (index == hat.goose)
            gooseHatAt = Xform::scale (1.8f * cast / size).then (hatX);           // a little larger on a goose, so it reads
    }

    /** One goose that lives on the pond and minds its own business, well out from the bank. Now
        and then (or when clicked) it tips up to feed, tail in the air. */
    void drawResident()
    {
        const float x = 5.0f * std::sin (residentPhase * 0.07f) - 1.0f, z = 7.5f + 1.5f * std::sin (residentPhase * 0.045f);
        const float heading = std::cos (residentPhase * 0.07f) >= 0.0f ? 0.0f : kPi;
        GoosePose pose;
        pose.swim = 1.0f;
        const float down = dabble < 1.0f ? envelope (dabble, 0.25f, 0.25f) : 0.0f;
        pose.wag = down * std::sin (time * 16.0f);
        const float s = 0.9f * cast;
        const Xform tip = Xform::move ({ -0.1f, -0.2f, 0.0f }).then (Xform::rotZ (-1.35f * down)).then (Xform::move ({ 0.1f, 0.2f - 0.1f * down, 0.0f }));
        drawGoose (tip.then (Xform::scale (s)).then (Xform::rotY (heading)).then (Xform::move ({ x, 0.02f * std::sin (time * 1.4f) * (1.0f - hush), z })), pose, 0.0f);
        addHot (Hot::resident, 0, { x, 0.4f * s, z }, 0.56f * s, 0.5f * s);
    }

    //============================================================================== the recordist
    // Toy proportions: a big square head, a short round body, stubby legs. His pose is worked out
    // in one place, so that what he does (where the microphone is) and what is drawn agree.
    struct KeeperRig
    {
        Xform body, upper, head;       // feet and legs; torso, arms and pole; the head (same coordinates as upper)
        float hipY = 0.62f, hipX = 0.0f, swing[2] {}, knee[2] {};
        float breath = 0.0f, cups = 0.0f, wide = 0.0f, hatDown = 0.0f, coil = 0.0f, fan = 0.0f, hatSquash = 0.0f;
        float frontRaw = 1.0f;         // 1 = the hand on his +z side leads on the pole
        Vec3 hatLag;                   // his hat trailing behind his head (world)
        Vec3 hand[2], elbow[2], grip, dir;
        float ang = 0, pyaw = 0, len = 2.1f;     // the pole's angle up, its heading against his, its length to the microphone
        float eyes = 1.0f, mouth = 0.0f, smile = 0.0f, happy = 0.0f, eyeShift = 0.0f, hatLift = 0.0f, hatTwist = 0.0f;
        float flask = 0.0f;            // how far the flask is out, in hand[flaskHand]
        int flaskHand = 0;
        float loop = 0.0f;             // the cable caught round his back boot, 0..1
        Vec3 drop { 0, -100, 0 };     // a drop of sweat flicked off, in upper coordinates
        Vec3 mic;                      // world
    };

    /** Holding the microphone out to the frog or the turtles, frame by frame. The pole comes down
        off his shoulder the way it went up; he turns to them, head first; the pole dips back a
        little and then comes round low in front of him, past its mark and back; he listens; and
        it all goes back the way it came. */
    struct OfferPose { float down = 0, turn = 0, back = 0, round = 0, low = 0, ear = 0; };
    OfferPose offerAt() const noexcept
    {
        OfferPose o;
        if (keeper.gesture != Gesture::offer)
            return o;
        const float end = keeper.glen / kF, f = keeper.gt * end;
        o.down = ramp (f, 0.0f, 26.0f) - ramp (f, end - 28.0f, end - 2.0f);
        o.turn = ramp (f, 10.0f, 24.0f) - ramp (f, end - 50.0f, end - 34.0f);
        o.back = pulse (f, 26.0f, 34.0f);
        o.round = (ramp (f, 32.0f, 52.0f) + 0.12f * wobble (f, 50.0f, 16.0f)) * (1.0f - ramp (f, end - 54.0f, end - 30.0f));
        o.low = pulse (f, 32.0f, 52.0f) + pulse (f, end - 54.0f, end - 30.0f);
        o.ear = ramp (f, 52.0f, 62.0f) - ramp (f, end - 66.0f, end - 56.0f);
        return o;
    }
    float offerTurn() const noexcept { return offerAt().turn; }

    KeeperRig keeperRig() const
    {
        const auto& k = keeper;
        KeeperRig g;
        const float sc = 1.5f * cast;                 // his size
        const float run = k.runAmt * (1.0f - k.sit);

        // The bit that is playing, as changes to the pose he holds (faded out if it is cut short).
        BitPose bp;
        if (k.bit != Bit::none)
        {
            bp = bitPoseAt (k.bit, k.bt / kF, k.bitVar);
            const float w = k.bitW;
            for (float* v : { &bp.pole, &bp.lean, &bp.sq, &bp.crouch, &bp.hipX, &bp.hop, &bp.footLift, &bp.backLift, &bp.spin, &bp.back, &bp.level, &bp.plant, &bp.aim,
                              &bp.draw, &bp.lift, &bp.flail, &bp.loop, &bp.headYaw, &bp.shut, &bp.hatLift, &bp.hatDown, &bp.cups, &bp.nod, &bp.wide, &bp.mouth, &bp.fan })
                *v *= w;
        }
        // Standing to work a goose: feet apart, weight going from one to the other; breathing.
        const float hold = k.stance * (1.0f - run) * (1.0f - k.sit);
        const float setOff = bump (k.setOff), skid = bump (k.skidT);
        g.breath = 0.02f * std::sin (k.idle * 1.571f) * (1.0f - run) * (1.0f - k.sit);
        // How he stands to the goose: upright; knees bent, leaning to it, a foot back; or low and
        // wide. Each honk rocks him back on his heels, and his shoulders go against his hips as
        // his weight shifts.
        const float mid = hold * smooth01 (k.stanceAt), deep = hold * smooth01 (k.stanceAt - 1.0f);
        const float rock = k.flinchAmt * ramp (k.flinchT, 0.0f, 0.2f) * (1.0f - ramp (k.flinchT, 0.2f, 1.0f)) * hold;
        float batLean = bp.lean - 0.16f * setOff - 0.24f * skid + 0.15f * mid + 0.1f * deep - 0.07f * k.weight * hold - 0.18f * rock;
        // His hat gone: a start, and a hand clapped to the top of his head. Getting it back with
        // the pole: out level to the goose's head, and a flick up (see stepHat).
        float slap = 0.0f, hook = 0.0f, flick = 0.0f;
        if (hat.state == 2)
            slap = ramp (hat.t / kF, 0.0f, 6.0f) - ramp (hat.t / kF, 16.0f, 26.0f);
        if (hat.state == 4)
        {
            hook = ramp (hat.t / kF, 0.0f, 10.0f) - ramp (hat.t / kF, 14.0f, 28.0f);
            flick = ramp (hat.t / kF, 10.0f, 15.0f) - ramp (hat.t / kF, 18.0f, 30.0f);
        }
        batLean += 0.26f * hook - 0.2f * flick - 0.1f * slap;
        // A shuffle step: where it has got him to, the hop in it, and how far through it is.
        const float stepOff = lerp (k.stepFrom, k.stepTo, smooth01 (k.stepT));
        const float stepHop = k.stepT < 1.0f ? 0.045f * bump ((k.stepT - 0.2f) / 0.6f) : 0.0f;
        // The pole on its way up onto his shoulder: his knees give as its weight lands.
        const OfferPose off = offerAt();
        const float restIn = k.rest * (1.0f - run) * (1.0f - k.sit);

        // The trip: the cable round his back boot, two skips on the front foot with his arms out
        // and the pole see-sawing, then he finds his feet with a small lean back.
        float tripPitch = 0.0f, tripHop = 0.0f, flail = 0.0f, caught = 0.0f, tripSquash = 0.0f;
        const float ts = k.trip * kTripSeconds;
        if (k.trip < 1.0f)
        {
            g.loop = 1.0f - smooth01 ((ts - 0.16f) / 0.12f);        // (a lunge keeps it there longer: see below)
            tripPitch = ts < 0.2f ? 0.45f * smooth01 (ts / 0.2f)
                      : ts < 0.56f ? 0.45f + 0.08f * std::sin ((ts - 0.2f) / 0.18f * kPi)
                                   : 0.45f * (1.0f - smooth01 ((ts - 0.56f) / 0.3f)) - 0.12f * bump ((ts - 0.7f) / 0.45f);
            tripHop = (0.1f / sc) * (bump ((ts - 0.2f) / 0.18f) + bump ((ts - 0.38f) / 0.18f));
            flail = smooth01 ((ts - 0.1f) / 0.1f) * (1.0f - smooth01 ((ts - 0.56f) / 0.35f));
            caught = 1.0f - smooth01 ((ts - 0.5f) / 0.3f);
            tripSquash = 0.08f * (bump ((ts - 0.35f) / 0.08f) + bump ((ts - 0.53f) / 0.12f)) + 0.05f * bump ((ts - 0.85f) / 0.2f);
        }

        // One small piece of business with the free hand.
        const float hs = k.hand;
        const int hi = hs > 0.0f ? 1 : 0;
        const float gt = k.gt;
        float gw = 0.0f, nod = 0.0f, pumpHop = 0.0f, pumpSquash = 0.0f, elbowW = 0.0f;
        float lookAdd = 0.0f, poleUp = 0.0f, upright = bp.plant, toMic = 0.0f, earUp = 0.0f;
        Vec3 target, elbowAt;
        float hipY = 0.62f - 0.4f * k.sit - (0.08f * mid + 0.07f * deep + bp.crouch + 0.05f * skid + 0.05f * hook + 0.035f * bump ((restIn - 0.84f) / 0.16f)) * (1.0f - k.sit);
        // At a run his hips drop at the full stretch of each stride, so that his feet stay on the grass.
        const float bob = -0.13f * run * std::abs (std::sin (k.phase)) + 0.012f * std::sin (k.idle * 1.6f) * (1.0f - run) + tripHop;

        if (k.gesture == Gesture::pump)
        {
            // A small hop with each of two quick pumps, landing with a squash, and a big grin.
            const float s = gt * k.glen;
            pumpHop = (0.08f / sc) * (bump ((s - 0.12f) / 0.15f) + bump ((s - 0.27f) / 0.15f));
            pumpSquash = 0.1f * (bump ((s - 0.25f) / 0.06f) + bump ((s - 0.40f) / 0.08f));
            g.happy = smooth01 (s / 0.1f) * (1.0f - smooth01 ((s - 0.6f) / 0.15f));
            g.smile = g.happy;
        }
        g.hipY = hipY + bob;
        hipY = g.hipY;
        g.hipX = (bp.hipX + 0.085f * k.weight * hold - 0.03f * mid - 0.03f * rock - 0.07f * setOff - 0.09f * skid) * (1.0f - k.sit);
        const Vec3 hc { 0.04f - 0.05f * k.flinch, hipY + 1.08f + 0.6f * g.breath, 0.0f };          // head centre

        switch (k.gesture)
        {
            case Gesture::pump:
            {
                // Fist to the chest (anticipation), then up above and in front of his head, on the
                // camera's side, twice; then it settles back down.
                const float s = gt * k.glen;
                gw = smooth01 (s / 0.08f) * (1.0f - smooth01 ((s - 0.45f) / 0.3f));
                const Vec3 chest { 0.34f, hipY + 0.5f, hs * 0.12f };
                const Vec3 cocked { 0.4f, hc.y + 0.02f, hs * 0.4f };
                const Vec3 high { 0.48f, hc.y + 0.62f, hs * 0.32f };
                const Vec3 base = lerp (chest, cocked, smooth01 ((s - 0.08f) / 0.08f));
                const float p = s > 0.12f && s < 0.42f ? std::abs (std::sin ((s - 0.12f) / 0.15f * kPi)) : 0.0f;
                target = lerp (base, high, p);
                elbowAt = { 0.3f, lerp (hipY + 0.55f, hipY + 0.85f, p), hs * 0.55f };
                elbowW = gw;
                break;
            }
            case Gesture::tap:
            {
                // A hand on the ear cup, elbow out, tapping it: is this thing on?
                gw = smooth01 (gt / 0.1f) * (1.0f - smooth01 ((gt - 0.42f) / 0.1f));
                const float a = std::clamp (gt / 0.45f, 0.0f, 1.0f);
                target = { hc.x - 0.03f, hc.y - 0.02f, hs * (0.42f + 0.05f * std::abs (std::sin (a * 3.0f * kPi))) };
                elbowAt = { hc.x + 0.12f, hc.y - 0.4f, hs * 0.6f };
                elbowW = gw;
                g.eyes = gt > 0.45f ? 1.0f : 0.8f;
                break;
            }
            case Gesture::hat:
            {
                // Finger and thumb on the front of the brim, elbow down and out, and a push: the
                // hat goes up a little past where it sits, and settles when he lets go.
                const float f = gt * k.glen / kF, end = k.glen / kF;
                gw = ramp (f, 0.0f, 10.0f) - ramp (f, end - 14.0f, end - 2.0f);
                g.hatLift = 0.06f * sprung (f, 10.0f, 9.0f) * (1.0f - ramp (f, end - 14.0f, end - 6.0f));
                g.hatTwist = 0.1f * wobble (f, 10.0f, 12.0f);
                target = { hc.x + 0.44f, hc.y + 0.17f + g.hatLift, hs * 0.1f };
                elbowAt = { hc.x + 0.62f, hc.y - 0.3f, hs * 0.3f };
                elbowW = gw;
                nod = 0.1f * gw;
                g.eyes = 1.0f - 0.3f * gw;
                break;
            }
            case Gesture::wipe:
            {
                gw = envelope (gt, 0.15f, 0.15f);
                const float u = smooth01 ((gt - 0.15f) / 0.45f), v = smooth01 ((gt - 0.6f) / 0.25f);
                target = lerp (Vec3 { hc.x + 0.42f, hc.y + 0.18f, hs * lerp (0.34f, -0.24f, u) }, Vec3 { 0.6f, hc.y - 0.05f, hs * 0.75f }, v);
                nod = 0.12f * bump ((gt - 0.1f) / 0.55f);
                g.eyes = 0.35f + 0.65f * (1.0f - bump ((gt - 0.15f) / 0.45f));
                if (gt > 0.62f && gt < 0.98f)
                {
                    const float d = (gt - 0.62f) / 0.36f;
                    g.drop = { 0.6f + 0.5f * d, hc.y - 0.05f + 0.35f * d - 0.9f * d * d, hs * (0.75f + 0.4f * d) };
                }
                break;
            }
            case Gesture::sip:
            {
                gw = envelope (gt, 0.25f, 0.25f);
                target = { hc.x + 0.42f, hc.y - 0.14f, hs * 0.08f };
                nod = -0.4f * gw;
                g.eyes = 1.0f - 0.8f * gw;
                break;
            }
            case Gesture::stretch:
            {
                // An arm right up, head back, eyes shut, and a yawn.
                gw = envelope (gt, 0.2f, 0.25f);
                target = { hc.x - 0.08f, hc.y + 0.78f, hs * 0.34f };
                nod = -0.26f * gw;
                g.eyes = 1.0f - 0.9f * gw;
                g.mouth = 0.75f * bump ((gt - 0.2f) / 0.55f);
                batLean = -0.12f * gw;
                break;
            }
            case Gesture::wave:
            {
                // A hand up beside his head, waggling, and a grin.
                gw = envelope (gt, 0.15f, 0.2f);
                target = { hc.x + 0.02f, hc.y + 0.3f, hs * (0.66f + 0.13f * std::sin (gt * k.glen * 11.0f)) };
                elbowAt = { 0.04f, hc.y - 0.3f, hs * 0.58f };
                elbowW = gw;
                g.smile = gw;
                break;
            }
            case Gesture::tip:
            {
                // His hat lifted clear of his head, with a small bow.
                gw = envelope (gt, 0.2f, 0.25f);
                const float u = (gt - 0.15f) / 0.7f;
                g.hatLift = 0.3f * bump (u);
                g.hatTwist = -0.35f * bump (u);
                target = { hc.x + 0.44f, hc.y + 0.17f + g.hatLift, hs * 0.1f };     // by the front of the brim
                elbowAt = { hc.x + 0.62f, hc.y - 0.3f + 0.5f * g.hatLift, hs * 0.3f };
                elbowW = gw;
                nod = 0.24f * bump (u);
                g.smile = gw;
                break;
            }
            case Gesture::shoulder:
            {
                // The pole goes up onto his shoulder (see `rest`), and he has a look round.
                gw = 0.5f * envelope (gt, 0.15f, 0.5f);
                target = { 0.3f, hipY + 0.62f, hs * 0.2f };
                lookAdd = 0.75f * std::sin ((gt - 0.4f) / 0.55f * 2.0f * kPi) * envelope ((gt - 0.4f) / 0.55f, 0.2f, 0.2f) * (gt > 0.4f && gt < 0.95f ? 1.0f : 0.0f);
                break;
            }
            case Gesture::coil:
            {
                // A loop of cable gathered up in his free hand, round and round.
                gw = envelope (gt, 0.12f, 0.15f);
                const float a = gt * k.glen * 7.5f;
                target = { 0.3f + 0.13f * std::cos (a), hipY + 0.26f + 0.13f * std::sin (a), hs * 0.36f };
                nod = 0.2f * gw;
                g.coil = gw;
                break;
            }
            case Gesture::polish:
            {
                // The microphone brought up in front of him: two taps on the windshield, and a wipe along it.
                gw = envelope (gt, 0.2f, 0.2f);
                poleUp = gw;
                toMic = gw;
                nod = -0.28f * gw;
                g.eyes = 1.0f - 0.25f * pulse (gt, 0.3f, 0.4f) - 0.25f * pulse (gt, 0.42f, 0.52f);
                break;
            }
            case Gesture::doze:
            {
                // Leaning on the pole, stood on its end; his head sinks, his eyes close, and he
                // comes to with a jerk.
                const float w = envelope (gt, 0.14f, 0.12f), sink = smooth01 ((gt - 0.15f) / 0.5f), jerk = smooth01 ((gt - 0.72f) / 0.035f);
                upright = std::max (upright, w);
                batLean += 0.13f * w;
                nod = 0.42f * sink * (1.0f - jerk) - 0.16f * pulse (gt, 0.72f, 0.86f);
                g.eyes = 1.0f - 0.92f * smooth01 ((gt - 0.25f) / 0.3f) * (1.0f - jerk);
                g.wide = pulse (gt, 0.72f, 0.9f);
                break;
            }
            case Gesture::offer:
            {
                // The microphone held out towards the stream, to the frog or a turtle, and a hand
                // to his ear to hear what it has to say.
                earUp = off.ear;
                nod = 0.12f * off.turn;
                break;
            }
            case Gesture::none:
                break;
        }

        // The body: running, he leans in; tripping, he pitches forward and catches himself; a
        // goose in his face, he leans back; listening, he hunches in. Looking about, his body
        // follows his head round a little, late.
        // Leaning is along the bank, the way he is going, whichever way his body is turned: a
        // turn into the direction of travel, a tilt, and back.
        // (A spin takes his whole body round, towards us; the lean stays along the bank.)
        const float spun = k.facing * bp.spin;
        const float towards = k.yaw + spun - (k.facing > 0.0f ? 0.0f : kPi);
        auto leanAlong = [towards] (float a) { return Xform::rotY (towards).then (Xform::rotZ (-a)).then (Xform::rotY (-towards)); };
        g.body = squash (pumpSquash + tripSquash + bp.sq - 0.07f * bump (slap)).then (leanAlong (0.22f * run)).then (Xform::scale (sc)).then (Xform::rotY (k.yaw + spun))
                     .then (Xform::move ({ k.x + stepOff, (pumpHop + bp.hop + stepHop) * sc, kKeeperZ }));
        const float torso = tripPitch - 0.36f * k.recoil + batLean;
        const float torsoYaw = 0.22f * std::clamp (k.torsoLook / 0.6f, -1.0f, 1.0f);
        g.upper = Xform::move ({ 0.0f, -hipY, 0.0f }).then (leanAlong (torso)).then (Xform::rotZ (-0.1f * k.solo)).then (Xform::rotY (torsoYaw))
                      .then (Xform::move ({ g.hipX, hipY, 0.0f })).then (g.body);
        // His head: down at the goose, up at the microphone, round towards us with a hand to his
        // headphones; back at a loud honk; and it leads when he turns, his body coming round after.
        const float lead = std::clamp (k.yawTo - k.yaw, -0.8f, 0.8f) * 0.7f * (1.0f - k.sit);
        const float tilt = 0.22f * k.solo * hs + 0.16f * k.lookEar * hold * hs;
        const float headYaw = k.look - torsoYaw + lookAdd + lead + k.facing * hold * (0.5f * k.lookEar + 0.14f * k.lookGoose - 0.08f * k.lookMic)
                              + k.facing * 0.5f * bump (std::clamp (k.fishLook / 0.9f, 0.0f, 1.0f)) + k.facing * bp.headYaw;
        nod += 0.1f * k.solo - 0.12f * k.recoil + bp.nod + hold * (0.13f * k.lookGoose - 0.15f * k.lookMic) - 0.12f * k.flinch - 0.1f * rock + 0.12f * slap + 0.1f * hook - 0.2f * flick;
        g.head = Xform::move ({ -hc.x, -hc.y, 0.0f }).then (Xform::rotZ (-nod)).then (Xform::rotX (tilt)).then (Xform::rotY (headYaw)).then (Xform::move (hc)).then (g.upper);
        g.eyeShift = -0.09f * std::clamp ((k.eyeLook - k.look) / 0.4f, -1.0f, 1.0f);
        g.hatLift += bp.hatLift + 0.12f * std::max (0.0f, k.hatHop.x);
        g.hatDown = bp.hatDown;
        g.cups = bp.cups;
        g.wide = std::max ({ g.wide, bp.wide, k.flinch, slap, flick });
        g.hatSquash = 0.4f * bump (k.hatLand * 2.5f);
        g.fan = bp.fan;
        g.hatLag = { k.hatLag.x, 0.0f, 0.0f };

        // Legs. Running, they swing from the hip. Standing, his feet stay where they are on the
        // grass and his knees take up whatever his hips do: a crouch, a shift of weight, a lean.
        // Caught, the back foot is held behind him while he skips on the front one.
        const float planted = (1.0f - run) * (1.0f - k.sit) * (1.0f - caught);
        for (int s = 0; s < 2; ++s)
        {
            const float ph = k.phase + (s == 1 ? 0.0f : kPi);
            g.swing[s] = (1.0f - k.sit) * (0.75f * run * std::sin (ph) * (1.0f - caught) + (s == 1 ? -0.9f : 0.3f) * caught) + k.sit * 1.5f;
            // The knee bends as the leg comes forward, and a little as the foot takes his weight.
            g.knee[s] = (1.0f - k.sit) * (1.0f - caught) * run * (0.18f + 1.05f * std::max (0.0f, std::cos (ph)));
            if (planted > 0.01f)
            {
                const float front = s == 0 ? 1.0f : 0.0f;
                float fx = (s == 0 ? 0.13f : -0.13f) * hold + front * (0.1f * setOff + 0.06f * bp.footLift / 0.16f + 0.03f * mid + 0.09f * deep + 0.1f * hook) + 0.12f * skid * front
                           - (1.0f - front) * (0.14f * mid + 0.07f * deep + 0.5f * bp.backLift);
                float fy = 0.05f + front * (bp.footLift + 0.15f * setOff) + (1.0f - front) * bp.backLift;
                if (k.stepT < 1.0f)
                {
                    // The foot on the side he is going leads, and the other comes after it.
                    const float along = std::cos (k.yaw), way = (k.stepTo - k.stepFrom) * along;
                    const float u = k.stepT * 1.7f - ((way > 0.0f) == (s == 0) ? 0.0f : 0.7f);
                    if (std::abs (along) > 0.3f)
                        fx += (lerp (k.stepFrom, k.stepTo, smooth01 (u)) - stepOff) / (sc * along);
                    fy += 0.07f * bump (u);
                }
                const float dx = fx - g.hipX, dy = hipY - fy;
                const float d = std::clamp (std::sqrt (dx * dx + dy * dy), 0.12f, 0.569f);
                const float thigh = std::atan2 (dx, dy) + std::acos (std::clamp ((0.09f + d * d - 0.0729f) / (0.6f * d), -1.0f, 1.0f));
                const float bend = kPi - std::acos (std::clamp ((0.09f + 0.0729f - d * d) / 0.162f, -1.0f, 1.0f));
                g.swing[s] = lerp (g.swing[s], thigh, planted);
                g.knee[s] = lerp (g.knee[s], bend, planted);
            }
        }

        // The pole: out to the side he is facing, pointed at the goose. Seated, it lies on the
        // grass beside him.
        // Held out over the goose he is working, with the slow drift of a pole held at arm's length;
        // carried upright at a run.
        float ang = 0.45f + k.shoo * (-0.25f + 0.02f * std::sin (k.idle * 1.1f)) + 0.2f * k.recoil - 0.18f * gw + 0.45f * flail * std::sin (ts * 2.0f * kPi * 2.6f);
        // (Offering the microphone to the stream, the pole comes round towards us: first a little
        // the other way, then round, past its mark and back.)
        const float round = std::clamp (off.round, 0.0f, 1.0f);
        g.pyaw = k.poleYaw - k.yaw + k.facing * (1.25f * off.round - 0.12f * off.back);
        g.grip = { 0.38f + 0.28f * k.recoil - 0.14f * run - bp.back, hipY + 0.44f - 0.12f * k.sit + g.breath, 0.0f };
        g.len = 2.1f + k.extend * (1.0f - k.sit);
        auto aim = [&] (float a)
        {
            g.ang = a;
            g.dir = { std::cos (a) * std::cos (g.pyaw), std::sin (a), -std::cos (a) * std::sin (g.pyaw) };
        };
        // Working a goose, the microphone hangs where stepKeeper says: over its head, a moment
        // behind it (or at its beak; or, when it has his hat, at the hat). Solved in the pole's own
        // upright plane: how far along the bank and how high, from his hands.
        const float handFlail = std::max (flail, bp.flail);
        const float aimW = std::max (k.aimW * (1.0f - 0.6f * k.solo), hook) * (1.0f - run) * (1.0f - k.sit) * (1.0f - flail);
        if (aimW > 0.01f)
        {
            Vec3 to = lerp (Vec3 { k.micX.x, k.micY.x, kKeeperZ }, biteAt, bp.aim) + Vec3 { -k.bitDir * bp.draw * sc, bp.lift * sc, 0.0f };
            to = lerp (to, gooseHatAt.t + Vec3 { 0.0f, 0.1f * sc, 0.0f }, hook / std::max (hook, std::max (aimW, 1.0e-3f)));
            const Vec3 e = g.upper.turn ({ std::cos (g.pyaw), 0.0f, -std::sin (g.pyaw) }), u = g.upper.turn ({ 0.0f, 1.0f, 0.0f });
            const Vec3 rel = to - g.upper.apply (g.grip);
            float det = e.x * u.y - e.y * u.x;
            if (std::abs (det) < 0.25f * sc * sc)
                det = std::copysign (0.25f * sc * sc, std::abs (det) > 1.0e-6f ? det : k.facing);
            const float cx = std::max (lerp (0.45f, 0.3f, bp.aim), (rel.x * u.y - rel.y * u.x) / det), cy = (e.x * rel.y - e.y * rel.x) / det;
            ang = lerp (ang, std::atan2 (cy, cx), aimW);
            // (Put right to a goose close by, the pole slides back through his hands.)
            g.len = lerp (g.len, std::clamp (std::sqrt (cx * cx + cy * cy), lerp (1.2f, 0.55f, std::max (bp.aim, hook)), 3.2f), aimW);
        }
        ang += bp.pole + k.tip.x + 0.03f * bump (k.grip) * hold + 0.8f * flick;
        ang = lerp (ang, -0.04f, bp.level);
        ang = lerp (ang, 1.0f, run * (1.0f - flail));
        ang = lerp (ang, -0.19f, k.sit);
        ang = lerp (ang, 0.25f, k.solo * 0.6f);
        ang = lerp (ang, -0.3f, round) + 0.1f * off.back - 0.14f * off.low;
        // Stood on its end beside him; brought up in front of his face; laid back over his shoulder.
        ang = lerp (ang, 1.47f, upright);
        ang = lerp (ang, 1.12f, poleUp);
        g.len = lerp (g.len, 1.5f, std::max (poleUp, 0.6f * upright));
        g.grip = lerp (g.grip, Vec3 { 0.3f, hipY + 0.5f, 0.0f }, upright);
        // Onto his shoulder, never over the top of him: first up on its end out in front of him,
        // drawn in short; a beat; then tipped back past his ear on our side, and let out again.
        const float rest = restIn * (1.0f - poleUp) * (1.0f - upright) * (1.0f - off.down);
        {
            const float up = smooth01 (rest / 0.4f), over = smooth01 ((rest - 0.52f) / 0.34f);
            ang = lerp (lerp (ang, 1.47f, up), 2.62f, over);
            g.len = lerp (g.len, 1.4f, up * (1.0f - over));
            g.grip = lerp (lerp (g.grip, Vec3 { 0.62f, hipY + 0.5f, -hs * 0.1f }, up), Vec3 { 0.14f, hipY + 0.6f, hs * (0.2f + 0.34f * bump (over)) }, over);
        }
        aim (ang);
        // Standing, the microphone stays inside the picture: above the water's edge (never down
        // among the creatures in the stream), clear of the top edge, and clear of the sides (the
        // pole swings round away from us, behind the geese, rather than out of the picture). The
        // nearer the pole is to upright the less of this there is, and none once it is on its end.
        const float keepIn = smooth01 ((std::cos (g.ang) - 0.05f) / 0.3f);
        if (k.sit < 0.5f && keepIn > 0.0f)
            for (int pass = 0; pass < 4; ++pass)
            {
                const Vec3 m = g.upper.apply (g.grip + g.dir * g.len);
                const float rad = 0.17f * sc;
                const float lowY = r.heightAtRow (kBankRow * (float) r.height, m.z) + rad;
                const float topY = r.heightAtRow (0.04f * (float) r.height, m.z) - rad;
                const float need = keepIn * (m.y < lowY ? lowY - m.y : m.y > topY ? topY - m.y : 0.0f);
                if (std::abs (need) >= 1.0e-3f)
                    aim (std::asin (std::clamp (std::sin (g.ang) + need * (1.0f - 2.0f * k.sit) / (g.len * sc), -0.97f, 0.97f)));
                const Vec3 m2 = g.upper.apply (g.grip + g.dir * g.len);
                const auto p0 = r.project (m2), p1 = r.project (m2 + Vec3 { 1.0f, 0.0f, 0.0f });
                if (! p0.ok || ! p1.ok)
                    break;
                const float perUnit = std::max (1.0f, p1.x - p0.x), margin = 0.62f * sc * perUnit + 4.0f;
                const float over = keepIn * (p0.x < margin ? (margin - p0.x) / perUnit : p0.x > (float) r.width - margin ? ((float) r.width - margin - p0.x) / perUnit : 0.0f);
                if (std::abs (over) < 1.0e-3f)
                    continue;
                // Swing it round: the pole's heading P across the bank, with (cos P, -sin P) its way,
                // always away from us. Only so far, though: past that, the pole is drawn in a little.
                const float P = g.pyaw + k.yaw, reachX = std::max (0.05f, g.len * sc * std::cos (g.ang));
                const float wantCos = std::cos (P) + 1.2f * over / reachX;
                const float cosP = std::clamp (wantCos, -0.98f, 0.98f), limit = std::cos (0.8f);
                const float cosUsed = std::copysign (std::max (std::abs (cosP), limit), std::cos (P));
                g.pyaw = -std::acos (cosUsed) - k.yaw;
                if (std::abs (cosP) < limit)
                    g.len = std::max (1.5f, g.len - (limit - std::abs (cosP)) * reachX / sc);
                aim (g.ang);
            }
        // His hands on the pole: one ahead of the other, and they change places smoothly when the
        // pole comes across him.
        g.frontRaw = -std::sin (g.pyaw) > 0.0f ? 1.0f : 0.0f;
        const int front = k.frontW > 0.5f ? 1 : 0;
        {
            const Vec3 ahead = g.grip + g.dir * (0.22f + 0.22f * k.recoil + 0.07f * bump (k.grip) * hold), behind = g.grip - g.dir * 0.26f;
            g.hand[1] = lerp (behind, ahead, smooth01 (k.frontW));
            g.hand[0] = lerp (ahead, behind, smooth01 (k.frontW));
        }
        if (rest > 0.01f)
        {
            // Over his shoulder: one hand on the end in front of him, the other at his side.
            const float w = smooth01 ((rest - 0.52f) / 0.34f);
            g.hand[1 - hi] = lerp (g.hand[1 - hi], Vec3 { 0.05f, hipY + 0.12f, -hs * 0.34f }, w);
            g.hand[hi] = lerp (g.hand[hi], g.grip - g.dir * 0.34f, w);
        }
        // Tripping, or losing his balance: the back hand comes off the pole and out, and goes round.
        {
            const float side = (1 - front) == 1 ? 1.0f : -1.0f, a = k.bt * 15.0f;
            const Vec3 out { 0.22f * std::cos (a) * bp.flail, hipY + 0.78f + 0.22f * std::sin (a) * bp.flail, side * 0.8f };
            g.hand[1 - front] = lerp (g.hand[1 - front], out, handFlail);
        }

        // Listening hard: a hand cupped over one headphone, elbow out.
        const Vec3 ear { hc.x - 0.02f, hc.y - 0.03f, hs * 0.42f };
        const float cup = std::max ({ k.solo, k.lookEar * hold, earUp }) * (1.0f - gw);
        g.hand[hi] = lerp (g.hand[hi], ear, cup);
        g.eyes = std::min (g.eyes, 1.0f - 0.55f * k.solo);

        // Sitting: the flask in his free hand, in his lap until he takes a sip.
        if (k.sit > 0.5f)
        {
            g.flask = smooth01 ((k.sit - 0.5f) * 2.0f);
            g.flaskHand = hi;
            g.hand[hi] = lerp (g.hand[hi], Vec3 { 0.36f, hipY + 0.3f, hs * 0.2f }, g.flask);
        }
        // At a run the free arm pumps, against the leg on its side.
        if (run > 0.02f)
        {
            const float sw = std::sin (k.phase + (hi == 1 ? kPi : 0.0f));
            g.hand[hi] = lerp (g.hand[hi], Vec3 { 0.08f + 0.32f * sw, hipY + 0.3f + 0.1f * std::max (0.0f, sw), hs * 0.36f }, run * (1.0f - flail) * (1.0f - cup));
        }
        if (toMic > 0.0f)
        {
            const float s = gt * k.glen;
            target = g.grip + g.dir * (g.len - 0.12f + (gt < 0.55f ? 0.0f : 0.2f * std::sin ((gt - 0.55f) * k.glen * 9.0f)))
                     + Vec3 { 0.0f, -0.16f - 0.07f * std::abs (std::sin (std::min (s, 0.55f * k.glen) * 9.0f)) * (gt < 0.55f ? 1.0f : 0.0f), hs * 0.1f };
        }
        g.hand[hi] = lerp (g.hand[hi], target, gw);
        g.hand[hi] = lerp (g.hand[hi], Vec3 { hc.x - 0.02f, hc.y + 0.37f, hs * 0.08f }, slap);        // clapped to the top of his bare head

        // Elbows: bent a little outwards and down (well out, standing low to a deep cut), unless a
        // gesture places one.
        for (int s = 0; s < 2; ++s)
        {
            const float side = s == 1 ? 1.0f : -1.0f;
            const Vec3 shoulder { 0.0f, hipY + 0.52f + g.breath, side * 0.27f };
            g.elbow[s] = (shoulder + g.hand[s]) * 0.5f + Vec3 { -0.05f - 0.04f * deep, -0.1f + 0.04f * deep, side * (0.1f + 0.12f * deep) };
        }
        g.elbow[hi] = lerp (g.elbow[hi], elbowAt, elbowW);
        g.elbow[hi] = lerp (g.elbow[hi], Vec3 { -0.22f, hc.y - 0.3f, hs * 0.5f }, slap);
        g.elbow[hi] = lerp (g.elbow[hi], Vec3 { hc.x + 0.1f, hc.y - 0.4f, hs * 0.62f }, cup * (1.0f - elbowW));

        g.eyes = std::min ({ g.eyes, 1.0f - k.blink.closed(), 1.0f - bp.shut });
        g.mouth = std::max ({ g.mouth, 0.9f * k.recoil, 0.6f * flail, bp.mouth, 0.5f * k.flinch, 0.8f * slap, 0.4f * rock });
        g.loop = std::max (g.loop, bp.loop);
        g.mic = g.upper.apply (g.grip + g.dir * g.len);
        return g;
    }

    void drawKeeper()
    {
        const Rgb jacket = rgb (0xe5533b), skin = rgb (0xf6d6b4), trousers = rgb (0x4a8a90), boots = rgb (0x3a2e2a);
        const Rgb cans = rgb (0x3a2e2a), band = rgb (0xf2c14e), fur = rgb (0xb0a694), hair = rgb (0x3a2e2a);
        const KeeperRig g = keeperRig();
        const float hipY = g.hipY;
        const Xform& at = g.body;
        const Xform& up = g.upper;
        const Xform& hd = g.head;
        const float sc = 1.5f * cast;
        addHot (Hot::keeper, 0, { keeper.x, 0.95f * sc, kKeeperZ }, 0.5f * sc, 1.0f * sc);

        // In Solo Honk, a warm pool of light on the grass where he stands.
        if (keeper.solo > 0.02f)
        {
            const Rgb pool = rgb (0xf2c14e);
            const Vec3 c { keeper.x, 0.015f, kKeeperZ - 0.2f };
            for (int i = 0; i < 12; ++i)
            {
                const float a0 = 6.2831853f * (float) i / 12.0f, a1 = 6.2831853f * (float) (i + 1) / 12.0f;
                r.triangle (c, c + Vec3 { std::cos (a0) * 0.85f * sc, 0.0f, std::sin (a0) * 0.45f * sc }, c + Vec3 { std::cos (a1) * 0.85f * sc, 0.0f, std::sin (a1) * 0.45f * sc },
                            pool, pool, pool, 0.32f * keeper.solo);
            }
        }

        // Legs: short, straight, swinging from the hip.
        Vec3 backFoot = at.t;
        for (int s = 0; s < 2; ++s)
        {
            const float side = s == 1 ? 1.0f : -1.0f;
            const Vec3 hip { g.hipX, hipY, side * 0.16f };
            const float thigh = g.swing[s], shin = thigh - g.knee[s];
            const Vec3 knee { hip.x + std::sin (thigh) * 0.3f, hip.y - std::cos (thigh) * 0.3f, hip.z };
            const Vec3 foot { knee.x + std::sin (shin) * 0.27f, std::max (0.08f, knee.y - std::cos (shin) * 0.27f), hip.z };
            r.limb (at.apply (hip), at.apply (knee), 0.12f, 0.115f, trousers, 6);
            r.limb (at.apply (knee), at.apply (foot), 0.115f, 0.105f, trousers, 6);
            if (g.knee[s] > 0.25f)
                r.blob (Xform::move (knee).then (at), 0.115f, 0.115f, 0.115f, trousers, 6, 4);
            // The boot tips toe-down as the foot swings through.
            r.box (Xform::rotZ (-0.5f * std::min (1.0f, g.knee[s])).then (Xform::move ({ foot.x + 0.08f, foot.y - 0.01f, foot.z })).then (at), 0.18f, 0.08f, 0.11f, boots);
            if (s == 1)
                backFoot = at.apply (foot);
        }

        // The cable from his belt, trailing along the grass. When it trips him it is looped round
        // his back boot.
        {
            const Vec3 belt = at.apply ({ -0.3f, hipY + 0.05f, 0.0f });
            const float trail = keeper.facing;
            Vec3 a { keeper.cableX + trail * 0.35f, 0.03f, kKeeperZ - 0.2f };
            a = lerp (a, backFoot + Vec3 { 0.0f, 0.05f, 0.0f }, g.loop);
            const float wave = keeper.cableWave.x;          // a wave runs along it when he turns
            const Vec3 b { keeper.cableX - trail * 0.9f, 0.03f + 0.22f * std::max (0.0f, wave), kKeeperZ - 0.3f - 0.25f * wave }, c { keeper.cableX - trail * 3.0f, 0.03f, kKeeperZ - 0.4f };
            const Rgb lead = rgb (0x3a2e2a);
            r.limb (belt, lerp (belt, a, 0.55f) + Vec3 { 0.0f, -0.12f, 0.0f }, 0.03f, 0.03f, lead, 3);
            r.limb (lerp (belt, a, 0.55f) + Vec3 { 0.0f, -0.12f, 0.0f }, a, 0.03f, 0.03f, lead, 3);
            r.limb (a, b, 0.03f, 0.03f, lead, 3);
            r.limb (b, c, 0.03f, 0.03f, lead, 3);
            if (g.loop > 0.02f)
            {
                const Vec3 o = backFoot + Vec3 { 0.0f, 0.06f * sc, 0.0f };
                const float rad = 0.2f * sc * g.loop;
                Vec3 prev = o + Vec3 { rad, 0.0f, 0.0f };
                for (int i = 1; i <= 7; ++i)
                {
                    const float a2 = 6.2831853f * (float) i / 7.0f;
                    const Vec3 p = o + Vec3 { std::cos (a2) * rad, std::sin (a2) * rad * 0.7f, std::sin (a2) * rad * 0.5f - 0.05f };
                    r.limb (prev, p, 0.035f, 0.035f, lead, 3);
                    prev = p;
                }
            }
        }

        // Body: a short tub.
        r.limb (up.apply ({ 0.0f, hipY - 0.05f, 0.0f }), up.apply ({ 0.0f, hipY + 0.62f, 0.0f }), 0.36f, 0.27f, jacket, 9);
        r.blob (Xform::move ({ 0.0f, hipY + 0.62f, 0.0f }).then (up), 0.27f, 0.10f, 0.27f, jacket, 9, 3);

        // Head: one big round head, a little hair showing at the back under the hat, dot eyes
        // and a button nose on the front, a headphone cup over each ear with the band going up
        // under the hat, and the hat sitting down on top.
        const Vec3 head { 0.04f - 0.05f * keeper.flinch, hipY + 1.08f + 0.6f * g.breath, 0.0f };
        constexpr float hx = 0.33f, hy = 0.32f, hz = 0.31f;
        auto faceX = [&] (float y, float z) { return hx * std::sqrt (std::max (0.0f, 1.0f - (y * y) / (hy * hy) - (z * z) / (hz * hz))); };
        r.blob (Xform::move (head).then (hd), hx, hy, hz, skin, 12, 7);
        r.blob (Xform::move ({ head.x - 0.07f, head.y + 0.1f, 0.0f }).then (hd), 0.32f, 0.24f, 0.315f, hair, 10, 5);
        for (float side : { -1.0f, 1.0f })
        {
            const float ez = side * 0.13f + g.eyeShift;
            if (g.happy > 0.5f)
                happyEye (hd, { head.x + faceX (0.04f, side * 0.13f) + 0.01f, head.y + 0.04f, side * 0.13f }, 0.055f, 0.028f, 0.017f, cans);      // eyes shut tight, grinning
            else
                r.blob (Xform::move ({ head.x + faceX (0.04f, ez) - 0.005f, head.y + 0.04f, ez }).then (hd), 0.03f, std::max (0.008f, 0.075f * g.eyes * (1.0f + 0.45f * g.wide)), 0.05f * (1.0f + 0.2f * g.wide), cans, 5, 3, 0.5f);
            r.blob (Xform::move ({ head.x - 0.03f, head.y - 0.03f, side * (hz + 0.02f + 0.16f * g.cups) }).then (hd), 0.13f, 0.14f, 0.07f, band, 9, 4);           // ear cups (a honk in his face springs them out)
            r.limb (hd.apply ({ head.x - 0.03f, head.y + 0.08f, side * (hz + 0.03f) }), hd.apply ({ head.x - 0.03f, head.y + 0.25f, side * 0.22f }), 0.03f, 0.03f, band, 5);   // the band, up under the hat
        }
        r.blob (Xform::move ({ head.x + faceX (-0.04f, 0.0f), head.y - 0.04f, 0.0f }).then (hd), 0.05f, 0.045f, 0.05f, skin * 0.96f, 6, 4);     // nose
        {
            const float mx = head.x + faceX (-0.15f, 0.0f) - 0.01f;
            if (g.smile > 0.3f)
                r.blob (Xform::move ({ mx, head.y - 0.15f, 0.0f }).then (hd), 0.02f, 0.03f, 0.12f, rgb (0xc04a4a), 7, 3);                      // a wide grin
            else
                r.blob (Xform::move ({ mx, head.y - 0.15f - 0.02f * g.mouth, 0.0f }).then (hd), 0.02f, 0.022f + 0.05f * g.mouth, 0.075f - 0.015f * g.mouth, rgb (0xc04a4a), 7, 3);     // mouth
        }
        // His hat: a little behind his head whenever he moves, and pushed down over his eyes when a
        // goose is standing on it.
        keeperHatAt = squash (0.3f * g.hatDown + g.hatSquash).then (Xform::rotY (g.hatTwist)).then (Xform::rotZ (-0.42f * g.hatDown - 0.5f * g.hatLag.x * (keeper.facing > 0.0f ? 1.0f : -1.0f)))
                          .then (Xform::move ({ head.x - 0.03f + 0.13f * g.hatDown, head.y + 0.2f + g.hatLift - 0.06f * g.hatDown, 0.0f })).then (hd).then (Xform::move (g.hatLag));

        // The pole, with a big fluffy end out in front.
        // It telescopes: a thinner second section slides out for a goose further off.
        const Vec3 back = g.grip - g.dir * (0.7f + 0.6f * std::max (0.0f, 1.2f - g.len)), joint = g.grip + g.dir * std::min (1.3f, std::max (0.1f, g.len - 0.3f)), tip = g.grip + g.dir * (g.len - 0.2f);
        r.limb (up.apply (back), up.apply (joint), 0.04f, 0.035f, rgb (0x505868), 5);
        r.limb (up.apply (joint), up.apply (tip), 0.026f, 0.024f, rgb (0x6a7282), 5);
        r.blob (Xform::rotZ (g.ang).then (Xform::rotY (g.pyaw)).then (Xform::move (g.grip + g.dir * g.len)).then (up), 0.34f, 0.17f, 0.17f, fur, 8, 5);
        if (g.fan > 0.0f && keeper.bit != Bit::none)
        {
            // The fastest frames of a swing: the pole shown where it was a frame and two frames
            // ago as well, paler, and the sweep between filled in, so that the stroke can be seen.
            const float f = keeper.bt / kF, now = bitPoseAt (keeper.bit, f, keeper.bitVar).pole;
            const Rgb pale = rgb (0xfffdf6);
            Vec3 last = up.apply (g.grip + g.dir * g.len);
            const Vec3 hub = up.apply (g.grip + g.dir * 0.3f);
            for (int i = 1; i <= 2; ++i)
            {
                const float a = g.ang + (bitPoseAt (keeper.bit, f - 1.1f * (float) i, keeper.bitVar).pole - now) * keeper.bitW;
                const Vec3 d { std::cos (a) * std::cos (g.pyaw), std::sin (a), -std::cos (a) * std::sin (g.pyaw) };
                const Vec3 end = up.apply (g.grip + d * g.len), mid = up.apply (g.grip + d * (g.len * 0.5f));
                r.triangle (hub, last, end, pale, pale, pale, 0.34f / (float) i);
                r.limb (mid, end, 0.03f, 0.03f, rgb (0xb9bfca), 4, 0.5f);
                r.blob (Xform::rotZ (a).then (Xform::rotY (g.pyaw)).then (Xform::move (g.grip + d * g.len)).then (up), 0.3f, 0.13f, 0.13f, rgb (0xd9d2c4), 6, 4, 0.5f);
                last = end;
            }
        }

        // Arms: two short pieces from the shoulders, bent at the elbow, with round mitts.
        for (int s = 0; s < 2; ++s)
        {
            const float side = s == 1 ? 1.0f : -1.0f;
            const Vec3 shoulder { 0.0f, hipY + 0.52f + g.breath, side * 0.27f };
            r.limb (up.apply (shoulder), up.apply (g.elbow[s]), 0.09f, 0.085f, jacket, 6);
            r.limb (up.apply (g.elbow[s]), up.apply (g.hand[s]), 0.085f, 0.08f, jacket, 6);
            r.blob (Xform::move (g.hand[s]).then (up), 0.1f, 0.1f, 0.1f, skin, 6, 4);
        }
        if (g.flask > 0.01f)
        {
            const Vec3 h = g.hand[g.flaskHand];
            const Vec3 to = keeper.gesture == Gesture::sip ? normalised (Vec3 { head.x + 0.6f, head.y + 0.2f, 0.0f } - h) : Vec3 { 0.0f, 1.0f, 0.0f };
            const float s = g.flask;
            r.limb (up.apply (h - to * (0.1f * s)), up.apply (h + to * (0.14f * s)), 0.075f * s, 0.075f * s, rgb (0xb9a8e0), 7);
            r.limb (up.apply (h + to * (0.14f * s)), up.apply (h + to * (0.2f * s)), 0.035f * s, 0.035f * s, rgb (0x505868), 5);
        }
        if (g.drop.y > -50.0f)
            r.blob (Xform::move (g.drop).then (up), 0.04f, 0.06f, 0.04f, rgb (0x8fd0e2), 5, 3, 0.6f);
        if (g.coil > 0.02f)
        {
            // The loop of cable he is gathering, hanging from his hand.
            const int hi = keeper.hand > 0.0f ? 1 : 0;
            const Vec3 o = up.apply (g.hand[hi] + Vec3 { 0.0f, -0.2f * g.coil, 0.0f });
            const float rad = 0.17f * sc * g.coil;
            Vec3 prev = o + Vec3 { rad, 0.0f, 0.0f };
            for (int i = 1; i <= 8; ++i)
            {
                const float a2 = 6.2831853f * (float) i / 8.0f;
                const Vec3 pt = o + Vec3 { std::cos (a2) * rad * 0.7f, std::sin (a2) * rad, 0.0f };
                r.limb (prev, pt, 0.03f, 0.03f, rgb (0x3a2e2a), 3);
                prev = pt;
            }
        }
    }

    /** His hat, wherever it is: on his head, on a goose, or flying between the two. */
    void drawHat()
    {
        Xform x = keeperHatAt;
        if (hat.state != 0 && hat.goose >= 0)
        {
            const Vec3 a = keeperHatAt.t, b = gooseHatAt.t;
            const float f = hat.t / kF;
            if (hat.state == 2 || (hat.state == 4 && f < 10.0f))
                x = gooseHatAt;
            else if (hat.state == 4 && f < 14.0f)
                x = Xform::rotZ (0.25f * keeper.facing).then (Xform::scale (1.7f * cast)).then (Xform::move (hat.from));     // on the end of his pole
            else if (hat.state == 4)
            {
                // Flicked up off the pole: over, once, and down onto his head.
                const float t = std::clamp ((f - 14.0f) / 14.0f, 0.0f, 1.0f);
                const Vec3 p = lerp (hat.from, a, t) + Vec3 { 0.0f, 1.5f * cast * 4.0f * t * (1.0f - t), 0.0f };
                x = Xform::rotZ (-6.2832f * smooth01 (t) * keeper.facing).then (Xform::scale (cast * lerp (1.7f, 1.5f, t))).then (Xform::move (p));
            }
            else
            {
                const float t = hat.state == 1 ? smooth01 ((hat.t - 0.15f) / 0.35f) : smooth01 (hat.t / 0.6f);
                const float s = cast * (hat.state == 1 ? lerp (1.5f, 1.8f, t) : lerp (1.8f, 1.5f, t));
                const Vec3 from = hat.state == 1 ? a : b, to = hat.state == 1 ? b : a;
                const Vec3 p = lerp (from, to, t) + Vec3 { 0.0f, 0.9f * s * bump (t) * (hat.state == 3 ? 1.0f : 0.4f), 0.0f };
                x = Xform::rotZ (hat.state == 3 ? 6.2832f * t : 0.6f * bump (t)).then (Xform::scale (s)).then (Xform::move (p));
            }
        }
        const Rgb felt = rgb (0xb9a8e0), ribbon = rgb (0xf4b6c6);
        r.blob (Xform::move ({ 0.0f, 0.09f, 0.0f }).then (x), 0.29f, 0.16f, 0.29f, felt, 9, 4);
        r.limb (x.apply ({ 0.0f, 0.04f, 0.0f }), x.apply ({ 0.0f, 0.09f, 0.0f }), 0.3f, 0.29f, ribbon, 9);
        r.blob (x, 0.46f, 0.035f, 0.46f, felt, 10, 3);
    }

    //============================================================================== the fish
    void drawFish()
    {
        drawFish (fish);
        drawFish (fish2);
    }
    void drawFish (const Fish& fs)
    {
        if (fs.t >= kFishLeap || fs.t < 0.0f)
            return;
        const float t = fs.t / kFishLeap, x = fishX() + fs.off + fs.dir * (t - 0.5f) * 1.5f;
        // High enough that the twist at the top happens clear of the reed tops.
        const float y = (2.5f + 0.9f * fs.power) * cast * std::sin (t * kPi) - 0.15f;
        // Nose up out of the water, a full twist at the top, and flat on its belly at the end.
        const float tilt = (0.75f * (1.0f - smooth01 (t / 0.55f)) - 0.1f * smooth01 ((t - 0.55f) / 0.3f)) * 1.4f;
        const float twist = 6.2832f * smooth01 ((t - 0.3f) / 0.4f);
        // Long and thin on the way up, and flat as it goes in.
        const float stretch = 1.0f + 0.25f * bump (t / 0.3f) - 0.2f * bump ((t - 0.88f) / 0.12f);
        const Xform at = Xform::scale (1.4f * cast * fs.size).then (Xform::rotX (twist)).then (Xform::rotZ (tilt))
                             .then (Xform::rotY (fs.dir > 0.0f ? 0.0f : kPi)).then (Xform::move ({ x, y, kFishZ }));
        const float flick = 0.12f * std::sin (fs.t * 30.0f);
        r.blob (at, 0.34f * stretch, 0.16f / std::sqrt (stretch), 0.1f, rgb (0xf4b6c6), 8, 4, 0.35f);
        r.limb (at.apply ({ -0.26f, 0.0f, 0.0f }), at.apply ({ -0.5f, 0.0f, flick }), 0.02f, 0.17f, rgb (0xe58aa6), 4, 0.35f);
        r.limb (at.apply ({ -0.02f, 0.12f, 0.0f }), at.apply ({ -0.16f, 0.24f, 0.0f }), 0.05f, 0.0f, rgb (0xe58aa6), 3, 0.35f);         // fin
        for (float side : { -1.0f, 1.0f })
            r.blob (Xform::move ({ 0.2f, 0.04f, side * 0.075f }).then (at), 0.035f, 0.035f, 0.02f, rgb (0x3a2e2a), 4, 3);
        r.blob (Xform::move ({ 0.33f, -0.03f, 0.0f }).then (at), 0.03f, 0.04f, 0.04f, rgb (0xc04a6a), 4, 3);                              // a pout
    }

    //============================================================================== the beaver's dam
    void drawDam()
    {
        if (dam <= 0.003f)
            return;
        const Rgb stick = rgb (0x9a7a5a), end = rgb (0xe0c49a), mud = rgb (0xa08868);
        const float half = damHalf (dam) * fg, z0 = streamZ;
        const float settle = 1.0f - 0.6f * damSquash.x;
        // A low mound of mud across the stream, and courses of sticks laid along it, the newer
        // ones higher up. Each stick slides into place as the dam grows to it, and out again as
        // it shrinks.
        r.blob (Xform::move ({ damX, -0.04f, z0 }), half * 1.05f, (0.3f * dam * settle + 0.03f) * fg, 0.55f * fg, mud, 9, 4);
        for (int i = 0; i < kDamSticks; ++i)
        {
            const float s = std::clamp (dam * (float) kDamSticks - (float) i, 0.0f, 1.0f);
            if (s <= 0.0f)
                break;
            Vec3 c;
            float yaw = 0.0f, len = 0.0f;
            stickRest (i, c, yaw, len);
            // (A stick part way in while the dam is neither growing nor shrinking lies in its
            // place: it is never left in the beaver's paws once it has stopped building.)
            if (s < 1.0f && std::abs (damGoal - dam) > 0.02f)
            {
                if (damGoal > dam)
                {
                    // On its way in: held across the beaver's chest in both paws while it wades
                    // over to the stick's place, lowered in its paws, and at the very end turned
                    // and let go into its place. It never leaves the paws until it is down.
                    const float across = beaver.yaw + 0.5f * kPi;
                    const float down = smooth01 ((s - 0.82f) / 0.15f);
                    c = lerp (beaverHoldAt, c, down);
                    yaw = lerp (-across, yaw, down);
                }
                else
                {
                    // Coming apart: it slides off the back of the dam into open water, clear of it,
                    // then floats off along the stream past the beaver, turning slowly.
                    const float u = 1.0f - s, off = smooth01 (u / 0.3f), drift = std::max (0.0f, u - 0.3f) / 0.7f;
                    c = Vec3 { c.x - 0.15f * fg * off - 3.6f * fg * drift,
                               lerp (c.y, 0.03f * fg + 0.02f * fg * std::sin (time * 2.6f + (float) i), off),
                               c.z + 0.75f * fg * off };
                    yaw += 1.8f * u;
                }
            }
            const Vec3 d { std::cos (yaw) * len, 0.03f * fg * std::sin ((float) i * 1.3f), std::sin (yaw) * len };
            r.limb (c - d, c + d, 0.055f * fg, 0.05f * fg, stick, 4);
            r.blob (Xform::rotY (-yaw).then (Xform::move (c + d)), 0.012f * fg, 0.05f * fg, 0.05f * fg, end, 4, 2);
        }
        // A twig with a leaf, stuck in the top once the dam is well along.
        const float top = smooth01 (dam * 3.0f - 2.0f);
        if (top > 0.0f)
        {
            const Vec3 base { damX + 0.1f * fg, (0.3f * settle) * fg, z0 - 0.3f * fg };
            const Vec3 tip = base + Vec3 { 0.08f, 0.38f, -0.02f } * (fg * top);
            r.limb (base, tip, 0.03f * fg, 0.02f * fg, stick, 3);
            r.blob (Xform::rotZ (-0.6f).then (Xform::move (tip + Vec3 { 0.06f, 0.0f, 0.0f } * fg)), 0.09f * fg * top, 0.035f * fg * top, 0.04f * fg * top, rgb (0xb7c468), 6, 3);
        }
    }

    //============================================================================== the beaver
    void drawBeaver()
    {
        const auto& b = beaver;
        const Rgb fur = rgb (0xc49a72), belly = rgb (0xf0d8b8), cream = rgb (0xf5eed8), tailC = rgb (0x7a6070), dark = rgb (0x3a2e2a), tooth = rgb (0xfffdf6);
        const float k = 0.62f * fg;
        // Floating on its back (belly up, at ease), it rolls over in the water and then sits up.
        const float u = b.upright;
        const float roll = kPi * (1.0f - smooth01 (u * 2.0f));
        const float sitUp = smooth01 (u * 2.0f - 1.0f);
        const float lazy = 1.0f - smooth01 (u * 2.0f);                   // 1 = floating on its back
        const float ease = 1.0f - hush;                                   // out of focus it lies still
        const float bobY = 0.03f * std::sin (time * kPi) * ease;         // half a bob a second
        const float floatY = lerp (0.06f + bobY, 0.3f, sitUp);
        // Building: it raises both paws (anticipation) and brings them down on the dam.
        const float patUp = b.patting ? (b.pat < 0.5f ? smooth01 (b.pat / 0.5f) : 1.0f - smooth01 ((b.pat - 0.5f) / 0.14f)) : 0.0f;
        const float lean = sitUp * (1.15f - 0.25f * b.proud + 0.18f * patUp - 0.3f * (b.patting ? smooth01 ((b.pat - 0.5f) / 0.12f) * (1.0f - smooth01 ((b.pat - 0.75f) / 0.25f)) : 0.0f));
        // A wave: it turns to us, and the whole arm goes up over its head and swings to and fro.
        const float waving = b.wave < 1.0f ? envelope (b.wave, 0.18f, 0.2f) : 0.0f;
        const Xform base = place ({ b.x, floatY * k, streamZ - 0.05f * fg }, lerp (b.yaw, 0.5f * kPi - 0.2f, waving), k);
        const Xform body = Xform::rotX (roll).then (Xform::move ({ 0.25f, 0.0f, 0.0f }))
                               .then (Xform::rotZ (lean)).then (Xform::move ({ -0.25f, 0.0f, 0.0f })).then (base);

        // Setting a stick down: the paws come forward and down with it.
        int stickIndex = 0;
        const float lower = b.carry * smooth01 ((placingStick (stickIndex) - 0.45f) / 0.5f);
        const Vec3 holdLocal = lerp (Vec3 { 0.44f, 0.2f, 0.0f }, Vec3 { 0.62f, -0.34f, 0.0f }, lower);
        beaverHoldAt = body.apply (holdLocal);
        addHot (Hot::beaver, 0, { b.x, (floatY + 0.1f + 0.25f * sitUp) * k, streamZ - 0.05f * fg }, 0.55f * k, (0.35f + 0.25f * sitUp) * k, 12.0f);
        r.blob (body, 0.34f, 0.24f, 0.25f, fur, 9, 5);
        // The tummy: big and cream, turned up to the sun while it floats, with a pebble on it.
        r.blob (Xform::move ({ 0.06f, -0.08f - 0.04f * lazy, 0.0f }).then (body), 0.26f + 0.05f * lazy, 0.17f + 0.03f * lazy, 0.2f + 0.03f * lazy, belly.mix (cream, lazy), 8, 4);
        if (lazy > 0.6f)
            r.blob (Xform::move ({ 0.0f, -0.33f, 0.03f }).then (body), 0.05f, 0.03f, 0.045f, rgb (0xb8b0a8), 5, 3);

        // Head: kept the right way up while it floats (the body rolls under it), chin raised.
        const float headTurn = 0.3f * std::sin (time * 0.4f) * (1.0f - sitUp) * ease;
        const Xform head = Xform::rotY (headTurn).then (Xform::rotZ (-lean * 0.75f + 0.25f * b.proud + 0.55f * lazy))
                               .then (Xform::rotX (-roll)).then (Xform::move ({ 0.36f, 0.1f - 0.25f * lazy, 0.0f })).then (body);
        r.blob (head, 0.2f, 0.17f, 0.18f, fur, 8, 5);
        r.blob (Xform::move ({ 0.12f, -0.04f, 0.0f }).then (head), 0.12f, 0.09f, 0.12f, belly, 7, 4);                // muzzle
        r.blob (Xform::move ({ 0.23f, 0.0f, 0.0f }).then (head), 0.045f, 0.035f, 0.05f, dark, 5, 3);              // nose
        for (float side : { -1.0f, 1.0f })
            r.box (Xform::move ({ 0.19f, -0.1f, side * 0.022f }).then (head), 0.012f, 0.04f, 0.02f, tooth);
        const bool happy = b.proud > 0.5f || waving > 0.5f;
        const float startled = b.take < 1.0f ? envelope (b.take, 0.12f, 0.3f) : 0.0f;
        const float brow = b.brow < 1.0f ? envelope (b.brow, 0.2f, 0.2f) : 0.0f;
        for (float side : { -1.0f, 1.0f })
        {
            if (lazy > 0.5f)
                happyEye (head, { 0.13f, 0.08f, side * 0.1f }, 0.035f, 0.018f, 0.011f, dark);     // eyes shut, smiling
            else
            {
                const float open = happy ? 0.25f : (1.0f - b.blink.closed()) * (1.0f - 0.45f * b.sulk) * (1.0f + 0.7f * startled);
                r.blob (Xform::move ({ 0.12f, 0.07f, side * 0.1f }).then (head), 0.03f, std::max (0.007f, 0.04f * open), 0.03f, dark, 5, 3);
            }
            const float flick = side > 0.0f ? 0.5f * bump (b.ear) : 0.0f;
            r.blob (Xform::rotX (side * flick).then (Xform::move ({ -0.04f, 0.15f + 0.05f * startled, side * 0.13f })).then (head), 0.05f, 0.05f * (1.0f + 0.5f * startled), 0.03f, fur * 0.85f, 6, 3);
        }

        // The flat tail: slapped down on the water when the dam is coming apart; floating, held
        // up out of the water as a paddle and sculled slowly.
        auto slapCurve = [] (float t) { return t < 0.5f ? 1.3f * smooth01 (t / 0.5f) : t < 0.62f ? 1.3f - 1.6f * smooth01 ((t - 0.5f) / 0.12f) : -0.3f * (1.0f - smooth01 ((t - 0.62f) / 0.38f)); };
        const float slapA = (b.sulk > 0.3f ? slapCurve (b.slap) * b.sulk : 0.0f) + (b.flop < 1.0f ? -0.8f * slapCurve (b.flop) : 0.0f);
        const float tailSway = (0.15f * std::sin (time * 1.1f) * (1.0f - lazy) + 0.4f * lazy * std::sin (time * 0.5f * kPi)) * ease;
        const Xform tail = Xform::move ({ -0.26f, 0.0f, 0.0f }).then (Xform::rotZ (slapA - lean * 0.9f + 0.55f * lazy)).then (Xform::rotY (tailSway))
                               .then (Xform::move ({ -0.26f, -0.08f, 0.0f })).then (body);
        r.blob (tail, 0.24f, 0.045f, 0.14f, tailC, 8, 3);

        // Paws: folded on its chest floating; patting the dam; on its hips, proud; folded, sulking.
        for (float side : { -1.0f, 1.0f })
        {
            const Vec3 shoulder { 0.22f, 0.04f, side * 0.17f };
            Vec3 paw = lerp (Vec3 { 0.3f, -0.12f, side * 0.12f }, Vec3 { 0.2f, -0.24f, side * 0.06f }, lazy);
            paw = lerp (paw, Vec3 { 0.48f + 0.08f * patUp, 0.0f + 0.32f * patUp, side * 0.12f }, b.patting ? 1.0f : 0.0f);
            paw = lerp (paw, Vec3 { 0.05f, -0.04f, side * 0.26f }, b.proud);
            paw = lerp (paw, Vec3 { 0.32f, 0.03f, side * 0.02f }, b.sulk);
            paw = lerp (paw, holdLocal + Vec3 { -0.04f, 0.0f, side * 0.13f }, b.carry);     // a stick held across its chest, then set down
            if (side > 0.0f)                                                                // the arm on our side, up over its head
            {
                paw = lerp (paw, Vec3 { 0.5f, 0.2f, 0.1f * std::sin (b.brow * 9.0f) }, brow);      // across its brow
                const float swing = std::sin (b.wave * 2.0f * kPi * 2.5f);                  // two and a half swings
                paw = lerp (paw, Vec3 { 0.84f - 0.06f * std::abs (swing), 0.1f, side * (0.2f + 0.2f * swing) }, waving);
            }
            r.limb (body.apply (shoulder), body.apply (paw), 0.05f, 0.045f, fur, 5);
            r.blob (Xform::move (paw).then (body), 0.05f, 0.045f, 0.05f, fur * 0.8f, 5, 3);
        }
    }

    //============================================================================== the turtles
    /** Where turtle `i` sits on the log. */
    Vec3 turtleAt (int i) const noexcept
    {
        const float k = fg, logY = 0.06f * k - logSink, rad = 0.2f * k;
        const Vec3 a { logX - kLogHalf * k, logY, streamZ - 0.05f * k }, b { logX + kLogHalf * k, logY + 0.02f * k, streamZ + 0.1f * k };
        return lerp (a, b, kTurtleAlong[i]) + Vec3 { 0.0f, rad * 0.9f + (kTurtleAlong[i] - 0.5f) * 2.0f * kLogHalf * k * logRock.x, 0.0f };
    }
    static constexpr float kTurtleAlong[3] = { 0.13f, 0.48f, 0.83f };

    void drawLogAndTurtles()
    {
        const Rgb bark = rgb (0x9a7a5a), endC = rgb (0xe0c49a);
        const float k = fg;
        const float logY = 0.06f * k - logSink, rad = 0.2f * k;          // riding low enough to keep clear of the geese (see setSize)
        const float tipY = kLogHalf * k * logRock.x;                   // rocked by a big hit
        const Vec3 a { logX - kLogHalf * k, logY - tipY, streamZ - 0.05f * k }, b { logX + kLogHalf * k, logY + 0.02f * k + tipY, streamZ + 0.1f * k };
        r.limb (a, b, rad, rad * 0.92f, bark, 8);
        r.blob (Xform::rotY (1.5708f).then (Xform::rotZ (1.5708f)).then (Xform::move (a)), rad * 0.95f, 0.02f * k, rad * 0.95f, endC, 8, 2);
        r.blob (Xform::rotY (1.5708f).then (Xform::rotZ (1.5708f)).then (Xform::move (b)), rad * 0.87f, 0.02f * k, rad * 0.87f, endC, 8, 2);

        static constexpr float sizes[3] = { 0.95f, 1.12f, 0.82f }, yaws[3] = { 3.1416f - 0.45f, 0.4f, 0.75f };
        for (int i = 0; i < 3; ++i)
        {
            const auto& t = turtles[(size_t) i];
            Vec3 at = turtleAt (i);
            const float s = 0.62f * 1.1f * k * sizes[i];
            addHot (Hot::turtle, i, at + Vec3 { 0.0f, 0.18f * s, 0.0f }, 0.42f * s, 0.34f * s, 11.0f);
            const float breathe = 1.0f + 0.03f * asleep * std::sin (time * 1.2f + (float) i);

            // Clicked: a start, then over backwards off the far side of the log and into the stream;
            // under for a moment; then its head comes up behind the log and it hauls itself back on.
            float tipBack = 0.0f;
            if (t.fall < 1.0f)
            {
                const float sec = t.fall * kFallSeconds;
                if (sec < 0.3f)
                    tipBack = 0.1f * std::sin (sec * 42.0f);
                else if (sec < 0.75f)
                {
                    const float u = (sec - 0.3f) / 0.45f;
                    tipBack = 2.7f * u * u;
                    at = at + Vec3 { 0.0f, 0.14f * k * std::sin (u * kPi) - 0.5f * k * u * u, 0.55f * k * u };
                }
                else if (sec < 1.6f)
                    continue;                                            // under
                else if (sec < 2.7f)
                {
                    const float v = (sec - 1.6f) / 1.1f, up = smooth01 (std::min (1.0f, v * 1.35f));
                    tipBack = -0.5f * (1.0f - smooth01 (v));
                    at = at + Vec3 { 0.0f, -0.42f * k * (1.0f - up) + 0.07f * k * bump (v), 0.44f * k * (1.0f - smooth01 (v)) };
                }
            }
            const Xform base = Xform::rotX (tipBack).then (place (at, yaws[i] + 0.15f * t.wobble.x, s));
            // Shut, the shell sits down on the log (its legs are in), squashes as it lands, and rocks.
            const Xform shellX = squash (t.drop.x).then (Xform::rotZ (0.4f * t.wobble.x)).then (Xform::rotX (0.2f * t.wobble.x))
                                     .then (Xform::move ({ 0.0f, -0.05f * (1.0f - smooth01 (t.out)), 0.0f })).then (base);
            drawTurtle (base, shellX, t, breathe, i == 1);
        }
    }

    void drawTurtle (const Xform& base, const Xform& shellX, const Turtle& t, float breathe, bool snores)
    {
        // A dusty teal shell with cream outlines to its plates and an ochre rim; soft peach skin
        // with pink cheeks. Nothing like the lawn or the frog.
        const Rgb shell = rgb (0x6f9e93), plate = rgb (0x5f8c82), outline = rgb (0xf5eed8), rim = rgb (0xe0b860);
        const Rgb skin = rgb (0xf2c4a8), cheek = rgb (0xf4b6c6), dark = rgb (0x3a2e2a), white = rgb (0xfffdf6);
        const float out = t.out, o = smooth01 (out), stretch = 0.12f * bump (t.stretch) * (1.0f - asleep);
        const float startle = ((! t.in && t.antic < 1.0f) || t.fall * kFallSeconds < 0.75f) ? 1.0f : 0.0f;   // the start before it ducks in (or goes over)
        r.blob (Xform::move ({ 0.0f, 0.17f, 0.0f }).then (shellX), 0.32f, 0.2f * breathe, 0.27f, shell, 9, 4);
        r.blob (Xform::move ({ 0.0f, 0.08f, 0.0f }).then (shellX), 0.36f, 0.06f, 0.3f, rim, 9, 2);
        for (int j = 0; j < 3; ++j)
        {
            const float px = -0.13f + 0.13f * (float) j, pz = 0.04f * std::sin ((float) j * 2.0f);
            const float py = 0.17f + 0.2f * breathe * std::sqrt (std::max (0.0f, 1.0f - (px * px) / (0.32f * 0.32f) - (pz * pz) / (0.27f * 0.27f)));
            r.blob (Xform::move ({ px, py - 0.012f, pz }).then (shellX), 0.085f, 0.03f, 0.085f, outline, 6, 2);
            r.blob (Xform::move ({ px, py - 0.004f, pz }).then (shellX), 0.062f, 0.03f, 0.062f, plate, 6, 2);
        }

        // Shut: dark little openings where the head and legs went.
        if (out < 0.35f)
        {
            const float sh = 1.0f - smooth01 (out / 0.35f);
            r.blob (Xform::move ({ 0.315f, 0.12f, 0.0f }).then (shellX), 0.02f, 0.045f * sh, 0.085f * sh, dark, 6, 3);
            for (int j = 0; j < 4; ++j)
            {
                const float fx = j < 2 ? 0.2f : -0.2f, fz = (j & 1) ? 0.235f : -0.235f;
                r.blob (Xform::move ({ fx, 0.085f, fz }).then (shellX), 0.05f * sh, 0.025f * sh, 0.02f, dark, 5, 2);
            }
        }
        // Coming out: first two eyes in the opening, looking about.
        if (t.eyes > 0.02f)
        {
            const float e = t.eyes, shut = t.blink.closed();
            const float glance = 0.025f * std::sin (time * 2.4f + (float) t.peekWait);
            const bool one = t.stage == 1 && t.peekWait > 0.45f;      // one eye first
            for (float side : { -1.0f, 1.0f })
            {
                if (one && side < 0.0f)
                    continue;
                r.blob (Xform::move ({ 0.32f, 0.13f, side * 0.045f + glance }).then (shellX), 0.034f * e, 0.04f * e * (1.0f - 0.8f * shut), 0.036f * e, white, 6, 3);
                r.blob (Xform::move ({ 0.35f, 0.13f, side * 0.045f + glance * 1.4f }).then (shellX), 0.015f * e, 0.022f * e * (1.0f - 0.8f * shut), 0.018f * e, dark, 5, 3);
            }
        }

        // Head on its neck: right in when hiding, a peek, out and looking about, resting on the log asleep.
        const float droop = asleep * 0.11f;
        const Vec3 neck0 { 0.2f, 0.13f, 0.0f };
        const Vec3 headP { 0.16f + 0.32f * o, 0.14f + 0.08f * o - droop * o + stretch + 0.07f * startle, 0.0f };
        const float hs = 0.55f + 0.45f * std::min (1.0f, out * 2.2f);
        if (out > 0.04f)
        {
            r.limb (base.apply (neck0), base.apply (headP), 0.07f, 0.065f, skin, 6);
            const Xform head = Xform::scale (hs).then (Xform::rotY (t.look)).then (Xform::move (headP)).then (base);
            r.blob (head, 0.13f, 0.1f, 0.1f, skin, 8, 5);
            for (float side : { -1.0f, 1.0f })
                r.blob (Xform::move ({ 0.05f, -0.025f, side * 0.083f }).then (head), 0.035f, 0.022f, 0.012f, cheek, 5, 3);
            const float shut = std::max (asleep, t.blink.closed()) * (1.0f - startle);
            const float wide = 1.0f + 0.5f * startle;
            for (float side : { -1.0f, 1.0f })
            {
                if (startle > 0.5f)
                    r.blob (Xform::move ({ 0.065f, 0.045f, side * 0.075f }).then (head), 0.03f, 0.038f, 0.03f, white, 5, 3);
                r.blob (Xform::move ({ 0.075f, 0.045f, side * 0.075f }).then (head), 0.026f * wide * 0.8f, std::max (0.006f, 0.034f * wide * 0.8f * (1.0f - 0.85f * shut)), 0.026f, dark, 5, 3);
            }
            r.limb (head.apply ({ 0.12f, -0.02f, -0.05f }), head.apply ({ 0.12f, -0.025f, 0.05f }), 0.008f, 0.008f, dark, 3);       // a little smile
            if (snores && asleep > 0.5f)
            {
                // Fast asleep: a bubble at its nose that swells and shrinks as it breathes.
                const float bb = (0.02f + 0.07f * (0.5f + 0.5f * std::sin (time * 1.2f))) * smooth01 ((asleep - 0.5f) * 2.0f);
                r.blob (Xform::move ({ 0.15f + bb, 0.0f, 0.0f }).then (head), bb, bb, bb, rgb (0xe4f4ff), 7, 4, 0.7f);
            }
        }
        // Legs and tail: out, or drawn right in.
        if (o > 0.05f)
        {
            for (int j = 0; j < 4; ++j)
            {
                const float fx = j < 2 ? 0.17f : -0.17f, fz = (j & 1) ? 0.2f : -0.2f;
                const float reach = 0.02f + 0.1f * o;
                r.blob (Xform::move ({ fx * (1.0f + reach), 0.05f, fz + (fz > 0.0f ? reach : -reach) }).then (base),
                        0.07f * o, 0.04f * o, 0.06f * o, skin, 5, 3);
            }
            r.limb (base.apply ({ -0.3f, 0.1f, 0.0f }), base.apply ({ -0.3f - 0.07f * o, 0.07f, 0.0f }), 0.025f * o, 0.0f, skin, 3);
        }
    }

    //============================================================================== the frog
    void drawFrog()
    {
        const auto& fr = frog;
        const float k = fg;
        // The pad, with a flower on it.
        const float padY = 0.02f - 0.08f * k * fr.pad.x;
        const Xform pad = Xform::rotZ (0.12f * fr.pad.x).then (Xform::move ({ fr.x, padY, streamZ }));
        r.blob (pad, 0.46f * k, 0.022f * k, 0.38f * k, rgb (0x8fc87a), 10, 2);
        // The pad's notch: a wedge of darker green from the middle to just inside the rim (inside
        // the drawn rim too, which cuts a little inside the true outline), lit like the pad.
        const Rgb notch = r.lit (rgb (0x5c9a5a), pad.turn ({ 0.0f, 1.0f, 0.0f }));
        r.triangle (pad.apply ({ 0.0f, 0.026f * k, 0.0f }), pad.apply ({ 0.41f * k, 0.026f * k, -0.045f * k }), pad.apply ({ 0.41f * k, 0.026f * k, 0.045f * k }),
                    notch, notch, notch);
        for (int p = 0; p < 5; ++p)
        {
            const float a = (float) p * 1.2566f;
            r.blob (Xform::rotY (a).then (Xform::move ({ -0.28f * k + 0.05f * k * std::cos (a), 0.05f * k, 0.15f * k - 0.05f * k * std::sin (a) })).then (pad),
                    0.06f * k, 0.03f * k, 0.03f * k, rgb (0xf4b6c6), 5, 2);
        }
        r.blob (Xform::move ({ -0.28f * k, 0.07f * k, 0.15f * k }).then (pad), 0.03f * k, 0.025f * k, 0.03f * k, rgb (0xf2c14e), 5, 2);

        // Where the frog is.
        Vec3 pos { fr.x + 0.05f * k, padY + 0.03f * k, streamZ };
        float sq = 0.0f, spin = 0.0f, legs = 0.0f;
        const float facing = 1.5708f - 0.55f * fr.look;
        float yaw = facing;
        const float tense = fr.tension;
        switch (fr.state)
        {
            case 1: sq = 0.32f * smooth01 (fr.t / 0.3f); break;                                        // crouch
            case 2:
            {
                const float t = fr.t / 0.7f;
                pos.x = lerp (fr.x, fr.landX, t);
                // Up fast, a beat at the top, and down.
                const float s2 = 2.0f * t - 1.0f, hang = 0.5f + 0.5f * std::copysign (std::pow (std::abs (s2), 1.7f), s2);
                pos.y += (0.62f * k * fr.leapH) * std::sin (hang * kPi) - 0.25f * k * smooth01 ((t - 0.75f) / 0.25f);
                sq = -0.34f * bump (t / 0.45f) + 0.12f * bump ((t - 0.4f) / 0.2f);
                legs = bump (t * 1.4f);
                spin = fr.side * 0.6f * std::sin (t * kPi);
                yaw = fr.side > 0.0f ? 0.2f : kPi - 0.2f;
                break;
            }
            case 3: pos.x = fr.landX; pos.y = -0.5f * k; break;                                         // under
            case 7:                                                                                     // staying in: its eyes above the water
                pos.x = fr.landX;
                pos.y = lerp (-0.5f * k, -0.17f * k, smooth01 (fr.t / 0.4f)) + 0.012f * k * std::sin (fr.t * 2.4f);
                yaw = 1.5708f - 0.4f * fr.side;
                break;
            case 6:                                                                                     // a shake from nose to tail
            {
                const float u = fr.t / (20.0f * kF), fade = 1.0f - u;
                yaw = facing + 0.4f * std::sin (fr.t * 46.0f) * fade;
                spin = 0.12f * std::sin (fr.t * 46.0f + 1.0f) * fade;
                sq = 0.06f * std::sin (fr.t * 23.0f) * fade;
                break;
            }
            case 4:                                                                                     // eyes above the water, swimming back
            {
                const float t = smooth01 (fr.t / 0.8f);
                pos.x = lerp (fr.landX, fr.x - fr.side * 0.3f * k, t);
                pos.y = -0.17f * k + 0.02f * k * std::sin (fr.t * 9.0f);
                yaw = fr.side > 0.0f ? kPi - 0.3f : 0.3f;
                legs = 0.5f + 0.5f * std::sin (fr.t * 12.0f);
                break;
            }
            case 5:
            {
                const float t = fr.t / 0.5f;
                pos.x = lerp (fr.x - fr.side * 0.3f * k, fr.x + 0.05f * k, smooth01 (t));
                pos.y = lerp (-0.17f * k, pos.y, smooth01 (t / 0.6f)) + 0.12f * k * bump (t);
                sq = 0.25f * bump ((t - 0.6f) / 0.4f);
                yaw = lerp (fr.side > 0.0f ? kPi - 0.3f : 0.3f, facing, smooth01 (t));
                break;
            }
            default:
            {
                // Tense: crouched low on the pad, ready to go, and with the ceiling right there,
                // shivering.
                sq = 0.24f * smooth01 (tense / 0.8f) + 0.02f * std::sin (time * 2.1f);
                const float high = smooth01 ((tense - 0.6f) / 0.4f);
                pos.x += 0.035f * k * high * std::sin (fr.quiver);
                // Where its pad cannot follow the goose, it looks up at it instead.
                spin = 0.32f * fr.lookUp * (1.0f - 0.5f * tense);
                break;
            }
        }
        const Xform at = squash (sq).then (Xform::rotZ (spin)).then (Xform::scale (0.55f * k)).then (Xform::rotY (yaw)).then (Xform::move (pos));
        drawFrogBody (at, legs, fr.state == 0 ? tense : 0.0f);
        if (fr.state == 0)
            addHot (Hot::frog, 0, { fr.x, padY + 0.14f * k, streamZ }, 0.4f * k, 0.28f * k, 12.0f);
    }

    void drawFrogBody (const Xform& at, float legs, float tense)
    {
        const auto& fr = frog;
        const Rgb green = rgb (0xa6d67e), light = rgb (0xf4f0c0), dark = rgb (0x3a2e2a), white = rgb (0xfffdf6);
        r.blob (Xform::move ({ 0.0f, 0.2f, 0.0f }).then (at), 0.3f, 0.21f, 0.27f, green, 9, 5);
        r.blob (Xform::move ({ 0.08f, 0.15f, 0.0f }).then (at), 0.22f, 0.13f, 0.2f, light, 8, 4);
        // Throat: puffs out twice, quicker and fuller as the ceiling comes near.
        // Near the ceiling the throat stays half full and the cheeks puff out.
        const float puff = std::max (fr.state == 0 ? std::max (0.0f, std::sin (fr.puff * 4.0f * kPi)) * bump (fr.puff) : 0.0f, 0.55f * smooth01 ((tense - 0.5f) / 0.4f));
        r.blob (Xform::move ({ 0.25f, 0.13f, 0.0f }).then (at), 0.07f + 0.09f * puff * (1.0f + 0.4f * tense), 0.06f + 0.08f * puff * (1.0f + 0.4f * tense), 0.08f + 0.08f * puff, light, 7, 4);
        if (tense > 0.3f)
            for (float side : { -1.0f, 1.0f })
                r.blob (Xform::move ({ 0.17f, 0.22f, side * (0.2f + 0.06f * tense) }).then (at), 0.08f * tense, 0.07f * tense, 0.06f * tense, green, 6, 3);
        // Eyes on top, bulging and wide open when tense: more white, a smaller pupil.
        const float shut = fr.blink.closed() * (1.0f - tense);
        for (float side : { -1.0f, 1.0f })
        {
            const float e = 0.085f * (1.0f + 0.75f * tense), w = 0.75f + 0.12f * tense, p = 0.32f - 0.08f * tense;
            r.blob (Xform::move ({ 0.14f, 0.38f + 0.04f * tense, side * 0.13f }).then (at), e, e, e, green, 7, 4);
            r.blob (Xform::move ({ 0.18f, 0.4f + 0.05f * tense, side * 0.14f }).then (at), e * w, e * (w + 0.05f) * (1.0f - 0.85f * shut), e * w, white, 6, 4);
            r.blob (Xform::move ({ 0.18f + 0.8f * e * w, 0.4f + 0.05f * tense, side * 0.15f }).then (at), e * p, e * (p + 0.08f) * (1.0f - 0.85f * shut), e * p, dark, 5, 3);
        }
        r.limb (at.apply ({ 0.27f, 0.22f, -0.13f }), at.apply ({ 0.29f, 0.21f, 0.13f }), 0.012f, 0.012f, dark, 3);       // mouth
        if (fr.tongue < 1.0f && fr.state == 0)
        {
            // A flick of the tongue at something only it can see.
            const float out = bump (fr.tongue);
            const Vec3 tipAt { 0.3f + 0.75f * out, 0.21f + 0.3f * out, 0.0f };
            r.limb (at.apply ({ 0.28f, 0.21f, 0.0f }), at.apply (tipAt), 0.022f, 0.02f, rgb (0xf2a0b4), 4, 0.3f);
            r.blob (Xform::move (tipAt).then (at), 0.045f, 0.045f, 0.045f, rgb (0xf2a0b4), 5, 3, 0.3f);
        }
        // Legs: folded at its sides, flung out behind in a leap.
        for (float side : { -1.0f, 1.0f })
        {
            const Vec3 thigh { -0.08f - 0.1f * legs, 0.1f, side * 0.2f };
            r.blob (Xform::move (thigh).then (at), 0.15f, 0.08f, 0.09f, green, 7, 3);
            const Vec3 foot { lerp (0.06f, -0.55f, legs), lerp (0.02f, 0.12f, legs), side * lerp (0.25f, 0.18f, legs) };
            r.limb (at.apply (thigh), at.apply (foot), 0.05f, 0.035f, green, 4);
            r.blob (Xform::move (foot).then (at), 0.08f, 0.02f, 0.06f, green, 5, 2);
            r.limb (at.apply ({ 0.16f, 0.12f, side * 0.12f }), at.apply ({ 0.24f + 0.1f * legs, 0.01f, side * 0.15f }), 0.035f, 0.03f, green, 4);
        }
    }

    //============================================================================== the dragonflies
    void drawDragonflies()
    {
        // A dark teal body, long and segmented, with big orange eyes; four long wings, each a pale
        // see-through shape with a darker rim so that it shows against water, sky and rain. In
        // flight the wings stand up in a V with a faint blur of the downstroke under them (a beat
        // this fast cannot be shown frame by frame); sitting, they lie flat out sideways.
        // (The wings are see-through and not lit, so they take the light of the day by hand.)
        const Rgb body = rgb (0x2f7f9a), tail = rgb (0x2a6f88), tip = rgb (0x234a5a), eye = rgb (0xf2946b);
        const Rgb wingC = rgb (0xf6fcff).mix (rgb (0xc8d0d8), 0.4f * weather) * sceneDim, rimC = rgb (0x4a6a80) * sceneDim;
        for (int i = 0; i < 3; ++i)
        {
            const auto& d = flies[(size_t) i];
            if (d.state == 0)
                continue;
            const bool perched = d.perch != 0 && d.hop >= 1.0f;
            const float hover = perched ? 0.0f : 0.06f * std::sin (time * 5.0f + (float) i * 2.0f);
            const Xform at = place (d.pos + Vec3 { 0.0f, hover, 0.0f }, d.yaw, 1.4f * cast);
            addHot (Hot::fly, i, d.pos + Vec3 { 0.0f, 0.1f * cast, 0.0f }, 0.42f * cast, 0.34f * cast, 8.0f);
            r.limb (at.apply ({ -0.46f, 0.0f, 0.0f }), at.apply ({ -0.2f, 0.0f, 0.0f }), 0.022f, 0.032f, tip, 4, 0.2f);
            r.limb (at.apply ({ -0.2f, 0.0f, 0.0f }), at.apply ({ 0.04f, 0.0f, 0.0f }), 0.032f, 0.042f, tail, 5, 0.2f);
            r.blob (Xform::move ({ 0.08f, 0.0f, 0.0f }).then (at), 0.07f, 0.055f, 0.055f, body, 7, 4, 0.2f);
            for (float side : { -1.0f, 1.0f })
                r.blob (Xform::move ({ 0.155f, 0.015f, side * 0.032f }).then (at), 0.04f, 0.042f, 0.04f, eye, 6, 3, 0.3f);
            const float flutter = perched ? 0.0f : 0.1f * std::sin (time * 11.0f + (float) i * 1.7f);
            // Each wing is turned about its own length most of the way towards the camera, so that
            // it never shows edge on (a wing seen edge on is a dark line, and the dragonfly then
            // reads as a loose bar): it keeps its place and its length, and shows its face.
            auto wing = [&] (Vec3 root, Vec3 tipW, float width, Rgb c, float alpha, float grow)
            {
                const Vec3 cen = (root + tipW) * 0.5f;
                const Vec3 cw = at.apply (cen), ax = at.turn ((tipW - root) * (0.5f * grow));
                const Vec3 own = at.turn ({ width * grow, 0.0f, 0.0f });
                const float wide = std::sqrt (dot (own, own));
                Vec3 face = cross (ax, cw - r.eye());
                if (dot (face, face) < 1.0e-8f)
                    face = own;
                face = normalised (face);
                if (dot (face, own) < 0.0f)
                    face = face * -1.0f;
                const Vec3 bx = normalised (own * (0.35f / std::max (1.0e-6f, wide)) + face * 0.65f) * wide;
                Vec3 prev = cw + ax;
                for (int s = 1; s <= 10; ++s)
                {
                    const float a = 6.2831853f * (float) s / 10.0f;
                    const Vec3 p = cw + ax * std::cos (a) + bx * std::sin (a);
                    r.triangle (cw, prev, p, c, c, c, alpha);
                    prev = p;
                }
            };
            for (int w = 0; w < 4; ++w)
            {
                const float side = (w & 1) ? 1.0f : -1.0f, fx = w < 2 ? 0.11f : 0.03f, sweep = w < 2 ? 0.04f : -0.08f;
                const Vec3 root { fx, 0.03f, side * 0.03f };
                const float lift = perched ? 0.02f : 0.24f + flutter;
                const Vec3 tipW { fx + sweep, 0.03f + lift, side * (perched ? 0.36f : 0.32f) };
                const float width = w < 2 ? 0.065f : 0.075f;
                wing (root, tipW, width, rimC, 0.55f, 1.12f);           // the rim
                wing (root, tipW, width, wingC, 0.7f, 1.0f);
                if (! perched)                                           // the blur of the downstroke
                    wing (root, Vec3 { fx + sweep, 0.0f - 0.04f, side * 0.3f }, width, wingC, 0.28f, 1.0f);
            }
        }
    }

    //============================================================================== small things
    void drawBursts()
    {
        for (const auto& b : bursts)
        {
            if (b.t >= 1.0f)
                continue;
            if (b.kind == 2)
            {
                // A puff of dust: three small clouds that swell, rise a little and thin away.
                const float t = b.t, out = 1.0f - (1.0f - t) * (1.0f - t);
                for (int i = 0; i < 3; ++i)
                {
                    const float a = b.seed + (float) i * 2.1f;
                    const float s = 0.17f * b.size * (0.5f + out) * (1.0f - t * t) * (1.0f - 0.2f * (float) i);
                    r.blob (Xform::move ({ b.x - b.dir * (0.1f + 0.22f * (float) i) * b.size * out + 0.08f * std::cos (a) * b.size,
                                           b.y + (0.1f + 0.16f * (float) i * out + 0.1f * out) * b.size, b.z + 0.06f * std::sin (a) }),
                            s, s * 0.8f, s * 0.7f, rgb (0xf3ead2), 6, 3, 0.75f);
                }
                continue;
            }
            if (b.kind == 3)
            {
                // Speed lines behind a lunge: three thin streaks, gone in a moment.
                const Rgb c = rgb (0xfffdf6);
                for (int i = 0; i < 3; ++i)
                {
                    const float y = b.y + (0.34f * (float) (i - 1) + 0.05f * std::sin (b.seed + (float) i)) * b.size;
                    const float x0 = b.x - b.dir * (0.2f + 0.25f * (float) (i & 1)) * b.size, len = (1.1f - 0.25f * (float) (i & 1)) * b.size * (1.0f - 0.5f * b.t);
                    const float x1 = x0 - b.dir * len, th = 0.035f * b.size;
                    r.triangle ({ x0, y + th, b.z }, { x0, y - th, b.z }, { x1, y, b.z }, c, c, c, 0.75f * (1.0f - b.t));
                }
                continue;
            }
            if (b.kind == 4)
            {
                // A hit: a small star of light where it landed.
                const Rgb c = rgb (0xfff3c4);
                const float reach = b.size * (0.35f + 0.75f * b.t), in = b.size * 0.18f * (1.0f + b.t);
                for (int i = 0; i < 7; ++i)
                {
                    const float a = b.seed + (float) i * 0.8976f, w = 0.16f;
                    const Vec3 tipAt { b.x + std::cos (a) * reach, b.y + std::sin (a) * reach, b.z };
                    r.triangle ({ b.x + std::cos (a - w) * in, b.y + std::sin (a - w) * in, b.z }, { b.x + std::cos (a + w) * in, b.y + std::sin (a + w) * in, b.z }, tipAt,
                                c, c, c, 0.95f * (1.0f - b.t * b.t));
                }
                continue;
            }
            if (b.feathers)
            {
                // A puff of feathers: out fast, then drifting down, rocking as they fall.
                for (int i = 0; i < 7; ++i)
                {
                    const float a = b.seed + (float) i * 0.8976f, t = b.t, out = 1.0f - (1.0f - t) * (1.0f - t);
                    const float spread = (0.7f + 0.35f * std::sin (a * 3.0f)) * b.size * out;
                    const float yy = b.y + b.size * ((0.5f + 0.3f * std::cos (a * 2.0f)) * out - 0.85f * t * t);
                    if (yy < 0.02f)
                        continue;
                    const float s = 0.085f * b.size * (1.0f - 0.35f * t);
                    r.blob (Xform::rotZ (0.7f * std::sin (a + t * 9.0f)).then (Xform::rotY (a)).then (Xform::move ({ b.x + std::cos (a) * spread, yy, b.z + std::sin (a) * spread * 0.5f })),
                            s * 1.5f, s * 0.35f, s * 0.7f, rgb (0xfffdf6), 5, 2, 0.5f);
                }
                continue;
            }
            // Drops at least three pixels across, wherever the splash is.
            const auto p0 = r.project ({ b.x, 0.0f, b.z }), p1 = r.project ({ b.x + 1.0f, 0.0f, b.z });
            const float minR = p0.ok && p1.ok ? 1.6f * pp / std::max (1.0f, std::abs (p1.x - p0.x)) : 0.0f;
            for (int i = 0; i < 5; ++i)
            {
                const float a = b.seed + (float) i * 1.2566f;
                const float spread = (0.35f + 0.25f * std::sin (a * 3.0f)) * b.size, t = b.t;
                const float yy = b.size * (1.7f * t - 1.9f * t * t) * (0.7f + 0.3f * std::cos (a * 2.0f));
                if (yy < -0.05f)
                    continue;
                const float s = std::max (minR, 0.08f * b.size * (1.0f - 0.4f * t));
                r.blob (Xform::move ({ b.x + std::cos (a) * spread * t, yy, b.z + std::sin (a) * spread * t * 0.6f }), s, s * 1.3f, s, rgb (0xd2ecf6), 5, 3, 0.0f);
            }
        }
    }

    void drawRings()
    {
        for (const auto& g : rings)
            if (g.age < 1.0f && g.age > 0.0f)
            {
                const float rad = g.size * (0.25f + 1.4f * g.age);
                if (g.splash)
                {
                    // Thick foam with a darker water edge round it, so it shows on pale water.
                    ring (g.x, g.y - 0.004f, g.z, rad * 1.14f, rgb (0x5aa8bc), 0.7f * (1.0f - g.age), true, 0.86f);
                    ring (g.x, g.y, g.z, rad, g.colour, 0.8f * (1.0f - g.age), true, 0.5f);
                }
                else
                    ring (g.x, g.y, g.z, rad, g.colour, (1.0f - g.age) * (g.flat ? 0.85f : 0.42f), g.flat);       // a honk's ring is faint
            }
    }

    /** Rain, once the weather has properly turned: slanted streaks over the whole picture. */
    void drawRain()
    {
        const float amount = smooth01 ((weather - 0.55f) / 0.4f);
        if (amount <= 0.0f)
            return;
        const int count = (int) (260.0f * amount);
        const float fall = 2.6f, slant = 0.28f, len = 11.0f * pp;
        const int wide = std::max (1, (int) std::lround (pp));
        for (int i = 0; i < count; ++i)
        {
            const float fx = (float) i * 0.6180339f, fy = (float) i * 0.7548776f + rainPhase * fall * (0.85f + 0.3f * (float) (i % 5) / 4.0f);
            const float y0 = (fy - std::floor (fy)) * ((float) r.height + 24.0f * pp) - 12.0f * pp;
            const float x0 = (fx - std::floor (fx)) * ((float) r.width + 60.0f * pp) - y0 * slant;
            for (int k = 0; k < wide; ++k)
                r.streak (x0 + (float) k, y0, x0 + (float) k - len * slant, y0 + len, rgb (0xe8f2ff), 0.30f + 0.25f * amount);
        }
    }

    /** A ring of light: upright and facing the camera (a honk), or flat on the water (a splash). */
    void ring (float x, float y, float z, float radius, Rgb colour, float alpha, bool flat, float innerShare = -1.0f)
    {
        const int n = 14;
        const float inner = radius * (innerShare > 0.0f ? innerShare : flat ? 0.7f : 0.82f);
        for (int i = 0; i < n; ++i)
        {
            const float a0 = 6.2831853f * (float) i / (float) n, a1 = 6.2831853f * (float) (i + 1) / (float) n;
            auto pt = [&] (float a, float rad) -> Vec3
            {
                return flat ? Vec3 { x + std::cos (a) * rad, y, z + std::sin (a) * rad * 0.8f }
                            : Vec3 { x + std::cos (a) * rad, y + std::sin (a) * rad, z };
            };
            const Vec3 p0 = pt (a0, inner), p1 = pt (a0, radius), p2 = pt (a1, radius), p3 = pt (a1, inner);
            r.triangle (p0, p1, p2, colour, colour, colour, alpha);
            r.triangle (p0, p2, p3, colour, colour, colour, alpha);
        }
    }

    Raster r;
    WorldInput input;
    std::array<Goose, kMaxGeese> geese {};
    std::array<Ring, 28> rings {};
    std::array<Burst, 14> bursts {};
    std::array<HotSpot, 24> hots {};               // where the mouse can find someone, as last drawn
    int hotCount = 0;
    Hot clicked = Hot::none;                       // a click waiting for the next step
    int clickedIndex = 0;
    std::array<TreeTop, kNumEdges> tops {};
    Rgb moduleCol[kNumModules + 1] { rgb (0xb9a8e0), rgb (0xf2c14e), rgb (0xb7c468), rgb (0xf2946b), rgb (0x8fd0e2) };
    std::array<Turtle, 3> turtles {};
    std::array<Fly, 3> flies {};
    Keeper keeper;
    Fish fish, fish2;                              // the second, smaller one follows when two clicks come close together
    float fishPend = 0.0f, snapWas = 0.0f;         // a leap waiting for its turn; the click reading last step
    Hat hat;
    Beaver beaver;
    Frog frog;
    Spring damSquash;
    Xform keeperHatAt, gooseHatAt;
    Vec3 beaverHoldAt;
    Vec3 micAt { -3.0f, 2.0f, kKeeperZ };
    float time = 0.0f;
    int spawned = 0, peekOrder = 0;
    std::uint32_t seed = 0x2545f491u;
    bool moving = false, placed = false;
    float cast = 1.0f;                 // size of the characters, 1 unless the picture is very wide
    float fg = 1.0f;                   // size of the small creatures in the stream
    float streamZ = -3.6f, streamFar = -3.0f, streamRatio = 0.7f, stageHalf = 5.6f, damX = -4.5f, logX = 4.4f;
    float logSink = 0.0f, logLeftPx = 0.0f, logRightPx = 0.0f;     // see setSize: geese behind the log
    float keeperToStream = 0.8f, keeperLo = -3.0f, keeperHi = 3.0f, sitLo = -2.5f, sitHi = 2.5f;   // see setSize
    float idleWait = 9.0f, idleBusy = 0.0f;        // the shared idle timer, and how long its event runs
    bool stillPrev = false, lively = true;         // the pond was still last frame; small business may start now
    bool fussing = false;
    float sceneDim = 1.0f;                         // how far the lights are down (Solo Honk), for colours drawn unlit                          // idle business is playing: full rate, but it does not wake the rest
    int deepestGoose = -1;                         // the goose of the deepest notch, this step
    int micVisits = 0;                             // how often a dragonfly has sat on the microphone since they came
    float washSlow = 0.0f, dam = 0.0f, damGoal = 0.0f, guardHold = 0.0f, asleep = 0.0f, keyHold = 0.0f;
    float honkiness = 0.0f, weather = 0.0f, windPhase = 0.0f, rainPhase = 0.0f;
    float backdropDue = 0.0f;                      // seconds until the backdrop is drawn afresh
    float backdropDim = -1.0f;
    std::uint32_t backdropSig = 0;
    float hush = 0.0f;                             // 1 = a range is in focus: everyone outside it holds still
    float reedLife = 0.0f, gust = 1.0f;            // how much the reeds sway; a breeze crossing them (0..1, 1 = none)
    float dabble = 1.0f, residentPhase = 0.0f;     // the goose out on the pond: tipping up to feed; where it has paddled to
    int ringGoose = -1, lastActor = -1;            // the goose whose honks show as rings; who had the last piece of small business
    float deepestCut = 0.0f;                       // the deepest cut in any band, this step
    Vec3 rideTip, rideGrip, rideHat, headAt, biteAt;               // where a goose stands on the end of his pole, near his hands, and on his hat
    float bowX = 0.0f, bowDir = 1.0f, bowT = 1.0f; // the reeds bowing away from a stroke or a skid (0..1, 1 = not)
    Spring logRock;                                // the turtles' log, rocked by a big hit
    float offerX = 0.0f;                           // where along the bank the creature he holds the microphone out to is
    bool offerFrog = false;
    bool guardBig = false;
    float axisWas[2] { 0.0f, 1.0f };
    float pp = 1.0f;                               // the picture's pixels per point of the window
    std::array<Cand, kMaxCands> cands {};          // notches that may yet bring a goose
    float spawnCool = 0.0f;                        // seconds before the next goose may arrive
    float silence = 0.0f;                          // seconds with no programme and nothing cut
};

} // namespace dehonk::scene

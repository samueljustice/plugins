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
//                goes, and holds the microphone out over it. Now and then, no more than once every
//                several seconds, he winds up and takes one swing at it with the pole. Bypass: he
//                sits down with a flask. Solo Honk: he cups a headphone and listens. Diff: he stops
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
            case Treat::swing:         keeper.swingCool = 0.0f; break;
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
    static constexpr float kPumpSeconds = 0.75f;     // the recordist's fist pump, start to settle
    static constexpr float kTripSeconds = 1.5f;      // caught, two skips on the front foot, settle
    static constexpr float kSwingSeconds = 1.25f;    // the bat: wind-up, a beat, the swing, back to rest
    static constexpr float kSwingHit = 0.47f;        // how far through it the pole comes down on the goose
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
        Spring squash;
        Blink blink;
    };
    enum class Gesture { none, tap, hat, pump, wipe, sip, stretch, wave, tip };
    struct Keeper
    {
        float x = -2.6f, speed = 0, phase = 0, facing = 1, yaw = 1.35f, poleYaw = -0.3f, shoo = 0, sit = 0, idle = 0;
        float recoil = 0, trip = 1.0f, tripWait = 9.0f, solo = 0, compare = 0, chase = 0, hand = -1.0f;
        float pumpWait = 0.0f, cableX = -3.9f, quiet = 0;
        float swing = 1.0f, swingCool = 4.0f;      // the bat (0..1, 1 = not swinging) and the seconds before the next one
        float runAmt = 0;                          // 0 standing .. 1 at a run, eased
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
    struct Burst { float x = 0, z = 0, t = 1.0f, size = 1.0f, seed = 0; float y = 0; bool feathers = false; };
    struct Fish { float t = 9.0f, power = 0.5f, dir = 1.0f; bool flopped = true; };
    struct HotSpot { float x = 0, y = 0, rx = 1, ry = 1; Hot kind = Hot::none; int index = 0; };
    struct Hat { int state = 0; float t = 0, wait = 140.0f; int goose = -1; };       // 0 on his head, 1 snatched, 2 on the goose, 3 on its way back
    struct Beaver
    {
        float x = 0, yaw = 0.4f, upright = 0, build = 0, sulk = 0, proud = 0, proudT = 0, pat = 0, slap = 0, earWait = 3.0f, ear = 1.0f, patWait = 5.0f;
        float patLen = 0.8f, carry = 0.0f;
        float wave = 1.0f, flop = 1.0f;     // a wave of the paw; a slap of the tail while floating (0..1, 1 = none)
        int pats = 0, patCount = 3;
        bool patting = false, idlePat = false;
        Blink blink;
    };
    struct Turtle
    {
        float out = 1.0f, delay = 0, peekWait = 0, look = 0, lookTo = 0, lookWait = 2.0f, stretch = 1.0f, stretchWait = 9.0f;
        float antic = 1.0f, eyes = 0.0f;   // the start of a hide (0..1, head up, eyes wide); the eyes showing in the opening
        float fall = 1.0f, stretchDelay = -1.0f;   // clicked: toppling off the log and climbing back (0..1, 1 = on the log)
        int stage = 3;                  // 0 right in, 1 eyes in the opening, 2 head peeking, 3 out
        bool in = false;
        Spring wobble, drop;            // the shell's rock, and its squash as it lands on the log
        Blink blink;
    };
    struct Frog
    {
        int state = 0;                  // 0 sitting, 1 crouching, 2 in the air, 3 under, 4 swimming back, 5 climbing on
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
                g.leaving = true;
                // A job well done, if he was at it: a small fist pump.
                if (! in.calm && keeper.shoo > 0.5f && std::abs (keeper.x - g.x) < 3.2f && g.age > 1.5f && g.peak > 3.0f && keeper.pumpWait <= 0.0f
                    && keeper.swing >= 1.0f && keeper.clickWait <= 0.0f)
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
                    if (least != nullptr && ready->depth > least->depth + 2.0f && least->knock >= 1.0f && ! (least->knockX < 0.0f || least->knockX > 0.0f))
                        least->leaving = true;
                }
                else if (spawnCool <= 0.0f && shown < kLiveGeese)
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
                g.squash.v = 2.6f;                                    // lands with a squash
                g.wag = 1.0f;                                         // and a wiggle of the tail
            }
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
            const float wanted = g.arrive < 1.0f || ! active ? 0.0f : std::clamp ((goal - g.x) * 4.0f, -top, top);
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

            // Only the one he is working minds the pole. When he winds up it stretches its neck out
            // at the microphone and hisses; when the pole comes down it flaps up off the ground in a
            // temper, and it stays cross for a second or two after.
            const bool worked = index == keeper.target && active && ! g.thief && ! away;
            g.temper = std::max (0.0f, g.temper - dt);
            const bool poleNear = worked && std::abs (micAt.x - g.x) < 2.0f && keeper.shoo > 0.5f;
            follow (g.hiss, poleNear && g.flap >= 1.0f && (keeper.swing < 0.7f || g.temper > 0.0f) ? 1.0f : 0.0f, dt, 0.18f);
            if (swingHit && worked && g.flap >= 1.0f && g.arrive >= 1.0f)
            {
                g.flap = 0.0f;
                g.temper = 1.8f;
            }
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
            if (g.hiss < 0.5f && g.still < 0.5f && ! away && g.preen >= 1.0f)
                g.honkPhase += dt * (0.55f + 0.1f * g.depth);
            if (active && g.depth > worstDepth) { worstDepth = g.depth; worstX = g.x; deepestGoose = index; }

            // A honk leaves the beak as a ring each time the beak opens: only from the one he is
            // working (or the deepest, when he is not), so that there is one set of rings at a time.
            const float cycle = g.honkPhase - std::floor (g.honkPhase);
            const bool ringing = keeper.target >= 0 ? worked : index == ringGoose;
            if (ringing && g.arrive >= 1.0f && cycle < g.lastCycle && dt > 0.0f && g.hiss < 0.5f && g.still < 0.5f)
                addRing (g.x + std::cos (g.yaw) * 0.9f * cast, (2.0f + 0.07f * g.depth) * cast, g.z - 0.3f, (0.4f + 0.05f * g.depth) * cast, false, 0.0f, 1.6f, rgb (0xe5533b));
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
                b.t += dt / (b.feathers ? 1.3f : 0.55f);
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
                    if (g.used && ! g.leaving && g.arrive >= 1.0f && g.knock >= 1.0f && ! (g.knockX < 0.0f || g.knockX > 0.0f) && ! g.thief)
                    {
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
        const bool big = keeper.swing < 1.0f || keeper.trip < 1.0f || hat.state != 0 || frog.state != 0 || fish.t < kFishLeap + 0.6f
                         || keeper.gesture != Gesture::none || reacting();
        if (dt <= 0.0f || big || (idleWait -= dt) > 0.0f)
            return;
        idleWait = stillPrev ? rnd (30.0f, 60.0f) : anyGoose ? rnd (7.0f, 12.0f) : rnd (6.0f, 11.0f);
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
        const bool kFree = k.gesture == Gesture::none && std::abs (k.speed) < 0.3f && k.trip >= 1.0f && hat.state == 0 && k.swing >= 1.0f
                           && ! in.diff && ! in.soloHonk && k.clickWait <= 0.0f;
        offer (0, kFree && (in.bypass ? k.sit > 0.97f : ((k.quiet > 4.0f || weather > 0.6f) && k.sit < 0.03f)));
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
        switch (lastActor)
        {
            case 0:
            {
                const float pick = rnd();
                if (in.bypass)                              { startGesture (Gesture::sip, 2.4f); idleBusy = 2.4f; }
                else if (weather > 0.6f && pick < 0.5f)     { startGesture (Gesture::wipe, 1.5f); idleBusy = 1.5f; }
                else if (pick < 0.4f)                       { startGesture (Gesture::tap, 2.6f); idleBusy = 2.6f; }
                else if (pick < 0.7f)                       { startGesture (Gesture::hat, 1.6f); idleBusy = 1.6f; }
                else                                        { startGesture (Gesture::stretch, 2.6f); idleBusy = 2.6f; }
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
            if (hat.wait > 0.0f || idleNow || keeper.trip < 1.0f || keeper.gesture != Gesture::none || keeper.swing < 1.0f)
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
                if (alone && (best < 0 || g.depth > geese[(size_t) best].depth))
                    best = i;
            }
            if (best < 0)
                return;
            hat = { 1, 0.0f, rnd (150.0f, 320.0f), best };
            auto& g = geese[(size_t) best];
            g.thief = true;
            g.fleeX = g.x;
            g.flap = 1.0f;
            g.hiss = 0.0f;
            return;
        }

        moving = true;
        auto& g = geese[(size_t) hat.goose];
        hat.t += dt;
        const float away = g.x >= keeper.x ? 1.0f : -1.0f;
        if (hat.state == 1 && hat.t > 0.5f)
        {
            hat.state = 2; hat.t = 0.0f;
            // Off it goes, towards the more open stretch of bank, away from him if it can.
            float bestTo = g.x, bestScore = -1.0e9f;
            for (float dir : { -1.0f, 1.0f })
            {
                const float to = std::clamp (g.x + dir * 4.5f, keeperLo - 1.0f, keeperHi + 1.0f);
                float score = std::abs (to - g.x) + (dir * away > 0.0f ? 0.8f : 0.0f);
                if ((keeper.x - g.x) * dir > 0.0f && (keeper.x - to) * dir < 0.0f)
                    score -= 3.0f;                                   // not past him, where it would be hidden
                for (const auto& o : geese)
                    if (&o != &g && o.used && ! o.leaving && (o.x - g.x) * dir > 0.0f && (o.x - to) * dir < 0.6f)
                        score -= 1.5f;
                if (score > bestScore) { bestScore = score; bestTo = to; }
            }
            g.fleeX = bestTo;
            g.wag = 1.0f;
        }
        else if (hat.state == 2 && (hat.t > 3.2f || (hat.t > 0.9f && std::abs (g.x - keeper.x) < 1.5f) || idleNow || ! g.used || g.leaving))
        {
            hat.state = 3; hat.t = 0.0f;
            g.thief = false;
            g.squash.v = 2.0f;
            g.flap = 0.0f;                                   // put out at losing it
        }
        else if (hat.state == 3 && hat.t > 0.6f)
        {
            hat.state = 0; hat.t = 0.0f;
            g.thief = false;
            keeper.adjustNext = true;
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
        const bool chasing = hat.state == 2;
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
            if (! anyGoose || ! ok (k.target))
            {
                k.target = anyGoose && ok (deepestGoose) ? deepestGoose : -1;
                k.switchT = 0.0f;
            }
            else if (ok (deepestGoose) && deepestGoose != k.target && geese[(size_t) deepestGoose].depth > geese[(size_t) k.target].depth + 2.0f)
            {
                if ((k.switchT += dt) > 2.0f && k.swing >= 1.0f)
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
            if (k.walking && k.swing >= 1.0f)
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

        // Now and then, at a run, his cable catches his foot.
        if (std::abs (k.speed) > 2.6f && k.trip >= 1.0f && ! chasing && k.sit < 0.05f)
            if ((k.tripWait -= dt) <= 0.0f)
            {
                k.trip = 0.0f;
                k.tripWait = rnd (14.0f, 40.0f);                       // seconds spent running, not seconds
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
        const bool going = std::abs (gap) >= (k.going ? 0.15f : 0.5f) && ! shuffle && ! resting && ! frozen && ! in.calm;
        k.going = going;
        if (going)
            k.facing = gap > 0.0f ? 1.0f : -1.0f;
        else if (std::abs (k.speed) > 0.3f && ! shuffle)
            k.facing = k.speed > 0.0f ? 1.0f : -1.0f;
        else if (work)
            k.facing = tx >= k.x ? 1.0f : -1.0f;
        // Until his body is within sixty degrees of the way he is going, he stays where he is.
        const float along = std::cos (k.yaw) * (gap > 0.0f ? 1.0f : -1.0f);
        const float turned = going ? smooth01 ((along - 0.5f) / 0.25f) : 1.0f;
        const float pace = shuffle ? 0.8f : top;
        const float speedWanted = std::clamp (gap * 3.0f, -pace, pace) * held * turned;
        k.speed += (speedWanted - k.speed) * std::min (1.0f, dt * (speedWanted * k.speed < 0.0f || turned < 1.0f ? 12.0f : 5.0f));
        // Bypass and Diff stop him where he is, once he has stepped clear of the creatures.
        if (((resting || frozen) && (inPlace || k.sit > 0.05f)) || in.calm)
            k.speed = 0.0f;
        k.x = std::clamp (k.x + k.speed * dt, keeperLo, keeperHi);
        // One turn of the phase is two steps, and the feet cover the ground he does.
        k.phase += std::abs (k.speed) * dt * 2.74f / cast;
        follow (k.runAmt, std::clamp (std::abs (k.speed) / 2.4f, 0.0f, 1.0f), dt, 0.12f);

        // Side on to us when he goes anywhere; nearly side on, turned to the goose, while he works
        // it (his head looks at it); three-quarters round towards us when there is nothing to do,
        // and squarely only when he compares, listens or greets us. A turn to set off or to a goose
        // is quick; the others take their time, and go round by way of facing us.
        float turn = work ? 0.3f : 0.78f;
        turn = lerp (turn, 0.3f, going ? 1.0f : k.runAmt);
        turn = lerp (turn, 1.5f, std::max ({ k.compare, 0.6f * k.solo, 0.8f * k.greet }));
        turn = lerp (turn, 0.75f, k.sit);                    // sitting: three-quarters on, so his legs show
        follow (k.yaw, k.facing > 0.0f ? turn : kPi - turn, dt, going || std::abs (k.speed) > 0.6f || work ? 0.12f : 0.26f);
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

        // The bat. One clear wind-up and one swing, for the goose he is working and no other, and
        // then nothing of the kind for several seconds.
        swingHit = false;
        k.swingCool = std::max (0.0f, k.swingCool - dt);
        if (k.swing < 1.0f)
        {
            const float before = k.swing;
            k.swing = std::min (1.0f, k.swing + dt / kSwingSeconds);
            swingHit = before < kSwingHit && k.swing >= kSwingHit;
            moving = true;
        }
        else if (has && close && k.shoo > 0.9f && std::abs (k.speed) < 0.2f && k.gesture == Gesture::none && k.swingCool <= 0.0f
                 && idleBusy <= 0.0f && dt > 0.0f && k.clickWait <= 0.0f)
        {
            const auto& g = geese[(size_t) k.target];
            if (g.landed > 1.2f && g.depth >= 2.5f && g.knock >= 1.0f && ! (g.knockX < 0.0f || g.knockX > 0.0f) && g.flap >= 1.0f && ! g.thief && g.preen >= 1.0f)
            {
                k.swing = 0.0f;
                k.swingCool = rnd (5.5f, 9.5f);
            }
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
            const bool fits = k.gesture == Gesture::pump || (standing && (greeting || (k.gesture == Gesture::sip) == (k.sit > 0.9f)));
            if (! fits)
                k.gt = std::max (k.gt, 0.85f);                         // cut short: straight to its ending
            k.gt += dt / k.glen;
            if (k.gt >= 1.0f)
                k.gesture = Gesture::none;
            (k.idleGesture ? fussing : moving) = true;
        }
        else if (standing && dt > 0.0f && k.swing >= 1.0f)
        {
            if (k.adjustNext)
            {
                k.adjustNext = false;
                startGesture (Gesture::hat, 1.6f);
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

        micAt = keeperRig().mic;
    }

    //------------------------------------------------------------------------------ the fish
    void stepFish (const WorldInput& in, float dt)
    {
        // One leap for a click, and no more than one every few seconds however many clicks there
        // are. With another range in focus the fish stays down.
        if (fish.t >= kFishRest && in.snapOn && in.frame.snapCutDb > 2.0f && dt > 0.0f && (in.focus < 0 || in.focus == kNumModules))
        {
            fish = { 0.0f, std::clamp (in.frame.snapCutDb / 12.0f, 0.3f, 1.0f), (spawned++ % 2) ? 1.0f : -1.0f, false };
            splash (fishX() - fish.dir * 0.75f, kFishZ, 0.55f, false);
        }
        if (fish.t < kFishRest)
            fish.t += dt;
        if (fish.t < kFishLeap + 0.6f)
        {
            if (! fish.flopped && fish.t >= kFishLeap)
            {
                fish.flopped = true;
                splash (fishX() + fish.dir * 0.75f, kFishZ, 1.6f + 0.6f * fish.power);       // the belly-flop
            }
            moving = true;
        }
    }
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
        if (b.sulk > 0.6f)
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
        const float wade = placing ? 1.8f : 0.9f;
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
                        t.drop.x = 0.2f; t.drop.v = 0.0f;
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
                    if (t.stage == 0)      { t.stage = 1; t.peekWait = 0.6f; }
                    else if (t.stage == 1) { t.stage = 2; t.peekWait = rnd (0.7f, 1.1f); }
                    else                   { t.stage = 3; t.in = false; ++peekOrder; }
                }
            }
            const float to = t.in ? (t.stage == 2 ? 0.42f : 0.0f) : 1.0f;
            follow (t.out, to, dt, to < t.out ? 0.035f : 0.22f);
            follow (t.eyes, t.in && t.stage == 1 ? 1.0f : 0.0f, dt, 0.08f);
            t.wobble.tick (0.0f, dt, 14.0f, 0.25f);                   // about 2 Hz, dying away over half a second
            t.drop.tick (0.0f, dt, 17.0f, 0.4f);

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
        follow (fr.tension, atCeiling && ! fr.armed ? 1.0f : near, dt, 0.5f);
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
                if (atCeiling && fr.armed && fr.cool <= 0.0f && dt > 0.0f)
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
            case 3: if (fr.t >= 0.5f)  { fr.state = 4; fr.t = 0.0f; addRing (fr.landX, 0.03f, streamZ, 0.3f * fg, true, 0.0f, 1.6f, rgb (0xf4fbff)); } break;
            case 4: if (fr.t >= 0.8f)  { fr.state = 5; fr.t = 0.0f; } break;
            case 5:
                if (fr.t >= 0.5f)
                {
                    fr.state = 0; fr.t = 0.0f; fr.pad.v = 1.8f;
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
        const bool micFree = std::abs (keeper.speed) < 0.3f && keeper.gesture == Gesture::none && keeper.trip >= 1.0f
                             && hat.state == 0 && keeper.sit < 0.1f && keeper.recoil < 0.3f;
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
            else if (i == 0 && d.perch == 0 && micFree && (micVisits == 0 || rnd() < 0.3f))
            {
                // The first one lands on the microphone soon after they come (a second or two),
                // and now and then again after that.
                d.perch = 1;
                d.to = micPerch();
                d.wait = rnd (2.5f, 5.0f);
                ++micVisits;
            }
            else
            {
                d.perch = 0;
                const float centre = 0.6f * keeper.x;
                // Short darts over the water, just above the reed tops and below the trees.
                d.to = { std::clamp (d.pos.x + rnd (-2.6f, 2.6f) + 0.15f * (centre - d.pos.x), -kHalfWidth + 0.8f, kHalfWidth - 0.8f), rnd (1.2f, 2.2f) * std::max (cast, 0.75f) + 0.2f, rnd (0.5f, 2.4f) };
                d.wait = rnd (0.35f, 1.5f);
            }
            const Vec3 way = d.to - d.from;
            d.hop = 0.0f;
            d.hopLen = 0.16f + 0.07f * std::sqrt (dot (way, way));
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
        float lean = 0.30f + 0.55f * p.honk + 0.28f * p.walkAmt * (0.5f + 0.5f * std::sin (p.walk * 2.0f));
        float length = 0.46f + 0.30f * p.honk;
        lean = lerp (lean, -0.12f, p.reach); length = lerp (length, 0.95f, p.reach);
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
        const float open = 0.02f + 0.17f * p.honk + p.hiss * (0.09f + 0.025f * std::sin (time * 9.0f));
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
        const float size = (1.0f + 0.07f * std::min (8.0f, g.depth)) * cast * (1.0f - 0.16f * g.crowd);
        GoosePose pose;
        pose.shade = inBackRow (g) ? 1.0f - 0.55f * g.crowd : 1.0f;
        pose.blink = g.blink.closed();
        Vec3 pos { g.x + g.knockX, 0.0f, g.z };
        float yaw = g.yaw, sq = g.squash.x, tumble = 0.0f;
        const float live = 1.0f - g.still;                           // out of focus it stands still, beak shut

        if (g.leaving)
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
            // Hops up out of the water.
            const float t = g.arrive;
            pos.z = g.z + (1.0f - t) * 1.6f;
            pos.y = 0.9f * std::sin (t * kPi);
            pose.flap = 0.4f + 0.6f * std::sin (t * kPi);
            sq = -0.12f * std::sin (t * kPi);                 // stretched in the air
        }
        else
        {
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
            // Snatching the hat: a stretch up at his head.
            if (g.thief && hat.state == 1)
                pose.reach = bump (hat.t / 0.5f);
            if (g.thief && hat.state == 2)
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
        drawGoose (squash (sq).then (roll).then (Xform::scale (size)).then (Xform::rotY (yaw)).then (Xform::move (pos)), pose, glow,
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
        float hipY = 0.62f, swing[2] {}, knee[2] {};
        Vec3 hand[2], elbow[2], grip, dir;
        float ang = 0, pyaw = 0, len = 2.1f;     // the pole's angle up, its heading against his, its length to the microphone
        float eyes = 1.0f, mouth = 0.0f, smile = 0.0f, happy = 0.0f, eyeShift = 0.0f, hatLift = 0.0f, hatTwist = 0.0f;
        float flask = 0.0f;            // how far the flask is out, in hand[flaskHand]
        int flaskHand = 0;
        float loop = 0.0f;             // the cable caught round his back boot, 0..1
        Vec3 drop { 0, -100, 0 };     // a drop of sweat flicked off, in upper coordinates
        Vec3 mic;                      // world
    };

    KeeperRig keeperRig() const
    {
        const auto& k = keeper;
        KeeperRig g;
        const float sc = 1.5f * cast;                 // his size
        const float run = k.runAmt * (1.0f - k.sit);

        // The bat: the pole drawn up and back as he leans away, a beat, one swing down at the goose
        // with his weight behind it, and back to rest.
        float batAng = 0.0f, batLean = 0.0f;
        if (k.swing < 1.0f)
        {
            const float up = smooth01 (k.swing / 0.34f), down = smooth01 ((k.swing - (kSwingHit - 0.08f)) / 0.08f), back = smooth01 ((k.swing - 0.62f) / 0.38f);
            batAng = 0.7f * up - 1.15f * down + 0.45f * back;
            batLean = -0.2f * up + 0.44f * down - 0.24f * back;
        }

        // The trip: the cable round his back boot, two skips on the front foot with his arms out
        // and the pole see-sawing, then he finds his feet with a small lean back.
        float tripPitch = 0.0f, tripHop = 0.0f, flail = 0.0f, caught = 0.0f, tripSquash = 0.0f;
        const float ts = k.trip * kTripSeconds;
        if (k.trip < 1.0f)
        {
            g.loop = 1.0f - smooth01 ((ts - 0.16f) / 0.12f);
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
        Vec3 target, elbowAt;
        float hipY = 0.62f - 0.4f * k.sit;
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
        g.hipY = hipY + bob + pumpHop;
        hipY = g.hipY;
        const Vec3 hc { 0.04f, hipY + 1.08f, 0.0f };          // head centre

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
                gw = envelope (gt, 0.15f, 0.2f);
                target = { hc.x + 0.04f, hc.y + 0.55f, hs * 0.22f };
                const float u = (gt - 0.2f) / 0.55f;
                g.hatLift = 0.13f * bump (u);
                g.hatTwist = 0.3f * std::sin (u * 2.0f * kPi) * bump (u);
                g.eyes = 1.0f - 0.5f * bump (u);
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
                target = { hc.x + 0.04f, hc.y + 0.55f + g.hatLift, hs * 0.22f };
                nod = 0.24f * bump (u);
                g.smile = gw;
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
        const float towards = k.yaw - (k.facing > 0.0f ? 0.0f : kPi);
        auto leanAlong = [towards] (float a) { return Xform::rotY (towards).then (Xform::rotZ (-a)).then (Xform::rotY (-towards)); };
        g.body = squash (pumpSquash + tripSquash).then (leanAlong (0.22f * run)).then (Xform::scale (sc)).then (Xform::rotY (k.yaw)).then (Xform::move ({ k.x, 0.0f, kKeeperZ }));
        const float torso = tripPitch - 0.36f * k.recoil + batLean;
        const float torsoYaw = 0.22f * std::clamp (k.torsoLook / 0.6f, -1.0f, 1.0f);
        g.upper = Xform::move ({ 0.0f, -hipY, 0.0f }).then (leanAlong (torso)).then (Xform::rotZ (-0.1f * k.solo)).then (Xform::rotY (torsoYaw))
                      .then (Xform::move ({ 0.0f, hipY, 0.0f })).then (g.body);
        const float tilt = 0.22f * k.solo * hs, headYaw = k.look - torsoYaw;
        nod += 0.1f * k.solo - 0.12f * k.recoil;
        g.head = Xform::move ({ -hc.x, -hc.y, 0.0f }).then (Xform::rotZ (-nod)).then (Xform::rotX (tilt)).then (Xform::rotY (headYaw)).then (Xform::move (hc)).then (g.upper);
        g.eyeShift = -0.09f * std::clamp ((k.eyeLook - k.look) / 0.4f, -1.0f, 1.0f);

        // Legs. Caught, the back foot is held behind him while he skips on the front one.
        for (int s = 0; s < 2; ++s)
        {
            const float ph = k.phase + (s == 1 ? 0.0f : kPi);
            g.swing[s] = (1.0f - k.sit) * (0.75f * run * std::sin (ph) * (1.0f - caught) + (s == 1 ? -0.9f : 0.3f) * caught) + k.sit * 1.5f;
            // The knee bends as the leg comes forward, and a little as the foot takes his weight.
            g.knee[s] = (1.0f - k.sit) * (1.0f - caught) * run * (0.18f + 1.05f * std::max (0.0f, std::cos (ph)));
        }

        // The pole: out to the side he is facing, pointed at the goose. Seated, it lies on the
        // grass beside him.
        // Held out over the goose he is working, with the slow drift of a pole held at arm's length;
        // carried upright at a run.
        float ang = 0.45f + k.shoo * (batAng - 0.25f + 0.02f * std::sin (k.idle * 1.1f)) + 0.2f * k.recoil - 0.18f * gw + 0.45f * flail * std::sin (ts * 2.0f * kPi * 2.6f);
        ang = lerp (ang, 1.0f, run * (1.0f - flail));
        ang = lerp (ang, -0.19f, k.sit);
        ang = lerp (ang, 0.25f, k.solo * 0.6f);
        g.pyaw = k.poleYaw - k.yaw;
        g.grip = { 0.38f + 0.28f * k.recoil - 0.14f * run, hipY + 0.44f - 0.12f * k.sit, 0.0f };
        auto aim = [&] (float a)
        {
            g.ang = a;
            g.dir = { std::cos (a) * std::cos (g.pyaw), std::sin (a), -std::cos (a) * std::sin (g.pyaw) };
        };
        g.len = 2.1f + k.extend * (1.0f - k.sit);
        aim (ang);
        // Standing, the microphone stays inside the picture: above the water's edge (never down
        // among the creatures in the stream), clear of the top edge, and clear of the sides (the
        // pole swings round away from us, behind the geese, rather than out of the picture).
        if (k.sit < 0.5f)
            for (int pass = 0; pass < 4; ++pass)
            {
                const Vec3 m = g.upper.apply (g.grip + g.dir * g.len);
                const float rad = 0.17f * sc;
                const float lowY = r.heightAtRow (kBankRow * (float) r.height, m.z) + rad;
                const float topY = r.heightAtRow (0.04f * (float) r.height, m.z) - rad;
                const float need = m.y < lowY ? lowY - m.y : m.y > topY ? topY - m.y : 0.0f;
                if (std::abs (need) >= 1.0e-3f)
                    aim (std::asin (std::clamp (std::sin (g.ang) + need * (1.0f - 2.0f * k.sit) / (g.len * sc), -0.97f, 0.97f)));
                const Vec3 m2 = g.upper.apply (g.grip + g.dir * g.len);
                const auto p0 = r.project (m2), p1 = r.project (m2 + Vec3 { 1.0f, 0.0f, 0.0f });
                if (! p0.ok || ! p1.ok)
                    break;
                const float perUnit = std::max (1.0f, p1.x - p0.x), margin = 0.62f * sc * perUnit + 4.0f;
                const float over = p0.x < margin ? (margin - p0.x) / perUnit : p0.x > (float) r.width - margin ? ((float) r.width - margin - p0.x) / perUnit : 0.0f;
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
        const int front = -std::sin (g.pyaw) > 0.0f ? 1 : 0;
        g.hand[front] = g.grip + g.dir * (0.22f + 0.22f * k.recoil);
        g.hand[1 - front] = g.grip - g.dir * 0.26f;
        // Tripping: the back hand comes off the pole and out, for balance.
        {
            const float side = (1 - front) == 1 ? 1.0f : -1.0f;
            g.hand[1 - front] = lerp (g.hand[1 - front], Vec3 { 0.0f, hipY + 0.78f, side * 0.8f }, flail);
        }

        // Listening hard: a hand cupped over one headphone, elbow out.
        const Vec3 ear { hc.x - 0.02f, hc.y - 0.03f, hs * 0.42f };
        const float cup = k.solo * (1.0f - gw);
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
        g.hand[hi] = lerp (g.hand[hi], target, gw);

        // Elbows: bent a little outwards and down, unless a gesture places one.
        for (int s = 0; s < 2; ++s)
        {
            const float side = s == 1 ? 1.0f : -1.0f;
            const Vec3 shoulder { 0.0f, hipY + 0.52f, side * 0.27f };
            g.elbow[s] = (shoulder + g.hand[s]) * 0.5f + Vec3 { -0.05f, -0.1f, side * 0.1f };
        }
        g.elbow[hi] = lerp (g.elbow[hi], elbowAt, elbowW);
        g.elbow[hi] = lerp (g.elbow[hi], Vec3 { hc.x + 0.1f, hc.y - 0.4f, hs * 0.62f }, cup * (1.0f - elbowW));

        g.eyes = std::min (g.eyes, 1.0f - k.blink.closed());
        g.mouth = std::max ({ g.mouth, 0.9f * k.recoil, 0.6f * flail });
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
            const Vec3 hip { 0.0f, hipY, side * 0.16f };
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
            const Vec3 b { keeper.cableX - trail * 0.9f, 0.03f, kKeeperZ - 0.3f }, c { keeper.cableX - trail * 3.0f, 0.03f, kKeeperZ - 0.4f };
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
        const Vec3 head { 0.04f, hipY + 1.08f, 0.0f };
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
                r.blob (Xform::move ({ head.x + faceX (0.04f, ez) - 0.005f, head.y + 0.04f, ez }).then (hd), 0.03f, std::max (0.008f, 0.075f * g.eyes), 0.05f, cans, 5, 3, 0.5f);
            r.blob (Xform::move ({ head.x - 0.03f, head.y - 0.03f, side * (hz + 0.02f) }).then (hd), 0.13f, 0.14f, 0.07f, band, 9, 4);           // ear cups
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
        keeperHatAt = Xform::rotY (g.hatTwist).then (Xform::move ({ head.x - 0.03f, head.y + 0.2f + g.hatLift, 0.0f })).then (hd);

        // The pole, with a big fluffy end out in front.
        // It telescopes: a thinner second section slides out for a goose further off.
        const Vec3 back = g.grip - g.dir * 0.7f, joint = g.grip + g.dir * 1.3f, tip = g.grip + g.dir * (g.len - 0.2f);
        r.limb (up.apply (back), up.apply (joint), 0.04f, 0.035f, rgb (0x505868), 5);
        r.limb (up.apply (joint), up.apply (tip), 0.026f, 0.024f, rgb (0x6a7282), 5);
        r.blob (Xform::rotZ (g.ang).then (Xform::rotY (g.pyaw)).then (Xform::move (g.grip + g.dir * g.len)).then (up), 0.34f, 0.17f, 0.17f, fur, 8, 5);

        // Arms: two short pieces from the shoulders, bent at the elbow, with round mitts.
        for (int s = 0; s < 2; ++s)
        {
            const float side = s == 1 ? 1.0f : -1.0f;
            const Vec3 shoulder { 0.0f, hipY + 0.52f, side * 0.27f };
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
    }

    /** His hat, wherever it is: on his head, on a goose, or flying between the two. */
    void drawHat()
    {
        Xform x = keeperHatAt;
        if (hat.state != 0 && hat.goose >= 0)
        {
            const Vec3 a = keeperHatAt.t, b = gooseHatAt.t;
            if (hat.state == 2)
                x = gooseHatAt;
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
        if (fish.t >= kFishLeap)
            return;
        const float t = fish.t / kFishLeap, x = fishX() + fish.dir * (t - 0.5f) * 1.5f;
        // High enough that the twist at the top happens clear of the reed tops.
        const float y = (2.5f + 0.9f * fish.power) * cast * std::sin (t * kPi) - 0.15f;
        // Nose up out of the water, a full twist at the top, and flat on its belly at the end.
        const float tilt = (0.75f * (1.0f - smooth01 (t / 0.55f)) - 0.1f * smooth01 ((t - 0.55f) / 0.3f)) * 1.4f;
        const float twist = 6.2832f * smooth01 ((t - 0.3f) / 0.4f);
        const float stretch = 1.0f + 0.25f * bump (t / 0.3f);
        const Xform at = Xform::scale (1.4f * cast).then (Xform::rotX (twist)).then (Xform::rotZ (tilt))
                             .then (Xform::rotY (fish.dir > 0.0f ? 0.0f : kPi)).then (Xform::move ({ x, y, kFishZ }));
        const float flick = 0.12f * std::sin (fish.t * 30.0f);
        r.blob (at, 0.34f * stretch, 0.16f, 0.1f, rgb (0xf4b6c6), 8, 4, 0.35f);
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
        for (float side : { -1.0f, 1.0f })
        {
            if (lazy > 0.5f)
                happyEye (head, { 0.13f, 0.08f, side * 0.1f }, 0.035f, 0.018f, 0.011f, dark);     // eyes shut, smiling
            else
            {
                const float open = happy ? 0.25f : (1.0f - b.blink.closed()) * (1.0f - 0.45f * b.sulk);
                r.blob (Xform::move ({ 0.12f, 0.07f, side * 0.1f }).then (head), 0.03f, std::max (0.007f, 0.04f * open), 0.03f, dark, 5, 3);
            }
            const float flick = side > 0.0f ? 0.5f * bump (b.ear) : 0.0f;
            r.blob (Xform::rotX (side * flick).then (Xform::move ({ -0.04f, 0.15f, side * 0.13f })).then (head), 0.05f, 0.05f, 0.03f, fur * 0.85f, 6, 3);
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
        return lerp (a, b, kTurtleAlong[i]) + Vec3 { 0.0f, rad * 0.9f, 0.0f };
    }
    static constexpr float kTurtleAlong[3] = { 0.13f, 0.48f, 0.83f };

    void drawLogAndTurtles()
    {
        const Rgb bark = rgb (0x9a7a5a), endC = rgb (0xe0c49a);
        const float k = fg;
        const float logY = 0.06f * k - logSink, rad = 0.2f * k;          // riding low enough to keep clear of the geese (see setSize)
        const Vec3 a { logX - kLogHalf * k, logY, streamZ - 0.05f * k }, b { logX + kLogHalf * k, logY + 0.02f * k, streamZ + 0.1f * k };
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
            for (float side : { -1.0f, 1.0f })
            {
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
                pos.y += (0.62f * k * fr.leapH) * std::sin (t * kPi) - 0.25f * k * smooth01 ((t - 0.75f) / 0.25f);
                sq = -0.3f * bump (t / 0.5f);
                legs = bump (t * 1.4f);
                spin = fr.side * 0.6f * std::sin (t * kPi);
                yaw = fr.side > 0.0f ? 0.2f : kPi - 0.2f;
                break;
            }
            case 3: pos.x = fr.landX; pos.y = -0.5f * k; break;                                         // under
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
    std::array<Burst, 8> bursts {};
    std::array<HotSpot, 24> hots {};               // where the mouse can find someone, as last drawn
    int hotCount = 0;
    Hot clicked = Hot::none;                       // a click waiting for the next step
    int clickedIndex = 0;
    std::array<TreeTop, kNumEdges> tops {};
    Rgb moduleCol[kNumModules + 1] { rgb (0xb9a8e0), rgb (0xf2c14e), rgb (0xb7c468), rgb (0xf2946b), rgb (0x8fd0e2) };
    std::array<Turtle, 3> turtles {};
    std::array<Fly, 3> flies {};
    Keeper keeper;
    Fish fish;
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
    bool swingHit = false;                         // this step, the pole came down
    float axisWas[2] { 0.0f, 1.0f };
    float pp = 1.0f;                               // the picture's pixels per point of the window
    std::array<Cand, kMaxCands> cands {};          // notches that may yet bring a goose
    float spawnCool = 0.0f;                        // seconds before the next goose may arrive
    float silence = 0.0f;                          // seconds with no programme and nothing cut
};

} // namespace dehonk::scene

// Renders Swooshbuckler's app icon in the window's own look: ship's ironwork with brass, stamped on plates.
//
// The tile is a brass plate with hard 45 degree cuts at its corners (the gussets of the sea's frame, a gunmetal bolt in
// each) round a window onto the sea: a night sky over the line-drawn swell, a crescent moon, and a comet in Bus A's
// coral sweeping behind the crossed cutlasses (the app's mark, top left of the window), which are the hero. Depth is
// flat and hard: a dark pressed outline, a lit lip and a dark lip on every stamped part, hard facets on the frame. No
// glows, no blurs, no round corners: the only round things are round on a ship (bolt heads, the moon, a comet's head).
//
// It writes three files:
//   <out>.png        the 1024 x 1024 master (ICON_BIG): the .icns from 64 px up, the window's own icon
//   <out>-small.png  32 x 32 (ICON_SMALL): the .icns at 16 and 32 px, drawn simpler (no bolts, stars, moon or fine
//                    swell; thicker blades and outlines) so it stays crisp in a Finder list or a taskbar
//   <out>.ico        the Windows installer's icon: 16, 24 and 32 px from the simple drawing, 48 to 256 from the master
//   Icon.icns        (on a Mac) the macOS app icon at every size, the plate set on the system's rounded tile
//
// Not part of the release build: run it after changing the artwork, and commit the three files.
//
//   cmake -B build -DSWOOSHBUCKLER_TOOLS=ON && cmake --build build --target SwooshbucklerIcon
//   ./build/SwooshbucklerIcon_artefacts/Release/SwooshbucklerIcon Resources/icon.png [--sheet]
//
// --sheet writes, instead of the icon, a sheet of it at 256 down to 16 px on a dark and a light ground (the sizes as
// shipped, so 32 and 16 are the simple drawing), with 32 and 16 also blown up four times. --simple writes only the
// simple drawing at 1024 px, to look at it closely.

#include <JuceHeader.h>

namespace
{
    // Theme::Colours (Source/Theme.h), by value: the tool builds without the plug-in's binary data that Theme.h includes.
    namespace C
    {
        const juce::Colour window    { 0xff081820 };
        const juce::Colour panel     { 0xff102a35 };
        const juce::Colour raised    { 0xff1a3b48 };
        const juce::Colour sand      { 0xfff2e8d2 };
        const juce::Colour brass     { 0xffe6b455 };
        const juce::Colour brassDark { 0xff7d5a26 };
        const juce::Colour busA      { 0xffff8466 }; // coral
        const juce::Colour busB      { 0xff3ccbc2 }; // turquoise
        const juce::Colour edgeDark  { 0xff040e13 };
        const juce::Colour bolt      { 0xff7e99a1 }; // gunmetal
        const juce::Colour boltSlot  { 0xff0b1c24 };
        // The sea (Source/SeaVisualiser.cpp).
        const juce::Colour deepWater { 0xff1f5f82 };
        const juce::Colour nearWater { 0xff79b6c6 };
        const juce::Colour hollow    { 0xff071820 };
        const juce::Colour moonlight { 0xfff3e3c0 };
        // Steel for the blades, wood for the grips.
        const juce::Colour steel     { 0xffc9d2cf };
        const juce::Colour steelLit  { 0xfff6f1e4 };
        const juce::Colour steelDark { 0xff6c8189 };
        const juce::Colour brassLit  { 0xfffbd98c };
        const juce::Colour grip      { 0xff5b3a22 };
    }

    constexpr int canvas = 1024;
    using P2 = juce::Point<float>;

    /** How the icon is drawn: in full (the master) or simple (for 32 px and below). */
    struct Style
    {
        bool simple = false;
        bool mac = false; // set on the macOS tile: see render()
        float k(float full, float small) const { return simple ? small : full; }
    };

    /** Theme::Plate::shape: the rectangle with its corners cut straight by `cut`, as its eight corners. */
    std::array<P2, 8> octagonPoints(juce::Rectangle<float> r, float cut)
    {
        const float x0 = r.getX(), y0 = r.getY(), x1 = r.getRight(), y1 = r.getBottom(), c = cut;
        return { { { x0 + c, y0 }, { x1 - c, y0 }, { x1, y0 + c }, { x1, y1 - c }, { x1 - c, y1 }, { x0 + c, y1 }, { x0, y1 - c }, { x0, y0 + c } } };
    }

    juce::Path polygon(const std::vector<P2>& points)
    {
        juce::Path path;
        path.startNewSubPath(points.front());
        for (size_t i = 1; i < points.size(); ++i)
            path.lineTo(points[i]);
        path.closeSubPath();
        return path;
    }

    juce::Path octagon(juce::Rectangle<float> r, float cut)
    {
        const auto p = octagonPoints(r, cut);
        return polygon({ p.begin(), p.end() });
    }

    juce::Path disc(P2 c, float r)
    {
        juce::Path p;
        p.addEllipse(c.x - r, c.y - r, 2.0f * r, 2.0f * r); // round object
        return p;
    }

    juce::PathStrokeType hard(float width) { return { width, juce::PathStrokeType::mitered, juce::PathStrokeType::butt }; }

    /** Stamps a part: a dark pressed outline round it, then the part flat in `base` with a lit lip along the edges
        that face the light (up and left) and a dark lip along the others. Overlapping paths are one part. */
    void stamp(juce::Graphics& g, const std::vector<juce::Path>& paths, juce::Colour base, juce::Colour lit, juce::Colour dark,
               float outline, P2 lip)
    {
        if (outline > 0.0f)
        {
            g.setColour(C::edgeDark);
            for (const auto& p : paths)
                g.strokePath(p, hard(outline * 2.0f));
        }
        juce::Image mask(juce::Image::ARGB, canvas, canvas, true);
        {
            juce::Graphics mg(mask);
            mg.setColour(juce::Colours::white);
            for (const auto& p : paths)
                mg.fillPath(p);
        }
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(mask, {});
        g.setColour(dark);
        g.fillAll();
        g.reduceClipRegion(mask, juce::AffineTransform::translation(lip));
        g.setColour(lit);
        g.fillAll();
        g.reduceClipRegion(mask, juce::AffineTransform::translation(-lip));
        g.setColour(base);
        g.fillAll();
    }

    /** Theme::Plate::drawBolt at icon size: a dark seat, a gunmetal head lit along its top, and the slot. */
    void drawBolt(juce::Graphics& g, P2 centre, float diameter, float turn)
    {
        const float r = diameter * 0.5f;
        g.setColour(C::edgeDark);
        g.fillPath(disc(centre, r * 1.18f));
        g.setColour(C::bolt.darker(0.5f));
        g.fillPath(disc(centre, r));
        g.setColour(C::bolt);
        g.fillPath(disc(centre.translated(-r * 0.08f, -r * 0.12f), r * 0.84f));
        const P2 dir { std::cos(turn), std::sin(turn) };
        juce::Path slot;
        slot.startNewSubPath(centre - dir * (r * 0.74f));
        slot.lineTo(centre + dir * (r * 0.74f));
        g.setColour(C::boltSlot);
        g.strokePath(slot, hard(r * 0.34f));
    }

    //==============================================================================
    /** The tile: an 824 px plate (the macOS icon grid) with its corners cut, a brass frame in eight flat facets whose
        corners are the big gussets of the sea's frame, a gunmetal bezel round the window and a bolt in each gusset. */
    struct Tile
    {
        explicit Tile(Style s) : style(s), edge(s.k(12.0f, 34.0f)), frame(s.k(22.0f, 30.0f)) {}

        Style style;
        juce::Rectangle<float> body { 100.0f, 100.0f, 824.0f, 824.0f };
        float outerCut = 132.0f, innerCut = 220.0f, edge, frame;

        juce::Rectangle<float> inner() const { return body.reduced(edge + frame); }
        juce::Path innerPath() const { return octagon(inner(), innerCut); }

        void drawBack(juce::Graphics& g) const
        {
            // The pressed edge (no drop shadow: the plate's own dark outline carries its edge on any ground).
            g.setColour(C::edgeDark);
            g.fillPath(octagon(body, outerCut));
            // The frame in eight flat facets, each lit by which way it faces; the gussets a step darker than the rails
            // (they are pressed in a little).
            const auto outer = octagonPoints(body.reduced(edge), outerCut - edge * 0.414f);
            const auto in = octagonPoints(inner(), innerCut);
            const P2 light = P2(-0.45f, -1.0f) / std::hypot(0.45f, 1.0f);
            const auto base = C::brass.darker(0.12f);
            for (size_t i = 0; i < 8; ++i)
            {
                const size_t j = (i + 1) % 8;
                const auto d = outer[j] - outer[i];
                const P2 normal = P2(d.y, -d.x) / std::hypot(d.x, d.y);
                const float lit = normal.x * light.x + normal.y * light.y;
                auto colour = lit >= 0.0f ? base.brighter(0.3f * lit) : base.darker(0.9f * -lit);
                if (i % 2 == 1)
                    colour = colour.darker(0.12f);
                g.setColour(colour);
                g.fillPath(polygon({ outer[i], outer[j], in[j], in[i] }));
            }
            // The hard lip just inside the outer edge: lit along the top and left, dark along the bottom and right.
            if (! style.simple)
            {
                const auto lip = octagonPoints(body.reduced(edge + 3.0f), outerCut - (edge + 3.0f) * 0.414f);
                for (size_t i = 0; i < 8; ++i)
                {
                    juce::Path seg;
                    seg.startNewSubPath(lip[i]);
                    seg.lineTo(lip[(i + 1) % 8]);
                    const bool up = (i == 7 || i == 0 || i == 6 || i == 1);
                    g.setColour(up ? C::brassLit.withAlpha(0.9f) : C::brassDark.darker(0.4f));
                    g.strokePath(seg, hard(6.0f));
                }
            }
        }

        void drawFront(juce::Graphics& g) const
        {
            // The window pressed in: a gunmetal bezel, then a dark line round the opening.
            g.setColour(C::bolt.darker(0.25f));
            g.strokePath(innerPath(), hard(style.k(20.0f, 24.0f)));
            g.setColour(C::edgeDark);
            g.strokePath(innerPath(), hard(style.k(8.0f, 14.0f)));
            if (style.simple)
                return;
            // A bolt in each gusset, halfway between the outer cut and the window's.
            const float outerDiag = outerCut / std::sqrt(2.0f);
            const float innerDiag = (edge + frame) * std::sqrt(2.0f) + innerCut / std::sqrt(2.0f);
            const float along = (outerDiag + innerDiag) * 0.5f / std::sqrt(2.0f);
            const float diameter = (innerDiag - outerDiag) * 0.56f;
            const P2 corners[4] = { body.getTopLeft(), body.getTopRight(), body.getBottomRight(), body.getBottomLeft() };
            const P2 inward[4] = { { 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 } };
            const float turns[4] = { 0.6f, 2.1f, 1.2f, 2.6f };
            for (int i = 0; i < 4; ++i)
                drawBolt(g, corners[i] + inward[i] * along, diameter, turns[i]);
        }
    };

    //==============================================================================
    /** The sky: the panel's teal-navy, lighter toward the horizon as the sea's sky is. */
    void fillSky(juce::Graphics& g, juce::Rectangle<float> r, float horizon)
    {
        g.setGradientFill(juce::ColourGradient(C::panel.darker(0.2f), 0.0f, r.getY(), C::raised.brighter(0.15f), 0.0f, horizon, false));
        g.fillRect(r);
    }

    /** The line-drawn swell, as the sea draws it: rows receding to the horizon, each a line over the dark water between
        rows; nearer rows lower, further apart, taller, brighter and thicker, with peaked crests. The nearest line is
        Bus B's turquoise. */
    void drawSwell(juce::Graphics& g, juce::Rectangle<float> area, Style style)
    {
        const int rows = style.simple ? 2 : 4;
        g.setColour(C::hollow);
        g.fillRect(area.withTrimmedTop(-2.0f));
        // The horizon: a hard line where the water starts.
        g.setColour(C::deepWater.withAlpha(0.8f));
        g.fillRect(area.withHeight(style.k(4.0f, 10.0f)));
        for (int row = 0; row < rows; ++row)
        {
            const float near = static_cast<float>(row + 1) / static_cast<float>(rows);
            const float y = area.getY() + area.getHeight() * (style.simple ? 0.22f + 0.5f * near : 0.06f + 0.80f * std::pow(near, 1.6f));
            const float amp = style.k(8.0f + 44.0f * std::pow(near, 1.3f), 34.0f);
            const float wave = style.k(0.030f / (0.6f + near), 0.016f);
            const float phase = 1.3f + 2.1f * static_cast<float>(row);
            juce::Path line;
            for (int i = 0; i <= 160; ++i)
            {
                const float x = area.getX() - 20.0f + (area.getWidth() + 40.0f) * static_cast<float>(i) / 160.0f;
                // Peaked crests over broad troughs (a trochoid, near enough): the crest is where the cosine is highest.
                const float c = 0.5f + 0.5f * std::cos(x * wave + phase);
                const float yy = y - amp * (std::pow(c, 4.0f) - 0.25f);
                if (i == 0)
                    line.startNewSubPath(x, yy);
                else
                    line.lineTo(x, yy);
            }
            juce::Path water = line;
            water.lineTo(area.getRight() + 20.0f, area.getBottom() + 40.0f);
            water.lineTo(area.getX() - 20.0f, area.getBottom() + 40.0f);
            water.closeSubPath();
            g.setColour(C::hollow.interpolatedWith(C::window, near));
            g.fillPath(water);
            const bool front = row == rows - 1;
            const auto colour = front ? C::busB.darker(0.15f) : C::deepWater.interpolatedWith(C::nearWater, near * 0.8f);
            g.setColour(colour.withAlpha(front ? 1.0f : 0.55f + 0.4f * near));
            g.strokePath(line, juce::PathStrokeType(style.k(5.0f + 9.0f * near, 22.0f), juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
        }
    }

    /** Stars: a few hard squares, big enough to be stars and not noise. */
    void drawStars(juce::Graphics& g)
    {
        const P2 at[] = { { 238.0f, 326.0f }, { 468.0f, 196.0f }, { 836.0f, 548.0f }, { 196.0f, 572.0f }, { 736.0f, 196.0f } };
        const float size[] = { 10.0f, 9.0f, 8.0f, 8.0f, 7.0f };
        for (int i = 0; i < 5; ++i)
        {
            g.setColour(C::sand.withAlpha(0.75f));
            g.fillRect(juce::Rectangle<float>(size[i], size[i]).withCentre(at[i]));
        }
    }

    /** The crescent moon: a disc with a bite out of it, hard edged, cream as in The Sea. */
    void drawMoon(juce::Graphics& g, P2 c, float r)
    {
        juce::Path moon = disc(c, r);
        juce::Path crescent;
        crescent.addPath(moon);
        crescent.addPath(disc(c.translated(r * 0.52f, -r * 0.34f), r * 0.86f));
        crescent.setUsingNonZeroWinding(false); // the bite drops out where it overlaps the disc
        juce::Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(moon);
        g.setColour(C::moonlight);
        g.fillPath(crescent);
    }

    /** A comet as the sea draws one, made hard: a round sand head with a hard rim in the bus colour, and a tail that
        tapers back along a curve to a point, in flat bands stepping out from a sand core to the bus colour. */
    void drawComet(juce::Graphics& g, P2 tail, P2 control, P2 head, float width, juce::Colour colour, Style style)
    {
        auto band = [&](float scale)
        {
            constexpr int n = 90;
            std::vector<P2> left, right;
            for (int i = 0; i <= n; ++i)
            {
                const float u = static_cast<float>(i) / n;
                const float v = 1.0f - u;
                const P2 p = tail * (v * v) + control * (2.0f * u * v) + head * (u * u);
                const P2 d = (control - tail) * (2.0f * v) + (head - control) * (2.0f * u);
                const P2 normal = P2(-d.y, d.x) / std::hypot(d.x, d.y);
                const float half = 0.5f * width * scale * std::pow(u, 1.1f);
                left.push_back(p + normal * half);
                right.push_back(p - normal * half);
            }
            std::vector<P2> all(left.begin(), left.end());
            all.insert(all.end(), right.rbegin(), right.rend());
            return std::vector<juce::Path> { polygon(all), disc(head, 0.5f * width * scale) };
        };
        auto fill = [&](const std::vector<juce::Path>& parts, juce::Colour c)
        {
            g.setColour(c);
            for (const auto& p : parts)
                g.fillPath(p);
        };
        g.setColour(C::edgeDark);
        for (const auto& p : band(1.0f))
            g.strokePath(p, hard(style.k(16.0f, 26.0f)));
        fill(band(1.0f), colour);
        if (! style.simple)
            fill(band(0.68f), colour.interpolatedWith(C::sand, 0.4f));
        fill(band(style.k(0.40f, 0.48f)), colour.interpolatedWith(C::sand, 0.75f));
        // The head: a sand disc inside a hard coral rim.
        g.setColour(C::sand);
        g.fillPath(disc(head, width * style.k(0.36f, 0.32f)));
    }

    //==============================================================================
    /** One cutlass, the window's mark drawn for an icon: a curved blade narrow at the guard and widening toward a
        clipped point (the edge on the outside of the curve, a bright bevel along it), a short heavy guard across the
        blade's foot, a wooden grip and a brass cap. Laid from the guard `guard` to the point `tip`; `bow` bends the
        blade toward the side its edge is on. */
    struct Cutlass
    {
        juce::Path blade, bevel;
        std::vector<juce::Path> brass;
        juce::Path grip;
    };

    Cutlass makeCutlass(P2 guard, P2 tip, float bow, float wGuard, float wMax, float guardSize, float gripLength)
    {
        const P2 axis = (tip - guard) / guard.getDistanceFrom(tip);
        const P2 side { -axis.y, axis.x }; // the edge's side (the blade bows out this way)
        const float length = guard.getDistanceFrom(tip);
        auto centre = [&](float u) { return guard + axis * (length * u) + side * (4.0f * bow * u * (1.0f - u)); };
        auto normalAt = [&](float u)
        {
            const P2 d = axis * length + side * (4.0f * bow * (1.0f - 2.0f * u));
            const P2 t = d / std::hypot(d.x, d.y);
            return P2(-t.y, t.x);
        };
        constexpr int n = 120;
        std::vector<P2> back, edge, bevelIn;
        for (int i = 0; i <= n; ++i)
        {
            const float u = static_cast<float>(i) / n;
            const float grow = juce::jlimit(0.0f, 1.0f, u / 0.76f);
            const float w = wGuard + (wMax - wGuard) * grow * grow * (3.0f - 2.0f * grow);
            // The back runs straight on, then is clipped down to the point; the edge sweeps up to meet it.
            const float backHalf = 0.5f * w * (u < 0.74f ? 1.0f : juce::jmax(0.0f, 1.0f - (u - 0.74f) / 0.26f));
            const float e = juce::jmax(0.0f, u - 0.80f) / 0.20f;
            const float edgeHalf = 0.5f * w * std::sqrt(juce::jmax(0.0f, 1.0f - e * e));
            const P2 c = centre(u), nrm = normalAt(u);
            back.push_back(c - nrm * backHalf);
            edge.push_back(c + nrm * edgeHalf);
            bevelIn.push_back(c + nrm * juce::jmax(-backHalf, edgeHalf - 0.36f * w));
        }
        Cutlass c;
        std::vector<P2> outline(back.begin(), back.end());
        outline.insert(outline.end(), edge.rbegin(), edge.rend());
        c.blade = polygon(outline);
        std::vector<P2> bevel(bevelIn.begin(), bevelIn.end());
        bevel.insert(bevel.end(), edge.rbegin(), edge.rend());
        c.bevel = polygon(bevel);

        // The guard: a short heavy bar square across the blade's foot, reaching a little further on the edge's side,
        // its ends cut square; then the grip and the cap at its end (cut corners, like every plate).
        const float gt = guardSize * 0.62f, gripHalf = guardSize * 0.34f;
        const P2 g0 = guard - axis * (gt * 0.5f);
        const P2 lo = g0 - side * (guardSize * 0.95f), hi = g0 + side * (guardSize * 1.25f);
        c.brass.push_back(polygon({ lo + axis * (gt * 0.5f), hi + axis * (gt * 0.5f), hi - axis * (gt * 0.5f), lo - axis * (gt * 0.5f) }));
        const P2 g1 = g0 - axis * (gt * 0.5f), g2 = g1 - axis * gripLength;
        c.grip = polygon({ g1 - side * gripHalf, g1 + side * gripHalf, g2 + side * gripHalf, g2 - side * gripHalf });
        const float capHalf = guardSize * 0.52f, capLong = guardSize * 0.62f, cut = capHalf * 0.4f;
        const P2 q0 = g2 + axis * 2.0f, q1 = g2 - axis * capLong;
        c.brass.push_back(polygon({ q0 - side * capHalf, q0 + side * capHalf, q1 + side * capHalf + axis * cut, q1 + side * (capHalf - cut),
                                    q1 - side * (capHalf - cut), q1 - side * capHalf + axis * cut }));
        return c;
    }

    void drawCutlass(juce::Graphics& g, Cutlass c, const juce::AffineTransform& t, Style style)
    {
        c.blade.applyTransform(t);
        c.bevel.applyTransform(t);
        c.grip.applyTransform(t);
        for (auto& p : c.brass)
            p.applyTransform(t);
        const float outline = style.k(9.0f, 20.0f);
        // A hard shadow on the sky under the whole weapon.
        g.setColour(juce::Colours::black.withAlpha(0.42f));
        const auto drop = juce::AffineTransform::translation(style.k(6.0f, 10.0f), style.k(12.0f, 18.0f));
        g.fillPath(c.blade, drop);
        g.fillPath(c.grip, drop);
        for (const auto& p : c.brass)
            g.fillPath(p, drop);

        stamp(g, { c.grip }, C::grip, C::grip.brighter(0.5f), C::grip.darker(0.6f), outline, { -4.0f, -6.0f });
        stamp(g, { c.blade }, C::steel, C::steelLit, C::steelDark, outline, { style.k(-5.0f, -10.0f), style.k(-8.0f, -14.0f) });
        // The sharpened edge: a flat bright bevel, cut hard from the blade's face.
        {
            juce::Graphics::ScopedSaveState s(g);
            g.reduceClipRegion(c.blade);
            g.setColour(C::steelLit.withAlpha(style.k(0.85f, 0.6f)));
            g.fillPath(c.bevel);
        }
        stamp(g, c.brass, C::brass, C::brassLit, C::brassDark, outline, { -5.0f, -8.0f });
    }

    /** The macOS icon outline: a superellipse |x|^5 + |y|^5 = 1 over the 824 px square in the middle of the 1024 grid,
        which matches the system's rounded square (corner radius about 185 px) to within a pixel or two. */
    juce::Path macTile()
    {
        juce::Path p;
        constexpr int n = 720;
        constexpr float half = 412.0f, centre = canvas * 0.5f;
        for (int i = 0; i < n; ++i)
        {
            const float t = juce::MathConstants<float>::twoPi * static_cast<float>(i) / n;
            const float c = std::cos(t), s = std::sin(t);
            const P2 q { centre + half * std::copysign(std::pow(std::abs(c), 0.4f), c),
                         centre + half * std::copysign(std::pow(std::abs(s), 0.4f), s) };
            if (i == 0)
                p.startNewSubPath(q);
            else
                p.lineTo(q);
        }
        p.closeSubPath();
        return p;
    }

    //==============================================================================
    juce::Image render(Style style)
    {
        juce::Image image(juce::Image::ARGB, canvas, canvas, true);
        juce::Graphics g(image);
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);

        // On macOS the plate is set on a dark iron tile of the shape the system expects (a rounded square 824 px on
        // the 1024 grid). An icon of any other outline, like the bare cut-corner plate, is shrunk by macOS 26 onto a
        // pale tile of its own in the Dock and the app switcher. The plate is drawn a little smaller, so its cut
        // corners sit inside the tile's rounded ones.
        if (style.mac)
        {
            g.setColour(C::edgeDark);
            g.fillPath(macTile());
            g.addTransform(juce::AffineTransform::scale(0.955f, 0.955f, canvas * 0.5f, canvas * 0.5f));
        }

        const Tile tile(style);
        tile.drawBack(g);
        const auto win = tile.inner();
        const float horizon = win.getY() + win.getHeight() * 0.62f;
        {
            juce::Graphics::ScopedSaveState s(g);
            g.reduceClipRegion(tile.innerPath());
            fillSky(g, win, horizon);
            if (! style.simple)
            {
                drawStars(g);
                drawMoon(g, { 806.0f, 420.0f }, 50.0f);
            }
            drawSwell(g, win.withTop(horizon), style);
            // The comet sweeps up from the left, behind both blades above where they cross, its head in the open
            // sky between the points.
            drawComet(g, { 168.0f, 488.0f }, { 380.0f, 420.0f }, { 588.0f, 292.0f }, style.k(80.0f, 100.0f), C::busA, style);
        }
        tile.drawFront(g);

        // The crossed cutlasses, crossing at the middle of the tile: each laid from its guard to its point, its hilt
        // toward a lower corner; the second is the first in a mirror.
        const P2 capEnd { 288.0f, 782.0f }, tip { 722.0f, 248.0f };
        const P2 axis = (tip - capEnd) / capEnd.getDistanceFrom(tip);
        const float wGuard = style.k(46.0f, 60.0f), wMax = style.k(70.0f, 84.0f), guardSize = style.k(48.0f, 52.0f);
        const float gripLength = style.k(92.0f, 70.0f);
        const float hilt = guardSize * 0.62f + gripLength + guardSize * 0.62f;
        const auto blade = makeCutlass(capEnd + axis * hilt, tip, style.k(30.0f, 22.0f), wGuard, wMax, guardSize, gripLength);
        drawCutlass(g, blade, juce::AffineTransform::scale(-1.0f, 1.0f, canvas * 0.5f, 0.0f), style);
        drawCutlass(g, blade, {}, style);
        return image;
    }

    /** Scales an image down by halves (each step averages its pixels), then to the exact size, so a 1024 px drawing
        comes down to 16 px without aliasing. */
    juce::Image scaledTo(juce::Image image, int size)
    {
        while (image.getWidth() / 2 >= size)
            image = image.rescaled(image.getWidth() / 2, image.getHeight() / 2, juce::Graphics::highResamplingQuality);
        return image.getWidth() == size ? image : image.rescaled(size, size, juce::Graphics::highResamplingQuality);
    }

    /** The icon at a size as shipped: the simple drawing at 32 px and below, the master above. */
    juce::Image atSize(const juce::Image& master, const juce::Image& simple, int size)
    {
        return scaledTo(size <= 32 ? simple : master, size);
    }

    /** A Windows .ico holding PNG images (allowed since Vista): header, directory, then each PNG. */
    bool writeIco(const juce::File& file, const juce::Image& master, const juce::Image& simple, std::initializer_list<int> sizes)
    {
        std::vector<juce::MemoryBlock> pngs;
        for (int size : sizes)
        {
            juce::MemoryOutputStream png;
            juce::PNGImageFormat().writeImageToStream(atSize(master, simple, size), png);
            pngs.push_back(png.getMemoryBlock());
        }
        juce::MemoryOutputStream out;
        out.writeShort(0);
        out.writeShort(1);
        out.writeShort(static_cast<short>(pngs.size()));
        int offset = 6 + 16 * static_cast<int>(pngs.size());
        size_t i = 0;
        for (int size : sizes)
        {
            out.writeByte(static_cast<char>(size >= 256 ? 0 : size));
            out.writeByte(static_cast<char>(size >= 256 ? 0 : size));
            out.writeByte(0);
            out.writeByte(0);
            out.writeShort(1);
            out.writeShort(32);
            out.writeInt(static_cast<int>(pngs[i].getSize()));
            out.writeInt(offset);
            offset += static_cast<int>(pngs[i].getSize());
            ++i;
        }
        for (const auto& png : pngs)
            out.write(png.getData(), png.getSize());
        return file.replaceWithData(out.getData(), out.getDataSize());
    }

    /** A sheet for judging the icon small: on a dark and a light ground, the icon at 256, 128, 64, 32 and 16 px as
        shipped, then the 32 and 16 px ones blown up four times, pixel for pixel. */
    juce::Image contactSheet(const juce::Image& master, const juce::Image& simple)
    {
        const int sizes[] = { 256, 128, 64, 32, 16 };
        constexpr int gap = 24, rowHeight = 256 + 2 * gap;
        juce::Image sheet(juce::Image::ARGB, 1000, 2 * rowHeight, true);
        juce::Graphics g(sheet);
        const juce::Colour grounds[] = { juce::Colour(0xff1e1e1e), juce::Colour(0xffececec) };
        for (int row = 0; row < 2; ++row)
        {
            const int y0 = row * rowHeight;
            g.setColour(grounds[row]);
            g.fillRect(0, y0, sheet.getWidth(), rowHeight);
            int x = gap;
            for (int size : sizes)
            {
                g.drawImageAt(atSize(master, simple, size), x, y0 + gap);
                x += size + gap;
            }
            g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
            for (int size : { 32, 16 })
            {
                g.drawImage(atSize(master, simple, size), x, y0 + gap, size * 4, size * 4, 0, 0, size, size);
                x += size * 4 + gap;
            }
        }
        return sheet;
    }

    bool writePng(const juce::File& file, const juce::Image& image)
    {
        file.deleteFile();
        juce::FileOutputStream stream(file);
        if (! stream.openedOk() || ! juce::PNGImageFormat().writeImageToStream(image, stream))
        {
            std::cerr << "Couldn't write " << file.getFullPathName() << "\n";
            return false;
        }
        std::cout << "Wrote " << file.getFullPathName() << "\n";
        return true;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::String outName = "icon.png";
    bool sheet = false, simpleOnly = false;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg(argv[i]);
        if (arg == "--sheet")
            sheet = true;
        else if (arg == "--simple")
            simpleOnly = true;
        else
            outName = arg;
    }
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(outName);
    const auto master = render({ false });
    const auto simple = render({ true });
    if (sheet)
        return writePng(out, contactSheet(master, simple)) ? 0 : 1;
    if (simpleOnly)
        return writePng(out, simple) ? 0 : 1;

    const auto small = out.getSiblingFile(out.getFileNameWithoutExtension() + "-small.png");
    const auto ico = out.withFileExtension("ico");
    if (! writePng(out, master) || ! writePng(small, scaledTo(simple, 32)))
        return 1;
    if (! writeIco(ico, master, simple, { 16, 24, 32, 48, 64, 128, 256 }))
    {
        std::cerr << "Couldn't write " << ico.getFullPathName() << "\n";
        return 1;
    }
    std::cout << "Wrote " << ico.getFullPathName() << "\n";

   #if JUCE_MAC
    // The macOS icon, built by Apple's iconutil from a full set of sizes, so the Dock, Finder and the app switcher
    // each find a picture drawn for them. The build copies it into the app over the one JUCE makes from ICON_BIG.
    const auto macMaster = render({ false, true });
    const auto macSimple = render({ true, true });
    const auto iconset = out.getSiblingFile("Icon.iconset");
    iconset.deleteRecursively();
    iconset.createDirectory();
    for (int size : { 16, 32, 128, 256, 512 })
    {
        const auto name = "icon_" + juce::String(size) + "x" + juce::String(size);
        if (! writePng(iconset.getChildFile(name + ".png"), atSize(macMaster, macSimple, size))
            || ! writePng(iconset.getChildFile(name + "@2x.png"), atSize(macMaster, macSimple, size * 2)))
            return 1;
    }
    const auto icns = out.getSiblingFile("Icon.icns");
    juce::ChildProcess iconutil;
    const bool built = iconutil.start(juce::StringArray { "/usr/bin/iconutil", "-c", "icns", "-o", icns.getFullPathName(),
                                                         iconset.getFullPathName() })
                       && iconutil.waitForProcessToFinish(30000) && iconutil.getExitCode() == 0;
    iconset.deleteRecursively();
    if (! built)
    {
        std::cerr << "iconutil couldn't build " << icns.getFullPathName() << "\n";
        return 1;
    }
    std::cout << "Wrote " << icns.getFullPathName() << "\n";
   #endif
    return 0;
}

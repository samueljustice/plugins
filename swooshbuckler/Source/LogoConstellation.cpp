#include "LogoConstellation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace
{
    using Pt = juce::Point<float>;

    // Everything below is in normalised units (the logo's longer side is 1) unless it says pixels.
    constexpr float c_pixelsPerUnit = 480.0f;  // the mask's resolution
    constexpr int   c_margin        = 16;      // pixels of empty mask around the mark
    constexpr float c_closeGaps     = 0.008f;  // the white gaps between the grenade's parts are narrower than twice this
    constexpr float c_thin          = 0.013f;  // anything narrower than twice this is a stem or the lever's tail
    constexpr float c_ringClear     = 0.014f;  // nothing else is drawn this close to the ring
    constexpr float c_cherryClear   = 0.016f;  // nor this close to the cherry
    constexpr float c_edgeTolerance = 0.0085f; // how far the joined stars may cut inside a curve
    constexpr float c_lineTolerance = 0.013f;  // the same for the stems, which are thinned lines and wander a little
    constexpr float c_longestGap    = 0.13f;   // a longer straight run gets a star part of the way along
    constexpr float c_mergeStars    = 0.028f;  // two stars closer than this are one star
    constexpr float c_shortestLine  = 0.05f;   // shorter scraps are dropped
    constexpr float c_cherryNotch   = 0.02f;   // the notch the stem sits in, at the top of the cherry, is narrower than twice this
    constexpr float c_longestEdge   = 0.12f;   // an inner edge shorter than this is detail, not shape
    constexpr float c_reach         = 0.06f;   // the loose end of a line is joined to a star this near
    constexpr float c_cherryTolerance = 0.006f; // the cherry is the roundest thing in the mark: it gets a few more stars
    constexpr float c_innerWeight   = 0.12f;   // the stars of an inner edge are small: the silhouette leads
    constexpr int   c_ringStars     = 10;

    //==============================================================================
    struct Mask
    {
        int w = 0, h = 0;
        std::vector<uint8_t> v;

        Mask() = default;
        Mask(int width, int height) : w(width), h(height), v(static_cast<size_t>(width * height), 0) {}

        bool at(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h && v[static_cast<size_t>(y * w + x)] != 0; }
        uint8_t& ref(int x, int y) { return v[static_cast<size_t>(y * w + x)]; }
    };

    struct Frame
    {
        int w = 0, h = 0;

        Pt toUnits(float px, float py) const
        {
            return { (px + 0.5f - 0.5f * static_cast<float>(w)) / c_pixelsPerUnit,
                     -(py + 0.5f - 0.5f * static_cast<float>(h)) / c_pixelsPerUnit };
        }
        juce::AffineTransform toPixels() const
        {
            return juce::AffineTransform::scale(c_pixelsPerUnit, -c_pixelsPerUnit)
                .translated(0.5f * static_cast<float>(w), 0.5f * static_cast<float>(h));
        }
    };

    Mask rasterise(const juce::Path& path, const Frame& frame)
    {
        juce::Image image(juce::Image::ARGB, frame.w, frame.h, true, juce::SoftwareImageType());
        {
            juce::Graphics g(image);
            g.setColour(juce::Colours::white);
            g.fillPath(path, frame.toPixels());
        }
        Mask mask(frame.w, frame.h);
        const juce::Image::BitmapData data(image, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < frame.h; ++y)
            for (int x = 0; x < frame.w; ++x)
                mask.ref(x, y) = data.getPixelColour(x, y).getAlpha() > 127 ? 1 : 0;
        return mask;
    }

    /** Distance in pixels from every pixel to the nearest one that is set (or, with toSet false, that is clear). */
    std::vector<float> distanceTo(const Mask& mask, bool toSet)
    {
        const float far = 1.0e6f, diagonal = 1.41421f;
        std::vector<float> d(mask.v.size());
        for (size_t i = 0; i < d.size(); ++i)
            d[i] = ((mask.v[i] != 0) == toSet) ? 0.0f : far;

        const int w = mask.w, h = mask.h;
        auto at = [&](int x, int y) -> float& { return d[static_cast<size_t>(y * w + x)]; };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                float best = at(x, y);
                if (x > 0)               best = std::min(best, at(x - 1, y) + 1.0f);
                if (y > 0)               best = std::min(best, at(x, y - 1) + 1.0f);
                if (x > 0 && y > 0)      best = std::min(best, at(x - 1, y - 1) + diagonal);
                if (x < w - 1 && y > 0)  best = std::min(best, at(x + 1, y - 1) + diagonal);
                at(x, y) = best;
            }
        for (int y = h - 1; y >= 0; --y)
            for (int x = w - 1; x >= 0; --x)
            {
                float best = at(x, y);
                if (x < w - 1)              best = std::min(best, at(x + 1, y) + 1.0f);
                if (y < h - 1)              best = std::min(best, at(x, y + 1) + 1.0f);
                if (x < w - 1 && y < h - 1) best = std::min(best, at(x + 1, y + 1) + diagonal);
                if (x > 0 && y < h - 1)     best = std::min(best, at(x - 1, y + 1) + diagonal);
                at(x, y) = best;
            }
        return d;
    }

    Mask grown(const Mask& mask, float pixels)
    {
        const auto d = distanceTo(mask, true);
        Mask out(mask.w, mask.h);
        for (size_t i = 0; i < d.size(); ++i)
            out.v[i] = d[i] <= pixels ? 1 : 0;
        return out;
    }

    Mask shrunk(const Mask& mask, float pixels)
    {
        const auto d = distanceTo(mask, false);
        Mask out(mask.w, mask.h);
        for (size_t i = 0; i < d.size(); ++i)
            out.v[i] = d[i] > pixels ? 1 : 0;
        return out;
    }

    //==============================================================================
    /** The outer edge of every separate piece of the mask that has at least minArea pixels, as closed loops of pixels.
        Smaller pieces are taken out of the mask. */
    std::vector<std::vector<juce::Point<int>>> traceEdges(Mask& mask, int minArea)
    {
        static constexpr int dx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
        static constexpr int dy[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };

        std::vector<std::vector<juce::Point<int>>> loops;
        Mask seen(mask.w, mask.h);
        std::vector<juce::Point<int>> stack;

        for (int y = 0; y < mask.h; ++y)
            for (int x = 0; x < mask.w; ++x)
            {
                if (! mask.at(x, y) || seen.at(x, y))
                    continue;

                // The piece this pixel starts (it is the piece's first pixel in reading order, so it is on its outer edge).
                int area = 0;
                stack.push_back({ x, y });
                seen.ref(x, y) = 1;
                while (! stack.empty())
                {
                    const auto p = stack.back();
                    stack.pop_back();
                    ++area;
                    for (int k = 0; k < 8; ++k)
                    {
                        const int nx = p.x + dx[k], ny = p.y + dy[k];
                        if (mask.at(nx, ny) && ! seen.at(nx, ny))
                        {
                            seen.ref(nx, ny) = 1;
                            stack.push_back({ nx, ny });
                        }
                    }
                }
                if (area < minArea)
                {
                    stack.push_back({ x, y });
                    mask.ref(x, y) = 0;
                    while (! stack.empty())
                    {
                        const auto p = stack.back();
                        stack.pop_back();
                        for (int k = 0; k < 8; ++k)
                        {
                            const int nx = p.x + dx[k], ny = p.y + dy[k];
                            if (mask.at(nx, ny))
                            {
                                mask.ref(nx, ny) = 0;
                                stack.push_back({ nx, ny });
                            }
                        }
                    }
                    continue;
                }

                // Walk round it, keeping the outside on the left.
                std::vector<juce::Point<int>> loop;
                juce::Point<int> current { x, y };
                int direction = 0;
                const int limit = 8 * (mask.w + mask.h) + 4 * area;
                for (int step = 0; step < limit; ++step)
                {
                    loop.push_back(current);
                    bool moved = false;
                    for (int k = 0; k < 8; ++k)
                    {
                        const int d = (direction + 5 + k) % 8;
                        if (mask.at(current.x + dx[d], current.y + dy[d]))
                        {
                            current = { current.x + dx[d], current.y + dy[d] };
                            direction = d;
                            moved = true;
                            break;
                        }
                    }
                    if (! moved || (current.x == x && current.y == y))
                        break;
                }
                if (loop.size() >= 8)
                    loops.push_back(std::move(loop));
            }
        return loops;
    }

    //==============================================================================
    /** Thins the mask to lines one pixel wide (Zhang and Suen's method). */
    void thin(Mask& mask)
    {
        std::vector<size_t> remove;
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (int pass = 0; pass < 2; ++pass)
            {
                remove.clear();
                for (int y = 1; y < mask.h - 1; ++y)
                    for (int x = 1; x < mask.w - 1; ++x)
                    {
                        if (! mask.at(x, y))
                            continue;
                        const int n[8] = { mask.at(x, y - 1), mask.at(x + 1, y - 1), mask.at(x + 1, y), mask.at(x + 1, y + 1),
                                           mask.at(x, y + 1), mask.at(x - 1, y + 1), mask.at(x - 1, y), mask.at(x - 1, y - 1) };
                        int count = 0, rises = 0;
                        for (int k = 0; k < 8; ++k)
                        {
                            count += n[k];
                            rises += (n[k] == 0 && n[(k + 1) % 8] == 1) ? 1 : 0;
                        }
                        if (count < 2 || count > 6 || rises != 1)
                            continue;
                        const bool go = pass == 0 ? (n[0] * n[2] * n[4] == 0 && n[2] * n[4] * n[6] == 0)
                                                  : (n[0] * n[2] * n[6] == 0 && n[0] * n[4] * n[6] == 0);
                        if (go)
                            remove.push_back(static_cast<size_t>(y * mask.w + x));
                    }
                for (const auto i : remove)
                    mask.v[i] = 0;
                changed = changed || ! remove.empty();
            }
        }
    }

    /** The lines of a thinned mask, each from one end or fork to the next. */
    std::vector<std::vector<juce::Point<int>>> traceLines(const Mask& lines)
    {
        static constexpr int dx[8] = { 1, 0, -1, 0, 1, 1, -1, -1 };   // the straight neighbours first
        static constexpr int dy[8] = { 0, 1, 0, -1, 1, -1, 1, -1 };

        auto neighbours = [&](juce::Point<int> p)
        {
            int count = 0;
            for (int k = 0; k < 8; ++k)
                count += lines.at(p.x + dx[k], p.y + dy[k]) ? 1 : 0;
            return count;
        };

        std::vector<std::vector<juce::Point<int>>> out;
        Mask seen(lines.w, lines.h);
        struct Start { juce::Point<int> at, from; bool hasFrom; };
        std::vector<Start> starts;

        auto follow = [&](Start start)
        {
            std::vector<juce::Point<int>> line;
            if (start.hasFrom)
                line.push_back(start.from);
            auto current = start.at;
            for (;;)
            {
                line.push_back(current);
                juce::Point<int> next[8];
                int numNext = 0;
                for (int k = 0; k < 8; ++k)
                {
                    const juce::Point<int> q { current.x + dx[k], current.y + dy[k] };
                    if (lines.at(q.x, q.y) && ! seen.at(q.x, q.y))
                        next[numNext++] = q;
                }
                if (numNext == 0)
                    break;

                // A line one pixel wide that steps sideways has two pixels ahead that touch each other: that is the
                // line carrying on, not a fork. It is a fork only when two of the ways on do not touch.
                bool fork = false;
                for (int a = 0; a < numNext; ++a)
                    for (int b = a + 1; b < numNext; ++b)
                        fork = fork || std::abs(next[a].x - next[b].x) > 1 || std::abs(next[a].y - next[b].y) > 1;
                if (! fork)
                {
                    current = next[0];
                    seen.ref(current.x, current.y) = 1;
                    continue;
                }
                for (int k = 0; k < numNext; ++k)
                    seen.ref(next[k].x, next[k].y) = 1;
                for (int k = 0; k < numNext; ++k)
                    starts.push_back({ next[k], current, true });
                break;
            }
            out.push_back(std::move(line));
        };

        // Ends first, so that a line is walked from its tip; whatever is left after that is a closed loop.
        for (int wantEnds = 1; wantEnds >= 0; --wantEnds)
            for (int y = 0; y < lines.h; ++y)
                for (int x = 0; x < lines.w; ++x)
                {
                    if (! lines.at(x, y) || seen.at(x, y) || (wantEnds == 1 && neighbours({ x, y }) != 1))
                        continue;
                    seen.ref(x, y) = 1;
                    starts.push_back({ { x, y }, {}, false });
                    while (! starts.empty())
                    {
                        const auto start = starts.back();
                        starts.pop_back();
                        follow(start);
                    }
                }
        return out;
    }

    //==============================================================================
    float lengthOf(const std::vector<Pt>& points)
    {
        float length = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
            length += points[i].getDistanceFrom(points[i - 1]);
        return length;
    }

    float distanceToSegment(Pt p, Pt a, Pt b)
    {
        const auto ab = b - a;
        const float lengthSquared = ab.x * ab.x + ab.y * ab.y;
        if (lengthSquared <= 1.0e-12f)
            return p.getDistanceFrom(a);
        const float t = juce::jlimit(0.0f, 1.0f, ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / lengthSquared);
        return p.getDistanceFrom(a + ab * t);
    }

    /** Douglas and Peucker's simplification of an open run: the indices of the points worth keeping, in order. */
    std::vector<int> simplify(const std::vector<Pt>& points, int first, int last, float tolerance)
    {
        std::vector<uint8_t> keep(points.size(), 0);
        keep[static_cast<size_t>(first)] = keep[static_cast<size_t>(last)] = 1;
        std::vector<std::pair<int, int>> spans { { first, last } };
        while (! spans.empty())
        {
            const auto span = spans.back();
            spans.pop_back();
            float worst = 0.0f;
            int at = -1;
            for (int i = span.first + 1; i < span.second; ++i)
            {
                const float d = distanceToSegment(points[static_cast<size_t>(i)], points[static_cast<size_t>(span.first)],
                                                  points[static_cast<size_t>(span.second)]);
                if (d > worst)
                {
                    worst = d;
                    at = i;
                }
            }
            if (at >= 0 && worst > tolerance)
            {
                keep[static_cast<size_t>(at)] = 1;
                spans.push_back({ span.first, at });
                spans.push_back({ at, span.second });
            }
        }

        // A long straight run still wants a star or two along it, or the eye loses the line.
        std::vector<int> kept;
        int previous = -1;
        for (int i = first; i <= last; ++i)
        {
            if (keep[static_cast<size_t>(i)] == 0)
                continue;
            if (previous >= 0)
            {
                const float gap = points[static_cast<size_t>(i)].getDistanceFrom(points[static_cast<size_t>(previous)]);
                const int pieces = static_cast<int>(std::ceil(gap / c_longestGap));
                for (int k = 1; k < pieces; ++k)
                    kept.push_back(previous + (i - previous) * k / pieces);
            }
            kept.push_back(i);
            previous = i;
        }
        return kept;
    }

    //==============================================================================
    struct Builder
    {
        LogoConstellation& out;
        std::vector<int> ends;

        int star(Pt at, float weight, bool cherry)
        {
            for (size_t i = 0; i < out.stars.size(); ++i)
                if (out.stars[i].at.getDistanceFrom(at) < c_mergeStars)
                {
                    out.stars[i].weight = std::max(out.stars[i].weight, weight);
                    return static_cast<int>(i);
                }
            out.stars.push_back({ at, weight, cherry });
            return static_cast<int>(out.stars.size()) - 1;
        }

        void join(int a, int b)
        {
            if (a == b)
                return;
            for (const auto& line : out.lines)
                if ((line.first == a && line.second == b) || (line.first == b && line.second == a))
                    return;
            out.lines.push_back({ a, b });
        }

        /** Adds a run of points as stars joined in order. Each star's weight is how sharply the line turns at it. */
        void add(const std::vector<Pt>& points, bool closed, bool cherry, float heaviest = 1.0f)
        {
            const int n = static_cast<int>(points.size());
            if (n < 2)
                return;
            std::vector<int> ids;
            for (int i = 0; i < n; ++i)
            {
                float weight = 0.55f;  // the end of a line
                if (closed || (i > 0 && i < n - 1))
                {
                    const auto a = points[static_cast<size_t>((i + n - 1) % n)], b = points[static_cast<size_t>(i)],
                               c = points[static_cast<size_t>((i + 1) % n)];
                    const float turn = std::abs(std::remainder(std::atan2(c.y - b.y, c.x - b.x) - std::atan2(b.y - a.y, b.x - a.x),
                                                               juce::MathConstants<float>::twoPi));
                    weight = juce::jlimit(0.0f, 1.0f, (turn - 0.5f) / 0.9f);   // about 30 degrees is a curve, 80 a corner
                }
                ids.push_back(star(points[static_cast<size_t>(i)], std::min(weight, heaviest), cherry));
            }
            for (int i = 0; i + 1 < n; ++i)
                join(ids[static_cast<size_t>(i)], ids[static_cast<size_t>(i + 1)]);
            if (closed)
                join(ids.back(), ids.front());
            else
                ends.insert(ends.end(), { ids.front(), ids.back() });
        }

        /** A line that stops just short of another shape (the ring, the cherry, the cap) is joined to it. */
        void tieLooseEnds()
        {
            for (const int end : ends)
            {
                int links = 0;
                for (const auto& line : out.lines)
                    links += (line.first == end || line.second == end) ? 1 : 0;
                if (links != 1)
                    continue;
                int nearest = -1;
                float best = c_reach;
                for (int i = 0; i < static_cast<int>(out.stars.size()); ++i)
                {
                    const float d = out.stars[static_cast<size_t>(i)].at.getDistanceFrom(out.stars[static_cast<size_t>(end)].at);
                    bool linked = i == end;
                    for (const auto& line : out.lines)
                        linked = linked || (line.first == end && line.second == i) || (line.second == end && line.first == i);
                    if (! linked && d < best)
                    {
                        best = d;
                        nearest = i;
                    }
                }
                if (nearest >= 0)
                    join(end, nearest);
            }
        }

        /** A closed edge: simplified from its two most distant points, so the choice of where it starts does not show. */
        void addLoop(std::vector<Pt> loop, float tolerance, bool cherry)
        {
            const int n = static_cast<int>(loop.size());
            if (n < 8)
                return;
            Pt centre;
            for (const auto& p : loop)
                centre += p / static_cast<float>(n);
            int first = 0;
            for (int i = 1; i < n; ++i)
                if (loop[static_cast<size_t>(i)].getDistanceFrom(centre) > loop[static_cast<size_t>(first)].getDistanceFrom(centre))
                    first = i;
            std::rotate(loop.begin(), loop.begin() + first, loop.end());
            int opposite = 1;
            for (int i = 1; i < n; ++i)
                if (loop[static_cast<size_t>(i)].getDistanceFrom(loop[0]) > loop[static_cast<size_t>(opposite)].getDistanceFrom(loop[0]))
                    opposite = i;
            loop.push_back(loop.front());

            std::vector<Pt> points;
            for (const int i : simplify(loop, 0, opposite, tolerance))
                points.push_back(loop[static_cast<size_t>(i)]);
            const auto second = simplify(loop, opposite, n, tolerance);
            for (size_t k = 1; k + 1 < second.size(); ++k)
                points.push_back(loop[static_cast<size_t>(second[k])]);
            add(points, true, cherry);
        }

        void addRun(const std::vector<Pt>& run, float tolerance, float shortest, float heaviest = 1.0f)
        {
            if (run.size() < 2 || lengthOf(run) < shortest)
                return;
            std::vector<Pt> points;
            for (const int i : simplify(run, 0, static_cast<int>(run.size()) - 1, tolerance))
                points.push_back(run[static_cast<size_t>(i)]);
            add(points, false, false, heaviest);
        }
    };

    /** The pull ring: the one long contour that is a circle. Returns false if the artwork has none. */
    bool findRing(const juce::Path& grenade, Pt& centre, float& radius, float& outerRadius)
    {
        std::vector<std::vector<Pt>> contours;
        for (juce::Path::Iterator it(grenade); it.next();)
        {
            if (it.elementType == juce::Path::Iterator::startNewSubPath)
                contours.emplace_back();
            if (it.elementType != juce::Path::Iterator::closePath && ! contours.empty())
                contours.back().push_back({ it.x1, it.y1 });
        }

        float best = 0.09f;   // how far from round the ring may be: its wire has a thickness and a gap
        bool found = false;
        for (const auto& contour : contours)
        {
            if (contour.size() < 16)
                continue;
            Pt middle;
            for (const auto& p : contour)
                middle += p / static_cast<float>(contour.size());
            float mean = 0.0f, furthest = 0.0f;
            for (const auto& p : contour)
            {
                mean += p.getDistanceFrom(middle) / static_cast<float>(contour.size());
                furthest = std::max(furthest, p.getDistanceFrom(middle));
            }
            float spread = 0.0f;
            for (const auto& p : contour)
                spread += juce::square(p.getDistanceFrom(middle) - mean) / static_cast<float>(contour.size());
            const float roundness = std::sqrt(spread) / std::max(1.0e-6f, mean);
            if (mean > 0.07f && roundness < best)
            {
                best = roundness;
                centre = middle;
                radius = mean;
                outerRadius = furthest;
                found = true;
            }
        }
        return found;
    }
}

//==============================================================================
LogoConstellation LogoConstellation::build(const LogoOutline& logo)
{
    LogoConstellation result;
    if (! logo.isLoaded())
        return result;

    const auto size = logo.getNormalisedSize();
    Frame frame;
    frame.w = juce::roundToInt(size.x * c_pixelsPerUnit) + 2 * c_margin;
    frame.h = juce::roundToInt(size.y * c_pixelsPerUnit) + 2 * c_margin;

    const auto grenadePath = logo.createOutlinePath(false);
    const auto grenade = rasterise(grenadePath, frame);
    const auto cherry = rasterise(logo.createOutlinePath(true), frame);

    Pt ringCentre;
    float ringRadius = 0.0f, ringOuter = 0.0f;
    const bool hasRing = findRing(grenadePath, ringCentre, ringRadius, ringOuter);
    auto nearRing = [&](Pt p) { return hasRing && p.getDistanceFrom(ringCentre) < ringOuter + c_ringClear; };

    Builder builder { result };

    // The cherry first: it is in front, so its whole outline shows and everything else stops short of it.
    auto cherrySolid = shrunk(grown(cherry, c_cherryNotch * c_pixelsPerUnit), c_cherryNotch * c_pixelsPerUnit);
    const auto cherryKeepOut = grown(cherrySolid, c_cherryClear * c_pixelsPerUnit);
    {
        auto loops = traceEdges(cherrySolid, 200);
        std::sort(loops.begin(), loops.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
        if (! loops.empty())
        {
            std::vector<Pt> loop;
            for (const auto& p : loops.front())
                loop.push_back(frame.toUnits(static_cast<float>(p.x), static_cast<float>(p.y)));
            builder.addLoop(std::move(loop), c_cherryTolerance, true);
        }
    }

    // The pull ring, down the middle of its wire.
    if (hasRing)
    {
        std::vector<Pt> ring;
        for (int i = 0; i < c_ringStars; ++i)
        {
            const float angle = juce::MathConstants<float>::twoPi * (static_cast<float>(i) + 0.5f) / static_cast<float>(c_ringStars);
            ring.push_back(ringCentre + Pt(std::cos(angle), std::sin(angle)) * ringRadius);
        }
        builder.add(ring, true, false);
        for (auto& s : result.stars)
            if (! s.cherry)
                s.weight = 0.35f;
    }

    // The grenade as one solid shape: its parts joined across the white gaps between them, and the ring filled in.
    Mask solid = grenade;
    if (hasRing)
        for (int y = 0; y < solid.h; ++y)
            for (int x = 0; x < solid.w; ++x)
                if (frame.toUnits(static_cast<float>(x), static_cast<float>(y)).getDistanceFrom(ringCentre) < ringOuter)
                    solid.ref(x, y) = 1;
    solid = shrunk(grown(solid, c_closeGaps * c_pixelsPerUnit), c_closeGaps * c_pixelsPerUnit);

    // Its body is what is left when everything thin is worn away; its outer edge is drawn where nothing is in front.
    // (A small piece that survives the wearing away, like the fork where the stems meet, is not the body.)
    auto body = grown(shrunk(solid, c_thin * c_pixelsPerUnit), c_thin * c_pixelsPerUnit);
    for (const auto& pixels : traceEdges(body, 2500))
    {
        std::vector<Pt> loop;
        std::vector<uint8_t> hidden;
        for (const auto& p : pixels)
        {
            loop.push_back(frame.toUnits(static_cast<float>(p.x), static_cast<float>(p.y)));
            hidden.push_back(cherryKeepOut.at(p.x, p.y) || nearRing(loop.back()) ? 1 : 0);
        }
        const int n = static_cast<int>(loop.size());
        int start = -1;
        for (int i = 0; i < n && start < 0; ++i)
            if (hidden[static_cast<size_t>(i)] != 0)
                start = i;
        if (start < 0)
        {
            builder.addLoop(std::move(loop), c_edgeTolerance, false);
            continue;
        }
        std::vector<Pt> run;
        for (int k = 1; k <= n; ++k)
        {
            const auto i = static_cast<size_t>((start + k) % n);
            if (hidden[i] == 0)
                run.push_back(loop[i]);
            if (hidden[i] != 0 || k == n)
            {
                builder.addRun(run, c_edgeTolerance, c_shortestLine);
                run.clear();
            }
        }
    }

    // The thin parts (the two stems, the lever's tail), each as a single line down its middle; and the long white
    // gaps inside the grenade (where the cap sits on the body), the same way.
    {
        const auto bodyAndAround = grown(body, 2.0f);
        Mask thinParts(solid.w, solid.h), gaps(solid.w, solid.h);
        for (int y = 0; y < solid.h; ++y)
            for (int x = 0; x < solid.w; ++x)
            {
                if (! solid.at(x, y) || nearRing(frame.toUnits(static_cast<float>(x), static_cast<float>(y))))
                    continue;
                thinParts.ref(x, y) = bodyAndAround.at(x, y) ? 0 : 1;
                gaps.ref(x, y) = grenade.at(x, y) || cherryKeepOut.at(x, y) ? 0 : 1;
            }
        thin(thinParts);
        for (const auto& pixels : traceLines(thinParts))
        {
            std::vector<Pt> run;
            for (const auto& p : pixels)
                run.push_back(frame.toUnits(static_cast<float>(p.x), static_cast<float>(p.y)));
            builder.addRun(run, c_lineTolerance, c_shortestLine);
        }
        thin(gaps);
        for (const auto& pixels : traceLines(gaps))
        {
            std::vector<Pt> run;
            for (const auto& p : pixels)
                run.push_back(frame.toUnits(static_cast<float>(p.x), static_cast<float>(p.y)));
            builder.addRun(run, c_lineTolerance, c_longestEdge, c_innerWeight);
        }
    }

    builder.tieLooseEnds();

    return result;
}

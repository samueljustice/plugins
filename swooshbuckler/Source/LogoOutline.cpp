#include "LogoOutline.h"
#include "Theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace
{
    //==============================================================================
    // Geometry
    constexpr int   c_targetPoints     = 2500;    // shared by all contours in proportion to their length
    constexpr int   c_minContourPoints = 24;      // keeps the smallest details from turning into polygons
    constexpr float c_flattenTolerance = 1.0e-4f; // curve flattening error, relative to the logo's longer side

    //==============================================================================
    // Timeline, in seconds
    constexpr float  c_staggerSeconds     = 1.1f;  // the longest contour starts at 0, the shortest this late
    constexpr float  c_traceSeconds       = 1.35f; // trace time of the shortest contour...
    constexpr float  c_traceExtraSeconds  = 0.55f; // ...and this much more for the longest one
    constexpr float  c_headFadeInSeconds  = 0.15f; // heads fade in rather than popping on
    constexpr float  c_trailSeconds       = 0.5f;  // how long the glow lingers after a head has passed
    constexpr float  c_settleSeconds      = 1.4f;  // the comet eases from trace speed into the shimmer
    constexpr float  c_shimmerLapSeconds  = 4.5f;  // shimmer lap of the shortest contour...
    constexpr float  c_shimmerLapExtra    = 3.0f;  // ...and this much more for the longest one
    constexpr double c_cometCycleSeconds  = 12.0;  // a fresh comet re-traces every contour this often
    constexpr float  c_cometOverrun       = 0.15f; // fraction of a lap a repeat comet coasts on while fading

    //==============================================================================
    // Look
    constexpr float  c_restAlpha        = 0.40f;
    constexpr float  c_breathDepth      = 0.10f;  // +-10% of the rest alpha
    constexpr double c_breathSeconds    = 4.0;
    constexpr float  c_shimmerIntensity = 0.55f;
    constexpr float  c_shimmerTrail     = 0.06f;  // trail + lead make the ~8% bright segment
    constexpr float  c_shimmerLead      = 0.02f;
    constexpr float  c_cometIntensity   = 0.85f;
    constexpr float  c_cometLead        = 0.01f;
    constexpr double c_driftSeconds     = 11.0;
    constexpr float  c_driftWaveX       = 2.4f;   // spatial phase of the colour drift (radians per unit), so
    constexpr float  c_driftWaveY       = 1.3f;   // the tint sweeps across the logo instead of pulsing it
    constexpr float  c_grenadeDrift     = 0.55f;  // how far each outline drifts toward its second colour
    constexpr float  c_cherryDrift      = 0.6f;
    constexpr float  c_glowFloor        = 0.002f; // halo vertices dimmer than this are not drawn

    //==============================================================================
    // View
    constexpr float  c_fill           = 0.76f; // share of the viewport's tighter dimension given to the logo
    constexpr float  c_cameraDistance = 3.0f;  // far enough back that the frustum is nearly orthographic
    constexpr float  c_swayYDegrees   = 7.0f;
    constexpr float  c_swayXDegrees   = 4.0f;
    constexpr double c_swayYSeconds   = 19.0;
    constexpr double c_swayXSeconds   = 13.0;
    constexpr float  c_swayAllowance  = 1.05f; // head room for the near side growing under perspective

    struct PassStyle
    {
        float width;   // px
        float alpha;   // glow passes: alpha at full glow
        float whiten;  // how far a head pushes the colour toward its hot colour
        bool glowOnly; // only the stretches near a comet or shimmer are drawn
    };

    // Wide faint halo, inner glow, then the core line, like the visualiser's traces.
    constexpr std::array<PassStyle, 3> c_passes { { { 7.5f, 0.10f, 0.25f, true },
                                                    { 3.2f, 0.24f, 0.50f, true },
                                                    { 1.8f, 1.00f, 0.80f, false } } };

    //==============================================================================
    struct Rgb
    {
        float r = 0.0f, g = 0.0f, b = 0.0f;
    };

    Rgb toRgb(juce::Colour colour)
    {
        return { colour.getFloatRed(), colour.getFloatGreen(), colour.getFloatBlue() };
    }

    Rgb mix(const Rgb& from, const Rgb& to, float amount)
    {
        return { from.r + (to.r - from.r) * amount,
                 from.g + (to.g - from.g) * amount,
                 from.b + (to.b - from.b) * amount };
    }

    struct Palette
    {
        Rgb base, drift, hot;
        float maxDrift = 0.0f;
    };

    struct Vertex
    {
        float x = 0.0f, y = 0.0f;
        float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    };

    float smoothStep(float x)
    {
        const float t = juce::jlimit(0.0f, 1.0f, x);
        return t * t * (3.0f - 2.0f * t);
    }

    // Phase in radians of a cycle with the given period. fmod in double keeps it smooth at large times.
    float phaseOf(double time, double period)
    {
        return static_cast<float>(juce::MathConstants<double>::twoPi * std::fmod(time, period) / period);
    }

    //==============================================================================
    // A bright spot on a contour: the comet while tracing, the shimmer afterwards.
    struct Head
    {
        float position = 0.0f;  // fraction of the contour, 0..1
        float intensity = 0.0f; // 0 = no head
        float trail = 0.0f;     // glow length behind the head, as a fraction of the contour
        float lead = 0.0f;      // glow length ahead of it

        float glowAt(float fraction) const
        {
            if (intensity <= 0.0f)
                return 0.0f;

            float behind = position - fraction;
            if (behind < 0.0f)
                behind += 1.0f;

            if (behind < trail)
            {
                const float t = 1.0f - behind / trail;
                return intensity * t * t;
            }

            const float ahead = 1.0f - behind;
            if (ahead < lead)
            {
                const float t = 1.0f - ahead / lead;
                return intensity * t * t;
            }

            return 0.0f;
        }
    };

    //==============================================================================
    // 4x4 column-major matrices.
    using Mat4 = std::array<float, 16>;

    Mat4 identityMatrix()
    {
        Mat4 m {};
        m[0] = m[5] = m[10] = m[15] = 1.0f;
        return m;
    }

    Mat4 multiply(const Mat4& a, const Mat4& b)
    {
        Mat4 m {};
        for (size_t column = 0; column < 4; ++column)
        {
            for (size_t row = 0; row < 4; ++row)
            {
                float sum = 0.0f;
                for (size_t k = 0; k < 4; ++k)
                    sum += a[k * 4 + row] * b[column * 4 + k];
                m[column * 4 + row] = sum;
            }
        }
        return m;
    }

    Mat4 rotationX(float radians)
    {
        auto m = identityMatrix();
        m[5] = std::cos(radians);
        m[6] = std::sin(radians);
        m[9] = -m[6];
        m[10] = m[5];
        return m;
    }

    Mat4 rotationY(float radians)
    {
        auto m = identityMatrix();
        m[0] = std::cos(radians);
        m[8] = std::sin(radians);
        m[2] = -m[8];
        m[10] = m[0];
        return m;
    }

    Mat4 translationZ(float z)
    {
        auto m = identityMatrix();
        m[14] = z;
        return m;
    }

    // A perspective frustum from -right..right and -top..top at nearZ, out to farZ.
    Mat4 frustum(float right, float top, float nearZ, float farZ)
    {
        Mat4 m {};
        m[0] = nearZ / right;
        m[5] = nearZ / top;
        m[10] = -(farZ + nearZ) / (farZ - nearZ);
        m[11] = -1.0f;
        m[14] = -2.0f * farZ * nearZ / (farZ - nearZ);
        return m;
    }

    // A point (x, y, 0, 1) through the matrix, then the perspective divide: -1..1 across the view.
    juce::Point<float> projectToNdc(const Mat4& m, float x, float y)
    {
        const float clipX = m[0] * x + m[4] * y + m[12];
        const float clipY = m[1] * x + m[5] * y + m[13];
        const float clipW = m[3] * x + m[7] * y + m[15];
        return { clipX / clipW, clipY / clipW };
    }

    //==============================================================================
    // Sinks receive the strips emitFrame() produces. Both see identical vertices.
    struct PreviewSink
    {
        PreviewSink(juce::Graphics& graphics, juce::Rectangle<float> bounds, const Mat4& matrix)
            : m_g(graphics), m_area(bounds), m_mvp(matrix)
        {
        }

        void beginPass(const PassStyle& pass) { m_width = pass.width; }
        void beginStrip() { m_hasPrevious = false; }
        void endStrip() { m_hasPrevious = false; }

        void vertex(const Vertex& v)
        {
            // -1..1 across the area, y up.
            const auto ndc = projectToNdc(m_mvp, v.x, v.y);
            const juce::Point<float> point { m_area.getX() + (0.5f + 0.5f * ndc.x) * m_area.getWidth(),
                                             m_area.getY() + (0.5f - 0.5f * ndc.y) * m_area.getHeight() };

            // Each segment is drawn at the mean colour of its two ends.
            if (m_hasPrevious)
            {
                const float alpha = 0.5f * (m_previous.a + v.a);

                if (alpha > 0.001f)
                {
                    m_g.setColour(juce::Colour::fromFloatRGBA(0.5f * (m_previous.r + v.r),
                                                              0.5f * (m_previous.g + v.g),
                                                              0.5f * (m_previous.b + v.b),
                                                              alpha));
                    m_g.drawLine({ m_previousPoint, point }, m_width);
                }
            }

            m_previous = v;
            m_previousPoint = point;
            m_hasPrevious = true;
        }

        juce::Graphics& m_g;
        const juce::Rectangle<float> m_area;
        const Mat4 m_mvp;
        float m_width = 1.0f;
        Vertex m_previous;
        juce::Point<float> m_previousPoint;
        bool m_hasPrevious = false;
    };

    struct StatsSink
    {
        StatsSink(LogoOutline::FrameStats& frameStats, const Mat4& matrix)
            : m_stats(frameStats), m_mvp(matrix)
        {
        }

        void beginPass(const PassStyle& pass) { m_glowPass = pass.glowOnly; }
        void beginStrip() { ++m_stats.strips; }
        void endStrip() {}

        void vertex(const Vertex& v)
        {
            ++m_stats.vertices;

            if (m_glowPass)
            {
                m_stats.maxGlowAlpha = std::max(m_stats.maxGlowAlpha, v.a);
                return;
            }

            const auto ndc = projectToNdc(m_mvp, v.x, v.y);

            if (m_lineVertices++ == 0)
            {
                m_stats.minLineAlpha = m_stats.maxLineAlpha = v.a;
                m_minX = m_maxX = ndc.x;
                m_minY = m_maxY = ndc.y;
                return;
            }

            m_stats.minLineAlpha = std::min(m_stats.minLineAlpha, v.a);
            m_stats.maxLineAlpha = std::max(m_stats.maxLineAlpha, v.a);
            m_minX = std::min(m_minX, ndc.x);
            m_maxX = std::max(m_maxX, ndc.x);
            m_minY = std::min(m_minY, ndc.y);
            m_maxY = std::max(m_maxY, ndc.y);
        }

        void finish()
        {
            if (m_lineVertices > 0)
                m_stats.ndcBounds = juce::Rectangle<float>::leftTopRightBottom(m_minX, m_minY, m_maxX, m_maxY);
        }

        LogoOutline::FrameStats& m_stats;
        const Mat4 m_mvp;
        bool m_glowPass = false;
        int m_lineVertices = 0;
        float m_minX = 0.0f, m_maxX = 0.0f, m_minY = 0.0f, m_maxY = 0.0f;
    };

    //==============================================================================
    // Construction helpers (message thread only).

    struct Shape
    {
        juce::Path path; // in the parsed SVG's coordinates, transforms applied
        bool cherry = false;
    };

    struct RawContour
    {
        std::vector<juce::Point<float>> points; // flattened; closes implicitly from the last point to the first
        bool cherry = false;
        float length = 0.0f;
    };

    bool isCherryFill(const juce::FillType& fill)
    {
        // The cherry is the logo's only red fill; everything else (black) belongs to the grenade.
        if (!fill.isColour())
            return false;

        const auto colour = fill.colour;
        const float hue = colour.getHue();
        return (hue < 0.06f || hue > 0.94f) && colour.getSaturation() > 0.45f && colour.getBrightness() > 0.3f;
    }

    void collectShapes(const juce::Drawable& drawable, const juce::AffineTransform& parentTransform, std::vector<Shape>& shapes)
    {
        // Drawable coordinates only differ between levels by each drawable's transform (as in getOutlineAsPath()).
        const auto transform = drawable.getTransform().followedBy(parentTransform);

        if (const auto* drawablePath = dynamic_cast<const juce::DrawablePath*>(&drawable))
        {
            const auto& fill = drawablePath->getFill();

            if (!fill.isInvisible())
            {
                Shape shape { drawablePath->getPath(), isCherryFill(fill) };
                shape.path.applyTransform(transform);
                shapes.push_back(std::move(shape));
            }
        }

        for (const auto* child : drawable.getChildren())
            if (const auto* childDrawable = dynamic_cast<const juce::Drawable*>(child))
                collectShapes(*childDrawable, transform, shapes);
    }

    // One path per subpath, so each can be flattened into its own closed contour.
    std::vector<juce::Path> splitSubPaths(const juce::Path& path)
    {
        std::vector<juce::Path> subPaths;

        const auto current = [&subPaths]() -> juce::Path&
        {
            if (subPaths.empty())
            {
                subPaths.emplace_back();
                subPaths.back().startNewSubPath(0.0f, 0.0f);
            }
            return subPaths.back();
        };

        juce::Path::Iterator it(path);

        while (it.next())
        {
            switch (it.elementType)
            {
                case juce::Path::Iterator::startNewSubPath:
                    subPaths.emplace_back();
                    subPaths.back().startNewSubPath(it.x1, it.y1);
                    break;
                case juce::Path::Iterator::lineTo:
                    current().lineTo(it.x1, it.y1);
                    break;
                case juce::Path::Iterator::quadraticTo:
                    current().quadraticTo(it.x1, it.y1, it.x2, it.y2);
                    break;
                case juce::Path::Iterator::cubicTo:
                    current().cubicTo(it.x1, it.y1, it.x2, it.y2, it.x3, it.y3);
                    break;
                case juce::Path::Iterator::closePath:
                    current().closeSubPath();
                    break;
            }
        }

        return subPaths;
    }

    std::vector<juce::Point<float>> flatten(const juce::Path& subPath, float tolerance)
    {
        std::vector<juce::Point<float>> points;
        const float minStepSquared = juce::square(tolerance * 0.05f);

        juce::PathFlatteningIterator it(subPath, juce::AffineTransform(), tolerance);

        while (it.next())
        {
            if (points.empty())
                points.emplace_back(it.x1, it.y1);

            const juce::Point<float> point { it.x2, it.y2 };

            if (point.getDistanceSquaredFrom(points.back()) > minStepSquared)
                points.push_back(point);
        }

        // Fills are closed loops: drop an end point that merely repeats the start.
        while (points.size() > 1 && points.back().getDistanceSquaredFrom(points.front()) <= minStepSquared)
            points.pop_back();

        return points;
    }

    float closedLength(const std::vector<juce::Point<float>>& points)
    {
        double length = 0.0;
        for (size_t i = 0; i < points.size(); ++i)
            length += static_cast<double>(points[i].getDistanceFrom(points[(i + 1) % points.size()]));
        return static_cast<float>(length);
    }

    // Appends count points spaced evenly by arc length around a closed polyline, starting at its first point.
    void resampleClosed(const std::vector<juce::Point<float>>& source, float length, int count,
                        std::vector<juce::Point<float>>& out)
    {
        const double step = static_cast<double>(length) / static_cast<double>(count);
        const size_t numSource = source.size();
        double segmentStart = 0.0;
        int placed = 0;

        for (size_t i = 0; i < numSource && placed < count; ++i)
        {
            const auto a = source[i];
            const auto b = source[(i + 1) % numSource];
            const double segmentLength = static_cast<double>(a.getDistanceFrom(b));

            while (placed < count)
            {
                const double target = step * static_cast<double>(placed);

                if (target > segmentStart + segmentLength)
                    break;

                const double u = segmentLength > 0.0 ? (target - segmentStart) / segmentLength : 0.0;
                out.push_back(a + (b - a) * static_cast<float>(u));
                ++placed;
            }

            segmentStart += segmentLength;
        }

        // Only reachable through rounding at the very end of the loop.
        for (; placed < count; ++placed)
            out.push_back(source.back());
    }
}

//==============================================================================
struct LogoOutline::FrameState
{
    double time = 0.0;
    float opacity = 0.0f;
    float lineAlpha = c_restAlpha; // rest alpha of the outline this frame, breathing included
    float driftSin = 0.0f;         // temporal phase of the colour drift
    float driftCos = 1.0f;
    std::array<Palette, 2> palettes {}; // [0] grenade, [1] cherry
    Mat4 projection {};
    Mat4 view {};
};

struct LogoOutline::ContourState
{
    float revealed = 0.0f;      // fraction of the contour drawn so far
    std::array<Head, 2> heads {}; // [0] the trace comet, which settles into the shimmer; [1] a repeat comet

    bool hasGlow() const { return heads[0].intensity > 0.0f || heads[1].intensity > 0.0f; }

    float glowAt(float fraction) const
    {
        // Screen blend: overlapping heads brighten each other without exceeding 1.
        const float first = heads[0].glowAt(fraction);
        const float second = heads[1].glowAt(fraction);
        return 1.0f - (1.0f - first) * (1.0f - second);
    }
};

//==============================================================================
LogoOutline::LogoOutline(const void* svgData, size_t svgSize)
{
    if (svgData == nullptr || svgSize == 0 || svgSize > static_cast<size_t>(std::numeric_limits<int>::max()))
        return;

    const auto xml = juce::parseXML(juce::String::createStringFromData(svgData, static_cast<int>(svgSize)));

    if (xml == nullptr)
        return;

    const auto drawable = juce::Drawable::createFromSVG(*xml);

    if (drawable == nullptr)
        return;

    std::vector<Shape> shapes;
    collectShapes(*drawable, juce::AffineTransform(), shapes);

    juce::Rectangle<float> shapeBounds;
    for (const auto& shape : shapes)
        shapeBounds = shapeBounds.getUnion(shape.path.getBounds());

    const float shapeSize = std::max(shapeBounds.getWidth(), shapeBounds.getHeight());

    if (!(shapeSize > 0.0f))
        return;

    // Flatten every subpath finely; resampling below evens out the point spacing.
    std::vector<RawContour> raw;
    for (const auto& shape : shapes)
    {
        for (const auto& subPath : splitSubPaths(shape.path))
        {
            auto points = flatten(subPath, shapeSize * c_flattenTolerance);

            if (points.size() >= 3)
                raw.push_back({ std::move(points), shape.cherry, 0.0f });
        }
    }

    if (raw.empty())
        return;

    // Normalise into a centred box whose longer side is 1, y up.
    float minX = std::numeric_limits<float>::max(), minY = minX;
    float maxX = std::numeric_limits<float>::lowest(), maxY = maxX;

    for (const auto& contour : raw)
    {
        for (const auto& p : contour.points)
        {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
    }

    m_sourceBounds = juce::Rectangle<float>::leftTopRightBottom(minX, minY, maxX, maxY);
    const float scale = 1.0f / std::max(m_sourceBounds.getWidth(), m_sourceBounds.getHeight());
    m_normalise = juce::AffineTransform::translation(-m_sourceBounds.getCentreX(), -m_sourceBounds.getCentreY())
                      .scaled(scale, -scale);
    m_halfWidth = 0.5f * m_sourceBounds.getWidth() * scale;
    m_halfHeight = 0.5f * m_sourceBounds.getHeight() * scale;

    double totalLength = 0.0;
    for (auto& contour : raw)
    {
        for (auto& p : contour.points)
            p = p.transformedBy(m_normalise);

        contour.length = closedLength(contour.points);
        totalLength += static_cast<double>(contour.length);
    }

    // Spend the point budget by length, so spacing is the same on every contour.
    m_points.reserve(static_cast<size_t>(c_targetPoints + c_minContourPoints * static_cast<int>(raw.size())));

    for (const auto& contour : raw)
    {
        if (contour.length <= 1.0e-5f)
            continue;

        const double share = static_cast<double>(contour.length) / totalLength;
        const int count = std::max(c_minContourPoints, static_cast<int>(std::lround(c_targetPoints * share)));

        Contour c;
        c.first = static_cast<int>(m_points.size());
        c.count = count;
        c.length = contour.length;
        c.cherry = contour.cherry;

        resampleClosed(contour.points, contour.length, count, m_points);
        m_contours.push_back(c);
        m_totalLength += contour.length;
    }

    if (m_contours.empty())
        return;

    // Longest (outer) contours start first and take a little longer, so it reads as being sketched.
    std::vector<size_t> order(m_contours.size());
    std::iota(order.begin(), order.end(), size_t { 0 });
    std::stable_sort(order.begin(), order.end(),
                     [this](size_t a, size_t b) { return m_contours[a].length > m_contours[b].length; });

    const float longest = m_contours[order.front()].length;
    const size_t numContours = order.size();

    for (size_t rank = 0; rank < numContours; ++rank)
    {
        auto& contour = m_contours[order[rank]];
        const float relativeLength = std::sqrt(contour.length / longest);

        contour.traceStart = numContours > 1
                               ? c_staggerSeconds * static_cast<float>(rank) / static_cast<float>(numContours - 1)
                               : 0.0f;
        contour.traceDuration = c_traceSeconds + c_traceExtraSeconds * relativeLength;
        contour.trailFraction = juce::jlimit(0.05f, 0.35f, c_trailSeconds / contour.traceDuration);
        contour.shimmerLap = c_shimmerLapSeconds + c_shimmerLapExtra * relativeLength;
    }

    // The colour drift is a slow wave across the logo; precompute each point's spatial phase.
    m_driftSin.reserve(m_points.size());
    m_driftCos.reserve(m_points.size());

    for (const auto& p : m_points)
    {
        const float phase = c_driftWaveX * p.x + c_driftWaveY * p.y;
        m_driftSin.push_back(std::sin(phase));
        m_driftCos.push_back(std::cos(phase));
    }

    m_loaded = true;
}

//==============================================================================
LogoOutline::FrameState LogoOutline::makeFrameState(float timeSeconds, float opacity, float viewportAspect) const
{
    FrameState frame;

    // Before the start (or a NaN) shows the first frame, which is still empty.
    frame.time = std::isfinite(timeSeconds) && timeSeconds > 0.0f ? static_cast<double>(timeSeconds) : 0.0;
    frame.opacity = opacity > 0.0f ? std::min(opacity, 1.0f) : 0.0f;
    frame.lineAlpha = c_restAlpha * (1.0f + c_breathDepth * std::sin(phaseOf(frame.time, c_breathSeconds)));

    const float drift = phaseOf(frame.time, c_driftSeconds);
    frame.driftSin = std::sin(drift);
    frame.driftCos = std::cos(drift);

    // The logo's cherry red is too dark to read as a thin line on the scope, so lift it.
    const auto cherryRed = juce::Colour(0xffcf1d35).withBrightness(1.0f).withMultipliedSaturation(0.8f);

    frame.palettes[0] = { toRgb(Theme::Colours::text),
                          toRgb(Theme::Colours::input),
                          toRgb(Theme::Colours::text.interpolatedWith(juce::Colours::white, 0.7f)),
                          c_grenadeDrift };
    frame.palettes[1] = { toRgb(cherryRed),
                          toRgb(Theme::Colours::harmonics),
                          toRgb(cherryRed.interpolatedWith(juce::Colours::white, 0.55f)),
                          c_cherryDrift };

    // Visible half extents in the logo's plane: fit the logo with margin and room for the sway.
    const float aspect = std::isfinite(viewportAspect) && viewportAspect > 1.0e-3f ? viewportAspect : 1.0f;
    const float halfHeight = std::max(m_halfHeight, m_halfWidth / aspect) * c_swayAllowance / c_fill;
    const float halfWidth = halfHeight * aspect;

    // A narrow frustum looks orthographic but lets the sway read as a slight turn in depth.
    const float nearZ = c_cameraDistance - 1.0f;
    const float farZ = c_cameraDistance + 1.0f;
    const float toNear = nearZ / c_cameraDistance;
    frame.projection = frustum(halfWidth * toNear, halfHeight * toNear, nearZ, farZ);

    const float swayY = juce::degreesToRadians(c_swayYDegrees) * std::sin(phaseOf(frame.time, c_swayYSeconds));
    const float swayX = juce::degreesToRadians(c_swayXDegrees) * std::sin(phaseOf(frame.time, c_swayXSeconds) + 0.7f);
    frame.view = multiply(translationZ(-c_cameraDistance), multiply(rotationX(swayX), rotationY(swayY)));

    return frame;
}

LogoOutline::ContourState LogoOutline::computeContourState(const Contour& contour, double time)
{
    ContourState state;
    const double local = time - static_cast<double>(contour.traceStart);

    if (local <= 0.0)
        return state;

    const double duration = static_cast<double>(contour.traceDuration);
    state.revealed = static_cast<float>(std::min(1.0, local / duration));

    auto& trace = state.heads[0];

    if (local < duration)
    {
        trace.position = static_cast<float>(local / duration);
        trace.intensity = smoothStep(static_cast<float>(local) / c_headFadeInSeconds);
        trace.trail = contour.trailFraction;
    }
    else
    {
        // Ease the head's speed from the trace speed down to the shimmer's, so the comet carries on
        // around the loop and becomes the shimmer instead of stopping and a new pulse appearing.
        // Distance is the integral of that speed: smoothstep integrates to x^3 - x^4 / 2.
        const double x = (local - duration) / static_cast<double>(c_settleSeconds);
        const double fromSpeed = 1.0 / duration;
        const double toSpeed = 1.0 / static_cast<double>(contour.shimmerLap);
        const double eased = x < 1.0 ? x * x * x - 0.5 * x * x * x * x : x - 0.5;
        const double travelled = static_cast<double>(c_settleSeconds) * (fromSpeed * x + (toSpeed - fromSpeed) * eased);
        const float blend = smoothStep(static_cast<float>(std::min(x, 1.0)));

        trace.position = static_cast<float>(travelled - std::floor(travelled));
        trace.intensity = juce::jmap(blend, 1.0f, c_shimmerIntensity);
        trace.trail = juce::jmap(blend, contour.trailFraction, c_shimmerTrail);
        trace.lead = c_shimmerLead * blend;
    }

    // Every cycle a fresh comet re-traces the contour over the lit outline, with the same stagger.
    if (local >= c_cometCycleSeconds)
    {
        const double sinceComet = std::fmod(local, c_cometCycleSeconds);
        const double lap = sinceComet / duration;

        if (lap < 1.0 + static_cast<double>(c_cometOverrun))
        {
            const float fadeOut = smoothStep((static_cast<float>(lap) - 0.8f) / (0.2f + c_cometOverrun));
            auto& comet = state.heads[1];
            comet.position = static_cast<float>(lap - std::floor(lap));
            comet.intensity = c_cometIntensity * smoothStep(static_cast<float>(sinceComet) / c_headFadeInSeconds)
                            * (1.0f - fadeOut);
            comet.trail = contour.trailFraction;
            comet.lead = c_cometLead;
        }
    }

    return state;
}

//==============================================================================
template <typename Sink>
void LogoOutline::emitFrame(const FrameState& frame, Sink& sink) const
{
    for (size_t passIndex = 0; passIndex < c_passes.size(); ++passIndex)
    {
        sink.beginPass(c_passes[passIndex]);

        for (const auto& contour : m_contours)
        {
            const auto state = computeContourState(contour, frame.time);

            if (state.revealed <= 0.0f || (c_passes[passIndex].glowOnly && !state.hasGlow()))
                continue;

            emitContour(contour, state, passIndex, frame, sink);
        }
    }
}

template <typename Sink>
void LogoOutline::emitContour(const Contour& contour, const ContourState& state, size_t passIndex,
                              const FrameState& frame, Sink& sink) const
{
    const auto& pass = c_passes[passIndex];
    const auto& palette = frame.palettes[contour.cherry ? 1 : 0];
    const int count = contour.count;
    const float step = 1.0f / static_cast<float>(count);

    // A finished contour repeats its first point to close the loop. While tracing, the walk stops at
    // the head, interpolated between two points so it glides instead of stepping.
    const bool closed = state.revealed >= 1.0f;
    const float headSamples = state.revealed * static_cast<float>(count);
    const int lastIndex = closed ? count : std::min(count - 1, static_cast<int>(headSamples));
    const float headOffset = closed ? 0.0f : headSamples - static_cast<float>(lastIndex);

    // The one style function renderPreview() and measureFrame() share.
    const auto makeVertex = [&](float px, float py, size_t pointIndex, float glow)
    {
        // sin(a + b), from the frame's temporal phase a and the point's precomputed spatial phase b.
        const float wave = 0.5f + 0.5f * (frame.driftSin * m_driftCos[pointIndex] + frame.driftCos * m_driftSin[pointIndex]);
        const auto colour = mix(mix(palette.base, palette.drift, palette.maxDrift * wave), palette.hot, glow * pass.whiten);
        const float alpha = pass.glowOnly ? pass.alpha * glow : frame.lineAlpha + (1.0f - frame.lineAlpha) * glow;
        return Vertex { px, py, colour.r, colour.g, colour.b, alpha * frame.opacity };
    };

    const auto walk = [&](auto&& visit)
    {
        for (int i = 0; i <= lastIndex; ++i)
        {
            const auto k = static_cast<size_t>(contour.first + i % count);
            visit(m_points[k].x, m_points[k].y, k, static_cast<float>(i) * step);
        }

        if (headOffset > 1.0e-4f)
        {
            const auto k = static_cast<size_t>(contour.first + lastIndex);
            const auto next = static_cast<size_t>(contour.first + (lastIndex + 1) % count);
            const auto head = m_points[k] + (m_points[next] - m_points[k]) * headOffset;
            visit(head.x, head.y, k, state.revealed);
        }
    };

    if (!pass.glowOnly)
    {
        sink.beginStrip();
        walk([&](float vx, float vy, size_t pointIndex, float fraction)
        {
            sink.vertex(makeVertex(vx, vy, pointIndex, state.glowAt(fraction)));
        });
        sink.endStrip();
        return;
    }

    // Glow passes only draw where a head glows; each run starts and ends on a dark vertex so it fades.
    struct Pending
    {
        float x = 0.0f, y = 0.0f;
        size_t pointIndex = 0;
        float glow = 0.0f;
    };

    Pending previous;
    bool hasPrevious = false;
    bool inStrip = false;

    walk([&](float vx, float vy, size_t pointIndex, float fraction)
    {
        const float glow = state.glowAt(fraction);

        if (glow > c_glowFloor)
        {
            if (!inStrip)
            {
                sink.beginStrip();
                inStrip = true;

                if (hasPrevious)
                    sink.vertex(makeVertex(previous.x, previous.y, previous.pointIndex, previous.glow));
            }

            sink.vertex(makeVertex(vx, vy, pointIndex, glow));
        }
        else if (inStrip)
        {
            sink.vertex(makeVertex(vx, vy, pointIndex, glow));
            sink.endStrip();
            inStrip = false;
        }

        previous = { vx, vy, pointIndex, glow };
        hasPrevious = true;
    });

    if (inStrip)
        sink.endStrip();
}

//==============================================================================
void LogoOutline::renderPreview(juce::Graphics& g, juce::Rectangle<float> area, float timeSeconds, float opacity) const
{
    if (!m_loaded || area.isEmpty())
        return;

    const auto frame = makeFrameState(timeSeconds, opacity, area.getWidth() / area.getHeight());

    if (frame.opacity <= 0.0f)
        return;

    PreviewSink sink(g, area, multiply(frame.projection, frame.view));
    emitFrame(frame, sink);
}

LogoOutline::FrameStats LogoOutline::measureFrame(float timeSeconds, float opacity, float viewportAspect) const
{
    FrameStats stats;

    if (!m_loaded)
        return stats;

    const auto frame = makeFrameState(timeSeconds, opacity, viewportAspect);

    double drawn = 0.0;
    for (const auto& contour : m_contours)
        drawn += static_cast<double>(computeContourState(contour, frame.time).revealed * contour.length);

    stats.revealed = static_cast<float>(drawn / static_cast<double>(m_totalLength));

    if (frame.opacity <= 0.0f)
        return stats;

    StatsSink sink(stats, multiply(frame.projection, frame.view));
    emitFrame(frame, sink);
    sink.finish();
    return stats;
}

juce::Path LogoOutline::createOutlinePath() const
{
    juce::Path path;

    for (const auto& contour : m_contours)
    {
        for (int i = 0; i < contour.count; ++i)
        {
            const auto& p = m_points[static_cast<size_t>(contour.first + i)];

            if (i == 0)
                path.startNewSubPath(p);
            else
                path.lineTo(p);
        }

        path.closeSubPath();
    }

    return path;
}

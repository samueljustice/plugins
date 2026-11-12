#include "MorphWander.h"
#include "LogoOutline.h"

#include <atomic>
#include <cmath>

namespace
{
    constexpr double c_lapT = 12.0 * juce::MathConstants<double>::pi; // the t range every closed shape is walked over
    constexpr float c_margin = 0.06f;                                 // pad margin for the wander figure
    constexpr float c_precessionPerLap = juce::MathConstants<float>::pi / 180.0f; // 1 degree per lap

    // Lorenz attractor, the classic parameters.
    constexpr double c_sigma = 10.0, c_rho = 28.0, c_beta = 8.0 / 3.0;
    constexpr double c_lorenzDt = 0.01; // time step of the recorded stretch (2400 steps, about 30 loops)

    juce::Vector3D<double> lorenzDeriv(const juce::Vector3D<double>& v)
    {
        return { c_sigma * (v.y - v.x), v.x * (c_rho - v.z) - v.y, v.x * v.y - c_beta * v.z };
    }

    juce::Vector3D<double> lorenzStep(const juce::Vector3D<double>& v, double h) // RK4
    {
        const auto k1 = lorenzDeriv(v);
        const auto k2 = lorenzDeriv(v + k1 * (h * 0.5));
        const auto k3 = lorenzDeriv(v + k2 * (h * 0.5));
        const auto k4 = lorenzDeriv(v + k3 * h);
        return v + (k1 + k2 * 2.0 + k3 * 2.0 + k4) * (h / 6.0);
    }

    struct Tables
    {
        std::array<MorphWander::Curve, MorphWander::NumShapes> curves;
        std::array<juce::Vector3D<double>, MorphWander::c_curvePoints> lorenzStates; // the 3-D states behind the Lorenz curve
        std::atomic<bool> cherriesReady { false };

        Tables()
        {
            constexpr double twoPi = juce::MathConstants<double>::twoPi;
            const int n = MorphWander::c_curvePoints;
            for (int i = 0; i < n; ++i)
            {
                const auto ii = static_cast<size_t>(i);
                const double u = static_cast<double>(i) / (n - 1);
                curves[MorphWander::FigureEight][ii] = { static_cast<float>(std::sin(twoPi * u)), static_cast<float>(std::sin(twoPi * u) * std::cos(twoPi * u)) };
                {
                    const double t = twoPi * u, r = std::cos(3.0 * t);
                    curves[MorphWander::Rose][ii] = { static_cast<float>(r * std::cos(t)), static_cast<float>(r * std::sin(t)) };
                }
                curves[MorphWander::Circle][ii] = { static_cast<float>(std::cos(twoPi * u)), static_cast<float>(std::sin(twoPi * u)) };
                curves[MorphWander::Cherries][ii] = curves[MorphWander::Circle][ii]; // until the artwork has been read
                {
                    // The butterfly curve: r = e^cos(t) - 2 cos(4t) - sin^5(t / 12), x = r sin(t), y = r cos(t), over
                    // t = 0 .. 12 pi, where it closes on itself. Its wings point up the pad.
                    const double t = c_lapT * u;
                    const double r = std::exp(std::cos(t)) - 2.0 * std::cos(4.0 * t) - std::pow(std::sin(t / 12.0), 5.0);
                    curves[MorphWander::Butterfly][ii] = { static_cast<float>(r * std::sin(t)), static_cast<float>(r * std::cos(t)) };
                }
            }
            // Lorenz: settle onto the attractor first, then record a long stretch of it.
            juce::Vector3D<double> v { 1.0, 1.0, 20.0 };
            for (int i = 0; i < 1500; ++i)
                v = lorenzStep(v, c_lorenzDt);
            for (int i = 0; i < n; ++i)
            {
                lorenzStates[static_cast<size_t>(i)] = v;
                curves[MorphWander::Lorenz][static_cast<size_t>(i)] = { static_cast<float>(v.x), static_cast<float>(v.z) };
                v = lorenzStep(v, c_lorenzDt);
            }
        }

        void readCherries()
        {
            if (cherriesReady.load())
                return;
            // The logo mark as one path, walked by arc length (y up, like the other shapes).
            LogoOutline logo(BinaryData::sjlogo_svg, static_cast<size_t>(BinaryData::sjlogo_svgSize));
            auto path = logo.createOutlinePath();
            const float length = juce::jmax(1.0e-3f, path.getLength());
            MorphWander::Curve points;
            for (int i = 0; i < MorphWander::c_curvePoints; ++i)
            {
                const auto p = path.getPointAlongPath(length * static_cast<float>(i) / static_cast<float>(MorphWander::c_curvePoints - 1));
                points[static_cast<size_t>(i)] = { p.x, p.y };
            }
            // Nothing reads the Cherries points until the flag is set (the circle stands in), and the swap happens
            // once: a wander in flight on the stand-in carries on from the same place along the new shape.
            cherries = points;
            cherriesReady.store(true);
        }

        MorphWander::Curve cherries;
    };

    Tables& tables()
    {
        static Tables t;
        return t;
    }
}

void MorphWander::prepareTables()
{
    auto& t = tables();
    if (t.cherriesReady.load())
        return;
    if (juce::MessageManager::existsAndIsCurrentThread())
        t.readCherries();
    else if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
        juce::MessageManager::callAsync([] { tables().readCherries(); });
}

const MorphWander::Curve& MorphWander::curve(int shape)
{
    auto& t = tables();
    shape = juce::jlimit(0, NumShapes - 1, shape);
    if (shape == Cherries && t.cherriesReady.load())
        return t.cherries;
    return t.curves[static_cast<size_t>(shape)];
}

juce::Rectangle<float> MorphWander::bounds(int shape, float rotation)
{
    const float c = std::cos(rotation), s = std::sin(rotation);
    float x0 = 1.0e9f, x1 = -1.0e9f, y0 = 1.0e9f, y1 = -1.0e9f;
    for (const auto& r : curve(shape))
    {
        const float x = r.x * c - r.y * s, y = r.x * s + r.y * c;
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
    }
    return { x0, y0, std::max(1.0e-3f, x1 - x0), std::max(1.0e-3f, y1 - y0) };
}

juce::Point<float> MorphWander::toPad(juce::Point<float> raw, float rotation, juce::Rectangle<float> box)
{
    const float c = std::cos(rotation), s = std::sin(rotation);
    const float x = raw.x * c - raw.y * s, y = raw.x * s + raw.y * c;
    return { juce::jlimit(0.0f, 1.0f, c_margin + (x - box.getX()) / box.getWidth() * (1.0f - 2.0f * c_margin)),
             juce::jlimit(0.0f, 1.0f, c_margin + (y - box.getY()) / box.getHeight() * (1.0f - 2.0f * c_margin)) };
}

float MorphWander::unitsPerSecond(float speed)
{
    return 0.05f * std::pow(30.0f, juce::jlimit(0.0f, 1.0f, speed));
}

juce::Point<float> MorphWander::shapeNorm(int shape, double t, juce::Rectangle<float> box) const
{
    // t runs over 0..c_lapT for every closed shape; read the points with linear interpolation.
    const auto& points = curve(shape);
    const double u = std::fmod(std::max(0.0, t), c_lapT) / c_lapT * (c_curvePoints - 1);
    const int i = juce::jlimit(0, c_curvePoints - 2, static_cast<int>(u));
    const float f = static_cast<float>(u - i);
    const auto a = points[static_cast<size_t>(i)], b = points[static_cast<size_t>(i + 1)];
    return toPad({ a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f }, m_rotation, box);
}

void MorphWander::join(int shape, float x, float y)
{
    shape = juce::jlimit(0, NumShapes - 1, shape);
    m_shape = shape;
    m_laps = 0.0;
    m_rotation = 0.0f;
    const auto box = bounds(shape, 0.0f);
    const juce::Point<float> from { x, y };
    float best = 1.0e9f;
    if (shape == Lorenz)
    {
        for (const auto& v : tables().lorenzStates)
        {
            const auto p = toPad({ static_cast<float>(v.x), static_cast<float>(v.z) }, 0.0f, box);
            const float d = p.getDistanceSquaredFrom(from);
            if (d < best) { best = d; m_lorenz = v; m_x = p.x; m_y = p.y; }
        }
        return;
    }
    const auto& points = curve(shape);
    m_t = 0.0;
    for (int i = 0; i < c_curvePoints; ++i)
    {
        const auto p = toPad(points[static_cast<size_t>(i)], 0.0f, box);
        const float d = p.getDistanceSquaredFrom(from);
        if (d < best) { best = d; m_t = c_lapT * i / (c_curvePoints - 1); m_x = p.x; m_y = p.y; }
    }
}

void MorphWander::step(float seconds, int shape, float speed)
{
    shape = juce::jlimit(0, NumShapes - 1, shape);
    if (shape != m_shape)
        join(shape, m_x, m_y);
    float remaining = unitsPerSecond(speed) * std::max(0.0f, seconds);

    if (shape == Lorenz)
    {
        // Constant speed across the pad: step the live system by distance / (speed on the pad) in small RK4 steps.
        // It is integrated, not looped, so the dot switches wings chaotically and never repeats.
        const auto box = bounds(shape, 0.0f);
        for (int i = 0; i < 64 && remaining > 1.0e-6f; ++i)
        {
            const auto d = lorenzDeriv(m_lorenz);
            const float vx = static_cast<float>(d.x) / box.getWidth(), vy = static_cast<float>(d.z) / box.getHeight();
            const float rate = std::max(1.0e-3f, std::sqrt(vx * vx + vy * vy) * (1.0f - 2.0f * c_margin));
            const double h = std::min(static_cast<double>(remaining / rate), 0.005);
            m_lorenz = lorenzStep(m_lorenz, h);
            remaining -= static_cast<float>(h) * rate;
        }
        const auto p = toPad({ static_cast<float>(m_lorenz.x), static_cast<float>(m_lorenz.z) }, 0.0f, box);
        m_x = p.x;
        m_y = p.y;
        m_rotation = 0.0f;
        return;
    }

    m_rotation = static_cast<float>((m_laps + m_t / c_lapT) * c_precessionPerLap);
    const auto box = bounds(shape, m_rotation);

    // Constant speed across the pad: advance t by distance / |dP/dt| in small steps (the curve would rush through
    // its outer loops and crawl near the cusps if t were advanced evenly).
    for (int i = 0; i < 16 && remaining > 1.0e-6f; ++i)
    {
        constexpr double h = 1.0e-3;
        const auto p0 = shapeNorm(shape, m_t, box), p1 = shapeNorm(shape, m_t + h, box);
        const float rate = std::max(1.0e-3f, p0.getDistanceFrom(p1) / static_cast<float>(h));
        const double advance = std::min(static_cast<double>(remaining / rate), 0.04);
        m_t += advance;
        remaining -= static_cast<float>(advance) * rate;
        if (m_t >= c_lapT)
        {
            m_t -= c_lapT;
            m_laps += 1.0;
        }
    }
    const auto p = shapeNorm(shape, m_t, box);
    m_x = p.x;
    m_y = p.y;
}

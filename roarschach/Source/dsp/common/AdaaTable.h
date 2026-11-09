#pragma once

#include "DspMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace roar
{
    /**
     * First-order antiderivative anti-aliasing (ADAA-1) for a static curve with no closed-form
     * antiderivative (ROAR's triode, tube-saturator §3.2).
     *
     * build() tabulates, on numPoints equally spaced nodes over [lo, hi]:
     *   y(g)   the curve,
     *   y'(g)  its slope (central difference of the curve),
     *   F(g)   the antiderivative, F(lo) = 0, integrated per interval with 5-point Gauss-Legendre.
     * Lookups are cubic Hermite: F uses (F, y) as value and slope, so it is accurate to
     * h^4 |y'''| / 384; y uses (y, y'). Outside [lo, hi] the curve is extended linearly from the
     * end value and slope, and F analytically to match (so tabulate where the curve still bends).
     *
     * ADAA-1 per channel (State is a plain struct, copy it to clone):
     *   y[n] = (F(x[n]) - F(x[n-1])) / (x[n] - x[n-1])     |dx| >= 1e-6
     *        = y((x[n] + x[n-1]) / 2)                       otherwise (tiny-delta fallback)
     * This adds half a sample of delay at the rate it runs at (a quarter of a base-rate sample
     * inside a 2x oversampler); account for it where a dry path must line up.
     *
     * build() allocates (prepare time only). The table is read-only afterwards, so one table can
     * be shared by every channel and both chains; lookups and process() never allocate.
     */
    class AdaaTable
    {
    public:
        struct State
        {
            double x1 = 0.0;
            double f1 = 0.0;
        };

        static constexpr double c_tinyDelta = 1.0e-6;

        template <typename Fn>
        void build (Fn&& curveFn, double lo, double hi, int numPoints = 4096)
        {
            numPoints = std::max (numPoints, 4);
            m_lo = lo;
            m_hi = std::max (hi, lo + 1.0e-6);
            m_step = (m_hi - m_lo) / static_cast<double> (numPoints - 1);
            m_invStep = 1.0 / m_step;
            m_y.assign (static_cast<size_t> (numPoints), 0.0);
            m_dy.assign (static_cast<size_t> (numPoints), 0.0);
            m_f.assign (static_cast<size_t> (numPoints), 0.0);

            const double h = 1.0e-5 * std::max (1.0, std::max (std::abs (m_lo), std::abs (m_hi)));
            for (int i = 0; i < numPoints; ++i)
            {
                const double g = node (i);
                m_y[static_cast<size_t> (i)] = static_cast<double> (curveFn (g));
                m_dy[static_cast<size_t> (i)] = (static_cast<double> (curveFn (g + h)) - static_cast<double> (curveFn (g - h))) / (2.0 * h);
            }

            // 5-point Gauss-Legendre on each interval.
            static constexpr std::array<double, 5> xs { -0.906179845938664, -0.538469310105683, 0.0,
                                                        0.538469310105683, 0.906179845938664 };
            static constexpr std::array<double, 5> ws { 0.236926885056189, 0.478628670499366, 0.568888888888889,
                                                        0.478628670499366, 0.236926885056189 };
            double acc = 0.0;
            m_f[0] = 0.0;
            for (int i = 1; i < numPoints; ++i)
            {
                const double a = node (i - 1);
                const double mid = a + 0.5 * m_step;
                double sum = 0.0;
                for (size_t k = 0; k < xs.size(); ++k)
                    sum += ws[k] * static_cast<double> (curveFn (mid + 0.5 * m_step * xs[k]));
                acc += 0.5 * m_step * sum;
                m_f[static_cast<size_t> (i)] = acc;
            }
        }

        bool isBuilt() const { return ! m_y.empty(); }
        double getLow() const { return m_lo; }
        double getHigh() const { return m_hi; }

        /** The tabulated curve (cubic Hermite on value and slope). */
        double curve (double g) const
        {
            if (m_y.size() < 2)
                return g;   // not built: identity
            if (! (g > m_lo))
                return m_y.front() + m_dy.front() * (g - m_lo);
            if (! (g < m_hi))
                return m_y.back() + m_dy.back() * (g - m_hi);

            const auto [i, t] = locate (g);
            return hermite (m_y[i], m_dy[i], m_y[i + 1], m_dy[i + 1], t);
        }

        /** The antiderivative, F(lo) = 0. */
        double antiderivative (double g) const
        {
            if (m_y.size() < 2)
                return 0.5 * g * g;
            if (! (g > m_lo))
            {
                const double d = g - m_lo;
                return m_f.front() + m_y.front() * d + 0.5 * m_dy.front() * d * d;
            }
            if (! (g < m_hi))
            {
                const double d = g - m_hi;
                return m_f.back() + m_y.back() * d + 0.5 * m_dy.back() * d * d;
            }

            const auto [i, t] = locate (g);
            return hermite (m_f[i], m_y[i], m_f[i + 1], m_y[i + 1], t);
        }

        /** ADAA-1 step. Non-finite input is taken as 0. */
        double process (State& s, double x) const
        {
            x = dsp::finiteOrZero (x);
            const double f = antiderivative (x);
            const double dx = x - s.x1;
            const double y = std::abs (dx) < c_tinyDelta ? curve (0.5 * (x + s.x1)) : (f - s.f1) / dx;
            s.x1 = x;
            s.f1 = f;
            return y;
        }

        /** Primes a state so the first process() after it has no step (x1 = x). */
        void resetState (State& s, double x = 0.0) const
        {
            s.x1 = x;
            s.f1 = isBuilt() ? antiderivative (x) : 0.0;
        }

    private:
        struct Location
        {
            size_t index;
            double t;
        };

        double node (int i) const { return m_lo + m_step * static_cast<double> (i); }

        Location locate (double g) const
        {
            const double pos = (g - m_lo) * m_invStep;
            const auto last = m_y.size() - 2;
            const auto i = std::min (static_cast<size_t> (pos), last);
            return { i, pos - static_cast<double> (i) };
        }

        // Cubic Hermite on [x_i, x_i + h] with values p and slopes m (per unit g).
        double hermite (double p0, double m0, double p1, double m1, double t) const
        {
            const double t2 = t * t, t3 = t2 * t;
            return (2.0 * t3 - 3.0 * t2 + 1.0) * p0 + (t3 - 2.0 * t2 + t) * m_step * m0
                 + (-2.0 * t3 + 3.0 * t2) * p1 + (t3 - t2) * m_step * m1;
        }

        double m_lo = 0.0, m_hi = 1.0, m_step = 1.0, m_invStep = 1.0;
        std::vector<double> m_y, m_dy, m_f;
    };
}

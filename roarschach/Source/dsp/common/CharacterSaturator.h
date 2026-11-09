#pragma once

#include "TptOnePole.h"

namespace roar
{
    /**
     * The shared character curves (tremolo-phaser §1.5 item 8, spec §5.8). Run it at the rate it
     * is prepared for: inside an Oversampler2x, prepare it with 2 fs.
     *
     *   Clean   y = v                                            (linear, no curve)
     *   Warm    lo = LP300 (v) (TPT one-pole), hi = v - lo
     *           y = tanh (1.5 lo + 0.1) / 1.5 - tanh (0.1) / 1.5 + hi
     *           smooth low-frequency 2nd + 3rd harmonics, the highs pass linearly
     *   Dirt    y = tanh (v + 0.05) - tanh (0.05)                (smooth broadband, odd-dominant)
     *   Grind   y = v > 0 ? 1.2 tanh (1.8 v) / 1.8 : 0.6 tanh (v / 0.6)
     *           asymmetric: strong even harmonics
     *
     * Every curve passes 0 -> 0 with unity slope (Grind 1.2 on the positive side), so drive and
     * makeup live in the caller. staticCurve() is the memoryless curve (for Warm: the response to
     * DC), which is what a bias tremolo subtracts: Sat (v g + b (1 - g)) - Sat (b (1 - g)).
     *
     * Optional ADAA-1 (closed-form antiderivatives, double precision, 1e-6 fallback) on the
     * nonlinear part; off by default because the modules run these curves 2x oversampled, and
     * ADAA adds half a sample of delay at the processing rate.
     *
     * Plain copyable value type, no allocation, NaN-safe.
     */
    class CharacterSaturator
    {
    public:
        enum class Curve { clean, warm, dirt, grind };

        static constexpr double c_warmSplitHz = 300.0;

        void prepare (double processingRate)
        {
            m_split.prepare (processingRate);
            m_split.setCutoff (c_warmSplitHz);
            reset();
        }

        void setCurve (Curve c)
        {
            if (c != m_curve)
            {
                m_curve = c;
                m_f1 = antiderivative (c, m_x1);   // keep the ADAA step consistent across a change
            }
        }
        Curve getCurve() const { return m_curve; }

        void setAdaa (bool on) { m_adaa = on; }

        void reset()
        {
            m_split.reset();
            m_x1 = 0.0;
            m_f1 = 0.0;
        }

        float process (float v)
        {
            v = dsp::finiteOrZero (v);
            switch (m_curve)
            {
                case Curve::clean:
                    return v;

                case Curve::warm:
                {
                    const float lo = m_split.processLowpass (v);
                    const float hi = v - lo;
                    return nonlinear (lo) + hi;
                }

                case Curve::dirt:
                case Curve::grind:
                default:
                    return nonlinear (v);
            }
        }

        /** The memoryless curve (Warm: its DC response). */
        static float staticCurve (Curve c, float v) { return static_cast<float> (shape (c, static_cast<double> (v))); }

        /** The nonlinear part of a curve (Warm: the low-band curve). */
        static double shape (Curve c, double u)
        {
            switch (c)
            {
                case Curve::clean: return u;
                case Curve::warm:  return (std::tanh (1.5 * u + 0.1) - std::tanh (0.1)) / 1.5;
                case Curve::dirt:  return std::tanh (u + c_dirtAsym) - std::tanh (c_dirtAsym);
                case Curve::grind:
                default:
                    return u > 0.0 ? 1.2 * std::tanh (1.8 * u) / 1.8 : 0.6 * std::tanh (u / 0.6);
            }
        }

        /** Antiderivative of shape(), 0 at u = 0. */
        static double antiderivative (Curve c, double u)
        {
            switch (c)
            {
                case Curve::clean: return 0.5 * u * u;
                case Curve::warm:
                    return (logCosh (1.5 * u + 0.1) - logCosh (0.1)) / 2.25 - std::tanh (0.1) / 1.5 * u;
                case Curve::dirt:
                    return logCosh (u + c_dirtAsym) - logCosh (c_dirtAsym) - std::tanh (c_dirtAsym) * u;
                case Curve::grind:
                default:
                    return u > 0.0 ? 1.2 / (1.8 * 1.8) * logCosh (1.8 * u) : 0.36 * logCosh (u / 0.6);
            }
        }

        static double logCosh (double x)
        {
            const double a = std::abs (x);
            return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994530942;
        }

        void copyStateFrom (const CharacterSaturator& other)
        {
            m_split.copyStateFrom (other.m_split);
            m_x1 = other.m_x1;
            m_f1 = other.m_f1;
        }

    private:
        static constexpr double c_dirtAsym = 0.05;

        float nonlinear (float u)
        {
            const double x = static_cast<double> (u);
            if (! m_adaa)
                return static_cast<float> (shape (m_curve, x));

            const double f = antiderivative (m_curve, x);
            const double dx = x - m_x1;
            const double y = std::abs (dx) < 1.0e-6 ? shape (m_curve, 0.5 * (x + m_x1)) : (f - m_f1) / dx;
            m_x1 = x;
            m_f1 = f;
            return static_cast<float> (y);
        }

        TptOnePole m_split;
        Curve m_curve = Curve::warm;
        bool m_adaa = false;
        double m_x1 = 0.0, m_f1 = 0.0;
    };
}

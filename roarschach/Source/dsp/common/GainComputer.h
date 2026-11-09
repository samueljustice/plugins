#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace roar
{
    /**
     * Static gain computers in the dB domain (channel-strip §5.1). x is the detector level in dB,
     * the result is a gain change in dB.
     *
     *   compressor (threshold T, ratio R, knee W, depth D <= 0), o = x - T, s = 1/R - 1:
     *     GR = 0                        2o < -W
     *        = s (o + W/2)^2 / (2W)     |2o| <= W
     *        = s o                      2o > W
     *     GR = max (GR, D)
     *     R = infinity (pass std::numeric_limits<double>::infinity()) is a limiter (s = -1).
     *
     *   expander (downward, ratio E >= 1, knee W, depth D <= 0), u = T - x:
     *     GR = 0 | -(E-1)(u + W/2)^2/(2W) | -(E-1) u,  GR = max (GR, D)
     *
     *   softDepth (g, D): the soft depth ceiling -|D| tanh (-g / |D|), so a curve approaches its
     *   maximum reduction without a corner (TAME).
     *
     * Plain value type (copyable). Settings in, no state.
     */
    class GainComputer
    {
    public:
        static constexpr double c_noDepth = -std::numeric_limits<double>::infinity();

        void setCompressor (double thresholdDb, double ratio, double kneeDb, double depthDb = c_noDepth)
        {
            m_threshold = thresholdDb;
            m_slope = (std::isinf (ratio) ? 0.0 : 1.0 / std::max (ratio, 1.0e-6)) - 1.0;
            m_knee = std::max (0.0, kneeDb);
            m_depth = std::min (0.0, depthDb);
        }

        /** Gain change in dB for a detector level in dB. */
        double compressorGainDb (double levelDb) const { return compress (levelDb, m_threshold, m_slope, m_knee, m_depth); }

        double getThresholdDb() const { return m_threshold; }
        double getKneeDb() const { return m_knee; }
        double getDepthDb() const { return m_depth; }

        /** One-off compressor curve; slope = 1/R - 1 (-1 for a limiter). */
        static double compress (double x, double threshold, double slope, double knee, double depth = c_noDepth)
        {
            const double o = x - threshold;
            double g;
            if (2.0 * o < -knee)
                g = 0.0;
            else if (2.0 * std::abs (o) <= knee && knee > 0.0)
            {
                const double t = o + 0.5 * knee;
                g = slope * t * t / (2.0 * knee);
            }
            else
                g = slope * o;
            return std::max (g, depth);
        }

        /** One-off downward expander; ratio E >= 1. */
        static double expand (double x, double threshold, double ratio, double knee, double depth = c_noDepth)
        {
            const double u = threshold - x;
            const double e = std::max (ratio, 1.0) - 1.0;
            double g;
            if (2.0 * u < -knee)
                g = 0.0;
            else if (2.0 * std::abs (u) <= knee && knee > 0.0)
            {
                const double t = u + 0.5 * knee;
                g = -e * t * t / (2.0 * knee);
            }
            else
                g = -e * u;
            return std::max (g, depth);
        }

        /** Soft depth ceiling: -|D| tanh (-g / |D|) for g <= 0. depth 0 gives 0. */
        static double softDepth (double gainDb, double depthDb)
        {
            const double d = std::abs (depthDb);
            if (d <= 0.0)
                return 0.0;
            return -d * std::tanh (-std::min (gainDb, 0.0) / d);
        }

    private:
        double m_threshold = 0.0;
        double m_slope = 0.0;
        double m_knee = 0.0;
        double m_depth = c_noDepth;
    };
}

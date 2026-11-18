#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <complex>

/**
 * The EQ maths, shared by the audio thread and the curve display.
 *
 * Every band is built from Andrew Simper's linear trapezoidal state-variable filter
 * (Cytomic "SvfLinearTrapOptimised2"). It is the same response as a prewarped
 * bilinear biquad, but it stays well behaved while frequency, gain and Q are being
 * swept, so automation is smooth and there is no zipper noise. Shelves with Q above
 * 0.707 get the classic resonant dip/bump on either side of the corner.
 *
 * Because each stage is an exact bilinear map of an analogue prototype, the display
 * evaluates the analogue transfer function at the prewarped frequency and gets the
 * plug-in's true digital response.
 */
namespace eq
{
    enum class Type
    {
        Bell = 0,
        LowShelf,
        HighShelf,
        LowCut12,
        LowCut24,
        LowCut48,
        HighCut12,
        HighCut24,
        HighCut48
    };

    constexpr int numTypes = 9;
    constexpr int maxStages = 4;

    inline const char* const typeNames[numTypes] = {
        "Bell", "Low Shelf", "High Shelf",
        "Low Cut 12", "Low Cut 24", "Low Cut 48",
        "High Cut 12", "High Cut 24", "High Cut 48"
    };

    inline bool isCut (Type t)     { return t >= Type::LowCut12; }
    inline bool isLowCut (Type t)  { return t >= Type::LowCut12 && t <= Type::LowCut48; }
    inline bool usesGain (Type t)  { return ! isCut (t); }

    inline int numStages (Type t)
    {
        switch (t)
        {
            case Type::LowCut24: case Type::HighCut24: return 2;
            case Type::LowCut48: case Type::HighCut48: return 4;
            default: return 1;
        }
    }

    struct StageCoeffs
    {
        double g = 0.1, k = 1.0;
        double a1 = 0.0, a2 = 0.0, a3 = 0.0;
        double m0 = 1.0, m1 = 0.0, m2 = 0.0;
    };

    struct BandCoeffs
    {
        int stages = 1;
        std::array<StageCoeffs, maxStages> s {};
    };

    struct StageState
    {
        double ic1 = 0.0, ic2 = 0.0;
    };

    inline void finish (StageCoeffs& c)
    {
        c.a1 = 1.0 / (1.0 + c.g * (c.g + c.k));
        c.a2 = c.g * c.a1;
        c.a3 = c.g * c.a2;
    }

    inline BandCoeffs design (Type type, double freq, double gainDb, double q, double sampleRate)
    {
        constexpr double pi = 3.14159265358979323846;

        BandCoeffs c;
        c.stages = numStages (type);

        freq = std::clamp (freq, 10.0, sampleRate * 0.49);
        q    = std::clamp (q, 0.025, 40.0);

        const double w = std::tan (pi * freq / sampleRate);
        const double A = std::pow (10.0, gainDb / 40.0);

        auto& s = c.s[0];

        switch (type)
        {
            case Type::Bell:
                s.g  = w;
                s.k  = 1.0 / (q * A);
                s.m0 = 1.0;
                s.m1 = s.k * (A * A - 1.0);
                s.m2 = 0.0;
                finish (s);
                break;

            case Type::LowShelf:
                s.g  = w / std::sqrt (A);
                s.k  = 1.0 / q;
                s.m0 = 1.0;
                s.m1 = s.k * (A - 1.0);
                s.m2 = A * A - 1.0;
                finish (s);
                break;

            case Type::HighShelf:
                s.g  = w * std::sqrt (A);
                s.k  = 1.0 / q;
                s.m0 = A * A;
                s.m1 = s.k * (1.0 - A) * A;
                s.m2 = 1.0 - A * A;
                finish (s);
                break;

            default:
            {
                // Butterworth cascades; the user's Q scales the sharpest stage, so Q above
                // 0.707 adds a resonant peak at the corner, below it rounds the knee off.
                static constexpr double bw12[] = { 0.70710678 };
                static constexpr double bw24[] = { 0.54119610, 1.30656296 };
                static constexpr double bw48[] = { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };

                const double* qs = c.stages == 1 ? bw12 : (c.stages == 2 ? bw24 : bw48);
                const bool lowCut = isLowCut (type);

                for (int i = 0; i < c.stages; ++i)
                {
                    auto& st = c.s[(size_t) i];
                    double qi = qs[i];
                    if (i == c.stages - 1)
                        qi *= q / 0.70710678;

                    st.g = w;
                    st.k = 1.0 / qi;

                    if (lowCut) { st.m0 = 1.0; st.m1 = -st.k; st.m2 = -1.0; } // high-pass
                    else        { st.m0 = 0.0; st.m1 = 0.0;   st.m2 = 1.0;  } // low-pass

                    finish (st);
                }
                break;
            }
        }

        return c;
    }

    inline double tick (const StageCoeffs& c, StageState& st, double v0) noexcept
    {
        const double v3 = v0 - st.ic2;
        const double v1 = c.a1 * st.ic1 + c.a2 * v3;
        const double v2 = st.ic2 + c.a2 * st.ic1 + c.a3 * v3;
        st.ic1 = 2.0 * v1 - st.ic1;
        st.ic2 = 2.0 * v2 - st.ic2;
        return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
    }

    /** Exact digital response of a band at frequency f. */
    inline std::complex<double> response (const BandCoeffs& c, double f, double sampleRate)
    {
        constexpr double pi = 3.14159265358979323846;
        const double wd = std::tan (pi * std::min (f, sampleRate * 0.4999) / sampleRate);

        std::complex<double> h (1.0, 0.0);

        for (int i = 0; i < c.stages; ++i)
        {
            const auto& st = c.s[(size_t) i];
            const std::complex<double> s (0.0, wd / st.g);
            const auto den = s * s + st.k * s + 1.0;
            h *= st.m0 + (st.m1 * s + st.m2) / den;
        }

        return h;
    }

    inline double responseDb (const BandCoeffs& c, double f, double sampleRate)
    {
        return 20.0 * std::log10 (std::max (1.0e-9, std::abs (response (c, f, sampleRate))));
    }
}

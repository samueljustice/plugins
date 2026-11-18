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
 *
 * Bells use a matched design (Vicanek, "Matched Second Order Digital Filters", 2016): the
 * poles are the analogue poles mapped exactly, and the zeros are solved so the gain matches
 * the analogue bell at DC, at the centre frequency and at Nyquist. A plain bilinear bell
 * gets narrower and lopsided as it nears Nyquist ("cramping"); this one keeps its analogue
 * shape, so a 16 kHz air boost sounds like one. The matched biquad is converted back to SVF
 * coefficients, so smoothing, processing and drawing all stay the same.
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

    /** Converts a biquad (b0 b1 b2 / 1 a1 a2) to the equivalent SVF stage. Exact. */
    inline StageCoeffs svfFromBiquad (double b0, double b1, double b2, double a1, double a2)
    {
        StageCoeffs c;
        const double p = 1.0 + a1 + a2, m = 1.0 - a1 + a2;
        c.g = std::sqrt (std::max (1.0e-18, p / m));
        c.k = 2.0 * (1.0 - a2) / (m * c.g);

        // Undo the bilinear map: numerator n2 s^2 + n1 s + n0 in the SVF's normalised s.
        const double d = 1.0 + c.k * c.g + c.g * c.g;
        const double c0 = b0 * d, c1 = b1 * d, c2 = b2 * d;
        const double n2 = (c0 + c2 - c1) * 0.25;
        const double n0 = (c0 + c2 + c1) * 0.25 / (c.g * c.g);
        const double n1 = (c0 - c2) / (2.0 * c.g);

        c.m0 = n2;
        c.m1 = n1 - c.k * n2;
        c.m2 = n0 - n2;
        finish (c);
        return c;
    }

    /** |H|^2 of the analogue bell (normalised to f0) at frequency ratio r = f / f0. */
    inline double analogBellPower (double r, double A, double q)
    {
        const double re = 1.0 - r * r;
        const double num = re * re + (r * A / q) * (r * A / q);
        const double den = re * re + (r / (A * q)) * (r / (A * q));
        return num / den;
    }

    /** Matched bell (Vicanek 2016), as an SVF stage. */
    inline StageCoeffs matchedBell (double freq, double A, double q, double sampleRate)
    {
        constexpr double pi = 3.14159265358979323846;
        const double w0 = 2.0 * pi * freq / sampleRate;

        // Poles: the analogue poles, mapped exactly with z = e^(sT).
        const double zeta = 1.0 / (2.0 * q * A);
        double a1;
        if (zeta <= 1.0)
            a1 = -2.0 * std::exp (-zeta * w0) * std::cos (std::sqrt (1.0 - zeta * zeta) * w0);
        else
            a1 = -2.0 * std::exp (-zeta * w0) * std::cosh (std::sqrt (zeta * zeta - 1.0) * w0);
        const double a2 = std::exp (-2.0 * zeta * w0);

        // |P(e^jw)|^2 = P0 phi0 + P1 phi1 + P2 phi2 for any quadratic P.
        const double A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2);
        const double A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2);
        const double A2 = -4.0 * a2;

        const double s2 = std::sin (w0 * 0.5) * std::sin (w0 * 0.5);
        const double phi0 = 1.0 - s2, phi1 = s2, phi2 = 4.0 * phi0 * phi1;

        // Zeros: match the analogue gain at DC (1), at f0 (A^4) and at Nyquist.
        const double gDc = 1.0;
        const double gNyq = analogBellPower (sampleRate * 0.5 / freq, A, q);
        const double gPeak = A * A * A * A;

        const double B0 = gDc * A0;
        const double B1 = gNyq * A1;
        const double denW0 = A0 * phi0 + A1 * phi1 + A2 * phi2;
        const double B2 = phi2 > 1.0e-12 ? (gPeak * denW0 - B0 * phi0 - B1 * phi1) / phi2 : 0.0;

        const double sB0 = std::sqrt (std::max (0.0, B0)), sB1 = std::sqrt (std::max (0.0, B1));
        const double W = 0.5 * (sB0 + sB1);
        const double b0 = 0.5 * (W + std::sqrt (std::max (0.0, W * W + B2)));
        const double b1 = 0.5 * (sB0 - sB1);
        const double b2 = b0 != 0.0 ? -B2 / (4.0 * b0) : 0.0;

        return svfFromBiquad (b0, b1, b2, a1, a2);
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
                s = matchedBell (freq, A, q, sampleRate);
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

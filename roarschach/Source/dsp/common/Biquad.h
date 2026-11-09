#pragma once

#include "DspMath.h"

#include <cmath>
#include <complex>

namespace roar
{
    /**
     * Second-order IIR section, transposed direct form II, coefficients and state in double (so a
     * 20 Hz high-pass at 192 kHz stays exact). Designs follow the RBJ Audio EQ Cookbook; the
     * first-order designs (b2 = a2 = 0) come from the bilinear transform with prewarping.
     *
     *   Biquad hp;
     *   hp.setCoeffs (Biquad::highpass (fs, 70.0, Biquad::c_butterworthQ));
     *   y = hp.process (x);
     *
     * Butterworth-4 = two sections with Q c_bw4Q1 and c_bw4Q2. Gains in dB, Q > 0.
     * Plain copyable value type, no allocation, NaN/denormal-safe (a non-finite state resets).
     */
    class Biquad
    {
    public:
        static constexpr double c_butterworthQ = 0.70710678118654752;
        static constexpr double c_bw4Q1 = 0.54119610014619698;
        static constexpr double c_bw4Q2 = 1.30656296487637652;

        struct Coeffs
        {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;   // a0 normalised to 1

            /** Complex response at hz. */
            std::complex<double> response (double hz, double fs) const
            {
                const std::complex<double> z1 = std::polar (1.0, -dsp::c_twoPi * hz / fs);
                const std::complex<double> z2 = z1 * z1;
                return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
            }

            double magnitudeDb (double hz, double fs) const { return dsp::gainToDb (std::abs (response (hz, fs))); }
        };

        // ---- RBJ second-order designs
        static Coeffs lowpass (double fs, double hz, double q)
        {
            const auto w = prewarp (fs, hz, q);
            return normalise ((1.0 - w.cs) * 0.5, 1.0 - w.cs, (1.0 - w.cs) * 0.5, 1.0 + w.alpha, -2.0 * w.cs, 1.0 - w.alpha);
        }

        static Coeffs highpass (double fs, double hz, double q)
        {
            const auto w = prewarp (fs, hz, q);
            return normalise ((1.0 + w.cs) * 0.5, -(1.0 + w.cs), (1.0 + w.cs) * 0.5, 1.0 + w.alpha, -2.0 * w.cs, 1.0 - w.alpha);
        }

        /** Band-pass, 0 dB peak at hz. */
        static Coeffs bandpass (double fs, double hz, double q)
        {
            const auto w = prewarp (fs, hz, q);
            return normalise (w.alpha, 0.0, -w.alpha, 1.0 + w.alpha, -2.0 * w.cs, 1.0 - w.alpha);
        }

        static Coeffs notch (double fs, double hz, double q)
        {
            const auto w = prewarp (fs, hz, q);
            return normalise (1.0, -2.0 * w.cs, 1.0, 1.0 + w.alpha, -2.0 * w.cs, 1.0 - w.alpha);
        }

        static Coeffs allpass (double fs, double hz, double q)
        {
            const auto w = prewarp (fs, hz, q);
            return normalise (1.0 - w.alpha, -2.0 * w.cs, 1.0 + w.alpha, 1.0 + w.alpha, -2.0 * w.cs, 1.0 - w.alpha);
        }

        static Coeffs peaking (double fs, double hz, double q, double gainDb)
        {
            const auto w = prewarp (fs, hz, q);
            const double A = std::pow (10.0, gainDb / 40.0);
            return normalise (1.0 + w.alpha * A, -2.0 * w.cs, 1.0 - w.alpha * A,
                              1.0 + w.alpha / A, -2.0 * w.cs, 1.0 - w.alpha / A);
        }

        /** RBJ low shelf; q = c_butterworthQ is the "slope 1" shelf. */
        static Coeffs lowShelf (double fs, double hz, double q, double gainDb)
        {
            const auto w = prewarp (fs, hz, q);
            const double A = std::pow (10.0, gainDb / 40.0);
            const double s = 2.0 * std::sqrt (A) * w.alpha;
            return normalise (A * ((A + 1.0) - (A - 1.0) * w.cs + s),
                              2.0 * A * ((A - 1.0) - (A + 1.0) * w.cs),
                              A * ((A + 1.0) - (A - 1.0) * w.cs - s),
                              (A + 1.0) + (A - 1.0) * w.cs + s,
                              -2.0 * ((A - 1.0) + (A + 1.0) * w.cs),
                              (A + 1.0) + (A - 1.0) * w.cs - s);
        }

        static Coeffs highShelf (double fs, double hz, double q, double gainDb)
        {
            const auto w = prewarp (fs, hz, q);
            const double A = std::pow (10.0, gainDb / 40.0);
            const double s = 2.0 * std::sqrt (A) * w.alpha;
            return normalise (A * ((A + 1.0) + (A - 1.0) * w.cs + s),
                              -2.0 * A * ((A - 1.0) + (A + 1.0) * w.cs),
                              A * ((A + 1.0) + (A - 1.0) * w.cs - s),
                              (A + 1.0) - (A - 1.0) * w.cs + s,
                              2.0 * ((A - 1.0) - (A + 1.0) * w.cs),
                              (A + 1.0) - (A - 1.0) * w.cs - s);
        }

        // ---- First-order designs (bilinear, prewarped at hz)
        static Coeffs lowpass1 (double fs, double hz)
        {
            const double t = tanFor (fs, hz);
            return normalise (t, t, 0.0, 1.0 + t, t - 1.0, 0.0);
        }

        static Coeffs highpass1 (double fs, double hz)
        {
            const double t = tanFor (fs, hz);
            return normalise (1.0, -1.0, 0.0, 1.0 + t, t - 1.0, 0.0);
        }

        /** First-order allpass, -90 degrees at hz. */
        static Coeffs allpass1 (double fs, double hz)
        {
            const double t = tanFor (fs, hz);
            const double c = (t - 1.0) / (t + 1.0);
            return { c, 1.0, 0.0, c, 0.0 };
        }

        /** First-order low shelf: gainDb below hz, 0 dB above; hz is the geometric mid point. */
        static Coeffs lowShelf1 (double fs, double hz, double gainDb)
        {
            // H(s) = (s + w sqrt(G)) / (s + w / sqrt(G)), normalised so H(inf) = 1, H(0) = G.
            const double G = dsp::dbToGain (gainDb), r = std::sqrt (G);
            const double t = tanFor (fs, hz);
            const double zb = t * r, pb = t / r;
            return normalise (1.0 + zb, zb - 1.0, 0.0, 1.0 + pb, pb - 1.0, 0.0);
        }

        /** First-order high shelf: gainDb above hz, 0 dB below. */
        static Coeffs highShelf1 (double fs, double hz, double gainDb)
        {
            // H(s) = G (s + w / sqrt(G)) / (s + w sqrt(G)): H(0) = 1, H(inf) = G.
            const double G = dsp::dbToGain (gainDb), r = std::sqrt (G);
            const double t = tanFor (fs, hz);
            const double zb = t / r, pb = t * r;
            return normalise (G * (1.0 + zb), G * (zb - 1.0), 0.0, 1.0 + pb, pb - 1.0, 0.0);
        }

        // ---- Processing
        void setCoeffs (const Coeffs& c) { m_c = c; }
        const Coeffs& getCoeffs() const { return m_c; }

        void reset() { m_z1 = m_z2 = 0.0; }

        float process (float x) { return static_cast<float> (process (static_cast<double> (x))); }

        double process (double x)
        {
            const double y = m_c.b0 * x + m_z1;
            m_z1 = m_c.b1 * x - m_c.a1 * y + m_z2;
            m_z2 = m_c.b2 * x - m_c.a2 * y;
            if (! std::isfinite (y))
            {
                reset();
                return 0.0;
            }
            m_z1 = dsp::flushTiny (m_z1);
            m_z2 = dsp::flushTiny (m_z2);
            return y;
        }

        void process (float* io, int numSamples)
        {
            for (int i = 0; i < numSamples; ++i)
                io[i] = process (io[i]);
        }

        void copyStateFrom (const Biquad& other) { m_z1 = other.m_z1; m_z2 = other.m_z2; }

    private:
        struct Warp
        {
            double cs, alpha;
        };

        static double tanFor (double fs, double hz)
        {
            fs = dsp::safeSampleRate (fs);
            return std::tan (dsp::c_pi * std::clamp (hz, 0.01, 0.49 * fs) / fs);
        }

        static Warp prewarp (double fs, double hz, double q)
        {
            fs = dsp::safeSampleRate (fs);
            const double w0 = dsp::c_twoPi * std::clamp (hz, 0.01, 0.49 * fs) / fs;
            return { std::cos (w0), std::sin (w0) / (2.0 * std::max (q, 1.0e-3)) };
        }

        static Coeffs normalise (double b0, double b1, double b2, double a0, double a1, double a2)
        {
            const double inv = 1.0 / a0;
            return { b0 * inv, b1 * inv, b2 * inv, a1 * inv, a2 * inv };
        }

        Coeffs m_c;
        double m_z1 = 0.0, m_z2 = 0.0;
    };
}

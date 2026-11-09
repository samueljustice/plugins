#pragma once

#include <array>
#include <complex>

namespace roar
{
    /**
     * Allpass Hilbert pair, designed per sample rate: two chains of first-order allpasses whose
     * outputs I and Q have equal magnitude (exactly 1) and a phase difference of 90 degrees
     * (Q lags I) across the band, so (I, Q) is an analytic signal for single-sideband shifting:
     *   up = I cos(theta) - Q sin(theta),  down = I cos(theta) + Q sin(theta).
     *
     * Design: a minimax 90-degree prototype (poles optimised offline on a log-frequency grid for a
     * 2400:1 band, geometric centre 1) is placed by frequency at each rate. Its centre goes to the
     * geometric middle of the prewarped band 30 Hz .. min (18 kHz, 0.45 fs), and each analog pole a
     * becomes a digital allpass (c + z^-1) / (1 + c z^-1) with c = (a - 1) / (a + 1). The bilinear
     * map is exact for allpass phase, so the error at every rate is the prototype's on the warped
     * axis:
     *   <= 50 kHz: 2 x 6 sections, |error| <= 0.36 degrees (image <= -50 dB)
     *   >  50 kHz: 2 x 8 sections, |error| <= 0.05 degrees (image <= -68 dB)
     * over the whole band, measured at 44.1, 48, 88.2, 96, 176.4 and 192 kHz.
     *
     * Zero latency apart from the phase shift itself (no one-sample offset between the paths).
     * State in double. prepare() computes the coefficients (no allocation anywhere); copyable.
     */
    class HilbertPair
    {
    public:
        static constexpr int c_maxSections = 8;
        static constexpr double c_lowHz = 30.0;
        static constexpr double c_highHz = 18000.0;

        void prepare (double sampleRate);
        void reset();

        /** One sample in, the in-phase and quadrature outputs. Non-finite input is taken as 0. */
        void process (float x, float& i, float& q);

        int getNumSections() const { return m_numSections; }

        /** Designed responses (for tests). */
        std::complex<double> responseI (double hz) const;
        std::complex<double> responseQ (double hz) const;

        void copyStateFrom (const HilbertPair& other);

    private:
        struct Chain
        {
            std::array<double, c_maxSections> c {};
            std::array<double, c_maxSections> x1 {}, y1 {};
        };

        static double runChain (Chain& chain, int numSections, double x);
        std::complex<double> response (const Chain& chain, double hz) const;

        double m_fs = 48000.0;
        int m_numSections = 6;
        Chain m_i, m_q;
    };
}

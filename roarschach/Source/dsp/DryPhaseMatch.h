#pragma once

#include "CleanStage.h"
#include "FinishStage.h"

#include <array>
#include <cmath>

namespace roar
{
    /**
     * The dry signal for a partial MIX, given the phase of the wet chain's fixed filters.
     *
     * With every stage neutral the wet path is a pure delay plus CLEAN's 60 Hz high-pass and
     * FINISH's 35 Hz high-pass and 15 kHz low-pass (all 12 dB/oct, minimum phase). Their phase
     * runs far from the pure-delay dry near the corners (about -80 degrees at 14 kHz), so a mix
     * of the two would comb. Each 2nd-order Butterworth corner is matched here by a 1st-order
     * allpass at the same frequency (within about 10 degrees everywhere): the dry keeps its full
     * band and its level, only its phase follows the wet.
     *
     *   low-pass at fc:  A(z) = (c + z^-1) / (1 + c z^-1),  c = (t - 1) / (t + 1),  t = tan (pi fc / fs)
     *   high-pass at fc: -A(z) (the high-pass leads by 180 degrees at DC, 0 at Nyquist); the two
     *                    high-passes' signs cancel.
     *
     * No allocation, NaN-safe (a non-finite input sample is treated as silence and the state is
     * reset if it ever goes non-finite).
     */
    class DryPhaseMatch
    {
    public:
        void prepare (double sampleRate)
        {
            const double fs = sampleRate > 1000.0 ? sampleRate : 48000.0;
            m_c[0] = coefficient (CleanStage::c_highPassHz, fs);
            m_c[1] = coefficient (FinishStage::c_highPassHz, fs);
            m_c[2] = coefficient (FinishStage::lowPassHzFor (fs), fs);
            reset();
        }

        void reset()
        {
            m_x1.fill (0.0);
            m_y1.fill (0.0);
        }

        float process (float in)
        {
            double x = std::isfinite (in) ? static_cast<double> (in) : 0.0;

            for (size_t k = 0; k < m_c.size(); ++k)
            {
                const double y = m_c[k] * x + m_x1[k] - m_c[k] * m_y1[k];
                m_x1[k] = x;
                m_y1[k] = y;
                x = y;
            }

            // Two high-pass corners: (-1)^2 = +1.
            if (! std::isfinite (x))
            {
                reset();
                return 0.0f;
            }

            return static_cast<float> (x);
        }

    private:
        static double coefficient (double hz, double fs)
        {
            const double t = std::tan (3.141592653589793 * hz / fs);
            return (t - 1.0) / (t + 1.0);
        }

        std::array<double, 3> m_c {};
        std::array<double, 3> m_x1 {}, m_y1 {};
    };
}

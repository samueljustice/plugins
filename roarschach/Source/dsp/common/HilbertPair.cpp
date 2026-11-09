#include "HilbertPair.h"

#include "DspMath.h"

#include <cmath>

namespace roar
{
    namespace
    {
        // Normalised analog prototype poles (sorted, geometric centre 1, p[k] p[n-1-k] = 1),
        // minimax 90-degree phase difference over a 2400:1 band. Even indices go to the Q chain.
        constexpr std::array<double, 12> c_poles6 {
            0.00799126342309198, 0.028871394883111676, 0.067443848861379857, 0.14734235160670189,
            0.3175236417783337, 0.68228380158079149, 1.4653465697491888, 3.148734908530102,
            6.7857270383488482, 14.825049029883482, 34.632671274417483, 125.1263762430298
        };

        constexpr std::array<double, 16> c_poles8 {
            0.0059193407128541797, 0.019744773991990407, 0.040197169927726477, 0.074153657612975293,
            0.13305029197132728, 0.23674553492857323, 0.4202385099248126, 0.74550294970686393,
            1.3224902590468861, 2.3468125732371985, 4.1688559486953825, 7.4275251435645337,
            13.350489803422185, 24.678004835703391, 50.333268599321109, 168.07633364211463
        };
    }

    void HilbertPair::prepare (double sampleRate)
    {
        m_fs = dsp::safeSampleRate (sampleRate);
        m_numSections = m_fs > 50000.0 ? 8 : 6;

        const double highHz = std::min (c_highHz, 0.45 * m_fs);
        const double wl = std::tan (dsp::c_pi * c_lowHz / m_fs);
        const double wh = std::tan (dsp::c_pi * highHz / m_fs);
        const double centre = std::sqrt (wl * wh);

        const double* poles = m_numSections == 8 ? c_poles8.data() : c_poles6.data();
        for (int k = 0; k < m_numSections; ++k)
        {
            const double aq = centre * poles[2 * k];
            const double ai = centre * poles[2 * k + 1];
            m_q.c[static_cast<size_t> (k)] = (aq - 1.0) / (aq + 1.0);
            m_i.c[static_cast<size_t> (k)] = (ai - 1.0) / (ai + 1.0);
        }

        reset();
    }

    void HilbertPair::reset()
    {
        m_i.x1.fill (0.0);
        m_i.y1.fill (0.0);
        m_q.x1.fill (0.0);
        m_q.y1.fill (0.0);
    }

    double HilbertPair::runChain (Chain& chain, int numSections, double x)
    {
        for (int k = 0; k < numSections; ++k)
        {
            const auto s = static_cast<size_t> (k);
            const double y = chain.c[s] * x + chain.x1[s] - chain.c[s] * chain.y1[s];
            chain.x1[s] = x;
            chain.y1[s] = dsp::flushTiny (y);
            x = y;
        }
        return x;
    }

    void HilbertPair::process (float x, float& i, float& q)
    {
        const double in = static_cast<double> (dsp::finiteOrZero (x));
        const double yi = runChain (m_i, m_numSections, in);
        const double yq = runChain (m_q, m_numSections, in);

        if (! std::isfinite (yi) || ! std::isfinite (yq))
        {
            reset();
            i = q = 0.0f;
            return;
        }

        i = static_cast<float> (yi);
        q = static_cast<float> (yq);
    }

    std::complex<double> HilbertPair::response (const Chain& chain, double hz) const
    {
        const std::complex<double> z1 = std::polar (1.0, -dsp::c_twoPi * hz / m_fs);
        std::complex<double> r (1.0, 0.0);
        for (int k = 0; k < m_numSections; ++k)
        {
            const double c = chain.c[static_cast<size_t> (k)];
            r *= (c + z1) / (1.0 + c * z1);
        }
        return r;
    }

    std::complex<double> HilbertPair::responseI (double hz) const { return response (m_i, hz); }
    std::complex<double> HilbertPair::responseQ (double hz) const { return response (m_q, hz); }

    void HilbertPair::copyStateFrom (const HilbertPair& other)
    {
        m_i.x1 = other.m_i.x1;
        m_i.y1 = other.m_i.y1;
        m_q.x1 = other.m_q.x1;
        m_q.y1 = other.m_q.y1;
    }
}

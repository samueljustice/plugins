#include "Oversampler2x.h"

#include "DspMath.h"

#include <cmath>

namespace roar
{
    namespace
    {
        // Modified Bessel function of the first kind, order 0 (power series).
        double besselI0 (double x)
        {
            double sum = 1.0, term = 1.0;
            const double q = 0.25 * x * x;
            for (int k = 1; k < 64; ++k)
            {
                term *= q / (static_cast<double> (k) * static_cast<double> (k));
                sum += term;
                if (term < 1.0e-18 * sum)
                    break;
            }
            return sum;
        }
    }

    std::array<double, 24> Oversampler2x::designTaps()
    {
        constexpr int centre = (c_numTaps - 1) / 2;   // 23
        const double i0Beta = besselI0 (c_kaiserBeta);

        std::array<double, 24> taps {};
        double sum = 0.0;
        for (int k = 0; k < 24; ++k)
        {
            const int n = 2 * k;                         // odd offset from the centre
            const double offset = static_cast<double> (n - centre);
            const double arg = 0.5 * dsp::c_pi * offset;
            const double sinc = std::sin (arg) / arg;     // offset is never 0 here
            const double r = offset / static_cast<double> (centre);
            const double window = besselI0 (c_kaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
            taps[static_cast<size_t> (k)] = 0.5 * sinc * window;
            sum += taps[static_cast<size_t> (k)];
        }

        // Odd taps sum to exactly 0.5, matching the centre tap: unity DC gain, and both polyphase
        // branches have the same DC gain (no image at DC).
        for (auto& t : taps)
            t *= 0.5 / sum;

        return taps;
    }

    Oversampler2x::Oversampler2x()
    {
        const auto taps = designTaps();
        for (int k = 0; k < c_half; ++k)
            m_taps[static_cast<size_t> (k)] = static_cast<float> (taps[static_cast<size_t> (k)]);
        reset();
    }

    void Oversampler2x::reset()
    {
        m_upHistory.fill (0.0f);
        m_downHistory.fill (0.0f);
        m_oddRing.fill (0.0f);
        m_upPos = 0;
        m_downPos = 0;
        m_oddPos = 0;
    }

    void Oversampler2x::upsample (float x, float& y0, float& y1)
    {
        x = dsp::finiteOrZero (x);

        m_upPos = (m_upPos == 0 ? c_branch : m_upPos) - 1;
        m_upHistory[static_cast<size_t> (m_upPos)] = x;
        m_upHistory[static_cast<size_t> (m_upPos + c_branch)] = x;

        // h[m] = m_upHistory[m_upPos + m] = x[n - m], m = 0..23.
        const float* h = m_upHistory.data() + m_upPos;
        float acc = 0.0f;
        for (int k = 0; k < c_half; ++k)
            acc += m_taps[static_cast<size_t> (k)] * (h[k] + h[c_branch - 1 - k]);

        y0 = 2.0f * acc;
        y1 = h[11];
    }

    float Oversampler2x::downsample (float u0, float u1)
    {
        m_downPos = (m_downPos == 0 ? c_branch : m_downPos) - 1;
        m_downHistory[static_cast<size_t> (m_downPos)] = u0;
        m_downHistory[static_cast<size_t> (m_downPos + c_branch)] = u0;

        const float* h = m_downHistory.data() + m_downPos;
        float acc = 0.0f;
        for (int k = 0; k < c_half; ++k)
            acc += m_taps[static_cast<size_t> (k)] * (h[k] + h[c_branch - 1 - k]);

        m_oddPos = (m_oddPos + 1) & 15;
        m_oddRing[static_cast<size_t> (m_oddPos)] = u1;
        const float odd = m_oddRing[static_cast<size_t> ((m_oddPos - 12) & 15)];

        return acc + 0.5f * odd;
    }

    void Oversampler2x::upsample (const float* in, float* out2x, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
            upsample (in[i], out2x[2 * i], out2x[2 * i + 1]);
    }

    void Oversampler2x::downsample (const float* in2x, float* out, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
            out[i] = downsample (in2x[2 * i], in2x[2 * i + 1]);
    }

    void Oversampler2x::copyStateFrom (const Oversampler2x& other)
    {
        m_upHistory = other.m_upHistory;
        m_upPos = other.m_upPos;
        m_downHistory = other.m_downHistory;
        m_downPos = other.m_downPos;
        m_oddRing = other.m_oddRing;
        m_oddPos = other.m_oddPos;
    }
}

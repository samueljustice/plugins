#pragma once

#include <cassert>
#include <cmath>
#include <cstddef>
#include <vector>

namespace roar
{
    /**
     * Real FFT of a power-of-two size N >= 4, single precision, for the convolver.
     *
     * forward(): N real samples -> N/2 + 1 complex bins (split re / im arrays), unnormalised:
     *     X[k] = sum_n x[n] e^(-2 pi i k n / N).
     * inverse(): N/2 + 1 bins -> N real samples, the exact inverse (scaled by 1/N), so
     *     inverse(forward(x)) = x and inverse(X H) is the circular convolution of x and h.
     *
     * Implementation: one N/2-point complex radix-2 FFT (iterative, bit-reversed input order,
     * precomputed twiddles) plus the usual even/odd split. prepare() allocates (prepare time only);
     * forward() and inverse() use only the instance's own scratch, so one instance must not be
     * used from two threads at once. Plain C++, no JUCE.
     */
    class RealFft
    {
    public:
        void prepare (int size)
        {
            assert (size >= 4 && (size & (size - 1)) == 0);
            m_n = size;
            m_m = size / 2;

            int bits = 0;
            while ((1 << bits) < m_m)
                ++bits;
            m_bitrev.assign (static_cast<size_t> (m_m), 0);
            for (int i = 0; i < m_m; ++i)
            {
                int r = 0;
                for (int b = 0; b < bits; ++b)
                    r |= ((i >> b) & 1) << (bits - 1 - b);
                m_bitrev[static_cast<size_t> (i)] = r;
            }

            // Twiddles of the N/2-point complex FFT (e^(-2 pi i k / M), k < M/2) and of the split
            // (e^(-2 pi i k / N), k <= M/2), in double, rounded once.
            m_twRe.assign (static_cast<size_t> (std::max (1, m_m / 2)), 0.0f);
            m_twIm.assign (m_twRe.size(), 0.0f);
            for (int k = 0; k < m_m / 2; ++k)
            {
                const double a = -2.0 * 3.14159265358979323846 * k / m_m;
                m_twRe[static_cast<size_t> (k)] = static_cast<float> (std::cos (a));
                m_twIm[static_cast<size_t> (k)] = static_cast<float> (std::sin (a));
            }
            m_splitRe.assign (static_cast<size_t> (m_m + 1), 0.0f);
            m_splitIm.assign (static_cast<size_t> (m_m + 1), 0.0f);
            for (int k = 0; k <= m_m; ++k)
            {
                const double a = -2.0 * 3.14159265358979323846 * k / m_n;
                m_splitRe[static_cast<size_t> (k)] = static_cast<float> (std::cos (a));
                m_splitIm[static_cast<size_t> (k)] = static_cast<float> (std::sin (a));
            }
            m_zr.assign (static_cast<size_t> (m_m), 0.0f);
            m_zi.assign (static_cast<size_t> (m_m), 0.0f);
        }

        int getSize() const { return m_n; }
        int getNumBins() const { return m_m + 1; }

        /** x: N samples. re, im: N/2 + 1 bins each. */
        void forward (const float* x, float* re, float* im)
        {
            const int m = m_m;
            for (int i = 0; i < m; ++i)
            {
                const int j = m_bitrev[static_cast<size_t> (i)];
                m_zr[static_cast<size_t> (j)] = x[2 * i];
                m_zi[static_cast<size_t> (j)] = x[2 * i + 1];
            }
            complexFft (false);

            // X[k] = E[k] + W^k O[k], E = (Z[k] + conj Z[M-k]) / 2, O = (Z[k] - conj Z[M-k]) / 2i.
            const float* zr = m_zr.data();
            const float* zi = m_zi.data();
            re[0] = zr[0] + zi[0];
            im[0] = 0.0f;
            re[m] = zr[0] - zi[0];
            im[m] = 0.0f;
            for (int k = 1; k < m; ++k)
            {
                const float ar = zr[k], ai = zi[k];
                const float br = zr[m - k], bi = -zi[m - k];   // conj Z[M-k]
                const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
                // (a - b) / 2i = (ai - bi) / 2 - i (ar - br) / 2
                const float orr = 0.5f * (ai - bi), oi = -0.5f * (ar - br);
                const float wr = m_splitRe[static_cast<size_t> (k)], wi = m_splitIm[static_cast<size_t> (k)];
                re[k] = er + (wr * orr - wi * oi);
                im[k] = ei + (wr * oi + wi * orr);
            }
        }

        /** re, im: N/2 + 1 bins (bins 0 and N/2 real, as for any real signal and products of their spectra). x: N samples. */
        void inverse (const float* re, const float* im, float* x)
        {
            const int m = m_m;
            // E[k] = (X[k] + conj X[M-k]) / 2, O[k] = (X[k] - conj X[M-k]) / (2 W^k), Z = E + i O.
            for (int k = 0; k < m; ++k)
            {
                const float ar = re[k], ai = im[k];
                const float br = re[m - k], bi = -im[m - k];
                const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
                const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
                // divide by W^k: multiply by conj W^k (|W| = 1)
                const float wr = m_splitRe[static_cast<size_t> (k)], wi = -m_splitIm[static_cast<size_t> (k)];
                const float orr = dr * wr - di * wi, oi = dr * wi + di * wr;
                // Z = E + i O
                const int j = m_bitrev[static_cast<size_t> (k)];
                m_zr[static_cast<size_t> (j)] = er - oi;
                m_zi[static_cast<size_t> (j)] = ei + orr;
            }
            complexFft (true);
            const float scale = 1.0f / static_cast<float> (m);
            for (int i = 0; i < m; ++i)
            {
                x[2 * i] = m_zr[static_cast<size_t> (i)] * scale;
                x[2 * i + 1] = m_zi[static_cast<size_t> (i)] * scale;
            }
        }

    private:
        /** In-place radix-2 DIT on m_zr / m_zi (already in bit-reversed order). */
        void complexFft (bool inverse)
        {
            const int m = m_m;
            float* zr = m_zr.data();
            float* zi = m_zi.data();
            const float sign = inverse ? -1.0f : 1.0f;
            for (int len = 2; len <= m; len <<= 1)
            {
                const int half = len >> 1;
                const int step = m / len;
                for (int start = 0; start < m; start += len)
                {
                    for (int k = 0; k < half; ++k)
                    {
                        const float wr = m_twRe[static_cast<size_t> (k * step)];
                        const float wi = sign * m_twIm[static_cast<size_t> (k * step)];
                        const int a = start + k, b = a + half;
                        const float tr = wr * zr[b] - wi * zi[b];
                        const float ti = wr * zi[b] + wi * zr[b];
                        zr[b] = zr[a] - tr;
                        zi[b] = zi[a] - ti;
                        zr[a] += tr;
                        zi[a] += ti;
                    }
                }
            }
        }

        int m_n = 0, m_m = 0;
        std::vector<int> m_bitrev;
        std::vector<float> m_twRe, m_twIm, m_splitRe, m_splitIm;
        std::vector<float> m_zr, m_zi;
    };
}

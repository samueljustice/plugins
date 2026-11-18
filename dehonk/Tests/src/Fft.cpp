#include "Fft.h"

#include <cmath>
#include <numbers>
#include <utility>

namespace dh
{

std::size_t nextPow2 (std::size_t n)
{
    std::size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

std::size_t prevPow2 (std::size_t n)
{
    if (n == 0)
        return 0;
    std::size_t p = 1;
    while ((p << 1) <= n)
        p <<= 1;
    return p;
}

void fft (std::vector<std::complex<double>>& a, bool inverse)
{
    const std::size_t n = a.size();
    if (n < 2)
        return;

    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }

    const double sign = inverse ? 1.0 : -1.0;
    std::vector<std::complex<double>> tw (n / 2);
    for (std::size_t k = 0; k < n / 2; ++k)
    {
        const double ph = sign * 2.0 * std::numbers::pi * (double) k / (double) n;
        tw[k] = { std::cos (ph), std::sin (ph) };
    }

    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const std::size_t half = len / 2;
        const std::size_t stride = n / len;
        for (std::size_t i = 0; i < n; i += len)
        {
            for (std::size_t k = 0; k < half; ++k)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + half] * tw[k * stride];
                a[i + k] = u + v;
                a[i + k + half] = u - v;
            }
        }
    }
}

} // namespace dh

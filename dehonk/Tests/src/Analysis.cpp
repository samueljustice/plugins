#include "Analysis.h"
#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace dh
{

double Spectrum::bandPower (double loHz, double hiHz) const
{
    if (binHz <= 0.0 || power.empty())
        return 0.0;
    const auto first = (std::size_t) std::max (0.0, std::ceil (loHz / binHz));
    double sum = 0.0;
    for (std::size_t k = first; k < power.size() && (double) k * binHz < hiHz; ++k)
        sum += power[k];
    return sum;
}

double Spectrum::centredDb (double centreHz, double halfWidthOct) const
{
    return bandDb (centreHz * std::exp2 (-halfWidthOct), centreHz * std::exp2 (halfWidthOct));
}

Spectrum powerSpectrum (const Audio& a, std::size_t start, std::size_t end)
{
    Spectrum s;
    s.sampleRate = a.sampleRate;
    end = std::min (end, a.length());
    if (end <= start || a.numChannels() == 0)
        return s;
    const std::size_t len = end - start;
    const std::size_t target = nextPow2 ((std::size_t) (a.sampleRate * 1.25));
    std::size_t n = std::min (target, prevPow2 (len));
    if (n < 64)
        return s;

    s.binHz = a.sampleRate / (double) n;
    s.power.assign (n / 2 + 1, 0.0);

    std::vector<double> w (n);
    double sumW2 = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        w[i] = 0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * (double) i / (double) n);
        sumW2 += w[i] * w[i];
    }

    std::vector<std::size_t> starts;
    for (std::size_t p = start; p + n <= end; p += n / 2)
        starts.push_back (p);
    if (starts.back() + n < end)
        starts.push_back (end - n); // cover the tail

    std::vector<std::complex<double>> buf (n);
    for (const auto& chan : a.ch)
    {
        for (std::size_t p : starts)
        {
            for (std::size_t i = 0; i < n; ++i)
                buf[i] = { (double) chan[p + i] * w[i], 0.0 };
            fft (buf, false);
            const double scale = 1.0 / ((double) n * sumW2);
            for (std::size_t k = 0; k <= n / 2; ++k)
            {
                const double mag2 = std::norm (buf[k]);
                s.power[k] += (k == 0 || k == n / 2 ? 1.0 : 2.0) * mag2 * scale;
            }
        }
    }
    const double frames = (double) starts.size() * (double) a.numChannels();
    for (auto& p : s.power)
        p /= frames;
    return s;
}

double meanSquare (const Audio& a, std::size_t start, std::size_t end)
{
    end = std::min (end, a.length());
    if (end <= start || a.numChannels() == 0)
        return 0.0;
    double sum = 0.0;
    for (const auto& c : a.ch)
        for (std::size_t i = start; i < end; ++i)
            sum += (double) c[i] * (double) c[i];
    return sum / ((double) (end - start) * (double) a.numChannels());
}

double residualDb (const Audio& x, const Audio& ref, double g, std::size_t start, std::size_t end)
{
    end = std::min ({ end, x.length(), ref.length() });
    double num = 0.0, den = 0.0;
    for (int c = 0; c < x.numChannels(); ++c)
        for (std::size_t i = start; i < end; ++i)
        {
            const double r = g * (double) ref.ch[(std::size_t) c][i];
            const double e = (double) x.ch[(std::size_t) c][i] - r;
            num += e * e;
            den += r * r;
        }
    if (num == 0.0)
        return -400.0;
    if (den == 0.0)
        return 400.0;
    return 10.0 * std::log10 (num / den);
}

double maxAbsDiff (const Audio& x, const Audio& ref, double g, std::size_t start, std::size_t end)
{
    end = std::min ({ end, x.length(), ref.length() });
    double m = 0.0;
    for (int c = 0; c < x.numChannels(); ++c)
        for (std::size_t i = start; i < end; ++i)
            m = std::max (m, std::abs ((double) x.ch[(std::size_t) c][i] - g * (double) ref.ch[(std::size_t) c][i]));
    return m;
}

double fitSineAmplitude (const float* x, std::size_t n, double freqHz, double sampleRate)
{
    const double w = 2.0 * std::numbers::pi * freqHz / sampleRate;
    double cc = 0, ss = 0, cs = 0, xc = 0, xs = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double c = std::cos (w * (double) i), s = std::sin (w * (double) i);
        cc += c * c;
        ss += s * s;
        cs += c * s;
        xc += (double) x[i] * c;
        xs += (double) x[i] * s;
    }
    const double det = cc * ss - cs * cs;
    if (std::abs (det) < 1e-12)
        return 0.0;
    const double a = (xc * ss - xs * cs) / det;
    const double b = (xs * cc - xc * cs) / det;
    return std::hypot (a, b);
}

double peakAbs (const Audio& a, std::size_t start, std::size_t end)
{
    end = std::min (end, a.length());
    double m = 0.0;
    for (const auto& c : a.ch)
        for (std::size_t i = start; i < end; ++i)
            m = std::max (m, (double) std::abs (c[i]));
    return m;
}

bool allFinite (const Audio& a)
{
    for (const auto& c : a.ch)
        for (float v : c)
            if (! std::isfinite (v))
                return false;
    return true;
}

double percentile (std::vector<double> v, double p)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const double idx = std::clamp (p, 0.0, 1.0) * (double) (v.size() - 1);
    const auto lo = (std::size_t) std::floor (idx);
    const auto hi = std::min (lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - (double) lo);
}

} // namespace dh

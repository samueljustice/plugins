#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace roar
{
    /**
     * Offline band-limited resampling (Kaiser-windowed sinc, beta 9, 32 zero crossings each side,
     * cutoff 0.97 x the lower Nyquist), for impulse responses at prepare time. Not for the audio
     * thread: it allocates. The kernel is tabulated (1024 phases per zero crossing, linear
     * interpolation), so a 0.75 s IR resamples in a few milliseconds.
     *
     *   y = SincResampler::resample (x, n, 44100.0, 48000.0, gain);
     *
     * The output has ceil (n * out / in) samples; y[m] is x at time m / out (no delay). gain
     * scales the result (an IR keeps its response at a new rate with gain = in / out).
     */
    class SincResampler
    {
    public:
        static constexpr int c_zeroCrossings = 32;
        static constexpr int c_phases = 1024;
        static constexpr double c_beta = 9.0;
        static constexpr double c_cutoff = 0.97;

        static std::vector<float> resample (const float* x, int n, double inRate, double outRate, double gain = 1.0)
        {
            std::vector<float> y;
            if (n <= 0 || ! (inRate > 0.0) || ! (outRate > 0.0))
                return y;
            const int outLength = static_cast<int> (std::ceil (static_cast<double> (n) * outRate / inRate - 1.0e-9));
            y.assign (static_cast<size_t> (std::max (0, outLength)), 0.0f);

            if (std::abs (inRate - outRate) < 1.0e-9 * inRate)   // the same rate: a copy (scaled)
            {
                for (int i = 0; i < n; ++i)
                    y[static_cast<size_t> (i)] = static_cast<float> (x[i] * gain);
                return y;
            }

            // Cutoff in cycles per input sample; zero crossings of the kernel every 1 / (2 nu) samples.
            const double nu = 0.5 * c_cutoff * std::min (1.0, outRate / inRate);
            const double halfWidth = c_zeroCrossings / (2.0 * nu);   // input samples each side
            const auto table = kernelTable();
            const double tableScale = c_phases * 2.0 * nu;            // table index per input sample of distance

            for (int m = 0; m < outLength; ++m)
            {
                const double t = static_cast<double> (m) * inRate / outRate;
                const int lo = std::max (0, static_cast<int> (std::ceil (t - halfWidth)));
                const int hi = std::min (n - 1, static_cast<int> (std::floor (t + halfWidth)));
                double acc = 0.0;
                for (int i = lo; i <= hi; ++i)
                {
                    const double pos = std::abs (t - static_cast<double> (i)) * tableScale;
                    const int k = static_cast<int> (pos);
                    if (k + 1 >= static_cast<int> (table.size()))
                        continue;
                    const double f = pos - k;
                    const double w = table[static_cast<size_t> (k)] + f * (table[static_cast<size_t> (k + 1)] - table[static_cast<size_t> (k)]);
                    acc += static_cast<double> (x[i]) * w;
                }
                y[static_cast<size_t> (m)] = static_cast<float> (acc * 2.0 * nu * gain);
            }
            return y;
        }

    private:
        static double besselI0 (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 64; ++k)
            {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
                if (term < 1.0e-14 * sum)
                    break;
            }
            return sum;
        }

        /** sinc (z) w (z / zeroCrossings) for z = 0 .. zeroCrossings in 1 / phases steps (z in zero crossings). */
        static const std::vector<double>& kernelTable()
        {
            static const std::vector<double> table = []
            {
                std::vector<double> t (static_cast<size_t> (c_zeroCrossings * c_phases + 2), 0.0);
                const double i0Beta = besselI0 (c_beta);
                for (size_t k = 0; k < t.size(); ++k)
                {
                    const double z = static_cast<double> (k) / c_phases;
                    if (z > c_zeroCrossings)
                        break;
                    const double r = z / c_zeroCrossings;
                    const double window = besselI0 (c_beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
                    const double sinc = z == 0.0 ? 1.0 : std::sin (3.14159265358979323846 * z) / (3.14159265358979323846 * z);
                    t[k] = sinc * window;
                }
                return t;
            }();
            return table;
        }
    };
}

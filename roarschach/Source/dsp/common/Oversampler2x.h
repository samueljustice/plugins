#pragma once

#include <array>

namespace roar
{
    /**
     * 2x up/down sampler for one channel: our own linear-phase half-band FIR, polyphase.
     *
     *   Prototype: 47 taps at the 2x rate, a Kaiser-windowed sinc cut at a quarter of the 2x rate
     *   (beta c_kaiserBeta). Being half-band, every second tap is 0 apart from the centre (0.5), so
     *   each direction costs one 24-tap symmetric FIR (12 multiplies) per base-rate sample plus a
     *   pure delay:
     *
     *     up:    y[2m]   = 2 sum_k h[2k] x[m-k]            (k = 0..23)
     *            y[2m+1] = x[m-11]
     *     down:  w[m]    = sum_k h[2k] u[2m-2k] + 0.5 u[2(m-12)+1]
     *
     *   Each filter delays by 23 samples at the 2x rate, so up + down together add exactly 23
     *   base-rate samples at every sample rate (c_latency). The band edges scale with the rate.
     *   Measured, up + down: ripple 0.0002 dB from DC to 0.35 fs; -0.47 dB at 20 kHz at 48 kHz,
     *   -2.6 dB at 20 kHz at 44.1 kHz; images and aliases at least 100 dB down from 0.65 fs.
     *
     * The state is plain arrays: no allocation anywhere, trivially copyable. A module that does
     * not need its nonlinearity on a given block must still delay the signal by c_latency (run a
     * DelayLine, or run this with a linear middle), so its latency never changes.
     *
     * Usage, per sample:
     *     float a, b;
     *     os.upsample (x, a, b);          // two 2x-rate samples, in time order
     *     y = os.downsample (f (a), f (b));
     * or os.process (block, n, f) for a block in place.
     */
    class Oversampler2x
    {
    public:
        static constexpr int c_numTaps = 47;
        static constexpr int c_latency = 23;          // base-rate samples, up + down
        static constexpr double c_kaiserBeta = 10.5;

        Oversampler2x();

        /** Nothing to allocate; clears the state. Kept for the uniform block interface. */
        void prepare() { reset(); }
        void reset();

        /** One base-rate sample in, two 2x-rate samples out (y0 first). Non-finite input is taken as 0. */
        void upsample (float x, float& y0, float& y1);

        /** Two 2x-rate samples in (time order), one base-rate sample out. */
        float downsample (float u0, float u1);

        /** in[n] -> out2x[2n]. */
        void upsample (const float* in, float* out2x, int numSamples);

        /** in2x[2n] -> out[n]. */
        void downsample (const float* in2x, float* out, int numSamples);

        /** In place: up, f at the 2x rate on each sample, down. f: float (float). */
        template <typename Fn>
        void process (float* io, int numSamples, Fn&& f)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                float a, b;
                upsample (io[i], a, b);
                const float fa = f (a);
                const float fb = f (b);
                io[i] = downsample (fa, fb);
            }
        }

        void copyStateFrom (const Oversampler2x& other);

        /** The 24 non-zero odd-offset taps h[0], h[2], ..., h[46] (the centre tap is 0.5). */
        static std::array<double, 24> designTaps();

    private:
        static constexpr int c_half = 12;      // symmetric pairs in the 24-tap polyphase branch
        static constexpr int c_branch = 24;

        std::array<float, c_half> m_taps {};   // h[0], h[2], ..., h[22] (h[46 - 2k] = h[2k])

        // Up: last 24 inputs, double buffered so the FIR reads contiguously.
        std::array<float, 2 * c_branch> m_upHistory {};
        int m_upPos = 0;

        // Down: last 24 even-phase inputs (double buffered) and a 16-entry ring of odd-phase inputs.
        std::array<float, 2 * c_branch> m_downHistory {};
        int m_downPos = 0;
        std::array<float, 16> m_oddRing {};
        int m_oddPos = 0;
    };
}

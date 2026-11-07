#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

/**
 * Small, allocation-free DSP building blocks used by the whoosh engine.
 * Nothing here depends on JUCE so it can be tested and reused offline.
 */
namespace sj
{
    constexpr float c_pi = 3.14159265358979f;
    constexpr float c_twoPi = 6.28318530717959f;

    inline float clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
    inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
    inline float dbToGain(float db) { return std::exp(db * 0.115129255f); }

    /** Pade-style tanh: smooth, monotonic, cheap; exact enough for saturation. */
    inline float fastTanh(float x)
    {
        x = clamp(x, -4.97f, 4.97f);
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    //==============================================================================
    /**
     * Rubber band: a damped spring that chases its target. With damping < 1 it
     * overshoots slightly and settles, which is what makes parameter moves feel
     * elastic instead of linear. Stable for any frequency below fs / 4.
     */
    struct Spring
    {
        float value = 0.0f, velocity = 0.0f, target = 0.0f;
        float omega = 0.0f, damping = 0.75f, dt = 1.0f / 48000.0f;

        void prepare(float rateHz, float settleHz, float dampingRatio)
        {
            dt = 1.0f / rateHz;
            omega = c_twoPi * settleHz;
            damping = dampingRatio;
        }

        void snap(float v) { value = target = v; velocity = 0.0f; }

        float step()
        {
            // Semi-implicit Euler: update velocity first, then position (symplectic, no energy gain).
            const float accel = omega * omega * (target - value) - 2.0f * damping * omega * velocity;
            velocity += accel * dt;
            value += velocity * dt;
            return value;
        }
    };

    //==============================================================================
    /** xorshift32: fast, deterministic per instance. */
    struct Random
    {
        std::uint32_t state = 0x9E3779B9u;

        void seed(std::uint32_t s) { state = s != 0 ? s : 0x9E3779B9u; }

        std::uint32_t nextInt()
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        }

        float bipolar() { return static_cast<float>(static_cast<std::int32_t>(nextInt())) * (1.0f / 2147483648.0f); }
        float unipolar() { return static_cast<float>(nextInt() >> 8) * (1.0f / 16777216.0f); }
        float range(float lo, float hi) { return lo + (hi - lo) * unipolar(); }

        /** Approximately gaussian (sum of three uniforms), unit-ish variance. */
        float gauss() { return (bipolar() + bipolar() + bipolar()) * 1.0f; }
    };

    /** Smoothed random walk in -1..1 (a "hand on the fader"). rateHz sets how often it changes direction. */
    struct Drift
    {
        Random rng;
        float current = 0.0f, target = 0.0f, coeff = 0.0f;
        int countdown = 0, period = 1;

        void prepare(float sampleRate, float rateHz, std::uint32_t seed)
        {
            rng.seed(seed);
            period = std::max(1, static_cast<int>(sampleRate / std::max(0.01f, rateHz)));
            coeff = 1.0f - std::exp(-c_twoPi * rateHz * 0.7f / sampleRate);
            current = target = rng.bipolar() * 0.5f;
            countdown = 0;
        }

        float next(int samples = 1)
        {
            countdown -= samples;
            if (countdown <= 0)
            {
                target = rng.bipolar();
                countdown = period / 2 + static_cast<int>(rng.unipolar() * static_cast<float>(period));
            }
            const float c = 1.0f - std::pow(1.0f - coeff, static_cast<float>(samples));
            current += (target - current) * c;
            return current;
        }
    };

    //==============================================================================
    /**
     * Topology-preserving state variable filter (Zavalishin / Simper). Gives
     * LP, BP (unity peak) and HP from the same state; safe to modulate.
     */
    struct Svf
    {
        float g = 0.0f, k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        float ic1 = 0.0f, ic2 = 0.0f;
        float lp = 0.0f, bp = 0.0f, hp = 0.0f;

        void set(float cutoff, float q, float sampleRate)
        {
            cutoff = clamp(cutoff, 10.0f, sampleRate * 0.45f);
            g = std::tan(c_pi * cutoff / sampleRate);
            k = 1.0f / std::max(0.05f, q);
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }

        void reset() { ic1 = ic2 = lp = bp = hp = 0.0f; }

        void process(float x)
        {
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            lp = v2;
            bp = v1;
            hp = x - k * v1 - v2;
        }

        float lowpass(float x) { process(x); return lp; }
        float bandpass(float x) { process(x); return bp * k; } // unity gain at the centre frequency
        float highpass(float x) { process(x); return hp; }
    };

    /** One-pole lowpass with a cutoff in Hz. */
    struct OnePole
    {
        float a = 0.0f, z = 0.0f;
        void set(float cutoff, float sampleRate) { a = 1.0f - std::exp(-c_twoPi * clamp(cutoff, 1.0f, sampleRate * 0.49f) / sampleRate); }
        float lowpass(float x) { z += a * (x - z); return z; }
        float highpass(float x) { return x - lowpass(x); }
        void reset() { z = 0.0f; }
    };

    /** Peak/RMS-ish envelope follower with separate attack and release. */
    struct Follower
    {
        float att = 0.0f, rel = 0.0f, env = 0.0f;
        void set(float attackMs, float releaseMs, float sampleRate)
        {
            att = 1.0f - std::exp(-1.0f / (0.001f * attackMs * sampleRate));
            rel = 1.0f - std::exp(-1.0f / (0.001f * releaseMs * sampleRate));
        }
        float process(float x)
        {
            const float a = std::abs(x);
            env += (a > env ? att : rel) * (a - env);
            return env;
        }
        void reset() { env = 0.0f; }
    };

    /** DC blocker (leaky differentiator). */
    struct DcBlock
    {
        float r = 0.995f, x1 = 0.0f, y1 = 0.0f;
        void set(float cutoff, float sampleRate) { r = std::exp(-c_twoPi * cutoff / sampleRate); }
        float process(float x) { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
        void reset() { x1 = y1 = 0.0f; }
    };

    //==============================================================================
    /** Mono delay line with a power-of-two ring and 4-point Hermite interpolation. */
    struct DelayLine
    {
        std::vector<float> buffer;
        int mask = 0, write = 0;

        void prepare(int minimumLength)
        {
            int size = 16;
            while (size < minimumLength + 4)
                size <<= 1;
            buffer.assign(static_cast<size_t>(size), 0.0f);
            mask = size - 1;
            write = 0;
        }

        void reset() { std::fill(buffer.begin(), buffer.end(), 0.0f); write = 0; }
        int maxDelay() const { return mask - 4; }

        void push(float x)
        {
            buffer[static_cast<size_t>(write)] = x;
            write = (write + 1) & mask;
        }

        /**
         * Read delaySamples behind the most recent push (>= 1). The delay is split into an integer and a fraction
         * before any index maths, so the interpolation fraction keeps full precision however large the ring
         * index gets (a float position would be quantised to 1/64 sample at 2^18).
         */
        float read(double delaySamples) const
        {
            int i = 0;
            const float t = locate(delaySamples, i);
            const float xm1 = buffer[static_cast<size_t>((i - 1) & mask)];
            const float x0  = buffer[static_cast<size_t>(i & mask)];
            const float x1  = buffer[static_cast<size_t>((i + 1) & mask)];
            const float x2  = buffer[static_cast<size_t>((i + 2) & mask)];

            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return ((c3 * t + c2) * t + c1) * t + x0;
        }

        float readLinear(double delaySamples) const
        {
            int i = 0;
            const float t = locate(delaySamples, i);
            const float a = buffer[static_cast<size_t>(i & mask)];
            const float b = buffer[static_cast<size_t>((i + 1) & mask)];
            return a + (b - a) * t;
        }

    private:
        /** Position (write - 1 - delay) as integer index i (unmasked) plus fraction t in [0, 1). */
        float locate(double delaySamples, int& i) const
        {
            delaySamples = std::clamp(delaySamples, 1.0, static_cast<double>(maxDelay()));
            const int whole = static_cast<int>(delaySamples);          // delay >= 1, so truncation == floor
            const double frac = delaySamples - static_cast<double>(whole);
            if (frac <= 0.0)
            {
                i = write - 1 - whole;
                return 0.0f;
            }
            i = write - 2 - whole;                                     // pos = i + (1 - frac)
            return static_cast<float>(1.0 - frac);
        }
    };

    /** Band-limited sawtooth (PolyBLEP). */
    struct Saw
    {
        float phase = 0.0f;

        static float blep(float t, float dt)
        {
            if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
            if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
            return 0.0f;
        }

        float next(float freq, float sampleRate)
        {
            const float dt = clamp(freq / sampleRate, 0.0f, 0.45f);
            float y = 2.0f * phase - 1.0f - blep(phase, dt);
            phase += dt;
            if (phase >= 1.0f)
                phase -= 1.0f;
            return y;
        }
    };

    /** First-order allpass used by the phaser. */
    struct Allpass1
    {
        float z = 0.0f;
        float process(float x, float a)
        {
            const float y = -a * x + z;
            z = x + a * y;
            return y;
        }
    };
}

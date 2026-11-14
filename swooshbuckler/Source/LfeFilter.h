#pragma once

#include <algorithm>
#include <array>
#include <cmath>

/**
 * The filters of the LFE feed, and the one that keeps the main speakers in step with it.
 *
 *   LowPass      the LFE's own low-pass: 120 Hz, 48 dB an octave (a Linkwitz-Riley of the eighth order: two
 *                fourth-order Butterworths in a row). It is fixed: whatever feeds the LFE, nothing above about
 *                120 Hz reaches it except down that slope (-6 dB at 120 Hz, -48 dB at 240 Hz, -64 dB at 300 Hz).
 *   PhaseMatch   an all-pass with exactly the low-pass's phase at every frequency: it changes no level, only
 *                when each frequency arrives. Every main speaker goes through it in a room with an LFE.
 *
 * WHY THE MAINS GO THROUGH AN ALL-PASS. The LFE copy of a sound is low-passed and the mains' copy is not, so
 * without help the two copies would arrive with different phase: at 120 Hz the low-pass alone turns its copy
 * half a cycle round. A system that sums them (bass management sends the mains' low end and the LFE to the same
 * subwoofer) would then cancel part of the low end. A Linkwitz-Riley low-pass has the phase of an all-pass
 * (its low-pass and high-pass add up to that all-pass exactly), so giving the mains that all-pass puts the two
 * copies exactly in phase at every frequency: they always add, never cancel. What the mains lose is nothing in
 * level; their low end arrives a few milliseconds later than their top (about 7 ms at 30 Hz, under 1 ms above
 * 500 Hz), which a whoosh does not show.
 *
 * Both are built from the same state-variable sections (the bilinear transform at the same pre-warped
 * frequency), so the phase match holds in the digital filters too, not only on paper. Double precision: a
 * 120 Hz section at 192 kHz in single precision leaves a noise floor only about 80 dB down.
 *
 * No allocation; safe for the audio thread.
 */
namespace lfe
{
    constexpr double c_lowPassHz = 120.0;
    // The two sections of a fourth-order Butterworth: Q = 1 / (2 sin(pi/8)) and 1 / (2 sin(3 pi/8)).
    constexpr double c_q1 = 0.54119610014619698;
    constexpr double c_q2 = 1.30656296487637652;

    /** One state-variable section (Simper's form, the same maths as sj::Svf64). */
    struct Section
    {
        double g = 0.0, k = 1.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
        double ic1 = 0.0, ic2 = 0.0;

        void set(double hz, double q, double sampleRate)
        {
            hz = std::clamp(hz, 5.0, sampleRate * 0.45);
            g = std::tan(3.14159265358979323846 * hz / sampleRate);
            k = 1.0 / q;
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
        void reset() { ic1 = ic2 = 0.0; }

        /** v1 is the band-pass state, v2 the low-pass output. */
        void tick(double x, double& v1, double& v2)
        {
            const double v3 = x - ic2;
            v1 = a1 * ic1 + a2 * v3;
            v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;
        }
        double lowpass(double x) { double v1, v2; tick(x, v1, v2); return v2; }
        /** (s^2 - k s + 1) / (s^2 + k s + 1): low-pass + high-pass - k band-pass = x - 2 k band-pass. */
        double allpass(double x) { double v1, v2; tick(x, v1, v2); return x - 2.0 * k * v1; }
    };

    struct LowPass
    {
        std::array<Section, 4> s;
        void prepare(double sampleRate)
        {
            for (size_t i = 0; i < s.size(); ++i)
                s[i].set(c_lowPassHz, (i & 1) == 0 ? c_q1 : c_q2, sampleRate);
            reset();
        }
        void reset() { for (auto& x : s) x.reset(); }
        float process(float in)
        {
            double y = in;
            for (auto& x : s)
                y = x.lowpass(y);
            return static_cast<float>(y);
        }
    };

    struct PhaseMatch
    {
        std::array<Section, 2> s;
        void prepare(double sampleRate)
        {
            s[0].set(c_lowPassHz, c_q1, sampleRate);
            s[1].set(c_lowPassHz, c_q2, sampleRate);
            reset();
        }
        void reset() { for (auto& x : s) x.reset(); }
        float process(float in) { return static_cast<float>(s[1].allpass(s[0].allpass(in))); }
    };
}

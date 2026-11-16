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
 *                when each frequency arrives. Every main speaker goes through it while the LFE carries a copy
 *                of what the mains carry (Mixdown, or Discrete with Stowaway's Sub), and only then.
 *
 * WHY THE MAINS GO THROUGH AN ALL-PASS. The LFE copy of a sound is low-passed and the mains' copy is not, so
 * without help the two copies would arrive with different phase: at 120 Hz the low-pass alone turns its copy
 * half a cycle round. A system that sums them (bass management sends the mains' low end and the LFE to the same
 * subwoofer) would then cancel part of the low end. A Linkwitz-Riley low-pass has the phase of an all-pass
 * (its low-pass and high-pass add up to that all-pass exactly), so giving the mains that all-pass puts the two
 * copies exactly in phase at every frequency: they always add, never cancel. What the mains lose is nothing in
 * level; their low end arrives later than their top: 7 ms under 60 Hz, rising to 10.3 ms at 110 Hz, 3 ms at
 * 200 Hz, under half a millisecond above 500 Hz. That is the low-pass's own delay, which the LFE's copy has
 * whatever is done: no filter that keeps the two copies in step can delay the mains' low end by less. (An
 * all-pass of half the order, fitted to the low-pass's phase where the LFE has level, is no gentler: its delay
 * peaks at 11.5 ms at 60 Hz, and above 110 Hz the two copies drift up to 140 degrees apart, which takes 1 to
 * 1.4 dB off their sum at 120 to 150 Hz with LFE Level at Unity and 3.5 dB at 0 dB. Without any all-pass the
 * sum dips 9 dB at 70 Hz at Unity.)
 *
 * SO IT RUNS ONLY WHEN THERE IS SOMETHING TO MATCH. With the LFE silent (LFE Mode Off, a width without an LFE,
 * Discrete without Stowaway's Sub) the mains go straight through, with no delay in their low end at all.
 * Between the two the all-pass is not faded against the plain signal (half way through, the two would cancel
 * at 72 Hz and 190 Hz): its frequency is moved instead (setAmount). It is an all-pass wherever its frequency
 * is, so no level changes at any moment, and moved up to half the sample rate it has become a plain wire, to
 * the last bit. On the way the low end's delay shrinks smoothly from 7 ms to nothing.
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
        /** The same section with its frequency moved to g = tan(pi f / sampleRate): any g above 0 (the state is
            kept: the filter carries on from where it is). */
        void setG(double newG)
        {
            g = newG;
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
        double gMatch = 0.0; // tan(pi 120 / sampleRate): where it matches the low-pass
        void prepare(double sampleRate)
        {
            s[0].set(c_lowPassHz, c_q1, sampleRate);
            s[1].set(c_lowPassHz, c_q2, sampleRate);
            gMatch = s[0].g;
            reset();
        }
        /** How much of the match is in: 1 is the match itself; below it the all-pass's frequency is higher, by
            1 / amount^2, so the delay it gives the low end is amount^2 of the match's 7 ms; towards 0 the
            frequency reaches half the sample rate, where the filter is a plain wire. amount above 0 (at 0, do
            not run it at all). The state is kept. */
        void setAmount(double amount)
        {
            const double g = gMatch / (amount * amount);
            s[0].setG(g);
            s[1].setG(g);
        }
        /** Before the first sample after not running at all: the state the filter would hold had it been running
            as a plain wire (its frequency at half the sample rate) on a signal standing at `x`. Started from
            nothing instead, the filter would carry a ripple at half the sample rate (about 1% of the signal, for
            a few milliseconds) as its frequency comes down. */
        void startAsWire(float x)
        {
            for (auto& section : s)
            {
                section.ic1 = 0.0;
                section.ic2 = x;
            }
        }
        void reset() { for (auto& x : s) x.reset(); }
        float process(float in) { return static_cast<float>(s[1].allpass(s[0].allpass(in))); }
    };
}

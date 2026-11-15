#pragma once

#include "DspUtils.h"

#include <array>

/**
 * The reverb behind the Space knob: eight delay lines that feed each other through a Hadamard mix (a feedback
 * delay network). The mix loses no energy, so the tail's length is set only by each line's own gain; the lines
 * have mutually prime lengths and breathe by a few samples so no single resonance stands out, and each loses its
 * top end faster than its body, the way air and walls do.
 *
 * What goes in is high-passed (the low end stays dry and solid), softened at the top, delayed a little (the dry sound speaks first)
 * and spread over a few tens of milliseconds by three rounds of short delays and mixing, so the tail is dense from
 * its first moment. What comes out is the wet signal only, as a mid and a side tap that are uncorrelated: the mono
 * sum keeps the mid, so it never cancels.
 *
 * Everything is sized in prepare(); process() does not allocate.
 */
class SpaceReverb
{
public:
    static constexpr int c_lines = 8;
    static constexpr float c_preDelayMs = 15.0f;
    static constexpr float c_sendHighPassHz = 150.0f;
    static constexpr float c_sendLowPassHz = 12000.0f;

    /** The tail length (seconds to fall 60 dB) for a Space amount of 0..1. */
    static float decaySecondsFor(float space) { return 1.2f + 3.6f * sj::clamp(space, 0.0f, 1.0f); }

    void prepare(double sampleRate);
    void reset();

    /** Tail length in seconds (to -60 dB) for the body; the top end falls about twice as fast. */
    void setDecay(float seconds);

    /**
     * Mixes `frames` samples in place: out = dry gain x in + wet gain x reverb(in). Both gains ramp from their
     * "0" value (before the first sample) to their "1" value (at the last), so a moving knob doesn't step.
     */
    void process(float* left, float* right, int frames, float dry0, float dry1, float wet0, float wet1);

private:
    static void mix(std::array<float, c_lines>& v);

    float m_fs = 48000.0f;
    float m_decay = -1.0f;

    sj::OnePole m_hpL1, m_hpL2, m_hpR1, m_hpR2, m_lpL, m_lpR;
    sj::DelayLine m_preL, m_preR;
    int m_preDelay = 1;
    // Three rounds of "delay every channel by a different amount, flip some, mix": one click in becomes hundreds.
    static constexpr int c_diffusers = 3;
    std::array<std::array<sj::DelayLine, c_lines>, c_diffusers> m_diff;
    std::array<std::array<int, c_lines>, c_diffusers> m_diffDelay {};

    std::array<sj::DelayLine, c_lines> m_line;
    std::array<float, c_lines> m_length {};          // samples
    std::array<float, c_lines> m_gainLow {}, m_gainHigh {};
    std::array<sj::OnePole, c_lines> m_damp;
    std::array<float, c_lines> m_lfoPhase {}, m_lfoInc {}; // phase in -1..1
    float m_modDepth = 4.0f;                         // samples
};

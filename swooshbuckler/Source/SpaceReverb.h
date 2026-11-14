#pragma once

#include "DspUtils.h"

#include <array>
#include <cstdint>

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

/**
 * The same reverb for a room of speakers (Quad to 7.1.4): the Space knob's reverb with a return on every main
 * speaker, each one uncorrelated with the others.
 *
 * It is SpaceReverb's design with sixteen lines instead of eight: the same send filters and pre-delay, the same
 * three rounds of diffusion (over sixteen), the same line lengths (SpaceReverb's eight and eight more in between
 * them), the same breathing, damping and tail length for every Space setting. Sixteen because the returns must
 * be uncorrelated: each speaker reads the lines through its own row of a 16 x 16 Hadamard matrix, and rows of it
 * are at right angles, so no two speakers carry the same reverb. (Eight lines have only eight such rows; 7.1.4
 * has eleven main speakers.) Every speaker reads the lines at the same moment, so a fold-down that adds speakers
 * together only adds their power: there is no delayed copy to comb against.
 *
 *   send     each channel (a speaker of the room) is filtered and delayed as SpaceReverb's send is, then goes
 *            into the lines on its own Hadamard row (channel c on row c): the rows are at right angles, so the
 *            reverb is fed exactly the room's power, as the stereo reverb is fed L's and R's
 *   return   channel c = sum over rows r of taps[c][r] x (row r of the lines, scaled to one line's power). The
 *            caller sets the taps: which rows a speaker reads and how loud. A tap of 1 gives a speaker the power
 *            of one line; the stereo reverb's L and R together come to c_stereoTapPower of those, so taps whose
 *            squares add up to that make the room's reverb as loud, all speakers together, as the stereo one.
 *
 * The taps glide: process() moves them in a straight line from where the last call left them to the ones it is
 * given, across the call, so a speaker's share of the reverb never steps.
 *
 * Everything is sized in prepare(); process() does not allocate.
 */
class RoomReverb
{
public:
    static constexpr int c_lines = 16;
    static constexpr int c_maxChannels = 12;
    using Taps = std::array<std::array<float, c_lines>, c_maxChannels>;

    /** The stereo reverb's two returns together, in the power one row of these lines gives (each line holding
        what one of SpaceReverb's does, spread over twice as many): L and R each read mid + 0.8 side over eight
        lines, 1.64 lines' power each, and there are half as many lines there. */
    static constexpr float c_stereoTapPower = 2.0f * 1.64f * (static_cast<float>(c_lines) / static_cast<float>(SpaceReverb::c_lines));

    void prepare(double sampleRate);
    void reset();
    void setDecay(float seconds);

    /** In place on `numChannels` channels: out = dry x in + wet x the speaker's return, both gains ramped as
        SpaceReverb::process ramps them, the taps from the last call's to `taps`. */
    void process(float* const* channels, int numChannels, int frames, float dry0, float dry1, float wet0, float wet1, const Taps& taps);

    /** The taps the last process() ended on. */
    const Taps& taps() const { return m_taps; }
    /** The next process() starts from these taps instead (nothing to glide from). */
    void snapTaps(const Taps& taps) { m_taps = taps; }

private:
    static void mix(std::array<float, c_lines>& v);

    float m_fs = 48000.0f;
    float m_decay = -1.0f;

    struct Send
    {
        sj::OnePole hp1, hp2, lp;
        sj::DelayLine pre;
    };
    std::array<Send, c_maxChannels> m_send;
    int m_preDelay = 1;
    static constexpr int c_diffusers = 3;
    std::array<std::array<sj::DelayLine, c_lines>, c_diffusers> m_diff;
    std::array<std::array<int, c_lines>, c_diffusers> m_diffDelay {};
    std::array<std::array<float, c_lines>, c_diffusers> m_diffSign {};

    std::array<sj::DelayLine, c_lines> m_line;
    std::array<float, c_lines> m_length {};
    std::array<float, c_lines> m_gainLow {}, m_gainHigh {};
    std::array<sj::OnePole, c_lines> m_damp;
    std::array<float, c_lines> m_lfoPhase {}, m_lfoInc {};
    float m_modDepth = 4.0f;

    Taps m_taps {};
};

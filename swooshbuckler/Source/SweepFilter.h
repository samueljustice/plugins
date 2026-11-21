#pragma once

#include "DspUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

/**
 * A BUS'S FILTER, WITH WHAT IS BEHIND ITS WHEEL. One per bus (RigEngine::BusState::sweep), up to c_lanes lanes.
 * Header only, JUCE-free; nothing here allocates, locks or does I/O.
 *
 * The laws (static, design 3.6): octaves(), cutoffHz() with each type's place on the axis, q() and trim(). The wet
 * mix law and the tanh stay the engine's.
 *
 * The filter: one state-variable filter per lane (sj::Svf's arithmetic), a second in series with 24 dB.
 *   - Low Pass and High Pass at 12 dB: the filter's own Q. At 24 dB the first stage sits at Q 0.541 and the second
 *     at 1.848 x Q, which is a fourth-order Butterworth at the knob's lowest Q (0.7) and keeps the resonant peak
 *     at about Q, so trim() fits both slopes.
 *   - Band Pass: the band output over 0.7, so its peak is Q / 0.7 (level at the knob's lowest Q). At 24 dB a first
 *     stage at Q 0.7 goes in front: 12 dB an octave either side, the same peak.
 *   - Notch: low plus high; Q sets its width. At 24 dB two in series (a wider floor).
 *   Where the filter is (the log of its frequency) runs straight from the last chunk's target to this one's and
 *   then through two one-pole smoothers of c_smoothSamples each, and g is worked out from that every sample, so a
 *   chunk's corners are rounded off (they are what leaves spurs at the chunk rate under a fast sweep); the cost is
 *   about a chunk of lag. k runs straight. Once a target has been held long enough for the smoothers to arrive,
 *   the coefficients are exactly sj::Svf::set()'s, and a Low Pass at 12 dB is then sj::Svf::lowpass() to the bit.
 *   A change of Type or of 24 dB crosses over in c_typeSeconds, not at once.
 *
 * The rhythm (advance()): Sine, Triangle, Ramp Up, Ramp Down, Square or Random (a fresh level each cycle, from its
 * own sj::Random), -1..1. It moves at c_slewPerSecond at the most, so a square's edge, a ramp's return and a random
 * step take 4 ms and a sine or triangle at 20 Hz is not touched. reset() puts it at the top of its cycle;
 * scatter() starts it elsewhere for renders.
 *
 * What the callers rely on (RigEngine::renderBus): prepare() and setSeed() from RigEngine::prepare and its seeded
 * restarts, reset() from RigEngine::reset, scatter() from RigEngine::scatterRide; once per chunk (32 samples at
 * most): advance() (the rhythm), then setTarget() with cutoffHz() and q() for the END of this chunk, then process()
 * on each lane in use, which puts the filtered stream into `out` (the engine mixes it in with its tanh, trim() and
 * mix law), then endChunk(), which makes this chunk's target the next chunk's start. The first setTarget() after
 * prepare() or reset() snaps (no glide from nowhere). One cutoff for every lane of the bus. A lane that is not
 * processed in a chunk keeps its state; a chunk in which no lane is processed moves the filter straight to its
 * target. With every detail at its default the cutoff and q are the plain law: 170 Hz x 2^(7 x sqrt(peakness) x
 * (1 - 0.9 x sqrt(f))) and 0.7 + 1.5 r^2.
 */
class SweepFilter
{
public:
    static constexpr int c_lanes = 3;
    enum Type { LowPass = 0, HighPass, BandPass, Notch, c_types };
    enum Shape { Sine = 0, Triangle, RampUp, RampDown, Square, Random, c_shapes };

    /** ParamIDs::busDetail FilterType .. FilterRate, in plain units. */
    struct Settings
    {
        int type = LowPass;
        bool steep = false;       // 24 dB an octave
        int shape = Sine;
        float closedAtHz = 170.0f; // 20 .. 2000
        float resonance = 0.0f;   // 0 .. 1, added to what the knob gives
        float follow = 1.0f;      // 0 .. 1: how far it tracks the whoosh
        float sweep = 0.0f;       // 0 .. 1: its own rhythm's depth, up to +-3 octaves
        float rateHz = 1.0f;      // 0.05 .. 20
    };

    /** Octaves over Closed At for the front wheel f (0..1), the bus's level against its peak (0..1) and the
        rhythm's value (-1..1). */
    static float octaves(const Settings& s, float f, float peakness, float lfo)
    {
        const float follow = sj::clamp(s.follow, 0.0f, 1.0f);
        return 7.0f * (1.0f - 0.9f * std::sqrt(sj::clamp(f, 0.0f, 1.0f))) * (follow * std::sqrt(sj::clamp(peakness, 0.0f, 1.0f)) + (1.0f - follow))
             + 3.0f * sj::clamp(s.sweep, 0.0f, 1.0f) * lfo;
    }
    /** Where the filter is (Hz), for the type in `s`: this is what setTarget() is given. Low Pass:
        clamp(Closed At x 2^octaves, 20 Hz, 0.45 fs). High Pass: that, mirrored on the log axis (20 x 22000 / it:
        closed at 170 Hz it sits at 2.6 kHz and opens downward). Band Pass and Notch: centred on it, 12 kHz at the
        most. */
    static float cutoffHz(const Settings& s, float f, float peakness, float lfo, float sampleRate)
    {
        const float top = 0.45f * sampleRate;
        const float open = sj::clamp(s.closedAtHz * std::exp2(octaves(s, f, peakness, lfo)), 20.0f, top);
        switch (s.type)
        {
            case HighPass: return sj::clamp(c_mirrorHz2 / open, 20.0f, top);
            case BandPass:
            case Notch:    return std::min(open, c_centreMaxHz);
            default:       return open;
        }
    }
    static constexpr float c_mirrorHz2 = 20.0f * 22000.0f, c_centreMaxHz = 12000.0f, c_sweepOctaves = 3.0f;
    static float q(const Settings& s, float f)
    {
        const float r = sj::clamp((f - 0.6f) / 0.4f, 0.0f, 1.0f);
        const float res = sj::clamp(s.resonance, 0.0f, 1.0f);
        return 0.7f + 1.5f * r * r + 8.0f * res * res;
    }
    static float trim(float q) { return 1.0f / (1.0f + 0.35f * (q - 0.7f)); }

    static constexpr int c_maxFrames = 32;
    static constexpr double c_smoothSamples = 16.0;   // each of the two smoothers on the filter's place
    static constexpr float c_typeSeconds = 0.005f;    // a change of Type or of 24 dB crossing over
    static constexpr float c_slewPerSecond = 500.0f;  // the rhythm's fastest move: its whole swing in 4 ms
    static constexpr float c_steepFirstQ = 0.5412f, c_steepSecondQ = 1.8478f; // 24 dB: see the class comment
    static constexpr float c_bandQ = 0.7f;            // the band output is level at this Q

    void prepare(double sampleRate)
    {
        m_fs = static_cast<float>(sampleRate > 0.0 ? sampleRate : 48000.0);
        m_smooth = 1.0 - std::exp(-1.0 / c_smoothSamples);
        reset();
    }
    void reset()
    {
        for (auto& lane : m_state)
            lane = {};
        m_phase = 0.0;
        m_lfo = m_held = 0.0f;
        m_lfoSnap = m_draw = m_snap = true;
        m_filled = 0;
        m_second = false;
    }
    void setSeed(std::uint32_t seed) { m_rng.seed(seed); m_draw = true; }
    /** The rhythm from this point of its cycle (0..1): renders start each file somewhere else. */
    void scatter(float phase)
    {
        m_phase = static_cast<double>(sj::clamp(phase, 0.0f, 1.0f));
        if (m_phase >= 1.0)
            m_phase = 0.0;
        m_lfoSnap = m_draw = true;
    }
    /** Moves the rhythm on by `frames` samples; returns its value at the end of them (-1..1). */
    float advance(const Settings& s, int frames)
    {
        frames = frames < 1 ? 1 : (frames > c_maxFrames ? c_maxFrames : frames);
        m_phase += static_cast<double>(sj::clamp(s.rateHz, 0.05f, 20.0f)) * static_cast<double>(frames) / static_cast<double>(m_fs);
        bool wrapped = false;
        if (m_phase >= 1.0)
        {
            m_phase -= std::floor(m_phase);
            wrapped = true;
        }
        const float ph = static_cast<float>(m_phase);
        float raw = 0.0f;
        switch (s.shape)
        {
            case Triangle: { const float u = ph + 0.25f; raw = 1.0f - 4.0f * std::abs(u - std::floor(u) - 0.5f); break; }
            case RampUp:   raw = 2.0f * ph - 1.0f; break;
            case RampDown: raw = 1.0f - 2.0f * ph; break;
            case Square:   raw = ph < 0.5f ? 1.0f : -1.0f; break;
            case Random:
                if (wrapped || m_draw)
                {
                    m_held = m_rng.bipolar();
                    m_draw = false;
                }
                raw = m_held;
                break;
            default:       raw = std::sin(sj::c_twoPi * ph); break;
        }
        if (m_lfoSnap)
        {
            m_lfo = raw;
            m_lfoSnap = false;
        }
        else
        {
            const float step = c_slewPerSecond * static_cast<float>(frames) / m_fs;
            m_lfo += sj::clamp(raw - m_lfo, -step, step);
        }
        return m_lfo;
    }
    /** The rhythm's value now (-1..1), as advance() last returned it. */
    float getRhythm() const { return m_lfo; }
    /** This chunk's filter: the type and slope from `s`, where it is (cutoffHz(), already placed for the type) and
        q() at the end of the chunk. */
    void setTarget(const Settings& s, float cutoff, float qValue)
    {
        Point t;
        t.hz = sj::clamp(cutoff, 10.0f, m_fs * 0.45f);
        t.lnHz = std::log(static_cast<double>(t.hz));
        const float k = 1.0f / std::max(0.05f, qValue);
        t.kA = t.kB = k;
        switch (s.type)
        {
            case HighPass: t.w = { 0.0f, 0.0f, 1.0f }; break;
            case BandPass: t.w = { 0.0f, 1.0f / c_bandQ, 0.0f }; break;
            case Notch:    t.w = { 1.0f, 0.0f, 1.0f }; break;
            default:       t.w = { 1.0f, 0.0f, 0.0f }; break;
        }
        t.second = s.steep ? 1.0f : 0.0f;
        if (s.steep)
        {
            if (s.type == BandPass)
                t.kA = 1.0f / c_bandQ;
            else if (s.type != Notch)
            {
                t.kA = 1.0f / c_steepFirstQ;
                t.kB = k / c_steepSecondQ;
            }
        }
        if (m_snap)
        {
            m_from = t;
            m_s1 = m_s2 = t.lnHz;
            m_second = s.steep;
            m_snap = false;
        }
        m_to = t;
        m_filled = 0;
    }
    /** One lane (0 .. c_lanes - 1): x in, the filtered stream out (out may be x), frames <= 32. */
    void process(int lane, const float* x, float* out, int frames)
    {
        if (lane < 0 || lane >= c_lanes || frames <= 0)
            return;
        frames = frames > c_maxFrames ? c_maxFrames : frames;
        if (m_filled != frames)
            fill(frames);
        auto& st = m_state[static_cast<size_t>(lane)];
        float a1s = st.a1, a2s = st.a2, b1s = st.b1, b2s = st.b2;
        for (int i = 0; i < frames; ++i)
        {
            const auto n = static_cast<size_t>(i);
            const float g = m_g[n];
            const float in = x[i];
            // The same steps as sj::Svf::process, so a held Low Pass is that filter to the bit.
            const float a1 = m_a1A[n];
            const float a2 = g * a1;
            const float a3 = g * a2;
            const float v3 = in - a2s;
            const float v1 = a1 * a1s + a2 * v3;
            const float v2 = a2s + a2 * a1s + a3 * v3;
            a1s = 2.0f * v1 - a1s;
            a2s = 2.0f * v2 - a2s;
            const float hp = in - m_kA[n] * v1 - v2;
            float y = m_wl[n] * v2 + m_wb[n] * v1 + m_wh[n] * hp;
            if (m_second)
            {
                const float c1 = m_a1B[n];
                const float c2 = g * c1;
                const float c3 = g * c2;
                const float u3 = y - b2s;
                const float u1 = c1 * b1s + c2 * u3;
                const float u2 = b2s + c2 * b1s + c3 * u3;
                b1s = 2.0f * u1 - b1s;
                b2s = 2.0f * u2 - b2s;
                const float hp2 = y - m_kB[n] * u1 - u2;
                y += m_mix[n] * (m_wl[n] * u2 + m_wb[n] * u1 + m_wh[n] * hp2 - y);
            }
            out[i] = y;
        }
        st.a1 = a1s; st.a2 = a2s; st.b1 = b1s; st.b2 = b2s;
    }
    void endChunk()
    {
        if (m_filled > 0)
        {
            m_s1 = m_endS1;
            m_s2 = m_endS2;
            m_from = m_end;
            m_from.hz = m_to.hz;
            m_from.lnHz = m_to.lnHz;
            if (m_second && m_end.second <= 0.0f && m_to.second <= 0.0f)
            {
                m_second = false;
                for (auto& lane : m_state)
                    lane.b1 = lane.b2 = 0.0f;
            }
        }
        else if (! m_snap)
        {
            // No lane ran: nothing to glide for, so the filter is at its target when one next does.
            m_from = m_to;
            m_s1 = m_s2 = m_to.lnHz;
            if (m_second != (m_to.second > 0.0f))
            {
                m_second = m_to.second > 0.0f;
                for (auto& lane : m_state)
                    lane.b1 = lane.b2 = 0.0f;
            }
        }
        m_filled = 0;
    }

private:
    /** Where the filter is at one end of a chunk. */
    struct Point
    {
        float hz = 1000.0f;
        double lnHz = 6.907755;
        float kA = 1.0f, kB = 1.0f;                 // each stage's damping (1 / Q)
        std::array<float, 3> w { 1.0f, 0.0f, 0.0f }; // how much of the low, band and high outputs is heard
        float second = 0.0f;                         // how much of the second stage is heard (24 dB)
    };
    struct LaneState { float a1 = 0.0f, a2 = 0.0f, b1 = 0.0f, b2 = 0.0f; };

    /** This chunk's coefficients, sample by sample, from where the last chunk ended. Changes nothing that
        endChunk() has not taken, so it can run again for a different length. */
    void fill(int frames)
    {
        Point end = m_to;
        // Type and slope cross over at a set rate, however long the chunk is.
        const float step = static_cast<float>(frames) / (c_typeSeconds * m_fs);
        for (size_t j = 0; j < 3; ++j)
            end.w[j] = m_from.w[j] + sj::clamp(m_to.w[j] - m_from.w[j], -step, step);
        end.second = m_from.second + sj::clamp(m_to.second - m_from.second, -step, step);
        if (std::abs(m_to.w[0] - end.w[0]) < 1.0e-6f && std::abs(m_to.w[1] - end.w[1]) < 1.0e-6f && std::abs(m_to.w[2] - end.w[2]) < 1.0e-6f)
            end.w = m_to.w;
        if (std::abs(m_to.second - end.second) < 1.0e-6f)
            end.second = m_to.second;
        if (end.second > 0.0f || m_from.second > 0.0f)
            m_second = true;

        double s1 = m_s1, s2 = m_s2;
        const bool held = ! (m_from.lnHz < m_to.lnHz) && ! (m_from.lnHz > m_to.lnHz) && std::abs(s1 - m_to.lnHz) < 1.0e-7 && std::abs(s2 - m_to.lnHz) < 1.0e-7;
        if (held)
            s1 = s2 = m_to.lnHz;
        const float gHeld = std::tan(sj::c_pi * m_to.hz / m_fs);
        const float piOverFs = sj::c_pi / m_fs;
        const float inv = 1.0f / static_cast<float>(frames);
        const double span = m_to.lnHz - m_from.lnHz;
        for (int i = 0; i < frames; ++i)
        {
            const auto n = static_cast<size_t>(i);
            const float t = static_cast<float>(i + 1) * inv;
            float g = gHeld;
            if (! held)
            {
                s1 += m_smooth * (m_from.lnHz + span * static_cast<double>(t) - s1);
                s2 += m_smooth * (s1 - s2);
                g = std::tan(piOverFs * static_cast<float>(std::exp(s2)));
            }
            const float kA = m_from.kA + (m_to.kA - m_from.kA) * t;
            const float kB = m_from.kB + (m_to.kB - m_from.kB) * t;
            m_g[n] = g;
            m_kA[n] = kA;
            m_kB[n] = kB;
            m_a1A[n] = 1.0f / (1.0f + g * (g + kA));
            m_a1B[n] = 1.0f / (1.0f + g * (g + kB));
            m_wl[n] = m_from.w[0] + (end.w[0] - m_from.w[0]) * t;
            m_wb[n] = m_from.w[1] + (end.w[1] - m_from.w[1]) * t;
            m_wh[n] = m_from.w[2] + (end.w[2] - m_from.w[2]) * t;
            m_mix[n] = m_from.second + (end.second - m_from.second) * t;
        }
        end.kA = m_to.kA;
        end.kB = m_to.kB;
        m_end = end;
        m_endS1 = s1;
        m_endS2 = s2;
        m_filled = frames;
    }

    float m_fs = 48000.0f;
    double m_smooth = 1.0 - 0.939413062813476; // 1 - exp(-1 / c_smoothSamples), set by prepare()

    // The rhythm.
    double m_phase = 0.0;
    float m_lfo = 0.0f, m_held = 0.0f;
    bool m_lfoSnap = true, m_draw = true;
    sj::Random m_rng;

    // The filter: the last chunk's end, this chunk's target, and the smoothers on its place.
    Point m_from, m_to, m_end;
    double m_s1 = 6.907755, m_s2 = 6.907755, m_endS1 = 6.907755, m_endS2 = 6.907755;
    bool m_snap = true, m_second = false;
    int m_filled = 0;
    std::array<float, c_maxFrames> m_g {}, m_kA {}, m_kB {}, m_a1A {}, m_a1B {}, m_wl {}, m_wb {}, m_wh {}, m_mix {};
    std::array<LaneState, c_lanes> m_state {};
};

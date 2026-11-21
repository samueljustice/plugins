#pragma once

#include "DspUtils.h"

#include <array>
#include <cmath>
#include <cstdint>

/**
 * A BUS'S FILTER, WITH WHAT IS BEHIND ITS WHEEL (a stub: process() leaves the sound untouched and the laws give
 * the filter the bus had before; the DSP is filled in by its own work package). One per bus
 * (RigEngine::BusState::sweep), up to c_lanes lanes. Header only, JUCE-free.
 *
 * What the filler must provide (design 3.6):
 *   (The laws are already here, and are not the filler's to change: octaves(), cutoffHz() with each type's place
 *   on the axis, q() and trim(). The wet mix law and the tanh stay the engine's.)
 *   - One state-variable filter per lane, two with 24 dB; g and k interpolated per sample across each chunk from
 *     where the last chunk ended (no stepping every 32 samples).
 *   - Its own rhythm (advance()): Sine, Triangle, Ramp Up, Ramp Down, Square, Random (from its own sj::Random,
 *     seeded by setSeed), -1..1; its phase is reset by reset() and set by scatter() for renders.
 *   - No allocation, locks or I/O anywhere. Stable at full Resonance; spurs under -80 dB under a 20 Hz sweep.
 *
 * What the callers rely on (RigEngine::renderBus, owned by the engine's work package): prepare() and setSeed() from
 * RigEngine::prepare and its seeded restarts, reset() from RigEngine::reset, scatter() from RigEngine::scatterRide;
 * once per chunk (32 samples at most): advance() (the rhythm), then setTarget() with cutoffHz() and q() for the END
 * of this chunk, then process() on each lane in use, which puts the filtered stream into `out` (the engine mixes it
 * in with its tanh, trim() and mix law, as it does now), then endChunk(), which makes this chunk's target the next
 * chunk's start. The first setTarget() after prepare() or reset() snaps (no glide from nowhere). One cutoff for
 * every lane of the bus. A lane that is not processed in a chunk keeps its state. With every detail at its default
 * the cutoff and q are exactly the bus's earlier law (only the glide between chunks is new).
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

    void prepare(double sampleRate) { m_fs = static_cast<float>(sampleRate > 0.0 ? sampleRate : 48000.0); reset(); }
    void reset() { m_phase = 0.0; m_lfo = 0.0f; }
    void setSeed(std::uint32_t seed) { m_rng.seed(seed); }
    /** The rhythm from this point of its cycle (0..1): renders start each file somewhere else. */
    void scatter(float phase) { m_phase = static_cast<double>(sj::clamp(phase, 0.0f, 1.0f)); }
    /** Moves the rhythm on by `frames` samples; returns its value at the end of them (-1..1). */
    float advance(const Settings& s, int frames) { (void) s; (void) frames; return m_lfo; }
    /** The rhythm's value now (-1..1), as advance() last returned it. */
    float getRhythm() const { return m_lfo; }
    /** This chunk's filter: the type and slope from `s`, where it is (cutoffHz(), already placed for the type) and
        q() at the end of the chunk. */
    void setTarget(const Settings& s, float cutoff, float qValue) { (void) s; (void) cutoff; (void) qValue; }
    /** One lane (0 .. c_lanes - 1): x in, the filtered stream out (out may be x), frames <= 32. */
    void process(int lane, const float* x, float* out, int frames)
    {
        (void) lane;
        if (out != x)
            for (int i = 0; i < frames; ++i)
                out[i] = x[i];
    }
    void endChunk() {}

private:
    float m_fs = 48000.0f;
    double m_phase = 0.0;
    float m_lfo = 0.0f;
    sj::Random m_rng;
};

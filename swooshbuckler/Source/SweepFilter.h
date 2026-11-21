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
 *   - octaves(): oct = 7 (1 - 0.9 sqrt(f)) (F sqrt(peakness) + (1 - F)) + 3 Sweep lfo, and cutoffHz():
 *     clamp(ClosedAt 2^oct, 20 Hz, 0.45 fs). High Pass mirrors on the log axis (20 x 22000 / cutoff); Band Pass and
 *     Notch centre on min(cutoff, 12 kHz).
 *   - q(): 0.7 + 1.5 r^2 + 8 Resonance^2, r = clamp((f - 0.6) / 0.4, 0, 1); trim() extends the trim law
 *     1 / (1 + 0.35 (q - 0.7)). The wet mix law and the tanh stay the engine's.
 *   - One state-variable filter per lane, two with 24 dB; g and k interpolated per sample across each chunk from
 *     where the last chunk ended (no stepping every 32 samples).
 *   - Its own rhythm (advance()): Sine, Triangle, Ramp Up, Ramp Down, Square, Random (from its own sj::Random,
 *     seeded by setSeed), -1..1; its phase is reset by reset() and set by scatter() for renders.
 *   - No allocation, locks or I/O anywhere. Stable at full Resonance; spurs under -80 dB under a 20 Hz sweep.
 *
 * What the callers rely on (RigEngine::renderBus, owned by the engine's work package): prepare() and setSeed() from
 * RigEngine::prepare and its seeded restarts, reset() from RigEngine::reset; once per chunk: advance() (the rhythm),
 * then setTarget() with cutoffHz() and q() for this chunk, then process() on each lane in place, which returns the
 * filtered sample stream into `out` (the engine mixes it in with its tanh and mix law, as it does now), then
 * endChunk(). With every detail at its default the cutoff and q are exactly the bus's earlier law.
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
    /** The cutoff (Hz) for the Low Pass type; see the class comment for the others. */
    static float cutoffHz(const Settings& s, float f, float peakness, float lfo, float sampleRate)
    {
        return sj::clamp(s.closedAtHz * std::exp2(octaves(s, f, peakness, lfo)), 20.0f, 0.45f * sampleRate);
    }
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
    /** This chunk's filter (the type, the cutoff from cutoffHz(), q from q()). */
    void setTarget(const Settings& s, float cutoff, float qValue) { (void) s; (void) cutoff; (void) qValue; }
    /** One lane: x in, the filtered stream out (out may be x). */
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

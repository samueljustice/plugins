#pragma once

#include <array>
#include <cstdint>

/**
 * OUTPUT EQ (a stub: it passes the sound untouched; the DSP is filled in by its own work package).
 *
 * Where it sits: SwooshbucklerAudioProcessor::renderAtRenderRate, between RigEngine::process and the output stage
 * (RoomMaster: Inflate, Push, the limiters), at the render rate, on every channel of the room's pipe with one set
 * of coefficients (so the mains and the LFE copy stay in phase). JUCE-free.
 *
 * What the filler must provide (design 3.2):
 *   - Sections, double-precision state-variable filters, in this order: Low Cut (one section at Q 0.7071 for
 *     12 dB an octave, two at Q 0.5412 and 1.3066 for 24), Low Shelf (Q 0.7071), Low Mid and High Mid bells,
 *     High Shelf, High Cut (one section, Q 0.7071).
 *   - Targets read every call of setParams (the processor calls it every 512 samples); frequency (in octaves) and
 *     gain (in dB) glide over 20 ms; coefficients recomputed every 32 samples and interpolated per sample.
 *   - FLAT IS BIT-EXACT: a band at 0 dB is skipped once it has settled there; a cut going Off glides to its end
 *     stop, fades its mix out over 5 ms and is then skipped; `on` false does that for every band. With every band
 *     skipped, process() leaves the samples exactly as they were.
 *   - responseDb(): the true discrete response at that sample rate (the graph draws it).
 *   - No allocation, locks or I/O in setParams(), process(), isBypassed() or reset(). prepare() may allocate.
 *   - Budget: at most 1% of a core at 7.1.4 and 192 kHz with every band in; nothing when flat.
 *
 * What the callers rely on:
 *   - The processor: prepare(renderRate, pipeChannels) from prepareInternal, reset() when the stream starts again,
 *     setParams() then process(chans, numChans, n) on the audio thread, every block, in place. It adds no latency.
 *   - The graph and the EQ key (EqGraph, PluginEditor): responseDb() and isFlat() on the message thread, with the
 *     settings from SwooshbucklerAudioProcessor::getOutputEqParams() and its getRenderRate().
 *   - Render Variations' description and the tape's: isFlat() (the EQ is described only when it is on and not flat).
 */
class OutputEq
{
public:
    enum Band { LowCut = 0, LowShelf, LowMid, HighMid, HighShelf, HighCut, c_bands };
    static constexpr int c_maxChannels = 16;

    /** The settings, in plain units. A cut at 0 Hz is Off (ParamIDs' parked end stops become 0 here). */
    struct Params
    {
        bool on = true;
        float lowCutHz = 0.0f;  // 0 = Off, else 20 .. 1000
        int lowCutSlope = 24;   // 12 or 24 dB an octave
        float lowShelfHz = 100.0f, lowShelfDb = 0.0f;
        float lowMidHz = 300.0f, lowMidDb = 0.0f, lowMidQ = 1.0f;
        float highMidHz = 3000.0f, highMidDb = 0.0f, highMidQ = 1.0f;
        float highShelfHz = 8000.0f, highShelfDb = 0.0f;
        float highCutHz = 0.0f; // 0 = Off, else 1000 .. 20000
    };

    /** The settings do nothing: off, or every gain at 0 dB and both cuts Off. */
    static bool isFlat(const Params& p);

    /** The response of these settings at hz, in dB, as the filters at sampleRate really give it (one band, or every
        band together with band < 0). Any thread. */
    static double responseDb(const Params& p, double hz, double sampleRate, int band = -1);

    void prepare(double sampleRate, int numChannels);
    /** Empties every filter and snaps every glide to the settings last given. */
    void reset();
    /** Audio thread, before process(). */
    void setParams(const Params& p) { m_target = p; }
    /** In place, every channel alike. */
    void process(float* const* channels, int numChannels, int numSamples);
    /** Every band is skipped right now (process() leaves the samples as they are). Audio thread. */
    bool isBypassed() const { return true; }

private:
    double m_sampleRate = 48000.0;
    int m_channels = 2;
    Params m_target;
};

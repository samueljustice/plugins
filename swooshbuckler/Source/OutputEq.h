#pragma once

#include <array>
#include <cstdint>

/**
 * OUTPUT EQ: six bands on the whole output, one curve for every speaker.
 *
 * Where it sits: SwooshbucklerAudioProcessor::renderAtRenderRate, between RigEngine::process and the output stage
 * (RoomMaster: Inflate, Push, the limiters), at the render rate, on every channel of the room's pipe with one set
 * of coefficients (so the mains and the LFE copy stay in phase). JUCE-free. It adds no latency.
 *
 * THE FILTERS. Seven state-variable sections in double precision (the trapezoidal form: g = tan(pi f / fs),
 * k = 1/Q, v1 the band-pass and v2 the low-pass, out = m0 x + m1 v1 + m2 v2), in this order:
 *   Low Cut     one section at Q 0.7071 (12 dB an octave) or two at Q 0.5412 and 1.3066 (24);  m = (1, -k, -1)
 *   Low Shelf   Q 0.7071, A = 10^(dB/40), g / sqrt(A);                                          m = (1, k(A-1), A^2-1)
 *   Low Mid,    bells: k = 1/(Q A);                                                             m = (1, k(A^2-1), 0)
 *   High Mid
 *   High Shelf  Q 0.7071, g * sqrt(A);                                                          m = (A^2, k(1-A)A, 1-A^2)
 *   High Cut    one section at Q 0.7071;                                                        m = (0, 0, 1)
 * Each is the bilinear transform (warped to its own frequency) of the textbook analogue shape; the test measures
 * every band against that. Near the top of the band at 44.1 and 48 kHz the curves are squeezed towards half the
 * sample rate; responseDb() gives that true curve, and the graph draws it.
 *
 * MOVES. The settings are read every 32 samples. Frequency and Q (both in octaves) and gain (in dB) ease to a
 * new value over 20 ms, starting and landing gently; the section's g, k and m are worked out for the end of each
 * 32 samples and run in a straight line per sample between (a1..a3 follow from g and k every sample, so the section stays a true filter all the
 * way). The 32-sample grid is the stream's, not the block's: the sound does not depend on the block sizes.
 *
 * FLAT IS BIT-EXACT. A band at 0 dB is skipped once it has glided there (its last sample out is its sample in, to
 * the bit, so the hand-over is silent). A cut going Off glides to its end stop (1 Hz, or 30 kHz or just under
 * half the sample rate), fades its mix out over 5 ms and is then skipped; coming On it starts empty at the end
 * stop (a filter that starts empty in the middle of a sound rings at its own frequency for a moment; the lower
 * that frequency, the smaller the ring, which is why the stop is 1 Hz and not the control's 10). Switching the Low Cut's slope glides the first section's Q and fades the second in or out over 20 ms.
 * `on` false does all of that for every band. With every band skipped, process() does not touch the samples.
 *
 * No allocation, locks or I/O anywhere (the object holds everything it needs).
 * Cost: see the Output EQ test's speed line (7.1.4 at 192 kHz, every band in); nothing when flat.
 *
 * What the callers rely on:
 *   - The processor: prepare(renderRate, pipeChannels) from prepareInternal, reset() when the stream starts again,
 *     setParams() then process(chans, numChans, n) on the audio thread, every block, in place.
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
    bool isBypassed() const { return m_running == 0; }

    static constexpr int c_sections = 7; // Low Cut twice, then one for each other band
    static constexpr int c_chunk = 32;
    static constexpr double c_glideSeconds = 0.020, c_fadeSeconds = 0.005;
    static constexpr double c_lowStopHz = 1.0, c_highStopHz = 30000.0; // where an Off cut rests
    static constexpr double c_maxDb = 24.0;                             // (the controls give +/-12)

    /** One section's numbers: g and k make the filter, m mixes its three outputs. */
    struct Coefs
    {
        double g = 1.0, k = 1.0, m0 = 1.0, m1 = 0.0, m2 = 0.0;
    };
    /** The sections of one band with everything settled (the Low Cut has two, the rest one); `count` is how many
        are in. What process() arrives at and what responseDb() draws. */
    struct BandCoefs
    {
        std::array<Coefs, 2> section;
        int count = 0;
    };
    static BandCoefs coefsFor(const Params& p, int band, double sampleRate);

private:
    /** A value that eases to its target (no corner at either end), in steps of one chunk. */
    struct Glide
    {
        double cur = 0.0, target = 0.0, from = 0.0;
        int left = 0, length = 1;
        void snap(double v) { cur = target = from = v; left = 0; }
        void set(double v, int chunks)
        {
            if (! (v < target || v > target)) // the target it already has
                return;
            target = v;
            from = cur;
            left = length = chunks;
        }
        /** One chunk on; true if it moved. */
        bool advance()
        {
            if (left <= 0)
                return false;
            if (--left == 0)
            {
                cur = target;
                return true;
            }
            const double t = 1.0 - static_cast<double>(left) / static_cast<double>(length);
            cur = from + (target - from) * t * t * (3.0 - 2.0 * t);
            return true;
        }
        bool moving() const { return left > 0; }
    };
    struct Section
    {
        bool running = false, moving = false;
        Coefs now, end, step;
        double a1 = 0.0, a2 = 0.0, a3 = 0.0; // from now.g and now.k, for a section that is not moving
        std::array<double, c_maxChannels> ic1 {}, ic2 {};
    };
    struct Bell
    {
        bool in = false;
        Glide octave, db, logQ;
    };
    struct Cut
    {
        bool in = false;
        Glide octave, mix, slope;
    };

    void startChunk(bool snap);
    void aim(Section& s, const Coefs& end, bool moved);
    void start(Section& s, const Coefs& at);
    void stop(Section& s);
    double clampOctave(double hz) const;
    Coefs lowCutCoefs(int section) const;
    Coefs highCutCoefs() const;

    double m_sampleRate = 48000.0;
    int m_channels = 2, m_lanes = 2; // lanes: the channels rounded up to an even count
    int m_glideChunks = 30, m_fadeChunks = 8;
    int m_phase = 0;   // samples into the current chunk
    int m_running = 0; // sections in
    Params m_target;
    std::array<Section, c_sections> m_sections;
    std::array<Bell, 4> m_bells; // Low Shelf, Low Mid, High Mid, High Shelf (the shelves' Q stays put)
    Cut m_lowCut, m_highCut;
};

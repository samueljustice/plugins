#pragma once

// Stamp: renders the matched shift onto the effect, keeping its length and timing.
//
// It runs as a stream, because that is how an offline render reaches a plug-in: block after
// block, with the same number of samples going out as came in. The output is the input shifted
// in pitch and delayed by latencySamples(), which the host compensates for.
//
// Timing: the stretcher's delay is not fixed, it depends on the pitch ratio (a shift up comes
// out a few milliseconds early, a shift down late, and a bend moves that around). The Stamper
// reads the stretcher's output back through the opposite delay, so every moment of the input
// leaves exactly latencySamples() later whatever the shift is doing. That is what keeps hits
// where they were.
//
// A render usually carries more than the learned effect: the host adds handles before and after
// it. So the Stamper first finds where the learned audio starts in the stream (it compares the
// stream with what Learn captured), and lays the bend curve from there. Before and after the
// learned region the curve's first and last values are held. If the learned audio is not found
// in the stream, the fixed shift is applied throughout and alignmentFound() stays false.
//
// No JUCE. Construction allocates; process() does not.

#include <cstdint>
#include <memory>
#include <vector>

namespace tonalrecall
{
class RubberBandDriver;
}

namespace tonalrecall::core
{

class Stamper
{
public:
    struct Setup
    {
        double sampleRate = 48000.0;
        int channels = 2;
        double staticSemitones = 0.0;      // used alone when there is no curve, or no alignment
        double curveHopSeconds = 0.005;
        std::vector<float> curve;          // semitones per frame of the learned effect; may be empty
        // What Learn captured (the sum of the channels), shared so that starting a run copies
        // none of it; may be null.
        std::shared_ptr<const std::vector<float>> learned;
        bool preserveFormants = false;
        // A looping preview plays the selection round and round: with this set (the selection's
        // length in samples) the curve starts again each time round. 0 = the stream plays once.
        std::int64_t loopLength = 0;
    };

    explicit Stamper(const Setup& setup);
    ~Stamper();

    int latencySamples() const noexcept { return m_latency; }

    /** In place. channels must have Setup::channels pointers; any block size. */
    void process(float* const* channels, int numSamples) noexcept;

    /**
     * Replaces the shift while running: a curve edit made during a looping preview is heard on
     * the spot. The learned audio and where it was found stay. The curve must have as many
     * frames as the one it replaces (the same effect); returns false and changes nothing
     * otherwise. No allocation.
     */
    bool updateShift(double staticSemitones, const std::vector<float>& curve) noexcept;

    bool alignmentFound() const noexcept { return m_aligned; }
    /** Stream sample at which the learned audio starts (valid once alignmentFound()). */
    std::int64_t alignmentOffset() const noexcept { return m_offset; }
    std::int64_t samplesProcessed() const noexcept { return m_fed; }

private:
    double semitonesAt(std::int64_t streamSample) const noexcept;
    void search() noexcept;

    Setup m_setup;
    std::unique_ptr<RubberBandDriver> m_driver;
    int m_chunk = 512;
    int m_latency = 0;
    int m_ratioAlignment = 0;

    // Input gathered into chunks for the stretcher.
    std::vector<std::vector<float>> m_in;
    std::vector<const float*> m_inPtrs;
    int m_inFill = 0;

    // The stretcher's output, kept as a history (a ring per channel, indexed by sample count)
    // and read back through a delay that depends on the pitch ratio: see process().
    std::vector<std::vector<float>> m_history;
    std::int64_t m_produced = 0;             // stretcher samples written to the history
    std::int64_t m_emitted = 0;              // samples returned to the caller
    int m_baseDelay = 0;

    // Alignment: a short window of the learned audio is looked for in the stream as it arrives.
    // Only the last stretch of the stream is kept, enough to compare one window.
    std::vector<float> m_wanted;             // the window of the learned audio that is searched for
    std::vector<float> m_stream;             // the stream's channel sum: a sliding stretch of it
    std::int64_t m_streamStart = 0;          // stream sample of m_stream[0]
    std::int64_t m_streamCount = 0;          // samples held in m_stream
    std::int64_t m_searchFrom = 0;           // next stream sample to try as the window's start
    std::int64_t m_anchor = 0;               // where the window sits in the learned audio
    int m_window = 0;
    float m_tolerance = 0.0f;
    bool m_aligned = false;
    bool m_canAlign = false;
    std::int64_t m_offset = 0;
    std::int64_t m_fed = 0;
};

} // namespace tonalrecall::core

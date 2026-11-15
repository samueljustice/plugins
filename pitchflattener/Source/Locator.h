#pragma once

// Where in the learned sound a stretch of incoming audio comes from.
//
// The plug-in plays the flattened sound in place of the input whenever the input is the learned
// sound again, from wherever in it the input happens to be. That needs the exact sample of the
// learned sound the input matches, found quickly enough that the flattened sound can take over
// before the input reaches the output (the plug-in's latency is what gives it that time).
//
// Method. The probe (a few thousand samples of incoming audio) is compared with every position of
// the learned sound by normalised correlation: the sum of probe times learned sound over the
// probe's length, divided by both stretches' energies. That is 1 for identical audio at any
// playback gain and falls away for anything else; a match is accepted only at 0.98 or more, so
// the same sound at another pitch or speed (which drifts out of step within a few thousand
// samples) and unrelated audio are rejected.
//
// The correlation at every position at once comes from the FFT (overlap-save in blocks of 32768
// or 65536 samples, two blocks per complex transform: the learned sound is real, so one block
// rides in the real part and the next in the imaginary part). The positions that come out near
// the top are then recomputed exactly, in double precision, before one is accepted. The search
// runs from the start of the sound and stops at the first position that matches exactly (the
// common case: the sound played from its top), so playing from the start costs only the first
// block or two.
//
// A sound that repeats itself (a steady tone) matches in many places equally well. Then the
// earliest of them is taken, at or after the position the caller says is consistent with what has
// already been heard; the plug-in keeps checking as the sound goes on and finds the place again if
// it was wrong.
//
// Timings (Apple M-series, one core, plain C++ FFT, 2026): a 120 s learned sound at 96 kHz,
// probe 8192 samples, whole-sound search when the match is at the very end or there is none:
// about 0.2 s; a match at the start: under 5 ms. A 10 s sound at 48 kHz: about 10 ms for a
// whole search. The tests print the figures for the machine they run on.
//
// Plain C++20, no JUCE. prepare() and locate() allocate: for a background thread.

#include <cstdint>
#include <functional>
#include <vector>

namespace pitchflattener
{

class Locator
{
public:
    static constexpr double kAccept = 0.98;            // the least correlation taken as the same audio
    static constexpr float kSilence = 3.1622777e-4f;   // -70 dBFS: below this a sample counts as silence

    struct Match
    {
        bool found = false;
        std::int64_t sourceSample = 0;   // where the probe's first sample is in the learned sound
        double correlation = 0.0;        // exact, at that position (or the best seen when not found)
        double gain = 1.0;               // the probe's level over the learned sound's there
        int candidates = 0;              // positions recomputed exactly
        bool searchedAll = false;        // false: stopped early on an exact match
    };

    /** Takes a copy of the learned sound (one channel: the mix). */
    void prepare(const float* mono, std::int64_t numSamples, double sampleRate);

    bool isPrepared() const noexcept { return m_length > 0; }
    std::int64_t length() const noexcept { return m_length; }
    double sampleRate() const noexcept { return m_sampleRate; }
    /** The learned sound (length() samples, followed by probeLength() zeros). */
    const float* samples() const noexcept { return m_samples.data(); }

    /** How many incoming samples a search wants: about 85 ms, never more than the sound. */
    int probeLength() const noexcept { return m_probeLength; }
    static int nominalProbeLength(double sampleRate) noexcept;

    /**
     * Finds where probe (probeLength() samples) is in the learned sound. preferFrom: positions
     * before this are taken only when nothing later matches as well (see the header comment).
     * cancelled is asked between blocks.
     */
    Match locate(const float* probe, std::int64_t preferFrom = 0, const std::function<bool()>& cancelled = {}) const;

    /** The exact normalised correlation of probe (length samples) with the learned sound at position. */
    double correlationAt(const float* probe, int length, std::int64_t position, double* gain = nullptr) const noexcept;

private:
    std::vector<float> m_samples;
    std::int64_t m_length = 0;
    double m_sampleRate = 0.0;
    int m_probeLength = 0;
};

} // namespace pitchflattener

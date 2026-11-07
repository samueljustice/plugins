#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <vector>

/**
 * [NEW] Master-sample fingerprints (see docs/experiments/07-fingerprint.md).
 *
 * The user marks one sample as the MASTER (a real whoosh, a vocal sketch, a pig squeal).
 * Its fingerprint is a set of descriptor contours sampled every 10 ms: loudness, pitch and
 * how clear the pitch is, brightness, noisiness, change (flux) and timbre (MFCC), plus onsets.
 * Uses:
 *   - makeControlCurves(): the whoosh FOLLOWS the master (loudness -> flight envelope,
 *     pitch contour -> pitch, brightness -> sweep), resampled to the whoosh length.
 *   - similarity():        DTW-based 0..1 score, "do these samples belong together".
 *   - align():             DTW warp map master time -> candidate time.
 *   - matchRegions():      ranks a candidate track's regions by how well they fit the master and
 *                          says where in each region the master's peak lands and how much to stretch.
 *
 * Threading: everything here allocates and is for the message thread or a background thread
 * (analysis of a 10 s sample takes a few ms). ControlCurves is a fixed-size POD meant to be
 * published to the audio thread (e.g. via an atomic pointer); its read functions are noexcept,
 * allocation-free and lock-free.
 */
namespace fp
{
    inline constexpr int c_numMfcc = 13;

    struct Fingerprint
    {
        double hopSeconds = 0.01;  // frame spacing
        double sampleRate = 48000.0;
        int numSamples = 0;

        // Per frame (all the same length). Frame i is centred on i * hopSeconds.
        std::vector<float> rmsDb;        // dBFS, floor -100
        std::vector<float> pitchHz;      // 0 = unvoiced
        std::vector<float> pitchClarity; // 0..1 (1 - YIN aperiodicity), 0 when silent
        std::vector<float> centroidHz;   // spectral centroid
        std::vector<float> rolloffHz;    // 85% energy rolloff
        std::vector<float> flatness;     // 0 (tonal) .. 1 (white noise)
        std::vector<float> flux;         // positive log-mel flux (onset strength), >= 0
        std::vector<std::array<float, c_numMfcc>> mfcc; // c0..c12 (c0 = log energy)

        std::vector<float> onsetsSeconds;

        struct Summary
        {
            float durationSeconds = 0;
            int activeStart = 0, activeEnd = 0; // frame range above (peak - 45 dB), end exclusive
            float peakDb = -100, meanDb = -100; // mean over the active range
            int peakFrame = 0;                  // loudest frame (smoothed)
            float medianPitchHz = 0;            // over voiced frames
            float voicedRatio = 0;              // voiced frames / active frames
            float meanClarity = 0, meanFlatness = 0, meanCentroidHz = 0;
            float onsetRate = 0;                // per second of active audio
            std::array<float, c_numMfcc> mfccMean {};
        } summary;

        int numFrames() const { return static_cast<int>(rmsDb.size()); }
        bool empty() const { return rmsDb.empty(); }
        bool voiced(int frame) const { return pitchHz[static_cast<size_t>(frame)] > 0.0f; }
    };

    /** Analyses mono audio. Pitch range 50..2000 Hz. Message / background thread. */
    Fingerprint analyse(const float* mono, int numSamples, double sampleRate);

    /** A frame range of a fingerprint as its own fingerprint (summary recomputed). */
    Fingerprint slice(const Fingerprint& f, int frameStart, int frameEnd);

    //==========================================================================
    /**
     * 0..1, how alike two sounds are in SHAPE and CHARACTER: loudness contour, relative pitch
     * contour, pitch clarity, noisiness, timbre and brightness movement, compared after DTW so
     * different lengths / speeds don't matter, and a relative pitch shift doesn't either.
     * Typical: same gesture stretched and transposed ~0.8, unrelated sounds < 0.4.
     */
    float similarity(const Fingerprint& a, const Fingerprint& b);

    struct Alignment
    {
        float cost = 0;                   // mean per-step DTW cost (lower = better)
        std::vector<float> bFrameForA;    // for each frame of a (full range), the matching frame of b
        float stretch = 1;                // b active length / a active length along the path
    };

    /** DTW alignment of a's active range onto b's active range (frames outside are extrapolated). */
    Alignment align(const Fingerprint& a, const Fingerprint& b);

    //==========================================================================
    /**
     * Engine control curves. Fixed size, no allocation: copy it, publish a pointer to it, read it
     * from the audio thread with at(). Phase 0..1 runs over the whole whoosh (flight time / length).
     */
    struct ControlCurves
    {
        static constexpr int c_points = 256;

        std::array<float, c_points> envelope {};   // 0..1 linear amplitude, peak = 1
        std::array<float, c_points> pitchSemis {}; // semitones relative to the master's median pitch (gaps filled)
        std::array<float, c_points> pitchConf {};  // 0..1, how much to trust pitchSemis at that point
        std::array<float, c_points> brightness {}; // 0..1, log centroid 100 Hz..10 kHz

        float lengthSeconds = 1;    // the whoosh length these were resampled to
        float masterSeconds = 0;    // the master's active duration (a natural LENGTH suggestion)
        float peakPhase = 0.5f;     // 0..1 where the envelope peaks
        float suggestedShape = 0;   // -1..1, the SHAPE value whose peak position matches (engine maps
                                    //  peakFraction = 0.5 + 0.43 * shape)
        float pitchAmount = 0;      // 0..1, how pitched the master is overall (voiced ratio * clarity)
        bool valid = false;

        /** Linear interpolation of a curve at phase 0..1 (clamped). Audio-thread safe. */
        static float at(const std::array<float, c_points>& curve, float phase) noexcept
        {
            const float x = (phase <= 0.0f ? 0.0f : (phase >= 1.0f ? 1.0f : phase)) * static_cast<float>(c_points - 1);
            const int i = static_cast<int>(x);
            const int j = i + 1 < c_points ? i + 1 : i;
            const float t = x - static_cast<float>(i);
            return curve[static_cast<size_t>(i)] + (curve[static_cast<size_t>(j)] - curve[static_cast<size_t>(i)]) * t;
        }
    };

    /**
     * The master's active range (silence trimmed) stretched onto a whoosh of lengthSeconds.
     * smoothingSeconds is applied in master time before resampling (removes frame jitter).
     */
    ControlCurves makeControlCurves(const Fingerprint& master, float lengthSeconds, float smoothingSeconds = 0.02f);

    //==========================================================================
    struct Span { int start = 0, end = 0; }; // samples, end exclusive (== CargoRegion start/end)

    struct RegionMatch
    {
        int region = -1;            // index into the spans given
        float score = 0;            // 0..1 similarity to the master (sorted best first)
        int startSample = 0;        // where to start playing (inside the region; for long regions the
        int endSample = 0;          //   best-matching window found by subsequence DTW)
        int peakSample = 0;         // the sample that matches the master's loudest moment: land it on the flight peak
        float stretch = 1;          // matched length / master length: play at rate `stretch` (x flight-length scaling) to fit
    };

    /**
     * Ranks a candidate track's regions against the master. candidate must be the fingerprint of
     * the whole track (analyse once per edit; regions are sliced from it, no re-analysis).
     * Regions longer than 1.6x the master are searched with subsequence DTW for their best window.
     */
    std::vector<RegionMatch> matchRegions(const Fingerprint& master, const Fingerprint& candidate,
                                          const std::vector<Span>& regions);
}

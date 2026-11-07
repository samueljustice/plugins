#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>

/**
 * Shared types for the Pitch Flattener analysis / correction pipeline.
 *
 * Everything in Source/dsp is plain C++17 + juce_dsp (FFT, IIR) so it can be
 * exercised from the offline test harness as well as from the plugin.
 */
namespace pf
{
    //==========================================================================
    // Units: pitch is carried as cents relative to A4 = 440 Hz.
    inline float hzToCents(float hz)    { return 1200.0f * std::log2(std::max(hz, 1.0e-3f) / 440.0f); }
    inline float centsToHz(float cents) { return 440.0f * std::exp2(cents / 1200.0f); }

    //==========================================================================
    enum class DetectorType : int
    {
        PYin = 0,        // probabilistic YIN: voice, speech, singing
        Mpm,             // McLeod pitch method (NSDF): solo instruments
        SpectralPeak,    // multi-resolution reassigned STFT peaks: sirens, whistles, tones
        HarmonicSum,     // subharmonic summation on a whitened log spectrum: engines, drones, field
        NumDetectors
    };

    enum class SourceType : int
    {
        Vocal = 0,
        Instrument,
        Siren,
        Whistle,
        Engine,
        Drone,
        NoisyField,
        Custom,
        NumSourceTypes
    };

    enum class TargetMode : int
    {
        Latch = 0,   // lock to the first stable pitch of each sound
        Fixed,       // flatten everything to the Target Pitch
        Auto,        // remove movement faster than the Auto Window, keep the overall contour
        NumModes
    };

    //==========================================================================
    /** One pitch candidate produced by a detector for one analysis frame. */
    struct Candidate
    {
        float hz = 0.0f;
        float salience = 0.0f;   // detector-specific score normalised to [0, 1]
    };

    static constexpr int maxCandidates = 8;

    /** Everything a detector says about one analysis frame. */
    struct FrameObservation
    {
        std::array<Candidate, maxCandidates> cands {};
        int numCands = 0;
        float voicedProb = 0.0f;   // probability the frame is pitched [0, 1]
        float rmsDb = -120.0f;     // level of the analysis frame (pre-filter)

        void clear() { numCands = 0; voicedProb = 0.0f; rmsDb = -120.0f; }

        /** Adds a candidate, merging with an existing one within mergeCents. */
        void add(float hz, float sal, float mergeCents = 35.0f)
        {
            if (! (hz > 0.0f) || ! std::isfinite(hz) || ! (sal > 0.0f))
                return;

            const float c = hzToCents(hz);
            for (int i = 0; i < numCands; ++i)
            {
                if (std::abs(hzToCents(cands[(size_t) i].hz) - c) < mergeCents)
                {
                    if (sal > cands[(size_t) i].salience)
                        cands[(size_t) i] = { hz, sal };
                    return;
                }
            }

            if (numCands < maxCandidates)
            {
                cands[(size_t) numCands++] = { hz, sal };
                return;
            }

            // replace the weakest
            int weakest = 0;
            for (int i = 1; i < numCands; ++i)
                if (cands[(size_t) i].salience < cands[(size_t) weakest].salience)
                    weakest = i;

            if (sal > cands[(size_t) weakest].salience)
                cands[(size_t) weakest] = { hz, sal };
        }

        void sortBySalience()
        {
            std::sort(cands.begin(), cands.begin() + numCands,
                      [] (const Candidate& a, const Candidate& b) { return a.salience > b.salience; });
        }
    };

    //==========================================================================
    /** Settings that require the detector to be rebuilt (allocation) when they change. */
    struct DetectorConfig
    {
        DetectorType type = DetectorType::SpectralPeak;
        float minHz = 300.0f;
        float maxHz = 3000.0f;
        double analysisRate = 24000.0;
        int hop = 120;          // analysis samples between frames
        int maxFrameLength = 8192; // frames are centred in a window of this many analysis samples

        bool operator== (const DetectorConfig& o) const
        {
            auto same = [] (double a, double b) { return ! (a < b) && ! (b < a); };
            return type == o.type && same (minHz, o.minHz) && same (maxHz, o.maxHz)
                && same (analysisRate, o.analysisRate) && hop == o.hop && maxFrameLength == o.maxFrameLength;
        }
        bool operator!= (const DetectorConfig& o) const { return ! (*this == o); }
    };

    /** Settings that can change on every hop without reallocation. */
    struct LiveSettings
    {
        float sensitivity = 0.5f;        // 0..1, higher = more readily voiced
        float trackingSpeedOctPerSec = 8.0f;
        float smoothingMs = 15.0f;       // sigma of the centred Gaussian on the pitch track
        float gapFillMs = 150.0f;
        TargetMode targetMode = TargetMode::Latch;
        float targetHz = 440.0f;
        float autoWindowMs = 1000.0f;
        float amount = 1.0f;             // 0..1 of the deviation removed
        float maxShiftSemitones = 12.0f;
        float gateDb = -50.0f;
        bool manualOverride = false;
        float overrideHz = 440.0f;
        float mix = 1.0f;
        bool formantPreserve = false;
    };

    /** Per-frame telemetry published for the editor. */
    struct TelemetryFrame
    {
        int64_t frame = 0;
        float rawHz = 0.0f;      // best raw detector candidate (0 = none)
        float trackHz = 0.0f;    // final cleaned/smoothed track (0 = unpitched)
        float targetHz = 0.0f;   // pitch being flattened to (0 = none)
        float ratioCents = 0.0f; // correction applied
        float voicedProb = 0.0f;
        float rmsDb = -120.0f;
    };
}

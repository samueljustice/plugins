#pragma once

#include <algorithm>
#include "FlattenerTypes.h"

namespace pf
{
    /** The defaults a Source Type writes into the user-facing parameters. */
    struct SourcePreset
    {
        const char* name;
        const char* blurb;
        DetectorType detector;
        float minHz, maxHz;
        float sensitivity;        // 0..1
        float trackingSpeed;      // oct/s
        float smoothingMs;
        float gapFillMs;
        float highpassHz, lowpassHz;
        bool formant;
        int quality;              // 0 = Smooth (standard window), 1 = Tight (short window)
        float maxShiftSemitones;  // sirens often switch between patterns far apart in pitch (yelp, two-tone, wail)
    };

    inline const SourcePreset& getSourcePreset(SourceType t)
    {
        static const SourcePreset presets[] = {
            { "Vocal / Speech", "pYIN with an HMM tracker. Voices, singing, vocal-like calls.",
              DetectorType::PYin,         60.0f, 1000.0f, 0.50f, 12.0f, 15.0f, 150.0f,   50.0f,  4000.0f, true, 0, 12.0f },
            { "Instrument",     "McLeod pitch method. Solo melodic instruments, vibrato, warble.",
              DetectorType::Mpm,          40.0f, 2500.0f, 0.50f, 16.0f, 10.0f, 150.0f,   30.0f,  8000.0f, false, 0, 12.0f },
            { "Siren",          "Spectral peaks with MPM. Wail, yelp, hi-lo and electronic sweeps.",
              DetectorType::SpectralMpm,  300.0f, 3000.0f, 0.50f, 30.0f,  8.0f, 300.0f,  200.0f, 10000.0f, false, 1, 24.0f },
            { "Whistle / Tone", "Spectral peaks with MPM. Whistles, feedback, test tones, theremin.",
              DetectorType::SpectralMpm,  150.0f, 6000.0f, 0.30f,  8.0f, 20.0f, 300.0f,  100.0f, 11000.0f, false, 0, 24.0f },
            { "Engine / Doppler", "Harmonic sum spectrum. Engines, pass-bys, props, motors, trains.",
              DetectorType::HarmonicSum,   25.0f, 1000.0f, 0.50f,  4.0f, 30.0f, 400.0f,   20.0f,  5000.0f, false, 0, 12.0f },
            { "Drone / Hum",    "Very stable harmonic tracking for hums, generators and drones.",
              DetectorType::HarmonicSum,   25.0f,  400.0f, 0.50f,  6.0f, 50.0f, 400.0f,   20.0f,  3000.0f, false, 0, 24.0f },
            { "Noisy Field",    "Harmonic sum with strict voicing for rough field recordings.",
              DetectorType::HarmonicSum,   40.0f, 1500.0f, 0.35f,  4.0f, 40.0f, 400.0f,   40.0f,  6000.0f, false, 0, 12.0f },
            { "Custom",         "Choose the detector and every setting yourself.",
              DetectorType::SpectralPeak, 100.0f, 2000.0f, 0.50f,  8.0f, 15.0f, 200.0f,   40.0f,  8000.0f, false, 0, 12.0f },
        };

        auto i = std::clamp((int) t, 0, (int) SourceType::NumSourceTypes - 1);
        return presets[i];
    }

    inline const char* getDetectorName(DetectorType d)
    {
        switch (d)
        {
            case DetectorType::PYin:         return "pYIN";
            case DetectorType::Mpm:          return "MPM (McLeod)";
            case DetectorType::SpectralPeak: return "Spectral Peak";
            case DetectorType::HarmonicSum:  return "Harmonic Sum";
            case DetectorType::SpectralMpm:  return "Spectral + MPM";
            case DetectorType::NumDetectors:
            default:                         return "?";
        }
    }
}

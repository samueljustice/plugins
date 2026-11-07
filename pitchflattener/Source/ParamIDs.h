#pragma once

/**
 * Parameter IDs for the APVTS. IDs that existed in 1.x and kept their meaning
 * keep their old ID so old sessions and presets restore what they can.
 */
namespace ParamIDs
{
    // Source / detection
    inline constexpr const char* sourceType      = "sourceType";       // Choice: SourceType
    inline constexpr const char* detector        = "detector";         // Choice: DetectorType
    inline constexpr const char* minFreq         = "minFreq";          // Hz (kept from 1.x)
    inline constexpr const char* maxFreq         = "maxFreq";          // Hz (kept from 1.x)
    inline constexpr const char* sensitivity     = "sensitivity";      // %
    inline constexpr const char* trackingSpeed   = "trackingSpeed";    // oct/s
    inline constexpr const char* trackSmoothing  = "trackSmoothing";   // ms
    inline constexpr const char* gapFill         = "gapFill";          // ms
    inline constexpr const char* volumeThreshold = "volumeThreshold";  // dB (kept from 1.x)
    inline constexpr const char* detectionHighpass = "detectionHighpass"; // Hz (kept from 1.x)
    inline constexpr const char* detectionLowpass  = "detectionLowpass";  // Hz (kept from 1.x)

    // Target / correction
    inline constexpr const char* targetMode      = "targetMode";       // Choice: Latch / Fixed / Auto
    inline constexpr const char* targetPitch     = "targetPitch";      // Hz (kept from 1.x)
    inline constexpr const char* autoWindow      = "autoWindow";       // ms
    inline constexpr const char* flattenAmount   = "flattenAmount";    // %
    inline constexpr const char* maxShift        = "maxShift";         // semitones
    inline constexpr const char* manualOverride  = "manualOverride";   // bool (kept from 1.x)
    inline constexpr const char* overrideFreq    = "overrideFreq";     // Hz (kept from 1.x)

    // Shifter / output
    inline constexpr const char* rbFormantPreserve = "rbFormantPreserve"; // bool (kept from 1.x)
    inline constexpr const char* quality         = "quality";          // Choice: Smooth / Tight
    inline constexpr const char* mix             = "mix";              // 0..1 (kept from 1.x)
}

#pragma once

#include <JuceHeader.h>

/** Swooshbuckler's version and build, for the About panel. The number comes from CMake (SWOOSHBUCKLER_VERSION). */
namespace Version
{
    inline juce::String number() { return JucePlugin_VersionString; }

    /** "2026-11-07 14:02 UTC" from the release build, otherwise the compile date. */
    inline juce::String builtOn()
    {
       #ifdef BUILD_TIMESTAMP
        return BUILD_TIMESTAMP;
       #else
        return juce::String(__DATE__).replace("  ", " ");
       #endif
    }

    inline juce::String commit()
    {
       #ifdef BUILD_COMMIT
        return BUILD_COMMIT;
       #else
        return {};
       #endif
    }

    inline juce::String juce() { return juce::String(JUCE_MAJOR_VERSION) + "." + juce::String(JUCE_MINOR_VERSION) + "." + juce::String(JUCE_BUILDNUMBER); }
}

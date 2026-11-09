#pragma once

#include <JuceHeader.h>
#include "ParamIDs.h"

/**
 * Changes to saved state that apply to sessions and presets from any earlier version.
 */
namespace StateMigration
{
    /**
     * Manual Override is retired: it set the target to Override Hz in every mode, which is what Fixed with
     * Target Hz does. A state with Override on becomes Fixed at the Override pitch, with Override off.
     */
    inline void overrideToFixed (juce::ValueTree& state)
    {
        const auto param = [&state] (const char* id) { return state.getChildWithProperty ("id", juce::String (id)); };

        auto overrideOn = param (ParamIDs::manualOverride);
        if (! overrideOn.isValid() || ! ((float) overrideOn.getProperty ("value") > 0.5f))
            return;

        const auto overrideHz = param (ParamIDs::overrideFreq);
        const float hz = overrideHz.isValid() ? (float) overrideHz.getProperty ("value") : 440.0f;

        const auto set = [&state, &param] (const char* id, float value)
        {
            auto child = param (id);
            if (! child.isValid())
            {
                child = juce::ValueTree ("PARAM");
                child.setProperty ("id", juce::String (id), nullptr);
                state.appendChild (child, nullptr);
            }
            child.setProperty ("value", value, nullptr);
        };

        set (ParamIDs::targetMode, 1.0f);   // Fixed
        set (ParamIDs::targetPitch, hz);
        set (ParamIDs::manualOverride, 0.0f);
    }
}

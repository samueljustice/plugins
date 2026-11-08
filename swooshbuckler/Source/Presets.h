#pragma once

#include <JuceHeader.h>
#include "Parameters.h"

#include <map>
#include <vector>

/**
 * Factory presets and the randomiser. Both return plain-value targets; the
 * editor glides the parameters to them on springs. Anything a preset doesn't
 * mention goes back to its default. Track levels, input and the output stage
 * are left alone (they belong to your session).
 */
namespace Presets
{
    struct Preset
    {
        juce::String name;
        std::map<juce::String, float> values; // plain (unnormalised) values
    };

    inline const std::vector<Preset>& factory()
    {
        using namespace ParamIDs;
        static const std::vector<Preset> presets {
            { "Steady Energy",   { { energy, 0.45f }, { rate, 0.3f }, { chaos, 0.3f }, { fill, 0.4f } } },
            { "Jet Passes",      { { energy, 0.6f }, { rate, 0.55f }, { "aDoppler", 0.9f }, { "bDoppler", 0.75f }, { "aSwirl", 0.6f },
                                   { "bSwirl", 0.5f }, { "aLow", 0.5f }, { hitSense, 0.4f } } },
            { "Monster Swells",  { { energy, 0.85f }, { rate, 0.25f }, { pitch, -7.0f }, { "aLow", 0.8f }, { "bLow", 0.5f },
                                   { "aDrive", 0.45f }, { hitBoost, 15.0f }, { hitDuck, 0.7f } } },
            { "Gentle Breeze",   { { energy, 0.35f }, { rate, 0.2f }, { "aDoppler", 0.3f }, { "bDoppler", 0.3f }, { "aDrive", 0.0f },
                                   { "aLow", 0.15f }, { "bFilter", 0.5f }, { space, 0.45f }, { hitBoost, 6.0f } } },
            { "Fast and Furious",{ { energy, 0.8f }, { rate, 0.85f }, { chaos, 0.7f }, { "aDoppler", 0.75f }, { "bDoppler", 0.7f },
                                   { "aFilter", 0.6f }, { "bDuck", 0.6f }, { hitSense, 0.65f } } },
            { "Magic Dust",      { { energy, 0.5f }, { rate, 0.45f }, { pitch, 7.0f }, { "aSwirl", 0.75f }, { "bSwirl", 0.8f },
                                   { space, 0.7f }, { "aLow", 0.1f }, { "bLow", 0.05f } } },
            { "Heavy Hits",      { { energy, 0.7f }, { rate, 0.4f }, { hitSense, 0.7f }, { hitBoost, 16.0f }, { hitRelease, 0.55f },
                                   { hitDuck, 0.8f }, { "aLow", 0.6f } } },
        };
        return presets;
    }

    inline bool isSessionParameter(const juce::String& id)
    {
        using namespace ParamIDs;
        if (id.startsWith("cargo")) // track levels, and each track's pitch and VARI
            return true;
        return id == input || id == inflate || id == push || id == ceiling || id == rideOn || id == hitSync
            || id == gateMode || id == duckMode || id == takeTurns || id == linkBuses // how the rig behaves: kept by presets and Randomise
            || id == morphOn || id == morphWander || id == morphShape || id == morphSpeed
            || id.startsWith("mod"); // the modulators and their wires belong to the session, not to a preset
    }

    /** A random but musical setting (wide where it's safe, narrow where it matters). */
    inline Preset roll(juce::Random& r)
    {
        using namespace ParamIDs;
        auto u = [&r](float lo, float hi) { return lo + (hi - lo) * r.nextFloat(); };
        Preset p { "Randomised", {} };
        p.values[energy] = u(0.35f, 0.9f);
        p.values[rate] = u(0.15f, 0.8f);
        p.values[fill] = u(0.1f, 0.6f);
        p.values[chaos] = u(0.2f, 0.7f);
        p.values[pitch] = std::round(u(-7.0f, 7.0f));
        for (int b = 0; b < numBuses; ++b)
        {
            p.values[bus[b][Doppler]] = u(0.2f, 0.95f);
            p.values[bus[b][Low]] = u(0.0f, 0.7f);
            p.values[bus[b][Swirl]] = u(0.0f, 0.8f);
            p.values[bus[b][Filter]] = u(0.0f, 0.7f);
            p.values[bus[b][Drive]] = u(0.0f, b == 0 ? 0.5f : 0.3f);
            if (b > 0)
            {
                p.values[bus[b][Duck]] = u(0.1f, 0.7f);
                p.values[bus[b][Key]] = u(0.0f, 0.6f);
            }
        }
        p.values[hitSense] = u(0.3f, 0.7f);
        p.values[hitBoost] = u(6.0f, 16.0f);
        p.values[hitRelease] = u(0.15f, 0.6f);
        p.values[hitDuck] = u(0.2f, 0.8f);
        p.values[space] = u(0.05f, 0.6f);
        return p;
    }
}

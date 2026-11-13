#pragma once

#include <JuceHeader.h>
#include "Parameters.h"

#include <map>
#include <vector>

/**
 * Factory presets and the randomiser. Both return plain-value targets; the
 * editor glides the parameters to them on springs. A factory preset is a
 * complete look: anything it doesn't mention goes back to its default.
 * Randomise only moves what it rolls and leaves every other knob where it is.
 * Track levels, input and the output stage are left alone (they belong to
 * your session).
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
            { "Steady Energy",   { { energy, 0.2f }, { rate, 0.3f }, { chaos, 0.3f }, { fill, 0.25f } } },
            { "Jet Passes",      { { energy, 0.36f }, { rate, 0.55f }, { "aDoppler", 0.9f }, { "bDoppler", 0.75f }, { "aSwirl", 0.48f },
                                   { "bSwirl", 0.37f }, { "aLow", 0.5f }, { hitSense, 0.4f }, { passMode[0], 1.0f } } },
            { "Monster Swells",  { { energy, 0.92f }, { rate, 0.25f }, { pitch, -7.0f }, { "aLow", 0.8f }, { "bLow", 0.5f },
                                   { "aDrive", 0.4f }, { hitBoost, 9.0f }, { hitDuck, 0.7f }, { passMode[0], 1.0f } } },
            { "Gentle Breeze",   { { energy, 0.14f }, { rate, 0.2f }, { "aDoppler", 0.3f }, { "bDoppler", 0.3f }, { "aDrive", 0.0f },
                                   { "aLow", 0.15f }, { "bFilter", 0.5f }, { space, 0.45f }, { hitBoost, 0.0f } } },
            { "Fast and Furious",{ { energy, 0.9f }, { rate, 0.85f }, { chaos, 0.7f }, { "aDoppler", 0.75f }, { "bDoppler", 0.7f },
                                   { "aFilter", 0.6f }, { "bDuck", 0.6f }, { hitSense, 0.65f }, { passMode[0], 1.0f } } },
            { "Magic Dust",      { { energy, 0.24f }, { rate, 0.45f }, { pitch, 7.0f }, { "aSwirl", 0.66f }, { "bSwirl", 0.73f },
                                   { space, 0.7f }, { "aLow", 0.1f }, { "bLow", 0.05f } } },
            { "Heavy Hits",      { { energy, 0.63f }, { rate, 0.4f }, { hitSense, 0.7f }, { hitBoost, 10.0f }, { hitRelease, 0.55f },
                                   { hitDuck, 0.8f }, { "aLow", 0.6f } } },
        };
        return presets;
    }

    inline bool isSessionParameter(const juce::String& id)
    {
        using namespace ParamIDs;
        if (id.startsWith("cargo")) // track levels, and each track's pitch and VARI
            return true;
        return id == inflate || id == push || id == ceiling || id == rideOn || id == hitSync
            || id == gateMode || id == duckMode || id == takeTurns || id == linkBuses || id == hitAim || id == masterTrim
            || id.startsWith("trem") // the peak tremolo, like the rest of the output stage // how the rig behaves: kept by presets and Randomise
            || id == morphOn || id == morphWander || id == morphShape || id == morphSpeed
            || id == play || id == keysMode || id == throwLength // the transport, what MIDI keys do, how long a throw is
            || id == rideFollowMaster // needs this session's Master track
            || id.startsWith("mod"); // the modulators and their wires belong to the session, not to a preset
    }

    /** The sections Randomise works in: each has its own die and its own lock in the window. */
    enum Section { Ride = 0, BusA, BusB, Hits, Output, Stowaway, numSections };
    inline const char* sectionName(int section)
    {
        static const char* names[numSections] = { "Ride", "Bus A", "Bus B", "Hits", "Output", "Stowaway" };
        return section >= 0 && section < numSections ? names[section] : "";
    }

    /** The values a section die may move that belong to the session (so targets() has to be told to let them through). */
    inline bool isOutputStageRoll(const juce::String& id) { return id == ParamIDs::inflate || id == ParamIDs::push; }

    /** What a preset does with the parameters it doesn't mention. */
    enum class Unlisted
    {
        Default, // back to their defaults: a factory or user preset is a complete look
        Keep,    // left where they are: Randomise, the section dice, the roll history
    };

    /** Where a preset puts each parameter it owns, as (parameter, normalised value). What it names goes there;
        what it doesn't name goes back to its default (Unlisted::Default) or stays put (Unlisted::Keep). The
        session's parameters are left out, except the modulators' own controls when the preset names them (a user
        preset saved with its modulators) and, with outputStage, Inflate and Push (the Output die and its history). */
    inline std::vector<std::pair<juce::RangedAudioParameter*, float>> targets(const juce::Array<juce::AudioProcessorParameter*>& parameters,
                                                                               const Preset& preset, Unlisted unlisted,
                                                                               bool outputStage = false)
    {
        std::vector<std::pair<juce::RangedAudioParameter*, float>> out;
        for (auto* p : parameters)
        {
            auto* param = dynamic_cast<juce::RangedAudioParameter*>(p);
            if (param == nullptr)
                continue;
            const auto id = param->getParameterID();
            const auto it = preset.values.find(id);
            const bool listed = it != preset.values.end();
            const bool sessionButAllowed = listed && (id.startsWith("mod") || (outputStage && isOutputStageRoll(id)));
            if (isSessionParameter(id) ? ! sessionButAllowed : (! listed && unlisted == Unlisted::Keep))
                continue;
            out.emplace_back(param, listed ? param->convertTo0to1(param->getNormalisableRange().snapToLegalValue(it->second)) : param->getDefaultValue());
        }
        return out;
    }

    /** One section's share of a roll (wide where it's safe, narrow where it matters). The Output die also rolls
        Inflate and Push, gently; the header's Randomise leaves those to the session (outputStage false). */
    inline void rollSectionInto(Preset& p, juce::Random& r, int section, bool outputStage)
    {
        using namespace ParamIDs;
        auto u = [&r](float lo, float hi) { return lo + (hi - lo) * r.nextFloat(); };
        switch (section)
        {
            case Ride:
                p.values[energy] = u(0.15f, 0.85f);
                p.values[rate] = u(0.15f, 0.8f);
                p.values[fill] = u(0.05f, 0.4f);
                p.values[chaos] = u(0.2f, 0.7f);
                p.values[pitch] = std::round(u(-7.0f, 7.0f));
                break;
            case BusA:
            case BusB:
            {
                const int b = section == BusA ? 0 : 1;
                p.values[bus[b][Doppler]] = u(0.2f, 0.95f);
                p.values[bus[b][Width]] = u(0.35f, 1.0f);   // never a pass stuck in the middle
                p.values[bus[b][Bend]] = u(0.3f, 0.9f);     // always some bend, never the full scream
                p.values[bus[b][Low]] = u(0.0f, 0.7f);
                p.values[bus[b][Swirl]] = u(0.0f, 0.75f);
                p.values[bus[b][Filter]] = u(0.0f, 0.7f);
                p.values[bus[b][Drive]] = u(0.0f, b == 0 ? 0.48f : 0.22f);
                if (b > 0)
                {
                    p.values[bus[b][Duck]] = u(0.1f, 0.7f);
                    p.values[bus[b][Key]] = u(0.0f, 0.6f);
                }
                break;
            }
            case Hits:
                p.values[hitSense] = u(0.3f, 0.7f);
                p.values[hitBoost] = u(0.0f, 10.0f);
                p.values[hitRelease] = u(0.15f, 0.6f);
                p.values[hitDuck] = u(0.2f, 0.8f);
                break;
            case Output:
                p.values[space] = u(0.05f, 0.6f);
                if (outputStage)
                {
                    p.values[inflate] = u(0.0f, 0.5f);
                    p.values[push] = u(0.0f, 6.0f);
                }
                break;
            case Stowaway:
                // The five knobs, never the Mode: that decides what Stowaway is (and where rumbles play), so only
                // a preset or your own hand changes it. The ranges suit every mode: Filter stays around the
                // middle (the Sub's crossover between about 85 and 165 Hz; open or gently closed otherwise).
                p.values[stow[StowFollow]] = u(0.4f, 1.0f);
                p.values[stow[StowLow]] = u(0.0f, 0.6f);
                p.values[stow[StowFilter]] = u(0.3f, 0.7f);
                p.values[stow[StowDrive]] = u(0.0f, 0.35f);
                p.values[stow[StowWidth]] = u(0.4f, 1.0f);
                break;
            default:
                break;
        }
    }

    /** A random but musical setting for every section that isn't locked (bit n of lockedSections = Section n). */
    inline Preset roll(juce::Random& r, unsigned lockedSections = 0)
    {
        Preset p { "Randomised", {} };
        for (int section = 0; section < numSections; ++section)
            if ((lockedSections & (1u << section)) == 0)
                rollSectionInto(p, r, section, false);
        return p;
    }

    /** A section's own die: just that section, locked or not. */
    inline Preset rollSection(juce::Random& r, int section)
    {
        Preset p { "Randomised", {} };
        rollSectionInto(p, r, section, true);
        return p;
    }
}

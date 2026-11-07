#pragma once

#include <JuceHeader.h>

/** Parameter IDs and the layout, in signal order: tracks -> ride -> bus A / bus B -> hits -> output. */
namespace ParamIDs
{
    // Tracks
    inline constexpr const char* cargoTrack[10] = { "cargo1", "cargo2", "cargo3", "cargo4", "cargo5",
                                                    "cargo6", "cargo7", "cargo8", "cargo9", "cargo10" };
    inline constexpr int numCargoTracks = 10;
    inline constexpr const char* pitch    = "pitch";
    inline constexpr const char* input    = "input";

    // Ride (the faders move by themselves)
    inline constexpr const char* rideOn   = "rideOn";
    inline constexpr const char* energy   = "energy";
    inline constexpr const char* rate     = "rate";
    inline constexpr const char* fill     = "fill";
    inline constexpr const char* chaos    = "chaos";

    // Buses: A (aggressive) and B (smooth). Tracks are routed by their type.
    // Bus A is the key for B, so it has no ducked-by / filtered-by controls.
    inline constexpr int numBuses = 2;
    enum BusControl { Doppler = 0, Width, Bend, Low, Swirl, Filter, Drive, Duck, Key, numBusControls };
    inline constexpr const char* bus[numBuses][numBusControls] = {
        { "aDoppler", "aWidth", "aBend", "aLow", "aSwirl", "aFilter", "aDrive", nullptr, nullptr },
        { "bDoppler", "bWidth", "bBend", "bLow", "bSwirl", "bFilter", "bDrive", "bDuck", "bKey" },
    };
    inline constexpr const char* busNames[numBuses] = { "A", "B" };

    // Hits
    inline constexpr const char* hitSense   = "hitSense";
    inline constexpr const char* hitBoost   = "hitBoost";
    inline constexpr const char* hitRelease = "hitRelease";
    inline constexpr const char* hitDuck    = "hitDuck";
    inline constexpr const char* hitSync    = "hitSync";
    inline constexpr const char* gateMode   = "gateMode";   // Peaks (original) / Level / Jumps / Open
    inline constexpr const char* hitHold    = "hitHold";
    inline constexpr const char* hitFloor   = "hitFloor";
    inline constexpr const char* duckMode   = "duckMode";   // Volume (original) / Darken / Mids
    inline constexpr const char* duckReturn = "duckReturn";
    inline constexpr const char* takeTurns  = "takeTurns";

    // Morph (Zynaptiq-style spectral morph between bus A and bus B)
    inline constexpr const char* morphOn     = "morphOn";
    inline constexpr const char* morphMode   = "morphMode";
    inline constexpr const char* morphX      = "morphX";
    inline constexpr const char* morphY      = "morphY";
    inline constexpr const char* morphWander = "morphWander";
    inline constexpr const char* morphSpeed  = "morphSpeed";
    inline constexpr const char* morphShape  = "morphShape";

    // Output
    inline constexpr const char* space    = "space";
    inline constexpr const char* inflate  = "inflate";
    inline constexpr const char* push     = "push";
    inline constexpr const char* ceiling  = "ceiling";

    // Modulators (the panel at the bottom; wired to knobs by dragging their jacks)
    inline constexpr int numMods = 4;
    inline constexpr const char* modType[numMods]  = { "mod1Type", "mod2Type", "mod3Type", "mod4Type" };
    inline constexpr const char* modShape[numMods] = { "mod1Shape", "mod2Shape", "mod3Shape", "mod4Shape" };
    inline constexpr const char* modKnob[numMods]  = { "mod1Knob", "mod2Knob", "mod3Knob", "mod4Knob" };
    inline constexpr const char* modFollow[numMods] = { "mod1Follow", "mod2Follow", "mod3Follow", "mod4Follow" }; // Follower input
}

namespace Params
{
    inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using namespace juce;
        using P = AudioParameterFloat;
        AudioProcessorValueTreeState::ParameterLayout layout;

        auto pct = [](const String& id, const String& name, float def)
        {
            return std::make_unique<P>(ParameterID { id, 2 }, name, NormalisableRange<float>(0.0f, 1.0f), def,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return String(roundToInt(v * 100.0f)) + "%"; }));
        };

        for (int i = 0; i < ParamIDs::numCargoTracks; ++i)
            layout.add(pct(ParamIDs::cargoTrack[i], "Track " + String(i + 1), 0.8f));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::pitch, 2 }, "Pitch", NormalisableRange<float>(-24.0f, 24.0f), 0.0f,
                                       AudioParameterFloatAttributes().withLabel("st").withStringFromValueFunction(
                                           [](float v, int) { return (v > 0.05f ? "+" : "") + String(v, 1) + " st"; })));
        layout.add(pct(ParamIDs::input, "Input", 0.0f));

        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::rideOn, 2 }, "Auto ride", true));
        layout.add(pct(ParamIDs::energy, "Energy", 0.6f));
        layout.add(pct(ParamIDs::rate, "Rate", 0.4f));
        layout.add(pct(ParamIDs::fill, "Fill", 0.3f));
        layout.add(pct(ParamIDs::chaos, "Chaos", 0.4f));

        const float defaults[ParamIDs::numBuses][ParamIDs::numBusControls] = {
            { 0.55f, 0.8f, 0.7f, 0.35f, 0.2f, 0.3f, 0.15f, 0.0f, 0.0f },
            { 0.4f, 0.8f, 0.6f, 0.2f, 0.3f, 0.4f, 0.0f, 0.4f, 0.3f },
        };
        const char* controlNames[] = { "Doppler", "Width", "Bend", "Low end", "Swirl", "Filter", "Drive", "Ducked by A", "Filtered by A" };
        for (int b = 0; b < ParamIDs::numBuses; ++b)
            for (int k = 0; k < ParamIDs::numBusControls; ++k)
                if (ParamIDs::bus[b][k] != nullptr)
                    layout.add(pct(ParamIDs::bus[b][k], String("Bus ") + ParamIDs::busNames[b] + " " + controlNames[k], defaults[b][k]));

        layout.add(pct(ParamIDs::hitSense, "Hit sensitivity", 0.5f));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::hitBoost, 2 }, "Hit boost", NormalisableRange<float>(0.0f, 18.0f), 12.0f,
                                       AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(
                                           [](float v, int) { return "+" + String(v, 1) + " dB"; })));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::hitRelease, 2 }, "Hit release", NormalisableRange<float>(0.0f, 1.0f), 0.35f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return String(roundToInt(60.0f + v * 1140.0f)) + " ms"; })));
        layout.add(pct(ParamIDs::hitDuck, "Hit duck", 0.5f));
        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::hitSync, 2 }, "Sync hits", false));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::gateMode, 2 }, "Gate mode",
                                                          StringArray { "Peaks", "Level", "Jumps", "Open" }, 0));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::hitHold, 2 }, "Gate hold", NormalisableRange<float>(0.0f, 1.0f), 0.12f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return String(roundToInt(v * 500.0f)) + " ms"; })));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::hitFloor, 2 }, "Gate floor", NormalisableRange<float>(0.0f, 1.0f), 0.0f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return v <= 0.001f ? String("Closed") : String(-40.0f + 37.0f * v, 1) + " dB"; })));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::duckMode, 2 }, "Duck mode",
                                                          StringArray { "Volume", "Darken", "Mids" }, 0));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::duckReturn, 2 }, "Duck return", NormalisableRange<float>(0.0f, 1.0f), 0.0f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return v <= 0.001f ? String("Fast") : String(roundToInt(20.0f + 980.0f * v)) + " ms"; })));
        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::takeTurns, 2 }, "Take turns", false));

        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::morphOn, 2 }, "Morph", false));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::morphMode, 2 }, "Morph mode", StringArray { "Blend", "Glide", "Cross" }, 0));
        layout.add(pct(ParamIDs::morphX, "Morph X", 0.5f));
        layout.add(pct(ParamIDs::morphY, "Morph Y", 0.5f));
        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::morphWander, 2 }, "Morph wander", false));
        layout.add(pct(ParamIDs::morphSpeed, "Morph speed", 0.5f));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::morphShape, 2 }, "Morph shape",
                                                          StringArray { "Lorenz", "Sweet Justice", "Figure eight", "Rose", "Circle" }, 0));

        layout.add(pct(ParamIDs::space, "Space", 0.25f));
        layout.add(pct(ParamIDs::inflate, "Inflate", 0.3f));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::push, 2 }, "Push", NormalisableRange<float>(0.0f, 18.0f), 3.0f,
                                       AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(
                                           [](float v, int) { return "+" + String(v, 1) + " dB"; })));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::ceiling, 2 }, "Ceiling", NormalisableRange<float>(-12.0f, 0.0f), -0.3f,
                                       AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(
                                           [](float v, int) { return String(v, 1) + " dB"; })));

        // Modulators: LFO, LFO, Random, Follower by default.
        const int defaultTypes[ParamIDs::numMods] = { 0, 0, 1, 2 };
        const int defaultShapes[ParamIDs::numMods] = { 0, 1, 0, 0 };
        const float defaultKnobs[ParamIDs::numMods] = { 0.3f, 0.45f, 0.35f, 0.5f };
        for (int m = 0; m < ParamIDs::numMods; ++m)
        {
            const String name = "Mod " + String(m + 1);
            layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::modType[m], 2 }, name + " type",
                                                              StringArray { "LFO", "Random", "Follower", "Macro" }, defaultTypes[m]));
            layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::modShape[m], 2 }, name + " shape",
                                                              StringArray { "Sine", "Triangle", "Ramp", "Square", "Steps" }, defaultShapes[m]));
            layout.add(pct(ParamIDs::modKnob[m], name + " rate / amount", defaultKnobs[m]));
            layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::modFollow[m], 2 }, name + " follows",
                                                              StringArray { "Bus A", "Bus B", "Hits", "Output" }, 0));
        }
        return layout;
    }
}

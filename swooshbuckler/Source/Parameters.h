#pragma once

#include <JuceHeader.h>

/** Parameter IDs and the layout, in signal order: tracks -> ride -> bus A / bus B -> hits -> output. */
namespace ParamIDs
{
    // Tracks
    inline constexpr const char* cargoTrack[10] = { "cargo1", "cargo2", "cargo3", "cargo4", "cargo5",
                                                    "cargo6", "cargo7", "cargo8", "cargo9", "cargo10" };
    inline constexpr int numCargoTracks = 10;
    // Per-track pitch: semitones and cents, and VARI (on: tape-style, the speed follows the pitch;
    // off: the pitch moves and the speed stays, through a pitch shifter).
    inline constexpr const char* trackSemis[10] = { "cargo1Semis", "cargo2Semis", "cargo3Semis", "cargo4Semis", "cargo5Semis",
                                                    "cargo6Semis", "cargo7Semis", "cargo8Semis", "cargo9Semis", "cargo10Semis" };
    inline constexpr const char* trackCents[10] = { "cargo1Cents", "cargo2Cents", "cargo3Cents", "cargo4Cents", "cargo5Cents",
                                                    "cargo6Cents", "cargo7Cents", "cargo8Cents", "cargo9Cents", "cargo10Cents" };
    inline constexpr const char* trackVari[10]  = { "cargo1Vari", "cargo2Vari", "cargo3Vari", "cargo4Vari", "cargo5Vari",
                                                    "cargo6Vari", "cargo7Vari", "cargo8Vari", "cargo9Vari", "cargo10Vari" };
    inline constexpr const char* pitch    = "pitch";

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
    // Pass Mode: Bed (two overlapped passes, a steady level) / Fly-By (one pass at a time). Linked: B uses A's.
    inline constexpr const char* passMode[numBuses] = { "aPassMode", "bPassMode" };

    // Perform: throw one whoosh on demand, the transport, and what MIDI keys do
    inline constexpr const char* throwLength = "throwLength"; // seconds; under Params::throwMinSeconds = follows Rate
    inline constexpr const char* play        = "play";        // the transport, for host automation
    inline constexpr const char* keysMode    = "keysMode";    // Swell / Pitch / Throw

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
    inline constexpr const char* linkBuses  = "linkBuses"; // bus B flies bus A's passes, with A's Doppler and Width
    inline constexpr const char* hitAim     = "hitAim";    // fire the hits so their transients land on the buses' pass peaks

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
    // Peak tremolo: a tremolo that deepens as the level nears its peak.
    inline constexpr const char* masterTrim    = "masterTrim";  // what you hear, after the limiter (the tape is before it)
    inline constexpr const char* tremOn        = "tremOn";
    inline constexpr const char* tremDepth     = "tremDepth";
    inline constexpr const char* tremRate      = "tremRate";
    inline constexpr const char* tremThreshold = "tremThreshold";
    inline constexpr const char* tremShape     = "tremShape";   // Sine / Square / Flutter
    inline constexpr const char* tremTarget    = "tremTarget";  // Everything / Buses A + B / Hits

    // Modulators (the panel at the bottom; wired to knobs by dragging their jacks)
    inline constexpr int numMods = 4;
    inline constexpr const char* modType[numMods]  = { "mod1Type", "mod2Type", "mod3Type", "mod4Type" };
    inline constexpr const char* modShape[numMods] = { "mod1Shape", "mod2Shape", "mod3Shape", "mod4Shape" };
    inline constexpr const char* modKnob[numMods]  = { "mod1Knob", "mod2Knob", "mod3Knob", "mod4Knob" };
    inline constexpr const char* modFollow[numMods] = { "mod1Follow", "mod2Follow", "mod3Follow", "mod4Follow" }; // Follower input
}

namespace Params
{
    /** Track fader: 30 dB of travel in dB, the last 5% fading to silence. */
    inline float faderGain(float k)
    {
        if (k <= 0.0f)
            return 0.0f;
        return std::pow(10.0f, 30.0f * (k - 1.0f) / 20.0f) * std::min(1.0f, k / 0.05f);
    }
    inline juce::String faderText(float k)
    {
        const float g = faderGain(k);
        return g <= 1.0e-5f ? juce::String("-inf dB") : juce::String(20.0f * std::log10(g), 1) + " dB";
    }
    /** Rate: the base swell period (each track rides at 0.7 .. 1.6x it). */
    inline float rideHz(float rate) { return 0.04f + 0.5f * std::pow(std::clamp(rate, 0.0f, 1.0f), 1.6f); }
    inline juce::String ridePeriodText(float rate)
    {
        const float s = 1.0f / rideHz(rate);
        return (s < 10.0f ? juce::String(s, 1) : juce::String(juce::roundToInt(s))) + " s";
    }
    /** A doppler pass's length before Chaos varies it: follows Rate (6 s .. 1.6 s). */
    inline float passSeconds(float rate) { return 6.0f * std::pow(1.6f / 6.0f, std::clamp(rate, 0.0f, 1.0f)); }
    /** Throw Length: the whole thrown pass, 0.3 .. 6 s. Settings under the minimum mean "as long as Rate's passes". */
    inline constexpr float throwMinSeconds = 0.3f, throwMaxSeconds = 6.0f;
    inline float throwSeconds(float setting, float rate)
    {
        return setting < throwMinSeconds ? passSeconds(rate) : std::min(setting, throwMaxSeconds);
    }
    inline float hitFloorDb(float k) { return -28.0f + 25.0f * std::pow(std::clamp(k, 0.0f, 1.0f), 0.6f); }
    inline float hitDuckDb(float k) { return 12.0f * std::clamp(k, 0.0f, 1.0f); }
    /** Tremolo Depth knob -> how far the level dips at the peak (0..1 of the gain). */
    inline float tremDepthAmount(float k) { return 1.0f - std::pow(1.0f - std::clamp(k, 0.0f, 1.0f), 1.6f); }
    /** Tremolo Threshold knob -> the level (against the recent peak) where the shake starts easing in. */
    inline float tremThresholdLevel(float k) { return 0.98f * (1.0f - std::pow(1.0f - std::clamp(k, 0.0f, 1.0f), 1.5f)); }
    /** Energy knob -> how far the ride dips (0..1): the lowest point falls 4 dB per 10%, to -36 dB at 90%, then to silence. */
    inline float energyDepth(float k)
    {
        k = std::clamp(k, 0.0f, 1.0f);
        return 1.0f - std::pow(10.0f, -2.0f * k) * std::min(1.0f, 10.0f * (1.0f - k));
    }

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

        // Track faders: a 30 dB fader law (see Params::faderGain), so every part of the travel balances evenly.
        for (int i = 0; i < ParamIDs::numCargoTracks; ++i)
            layout.add(std::make_unique<P>(ParameterID { ParamIDs::cargoTrack[i], 2 }, "Track " + String(i + 1),
                                           NormalisableRange<float>(0.0f, 1.0f), 0.93f,
                                           AudioParameterFloatAttributes().withStringFromValueFunction(
                                               [](float v, int) { return faderText(v); })));
        for (int i = 0; i < ParamIDs::numCargoTracks; ++i)
        {
            const String track = "Track " + String(i + 1);
            layout.add(std::make_unique<P>(ParameterID { ParamIDs::trackSemis[i], 3 }, track + " Semitones",
                                           NormalisableRange<float>(-24.0f, 24.0f, 1.0f), 0.0f,
                                           AudioParameterFloatAttributes().withLabel("st").withStringFromValueFunction(
                                               [](float v, int) { const int s = roundToInt(v); return (s > 0 ? "+" : "") + String(s) + " st"; })));
            layout.add(std::make_unique<P>(ParameterID { ParamIDs::trackCents[i], 3 }, track + " Cents",
                                           NormalisableRange<float>(-100.0f, 100.0f, 1.0f), 0.0f,
                                           AudioParameterFloatAttributes().withLabel("c").withStringFromValueFunction(
                                               [](float v, int) { const int c = roundToInt(v); return (c > 0 ? "+" : "") + String(c) + " c"; })));
            layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::trackVari[i], 3 }, track + " Vari", true));
        }
        // Finer around the centre (where most settings live), still two octaves either way.
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::pitch, 2 }, "Pitch", NormalisableRange<float>(-24.0f, 24.0f, 0.0f, 1.0f / 1.3f, true), 0.0f,
                                       AudioParameterFloatAttributes().withLabel("st").withStringFromValueFunction(
                                           [](float v, int) { const float st = std::round(v * 10.0f) / 10.0f; return (st > 0.0f ? "+" : "") + String(st == 0.0f ? 0.0f : st, 1) + " st"; })));

        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::rideOn, 2 }, "Auto Ride", true));
        layout.add(pct(ParamIDs::energy, "Energy", 0.35f));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::rate, 2 }, "Rate", NormalisableRange<float>(0.0f, 1.0f), 0.4f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return ridePeriodText(v); })));
        layout.add(pct(ParamIDs::fill, "Fill", 0.15f));
        layout.add(pct(ParamIDs::chaos, "Chaos", 0.4f));

        const float defaults[ParamIDs::numBuses][ParamIDs::numBusControls] = {
            { 0.55f, 0.7f, 0.7f, 0.35f, 0.3f, 0.3f, 0.12f, 0.0f, 0.0f },
            { 0.4f, 0.7f, 0.6f, 0.3f, 0.3f, 0.4f, 0.0f, 0.35f, 0.3f },
        };
        const char* controlNames[] = { "Doppler", "Width", "Bend", "Low End", "Swirl", "Filter", "Drive", "Ducked by A", "Filtered by A" };
        for (int b = 0; b < ParamIDs::numBuses; ++b)
            for (int k = 0; k < ParamIDs::numBusControls; ++k)
                if (ParamIDs::bus[b][k] != nullptr)
                    layout.add(pct(ParamIDs::bus[b][k], String("Bus ") + ParamIDs::busNames[b] + " " + controlNames[k], defaults[b][k]));

        layout.add(pct(ParamIDs::hitSense, "Hit Sensitivity", 0.5f));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::hitBoost, 2 }, "Hit Boost", NormalisableRange<float>(-3.0f, 12.0f), 6.0f,
                                       AudioParameterFloatAttributes().withLabel("dB")
                                           .withStringFromValueFunction([](float v, int)
                                           {
                                               // Round first so values just under 0 read "+0.0", not "-0.0".
                                               const float db = std::round(v * 10.0f) / 10.0f;
                                               return (db >= 0.0f ? "+" : "") + String(db == 0.0f ? 0.0f : db, 1) + " dB";
                                           })
                                           .withValueFromStringFunction([](const String& t) { return t.retainCharacters("-+.0123456789").getFloatValue(); })));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::hitRelease, 2 }, "Hit Release", NormalisableRange<float>(0.0f, 1.0f), 0.35f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return String(roundToInt(60.0f + v * 1140.0f)) + " ms"; })));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::hitDuck, 2 }, "Hit Duck", NormalisableRange<float>(0.0f, 1.0f), 0.4f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return v <= 0.001f ? String("Off") : "-" + String(hitDuckDb(v), 1) + " dB"; })));
        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::hitSync, 2 }, "Sync Hits", false));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::gateMode, 2 }, "Gate Mode",
                                                          StringArray { "Peaks", "Level", "Jumps", "Open" }, 0));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::hitHold, 2 }, "Gate Hold", NormalisableRange<float>(0.0f, 1.0f), 0.12f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return String(roundToInt(v * 500.0f)) + " ms"; })));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::hitFloor, 2 }, "Gate Floor", NormalisableRange<float>(0.0f, 1.0f), 0.0f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return v <= 0.001f ? String("Closed") : String(hitFloorDb(v), 1) + " dB"; })));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::duckMode, 2 }, "Duck Mode",
                                                          StringArray { "Volume", "Darken", "Mids" }, 0));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::duckReturn, 2 }, "Duck Return", NormalisableRange<float>(0.0f, 1.0f), 0.0f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return v <= 0.001f ? String("Fast") : String(roundToInt(20.0f + 980.0f * v)) + " ms"; })));
        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::takeTurns, 2 }, "Take Turns", false));
        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::linkBuses, 3 }, "Link Buses", false));
        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::hitAim, 3 }, "Aim Hits", true));

        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::morphOn, 2 }, "Morph", false));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::morphMode, 2 }, "Morph Mode", StringArray { "Blend", "Glide", "Cross" }, 0));
        layout.add(pct(ParamIDs::morphX, "Morph X", 0.5f));
        layout.add(pct(ParamIDs::morphY, "Morph Y", 0.5f));
        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::morphWander, 2 }, "Morph Wander", false));
        layout.add(pct(ParamIDs::morphSpeed, "Morph Speed", 0.5f));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::morphShape, 2 }, "Morph Shape",
                                                          StringArray { "Lorenz", "Cherries", "Figure Eight", "Rose", "Circle", "Butterfly" }, 0));

        layout.add(pct(ParamIDs::space, "Space", 0.3f));
        layout.add(pct(ParamIDs::inflate, "Inflate", 0.3f));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::push, 2 }, "Push", NormalisableRange<float>(0.0f, 15.0f), 3.0f,
                                       AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(
                                           [](float v, int) { return "+" + String(v, 1) + " dB"; })));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::ceiling, 2 }, "Ceiling", NormalisableRange<float>(-6.0f, 0.0f, 0.0f, 2.0f), -0.3f,
                                       AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(
                                           [](float v, int) { return String(v, v > -1.0f ? 2 : 1) + " dB"; })));

        layout.add(std::make_unique<P>(ParameterID { ParamIDs::masterTrim, 3 }, "Master Trim", NormalisableRange<float>(-60.0f, 0.0f, 0.0f, 2.5f), 0.0f,
                                       AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(
                                           [](float v, int) { return v <= -59.9f ? String("-inf dB") : String(v, 1) + " dB"; })));

        // Peak tremolo (off by default).
        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::tremOn, 3 }, "Tremolo", false));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::tremDepth, 3 }, "Tremolo Depth", NormalisableRange<float>(0.0f, 1.0f), 0.45f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return v >= 0.999f ? String("Full") : String(20.0f * std::log10(1.0f - tremDepthAmount(v)), 1) + " dB"; })));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::tremRate, 3 }, "Tremolo Rate", NormalisableRange<float>(0.0f, 1.0f), 0.5f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { const float hz = 1.5f * std::pow(20.0f, v); return (hz < 10.0f ? String(hz, 1) : String(roundToInt(hz))) + " Hz"; })));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::tremThreshold, 3 }, "Tremolo Threshold", NormalisableRange<float>(0.0f, 1.0f), 0.45f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return v < 0.005f ? String("All") : String(20.0f * std::log10(tremThresholdLevel(v)), 1) + " dB"; })));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::tremShape, 3 }, "Tremolo Shape",
                                                          StringArray { "Sine", "Square", "Flutter" }, 0));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::tremTarget, 3 }, "Tremolo Shakes",
                                                          StringArray { "Everything", "Buses A + B", "Hits" }, 0));

        // Modulators: LFO, LFO, Random, Follower by default.
        const int defaultTypes[ParamIDs::numMods] = { 0, 0, 1, 2 };
        const int defaultShapes[ParamIDs::numMods] = { 0, 1, 0, 0 };
        const float defaultKnobs[ParamIDs::numMods] = { 0.35f, 0.5f, 0.4f, 0.5f };
        for (int m = 0; m < ParamIDs::numMods; ++m)
        {
            const String name = "Mod " + String(m + 1);
            layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::modType[m], 2 }, name + " Type",
                                                              StringArray { "LFO", "Random", "Follower", "Macro" }, defaultTypes[m]));
            layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::modShape[m], 2 }, name + " Shape",
                                                              StringArray { "Sine", "Triangle", "Ramp", "Square", "Steps" }, defaultShapes[m]));
            layout.add(pct(ParamIDs::modKnob[m], name + " Rate / Amount", defaultKnobs[m]));
            layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::modFollow[m], 2 }, name + " Follows",
                                                              StringArray { "Bus A", "Bus B", "Hits", "Output" }, 0));
        }

        // Added later: kept at the end, so every parameter before them keeps its place in the host's list.
        for (int b = 0; b < ParamIDs::numBuses; ++b)
            layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::passMode[b], 4 }, String("Bus ") + ParamIDs::busNames[b] + " Pass Mode",
                                                              StringArray { "Bed", "Fly-By" }, 0));
        // Throw Length: the bottom of the travel is "Rate" (a throw is as long as Rate's passes), then 0.3 .. 6 s.
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::throwLength, 4 }, "Throw Length",
                                       NormalisableRange<float>(0.0f, throwMaxSeconds, 0.0f, 0.6f), 0.0f,
                                       AudioParameterFloatAttributes().withLabel("s")
                                           .withStringFromValueFunction([](float v, int) { return v < throwMinSeconds ? String("Rate") : String(v, v < 1.0f ? 2 : 1) + " s"; })
                                           .withValueFromStringFunction([](const String& t)
                                           {
                                               const float v = t.retainCharacters(".0123456789").getFloatValue();
                                               return v < throwMinSeconds ? 0.0f : v;
                                           })));
        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::play, 4 }, "Play", false));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::keysMode, 4 }, "Keys",
                                                          StringArray { "Swell", "Pitch", "Throw" }, 0));
        return layout;
    }
}

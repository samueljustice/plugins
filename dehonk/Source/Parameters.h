#pragma once

#include <JuceHeader.h>

#include "DeHonkCore.h"

#include <array>
#include <cmath>

/** Parameter IDs and ranges (SPEC section 4 and 12). IDs never change: sessions, presets and host
    automation address parameters by them. The order of createParameterLayout() is the order the
    host sees (Logic addresses AU parameters by index). Nothing has been released yet, so that order
    may still change; once it has been, a parameter added later goes at the end. */
namespace ParamIDs
{
    inline constexpr const char* threshold  = "threshold";
    inline constexpr const char* depth      = "depth";
    inline constexpr const char* sharpness  = "sharpness";
    inline constexpr const char* speed      = "speed";
    inline constexpr const char* woofOn     = "woofOn";
    inline constexpr const char* tubOn      = "tubOn";
    inline constexpr const char* fogOn      = "fogOn";
    inline constexpr const char* barkOn     = "barkOn";
    inline constexpr const char* snapOn    = "snapOn";
    inline constexpr const char* edge0      = "edge0";
    inline constexpr const char* edge1      = "edge1";
    inline constexpr const char* edge2      = "edge2";
    inline constexpr const char* edge3      = "edge3";
    inline constexpr const char* edge4      = "edge4";
    inline constexpr const char* link       = "link";
    inline constexpr const char* lookahead  = "lookahead";
    inline constexpr const char* mix        = "mix";
    inline constexpr const char* trim       = "trim";
    inline constexpr const char* bypass     = "bypass";
    inline constexpr const char* maxCut     = "maxCut";
    inline constexpr const char* release    = "release";
    inline constexpr const char* impactGuard = "impactGuard";

    /** Indexed by dehonk::Module and by edge number (module m spans edge m to edge m + 1). */
    inline constexpr const char* moduleOn[dehonk::kNumModules] { woofOn, tubOn, fogOn, barkOn };
    inline constexpr const char* edges[dehonk::kNumEdges] { edge0, edge1, edge2, edge3, edge4 };

    inline constexpr int versionHint = 1;
}

namespace ParamRanges
{
    inline constexpr dehonk::Params coreDefaults {};

    inline constexpr float thresholdMin = -60.0f, thresholdMax = 0.0f;
    inline constexpr float thresholdDefault = coreDefaults.thresholdDb;
    /** At the bottom of its range the threshold is off: De-Honk always acts. The core treats any
        value at or below thresholdAlwaysAtOrBelow that way, so the text shows "Always" there too. */
    inline constexpr float thresholdAlways = thresholdMin;
    inline constexpr float thresholdAlwaysAtOrBelow = -59.9f;

    inline constexpr float sharpnessDefault = coreDefaults.sharpness * 100.0f;   // percent

    inline constexpr float maxCutMin = 3.0f, maxCutMax = 12.0f;                  // dB
    inline constexpr float maxCutDefault = coreDefaults.maxCutDb;
    inline constexpr float releaseMin = 50.0f, releaseMax = 1500.0f;             // milliseconds
    inline constexpr float releaseDefault = coreDefaults.releaseMs;
    inline constexpr float impactGuardDefault = coreDefaults.impactGuard * 100.0f;   // percent

    /** Release, logarithmic: equal distances on the control are equal ratios of time. Whole
        milliseconds up to 100, then tens. */
    inline juce::NormalisableRange<float> releaseRange()
    {
        return { releaseMin, releaseMax,
                 [] (float start, float end, float t) { return start * std::pow (end / start, t); },
                 [] (float start, float end, float v)
                 { return std::log (juce::jlimit (start, end, v) / start) / std::log (end / start); },
                 [] (float start, float end, float v)
                 { return juce::jlimit (start, end, v < 100.0f ? std::round (v) : std::round (v / 10.0f) * 10.0f); } };
    }

    inline constexpr const char* moduleNames[dehonk::kNumModules] { "Woof", "Tub", "Fog", "Bark" };
    inline constexpr const char* edgeNames[dehonk::kNumEdges] { "Woof low", "Woof / Tub", "Tub / Fog", "Fog / Bark", "Bark high" };

    /** Neighbouring edges never come closer than this ratio (the core enforces the same). */
    inline constexpr float minEdgeRatio = 1.1f;

    /** The edges as the core uses them: each clamped to its own range, then pushed up to at least
        minEdgeRatio above the one below (the same pass as the core). Automation, presets or saved
        state can set neighbours closer than that; the parameters keep those values, and anything
        that shows an edge shows these. */
    inline std::array<float, dehonk::kNumEdges> edgesAsUsed (const std::array<float, dehonk::kNumEdges>& raw) noexcept
    {
        std::array<float, dehonk::kNumEdges> used {};
        for (size_t e = 0; e < used.size(); ++e)
        {
            used[e] = juce::jlimit (dehonk::kEdgeMinHz[e], dehonk::kEdgeMaxHz[e], raw[e]);
            if (e > 0)
                used[e] = juce::jmax (used[e], used[e - 1] * minEdgeRatio);
        }
        return used;
    }

    /** The Lookahead choices, in milliseconds, in the order of the choice parameter. */
    inline constexpr float lookaheadMs[] { 0.0f, 5.0f, 10.0f, 20.0f };
    inline constexpr int numLookaheadChoices = 4;

    inline float lookaheadMsForIndex (int index) noexcept
    {
        return lookaheadMs[juce::jlimit (0, numLookaheadChoices - 1, index)];
    }

    /** Edge e's range, logarithmic: equal distances on a control are equal frequency ratios. */
    inline juce::NormalisableRange<float> edgeRange (int e)
    {
        const float lo = dehonk::kEdgeMinHz[e], hi = dehonk::kEdgeMaxHz[e];
        return { lo, hi,
                 [] (float start, float end, float t) { return start * std::pow (end / start, t); },
                 [] (float start, float end, float v)
                 { return std::log (juce::jlimit (start, end, v) / start) / std::log (end / start); },
                 [] (float start, float end, float v) { return juce::jlimit (start, end, std::round (v)); } };
    }
}

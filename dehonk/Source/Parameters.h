#pragma once

#include <JuceHeader.h>

#include "DeHonkCore.h"

#include <array>
#include <cmath>

/** Parameter IDs and ranges (SPEC section 4, 12 and 13.17). IDs never change: sessions, presets and
    host automation address parameters by them.

    Logic and GarageBand address AU parameters by index, and JUCE's AU wrapper orders them by
    version hint first and then by a hash of the ID, not by the order of createParameterLayout().
    So a parameter added after a release must carry a higher version hint than every parameter
    that release had (it then comes after all of them), and a parameter a release had is never
    taken out. The first test build's parameters have hint 1 (versionHint); everything added for
    the second has hint 2 (versionHintSecond).

    "link" (a percentage) was replaced by "linkMode" (a choice) after the first test build. "link"
    is still a parameter, for its place in that order only: no control sets it, a host cannot
    automate it, the core does not read it and presets leave it out. A session or preset from the
    first test build is read as Linked from 50 % up and Unlinked below (legacyLinkMode()). */
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
    inline constexpr const char* linkMode   = "linkMode";
    inline constexpr const char* legacyLink = "link";        // kept for its place in the host's order only (see above)
    inline constexpr const char* lookahead  = "lookahead";
    inline constexpr const char* mix        = "mix";
    inline constexpr const char* trim       = "trim";
    inline constexpr const char* bypass     = "bypass";
    inline constexpr const char* maxCut     = "maxCut";
    inline constexpr const char* release    = "release";
    inline constexpr const char* impactGuard = "impactGuard";
    inline constexpr const char* dialogue   = "dialogue";
    inline constexpr const char* dialogueAmount = "dialogueAmount";
    inline constexpr const char* woofTimingLocked = "woofTimingLocked";
    inline constexpr const char* woofAttack  = "woofAttack";
    inline constexpr const char* woofRelease = "woofRelease";
    inline constexpr const char* tubTimingLocked = "tubTimingLocked";
    inline constexpr const char* tubAttack   = "tubAttack";
    inline constexpr const char* tubRelease  = "tubRelease";
    inline constexpr const char* fogTimingLocked = "fogTimingLocked";
    inline constexpr const char* fogAttack   = "fogAttack";
    inline constexpr const char* fogRelease  = "fogRelease";
    inline constexpr const char* barkTimingLocked = "barkTimingLocked";
    inline constexpr const char* barkAttack  = "barkAttack";
    inline constexpr const char* barkRelease = "barkRelease";
    inline constexpr const char* detection  = "detection";

    /** Indexed by dehonk::Module and by edge number (module m spans edge m to edge m + 1). */
    inline constexpr const char* moduleOn[dehonk::kNumModules] { woofOn, tubOn, fogOn, barkOn };
    inline constexpr const char* edges[dehonk::kNumEdges] { edge0, edge1, edge2, edge3, edge4 };
    /** Each range's own timing: the lock (on = the range follows Speed and Release), and the
        attack and release it uses when unlocked. */
    inline constexpr const char* timingLocked[dehonk::kNumModules] { woofTimingLocked, tubTimingLocked, fogTimingLocked, barkTimingLocked };
    inline constexpr const char* rangeAttack[dehonk::kNumModules] { woofAttack, tubAttack, fogAttack, barkAttack };
    inline constexpr const char* rangeRelease[dehonk::kNumModules] { woofRelease, tubRelease, fogRelease, barkRelease };

    inline constexpr int versionHint = 1;         // the parameters of the first test build
    inline constexpr int versionHintSecond = 2;   // those added for the second
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

    /** A range's own attack, when its timing is unlocked: 2 to 100 ms, logarithmic (Speed covers
        40 ms at Slow down to 5 ms at Fast). Tenths of a millisecond up to 10, then whole ones. */
    inline constexpr float rangeAttackMin = 2.0f, rangeAttackMax = 100.0f;
    inline constexpr float rangeAttackDefault = coreDefaults.rangeAttackMs[0];     // 14 ms: Speed at 50
    inline constexpr float rangeReleaseDefault = coreDefaults.rangeReleaseMs[0];   // 400 ms; the range is Release's
    inline juce::NormalisableRange<float> rangeAttackRange()
    {
        return { rangeAttackMin, rangeAttackMax,
                 [] (float start, float end, float t) { return start * std::pow (end / start, t); },
                 [] (float start, float end, float v)
                 { return std::log (juce::jlimit (start, end, v) / start) / std::log (end / start); },
                 [] (float start, float end, float v)
                 { return juce::jlimit (start, end, v < 10.0f ? std::round (v * 10.0f) / 10.0f : std::round (v)); } };
    }

    /** Link, in the order of the choice parameter (and of dehonk::LinkMode). */
    inline constexpr const char* linkModeNames[] { "Linked", "Grouped", "Unlinked" };
    inline constexpr int numLinkModes = 3;
    inline constexpr int linkModeDefault = 0;
    /** What the Link percentage of the first test build becomes. */
    inline int legacyLinkMode (float linkPercent) noexcept { return linkPercent >= 50.0f ? 0 : 2; }

    /** Detection, in the order of the choice parameter (and of dehonk::Detection). */
    inline constexpr const char* detectionNames[] { "Fast", "Steady" };
    inline constexpr int numDetectionModes = 2;
    inline constexpr int detectionDefault = 0;

    inline constexpr float dialogueAmountDefault = coreDefaults.dialogueAmount * 100.0f;   // percent

    inline constexpr const char* moduleNames[dehonk::kNumModules] { "Woof", "Tub", "Fog", "Bark" };
    /** The host's names for the range switches and the edges. */
    inline constexpr const char* moduleOnNames[dehonk::kNumModules] { "Woof On", "Tub On", "Fog On", "Bark On" };
    inline constexpr const char* edgeNames[dehonk::kNumEdges] { "Edge Woof Low", "Edge Woof/Tub", "Edge Tub/Fog", "Edge Fog/Bark", "Edge Bark High" };

    /** Attack (the "speed" parameter, 0..100): the attack time a setting gives, in milliseconds,
        on the core's curve (DeHonkCore.cpp): 40 ms at 0 down to 5 ms at 100, in equal ratios.
        The stored value keeps its meaning (higher is quicker); the window and the host show the time. */
    inline double attackMsForSpeed (double speedPercent) noexcept
    {
        const dehonk::Tuning tune;
        const double s = juce::jlimit (0.0, 1.0, speedPercent * 0.01);
        return (double) tune.attackSlowMs * std::pow ((double) tune.attackFastMs / (double) tune.attackSlowMs, s);
    }

    /** The setting (0..100) that gives an attack of ms, clamped to what the control reaches. */
    inline double speedForAttackMs (double ms) noexcept
    {
        const dehonk::Tuning tune;
        const double slow = tune.attackSlowMs, fast = tune.attackFastMs;
        const double clamped = juce::jlimit (fast, slow, ms);
        return 100.0 * std::log (clamped / slow) / std::log (fast / slow);
    }

    /** An attack time as the window and the host show it: tenths of a millisecond under 10. */
    inline juce::String attackText (double ms)
    {
        return (ms < 9.95 ? juce::String (ms, 1) : juce::String (juce::roundToInt (ms))) + " ms";
    }

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

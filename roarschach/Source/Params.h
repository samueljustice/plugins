#pragma once

#include <array>

// Parameter IDs, ranges and defaults: the single source of truth for the processor, the editor,
// the presets and the tests. Append-only: never remove, rename or reorder (Logic addresses AU
// parameters by index).
namespace Params
{
    inline constexpr int versionHint = 1;

    inline constexpr const char* pitch = "pitch";
    inline constexpr const char* size = "size";
    inline constexpr const char* growl = "growl";
    inline constexpr const char* liquid = "liquid";
    inline constexpr const char* roar = "roar";
    inline constexpr const char* sub = "sub";
    inline constexpr const char* detailLow = "detailLow";
    inline constexpr const char* detailHigh = "detailHigh";
    inline constexpr const char* gravel = "gravel";
    inline constexpr const char* mix = "mix";
    inline constexpr const char* detailSlot = "detailSlot";
    inline constexpr const char* vocoderSlot = "vocoderSlot";
    inline constexpr const char* vocoderOctave = "vocoderOctave";

    // Pitch in semitones.
    inline constexpr float pitchMin = -12.0f, pitchMax = 12.0f, pitchDefault = -2.0f;

    // Percent parameters (0..100) and their defaults.
    inline constexpr float sizeDefault = 35.0f;
    inline constexpr float growlDefault = 30.0f;
    inline constexpr float liquidDefault = 30.0f;
    inline constexpr float roarDefault = 35.0f;
    inline constexpr float subDefault = 25.0f;
    inline constexpr float detailLowDefault = 30.0f;
    inline constexpr float detailHighDefault = 20.0f;
    inline constexpr float gravelDefault = 0.0f;
    inline constexpr float mixDefault = 100.0f;

    // Where a movable module sits in the chain. Slot n is the gap after the n-th fixed stage:
    // CLEAN > [0] > PITCH > [1] > GROWL > [2] > LIQUID > [3] > ROAR > [4] > SUB > [5] > FINISH
    inline constexpr int numSlots = 6;
    inline constexpr std::array<const char*, numSlots> slotNames {
        "Start", "After Pitch", "After Growl", "After Liquid", "After Roar", "End"
    };
    inline constexpr int detailSlotDefault = 0;
    inline constexpr int vocoderSlotDefault = 0;

    inline constexpr std::array<const char*, 2> octaveNames { "-1 Oct", "-2 Oct" };
    inline constexpr int vocoderOctaveDefault = 0;
}

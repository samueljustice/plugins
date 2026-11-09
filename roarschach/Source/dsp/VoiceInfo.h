#pragma once

namespace roar
{
    // Pitch of the voice at some point in the chain, shared by the stages that track it.
    struct VoiceInfo
    {
        float f0Hz = 0.0f;        // 0 when no pitch has been found yet (stages hold their last pitch)
        float confidence = 0.0f;  // 0..1
        bool voiced = false;
    };
}

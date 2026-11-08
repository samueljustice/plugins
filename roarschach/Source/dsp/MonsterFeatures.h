#pragma once

#include <array>
#include <atomic>

namespace roar
{
    // Features the audio thread publishes once per chunk for the visualiser. Every field is a
    // relaxed atomic: the GL thread reads them in renderOpenGL, a torn frame between fields is fine.
    struct MonsterFeatures
    {
        static constexpr int numBands = 32;

        std::atomic<float> inputLevel { 0.0f };   // 0..1 linear, smoothed
        std::atomic<float> outputLevel { 0.0f };  // 0..1 linear, smoothed
        std::atomic<float> jaw { 0.0f };          // 0..1 fast envelope of the input (5 ms / 120 ms)
        std::atomic<float> f0Hz { 0.0f };
        std::atomic<float> voiced { 0.0f };       // 0 or 1
        std::atomic<float> growlMod { 0.0f };     // -1..1, current growl modulation, low-passed
        std::atomic<float> liquidPhase { 0.0f };  // 0..1
        std::atomic<float> roarDrive { 0.0f };    // 0..1 effective drive
        std::atomic<float> subLevel { 0.0f };     // 0..1
        std::atomic<float> detailBoostDb { 0.0f };
        std::atomic<float> detailCutDb { 0.0f };
        std::atomic<float> gravelActive { 0.0f }; // 0 or 1
        std::array<std::atomic<float>, numBands> vocoderBands {};  // 0..1 per band
        std::atomic<int> generation { 0 };        // bumped per chunk

        MonsterFeatures()
        {
            for (auto& b : vocoderBands)
                b.store (0.0f, std::memory_order_relaxed);
        }
    };
}

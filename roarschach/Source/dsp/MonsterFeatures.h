#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace roar
{
    // Features the audio thread publishes once per chunk for the visualiser and the strip readouts.
    // Every field is a relaxed atomic: the GL and message threads read them, a torn frame between
    // fields is fine (a hit's count and strength are the exception: publishHit). Modules write
    // their own fields in Module::publishFeatures(), scaled by their On fade, so an Off module
    // reads 0 (the views smooth that over 200 ms).
    struct MonsterFeatures
    {
        static constexpr int numBands = 32;
        static constexpr int numOvertoneHarmonics = 5;
        static constexpr int numOvertoneBands = 2;

        // IN / chain
        std::atomic<float> inputLevel { 0.0f };    // 0..1 linear, smoothed
        std::atomic<float> outputLevel { 0.0f };   // 0..1 linear, smoothed
        std::atomic<float> jaw { 0.0f };           // 0..1 fast envelope of the input (5 ms / 120 ms)
        std::atomic<float> jawFast { 0.0f };       // 0..1 the same mapping, 5 ms / 30 ms (the jaw's phrase and lip detail)
        std::atomic<float> f0Hz { 0.0f };          // IN's VoiceStream
        std::atomic<float> voiced { 0.0f };        // 0 or 1

        // Modules
        std::atomic<float> gateOpen { 0.0f };      // CLEAN: 0 closed .. 1 open
        std::atomic<float> cleanGrDb { 0.0f };     // CLEAN: Contain's gain reduction (leveller + catcher, before makeup), dB (<= 0)
        std::atomic<float> deessGrDb { 0.0f };     // DE-ESS gain reduction, dB (<= 0)
        std::atomic<float> detailBoostDb { 0.0f }; // DETAIL upward gain, dB (>= 0)
        std::atomic<float> detailCutDb { 0.0f };   // DETAIL downward gain, dB (<= 0)
        std::atomic<float> gravelActive { 0.0f };  // VOCODER: 0 or 1
        std::array<std::atomic<float>, numBands> vocoderBands {};  // 0..1 per band
        std::atomic<float> shimmerPhase { 0.0f };  // SHIMMER: 0..1, the shift oscillator at its rate
        std::atomic<float> growlMod { 0.0f };      // GROWL: the actual gain modulation, -1..1
        std::atomic<float> swirlNotch { 0.0f };    // SWIRL: 0..1, log position of the first notch
        std::atomic<float> roarHeat { 0.0f };      // ROAR: 0..1, VU-ballistic heat
        std::atomic<float> ensembleWidth { 0.0f }; // ENSEMBLE: 0..1, from the output correlation
        std::atomic<float> tameGrDb { 0.0f };      // TAME: max band gain reduction, dB (<= 0)
        std::atomic<float> subLevel { 0.0f };      // SUB: 0..1
        std::atomic<float> subHz { 0.0f };         // SUB: frequency of the octave layer
        std::atomic<float> overtoneLevel { 0.0f }; // OVERTONE: generated layer re input, 0..1
        std::array<std::atomic<float>, numOvertoneHarmonics> overtoneHarm {};  // OVERTONE: H2..H6 drive, 0..1
        std::atomic<float> overtoneReplace { 0.0f };  // OVERTONE: 1 in Replace mode (scaled by the On fade)
        std::array<std::atomic<float>, numOvertoneBands> overtoneBandHz {};     // OVERTONE: band centres, Hz (0 = band off)
        std::array<std::atomic<float>, numOvertoneBands> overtoneBandLevel {};  // OVERTONE: band level, 0..1 (-24..+6 dB)
        std::atomic<float> ringAmount { 0.0f };    // RING: 0..1, scaled by the On fade
        std::atomic<float> ringHz { 0.0f };        // RING: the carrier frequency in effect
        std::atomic<int> ringBody { 0 };           // RING: 0 Off, 1 Box, 2 Can
        std::atomic<float> ringBodyMix { 0.0f };   // RING: body mix in effect, 0..1 (On fade scaled)
        std::atomic<float> pitchSt { 0.0f };       // PITCH: applied shift in semitones (0 when Off)
        std::atomic<float> chamberWet { 0.0f };    // CHAMBER: the wet weight in effect, 0..1 (On fade scaled)
        std::atomic<float> chamberRing { 0.0f };   // CHAMBER: how strongly the room rings right now, 0..1
        std::atomic<float> chamberEdge { 0.0f };   // CHAMBER: the speaker's breakup re its input, 0..1
        std::atomic<float> chamberBoom { 0.0f };   // CHAMBER: the Boom shelf in effect / 9 dB, 0..1
        std::atomic<float> chamberResonance { 0.0f }; // CHAMBER: the Resonance knob in effect, 0..1 (On fade scaled)
        std::atomic<float> chamberDecay { 0.0f };  // CHAMBER: the decay in effect, seconds (0 when Off)
        std::atomic<int> chamberRoom { 0 };        // CHAMBER: the room (Params::ChamberRoom)

        // OUTPUT
        std::atomic<float> limiterGrDb { 0.0f };   // maximizer gain reduction, dB (<= 0)
        std::atomic<float> pushDb { 0.0f };        // the Push setting in effect, dB
        std::atomic<float> clipDb { 0.0f };        // clipper activity: peak dB flattened per chunk (>= 0)
        // Hits in the output (HitDetector; the picture's ripples). hits is the count so far (wraps
        // at hitCountMask); hitLog keeps the last few hits, each packed with its own count, so a
        // reader never pairs a count with another hit's strength (publishHit, strongestHitSince).
        static constexpr int hitCountMask = 0x3fffffff;
        static constexpr int numHitLog = 4;
        std::atomic<int> hits { 0 };
        std::array<std::atomic<std::uint32_t>, numHitLog> hitLog {};
        std::atomic<float> rumble { 0.0f };        // the output's low end, 0..1 (a fine tremble while it is sustained)

        // Bit m (Params::ModuleId order) set while module m is On.
        std::atomic<int> moduleOnMask { 0 };
        // The order the audio is running in (packed 4 bits per slot, slot 0 lowest), and the
        // stereo split slot (numModules = all mono). During a reorder fade: the incoming order.
        std::atomic<std::uint64_t> orderInUse { 0 };
        std::atomic<int> stereoSplit { 0 };
        std::atomic<int> generation { 0 };         // bumped per chunk
        // The host is bypassing the plug-in (processBlockBypassed). The chain keeps running
        // underneath so that leaving bypass is seamless, and keeps publishing; the views read every
        // value above as silence while this is set.
        std::atomic<bool> hostBypassed { false };

        /** Audio thread: hit number count (already counted) had this strength. Log first, count last. */
        void publishHit (int count, float strength) noexcept
        {
            count &= hitCountMask;
            hitLog[static_cast<size_t> (count % numHitLog)].store (packHit (count, strength), std::memory_order_relaxed);
            hits.store (count, std::memory_order_release);
        }

        /** Hits counted after `last` up to `now` (a count read from `hits` first): the strongest of
            those still in the log, 0..1 (0 when none moved or every one was overwritten). */
        float strongestHitSince (int last, int now) const noexcept
        {
            const int moved = (now - last) & hitCountMask;
            float strongest = 0.0f;
            for (int k = 0; k < std::min (moved, numHitLog); ++k)
            {
                const int count = (now - k) & hitCountMask;
                const auto v = hitLog[static_cast<size_t> (count % numHitLog)].load (std::memory_order_acquire);
                if (static_cast<int> (v >> c_strengthBits) == (count & c_packedCountMask))
                    strongest = std::max (strongest, static_cast<float> (v & c_strengthMax) / static_cast<float> (c_strengthMax));
            }
            return strongest;
        }

        static std::uint32_t packHit (int count, float strength) noexcept
        {
            const float s = strength >= 0.0f ? std::min (strength, 1.0f) : 0.0f;   // (NaN: 0)
            const auto q = static_cast<std::uint32_t> (s * static_cast<float> (c_strengthMax) + 0.5f);
            return (static_cast<std::uint32_t> (count & c_packedCountMask) << c_strengthBits) | q;
        }

        static constexpr int c_strengthBits = 10;
        static constexpr std::uint32_t c_strengthMax = (1u << c_strengthBits) - 1u;
        static constexpr int c_packedCountMask = (1 << (32 - c_strengthBits)) - 1;

        MonsterFeatures()
        {
            for (auto& b : vocoderBands)
                b.store (0.0f, std::memory_order_relaxed);
            for (auto& h : overtoneHarm)
                h.store (0.0f, std::memory_order_relaxed);
            for (auto& b : overtoneBandHz)
                b.store (0.0f, std::memory_order_relaxed);
            for (auto& b : overtoneBandLevel)
                b.store (0.0f, std::memory_order_relaxed);
            for (auto& h : hitLog)
                h.store (0, std::memory_order_relaxed);
        }
    };
}

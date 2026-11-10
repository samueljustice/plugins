#pragma once

#include "Params.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

// The dice: where a roll may put each module control. Plain C++ (no JUCE), next to Params.h so the
// table is checked against the parameter table at compile time.
//
// A knob rolls inside its audited useful range (docs/parameter-audit.md: the "Usable" column and
// the sweet-spot guide), never over its raw 0..1 travel, so a roll never lands in a dead or harmful
// zone. Hertz, ratio and millisecond knobs roll evenly over their knob travel (their log taper), the
// others evenly over their values. A choice rolls uniformly among the options in its mask.
//
// What a roll never touches: the module positions (the order), IN, and OUTPUT (Push, Ceiling, the
// maximizer, Mix), so the level safety and the dry/wet balance stay as they are.
namespace Params::Random
{
    struct Range
    {
        const char* id;
        float lo;                  // knobs: real values, lo <= hi, inside the parameter's range
        float hi;
        std::uint32_t choices;     // choices: a bit per allowed option (bit i = option i); 0 for knobs
    };

    /** Every option of a choice with n options. */
    constexpr std::uint32_t all (int n) { return (1u << n) - 1u; }

    /** How many modules ROLL ALL turns on (the rest are turned off). */
    inline constexpr int minModulesOn = 3, maxModulesOn = 7;

    /** The modules that only clean up or level the voice. ROLL ALL turns on at least
        minCharacterModules of the others, so every roll changes the voice audibly. */
    inline constexpr std::array<ModuleId, 4> utilityModules { ModuleId::clean, ModuleId::deess, ModuleId::detail, ModuleId::tame };
    inline constexpr int minCharacterModules = 2;

    inline constexpr std::array<Range, 56> ranges {
        // PITCH: Shift and Snap: a bigger human to a monster (-2..-14 st: below -15 the words blur in
        // Follow and Keep buzzes). Mono makes the knob a note (0 st = C3), so a Mono roll is a
        // fixed-note drone whatever the speaker's pitch: a whole note from A#1 to A#2 (see
        // adjustModule). Size and Drive up to grit (warmth to 25 %, grit 35-70 %).
        Range { pitch, -14.0f, -2.0f, 0 },
        Range { pitchSize, 0.0f, 60.0f, 0 },
        Range { pitchDrive, 0.0f, 60.0f, 0 },
        Range { pitchMode, 0, 0, all (3) },
        Range { pitchFormant, 0, 0, all (2) },
        // CLEAN: Contain is dead below 20 %; Gate's sweet spot for dialogue is 25-40 %.
        Range { cleanContain, 20.0f, 80.0f, 0 },
        Range { cleanGate, 15.0f, 40.0f, 0 },
        Range { cleanTone, 0, 0, all (4) },
        // DE-ESS: 0-55 % in a preset.
        Range { deess, 10.0f, 55.0f, 0 },
        Range { deessMode, 0, 0, all (2) },
        // DETAIL: usable 20-80 %, sweet spot 40-60 / 30-60 %; kept under 60 % so room tone stays down.
        Range { detailLow, 20.0f, 60.0f, 0 },
        Range { detailHigh, 20.0f, 60.0f, 0 },
        Range { detailSpeed, 0, 0, all (2) },
        // VOCODER: gritty with the words intact (30 %) to a dominant buzz (70 %).
        Range { gravel, 20.0f, 70.0f, 0 },
        Range { vocoderOctave, 0, 0, all (2) },
        // RING: a metallic edge to equal metal (100 % is a pure ring); 60-450 Hz stays a metallic voice
        // (no tremolo, no squeak). Ratio x0.5-x2 for Fixed (where it is not used); Follow puts the
        // carrier at the ratio times the voice's own pitch, so it rolls x1-x2 only (see adjustModule):
        // under x1 a pitched-down voice puts the carrier at 20-30 Hz, a chop. A deep PITCH can still
        // take even x1 near tremolo rates (45-56 Hz at -14 st).
        Range { ring, 15.0f, 60.0f, 0 },
        Range { ringFreq, 60.0f, 450.0f, 0 },
        Range { ringRatio, 0.5f, 2.0f, 0 },
        Range { ringCarrier, 0, 0, all (3) },
        Range { ringTrack, 0, 0, all (2) },
        Range { ringMode, 0, 0, all (2) },
        Range { ringBody, 0, 0, all (3) },
        // SHIMMER: a subtle layer to a clear wobble; liquid swell to warble (13 Hz and up is rough).
        Range { shimmer, 10.0f, 60.0f, 0 },
        Range { shimmerRate, 0.5f, 9.0f, 0 },
        Range { shimmerMode, 0, 0, all (4) },
        // GROWL: texture to an obvious growl; stutter, growl and the low end of buzz.
        Range { growl, 20.0f, 80.0f, 0 },
        Range { growlRate, 5.0f, 80.0f, 0 },
        Range { growlShape, 0, 0, all (5) },
        Range { growlCharacter, 0, 0, all (3) },
        // SWIRL: 10-90 % is usable, Deep 6 only to 75 %; slow liquid to animated (4 Hz and up warbles).
        // Capped at 50 %, and Deep 6 at 30 % (see adjustModule): Deep 6 near 50 % with a deep GROWL
        // or RING took 3.5-6 LU off real voices in the batch renders, more than OUTPUT's auto gain
        // gives back. Deep 6 takes level at any amount (about 1.5 LU at 28 % in a dense roll).
        Range { swirl, 15.0f, 50.0f, 0 },
        Range { swirlRate, 0.1f, 2.5f, 0 },
        Range { swirlCircuit, 0, 0, all (6) },
        Range { swirlMotion, 0, 0, all (3) },
        // ROAR: warmth to grit; Tone either way, short of the extremes.
        Range { roar, 20.0f, 85.0f, 0 },
        Range { roarTone, -50.0f, 50.0f, 0 },
        Range { roarVoicing, 0, 0, all (2) },
        // ENSEMBLE: 10-70 % usable (seasick above with the slow motions); Width stays mono-safe.
        Range { ensemble, 10.0f, 60.0f, 0 },
        Range { ensembleWidth, 20.0f, 100.0f, 0 },
        Range { ensembleVoices, 0, 0, all (2) },
        Range { ensembleMotion, 0, 0, all (4) },
        // TAME
        Range { tame, 0.0f, 60.0f, 0 },
        // SUB: up to a solid octave (40-65 %); above that the octave eats the limiter's headroom.
        Range { sub, 20.0f, 65.0f, 0 },
        Range { subDrive, 0.0f, 60.0f, 0 },
        Range { subRegister, 0, 0, all (3) },
        Range { subCharacter, 0, 0, all (3) },
        // OVERTONE: Add is heard from about 40 %, and Lean and Replace below 40 % are only a high-pass;
        // the Frequency where every mode works (Replace above +0.4 oct thins the voice).
        Range { overtone, 40.0f, 85.0f, 0 },
        Range { overtoneFreq, -0.5f, 0.4f, 0 },
        Range { overtonePattern, 0, 0, all (6) },
        Range { overtoneMode, 0, 0, all (3) },
        // CHAMBER: a hint of a box to a clear chamber (the voice inside the can from 75 % is left to
        // the knob); tight to ringing; speaker grit to a cranked speaker; just off mono to clearly
        // stereo. Boom Heavy is left out: it is a deliberate thump for a lighter SUB, and it ducks
        // the voice on a low, dense chain.
        Range { chamber, 15.0f, 65.0f, 0 },
        Range { chamberResonance, 0.0f, 80.0f, 0 },
        Range { chamberDrive, 0.0f, 60.0f, 0 },
        Range { chamberWidth, 20.0f, 70.0f, 0 },
        Range { chamberRoom, 0, 0, all (8) },
        Range { chamberSpeaker, 0, 0, all (3) },
        Range { chamberBoom, 0, 0, 0b0111u },
    };

    /** The range of a module control, nullptr for anything a roll does not touch. */
    constexpr const Range* find (std::string_view id)
    {
        for (const auto& r : ranges)
            if (id == r.id)
                return &r;
        return nullptr;
    }

    /** True if a roll may give this parameter this (real) value. */
    constexpr bool allows (std::string_view id, float value, float tolerance = 1.0e-3f)
    {
        const Range* r = find (id);
        if (r == nullptr)
            return false;
        if (r->choices != 0)
        {
            const int i = static_cast<int> (value + 0.5f);
            return i >= 0 && i < 32 && (r->choices & (1u << i)) != 0;
        }
        return value >= r->lo - tolerance && value <= r->hi + tolerance;
    }

    //==============================================================================================
    // Rolling. Any engine with operator() returning 32 random bits (std::mt19937) works; the
    // numbers are drawn with plain arithmetic, so a seed gives the same roll on every platform.

    template <typename Rng>
    double uniform01 (Rng& rng)
    {
        return static_cast<double> (static_cast<std::uint32_t> (rng()) >> 8) * (1.0 / 16777216.0);
    }

    template <typename Rng>
    int uniformInt (Rng& rng, int n)   // 0..n-1
    {
        const int i = static_cast<int> (uniform01 (rng) * n);
        return i < n - 1 ? i : n - 1;
    }

    using Values = std::vector<std::pair<const char*, float>>;

    /** A new real value for one module control (knob or choice) inside its range. */
    template <typename Rng>
    float rollControl (Rng& rng, const Range& r)
    {
        if (r.choices != 0)
        {
            int allowed[32] {};
            int n = 0;
            for (int i = 0; i < 32; ++i)
                if ((r.choices & (1u << i)) != 0)
                    allowed[n++] = i;
            return static_cast<float> (allowed[uniformInt (rng, n)]);
        }

        const Spec& s = spec (r.id);
        const double u = uniform01 (rng);
        if (hasLogTaper (s))
        {
            // Even over the knob's travel between the two ends.
            const float a = logTaperPosition (s, r.lo), b = logTaperPosition (s, r.hi);
            const float v = logTaperValue (s, a + static_cast<float> (u) * (b - a));
            return v < r.lo ? r.lo : (v > r.hi ? r.hi : v);
        }
        return r.lo + static_cast<float> (u) * (r.hi - r.lo);
    }

    // Where a choice changes what a knob does, the knob's roll is narrowed to fit (by rescaling the
    // value already rolled, so a seed still draws the same numbers):
    inline constexpr float swirlDeep6Max = 30.0f;     // SWIRL Deep 6: 15..30 %
    inline constexpr float ringFollowMinRatio = 1.0f;  // RING Follow: x1..x2
    inline constexpr int swirlDeep6 = 3, ringFollow = 1, pitchMono = 2;

    namespace detail
    {
        inline float* valueOf (Values& out, size_t from, std::string_view id)
        {
            for (size_t i = from; i < out.size(); ++i)
                if (id == out[i].first)
                    return &out[i].second;
            return nullptr;
        }

        /** v (inside r) moved to the same place inside lo..hi. */
        inline float rescale (const Range& r, float v, float lo, float hi)
        {
            return r.hi > r.lo ? lo + (v - r.lo) * (hi - lo) / (r.hi - r.lo) : lo;
        }

        inline int choiceOf (const float* v) { return v != nullptr ? static_cast<int> (*v + 0.5f) : -1; }
    }

    /** Narrows the knobs of one module's roll (out[from..]) that its rolled choices make unsafe. */
    inline void adjustModule (int module, Values& out, size_t from)
    {
        using detail::valueOf;
        using detail::choiceOf;
        const auto id = static_cast<ModuleId> (module);
        if (id == ModuleId::pitch)
        {
            if (auto* v = valueOf (out, from, pitch); v != nullptr && choiceOf (valueOf (out, from, pitchMode)) == pitchMono)
                *v = static_cast<float> (static_cast<int> (*v - 0.5f));   // the nearest whole note (the values are negative)
        }
        else if (id == ModuleId::swirl)
        {
            if (auto* v = valueOf (out, from, swirl); v != nullptr && choiceOf (valueOf (out, from, swirlCircuit)) == swirlDeep6)
                *v = detail::rescale (*find (swirl), *v, find (swirl)->lo, swirlDeep6Max);
        }
        else if (id == ModuleId::ring)
        {
            if (auto* v = valueOf (out, from, ringRatio); v != nullptr && choiceOf (valueOf (out, from, ringTrack)) == ringFollow)
            {
                // Evenly over the knob's (log) travel: x0.5..x2 onto x1..x2 is the square root of 2 x the
                // ratio (sqrt is exact in IEEE arithmetic, so a seed rolls the same on every platform).
                static_assert (ringFollowMinRatio == 1.0f, "the mapping below is for x1..x2 from x0.5..x2");
                const float r = std::sqrt (2.0f * *v);
                *v = r < ringFollowMinRatio ? ringFollowMinRatio : (r > find (ringRatio)->hi ? find (ringRatio)->hi : r);
            }
        }
    }

    /** New values for one module's knobs and choices (not its On switch, not its position). */
    template <typename Rng>
    void rollModule (Rng& rng, int module, Values& out)
    {
        const auto& m = modules[static_cast<size_t> (module)];
        const size_t from = out.size();
        for (const char* id : m.knobIds)
            if (id != nullptr)
                out.emplace_back (id, rollControl (rng, *find (id)));
        for (const char* id : m.chipIds)
            if (id != nullptr)
                out.emplace_back (id, rollControl (rng, *find (id)));
        adjustModule (module, out, from);
    }

    constexpr bool isUtility (int module)
    {
        for (const auto u : utilityModules)
            if (static_cast<int> (u) == module)
                return true;
        return false;
    }

    /** Which modules ROLL ALL turns on: minModulesOn..maxModulesOn of them, at least
        minCharacterModules of them not utility modules. */
    template <typename Rng>
    std::array<bool, numModules> rollOnSet (Rng& rng)
    {
        std::array<int, numModules> pick {};
        for (int m = 0; m < numModules; ++m)
            pick[static_cast<size_t> (m)] = m;
        for (int i = numModules - 1; i > 0; --i)   // Fisher-Yates
        {
            const int j = uniformInt (rng, i + 1);
            std::swap (pick[static_cast<size_t> (i)], pick[static_cast<size_t> (j)]);
        }

        const int count = minModulesOn + uniformInt (rng, maxModulesOn - minModulesOn + 1);
        // Take the first `count`, then swap character modules in from further down the shuffle
        // while too few were taken (the shuffle stays random; only the utility share is bounded).
        int character = 0;
        for (int i = 0; i < count; ++i)
            character += isUtility (pick[static_cast<size_t> (i)]) ? 0 : 1;
        for (int i = count - 1, j = count; character < minCharacterModules && i >= 0; --i)
        {
            if (! isUtility (pick[static_cast<size_t> (i)]))
                continue;
            while (j < numModules && isUtility (pick[static_cast<size_t> (j)]))
                ++j;
            if (j >= numModules)
                break;
            std::swap (pick[static_cast<size_t> (i)], pick[static_cast<size_t> (j)]);
            ++character;
        }

        std::array<bool, numModules> on {};
        for (int i = 0; i < count; ++i)
            on[static_cast<size_t> (pick[static_cast<size_t> (i)])] = true;
        return on;
    }

    /** ROLL ALL: every module's knobs and choices, and its On switch (see rollOnSet). */
    template <typename Rng>
    void rollAll (Rng& rng, Values& out)
    {
        const auto on = rollOnSet (rng);
        for (int m = 0; m < numModules; ++m)
        {
            out.emplace_back (modules[static_cast<size_t> (m)].onId, on[static_cast<size_t> (m)] ? 1.0f : 0.0f);
            rollModule (rng, m, out);
        }
    }

    //==============================================================================================
    namespace detail
    {
        constexpr bool rangesAreConsistent()
        {
            // Every module knob and choice has exactly one range, and nothing else has one.
            int controls = 0;
            for (const auto& m : modules)
            {
                for (const char* id : m.knobIds)
                    if (id != nullptr && (++controls, find (id) == nullptr))
                        return false;
                for (const char* id : m.chipIds)
                    if (id != nullptr && (++controls, find (id) == nullptr))
                        return false;
            }
            if (controls != static_cast<int> (ranges.size()))
                return false;
            for (size_t i = 0; i < ranges.size(); ++i)
                for (size_t j = i + 1; j < ranges.size(); ++j)
                    if (std::string_view (ranges[i].id) == ranges[j].id)
                        return false;

            for (const auto& r : ranges)
            {
                if (indexOf (r.id) < 0)
                    return false;
                const Spec& s = spec (r.id);
                if (s.kind == Kind::choice)
                {
                    // At least one option, and only options that exist.
                    if (r.choices == 0 || (r.choices >> s.numChoices) != 0)
                        return false;
                }
                else
                {
                    if (r.choices != 0 || s.kind == Kind::toggle || s.kind == Kind::position)
                        return false;
                    if (! (r.lo <= r.hi) || r.lo < s.minValue || r.hi > s.maxValue)
                        return false;
                }
            }
            return minModulesOn >= minCharacterModules && maxModulesOn <= numModules && minModulesOn <= maxModulesOn
                && numModules - static_cast<int> (utilityModules.size()) >= maxModulesOn;
        }
    }

    static_assert (detail::rangesAreConsistent(), "every module control needs one random range inside its parameter's range");
}

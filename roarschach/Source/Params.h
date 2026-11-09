#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// Parameter IDs, ranges, defaults, choice names and the module table: the single source of truth
// for the processor, the editor, the presets and the tests. Plain C++ (no JUCE).
//
// The declaration order of `all` is the AU index order (Logic addresses AU parameters by index):
// it is frozen at 1.0. Every parameter below has version hint 1. Anything added after 1.0 gets
// hint 2 or later and is appended, never inserted; nothing is ever removed, renamed or reordered.
namespace Params
{
    inline constexpr int versionHint = 1;

    // ---------------------------------------------------------------------------------- IDs
    // IN
    inline constexpr const char* inTrim = "inTrim";
    inline constexpr const char* inAuto = "inAuto";
    // PITCH
    inline constexpr const char* pitchOn = "pitchOn";
    inline constexpr const char* pitchPos = "pitchPos";
    inline constexpr const char* pitch = "pitch";
    inline constexpr const char* pitchSize = "pitchSize";
    inline constexpr const char* pitchDrive = "pitchDrive";
    inline constexpr const char* pitchMode = "pitchMode";
    inline constexpr const char* pitchFormant = "pitchFormant";
    // CLEAN
    inline constexpr const char* cleanOn = "cleanOn";
    inline constexpr const char* cleanPos = "cleanPos";
    inline constexpr const char* cleanContain = "cleanContain";
    inline constexpr const char* cleanGate = "cleanGate";
    inline constexpr const char* cleanTone = "cleanTone";
    // DE-ESS
    inline constexpr const char* deessOn = "deessOn";
    inline constexpr const char* deessPos = "deessPos";
    inline constexpr const char* deess = "deess";
    inline constexpr const char* deessMode = "deessMode";
    // DETAIL
    inline constexpr const char* detailOn = "detailOn";
    inline constexpr const char* detailPos = "detailPos";
    inline constexpr const char* detailLow = "detailLow";
    inline constexpr const char* detailHigh = "detailHigh";
    inline constexpr const char* detailSpeed = "detailSpeed";
    // VOCODER
    inline constexpr const char* vocoderOn = "vocoderOn";
    inline constexpr const char* vocoderPos = "vocoderPos";
    inline constexpr const char* gravel = "gravel";
    inline constexpr const char* vocoderOctave = "vocoderOctave";
    // SHIMMER
    inline constexpr const char* shimmerOn = "shimmerOn";
    inline constexpr const char* shimmerPos = "shimmerPos";
    inline constexpr const char* shimmer = "shimmer";
    inline constexpr const char* shimmerRate = "shimmerRate";
    inline constexpr const char* shimmerMode = "shimmerMode";
    // GROWL
    inline constexpr const char* growlOn = "growlOn";
    inline constexpr const char* growlPos = "growlPos";
    inline constexpr const char* growl = "growl";
    inline constexpr const char* growlRate = "growlRate";
    inline constexpr const char* growlShape = "growlShape";
    inline constexpr const char* growlCharacter = "growlCharacter";
    // SWIRL
    inline constexpr const char* swirlOn = "swirlOn";
    inline constexpr const char* swirlPos = "swirlPos";
    inline constexpr const char* swirl = "swirl";
    inline constexpr const char* swirlRate = "swirlRate";
    inline constexpr const char* swirlCircuit = "swirlCircuit";
    inline constexpr const char* swirlMotion = "swirlMotion";
    // ROAR
    inline constexpr const char* roarOn = "roarOn";
    inline constexpr const char* roarPos = "roarPos";
    inline constexpr const char* roar = "roar";
    inline constexpr const char* roarTone = "roarTone";
    inline constexpr const char* roarVoicing = "roarVoicing";
    // ENSEMBLE
    inline constexpr const char* ensembleOn = "ensembleOn";
    inline constexpr const char* ensemblePos = "ensemblePos";
    inline constexpr const char* ensemble = "ensemble";
    inline constexpr const char* ensembleWidth = "ensembleWidth";
    inline constexpr const char* ensembleVoices = "ensembleVoices";
    inline constexpr const char* ensembleMotion = "ensembleMotion";
    // TAME
    inline constexpr const char* tameOn = "tameOn";
    inline constexpr const char* tamePos = "tamePos";
    inline constexpr const char* tame = "tame";
    // SUB
    inline constexpr const char* subOn = "subOn";
    inline constexpr const char* subPos = "subPos";
    inline constexpr const char* sub = "sub";
    inline constexpr const char* subDrive = "subDrive";
    inline constexpr const char* subRegister = "subRegister";
    inline constexpr const char* subCharacter = "subCharacter";
    // OVERTONE
    inline constexpr const char* overtoneOn = "overtoneOn";
    inline constexpr const char* overtonePos = "overtonePos";
    inline constexpr const char* overtone = "overtone";
    inline constexpr const char* overtoneFreq = "overtoneFreq";
    inline constexpr const char* overtonePattern = "overtonePattern";
    inline constexpr const char* overtoneMode = "overtoneMode";
    // RING
    inline constexpr const char* ringOn = "ringOn";
    inline constexpr const char* ringPos = "ringPos";
    inline constexpr const char* ring = "ring";
    inline constexpr const char* ringFreq = "ringFreq";
    inline constexpr const char* ringRatio = "ringRatio";
    inline constexpr const char* ringCarrier = "ringCarrier";
    inline constexpr const char* ringTrack = "ringTrack";
    inline constexpr const char* ringMode = "ringMode";
    inline constexpr const char* ringBody = "ringBody";
    // OUTPUT
    inline constexpr const char* outPush = "outPush";
    inline constexpr const char* outCeiling = "outCeiling";
    inline constexpr const char* outClip = "outClip";
    inline constexpr const char* outClipShape = "outClipShape";
    inline constexpr const char* outAttack = "outAttack";
    inline constexpr const char* outRelease = "outRelease";
    inline constexpr const char* outAuto = "outAuto";
    inline constexpr const char* mix = "mix";

    // ---------------------------------------------------------------------------------- choice names
    inline constexpr std::array<const char*, 3> pitchModeNames { "Shift", "Snap", "Mono" };
    inline constexpr std::array<const char*, 2> pitchFormantNames { "Follow", "Keep" };
    inline constexpr std::array<const char*, 4> cleanToneNames { "Flat", "Voice", "Heavy", "Radio" };
    inline constexpr std::array<const char*, 2> deessModeNames { "Split", "Wide" };
    inline constexpr std::array<const char*, 2> detailSpeedNames { "Natural", "Snappy" };
    inline constexpr std::array<const char*, 2> vocoderOctaveNames { "-1 Oct", "-2 Oct" };
    inline constexpr std::array<const char*, 4> shimmerModeNames { "Up", "Down", "Up+Down", "Stereo" };
    inline constexpr std::array<const char*, 5> growlShapeNames { "Sine", "Opto", "Square", "Throat", "Rattle" };
    inline constexpr std::array<const char*, 3> growlCharacterNames { "Clean", "Warm", "Grind" };
    inline constexpr std::array<const char*, 6> swirlCircuitNames { "Liquid 4", "Smooth 4", "Talky 4", "Deep 6", "Deep 12", "Dark 5" };
    inline constexpr std::array<const char*, 3> swirlMotionNames { "LFO", "LFO+Level", "Random" };
    inline constexpr std::array<const char*, 2> roarVoicingNames { "Line", "Mic" };
    inline constexpr std::array<const char*, 2> ensembleVoicesNames { "3", "6" };
    inline constexpr std::array<const char*, 4> ensembleMotionNames { "Slow", "Medium", "Fast", "Vibrato" };
    inline constexpr std::array<const char*, 3> subRegisterNames { "Follow", "Tight", "Deep" };
    inline constexpr std::array<const char*, 3> subCharacterNames { "Clean", "Resonant", "Thick" };
    inline constexpr std::array<const char*, 6> overtonePatternNames { "Chest", "Throat", "Beast", "Speaker", "Broad", "Tracked" };
    inline constexpr std::array<const char*, 3> overtoneModeNames { "Add", "Lean", "Replace" };
    inline constexpr std::array<const char*, 3> ringCarrierNames { "Sine", "Triangle", "Square" };
    inline constexpr std::array<const char*, 2> ringTrackNames { "Fixed", "Follow" };
    inline constexpr std::array<const char*, 2> ringModeNames { "Ring", "AM" };
    inline constexpr std::array<const char*, 3> ringBodyNames { "Off", "Box", "Can" };

    // Choice indices the DSP switches on (same order as the names above). OVERTONE and RING use
    // the enums their modules define (roar::OvertonePattern, roar::RingCarrier, ...), in the same
    // order as these names; the chain casts the choice index.
    enum class PitchMode : int { shift, snap, mono };
    enum class PitchFormant : int { follow, keep };
    enum class CleanTone : int { flat, voice, heavy, radio };
    enum class DeessMode : int { split, wide };
    enum class DetailSpeed : int { natural, snappy };
    enum class VocoderOctave : int { down1, down2 };
    enum class ShimmerMode : int { up, down, upDown, stereo };
    enum class GrowlShape : int { sine, opto, square, throat, rattle };
    enum class GrowlCharacter : int { clean, warm, grind };
    enum class SwirlCircuit : int { liquid4, smooth4, talky4, deep6, deep12, dark5 };
    enum class SwirlMotion : int { lfo, lfoLevel, random };
    enum class RoarVoicing : int { line, mic };
    enum class EnsembleVoices : int { three, six };
    enum class EnsembleMotion : int { slow, medium, fast, vibrato };
    enum class SubRegister : int { follow, tight, deep };
    enum class SubCharacter : int { clean, resonant, thick };

    // ---------------------------------------------------------------------------------- parameter table
    // How a parameter is created and displayed:
    //   percent       0..max %, display "45%"                     (float)
    //   bipolar       -100..+100 %, roarTone: "Bass 40%" / "Even" / "Treble 40%"
    //   decibels      display "+3.0 dB" (outClip shows "Off" at 0)
    //   semitones     display "-2.00 st"
    //   octaves       display "+0.25 oct"
    //   hertz         log taper (centre = the value at the knob's mid point), display "27.0 Hz"
    //                 (>= 10 Hz: one decimal, < 10 Hz: two decimals, >= 1 kHz: "1.20 kHz")
    //   ratio         log taper around `centre`, display "x1.50"
    //   milliseconds  log taper around `centre`, display "2.0 ms" / "0.05 ms" / "60 ms" / "1.00 s"
    //   choice        AudioParameterChoice over `choices`
    //   toggle        AudioParameterBool, display "On" / "Off"
    //   position      AudioParameterInt 0..numModules-1, display "1st" .. "14th"
    enum class Kind { percent, bipolar, decibels, semitones, octaves, hertz, ratio, milliseconds, choice, toggle, position };

    struct Spec
    {
        const char* id;
        const char* name;
        Kind kind;
        float minValue;
        float maxValue;
        float step;              // 0 = continuous
        float defaultValue;      // choice / toggle / position: the index (0 or 1 for toggles)
        float centre;            // hertz / ratio / milliseconds: the value at the knob's mid point; 0 otherwise
        const char* const* choices;
        int numChoices;
    };

    // The 14 movable modules. Declared here because the position range depends on it.
    inline constexpr int numModules = 14;

    namespace detail
    {
        constexpr Spec percent (const char* id, const char* name, float def, float max = 100.0f)
        {
            return { id, name, Kind::percent, 0.0f, max, 0.1f, def, 0.0f, nullptr, 0 };
        }

        constexpr Spec toggle (const char* id, const char* name, bool def)
        {
            return { id, name, Kind::toggle, 0.0f, 1.0f, 1.0f, def ? 1.0f : 0.0f, 0.0f, nullptr, 0 };
        }

        constexpr Spec position (const char* id, const char* name, int def)
        {
            return { id, name, Kind::position, 0.0f, static_cast<float> (numModules - 1), 1.0f, static_cast<float> (def), 0.0f, nullptr, 0 };
        }

        template <size_t N>
        constexpr Spec choice (const char* id, const char* name, const std::array<const char*, N>& names, int def)
        {
            return { id, name, Kind::choice, 0.0f, static_cast<float> (N - 1), 1.0f, static_cast<float> (def), 0.0f,
                     names.data(), static_cast<int> (N) };
        }

        constexpr Spec decibels (const char* id, const char* name, float lo, float hi, float def)
        {
            return { id, name, Kind::decibels, lo, hi, 0.1f, def, 0.0f, nullptr, 0 };
        }

        constexpr Spec hertz (const char* id, const char* name, float lo, float hi, float def, float centre)
        {
            return { id, name, Kind::hertz, lo, hi, 0.0f, def, centre, nullptr, 0 };
        }

        constexpr Spec milliseconds (const char* id, const char* name, float lo, float hi, float def, float centre)
        {
            return { id, name, Kind::milliseconds, lo, hi, 0.0f, def, centre, nullptr, 0 };
        }
    }

    inline constexpr int numParameters = 87;

    inline constexpr std::array<Spec, numParameters> all {
        // IN
        detail::decibels (inTrim, "Input", -24.0f, 24.0f, 0.0f),
        detail::toggle (inAuto, "Auto Level", true),
        // PITCH
        detail::toggle (pitchOn, "Pitch On", true),
        detail::position (pitchPos, "Pitch Position", 0),
        Spec { pitch, "Pitch", Kind::semitones, -24.0f, 12.0f, 0.01f, -2.0f, 0.0f, nullptr, 0 },
        detail::percent (pitchSize, "Size", 0.0f),
        detail::percent (pitchDrive, "Drive", 5.0f),
        detail::choice (pitchMode, "Pitch Mode", pitchModeNames, 0),
        detail::choice (pitchFormant, "Formant", pitchFormantNames, 0),
        // CLEAN
        detail::toggle (cleanOn, "Clean On", true),
        detail::position (cleanPos, "Clean Position", 1),
        detail::percent (cleanContain, "Contain", 35.0f),
        detail::percent (cleanGate, "Gate", 35.0f),
        detail::choice (cleanTone, "Clean Tone", cleanToneNames, 1),
        // DE-ESS
        detail::toggle (deessOn, "De-ess On", true),
        detail::position (deessPos, "De-ess Position", 2),
        detail::percent (deess, "De-ess", 30.0f),
        detail::choice (deessMode, "De-ess Mode", deessModeNames, 0),
        // DETAIL
        detail::toggle (detailOn, "Detail On", true),
        detail::position (detailPos, "Detail Position", 3),
        detail::percent (detailLow, "Detail Low", 30.0f),
        detail::percent (detailHigh, "Detail High", 25.0f),
        detail::choice (detailSpeed, "Detail Speed", detailSpeedNames, 0),
        // VOCODER
        detail::toggle (vocoderOn, "Vocoder On", false),
        detail::position (vocoderPos, "Vocoder Position", 4),
        detail::percent (gravel, "Gravel", 50.0f),
        detail::choice (vocoderOctave, "Vocoder Octave", vocoderOctaveNames, 0),
        // SHIMMER
        detail::toggle (shimmerOn, "Shimmer On", true),
        detail::position (shimmerPos, "Shimmer Position", 6),
        detail::percent (shimmer, "Shimmer", 12.0f),
        detail::hertz (shimmerRate, "Shimmer Rate", 0.5f, 40.0f, 11.0f, 5.0f),
        detail::choice (shimmerMode, "Shimmer Mode", shimmerModeNames, 0),
        // GROWL
        detail::toggle (growlOn, "Growl On", true),
        detail::position (growlPos, "Growl Position", 7),
        detail::percent (growl, "Growl Depth", 48.0f),
        detail::hertz (growlRate, "Growl Rate", 1.0f, 150.0f, 27.0f, 12.247449f),   // sqrt (1 * 150)
        detail::choice (growlShape, "Growl Shape", growlShapeNames, 0),
        detail::choice (growlCharacter, "Growl Character", growlCharacterNames, 1),
        // SWIRL
        detail::toggle (swirlOn, "Swirl On", true),
        detail::position (swirlPos, "Swirl Position", 8),
        detail::percent (swirl, "Swirl", 30.0f),
        detail::hertz (swirlRate, "Swirl Rate", 0.05f, 10.0f, 0.35f, 0.70710678f),  // sqrt (0.05 * 10)
        detail::choice (swirlCircuit, "Swirl Circuit", swirlCircuitNames, 0),
        detail::choice (swirlMotion, "Swirl Motion", swirlMotionNames, 1),
        // ROAR
        detail::toggle (roarOn, "Roar On", true),
        detail::position (roarPos, "Roar Position", 9),
        detail::percent (roar, "Roar Drive", 45.0f),
        Spec { roarTone, "Roar Tone", Kind::bipolar, -100.0f, 100.0f, 0.1f, 10.0f, 0.0f, nullptr, 0 },
        detail::choice (roarVoicing, "Roar Voicing", roarVoicingNames, 0),
        // ENSEMBLE
        detail::toggle (ensembleOn, "Ensemble On", true),
        detail::position (ensemblePos, "Ensemble Position", 10),
        detail::percent (ensemble, "Ensemble", 18.0f),
        detail::percent (ensembleWidth, "Width", 12.0f, 150.0f),
        detail::choice (ensembleVoices, "Ensemble Voices", ensembleVoicesNames, 0),
        detail::choice (ensembleMotion, "Ensemble Motion", ensembleMotionNames, 1),
        // TAME
        detail::toggle (tameOn, "Tame On", true),
        detail::position (tamePos, "Tame Position", 11),
        detail::percent (tame, "Tame", 30.0f),
        // SUB
        detail::toggle (subOn, "Sub On", true),
        detail::position (subPos, "Sub Position", 12),
        detail::percent (sub, "Sub Level", 45.0f),
        detail::percent (subDrive, "Sub Drive", 60.0f),
        detail::choice (subRegister, "Sub Register", subRegisterNames, 0),
        detail::choice (subCharacter, "Sub Character", subCharacterNames, 0),
        // OVERTONE
        detail::toggle (overtoneOn, "Overtone On", true),
        detail::position (overtonePos, "Overtone Position", 13),
        detail::percent (overtone, "Overtone Amount", 10.0f),
        Spec { overtoneFreq, "Overtone Frequency", Kind::octaves, -1.0f, 1.0f, 0.01f, 0.0f, 0.0f, nullptr, 0 },
        detail::choice (overtonePattern, "Overtone Pattern", overtonePatternNames, 0),
        detail::choice (overtoneMode, "Overtone Mode", overtoneModeNames, 0),
        // RING
        detail::toggle (ringOn, "Ring On", false),
        detail::position (ringPos, "Ring Position", 5),
        detail::percent (ring, "Ring Amount", 40.0f),
        detail::hertz (ringFreq, "Ring Freq", 20.0f, 4000.0f, 150.0f, 150.0f),
        Spec { ringRatio, "Ring Ratio", Kind::ratio, 0.25f, 4.0f, 0.01f, 1.0f, 1.0f, nullptr, 0 },
        detail::choice (ringCarrier, "Ring Carrier", ringCarrierNames, 0),
        detail::choice (ringTrack, "Ring Track", ringTrackNames, 0),
        detail::choice (ringMode, "Ring Mode", ringModeNames, 0),
        detail::choice (ringBody, "Ring Body", ringBodyNames, 0),
        // OUTPUT
        detail::decibels (outPush, "Push", 0.0f, 24.0f, 8.0f),
        detail::decibels (outCeiling, "Ceiling", -12.0f, 0.0f, -1.0f),
        detail::decibels (outClip, "Clip", 0.0f, 18.0f, 0.0f),
        detail::percent (outClipShape, "Clip Shape", 0.0f),
        detail::milliseconds (outAttack, "Attack", 0.05f, 10.0f, 2.0f, 2.0f),
        detail::milliseconds (outRelease, "Release", 1.0f, 1000.0f, 60.0f, 60.0f),
        detail::toggle (outAuto, "Auto Release", true),
        detail::percent (mix, "Mix", 100.0f),
    };

    /** Index of a parameter in `all` (the AU index), -1 if unknown. */
    constexpr int indexOf (std::string_view id)
    {
        for (int i = 0; i < numParameters; ++i)
            if (id == all[static_cast<size_t> (i)].id)
                return i;
        return -1;
    }

    /** The spec of a known ID (the first entry for an unknown one; check indexOf first). */
    constexpr const Spec& spec (std::string_view id)
    {
        const int i = indexOf (id);
        return all[static_cast<size_t> (i < 0 ? 0 : i)];
    }

    constexpr float defaultOf (std::string_view id) { return spec (id).defaultValue; }

    // ---------------------------------------------------------------------------------- modules
    // The 14 movable modules, in the reference (default) order. `defaultIndex` is the default
    // position and the tie-break when two modules ask for the same position.
    enum class ModuleId : int { pitch, clean, deess, detail, vocoder, ring, shimmer, growl, swirl, roar, ensemble, tame, sub, overtone };

    struct ModuleSpec
    {
        ModuleId id;
        const char* code;              // two letters, used in presets, tests and order strings
        const char* uiName;
        int defaultIndex;
        std::uint32_t accent;          // ARGB
        const char* onId;
        const char* posId;
        std::array<const char*, 3> knobIds;   // nullptr where unused (PITCH has 3)
        std::array<const char*, 4> chipIds;   // the mode chip's choice parameters, nullptr where unused
    };

    inline constexpr std::array<ModuleSpec, numModules> modules {
        ModuleSpec { ModuleId::pitch, "PI", "PITCH", 0, 0xffa992ffu, pitchOn, pitchPos,
                     { pitch, pitchSize, pitchDrive }, { pitchMode, pitchFormant, nullptr, nullptr } },
        ModuleSpec { ModuleId::clean, "CL", "CLEAN", 1, 0xff8fb7a3u, cleanOn, cleanPos,
                     { cleanContain, cleanGate, nullptr }, { cleanTone, nullptr, nullptr, nullptr } },
        ModuleSpec { ModuleId::deess, "DS", "DE-ESS", 2, 0xffb3c6dcu, deessOn, deessPos,
                     { deess, nullptr, nullptr }, { deessMode, nullptr, nullptr, nullptr } },
        ModuleSpec { ModuleId::detail, "DT", "DETAIL", 3, 0xff6fd6b4u, detailOn, detailPos,
                     { detailLow, detailHigh, nullptr }, { detailSpeed, nullptr, nullptr, nullptr } },
        ModuleSpec { ModuleId::vocoder, "VO", "VOCODER", 4, 0xffc6e86bu, vocoderOn, vocoderPos,
                     { gravel, nullptr, nullptr }, { vocoderOctave, nullptr, nullptr, nullptr } },
        ModuleSpec { ModuleId::ring, "RM", "RING", 5, 0xffe6edf5u, ringOn, ringPos,
                     { ring, ringFreq, ringRatio }, { ringCarrier, ringTrack, ringMode, ringBody } },
        ModuleSpec { ModuleId::shimmer, "SH", "SHIMMER", 6, 0xff7fe3f0u, shimmerOn, shimmerPos,
                     { shimmer, shimmerRate, nullptr }, { shimmerMode, nullptr, nullptr, nullptr } },
        ModuleSpec { ModuleId::growl, "GR", "GROWL", 7, 0xffef6f8eu, growlOn, growlPos,
                     { growl, growlRate, nullptr }, { growlShape, growlCharacter, nullptr, nullptr } },
        ModuleSpec { ModuleId::swirl, "SW", "SWIRL", 8, 0xffd38cf0u, swirlOn, swirlPos,
                     { swirl, swirlRate, nullptr }, { swirlCircuit, swirlMotion, nullptr, nullptr } },
        ModuleSpec { ModuleId::roar, "RO", "ROAR", 9, 0xffff7a45u, roarOn, roarPos,
                     { roar, roarTone, nullptr }, { roarVoicing, nullptr, nullptr, nullptr } },
        ModuleSpec { ModuleId::ensemble, "EN", "ENSEMBLE", 10, 0xff5ec8d8u, ensembleOn, ensemblePos,
                     { ensemble, ensembleWidth, nullptr }, { ensembleVoices, ensembleMotion, nullptr, nullptr } },
        ModuleSpec { ModuleId::tame, "TA", "TAME", 11, 0xffd9b66au, tameOn, tamePos,
                     { tame, nullptr, nullptr }, { nullptr, nullptr, nullptr, nullptr } },
        ModuleSpec { ModuleId::sub, "SU", "SUB", 12, 0xff6f8dffu, subOn, subPos,
                     { sub, subDrive, nullptr }, { subRegister, subCharacter, nullptr, nullptr } },
        ModuleSpec { ModuleId::overtone, "OV", "OVERTONE", 13, 0xffff6fcfu, overtoneOn, overtonePos,
                     { overtone, overtoneFreq, nullptr }, { overtonePattern, overtoneMode, nullptr, nullptr } },
    };

    // The fixed ends of the chain.
    inline constexpr std::uint32_t accentIn = 0xff7f97a6u;       // IN: teal-grey
    inline constexpr std::uint32_t accentOutput = 0xffffb54du;   // OUTPUT and chrome: amber

    // The OUTPUT tile's controls, in tile order (Push, Ceiling, Clip, Clip Shape, Attack, Release,
    // Auto, Mix), and IN's.
    inline constexpr std::array<const char*, 8> outputIds { outPush, outCeiling, outClip, outClipShape, outAttack, outRelease, outAuto, mix };
    inline constexpr std::array<const char*, 2> inIds { inTrim, inAuto };

    constexpr const ModuleSpec& module (ModuleId id) { return modules[static_cast<size_t> (id)]; }

    /** Module for a two-letter code ("PI" .. "OV"), -1 if unknown. */
    constexpr int moduleIndexForCode (std::string_view code)
    {
        for (int i = 0; i < numModules; ++i)
            if (code == modules[static_cast<size_t> (i)].code)
                return i;
        return -1;
    }

    /** "1st" .. "14th" for positions 0..13. */
    inline constexpr std::array<const char*, numModules> positionNames {
        "1st", "2nd", "3rd", "4th", "5th", "6th", "7th", "8th", "9th", "10th", "11th", "12th", "13th", "14th"
    };

    using Order = std::array<int, numModules>;

    /**
     * The resolved order: the modules sorted by (position, defaultIndex), so duplicates and
     * partial states always resolve the same way. positions[m] is module m's position parameter
     * (any int; clamped to 0..numModules-1). Returns order[slot] = module index. Insertion sort
     * over 14 entries: allocation-free, fine on the audio thread.
     */
    constexpr Order resolveOrder (const Order& positions)
    {
        Order order {};
        Order key {};
        for (int m = 0; m < numModules; ++m)
        {
            const int p = positions[static_cast<size_t> (m)];
            const int clamped = p < 0 ? 0 : (p > numModules - 1 ? numModules - 1 : p);
            order[static_cast<size_t> (m)] = m;
            key[static_cast<size_t> (m)] = clamped * numModules + modules[static_cast<size_t> (m)].defaultIndex;
        }

        for (int i = 1; i < numModules; ++i)
        {
            const int moving = order[static_cast<size_t> (i)];
            int j = i - 1;
            while (j >= 0 && key[static_cast<size_t> (order[static_cast<size_t> (j)])] > key[static_cast<size_t> (moving)])
            {
                order[static_cast<size_t> (j + 1)] = order[static_cast<size_t> (j)];
                --j;
            }
            order[static_cast<size_t> (j + 1)] = moving;
        }
        return order;
    }

    /** The default positions (the reference order). */
    constexpr Order defaultPositions()
    {
        Order p {};
        for (int m = 0; m < numModules; ++m)
            p[static_cast<size_t> (m)] = modules[static_cast<size_t> (m)].defaultIndex;
        return p;
    }

    // ---------------------------------------------------------------------------------- checks
    namespace detail
    {
        constexpr bool tableIsConsistent()
        {
            // Every module's On, position, knob and chip parameters exist, the chips are choices,
            // and the default positions are the reference order 0..numModules-1.
            for (int m = 0; m < numModules; ++m)
            {
                const auto& mod = modules[static_cast<size_t> (m)];
                if (mod.defaultIndex != m || static_cast<int> (mod.id) != m)
                    return false;
                if (indexOf (mod.onId) < 0 || indexOf (mod.posId) < 0)
                    return false;
                if (spec (mod.onId).kind != Kind::toggle || spec (mod.posId).kind != Kind::position)
                    return false;
                if (static_cast<int> (spec (mod.posId).defaultValue) != mod.defaultIndex)
                    return false;
                for (const char* k : mod.knobIds)
                    if (k != nullptr && indexOf (k) < 0)
                        return false;
                for (const char* c : mod.chipIds)
                    if (c != nullptr && (indexOf (c) < 0 || spec (c).kind != Kind::choice))
                        return false;
            }
            // Every position parameter belongs to exactly one module.
            int positions = 0;
            for (const auto& s : all)
                if (s.kind == Kind::position)
                    ++positions;
            if (positions != numModules)
                return false;
            // IDs are unique.
            for (int i = 0; i < numParameters; ++i)
                for (int j = i + 1; j < numParameters; ++j)
                    if (std::string_view (all[static_cast<size_t> (i)].id) == all[static_cast<size_t> (j)].id)
                        return false;
            // Defaults lie in range, choices have names.
            for (const auto& s : all)
            {
                if (s.defaultValue < s.minValue || s.defaultValue > s.maxValue)
                    return false;
                if (s.kind == Kind::choice && (s.choices == nullptr || s.numChoices != static_cast<int> (s.maxValue) + 1))
                    return false;
                if ((s.kind == Kind::hertz || s.kind == Kind::ratio || s.kind == Kind::milliseconds)
                    && (s.centre <= s.minValue || s.centre >= s.maxValue))
                    return false;
            }
            return true;
        }

        constexpr bool defaultOrderIsReference()
        {
            const Order order = resolveOrder (defaultPositions());
            for (int i = 0; i < numModules; ++i)
                if (order[static_cast<size_t> (i)] != i)
                    return false;
            return true;
        }
    }

    static_assert (all.size() == 87, "the parameter table has 87 parameters");
    static_assert (detail::tableIsConsistent(), "module table and parameter table disagree");
    static_assert (detail::defaultOrderIsReference(), "the default positions resolve to the reference order");
    static_assert (indexOf (inTrim) == 0 && indexOf (outPush) == 79 && indexOf (mix) == 86, "AU index order is frozen");
    static_assert (indexOf (overtoneOn) == 64 && indexOf (ringOn) == 70, "AU index order is frozen");
    static_assert (moduleIndexForCode ("RM") == static_cast<int> (ModuleId::ring)
                   && moduleIndexForCode ("OV") == static_cast<int> (ModuleId::overtone), "module codes");
}

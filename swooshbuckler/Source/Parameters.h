#pragma once

#include <JuceHeader.h>

#include "TypedValue.h"

/** Parameter IDs and the layout, in signal order: tracks -> ride -> bus A / bus B -> Stowaway -> hits -> output. */
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
    // How the faders ride: each on its own wave (Faders Apart), all on one repeating swell with the passes and the
    // aimed hits on its peak (Faders Together), or with the Master track's loudness (Follow Master).
    inline constexpr const char* rideMode = "rideMode";
    enum RideModeChoice { RideApart = 0, RideTogether, RideFollowMaster, numRideModes };
    inline constexpr const char* rideModeNames[numRideModes] = { "Faders Apart", "Faders Together", "Follow Master" };
    // Faders Together, behind Rate: where in the swell the top falls, how far the tracks fan in, which peaks get a hit.
    inline constexpr const char* togetherPeakAt   = "togetherPeakAt";
    inline constexpr const char* togetherStagger  = "togetherStagger";
    inline constexpr const char* togetherHitEvery = "togetherHitEvery";
    inline constexpr int numHitEvery = 4;
    inline constexpr const char* hitEveryNames[numHitEvery] = { "1 Peak", "2 Peaks", "3 Peaks", "4 Peaks" };
    inline constexpr float togetherPeakAtMin = 0.2f, togetherPeakAtMax = 0.8f;
    // Swell as a host parameter (a DAW records the presses), and Auto Swell: Swell firing by itself.
    inline constexpr const char* swell       = "swell";
    inline constexpr const char* swellAuto   = "swellAuto";
    inline constexpr const char* swellEvery  = "swellEvery";
    inline constexpr const char* swellHold   = "swellHold";
    inline constexpr const char* swellChance = "swellChance";
    inline constexpr const char* swellLoose  = "swellLoose";
    enum AutoSwellChoice { AutoSwellOff = 0, AutoSwellPeaks, AutoSwellClock, numAutoSwells };
    inline constexpr const char* autoSwellNames[numAutoSwells] = { "Off", "On Peaks", "On a Clock" };
    inline constexpr float swellEveryMin = 0.5f, swellEveryMax = 30.0f, swellHoldMin = 0.25f, swellHoldMax = 2.5f, swellLooseMax = 0.5f;
    /** Hold is never more than this share of Every. */
    inline constexpr float swellHoldShareMax = 0.6f;
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

    // What is behind a bus's Swirl and Filter wheels (each bus its own). Part of a look (presets and A/B hold them;
    // Randomise, the dice and Small Changes leave them alone), except Varispeed, which is a hand on the second tape
    // machine: the session's, not in undo.
    enum BusDetail
    {
        SwirlType = 0, SwirlFeedback, SwirlSpeed, SwirlDepth, SwirlFollow,
        TapeMotion, TapeInvert, TapeDepth, TapeRate, TapeFeedback, TapeWobble, Varispeed,
        DoublerWave, DoublerThicken, DoublerSpread, DoublerDelay, DoublerWander, DoublerRate, DoublerFeedback,
        FilterType, FilterSteep, FilterShape, FilterClosedAt, FilterResonance, FilterFollow, FilterSweep, FilterRate,
        numBusDetails
    };
    inline constexpr const char* busDetail[numBuses][numBusDetails] = {
        { "aSwirlType", "aSwirlFeedback", "aSwirlSpeed", "aSwirlDepth", "aSwirlFollow",
          "aTapeMotion", "aTapeInvert", "aTapeDepth", "aTapeRate", "aTapeFeedback", "aTapeWobble", "aVarispeed",
          "aDoublerWave", "aDoublerThicken", "aDoublerSpread", "aDoublerDelay", "aDoublerWander", "aDoublerRate", "aDoublerFeedback",
          "aFilterType", "aFilterSteep", "aFilterShape", "aFilterClosedAt", "aFilterResonance", "aFilterFollow", "aFilterSweep", "aFilterRate" },
        { "bSwirlType", "bSwirlFeedback", "bSwirlSpeed", "bSwirlDepth", "bSwirlFollow",
          "bTapeMotion", "bTapeInvert", "bTapeDepth", "bTapeRate", "bTapeFeedback", "bTapeWobble", "bVarispeed",
          "bDoublerWave", "bDoublerThicken", "bDoublerSpread", "bDoublerDelay", "bDoublerWander", "bDoublerRate", "bDoublerFeedback",
          "bFilterType", "bFilterSteep", "bFilterShape", "bFilterClosedAt", "bFilterResonance", "bFilterFollow", "bFilterSweep", "bFilterRate" },
    };
    /** The names after "Bus A " / "Bus B " (the layer pages show the part after "Swirl " / "Tape " / ... as the caption). */
    inline constexpr const char* busDetailNames[numBusDetails] = {
        "Swirl Type", "Swirl Feedback", "Swirl Speed", "Swirl Depth", "Swirl Follow",
        "Tape Motion", "Tape Invert", "Tape Depth", "Tape Rate", "Tape Feedback", "Tape Wobble", "Varispeed",
        "Doubler Wave", "Doubler Thicken", "Doubler Spread", "Doubler Delay", "Doubler Wander", "Doubler Rate", "Doubler Feedback",
        "Filter Type", "Filter 24 dB", "Filter Shape", "Filter Closed At", "Filter Resonance", "Filter Follow", "Filter Sweep", "Filter Rate"
    };
    enum SwirlTypeChoice { SwirlBoth = 0, SwirlPhaser, SwirlFlanger, SwirlTape, SwirlDoubler, numSwirlTypes };
    inline constexpr const char* swirlTypeNames[numSwirlTypes] = { "Phaser + Flanger", "Phaser", "Flanger", "Tape Flanger", "Doubler" };
    enum TapeMotionChoice { TapeWithPass = 0, TapeOnItsOwn, TapeByHand, numTapeMotions };
    inline constexpr const char* tapeMotionNames[numTapeMotions] = { "With the Pass", "On Its Own", "By Hand" };
    enum DoublerWaveChoice { WaveRandom = 0, WaveSine, WaveTriangle, numDoublerWaves };
    inline constexpr const char* doublerWaveNames[numDoublerWaves] = { "Random", "Sine", "Triangle" };
    enum FilterTypeChoice { FilterLowPass = 0, FilterHighPass, FilterBandPass, FilterNotch, numFilterTypes };
    inline constexpr const char* filterTypeNames[numFilterTypes] = { "Low Pass", "High Pass", "Band Pass", "Notch" };
    enum FilterShapeChoice { ShapeSine = 0, ShapeTriangle, ShapeRampUp, ShapeRampDown, ShapeSquare, ShapeRandom, numFilterShapes };
    inline constexpr const char* filterShapeNames[numFilterShapes] = { "Sine", "Triangle", "Ramp Up", "Ramp Down", "Square", "Random" };
    // Ranges (plain units: ms, Hz, x, and -1..1 or 0..1 for the percentages).
    inline constexpr float swirlSpeedMin = 0.1f, swirlSpeedMax = 8.0f;
    inline constexpr float tapeDepthMaxMs = 6.0f, tapeRateMin = 0.01f, tapeRateMax = 4.0f;
    inline constexpr float doublerDelayMinMs = 3.0f, doublerDelayMaxMs = 75.0f, doublerRateMin = 0.01f, doublerRateMax = 6.0f;
    /** Doubler Feedback reads -68% .. +68%: the loop gain is 0.68 x the parameter (-1 .. 1). */
    inline constexpr float doublerFeedbackMax = 0.68f;
    inline constexpr float filterClosedMinHz = 20.0f, filterClosedMaxHz = 2000.0f, filterRateMin = 0.05f, filterRateMax = 20.0f;
    /** Each detail's default, in plain units. Swirl's and the Filter's leave the front wheels doing what they do
        with no layer opened. */
    inline constexpr float busDetailDefaults[numBusDetails] = {
        0.0f, 0.55f, 1.0f, 1.0f, 1.0f,                    // Swirl: Phaser + Flanger, +55%, 1x, 100%, 100%
        0.0f, 0.0f, 4.0f, 0.3f, 0.0f, 0.25f, 0.0f,        // Tape: With the Pass, Invert off, 4 ms, 0.3 Hz, 0%, 25%, centre
        0.0f, 1.0f, 1.0f, 20.0f, 0.12f, 0.6f, 0.0f,       // Doubler: Random, Thicken on, Spread on, 20 ms, 12%, 0.6 Hz, 0%
        0.0f, 0.0f, 0.0f, 170.0f, 0.0f, 1.0f, 0.0f, 1.0f  // Filter: Low Pass, 12 dB, Sine, 170 Hz, 0%, 100%, 0%, 1 Hz
    };

    // Output EQ: after Space, before Inflate and the limiter. The session's (presets, Randomise, A/B and Small
    // Changes leave it), not a wire target.
    inline constexpr const char* eqOn            = "eqOn";
    inline constexpr const char* eqLowCut        = "eqLowCut";       // Hz; under eqLowCutLowestHz = Off
    inline constexpr const char* eqLowCutSlope   = "eqLowCutSlope";  // 12 / 24 dB an octave
    inline constexpr const char* eqLowShelfFreq  = "eqLowShelfFreq";
    inline constexpr const char* eqLowShelfGain  = "eqLowShelfGain";
    inline constexpr const char* eqLowMidFreq    = "eqLowMidFreq";
    inline constexpr const char* eqLowMidGain    = "eqLowMidGain";
    inline constexpr const char* eqLowMidQ       = "eqLowMidQ";
    inline constexpr const char* eqHighMidFreq   = "eqHighMidFreq";
    inline constexpr const char* eqHighMidGain   = "eqHighMidGain";
    inline constexpr const char* eqHighMidQ      = "eqHighMidQ";
    inline constexpr const char* eqHighShelfFreq = "eqHighShelfFreq";
    inline constexpr const char* eqHighShelfGain = "eqHighShelfGain";
    inline constexpr const char* eqHighCut       = "eqHighCut";      // Hz; over eqHighCutHighestHz = Off
    inline constexpr int numEqParams = 14;
    inline constexpr const char* eqIds[numEqParams] = { eqOn, eqLowCut, eqLowCutSlope, eqLowShelfFreq, eqLowShelfGain, eqLowMidFreq, eqLowMidGain,
                                                        eqLowMidQ, eqHighMidFreq, eqHighMidGain, eqHighMidQ, eqHighShelfFreq, eqHighShelfGain, eqHighCut };
    inline constexpr int numEqSlopes = 2;
    inline constexpr const char* eqSlopeNames[numEqSlopes] = { "12 dB", "24 dB" };
    // The cuts park at their end stops when Off: Low Cut's plain value is then eqLowCutOffHz, High Cut's eqHighCutOffHz.
    inline constexpr float eqLowCutOffHz = 10.0f, eqLowCutLowestHz = 20.0f, eqLowCutHighestHz = 1000.0f;
    inline constexpr float eqHighCutLowestHz = 1000.0f, eqHighCutHighestHz = 20000.0f, eqHighCutOffHz = 30000.0f;
    inline constexpr float eqLowShelfMinHz = 30.0f, eqLowShelfMaxHz = 600.0f, eqMidMinHz = 40.0f, eqMidMaxHz = 16000.0f;
    inline constexpr float eqHighShelfMinHz = 1000.0f, eqHighShelfMaxHz = 18000.0f, eqGainMaxDb = 12.0f, eqQMin = 0.3f, eqQMax = 8.0f;
    /** How much of each cut's travel, at its end stop, is Off. */
    inline constexpr float eqOffZone = 0.04f;
    // Pass Mode: Bed (two overlapped passes, a steady level) / Fly-By (one pass at a time). Linked: B uses A's.
    inline constexpr const char* passMode[numBuses] = { "aPassMode", "bPassMode" };
    // Flight Path: where a bus's passes fly in a room (Quad and wider). Linked: B uses A's. Stowaway's is Fly's.
    inline constexpr const char* flightPath[numBuses] = { "aFlightPath", "bFlightPath" };
    inline constexpr const char* stowFlightPath = "stowFlightPath";
    inline constexpr int numFlightPaths = 4;
    inline constexpr const char* flightPathNames[numFlightPaths] = { "Across", "Through", "Overhead", "Around" };
    // Direction: which way a bus's passes fly. Random (each pass picks its own), or one way or the other: what
    // that way is called follows the Flight Path (Params::directionLabel). Heard in Stereo too (an Across pass
    // goes left to right or right to left). Linked: B uses A's. Stowaway's is Fly's.
    // Side: which side of you a pass takes (Through, Overhead), or where a pass going Around is at its loudest
    // (Params::sideLabel); Across has no sides. Rooms only.
    // Each is a modulation target too: a modulator wired to it decides, pass by pass, as the pass starts.
    inline constexpr const char* direction[numBuses] = { "aDirection", "bDirection" };
    inline constexpr const char* stowDirection = "stowDirection";
    inline constexpr const char* side[numBuses] = { "aSide", "bSide" };
    inline constexpr const char* stowSide = "stowSide";
    inline constexpr int numDirections = 3;
    enum DirectionChoice { DirectionRandom = 0, DirectionForwards, DirectionBackwards };
    inline constexpr const char* directionNames[numDirections] = { "Random", "Forwards", "Backwards" };
    enum SideChoice { SideRandom = 0, SideLeft, SideRight };
    inline constexpr const char* sideNames[numDirections] = { "Random", "Left", "Right" };
    inline constexpr const char* directionTip =
        "Which way this bus's passes fly. Random: each pass picks its own way. Or always one way: left to right or "
        "right to left on Across (in Stereo too), front to back or back to front on Through and Overhead, clockwise "
        "or anticlockwise on Around. A modulator wired here decides instead, pass by pass, as each pass starts: "
        "above its middle is the first way, below it the other.";
    inline constexpr const char* sideTip =
        "Which side of you this bus's passes take on Through and Overhead: Random (each pass picks its own), always "
        "your left, or always your right. On Around: where the pass is at its loudest, in front of you or behind "
        "you. Across has no sides. A modulator wired here decides instead, pass by pass.";
    // The room itself (Quad and wider): how far a sound bleeds into the other speakers, how much of its height
    // reaches the top speakers, and whether the centre speaker is used.
    inline constexpr const char* roomSpread = "roomSpread";
    inline constexpr const char* roomHeight = "roomHeight";
    inline constexpr const char* roomCentre = "roomCentre";
    // Height Varies: how far each pass's own height may differ from the next one's (rooms with speakers overhead).
    inline constexpr const char* roomHeightVaries = "roomHeightVaries";
    inline constexpr const char* heightVariesTip =
        "How much the height of each pass varies. At 0% every Across, Through and Around pass flies at ear level and "
        "every Overhead pass flies its full arch. Turned up, each pass picks a height of its own: the others rise "
        "towards the speakers overhead, as high as an Overhead pass at 100%, and Overhead passes fly a little lower. "
        "Height then sets how much of that reaches the speakers overhead. Only at 7.1.2 and 7.1.4.";
    // The LFE (widths with one: 5.1 and wider; the others ignore both). What feeds it, and how loud it is.
    inline constexpr const char* roomLfeMode = "roomLfeMode";
    inline constexpr const char* roomLfeLevel = "roomLfeLevel";
    inline constexpr int numLfeModes = 3;
    inline constexpr const char* lfeModeNames[numLfeModes] = { "Off", "Discrete", "Mixdown" };
    inline constexpr float lfeLevelMinDb = -40.0f, lfeLevelMaxDb = 0.0f;
    inline constexpr float lfeLevelMainsDb = -10.0f; // a calibrated room plays the LFE 10 dB hotter: here it is as loud as a main speaker
    // Tooltips for the LFE controls (for the room strip).
    inline constexpr const char* lfeModeTip =
        "What plays from the LFE (the subwoofer channel). Off: nothing. Discrete: Stowaway's Sub, when Stowaway is "
        "on in Sub mode; it stays in the front speakers too, so nothing is lost without a subwoofer. Mixdown: the "
        "low end of the whole mix, with nothing else to set up. Either way only what is under 120 Hz. Only at 5.1 and wider.";
    inline constexpr const char* lfeLevelTip =
        "How loud the LFE is. At -10 dB (marked Mains, and by the tick on the knob) it plays in line with the main "
        "speakers in a calibrated room, which turns the LFE up by 10 dB. Higher adds weight; 0 dB is 10 dB louder than the mains.";

    // Stowaway: one extra bus beside A and B. A Mode menu, and five knobs that keep their names and places in
    // every mode (what each does, and how it reads, follows the mode: see Params::stowText).
    inline constexpr const char* stowOn = "stowOn";     // the bus is there at all: off, it is silent and its tracks wait
    inline constexpr const char* stowMode = "stowMode";
    enum StowMode { StowSub = 0, StowWake, StowFly, StowDry, numStowModes };
    inline constexpr const char* stowModeNames[numStowModes] = { "Sub", "Wake", "Fly", "Dry" };
    enum StowControl { StowFollow = 0, StowLow, StowFilter, StowDrive, StowWidth, numStowControls };
    inline constexpr const char* stow[numStowControls] = { "stowFollow", "stowLow", "stowFilter", "stowDrive", "stowWidth" };
    inline constexpr const char* stowNames[numStowControls] = { "Follow", "Low End", "Filter", "Drive", "Width" };

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
    // Where a hit lands (Stereo and wider), and how far it bleeds into the other speakers.
    inline constexpr const char* hitPosition = "hitPosition";
    inline constexpr const char* hitSpread   = "hitSpread";
    enum HitPositionChoice { HitWithPass = 0, HitFront, HitRandom, numHitPositions };
    inline constexpr const char* hitPositionNames[numHitPositions] = { "With the Pass", "Front", "Random" };
    inline constexpr float hitSpreadDefault = 0.2f;
    inline constexpr const char* hitPositionTip =
        "Where a hit lands. With the Pass: where the whoosh it belongs to is at that moment, left or right, in "
        "front or behind, and as high, and it stays with that whoosh as it flies on. With Aim on that is the pass "
        "the hit was aimed at; with Aim off, the loudest pass in the air as the hit starts. With nothing in the "
        "air it lands in front. Front: always in front of you, at ear level. Random: each hit at a place of its "
        "own somewhere round you. At Stereo the hit moves between left and right; at Mono it stays in the middle.";
    inline constexpr const char* hitSpreadTip =
        "How far a hit bleeds into the other speakers. 0%: a point at the place it lands. 100%: every speaker "
        "alike, never the LFE. A stereo hit keeps its left on your left and its right on your right, wider the "
        "further you turn this up, and stays as loud all the way. At Stereo it is how wide the hit is; at Mono "
        "it does nothing.";

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
    /** What a Direction choice is called on a Flight Path (0 Across, 1 Through, 2 Overhead, 3 Around): "Random",
        then "Left to Right" / "Right to Left", "Front to Back" / "Back to Front", "Clockwise" / "Anticlockwise". */
    inline const char* directionLabel(int path, int choice)
    {
        if (choice != ParamIDs::DirectionForwards && choice != ParamIDs::DirectionBackwards)
            return "Random";
        const bool forwards = choice == ParamIDs::DirectionForwards;
        switch (path)
        {
            case 0:  return forwards ? "Left to Right" : "Right to Left";
            case 3:  return forwards ? "Clockwise" : "Anticlockwise";
            default: return forwards ? "Front to Back" : "Back to Front";
        }
    }

    /** What a Side choice is called on a Flight Path: "Random", then "Left" / "Right" (Through, Overhead) or
        "Loudest in Front" / "Loudest Behind" (Around). Across has no sides: nullptr. */
    inline const char* sideLabel(int path, int choice)
    {
        if (path == 0)
            return nullptr;
        if (choice != ParamIDs::SideLeft && choice != ParamIDs::SideRight)
            return "Random";
        const bool first = choice == ParamIDs::SideLeft;
        return path == 3 ? (first ? "Loudest in Front" : "Loudest Behind") : (first ? "Left" : "Right");
    }

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
    /** What a Rate wheel reads: in Faders Together the length of the one swell, otherwise the ride's own time. */
    inline juce::String rateText(float rate, bool together)
    {
        return together ? juce::String(passSeconds(rate), 1) + " s" : ridePeriodText(rate);
    }
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

    // Stowaway's Filter knob.
    // Sub: the crossover, 40 .. 250 Hz, with 120 Hz at the middle of the travel (the knob's default). One smooth
    // curve through the three: Hz = 128 x 2.640625^k - 88.
    inline constexpr float stowCrossoverMinHz = 40.0f, stowCrossoverMaxHz = 250.0f, stowCrossoverDefaultHz = 120.0f;
    inline float stowCrossoverHz(float k) { return 128.0f * std::pow(2.640625f, std::clamp(k, 0.0f, 1.0f)) - 88.0f; }
    inline float stowCrossoverKnob(float hz)
    {
        return std::clamp(std::log((std::clamp(hz, stowCrossoverMinHz, stowCrossoverMaxHz) + 88.0f) / 128.0f) / std::log(2.640625f), 0.0f, 1.0f);
    }
    // The other modes: open in the middle of the travel, a low-pass that closes towards the bottom (18 kHz ..
    // 150 Hz) and a high-pass that rises towards the top (20 Hz .. 2.5 kHz). 0 = that side is open.
    inline constexpr float stowFilterOpenZone = 0.02f; // either side of the middle
    inline float stowLowpassHz(float k)
    {
        const float u = (0.5f - stowFilterOpenZone - std::clamp(k, 0.0f, 1.0f)) / (0.5f - stowFilterOpenZone);
        return u <= 0.0f ? 0.0f : 18000.0f * std::pow(150.0f / 18000.0f, u);
    }
    inline float stowHighpassHz(float k)
    {
        const float u = (std::clamp(k, 0.0f, 1.0f) - 0.5f - stowFilterOpenZone) / (0.5f - stowFilterOpenZone);
        return u <= 0.0f ? 0.0f : 20.0f * std::pow(2500.0f / 20.0f, u);
    }
    inline juce::String hzText(float hz)
    {
        if (hz < 999.5f)
            return juce::String(juce::roundToInt(hz)) + " Hz";
        return (hz < 9950.0f ? juce::String(hz / 1000.0f, 1) : juce::String(juce::roundToInt(hz / 1000.0f))) + " kHz";
    }
    /** How LFE Level reads: "-10.0 dB (Mains)" at the reference (where the LFE plays in line with the main speakers in a
        calibrated room), so it is easy to find; "-18.5 dB" elsewhere. */
    inline juce::String lfeLevelText(float db)
    {
        const float v = std::round(std::clamp(db, ParamIDs::lfeLevelMinDb, ParamIDs::lfeLevelMaxDb) * 10.0f) / 10.0f;
        const juce::String text = juce::String(v, 1) + " dB";
        return std::abs(v - ParamIDs::lfeLevelMainsDb) < 0.05f ? text + " (Mains)" : text;
    }
    /** What a typed LFE Level means. The level is never above 0 dB, so a number typed without its minus sign is
        read as that many dB down ("10" is -10 dB, not a clamp to 0); "Mains" (or "Unity") finds the reference. */
    inline float lfeLevelFromText(const juce::String& text)
    {
        if (text.containsIgnoreCase("mains") || text.containsIgnoreCase("unity"))
            return ParamIDs::lfeLevelMainsDb;
        const float typed = text.retainCharacters("-.0123456789").getFloatValue();
        return juce::jlimit(ParamIDs::lfeLevelMinDb, ParamIDs::lfeLevelMaxDb, -std::abs(typed));
    }
    /** How a Stowaway knob reads at position v in a mode (ParamIDs::StowMode, ParamIDs::StowControl). The knobs
        keep their names; Filter reads the Sub's crossover in Hz, and otherwise what it lets through ("Under 2.4 kHz"
        for the low-pass side, "Over 320 Hz" for the high-pass side, "Open" between), and Width says Mono in Sub
        (a sub is planted in the middle). */
    inline juce::String stowText(int mode, int control, float v)
    {
        if (control == ParamIDs::StowFilter)
        {
            if (mode == ParamIDs::StowSub)
                return hzText(stowCrossoverHz(v));
            if (const float lp = stowLowpassHz(v); lp > 0.0f)
                return "Under " + hzText(lp);
            if (const float hp = stowHighpassHz(v); hp > 0.0f)
                return "Over " + hzText(hp);
            return "Open";
        }
        if (control == ParamIDs::StowWidth && mode == ParamIDs::StowSub)
            return "Mono";
        return juce::String(juce::roundToInt(v * 100.0f)) + "%";
    }
    /** Whether a Stowaway knob does anything in a mode. The knobs never move or change their names; one that is
        out of use is dimmed. Sub is mono: Width is out of use there (and reads "Mono"). Dry skips the colours: Low
        End and Drive are out of use there (its Width is the width of the stereo sounds sent to it). */
    inline bool stowControlInUse(int mode, int control)
    {
        if (mode == ParamIDs::StowSub)
            return control != ParamIDs::StowWidth;
        if (mode == ParamIDs::StowDry)
            return control != ParamIDs::StowLow && control != ParamIDs::StowDrive;
        return true;
    }

    /** Where the Stowaway knobs' readouts find the Mode in use: the owner of the parameters points `mode` at the
        Mode parameter's value once it exists (until then they read as in Sub), and `knobs` at the five knobs'
        own values (what a typed value that does not say where on the knob it means is read against). */
    struct StowModeSource
    {
        std::atomic<const std::atomic<float>*> mode { nullptr };
        std::array<std::atomic<const std::atomic<float>*>, ParamIDs::numStowControls> knobs {};
        int get() const
        {
            const auto* raw = mode.load(std::memory_order_relaxed);
            return raw != nullptr ? std::clamp(juce::roundToInt(raw->load(std::memory_order_relaxed)), 0, ParamIDs::numStowModes - 1) : 0;
        }
        /** A knob's position now (its default until the parameters exist). */
        float knob(int control, float fallback) const
        {
            const auto* raw = knobs[static_cast<size_t>(control)].load(std::memory_order_relaxed);
            return raw != nullptr ? std::clamp(raw->load(std::memory_order_relaxed), 0.0f, 1.0f) : fallback;
        }
    };
    /** The position of a Stowaway knob that reads as `text` in a mode: what a host hands back when a value is typed
        into its own parameter list or automation lane. Read off the readouts themselves (TypedValue), so "95 Hz",
        "Under 2.4 kHz", "Over 320 Hz", "Open" and "35%" all work. A number with no "Under" or "Over" is looked for
        on the side of Filter the knob is on now, then on the other. Text that says nothing about where (Width's
        "Mono" in Sub, or something that can't be read) leaves the knob where it is. */
    inline float stowValueFromText(int mode, int control, const juce::String& text, float current)
    {
        if (control == ParamIDs::StowWidth && mode == ParamIDs::StowSub)
        {
            // Every position reads "Mono" here: a number is still taken as the width it will have in the other modes.
            const auto typed = TypedValue::parse(text);
            return typed.isNumber ? std::clamp(static_cast<float>(typed.value) / 100.0f, 0.0f, 1.0f) : current;
        }
        const float n = TypedValue::toNormalised([mode, control](float v) { return stowText(mode, control, v); }, text, current);
        return n >= 0.0f ? n : current;
    }

    // The detail layers and the Output EQ: laws and readouts. ----------------------------------------------------

    /** A range whose travel is even in octaves (lo .. hi, both above 0). */
    inline juce::NormalisableRange<float> logRange(float lo, float hi)
    {
        return { lo, hi,
                 [](float start, float end, float n) { return start * std::pow(end / start, std::clamp(n, 0.0f, 1.0f)); },
                 [](float start, float end, float v) { return std::clamp(std::log(std::clamp(v, start, end) / start) / std::log(end / start), 0.0f, 1.0f); },
                 [](float start, float end, float v) { return std::clamp(v, start, end); } };
    }
    /** Low Cut: Off at the bottom of its travel (eqOffZone of it), then 20 Hz .. 1 kHz even in octaves. */
    inline juce::NormalisableRange<float> eqLowCutRange()
    {
        using namespace ParamIDs;
        return { eqLowCutOffHz, eqLowCutHighestHz,
                 [](float, float, float n)
                 {
                     return n < eqOffZone ? eqLowCutOffHz
                                          : eqLowCutLowestHz * std::pow(eqLowCutHighestHz / eqLowCutLowestHz, std::clamp((n - eqOffZone) / (1.0f - eqOffZone), 0.0f, 1.0f));
                 },
                 [](float, float, float v)
                 {
                     return v < 0.5f * (eqLowCutOffHz + eqLowCutLowestHz) ? 0.0f
                            : eqOffZone + (1.0f - eqOffZone) * std::clamp(std::log(std::max(v, eqLowCutLowestHz) / eqLowCutLowestHz)
                                                                          / std::log(eqLowCutHighestHz / eqLowCutLowestHz), 0.0f, 1.0f);
                 },
                 [](float, float, float v) { return v < 0.5f * (eqLowCutOffHz + eqLowCutLowestHz) ? eqLowCutOffHz : std::clamp(v, eqLowCutLowestHz, eqLowCutHighestHz); } };
    }
    /** High Cut: 1 kHz .. 20 kHz even in octaves, then Off at the top of its travel (eqOffZone of it). */
    inline juce::NormalisableRange<float> eqHighCutRange()
    {
        using namespace ParamIDs;
        return { eqHighCutLowestHz, eqHighCutOffHz,
                 [](float, float, float n)
                 {
                     return n > 1.0f - eqOffZone ? eqHighCutOffHz
                                                 : eqHighCutLowestHz * std::pow(eqHighCutHighestHz / eqHighCutLowestHz, std::clamp(n / (1.0f - eqOffZone), 0.0f, 1.0f));
                 },
                 [](float, float, float v)
                 {
                     return v > 0.5f * (eqHighCutHighestHz + eqHighCutOffHz) ? 1.0f
                            : (1.0f - eqOffZone) * std::clamp(std::log(std::max(v, eqHighCutLowestHz) / eqHighCutLowestHz)
                                                              / std::log(eqHighCutHighestHz / eqHighCutLowestHz), 0.0f, 1.0f);
                 },
                 [](float, float, float v) { return v > 0.5f * (eqHighCutHighestHz + eqHighCutOffHz) ? eqHighCutOffHz : std::clamp(v, eqHighCutLowestHz, eqHighCutHighestHz); } };
    }
    /** The cuts: is this plain value Off? */
    inline bool eqLowCutIsOff(float hz) { return hz < ParamIDs::eqLowCutLowestHz; }
    inline bool eqHighCutIsOff(float hz) { return hz > ParamIDs::eqHighCutHighestHz; }

    /** Seconds as the swell controls read: "0.60 s" under 1 s, "4.0 s" under 10 s, "12 s" above. */
    inline juce::String secondsText(float s)
    {
        return s < 0.995f ? juce::String(s, 2) + " s" : (s < 9.95f ? juce::String(s, 1) + " s" : juce::String(juce::roundToInt(s)) + " s");
    }
    /** Rates in Hz: "0.30 Hz" under 1 Hz, "2.5 Hz" under 10 Hz, "12 Hz" above. */
    inline juce::String rateText(float hz)
    {
        return hz < 0.995f ? juce::String(hz, 2) + " Hz" : (hz < 9.95f ? juce::String(hz, 1) + " Hz" : juce::String(juce::roundToInt(hz)) + " Hz");
    }
    /** -1 .. 1 as a signed percentage of `scale` ("+55%", "0%", "-30%"). */
    inline juce::String signedPercentText(float v, float scale = 100.0f)
    {
        const int pct = juce::roundToInt(v * scale);
        return (pct > 0 ? "+" : "") + juce::String(pct) + "%";
    }
    inline juce::String gainDbText(float db)
    {
        const float v = std::round(db * 10.0f) / 10.0f;
        return (v > 0.0f ? "+" : "") + juce::String(v == 0.0f ? 0.0f : v, 1) + " dB";
    }

    /** Each new control's tooltip body, in plain words (the heading, its name, is added by whoever shows it). Bus
        detail IDs are found by what follows the bus letter. Empty for anything else. */
    inline juce::String tipFor(const juce::String& id)
    {
        using namespace ParamIDs;
        if (id == rideMode)
            return "How the faders ride. Faders Apart: each track's fader on a slow wave of its own, so the swells overlap and never "
                   "repeat exactly. Faders Together: every fader on one repeating swell, each bus's pass and the aimed hit on its peak; "
                   "Rate sets how long a swell is and Chaos varies its size, never its timing. Follow Master: every ride swells with "
                   "the Master track's loudness; it needs a Master track (the star on a lane).";
        if (id == togetherPeakAt)
            return "Where in each swell its top falls. 50%: halfway, a build as long as the fall. 70%: a long build and a short tail. "
                   "Only in Faders Together.";
        if (id == togetherStagger)
            return "Fans the tracks in, one after another in track order, up to a tenth of a swell apart. The order stays the same "
                   "every swell, so it repeats. 0%: every fader peaks together. Only in Faders Together.";
        if (id == togetherHitEvery)
            return "Which peaks get a hit: every one, or every second, third or fourth. Only in Faders Together with Aim on.";
        if (id == swell)
            return "Swell as a host control: hold it to push every fader up at once. Recorded by a host when you press the Swell "
                   "button, its key or the sea, so the presses play back.";
        if (id == swellAuto)
            return "Swell firing by itself while the rig plays. On Peaks: just before each peak of bus A's passes. On a Clock: every "
                   "so often, as Every says. A press of Swell still works on top. It rests while Auto Ride is off or Energy is 0, and "
                   "On Peaks rests in Faders Together (the faders already rise to every peak there).";
        if (id == swellEvery)
            return "How often Auto Swell fires on a clock.";
        if (id == swellHold)
            return "How long each Auto Swell is held. Never more than 60% of Every.";
        if (id == swellChance)
            return "The chance that each Auto Swell fires. 100%: every one; 50%: about half of them, at random.";
        if (id == swellLoose)
            return "How far each Auto Swell on a clock may wander from its place, as a share of Every. 0%: exactly on the clock.";
        if (id == eqOn)
            return "The Output EQ, after Space and before Inflate and the limiter: one curve on every speaker, the LFE too. "
                   "Off: it passes the sound untouched.";
        if (id == eqLowCut)
            return "A high-pass that takes the low end away under this frequency. Off at the far left.";
        if (id == eqLowCutSlope)
            return "How steeply Low Cut falls away: 12 or 24 dB an octave.";
        if (id == eqLowShelfFreq || id == eqLowShelfGain)
            return "Lifts or lowers everything under this frequency.";
        if (id == eqLowMidFreq || id == eqLowMidGain || id == eqHighMidFreq || id == eqHighMidGain)
            return "Lifts or lowers a band around this frequency.";
        if (id == eqLowMidQ || id == eqHighMidQ)
            return "How narrow the band is: higher is narrower.";
        if (id == eqHighShelfFreq || id == eqHighShelfGain)
            return "Lifts or lowers everything over this frequency.";
        if (id == eqHighCut)
            return "A low-pass that takes the top end away over this frequency. Off at the far right.";

        if (! (id.startsWith("a") || id.startsWith("b")))
            return {};
        const auto rest = id.substring(1);
        if (rest == "SwirlType")
            return "What Swirl is. Phaser + Flanger: the phaser, with the flanger coming in from about a seventh of the knob. Phaser: "
                   "the phaser alone, its notches through the middle of the sound. Flanger: the flanger alone, sweeping up as each "
                   "pass comes close and back down as it leaves. Tape Flanger: a second copy of the bus on a moving delay that runs "
                   "either side of the first, so the two pass through zero. Doubler: one delayed copy that wanders a little, "
                   "for a thicker sound. The Swirl wheel is how much, whatever it is.";
        if (rest == "SwirlFeedback")
            return "How much of the swirl is fed back into itself. More is a stronger, more ringing colour; below 0% the feedback is "
                   "turned upside down, which moves the notches.";
        if (rest == "SwirlSpeed")
            return "How fast the swirl sweeps, against the speed the whoosh gives it.";
        if (rest == "SwirlDepth")
            return "How far the swirl's own sweep goes.";
        if (rest == "SwirlFollow")
            return "How far the sweep follows the whoosh. 100%: it opens and closes with each pass. 0%: it sweeps by itself.";
        if (rest == "TapeMotion")
            return "What moves the second copy. With the Pass: it runs late while the pass is far off and passes through zero just "
                   "before the pass is closest. On Its Own: a slow sweep at Rate. By Hand: it waits Depth behind the first, and only "
                   "Varispeed moves it: lean faster to bring it through zero.";
        if (rest == "TapeInvert")
            return "Turns the second copy upside down, so it cancels the first where the two meet: the sound thins right out "
                   "there, the low end first and for longest. Off: the two add up there, and the low end keeps its weight.";
        if (rest == "TapeDepth")
            return "How far the second copy moves either side of the first. By Hand: how far behind it waits.";
        if (rest == "TapeRate")
            return "How fast the second copy sweeps On Its Own.";
        if (rest == "TapeFeedback")
            return "How much of the second copy is fed back into itself, either way up. It never runs away.";
        if (rest == "TapeWobble")
            return "A slow settle after each move, like a motor catching up.";
        if (rest == "Varispeed")
            return "Lean the second tape machine faster (right) or slower (left). The gap between the copies drifts for as long as "
                   "you lean, and stays where it got to when you let go: lean the other way to bring it back, or double-click the "
                   "meter under it. It goes back to the middle when the transport starts.";
        if (rest == "DoublerWave")
            return "How the delay wanders: Random, a smooth drift; Sine or Triangle, a regular sway.";
        if (rest == "DoublerThicken")
            return "Feeds the copy back upside down, by an amount set from the delay, so a short delay sounds thicker instead of "
                   "like a flanger. Feedback rests while it is on.";
        if (rest == "DoublerSpread")
            return "Moves the second side's copy the other way: between your ears in Stereo, between the passes in a room.";
        if (rest == "DoublerDelay")
            return "How far behind the copy plays. Fine at the short end.";
        if (rest == "DoublerWander")
            return "How much the delay moves.";
        if (rest == "DoublerRate")
            return "How fast the delay wanders.";
        if (rest == "DoublerFeedback")
            return "How much of the copy is fed back into itself, either way up. Rests while Thicken is on.";
        if (rest == "FilterType")
            return "What the filter is: Low Pass (it closes in the tail), High Pass (it closes from the bottom, opening downward), "
                   "Band Pass or Notch.";
        if (rest == "FilterSteep")
            return "Doubles how steeply the filter falls away: 24 dB an octave instead of 12.";
        if (rest == "FilterShape")
            return "The shape of the filter's own rhythm.";
        if (rest == "FilterClosedAt")
            return "Where the filter sits in the tail of a pass, when it is most closed.";
        if (rest == "FilterResonance")
            return "A peak at the edge of the filter, added to what the Filter wheel gives.";
        if (rest == "FilterFollow")
            return "How far the filter follows the whoosh. 100%: it opens with each pass. 0%: it stays put and the Filter wheel is "
                   "a plain cutoff.";
        if (rest == "FilterSweep")
            return "How far the filter's own rhythm moves it, up to three octaves either way.";
        if (rest == "FilterRate")
            return "How fast the filter's own rhythm runs. Rests while Sweep is 0%.";
        return {};
    }

    inline void addLayerParameters(juce::AudioProcessorValueTreeState::ParameterLayout& layout); // below

    inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout(std::shared_ptr<StowModeSource> stowModeSource = std::make_shared<StowModeSource>())
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

        // Stowaway: its switch (off until it is wanted), the Mode, then the five knobs. Each knob reads by the mode
        // in use (see stowText).
        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::stowOn, 4 }, "Stowaway", false));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::stowMode, 4 }, "Stowaway Mode",
                                                          StringArray(ParamIDs::stowModeNames, ParamIDs::numStowModes), ParamIDs::StowSub));
        const float stowDefaults[ParamIDs::numStowControls] = { 0.7f, 0.0f, 0.5f, 0.0f, 1.0f }; // Filter: 120 Hz in Sub, open otherwise
        for (int k = 0; k < ParamIDs::numStowControls; ++k)
            layout.add(std::make_unique<P>(ParameterID { ParamIDs::stow[k], 4 }, String("Stowaway ") + ParamIDs::stowNames[k],
                                           NormalisableRange<float>(0.0f, 1.0f), stowDefaults[k],
                                           AudioParameterFloatAttributes()
                                               .withStringFromValueFunction([stowModeSource, k](float v, int) { return stowText(stowModeSource->get(), k, v); })
                                               .withValueFromStringFunction([stowModeSource, k, fallback = stowDefaults[k]](const String& t)
                                               {
                                                   return stowValueFromText(stowModeSource->get(), k, t, stowModeSource->knob(k, fallback));
                                               })));

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
                                                              StringArray { "Bus A", "Bus B", "Hits", "Stowaway", "Output", "Master", "Master Pitch", "Master Brightness" }, 0));
        }

        // The room (Quad and wider): each bus's Flight Path, Stowaway's (Fly), and the room's own settings.
        for (int b = 0; b < ParamIDs::numBuses; ++b)
            layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::flightPath[b], 5 }, String("Bus ") + ParamIDs::busNames[b] + " Flight Path",
                                                              StringArray(ParamIDs::flightPathNames, ParamIDs::numFlightPaths), 0));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::stowFlightPath, 5 }, "Stowaway Flight Path",
                                                          StringArray(ParamIDs::flightPathNames, ParamIDs::numFlightPaths), 0));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::roomSpread, 5 }, "Room Spread", NormalisableRange<float>(0.0f, 1.0f), 0.0f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return String(roundToInt(v * 100.0f)) + "%"; })));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::roomHeight, 5 }, "Room Height", NormalisableRange<float>(0.0f, 1.0f), 1.0f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return String(roundToInt(v * 100.0f)) + "%"; })));
        layout.add(std::make_unique<AudioParameterBool>(ParameterID { ParamIDs::roomCentre, 5 }, "Centre Speaker", false));
        // Which way and which side each bus's passes fly (and Stowaway's, in Fly), and how much their height varies.
        for (int b = 0; b < ParamIDs::numBuses; ++b)
        {
            layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::direction[b], 6 }, String("Bus ") + ParamIDs::busNames[b] + " Direction",
                                                              StringArray(ParamIDs::directionNames, ParamIDs::numDirections), 0));
            layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::side[b], 6 }, String("Bus ") + ParamIDs::busNames[b] + " Side",
                                                              StringArray(ParamIDs::sideNames, ParamIDs::numDirections), 0));
        }
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::stowDirection, 6 }, "Stowaway Direction",
                                                          StringArray(ParamIDs::directionNames, ParamIDs::numDirections), 0));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::stowSide, 6 }, "Stowaway Side",
                                                          StringArray(ParamIDs::sideNames, ParamIDs::numDirections), 0));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::roomHeightVaries, 6 }, "Room Height Varies", NormalisableRange<float>(0.0f, 1.0f), 0.5f,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return String(roundToInt(v * 100.0f)) + "%"; })));
        // Where a hit lands, and how far it bleeds.
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::hitPosition, 7 }, "Hit Position",
                                                          StringArray(ParamIDs::hitPositionNames, ParamIDs::numHitPositions), 0));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::hitSpread, 7 }, "Hit Spread", NormalisableRange<float>(0.0f, 1.0f), ParamIDs::hitSpreadDefault,
                                       AudioParameterFloatAttributes().withStringFromValueFunction(
                                           [](float v, int) { return String(roundToInt(v * 100.0f)) + "%"; })));
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::roomLfeMode, 5 }, "LFE Mode",
                                                          StringArray(ParamIDs::lfeModeNames, ParamIDs::numLfeModes), 0));
        layout.add(std::make_unique<P>(ParameterID { ParamIDs::roomLfeLevel, 5 }, "LFE Level",
                                       NormalisableRange<float>(ParamIDs::lfeLevelMinDb, ParamIDs::lfeLevelMaxDb, 0.1f), ParamIDs::lfeLevelMainsDb,
                                       AudioParameterFloatAttributes().withLabel("dB")
                                           .withStringFromValueFunction([](float v, int) { return lfeLevelText(v); })
                                           .withValueFromStringFunction([](const String& t) { return lfeLevelFromText(t); })));

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
        // Faders Apart / Faders Together / Follow Master (where Follow Master's switch was).
        layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ParamIDs::rideMode, 8 }, "Ride Mode",
                                                          StringArray(ParamIDs::rideModeNames, ParamIDs::numRideModes), ParamIDs::RideApart));
        addLayerParameters(layout);
        return layout;
    }
}

namespace Params
{
    /** The parameters added with the detail layers, Faders Together, Auto Swell and the Output EQ (78 with Ride Mode),
        in the order the host lists them, after every older one. */
    inline void addLayerParameters(juce::AudioProcessorValueTreeState::ParameterLayout& layout)
    {
        using namespace juce;
        using P = AudioParameterFloat;
        constexpr int hint = 8;
        auto percent = [](float v, int) { return String(roundToInt(v * 100.0f)) + "%"; };
        auto floatParam = [&layout](const char* id, const String& name, NormalisableRange<float> range, float def,
                                    std::function<String(float, int)> text, const String& label = {})
        {
            layout.add(std::make_unique<P>(ParameterID { id, hint }, name, range, def,
                                           AudioParameterFloatAttributes().withLabel(label).withStringFromValueFunction(std::move(text))));
        };
        auto choiceParam = [&layout](const char* id, const String& name, const char* const* names, int count, int def)
        {
            layout.add(std::make_unique<AudioParameterChoice>(ParameterID { id, hint }, name, StringArray(names, count), def));
        };
        auto boolParam = [&layout](const char* id, const String& name, bool def)
        {
            layout.add(std::make_unique<AudioParameterBool>(ParameterID { id, hint }, name, def));
        };

        // Faders Together (behind Rate).
        floatParam(ParamIDs::togetherPeakAt, "Together Peak At", NormalisableRange<float>(ParamIDs::togetherPeakAtMin, ParamIDs::togetherPeakAtMax), 0.5f, percent);
        floatParam(ParamIDs::togetherStagger, "Together Stagger", NormalisableRange<float>(0.0f, 1.0f), 0.0f, percent);
        choiceParam(ParamIDs::togetherHitEvery, "Together Hit Every", ParamIDs::hitEveryNames, ParamIDs::numHitEvery, 0);

        // Swell as a host control, and Auto Swell (behind Swell).
        boolParam(ParamIDs::swell, "Swell", false);
        choiceParam(ParamIDs::swellAuto, "Auto Swell", ParamIDs::autoSwellNames, ParamIDs::numAutoSwells, ParamIDs::AutoSwellOff);
        floatParam(ParamIDs::swellEvery, "Auto Swell Every", logRange(ParamIDs::swellEveryMin, ParamIDs::swellEveryMax), 4.0f,
                   [](float v, int) { return secondsText(v); }, "s");
        floatParam(ParamIDs::swellHold, "Auto Swell Hold", logRange(ParamIDs::swellHoldMin, ParamIDs::swellHoldMax), 0.6f,
                   [](float v, int) { return secondsText(v); }, "s");
        floatParam(ParamIDs::swellChance, "Auto Swell Chance", NormalisableRange<float>(0.0f, 1.0f), 1.0f, percent);
        floatParam(ParamIDs::swellLoose, "Auto Swell Loose", NormalisableRange<float>(0.0f, ParamIDs::swellLooseMax), 0.0f, percent);

        // What is behind each bus's Swirl and Filter.
        for (int b = 0; b < ParamIDs::numBuses; ++b)
        {
            const auto* ids = ParamIDs::busDetail[b];
            const String bus = String("Bus ") + ParamIDs::busNames[b] + " ";
            auto name = [&bus](int k) { return bus + ParamIDs::busDetailNames[k]; };
            const auto* def = ParamIDs::busDetailDefaults;
            using namespace ParamIDs;
            choiceParam(ids[SwirlType], name(SwirlType), swirlTypeNames, numSwirlTypes, SwirlBoth);
            floatParam(ids[SwirlFeedback], name(SwirlFeedback), NormalisableRange<float>(-1.0f, 1.0f), def[SwirlFeedback],
                       [](float v, int) { return signedPercentText(v); });
            floatParam(ids[SwirlSpeed], name(SwirlSpeed), logRange(swirlSpeedMin, swirlSpeedMax), def[SwirlSpeed],
                       [](float v, int) { return (v < 0.995f ? String(v, 2) : String(v, 1)) + "x"; });
            floatParam(ids[SwirlDepth], name(SwirlDepth), NormalisableRange<float>(0.0f, 1.0f), def[SwirlDepth], percent);
            floatParam(ids[SwirlFollow], name(SwirlFollow), NormalisableRange<float>(0.0f, 1.0f), def[SwirlFollow], percent);

            choiceParam(ids[TapeMotion], name(TapeMotion), tapeMotionNames, numTapeMotions, TapeWithPass);
            boolParam(ids[TapeInvert], name(TapeInvert), def[TapeInvert] > 0.5f);
            floatParam(ids[TapeDepth], name(TapeDepth), NormalisableRange<float>(0.0f, tapeDepthMaxMs), def[TapeDepth],
                       [](float v, int) { return String(v, 1) + " ms"; }, "ms");
            floatParam(ids[TapeRate], name(TapeRate), logRange(tapeRateMin, tapeRateMax), def[TapeRate],
                       [](float v, int) { return rateText(v); }, "Hz");
            floatParam(ids[TapeFeedback], name(TapeFeedback), NormalisableRange<float>(-1.0f, 1.0f), def[TapeFeedback],
                       [](float v, int) { return signedPercentText(v); });
            floatParam(ids[TapeWobble], name(TapeWobble), NormalisableRange<float>(0.0f, 1.0f), def[TapeWobble], percent);
            floatParam(ids[Varispeed], name(Varispeed), NormalisableRange<float>(-1.0f, 1.0f), def[Varispeed],
                       [](float v, int) { return signedPercentText(v); });

            choiceParam(ids[DoublerWave], name(DoublerWave), doublerWaveNames, numDoublerWaves, WaveRandom);
            boolParam(ids[DoublerThicken], name(DoublerThicken), def[DoublerThicken] > 0.5f);
            boolParam(ids[DoublerSpread], name(DoublerSpread), def[DoublerSpread] > 0.5f);
            // Delay: 3 x 25^k ms over the travel, so the short end is fine.
            floatParam(ids[DoublerDelay], name(DoublerDelay), logRange(doublerDelayMinMs, doublerDelayMaxMs), def[DoublerDelay],
                       [](float v, int) { return (v < 9.95f ? String(v, 1) : String(roundToInt(v))) + " ms"; }, "ms");
            floatParam(ids[DoublerWander], name(DoublerWander), NormalisableRange<float>(0.0f, 1.0f), def[DoublerWander], percent);
            floatParam(ids[DoublerRate], name(DoublerRate), logRange(doublerRateMin, doublerRateMax), def[DoublerRate],
                       [](float v, int) { return rateText(v); }, "Hz");
            floatParam(ids[DoublerFeedback], name(DoublerFeedback), NormalisableRange<float>(-1.0f, 1.0f), def[DoublerFeedback],
                       [](float v, int) { return signedPercentText(v, 100.0f * doublerFeedbackMax); });

            choiceParam(ids[FilterType], name(FilterType), filterTypeNames, numFilterTypes, FilterLowPass);
            boolParam(ids[FilterSteep], name(FilterSteep), false);
            choiceParam(ids[FilterShape], name(FilterShape), filterShapeNames, numFilterShapes, ShapeSine);
            floatParam(ids[FilterClosedAt], name(FilterClosedAt), logRange(filterClosedMinHz, filterClosedMaxHz), def[FilterClosedAt],
                       [](float v, int) { return hzText(v); }, "Hz");
            floatParam(ids[FilterResonance], name(FilterResonance), NormalisableRange<float>(0.0f, 1.0f), def[FilterResonance], percent);
            floatParam(ids[FilterFollow], name(FilterFollow), NormalisableRange<float>(0.0f, 1.0f), def[FilterFollow], percent);
            floatParam(ids[FilterSweep], name(FilterSweep), NormalisableRange<float>(0.0f, 1.0f), def[FilterSweep], percent);
            floatParam(ids[FilterRate], name(FilterRate), logRange(filterRateMin, filterRateMax), def[FilterRate],
                       [](float v, int) { return rateText(v); }, "Hz");
        }

        // Output EQ.
        {
            using namespace ParamIDs;
            boolParam(eqOn, "Output EQ", true);
            floatParam(eqLowCut, "EQ Low Cut", eqLowCutRange(), eqLowCutOffHz,
                       [](float v, int) { return eqLowCutIsOff(v) ? String("Off") : hzText(v); }, "Hz");
            choiceParam(eqLowCutSlope, "EQ Low Cut Slope", eqSlopeNames, numEqSlopes, 1);
            auto hz = [](float v, int) { return hzText(v); };
            auto db = [](float v, int) { return gainDbText(v); };
            auto q = [](float v, int) { return v < 0.995f ? String(v, 2) : String(v, 1); };
            const NormalisableRange<float> gain(-eqGainMaxDb, eqGainMaxDb);
            floatParam(eqLowShelfFreq, "EQ Low Shelf Frequency", logRange(eqLowShelfMinHz, eqLowShelfMaxHz), 100.0f, hz, "Hz");
            floatParam(eqLowShelfGain, "EQ Low Shelf Gain", gain, 0.0f, db, "dB");
            floatParam(eqLowMidFreq, "EQ Low Mid Frequency", logRange(eqMidMinHz, eqMidMaxHz), 300.0f, hz, "Hz");
            floatParam(eqLowMidGain, "EQ Low Mid Gain", gain, 0.0f, db, "dB");
            floatParam(eqLowMidQ, "EQ Low Mid Q", logRange(eqQMin, eqQMax), 1.0f, q);
            floatParam(eqHighMidFreq, "EQ High Mid Frequency", logRange(eqMidMinHz, eqMidMaxHz), 3000.0f, hz, "Hz");
            floatParam(eqHighMidGain, "EQ High Mid Gain", gain, 0.0f, db, "dB");
            floatParam(eqHighMidQ, "EQ High Mid Q", logRange(eqQMin, eqQMax), 1.0f, q);
            floatParam(eqHighShelfFreq, "EQ High Shelf Frequency", logRange(eqHighShelfMinHz, eqHighShelfMaxHz), 8000.0f, hz, "Hz");
            floatParam(eqHighShelfGain, "EQ High Shelf Gain", gain, 0.0f, db, "dB");
            floatParam(eqHighCut, "EQ High Cut", eqHighCutRange(), eqHighCutOffHz,
                       [](float v, int) { return eqHighCutIsOff(v) ? String("Off") : hzText(v); }, "Hz");
        }
    }
}

/**
 * The detail layers: what is behind a notched control. Each layer belongs to one control on the front (a bus's
 * Swirl or Filter wheel, Rate, or the Swell button) and holds the parameters listed here. The layer host
 * (DetailLayer) and the pages (LayerPages) both go by these lists.
 */
namespace Layers
{
    enum class Kind { Swirl = 0, Filter, Together, Swell };
    struct Target
    {
        Kind kind = Kind::Swirl;
        int bus = 0; // Swirl and Filter: 0 bus A, 1 bus B. Together and Swell: ignored (they are Ride's)
        bool operator==(const Target& o) const { return kind == o.kind && (bus == o.bus || kind == Kind::Together || kind == Kind::Swell); }
        bool operator!=(const Target& o) const { return ! (*this == o); }
    };

    /** "Bus A / Swirl", "Bus B / Filter", "Ride / Together", "Ride / Swell". */
    inline juce::String title(Target t)
    {
        const juce::String bus = juce::String("Bus ") + ParamIDs::busNames[std::clamp(t.bus, 0, ParamIDs::numBuses - 1)];
        switch (t.kind)
        {
            case Kind::Swirl:    return bus + " / Swirl";
            case Kind::Filter:   return bus + " / Filter";
            case Kind::Together: return "Ride / Together";
            case Kind::Swell:    return "Ride / Swell";
        }
        return {};
    }

    /** The front control the layer belongs to: its parameter (Swirl, Filter, Rate), or nullptr for Swell (its front is
        the Swell button, not a wheel). */
    inline const char* frontId(Target t)
    {
        const int b = std::clamp(t.bus, 0, ParamIDs::numBuses - 1);
        switch (t.kind)
        {
            case Kind::Swirl:    return ParamIDs::bus[b][ParamIDs::Swirl];
            case Kind::Filter:   return ParamIDs::bus[b][ParamIDs::Filter];
            case Kind::Together: return ParamIDs::rate;
            case Kind::Swell:    return nullptr;
        }
        return nullptr;
    }

    /** Every parameter on the layer's page, in page order. */
    inline juce::StringArray pageIds(Target t)
    {
        juce::StringArray ids;
        const int b = std::clamp(t.bus, 0, ParamIDs::numBuses - 1);
        switch (t.kind)
        {
            case Kind::Swirl:
                for (int k = ParamIDs::SwirlType; k <= ParamIDs::DoublerFeedback; ++k)
                    ids.add(ParamIDs::busDetail[b][k]);
                break;
            case Kind::Filter:
                for (int k = ParamIDs::FilterType; k <= ParamIDs::FilterRate; ++k)
                    ids.add(ParamIDs::busDetail[b][k]);
                break;
            case Kind::Together:
                ids.addArray({ ParamIDs::togetherPeakAt, ParamIDs::togetherStagger, ParamIDs::togetherHitEvery });
                break;
            case Kind::Swell:
                ids.addArray({ ParamIDs::swellAuto, ParamIDs::swellEvery, ParamIDs::swellHold, ParamIDs::swellChance, ParamIDs::swellLoose });
                break;
        }
        return ids;
    }

    /** What the layer's Reset puts back to default and what fills its notch when off default: the page, but for
        Varispeed (a hand on the tape machine, not a setting: it springs back to the middle by itself). */
    inline juce::StringArray detailIds(Target t)
    {
        auto ids = pageIds(t);
        for (int b = 0; b < ParamIDs::numBuses; ++b)
            ids.removeString(ParamIDs::busDetail[b][ParamIDs::Varispeed]);
        return ids;
    }

    /** Anything behind the control is off its default (its notch is filled). Any thread that may read parameters. */
    inline bool isOffDefault(juce::AudioProcessorValueTreeState& state, Target t)
    {
        for (const auto& id : detailIds(t))
            if (auto* p = state.getParameter(id); p != nullptr && std::abs(p->getValue() - p->getDefaultValue()) > 1.0e-6f)
                return true;
        return false;
    }
}

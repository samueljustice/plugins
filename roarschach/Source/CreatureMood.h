#pragma once

#include "Params.h"

#include <array>

namespace roar
{
    /**
     * The colour mood of the clouds: a few mood axes read from the current settings (the modules
     * that are On and how far their key knobs are turned; their order does not matter), and the
     * colour the high-key blue-grey palette leans towards for them.
     *
     *   aggression  ROAR drive, GROWL depth, PITCH drive and a deep pitch, SUB drive,
     *               OUTPUT push and clip, CHAMBER edge                         -> ember red
     *   machine     VOCODER, RING (and its body), SHIMMER, Snap and Mono pitch,
     *               the Radio tone, the metal rooms                            -> cold electric blue
     *   eerie       SWIRL, ENSEMBLE (and its width), OVERTONE                  -> violet
     *   room        CHAMBER amount and resonance                               -> stone (see below)
     *
     * Each axis is 0 at the default settings (the reference blue-grey) and 1 at a strong setting of
     * its modules; fewer or lighter modules stay at the reference. Mix scales them all (a dry signal
     * is neutral).
     *
     * The colour (tone) is worked out in OkLCh at the reference tone's lightness:
     *  - aggression and machine sit across the wheel from each other, so they never average into a
     *    third colour: the stronger one keeps its own hue (aggression leads a tie) and the other
     *    only mutes it, warm towards a rust, cold towards a steel blue;
     *  - eerie blends with that by hue (with ember a crimson rose, with blue an indigo);
     *  - room is a modifier, not a hue of its own in a blend: it mutes the other moods and turns
     *    them a little towards stone (a warm mood to a warm stone, never on to khaki; a blue to
     *    teal); on its own, past a light setting, it leans the blue-grey to lichen;
     *  - the hue turns from the reference to the mood's along the arc that avoids the khaki and
     *    olive hues, mostly in the first part of the mood's strength, and the chroma never drops
     *    below the reference's (no grey on the way).
     *
     * Plain C++; the editor evaluates it on the message thread, the view springs the axes towards
     * it (MoodSpring), maps them to a tone, follows that at a bounded speed (ToneFollower) and turns
     * it into the shader's hue rotation (shaderMood).
     */
    struct CreatureMood
    {
        enum Axis { aggression, machine, eerie, room, numAxes };
        using Axes = std::array<float, numAxes>;

        /** The settings the mood reads (real values, as the parameters hold them). */
        struct Inputs
        {
            float pitchOn = 1, pitch = -2, pitchDrive = 1, pitchMode = 0;
            float cleanOn = 1, cleanTone = 1;
            float vocoderOn = 0, gravel = 41;
            float ringOn = 0, ring = 40, ringBody = 0;
            float shimmerOn = 1, shimmer = 18;
            float growlOn = 1, growl = 25, growlCharacter = 1, growlShape = 0;
            float swirlOn = 1, swirl = 21;
            float roarOn = 1, roar = 45;
            float ensembleOn = 1, ensemble = 10, ensembleWidth = 10;
            float subOn = 1, subDrive = 20;
            float overtoneOn = 1, overtone = 4, overtoneMode = 0;
            float outPush = 8, outClip = 0, mix = 100;
            float chamberOn = 0, chamber = 50, chamberResonance = 40, chamberDrive = 30, chamberRoom = 2;

            /** value (id) returns a parameter's real value (e.g. a lambda over the value tree). */
            template <typename Get>
            static Inputs read (Get&& value)
            {
                Inputs in;
                in.pitchOn = value (Params::pitchOn); in.pitch = value (Params::pitch); in.pitchDrive = value (Params::pitchDrive);
                in.pitchMode = value (Params::pitchMode);
                in.cleanOn = value (Params::cleanOn); in.cleanTone = value (Params::cleanTone);
                in.vocoderOn = value (Params::vocoderOn); in.gravel = value (Params::gravel);
                in.ringOn = value (Params::ringOn); in.ring = value (Params::ring); in.ringBody = value (Params::ringBody);
                in.shimmerOn = value (Params::shimmerOn); in.shimmer = value (Params::shimmer);
                in.growlOn = value (Params::growlOn); in.growl = value (Params::growl);
                in.growlCharacter = value (Params::growlCharacter); in.growlShape = value (Params::growlShape);
                in.swirlOn = value (Params::swirlOn); in.swirl = value (Params::swirl);
                in.roarOn = value (Params::roarOn); in.roar = value (Params::roar);
                in.ensembleOn = value (Params::ensembleOn); in.ensemble = value (Params::ensemble); in.ensembleWidth = value (Params::ensembleWidth);
                in.subOn = value (Params::subOn); in.subDrive = value (Params::subDrive);
                in.overtoneOn = value (Params::overtoneOn); in.overtone = value (Params::overtone); in.overtoneMode = value (Params::overtoneMode);
                in.outPush = value (Params::outPush); in.outClip = value (Params::outClip); in.mix = value (Params::mix);
                in.chamberOn = value (Params::chamberOn); in.chamber = value (Params::chamber);
                in.chamberResonance = value (Params::chamberResonance); in.chamberDrive = value (Params::chamberDrive);
                in.chamberRoom = value (Params::chamberRoom);
                return in;
            }

            /** The parameter defaults (Init). */
            static Inputs defaults() { return read ([] (const char* id) { return Params::defaultOf (id); }); }
        };

        /** Each axis before it is placed against the defaults (a soft sum of its contributions, 0..1). */
        static Axes raw (const Inputs& in);

        /** The mood axes, 0..1: 0 at the defaults (and below them), 1 at a strong setting. */
        static Axes evaluate (const Inputs& in);

        /** A colour as the mood shows it: OkLCh hue (degrees, 0..360) and chroma, at the reference
            tone's lightness (the palette keeps its values; only its hue and chroma move). */
        struct Tone
        {
            float hue = 0.0f, chroma = 0.0f;
        };

        /** The reference tone (#94b0c1, the palette's main tone): the tone of no mood. */
        static Tone reference();

        /** The tone this mood leans the palette to; axes outside 0..1 are clamped. */
        static Tone tone (const Axes& mood);

        /** A tone as sRGB 0..1 (at the reference's lightness). */
        static std::array<float, 3> colourOf (const Tone& tone);

        /** The reference tone as this mood turns it (sRGB 0..1). */
        static std::array<float, 3> referenceColour (const Axes& mood) { return colourOf (tone (mood)); }

        /** The shader's uMood.xyz for a tone: the hue rotation and chroma scale on the YIQ chroma
            plane that takes the reference tone to it, as (s cos a - 1, s sin a), and how warm it is
            (0..1, for the warm accents to stand apart from it); (0, 0, 0) is no change. */
        static std::array<float, 3> shaderMood (const Tone& tone);
        static std::array<float, 3> shaderMood (const Axes& mood) { return shaderMood (tone (mood)); }
    };

    /**
     * The displayed mood: a slow, slightly under-damped spring per axis towards the target, so the
     * colour drifts over several seconds and never jumps (a preset change, a dice roll or a turned
     * knob). Exact for any frame time (the closed-form solution over each step). Frozen holds the
     * colour (it eases to a stop where it is, or to a kept colour: the one saved with the session);
     * a reset while frozen takes the current settings' colour once and holds it there. GL thread only.
     */
    struct MoodSpring
    {
        static constexpr float c_omega = 0.5f;    // rad/s: settles (within 2 %) in about 10 s
        static constexpr float c_zeta = 0.8f;     // a gentle overshoot (about 1.5 %)

        CreatureMood::Axes value {}, velocity {};
        CreatureMood::Axes held {};               // the frozen target
        bool frozen = false;

        /** The first frame of a view: start settled on the target (or, frozen with a kept colour,
            on that). */
        void snap (const CreatureMood::Axes& target, bool freeze = false, const CreatureMood::Axes* kept = nullptr);
        /** reset: the creature was reset (once per reset). kept: while frozen, the colour to hold
            (the one saved with the session), if there is one. */
        void step (float dt, const CreatureMood::Axes& target, bool freeze, bool reset, const CreatureMood::Axes* kept = nullptr);
    };

    /**
     * The tone on screen: follows the mood's tone with its hue turning at most c_maxTurn degrees a
     * second (a long way round takes several seconds, never a quick flip), and while the hue still
     * has far to go the chroma stays near the reference's, so a turn across the wheel passes
     * through soft, muted tones rather than a saturated band of another mood's colour (or grey).
     * GL thread only.
     */
    struct ToneFollower
    {
        static constexpr float c_maxTurn = 30.0f;    // degrees per second
        static constexpr float c_hueLag = 0.7f;      // s: the hue's own easing (small turns)
        static constexpr float c_chromaLag = 0.9f;   // s

        CreatureMood::Tone value = CreatureMood::reference();

        void snap (const CreatureMood::Tone& target);
        void step (float dt, const CreatureMood::Tone& target);
    };
}

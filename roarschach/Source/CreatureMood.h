#pragma once

#include "Params.h"

#include <array>

namespace roar
{
    /**
     * The colour mood of the clouds: a few mood axes read from the current settings (the modules
     * that are On and how far their key knobs are turned; their order does not matter), and the hue
     * the high-key blue-grey palette leans towards for them.
     *
     *   aggression  ROAR drive, GROWL depth, PITCH drive and a deep pitch, SUB drive,
     *               OUTPUT push and clip, CHAMBER edge                         -> ember red
     *   machine     VOCODER, RING (and its body), SHIMMER, Snap and Mono pitch,
     *               the Radio tone, the metal rooms                            -> cold electric blue
     *   eerie       SWIRL, ENSEMBLE (and its width), OVERTONE                  -> violet
     *   room        CHAMBER amount and resonance                               -> green-grey (lichen)
     *
     * Each axis is 0 at the default settings (the reference blue-grey) and 1 at a strong setting of
     * its modules; fewer or lighter modules stay at the reference. Mix scales them all (a dry signal
     * is neutral). Blends are mixed on the OkLab hue plane (ember with blue leans magenta, ember with
     * lichen a warm stone, blue with lichen teal).
     *
     * Plain C++; the editor evaluates it on the message thread, the view springs towards it on the
     * GL thread (MoodSpring) and turns it into the shader's hue rotation (shaderMood).
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

        /** The reference tone (#94b0c1, the palette's main tone) as this mood turns it (sRGB 0..1);
            axes outside 0..1 are clamped. */
        static std::array<float, 3> referenceColour (const Axes& mood);

        /** The shader's uMood.xy for this mood: the hue rotation and chroma scale on the YIQ chroma
            plane that takes the reference tone to referenceColour (mood), as (s cos a - 1, s sin a);
            (0, 0) is no change. */
        static std::array<float, 2> shaderMood (const Axes& mood);
    };

    /**
     * The displayed mood: a slow, slightly under-damped spring per axis towards the target, so the
     * colour drifts over several seconds and never jumps (a preset change, a dice roll or a turned
     * knob). Exact for any frame time (the closed-form solution over each step). Frozen holds the
     * colour (it eases to a stop where it is); a reset while frozen takes the current settings'
     * colour once and holds it there. GL thread only.
     */
    struct MoodSpring
    {
        static constexpr float c_omega = 0.5f;    // rad/s: settles (within 2 %) in about 10 s
        static constexpr float c_zeta = 0.8f;     // a gentle overshoot (about 1.5 %)

        CreatureMood::Axes value {}, velocity {};
        CreatureMood::Axes held {};               // the frozen target
        bool frozen = false;

        /** The first frame of a view: start settled on the target. */
        void snap (const CreatureMood::Axes& target);
        /** reset: the creature was reset (once per reset). */
        void step (float dt, const CreatureMood::Axes& target, bool freeze, bool reset);
    };
}

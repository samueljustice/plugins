#include "CreatureMood.h"

#include "dsp/KnobLaws.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        float clamp01 (float v) { return std::isfinite (v) ? std::min (1.0f, std::max (0.0f, v)) : 0.0f; }
        float on (float v) { return v >= 0.5f ? 1.0f : 0.0f; }
        float pct (float v) { return clamp01 (v / 100.0f); }
        float ramp (float lo, float hi, float x) { return clamp01 ((x - lo) / (hi - lo)); }
        float law (double (*f) (double), float u) { return static_cast<float> (f (static_cast<double> (u))); }

        /** A soft sum: contributions add up but the axis never passes 1 (1 - the product of what each leaves). */
        struct SoftSum
        {
            float keep = 1.0f;
            void add (float x) { keep *= 1.0f - clamp01 (x); }
            float value() const { return 1.0f - keep; }
        };

        // Where each axis counts as fully in its mood (its raw value; the defaults are its zero).
        constexpr float c_full[CreatureMood::numAxes] = { 0.90f, 0.72f, 0.72f, 0.62f };

        // The moods' tones on the OkLab hue plane (hue in degrees, chroma), each at the reference
        // tone's lightness: ember red, cold electric blue, violet, lichen green-grey.
        constexpr float c_hue[CreatureMood::numAxes] = { 22.0f, 240.0f, 310.0f, 148.0f };
        constexpr float c_chroma[CreatureMood::numAxes] = { 0.068f, 0.062f, 0.076f, 0.047f };

        constexpr float c_ref[3] = { 0.580f, 0.690f, 0.757f };   // #94b0c1, the palette's main tone

        // sRGB <-> OkLab
        float toLinear (float c) { return c <= 0.04045f ? c / 12.92f : std::pow ((c + 0.055f) / 1.055f, 2.4f); }
        float toGamma (float c) { c = std::max (0.0f, c); return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow (c, 1.0f / 2.4f) - 0.055f; }

        std::array<float, 3> okLab (const float rgb[3])
        {
            const float r = toLinear (rgb[0]), g = toLinear (rgb[1]), b = toLinear (rgb[2]);
            const float l = std::cbrt (0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
            const float m = std::cbrt (0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
            const float s = std::cbrt (0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
            return { 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
                     1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
                     0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s };
        }

        std::array<float, 3> fromOkLab (float L, float a, float b)
        {
            const float l = std::pow (L + 0.3963377774f * a + 0.2158037573f * b, 3.0f);
            const float m = std::pow (L - 0.1055613458f * a - 0.0638541728f * b, 3.0f);
            const float s = std::pow (L - 0.0894841775f * a - 1.2914855480f * b, 3.0f);
            return { clamp01 (toGamma (4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s)),
                     clamp01 (toGamma (-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s)),
                     clamp01 (toGamma (-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s)) };
        }
    }

    CreatureMood::Axes CreatureMood::raw (const Inputs& in)
    {
        namespace view = roar::laws::view;
        SoftSum aggr, mach, eerie, room;

        // aggression: the drives, the growl, a deep pitch, the output pushed and clipped
        const float pitchOn = on (in.pitchOn);
        aggr.add (0.75f * on (in.roarOn) * law (view::roar, pct (in.roar)));
        const float grit = in.growlCharacter >= 1.5f ? 1.0f : (in.growlCharacter >= 0.5f ? 0.8f : 0.6f);
        const float rattle = in.growlShape >= 3.5f ? 1.15f : 1.0f;
        aggr.add (0.60f * on (in.growlOn) * law (view::growl, pct (in.growl)) * grit * rattle);
        aggr.add (0.70f * pitchOn * pct (in.pitchDrive));
        aggr.add (0.45f * pitchOn * ramp (0.0f, 12.0f, -in.pitch));
        aggr.add (0.60f * on (in.subOn) * ramp (20.0f, 90.0f, in.subDrive));
        aggr.add (0.40f * ramp (6.0f, 20.0f, in.outPush));
        aggr.add (0.70f * ramp (0.0f, 9.0f, in.outClip));
        aggr.add (0.30f * on (in.chamberOn) * pct (in.chamberDrive));

        // machine: the vocoder, the ring modulator and its box, the frequency shifter, a stepped or
        // robotic pitch, the radio tone, the metal rooms
        mach.add (0.85f * on (in.vocoderOn) * (0.35f + 0.65f * law (view::gravel, pct (in.gravel))));
        mach.add (0.85f * on (in.ringOn) * (0.30f + 0.70f * pct (in.ring)) * (in.ringBody >= 0.5f ? 1.15f : 1.0f));
        mach.add (0.60f * on (in.shimmerOn) * law (view::shimmer, pct (in.shimmer)));
        mach.add (0.35f * pitchOn * (in.pitchMode >= 0.5f ? 1.0f : 0.0f));
        mach.add (0.25f * on (in.cleanOn) * (in.cleanTone >= 2.5f ? 1.0f : 0.0f));
        const int roomType = static_cast<int> (std::lround (in.chamberRoom));
        const bool metal = roomType == 0 || roomType == 3;   // Steel Box, Tin Booth
        mach.add (0.30f * on (in.chamberOn) * (metal ? pct (in.chamber) : 0.0f));

        // eerie: the phaser, the chorus (wider is stranger), the overtones (most when they replace)
        eerie.add (0.80f * on (in.swirlOn) * law (view::swirl, pct (in.swirl)));
        eerie.add (0.80f * on (in.ensembleOn) * law (view::ensemble, pct (in.ensemble)) * (0.6f + 0.4f * clamp01 (in.ensembleWidth / 150.0f)));
        // (the overtone knob's own travel: its audible law is nearly all in the first fifth)
        eerie.add (0.70f * on (in.overtoneOn) * pct (in.overtone) * (in.overtoneMode >= 1.5f ? 1.3f : 1.0f));

        // room: how much of the voice is in the room, and how long it rings
        room.add (on (in.chamberOn) * (0.65f * pct (in.chamber) + 0.35f * pct (in.chamberResonance)));

        return { aggr.value(), mach.value(), eerie.value(), room.value() };
    }

    CreatureMood::Axes CreatureMood::evaluate (const Inputs& in)
    {
        static const Axes base = raw (Inputs::defaults());
        const Axes r = raw (in);
        // (a dry signal is the voice as it is: the mood follows the wet share)
        const float wet = std::sqrt (pct (in.mix));
        Axes m {};
        for (int i = 0; i < numAxes; ++i)
            m[(size_t) i] = clamp01 ((r[(size_t) i] - base[(size_t) i]) / (c_full[i] - base[(size_t) i])) * wet;
        return m;
    }

    std::array<float, 3> CreatureMood::referenceColour (const Axes& mood)
    {
        const auto ref = okLab (c_ref);
        float sumW = 0.0f, sumC = 0.0f, vx = 0.0f, vy = 0.0f, keep = 1.0f;
        for (int i = 0; i < numAxes; ++i)
        {
            const float m = clamp01 (mood[(size_t) i]);
            const float w = m * m;        // (the strongest mood leads the blend's hue)
            const float h = c_hue[i] * 0.017453292f;
            sumW += w;
            sumC += w * c_chroma[i];
            vx += w * c_chroma[i] * std::cos (h);
            vy += w * c_chroma[i] * std::sin (h);
            keep *= 1.0f - m;
        }
        if (sumW < 1.0e-5f)
            return { c_ref[0], c_ref[1], c_ref[2] };
        // the blend's hue: the weighted direction on the hue plane; its chroma: the weighted mean
        // (so two moods across the wheel from each other meet at the hue between them, not in grey;
        // the weights are squared, so the strongest mood leads)
        const float len = std::sqrt (vx * vx + vy * vy);
        const float c = sumC / sumW;
        const float ma = len > 1.0e-6f ? c * vx / len : 0.0f, mb = len > 1.0e-6f ? c * vy / len : 0.0f;
        // how far from the reference: a soft sum of the axes
        const float t = 1.0f - keep;
        return fromOkLab (ref[0], ref[1] + t * (ma - ref[1]), ref[2] + t * (mb - ref[2]));
    }

    std::array<float, 2> CreatureMood::shaderMood (const Axes& mood)
    {
        const auto iq = [] (const float* c) -> std::array<float, 2>
        {
            return { 0.596f * c[0] - 0.274f * c[1] - 0.322f * c[2], 0.211f * c[0] - 0.523f * c[1] + 0.312f * c[2] };
        };
        const auto target = referenceColour (mood);
        const auto z0 = iq (c_ref), z1 = iq (target.data());
        const float d = z0[0] * z0[0] + z0[1] * z0[1];
        // k = z1 / z0 (complex): the rotation and scale on the chroma plane
        float kr = (z1[0] * z0[0] + z1[1] * z0[1]) / d, ki = (z1[1] * z0[0] - z1[0] * z0[1]) / d;
        const float s = std::sqrt (kr * kr + ki * ki);
        if (s > 3.0f)
        {
            kr *= 3.0f / s;
            ki *= 3.0f / s;
        }
        return { kr - 1.0f, ki };
    }

    //==============================================================================================
    void MoodSpring::snap (const CreatureMood::Axes& target)
    {
        value = target;
        velocity = {};
        held = target;
    }

    void MoodSpring::step (float dt, const CreatureMood::Axes& target, bool freeze, bool reset)
    {
        if (freeze && ! frozen)
            held = value;                  // ease to a stop where it is
        if (freeze && reset)
            held = target;                 // a new creature takes the settings' colour, then holds it
        frozen = freeze;
        if (! std::isfinite (dt) || dt <= 0.0f)
            return;

        const float w = c_omega, z = c_zeta, wd = w * std::sqrt (1.0f - z * z);
        const float decay = std::exp (-z * w * dt), c = std::cos (wd * dt), s = std::sin (wd * dt);
        for (size_t i = 0; i < value.size(); ++i)
        {
            float goal = frozen ? held[i] : target[i];
            if (! std::isfinite (goal))
                goal = value[i];
            const float e0 = value[i] - goal, v0 = velocity[i];
            const float e = decay * (e0 * c + (v0 + z * w * e0) / wd * s);
            const float v = decay * (v0 * c - (z * w * v0 + w * w * e0) / wd * s);
            value[i] = std::isfinite (e) ? goal + e : goal;
            velocity[i] = std::isfinite (v) ? v : 0.0f;
        }
    }
}

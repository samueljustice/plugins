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

        // The moods' tones in OkLCh (hue in degrees, chroma), each at the reference tone's lightness:
        // ember red, cold electric blue (a clearer, deeper blue than the reference's grey-blue),
        // violet; and lichen green-grey, the room's lean on its own.
        constexpr float c_hue[CreatureMood::numAxes] = { 28.0f, 262.0f, 318.0f, 160.0f };
        constexpr float c_chroma[CreatureMood::numAxes] = { 0.085f, 0.082f, 0.080f, 0.042f };

        // Aggression against machine: aggression's weight counts this much more (a tie leans warm);
        // the weaker one mutes the stronger by up to this share (at a tie), and turns a warm lead
        // this far towards rust.
        constexpr float c_warmLead = 2.0f;
        constexpr float c_pairMute = 0.35f;
        constexpr float c_rustTurn = 15.0f;

        // Room: no lean below c_roomLo (light rooms keep the reference), full by c_roomHi. It turns
        // the moods towards the lichen by a share of the way, at most c_roomTurn degrees, a warm mood
        // no further than c_stoneHue (a warm stone), and mutes them by up to c_roomMute.
        constexpr float c_roomLo = 0.40f, c_roomHi = 0.95f;
        constexpr float c_roomShare = 0.55f, c_roomTurn = 70.0f, c_stoneHue = 60.0f, c_roomMute = 0.45f;

        // The hue turns mostly early: 1 - (1 - t)^c_hueLead of the way at strength t.
        constexpr float c_hueLead = 3.0f;

        // The hues a turn never passes through (khaki and olive: muddy at these values).
        constexpr float c_mudLo = 75.0f, c_mudHi = 135.0f;

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

        float wrap180 (float d) { d = std::fmod (d + 180.0f, 360.0f); return (d < 0.0f ? d + 360.0f : d) - 180.0f; }
        float wrap360 (float h) { h = std::fmod (h, 360.0f); return h < 0.0f ? h + 360.0f : h; }
        float smooth (float lo, float hi, float x) { const float t = clamp01 ((x - lo) / (hi - lo)); return t * t * (3.0f - 2.0f * t); }

        // does the turn from h0 by d degrees pass through the muddy hues?
        bool crossesMud (float h0, float d)
        {
            if (std::abs (d) < 1.0e-4f)
                return false;
            const float start = wrap360 (h0);
            if (start >= c_mudLo && start <= c_mudHi)
                return false;   // (it starts there: nothing to avoid)
            const float ahead = d > 0.0f ? wrap360 (c_mudLo - start) : wrap360 (start - c_mudHi);
            return ahead < std::abs (d);
        }

        const CreatureMood::Tone& referenceTone()
        {
            static const CreatureMood::Tone t = []
            {
                const auto lab = okLab (c_ref);
                return CreatureMood::Tone { wrap360 (std::atan2 (lab[2], lab[1]) * 57.29578f), std::sqrt (lab[1] * lab[1] + lab[2] * lab[2]) };
            }();
            return t;
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

    CreatureMood::Tone CreatureMood::reference() { return referenceTone(); }

    CreatureMood::Tone CreatureMood::tone (const Axes& mood)
    {
        const Tone ref = referenceTone();
        const float a = clamp01 (mood[aggression]), mc = clamp01 (mood[machine]), e = clamp01 (mood[eerie]);
        const float roomLean = smooth (c_roomLo, c_roomHi, clamp01 (mood[room]));
        const float strength = 1.0f - (1.0f - a) * (1.0f - mc) * (1.0f - e);

        // The base the moods turn from: the reference, leaning to lichen with the room as far as
        // the other moods leave it room to.
        const float lean = roomLean * (1.0f - strength) * (1.0f - strength);
        const Tone base { wrap360 (ref.hue + wrap180 (c_hue[room] - ref.hue) * lean), ref.chroma + (c_chroma[room] - ref.chroma) * lean };
        if (strength < 1.0e-5f)
            return base;

        // The moods' own tone. Aggression against machine: the stronger keeps its hue, the other mutes it.
        float vx = 0.0f, vy = 0.0f, sumW = 0.0f, sumC = 0.0f;
        const auto add = [&] (float hue, float chroma, float w)
        {
            vx += w * chroma * std::cos (hue * 0.017453292f);
            vy += w * chroma * std::sin (hue * 0.017453292f);
            sumW += w;
            sumC += w * chroma;
        };
        const float wa = a * a, wm = mc * mc, we = e * e;
        if (wa + wm > 1.0e-9f)
        {
            const float warmShare = c_warmLead * wa / (c_warmLead * wa + wm);
            const float other = warmShare >= 0.5f ? 2.0f * (1.0f - warmShare) : 2.0f * warmShare;   // 0 alone .. 1 at a tie
            const float mute = 1.0f - c_pairMute * other * std::sqrt (other);
            if (warmShare >= 0.5f)
                add (c_hue[aggression] + c_rustTurn * other, c_chroma[aggression] * mute, wa + wm);
            else
                add (c_hue[machine], c_chroma[machine] * mute, wa + wm);
        }
        if (we > 1.0e-9f)
            add (c_hue[eerie], c_chroma[eerie], we);
        // (the pair and eerie are never across the wheel from each other, so their direction is sound)
        float hue = wrap360 (std::atan2 (vy, vx) * 57.29578f);
        float chroma = sumC / std::max (sumW, 1.0e-9f);

        // Room: towards stone, muted.
        if (roomLean > 0.0f)
        {
            const float toLichen = wrap180 (c_hue[room] - hue);
            const float turned = hue + std::copysign (std::min (std::abs (toLichen) * c_roomShare, c_roomTurn), toLichen) * roomLean;
            const bool warm = std::abs (wrap180 (hue - c_hue[aggression])) < 60.0f;
            hue = wrap360 (warm && wrap180 (turned - c_stoneHue) > 0.0f && wrap180 (hue - c_stoneHue) <= 0.0f ? c_stoneHue : turned);
            chroma *= 1.0f - c_roomMute * roomLean;
        }

        // From the base to the moods' tone: the hue along the arc that avoids the muddy hues, mostly
        // early; the chroma by strength, never below the reference's.
        float d = wrap180 (hue - base.hue);
        if (crossesMud (base.hue, d) && ! crossesMud (base.hue, d - std::copysign (360.0f, d)))
            d -= std::copysign (360.0f, d);
        const float turn = 1.0f - std::pow (1.0f - strength, c_hueLead);
        return { wrap360 (base.hue + d * turn), std::max (base.chroma + (chroma - base.chroma) * strength, ref.chroma) };
    }

    std::array<float, 3> CreatureMood::colourOf (const Tone& t)
    {
        const auto ref = okLab (c_ref);
        const float c = std::isfinite (t.chroma) ? std::max (0.0f, std::min (t.chroma, 0.2f)) : 0.0f;
        const float h = (std::isfinite (t.hue) ? t.hue : 0.0f) * 0.017453292f;
        return fromOkLab (ref[0], c * std::cos (h), c * std::sin (h));
    }

    std::array<float, 3> CreatureMood::shaderMood (const Tone& tone)
    {
        const auto iq = [] (const float* c) -> std::array<float, 2>
        {
            return { 0.596f * c[0] - 0.274f * c[1] - 0.322f * c[2], 0.211f * c[0] - 0.523f * c[1] + 0.312f * c[2] };
        };
        const auto target = colourOf (tone);
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
        // how warm: the chroma towards ember (squared cosine, so violet and stone-green count little)
        const float along = std::max (0.0f, std::cos ((tone.hue - 35.0f) * 0.017453292f));
        const float warmth = std::isfinite (tone.chroma) ? clamp01 (tone.chroma * along * along / 0.06f) : 0.0f;
        const bool neutral = std::abs (wrap180 (tone.hue - referenceTone().hue)) < 1.0e-3f && std::abs (tone.chroma - referenceTone().chroma) < 1.0e-6f;
        if (neutral)
            return { 0.0f, 0.0f, 0.0f };
        return { kr - 1.0f, ki, warmth };
    }

    //==============================================================================================
    void MoodSpring::snap (const CreatureMood::Axes& target, bool freeze, const CreatureMood::Axes* kept)
    {
        value = freeze && kept != nullptr ? *kept : target;
        for (auto& v : value)
            v = clamp01 (v);
        velocity = {};
        held = value;
        frozen = freeze;
    }

    void MoodSpring::step (float dt, const CreatureMood::Axes& target, bool freeze, bool reset, const CreatureMood::Axes* kept)
    {
        if (freeze && ! frozen)
            held = value;                  // ease to a stop where it is
        if (freeze && reset)
            held = target;                 // a new creature takes the settings' colour, then holds it
        else if (freeze && kept != nullptr)
            for (size_t i = 0; i < held.size(); ++i)
                held[i] = std::isfinite ((*kept)[i]) ? clamp01 ((*kept)[i]) : held[i];   // the colour saved with the session
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

    //==============================================================================================
    void ToneFollower::snap (const CreatureMood::Tone& target)
    {
        if (std::isfinite (target.hue) && std::isfinite (target.chroma))
            value = { wrap360 (target.hue), std::max (0.0f, target.chroma) };
    }

    void ToneFollower::step (float dt, const CreatureMood::Tone& target)
    {
        if (! std::isfinite (dt) || dt <= 0.0f || ! std::isfinite (target.hue) || ! std::isfinite (target.chroma))
            return;
        dt = std::min (dt, 0.5f);
        // the hue: eased, and never faster than c_maxTurn
        const float err = wrap180 (target.hue - value.hue);
        const float cap = c_maxTurn * dt;
        value.hue = wrap360 (value.hue + std::max (-cap, std::min (cap, err * (1.0f - std::exp (-dt / c_hueLag)))));
        // the chroma: towards the target's once the hue is close (within 20 degrees), towards a
        // muted tone (the reference's chroma, or the target's if lower) while it is far (70 and more)
        const float left = std::abs (wrap180 (target.hue - value.hue));
        const float near = clamp01 (1.0f - (left - 20.0f) / 50.0f);
        const float muted = std::min (target.chroma, referenceTone().chroma);
        const float goal = muted + (std::max (0.0f, target.chroma) - muted) * near;
        value.chroma += (goal - value.chroma) * (1.0f - std::exp (-dt / c_chromaLag));
    }
}

#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

// The knob laws: how a knob position u (0..1 for the % knobs) becomes the value a module works
// with. One home for every law, so the modules, the creature view, the state migration and the
// tests all agree. Plain C++ (no JUCE).
//
//   roar::laws       the laws the modules use
//   roar::laws::v1   the laws of the first release (state format 2), kept only so states saved by it
//                    can be converted to the nearest sound (migrateFromV1) and for the tests
namespace roar::laws
{
    inline double clamp01 (double u) { return std::isfinite (u) ? std::clamp (u, 0.0, 1.0) : 0.0; }

    // ---------------------------------------------------------------------------------- IN
    /** Auto Level's target offset for the Input knob (dB): Input moves the levelled voice by half its value. */
    inline double inAutoTargetOffsetDb (double trimDb) { return 0.5 * trimDb; }
    inline constexpr double inAutoMinGainDb = -12.0, inAutoMaxGainDb = 30.0;

    // ---------------------------------------------------------------------------------- PITCH
    /** The drive amount d (the drive stage's own scale: gain 24 d^1.5 dB) for Drive u: the gain in dB
        is linear in the knob (24 u dB). */
    inline double pitchDriveAmount (double u) { return std::pow (clamp01 (u), 2.0 / 3.0); }

    // ---------------------------------------------------------------------------------- CLEAN
    /** The leveller's containment curve value for Contain u. */
    inline double containCurve (double u) { return std::pow (clamp01 (u), 0.7); }
    /** Gate expander ratio and depth (dB, <= 0) for Gate u. */
    inline double gateRatio (double u) { return 1.5 + 3.5 * clamp01 (u); }
    inline double gateDepthDb (double u) { return -30.0 * clamp01 (u); }
    /** How far above the learnt room floor the gate threshold sits (dB): higher settings close on more of the gaps. */
    inline double gateThresholdOffsetDb (double u) { return 6.0 + 12.0 * clamp01 (u); }

    // ---------------------------------------------------------------------------------- DE-ESS
    /** The top part of De-ess's travel (0 up to 40 %, 1 at 100 %), where the threshold and the range grow further. */
    inline double deessTop (double u)
    {
        const double x = std::max (0.0, clamp01 (u) - 0.4) / 0.6;
        return x * std::sqrt (x);
    }
    /** The most gain reduction (dB, <= 0): -15 u, and 9 dB more over the top part (-24 dB at 100 %). */
    inline double deessRangeDb (double u) { return -15.0 * clamp01 (u) - 9.0 * deessTop (u); }
    /** How far the detector threshold moves down (dB): none up to 40 %, 12 dB at 100 %, so the top of
        the travel keeps taking more instead of stopping at the detector's slope. */
    inline double deessThresholdShiftDb (double u) { return 12.0 * deessTop (u); }

    // ---------------------------------------------------------------------------------- DETAIL
    /** Depth (dB) of the Low and High sections. */
    inline double detailDepthDb (double u) { return 34.0 * std::pow (clamp01 (u), 1.2); }

    // ---------------------------------------------------------------------------------- VOCODER
    /** The vocoder's character g for Gravel u: 0.15 at the bottom of the travel, faded in over the first 1 %. */
    inline double gravelAmount (double u)
    {
        u = clamp01 (u);
        return u < 0.01 ? (15.0 + 0.85) * u : 0.15 + 0.85 * u;
    }

    // ---------------------------------------------------------------------------------- RING
    /** AM mode: the amount a whose modulation index is a / 2. */
    inline double ringAmAmount (double u) { return std::sqrt (clamp01 (u)); }

    // ---------------------------------------------------------------------------------- SHIMMER
    inline double shimmerAmount (double u) { return 0.65 * clamp01 (u); }
    inline constexpr double shimmerMaxRateHz = 25.0;

    // ---------------------------------------------------------------------------------- GROWL
    /** Tremolo depth D (0..0.874): 18 dB of trough at full Depth, even steps in dB of trough. */
    inline double growlDepth (double u) { return 1.0 - std::pow (10.0, -18.0 * clamp01 (u) / 20.0); }
    inline constexpr double growlMinRateHz = 2.0;

    // ---------------------------------------------------------------------------------- SWIRL
    inline double swirlMix (double u) { return 1.0 - std::pow (1.0 - clamp01 (u), 1.5); }

    // ---------------------------------------------------------------------------------- ROAR
    /** The mud scoop at 200 Hz (dB, <= 0): -14 a up to 40 %, a smooth turn to -7 dB at 60 %, flat above. */
    inline double roarScoopDb (double a)
    {
        a = clamp01 (a);
        if (a <= 0.4)
            return -14.0 * a;
        if (a < 0.6)
        {
            const double x = a - 0.4;
            return -(5.6 + 14.0 * x - 35.0 * x * x);
        }
        return -7.0;
    }
    /** The first stage's drive (dB above its calibration). */
    inline double roarDrive1Db (double a)
    {
        a = clamp01 (a);
        const double over = std::max (0.0, a - 0.5);
        return 18.0 * a + 48.0 * over * over;
    }
    /** How strongly Tone tilts the tone stack: the same at every Drive (it used to follow Drive, so Tone did little below 50 %). */
    inline double roarToneStrength (double) { return 1.5; }

    // ---------------------------------------------------------------------------------- ENSEMBLE
    inline double ensembleAmount (double u) { return 0.7 * std::pow (clamp01 (u), 0.6); }
    /** Vibrato's wet share for an ensemble amount m (from ensembleAmount). */
    inline double ensembleVibratoAmount (double m) { return std::sqrt (clamp01 (m)); }
    /** Side gain for Width W (0..1.5). */
    inline double ensembleSideGain (double w)
    {
        constexpr double top = 0.85914057;   // 1 - exp (-(1.05 / 0.75)^2)
        w = std::clamp (std::isfinite (w) ? w : 0.0, 0.0, 1.5);
        return top * std::pow (w / 1.5, 1.3);
    }

    // ---------------------------------------------------------------------------------- TAME
    /** The curve position t for Tame u (0..2.2): front-loaded, so the first steps act on a quiet top band too. */
    inline double tameT (double u) { return 2.2 * std::pow (clamp01 (u), 0.7); }

    // ---------------------------------------------------------------------------------- SUB
    /** Sub Level law (dB re the divided harmonic), above the fade-in at the bottom of the travel. */
    inline double subLevelDb (double u) { return -14.0 + 24.0 * std::sqrt (clamp01 (u)); }
    inline constexpr double subDriveNeutral = 20.0;   // % : the highest Drive whose products stay inaudible

    // ---------------------------------------------------------------------------------- OVERTONE
    inline double overtoneLevelDb (double u) { return -10.0 + 20.0 * std::pow (clamp01 (u), 0.6); }
    /** Add mode's level (linear gain) g = k sqrt (10^u - 1): 1.1 to 1.8 dB more per 10 % above 30 %,
        so the layer's share of the bands it lands in grows evenly; 0 at u = 0 (no fade needed),
        +14.8 dB at 100 %. Lean and Replace use overtoneLevelDb. */
    inline constexpr double overtoneAddScale = 1.831803;   // +14.8 dB at 100 %
    inline constexpr double overtoneAddExponent = 1.0;
    inline double overtoneAddGain (double u) { return overtoneAddScale * std::sqrt (std::pow (10.0, overtoneAddExponent * clamp01 (u)) - 1.0); }
    /** How much of Lean / Replace's low cut applies at Amount u (the cut grows with the overtones). */
    inline double overtoneLowCut (double u) { return std::min (1.0, clamp01 (u) / 0.25); }

    // ---------------------------------------------------------------------------------- OUTPUT
    /** The clipper's knee half-width for Clip Shape s (0 Soft .. 1 Hard) and Clip's cap 1 - 10^(-Clip / 20). */
    inline double clipKnee (double s, double cap) { return std::min (0.5, cap) * (1.0 - clamp01 (s)); }
    /** The pre-limiter gain (dB) for Push and Ceiling: Push is relative to the ceiling (Ceiling -1 dB is the reference). */
    inline double pushGainDb (double pushDb, double ceilingDb) { return pushDb + ceilingDb + 1.0; }
    /** Auto Release's fast stage (ms) for Release R (ms): 3 ms at 1 ms, 15 ms at 60 ms (log), R / 4 above, at most 250 ms. */
    inline double autoFastReleaseMs (double releaseMs)
    {
        const double r = std::clamp (std::isfinite (releaseMs) ? releaseMs : 60.0, 1.0, 1000.0);
        if (r <= 60.0)
            return 3.0 * std::pow (5.0, std::log (r) / std::log (60.0));
        return std::min (0.25 * r, 250.0);
    }
    /** The Mix crossfade (equal power): the dry's and the wet's gains for Mix m. */
    inline double mixDryGain (double m) { return std::cos (clamp01 (m) * 1.5707963267948966); }
    inline double mixWetGain (double m) { return std::sin (clamp01 (m) * 1.5707963267948966); }

    // ---------------------------------------------------------------------------------- CHAMBER (1.1)
    /** Chamber u: the wet's balance against the dry (dB), -24 + 30 u: even in dB over the travel (the
        ring heard in the pauses grows about 1.2 dB per 5 %), equal at 80 %, the dry 6 dB down at 100 %. */
    inline double chamberBalanceDb (double u) { return -24.0 + 30.0 * clamp01 (u); }
    /** Below 2 % the wet fades out (0 at 0 %, so 0 % is the dry alone). */
    inline double chamberWetFade (double u)
    {
        const double x = std::min (1.0, clamp01 (u) / 0.02);
        return x * x * (3.0 - 2.0 * x);
    }
    /** Resonance u: the late part's level change (dB): -9 dB at 0 %, the room as captured at 40 %, +9 dB at 100 %. */
    inline double chamberLateLevelDb (double u)
    {
        u = clamp01 (u);
        return u < 0.4 ? 22.5 * (u - 0.4) : 15.0 * (u - 0.4);
    }
    /** Resonance u: the decay's scale (x the room's own): 0.55 at 0 %, 1 at 40 %, the room's longest
        (maxScale: what its IR's length allows, at most 1.6) at 100 %. */
    inline double chamberDecayScale (double u, double maxScale)
    {
        u = clamp01 (u);
        return u < 0.4 ? 0.55 + 1.125 * u : 1.0 + (std::max (1.0, maxScale) - 1.0) * (u - 0.4) / 0.6;
    }
    /** Edge u: the speaker's drive (dB). Linear in dB: at IN's working level the distortion residual grows
        about 2 dB per 10 %. */
    inline double chamberEdgeDriveDb (double u) { return 24.0 * clamp01 (u); }
    /** Width w: the gain on the room's side signal. w^0.75 spreads the L/R correlation evenly over the
        travel (a linear gain left the bottom 20 % nearly mono). */
    inline double chamberSideGain (double w) { return std::pow (clamp01 (w), 0.75); }

    // ================================================================================== v1
    namespace v1
    {
        inline double containCurve (double u) { return clamp01 (u); }
        inline double gateRatio (double) { return 2.5; }
        inline double gateDepthDb (double u) { return -40.0 * clamp01 (u); }
        inline double deessRangeDb (double u, bool roarOn, double roarDrive)
        {
            return -24.0 * clamp01 (u) * (roarOn ? 1.0 + 0.5 * clamp01 (roarDrive) : 1.0);
        }
        inline double detailDepthDb (double u) { return 48.0 * std::pow (clamp01 (u), 1.68); }
        inline double gravelAmount (double u) { return clamp01 (u); }
        inline double ringAmAmount (double u) { return clamp01 (u); }
        inline double shimmerAmount (double u) { return clamp01 (u); }
        inline double growlDepth (double u) { return 0.85 * clamp01 (u); }
        inline double swirlMix (double u) { return clamp01 (u); }
        inline double roarScoopDb (double a) { return -std::min (7.0, 14.0 * clamp01 (a)); }
        inline double roarDrive1Db (double a) { return 18.0 * clamp01 (a); }
        inline double roarToneStrength (double a) { return clamp01 (a); }
        inline double ensembleAmount (double u) { return clamp01 (u); }
        inline double ensembleVibratoAmount (double m) { return std::max (clamp01 (m), 0.7 * std::min (1.0, clamp01 (m) / 0.1)); }
        inline double ensembleSideGain (double w)
        {
            const double x = std::clamp (std::isfinite (w) ? w : 0.0, 0.0, 1.5) / 0.75;
            return 1.0 - std::exp (-x * x);
        }
        inline double tameT (double u) { return clamp01 (u); }
        inline double subLevelDb (double u) { return -14.0 + 26.0 * std::pow (clamp01 (u), 0.55); }
        inline double overtoneLevelDb (double u) { return -20.0 + 26.0 * std::pow (clamp01 (u), 0.6); }
        inline double clipKnee (double s, double cap) { return std::min (0.5 * (1.0 - clamp01 (s)), cap); }
        inline double pushGainDb (double pushDb, double) { return pushDb; }
        inline double autoFastReleaseMs (double releaseMs) { return std::clamp (0.25 * releaseMs, 3.0, 250.0); }
        inline double mixDryGain (double m) { return 1.0 - clamp01 (m); }
        inline double mixWetGain (double m) { return clamp01 (m); }
    }

    // ================================================================================== creature view
    // The creature view was tuned on the first release's knob scales: these give, for a knob
    // position u, the position that had the same effect there (0..1), so it reacts to the sound.
    namespace view
    {
        inline double growl (double u) { return clamp01 (growlDepth (u) / 0.85); }
        inline double shimmer (double u) { return clamp01 (shimmerAmount (u)); }
        inline double swirl (double u) { return swirlMix (u); }
        inline double ensemble (double u) { return clamp01 (ensembleAmount (u)); }
        inline double gravel (double u) { return clamp01 (gravelAmount (u)); }
        inline double detail (double u) { return clamp01 (std::pow (detailDepthDb (u) / 48.0, 1.0 / 1.68)); }
        inline double roar (double u) { return clamp01 (roarDrive1Db (u) / 18.0); }
        inline double sub (double u)
        {
            return u > 0.0 ? clamp01 (std::pow (std::max (0.0, subLevelDb (u) + 14.0) / 26.0, 1.0 / 0.55)) : 0.0;
        }
        /** (Add, the default mode.) */
        inline double overtone (double u)
        {
            const double g = overtoneAddGain (u);
            return g > 0.0 ? clamp01 (std::pow (std::max (0.0, 20.0 * std::log10 (g) + 20.0) / 26.0, 1.0 / 0.6)) : 0.0;
        }
    }

    // ================================================================================== conversion
    // Per-parameter converters: a value stored by the first release (real units: % as 0..100) to
    // the value that gives the same sound under the laws above.
    namespace convert
    {
        inline double pct (double u) { return std::clamp (100.0 * u, 0.0, 100.0); }

        inline double contain (double old) { return pct (std::pow (clamp01 (old / 100.0), 1.0 / 0.7)); }
        /** The room-tone cut matches (the previous depth beyond about 15 dB was never reached). */
        inline double gate (double old) { return pct (std::min (40.0 * clamp01 (old / 100.0), 15.0) / 30.0); }
        /**
         * The same high-band reduction. A range r (dB, <= 0) on the previous detector (fixed threshold)
         * is the setting -r / 15 up to 40 % (-6 dB), where the laws agree. Beyond that the previous
         * reduction stopped growing at the detector's slope (the previous -15 dB gave about what the new
         * 56 % gives); this part is the mean of measurements on three voice recordings (the 5 to
         * 12.6 kHz bands, within 1 dB on each).
         */
        inline double deessForOldRange (double rangeDb)
        {
            const double c = std::min (-rangeDb / 15.0, 1.0) * 100.0;
            if (c <= 40.0)
                return pct (std::max (0.0, c) / 100.0);
            static constexpr double table[] = { 40.0, 43.3, 45.8, 47.9, 49.6, 51.0, 52.2, 53.1, 53.9, 54.5, 55.0, 55.3, 55.6 };   // c = 40, 45, .. 100
            const double x = (c - 40.0) / 5.0;
            const int i = std::min (static_cast<int> (x), 11);
            return table[i] + (x - i) * (table[i + 1] - table[i]);
        }
        inline double deess (double old, bool roarOn, double roarDrivePct)
        {
            return deessForOldRange (v1::deessRangeDb (old / 100.0, roarOn, roarDrivePct / 100.0));
        }
        inline double detail (double old)
        {
            const double db = v1::detailDepthDb (old / 100.0);
            return db <= 0.0 ? 0.0 : pct (std::pow (db / 34.0, 1.0 / 1.2));
        }
        inline double gravel (double old)
        {
            const double g = clamp01 (old / 100.0);
            return g < gravelAmount (0.01) ? pct (g / 15.85) : pct ((g - 0.15) / 0.85);
        }
        inline double ringAm (double old) { return pct (clamp01 (old / 100.0) * clamp01 (old / 100.0)); }
        inline double shimmer (double old) { return pct (clamp01 (old / 100.0) / 0.65); }
        inline double growl (double old)
        {
            const double d = v1::growlDepth (old / 100.0);
            return pct (-20.0 * std::log10 (1.0 - d) / 18.0);
        }
        inline double swirl (double old) { return pct (1.0 - std::pow (1.0 - clamp01 (old / 100.0), 2.0 / 3.0)); }
        inline double roarDrive (double old)
        {
            const double a = clamp01 (old / 100.0);
            if (a <= 0.5)
                return 100.0 * a;
            const double target = v1::roarDrive1Db (a);
            double lo = 0.5, hi = 1.0;
            for (int i = 0; i < 60; ++i)
            {
                const double mid = 0.5 * (lo + hi);
                (roarDrive1Db (mid) < target ? lo : hi) = mid;
            }
            return 100.0 * lo;
        }
        /** Same tone-stack tilt: t a_old = t' s (a_new). */
        inline double roarTone (double oldTonePct, double oldDrivePct, double newDrivePct)
        {
            const double t = oldTonePct * v1::roarToneStrength (oldDrivePct / 100.0) / roarToneStrength (newDrivePct / 100.0);
            return std::clamp (t, -100.0, 100.0);
        }
        /** The knob position whose ensembleAmount is m (up to its top, 0.7). */
        inline double ensembleForAmount (double m) { return pct (std::pow (std::min (clamp01 (m), 0.7) / 0.7, 1.0 / 0.6)); }
        inline double ensemble (double old, bool vibrato)
        {
            const double m = clamp01 (old / 100.0);
            if (vibrato)
            {
                // Every old amount from 10 % sounded the same (at least 70 % wet): the amount whose
                // new curve gives the previous wet share.
                const double wet = v1::ensembleVibratoAmount (m);
                return ensembleForAmount (wet * wet);
            }
            return ensembleForAmount (m);
        }
        inline double width (double old)
        {
            const double g = v1::ensembleSideGain (old / 100.0);
            return std::clamp (150.0 * std::pow (g / ensembleSideGain (1.5), 1.0 / 1.3), 0.0, 150.0);
        }
        inline double tame (double old) { return pct (std::pow (v1::tameT (old / 100.0) / tameT (1.0), 1.0 / 0.7)); }
        /** The same drive amount d (the first release's knob was d itself). */
        inline double pitchDrive (double old) { const double d = clamp01 (old / 100.0); return pct (d * std::sqrt (d)); }
        inline double sub (double old)
        {
            const double u = clamp01 (old / 100.0);
            if (u <= 0.0)
                return 0.0;
            const double x = (26.0 / 24.0) * std::pow (u, 0.55);
            return pct (x * x);
        }
        /** The first release's linear gain for Overtone old (%), including its fade-in below 2 %. */
        inline double overtoneV1Gain (double old)
        {
            constexpr double fadeBelow = 0.02;
            const double u = clamp01 (old / 100.0);
            if (u <= 0.0)
                return 0.0;
            return u < fadeBelow ? std::pow (10.0, v1::overtoneLevelDb (fadeBelow) / 20.0) * u / fadeBelow
                                 : std::pow (10.0, v1::overtoneLevelDb (u) / 20.0);
        }
        /** The Overtone knob (%) whose level in mode `add` (else Lean / Replace) is the linear gain g;
            Lean / Replace solve their law including its fade-in below 2 %. */
        inline double overtoneForGain (double g, bool add)
        {
            if (! (g > 0.0))
                return 0.0;
            if (add)
            {
                const double r = g / overtoneAddScale;
                return pct (std::log10 (1.0 + r * r) / overtoneAddExponent);
            }
            constexpr double fadeBelow = 0.02;
            const double fadeTopGain = std::pow (10.0, overtoneLevelDb (fadeBelow) / 20.0);
            if (g < fadeTopGain)
                return pct (fadeBelow * g / fadeTopGain);
            const double x = (20.0 * std::log10 (g) + 10.0) / 20.0;
            return pct (std::pow (std::max (0.0, x), 1.0 / 0.6));
        }
        /** Same level: solves the new law (of the stored mode) for the previous level. */
        inline double overtone (double old, bool add) { return overtoneForGain (overtoneV1Gain (old), add); }
        inline double push (double oldPushDb, double ceilingDb) { return std::clamp (oldPushDb - (ceilingDb + 1.0), 0.0, 24.0); }
        /** The same knee half-width. */
        inline double clipShape (double oldShapePct, double clipDb)
        {
            if (! (clipDb > 0.0))
                return std::clamp (oldShapePct, 0.0, 100.0);
            const double cap = 1.0 - std::pow (10.0, -clipDb / 20.0);
            const double w = v1::clipKnee (oldShapePct / 100.0, cap);
            return pct (1.0 - w / std::min (0.5, cap));
        }
        /** Input with Auto Level on: inside +-12 dB it had no effect; beyond, it moved the level 1:1. */
        inline double inTrimWithAuto (double oldDb)
        {
            if (std::abs (oldDb) <= 12.0)
                return 0.0;
            return std::clamp (2.0 * (oldDb - std::copysign (12.0, oldDb)), -24.0, 24.0);
        }
    }

    /**
     * Converts the parameter values of a state or preset saved by the first release (by ID, real
     * units as stored) to the settings nearest its sound under these laws. Values that are missing
     * are left missing (they take today's defaults). Values the first release did not use (Sub
     * Drive, which was inaudible) take their neutral value.
     *
     * Not carried over, because one knob now sets several things or a module itself changed:
     *   Gate     matched on depth only; its ratio (1.5 + 3.5u, was 2.5) and threshold offset
     *            (6 + 12u dB above the room floor, was 10 dB) follow the converted knob, so a 1.0
     *            Gate closes on slightly more or less of the gaps (25 % converts exactly).
     *   Swirl    the mix is kept, but the resonance no longer drops at small mix nor stops at
     *            1.3x at large mix, so low and high Swirl settings ring a little differently.
     *   Detail   the quiet-floor tracker and the boost cap near the floor act on every setting,
     *            so Detail Low lifts less room tone than in 1.0.
     * The release notes and docs/parameter-audit.md say so.
     */
    inline void migrateFromV1 (std::map<std::string, double>& v)
    {
        const auto has = [&v] (const char* id) { return v.find (id) != v.end(); };
        const auto get = [&v] (const char* id, double fallback)
        {
            const auto it = v.find (id);
            return it != v.end() && std::isfinite (it->second) ? it->second : fallback;
        };
        const auto apply = [&v] (const char* id, auto&& fn)
        {
            const auto it = v.find (id);
            if (it != v.end() && std::isfinite (it->second))
                it->second = fn (it->second);
        };

        // Read every value a converter depends on before anything is changed (first-release defaults).
        const bool roarOn = get ("roarOn", 1.0) >= 0.5;
        const double roarOld = get ("roar", 45.0);
        const double roarNew = convert::roarDrive (roarOld);
        const double ceiling = get ("outCeiling", -1.0);
        const double clip = get ("outClip", 0.0);
        const bool ringAm = static_cast<int> (std::lround (get ("ringMode", 0.0))) == 1;
        const bool vibrato = static_cast<int> (std::lround (get ("ensembleMotion", 1.0))) == 3;
        const bool inAuto = get ("inAuto", 1.0) >= 0.5;
        const bool overtoneAdd = static_cast<int> (std::lround (get ("overtoneMode", 0.0))) == 0;

        if (inAuto)
            apply ("inTrim", [] (double x) { return convert::inTrimWithAuto (x); });
        apply ("cleanContain", [] (double x) { return convert::contain (x); });
        apply ("cleanGate", [] (double x) { return convert::gate (x); });
        apply ("deess", [&] (double x) { return convert::deess (x, roarOn, roarOld); });
        apply ("detailLow", [] (double x) { return convert::detail (x); });
        apply ("detailHigh", [] (double x) { return convert::detail (x); });
        apply ("gravel", [] (double x) { return convert::gravel (x); });
        if (ringAm)
            apply ("ring", [] (double x) { return convert::ringAm (x); });
        apply ("shimmer", [] (double x) { return convert::shimmer (x); });
        apply ("shimmerRate", [] (double x) { return std::min (x, shimmerMaxRateHz); });
        apply ("growl", [] (double x) { return convert::growl (x); });
        apply ("growlRate", [] (double x) { return std::max (x, growlMinRateHz); });
        apply ("swirl", [] (double x) { return convert::swirl (x); });
        if (has ("roar"))
            apply ("roarTone", [&] (double x) { return convert::roarTone (x, roarOld, roarNew); });
        apply ("roar", [&] (double) { return roarNew; });
        apply ("ensemble", [&] (double x) { return convert::ensemble (x, vibrato); });
        apply ("ensembleWidth", [] (double x) { return convert::width (x); });
        apply ("tame", [] (double x) { return convert::tame (x); });
        apply ("sub", [] (double x) { return convert::sub (x); });
        apply ("subDrive", [] (double) { return subDriveNeutral; });
        apply ("overtone", [&] (double x) { return convert::overtone (x, overtoneAdd); });
        apply ("pitchDrive", [] (double x) { return convert::pitchDrive (x); });
        apply ("outPush", [&] (double x) { return convert::push (x, ceiling); });
        apply ("outClipShape", [&] (double x) { return convert::clipShape (x, clip); });
        apply ("outAttack", [] (double x) { return std::max (x, 0.2); });
    }
}

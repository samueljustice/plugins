#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

#include <cmath>

/**
 * Roarschach's look: ink on paper. The controls are drawn in the picture's own materials: a pale
 * paper toned by the cloud's colour mood, near-black ink for text and marks, and one dyed ink in the
 * mood's hue for value arcs, lamps and meters. A module is known by its number, name and place in the
 * chain, never by a colour of its own.
 *
 * Everything is defined here as roles (colours, type, radii and strokes); drawing code asks for a
 * role, never for a literal colour or a typeface.
 *
 * Colour: every role has a fixed lightness (OkLab L) and only its hue and chroma follow the mood, so
 * contrast holds in every mood (the tests measure it at the extremes). The editor feeds the cloud's
 * shown tone in with applyMood() a few times a second while it drifts; the palette only changes
 * (and the window only repaints) when the quantised tone does.
 *
 * Two role sets: the paper set (the look) and a dark set kept for a possible dark variant, chosen
 * only in code (setVariant), never in the UI.
 */
namespace Theme
{
    /** The line under the name in the editor header and the About window. */
    inline constexpr const char* tagline = "SAMMY J  /  MODULAR VOICE PROCESSOR";

    //==============================================================================
    /** OkLab / OkLCh, for a palette whose lightness holds while its hue moves. */
    namespace Ok
    {
        inline float encode(float c)
        {
            c = juce::jlimit(0.0f, 1.0f, c);
            return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }

        inline float decode(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

        /** Linear sRGB of an OkLab colour; false when it is outside the sRGB gamut. */
        inline bool linearOf(float L, float a, float b, float out[3])
        {
            float l = L + 0.3963377774f * a + 0.2158037573f * b;
            float m = L - 0.1055613458f * a - 0.0638541728f * b;
            float s = L - 0.0894841775f * a - 1.2914855480f * b;
            l = l * l * l; m = m * m * m; s = s * s * s;
            out[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
            out[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
            out[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
            constexpr float lo = -1.0e-4f, hi = 1.0001f;
            return out[0] >= lo && out[0] <= hi && out[1] >= lo && out[1] <= hi && out[2] >= lo && out[2] <= hi;
        }

        /** An OkLCh colour (hue in degrees), its chroma reduced until it is inside sRGB (the
            lightness is kept, so a role's contrast never depends on how vivid the mood is). */
        inline juce::Colour lch(float L, float C, float hueDeg)
        {
            const float h = juce::degreesToRadians(std::isfinite(hueDeg) ? hueDeg : 0.0f);
            C = std::isfinite(C) ? std::max(0.0f, C) : 0.0f;
            float rgb[3] {};
            for (int i = 0; i < 60; ++i)
            {
                if (linearOf(L, C * std::cos(h), C * std::sin(h), rgb) || C < 1.0e-4f)
                    break;
                C *= 0.93f;
            }
            return juce::Colour::fromFloatRGBA(encode(rgb[0]), encode(rgb[1]), encode(rgb[2]), 1.0f);
        }
    }

    /** WCAG relative luminance of an opaque colour, and the contrast ratio of two. */
    inline float luminance(juce::Colour c)
    {
        return 0.2126f * Ok::decode(c.getFloatRed()) + 0.7152f * Ok::decode(c.getFloatGreen()) + 0.0722f * Ok::decode(c.getFloatBlue());
    }

    inline float contrast(juce::Colour a, juce::Colour b)
    {
        const float la = luminance(a), lb = luminance(b);
        return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
    }

    //==============================================================================
    /** Every colour the UI draws with, by what it is for. */
    struct Roles
    {
        // Surfaces
        juce::Colour paper;        ///< the sheet: window, header, around the picture
        juce::Colour wash;         ///< a tile: a faint wash on the paper (no edge)
        juce::Colour sheet;        ///< raised paper: menus, tooltips, dialogs, fields, chips
        juce::Colour groove;       ///< a pressed groove: meter tracks, knob tracks

        // Ink, by rank (text and marks). Every rank is text-strength (4.5:1 or more) on every surface.
        juce::Colour ink;          ///< names, values, knob discs, the mark
        juce::Colour inkDim;       ///< captions, secondary text
        juce::Colour inkFaint;     ///< off and unused states, slot numbers, the least text
        juce::Colour rule;         ///< hairlines that must be seen (outlines of fields and menus)
        juce::Colour ruleSoft;     ///< quiet hairlines (separators)

        // The mood's dye
        juce::Colour accent;       ///< value arcs, lit lamps, selection marks (3:1 or more on a tile)
        juce::Colour accentInk;    ///< the dye as text and as meter fill (4.5:1 or more on paper)
        juce::Colour meterFill;    ///< a meter's level, solid (3:1 or more against its groove)
        juce::Colour selection;    ///< selected text, highlighted menu rows (a wash, not text)

        // State
        juce::Colour warn;         ///< clipping and warnings, always with a shape and a word too
        juce::Colour warnTag;      ///< the fill of a warning tag: the warning ink, or plain ink in a mood whose dye is near it
        juce::Colour focus;        ///< keyboard focus rings (the plain ink: never the same dye as the arc it rings)

        // Over the picture
        juce::Colour readout;      ///< the soft label behind a readout
        juce::Colour readoutText;  ///< its value
        juce::Colour readoutCaption; ///< its caption

        juce::Colour shadow;       ///< drop shadows (dragged tile, dialogs)
    };

    enum class Variant { paper, dark };

    /** The reference tone of no mood (the cloud's #94b0c1 in OkLCh). */
    inline constexpr float c_referenceHue = 234.7f, c_referenceChroma = 0.0394f;

    /** The role set for a variant and a mood tone (OkLCh hue in degrees, chroma ~0.03..0.12). */
    inline Roles makeRoles(Variant variant, float hue, float chroma)
    {
        hue = std::isfinite(hue) ? std::fmod(std::fmod(hue, 360.0f) + 360.0f, 360.0f) : c_referenceHue;
        chroma = std::isfinite(chroma) ? juce::jlimit(0.0f, 0.12f, chroma) : c_referenceChroma;
        using Ok::lch;
        Roles r;

        if (variant == Variant::dark)
        {
            // Dark water: ink surfaces faintly in the cloud's tone, pale text, the accent the cloud
            // brightened. (Lightness per role: surfaces .16 / .195 / .235, text .945 / .80 / .68.)
            const float gc = 0.004f + chroma * 0.07f;
            const float ac = juce::jlimit(0.07f, 0.13f, 0.03f + chroma * 1.2f);
            r.paper = lch(0.160f, gc, hue);
            r.wash = lch(0.195f, gc * 1.15f, hue);
            r.sheet = lch(0.235f, gc * 1.25f, hue);
            r.groove = lch(0.300f, gc * 1.1f, hue);
            r.ink = lch(0.945f, 0.006f, hue);
            r.inkDim = lch(0.800f, 0.012f, hue);
            r.inkFaint = lch(0.700f, 0.014f, hue);
            r.rule = r.ink.withAlpha(0.30f);
            r.ruleSoft = r.ink.withAlpha(0.14f);
            r.accent = lch(0.800f, ac, hue);
            r.accentInk = lch(0.820f, ac * 0.8f, hue);
            r.meterFill = r.accent;
            r.selection = r.accent.withAlpha(0.22f);
            r.warn = lch(0.720f, 0.140f, 30.0f);
            r.warnTag = r.warn;
            r.focus = r.ink;
            r.readout = lch(0.080f, gc * 0.5f, hue).withAlpha(0.86f);
            r.readoutText = r.ink;
            r.readoutCaption = r.inkDim;
            r.shadow = juce::Colours::black.withAlpha(0.55f);
            return r;
        }

        // Paper: the cloud's own tone, greyed to a fifth of the mood's chroma (never above 0.017; a
        // little more in warm moods), so the mood washes through without colouring the sheet. Warm moods
        // lean the paper a little towards bone (an ember cloud gives a warm bone with a blush: the same
        // pigment as the cloud, never a plaster pink nor a tan), and the dye keeps the mood's own hue.
        // `warm` weighs the lean: 1 at an ember cloud, 0 from 70 degrees away.
        const float warm = std::max(0.0f, 1.0f - std::abs(std::remainder(hue - 28.0f, 360.0f)) / 70.0f);
        const float paperHue = hue + 21.0f * warm;   // an ember cloud's paper sits at hue ~50: bone, blushed
        const float pc = std::min(0.017f + 0.006f * warm, (0.2f + 0.12f * warm) * chroma);
        const float ac = juce::jlimit(0.060f, 0.130f, chroma * 1.45f);
        const float ic = juce::jlimit(0.050f, 0.110f, chroma * 1.20f);

        // A pale paper (lighter than the cloud's shade, darker than its lights), so the picture's
        // whites stay the brightest thing in the window.
        r.paper = lch(0.800f, pc, paperHue);
        r.wash = lch(0.818f, pc * (0.8f + 0.1f * warm), paperHue + 4.0f * warm);   // barely lighter than the paper: a wash, not a card
        r.sheet = lch(0.905f, pc * 0.5f, paperHue + 8.0f * warm);
        r.groove = lch(0.725f, pc * 0.9f, paperHue);

        // Ink carries a trace of the mood (barely: chroma .008 to .012).
        r.ink = lch(0.205f, 0.008f, hue);
        r.inkDim = lch(0.315f, 0.010f, hue);
        r.inkFaint = lch(0.380f, 0.012f, hue);
        r.rule = r.ink.withAlpha(0.34f);
        r.ruleSoft = r.ink.withAlpha(0.15f);

        r.accent = lch(0.480f, ac, hue);
        r.accentInk = lch(0.370f, ic, hue);
        r.meterFill = r.accentInk;
        r.selection = r.accent.withAlpha(0.20f);

        r.warn = lch(0.420f, 0.160f, 27.0f);   // a deep vermilion
        // Where the mood's dye is a red itself (within 40 degrees of the warning's), a warning tag is
        // solid ink, so the warning still stands apart from the dye: its word, its end block and its value
        // keep the warning ink.
        r.warnTag = std::abs(std::remainder(hue - 27.0f, 360.0f)) < 40.0f ? r.ink : r.warn;
        // Focus is shape and plain ink, never the dye: a knob's ring would sit against its value arc.
        r.focus = r.ink;

        // The readouts over the creature: a soft label of raised paper, ink letters.
        r.readout = r.sheet.withAlpha(0.80f);
        r.readoutText = r.ink;
        r.readoutCaption = r.inkDim;

        r.shadow = r.ink.withAlpha(0.30f);
        return r;
    }

    //==============================================================================
    /** The live palette (message thread): the roles of the current variant and mood. */
    inline Roles liveRoles = makeRoles(Variant::paper, c_referenceHue, c_referenceChroma);

    namespace Colours
    {
        // Short names for drawing code (references into the live roles).
        inline const juce::Colour& paper = liveRoles.paper;
        inline const juce::Colour& wash = liveRoles.wash;
        inline const juce::Colour& sheet = liveRoles.sheet;
        inline const juce::Colour& groove = liveRoles.groove;
        inline const juce::Colour& ink = liveRoles.ink;
        inline const juce::Colour& inkDim = liveRoles.inkDim;
        inline const juce::Colour& inkFaint = liveRoles.inkFaint;
        inline const juce::Colour& rule = liveRoles.rule;
        inline const juce::Colour& ruleSoft = liveRoles.ruleSoft;
        inline const juce::Colour& accent = liveRoles.accent;
        inline const juce::Colour& accentInk = liveRoles.accentInk;
        inline const juce::Colour& meterFill = liveRoles.meterFill;
        inline const juce::Colour& selection = liveRoles.selection;
        inline const juce::Colour& warn = liveRoles.warn;
        inline const juce::Colour& warnTag = liveRoles.warnTag;
        inline const juce::Colour& focus = liveRoles.focus;
        inline const juce::Colour& readout = liveRoles.readout;
        inline const juce::Colour& readoutText = liveRoles.readoutText;
        inline const juce::Colour& readoutCaption = liveRoles.readoutCaption;
        inline const juce::Colour& shadow = liveRoles.shadow;
    }

    namespace detail
    {
        struct MoodState
        {
            Variant variant = Variant::paper;
            float hue = c_referenceHue, chroma = c_referenceChroma;
            int serial = 0;
        };

        inline MoodState& moodState()
        {
            static MoodState state;
            return state;
        }
    }

    /** Bumped whenever the palette changes (so cached art knows to redraw). */
    inline int paletteSerial() { return detail::moodState().serial; }
    inline Variant variant() { return detail::moodState().variant; }

    /** The quantisation applyMood() uses: a step finer than the eye sees on paper this pale. */
    inline constexpr float c_hueStep = 2.0f, c_chromaStep = 0.004f;

    /** Leans the palette to the mood's tone (OkLCh hue in degrees and chroma, as CreatureMood::Tone
        holds it). Returns true when the palette changed. Message thread only. */
    inline bool applyMood(float hueDeg, float chroma)
    {
        if (!std::isfinite(hueDeg) || !std::isfinite(chroma))
            return false;
        hueDeg = std::fmod(std::fmod(std::round(hueDeg / c_hueStep) * c_hueStep, 360.0f) + 360.0f, 360.0f);
        chroma = std::round(juce::jlimit(0.0f, 0.12f, chroma) / c_chromaStep) * c_chromaStep;

        auto& s = detail::moodState();
        if (std::abs(hueDeg - s.hue) < 0.01f && std::abs(chroma - s.chroma) < 1.0e-5f)
            return false;
        s.hue = hueDeg;
        s.chroma = chroma;
        liveRoles = makeRoles(s.variant, hueDeg, chroma);
        ++s.serial;
        return true;
    }

    /** The role set in use (code only: the UI offers no choice). */
    inline void setVariant(Variant v)
    {
        auto& s = detail::moodState();
        if (s.variant == v)
            return;
        s.variant = v;
        liveRoles = makeRoles(v, s.hue, s.chroma);
        ++s.serial;
    }

    /** The creature view's GL clear colour: the picture's own ground, part of its look (not a role). */
    inline constexpr juce::uint32 c_glClear = 0xff090b0f;

    /** The mark in colour, kept for the About window only (the editor's mark is one ink). */
    namespace Brand
    {
        inline constexpr juce::uint32 markTop = 0xffff7a45;    // ember, at the brow
        inline constexpr juce::uint32 markChin = 0xffef6f8e;   // rose, at the chin
        inline constexpr juce::uint32 markFangs = 0xffd9485e;  // the fangs, biting back in a deeper rose
    }

    //==============================================================================
    /** Corner radii and stroke widths (design px). */
    namespace Metrics
    {
        inline constexpr float radius = 4.0f;        ///< chips, fields, menus, tooltips, dialogs
        inline constexpr float radiusSoft = 9.0f;    ///< the readouts over the picture (half their height)
        inline constexpr float hairline = 1.0f;      ///< rules and outlines
        inline constexpr float focusRing = 1.5f;     ///< keyboard focus
        inline constexpr float pointer = 2.0f;       ///< a knob's pointer
        inline constexpr float meter = 5.0f;         ///< a meter's thickness in the footers
        inline constexpr float meterTick = 3.0f;     ///< the ticks ruled under a meter's groove
        inline constexpr float lamp = 14.0f;         ///< an On lamp's drawn size

        /** The smallest clickable area of a small control (design px): 16 px or more at 65 %. Small
            glyphs (an On lamp, a die, the move chip, a mode chip) are drawn smaller than this and click
            in a margin around what is drawn. */
        inline constexpr int hitTarget = 26;
        inline constexpr int chipHeight = 18;        ///< a mode chip as drawn (its button is hitTarget high)

        /** How long a clip mark stays up after the last clipped sample (ms). */
        inline constexpr double clipLatchMs = 1500.0;
    }

    /** The picture's own tones for its still cloud (when the visualiser is Off or OpenGL is not
        available): part of the visualiser's look, like c_glClear, not roles. These are the tones of no
        mood; the still cloud takes the mood's hue and chroma at their two lightnesses (stillCloud()). */
    namespace Picture
    {
        inline constexpr juce::uint32 cloudLit = 0xffd9e3eb;
        inline constexpr juce::uint32 cloudTone = 0xff94b0c1;

        /** OkLCh of the two (lightness, and the lit tone's chroma as a share of the shade's). */
        inline constexpr float litL = 0.9106f, toneL = 0.7418f, litChromaShare = 0.39f;
    }

    /** The mood tone the palette was last made from (OkLCh hue in degrees, chroma). */
    inline float moodHue() { return detail::moodState().hue; }
    inline float moodChroma() { return detail::moodState().chroma; }

    /** The still cloud's lit and shaded tones in a mood: the reference cloud's lightnesses, the mood's
        hue and chroma (at the reference tone they are Picture::cloudLit and cloudTone). */
    inline std::pair<juce::Colour, juce::Colour> stillCloud(float hue, float chroma)
    {
        hue = std::isfinite(hue) ? hue : c_referenceHue;
        chroma = std::isfinite(chroma) ? juce::jlimit(0.0f, 0.12f, chroma) : c_referenceChroma;
        return { Ok::lch(Picture::litL, chroma * Picture::litChromaShare, hue), Ok::lch(Picture::toneL, chroma, hue) };
    }

    //==============================================================================
    /** A paper grain: soft blotches and fibres of ink at a few per cent, tiled (made once, 192 px).
        Drawn over paper with an image fill, never a blur. */
    inline const juce::Image& paperGrain()
    {
        static const juce::Image grain = []
        {
            constexpr int n = 192, cell = 24, m = n / cell;
            juce::Image img(juce::Image::ARGB, n, n, true);
            juce::Random rng(0x5eed);
            float coarse[m][m];
            for (auto& row : coarse)
                for (auto& v : row)
                    v = rng.nextFloat();
            const auto smooth = [](float t) { return t * t * (3.0f - 2.0f * t); };
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                {
                    const int cx = x / cell, cy = y / cell;
                    const float fx = smooth(static_cast<float>(x % cell) / cell), fy = smooth(static_cast<float>(y % cell) / cell);
                    const float a = coarse[cy][cx], b = coarse[cy][(cx + 1) % m];
                    const float c = coarse[(cy + 1) % m][cx], d = coarse[(cy + 1) % m][(cx + 1) % m];
                    const float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
                    const float blotch = top + (bottom - top) * fy;
                    const float fibre = rng.nextFloat();
                    img.setPixelAt(x, y, juce::Colours::black.withAlpha(0.028f * blotch + 0.024f * fibre * fibre));
                }
            return img;
        }();
        return grain;
    }

    /** The paper's colour under one pixel of the grain (the grain is ink at this alpha), for paper that is
        drawn pixel by pixel and must match a grained sheet. */
    inline juce::Colour grained(juce::Colour paper, float grainAlpha)
    {
        return paper.overlaidWith(juce::Colours::black.withAlpha(juce::jlimit(0.0f, 1.0f, grainAlpha)));
    }

    /** origin: where the grain's tiling starts in g's coordinates (a child drawing paper that must match
        its parent's grain passes minus its position in the parent). */
    inline void fillPaperGrain(juce::Graphics& g, juce::Rectangle<float> area, float cornerRadius = 0.0f, juce::Point<int> origin = {})
    {
        if (variant() == Variant::dark)
            return;
        g.setTiledImageFill(paperGrain(), origin.x, origin.y, 1.0f);
        if (cornerRadius > 0.0f)
            g.fillRoundedRectangle(area, cornerRadius);
        else
            g.fillRect(area);
    }

    //==============================================================================
    /** Type sizes by role (point heights, design px; the window opens at 80-100 % of design size and
        can go down to 65 %). The capitals of the sans are about 0.65 of its point height and of the
        display face 0.70, so every text a reader needs (captions, values, mode chips, buttons, module
        names) keeps capitals of 5.5 px or more at 65 % (the tests measure the glyphs). */
    namespace Type
    {
        inline constexpr float wordmark = 27.0f;   ///< ROARSCHACH in the header
        inline constexpr float presetName = 17.0f; ///< the preset's name in the header
        inline constexpr float heading = 17.0f;    ///< dialog and About titles
        inline constexpr float tileTitle = 14.5f;  ///< a module's name on its tile (all at one size: the largest every name fits)
        inline constexpr float tileTitleMin = 12.25f; ///< (the display face's capitals are 0.70 of it: 5.5 px at 65 %)
        inline constexpr float menuHeader = 13.5f; ///< a section of a menu
        inline constexpr float body = 13.5f;       ///< prose: tooltips, dialogs, the hint over the picture
        inline constexpr float prose = 12.5f;      ///< dense prose: the About window's quick start and licence (a window of its own, never scaled down)
        inline constexpr float menu = 14.0f;       ///< menu items
        inline constexpr float button = 13.0f;     ///< button labels (capitals)
        inline constexpr float chip = 13.0f;       ///< a mode chip's choices (capitals, untracked: they are long)
        inline constexpr float value = 13.5f;      ///< values under knobs and in footers, and slot numbers (figures: a little over 5.5 px at 65 %)
        inline constexpr float caption = 13.0f;    ///< captions, footer labels, slot numbers, badges (the floor)
        inline constexpr float credit = 11.5f;     ///< the header's credit line (has room to grow, never needed to read)
    }

    /**
     * Typefaces loaded once and shared by every editor instance (SharedResourcePointer), embedded:
     *
     *  - Marcellus (SIL OFL 1.1): the display face, flared roman capitals. Wordmark, tile titles,
     *    headings, the preset name.
     *  - Source Sans 3 Regular and Semibold (SIL OFL 1.1): every word, caption and value. Its
     *    default figures are tabular (the tests check it), so values do not shift while a knob turns.
     */
    struct Fonts
    {
        Fonts()
            : regular  (juce::Typeface::createSystemTypefaceFor(BinaryData::SourceSans3Regular_ttf,  BinaryData::SourceSans3Regular_ttfSize))
            , semibold (juce::Typeface::createSystemTypefaceFor(BinaryData::SourceSans3Semibold_ttf, BinaryData::SourceSans3Semibold_ttfSize))
            , displayFace (juce::Typeface::createSystemTypefaceFor(BinaryData::MarcellusRegular_ttf, BinaryData::MarcellusRegular_ttfSize))
        {
        }

        juce::Font make(const juce::Typeface::Ptr& face, float pointHeight, float tracking) const
        {
            pointHeight = std::max(1.0f, pointHeight);
            if (face == nullptr)
                return juce::Font(juce::FontOptions().withPointHeight(pointHeight));
            return juce::Font(juce::FontOptions(face).withPointHeight(pointHeight)).withExtraKerningFactor(tracking);
        }

        /** The wordmark: Marcellus, lightly spaced. */
        juce::Font display(float pt) const { return make(displayFace, pt, 0.06f); }
        /** Names and headings (module names in capitals, dialog titles, menu sections): Marcellus. */
        juce::Font title(float pt) const   { return make(displayFace, pt, 0.04f); }
        /** Short labels in capitals (buttons, chips): Source Sans Semibold, tracked. */
        juce::Font label(float pt) const   { return make(semibold, pt, 0.035f); }
        /** Captions and footer labels: Source Sans Semibold, tracked a little more (they are small). */
        juce::Font caption(float pt) const { return make(semibold, pt, 0.045f); }
        /** Values: Source Sans Semibold, set a hair tight (its figures are tabular). */
        juce::Font value(float pt) const   { return make(semibold, pt, -0.01f); }
        /** Prose (tooltips, menus, dialogs, About): Source Sans Regular. */
        juce::Font body(float pt) const    { return make(regular, pt, 0.0f); }

        /** True for a font made by one of the roles above (so a label that set one keeps it). */
        bool isThemeFont(const juce::Font& f) const
        {
            const auto face = f.getTypefacePtr();
            return face != nullptr && (face == regular || face == semibold || face == displayFace);
        }

        juce::Typeface::Ptr regular, semibold, displayFace;
    };
}

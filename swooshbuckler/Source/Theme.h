#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

/**
 * Sweet Justice house style, Caribbean edition: deep-sea teal panels, sand
 * text, one mono family (Azeret Mono, embedded) and a harmonious set of sea
 * and harbour colours, each tied to a meaning.
 *
 *   lagoon turquoise = sources      sunset gold = flight (envelope, doppler)
 *   coral            = movement FX  sky blue    = filter / sweep
 *   sea glass        = space        driftwood   = master
 */
namespace Theme
{
    namespace Colours
    {
        inline const juce::Colour background  { 0xff0a1a22 }; // deep water
        inline const juce::Colour panel       { 0xff0f2630 };
        inline const juce::Colour panelRaised { 0xff15323e };
        inline const juce::Colour outline     { 0xff22495a };
        inline const juce::Colour outlineSoft { 0xff183a48 };

        inline const juce::Colour text        { 0xfff2e8d2 }; // sand
        inline const juce::Colour textDim     { 0xff9db9bf };
        inline const juce::Colour textFaint   { 0xff5d7f88 };

        inline const juce::Colour input       { 0xff3ccbc2 }; // lagoon turquoise (sources; LogoOutline)
        inline const juce::Colour harmonics   { 0xffff8466 }; // coral (movement; LogoOutline)
        inline const juce::Colour filter      { 0xff86b4ff }; // sky blue
        inline const juce::Colour space       { 0xff8fd9b0 }; // sea glass
        inline const juce::Colour go          { 0xff4fd68a }; // switched on (morph ON)
        inline const juce::Colour master      { 0xffd3c9b4 }; // driftwood
        inline const juce::Colour flight      { 0xfff2b84b }; // sunset gold

        inline const juce::Colour sources     = input;
        inline const juce::Colour movement    = harmonics;
        inline const juce::Colour accent      = flight;

        inline const juce::Colour record      { 0xffff4f5e }; // cherry
        inline const juce::Colour good        { 0xff8fd9b0 };

        inline const juce::Colour scope       { 0xff06131a };
        inline const juce::Colour grid        { 0xff173a46 };

        inline const juce::Colour hover       { 0xff1b3d4a };
        inline const juce::Colour selection   { 0x333ccbc2 };
        inline const juce::Colour shadow      { 0x80000000 };

        inline const juce::Colour brass       { 0xffdcae5c };
        inline const juce::Colour brassDark   { 0xff7d5a26 };
    }

    /** Text the user reads names Mac shortcuts ("Cmd+Z"); on Windows and Linux that key is Ctrl. */
    inline juce::String localiseShortcuts(const juce::String& text)
    {
       #if JUCE_MAC
        return text;
       #else
        return text.replace("Shift+Cmd+", "Ctrl+Shift+").replace("Cmd+", "Ctrl+").replace("Finder / Explorer", "Explorer");
       #endif
    }

    /** A long name cut in the middle to at most maxChars characters, keeping its start and its end (where file
        names carry their take numbers): "ANMLFarm_Pig...al_Close_01". Short names come back as they are. */
    inline juce::String middleTruncated(const juce::String& text, int maxChars)
    {
        if (text.length() <= maxChars)
            return text;
        const int keep = maxChars - 3; // the three dots
        if (keep < 2)
            return {};
        const int head = (keep + 1) / 2, tail = keep - head;
        return text.substring(0, head).trimEnd() + "..." + text.substring(text.length() - tail).trimStart();
    }

    /** The name, cut in the middle as far as it takes to fit maxWidth in this font (empty when even a few
        characters don't fit). */
    inline juce::String fitMiddle(const juce::String& text, const juce::Font& font, float maxWidth)
    {
        for (int chars = text.length(); chars >= 6; --chars)
        {
            const auto cut = middleTruncated(text, chars);
            if (juce::GlyphArrangement::getStringWidth(font, cut) <= maxWidth)
                return cut;
        }
        return {};
    }

    /** Colours for the tracks: lagoon, coral, gold, sea glass, sky, hibiscus, harbour, rope, lilac, palm. */
    inline juce::Colour trackColour(int lane)
    {
        static const juce::uint32 colours[10] = { 0xff3ccbc2, 0xffff8466, 0xfff2b84b, 0xff8fd9b0, 0xff86b4ff,
                                                  0xffe889b8, 0xff5fa8d3, 0xffd9a35f, 0xffb59cf2, 0xff7fd1a0 };
        return juce::Colour(colours[((lane % 10) + 10) % 10]);
    }

    /**
     * Typefaces loaded once and shared by every editor instance (SharedResourcePointer):
     *   display  Pirata One (OFL): the title and panel headings, in Title Case, never tiny
     *   heading  IBM Plex Sans Condensed SemiBold (OFL): names, buttons, emphasis
     *   label    IBM Plex Sans Condensed Medium, lightly tracked: small all-caps captions
     *   body     IBM Plex Sans Condensed Regular: values, hints, tooltips
     */
    struct Fonts
    {
        Fonts()
            : pirata  (juce::Typeface::createSystemTypefaceFor(BinaryData::PirataOneRegular_ttf, BinaryData::PirataOneRegular_ttfSize))
            , regular (juce::Typeface::createSystemTypefaceFor(BinaryData::IBMPlexSansCondensedRegular_ttf, BinaryData::IBMPlexSansCondensedRegular_ttfSize))
            , medium  (juce::Typeface::createSystemTypefaceFor(BinaryData::IBMPlexSansCondensedMedium_ttf, BinaryData::IBMPlexSansCondensedMedium_ttfSize))
            , bold    (juce::Typeface::createSystemTypefaceFor(BinaryData::IBMPlexSansCondensedSemiBold_ttf, BinaryData::IBMPlexSansCondensedSemiBold_ttfSize))
        {
        }

        juce::Font make(const juce::Typeface::Ptr& face, float height, float kerning = 0.0f) const
        {
            if (face == nullptr)
                return juce::Font(juce::FontOptions(height));
            return juce::Font(juce::FontOptions(face).withHeight(height)).withExtraKerningFactor(kerning);
        }

        juce::Font display(float height) const { return make(pirata, height, 0.02f); }
        juce::Font heading(float height) const { return make(bold, height * 1.1f, 0.03f); }
        juce::Font label(float height) const   { return make(medium, height * 1.1f, 0.06f); }
        juce::Font body(float height) const    { return make(regular, height * 1.1f); }

        juce::Typeface::Ptr pirata, regular, medium, bold;
    };
}

#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

/**
 * Shared look for Pitch Flattener (the same design system as Subbertone):
 * one palette and one font family (Azeret Mono, embedded).
 *
 * Every accent colour has exactly one meaning wherever it appears (knob arcs,
 * section dots, traces and readouts):
 *   teal   = detection: the detected input pitch and everything that shapes it
 *   amber  = target: the pitch being flattened to
 *   violet = correction: the shift applied, and the corrected output pitch
 *   rose   = warnings
 */
namespace Theme
{
    namespace Colours
    {
        inline const juce::Colour background  { 0xff0d0f14 };
        inline const juce::Colour panel       { 0xff141720 };
        inline const juce::Colour panelRaised { 0xff1c202b };
        inline const juce::Colour outline     { 0xff262b37 };
        inline const juce::Colour outlineSoft { 0xff1d212b };

        inline const juce::Colour text        { 0xffece9e3 };
        inline const juce::Colour textDim     { 0xff8d92a0 };
        inline const juce::Colour textFaint   { 0xff565b68 };

        inline const juce::Colour detect      { 0xff5ec8d8 }; // teal
        inline const juce::Colour target      { 0xffffb54d }; // amber
        inline const juce::Colour correction  { 0xffa992ff }; // violet
        inline const juce::Colour warning     { 0xffef6f8e }; // rose

        inline const juce::Colour good        { 0xff7fd49b };
        inline const juce::Colour bad         { 0xffe5686b };

        inline const juce::Colour scope       { 0xff090b0f };
        inline const juce::Colour grid        { 0xff1e2633 };
    }

    /** Typefaces loaded once and shared by every component (via SharedResourcePointer). */
    struct Fonts
    {
        Fonts()
            : regular (juce::Typeface::createSystemTypefaceFor (BinaryData::AzeretMonoRegular_ttf, BinaryData::AzeretMonoRegular_ttfSize))
            , medium  (juce::Typeface::createSystemTypefaceFor (BinaryData::AzeretMonoMedium_ttf,  BinaryData::AzeretMonoMedium_ttfSize))
            , bold    (juce::Typeface::createSystemTypefaceFor (BinaryData::AzeretMonoBold_ttf,    BinaryData::AzeretMonoBold_ttfSize))
        {
        }

        juce::Font make (const juce::Typeface::Ptr& face, float height, float kerning = 0.0f) const
        {
            if (face == nullptr)
                return juce::Font (juce::FontOptions (height));

            return juce::Font (juce::FontOptions (face).withHeight (height)).withExtraKerningFactor (kerning);
        }

        // A tiny tracking (0.02 px at 12 px) makes JUCE switch the font's ligatures off: Azeret Mono's
        // "fi" would otherwise draw as "fı" and "<-" as an arrow.
        juce::Font body (float height) const    { return make (regular, height, 0.002f); }
        juce::Font label (float height) const   { return make (medium, height, 0.08f); }
        juce::Font heading (float height) const { return make (bold, height, 0.12f); }

        juce::Typeface::Ptr regular, medium, bold;
    };

    //==========================================================================
    // Shared text helpers

    /** "A4", "C#3" ... or an empty string when the frequency is not a usable pitch. */
    inline juce::String noteName (float hz)
    {
        if (! std::isfinite (hz) || hz < 8.0f || hz > 30000.0f)
            return {};

        static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int midi = juce::roundToInt (69.0 + 12.0 * std::log2 ((double) hz / 440.0));

        if (midi < 0 || midi > 135)
            return {};

        return juce::String (names[midi % 12]) + juce::String (midi / 12 - 1);
    }

    /** Deviation from the nearest equal-tempered note, in cents (-50..50). */
    inline float centsFromNote (float hz)
    {
        if (! std::isfinite (hz) || hz < 8.0f)
            return 0.0f;

        const double midi = 69.0 + 12.0 * std::log2 ((double) hz / 440.0);
        return (float) ((midi - std::round (midi)) * 100.0);
    }

    /** "440 Hz" below 1 kHz, "1.25 kHz" above, one decimal from 10 kHz up. */
    inline juce::String formatHertz (double v)
    {
        if (v >= 1000.0)
            return juce::String (v / 1000.0, v >= 10000.0 ? 1 : 2) + " kHz";

        return v < 100.0 ? juce::String (v, 1) + " Hz"
                         : juce::String (juce::roundToInt (v)) + " Hz";
    }

    /** One decimal with an explicit sign, and no "-0.0" for values that round to zero. */
    inline juce::String signedText (double v, const char* unit, int decimals = 1)
    {
        const double scale = std::pow (10.0, decimals);
        const double rounded = std::round (v * scale) / scale;

        if (std::abs (rounded) < 0.5 / scale)
            return juce::String (0.0, decimals) + unit;

        return (rounded > 0.0 ? "+" : "") + juce::String (rounded, decimals) + unit;
    }
}

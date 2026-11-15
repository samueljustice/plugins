#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

/**
 * Pitch Flattener's look: one palette and one font family (Azeret Mono, embedded).
 *
 * Every accent colour has exactly one meaning wherever it appears (buttons, gauges, traces, the
 * flight view, banners and readouts), so the colour alone tells the user what they are looking at:
 *   orange  = things you set: controls, the target pitch, headings
 *   cyan    = what was heard: the sound's pitch as read
 *   lime    = the flattened result, "go", success
 *   magenta = recording, warnings, problems
 * Amber is orange's highlight (a pressed or hot control), never a meaning of its own.
 */
namespace Theme
{
    namespace Colours
    {
        inline const juce::Colour space      { 0xff05070d }; // the window ("void")
        inline const juce::Colour panel      { 0xff0b1220 };
        inline const juce::Colour panelEdge  { 0xff1b2a44 };
        inline const juce::Colour grid       { 0xff13203a };

        inline const juce::Colour orange     { 0xffff8a1f }; // holo orange: things you set
        inline const juce::Colour amber      { 0xffffc24a }; // orange's highlight
        inline const juce::Colour cyan       { 0xff2ee6ff }; // ion cyan: the pitch as heard
        inline const juce::Colour lime       { 0xffc6ff3d }; // acid lime: the flattened result
        inline const juce::Colour magenta    { 0xffff2e7e }; // alert magenta: recording, problems

        inline const juce::Colour text       { 0xffe8f1ff };
        inline const juce::Colour textDim    { 0xff6f86a8 };
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
        juce::Font body (float height) const    { return make (medium, height, 0.002f); }   // upright body and status text
        juce::Font label (float height) const   { return make (medium, height, 0.08f); }
        juce::Font heading (float height) const { return make (bold, height, 0.12f); }
        // Button labels, banners and the title: bold and wide-tracked, drawn sheared (Hud::drawSheared).
        juce::Font display (float height) const { return make (bold, height, 0.16f); }

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

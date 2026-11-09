#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

/**
 * Shared look for Roarschach: one palette and one font family (Azeret Mono, embedded), the same chrome
 * as its sibling plug-ins. Each block of the signal chain has its own accent, in chain order (clean, pitch,
 * growl, liquid, roar, sub, then the two movable modules detail and vocoder, and output). Knobs, panels,
 * menus and the creature view share them so the UI reads at a glance.
 *
 * The generic tokens (input, harmonics, filter, overtones, master, output) keep their names so the shared
 * chrome (LookAndFeel, About window) reads the same as in the sibling plug-ins.
 */
namespace Theme
{
    /** The line under the name in the editor header and the About window. */
    inline constexpr const char* tagline = "SAMMY J  /  CREATURE VOICE DESIGNER";

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

        inline const juce::Colour input       { 0xff5ec8d8 }; // teal
        inline const juce::Colour harmonics   { 0xffef6f8e }; // rose
        inline const juce::Colour filter      { 0xffa992ff }; // violet
        inline const juce::Colour overtones   { 0xff6fd6b4 }; // mint
        inline const juce::Colour master      { 0xffc9cdd8 }; // silver
        inline const juce::Colour output      { 0xffffb54d }; // amber

        // Signal-chain accents, one per block (in chain order).
        inline const juce::Colour clean       { 0xff7f97a6 }; // teal-grey: input and the automatic clean-up
        inline const juce::Colour pitch       { 0xffa992ff }; // violet
        inline const juce::Colour growl       { 0xffef6f8e }; // rose
        inline const juce::Colour liquid      { 0xff5ec8d8 }; // teal
        inline const juce::Colour roar        { 0xffff7a45 }; // ember
        inline const juce::Colour sub         { 0xff6f8dff }; // blue
        inline const juce::Colour detail      { 0xff6fd6b4 }; // mint
        inline const juce::Colour vocoder     { 0xffc6e86b }; // acid
        // The OUT block uses output (amber), which is also the chrome accent.

        inline const juce::Colour good        { 0xff7fd49b };
        inline const juce::Colour bad         { 0xffe5686b };

        inline const juce::Colour scope       { 0xff090b0f };
        inline const juce::Colour grid        { 0xff1e2633 };
    }

    /** Typefaces loaded once and shared by every editor instance (SharedResourcePointer). */
    struct Fonts
    {
        Fonts()
            : regular (juce::Typeface::createSystemTypefaceFor(BinaryData::AzeretMonoRegular_ttf, BinaryData::AzeretMonoRegular_ttfSize))
            , medium  (juce::Typeface::createSystemTypefaceFor(BinaryData::AzeretMonoMedium_ttf,  BinaryData::AzeretMonoMedium_ttfSize))
            , bold    (juce::Typeface::createSystemTypefaceFor(BinaryData::AzeretMonoBold_ttf,    BinaryData::AzeretMonoBold_ttfSize))
        {
        }

        juce::Font make(const juce::Typeface::Ptr& face, float height, float kerning = 0.0f) const
        {
            if (face == nullptr)
                return juce::Font(juce::FontOptions(height));

            return juce::Font(juce::FontOptions(face).withHeight(height)).withExtraKerningFactor(kerning);
        }

        // A tiny tracking (0.02 px at 12 px) makes JUCE switch the font's ligatures off: Azeret Mono's
        // "fi" would otherwise draw as "fı" and "<-" as an arrow.
        juce::Font body(float height) const    { return make(regular, height, 0.002f); }
        juce::Font label(float height) const   { return make(medium, height, 0.08f); }
        juce::Font heading(float height) const { return make(bold, height, 0.12f); }

        juce::Typeface::Ptr regular, medium, bold;
    };
}

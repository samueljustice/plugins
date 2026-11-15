#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

/**
 * Shared look for Reversinator: one palette and one font family (Azeret Mono,
 * embedded). It borrows Backwards Machine's cues - warm metal, green mode
 * lamps, amber hourglass sand, blue mix faders - on a modern dark panel.
 * Colours are tied to meaning so the controls read at a glance.
 */
namespace Theme
{
    namespace Colours
    {
        inline const juce::Colour background  { 0xff121110 };
        inline const juce::Colour panel       { 0xff1a1917 };
        inline const juce::Colour panelRaised { 0xff25231f };
        inline const juce::Colour outline     { 0xff35322c };
        inline const juce::Colour outlineSoft { 0xff27251f };

        inline const juce::Colour text        { 0xffefe9df };
        inline const juce::Colour textDim     { 0xffa39b8e };
        inline const juce::Colour textFaint   { 0xff6b655b };

        // Keycap colours, like moulded plastic: each control keeps one, so it reads at a glance.
        inline const juce::Colour mode        { 0xff5fd6a8 }; // mint (the MODE panel, and the first mode key)
        inline const juce::Colour modeSecond  { 0xffffc53d }; // mustard
        inline const juce::Colour modeThird   { 0xffff6f59 }; // coral
        inline const juce::Colour reverser    { 0xff3fa9ff }; // electric blue
        inline const juce::Colour sand        { 0xffffb347 }; // hourglass sand
        inline const juce::Colour feedback    { 0xffff5c9d }; // hot pink
        inline const juce::Colour mix         { 0xff3ed6c8 }; // turquoise (wet)
        inline const juce::Colour dry         { 0xffb69cff }; // lilac

        inline const juce::Colour scope       { 0xff0c0b0a };
        inline const juce::Colour grid        { 0xff24221e };
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
        // "fi" would otherwise draw as "fı" and "<-" as an arrow. (As in Subbertone.)
        juce::Font body(float height) const    { return make(regular, height, 0.002f); }
        juce::Font label(float height) const   { return make(medium, height, 0.08f); }
        juce::Font heading(float height) const { return make(bold, height, 0.12f); }

        juce::Typeface::Ptr regular, medium, bold;
    };
}

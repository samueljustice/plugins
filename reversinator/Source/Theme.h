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

        inline const juce::Colour mode        { 0xff8be07f }; // lamp green
        inline const juce::Colour reverser    { 0xff58b6ff }; // logo-arrow blue
        inline const juce::Colour sand        { 0xffffb347 }; // hourglass sand
        inline const juce::Colour feedback    { 0xffef7a8e }; // loop rose
        inline const juce::Colour mix         { 0xff8fc1ff }; // fader blue

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

        juce::Font body(float height) const    { return make(regular, height); }
        juce::Font label(float height) const   { return make(medium, height, 0.08f); }
        juce::Font heading(float height) const { return make(bold, height, 0.12f); }

        juce::Typeface::Ptr regular, medium, bold;
    };
}

#pragma once

#include <JuceHeader.h>

/** Colours and type. Two palettes: paper (day) and ink (night), both low-contrast enough to sit
    beside a mix window for hours without glaring. */
namespace Theme
{
    struct Palette
    {
        juce::Colour background, backgroundEdge, ink, muted, faint, accent, hairline;
    };

    inline Palette palette (bool night)
    {
        if (night)
            return { juce::Colour (0xff1d1c21), juce::Colour (0xff151418), juce::Colour (0xffe8e0d1),
                     juce::Colour (0xff9d9486), juce::Colour (0xff57525a), juce::Colour (0xffc9a46c),
                     juce::Colour (0xff35323a) };

        return { juce::Colour (0xfff4efe5), juce::Colour (0xffe9e1d3), juce::Colour (0xff2c2824),
                 juce::Colour (0xff7b7166), juce::Colour (0xffb9ae9f), juce::Colour (0xff9a5a3a),
                 juce::Colour (0xffd8cebf) };
    }

    /** Cormorant Garamond, embedded. Loaded once and shared. */
    struct Type
    {
        juce::Typeface::Ptr medium, italic, semibold;

        static const Type& get()
        {
            static const Type type;
            return type;
        }

    private:
        Type()
        {
            medium = juce::Typeface::createSystemTypefaceFor (BinaryData::CormorantGaramondMedium_ttf,
                                                              BinaryData::CormorantGaramondMedium_ttfSize);
            italic = juce::Typeface::createSystemTypefaceFor (BinaryData::CormorantGaramondMediumItalic_ttf,
                                                              BinaryData::CormorantGaramondMediumItalic_ttfSize);
            semibold = juce::Typeface::createSystemTypefaceFor (BinaryData::CormorantGaramondSemiBold_ttf,
                                                                BinaryData::CormorantGaramondSemiBold_ttfSize);
        }
    };

    inline juce::Font font (const juce::Typeface::Ptr& face, float height, float tracking = 0.0f)
    {
        return juce::Font (juce::FontOptions (face).withHeight (height).withKerningFactor (tracking));
    }

    inline juce::Font text (float height)        { return font (Type::get().medium, height); }
    inline juce::Font italic (float height)      { return font (Type::get().italic, height); }
    /** Small capitals look: semibold, upper-cased by the caller, spaced out. */
    inline juce::Font caps (float height, float tracking = 0.14f) { return font (Type::get().semibold, height, tracking); }
}

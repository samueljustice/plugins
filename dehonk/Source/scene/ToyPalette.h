// The colours of De-Honk's window and its pond: off-beat pastels on cream with thin warm-brown
// lines, in the manner of Japanese graphic design of the early 2000s. Flat, a little strange, and
// not cute. One place, so that the scene, the band view and the controls agree.
#pragma once

#include <JuceHeader.h>

namespace dehonk::toy
{
    inline const juce::Colour ink        { 0xff3a2e2a };   // lines and text: a warm dark brown, never black
    inline const juce::Colour paper      { 0xfffffdf6 };
    inline const juce::Colour cream      { 0xfff5eed8 };   // the window
    inline const juce::Colour creamDeep  { 0xffeee3c4 };   // the rays behind everything
    inline const juce::Colour sky        { 0xffbfe6de };   // mint
    inline const juce::Colour skyPale    { 0xffe6f4ee };
    inline const juce::Colour lawnA      { 0xffcfe08a };
    inline const juce::Colour lawnB      { 0xffd9e89c };
    inline const juce::Colour water      { 0xff7cc4d8 };   // bars: what comes in
    inline const juce::Colour waterPale  { 0xffbfe3ec };
    inline const juce::Colour jacketRed  { 0xffe5533b };   // cuts: what is being taken out (a tomato red)
    inline const juce::Colour sunYellow  { 0xfff2c14e };   // anything live or selected (mustard)
    inline const juce::Colour pink       { 0xfff4b6c6 };
    inline const juce::Colour orange     { 0xfff2946b };
    inline const juce::Colour mint       { 0xffa9dcc4 };
    inline const juce::Colour lavender   { 0xffb9a8e0 };
    inline const juce::Colour grey       { 0xffd3ccba };   // switched off

    /** One colour per module, the same as its strip of the bank in the scene: Woof, Tub, Fog, Bark. */
    inline const juce::Colour module[4] { juce::Colour (0xffb9a8e0), juce::Colour (0xfff2c14e), juce::Colour (0xffb7c468), juce::Colour (0xfff2946b) };
    inline const juce::Colour click      { 0xff8fd0e2 };

    /** The five stripes of the little rainbow used as a rule here and there. */
    inline const juce::Colour rainbow[5] { juce::Colour (0xffe5533b), juce::Colour (0xfff2c14e), juce::Colour (0xffb7c468), juce::Colour (0xff8fd0e2), juce::Colour (0xffb9a8e0) };

    inline constexpr float outline = 1.25f;     // thin lines everywhere
    inline constexpr float dropY = 0.0f;        // no shadows: everything lies flat
    inline constexpr float cardCorner = 10.0f;
}

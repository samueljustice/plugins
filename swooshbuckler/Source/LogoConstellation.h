#pragma once

#include <JuceHeader.h>

#include "LogoOutline.h"

#include <utility>
#include <vector>

/**
 * The logo as a constellation: three dozen stars on the corners that make the mark what it is, joined by lines.
 *
 * A constellation is a few stars and long straight lines, so the stars are placed by hand, on the artwork, where
 * a person drawing the mark from memory would put them:
 *  - the cherry, all the way round (it hangs in front of the grenade), with the notch its stem sits in;
 *  - the grenade's cap as the angular block it is, with the lever running from it down the left to its tip;
 *  - the pull ring, a small circle of faint stars hanging on the lever;
 *  - the round body, from the ring round the bottom to where the cherry hides it;
 *  - the two stems, parting from one bright star at the top.
 * The stars that hold the shape (the top, the cap's corners, the lever's tip, the bottom of the body, the cherry's
 * notch) are heavy; the ones that only carry a curve round are light.
 */
struct LogoConstellation
{
    struct Star
    {
        juce::Point<float> at;   // normalised, centred, y-up (the logo's longer side is 1)
        float weight = 0.0f;     // 0 for a star that only carries a curve round, 1 for one the shape hangs on
        bool cherry = false;
    };

    std::vector<Star> stars;
    std::vector<std::pair<int, int>> lines;

    static LogoConstellation build(const LogoOutline& logo);
};

#pragma once

#include <JuceHeader.h>

#include "LogoOutline.h"

#include <vector>

/**
 * The logo as a constellation: some forty stars on the corners and curves that make the mark what it is, joined
 * by lines.
 *
 * A constellation is a few stars and straight lines, so the stars are placed by hand, on the artwork, where a
 * person drawing the mark from memory would put them, and nothing else is drawn:
 *  - the pull ring, a true circle of ten small stars: the brightest line in the figure, because it is the part of
 *    the mark nothing else has;
 *  - the round body, from under the ring round the bottom to where the cherry hides it (the line stops on the
 *    cherry's edge, which is what puts the cherry in front);
 *  - the cherry, all the way round, with the dimple its stem sits in;
 *  - the cap as one tilted block, and the lever as one stroke from the ring down the left;
 *  - the two stems, parting from one bright star near the top.
 * The outlines (ring, body, cherry) are the strong lines, the cap and stems are quieter and the lever and the
 * dimple quieter still, so the eye finds a ring, a ball and a cherry before it finds anything else.
 */
struct LogoConstellation
{
    struct Star
    {
        juce::Point<float> at;   // normalised, centred, y-up (the logo's longer side is 1)
        float weight = 0.0f;     // 0 for a star that only carries a curve round, 1 for one the shape hangs on
        bool cherry = false;
    };

    enum Tier : int { detail = 0, structure, silhouette, ring };

    struct Line
    {
        int first = 0, second = 0;
        int tier = structure;
    };

    std::vector<Star> stars;
    std::vector<Line> lines;

    static LogoConstellation build(const LogoOutline& logo);
};

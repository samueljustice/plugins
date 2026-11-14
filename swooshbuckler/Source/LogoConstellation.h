#pragma once

#include <JuceHeader.h>

#include "LogoOutline.h"

#include <utility>
#include <vector>

/**
 * The logo as a constellation: a few dozen stars on the shapes that make the mark what it is, joined by lines.
 *
 * The logo's outline has a contour for every fill and every gap in the artwork, which is far too much for a
 * handful of stars to say. What a person recognises is the silhouette, so that is what is traced here:
 *  - the cherry's whole outline (it hangs in front of the grenade);
 *  - the pull ring, as a clean circle;
 *  - the grenade's outer edge (the round body, the cap and the lever), wherever the cherry and the ring do not
 *    cover it;
 *  - the two stems and the lever's tail, each down its middle, as single lines.
 * Stars sit on the corners and are spaced around the curves; the sharper the corner, the heavier the star.
 *
 * Built once (it rasterises the mark into a small mask and traces that); drawing only reads the result.
 */
struct LogoConstellation
{
    struct Star
    {
        juce::Point<float> at;   // normalised, centred, y-up (the logo's longer side is 1)
        float weight = 0.0f;     // 0 on a gentle curve, 1 on a sharp corner or at the end of a line
        bool cherry = false;
    };

    std::vector<Star> stars;
    std::vector<std::pair<int, int>> lines;

    static LogoConstellation build(const LogoOutline& logo);
};

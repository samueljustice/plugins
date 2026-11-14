#include "LogoConstellation.h"

#include <cmath>

namespace
{
    // Where the stars go, in the artwork's own coordinates (sj-logo.svg, 1080 across, y down).
    struct Placed { float x, y, weight; bool cherry; };

    constexpr float c_ringX = 316.3f, c_ringY = 455.1f, c_ringRadius = 91.8f;  // the middle of the ring's wire
    constexpr int   c_ringStars = 8;
    constexpr float c_ringFirst = -72.0f;  // degrees: this star is the cap's left shoulder, where it comes out of the ring

    enum : int
    {
        // The cherry, clockwise from the notch its stem sits in.
        notch, cherry1, cherry2, cherry3, cherry4, cherry5, cherry6, cherry7, cherry8, cherry9, cherry10,
        // The stems: the tip, where the two part, and the middle of the cherry's.
        tip, parting, stemMiddle,
        // The cap: its top, the corner the grenade's stem lands on, round the right side and back along the skirt.
        capTop, capCorner, capRight, capWaist, skirtRight, skirtLeft,
        // The lever: where it hinges on the cap, and its tip down the left.
        hinge, leverTip,
        // The body, from under the lever round the bottom.
        body1, body2, body3, bodyBottom, body5, body6,
        ring0   // and the ring's stars after it
    };

    constexpr Placed c_placed[] = {
        { 681.0f, 493.0f, 0.85f, true },   // notch
        { 626.0f, 492.0f, 0.10f, true },
        { 553.0f, 518.0f, 0.25f, true },
        { 522.0f, 595.0f, 0.15f, true },
        { 548.0f, 693.0f, 0.20f, true },
        { 614.0f, 763.0f, 0.15f, true },
        { 708.0f, 784.0f, 0.55f, true },
        { 797.0f, 756.0f, 0.15f, true },
        { 863.0f, 635.0f, 0.45f, true },
        { 846.0f, 528.0f, 0.20f, true },
        { 773.0f, 460.0f, 0.45f, true },

        { 591.0f, 211.0f, 1.00f, false },  // tip
        { 578.0f, 246.0f, 0.20f, false },  // parting
        { 630.0f, 388.0f, 0.05f, false },  // stemMiddle

        { 425.0f, 316.0f, 0.70f, false },  // capTop
        { 527.0f, 348.0f, 0.80f, false },  // capCorner
        { 556.0f, 393.0f, 0.35f, false },  // capRight
        { 534.0f, 472.0f, 0.30f, false },  // capWaist
        { 514.0f, 517.0f, 0.65f, false },  // skirtRight
        { 417.0f, 495.0f, 0.20f, false },  // skirtLeft

        { 478.0f, 362.0f, 0.25f, false },  // hinge
        { 182.0f, 655.0f, 0.85f, false },  // leverTip

        { 236.0f, 612.0f, 0.15f, false },  // body
        { 257.0f, 702.0f, 0.25f, false },
        { 316.0f, 765.0f, 0.20f, false },
        { 403.0f, 793.0f, 0.70f, false },  // bodyBottom
        { 477.0f, 781.0f, 0.25f, false },
        { 541.0f, 739.0f, 0.10f, false },
    };

    // Ring stars, by where they fall: 0 is the cap's shoulder, 1 is on the lever, 2 is by the skirt, 5 is where
    // the lever leaves the ring and the body comes out from under it.
    constexpr int c_onShoulder = ring0, c_onLever = ring0 + 1, c_bySkirt = ring0 + 2, c_leverOut = ring0 + 5;

    constexpr std::pair<int, int> c_lines[] = {
        { notch, cherry1 }, { cherry1, cherry2 }, { cherry2, cherry3 }, { cherry3, cherry4 }, { cherry4, cherry5 },
        { cherry5, cherry6 }, { cherry6, cherry7 }, { cherry7, cherry8 }, { cherry8, cherry9 }, { cherry9, cherry10 },
        { cherry10, notch },

        { tip, parting }, { parting, stemMiddle }, { stemMiddle, notch }, { parting, capCorner },

        { c_onShoulder, capTop }, { capTop, capCorner }, { capCorner, capRight }, { capRight, capWaist },
        { capWaist, skirtRight }, { skirtRight, skirtLeft }, { skirtLeft, c_bySkirt },

        { capRight, hinge }, { hinge, c_onLever }, { c_onLever, c_leverOut }, { c_leverOut, leverTip },

        { c_leverOut, body1 }, { body1, body2 }, { body2, body3 }, { body3, bodyBottom }, { bodyBottom, body5 },
        { body5, body6 },
        { body6, cherry4 },   // the cherry hides the rest of the body
    };
}

LogoConstellation LogoConstellation::build(const LogoOutline& logo)
{
    LogoConstellation result;
    if (! logo.isLoaded())
        return result;

    const auto toLogo = logo.getNormalisingTransform();
    for (const auto& p : c_placed)
        result.stars.push_back({ juce::Point<float>(p.x, p.y).transformedBy(toLogo), p.weight, p.cherry });

    for (int i = 0; i < c_ringStars; ++i)
    {
        const float angle = juce::degreesToRadians(c_ringFirst + 360.0f * static_cast<float>(i) / static_cast<float>(c_ringStars));
        const juce::Point<float> at(c_ringX + c_ringRadius * std::cos(angle), c_ringY + c_ringRadius * std::sin(angle));
        result.stars.push_back({ at.transformedBy(toLogo), i == 0 ? 0.3f : 0.0f, false });
        result.lines.push_back({ ring0 + i, ring0 + (i + 1) % c_ringStars });
    }

    for (const auto& line : c_lines)
        result.lines.push_back(line);

    return result;
}

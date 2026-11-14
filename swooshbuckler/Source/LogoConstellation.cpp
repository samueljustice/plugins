#include "LogoConstellation.h"

#include <cmath>

namespace
{
    // Where the stars go, in the artwork's own coordinates (sj-logo.svg, 1080 across, y down).
    struct Placed { float x, y, weight; bool cherry; };

    constexpr float c_ringX = 316.3f, c_ringY = 455.1f, c_ringRadius = 91.8f;  // the middle of the ring's wire
    constexpr int   c_ringStars = 10;
    constexpr float c_ringFirst = -72.0f;  // degrees: this star is where the cap's top edge comes out of the ring

    constexpr float c_bodyX = 413.0f, c_bodyY = 610.0f, c_bodyRadius = 182.0f;  // the grenade's round body
    constexpr float c_bodyAngles[] = { 200.0f, 172.0f, 144.0f, 116.0f, 88.0f, 60.0f };  // from under the ring, round the bottom
    constexpr int   c_bodyStars = static_cast<int>(std::size(c_bodyAngles));
    constexpr int   c_bodyBottom = 4;      // the one at the very bottom is a little heavier

    enum : int
    {
        // The cherry, clockwise from where its stem crosses its top edge; cherryJoin is where the body runs in behind it.
        cherryTop, cherry1, cherry2, cherry3, cherry4, cherryJoin, cherry5, cherry6, cherry7, cherry8, cherry9, cherry10,
        // The dimple the stem sits in.
        dimpleLeft, dimple, dimpleRight,
        // The stems: the tip, where the two part, and the middle of the cherry's.
        tip, parting, stemMiddle,
        // The cap: its top, the corner the grenade's stem lands on, down the right side and back along the skirt.
        capTop, capCorner, capRight, capWaist, skirtRight, skirtLeft,
        leverTip,
        ring0,                       // the ring's stars
        body0 = ring0 + c_ringStars  // and the body's after them
    };

    constexpr Placed c_placed[] = {
        { 681.0f, 490.0f, 0.10f, true },   // cherryTop
        { 626.0f, 492.0f, 0.10f, true },
        { 553.0f, 518.0f, 0.25f, true },
        { 522.0f, 595.0f, 0.10f, true },
        { 548.0f, 693.0f, 0.10f, true },
        { 570.0f, 731.0f, 0.00f, true },   // cherryJoin
        { 614.0f, 763.0f, 0.15f, true },
        { 708.0f, 784.0f, 0.35f, true },
        { 797.0f, 756.0f, 0.15f, true },
        { 863.0f, 635.0f, 0.30f, true },
        { 846.0f, 528.0f, 0.15f, true },
        { 773.0f, 460.0f, 0.35f, true },

        { 612.0f, 529.0f, 0.00f, true },   // dimpleLeft
        { 679.0f, 541.0f, 0.55f, true },   // dimple
        { 745.0f, 515.0f, 0.00f, true },   // dimpleRight

        { 591.0f, 211.0f, 0.05f, false },  // tip
        { 578.0f, 246.0f, 1.00f, false },  // parting
        { 630.0f, 388.0f, 0.05f, false },  // stemMiddle

        { 425.0f, 316.0f, 0.45f, false },  // capTop
        { 527.0f, 348.0f, 0.50f, false },  // capCorner
        { 556.0f, 393.0f, 0.15f, false },  // capRight
        { 534.0f, 472.0f, 0.05f, false },  // capWaist
        { 514.0f, 517.0f, 0.35f, false },  // skirtRight
        { 417.0f, 495.0f, 0.05f, false },  // skirtLeft

        { 182.0f, 655.0f, 0.25f, false },  // leverTip
    };

    // Ring stars, by where they fall: 0 is on the cap's top edge, 1 is the eyelet that holds the ring to the cap,
    // 3 is by the skirt, 6 is where the lever leaves the ring and the body comes out from under it.
    constexpr int c_onCapTop = ring0, c_eyelet = ring0 + 1, c_bySkirt = ring0 + 3, c_leverOut = ring0 + 6;

    using Tier = LogoConstellation::Tier;
    constexpr LogoConstellation::Line c_lines[] = {
        { cherryTop, cherry1, Tier::silhouette }, { cherry1, cherry2, Tier::silhouette }, { cherry2, cherry3, Tier::silhouette },
        { cherry3, cherry4, Tier::silhouette }, { cherry4, cherryJoin, Tier::silhouette }, { cherryJoin, cherry5, Tier::silhouette },
        { cherry5, cherry6, Tier::silhouette }, { cherry6, cherry7, Tier::silhouette }, { cherry7, cherry8, Tier::silhouette },
        { cherry8, cherry9, Tier::silhouette }, { cherry9, cherry10, Tier::silhouette }, { cherry10, cherryTop, Tier::silhouette },

        { dimpleLeft, dimple, Tier::detail }, { dimple, dimpleRight, Tier::detail },

        { tip, parting, Tier::structure }, { parting, stemMiddle, Tier::structure }, { stemMiddle, cherryTop, Tier::structure },
        { cherryTop, dimple, Tier::structure }, { parting, capCorner, Tier::structure },

        { c_onCapTop, capTop, Tier::structure }, { capTop, capCorner, Tier::structure }, { capCorner, capRight, Tier::structure },
        { capRight, capWaist, Tier::structure }, { capWaist, skirtRight, Tier::structure }, { skirtRight, skirtLeft, Tier::structure },
        { skirtLeft, c_bySkirt, Tier::structure },

        { c_leverOut, leverTip, Tier::detail },

        { c_leverOut, body0, Tier::silhouette },
        { body0 + c_bodyStars - 1, cherryJoin, Tier::silhouette },   // the cherry hides the rest of the body
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

    auto onCircle = [](float cx, float cy, float radius, float degrees)
    {
        const float angle = juce::degreesToRadians(degrees);
        return juce::Point<float>(cx + radius * std::cos(angle), cy + radius * std::sin(angle));
    };

    for (int i = 0; i < c_ringStars; ++i)
    {
        const auto at = onCircle(c_ringX, c_ringY, c_ringRadius, c_ringFirst + 360.0f * static_cast<float>(i) / static_cast<float>(c_ringStars));
        result.stars.push_back({ at.transformedBy(toLogo), ring0 + i == c_eyelet ? 0.5f : 0.1f, false });
        result.lines.push_back({ ring0 + i, ring0 + (i + 1) % c_ringStars, Tier::ring });
    }

    for (int i = 0; i < c_bodyStars; ++i)
    {
        const auto at = onCircle(c_bodyX, c_bodyY, c_bodyRadius, c_bodyAngles[i]);
        result.stars.push_back({ at.transformedBy(toLogo), i == c_bodyBottom ? 0.4f : 0.1f, false });
        if (i > 0)
            result.lines.push_back({ body0 + i - 1, body0 + i, Tier::silhouette });
    }

    for (const auto& line : c_lines)
        result.lines.push_back(line);

    return result;
}

#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <vector>

/**
 * A two-channel signed distance field of the maker's logo (Resources/sj-logo.svg) for the creature
 * view's hidden morph: R = the black group, G = the red group, each encoded as
 * clamp(d / 0.125, -1, 1) * 0.5 + 0.5 with d in logo-box units (the box is 0..1, positive outside).
 * Rows run bottom to top (GL order). The artwork is fitted into the box with a 6 % margin.
 *
 * Built once per process (the SVG is parsed into Drawables, so on the message thread): each group
 * is filled at twice the output size, a separable exact Euclidean distance transform runs on the
 * inside and outside masks, and the signed distances are averaged down.
 */
struct LogoSdf
{
    static constexpr int c_size = 256;
    static constexpr float c_range = 0.125f;   // distance (box units) that maps to the encoding's ends

    /** The RGBA8 texels (c_size * c_size * 4), empty if the SVG could not be read. */
    std::vector<std::uint8_t> rgba;
    /** Area of each group's filled path, as a fraction of the box (for tests). */
    float grenadeArea = 0.0f, cherryArea = 0.0f;
    double buildMilliseconds = 0.0;

    LogoSdf();   // builds from BinaryData

    /** The one built from BinaryData, kept for the life of the process (first call on the message thread). */
    static const LogoSdf& shared();

    static LogoSdf build (const void* svgData, size_t svgSize, int size);

    /** Decodes texel (x, y) of a channel back to a distance in box units. */
    float distanceAt (int x, int y, int channel) const;

private:
    LogoSdf (int) {}
};

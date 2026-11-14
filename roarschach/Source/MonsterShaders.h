#pragma once

#include <string>

namespace roar::shaders
{
    /** The creature view's programs: the ink creature's single pass (tier 0, no buffer), one step
        of its ink buffer and the draw from that buffer (tiers 1 to 3), the upscale copy and the
        half-float probe. The first three are built from one shared block, so the tiers cannot
        drift apart. */
    enum class Program
    {
        singlePass,   // the ink creature without any buffer
        sim,          // one step of the ink buffer: the pigment rule
        composite,    // the ink creature drawn from the buffer
        copy,         // a texture onto the target (the half-size pass's upscale, the tier crossfade)
        probe,        // writes known half-float values (can this GPU render to RGBA16F?)
        numPrograms
    };

    /** The shared vertex shader (a full-screen quad, attribute aPosition at location 0), GLSL 1.10. */
    const char* vertex();

    /** A program's fragment shader, GLSL 1.10 (concatenated from the shared chunks; setup only).
        textureLod: the context can read a texture at an explicit level of detail. */
    std::string fragment (Program, bool textureLod = true);

    const char* name (Program);
}

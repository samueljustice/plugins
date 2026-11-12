#pragma once

#include <string>

namespace roar::shaders
{
    /** The creature view's programs. The fluid tiers run every one but the single pass; tier 0 runs
        only the single pass (today's look plus a stateless twirl). */
    enum class Program
    {
        creatureField, emitter, advect, curl, forces, divergence, jacobi, gradient, dye, light,
        bright, down, up, composite, copy, probe, singlePass,
        inkSinglePass,   // the ink creature without any buffer (see c_inkCommon)
        inkSim,          // one step of the ink buffer: the pigment rule
        inkComposite,    // the ink creature drawn from the buffer
        numPrograms
    };

    /** The shared vertex shader (a full-screen quad, attribute aPosition at location 0), GLSL 1.10. */
    const char* vertex();

    /** A program's fragment shader, GLSL 1.10 (concatenated from the shared chunks; setup only).
        textureLod: the context can read a texture at an explicit level of detail. */
    std::string fragment (Program, bool textureLod = true);

    const char* name (Program);
}

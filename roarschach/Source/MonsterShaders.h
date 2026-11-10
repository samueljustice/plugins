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
        numPrograms
    };

    /** The shared vertex shader (a full-screen quad, attribute aPosition at location 0), GLSL 1.10. */
    const char* vertex();

    /** A program's fragment shader, GLSL 1.10 (concatenated from the shared chunks; setup only). */
    std::string fragment (Program);

    const char* name (Program);
}

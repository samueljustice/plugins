// Swirl's Tape Flanger and Doubler test (filled in by their work package): the delay over time, the null at zero,
// Varispeed's pitch and held offset, feedback that never runs away, the Doubler's delay law, Thicken's gain and
// its mono sum. Plain C++, no JUCE: SwirlFx is free of it.
//
//   SwooshbucklerSwirlFx       prints what it measures and ends with "ALL PASSED" (exit code 0) or "n FAILED".

#include "SwirlFx.h"

#include <cmath>
#include <iostream>
#include <string>

namespace
{
    int g_failures = 0;

    void check(bool ok, const std::string& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }
}

int main()
{
    std::cout << "Swirl types" << std::endl;
    {
        // The front wheel's mix law (equal power, half wet at the top of the wheel).
        check(SwirlFx::mixFor(0.0f) == 0.0f && std::abs(SwirlFx::mixFor(1.0f) - 0.5f) < 1.0e-6f, "mix: none at 0, half at the top of the wheel");
        SwirlFx fx;
        fx.prepare(48000.0);
        check(fx.getOffsetMs() == 0.0f, "varispeed: the held offset starts in the middle");
        check(fx.dryDelaySamples() == 384, "tape flanger: the dry ring is 8 ms in whole samples (384 at 48 kHz)");
        fx.prepare(44100.0);
        check(fx.dryDelaySamples() == 353, "tape flanger: 353 samples at 44.1 kHz");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : std::to_string(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

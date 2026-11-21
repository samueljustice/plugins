// Bus filter test (filled in by its work package): the response of each type and slope, spurs under a 20 Hz sweep,
// stability at full Resonance. Plain C++, no JUCE: SweepFilter is free of it.
//
//   SwooshbucklerSweepFilter   prints what it measures and ends with "ALL PASSED" (exit code 0) or "n FAILED".

#include "SweepFilter.h"

#include <algorithm>
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
    std::cout << "Filter laws" << std::endl;
    {
        // At its defaults the filter's cutoff and Q are the bus's earlier law.
        const SweepFilter::Settings defaults;
        float worst = 0.0f;
        for (float f = 0.0f; f <= 1.0f; f += 0.05f)
            for (float peak = 0.0f; peak <= 1.0f; peak += 0.1f)
            {
                const float before = std::max(170.0f, 170.0f * std::exp2(7.0f * std::sqrt(peak) * (1.0f - 0.9f * std::sqrt(f))));
                const float now = SweepFilter::cutoffHz(defaults, f, peak, 0.0f, 48000.0f);
                worst = std::max(worst, std::abs(now - before) / before);
                const float r = std::clamp((f - 0.6f) / 0.4f, 0.0f, 1.0f);
                worst = std::max(worst, std::abs(SweepFilter::q(defaults, f) - (0.7f + 1.5f * r * r)));
            }
        check(worst < 1.0e-5f, "defaults: the cutoff and Q are the bus's earlier law");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : std::to_string(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

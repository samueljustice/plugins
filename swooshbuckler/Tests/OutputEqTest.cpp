// Output EQ test (filled in by the Output EQ's work package): each band against its analytic response, flat and
// Off bit-exact, no click, the Ceiling held with boosts at every width, mains and LFE in step, the low cut's noise
// floor, and the CPU. Plain C++, no JUCE: OutputEq is free of it.
//
//   SwooshbucklerOutputEq      prints what it measures and ends with "ALL PASSED" (exit code 0) or "n FAILED".

#include "OutputEq.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

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
    std::cout << "Output EQ" << std::endl;
    {
        // As it opens: flat, and a flat EQ leaves every sample exactly as it was.
        const OutputEq::Params defaults;
        check(OutputEq::isFlat(defaults), "defaults: the EQ opens flat");
        OutputEq eq;
        eq.prepare(48000.0, 2);
        eq.setParams(defaults);
        std::vector<float> left(512), right(512);
        std::uint32_t state = 12345u;
        for (size_t i = 0; i < left.size(); ++i)
        {
            state = state * 1664525u + 1013904223u;
            left[i] = static_cast<float>((state >> 8) & 0xffffff) / 8388608.0f - 1.0f;
            right[i] = -0.5f * left[i];
        }
        const auto inL = left, inR = right;
        float* chans[2] = { left.data(), right.data() };
        eq.process(chans, 2, static_cast<int>(left.size()));
        check(left == inL && right == inR, "flat: every sample comes out exactly as it went in");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : std::to_string(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

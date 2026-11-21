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

        // Each type's place on the axis: High Pass mirrors, Band Pass and Notch stop at 12 kHz.
        SweepFilter::Settings s;
        s.type = SweepFilter::HighPass;
        const float closedHigh = SweepFilter::cutoffHz(s, 1.0f, 0.0f, 0.0f, 48000.0f); // closed: Low Pass would sit at 170 Hz
        const float openHigh = SweepFilter::cutoffHz(s, 0.0f, 1.0f, 0.0f, 48000.0f);   // open: Low Pass would sit at 21.6 kHz
        std::cout << "         high pass: closed " << closedHigh << " Hz, open " << openHigh << " Hz" << std::endl;
        check(std::abs(closedHigh - 440000.0f / 170.0f) < 1.0f && std::abs(openHigh - 440000.0f / 21600.0f) < 0.1f,
              "high pass: mirrored on the log axis (closed at 170 Hz it sits at 2.6 kHz and opens downward)");
        s.type = SweepFilter::BandPass;
        const float band = SweepFilter::cutoffHz(s, 0.0f, 1.0f, 0.0f, 48000.0f);
        s.type = SweepFilter::Notch;
        const float notch = SweepFilter::cutoffHz(s, 0.0f, 1.0f, 0.0f, 48000.0f);
        s.type = SweepFilter::BandPass;
        const float bandClosed = SweepFilter::cutoffHz(s, 1.0f, 0.0f, 0.0f, 48000.0f);
        check(band == 12000.0f && notch == 12000.0f && std::abs(bandClosed - 170.0f) < 0.01f, "band pass and notch: centred on the cutoff, 12 kHz at the most");
        // Follow at 0: the wheel is a plain cutoff (the level does not move it); Sweep moves it three octaves each way.
        SweepFilter::Settings plain;
        plain.follow = 0.0f;
        const bool still = SweepFilter::cutoffHz(plain, 0.5f, 0.0f, 0.0f, 48000.0f) == SweepFilter::cutoffHz(plain, 0.5f, 1.0f, 0.0f, 48000.0f);
        plain.sweep = 1.0f;
        const float up = SweepFilter::octaves(plain, 1.0f, 0.0f, 1.0f) - SweepFilter::octaves(plain, 1.0f, 0.0f, 0.0f);
        check(still && std::abs(up - 3.0f) < 1.0e-5f, "follow 0%: the level does not move it; sweep 100%: three octaves either way");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : std::to_string(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

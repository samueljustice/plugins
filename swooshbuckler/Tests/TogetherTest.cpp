// Faders Together test (filled in by the engine's work package): the cycle clock, the passes and the aimed hits on
// its peaks, Stagger, Hit Every, the make-up, a change of mode, and Faders Apart left as it was; Auto Swell.
//
//   SwooshbucklerTogether      prints what it measures and ends with "ALL PASSED" (exit code 0) or "n FAILED".

#include <JuceHeader.h>
#include "RigEngine.h"
#include "Parameters.h"

namespace
{
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    std::cout << "Faders Together" << std::endl;
    {
        const RigEngine::Params p;
        check(p.rideMode == RigEngine::RideApart && p.autoSwell.mode == RigEngine::AutoSwellOff
                  && p.swirlDetail[0].type == RigEngine::SwirlBoth && p.swirlDetail[1].type == RigEngine::SwirlBoth,
              "defaults: Faders Apart, no Auto Swell, Swirl as it was");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}

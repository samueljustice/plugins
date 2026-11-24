#pragma once

#include "ProToolsDriver.h"
#include "core/Types.h"

#include <functional>
#include <string>
#include <vector>

// A test of running Pro Tools' Event > Identify Beat from outside, one careful step at a time.
// It stops at the first step that does not go as expected and writes everything it sees to a log
// that can be sent back. Plain C++; it blocks, so it is run on a background thread.
namespace tt::pt
{

enum class TestMode
{
    lookOnly,   // find everything, open the dialog, write down what it contains, cancel it; no key is sent
    onePin,     // write the first pin
    allPins     // write every pin, left to right
};

struct TestSettings
{
    TestMode mode = TestMode::lookOnly;

    std::vector<Pin> pins;              // sorted by time
    int beatsPerBar = 4;
    double sessionSampleRate = 0.0;     // converts pin times to samples; must match the session

    // Where the music file starts in the session, in samples. Without it the Main Counter is read
    // at the start of the run: the cursor is then expected to be at the start of the music clip.
    bool hasMusicStart = false;
    long long musicStartSample = 0;

    bool typeLocation = false;          // type the bar|beat into the dialog instead of setting the field
    std::string appVersion;
    std::string musicName;

    // The application to drive. Another identifier is for trying the first steps without Pro Tools.
    std::string bundleIdentifier = Driver::defaultBundleIdentifier;
};

struct TestOutcome
{
    bool ok = false;
    int written = 0;            // Identify Beat steps confirmed with OK, the dialog seen to close
    int unsure = 0;             // steps where OK was pressed and the run then stopped: maybe written
    std::string summary;        // one line for the top of the panel
};

// `log` receives one line at a time, each starting with the time since the start of the run.
// `cancelled` is asked between steps; once it returns true the run ends.
TestOutcome runIdentifyBeatTest (const TestSettings& settings,
                                 const std::function<void (const std::string&)>& log,
                                 const std::function<bool()>& cancelled);

} // namespace tt::pt

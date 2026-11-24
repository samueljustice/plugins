#pragma once

#include "ProToolsDriver.h"
#include "PtslSession.h"
#include "core/Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Writes a map into Pro Tools: Event > Identify Beat once for each pin, left to right, at the
// driver's quick pace. It stops at the first step that does not go as expected, and writes what
// it does to a log in the same form as the test's. Plain C++; every function here blocks, so it
// is run on a background thread.
//
// When Pro Tools' scripting link answers, the run reads the session through it, stops the
// transport, sets the Main Counter to Samples and links the timeline and edit selections, moves
// the cursor to each pin, checks the pins afterwards and reads the tempo of the rest of the
// session before and after, and puts the counter and the edit options back at the end. Identify
// Beat itself has no scripting command, so it is always run through Accessibility and keys.
// Without the link every step is done with key presses.
namespace tt::pt
{

struct WriteSettings
{
    std::vector<Pin> pins;              // sorted by time
    int beatsPerBar = 4;
    double sessionSampleRate = 0.0;     // converts pin times to samples; must match the session
    bool rateTyped = false;             // the rate was typed under More: the session's own rate is not used in its place

    // Where the music file starts in the session, in samples. Without it the start is read through
    // the scripting link (the selected clip), or else from the Main Counter at the start of the
    // run: the cursor is then expected to be at the start of the music clip.
    bool hasMusicStart = false;
    long long musicStartSample = 0;

    bool useScriptingLink = true;       // false: keys only, as without the link
    MusicFacts music;                   // for the scripting link's reading of the selected clip

    bool checkAfterwards = true;        // read every pin's location back once all are written
    bool slowKeys = false;              // hold each key as long as the test does

    // The music start of the run before this one, to say so in the log when it is the same.
    bool hasLastStart = false;
    long long lastStartSample = 0;

    std::string appVersion;
    std::string musicName;

    // The application to drive. Another identifier is for trying the first steps without Pro Tools.
    std::string bundleIdentifier = Driver::defaultBundleIdentifier;
};

// A pin whose location in Pro Tools read differently from the pin's, or could not be read.
struct PinCheck
{
    int pin = 0;                // counted from 1
    std::string wanted;         // "46|1|000"
    std::string found;          // as the dialog showed it, or the sample the scripting link gave
    long long ticksOff = 0;     // found minus wanted, 960 ticks to the beat (a check with the dialog)
    bool inSamples = false;     // a check through the scripting link: samplesOff is used
    long long samplesOff = 0;   // where the bar|beat is minus the pin's sample
};

struct WriteOutcome
{
    bool ok = false;            // every pin was written
    int total = 0;
    int written = 0;            // Identify Beat steps confirmed with OK, the dialog seen to close
    int failedPin = 0;          // the pin the run stopped at, counted from 1. 0: before the first pin, or none
    bool okPressedOnFailedPin = false;   // OK was pressed for failedPin before its step failed: maybe written
    std::string reason;         // why it stopped, one sentence without a full stop
    std::string summary;        // "Written: 24 pins" or "Stopped at pin 7 of 24: <reason>"

    bool hasMusicStart = false; // the start of the music was known when the run ended
    long long musicStart = 0;

    bool checked = false;       // the check ran to the end
    int checkedPins = 0;
    std::vector<PinCheck> off;  // the pins more than 2 ticks (or 2 samples) from where they should be
    std::string checkSummary;   // empty when no check was asked for or the run stopped before it

    // The scripting link.
    bool linked = false;        // it was used for this run
    std::string linkNote;       // why it was not used, or what it did not do
    bool movedWithLink = false; // the cursor was moved to the pins through the link
    std::string moveNote;       // "Identify Beat saw the cursor moves ...", or why the keys were used instead
    std::string restSummary;    // "Rest of the session: tempo unchanged" ...
    bool restChanged = false;
    std::string beforeSummary;  // what happened to the tempo before the music, when it changed
    std::string restoredNote;   // what was put back at the end, or what could not be
    bool restoreFailed = false; // something could not be put back: restoredNote says what to set by hand
};

struct UndoOutcome
{
    bool ok = false;
    int sent = 0;
    std::string summary;        // "Sent 24 undos" or "Stopped after 7 of 24 undos: <reason>"
};

enum class Phase { starting, writing, checking, undoing };

// `done` of `total` are finished; the one after is under way.
using Progress = std::function<void (Phase, int done, int total)>;
using LogLine = std::function<void (const std::string&)>;
using Cancelled = std::function<bool()>;

// `log` receives one line at a time, each starting with the time since the start of the run.
// `cancelled` is asked between steps; once it returns true the run ends.
WriteOutcome runWrite (const WriteSettings& settings, const LogLine& log, const Cancelled& cancelled,
                       const Progress& progress);

// Brings the application forward and presses Edit > Undo `steps` times. What Pro Tools did with them
// cannot be read from outside.
UndoOutcome runUndo (const std::string& bundleIdentifier, int steps, const std::string& appVersion,
                     const LogLine& log, const Cancelled& cancelled, const Progress& progress);

// What the Main Counter shows, read without bringing the application forward or pressing anything.
struct CounterReading
{
    bool running = false;
    bool read = false;          // the counter was found and gave a value
    bool isSamples = false;
    long long sample = 0;
    std::string text;           // the counter as shown
};

// Reads the Main Counter again and again with one driver, which keeps the counter once found.
class CounterReader
{
public:
    CounterReader (std::string bundleIdentifier, Cancelled cancelled);
    ~CounterReader();

    CounterReading read();

private:
    std::string bundleIdentifier;
    Cancelled cancelled;
    std::unique_ptr<Driver> driver;
    int failures = 0;       // reads in a row that gave nothing
};

// The bar a pin is in, counted from 1.
int barOf (const Pin& pin, int beatsPerBar);

// "  46| 1| 000" into ticks from bar 1 beat 1, 960 ticks to the beat. False when it is not bar|beat|ticks.
bool ticksOfLocation (const std::string& location, int beatsPerBar, long long& ticks);

} // namespace tt::pt

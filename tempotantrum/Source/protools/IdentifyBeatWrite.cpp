#include "IdentifyBeatWrite.h"

#include "core/Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace tt::pt
{

namespace
{
    constexpr long long ticksPerBeat = 960;
    constexpr long long ticksAllowed = 2;

    std::string dateAndTime()
    {
        const std::time_t now = std::time (nullptr);
        std::tm local {};
        localtime_r (&now, &local);
        char text[64];
        std::strftime (text, sizeof (text), "%Y-%m-%d %H:%M:%S %z", &local);
        return text;
    }

    std::string seconds (double value, int decimals = 3)
    {
        char text[48];
        std::snprintf (text, sizeof (text), "%.*f", decimals, value);
        return text;
    }

    std::string pins (int count)
    {
        return std::to_string (count) + (count == 1 ? " pin" : " pins");
    }

    std::string undos (int count)
    {
        return std::to_string (count) + (count == 1 ? " undo" : " undos");
    }

    bool allDigits (const std::string& text)
    {
        return ! text.empty() && std::all_of (text.begin(), text.end(), [] (char c) { return c >= '0' && c <= '9'; });
    }

    // A sample count as a counter shows it, without spaces or thousands separators.
    std::string counterDigits (std::string text)
    {
        for (size_t at = 0; (at = text.find ("\xc2\xa0", at)) != std::string::npos;)
            text.erase (at, 2);

        text.erase (std::remove_if (text.begin(), text.end(), [] (char c) { return c == ' ' || c == ',' || c == '\t'; }), text.end());
        return text;
    }

    // "  46| 1| 000" into ticks from bar 1 beat 1. False when it is not bar|beat|ticks.
    bool ticksOf (const std::string& location, int beatsPerBar, long long& ticks)
    {
        long long parts[3] = {};
        size_t start = 0;

        for (int part = 0; part < 3; ++part)
        {
            const size_t bar = location.find ('|', start);

            if ((part < 2) == (bar == std::string::npos))
                return false;

            auto piece = location.substr (start, bar == std::string::npos ? std::string::npos : bar - start);
            piece.erase (std::remove_if (piece.begin(), piece.end(), [] (char c) { return c == ' ' || c == '\t'; }), piece.end());

            if (! allDigits (piece) || piece.size() > 9)
                return false;

            parts[part] = std::stoll (piece);
            start = bar + 1;
        }

        ticks = ((parts[0] - 1) * std::max (1, beatsPerBar) + (parts[1] - 1)) * ticksPerBeat + parts[2];
        return true;
    }

    // Stamps every line with the time since the start of the run, in seconds, and writes two
    // empty lines in a row as one.
    class Stamped
    {
    public:
        explicit Stamped (const LogLine& target) : log (target) {}

        void operator() (const std::string& text)
        {
            if (! log || (text.empty() && lastWasEmpty))
                return;

            lastWasEmpty = text.empty();

            char stamp[32];
            std::snprintf (stamp, sizeof (stamp), "[%8.3f] ", elapsed());

            size_t from = 0;

            while (from <= text.size())
            {
                const size_t end = text.find ('\n', from);
                log (stamp + text.substr (from, end == std::string::npos ? std::string::npos : end - from));

                if (end == std::string::npos)
                    break;

                from = end + 1;
            }
        }

        double elapsed() const
        {
            return std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        }

    private:
        const LogLine& log;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        bool lastWasEmpty = false;
    };

    const char* const permissionSentence = "macOS has not given Tempo Tantrum permission to control other apps. Switch it on in "
                                           "System Settings > Privacy & Security > Accessibility, and reopen the app";
}

int barOf (const Pin& pin, int beatsPerBar)
{
    const long long perBar = ticksPerBeat * std::max (1, beatsPerBar);
    const long long ticks = std::llround (pin.beat * (double) ticksPerBeat);
    return (int) ((ticks >= 0 ? ticks / perBar : -((-ticks + perBar - 1) / perBar)) + 1);
}

//==============================================================================
WriteOutcome runWrite (const WriteSettings& settings, const LogLine& log, const Cancelled& cancelled,
                       const Progress& progress)
{
    Stamped say (log);

    const int total = (int) settings.pins.size();
    const auto offsetOf = [&settings] (const Pin& pin) { return (long long) std::llround (pin.time * settings.sessionSampleRate); };
    const auto barBeatOf = [&settings] (const Pin& pin) { return formatBarBeat (pin.beat, settings.beatsPerBar); };
    const auto tell = [&progress] (Phase phase, int done, int of)
    {
        if (progress)
            progress (phase, done, of);
    };

    WriteOutcome outcome;
    outcome.total = total;

    // Ends the run: the summary, and what was written to the session.
    const auto finish = [&] () -> WriteOutcome
    {
        outcome.ok = outcome.reason.empty();

        if (outcome.ok)
            outcome.summary = "Written: " + pins (outcome.written);
        else if (outcome.failedPin > 0)
            outcome.summary = "Stopped at pin " + std::to_string (outcome.failedPin) + " of " + std::to_string (total) + ": " + outcome.reason;
        else
            outcome.summary = "Nothing was written: " + outcome.reason;

        say ("");
        say (outcome.summary);

        if (outcome.written > 0)
            say (std::to_string (outcome.written) + (outcome.written == 1 ? " Identify Beat step was written." : " Identify Beat steps were written.")
                 + " Cmd+Z in Pro Tools undoes them one at a time.");
        else if (! outcome.okPressedOnFailedPin)
            say ("No Identify Beat step was written.");

        if (outcome.okPressedOnFailedPin)
            say ("OK was pressed for pin " + std::to_string (outcome.failedPin) + " before the run stopped, so that step may have been "
                 "written as well. Check Pro Tools.");

        if (! outcome.checkSummary.empty())
            say (outcome.checkSummary);

        return outcome;
    };

    // Set when an Identify Beat dialog could not be cancelled; it goes into the reason the run stopped.
    std::string dialogNote;

    const auto stop = [&] (const std::string& reason, int pin = 0) -> WriteOutcome
    {
        outcome.reason = dialogNote.empty() ? reason : reason + ". " + dialogNote;
        outcome.failedPin = pin;
        return finish();
    };

    //==============================================================================
    say ("Tempo Tantrum " + settings.appVersion + ", write to Pro Tools with Identify Beat");
    say ("macOS " + Driver::systemVersion());
    say ("Date: " + dateAndTime());
    say ("Music: " + (settings.musicName.empty() ? std::string ("none open") : settings.musicName));
    say (std::string ("Accessibility permission: ") + (Driver::accessibilityAllowed() ? "allowed" : "missing")
         + ". Posting key events: " + (Driver::keyEventsAllowed() ? "allowed" : "not allowed"));

    Driver driver ([&say] (const std::string& text) { say (text); }, cancelled);
    driver.setBundleIdentifier (settings.bundleIdentifier);
    driver.setQuick (true, settings.slowKeys);

    const auto cancelOrNote = [&] (const Element& dialog)
    {
        const auto closed = driver.cancelDialog (dialog);

        if (! closed.ok)
        {
            say ("The Identify Beat dialog may still be open in Pro Tools (" + closed.message + "). Cancel it by hand.");
            dialogNote = "The Identify Beat dialog may still be open in Pro Tools: press its Cancel button";
        }

        return closed;
    };

    if (settings.bundleIdentifier != Driver::defaultBundleIdentifier)
        say ("Looking for the application " + settings.bundleIdentifier + " in place of Pro Tools");

    const auto app = driver.findProTools();

    if (app.running)
        say ("Pro Tools version: " + (app.version.empty() ? std::string ("not given by its bundle") : app.version));

    {
        char rate[48];
        std::snprintf (rate, sizeof (rate), "%.10g", settings.sessionSampleRate);
        say (std::string ("Session sample rate used for the pins: ") + rate);
        say (settings.hasMusicStart ? "Music starts at sample " + std::to_string (settings.musicStartSample) + " (typed in the sheet)"
                                    : std::string ("Music start: read from the Main Counter at the start of the run"));
        say (std::string ("Keys: ") + (settings.slowKeys ? "typed slowly" : "typed at the quick pace"));
        say (std::string ("Check each pin afterwards: ") + (settings.checkAfterwards ? "yes" : "no"));
        say ("Pins to write: " + std::to_string (total));

        for (int i = 0; i < total; ++i)
        {
            const auto& pin = settings.pins[(size_t) i];
            char line[160];
            std::snprintf (line, sizeof (line), "  %3d  %-12s %10s s   music start + %lld samples", i + 1,
                           barBeatOf (pin).c_str(), seconds (pin.time).c_str(), offsetOf (pin));
            say (line);
        }
    }

    say ("");
    tell (Phase::starting, 0, total);

    //==============================================================================
    // Everything that can be checked before Pro Tools is touched.
    if (! app.running)
        return stop ("Pro Tools is not running");

    if (! Driver::accessibilityAllowed())
        return stop (permissionSentence);

    if (total == 0)
        return stop ("there are no pins to write");

    if (! (settings.sessionSampleRate > 0.0))
        return stop ("the session sample rate is not set");

    for (int i = 0; i < total; ++i)
    {
        const auto barBeat = barBeatOf (settings.pins[(size_t) i]);

        if (barBeat.empty() || barBeat[0] == '-' || barBeat[0] == '0')
            return stop ("pin " + std::to_string (i + 1) + " is at " + barBeat + ", before bar 1 beat 1, and Identify Beat takes "
                         "only locations from bar 1 on");
    }

    // The first pin that would land before the start of the session, counted from 1. 0: none.
    const auto pinBeforeSessionStart = [&] (long long musicStart)
    {
        for (int i = 0; i < total; ++i)
            if (musicStart + offsetOf (settings.pins[(size_t) i]) < 0)
                return i + 1;

        return 0;
    };

    const auto beforeSessionStart = [&] (int pinNumber, long long musicStart)
    {
        return "pin " + std::to_string (pinNumber) + " would be at sample "
               + std::to_string (musicStart + offsetOf (settings.pins[(size_t) (pinNumber - 1)]))
               + ", before the start of the session";
    };

    if (settings.hasMusicStart)
        if (const int bad = pinBeforeSessionStart (settings.musicStartSample); bad > 0)
            return stop (beforeSessionStart (bad, settings.musicStartSample));

    say ("Step: bring Pro Tools to the front");

    if (const auto r = driver.bringToFront(); ! r.ok)
        return stop (r.message);

    say ("Step: look at the windows");

    if (const auto r = driver.checkWindowsAtStart(); ! r.ok)
        return stop (r.message);

    say ("Step: find the menu item Event > Identify Beat...");
    Element menuItem;
    bool menuEnabled = false;

    if (const auto r = driver.findMenuItem ({ "Event", "Identify Beat..." }, menuItem, menuEnabled); ! r.ok)
        return stop (r.message);

    if (! menuEnabled)
        return stop ("the Identify Beat menu item is switched off. Open a session and stop the transport");

    // The sample of every pin is typed into the Main Counter and read back from it, so the
    // counter has to be found and has to show samples before a key is sent.
    say ("Step: read the Main Counter");
    std::string counter;

    if (const auto r = driver.readMainCounter (counter); ! r.ok)
        return stop (r.message + ". No key was sent");

    const auto counterAsDigits = counterDigits (counter);

    if (! allDigits (counterAsDigits) || counterAsDigits.size() > 15)
        return stop ("the Main Counter is not set to Samples (it shows \"" + counter + "\"). Set it to Samples and try again. "
                     "No key was sent");

    say ("The Main Counter reads as samples: " + counterAsDigits);

    long long musicStart = settings.musicStartSample;

    if (! settings.hasMusicStart)
    {
        musicStart = std::stoll (counterAsDigits);
        say ("");
        say ("MUSIC START: sample " + std::to_string (musicStart) + ", read from the Main Counter. Every pin is placed from this sample.");

        if (const int bad = pinBeforeSessionStart (musicStart); bad > 0)
            return stop (beforeSessionStart (bad, musicStart));
    }

    outcome.hasMusicStart = true;
    outcome.musicStart = musicStart;

    if (musicStart == 0)
        say ("Note: the music start is sample 0, the start of the session.");

    if (settings.hasLastStart && settings.lastStartSample == musicStart)
        say ("Note: the run before this one wrote from the same sample.");

    if (const auto r = driver.rememberWindows(); ! r.ok)
        return stop (r.message);

    //==============================================================================
    const double writeStart = say.elapsed();

    for (int i = 0; i < total; ++i)
    {
        const int number = i + 1;
        tell (Phase::writing, i, total);

        if (cancelled && cancelled())
            return stop ("the run was stopped", number);

        const auto& pin = settings.pins[(size_t) i];
        const long long sample = musicStart + offsetOf (pin);
        const auto barBeat = barBeatOf (pin);
        const double pinStart = say.elapsed();

        say ("");
        say ("Pin " + std::to_string (number) + " of " + std::to_string (total) + ": " + barBeat + " at sample " + std::to_string (sample)
             + " (" + seconds (pin.time) + " s into the music)");

        if (const auto r = driver.goToSample (sample); ! r.ok)
            return stop (r.message, number);

        Element dialog;

        if (const auto r = driver.openIdentifyBeat (menuItem, dialog); ! r.ok)
            return stop (r.message, number);

        // A location that does not read back as typed has already cancelled the dialog.
        if (const auto r = driver.setIdentifyBeatLocation (dialog, barBeat, true); ! r.ok || ! r.confirmed)
        {
            cancelOrNote (dialog);
            return stop (r.ok ? "the bar|beat in the Identify Beat dialog could not be read back, so OK was not pressed" : r.message, number);
        }

        bool okPressed = false;

        // A failure here may have left an alert up. Nothing is pressed in it.
        if (const auto r = driver.confirmIdentifyBeat (dialog, okPressed); ! r.ok)
        {
            outcome.okPressedOnFailedPin = okPressed;

            if (! okPressed)   // the dialog is still up with the location typed in it
                cancelOrNote (dialog);

            return stop (r.message, number);
        }

        ++outcome.written;
        say ("Pin " + std::to_string (number) + " written: " + barBeat + " at sample " + std::to_string (sample)
             + ", " + seconds (say.elapsed() - pinStart, 2) + " s");
    }

    tell (Phase::writing, total, total);

    {
        const double took = say.elapsed() - writeStart;
        say ("");
        say (pins (total) + " in " + seconds (took, 1) + " s, " + seconds (took / (double) total, 2) + " s for each");
    }

    //==============================================================================
    // The check: at each pin the Identify Beat dialog is opened, the location it suggests is
    // read, and the dialog is cancelled. OK is never pressed, so nothing is changed.
    if (settings.checkAfterwards)
    {
        say ("");
        say ("Check: at each pin, open Identify Beat, read the location it suggests and cancel");

        const auto checkStopped = [&] (int number, const std::string& reason)
        {
            outcome.checkSummary = "The check stopped at pin " + std::to_string (number) + " of " + std::to_string (total) + ": " + reason
                                   + ". The check presses no OK, so it changed nothing";
        };

        bool complete = true;

        for (int i = 0; i < total && complete; ++i)
        {
            const int number = i + 1;
            tell (Phase::checking, i, total);

            if (cancelled && cancelled())
            {
                checkStopped (number, "the run was stopped");
                complete = false;
                break;
            }

            const auto& pin = settings.pins[(size_t) i];
            const auto barBeat = barBeatOf (pin);
            Element dialog;
            std::string shown;
            auto step = driver.goToSample (musicStart + offsetOf (pin));

            if (step.ok)
                step = driver.openIdentifyBeat (menuItem, dialog);

            if (step.ok)
            {
                step = driver.readIdentifyBeatLocation (dialog, shown);

                if (const auto closed = cancelOrNote (dialog); ! closed.ok && step.ok)
                    step = closed;
            }

            if (! step.ok)
            {
                checkStopped (number, step.message);
                complete = false;
                break;
            }

            const long long wanted = std::llround (pin.beat * (double) ticksPerBeat);
            long long found = 0;
            ++outcome.checkedPins;

            if (! ticksOf (shown, settings.beatsPerBar, found))
            {
                checkStopped (number, "the dialog showed \"" + shown + "\", which is not bar|beat|ticks");
                complete = false;
                break;
            }

            const long long diff = found - wanted;
            say ("Check pin " + std::to_string (number) + ": wanted " + barBeat + ", Pro Tools shows \"" + shown + "\""
                 + (diff == 0 ? std::string() : ", " + std::to_string (std::llabs (diff)) + (std::llabs (diff) == 1 ? " tick " : " ticks ")
                                                + (diff > 0 ? "late" : "early"))
                 + (std::llabs (diff) <= ticksAllowed ? "" : "  NOT WITHIN 2 TICKS"));

            if (std::llabs (diff) > ticksAllowed)
                outcome.off.push_back ({ number, barBeat, shown, diff });
        }

        if (complete)
        {
            tell (Phase::checking, total, total);
            outcome.checked = true;

            if (outcome.off.empty())
            {
                outcome.checkSummary = total == 1 ? std::string ("The pin checks out") : "All " + pins (total) + " check out";
            }
            else
            {
                outcome.checkSummary = std::to_string (outcome.off.size()) + " of " + pins (total)
                                       + (outcome.off.size() == 1 ? " does not check out" : " do not check out");

                // The check reads what Identify Beat suggests at each pin. If that is not the
                // marker's own position, most pins read as off although they are in place.
                if ((int) outcome.off.size() * 2 >= total)
                    outcome.checkSummary += ". That is half of them or more: look at the markers in Pro Tools. If they are in "
                                            "place, the location Identify Beat suggests is not the marker's own position and "
                                            "this check cannot be relied on";
            }
        }
    }

    // Back to the start of the music, so the cursor is where a run expects it.
    if (! (cancelled && cancelled()))
    {
        say ("");
        say ("Step: move the cursor back to the start of the music");

        if (const auto r = driver.goToSample (musicStart); ! r.ok)
            say ("The cursor was not moved back: " + r.message);
    }

    return finish();
}

//==============================================================================
UndoOutcome runUndo (const std::string& bundleIdentifier, int steps, const std::string& appVersion,
                     const LogLine& log, const Cancelled& cancelled, const Progress& progress)
{
    Stamped say (log);
    UndoOutcome outcome;

    const auto finish = [&] (const std::string& reason) -> UndoOutcome
    {
        outcome.ok = reason.empty();
        outcome.summary = outcome.ok ? "Sent " + undos (outcome.sent)
                                     : "Stopped after " + std::to_string (outcome.sent) + " of " + undos (steps) + ": " + reason;
        say ("");
        say (outcome.summary);
        say ("What Pro Tools did with them cannot be read from outside. Look at the session.");
        return outcome;
    };

    say ("Tempo Tantrum " + appVersion + ", undo in Pro Tools");
    say ("Date: " + dateAndTime());
    say ("Undo steps to press: " + std::to_string (steps));
    say ("");

    if (progress)
        progress (Phase::undoing, 0, steps);

    Driver driver ([&say] (const std::string& text) { say (text); }, cancelled);
    driver.setBundleIdentifier (bundleIdentifier);
    driver.setQuick (true, true);

    if (! driver.findProTools().running)
        return finish ("Pro Tools is not running");

    if (! Driver::accessibilityAllowed())
        return finish (permissionSentence);

    if (const auto r = driver.bringToFront(); ! r.ok)
        return finish (r.message);

    if (const auto r = driver.checkWindowsAtStart(); ! r.ok)
        return finish (r.message);

    if (const auto r = driver.rememberWindows(); ! r.ok)
        return finish (r.message);

    for (int i = 0; i < steps; ++i)
    {
        if (progress)
            progress (Phase::undoing, i, steps);

        if (const auto r = driver.sendUndo(); ! r.ok)
            return finish (r.message);

        ++outcome.sent;

        // Time for Pro Tools to redraw before the next one.
        if (const auto r = driver.pause (150); ! r.ok)
            return finish (r.message);
    }

    if (progress)
        progress (Phase::undoing, steps, steps);

    return finish ({});
}

//==============================================================================
CounterReader::CounterReader (std::string identifier, Cancelled isCancelled)
    : bundleIdentifier (std::move (identifier)), cancelled (std::move (isCancelled))
{
}

CounterReader::~CounterReader() = default;

CounterReading CounterReader::read()
{
    CounterReading reading;

    if (! Driver::isRunning (bundleIdentifier))
    {
        driver = nullptr;
        return reading;
    }

    reading.running = true;

    if (! Driver::accessibilityAllowed())
        return reading;

    if (driver == nullptr)
    {
        driver = std::make_unique<Driver> (Driver::Log(), cancelled);
        driver->setBundleIdentifier (bundleIdentifier);
        driver->findProTools();
        failures = 0;
    }

    if (driver->readMainCounter (reading.text).ok)
    {
        failures = 0;
        reading.read = true;
        const auto digits = counterDigits (reading.text);

        if (allDigits (digits) && digits.size() <= 15)
        {
            reading.isSamples = true;
            reading.sample = std::stoll (digits);
        }
    }
    else if (++failures >= 12)
    {
        // The counter was not found, or Pro Tools has been restarted: look afresh.
        driver = nullptr;
    }

    return reading;
}

} // namespace tt::pt

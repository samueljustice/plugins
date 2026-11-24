#include "IdentifyBeatTest.h"

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
    const char* modeName (TestMode mode)
    {
        switch (mode)
        {
            case TestMode::lookOnly: return "Look only";
            case TestMode::onePin:   return "Send one pin";
            case TestMode::allPins:  return "Send all pins";
        }

        return "";
    }

    std::string dateAndTime()
    {
        const std::time_t now = std::time (nullptr);
        std::tm local {};
        localtime_r (&now, &local);
        char text[64];
        std::strftime (text, sizeof (text), "%Y-%m-%d %H:%M:%S %z", &local);
        return text;
    }

    std::string seconds (double value)
    {
        char text[48];
        std::snprintf (text, sizeof (text), "%.3f", value);
        return text;
    }

    std::string pins (int count)
    {
        return std::to_string (count) + (count == 1 ? " pin" : " pins");
    }

    bool allDigits (const std::string& text)
    {
        return ! text.empty() && std::all_of (text.begin(), text.end(), [] (char c) { return c >= '0' && c <= '9'; });
    }

    // A sample count as a counter shows it, without spaces or thousands separators.
    std::string counterDigits (std::string text)
    {
        text.erase (std::remove_if (text.begin(), text.end(), [] (char c) { return c == ' ' || c == ',' || c == '\t'; }), text.end());
        return text;
    }
}

TestOutcome runIdentifyBeatTest (const TestSettings& settings,
                                 const std::function<void (const std::string&)>& log,
                                 const std::function<bool()>& cancelled)
{
    const auto start = std::chrono::steady_clock::now();

    // Every line gets the time since the start of the run, in seconds.
    // Two empty lines in a row are written as one.
    bool lastWasEmpty = false;

    const auto say = [&log, &lastWasEmpty, start] (const std::string& text)
    {
        if (! log || (text.empty() && lastWasEmpty))
            return;

        lastWasEmpty = text.empty();

        const double elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        char stamp[32];
        std::snprintf (stamp, sizeof (stamp), "[%8.3f] ", elapsed);

        size_t from = 0;

        while (from <= text.size())
        {
            const size_t end = text.find ('\n', from);
            log (stamp + text.substr (from, end == std::string::npos ? std::string::npos : end - from));

            if (end == std::string::npos)
                break;

            from = end + 1;
        }
    };

    const bool sending = settings.mode != TestMode::lookOnly;
    std::vector<Pin> toSend;

    if (settings.mode == TestMode::onePin && ! settings.pins.empty())
        toSend.push_back (settings.pins.front());
    else if (settings.mode == TestMode::allPins)
        toSend = settings.pins;

    const int total = (int) toSend.size();
    const auto offsetOf = [&settings] (const Pin& pin) { return std::llround (pin.time * settings.sessionSampleRate); };
    const auto barBeatOf = [&settings] (const Pin& pin) { return formatBarBeat (pin.beat, settings.beatsPerBar); };

    TestOutcome outcome;
    int notReadBack = 0;
    int pinWithOkPressed = 0;   // the pin whose OK was pressed before its step failed, counted from 1

    // Ends the run: the summary, and what was written to the session.
    const auto finish = [&] (bool ok, const std::string& message)
    {
        outcome.ok = ok;

        if (ok)
        {
            outcome.summary = "Worked: " + message;
        }
        else
        {
            outcome.summary = "Stopped: " + message;

            if (sending && total > 0)
                outcome.summary += ". " + std::to_string (outcome.written) + " of " + pins (total)
                                   + (total == 1 ? " was written" : " were written");

            if (pinWithOkPressed > 0)
                outcome.summary += ". OK was pressed for pin " + std::to_string (pinWithOkPressed)
                                   + ", so it may have been written: check Pro Tools";
        }

        outcome.unsure = pinWithOkPressed > 0 ? 1 : 0;

        say ("");
        say (outcome.summary);

        if (outcome.written > 0)
            say (std::to_string (outcome.written) + (outcome.written == 1 ? " Identify Beat step was written." : " Identify Beat steps were written.")
                 + " Cmd+Z in Pro Tools should undo them one at a time.");

        if (pinWithOkPressed > 0)
            say ("OK was pressed for pin " + std::to_string (pinWithOkPressed) + " before the run stopped, so that step may have been "
                 "written as well. Check Pro Tools. Cmd+Z should undo it.");
        else if (outcome.written == 0)
            say ("No Identify Beat step was written.");

        return outcome;
    };

    //==============================================================================
    say ("Tempo Tantrum " + settings.appVersion + ", test of Identify Beat in Pro Tools");
    say ("macOS " + Driver::systemVersion());
    say ("Date: " + dateAndTime());
    say (std::string ("Mode: ") + modeName (settings.mode));
    say ("Music: " + (settings.musicName.empty() ? std::string ("none open") : settings.musicName));
    say (std::string ("Accessibility permission: ") + (Driver::accessibilityAllowed() ? "allowed" : "missing")
         + ". Posting key events: " + (Driver::keyEventsAllowed() ? "allowed" : "not allowed"));

    Driver driver (say, cancelled);
    driver.setBundleIdentifier (settings.bundleIdentifier);

    if (settings.bundleIdentifier != Driver::defaultBundleIdentifier)
        say ("Looking for the application " + settings.bundleIdentifier + " in place of Pro Tools");

    const auto app = driver.findProTools();

    if (app.running)
        say ("Pro Tools version: " + (app.version.empty() ? std::string ("not given by its bundle") : app.version));

    if (sending)
    {
        char rate[48];
        std::snprintf (rate, sizeof (rate), "%.10g", settings.sessionSampleRate);
        say (std::string ("Session sample rate used for the pins: ") + rate);
        say (settings.hasMusicStart ? "Music starts at sample " + std::to_string (settings.musicStartSample) + " (typed in the panel)"
                                    : std::string ("Music start: read from the Main Counter at the start of the run"));
        say (settings.typeLocation ? "Bar|beat entry: typed into the dialog"
                                   : "Bar|beat entry: set the field's value, and type it if that does not work");
        say ("Pins to send: " + std::to_string (total));

        for (int i = 0; i < total; ++i)
        {
            const auto& pin = toSend[(size_t) i];
            char line[160];
            std::snprintf (line, sizeof (line), "  %3d  %-12s %10s s   music start + %lld samples", i + 1,
                           barBeatOf (pin).c_str(), seconds (pin.time).c_str(), (long long) offsetOf (pin));
            say (line);
        }
    }
    else
    {
        say ("Pins to send: none. Look only writes nothing to the session.");
    }

    say ("");

    //==============================================================================
    if (! app.running)
        return finish (false, "Pro Tools is not running");

    if (! Driver::accessibilityAllowed())
        return finish (false, "macOS has not given Tempo Tantrum permission to control other apps. Switch it on in "
                              "System Settings > Privacy & Security > Accessibility, and reopen the app");

    if (sending && total == 0)
        return finish (false, "there are no pins to send");

    if (sending && ! (settings.sessionSampleRate > 0.0))
        return finish (false, "the session sample rate is not set");

    // Every pin is checked before Pro Tools is touched, so a run does not stop half way on a
    // pin that could never be sent.
    for (int i = 0; i < total; ++i)
    {
        const auto barBeat = barBeatOf (toSend[(size_t) i]);

        if (barBeat.empty() || barBeat[0] == '-' || barBeat[0] == '0')
            return finish (false, "pin " + std::to_string (i + 1) + " is at " + barBeat + ", before bar 1 beat 1. "
                                  "This test sends only pins from bar 1 on");
    }

    // The first pin that would land before the start of the session, counted from 1. 0: none.
    const auto pinBeforeSessionStart = [&] (long long musicStart)
    {
        for (int i = 0; i < total; ++i)
            if (musicStart + (long long) offsetOf (toSend[(size_t) i]) < 0)
                return i + 1;

        return 0;
    };

    const auto beforeSessionStart = [&] (int pinNumber, long long musicStart)
    {
        return "pin " + std::to_string (pinNumber) + " would be at sample "
               + std::to_string (musicStart + (long long) offsetOf (toSend[(size_t) (pinNumber - 1)]))
               + ", before the start of the session";
    };

    if (sending && settings.hasMusicStart)
        if (const int bad = pinBeforeSessionStart (settings.musicStartSample); bad > 0)
            return finish (false, beforeSessionStart (bad, settings.musicStartSample));

    say ("Step: bring Pro Tools to the front");

    if (const auto r = driver.bringToFront(); ! r.ok)
        return finish (false, r.message);

    driver.logWindows();

    say ("Step: find the menu item Event > Identify Beat...");
    Element menuItem;
    bool menuEnabled = false;

    if (const auto r = driver.findMenuItem ({ "Event", "Identify Beat..." }, menuItem, menuEnabled); ! r.ok)
        return finish (false, r.message);

    if (! menuEnabled)
        return finish (false, "the Identify Beat menu item was found but is switched off");

    say ("Step: read the Main Counter");
    std::string counter;
    const auto counterRead = driver.readMainCounter (counter);
    const auto counterAsDigits = counterDigits (counter);
    const bool counterIsSamples = counterRead.ok && allDigits (counterAsDigits) && counterAsDigits.size() <= 15;

    if (cancelled && cancelled())
        return finish (false, "the run was stopped");

    if (! counterRead.ok)
        say ("The Main Counter could not be read: " + counterRead.message);
    else if (counterIsSamples)
        say ("The Main Counter reads as samples: " + counterAsDigits);
    else
        say ("The Main Counter shows \"" + counter + "\", which is not a sample count. Set the Main Counter to Samples.");

    //==============================================================================
    // Look only sends no key: it opens the dialog from the menu, writes it down and cancels it.
    if (! sending)
    {
        say ("Step: open the Identify Beat dialog");
        Element dialog;

        if (const auto r = driver.openIdentifyBeat (menuItem, dialog); ! r.ok)
            return finish (false, r.message);

        say ("Step: cancel the dialog");

        if (const auto r = driver.cancelDialog (dialog); ! r.ok)
            return finish (false, r.message);

        return finish (true, "Pro Tools was found, the Identify Beat dialog opened and was cancelled. Nothing was written to the session");
    }

    //==============================================================================
    // The sample is typed into the Main Counter, so a counter that was read and shows anything
    // but a sample count ends the run before a key is sent. A counter that cannot be read at all
    // does not: the run goes on and says what it could not confirm.
    if (counterRead.ok && ! counterIsSamples)
        return finish (false, "the Main Counter is not set to Samples (it shows \"" + counter + "\"). No key was sent");

    long long musicStart = settings.musicStartSample;

    if (! settings.hasMusicStart)
    {
        if (! counterIsSamples)
            return finish (false, "the Main Counter could not be read as samples, so the start of the music is not known. "
                                  "Type it into Music starts at sample");

        musicStart = std::stoll (counterAsDigits);
        say ("");
        say ("MUSIC START: sample " + std::to_string (musicStart) + ", read from the Main Counter. Every pin is placed from this sample."
             + (musicStart == 0 ? " It is the start of the session: if the music clip is not there, stop and type its start "
                                  "into Music starts at sample." : ""));

        if (const int bad = pinBeforeSessionStart (musicStart); bad > 0)
            return finish (false, beforeSessionStart (bad, musicStart));
    }

    if (const auto r = driver.rememberWindows(); ! r.ok)
        return finish (false, r.message);

    for (int i = 0; i < total; ++i)
    {
        if (cancelled && cancelled())
            return finish (false, "the run was stopped");

        const auto& pin = toSend[(size_t) i];
        const long long sample = musicStart + (long long) offsetOf (pin);
        const auto barBeat = barBeatOf (pin);

        say ("");
        say ("Pin " + std::to_string (i + 1) + " of " + std::to_string (total) + ": " + barBeat + " at sample " + std::to_string (sample)
             + " (" + seconds (pin.time) + " s into the music)");

        say ("Step: move the cursor to sample " + std::to_string (sample));
        const auto moved = driver.goToSample (sample);

        if (! moved.ok)
            return finish (false, moved.message);

        say ("Step: open the Identify Beat dialog");
        Element dialog;

        if (const auto r = driver.openIdentifyBeat (menuItem, dialog); ! r.ok)
            return finish (false, r.message);

        say ("Step: enter " + barBeat);
        const auto entered = driver.setIdentifyBeatLocation (dialog, barBeat, settings.typeLocation);

        if (! entered.ok)
        {
            if (const auto closed = driver.cancelDialog (dialog); ! closed.ok)
                say ("The Identify Beat dialog may still be open in Pro Tools (" + closed.message + "). Cancel it by hand.");

            return finish (false, entered.message);
        }

        // A location that could not be read back is confirmed only for one pin, where the
        // tester looks at the marker. With all pins nobody checks each one, so the run ends.
        if (! entered.confirmed && settings.mode == TestMode::allPins)
        {
            if (const auto closed = driver.cancelDialog (dialog); ! closed.ok)
                say ("The Identify Beat dialog may still be open in Pro Tools (" + closed.message + "). Cancel it by hand.");

            return finish (false, "the bar|beat in the Identify Beat dialog could not be read back for pin " + std::to_string (i + 1)
                                  + ", so OK was not pressed. Send one pin presses OK without the read-back");
        }

        say ("Step: press OK");
        bool okPressed = false;

        // A failure here may have left an alert up. Nothing is pressed in it.
        if (const auto r = driver.confirmIdentifyBeat (dialog, okPressed); ! r.ok)
        {
            if (okPressed)
                pinWithOkPressed = i + 1;

            return finish (false, r.message);
        }

        ++outcome.written;

        if (! moved.confirmed || ! entered.confirmed)
            ++notReadBack;

        say ("Pin " + std::to_string (i + 1) + " written: " + barBeat + " at sample " + std::to_string (sample)
             + (moved.confirmed ? "" : ". The cursor position could not be confirmed")
             + (entered.confirmed ? "" : ". The bar|beat in the dialog could not be read back"));
    }

    std::string message = total == 1 ? std::string ("1 pin written and the dialog closed")
                                     : pins (total) + " written, and the dialog closed each time";

    if (notReadBack > 0)
        message += ". The cursor position or the bar|beat could not be read back for " + std::to_string (notReadBack)
                   + " of them, so check them in Pro Tools";

    return finish (true, message);
}

} // namespace tt::pt

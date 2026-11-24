#include "IdentifyBeatWrite.h"

#include "core/Engine.h"

#include <algorithm>
#include <cassert>
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
        long long bar = 0, beat = 0, tick = 0;

        if (! barsBeatsOf (location, bar, beat, tick))
            return false;

        ticks = ((bar - 1) * std::max (1, beatsPerBar) + (beat - 1)) * ticksPerBeat + tick;
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

bool ticksOfLocation (const std::string& location, int beatsPerBar, long long& ticks)
{
    return ticksOf (location, beatsPerBar, ticks);
}

namespace
{
    // The scripting link during a run. Pro Tools queues scripting commands while one of its modal
    // dialogs is open, so a command sent while the Identify Beat dialog is up would wait until it
    // closes and could hang the run. The run marks every dialog it opens here, and gets the client
    // only while none is open: in a debug build asking for it then is an assertion, and in any
    // build the answer is no client.
    class LinkGate
    {
    public:
        void use (ptsl::Client& linkedClient)   { client = &linkedClient; }
        bool linked() const                     { return client != nullptr; }

        ptsl::Client* get()
        {
            assert (! dialogOpen && "no scripting command may be sent while a Pro Tools dialog is open");
            return dialogOpen ? nullptr : client;
        }

        void dialogOpening()                    { dialogOpen = true; }
        void dialogClosed()                     { dialogOpen = false; }
        bool dialogMayBeOpen() const            { return dialogOpen; }

    private:
        ptsl::Client* client = nullptr;
        bool dialogOpen = false;
    };

    std::string counterName (const std::string& type)
    {
        if (type == "TLType_Samples")       return "Samples";
        if (type == "TLType_BarsBeats")     return "Bars|Beats";
        if (type == "TLType_MinSecs")       return "Min:Sec";
        if (type == "TLType_TimeCode")      return "Timecode";
        if (type == "TLType_FeetFrames")    return "Feet+Frames";
        if (type == "TLType_Seconds")       return "Seconds";
        if (type == "TLType_Frames")        return "Frames";
        return type.empty() ? std::string ("unknown") : type;
    }

    const char* const linkOption = "link_timeline_and_edit_selection";
    const char* const samplesType = "TLType_Samples";
    const char* const barsBeatsType = "TLType_BarsBeats";
}

//==============================================================================
WriteOutcome runWrite (const WriteSettings& settings, const LogLine& log, const Cancelled& cancelled,
                       const Progress& progress)
{
    Stamped say (log);

    const int total = (int) settings.pins.size();
    double rate = settings.sessionSampleRate;   // the session's own rate replaces it when the link gives one
    const auto offsetOf = [&rate] (const Pin& pin) { return (long long) std::llround (pin.time * rate); };
    const auto barBeatOf = [&settings] (const Pin& pin) { return formatBarBeat (pin.beat, settings.beatsPerBar); };
    const auto isCancelled = [&cancelled] { return cancelled && cancelled(); };
    const auto tell = [&progress] (Phase phase, int done, int of)
    {
        if (progress)
            progress (phase, done, of);
    };

    WriteOutcome outcome;
    outcome.total = total;

    Driver driver ([&say] (const std::string& text) { say (text); }, cancelled);
    driver.setBundleIdentifier (settings.bundleIdentifier);
    driver.setQuick (true, settings.slowKeys);

    ptsl::Client client;
    LinkGate gate;

    // What the run changed through the link, to put back at the end, after a stop or a failure too.
    bool counterChanged = false, optionsChanged = false, cursorMoved = false, selectionKnown = false;
    std::string counterWas;
    ptsl::EditModeOptions optionsWere;
    long long selectionWasIn = 0, selectionWasOut = 0;

    const auto putBack = [&]
    {
        if (! gate.linked() || ! (counterChanged || optionsChanged || cursorMoved))
            return;

        say ("");
        say ("Step: put back what the scripting link changed");

        // A dialog this run opened may still be up, after a stop or a failure. An Identify Beat
        // dialog is cancelled, even after a stop; any other dialog is left alone, and then
        // nothing is sent, as Pro Tools would hold the commands back until it closes.
        if (gate.dialogMayBeOpen())
        {
            if (const auto closed = driver.closeIdentifyBeatIfOpen (0); closed.ok)
                gate.dialogClosed();
            else
                say ("A dialog may still be open in Pro Tools: " + closed.message);
        }

        if (gate.dialogMayBeOpen())
        {
            std::string byHand;

            if (counterChanged)
                byHand += " Set the Main Counter back to " + counterName (counterWas) + ".";

            if (optionsChanged)
                byHand += " Switch Link Timeline and Edit Selection off again.";

            outcome.restoreFailed = true;
            outcome.restoredNote = "A Pro Tools dialog may still be open, so nothing was put back." + byHand;
            say (outcome.restoredNote);
            return;
        }

        auto* c = gate.get();
        std::string done, notDone;

        const auto note = [] (std::string& text, const std::string& sentence)
        {
            text += (text.empty() ? "" : " ") + sentence;
        };

        if (cursorMoved && selectionKnown)
        {
            const auto r = c->setTimelineSelection (std::to_string (selectionWasIn), std::to_string (selectionWasOut), samplesType);
            say ("The selection: samples " + std::to_string (selectionWasIn) + " to " + std::to_string (selectionWasOut));

            if (r.ok)
                note (done, "The selection is as it was before the run.");
            else
                note (notDone, "The selection could not be put back (" + r.message + ").");
        }

        if (optionsChanged)
        {
            const auto r = c->setEditModeOptions (optionsWere);

            if (r.ok)
                note (done, "Link Timeline and Edit Selection is off again.");
            else
                note (notDone, "Link Timeline and Edit Selection could not be switched off again (" + r.message + "). Switch it off by hand.");
        }

        if (counterChanged)
        {
            const auto r = c->setMainCounterFormat (counterWas);
            bool confirmed = false;

            if (r.ok)
                if (const auto now = c->mainCounterFormat(); now.ok())
                    confirmed = now.value.type == counterWas;

            if (confirmed)
                note (done, "The Main Counter shows " + counterName (counterWas) + " again.");
            else
                note (notDone, "The Main Counter could not be set back to " + counterName (counterWas) + (r.ok ? std::string() : " (" + r.message + ")")
                               + ". Set it by hand.");
        }

        outcome.restoreFailed = ! notDone.empty();
        outcome.restoredNote = done;
        note (outcome.restoredNote, notDone);
        say (outcome.restoredNote);
    };

    // Ends the run: what was changed is put back, then the summary.
    const auto finish = [&] () -> WriteOutcome
    {
        putBack();
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

        for (const auto* line : { &outcome.moveNote, &outcome.checkSummary, &outcome.restSummary, &outcome.beforeSummary })
            if (! line->empty())
                say (*line);

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
    say (std::string ("Pro Tools Scripting link: ") + (! ptsl::Client::hasLink() ? ptsl::noLinkMessage
                                                       : settings.useScriptingLink ? "built in" : "built in, not used for this run"));

    // Clears the dialog's mark once it is seen to close.
    const auto cancelOrNote = [&] (const Element& dialog)
    {
        const auto closed = driver.cancelDialog (dialog);

        if (closed.ok)
        {
            gate.dialogClosed();
        }
        else
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
        char text[48];
        std::snprintf (text, sizeof (text), "%.10g", settings.sessionSampleRate);
        say (std::string ("Session sample rate given for the pins: ") + text + (settings.rateTyped ? " (typed in the sheet)" : ""));
        say (settings.hasMusicStart ? "Music starts at sample " + std::to_string (settings.musicStartSample) + " (" + settings.musicStartSource + ")"
                                    : std::string ("Music start: read from Pro Tools at the start of the run"));
        say (std::string ("Keys: ") + (settings.slowKeys ? "typed slowly" : "typed at the quick pace"));
        say (std::string ("Check each pin afterwards: ") + (settings.checkAfterwards ? "yes" : "no"));
        say ("Pins to write: " + std::to_string (total));

        for (int i = 0; i < total; ++i)
        {
            const auto& pin = settings.pins[(size_t) i];
            char line[160];
            std::snprintf (line, sizeof (line), "  %3d  %-12s %10s s", i + 1, barBeatOf (pin).c_str(), seconds (pin.time).c_str());
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

    //==============================================================================
    // The scripting link, before any dialog is opened: the session, the transport, the Main
    // Counter and the edit options. Without it the run goes on with keys only.
    bool haveStart = settings.hasMusicStart;
    long long musicStart = settings.musicStartSample;
    bool movesByLink = false;
    const long long sessionStartSample = 0;     // the session's start time is sample 0
    long long clipEndSample = 0;
    TempoSurvey restBefore, earlyBefore;

    const auto keysOnly = [&] (const std::string& why)
    {
        outcome.linkNote = why;
        say (why);
    };

    if (settings.useScriptingLink)
    {
        say ("Step: connect to Pro Tools' scripting link");

        if (std::string problem; openLink (client, problem))
        {
            gate.use (client);
            outcome.linked = true;
        }
        else
        {
            keysOnly ("The scripting link was not used: " + problem + ". The Main Counter must show Samples and the transport must be "
                      "stopped, as without it.");
        }
    }

    if (gate.linked())
    {
        auto* c = gate.get();
        SessionReader reader;
        const auto reading = reader.read (*c, settings.music, isCancelled);

        if (! reading.sessionOpen)
            return stop (reading.problem.empty() ? std::string ("the session could not be read through the scripting link") : reading.problem);

        say ("Session: " + reading.sessionName);
        say ("Session sample rate: " + std::to_string ((long long) reading.sampleRate)
             + ". Transport: " + (reading.transport.empty() ? std::string ("not read") : reading.transport)
             + ". Main Counter: " + counterName (reading.counterType));

        std::string files;

        for (const auto& name : reading.selectedClips)
            files += (files.empty() ? "" : ", ") + name;

        say ("Selected clips: " +(files.empty() ? std::string ("none") : files)
             + (reading.hasSelection ? ". Edit selection: samples " + std::to_string (reading.selectionIn) + " to " + std::to_string (reading.selectionOut)
                                     : std::string (". The edit selection could not be read")));

        if (reading.sampleRate > 0.0 && std::abs (reading.sampleRate - rate) > 0.5)
        {
            if (settings.rateTyped)
                say ("Note: the session runs at " + std::to_string ((long long) reading.sampleRate) + " Hz, but the rate typed in the sheet is used.");
            else
            {
                say ("The pins are placed at the session's own rate, " + std::to_string ((long long) reading.sampleRate) + " Hz.");
                rate = reading.sampleRate;
            }
        }

        if (! haveStart && reading.hasMusicStart)
        {
            haveStart = true;
            musicStart = reading.musicStart;

            if (reading.placedFromClips)
            {
                say ("The music's first clip in the edit selection, \"" + reading.clipName + "\", starts at sample " + std::to_string (reading.clipStart)
                     + "; " + std::to_string (reading.clipsInSelection) + (reading.clipsInSelection == 1 ? " clip" : " clips")
                     + " of the file are in the selection, up to sample " + std::to_string (reading.clipEnd) + ".");

                if (reading.clipsOffLine > 0)
                    say ("Note: " + std::to_string (reading.clipsOffLine) + " of those clips were moved against the first, so the music "
                         "start fits only the first clip.");
            }

            switch (reading.trim)
            {
                case Trim::whole:   say ("The selected clip starts at the start of its file."); break;
                case Trim::found:   say ("The selected clip starts " + std::to_string (reading.trimSamples)
                                         + " samples into its file, read from the clip list."); break;
                case Trim::unknown: say ("Note: if the clip has been trimmed at its start, the grid will be off by the trimmed amount."); break;
                case Trim::none:    say ("Note: no single clip is selected. The music start is where the cursor is."); break;
            }

            say ("");
            say ("MUSIC START: sample " + std::to_string (musicStart) + ", read through the scripting link. Every pin is placed from this sample.");
        }

        if (haveStart)
            if (const int bad = pinBeforeSessionStart (musicStart); bad > 0)
                return stop (beforeSessionStart (bad, musicStart));

        selectionKnown = reading.hasSelection;
        selectionWasIn = reading.selectionIn;
        selectionWasOut = reading.selectionOut;

        // The transport.
        if (! reading.stopped)
        {
            say ("Step: stop the transport (" + (reading.transport.empty() ? std::string ("its state was not read") : reading.transport) + ")");

            if (reading.transport.empty())
                return stop ("the transport's state could not be read through the scripting link");

            // Recording is never stopped from here. Play is toggled off; anything else (stopping,
            // cueing) is given a moment to settle.
            if (reading.transport.find ("Recording") != std::string::npos)
                return stop ("Pro Tools is recording. Stop it and try again");

            if (reading.transport.find ("Playing") != std::string::npos)
                if (const auto r = c->togglePlayState(); ! r.ok)
                    return stop ("the transport could not be stopped through the scripting link: " + r.message + ". Stop it and try again");

            bool stopped = false;

            for (int i = 0; i < 30 && ! stopped; ++i)
            {
                if (const auto state = c->transportState(); state.ok())
                    stopped = ptsl::Client::isStopped (state.value);

                if (! stopped && ! driver.pause (100).ok)
                    return stop ("the run was stopped");
            }

            if (! stopped)
                return stop ("the transport did not stop. Stop it and try again");

            say ("The transport is stopped.");
        }

        // The Main Counter, which the key route and the checks read in samples.
        if (reading.counterType.empty())
        {
            // Pro Tools before 2024.06 has no command for it: the counter must show Samples already,
            // which is read from the window further on.
            say ("Note: the Main Counter's format could not be read through the scripting link. It must be set to Samples by hand.");
        }
        else if (! reading.counterIsSamples)
        {
            say ("Step: set the Main Counter to Samples (it shows " + counterName (reading.counterType) + ")");
            counterWas = reading.counterType;
            counterChanged = true;
            const auto r = c->setMainCounterFormat (samplesType);
            const auto now = c->mainCounterFormat();

            if (! r.ok || ! now.ok() || now.value.type != samplesType)
                return stop ("the Main Counter could not be set to Samples" + (r.ok ? std::string() : ": " + r.message));

            say ("The Main Counter shows Samples. It is put back to " + counterName (counterWas) + " at the end.");
        }

        // Link Timeline and Edit Selection, so a timeline selection moves the edit cursor that
        // Identify Beat reads.
        movesByLink = true;

        if (const auto options = c->editModeOptions(); ! options.ok())
        {
            movesByLink = false;
            outcome.moveNote = "The edit options could not be read through the scripting link (" + options.reply.message
                               + "), so the cursor was moved with the keys";
        }
        else if (bool on = false; ! options.value.get (linkOption, on))
        {
            movesByLink = false;
            outcome.moveNote = "Pro Tools did not give the Link Timeline and Edit Selection option, so the cursor was moved with the keys";
        }
        else if (! on)
        {
            say ("Step: switch Link Timeline and Edit Selection on");
            optionsWere = options.value;
            auto linkedOptions = options.value;
            linkedOptions.set (linkOption, true);
            optionsChanged = true;

            const auto r = c->setEditModeOptions (linkedOptions);
            bool nowOn = false;

            if (const auto now = c->editModeOptions(); ! r.ok || ! now.ok() || ! now.value.get (linkOption, nowOn) || ! nowOn)
            {
                movesByLink = false;
                outcome.moveNote = "Link Timeline and Edit Selection could not be switched on" + (r.ok ? std::string() : " (" + r.message + ")")
                                   + ", so the cursor was moved with the keys";
            }
            else
            {
                say ("Link Timeline and Edit Selection is on. It is switched off again at the end.");
            }
        }

        if (! outcome.moveNote.empty())
            say (outcome.moveNote + ".");

        // The tempo of the rest of the session, read before the write and again after it.
        if (haveStart)
        {
            // The session starts at sample 0 (Pro Tools turns sample 0 into the session's start
            // time). Its length is a default duration, not where its material ends, so the end
            // of the last clip on any track is read instead.
            const double fileToSession = settings.music.sampleRate > 0.0 ? rate / settings.music.sampleRate : 1.0;
            clipEndSample = reading.placedFromClips && reading.clipEnd > musicStart ? reading.clipEnd
                          : reading.selectedClips.size() == 1 && reading.hasSelection && reading.selectionOut > reading.selectionIn
                                ? reading.selectionOut
                                : musicStart + (long long) std::llround ((double) settings.music.lengthSamples * fileToSession);

            long long materialEnd = 0;
            std::string materialProblem;
            const bool materialKnown = readMaterialEnd (*c, materialEnd, materialProblem);
            const auto plan = planSurvey (musicStart, clipEndSample, materialKnown, materialEnd, rate);

            say ("Step: read the bars and beats of the session before and after the music");
            say ("The survey after the music runs to sample " + std::to_string (plan.afterEnd) + ", " + describeAfterEnd (plan.afterSource)
                 + (materialKnown ? std::string() : " (" + materialProblem + ")") + ".");

            const auto allowed = [&] { return ! isCancelled() && ! gate.dialogMayBeOpen(); };
            const auto surveyStart = std::chrono::steady_clock::now();

            if (plan.after.use)
                restBefore = surveyTempo (*c, probePoints (plan.after.from, plan.after.to, rate), settings.beatsPerBar, allowed);

            if (plan.before.use)
                earlyBefore = surveyTempo (*c, probePoints (plan.before.from, plan.before.to, rate), settings.beatsPerBar, allowed);

            const double surveySeconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - surveyStart).count();

            say ("After the music: " + std::to_string (restBefore.samples.size()) + " points"
                 + (restBefore.ok || restBefore.samples.empty() ? std::string() : ", not read: " + restBefore.problem)
                 + ". Before it: " + std::to_string (earlyBefore.samples.size()) + " points"
                 + (earlyBefore.ok || earlyBefore.samples.empty() ? std::string() : ", not read: " + earlyBefore.problem)
                 + (restBefore.inTickUnits ? ". Read in ticks, " + std::to_string (restBefore.ticksPerBeat) + " to the beat" : std::string())
                 + ". " + seconds (surveySeconds, 1) + " s.");
        }
    }

    //==============================================================================
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

    // The keys type each pin's sample into the Main Counter and read it back, so the counter has
    // to be found and has to show samples before a key is sent. It is read with the link too, as
    // the keys are the way back when a move through the link fails.
    say ("Step: read the Main Counter");
    std::string counter;

    if (const auto r = driver.readMainCounter (counter); ! r.ok)
        return stop (r.message + ". No key was sent");

    const auto counterAsDigits = counterDigits (counter);

    if (! allDigits (counterAsDigits) || counterAsDigits.size() > 15)
        return stop ("the Main Counter is not set to Samples (it shows \"" + counter + "\"). Set it to Samples and try again. "
                     "No key was sent");

    say ("The Main Counter reads as samples: " + counterAsDigits);

    if (! haveStart)
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
    // Moves the cursor to a sample through the link and reads the edit selection back.
    enum class Move { moved, failed, stopped };

    const auto moveWithLink = [&] (long long sample, std::string& why)
    {
        auto* c = gate.get();

        if (c == nullptr)
        {
            why = "a dialog may be open";
            return Move::failed;
        }

        const auto at = std::to_string (sample);
        cursorMoved = true;

        if (const auto r = c->setTimelineSelection (at, at, samplesType); ! r.ok)
        {
            why = "the timeline selection could not be set: " + r.message;
            return isCancelled() ? Move::stopped : Move::failed;
        }

        for (int i = 0; i < 10; ++i)
        {
            long long in = 0;

            if (const auto selection = c->editSelection (samplesType); selection.ok() && samplesOf (selection.value.in, in) && in == sample)
                return Move::moved;

            if (! driver.pause (50).ok)
                return Move::stopped;
        }

        why = "the edit cursor was not seen at sample " + at;
        return Move::failed;
    };

    // The first move through the link is compared with what Identify Beat suggests, once per run.
    bool compareFirstMove = movesByLink;

    // From here the cursor moves, by the link or the keys. With the link, the selection the run
    // started with is put back at the end.
    cursorMoved = gate.linked();
    const double writeStart = say.elapsed();

    for (int i = 0; i < total; ++i)
    {
        const int number = i + 1;
        tell (Phase::writing, i, total);

        if (isCancelled())
            return stop ("the run was stopped", number);

        const auto& pin = settings.pins[(size_t) i];
        const long long sample = musicStart + offsetOf (pin);
        const auto barBeat = barBeatOf (pin);
        const double pinStart = say.elapsed();

        say ("");
        say ("Pin " + std::to_string (number) + " of " + std::to_string (total) + ": " + barBeat + " at sample " + std::to_string (sample)
             + " (" + seconds (pin.time) + " s into the music)");

        std::string expected;   // what the link says the cursor's bar|beat is, for the comparison

        if (movesByLink)
        {
            std::string why;
            const auto moved = moveWithLink (sample, why);

            if (moved == Move::stopped)
                return stop ("the run was stopped", number);

            if (moved == Move::failed)
            {
                movesByLink = compareFirstMove = false;
                outcome.moveNote = "The cursor could not be moved through the scripting link at pin " + std::to_string (number) + " (" + why
                                   + "), so the keys moved it from there on";
                say (outcome.moveNote + ".");
            }
            else
            {
                say ("Moved the cursor through the scripting link.");

                if (compareFirstMove)
                {
                    // Read before the dialog opens: no scripting command is sent while it is up.
                    if (const auto converted = gate.get()->timeAsType (std::to_string (sample), samplesType, barsBeatsType); converted.ok())
                    {
                        expected = converted.value;
                    }
                    else
                    {
                        movesByLink = compareFirstMove = false;
                        outcome.moveNote = "The bar|beat at the cursor could not be read through the scripting link (" + converted.reply.message
                                           + "), so the cursor was moved with the keys";
                        say (outcome.moveNote + ".");
                    }
                }
            }
        }

        if (! movesByLink)
            if (const auto r = driver.goToSample (sample); ! r.ok)
                return stop (r.message, number);

        Element dialog;
        gate.dialogOpening();

        if (const auto r = driver.openIdentifyBeat (menuItem, dialog); ! r.ok)
            return stop (r.message, number);

        if (compareFirstMove && movesByLink)
        {
            compareFirstMove = false;
            std::string shown;
            long long shownTicks = 0, expectedTicks = 0;
            const bool read = driver.readIdentifyBeatLocation (dialog, shown).ok;
            const bool agree = read && ticksOf (shown, settings.beatsPerBar, shownTicks) && ticksOf (expected, settings.beatsPerBar, expectedTicks)
                               && std::llabs (shownTicks - expectedTicks) <= ticksAllowed;

            if (agree)
            {
                outcome.moveNote = "Identify Beat followed the cursor moves made through the scripting link";
                say (outcome.moveNote + ": it suggests \"" + shown + "\", where the link puts the cursor at \"" + expected + "\".");
            }
            else
            {
                outcome.moveNote = "Identify Beat did not follow the cursor moves made through the scripting link (it suggested \""
                                   + (read ? shown : std::string ("nothing readable")) + "\" where \"" + expected
                                   + "\" was expected), so the keys moved the cursor for this run";
                say (outcome.moveNote + ". The dialog is cancelled and the pin is written again with the keys.");
                movesByLink = false;

                if (const auto closed = cancelOrNote (dialog); ! closed.ok)
                    return stop (closed.message, number);

                if (const auto r = driver.goToSample (sample); ! r.ok)
                    return stop (r.message, number);

                gate.dialogOpening();

                if (const auto r = driver.openIdentifyBeat (menuItem, dialog); ! r.ok)
                    return stop (r.message, number);
            }
        }

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

        gate.dialogClosed();
        ++outcome.written;
        say ("Pin " + std::to_string (number) + " written: " + barBeat + " at sample " + std::to_string (sample)
             + ", " + seconds (say.elapsed() - pinStart, 2) + " s");
    }

    outcome.movedWithLink = movesByLink;
    tell (Phase::writing, total, total);

    {
        const double took = say.elapsed() - writeStart;
        say ("");
        say (pins (total) + " in " + seconds (took, 1) + " s, " + seconds (took / (double) total, 2) + " s for each");
    }

    //==============================================================================
    // The check. Through the link: the pin's sample and the pin's bar|beat are both turned into
    // the session's ticks, and the two must be within 2 samples' worth of ticks. Ticks are used
    // because bar|beat to samples rounds through the display's 960 ticks to the beat (a few
    // samples at most tempos: 2882880 read as bar|beat came back as 2882877), while Pro Tools'
    // own ticks (960000 to the beat, counted from a large fixed offset) give both positions
    // exactly. Opens no dialog. Without the link: at each pin the Identify Beat dialog is opened,
    // the location it suggests is read, and the dialog is cancelled. Either way nothing is changed.
    bool checkedWithLink = false;

    if (settings.checkAfterwards && gate.linked() && ! gate.dialogMayBeOpen())
    {
        say ("");
        say ("Check: turn each pin's sample and its bar|beat into ticks through the scripting link");
        auto* c = gate.get();
        constexpr double samplesAllowed = 2.0;
        double worst = 0.0;
        std::vector<PinCheck> off;
        std::string failed;

        struct Read { long long sample = 0, sampleTicks = 0, barBeatTicks = 0; };
        std::vector<Read> reads;

        const auto ticksAt = [&] (const std::string& location, const char* type, long long& ticks, std::string& why)
        {
            const auto converted = c->timeAsType (location, type, "TLType_Ticks");

            if (converted.ok() && samplesOf (converted.value, ticks))
                return true;

            why = converted.ok() ? "\"" + converted.value + "\" is not a number of ticks" : converted.reply.message;
            return false;
        };

        for (int i = 0; i < total && failed.empty(); ++i)
        {
            tell (Phase::checking, i, total);

            if (isCancelled())
            {
                failed = "the run was stopped";
                break;
            }

            const auto& pin = settings.pins[(size_t) i];
            Read read;
            read.sample = musicStart + offsetOf (pin);
            std::string why;

            if (! ticksAt (std::to_string (read.sample), samplesType, read.sampleTicks, why)
                || ! ticksAt (barBeatOf (pin), barsBeatsType, read.barBeatTicks, why))
                failed = "pin " + std::to_string (i + 1) + ": " + why;

            reads.push_back (read);
        }

        // Ticks per sample near each pin, from its neighbours; for one pin, a second later.
        long long lone = 0;
        std::string why;

        if (failed.empty() && total == 1 && ! ticksAt (std::to_string (reads[0].sample + (long long) std::llround (rate)), samplesType, lone, why))
            failed = "the ticks a second after the pin: " + why;

        for (int i = 0; i < total && failed.empty(); ++i)
        {
            const auto& read = reads[(size_t) i];
            double perSample = 0.0;

            if (total == 1)
                perSample = (double) (lone - read.sampleTicks) / rate;
            else
            {
                const auto& a = reads[(size_t) (i + 1 < total ? i : i - 1)];
                const auto& b = reads[(size_t) (i + 1 < total ? i + 1 : i)];
                perSample = b.sample != a.sample ? (double) (b.sampleTicks - a.sampleTicks) / (double) (b.sample - a.sample) : 0.0;
            }

            if (! (perSample > 0.0))
            {
                failed = "pin " + std::to_string (i + 1) + ": the ticks do not rise with the samples";
                break;
            }

            const auto& pin = settings.pins[(size_t) i];
            const auto barBeat = barBeatOf (pin);
            const double diff = samplesOffInTicks (read.sampleTicks, read.barBeatTicks, perSample);
            const bool within = std::abs (diff) <= samplesAllowed;
            worst = std::max (worst, std::abs (diff));
            say ("Check pin " + std::to_string (i + 1) + ": " + barBeat + " is at " + std::to_string (read.barBeatTicks) + " ticks, sample "
                 + std::to_string (read.sample) + " at " + std::to_string (read.sampleTicks) + " ticks"
                 + (read.barBeatTicks == read.sampleTicks ? std::string() : ", " + seconds (std::abs (diff), 2) + " samples " + (diff > 0 ? "later" : "earlier"))
                 + (within ? "" : "  NOT WITHIN 2 SAMPLES"));

            if (! within)
            {
                PinCheck check;
                check.pin = i + 1;
                check.wanted = barBeat;
                check.found = std::to_string (read.sample + (long long) std::llround (diff));
                check.inSamples = true;
                check.samplesOff = std::llround (diff);
                off.push_back (check);
            }
        }

        if (failed.empty())
        {
            checkedWithLink = true;
            tell (Phase::checking, total, total);
            outcome.checked = true;
            outcome.checkedPins = total;
            outcome.off = off;
            const auto largest = " (largest error " + seconds (worst, 2) + " samples)";

            if (off.empty())
                outcome.checkSummary = (total == 1 ? std::string ("The pin checks out") : "All " + pins (total) + " check out") + largest;
            else
                outcome.checkSummary = std::to_string (off.size()) + " of " + pins (total)
                                       + (off.size() == 1 ? " does not check out" : " do not check out") + largest;
        }
        else
        {
            say ("The check through the scripting link stopped (" + failed + "). The pins are checked with the dialog instead.");
        }
    }

    if (settings.checkAfterwards && ! checkedWithLink)
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

            if (isCancelled())
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
            {
                gate.dialogOpening();
                step = driver.openIdentifyBeat (menuItem, dialog);
            }

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

    //==============================================================================
    // The rest of the session: the same points read again. After the music the distance between
    // neighbouring points must be the same (all of them may move by whole beats, when the bars
    // after the music are numbered on from it). Before the music any change is reported.
    if (gate.linked() && ! gate.dialogMayBeOpen() && haveStart && ! isCancelled())
    {
        say ("");
        say ("Step: read the bars and beats of the session again");
        auto* c = gate.get();
        const auto allowed = [&] { return ! isCancelled() && ! gate.dialogMayBeOpen(); };
        const auto timeOf = [&] (long long sample)
        {
            return minutesAndSeconds ((double) (sample - sessionStartSample) / rate);
        };

        // A shift by part of a beat, as "300 ticks (about a third of a beat) later".
        const auto partOfBeat = [] (const TempoChange& change, long long perBeat)
        {
            const double beats = (double) std::llabs (change.shiftTicks) / (double) std::max (1LL, perBeat);
            char amount[32];
            std::snprintf (amount, sizeof (amount), "%.2f", beats);
            return std::to_string (std::llabs (change.shiftTicks)) + " ticks (" + amount + " of a beat) " + (change.shiftTicks > 0 ? "later" : "earlier");
        };

        if (restBefore.samples.empty())
        {
            outcome.restSummary = "Rest of the session: nothing after the music to check";
        }
        else
        {
            const auto restAfter = surveyTempo (*c, restBefore.samples, settings.beatsPerBar, allowed, restBefore.inTickUnits);
            const auto change = compareTempo (restBefore, restAfter);

            if (! change.compared)
            {
                outcome.restSummary = "Rest of the session: not checked (" + change.problem + ")";
            }
            else if (change.changed)
            {
                outcome.restChanged = true;
                outcome.restSummary = "Rest of the session: the tempo after the music changed between " + timeOf (change.firstSample) + " and "
                                      + timeOf (change.lastSample) + ". Check it, or Undo.";
            }
            else if (change.partBeat)
            {
                // The tempo is the same, but every beat after the music has moved by part of a
                // beat, so the beats no longer fall on the same audio.
                outcome.restChanged = true;
                outcome.restSummary = "Rest of the session: the beats after the music moved " + partOfBeat (change, restBefore.ticksPerBeat)
                                      + ", so they no longer sit on the same audio. Check it, or Undo.";
            }
            else
            {
                outcome.restSummary = "Rest of the session: tempo unchanged";

                if (change.shifted)
                    outcome.restSummary += ". The bar numbers after the music moved " + describeShift (change.shiftBeats, settings.beatsPerBar);
            }
        }

        if (! earlyBefore.samples.empty())
        {
            const auto earlyAfter = surveyTempo (*c, earlyBefore.samples, settings.beatsPerBar, allowed, earlyBefore.inTickUnits);
            const auto change = compareTempo (earlyBefore, earlyAfter);

            if (! change.compared)
                say ("Before the music: not checked (" + change.problem + ")");
            else if (change.changed)
                outcome.beforeSummary = "Before the music: the tempo changed between " + timeOf (change.firstSample) + " and "
                                        + timeOf (change.lastSample) + ". Identify Beat can change the tempo before the first pin.";
            else if (change.partBeat)
                outcome.beforeSummary = "Before the music: the beats moved " + partOfBeat (change, earlyBefore.ticksPerBeat) + ".";
            else if (change.shifted)
                outcome.beforeSummary = "Before the music: the bars moved " + describeShift (change.shiftBeats, settings.beatsPerBar) + ".";
            else
                say ("Before the music: tempo unchanged.");
        }
    }

    // Back to the start of the music, so the cursor is where a run expects it. With the link the
    // selection is put back as it was when the run started.
    if (! isCancelled() && ! (gate.linked() && cursorMoved && selectionKnown))
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

#include "AudioSuiteRoute.h"

#include "plugin/AppCaptureListener.h"
#include "protools/ProToolsDriver.h"
#include "protools/PtslSession.h"
#include "ptsl/PtslClient.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace tt::capture
{

namespace
{
    using Clock = std::chrono::steady_clock;

    int millisecondsSince (Clock::time_point start)
    {
        return (int) std::chrono::duration_cast<std::chrono::milliseconds> (Clock::now() - start).count();
    }

    // What the scripting link reads before the render and again after the Undo.
    struct SessionReading
    {
        bool read = false;
        long long in = 0, out = 0;
        std::size_t clips = 0;
        double rate = 0.0;
        std::string sessionName;
        std::string problem;

        bool sameAs (const SessionReading& other) const
        {
            return read && other.read && in == other.in && out == other.out && clips == other.clips;
        }

        std::string text() const
        {
            if (! read)
                return "not read (" + problem + ")";

            return "edit selection " + std::to_string (in) + " to " + std::to_string (out) + ", " + std::to_string (clips) + " clips";
        }
    };

    SessionReading readSession (ptsl::Client& client)
    {
        SessionReading r;

        if (! ptsl::Client::hasLink())
        {
            r.problem = ptsl::noLinkMessage;
            return r;
        }

        if (! pt::openLink (client, r.problem))
            return r;

        const auto selection = client.editSelection ("TLType_Samples");

        if (! selection.ok() || ! pt::samplesOf (selection.value.in, r.in) || ! pt::samplesOf (selection.value.out, r.out))
        {
            r.problem = "edit selection: " + selection.reply.message;
            return r;
        }

        const auto clips = client.clipList();

        if (! clips.ok())
        {
            r.problem = "clip list: " + clips.reply.message;
            return r;
        }

        r.clips = clips.value.size();

        if (const auto rate = client.sessionSampleRate(); rate.ok())
            r.rate = rate.value;

        if (const auto name = client.sessionName(); name.ok())
            r.sessionName = name.value;

        r.read = true;
        return r;
    }

    // Before Render, through the scripting link: Pro Tools is ready, stopped, and exactly one
    // audio track is selected. AudioSuite renders each selected track (and each channel of a
    // multi-mono render, and each clip clip by clip) as a pass of its own. Empty when all is well,
    // otherwise the sentence for the person.
    std::string readyToRender (ptsl::Client& client, const std::function<void (const std::string&)>& say)
    {
        const auto ready = client.hostReadyCheck();
        say ("HostReadyCheck: " + (ready.ok ? std::string ("ready") : ready.message));

        if (! ready.ok)
            return "Pro Tools is not ready (" + ready.message + "). Close any dialog open in it and try again";

        const auto transport = client.transportState();
        say ("Transport: " + (transport.ok() ? transport.value : "not read: " + transport.reply.message));

        if (! transport.ok())
            return "Pro Tools did not say whether it is playing. Try again once it is stopped";

        if (! pt::isStoppedState (transport.value))
            return "Stop playback in Pro Tools first";

        const auto tracks = client.trackList();

        if (! tracks.ok())
            return "Could not read the tracks in Pro Tools";

        std::vector<std::string> selected;
        bool audio = false;

        for (const auto& track : tracks.value)
        {
            if (! track.selected)
                continue;

            selected.push_back (track.name);
            audio = track.type.size() >= 5 && track.type.compare (track.type.size() - 5, 5, "Audio") == 0;
        }

        say ("Selected tracks: " + std::to_string (selected.size()));

        if (selected.empty())
            return "Select the music in Pro Tools first";

        if (selected.size() > 1)
            return std::to_string (selected.size()) + " tracks are selected in Pro Tools. AudioSuite renders each one separately, so select only the music track";

        if (! audio)
            return "The selected track in Pro Tools is not an audio track. Select the music on its audio track";

        return {};
    }

    std::mutex logLock;
}

//==============================================================================
int captureWaitMilliseconds (double selectionSeconds)
{
    const double ms = 10000.0 + std::ceil (std::max (0.0, selectionSeconds) / 60.0 * 5000.0);
    return (int) std::min (ms, (double) longestCaptureWaitMs);
}

juce::File getFromProToolsLog()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
               .getChildFile ("Library/Logs/Sammy J/Tempo Tantrum/Get from Pro Tools.log");
}

void appendToLog (const std::vector<std::string>& lines)
{
    const std::lock_guard<std::mutex> hold (logLock);
    const auto file = getFromProToolsLog();
    file.getParentDirectory().createDirectory();

    // Kept to about a megabyte: the older half goes.
    if (file.getSize() > 1024 * 1024)
    {
        const auto text = file.loadFileAsString();
        file.replaceWithText (text.substring (text.length() / 2));
    }

    juce::FileOutputStream out (file);

    if (! out.openedOk())
        return;

    const auto now = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S");

    for (const auto& line : lines)
        out << now << "  " << juce::String::fromUTF8 (line.c_str()) << "\n";
}

//==============================================================================
AudioSuiteOutcome findCapturePlugIn (const std::function<void (const std::string&)>& log, const std::function<bool()>& stop)
{
    AudioSuiteOutcome o;
    const auto say = [&] (const std::string& line)
    {
        o.log.push_back (line);

        if (log)
            log (line);
    };

    if (! pt::Driver::accessibilityAllowed())
    {
        say ("Accessibility permission is missing, so the AudioSuite menu cannot be read");
        o.noAccessibility = true;
        o.error = "To use Tempo Tantrum Capture, allow Tempo Tantrum in System Settings > Privacy & Security > Accessibility";
        return o;
    }

    pt::Driver driver (say, stop);

    if (! driver.findProTools().running)
    {
        o.error = "Pro Tools is not open";
        return o;
    }

    pt::Element item;
    bool enabled = false;
    const auto found = driver.findAudioSuiteItem (capturePlugInName, item, enabled, o.menuPath);

    o.found = found.ok;
    o.ok = found.ok;

    if (! found.ok)
        o.error = juce::String (found.message);
    else if (! enabled)
        say ("The menu item is switched off now (no selection?). It is still taken as installed.");

    return o;
}

AudioSuiteOutcome renderWithCapturePlugIn (const std::function<void (const std::string&)>& log,
                                           const std::function<void (const juce::String&)>& progress,
                                           const std::function<bool()>& stop)
{
    using audiosuite::AppCaptureListener;

    AudioSuiteOutcome o;
    const auto say = [&] (const std::string& line)
    {
        o.log.push_back (line);

        if (log)
            log (line);
    };
    const auto tell = [&] (const juce::String& text)
    {
        if (progress)
            progress (text);
    };

    // Once Render has been pressed a stop is not taken: the render must still be undone.
    std::atomic<bool> renderPressed { false };
    const auto stopped = [&] { return ! renderPressed.load() && stop && stop(); };

    const auto fail = [&] (const std::string& sentence)
    {
        say ("Stopped: " + sentence);
        o.ok = false;
        o.error = juce::String::fromUTF8 (sentence.c_str());
        return o;
    };

    say ("AudioSuite route: render the edit selection with " + std::string (capturePlugInName) + ", take the capture, undo the render");

    if (! pt::Driver::accessibilityAllowed())
    {
        o.noAccessibility = true;
        return fail ("To use Tempo Tantrum Capture, allow Tempo Tantrum in System Settings > Privacy & Security > Accessibility");
    }

    // 1. What must be the same afterwards, through the scripting link, and whether Pro Tools is
    //    ready to render one track.
    ptsl::Client client;
    const auto before = readSession (client);
    say ("Before: " + before.text());

    if (before.read && before.out <= before.in)
        return fail ("Select the music in Pro Tools first");

    if (before.read)
        if (const auto problem = readyToRender (client, say); ! problem.empty())
            return fail (problem);

    const double selectionSeconds = before.read && before.rate > 0.0 ? (double) (before.out - before.in) / before.rate : 600.0;
    const int waitMs = captureWaitMilliseconds (selectionSeconds);
    const int settleMs = std::max (60000, waitMs);
    say ("The capture is waited for up to " + std::to_string (waitMs) + " ms after Render, and Pro Tools' windows up to "
         + std::to_string (settleMs) + " ms more");

    // 2. Pro Tools, in front, with no dialog or progress window open.
    pt::Driver driver (say, stopped);
    const auto app = driver.findProTools();

    if (! app.running)
        return fail ("Pro Tools is not open");

    tell ("Rendering the selection with Tempo Tantrum Capture\xe2\x80\xa6");

    if (auto r = driver.bringToFront(); ! r.ok)
        return fail (r.message);

    if (auto r = driver.checkWindowsAtStart(); ! r.ok)
        return fail (r.message);

    if (auto r = driver.rememberWindows(); ! r.ok)
        return fail (r.message);

    // 3. The AudioSuite window: one already open is used and left open.
    pt::Element window;
    bool openedHere = false;

    if (auto r = driver.findWindow (capturePlugInName, window); ! r.ok)
        return fail (r.message);

    if (window)
    {
        say ("The " + std::string (capturePlugInName) + " window is already open; it is used and left open");
    }
    else
    {
        pt::Element item;
        bool enabled = false;

        if (auto r = driver.findAudioSuiteItem (capturePlugInName, item, enabled, o.menuPath); ! r.ok)
            return fail (r.message);

        o.found = true;

        if (! enabled)
            return fail (o.menuPath + " is switched off in Pro Tools. Select the music on the timeline first");

        if (auto r = driver.openWindowFromMenu (item, capturePlugInName, window); ! r.ok)
            return fail (r.message);

        openedHere = true;
    }

    o.found = true;

    const auto closeIfOpenedHere = [&]
    {
        if (! openedHere)
            return true;

        const auto closed = driver.closeWindow (window);

        if (! closed.ok)
            say ("The AudioSuite window was not closed: " + closed.message);

        return closed.ok;
    };

    const auto failAndClose = [&] (const std::string& sentence)
    {
        closeIfOpenedHere();
        return fail (sentence);
    };

    // 4. Its controls, read and written to the log, and the Render button.
    pt::Element render;

    if (auto r = driver.readAudioSuiteWindow (window, "Render", render); ! r.ok)
        return failAndClose (r.message);

    std::string undoBefore;
    bool undoEnabled = false;

    if (auto r = driver.readUndo (undoBefore, undoEnabled); r.ok)
        say ("Before Render the Undo item reads \"" + undoBefore + "\"" + (undoEnabled ? "" : " (switched off)"));
    else
        say ("Before Render the Undo item could not be read: " + r.message);

    if (stopped())
        return failAndClose ("Stopped before Render");

    // 5. Render, once. From here the listener holds every capture that comes in, reads nothing
    //    from Pro Tools and opens nothing: this route decides, once Pro Tools has finished.
    AppCaptureListener::startHolding();
    bool handedOver = false;
    int ignoreLaterForMs = 0;
    const struct ReleaseHeld
    {
        bool& handedOver;
        int& ignoreLaterForMs;
        ~ReleaseHeld() { if (! handedOver) AppCaptureListener::stopHolding ({}, {}, {}, ignoreLaterForMs); }
    } releaseHeld { handedOver, ignoreLaterForMs };

    const auto captures = [] { return AppCaptureListener::heldCaptures().size(); };
    const auto renderStart = Clock::now();
    bool pressed = false;

    if (auto r = driver.pressButton (render, "Render", pressed); ! r.ok || ! pressed)
        return failAndClose (r.ok ? "Render was not pressed" : r.message);

    renderPressed = true;

    // 6. The capture: the plug-in's notification, held by the listener.
    while (millisecondsSince (renderStart) < waitMs && captures() == 0)
        juce::Thread::sleep (100);

    say (captures() > 0 ? "A capture came in " + std::to_string (millisecondsSince (renderStart)) + " ms after Render"
                        : "No capture came in within " + std::to_string (waitMs) + " ms of Render");

    // 7. Pro Tools' progress window, and anything else that opened, must be gone before anything
    //    more is read or pressed. While one is open the render may still be running: nothing is
    //    sent to Pro Tools, and if it does not finish in time the route stops there.
    {
        const auto settle = Clock::now();
        int extra = -1;

        for (;;)
        {
            if (auto r = driver.countNewWindows (window, extra); ! r.ok)
                extra = -1;

            if (extra == 0 || millisecondsSince (settle) >= settleMs)
                break;

            juce::Thread::sleep (200);
        }

        if (extra != 0)
        {
            say ("After Render, " + (extra > 0 ? std::to_string (extra) + " other window(s) were still open" : std::string ("Pro Tools did not answer"))
                 + " " + std::to_string (millisecondsSince (renderStart)) + " ms after Render. Nothing more is read or pressed");

            // A render still running would hand its capture in later; that one is not opened.
            ignoreLaterForMs = 10 * 60 * 1000;
            return fail ("Pro Tools is still busy after Render (a progress window or a dialog is open), so nothing was undone and nothing "
                         "more was pressed. When it has finished, check the session and press Cmd+Z in Pro Tools if a rendered clip is there");
        }
    }

    // The last pass's capture is written as that pass ends, so it may come a moment after the
    // progress window has gone: wait until none has come for a second and a half (at most ten).
    {
        auto count = captures();
        const auto late = Clock::now();
        auto lastChange = Clock::now();

        while (millisecondsSince (lastChange) < 1500 && millisecondsSince (late) < 10000)
        {
            juce::Thread::sleep (100);

            if (const auto now = captures(); now != count)
            {
                count = now;
                lastChange = Clock::now();
            }
        }
    }

    const auto held = AppCaptureListener::heldCaptures();
    o.captured = ! held.empty();
    say ("Captures in: " + std::to_string (held.size()));

    // 8. Whether Pro Tools rendered: the capture, the Undo item naming AudioSuite where it did not
    //    before, or more clips. The Undo item can take a moment to change, so it is read a few times.
    std::string undoAfter;
    {
        const auto look = Clock::now();
        bool enabledAfter = false;

        do
        {
            if (driver.readUndo (undoAfter, enabledAfter).ok
                && juce::String (undoAfter).containsIgnoreCase ("AudioSuite") && enabledAfter)
                break;

            juce::Thread::sleep (200);
        }
        while (millisecondsSince (look) < 10000);

        say ("After Render the Undo item reads \"" + undoAfter + "\"");
    }

    const bool undoNamesAudioSuite = juce::String (undoAfter).containsIgnoreCase ("AudioSuite");
    const bool undoChanged = undoNamesAudioSuite && undoAfter != undoBefore;
    const auto afterRender = readSession (client);
    say ("After Render: " + afterRender.text());
    const bool moreClips = before.read && afterRender.read && afterRender.clips > before.clips;

    o.rendered = o.captured || undoChanged || moreClips;

    // The top step in Pro Tools' undo list must be this render, not an earlier AudioSuite step:
    // the session shows it (more clips, or an Undo item that changed), or, when the session cannot
    // be read, the capture came in.
    const bool renderIsLastStep = moreClips || undoChanged || (! before.read && o.captured);

    say (std::string ("Rendered: ") + (o.rendered ? "yes" : "no") + " (capture " + (o.captured ? "yes" : "no") + ", Undo item changed "
         + (undoChanged ? "yes" : "no") + ", more clips " + (moreClips ? "yes" : "no") + ")");

    if (! o.rendered || ! renderIsLastStep)
    {
        closeIfOpenedHere();

        if (! o.rendered)
            return fail (undoNamesAudioSuite
                             ? "Tempo Tantrum Capture sent nothing, and it is not clear whether Pro Tools rendered. Nothing was undone; check the session and press Cmd+Z in Pro Tools if a rendered clip is there"
                             : "Pro Tools did not render the selection with Tempo Tantrum Capture. Nothing was undone");

        return fail ("The capture came in, but the session does not show the render as the last step, so nothing was undone. "
                     "Check the session and press Cmd+Z in Pro Tools if a rendered clip is there");
    }

    // 9. Undo the render, once, only as an AudioSuite step.
    std::string pressedTitle;

    if (auto r = driver.undoNaming ("AudioSuite", window, pressedTitle); ! r.ok)
    {
        closeIfOpenedHere();
        return fail (r.message + ". The rendered clip is still in the session: press Cmd+Z in Pro Tools to remove it");
    }

    o.undone = true;

    // 10. The session as before, read through the scripting link.
    if (before.read)
    {
        const auto check = Clock::now();
        SessionReading after;

        do
        {
            after = readSession (client);

            if (after.sameAs (before))
                break;

            juce::Thread::sleep (250);
        }
        while (millisecondsSince (check) < 5000);

        o.confirmed = after.sameAs (before);
        say ("After Undo: " + after.text() + (o.confirmed ? ", the same as before" : ", NOT the same as before"));
    }
    else
    {
        say ("The session could not be read through the scripting link, so the Undo is not confirmed");
    }

    client.disconnect();

    // 11. The window, when it was opened here.
    const bool closed = closeIfOpenedHere();

    if (! o.captured)
        return fail ("Pro Tools rendered and the render was undone, but Tempo Tantrum Capture sent nothing. Is it the latest version?");

    if (held.size() > 1)
        return fail ("Pro Tools rendered the selection in " + std::to_string (held.size()) + " passes (multi-mono, or clip by clip), so "
                     "nothing was opened. The render was undone. Set the AudioSuite window to render the whole selection as one file and try again");

    if (before.read && ! o.confirmed)
        return fail ("The render was undone, but the session does not read as it did before. Check the clips and the selection in Pro Tools");

    // The one capture opens, placed against the edit selection that was rendered.
    audiosuite::EditSelection rendered;

    if (before.read)
    {
        rendered.known = true;
        rendered.in = before.in;
        rendered.out = before.out;
    }
    else
    {
        rendered.problem = "the scripting link could not be read";
    }

    AppCaptureListener::stopHolding (held.front(), rendered, juce::String::fromUTF8 (before.sessionName.c_str()));
    handedOver = true;

    o.ok = true;

    if (! closed)
        say ("Done, but the AudioSuite window was left open");

    say ("Done: captured, the render undone" + std::string (o.confirmed ? " and confirmed" : " (not confirmed)"));
    return o;
}

//==============================================================================
AudioSuiteJob::AudioSuiteJob() : juce::Thread ("Get from Pro Tools (AudioSuite)") {}

AudioSuiteJob::~AudioSuiteJob()
{
    alive->store (false);
    // After Render the run goes on until the render is undone. Every wait in it is bounded: the
    // capture wait and the wait for Pro Tools' windows are each at most longestCaptureWaitMs, and
    // the steps after them take well under a minute.
    stopThread (2 * longestCaptureWaitMs + 120000);
}

bool AudioSuiteJob::start (Task newTask, std::function<void (const juce::String&)> onProgress,
                           std::function<void (const AudioSuiteOutcome&)> onDone)
{
    if (isThreadRunning())
        return false;

    task = newTask;
    progressCallback = std::move (onProgress);
    doneCallback = std::move (onDone);
    return startThread();
}

void AudioSuiteJob::run()
{
    const auto flag = alive;
    const auto log = [] (const std::string& line) { appendToLog ({ line }); };
    const auto stop = [this] { return threadShouldExit(); };

    const auto outcome = task == Task::look
        ? findCapturePlugIn (log, stop)
        : renderWithCapturePlugIn (log,
                                   [this, flag] (const juce::String& line)
                                   {
                                       juce::MessageManager::callAsync ([this, flag, line]
                                       {
                                           if (flag->load() && progressCallback)
                                               progressCallback (line);
                                       });
                                   }, stop);

    juce::MessageManager::callAsync ([this, flag, outcome]
    {
        if (flag->load() && doneCallback)
            doneCallback (outcome);
    });
}

} // namespace tt::capture

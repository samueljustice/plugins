#include <JuceHeader.h>

#include "MainComponent.h"
#include "ProToolsTestPanel.h"
#include "ProToolsWrite.h"
#include "protools/IdentifyBeatTest.h"
#include "protools/IdentifyBeatWrite.h"
#include "ptsl/PtslClient.h"
#include "ui/Theme.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace tt
{

//==============================================================================
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow()
        : juce::DocumentWindow (JUCE_APPLICATION_NAME_STRING, ui::Theme::bg(), juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent (true), true);
        setResizable (true, false);
        setResizeLimits (900, 560, 16384, 16384);
        centreWithSize (1280, 760);
        setVisible (true);

        if (auto* content = getContentComponent())
            content->grabKeyboardFocus();
    }

    MainComponent* getMainComponent() const
    {
        return dynamic_cast<MainComponent*> (getContentComponent());
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

//==============================================================================
class TempoTantrumApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return true; }

    void initialise (const juce::String&) override
    {
        lookAndFeel = std::make_unique<ui::TantrumLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        const auto args = getCommandLineParameterArray();

        if (args.contains ("--snapshot") || args.contains ("--test-panel-snapshot") || args.contains ("--write-sheet-snapshot"))
        {
            runSnapshot (args, args.contains ("--test-panel-snapshot"), args.contains ("--write-sheet-snapshot"));
            return;
        }

        if (args.contains ("--write-test"))
        {
            runWriteTestHeadless();
            return;
        }

        if (args.contains ("--ptsl-probe"))
        {
            runPtslProbe();
            return;
        }

        if (args.contains ("--ax-selfcheck"))
        {
            runAccessibilityCheck();
            return;
        }

        if (args.contains ("--ax-menu-check"))
        {
            runMenuCheck (args);
            return;
        }

        if (args.contains ("--identify-beat-test"))
        {
            runIdentifyBeatTestHeadless (args);
            return;
        }

        if (args.contains ("--snapshot-empty"))
        {
            runEmptySnapshot (args);
            return;
        }

        if (args.contains ("--selftest"))
        {
            const int at = args.indexOf ("--selftest");
            const bool passed = at + 1 < args.size()
                                && runSelfTest (juce::File::getCurrentWorkingDirectory().getChildFile (args[at + 1]));
            std::cout << (passed ? "PASS" : "FAIL") << std::endl;
            setApplicationReturnValue (passed ? 0 : 1);
            quit();
            return;
        }

        mainWindow = std::make_unique<MainWindow>();

        // A file path as the only argument opens that file.
        if (args.size() == 1)
            openFile (args[0]);
    }

    void shutdown() override
    {
        if (headlessRun.joinable())
            headlessRun.join();

        mainWindow = nullptr;
        snapshot = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel = nullptr;
    }

    // Quitting, or closing the window, asks first whether to save a map with unsaved changes.
    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            if (auto* main = mainWindow->getMainComponent())
            {
                main->askToSaveThen ([] { juce::JUCEApplicationBase::quit(); });
                return;
            }

        quit();
    }

    // macOS passes files opened from the Finder (or dropped on the Dock icon) here.
    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        const auto args = juce::StringArray::fromTokens (commandLine, true);

        for (auto arg : args)
            if (openFile (arg.unquoted()))
                break;
    }

private:
    std::unique_ptr<ui::TantrumLookAndFeel> lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;

    std::unique_ptr<MainComponent> snapshot;
    juce::File snapshotOut;
    bool printView = false;
    std::thread headlessRun;

    bool openFile (const juce::String& path)
    {
        if (mainWindow == nullptr || path.isEmpty() || path.startsWith ("-"))
            return false;

        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (path);

        if (! file.existsAsFile())
            return false;

        if (auto* main = mainWindow->getMainComponent())
            main->openMusic (file);

        return true;
    }

    //==============================================================================
    // --snapshot <audio file> <out.png> [--drums <file>] [--bar N] [--bpm X] [--width W --height H]
    // Maps the file, lays the window's contents out at the given size and saves them as a PNG at
    // twice that size, then quits. No window is opened and no audio device is used.
    //
    // What is in view can be set up first, in this order:
    //   --view-start S --view-end S   the stretch in view, in seconds (0 and a large number: the whole file)
    //   --select-pin N                selects a pin, counted from 0
    //   --snap hits|grid|off          the Snap setting
    //   --wave-height X               draws the waveform X times taller
    //   --keys "t t opt+f shift+."    presses these keys, one after the other (see keyFromName)
    //   --drag-pin N --drag-to S      shows pin N part-way through a drag to S seconds
    //   --print-view                  prints the stretch in view, the selected pin and the playhead
    //
    // --test-panel-snapshot takes the same arguments and has the Pro Tools test panel open, showing
    // the log of a Look only run that looked for an application that is not running. With
    // --permission yes or --permission no the panel shows the Accessibility permission that way.
    //
    // --write-sheet-snapshot takes the same arguments and has the Write to Pro Tools sheet open:
    //   --state ready|permission|notrunning|running|checking|finished|stopped   what the sheet shows (ready)
    //   --link connected|multimono|nothing|mismatch|nosession|none   what the scripting link says about the session (none)
    //   --rest-changed, --rest-partbeat, --restore-failed   with --state finished --link connected: how the run ended
    //   --more                        with More opened
    //   --float <out.png>            also saves the floating panel of a run as a picture
    void runSnapshot (const juce::StringArray& args, bool withTestPanel, bool withWriteSheet)
    {
        const int at = args.indexOf (withTestPanel ? "--test-panel-snapshot" : withWriteSheet ? "--write-sheet-snapshot" : "--snapshot");
        const auto valueAfter = [&args] (const juce::String& flag) -> juce::String
        {
            const int i = args.indexOf (flag);
            return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String();
        };

        if (at + 2 >= args.size())
            return failSnapshot ("Usage: --snapshot <audio file> <out.png> [--drums <file>] [--bar N] [--bpm X] [--width W --height H]");

        const auto cwd = juce::File::getCurrentWorkingDirectory();
        const auto audioFile = cwd.getChildFile (args[at + 1]);
        snapshotOut = cwd.getChildFile (args[at + 2]);

        const int width = valueAfter ("--width").isNotEmpty() ? valueAfter ("--width").getIntValue() : 1280;
        const int height = valueAfter ("--height").isNotEmpty() ? valueAfter ("--height").getIntValue() : 760;

        if (width < 200 || height < 200)
            return failSnapshot ("The snapshot must be at least 200 by 200");

        snapshot = std::make_unique<MainComponent> (false);
        auto& doc = snapshot->getDocument();
        doc.setSynchronous (true);

        if (doc.loadMusic (audioFile).failed() || ! doc.hasMusic())
            return failSnapshot ("Could not open " + audioFile.getFullPathName());

        if (const auto drums = valueAfter ("--drums"); drums.isNotEmpty())
        {
            const auto result = doc.loadDrumStem (cwd.getChildFile (drums));
            if (result.failed())
                return failSnapshot (result.getErrorMessage());
        }

        if (const auto bar = valueAfter ("--bar"); bar.isNotEmpty())
            doc.setFirstHitBar (bar.getIntValue());

        if (const auto bpm = valueAfter ("--bpm"); bpm.isNotEmpty())
            doc.setTempo (bpm.getDoubleValue());

        snapshot->setSize (width, height);
        snapshot->refresh();
        setUpSnapshotView (args);

        if (withTestPanel)
        {
            snapshot->showProToolsTest();
            auto& panel = snapshot->getProToolsTest();

            if (const auto permission = valueAfter ("--permission"); permission.isNotEmpty())
                panel.showPermissionAs (permission == "yes");

            pt::TestSettings settings;
            settings.appVersion = JUCE_APPLICATION_VERSION_STRING;
            settings.musicName = audioFile.getFileName().toStdString();
            settings.bundleIdentifier = notRunningBundleIdentifier;

            juce::String log;
            const auto outcome = pt::runIdentifyBeatTest (settings,
                                                          [&log] (const std::string& line) { log << juce::String::fromUTF8 (line.c_str()) << "\n"; },
                                                          [] { return false; });
            panel.showFinishedRun (log, outcome);
        }

        if (withWriteSheet)
            setUpWriteSheet (args, audioFile.getFileName());

        // A moment for anything the view prepares asynchronously, then the picture.
        juce::Timer::callAfterDelay (500, [this] { finishSnapshot(); });
    }

    // A key by name, for --keys: "t", "return", "tab", "left", "right", "space", "delete", "f2",
    // "," and so on, with "cmd+", "opt+" and "shift+" before it in any order.
    static juce::KeyPress keyFromName (const juce::String& name)
    {
        auto parts = juce::StringArray::fromTokens (name.toLowerCase(), "+", "");
        parts.removeEmptyStrings();

        if (parts.isEmpty())
            return {};

        const auto keyName = parts[parts.size() - 1];
        parts.remove (parts.size() - 1);
        int modifiers = 0;

        for (const auto& part : parts)
            modifiers |= part == "cmd" ? juce::ModifierKeys::commandModifier
                       : part == "opt" ? juce::ModifierKeys::altModifier
                       : part == "shift" ? juce::ModifierKeys::shiftModifier : 0;

        const int code = keyName == "return" ? juce::KeyPress::returnKey
                       : keyName == "tab" ? juce::KeyPress::tabKey
                       : keyName == "left" ? juce::KeyPress::leftKey
                       : keyName == "right" ? juce::KeyPress::rightKey
                       : keyName == "space" ? juce::KeyPress::spaceKey
                       : keyName == "delete" ? juce::KeyPress::deleteKey
                       : keyName == "escape" ? juce::KeyPress::escapeKey
                       : keyName == "f2" ? juce::KeyPress::F2Key
                       : keyName == "f3" ? juce::KeyPress::F3Key
                       : keyName == "f4" ? juce::KeyPress::F4Key
                       : (int) juce::CharacterFunctions::toUpperCase (keyName[0]);

        return juce::KeyPress (code, juce::ModifierKeys (modifiers), 0);
    }

    // The view options of --snapshot.
    void setUpSnapshotView (const juce::StringArray& args)
    {
        auto& view = snapshot->getWaveform();

        if (const auto start = valueAfterFlag (args, "--view-start"), end = valueAfterFlag (args, "--view-end");
            start.isNotEmpty() && end.isNotEmpty())
            view.showRange (start.getDoubleValue(), end.getDoubleValue());

        if (const auto pin = valueAfterFlag (args, "--select-pin"); pin.isNotEmpty())
            view.setSelectedPin (pin.getIntValue());

        if (const auto snap = valueAfterFlag (args, "--snap"); snap.isNotEmpty())
            snapshot->keyPressed (keyFromName (snap == "off" ? "f2" : snap == "grid" ? "f4" : "f3"));

        if (const auto height = valueAfterFlag (args, "--wave-height"); height.isNotEmpty())
            view.setWaveHeight (height.getDoubleValue());

        for (const auto& name : juce::StringArray::fromTokens (valueAfterFlag (args, "--keys"), " ", ""))
            if (name.isNotEmpty())
                snapshot->keyPressed (keyFromName (name));

        if (const auto pin = valueAfterFlag (args, "--drag-pin"), to = valueAfterFlag (args, "--drag-to");
            pin.isNotEmpty() && to.isNotEmpty())
            view.showDragForSnapshot (pin.getIntValue(), to.getDoubleValue());

        printView = args.contains ("--print-view");
    }

    // The states of the Write to Pro Tools sheet, for --write-sheet-snapshot. Nothing is run and
    // Pro Tools is not looked for: each state is made up from the pins of the open music.
    void setUpWriteSheet (const juce::StringArray& args, const juce::String& musicName)
    {
        const auto state = valueAfterFlag (args, "--state");
        const auto& pins = snapshot->getDocument().getMap().pins;
        const int total = (int) pins.size();
        auto& sheet = snapshot->getProToolsWrite();

        sheet.showPermissionAs (state != "permission");
        snapshot->showProToolsWrite();
        sheet.showMoreOpen (args.contains ("--more"));

        pt::CounterReading reading;
        reading.running = state != "notrunning";
        reading.read = reading.isSamples = reading.running;
        reading.sample = 2880000;
        reading.text = "2880000";
        sheet.showCounterAs (reading);

        // What the scripting link says, made up: --link connected|multimono|nothing|mismatch|nosession|none (none).
        {
            const auto linkState = valueAfterFlag (args, "--link");
            const auto& doc = snapshot->getDocument();
            pt::SessionReading link;

            if (linkState.isEmpty() || linkState == "none" || state == "notrunning")
            {
                link.problem = ptsl::Client::hasLink() ? "Pro Tools did not answer on its scripting port. Is Pro Tools running, with a session open?"
                                                       : ptsl::noLinkMessage;
            }
            else
            {
                link.linked = true;
                link.sessionOpen = linkState != "nosession";
                link.problem = link.sessionOpen ? std::string() : std::string ("No session is open in Pro Tools");
                link.sessionName = "Trailer Session";
                link.sampleRate = linkState == "mismatch" ? 44100.0 : doc.getSampleRate();
                link.lengthSeconds = 600.0;
                link.transport = linkState == "nothing" ? "TS_TransportPlaying" : "TS_TransportStopped";
                link.stopped = linkState != "nothing";
                link.counterType = linkState == "connected" ? "TLType_BarsBeats" : "TLType_Samples";
                link.counterIsSamples = linkState != "connected";
                link.hasSelection = link.hasMusicStart = true;
                link.selectionIn = link.musicStart = linkState == "nothing" ? 1440000 : 2880000;
                link.selectionOut = linkState == "nothing" ? link.selectionIn
                                                           : link.selectionIn + (long long) std::llround ((double) doc.getLengthInSamples()
                                                                                                         * link.sampleRate / doc.getSampleRate());

                if (linkState == "connected")
                {
                    link.selectedFiles = { musicName.toStdString() };
                    link.fileIsMusic = true;
                    link.trim = pt::Trim::whole;
                }
                else if (linkState == "multimono")
                {
                    // A stereo clip made of two mono files.
                    const auto base = juce::File::createFileWithoutCheckingPath (musicName).getFileNameWithoutExtension().toStdString();
                    link.selectedFiles = { base + ".L.wav", base + ".R.wav" };
                    link.fileIsMusic = true;
                    link.trim = pt::Trim::whole;
                }
                else if (linkState == "mismatch")
                {
                    link.selectedFiles = { "Other Cue v2.wav" };
                    link.trim = pt::Trim::unknown;
                }
            }

            link.selectedClips = pt::clipsOf (link.selectedFiles);
            sheet.showLinkAs (link);
        }

        const int seventh = std::min (6, std::max (0, total - 1));

        if (state == "running" || state == "checking")
            sheet.showRunning (state == "running" ? pt::Phase::writing : pt::Phase::checking, seventh, total);

        if (state == "finished" || state == "stopped")
        {
            // The log of a run that found no application to drive stands in for a real one.
            pt::WriteSettings settings;
            settings.pins = pins;
            settings.sessionSampleRate = snapshot->getDocument().getSampleRate();
            settings.appVersion = JUCE_APPLICATION_VERSION_STRING;
            settings.musicName = musicName.toStdString();
            settings.bundleIdentifier = notRunningBundleIdentifier;

            juce::String log;
            auto outcome = pt::runWrite (settings,
                                         [&log] (const std::string& line) { log << juce::String::fromUTF8 (line.c_str()) << "\n"; },
                                         [] { return false; }, {});
            outcome.hasMusicStart = true;
            outcome.musicStart = reading.sample;

            if (state == "finished")
            {
                outcome.ok = true;
                outcome.written = total;
                outcome.checked = true;
                outcome.checkedPins = total;
                outcome.summary = "Written: " + std::to_string (total) + (total == 1 ? " pin" : " pins");
                outcome.checkSummary = total == 1 ? std::string ("The pin checks out")
                                                  : "All " + std::to_string (total) + " pins check out";

                // A run through the scripting link, made up.
                if (valueAfterFlag (args, "--link") == "connected")
                {
                    outcome.linked = outcome.movedWithLink = true;
                    outcome.moveNote = "Identify Beat followed the cursor moves made through the scripting link";
                    outcome.checkSummary += " (largest error 1 sample)";
                    outcome.restChanged = args.contains ("--rest-changed") || args.contains ("--rest-partbeat");
                    outcome.restSummary = args.contains ("--rest-changed")
                                              ? "Rest of the session: the tempo after the music changed between 3:12 and 3:40. Check it, or Undo."
                                          : args.contains ("--rest-partbeat")
                                              ? "Rest of the session: the beats after the music moved 300 ticks (0.31 of a beat) later, so they no "
                                                "longer sit on the same audio. Check it, or Undo."
                                              : "Rest of the session: tempo unchanged. The bar numbers after the music moved 2 bars later";
                    outcome.restoreFailed = args.contains ("--restore-failed");
                    outcome.restoredNote = outcome.restoreFailed
                                               ? "A Pro Tools dialog may still be open, so nothing was put back. Set the Main Counter back to "
                                                 "Bars|Beats. Switch Link Timeline and Edit Selection off again."
                                               : "The selection is as it was before the run. Link Timeline and Edit Selection is off again. "
                                                 "The Main Counter shows Bars|Beats again.";
                }
            }
            else
            {
                outcome.failedPin = seventh + 1;
                outcome.written = seventh;
                outcome.reason = "Pro Tools stopped being the front application";
                outcome.summary = "Stopped at pin " + std::to_string (outcome.failedPin) + " of " + std::to_string (total)
                                  + ": " + outcome.reason;
            }

            sheet.showFinished (outcome, log);
        }

        if (const auto floatOut = valueAfterFlag (args, "--float"); floatOut.isNotEmpty())
        {
            const auto image = ProToolsWriteSheet::floatingPanelPicture (state == "checking" ? pt::Phase::checking : pt::Phase::writing,
                                                                         seventh, total);
            const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (floatOut);
            file.deleteFile();

            if (auto stream = file.createOutputStream())
            {
                juce::PNGImageFormat png;

                if (png.writeImageToStream (image, *stream))
                    std::cout << "Wrote " << file.getFullPathName() << " (" << image.getWidth() << " x " << image.getHeight() << ")" << std::endl;
            }
        }
    }

    // --snapshot-empty <out.png> [--width W --height H]
    // The window's contents with no music open, saved the same way.
    void runEmptySnapshot (const juce::StringArray& args)
    {
        const int at = args.indexOf ("--snapshot-empty");
        const auto valueAfter = [&args] (const juce::String& flag) -> juce::String
        {
            const int i = args.indexOf (flag);
            return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String();
        };

        if (at + 1 >= args.size())
            return failSnapshot ("Usage: --snapshot-empty <out.png> [--width W --height H]");

        snapshotOut = juce::File::getCurrentWorkingDirectory().getChildFile (args[at + 1]);

        const int width = valueAfter ("--width").isNotEmpty() ? valueAfter ("--width").getIntValue() : 1280;
        const int height = valueAfter ("--height").isNotEmpty() ? valueAfter ("--height").getIntValue() : 760;

        if (width < 200 || height < 200)
            return failSnapshot ("The snapshot must be at least 200 by 200");

        snapshot = std::make_unique<MainComponent> (false);
        snapshot->setSize (width, height);
        snapshot->refresh();
        juce::Timer::callAfterDelay (200, [this] { finishSnapshot(); });
    }

    //==============================================================================
    // An identifier no application has, for runs that must find nothing to drive.
    static constexpr const char* notRunningBundleIdentifier = "com.samueljustice.tempotantrum.nothing";

    static juce::String valueAfterFlag (const juce::StringArray& args, const juce::String& flag)
    {
        const int i = args.indexOf (flag);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String();
    }

    // --ptsl-probe [--raw]
    // Connects to Pro Tools' scripting link, registers, and sends only commands that read: the
    // session's name, rate, start and length, the transport, the Main Counter's format, the edit
    // and timeline selections, the edit mode options, the files of the selected clips, a few time
    // conversions and the number of tracks. Prints each command, what came back and how long it
    // took. Changes nothing in Pro Tools. With --raw the JSON answers are printed too (file paths
    // excepted).
    void runPtslProbe()
    {
        const bool raw = getCommandLineParameterArray().contains ("--raw");

        headlessRun = std::thread ([this, raw]
        {
            ptsl::Client client;
            int failures = 0;

            const auto show = [&] (const char* name, const ptsl::Reply& reply, const std::string& answer, bool printRaw = true)
            {
                char time[32];
                std::snprintf (time, sizeof (time), "%7.1f ms", reply.seconds * 1000.0);
                std::cout << time << "  " << name << ": ";

                if (reply.ok)
                    std::cout << answer << std::endl;
                else
                {
                    ++failures;
                    std::cout << "FAILED: " << reply.message << (reply.timedOut ? " (timed out)" : "") << std::endl;
                }

                if (raw && printRaw && ! reply.body.empty())
                    std::cout << "            raw: " << reply.body << std::endl;

                if (raw && ! reply.error.empty())
                    std::cout << "            error: " << reply.error << std::endl;
            };

            std::cout << "Pro Tools Scripting link: " << (ptsl::Client::hasLink() ? "built in" : ptsl::noLinkMessage) << std::endl;
            std::string problem;
            const auto start = std::chrono::steady_clock::now();
            const bool connected = client.connect (problem);
            std::cout << "Connect: " << (connected ? "connected" : problem) << " ("
                      << juce::String (std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count(), 1)
                      << " ms)" << std::endl;

            if (connected)
            {
                const auto registered = client.registerConnection ("Sammy J", "Tempo Tantrum");
                show ("RegisterConnection", registered, client.isRegistered() ? "registered, session id received" : "no session id", false);

                const auto ready = client.hostReadyCheck();
                show ("HostReadyCheck", ready, (bool) juce::JSON::parse (juce::String::fromUTF8 (ready.body.c_str())).getProperty ("is_host_ready", false)
                                                   ? "ready" : "not ready");

                const auto version = client.ptslVersion();
                show ("GetPTSLVersion", version.reply, version.value);

                const auto name = client.sessionName();
                show ("GetSessionName", name.reply, "\"" + name.value + "\"");

                const auto rate = client.sessionSampleRate();
                show ("GetSessionSampleRate", rate.reply, juce::String (rate.value).toStdString());

                const auto sessionStart = client.sessionStartTime();
                show ("GetSessionStartTime", sessionStart.reply, "\"" + sessionStart.value + "\"");

                const auto length = client.sessionLength();
                show ("GetSessionLength", length.reply, "\"" + length.value + "\"");

                const auto transport = client.transportState();
                show ("GetTransportState", transport.reply, transport.value);

                const auto counter = client.mainCounterFormat();
                show ("GetMainCounterFormat", counter.reply, counter.value.type + " (" + counter.value.setting + ")");

                const auto edit = client.editSelection ("TLType_Samples");
                show ("GetEditSelection (samples)", edit.reply, "in " + edit.value.in + ", out " + edit.value.out);

                const auto timeline = client.timelineSelection ("TLType_Samples");
                show ("GetTimelineSelection (samples)", timeline.reply, "in " + timeline.value.in + ", out " + timeline.value.out);

                const auto options = client.editModeOptions();
                std::string optionText;

                for (const auto& [key, on] : options.value.options)
                    optionText += (optionText.empty() ? "" : ", ") + key + "=" + (on ? "on" : "off");

                show ("GetEditModeOptions", options.reply, optionText);

                const auto files = client.fileLocations ("SelectedClipsTimeline");
                std::string fileText = std::to_string (files.value.size()) + (files.value.size() == 1 ? " file" : " files");

                for (const auto& file : files.value)
                    fileText += ", \"" + juce::File (juce::String::fromUTF8 (file.path.c_str())).getFileName().toStdString() + "\""
                                + (file.online ? " online" : " offline") + " id " + file.fileId;

                show ("GetFileLocation (selected clips on the timeline)", files.reply, fileText, false);

                // Time conversions at the edit selection's start, and back.
                const auto at = edit.ok() && ! edit.value.in.empty() ? edit.value.in : std::string ("0");
                const auto bars = client.timeAsType (at, "TLType_Samples", "TLType_BarsBeats");
                show ("GetTimeAsType (samples to bars|beats)", bars.reply, at + " -> \"" + bars.value + "\"");

                const auto ticks = client.timeAsType (at, "TLType_Samples", "TLType_Ticks");
                show ("GetTimeAsType (samples to ticks)", ticks.reply, at + " -> \"" + ticks.value + "\"");

                if (bars.ok())
                {
                    const auto back = client.timeAsType (bars.value, "TLType_BarsBeats", "TLType_Samples");
                    show ("GetTimeAsType (bars|beats to samples)", back.reply, "\"" + bars.value + "\" -> " + back.value);
                }

                for (const auto* location : { "1|1|000", "1|2|000", "2|1|000" })
                {
                    const auto converted = client.timeAsType (location, "TLType_BarsBeats", "TLType_Ticks");
                    show ("GetTimeAsType (bars|beats to ticks)", converted.reply, std::string (location) + " -> \"" + converted.value + "\"");
                }

                const auto secondsAt = client.timeAsType (at, "TLType_Samples", "TLType_MinSecs");
                show ("GetTimeAsType (samples to min:sec)", secondsAt.reply, at + " -> \"" + secondsAt.value + "\"");

                const auto zero = client.timeAsType ("0", "TLType_Samples", "TLType_TimeCode");
                show ("GetTimeAsType (sample 0 to timecode)", zero.reply, "0 -> \"" + zero.value + "\"");

                if (sessionStart.ok())
                    for (const auto* type : { "TLType_TimeCode", "TLType_MinSecs" })
                    {
                        const auto converted = client.timeAsType (sessionStart.value, type, "TLType_Samples");
                        show ("GetTimeAsType (session start to samples)", converted.reply,
                              "\"" + sessionStart.value + "\" as " + type + " -> " + converted.value);
                    }

                if (length.ok())
                    for (const auto* type : { "TLType_TimeCode", "TLType_MinSecs" })
                    {
                        const auto converted = client.timeAsType (length.value, type, "TLType_Samples");
                        show ("GetTimeAsType (session length to samples)", converted.reply,
                              "\"" + length.value + "\" as " + type + " -> " + converted.value);
                    }

                // How long a run of conversions takes, as the tempo check before and after a write makes.
                {
                    const auto from = std::chrono::steady_clock::now();
                    int good = 0;
                    constexpr int count = 50;

                    for (int i = 0; i < count; ++i)
                        good += client.timeAsType (std::to_string (i * 96000), "TLType_Samples", "TLType_Ticks").ok() ? 1 : 0;

                    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - from).count();
                    std::cout << juce::String (ms, 1) << " ms  " << count << " conversions (samples to ticks), " << good << " answered, "
                              << juce::String (ms / count, 2) << " ms each" << std::endl;
                }

                const auto tracks = client.trackCount();
                show ("GetTrackList", tracks.reply, std::to_string (tracks.value) + " tracks", false);
            }

            const bool ok = connected && failures == 0;
            std::cout << (ok ? "Every command answered" : "Not every command answered") << std::endl;

            juce::MessageManager::callAsync ([this, ok]
            {
                setApplicationReturnValue (ok ? 0 : 1);
                quit();
            });
        });
    }

    // --ax-selfcheck
    // Prints whether macOS lets this program control other applications and whether Pro Tools is
    // running, then quits. Touches no other application and shows no prompt.
    void runAccessibilityCheck()
    {
        std::cout << "Accessibility permission: " << (pt::Driver::accessibilityAllowed() ? "allowed" : "missing") << std::endl;
        std::cout << "Posting key events: " << (pt::Driver::keyEventsAllowed() ? "allowed" : "not allowed") << std::endl;
        std::cout << "Pro Tools (" << pt::Driver::defaultBundleIdentifier << "): "
                  << (pt::Driver::isRunning() ? "running" : "not running") << std::endl;
        std::cout << "macOS " << pt::Driver::systemVersion() << std::endl;
        quit();
    }

    // --ax-menu-check <bundle identifier> <menu> <item>
    // Finds a menu item of a running application through Accessibility and prints what it is.
    // Reads only: the application is not brought forward and nothing is pressed.
    void runMenuCheck (const juce::StringArray& args)
    {
        const int at = args.indexOf ("--ax-menu-check");

        if (at + 3 >= args.size())
        {
            std::cerr << "Usage: --ax-menu-check <bundle identifier> <menu> <item>" << std::endl;
            setApplicationReturnValue (1);
            quit();
            return;
        }

        pt::Driver driver ([] (const std::string& line) { std::cout << line << std::endl; }, [] { return false; });
        driver.setBundleIdentifier (args[at + 1].toStdString());
        bool ok = false;

        if (! pt::Driver::accessibilityAllowed())
        {
            std::cout << "Accessibility permission is missing, so the menus cannot be read" << std::endl;
        }
        else if (driver.findProTools().running)
        {
            pt::Element item;
            bool enabled = false;
            const auto result = driver.findMenuItem ({ args[at + 2].toStdString(), args[at + 3].toStdString() }, item, enabled);
            ok = result.ok;

            if (result.ok)
                std::cout << driver.describe (item, 1) << std::endl;
            else
                std::cout << "Not found: " << result.message << std::endl;

            driver.logWindows();
        }

        setApplicationReturnValue (ok ? 0 : 1);
        quit();
    }

    // --identify-beat-test look [--app <bundle identifier>]
    // Runs the Look only test without the window and prints its log. With Pro Tools running this
    // brings Pro Tools forward and opens and cancels its Identify Beat dialog, as the button does.
    // --app none looks for an application that is not running.
    void runIdentifyBeatTestHeadless (const juce::StringArray& args)
    {
        if (valueAfterFlag (args, "--identify-beat-test") != "look")
        {
            std::cerr << "Usage: --identify-beat-test look [--app <bundle identifier>]" << std::endl;
            setApplicationReturnValue (1);
            quit();
            return;
        }

        pt::TestSettings settings;
        settings.appVersion = JUCE_APPLICATION_VERSION_STRING;

        if (const auto app = valueAfterFlag (args, "--app"); app.isNotEmpty())
            settings.bundleIdentifier = app == "none" ? std::string (notRunningBundleIdentifier) : app.toStdString();

        // On its own thread, as in the window: the message thread keeps running meanwhile.
        headlessRun = std::thread ([this, settings]
        {
            const auto outcome = pt::runIdentifyBeatTest (settings,
                                                          [] (const std::string& line) { std::cout << line << std::endl; },
                                                          [] { return false; });

            juce::MessageManager::callAsync ([this, ok = outcome.ok]
            {
                setApplicationReturnValue (ok ? 0 : 1);
                quit();
            });
        });
    }

    // --write-test
    // Runs the write to Pro Tools without the window, against an application that is not running,
    // and prints its log. It sends three made-up pins, so it never looks for Pro Tools itself.
    void runWriteTestHeadless()
    {
        pt::WriteSettings settings;
        settings.appVersion = JUCE_APPLICATION_VERSION_STRING;
        settings.musicName = "made-up pins";
        settings.sessionSampleRate = 48000.0;
        settings.bundleIdentifier = notRunningBundleIdentifier;

        // Bars 40, 44 and 48 at 120 bpm.
        for (int i = 0; i < 3; ++i)
            settings.pins.push_back ({ 156.0 + 16.0 * i, 1.0 + 8.0 * i, false, i == 0 });

        headlessRun = std::thread ([this, settings]
        {
            const auto outcome = pt::runWrite (settings,
                                               [] (const std::string& line) { std::cout << line << std::endl; },
                                               [] { return false; },
                                               [] (pt::Phase, int, int) {});

            juce::MessageManager::callAsync ([this, outcome]
            {
                std::cout << "Outcome: ok=" << (outcome.ok ? "yes" : "no") << " written=" << outcome.written
                          << " of " << outcome.total << " summary=\"" << outcome.summary << "\"" << std::endl;

                // The run is meant to stop before it touches anything.
                setApplicationReturnValue (! outcome.ok && outcome.written == 0 ? 0 : 1);
                quit();
            });
        });
    }

    //==============================================================================
    // --selftest <audio file>
    // Works on a copy of the file in a temporary folder: maps it, makes two edits, changes the first
    // bar, adds the copy as its own drum stem, saves the map, opens the copy again and compares what
    // came back, then exports the click and reads it back.
    // Prints one line per check and PASS or FAIL. No window is opened and no audio device is used.
    static bool runSelfTest (const juce::File& audioFile)
    {
        bool allPassed = true;
        const auto check = [&allPassed] (bool passed, const juce::String& what)
        {
            std::cout << (passed ? "  ok    " : "  FAIL  ") << what << std::endl;
            allPassed = allPassed && passed;
        };

        if (! audioFile.existsAsFile())
        {
            check (false, "the audio file exists");
            return false;
        }

        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("TempoTantrumSelfTest-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
        folder.createDirectory();
        const auto music = folder.getChildFile ("music" + audioFile.getFileExtension());
        check (audioFile.copyFileTo (music), "copied the audio to a temporary folder");

        MapInputs savedInputs;
        std::vector<Pin> savedPins;
        std::shared_ptr<const std::vector<float>> savedClick;
        double savedRate = 0.0;
        juce::int64 savedLength = 0;

        {
            Document doc;
            doc.setSynchronous (true);
            check (doc.loadMusic (music).wasOk() && doc.hasMusic(), "opened the music");
            check (doc.hasMap() && doc.getMap().pins.size() >= 2, "mapped it: " + juce::String ((int) doc.getMap().pins.size()) + " pins");

            // Two edits: the last pin moved 4 ms earlier, and the second section's start marked as a restart.
            if (doc.hasMap() && doc.getMap().pins.size() >= 2)
            {
                const auto map = doc.getMap();
                const int last = (int) map.pins.size() - 1;
                doc.movePin (last, map.pins[(size_t) last].time - 0.004);

                if (map.sections.size() >= 2)
                    doc.restartHere (map.sections[1].startTime);
            }

            check (doc.getInputs().userPins.size() >= 1, "edits made user pins: " + juce::String ((int) doc.getInputs().userPins.size()));
            check (doc.undo() && doc.redo(), "undo and redo");
            check (doc.hasUnsavedChanges(), "the edits count as unsaved changes");

            // Three moves of one pin, the second and third joined to the first, are one undo step.
            if (doc.getMap().pins.size() >= 2)
            {
                const auto before = doc.getInputs();
                const int last = (int) doc.getMap().pins.size() - 1;
                const double from = doc.getMap().pins[(size_t) last].time;

                for (int i = 1; i <= 3; ++i)
                    doc.movePin ((int) doc.getMap().pins.size() - 1, from + 0.001 * i, i > 1);

                const bool moved = std::abs (doc.getMap().pins.back().time - (from + 0.003)) < 1.0e-9;
                check (moved && doc.undo() && doc.getInputs() == before, "a run of joined moves is undone in one step");
            }

            // How often pins are placed is an edit like any other, and is saved with the map.
            {
                const auto before = doc.getInputs();
                doc.setPinEveryBars (8);
                const bool set = doc.getInputs().options.pinEveryBars == 8;
                check (set && doc.undo() && doc.getInputs() == before && doc.redo() && doc.getInputs().options.pinEveryBars == 8,
                       "Pin every 8 bars is set, undone and redone");
            }

            // The first bar changed after the edits: every pin keeps its time and moves 39 bars.
            {
                const auto before = doc.getMap().pins;
                doc.setFirstHitBar (40);
                const auto& after = doc.getMap().pins;
                bool moved = ! before.empty() && after.size() == before.size();

                for (size_t i = 0; moved && i < after.size(); ++i)
                    moved = std::abs (after[i].time - before[i].time) < 1.0e-9
                            && std::abs (after[i].beat - before[i].beat - 39.0 * Document::beatsPerBar) < 1.0e-9
                            && after[i].user == before[i].user;

                check (moved, "changing the first bar after the edits renumbers every pin: " + juce::String ((int) after.size()) + " pins");
            }

            // A bar that is not after the pin before it is refused, and nothing changes.
            if (doc.getMap().pins.size() >= 2)
            {
                const auto before = doc.getInputs();
                doc.setPinBeat ((int) doc.getMap().pins.size() - 1, doc.getMap().pins.front().beat);
                check (doc.getInputs() == before && doc.getStatus().contains ("not changed"), "a bar before the pin before it is refused: " + doc.getStatus());
            }

            // A drum stem added after the edits is still listened to when an earlier edit is undone and redone.
            check (doc.loadDrumStem (music).wasOk() && doc.getInputs().listenToDrums, "added a drum stem");
            const bool undone = doc.undo();
            const bool stillListening = doc.getInputs().listenToDrums;
            check (undone && stillListening && doc.redo() && doc.getInputs().listenToDrums, "undo and redo of an earlier edit keep listening to the stem");

            check (doc.saveMap().wasOk() && doc.getMapFile().existsAsFile(), "saved the map beside the music");
            check (! doc.hasUnsavedChanges(), "nothing is unsaved after saving");

            savedInputs = doc.getInputs();
            savedPins = doc.getMap().pins;
            savedClick = doc.getClick();
            savedRate = doc.getSampleRate();
            savedLength = doc.getLengthInSamples();

            const auto clickFile = folder.getChildFile ("click.wav");
            check (doc.exportClick (clickFile).wasOk() && clickFile.existsAsFile(), "exported the click");

            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (clickFile));
            check (reader != nullptr, "the click file opens");

            if (reader != nullptr && savedClick != nullptr)
            {
                check (reader->numChannels == 1 && reader->bitsPerSample == 24, "the click is mono 24-bit");
                check (juce::exactlyEqual (reader->sampleRate, savedRate) && reader->lengthInSamples == savedLength,
                       "the click has the music's sample rate and length");

                juce::AudioBuffer<float> read (1, (int) reader->lengthInSamples);
                reader->read (&read, 0, read.getNumSamples(), 0, true, false);
                float worst = 0.0f, peak = 0.0f;
                const int n = std::min (read.getNumSamples(), (int) savedClick->size());

                for (int i = 0; i < n; ++i)
                {
                    worst = std::max (worst, std::abs (read.getSample (0, i) - (*savedClick)[(size_t) i]));
                    peak = std::max (peak, std::abs (read.getSample (0, i)));
                }

                check (worst < 1.0e-6f && peak > 0.1f, "the click file holds the click that is played");
            }
        }

        {
            Document doc;
            doc.setSynchronous (true);
            check (doc.loadMusic (music).wasOk(), "opened the music again");
            check (doc.getStatus().containsIgnoreCase ("saved map"), "status: " + doc.getStatus());
            check (doc.getInputs() == savedInputs, "the saved bar, tempo, options and user pins came back");

            const auto& pins = doc.getMap().pins;
            bool same = pins.size() == savedPins.size();

            for (size_t i = 0; same && i < pins.size(); ++i)
                same = std::abs (pins[i].time - savedPins[i].time) < 1.0e-9 && std::abs (pins[i].beat - savedPins[i].beat) < 1.0e-9
                       && pins[i].user == savedPins[i].user && pins[i].restart == savedPins[i].restart;

            check (same, "the map is the same: " + juce::String ((int) pins.size()) + " pins");
        }

        folder.deleteRecursively();
        return allPassed;
    }

    void finishSnapshot()
    {
        if (snapshot == nullptr)
            return;

        if (printView)
        {
            const auto& view = snapshot->getWaveform();
            const auto& pins = snapshot->getDocument().getMap().pins;
            const int selected = view.getSelectedPin();

            std::cout << "View " << juce::String (view.getViewStart(), 4) << " to " << juce::String (view.getViewEnd(), 4) << " s"
                      << ", playhead " << juce::String (view.getPlayhead(), 5) << " s"
                      << ", " << (int) pins.size() << " pins";

            if (selected >= 0 && selected < (int) pins.size())
                std::cout << ", selected pin " << selected << " at " << juce::String (pins[(size_t) selected].time, 6) << " s";
            else
                std::cout << ", no pin selected";

            std::cout << ", wave height " << juce::String (view.getWaveHeight(), 3)
                      << ", snap " << (view.getSnap() == ui::WaveformView::Snap::hits ? "hits"
                                       : view.getSnap() == ui::WaveformView::Snap::grid ? "grid" : "off")
                      << ", status: " << snapshot->getDocument().getStatus() << std::endl;
        }

        const auto image = snapshot->createComponentSnapshot (snapshot->getLocalBounds(), true, 2.0f);

        snapshotOut.deleteFile();
        bool ok = false;

        if (auto stream = snapshotOut.createOutputStream())
        {
            juce::PNGImageFormat png;
            ok = png.writeImageToStream (image, *stream);
        }

        if (ok)
            std::cout << "Wrote " << snapshotOut.getFullPathName() << " (" << image.getWidth() << " x " << image.getHeight() << ")" << std::endl;
        else
            std::cerr << "Could not write " << snapshotOut.getFullPathName() << std::endl;

        snapshot = nullptr;
        setApplicationReturnValue (ok ? 0 : 1);
        quit();
    }

    void failSnapshot (const juce::String& message)
    {
        std::cerr << message << std::endl;
        snapshot = nullptr;
        setApplicationReturnValue (1);
        quit();
    }
};

} // namespace tt

START_JUCE_APPLICATION (tt::TempoTantrumApplication)

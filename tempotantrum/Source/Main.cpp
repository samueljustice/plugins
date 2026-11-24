#include <JuceHeader.h>

#include "MainComponent.h"
#include "ProToolsTestPanel.h"
#include "ProToolsWrite.h"
#include "protools/IdentifyBeatTest.h"
#include "protools/IdentifyBeatWrite.h"
#include "ui/Theme.h"

#include <algorithm>
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
    //   --more                        with More opened
    //   --float <out.png>             also saves the floating panel of a run as a picture
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

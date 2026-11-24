#include <JuceHeader.h>

#include "MainComponent.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
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

        if (args.contains ("--snapshot"))
        {
            runSnapshot (args);
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
    void runSnapshot (const juce::StringArray& args)
    {
        const int at = args.indexOf ("--snapshot");
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

        // A moment for anything the view prepares asynchronously, then the picture.
        juce::Timer::callAfterDelay (500, [this] { finishSnapshot(); });
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

// UI shots: renders the real editor offline to PNG files, and checks every text it draws.
//
//   SwooshbucklerUiShot --out <dir> [--scale 0.6|1.0|1.5|2.0] [--seconds 3] [--state <session file>]
//                       [--work <private dir>] [--list] [--no-images] [--constellation] [--sea]
//
// It makes the processor and its editor with no window on screen, loads its own demo sounds (or a session
// file given with --state), plays for a few seconds so the sea, the rides, the scopes and the meters are alive
// (the editor's timers are stepped by hand), and writes:
//
//   empty.png sea-idle.png    the first run: no sounds loaded
//   window.png                the whole window, playing
//   header.png tracks.png lane.png sea.png ride.png bus-a.png bus-b.png hits.png output.png modulation.png
//                             one crop per panel
//   window-stowaway.png tracks-stowaway.png
//                             a track sent to Stowaway by hand while it is off (its tag rests: the track waits)
//   stowaway.png              Stowaway's plate as a new session has it: off, and folded
//   stowaway-off-open.png     off, with the plate open (everything on it rests)
//   stowaway-folded.png       on, and folded
//   window-stowaway-<mode>.png stowaway-<mode>.png sea-stowaway-<mode>.png   (sub, wake, fly, dry)
//                             on and open in each Mode, with a track playing on it: the plate, and the sea showing it
//   morph.png                 the morph pad open over the sea
//   cables.png                three wires, cables on, the buses linked (the Linked, Resting and Disabled looks)
//   settings.png takes.png variations.png variations-rendering.png about.png update.png
//                             the panels that open over the window
//   variations-cancelled.png variations-choices.png variations-repeat.png variations-finished.png
//                             Render Variations cancelled part-way, with its other choices, repeating a batch, and done
//   menu.png                  the preset menu open
//   name-dialog.png           the window that asks for a preset's name
//
// --sea adds one crop of the sea at each of its moments (a hit, a throw, a ripple with the swell up, recording,
// the constellation showing) and prints what a frame of it costs to paint at 1x and 2x. --constellation holds
// the constellation on for every picture.
//
// --docs writes, instead of all of the above, exactly the pictures the manual uses, under the names it uses
// (screenshot.png, header.png, tracks.png, track-lane.png, track-master.png, ride.png, throw-keys.png,
// bus-a.png, bus-b.png, hits.png, output.png, stowaway.png, stowaway-folded.png, sea.png, morph-pad.png, morph-rope.png, modulation.png,
// cables.png, settings.png, shortcuts.png, takes.png, variations.png, about.png). Run it as
// tools/make-docs-images.sh does, at --scale 2.0, and the files go straight into docs/ and docs/images/.
//
// --list prints every text drawn in each of those states with the box it had to fit, and what the checker
// (tools/UiProbe.h) has to say: a text that had to be cut, runs outside its box, is under the smallest size, sits
// on another text, or is capitalised against the rule. The tool exits non-zero when anything is wrong, so it can
// be run by CI and by anyone changing the interface. --no-images checks without writing files.
//
// Nothing is read from or written to the user's own settings: they live in --work (a temporary folder by default).

#include "UiRig.h"
#include "UiProbe.h"
#include "DemoSounds.h"

#include <iostream>

namespace
{
    using namespace UiRig;

    void save(const juce::Image& image, const juce::File& file)
    {
        file.deleteFile();
        juce::FileOutputStream out(file);
        if (out.openedOk())
            juce::PNGImageFormat().writeImageToStream(image, out);
    }

    struct Options
    {
        juce::File out, work, state;
        float scale = 1.0f;
        double seconds = 3.0;
        bool list = false, images = true, constellation = false, sea = false, docs = false;
    };

    /** One state of the window: its picture, and the texts in it. */
    struct Shots
    {
        Rig& rig;
        const Options& options;
        int faults = 0, texts = 0;

        /** Paints the whole editor; `checked` (a panel over the window, or the editor itself) is painted on its own
            for the checker, so texts under an open panel are not counted against it. */
        juce::Image shoot(const juce::String& name, juce::Component* checked = nullptr)
        {
            juce::Image image;
            {
                UiProbe::Recorder whole;
                image = whole.paint(*rig.editor, 1.0f, rig.scale);
                if (checked == nullptr)
                    report(name, whole);
            }
            if (checked != nullptr)
            {
                UiProbe::Recorder part;
                // A panel's own coordinates are the window's 1.0 coordinates unless it sits on the scaled editor itself.
                const bool onEditor = checked->getParentComponent() == rig.editor.get() && checked != &rig.editor->m_content
                                      && checked->getTransform().isIdentity();
                part.paint(*checked, onEditor ? 1.0f : rig.scale, onEditor ? rig.scale : 1.0f);
                report(name, part);
            }
            if (options.images)
                save(image, options.out.getChildFile(name + ".png"));
            return image;
        }

        void crop(const juce::Image& window, const juce::String& name, juce::Rectangle<int> designArea, int margin = 8)
        {
            if (! options.images)
                return;
            const auto area = (designArea.expanded(margin).toFloat() * rig.scale).getSmallestIntegerContainer().getIntersection(window.getBounds());
            if (! area.isEmpty())
                save(window.getClippedImage(area), options.out.getChildFile(name + ".png"));
        }

        void report(const juce::String& name, const UiProbe::Recorder& recorder)
        {
            const auto found = UiProbe::check(recorder.texts);
            texts += static_cast<int>(recorder.texts.size());
            faults += static_cast<int>(found.size());
            if (options.list)
            {
                std::cout << "== " << name << ": " << recorder.texts.size() << " texts";
                if (recorder.glyphRuns > recorder.fitted)
                    std::cout << " (and " << (recorder.glyphRuns - recorder.fitted) << " runs of text not drawn through the fitting helper: text boxes, links)";
                std::cout << std::endl;
                for (const auto& t : recorder.texts)
                    std::cout << "   " << UiProbe::describe(t) << std::endl;
            }
            for (const auto& f : found)
                std::cout << "FAULT in " << name << ": " << f.what << ": " << UiProbe::describe(f.text) << " - " << f.detail << std::endl;
        }
    };
}

namespace
{
    /** --docs: exactly the pictures the manual uses, under the names it uses. One run, in the order the manual
        reads, so each picture shows the window in the state its section describes. */
    int runDocs(const Options& options)
    {
        using namespace UiRig;

        Sandbox sandbox(options.work, "uishot-docs");
        int faults = 0, texts = 0;
        {
            auto base = makeProcessor();
            auto& proc = *dynamic_cast<Proc*>(base.get());
            // Render Variations prints where its files go, so the recordings folder is named as a reader's would be.
            // Give --work a short, plain folder (make-docs-images.sh does) and the line reads like anyone's.
            const auto recordings = options.work.getChildFile("Desktop");
            recordings.deleteRecursively();
            recordings.createDirectory();
            proc.setRecordFolder(recordings);
            proc.setVariationFolder({}); // Dated Folder: the default
            DemoSounds::loadInto(proc);

            Rig rig(proc, options.scale);
            Shots shots { rig, options };
            rig.open();
            auto& ed = *rig.editor;
            proc.setPlaying(true);
            rig.run(options.seconds);

            // The window as it comes, and one crop per part of it.
            {
                const auto window = shots.shoot("screenshot");
                shots.crop(window, "header", { 0, 0, Editor::c_width, 56 }, 0);
                shots.crop(window, "tracks", ed.m_hold.getBounds(), 2);
                shots.crop(window, "sea", ed.m_sea.getBounds(), 2);
                shots.crop(window, "ride", ed.m_performPanel, 2);
                shots.crop(window, "bus-a", ed.m_sections[0].bounds, 2);
                shots.crop(window, "bus-b", ed.m_sections[1].bounds, 2);
                shots.crop(window, "hits", ed.m_sections[2].bounds, 2);
                shots.crop(window, "output", ed.m_sections[3].bounds, 2);
                shots.crop(window, "modulation", ed.m_modSection, 2);
                shots.crop(window, "stowaway-folded", ed.m_stowPanel, 2);
                for (auto* lane : ed.m_hold.m_laneHolder.getChildren())
                    if (lane->isVisible())
                    {
                        shots.crop(window, "track-lane", ed.m_content.getLocalArea(lane, lane->getLocalBounds()), 2);
                        break;
                    }
            }

            // Stowaway, switched on with a track sent to it, and its plate open. (Not through the lane's menu: its
            // line in the Tracks title bar would still be there in the pictures that follow.)
            {
                proc.setTrackBus(1, CargoHold::c_stowBus);
                proc.setStowawayOn(true);
                ed.m_hold.historyStepped();
                rig.run(0.1);
                ed.m_stowOpen.snap(1.0f); // (it eases open over 180 ms of real time: the picture wants it open)
                ed.layoutBottomRow();
                rig.run(2.5);
                shots.crop(shots.shoot("window-stowaway"), "stowaway", ed.m_stowPanel, 2);
                proc.setTrackBus(1, -1);
                proc.setStowawayOn(false);
                proc.setStowawayPanel(-1);
                ed.m_stowOpen.snap(0.0f);
                ed.layoutBottomRow();
                ed.m_hold.historyStepped();
                rig.run(0.3);
            }

            // A throw on its way in: the Ride panel counting down to its peak, with Throw, Throw Length and Keys.
            {
                proc.throwWhoosh();
                rig.run(0.5);
                const auto window = shots.shoot("window-throw");
                shots.crop(window, "throw-keys", ed.m_swell.getBounds().getUnion(ed.m_throwButton.getBounds())
                                                     .getUnion(ed.m_keysMode.getBounds()), 10);
                rig.run(2.5); // let it go by, so it is not still in flight in the pictures that follow
            }

            // The Master star on one lane, and a bus chosen by hand on another.
            {
                proc.setMasterTrack(0);
                proc.setTrackBus(1, 0);
                ed.m_hold.historyStepped();
                rig.run(0.4);
                const auto window = shots.shoot("window-master");
                juce::Rectangle<int> two;
                int seen = 0;
                for (auto* lane : ed.m_hold.m_laneHolder.getChildren())
                    if (lane->isVisible() && seen++ < 2)
                        two = two.getUnion(ed.m_content.getLocalArea(lane, lane->getLocalBounds()));
                shots.crop(window, "track-master", two, 2);
                proc.setTrackBus(1, -1);
                proc.setMasterTrack(-1);
                ed.m_hold.historyStepped();
                rig.run(0.2);
            }

            // The morph pad, open and on, over the sea; then one of its ropes tied to a track.
            {
                if (auto* on = proc.getState().getParameter(ParamIDs::morphOn))
                    on->setValueNotifyingHost(1.0f);
                ed.m_morphButton.setToggleState(true, juce::dontSendNotification);
                ed.syncMorphPad();
                rig.run(0.6);
                shots.crop(shots.shoot("window-morph"), "morph-pad", ed.m_sea.getBounds(), 2); // the pad docks in the sea

                proc.setMorphSource(0, 0);
                ed.syncMorphPad();
                ed.m_morphPad.ropesMoved();
                rig.run(0.4);
                shots.crop(shots.shoot("window-rope"), "morph-rope", ed.m_hold.getBounds().getUnion(ed.m_sea.getBounds()), 2);

                proc.setMorphSource(0, -1);
                ed.m_morphButton.setToggleState(false, juce::dontSendNotification);
                if (auto* on = proc.getState().getParameter(ParamIDs::morphOn))
                    on->setValueNotifyingHost(0.0f);
                ed.syncMorphPad();
                ed.m_morphPad.ropesMoved();
                rig.run(0.2);
            }

            // Three wires with the cables showing: the Linked, Resting and Disabled looks, as the manual describes.
            {
                auto& mods = proc.getMods();
                mods.connect(0, ParamIDs::bus[0][ParamIDs::Doppler], 0.6f);
                mods.connect(1, ParamIDs::bus[1][4], -0.4f);
                mods.connect(1, ParamIDs::energy, 0.3f);
                mods.connect(2, ParamIDs::hitBoost, 0.5f);
                if (auto* link = proc.getState().getParameter(ParamIDs::linkBuses))
                    link->setValueNotifyingHost(1.0f);
                ed.m_wires.rebuild();
                ed.toggleLog(); // arm the tape, so Takes has something in it
                rig.run(1.0);
                ed.m_wires.rebuild();
                shots.shoot("cables");
                ed.toggleLog();
                ed.saveRetro();
                proc.timerCallback();
                rig.run(0.2);

                // A few more, so the Takes list in the manual has something to show.
                for (int i = 0; i < 2; ++i)
                {
                    ed.toggleLog();
                    rig.run(0.8);
                    ed.toggleLog();
                    ed.saveRetro();
                    proc.timerCallback();
                    rig.run(0.3);
                }
            }

            // The panels that open over the window, and the shortcuts list inside Settings.
            {
                ed.m_toast.clear(); // nothing of the run's own doings over the panels
                ed.m_toastRise.snap(0.0f);
                rig.run(0.1);

                ed.m_settings.refresh();
                ed.m_settings.setVisible(true);
                ed.m_settings.toFront(false);
                const auto window = shots.shoot("settings", &ed.m_settings);
                auto& list = ed.m_settings.keys();
                // Its own box, with a hair's breadth around it: any more catches the sentence above it.
                shots.crop(window, "shortcuts", ed.m_content.getLocalArea(&list, list.getLocalBounds()).expanded(6).withTrimmedTop(6), 0);
                ed.m_settings.setVisible(false);

                ed.showTakes();
                rig.run(0.3);
                shots.shoot("takes", &ed.m_takes);
                ed.m_takes.setVisible(false);

                proc.setVariationFolder({}); // Dated Folder, the default: no scratch path in the manual's picture
                ed.showVariations();
                ed.m_variations.readSettings();
                rig.run(0.2);
                shots.shoot("variations", &ed.m_variations);
                ed.m_variations.setVisible(false);

                ed.m_about.setVisible(true);
                ed.m_about.toFront(false);
                // The mark draws itself over about two and a half seconds of real time from the moment About opens
                // (it reads the clock, not the rig), so the picture waits for it rather than playing more audio.
                juce::Thread::sleep(3000);
                shots.shoot("about", &ed.m_about);
                ed.m_about.setVisible(false);
            }

            proc.setPlaying(false);
            faults = shots.faults;
            texts = shots.texts;
            rig.editor.reset();
        }
        std::cout << texts << " texts checked at scale " << juce::String(options.scale, 2) << ": "
                  << (faults == 0 ? juce::String("every one fits its box and follows the capitals rule.") : juce::String(faults) + " faults.") << std::endl;
        return faults == 0 ? 0 : 1;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;

    Options options;
    options.work = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SwooshbucklerUiShot");
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a(argv[i]);
        auto next = [&]() { return i + 1 < argc ? juce::String(argv[++i]) : juce::String(); };
        auto file = [&]() { return juce::File::getCurrentWorkingDirectory().getChildFile(next()); };
        if (a == "--out") options.out = file();
        else if (a == "--work") options.work = file();
        else if (a == "--state") options.state = file();
        else if (a == "--scale") options.scale = juce::jlimit(0.5f, 2.0f, next().getFloatValue());
        else if (a == "--seconds") options.seconds = juce::jlimit(0.5, 30.0, next().getDoubleValue());
        else if (a == "--list") options.list = true;
        else if (a == "--no-images") options.images = false;
        else if (a == "--constellation") options.constellation = true;
        else if (a == "--sea") options.sea = true;
        else if (a == "--docs") options.docs = true;
        else
        {
            std::cerr << "Unknown option " << a << "\nUsage: SwooshbucklerUiShot --out <dir> [--scale 1.0] [--seconds 3] [--state <session file>] "
                         "[--work <dir>] [--list] [--no-images] [--constellation] [--sea] [--docs]" << std::endl;
            return 2;
        }
    }
    if (options.images && options.out == juce::File())
    {
        std::cerr << "Say where the pictures go with --out <dir> (or check only, with --no-images)." << std::endl;
        return 2;
    }
    if (options.images)
        options.out.createDirectory();
    options.work.createDirectory();

    if (options.docs)
        return runDocs(options);

    // Our own settings file and folders: nothing of the user's is touched, and nothing goes on line.
    Sandbox sandbox(options.work, "uishot");

    int faults = 0, texts = 0;
    {
        auto base = makeProcessor();
        auto& proc = *dynamic_cast<Proc*>(base.get());
        const auto recordings = options.work.getChildFile("Recordings");
        recordings.deleteRecursively();
        recordings.createDirectory();
        proc.setRecordFolder(recordings);
        proc.setVariationFolder(options.work.getChildFile("Variations").getFullPathName());

        Rig rig(proc, options.scale);
        Shots shots { rig, options };

        // The first run: nothing loaded.
        rig.open();
        rig.run(0.3);
        shots.crop(shots.shoot("empty"), "sea-idle", rig.editor->m_sea.getBounds());
        rig.editor.reset();

        // The sounds: a session file, or the tool's own demo sounds (tools/DemoSounds.cpp). The editor is made after them (there is no
        // message loop to tell an open one).
        if (options.state != juce::File())
        {
            juce::String error;
            if (! proc.openSession(options.state, error))
            {
                std::cerr << "Couldn't open " << options.state.getFullPathName() << ": " << error << std::endl;
                return 2;
            }
        }
        else
            DemoSounds::loadInto(proc);

        rig.open();
        auto& ed = *rig.editor;
        proc.setPlaying(true);
        if (options.constellation)
            ed.m_sea.showConstellation(true);
        rig.run(options.seconds);

        // The window and its panels.
        {
            const auto window = shots.shoot("window");
            shots.crop(window, "header", { 0, 0, Editor::c_width, 56 }, 0);
            shots.crop(window, "tracks", ed.m_hold.getBounds());
            if (ed.m_hold.m_laneHolder.getNumChildComponents() > 0)
                for (auto* lane : ed.m_hold.m_laneHolder.getChildren())
                    if (lane->isVisible())
                    {
                        shots.crop(window, "lane", ed.m_content.getLocalArea(lane, lane->getLocalBounds()), 2);
                        break;
                    }
            shots.crop(window, "sea", ed.m_sea.getBounds());
            shots.crop(window, "ride", ed.m_performPanel);
            shots.crop(window, "bus-a", ed.m_sections[0].bounds);
            shots.crop(window, "bus-b", ed.m_sections[1].bounds);
            shots.crop(window, "hits", ed.m_sections[2].bounds);
            shots.crop(window, "output", ed.m_sections[3].bounds);
            shots.crop(window, "modulation", ed.m_modSection);
            shots.crop(window, "stowaway", ed.m_stowPanel);

            // The panels bolted to the window: listed, and none may run into another or leave the window.
            const auto panels = ed.panelBounds();
            const juce::Rectangle<int> whole(0, 0, Editor::c_width, Editor::c_height);
            if (options.list)
                std::cout << "== panels: " << panels.size() << std::endl;
            for (size_t i = 0; i < panels.size(); ++i)
            {
                if (options.list)
                    std::cout << "   " << panels[i].first << " " << panels[i].second.toString() << std::endl;
                if (panels[i].second.isEmpty() || ! whole.contains(panels[i].second))
                {
                    ++shots.faults;
                    std::cout << "FAULT in layout: " << panels[i].first << " " << panels[i].second.toString() << " is not whole in the window" << std::endl;
                }
                for (size_t j = i + 1; j < panels.size(); ++j)
                    if (panels[i].second.intersects(panels[j].second))
                    {
                        ++shots.faults;
                        std::cout << "FAULT in layout: " << panels[i].first << " " << panels[i].second.toString() << " runs into " << panels[j].first << " "
                                  << panels[j].second.toString() << std::endl;
                    }
            }
        }

        // What the rig says back: the toast's plate at the top of the open water, risen into place.
        {
            ed.toast("Kept as the preset Roll 2. Rename it in the preset list.");
            rig.run(0.4);
            const auto window = shots.shoot("toast");
            shots.crop(window, "toast-plate", ed.m_toastPill.expanded(40, 24), 0);
            // The longest thing it says: a preset has switched Stowaway on with nothing sent to it.
            ed.toast("Stowaway is on with nothing on it: send it a track from a lane's bus tag");
            rig.run(0.4);
            shots.crop(shots.shoot("toast-stowaway"), "toast-stowaway-plate", ed.m_toastPill.expanded(40, 24), 0);
            ed.m_toast.clear();
            ed.m_toastRise.snap(0.0f);
            rig.run(0.1);
        }

        // Stowaway. A track sent to it by hand while it is off waits: its tag rests, with a hollow S. Then the plate
        // in each of its states, and (on, with the track playing on it) the sea showing it in each Mode.
        {
            auto set = [&proc](const char* id, float plain)
            {
                auto* param = proc.getState().getParameter(id);
                param->setValueNotifyingHost(param->convertTo0to1(plain));
            };
            // The plate eases open over 180 ms of real time; the pictures want it where it is going.
            auto settle = [&](bool open)
            {
                proc.setStowawayPanel(open ? 1 : 0);
                ed.m_stowOpen.snap(open ? 1.0f : 0.0f);
                ed.layoutBottomRow();
            };
            auto plate = [&](const juce::String& name) { shots.crop(shots.shoot("window-" + name), name, ed.m_stowPanel.getUnion(ed.m_modSection.withWidth(120))); };

            proc.getCargo().setBusOverride(1, CargoHold::c_stowBus);
            ed.m_hold.historyStepped(); // the lanes take up the change (no message loop runs here)
            rig.run(0.3);
            shots.crop(shots.shoot("window-stowaway"), "tracks-stowaway", ed.m_hold.getBounds());

            settle(true);
            rig.run(0.2);
            plate("stowaway-off-open");

            set(ParamIDs::stowOn, 1.0f);
            settle(false);
            rig.run(1.0);
            plate("stowaway-folded");

            const char* names[ParamIDs::numStowModes] = { "sub", "wake", "fly", "dry" };
            settle(true);
            for (int mode = 0; mode < ParamIDs::numStowModes; ++mode)
            {
                set(ParamIDs::stowMode, static_cast<float>(mode));
                rig.run(mode == ParamIDs::StowDry ? 1.5 : 3.0);
                const juce::String name = juce::String("stowaway-") + names[mode];
                const auto window = shots.shoot("window-" + name);
                shots.crop(window, name, ed.m_stowPanel.getUnion(ed.m_modSection.withWidth(120)));
                shots.crop(window, "sea-" + name, ed.m_sea.getBounds());
            }

            set(ParamIDs::stowMode, static_cast<float>(ParamIDs::StowSub));
            set(ParamIDs::stowOn, 0.0f);
            proc.setStowawayPanel(-1);
            ed.m_stowOpen.snap(0.0f);
            ed.layoutBottomRow();
            proc.getCargo().setBusOverride(1, -1);
            ed.m_hold.historyStepped();
            rig.run(0.1);
        }

        // The sea at its moments: one crop each, so the hero can be looked at on its own. Everything the sea
        // answers to is set here by hand, because none of it happens to order in three seconds of demo sounds.
        if (options.sea)
        {
            auto seaShot = [&](const juce::String& name)
            {
                rig.run(0.1);
                shots.crop(shots.shoot("window-" + name), name, ed.m_sea.getBounds());
            };
            auto& sea = ed.m_sea;
            std::cout << "constellation: " << sea.m_constellation.size() << " stars, "
                      << sea.m_constellationLines.size() << " lines" << std::endl;

            sea.showConstellation(true);
            seaShot("sea-constellation");
            sea.showConstellation(false);
            sea.m_constellationOpacity = 0.0f;

            sea.m_hitFlash = sea.m_hitRing = 1.0f;
            seaShot("sea-hit");

            proc.throwWhoosh();
            rig.run(0.8);
            seaShot("sea-throw");

            sea.m_ripples[0] = { 0.2f, 1.4f, 0.35f };
            sea.m_swell = 1.0f;
            seaShot("sea-ripple");
            sea.m_swell = 0.0f;

            ed.toggleLog();                 // arm the tape: the recording lamp
            rig.run(0.6);
            seaShot("sea-recording");
            ed.toggleLog();
            proc.timerCallback();
            rig.run(0.2);

            // What a frame of the sea costs, at the window's scale and at twice it.
            for (float zoom : { 1.0f, 2.0f })
            {
                const auto size = (ed.m_sea.getLocalBounds().toFloat() * zoom).getSmallestIntegerContainer();
                juce::Image image(juce::Image::ARGB, size.getWidth(), size.getHeight(), true);
                constexpr int frames = 60;
                juce::Graphics g(image);
                g.addTransform(juce::AffineTransform::scale(zoom));
                ed.m_sea.paint(g); // the sky image is built on the first paint at a new scale
                const auto start = juce::Time::getHighResolutionTicks();
                for (int i = 0; i < frames; ++i)
                    ed.m_sea.paint(g);
                std::cout << "sea paint at " << juce::String(zoom, 1) << "x: "
                          << juce::String(juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start) * 1000.0 / frames, 2)
                          << " ms a frame" << std::endl;
            }
        }

        // The morph pad, open and on.
        {
            if (auto* on = proc.getState().getParameter(ParamIDs::morphOn))
                on->setValueNotifyingHost(1.0f);
            ed.m_morphButton.setToggleState(true, juce::dontSendNotification);
            ed.syncMorphPad();
            rig.run(0.6);
            const auto window = shots.shoot("morph");
            shots.crop(window, "morph", ed.m_sea.getBounds().getUnion(ed.m_performPanel));
            ed.m_morphButton.setToggleState(false, juce::dontSendNotification);
            if (auto* on = proc.getState().getParameter(ParamIDs::morphOn))
                on->setValueNotifyingHost(0.0f);
            ed.syncMorphPad();
        }

        // Three wires with the cables showing, the buses linked and the tape armed: the Linked look (bus B's Doppler,
        // Width and Pass), the Resting look (Peak Tremolo is off) and the Disabled look (Aim is on, so the gate rests).
        {
            auto& mods = proc.getMods();
            mods.connect(0, ParamIDs::bus[0][ParamIDs::Doppler], 0.6f);
            mods.connect(1, ParamIDs::bus[1][4], -0.4f);
            mods.connect(1, ParamIDs::energy, 0.3f);
            mods.connect(2, ParamIDs::hitBoost, 0.5f);
            if (auto* link = proc.getState().getParameter(ParamIDs::linkBuses))
                link->setValueNotifyingHost(1.0f);
            ed.m_wires.rebuild();
            ed.toggleLog(); // arm the tape
            rig.run(1.0);
            ed.m_wires.rebuild();
            shots.shoot("cables");
            ed.toggleLog(); // and stop it: a take for the Takes panel
            ed.saveRetro(); // and another
            proc.timerCallback();
            rig.run(0.2);
        }

        // The panels that open over the window.
        {
            ed.m_settings.refresh();
            ed.m_settings.setVisible(true);
            ed.m_settings.toFront(false);
            shots.shoot("settings", &ed.m_settings);
            ed.m_settings.setVisible(false);

            ed.showTakes();
            rig.run(0.2);
            shots.shoot("takes", &ed.m_takes);
            ed.m_takes.setVisible(false);

            ed.showVariations();
            rig.run(0.1);
            shots.shoot("variations", &ed.m_variations);
            // A batch in hand: enough files that it is still going when the picture is taken.
            proc.setVariationCount(400);
            ed.m_variations.readSettings();
            ed.m_variations.startBatch();
            for (int i = 0; i < 400 && proc.getVariations().getProgress().done < 3 && proc.getVariations().isRendering(); ++i)
                juce::Thread::sleep(10);
            rig.run(0.1);
            shots.shoot("variations-rendering", &ed.m_variations);
            proc.getVariations().cancel();
            proc.getVariations().waitUntilDone(60000);
            rig.run(0.2);
            ed.m_variations.timerCallback();
            shots.shoot("variations-cancelled", &ed.m_variations);

            // The other choices: a whole ride, small changes, a typed batch name and one file with markers...
            auto& v = ed.m_variations;
            auto pick = [](juce::ComboBox& box, int id) { box.setSelectedId(id, juce::sendNotificationSync); };
            pick(v.m_each, 2);
            pick(v.m_mode, 2);
            pick(v.m_names, 4);
            v.m_batchName.setText("Dragon Wings", true);
            pick(v.m_files, 2);
            rig.run(0.1);
            shots.shoot("variations-choices", &v);
            // ...date and time, both kinds of file, Randomise Everything and a batch to repeat...
            pick(v.m_names, 2);
            pick(v.m_files, 3);
            pick(v.m_batch, 2); // takes the count, lengths and What Varies of the batch it repeats
            rig.run(0.1);
            shots.shoot("variations-repeat", &v);
            // ...and a small batch that has finished.
            pick(v.m_batch, 1);
            pick(v.m_each, 1);
            pick(v.m_files, 1);
            pick(v.m_mode, 1);
            pick(v.m_names, 1);
            v.m_count.setText("3", true);
            v.startBatch();
            proc.getVariations().waitUntilDone(120000);
            rig.run(0.3);
            v.timerCallback();
            shots.shoot("variations-finished", &v);
            v.m_count.setText("100", true);
            ed.m_variations.setVisible(false);

            ed.m_about.setVisible(true);
            ed.m_about.toFront(false);
            shots.shoot("about", &ed.m_about);
            ed.m_about.setVisible(false);

            sammyj::UpdateInfo info;
            info.pluginId = "swooshbuckler";
            info.currentVersion = JucePlugin_VersionString;
            info.latestVersion = "9.9.9";
            ed.showUpdatePopup(info);
            shots.shoot("update", &ed.m_updatePopup);
            ed.m_updatePopup.setVisible(false);
        }

        // The preset menu, open inside the window (in the app it is a window of its own).
        {
            const auto items = ed.presetList();
            juce::File currentFile;
            juce::Array<juce::File> recent;
            auto menu = ed.buildPresetMenu(items, currentFile, recent);
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(ed.m_presetName).withParentComponent(&ed), [](int) {});
            juce::Component* window = nullptr;
            for (auto* child : ed.getChildren())
                if (child != &ed.m_content && child != &ed.m_updatePopup && child->isVisible())
                    window = child;
            // As a window of its own the menu is scaled with the control it hangs from; here that is done by hand.
            if (window != nullptr)
            {
                const auto under = ed.getLocalArea(&ed.m_presetName, ed.m_presetName.getLocalBounds());
                window->setTopLeftPosition(under.getX(), under.getBottom());
                window->setTransform(juce::AffineTransform::scale(rig.scale, rig.scale, static_cast<float>(under.getX()), static_cast<float>(under.getBottom())));
            }
            shots.shoot("menu", window);
            juce::PopupMenu::dismissAllActiveMenus();
        }

        // The window that asks for a preset's name: a window of its own, painted here without being shown.
        {
            std::unique_ptr<juce::AlertWindow> dialog(ed.makeNameDialog("Save Preset", "The settings as they are now are saved under this name. "
                                                                        "A preset of yours with the same name is replaced.", "My Preset", "Save", true));
            UiProbe::Recorder recorder;
            const auto image = recorder.paint(*dialog, rig.scale);
            shots.report("name-dialog", recorder);
            if (options.images)
                save(image, options.out.getChildFile("name-dialog.png"));
            dialog->removeCustomComponent(0);
            dialog.reset();
            ed.m_dialogToggle.reset();
        }

        proc.setPlaying(false);
        faults = shots.faults;
        texts = shots.texts;
        rig.editor.reset();
    }
    std::cout << texts << " texts checked at scale " << juce::String(options.scale, 2) << ": "
              << (faults == 0 ? juce::String("every one fits its box and follows the capitals rule.") : juce::String(faults) + " faults.") << std::endl;
    return faults == 0 ? 0 : 1;
}

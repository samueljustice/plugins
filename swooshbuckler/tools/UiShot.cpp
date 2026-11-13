// UI shots: renders the real editor offline to PNG files, and checks every text it draws.
//
//   SwooshbucklerUiShot --out <dir> [--scale 0.6|1.0|1.5|2.0] [--seconds 3] [--state <session file>]
//                       [--work <private dir>] [--list] [--no-images]
//
// It makes the processor and its editor with no window on screen, loads the built-in demo sounds (or a session
// file given with --state), plays for a few seconds so the sea, the rides, the scopes and the meters are alive
// (the editor's timers are stepped by hand), and writes:
//
//   empty.png                 the first run: no sounds loaded
//   window.png                the whole window, playing
//   header.png tracks.png lane.png sea.png ride.png bus-a.png bus-b.png hits.png output.png modulation.png
//                             one crop per panel
//   morph.png                 the morph pad open over the sea
//   cables.png                three wires, cables on, the buses linked (the Linked, Resting and Disabled looks)
//   settings.png takes.png variations.png variations-rendering.png about.png update.png
//                             the panels that open over the window
//   menu.png                  the preset menu open
//   name-dialog.png           the window that asks for a preset's name
//
// --list prints every text drawn in each of those states with the box it had to fit, and what the checker
// (tools/UiProbe.h) has to say: a text that had to be cut, runs outside its box, is under the smallest size, sits
// on another text, or is capitalised against the rule. The tool exits non-zero when anything is wrong, so it can
// be run by CI and by anyone changing the interface. --no-images checks without writing files.
//
// Nothing is read from or written to the user's own settings: they live in --work (a temporary folder by default).

#include "UiRig.h"
#include "UiProbe.h"

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
        bool list = false, images = true;
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
        else
        {
            std::cerr << "Unknown option " << a << "\nUsage: SwooshbucklerUiShot --out <dir> [--scale 1.0] [--seconds 3] [--state <session file>] "
                         "[--work <dir>] [--list] [--no-images]" << std::endl;
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
        shots.shoot("empty");
        rig.editor.reset();

        // The sounds: a session file, or the built-in demo sounds. The editor is made after them (there is no
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
            proc.loadDemoSounds();

        rig.open();
        auto& ed = *rig.editor;
        proc.setPlaying(true);
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
            rig.run(0.1);
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

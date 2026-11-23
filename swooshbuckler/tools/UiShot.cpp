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
//   variations-51.png variations-714-multimono.png variations-714-finished.png variations-714-multimono-finished.png
//   variations-714-flac.png
//                             Render Variations wider than the rig: the order line at 5.1, Multi-Mono at 7.1.4, a 7.1.4
//                             batch done (interleaved, and as Multi-Mono sets: takes and files counted), and FLAC refusing
//                             7.1.4 in one file before anything is rendered (Render rests)
//   settings-714.png settings-714-stereo-fold.png settings-714-flac.png takes-714.png
//                             the rig in a 7.1.4 room: the tape's settings (as played, folded to Stereo, FLAC refused)
//                             and the Takes list with a 7.1.4 take, a Stereo fold, a Multi-Mono set and 7.1.4 variations
//   settings-app.png settings-app-51.png settings-app-714.png settings-app-714-folded.png audio-device-51.png
//   audio-device-714.png audio-device-714-folded.png window-app-folded.png footer-app-folded.png
//                             the app, played from a pretend audio device: Speakers in Settings (Stereo, 5.1, 7.1.4,
//                             and 7.1.4 on a two-output device), the audio device dialog, and the footer saying it is folded
//   window-app.png room-strip-app.png room-strip-app-714.png
//                             the app's room strip, its Speakers a menu (at Stereo, and at 7.1.4)
//   window-room-stereo.png room-strip-stereo.png bus-a-stereo.png
//                             the room strip, the Flight Paths and the rest of the window at Stereo, where they rest
//                             (the sea's note in the plan's place says why)
//   window-51.png sea-51.png room-strip-51.png output-51.png sea-plan-51.png
//                             a 5.1 track: the strip lit, the speaker meters, the plan of the room (Centre Speaker on)
//   window-714-<path>.png sea-714-<path>.png sea-plan-714-<path>.png   (across, through, overhead, around)
//   output-714.png room-strip-714.png bus-a-714.png bus-b-714.png
//                             a 7.1.4 track with both buses on each Flight Path: the comets, the plan, the meters
//   window-714-stowaway-fly.png stowaway-fly-714.png sea-714-stowaway-fly.png
//                             Stowaway's Fly going Around in a 7.1.4 room, its plate open with its Flight Path
//   window-714-stowaway-fly-folded.png stowaway-fly-714-folded.png
//                             the same plate folded: its note names Fly's Flight Path and way
//   window-51-lfe-unfed.png room-strip-51-lfe-unfed.png variations-51-lfe-unfed.png
//                             LFE Mode Discrete with Stowaway off: the strip's cue and fix, and Render Variations' note
//   window-714-held.png bus-a-714-held.png bus-b-714-linked.png
//                             bus A held to Through, Front to Back, on the Right; bus B linked, showing bus A's
//   window-714-direction-wired.png bus-a-714-direction-wired.png
//                             a modulator wired to bus A's Direction: the key it picks now marked in its colour
//   front-<mono|stereo|714>.png front-<width>-<ride|bus-a|bus-b|output>.png
//                             the front at Mono, Stereo and 7.1.4: four hollow notches (Swirl and Filter on each bus,
//                             Rate, Swell) and the EQ key in Output's title bar
//   layer-bus-a-swirl-<type>.png layer-bus-a-filter.png layer-bus-b-swirl.png layer-bus-b-filter.png
//   layer-ride-together.png layer-ride-swell.png layer-ride-together-on.png layer-ride-swell-on.png
//                             each panel turned over to each of its layers (window-<name>.png the whole window)
//   front-notches-filled.png layers-three.png layers-three-closed.png eq-plate-flat.png eq-plate.png eq-key-curve.png
//                             filled notches (Faders Together, so Ride counts down to the peak), three layers up at
//                             once with wires on Swell and Varispeed (and the front again after Back), the Output EQ
//                             plate over Bus B, flat and with a curve, the EQ key drawing that curve
//                             (--layers writes only these, and checks their texts)
//   follow-pass.png follow-filter.png follow-menu.png motion-with-filter.png motion-menu.png
//   motion-with-filter-resting.png (and window-... of each)
//                             a Follower on Bus A Pass and on Bus A Filter (its scope drawing them), its menu open with
//                             the four Pass and Filter inputs; bus A's Tape Flanger page With the Filter, Motion's menu
//                             open, and the Motion box resting with Filter at 0% (--follow writes only these)
//   menu.png                 the preset menu open
//   name-dialog.png           the window that asks for a preset's name
//   speakers-dialog.png       the app asking before a change of Speakers empties the Last 60 s memory
//
// --sea adds one crop of the sea at each of its moments (a hit, a throw, a ripple with the swell up, recording,
// the constellation showing) and prints what a frame of it costs to paint at 1x and 2x. --constellation holds
// the constellation on for every picture.
//
// --docs writes, instead of all of the above, exactly the pictures the manual uses, under the names it uses
// (screenshot.png, header.png, tracks.png, track-lane.png, track-master.png, ride.png, throw-keys.png,
// bus-a.png, bus-b.png, hits.png, output.png, stowaway.png, stowaway-folded.png, sea.png, morph-pad.png, morph-rope.png, modulation.png,
// cables.png, settings.png, shortcuts.png, takes.png, variations.png, about.png, and for surround room-strip.png,
// output-room.png, sea-room.png, room-plan.png, drawn at 7.1.4, and layer-swirl.png, layer-filter.png, layer-together.png,
// layer-swell.png and output-eq.png: the layers and the Output EQ). Run it as
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
#include "AppOutput.h"
#include "FakeAudioDevice.h"
#include "DemoSounds.h"

#include <functional>
#include <iostream>
#include <set>
#include <vector>

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
        bool list = false, images = true, constellation = false, sea = false, docs = false, layersOnly = false, followOnly = false;
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
    /** The layers (design 2.0): the front as it is at Mono, Stereo and 7.1.4 (four hollow notches, the EQ key), each
        panel turned over to each of its layers (every Swirl type, the Filter, Together, Swell), the notches filled,
        three layers up at once with wires on Swell and Varispeed, and the EQ plate over Bus B. The panel crops are
        named after the layer: layer-bus-a-swirl-tape-flanger.png, layer-ride-swell.png, and so on. */
    void shootLayers(Shots& shots, Rig& rig, Proc& proc)
    {
        auto& ed = *rig.editor;
        auto set = [&](const char* id, float plain)
        {
            auto* param = proc.getState().getParameter(id);
            param->setValueNotifyingHost(param->convertTo0to1(plain));
        };
        auto settle = [&]()
        {
            for (auto& host : ed.m_layers)
                host->finishFold();
            ed.applyLayers();
            ed.updateNotches(true);
            rig.run(0.25);
        };
        auto open = [&](Layers::Target t) { ed.openLayer(t); settle(); };
        auto closeAll = [&]()
        {
            for (auto& host : ed.m_layers)
                host->close();
            settle();
        };
        auto widen = [&](room::Width width)
        {
            auto layout = proc.getBusesLayout();
            layout.outputBuses.getReference(0) = room::channelSet(width);
            proc.setBusesLayout(layout);
            proc.prepareToPlay(UiRig::c_fs, UiRig::c_block);
            rig.block.setSize(room::layout(width).numChannels, UiRig::c_block);
        };
        auto slug = [](juce::String text) { return text.toLowerCase().replace(" / ", "-").replace(" + ", "-").replace(" ", "-"); };
        auto panelOf = [&](Layers::Target t) { return ed.layerPanelBounds(Editor::panelFor(t)); };

        // The front at three widths: as calm as it was, with four hollow notches and the EQ key.
        proc.setPlaying(true);
        const std::pair<room::Width, const char*> widths[] = { { room::Width::Mono, "mono" }, { room::Width::Stereo, "stereo" },
                                                               { room::Width::Surround714, "714" } };
        for (const auto& [width, name] : widths)
        {
            widen(width);
            rig.run(1.5);
            const auto window = shots.shoot(juce::String("front-") + name);
            shots.crop(window, juce::String("front-") + name + "-ride", ed.m_performPanel);
            shots.crop(window, juce::String("front-") + name + "-bus-a", ed.m_sections[0].bounds);
            shots.crop(window, juce::String("front-") + name + "-bus-b", ed.m_sections[1].bounds);
            shots.crop(window, juce::String("front-") + name + "-output", ed.m_sections[3].bounds);
        }
        widen(room::Width::Stereo);
        rig.run(0.5);

        // Each panel turned over: every Swirl type on bus A (and the Tape Flanger on bus B), the Filter on both, and
        // Ride's two.
        for (int type = 0; type < ParamIDs::numSwirlTypes; ++type)
        {
            set(ParamIDs::busDetail[0][ParamIDs::SwirlType], static_cast<float>(type));
            open({ Layers::Kind::Swirl, 0 });
            const auto name = "layer-bus-a-swirl-" + slug(ParamIDs::swirlTypeNames[type]);
            shots.crop(shots.shoot("window-" + name), name, ed.m_sections[0].bounds);
        }
        set(ParamIDs::busDetail[0][ParamIDs::SwirlType], 0.0f);
        set(ParamIDs::busDetail[1][ParamIDs::SwirlType], static_cast<float>(ParamIDs::numSwirlTypes - 2)); // Tape Flanger
        const Layers::Target others[] = { { Layers::Kind::Filter, 0 }, { Layers::Kind::Swirl, 1 }, { Layers::Kind::Filter, 1 },
                                          { Layers::Kind::Together, 0 }, { Layers::Kind::Swell, 0 } };
        for (const auto t : others)
        {
            closeAll();
            open(t);
            const auto name = "layer-" + slug(Layers::title(t));
            shots.crop(shots.shoot("window-" + name), name, panelOf(t));
        }
        // Faders Together and Auto Swell on, so neither page rests.
        set(ParamIDs::rideMode, static_cast<float>(ParamIDs::RideTogether));
        set(ParamIDs::swellAuto, static_cast<float>(ParamIDs::AutoSwellClock));
        set(ParamIDs::busDetail[0][ParamIDs::FilterType], 1.0f); // High Pass
        closeAll();
        for (const auto t : { Layers::Target { Layers::Kind::Together, 0 }, Layers::Target { Layers::Kind::Swell, 0 } })
        {
            open(t);
            const auto name = "layer-" + slug(Layers::title(t)) + "-on";
            shots.crop(shots.shoot("window-" + name), name, panelOf(t));
        }
        closeAll();

        // The front with things behind it: bus A's Filter a High Pass, bus B's Swirl a Tape Flanger, Faders
        // Together, Auto Swell on a clock: four filled notches, Swell's line saying so.
        {
            const auto window = shots.shoot("front-notches-filled");
            shots.crop(window, "front-notches-filled-ride", ed.m_performPanel);
            shots.crop(window, "front-notches-filled-buses", ed.m_sections[0].bounds.getUnion(ed.m_sections[1].bounds));
        }

        // Three layers up at once, with a wire on Swell and one on bus B's Varispeed.
        {
            const int swellWire = proc.getMods().connect(0, ParamIDs::swell, 0.5f);
            const int leanWire = proc.getMods().connect(1, ParamIDs::busDetail[1][ParamIDs::Varispeed], 0.4f);
            open({ Layers::Kind::Swell, 0 });
            open({ Layers::Kind::Swirl, 0 });
            open({ Layers::Kind::Swirl, 1 });
            ed.m_wires.rebuild();
            rig.run(0.5);
            const auto window = shots.shoot("layers-three");
            shots.crop(window, "layers-three-row", ed.m_performPanel.getUnion(ed.m_sections[1].bounds));
            closeAll();
            ed.m_wires.rebuild();
            const auto front = shots.shoot("layers-three-closed");
            shots.crop(front, "layers-three-closed-row", ed.m_performPanel.getUnion(ed.m_sections[1].bounds));
            proc.getMods().disconnect(leanWire);
            proc.getMods().disconnect(swellWire);
        }

        // The Output EQ's plate over Bus B: flat as it opens, then with a curve, a Low Cut and a band picked.
        {
            ed.setEqPlateOpen(true);
            rig.run(0.5);
            shots.crop(shots.shoot("eq-plate-flat"), "eq-plate-flat-row", ed.m_sections[1].bounds.getUnion(ed.m_sections[3].bounds));
            set(ParamIDs::eqLowCut, 80.0f);
            set(ParamIDs::eqLowMidGain, -6.0f);
            set(ParamIDs::eqHighShelfGain, 4.0f);
            ed.m_eqPlate->getGraph().selectBand(OutputEq::LowMid);
            rig.run(0.5);
            const auto window = shots.shoot("eq-plate");
            shots.crop(window, "eq-plate-row", ed.m_sections[1].bounds.getUnion(ed.m_sections[3].bounds));
            ed.setEqPlateOpen(false);
            rig.run(0.2);
            shots.crop(shots.shoot("eq-key-curve"), "eq-key-curve", ed.m_sections[3].bounds);
        }

        // Back as a new session has it.
        for (const auto* id : { ParamIDs::rideMode, ParamIDs::swellAuto, ParamIDs::busDetail[0][ParamIDs::FilterType],
                                ParamIDs::busDetail[1][ParamIDs::SwirlType], ParamIDs::eqLowCut, ParamIDs::eqLowMidGain,
                                ParamIDs::eqHighShelfGain })
            if (auto* param = proc.getState().getParameter(id))
                param->setValueNotifyingHost(param->getDefaultValue());
        rig.run(0.3);
    }
}

namespace
{
    /** The Followers' Pass and Filter inputs and the Tape Flanger's With the Filter: the Follower card's menu open
        (follow-menu.png), the card on Bus A Pass and on Bus A Filter with its scope drawing them
        (follow-pass.png, follow-filter.png), bus A's Tape Flanger page with Motion's menu open (motion-menu.png), with
        With the Filter in use (motion-with-filter.png) and with it resting because Filter is at 0%
        (motion-with-filter-resting.png). Everything is put back as it was. */
    void shootFollowers(Shots& shots, Rig& rig, Proc& proc)
    {
        auto& ed = *rig.editor;
        auto set = [&](const char* id, float plain)
        {
            auto* param = proc.getState().getParameter(id);
            param->setValueNotifyingHost(param->convertTo0to1(plain));
        };
        auto settle = [&]()
        {
            for (auto& host : ed.m_layers)
                host->finishFold();
            ed.applyLayers();
            ed.updateNotches(true);
            rig.run(0.4);
        };
        // A box's own menu, open inside the window under the box, with the choice in use ticked (as the box shows it).
        auto shootMenu = [&](juce::ComboBox& box, const juce::String& name, juce::Rectangle<int> cropTo)
        {
            juce::PopupMenu menu = *box.getRootMenu();
            for (juce::PopupMenu::MenuItemIterator it(menu, true); it.next();)
                it.getItem().isTicked = it.getItem().itemID == box.getSelectedId();
            std::set<juce::Component*> before;
            for (auto* child : ed.getChildren())
                before.insert(child);
            menu.setLookAndFeel(&box.getLookAndFeel());
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(box).withParentComponent(&ed).withMinimumWidth(box.getWidth()), [](int) {});
            juce::Component* window = nullptr;
            for (auto* child : ed.getChildren())
                if (before.count(child) == 0 && child->isVisible())
                    window = child;
            juce::Rectangle<int> menuArea; // in the window's own (unscaled) coordinates
            if (window != nullptr)
            {
                // Under the box, or over it where there is no room under (as a box near the bottom of a screen does).
                const auto under = ed.getLocalArea(&box, box.getLocalBounds());
                const int height = juce::roundToInt(static_cast<float>(window->getHeight()) * rig.scale);
                const int top = under.getBottom() + height <= ed.getHeight() ? under.getBottom() : under.getY() - height;
                window->setTransform({});
                window->setTopLeftPosition(under.getX(), top);
                window->setTransform(juce::AffineTransform::scale(rig.scale, rig.scale, static_cast<float>(under.getX()), static_cast<float>(top)));
                menuArea = (juce::Rectangle<int>(under.getX(), top, juce::roundToInt(static_cast<float>(window->getWidth()) * rig.scale), height).toFloat() / rig.scale).getSmallestIntegerContainer();
            }
            const auto image = shots.shoot("window-" + name, window);
            shots.crop(image, name, cropTo.getUnion(menuArea));
            juce::PopupMenu::dismissAllActiveMenus();
            rig.run(0.2);
        };

        // The Follower card (Mod 4 in a new session).
        const int source = 3;
        std::vector<juce::ComboBox*> followBoxes;
        std::function<void(juce::Component&)> walk = [&](juce::Component& c)
        {
            for (auto* child : c.getChildren())
            {
                if (auto* box = dynamic_cast<juce::ComboBox*>(child); box != nullptr && box->getNumItems() > 0 && box->getItemText(0) == "Bus A")
                    followBoxes.push_back(box);
                walk(*child);
            }
        };
        walk(ed.m_modPanel);
        proc.setPlaying(true);
        if (followBoxes.size() == 4)
        {
            auto& follow = *followBoxes[static_cast<size_t>(source)];
            set(ParamIDs::modType[source], 2.0f);
            set(ParamIDs::modKnob[source], 0.9f);
            const int wire = proc.getMods().connect(source, ParamIDs::bus[0][ParamIDs::Swirl], 0.6f);
            set(ParamIDs::modFollow[source], static_cast<float>(ModMatrix::FollowPassA));
            ed.m_wires.rebuild();
            rig.run(4.0);
            shots.crop(shots.shoot("window-follow-pass"), "follow-pass", ed.m_modSection);
            shootMenu(follow, "follow-menu", ed.m_modSection);
            set(ParamIDs::busDetail[0][ParamIDs::FilterSweep], 0.5f);
            set(ParamIDs::modFollow[source], static_cast<float>(ModMatrix::FollowFilterA));
            rig.run(4.0);
            shots.crop(shots.shoot("window-follow-filter"), "follow-filter", ed.m_modSection);
            proc.getMods().disconnect(wire);
            ed.m_wires.rebuild();
            for (const auto* id : { ParamIDs::modType[source], ParamIDs::modKnob[source], ParamIDs::modFollow[source] })
                proc.getState().getParameter(id)->setValueNotifyingHost(proc.getState().getParameter(id)->getDefaultValue());
        }
        else
        {
            std::cout << "FAULT in follow-menu: the four Follower menus were not found" << std::endl;
            ++shots.faults;
        }

        // Bus A's Tape Flanger page: Motion's menu, With the Filter in use, and resting with Filter at 0%.
        {
            const Layers::Target t { Layers::Kind::Swirl, 0 };
            const auto* filterParam = proc.getState().getParameter(ParamIDs::bus[0][ParamIDs::Filter]);
            const float filterWas = filterParam->convertFrom0to1(filterParam->getValue());
            set(ParamIDs::busDetail[0][ParamIDs::SwirlType], static_cast<float>(ParamIDs::SwirlTape));
            set(ParamIDs::busDetail[0][ParamIDs::TapeMotion], static_cast<float>(ParamIDs::TapeWithFilter));
            set(ParamIDs::busDetail[0][ParamIDs::FilterSweep], 0.5f);
            ed.openLayer(t);
            settle();
            const auto panel = ed.layerPanelBounds(Editor::panelFor(t));
            shots.crop(shots.shoot("window-motion-with-filter"), "motion-with-filter", panel);
            juce::ComboBox* motion = nullptr;
            if (auto* page = ed.m_layers[static_cast<size_t>(Editor::panelFor(t))]->getPage())
                for (auto* child : page->getChildren())
                    if (auto* box = dynamic_cast<juce::ComboBox*>(child); box != nullptr && box->getTitle().endsWith(": Motion"))
                        motion = box;
            if (motion != nullptr)
                shootMenu(*motion, "motion-menu", panel);
            else
            {
                std::cout << "FAULT in motion-menu: the Motion box was not found" << std::endl;
                ++shots.faults;
            }
            set(ParamIDs::bus[0][ParamIDs::Filter], 0.0f);
            rig.run(0.5);
            shots.crop(shots.shoot("window-motion-with-filter-resting"), "motion-with-filter-resting", panel);
            set(ParamIDs::bus[0][ParamIDs::Filter], filterWas);
            for (auto& host : ed.m_layers)
                host->close();
            settle();
            for (const auto* id : { ParamIDs::busDetail[0][ParamIDs::SwirlType], ParamIDs::busDetail[0][ParamIDs::TapeMotion], ParamIDs::busDetail[0][ParamIDs::FilterSweep] })
                proc.getState().getParameter(id)->setValueNotifyingHost(proc.getState().getParameter(id)->getDefaultValue());
            rig.run(0.3);
        }
    }
}

namespace
{
    /** The manual's pictures of what is behind the notched controls, and of the Output EQ: one layer of each kind
        turned over (a Tape Flanger behind bus A's Swirl, bus A's Filter, Ride in Faders Together, Auto Swell on a
        clock) and the EQ's plate with a curve. Everything is put back as it was. */
    void shootDocLayers(Shots& shots, Rig& rig, Proc& proc)
    {
        auto& ed = *rig.editor;
        auto set = [&](const char* id, float plain)
        {
            auto* param = proc.getState().getParameter(id);
            param->setValueNotifyingHost(param->convertTo0to1(plain));
        };
        auto settle = [&]()
        {
            for (auto& host : ed.m_layers)
                host->finishFold();
            ed.applyLayers();
            ed.updateNotches(true);
            rig.run(0.4);
        };
        auto shot = [&](Layers::Target t, const char* name)
        {
            ed.openLayer(t);
            settle();
            shots.crop(shots.shoot(juce::String("window-") + name), name, ed.layerPanelBounds(Editor::panelFor(t)), 2);
            for (auto& host : ed.m_layers)
                host->close();
            settle();
        };
        const char* const changed[] = { ParamIDs::busDetail[0][ParamIDs::SwirlType], ParamIDs::rideMode, ParamIDs::swellAuto,
                                        ParamIDs::eqLowCut, ParamIDs::eqLowMidGain, ParamIDs::eqHighShelfGain };
        set(ParamIDs::busDetail[0][ParamIDs::SwirlType], static_cast<float>(ParamIDs::SwirlTape));
        shot({ Layers::Kind::Swirl, 0 }, "layer-swirl");
        set(ParamIDs::busDetail[0][ParamIDs::SwirlType], 0.0f);
        shot({ Layers::Kind::Filter, 0 }, "layer-filter");
        set(ParamIDs::rideMode, static_cast<float>(ParamIDs::RideTogether));
        rig.run(1.0);
        shot({ Layers::Kind::Together, 0 }, "layer-together");
        set(ParamIDs::rideMode, static_cast<float>(ParamIDs::RideApart));
        set(ParamIDs::swellAuto, static_cast<float>(ParamIDs::AutoSwellClock));
        shot({ Layers::Kind::Swell, 0 }, "layer-swell");
        set(ParamIDs::swellAuto, static_cast<float>(ParamIDs::AutoSwellOff));

        set(ParamIDs::eqLowCut, 80.0f);
        set(ParamIDs::eqLowMidGain, -6.0f);
        set(ParamIDs::eqHighShelfGain, 4.0f);
        ed.setEqPlateOpen(true);
        ed.m_eqPlate->getGraph().selectBand(OutputEq::LowMid);
        rig.run(0.6);
        shots.crop(shots.shoot("window-output-eq"), "output-eq", ed.m_sections[1].bounds.getUnion(ed.m_sections[3].bounds), 2);
        ed.m_eqPlate->getGraph().selectBand(-1);
        ed.setEqPlateOpen(false);
        for (const auto* id : changed)
            if (auto* param = proc.getState().getParameter(id))
                param->setValueNotifyingHost(param->getDefaultValue());
        ed.updateNotches(true);
        proc.getHistory().clear();
        rig.run(1.0);
    }

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

            // What is behind the notched controls, and the Output EQ.
            shootDocLayers(shots, rig, proc);

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

            // Surround: the track made 7.1.4 (as a host would), Centre Speaker on, the LFE on Mixdown and a little
            // Spread, so every lamp and meter has something to show. The room strip, Output with the speaker
            // meters, and the sea with its plan of the room while the passes fly Overhead; then back to Stereo.
            {
                auto widen = [&](room::Width width)
                {
                    auto layout = proc.getBusesLayout();
                    layout.outputBuses.getReference(0) = room::channelSet(width);
                    proc.setBusesLayout(layout);
                    proc.prepareToPlay(UiRig::c_fs, UiRig::c_block);
                    rig.block.setSize(room::layout(width).numChannels, UiRig::c_block);
                };
                auto set = [&](const char* id, float plain)
                {
                    auto* param = proc.getState().getParameter(id);
                    param->setValueNotifyingHost(param->convertTo0to1(plain));
                };
                auto paths = [&](int path)
                {
                    for (auto* id : ParamIDs::flightPath)
                        set(id, static_cast<float>(path));
                };
                widen(room::Width::Surround714);
                set(ParamIDs::roomCentre, 1.0f);
                set(ParamIDs::roomLfeMode, static_cast<float>(ParamIDs::numLfeModes - 1)); // Mixdown: the LFE meter moves
                set(ParamIDs::roomSpread, 0.25f);
                paths(0);
                rig.run(3.0);
                {
                    const auto window = shots.shoot("window-room");
                    shots.crop(window, "room-strip", ed.m_sea.getBounds().withTop(ed.m_roomStrip.getY() - 16), 2);
                    shots.crop(window, "output-room", ed.m_sections[3].bounds, 2);
                }
                // The manual's main picture: the window at 7.1.4 with both buses flying, the hits landing with their
                // pass, every speaker meter moving, the plan of the room lit and the mark in the stars. It replaces
                // the Stereo window shot above (the crops already taken from that one stay Stereo).
                {
                    set(ParamIDs::hitAim, 0.0f);
                    set(ParamIDs::gateMode, 3.0f); // Open: the hits sound, so they glow on the plan
                    set(ParamIDs::hitPosition, static_cast<float>(ParamIDs::HitWithPass));
                    ed.m_sea.showConstellation(true);
                    rig.run(2.0);
                    shots.shoot("screenshot");
                    ed.m_sea.showConstellation(false);
                    for (auto* id : { ParamIDs::hitAim, ParamIDs::gateMode, ParamIDs::hitPosition })
                        proc.getState().getParameter(id)->setValueNotifyingHost(proc.getState().getParameter(id)->getDefaultValue());
                }
                paths(2); // Overhead: the tops light on the plan
                rig.run(2.4);
                {
                    const auto window = shots.shoot("window-room-overhead");
                    shots.crop(window, "sea-room", ed.m_sea.getBounds(), 2);
                    shots.crop(window, "room-plan", ed.m_content.getLocalArea(&ed.m_sea, ed.m_sea.roomPlanArea().getSmallestIntegerContainer()), 6);
                }
                for (auto* id : { ParamIDs::roomCentre, ParamIDs::roomLfeMode, ParamIDs::roomSpread })
                    proc.getState().getParameter(id)->setValueNotifyingHost(proc.getState().getParameter(id)->getDefaultValue());
                paths(0);
                widen(room::Width::Stereo);
                rig.run(0.5);
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
        else if (a == "--layers") options.layersOnly = true;
        else if (a == "--follow") options.followOnly = true;
        else
        {
            std::cerr << "Unknown option " << a << "\nUsage: SwooshbucklerUiShot --out <dir> [--scale 1.0] [--seconds 3] [--state <session file>] "
                         "[--work <dir>] [--list] [--no-images] [--constellation] [--sea] [--docs] [--layers] [--follow]" << std::endl;
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

        // --layers: only the layers' pictures (shootLayers), and done.
        if (options.followOnly)
        {
            shootFollowers(shots, rig, proc);
            proc.setPlaying(false);
            const int found = shots.faults, counted = shots.texts;
            rig.editor.reset();
            std::cout << counted << " texts checked, " << found << " faults" << std::endl;
            return found == 0 ? 0 : 1;
        }
        if (options.layersOnly)
        {
            shootLayers(shots, rig, proc);
            proc.setPlaying(false);
            const int found = shots.faults, counted = shots.texts;
            rig.editor.reset();
            std::cout << counted << " texts checked, " << found << " faults" << std::endl;
            return found == 0 ? 0 : 1;
        }

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

        // The layers, the notches and the EQ plate (shootLayers).
        shootLayers(shots, rig, proc);
        // The Followers' Pass and Filter inputs and With the Filter (shootFollowers).
        shootFollowers(shots, rig, proc);

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

            // Wider than the rig plays: the order line at 5.1, Multi-Mono at 7.1.4, a 7.1.4 batch rendered, and FLAC,
            // which can't hold 7.1.4 in one file, said before anything is rendered.
            pick(v.m_width, static_cast<int>(room::Width::Surround51) + 1);
            rig.run(0.1);
            shots.shoot("variations-51", &v);
            pick(v.m_width, static_cast<int>(room::Width::Surround714) + 1);
            pick(v.m_layout, 2);
            rig.run(0.1);
            shots.shoot("variations-714-multimono", &v);
            pick(v.m_layout, 1);
            v.m_count.setText("2", true);
            v.startBatch();
            proc.getVariations().waitUntilDone(120000);
            rig.run(0.3);
            v.timerCallback();
            shots.shoot("variations-714-finished", &v);
            pick(v.m_layout, 2);
            v.startBatch();
            proc.getVariations().waitUntilDone(120000);
            rig.run(0.3);
            v.timerCallback();
            shots.shoot("variations-714-multimono-finished", &v);
            pick(v.m_layout, 1);
            proc.setFileType(2);
            v.readSettings();
            rig.run(0.1);
            shots.shoot("variations-714-flac", &v);
            proc.setFileType(0);
            pick(v.m_width, static_cast<int>(proc.getRoomWidth()) + 1); // back to following the rig
            v.m_count.setText("100", true);
            ed.m_variations.setVisible(false);

            // The rig in a 7.1.4 room (a host's track): the tape's settings, and takes of every kind in the list (the
            // room as played, a Stereo fold, a Multi-Mono set and a 7.1.4 variation).
            {
                auto widen = [&](room::Width width)
                {
                    auto layout = proc.getBusesLayout();
                    layout.outputBuses.getReference(0) = room::channelSet(width);
                    proc.setBusesLayout(layout);
                    proc.prepareToPlay(UiRig::c_fs, UiRig::c_block);
                    rig.block.setSize(room::layout(width).numChannels, UiRig::c_block);
                };
                widen(room::Width::Surround714);
                rig.run(2.0);
                ed.saveRetro();                                   // 7.1.4, as played
                proc.setTapeFold(Proc::TapeFold::Stereo);
                ed.saveRetro();                                   // a Stereo fold
                proc.setTapeFold(Proc::TapeFold::AsPlayed);
                proc.setFileLayout(takefile::Layout::MultiMono);
                ed.saveRetro();                                   // a Multi-Mono set
                proc.setFileLayout(takefile::Layout::Interleaved);
                proc.timerCallback();
                rig.run(0.2);
                ed.m_settings.refresh();
                ed.m_settings.setVisible(true);
                ed.m_settings.toFront(false);
                shots.shoot("settings-714", &ed.m_settings);
                proc.setTapeFold(Proc::TapeFold::Stereo);
                ed.m_settings.refresh();
                shots.shoot("settings-714-stereo-fold", &ed.m_settings);
                proc.setTapeFold(Proc::TapeFold::AsPlayed);
                proc.setFileType(2);
                ed.m_settings.refresh();
                shots.shoot("settings-714-flac", &ed.m_settings);
                proc.setFileType(0);
                ed.m_settings.setVisible(false);
                ed.showTakes();
                rig.run(0.2);
                shots.shoot("takes-714", &ed.m_takes);
                ed.m_takes.setVisible(false);
                widen(room::Width::Stereo);
                rig.run(0.2);
            }

            // The room on screen: the strip resting at Stereo, the window at 5.1 and at 7.1.4 with passes in flight on
            // each Flight Path, the speaker meters, the plan of the room close up, and Stowaway's Fly in a room.
            {
                auto widen = [&](room::Width width)
                {
                    auto layout = proc.getBusesLayout();
                    layout.outputBuses.getReference(0) = room::channelSet(width);
                    proc.setBusesLayout(layout);
                    proc.prepareToPlay(UiRig::c_fs, UiRig::c_block);
                    rig.block.setSize(room::layout(width).numChannels, UiRig::c_block);
                };
                auto set = [&](const char* id, float plain)
                {
                    auto* param = proc.getState().getParameter(id);
                    param->setValueNotifyingHost(param->convertTo0to1(plain));
                };
                auto paths = [&](int path)
                {
                    for (auto* id : ParamIDs::flightPath)
                        set(id, static_cast<float>(path));
                };
                auto planCrop = [&](const juce::Image& window, const juce::String& name)
                {
                    shots.crop(window, name, ed.m_content.getLocalArea(&ed.m_sea, ed.m_sea.roomPlanArea().getSmallestIntegerContainer()), 6);
                };
                proc.setPlaying(true);
                rig.run(0.5);
                {
                    const auto window = shots.shoot("window-room-stereo");
                    shots.crop(window, "room-strip-stereo", ed.m_sea.getBounds().withTop(ed.m_roomStrip.getY() - 16), 2);
                    shots.crop(window, "bus-a-stereo", ed.m_sections[0].bounds);
                }

                widen(room::Width::Surround51);
                set(ParamIDs::roomCentre, 1.0f);
                set(ParamIDs::roomLfeMode, static_cast<float>(ParamIDs::numLfeModes - 1)); // Mixdown: the LFE meter moves
                paths(0);
                rig.run(3.0);
                {
                    const auto window = shots.shoot("window-51");
                    shots.crop(window, "sea-51", ed.m_sea.getBounds());
                    shots.crop(window, "room-strip-51", ed.m_sea.getBounds().withTop(ed.m_roomStrip.getY() - 16), 2);
                    shots.crop(window, "output-51", ed.m_sections[3].bounds);
                    planCrop(window, "sea-plan-51");
                }
                // Discrete with Stowaway off: nothing feeds the LFE, and the strip says so where LFE Level was, with the
                // fix; Render Variations says it too, under its Channels.
                set(ParamIDs::roomLfeMode, 1.0f);
                rig.run(0.3);
                {
                    const auto window = shots.shoot("window-51-lfe-unfed");
                    shots.crop(window, "room-strip-51-lfe-unfed", ed.m_sea.getBounds().withTop(ed.m_roomStrip.getY() - 16), 2);
                    auto& v = ed.m_variations;
                    v.setVisible(true);
                    v.toFront(false);
                    rig.run(0.2);
                    shots.shoot("variations-51-lfe-unfed", &v);
                    v.setVisible(false);
                }
                set(ParamIDs::roomLfeMode, static_cast<float>(ParamIDs::numLfeModes - 1));

                widen(room::Width::Surround714);
                set(ParamIDs::roomSpread, 0.25f);
                const char* pathNames[] = { "across", "through", "overhead", "around" };
                for (int path = 0; path < ParamIDs::numFlightPaths; ++path)
                {
                    paths(path);
                    rig.run(path == 0 ? 3.0 : 2.4);
                    const auto window = shots.shoot(juce::String("window-714-") + pathNames[path]);
                    shots.crop(window, juce::String("sea-714-") + pathNames[path], ed.m_sea.getBounds());
                    planCrop(window, juce::String("sea-plan-714-") + pathNames[path]);
                    if (path == 0)
                    {
                        shots.crop(window, "output-714", ed.m_sections[3].bounds);
                        shots.crop(window, "room-strip-714", ed.m_sea.getBounds().withTop(ed.m_roomStrip.getY() - 16), 2);
                        shots.crop(window, "bus-a-714", ed.m_sections[0].bounds);
                        shots.crop(window, "bus-b-714", ed.m_sections[1].bounds);
                    }
                }

                // Output at 7.1 too (5.1 and 7.1.4 are above), and where a hit lands on the plan: the gate held open so the
                // hits always play, a hit in front at Hit Spread 0% (a small glow) and 100% (the whole room), and one
                // going with its pass at the default.
                {
                    widen(room::Width::Surround71);
                    paths(0);
                    rig.run(2.0);
                    shots.crop(shots.shoot("window-71"), "output-71", ed.m_sections[3].bounds);
                    widen(room::Width::Surround714);
                    set(ParamIDs::hitAim, 0.0f);
                    set(ParamIDs::gateMode, 3.0f);
                    set(ParamIDs::hitPosition, static_cast<float>(ParamIDs::HitFront));
                    for (const float spread : { 0.0f, 1.0f })
                    {
                        set(ParamIDs::hitSpread, spread);
                        rig.run(2.0);
                        const juce::String which = spread > 0.5f ? "100" : "0";
                        {
                            const auto flash = ed.m_sea.planFlash();
                            const auto hit = proc.getEngine().getHitView();
                            std::cout << "hit flash at Hit Spread " << which << "%: level " << flash.level << ", radius " << flash.radiusX << " x " << flash.radiusY
                                      << " (the hits' level " << hit.level << ", spread " << hit.spread << ")" << std::endl;
                        }
                        planCrop(shots.shoot("window-714-hit-" + which), "sea-plan-714-hit-" + which);
                    }
                    set(ParamIDs::hitPosition, static_cast<float>(ParamIDs::HitWithPass));
                    set(ParamIDs::hitSpread, ParamIDs::hitSpreadDefault);
                    rig.run(2.0);
                    {
                        const auto window = shots.shoot("window-714-hit-with-pass");
                        planCrop(window, "sea-plan-714-hit-with-pass");
                        shots.crop(window, "hits-714", ed.m_sections[2].bounds);
                    }
                    for (auto* id : { ParamIDs::hitAim, ParamIDs::gateMode, ParamIDs::hitPosition, ParamIDs::hitSpread })
                        proc.getState().getParameter(id)->setValueNotifyingHost(proc.getState().getParameter(id)->getDefaultValue());
                    rig.run(0.3);
                }

                // Direction and Side held: bus A Through, front to back, on your right, with a modulator deciding bus A's
                // Direction pass by pass; Link on, so bus B shows (and flies) bus A's. Then back as they were.
                {
                    set(ParamIDs::flightPath[0], 1.0f);
                    set(ParamIDs::direction[0], 1.0f);
                    set(ParamIDs::side[0], 2.0f);
                    set(ParamIDs::linkBuses, 1.0f);
                    rig.run(1.0);
                    const auto held = shots.shoot("window-714-held");
                    shots.crop(held, "bus-a-714-held", ed.m_sections[0].bounds);
                    shots.crop(held, "bus-b-714-linked", ed.m_sections[1].bounds);
                    const int wire = proc.getMods().connect(3, ParamIDs::direction[0], 1.0f);
                    ed.m_wires.rebuild();
                    rig.run(1.0);
                    ed.m_wires.rebuild();
                    shots.crop(shots.shoot("window-714-direction-wired"), "bus-a-714-direction-wired", ed.m_sections[0].bounds.withTrimmedTop(80));
                    proc.getMods().disconnect(wire);
                    ed.m_wires.rebuild();
                    for (auto* id : { ParamIDs::direction[0], ParamIDs::side[0], ParamIDs::linkBuses })
                        proc.getState().getParameter(id)->setValueNotifyingHost(proc.getState().getParameter(id)->getDefaultValue());
                    paths(0);
                    rig.run(0.3);
                }

                // Stowaway's Fly in the room, its plate open with its own Flight Path.
                proc.setTrackBus(1, CargoHold::c_stowBus);
                proc.setStowawayOn(true);
                set(ParamIDs::stowMode, static_cast<float>(ParamIDs::StowFly));
                set(ParamIDs::stowFlightPath, 3.0f);
                proc.setStowawayPanel(1);
                rig.run(3.0);
                {
                    const auto window = shots.shoot("window-714-stowaway-fly");
                    shots.crop(window, "stowaway-fly-714", ed.m_stowPanel);
                    shots.crop(window, "sea-714-stowaway-fly", ed.m_sea.getBounds());
                }
                // Folded: its note names Fly's Flight Path and way.
                set(ParamIDs::stowDirection, 2.0f);
                proc.setStowawayPanel(0);
                rig.run(1.0);
                shots.crop(shots.shoot("window-714-stowaway-fly-folded"), "stowaway-fly-714-folded", ed.m_stowPanel);
                set(ParamIDs::stowDirection, 0.0f);
                proc.setStowawayOn(false);
                proc.setTrackBus(1, -1);
                set(ParamIDs::stowMode, 0.0f);
                set(ParamIDs::stowFlightPath, 0.0f);
                proc.setStowawayPanel(0);
                for (auto* id : { ParamIDs::roomCentre, ParamIDs::roomLfeMode, ParamIDs::roomSpread })
                    proc.getState().getParameter(id)->setValueNotifyingHost(proc.getState().getParameter(id)->getDefaultValue());
                paths(0);
                widen(room::Width::Stereo);
                rig.run(0.5);

                // The Hits panel with each Hit Position (at Stereo, where they work: the hit moves between left and
                // right), then at Mono, where Hit Position and Hit Spread rest and the group says why.
                {
                    const char* names[ParamIDs::numHitPositions] = { "with-pass", "front", "random" };
                    for (int position = 0; position < ParamIDs::numHitPositions; ++position)
                    {
                        set(ParamIDs::hitPosition, static_cast<float>(position));
                        rig.run(0.2);
                        shots.crop(shots.shoot(juce::String("window-hits-") + names[position]), juce::String("hits-") + names[position], ed.m_sections[2].bounds);
                    }
                    set(ParamIDs::hitPosition, 0.0f);
                    widen(room::Width::Mono);
                    rig.run(0.5);
                    shots.crop(shots.shoot("window-mono"), "hits-mono", ed.m_sections[2].bounds);
                    widen(room::Width::Stereo);
                    rig.run(0.5);
                }
            }

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

    // The app: its own rig, played through AppOutput from a pretend audio device (tools/FakeAudioDevice.h), as
    // the standalone app plays it. Listening in Settings, the audio device dialog, and the footer saying when the
    // room is folded down because the device has too few outputs.
    {
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
        std::unique_ptr<juce::AudioProcessor> base(createPluginFilter());
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        auto& proc = *dynamic_cast<Proc*>(base.get());
        proc.setListening(room::Width::Stereo);
        proc.setRecordFolder(options.work.getChildFile("Recordings"));
        DemoSounds::loadInto(proc);

        juce::AudioDeviceManager devices;
        devices.addAudioDeviceType(std::make_unique<FakeAudio::Type>());
        devices.initialise(0, 2, nullptr, true);
        FakeAudio::choose(devices, "Fake 16", 2);
        juce::AudioProcessorPlayer player;
        devices.addAudioCallback(&player);
        auto audio = std::make_unique<AppAudio>(proc, devices, player);
        audio->start();
        Proc::showAudioSettings = [] {}; // the app has an Audio Device button

        Rig rig(proc, options.scale);
        Shots shots { rig, options };
        rig.open();
        auto& ed = *rig.editor;
        // Plays from the pretend device, the window's timers stepped as it goes.
        auto run = [&](double seconds)
        {
            juce::AudioBuffer<float> block;
            const int blocks = static_cast<int>(seconds * FakeAudio::c_rate / FakeAudio::c_buffer);
            for (int i = 0; i < blocks; ++i)
            {
                if (auto* device = FakeAudio::current(devices))
                    device->pull(block);
                if ((i & 3) == 0)
                    tick(rig.editor.get());
            }
            tick(rig.editor.get());
        };
        auto listen = [&](room::Width width)
        {
            proc.setListening(width);
            for (int i = 0; i < 8 && ! audio->output().isFadedOut(); ++i)
                run(FakeAudio::c_buffer / FakeAudio::c_rate);
            audio->finishChange();
        };
        auto settingsShot = [&](const juce::String& name)
        {
            run(0.5);
            ed.m_settings.refresh();
            ed.m_settings.setVisible(true);
            ed.m_settings.toFront(false);
            shots.shoot(name, &ed.m_settings);
            ed.m_settings.setVisible(false);
        };
        auto deviceShot = [&](const juce::String& name)
        {
            AppAudio::DevicePanel panel(devices, proc);
            UiProbe::Recorder recorder;
            const auto image = recorder.paint(panel, rig.scale);
            shots.report(name, recorder);
            if (options.images)
                save(image, options.out.getChildFile(name + ".png"));
        };
        proc.setPlaying(true);
        run(1.0);
        settingsShot("settings-app");                     // Stereo on the first two of sixteen outputs
        {
            const auto window = shots.shoot("window-app");   // the room strip in the app: Speakers is a menu
            shots.crop(window, "room-strip-app", ed.m_sea.getBounds().withTop(ed.m_roomStrip.getY() - 16), 2);
        }
        {
            // Changing Speakers while the Last 60 s memory holds something asks first (the change empties it).
            std::unique_ptr<juce::AlertWindow> dialog(ed.makeSpeakersDialog(room::Width::Surround51));
            UiProbe::Recorder recorder;
            const auto image = recorder.paint(*dialog, rig.scale);
            shots.report("speakers-dialog", recorder);
            if (options.images)
                save(image, options.out.getChildFile("speakers-dialog.png"));
        }
        listen(room::Width::Surround51);
        settingsShot("settings-app-51");                  // 5.1: outputs 1 to 6 switched on for it
        deviceShot("audio-device-51");
        listen(room::Width::Surround714);
        settingsShot("settings-app-714");                 // 7.1.4 on twelve of the sixteen outputs
        deviceShot("audio-device-714");
        FakeAudio::choose(devices, "Fake 2", 2);
        audio->deviceChanged();
        settingsShot("settings-app-714-folded");          // 7.1.4 on a two-output device: the fold, said in brass
        deviceShot("audio-device-714-folded");
        {
            run(0.5);
            const auto window = shots.shoot("window-app-folded");
            shots.crop(window, "footer-app-folded", ed.m_footer, 4);
            shots.crop(window, "room-strip-app-714", ed.m_sea.getBounds().withTop(ed.m_roomStrip.getY() - 16), 2);
        }
        proc.setPlaying(false);
        faults += shots.faults;
        texts += shots.texts;
        rig.editor.reset();
        Proc::showAudioSettings = nullptr;
        audio.reset();
        devices.removeAudioCallback(&player);
        devices.closeAudioDevice();
    }
    std::cout << texts << " texts checked at scale " << juce::String(options.scale, 2) << ": "
              << (faults == 0 ? juce::String("every one fits its box and follows the capitals rule.") : juce::String(faults) + " faults.") << std::endl;
    return faults == 0 ? 0 : 1;
}

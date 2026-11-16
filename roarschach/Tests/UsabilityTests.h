// The usability items of 1.3.1: the flat mode menus and their stepping, holds and Shift in Randomise
// All, knob handling, solo, and the parameter names a host shows. Included by RoarschachTests.cpp
// inside its anonymous namespace, after its helpers (check, makeProcessor, getParam, setParam,
// GestureLog, mouseEventFor, presetIndex ...). Groups: "editor" and "ui" (and "params" for the names).

namespace usability
{
    using Editor = RoarschachAudioProcessorEditor;

    struct OpenEditor
    {
        explicit OpenEditor(RoarschachAudioProcessor& p) : base(p.createEditor())
        {
            editor = dynamic_cast<Editor*>(base.get());
            editor->setSize(Editor::c_baseWidth, Editor::c_baseHeight);
        }
        SignalFlowStrip& strip() { return RoarschachEditorInspector::strip(*editor); }

        std::unique_ptr<juce::AudioProcessorEditor> base;
        Editor* editor = nullptr;
    };

    juce::MouseWheelDetails wheelOf(float deltaY, bool smooth)
    {
        juce::MouseWheelDetails w {};
        w.deltaX = 0.0f;
        w.deltaY = deltaY;
        w.isReversed = false;
        w.isSmooth = smooth;
        w.isInertial = false;
        return w;
    }

    /** A mouse event for a wheel turn over `c`, with its own event time (the wheel steps once per time). */
    juce::MouseEvent wheelEventFor(juce::Component& c, juce::ModifierKeys mods = {})
    {
        static juce::int64 ms = 1000;
        const auto at = juce::Time(ms += 10);
        const auto pos = c.getLocalBounds().getCentre().toFloat();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                &c, &c, at, pos, at, 0, false);
    }

    /** Every parameter set in the log was begun before and ended after, once each per begin. */
    bool gesturesBalanced(const GestureLog& log, int& sets)
    {
        std::map<int, int> open;
        sets = 0;
        for (const auto& [kind, index] : log.events)
        {
            if (kind == 'b')
                ++open[index];
            else if (kind == 'e')
            {
                if (--open[index] < 0)
                    return false;
            }
            else
            {
                ++sets;
                if (open[index] <= 0)
                    return false;
            }
        }
        for (const auto& [index, n] : open)
            if (n != 0)
                return false;
        return true;
    }

    //==========================================================================
    // U5: one flat mode menu with a section per choice parameter; the wheel and Up / Down on the chip
    // step the first choice without opening the menu.
    void testModeMenus()
    {
        std::printf("[editor] mode menus: one flat list, the wheel and Up / Down step the first choice\n");
        auto p = makeProcessor(48000.0, 512);
        OpenEditor open(*p);
        auto& strip = open.strip();

        int nested = 0, wrongSections = 0, wrongHeaders = 0, missingTip = 0;
        for (int m = 0; m < Params::numModules; ++m)
        {
            const auto& spec = Params::modules[static_cast<size_t>(m)];
            if (spec.chipIds[0] == nullptr)
                continue;
            std::vector<std::pair<const char*, int>> map;
            const auto menu = strip.buildModeMenu(m, map);
            juce::StringArray headers;
            int items = 0, expectedItems = 0;
            for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
            {
                const auto& item = it.getItem();
                nested += item.subMenu != nullptr ? 1 : 0;
                if (item.isSectionHeader)
                    headers.add(item.text);
                else if (item.itemID > 0)
                    ++items;
            }
            for (const char* id : spec.chipIds)
                if (id != nullptr)
                    expectedItems += Params::spec(id).numChoices;
            int numParams = 0;
            for (const char* id : spec.chipIds)
                numParams += id != nullptr ? 1 : 0;
            if (headers.size() != numParams || items != expectedItems)
            {
                ++wrongSections;
                std::printf("    %s: %d headers for %d parameters, %d items for %d choices\n", spec.code, headers.size(), numParams, items, expectedItems);
            }
            for (const auto& h : headers)
                if (!h.startsWith(juce::String(spec.uiName) + " ") || h != h.toUpperCase())
                {
                    ++wrongHeaders;
                    std::printf("    %s: header \"%s\" is not the module's name and a caption in capitals\n", spec.code, h.toRawUTF8());
                }
            const auto tip = strip.getModeChip(m)->getTooltip();
            if (!tip.contains("Scroll over the chip") || !tip.contains("Up or Down"))
                ++missingTip;
        }
        check(nested == 0, "mode menus: no submenus (" + juce::String(nested) + ")");
        check(wrongSections == 0, "mode menus: one section per choice parameter, every choice listed once");
        check(wrongHeaders == 0, "mode menus: each section is headed by the module's name and the parameter's caption, in capitals");
        check(missingTip == 0, "mode chips: the tooltip says how to step a choice with the wheel and Up / Down");

        // SWIRL: the wheel steps Circuit, one notch a step, and stops at the ends.
        const int swirl = static_cast<int>(Params::ModuleId::swirl);
        auto* chip = strip.getModeChip(swirl);
        setParam(*p, Params::swirlCircuit, 0.0f);
        const float motionBefore = getParam(*p, Params::swirlMotion);
        GestureLog log;
        p->addListener(&log);
        chip->mouseWheelMove(wheelEventFor(*chip), wheelOf(-0.1f, false));   // down: the next choice
        const int afterDown = juce::roundToInt(getParam(*p, Params::swirlCircuit));
        chip->mouseWheelMove(wheelEventFor(*chip), wheelOf(-0.1f, false));
        const int afterTwo = juce::roundToInt(getParam(*p, Params::swirlCircuit));
        chip->mouseWheelMove(wheelEventFor(*chip), wheelOf(0.1f, false));    // up: back one
        const int afterUp = juce::roundToInt(getParam(*p, Params::swirlCircuit));
        p->removeListener(&log);
        int sets = 0;
        const bool balanced = gesturesBalanced(log, sets);
        std::printf("  SWIRL Circuit by the wheel: %d, %d, then back to %d (%d values set, gestures %s)\n", afterDown, afterTwo, afterUp, sets,
                    balanced ? "balanced" : "NOT balanced");
        check(afterDown == 1 && afterTwo == 2 && afterUp == 1, "mode chip: a wheel notch down steps the first choice to the next, up to the previous");
        check(balanced && sets == 3, "mode chip: each step is one gesture on the choice parameter");
        check(juce::approximatelyEqual(getParam(*p, Params::swirlMotion), motionBefore), "mode chip: the wheel leaves the other choices alone");

        // A trackpad: small smooth deltas add up to one step; a glide after lifting the fingers makes none.
        setParam(*p, Params::swirlCircuit, 0.0f);
        for (int k = 0; k < 4; ++k)
            chip->mouseWheelMove(wheelEventFor(*chip), wheelOf(-0.01f, true));
        const int partial = juce::roundToInt(getParam(*p, Params::swirlCircuit));
        chip->mouseWheelMove(wheelEventFor(*chip), wheelOf(-0.012f, true));
        const int whole = juce::roundToInt(getParam(*p, Params::swirlCircuit));
        auto glide = wheelOf(-0.2f, true);
        glide.isInertial = true;
        chip->mouseWheelMove(wheelEventFor(*chip), glide);
        const int glided = juce::roundToInt(getParam(*p, Params::swirlCircuit));
        check(partial == 0 && whole == 1 && glided == 1, "mode chip: a trackpad's small scrolls add up to one step, its glide adds none ("
                                                             + juce::String(partial) + ", " + juce::String(whole) + ", " + juce::String(glided) + ")");

        // The ends: no wrap.
        setParam(*p, Params::swirlCircuit, 5.0f);
        chip->mouseWheelMove(wheelEventFor(*chip), wheelOf(-0.1f, false));
        const int atEnd = juce::roundToInt(getParam(*p, Params::swirlCircuit));
        setParam(*p, Params::swirlCircuit, 0.0f);
        chip->mouseWheelMove(wheelEventFor(*chip), wheelOf(0.1f, false));
        const int atStart = juce::roundToInt(getParam(*p, Params::swirlCircuit));
        check(atEnd == 5 && atStart == 0, "mode chip: stepping stops at the first and the last choice");

        // Up / Down while the chip has the focus; with a modifier the key goes on.
        setParam(*p, Params::swirlCircuit, 2.0f);
        const bool usedDown = static_cast<juce::Component*>(chip)->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        const int keyDown = juce::roundToInt(getParam(*p, Params::swirlCircuit));
        const bool usedUp = static_cast<juce::Component*>(chip)->keyPressed(juce::KeyPress(juce::KeyPress::upKey));
        const int keyUp = juce::roundToInt(getParam(*p, Params::swirlCircuit));
        const bool passedOn = !static_cast<juce::Component*>(chip)->keyPressed(juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::altModifier, 0));
        check(usedDown && usedUp && keyDown == 3 && keyUp == 2 && passedOn, "mode chip: Down steps to the next choice and Up to the previous; Alt+Down goes on");

        // A single-choice chip (DE-ESS) steps its only choice; RING's steps Carrier (its first).
        setParam(*p, Params::deessMode, 0.0f);
        auto* deessChip = strip.getModeChip(static_cast<int>(Params::ModuleId::deess));
        deessChip->mouseWheelMove(wheelEventFor(*deessChip), wheelOf(-0.1f, false));
        setParam(*p, Params::ringCarrier, 0.0f);
        setParam(*p, Params::ringTrack, 0.0f);
        auto* ringChip = strip.getModeChip(static_cast<int>(Params::ModuleId::ring));
        ringChip->mouseWheelMove(wheelEventFor(*ringChip), wheelOf(-0.1f, false));
        check(juce::roundToInt(getParam(*p, Params::deessMode)) == 1 && juce::roundToInt(getParam(*p, Params::ringCarrier)) == 1
                  && juce::roundToInt(getParam(*p, Params::ringTrack)) == 0,
              "mode chip: a one-choice chip steps its choice; RING's steps Carrier and leaves Track");
    }

    //==========================================================================
    // U6: Randomise All holds modules (a die's right-click menu: view state, not a parameter, never
    // marking the session changed) and, Shift-clicked, rolls only the modules that are on.
    struct HostChanges final : juce::AudioProcessorListener
    {
        int stateChanges = 0, parameterChanges = 0;
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override { ++parameterChanges; }
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& d) override { stateChanges += d.nonParameterStateChanged ? 1 : 0; }
    };

    std::vector<float> moduleValues(RoarschachAudioProcessor& p, int module, bool withOn)
    {
        const auto& spec = Params::modules[static_cast<size_t>(module)];
        std::vector<float> v;
        if (withOn)
            v.push_back(getParam(p, spec.onId));
        for (const char* id : spec.knobIds)
            if (id != nullptr)
                v.push_back(getParam(p, id));
        for (const char* id : spec.chipIds)
            if (id != nullptr)
                v.push_back(getParam(p, id));
        return v;
    }

    int modulesOn(RoarschachAudioProcessor& p)
    {
        int n = 0;
        for (const auto& m : Params::modules)
            n += getParam(p, m.onId) >= 0.5f ? 1 : 0;
        return n;
    }

    void testRollHolds()
    {
        std::printf("[editor] Randomise All: holds from a die's menu, Shift rolls only the modules that are on\n");
        const int swirl = static_cast<int>(Params::ModuleId::swirl), pitch = static_cast<int>(Params::ModuleId::pitch);

        // The die's menu: Hold in Randomise All on a module's die only; it toggles the hold and the mark.
        {
            auto p = makeProcessor(48000.0, 512);
            OpenEditor open(*p);
            auto& strip = open.strip();
            HostChanges changes;
            p->addListener(&changes);
            runLoop(20);
            changes.stateChanges = 0;

            const auto holdItem = [](const juce::PopupMenu& menu) -> std::optional<bool>
            {
                for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
                    if (it.getItem().text == "Hold in Randomise All")
                        return it.getItem().isTicked;
                return std::nullopt;
            };
            auto& die = *strip.getModuleDice(swirl);
            const auto before = holdItem(RoarschachEditorInspector::diceMenu(*open.editor, die));
            const auto onAll = holdItem(RoarschachEditorInspector::diceMenu(*open.editor, *strip.getRollAllDice()));
            const auto onPreset = holdItem(RoarschachEditorInspector::diceMenu(*open.editor, RoarschachEditorInspector::presetDice(*open.editor)));
            RoarschachEditorInspector::applyDiceMenu(*open.editor, die, 3);
            runLoop(60);
            const auto after = holdItem(RoarschachEditorInspector::diceMenu(*open.editor, die));
            const bool held = p->getRollHolds()[static_cast<size_t>(swirl)] && die.isHeld() && !strip.getModuleDice(pitch)->isHeld();
            check(before.has_value() && !*before && after.has_value() && *after && held,
                  "dice: a module die's menu has Hold in Randomise All; choosing it holds the module and marks its die");
            check(!onAll.has_value() && !onPreset.has_value(), "dice: Randomise All's and the preset die's menus have no hold item");
            check(die.getTooltip().startsWith("Held:") && strip.getModuleDice(pitch)->getTooltip().contains("Hold in Randomise All"),
                  "dice: a held die's tooltip says it is held; every module die's tooltip names the hold");
            check(changes.stateChanges == 0 && changes.parameterChanges == 0,
                  "dice: holding a module tells the host nothing (" + juce::String(changes.stateChanges) + " state changes, "
                      + juce::String(changes.parameterChanges) + " parameter changes)");

            // Saved with the session, read back by a new instance; a state without holds has none; a
            // preset load keeps them (they are not part of a sound).
            juce::MemoryBlock state;
            p->getStateInformation(state);
            auto q = makeProcessor(48000.0, 512);
            q->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            const auto restored = q->getRollHolds();
            auto fresh = makeProcessor(48000.0, 512);
            juce::MemoryBlock plain;
            fresh->getStateInformation(plain);
            q->setStateInformation(plain.getData(), static_cast<int>(plain.getSize()));
            const auto cleared = q->getRollHolds();
            p->setCurrentProgram(presetIndex("Kaiju"));
            const bool keptOverPreset = p->getRollHolds()[static_cast<size_t>(swirl)];
            int restoredCount = 0, clearedCount = 0;
            for (int m = 0; m < Params::numModules; ++m)
            {
                restoredCount += restored[static_cast<size_t>(m)] ? 1 : 0;
                clearedCount += cleared[static_cast<size_t>(m)] ? 1 : 0;
            }
            check(restored[static_cast<size_t>(swirl)] && restoredCount == 1 && clearedCount == 0 && keptOverPreset,
                  "dice: holds are saved with the session, a session without them has none, and a preset load keeps them");

            // A session's holds reach an open editor on its next tick; choosing the item again lets go.
            RoarschachEditorInspector::tick(*open.editor);
            RoarschachEditorInspector::applyDiceMenu(*open.editor, die, 3);
            check(!p->getRollHolds()[static_cast<size_t>(swirl)] && !die.isHeld(), "dice: choosing Hold in Randomise All again lets the module go");
            p->removeListener(&changes);
        }

        // The held mark: drawn inside the die's pill, clear of the mode chip, and it changes the picture.
        {
            auto p = makeProcessor(48000.0, 512);
            OpenEditor open(*p);
            auto& strip = open.strip();
            auto& die = *strip.getModuleDice(swirl);
            const auto plain = die.createComponentSnapshot(die.getLocalBounds(), true, 2.0f);
            die.setHeld(true);
            const auto held = die.createComponentSnapshot(die.getLocalBounds(), true, 2.0f);
            int changed = 0;
            for (int y = 0; y < plain.getHeight(); ++y)
                for (int x = 0; x < plain.getWidth(); ++x)
                    changed += plain.getPixelAt(x, y) != held.getPixelAt(x, y) ? 1 : 0;
            const auto mark = die.getHeldMarkBounds();
            auto* tile = strip.getModuleTile(swirl);
            const auto markInTile = tile->getLocalArea(&die, mark);
            const auto chip = strip.getModeChip(swirl)->getBounds().toFloat();
            const bool inside = die.getPillBounds().expanded(0.01f).contains(mark) && tile->getLocalBounds().toFloat().contains(markInTile);
            std::printf("  held mark: %.1f x %.1f at (%.1f, %.1f) in the die; %d pixels changed at 2x; chip ends at %.1f, mark starts at %.1f\n",
                        static_cast<double>(mark.getWidth()), static_cast<double>(mark.getHeight()), static_cast<double>(mark.getX()), static_cast<double>(mark.getY()),
                        changed, static_cast<double>(chip.getRight()), static_cast<double>(markInTile.getX()));
            check(changed > 20 && inside && markInTile.getX() > chip.getRight(), "dice: the held mark shows, inside the die's pill and its tile, clear of the mode chip");
            die.setHeld(false);
        }

        // Rolls with a hold: the held module never changes (switch and settings); the others are rolled,
        // and three to seven modules are on.
        {
            auto p = makeProcessor(48000.0, 512);
            auto& dice = p->getDice();
            Params::Random::ModuleSet held {};
            held[static_cast<size_t>(swirl)] = true;
            int heldMoved = 0, othersMoved = 0, countWrong = 0, rolls = 0;
            for (int seed = 1; seed <= 30; ++seed)
            {
                dice.setSeed(static_cast<std::uint32_t>(seed));
                setParam(*p, Params::swirlOn, seed % 2 == 0 ? 1.0f : 0.0f);
                setParam(*p, Params::swirl, 33.0f);
                const auto heldBefore = moduleValues(*p, swirl, true);
                const auto otherBefore = moduleValues(*p, pitch, false);
                rolls += dice.rollAll(held) ? 1 : 0;
                heldMoved += moduleValues(*p, swirl, true) != heldBefore ? 1 : 0;
                othersMoved += moduleValues(*p, pitch, false) != otherBefore ? 1 : 0;
                const int on = modulesOn(*p);
                countWrong += on < Params::Random::minModulesOn || on > Params::Random::maxModulesOn ? 1 : 0;
            }
            std::printf("  30 rolls holding SWIRL: SWIRL moved in %d, PITCH rolled in %d, %d with a count of modules on outside 3-7\n", heldMoved, othersMoved, countWrong);
            check(rolls == 30 && heldMoved == 0 && othersMoved == 30 && countWrong == 0,
                  "dice: Randomise All leaves a held module exactly as it is, rolls the rest, and keeps three to seven modules on");

            // Nothing held: the same roll as ever for a seed.
            auto a = makeProcessor(48000.0, 512), b = makeProcessor(48000.0, 512);
            a->getDice().setSeed(7);
            b->getDice().setSeed(7);
            a->getDice().rollAll();
            b->getDice().rollAll(Params::Random::ModuleSet {}, false);
            check(normalisedState(*a) == normalisedState(*b), "dice: with nothing held Randomise All rolls what it always rolled for a seed");
        }

        // Shift-click: only the modules that are on are rolled; every switch and every module that is off stays.
        {
            auto p = makeProcessor(48000.0, 512);
            OpenEditor open(*p);
            auto& strip = open.strip();
            p->getDice().setSeed(11);
            for (int m = 0; m < Params::numModules; ++m)
                setParam(*p, Params::modules[static_cast<size_t>(m)].onId, m % 3 == 0 ? 1.0f : 0.0f);
            std::vector<std::vector<float>> before;
            for (int m = 0; m < Params::numModules; ++m)
                before.push_back(moduleValues(*p, m, true));
            strip.rollAllClicked(juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
            int offMoved = 0, switchesMoved = 0, onRolled = 0, onCount = 0;
            for (int m = 0; m < Params::numModules; ++m)
            {
                const auto now = moduleValues(*p, m, true);
                const bool on = m % 3 == 0;
                switchesMoved += !juce::approximatelyEqual(now[0], before[static_cast<size_t>(m)][0]) ? 1 : 0;
                if (on)
                {
                    ++onCount;
                    onRolled += now != before[static_cast<size_t>(m)] ? 1 : 0;
                }
                else
                    offMoved += now != before[static_cast<size_t>(m)] ? 1 : 0;
            }
            const auto undoName = p->getDice().getUndoManager().getUndoDescription();
            std::printf("  Shift-click: %d of %d modules on rolled, %d off modules moved, %d switches moved (undo: \"%s\")\n", onRolled, onCount, offMoved,
                        switchesMoved, undoName.toRawUTF8());
            check(onRolled == onCount && offMoved == 0 && switchesMoved == 0, "dice: Shift-click on Randomise All rolls the modules that are on and nothing else");
            const bool undone = p->getDice().undo();
            const auto back = moduleValues(*p, 0, true);
            bool same = back.size() == before[0].size();
            for (size_t k = 0; same && k < back.size(); ++k)
                same = std::abs(back[k] - before[0][k]) < 1.0e-3f * (1.0f + std::abs(before[0][k]));
            check(undone && same, "dice: a Shift roll is one step in the dice's undo history");

            // With every module off (or held) there is nothing to roll: no step in the history.
            for (int m = 0; m < Params::numModules; ++m)
                setParam(*p, Params::modules[static_cast<size_t>(m)].onId, 0.0f);
            check(!p->getDice().rollAll({}, true), "dice: a Shift roll with no module on does nothing");
        }
    }
}

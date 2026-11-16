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

    //==========================================================================
    // U7: knobs: Shift-drag is fine, the wheel moves in small steps on a grid (Shift finer), right-click
    // resets or types a value; PITCH takes whole semitones and single cents without a fight.
    juce::MouseEvent knobEvent(juce::Component& c, juce::Point<float> pos, juce::Point<float> down, juce::ModifierKeys mods, bool dragged)
    {
        static juce::int64 ms = 500000;
        const auto at = juce::Time(ms += 10);
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                &c, &c, at, down, at, 1, dragged);
    }

    /** Drags a knob straight up by `pixels` in 1 px steps (with Shift from step `shiftFrom` on), returning
        the parameter's real value after every step. */
    std::vector<float> dragKnob(RoarschachAudioProcessor& p, juce::Slider& knob, const char* id, int pixels, int shiftFrom = 1 << 20)
    {
        const juce::ModifierKeys left(juce::ModifierKeys::leftButtonModifier);
        const juce::ModifierKeys leftShift(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
        const auto start = knob.getLocalBounds().getCentre().toFloat();
        knob.mouseDown(knobEvent(knob, start, start, left, false));
        std::vector<float> values;
        for (int k = 1; k <= pixels; ++k)
        {
            knob.mouseDrag(knobEvent(knob, start - juce::Point<float>(0.0f, static_cast<float>(k)), start, k >= shiftFrom ? leftShift : left, true));
            values.push_back(getParam(p, id));
        }
        knob.mouseUp(knobEvent(knob, start - juce::Point<float>(0.0f, static_cast<float>(pixels)), start, left, true));
        return values;
    }

    void testKnobs()
    {
        std::printf("[editor] knobs: Shift-drag, the wheel, the right-click menu, PITCH by semitones and cents\n");
        auto p = makeProcessor(48000.0, 512);
        OpenEditor open(*p);
        auto& strip = open.strip();
        auto* pitchKnob = strip.getKnobFor(Params::pitch);
        auto* swirlKnob = strip.getKnobFor(Params::swirl);
        auto* rateKnob = strip.getKnobFor(Params::swirlRate);
        auto* trimKnob = strip.getKnobFor(Params::inTrim);
        auto* pushKnob = strip.getKnobFor(Params::outPush);
        check(pitchKnob != nullptr && swirlKnob != nullptr && rateKnob != nullptr && trimKnob != nullptr && pushKnob != nullptr,
              "knobs: every tile's knobs, IN's and OUTPUT's included, are found by parameter");
        if (pitchKnob == nullptr || swirlKnob == nullptr || rateKnob == nullptr || trimKnob == nullptr || pushKnob == nullptr)
            return;

        // Shift-drag: a fraction of the travel; Shift pressed in the middle of a drag does not jump.
        setParam(*p, Params::swirl, 20.0f);
        const float normal = dragKnob(*p, *swirlKnob, Params::swirl, 20).back() - 20.0f;
        setParam(*p, Params::swirl, 20.0f);
        const float fine = dragKnob(*p, *swirlKnob, Params::swirl, 20, 1).back() - 20.0f;
        setParam(*p, Params::swirl, 20.0f);
        const auto mixed = dragKnob(*p, *swirlKnob, Params::swirl, 20, 11);
        const float jump = mixed[10] - mixed[9];
        std::printf("  SWIRL Amount, 20 px up: %.2f %% plain, %.2f %% with Shift; the first pixel after Shift went down mid-drag moved %.2f %%\n",
                    static_cast<double>(normal), static_cast<double>(fine), static_cast<double>(jump));
        check(normal > 5.0f && fine > 0.05f * normal && fine < 0.15f * normal, "knobs: Shift-drag moves about a tenth as far");
        check(jump >= 0.0f && jump < 0.1f, "knobs: pressing Shift in the middle of a drag goes on from where the knob is, at the fine rate");

        // PITCH with Shift: every cent on the way is reachable (no cent skipped), under half a cent a pixel.
        setParam(*p, Params::pitch, 0.0f);
        const auto cents = dragKnob(*p, *pitchKnob, Params::pitch, 60, 1);
        std::set<int> visited;
        for (const float v : cents)
            visited.insert(juce::roundToInt(v * 100.0f));
        const int first = *visited.begin(), last = *visited.rbegin();
        const bool everyCent = static_cast<int>(visited.size()) == last - first + 1;
        setParam(*p, Params::pitch, 0.0f);
        const float plainPitch = dragKnob(*p, *pitchKnob, Params::pitch, 10).back();
        std::printf("  PITCH: 60 px with Shift went %d cents (%d distinct values, every cent: %s); 10 px plain went %.2f st\n", last - first,
                    static_cast<int>(visited.size()), everyCent ? "yes" : "no", static_cast<double>(plainPitch));
        check(everyCent && last - first >= 10 && last - first <= 40, "knobs: PITCH with Shift steps through every cent, at most two thirds of a cent a pixel");

        // The wheel: PITCH a semitone (onto whole semitones), with Shift a cent.
        GestureLog log;
        setParam(*p, Params::pitch, -2.37f);
        p->addListener(&log);
        pitchKnob->mouseWheelMove(wheelEventFor(*pitchKnob), wheelOf(0.1f, false));
        const float up1 = getParam(*p, Params::pitch);
        pitchKnob->mouseWheelMove(wheelEventFor(*pitchKnob), wheelOf(0.1f, false));
        const float up2 = getParam(*p, Params::pitch);
        pitchKnob->mouseWheelMove(wheelEventFor(*pitchKnob, juce::ModifierKeys(juce::ModifierKeys::shiftModifier)), wheelOf(0.1f, false));
        const float cent = getParam(*p, Params::pitch);
        auto sideways = wheelOf(0.0f, false);
        sideways.deltaX = 0.1f;   // (Shift turns a mouse wheel sideways on some systems)
        pitchKnob->mouseWheelMove(wheelEventFor(*pitchKnob, juce::ModifierKeys(juce::ModifierKeys::shiftModifier)), sideways);
        const float cent2 = getParam(*p, Params::pitch);
        pitchKnob->mouseWheelMove(wheelEventFor(*pitchKnob), wheelOf(-0.1f, false));
        const float down = getParam(*p, Params::pitch);
        p->removeListener(&log);
        int sets = 0;
        const bool balanced = gesturesBalanced(log, sets);
        std::printf("  PITCH by the wheel from -2.37 st: %.2f, %.2f, Shift %.2f, Shift (sideways) %.2f, down %.2f (%d values set)\n", static_cast<double>(up1),
                    static_cast<double>(up2), static_cast<double>(cent), static_cast<double>(cent2), static_cast<double>(down), sets);
        const auto near = [](float a, float b) { return std::abs(a - b) < 0.002f; };
        check(near(up1, -2.0f) && near(up2, -1.0f) && near(cent, -0.99f) && near(cent2, -0.98f) && near(down, -1.0f),
              "knobs: the wheel moves PITCH onto the next whole semitone, and with Shift by one cent");
        check(balanced && sets == 5, "knobs: every wheel step is one gesture");

        // Other scales: percent by 1 (Shift 0.1), decibels by 0.5 (Shift 0.1), a log knob by a hundredth of its travel.
        setParam(*p, Params::swirl, 21.0f);
        swirlKnob->mouseWheelMove(wheelEventFor(*swirlKnob), wheelOf(0.1f, false));
        const float pct = getParam(*p, Params::swirl);
        swirlKnob->mouseWheelMove(wheelEventFor(*swirlKnob, juce::ModifierKeys(juce::ModifierKeys::shiftModifier)), wheelOf(0.1f, false));
        const float pctFine = getParam(*p, Params::swirl);
        setParam(*p, Params::inTrim, 0.0f);
        trimKnob->mouseWheelMove(wheelEventFor(*trimKnob), wheelOf(-0.1f, false));
        const float db = getParam(*p, Params::inTrim);
        auto* rate = p->getParameters()[Params::indexOf(Params::swirlRate)];
        setParam(*p, Params::swirlRate, 0.35f);
        const float n0 = rate->getValue();
        rateKnob->mouseWheelMove(wheelEventFor(*rateKnob), wheelOf(0.1f, false));
        const float n1 = rate->getValue();
        std::printf("  the wheel: SWIRL Amount 21 -> %.1f -> %.1f (Shift); IN -> %.1f dB; SWIRL Rate travel %.4f -> %.4f\n", static_cast<double>(pct),
                    static_cast<double>(pctFine), static_cast<double>(db), static_cast<double>(n0), static_cast<double>(n1));
        check(near(pct, 22.0f) && near(pctFine, 22.1f) && near(db, -0.5f) && n1 > n0 && n1 - n0 <= 0.0101f && n1 - n0 > 0.001f,
              "knobs: the wheel steps each scale by a small unit (a hundredth of the travel on a log knob), Shift by a tenth of it");
        // A trackpad's small scrolls add up; its glide after the fingers lift adds nothing.
        setParam(*p, Params::swirl, 30.0f);
        for (int k = 0; k < 3; ++k)
            swirlKnob->mouseWheelMove(wheelEventFor(*swirlKnob), wheelOf(0.02f, true));
        auto glide = wheelOf(0.3f, true);
        glide.isInertial = true;
        swirlKnob->mouseWheelMove(wheelEventFor(*swirlKnob), glide);
        check(near(getParam(*p, Params::swirl), 31.0f), "knobs: a trackpad scroll moves in whole steps and its glide adds none ("
                                                            + juce::String(getParam(*p, Params::swirl), 2) + ")");

        // The right-click menu: Reset to Default (with the default's value; off at the default) and Type a Value.
        setParam(*p, Params::outPush, 12.0f);
        const auto menu = strip.buildKnobMenu(Params::outPush);
        juce::String resetText;
        bool resetEnabled = false, hasType = false;
        for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
        {
            if (it.getItem().itemID == SignalFlowStrip::knobReset)
            {
                resetText = it.getItem().text;
                resetEnabled = it.getItem().isEnabled;
            }
            hasType = hasType || (it.getItem().itemID == SignalFlowStrip::knobType && it.getItem().text == "Type a Value");
        }
        GestureLog resetLog;
        p->addListener(&resetLog);
        strip.applyKnobMenu(Params::outPush, SignalFlowStrip::knobReset);
        p->removeListener(&resetLog);
        int resetSets = 0;
        const bool resetBalanced = gesturesBalanced(resetLog, resetSets);
        bool disabledAtDefault = false;
        const auto menuAtDefault = strip.buildKnobMenu(Params::outPush);
        for (juce::PopupMenu::MenuItemIterator it(menuAtDefault); it.next();)
            if (it.getItem().itemID == SignalFlowStrip::knobReset)
                disabledAtDefault = !it.getItem().isEnabled;
        std::printf("  OUTPUT Push's menu: \"%s\" (enabled %d, type %d, off at the default %d)\n", resetText.toRawUTF8(), resetEnabled ? 1 : 0, hasType ? 1 : 0, disabledAtDefault ? 1 : 0);
        check(resetText.startsWith("Reset to Default (") && resetText.contains("6.5") && resetEnabled && hasType && disabledAtDefault,
              "knobs: the right-click menu offers Reset to Default with the default's value (not at the default) and Type a Value");
        check(near(getParam(*p, Params::outPush), 6.5f) && resetBalanced && resetSets == 1, "knobs: Reset to Default is one gesture to the default");
        strip.applyKnobMenu(Params::outPush, SignalFlowStrip::knobType);
        bool editing = false;
        for (auto* c : pushKnob->getChildren())
            if (auto* label = dynamic_cast<juce::Label*>(c))
                editing = editing || label->isBeingEdited();
        check(editing, "knobs: Type a Value opens the knob's value for typing");
        pushKnob->hideTextBox(true);

        // Tooltips: every knob says how it handles; PITCH's names the semitone and the cent.
        int withLine = 0, knobs = 0;
        std::vector<juce::Component*> tiles { strip.getInTile(), strip.getOutputTile() };
        for (int m = 0; m < Params::numModules; ++m)
            tiles.push_back(strip.getModuleTile(m));
        for (auto* tile : tiles)
            for (const auto& [slider, id] : strip.getKnobs(tile))
            {
                ++knobs;
                const auto tip = slider->getTooltip();
                withLine += tip.contains("Shift-drag for fine control") && tip.contains("Right-click to reset it or type a value") ? 1 : 0;
            }
        check(withLine == knobs && pitchKnob->getTooltip().contains("a semitone at a time, with Shift a cent"),
              "knobs: every knob's tooltip explains Shift-drag, the wheel and the right-click menu (" + juce::String(withLine) + " of " + juce::String(knobs) + ")");
    }

    //==========================================================================
    // U8: Alt-click a module's On switch to solo it; Alt-click again to turn back on what was on.
    std::array<bool, Params::numModules> onSet(RoarschachAudioProcessor& p)
    {
        std::array<bool, Params::numModules> on {};
        for (int m = 0; m < Params::numModules; ++m)
            on[static_cast<size_t>(m)] = getParam(p, Params::modules[static_cast<size_t>(m)].onId) >= 0.5f;
        return on;
    }

    std::array<bool, Params::numModules> only(int module)
    {
        std::array<bool, Params::numModules> on {};
        on[static_cast<size_t>(module)] = true;
        return on;
    }

    void altClick(juce::Component& c)
    {
        const juce::ModifierKeys mods(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier);
        const auto at = c.getLocalBounds().getCentre().toFloat();
        c.mouseDown(knobEvent(c, at, at, mods, false));
        c.mouseUp(knobEvent(c, at, at, juce::ModifierKeys(juce::ModifierKeys::altModifier), false));
    }

    /** One gesture over On switches only: every switch set is begun before any is set and ended after
        all are set, once each. */
    bool isOneSwitchGesture(const GestureLog& log, int& switches)
    {
        std::map<int, std::array<int, 3>> counts;
        int phase = 0;
        for (const auto& [kind, index] : log.events)
        {
            const int k = kind == 'b' ? 0 : kind == 's' ? 1 : 2;
            if (k < phase || Params::all[static_cast<size_t>(index)].kind != Params::Kind::toggle
                || !juce::String(Params::all[static_cast<size_t>(index)].id).endsWith("On"))
                return false;
            phase = k;
            ++counts[index][static_cast<size_t>(k)];
        }
        switches = static_cast<int>(counts.size());
        for (const auto& [index, c] : counts)
            if (c[0] != 1 || c[1] != 1 || c[2] != 1)
                return false;
        return true;
    }

    void testSolo()
    {
        std::printf("[editor] solo: Alt-click an On switch, again to come back\n");
        const int swirl = static_cast<int>(Params::ModuleId::swirl), pitch = static_cast<int>(Params::ModuleId::pitch);
        auto p = makeProcessor(48000.0, 512);
        p->setCurrentProgram(presetIndex("Kaiju"));
        auto open = std::make_unique<OpenEditor>(*p);
        auto* swirlOn = open->strip().getOnButton(swirl);
        setParam(*p, Params::swirlOn, 0.0f);   // (a module that is off solos too)
        const auto original = onSet(*p);

        GestureLog log;
        p->addListener(&log);
        altClick(*swirlOn);
        p->removeListener(&log);
        int switches = 0;
        const bool oneGesture = isOneSwitchGesture(log, switches);
        int originallyOn = 0;
        for (const bool b : original)
            originallyOn += b ? 1 : 0;
        std::printf("  Kaiju with SWIRL off: %d modules on; Alt-click SWIRL wrote %d switches in %s\n", originallyOn, switches, oneGesture ? "one gesture" : "MORE than one gesture");
        check(onSet(*p) == only(swirl) && p->getSoloModule() == swirl, "solo: Alt-click on an On switch leaves that module alone on (even one that was off)");
        check(oneGesture && switches == originallyOn + 1, "solo: every switch it changes is written in one gesture, and nothing else");

        GestureLog back;
        p->addListener(&back);
        altClick(*swirlOn);
        p->removeListener(&back);
        int backSwitches = 0;
        check(onSet(*p) == original && p->getSoloModule() < 0 && isOneSwitchGesture(back, backSwitches),
              "solo: Alt-click again turns back on exactly what was on, in one gesture");

        // Moving the solo keeps the first memory; the editor closing does not lose it.
        altClick(*swirlOn);
        altClick(*open->strip().getOnButton(pitch));
        const bool moved = onSet(*p) == only(pitch) && p->getSoloModule() == pitch;
        open.reset();
        open = std::make_unique<OpenEditor>(*p);
        altClick(*open->strip().getOnButton(pitch));
        check(moved && onSet(*p) == original, "solo: soloing another module moves the solo, and coming back from it restores the set before the first solo, "
                                               "with the editor closed and opened in between");

        // A preset load lets the memory go: Alt-click then solos afresh, and comes back to the preset's set.
        altClick(*open->strip().getOnButton(swirl));
        p->setCurrentProgram(presetIndex("Smashed"));
        const auto smashed = onSet(*p);
        const bool forgotten = p->getSoloModule() < 0;
        altClick(*open->strip().getOnButton(swirl));
        const bool fresh = onSet(*p) == only(swirl);
        altClick(*open->strip().getOnButton(swirl));
        check(forgotten && fresh && onSet(*p) == smashed, "solo: a preset load clears the remembered set; the next solo comes back to the loaded preset's modules");

        // A hand on another switch while soloed lets the memory go too (the solo no longer shows).
        altClick(*open->strip().getOnButton(swirl));
        setParam(*p, Params::pitchOn, 1.0f);
        const bool handForgets = p->getSoloModule() < 0;
        const auto byHand = onSet(*p);
        altClick(*open->strip().getOnButton(swirl));
        altClick(*open->strip().getOnButton(swirl));
        check(handForgets && onSet(*p) == byHand, "solo: switches changed by hand during a solo end it; the next solo remembers them as they are");

        check(swirlOn != nullptr && open->strip().getOnButton(swirl)->getTooltip().contains("Alt-click"), "solo: the On switch's tooltip explains Alt-click");
    }
}

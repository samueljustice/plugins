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
}

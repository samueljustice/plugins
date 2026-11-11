#pragma once

#include <JuceHeader.h>

#include "PresetManager.h"
#include "RandomRanges.h"

#include <cstdint>
#include <functional>
#include <random>
#include <vector>

/**
 * The three dice: a random factory preset, a whole new creature (ROLL ALL), and new settings for one
 * module. Owned by the processor, so a roll works the same with the editor shut (the tests) and
 * the undo history outlives the editor.
 *
 * - Every roll is one undoable step in its own UndoManager: undo puts back the parameters the roll
 *   changed and the preset that was showing (name, factory or user, edited flag); redo puts the roll
 *   back. A control turned by hand after a roll keeps the hand's value through undo and redo (and
 *   keeps the preset marked edited), whether or not the roll had changed it.
 * - A preset loaded from the plug-in's own preset bar (the menu, the arrows) goes through here too
 *   and is one step in the same history: undo brings back the sound that was showing, edits and
 *   all, and redo loads the preset again, so the two can be compared.
 * - A preset or session loaded any other way (the host's program list, a session recall, even the
 *   same one again) ends the history: undo then does nothing rather than mix two sounds.
 * - Every parameter a roll (or its undo) changes gets its own begin / set / end gesture, so the
 *   host records the change and its automation sees each parameter once.
 * - ROLL ALL and a module roll are edits, so the preset shows as edited ("*"). The preset die loads
 *   a preset like the preset menu.
 * - Nothing here touches the module positions, so a roll never moves a module. The chain still
 *   crossfades once if a roll changes which module makes the sound stereo (as turning ENSEMBLE on
 *   by hand does).
 *
 * The random numbers come from a std::mt19937 with a seed (setSeed for the tests; otherwise a
 * random one per instance) and are drawn with plain arithmetic (Params::Random), so a seed rolls the
 * same values on every platform.
 *
 * Message thread only.
 */
class DiceRoller
{
public:
    using HostNotifier = std::function<void(const juce::AudioProcessorListener::ChangeDetails&)>;

    DiceRoller(juce::AudioProcessorValueTreeState& parameters, PresetManager& presets, HostNotifier notifyHost = {});

    /** Restarts the random numbers (and the preset die's bag) from this seed. */
    void setSeed(std::uint32_t seed);

    /** Loads a random factory preset: never the one showing, and every factory preset comes up
        once before any comes up again (a shuffled bag). Returns its factory index, -1 if none. */
    int rollPreset();
    /** Turns a random set of modules on and rolls every module's knobs and choices (see
        Params::Random). The order, IN and OUTPUT are left alone. */
    void rollAll();
    /** Rolls one module's knobs and choices (not its On switch or its position). */
    void rollModule(int module);

    /** The preset bar's loads, each one undoable step ("Load"). They return what the PresetManager
        call of the same name returns; a load that fails leaves the history as it was. */
    bool loadPreset(int index);
    bool loadUserPreset(const juce::File& file);
    bool loadNext();
    bool loadPrevious();

    /** False once the host has loaded another preset or session since the last roll or load. */
    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();
    juce::UndoManager& getUndoManager() { return m_undo; }

    /** The face (1..6) the dice show: a new one after every roll. */
    int getFace() const { return m_face; }

    /** The preset die's next pick, without loading it (the bag moves on). For the tests. */
    int pickPreset(int current);

private:
    struct State
    {
        std::vector<float> normalised;   // every parameter, in the processor's order
        PresetManager::StateInfo preset;
    };

    class RollAction;

    State capture() const;
    /** The state before a roll or a load. A history that no longer belongs to the sound showing is
        dropped first, so the new step never sits on top of it. */
    State begin();
    bool load(const std::function<bool()>& loadIt);
    /** Sets the parameters in `changed` to the target's values and the preset to the target's.
        A parameter that no longer has the value `expected` gave it was turned by hand: it stays. */
    void restore(const State& target, const State& expected, const std::vector<int>& changed);
    /** Which preset is showing (not whether it is edited): the history belongs to it. */
    static juce::String presetKey(const PresetManager::StateInfo& info);
    bool historyLive() const;
    bool historyApplies();
    void applyRealValues(const Params::Random::Values& values);
    void commit(State before, const juce::String& name, bool newFace = true);

    juce::AudioProcessorValueTreeState& m_parameters;
    PresetManager& m_presets;
    const HostNotifier m_notifyHost;

    std::mt19937 m_rng;
    std::vector<int> m_bag;     // the factory presets still to come in this round
    int m_lastPick = -1;
    int m_face = 5;
    juce::UndoManager m_undo;
    juce::String m_historyPreset;   // the preset showing after the last roll, undo or redo
    std::uint32_t m_historyGeneration = 0;   // the presets' load generation then

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DiceRoller)
};

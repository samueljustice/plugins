#pragma once

#include <JuceHeader.h>
#include "Params.h"
#include "PositionWrites.h"
#include "Theme.h"
#include "dsp/MonsterFeatures.h"

#include <array>
#include <memory>
#include <vector>

/**
 * The signal chain: IN, the 14 movable modules in the order the audio takes them (two rows of
 * seven, joined by a return connector), and OUTPUT.
 *
 * Every module tile has a header (its On LED, its place in the chain, its name and a move chip),
 * its knobs, a mode chip (a menu of its choice parameters) and a footer readout. A tile is moved
 * by dragging its header across both rows, from the move chip's menu, or with Alt+Left/Right while
 * one of its controls has the keyboard focus. Every move writes the whole permutation as one host
 * gesture. Position changes from the host or a preset animate the tiles into place: the strip polls
 * the positions and never writes them in response.
 *
 * Message thread only. A timer polls the positions and the readouts (MonsterFeatures: relaxed
 * atomics written by the audio thread).
 */
class SignalFlowStrip : public juce::Component, private juce::Timer
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SignalFlowStrip)

public:
    /** positionWrites (optional) is held while a reorder writes the module positions. */
    SignalFlowStrip(juce::AudioProcessorValueTreeState& parameters, const roar::MonsterFeatures& features,
                    const Theme::Fonts& fonts, roar::PositionWrites* positionWrites = nullptr);
    ~SignalFlowStrip() override;

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    //==============================================================================
    // The chain order
    /** The resolved order (order[slot] = module), from the position parameters. */
    Params::Order getOrder() const;
    /** Writes a complete permutation as one gesture: begin on every position that changes, set
        them all, then end them all. Positions that stay are not touched. */
    void setOrder(const Params::Order& order);

    /** The move menu's item IDs. Move After uses moveAfterBase + the module it goes after. */
    enum MoveItem
    {
        moveToStart = 1, moveEarlier, moveLater, moveToEnd, toggleOnOff, resetOrder,
        moveAfterBase = 100
    };

    /** The order after a move menu item (pure; toggleOnOff leaves it as it is). */
    static Params::Order orderAfterMove(const Params::Order& order, int module, int item);
    juce::PopupMenu buildMoveMenu(int module) const;
    void applyMove(int module, int item);

    /** The mode chip's menu. itemMap[itemId] = (choice parameter, choice index). */
    juce::PopupMenu buildModeMenu(int module, std::vector<std::pair<const char*, int>>& itemMap) const;
    /** The mode chip's text: the current choices, compact. */
    juce::String modeChipText(int module, float maxWidth) const;

    void showMoveMenu(int module);
    void showModeMenu(int module);

    /** Menus open inside this component instead of on the desktop (a headless snapshot). */
    void setMenuParentForSnapshots(juce::Component* parent) { m_menuParent = parent; }

    //==============================================================================
    // Dragging a tile (the tile's header calls these; strip coordinates)
    void beginDrag(int module, juce::Point<float> position);
    void dragTo(juce::Point<float> position);
    void endDrag(bool commit);
    bool isDragging() const noexcept { return m_drag.module >= 0; }

    /** Puts every tile where it belongs now (no animation left). */
    void finishAnimations();
    /** Polls the positions and the readouts now (the timer does this 30 times a second). */
    void refresh() { timerCallback(); }

    //==============================================================================
    // For the tests and the snapshot harness
    juce::Component* getModuleTile(int module) const;
    juce::Component* getInTile() const;
    juce::Component* getOutputTile() const;
    juce::Button* getModeChip(int module) const;   // nullptr for a module without choices
    juce::Button* getMoveChip(int module) const;
    juce::Button* getOnButton(int module) const;
    juce::Rectangle<int> getSlotBounds(int slot) const;
    /** The knob sliders of a tile, with the parameter each one controls. */
    std::vector<std::pair<juce::Slider*, const char*>> getKnobs(juce::Component* tile) const;

    /** The tooltip of a parameter's control (plain words). Empty for an unknown ID. */
    static juce::String tooltipFor(const char* paramId);

    static constexpr int c_headerHeight = 26;

private:
    struct Knob;
    class Tile;
    class ModuleTile;
    class InTile;
    class OutputTile;
    class MoveChip;
    class DragShadow;

    void timerCallback() override;

    void layoutTiles(bool animate, double durationMs);
    void animateTile(int module, juce::Rectangle<int> target, double durationMs);
    void stepAnimations();
    int stereoSplitSlot(const Params::Order& order) const;
    int nearestSlot(juce::Point<float> centre) const;
    juce::PopupMenu::Options menuOptions(juce::Component& target) const;
    void setParameter(const char* id, float plainValue);
    float rawValue(const char* id) const;
    bool isModuleOn(int module) const;

    juce::AudioProcessorValueTreeState& m_parameters;
    const roar::MonsterFeatures& m_features;
    roar::PositionWrites* const m_positionWrites;
    const Theme::Fonts& m_fonts;

    std::unique_ptr<InTile> m_inTile;
    std::unique_ptr<OutputTile> m_outTile;
    std::array<std::unique_ptr<ModuleTile>, Params::numModules> m_tiles;
    std::unique_ptr<DragShadow> m_shadow;

    Params::Order m_order {};          // the order the parameters ask for (last poll)
    Params::Order m_shown {};          // the order the tiles show (m_order, or the drag preview)
    int m_split = Params::numModules;  // the slot whose module makes the chain stereo

    struct Drag
    {
        int module = -1;
        juce::Point<float> grabOffset;  // mouse position - tile position at the press
        int slot = -1;                  // the insertion slot shown
    };
    Drag m_drag;

    struct Animation
    {
        juce::Rectangle<float> from, to;
        double startMs = 0.0, durationMs = 0.0;
        bool active = false;
    };
    std::array<Animation, Params::numModules> m_animations {};

    class AnimationTimer : public juce::Timer
    {
    public:
        explicit AnimationTimer(SignalFlowStrip& s) : m_strip(s) {}
        void timerCallback() override { m_strip.stepAnimations(); }
    private:
        SignalFlowStrip& m_strip;
    };
    AnimationTimer m_animationTimer { *this };

    juce::Component* m_menuParent = nullptr;
    double m_lastTickMs = 0.0;
};

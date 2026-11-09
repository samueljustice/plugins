#pragma once

#include <JuceHeader.h>
#include "LookAndFeel.h"
#include "Modulators.h"
#include "PluginProcessor.h"
#include "Theme.h"

#include <map>

class ModWires;

/**
 * The modulation panel at the bottom of the window (MegaMod style): four
 * modulators side by side. Each has a jack: drag it onto any knob to wire it.
 */
class ModPanel : public juce::Component, private juce::Timer
{
public:
    ModPanel(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts, ModWires& wires);
    ~ModPanel() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    /** Centre of a modulator's jack, in this panel's coordinates. */
    juce::Point<float> getJackCentre(int source) const;
    static juce::Colour sourceColour(int source);
    static juce::String sourceName(int source) { return "MOD " + juce::String(source + 1); }

private:
    class Card;
    void timerCallback() override;

    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
    ModWires& m_wires;
    std::array<std::unique_ptr<Card>, ModMatrix::c_numSources> m_cards;
};

//==============================================================================
/**
 * The wires, drawn over the whole window: a sagging cable from each modulator's
 * jack to a plug on the knob it moves. Only the plugs take the mouse (everything
 * else clicks straight through):
 *   drag a plug up / down : depth (Shift = fine)
 *   Cmd-drag the control  : the same, for its newest wire (Ctrl on Windows)
 *   double-click a plug   : depth back to +50%
 *   right-click a plug    : invert, depths, remove
 * The cables are drawn once into an image and only redrawn when the wiring or the layout changes.
 */
class ModWires : public juce::Component, public juce::TooltipClient, private juce::AsyncUpdater
{
public:
    ModWires(ModMatrix& matrix, const Theme::Fonts& fonts);

    /** The controls a wire can land on (parameter ID -> control): the knobs and the tracks' Semitones boxes. They
        must be descendants of this overlay's parent. Also hooks up their Cmd-drag (Ctrl on Windows) depth gesture. */
    void setTargets(std::map<juce::String, ModTarget*> knobs);
    void setPanel(ModPanel* panel) { m_panel = panel; rebuild(); }
    void setShowCables(bool show) { m_showCables = show; rebuild(); }
    bool getShowCables() const { return m_showCables; }

    // Dragging a new wire out of a jack (positions in this overlay's coordinates).
    void beginWire(int source, juce::Point<float> from);
    void dragWire(juce::Point<float> to);
    void endWire(juce::Point<float> at);

    /** Wiring or layout changed: redraw the cables and update the knobs' depth arcs. */
    void rebuild();
    /** The same, once, a moment later: for bursts like scrolling the track list. */
    void rebuildSoon() { triggerAsyncUpdate(); }
    /** At the UI rate: the knobs' live modulated values. */
    void updateKnobs();

    void paint(juce::Graphics&) override;
    void resized() override { rebuild(); }
    bool hitTest(int x, int y) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    juce::String getTooltip() override;

    std::function<void(const juce::String&)> onStatus;
    /** LFO / Random swing both ways; Follower / Macro one way (decides how depth arcs are drawn). */
    std::function<bool(int source)> isSourceBipolar;

private:
    struct Plug
    {
        ModMatrix::Route route;
        juce::Point<float> pos;
        ModTarget* knob = nullptr;
    };

    juce::Rectangle<float> knobArea(ModTarget* knob) const;
    /** False when the control is inside a scrolling list (the track lanes) and scrolled out of its visible area. */
    static bool isTargetInView(const ModTarget* t);
    ModTarget* knobAt(juce::Point<float> p, juce::String* id = nullptr) const;
    const Plug* plugAt(juce::Point<float> p) const;
    juce::Path cablePath(juce::Point<float> from, juce::Point<float> to) const;
    static void drawCable(juce::Graphics&, const juce::Path&, juce::Colour, float alpha);
    void showPlugMenu(const Plug& plug);
    juce::String describe(const ModMatrix::Route& r) const;

    ModMatrix& m_matrix;
    const Theme::Fonts& m_fonts;
    ModPanel* m_panel = nullptr;
    std::map<juce::String, ModTarget*> m_knobs;
    std::vector<Plug> m_plugs;
    juce::Image m_cables;
    float m_scale = 2.0f;
    bool m_hasCables = false;
    int m_generation = 0;   // ModMatrix::generation() the plugs were built from
    /** A depth changed (no cable moves): refresh that plug, its bubble and its knob's arcs only. */
    void depthChanged(int slot, float depth);
    void updateArcs(ModTarget* knob);
    bool m_showCables = true;

    // Live drag of a new wire
    int m_dragSource = -1;
    juce::Point<float> m_dragFrom, m_dragTo;
    ModTarget* m_dragOver = nullptr;

    /** Cmd-drag on a modulated control (see ModTarget::onModDepthGesture): its newest wire's depth. */
    void modDepthGesture(ModTarget* target, const juce::MouseEvent& e, int phase);
    void handleAsyncUpdate() override { rebuild(); }
    /** The wire last added to this control if it's still there, else its last plug; nullptr if it has none in view. */
    const Plug* newestPlug(ModTarget* target) const;
    std::map<ModTarget*, int> m_newestSlot; // the slot of the wire last added to each control (for Cmd-drag)

    // Depth drag on a plug (or a Cmd-drag on its control)
    int m_depthSlot = -1;
    float m_depthStart = 0.0f;
    juce::Point<float> m_depthPos;
};

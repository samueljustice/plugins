#pragma once

#include <JuceHeader.h>
#include "Theme.h"
#include "dsp/MonsterFeatures.h"

#include <array>
#include <memory>

/**
 * The signal chain, left to right: one panel per stage (IN, PITCH, GROWL, LIQUID, ROAR, SUB, OUT) with
 * chevrons between them, and the two movable modules (DETAIL and VOCODER) drawn in the slot they sit in.
 *
 * Moving a module: drag its panel by the header and drop it on one of the markers that appear in the
 * gaps, or pick a position from the menu of the header's "..." chip (also reachable with Tab and Return).
 * Either way the slot parameter is set as one complete host gesture. The strip follows the slot
 * parameters (a click, automation, a preset), sliding the panels to their new places.
 *
 * Message thread only. A timer polls the level meters and the vocoder bands (MonsterFeatures, atomics
 * written by the audio thread) and repaints only what moved.
 */
class SignalFlowStrip : public juce::Component, private juce::Timer
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SignalFlowStrip)

public:
    enum class Module { detail, vocoder };

    SignalFlowStrip(juce::AudioProcessorValueTreeState& parameters, const roar::MonsterFeatures& features,
                    const Theme::Fonts& fonts);
    ~SignalFlowStrip() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    /** Opens the "Move to" menu of a movable module (what its header chip does). */
    void showMoveMenu(Module module);
    /** That menu (items are slot + 1, the current slot ticked) and where it opens. */
    juce::PopupMenu buildMoveMenu(Module module);
    juce::PopupMenu::Options moveMenuOptions(Module module);

    /** Where a module's panel is (or is heading), in local coordinates; its header chip. */
    juce::Rectangle<int> getModuleBounds(Module module) const;
    juce::Component& getMoveChip(Module module);

    /** Lays the panels out at once (no slide), e.g. after the slots were set while hidden. */
    void finishAnimations();

private:
    enum PanelId { inPanel, pitchPanel, growlPanel, liquidPanel, roarPanel, subPanel, outPanel,
                   detailPanel, vocoderPanel, numPanels };

    enum class Format { Percent, Semitones };

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    /** A panel of the chain. It paints its frame and its readouts through the strip; movable
        panels hand their header's mouse events to the strip, which runs the drag. */
    class Panel : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        Panel(SignalFlowStrip& owner, PanelId id, juce::String title, juce::Colour accent);

        void paint(juce::Graphics&) override;
        void mouseMove(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;

        bool isMovable() const noexcept { return m_id == detailPanel || m_id == vocoderPanel; }
        bool isInHeader(juce::Point<int> p) const;

        const PanelId m_id;
        const juce::String m_title;
        const juce::Colour m_accent;
        bool m_headerHot = false;

    private:
        SignalFlowStrip& m_owner;
    };

    /** The "..." chip in a movable panel's header: opens its "Move to" menu. */
    class MoveChip final : public juce::Button
    {
    public:
        MoveChip(juce::Colour accent);
        void paintButton(juce::Graphics&, bool isMouseOverButton, bool isButtonDown) override;

    private:
        juce::Colour m_accent;
    };

    void timerCallback() override;

    void setupKnob(Knob& knob, Panel& panel, const juce::String& name, const juce::String& paramId,
                   const juce::String& tooltip, juce::Colour accent, Format format);

    // Layout
    std::array<juce::Rectangle<int>, numPanels> computeLayout(int detailSlot, int vocoderSlot) const;
    void relayout(bool animate);
    void layoutPanelContents(Panel& panel);
    juce::Rectangle<int> knobRowArea(const Panel& panel) const;
    juce::Rectangle<int> readoutArea(const Panel& panel) const;

    // Painting
    void paintPanel(juce::Graphics&, const Panel& panel);
    void paintReadout(juce::Graphics&, const Panel& panel, juce::Rectangle<float> area);
    void paintMeterRow(juce::Graphics&, juce::Rectangle<float> area, const juce::String& caption,
                       const juce::String& value, float fill01, juce::Colour accent, bool centredFill = false);
    void paintChevron(juce::Graphics&, float x, float y) const;
    void paintDropMarkers(juce::Graphics&);
    void paintOverChildren(juce::Graphics&) override;

    // Moving the modules
    int getSlot(Module module) const;
    void setSlot(Module module, int slot);
    void panelMouseDown(Panel& panel, const juce::MouseEvent& e);
    void panelMouseDrag(Panel& panel, const juce::MouseEvent& e);
    void panelMouseUp(Panel& panel, const juce::MouseEvent& e);
    int nearestSlot(float x) const;
    float slotMarkerX(int slot) const;
    Panel& panelFor(Module module) { return *m_panels[static_cast<size_t>(module == Module::detail ? detailPanel : vocoderPanel)]; }
    static Module moduleOf(PanelId id) { return id == detailPanel ? Module::detail : Module::vocoder; }

    juce::AudioProcessorValueTreeState& m_parameters;
    const roar::MonsterFeatures& m_features;
    const Theme::Fonts& m_fonts;

    std::array<std::unique_ptr<Panel>, numPanels> m_panels;

    Knob m_pitch, m_size, m_growl, m_liquid, m_roar, m_sub, m_mix, m_detailLow, m_detailHigh, m_gravel;

    MoveChip m_detailChip { Theme::Colours::detail }, m_vocoderChip { Theme::Colours::vocoder };
    juce::TextButton m_octaveButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_octaveAttachment;

    std::unique_ptr<juce::ParameterAttachment> m_detailSlotAttachment, m_vocoderSlotAttachment;
    int m_detailSlot = 0, m_vocoderSlot = 0;

    // Panel positions: where each one is drawn now, and where it is sliding to.
    std::array<juce::Rectangle<float>, numPanels> m_current, m_target;
    bool m_animating = false;
    bool m_laidOut = false;

    // Drag of a movable panel by its header.
    PanelId m_dragPanel = numPanels;   // numPanels: no drag
    bool m_dragStarted = false;
    int m_dropSlot = -1;
    juce::Point<float> m_dragOffset;    // mouse position in the panel at the press

    // Meters and readouts (smoothed for display).
    float m_inDb = -100.0f, m_outDb = -100.0f;
    float m_growlActivity = 0.0f, m_roarDrive = 0.0f, m_subLevel = 0.0f, m_liquidPhase = 0.0f;
    float m_boostDb = 0.0f, m_cutDb = 0.0f, m_f0 = 0.0f;
    bool m_voiced = false, m_gravelActive = false;
    std::array<float, roar::MonsterFeatures::numBands> m_bands {};
    int m_tick = 0;
    int m_lastGeneration = -1, m_staleTicks = 0; // feature freshness (host stopped processing)
};

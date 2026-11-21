#pragma once

#include <JuceHeader.h>
#include "Parameters.h"
#include "Theme.h"

#include <functional>
#include <memory>
#include <vector>

class SwooshbucklerAudioProcessor;
class ModTarget;

/**
 * THE DETAIL LAYER (a stub: it is never shown and draws nothing; filled in by the layers' work package, which also
 * owns LookAndFeel, PluginEditor and ModPanel).
 *
 * A layer opens in place, inside one panel: the panel's knob row and title-bar controls give way to what is behind
 * one notched control (Layers::Target, Parameters.h), while the panel's scope strip stays. One layer per panel;
 * opening another swaps it. The open state is never saved.
 *
 *   [ front wheel 64 ][ choices column 100: up to three 22 px boxes or keys ][ up to five 56 px cells ]   (a bus)
 *   [ box 100 ][ up to four 44 px cells ]                                                                  (Ride)
 *
 * Title "Bus A / Swirl" (Layers::title), then Reset (Layers::detailIds back to default, one undo step; the front
 * wheel is not touched) and Back. The front wheel stays in the first cell, live. Close: Back, Esc, or
 * double-clicking the front wheel again. It folds with the Stowaway plate's 180 ms Motion::Fade.
 *
 * What the filler (the layers' package) must provide: the host below (opening, closing, Reset, Back, Esc, the fold,
 * hiding the panel's front controls so their cables hide); the notch on notched wheels and its gestures (double-
 * click or the notch opens, Shift-double-click resets, the right-click menu); registering a page's wireTargets()
 * as wire targets while the page is open.
 *
 * What the pages (LayerPages, the pages' package) rely on: LayerPage below, unchanged. The host gives a page its
 * bounds (everything right of the front wheel's cell, the height of the knob row) and calls refresh() about 30
 * times a second while it is open. A page makes its own controls (WheelKnob, juce::ComboBox, juce::TextButton with
 * the usual attachments to context.processor.getState()) and lays them out in resized() on the grid constants
 * below.
 */
class LayerPage : public juce::Component
{
public:
    struct Context
    {
        SwooshbucklerAudioProcessor& processor;
        const Theme::Fonts& fonts;
        juce::Colour accent;      // the panel's section colour (Bus A, Bus B, or the Ride panel's)
        Layers::Target target;
    };

    explicit LayerPage(const Context& c) : context(c) {}
    ~LayerPage() override = default;

    /** About 30 times a second while open: resting looks ("It rests..." and the hollow dashed look for a control
        that does nothing in the current choice), readouts, meters. */
    virtual void refresh() {}
    /** The wheels on this page a modulator's jack may be dropped on (Varispeed on a Swirl page; none elsewhere). */
    virtual std::vector<ModTarget*> wireTargets() { return {}; }

    const Context context;
};

class DetailLayer : public juce::Component
{
public:
    // The grid (design 2.0), in unscaled window pixels.
    static constexpr int c_frontCell = 64, c_choicesWidth = 100, c_choiceHeight = 22, c_maxChoices = 3, c_cell = 56, c_maxCells = 5;
    static constexpr int c_rideChoicesWidth = 100, c_rideCell = 44, c_rideMaxCells = 4;
    static constexpr float c_foldMs = 180.0f;

    DetailLayer(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts);
    ~DetailLayer() override;

    /** Opens the layer for `target` in this host's panel (another one open here is swapped for it). */
    void open(Layers::Target target, juce::Colour accent);
    /** Back: the front comes back. */
    void close();
    bool isOpen() const { return m_open; }
    Layers::Target getTarget() const { return m_target; }
    /** Reset: every Layers::detailIds parameter of the open layer back to its default, as one undo step. */
    void resetDetail();

    /** The editor hides the panel's front controls when a layer opens (so their cables hide) and shows them on close. */
    std::function<void(Layers::Target)> onOpened;
    std::function<void(Layers::Target)> onClosed;

    void paint(juce::Graphics&) override {}
    void resized() override {}

private:
    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
    bool m_open = false;
    Layers::Target m_target;
    std::unique_ptr<LayerPage> m_page;
};

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
 * What the pages (LayerPages, the pages' package) rely on: LayerPage below and DetailLayer's grid (its constants
 * and the static pageWidth(), choiceBox(), choiceHalf(), cell(), wheelBox() and meterBox()), unchanged. The host
 * gives a page bounds of pageWidth(target) x c_rowHeight (a bus: from the right edge of the front wheel's cell;
 * Ride: from the left of its knob row; both at the top of the knob row) and calls refresh() about 30 times a second
 * while it is open. A page makes its own controls (WheelKnob at Size::Secondary, juce::ComboBox, juce::TextButton
 * with the usual attachments to context.processor.getState()) and places them in resized() with the grid functions,
 * which answer in the page's own coordinates and need no host: a page can be made and laid out by itself (the
 * picture tool and the tests do).
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
    // A panel's knob row is this tall (from its knob cells' top to its scope strip); a Secondary wheel's box (its
    // dial over its two lines of text) is c_wheelWidth x c_wheelHeight, c_wheelY down a cell, which leaves room
    // under it for the offset meter. Boxes and keys in the choices column keep c_choiceInset clear at each side
    // and c_choiceGap between them.
    static constexpr int c_rowHeight = 92, c_wheelWidth = 44, c_wheelHeight = 70, c_wheelY = 2;
    static constexpr int c_choiceInset = 4, c_choiceGap = 4, c_meterWidth = 56, c_meterHeight = 10, c_meterGap = 4;

    // The grid, in a page's own coordinates (see LayerPage). Ride's pages (Together, Swell) have the narrow cells.
    static constexpr bool isRide(Layers::Target t) { return t.kind == Layers::Kind::Together || t.kind == Layers::Kind::Swell; }
    static constexpr int choicesWidth(Layers::Target t) { return isRide(t) ? c_rideChoicesWidth : c_choicesWidth; }
    static constexpr int cellWidth(Layers::Target t) { return isRide(t) ? c_rideCell : c_cell; }
    static constexpr int maxCells(Layers::Target t) { return isRide(t) ? c_rideMaxCells : c_maxCells; }
    /** How wide a page is: its choices column and all of its cells. */
    static constexpr int pageWidth(Layers::Target t) { return choicesWidth(t) + maxCells(t) * cellWidth(t); }
    /** The index-th of `count` boxes or keys stacked in the choices column (count 1 .. c_maxChoices), the stack
        centred in the row's height. */
    static juce::Rectangle<int> choiceBox(Layers::Target t, int index, int count)
    {
        count = juce::jlimit(1, c_maxChoices, count);
        const int stack = count * c_choiceHeight + (count - 1) * c_choiceGap;
        return { c_choiceInset, (c_rowHeight - stack) / 2 + juce::jlimit(0, count - 1, index) * (c_choiceHeight + c_choiceGap),
                 choicesWidth(t) - 2 * c_choiceInset, c_choiceHeight };
    }
    /** Two keys side by side in one place of the stack (Thicken and Spread): its left or its right half. */
    static juce::Rectangle<int> choiceHalf(Layers::Target t, int index, int count, bool right)
    {
        auto box = choiceBox(t, index, count);
        const int half = (box.getWidth() - c_choiceGap) / 2;
        return right ? box.removeFromRight(half) : box.removeFromLeft(half);
    }
    /** The index-th cell after the choices column (0 .. maxCells - 1), the whole height of the row. */
    static juce::Rectangle<int> cell(Layers::Target t, int index)
    {
        return { choicesWidth(t) + juce::jlimit(0, maxCells(t) - 1, index) * cellWidth(t), 0, cellWidth(t), c_rowHeight };
    }
    /** A Secondary wheel's box in that cell (the wheel is whole in it: WheelKnob::cellWidth / cellHeight). */
    static juce::Rectangle<int> wheelBox(Layers::Target t, int index)
    {
        const auto c = cell(t, index);
        return { c.getX() + (c.getWidth() - c_wheelWidth) / 2, c_wheelY, c_wheelWidth, c_wheelHeight };
    }
    /** Under the wheel in that cell: the Varispeed offset meter (OffsetMeter). */
    static juce::Rectangle<int> meterBox(Layers::Target t, int index)
    {
        const auto c = cell(t, index);
        return { c.getX() + (c.getWidth() - c_meterWidth) / 2, c_wheelY + c_wheelHeight + c_meterGap, c_meterWidth, c_meterHeight };
    }

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

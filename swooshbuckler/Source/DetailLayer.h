#pragma once

#include <JuceHeader.h>
#include "Motion.h"
#include "Parameters.h"
#include "Theme.h"

#include <functional>
#include <memory>
#include <vector>

class SwooshbucklerAudioProcessor;
class ModTarget;

/**
 * THE DETAIL LAYER: the host that turns one panel over to show what is behind one of its notched controls.
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
 * The host (one for each panel that has layers: Ride, Bus A, Bus B) covers its panel and takes clicks only on its
 * children: the page, and Reset and Back at the right of the title bar. The window keeps the rest: it hides the
 * panel's front controls while the layer shows (so their cables hide), moves the front wheel into the first cell,
 * writes the title, closes the layer on Esc and on a second double-click of the front wheel, and registers the
 * page's wireTargets() as wire targets while it is open (onPageChanged tells it when to). The notch on the front
 * wheels and its gestures are WheelKnob's (LookAndFeel.h).
 *
 * The fold (Motion::Fade, 180 ms, as the Stowaway plate's): the first half the front fades out, the second half the
 * layer fades in; showsLayer() says which side is up and sideAlpha() how far it has faded in. A page is made when
 * the layer opens and goes once it has folded shut.
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

    // The window's side of the host (pages need none of this).
    /** Where the page goes and where Reset and Back sit, in this host's own coordinates (the panel's). */
    void setPlaces(juce::Rectangle<int> page, juce::Rectangle<int> keys);
    /** One frame of the fold, dt seconds; refreshes the page about 30 times a second while it shows. True when the
        side that shows or its fade changed (the window then lays the panel out again). */
    bool step(float dt);
    /** The fold to its end at once (the pictures and the tests). */
    void finishFold();
    /** The layer's side is up (the fold is past its middle): the panel shows the page, not its front. */
    bool showsLayer() const { return m_fold.value >= 0.5f; }
    /** How far the side that is up has faded in: 1 at rest, 0 at the middle of the fold. */
    float sideAlpha() const { return showsLayer() ? juce::jlimit(0.0f, 1.0f, 2.0f * m_fold.value - 1.0f) : juce::jlimit(0.0f, 1.0f, 1.0f - 2.0f * m_fold.value); }
    bool isFolding() const { return m_fold.moving(); }
    /** The page while one is made (open, or folding shut), or nullptr. */
    LayerPage* getPage() const { return m_page.get(); }
    /** The page's wire targets (none without a page). */
    std::vector<ModTarget*> wireTargets() const { return m_page != nullptr ? m_page->wireTargets() : std::vector<ModTarget*> {}; }
    /** A page was made, swapped or is about to go (it is still alive during the call): the wire targets change. */
    std::function<void()> onPageChanged;
    juce::TextButton& getResetKey() { return m_reset; }
    juce::TextButton& getBackKey() { return m_back; }

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
    void resized() override;

private:
    /** Shows the side that is up at its fade (the page, Reset and Back), and drops the page once folded shut. */
    void applyFold();
    void dropPage();
    void makePage(juce::Colour accent);

    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
    bool m_open = false;
    Layers::Target m_target;
    std::unique_ptr<LayerPage> m_page;
    Motion::Fade m_fold;
    juce::TextButton m_reset { "Reset" }, m_back { "Back" };
    juce::Rectangle<int> m_pagePlace, m_keysPlace;
    int m_refreshTick = 0;
    bool m_shownSide = false;
    float m_shownAlpha = -1.0f;
};

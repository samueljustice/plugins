#pragma once

#include <JuceHeader.h>
#include "BusView.h"
#include "Parameters.h"
#include "Theme.h"

#include <array>
#include <memory>
#include <vector>

class SwooshbucklerAudioProcessor;
class OutputSpectrum;

/**
 * THE PLATE OVER A BUS (design 2.2, as the Output EQ's plate over Bus B): while a bus's Filter or Swirl layer is
 * open, a hard-edged plate stands in the row above that bus's panel and shows what that wheel is doing, live. It is
 * read-only: everything on it is set on the layer under it.
 *
 * Where (PluginEditor::busPlateBounds): the foot of the top row's panel the bus sits under, across that panel's
 * whole width: Tracks for Bus A, the Sea for Bus B. Unlike the EQ plate, which is exactly as wide as Output under
 * it, these line up with the panel they cover rather than with the bus (Bus A sits under Tracks' right half, and a
 * plate as wide as Bus A would cut Tracks and the sea each in two). The panel makes room above it: Tracks shows the
 * lanes that fit whole (scrolling) and leaves its drop line out; the sea's water ends at the plate's top, with the
 * plan of the room and its note above it, and the room strip goes.
 *
 * Filter: one graph, 20 Hz .. 20 kHz on a log axis, +24 .. -48 dB, back to front:
 *   - the bus's own spectrum, faint, as the filter leaves it (busview::Tap, through OutputSpectrum);
 *   - the band of the filter's travel: from where it sits with the whoosh at nothing and its rhythm at its lowest
 *     to where it opens with the whoosh at its peak and its rhythm at its highest, for the front wheel where it is
 *     now (so Closed At, Follow and Sweep each move or widen it);
 *   - the filter's curve, busview::filterGain at the render rate for the type, the slope, the place and the Q the
 *     audio thread published this chunk (so it rides the whoosh, Sweep's rhythm and any wire as the filter does),
 *     with a marker at its place; and what the bus hears (busview::heardGain: the dry and the trimmed filter, mixed
 *     by the wheel's wet mix law) as a thin light line whenever that is not the curve itself (showsHeard()).
 *   With Filter at 0% the curve is drawn unlit and the row says the filter is mixed out. With the rig at rest
 *   (no chunk rendered for a moment) it is drawn from the parameters, where the filter sits closed, and says so.
 *   The row under it: the type and slope, where the filter is, its Q, and the travel.
 *
 * Swirl: by type (busview::SwirlLook, the first lane):
 *   - Tape Flanger: the copy against the dry over the last four seconds (later above the line, earlier under it,
 *     the line where they are level marked), with how near the bus's passes are faint behind it, and the comb the
 *     copy makes with the dry now (busview::swirlTransfer, Feedback and the loop's low-pass included).
 *   - Phaser + Flanger and Flanger: the flanger's delay over the last four seconds, and the response now; the row
 *     says where Phaser + Flanger's lowest phaser notch is (its all-passes turn near the top of the band).
 *   - Phaser: where the all-passes turn over the last four seconds (100 Hz .. 10 kHz), and the response now.
 *   - Doubler: its copy's delay over the last four seconds (0 .. 80 ms) across the whole graph; its comb is not
 *     drawn (its notches are too close together to read), and the row says how far the copy wandered.
 *   With Swirl at 0% the curve is unlit and the row says none of it is heard.
 *
 * How it is drawn (the picture only; none of it changes what is shown):
 *   - the wells are a shade darker outside the plot, as the Output EQ's graph is outside 20 Hz and 20 kHz;
 *   - what is behind (the spectrum, the passes) is a faint grey-blue shape with a line along its top, so the bus's
 *     colour is kept for what the wheel is doing;
 *   - the filter's curve is tinted under it, deepest at the head of the well and gone by its foot; its travel is a
 *     faint band between two lines with a rail along the well's head, and the marker is a line down the well, a tab
 *     on the rail and a stamped square on the curve (as the EQ's handles);
 *   - a trace is one line through its points, fainter the older it is, with a stamped square on the newest point; a
 *     point that wobbled further than the line shows has its whole range drawn behind the line;
 *   - the Swirl response is tinted to the 0 dB line. Where a comb's peaks come closer than c_densePx (they crowd at
 *     the top of a log axis) each tooth is no longer drawn: the band the teeth cover is, between a line along
 *     their peaks and one along their notches, both lighter than the curve (which ends where the band starts);
 *   - lines with sharp turns have beveled corners (a mitered one reaches past the point and deepens a notch).
 *   What never moves (plate, wells, grids) is kept as an image while the context's scale is a whole number, and
 *   drawn afresh at any other scale.
 *
 * The graphs' labels are drawn last, each on a patch of the well's colour, so no curve runs through one.
 *
 * Read-only: it takes the clicks over its area (so nothing under it is reached through it) and no keys, so Esc stays
 * with the window and closes the layer, and the plate with it.
 *
 * What the callers rely on (PluginEditor): one plate per bus, placed by busPlateBounds, c_height tall and at least
 * 400 wide; setKind() as the layer opens; shown while the bus's Filter or Swirl layer shows (fading in and out with
 * it), not under the EQ plate, and Bus B's not while the morph pad is shown (the pad docks where it stands); refresh()
 * about 30 times a second while shown. refresh() works everything out and repaints only when something on the plate
 * changed (so a plate over a rig at rest costs nothing); paint() is fills and strokes of what it worked out. It reads
 * the engine only through RigEngine::getBusView (relaxed atomics), and tells the text checker
 * (Theme::TextProbe::covered) that the texts under it are not seen.
 */
class BusViewPlate : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int c_height = 156;
    static constexpr int c_graphX = 12, c_graphY = 9, c_graphHeight = 110, c_rowY = 125, c_rowHeight = 22;
    static constexpr float c_minHz = 20.0f, c_maxHz = 20000.0f;
    static constexpr float c_filterTopDb = 24.0f, c_filterBottomDb = -48.0f;
    static constexpr float c_swirlTopDb = 12.0f, c_swirlBottomDb = -36.0f;
    /** Frames without a new chunk before the plate says the rig is at rest (about a fifth of a second). */
    static constexpr int c_restFrames = 6;

    enum class Kind { Filter, Swirl };

    BusViewPlate(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts, int bus);
    ~BusViewPlate() override;

    void setKind(Kind kind);
    Kind getKind() const { return m_kind; }
    int getBus() const { return m_bus; }
    /** Message thread, about 30 times a second while shown. */
    void refresh();

    /** The graph's well, and the Swirl's two panes in it (the trace, the response), in this component's coordinates. */
    juce::Rectangle<float> graphArea() const;
    juce::Rectangle<float> tracePane() const;
    juce::Rectangle<float> responsePane() const;
    juce::Rectangle<int> rowArea() const { return { c_graphX, c_rowY, getWidth() - 2 * c_graphX, c_rowHeight }; }

    /** What the filter graph shows now (the tests compare it with the filter). */
    const busview::FilterLook& getFilterLook() const { return m_filter; }
    const busview::SwirlLook& getSwirlLook() const { return m_swirl; }
    /** The rig has rendered no chunk for a moment: the filter is drawn from its parameters. */
    bool isResting() const { return m_resting; }
    /** The filter curve's dB at `hz` as drawn (the same sum the curve's points are). */
    double drawnFilterDb(double hz) const;
    /** The light line for what the bus hears is drawn (it differs from the filter's curve: mixed in part, or trimmed). */
    bool showsHeard() const;
    double renderRate() const;

    /** Where a frequency is across a plot, and back. */
    static float xForHz(float hz, juce::Rectangle<float> plot);
    static float hzForX(float x, juce::Rectangle<float> plot);

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void updateTooltip();
    busview::FilterLook filterFromParameters() const;
    void refreshFilter();
    void refreshSwirl();
    void buildTrace();
    void buildResponse();
    void paintFilter(juce::Graphics&);
    void paintSwirl(juce::Graphics&);
    void paintBase(juce::Graphics&) const;
    int baseLayout() const;
    void paintFrequencyGrid(juce::Graphics&, juce::Rectangle<float> well, juce::Rectangle<float> plot, float topDb, float bottomDb,
                            std::initializer_list<float> marked) const;
    void paintFrequencyLabels(juce::Graphics&, juce::Rectangle<float> well, juce::Rectangle<float> plot);
    void paintTrace(juce::Graphics&, juce::Rectangle<float> well);
    juce::Rectangle<float> plotOf(juce::Rectangle<float> well) const;
    juce::String rowText() const;

    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
    const int m_bus;
    const juce::Colour m_accent;
    Kind m_kind = Kind::Filter;

    busview::FilterLook m_filter;
    busview::SwirlLook m_swirl;
    bool m_resting = false, m_lit = true;
    std::uint32_t m_lastTick = 0;
    int m_stillFrames = c_restFrames;

    std::unique_ptr<OutputSpectrum> m_spectrum;
    std::array<float, busview::Tap::c_size> m_samples {};
    std::vector<float> m_levels;            // the spectrum, one value every c_spectrumStep px
    bool m_spectrumQuiet = true;
    static constexpr int c_spectrumStep = 3;

    juce::Path m_curve, m_curveFill, m_heard; // worked out in refresh()
    juce::Path m_spectrumShape, m_spectrumLine; // the bus's sound behind the filter: its shape and the line along its top
    static constexpr float c_railHeight = 3.0f; // the travel's rail along the head of the filter's well

    // The Swirl response where a comb's teeth are closer than c_densePx: the band they cover, and the lines along
    // their peaks and along their notches (m_curve ends where the band starts). The rest is buildResponse()'s
    // working room.
    static constexpr float c_densePx = 5.0f;
    juce::Path m_combBand, m_combHead, m_combFoot;
    std::vector<float> m_db;
    std::vector<int> m_peaks, m_notches;
    std::vector<juce::Point<float>> m_lows;
    float m_markerX = 0.0f, m_markerY = 0.0f, m_lowX = 0.0f, m_highX = 0.0f;

    // The Swirl's trace, newest last: lowest and highest of each point, whether it was written in the type in use,
    // and the passes' nearness.
    std::array<float, busview::Trace::c_points> m_traceLo {}, m_traceHi {}, m_near {};
    std::array<bool, busview::Trace::c_points> m_traceHas {};
    float m_traceMin = 0.0f, m_traceMax = 0.0f;
    bool m_traceAny = false;
    // Their pictures (buildTrace): the passes' shape and its top line, the trace's line, the ranges the line does not
    // cover, and where the newest point is.
    juce::Path m_nearShape, m_nearLine, m_traceLine;
    juce::RectangleList<float> m_traceRange;
    juce::Point<float> m_now;
    bool m_nearAny = false, m_nowHas = false;
    std::uint32_t m_traceWritten = 0; // the traces' points written when last refreshed (a new one: repaint)
    bool m_dirty = true;              // repaint at the next refresh whatever it finds (a new kind or size)
    int m_repaintsAsked = 0;          // how many times refresh() has asked for a paint (the tests)
    juce::String m_row;

    // What never moves (the plate, its wells and their grids), kept as an image for the layout it was drawn for
    // (baseLayout(): the filter's one well, the Doubler's one, or the Swirl's two).
    juce::Image m_base;
    int m_baseLayout = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BusViewPlate)
};

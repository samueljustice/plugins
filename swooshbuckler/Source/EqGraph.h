#pragma once

#include <JuceHeader.h>
#include "OutputEq.h"
#include "Theme.h"

#include <array>
#include <memory>
#include <vector>

class SwooshbucklerAudioProcessor;
class OutputSpectrum;

/**
 * THE OUTPUT EQ'S GRAPH (design 2.2): 576 x 110, 20 Hz .. 20 kHz on a log axis, +-12 dB, a well cut into the
 * Output EQ plate.
 *
 * What it draws, back to front: the grid (decades and the 0 dB line marked, +-6 dB faint), the output's spectrum
 * faint behind, the selected band's own curve as a tint, the whole curve as OutputEq::responseDb() gives it at the
 * processor's render rate (so what is drawn is what the filters do there, cramping near the top at 44.1 and 48 kHz
 * included), and six square handles in the Output colour, one per OutputEq::Band. With the EQ switched off the
 * curve it would make is still drawn, unlit, and the handles are hollow.
 *
 * The handles:
 *   drag           frequency and gain (within a third of a dB of the line the gain sits on 0 dB exactly). A cut
 *                  slides sideways only; parked, it sits in the margin outside its end of the axis, hollow and
 *                  dashed, and pulling it in over the axis turns it on. Pushing it back out parks it.
 *   Cmd-drag       up and down sets a bell's Q (Ctrl on Windows); so does the mouse wheel over a bell (or with a
 *                  bell selected).
 *   double-click   flattens the band (its gain to 0 dB) or parks the cut.
 *   right-click    a box to type values into: "320 Hz -3 dB Q 1.4", any part of it, units optional ("2.5k",
 *                  "off" for a cut). Return keeps it, Esc or a click elsewhere leaves the band as it was.
 * A drag, a double-click and a typed line are each one undo step (EditHistory::Group around the parameters' own
 * change gestures); wheel moves in a row join into one as loose moves do everywhere.
 *
 * What it reads: the settings from the ParamIDs::eq* parameters (and getOutputEqParams() for the curve), the rate
 * from getRenderRate(), and the spectrum through OutputSpectrum (SeaVisualiser.h): the output's mono sum after the
 * EQ and the output stage, at the device's rate.
 *
 * What the callers rely on: the plate (EqPlate) owns one, calls refresh() about 30 times a second while it shows
 * and reads getSelectedBand() and readout() for its readout row; the EQ key in Output's title bar (PluginEditor)
 * draws its miniature with paintMiniature().
 */
class EqGraph : public juce::Component, public juce::TooltipClient
{
public:
    static constexpr int c_width = 576, c_height = 110;
    static constexpr float c_minHz = 20.0f, c_maxHz = 20000.0f, c_rangeDb = 12.0f;
    /** The axis is this far inside the well: the margin at each side is where a parked cut sits. */
    static constexpr float c_marginX = 18.0f, c_marginY = 9.0f;
    /** A handle's side, how near a click must be to take it, and where a parked cut sits from the well's edge. */
    static constexpr float c_handle = 10.0f, c_reach = 10.0f, c_parkedInset = 8.0f;
    /** A drag this close to 0 dB is 0 dB. */
    static constexpr float c_detentDb = 0.3f;

    EqGraph(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts);
    ~EqGraph() override;

    /** The band whose handle was touched last (OutputEq::Band), or -1. */
    int getSelectedBand() const { return m_selected; }
    void selectBand(int band);
    /** Called when the selected band or its values change (the plate's readout follows). */
    std::function<void()> onSelectionChanged;
    /** Message thread, about 30 times a second while shown: the spectrum and the curve. */
    void refresh();

    /** The EQ key's picture: the curve of `p` in `area` (a flat line when isFlat(p)), drawn unlit when `lit` is false.
        `colour` is what the lit curve is drawn in (on a key that is lit by its whole plate, the lettering's colour). */
    static void paintMiniature(juce::Graphics& g, juce::Rectangle<float> area, const OutputEq::Params& p, double sampleRate,
                               juce::Colour colour, bool lit);

    // A band's name ("Low Mid") and what it is set to. A band has a frequency; all but the cuts have a gain; the
    // two bells have a Q.
    static const char* bandName(int band);
    static bool isCut(int band) { return band == OutputEq::LowCut || band == OutputEq::HighCut; }
    static bool isBell(int band) { return band == OutputEq::LowMid || band == OutputEq::HighMid; }
    /** A cut that is Off. */
    bool isParked(int band) const;
    float frequencyOf(int band) const; // Hz (a parked cut: its parked end stop)
    float gainOf(int band) const;      // dB (0 for a cut)
    float qOf(int band) const;         // (0.7071 for anything but a bell)
    /** What the readout row says of a band: its frequency ("Off" for a parked cut), its gain (a cut: its slope),
        and a bell's Q; empty strings for what it has not got. */
    struct Readout { juce::String name, frequency, gain, q; };
    Readout readout(int band) const;

    // Where things are, in this component's coordinates.
    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced(c_marginX, c_marginY); }
    float xForHz(float hz) const;
    float hzForX(float x) const;
    float yForDb(float db) const;
    float dbForY(float y) const;
    /** The centre of a band's handle now. */
    juce::Point<float> handleCentre(int band) const;
    /** The band whose handle is at a point (the nearest within reach; the selected one wins a tie), or -1. */
    int bandAt(juce::Point<float> at) const;

    /** Double-click: the band's gain to 0 dB, or the cut parked. One undo step. */
    void flattenBand(int band);
    /** Every gain to 0 dB and both cuts parked (the plate's Flat key). One undo step. */
    void flattenAll();
    /** A bell's Q, as the wheel sets it (clamped to its range). */
    void setQ(int band, float q);
    /** Right-click: opens the box over the band's handle. */
    void beginTyping(int band);
    bool isTyping() const;
    /** What the box does with its text: sets what the text names. False when nothing in it could be read. One undo step. */
    bool typeValues(int band, const juce::String& text);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    juce::String getTooltip() override;

private:
    struct Drag;
    static constexpr int c_curveStep = 2;    // the curve is worked out every this many pixels
    static constexpr int c_spectrumStep = 4; // and the spectrum every this many

    juce::RangedAudioParameter* frequencyParam(int band) const { return m_freq[static_cast<size_t>(band)]; }
    juce::RangedAudioParameter* gainParam(int band) const { return m_gain[static_cast<size_t>(band)]; }
    juce::RangedAudioParameter* qParam(int band) const { return m_q[static_cast<size_t>(band)]; }
    static float plain(const juce::RangedAudioParameter* p);
    static void setPlain(juce::RangedAudioParameter* p, float value);
    void setFrequency(int band, float hz, bool parked);
    void endDrag();
    void endTyping(bool keep, bool takeKeysBack);
    void placeTyping();
    void rebuildCurve();
    void setHover(int band);
    void changed();
    void paintHandle(juce::Graphics&, int band, bool eqOn) const;

    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
    std::array<juce::RangedAudioParameter*, OutputEq::c_bands> m_freq {}, m_gain {}, m_q {};
    int m_selected = -1, m_hover = -1;

    std::unique_ptr<Drag> m_drag;
    std::unique_ptr<juce::TextEditor> m_typing;
    int m_typingBand = -1;

    // The curve as it was last worked out, and what it was worked out from.
    std::vector<float> m_curve, m_bandCurve; // dB at every c_curveStep pixels across the plot
    OutputEq::Params m_curveParams;
    double m_curveRate = 0.0;
    int m_curveBand = -2, m_curveWidth = 0;
    juce::Path m_path;                       // kept, so painting builds no new one

    std::unique_ptr<OutputSpectrum> m_spectrum;
    std::vector<float> m_levels;             // 0 .. 1 at every c_spectrumStep pixels, smoothed
    bool m_spectrumQuiet = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqGraph)
};

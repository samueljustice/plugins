#pragma once

#include <JuceHeader.h>

#include "Engine.h"
#include "Theme.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

/**
 * The SCOPE view, and its curve editor.
 *
 * The learned sound over time on a note axis: all three readings of its pitch (the one in use
 * cyan, the others dim, each with its name tag), the pitch it is flattened to (orange), a level
 * strip so timing can be read, a lime play head while the flattened sound plays, and under it all
 * an overview of the whole sound with the part on view marked.
 *
 * Editing: the line in use can be corrected by hand. Pulling it up at a moment says "the pitch here
 * is higher than was read", so the flattener pulls that moment down further. Pull mode adds or
 * moves a point where the line is grabbed (Shift for fine moves, a double-click removes a point);
 * draw mode redraws a stretch freehand. Snap puts the corrected pitch on the nearest semitone. The
 * corrected line is drawn bright, the reading as it was faint beneath it, and each point as a
 * diamond. A click on another reading's line or tag asks to flatten by it instead (onLineClicked):
 * near the line in use, the line in use wins.
 *
 * Zoom: the wheel (or a trackpad's scroll) zooms time about the pointer, with Alt the pitch; a
 * sideways scroll or a drag on the time strip or the overview pans; a pinch zooms time. fit()
 * shows the whole sound again. All drawing and editing go through one mapping, so a point lands on
 * the moment and pitch under the mouse however far in the view is zoomed.
 */
class PitchDisplay final : public juce::Component, public juce::TooltipClient
{
    enum class Gesture { None, Pull, Draw, Pan, Overview };

public:
    using CurvePoints = std::vector<pitchflattener::core::CurvePoint>;

    PitchDisplay();

    struct View
    {
        std::shared_ptr<const pitchflattener::Result> result;
        pitchflattener::Engine::State state = pitchflattener::Engine::State::Idle;
        double captureSeconds = 0.0;
        std::int64_t playPosition = -1;   // in the flattened sound
        std::uint64_t playGeneration = 0;
        bool rendering = false;
        bool autoSource = true;           // SOURCE is Auto: the pick is named PICKED
        int lineInUse = -1;               // the reading the corrections apply to
        CurvePoints edits;                // its corrections

        bool operator== (const View&) const noexcept;
    };

    void setView (const View&);

    // ---- tools ----
    void setDrawMode (bool draw);
    bool drawMode() const noexcept { return m_draw; }
    void setSnap (bool semitones);
    bool snap() const noexcept { return m_snap; }
    bool isEditing() const noexcept { return m_gesture == Gesture::Pull || m_gesture == Gesture::Draw; }
    /** The mouse is over the graph and the line can be edited there. */
    bool isHoveringEditable() const noexcept { return m_hoverEditable; }

    // ---- zoom ----
    static constexpr double kMinSpanSeconds = 0.05;
    static constexpr double kMinSemitones = 2.0;
    double viewFrom() const noexcept;
    double viewTo() const noexcept;
    double lowMidi() const noexcept { return m_low; }
    double highMidi() const noexcept { return m_high; }
    double durationSeconds() const noexcept { return m_seconds; }
    bool isZoomed() const noexcept;
    /** Zooms time by `factor` (below 1 zooms in) keeping `anchorSeconds` where it is on screen. */
    void zoomTime (double anchorSeconds, double factor);
    /** Zooms pitch by `factor` keeping `anchorMidi` where it is. */
    void zoomPitch (double anchorMidi, double factor);
    void panTime (double deltaSeconds);
    void fit();

    // The mapping (this component's coordinates).
    float xFor (double seconds) const noexcept;
    double secondsFor (float x) const noexcept;   // clamped to the view
    float yFor (double midi) const noexcept;
    double midiFor (float y) const noexcept;
    juce::Rectangle<float> plotArea() const noexcept { return m_plot; }
    juce::Rectangle<float> overviewArea() const noexcept { return m_overview; }
    juce::Rectangle<float> timeStripArea() const noexcept { return m_timeStrip; }
    /** Where each reading's name tag is (empty for a reading with no line). */
    std::array<juce::Rectangle<float>, pitchflattener::core::kNumTrackers> tagAreas() const;
    /** The separate sounds in the recording, each with the pitch it is flattened to (one span for
        a single sound: the whole of it). Empty when nothing is flattened. */
    struct TargetSpan
    {
        double start = 0.0, end = 0.0, midi = 0.0;
    };
    std::vector<TargetSpan> targetSpans() const;

    /** The line in use as corrected, at a moment (MIDI); NaN with no line. */
    double correctedAt (double seconds) const;

    // ---- what the user did ----
    std::function<void (int)> onLineClicked;                                   // a reading was chosen
    std::function<void (const CurvePoints&)> onEditsChanging;                  // during a gesture
    std::function<void (const CurvePoints& before, const CurvePoints& after)> onEdited;   // a gesture finished
    std::function<void (bool redo)> onUndoKey;
    std::function<void()> onViewChanged;                                      // zoomed or panned, or hover changed

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;
    bool keyPressed (const juce::KeyPress&) override;
    juce::String getTooltip() override;

private:
    void layout();
    void rebuild();
    void autoRange (double& low, double& high) const;
    void clampView();
    void changed();
    const pitchflattener::core::PitchLine* line (int index) const;
    const CurvePoints& points() const noexcept { return isEditing() ? m_points : m_view.edits; }
    double baseAt (double seconds) const;                       // the line in use, uncorrected
    double lineAt (int index, double seconds) const;
    int pointAt (juce::Point<float>) const;
    int lineNear (juce::Point<float>, float tolerance) const;    // the nearest reading's line within tolerance (in use first)
    int tagAt (juce::Point<float>) const;
    double snapped (double seconds, double value) const;
    void addStrokeSample (juce::Point<float>);
    void drawReading (juce::Graphics&, int index, bool inUse) const;
    void drawGrid (juce::Graphics&) const;
    void drawOverview (juce::Graphics&) const;
    std::pair<std::size_t, std::size_t> framesInView (const pitchflattener::core::PitchLine&) const;

    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    View m_view;
    std::uint64_t m_learnedId = 0;

    // Layout.
    juce::Rectangle<float> m_plot, m_strip, m_timeStrip, m_overview;

    // The sound and the view on it.
    double m_seconds = 1.0;
    double m_from = 0.0, m_to = 0.0;           // to <= from: the whole sound
    double m_autoLow = 48.0, m_autoHigh = 72.0;
    double m_low = 48.0, m_high = 72.0;
    std::vector<float> m_peaks;                // the loudest sample in each block of kPeakBlock
    static constexpr int kPeakBlock = 16;

    // Tools and gestures.
    bool m_draw = false, m_snap = false;
    Gesture m_gesture = Gesture::None;
    CurvePoints m_points, m_before, m_stroke;
    int m_drag = -1;
    bool m_created = false, m_moved = false;
    pitchflattener::core::CurvePoint m_dragStart;
    juce::Point<float> m_downAt;
    double m_panFrom = 0.0, m_strokeFrom = 0.0;
    double m_readoutSeconds = -1.0;

    // Hover.
    int m_hoverLine = -1, m_hoverPoint = -1, m_hoverTag = -1;
    juce::Point<float> m_hoverAt { -1.0f, -1.0f };
    bool m_hoverEditable = false;
};

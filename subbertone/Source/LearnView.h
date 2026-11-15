#pragma once

#include <JuceHeader.h>
#include "LearnWorker.h"
#include "Theme.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

/**
 * LEARN's view: what was learned, in place of the scope while it is open (the editor hides the scope, whose OpenGL
 * view would cover anything drawn over it). Opened and closed by double-clicking LEARN (or Shift-Return on it, or a
 * screen reader's "show menu"); closed with x or Esc too.
 *
 * Header: the region on show (its menu lists every learned region and can forget this one); METHOD, Auto, Engine or
 * Tonal (analyses the region again at once if its audio is still kept, otherwise arms LEARN with that method and says
 * so); GAP FILL, the longest unpitched gap the curve bridges (0-1000 ms, applied at once); USE LEARNED; CLEAR; UNDO
 * (for 10 s after forgetting) and close.
 *
 * The pitch lane, on a log-frequency axis with octave lines on C: the line the sub follows (teal) and the sub it makes
 * an octave below (amber), dotted where gap fill bridged them and fainter where the analysis was less sure; the other
 * lines the analysis found, faint and only where they are present, each with a tag at the right edge (click a line or
 * its tag to follow it); the stretches that no longer matched what was learned, in the pass under way and, ahead of
 * the playhead, in the pass before (red); what the live tracker hears during playback (dots) and the playhead. The
 * wheel zooms the frequency axis; double-click fits it again. The status line names the analysis, then any warning
 * (part no longer matches, the pitch was ambiguous), then its figures: cut to fit its row, it loses figures first.
 *
 * Message thread only. The editor calls update() 20 times a second while the view shows. It repaints only what changed:
 * nothing while nothing does, and during playback the playhead and the newest dots, not the whole lane (on macOS every
 * area repainted in one frame is drawn as the one rectangle around them all, so a wide repaint here would redraw much
 * of the editor 20 times a second).
 *
 * From the keyboard: Tab moves through its buttons, Esc closes it. A screen reader reads which METHOD is lit.
 */
class LearnView : public juce::Component
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LearnView)

public:
    LearnView(subbertone::learn::Learner& learner, const Theme::Fonts& fonts);
    ~LearnView() override;

    /** LEARN's status, what is learned and the live tracker's trail, now. */
    void update(const subbertone::learn::Status& status, const std::shared_ptr<const subbertone::learn::Snapshot>& snapshot,
                const std::vector<subbertone::learn::TrailPoint>& trail);

    /** x or Esc. */
    std::function<void()> onClose;

    /** The region on show (0 = none), and showing another. */
    std::uint32_t getSelectedRegion() const noexcept { return m_selected; }
    void selectRegion(std::uint32_t id);

    /** Follows another line of the region on show: a candidate's ratio to the analysis' own line. */
    void followLine(float ratio);

    /** How much of the region on show is marked as no longer matching the audio, in seconds (for tests). */
    double getMismatchSeconds() const;

    /** The status line as it reads now (for tests). */
    juce::String getStatusText() const;

    //==============================================================================
    // How Learn's data is put into words (shared with LearnButton).

    /** A line's ratio to the analysis' own line: "x1", "x1/2", "x3/2", or "x1.37" when no small fraction is close. */
    static juce::String describeRatio(float ratio);

    /** "Auto", "Engine", "Tonal". */
    static juce::String describeMethod(subbertone::learn::Method method);

    /** A timeline position: "0:07.8", "12:03.5", "1:02:03.5", "-0:04.0" (before 0). */
    static juce::String formatTime(double seconds);

    //==============================================================================
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    class PitchLane;
    class LiveLayer;
    class MenuButton;
    class CloseButton;

    struct Run
    {
        juce::String text;
        juce::Colour colour;

        bool operator==(const Run&) const = default;
    };

    void applyState(const subbertone::learn::Status& status, const std::shared_ptr<const subbertone::learn::Snapshot>& snapshot);
    void applyTrail(const std::vector<subbertone::learn::TrailPoint>& trail);
    void refreshStatusRow();
    const subbertone::learn::Region* selectedRegion() const;
    void chooseRegion(const subbertone::learn::Snapshot& snapshot, const subbertone::learn::Status& status);
    void refreshControls();
    void showRegionMenu();
    void chooseMethod(subbertone::learn::Method method);
    void setGapFillFromSlider(bool dragEnded);
    std::vector<Run> statusRuns() const;
    void paintStatusLine(juce::Graphics& g, juce::Rectangle<float> area) const;
    float paintLegend(juce::Graphics& g, juce::Rectangle<float> area) const;

    subbertone::learn::Learner& m_learner;
    const Theme::Fonts& m_fonts;

    std::shared_ptr<const subbertone::learn::Snapshot> m_snapshot;
    std::uint64_t m_revision = 0;
    subbertone::learn::Status m_status;
    std::uint32_t m_selected = 0;
    double m_selectedStart = 0.0, m_selectedEnd = 0.0;   // its span, to find it again after it is analysed anew
    std::uint32_t m_lastRegionHere = 0;

    // A METHOD click that could not analyse at once armed LEARN: the status line says so while that lasts.
    bool m_methodArmed = false;
    subbertone::learn::Method m_armedMethod = subbertone::learn::Method::Auto;

    static constexpr double c_gapFillDragIntervalMs = 120.0;   // GAP FILL applies this often while dragged
    double m_lastGapFillMs = 0.0;

    juce::Rectangle<int> m_headerRow, m_laneArea, m_statusRow;
    std::vector<Run> m_shownRuns;   // the status line as the status row shows it: repainted only when it changes

    std::unique_ptr<MenuButton> m_regionButton;
    juce::Label m_methodLabel, m_gapFillLabel, m_gapFillValue;
    std::array<juce::TextButton, 3> m_methodButtons;
    juce::Slider m_gapFillSlider;
    juce::TextButton m_followButton, m_clearButton, m_undoButton;
    std::unique_ptr<CloseButton> m_closeButton;
    std::unique_ptr<PitchLane> m_lane;
    std::unique_ptr<LiveLayer> m_live;
};

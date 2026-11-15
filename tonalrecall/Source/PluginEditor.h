#pragma once

#include <JuceHeader.h>

#include "Audition.h"
#include "CaptureStore.h"
#include "MatchHub.h"
#include "PluginProcessor.h"

#include <array>
#include <functional>
#include <memory>
#include <utility>

namespace sammyj
{
class UpdateCheck;
struct UpdateInfo;
}

namespace tonalrecall::ui
{
class MusicPanel;
class SfxPanel;
class WarningStrip;
class ShiftPanel;
class AboutOverlay;
class UpdateOverlay;
class MonitorOverlay;
class InsertNotice;
class ConsoleButton;
class ConsoleLook;
}

// The console: learn the music and the effect, see what was found, choose the note and how the
// shift behaves, and switch Stamp on or off. Everything it shows comes from the process-wide
// capture store and match hub, so every instance's editor agrees.
class TonalRecallEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TonalRecallEditor(TonalRecallProcessor&);
    ~TonalRecallEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;

    // The new-version popup (tests show it directly; in a host the update check does).
    void showUpdate(const sammyj::UpdateInfo& info);
    // The "AudioSuite only" notice (in Pro Tools it appears by itself when loaded as an insert).
    void showInsertNotice(bool shouldShow);
    void toggleLearnForTesting(tonalrecall::Slot slot) { toggleLearn(slot); }
    // What a click on PLAY MUSIC, PLAY INPUT or PLAY OUTPUT does.
    void togglePlayForTesting(tonalrecall::Playback what) { togglePlay(what); }

    static constexpr int kWidth = 980;
    static constexpr int kHeight = 706;

    // Tests: read the hub, the store and the processor now (the timer does this at 20 Hz), and
    // what the warning strip says.
    void refreshNow() { refresh(true); }
    juce::StringArray currentWarnings() const { return m_warnings; }
    void showAbout(bool shouldShow);
    // Texts drawn wider or taller than their box since the process started (tests require none).
    static int textOverflowCount() noexcept;
    // Tests draw still frames: with motion off every transition reads as finished.
    static void setMotionEnabled(bool shouldMove) noexcept;

    // Tests: the effect's scan in time. The view is in seconds of the effect; x and the plot are
    // in the scan's own coordinates, which its curve editor and its playhead share.
    std::pair<double, double> effectViewForTesting() const;
    void setEffectViewForTesting(double start, double end);
    void pressEffectViewButtonForTesting(const juce::String& name);   // "-", "+", "FIT" or "SOUND"
    float effectXForSecondsForTesting(double seconds) const;
    double effectSecondsForXForTesting(float x) const;
    float effectPlayheadXForSecondsForTesting(double seconds) const;
    double effectPlayheadSecondsForTesting() const;                    // negative: no playhead
    juce::Rectangle<float> effectPlotForTesting() const;
    std::pair<double, double> effectSoundRangeForTesting() const;      // what SOUND frames
    juce::Component& effectScanForTesting();                           // the curve editor over the scan
    juce::Component& effectOverviewForTesting();
    double effectRedrawMsForTesting() const;                           // the last redraw for a new view
    // The editing tools (DRAW, SNAP: 0 off, 1 notes, 2 scale; HEAR).
    static void setEditToolsForTesting(bool draw, int snap, bool hear) noexcept;

private:
    void timerCallback() override;
    void refresh(bool force);
    void startUpdateCheckWhenShown();
    void toggleLearn(tonalrecall::Slot slot);
    void clearSlot(tonalrecall::Slot slot);
    void togglePlay(tonalrecall::Playback what);
    void updatePlayback();   // the play buttons and playheads, from the audition bus
    int playbackSignature() const;
    void changeOptions(const std::function<void(tonalrecall::MatchOptions&)>& change);
    void copyValues();
    juce::StringArray buildWarnings() const;

    TonalRecallProcessor& m_processor;
    std::unique_ptr<tonalrecall::ui::ConsoleLook> m_look;

    std::unique_ptr<tonalrecall::ui::MusicPanel> m_music;
    std::unique_ptr<tonalrecall::ui::SfxPanel> m_sfx;
    std::unique_ptr<tonalrecall::ui::WarningStrip> m_strip;
    std::unique_ptr<tonalrecall::ui::ShiftPanel> m_shift;
    std::unique_ptr<tonalrecall::ui::AboutOverlay> m_about;
    std::unique_ptr<tonalrecall::ui::UpdateOverlay> m_update;     // the new-version popup, over the editor
    std::unique_ptr<tonalrecall::ui::InsertNotice> m_insertNotice;   // "AudioSuite only", over everything but the glass
    std::unique_ptr<tonalrecall::ui::MonitorOverlay> m_monitor;   // the glass, in front of everything
    std::unique_ptr<sammyj::UpdateCheck> m_updateCheck;           // after the popup: stopped before anything its callback uses goes
    std::unique_ptr<tonalrecall::ui::ConsoleButton> m_help, m_log, m_link;
    std::unique_ptr<juce::TooltipWindow> m_tooltips;

    // What was last shown, to repaint only what changed.
    std::uint64_t m_hubGeneration = 0;
    std::array<std::uint32_t, tonalrecall::kNumSlots> m_storeGenerations {};
    std::uint32_t m_runIndex = 0xffffffffu;
    bool m_runEnded = false;
    tonalrecall::HubSnapshot m_hub;
    std::array<tonalrecall::SlotSnapshot, tonalrecall::kNumSlots> m_slots;
    std::array<bool, tonalrecall::kNumSlots> m_nothingCaptured {};   // the user disarmed before any audio arrived
    StampRunInfo m_run;
    juce::StringArray m_warnings;
    double m_openedAt = 0.0;   // while the opening animation runs
    bool m_auditionHintShown = false;
    int m_playbackShown = -1;   // playbackSignature() when the strip was last built

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TonalRecallEditor)
};

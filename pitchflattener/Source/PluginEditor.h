#pragma once

#include <JuceHeader.h>

#include "Banner.h"
#include "FlightView.h"
#include "Framebuffer.h"
#include "Hud.h"
#include "HudControls.h"
#include "PitchDisplay.h"
#include "PitchFlattenerLookAndFeel.h"
#include "PluginProcessor.h"
#include "SammyJUpdateCheck.h"
#include "Theme.h"

#include <array>
#include <memory>

class AboutWindow;
class UpdatePopup;

/**
 * Pitch Flattener's editor, top to bottom: the header; LEARN, CLEAR and the read-outs; the display
 * (SCOPE, the exact graph, or FLIGHT, the sound flown as a course) with the banners over it; a
 * guidance bar that says what to do next or what is happening; and the controls (target, note,
 * amount, line).
 *
 * Everything in the window is drawn through one console framebuffer (Framebuffer.h): chunky 2 x 2
 * blocks for shapes, 1x pixels for small text, all dithered to 5 bits, shown with no smoothing.
 *
 * A fixed size. Everything it opens (tooltips, the About window, the update popup) is drawn with
 * its PitchFlattenerLookAndFeel and is closed when it goes, so nothing outlives it. Every pulse,
 * blink and flight frame runs from one clock and one 30 Hz timer, which idles while the editor is
 * not showing.
 */
class PitchFlattenerAudioProcessorEditor final : public juce::AudioProcessorEditor, public juce::TooltipClient, private juce::Timer
{
public:
    explicit PitchFlattenerAudioProcessorEditor (PitchFlattenerAudioProcessor&);
    ~PitchFlattenerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    /** Over the FLAT ON readout of a recording of several sounds: what SEVERAL means. */
    juce::String getTooltip() override;

    /** Shows the "new version available" popup over everything in the editor (the update check calls it). */
    void showUpdatePopup (const sammyj::UpdateInfo&);

    /** Reads the engine now (the timer does this 30 times a second; tests call it before a snapshot). */
    void refreshNow();
    const juce::String& statusText() const noexcept { return m_status; }
    const juce::String& learnButtonText() const noexcept { return m_learnText; }
    const juce::String& flatOnText() const noexcept { return m_flatText; }

    /** The view the display shows now (PitchFlattenerAudioProcessor::kViewScope or kViewFlight). */
    int shownView() const;
    /** As a click on the SCOPE or FLIGHT tab. */
    void chooseView (int view);

    BannerLogic::Kind bannerKind() const noexcept { return m_bannerLogic.kind(); }
    bool flatTagShown() const noexcept { return m_bannerLogic.flatTag(); }
    bool learnButtonPulses() const noexcept { return m_learn.isPulsing(); }

    /** Opens the About window (or brings it forward); its content, for the tests' pictures. */
    void showAbout();
    juce::Component* aboutContent() const;

    // The curve editor (undo, redo and reset as their buttons do).
    void undoEdit();
    void redoEdit();
    void resetCurve();
    bool canUndo() const noexcept { return ! m_undoSteps.empty(); }
    bool canRedo() const noexcept { return ! m_redoSteps.empty(); }
    PitchDisplay& scopeForTests() noexcept { return m_display; }
    /** The controls row's panels, by name (for the manual's pictures). */
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> panelsForTests() const
    {
        return { { "TARGET", m_targetCell }, { "NOTE", m_noteCell }, { "AMOUNT", m_amountCell }, { "SOURCE", m_sourceCell },
                 { "LENGTH", m_lengthCell }, { "CHARACTER", m_characterCell }, { "DISPLAY", m_displayPanel }, { "GUIDANCE", m_guide } };
    }
    /** How often corrections are sent to the engine while a line is being dragged (the render follows). */
    static constexpr double kLivePushSeconds = 0.25;

    /** Tests: freezes the animation clock at this time, so a picture shows a chosen moment. */
    void setClockForTests (double seconds) { m_clock.setManual (seconds); }
    /** The block-aligned repaint that must follow a repaint of `region` (empty when none is needed). */
    juce::Rectangle<int> followUpRepaint (juce::Rectangle<int> region) const;
    /** Tests: shows or hides a tooltip as the tooltip window would. */
    void showTooltipForTests (juce::Point<int> screenPosition, const juce::String& text) { m_tooltipWindow.displayTip (screenPosition, text); }
    void hideTooltipForTests() { m_tooltipWindow.hideTip(); }
    juce::Rectangle<int> tooltipBoundsForTests() const { return m_tooltipWindow.isVisible() ? m_tooltipWindow.getBounds() : juce::Rectangle<int>(); }
    /** Tests: draws an area into the framebuffer as a repaint of it would, and the finished picture. */
    void renderForTests (juce::Rectangle<int> area) { m_framebuffer.render (*this, area, [this] (juce::Graphics& fb) { paintContent (fb); }); }
    const juce::Image& framebufferForTests() const noexcept { return m_framebuffer.output(); }
    /** How long the last framebuffer repaint took (drawing the area afresh and finishing it). */
    double framebufferMilliseconds() const noexcept { return m_framebuffer.lastMilliseconds(); }
    /** How long the last FLIGHT frame took to draw. */
    double flightFrameMilliseconds() const noexcept { return m_flight.lastFrameMilliseconds(); }
    /** Texts anywhere in the editor that did not fit their box since the process started. */
    static int textOverflowCount() noexcept { return Hud::overflowCount(); }

    static constexpr int width = 980;
    static constexpr int height = 660;

private:
    void timerCallback() override;
    void learnClicked();
    void setupKnob (juce::Slider&, const char* paramId, const juce::String& tooltip,
                    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>&);
    void paintContent (juce::Graphics&);
    void paintHeader (juce::Graphics&);
    void paintReadout (juce::Graphics&);
    void paintGuide (juce::Graphics&);

    PitchFlattenerAudioProcessor& m_processor;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    PitchFlattenerLookAndFeel m_lookAndFeel;   // before every component, menu and window that uses it
    Hud::Clock m_clock;
    Framebuffer m_framebuffer;

    LearnButton m_learn { m_clock };
    HudButton m_clear { "CLEAR", HudButton::Style::Action }, m_about { "?", HudButton::Style::Action };
    HudButton m_scopeTab { "SCOPE", HudButton::Style::Tab }, m_flightTab { "FLIGHT", HudButton::Style::Tab };
    std::array<std::unique_ptr<HudButton>, 5> m_targets;
    std::array<std::unique_ptr<HudButton>, 4> m_sources;         // AUTO, VOICE, ENGINE, TONE
    std::array<std::unique_ptr<HudButton>, 2> m_lengthModes;     // VARISPEED, KEEP LENGTH
    std::array<std::unique_ptr<HudButton>, 3> m_characters;      // HARMONIC, TRANSIENTS, VOICE (Keep length only)
    // The curve editor's tools, in the display's header (SCOPE only).
    HudButton m_drawTool { "DRAW", HudButton::Style::Tab }, m_snapTool { "SNAP", HudButton::Style::Tab };
    HudButton m_undo { "UNDO", HudButton::Style::Action }, m_redo { "REDO", HudButton::Style::Action };
    HudButton m_resetCurve { "RESET CURVE", HudButton::Style::Action };
    HudButton m_zoomOut { "-", HudButton::Style::Action }, m_zoomIn { "+", HudButton::Style::Action }, m_fit { "FIT", HudButton::Style::Action };
    juce::Slider m_note, m_amount;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> m_noteAttachment, m_amountAttachment;

    PitchDisplay m_display;
    FlightView m_flight { m_clock };
    Banner m_banner { m_clock };
    BannerLogic m_bannerLogic;

    // Layout (set in resized, drawn in paint).
    juce::Rectangle<int> m_header, m_readout, m_displayPanel, m_viewArea, m_guide, m_targetCell, m_noteCell, m_amountCell, m_sourceCell, m_lengthCell, m_characterCell;

    // Undo for the corrections: what a reading's corrections were before each edit.
    struct EditStep
    {
        int line = -1;
        PitchDisplay::CurvePoints points;
    };
    std::vector<EditStep> m_undoSteps, m_redoSteps;
    std::uint64_t m_stepsFor = 0;           // the learned sound the steps belong to
    double m_lastLivePush = 0.0;            // when corrections were last sent while dragging
    void applyEdits (int line, PitchDisplay::CurvePoints points);
    void updateTools();

    juce::String m_status, m_learnText, m_lengthBefore, m_lengthAfter, m_flatText;
    juce::Colour m_statusColour = Theme::Colours::textDim;
    bool m_flatTagShown = false;
    juce::Rectangle<int> m_flatRow;

    juce::TooltipWindow m_tooltipWindow { this, 600 };
    std::unique_ptr<AboutWindow> m_aboutWindow;
    std::unique_ptr<UpdatePopup> m_updatePopup;
    std::unique_ptr<sammyj::UpdateCheck> m_updateCheck;   // after m_updatePopup: stopped before anything its callback touches goes

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchFlattenerAudioProcessorEditor)
};

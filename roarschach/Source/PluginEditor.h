#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "RoarschachLookAndFeel.h"
#include "Theme.h"
#include "MonsterView.h"
#include "SignalFlowStrip.h"
#include "DiceButton.h"
#include "AboutWindow.h"

#include <memory>

class UpdatePopup;
namespace sammyj { class UpdateCheck; struct UpdateInfo; }

/**
 * The editor is laid out once at a fixed design size (c_baseWidth x c_baseHeight) inside a single
 * content component, which is then scaled uniformly with an AffineTransform. Resizing therefore scales
 * text, knobs and the OpenGL view together and nothing reflows; the chosen size is saved with the
 * plugin state.
 *
 * The size a window opens at: the size saved with this instance; else the size last chosen on this
 * computer (any instance, any host); else the largest of 80-100 % that leaves room on the main display.
 *
 * Layout: the header (mark, name, tagline, website link, preset bar with its die, About), the creature
 * view, and the signal-flow strip with every control (and the ROLL ALL and module dice).
 *
 * Everything the editor opens (menus, alerts, tooltips, the About window) is drawn with its
 * RoarschachLookAndFeel, and is owned by it or dismissed when it is deleted, so nothing outlives it or
 * its LookAndFeel.
 */
class RoarschachAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RoarschachAudioProcessorEditor)

    friend struct RoarschachEditorInspector; // lets a test harness reach the parts; not defined in the plugin

public:
    explicit RoarschachAudioProcessorEditor(RoarschachAudioProcessor& audioProcessor);
    ~RoarschachAudioProcessorEditor() override;

    void resized() override;
    void paint(juce::Graphics&) override;
    void timerCallback() override;
    bool keyPressed(const juce::KeyPress&) override;

    static constexpr int c_baseWidth = 1760;
    static constexpr int c_baseHeight = 900;
    static constexpr float c_minScale = 0.65f, c_maxScale = 1.25f;
    static constexpr float c_smallestDefaultScale = 0.8f, c_largestDefaultScale = 1.0f;

    //==============================================================================
    // The window size
    /** The scale a first window opens at on a display with this work area: the largest of
        80-100 % that fits it with room left for the host's own window frame. */
    static float defaultScaleForDisplay(int workAreaWidth, int workAreaHeight) noexcept;
    /** Where the size last chosen on this computer is kept (beside the update check's settings). */
    static juce::File getSettingsFile();
    /** The scale kept in that file; 0 if there is none (or it cannot be read). */
    static float readRememberedScale(const juce::File& settingsFile);
    static bool rememberScale(const juce::File& settingsFile, float scale);
    /** How much the creature view draws, as last chosen on this computer (Full where none is kept). */
    static MonsterView::Visualiser readRememberedVisualiser(const juce::File& settingsFile);
    static bool rememberVisualiser(const juce::File& settingsFile, MonsterView::Visualiser visualiser);

    //==============================================================================
    /** Cmd+Z / Ctrl+Z (with Shift: redo) for the last roll or preset load. True if it undid or
        redid something; any other key, or nothing to take back, is false (the key is the host's). */
    bool undoKeyPressed(const juce::KeyPress& key);

    /** The plug-in's latency as the processor reports it, in words ("" before the host has
        prepared it). */
    static juce::String latencyNote(int latencySamples, double sampleRate);

    /** Pictures only: the preset list opens inside this component (so a snapshot can show it). */
    void setMenuParentForSnapshots(juce::Component* parent) { m_menuParentForSnapshots = parent; }

private:
    // Everything visible lives in here, at design size; the editor just scales it.
    struct Content : public juce::Component
    {
        std::function<void(juce::Graphics&)> onPaint;
        std::function<void()> onResized;
        void paint(juce::Graphics& g) override { if (onPaint) onPaint(g); }
        void resized() override { if (onResized) onResized(); }
    };

    void paintContent(juce::Graphics& g);
    void layoutContent();
    juce::Rectangle<float> wordmarkBounds() const;
    juce::Rectangle<float> creditLineBounds() const;
    float creditBaseline() const;

    /** The colour mood: the cloud's shown tone, eased, into the palette (Theme::applyMood) a few times a
        second, and what uses it repainted when it changed. Only the editor last opened or pointed at
        drives the (shared) palette. */
    void followMood();
    void applyMood(bool force);
    /** Takes the palette as it is now into everything this window drew with it (the LookAndFeel's
        colours, the picture's surround, the link, the About window) and repaints. Every open editor
        does this when the palette's serial moves, whichever editor moved it, so no window is ever drawn
        half in one mood and half in another. */
    void refreshPalette();

    struct MoodFocus final : public juce::MouseListener
    {
        explicit MoodFocus(RoarschachAudioProcessorEditor& e) : m_editor(e) {}
        void mouseEnter(const juce::MouseEvent&) override;
        RoarschachAudioProcessorEditor& m_editor;
    };
    MoodFocus m_moodFocus { *this };
    roar::CreatureMood::Tone m_moodTone { Theme::c_referenceHue, Theme::c_referenceChroma };
    int m_moodTick = 0;
    int m_paletteSerialSeen = -1;   // the palette this window was last drawn with (Theme::paletteSerial)
    juce::Component* m_menuParentForSnapshots = nullptr;
    void showAboutWindow();
    void updateMonsterParams();

    /** The "new version available" popup, drawn over the editor (called by the update check). */
    void showUpdatePopup(const sammyj::UpdateInfo& info);
    void closeUpdatePopup();

    // Presets
    void setupPresetBar();
    void showPresetMenu();
    void promptSavePreset(const juce::String& suggestedName);
    void checkNameAndSave(const juce::String& name);
    void confirmOverwrite(const juce::String& name, const juce::String& replacedName);
    void confirmDeletePreset(const juce::File& file, const juce::String& displayName);
    void savePresetAs(const juce::String& name);
    std::unique_ptr<juce::AlertWindow> makeAlert(const juce::String& title, const juce::String& message,
                                                 juce::MessageBoxIconType icon);
    void showAlert(std::unique_ptr<juce::AlertWindow> alert, std::function<void(int, juce::AlertWindow&)> onResult);
    void updatePresetDisplay();
    void afterPresetLoad(bool fromMenu);
    void rememberSizeNow();

    // Dice
    void setupDice();
    void afterRoll();
    void showDiceMenu(juce::Component& die);
    bool diceUndoKey(bool redo);

    /** A die or a preset bar button keeps the keyboard focus after a click (knobs never take it),
        and while it has the focus Cmd+Z undoes rolls and preset loads. Using any other control
        gives the focus up, so Cmd+Z goes back to the host's undo. */
    class DiceFocusRelease final : public juce::MouseListener
    {
    public:
        void mouseDown(const juce::MouseEvent& e) override { release(e); }
        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails&) override { release(e); }
        static void release(const juce::MouseEvent& e);
    };
    DiceFocusRelease m_diceFocusRelease;

    /** Tooltips in a desktop window of their own: the creature's OpenGL view is a native view on
        top of the editor's window, so it would cover any part of a tip the editor paints itself. */
    class EditorTooltipWindow final : public juce::TooltipWindow
    {
    public:
        explicit EditorTooltipWindow(juce::Component& editor);

        // Only this editor's controls: another instance's editor has its own tooltip window.
        juce::String getTipFor(juce::Component&) override;
        // The editor's scale, not that of its design-size content, so tips keep their size.
        float getDesktopScaleFactor() const override;

    private:
        juce::Component& m_editor;
    };

    RoarschachAudioProcessor& m_audioProcessor;

    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    RoarschachLookAndFeel m_lookAndFeel; // before every component, menu, alert and window that uses it

    Content m_content;
    MonsterView m_monsterView;
    SignalFlowStrip m_strip;
    bool m_sizeRestored = false;  // don't save the size until the saved one has been applied
    bool m_useMachineSettings = false;   // in a host or the standalone app (not under the tests)
    float m_openedScale = 0.0f;   // the scale the window opened at
    float m_scaleToRemember = 0.0f;   // a size the user chose, not written to the settings file yet
    double m_rememberAtMs = 0.0;      // ... and when to write it (once the resize has come to rest)


    juce::TextButton m_aboutButton;
    ThemeLink m_websiteLink { "SWEETJUSTICESOUND.COM", juce::URL("https://sweetjusticesound.com") };

    juce::TextButton m_presetPrevButton, m_presetNameButton, m_presetNextButton, m_presetSaveButton;
    DiceButton m_presetDice { "Random Preset" };
    std::unique_ptr<juce::AlertWindow> m_alert; // owned so it never outlives the LookAndFeel
    EditorTooltipWindow m_tooltipWindow { *this }; // after m_lookAndFeel, so it is deleted first
    std::unique_ptr<AboutWindow> m_aboutWindow;     // at most one per editor, closed with it
    std::unique_ptr<UpdatePopup> m_updatePopup;     // over everything while it is up
    std::unique_ptr<sammyj::UpdateCheck> m_updateCheck; // last: stopped before anything its callback touches goes
};

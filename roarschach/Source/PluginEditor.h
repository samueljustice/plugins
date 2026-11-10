#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "RoarschachLookAndFeel.h"
#include "Theme.h"
#include "MonsterView.h"
#include "SignalFlowStrip.h"

#include <memory>

class AboutWindow;
class UpdatePopup;
namespace sammyj { class UpdateCheck; struct UpdateInfo; }

/**
 * The editor is laid out once at a fixed design size (c_baseWidth x c_baseHeight) inside a single
 * content component, which is then scaled uniformly with an AffineTransform. Resizing therefore scales
 * text, knobs and the OpenGL view together and nothing reflows; the chosen size is saved with the
 * plugin state.
 *
 * Layout: the header (mark, name, tagline, website link, preset bar, About), the creature view, and the
 * signal-flow strip with every control.
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

    static constexpr int c_baseWidth = 1600;
    static constexpr int c_baseHeight = 900;

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


    juce::TextButton m_aboutButton;
    ThemeLink m_websiteLink { "SWEETJUSTICESOUND.COM", juce::URL("https://sweetjusticesound.com") };

    juce::TextButton m_presetPrevButton, m_presetNameButton, m_presetNextButton, m_presetSaveButton;
    std::unique_ptr<juce::AlertWindow> m_alert; // owned so it never outlives the LookAndFeel
    EditorTooltipWindow m_tooltipWindow { *this }; // after m_lookAndFeel, so it is deleted first
    std::unique_ptr<AboutWindow> m_aboutWindow;     // at most one per editor, closed with it
    std::unique_ptr<UpdatePopup> m_updatePopup;     // over everything while it is up
    std::unique_ptr<sammyj::UpdateCheck> m_updateCheck; // last: stopped before anything its callback touches goes
};

#pragma once

#include <JuceHeader.h>
#include "CargoHoldView.h"
#include "IconButton.h"
#include "LookAndFeel.h"
#include "ModPanel.h"
#include "MorphPad.h"
#include "PluginProcessor.h"
#include "Presets.h"
#include "SeaVisualiser.h"
#include "SectionScope.h"
#include "TakesView.h"
#include "Theme.h"
#include "LogoOutline.h"
#include "UpdatePopup.h"
#include "VariationsView.h"
#include "SammyJUpdateCheck.h"

/**
 * The ship. Laid out once at a fixed design size inside a content component
 * that is scaled uniformly, so the window can be resized without reflowing.
 *
 * One screen, in signal order (the whoosh rig):
 *
 *   header : title | play, new, undo, redo | presets, A / B | randomise, its history, keep | tape | last 60 s | variations | takes | settings
 *   row 1  : TRACKS (each loops; start markers; the ride drawn live)   | OUTPUT view (the sea) / RIDE
 *   row 2  : BUS A (aggressive)                                 ->     BUS B (smooth, ducked and filtered by A)
 *   row 3  : HITS (gated at the loudest moments)                ->     OUTPUT
 *   row 4  : MODULATION: four modulators; drag a jack onto any knob to wire it (MegaMod style)
 */
class SwooshbucklerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                          public juce::FileDragAndDropTarget,
                                          private juce::Timer
{
public:
    explicit SwooshbucklerAudioProcessorEditor(SwooshbucklerAudioProcessor&);
    ~SwooshbucklerAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool isKeyDown) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    /** A newer release is out: dims the ship and offers DOWNLOAD / LATER / SKIP THIS VERSION. */
    void showUpdatePopup(const sammyj::UpdateInfo& info);

    static constexpr int c_width = 1280;
    static constexpr int c_height = 1024;

private:
    struct Content : public juce::Component
    {
        std::function<void(juce::Graphics&)> onPaint;
        std::function<void(const juce::MouseEvent&)> onMouseDown;
        void paint(juce::Graphics& g) override { if (onPaint) onPaint(g); }
        void mouseDown(const juce::MouseEvent& e) override { if (onMouseDown) onMouseDown(e); }
    };

    /** Swell: hold it and every track's fader rides up; let go and the ride takes over again. */
    class SwellButton : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        SwellButton(SwooshbucklerAudioProcessor& p, const Theme::Fonts& f) : m_processor(p), m_fonts(f)
        {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            startTimerHz(60);
        }
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override { m_processor.setSwell(true); m_target = 0.9f; }
        void mouseUp(const juce::MouseEvent&) override { m_processor.setSwell(false); m_target = 1.0f; }
        void press(bool down) { m_target = down ? 0.9f : 1.0f; }
    private:
        void timerCallback() override;
        SwooshbucklerAudioProcessor& m_processor;
        const Theme::Fonts& m_fonts;
        float m_scale = 1.0f, m_velocity = 0.0f, m_target = 1.0f, m_glow = 0.0f;
    };

    /** The tape machine: two reels that spin while audio is being captured. Click to arm / disarm. */
    class TapeButton : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        TapeButton(SwooshbucklerAudioProcessor& p, const Theme::Fonts& f) : m_processor(p), m_fonts(f)
        {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            startTimerHz(30);
        }
        void paint(juce::Graphics&) override;
        void mouseUp(const juce::MouseEvent& e) override { if (onClick && contains(e.getPosition())) onClick(); }
        std::function<void()> onClick;
    private:
        void timerCallback() override;
        SwooshbucklerAudioProcessor& m_processor;
        const Theme::Fonts& m_fonts;
        float m_angle = 0.0f, m_blink = 0.0f;
        bool m_wasArmed = false;
    };

    /** Settings: where the tape goes and how it's written (type, bit depth, sample rate, channels), audio device. */
    class SettingsPanel : public juce::Component
    {
    public:
        explicit SettingsPanel(SwooshbucklerAudioProcessorEditor& owner);
        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseDown(const juce::MouseEvent&) override;
        void refresh();
    private:
        SwooshbucklerAudioProcessorEditor& m_owner;
        juce::TextButton m_choose { "CHOOSE..." }, m_reveal { "SHOW" }, m_close { "CLOSE" }, m_desktop { "DESKTOP" }, m_audio { "AUDIO DEVICE..." };
        std::array<juce::TextButton, 3> m_types, m_bits;
        std::array<juce::TextButton, 6> m_rates;
        std::array<juce::TextButton, 2> m_channels;
        juce::ToggleButton m_auto { "Arm the tape as soon as Swooshbuckler opens" };
        juce::ToggleButton m_skip { "Skip Silence" };
        // Harvesting: one file per whoosh, how files are named and who made them, and how much LAST 60s keeps.
        juce::ToggleButton m_perWhoosh { "One File Per Whoosh" };
        juce::TextEditor m_name, m_creator;
        std::array<juce::TextButton, 2> m_nameStyles;
        std::array<juce::TextButton, 3> m_retroLengths;
        std::unique_ptr<juce::FileChooser> m_chooser;
        juce::Rectangle<int> m_panel;
        std::array<juce::Rectangle<int>, 6> m_captions;
        std::array<juce::Rectangle<int>, 4> m_moreCaptions; // Take Name, Creator, File Names, Keep the Last
    };

    /** About: the version and build, what Swooshbuckler is, and the credits. Opened from the title or the version link. */
    class AboutPanel : public juce::Component, private juce::Timer
    {
    public:
        explicit AboutPanel(SwooshbucklerAudioProcessorEditor& owner);
        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseDown(const juce::MouseEvent&) override;
        void visibilityChanged() override;
    private:
        void timerCallback() override { repaint(m_logoArea); }
        SwooshbucklerAudioProcessorEditor& m_owner;
        juce::TextButton m_close { "CLOSE" }, m_website { "WEBSITE" }, m_copy { "COPY VERSION" };
        std::unique_ptr<LogoOutline> m_logo;
        double m_shownAt = 0.0;
        juce::Rectangle<int> m_panel, m_logoArea, m_textArea;
    };

    /** Glides parameters to new targets on springs (presets, Roll the Bones). */
    struct Morph
    {
        struct Item
        {
            juce::RangedAudioParameter* param = nullptr;
            float pos = 0.0f, vel = 0.0f, target = 0.0f;
        };
        std::vector<Item> items;
        bool step();
        void cancel(const juce::String& id);
        void finish();
    };

    struct Section
    {
        int number = 0;
        juce::String title, subtitle;
        juce::Colour accent;
        juce::Rectangle<int> bounds;
    };

    void paintContent(juce::Graphics&);
    void layoutContent();
    void timerCallback() override;

    WheelKnob& addKnob(const char* paramId, const juce::String& caption, juce::Colour accent, const juce::String& tip);
    void toggleLog();
    void saveRetro();
    void showPresetMenu();
    /** Glides the parameters to a look, as one undo step called undoName. presetName: what the preset box shows
        afterwards (empty = as it is). */
    void applyLook(const Presets::Preset& look, Presets::Unlisted unlisted, const juce::String& undoName,
                   const juce::String& presetName, bool outputStage = false);
    void stepPreset(int delta);
    // Presets: the factory ones, then your own (files in the presets folder).
    struct PresetItem
    {
        juce::String name;
        int factory = -1;   // index into Presets::factory(), or -1 for one of yours
        juce::File file;
    };
    std::vector<PresetItem> presetList() const;
    /** The preset list: factory presets, yours, saving and renaming, A / B and the session. currentFile gets the
        preset of yours that is loaded (if one is), recent the session files offered. */
    juce::PopupMenu buildPresetMenu(const std::vector<PresetItem>& items, juce::File& currentFile, juce::Array<juce::File>& recent) const;
    void choosePreset(const PresetItem& item);
    void loadPresetFile(const juce::File& file, bool addToPresets);
    void savePresetAs();
    void renamePreset(const juce::File& file);
    void deletePreset(const juce::File& file);
    /** A small window asking for a name (and, with modsOption, whether the modulators and wires go in too). */
    void askForName(const juce::String& title, const juce::String& message, const juce::String& initial, const juce::String& okText,
                    bool modsOption, std::function<void(const juce::String&, bool)> done);
    std::unique_ptr<juce::ToggleButton> m_dialogToggle;
    // Randomise: the header's roll or a section's die (section -1 = the header), the way back, and keeping one.
    void rollDice(int section);
    void stepRoll(int delta);
    void keepRoll();
    // Undo / redo: the processor's one history (parameters, presets, rolls, tracks), from the buttons and the keys.
    void stepHistory(bool redo);
    // Session files
    void saveSession(bool saveAs);
    void openSession();
    void openSessionFile(const juce::File& file);
    std::unique_ptr<juce::FileChooser> m_sessionChooser;
    void toast(const juce::String& text, juce::File file = {});

    SwooshbucklerAudioProcessor& m_processor;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    SwooshLookAndFeel m_lnf;
    juce::TooltipWindow m_tooltips { nullptr, 600 };

    Content m_content;

    // Header
    juce::TextButton m_presetPrev { "<" }, m_presetName, m_presetNext { ">" }, m_dice { "RANDOMISE" };
    IconButton m_undoButton { IconButton::Icon::Undo, Theme::Colours::flight }, m_redoButton { IconButton::Icon::Redo, Theme::Colours::flight };
    IconButton m_rollBack { IconButton::Icon::Back, Theme::Colours::movement }, m_rollForward { IconButton::Icon::Forward, Theme::Colours::movement };
    IconButton m_keepRoll { IconButton::Icon::Star, Theme::Colours::movement };
    juce::TextButton m_abButton { "A" };
    // A die and a lock in each section's title bar: Ride, Bus A, Bus B, Hits, Output.
    std::array<std::unique_ptr<IconButton>, Presets::numSections> m_sectionDice, m_sectionLocks;
    juce::TextButton m_retroButton { "LAST 60s" }, m_settingsButton { "SETTINGS" }, m_takesButton { "Takes" };
    int m_retroShown = 0, m_takesShown = -1, m_takesSeen = -1, m_slowTick = 0; // what those two buttons last showed
    void showTakes();
    // Render Variations: the button opens the panel, and shows how far a batch is while one renders.
    juce::TextButton m_variationsButton { "Variations" };
    int m_variationsShown = -2; // the percentage the button last showed (-1 = no batch rendering)
    void showVariations();
    juce::TextButton m_playButton { "PLAY" }, m_newButton { "NEW" };
    TapeButton m_tape;
    juce::HyperlinkButton m_websiteLink { "www.sweetjusticesound.com", juce::URL("https://www.sweetjusticesound.com") };

    SeaVisualiser m_sea;
    MorphPad m_morphPad;
    juce::TextButton m_morphButton { "MORPH" }, m_syncHits { "SYNC HITS" }, m_aimHits { "AIM" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_aimAttachment;
    int m_aimShown = -1;   // the AIM state the gate controls were last dimmed for
    int m_tremShown = -1;  // the tremolo on / off state its controls were last dimmed for
    void showAimControls();
    // The PEAK TREMOLO group's menus (in its frame at the left of Output) and the Master Trim fader (by the OUT meter).
    juce::ComboBox m_tremShape, m_tremTarget;
    juce::ComboBox m_tremOn;    // Off / On
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_tremOnAttachment;
    juce::Rectangle<int> m_tremGroup; // the framed PEAK TREMOLO group at the left of the Output panel
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_tremShapeAttachment, m_tremTargetAttachment;
    juce::Slider m_trim;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> m_trimAttachment;
    juce::Rectangle<int> m_trimLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_syncAttachment;
    // Gate and ducking options (defaults = the original rig)
    juce::ComboBox m_gateMode, m_duckMode;
    juce::TextButton m_takeTurns { "TAKE TURNS" };
    juce::TextButton m_linkBuses { "LINK" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_linkBusesAttachment;
    int m_linkShown = -1; // the LINK state the bus B knobs were last dimmed for
    void showLinkedKnobs();
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_gateModeAttachment, m_duckModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_takeTurnsAttachment;
    juce::Rectangle<int> m_gateModeLabel, m_duckModeLabel;
    // Pass Mode, one menu in each bus's title bar (bus B's rests while LINK is on: it uses bus A's).
    std::array<juce::ComboBox, ParamIDs::numBuses> m_passMode;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>, ParamIDs::numBuses> m_passModeAttachment;
    std::array<juce::Rectangle<int>, ParamIDs::numBuses> m_passModeLabel;
    // A little waveform in each section's title bar: bus A, bus B, hits, output.
    std::array<std::unique_ptr<SectionScope>, 4> m_sectionScopes;
    void syncMorphPad();
    int m_morphSyncTick = 0;
    SwellButton m_swell;
    juce::TextButton m_rideButton { "AUTO RIDE" };
    juce::TextButton m_followMasterButton { "Follow Master" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_followMasterAttachment;
    int m_followMasterShown = -1; // what the Follow Master button's dimming was last set for
    // Perform: Throw (one whoosh on demand), what MIDI keys do, and (in a host) Follow Host by the Play button.
    juce::TextButton m_throwButton { "Throw" }, m_followHost { "Follow Host" };
    juce::ComboBox m_keysMode;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_keysModeAttachment;
    juce::Rectangle<int> m_keysLabel, m_rideStatusArea;
    juce::String m_rideStatus; // the Ride panel's subtitle line: the time to a throw's peak, the held keys, or what the panel does
    juce::String rideStatus() const;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_rideAttachment;
    bool m_spaceHeld = false;

    std::vector<std::unique_ptr<WheelKnob>> m_knobs;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> m_attachments;
    std::map<juce::String, juce::Slider*> m_controls;

    CargoHoldView m_hold;
    ModWires m_wires;    // the cables, over everything
    ModPanel m_modPanel; // the modulators at the bottom
    juce::TextButton m_cablesButton { "CABLES" };
    juce::Rectangle<int> m_modSection;
    SettingsPanel m_settings;
    TakesView m_takes;
    VariationsView m_variations;
    AboutPanel m_about;
    juce::HyperlinkButton m_versionLink;
    juce::Rectangle<int> m_titleArea;
    juce::String versionLine() const;

    Morph m_morph;

    juce::String m_toast;
    juce::File m_toastFile;
    double m_toastTime = 0.0;
    // Output meters: the bars, and the peak holds (the deepest reduction / highest level of the last moments).
    float m_grDb = 0.0f, m_outLevel = 0.0f, m_grHoldDb = 0.0f, m_outHold = 0.0f;
    double m_meterTime = 0.0, m_grHoldTime = 0.0, m_outHoldTime = 0.0;
    static constexpr float c_grFallDbPerSecond = 12.0f;
    static constexpr double c_meterHoldSeconds = 1.5;
    /** Where a level sits on the OUT meter, 0 (-60 dB) to 1 (0 dBFS): the top of the range gets the most room. */
    static float outMeterPosition(float db)
    {
        const float t = juce::jlimit(0.0f, 1.0f, 1.0f + db / 60.0f);
        return t * t;
    }

    juce::Rectangle<int> m_performPanel, m_meterArea, m_gateLamp, m_footer, m_toastArea, m_busKnobArea;
    std::array<Section, 4> m_sections; // ride, buses, hits, output
    float m_gate = 0.0f;
    bool m_sizeRestored = false;

    // Over everything, the content included. The check is declared after the popup so it is destroyed first:
    // its destructor stops the thread and drops any pending callback before the popup goes.
    UpdatePopup m_updatePopup { *m_fonts };
    std::unique_ptr<sammyj::UpdateCheck> m_updateCheck;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwooshbucklerAudioProcessorEditor)
};

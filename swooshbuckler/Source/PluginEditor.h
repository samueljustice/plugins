#pragma once

#include <JuceHeader.h>
#include "CargoHoldView.h"
#include "IconButton.h"
#include "LookAndFeel.h"
#include "ModPanel.h"
#include "MorphPad.h"
#include "Motion.h"
#include "PluginProcessor.h"
#include "Presets.h"
#include "SeaVisualiser.h"
#include "SectionScope.h"
#include "Shortcuts.h"
#include "TakesView.h"
#include "Theme.h"
#include "Tooltips.h"
#include "LogoOutline.h"
#include "UpdatePopup.h"
#include "VariationsView.h"
#include "SammyJUpdateCheck.h"

/**
 * The window. Laid out once at a fixed design size inside a content component
 * that is scaled uniformly, so the window can be resized without reflowing.
 *
 * Keys (never while a text box is being typed in) are not written into this window: every one of them is an
 * action in Shortcuts.h, with a default key the user can change in Settings. The same registry gives the key
 * shown beside an item in the menus and at the end of a control's tooltip, and fills the Mac menu bar in the app.
 *
 * One screen, in signal order (the whoosh rig):
 *
 *   header : title | transport: follow host, play, new | presets: undo, redo, presets, A / B, randomise, its history, keep
 *            | tape: the tape, last 60 s, variations, takes | settings
 *   row 1  : TRACKS (each loops; start markers; the ride drawn live)   | the sea (what the rig sends out)
 *   row 2  : RIDE | BUS A (aggressive)  ->  BUS B (smooth, ducked and filtered by A)
 *   row 3  : HITS (gated at the loudest moments; its knobs under bus A's)  ->  OUTPUT
 *   row 4  : MODULATION: four modulators; drag a jack onto any knob to wire it (MegaMod style)
 *
 * One 12 px gutter between panels and around the window; bus A, bus B and the Hits share one knob column pitch.
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

    /** A newer release is out: dims the window and offers Download, Later and Skip This Version. */
    void showUpdatePopup(const sammyj::UpdateInfo& info);

    /** Whether an editor looks for a newer release when it opens. Tools and tests switch it off before making one. */
    static inline bool checkForUpdates = true;

    /** The design size: a landscape window. 1520 wide is the widest that still opens whole on a 1536-point laptop
        screen, and wide enough for the tracks and the sea to sit side by side over three panels of knobs on one
        column pitch; 900 high keeps the mock-up's proportions. The window scales uniformly from here. */
    static constexpr int c_width = 1520;
    static constexpr int c_height = 900;
    /** The smallest and largest the window gets. At the smallest, the 10 px text is 8.5 px on screen. */
    static constexpr float c_minScale = 0.85f, c_maxScale = 2.0f;
    /** The width the window first opens at: the design size, or the largest that fits the screen it is on. */
    static int firstOpenWidth(juce::Rectangle<int> usableArea);

    /** The panels bolted to the window, by name, in design coordinates: none may touch another (UiTest checks). */
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> panelBounds() const;

private:
    struct Content : public juce::Component
    {
        std::function<void(juce::Graphics&)> onPaint;
        std::function<void(const juce::MouseEvent&)> onMouseDown;
        void paint(juce::Graphics& g) override { if (onPaint) onPaint(g); }
        void mouseDown(const juce::MouseEvent& e) override { if (onMouseDown) onMouseDown(e); }
    };

    /** Swell: hold it and every track's fader rides up; let go and the ride takes over again.
        It squashes on the Swell spring and its halo follows the energy of the ride. */
    class SwellButton : public juce::Component, public juce::SettableTooltipClient, public Motion::ClockClient
    {
    public:
        SwellButton(SwooshbucklerAudioProcessor& p, const Theme::Fonts& f) : m_processor(p), m_fonts(f)
        {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            m_scale.snap(1.0f);
        }
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override { m_processor.setSwell(true); press(true); }
        void mouseUp(const juce::MouseEvent&) override { m_processor.setSwell(false); press(false); }
        void press(bool down) { m_scale.to(down ? 0.9f : 1.0f); }
        /** The key that holds Swell, as the user reads it: the hint under the word follows it when it is changed
            (empty when Swell has been left with no key, and then the hint is just "Hold"). */
        void setKeyHint(const juce::String& key) { if (key != m_keyHint) { m_keyHint = key; repaint(); } }
    private:
        juce::String m_keyHint;
        void frame(float dt) override;
        SwooshbucklerAudioProcessor& m_processor;
        const Theme::Fonts& m_fonts;
        Motion::Spring m_scale;
        float m_glow = 0.0f;
    };

    /** The tape machine: two reels that spin up when capture starts and coast down when it stops, and a record
        lamp that breathes while it is armed and waiting. Click to arm / disarm. */
    class TapeButton : public juce::Component, public juce::SettableTooltipClient, public Motion::ClockClient
    {
    public:
        TapeButton(SwooshbucklerAudioProcessor& p, const Theme::Fonts& f) : m_processor(p), m_fonts(f)
        {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }
        void paint(juce::Graphics&) override;
        void mouseUp(const juce::MouseEvent& e) override { if (onClick && contains(e.getPosition())) onClick(); }
        /** Last 60 s was pressed: the reels flick back half a turn. Playing winds the angle down, so going back
            over what has just been played winds it up. */
        void rewind() { m_rewind = juce::MathConstants<float>::pi; }
        std::function<void()> onClick;
    private:
        void frame(float dt) override;
        SwooshbucklerAudioProcessor& m_processor;
        const Theme::Fonts& m_fonts;
        float m_angle = 0.0f, m_breath = 0.0f, m_speed = 0.0f, m_rewind = 0.0f, m_lamp = 0.0f;
        bool m_wasArmed = false;
    };

    /**
     * The keys, in Settings: every action with the key it has now. Click a row and press a key to give it that
     * one, Esc leaves the row as it was, and a key another action already has is offered to be moved.
     */
    class ShortcutsList : public juce::Component
    {
    public:
        explicit ShortcutsList(SwooshbucklerAudioProcessorEditor& owner);
        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseMove(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        bool keyPressed(const juce::KeyPress&) override;
        void focusLost(FocusChangeType) override;
        /** Whether a row is waiting for a key: while it is, the window's own keys are off. */
        bool isListening() const { return m_listening >= 0; }
        void stopListening();
        /** The row height and how tall the whole list is, so the panel can give it the room. */
        static constexpr int c_row = 22;
        static int idealHeight() { return Shortcuts::numActions * c_row; }
    private:
        juce::Rectangle<int> rowBounds(int index) const;
        int rowAt(juce::Point<int> p) const;
        void bind(Shortcuts::Action action, const juce::KeyPress& key);
        SwooshbucklerAudioProcessorEditor& m_owner;
        int m_listening = -1, m_over = -1;
    };

    /** Settings: where the tape goes and how it's written (type, bit depth, sample rate, channels), audio
        device, and the keys. */
    class SettingsPanel : public juce::Component
    {
    public:
        explicit SettingsPanel(SwooshbucklerAudioProcessorEditor& owner);
        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseDown(const juce::MouseEvent&) override;
        void refresh();
        /** The keys list, so the window can ask whether it is waiting for a key. */
        ShortcutsList& keys() { return m_keysList; }
    private:
        SwooshbucklerAudioProcessorEditor& m_owner;
        ShortcutsList m_keysList;
        juce::TextButton m_resetKeys { "Reset To Defaults" };
        juce::Rectangle<int> m_keysCaption, m_keysHint;
        juce::TextButton m_choose { "Choose..." }, m_reveal { "Show" }, m_close { "Close" }, m_desktop { "Desktop" }, m_audio { "Audio Device..." };
        std::array<juce::TextButton, 3> m_types, m_bits;
        std::array<juce::TextButton, 6> m_rates;
        std::array<juce::TextButton, 2> m_channels;
        juce::ToggleButton m_auto { "Auto-Arm on Opening" };
        juce::ToggleButton m_skip { "Skip Silence" };
        // Harvesting: one file per whoosh, how files are named and who made them, and how much Last 60 s keeps.
        juce::ToggleButton m_perWhoosh { "One File per Whoosh" };
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
        void showNotices(bool show);
        juce::TextButton m_close { "Close" }, m_website { "Website" }, m_copy { "Copy Version" }, m_licences { "Licences" };
        juce::TextEditor m_notices; // the third-party notices, shown in place of the About text
        std::unique_ptr<LogoOutline> m_logo;
        double m_shownAt = 0.0;
        juce::Rectangle<int> m_panel, m_logoArea, m_textArea;
    };

    /** Glides parameters to new targets on the Swell spring (presets, Roll the Bones). Each knob waits a little
        longer than the one to its left, so a preset passes through the window like a swell. */
    struct Morph
    {
        struct Item
        {
            juce::RangedAudioParameter* param = nullptr;
            float pos = 0.0f, vel = 0.0f, target = 0.0f;
            float delay = 0.0f; // seconds still to wait before this one starts moving
        };
        std::vector<Item> items;
        /** Advances the glide by dt seconds. True while anything is still moving. */
        bool step(float dt);
        void cancel(const juce::String& id);
        void finish();
        /** The longest a preset takes to pass through the panels, in seconds. */
        static constexpr float c_wave = 0.24f, c_waveStep = 0.012f;
    };
    /** Gives the items of a glide their delays: left to right, row by row, from where the knobs are. */
    void stagger();

    struct Section
    {
        int number = 0;
        juce::String title, subtitle;
        juce::Colour accent;
        juce::Rectangle<int> bounds;
        Icons::Icon mark = Icons::Icon::None;
        int subtitleRight = 0; // where the subtitle has to stop: the first control in the title bar
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
    /** That window, built and not yet shown (the caller owns it; its switch is m_dialogToggle). */
    juce::AlertWindow* makeNameDialog(const juce::String& title, const juce::String& message, const juce::String& initial,
                                      const juce::String& okText, bool modsOption);
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
    /** The keys, and the one frame clock everything that animates runs on. Both are declared before the
        components, so they are made first and destroyed last. */
    Shortcuts::Registry m_keys;
    Motion::FrameClock m_clock;
    double m_frameTime = 0.0;
    SwooshTooltips m_tooltips { nullptr, m_keys };

    // What a key, a menu item or a button asks for; one place, so a key and its menu item can never drift apart.
    void doAction(Shortcuts::Action action);
    /** The keys have changed: the menu bar and the tooltips that name one are written again. */
    void refreshShortcuts();
    /** Whether the action can be used now (a session key in a host belongs to the host, Redo with nothing to redo). */
    bool canDoAction(Shortcuts::Action action) const;
    /** Something is being typed into a text box: no single key fires. */
    static bool isTyping();

   #if JUCE_MAC
    /** The app's menu bar: the same actions, with the keys the user has given them. */
    struct MenuBar;
    std::unique_ptr<MenuBar> m_menuBar;
   #endif

    Content m_content;

    // Header
    IconButton m_presetPrev { IconButton::Icon::Previous, Theme::Colours::brass }, m_presetNext { IconButton::Icon::Next, Theme::Colours::brass };
    juce::TextButton m_presetName, m_dice { "Randomise" };
    IconButton m_undoButton { IconButton::Icon::Undo, Theme::Colours::flight }, m_redoButton { IconButton::Icon::Redo, Theme::Colours::flight };
    IconButton m_rollBack { IconButton::Icon::Back, Theme::Colours::movement }, m_rollForward { IconButton::Icon::Forward, Theme::Colours::movement };
    IconButton m_keepRoll { IconButton::Icon::Star, Theme::Colours::movement };
    juce::TextButton m_abButton { "A" };
    // A die and a lock in each section's title bar: Ride, Bus A, Bus B, Hits, Output.
    std::array<std::unique_ptr<IconButton>, Presets::numSections> m_sectionDice, m_sectionLocks;
    juce::TextButton m_retroButton { "Last 60 s" }, m_settingsButton { "Settings" }, m_takesButton { "Takes" };
    int m_retroShown = 0, m_takesShown = -1, m_takesSeen = -1, m_slowTick = 0; // what those two buttons last showed
    void showTakes();
    // Render Variations: the button opens the panel, and shows how far a batch is while one renders.
    juce::TextButton m_variationsButton { "Variations" };
    int m_variationsShown = -2; // the percentage the button last showed (-1 = no batch rendering)
    void showVariations();
    juce::TextButton m_playButton { "Play" }, m_newButton { "New" };
    TapeButton m_tape;
    juce::HyperlinkButton m_websiteLink { "www.sweetjusticesound.com", juce::URL("https://www.sweetjusticesound.com") };

    SeaVisualiser m_sea;
    MorphPad m_morphPad;
    juce::TextButton m_morphButton { "Morph" }, m_syncHits { "Sync Hits" }, m_aimHits { "Aim" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_aimAttachment;
    int m_aimShown = -1;   // the Aim state the gate controls were last dimmed for
    int m_tremShown = -1;  // the tremolo on / off state its controls were last dimmed for
    void showAimControls();
    // The Peak Tremolo group (a tinted inset at the left of Output): its switch and two menus over its three knobs;
    // and the Master Trim fader (by the Out meter).
    juce::ComboBox m_tremShape, m_tremTarget;
    juce::ToggleButton m_tremOn; // a switch: the tremolo is on or off
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_tremOnAttachment;
    juce::Rectangle<int> m_tremGroup, m_tremCaption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_tremShapeAttachment, m_tremTargetAttachment;
    ValueSlider m_trim;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> m_trimAttachment;
    juce::Rectangle<int> m_trimLabel, m_trimValue;
    juce::String m_grReadout, m_outReadout, m_trimReadout; // the numbers under the meters and the fader, in dB
    int m_grReadoutTenths = 0, m_outReadoutTenths = 0;
    void updateMeterReadouts();
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_syncAttachment;
    // Gate and ducking options
    juce::ComboBox m_gateMode, m_duckMode;
    juce::TextButton m_takeTurns { "Take Turns" };
    juce::TextButton m_linkBuses { "Link" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_linkBusesAttachment;
    int m_linkShown = -1; // the Link state the bus B knobs were last shown for
    void showLinkedKnobs();
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_gateModeAttachment, m_duckModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_takeTurnsAttachment;
    juce::Rectangle<int> m_gateModeLabel, m_duckModeLabel;
    // Pass Mode, one menu in each bus's title bar (bus B's is linked while Link is on: it uses bus A's).
    std::array<juce::ComboBox, ParamIDs::numBuses> m_passMode;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>, ParamIDs::numBuses> m_passModeAttachment;
    std::array<juce::Rectangle<int>, ParamIDs::numBuses> m_passModeLabel;
    // A little waveform in each section's title bar: bus A, bus B, hits, output.
    std::array<std::unique_ptr<SectionScope>, 4> m_sectionScopes;
    void syncMorphPad();
    int m_morphSyncTick = 0;
    SwellButton m_swell;
    juce::TextButton m_rideButton { "Auto Ride" };
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
    juce::TextButton m_cablesButton { "Cables" };
    juce::Rectangle<int> m_modSection;
    SettingsPanel m_settings;
    TakesView m_takes;
    VariationsView m_variations;
    AboutPanel m_about;
    juce::HyperlinkButton m_versionLink;
    juce::Rectangle<int> m_titleArea;
    juce::String versionLine() const;

    Morph m_morph;

    /** The line along the foot of the window. It names the key Swell has, so it is written in refreshShortcuts
        rather than in paint. */
    juce::String m_footerHint;
    juce::String m_toast;
    double m_toastTime = 0.0;
    /** The toast's plate: it rises 8 px and fades in at the top of the open water, holds, and fades out. It is a
        child of its own so it lies over the sea and the panels; the footer keeps the same line as a log. */
    struct ToastPlate : public juce::Component
    {
        explicit ToastPlate(SwooshbucklerAudioProcessorEditor& o) : owner(o) { setInterceptsMouseClicks(false, false); }
        void paint(juce::Graphics&) override;
        SwooshbucklerAudioProcessorEditor& owner;
    };
    ToastPlate m_toastPlate { *this };
    Motion::Fade m_toastRise;
    juce::Rectangle<int> m_toastPill;
    void placeToast(juce::Rectangle<int> near);
    /** Randomise: the die that was rolled tumbles through three faces for 300 ms, then settles on five.
        Nothing else on the button moves. */
    double m_diceTime = 0.0;
    int m_diceStep = -1;
    juce::Component* m_diceRolling = nullptr; // the die that is tumbling: a child of ours, alive as long as we are
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
    juce::Rectangle<int> m_fromBusAGroup, m_fromBusACaption; // the tinted group around bus B's Ducked and Filtered
    std::array<int, 2> m_headerRules {};                     // the x of the rules between the header's groups
    // The ride of the last 8 seconds, in the Ride panel: each bus's faders (the mean of its tracks' rides).
    class RideStrip;
    std::unique_ptr<RideStrip> m_rideStrip;
    juce::Rectangle<int> m_rideStripArea;
    std::array<Section, 4> m_sections; // ride, buses, hits, output
    float m_gate = 0.0f;
    bool m_sizeRestored = false;

    // Over everything, the content included. The check is declared after the popup so it is destroyed first:
    // its destructor stops the thread and drops any pending callback before the popup goes.
    UpdatePopup m_updatePopup { *m_fonts };
    std::unique_ptr<sammyj::UpdateCheck> m_updateCheck;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwooshbucklerAudioProcessorEditor)
};

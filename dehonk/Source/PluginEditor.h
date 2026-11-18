#pragma once

#include <JuceHeader.h>

#include "DeHonkLookAndFeel.h"
#include "DisplayComponent.h"
#include "Overlays.h"
#include "PluginProcessor.h"
#include "Theme.h"
#include "Widgets.h"

#include "SammyJUpdateCheck.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>

/** A large listening button (Solo Honk, Diff): a rounded square that stays pressed down, in its
    second colour, with its name bobbing, while on. */
class ListenButton : public juce::Button
{
public:
    ListenButton (const juce::String& name, juce::Colour colour, juce::Colour whenActive);

    /** 0..1, set by the editor's timer while the button is on. */
    void setPulse (float amount);

protected:
    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
    virtual bool isActive() const { return getToggleState(); }

private:
    juce::SharedResourcePointer<Theme::Fonts> fonts;
    juce::Colour colour, activeColour;
    float pulse = 0.0f;
};

/** Diff: on while the mouse is held; a click without a drag latches it on, and the next click
    releases it (SPEC 4.1). Space or Return toggles the latch. */
class DiffButton : public ListenButton
{
public:
    DiffButton();

    std::function<void (bool)> onActiveChange;

    bool active() const noexcept { return on; }
    /** Turns it off without calling onActiveChange (the other listening tool took over, or the
        processor's state was restored). */
    void release();
    /** Turns it on and latched without calling onActiveChange (adopting the processor's state). */
    void latch();

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

protected:
    void clicked() override;
    bool isActive() const override { return on; }

private:
    void setActive (bool shouldBeOn);

    bool on = false, latched = false, releasingLatch = false, dragged = false;
    juce::uint32 pressTime = 0;

    static constexpr juce::uint32 clickMs = 350;  // longer than this is a hold, not a click
};

//==============================================================================
/** Detection: one pill in two parts, Fast and Steady, the chosen one filled, as Link's. */
class DetectionSelector : public juce::Component,
                          public juce::SettableTooltipClient
{
public:
    DetectionSelector (juce::RangedAudioParameter& detection, const Theme::Fonts&);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

private:
    int segmentAt (juce::Point<float>) const noexcept;
    void choose (int mode);

    const Theme::Fonts& fonts;
    juce::ParameterAttachment attachment;
    int mode = 0, hotSegment = -1;
};

//==============================================================================
/** De-Honk's window. From the top: the name and the presets; the pond; the band view, where the
    range edges, the Threshold and Max Cut are dragged; the shelf of range tabs with the selected
    range's drawer; the knobs with the listening buttons, Mix and Trim; and the row of Impact
    Guard, Link, Lookahead and Dialogue.

    Its dialogs, its About page, its menus and its tooltips are all drawn inside it. */
class DeHonkAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit DeHonkAudioProcessorEditor (DeHonkAudioProcessor&);
    ~DeHonkAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    bool keyStateChanged (bool isKeyDown) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void parentHierarchyChanged() override;

    static constexpr int defaultWidth = 960, defaultHeight = 800;
    static constexpr int minWidth = 820, minHeight = 700;

    //============================================================================== for pictures and checks
    /** Selects a range's tab: 0 Woof, 1 Tub, 2 Fog, 3 Bark, 4 Snap, -1 none. */
    void selectRange (int range) { shelf.select (range); }
    void showAbout();
    void showUpdate (const sammyj::UpdateInfo&);
    void showPresetMenu();
    void savePreset();
    void deletePreset();
    ToyTooltip& tips() noexcept { return tooltip; }
    OverlayCard* currentOverlay() const noexcept { return overlay.get(); }

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void timerCallback() override;
    void updateShowing();
    void startUpdateCheckWhenShown();
    juce::RangedAudioParameter* handleParam (int handle) const;
    void setUpKnob (juce::Slider&, juce::Label&, const juce::String& caption, const char* paramID, const juce::String& tip);
    void setUpBar (juce::Slider&, juce::Label&, const juce::String& caption, const char* paramID, const juce::String& tip);
    void setUpCaption (juce::Label&, const juce::String& text);
    void setSoloHonk (bool on);
    void setDiff (bool on);
    void toggleGuard();
    void updatePresetBar();
    void showOverlay (std::unique_ptr<OverlayCard>);
    void overlayClosed (OverlayCard*);
    void showFrame (const dehonk::DisplayFrame&);

    DeHonkAudioProcessor& deHonk;
    juce::SharedResourcePointer<Theme::Fonts> fonts;
    DeHonkLookAndFeel lookAndFeel { *fonts };

    DisplayComponent display;
    dehonk::scene::BandView bandView;
    RangeShelf shelf;

    TitleBlock title { *fonts };
    juce::TextButton previousButton { "<" }, nextButton { ">" }, presetButton, saveButton { "Save" }, deleteButton { "Delete" };
    juce::TextButton bypassButton { "Bypass" };

    juce::Slider thresholdKnob, depthKnob, maxCutKnob, sharpnessKnob, speedKnob, releaseKnob;
    juce::Label thresholdCaption, depthCaption, maxCutCaption, sharpnessCaption, speedCaption, releaseCaption;
    DetectionSelector detectionSelector;    // under Sharpness, with its name to its left
    juce::Label detectionCaption;
    ListenButton soloButton { "Solo Honk", dehonk::toy::sunYellow, dehonk::toy::jacketRed };
    DiffButton diffButton;

    juce::Slider guardBar, dialogueBar, mixBar, trimBar;
    SwitchLamp guardSwitch { "Impact Guard" }, dialogueSwitch { "Dialogue" };
    LinkSelector linkSelector;
    juce::ComboBox lookaheadBox;
    juce::Label guardCaption, linkCaption, dialogueCaption, mixCaption, trimCaption, lookaheadCaption;

    std::unique_ptr<SliderAttachment> thresholdAttachment, depthAttachment, maxCutAttachment, sharpnessAttachment, speedAttachment,
                                      releaseAttachment, guardAttachment, dialogueAmountAttachment, mixAttachment, trimAttachment;
    std::unique_ptr<ComboBoxAttachment> lookaheadAttachment;
    std::unique_ptr<ButtonAttachment> bypassAttachment, dialogueAttachment;

    std::unique_ptr<OverlayCard> overlay;
    std::optional<sammyj::UpdateInfo> pendingUpdate;
    std::unique_ptr<sammyj::UpdateCheck> updateCheck;
    ToyTooltip tooltip { *this, *fonts };      // after everything it may be shown over

    bool showing = false;  // registered with the processor as showing; display timer running
    juce::String shownPresetText;
    std::array<bool, dehonk::kNumModules + 1> cutting {};
    double snapLitUntilMs = 0.0;
    float shownKeyLamp = -1.0f;
    bool shownKeyWarn = false;
    double keyUnfoundMs = 0.0, keyWarnUntilMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeHonkAudioProcessorEditor)
};

#pragma once

#include <JuceHeader.h>

#include "DeHonkLookAndFeel.h"
#include "DisplayComponent.h"
#include "PluginProcessor.h"
#include "Theme.h"

#include <array>
#include <functional>
#include <memory>

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

/** A module switch (Woof, Tub, Fog, Bark, Snap): a fat pill in the module's colour while on, with a
    light that comes on while it is cutting and a small meter of the cut beside its name.

    The meter reads 0 to fullScaleDb. With a hold time it jumps up at once and falls back to zero
    over that time (Click: its cuts are over in milliseconds); without one it follows the value. */
class ModuleButton : public juce::Button
{
public:
    ModuleButton (const juce::String& name, juce::Colour colour, float fullScaleDb, double holdSeconds = 0.0);

    /** The module's deepest cut, dB (positive = reduction). */
    void setCutDb (float db);
    /** What a full meter stands for, dB. For Woof, Tub, Fog and Bark it is Max Cut as it is set,
        so a module cutting as deep as it may shows a full meter. */
    void setFullScaleDb (float db);
    /** Lets a held reading fall. Called by the editor's timer with the time since the last call. */
    void advance (double seconds);

    float shownDb() const noexcept { return shown; }

protected:
    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

private:
    void show (float db);

    juce::SharedResourcePointer<Theme::Fonts> fonts;
    juce::Colour colour;
    float fullScale;
    const double hold;
    float shown = 0.0f, peak = 0.0f;
    double sincePeak = 0.0;
};

/** A small round lamp, a disc like the big buttons: lit while the impact guard is up. Its
    value is how far, 0 to 1. */
class GuardLamp : public juce::Component,
                  public juce::SettableTooltipClient
{
public:
    void setHeld (float amount);
    void paint (juce::Graphics&) override;

private:
    float held = 0.0f;
};

//==============================================================================
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
    void parentHierarchyChanged() override;

    static constexpr int defaultWidth = 960, defaultHeight = 800;
    static constexpr int minWidth = 820, minHeight = 700;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void timerCallback() override;
    void updateShowing();
    juce::RangedAudioParameter* edgeParam (int edge) const;
    void setUpKnob (juce::Slider&, juce::Label&, const juce::String& caption, const char* paramID, const juce::String& tip);
    void setUpBar (juce::Slider&, juce::Label&, const juce::String& caption, const char* paramID, const juce::String& tip);
    void setUpCaption (juce::Label&, const juce::String& text);
    void setSoloHonk (bool on);
    void setDiff (bool on);
    void updatePresetBar();
    void showPresetMenu();
    void savePreset();
    void deletePreset();

    DeHonkAudioProcessor& deHonk;
    juce::SharedResourcePointer<Theme::Fonts> fonts;
    DeHonkLookAndFeel lookAndFeel { *fonts };
    juce::TooltipWindow tooltipWindow { this, 700 };

    DisplayComponent display;
    dehonk::scene::BandView bandView;

    juce::Label title;
    juce::TextButton previousButton { "<" }, nextButton { ">" }, presetButton, saveButton { "Save" }, deleteButton { "Delete" };
    juce::TextButton bypassButton { "Bypass" };

    juce::Slider thresholdKnob, depthKnob, maxCutKnob, sharpnessKnob, speedKnob, releaseKnob;
    juce::Label thresholdCaption, depthCaption, maxCutCaption, sharpnessCaption, speedCaption, releaseCaption;
    std::array<std::unique_ptr<ModuleButton>, dehonk::kNumModules> moduleButtons;
    ModuleButton snapButton;
    ListenButton soloButton { "Solo Honk", dehonk::toy::sunYellow, dehonk::toy::jacketRed };
    DiffButton diffButton;

    juce::Slider guardBar, linkBar, mixBar, trimBar;
    GuardLamp guardLamp;
    juce::ComboBox lookaheadBox;
    juce::Label guardCaption, linkCaption, mixCaption, trimCaption, lookaheadCaption;

    std::unique_ptr<SliderAttachment> thresholdAttachment, depthAttachment, maxCutAttachment, sharpnessAttachment, speedAttachment,
                                      releaseAttachment, guardAttachment, linkAttachment, mixAttachment, trimAttachment;
    std::array<std::unique_ptr<ButtonAttachment>, dehonk::kNumModules> moduleAttachments;
    std::unique_ptr<ButtonAttachment> snapAttachment;
    std::unique_ptr<ComboBoxAttachment> lookaheadAttachment;
    std::unique_ptr<ButtonAttachment> bypassAttachment;

    std::unique_ptr<juce::AlertWindow> dialog;

    bool showing = false;  // registered with the processor as showing; display timer running
    juce::String shownPresetText;
    double lastTimerMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeHonkAudioProcessorEditor)
};

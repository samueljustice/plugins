#pragma once

#include <JuceHeader.h>

#include "DeHonkCore.h"
#include "DeHonkLookAndFeel.h"
#include "Theme.h"

#include <array>
#include <functional>
#include <memory>

/** A small square switch with a lamp in it. Everything in the window that is switched on and off
    with a lamp uses this one shape, so a lamp always means "click me": a range, the impact guard,
    the dialogue sidechain. Off, it is an empty square; on, it has an ink dot; and while what it
    switches is at work the square fills with its glow colour (how far, 0 to 1). */
class SwitchLamp : public juce::Button
{
public:
    explicit SwitchLamp (const juce::String& name, juce::Colour glowColour = dehonk::toy::sunYellow);

    /** 0..1: how hard the thing it switches is working now. Only repaints when it changes visibly. */
    void setGlow (float amount);

    /** Draws the lamp into any rectangle (the range tabs draw their own with it). */
    static void draw (juce::Graphics&, juce::Rectangle<float> square, bool on, float glow, juce::Colour glowColour, bool hot);

protected:
    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

private:
    juce::Colour colour;
    float glow = 0.0f;
};

//==============================================================================
/** One range's tab on the shelf under the band view, in the range's own colour (the colour of its
    tree and its strip of the pond). Its lamp switches the range on and off; a click anywhere else
    on the tab selects it. It has no meter: the lamp's dot turns red while the range is cutting, and
    that is the only thing that ever repaints it. */
class RangeTab : public juce::Component,
                 public juce::SettableTooltipClient
{
public:
    RangeTab (const juce::String& name, juce::Colour colour, juce::RangedAudioParameter& onParameter);

    std::function<void()> onSelect;

    void setSelected (bool shouldBeSelected);
    bool isSelected() const noexcept { return selected; }
    bool isOn() const noexcept { return on; }
    void setCutting (bool isCutting);

    /** The tab's colour as it is drawn now (full while the range is on, pale while it is off). */
    juce::Colour fillColour() const noexcept;
    static juce::Colour fillFor (juce::Colour rangeColour, bool on) noexcept;

    /** How far an unselected tab's top sits below a selected one's. */
    static constexpr int rise = 5;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    juce::Rectangle<float> lampBounds() const noexcept;
    bool overLamp (juce::Point<float>) const noexcept;
    void toggle();

    juce::SharedResourcePointer<Theme::Fonts> fonts;
    const juce::String name;
    const juce::Colour colour;
    juce::ParameterAttachment attachment;
    bool on = false, selected = false, cutting = false, hotLamp = false, hot = false;
};

//==============================================================================
/** The shelf: the five range tabs standing on a line under the band view, and under the line a
    drawer with the selected range's own controls: a lock, and its attack and release. Locked, the
    range follows Speed and Release, and the two bars are greyed and show what those come to;
    unlocked, they are the range's own. With nothing selected the drawer says what a click does.

    Selecting is the shelf's own state, not a parameter. onSelectionChanged says which range is
    selected: 0 Woof, 1 Tub, 2 Fog, 3 Bark, 4 Snap, -1 none. */
class RangeShelf : public juce::Component
{
public:
    RangeShelf (juce::AudioProcessorValueTreeState&, const Theme::Fonts&);
    ~RangeShelf() override;

    std::function<void (int)> onSelectionChanged;

    int selection() const noexcept { return selectedRange; }
    void select (int range);

    /** Which ranges are cutting now (the four ranges, then Snap). Cheap: a tab repaints only when
        its own lamp changes. */
    void setCutting (const std::array<bool, dehonk::kNumModules + 1>&);

    /** Call a few times a second: brings the greyed bars of a locked range up to date with Speed
        and Release. */
    void refresh();

    static constexpr int tabHeight = 30, drawerHeight = 36;
    static constexpr int height = tabHeight + drawerHeight;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    class TimingBar;
    class LockButton;

    void showControlsFor (int range);
    juce::Rectangle<int> drawerBounds() const noexcept;
    double globalAttackMs() const;
    double globalReleaseMs() const;

    juce::AudioProcessorValueTreeState& apvts;
    const Theme::Fonts& fonts;
    std::array<std::unique_ptr<RangeTab>, dehonk::kNumModules + 1> tabs;
    int selectedRange = -1;

    std::unique_ptr<LockButton> lock;
    std::unique_ptr<TimingBar> attack, release;
    juce::Label attackCaption, releaseCaption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lockAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attackAttachment, releaseAttachment;
    juce::String shownAttack, shownRelease;
    bool shownLocked = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RangeShelf)
};

//==============================================================================
/** Link: one pill in three parts, Linked, Grouped and Unlinked, the chosen one filled. Grouped is
    left out while the layout is mono or stereo (it is the same as Linked there). */
class LinkSelector : public juce::Component,
                     public juce::SettableTooltipClient
{
public:
    LinkSelector (juce::RangedAudioParameter& linkMode, const Theme::Fonts&);

    void setGroupedShown (bool shouldShowGrouped);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

private:
    int numShown() const noexcept { return groupedShown ? 3 : 2; }
    int modeOfSegment (int segment) const noexcept { return groupedShown ? segment : (segment == 0 ? 0 : 2); }
    int segmentAt (juce::Point<float>) const noexcept;
    int shownSegment() const noexcept;
    void choose (int mode);

    const Theme::Fonts& fonts;
    juce::ParameterAttachment attachment;
    int mode = 0, hotSegment = -1;
    bool groupedShown = true;
};

//==============================================================================
/** The name at the top left: "de-honk", who made it, and the address of his site. A click on the
    name opens the About page; a click on the address opens the site. */
class TitleBlock : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    explicit TitleBlock (const Theme::Fonts&);

    std::function<void()> onAbout, onSite;

    static constexpr const char* siteText = "sweetjusticesound.com";
    static constexpr const char* siteUrl = "https://sweetjusticesound.com";

    juce::String getTooltip() override;
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> nameArea() const noexcept;
    juce::Rectangle<float> siteArea() const noexcept;
    int partAt (juce::Point<float>) const noexcept;   // 0 the name, 1 the address, -1 neither

    const Theme::Fonts& fonts;
    int hot = -1;
};

//==============================================================================
/** The window's own tooltip. It waits until the pointer has rested on a control for a good second,
    then grows open from the pointer with a small bounce. It is placed beside the control it
    describes and never over it (for the two big pictures, which have no "beside" inside the window,
    it keeps clear of the pointer instead). A click, a key, the wheel, or the pointer leaving the
    control closes it at once.

    It is a child of the editor, so it is always inside the plug-in's window, whatever the host does
    with other windows. */
class ToyTooltip : public juce::Component,
                   private juce::Timer
{
public:
    ToyTooltip (juce::Component& owner, const Theme::Fonts&);
    ~ToyTooltip() override;

    static constexpr int delayMs = 1200, growMs = 240;

    /** Shows the tip of `target` now, as if the pointer had rested at `at` (in the owner's
        coordinates), fully grown. For pictures and checks. */
    void showNow (juce::Component& target, juce::Point<int> at);
    void hideNow();
    bool isOpen() const noexcept { return open; }
    /** The card's rectangle in the owner's coordinates (empty while closed). */
    juce::Rectangle<int> cardBounds() const noexcept { return open ? card : juce::Rectangle<int>(); }

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    void openFor (juce::Component& target, const juce::String& text, juce::Point<int> at, bool animate);
    juce::TextLayout layoutFor (const juce::String& text) const;
    static juce::Component* tipOwner (juce::Component* under, juce::String& text);

    juce::Component& owner;
    const Theme::Fonts& fonts;
    juce::Component::SafePointer<juce::Component> restingOn, shownFor;
    juce::String restingText, shownText;
    juce::Point<int> restingAt, shownAt;
    juce::Rectangle<int> card;          // in the owner's coordinates
    juce::Point<float> origin;          // where it grows from, in the owner's coordinates
    juce::uint32 restingSince = 0, openedAt = 0;
    float grown = 1.0f;
    bool open = false, suppressed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToyTooltip)
};

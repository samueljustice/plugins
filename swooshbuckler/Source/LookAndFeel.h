#pragma once

#include <JuceHeader.h>
#include "Theme.h"

/**
 * Sweet Justice house style with a bit of salt: brass ship's-wheel knobs,
 * rope dividers, riveted panels, on a deep Caribbean sea palette.
 */
class SwooshLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit SwooshLookAndFeel(const Theme::Fonts& fonts);

    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                          float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;
    juce::Label* createSliderTextBox(juce::Slider&) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                              bool isMouseOverButton, bool isButtonDown) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool isMouseOverButton, bool isButtonDown) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool isMouseOverButton, bool isButtonDown) override;

    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                      int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getLabelFont(juce::Label&) override;
    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    void drawPopupMenuSectionHeader(juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                           const juce::Drawable* icon, const juce::Colour* textColour) override;
    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth, int& idealHeight) override;

    void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;
    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;

    /** A panel with a title plate, rivets and a rope rule under the title. */
    static void drawPanel(juce::Graphics&, juce::Rectangle<float> area, const juce::String& title,
                          const juce::String& subtitle, juce::Colour accent, const Theme::Fonts& fonts);
    /** A twisted-rope line between two points. */
    static void drawRope(juce::Graphics&, juce::Point<float> a, juce::Point<float> b, juce::Colour colour);
    /** Two crossed cutlasses (the Swooshbuckler mark). */
    static void drawCutlasses(juce::Graphics&, juce::Rectangle<float> area, juce::Colour blade, juce::Colour hilt);

private:
    const Theme::Fonts& m_fonts;
};

//==============================================================================
/**
 * A slider you can type a value into. A right-click (or an Alt-double-click) opens a small box over the control
 * showing its value; type a new one in the control's own units ("-12 dB", "4.2 s", "35%", or just the number),
 * Return keeps it and Esc (or clicking away) leaves the control as it was. A plain double-click still resets.
 * The typed value is one undo step, like a drag.
 *
 * The owner says which parameter the control is attached to (setTypedParameter); what is typed is matched against
 * that parameter's own readout (see TypedValue), so every law works without a formula of its own.
 * A control with a menu of its own (onContextMenu) shows that on a right-click, and offers typing in it.
 */
class ValueSlider : public juce::Slider
{
public:
    ValueSlider();
    ~ValueSlider() override;

    void setTypedParameter(juce::RangedAudioParameter* parameter, const Theme::Fonts& fonts);
    juce::RangedAudioParameter* getTypedParameter() const { return m_typedParameter; }
    /** Opens the box (nothing happens without a parameter). */
    void beginTyping();
    bool isTyping() const;
    /** Called just before a typed value is set (a glide towards a preset is stopped here). */
    std::function<void()> onTypedValue;
    /** A right-click opens this instead of the box. */
    std::function<void()> onContextMenu;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void resized() override;
    void moved() override;

    /** The narrowest the box may be: a control narrower than this (a vertical fader) gets a box wider than itself. */
    int minTypingWidth = 0;

protected:
    /** Where the box sits, in this control's coordinates: over the control (across the middle of a tall one)
        unless a subclass says otherwise. */
    virtual juce::Rectangle<int> typingBounds() const;

private:
    void placeTyping();
    void endTyping(bool keep, bool takeKeysBack);
    juce::RangedAudioParameter* m_typedParameter = nullptr;
    const Theme::Fonts* m_typedFonts = nullptr;
    std::unique_ptr<juce::TextEditor> m_typing;
    bool m_swallowMouse = false; // this click opened the box or a menu: the slider never sees it
};

//==============================================================================
/**
 * A control a modulator wire can land on: the knobs, and each track's Semitones box. The wire overlay (ModWires)
 * draws the cables and the plugs; the control shows each wire's reach (an arc, dashed when the depth is negative)
 * and where the modulation has it now, and a Cmd-drag on it (Ctrl on Windows) sets its newest wire's depth, up or
 * down through zero, without hunting for the plug.
 */
class ModTarget : public ValueSlider
{
public:
    /** One wire: its colour, its depth (-1..1, normalised) and whether it swings both ways. */
    struct ModArc
    {
        juce::Colour colour;
        float depth = 0.0f;   // -1..1 (normalised)
        bool bipolar = true;  // swings either side of the value (LFO / Random) or one way (Follower / Macro)
    };
    void setModArcs(std::vector<ModArc> arcs)
    {
        auto same = [](const ModArc& a, const ModArc& b) { return a.colour == b.colour && std::abs(a.depth - b.depth) < 1.0e-4f && a.bipolar == b.bipolar; };
        if (arcs.size() == m_modArcs.size() && std::equal(arcs.begin(), arcs.end(), m_modArcs.begin(), same))
            return;
        m_modArcs = std::move(arcs);
        if (m_modArcs.empty()) m_modLive = -1.0f;
        repaint();
    }
    void setModLive(float normalised);
    bool isModulated() const { return ! m_modArcs.empty(); }
    /** Where a wire's plug sits around (and how big the target is), in this control's coordinates. */
    virtual juce::Point<float> getDialCentre() const = 0;
    virtual float getDialRadius() const = 0;
    /** Where the index-th wire's plug sits, in this control's coordinates: around a knob's rim by default. */
    virtual juce::Point<float> getPlugPosition(int index) const
    {
        const float a = juce::degreesToRadians(38.0f + 24.0f * static_cast<float>(index));
        return getDialCentre() + juce::Point<float>(std::sin(a), -std::cos(a)) * (getDialRadius() + 5.0f);
    }
    /** A wire is being dragged over this control. */
    void setWireHover(juce::Colour c) { m_wireHover = c; repaint(); }
    /** Can a wire land on (and a plug be shown at) this control now: showing, enabled, not scrolled out of view. */
    virtual bool isReachable() const { return isShowing() && isEnabled(); }

    /** Its name, as wires describe it ("MOD 1 -> Doppler"); a knob also draws it under the dial. */
    juce::String caption;

    /** Cmd-drag (Ctrl on Windows) on a modulated control: phase 0 = start, 1 = drag, 2 = end, 3 = Cmd-double-click
        (+50%). Set by ModWires. */
    std::function<void(const juce::MouseEvent&, int phase)> onModDepthGesture;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override; // Cmd-double-click: the newest wire back to +50%

protected:
    std::vector<ModArc> m_modArcs;
    float m_modLive = -1.0f;
    juce::Colour m_wireHover = juce::Colours::transparentBlack;
    bool m_modGesture = false;
    /** Strokes p in colour, dashed when the wire's depth is negative (so the sign shows at a glance). */
    static void strokeModPath(juce::Graphics&, const juce::Path& p, juce::Colour colour, float depth, float thickness);
};

/**
 * Rotary knob drawn as a brass ship's wheel. The wheel chases the real value
 * on a damped spring, so every move (and every preset morph) swings in with a
 * little rubber-band overshoot.
 */
class WheelKnob : public ModTarget, private juce::Timer
{
public:
    WheelKnob(const Theme::Fonts& fonts, juce::Colour accent);

    void setAccent(juce::Colour c) { m_accent = c; repaint(); }
    void paint(juce::Graphics&) override;
    void valueChanged() override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;

    /** The dial (without the caption), in this knob's coordinates. */
    juce::Point<float> getDialCentre() const override;
    float getDialRadius() const override;

protected:
    juce::Rectangle<int> typingBounds() const override; // over the caption, under the dial

private:
    void timerCallback() override;

    const Theme::Fonts& m_fonts;
    juce::Colour m_accent;
    float m_shown = 0.0f, m_velocity = 0.0f;
    bool m_hover = false;
};

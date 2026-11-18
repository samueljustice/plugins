#pragma once

#include <JuceHeader.h>

#include <optional>
#include "Theme.h"

/**
 * Roarschach's LookAndFeel (the house style shared with its sibling plug-ins): the editor and everything it opens (popup menus and their submenus, ComboBox
 * popups, TextEditor right-click menus, alerts, its tooltip window, the knob value boxes, the corner
 * resizer and the About window) are drawn with one instance of this class, owned by the editor.
 *
 * Fonts: every font comes from Theme::Fonts' roles. JUCE builds several of its own fonts without a
 * typeface (menus, alerts, tooltips, V4 buttons), which resolve to the system sans-serif whatever the
 * LookAndFeel's default typeface is, so each of those hooks is overridden here.
 *
 * Lifetime: menus, alerts, tooltip windows and the About window keep a (weak) reference to the LookAndFeel
 * they are drawn with. The owner closes them before this object goes (PopupMenu::dismissAllActiveMenus(),
 * resetting the alerts and the About window, setLookAndFeel(nullptr)).
 *
 * The rules it implements: ink on paper (Theme's roles, the mood's dye as the one accent), strokes instead of
 * glyph icons, state shown by shape as well as tone, popups no smaller than at the editor's default size.
 * Colours are read from the live palette whenever something is drawn; refreshColours() takes a changed
 * palette into the colour ids JUCE's own drawing reads.
 */
class RoarschachLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit RoarschachLookAndFeel(const Theme::Fonts& fonts);

    /** Takes the palette's current (mood-toned) roles into the colour ids. Call after Theme::applyMood(). */
    void refreshColours();

    /** How a knob is drawn (a property on the slider): in use, doing little, not used by the current
        settings, or its module is Off. The last two are drawn as outlines, their values in the faint ink. */
    enum class KnobStyle { full = 0, weak = 1, unused = 2, off = 3 };
    static void setKnobStyle(juce::Slider&, KnobStyle);
    static KnobStyle getKnobStyle(const juce::Slider&);

    /** A caption whose ink follows the palette by rank: dim while its control is in use, the faint ink
        when it is not (or its module is Off). */
    static void setCaptionLive(juce::Label&, bool live);
    static bool isCaptionLive(const juce::Label&);

    /** A chip that opens a menu (a mode chip): drawn with a caret at its right. `off`: its module is Off, so
        the chip is drawn without its fill and its label in the faint ink (still readable, still clickable). */
    static void setMenuChip(juce::Button&, bool off = false);
    /** 0: not a menu chip, 1: a menu chip, 2: ... of an Off module. */
    static int getMenuChip(const juce::Button&);

    /** A switch (IN's and OUTPUT's AUTO): an ink outline with a lamp; it reads its own name, or ON and OFF
        where a caption names it (wordsOnOff). */
    static void setSwitch(juce::Button&, bool wordsOnOff = false);
    /** Draws a switch smaller inside its hit target, with a smaller lamp and label (IN's AUTO, which sits
        between the knob's value and the meter's scale). */
    static void setCompact(juce::Button&);
    /** A tag (SAVE): an outlined ink tag, no raised fill, filled with the dyed ink while toggled on. */
    static void setTag(juce::Button&);
    /** A glyph button drawn in an outlined ink circle (the header's ?). */
    static void setCircled(juce::Button&);
    /** A header button drawn as a quiet glyph: no chip until the mouse is over it. */
    static void setQuiet(juce::Button&);
    /** A text button whose label is set in the display face (the preset name). */
    static void setDisplayFace(juce::Button&);

    /** A brushed value arc: from `from` to `to` (radians), swelling from a fine start to a round end. */
    static void strokeBrush(juce::Graphics&, juce::Point<float> centre, float radius, float from, float to, float width);

    //==============================================================================
    /** Row height of a menu at design size; every other menu metric follows it. */
    static constexpr int c_menuItemHeight = 24;

    /** Options for a menu dropping from `target` (a control in the editor's scaled content). The menu follows
        the editor's scale like the control does, but never gets smaller than at the editor's default size. */
    static juce::PopupMenu::Options menuOptionsFor(juce::Component& target, int minimumWidth = 0);

    /** Keyboard focus rings are shown in a window once Tab has moved the focus there, and hidden again by the
        next click in it. Call once for each window drawn with this LookAndFeel (editor, alert, About window). */
    static void showFocusRingsAfterTab(juce::Component& window);
    /** True when `c` has the keyboard focus and should show it (see showFocusRingsAfterTab()). */
    static bool shouldShowFocusRing(const juce::Component& c);

    /** Draws a button's label in capitals, like the editor's buttons. Its text (and accessible name) is kept. */
    static void setLabelDrawnInCapitals(juce::Button&);

    /** The accent of a dialog of this kind: its Return-key button (the warning ink for a warning). */
    static juce::Colour accentFor(juce::MessageBoxIconType);

    /** The room a menu chip of this width gives its label (between its left end and its caret's zone). */
    static float menuChipTextWidth(int buttonWidth);

    /** Lays a dialog out as the house does (after JUCE's layout): fields from the title's left edge, the
        buttons at the right, the primary one rightmost. */
    static void arrangeDialog(juce::AlertWindow&);

    /** The Roarschach mark: a small mirror-symmetric ink-blot maw, horned, with an open jaw, fangs top and
        bottom and slit eyes, designed at 30 x 22 and scaled to the area. drawMark is one ink with nothing
        behind it (the editor header); drawColourMark is the mark in colour (the About window only). */
    static void drawMark(juce::Graphics&, juce::Rectangle<float> area);
    static void drawColourMark(juce::Graphics&, juce::Rectangle<float> area);

    /** A section on the plain sheet (the About window's): no card, its title in the display face and a
        hairline running on from it to ruleEnd (the section's right edge when 0), as a menu's section header.
        Returns its header row. */
    juce::Rectangle<float> drawPanel(juce::Graphics&, juce::Rectangle<float> bounds, const juce::String& title,
                                     float headerHeight = 32.0f, float ruleEnd = 0.0f) const;

    const Theme::Fonts& getFonts() const noexcept { return m_fonts; }

    //==============================================================================
    // Knobs and their value boxes
    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                          float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;
    juce::Label* createSliderTextBox(juce::Slider&) override;

    /** A knob whose value box uses another size than Theme::Type::value (a point height). */
    static void setValueFontHeight(juce::Slider&, float pointHeight);

    // Labels (a knob's value box is drawn here too: ink, or the faint ink for an unused or Off knob)
    juce::Font getLabelFont(juce::Label&) override;
    void drawLabel(juce::Graphics&, juce::Label&) override;

    // Toggle buttons: an On lamp (the chain tiles' switches). No tick boxes anywhere: every ToggleButton is
    // drawn as a lamp, filled when on and hollow when off, with its text (if any) beside it.
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool isMouseOverButton, bool isButtonDown) override;
    static void drawLed(juce::Graphics&, juce::Rectangle<float> area, bool on, bool hot, bool focused);

    // Combo boxes and their popups
    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                      int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu(juce::ComboBox&, juce::Label&) override;

    // Buttons (text buttons, link-style text buttons with a transparent fill)
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                              bool isMouseOverButton, bool isButtonDown) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool isMouseOverButton, bool isButtonDown) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;

    // Popup menus
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackgroundWithOptions(juce::Graphics&, int width, int height, const juce::PopupMenu::Options&) override;
    void drawPopupMenuItemWithOptions(juce::Graphics&, const juce::Rectangle<int>& area, bool isHighlighted,
                                      const juce::PopupMenu::Item&, const juce::PopupMenu::Options&) override;
    void drawPopupMenuSectionHeaderWithOptions(juce::Graphics&, const juce::Rectangle<int>& area,
                                               const juce::String& sectionName, const juce::PopupMenu::Options&) override;
    void getIdealPopupMenuItemSizeWithOptions(const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                              int& idealWidth, int& idealHeight, const juce::PopupMenu::Options&) override;
    void getIdealPopupMenuSectionHeaderSizeWithOptions(const juce::String& text, int standardMenuItemHeight,
                                                       int& idealWidth, int& idealHeight, const juce::PopupMenu::Options&) override;
    int getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options&) override;
    void drawPopupMenuUpDownArrowWithOptions(juce::Graphics&, int width, int height, bool isScrollUpArrow,
                                             const juce::PopupMenu::Options&) override;
    int getPopupMenuColumnSeparatorWidthWithOptions(const juce::PopupMenu::Options&) override;
    void drawPopupMenuColumnSeparatorWithOptions(juce::Graphics&, const juce::Rectangle<int>&, const juce::PopupMenu::Options&) override;

    // Alerts
    juce::AlertWindow* createAlertWindow(const juce::String& title, const juce::String& message,
                                         const juce::String& button1, const juce::String& button2, const juce::String& button3,
                                         juce::MessageBoxIconType, int numButtons, juce::Component* associatedComponent) override;
    void drawAlertBox(juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>& textArea, juce::TextLayout&) override;
    int getAlertWindowButtonHeight() override;
    juce::Array<int> getWidthsForTextButtons(juce::AlertWindow&, const juce::Array<juce::TextButton*>&) override;
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;

    // Text fields: alert name field, knob value box while editing, About licence text
    void fillTextEditorBackground(juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;

    // Scrollbars (the licence text, any long list)
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                       int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;

    // Tooltips
    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                          juce::Rectangle<int> parentArea) override;
    void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;

    // A menu shown inside a component (withParentComponent) would get a V4 frame line inside its border: none.
    void drawResizableFrame(juce::Graphics&, int, int, const juce::BorderSize<int>&) override {}

    // The resizer AudioProcessorEditor::setResizable(true, true) puts in the bottom-right corner
    void drawCornerResizer(juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

    // Window shadows drawn by JUCE (Windows), and focus outlines of components that ask for one
    std::unique_ptr<juce::DropShadower> createDropShadowerForComponent(juce::Component&) override;
    std::unique_ptr<juce::FocusOutline> createFocusOutlineForComponent(juce::Component&) override;

private:
    juce::Font menuFont(float scale) const;
    juce::TextLayout layoutTooltip(const juce::String& text, juce::Colour colour) const;
    static bool isPrimaryDialogButton(const juce::Button&);

    const Theme::Fonts& m_fonts;

    JUCE_DECLARE_NON_COPYABLE(RoarschachLookAndFeel)
};

//==============================================================================
/**
 * A HyperlinkButton in the theme's link style. juce::HyperlinkButton draws its hover and pressed states as
 * textColourId.darker(0.4 / 1.3) and has no LookAndFeel hook; this one draws the idle colour (textColourId),
 * the dyed ink under the mouse, and an underline on hover and for keyboard focus.
 */
class ThemeLink : public juce::HyperlinkButton
{
public:
    ThemeLink(const juce::String& linkText, const juce::URL& linkUrl);

    /** Hides HyperlinkButton::setFont (whose font is private) so the link draws with the same font. */
    void setFont(const juce::Font& newFont, bool resizeToMatchComponentHeight,
                 juce::Justification newJustification = juce::Justification::horizontallyCentred);

    /** Draws the text with its baseline's left end at this point (local, fractional), so it can sit
        exactly on another line's baseline whatever whole-pixel bounds the button has. */
    void setTextOrigin(juce::Point<float> baselineStart) { m_origin = baselineStart; repaint(); }

protected:
    void paintButton(juce::Graphics&, bool isMouseOverButton, bool isButtonDown) override;

private:
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    juce::Font m_font { m_fonts->caption(Theme::Type::credit) };
    juce::Justification m_justification { juce::Justification::horizontallyCentred };
    std::optional<juce::Point<float>> m_origin;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ThemeLink)
};

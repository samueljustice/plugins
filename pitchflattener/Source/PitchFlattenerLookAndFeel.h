#pragma once

#include <JuceHeader.h>
#include "Theme.h"

/**
 * Pitch Flattener's LookAndFeel (the same design system as Subbertone's SubbertoneLookAndFeel): the editor and
 * everything it opens (ComboBox popups, TextEditor right-click menus, the preset dialogs, its tooltip window,
 * the knob value boxes, the corner resizer and the About window) are drawn with one instance of this class,
 * owned by the editor.
 *
 * Fonts: every font comes from Theme::Fonts (Azeret Mono). JUCE builds several of its own fonts without a
 * typeface (menus, alerts, tooltips, V4 buttons), which resolve to the system sans-serif whatever the
 * LookAndFeel's default typeface is, so each of those hooks is overridden here.
 *
 * Accents keep Theme's meanings where a surface belongs to a section (a ComboBox popup takes the colour of its
 * box). Chrome that belongs to no section (focus rings, text selection and caret, links, the fallback menu
 * accent) is amber, the colour of the header mark's flat line and the version, as in Subbertone. Dialogs:
 * a question (replace, reset) or a plain prompt (save) is amber, information teal, a warning (delete, errors)
 * rose.
 *
 * Lifetime: menus, alerts, tooltip windows and the About window keep a (weak) reference to the LookAndFeel
 * they are drawn with. The owner closes them before this object goes (PopupMenu::dismissAllActiveMenus(),
 * resetting the alerts and the About window, setLookAndFeel (nullptr)).
 */
class PitchFlattenerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit PitchFlattenerLookAndFeel (const Theme::Fonts& fonts);

    //==============================================================================
    /** Row height of a menu at design size; every other menu metric follows it. */
    static constexpr int c_menuItemHeight = 24;

    /** Options for a menu dropping from `target` (a control in the editor's scaled content). The menu follows
        the editor's scale like the control does, but never gets smaller than at the editor's default size. */
    static juce::PopupMenu::Options menuOptionsFor (juce::Component& target, int minimumWidth = 0);

    /** Keyboard focus rings are shown in a window once Tab has moved the focus there, and hidden again by the
        next click in it. Call once for each window drawn with this LookAndFeel (editor, alert, About window). */
    static void showFocusRingsAfterTab (juce::Component& window);
    /** True when `c` has the keyboard focus and should show it (see showFocusRingsAfterTab()). */
    static bool shouldShowFocusRing (const juce::Component& c);

    /** Draws a button's label in capitals, like the editor's buttons. Its text (and accessible name) is kept. */
    static void setLabelDrawnInCapitals (juce::Button&);

    /** The accent of a dialog of this kind: its header dot and its Return-key button. */
    static juce::Colour accentFor (juce::MessageBoxIconType);

    /** The Pitch Flattener mark: a wobbling teal line that settles into a flat amber line (editor header,
        designed at 34 x 22 and scaled to the area). */
    static void drawMark (juce::Graphics&, juce::Rectangle<float> area);

    /** A panel like the editor's sections (fill, outline, accent dot, title, divider); returns its header row. */
    juce::Rectangle<float> drawPanel (juce::Graphics&, juce::Rectangle<float> bounds, const juce::String& title,
                                      juce::Colour accent, float headerHeight = 32.0f) const;

    const Theme::Fonts& getFonts() const noexcept { return m_fonts; }

    //==============================================================================
    // Knobs and their value boxes
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    juce::Slider::SliderLayout getSliderLayout (juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;

    // Labels
    juce::Font getLabelFont (juce::Label&) override;

    // Combo boxes and their popups
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu (juce::ComboBox&, juce::Label&) override;

    // Buttons (text buttons, link-style text buttons with a transparent fill, the pill switch)
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isMouseOverButton, bool isButtonDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool isMouseOverButton, bool isButtonDown) override;

    // Popup menus
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackgroundWithOptions (juce::Graphics&, int width, int height, const juce::PopupMenu::Options&) override;
    void drawPopupMenuItemWithOptions (juce::Graphics&, const juce::Rectangle<int>& area, bool isHighlighted,
                                       const juce::PopupMenu::Item&, const juce::PopupMenu::Options&) override;
    void drawPopupMenuSectionHeaderWithOptions (juce::Graphics&, const juce::Rectangle<int>& area,
                                                const juce::String& sectionName, const juce::PopupMenu::Options&) override;
    void getIdealPopupMenuItemSizeWithOptions (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                               int& idealWidth, int& idealHeight, const juce::PopupMenu::Options&) override;
    void getIdealPopupMenuSectionHeaderSizeWithOptions (const juce::String& text, int standardMenuItemHeight,
                                                        int& idealWidth, int& idealHeight, const juce::PopupMenu::Options&) override;
    int getPopupMenuBorderSizeWithOptions (const juce::PopupMenu::Options&) override;
    void drawPopupMenuUpDownArrowWithOptions (juce::Graphics&, int width, int height, bool isScrollUpArrow,
                                              const juce::PopupMenu::Options&) override;
    int getPopupMenuColumnSeparatorWidthWithOptions (const juce::PopupMenu::Options&) override;
    void drawPopupMenuColumnSeparatorWithOptions (juce::Graphics&, const juce::Rectangle<int>&, const juce::PopupMenu::Options&) override;

    // Alerts
    juce::AlertWindow* createAlertWindow (const juce::String& title, const juce::String& message,
                                          const juce::String& button1, const juce::String& button2, const juce::String& button3,
                                          juce::MessageBoxIconType, int numButtons, juce::Component* associatedComponent) override;
    void drawAlertBox (juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>& textArea, juce::TextLayout&) override;
    int getAlertWindowButtonHeight() override;
    juce::Array<int> getWidthsForTextButtons (juce::AlertWindow&, const juce::Array<juce::TextButton*>&) override;
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;

    // Text fields: the save dialog's name field, a knob value box while editing, the About credits text
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    // Scrollbars (the About credits, a long preset list)
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                        int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;

    // Tooltips
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    // The resizer AudioProcessorEditor::setResizable (true, true) puts in the bottom-right corner
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

    // Window shadows drawn by JUCE (Windows), and focus outlines of components that ask for one
    std::unique_ptr<juce::DropShadower> createDropShadowerForComponent (juce::Component&) override;
    std::unique_ptr<juce::FocusOutline> createFocusOutlineForComponent (juce::Component&) override;

private:
    juce::Font menuFont (float scale) const;
    juce::TextLayout layoutTooltip (const juce::String& text, juce::Colour colour) const;
    static bool isPrimaryDialogButton (const juce::Button&);

    const Theme::Fonts& m_fonts;

    JUCE_DECLARE_NON_COPYABLE (PitchFlattenerLookAndFeel)
};

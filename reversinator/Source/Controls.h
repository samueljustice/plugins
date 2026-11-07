#pragma once

#include <JuceHeader.h>
#include "Theme.h"

namespace Controls
{
    /** Slider looks, chosen per slider with setStyle(). */
    enum class SliderStyle { Hourglass, Loop, Fader };

    void setStyle(juce::Slider& slider, SliderStyle style);

    /**
     * One LookAndFeel for the editor and everything it opens: the three vertical
     * slider styles (hourglass, feedback loop, mix fader) and their value boxes,
     * buttons and text links, popup menus and their submenus (the text fields'
     * right-click menus), text fields, scrollbars, tooltips, the corner resizer,
     * window shadows and keyboard focus rings.
     *
     * It follows the house style of Subbertone's LookAndFeel
     * (subbertone/Source/SubbertoneLookAndFeel.cpp): the same metrics, shapes and
     * rules, in Reversinator's palette, with the logo arrow's blue as the accent
     * where Subbertone uses its amber. Every font is set explicitly: JUCE builds
     * some of its own (menus, tooltips) without a typeface, and those would come
     * out in the system font. Anything left to JUCE draws in the theme's colours.
     */
    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        explicit LookAndFeel(const Theme::Fonts& fonts);

        /** Row height of a menu at design size; every other menu metric follows it. */
        static constexpr int menuItemHeight = 24;

        /** Keyboard focus rings are shown in a window once Tab has moved the focus there, and hidden
            again by the next click in it. Call once for each window drawn with this LookAndFeel. */
        static void showFocusRingsAfterTab(juce::Component& window);
        /** True when `c` has the keyboard focus and should show it (see showFocusRingsAfterTab()). */
        static bool shouldShowFocusRing(const juce::Component& c);
        /** Draws a button's label in capitals, like the editor's other chrome. Its text is kept. */
        static void setLabelDrawnInCapitals(juce::Button&);

        // Sliders and their value boxes
        void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                              float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
        int getSliderThumbRadius(juce::Slider&) override;
        juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;
        juce::Label* createSliderTextBox(juce::Slider&) override;
        juce::Font getLabelFont(juce::Label&) override;

        // Buttons (pills, and text links: text buttons with a transparent fill)
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

        // Text fields: a value box while it is typed into, the About licence text
        void fillTextEditorBackground(juce::Graphics&, int width, int height, juce::TextEditor&) override;
        void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;

        // Scrollbars (the licence text)
        void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                           int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;

        // Tooltips
        juce::Rectangle<int> getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                              juce::Rectangle<int> parentArea) override;
        void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;

        // The resizer AudioProcessorEditor::setResizable(true, true) puts in the bottom-right corner
        void drawCornerResizer(juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

        // Window shadows drawn by JUCE (Windows), and focus outlines of components that ask for one
        std::unique_ptr<juce::DropShadower> createDropShadowerForComponent(juce::Component&) override;
        std::unique_ptr<juce::FocusOutline> createFocusOutlineForComponent(juce::Component&) override;

    private:
        juce::Font menuFont(float scale) const;
        juce::TextLayout layoutTooltip(const juce::String& text, juce::Colour colour) const;

        void drawHourglass(juce::Graphics&, juce::Rectangle<float> area, float sliderPos, float minPos, float maxPos, juce::Slider&);
        void drawLoop(juce::Graphics&, juce::Rectangle<float> area, float sliderPos, float minPos, float maxPos, juce::Slider&);
        void drawFader(juce::Graphics&, juce::Rectangle<float> area, float sliderPos, float minPos, float maxPos, juce::Slider&);
        void drawThumb(juce::Graphics&, juce::Point<float> centre, float width, juce::Colour accent, bool hot);

        const Theme::Fonts& m_fonts;
    };

    /** The three effect lamps; a radio group bound to the "mode" choice parameter. */
    class ModeSelector : public juce::Component
    {
    public:
        ModeSelector(juce::RangedAudioParameter& modeParameter, const Theme::Fonts& fonts);

        void resized() override;

    private:
        class Lamp : public juce::Button
        {
        public:
            Lamp(const juce::String& title, const juce::String& subtitle, const Theme::Fonts& fonts);
            void paintButton(juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

        private:
            juce::String m_title, m_subtitle;
            const Theme::Fonts& m_fonts;
        };

        void select(int index);

        std::array<std::unique_ptr<Lamp>, 3> m_lamps;
        juce::ParameterAttachment m_attachment;
    };

    /** Big round REVERSER button: a left-pointing arrow while reversing, right-pointing when not. */
    class ReverserButton : public juce::Button
    {
    public:
        ReverserButton();
        void paintButton(juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
    };

    /** Draws text with some letters mirrored left-to-right in place (the "REVƎRSƎЯ" label). */
    void drawMirroredText(juce::Graphics&, const juce::String& text, const juce::Font& font,
                          juce::Rectangle<float> area, const juce::Array<int>& mirroredIndices);
}

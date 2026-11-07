#pragma once

#include <JuceHeader.h>
#include "Theme.h"

namespace Controls
{
    /** Slider looks, chosen per slider with setStyle(). */
    enum class SliderStyle { Hourglass, Loop, Fader };

    void setStyle(juce::Slider& slider, SliderStyle style);

    /**
     * One LookAndFeel for the whole editor. Draws the three vertical slider
     * styles (hourglass, feedback loop, mix fader), their value boxes, buttons
     * and tooltips in the Reversinator palette.
     */
    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        explicit LookAndFeel(const Theme::Fonts& fonts);

        void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                              float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
        int getSliderThumbRadius(juce::Slider&) override;
        juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;
        juce::Label* createSliderTextBox(juce::Slider&) override;
        juce::Font getLabelFont(juce::Label&) override;

        void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                  bool isMouseOverButton, bool isButtonDown) override;
        juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;

        juce::Rectangle<int> getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                              juce::Rectangle<int> parentArea) override;
        void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;

    private:
        juce::TextLayout layoutTooltip(const juce::String& text) const;

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

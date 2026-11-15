#pragma once

#include <JuceHeader.h>

#include "Hud.h"

/**
 * The editor's buttons, drawn in the HUD style: chamfered, 1.5 px edges, sheared bold labels.
 *
 *   Action   an outlined command (CLEAR, ?); its accent edge, amber when hot.
 *   Segment  one of a set (TARGET, LINE): dim outline when off, filled in its accent when on.
 *   Tab      a small view switch at the display's corner (SCOPE, FLIGHT), filled when on.
 */
class HudButton : public juce::Button
{
public:
    enum class Style { Action, Segment, Tab };

    HudButton (const juce::String& label, Style, juce::Colour accent = Theme::Colours::orange);

protected:
    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

private:
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    Style m_style;
    juce::Colour m_accent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HudButton)
};

/**
 * LEARN: the state word large, a hint below it small (LEARN / PLAY THE SOUND ONCE, LISTENING /
 * 2.4 S ...), coloured by what is happening (lime armed, magenta listening, cyan analysing,
 * orange otherwise). While nothing is learned it pulses lime: it is the next thing to press.
 */
class LearnButton final : public juce::Button
{
public:
    explicit LearnButton (const Hud::Clock&);

    void setContent (const juce::String& word, const juce::String& hint, juce::Colour accent, bool filled, bool pulsing);
    bool isPulsing() const noexcept { return m_pulsing; }
    const juce::String& hint() const noexcept { return m_hint; }

protected:
    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

private:
    const Hud::Clock& m_clock;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    juce::String m_word { "LEARN" }, m_hint;
    juce::Colour m_accent = Theme::Colours::orange;
    bool m_filled = false, m_pulsing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LearnButton)
};

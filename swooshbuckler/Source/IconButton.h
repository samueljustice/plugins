#pragma once

#include <JuceHeader.h>
#include "Icons.h"
#include "Theme.h"

/**
 * A small button with a drawn icon instead of text (undo, redo, the roll history, keep, the section dice and
 * locks, a lane's remove, the preset arrows). It uses the same plate as the text buttons; a toggled one lights
 * in its accent colour, and a quiet one has no plate until the mouse is over it.
 */
class IconButton : public juce::Button
{
public:
    using Icon = Icons::Icon;

    IconButton(Icon icon, juce::Colour accent, bool quiet = false) : juce::Button({}), m_icon(icon), m_quiet(quiet)
    {
        setColour(juce::TextButton::buttonOnColourId, accent);
        setWantsKeyboardFocus(false);
    }

    /** The tooltip's heading line is also the button's name for screen readers (it has no text of its own). */
    void setTooltip(const juce::String& tip) override
    {
        juce::Button::setTooltip(tip);
        setTitle(tip.upToFirstOccurrenceOf("\n", false, false));
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        namespace TC = Theme::Colours;
        const bool on = getToggleState();
        if (! m_quiet || over || down || on)
            getLookAndFeel().drawButtonBackground(g, *this, findColour(juce::TextButton::buttonColourId), over, down);
        const auto accent = findColour(juce::TextButton::buttonOnColourId);
        const auto colour = ! isEnabled() ? TC::textFaint.withAlpha(0.45f) : (on ? accent.brighter(0.4f) : (over ? TC::text : TC::textDim));
        const float side = juce::jmin(static_cast<float>(getWidth()), static_cast<float>(getHeight())) - 6.0f;
        Icons::draw(g, m_icon, juce::Rectangle<float>(side, side).withCentre(getLocalBounds().toFloat().getCentre()), colour, on || (down && m_icon == Icon::Star));
    }

private:
    Icon m_icon;
    bool m_quiet;
};

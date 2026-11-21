#pragma once

#include <JuceHeader.h>
#include "LookAndFeel.h"
#include "Shortcuts.h"
#include "Theme.h"

/**
 * Tooltips in two tiers.
 *
 * Rest on a control and the first thing it says is short: its name, and what it is set to now out of what it can
 * be (or its key, for a button). Stay a moment longer and the tip grows into the whole explanation, with the way
 * to handle it and its key at the end, dimmer.
 *
 * The how-to line that is the same on every knob ("Double-click to reset it...") is not written forty times in
 * the knobs' own tips: it is added here, in the second tier only, from what the control is. A control handled
 * differently says so in its "howTo" property, which is used instead: the notched wheels ("Double-click to open it.
 * Shift-double-click to reset it..."), Swell, the layers' own wheels.
 *
 * Message thread only, like everything to do with painting.
 */
class SwooshTooltips final : public juce::TooltipWindow
{
public:
    /** The first tier appears after c_shortMs of rest, the second c_growMs after that. */
    static constexpr int c_shortMs = 500, c_growMs = 1100;

    SwooshTooltips(juce::Component* parent, const Shortcuts::Registry& keys)
        : juce::TooltipWindow(parent, c_shortMs), m_keys(keys)
    {
    }

    juce::String getTipFor(juce::Component& c) override
    {
        const auto full = juce::TooltipWindow::getTipFor(c);
        if (full.isEmpty())
        {
            m_over = nullptr;
            return {};
        }
        const auto now = juce::Time::getMillisecondCounter();
        if (&c != m_over)
        {
            m_over = &c;
            m_since = now;
        }
        const bool grown = now > m_since + static_cast<unsigned int>(c_growMs);
        return grown ? whole(c, full) : summary(c, full);
    }

private:
    /** The key that does the same thing as this control, written for the user, or empty. */
    juce::String keyFor(const juce::Component& c) const
    {
        const auto action = Shortcuts::tagged(c);
        return action == Shortcuts::Action::Count ? juce::String() : m_keys.keyText(action);
    }

    /** Tier one: the name, then what it is set to now (or its key). A control that rests says why at once: the
        line of its tip that starts "It rests" comes with the first tier, not only with the whole tip. */
    juce::String summary(juce::Component& c, const juce::String& full) const
    {
        const auto first = summaryLine(c, full);
        if (Theme::getLook(c) != Theme::Look::Resting)
            return first;
        for (const auto& line : juce::StringArray::fromLines(full))
            if (line.startsWith("It rests") && ! first.contains(line))
                return first + "\n" + line;
        return first;
    }

    juce::String summaryLine(juce::Component& c, const juce::String& full) const
    {
        const auto heading = full.upToFirstOccurrenceOf("\n", false, false).trim();
        juce::String line;
        // A control that names its own state in words (Direction: "Left to Right").
        if (const auto own = c.getProperties()["tipLine"].toString(); own.isNotEmpty())
            line = own;
        else if (auto* slider = dynamic_cast<juce::Slider*>(&c))
        {
            const auto value = slider->getTextFromValue(slider->getValue());
            const auto low = slider->getTextFromValue(slider->getMinimum());
            const auto high = slider->getTextFromValue(slider->getMaximum());
            line = low == high ? value : value + "   (" + low + " to " + high + ")";
        }
        else if (auto* box = dynamic_cast<juce::ComboBox*>(&c))
        {
            line = box->getText();
        }
        if (line.isEmpty())
            line = keyFor(c);
        return line.isEmpty() ? full : heading + "\n" + line;
    }

    /** Tier two: the whole thing, with the shared how-to and the key after it. */
    juce::String whole(juce::Component& c, const juce::String& full) const
    {
        juce::String last;
        if (const auto howTo = c.getProperties()["howTo"].toString(); howTo.isNotEmpty())
            last = howTo;
        else if (dynamic_cast<ModTarget*>(&c) != nullptr)
            last = "Double-click to reset it. Right-click (or Alt-double-click) to type a value. Drag a Mod jack onto it to modulate it.";
        else if (auto* typed = dynamic_cast<ValueSlider*>(&c); typed != nullptr && typed->getTypedParameter() != nullptr)
            last = "Double-click to reset it. Right-click (or Alt-double-click) to type a value.";
        else if (dynamic_cast<juce::Slider*>(&c) != nullptr)
            last = "Double-click to reset it.";
        if (const auto key = keyFor(c); key.isNotEmpty())
            last = last.isEmpty() ? "Key: " + key : last + "  Key: " + key;
        if (last.isEmpty())
            return full;
        // The third line is the dim one: a tip with only a heading and a body gets the how-to as its third.
        return full + "\n" + last;
    }

    const Shortcuts::Registry& m_keys;
    const juce::Component* m_over = nullptr; // compared only, never followed
    unsigned int m_since = 0;
};

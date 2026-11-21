#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"

/**
 * THE VARISPEED OFFSET METER: how far a Tape Flanger's second machine has been leant away from the first.
 *
 * 56 x 10 under the Varispeed wheel (design 2.5): a slot with a line along it, a mark at its centre and a block
 * that sits where the held offset is (SwooshbucklerAudioProcessor::getVarispeedOffset(bus), -1 .. 1 of the travel
 * either side: left is behind, right is ahead). The block is in the bus's colour while there is an offset and dim
 * when it is home. Double-click slides it home (recentreVarispeed(bus)). Hard edges only.
 *
 * What the callers rely on (LayerPages): construct it with the bus, place it, call refresh() from the page's
 * refresh(). Header only.
 */
class OffsetMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int c_width = 56, c_height = 10;
    /** The block's width, and how near the centre counts as home (a share of the travel). */
    static constexpr float c_block = 4.0f, c_home = 0.004f;

    OffsetMeter(SwooshbucklerAudioProcessor& processor, int bus) : m_processor(processor), m_bus(bus)
    {
        setTooltip("Varispeed Offset\nHow far the second machine has drifted from the first: left is behind it, right is ahead. "
                   "It stays where Varispeed left it.\nDouble-click to slide it home.");
    }

    /** About 30 times a second while its page shows: repaints when the offset has moved. */
    void refresh()
    {
        const float now = juce::jlimit(-1.0f, 1.0f, m_processor.getVarispeedOffset(m_bus));
        if (std::abs(now - m_shown) < 0.002f && (std::abs(now) < c_home) == (std::abs(m_shown) < c_home))
            return;
        m_shown = now;
        repaint();
    }

    /** The offset it is showing (-1 .. 1). */
    float getShown() const { return m_shown; }
    /** Where the block's centre is for an offset, in this component's coordinates. */
    float xFor(float offset) const
    {
        const auto slot = track();
        return slot.getCentreX() + juce::jlimit(-1.0f, 1.0f, offset) * 0.5f * (slot.getWidth() - c_block);
    }

    void paint(juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        const auto slot = track();
        const bool live = isEnabled() && Theme::getLook(*this) == Theme::Look::Normal;
        const bool home = std::abs(m_shown) < c_home;
        const auto colour = m_bus == 0 ? Theme::Colours::busA : Theme::Colours::busB;

        // The slot: a well cut into the plate.
        g.setColour(Theme::Colours::well);
        g.fillRect(r);
        Theme::Plate::wellEdge(g, r, 0.0f);
        // The line along it, and the centre mark standing over it.
        g.setColour(Theme::Colours::stroke.withAlpha(live ? 1.0f : 0.5f));
        g.fillRect(juce::Rectangle<float>(slot.getX(), std::floor(slot.getCentreY()), slot.getWidth(), 1.0f));
        g.setColour(Theme::Colours::textFaint.withAlpha(live ? 1.0f : 0.5f));
        g.fillRect(juce::Rectangle<float>(std::floor(slot.getCentreX()), slot.getY(), 1.0f, slot.getHeight()));
        // How far it has moved: a bar from the centre out to the block, then the block.
        const float x = xFor(m_shown);
        if (live && ! home)
        {
            g.setColour(colour.withAlpha(0.45f));
            g.fillRect(juce::Rectangle<float>(juce::jmin(x, slot.getCentreX()), std::floor(slot.getCentreY()) - 1.0f,
                                              std::abs(x - slot.getCentreX()), 3.0f));
        }
        g.setColour(! live ? Theme::Colours::textFaint.withAlpha(0.5f) : (home ? Theme::Colours::textDim : colour));
        g.fillRect(juce::Rectangle<float>(c_block, slot.getHeight()).withCentre({ std::round(x), slot.getCentreY() }));
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (isEnabled())
            m_processor.recentreVarispeed(m_bus);
    }

private:
    /** Inside the slot's edge: where the line runs and the block travels. */
    juce::Rectangle<float> track() const { return getLocalBounds().toFloat().reduced(2.0f, 2.0f); }

    SwooshbucklerAudioProcessor& m_processor;
    int m_bus = 0;
    float m_shown = 0.0f;
};

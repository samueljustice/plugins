#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

/**
 * THE VARISPEED OFFSET METER (a stub: it draws nothing; filled in by the EQ view's work package).
 *
 * What the filler must provide (design 2.5): 56 x 10 under the Varispeed wheel on a Tape Flanger page: a line, a
 * centre mark and a tick that moves with the held offset (SwooshbucklerAudioProcessor::getVarispeedOffset(bus),
 * -1 .. 1 of the travel either side); double-click slides it home (recentreVarispeed(bus)). Hard edges only.
 *
 * What the callers rely on (LayerPages): construct it with the bus, place it, call refresh() from the page's
 * refresh(). Header only.
 */
class OffsetMeter : public juce::Component
{
public:
    static constexpr int c_width = 56, c_height = 10;

    OffsetMeter(SwooshbucklerAudioProcessor& processor, int bus) : m_processor(processor), m_bus(bus) {}

    void refresh() { juce::ignoreUnused(m_processor, m_bus); }
    void paint(juce::Graphics&) override {}
    void mouseDoubleClick(const juce::MouseEvent&) override {}

private:
    SwooshbucklerAudioProcessor& m_processor;
    int m_bus = 0;
};

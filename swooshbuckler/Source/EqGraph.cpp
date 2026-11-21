#include "EqGraph.h"
#include "PluginProcessor.h"

// A stub: nothing is drawn. See EqGraph.h for what the filler provides.

EqGraph::EqGraph(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_fonts(fonts)
{
    juce::ignoreUnused(m_processor, m_fonts);
}

EqGraph::~EqGraph() = default;

void EqGraph::paintMiniature(juce::Graphics& g, juce::Rectangle<float> area, const OutputEq::Params& p, double sampleRate,
                             juce::Colour colour, bool lit)
{
    juce::ignoreUnused(g, area, p, sampleRate, colour, lit);
}

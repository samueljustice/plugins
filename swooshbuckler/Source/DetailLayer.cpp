#include "DetailLayer.h"
#include "PluginProcessor.h"

// A stub: the layer is never shown. See DetailLayer.h for what the filler provides.

DetailLayer::DetailLayer(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_fonts(fonts)
{
    setInterceptsMouseClicks(false, false);
    setVisible(false);
}

DetailLayer::~DetailLayer() = default;

void DetailLayer::open(Layers::Target target, juce::Colour accent)
{
    juce::ignoreUnused(accent);
    m_target = target;
}

void DetailLayer::close()
{
    m_open = false;
}

void DetailLayer::resetDetail()
{
    juce::ignoreUnused(m_processor, m_fonts);
}

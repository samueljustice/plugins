#include "EqPlate.h"
#include "PluginProcessor.h"

// A stub: nothing is drawn. See EqPlate.h for what the filler provides.

EqPlate::EqPlate(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_fonts(fonts)
{
    juce::ignoreUnused(m_processor, m_fonts);
}

EqPlate::~EqPlate() = default;

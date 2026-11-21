#include "DetailLayer.h"
#include "PluginProcessor.h"
#include "LookAndFeel.h"
#include "OffsetMeter.h"

// The grid's numbers are the wheel's own, and everything in a cell fits the knob row.
static_assert(DetailLayer::c_wheelWidth == WheelKnob::cellWidth(WheelKnob::Size::Secondary)
                  && DetailLayer::c_wheelHeight == WheelKnob::cellHeight(WheelKnob::Size::Secondary),
              "a layer's cells hold a Secondary wheel whole");
static_assert(DetailLayer::c_wheelWidth <= DetailLayer::c_rideCell && DetailLayer::c_meterWidth <= DetailLayer::c_cell
                  && DetailLayer::c_meterWidth == OffsetMeter::c_width && DetailLayer::c_meterHeight == OffsetMeter::c_height
                  && DetailLayer::c_wheelY + DetailLayer::c_wheelHeight + DetailLayer::c_meterGap + DetailLayer::c_meterHeight <= DetailLayer::c_rowHeight
                  && DetailLayer::c_maxChoices * DetailLayer::c_choiceHeight + (DetailLayer::c_maxChoices - 1) * DetailLayer::c_choiceGap <= DetailLayer::c_rowHeight,
              "wheels, the offset meter and three stacked boxes fit a cell and the knob row");
static_assert(DetailLayer::c_frontCell + DetailLayer::pageWidth({ Layers::Kind::Swirl, 0 }) <= 448
                  && DetailLayer::pageWidth({ Layers::Kind::Together, 0 }) <= 286,
              "a bus's layer fits Bus A's 448 px of knobs, Ride's its 286 px");

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

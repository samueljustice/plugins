#include "EqPlate.h"
#include "LookAndFeel.h"
#include "Parameters.h"
#include "PluginProcessor.h"

namespace
{
    namespace TC = Theme::Colours;
    const char* const c_slopeRests = "It rests while Low Cut is off.";
}

EqPlate::EqPlate(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_fonts(fonts), m_graph(processor, fonts)
{
    setOpaque(false);
    setWantsKeyboardFocus(true); // Esc closes it
    addAndMakeVisible(m_graph);
    m_graph.onSelectionChanged = [this] { updateKeys(); repaint(readoutArea()); };

    for (auto* key : { &m_on, &m_slope, &m_flat, &m_close })
    {
        key->setColour(juce::TextButton::buttonOnColourId, TC::output);
        addAndMakeVisible(*key);
    }
    m_on.setClickingTogglesState(true);
    m_slope.setClickingTogglesState(true);
    m_onAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.getState(), ParamIDs::eqOn, m_on);
    m_slopeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.getState(), ParamIDs::eqLowCutSlope, m_slope);
    m_flat.onClick = [this] { m_graph.flattenAll(); };
    m_close.onClick = [this]
    {
        if (onClose)
            onClose();
    };

    m_on.setTooltip("Output EQ\n" + Params::tipFor(ParamIDs::eqOn));
    m_flat.setTooltip("Flat\nEvery band back to 0 dB and both cuts off, in one step. Each band keeps its frequency and its Q.");
    m_close.setTooltip("Close\nPuts the plate away; the EQ stays as it is set. Esc does the same.");

    setSize(c_width, c_height);
    updateKeys();
}

EqPlate::~EqPlate() = default;

juce::Rectangle<int> EqPlate::readoutArea() const
{
    const int keys = c_onWidth + c_slopeWidth + c_flatWidth + c_closeWidth + 3 * c_keyGap;
    return { c_graphX, c_rowY, getWidth() - 2 * c_graphX - keys - 8, c_rowHeight };
}

void EqPlate::resized()
{
    m_graph.setBounds(c_graphX, c_graphY, EqGraph::c_width, EqGraph::c_height);
    int x = getWidth() - c_graphX;
    const auto place = [&](juce::Button& key, int width)
    {
        x -= width;
        key.setBounds(x, c_rowY, width, c_rowHeight);
        x -= c_keyGap;
    };
    place(m_close, c_closeWidth);
    place(m_flat, c_flatWidth);
    place(m_slope, c_slopeWidth);
    place(m_on, c_onWidth);
}

void EqPlate::updateKeys()
{
    // The slope belongs to Low Cut: while that is off there is nothing for it to shape.
    const bool rests = m_graph.isParked(OutputEq::LowCut);
    Theme::setLook(m_slope, rests ? Theme::Look::Resting : Theme::Look::Normal);
    const auto tip = "Low Cut Slope\n" + Params::tipFor(ParamIDs::eqLowCutSlope) + " Lit: 24; unlit: 12." + (rests ? juce::String("\n") + c_slopeRests : juce::String());
    if (m_slope.getTooltip() != tip)
        m_slope.setTooltip(tip);
}

void EqPlate::refresh()
{
    if (! Theme::isShownInWindow(*this))
        return;
    m_graph.refresh();
    updateKeys();
    // The readout follows whatever moved the band (a drag, the wheel, undo, a host).
    const int band = m_graph.getSelectedBand();
    const auto now = m_graph.readout(band);
    if (band != m_readoutBand || now.frequency != m_readout.frequency || now.gain != m_readout.gain || now.q != m_readout.q)
        repaint(readoutArea());
}

bool EqPlate::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (onClose)
            onClose();
        return true;
    }
    return false;
}

void EqPlate::visibilityChanged()
{
    if (isShowing())
        grabKeyboardFocus();
}

void EqPlate::paint(juce::Graphics& g)
{
    // The plate, with room under it for its shadow, and a tally of the Output colour along its head.
    const auto plate = getLocalBounds().toFloat().withTrimmedBottom(3.0f);
    SwooshLookAndFeel::drawSurface(g, plate, Theme::Cut::panel, TC::panel);
    g.setColour(TC::output);
    g.fillRect(juce::Rectangle<float>(plate.getX() + 1.0f, plate.getY() + 1.0f, plate.getWidth() - 2.0f, 2.0f));
    g.setColour(TC::edgeDark);
    g.fillRect(juce::Rectangle<float>(plate.getX() + 1.0f, plate.getY() + 3.0f, plate.getWidth() - 2.0f, 1.0f));

    // The readout row.
    const auto row = readoutArea().toFloat();
    const int band = m_graph.getSelectedBand();
    m_readoutBand = band;
    m_readout = m_graph.readout(band);
    if (band < 0)
    {
        g.setColour(TC::textDim);
        Theme::fitText(g, "Drag a handle, or right-click one to type its values.", row, m_fonts.body(), juce::Justification::centredLeft,
                       Theme::Fit::Sentence);
        return;
    }
    const bool on = m_processor.getOutputEqParams().on;
    auto rest = row;
    g.setColour(on ? TC::output : TC::textDim);
    Theme::fitText(g, m_readout.name, rest.removeFromLeft(84.0f), m_fonts.title(), juce::Justification::centredLeft);
    g.setColour(TC::text);
    Theme::fitText(g, m_readout.frequency, rest.removeFromLeft(64.0f), m_fonts.body(), juce::Justification::centredLeft);
    g.setColour(EqGraph::isCut(band) ? TC::textDim : TC::text);
    if (! (band == OutputEq::LowCut && m_graph.isParked(band)) && ! (band == OutputEq::HighCut && m_graph.isParked(band)))
        Theme::fitText(g, m_readout.gain, rest.removeFromLeft(64.0f), m_fonts.body(), juce::Justification::centredLeft);
    g.setColour(TC::text);
    if (m_readout.q.isNotEmpty())
        Theme::fitText(g, m_readout.q, rest.removeFromLeft(56.0f), m_fonts.body(), juce::Justification::centredLeft);
}

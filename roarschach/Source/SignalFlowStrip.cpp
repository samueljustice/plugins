#include "SignalFlowStrip.h"

#include "Params.h"
#include "RoarschachLookAndFeel.h"

#include <cmath>

namespace
{
    namespace Palette = Theme::Colours;

    // Design-size metrics (the editor scales everything uniformly).
    constexpr int c_staleAfterTicks = 15;  // 0.5 s of the 30 Hz meter tick without a new feature chunk
    constexpr int c_gap = 22;              // between panels: a chevron, or a drop marker while dragging
    constexpr int c_headerHeight = 32;
    constexpr int c_readoutHeight = 66;    // meters and readouts along the bottom of every panel
    constexpr int c_readoutPad = 12;
    constexpr int c_maxKnobWidth = 96;
    constexpr int c_dragThreshold = 4;     // px before a press on a header becomes a drag

    constexpr float c_meterFloorDb = -60.0f;
    constexpr float c_slide = 0.28f;       // share of the remaining distance a panel slides per frame (60 Hz)

    // Relative panel widths, in PanelId order.
    constexpr std::array<float, 9> c_weights { 1.2f, 2.0f, 1.15f, 1.15f, 1.15f, 1.15f, 1.3f, 2.0f, 2.0f };

    float toDb(float linear)
    {
        return linear > 1.0e-5f ? 20.0f * std::log10(linear) : -100.0f;
    }

    juce::String dbText(float db)
    {
        if (db <= c_meterFloorDb)
            return "-inf dB";
        return juce::String(db, 1) + " dB";
    }

    float sanitise(float v, float fallback = 0.0f)
    {
        return std::isfinite(v) ? v : fallback;
    }
}

//==============================================================================
SignalFlowStrip::Panel::Panel(SignalFlowStrip& owner, PanelId id, juce::String title, juce::Colour accent)
    : m_id(id)
    , m_title(std::move(title))
    , m_accent(accent)
    , m_owner(owner)
{
    setTitle(m_title);
    setOpaque(false);
}

void SignalFlowStrip::Panel::paint(juce::Graphics& g)
{
    m_owner.paintPanel(g, *this);
}

bool SignalFlowStrip::Panel::isInHeader(juce::Point<int> p) const
{
    return p.y >= 0 && p.y < c_headerHeight;
}

void SignalFlowStrip::Panel::mouseMove(const juce::MouseEvent& e)
{
    if (!isMovable())
        return;

    const bool hot = isInHeader(e.getPosition());
    setMouseCursor(hot ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    if (hot != m_headerHot)
    {
        m_headerHot = hot;
        repaint(getLocalBounds().withHeight(c_headerHeight));
    }
}

void SignalFlowStrip::Panel::mouseExit(const juce::MouseEvent&)
{
    if (m_headerHot)
    {
        m_headerHot = false;
        repaint(getLocalBounds().withHeight(c_headerHeight));
    }
}

void SignalFlowStrip::Panel::mouseDown(const juce::MouseEvent& e)
{
    if (isMovable() && isInHeader(e.getPosition()))
        m_owner.panelMouseDown(*this, e);
}

void SignalFlowStrip::Panel::mouseDrag(const juce::MouseEvent& e)
{
    if (isMovable())
        m_owner.panelMouseDrag(*this, e);
}

void SignalFlowStrip::Panel::mouseUp(const juce::MouseEvent& e)
{
    if (isMovable())
        m_owner.panelMouseUp(*this, e);
}

//==============================================================================
SignalFlowStrip::MoveChip::MoveChip(juce::Colour accent)
    : juce::Button("Move")
    , m_accent(accent)
{
    setWantsKeyboardFocus(true);
}

void SignalFlowStrip::MoveChip::paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown)
{
    // A pill like the editor's buttons, with three dots drawn as shapes (no glyph).
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    const float corner = r.getHeight() * 0.5f;

    g.setColour(Palette::panelRaised.brighter(isButtonDown ? 0.2f : isMouseOverButton ? 0.1f : 0.0f));
    g.fillRoundedRectangle(r, corner);

    if (RoarschachLookAndFeel::shouldShowFocusRing(*this))
    {
        g.setColour(Palette::output);
        g.drawRoundedRectangle(r.reduced(0.25f), corner, 1.5f);
    }
    else
    {
        g.setColour(isMouseOverButton ? Palette::textDim : Palette::outline);
        g.drawRoundedRectangle(r, corner, 1.0f);
    }

    g.setColour(isMouseOverButton || isButtonDown ? m_accent : Palette::textDim);
    const auto c = r.getCentre();
    for (int i = -1; i <= 1; ++i)
        g.fillEllipse(juce::Rectangle<float>(3.0f, 3.0f).withCentre({ c.x + static_cast<float>(i) * 5.5f, c.y }));
}

//==============================================================================
SignalFlowStrip::SignalFlowStrip(juce::AudioProcessorValueTreeState& parameters, const roar::MonsterFeatures& features,
                                 const Theme::Fonts& fonts)
    : m_parameters(parameters)
    , m_features(features)
    , m_fonts(fonts)
{
    setOpaque(false);

    const auto addPanel = [this](PanelId id, const char* title, juce::Colour accent, const juce::String& tooltip)
    {
        auto panel = std::make_unique<Panel>(*this, id, title, accent);
        panel->setTooltip(tooltip);
        addAndMakeVisible(*panel);
        m_panels[static_cast<size_t>(id)] = std::move(panel);
    };

    addPanel(inPanel, "IN", Palette::clean,
             "The input level. Before anything else the voice is cleaned up automatically: a gentle gate keeps room "
             "noise down, a compressor evens out the level, a low shelf trims the bass and a de-esser softens sharp "
             "S sounds.");
    addPanel(pitchPanel, "PITCH", Palette::pitch,
             "The pitch of the voice going in, and where it comes out after the shift.");
    addPanel(growlPanel, "GROWL", Palette::growl,
             "How much the growl is shaking the voice right now.");
    addPanel(liquidPanel, "LIQUID", Palette::liquid,
             "The slow swirl of the liquid movement.");
    addPanel(roarPanel, "ROAR", Palette::roar,
             "How hard the saturation is being driven right now.");
    addPanel(subPanel, "SUB", Palette::sub,
             "The level of the octave-down tone right now.");
    addPanel(outPanel, "OUT", Palette::output,
             "The output level, after the final clean-up: a high and low cut, automatic level matching (so turning "
             "the knobs doesn't make it jump in loudness) and a limiter that catches the peaks.");
    addPanel(detailPanel, "DETAIL", Palette::detail,
             "Evens out the voice: lifts the quiet details and holds down the loudest parts. Drag the header to move "
             "it to another point in the chain. Early on, the stages after it hear the evened-out voice; late in the "
             "chain, it evens out what they made.");
    addPanel(vocoderPanel, "VOCODER", Palette::vocoder,
             "A 32-band vocoder that plays the voice on a buzzy tone following its pitch. Drag the header to move it "
             "to another point in the chain. Early on, the stages after it shape the vocoded sound; late in the "
             "chain, it vocodes what they made.");

    auto& pitchP = *m_panels[pitchPanel];
    setupKnob(m_pitch, pitchP, "Pitch", Params::pitch,
              "Shifts the voice down (or up) in semitones while the vowels stay in place, so the words stay clear.",
              Palette::pitch, Format::Semitones);
    setupKnob(m_size, pitchP, "Size", Params::size,
              "Body size: lowers the vowel resonances and adds chest and throat resonance, so the voice sounds as if "
              "it comes from a bigger body. The pitch stays where it is.",
              Palette::pitch, Format::Percent);
    setupKnob(m_growl, *m_panels[growlPanel], "Growl", Params::growl,
              "A rough, rattling texture: a growl an octave below the voice, a fast tremolo and a little "
              "unsteadiness. Loud lines growl harder than quiet ones.",
              Palette::growl, Format::Percent);
    setupKnob(m_liquid, *m_panels[liquidPanel], "Liquid", Params::liquid,
              "Wet, shifting movement: a slow pitch shimmer, a phaser and an ensemble of voices that also spreads "
              "the sound into stereo.",
              Palette::liquid, Format::Percent);
    setupKnob(m_roar, *m_panels[roarPanel], "Roar", Params::roar,
              "Tube saturation that drives the lows and highs hardest. Harshness in the top end is tamed "
              "automatically, and the level stays about the same as you turn it up.",
              Palette::roar, Format::Percent);
    setupKnob(m_sub, *m_panels[subPanel], "Sub", Params::sub,
              "A clean tone an octave below the voice that follows its pitch, for weight. It only sounds while the "
              "voice is voiced.",
              Palette::sub, Format::Percent);
    setupKnob(m_mix, *m_panels[outPanel], "Mix", Params::mix,
              "Blend between the dry input and the creature voice. The dry signal is delayed to line up with it.",
              Palette::output, Format::Percent);
    setupKnob(m_detailLow, *m_panels[detailPanel], "Low", Params::detailLow,
              "Lifts the quiet parts (breaths, word endings, small mouth sounds) toward the level of the rest. Room "
              "noise and silence are left alone.",
              Palette::detail, Format::Percent);
    setupKnob(m_detailHigh, *m_panels[detailPanel], "High", Params::detailHigh,
              "Holds down the loudest parts so shouts don't jump out. Higher settings start lower and hold harder.",
              Palette::detail, Format::Percent);
    setupKnob(m_gravel, *m_panels[vocoderPanel], "Gravel", Params::gravel,
              "How much of the vocoder is blended in, and how gritty it is: higher settings let more rasp through. "
              "At 0% the vocoder is off.",
              Palette::vocoder, Format::Percent);

    // Movable panels: the "Move to" chip in the header.
    for (auto* chip : { &m_detailChip, &m_vocoderChip })
    {
        const bool isDetail = chip == &m_detailChip;
        chip->setTooltip(juce::String("Move ") + (isDetail ? "DETAIL" : "VOCODER") + " to another point in the chain");
        chip->setTitle(juce::String("Move ") + (isDetail ? "Detail" : "Vocoder"));
        chip->onClick = [this, isDetail] { showMoveMenu(isDetail ? Module::detail : Module::vocoder); };
        // The move menu takes the colour of the module it drops from (see menuAccent in the LookAndFeel).
        chip->setColour(juce::PopupMenu::highlightedBackgroundColourId, isDetail ? Palette::detail : Palette::vocoder);
        m_panels[isDetail ? detailPanel : vocoderPanel]->addAndMakeVisible(*chip);
    }

    // VOCODER header: the carrier octave, one switch that shows the current choice.
    m_octaveButton.setClickingTogglesState(true);
    m_octaveButton.setTooltip("The vocoder's tone sits one or two octaves below the voice. Click to switch.");
    m_octaveButton.setColour(juce::TextButton::buttonOnColourId, Palette::vocoder.withAlpha(0.28f));
    m_octaveButton.setColour(juce::TextButton::textColourOnId, Palette::text);
    m_octaveButton.onStateChange = [this]
    {
        const auto label = juce::String(m_octaveButton.getToggleState() ? "-2 OCT" : "-1 OCT");
        if (m_octaveButton.getButtonText() != label)
            m_octaveButton.setButtonText(label);
    };
    m_octaveButton.setButtonText("-1 OCT");
    m_panels[vocoderPanel]->addAndMakeVisible(m_octaveButton);
    m_octaveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(m_parameters, Params::vocoderOctave,
                                                                                              m_octaveButton);

    // The slot parameters: a click, automation or a preset slides the panels to their new places.
    const auto attachSlot = [this](const char* id, int& slot, std::unique_ptr<juce::ParameterAttachment>& attachment)
    {
        if (auto* param = m_parameters.getParameter(id))
        {
            attachment = std::make_unique<juce::ParameterAttachment>(*param, [this, &slot](float value)
            {
                const int s = juce::jlimit(0, Params::numSlots - 1, juce::roundToInt(value));
                if (s != slot)
                {
                    slot = s;
                    relayout(true);
                }
            });
            attachment->sendInitialUpdate();
        }
    };
    attachSlot(Params::detailSlot, m_detailSlot, m_detailSlotAttachment);
    attachSlot(Params::vocoderSlot, m_vocoderSlot, m_vocoderSlotAttachment);

    startTimerHz(60);
}

SignalFlowStrip::~SignalFlowStrip()
{
    stopTimer();
    m_octaveButton.onStateChange = nullptr;
}

void SignalFlowStrip::setupKnob(Knob& knob, Panel& panel, const juce::String& name, const juce::String& paramId,
                                const juce::String& tooltip, juce::Colour accent, Format format)
{
    auto& slider = knob.slider;
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 84, 18);
    slider.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    slider.setColour(juce::Slider::rotarySliderFillColourId, accent);
    slider.setTooltip(tooltip);
    slider.setTitle(name);

    switch (format)
    {
        case Format::Percent:
            slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v)) + "%"; };
            break;
        case Format::Semitones:
            slider.textFromValueFunction = [](double v)
            {
                const double rounded = std::round(v * 100.0) / 100.0;
                if (std::abs(rounded) < 0.005)
                    return juce::String("0.00 st");
                return (rounded > 0.0 ? "+" : "") + juce::String(rounded, 2) + " st";
            };
            break;
    }

    knob.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(m_parameters, paramId, slider);

    if (auto* param = m_parameters.getParameter(paramId))
    {
        // Typed values go through the parameter's own parser, so the value box reads text exactly as the
        // host does (and as it is displayed).
        slider.valueFromTextFunction = [param](const juce::String& text)
        {
            return static_cast<double>(param->convertFrom0to1(param->getValueForText(text)));
        };
        slider.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));
    }

    slider.updateText();

    panel.addAndMakeVisible(slider);

    // The value box was created before the slider had a parent, i.e. with the
    // default LookAndFeel. Setting a colour makes the slider rebuild it via ours.
    slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    auto& label = knob.label;
    label.setText(name.toUpperCase(), juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, Palette::textDim);
    label.setInterceptsMouseClicks(false, false);
    panel.addAndMakeVisible(label);
}

//==============================================================================
// Layout
//==============================================================================
std::array<juce::Rectangle<int>, SignalFlowStrip::numPanels> SignalFlowStrip::computeLayout(int detailSlot, int vocoderSlot) const
{
    // Chain order: IN, [slot 0], PITCH, [slot 1], GROWL, [slot 2], LIQUID, [slot 3], ROAR, [slot 4], SUB, [slot 5], OUT.
    // When both modules share a slot, DETAIL runs first.
    std::vector<PanelId> order;
    for (int fixed = inPanel; fixed <= outPanel; ++fixed)
    {
        order.push_back(static_cast<PanelId>(fixed));
        if (fixed <= subPanel)
        {
            if (detailSlot == fixed)
                order.push_back(detailPanel);
            if (vocoderSlot == fixed)
                order.push_back(vocoderPanel);
        }
    }

    float totalWeight = 0.0f;
    for (auto id : order)
        totalWeight += c_weights[static_cast<size_t>(id)];

    const auto area = getLocalBounds();
    const int available = area.getWidth() - c_gap * static_cast<int>(order.size() - 1);
    const float unit = static_cast<float>(available) / totalWeight;

    std::array<juce::Rectangle<int>, numPanels> layout;
    float x = static_cast<float>(area.getX());
    for (size_t i = 0; i < order.size(); ++i)
    {
        const float w = unit * c_weights[static_cast<size_t>(order[i])];
        const int left = juce::roundToInt(x);
        const int right = (i + 1 == order.size()) ? area.getRight() : juce::roundToInt(x + w);
        layout[static_cast<size_t>(order[i])] = { left, area.getY(), right - left, area.getHeight() };
        x += w + static_cast<float>(c_gap);
    }
    return layout;
}

void SignalFlowStrip::resized()
{
    relayout(false);
}

void SignalFlowStrip::relayout(bool animate)
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    const bool dragging = m_dragPanel != numPanels && m_dragStarted;
    const int detailSlot = dragging && m_dragPanel == detailPanel ? m_dropSlot : m_detailSlot;
    const int vocoderSlot = dragging && m_dragPanel == vocoderPanel ? m_dropSlot : m_vocoderSlot;
    const auto layout = computeLayout(detailSlot, vocoderSlot);

    for (size_t i = 0; i < m_panels.size(); ++i)
    {
        m_target[i] = layout[i].toFloat();

        // Contents follow the panel's size, which never changes while it slides.
        auto& panel = *m_panels[i];
        const bool sizeChanged = panel.getWidth() != layout[i].getWidth() || panel.getHeight() != layout[i].getHeight();

        if (!animate || !m_laidOut)
            m_current[i] = m_target[i];

        if (!(dragging && static_cast<PanelId>(i) == m_dragPanel))
            panel.setBounds(m_current[i].withSize(m_target[i].getWidth(), m_target[i].getHeight()).toNearestInt());

        if (sizeChanged)
            layoutPanelContents(panel);
    }

    m_laidOut = true;
    m_animating = animate;
    repaint();
}

void SignalFlowStrip::finishAnimations()
{
    relayout(false);
}

juce::Rectangle<int> SignalFlowStrip::knobRowArea(const Panel& panel) const
{
    return panel.getLocalBounds().withTrimmedTop(c_headerHeight).withTrimmedBottom(c_readoutHeight + c_readoutPad).reduced(6, 8);
}

juce::Rectangle<int> SignalFlowStrip::readoutArea(const Panel& panel) const
{
    return panel.getLocalBounds().removeFromBottom(c_readoutHeight + c_readoutPad).withTrimmedBottom(c_readoutPad).reduced(12, 0);
}

void SignalFlowStrip::layoutPanelContents(Panel& panel)
{
    std::vector<Knob*> knobs;
    switch (panel.m_id)
    {
        case pitchPanel:   knobs = { &m_pitch, &m_size }; break;
        case growlPanel:   knobs = { &m_growl }; break;
        case liquidPanel:  knobs = { &m_liquid }; break;
        case roarPanel:    knobs = { &m_roar }; break;
        case subPanel:     knobs = { &m_sub }; break;
        case outPanel:     knobs = { &m_mix }; break;
        case detailPanel:  knobs = { &m_detailLow, &m_detailHigh }; break;
        case vocoderPanel: knobs = { &m_gravel }; break;
        case inPanel:
        case numPanels:
        default: break;
    }

    if (!knobs.empty())
    {
        auto area = knobRowArea(panel);
        const int cellWidth = area.getWidth() / static_cast<int>(knobs.size());
        const int knobWidth = std::min(cellWidth - 8, c_maxKnobWidth);
        const int sliderHeight = c_maxKnobWidth + 18; // the dial and its value box, at the same height everywhere
        const int blockHeight = 16 + 4 + c_maxKnobWidth + 18; // the same in every panel, so labels and dials line up
        const int top = area.getY() + std::max(0, (area.getHeight() - blockHeight) / 2);

        for (auto* knob : knobs)
        {
            auto cell = area.removeFromLeft(cellWidth);
            knob->label.setBounds(cell.getX(), top, cell.getWidth(), 16);
            knob->slider.setBounds(cell.withY(top + 20).withHeight(sliderHeight).withSizeKeepingCentre(knobWidth + 8, sliderHeight));
        }
    }

    // Header controls, right-aligned in the header row.
    if (panel.isMovable())
    {
        auto header = panel.getLocalBounds().withHeight(c_headerHeight).reduced(8, 6);
        auto& chip = panel.m_id == detailPanel ? m_detailChip : m_vocoderChip;
        chip.setBounds(header.removeFromRight(30));

        if (panel.m_id == vocoderPanel)
        {
            header.removeFromRight(6);
            m_octaveButton.setBounds(header.removeFromRight(64));
        }
    }
}

//==============================================================================
// Painting
//==============================================================================
void SignalFlowStrip::paint(juce::Graphics& g)
{
    // While a panel is dragged, the gaps show drop markers instead of chevrons.
    if (m_dragStarted)
    {
        paintDropMarkers(g);
        return;
    }

    // Chevrons in the gaps between neighbouring panels (in chain order, where they are now).
    std::vector<juce::Rectangle<float>> shown;
    for (size_t i = 0; i < m_panels.size(); ++i)
        shown.push_back(m_panels[i]->getBounds().toFloat());

    std::sort(shown.begin(), shown.end(), [](const auto& a, const auto& b) { return a.getX() < b.getX(); });

    for (size_t i = 0; i + 1 < shown.size(); ++i)
    {
        const float gapLeft = shown[i].getRight(), gapRight = shown[i + 1].getX();
        if (gapRight - gapLeft >= 8.0f)
            paintChevron(g, (gapLeft + gapRight) * 0.5f, shown[i].getY() + c_headerHeight * 0.5f);
    }
}

void SignalFlowStrip::paintChevron(juce::Graphics& g, float x, float y) const
{
    juce::Path chevron;
    chevron.startNewSubPath(x - 2.0f, y - 4.0f);
    chevron.lineTo(x + 2.0f, y);
    chevron.lineTo(x - 2.0f, y + 4.0f);
    g.setColour(Palette::textFaint);
    g.strokePath(chevron, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void SignalFlowStrip::paintDropMarkers(juce::Graphics& g)
{
    const auto& panel = *m_panels[static_cast<size_t>(m_dragPanel)];
    const auto accent = panel.m_accent;

    // Every other slot gap gets a faint marker; the one the panel would land in is lit (drawn over
    // the panels in paintOverChildren, since the dragged panel usually hangs over it), and its
    // place is outlined.
    for (int slot = 0; slot < Params::numSlots; ++slot)
    {
        if (slot == m_dropSlot)
            continue;

        const auto line = juce::Rectangle<float>(2.0f, static_cast<float>(getHeight()) - 24.0f)
                              .withCentre({ slotMarkerX(slot), static_cast<float>(getHeight()) * 0.5f });
        g.setColour(accent.withAlpha(0.3f));
        g.fillRoundedRectangle(line, 1.0f);
    }

    const auto hole = m_target[static_cast<size_t>(m_dragPanel)].reduced(1.0f);
    g.setColour(accent.withAlpha(0.06f));
    g.fillRoundedRectangle(hole, 8.0f);

    juce::Path outlinePath;
    outlinePath.addRoundedRectangle(hole, 8.0f);
    juce::Path dashed;
    const float dashes[] = { 6.0f, 5.0f };
    juce::PathStrokeType(1.5f).createDashedStroke(dashed, outlinePath, dashes, 2);
    g.setColour(accent.withAlpha(0.75f));
    g.fillPath(dashed);

    g.setFont(m_fonts.label(10.0f));
    g.setColour(accent);
    g.drawText(juce::String(Params::slotNames[static_cast<size_t>(juce::jlimit(0, Params::numSlots - 1, m_dropSlot))]).toUpperCase(),
               hole.withTrimmedTop(hole.getHeight() * 0.5f - 10.0f).withHeight(20.0f), juce::Justification::centred, false);
}

void SignalFlowStrip::paintOverChildren(juce::Graphics& g)
{
    if (!m_dragStarted || m_dropSlot < 0 || m_dropSlot >= Params::numSlots)
        return;

    const auto accent = m_panels[static_cast<size_t>(m_dragPanel)]->m_accent;
    const auto line = juce::Rectangle<float>(3.0f, static_cast<float>(getHeight()) - 24.0f)
                          .withCentre({ slotMarkerX(m_dropSlot), static_cast<float>(getHeight()) * 0.5f });
    g.setColour(accent.withAlpha(0.9f));
    g.fillRoundedRectangle(line, 1.5f);
}

float SignalFlowStrip::slotMarkerX(int slot) const
{
    // The gap right after the fixed stage the slot follows (IN for slot 0 ... SUB for slot 5), in the layout
    // the panels are sliding to.
    const auto& fixed = m_target[static_cast<size_t>(slot)];
    return fixed.getRight() + static_cast<float>(c_gap) * 0.5f;
}

void SignalFlowStrip::paintPanel(juce::Graphics& g, const Panel& panel)
{
    auto* lf = dynamic_cast<RoarschachLookAndFeel*>(&getLookAndFeel());
    const auto bounds = panel.getLocalBounds().toFloat();
    const bool lifted = m_dragStarted && panel.m_id == m_dragPanel;

    if (lf != nullptr)
        lf->drawPanel(g, bounds, panel.m_title, panel.m_accent, static_cast<float>(c_headerHeight));

    if (panel.isMovable())
    {
        // Outline in the module's accent while it is lifted or its header is under the mouse, and a grip
        // (2 x 3 dots) after the title that says the header can be dragged.
        if (lifted || panel.m_headerHot)
        {
            g.setColour(panel.m_accent.withAlpha(lifted ? 0.8f : 0.45f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 1.0f);
        }

        const float titleWidth = juce::GlyphArrangement::getStringWidth(m_fonts.heading(11.0f), panel.m_title);
        const float gx = 12.0f + 6.0f + 8.0f + titleWidth + 12.0f;
        const float gy = c_headerHeight * 0.5f;
        g.setColour(panel.m_headerHot || lifted ? panel.m_accent.withAlpha(0.8f) : Palette::textFaint);
        for (int col = 0; col < 2; ++col)
            for (int row = -1; row <= 1; ++row)
                g.fillEllipse(juce::Rectangle<float>(2.4f, 2.4f).withCentre({ gx + static_cast<float>(col) * 4.5f,
                                                                               gy + static_cast<float>(row) * 4.5f }));
    }

    // Divider above the readouts.
    const auto readout = readoutArea(panel).toFloat();
    g.setColour(Palette::outlineSoft);
    g.fillRect(juce::Rectangle<float>(bounds.getX() + 12.0f, readout.getY() - 9.0f, bounds.getWidth() - 24.0f, 1.0f));

    if (panel.m_id == inPanel)
    {
        // IN has no controls: what the automatic clean-up does, as a short list.
        auto list = knobRowArea(panel).toFloat().reduced(4.0f, 0.0f);
        const std::array<const char*, 4> steps { "GATE", "LEVELLER", "LOW SHELF", "DE-ESSER" };
        const float rowHeight = 20.0f;
        auto rows = list.withSizeKeepingCentre(list.getWidth(), rowHeight * (static_cast<float>(steps.size()) + 1.4f));

        g.setFont(m_fonts.label(9.0f));
        g.setColour(Palette::textFaint);
        g.drawText("ALWAYS ON", rows.removeFromTop(rowHeight * 1.4f).withTrimmedBottom(6.0f), juce::Justification::centredLeft, false);

        g.setFont(m_fonts.label(9.5f));
        for (const auto* step : steps)
        {
            auto row = rows.removeFromTop(rowHeight);
            g.setColour(Palette::clean.withAlpha(0.8f));
            g.fillEllipse(row.removeFromLeft(4.0f).withSizeKeepingCentre(4.0f, 4.0f));
            row.removeFromLeft(8.0f);
            g.setColour(Palette::textDim);
            g.drawText(step, row, juce::Justification::centredLeft, false);
        }
    }

    paintReadout(g, panel, readout);
}

void SignalFlowStrip::paintMeterRow(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& caption,
                                    const juce::String& value, float fill01, juce::Colour accent, bool centredFill)
{
    // Caption left, value right, then a thin bar.
    auto textRow = area.removeFromTop(14.0f);
    g.setFont(m_fonts.label(9.5f));
    g.setColour(Palette::textFaint);
    g.drawText(caption, textRow, juce::Justification::centredLeft, false);

    if (value.isNotEmpty())
    {
        g.setFont(m_fonts.body(10.0f));
        g.setColour(Palette::textDim);
        g.drawText(value, textRow, juce::Justification::centredRight, false);
    }

    area.removeFromTop(6.0f);
    const auto bar = area.removeFromTop(4.0f);
    g.setColour(Palette::outline);
    g.fillRoundedRectangle(bar, 2.0f);

    fill01 = juce::jlimit(0.0f, 1.0f, sanitise(fill01));
    g.setColour(accent);
    if (centredFill)
    {
        // A moving dot along the track (a phase, not a level).
        g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre({ bar.getX() + 4.0f + fill01 * (bar.getWidth() - 8.0f), bar.getCentreY() }));
    }
    else if (fill01 > 0.0f)
    {
        g.fillRoundedRectangle(bar.withWidth(std::max(4.0f, bar.getWidth() * fill01)), 2.0f);
    }
}

void SignalFlowStrip::paintReadout(juce::Graphics& g, const Panel& panel, juce::Rectangle<float> area)
{
    // Rows sit on the bottom edge so the bars line up across panels; two-row panels stack upwards.
    const float rowHeight = 24.0f;
    auto lower = area.withTop(area.getBottom() - rowHeight);
    auto upper = lower.translated(0.0f, -(rowHeight + 16.0f));

    switch (panel.m_id)
    {
        case inPanel:
            paintMeterRow(g, lower, "INPUT", dbText(m_inDb), (m_inDb - c_meterFloorDb) / -c_meterFloorDb, Palette::clean);
            break;

        case pitchPanel:
        {
            const float ratio = std::pow(2.0f, static_cast<float>(m_pitch.slider.getValue()) / 12.0f);
            const bool live = m_voiced && m_f0 > 0.0f;
            const auto hz = [live](float f) { return live ? juce::String(juce::roundToInt(f)) + " Hz" : juce::String("--"); };
            auto row = [&](juce::Rectangle<float> r, const char* caption, const juce::String& value)
            {
                r = r.removeFromTop(14.0f);
                g.setFont(m_fonts.label(9.5f));
                g.setColour(Palette::textFaint);
                g.drawText(caption, r, juce::Justification::centredLeft, false);
                g.setFont(m_fonts.body(10.0f));
                g.setColour(live ? Palette::pitch : Palette::textDim);
                g.drawText(value, r, juce::Justification::centredRight, false);
            };
            row(upper, "VOICE IN", hz(m_f0));
            row(lower, "VOICE OUT", hz(m_f0 * ratio));
            break;
        }

        case growlPanel:
            paintMeterRow(g, lower, "SHAKE", {}, m_growlActivity, Palette::growl);
            break;

        case liquidPanel:
            paintMeterRow(g, lower, "SWIRL", {}, 0.5f + 0.5f * std::sin(m_liquidPhase * juce::MathConstants<float>::twoPi),
                          Palette::liquid, true);
            break;

        case roarPanel:
            paintMeterRow(g, lower, "DRIVE", juce::String(juce::roundToInt(m_roarDrive * 100.0f)) + "%", m_roarDrive,
                          Palette::roar);
            break;

        case subPanel:
            paintMeterRow(g, lower, "LEVEL", {}, m_subLevel, Palette::sub);
            break;

        case outPanel:
            paintMeterRow(g, lower, "OUTPUT", dbText(m_outDb), (m_outDb - c_meterFloorDb) / -c_meterFloorDb, Palette::output);
            break;

        case detailPanel:
        {
            // Magnitudes, signed for reading; no "-0.0".
            const auto signedDb = [](float db, const char* sign)
            {
                return db < 0.05f ? juce::String("0.0 dB") : sign + juce::String(db, 1) + " dB";
            };
            paintMeterRow(g, upper, "LIFT", signedDb(m_boostDb, "+"), m_boostDb / 24.0f, Palette::detail);
            paintMeterRow(g, lower, "HOLD", signedDb(m_cutDb, "-"), m_cutDb / 24.0f, Palette::detail);
            break;
        }

        case vocoderPanel:
        {
            // The 32 band levels, low to high, as a small spectrum.
            auto caption = area.removeFromTop(14.0f);
            g.setFont(m_fonts.label(9.5f));
            g.setColour(Palette::textFaint);
            g.drawText("BANDS", caption, juce::Justification::centredLeft, false);
            g.setFont(m_fonts.body(10.0f));
            g.setColour(m_gravelActive ? Palette::textDim : Palette::textFaint);
            g.drawText(m_gravelActive ? "ON" : "OFF", caption, juce::Justification::centredRight, false);

            area.removeFromTop(6.0f);
            const auto n = static_cast<int>(m_bands.size());
            const float step = area.getWidth() / static_cast<float>(n);
            for (int b = 0; b < n; ++b)
            {
                const auto column = juce::Rectangle<float>(area.getX() + step * static_cast<float>(b), area.getY(), step, area.getHeight())
                                        .reduced(std::min(1.0f, step * 0.2f), 0.0f);
                g.setColour(Palette::outline);
                g.fillRoundedRectangle(column.withTop(column.getBottom() - 2.0f), 1.0f);

                // Square root: quiet upper bands stay visible next to the strong low ones.
                const float level = std::sqrt(juce::jlimit(0.0f, 1.0f, sanitise(m_bands[static_cast<size_t>(b)])));
                if (m_gravelActive && level > 0.01f)
                {
                    g.setColour(Palette::vocoder.withAlpha(0.35f + 0.65f * level));
                    g.fillRoundedRectangle(column.withTop(column.getBottom() - std::max(2.0f, column.getHeight() * level)), 1.0f);
                }
            }
            break;
        }

        case numPanels:
        default:
            break;
    }
}

//==============================================================================
// Meters
//==============================================================================
void SignalFlowStrip::timerCallback()
{
    // Slides (60 Hz).
    if (m_animating)
    {
        bool moving = false;
        for (size_t i = 0; i < m_panels.size(); ++i)
        {
            if (m_dragStarted && static_cast<PanelId>(i) == m_dragPanel)
                continue;

            auto& current = m_current[i];
            const auto& target = m_target[i];
            const float dx = target.getX() - current.getX(), dy = target.getY() - current.getY();

            if (std::abs(dx) < 0.5f && std::abs(dy) < 0.5f)
                current = target;
            else
            {
                current = current.translated(dx * c_slide, dy * c_slide);
                moving = true;
            }
            m_panels[i]->setTopLeftPosition(current.getPosition().roundToInt());
        }

        m_animating = moving;
        repaint(); // chevrons follow the gaps
    }

    // Meters and readouts (30 Hz).
    if (++m_tick % 2 != 0)
        return;

    // When the host stops calling processBlock the features stop updating: after half a second
    // without a new chunk they read as silence, the same rule the creature view uses.
    const int generation = m_features.generation.load(std::memory_order_relaxed);
    if (generation != m_lastGeneration)
    {
        m_lastGeneration = generation;
        m_staleTicks = 0;
    }
    else
    {
        m_staleTicks = std::min(m_staleTicks + 1, c_staleAfterTicks);
    }
    const float live = m_staleTicks >= c_staleAfterTicks ? 0.0f : 1.0f;

    const auto load = [live](const std::atomic<float>& a) { return sanitise(a.load(std::memory_order_relaxed)) * live; };

    const auto smoothDb = [](float& shown, float db)
    {
        db = juce::jlimit(c_meterFloorDb - 1.0f, 6.0f, db);
        shown = db > shown ? db : shown + (db - shown) * 0.25f; // instant rise, eased fall
    };

    const float oldIn = m_inDb, oldOut = m_outDb;
    smoothDb(m_inDb, toDb(load(m_features.inputLevel)));
    smoothDb(m_outDb, toDb(load(m_features.outputLevel)));

    const float growl = std::abs(load(m_features.growlMod)) * static_cast<float>(m_growl.slider.getValue() / 100.0) * 2.0f;
    const float oldGrowl = m_growlActivity, oldDrive = m_roarDrive, oldSub = m_subLevel, oldPhase = m_liquidPhase;
    m_growlActivity += (juce::jlimit(0.0f, 1.0f, growl) - m_growlActivity) * 0.4f;
    m_roarDrive = juce::jlimit(0.0f, 1.0f, load(m_features.roarDrive));
    m_subLevel += (juce::jlimit(0.0f, 1.0f, load(m_features.subLevel)) - m_subLevel) * 0.4f;
    m_liquidPhase = sanitise(m_features.liquidPhase.load(std::memory_order_relaxed)); // a phase, not a level

    const float oldBoost = m_boostDb, oldCut = m_cutDb, oldF0 = m_f0;
    const bool oldVoiced = m_voiced, oldGravel = m_gravelActive;
    m_boostDb = juce::jlimit(0.0f, 99.0f, std::abs(load(m_features.detailBoostDb)));
    m_cutDb = juce::jlimit(0.0f, 99.0f, std::abs(load(m_features.detailCutDb)));
    m_voiced = load(m_features.voiced) > 0.5f;
    const float f0 = load(m_features.f0Hz);
    if (m_voiced && f0 > 0.0f && f0 < 5000.0f)
        m_f0 = f0;
    m_gravelActive = load(m_features.gravelActive) > 0.5f;

    bool bandsMoved = oldGravel != m_gravelActive;
    for (size_t b = 0; b < m_bands.size(); ++b)
    {
        const float v = juce::jlimit(0.0f, 1.0f, load(m_features.vocoderBands[b]));
        if (std::abs(v - m_bands[b]) > 0.01f)
            bandsMoved = true;
        m_bands[b] = v;
    }

    const auto repaintReadout = [this](PanelId id)
    {
        auto& panel = *m_panels[static_cast<size_t>(id)];
        panel.repaint(readoutArea(panel).expanded(4, 2));
    };

    if (std::abs(oldIn - m_inDb) > 0.05f)              repaintReadout(inPanel);
    if (std::abs(oldOut - m_outDb) > 0.05f)            repaintReadout(outPanel);
    if (std::abs(oldGrowl - m_growlActivity) > 0.004f) repaintReadout(growlPanel);
    if (std::abs(oldDrive - m_roarDrive) > 0.004f)     repaintReadout(roarPanel);
    if (std::abs(oldSub - m_subLevel) > 0.004f)        repaintReadout(subPanel);
    if (std::abs(oldPhase - m_liquidPhase) > 0.002f)   repaintReadout(liquidPanel);
    if (std::abs(oldBoost - m_boostDb) > 0.05f || std::abs(oldCut - m_cutDb) > 0.05f) repaintReadout(detailPanel);
    if (oldVoiced != m_voiced || std::abs(oldF0 - m_f0) > 0.5f) repaintReadout(pitchPanel);
    if (bandsMoved)                                    repaintReadout(vocoderPanel);
}

//==============================================================================
// Moving the modules
//==============================================================================
int SignalFlowStrip::getSlot(Module module) const
{
    return module == Module::detail ? m_detailSlot : m_vocoderSlot;
}

void SignalFlowStrip::setSlot(Module module, int slot)
{
    auto& attachment = module == Module::detail ? m_detailSlotAttachment : m_vocoderSlotAttachment;
    if (attachment != nullptr)
        attachment->setValueAsCompleteGesture(static_cast<float>(juce::jlimit(0, Params::numSlots - 1, slot)));
}

juce::Rectangle<int> SignalFlowStrip::getModuleBounds(Module module) const
{
    return m_target[static_cast<size_t>(module == Module::detail ? detailPanel : vocoderPanel)].toNearestInt();
}

juce::Component& SignalFlowStrip::getMoveChip(Module module)
{
    return module == Module::detail ? static_cast<juce::Component&>(m_detailChip) : m_vocoderChip;
}

juce::PopupMenu SignalFlowStrip::buildMoveMenu(Module module)
{
    const int current = getSlot(module);

    juce::PopupMenu menu;
    menu.setLookAndFeel(&getLookAndFeel()); // a menu without a parent window would use JUCE's default look
    menu.addSectionHeader(module == Module::detail ? "MOVE DETAIL TO" : "MOVE VOCODER TO");
    for (int slot = 0; slot < Params::numSlots; ++slot)
        menu.addItem(slot + 1, Params::slotNames[static_cast<size_t>(slot)], true, slot == current);
    return menu;
}

juce::PopupMenu::Options SignalFlowStrip::moveMenuOptions(Module module)
{
    return RoarschachLookAndFeel::menuOptionsFor(getMoveChip(module), 150);
}

void SignalFlowStrip::showMoveMenu(Module module)
{
    buildMoveMenu(module).showMenuAsync(moveMenuOptions(module),
                       [safe = juce::Component::SafePointer<SignalFlowStrip>(this), module](int result)
    {
        if (safe != nullptr && result > 0)
            safe->setSlot(module, result - 1);
    });
}

void SignalFlowStrip::panelMouseDown(Panel& panel, const juce::MouseEvent& e)
{
    m_dragPanel = panel.m_id;
    m_dragStarted = false;
    m_dropSlot = getSlot(moduleOf(panel.m_id));
    m_dragOffset = e.getEventRelativeTo(&panel).position;
}

void SignalFlowStrip::panelMouseDrag(Panel& panel, const juce::MouseEvent& e)
{
    if (m_dragPanel != panel.m_id)
        return;

    if (!m_dragStarted)
    {
        if (e.getDistanceFromDragStart() < c_dragThreshold)
            return;

        m_dragStarted = true;
        panel.toFront(false);
        panel.repaint();
    }

    // The panel follows the mouse along the strip (outlined in its accent); the others make room where it would land.
    const auto mouse = e.getEventRelativeTo(this).position;
    const auto size = panel.getBounds().toFloat();
    const float x = juce::jlimit(static_cast<float>(-size.getWidth() * 0.5f), static_cast<float>(getWidth()) - size.getWidth() * 0.5f,
                                 mouse.x - m_dragOffset.x);
    const auto lifted = size.withPosition(x, 0.0f);
    m_current[static_cast<size_t>(panel.m_id)] = lifted;
    panel.setBounds(lifted.toNearestInt());

    const int slot = nearestSlot(lifted.getCentreX());
    if (slot != m_dropSlot)
    {
        m_dropSlot = slot;
        relayout(true);
    }
    repaint();
}

int SignalFlowStrip::nearestSlot(float x) const
{
    // The slot whose landing place (with the panel in it) is centred closest to where the panel is now.
    int best = 0;
    float bestDistance = std::numeric_limits<float>::max();
    const bool isDetail = m_dragPanel == detailPanel;

    for (int slot = 0; slot < Params::numSlots; ++slot)
    {
        const auto layout = computeLayout(isDetail ? slot : m_detailSlot, isDetail ? m_vocoderSlot : slot);
        const float distance = std::abs(layout[static_cast<size_t>(m_dragPanel)].toFloat().getCentreX() - x);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = slot;
        }
    }
    return best;
}

void SignalFlowStrip::panelMouseUp(Panel& panel, const juce::MouseEvent&)
{
    if (m_dragPanel != panel.m_id)
        return;

    const bool wasDragging = m_dragStarted;
    const int slot = m_dropSlot;
    const auto module = moduleOf(panel.m_id);

    m_dragPanel = numPanels;
    m_dragStarted = false;
    m_dropSlot = -1;

    if (wasDragging)
    {
        // Slides from where it was dropped into its new place.
        if (slot >= 0 && slot != getSlot(module))
            setSlot(module, slot); // the attachment's callback lays the strip out again
        relayout(true);
        panel.repaint();
    }
}

#include "DetailLayer.h"
#include "PluginProcessor.h"
#include "LookAndFeel.h"
#include "OffsetMeter.h"
#include "LayerPages.h"
#include "EditHistory.h"

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

DetailLayer::DetailLayer(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_fonts(fonts)
{
    // Only the page and the two keys take the mouse: everywhere else (the front wheel in its cell, the scope strip)
    // clicks go through to the panel under the host.
    setInterceptsMouseClicks(false, true);
    setVisible(false);
    m_fold.snap(0.0f);

    for (auto* key : { &m_reset, &m_back })
    {
        key->setWantsKeyboardFocus(false); // Esc and the rig's keys stay with the window
        addAndMakeVisible(*key);
    }
    m_reset.onClick = [this] { resetDetail(); };
    m_back.onClick = [this] { close(); };
}

DetailLayer::~DetailLayer()
{
    // The window is going too: nobody needs telling about the page.
    onPageChanged = nullptr;
    m_page.reset();
}

void DetailLayer::setPlaces(juce::Rectangle<int> page, juce::Rectangle<int> keys)
{
    m_pagePlace = page;
    m_keysPlace = keys;
    resized();
}

void DetailLayer::resized()
{
    if (m_page != nullptr)
        m_page->setBounds(m_pagePlace);
    // Back at the right end of the title bar, where the panel's last control was; Reset left of it.
    auto keys = m_keysPlace;
    m_back.setBounds(keys.removeFromRight(58));
    keys.removeFromRight(6);
    m_reset.setBounds(keys.removeFromRight(62));
}

void DetailLayer::makePage(juce::Colour accent)
{
    m_page = LayerPages::make({ m_processor, m_fonts, accent, m_target });
    addAndMakeVisible(*m_page);
    m_page->setBounds(m_pagePlace);
    m_page->refresh();
    const juce::String front = m_target.kind == Layers::Kind::Swirl    ? "the Swirl wheel"
                             : m_target.kind == Layers::Kind::Filter   ? "the Filter wheel"
                             : m_target.kind == Layers::Kind::Together ? "the Rate wheel"
                                                                       : "Swell itself";
    m_reset.setTooltip("Reset\nPuts everything on this layer back to its default, as one step you can undo. "
                       + front.substring(0, 1).toUpperCase() + front.substring(1) + " is left as it is.");
    m_back.setTooltip("Back\nTurns the panel back over to its front. Esc does the same.");
    if (onPageChanged)
        onPageChanged();
}

void DetailLayer::dropPage()
{
    if (m_page == nullptr)
        return;
    // Out of the window's lists while it is still alive, then gone.
    auto going = std::move(m_page);
    removeChildComponent(going.get());
    if (onPageChanged)
        onPageChanged();
    going.reset();
}

void DetailLayer::open(Layers::Target target, juce::Colour accent)
{
    if (m_open && target == m_target)
        return;
    const bool swap = m_open || m_page != nullptr;
    dropPage();
    m_target = target;
    m_open = true;
    makePage(accent);
    // Opening: the front folds away, then the layer comes in. Swapping one layer for another: only the new page
    // fades in (the front stays away).
    if (swap && showsLayer())
        m_fold.snap(0.5f);
    m_fold.to(1.0f, c_foldMs);
    setVisible(true);
    applyFold();
    if (onOpened)
        onOpened(target);
}

void DetailLayer::close()
{
    if (! m_open)
        return;
    m_open = false;
    m_fold.to(0.0f, c_foldMs);
    applyFold();
    if (onClosed)
        onClosed(m_target);
}

void DetailLayer::finishFold()
{
    m_fold.snap(m_fold.target());
    applyFold();
}

bool DetailLayer::step(float dt)
{
    const bool side = m_shownSide;
    const float alpha = m_shownAlpha;
    if (m_fold.moving())
    {
        m_fold.step(dt);
        applyFold();
    }
    // The page's own looks and readouts, about 30 times a second while it shows.
    if (m_page != nullptr && showsLayer() && (++m_refreshTick & 1) == 0)
    {
        m_page->refresh();
        // Reset has something to do only while something on the layer is off its default.
        m_reset.setEnabled(Layers::isOffDefault(m_processor.getState(), m_target));
    }
    return side != m_shownSide || std::abs(alpha - m_shownAlpha) > 1.0e-4f;
}

void DetailLayer::applyFold()
{
    const bool layer = showsLayer();
    const float alpha = sideAlpha();
    m_shownSide = layer;
    m_shownAlpha = alpha;
    // Folded shut and at rest: the page goes, and the host with it.
    if (! layer && ! m_open && ! m_fold.moving())
    {
        dropPage();
        setVisible(false);
        return;
    }
    // The front's half of the fold: the host is there but shows nothing (the page waits, invisible, so no cable can
    // find a wheel on it). The layer's half: the page and the keys fade in.
    setAlpha(layer ? alpha : 0.0f);
    for (auto* c : getChildren())
        c->setVisible(layer);
    setVisible(true);
}

void DetailLayer::resetDetail()
{
    if (! m_open)
        return;
    const auto ids = Layers::detailIds(m_target);
    auto& state = m_processor.getState();
    EditHistory::Group group(m_processor.getHistory(), "Reset " + Layers::title(m_target).replace(" / ", " "));
    for (const auto& id : ids)
        if (auto* param = state.getParameter(id); param != nullptr && ! juce::exactlyEqual(param->getValue(), param->getDefaultValue()))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost(param->getDefaultValue());
            param->endChangeGesture();
        }
    if (m_page != nullptr)
        m_page->refresh();
}

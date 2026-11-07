#include "ModPanel.h"

namespace TC = Theme::Colours;

namespace
{
    constexpr int c_history = 120;
    constexpr float c_jackRadius = 9.0f;
    constexpr float c_plugRadius = 5.5f;

    juce::String knobCaption(ModMatrix::Type t)
    {
        switch (t)
        {
            case ModMatrix::Type::Lfo:
            case ModMatrix::Type::Random:   return "Rate";
            case ModMatrix::Type::Follower: return "Speed";
            case ModMatrix::Type::Macro:    return "Amount";
        }
        return {};
    }

    juce::String typeTip(ModMatrix::Type t)
    {
        switch (t)
        {
            case ModMatrix::Type::Lfo:      return "Modulator type: LFO\nA steady repeating wave that moves each wired knob up and down around where you set it. Pick its shape below.";
            case ModMatrix::Type::Random:   return "Modulator type: Random\nA smooth random wander up and down around each wired knob, never the same twice.";
            case ModMatrix::Type::Follower: return "Modulator type: Follower\nFollows the loudness of the output: the louder the rig gets, the further it turns each wired knob (one way only).";
            case ModMatrix::Type::Macro:    return "Modulator type: Macro\nOne knob that turns every knob it is wired to at once, each by its own depth. Great to automate from your DAW.";
        }
        return {};
    }
}

juce::Colour ModPanel::sourceColour(int source)
{
    static const juce::Colour colours[ModMatrix::c_numSources] = {
        juce::Colour(0xffffd166), // sun yellow
        juce::Colour(0xffff6b9a), // hibiscus pink
        juce::Colour(0xff5ee0a8), // palm green
        juce::Colour(0xffa78bff), // twilight violet
    };
    return colours[juce::jlimit(0, ModMatrix::c_numSources - 1, source)];
}

//==============================================================================
class ModPanel::Card : public juce::Component, public juce::TooltipClient
{
public:
    Card(ModPanel& owner, int source)
        : m_owner(owner), m_source(source), m_knob(owner.m_fonts, sourceColour(source)),
          m_typeAttachment(owner.m_processor.getState(), ParamIDs::modType[source], m_type),
          m_shapeAttachment(owner.m_processor.getState(), ParamIDs::modShape[source], m_shape),
          m_knobAttachment(owner.m_processor.getState(), ParamIDs::modKnob[source], m_knob)
    {
        m_type.addItemList({ "LFO", "Random", "Follower", "Macro" }, 1);
        m_shape.addItemList({ "Sine", "Triangle", "Ramp", "Square", "Steps" }, 1);
        // Attachments set the selection before the items existed: sync once more.
        m_type.setSelectedItemIndex(juce::roundToInt(owner.m_processor.getState().getRawParameterValue(ParamIDs::modType[source])->load()),
                                    juce::dontSendNotification);
        m_shape.setSelectedItemIndex(juce::roundToInt(owner.m_processor.getState().getRawParameterValue(ParamIDs::modShape[source])->load()),
                                     juce::dontSendNotification);
        m_type.onChange = [this] { updateType(); };
        m_shape.setTooltip("LFO shape\nSine and Triangle are smooth, Ramp rises then drops, Square jumps between two values, Steps jumps to a new random value every cycle.");
        m_clear.setTooltip("Clear\nPulls out every wire of this modulator. The knobs go back to just where you set them.");
        m_clear.setWantsKeyboardFocus(false);
        m_clear.onClick = [this]
        {
            m_owner.m_processor.getMods().clearSource(m_source);
        };
        m_knob.setDoubleClickReturnValue(true, 0.4);
        for (auto* c : std::initializer_list<juce::Component*> { &m_type, &m_shape, &m_knob, &m_clear })
            addAndMakeVisible(c);
        updateType();
    }

    void updateType()
    {
        const auto t = type();
        m_knob.caption = knobCaption(t);
        m_knob.setTooltip(t == ModMatrix::Type::Macro ? "Amount\nThe macro's value: turn it and every wired knob follows, each by its own depth.\nDouble-click to reset it."
                          : t == ModMatrix::Type::Follower ? "Speed\nHow quickly it follows the output: slow is smooth and gentle, fast is jumpy and follows every hit.\nDouble-click to reset it."
                                                           : "Rate\nHow fast it moves, from one cycle every 50 seconds (0.02 Hz) to 12 times a second.\nDouble-click to reset it.");
        m_type.setTooltip(typeTip(t));
        m_shape.setVisible(t == ModMatrix::Type::Lfo);
        m_knob.repaint();
        repaint();
        // Bipolar (LFO / Random) or one-way (Follower / Macro) changes the arcs on the wired knobs.
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<ModWires>(&m_owner.m_wires)] { if (safe != nullptr) safe->rebuild(); });
    }

    ModMatrix::Type type() const
    {
        return static_cast<ModMatrix::Type>(juce::jlimit(0, 3, m_type.getSelectedItemIndex()));
    }

    juce::Rectangle<float> jackArea() const { return { 12.0f, 10.0f, c_jackRadius * 2.0f, c_jackRadius * 2.0f }; }

    juce::String getTooltip() override
    {
        const auto pos = getMouseXYRelative();
        if (jackArea().expanded(4.0f).contains(pos.toFloat()))
            return sourceName(m_source) + " jack\nDrag from here onto any knob to wire this modulator to it. Drag again to wire it to more knobs. "
                   "Every knob can be wired except the morph pad and the track faders.\nDrop it back here (or anywhere empty) to cancel.";
        if (m_scopeArea.contains(pos))
            return sourceName(m_source) + " output\nWhat this modulator is doing right now, newest at the right.";
        return {};
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(10, 8);
        auto top = r.removeFromTop(22);
        top.removeFromLeft(static_cast<int>(c_jackRadius * 2.0f) + 8 + 60);
        m_type.setBounds(top.removeFromRight(juce::jmin(104, top.getWidth())));
        r.removeFromTop(6);
        auto bottom = r.removeFromBottom(22);
        m_clear.setBounds(bottom.removeFromRight(54));
        m_shape.setBounds(bottom.removeFromLeft(juce::jmin(104, bottom.getWidth())));
        r.removeFromBottom(6);
        m_knob.setBounds(r.removeFromRight(juce::jmin(78, r.getWidth())));
        r.removeFromRight(8);
        m_scopeArea = r;
    }

    void paint(juce::Graphics& g) override
    {
        const auto colour = sourceColour(m_source);
        const auto& fonts = m_owner.m_fonts;
        auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(TC::panelRaised);
        g.fillRoundedRectangle(bounds, 6.0f);
        g.setColour(colour.withAlpha(0.45f));
        g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

        // The jack: a brass socket in the modulator's colour. Drag it onto a knob.
        const auto jack = jackArea();
        g.setColour(TC::scope);
        g.fillEllipse(jack);
        g.setColour(colour);
        g.drawEllipse(jack.reduced(1.0f), 2.2f);
        g.fillEllipse(jack.reduced(c_jackRadius * 0.55f));
        if (m_dragging)
        {
            g.setColour(colour.withAlpha(0.3f));
            g.fillEllipse(jack.expanded(4.0f));
        }

        g.setFont(fonts.display(19.0f));
        g.setColour(colour);
        g.drawText(sourceName(m_source), juce::Rectangle<float>(jack.getRight() + 8.0f, 7.0f, 70.0f, 24.0f), juce::Justification::centredLeft);

        // Scope: what the modulator is doing (the last 4 s)
        const auto sa = m_scopeArea.toFloat();
        g.setColour(TC::scope);
        g.fillRoundedRectangle(sa, 4.0f);
        const bool bipolar = type() == ModMatrix::Type::Lfo || type() == ModMatrix::Type::Random;
        if (bipolar)
        {
            g.setColour(TC::grid);
            g.drawHorizontalLine(static_cast<int>(sa.getCentreY()), sa.getX() + 4.0f, sa.getRight() - 4.0f);
        }
        juce::Path p;
        for (int k = 0; k < c_history; ++k)
        {
            const float v = m_history[static_cast<size_t>((m_head + k) % c_history)];
            const float n = bipolar ? 0.5f + 0.5f * v : v;
            const float x = sa.getX() + 4.0f + (sa.getWidth() - 8.0f) * static_cast<float>(k) / static_cast<float>(c_history - 1);
            const float y = sa.getBottom() - 5.0f - (sa.getHeight() - 10.0f) * juce::jlimit(0.0f, 1.0f, n);
            if (k == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
        g.setColour(colour.withAlpha(0.25f));
        g.strokePath(p, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(colour);
        g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const int wires = m_owner.m_processor.getMods().numRoutes(m_source);
        g.setFont(fonts.label(10.5f));
        g.setColour(wires > 0 ? TC::text : TC::textFaint);
        const auto infoArea = juce::Rectangle<int>(m_shape.isVisible() ? m_shape.getRight() + 8 : 10, m_clear.getY(),
                                                   m_clear.getX() - (m_shape.isVisible() ? m_shape.getRight() + 8 : 10) - 4, m_clear.getHeight());
        g.drawText(wires == 0 ? juce::String("drag the jack to a knob") : juce::String(wires) + (wires == 1 ? " wire" : " wires"),
                   infoArea, juce::Justification::centredLeft, true);
    }

    void push()
    {
        m_history[static_cast<size_t>(m_head)] = m_owner.m_processor.getMods().getSourceValue(m_source);
        m_head = (m_head + 1) % c_history;
        repaint(m_scopeArea);
        const int wires = m_owner.m_processor.getMods().numRoutes(m_source);
        if (wires != m_wires)
        {
            m_wires = wires;
            m_clear.setEnabled(wires > 0);
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (! jackArea().expanded(4.0f).contains(e.position))
            return;
        m_dragging = true;
        m_owner.m_wires.beginWire(m_source, m_owner.m_wires.getLocalPoint(this, jackArea().getCentre()));
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (m_dragging)
            m_owner.m_wires.dragWire(m_owner.m_wires.getLocalPoint(this, e.position));
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (! m_dragging)
            return;
        m_dragging = false;
        m_owner.m_wires.endWire(m_owner.m_wires.getLocalPoint(this, e.position));
        repaint();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        setMouseCursor(jackArea().expanded(4.0f).contains(e.position) ? juce::MouseCursor::DraggingHandCursor
                                                                       : juce::MouseCursor::NormalCursor);
    }

private:
    ModPanel& m_owner;
    int m_source;
    juce::ComboBox m_type, m_shape;
    WheelKnob m_knob;
    juce::TextButton m_clear { "CLEAR" };
    juce::AudioProcessorValueTreeState::ComboBoxAttachment m_typeAttachment, m_shapeAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment m_knobAttachment;
    juce::Rectangle<int> m_scopeArea;
    std::array<float, c_history> m_history {};
    int m_head = 0;
    int m_wires = -1;
    bool m_dragging = false;
};

//==============================================================================
ModPanel::ModPanel(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts, ModWires& wires)
    : m_processor(processor), m_fonts(fonts), m_wires(wires)
{
    for (int s = 0; s < ModMatrix::c_numSources; ++s)
    {
        m_cards[static_cast<size_t>(s)] = std::make_unique<Card>(*this, s);
        addAndMakeVisible(*m_cards[static_cast<size_t>(s)]);
    }
    startTimerHz(30);
}

ModPanel::~ModPanel() = default;

void ModPanel::paint(juce::Graphics&) {}

void ModPanel::resized()
{
    auto r = getLocalBounds();
    const int gap = 12;
    const int w = (r.getWidth() - gap * (ModMatrix::c_numSources - 1)) / ModMatrix::c_numSources;
    for (auto& c : m_cards)
    {
        c->setBounds(r.removeFromLeft(w));
        r.removeFromLeft(gap);
    }
}

juce::Point<float> ModPanel::getJackCentre(int source) const
{
    const auto& c = *m_cards[static_cast<size_t>(source)];
    return c.getBoundsInParent().toFloat().getTopLeft() + c.jackArea().getCentre();
}

void ModPanel::timerCallback()
{
    for (auto& c : m_cards)
        c->push();
    m_wires.updateKnobs();
}

//==============================================================================
ModWires::ModWires(ModMatrix& matrix, const Theme::Fonts& fonts) : m_matrix(matrix), m_fonts(fonts)
{
    setInterceptsMouseClicks(true, false); // only the plugs (see hitTest)
}

juce::Rectangle<float> ModWires::knobArea(WheelKnob* knob) const
{
    return getLocalArea(knob, knob->getLocalBounds()).toFloat();
}

WheelKnob* ModWires::knobAt(juce::Point<float> p, juce::String* id) const
{
    // The nearest dial wins (neighbouring capture zones overlap a little in the bus rows).
    WheelKnob* best = nullptr;
    float bestDistance = 0.0f;
    for (const auto& [paramId, knob] : m_knobs)
    {
        if (knob == nullptr || ! knob->isShowing())
            continue;
        const float d = getLocalPoint(knob, knob->getDialCentre()).getDistanceFrom(p);
        if (d <= knob->getDialRadius() + 10.0f && (best == nullptr || d < bestDistance))
        {
            best = knob;
            bestDistance = d;
            if (id != nullptr) *id = paramId;
        }
    }
    return best;
}

juce::Path ModWires::cablePath(juce::Point<float> from, juce::Point<float> to) const
{
    // A hanging cable: both handles pulled down by a sag that grows with the span.
    const float span = from.getDistanceFrom(to);
    const float sag = 24.0f + span * 0.22f;
    juce::Path p;
    p.startNewSubPath(from);
    p.cubicTo(from + (to - from) * 0.25f + juce::Point<float>(0.0f, sag),
              from + (to - from) * 0.75f + juce::Point<float>(0.0f, sag * 0.8f), to);
    return p;
}

void ModWires::drawCable(juce::Graphics& g, const juce::Path& p, juce::Colour c, float alpha)
{
    g.setColour(TC::shadow.withAlpha(0.45f * alpha));
    g.strokePath(p, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                 juce::AffineTransform::translation(0.0f, 2.0f));
    g.setColour(c.darker(0.35f).withAlpha(alpha));
    g.strokePath(p, juce::PathStrokeType(3.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(c.brighter(0.25f).withAlpha(alpha * 0.9f));
    g.strokePath(p, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void ModWires::rebuild()
{
    if (m_generation != m_matrix.generation())
    {
        m_generation = m_matrix.generation();
        m_depthSlot = -1; // the slots were renumbered: a drag in progress must not move another wire
    }
    m_plugs.clear();
    // Arcs on every knob, plugs beside the ones with wires.
    std::map<WheelKnob*, std::vector<WheelKnob::ModArc>> arcs;
    std::map<WheelKnob*, int> plugCount;
    for (const auto& r : m_matrix.getRoutes())
    {
        const auto it = m_knobs.find(r.param);
        if (it == m_knobs.end() || it->second == nullptr)
            continue;
        auto* knob = it->second;
        const bool bipolar = isSourceBipolar ? isSourceBipolar(r.source) : m_matrix.isBipolar(r.source);
        arcs[knob].push_back({ ModPanel::sourceColour(r.source), r.depth, bipolar });
        // Plugs sit on the dial's upper right, one per wire, around the rim.
        const int k = plugCount[knob]++;
        const float a = juce::degreesToRadians(38.0f + 24.0f * static_cast<float>(k));
        const auto centre = getLocalPoint(knob, knob->getDialCentre());
        const float rad = knob->getDialRadius() + 5.0f;
        m_plugs.push_back({ r, centre + juce::Point<float>(std::sin(a), -std::cos(a)) * rad, knob });
    }
    for (const auto& [id, knob] : m_knobs)
        if (knob != nullptr)
        {
            const auto it = arcs.find(knob);
            knob->setModArcs(it != arcs.end() ? it->second : std::vector<WheelKnob::ModArc> {});
        }

    // The cables, drawn once into an image at the screen's resolution.
    const float scale = m_scale; // physical pixels per unit, learnt in paint
    const int iw = juce::jmax(1, juce::roundToInt(static_cast<float>(getWidth()) * scale));
    const int ih = juce::jmax(1, juce::roundToInt(static_cast<float>(getHeight()) * scale));
    if (m_cables.getWidth() != iw || m_cables.getHeight() != ih)
        m_cables = juce::Image(juce::Image::ARGB, iw, ih, true);
    else
        m_cables.clear(m_cables.getBounds());
    m_hasCables = m_showCables && m_panel != nullptr && ! m_plugs.empty();
    if (m_hasCables)
    {
        juce::Graphics g(m_cables);
        g.addTransform(juce::AffineTransform::scale(scale));
        for (const auto& plug : m_plugs)
        {
            const auto jack = getLocalPoint(m_panel, m_panel->getJackCentre(plug.route.source));
            drawCable(g, cablePath(jack, plug.pos), ModPanel::sourceColour(plug.route.source), 0.62f);
        }
    }
    repaint();
}

void ModWires::updateArcs(WheelKnob* knob)
{
    std::vector<WheelKnob::ModArc> arcs;
    for (const auto& plug : m_plugs)
        if (plug.knob == knob)
            arcs.push_back({ ModPanel::sourceColour(plug.route.source), plug.route.depth,
                             isSourceBipolar ? isSourceBipolar(plug.route.source) : m_matrix.isBipolar(plug.route.source) });
    knob->setModArcs(std::move(arcs));
}

void ModWires::depthChanged(int slot, float depth)
{
    for (auto& plug : m_plugs)
        if (plug.route.slot == slot)
        {
            plug.route.depth = depth;
            updateArcs(plug.knob);
            repaint(juce::Rectangle<float>(60.0f, 50.0f).withCentre(plug.pos.translated(0.0f, -10.0f)).getSmallestIntegerContainer());
        }
}

void ModWires::updateKnobs()
{
    for (const auto& [id, knob] : m_knobs)
        if (knob != nullptr && knob->isModulated())
        {
            const int t = ModMatrix::targetIndex(id);
            if (t >= 0)
                knob->setModLive(m_matrix.getModulatedView(t));
        }
}

void ModWires::paint(juce::Graphics& g)
{
    // Cable image resolution: the physical scale in half steps (so a live window resize doesn't redraw it every frame).
    const float scale = juce::jlimit(1.0f, 3.0f, std::ceil(g.getInternalContext().getPhysicalPixelScaleFactor() * 2.0f) / 2.0f);
    if (std::abs(scale - m_scale) > 0.01f)
    {
        m_scale = scale;
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<ModWires>(this)] { if (safe != nullptr) safe->rebuild(); });
    }
    if (m_hasCables)
    {
        // One fixed transform for every repaint (the context only touches the clip region), so no seams or shimmer.
        g.drawImageTransformed(m_cables, juce::AffineTransform::scale(static_cast<float>(getWidth()) / static_cast<float>(juce::jmax(1, m_cables.getWidth())),
                                                                      static_cast<float>(getHeight()) / static_cast<float>(juce::jmax(1, m_cables.getHeight()))));
    }

    for (const auto& plug : m_plugs)
    {
        const auto c = ModPanel::sourceColour(plug.route.source);
        const auto r = juce::Rectangle<float>(c_plugRadius * 2.0f, c_plugRadius * 2.0f).withCentre(plug.pos);
        g.setColour(TC::shadow.withAlpha(0.6f));
        g.fillEllipse(r.translated(0.0f, 1.5f));
        g.setColour(c);
        g.fillEllipse(r);
        g.setColour(plug.route.depth < 0.0f ? TC::background : TC::text.withAlpha(0.9f));
        g.drawEllipse(r.reduced(0.8f), 1.2f);
        if (plug.route.slot == m_depthSlot)
        {
            const auto text = juce::String(plug.route.depth > 0.0f ? "+" : "") + juce::String(juce::roundToInt(plug.route.depth * 100.0f)) + "%";
            auto bubble = juce::Rectangle<float>(48.0f, 18.0f).withCentre(plug.pos.translated(0.0f, -18.0f));
            g.setColour(TC::panelRaised);
            g.fillRoundedRectangle(bubble, 4.0f);
            g.setColour(c);
            g.drawRoundedRectangle(bubble, 4.0f, 1.0f);
            g.setFont(m_fonts.label(11.0f));
            g.drawText(text, bubble, juce::Justification::centred);
        }
    }

    if (m_dragSource >= 0)
    {
        const auto c = ModPanel::sourceColour(m_dragSource);
        drawCable(g, cablePath(m_dragFrom, m_dragTo), c, 0.95f);
        g.setColour(c);
        g.fillEllipse(juce::Rectangle<float>(c_plugRadius * 2.4f, c_plugRadius * 2.4f).withCentre(m_dragTo));
    }
}

bool ModWires::hitTest(int x, int y)
{
    return plugAt({ static_cast<float>(x), static_cast<float>(y) }) != nullptr;
}

const ModWires::Plug* ModWires::plugAt(juce::Point<float> p) const
{
    for (const auto& plug : m_plugs)
        if (plug.pos.getDistanceFrom(p) <= c_plugRadius + 3.0f)
            return &plug;
    return nullptr;
}

void ModWires::beginWire(int source, juce::Point<float> from)
{
    m_dragSource = source;
    m_dragFrom = from;
    m_dragTo = from;
    repaint();
}

void ModWires::dragWire(juce::Point<float> to)
{
    if (m_dragSource < 0)
        return;
    const auto old = cablePath(m_dragFrom, m_dragTo).getBounds().expanded(12.0f);
    m_dragTo = to;
    auto* over = knobAt(to);
    if (over != m_dragOver)
    {
        if (m_dragOver != nullptr) m_dragOver->setWireHover(juce::Colours::transparentBlack);
        m_dragOver = over;
        if (m_dragOver != nullptr) m_dragOver->setWireHover(ModPanel::sourceColour(m_dragSource));
    }
    repaint(old.getUnion(cablePath(m_dragFrom, m_dragTo).getBounds().expanded(12.0f)).getSmallestIntegerContainer());
}

void ModWires::endWire(juce::Point<float> at)
{
    if (m_dragSource < 0)
        return;
    if (m_dragOver != nullptr)
        m_dragOver->setWireHover(juce::Colours::transparentBlack);
    m_dragOver = nullptr;
    juce::String id;
    if (knobAt(at, &id) != nullptr)
    {
        const int slot = m_matrix.connect(m_dragSource, id, 0.5f);
        if (onStatus)
            onStatus(slot >= 0 ? ModPanel::sourceName(m_dragSource) + " wired. Drag its plug up or down for depth, right-click to pull it out."
                               : juce::String("No more room for wires (") + juce::String(ModMatrix::c_maxRoutes) + " max).");
    }
    m_dragSource = -1;
    rebuild();
}

void ModWires::mouseDown(const juce::MouseEvent& e)
{
    const auto* plug = plugAt(e.position);
    if (plug == nullptr)
        return;
    if (e.mods.isPopupMenu())
    {
        showPlugMenu(*plug);
        return;
    }
    if (e.mods.isAltDown())
    {
        m_matrix.disconnect(plug->route.slot, m_generation);
        return;
    }
    m_depthSlot = plug->route.slot;
    m_depthStart = plug->route.depth;
    m_depthPos = plug->pos;
    repaint(juce::Rectangle<float>(60.0f, 50.0f).withCentre(m_depthPos.translated(0.0f, -10.0f)).getSmallestIntegerContainer());
}

void ModWires::mouseDrag(const juce::MouseEvent& e)
{
    if (m_depthSlot < 0)
        return;
    const float perPixel = e.mods.isShiftDown() ? 0.001f : 0.006f;
    const float depth = juce::jlimit(-1.0f, 1.0f, m_depthStart - static_cast<float>(e.getDistanceFromDragStartY()) * perPixel);
    if (m_matrix.setDepth(m_depthSlot, depth, m_generation))
        depthChanged(m_depthSlot, depth);
}

void ModWires::mouseUp(const juce::MouseEvent&)
{
    if (m_depthSlot < 0)
        return;
    m_depthSlot = -1;
    repaint(juce::Rectangle<float>(60.0f, 50.0f).withCentre(m_depthPos.translated(0.0f, -10.0f)).getSmallestIntegerContainer());
}

void ModWires::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (const auto* plug = plugAt(e.position))
        if (m_matrix.setDepth(plug->route.slot, 0.5f, m_generation))
            depthChanged(plug->route.slot, 0.5f);
}

void ModWires::showPlugMenu(const Plug& plug)
{
    const auto route = plug.route;
    juce::PopupMenu menu;
    menu.addSectionHeader(describe(route).toUpperCase());
    menu.addItem(1, "Invert");
    menu.addItem(2, "Depth 25%");
    menu.addItem(3, "Depth 50%");
    menu.addItem(4, "Depth 100%");
    menu.addSeparator();
    menu.addItem(9, "Pull out the wire");
    const int generation = m_generation;
    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetComponent(this) // closes with the editor
                           .withTargetScreenArea(localAreaToGlobal(juce::Rectangle<float>(14.0f, 14.0f).withCentre(plug.pos).getSmallestIntegerContainer())),
                       [safe = juce::Component::SafePointer<ModWires>(this), route, generation](int result)
                       {
                           if (safe == nullptr || result == 0)
                               return;
                           const float sign = route.depth < 0.0f ? -1.0f : 1.0f;
                           const float depths[] = { -route.depth, 0.25f * sign, 0.5f * sign, 1.0f * sign };
                           if (result == 9)
                               safe->m_matrix.disconnect(route.slot, generation);
                           else if (result >= 1 && result <= 4 && safe->m_matrix.setDepth(route.slot, depths[result - 1], generation))
                               safe->depthChanged(route.slot, depths[result - 1]);
                       });
}

juce::String ModWires::describe(const ModMatrix::Route& r) const
{
    juce::String name = r.param;
    const auto it = m_knobs.find(r.param);
    if (it != m_knobs.end() && it->second != nullptr)
        name = it->second->caption;
    return ModPanel::sourceName(r.source) + " -> " + name + "  " + (r.depth > 0.0f ? "+" : "")
         + juce::String(juce::roundToInt(r.depth * 100.0f)) + "%";
}

juce::String ModWires::getTooltip()
{
    const auto pos = getMouseXYRelative().toFloat();
    if (const auto* plug = plugAt(pos))
        return describe(plug->route) + "\nA wire: this modulator moves this knob, by the depth shown (minus turns it the other way).\n"
               "Drag up or down for depth (Shift = fine). Double-click for +50%. Right-click to invert or pull it out (or Alt-click).";
    return {};
}

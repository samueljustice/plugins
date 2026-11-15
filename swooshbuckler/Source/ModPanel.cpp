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
            case ModMatrix::Type::Lfo:      return "Modulator Type: LFO\nA steady repeating wave that moves each wired knob up and down around where you set it. Pick its shape below.";
            case ModMatrix::Type::Random:   return "Modulator Type: Random\nA smooth random wander up and down around each wired knob, never the same twice.";
            case ModMatrix::Type::Follower: return "Modulator Type: Follower\nFollows the loudness of bus A, bus B, the hits or the output, or the Master track's loudness, pitch or brightness (choose which underneath): the more there is of it, the further it turns each wired knob (one way only).";
            case ModMatrix::Type::Macro:    return "Modulator Type: Macro\nOne knob that turns every knob it is wired to at once, each by its own depth. Great to automate from your DAW.";
        }
        return {};
    }
}

juce::Colour ModPanel::sourceColour(int source)
{
    // The modulators' own family (Theme::modColour): none is a section's colour or the brass.
    return Theme::modColour(juce::jlimit(0, ModMatrix::c_numSources - 1, source));
}

//==============================================================================
class ModPanel::Card : public juce::Component, public juce::TooltipClient
{
public:
    Card(ModPanel& owner, int source)
        : m_owner(owner), m_source(source), m_knob(owner.m_fonts, sourceColour(source), WheelKnob::Size::Secondary),
          m_typeAttachment(owner.m_processor.getState(), ParamIDs::modType[source], m_type),
          m_shapeAttachment(owner.m_processor.getState(), ParamIDs::modShape[source], m_shape),
          m_followAttachment(owner.m_processor.getState(), ParamIDs::modFollow[source], m_follow),
          m_knobAttachment(owner.m_processor.getState(), ParamIDs::modKnob[source], m_knob)
    {
        m_type.addItemList({ "LFO", "Random", "Follower", "Macro" }, 1);
        m_shape.addItemList({ "Sine", "Triangle", "Ramp", "Square", "Steps" }, 1);
        m_follow.addItemList(ModMatrix::followerNames(), 1);
        // Attachments set the selection before the items existed: sync once more.
        m_type.setSelectedItemIndex(juce::roundToInt(owner.m_processor.getState().getRawParameterValue(ParamIDs::modType[source])->load()),
                                    juce::dontSendNotification);
        m_shape.setSelectedItemIndex(juce::roundToInt(owner.m_processor.getState().getRawParameterValue(ParamIDs::modShape[source])->load()),
                                     juce::dontSendNotification);
        m_follow.setSelectedItemIndex(juce::roundToInt(owner.m_processor.getState().getRawParameterValue(ParamIDs::modFollow[source])->load()),
                                      juce::dontSendNotification);
        m_follow.setTooltip("Follows\nWhat the follower listens to: bus A, bus B or the hits on their own, or the whole output. "
                            "Follow one bus to make the other react to it, or follow the hits to make every impact kick a knob.\n"
                            "Master, Master Pitch and Master Brightness follow the Master track (the lane with the lit star): how loud, "
                            "how high and how bright that sound is at the moment it is playing, each over the sound's own range. "
                            "They rest until a track is made the Master.");
        m_type.onChange = [this] { updateType(); };
        m_shape.setTooltip("LFO Shape\nSine and Triangle are smooth, Ramp rises then drops, Square jumps between two values, Steps jumps to a new random value every cycle.");
        m_clear.setTooltip("Clear\nPulls out every wire of this modulator. The knobs go back to just where you set them.");
        m_clear.setWantsKeyboardFocus(false);
        m_clear.onClick = [this]
        {
            m_owner.m_processor.getMods().clearSource(m_source);
        };
        if (auto* param = owner.m_processor.getState().getParameter(ParamIDs::modKnob[source]))
            m_knob.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));
        for (auto* c : std::initializer_list<juce::Component*> { &m_type, &m_shape, &m_follow, &m_knob, &m_clear })
            addAndMakeVisible(c);
        updateType();
    }

    void updateType()
    {
        const auto t = type();
        m_knob.caption = knobCaption(t);
        m_knob.setTooltip(t == ModMatrix::Type::Macro ? "Amount\nThe macro's value: turn it and every wired knob follows, each by its own depth."
                          : t == ModMatrix::Type::Follower ? "Speed\nHow quickly it follows its input: slow is smooth and gentle, fast is jumpy and follows every hit."
                                                           : "Rate\nHow fast it moves, from one cycle every 20 seconds to 8 times a second.");
        // The readout in the knob's own units: the cycle time or rate, the follow time, or the amount.
        m_knob.textFromValueFunction = [t](double v)
        {
            const float k = static_cast<float>(v);
            if (t == ModMatrix::Type::Macro)
                return juce::String(juce::roundToInt(k * 100.0f)) + "%";
            if (t == ModMatrix::Type::Follower)
            {
                const float sec = ModMatrix::followerSeconds(k);
                return sec < 1.0f ? juce::String(juce::roundToInt(sec * 1000.0f)) + " ms" : juce::String(sec, 1) + " s";
            }
            const float hz = ModMatrix::rateHz(k);
            return hz < 1.0f ? juce::String(1.0f / hz, 1) + " s" : juce::String(hz, 1) + " Hz";
        };
        m_type.setTooltip(typeTip(t));
        m_shape.setVisible(t == ModMatrix::Type::Lfo);
        m_follow.setVisible(t == ModMatrix::Type::Follower);
        m_knob.valueTextChanged();
        repaint();
        // Bipolar (LFO / Random) or one-way (Follower / Macro) changes the arcs on the wired knobs.
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<ModWires>(&m_owner.m_wires)] { if (safe != nullptr) safe->rebuild(); });
    }

    ModMatrix::Type type() const
    {
        return static_cast<ModMatrix::Type>(juce::jlimit(0, 3, m_type.getSelectedItemIndex()));
    }

    juce::Rectangle<float> jackArea() const { return { 12.0f, 7.0f, c_jackRadius * 2.0f, c_jackRadius * 2.0f }; }

    juce::String getTooltip() override
    {
        const auto pos = getMouseXYRelative();
        if (jackArea().expanded(4.0f).contains(pos.toFloat()))
            return sourceName(m_source) + " Jack\nDrag from here onto any knob to wire this modulator to it. Drag again to wire it to more knobs. "
                   "Every knob can be wired except the morph pad and the track faders.\nDrop it back here (or anywhere empty) to cancel.";
        if (m_scopeArea.contains(pos))
            return sourceName(m_source) + " Output\nWhat this modulator is doing right now, newest at the right.";
        return {};
    }

    // Every type is laid out the same way. Along the top: the jack, the name, the wires it has, Clear. Under that:
    // the type and (for an LFO or a Follower) its second menu at the left, the scope in the middle, the knob at the right.
    void resized() override
    {
        auto r = getLocalBounds().reduced(8, 6);
        auto knob = r.removeFromRight(WheelKnob::cellWidth(WheelKnob::Size::Secondary) + 10);
        m_knob.setBounds(knob.withSizeKeepingCentre(knob.getWidth(), juce::jmin(knob.getHeight(), WheelKnob::cellHeight(WheelKnob::Size::Secondary))));
        r.removeFromRight(6);
        auto top = r.removeFromTop(20);
        m_clear.setBounds(top.removeFromRight(48));
        top.removeFromRight(6);
        m_nameArea = top.withTrimmedLeft(static_cast<int>(c_jackRadius * 2.0f) + 12).withWidth(52);
        m_summaryArea = top.withLeft(m_nameArea.getRight() + 6);
        r.removeFromTop(6);
        auto menus = r.removeFromLeft(88);
        m_type.setBounds(menus.removeFromTop(22));
        menus.removeFromTop(6);
        m_shape.setBounds(menus.removeFromTop(22));
        m_follow.setBounds(m_shape.getBounds());
        r.removeFromLeft(8);
        m_scopeArea = r;
    }

    void paint(juce::Graphics& g) override
    {
        const auto colour = sourceColour(m_source);
        const auto& fonts = m_owner.m_fonts;
        // A plate of its own, owned by its colour: a bar down its left edge.
        const auto bounds = getLocalBounds().toFloat();
        g.setColour(TC::raised.withAlpha(0.6f));
        Theme::Plate::fill(g, bounds, Theme::Cut::control);
        Theme::Plate::edge(g, bounds, Theme::Cut::control, TC::edgeDark.withAlpha(0.8f), TC::edgeLight.withMultipliedAlpha(0.5f));
        g.setColour(colour);
        g.fillRect(juce::Rectangle<float>(bounds.getX() + 2.0f, bounds.getY() + 32.0f, 3.0f, bounds.getHeight() - 40.0f));

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

        Theme::stampText(g, sourceName(m_source), m_nameArea.toFloat().expanded(0.0f, 3.0f), fonts.display(), juce::Justification::centredLeft, colour);

        // What it is wired to: "1 wire - Bus B Swirl".
        g.setColour(m_wires > 0 ? TC::text : TC::textFaint);
        Theme::fitText(g, m_summary, m_summaryArea, fonts.caption(), juce::Justification::centredLeft, Theme::Fit::Message);

        // Scope: what the modulator is doing (the last 4 s)
        const auto sa = m_scopeArea.toFloat();
        SwooshLookAndFeel::drawWell(g, sa);
        const bool bipolar = type() == ModMatrix::Type::Lfo || type() == ModMatrix::Type::Random;
        if (bipolar)
        {
            g.setColour(TC::grid);
            g.drawHorizontalLine(static_cast<int>(sa.getCentreY()), sa.getX() + 4.0f, sa.getRight() - 4.0f);
        }
        auto& p = m_path; // kept between frames
        p.clear();
        for (int k = 0; k < c_history; ++k)
        {
            const float v = m_history[static_cast<size_t>((m_head + k) % c_history)];
            const float n = bipolar ? 0.5f + 0.5f * v : v;
            const float x = sa.getX() + 4.0f + (sa.getWidth() - 8.0f) * static_cast<float>(k) / static_cast<float>(c_history - 1);
            const float y = sa.getBottom() - 5.0f - (sa.getHeight() - 10.0f) * juce::jlimit(0.0f, 1.0f, n);
            if (k == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
        g.setColour(colour.withAlpha(0.25f));
        g.strokePath(p, juce::PathStrokeType(5.0f, juce::PathStrokeType::beveled, juce::PathStrokeType::butt));
        g.setColour(colour);
        g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::beveled, juce::PathStrokeType::butt));
    }

    /** The line beside the name: how many wires, and what they land on (by the parameters' own names). */
    void updateSummary()
    {
        auto& mods = m_owner.m_processor.getMods();
        juce::String text;
        int count = 0;
        for (const auto& route : mods.getRoutes())
        {
            if (route.source != m_source)
                continue;
            juce::String name = route.param;
            if (auto* param = m_owner.m_processor.getState().getParameter(route.param))
                name = param->getName(48);
            text << (count++ == 0 ? "" : ", ") << name;
        }
        text = count == 0 ? juce::String("No wires") : juce::String(count) + (count == 1 ? " wire - " : " wires - ") + text;
        if (text != m_summary)
        {
            m_summary = text;
            repaint(m_summaryArea);
        }
    }

    void push()
    {
        m_history[static_cast<size_t>(m_head)] = m_owner.m_processor.getMods().getSourceValue(m_source);
        m_head = (m_head + 1) % c_history;
        repaint(m_scopeArea);
        const int wires = m_owner.m_processor.getMods().numRoutes(m_source);
        const int generation = m_owner.m_processor.getMods().generation();
        // The summary is written when the wiring changes, and looked at again twice a second (a wire can move
        // from one knob to another without the count changing).
        if (wires != m_wires || generation != m_generation || (++m_summaryTick % 15) == 0)
        {
            if (wires != m_wires)
            {
                m_wires = wires;
                m_clear.setEnabled(wires > 0);
                repaint();
            }
            m_generation = generation;
            updateSummary();
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
    juce::ComboBox m_type, m_shape, m_follow;
    WheelKnob m_knob;
    juce::TextButton m_clear { "Clear" };
    juce::AudioProcessorValueTreeState::ComboBoxAttachment m_typeAttachment, m_shapeAttachment, m_followAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment m_knobAttachment;
    juce::Rectangle<int> m_scopeArea, m_nameArea, m_summaryArea;
    juce::String m_summary { "No wires" };
    juce::Path m_path;
    int m_generation = -1, m_summaryTick = 0;
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
    const int gap = 8;
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
    int hovered = -1;
    for (size_t i = 0; i < m_cards.size(); ++i)
    {
        m_cards[i]->push();
        if (m_cards[i]->isMouseOver(true))
            hovered = static_cast<int>(i);
    }
    m_wires.setHoverCard(hovered); // the modulator under the mouse: its cables stand out
    m_wires.updateKnobs();
}

//==============================================================================
ModWires::ModWires(ModMatrix& matrix, const Theme::Fonts& fonts) : m_matrix(matrix), m_fonts(fonts)
{
    setInterceptsMouseClicks(true, false); // only the plugs (see hitTest)
}

bool ModWires::isTargetInView(const ModTarget* t)
{
    if (const auto* vp = t->findParentComponentOfClass<juce::Viewport>())
        return vp->getLocalBounds().contains(vp->getLocalArea(t, t->getLocalBounds()));
    return true;
}

juce::Rectangle<float> ModWires::knobArea(ModTarget* knob) const
{
    return getLocalArea(knob, knob->getLocalBounds()).toFloat();
}

ModTarget* ModWires::knobAt(juce::Point<float> p, juce::String* id) const
{
    // The nearest dial wins (neighbouring capture zones overlap a little in the bus rows).
    ModTarget* best = nullptr;
    float bestDistance = 0.0f;
    for (const auto& [paramId, knob] : m_knobs)
    {
        if (knob == nullptr || ! knob->isReachable()) // dimmed (e.g. bus B's linked Doppler) or scrolled away: no new wires
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

juce::Path ModWires::cablePath(juce::Point<float> from, juce::Point<float> to, const juce::Point<float>* gate) const
{
    // A hanging cable: both handles pulled down by a sag that grows with the span. One that goes to a knob comes
    // up to the knob's gate (beside its caption and value, never over them) and curves in to its plug from there.
    const auto end = gate != nullptr ? *gate : to;
    const float span = from.getDistanceFrom(end);
    const float sag = 24.0f + span * 0.22f;
    // The modulators are along the bottom of the window, so nearly every cable goes up: it leaves its jack straight
    // up (out of the top of its card, over none of the card's own text) and hangs from there. One that goes
    // across or down hangs from the jack as it always did.
    const bool up = end.y < from.y - 30.0f;
    const auto leave = up ? from + juce::Point<float>((end.x - from.x) * 0.06f, -juce::jlimit(36.0f, 120.0f, (from.y - end.y) * 0.42f))
                          : from + (end - from) * 0.25f + juce::Point<float>(0.0f, sag);
    juce::Path p;
    p.startNewSubPath(from);
    if (gate == nullptr)
    {
        p.cubicTo(leave, from + (to - from) * 0.75f + juce::Point<float>(0.0f, sag * 0.8f), to);
        return p;
    }
    const float rise = juce::jmax(8.0f, end.y - to.y);
    p.cubicTo(leave, end + juce::Point<float>(0.0f, juce::jlimit(40.0f, 130.0f, sag * 0.9f)), end);
    p.cubicTo(end - juce::Point<float>(0.0f, rise * 0.55f), { end.x, to.y }, to);
    return p;
}

void ModWires::drawCable(juce::Graphics& g, const juce::Path& p, juce::Colour c, float alpha)
{
    g.setColour(TC::shadow.withAlpha(0.45f * alpha));
    g.strokePath(p, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), // round object: a cable
                 juce::AffineTransform::translation(0.0f, 2.0f));
    g.setColour(c.darker(0.35f).withAlpha(alpha));
    g.strokePath(p, juce::PathStrokeType(3.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)); // round object: a cable
    g.setColour(c.brighter(0.25f).withAlpha(alpha * 0.9f));
    g.strokePath(p, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)); // round object: a cable
}

void ModWires::rebuild()
{
    if (m_generation != m_matrix.generation())
    {
        m_generation = m_matrix.generation();
        m_depthSlot = -1; // the slots were renumbered: a drag in progress must not move another wire
        m_newestSlot.clear();
    }
    m_plugs.clear();
    // Arcs on every knob, plugs beside the ones with wires.
    std::map<ModTarget*, std::vector<ModTarget::ModArc>> arcs;
    std::map<ModTarget*, int> plugCount;
    for (const auto& r : m_matrix.getRoutes())
    {
        const auto it = m_knobs.find(r.param);
        if (it == m_knobs.end() || it->second == nullptr)
            continue;
        auto* knob = it->second;
        const bool bipolar = isSourceBipolar ? isSourceBipolar(r.source) : m_matrix.isBipolar(r.source);
        arcs[knob].push_back({ ModPanel::sourceColour(r.source), r.depth, bipolar });
        if (! Theme::isShownInWindow(*knob) || ! isTargetInView(knob))
            continue; // scrolled out of view (a track lane): no plug or cable until it scrolls back
        // Plugs sit on the dial's upper right, one per wire, around the rim.
        const int k = plugCount[knob]++;
        Plug plug;
        plug.route = r;
        plug.pos = getLocalPoint(knob, knob->getPlugPosition(k));
        plug.knob = knob;
        juce::Point<float> gate;
        if (knob->getCableGate(k, gate))
        {
            plug.gate = getLocalPoint(knob, gate);
            plug.hasGate = true;
        }
        m_plugs.push_back(plug);
    }
    for (const auto& [id, knob] : m_knobs)
        if (knob != nullptr)
        {
            const auto it = arcs.find(knob);
            knob->setModArcs(it != arcs.end() ? it->second : std::vector<ModTarget::ModArc> {});
        }

    drawCables();
}

int ModWires::hoverSource() const { return m_hoverPlug >= 0 ? m_hoverPlug : m_hoverCard; }

void ModWires::setHover(int card, int plug)
{
    const int before = hoverSource();
    m_hoverCard = card;
    m_hoverPlug = plug;
    if (hoverSource() != before && m_hasCables)
        drawCables();
}

float ModWires::cableAlpha(int source) const
{
    // The modulator under the mouse (its card, or one of its plugs): its cables at full strength, the others faded.
    const int hover = hoverSource();
    return hover < 0 ? c_cableAlpha : (source == hover ? c_cableAlphaHovered : c_cableAlphaFaded);
}

void ModWires::drawCables()
{
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
        // The faded ones first, so the one that stands out lies over them.
        const int hover = hoverSource();
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& plug : m_plugs)
            {
                if ((plug.route.source == hover) != (pass == 1))
                    continue;
                const auto jack = getLocalPoint(m_panel, m_panel->getJackCentre(plug.route.source));
                drawCable(g, cablePath(jack, plug.pos, plug.hasGate ? &plug.gate : nullptr), ModPanel::sourceColour(plug.route.source),
                          cableAlpha(plug.route.source));
            }
    }
    repaint();
}

void ModWires::setTargets(std::map<juce::String, ModTarget*> knobs)
{
    m_knobs = std::move(knobs);
    for (const auto& [id, target] : m_knobs)
        if (target != nullptr)
            target->onModDepthGesture = [this, t = target](const juce::MouseEvent& e, int phase) { modDepthGesture(t, e, phase); };
    rebuild();
}

const ModWires::Plug* ModWires::newestPlug(ModTarget* target) const
{
    const Plug* last = nullptr;
    const auto known = m_newestSlot.find(target);
    for (const auto& plug : m_plugs)
        if (plug.knob == target)
        {
            if (known != m_newestSlot.end() && plug.route.slot == known->second)
                return &plug;
            last = &plug;
        }
    return last;
}

void ModWires::modDepthGesture(ModTarget* target, const juce::MouseEvent& e, int phase)
{
    if (phase == 0)
    {
        const Plug* newest = newestPlug(target);
        if (newest == nullptr)
            return;
        m_depthSlot = newest->route.slot;
        m_depthStart = newest->route.depth;
        m_depthPos = newest->pos;
        repaint(juce::Rectangle<float>(60.0f, 50.0f).withCentre(m_depthPos.translated(0.0f, -10.0f)).getSmallestIntegerContainer());
        if (onStatus)
            onStatus("Drag up or down to set " + describe(newest->route) + " (down through 0 for negative)");
        return;
    }
    if (phase == 1)
    {
        mouseDrag(e);
        return;
    }
    if (phase == 3)
    {
        // Cmd-double-click: the newest wire back to +50%, like double-clicking its plug.
        const Plug* newest = newestPlug(target);
        if (newest != nullptr && m_matrix.setDepth(newest->route.slot, 0.5f, m_generation))
            depthChanged(newest->route.slot, 0.5f);
        return;
    }
    mouseUp(e);
}

void ModWires::updateArcs(ModTarget* knob)
{
    std::vector<ModTarget::ModArc> arcs;
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

void ModWires::clearLandFlash()
{
    if (m_landKnob != nullptr)
        m_landKnob->setWireHover(juce::Colours::transparentBlack);
    m_landKnob = nullptr;
    m_landFlash = 0.0f;
}

void ModWires::frame(float dt)
{
    if (m_landSlot < 0)
        return;
    const bool settling = m_landScale.stepHelm(dt);
    if (m_landFlash > 0.0f)
    {
        m_landFlash -= dt;
        if (m_landFlash <= 0.0f)
            clearLandFlash();
    }
    for (const auto& plug : m_plugs)
        if (plug.route.slot == m_landSlot)
        {
            const float room = c_plugRadius * 2.0f;
            repaint(juce::Rectangle<float>(room * 2.0f, room * 2.0f).withCentre(plug.pos).getSmallestIntegerContainer());
            break;
        }
    if (! settling && m_landFlash <= 0.0f)
        m_landSlot = -1;
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
        // Round object: a plug. The one that has just landed is still settling onto its seat.
        const float size = c_plugRadius * 2.0f * (plug.route.slot == m_landSlot ? juce::jlimit(0.8f, 1.6f, m_landScale.value) : 1.0f);
        const auto r = juce::Rectangle<float>(size, size).withCentre(plug.pos);
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
            Theme::Plate::fill(g, bubble, Theme::Cut::small);
            g.setColour(c);
            Theme::Plate::stroke(g, bubble, Theme::Cut::small);
            Theme::fitText(g, text, bubble, m_fonts.caption(), juce::Justification::centred);
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

void ModWires::mouseMove(const juce::MouseEvent& e)
{
    const auto* plug = plugAt(e.position);
    setHover(m_hoverCard, plug != nullptr ? plug->route.source : -1);
}

void ModWires::mouseExit(const juce::MouseEvent&) { setHover(m_hoverCard, -1); }

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
        if (slot >= 0)
        {
            m_newestSlot[m_knobs[id]] = slot;
            // It clicks in: the plug drops onto its seat and the knob's ring flashes in the wire's colour.
            clearLandFlash();
            m_landSlot = slot;
            m_landScale.snap(1.4f);
            m_landScale.to(1.0f);
            m_landFlash = 0.2f;
            m_landKnob = m_knobs[id];
            if (m_landKnob != nullptr)
                m_landKnob->setWireHover(ModPanel::sourceColour(m_dragSource));
        }
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
    menu.addSectionHeader(describe(route));
    menu.addItem(1, "Invert");
    menu.addItem(2, "Depth 25%");
    menu.addItem(3, "Depth 50%");
    menu.addItem(4, "Depth 100%");
    menu.addSeparator();
    menu.addItem(9, "Pull Out the Wire");
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

#include "EqGraph.h"
#include "LookAndFeel.h"
#include "Parameters.h"
#include "PluginProcessor.h"
#include "SeaVisualiser.h"

namespace
{
    namespace TC = Theme::Colours;

    struct BandInfo
    {
        const char* name;
        const char* freq;
        const char* gain;
        const char* q;
        float minHz, maxHz;
    };
    const BandInfo c_info[OutputEq::c_bands] = {
        { "Low Cut", ParamIDs::eqLowCut, nullptr, nullptr, ParamIDs::eqLowCutLowestHz, ParamIDs::eqLowCutHighestHz },
        { "Low Shelf", ParamIDs::eqLowShelfFreq, ParamIDs::eqLowShelfGain, nullptr, ParamIDs::eqLowShelfMinHz, ParamIDs::eqLowShelfMaxHz },
        { "Low Mid", ParamIDs::eqLowMidFreq, ParamIDs::eqLowMidGain, ParamIDs::eqLowMidQ, ParamIDs::eqMidMinHz, ParamIDs::eqMidMaxHz },
        { "High Mid", ParamIDs::eqHighMidFreq, ParamIDs::eqHighMidGain, ParamIDs::eqHighMidQ, ParamIDs::eqMidMinHz, ParamIDs::eqMidMaxHz },
        { "High Shelf", ParamIDs::eqHighShelfFreq, ParamIDs::eqHighShelfGain, nullptr, ParamIDs::eqHighShelfMinHz, ParamIDs::eqHighShelfMaxHz },
        { "High Cut", ParamIDs::eqHighCut, nullptr, nullptr, ParamIDs::eqHighCutLowestHz, ParamIDs::eqHighCutHighestHz },
    };

    bool validBand(int band) { return band >= 0 && band < OutputEq::c_bands; }

    bool sameParams(const OutputEq::Params& a, const OutputEq::Params& b)
    {
        const auto eq = [](float x, float y) { return juce::exactlyEqual(x, y); };
        return a.on == b.on && a.lowCutSlope == b.lowCutSlope && eq(a.lowCutHz, b.lowCutHz) && eq(a.lowShelfHz, b.lowShelfHz)
            && eq(a.lowShelfDb, b.lowShelfDb) && eq(a.lowMidHz, b.lowMidHz) && eq(a.lowMidDb, b.lowMidDb) && eq(a.lowMidQ, b.lowMidQ)
            && eq(a.highMidHz, b.highMidHz) && eq(a.highMidDb, b.highMidDb) && eq(a.highMidQ, b.highMidQ)
            && eq(a.highShelfHz, b.highShelfHz) && eq(a.highShelfDb, b.highShelfDb) && eq(a.highCutHz, b.highCutHz);
    }

    /** The response the graph draws: what the filters do at the rate, never asked for above what that rate carries. */
    float responseAt(const OutputEq::Params& p, float hz, double rate, int band = -1)
    {
        const double top = 0.499 * rate;
        return static_cast<float>(OutputEq::responseDb(p, juce::jmin(static_cast<double>(hz), top), rate, band));
    }

    /** What was typed into the box: "320 Hz -3 dB Q 1.4", any part of it, units optional. */
    struct Typed
    {
        bool hasFrequency = false, hasGain = false, hasQ = false, off = false;
        double frequency = 0.0, gain = 0.0, q = 0.0;
        bool any() const { return hasFrequency || hasGain || hasQ || off; }
    };

    Typed parseTyped(const juce::String& text, bool cut, bool bell)
    {
        Typed out;
        // A typed minus may be a real minus sign or a dash; a decimal comma is a decimal point.
        const auto t = text.trim().toLowerCase().replaceCharacter(static_cast<juce::juce_wchar>(0x2212), '-')
                           .replaceCharacter(static_cast<juce::juce_wchar>(0x2013), '-').replaceCharacter(',', '.');
        if (cut && t.contains("off"))
        {
            out.off = true;
            return out;
        }
        const auto isDigit = [](juce::juce_wchar c) { return juce::CharacterFunctions::isDigit(c); };
        const auto isLetter = [](juce::juce_wchar c) { return c >= 'a' && c <= 'z'; };
        std::array<double, 3> bare {};
        int numBare = 0;
        const int len = t.length();
        int i = 0;
        juce::String before; // the letters since the last number
        while (i < len)
        {
            const bool startsNumber = isDigit(t[i]) || (t[i] == '.' && i + 1 < len && isDigit(t[i + 1]));
            if (! startsNumber)
            {
                if (isLetter(t[i]))
                    before << juce::String::charToString(t[i]);
                ++i;
                continue;
            }
            int start = i;
            if (start > 0 && (t[start - 1] == '-' || t[start - 1] == '+'))
                --start;
            bool seenPoint = false;
            while (i < len && (isDigit(t[i]) || (t[i] == '.' && ! seenPoint)))
            {
                seenPoint = seenPoint || t[i] == '.';
                ++i;
            }
            const double value = t.substring(start, i).getDoubleValue();
            // The unit after it, when it is one.
            int j = i;
            while (j < len && t[j] == ' ')
                ++j;
            int k = j;
            while (k < len && isLetter(t[k]))
                ++k;
            const auto unit = t.substring(j, k);
            const bool isUnit = unit == "hz" || unit == "khz" || unit == "k" || unit == "db";
            if (isUnit)
                i = k;
            if (before.endsWith("q") && bell)
            {
                out.hasQ = true;
                out.q = value;
            }
            else if (unit == "hz" || unit == "khz" || unit == "k")
            {
                out.hasFrequency = true;
                out.frequency = value * (unit == "hz" ? 1.0 : 1000.0);
            }
            else if (unit == "db" && ! cut)
            {
                out.hasGain = true;
                out.gain = value;
            }
            else if (! isUnit && numBare < 3)
                bare[static_cast<size_t>(numBare++)] = value;
            before.clear();
        }
        // Numbers with no unit fill what was not named, in the readout's order: frequency, gain, Q.
        int next = 0;
        if (! out.hasFrequency && next < numBare)
        {
            out.hasFrequency = true;
            out.frequency = bare[static_cast<size_t>(next++)];
        }
        if (! cut && ! out.hasGain && next < numBare)
        {
            out.hasGain = true;
            out.gain = bare[static_cast<size_t>(next++)];
        }
        if (bell && ! out.hasQ && next < numBare)
        {
            out.hasQ = true;
            out.q = bare[static_cast<size_t>(next++)];
        }
        return out;
    }

    juce::String qText(float q) { return "Q " + (q < 0.995f ? juce::String(q, 2) : juce::String(q, 1)); }

    /** The hard-edged line a curve is drawn with. */
    juce::PathStrokeType curveStroke(float thickness)
    {
        return juce::PathStrokeType(thickness, juce::PathStrokeType::mitered, juce::PathStrokeType::butt);
    }
}

//==============================================================================
/** A handle held by the mouse: one undo step from the press to the release, whatever it moved. */
struct EqGraph::Drag
{
    Drag(EditHistory& history, const juce::String& name, int whichBand) : group(history, name), band(whichBand) {}
    ~Drag()
    {
        // The gestures end before the group does (its member is destroyed after this), so they are its step.
        for (auto* p : params)
            if (p != nullptr)
                p->endChangeGesture();
    }
    void hold(juce::RangedAudioParameter* p)
    {
        if (p == nullptr)
            return;
        p->beginChangeGesture();
        params[static_cast<size_t>(count++)] = p;
    }

    EditHistory::Group group;
    int band = -1;
    std::array<juce::RangedAudioParameter*, 3> params {};
    int count = 0;
    juce::Point<float> grab;  // from the mouse to the handle's centre
    bool settingQ = false;    // Cmd is down: up and down is the bell's Q
    float qFrom = 1.0f, qFromY = 0.0f;
};

//==============================================================================
EqGraph::EqGraph(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_fonts(fonts), m_spectrum(std::make_unique<OutputSpectrum>())
{
    auto& state = m_processor.getState();
    for (int b = 0; b < OutputEq::c_bands; ++b)
    {
        const auto& info = c_info[b];
        m_freq[static_cast<size_t>(b)] = state.getParameter(info.freq);
        m_gain[static_cast<size_t>(b)] = info.gain != nullptr ? state.getParameter(info.gain) : nullptr;
        m_q[static_cast<size_t>(b)] = info.q != nullptr ? state.getParameter(info.q) : nullptr;
        jassert(m_freq[static_cast<size_t>(b)] != nullptr);
    }
    m_path.preallocateSpace(3 * (c_width / c_curveStep + 8));
    setSize(c_width, c_height);
   #if JUCE_MAC
    const juce::String q = "Cmd-drag";
   #else
    const juce::String q = "Ctrl-drag";
   #endif
    getProperties().set("howTo", "Drag a handle for frequency and gain. " + q + " or the mouse wheel sets a bell's Q. "
                                 "Double-click flattens a band or parks a cut. Right-click to type values.");
}

EqGraph::~EqGraph() = default;

const char* EqGraph::bandName(int band) { return validBand(band) ? c_info[band].name : ""; }

float EqGraph::plain(const juce::RangedAudioParameter* p) { return p != nullptr ? p->convertFrom0to1(p->getValue()) : 0.0f; }

void EqGraph::setPlain(juce::RangedAudioParameter* p, float value)
{
    if (p == nullptr)
        return;
    const float n = juce::jlimit(0.0f, 1.0f, p->convertTo0to1(value));
    if (std::abs(n - p->getValue()) > 1.0e-7f)
        p->setValueNotifyingHost(n);
}

bool EqGraph::isParked(int band) const
{
    if (band == OutputEq::LowCut)
        return Params::eqLowCutIsOff(plain(frequencyParam(band)));
    if (band == OutputEq::HighCut)
        return Params::eqHighCutIsOff(plain(frequencyParam(band)));
    return false;
}

float EqGraph::frequencyOf(int band) const { return validBand(band) ? plain(frequencyParam(band)) : 0.0f; }
float EqGraph::gainOf(int band) const { return validBand(band) ? plain(gainParam(band)) : 0.0f; }
float EqGraph::qOf(int band) const { return validBand(band) && qParam(band) != nullptr ? plain(qParam(band)) : 0.7071f; }

EqGraph::Readout EqGraph::readout(int band) const
{
    Readout r;
    if (! validBand(band))
        return r;
    r.name = bandName(band);
    r.frequency = isParked(band) ? juce::String("Off") : Params::hzText(frequencyOf(band));
    if (band == OutputEq::LowCut)
        r.gain = m_processor.getOutputEqParams().lowCutSlope == 12 ? "12 dB" : "24 dB";
    else if (band == OutputEq::HighCut)
        r.gain = "12 dB";
    else
        r.gain = Params::gainDbText(gainOf(band));
    if (isBell(band))
        r.q = qText(qOf(band));
    return r;
}

//==============================================================================
float EqGraph::xForHz(float hz) const
{
    const auto plot = plotArea();
    return plot.getX() + plot.getWidth() * std::log(juce::jlimit(c_minHz, c_maxHz, hz) / c_minHz) / std::log(c_maxHz / c_minHz);
}

float EqGraph::hzForX(float x) const
{
    const auto plot = plotArea();
    const float t = plot.getWidth() > 0.0f ? juce::jlimit(0.0f, 1.0f, (x - plot.getX()) / plot.getWidth()) : 0.0f;
    return c_minHz * std::pow(c_maxHz / c_minHz, t);
}

float EqGraph::yForDb(float db) const
{
    const auto plot = plotArea();
    return plot.getCentreY() - 0.5f * plot.getHeight() * db / c_rangeDb;
}

float EqGraph::dbForY(float y) const
{
    const auto plot = plotArea();
    return plot.getHeight() > 0.0f ? juce::jlimit(-c_rangeDb, c_rangeDb, (plot.getCentreY() - y) / (0.5f * plot.getHeight()) * c_rangeDb) : 0.0f;
}

juce::Point<float> EqGraph::handleCentre(int band) const
{
    if (! validBand(band))
        return {};
    if (isParked(band))
        return { band == OutputEq::LowCut ? c_parkedInset : static_cast<float>(getWidth()) - c_parkedInset, yForDb(0.0f) };
    return { xForHz(frequencyOf(band)), yForDb(gainOf(band)) };
}

int EqGraph::bandAt(juce::Point<float> at) const
{
    int best = -1;
    float bestDistance = c_reach;
    for (int b = 0; b < OutputEq::c_bands; ++b)
    {
        const auto c = handleCentre(b);
        const float d = juce::jmax(std::abs(c.x - at.x), std::abs(c.y - at.y)); // a square's reach
        if (d < bestDistance - 0.01f || (d <= bestDistance && b == m_selected))
        {
            best = b;
            bestDistance = d;
        }
    }
    return best;
}

//==============================================================================
void EqGraph::changed()
{
    if (validBand(m_hover))
    {
        const auto r = readout(m_hover);
        getProperties().set("tipLine", (r.frequency + "   " + r.gain + (r.q.isNotEmpty() ? "   " + r.q : juce::String())).trim());
    }
    else
        getProperties().remove("tipLine");
    if (onSelectionChanged)
        onSelectionChanged();
    repaint();
}

void EqGraph::selectBand(int band)
{
    band = validBand(band) ? band : -1;
    if (band == m_selected)
        return;
    m_selected = band;
    changed();
}

void EqGraph::setHover(int band)
{
    if (band == m_hover)
        return;
    m_hover = band;
    changed();
}

void EqGraph::setFrequency(int band, float hz, bool parked)
{
    const auto& info = c_info[band];
    if (band == OutputEq::LowCut && parked)
        hz = ParamIDs::eqLowCutOffHz;
    else if (band == OutputEq::HighCut && parked)
        hz = ParamIDs::eqHighCutOffHz;
    else
        hz = juce::jlimit(info.minHz, info.maxHz, hz);
    setPlain(frequencyParam(band), hz);
}

void EqGraph::setQ(int band, float q)
{
    if (! validBand(band) || qParam(band) == nullptr)
        return;
    setPlain(qParam(band), juce::jlimit(ParamIDs::eqQMin, ParamIDs::eqQMax, q));
}

void EqGraph::flattenBand(int band)
{
    if (! validBand(band))
        return;
    endDrag();
    {
        EditHistory::Group group(m_processor.getHistory(), juce::String("Output EQ ") + bandName(band));
        auto* p = isCut(band) ? frequencyParam(band) : gainParam(band);
        p->beginChangeGesture();
        if (isCut(band))
            setFrequency(band, 0.0f, true);
        else
            setPlain(p, 0.0f);
        p->endChangeGesture();
    }
    changed();
}

void EqGraph::flattenAll()
{
    endDrag();
    {
        EditHistory::Group group(m_processor.getHistory(), "Output EQ Flat");
        for (int b = 0; b < OutputEq::c_bands; ++b)
        {
            auto* p = isCut(b) ? frequencyParam(b) : gainParam(b);
            p->beginChangeGesture();
            if (isCut(b))
                setFrequency(b, 0.0f, true);
            else
                setPlain(p, 0.0f);
            p->endChangeGesture();
        }
    }
    changed();
}

bool EqGraph::typeValues(int band, const juce::String& text)
{
    if (! validBand(band))
        return false;
    const auto typed = parseTyped(text, isCut(band), isBell(band));
    if (! typed.any())
        return false;
    endDrag();
    {
        EditHistory::Group group(m_processor.getHistory(), juce::String("Output EQ ") + bandName(band));
        if (typed.off || typed.hasFrequency)
        {
            auto* p = frequencyParam(band);
            p->beginChangeGesture();
            setFrequency(band, static_cast<float>(typed.frequency), typed.off);
            p->endChangeGesture();
        }
        if (auto* p = gainParam(band); p != nullptr && typed.hasGain)
        {
            p->beginChangeGesture();
            setPlain(p, juce::jlimit(-ParamIDs::eqGainMaxDb, ParamIDs::eqGainMaxDb, static_cast<float>(typed.gain)));
            p->endChangeGesture();
        }
        if (auto* p = qParam(band); p != nullptr && typed.hasQ)
        {
            p->beginChangeGesture();
            setQ(band, static_cast<float>(typed.q));
            p->endChangeGesture();
        }
    }
    changed();
    return true;
}

//==============================================================================
bool EqGraph::isTyping() const { return m_typing != nullptr && m_typing->isVisible(); }

void EqGraph::beginTyping(int band)
{
    if (! validBand(band))
        return;
    endDrag();
    selectBand(band);
    if (m_typing == nullptr)
    {
        m_typing = std::make_unique<juce::TextEditor>();
        m_typing->setColour(juce::TextEditor::backgroundColourId, TC::well);
        m_typing->setColour(juce::TextEditor::textColourId, TC::text);
        m_typing->setColour(juce::TextEditor::highlightColourId, TC::output.withAlpha(0.35f));
        m_typing->setColour(juce::TextEditor::focusedOutlineColourId, TC::output);
        m_typing->setJustification(juce::Justification::centred);
        m_typing->setBorder(juce::BorderSize<int>(1));
        m_typing->setIndents(2, 0);
        m_typing->setSelectAllWhenFocused(true);
        m_typing->onReturnKey = [this] { endTyping(true, true); };
        m_typing->onEscapeKey = [this] { endTyping(false, true); };
        m_typing->onFocusLost = [this] { endTyping(false, false); }; // clicked away: whatever was clicked keeps the keys
        addChildComponent(*m_typing);
    }
    m_typingBand = band;
    placeTyping();
    const auto r = readout(band);
    m_typing->applyFontToAllText(m_fonts.caption());
    m_typing->setText(isCut(band) ? r.frequency : (r.frequency + "  " + r.gain + (r.q.isNotEmpty() ? "  " + r.q : juce::String())), false);
    m_typing->setVisible(true);
    m_typing->toFront(false);
    m_typing->grabKeyboardFocus();
    m_typing->selectAll();
}

void EqGraph::placeTyping()
{
    if (m_typing == nullptr || ! validBand(m_typingBand))
        return;
    const auto c = handleCentre(m_typingBand);
    const int w = isCut(m_typingBand) ? 72 : 148, h = 20;
    // Over the handle when there is room above it, else under; always whole inside the graph.
    int y = juce::roundToInt(c.y - c_handle - static_cast<float>(h));
    if (y < 3)
        y = juce::roundToInt(c.y + c_handle);
    const int x = juce::jlimit(3, juce::jmax(3, getWidth() - w - 3), juce::roundToInt(c.x) - w / 2);
    m_typing->setBounds(x, juce::jlimit(3, juce::jmax(3, getHeight() - h - 3), y), w, h);
}

void EqGraph::endTyping(bool keep, bool takeKeysBack)
{
    if (! isTyping())
        return;
    const auto text = m_typing->getText();
    m_typing->setVisible(false); // first: hiding it loses the focus, which comes back here
    if (keep)
        typeValues(m_typingBand, text);
    // The keys go back to what holds the graph (the plate takes Esc), not to nothing.
    if (takeKeysBack)
        if (auto* parent = getParentComponent())
            parent->grabKeyboardFocus();
}

//==============================================================================
void EqGraph::endDrag() { m_drag.reset(); }

void EqGraph::mouseMove(const juce::MouseEvent& e) { setHover(bandAt(e.position)); }

void EqGraph::mouseExit(const juce::MouseEvent&)
{
    if (m_drag == nullptr)
        setHover(-1);
}

void EqGraph::mouseDown(const juce::MouseEvent& e)
{
    endDrag();
    const int band = bandAt(e.position);
    setHover(band);
    if (band < 0)
        return;
    selectBand(band);
    if (e.mods.isPopupMenu())
    {
        beginTyping(band);
        return;
    }
    m_drag = std::make_unique<Drag>(m_processor.getHistory(), juce::String("Output EQ ") + bandName(band), band);
    m_drag->hold(frequencyParam(band));
    m_drag->hold(gainParam(band));
    m_drag->hold(qParam(band));
    m_drag->grab = handleCentre(band) - e.position;
    m_drag->settingQ = isBell(band) && e.mods.isCommandDown();
    m_drag->qFrom = qOf(band);
    m_drag->qFromY = e.position.y;
}

void EqGraph::mouseDrag(const juce::MouseEvent& e)
{
    if (m_drag == nullptr)
        return;
    auto& drag = *m_drag;
    const int band = drag.band;
    // Cmd held on a bell: up and down is its Q, and the handle stays where it is.
    const bool wantsQ = isBell(band) && e.mods.isCommandDown();
    if (wantsQ != drag.settingQ)
    {
        drag.settingQ = wantsQ;
        drag.qFrom = qOf(band);
        drag.qFromY = e.position.y;
        drag.grab = handleCentre(band) - e.position; // let go of Cmd and the handle is picked up where it is
    }
    if (drag.settingQ)
    {
        setQ(band, drag.qFrom * std::pow(2.0f, (drag.qFromY - e.position.y) / 24.0f)); // an octave of Q every 24 px
        changed();
        return;
    }
    const auto to = e.position + drag.grab;
    if (isCut(band))
    {
        // Out in the margin past its end of the axis it is parked; over the axis it is on.
        const auto plot = plotArea();
        const bool parked = band == OutputEq::LowCut ? to.x < 0.5f * (c_parkedInset + plot.getX())
                                                      : to.x > 0.5f * (static_cast<float>(getWidth()) - c_parkedInset + plot.getRight());
        setFrequency(band, hzForX(to.x), parked);
    }
    else
    {
        setFrequency(band, hzForX(to.x), false);
        const float db = dbForY(to.y);
        setPlain(gainParam(band), std::abs(db) < c_detentDb ? 0.0f : db);
    }
    changed();
}

void EqGraph::mouseUp(const juce::MouseEvent& e)
{
    endDrag();
    setHover(contains(e.getPosition()) ? bandAt(e.position) : -1);
}

void EqGraph::mouseDoubleClick(const juce::MouseEvent& e)
{
    // The second press began a drag: that is over, and what a small slip of the hand did in it is put right here.
    endDrag();
    const int band = bandAt(e.position);
    if (band < 0 || e.mods.isPopupMenu())
        return;
    selectBand(band);
    flattenBand(band);
}

void EqGraph::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    int band = bandAt(e.position);
    if (! isBell(band))
        band = bandAt(e.position) < 0 && isBell(m_selected) ? m_selected : -1;
    if (band < 0 || m_drag != nullptr)
    {
        juce::Component::mouseWheelMove(e, wheel);
        return;
    }
    selectBand(band);
    const float turn = (wheel.isReversed ? -wheel.deltaY : wheel.deltaY) * 4.0f; // up is narrower
    if (std::abs(turn) < 1.0e-4f)
        return;
    // One gesture per move: moves in a row join into one undo step, as loose moves do everywhere.
    auto* p = qParam(band);
    p->beginChangeGesture();
    setQ(band, qOf(band) * std::pow(2.0f, juce::jlimit(-1.0f, 1.0f, turn)));
    p->endChangeGesture();
    changed();
}

juce::String EqGraph::getTooltip()
{
    if (! validBand(m_hover))
        return "Output EQ\nOne curve on every speaker, after Space and before Inflate and the limiter. The line is what the EQ does "
               "at each frequency; the output's spectrum shows faint behind it.";
    juce::String body = Params::tipFor(c_info[m_hover].freq);
    if (isBell(m_hover))
        body << " Q is how narrow the band is: higher is narrower.";
    return juce::String(bandName(m_hover)) + "\n" + body;
}

//==============================================================================
void EqGraph::resized()
{
    const int plotWidth = juce::jmax(0, juce::roundToInt(plotArea().getWidth()));
    m_curve.assign(static_cast<size_t>(plotWidth / c_curveStep + 1), 0.0f);
    m_bandCurve.assign(m_curve.size(), 0.0f);
    m_levels.assign(static_cast<size_t>(plotWidth / c_spectrumStep + 1), 0.0f);
    m_curveWidth = -1; // worked out again at the next refresh or paint
    if (isTyping())
        placeTyping();
}

void EqGraph::rebuildCurve()
{
    m_curveParams = m_processor.getOutputEqParams();
    m_curveRate = m_processor.getRenderRate() > 0.0 ? m_processor.getRenderRate() : 48000.0;
    m_curveBand = m_selected;
    m_curveWidth = getWidth();
    // The curve the settings make, whether or not the EQ is switched on (off, it is drawn unlit).
    auto p = m_curveParams;
    p.on = true;
    const auto plot = plotArea();
    for (size_t i = 0; i < m_curve.size(); ++i)
    {
        const float hz = hzForX(plot.getX() + static_cast<float>(i) * static_cast<float>(c_curveStep));
        m_curve[i] = responseAt(p, hz, m_curveRate);
        m_bandCurve[i] = validBand(m_selected) ? responseAt(p, hz, m_curveRate, m_selected) : 0.0f;
    }
}

void EqGraph::refresh()
{
    if (! Theme::isShownInWindow(*this))
        return;
    bool dirty = false;

    // The spectrum: each column rises fast and falls slowly, as the sea's does.
    m_spectrum->read(m_processor);
    const float rate = m_spectrum->rate(), top = 0.45f * rate, bin = rate / static_cast<float>(OutputSpectrum::c_fftSize);
    const auto plot = plotArea();
    bool quiet = true;
    for (size_t i = 0; i < m_levels.size(); ++i)
    {
        const float x = plot.getX() + static_cast<float>(i) * static_cast<float>(c_spectrumStep);
        const float f0 = hzForX(x - 0.5f * c_spectrumStep), f1 = hzForX(x + 0.5f * c_spectrumStep);
        float v = 0.0f;
        if (f0 < top)
            v = f1 - f0 > bin ? m_spectrum->band(f0, f1) : m_spectrum->at(hzForX(x));
        auto& s = m_levels[i];
        s += (v - s) * (v > s ? 0.5f : 0.15f);
        if (s < 0.002f)
            s = 0.0f;
        quiet = quiet && s <= 0.0f;
    }
    dirty = ! (quiet && m_spectrumQuiet);
    m_spectrumQuiet = quiet;

    const double renderRate = m_processor.getRenderRate() > 0.0 ? m_processor.getRenderRate() : 48000.0;
    if (m_curveWidth != getWidth() || m_curveBand != m_selected || ! juce::exactlyEqual(renderRate, m_curveRate)
        || ! sameParams(m_curveParams, m_processor.getOutputEqParams()))
    {
        const bool settingsMoved = ! sameParams(m_curveParams, m_processor.getOutputEqParams());
        rebuildCurve();
        if (isTyping())
            placeTyping();
        if (settingsMoved && onSelectionChanged)
            onSelectionChanged(); // moved from somewhere else too (undo, a host): the readout follows
        dirty = true;
    }
    if (dirty)
        repaint();
}

void EqGraph::paintHandle(juce::Graphics& g, int band, bool eqOn) const
{
    const auto c = handleCentre(band);
    const bool selected = band == m_selected, over = band == m_hover;
    const bool parked = isParked(band);
    const float side = c_handle + (selected ? 2.0f : 0.0f);
    // On whole pixels, so its edges are hard.
    const auto box = juce::Rectangle<float>(side, side).withCentre({ std::round(c.x), std::round(c.y) });

    // A band that is doing something stands on a stem from the 0 dB line.
    if (! isCut(band) && std::abs(gainOf(band)) > 0.01f)
    {
        const float y0 = yForDb(0.0f);
        g.setColour(TC::output.withAlpha(eqOn ? 0.4f : 0.2f));
        g.fillRect(juce::Rectangle<float>(std::round(c.x) - 0.5f, juce::jmin(y0, c.y), 1.0f, std::abs(c.y - y0)));
    }
    if (selected)
    {
        g.setColour(TC::text);
        Theme::Plate::stroke(g, box.expanded(2.0f), 0.0f);
    }
    g.setColour(TC::edgeDark);
    g.fillRect(box.expanded(1.0f));
    if (parked || ! eqOn)
    {
        // Hollow: a parked cut is dashed (it rests until it is pulled in); with the EQ off every handle is an outline.
        g.setColour(over ? TC::hover : TC::raised);
        g.fillRect(box);
        const auto edge = TC::output.withAlpha(over || selected ? 0.95f : 0.6f);
        if (parked)
        {
            juce::Path dashed;
            const float dashes[] = { 3.0f, 2.0f };
            curveStroke(1.0f).createDashedStroke(dashed, Theme::Plate::shape(box.reduced(0.5f), 0.0f), dashes, 2);
            g.setColour(edge);
            g.fillPath(dashed);
        }
        else
        {
            g.setColour(edge);
            Theme::Plate::stroke(g, box, 0.0f);
        }
        return;
    }
    g.setColour(over ? TC::output.brighter(0.35f) : TC::output);
    g.fillRect(box);
    // Stamped: a light line along its top, a dark one along its foot.
    g.setColour(juce::Colours::white.withAlpha(0.35f));
    g.fillRect(box.withHeight(1.0f));
    g.setColour(TC::output.darker(0.45f));
    g.fillRect(box.withTop(box.getBottom() - 1.0f));
}

void EqGraph::paint(juce::Graphics& g)
{
    const auto well = getLocalBounds().toFloat();
    SwooshLookAndFeel::drawWell(g, well);
    const auto plot = plotArea();
    if (plot.getWidth() <= 1.0f || plot.getHeight() <= 1.0f)
        return;
    if (m_curveWidth != getWidth() || m_curveBand != m_selected)
        rebuildCurve();
    const bool on = m_curveParams.on;
    const float y0 = std::round(yForDb(0.0f));
    const auto inside = well.reduced(1.0f);

    // The grid: every decade and the 0 dB line marked, the steps between and +-6 dB faint. The axis ends at
    // 20 Hz and 20 kHz; outside them is the margin a parked cut sits in.
    g.setColour(TC::shadow.withAlpha(0.45f));
    g.fillRect(inside.withRight(plot.getX()));
    g.fillRect(inside.withLeft(plot.getRight()));
    for (const float hz : { 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f, 90.0f, 200.0f, 300.0f, 400.0f, 500.0f, 600.0f, 700.0f, 800.0f, 900.0f,
                            2000.0f, 3000.0f, 4000.0f, 5000.0f, 6000.0f, 7000.0f, 8000.0f, 9000.0f })
    {
        g.setColour(TC::grid.withAlpha(0.45f));
        g.fillRect(juce::Rectangle<float>(std::round(xForHz(hz)), inside.getY(), 1.0f, inside.getHeight()));
    }
    for (const float hz : { 20.0f, 100.0f, 1000.0f, 10000.0f, 20000.0f })
    {
        g.setColour(TC::grid);
        g.fillRect(juce::Rectangle<float>(std::round(xForHz(hz)), inside.getY(), 1.0f, inside.getHeight()));
    }
    for (const float db : { -12.0f, -6.0f, 6.0f, 12.0f })
    {
        g.setColour(TC::grid.withAlpha(std::abs(db) > 6.0f ? 0.9f : 0.5f));
        g.fillRect(juce::Rectangle<float>(plot.getX(), std::round(yForDb(db)), plot.getWidth(), 1.0f));
    }
    g.setColour(TC::stroke);
    g.fillRect(juce::Rectangle<float>(plot.getX(), y0, plot.getWidth(), 1.0f));

    // The scale, in the corners the handles are seldom in.
    {
        const auto font = m_fonts.caption();
        g.setColour(TC::textFaint);
        const float labelHeight = 12.0f;
        static const std::pair<float, const char*> decades[] = { { 100.0f, "100 Hz" }, { 1000.0f, "1 kHz" }, { 10000.0f, "10 kHz" } };
        for (const auto& [hz, text] : decades)
            Theme::fitText(g, text, juce::Rectangle<float>(std::round(xForHz(hz)) + 4.0f, inside.getBottom() - labelHeight - 1.0f, 44.0f, labelHeight),
                           font, juce::Justification::centredLeft);
        Theme::fitText(g, "+12 dB", juce::Rectangle<float>(plot.getX() + 4.0f, std::round(yForDb(12.0f)) + 2.0f, 40.0f, labelHeight), font,
                       juce::Justification::centredLeft);
        Theme::fitText(g, "-12 dB", juce::Rectangle<float>(plot.getX() + 4.0f, std::round(yForDb(-12.0f)) - labelHeight - 1.0f, 40.0f, labelHeight), font,
                       juce::Justification::centredLeft);
    }

    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(inside.getSmallestIntegerContainer());

        // The output's spectrum, faint behind: a filled shape up from the well's foot.
        if (! m_spectrumQuiet)
        {
            m_path.clear();
            m_path.startNewSubPath(plot.getX(), inside.getBottom());
            for (size_t i = 0; i < m_levels.size(); ++i)
                m_path.lineTo(plot.getX() + static_cast<float>(i) * static_cast<float>(c_spectrumStep),
                              inside.getBottom() - m_levels[i] * (inside.getHeight() - 6.0f));
            m_path.lineTo(plot.getX() + static_cast<float>(m_levels.size() - 1) * static_cast<float>(c_spectrumStep), inside.getBottom());
            m_path.closeSubPath();
            g.setColour(TC::output.withAlpha(0.13f));
            g.fillPath(m_path);
        }

        const auto curveInto = [&](const std::vector<float>& db, bool closedToLine)
        {
            m_path.clear();
            if (closedToLine)
                m_path.startNewSubPath(plot.getX(), y0 + 0.5f);
            for (size_t i = 0; i < db.size(); ++i)
            {
                const float x = plot.getX() + static_cast<float>(i) * static_cast<float>(c_curveStep);
                const float y = juce::jlimit(inside.getY() - 4.0f, inside.getBottom() + 4.0f, yForDb(db[i]));
                if (i == 0 && ! closedToLine)
                    m_path.startNewSubPath(x, y);
                else
                    m_path.lineTo(x, y);
            }
            if (closedToLine)
            {
                m_path.lineTo(plot.getX() + static_cast<float>(db.size() - 1) * static_cast<float>(c_curveStep), y0 + 0.5f);
                m_path.closeSubPath();
            }
        };

        // The selected band's own share of the curve, as a tint.
        if (validBand(m_selected))
        {
            curveInto(m_bandCurve, true);
            g.setColour(TC::output.withAlpha(on ? 0.16f : 0.07f));
            g.fillPath(m_path);
        }
        // The whole curve: tinted to the 0 dB line, then the line itself. Unlit while the EQ is off.
        curveInto(m_curve, true);
        g.setColour(TC::output.withAlpha(on ? 0.10f : 0.04f));
        g.fillPath(m_path);
        curveInto(m_curve, false);
        g.setColour(on ? TC::output : TC::textFaint.withAlpha(0.7f));
        g.strokePath(m_path, curveStroke(on ? 1.6f : 1.2f));
    }

    // The handles, the selected one last so it is on top.
    for (int b = 0; b < OutputEq::c_bands; ++b)
        if (b != m_selected)
            paintHandle(g, b, on);
    if (validBand(m_selected))
        paintHandle(g, m_selected, on);
}

//==============================================================================
void EqGraph::paintMiniature(juce::Graphics& g, juce::Rectangle<float> area, const OutputEq::Params& p, double sampleRate,
                             juce::Colour colour, bool lit)
{
    if (area.getWidth() < 4.0f || area.getHeight() < 3.0f)
        return;
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const float yMid = std::floor(area.getCentreY()) + 0.5f;
    const float half = 0.5f * area.getHeight() - 1.0f;
    const auto line = lit ? colour : colour.withMultipliedAlpha(0.5f);
    // The 0 dB line, faint under a curve and the whole picture when the EQ does nothing.
    const bool flat = OutputEq::isFlat(p);
    g.setColour(flat ? line : colour.withMultipliedAlpha(0.25f));
    g.fillRect(juce::Rectangle<float>(area.getX(), yMid - 0.5f, area.getWidth(), 1.0f));
    if (flat)
        return;
    juce::Path path;
    const int steps = juce::jmax(8, juce::roundToInt(area.getWidth() / 1.5f));
    for (int i = 0; i <= steps; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float hz = c_minHz * std::pow(c_maxHz / c_minHz, t);
        const float db = juce::jlimit(-c_rangeDb, c_rangeDb, responseAt(p, hz, rate));
        const juce::Point<float> at { area.getX() + t * area.getWidth(), yMid - half * db / c_rangeDb };
        if (i == 0)
            path.startNewSubPath(at);
        else
            path.lineTo(at);
    }
    g.setColour(line);
    g.strokePath(path, curveStroke(1.3f));
}

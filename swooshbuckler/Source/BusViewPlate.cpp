#include "BusViewPlate.h"
#include "LookAndFeel.h"
#include "PluginProcessor.h"
#include "SeaVisualiser.h"
#include "SwirlFx.h"

#include <cmath>

namespace
{
    namespace TC = Theme::Colours;

    juce::PathStrokeType lineStroke(float thickness)
    {
        return juce::PathStrokeType(thickness, juce::PathStrokeType::mitered, juce::PathStrokeType::butt);
    }
    float yForDb(float db, juce::Rectangle<float> plot, float topDb, float bottomDb)
    {
        return plot.getY() + (topDb - db) / (topDb - bottomDb) * plot.getHeight();
    }
    float dbOf(double gain) { return static_cast<float>(20.0 * std::log10(std::max(gain, 1.0e-9))); }
    juce::String hzText(float hz)
    {
        if (hz < 1000.0f)
            return juce::String(juce::roundToInt(hz)) + " Hz";
        return juce::String(hz / 1000.0f, hz < 10000.0f ? 2 : 1) + " kHz";
    }
    juce::String msText(float ms) { return juce::String(std::abs(ms), 1) + " ms"; }

    /** A graph's label, drawn after the data on a patch of the well's own colour, so no curve, band or trace runs
        through it and it keeps the well's contrast. */
    void graphLabel(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> box, const juce::Font& font, juce::Justification just,
                    juce::Colour colour)
    {
        const float w = juce::jmin(box.getWidth(), Theme::textWidth(font, text));
        const float x = just.testFlags(juce::Justification::right) ? box.getRight() - w : box.getX();
        g.setColour(TC::well);
        g.fillRect(juce::Rectangle<float>(x - 2.0f, box.getY(), w + 4.0f, box.getHeight()).getSmallestIntegerContainer().toFloat());
        g.setColour(colour);
        Theme::fitText(g, text, box, font, just);
    }

    /** The Swirl trace's range by type (the value at the top, the value at the foot), and its scale's two labels. */
    struct TraceScale
    {
        float top, foot;
        const char* topLabel;
        const char* footLabel;
        const char* title;
    };
    TraceScale traceScale(int type)
    {
        switch (type)
        {
            case busview::SwirlTape:    return { SwirlFx::c_maxOffsetMs, -SwirlFx::c_maxOffsetMs, "Copy Later", "Copy Earlier", "Copy Against Dry" };
            case busview::SwirlDoubler: return { 80.0f, 0.0f, "80 ms", "0 ms", "Copy Delay" };
            case busview::SwirlPhaser:  return { std::log2(10000.0f / 20.0f), std::log2(100.0f / 20.0f), "10 kHz", "100 Hz", "Phaser Tuning" };
            default:                    return { 7.5f, 0.0f, "7.5 ms", "0 ms", "Flanger Delay" };
        }
    }
}

BusViewPlate::BusViewPlate(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts, int bus)
    : m_processor(processor), m_fonts(fonts), m_bus(juce::jlimit(0, ParamIDs::numBuses - 1, bus)),
      m_accent(bus == 0 ? TC::busA : TC::busB), m_spectrum(std::make_unique<OutputSpectrum>())
{
    setOpaque(false);
    // Read-only: it takes the clicks over its area (nothing under it is reached through it), and none of the keys.
    setInterceptsMouseClicks(true, false);
    setWantsKeyboardFocus(false);
    updateTooltip();
}

void BusViewPlate::updateTooltip()
{
    const juce::String bus = juce::String("Bus ") + ParamIDs::busNames[m_bus];
    if (m_kind == Kind::Filter)
        setTooltip(bus + ": Filter\nWhat the bus's filter is doing now: its curve, a square where it is, a band as wide as it can travel with "
                         "the wheel where it is, the bus's sound faint behind, and the light line for what is heard once the filter is mixed in. "
                         "Everything here is set on the layer below.");
    else
        setTooltip(bus + ": Swirl\nWhat Swirl is doing now: at the left the last four seconds (the copy against the dry, or the delay, or "
                         "where the phaser is tuned), with the passes faint behind; at the right what it does to each frequency now. "
                         "Everything here is set on the layer below.");
}

BusViewPlate::~BusViewPlate() = default;

void BusViewPlate::setKind(Kind kind)
{
    if (kind == m_kind)
        return;
    m_kind = kind;
    m_stillFrames = c_restFrames;
    m_dirty = true;
    updateTooltip();
    refresh();
    repaint();
}

double BusViewPlate::renderRate() const
{
    const double rate = m_processor.getRenderRate();
    return rate > 0.0 ? rate : 48000.0;
}

juce::Rectangle<float> BusViewPlate::graphArea() const
{
    return juce::Rectangle<int>(c_graphX, c_graphY, getWidth() - 2 * c_graphX, c_graphHeight).toFloat();
}

juce::Rectangle<float> BusViewPlate::tracePane() const
{
    auto area = graphArea();
    if (m_swirl.type == busview::SwirlDoubler)
        return area;
    return area.removeFromLeft(std::round(area.getWidth() * 0.42f));
}

juce::Rectangle<float> BusViewPlate::responsePane() const
{
    auto area = graphArea();
    area.removeFromLeft(std::round(area.getWidth() * 0.42f) + 8.0f);
    return area;
}

juce::Rectangle<float> BusViewPlate::plotOf(juce::Rectangle<float> well) const
{
    return well.reduced(8.0f, 6.0f);
}

float BusViewPlate::xForHz(float hz, juce::Rectangle<float> plot)
{
    return plot.getX() + plot.getWidth() * std::log(juce::jmax(hz, 1.0f) / c_minHz) / std::log(c_maxHz / c_minHz);
}

float BusViewPlate::hzForX(float x, juce::Rectangle<float> plot)
{
    return c_minHz * std::pow(c_maxHz / c_minHz, (x - plot.getX()) / juce::jmax(1.0f, plot.getWidth()));
}

bool BusViewPlate::showsHeard() const
{
    // Whenever what is heard is not the curve: the filter part mixed in, or its trim (from 80% up the filter is all
    // that is heard, but trimmed by up to 13 dB as Resonance and the wheel raise its Q).
    return m_lit && (m_filter.mix < 0.999f || std::abs(dbOf(static_cast<double>(m_filter.trim))) > 0.25f);
}

double BusViewPlate::drawnFilterDb(double hz) const
{
    return static_cast<double>(dbOf(busview::filterGain(m_filter, hz, renderRate())));
}

void BusViewPlate::resized()
{
    m_dirty = true;
    m_levels.assign(static_cast<size_t>(juce::jmax(2, juce::roundToInt(plotOf(graphArea()).getWidth()) / c_spectrumStep + 1)), 0.0f);
    refresh();
}

busview::FilterLook BusViewPlate::filterFromParameters() const
{
    auto& state = m_processor.getState();
    const auto* ids = ParamIDs::busDetail[m_bus];
    auto raw = [&state](const char* id) { auto* v = state.getRawParameterValue(id); return v != nullptr ? v->load() : 0.0f; };
    SweepFilter::Settings s;
    s.type = juce::jlimit(0, SweepFilter::c_types - 1, juce::roundToInt(raw(ids[ParamIDs::FilterType])));
    s.steep = raw(ids[ParamIDs::FilterSteep]) > 0.5f;
    s.shape = juce::jlimit(0, SweepFilter::c_shapes - 1, juce::roundToInt(raw(ids[ParamIDs::FilterShape])));
    s.closedAtHz = raw(ids[ParamIDs::FilterClosedAt]);
    s.resonance = raw(ids[ParamIDs::FilterResonance]);
    s.follow = raw(ids[ParamIDs::FilterFollow]);
    s.sweep = raw(ids[ParamIDs::FilterSweep]);
    s.rateHz = raw(ids[ParamIDs::FilterRate]);
    const float f = raw(ParamIDs::bus[m_bus][ParamIDs::Filter]);
    const auto fs = static_cast<float>(renderRate());
    busview::FilterLook look;
    look.type = s.type;
    look.steep = s.steep;
    // At rest: the whoosh at nothing and the rhythm at its middle, which is where it sits closed.
    look.hz = SweepFilter::cutoffHz(s, f, 0.0f, 0.0f, fs);
    look.q = SweepFilter::q(s, f);
    look.mix = busview::filterMix(f);
    look.trim = SweepFilter::trim(look.q);
    const float a = SweepFilter::placeHz(s, SweepFilter::openHz(s, f, 0.0f, -1.0f, fs), fs);
    const float b = SweepFilter::placeHz(s, SweepFilter::openHz(s, f, 1.0f, 1.0f, fs), fs);
    look.lowHz = juce::jmin(a, b);
    look.highHz = juce::jmax(a, b);
    return look;
}

void BusViewPlate::refresh()
{
    if (! Theme::isShownInWindow(*this) || getWidth() <= 2 * c_graphX)
        return;
    const auto& view = m_processor.getEngine().getBusView(m_bus);
    const auto tick = view.tick.load(std::memory_order_acquire);
    if (tick != m_lastTick)
    {
        m_lastTick = tick;
        m_stillFrames = 0;
    }
    else if (m_stillFrames < c_restFrames)
        ++m_stillFrames;
    const bool wasResting = m_resting, wasLit = m_lit, wasQuiet = m_spectrumQuiet;
    const auto oldFilter = m_filter;
    const auto oldSwirl = m_swirl;
    m_resting = m_stillFrames >= c_restFrames;
    bool moved = false;
    if (m_kind == Kind::Filter)
    {
        refreshFilter();
        moved = ! busview::same(m_filter, oldFilter) || ! m_spectrumQuiet || ! wasQuiet;
    }
    else
    {
        const auto written = view.swirlTrace.written() + view.passTrace.written();
        refreshSwirl();
        moved = ! busview::same(m_swirl, oldSwirl) || written != m_traceWritten;
        m_traceWritten = written;
    }
    auto row = rowText();
    moved = moved || row != m_row || m_resting != wasResting || m_lit != wasLit || m_dirty;
    m_row = std::move(row);
    // Only when something on it changed: a plate over a rig at rest costs nothing.
    if (moved)
    {
        m_dirty = false;
        ++m_repaintsAsked;
        repaint();
    }
}

void BusViewPlate::refreshFilter()
{
    const auto& view = m_processor.getEngine().getBusView(m_bus);
    m_filter = m_resting ? filterFromParameters() : view.filter();
    m_lit = m_filter.mix > 1.0e-4f;
    const double fs = renderRate();
    const auto plot = plotOf(graphArea());
    const float top = c_filterTopDb, foot = c_filterBottomDb;
    const auto clampY = [&](float db) { return juce::jlimit(plot.getY() - 3.0f, plot.getBottom() + 3.0f, yForDb(db, plot, top, foot)); };

    // The curves: the filter, filled to the foot, and what the bus hears.
    m_curve.clear();
    m_curveFill.clear();
    m_heard.clear();
    m_curveFill.startNewSubPath(plot.getX(), plot.getBottom() + 3.0f);
    const float step = 1.5f;
    for (int n = 0;; ++n)
    {
        const float at = juce::jmin(plot.getX() + step * static_cast<float>(n), plot.getRight());
        const double hz = static_cast<double>(hzForX(at, plot));
        const float y = clampY(dbOf(busview::filterGain(m_filter, hz, fs)));
        const float yh = clampY(dbOf(busview::heardGain(m_filter, hz, fs)));
        if (n == 0)
        {
            m_curve.startNewSubPath(at, y);
            m_heard.startNewSubPath(at, yh);
        }
        else
        {
            m_curve.lineTo(at, y);
            m_heard.lineTo(at, yh);
        }
        m_curveFill.lineTo(at, y);
        if (at >= plot.getRight())
            break;
    }
    m_curveFill.lineTo(plot.getRight(), plot.getBottom() + 3.0f);
    m_curveFill.closeSubPath();

    const float hzNow = juce::jlimit(c_minHz, c_maxHz, m_filter.hz);
    m_markerX = std::round(xForHz(hzNow, plot));
    m_markerY = clampY(dbOf(busview::filterGain(m_filter, static_cast<double>(hzNow), fs)));
    // Deeper than the frequency labels at the foot (a notch's middle): marked just over them, so it is never hidden.
    m_markerY = juce::jmin(m_markerY, graphArea().getBottom() - 1.0f - 13.0f - 6.0f);
    m_lowX = xForHz(juce::jlimit(c_minHz, c_maxHz, m_filter.lowHz), plot);
    m_highX = xForHz(juce::jlimit(c_minHz, c_maxHz, m_filter.highHz), plot);

    // The bus's sound as the filter leaves it, behind: each column rises fast and falls slowly, as the sea's does.
    bool quiet = true;
    if (! m_resting)
    {
        view.tap.copy(m_samples.data(), busview::Tap::c_size);
        m_spectrum->read(m_samples.data(), static_cast<float>(fs));
    }
    const float topHz = 0.45f * static_cast<float>(fs), bin = static_cast<float>(fs) / static_cast<float>(OutputSpectrum::c_fftSize);
    for (size_t i = 0; i < m_levels.size(); ++i)
    {
        const float x = plot.getX() + static_cast<float>(i) * static_cast<float>(c_spectrumStep);
        const float f0 = hzForX(x - 0.5f * c_spectrumStep, plot), f1 = hzForX(x + 0.5f * c_spectrumStep, plot);
        float v = 0.0f;
        if (! m_resting && f0 < topHz)
            v = f1 - f0 > bin ? m_spectrum->band(f0, f1) : m_spectrum->at(hzForX(x, plot));
        auto& s = m_levels[i];
        s += (v - s) * (v > s ? 0.5f : 0.15f);
        if (s < 0.002f)
            s = 0.0f;
        quiet = quiet && s <= 0.0f;
    }
    m_spectrumQuiet = quiet;
}

void BusViewPlate::refreshSwirl()
{
    const auto& view = m_processor.getEngine().getBusView(m_bus);
    m_swirl = view.swirl();
    if (m_resting)
    {
        // At rest nothing moves: the last state the rig was in, under the type and the wheel as they are set now.
        auto& state = m_processor.getState();
        m_swirl.type = juce::jlimit(0, ParamIDs::numSwirlTypes - 1, juce::roundToInt(state.getRawParameterValue(ParamIDs::busDetail[m_bus][ParamIDs::SwirlType])->load()));
        m_swirl.knob = state.getRawParameterValue(ParamIDs::bus[m_bus][ParamIDs::Swirl])->load();
    }
    const bool tapeOrDoubler = m_swirl.type == busview::SwirlTape || m_swirl.type == busview::SwirlDoubler;
    m_lit = m_swirl.knob > 1.0e-4f && (tapeOrDoubler || m_swirl.on);

    // The traces, newest last; only the points written in the type in use.
    m_traceAny = false;
    m_traceMin = 1.0e30f;
    m_traceMax = -1.0e30f;
    for (int age = 0; age < busview::Trace::c_points; ++age)
    {
        const auto i = static_cast<size_t>(busview::Trace::c_points - 1 - age);
        float lo = 0.0f, hi = 0.0f, nearLo = 0.0f, nearHi = 0.0f;
        int kind = -1, nearKind = -1;
        view.swirlTrace.point(age, lo, hi, kind);
        view.passTrace.point(age, nearLo, nearHi, nearKind);
        const bool has = age < static_cast<int>(view.swirlTrace.written()) && kind == m_swirl.type;
        m_traceHas[i] = has;
        m_traceLo[i] = lo;
        m_traceHi[i] = hi;
        m_near[i] = age < static_cast<int>(view.passTrace.written()) ? juce::jlimit(0.0f, 1.0f, nearHi) : 0.0f;
        if (has)
        {
            m_traceAny = true;
            m_traceMin = juce::jmin(m_traceMin, lo);
            m_traceMax = juce::jmax(m_traceMax, hi);
        }
    }

    // The response now: in each column of pixels, the lowest and the highest it reaches across the column's
    // frequencies (a comb's notches crowd together at the top), filled between, the highest stroked.
    m_curve.clear();
    m_curveFill.clear();
    if (m_swirl.type == busview::SwirlDoubler)
        return;
    const double fs = renderRate();
    const auto plot = plotOf(responsePane());
    const float top = c_swirlTopDb, foot = c_swirlBottomDb;
    const auto clampY = [&](float db) { return juce::jlimit(plot.getY() - 3.0f, plot.getBottom() + 3.0f, yForDb(db, plot, top, foot)); };
    std::vector<juce::Point<float>> lows;
    const int columns = juce::jmax(2, juce::roundToInt(plot.getWidth()));
    lows.reserve(static_cast<size_t>(columns) + 1);
    constexpr int c_subs = 6;
    for (int c = 0; c <= columns; ++c)
    {
        const float x0 = plot.getX() + static_cast<float>(c) - 0.5f, x = plot.getX() + static_cast<float>(c);
        float lo = 1.0e9f, hi = -1.0e9f;
        for (int k = 0; k < c_subs; ++k)
        {
            const double hz = static_cast<double>(hzForX(x0 + (static_cast<float>(k) + 0.5f) / static_cast<float>(c_subs), plot));
            const float db = dbOf(std::abs(busview::swirlTransfer(m_swirl, hz, fs)));
            lo = juce::jmin(lo, db);
            hi = juce::jmax(hi, db);
        }
        const float yHi = clampY(hi), yLo = clampY(lo);
        if (c == 0)
        {
            m_curve.startNewSubPath(x, yHi);
            m_curveFill.startNewSubPath(x, yHi);
        }
        else
        {
            m_curve.lineTo(x, yHi);
            m_curveFill.lineTo(x, yHi);
        }
        lows.push_back({ x, juce::jmax(yLo, yHi + 1.0f) });
    }
    for (auto it = lows.rbegin(); it != lows.rend(); ++it)
        m_curveFill.lineTo(*it);
    m_curveFill.closeSubPath();
}

juce::String BusViewPlate::rowText() const
{
    if (m_kind == Kind::Filter)
    {
        if (! m_lit)
            return "Filter is at 0%: the bus is heard without it.";
        if (m_resting)
            return "Not playing: drawn where it sits closed.";
        return "Travel " + hzText(m_filter.lowHz) + " to " + hzText(m_filter.highHz);
    }
    if (m_swirl.knob <= 1.0e-4f)
        return "Swirl is at 0%: none of it is heard.";
    juce::String line;
    switch (m_swirl.type)
    {
        case busview::SwirlTape:
        {
            const float d = m_swirl.offsetMs;
            if (std::abs(d) >= 0.05f)
                line = "The copy is " + msText(d) + (d > 0.0f ? " behind the dry." : " ahead of the dry.");
            else if (m_swirl.wetGain >= 0.0f)
                line = "The copy is level with the dry, so they add.";
            else // inverted: they cancel only where the two are as loud (the wheel at 100%)
                line = std::abs(m_swirl.dryGain + m_swirl.wetGain) < 0.05f ? "The copy is level with the dry, inverted: they cancel."
                                                                           : "The copy is level with the dry, inverted: they thin out.";
            break;
        }
        case busview::SwirlDoubler:
            line = "The copy is " + msText(m_swirl.offsetMs) + " behind"
                   + (m_traceAny && m_traceMax - m_traceMin >= 0.1f ? ", wandering " + juce::String(m_traceMin, 1) + " to " + msText(m_traceMax) + "." : juce::String("."));
            break;
        case busview::SwirlPhaser:
            line = "The phaser is tuned to " + hzText(m_swirl.phaserHz) + ".";
            break;
        case busview::SwirlFlanger:
            line = "A delay of " + msText(m_swirl.flangerMs) + ": its notches are " + hzText(1000.0f / juce::jmax(0.05f, m_swirl.flangerMs)) + " apart.";
            break;
        default:
        {
            // The all-passes turn near the top of the band here: say where the phaser's lowest notch is, or that its
            // notches are all above hearing.
            const auto notch = static_cast<float>(busview::phaserFirstNotchHz(static_cast<double>(m_swirl.phaserHz), renderRate()));
            line = "Delay " + msText(m_swirl.flangerMs)
                   + (notch < c_maxHz ? ", the phaser's first notch at " + hzText(notch) + "." : juce::String(", the phaser's notches above 20 kHz."));
            break;
        }
    }
    if (m_resting)
        return "Not playing. " + line;
    return line;
}

//==============================================================================
void BusViewPlate::paint(juce::Graphics& g)
{
    // The plate, with room under it for its shadow, and a tally of the bus's colour along its head (as the EQ's).
    const auto plate = getLocalBounds().toFloat().withTrimmedBottom(3.0f);
    SwooshLookAndFeel::drawSurface(g, plate, Theme::Cut::panel, TC::panel);
    if (auto* probe = Theme::textProbe())
        probe->covered(g, plate); // (the checker: the texts under it are not seen)
    g.setColour(m_accent);
    g.fillRect(juce::Rectangle<float>(plate.getX() + 1.0f, plate.getY() + 1.0f, plate.getWidth() - 2.0f, 2.0f));
    g.setColour(TC::edgeDark);
    g.fillRect(juce::Rectangle<float>(plate.getX() + 1.0f, plate.getY() + 3.0f, plate.getWidth() - 2.0f, 1.0f));
    if (m_kind == Kind::Filter)
        paintFilter(g);
    else
        paintSwirl(g);
}

void BusViewPlate::paintFrequencyGrid(juce::Graphics& g, juce::Rectangle<float> well, juce::Rectangle<float> plot, float topDb, float bottomDb,
                                      std::initializer_list<float> marked)
{
    const auto inside = well.reduced(1.0f);
    for (const float hz : { 50.0f, 200.0f, 500.0f, 2000.0f, 5000.0f })
    {
        g.setColour(TC::grid.withAlpha(0.45f));
        g.fillRect(juce::Rectangle<float>(std::round(xForHz(hz, plot)), inside.getY(), 1.0f, inside.getHeight()));
    }
    for (const float hz : { 100.0f, 1000.0f, 10000.0f })
    {
        g.setColour(TC::grid);
        g.fillRect(juce::Rectangle<float>(std::round(xForHz(hz, plot)), inside.getY(), 1.0f, inside.getHeight()));
    }
    for (const float db : marked)
    {
        g.setColour(db == 0.0f ? TC::stroke : TC::grid);
        g.fillRect(juce::Rectangle<float>(plot.getX(), std::round(yForDb(db, plot, topDb, bottomDb)), plot.getWidth(), 1.0f));
    }
}

void BusViewPlate::paintFrequencyLabels(juce::Graphics& g, juce::Rectangle<float> well, juce::Rectangle<float> plot)
{
    const auto inside = well.reduced(1.0f);
    const auto font = m_fonts.caption();
    const float labelHeight = 12.0f;
    static const std::pair<float, const char*> decades[] = { { 100.0f, "100 Hz" }, { 1000.0f, "1 kHz" }, { 10000.0f, "10 kHz" } };
    for (const auto& [hz, text] : decades)
        graphLabel(g, text, juce::Rectangle<float>(std::round(xForHz(hz, plot)) + 4.0f, inside.getBottom() - labelHeight - 1.0f, 40.0f, labelHeight),
                   font, juce::Justification::centredLeft, TC::textFaint);
}

void BusViewPlate::paintFilter(juce::Graphics& g)
{
    const auto well = graphArea();
    SwooshLookAndFeel::drawWell(g, well);
    const auto plot = plotOf(well);
    const auto inside = well.reduced(1.0f);
    const float top = c_filterTopDb, foot = c_filterBottomDb;
    paintFrequencyGrid(g, well, plot, top, foot, { 12.0f, 0.0f, -24.0f });

    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(inside.getSmallestIntegerContainer());
        // The bus's sound, faint behind.
        if (! m_spectrumQuiet && ! m_levels.empty())
        {
            juce::Path shape;
            shape.preallocateSpace(static_cast<int>(m_levels.size()) * 3 + 8);
            shape.startNewSubPath(plot.getX(), inside.getBottom());
            for (size_t i = 0; i < m_levels.size(); ++i)
                shape.lineTo(plot.getX() + static_cast<float>(i) * static_cast<float>(c_spectrumStep), inside.getBottom() - m_levels[i] * (inside.getHeight() - 6.0f));
            shape.lineTo(plot.getX() + static_cast<float>(m_levels.size() - 1) * static_cast<float>(c_spectrumStep), inside.getBottom());
            shape.closeSubPath();
            g.setColour(TC::text.withAlpha(0.09f));
            g.fillPath(shape);
        }
        // The travel: where the cutoff can go with the front wheel where it is.
        if (m_lit && m_highX - m_lowX >= 1.0f)
        {
            g.setColour(m_accent.withAlpha(0.1f));
            g.fillRect(juce::Rectangle<float>(m_lowX, inside.getY(), m_highX - m_lowX, inside.getHeight()));
            g.setColour(m_accent.withAlpha(0.45f));
            g.fillRect(juce::Rectangle<float>(std::round(m_lowX), inside.getY(), 1.0f, inside.getHeight()));
            g.fillRect(juce::Rectangle<float>(std::round(m_highX), inside.getY(), 1.0f, inside.getHeight()));
        }
        // The filter, then what is heard, then the marker at its place.
        g.setColour(m_accent.withAlpha(m_lit ? 0.12f : 0.04f));
        g.fillPath(m_curveFill);
        g.setColour(m_lit ? m_accent : TC::textFaint.withAlpha(0.7f));
        g.strokePath(m_curve, lineStroke(m_lit ? 1.6f : 1.2f));
        if (showsHeard())
        {
            g.setColour(TC::text.withAlpha(0.8f));
            g.strokePath(m_heard, lineStroke(1.0f));
        }
        g.setColour((m_lit ? m_accent : TC::textFaint).withAlpha(0.45f));
        g.fillRect(juce::Rectangle<float>(m_markerX, inside.getY(), 1.0f, inside.getHeight()));
        const auto dot = juce::Rectangle<float>(7.0f, 7.0f).withCentre({ m_markerX + 0.5f, std::round(m_markerY) });
        g.setColour(TC::edgeDark);
        g.fillRect(dot.expanded(1.0f));
        g.setColour(m_lit ? m_accent : TC::textFaint);
        g.fillRect(dot);
    }

    // The labels, last, each on its own patch of the well.
    {
        const auto font = m_fonts.caption();
        const float labelHeight = 12.0f;
        paintFrequencyLabels(g, well, plot);
        graphLabel(g, "+12 dB", juce::Rectangle<float>(plot.getX() + 2.0f, std::round(yForDb(12.0f, plot, top, foot)) - labelHeight - 1.0f, 40.0f, labelHeight), font,
                   juce::Justification::centredLeft, TC::textFaint);
        graphLabel(g, "-24 dB", juce::Rectangle<float>(plot.getX() + 2.0f, std::round(yForDb(-24.0f, plot, top, foot)) - labelHeight - 1.0f, 40.0f, labelHeight), font,
                   juce::Justification::centredLeft, TC::textFaint);
        // The key to the two lines, in the corner opposite the scale.
        const auto key = juce::Rectangle<float>(plot.getRight() - 104.0f, inside.getY() + 2.0f, 104.0f, labelHeight);
        g.setColour(TC::well);
        g.fillRect(key.expanded(2.0f, 0.0f).getSmallestIntegerContainer());
        auto left = key.withWidth(52.0f), right = key.withTrimmedLeft(52.0f);
        g.setColour(m_lit ? m_accent : TC::textFaint);
        g.fillRect(juce::Rectangle<float>(left.getX(), std::round(left.getCentreY()) - 1.0f, 8.0f, 2.0f));
        Theme::fitText(g, "Filter", left.withTrimmedLeft(11.0f), font, juce::Justification::centredLeft);
        if (showsHeard())
        {
            g.setColour(TC::text.withAlpha(0.8f));
            g.fillRect(juce::Rectangle<float>(right.getX(), std::round(right.getCentreY()) - 0.5f, 8.0f, 1.0f));
            Theme::fitText(g, "Heard", right.withTrimmedLeft(11.0f), font, juce::Justification::centredLeft);
        }
    }

    // The row: the type and slope, where it is and its Q, then the travel (or why there is nothing to see).
    auto row = rowArea().toFloat();
    g.setColour(m_lit ? m_accent : TC::textDim);
    juce::String name = ParamIDs::filterTypeNames[juce::jlimit(0, ParamIDs::numFilterTypes - 1, m_filter.type)];
    if (m_filter.steep)
        name << ", 24 dB";
    Theme::fitText(g, name, row.removeFromLeft(116.0f), m_fonts.title(), juce::Justification::centredLeft);
    if (m_lit && ! m_resting)
    {
        g.setColour(TC::text);
        Theme::fitText(g, "At " + hzText(m_filter.hz), row.removeFromLeft(84.0f), m_fonts.body(), juce::Justification::centredLeft);
        Theme::fitText(g, "Q " + juce::String(m_filter.q, 1), row.removeFromLeft(48.0f), m_fonts.body(), juce::Justification::centredLeft);
        g.setColour(TC::textDim);
        Theme::fitText(g, m_row, row, m_fonts.body(), juce::Justification::centredLeft);
        return;
    }
    g.setColour(TC::textDim);
    Theme::fitText(g, m_row, row, m_fonts.body(), juce::Justification::centredLeft, Theme::Fit::Sentence);
}

void BusViewPlate::paintTrace(juce::Graphics& g, juce::Rectangle<float> well)
{
    SwooshLookAndFeel::drawWell(g, well);
    const auto plot = plotOf(well);
    const auto inside = well.reduced(1.0f);
    const auto scale = traceScale(m_swirl.type);
    const auto yFor = [&](float v) { return plot.getY() + (scale.top - v) / (scale.top - scale.foot) * plot.getHeight(); };
    const float column = plot.getWidth() / static_cast<float>(busview::Trace::c_points);

    // Seconds, from the newest at the right.
    for (int s = 1; s < static_cast<int>(busview::Trace::c_seconds); ++s)
    {
        g.setColour(TC::grid.withAlpha(0.6f));
        g.fillRect(juce::Rectangle<float>(std::round(plot.getRight() - plot.getWidth() * static_cast<float>(s) / static_cast<float>(busview::Trace::c_seconds)),
                                          inside.getY(), 1.0f, inside.getHeight()));
    }
    // Where the copy is level with the dry (Tape Flanger), or the foot of the scale.
    const bool tape = m_swirl.type == busview::SwirlTape;
    if (tape)
    {
        g.setColour(m_accent.withAlpha(0.55f));
        g.fillRect(juce::Rectangle<float>(plot.getX(), std::round(yFor(0.0f)), plot.getWidth(), 1.0f));
    }
    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(inside.getSmallestIntegerContainer());
        // How near the bus's passes are, faint from the foot.
        // (Each as one list of rectangles: one fill, not one per point.)
        juce::RectangleList<float> near, trace;
        near.ensureStorageAllocated(static_cast<int>(m_near.size()));
        trace.ensureStorageAllocated(static_cast<int>(m_traceHas.size()));
        for (size_t i = 0; i < m_near.size(); ++i)
            if (m_near[i] > 0.01f)
            {
                const float h = m_near[i] * (inside.getHeight() - 4.0f);
                near.addWithoutMerging(juce::Rectangle<float>(plot.getX() + static_cast<float>(i) * column, inside.getBottom() - h, column + 0.3f, h));
            }
        g.setColour(TC::text.withAlpha(0.07f));
        g.fillRectList(near);
        // The trace: each point its lowest to its highest.
        for (size_t i = 0; i < m_traceHas.size(); ++i)
            if (m_traceHas[i])
            {
                const float y0 = yFor(m_traceHi[i]), y1 = yFor(m_traceLo[i]);
                const float h = juce::jmax(1.5f, y1 - y0);
                trace.addWithoutMerging(juce::Rectangle<float>(plot.getX() + static_cast<float>(i) * column,
                                                               juce::jlimit(inside.getY(), inside.getBottom() - h, 0.5f * (y0 + y1) - 0.5f * h), column + 0.3f, h));
            }
        g.setColour(m_lit ? m_accent : TC::textFaint.withAlpha(0.7f));
        g.fillRectList(trace);
    }
    // The labels, last, each on its own patch of the well ("Through Zero" at the old end of its line, where the
    // newest of the trace is not).
    const auto font = m_fonts.caption();
    const float labelHeight = 12.0f;
    graphLabel(g, scale.topLabel, juce::Rectangle<float>(plot.getX() + 2.0f, inside.getY() + 2.0f, 76.0f, labelHeight), font, juce::Justification::centredLeft,
               TC::textFaint);
    graphLabel(g, scale.footLabel, juce::Rectangle<float>(plot.getX() + 2.0f, inside.getBottom() - labelHeight - 1.0f, 76.0f, labelHeight), font,
               juce::Justification::centredLeft, TC::textFaint);
    graphLabel(g, "Passes", juce::Rectangle<float>(plot.getRight() - 46.0f, inside.getBottom() - labelHeight - 1.0f, 46.0f, labelHeight), font,
               juce::Justification::centredRight, TC::textFaint);
    if (tape)
        graphLabel(g, "Through Zero", juce::Rectangle<float>(plot.getX() + 2.0f, std::round(yFor(0.0f)) - labelHeight - 1.0f, 76.0f, labelHeight), font,
                   juce::Justification::centredLeft, m_accent);
    graphLabel(g, scale.title, juce::Rectangle<float>(plot.getRight() - 96.0f, inside.getY() + 2.0f, 96.0f, labelHeight), font, juce::Justification::centredRight,
               TC::textDim);
}

void BusViewPlate::paintSwirl(juce::Graphics& g)
{
    paintTrace(g, tracePane());
    if (m_swirl.type != busview::SwirlDoubler)
    {
        const auto well = responsePane();
        SwooshLookAndFeel::drawWell(g, well);
        const auto plot = plotOf(well);
        const auto inside = well.reduced(1.0f);
        const float top = c_swirlTopDb, foot = c_swirlBottomDb;
        paintFrequencyGrid(g, well, plot, top, foot, { 0.0f, -12.0f, -24.0f });
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(inside.getSmallestIntegerContainer());
            g.setColour((m_lit ? m_accent : TC::textFaint).withAlpha(m_lit ? 0.35f : 0.15f));
            g.fillPath(m_curveFill);
            g.setColour(m_lit ? m_accent : TC::textFaint.withAlpha(0.7f));
            g.strokePath(m_curve, lineStroke(m_lit ? 1.4f : 1.1f));
        }
        const auto font = m_fonts.caption();
        const float labelHeight = 12.0f;
        paintFrequencyLabels(g, well, plot);
        graphLabel(g, "0 dB", juce::Rectangle<float>(plot.getX() + 2.0f, std::round(yForDb(0.0f, plot, top, foot)) - labelHeight - 1.0f, 40.0f, labelHeight), font,
                   juce::Justification::centredLeft, TC::textFaint);
        graphLabel(g, "-24 dB", juce::Rectangle<float>(plot.getX() + 2.0f, std::round(yForDb(-24.0f, plot, top, foot)) - labelHeight - 1.0f, 40.0f, labelHeight), font,
                   juce::Justification::centredLeft, TC::textFaint);
        graphLabel(g, "Response", juce::Rectangle<float>(plot.getRight() - 70.0f, inside.getY() + 2.0f, 70.0f, labelHeight), font, juce::Justification::centredRight,
                   TC::textDim);
    }

    auto row = rowArea().toFloat();
    g.setColour(m_lit ? m_accent : TC::textDim);
    Theme::fitText(g, ParamIDs::swirlTypeNames[juce::jlimit(0, ParamIDs::numSwirlTypes - 1, m_swirl.type)], row.removeFromLeft(116.0f), m_fonts.title(),
                   juce::Justification::centredLeft);
    g.setColour(m_lit ? TC::text : TC::textDim);
    Theme::fitText(g, m_row, row, m_fonts.body(), juce::Justification::centredLeft, Theme::Fit::Sentence);
}

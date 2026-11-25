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
    /** For a line with sharp turns (a comb's teeth, a trace through zero): a mitered corner would reach past the
        point it turns at, and draw a notch deeper than it is. */
    juce::PathStrokeType toothStroke(float thickness)
    {
        return juce::PathStrokeType(thickness, juce::PathStrokeType::beveled, juce::PathStrokeType::butt);
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

    /** The square that marks where something is now: stamped as the EQ's handles are (a dark seat, a light line along
        its top, a dark one along its foot), on whole pixels so its edges are hard. */
    void stampedMarker(juce::Graphics& g, juce::Point<float> centre, float side, juce::Colour colour)
    {
        const auto box = juce::Rectangle<float>(side, side).withCentre({ std::round(centre.x) + 0.5f, std::round(centre.y) + 0.5f });
        g.setColour(TC::edgeDark);
        g.fillRect(box.expanded(1.0f));
        g.setColour(colour);
        g.fillRect(box);
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.fillRect(box.withHeight(1.0f));
        g.setColour(colour.darker(0.45f));
        g.fillRect(box.withTop(box.getBottom() - 1.0f));
    }

    /** A well with its margins: outside the plot (the axis' two ends, or the oldest and the newest of a trace) it is
        a shade darker, as the Output EQ's graph is outside 20 Hz and 20 kHz. */
    void drawGraphWell(juce::Graphics& g, juce::Rectangle<float> well, juce::Rectangle<float> plot)
    {
        SwooshLookAndFeel::drawWell(g, well);
        const auto inside = well.reduced(1.0f);
        g.setColour(TC::shadow.withAlpha(0.45f));
        g.fillRect(inside.withRight(plot.getX()));
        g.fillRect(inside.withLeft(plot.getRight()));
    }

    /** The colour of what is faint behind a graph (the bus's spectrum, its passes): the scale's own grey-blue, so the
        bus's colour is kept for what the wheel is doing. */
    const juce::Colour c_behind = TC::textFaint;

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
                         "Where a comb's teeth are too close together to draw, the band they cover is drawn instead. "
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
    // (Past the scale a curve leaves the well, which clips it: it does not run along the edge as if it levelled off.)
    const float over = graphArea().getY() - 4.0f, under = graphArea().getBottom() + 4.0f;
    const auto clampY = [&](float db) { return juce::jlimit(over, under, yForDb(db, plot, top, foot)); };

    // The curves: the filter, filled to the foot, and what the bus hears.
    m_curve.clear();
    m_curveFill.clear();
    m_heard.clear();
    m_curveFill.startNewSubPath(plot.getX(), under);
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
    m_curveFill.lineTo(plot.getRight(), under);
    m_curveFill.closeSubPath();

    const float hzNow = juce::jlimit(c_minHz, c_maxHz, m_filter.hz);
    m_markerX = std::round(xForHz(hzNow, plot));
    m_markerY = clampY(dbOf(busview::filterGain(m_filter, static_cast<double>(hzNow), fs)));
    // Deeper than the frequency labels at the foot (a notch's middle): marked just over them, so it is never hidden;
    // and over the top of the scale, at the top.
    m_markerY = juce::jlimit(graphArea().getY() + 6.0f, graphArea().getBottom() - 1.0f - 13.0f - 6.0f, m_markerY);
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

    // Its shape: each column evened with its two neighbours (the picture only), filled, with a line along its top.
    m_spectrumShape.clear();
    m_spectrumLine.clear();
    if (! quiet && m_levels.size() >= 2)
    {
        const auto inside = graphArea().reduced(1.0f);
        const size_t last = m_levels.size() - 1;
        m_spectrumShape.startNewSubPath(plot.getX(), inside.getBottom() + 2.0f);
        for (size_t i = 0; i <= last; ++i)
        {
            const float v = 0.25f * m_levels[i > 0 ? i - 1 : i] + 0.5f * m_levels[i] + 0.25f * m_levels[i < last ? i + 1 : i];
            const float x = juce::jmin(plot.getRight(), plot.getX() + static_cast<float>(i) * static_cast<float>(c_spectrumStep));
            const float y = inside.getBottom() + 1.0f - v * (inside.getHeight() - 5.0f);
            m_spectrumShape.lineTo(x, y);
            if (i == 0)
                m_spectrumLine.startNewSubPath(x, y);
            else
                m_spectrumLine.lineTo(x, y);
        }
        m_spectrumShape.lineTo(juce::jmin(plot.getRight(), plot.getX() + static_cast<float>(last) * static_cast<float>(c_spectrumStep)), inside.getBottom() + 2.0f);
        m_spectrumShape.closeSubPath();
    }
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

    buildTrace();
    buildResponse();
}

void BusViewPlate::buildTrace()
{
    // The trace's picture: the passes as one shape with a line along its top, and the trace as one line through its
    // points, newest at the right, with a square on the newest.
    const auto well = tracePane();
    const auto plot = plotOf(well);
    const auto inside = well.reduced(1.0f);
    const auto scale = traceScale(m_swirl.type);
    const float column = plot.getWidth() / static_cast<float>(busview::Trace::c_points);
    const auto yFor = [&](float v)
    {
        return juce::jlimit(inside.getY() + 1.0f, inside.getBottom() - 1.0f, plot.getY() + (scale.top - v) / (scale.top - scale.foot) * plot.getHeight());
    };
    const auto xFor = [&](size_t i) { return plot.getX() + (static_cast<float>(i) + 0.5f) * column; };
    constexpr size_t count = static_cast<size_t>(busview::Trace::c_points);

    m_nearShape.clear();
    m_nearLine.clear();
    m_nearAny = false;
    for (size_t i = 0; i < count && ! m_nearAny; ++i)
        m_nearAny = m_near[i] > 0.01f;
    if (m_nearAny)
    {
        const float foot = inside.getBottom() + 2.0f;
        const auto yNear = [&](size_t i) { return inside.getBottom() + 1.0f - m_near[i] * (inside.getHeight() - 4.0f); };
        m_nearShape.startNewSubPath(plot.getX(), foot);
        m_nearShape.lineTo(plot.getX(), yNear(0));
        m_nearLine.startNewSubPath(plot.getX(), yNear(0));
        for (size_t i = 0; i < count; ++i)
        {
            m_nearShape.lineTo(xFor(i), yNear(i));
            m_nearLine.lineTo(xFor(i), yNear(i));
        }
        m_nearShape.lineTo(plot.getRight(), yNear(count - 1));
        m_nearLine.lineTo(plot.getRight(), yNear(count - 1));
        m_nearShape.lineTo(plot.getRight(), foot);
        m_nearShape.closeSubPath();
    }

    // Each point is the lowest and the highest the value reached in its sixtieth of a second. The line goes through
    // each point's middle, which is where a value on its way up or down crosses it, and through its highest or its
    // lowest where it turned there; a point whose range the line does not cover (it wobbled faster than the points
    // come) has that range drawn behind the line.
    m_traceLine.clear();
    m_traceRange.clear();
    m_nowHas = false;
    std::array<float, count> at {};
    const auto mid = [&](size_t i) { return 0.5f * (m_traceLo[i] + m_traceHi[i]); };
    for (size_t i = 0; i < count; ++i)
    {
        at[i] = mid(i);
        if (m_traceHas[i] && i > 0 && i + 1 < count && m_traceHas[i - 1] && m_traceHas[i + 1])
        {
            if (mid(i) >= mid(i - 1) && mid(i) >= mid(i + 1))
                at[i] = m_traceHi[i];
            else if (mid(i) <= mid(i - 1) && mid(i) <= mid(i + 1))
                at[i] = m_traceLo[i];
        }
    }
    bool open = false;
    for (size_t i = 0; i < count; ++i)
    {
        if (! m_traceHas[i])
        {
            open = false;
            continue;
        }
        const bool before = i > 0 && m_traceHas[i - 1], after = i + 1 < count && m_traceHas[i + 1];
        const float x = xFor(i), y = yFor(at[i]);
        if (! open)
            m_traceLine.startNewSubPath(x, y);
        else
            m_traceLine.lineTo(x, y);
        open = true;
        // What the line covers of this point's column, against the point's whole range.
        float coveredTop = y, coveredFoot = y;
        if (before)
        {
            const float joint = 0.5f * (y + yFor(at[i - 1]));
            coveredTop = juce::jmin(coveredTop, joint);
            coveredFoot = juce::jmax(coveredFoot, joint);
        }
        if (after)
        {
            const float joint = 0.5f * (y + yFor(at[i + 1]));
            coveredTop = juce::jmin(coveredTop, joint);
            coveredFoot = juce::jmax(coveredFoot, joint);
        }
        const float top = yFor(m_traceHi[i]), foot = yFor(m_traceLo[i]);
        if (top < coveredTop - 1.5f || foot > coveredFoot + 1.5f || (! before && ! after))
        {
            const float h = juce::jmax(1.5f, foot - top);
            m_traceRange.addWithoutMerging(juce::Rectangle<float>(plot.getX() + static_cast<float>(i) * column, 0.5f * (top + foot) - 0.5f * h, column + 0.3f, h));
        }
        if (i == count - 1)
        {
            m_traceLine.lineTo(plot.getRight(), y);
            m_nowHas = true;
            m_now = { plot.getRight(), y };
        }
    }
}

void BusViewPlate::buildResponse()
{
    // The response now, worked out six times across every pixel. Where its peaks are far enough apart to see, it is
    // one line through each pixel's lowest and highest (so a notch keeps its depth), tinted to the 0 dB line. Where a
    // comb's teeth come closer together than c_densePx (they crowd at the top of a log axis), drawing each would make
    // a solid block: from the first peak that close it is the band the teeth cover instead, between a line along
    // their peaks and one along their notches. The two lines are alike and lighter than the curve, which ends where
    // the band starts: the curve carried on along the peaks would read as a response that had levelled off there.
    m_curve.clear();
    m_curveFill.clear();
    m_combBand.clear();
    m_combHead.clear();
    m_combFoot.clear();
    if (m_swirl.type == busview::SwirlDoubler)
        return;
    const double fs = renderRate();
    const auto well = responsePane();
    const auto plot = plotOf(well);
    const float top = c_swirlTopDb, foot = c_swirlBottomDb;
    // (Past the scale it leaves the well, as the filter's does: Feedback right up makes peaks taller than the scale.)
    const auto clampY = [&](float db) { return juce::jlimit(well.getY() - 4.0f, well.getBottom() + 4.0f, yForDb(db, plot, top, foot)); };
    constexpr int c_subs = 6;
    const int columns = juce::jmax(2, juce::roundToInt(plot.getWidth()));
    const int count = columns * c_subs + 1;
    const auto xOf = [&](int k) { return plot.getX() + static_cast<float>(k) / static_cast<float>(c_subs); };
    m_db.resize(static_cast<size_t>(count));
    for (int k = 0; k < count; ++k)
        m_db[static_cast<size_t>(k)] = dbOf(std::abs(busview::swirlTransfer(m_swirl, static_cast<double>(hzForX(xOf(k), plot)), fs)));
    const auto db = [&](int k) { return m_db[static_cast<size_t>(k)]; };

    // Its peaks and its notches, and the first peak whose next is closer than c_densePx.
    m_peaks.clear();
    m_notches.clear();
    for (int k = 1; k + 1 < count; ++k)
    {
        if (db(k) > db(k - 1) && db(k) >= db(k + 1))
            m_peaks.push_back(k);
        else if (db(k) < db(k - 1) && db(k) <= db(k + 1))
            m_notches.push_back(k);
    }
    int denseFrom = count - 1;
    size_t densePeak = m_peaks.size();
    for (size_t j = 0; j + 3 < m_peaks.size(); ++j)
        if (static_cast<float>(m_peaks[j + 1] - m_peaks[j]) < c_densePx * static_cast<float>(c_subs))
        {
            denseFrom = m_peaks[j];
            densePeak = j;
            break;
        }

    // The line, and its tint to the 0 dB line.
    const float y0 = std::round(yForDb(0.0f, plot, top, foot)) + 0.5f;
    m_curveFill.startNewSubPath(plot.getX(), y0);
    bool started = false;
    const auto point = [&](int k)
    {
        const float x = xOf(k), y = clampY(db(k));
        if (! started)
            m_curve.startNewSubPath(x, y);
        else
            m_curve.lineTo(x, y);
        started = true;
        m_curveFill.lineTo(x, y);
    };
    for (int c = 0; c * c_subs < denseFrom; ++c)
    {
        const int from = c * c_subs, to = juce::jmin(from + c_subs, denseFrom);
        int lowest = from, highest = from;
        for (int k = from + 1; k < to; ++k)
        {
            if (db(k) < db(lowest))
                lowest = k;
            if (db(k) > db(highest))
                highest = k;
        }
        if (c == 0 && lowest != from && highest != from)
            point(from);
        point(juce::jmin(lowest, highest));
        if (lowest != highest)
            point(juce::jmax(lowest, highest));
    }
    point(denseFrom);
    m_curveFill.lineTo(xOf(denseFrom), y0);
    m_curveFill.closeSubPath();
    if (densePeak >= m_peaks.size())
        return;

    // The band: along the peaks to the right edge, and back along the notches. (Each peak and notch as the deepest
    // of itself and its two neighbours: six samples a pixel fall a little short of the tip of a tooth this fine.)
    m_combBand.startNewSubPath(xOf(denseFrom), clampY(db(denseFrom)));
    m_combHead.startNewSubPath(xOf(denseFrom), clampY(db(denseFrom)));
    float y = clampY(db(denseFrom));
    for (size_t j = densePeak + 1; j < m_peaks.size(); ++j)
    {
        float v = db(m_peaks[j]);
        if (j > densePeak + 1)
            v = juce::jmax(v, db(m_peaks[j - 1]));
        if (j + 1 < m_peaks.size())
            v = juce::jmax(v, db(m_peaks[j + 1]));
        y = clampY(v);
        m_combHead.lineTo(xOf(m_peaks[j]), y);
        m_combBand.lineTo(xOf(m_peaks[j]), y);
    }
    m_combHead.lineTo(plot.getRight(), y);
    m_combBand.lineTo(plot.getRight(), y);
    size_t first = 0;
    while (first < m_notches.size() && m_notches[first] <= denseFrom)
        ++first;
    m_combFoot.startNewSubPath(xOf(denseFrom), clampY(db(denseFrom)));
    m_lows.clear();
    for (size_t j = first; j < m_notches.size(); ++j)
    {
        float v = db(m_notches[j]);
        if (j > first)
            v = juce::jmin(v, db(m_notches[j - 1]));
        if (j + 1 < m_notches.size())
            v = juce::jmin(v, db(m_notches[j + 1]));
        m_lows.push_back({ xOf(m_notches[j]), clampY(v) });
        m_combFoot.lineTo(m_lows.back());
    }
    if (! m_lows.empty())
    {
        m_lows.push_back({ plot.getRight(), m_lows.back().y });
        m_combFoot.lineTo(m_lows.back());
    }
    for (auto it = m_lows.rbegin(); it != m_lows.rend(); ++it)
        m_combBand.lineTo(*it);
    m_combBand.closeSubPath();
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
            // The all-passes turn near the top of the band here: say where the phaser's lowest notch is (with its
            // Feedback, as the curve has it), or that its notches are all above hearing.
            const auto notch = static_cast<float>(busview::phaserFirstNotchHz(m_swirl, renderRate()));
            line = "Delay " + msText(m_swirl.flangerMs)
                   + (notch > 0.0f && notch < c_maxHz ? ", the phaser's first notch at " + hzText(notch) + "." : juce::String(", the phaser's notches above 20 kHz."));
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
    // What never moves (the plate, the wells, the grids) is kept as an image and put down in one go, while the
    // context's scale is a whole number: then the image's pixels are the context's, one for one. At any other scale
    // it is drawn afresh, so its hard edges fall exactly where they would.
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    const int layout = baseLayout();
    const bool whole = scale >= 0.999f && scale <= 3.001f && std::abs(scale - std::round(scale)) < 1.0e-3f && ! g.getInternalContext().isVectorDevice();
    if (whole)
    {
        const int s = juce::roundToInt(scale);
        if (m_base.isNull() || m_baseLayout != layout || m_base.getWidth() != getWidth() * s || m_base.getHeight() != getHeight() * s)
        {
            m_base = juce::Image(juce::Image::ARGB, getWidth() * s, getHeight() * s, true);
            m_baseLayout = layout;
            juce::Graphics ig(m_base);
            ig.addTransform(juce::AffineTransform::scale(static_cast<float>(s)));
            paintBase(ig);
        }
        juce::Graphics::ScopedSaveState state(g);
        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
        g.drawImageTransformed(m_base, juce::AffineTransform::scale(1.0f / static_cast<float>(s)));
    }
    else
        paintBase(g);
    if (auto* probe = Theme::textProbe())
        probe->covered(g, getLocalBounds().toFloat().withTrimmedBottom(3.0f)); // (the checker: the texts under it are not seen)
    if (m_kind == Kind::Filter)
        paintFilter(g);
    else
        paintSwirl(g);
}

int BusViewPlate::baseLayout() const
{
    return m_kind == Kind::Filter ? 0 : m_swirl.type == busview::SwirlDoubler ? 1 : 2;
}

void BusViewPlate::paintBase(juce::Graphics& g) const
{
    // The plate, with room under it for its shadow, and a tally of the bus's colour along its head (as the EQ's).
    const auto plate = getLocalBounds().toFloat().withTrimmedBottom(3.0f);
    SwooshLookAndFeel::drawSurface(g, plate, Theme::Cut::panel, TC::panel);
    g.setColour(m_accent);
    g.fillRect(juce::Rectangle<float>(plate.getX() + 1.0f, plate.getY() + 1.0f, plate.getWidth() - 2.0f, 2.0f));
    g.setColour(TC::edgeDark);
    g.fillRect(juce::Rectangle<float>(plate.getX() + 1.0f, plate.getY() + 3.0f, plate.getWidth() - 2.0f, 1.0f));
    if (m_kind == Kind::Filter)
    {
        paintFrequencyGrid(g, graphArea(), plotOf(graphArea()), c_filterTopDb, c_filterBottomDb, { 12.0f, 0.0f, -24.0f });
        return;
    }
    // The trace's well: seconds, from the newest at the right.
    {
        const auto well = tracePane();
        const auto plot = plotOf(well);
        const auto inside = well.reduced(1.0f);
        drawGraphWell(g, well, plot);
        g.setColour(TC::grid.withAlpha(0.6f));
        for (int s = 1; s < static_cast<int>(busview::Trace::c_seconds); ++s)
            g.fillRect(juce::Rectangle<float>(std::round(plot.getRight() - plot.getWidth() * static_cast<float>(s) / static_cast<float>(busview::Trace::c_seconds)),
                                              inside.getY(), 1.0f, inside.getHeight()));
        g.setColour(TC::grid);
        g.fillRect(juce::Rectangle<float>(std::round(plot.getX()), inside.getY(), 1.0f, inside.getHeight()));
        g.fillRect(juce::Rectangle<float>(std::round(plot.getRight()), inside.getY(), 1.0f, inside.getHeight()));
    }
    if (m_swirl.type != busview::SwirlDoubler)
        paintFrequencyGrid(g, responsePane(), plotOf(responsePane()), c_swirlTopDb, c_swirlBottomDb, { 0.0f, -12.0f, -24.0f });
}

void BusViewPlate::paintFrequencyGrid(juce::Graphics& g, juce::Rectangle<float> well, juce::Rectangle<float> plot, float topDb, float bottomDb,
                                      std::initializer_list<float> marked) const
{
    drawGraphWell(g, well, plot);
    const auto inside = well.reduced(1.0f);
    g.setColour(TC::grid.withAlpha(0.45f));
    for (const float hz : { 50.0f, 200.0f, 500.0f, 2000.0f, 5000.0f })
        g.fillRect(juce::Rectangle<float>(std::round(xForHz(hz, plot)), inside.getY(), 1.0f, inside.getHeight()));
    g.setColour(TC::grid);
    for (const float hz : { 20.0f, 100.0f, 1000.0f, 10000.0f, 20000.0f })
        g.fillRect(juce::Rectangle<float>(std::round(xForHz(hz, plot)), inside.getY(), 1.0f, inside.getHeight()));
    for (const float db : marked)
    {
        g.setColour(db == 0.0f ? TC::stroke : TC::grid.withAlpha(0.8f));
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
    const auto plot = plotOf(well);
    const auto inside = well.reduced(1.0f);
    const float top = c_filterTopDb, foot = c_filterBottomDb;
    const auto live = m_lit ? m_accent : TC::textFaint;

    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(inside.getSmallestIntegerContainer());
        // The bus's sound behind: a faint shape with a line along its top.
        if (! m_spectrumQuiet)
        {
            g.setColour(c_behind.withAlpha(0.12f));
            g.fillPath(m_spectrumShape);
            g.setColour(c_behind.withAlpha(0.24f));
            g.strokePath(m_spectrumLine, lineStroke(1.0f));
        }
        // The travel: where the cutoff can go with the front wheel where it is. A faint band between two lines, and a
        // rail along the well's head that the marker rides.
        if (m_lit && m_highX - m_lowX >= 1.0f)
        {
            const float x0 = std::round(m_lowX), x1 = std::round(m_highX) + 1.0f;
            g.setColour(m_accent.withAlpha(0.055f));
            g.fillRect(juce::Rectangle<float>(x0, inside.getY(), x1 - x0, inside.getHeight()));
            g.setColour(m_accent.withAlpha(0.3f));
            g.fillRect(juce::Rectangle<float>(x0, inside.getY(), 1.0f, inside.getHeight()));
            g.fillRect(juce::Rectangle<float>(x1 - 1.0f, inside.getY(), 1.0f, inside.getHeight()));
            g.setColour(m_accent.withAlpha(0.6f));
            g.fillRect(juce::Rectangle<float>(x0, inside.getY() + 1.0f, x1 - x0, c_railHeight));
        }
        // The filter: tinted under it, deepest at the head of the well and gone by its foot; then the line; then what is heard.
        if (m_lit)
            g.setGradientFill(juce::ColourGradient::vertical(m_accent.withAlpha(0.3f), inside.getY(), m_accent.withAlpha(0.02f), inside.getBottom()));
        else
            g.setColour(TC::textFaint.withAlpha(0.05f));
        g.fillPath(m_curveFill);
        g.setColour(m_lit ? m_accent : TC::textFaint.withAlpha(0.7f));
        g.strokePath(m_curve, lineStroke(m_lit ? 2.0f : 1.2f));
        if (showsHeard())
        {
            g.setColour(TC::text.withAlpha(0.85f));
            g.strokePath(m_heard, lineStroke(1.0f));
        }
        // The marker at its place: a line down the well, a tab on the rail, and the square on the curve.
        g.setColour(live.withAlpha(0.5f));
        g.fillRect(juce::Rectangle<float>(m_markerX, inside.getY(), 1.0f, inside.getHeight()));
        if (m_lit)
        {
            g.setColour(TC::text);
            g.fillRect(juce::Rectangle<float>(m_markerX - 2.0f, inside.getY() + 1.0f, 5.0f, c_railHeight + 2.0f));
        }
        stampedMarker(g, { m_markerX, m_markerY }, 9.0f, live);
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
        // The key to the two lines, in the corner opposite the scale, under the rail.
        const auto key = juce::Rectangle<float>(plot.getRight() - 104.0f, inside.getY() + c_railHeight + 5.0f, 104.0f, labelHeight);
        g.setColour(TC::well);
        g.fillRect(key.expanded(2.0f, 0.0f).getSmallestIntegerContainer());
        auto left = key.withWidth(52.0f), right = key.withTrimmedLeft(52.0f);
        g.setColour(live);
        g.fillRect(juce::Rectangle<float>(left.getX(), std::round(left.getCentreY()) - 1.0f, 8.0f, 2.0f));
        Theme::fitText(g, "Filter", left.withTrimmedLeft(11.0f), font, juce::Justification::centredLeft);
        if (showsHeard())
        {
            g.setColour(TC::text.withAlpha(0.85f));
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
    const auto plot = plotOf(well);
    const auto inside = well.reduced(1.0f);
    const auto scale = traceScale(m_swirl.type);
    const auto yFor = [&](float v) { return plot.getY() + (scale.top - v) / (scale.top - scale.foot) * plot.getHeight(); };
    const auto live = m_lit ? m_accent : TC::textFaint;

    // Where the copy is level with the dry (Tape Flanger).
    const bool tape = m_swirl.type == busview::SwirlTape;
    if (tape)
    {
        g.setColour(live.withAlpha(0.5f));
        g.fillRect(juce::Rectangle<float>(plot.getX(), std::round(yFor(0.0f)), plot.getWidth(), 1.0f));
    }
    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(inside.getSmallestIntegerContainer());
        // How near the bus's passes are, faint from the foot, with a line along the top.
        if (m_nearAny)
        {
            g.setColour(c_behind.withAlpha(0.1f));
            g.fillPath(m_nearShape);
            g.setColour(c_behind.withAlpha(0.28f));
            g.strokePath(m_nearLine, lineStroke(1.0f));
        }
        // The trace: fainter the older it is.
        if (! m_traceRange.isEmpty())
        {
            g.setColour(live.withAlpha(0.3f));
            g.fillRectList(m_traceRange);
        }
        if (m_lit)
            g.setGradientFill(juce::ColourGradient::horizontal(m_accent.withAlpha(0.35f), plot.getX(), m_accent, plot.getX() + 0.7f * plot.getWidth()));
        else
            g.setColour(TC::textFaint.withAlpha(0.7f));
        g.strokePath(m_traceLine, toothStroke(m_lit ? 1.8f : 1.2f));
    }
    // The labels, last, each on its own patch of the well ("Through Zero" at the old end of its line, where the
    // newest of the trace is not).
    const auto font = m_fonts.caption();
    const float labelHeight = 12.0f;
    graphLabel(g, scale.topLabel, juce::Rectangle<float>(plot.getX() + 2.0f, inside.getY() + 2.0f, 76.0f, labelHeight), font, juce::Justification::centredLeft,
               TC::textFaint);
    graphLabel(g, scale.footLabel, juce::Rectangle<float>(plot.getX() + 2.0f, inside.getBottom() - labelHeight - 1.0f, 76.0f, labelHeight), font,
               juce::Justification::centredLeft, TC::textFaint);
    graphLabel(g, "Passes", juce::Rectangle<float>(plot.getRight() - 4.0f - 46.0f, inside.getBottom() - labelHeight - 1.0f, 46.0f, labelHeight), font,
               juce::Justification::centredRight, TC::textFaint);
    if (tape)
        graphLabel(g, "Through Zero", juce::Rectangle<float>(plot.getX() + 2.0f, std::round(yFor(0.0f)) - labelHeight - 1.0f, 76.0f, labelHeight), font,
                   juce::Justification::centredLeft, live);
    graphLabel(g, scale.title, juce::Rectangle<float>(plot.getRight() - 4.0f - 96.0f, inside.getY() + 2.0f, 96.0f, labelHeight), font, juce::Justification::centredRight,
               TC::textDim);
    // Where it is now: over the labels, since it is the live thing.
    if (m_nowHas)
        stampedMarker(g, m_now, 7.0f, live);
}

void BusViewPlate::paintSwirl(juce::Graphics& g)
{
    paintTrace(g, tracePane());
    if (m_swirl.type != busview::SwirlDoubler)
    {
        const auto well = responsePane();
        const auto plot = plotOf(well);
        const auto inside = well.reduced(1.0f);
        const float top = c_swirlTopDb, foot = c_swirlBottomDb;
        const auto live = m_lit ? m_accent : TC::textFaint;
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(inside.getSmallestIntegerContainer());
            // Tinted to the 0 dB line (as the EQ's curve is); the band a comb's close teeth cover, between the line
            // along their peaks and the line along their notches; then the curve.
            g.setColour(live.withAlpha(m_lit ? 0.13f : 0.05f));
            g.fillPath(m_curveFill);
            if (! m_combBand.isEmpty())
            {
                g.setColour(live.withAlpha(m_lit ? 0.24f : 0.1f));
                g.fillPath(m_combBand);
                g.setColour(live.withAlpha(m_lit ? 0.7f : 0.3f));
                g.strokePath(m_combHead, toothStroke(1.0f));
                g.strokePath(m_combFoot, toothStroke(1.0f));
            }
            g.setColour(m_lit ? m_accent : TC::textFaint.withAlpha(0.7f));
            g.strokePath(m_curve, toothStroke(m_lit ? 1.8f : 1.2f));
        }
        const auto font = m_fonts.caption();
        const float labelHeight = 12.0f;
        paintFrequencyLabels(g, well, plot);
        graphLabel(g, "0 dB", juce::Rectangle<float>(plot.getX() + 2.0f, std::round(yForDb(0.0f, plot, top, foot)) - labelHeight - 1.0f, 40.0f, labelHeight), font,
                   juce::Justification::centredLeft, TC::textFaint);
        graphLabel(g, "-24 dB", juce::Rectangle<float>(plot.getX() + 2.0f, std::round(yForDb(-24.0f, plot, top, foot)) - labelHeight - 1.0f, 40.0f, labelHeight), font,
                   juce::Justification::centredLeft, TC::textFaint);
        graphLabel(g, "Response", juce::Rectangle<float>(plot.getRight() - 4.0f - 70.0f, inside.getY() + 2.0f, 70.0f, labelHeight), font, juce::Justification::centredRight,
                   TC::textDim);
    }

    auto row = rowArea().toFloat();
    g.setColour(m_lit ? m_accent : TC::textDim);
    Theme::fitText(g, ParamIDs::swirlTypeNames[juce::jlimit(0, ParamIDs::numSwirlTypes - 1, m_swirl.type)], row.removeFromLeft(116.0f), m_fonts.title(),
                   juce::Justification::centredLeft);
    g.setColour(m_lit ? TC::text : TC::textDim);
    Theme::fitText(g, m_row, row, m_fonts.body(), juce::Justification::centredLeft, Theme::Fit::Sentence);
}

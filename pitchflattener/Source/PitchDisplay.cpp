#include "PitchDisplay.h"

#include "Hud.h"
#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
    namespace core = pitchflattener::core;

    // Exactly the same number (an edit compared with its own copy, never with arithmetic).
    bool sameBits (double a, double b) noexcept { return std::memcmp (&a, &b, sizeof (double)) == 0; }

    juce::String noteLabel (double midi)
    {
        static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int n = (int) std::lround (midi);
        return juce::String (names[((n % 12) + 12) % 12]) + juce::String (n / 12 - 1);
    }

    juce::String tagName (int index) { return core::trackerName ((core::Tracker) index); }

    // What each reading suits, in the words of the SOURCE control.
    const char* const c_readingTips[] = {
        "The voice reading: for a sound with one clear pitch: voices, animal calls, single tones and whines.",
        "The engine reading: for engines, props and rich drones, where many partials rise and fall together.",
        "The tone reading: for sirens, whistles and feedback, or a whine inside noise: it follows the strongest partial at any height."
    };

    constexpr float c_noteAxisWidth = 42.0f;
    constexpr float c_tagHeight = 16.0f;
    constexpr float c_lineTolerance = 9.0f;    // how near the line in use a click grabs it
    constexpr float c_pointTolerance = 7.0f;
    constexpr int c_denseDiamonds = 24;        // more points than this on view: drawn small

    void insertSorted (PitchDisplay::CurvePoints& points, core::CurvePoint p, int& index)
    {
        const auto at = std::upper_bound (points.begin(), points.end(), p.seconds,
                                          [] (double s, const core::CurvePoint& q) { return s < q.seconds; });
        index = (int) (at - points.begin());
        points.insert (at, p);
    }

    // Keeps n of the points, evenly spread through them in time, the first and the last included.
    void thinTo (PitchDisplay::CurvePoints& points, std::size_t n)
    {
        if (points.size() <= n)
            return;
        std::sort (points.begin(), points.end(), [] (const core::CurvePoint& a, const core::CurvePoint& b) { return a.seconds < b.seconds; });
        PitchDisplay::CurvePoints kept;
        kept.reserve (n);
        for (std::size_t k = 0; k < n; ++k)
            kept.push_back (points[n > 1 ? k * (points.size() - 1) / (n - 1) : 0]);
        points = std::move (kept);
    }
}

bool PitchDisplay::View::operator== (const View& o) const noexcept
{
    if (result != o.result || state != o.state || std::abs (captureSeconds - o.captureSeconds) >= 0.05 || playPosition != o.playPosition
        || playGeneration != o.playGeneration || rendering != o.rendering || autoSource != o.autoSource || lineInUse != o.lineInUse
        || edits.size() != o.edits.size())
        return false;
    for (std::size_t i = 0; i < edits.size(); ++i)
        if (! sameBits (edits[i].seconds, o.edits[i].seconds) || ! sameBits (edits[i].semitones, o.edits[i].semitones))
            return false;
    return true;
}

PitchDisplay::PitchDisplay()
{
    setOpaque (false);
    setWantsKeyboardFocus (true);   // for undo and redo by key once the graph has been clicked
}

//==============================================================================
void PitchDisplay::setView (const View& view)
{
    if (view == m_view)
        return;

    const bool newResult = view.result != m_view.result;
    // While it plays, only the play head moves: repaint the strip it crossed (with the lime ticks
    // it adds behind it), not the whole graph.
    const bool onlyHeadMoved = ! newResult && view.state == m_view.state && view.rendering == m_view.rendering
                            && view.autoSource == m_view.autoSource && view.lineInUse == m_view.lineInUse
                            && std::abs (view.captureSeconds - m_view.captureSeconds) < 0.05 && view.edits.size() == m_view.edits.size()
                            && std::equal (view.edits.begin(), view.edits.end(), m_view.edits.begin(),
                                           [] (const core::CurvePoint& a, const core::CurvePoint& b)
                                           { return sameBits (a.seconds, b.seconds) && sameBits (a.semitones, b.semitones); });
    const auto headX = [this] (const View& v) -> float
    {
        const auto* r = v.result.get();
        if (r == nullptr || ! r->playable() || v.playPosition < 0 || v.playGeneration != r->generation)
            return -1.0f;
        return xFor (r->flattened.sourceSampleFor ((double) v.playPosition) / r->learned->sampleRate);
    };
    const float before = headX (m_view);
    m_view = view;
    if (newResult)
        rebuild();

    const float after = headX (m_view);
    if (onlyHeadMoved && before >= 0.0f && after >= 0.0f)
    {
        const int x0 = (int) std::floor (std::min (before, after)) - 10, x1 = (int) std::ceil (std::max (before, after)) + 10;
        repaint (x0, 0, x1 - x0, getHeight());
        return;
    }
    repaint();
}

void PitchDisplay::setDrawMode (bool draw)
{
    m_draw = draw;
    setMouseCursor (draw ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void PitchDisplay::setSnap (bool semitones)
{
    m_snap = semitones;
}

void PitchDisplay::resized()
{
    layout();
    rebuild();
}

void PitchDisplay::layout()
{
    auto area = getLocalBounds().toFloat().reduced (10.0f, 6.0f);
    m_overview = area.removeFromBottom (10.0f).withTrimmedLeft (c_noteAxisWidth);
    area.removeFromBottom (4.0f);
    const auto labels = area.removeFromBottom (14.0f).withTrimmedLeft (c_noteAxisWidth);
    m_strip = area.removeFromBottom (24.0f).withTrimmedLeft (c_noteAxisWidth);
    m_timeStrip = m_strip.getUnion (labels);
    area.removeFromBottom (4.0f);
    m_plot = area.withTrimmedLeft (c_noteAxisWidth).withTrimmedTop (4.0f);
}

void PitchDisplay::autoRange (double& low, double& high) const
{
    low = 1.0e9;
    high = -1.0e9;
    const auto* r = m_view.result.get();
    if (r == nullptr || r->learned == nullptr)
    {
        low = 48.0;
        high = 72.0;
        return;
    }
    for (const auto& l : r->learned->analysis.lines)
        if (l.usable)
        {
            low = std::min (low, l.lowMidi);
            high = std::max (high, l.highMidi);
        }
    for (const auto& span : targetSpans())
    {
        low = std::min (low, span.midi);
        high = std::max (high, span.midi);
    }
    if (low > high)
        for (const auto& l : r->learned->analysis.lines)
            if (! l.midi.empty())
            {
                low = std::min (low, l.centreMidi - 6.0);
                high = std::max (high, l.centreMidi + 6.0);
            }
    if (low > high)
    {
        low = 48.0;
        high = 72.0;
    }
    low -= 3.0;
    high += 3.0;
    if (high - low < 12.0)
    {
        const double mid = 0.5 * (low + high);
        low = mid - 6.0;
        high = mid + 6.0;
    }
}

void PitchDisplay::rebuild()
{
    m_peaks.clear();
    const auto* r = m_view.result.get();
    if (r == nullptr || r->learned == nullptr)
    {
        m_learnedId = 0;
        m_seconds = 1.0;
        m_from = m_to = 0.0;
        autoRange (m_autoLow, m_autoHigh);
        m_low = m_autoLow;
        m_high = m_autoHigh;
        return;
    }

    const auto& learned = *r->learned;
    m_seconds = std::max (0.01, (double) learned.locator.length() / learned.sampleRate);
    const bool pitchWasAuto = std::abs (m_low - m_autoLow) < 1.0e-9 && std::abs (m_high - m_autoHigh) < 1.0e-9;
    autoRange (m_autoLow, m_autoHigh);

    // Another sound: the whole of it. Another render of the same sound keeps the view.
    if (learned.id != m_learnedId)
    {
        m_learnedId = learned.id;
        m_from = m_to = 0.0;
        m_low = m_autoLow;
        m_high = m_autoHigh;
        m_gesture = Gesture::None;
    }
    else if (pitchWasAuto)
    {
        m_low = m_autoLow;
        m_high = m_autoHigh;
    }
    clampView();

    // The loudest sample in each short block, so the level strip can be drawn at any zoom.
    const auto length = learned.locator.length();
    const float* x = learned.locator.samples();
    m_peaks.assign ((std::size_t) ((length + kPeakBlock - 1) / kPeakBlock), 0.0f);
    float loudest = 1.0e-6f;
    for (std::size_t b = 0; b < m_peaks.size(); ++b)
    {
        float peak = 0.0f;
        const auto end = std::min<std::int64_t> (length, (std::int64_t) ((b + 1) * kPeakBlock));
        for (auto i = (std::int64_t) (b * kPeakBlock); i < end; ++i)
            peak = std::max (peak, std::abs (x[i]));
        m_peaks[b] = peak;
        loudest = std::max (loudest, peak);
    }
    for (auto& p : m_peaks)
        p /= loudest;
}

//==============================================================================
// Zoom

double PitchDisplay::viewFrom() const noexcept { return m_to > m_from ? m_from : 0.0; }
double PitchDisplay::viewTo() const noexcept { return m_to > m_from ? m_to : m_seconds; }

bool PitchDisplay::isZoomed() const noexcept
{
    return m_to > m_from || std::abs (m_low - m_autoLow) > 1.0e-9 || std::abs (m_high - m_autoHigh) > 1.0e-9;
}

void PitchDisplay::clampView()
{
    if (m_to > m_from)
    {
        const double span = std::clamp (m_to - m_from, std::min (kMinSpanSeconds, m_seconds), m_seconds);
        if (span >= m_seconds - 1.0e-9)
            m_from = m_to = 0.0;
        else
        {
            m_from = std::clamp (m_from, 0.0, m_seconds - span);
            m_to = m_from + span;
        }
    }
    const double autoSpan = m_autoHigh - m_autoLow;
    const double span = std::clamp (m_high - m_low, std::min (kMinSemitones, autoSpan), autoSpan);
    m_low = std::clamp (m_low, m_autoLow, m_autoHigh - span);
    m_high = m_low + span;
}

void PitchDisplay::changed()
{
    repaint();
    if (onViewChanged != nullptr)
        onViewChanged();
}

void PitchDisplay::zoomTime (double anchorSeconds, double factor)
{
    if (m_view.result == nullptr || factor <= 0.0)
        return;
    const double from = viewFrom(), span = viewTo() - from;
    const double newSpan = std::clamp (span * factor, std::min (kMinSpanSeconds, m_seconds), m_seconds);
    anchorSeconds = std::clamp (anchorSeconds, from, from + span);
    m_from = anchorSeconds - (anchorSeconds - from) * newSpan / span;
    m_to = m_from + newSpan;
    clampView();
    changed();
}

void PitchDisplay::zoomPitch (double anchorMidi, double factor)
{
    if (m_view.result == nullptr || factor <= 0.0)
        return;
    const double span = m_high - m_low;
    const double newSpan = std::clamp (span * factor, std::min (kMinSemitones, m_autoHigh - m_autoLow), m_autoHigh - m_autoLow);
    anchorMidi = std::clamp (anchorMidi, m_low, m_high);
    m_low = anchorMidi - (anchorMidi - m_low) * newSpan / span;
    m_high = m_low + newSpan;
    clampView();
    changed();
}

void PitchDisplay::panTime (double deltaSeconds)
{
    if (m_to <= m_from)
        return;
    const double span = m_to - m_from;
    m_from += deltaSeconds;
    m_to = m_from + span;
    clampView();
    changed();
}

void PitchDisplay::fit()
{
    m_from = m_to = 0.0;
    m_low = m_autoLow;
    m_high = m_autoHigh;
    changed();
}

float PitchDisplay::xFor (double seconds) const noexcept
{
    return m_plot.getX() + (float) ((seconds - viewFrom()) / std::max (1.0e-9, viewTo() - viewFrom())) * m_plot.getWidth();
}

double PitchDisplay::secondsFor (float x) const noexcept
{
    return viewFrom() + std::clamp ((double) ((x - m_plot.getX()) / m_plot.getWidth()), 0.0, 1.0) * (viewTo() - viewFrom());
}

float PitchDisplay::yFor (double midi) const noexcept
{
    return m_plot.getBottom() - (float) ((midi - m_low) / std::max (1.0e-9, m_high - m_low)) * m_plot.getHeight();
}

double PitchDisplay::midiFor (float y) const noexcept
{
    return m_low + (double) ((m_plot.getBottom() - y) / m_plot.getHeight()) * (m_high - m_low);
}

//==============================================================================
// The readings

const core::PitchLine* PitchDisplay::line (int index) const
{
    const auto* r = m_view.result.get();
    if (r == nullptr || r->learned == nullptr || index < 0 || index >= core::kNumTrackers)
        return nullptr;
    const auto& l = r->learned->analysis.lines[(size_t) index];
    return l.midi.empty() ? nullptr : &l;
}

double PitchDisplay::lineAt (int index, double seconds) const
{
    const auto* l = line (index);
    if (l == nullptr)
        return std::numeric_limits<double>::quiet_NaN();
    // Frame f is the sound around (f + 0.5) * hop.
    const double fp = std::clamp (seconds / l->hopSeconds - 0.5, 0.0, (double) l->midi.size() - 1.0);
    const auto f0 = (std::size_t) fp;
    const auto f1 = std::min (f0 + 1, l->midi.size() - 1);
    return l->midi[f0] + (l->midi[f1] - l->midi[f0]) * (fp - (double) f0);
}

double PitchDisplay::baseAt (double seconds) const { return lineAt (m_view.lineInUse, seconds); }

std::vector<PitchDisplay::TargetSpan> PitchDisplay::targetSpans() const
{
    const auto* r = m_view.result.get();
    if (r == nullptr || r->learned == nullptr || ! r->playable())
        return {};
    const auto& events = r->learned->analysis.events;
    const auto& targets = r->flattened.eventTargets;
    if (events.size() < 2 || targets.size() != events.size())
        return { { 0.0, m_seconds, r->flattened.targetMidi } };
    std::vector<TargetSpan> spans;
    for (std::size_t i = 0; i < events.size(); ++i)
        spans.push_back ({ events[i].startSeconds, events[i].endSeconds, targets[i] });
    return spans;
}

double PitchDisplay::correctedAt (double seconds) const
{
    return baseAt (seconds) + core::editAt (points(), seconds);
}

std::pair<std::size_t, std::size_t> PitchDisplay::framesInView (const core::PitchLine& l) const
{
    const auto last = (double) l.midi.size() - 1.0;
    const auto a = (std::size_t) std::clamp (std::floor (viewFrom() / l.hopSeconds) - 1.0, 0.0, last);
    const auto b = (std::size_t) std::clamp (std::ceil (viewTo() / l.hopSeconds) + 1.0, 0.0, last);
    return { a, std::max (a, b) };
}

std::array<juce::Rectangle<float>, core::kNumTrackers> PitchDisplay::tagAreas() const
{
    // Each tag sits just above its own line near the left of the view; where lines run close the
    // tags stack downwards in order, so three tags never cover one another.
    std::array<juce::Rectangle<float>, core::kNumTrackers> boxes {};
    const auto font = m_fonts->label (10.0f);
    const float x = m_plot.getX() + 6.0f;
    std::vector<int> order;
    for (int i = 0; i < core::kNumTrackers; ++i)
        if (line (i) != nullptr)
        {
            // Zoomed in on pitch, a reading whose line is off the graph here has no tag.
            const double value = lineAt (i, secondsFor (x + 30.0f)) + (i == m_view.lineInUse ? core::editAt (points(), secondsFor (x + 30.0f)) : 0.0);
            if (! std::isfinite (value) || value < m_low - 0.5 || value > m_high + 0.5)
                continue;
            const float w = Hud::textWidth (font, tagName (i)) + 14.0f;
            boxes[(size_t) i] = { x, yFor (value) - c_tagHeight - 4.0f, w, c_tagHeight };
            order.push_back (i);
        }
    std::sort (order.begin(), order.end(), [&] (int a, int b) { return boxes[(size_t) a].getY() < boxes[(size_t) b].getY(); });
    for (std::size_t k = 0; k < order.size(); ++k)
    {
        auto& box = boxes[(size_t) order[k]];
        box.setY (std::max (box.getY(), m_plot.getY() + 2.0f));
        if (k > 0)
            box.setY (std::max (box.getY(), boxes[(size_t) order[k - 1]].getBottom() + 3.0f));
    }
    if (! order.empty())
    {
        // Past the bottom: stack upwards from it instead (never above the top).
        float bottom = m_plot.getBottom() - 2.0f;
        for (auto k = order.size(); k-- > 0;)
        {
            auto& box = boxes[(size_t) order[k]];
            if (box.getBottom() <= bottom)
                break;
            box.setY (std::max (m_plot.getY() + 2.0f, bottom - c_tagHeight));
            bottom = box.getY() - 3.0f;
        }
    }
    return boxes;
}

int PitchDisplay::tagAt (juce::Point<float> p) const
{
    const auto boxes = tagAreas();
    for (int i = 0; i < core::kNumTrackers; ++i)
        if (! boxes[(size_t) i].isEmpty() && boxes[(size_t) i].expanded (2.0f).contains (p))
            return i;
    return -1;
}

int PitchDisplay::pointAt (juce::Point<float> p) const
{
    if (m_view.lineInUse < 0)
        return -1;
    const auto& pts = points();
    for (int i = (int) pts.size() - 1; i >= 0; --i)
    {
        const auto& q = pts[(size_t) i];
        if (q.seconds < viewFrom() - 1.0e-9 || q.seconds > viewTo() + 1.0e-9)
            continue;
        const float y = std::clamp (yFor (correctedAt (q.seconds)), m_plot.getY(), m_plot.getBottom());
        if (juce::Point<float> (xFor (q.seconds), y).getDistanceFrom (p) <= c_pointTolerance)
            return i;
    }
    return -1;
}

int PitchDisplay::lineNear (juce::Point<float> p, float tolerance) const
{
    if (! m_plot.expanded (0.0f, 4.0f).contains (p))
        return -1;
    const double t = secondsFor (p.x);
    // The line in use first: reaching for it never picks another reading.
    if (m_view.lineInUse >= 0)
        if (const double y = yFor (correctedAt (t)); std::isfinite (y) && std::abs ((float) y - p.y) <= tolerance)
            return m_view.lineInUse;
    int best = -1;
    float bestDistance = tolerance * 0.8f;
    for (int i = 0; i < core::kNumTrackers; ++i)
    {
        if (i == m_view.lineInUse || line (i) == nullptr)
            continue;
        const float d = std::abs (yFor (lineAt (i, t)) - p.y);
        if (d < bestDistance)
        {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

double PitchDisplay::snapped (double seconds, double value) const
{
    if (! m_snap)
        return value;
    const double base = baseAt (seconds);
    return std::round (base + value) - base;
}

//==============================================================================
// Mouse

void PitchDisplay::mouseMove (const juce::MouseEvent& e)
{
    m_hoverAt = e.position;
    m_hoverTag = tagAt (e.position);
    m_hoverPoint = m_draw || m_hoverTag >= 0 ? -1 : pointAt (e.position);
    m_hoverLine = m_hoverTag >= 0 ? -1 : lineNear (e.position, c_lineTolerance);
    const bool editable = m_view.lineInUse >= 0 && m_view.result != nullptr && m_view.result->playable() && m_hoverTag < 0
                       && (m_draw ? m_plot.contains (e.position) : (m_hoverPoint >= 0 || m_hoverLine == m_view.lineInUse));
    const bool picks = m_hoverTag >= 0 || (m_hoverLine >= 0 && m_hoverLine != m_view.lineInUse);
    setMouseCursor (m_draw && m_plot.contains (e.position) ? juce::MouseCursor::CrosshairCursor
                    : picks || editable ? juce::MouseCursor::PointingHandCursor
                    : m_timeStrip.contains (e.position) || m_overview.contains (e.position) ? juce::MouseCursor::DraggingHandCursor
                    : juce::MouseCursor::NormalCursor);
    const bool hoverChanged = editable != m_hoverEditable;
    m_hoverEditable = editable;
    repaint();
    if (hoverChanged && onViewChanged != nullptr)
        onViewChanged();
}

void PitchDisplay::mouseExit (const juce::MouseEvent&)
{
    m_hoverAt = { -1.0f, -1.0f };
    m_hoverLine = m_hoverPoint = m_hoverTag = -1;
    const bool hoverChanged = m_hoverEditable;
    m_hoverEditable = false;
    repaint();
    if (hoverChanged && onViewChanged != nullptr)
        onViewChanged();
}

void PitchDisplay::mouseDown (const juce::MouseEvent& e)
{
    const auto* r = m_view.result.get();
    if (r == nullptr || r->learned == nullptr)
        return;
    m_downAt = e.position;
    m_moved = false;

    // The overview and the time strip move the view.
    if (m_overview.contains (e.position))
    {
        m_gesture = Gesture::Overview;
        mouseDrag (e);
        return;
    }
    if (m_timeStrip.contains (e.position))
    {
        m_gesture = Gesture::Pan;
        m_panFrom = viewFrom();
        return;
    }

    // A tag chooses its reading.
    if (const int tag = tagAt (e.position); tag >= 0)
    {
        if (tag != m_view.lineInUse && line (tag) != nullptr && line (tag)->usable && onLineClicked != nullptr)
            onLineClicked (tag);
        return;
    }

    const bool editable = m_view.lineInUse >= 0 && r->playable();
    if (editable)
        grabKeyboardFocus();

    if (editable && m_draw && m_plot.contains (e.position))
    {
        m_before = m_view.edits;
        m_points = m_before;
        m_stroke.clear();
        m_gesture = Gesture::Draw;
        m_strokeFrom = secondsFor (e.position.x);
        addStrokeSample (e.position);
        return;
    }

    int index = editable && ! m_draw ? pointAt (e.position) : -1;
    const int near = index >= 0 ? m_view.lineInUse : lineNear (e.position, c_lineTolerance);
    if (near >= 0 && near != m_view.lineInUse)
    {
        if (line (near)->usable && onLineClicked != nullptr)
            onLineClicked (near);
        return;
    }
    if (! editable || m_draw || near < 0)
        return;

    // Pull: grab a point, or put one on the line where it was grabbed (at the correction already
    // there, so the line does not jump).
    m_before = m_view.edits;
    m_points = m_before;
    m_created = false;
    if (index < 0)
    {
        if (m_points.size() >= PitchFlattenerAudioProcessor::kMaxEdits)
            return;
        const double t = secondsFor (e.position.x);
        insertSorted (m_points, { t, core::editAt (m_before, t) }, index);
        m_created = true;
    }
    m_gesture = Gesture::Pull;
    m_drag = index;
    m_dragStart = m_points[(size_t) index];
    m_readoutSeconds = m_dragStart.seconds;
    repaint();
}

void PitchDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (e.position.getDistanceFrom (m_downAt) > 2.5f)
        m_moved = true;

    switch (m_gesture)
    {
        case Gesture::Overview:
        {
            if (m_to <= m_from)
                return;
            const double span = m_to - m_from;
            const double centre = (double) ((e.position.x - m_overview.getX()) / m_overview.getWidth()) * m_seconds;
            m_from = centre - span * 0.5;
            m_to = m_from + span;
            clampView();
            changed();
            return;
        }
        case Gesture::Pan:
            if (m_to > m_from)
            {
                const double span = m_to - m_from;
                m_from = m_panFrom - (double) ((e.position.x - m_downAt.x) / m_plot.getWidth()) * span;
                m_to = m_from + span;
                clampView();
                changed();
            }
            return;
        case Gesture::Draw:
            addStrokeSample (e.position);
            return;
        case Gesture::Pull:
            break;
        case Gesture::None:
            return;
    }

    if (m_drag < 0 || ! m_moved)
        return;
    // Shift for fine work; sideways only once the mouse has clearly moved that way, so a pitch
    // nudge stays put in time. Up says "the pitch here is higher than was read".
    const double fine = e.mods.isShiftDown() ? 0.2 : 1.0;
    auto p = m_points[(size_t) m_drag];
    const float dx = e.position.x - m_downAt.x;
    if (std::abs (dx) > 4.0f)
        p.seconds = std::clamp (m_dragStart.seconds + (double) (dx / m_plot.getWidth()) * (viewTo() - viewFrom()) * fine, 0.0, m_seconds);
    const double perPixel = (m_high - m_low) / (double) m_plot.getHeight();
    double value = m_dragStart.semitones + (double) (m_downAt.y - e.position.y) * perPixel * fine;
    if (! e.mods.isAltDown())
        value = snapped (p.seconds, value);
    p.semitones = std::clamp (value, -24.0, 24.0);
    m_points.erase (m_points.begin() + m_drag);
    insertSorted (m_points, p, m_drag);
    m_readoutSeconds = p.seconds;
    if (onEditsChanging != nullptr)
        onEditsChanging (m_points);
    repaint();
}

void PitchDisplay::mouseUp (const juce::MouseEvent&)
{
    const auto gesture = m_gesture;
    m_gesture = Gesture::None;
    m_readoutSeconds = -1.0;
    m_drag = -1;
    if (gesture == Gesture::Pull)
    {
        // A plain click on the line adds nothing.
        if (m_moved && onEdited != nullptr)
            onEdited (m_before, m_points);
        if (m_moved)
            m_view.edits = m_points;   // until the owner sends them back
    }
    else if (gesture == Gesture::Draw)
    {
        if (onEdited != nullptr)
            onEdited (m_before, m_points);
        m_view.edits = m_points;
    }
    repaint();
}

void PitchDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (m_draw || m_gesture != Gesture::None)
        return;
    const int index = pointAt (e.position);
    if (index < 0)
        return;
    auto after = m_view.edits;
    after.erase (after.begin() + index);
    const auto before = m_view.edits;
    m_view.edits = after;
    m_hoverPoint = -1;
    if (onEdited != nullptr)
        onEdited (before, after);
    repaint();
}

void PitchDisplay::addStrokeSample (juce::Point<float> at)
{
    // Freehand: the mouse is the corrected pitch. The stroke replaces the points under it, and the
    // curve either side of it is pinned where it was.
    const double seconds = secondsFor (at.x);
    double pitch = midiFor (std::clamp (at.y, m_plot.getY(), m_plot.getBottom()));
    // Alt lets go of the snap while drawing, as it does while pulling a point.
    if (m_snap && ! juce::ModifierKeys::currentModifiers.isAltDown())
        pitch = std::round (pitch);
    const double value = std::clamp (pitch - baseAt (seconds), -24.0, 24.0);

    // One sample per 1/240 of the view (a line drawn zoomed in keeps its detail), never closer than
    // the reading's frames. A newer sample replaces the one in its slot, so going back over the
    // line redraws it.
    const auto* l = line (m_view.lineInUse);
    const double span = viewTo() - viewFrom();
    const double spacing = std::max ({ 1.0e-4, l != nullptr ? l->hopSeconds : 0.005, span / 240.0 });
    const auto slot = [spacing] (double t) { return std::llround (t / spacing); };
    m_stroke.erase (std::remove_if (m_stroke.begin(), m_stroke.end(), [&] (const core::CurvePoint& p) { return slot (p.seconds) == slot (seconds); }),
                    m_stroke.end());
    m_stroke.push_back ({ seconds, value });

    double lo = seconds, hi = seconds;
    for (const auto& p : m_stroke)
    {
        lo = std::min (lo, p.seconds);
        hi = std::max (hi, p.seconds);
    }
    const double eps = std::max (2.0e-4, span / 400.0);
    CurvePoints kept, pins, stroke = m_stroke;
    for (const auto& p : m_before)
        if (p.seconds < lo - eps || p.seconds > hi + eps)
            kept.push_back (p);
    if (lo - eps > 0.0)
        pins.push_back ({ lo - eps, core::editAt (m_before, lo - eps) });
    if (hi + eps < m_seconds)
        pins.push_back ({ hi + eps, core::editAt (m_before, hi + eps) });

    // Within the limit on points: the stroke is thinned first but keeps a fair share; past that the
    // older points elsewhere are thinned too. Nothing is simply cut off the end.
    const std::size_t limit = PitchFlattenerAudioProcessor::kMaxEdits - pins.size();
    if (kept.size() + stroke.size() > limit)
    {
        const std::size_t share = std::min (stroke.size(), std::max<std::size_t> (64, limit / 4));
        thinTo (stroke, std::max (share, limit - std::min (limit, kept.size())));
        thinTo (kept, limit - stroke.size());
    }
    CurvePoints all = std::move (kept);
    all.insert (all.end(), pins.begin(), pins.end());
    all.insert (all.end(), stroke.begin(), stroke.end());
    std::sort (all.begin(), all.end(), [] (const core::CurvePoint& a, const core::CurvePoint& b) { return a.seconds < b.seconds; });
    m_points = std::move (all);
    m_readoutSeconds = seconds;
    if (onEditsChanging != nullptr)
        onEditsChanging (m_points);
    repaint();
}

void PitchDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (m_view.result == nullptr || m_gesture != Gesture::None)
        return;
    const float dx = wheel.isReversed ? -wheel.deltaX : wheel.deltaX;
    const float dy = wheel.isReversed ? -wheel.deltaY : wheel.deltaY;
    if (std::abs (dx) > std::abs (dy))
    {
        panTime (-(double) dx * (viewTo() - viewFrom()) * 0.5);
        return;
    }
    // Up zooms in, about the moment (or with Alt the pitch) under the pointer.
    const double factor = std::exp (-(double) dy * 2.5);
    if (e.mods.isAltDown())
        zoomPitch (midiFor (e.position.y), factor);
    else
        zoomTime (secondsFor (e.position.x), factor);
}

void PitchDisplay::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    if (scaleFactor > 0.0f && m_gesture == Gesture::None)
        zoomTime (secondsFor (e.position.x), 1.0 / (double) scaleFactor);
}

bool PitchDisplay::keyPressed (const juce::KeyPress& key)
{
    // Cmd+Z undoes, Shift+Cmd+Z (or Cmd+Y) redoes; everything else goes back to the host.
    const auto mods = key.getModifiers();
    if (! mods.isCommandDown() || onUndoKey == nullptr)
        return false;
    const int code = key.getKeyCode();
    if (code == 'Z' || code == 'z')
    {
        onUndoKey (mods.isShiftDown());
        return true;
    }
    if (code == 'Y' || code == 'y')
    {
        onUndoKey (true);
        return true;
    }
    return false;
}

juce::String PitchDisplay::getTooltip()
{
    const auto* r = m_view.result.get();
    const auto mouse = getMouseXYRelative().toFloat();
    if (r != nullptr && r->learned != nullptr)
    {
        if (m_overview.contains (mouse))
            return "The whole sound, with the part on view marked: drag to move the view.";
        if (m_timeStrip.contains (mouse))
            return "The sound's level over time. Drag here to move a zoomed-in view; scroll to zoom in and out.";
        if (! m_draw && pointAt (mouse) >= 0)
            return "A correction: drag it to move it (Shift for fine moves), double-click it to remove it.";
        int index = tagAt (mouse);
        if (index < 0)
            index = lineNear (mouse, c_lineTolerance);
        if (index >= 0)
        {
            const juce::String what = c_readingTips[index];
            const auto& reading = r->learned->analysis.lines[(size_t) index];
            if (! reading.usable)
                return what + " It found too little pitch in this sound to flatten by.";
            if (index == m_view.lineInUse)
                return what + " The sound is flattened by this line: drag it to correct it where it is wrong.";
            return what + " Click to flatten the sound by this line instead.";
        }
    }
    return "Scope: the learned sound from start to end, its pitch over time on a note scale (the line it is flattened by "
           "in cyan, dashed where the line only bridges a gap with no pitch), the pitch it is flattened to in orange, and "
           "its level along the bottom. While the flattened sound plays, a lime line shows where it is. Scroll to zoom "
           "time, Alt and scroll to zoom pitch.";
}

//==============================================================================
// Drawing

void PitchDisplay::drawReading (juce::Graphics& g, int index, bool inUse) const
{
    const auto* l = line (index);
    if (l == nullptr)
        return;
    const auto& pts = points();
    const bool corrected = inUse && ! pts.empty();

    const auto colour = inUse ? Theme::Colours::cyan
                              : (m_hoverLine == index || m_hoverTag == index) && l->usable ? Theme::Colours::textDim.brighter (0.5f)
                                                                                            : Theme::Colours::textDim.withAlpha (0.75f);
    const float alpha = l->usable ? 1.0f : 0.45f;
    const auto [first, last] = framesInView (*l);
    const std::size_t stride = std::max<std::size_t> (1, (last - first) / (std::size_t) std::max (1.0f, m_plot.getWidth() * 2.0f));

    const auto stroke = [&] (bool withEdits, juce::Colour c, float width)
    {
        // The stretches the tracker read (solid), and those where the line only bridges a gap
        // (dashed: every other 4 px of it).
        juce::Path read, bridged;
        float px = 0.0f, py = 0.0f;
        bool readOpen = false, dashOpen = false, started = false;
        for (std::size_t f = first;; f = std::min (f + stride, last))
        {
            const double t = ((double) f + 0.5) * l->hopSeconds;
            const float x = xFor (t);
            const float y = yFor (l->midi[f] + (withEdits ? core::editAt (pts, t) : 0.0));
            const bool gap = f < l->trackedHz.size() && l->trackedHz[f] <= 0.0f;
            if (started)
            {
                if (! gap)
                {
                    if (! readOpen)
                        read.startNewSubPath (px, py);
                    read.lineTo (x, y);
                    readOpen = true;
                    dashOpen = false;
                }
                else
                {
                    readOpen = false;
                    const bool on = ((int) std::floor (x / 4.0f) % 2) == 0;
                    if (on)
                    {
                        if (! dashOpen)
                            bridged.startNewSubPath (px, py);
                        bridged.lineTo (x, y);
                    }
                    dashOpen = on;
                }
            }
            started = true;
            px = x;
            py = y;
            if (f >= last)
                break;
        }
        g.setColour (c);
        g.strokePath (read, juce::PathStrokeType (width, juce::PathStrokeType::mitered, juce::PathStrokeType::square));
        g.setColour (c.withMultipliedAlpha (0.55f));
        g.strokePath (bridged, juce::PathStrokeType (width, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    };

    if (corrected)
        stroke (false, colour.withAlpha (0.35f * alpha), 1.4f);   // the reading as it was, faint beneath
    stroke (corrected, colour.withMultipliedAlpha (alpha), inUse ? 2.4f : 1.4f);
}

void PitchDisplay::drawGrid (juce::Graphics& g) const
{
    using namespace Theme::Colours;
    const auto font = m_fonts->label (10.0f);

    // Pitch: a line on every C, and on other notes when there is room for their labels; zoomed
    // far in, lines every 25 cents too, and the half-semitones labelled.
    {
        const double pxPerSemitone = m_plot.getHeight() / (m_high - m_low);
        int step = 12;
        for (const int s : { 1, 2, 3, 4, 6, 12 })
            if (pxPerSemitone * s >= 18.0)
            {
                step = s;
                break;
            }
        if (pxPerSemitone >= 72.0)
        {
            g.setColour (grid.withAlpha (0.6f));
            for (double c = std::ceil (m_low * 4.0) / 4.0; c <= m_high; c += 0.25)
                if (std::abs (c - std::round (c)) > 0.01)
                {
                    const float y = yFor (c);
                    g.drawHorizontalLine ((int) std::round (y), m_plot.getX(), m_plot.getRight());
                    if (std::abs (c - std::round (c) - 0.5) < 0.01 || std::abs (c - std::round (c) + 0.5) < 0.01)
                        Hud::drawText (g, "+50", { m_plot.getX() - c_noteAxisWidth, y - 7.0f, c_noteAxisWidth - 6.0f, 14.0f }, font, textDim.withAlpha (0.7f),
                                       juce::Justification::centredRight);
                }
        }
        for (int n = (int) std::ceil (m_low); n <= (int) std::floor (m_high); ++n)
        {
            if (n % step != 0)
                continue;
            const float y = yFor (n);
            g.setColour (n % 12 == 0 ? panelEdge : grid);
            g.drawHorizontalLine ((int) std::round (y), m_plot.getX(), m_plot.getRight());
            Hud::drawText (g, noteLabel (n), { m_plot.getX() - c_noteAxisWidth, y - 7.0f, c_noteAxisWidth - 6.0f, 14.0f }, font,
                           n % 12 == 0 ? text.withAlpha (0.8f) : textDim, juce::Justification::centredRight);
        }
    }

    // Time: seconds, or milliseconds once the ticks are under a tenth of a second apart.
    {
        const double from = viewFrom(), span = viewTo() - from;
        double tick = 60.0;
        for (const double t : { 0.001, 0.002, 0.005, 0.01, 0.02, 0.025, 0.05, 0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 30.0, 60.0 })
            if (m_plot.getWidth() * t / span >= 70.0)
            {
                tick = t;
                break;
            }
        const auto labels = m_timeStrip.withTrimmedTop (m_strip.getHeight());
        for (double t = std::ceil (from / tick - 1.0e-9) * tick; t <= from + span + 1.0e-9; t += tick)
        {
            const float x = xFor (t);
            g.setColour (grid);
            g.drawVerticalLine ((int) std::round (x), m_plot.getY(), m_strip.getBottom());
            const double shown = std::abs (t) < tick * 1.0e-6 ? 0.0 : t;   // never "-0"
            const juce::String label = tick < 0.1 ? juce::String (juce::roundToInt (shown * 1000.0)) + " MS"
                                                  : juce::String (shown, tick < 1.0 ? (tick < 0.25 ? 1 : 2) : 0) + " S";
            const float w = Hud::textWidth (font, label) + 4.0f;
            const auto box = juce::Rectangle<float> (x - w * 0.5f, labels.getY() + 1.0f, w, 13.0f);
            if (box.getRight() > labels.getRight() + 2.0f || box.getX() < labels.getX() - c_noteAxisWidth + 2.0f)
                continue;
            Hud::drawText (g, label, box, font, textDim, juce::Justification::centred);
        }
    }

    // Level strip: what was heard, so in cyan, dimmed to stay behind the lines.
    if (const auto* r = m_view.result.get(); r != nullptr && ! m_peaks.empty())
    {
        g.setColour (cyan.withAlpha (0.32f));
        const double rate = r->learned->sampleRate;
        const float mid = m_strip.getCentreY(), half = m_strip.getHeight() * 0.5f - 1.0f;
        const int columns = (int) m_strip.getWidth();
        const double from = viewFrom(), span = viewTo() - from;
        for (int c = 0; c < columns; ++c)
        {
            const auto b0 = (std::size_t) std::clamp ((from + span * c / columns) * rate / kPeakBlock, 0.0, (double) m_peaks.size() - 1.0);
            const auto b1 = (std::size_t) std::clamp ((from + span * (c + 1) / columns) * rate / kPeakBlock, (double) b0 + 1.0, (double) m_peaks.size());
            float peak = 0.0f;
            for (auto b = b0; b < b1; ++b)
                peak = std::max (peak, m_peaks[b]);
            const float h = std::max (0.5f, peak * half);
            g.fillRect (m_strip.getX() + (float) c, mid - h, 1.0f, 2.0f * h);
        }
    }
}

void PitchDisplay::drawOverview (juce::Graphics& g) const
{
    using namespace Theme::Colours;
    const auto* r = m_view.result.get();
    g.setColour (panel);
    g.fillRect (m_overview);
    if (r != nullptr && ! m_peaks.empty())
    {
        g.setColour (cyan.withAlpha (0.4f));
        const int columns = (int) m_overview.getWidth();
        const float mid = m_overview.getCentreY(), half = m_overview.getHeight() * 0.5f - 1.0f;
        for (int c = 0; c < columns; ++c)
        {
            const auto b0 = (std::size_t) ((double) c / columns * (double) m_peaks.size());
            const auto b1 = std::max (b0 + 1, (std::size_t) ((double) (c + 1) / columns * (double) m_peaks.size()));
            float peak = 0.0f;
            for (auto b = b0; b < b1 && b < m_peaks.size(); ++b)
                peak = std::max (peak, m_peaks[b]);
            const float h = std::max (0.5f, peak * half);
            g.fillRect (m_overview.getX() + (float) c, mid - h, 1.0f, 2.0f * h);
        }
    }
    // The part on view (orange: the user sets it).
    const float x0 = m_overview.getX() + (float) (viewFrom() / m_seconds) * m_overview.getWidth();
    const float x1 = m_overview.getX() + (float) (viewTo() / m_seconds) * m_overview.getWidth();
    const auto window = juce::Rectangle<float> (x0, m_overview.getY(), std::max (4.0f, x1 - x0), m_overview.getHeight());
    g.setColour (orange.withAlpha (isZoomed() ? 0.18f : 0.08f));
    g.fillRect (window);
    g.setColour (orange.withAlpha (isZoomed() ? 1.0f : 0.45f));
    g.drawRect (window, 1.0f);
}

void PitchDisplay::paint (juce::Graphics& g)
{
    if (Hud::skipOnScreen (*this))
        return;
    using namespace Theme::Colours;
    const auto bounds = getLocalBounds().toFloat();

    // Inside the display's chamfered frame: its bottom-right corner is cut here too.
    {
        juce::Path clip;
        clip.startNewSubPath (bounds.getTopLeft());
        clip.lineTo (bounds.getTopRight());
        clip.lineTo (bounds.getRight(), bounds.getBottom() - 8.0f);
        clip.lineTo (bounds.getRight() - 8.0f, bounds.getBottom());
        clip.lineTo (bounds.getBottomLeft());
        clip.closeSubPath();
        g.reduceClipRegion (clip);
    }
    g.fillAll (space);

    const auto* r = m_view.result.get();
    if (r == nullptr || r->learned == nullptr)
    {
        // Nothing learned: an empty scale (the banner over it says what to do).
        g.setColour (grid);
        for (int i = 1; i < 6; ++i)
            g.drawHorizontalLine ((int) std::round (m_plot.getY() + m_plot.getHeight() * (float) i / 6.0f), m_plot.getX(), m_plot.getRight());
        for (int i = 0; i <= 8; ++i)
            g.drawVerticalLine ((int) std::round (m_plot.getX() + m_plot.getWidth() * (float) i / 8.0f), m_plot.getY(), m_strip.getBottom());
        drawOverview (g);
        return;
    }

    drawGrid (g);

    const bool playing = r->playable() && m_view.playPosition >= 0 && m_view.playGeneration == r->generation;
    float headX = -1.0f;
    if (playing)
        headX = xFor (std::clamp (r->flattened.sourceSampleFor ((double) m_view.playPosition) / r->learned->sampleRate, 0.0, m_seconds));

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (m_plot.expanded (0.0f, 2.0f).toNearestInt());

        // The pitch it is flattened to (orange: the user's choice): one segment for each separate
        // sound in the recording, at that sound's own target, linked faintly across the silences,
        // which are shaded so the sounds read as separate. Behind the play head, the stretch
        // already heard flattened, as a trail of lime ticks along it.
        const auto spans = targetSpans();
        const bool several = spans.size() > 1;
        for (std::size_t i = 0; i + 1 < spans.size(); ++i)
        {
            const float x0 = xFor (spans[i].end), x1 = xFor (spans[i + 1].start);
            g.setColour (grid.withAlpha (0.35f));
            g.fillRect (juce::Rectangle<float> (x0, m_plot.getY(), std::max (0.0f, x1 - x0), m_plot.getHeight()));
            g.setColour (orange.withAlpha (0.3f));
            g.drawLine (x0, yFor (spans[i].midi), x1, yFor (spans[i + 1].midi), 1.0f);
        }
        const float tickPhase = std::fmod (xFor (0.0) - m_plot.getX() + 700.0f, 7.0f);
        for (const auto& span : spans)
        {
            const float y = std::round (yFor (span.midi));
            const float x0 = several ? xFor (span.start) : m_plot.getX(), x1 = several ? xFor (span.end) : m_plot.getRight();
            g.setColour (orange);
            g.fillRect (juce::Rectangle<float> (x0, y - 0.75f, std::max (1.0f, x1 - x0), 1.5f));
            if (headX > x0)
            {
                g.setColour (lime.withAlpha (0.85f));
                const float from = m_plot.getX() + 2.0f + tickPhase + 7.0f * std::ceil ((x0 - m_plot.getX() - 2.0f - tickPhase) / 7.0f);
                for (float x = std::max (from, m_plot.getX() + 2.0f); x < std::min ({ headX - 3.0f, x1, m_plot.getRight() }); x += 7.0f)
                    g.fillRect (juce::Rectangle<float> (x, y - 4.0f, 2.0f, 8.0f));
            }
        }

        // The readings: the others first, the one in use on top.
        for (int i = 0; i < core::kNumTrackers; ++i)
            if (i != m_view.lineInUse)
                drawReading (g, i, false);
        if (m_view.lineInUse >= 0)
            drawReading (g, m_view.lineInUse, true);

        // The corrections: diamonds on the corrected line (orange: the user set them). Many on view
        // (a drawn line) are small squares, unless under the mouse.
        const auto& pts = points();
        int onView = 0;
        for (const auto& p : pts)
            onView += p.seconds >= viewFrom() && p.seconds <= viewTo() ? 1 : 0;
        const bool dense = onView > c_denseDiamonds;
        for (int i = 0; i < (int) pts.size(); ++i)
        {
            const auto& p = pts[(size_t) i];
            if (p.seconds < viewFrom() - 1.0e-9 || p.seconds > viewTo() + 1.0e-9)
                continue;
            // (Held at the graph's edge when the correction takes the line off it.)
            const juce::Point<float> c { xFor (p.seconds), std::clamp (yFor (correctedAt (p.seconds)), m_plot.getY(), m_plot.getBottom()) };
            const bool hot = m_gesture != Gesture::Draw && (i == m_hoverPoint || i == m_drag);
            if (dense && ! hot)
            {
                g.setColour (orange);
                g.fillRect (juce::Rectangle<float> (4.0f, 4.0f).withCentre (c));
                continue;
            }
            const float s = hot ? 7.0f : 5.5f;
            juce::Path diamond;
            diamond.addQuadrilateral (c.x, c.y - s, c.x + s, c.y, c.x, c.y + s, c.x - s, c.y);
            g.setColour (hot ? orange : space);
            g.fillPath (diamond);
            g.setColour (orange);
            g.strokePath (diamond, juce::PathStrokeType (2.0f));
        }

        // Where a pull would take hold (a ring on the line), or where a stroke would draw (a cross).
        if (m_gesture == Gesture::None && m_hoverEditable && m_hoverPoint < 0)
        {
            g.setColour (orange.withAlpha (0.8f));
            if (m_draw)
            {
                g.fillRect (juce::Rectangle<float> (m_hoverAt.x - 6.0f, m_hoverAt.y - 1.0f, 12.0f, 2.0f));
                g.fillRect (juce::Rectangle<float> (m_hoverAt.x - 1.0f, m_hoverAt.y - 6.0f, 2.0f, 12.0f));
            }
            else
                g.drawEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ m_hoverAt.x, yFor (correctedAt (secondsFor (m_hoverAt.x))) }), 2.0f);
        }
    }

    // Tags.
    const auto boxes = tagAreas();
    for (int i = 0; i < core::kNumTrackers; ++i)
    {
        const auto box = boxes[(size_t) i];
        if (box.isEmpty())
            continue;
        const auto* l = line (i);
        const bool inUse = m_view.lineInUse == i;
        const auto colour = (inUse ? cyan : (m_hoverTag == i || m_hoverLine == i) && l->usable ? textDim.brighter (0.5f) : textDim)
                                .withMultipliedAlpha (l->usable ? 1.0f : 0.6f);
        Hud::frame (g, box, 4.0f, space.withAlpha (0.88f), colour, 1.0f);
        Hud::drawText (g, tagName (i), box, m_fonts->label (10.0f), colour, juce::Justification::centred);
        if (m_view.autoSource && r->learned->analysis.pick == i)
        {
            const auto font = m_fonts->label (10.0f);
            const auto picked = box.translated (box.getWidth() + 4.0f, 0.0f).withWidth (Hud::textWidth (font, "PICKED") + 10.0f);
            g.setColour (space.withAlpha (0.85f));
            g.fillRect (picked);
            Hud::drawText (g, "PICKED", picked, font, textDim, juce::Justification::centred);
        }
    }

    // The target's caption, on its line where it covers neither the line in use nor a tag: the
    // right end above it, then below it, then further left.
    if (const auto spans = targetSpans(); ! spans.empty())
    {
        // The second and later sounds: a small note tag at the start of each segment, where there is room.
        const auto noteFont = m_fonts->label (10.0f);
        for (std::size_t i = 1; i < spans.size(); ++i)
        {
            const auto words = PitchFlattenerAudioProcessor::noteText (spans[i].midi);
            const float w = Hud::textWidth (noteFont, words) + 12.0f;
            const float x0 = std::max (xFor (spans[i].start), m_plot.getX()), x1 = std::min (xFor (spans[i].end), m_plot.getRight());
            const float sy = yFor (spans[i].midi);
            if (x1 - x0 < w + 6.0f || sy < m_plot.getY() || sy > m_plot.getBottom())
                continue;
            auto tag = juce::Rectangle<float> (x0 + 3.0f, sy - 19.0f, w, 15.0f);
            if (tag.getY() < m_plot.getY())
                tag.setY (sy + 4.0f);
            Hud::frame (g, tag, 4.0f, space.withAlpha (0.88f), orange, 1.0f);
            Hud::drawText (g, words, tag, noteFont, orange, juce::Justification::centred);
        }
    }

    if (const auto spans = targetSpans(); ! spans.empty())
    {
        const auto& first = spans.front();
        const float y = yFor (first.midi);
        const auto caption = "TARGET " + PitchFlattenerAudioProcessor::noteText (first.midi);
        const auto font = m_fonts->display (11.0f);
        const float w = Hud::shearedWidth (font, caption) + 16.0f;
        float tagsRight = m_plot.getX();
        for (const auto& b : boxes)
            if (! b.isEmpty())
                tagsRight = std::max (tagsRight, b.getRight() + 70.0f);
        const auto covers = [&] (juce::Rectangle<float> box)
        {
            const auto zone = box.expanded (4.0f);
            if (! m_plot.contains (box))
                return true;
            for (const auto& b : boxes)
                if (! b.isEmpty() && zone.intersects (b.withWidth (b.getWidth() + 70.0f)))
                    return true;
            if (m_view.lineInUse < 0)
                return false;
            for (float x = zone.getX(); x <= zone.getRight(); x += 2.0f)
                if (zone.contains (x, yFor (correctedAt (secondsFor (x)))))
                    return true;
            return false;
        };
        // (Several sounds: the caption belongs to the first, so it stays over the first segment.)
        const float right = spans.size() > 1 ? std::clamp (xFor (first.end), m_plot.getX() + w, m_plot.getRight()) : m_plot.getRight();
        const juce::Rectangle<float> candidates[] = {
            { right - w, y - 20.0f, w, 17.0f }, { right - w, y + 4.0f, w, 17.0f },
            { m_plot.getX() + 8.0f, y - 20.0f, w, 17.0f },   { m_plot.getX() + 8.0f, y + 4.0f, w, 17.0f },
            { tagsRight, y - 20.0f, w, 17.0f },               { tagsRight, y + 4.0f, w, 17.0f },
            { m_plot.getCentreX() - w * 0.5f, y - 20.0f, w, 17.0f }, { m_plot.getCentreX() - w * 0.5f, y + 4.0f, w, 17.0f }
        };
        auto box = candidates[0];
        for (const auto& c : candidates)
            if (! covers (c))
            {
                box = c;
                break;
            }
        if (y >= m_plot.getY() - 4.0f && y <= m_plot.getBottom() + 4.0f)
        {
            Hud::frame (g, box, 5.0f, orange, juce::Colours::transparentBlack);
            Hud::drawSheared (g, caption, box.reduced (6.0f, 0.0f), font, space, juce::Justification::centred);
        }
    }

    // Play head: where the flattened sound is, as a moment of the original.
    if (headX >= m_plot.getX() && headX <= m_plot.getRight())
    {
        g.setColour (lime);
        g.fillRect (juce::Rectangle<float> (std::round (headX) - 1.0f, m_plot.getY() - 2.0f, 2.0f, m_strip.getBottom() - m_plot.getY() + 2.0f));
        juce::Path head;
        head.addTriangle (headX - 6.0f, m_plot.getY() - 6.0f, headX + 6.0f, m_plot.getY() - 6.0f, headX, m_plot.getY() + 3.0f);
        g.fillPath (head);
    }

    // What the gesture is doing, in words: the corrected note, and the correction in cents.
    if (m_gesture != Gesture::None && m_readoutSeconds >= 0.0 && m_view.lineInUse >= 0)
    {
        const double pitch = correctedAt (m_readoutSeconds);
        const int nearest = (int) std::lround (pitch);
        const int cents = (int) std::lround ((pitch - nearest) * 100.0);
        const int correction = (int) std::lround (core::editAt (points(), m_readoutSeconds) * 100.0);
        const auto words = noteLabel (pitch) + " " + (cents >= 0 ? "+" : "") + juce::String (cents) + "  /  CORRECTION "
                         + (correction >= 0 ? "+" : "") + juce::String (correction) + " C";
        const auto font = m_fonts->label (10.0f);
        const float w = Hud::textWidth (font, words) + 12.0f;
        const float x = std::clamp (xFor (m_readoutSeconds) + 12.0f, m_plot.getX(), m_plot.getRight() - w);
        const float py = yFor (pitch);
        const float y = py > m_plot.getY() + 24.0f ? py - 22.0f : py + 10.0f;
        const auto box = juce::Rectangle<float> (x, std::clamp (y, m_plot.getY(), m_plot.getBottom() - 15.0f), w, 15.0f);
        Hud::frame (g, box, 4.0f, space.withAlpha (0.92f), orange, 1.0f);
        Hud::drawText (g, words, box.reduced (6.0f, 0.0f), font, text, juce::Justification::centredLeft);
    }

    drawOverview (g);
}

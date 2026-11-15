#include "PitchDisplay.h"

#include "Hud.h"
#include "PluginProcessor.h"

#include <cmath>

namespace
{
    namespace core = pitchflattener::core;

    const char* const c_lineNames[] = { "HARMONIC", "RIDGE" };

    juce::String noteLabel (double midi)
    {
        static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int n = (int) std::lround (midi);
        return juce::String (names[((n % 12) + 12) % 12]) + juce::String (n / 12 - 1);
    }

    constexpr float c_noteAxisWidth = 42.0f;
    constexpr float c_stripHeight = 34.0f;
    constexpr float c_timeAxisHeight = 16.0f;
}

bool PitchDisplay::View::operator== (const View& o) const noexcept
{
    return result == o.result && state == o.state && std::abs (captureSeconds - o.captureSeconds) < 0.05
        && playPosition == o.playPosition && playGeneration == o.playGeneration && rendering == o.rendering && autoLine == o.autoLine;
}

PitchDisplay::PitchDisplay()
{
    setOpaque (false);
}

void PitchDisplay::setView (const View& view)
{
    if (view == m_view)
        return;

    const bool newResult = view.result != m_view.result;
    // While it plays, only the play head moves: repaint the strip it moved across (with the lime
    // ticks it adds behind it), not the whole graph.
    const bool onlyHeadMoved = ! newResult && view.state == m_view.state && view.rendering == m_view.rendering
                            && view.autoLine == m_view.autoLine && std::abs (view.captureSeconds - m_view.captureSeconds) < 0.05;
    const auto headX = [this] (const View& v) -> float
    {
        const auto* r = v.result.get();
        if (r == nullptr || ! r->playable() || v.playPosition < 0 || v.playGeneration != r->generation)
            return -1.0f;
        const double source = r->flattened.sourceSampleFor ((double) v.playPosition) / r->learned->sampleRate;
        return m_geometry.xFor (juce::jlimit (0.0, m_geometry.seconds, source));
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

void PitchDisplay::resized()
{
    rebuild();
}

void PitchDisplay::rebuild()
{
    auto area = getLocalBounds().toFloat().reduced (10.0f, 8.0f);
    m_geometry.timeAxis = area.removeFromBottom (c_timeAxisHeight).withTrimmedLeft (c_noteAxisWidth);
    m_geometry.strip = area.removeFromBottom (c_stripHeight).withTrimmedLeft (c_noteAxisWidth);
    area.removeFromBottom (6.0f);
    m_geometry.plot = area.withTrimmedLeft (c_noteAxisWidth).withTrimmedTop (4.0f);
    m_labels = {};
    m_peaks.clear();

    const auto* r = m_view.result.get();
    if (r == nullptr || r->learned == nullptr)
        return;

    const auto& learned = *r->learned;
    m_geometry.seconds = std::max (0.01, (double) learned.locator.length() / learned.sampleRate);

    // The note range: where the usable readings sit (by level), and the target.
    double lo = 1.0e9, hi = -1.0e9;
    for (const auto& line : learned.analysis.lines)
        if (line.usable)
        {
            lo = std::min (lo, line.lowMidi);
            hi = std::max (hi, line.highMidi);
        }
    if (r->playable())
    {
        lo = std::min (lo, r->flattened.targetMidi);
        hi = std::max (hi, r->flattened.targetMidi);
    }
    if (lo > hi)
    {
        // Nothing usable: frame whatever the readings found.
        for (const auto& line : learned.analysis.lines)
            if (! line.midi.empty())
            {
                lo = std::min (lo, line.centreMidi - 6.0);
                hi = std::max (hi, line.centreMidi + 6.0);
            }
        if (lo > hi)
        {
            lo = 48.0;
            hi = 72.0;
        }
    }
    lo -= 3.0;
    hi += 3.0;
    if (hi - lo < 12.0)
    {
        const double mid = 0.5 * (lo + hi);
        lo = mid - 6.0;
        hi = mid + 6.0;
    }
    m_geometry.lowMidi = lo;
    m_geometry.highMidi = hi;

    // The level strip: the loudest sample under each column.
    const int columns = std::max (1, (int) m_geometry.strip.getWidth());
    const auto length = learned.locator.length();
    const float* x = learned.locator.samples();
    m_peaks.assign ((size_t) columns, 0.0f);
    float loudest = 1.0e-6f;
    for (int c = 0; c < columns; ++c)
    {
        const auto a = length * c / columns, b = std::max (a + 1, length * (c + 1) / columns);
        float peak = 0.0f;
        for (auto i = a; i < b && i < length; ++i)
            peak = std::max (peak, std::abs (x[i]));
        m_peaks[(size_t) c] = peak;
        loudest = std::max (loudest, peak);
    }
    for (auto& p : m_peaks)
        p /= loudest;

    // Each line's label sits just above the line near its start, kept clear of the other's.
    const auto font = m_fonts->label (10.0f);
    for (int i = 0; i < core::kNumTrackers; ++i)
    {
        const auto& line = learned.analysis.lines[(size_t) i];
        if (line.midi.empty())
            continue;
        const auto frame = std::min (line.midi.size() - 1, (size_t) ((0.04 * m_geometry.seconds) / line.hopSeconds));
        const float y = m_geometry.yFor (line.midi[frame]);
        const float w = juce::GlyphArrangement::getStringWidth (font, c_lineNames[i]) + 14.0f;
        auto box = juce::Rectangle<float> (m_geometry.plot.getX() + 6.0f, y - 20.0f, w, 16.0f);
        if (i == 1 && ! m_labels[0].isEmpty() && box.intersects (m_labels[0].expanded (0.0f, 2.0f)))
            box.setY (m_labels[0].getBottom() + 4.0f);
        box = box.constrainedWithin (m_geometry.plot.reduced (2.0f));
        m_labels[(size_t) i] = box;
    }
}

int PitchDisplay::lineAt (juce::Point<float> p) const
{
    const auto* r = m_view.result.get();
    if (r == nullptr || r->learned == nullptr)
        return -1;

    for (int i = 0; i < core::kNumTrackers; ++i)
        if (m_labels[(size_t) i].expanded (3.0f).contains (p))
            return i;

    if (! m_geometry.plot.contains (p))
        return -1;

    const double seconds = (p.x - m_geometry.plot.getX()) / m_geometry.plot.getWidth() * m_geometry.seconds;
    int best = -1;
    float bestDistance = 7.0f;
    for (int i = 0; i < core::kNumTrackers; ++i)
    {
        const auto& line = r->learned->analysis.lines[(size_t) i];
        if (line.midi.empty())
            continue;
        const auto frame = (size_t) juce::jlimit (0.0, (double) line.midi.size() - 1.0, seconds / line.hopSeconds - 0.5);
        const float d = std::abs (m_geometry.yFor (line.midi[frame]) - p.y);
        if (d < bestDistance)
        {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

void PitchDisplay::mouseMove (const juce::MouseEvent& e)
{
    const int line = lineAt (e.position);
    const auto* r = m_view.result.get();
    const bool clickable = line >= 0 && r != nullptr && r->learned->analysis.lines[(size_t) line].usable && r->options.line != line;
    setMouseCursor (clickable ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);

    if (line != m_hoverLine)
    {
        m_hoverLine = line;
        repaint();
    }
}

void PitchDisplay::mouseExit (const juce::MouseEvent&)
{
    if (m_hoverLine >= 0)
    {
        m_hoverLine = -1;
        repaint();
    }
}

void PitchDisplay::mouseDown (const juce::MouseEvent& e)
{
    const int line = lineAt (e.position);
    const auto* r = m_view.result.get();
    if (line < 0 || r == nullptr || ! r->learned->analysis.lines[(size_t) line].usable)
        return;
    if (onLineClicked != nullptr)
        onLineClicked (line);
}

juce::String PitchDisplay::getTooltip()
{
    const auto* r = m_view.result.get();
    const int line = lineAt (getMouseXYRelative().toFloat());

    if (r != nullptr && line >= 0)
    {
        const auto& reading = r->learned->analysis.lines[(size_t) line];
        const juce::String what = line == 0
            ? "The harmonic reading: the sound's pitch from its whole series of harmonics. Suits voices, engines and whines up to about 1 kHz."
            : "The ridge reading: the strongest partial, followed from start to end. Suits sirens, whistles and anything pitched higher.";

        if (! reading.usable)
            return what + " It found too little pitch in this sound to flatten by.";
        if (r->options.line == line)
            return what + " The sound is flattened by this line.";
        return what + " Click to flatten the sound by this line instead.";
    }

    return "Scope: the learned sound from start to end, its pitch over time on a note scale (the line it is flattened by "
           "in cyan, dashed where the line only bridges a gap with no pitch), the pitch it is flattened to in orange, and "
           "its level along the bottom. While the flattened sound plays, a lime line shows where it is.";
}

//==============================================================================
void PitchDisplay::drawLine (juce::Graphics& g, int index, bool inUse) const
{
    const auto& line = m_view.result->learned->analysis.lines[(size_t) index];
    if (line.midi.empty())
        return;

    const auto colour = inUse ? Theme::Colours::cyan
                              : (m_hoverLine == index && line.usable ? Theme::Colours::textDim.brighter (0.5f) : Theme::Colours::textDim.withAlpha (0.75f));
    const float width = inUse ? 2.4f : 1.4f;
    const float alpha = line.usable ? 1.0f : 0.45f;

    // Two paths: the stretches the tracker read (solid), and the stretches where the line only
    // bridges a gap (dashed: every other 4 px of it). Each step from one frame to the next goes into
    // the path its later frame belongs to.
    juce::Path read, bridged;
    const size_t frames = line.midi.size();
    const size_t step = std::max<size_t> (1, frames / (size_t) std::max (1.0f, m_geometry.plot.getWidth() * 2.0f));
    float px = 0.0f, py = 0.0f;
    bool readOpen = false, dashOpen = false;
    for (size_t f = 0; f < frames; f += step)
    {
        const float x = m_geometry.xFor (((double) f + 0.5) * line.hopSeconds);
        const float y = m_geometry.yFor (line.midi[f]);
        const bool gap = f < line.trackedHz.size() && line.trackedHz[f] <= 0.0f;
        if (f > 0)
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
        px = x;
        py = y;
    }

    g.setColour (colour.withAlpha (alpha));
    g.strokePath (read, juce::PathStrokeType (width, juce::PathStrokeType::mitered, juce::PathStrokeType::square));
    g.setColour (colour.withMultipliedAlpha (alpha * 0.55f));
    g.strokePath (bridged, juce::PathStrokeType (width, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
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
    const auto& geo = m_geometry;

    // Nothing learned: an empty scale (the banner over it says what to do).
    if (r == nullptr || r->learned == nullptr)
    {
        g.setColour (grid);
        for (int i = 1; i < 6; ++i)
            g.drawHorizontalLine ((int) std::round (geo.plot.getY() + geo.plot.getHeight() * (float) i / 6.0f), geo.plot.getX(), geo.plot.getRight());
        for (int i = 0; i <= 8; ++i)
            g.drawVerticalLine ((int) std::round (geo.plot.getX() + geo.plot.getWidth() * (float) i / 8.0f), geo.plot.getY(), geo.strip.getBottom());
        return;
    }

    // Note grid: a line on every C, and on other notes when there is room for their labels.
    {
        const double span = geo.highMidi - geo.lowMidi;
        const double pxPerSemitone = geo.plot.getHeight() / span;
        int step = 12;
        for (const int s : { 1, 2, 3, 4, 6, 12 })
            if (pxPerSemitone * s >= 18.0)
            {
                step = s;
                break;
            }
        const auto font = m_fonts->label (10.0f);
        for (int n = (int) std::ceil (geo.lowMidi); n <= (int) std::floor (geo.highMidi); ++n)
        {
            if (n % step != 0)
                continue;
            const float y = geo.yFor (n);
            g.setColour (n % 12 == 0 ? panelEdge : grid);
            g.drawHorizontalLine ((int) std::round (y), geo.plot.getX(), geo.plot.getRight());
            Hud::drawText (g, noteLabel (n), juce::Rectangle<float> (geo.plot.getX() - c_noteAxisWidth, y - 7.0f, c_noteAxisWidth - 6.0f, 14.0f),
                           font, n % 12 == 0 ? text.withAlpha (0.8f) : textDim, juce::Justification::centredRight);
        }
    }

    // Time axis.
    {
        double tick = 0.1;
        for (const double t : { 0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 30.0, 60.0 })
            if (geo.plot.getWidth() * t / geo.seconds >= 64.0)
            {
                tick = t;
                break;
            }
        const auto font = m_fonts->label (10.0f);
        for (double t = 0.0; t <= geo.seconds + 1.0e-9; t += tick)
        {
            const float x = geo.xFor (t);
            g.setColour (grid);
            g.drawVerticalLine ((int) std::round (x), geo.plot.getY(), geo.strip.getBottom());
            const auto label = juce::String (t, tick < 1.0 ? (tick < 0.25 ? 1 : 2) : 0) + " S";
            auto box = juce::Rectangle<float> (x - 32.0f, geo.timeAxis.getY() + 1.0f, 64.0f, 14.0f);
            if (box.getRight() > geo.timeAxis.getRight() + 4.0f || box.getX() < geo.timeAxis.getX() - 34.0f)
                continue;
            Hud::drawText (g, label, box, font, textDim, juce::Justification::centred);
        }
    }

    // Level strip: what was heard, so in cyan, dimmed to stay behind the lines.
    {
        g.setColour (cyan.withAlpha (0.32f));
        const float mid = geo.strip.getCentreY(), half = geo.strip.getHeight() * 0.5f - 1.0f;
        for (size_t c = 0; c < m_peaks.size(); ++c)
        {
            const float h = std::max (0.5f, m_peaks[c] * half);
            g.fillRect (geo.strip.getX() + (float) c, mid - h, 1.0f, 2.0f * h);
        }
    }

    const bool playing = r->playable() && m_view.playPosition >= 0 && m_view.playGeneration == r->generation;
    float headX = -1.0f;
    if (playing)
    {
        const double source = r->flattened.sourceSampleFor ((double) m_view.playPosition) / r->learned->sampleRate;
        headX = geo.xFor (juce::jlimit (0.0, geo.seconds, source));
    }

    // The pitch it is flattened to (orange: the user's choice), and behind the play head the stretch
    // already heard flattened, as a trail of lime ticks along it.
    if (r->playable())
    {
        const float y = geo.yFor (r->flattened.targetMidi);
        g.setColour (orange);
        g.fillRect (juce::Rectangle<float> (geo.plot.getX(), std::round (y) - 0.75f, geo.plot.getWidth(), 1.5f));
        if (headX > geo.plot.getX())
        {
            g.setColour (lime.withAlpha (0.85f));
            for (float x = geo.plot.getX() + 2.0f; x < headX - 3.0f; x += 7.0f)
                g.fillRect (juce::Rectangle<float> (x, std::round (y) - 4.0f, 2.0f, 8.0f));
        }
    }

    // The two readings: the one in use on top.
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (geo.plot.expanded (0.0f, 2.0f).toNearestInt());
        const int inUse = r->options.line;
        for (int i = 0; i < core::kNumTrackers; ++i)
            if (i != inUse)
                drawLine (g, i, false);
        if (inUse >= 0)
            drawLine (g, inUse, true);
    }

    // Labels.
    for (int i = 0; i < core::kNumTrackers; ++i)
    {
        const auto box = m_labels[(size_t) i];
        if (box.isEmpty())
            continue;
        const auto& line = r->learned->analysis.lines[(size_t) i];
        const bool inUse = r->options.line == i;
        const auto colour = inUse ? cyan : (m_hoverLine == i && line.usable ? textDim.brighter (0.5f) : textDim);
        Hud::frame (g, box, 4.0f, space.withAlpha (0.88f), colour.withMultipliedAlpha (line.usable ? 1.0f : 0.6f), 1.0f);
        Hud::drawText (g, c_lineNames[i], box, m_fonts->label (10.0f), colour.withMultipliedAlpha (line.usable ? 1.0f : 0.6f), juce::Justification::centred);
    }
    if (r->options.line >= 0 && m_view.autoLine)
    {
        const auto box = m_labels[(size_t) r->options.line];
        Hud::drawText (g, "PICKED", box.translated (box.getWidth() + 6.0f, 0.0f).withWidth (64.0f), m_fonts->label (10.0f), textDim, juce::Justification::centredLeft);
    }

    // The target's caption, on its line where it covers neither the line in use nor a label: the
    // right end above it, then below it, then the left end (after the line labels).
    if (r->playable())
    {
        const float y = geo.yFor (r->flattened.targetMidi);
        const auto caption = "TARGET " + PitchFlattenerAudioProcessor::noteText (r->flattened.targetMidi);
        const auto font = m_fonts->display (11.0f);
        const float w = Hud::shearedWidth (font, caption) + 16.0f;
        const auto* inUse = r->options.line >= 0 ? &r->learned->analysis.lines[(size_t) r->options.line] : nullptr;
        const auto covers = [&] (juce::Rectangle<float> box)
        {
            const auto zone = box.expanded (4.0f);
            if (! geo.plot.contains (box) || zone.intersects (m_labels[0]) || zone.intersects (m_labels[1]))
                return true;
            if (inUse == nullptr || inUse->midi.empty())
                return false;
            const auto frameAt = [&] (float x) { return (double) (x - geo.plot.getX()) / geo.plot.getWidth() * geo.seconds / inUse->hopSeconds; };
            const auto f0 = (size_t) juce::jlimit (0.0, (double) inUse->midi.size() - 1.0, frameAt (zone.getX()));
            const auto f1 = (size_t) juce::jlimit (0.0, (double) inUse->midi.size() - 1.0, frameAt (zone.getRight()));
            for (auto f = f0; f <= f1; ++f)
                if (zone.contains (zone.getCentreX(), geo.yFor (inUse->midi[f])))
                    return true;
            return false;
        };
        float labelsRight = geo.plot.getX();
        for (const auto& l : m_labels)
            if (! l.isEmpty())
                labelsRight = std::max (labelsRight, l.getRight() + 70.0f);   // past the label and its PICKED note
        const juce::Rectangle<float> candidates[] = {
            { geo.plot.getRight() - w, y - 20.0f, w, 17.0f }, { geo.plot.getRight() - w, y + 4.0f, w, 17.0f },
            { geo.plot.getX() + 8.0f, y - 20.0f, w, 17.0f },   { geo.plot.getX() + 8.0f, y + 4.0f, w, 17.0f },
            { labelsRight, y - 20.0f, w, 17.0f },               { labelsRight, y + 4.0f, w, 17.0f },
            { geo.plot.getCentreX() - w * 0.5f, y - 20.0f, w, 17.0f }, { geo.plot.getCentreX() - w * 0.5f, y + 4.0f, w, 17.0f }
        };
        auto box = candidates[0];
        for (const auto& c : candidates)
            if (! covers (c))
            {
                box = c;
                break;
            }
        Hud::frame (g, box, 5.0f, orange, juce::Colours::transparentBlack);
        Hud::drawSheared (g, caption, box.reduced (6.0f, 0.0f), font, space, juce::Justification::centred);
    }

    // Play head: where the flattened sound is, as a moment of the original.
    if (headX >= 0.0f)
    {
        g.setColour (lime);
        g.fillRect (juce::Rectangle<float> (std::round (headX) - 1.0f, geo.plot.getY() - 2.0f, 2.0f, geo.strip.getBottom() - geo.plot.getY() + 2.0f));
        juce::Path head;
        head.addTriangle (headX - 6.0f, geo.plot.getY() - 6.0f, headX + 6.0f, geo.plot.getY() - 6.0f, headX, geo.plot.getY() + 3.0f);
        g.fillPath (head);
    }

    // A new render on its way.
    if (m_view.rendering)
    {
        const auto font = m_fonts->display (11.0f);
        const juce::String words = "RENDERING";
        const float w = Hud::shearedWidth (font, words) + 20.0f;
        const auto box = juce::Rectangle<float> (geo.plot.getCentreX() - w * 0.5f, geo.plot.getY() + 2.0f, w, 18.0f);
        Hud::frame (g, box, 5.0f, space.withAlpha (0.9f), lime.withAlpha (0.8f), 1.0f);
        Hud::drawSheared (g, words, box.reduced (6.0f, 0.0f), font, lime, juce::Justification::centred);
    }
}

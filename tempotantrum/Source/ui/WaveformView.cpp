#include "WaveformView.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tt::ui
{

namespace
{
    constexpr int rulerHeight = 22;
    constexpr int overviewHeight = 4;
    constexpr float pinGrabPixels = 5.0f;
    constexpr double shortestSpan = 0.2;        // seconds: the closest zoom
    constexpr double pinSpacing = 0.005;        // seconds kept between a dragged pin and its neighbours

    juce::String ellipsised (const char* text)
    {
        return juce::String::fromUTF8 (text) + juce::String::charToString ((juce::juce_wchar) 0x2026);
    }

    long long floorDiv (long long a, long long b)
    {
        const long long q = a / b;
        return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
    }

    // Reads "46", "46|1", "46|1.5" or "46|1|480" as a beat number counted from bar 1 beat 1 = 0.
    bool parseBarBeat (const juce::String& text, int beatsPerBar, double& beatOut)
    {
        juce::StringArray parts;
        parts.addTokens (text.trim(), "| ", "");
        parts.removeEmptyStrings();

        if (parts.isEmpty() || parts.size() > 3)
            return false;

        for (const auto& part : parts)
            if (! part.containsOnly ("0123456789.") || ! part.containsAnyOf ("0123456789"))
                return false;

        if (! parts[0].containsOnly ("0123456789"))
            return false;

        const int bar = parts[0].getIntValue();
        const double beatInBar = parts.size() > 1 ? parts[1].getDoubleValue() : 1.0;
        const double ticks = parts.size() > 2 ? parts[2].getDoubleValue() : 0.0;

        if (bar < 1 || beatInBar < 1.0 || beatInBar >= (double) beatsPerBar + 1.0 || ticks < 0.0 || ticks >= 960.0)
            return false;

        beatOut = (double) (bar - 1) * (double) beatsPerBar + (beatInBar - 1.0) + ticks / 960.0;
        return true;
    }
}

//==============================================================================
void WaveformView::PeakCache::build (const juce::AudioBuffer<float>& buffer)
{
    clear();

    const int n = buffer.getNumSamples();
    const int channels = buffer.getNumChannels();

    if (n <= 0 || channels <= 0)
        return;

    numSamples = n;

    // The finest level, from the samples: channels are averaged to mono.
    const int base = 16;
    const size_t buckets = (size_t) ((n + base - 1) / base);
    std::vector<float> lo (buckets), hi (buckets);
    const float gain = 1.0f / (float) channels;
    float peak = 0.0f;

    for (size_t b = 0; b < buckets; ++b)
    {
        const int start = (int) b * base;
        const int end = juce::jmin (n, start + base);
        float mn = std::numeric_limits<float>::max();
        float mx = std::numeric_limits<float>::lowest();

        for (int i = start; i < end; ++i)
        {
            float s = 0.0f;

            for (int c = 0; c < channels; ++c)
                s += buffer.getReadPointer (c)[i];

            s *= gain;
            mn = juce::jmin (mn, s);
            mx = juce::jmax (mx, s);
        }

        lo[b] = mn;
        hi[b] = mx;
        peak = juce::jmax (peak, -mn, mx);
    }

    // Stored so the loudest sample fills the lane: quiet stems stay readable.
    const float scale = 32767.0f / (peak > 1.0e-4f ? peak : 1.0f);

    Level first;
    first.samplesPerBucket = base;
    first.lo.resize (buckets);
    first.hi.resize (buckets);

    for (size_t b = 0; b < buckets; ++b)
    {
        first.lo[b] = (std::int16_t) juce::jlimit (-32767.0f, 32767.0f, lo[b] * scale);
        first.hi[b] = (std::int16_t) juce::jlimit (-32767.0f, 32767.0f, hi[b] * scale);
    }

    levels.push_back (std::move (first));

    // Each coarser level joins eight buckets of the one before: 128, 1024 and 8192 samples.
    for (int k = 0; k < 3; ++k)
    {
        const Level& fine = levels.back();
        const size_t count = (fine.lo.size() + 7) / 8;

        Level coarse;
        coarse.samplesPerBucket = fine.samplesPerBucket * 8;
        coarse.lo.resize (count);
        coarse.hi.resize (count);

        for (size_t b = 0; b < count; ++b)
        {
            const size_t start = b * 8;
            const size_t end = juce::jmin (fine.lo.size(), start + 8);
            std::int16_t mn = fine.lo[start], mx = fine.hi[start];

            for (size_t i = start + 1; i < end; ++i)
            {
                mn = juce::jmin (mn, fine.lo[i]);
                mx = juce::jmax (mx, fine.hi[i]);
            }

            coarse.lo[b] = mn;
            coarse.hi[b] = mx;
        }

        levels.push_back (std::move (coarse));
    }
}

void WaveformView::PeakCache::read (double firstSample, double lastSample, double samplesPerPixel,
                                    float& lo, float& hi) const
{
    lo = hi = 0.0f;

    if (levels.empty() || lastSample <= 0.0 || firstSample >= (double) numSamples)
        return;

    // The coarsest level that is still finer than one pixel.
    const Level* level = &levels.front();

    for (const auto& l : levels)
        if ((double) l.samplesPerBucket <= samplesPerPixel)
            level = &l;

    const auto size = (juce::int64) level->lo.size();
    const double perBucket = (double) level->samplesPerBucket;
    const auto b0 = juce::jlimit ((juce::int64) 0, size - 1, (juce::int64) std::floor (firstSample / perBucket));
    const auto b1 = juce::jlimit (b0 + 1, size, (juce::int64) std::ceil (lastSample / perBucket));

    std::int16_t mn = level->lo[(size_t) b0], mx = level->hi[(size_t) b0];

    for (auto b = b0 + 1; b < b1; ++b)
    {
        mn = juce::jmin (mn, level->lo[(size_t) b]);
        mx = juce::jmax (mx, level->hi[(size_t) b]);
    }

    lo = (float) mn / 32767.0f;
    hi = (float) mx / 32767.0f;
}

//==============================================================================
WaveformView::WaveformView()
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    setTitle ("Waveform");
    setDescription ("The music with the bar and beat grid and the pins. Drag a pin to move it, "
                    "double-click a hit to add one, right-click for more.");

    addChildComponent (barEditor);
    barEditor.setTitle ("Bar and beat of this hit");
    barEditor.setFont (Theme::mono (12.0f));
    barEditor.setJustification (juce::Justification::centredLeft);
    barEditor.setIndents (7, 0);
    barEditor.setSelectAllWhenFocused (true);
    barEditor.setInputRestrictions (14, "0123456789|. ");
    barEditor.setColour (juce::TextEditor::backgroundColourId, Theme::panel());
    barEditor.setColour (juce::TextEditor::textColourId, Theme::text());
    barEditor.setColour (juce::TextEditor::outlineColourId, Theme::pin());
    barEditor.setColour (juce::TextEditor::focusedOutlineColourId, Theme::pin());
    barEditor.setColour (juce::TextEditor::highlightColourId, Theme::wave().withAlpha (0.45f));
    barEditor.setColour (juce::TextEditor::highlightedTextColourId, Theme::text());
    barEditor.setColour (juce::CaretComponent::caretColourId, Theme::pin());

    barEditor.onReturnKey = [this] { hideBarEditor (true); };
    barEditor.onEscapeKey = [this] { hideBarEditor (false); };
    barEditor.onFocusLost = [this] { hideBarEditor (false); };
}

WaveformView::~WaveformView() = default;

//==============================================================================
void WaveformView::setAudio (const juce::AudioBuffer<float>* mix, const juce::AudioBuffer<float>* drums,
                             double newSampleRate)
{
    const double oldDuration = duration;

    mixPeaks.clear();
    drumPeaks.clear();
    sampleRate = newSampleRate;
    duration = 0.0;

    if (mix != nullptr && sampleRate > 0.0 && mix->getNumSamples() > 0 && mix->getNumChannels() > 0)
    {
        mixPeaks.build (*mix);
        duration = (double) mix->getNumSamples() / sampleRate;

        if (drums != nullptr && drums->getNumSamples() > 0 && drums->getNumChannels() > 0)
            drumPeaks.build (*drums);
    }

    hideBarEditor (false);
    pressedPin = -1;
    dragging = false;

    if (! hasAudio())
    {
        viewStart = viewEnd = 0.0;
        repaint();
    }
    else if (std::abs (duration - oldDuration) > 1.0e-6 || span() <= 0.0)
    {
        showAll();
    }
    else
    {
        // Same length as before (a drum stem was added or removed): keep what is in view.
        repaint();
    }
}

void WaveformView::setMap (const tt::MapResult& newMap, int newBeatsPerBar)
{
    // Keep the selection on the same hit: pin numbers change when the map is rebuilt.
    double wanted = keepSelectionNear;

    if (wanted < 0.0 && selectedPin >= 0 && selectedPin < (int) map.pins.size())
        wanted = map.pins[(size_t) selectedPin].time;

    hideBarEditor (false);

    map = newMap;
    beatsPerBar = juce::jmax (1, newBeatsPerBar);

    std::sort (map.onsets.begin(), map.onsets.end(),
               [] (const tt::Onset& a, const tt::Onset& b) { return a.time < b.time; });
    std::sort (map.checks.begin(), map.checks.end(),
               [] (const tt::Check& a, const tt::Check& b) { return a.startTime < b.startTime; });

    grid = tt::BeatGrid (map.pins, map.bpm > 0.0 ? map.bpm : 120.0);

    selectedPin = -1;
    hoverPin = -1;
    pressedPin = -1;
    dragging = false;
    keepSelectionNear = -1.0;

    if (wanted >= 0.0)
    {
        double best = 0.002;

        for (size_t i = 0; i < map.pins.size(); ++i)
        {
            const double d = std::abs (map.pins[i].time - wanted);

            if (d <= best)
            {
                best = d;
                selectedPin = (int) i;
            }
        }
    }

    repaint();
}

void WaveformView::setPlayhead (double seconds)
{
    if (juce::exactlyEqual (seconds, playhead))
        return;

    const double previous = playhead;
    playhead = seconds;

    if (! hasAudio())
        return;

    // A small step forward means the music is playing. When the playhead reaches the right
    // edge of what is in view, the view moves on so the playhead sits near its left edge.
    const double step = seconds - previous;
    const bool playing = step > 0.0 && step < 0.5;
    const bool wasInView = previous >= viewStart && previous <= viewEnd;

    if (playing && wasInView && ! dragging && barEditorPin < 0
        && seconds >= viewEnd - span() * 0.02 && viewEnd < duration - 1.0e-6)
    {
        const double length = span();
        setView (seconds - length * 0.05, seconds + length * 0.95);
        return;
    }

    repaintPlayheadAt (previous);
    repaintPlayheadAt (seconds);
}

void WaveformView::repaintPlayheadAt (double seconds)
{
    if (seconds < viewStart || seconds > viewEnd)
        return;

    repaint ((int) std::floor (timeToX (seconds)) - 2, 0, 5, getHeight());
}

void WaveformView::setBusy (bool analysing)
{
    if (busy == analysing)
        return;

    busy = analysing;

    if (busy)
    {
        hideBarEditor (false);
        pressedPin = -1;
        dragging = false;
    }

    repaint();
}

void WaveformView::showRange (double startSeconds, double endSeconds)
{
    if (hasAudio() && endSeconds > startSeconds)
        setView (startSeconds, endSeconds);
}

void WaveformView::showAll()
{
    if (hasAudio())
        setView (0.0, duration);
}

int WaveformView::getSelectedPin() const
{
    return selectedPin;
}

void WaveformView::setSelectedPin (int pinIndex)
{
    const int wanted = pinIndex >= 0 && pinIndex < (int) map.pins.size() ? pinIndex : -1;

    if (wanted != selectedPin)
    {
        selectedPin = wanted;
        repaint();
    }
}

//==============================================================================
WaveformView::Layout WaveformView::getLayout() const
{
    Layout l;
    auto area = getLocalBounds();

    l.ruler = area.removeFromTop (juce::jmin (rulerHeight, area.getHeight()));
    l.overview = area.removeFromBottom (juce::jmin (overviewHeight, area.getHeight()));
    area.removeFromBottom (juce::jmin (2, area.getHeight()));
    l.lanes = area;

    if (! drumPeaks.empty())
    {
        l.music = area.removeFromTop (juce::roundToInt ((float) area.getHeight() * 0.7f));
        l.drums = area;
    }
    else
    {
        l.music = area;
    }

    return l;
}

double WaveformView::pixelsPerSecond() const
{
    return span() > 0.0 ? (double) getWidth() / span() : 0.0;
}

float WaveformView::timeToX (double seconds) const
{
    return (float) ((seconds - viewStart) * pixelsPerSecond());
}

double WaveformView::xToTime (float x) const
{
    const double pps = pixelsPerSecond();
    return pps > 0.0 ? viewStart + (double) x / pps : 0.0;
}

void WaveformView::setView (double start, double end)
{
    if (duration <= 0.0)
        return;

    const double length = juce::jlimit (juce::jmin (shortestSpan, duration), duration, end - start);
    start = juce::jlimit (0.0, duration - length, start);

    viewStart = start;
    viewEnd = start + length;
    hideBarEditor (false);
    repaint();
}

void WaveformView::zoomAbout (float x, double factor)
{
    if (! hasAudio() || factor <= 0.0 || getWidth() <= 0)
        return;

    const double anchor = xToTime (x);
    const double length = juce::jlimit (juce::jmin (shortestSpan, duration), duration, span() / factor);
    const double start = anchor - (double) x / (double) getWidth() * length;
    setView (start, start + length);
}

void WaveformView::scrollBy (double seconds)
{
    if (hasAudio())
        setView (viewStart + seconds, viewEnd + seconds);
}

//==============================================================================
double WaveformView::pinTime (int index) const
{
    if (dragging && index == pressedPin)
        return dragTime;

    return map.pins[(size_t) index].time;
}

int WaveformView::pinAt (juce::Point<float> p, const Layout& l) const
{
    if (p.y >= (float) l.lanes.getBottom())
        return -1;

    int best = -1;
    float bestDistance = pinGrabPixels;

    for (size_t i = 0; i < map.pins.size(); ++i)
    {
        const float d = std::abs (timeToX (map.pins[i].time) - p.x);

        if (d <= bestDistance)
        {
            bestDistance = d;
            best = (int) i;
        }
    }

    return best;
}

bool WaveformView::snapToOnset (double& time, float withinPixels, double lowest, double highest) const
{
    const double pps = pixelsPerSecond();

    if (map.onsets.empty() || pps <= 0.0)
        return false;

    const double reach = (double) withinPixels / pps;
    const auto after = std::lower_bound (map.onsets.begin(), map.onsets.end(), time,
                                         [] (const tt::Onset& o, double t) { return o.time < t; });
    double best = reach;
    double found = -1.0;

    auto consider = [&] (const tt::Onset& o)
    {
        const double d = std::abs (o.time - time);

        if (d <= best && o.time >= lowest && o.time <= highest)
        {
            best = d;
            found = o.time;
        }
    };

    if (after != map.onsets.end())
        consider (*after);

    if (after != map.onsets.begin())
        consider (*(after - 1));

    if (found < 0.0)
        return false;

    time = found;
    return true;
}

juce::String WaveformView::pinLabel (double beat) const
{
    const juce::String full (tt::formatBarBeat (beat, beatsPerBar));

    // On a whole beat the ticks say nothing, so "46|1|000" is shown as "46|1".
    return full.endsWith ("|000") ? full.dropLastCharacters (4) : full;
}

//==============================================================================
void WaveformView::collectGrid (std::vector<GridLine>& out, int& labelStep, double& pixelsPerBar) const
{
    labelStep = 1;
    pixelsPerBar = 0.0;

    if (map.pins.empty() || ! hasAudio() || getWidth() <= 0)
        return;

    const double bpm = grid.bpmAtTime (0.5 * (viewStart + viewEnd));

    if (! (bpm > 1.0 && bpm < 2000.0))
        return;

    const double pixelsPerBeat = 60.0 / bpm * pixelsPerSecond();
    pixelsPerBar = pixelsPerBeat * (double) beatsPerBar;

    // Beats are drawn while they are at least 6 px apart. Further out only bars are drawn,
    // then every 2nd, 4th, 8th bar, and bar numbers are kept at least 28 px apart.
    const bool beats = pixelsPerBeat >= 6.0;
    long long barStep = 1;

    while (pixelsPerBar * (double) barStep < 6.0 && barStep < 4096)
        barStep *= 2;

    long long labelEvery = barStep;

    while (pixelsPerBar * (double) labelEvery < 28.0 && labelEvery < 4096)
        labelEvery *= 2;

    labelStep = (int) labelEvery;

    const double b0 = grid.beatAtTime (viewStart);
    const double b1 = grid.beatAtTime (viewEnd);
    const long long stepBeats = beats ? 1 : (long long) beatsPerBar * barStep;

    if (! std::isfinite (b0) || ! std::isfinite (b1) || b1 <= b0 || (b1 - b0) / (double) stepBeats > 6000.0)
        return;

    const float width = (float) getWidth();

    for (long long b = floorDiv ((long long) std::floor (b0), stepBeats) * stepBeats; (double) b <= b1; b += stepBeats)
    {
        const float x = timeToX (grid.timeAtBeat ((double) b));

        if (x < -1.0f || x > width + 1.0f)
            continue;

        GridLine line;
        line.x = std::round (x);
        line.barIndex = floorDiv (b, (long long) beatsPerBar);
        line.bar = (b - line.barIndex * (long long) beatsPerBar) == 0;
        out.push_back (line);
    }
}

void WaveformView::drawWave (juce::Graphics& g, const PeakCache& peaks, juce::Rectangle<int> lane,
                             juce::Colour colour, juce::Rectangle<int> clip) const
{
    const int x0 = juce::jmax (lane.getX(), clip.getX());
    const int x1 = juce::jmin (lane.getRight(), clip.getRight());

    if (peaks.empty() || x1 <= x0 || lane.getHeight() < 4 || sampleRate <= 0.0)
        return;

    const double samplesPerPixel = span() * sampleRate / (double) juce::jmax (1, getWidth());
    const float mid = (float) lane.getCentreY();
    const float half = (float) lane.getHeight() * 0.5f - 2.0f;

    juce::RectangleList<float> columns;
    columns.ensureStorageAllocated (x1 - x0);

    for (int x = x0; x < x1; ++x)
    {
        const double first = xToTime ((float) x) * sampleRate;
        const double last = first + samplesPerPixel;

        if (last <= 0.0 || first >= (double) peaks.numSamples)
            continue;

        float lo = 0.0f, hi = 0.0f;
        peaks.read (first, last, samplesPerPixel, lo, hi);

        float top = mid - hi * half;
        float bottom = mid - lo * half;

        if (bottom - top < 1.0f)
        {
            top = (top + bottom) * 0.5f - 0.5f;
            bottom = top + 1.0f;
        }

        columns.addWithoutMerging ({ (float) x, top, 1.0f, bottom - top });
    }

    g.setColour (colour);
    g.fillRectList (columns);
}

void WaveformView::drawCentredMessage (juce::Graphics& g, const juce::String& message, juce::Rectangle<int> area) const
{
    g.setColour (Theme::dim());
    g.setFont (Theme::ui (14.0f));
    g.drawText (message, area.reduced (16, 0), juce::Justification::centred, true);
}

void WaveformView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::bg());

    const auto l = getLayout();
    const auto clip = g.getClipBounds();
    const float width = (float) getWidth();
    const float rulerBottom = (float) l.ruler.getBottom();
    const float lanesBottom = (float) l.lanes.getBottom();
    const float lanesHeight = lanesBottom - rulerBottom;

    g.setColour (Theme::line());
    g.fillRect (0, l.ruler.getBottom() - 1, getWidth(), 1);

    if (! hasAudio())
    {
        drawCentredMessage (g, busy ? ellipsised ("Listening") : juce::String ("Drop the trailer music here, or press Open"),
                            l.lanes);
        return;
    }

    // Stretches the engine bridged without hits, until the user confirms the restart after them.
    for (size_t i = 1; i < map.sections.size(); ++i)
    {
        const auto& section = map.sections[i];

        if (section.kind == tt::Section::Kind::first || section.confirmed)
            continue;

        const float xa = juce::jmax (0.0f, timeToX (map.sections[i - 1].endTime));
        const float xb = juce::jmin (width, timeToX (section.startTime));

        if (xb > xa)
        {
            g.setColour (Theme::check().withAlpha (0.14f));
            g.fillRect (juce::Rectangle<float> (xa, rulerBottom, xb - xa, lanesHeight));
        }
    }

    std::vector<GridLine> lines;
    int labelStep = 1;
    double pixelsPerBar = 0.0;
    collectGrid (lines, labelStep, pixelsPerBar);

    // The grid under the waveforms and in the ruler.
    for (const auto& line : lines)
    {
        if (line.bar)
        {
            g.setColour (Theme::line().brighter (0.35f));
            g.fillRect (juce::Rectangle<float> (line.x, 0.0f, 1.0f, lanesBottom));
        }
        else
        {
            g.setColour (Theme::line().withAlpha (0.7f));
            g.fillRect (juce::Rectangle<float> (line.x, rulerBottom - 5.0f, 1.0f, lanesHeight + 5.0f));
        }
    }

    if (! lines.empty())
    {
        const auto font = Theme::mono (11.0f);
        g.setFont (font);
        g.setColour (Theme::dim());

        // Bar numbers are thinned until the longest one in view fits between two of them.
        long long longest = 1;

        for (const auto& line : lines)
            if (line.bar)
                longest = juce::jmax (longest, line.barIndex + 1);

        const float numberWidth = std::ceil (Theme::textWidth (font, juce::String (longest)));

        while (pixelsPerBar * (double) labelStep < (double) numberWidth + 14.0 && labelStep < 4096)
            labelStep *= 2;

        const float room = (float) (pixelsPerBar * (double) labelStep) - 8.0f;

        for (const auto& line : lines)
        {
            if (! line.bar || line.barIndex < 0 || line.barIndex % (long long) labelStep != 0 || room < numberWidth)
                continue;

            g.drawText (juce::String (line.barIndex + 1),
                        juce::Rectangle<float> (line.x + 5.0f, (float) l.ruler.getY() + 1.0f, room, 13.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    drawWave (g, mixPeaks, l.music, Theme::wave(), clip);

    if (! l.drums.isEmpty())
    {
        g.setColour (Theme::line());
        g.fillRect (0, l.drums.getY(), getWidth(), 1);
        drawWave (g, drumPeaks, l.drums, Theme::drum(), clip);
    }

    // The same grid again, faintly, so it can be read through a dense waveform.
    for (const auto& line : lines)
    {
        g.setColour (Theme::bg().withAlpha (line.bar ? 0.5f : 0.28f));
        g.fillRect (juce::Rectangle<float> (line.x, rulerBottom, 1.0f, lanesHeight));
    }

    if (! l.drums.isEmpty() && l.drums.getHeight() >= 20)
    {
        g.setColour (Theme::dim());
        g.setFont (Theme::ui (11.0f));
        g.drawText ("Drums", juce::Rectangle<int> (l.drums.getX() + 6, l.drums.getY() + 4, juce::jmin (60, l.drums.getWidth() - 12), 14),
                    juce::Justification::centredLeft, true);
    }

    // What the engine heard: a tick for every hit along the bottom of the music lane.
    {
        const double from = xToTime ((float) clip.getX() - 1.0f);
        const double to = xToTime ((float) clip.getRight() + 1.0f);
        auto it = std::lower_bound (map.onsets.begin(), map.onsets.end(), from,
                                    [] (const tt::Onset& o, double t) { return o.time < t; });
        const float y = (float) l.music.getBottom() - 3.0f;

        for (; it != map.onsets.end() && it->time <= to; ++it)
        {
            g.setColour (Theme::dim().withAlpha (0.4f + 0.6f * juce::jlimit (0.0f, 1.0f, it->strength)));
            g.fillRect (juce::Rectangle<float> (std::round (timeToX (it->time)), y, 1.0f, 3.0f));
        }
    }

    // Restarts the user has confirmed.
    for (size_t i = 1; i < map.sections.size(); ++i)
    {
        const auto& section = map.sections[i];

        if (section.kind == tt::Section::Kind::first || ! section.confirmed)
            continue;

        const float x = std::round (timeToX (section.startTime)) - 3.5f;

        if (x >= -4.0f && x <= width + 4.0f)
        {
            const float dashes[] = { 4.0f, 3.0f };
            g.setColour (Theme::pin().withAlpha (0.8f));
            g.drawDashedLine ({ x, rulerBottom, x, lanesBottom }, dashes, 2, 1.0f);
        }
    }

    // Pins.
    const int shownSelected = dragging ? pressedPin : selectedPin;

    for (size_t i = 0; i < map.pins.size(); ++i)
    {
        const auto& pin = map.pins[i];
        const float x = timeToX (pinTime ((int) i));

        if (x < -12.0f || x > width + 12.0f)
            continue;

        const bool selected = (int) i == shownSelected;
        const float thickness = selected ? 2.0f : 1.5f;

        g.setColour (selected ? Theme::pin().brighter (0.7f) : Theme::pin());
        g.fillRect (juce::Rectangle<float> (x - thickness * 0.5f, rulerBottom - 1.0f, thickness, lanesHeight + 1.0f));

        if (pin.restart)
            g.fillRect (juce::Rectangle<float> (x - 3.5f, rulerBottom, 1.0f, lanesHeight));

        if (pin.user)
        {
            g.fillEllipse (juce::Rectangle<float> (x - 4.5f, rulerBottom - 10.0f, 9.0f, 9.0f));
        }
        else
        {
            juce::Path head;
            head.addTriangle (x - 5.0f, rulerBottom - 8.0f, x + 5.0f, rulerBottom - 8.0f, x, rulerBottom);
            g.fillPath (head);
        }
    }

    if (shownSelected >= 0 && shownSelected < (int) map.pins.size())
    {
        const float x = timeToX (pinTime (shownSelected));

        if (x >= -12.0f && x <= width + 12.0f)
        {
            const auto font = Theme::mono (11.0f);
            const auto label = pinLabel (map.pins[(size_t) shownSelected].beat);
            const float w = std::ceil (Theme::textWidth (font, label)) + 8.0f;
            float left = x + 8.0f;

            if (left + w > width)
                left = x - 8.0f - w;

            // Covers the ruler's height, so a bar number behind it does not show around it.
            const juce::Rectangle<float> box (juce::jmax (0.0f, left), (float) l.ruler.getY() + 1.0f, juce::jmin (w, width),
                                              juce::jmax (0.0f, (float) l.ruler.getHeight() - 3.0f));
            g.setColour (Theme::bg());
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (Theme::pin().brighter (0.7f));
            g.setFont (font);
            g.drawText (label, box, juce::Justification::centred, true);
        }
    }

    // Places to check: a thin bar over each stretch, and the notes beside its start. Notes that
    // start at the same place share one label; a label that would run into the one before it
    // goes on a second row.
    {
        struct Label
        {
            float x = 0.0f;
            juce::String text;
        };

        std::vector<Label> labels;

        for (const auto& c : map.checks)
        {
            const float xa = timeToX (c.startTime);
            const float xb = timeToX (juce::jmax (c.startTime, c.endTime));

            if (xb < 0.0f || xa > width)
                continue;

            g.setColour (Theme::check().withAlpha (0.7f));
            g.fillRect (juce::Rectangle<float> (juce::jmax (0.0f, xa), (float) l.music.getY(),
                                                juce::jmax (2.0f, juce::jmin (width, xb) - juce::jmax (0.0f, xa)), 2.0f));

            const float x = juce::jmax (0.0f, xa);
            const auto note = juce::String::fromUTF8 (c.note.c_str());
            const auto separator = juce::String::fromUTF8 ("  \xc2\xb7  ");

            if (! labels.empty() && std::abs (labels.back().x - x) < 3.0f)
            {
                // The question about the restart reads first.
                if (c.kind == tt::Check::Kind::restart)
                    labels.back().text = note + separator + labels.back().text;
                else
                    labels.back().text << separator << note;
            }
            else
            {
                labels.push_back ({ x, note });
            }
        }

        const auto font = Theme::ui (11.0f, true);
        const float rowHeight = 18.0f;
        const int rows = juce::jlimit (0, 2, (l.music.getHeight() - 6) / (int) rowHeight);
        float rowEnd[2] = { -1.0e9f, -1.0e9f };
        g.setFont (font);

        for (const auto& label : labels)
        {
            const float wanted = std::ceil (Theme::textWidth (font, label.text)) + 12.0f;
            const float w = juce::jmin (wanted, width - 8.0f);
            const float x = juce::jlimit (4.0f, juce::jmax (4.0f, width - 4.0f - w), label.x + 6.0f);

            int row = 0;
            while (row < rows && x < rowEnd[row] + 6.0f)
                ++row;

            if (row >= rows || w < 30.0f)
                continue;

            rowEnd[row] = x + w;
            const juce::Rectangle<float> plate (x, (float) l.music.getY() + 6.0f + rowHeight * (float) row, w, rowHeight - 2.0f);

            // On a plate, so the pins and the waveform behind it do not cut through the words.
            g.setColour (Theme::bg().withAlpha (0.9f));
            g.fillRoundedRectangle (plate, 3.0f);
            g.setColour (Theme::check());
            g.drawText (label.text, plate.reduced (6.0f, 0.0f), juce::Justification::centredLeft, true);
        }
    }

    // Playhead.
    if (playhead >= viewStart && playhead <= viewEnd)
    {
        g.setColour (Theme::text());
        g.fillRect (juce::Rectangle<float> (std::floor (timeToX (playhead)), 0.0f, 1.0f, lanesBottom));
    }

    if (busy)
    {
        g.setColour (Theme::bg().withAlpha (0.7f));
        g.fillRect (l.lanes);
        drawCentredMessage (g, ellipsised ("Listening"), l.lanes);
    }

    // The part of the file in view.
    if (! l.overview.isEmpty())
    {
        const auto strip = l.overview.toFloat();
        g.setColour (Theme::panel());
        g.fillRect (strip);

        const float xa = (float) (viewStart / duration) * strip.getWidth();
        const float xb = (float) (viewEnd / duration) * strip.getWidth();
        const float w = juce::jmax (3.0f, xb - xa);
        g.setColour (Theme::wave());
        g.fillRect (juce::Rectangle<float> (juce::jmin (xa, strip.getWidth() - w), strip.getY(), w, strip.getHeight()));
    }
}

void WaveformView::resized()
{
    hideBarEditor (false);
}

//==============================================================================
void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    const int now = (hasAudio() && ! busy) ? pinAt (e.position, getLayout()) : -1;

    if (now != hoverPin)
    {
        hoverPin = now;
        setMouseCursor (now >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }
}

void WaveformView::mouseExit (const juce::MouseEvent&)
{
    if (hoverPin != -1)
    {
        hoverPin = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    hideBarEditor (false);
    pressedPin = -1;
    dragging = false;

    if (! hasAudio())
        return;

    const auto l = getLayout();
    const int hit = busy ? -1 : pinAt (e.position, l);

    if (e.mods.isPopupMenu())
    {
        // The map is being rebuilt while busy, so edits wait until it is back.
        if (busy || e.position.y >= (float) l.lanes.getBottom())
            return;

        if (hit != selectedPin)
        {
            selectedPin = hit;
            repaint();
        }

        double time = juce::jlimit (0.0, duration, xToTime (e.position.x));

        if (hit >= 0)
            time = map.pins[(size_t) hit].time;
        else
            snapToOnset (time, 12.0f, 0.0, duration);

        showMenu (time, hit);
        return;
    }

    pressedPin = hit;

    if (hit != selectedPin)
    {
        selectedPin = hit;
        repaint();
    }
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (pressedPin < 0 || pressedPin >= (int) map.pins.size() || e.mods.isPopupMenu())
        return;

    if (! dragging && e.getDistanceFromDragStart() < 3)
        return;

    // A pin stays between its neighbours, so the pins keep their order.
    const auto index = (size_t) pressedPin;
    const double lowest = index > 0 ? map.pins[index - 1].time + pinSpacing : 0.0;
    const double highest = index + 1 < map.pins.size() ? map.pins[index + 1].time - pinSpacing : duration;

    if (lowest > highest)
        return;

    double time = juce::jlimit (lowest, highest, xToTime (e.position.x));

    if (! e.mods.isAltDown())
        snapToOnset (time, 10.0f, lowest, highest);

    dragging = true;
    dragTime = time;
    repaint();
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    const int pin = pressedPin;
    const bool wasDragging = dragging;
    pressedPin = -1;
    dragging = false;

    if (e.mods.isPopupMenu() || ! hasAudio())
        return;

    if (wasDragging)
    {
        repaint();

        if (pin >= 0 && pin < (int) map.pins.size() && std::abs (dragTime - map.pins[(size_t) pin].time) > 1.0e-9)
        {
            keepSelectionNear = dragTime;

            if (onPinMoved)
                onPinMoved (pin, dragTime);
        }

        return;
    }

    if (pin < 0 && e.mouseWasClicked() && e.getNumberOfClicks() == 1
        && e.position.y < (float) getLayout().lanes.getBottom() && onSeek)
    {
        onSeek (juce::jlimit (0.0, duration, xToTime (e.position.x)));
    }
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! hasAudio() || e.mods.isPopupMenu())
        return;

    const auto l = getLayout();

    if (l.ruler.contains (e.getPosition()))
    {
        showAll();
        return;
    }

    if (busy || ! l.lanes.contains (e.getPosition()) || pinAt (e.position, l) >= 0)
        return;

    double time = juce::jlimit (0.0, duration, xToTime (e.position.x));
    snapToOnset (time, 12.0f, 0.0, duration);
    keepSelectionNear = time;

    if (onPinAdded)
        onPinAdded (time);
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! hasAudio())
        return;

    const float coarse = wheel.isSmooth ? 1.0f : 4.0f;   // a wheel with notches sends fewer, larger steps

    if (e.mods.isCommandDown())
    {
        const float d = juce::exactlyEqual (wheel.deltaY, 0.0f) ? wheel.deltaX : wheel.deltaY;
        zoomAbout (e.position.x, std::exp ((double) (d * 2.0f * coarse)));
        return;
    }

    // Either direction of the wheel or trackpad scrolls along the time axis. On a trackpad
    // the content follows the fingers: JUCE scales a movement of 512 px to a delta of 1.
    const float d = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
    const double pps = pixelsPerSecond();

    if (pps > 0.0)
        scrollBy (-(double) (d * 512.0f * coarse) / pps);
}

void WaveformView::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    zoomAbout (e.position.x, (double) scaleFactor);
}

bool WaveformView::keyPressed (const juce::KeyPress& key)
{
    if (key.isKeyCode (juce::KeyPress::escapeKey) && dragging)
    {
        dragging = false;
        pressedPin = -1;
        repaint();
        return true;
    }

    if ((key.isKeyCode (juce::KeyPress::backspaceKey) || key.isKeyCode (juce::KeyPress::deleteKey))
        && ! key.getModifiers().isAnyModifierKeyDown()
        && ! busy && selectedPin >= 0 && selectedPin < (int) map.pins.size())
    {
        if (onPinDeleted)
            onPinDeleted (selectedPin);

        return true;
    }

    // Everything else (Space, Tab, letters, arrows) belongs to the window.
    return false;
}

//==============================================================================
void WaveformView::showMenu (double time, int pinIndex)
{
    juce::PopupMenu menu;
    menu.addItem (1, "The beat restarts here");
    menu.addItem (2, "This is the first hit");

    if (pinIndex >= 0)
    {
        menu.addSeparator();
        menu.addItem (3, ellipsised ("This hit is bar"));
        menu.addItem (4, "Delete pin");
    }

    juce::Component::SafePointer<WaveformView> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                        [safe, time, pinIndex] (int chosen)
                        {
                            if (safe == nullptr || chosen == 0)
                                return;

                            auto& view = *safe;

                            if (chosen == 1 || chosen == 2)
                            {
                                view.keepSelectionNear = time;
                                auto& callback = chosen == 1 ? view.onRestartHere : view.onFirstHitHere;

                                if (callback)
                                    callback (time);

                                return;
                            }

                            // The map may have been rebuilt while the menu was open.
                            if (pinIndex < 0 || pinIndex >= (int) view.map.pins.size()
                                || std::abs (view.map.pins[(size_t) pinIndex].time - time) > 1.0e-6)
                                return;

                            if (chosen == 3)
                            {
                                // Shown once the menu has finished closing: the menu hands the
                                // keyboard focus back to the view as it goes, and the editor
                                // closes when it loses the focus.
                                juce::MessageManager::callAsync ([safe, time, pinIndex]
                                {
                                    if (safe != nullptr && pinIndex < (int) safe->map.pins.size()
                                        && std::abs (safe->map.pins[(size_t) pinIndex].time - time) <= 1.0e-6)
                                        safe->showBarEditor (pinIndex);
                                });
                            }
                            else if (chosen == 4 && view.onPinDeleted)
                                view.onPinDeleted (pinIndex);
                        });
}

void WaveformView::showBarEditor (int pinIndex)
{
    if (pinIndex < 0 || pinIndex >= (int) map.pins.size())
        return;

    hideBarEditor (false);

    const auto l = getLayout();
    const int w = 92, h = 24;
    const int pinX = juce::roundToInt (timeToX (map.pins[(size_t) pinIndex].time));
    int x = pinX + 8;

    if (x + w > getWidth() - 2)
        x = pinX - 8 - w;

    x = juce::jlimit (2, juce::jmax (2, getWidth() - w - 2), x);

    barEditor.setColour (juce::TextEditor::outlineColourId, Theme::pin());
    barEditor.setColour (juce::TextEditor::focusedOutlineColourId, Theme::pin());
    barEditor.setBounds (x, l.ruler.getBottom() + 4, juce::jmin (w, getWidth() - 4), h);
    barEditor.setText (pinLabel (map.pins[(size_t) pinIndex].beat), false);

    barEditorPin = pinIndex;
    selectedPin = pinIndex;
    barEditor.setVisible (true);
    barEditor.grabKeyboardFocus();
    barEditor.selectAll();
    repaint();
}

void WaveformView::hideBarEditor (bool commit)
{
    if (barEditorPin < 0)
        return;

    const int pin = barEditorPin;
    double beat = 0.0;

    if (commit && ! parseBarBeat (barEditor.getText(), beatsPerBar, beat))
    {
        // Not a bar and beat: the editor stays open and its outline says so.
        barEditor.setColour (juce::TextEditor::outlineColourId, Theme::check());
        barEditor.setColour (juce::TextEditor::focusedOutlineColourId, Theme::check());
        barEditor.repaint();
        return;
    }

    // Cleared first: hiding the editor takes its focus away, which calls back into here.
    barEditorPin = -1;
    const bool hadFocus = barEditor.hasKeyboardFocus (true);
    barEditor.setVisible (false);

    if (hadFocus && isShowing())
        grabKeyboardFocus();

    if (commit && pin < (int) map.pins.size())
    {
        keepSelectionNear = map.pins[(size_t) pin].time;

        if (onPinBeatChanged)
            onPinBeatChanged (pin, beat);
    }
}

} // namespace tt::ui

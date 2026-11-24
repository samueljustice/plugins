#include "WaveformView.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tt::ui
{

namespace
{
    constexpr int rulerHeight = 38;             // bar numbers, the tempo between pins, the pin heads
    constexpr int overviewHeight = 14;
    constexpr float pinGrabPixels = 6.0f;       // either side of a pin
    constexpr float snapPixels = 12.0f;         // how near a hit must be for a pin to snap to it
    constexpr double shortestSpan = 0.05;       // seconds: the closest zoom
    constexpr double pinSpacing = 0.005;        // seconds kept between a moved pin and its neighbours
    constexpr double glideMs = 120.0;           // how long a zoom from the keyboard takes
    constexpr double detailPixelsPerBeat = 60.0;  // from here in, the waveform is drawn as a shape and the hits as ticks
    constexpr double lowestWaveHeight = 0.25, highestWaveHeight = 64.0;

    // "1:23.456"
    juce::String clockText (double seconds)
    {
        const auto ms = (juce::int64) std::llround (juce::jmax (0.0, seconds) * 1000.0);
        return juce::String (ms / 60000) + ":" + juce::String ((ms / 1000) % 60).paddedLeft ('0', 2)
             + "." + juce::String (ms % 1000).paddedLeft ('0', 3);
    }

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
    std::vector<float> lo (buckets), hi (buckets), rms (buckets);
    const float gain = 1.0f / (float) channels;
    float peak = 0.0f;

    for (size_t b = 0; b < buckets; ++b)
    {
        const int start = (int) b * base;
        const int end = juce::jmin (n, start + base);
        float mn = std::numeric_limits<float>::max();
        float mx = std::numeric_limits<float>::lowest();
        double squares = 0.0;

        for (int i = start; i < end; ++i)
        {
            float s = 0.0f;

            for (int c = 0; c < channels; ++c)
                s += buffer.getReadPointer (c)[i];

            s *= gain;
            mn = juce::jmin (mn, s);
            mx = juce::jmax (mx, s);
            squares += (double) s * (double) s;
        }

        lo[b] = mn;
        hi[b] = mx;
        rms[b] = (float) std::sqrt (squares / (double) juce::jmax (1, end - start));
        peak = juce::jmax (peak, -mn, mx);
    }

    // Stored so the loudest sample fills the lane: quiet stems stay readable.
    const float scale = 32767.0f / (peak > 1.0e-4f ? peak : 1.0f);

    Level first;
    first.samplesPerBucket = base;
    first.lo.resize (buckets);
    first.hi.resize (buckets);
    first.rms.resize (buckets);

    for (size_t b = 0; b < buckets; ++b)
    {
        first.lo[b] = (std::int16_t) juce::jlimit (-32767.0f, 32767.0f, lo[b] * scale);
        first.hi[b] = (std::int16_t) juce::jlimit (-32767.0f, 32767.0f, hi[b] * scale);
        first.rms[b] = (std::uint16_t) juce::jlimit (0.0f, 32767.0f, rms[b] * scale);
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
        coarse.rms.resize (count);

        for (size_t b = 0; b < count; ++b)
        {
            const size_t start = b * 8;
            const size_t end = juce::jmin (fine.lo.size(), start + 8);
            std::int16_t mn = fine.lo[start], mx = fine.hi[start];
            double squares = (double) fine.rms[start] * (double) fine.rms[start];

            for (size_t i = start + 1; i < end; ++i)
            {
                mn = juce::jmin (mn, fine.lo[i]);
                mx = juce::jmax (mx, fine.hi[i]);
                squares += (double) fine.rms[i] * (double) fine.rms[i];
            }

            coarse.lo[b] = mn;
            coarse.hi[b] = mx;
            coarse.rms[b] = (std::uint16_t) std::sqrt (squares / (double) (end - start));
        }

        levels.push_back (std::move (coarse));
    }
}

void WaveformView::PeakCache::read (double firstSample, double lastSample, double samplesPerPixel,
                                    float& lo, float& hi, float* rms) const
{
    lo = hi = 0.0f;

    if (rms != nullptr)
        *rms = 0.0f;

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
    double squares = 0.0;

    for (auto b = b0; b < b1; ++b)
    {
        mn = juce::jmin (mn, level->lo[(size_t) b]);
        mx = juce::jmax (mx, level->hi[(size_t) b]);
        squares += (double) level->rms[(size_t) b] * (double) level->rms[(size_t) b];
    }

    lo = (float) mn / 32767.0f;
    hi = (float) mx / 32767.0f;

    if (rms != nullptr)
        *rms = (float) (std::sqrt (squares / (double) (b1 - b0)) / 32767.0);
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
    gesture = Gesture::none;
    gliding = false;

    if (std::abs (duration - oldDuration) > 1.0e-6)
        zoomToggled = false;

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
    nudgedTo = -1.0;

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
    const auto l = getLayout();

    if (seconds >= viewStart && seconds <= viewEnd)
        repaint ((int) std::floor (timeToX (seconds)) - 2, 0, 5, l.lanes.getBottom());

    // Its mark in the overview, where the whole file is as wide as the view.
    if (duration > 0.0)
        repaint ((int) std::floor (seconds / duration * (double) getWidth()) - 2, l.overview.getY(), 5, l.overview.getHeight());
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

    nudgedTo = -1.0;

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

    gliding = false;

    const double length = juce::jlimit (juce::jmin (shortestSpan, duration), duration, end - start);
    start = juce::jlimit (0.0, duration - length, start);
    applyView (start, start + length);
}

void WaveformView::applyView (double start, double end)
{
    viewStart = start;
    viewEnd = end;
    hideBarEditor (false);
    repaint();
}

juce::Range<double> WaveformView::targetView() const
{
    return gliding ? juce::Range<double> (glideToStart, glideToEnd) : juce::Range<double> (viewStart, viewEnd);
}

void WaveformView::glideTo (double start, double end)
{
    if (duration <= 0.0)
        return;

    // With nothing on screen to glide, or with macOS set to reduce motion, the view just changes.
    if (! isShowing() || Theme::reduceMotion() || span() <= 0.0)
    {
        setView (start, end);
        return;
    }

    const double length = juce::jlimit (juce::jmin (shortestSpan, duration), duration, end - start);
    start = juce::jlimit (0.0, duration - length, start);

    glideFromStart = viewStart;
    glideFromEnd = viewEnd;
    glideToStart = start;
    glideToEnd = start + length;
    glideBegan = juce::Time::getMillisecondCounterHiRes();
    gliding = true;
    startTimerHz (60);
}

void WaveformView::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();

    if (gliding)
    {
        const double t = (now - glideBegan) / glideMs;

        if (t >= 1.0)
        {
            gliding = false;
            applyView (glideToStart, glideToEnd);
        }
        else
        {
            // Fast at first, slowing into place.
            const double eased = 1.0 - std::pow (1.0 - juce::jmax (0.0, t), 3.0);
            applyView (glideFromStart + (glideToStart - glideFromStart) * eased,
                       glideFromEnd + (glideToEnd - glideFromEnd) * eased);
        }
    }

    if (! gliding && now >= waveHeightShownUntil)
    {
        stopTimer();
        repaint();   // takes the waveform height off the lane
    }
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

double WaveformView::anchorTime (bool mustBeInView, bool& found) const
{
    const auto view = targetView();
    const auto usable = [&] (double t) { return ! mustBeInView || (t >= view.getStart() && t <= view.getEnd()); };
    found = true;

    if (selectedPin >= 0 && selectedPin < (int) map.pins.size() && usable (map.pins[(size_t) selectedPin].time))
        return map.pins[(size_t) selectedPin].time;

    if (usable (playhead))
        return playhead;

    found = false;
    return 0.5 * (view.getStart() + view.getEnd());
}

void WaveformView::zoomBy (double factor)
{
    if (! hasAudio() || factor <= 0.0)
        return;

    bool found = false;
    const double anchor = anchorTime (true, found);
    const double length = juce::jlimit (juce::jmin (shortestSpan, duration), duration, targetView().getLength() / factor);
    glideTo (anchor - length * 0.5, anchor + length * 0.5);
}

void WaveformView::showAllAnimated()
{
    if (hasAudio())
        glideTo (0.0, duration);
}

void WaveformView::zoomToPinsAround()
{
    if (! hasAudio() || map.pins.empty())
        return;

    const auto& pins = map.pins;
    const double bar = 60.0 / juce::jmax (1.0, grid.bpmAtTime (playhead)) * (double) beatsPerBar;
    double start = 0.0, end = duration;

    if (selectedPin >= 0 && selectedPin < (int) pins.size())
    {
        // The first and last pins have no pin on one side: a bar is shown there.
        const auto i = (size_t) selectedPin;
        start = i > 0 ? pins[i - 1].time : pins[i].time - bar;
        end = i + 1 < pins.size() ? pins[i + 1].time : pins[i].time + bar;
    }
    else
    {
        // The pins either side of the playhead, or the start or end of the file.
        for (const auto& pin : pins)
        {
            if (pin.time <= playhead)
                start = pin.time;
            else
            {
                end = pin.time;
                break;
            }
        }
    }

    const double margin = (end - start) * 0.04;
    glideTo (start - margin, end + margin);
}

void WaveformView::toggleZoom()
{
    if (! hasAudio())
        return;

    if (zoomToggled)
    {
        zoomToggled = false;
        glideTo (beforeToggleStart, beforeToggleEnd);
        return;
    }

    const auto before = targetView();
    beforeToggleStart = before.getStart();
    beforeToggleEnd = before.getEnd();
    zoomToggled = true;

    bool found = false;
    const double anchor = anchorTime (false, found);

    if (map.pins.empty())
    {
        glideTo (anchor - 2.0, anchor + 2.0);
        return;
    }

    // One bar either side.
    const double beat = grid.beatAtTime (anchor);
    glideTo (grid.timeAtBeat (beat - (double) beatsPerBar), grid.timeAtBeat (beat + (double) beatsPerBar));
}

void WaveformView::reveal (double seconds)
{
    if (! hasAudio())
        return;

    const auto view = targetView();

    if (seconds >= view.getStart() && seconds <= view.getEnd())
        return;

    glideTo (seconds - view.getLength() * 0.5, seconds + view.getLength() * 0.5);
}

void WaveformView::setWaveHeight (double factor)
{
    waveHeight = juce::jlimit (lowestWaveHeight, highestWaveHeight, factor);
    repaint();
}

void WaveformView::stepWaveHeight (int steps)
{
    // Whole steps of the square root of two from 1, so two steps are exactly twice as tall.
    const double step = std::round (std::log2 (waveHeight) * 2.0) + (double) steps;
    waveHeight = juce::jlimit (lowestWaveHeight, highestWaveHeight, std::pow (2.0, step * 0.5));
    waveHeightShownUntil = juce::Time::getMillisecondCounterHiRes() + 1200.0;
    startTimerHz (60);
    repaint();
}

//==============================================================================
double WaveformView::pinTime (int index) const
{
    if (dragging && index == pressedPin)
        return dragTime;

    return map.pins[(size_t) index].time;
}

juce::Range<double> WaveformView::pinLimits (int index) const
{
    const auto i = (size_t) index;
    const double lowest = i > 0 ? map.pins[i - 1].time + pinSpacing : 0.0;
    const double highest = i + 1 < map.pins.size() ? map.pins[i + 1].time - pinSpacing : duration;
    return { lowest, juce::jmax (lowest, highest) };
}

double WaveformView::bpmAfterPin (int index) const
{
    const auto i = (size_t) index;

    if (index < 0 || i >= map.pins.size())
        return 0.0;

    // After the last pin the grid carries on at the tempo it arrived with.
    if (i + 1 >= map.pins.size())
        return grid.bpmAtTime (map.pins[i].time + 0.001);

    const double seconds = pinTime (index + 1) - pinTime (index);
    return seconds > 1.0e-6 ? 60.0 * (map.pins[i + 1].beat - map.pins[i].beat) / seconds : 0.0;
}

double WaveformView::pixelsPerBeatAt (double seconds) const
{
    if (map.pins.empty())
        return 0.0;

    const double bpm = grid.bpmAtTime (seconds);
    return bpm > 1.0 && bpm < 2000.0 ? 60.0 / bpm * pixelsPerSecond() : 0.0;
}

bool WaveformView::nudgeSelectedPin (double seconds)
{
    if (busy || dragging || selectedPin < 0 || selectedPin >= (int) map.pins.size())
        return false;

    // Until the map comes back with the pin in its new place, the next nudge starts from where
    // the last one put it.
    const double from = nudgedTo >= 0.0 ? nudgedTo : map.pins[(size_t) selectedPin].time;
    const auto limits = pinLimits (selectedPin);
    const double to = limits.clipValue (from + seconds);

    if (std::abs (to - from) < 1.0e-12)
        return true;

    const int pin = selectedPin;
    nudgedTo = to;
    keepSelectionNear = to;

    if (onPinNudged)
        onPinNudged (pin, to);

    return true;
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

bool WaveformView::snapToGrid (double& time, double lowest, double highest) const
{
    if (map.pins.empty())
        return false;

    // Beats, and eighth notes once they are far enough apart to tell from one another.
    const double step = pixelsPerBeatAt (time) * 0.5 > (double) snapPixels ? 0.5 : 1.0;
    const double nearest = std::round (grid.beatAtTime (time) / step) * step;

    // The nearest line that is still between the neighbouring pins.
    for (const double beat : { nearest, nearest + step, nearest - step })
    {
        const double t = grid.timeAtBeat (beat);

        if (std::isfinite (t) && t >= lowest && t <= highest)
        {
            time = t;
            return true;
        }
    }

    return false;
}

WaveformView::SnapKind WaveformView::snapTime (double& time, bool free, double lowest, double highest) const
{
    if (free || snap == Snap::off)
        return SnapKind::none;

    if (snap == Snap::hits)
        return snapToOnset (time, snapPixels, lowest, highest) ? SnapKind::hit : SnapKind::none;

    return snapToGrid (time, lowest, highest) ? SnapKind::grid : SnapKind::none;
}

void WaveformView::showDragForSnapshot (int pinIndex, double time)
{
    if (pinIndex < 0 || pinIndex >= (int) map.pins.size())
        return;

    const auto limits = pinLimits (pinIndex);
    time = limits.clipValue (time);
    dragSnap = snapTime (time, false, limits.getStart(), limits.getEnd());
    pressedPin = pinIndex;
    dragging = true;
    dragTime = time;
    repaint();
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
    const float gain = (float) waveHeight;

    // Two tones: the peaks dimmer, and the RMS level over them at full strength, so the hits
    // stand out of a loud, dense mix.
    juce::RectangleList<float> columns, bodies;
    columns.ensureStorageAllocated (x1 - x0);
    bodies.ensureStorageAllocated (x1 - x0);

    for (int x = x0; x < x1; ++x)
    {
        const double first = xToTime ((float) x) * sampleRate;
        const double last = first + samplesPerPixel;

        if (last <= 0.0 || first >= (double) peaks.numSamples)
            continue;

        float lo = 0.0f, hi = 0.0f, rms = 0.0f;
        peaks.read (first, last, samplesPerPixel, lo, hi, &rms);

        float top = juce::jmax (mid - half, mid - hi * gain * half);
        float bottom = juce::jmin (mid + half, mid - lo * gain * half);

        if (bottom - top < 1.0f)
        {
            top = (top + bottom) * 0.5f - 0.5f;
            bottom = top + 1.0f;
        }

        columns.addWithoutMerging ({ (float) x, top, 1.0f, bottom - top });

        const float body = juce::jmin (half, rms * gain * half);
        bodies.addWithoutMerging ({ (float) x, mid - juce::jmax (0.5f, body), 1.0f, juce::jmax (1.0f, body * 2.0f) });
    }

    g.setColour (colour.withAlpha (0.45f));
    g.fillRectList (columns);
    g.setColour (colour);
    g.fillRectList (bodies);
}

// The waveform as one filled shape with its outline, for when the view is close enough to see
// the shape of a single hit.
void WaveformView::drawWaveShape (juce::Graphics& g, const PeakCache& peaks, juce::Rectangle<int> lane,
                                  juce::Colour colour, juce::Rectangle<int> clip) const
{
    // A little beyond what is being repainted, so the outline joins up across repaints.
    const int x0 = juce::jmax (lane.getX(), clip.getX()) - 3;
    const int x1 = juce::jmin (lane.getRight(), clip.getRight()) + 3;

    if (peaks.empty() || x1 <= x0 || lane.getHeight() < 4 || sampleRate <= 0.0)
        return;

    const double samplesPerPixel = span() * sampleRate / (double) juce::jmax (1, getWidth());
    const float mid = (float) lane.getCentreY();
    const float half = (float) lane.getHeight() * 0.5f - 2.0f;
    const float gain = (float) waveHeight;

    std::vector<juce::Point<float>> upper, lower;
    const auto add = [&] (float x, float lo, float hi)
    {
        upper.emplace_back (x, juce::jmax (mid - half, mid - hi * gain * half));
        lower.emplace_back (x, juce::jmin (mid + half, mid - lo * gain * half));
    };

    const auto& finest = peaks.levels.front();
    const double perBucket = (double) finest.samplesPerBucket;

    if (samplesPerPixel >= perBucket)
    {
        // One point for each pixel column.
        for (int x = x0; x <= x1; ++x)
        {
            const double first = xToTime ((float) x) * sampleRate;
            const double last = first + samplesPerPixel;

            if (last <= 0.0 || first >= (double) peaks.numSamples)
                continue;

            float lo = 0.0f, hi = 0.0f;
            peaks.read (first, last, samplesPerPixel, lo, hi);
            add ((float) x + 0.5f, lo, hi);
        }
    }
    else
    {
        // Closer than the finest peaks: one point for each of them, joined by straight lines.
        const auto size = (juce::int64) finest.lo.size();
        const auto b0 = juce::jlimit ((juce::int64) 0, size, (juce::int64) std::floor (xToTime ((float) x0) * sampleRate / perBucket));
        const auto b1 = juce::jlimit ((juce::int64) 0, size, (juce::int64) std::ceil (xToTime ((float) x1) * sampleRate / perBucket) + 1);

        for (auto b = b0; b < b1; ++b)
            add (timeToX (((double) b + 0.5) * perBucket / sampleRate),
                 (float) finest.lo[(size_t) b] / 32767.0f, (float) finest.hi[(size_t) b] / 32767.0f);
    }

    if (upper.size() < 2)
        return;

    juce::Path shape;
    shape.startNewSubPath (upper.front());

    for (size_t i = 1; i < upper.size(); ++i)
        shape.lineTo (upper[i]);

    for (size_t i = lower.size(); i-- > 0;)
        shape.lineTo (lower[i]);

    shape.closeSubPath();

    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (lane);
    g.setColour (colour.withAlpha (0.42f));
    g.fillPath (shape);
    g.setColour (colour);
    g.strokePath (shape, juce::PathStrokeType (1.0f));
}

// The whole file in miniature along the bottom: its waveform, the pins, the playhead, and a box
// round the part that is in view.
void WaveformView::drawOverview (juce::Graphics& g, juce::Rectangle<int> strip) const
{
    if (strip.isEmpty() || duration <= 0.0)
        return;

    const auto clip = g.getClipBounds().getIntersection (strip);

    if (clip.isEmpty())
        return;

    g.setColour (Theme::panel());
    g.fillRect (clip);

    const float width = (float) strip.getWidth();
    const float mid = (float) strip.getCentreY();
    const float half = (float) strip.getHeight() * 0.5f - 1.0f;
    const double samplesPerPixel = (double) mixPeaks.numSamples / (double) juce::jmax (1, strip.getWidth());

    juce::RectangleList<float> columns;

    for (int x = clip.getX(); x < clip.getRight(); ++x)
    {
        float lo = 0.0f, hi = 0.0f;
        const double first = (double) (x - strip.getX()) * samplesPerPixel;
        mixPeaks.read (first, first + samplesPerPixel, samplesPerPixel, lo, hi);
        const float top = mid - hi * half;
        columns.addWithoutMerging ({ (float) x, top, 1.0f, juce::jmax (1.0f, mid - lo * half - top) });
    }

    g.setColour (Theme::wave().withAlpha (0.55f));
    g.fillRectList (columns);

    const auto xOf = [&] (double seconds) { return (float) strip.getX() + (float) (seconds / duration) * width; };

    g.setColour (Theme::pin());

    for (size_t i = 0; i < map.pins.size(); ++i)
        g.fillRect (juce::Rectangle<float> (std::floor (xOf (pinTime ((int) i))), (float) strip.getY(), 1.0f, 4.0f));

    g.setColour (Theme::text());
    g.fillRect (juce::Rectangle<float> (std::floor (xOf (playhead)), (float) strip.getY(), 1.0f, (float) strip.getHeight()));

    const float xa = xOf (viewStart);
    const float w = juce::jmax (4.0f, xOf (viewEnd) - xa);
    const juce::Rectangle<float> box (juce::jmin (xa, (float) strip.getRight() - w), (float) strip.getY(), w, (float) strip.getHeight());
    g.setColour (Theme::text().withAlpha (0.12f));
    g.fillRect (box);
    g.setColour (Theme::text().withAlpha (0.8f));
    g.drawRect (box, 1.0f);
}

// Beside a pin that is being dragged: where it is, and how far it has come. When it is sitting
// on a hit or a grid line, a mark on that hit or line.
void WaveformView::drawDragReadout (juce::Graphics& g, const Layout& l) const
{
    if (! dragging || pressedPin < 0 || pressedPin >= (int) map.pins.size())
        return;

    const float x = timeToX (dragTime);
    const float width = (float) getWidth();

    if (dragSnap != SnapKind::none)
    {
        // A hit is marked where the hits are drawn, a grid line just under the ruler.
        const float y = dragSnap == SnapKind::hit ? (float) l.music.getBottom() - 9.0f : (float) l.ruler.getBottom() + 8.0f;
        juce::Path mark;
        mark.addQuadrilateral (x, y - 5.0f, x + 5.0f, y, x, y + 5.0f, x - 5.0f, y);
        g.setColour (Theme::bg());
        g.fillPath (mark);
        g.setColour (Theme::pin().brighter (0.7f));
        g.strokePath (mark, juce::PathStrokeType (1.5f));
    }

    const double movedMs = (dragTime - map.pins[(size_t) pressedPin].time) * 1000.0;
    const auto text = clockText (dragTime) + "   " + (movedMs < 0.0 ? "-" : "+") + juce::String (std::abs (movedMs), 1) + " ms";
    const auto font = Theme::mono (11.0f);
    const float w = juce::jmin (width - 4.0f, std::ceil (Theme::textWidth (font, text)) + 14.0f);
    float left = x + 10.0f;

    if (left + w > width - 2.0f)
        left = x - 10.0f - w;

    const float y = (float) l.music.getY() + (float) juce::jlimit (4, 46, l.music.getHeight() - 26);
    const juce::Rectangle<float> plate (juce::jmax (2.0f, left), y, w, 20.0f);
    g.setColour (Theme::bg().withAlpha (0.92f));
    g.fillRoundedRectangle (plate, 4.0f);
    g.setColour (Theme::pin().withAlpha (0.7f));
    g.drawRoundedRectangle (plate.reduced (0.5f), 4.0f, 1.0f);
    g.setColour (Theme::text());
    g.setFont (font);
    g.drawText (text, plate, juce::Justification::centred, false);
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

    // Close in, the waveform is a shape with an outline and every hit is a tick.
    const bool detail = pixelsPerBeatAt (0.5 * (viewStart + viewEnd)) >= detailPixelsPerBeat;
    const auto wave = [&] (const PeakCache& peaks, juce::Rectangle<int> lane, juce::Colour colour)
    {
        if (detail)
            drawWaveShape (g, peaks, lane, colour, clip);
        else
            drawWave (g, peaks, lane, colour, clip);
    };

    wave (mixPeaks, l.music, Theme::wave());

    if (! l.drums.isEmpty())
    {
        g.setColour (Theme::line());
        g.fillRect (0, l.drums.getY(), getWidth(), 1);
        wave (drumPeaks, l.drums, Theme::drum());
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

    // What the engine heard: a tick for every hit along the bottom of the music lane. Close in,
    // a stronger hit has a taller tick, so it can be seen what a pin would snap to.
    {
        const double from = xToTime ((float) clip.getX() - 2.0f);
        const double to = xToTime ((float) clip.getRight() + 2.0f);
        auto it = std::lower_bound (map.onsets.begin(), map.onsets.end(), from,
                                    [] (const tt::Onset& o, double t) { return o.time < t; });
        const float bottom = (float) l.music.getBottom();
        const float tallest = juce::jmin (30.0f, (float) l.music.getHeight() * 0.25f);

        for (; it != map.onsets.end() && it->time <= to; ++it)
        {
            const float strength = juce::jlimit (0.0f, 1.0f, it->strength);
            const float x = std::round (timeToX (it->time));

            if (detail)
            {
                const float h = 5.0f + (tallest - 5.0f) * strength;
                g.setColour (Theme::bg().withAlpha (0.6f));
                g.fillRect (juce::Rectangle<float> (x - 2.0f, bottom - h - 1.0f, 4.0f, h + 1.0f));
                g.setColour (Theme::text().withAlpha (0.6f + 0.4f * strength));
                g.fillRect (juce::Rectangle<float> (x - 1.0f, bottom - h, 2.0f, h));
            }
            else
            {
                g.setColour (Theme::dim().withAlpha (0.4f + 0.6f * strength));
                g.fillRect (juce::Rectangle<float> (x, bottom - 3.0f, 1.0f, 3.0f));
            }
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

    const int shownSelected = dragging ? pressedPin : selectedPin;

    // The tempo of each stretch between two pins, in the ruler beside the pin it starts at. It
    // stays at the left edge while its pin is out of view to the left.
    {
        const auto font = Theme::mono (10.0f);
        const float y = (float) l.ruler.getY() + 14.0f;
        g.setFont (font);

        for (size_t i = 0; i < map.pins.size(); ++i)
        {
            if ((int) i == shownSelected)
                continue;   // on the selected pin's own label

            const float xa = timeToX (pinTime ((int) i));
            const float xb = i + 1 < map.pins.size() ? timeToX (pinTime ((int) i + 1)) : width + 100.0f;

            if (xb < 0.0f || xa > width)
                continue;

            const double bpm = bpmAfterPin ((int) i);

            if (! (bpm > 0.0 && bpm < 2000.0))
                continue;

            const auto text = juce::String (bpm, 2);
            const float w = std::ceil (Theme::textWidth (font, text));
            const float x = juce::jmax (xa + 8.0f, 4.0f);

            if (x + w > xb - 7.0f || x + w > width - 2.0f)
                continue;

            g.setColour (Theme::bg());
            g.fillRect (juce::Rectangle<float> (x - 2.0f, y, w + 4.0f, 12.0f));
            g.setColour (Theme::dim());
            g.drawText (text, juce::Rectangle<float> (x, y, w, 12.0f), juce::Justification::centredLeft, false);
        }
    }

    // Pins.

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
            // Its bar and beat, and under that the tempo from it to the next pin.
            const auto font = Theme::mono (11.0f);
            const auto label = pinLabel (map.pins[(size_t) shownSelected].beat);
            const double bpm = bpmAfterPin (shownSelected);
            const auto tempo = bpm > 0.0 && bpm < 2000.0 ? juce::String (bpm, 2) : juce::String();
            const float w = std::ceil (juce::jmax (Theme::textWidth (font, label), Theme::textWidth (font, tempo))) + 10.0f;
            float left = x + 4.0f;

            if (left + w > width)
                left = x - 4.0f - w;

            // Covers the ruler's two rows of text, so what is behind it does not show around it.
            const juce::Rectangle<float> box (juce::jmax (0.0f, left), (float) l.ruler.getY() + 1.0f, juce::jmin (w, width), 26.0f);
            g.setColour (Theme::bg());
            g.fillRoundedRectangle (box, 3.0f);
            g.setFont (font);
            g.setColour (Theme::pin().brighter (0.7f));
            g.drawText (label, box.withHeight (13.0f), juce::Justification::centred, true);
            g.setColour (Theme::text());
            g.drawText (tempo, box.withTrimmedTop (13.0f), juce::Justification::centred, true);
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

    drawDragReadout (g, l);

    // Playhead.
    if (playhead >= viewStart && playhead <= viewEnd)
    {
        g.setColour (Theme::text());
        g.fillRect (juce::Rectangle<float> (std::floor (timeToX (playhead)), 0.0f, 1.0f, lanesBottom));
    }

    // How much taller the waveform is drawn, for a moment after it is changed.
    if (juce::Time::getMillisecondCounterHiRes() < waveHeightShownUntil && l.music.getHeight() >= 40)
    {
        const auto font = Theme::mono (11.0f);
        const auto text = "Waveform height " + juce::String::fromUTF8 ("\xc3\x97")
                        + juce::String (waveHeight, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
        const float w = juce::jmin (width - 16.0f, std::ceil (Theme::textWidth (font, text)) + 16.0f);
        const juce::Rectangle<float> plate (width - w - 8.0f, lanesBottom - 30.0f, w, 22.0f);
        g.setColour (Theme::bg().withAlpha (0.92f));
        g.fillRoundedRectangle (plate, 4.0f);
        g.setColour (Theme::text());
        g.setFont (font);
        g.drawText (text, plate, juce::Justification::centred, true);
    }

    if (busy)
    {
        g.setColour (Theme::bg().withAlpha (0.7f));
        g.fillRect (l.lanes);
        drawCentredMessage (g, ellipsised ("Listening"), l.lanes);
    }

    drawOverview (g, l.overview);
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

void WaveformView::moveViewFromOverview (float x)
{
    const double at = (double) x / (double) juce::jmax (1, getWidth()) * duration;
    const double length = span();
    setView (at - overviewGrabOffset, at - overviewGrabOffset + length);
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    hideBarEditor (false);
    pressedPin = -1;
    dragging = false;
    dragSnap = SnapKind::none;
    gesture = Gesture::none;
    nudgedTo = -1.0;

    if (! hasAudio())
        return;

    const auto l = getLayout();

    // In the overview: the box round the part in view is dragged from where it was taken hold
    // of. A press outside the box first centres the view there.
    if (e.position.y >= (float) l.overview.getY())
    {
        if (e.mods.isPopupMenu())
            return;

        const double at = (double) e.position.x / (double) juce::jmax (1, getWidth()) * duration;
        overviewGrabOffset = at >= viewStart && at <= viewEnd ? at - viewStart : span() * 0.5;
        gesture = Gesture::overview;
        moveViewFromOverview (e.position.x);
        return;
    }

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
            snapToOnset (time, snapPixels, 0.0, duration);

        showMenu (time, hit);
        return;
    }

    // In the ruler, away from the pin heads: the playhead follows the pointer.
    if (hit < 0 && l.ruler.contains (e.getPosition()))
    {
        gesture = Gesture::scrub;

        if (onSeek)
            onSeek (juce::jlimit (0.0, duration, xToTime (e.position.x)));

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
    if (! hasAudio())
        return;

    if (gesture == Gesture::overview)
    {
        moveViewFromOverview (e.position.x);
        return;
    }

    if (gesture == Gesture::scrub)
    {
        if (onSeek)
            onSeek (juce::jlimit (viewStart, viewEnd, xToTime (e.position.x)));

        return;
    }

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

    // Snapped as the Snap setting says, to the hits or to the grid as it stood when the drag
    // began (the map does not change until the pin is let go). Cmd places it freely.
    double time = juce::jlimit (lowest, highest, xToTime (e.position.x));
    dragSnap = snapTime (time, e.mods.isCommandDown(), lowest, highest);

    dragging = true;
    dragTime = time;
    repaint();
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    const int pin = pressedPin;
    const bool wasDragging = dragging;
    const auto finished = gesture;
    pressedPin = -1;
    dragging = false;
    gesture = Gesture::none;

    if (e.mods.isPopupMenu() || ! hasAudio() || finished != Gesture::none)
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

    if (l.ruler.contains (e.getPosition()) && pinAt (e.position, l) < 0)
    {
        showAllAnimated();
        return;
    }

    if (busy || ! l.lanes.contains (e.getPosition()) || pinAt (e.position, l) >= 0)
        return;

    double time = juce::jlimit (0.0, duration, xToTime (e.position.x));
    snapTime (time, e.mods.isCommandDown(), 0.0, duration);
    keepSelectionNear = time;

    if (onPinAdded)
        onPinAdded (time);
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! hasAudio())
        return;

    const float coarse = wheel.isSmooth ? 1.0f : 4.0f;   // a wheel with notches sends fewer, larger steps

    // Option, as in Pro Tools, or Cmd: zoom about the pointer.
    if (e.mods.isAltDown() || e.mods.isCommandDown())
    {
        const float d = juce::exactlyEqual (wheel.deltaY, 0.0f) ? wheel.deltaX : wheel.deltaY;
        zoomAbout (e.position.x, std::exp ((double) (d * 2.0f * coarse)));
        return;
    }

    // Either direction of the wheel or trackpad scrolls along the time axis (so a wheel that only
    // turns one way does too, with or without Shift). On a trackpad
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
        dragSnap = SnapKind::none;
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

    // Everything else (Space, Tab, letters, arrows) belongs to the window, which calls back
    // here for the keys that move the view.
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

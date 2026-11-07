#include "HarmonicStrip.h"

#include <cmath>

namespace
{
    constexpr float c_clickTolerance = 3.0f;    // screen px of movement that still counts as a click
    constexpr float c_fineScale = 0.1f;         // shift-drag: a full bar height moves the level 10 %
    constexpr float c_wheelStep = 0.05f;        // 5 % of the range per wheel notch
    constexpr float c_trackpadNotch = 0.05f;    // smooth-scroll delta treated as one notch (~25 px on macOS)
    constexpr juce::int64 c_trackpadPauseMs = 250; // a longer gap between smooth-scroll events starts a new scroll
    constexpr juce::int64 c_wheelBurstMs = 500; // wheel events closer than this are one scroll: one host gesture

    // JUCE's delta for one notch of a wheel that does not scroll smoothly. On Windows a notch is
    // WHEEL_DELTA (120) and reaches JUCE as 0.5 * 120 / 256; high-resolution wheels and precision
    // touchpads send a fraction of that per (non-smooth) event, so those deltas are summed and
    // every whole notch is one step, as Windows itself does. On Linux every wheel event is one
    // notch (50 / 256). macOS sends one event per notch, but with an accelerated line delta that
    // can be a fraction of a line for a slow notch, so there every such event is one step (0 here).
   #if JUCE_WINDOWS
    constexpr float c_wheelNotch = 0.5f * 120.0f / 256.0f;
   #elif JUCE_LINUX || JUCE_BSD
    constexpr float c_wheelNotch = 50.0f / 256.0f;
   #else
    constexpr float c_wheelNotch = 0.0f;
   #endif
    constexpr float c_readoutClearance = 0.7f;  // gap kept between the readout and a ratio caption, in readout heights (~1.5 characters)
    constexpr float c_offLevel = 1.0e-4f;       // normalised level treated as off (the engine uses the same)
    constexpr float c_meterRelease = 0.78f;     // per 30 Hz tick: ~130 ms fall, so the glow doesn't flicker
    constexpr float c_meterRepaintPixels = 0.5f;

    // Simple intervals by semitone count, as musicians name them.
    const char* const c_intervalNames[12] { "unison", "minor 2nd", "major 2nd", "minor 3rd", "major 3rd", "perfect 4th",
                                            "tritone", "perfect 5th", "minor 6th", "major 6th", "minor 7th", "major 7th" };

    juce::String describeSemitones(int semitones)
    {
        if (semitones == 0)
            return "unison";

        const int distance = std::abs(semitones);
        const int octaves = distance / 12;
        const int rest = distance % 12;

        juce::String name = octaves == 1 ? juce::String("octave")
                          : octaves > 1  ? juce::String(octaves) + " octaves"
                                         : juce::String();

        if (rest > 0)
            name = name.isEmpty() ? juce::String(c_intervalNames[rest]) : name + " + " + c_intervalNames[rest];

        return semitones < 0 ? name + " below" : name;
    }

    juce::String formatPercent(float value)
    {
        // Low levels keep a decimal so a faint harmonic doesn't read as "0%".
        return value < 0.95f ? juce::String(value, 1) + "%" : juce::String(juce::roundToInt(value)) + "%";
    }

    // Width of a line of text as drawn. getStringWidth() measures the glyph boxes and skips
    // ligature placeholders, so text ending in a ligature (the body font joins the "ff" of
    // "off") would come out a character short; a trailing space, taken off again, avoids that.
    float lineWidth(const juce::Font& font, const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth(font, text + " ") - juce::GlyphArrangement::getStringWidth(font, " ");
    }
}

//==============================================================================
HarmonicStrip::HarmonicStrip(juce::AudioProcessorValueTreeState& parameters, const Theme::Fonts& fonts,
                             std::function<float(int harmonic)> meterSource)
    : m_fonts(fonts)
    , m_meterSource(std::move(meterSource))
{
    for (int k = 1; k <= c_numHarmonics; ++k)
    {
        const auto index = static_cast<size_t>(k - 1);
        m_labelTexts[index] = "H" + juce::String(k);
        m_ratioTexts[index] = getRatioText(k);

        auto& bar = barFor(k);
        bar.parameter = parameters.getParameter("harmonic" + juce::String(k));

        // The processor must define harmonic1..harmonic16; a missing one is drawn off and ignores the mouse.
        jassert(bar.parameter != nullptr);
        if (bar.parameter == nullptr)
            continue;

        // Same undo manager as the APVTS slider attachments, so gestures join its transactions.
        bar.attachment = std::make_unique<juce::ParameterAttachment>(*bar.parameter, [this, k](float value) { parameterChanged(k, value); },
                                                                     parameters.undoManager);
        bar.attachment->sendInitialUpdate();
    }

    setOpaque(false);

    if (m_meterSource)
        startTimerHz(30);
}

HarmonicStrip::~HarmonicStrip()
{
    stopTimer();
    endDragGesture(); // never leave a host gesture open
    endWheelGesture();
}

HarmonicStrip::Bar& HarmonicStrip::barFor(int harmonic)
{
    jassert(harmonic >= 1 && harmonic <= c_numHarmonics);
    return m_bars[static_cast<size_t>(juce::jlimit(1, c_numHarmonics, harmonic) - 1)];
}

const HarmonicStrip::Bar& HarmonicStrip::barFor(int harmonic) const
{
    jassert(harmonic >= 1 && harmonic <= c_numHarmonics);
    return m_bars[static_cast<size_t>(juce::jlimit(1, c_numHarmonics, harmonic) - 1)];
}

void HarmonicStrip::parameterChanged(int harmonic, float value)
{
    auto& bar = barFor(harmonic);
    bar.value = value;
    bar.level = bar.parameter->convertTo0to1(value);
    repaint();
}

//==============================================================================
void HarmonicStrip::setGhostProfile(const std::array<float, 17>& levels01)
{
    for (int k = 1; k <= c_numHarmonics; ++k)
    {
        const float level = levels01[static_cast<size_t>(k)];
        barFor(k).ghost = std::isfinite(level) ? juce::jlimit(0.0f, 1.0f, level) : 0.0f;
    }

    m_hasGhost = true;
    repaint();
}

void HarmonicStrip::clearGhostProfile()
{
    if (m_hasGhost)
    {
        m_hasGhost = false;
        repaint();
    }
}

void HarmonicStrip::timerCallback()
{
    if (isShowing())
        refreshMeters();
}

void HarmonicStrip::refreshMeters()
{
    if (!m_meterSource)
        return;

    for (int k = 1; k <= c_numHarmonics; ++k)
    {
        auto& bar = barFor(k);

        float reading = m_meterSource(k);
        reading = std::isfinite(reading) ? juce::jlimit(0.0f, 1.0f, reading) : 0.0f;

        // Instant attack, short release: steadier than raw 30 Hz samples of the engine's envelope.
        // A glow too small to see counts as silent, which also ends the release tail early.
        const float barPixels = bar.level * bar.area.getHeight();
        bar.meter = std::max(reading, bar.meter * c_meterRelease);
        if (bar.meter * barPixels < c_meterRepaintPixels)
            bar.meter = 0.0f;

        // Repaint only when the glow would move by a visible amount, and only that bar.
        const float pixels = std::abs(bar.meter - bar.paintedMeter) * barPixels;
        if (pixels >= c_meterRepaintPixels)
        {
            bar.paintedMeter = bar.meter;
            repaint(bar.slot.getSmallestIntegerContainer().expanded(1));
        }
        else if (bar.level <= c_offLevel)
        {
            bar.paintedMeter = bar.meter; // invisible while off; keep it current for when it turns on
        }
    }
}

//==============================================================================
juce::String HarmonicStrip::getRatioText(int harmonic)
{
    if (harmonic == 1)
        return "1/2";

    const juce::String whole(harmonic / 2);
    return (harmonic % 2 == 0 ? whole : whole + ".5") + "x";
}

juce::String HarmonicStrip::getIntervalText(int harmonic)
{
    if (harmonic < 1)
        return {};

    const double semitones = 12.0 * std::log2(static_cast<double>(harmonic) / 2.0);
    const int nearest = static_cast<int>(std::lround(semitones));
    const int cents = static_cast<int>(std::lround((semitones - nearest) * 100.0));

    auto text = describeSemitones(nearest);
    if (cents != 0)
        text << " (" << (cents > 0 ? "+" : "") << cents << " cents)";

    return text;
}

HarmonicStrip::ReadoutParts HarmonicStrip::getReadoutParts(int harmonic) const
{
    const auto& bar = barFor(harmonic);

    ReadoutParts parts;
    parts.name = "H" + juce::String(harmonic) + (harmonic == 1 ? " (sub)" : "");
    parts.details = " - " + getRatioText(harmonic) + " input - " + getIntervalText(harmonic) + " - ";
    parts.value = bar.level > c_offLevel ? formatPercent(bar.value) : juce::String("off");
    return parts;
}

juce::String HarmonicStrip::getReadoutText(int harmonic) const
{
    if (harmonic < 1 || harmonic > c_numHarmonics)
        return {};

    const auto parts = getReadoutParts(harmonic);
    return parts.name + parts.details + parts.value;
}

juce::Rectangle<float> HarmonicStrip::getReadoutBounds(int harmonic) const
{
    return layoutReadout(harmonic).bounds;
}

HarmonicStrip::ReadoutLayout HarmonicStrip::layoutReadout(int harmonic) const
{
    ReadoutLayout layout;
    if (harmonic < 1 || harmonic > c_numHarmonics)
        return layout;

    layout.parts = getReadoutParts(harmonic);
    layout.nameWidth = lineWidth(m_readoutFont, layout.parts.name);
    layout.detailsWidth = lineWidth(m_readoutFont, layout.parts.details);
    layout.valueWidth = lineWidth(m_readoutFont, layout.parts.value);
    layout.padding = m_readoutHeight * 0.6f;

    // Tooltip-style chip in the caption row, centred on the bar but kept inside the strip.
    const float stripWidth = static_cast<float>(getWidth());
    const float width = std::min(stripWidth, layout.nameWidth + layout.detailsWidth + layout.valueWidth + 2.0f * layout.padding);
    const float x = juce::jlimit(0.0f, std::max(0.0f, stripWidth - width), barFor(harmonic).column.getCentreX() - width * 0.5f);
    layout.bounds = { x, 0.0f, width, m_readoutHeight };
    return layout;
}

//==============================================================================
juce::Rectangle<float> HarmonicStrip::getBarArea(int harmonic) const
{
    return (harmonic >= 1 && harmonic <= c_numHarmonics) ? barFor(harmonic).area : juce::Rectangle<float>();
}

int HarmonicStrip::getHarmonicAt(float x) const
{
    const auto width = static_cast<float>(getWidth());
    if (width <= 0.0f || x < 0.0f || x >= width)
        return 0;

    return clampedHarmonicAt(x);
}

int HarmonicStrip::clampedHarmonicAt(float x) const
{
    // Columns tile the whole width, so the gaps between bars have no dead zones.
    const float width = std::max(1.0f, static_cast<float>(getWidth()));
    const int column = static_cast<int>(std::floor(x / width * static_cast<float>(c_numHarmonics)));
    return juce::jlimit(1, c_numHarmonics, column + 1);
}

int HarmonicStrip::hotHarmonic() const
{
    return m_activeHarmonic != 0 ? m_activeHarmonic : m_hoveredHarmonic;
}

void HarmonicStrip::setHovered(int harmonic)
{
    if (harmonic != m_hoveredHarmonic)
    {
        m_hoveredHarmonic = harmonic;
        repaint();
    }
}

//==============================================================================
void HarmonicStrip::resized()
{
    // Everything is proportional to the bounds, in float, so the strip stays crisp
    // inside the editor's AffineTransform scaling.
    const auto bounds = getLocalBounds().toFloat();
    const float height = bounds.getHeight();
    const float columnWidth = bounds.getWidth() / static_cast<float>(c_numHarmonics);
    const float gap = juce::jlimit(3.0f, 10.0f, columnWidth * 0.12f);

    // The caption row doubles as the lane for the hover readout, so it is tall enough
    // for the readout and the readout never covers the top of the bar being edited.
    const float captionHeight = juce::jlimit(13.5f, 18.0f, height * 0.17f);
    const float labelHeight = juce::jlimit(10.5f, 17.0f, height * 0.16f);

    m_captionFont = m_fonts.label(juce::jlimit(8.5f, 10.5f, height * 0.11f));
    m_labelFont = m_fonts.label(juce::jlimit(9.0f, 11.0f, labelHeight * 0.72f));
    m_readoutHeight = captionHeight - 1.0f;
    m_readoutFont = m_fonts.body(m_readoutHeight * 0.72f);

    const float slotWidth = columnWidth - gap;
    m_cornerRadius = juce::jlimit(2.0f, 5.0f, slotWidth * 0.08f);
    m_capThickness = juce::jlimit(1.5f, 2.5f, height * 0.025f);
    const float inset = juce::jlimit(1.5f, 3.0f, slotWidth * 0.04f);

    for (int k = 1; k <= c_numHarmonics; ++k)
    {
        auto& bar = barFor(k);
        bar.column = { bounds.getX() + columnWidth * static_cast<float>(k - 1), bounds.getY(), columnWidth, height };

        auto column = bar.column;
        bar.caption = column.removeFromTop(captionHeight);
        bar.label = column.removeFromBottom(labelHeight);
        bar.slot = column.reduced(gap * 0.5f, 0.0f);
        bar.area = bar.slot.reduced(inset);

        const float captionWidth = lineWidth(m_captionFont, m_ratioTexts[static_cast<size_t>(k - 1)]);
        bar.captionText = bar.caption.withSizeKeepingCentre(std::min(captionWidth, bar.caption.getWidth()), bar.caption.getHeight());
    }
}

void HarmonicStrip::paint(juce::Graphics& g)
{
    const int hot = hotHarmonic();

    // The readout is laid out first, so the bars know which ratio captions to leave out.
    const auto readout = hot != 0 ? layoutReadout(hot) : ReadoutLayout();

    // Meter updates repaint single bars, so skip the ones outside the clip.
    for (int k = 1; k <= c_numHarmonics; ++k)
        if (g.clipRegionIntersects(barFor(k).column.getSmallestIntegerContainer()))
            paintBar(g, k, k == hot, readout.bounds);

    if (hot != 0)
        paintReadout(g, hot, readout);
}

void HarmonicStrip::paintBar(juce::Graphics& g, int harmonic, bool hot, juce::Rectangle<float> readoutBounds) const
{
    using namespace Theme::Colours;

    const auto& bar = barFor(harmonic);
    const auto index = static_cast<size_t>(harmonic - 1);
    const bool on = bar.level > c_offLevel;
    const auto accent = harmonic == 1 ? output : overtones; // H1 is the sub itself
    const float innerRadius = std::max(1.0f, m_cornerRadius - 1.0f);

    // Ratio to the input pitch, above the bar. A caption the readout would cover even partly
    // is left out: the uncovered end ("1.5" or ".5x") would read as a different ratio.
    const bool coveredByReadout = !readoutBounds.isEmpty()
                               && bar.captionText.intersects(readoutBounds.expanded(m_readoutHeight * c_readoutClearance, 0.0f));
    if (!coveredByReadout)
    {
        g.setFont(m_captionFont);
        g.setColour(on ? textDim : textFaint.withAlpha(0.8f));
        g.drawText(m_ratioTexts[index], bar.caption, juce::Justification::centred, false);
    }

    // Slot: off bars are recessed and dim so they read as "off" at a glance.
    g.setColour(on ? panelRaised.brighter(hot ? 0.12f : 0.0f) : panelRaised.withAlpha(hot ? 0.8f : 0.45f));
    g.fillRoundedRectangle(bar.slot, m_cornerRadius);
    g.setColour(hot ? accent.withAlpha(0.6f) : on ? outline : outlineSoft);
    g.drawRoundedRectangle(bar.slot.reduced(0.5f), m_cornerRadius, 1.0f);

    // Ghost profile (e.g. an input analysis) as an outline behind the fill.
    if (m_hasGhost && bar.ghost > c_offLevel)
    {
        const auto ghost = bar.area.withTop(bar.area.getBottom() - bar.area.getHeight() * bar.ghost);
        g.setColour(input.withAlpha(0.07f));
        g.fillRoundedRectangle(ghost, innerRadius);
        g.setColour(input.withAlpha(0.65f));
        g.drawRoundedRectangle(ghost.reduced(0.5f), innerRadius, 1.0f);
    }

    if (on)
    {
        const float fillHeight = bar.area.getHeight() * bar.level;
        const auto fill = bar.area.withTop(bar.area.getBottom() - fillHeight);

        g.setGradientFill(juce::ColourGradient(accent.withAlpha(hot ? 0.58f : 0.44f), 0.0f, fill.getY(),
                                               accent.withAlpha(hot ? 0.28f : 0.18f), 0.0f, fill.getBottom(), false));
        g.fillRoundedRectangle(fill, innerRadius);

        // Live meter: an inner glow showing how much of the set level is sounding right now.
        const float glowHeight = fillHeight * bar.paintedMeter;
        if (glowHeight >= 0.5f)
        {
            const auto glow = fill.withTop(fill.getBottom() - glowHeight);

            // Brightest down the middle and softer at the sides, so it reads as light inside the bar.
            juce::ColourGradient tube(accent.withAlpha(0.4f), glow.getX(), 0.0f, accent.withAlpha(0.4f), glow.getRight(), 0.0f, false);
            tube.addColour(0.5, accent.brighter(0.3f).withAlpha(0.95f));
            g.setGradientFill(tube);
            g.fillRoundedRectangle(glow, innerRadius);
        }

        // Level line at the top of the fill.
        g.setColour(accent.withAlpha(hot ? 1.0f : 0.9f));
        g.fillRoundedRectangle(fill.withHeight(std::min(m_capThickness, fillHeight)), m_capThickness * 0.5f);
    }
    else
    {
        // Off: a short dash on the floor of the slot.
        const float dash = bar.area.getWidth() * 0.3f;
        g.setColour(textFaint.withAlpha(hot ? 0.9f : 0.5f));
        g.fillRoundedRectangle(juce::Rectangle<float>(dash, 1.5f).withCentre({ bar.area.getCentreX(), bar.area.getBottom() - 1.5f }), 0.75f);
    }

    // Harmonic number, below the bar.
    g.setFont(m_labelFont);
    g.setColour(hot ? text : !on ? textFaint : harmonic == 1 ? accent.withAlpha(0.85f) : textDim);
    g.drawText(m_labelTexts[index], bar.label, juce::Justification::centred, false);
}

void HarmonicStrip::paintReadout(juce::Graphics& g, int harmonic, const ReadoutLayout& layout) const
{
    using namespace Theme::Colours;

    const auto accent = harmonic == 1 ? output : overtones;
    const auto chip = layout.bounds;
    if (chip.isEmpty())
        return;

    g.setColour(background.withAlpha(0.94f));
    g.fillRoundedRectangle(chip, m_readoutHeight * 0.5f);
    g.setColour(accent.withAlpha(0.55f));
    g.drawRoundedRectangle(chip.reduced(0.5f), m_readoutHeight * 0.5f, 1.0f);

    // Each part's box runs on to the chip's edge: a box exactly as wide as the measured
    // text can lose its last glyphs to float rounding when JUCE shapes the line.
    auto textArea = chip.withTrimmedLeft(layout.padding);
    g.setFont(m_readoutFont);
    g.setColour(accent);
    g.drawText(layout.parts.name, textArea, juce::Justification::centredLeft, false);
    textArea.removeFromLeft(layout.nameWidth);
    g.setColour(textDim);
    g.drawText(layout.parts.details, textArea, juce::Justification::centredLeft, false);
    textArea.removeFromLeft(layout.detailsWidth);
    g.setColour(text);
    g.drawText(layout.parts.value, textArea, juce::Justification::centredLeft, false);
}

//==============================================================================
void HarmonicStrip::setLevel(int harmonic, float level)
{
    auto& bar = barFor(harmonic);
    if (bar.attachment != nullptr)
        bar.attachment->setValueAsCompleteGesture(bar.parameter->convertFrom0to1(juce::jlimit(0.0f, 1.0f, level)));
}

void HarmonicStrip::toggle(int harmonic)
{
    auto& bar = barFor(harmonic);
    if (bar.parameter == nullptr)
        return;

    if (bar.level > c_offLevel)
    {
        bar.remembered = bar.level;
        setLevel(harmonic, 0.0f);
        return;
    }

    // Back on: the level it had, else its default, else half way.
    const float defaultLevel = bar.parameter->getDefaultValue();
    setLevel(harmonic, bar.remembered > c_offLevel ? bar.remembered
                     : defaultLevel > c_offLevel    ? defaultLevel
                                                    : 0.5f);
}

void HarmonicStrip::resetToDefault(int harmonic)
{
    if (auto* parameter = barFor(harmonic).parameter)
        setLevel(harmonic, parameter->getDefaultValue());
}

void HarmonicStrip::nudge(int harmonic, int steps)
{
    auto& bar = barFor(harmonic);
    if (bar.attachment == nullptr)
        return;

    // Part of the scroll's gesture, which stays open until the wheel pauses.
    beginWheelGesture(harmonic);
    const float level = bar.level + static_cast<float>(steps) * c_wheelStep;
    bar.attachment->setValueAsPartOfGesture(bar.parameter->convertFrom0to1(juce::jlimit(0.0f, 1.0f, level)));
}

void HarmonicStrip::beginWheelGesture(int harmonic)
{
    if (m_wheelGestureHarmonic != harmonic)
    {
        endWheelGesture();

        if (auto* attachment = barFor(harmonic).attachment.get())
        {
            attachment->beginGesture();
            m_wheelGestureHarmonic = harmonic;
        }
    }

    if (m_wheelGestureHarmonic != 0)
        m_wheelGestureTimer.startTimer(static_cast<int>(c_wheelBurstMs));
}

void HarmonicStrip::endWheelGesture()
{
    m_wheelGestureTimer.stopTimer();

    if (m_wheelGestureHarmonic == 0)
        return;

    if (auto* attachment = barFor(m_wheelGestureHarmonic).attachment.get())
        attachment->endGesture();

    m_wheelGestureHarmonic = 0;
}

void HarmonicStrip::beginDragGesture(int harmonic, float y)
{
    endDragGesture();

    auto& bar = barFor(harmonic);
    m_activeHarmonic = harmonic;
    m_anchorY = y;

    if (m_fineDrag)
    {
        // Fine: move relative to the current level, so the bar never jumps.
        m_anchorLevel = bar.level;
        m_dragScale = c_fineScale;
    }
    else
    {
        // Normal: the level follows the mouse (top = 100 %, bottom = 0 %).
        m_anchorLevel = (bar.area.getBottom() - y) / std::max(1.0f, bar.area.getHeight());
        m_dragScale = 1.0f;
    }

    if (bar.attachment != nullptr)
        bar.attachment->beginGesture();
}

void HarmonicStrip::endDragGesture()
{
    if (m_activeHarmonic == 0)
        return;

    if (auto* attachment = barFor(m_activeHarmonic).attachment.get())
        attachment->endGesture();

    m_activeHarmonic = 0;
}

void HarmonicStrip::dragActiveBarTo(float y)
{
    if (m_activeHarmonic == 0)
        return;

    auto& bar = barFor(m_activeHarmonic);
    if (bar.attachment == nullptr)
        return;

    const float level = m_anchorLevel + (m_anchorY - y) / std::max(1.0f, bar.area.getHeight()) * m_dragScale;
    bar.attachment->setValueAsPartOfGesture(bar.parameter->convertFrom0to1(juce::jlimit(0.0f, 1.0f, level)));
}

void HarmonicStrip::drawAcrossTo(int harmonic, juce::Point<float> from, juce::Point<float> to)
{
    const int previous = m_paintHarmonic;
    endDragGesture();

    // A fast swipe can skip bars between two mouse events: give each the height
    // where the swipe crossed its centre, as its own gesture.
    if (previous != 0)
    {
        const int step = harmonic > previous ? 1 : -1;
        for (int k = previous + step; k != harmonic; k += step)
        {
            const auto& bar = barFor(k);
            const float dx = to.x - from.x;
            const float t = std::abs(dx) > 1.0e-3f ? juce::jlimit(0.0f, 1.0f, (bar.column.getCentreX() - from.x) / dx) : 1.0f;
            const float y = from.y + (to.y - from.y) * t;
            setLevel(k, (bar.area.getBottom() - y) / std::max(1.0f, bar.area.getHeight()));
        }
    }

    beginDragGesture(harmonic, to.y);
}

//==============================================================================
void HarmonicStrip::mouseEnter(const juce::MouseEvent& e)
{
    setHovered(getHarmonicAt(e.position.x));
}

void HarmonicStrip::mouseMove(const juce::MouseEvent& e)
{
    setHovered(getHarmonicAt(e.position.x));
}

void HarmonicStrip::mouseExit(const juce::MouseEvent&)
{
    setHovered(0);
    endWheelGesture(); // a scroll ends when the mouse leaves
}

void HarmonicStrip::mouseDown(const juce::MouseEvent& e)
{
    endDragGesture(); // in case an earlier drag never saw its mouse-up
    endWheelGesture(); // a click or drag is an edit of its own

    // Right-clicks are left alone so a host or editor menu doesn't also toggle a bar.
    m_pressedHarmonic = e.mods.isPopupMenu() ? 0 : getHarmonicAt(e.position.x);
    m_pressPosition = e.position;
    m_lastDragPosition = e.position;
    m_isDragging = false;
    m_pressWasDrag = false;

    // The click tolerance is in screen pixels: the editor scales its content, so at its
    // smallest size 3 local pixels would be under 2 on screen and a click would become a drag.
    const float scale = juce::Component::getApproximateScaleFactorForComponent(this);
    m_clickTolerance = c_clickTolerance / (scale > 0.0f ? scale : 1.0f);
}

void HarmonicStrip::mouseDrag(const juce::MouseEvent& e)
{
    if (m_pressedHarmonic == 0)
        return;

    const bool fine = e.mods.isShiftDown();

    if (!m_isDragging)
    {
        // Small wobbles during a click mustn't move the level.
        if (e.position.getDistanceFrom(m_pressPosition) < m_clickTolerance)
            return;

        m_isDragging = true;
        m_pressWasDrag = true;
        m_fineDrag = fine;
        m_paintHarmonic = m_pressedHarmonic;
        beginDragGesture(m_pressedHarmonic, m_pressPosition.y);
        dragActiveBarTo(m_pressPosition.y); // the pressed bar takes the press height even if a swipe has already left it
    }
    else if (fine != m_fineDrag && m_activeHarmonic != 0)
    {
        // Shift pressed or released mid-drag: carry on from the current level without a jump.
        m_fineDrag = fine;
        m_anchorLevel = barFor(m_activeHarmonic).level;
        m_anchorY = m_lastDragPosition.y;
        m_dragScale = fine ? c_fineScale : 1.0f;

        // A fine drag needs ~10x the mouse travel, so by now the mouse is usually well above or
        // below the strip, and it may have drifted over other bars. Drawing restarts from the
        // column the mouse is in now and only once it enters another one: letting go of shift
        // must not set the bars it drifted over to 0 or 100 %.
        if (!fine)
            m_paintHarmonic = clampedHarmonicAt(e.position.x);
    }

    // Plain drags draw across bars; fine drags stay on the bar they started on.
    if (!m_fineDrag)
    {
        const int harmonic = clampedHarmonicAt(e.position.x);
        if (harmonic != m_paintHarmonic)
        {
            // Coming back over the bar still being dragged (after a fine drag drifted off it) just carries on with it.
            if (harmonic != m_activeHarmonic)
                drawAcrossTo(harmonic, m_lastDragPosition, e.position);

            m_paintHarmonic = harmonic;
        }
    }

    dragActiveBarTo(e.position.y);
    m_lastDragPosition = e.position;
}

void HarmonicStrip::mouseUp(const juce::MouseEvent& e)
{
    if (m_isDragging)
    {
        endDragGesture();
    }
    else if (m_pressedHarmonic != 0 && e.getNumberOfClicks() < 2) // the second click of a double-click is handled there
    {
        // Alt-click resets, like the editor's knobs.
        if (e.mods.isAltDown())
            resetToDefault(m_pressedHarmonic);
        else
            toggle(m_pressedHarmonic);
    }

    m_isDragging = false;
    m_pressedHarmonic = 0;

    setHovered(getLocalBounds().toFloat().contains(e.position) ? getHarmonicAt(e.position.x) : 0);
    repaint();
}

void HarmonicStrip::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (m_pressWasDrag || e.mods.isPopupMenu())
        return;

    if (const int harmonic = getHarmonicAt(e.position.x); harmonic != 0)
        resetToDefault(harmonic);
}

void HarmonicStrip::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int harmonic = getHarmonicAt(e.position.x);
    const float delta = wheel.deltaY * (wheel.isReversed ? -1.0f : 1.0f); // same direction as the knobs

    // The bars are vertical, so a mostly sideways swipe isn't aimed at them: its small
    // vertical part mustn't nudge a level, and a parent may want to scroll with it.
    const bool sideways = std::abs(wheel.deltaX) > std::abs(wheel.deltaY);

    if (harmonic == 0 || sideways || juce::exactlyEqual(delta, 0.0f))
    {
        juce::Component::mouseWheelMove(e, wheel); // let a parent scroll
        return;
    }

    // Momentum after the fingers lift would keep changing the level; some hosts send duplicates.
    if (e.mods.isAnyMouseButtonDown() || wheel.isInertial || e.eventTime == m_lastWheelTime)
        return;

    const auto msSinceLastEvent = (e.eventTime - m_lastWheelTime).inMilliseconds();
    m_lastWheelTime = e.eventTime;

    // One scroll (events on the same bar without a pause) is one host gesture, however many
    // steps it makes. While it lasts, every event keeps the gesture open.
    const bool sameScroll = harmonic == m_wheelHarmonic && msSinceLastEvent >= 0 && msSinceLastEvent <= c_wheelBurstMs;
    if (!sameScroll)
        endWheelGesture();
    else if (m_wheelGestureHarmonic != 0)
        m_wheelGestureTimer.startTimer(static_cast<int>(c_wheelBurstMs));

    int steps = 0;
    const float notch = wheel.isSmooth ? c_trackpadNotch : c_wheelNotch;

    if (notch > 0.0f)
    {
        // Trackpads (and, on Windows, high-resolution wheels and precision touchpads) send many
        // small deltas: step once per notch-sized amount. What is left over belongs to that scroll
        // only: a pause, a change of direction or of event kind drops it, so a light touch later
        // on can't complete a step by itself.
        const auto pauseMs = wheel.isSmooth ? c_trackpadPauseMs : c_wheelBurstMs;
        const bool newScroll = harmonic != m_wheelHarmonic || msSinceLastEvent < 0 || msSinceLastEvent > pauseMs
                            || m_wheelAccumulator * delta < 0.0f || wheel.isSmooth != m_wheelWasSmooth;
        if (newScroll)
            m_wheelAccumulator = 0.0f;

        m_wheelAccumulator += delta;

        // The small margin stops float drift (5 x 0.02 = 0.0499999) from swallowing a step.
        const float notches = m_wheelAccumulator / notch;
        steps = static_cast<int>(notches + (notches > 0.0f ? 1.0e-3f : -1.0e-3f));
        m_wheelAccumulator -= static_cast<float>(steps) * notch;
    }
    else
    {
        // A notched wheel that reports one event per notch (see c_wheelNotch).
        m_wheelAccumulator = 0.0f;
        steps = delta > 0.0f ? 1 : -1;
    }

    m_wheelHarmonic = harmonic;
    m_wheelWasSmooth = wheel.isSmooth;

    if (steps != 0)
        nudge(harmonic, steps);
}

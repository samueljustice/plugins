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
    constexpr float c_subFloorHz = 20.0f;       // the engine generates no S2 / S3 at or below this (its Sub Floor)

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
                             std::function<float(int bar)> meterSource, std::function<float()> inputPitchSource)
    : m_fonts(fonts)
    , m_meterSource(std::move(meterSource))
    , m_inputPitchSource(std::move(inputPitchSource))
{
    for (int column = 0; column < c_numBars; ++column)
    {
        const int bar = barInColumn(column);
        const auto index = static_cast<size_t>(column);
        m_labelTexts[index] = labelFor(bar);
        m_ratioTexts[index] = getRatioText(bar);

        auto& b = barFor(bar);
        b.parameter = parameters.getParameter(bar > 0 ? "harmonic" + juce::String(bar) : "subOctave" + juce::String(-bar));

        // The processor must define harmonic1..harmonic16, subOctave2 and subOctave3; a missing one is drawn off and
        // ignores the mouse.
        jassert(b.parameter != nullptr);
        if (b.parameter == nullptr)
            continue;

        // Same undo manager as the APVTS slider attachments, so gestures join its transactions.
        b.attachment = std::make_unique<juce::ParameterAttachment>(*b.parameter, [this, bar](float value) { parameterChanged(bar, value); },
                                                                   parameters.undoManager);
        b.attachment->sendInitialUpdate();
    }

    setOpaque(false);

    if (m_meterSource || m_inputPitchSource)
        startTimerHz(30);
}

HarmonicStrip::~HarmonicStrip()
{
    stopTimer();
    endDragGesture(); // never leave a host gesture open
    endWheelGesture();
}

//==============================================================================
int HarmonicStrip::columnOf(int bar)
{
    if (bar >= 1 && bar <= c_numHarmonics)
        return bar + 1;

    return bar == c_subOctave3 ? 0 : bar == c_subOctave2 ? 1 : -1;
}

int HarmonicStrip::barInColumn(int column)
{
    column = juce::jlimit(0, c_numBars - 1, column);
    return column == 0 ? c_subOctave3 : column == 1 ? c_subOctave2 : column - 1;
}

bool HarmonicStrip::isBar(int bar)
{
    return columnOf(bar) >= 0;
}

juce::String HarmonicStrip::labelFor(int bar)
{
    return isSubOctave(bar) ? "S" + juce::String(-bar) : "H" + juce::String(bar);
}

HarmonicStrip::Bar& HarmonicStrip::barFor(int bar)
{
    jassert(isBar(bar));
    return m_bars[static_cast<size_t>(std::max(0, columnOf(bar)))];
}

const HarmonicStrip::Bar& HarmonicStrip::barFor(int bar) const
{
    jassert(isBar(bar));
    return m_bars[static_cast<size_t>(std::max(0, columnOf(bar)))];
}

void HarmonicStrip::parameterChanged(int bar, float value)
{
    auto& b = barFor(bar);
    b.value = value;
    b.level = b.parameter->convertTo0to1(value);
    repaint();
}

bool HarmonicStrip::isBelowFloor(int bar) const
{
    if (!isSubOctave(bar) || !m_inputPitchSource)
        return false;

    // S2 sounds at a quarter of the input's pitch and S3 at an eighth.
    const float pitch = m_inputPitchSource();
    return std::isfinite(pitch) && pitch > 0.0f && pitch / static_cast<float>(1 << -bar) <= c_subFloorHz;
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
    if (m_meterSource)
    {
        for (int column = 0; column < c_numBars; ++column)
        {
            auto& bar = m_bars[static_cast<size_t>(column)];

            float reading = m_meterSource(barInColumn(column));
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

    // S2 / S3: their readout says when the input's pitch puts them below 20 Hz, so the caption row it is drawn in is
    // repainted when that changes for the bar it shows.
    for (const int bar : { c_subOctave3, c_subOctave2 })
    {
        auto& b = barFor(bar);
        const bool below = isBelowFloor(bar);

        if (below != b.belowFloor)
        {
            b.belowFloor = below;

            if (hotBar() == bar)
                repaint(getLocalBounds().withHeight(static_cast<int>(std::ceil(b.caption.getBottom())) + 1));
        }
    }
}

//==============================================================================
juce::String HarmonicStrip::getRatioText(int bar)
{
    if (bar == c_subOctave3)
        return "1/8";

    if (bar == c_subOctave2)
        return "1/4";

    if (bar == 1)
        return "1/2";

    if (bar < 1)
        return {};

    const juce::String whole(bar / 2);
    return (bar % 2 == 0 ? whole : whole + ".5") + "x";
}

juce::String HarmonicStrip::getIntervalText(int bar)
{
    if (bar == c_subOctave2)
        return "two octaves below";

    if (bar == c_subOctave3)
        return "three octaves below";

    if (bar < 1)
        return {};

    const double semitones = 12.0 * std::log2(static_cast<double>(bar) / 2.0);
    const int nearest = static_cast<int>(std::lround(semitones));
    const int cents = static_cast<int>(std::lround((semitones - nearest) * 100.0));

    auto text = describeSemitones(nearest);
    if (cents != 0)
        text << " (" << (cents > 0 ? "+" : "") << cents << " cents)";

    return text;
}

HarmonicStrip::ReadoutParts HarmonicStrip::getReadoutParts(int bar) const
{
    const auto& b = barFor(bar);
    const bool on = b.level > c_offLevel;

    ReadoutParts parts;

    if (isSubOctave(bar))
    {
        parts.name = labelFor(bar);
        parts.details = " - " + getRatioText(bar) + " of the input - " + getIntervalText(bar) + " - ";

        // Set, but silent at the pitch being played: the engine makes nothing at or below 20 Hz.
        if (on && isBelowFloor(bar))
            parts.note = " - below 20 Hz at this pitch";
    }
    else
    {
        parts.name = "H" + juce::String(bar) + (bar == 1 ? " (sub)" : "");
        parts.details = " - " + getRatioText(bar) + " input - " + getIntervalText(bar) + " - ";
    }

    parts.value = on ? formatPercent(b.value) : juce::String("off");
    return parts;
}

juce::String HarmonicStrip::getReadoutText(int bar) const
{
    if (!isBar(bar))
        return {};

    const auto parts = getReadoutParts(bar);
    return parts.name + parts.details + parts.value + parts.note;
}

juce::Rectangle<float> HarmonicStrip::getReadoutBounds(int bar) const
{
    return layoutReadout(bar).bounds;
}

HarmonicStrip::ReadoutLayout HarmonicStrip::layoutReadout(int bar) const
{
    ReadoutLayout layout;
    if (!isBar(bar))
        return layout;

    layout.parts = getReadoutParts(bar);
    layout.nameWidth = lineWidth(m_readoutFont, layout.parts.name);
    layout.detailsWidth = lineWidth(m_readoutFont, layout.parts.details);
    layout.valueWidth = lineWidth(m_readoutFont, layout.parts.value);
    layout.noteWidth = layout.parts.note.isEmpty() ? 0.0f : lineWidth(m_readoutFont, layout.parts.note);
    layout.padding = m_readoutHeight * 0.6f;

    // Tooltip-style chip in the caption row, centred on the bar but kept inside the strip.
    const float stripWidth = static_cast<float>(getWidth());
    const float width = std::min(stripWidth, layout.nameWidth + layout.detailsWidth + layout.valueWidth + layout.noteWidth
                                                 + 2.0f * layout.padding);
    const float x = juce::jlimit(0.0f, std::max(0.0f, stripWidth - width), barFor(bar).column.getCentreX() - width * 0.5f);
    layout.bounds = { x, 0.0f, width, m_readoutHeight };
    return layout;
}

//==============================================================================
juce::Rectangle<float> HarmonicStrip::getBarArea(int bar) const
{
    return isBar(bar) ? barFor(bar).area : juce::Rectangle<float>();
}

int HarmonicStrip::getBarAt(float x) const
{
    const auto width = static_cast<float>(getWidth());
    if (width <= 0.0f || x < 0.0f || x >= width)
        return 0;

    return clampedBarAt(x);
}

int HarmonicStrip::clampedBarAt(float x) const
{
    // Columns tile the whole width, so the gaps between bars have no dead zones.
    const float width = std::max(1.0f, static_cast<float>(getWidth()));
    const int column = static_cast<int>(std::floor(x / width * static_cast<float>(c_numBars)));
    return barInColumn(column);
}

int HarmonicStrip::hotBar() const
{
    return m_activeBar != 0 ? m_activeBar : m_hoveredBar;
}

void HarmonicStrip::setHovered(int bar)
{
    if (bar != m_hoveredBar)
    {
        m_hoveredBar = bar;
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
    const float columnWidth = bounds.getWidth() / static_cast<float>(c_numBars);
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

    for (int column = 0; column < c_numBars; ++column)
    {
        auto& bar = m_bars[static_cast<size_t>(column)];
        bar.column = { bounds.getX() + columnWidth * static_cast<float>(column), bounds.getY(), columnWidth, height };

        auto area = bar.column;
        bar.caption = area.removeFromTop(captionHeight);
        bar.label = area.removeFromBottom(labelHeight);
        bar.slot = area.reduced(gap * 0.5f, 0.0f);
        bar.area = bar.slot.reduced(inset);

        const float captionWidth = lineWidth(m_captionFont, m_ratioTexts[static_cast<size_t>(column)]);
        bar.captionText = bar.caption.withSizeKeepingCentre(std::min(captionWidth, bar.caption.getWidth()), bar.caption.getHeight());
    }
}

void HarmonicStrip::paint(juce::Graphics& g)
{
    const int hot = hotBar();

    // The readout is laid out first, so the bars know which ratio captions to leave out.
    const auto readout = hot != 0 ? layoutReadout(hot) : ReadoutLayout();

    // Meter updates repaint single bars, so skip the ones outside the clip.
    for (int column = 0; column < c_numBars; ++column)
    {
        const int bar = barInColumn(column);
        if (g.clipRegionIntersects(m_bars[static_cast<size_t>(column)].column.getSmallestIntegerContainer()))
            paintBar(g, bar, bar == hot, readout.bounds);
    }

    if (hot != 0)
        paintReadout(g, hot, readout);
}

void HarmonicStrip::paintBar(juce::Graphics& g, int bar, bool hot, juce::Rectangle<float> readoutBounds) const
{
    using namespace Theme::Colours;

    const auto& b = barFor(bar);
    const auto index = static_cast<size_t>(columnOf(bar));
    const bool on = b.level > c_offLevel;
    const bool sub = bar == 1 || isSubOctave(bar);   // S3, S2 and H1 are the subs; the rest are overtones
    const auto accent = sub ? output : overtones;
    const float innerRadius = std::max(1.0f, m_cornerRadius - 1.0f);

    // Ratio to the input pitch, above the bar. A caption the readout would cover even partly
    // is left out: the uncovered end ("1.5" or ".5x") would read as a different ratio.
    const bool coveredByReadout = !readoutBounds.isEmpty()
                               && b.captionText.intersects(readoutBounds.expanded(m_readoutHeight * c_readoutClearance, 0.0f));
    if (!coveredByReadout)
    {
        g.setFont(m_captionFont);
        g.setColour(on ? textDim : textFaint.withAlpha(0.8f));
        g.drawText(m_ratioTexts[index], b.caption, juce::Justification::centred, false);
    }

    // Slot: off bars are recessed and dim so they read as "off" at a glance.
    g.setColour(on ? panelRaised.brighter(hot ? 0.12f : 0.0f) : panelRaised.withAlpha(hot ? 0.8f : 0.45f));
    g.fillRoundedRectangle(b.slot, m_cornerRadius);
    g.setColour(hot ? accent.withAlpha(0.6f) : on ? outline : outlineSoft);
    g.drawRoundedRectangle(b.slot.reduced(0.5f), m_cornerRadius, 1.0f);

    // Ghost profile (e.g. an input analysis) as an outline behind the fill.
    if (m_hasGhost && b.ghost > c_offLevel)
    {
        const auto ghost = b.area.withTop(b.area.getBottom() - b.area.getHeight() * b.ghost);
        g.setColour(input.withAlpha(0.07f));
        g.fillRoundedRectangle(ghost, innerRadius);
        g.setColour(input.withAlpha(0.65f));
        g.drawRoundedRectangle(ghost.reduced(0.5f), innerRadius, 1.0f);
    }

    if (on)
    {
        const float fillHeight = b.area.getHeight() * b.level;
        const auto fill = b.area.withTop(b.area.getBottom() - fillHeight);

        g.setGradientFill(juce::ColourGradient(accent.withAlpha(hot ? 0.58f : 0.44f), 0.0f, fill.getY(),
                                               accent.withAlpha(hot ? 0.28f : 0.18f), 0.0f, fill.getBottom(), false));
        g.fillRoundedRectangle(fill, innerRadius);

        // Live meter: an inner glow showing how much of the set level is sounding right now.
        const float glowHeight = fillHeight * b.paintedMeter;
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
        const float dash = b.area.getWidth() * 0.3f;
        g.setColour(textFaint.withAlpha(hot ? 0.9f : 0.5f));
        g.fillRoundedRectangle(juce::Rectangle<float>(dash, 1.5f).withCentre({ b.area.getCentreX(), b.area.getBottom() - 1.5f }), 0.75f);
    }

    // Bar name, below the bar.
    g.setFont(m_labelFont);
    g.setColour(hot ? text : !on ? textFaint : sub ? accent.withAlpha(0.85f) : textDim);
    g.drawText(m_labelTexts[index], b.label, juce::Justification::centred, false);
}

void HarmonicStrip::paintReadout(juce::Graphics& g, int bar, const ReadoutLayout& layout) const
{
    using namespace Theme::Colours;

    const auto accent = (bar == 1 || isSubOctave(bar)) ? output : overtones;
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

    if (layout.parts.note.isNotEmpty())
    {
        textArea.removeFromLeft(layout.valueWidth);
        g.setColour(textDim);
        g.drawText(layout.parts.note, textArea, juce::Justification::centredLeft, false);
    }
}

//==============================================================================
void HarmonicStrip::setLevel(int bar, float level)
{
    auto& b = barFor(bar);
    if (b.attachment != nullptr)
        b.attachment->setValueAsCompleteGesture(b.parameter->convertFrom0to1(juce::jlimit(0.0f, 1.0f, level)));
}

void HarmonicStrip::toggle(int bar)
{
    auto& b = barFor(bar);
    if (b.parameter == nullptr)
        return;

    if (b.level > c_offLevel)
    {
        b.remembered = b.level;
        setLevel(bar, 0.0f);
        return;
    }

    // Back on: the level it had, else its default, else half way.
    const float defaultLevel = b.parameter->getDefaultValue();
    setLevel(bar, b.remembered > c_offLevel ? b.remembered
                : defaultLevel > c_offLevel  ? defaultLevel
                                             : 0.5f);
}

void HarmonicStrip::resetToDefault(int bar)
{
    if (auto* parameter = barFor(bar).parameter)
        setLevel(bar, parameter->getDefaultValue());
}

void HarmonicStrip::nudge(int bar, int steps)
{
    auto& b = barFor(bar);
    if (b.attachment == nullptr)
        return;

    // Part of the scroll's gesture, which stays open until the wheel pauses.
    beginWheelGesture(bar);
    const float level = b.level + static_cast<float>(steps) * c_wheelStep;
    b.attachment->setValueAsPartOfGesture(b.parameter->convertFrom0to1(juce::jlimit(0.0f, 1.0f, level)));
}

void HarmonicStrip::beginWheelGesture(int bar)
{
    if (m_wheelGestureBar != bar)
    {
        endWheelGesture();

        if (auto* attachment = barFor(bar).attachment.get())
        {
            attachment->beginGesture();
            m_wheelGestureBar = bar;
        }
    }

    if (m_wheelGestureBar != 0)
        m_wheelGestureTimer.startTimer(static_cast<int>(c_wheelBurstMs));
}

void HarmonicStrip::endWheelGesture()
{
    m_wheelGestureTimer.stopTimer();

    if (m_wheelGestureBar == 0)
        return;

    if (auto* attachment = barFor(m_wheelGestureBar).attachment.get())
        attachment->endGesture();

    m_wheelGestureBar = 0;
}

void HarmonicStrip::beginDragGesture(int bar, float y)
{
    endDragGesture();

    auto& b = barFor(bar);
    m_activeBar = bar;
    m_anchorY = y;

    if (m_fineDrag)
    {
        // Fine: move relative to the current level, so the bar never jumps.
        m_anchorLevel = b.level;
        m_dragScale = c_fineScale;
    }
    else
    {
        // Normal: the level follows the mouse (top = 100 %, bottom = 0 %).
        m_anchorLevel = (b.area.getBottom() - y) / std::max(1.0f, b.area.getHeight());
        m_dragScale = 1.0f;
    }

    if (b.attachment != nullptr)
        b.attachment->beginGesture();
}

void HarmonicStrip::endDragGesture()
{
    if (m_activeBar == 0)
        return;

    if (auto* attachment = barFor(m_activeBar).attachment.get())
        attachment->endGesture();

    m_activeBar = 0;
}

void HarmonicStrip::dragActiveBarTo(float y)
{
    if (m_activeBar == 0)
        return;

    auto& b = barFor(m_activeBar);
    if (b.attachment == nullptr)
        return;

    const float level = m_anchorLevel + (m_anchorY - y) / std::max(1.0f, b.area.getHeight()) * m_dragScale;
    b.attachment->setValueAsPartOfGesture(b.parameter->convertFrom0to1(juce::jlimit(0.0f, 1.0f, level)));
}

void HarmonicStrip::drawAcrossTo(int bar, juce::Point<float> from, juce::Point<float> to)
{
    const int previous = m_paintBar;
    endDragGesture();

    // A fast swipe can skip bars between two mouse events: give each the height
    // where the swipe crossed its centre, as its own gesture.
    if (previous != 0)
    {
        const int first = columnOf(previous), last = columnOf(bar);
        const int step = last > first ? 1 : -1;
        for (int column = first + step; column != last; column += step)
        {
            const int skipped = barInColumn(column);
            const auto& b = barFor(skipped);
            const float dx = to.x - from.x;
            const float t = std::abs(dx) > 1.0e-3f ? juce::jlimit(0.0f, 1.0f, (b.column.getCentreX() - from.x) / dx) : 1.0f;
            const float y = from.y + (to.y - from.y) * t;
            setLevel(skipped, (b.area.getBottom() - y) / std::max(1.0f, b.area.getHeight()));
        }
    }

    beginDragGesture(bar, to.y);
}

//==============================================================================
void HarmonicStrip::mouseEnter(const juce::MouseEvent& e)
{
    setHovered(getBarAt(e.position.x));
}

void HarmonicStrip::mouseMove(const juce::MouseEvent& e)
{
    setHovered(getBarAt(e.position.x));
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
    m_pressedBar = e.mods.isPopupMenu() ? 0 : getBarAt(e.position.x);
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
    if (m_pressedBar == 0)
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
        m_paintBar = m_pressedBar;
        beginDragGesture(m_pressedBar, m_pressPosition.y);
        dragActiveBarTo(m_pressPosition.y); // the pressed bar takes the press height even if a swipe has already left it
    }
    else if (fine != m_fineDrag && m_activeBar != 0)
    {
        // Shift pressed or released mid-drag: carry on from the current level without a jump.
        m_fineDrag = fine;
        m_anchorLevel = barFor(m_activeBar).level;
        m_anchorY = m_lastDragPosition.y;
        m_dragScale = fine ? c_fineScale : 1.0f;

        // A fine drag needs ~10x the mouse travel, so by now the mouse is usually well above or
        // below the strip, and it may have drifted over other bars. Drawing restarts from the
        // column the mouse is in now and only once it enters another one: letting go of shift
        // must not set the bars it drifted over to 0 or 100 %.
        if (!fine)
            m_paintBar = clampedBarAt(e.position.x);
    }

    // Plain drags draw across bars; fine drags stay on the bar they started on.
    if (!m_fineDrag)
    {
        const int bar = clampedBarAt(e.position.x);
        if (bar != m_paintBar)
        {
            // Coming back over the bar still being dragged (after a fine drag drifted off it) just carries on with it.
            if (bar != m_activeBar)
                drawAcrossTo(bar, m_lastDragPosition, e.position);

            m_paintBar = bar;
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
    else if (m_pressedBar != 0 && e.getNumberOfClicks() < 2) // the second click of a double-click is handled there
    {
        // Alt-click resets, like the editor's knobs.
        if (e.mods.isAltDown())
            resetToDefault(m_pressedBar);
        else
            toggle(m_pressedBar);
    }

    m_isDragging = false;
    m_pressedBar = 0;

    setHovered(getLocalBounds().toFloat().contains(e.position) ? getBarAt(e.position.x) : 0);
    repaint();
}

void HarmonicStrip::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (m_pressWasDrag || e.mods.isPopupMenu())
        return;

    if (const int bar = getBarAt(e.position.x); bar != 0)
        resetToDefault(bar);
}

void HarmonicStrip::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int bar = getBarAt(e.position.x);
    const float delta = wheel.deltaY * (wheel.isReversed ? -1.0f : 1.0f); // same direction as the knobs

    // The bars are vertical, so a mostly sideways swipe isn't aimed at them: its small
    // vertical part mustn't nudge a level, and a parent may want to scroll with it.
    const bool sideways = std::abs(wheel.deltaX) > std::abs(wheel.deltaY);

    if (bar == 0 || sideways || juce::exactlyEqual(delta, 0.0f))
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
    const bool sameScroll = bar == m_wheelBar && msSinceLastEvent >= 0 && msSinceLastEvent <= c_wheelBurstMs;
    if (!sameScroll)
        endWheelGesture();
    else if (m_wheelGestureBar != 0)
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
        const bool newScroll = bar != m_wheelBar || msSinceLastEvent < 0 || msSinceLastEvent > pauseMs
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

    m_wheelBar = bar;
    m_wheelWasSmooth = wheel.isSmooth;

    if (steps != 0)
        nudge(bar, steps);
}

#include "SnippetDisplay.h"
#include "PluginProcessor.h"

namespace
{
    const char* const c_modeNames[] = { "REVERSE PLAYBACK", "FORWARD BACKWARDS", "REVERSE REPEAT" };

    float targetSpanFor(float timeSeconds, int mode)
    {
        // Enough history to show every read head: a reversing voice reaches back 2T,
        // and Reverse Repeat's second (delayed) pass a further T/2.
        const float reach = mode == 2 ? 2.5f : 2.0f;
        return juce::jlimit(2.5f, 11.0f, timeSeconds * reach + 0.35f);
    }
}

SnippetDisplay::SnippetDisplay(ReversinatorAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor)
    , m_fonts(fonts)
    , m_history(static_cast<size_t>(ReversinatorAudioProcessor::peakHistorySize), 0.0f)
{
    setOpaque(true);
    const auto initial = m_processor.getDisplayState();
    m_spanSeconds = targetSpanFor(initial.timeSeconds, initial.mode);
    startTimerHz(30);
}

SnippetDisplay::~SnippetDisplay()
{
    stopTimer();
}

void SnippetDisplay::update()
{
    const auto state = m_processor.getDisplayState();
    const float target = targetSpanFor(state.timeSeconds, state.mode);
    m_spanSeconds += (target - m_spanSeconds) * 0.18f;

    // Snapshot just the history the view needs (newest last).
    const double binSeconds = m_processor.getDisplayBinSeconds();
    m_historyCount = binSeconds > 0.0
        ? juce::jlimit(1, ReversinatorAudioProcessor::peakHistorySize, static_cast<int>(m_spanSeconds / binSeconds) + 2)
        : 0;
    m_processor.copyPeakHistory(m_history.data(), m_historyCount);

    repaint();
}

void SnippetDisplay::paint(juce::Graphics& g)
{
    using namespace Theme::Colours;

    const auto state = m_processor.getDisplayState();
    const auto bounds = getLocalBounds().toFloat();

    g.fillAll(background);
    g.setColour(scope);
    g.fillRoundedRectangle(bounds, 8.0f);

    auto plot = bounds.reduced(14.0f, 10.0f);
    plot.removeFromTop(16.0f);
    const auto scale = plot.removeFromBottom(14.0f);
    const float midY = plot.getCentreY();
    const float halfHeight = plot.getHeight() * 0.5f - 2.0f;
    const double binSeconds = m_processor.getDisplayBinSeconds();
    const float span = m_spanSeconds;

    const auto xForAge = [&](float ageSeconds) { return plot.getRight() - (ageSeconds / span) * plot.getWidth(); };

    // Grid: one line per second, labelled underneath.
    g.setFont(m_fonts.body(9.5f));
    for (int s = 0; static_cast<float>(s) <= span; ++s)
    {
        const float x = xForAge(static_cast<float>(s));
        g.setColour(grid);
        g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
        g.setColour(textFaint);
        const juce::String label = s == 0 ? juce::String("NOW") : "-" + juce::String(s) + " s";
        g.drawText(label, juce::Rectangle<float>(x - 40.0f, scale.getY(), 40.0f, scale.getHeight()),
                   juce::Justification::centredRight, false);
    }
    g.setColour(grid);
    g.drawHorizontalLine(juce::roundToInt(midY), plot.getX(), plot.getRight());

    // Input history as a mirrored envelope, newest at the right.
    if (binSeconds > 0.0 && m_historyCount > 1)
    {
        const int binsVisible = juce::jmin(m_historyCount - 1, static_cast<int>(span / binSeconds));
        const float pixelsPerBin = plot.getWidth() * static_cast<float>(binSeconds) / span;
        const int step = juce::jmax(1, static_cast<int>(1.0f / pixelsPerBin));

        juce::Path wave;
        bool started = false;
        std::vector<juce::Point<float>> lower;
        lower.reserve(static_cast<size_t>(binsVisible / step + 2));

        for (int i = 0; i <= binsVisible; i += step)
        {
            float peak = 0.0f;
            for (int k = 0; k < step && i + k < m_historyCount; ++k)
                peak = std::max(peak, m_history[static_cast<size_t>(m_historyCount - 1 - i - k)]);

            // Gentle compression so quiet material is still visible.
            const float h = halfHeight * std::sqrt(juce::jlimit(0.0f, 1.0f, peak));
            const float x = plot.getRight() - static_cast<float>(i) * pixelsPerBin;

            if (! started) { wave.startNewSubPath(x, midY - h); started = true; }
            else           { wave.lineTo(x, midY - h); }
            lower.push_back({ x, midY + h });
        }

        for (auto it = lower.rbegin(); it != lower.rend(); ++it)
            wave.lineTo(*it);
        wave.closeSubPath();

        g.setColour(textDim.withAlpha(0.22f));
        g.fillPath(wave);
        g.setColour(textDim.withAlpha(0.35f));
        g.strokePath(wave, juce::PathStrokeType(0.8f));
    }

    // Read heads.
    const auto accent = state.reversing ? reverser : textFaint;
    for (int v = 0; v < state.numVoices; ++v)
    {
        const auto& voice = state.voices[static_cast<size_t>(v)];
        if (voice.gain <= 0.002f || voice.delaySeconds > span)
            continue;

        const float x = xForAge(voice.delaySeconds);
        const float alpha = juce::jlimit(0.0f, 1.0f, voice.gain);

        g.setGradientFill(juce::ColourGradient(accent.withAlpha(0.0f), x - 26.0f, 0.0f, accent.withAlpha(0.16f * alpha), x, 0.0f, false));
        g.fillRect(juce::Rectangle<float>(x - 26.0f, plot.getY(), 26.0f, plot.getHeight()));

        g.setColour(accent.withAlpha(0.25f + 0.75f * alpha));
        g.fillRect(juce::Rectangle<float>(x - 1.0f, plot.getY(), 2.0f, plot.getHeight()));
        g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre({ x, midY }));
    }

    // Write head ("now")
    g.setColour(sand.withAlpha(0.8f));
    g.fillRect(juce::Rectangle<float>(plot.getRight() - 1.0f, plot.getY(), 2.0f, plot.getHeight()));

    // Labels
    auto top = bounds.reduced(14.0f, 8.0f).removeFromTop(16.0f);
    g.setFont(m_fonts.label(10.0f));
    g.setColour(text.withAlpha(0.85f));
    g.drawText(c_modeNames[juce::jlimit(0, 2, state.mode)], top, juce::Justification::centredLeft, false);

    g.setColour(state.reversing ? reverser : textDim);
    const juce::String status = state.reversing ? juce::String::fromUTF8("\xe2\x97\x80  REVERSING") : juce::String::fromUTF8("\xe2\x96\xb6  FORWARDS");
    g.drawText(status + "   " + juce::String(state.timeSeconds, 2) + " s", top, juce::Justification::centredRight, false);

    g.setColour(outlineSoft);
    g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 1.0f);
}

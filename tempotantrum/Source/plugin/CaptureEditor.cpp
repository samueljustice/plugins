#include "CaptureEditor.h"

#include "ui/Theme.h"

namespace tt::audiosuite
{

namespace
{
    // "0:38.04"
    juce::String lengthText (juce::int64 samples, double sampleRate)
    {
        if (sampleRate <= 0.0)
            return juce::String (samples) + " samples";

        const double seconds = (double) samples / sampleRate;
        const int minutes = (int) (seconds / 60.0);
        return juce::String (minutes) + ":" + juce::String (seconds - minutes * 60.0, 2).paddedLeft ('0', 5);
    }
}

CaptureEditor::CaptureEditor (CaptureProcessor& p)
    : juce::AudioProcessorEditor (p), captureProcessor (p)
{
    setSize (440, 140);
    timerCallback();
    startTimer (500);
}

CaptureEditor::~CaptureEditor()
{
    stopTimer();
}

void CaptureEditor::timerCallback()
{
    const int count = captureProcessor.getRecorder().finishedCount();

    if (count == shownCount)
        return;

    shownCount = count;
    const auto last = captureProcessor.getRecorder().lastFinished();

    if (! last.any)
        lastLine = "No capture yet";
    else if (! last.complete)
        lastLine = "The last Render could not be recorded";
    else
        lastLine = "Last capture " + lengthText (last.info.lengthSamples, last.info.sampleRate)
                   + " at " + last.info.captured.formatted ("%H:%M:%S");

    repaint();
}

void CaptureEditor::paint (juce::Graphics& g)
{
    using ui::Theme;

    g.fillAll (Theme::bg());

    auto area = getLocalBounds().reduced (20, 16);

    // The name, with the amber accent beside it.
    auto title = area.removeFromTop (20);
    g.setColour (Theme::pin());
    g.fillRoundedRectangle (title.removeFromLeft (4).toFloat().reduced (0.0f, 2.0f), 2.0f);
    title.removeFromLeft (10);
    g.setColour (Theme::text());
    g.setFont (Theme::monoBold (13.0f));
    g.drawText ("TEMPO TANTRUM CAPTURE", title, juce::Justification::centredLeft);

    area.removeFromTop (12);
    g.setColour (Theme::text());
    g.setFont (Theme::ui (13.0f));
    g.drawFittedText ("Press Render to send the selected audio to Tempo Tantrum. The audio is not changed.",
                      area.removeFromTop (36), juce::Justification::topLeft, 2);

    area.removeFromTop (8);
    auto status = area.removeFromTop (28);
    g.setColour (Theme::panel());
    g.fillRoundedRectangle (status.toFloat(), Theme::cornerRadius);
    g.setColour (Theme::line());
    g.drawRoundedRectangle (status.toFloat().reduced (0.5f), Theme::cornerRadius, 1.0f);
    g.setColour (Theme::dim());
    g.setFont (Theme::mono (12.0f));
    g.drawText (lastLine, status.reduced (10, 0), juce::Justification::centredLeft);
}

} // namespace tt::audiosuite

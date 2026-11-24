#pragma once

#include "CaptureProcessor.h"

// The plug-in's window: what it does, in one line, and the last capture's length and time.
namespace tt::audiosuite
{

class CaptureEditor : public juce::AudioProcessorEditor,
                      private juce::Timer
{
public:
    explicit CaptureEditor (CaptureProcessor&);
    ~CaptureEditor() override;

    void paint (juce::Graphics&) override;

private:
    CaptureProcessor& captureProcessor;
    int shownCount = -1;
    juce::String lastLine;

    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureEditor)
};

} // namespace tt::audiosuite

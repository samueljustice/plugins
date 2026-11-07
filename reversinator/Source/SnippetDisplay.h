#pragma once

#include <JuceHeader.h>
#include "Theme.h"

class ReversinatorAudioProcessor;

/**
 * Scrolling view of the recent input with the reverser's read heads drawn on
 * top: each voice is a glowing line at the point in the past it is currently
 * playing, faded by its window envelope. "Now" is the right-hand edge, so the
 * heads visibly sweep backwards through the audio.
 */
class SnippetDisplay : public juce::Component, private juce::Timer
{
public:
    SnippetDisplay(ReversinatorAudioProcessor&, const Theme::Fonts&);
    ~SnippetDisplay() override;

    void paint(juce::Graphics&) override;

    /** Pulls new data from the processor; called by the timer (and by tests). */
    void update();

private:
    void timerCallback() override { update(); }

    ReversinatorAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;

    std::vector<float> m_history;              // snapshot of recent input peaks, newest last
    int m_historyCount = 0;

    float m_spanSeconds = 5.0f;                // smoothed visible span
};

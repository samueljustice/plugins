#pragma once

#include <JuceHeader.h>
#include "Theme.h"
#include "TimeHighway.h"

class ReversinatorAudioProcessor;

/**
 * Scrolling view of the recent input with the reverser's read heads drawn on
 * top: each voice is a glowing line at the point in the past it is currently
 * playing, faded by its window envelope. "Now" is the right-hand edge, so the
 * heads visibly sweep backwards through the audio.
 *
 * That flat view is the fallback. When the editor's OpenGL context is running, the editor draws the
 * TimeHighway scene behind this component from getFrame(), and this component paints only the
 * labels over it (setSceneLive()).
 */
class SnippetDisplay : public juce::Component, private juce::Timer
{
public:
    SnippetDisplay(ReversinatorAudioProcessor&, const Theme::Fonts&);
    ~SnippetDisplay() override;

    void paint(juce::Graphics&) override;

    /** Pulls new data from the processor; called by the timer (and by tests). */
    void update();

    /** The latest data for the OpenGL scene. Safe to call from the GL thread. */
    TimeHighway::Frame getFrame() const;

    /** True while the OpenGL scene is being drawn behind this component. Message thread. */
    void setSceneLive(bool live);
    bool isSceneLive() const { return m_sceneLive; }

private:
    void paintFlat(juce::Graphics&);
    void paintOverScene(juce::Graphics&);
    void paintLabels(juce::Graphics&, juce::Rectangle<float> bounds);

    void timerCallback() override { update(); }

    ReversinatorAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;

    std::vector<float> m_history;              // snapshot of recent input peaks, newest last
    int m_historyCount = 0;

    float m_spanSeconds = 5.0f;                // smoothed visible span

    bool m_sceneLive = false;
    juce::SpinLock m_frameLock;
    TimeHighway::Frame m_frame;                // guarded by m_frameLock
};

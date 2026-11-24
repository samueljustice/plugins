#pragma once

#include <JuceHeader.h>

#include "../core/Engine.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace tt::ui
{

// The waveform with the grid, the pins and the playhead on it.
//
// From top to bottom: a ruler with bar numbers and pin heads, the music lane, the drum lane
// (when a drum buffer is set) and a thin strip showing which part of the file is in view.
// The view never changes the map itself: every edit is reported through a callback and the
// owner answers with setMap().
class WaveformView : public juce::Component
{
public:
    WaveformView();
    ~WaveformView() override;

    // The buffers stay owned by the caller and must outlive the view's use of them; nullptr clears.
    // Only the peaks are read, once, during this call. Call on the message thread.
    void setAudio (const juce::AudioBuffer<float>* mix, const juce::AudioBuffer<float>* drums, double sampleRate);
    void setMap (const tt::MapResult& map, int beatsPerBar);     // copies what it needs
    void setPlayhead (double seconds);                           // repaints two thin strips only
    void setBusy (bool analysing);                               // dims the lanes and shows "Listening…"
    void showRange (double startSeconds, double endSeconds);     // zoom to this range
    void showAll();
    double getViewStart() const                     { return viewStart; }   // the range in view, in seconds
    double getViewEnd() const                       { return viewEnd; }
    int getSelectedPin() const;                                  // index into map.pins or -1
    void setSelectedPin (int pinIndex);                           // -1 selects none; calls nothing back

    std::function<void (double seconds)> onSeek;                          // click on empty space
    std::function<void (int pinIndex, double newTime)> onPinMoved;        // drag finished; time snapped to a hit unless Alt was held
    std::function<void (double time)> onPinAdded;                         // double-click near a hit (time snapped to the hit)
    std::function<void (int pinIndex)> onPinDeleted;                      // Backspace/Delete on the selected pin, or the menu
    std::function<void (double time)> onRestartHere;                      // menu "The beat restarts here"
    std::function<void (double time)> onFirstHitHere;                     // menu "This is the first hit"
    std::function<void (int pinIndex, double newBeat)> onPinBeatChanged;  // menu "This hit is bar…"

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    // Minimum and maximum of the audio in buckets of several sizes, so that drawing reads a
    // handful of values for each pixel column whatever the zoom.
    struct PeakCache
    {
        struct Level
        {
            int samplesPerBucket = 0;
            std::vector<std::int16_t> lo, hi;
        };

        void build (const juce::AudioBuffer<float>&);
        void clear()                                { levels.clear(); numSamples = 0; }
        bool empty() const                          { return levels.empty(); }
        void read (double firstSample, double lastSample, double samplesPerPixel, float& lo, float& hi) const;

        std::vector<Level> levels;
        juce::int64 numSamples = 0;
    };

    struct Layout
    {
        juce::Rectangle<int> ruler, music, drums, overview, lanes;
    };

    struct GridLine
    {
        float x = 0.0f;
        bool bar = false;
        long long barIndex = 0;     // zero-based
    };

    Layout getLayout() const;
    bool hasAudio() const                           { return duration > 0.0 && ! mixPeaks.empty(); }
    double span() const                             { return viewEnd - viewStart; }
    double pixelsPerSecond() const;
    float timeToX (double seconds) const;
    double xToTime (float x) const;
    void setView (double start, double end);
    void zoomAbout (float x, double factor);
    void scrollBy (double seconds);

    int pinAt (juce::Point<float>, const Layout&) const;
    double pinTime (int index) const;               // the dragged position while a drag is under way
    bool snapToOnset (double& time, float withinPixels, double lowest, double highest) const;
    juce::String pinLabel (double beat) const;

    void collectGrid (std::vector<GridLine>&, int& labelStep, double& pixelsPerBar) const;
    void drawWave (juce::Graphics&, const PeakCache&, juce::Rectangle<int> lane, juce::Colour, juce::Rectangle<int> clip) const;
    void drawCentredMessage (juce::Graphics&, const juce::String&, juce::Rectangle<int>) const;
    void repaintPlayheadAt (double seconds);

    void showMenu (double time, int pinIndex);
    void showBarEditor (int pinIndex);
    void hideBarEditor (bool commit);

    PeakCache mixPeaks, drumPeaks;
    double sampleRate = 0.0;
    double duration = 0.0;

    tt::MapResult map;
    tt::BeatGrid grid;
    int beatsPerBar = 4;

    double viewStart = 0.0, viewEnd = 0.0;
    double playhead = 0.0;
    bool busy = false;

    int selectedPin = -1;
    int hoverPin = -1;

    // A press on a pin becomes a drag once the pointer has moved a few pixels.
    int pressedPin = -1;
    bool dragging = false;
    double dragTime = 0.0;
    double keepSelectionNear = -1.0;   // after an edit, select the pin the next map has at this time

    juce::TextEditor barEditor;
    int barEditorPin = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};

} // namespace tt::ui
